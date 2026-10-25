/**
 * 派活工具：`worker_dispatch` / `worker_list` / `worker_cancel`。
 *
 * ## 为什么 worker 要工具化
 *
 * `WorkerSpawner` 只是宿主侧的实现；模型要能用它，得有工具。而没有工具时的后果是：
 * driver 遇到「要查一下」的活只能自己查——查的过程中群里的新消息**全被堵在队列里**
 * （driver 是串行会话，一轮没结束下一轮进不来）。这正是「一个脑子 + 多只手」要解决的。
 *
 * ## 三个刻意的设计
 *
 * ### 1. 派活默认**异步**（fire-and-forget）
 *
 * 工具立刻返回 runId，结论稍后作为**事件**回到 driver（`workerReportEvent`）。
 * 若同步等，等于「派活 = 自己干」——driver 照样被堵住，只是多花了一个会话的钱。
 *
 * ### 2. 结论必带 `verify`（验收标准）
 *
 * 不给验收标准的活回来的是「我看了一下，大概是……」。Driver 写清楚「必须给出可复现的
 * 命令 / 具体文件:行号」，worker 才有交付形态可言。这一条也顺带压住了「派出去查但没说查
 * 到什么算完」这个最常见的失败形态。
 *
 * ### 3. 工具名用 `worker_*` 而不是复用 `subsession_run`
 *
 * 两者语义不同（见 `worker-spawner.ts` 文件头的对照表）。共用名字会让模型在人格会话里
 * 用错形态：subsession 依附于当前 run，run 一结束活就被取消——而派出去的活本该继续。
 */

import type { Tool, ToolContext } from "../base/types"
import type { WorkerOutcome, WorkerSpawner } from "@gebai/persona"
import { personaOwnerOf, type PersonaOwner } from "./persona-driver-tools"
import type { SessionStore } from "./store"

/** 在跑的活（派活记录；结论回来后由宿主投事件）。 */
export interface WorkerTaskRecord {
  runId: string
  personaId: string
  driverId: string
  /** 派出它的 driver 会话（回报事件定向回这里）。 */
  driverSessionId: string
  user: string
  task: string
  label?: string
  startedAt: number
  status: "running" | "done" | "failed" | "cancelled"
  outcome?: WorkerOutcome
}

/** 派活账本：宿主持有，工具与回报投递共用。 */
export interface WorkerTaskBook {
  add(record: WorkerTaskRecord): void
  update(runId: string, patch: Partial<WorkerTaskRecord>): void
  get(runId: string): WorkerTaskRecord | undefined
  /** 某 driver 会话派出的全部活（最近的在前）。 */
  listByDriver(driverSessionId: string): WorkerTaskRecord[]
  /** 全量（部署面板用；账本有界，直接给全量不会涨）。 */
  all(): WorkerTaskRecord[]
  remove(runId: string): void
}

/** 简单的内存账本（进程级；结论落地后条目保留一段时间供 driver 查询，见 `keep`）。 */
export function createWorkerTaskBook(opts: { keep?: number } = {}): WorkerTaskBook {
  const keep = Math.max(1, opts.keep ?? 50)
  const map = new Map<string, WorkerTaskRecord>()
  const order: string[] = []
  const trim = () => {
    while (order.length > keep) {
      const old = order.shift()
      if (old) map.delete(old)
    }
  }
  return {
    add(r) {
      map.set(r.runId, r)
      order.push(r.runId)
      trim()
    },
    update(runId, patch) {
      const cur = map.get(runId)
      if (!cur) return
      map.set(runId, { ...cur, ...patch })
    },
    get: (runId) => map.get(runId),
    listByDriver(driverSessionId) {
      return [...map.values()].filter((r) => r.driverSessionId === driverSessionId).sort((a, b) => b.startedAt - a.startedAt)
    },
    all: () => [...map.values()].sort((a, b) => b.startedAt - a.startedAt),
    remove(runId) {
      map.delete(runId)
      const i = order.indexOf(runId)
      if (i >= 0) order.splice(i, 1)
    },
  }
}

export interface WorkerToolsOptions {
  spawner: WorkerSpawner
  book: WorkerTaskBook
  store: Pick<SessionStore, "load">
  /**
   * 人格身份查询（`PersonaCatalog.get`）。
   *
   * 为何需要：worker 是**独立上下文**，除了提示词里那几句它一无所知——
   * 「谁派的、他是干什么的」直接决定它怎么理解任务（同一句「看看写入延迟」在一个
   * 值班工程师与一个数据分析师眼里不是同一件事）。拿不到就退回 id，不阻断派活。
   */
  identity: (personaId: string) => { id: string; name: string; identity: string; role: string } | null
  /** 结论如何回到 driver（宿主注入：`runtime.reportWorkerDone(workerReportEvent(...))`）。 */
  report: (record: WorkerTaskRecord, outcome: WorkerOutcome) => Promise<void>
  /**
   * 是否同步等待结论（默认 false）。
   * 打开后工具会阻塞到 worker 结束——只在「driver 明确要「等这个结果再说话」」的
   * 场景下有意义，会让 driver 又被堵住，所以不默认开。
   */
  sync?: boolean
  log?: { info(msg: string): void; warn(msg: string): void }
}

const DISPATCH = "worker_dispatch"
const LIST = "worker_list"
const CANCEL = "worker_cancel"

/**
 * 派活的三个工具（全局注册 + 执行时自检人格身份，与 `driver_*` 同一取向）。
 */
export function createWorkerTools(opts: WorkerToolsOptions): Tool[] {
  const { spawner, book } = opts

  const dispatch: Tool = {
    name: DISPATCH,
    description:
      "【仅人格会话可用】把一件**耗时的活**派给一个独立工作会话去做（它有自己的上下文，跑完把结论交回给你）。\n" +
      "为什么用它：你自己的会话是串行的——你在查东西的时候，群里新来的消息全被堵在队列里。派出去之后你立刻能继续响应用户。\n" +
      "**结论稍后作为一条新事件回到你**（不是本工具的返回值），届时你会有机会组织答复。\n" +
      "什么时候不要用：一句话能答的、需要跟用户来回确认的、以及「三步之内就能查完」的——派活要为结果多花一个会话的成本。",
    parameters: {
      type: "object",
      properties: {
        task: { type: "string", description: "要干的活（自然语言指令，写清「查什么、在哪查、要什么形态的结论」）" },
        context: { type: "string", description: "已知背景（你已经查到的关键事实，免得它重复查）" },
        verify: {
          type: "string",
          description:
            "验收标准：什么算查清了（如「必须给出具体文件:行号」「必须给出可复现的命令」）。不写的话回来的多半是模糊结论。",
        },
        label: { type: "string", description: "简称（展示与回报里用，如「查tsdb写入延迟」）" },
        wait: {
          type: "boolean",
          description: "同步等结论（默认 false=立刻返回 runId）。开了会把你堵住，只在你必须拿到结论才能说话时用",
        },
      },
      required: ["task"],
    },
    async execute(args, ctx: ToolContext) {
      const owner = await resolveOwner(opts, ctx)
      if ("error" in owner) return { output: owner.error }
      const task = String(args.task ?? "").trim()
      if (!task) return { output: "派活失败：`task` 不能为空（写清楚要查什么）" }
      if (!args.verify || !String(args.verify).trim()) {
        // 不给验收标准的活回来的是「我看了一下，大概是……」——这是最常见的无效派活
        return {
          output:
            "派活失败：缺 `verify`（什么算查清了）。\n" +
            "这不是形式要求：不写验收标准时，worker 只能自己猜「查到哪里算完」，回来的结论通常无法直接引用。\n" +
            "例：`verify: \"必须给出具体的文件:行号与可复现的命令\"`。",
        }
      }

      const wait = args.wait === true || opts.sync === true
      const label = args.label ? String(args.label).trim().slice(0, 40) : undefined
      const controller = new AbortController()
      // runId 由工具**预先分配**：异步形态下必须在 spawn 返回 promise 之前就能告诉模型
      // （否则它拿到的是“已派出”而 worker_list 里找不到该 runId，也无法叫停）
      const runId = newWorkerRunId()
      const input = {
        personaId: owner.personaId,
        driverId: owner.driverId,
        user: ctx.user,
        identity: owner.identity,
        task,
        ...(args.context ? { context: String(args.context) } : {}),
        verify: String(args.verify),
        driverSessionId: ctx.sessionId,
        runId,
        ...(label ? { label } : {}),
        signal: controller.signal,
      }

      const startedAt = Date.now()
      // 先记账（running）：模型可能立即 worker_list / worker_cancel，账本必须一开始就有
      book.add({
        runId,
        personaId: owner.personaId,
        driverId: owner.driverId,
        driverSessionId: ctx.sessionId,
        user: ctx.user,
        task,
        ...(label ? { label } : {}),
        startedAt,
        status: "running",
      })
      const promise = spawner.spawn(input)
      const onDone = async (outcome: WorkerOutcome) => {
        // 账本里可能已被 worker_cancel 标成 cancelled：保留 canceled 终态（叫停后 worker
        // 可能仍正常跑完，但模型已经决定不要这个结果了，不该反过来把它改成 done）
        const cur = book.get(runId)
        book.update(runId, {
          status: cur?.status === "cancelled" ? "cancelled" : outcome.status === "done" ? "done" : outcome.status,
          outcome,
        })
        // 回报事件带**账本里的当前态**：让 report 也能知道“这条是被叫停的”
        const record = book.get(runId)
        if (record) {
          try {
            await opts.report(record, outcome)
          } catch (err) {
            opts.log?.warn(`[persona:worker] 回报 ${outcome.runId} 投递失败: ${String((err as Error)?.message || err)}`)
          }
        }
      }

      if (wait) {
        const outcome = await promise
        await onDone(outcome)
        return {
          output:
            `已等回结论（${outcome.status}${outcome.elapsedMs !== undefined ? `，${Math.round(outcome.elapsedMs / 1000)}s` : ""}）：\n\n` +
            (outcome.report ?? `（未给出结论：${outcome.error ?? "原因未知"}）`),
          data: { runId: outcome.runId, status: outcome.status },
        }
      }

      // 异步：立刻返回，结论走事件。这里挂一个不 await 的回调（工具已返回，事件稍后到）
      void promise.then(onDone).catch((err) => {
        const message = String((err as Error)?.message || err)
        book.update(runId, { status: "failed", outcome: { runId, status: "failed", error: message } })
        opts.log?.warn(`[persona:worker] 派活失败: ${message}`)
      })
      return {
        output:
          `已派出${label ? `「${label}」` : ""}（runId \`${runId}\`，异步执行中）。\n` +
          "结论到达时会以一条新事件回到你——**届时你再组织答复**。现在可以继续处理别的事。\n" +
          `（若这活不再需要了，用 worker_cancel(run_id=\"${runId}\") 叫停。）`,
        data: { accepted: true, runId, label: label ?? null },
      }
    },
  }

  const list: Tool = {
    name: LIST,
    description: "【仅人格会话可用】查看**本会话派出去的活**：状态、耗时、结论摘要。派活前先看这里，避免同一件事派两遍。",
    parameters: { type: "object", properties: {} },
    async execute(_args, ctx) {
      const owner = await resolveOwner(opts, ctx)
      if ("error" in owner) return { output: owner.error }
      const items = book.listByDriver(ctx.sessionId)
      if (items.length === 0) return { output: "本会话还没有派过活。用 worker_dispatch 派一件。", data: { workers: [] } }
      const lines = items.map((r) => {
        const age = Math.round((Date.now() - r.startedAt) / 1000)
        const head = `- ${r.runId}｜${r.status}｜${age}s｜${r.label ? `${r.label}：` : ""}${r.task.slice(0, 60)}`
        if (r.status === "done" && r.outcome?.report) {
          return `${head}\n    结论：${r.outcome.report.replace(/\s+/g, " ").slice(0, 160)}…`
        }
        if (r.outcome?.error) return `${head}\n    原因：${r.outcome.error.slice(0, 160)}`
        return head
      })
      return {
        output: `本会话派出的活共 ${items.length} 个（最近的在前）：\n${lines.join("\n")}`,
        data: { workers: items.map((r) => ({ runId: r.runId, status: r.status, label: r.label ?? null, task: r.task })) },
      }
    },
  }

  const cancel: Tool = {
    name: CANCEL,
    description: "【仅人格会话可用】叫停一个还在跑的活（runId 从 worker_list 取）。已结束的不用叫停。",
    parameters: {
      type: "object",
      properties: { run_id: { type: "string", description: "worker_list 里的 runId" } },
      required: ["run_id"],
    },
    async execute(args, ctx) {
      const owner = await resolveOwner(opts, ctx)
      if ("error" in owner) return { output: owner.error }
      const runId = String(args.run_id ?? "").trim()
      if (!runId) return { output: "叫停失败：缺 `run_id`" }
      const rec = book.get(runId)
      // 只允许停本会话派出的活：跨会话叫停会让 A 人格能掐断 B 人格的活（人格之间没有隶属关系）
      if (!rec || rec.driverSessionId !== ctx.sessionId) {
        return { output: `叫停失败：本会话没有 ${runId} 这个活（用 worker_list 看当前派出的）` }
      }
      if (rec.status !== "running") return { output: `无需叫停：${runId} 已经是 ${rec.status} 状态。` }
      await spawner.cancel(runId)
      book.update(runId, { status: "cancelled" })
      return { output: `已叫停 ${runId}（它不会再回报结论）。`, data: { runId } }
    },
  }

  return [dispatch, list, cancel]
}

/** worker runId（由工具层预分配，见 `WorkerSpawnInput.runId`）。 */
function newWorkerRunId(): string {
  return `worker_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`
}

/** 身份解析（与 driver 工具同一判定：人格会话才有）。 */
async function resolveOwner(
  opts: WorkerToolsOptions,
  ctx: ToolContext,
): Promise<{ personaId: string; driverId: string; identity: { id: string; name: string; identity: string; role: string } } | { error: string }> {
  const owner: PersonaOwner | null = await personaOwnerOf(opts.store, ctx.sessionId, ctx.user)
  if (!owner) {
    return {
      error:
        "当前会话不属于任何人格，无法派活。\n" +
        "派活是**人格**的能力（一个脑子带多只手）：只有人格的常驻会话才能用。\n" +
        "普通会话里要做长任务，用 subsession_run（它依附于当前任务，任务结束即止）。",
    }
  }
  // 角色身份尽量取全（取不到退回 id）：worker 靠它理解「派活的人是谁」
  const identity = opts.identity(owner.personaId) ?? {
    id: owner.personaId,
    name: owner.personaId,
    identity: "",
    role: "人格",
  }
  return { personaId: owner.personaId, driverId: owner.driverId, identity }
}
