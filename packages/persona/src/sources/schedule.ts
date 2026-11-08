/**
 * 内置事件源：**人格定时任务**（`channel: schedule`）。
 *
 * ## 它做什么
 *
 * 把人格定义里的 `DriverDef.schedules` 声明变成到点到达的事件：
 *
 * ```ts
 * // custom/persona/xiaoshuang.ts
 * drivers: [{
 *   id: "report",
 *   focus: "写日报",
 *   subscriptions: ["schedule", "message"],   // 必须含 schedule（加载期强校验）
 *   schedules: [{ id: "morning", cron: "0 9 * * 1-5", description: "工作日早报", prompt: "写今天的早报" }],
 * }]
 * ```
 *
 * 到点后走的是**和群消息完全相同**的一条路：`ctx.emit` → `PersonaRuntime` → 该分身的
 * 常驻会话。所以人格身份、出站纪律、审批姿态、同会话串行全部自动生效——
 * 这个源自己不认识人格、不发消息、不调度（与 `docs/event-sources.md` 的边界一致）。
 *
 * ## 声明从哪来（为什么走 `ctx.deps`）
 *
 * 人格定义由**宿主**发现并热加载（`PersonaCatalog`），源不该自己扫目录（那是第二份发现逻辑）。
 * 所以宿主经 `deps.personaSchedules` 注入一个读取函数，源每次 tick 现取——人格定义改了
 * （热加载整体替换 catalog）下一个 tick 就是新的，不需要重启或额外的同步调用。
 *
 * 未注入时**不静默空转**：记一条 warn 说明「人格定时未通电」，再退化成无害的长眠。
 *
 * ## 三条硬约束（与其它源一致）
 *
 * - `start` 订阅后立即 resolve，长循环在后台
 * - 状态（游标 / timer / stopped）全在 `start` 闭包里——多 manager 实例、热加载、测试并行都不串扰
 * - 幂等键按「人格/分身/声明/档期时刻」推导（同一档期重投必被丢弃）
 */

import type { EventSourceContext, EventSourceDef, IngressEvent } from "../source"
import {
  collectDue,
  type DueEvent,
  type ScheduleDeclaration,
} from "../schedules"

/** tick 周期：档期精度是分钟级（cron 最小粒度），20s 足够准且几乎无开销。 */
const TICK_MS = 20_000

/**
 * 宿主注入的声明读取器（`ctx.deps.personaSchedules`）。
 *
 * 每次 tick 现取而不是 start 时快照一次：人格定义是**热加载**的（整体替换 catalog），
 * 快照会让「新加的定时任务要重启才生效」。
 */
export type PersonaSchedulesReader = () => ReadonlyArray<ScheduleDeclaration>

/**
 * 清理器栈（`stop()` 无参数，但状态必须按实例隔离——与 `file_drop` 同款做法）。
 *
 * 只存**函数、不存共享数据**：游标与 timer 都在闭包里，因此不会跨实例串扰。
 */
const stopQueue: Array<() => void> = []

/** 把到期档期交给内核（逐条 `emit`，逐条看裁决结果）。 */
async function emitDue(ctx: EventSourceContext, due: DueEvent[]): Promise<void> {
  for (const d of due) {
    const ingress: IngressEvent = {
      kind: "schedule",
      channel: "schedule",
      source: d.source,
      summary: d.summary,
      body: d.body,
      refs: d.refs,
      id: d.id,
      at: d.at,
      personaId: d.personaId,
      driverId: d.driverId,
    }
    try {
      const res = await ctx.emit(ingress)
      if (res.status !== "accepted") {
        // 非 accepted 一律留痕：`unattended` 说明订阅没覆盖到（加载期本该拦住）、
        // `duplicate` 是重启后的同档期重投（正常），`overflow`/`unavailable` 是容量/生命周期问题。
        ctx.log(res.status === "duplicate" ? "info" : "warn", `定时 ${d.id} 未被接管（${res.status}${res.reason ? `：${res.reason}` : ""}）`)
      }
    } catch (err) {
      // 单条投递炸掉不该拖死整个 tick（后面的档期照发）
      ctx.log("warn", `定时 ${d.id} 投递失败: ${String((err as Error)?.message || err)}`)
    }
  }
}

export const source: EventSourceDef = {
  name: "schedule",
  description: "人格定时任务：按人格定义里 drivers[].schedules 的 cron 声明唤醒对应分身",
  channel: "schedule",

  async start(ctx: EventSourceContext): Promise<void> {
    let stopped = false
    let timer: ReturnType<typeof setInterval> | null = null
    /** 档期游标（`personaId/driverId/scheduleId` → 下个待触发时刻）：本实例私有。 */
    const cursors = new Map<string, number>()

    // 多实例自防御：栈里有残留（上一实例未 stop）先执行掉，见 stopQueue 注释。
    while (stopQueue.length > 0) {
      try {
        stopQueue.pop()?.()
        ctx.log("warn", "人格定时源检测到上一实例未 stop，已先清理")
      } catch {
        /* 旧实例清理失败不阻断新实例 */
      }
    }
    stopQueue.push(() => {
      stopped = true
      if (timer !== null) clearInterval(timer)
      timer = null
      cursors.clear()
    })

    const reader = ctx.deps.personaSchedules as PersonaSchedulesReader | undefined
    if (typeof reader !== "function") {
      // 不静默：没有它，`subscriptions: ["schedule"]` 的分身永远不会被唤醒，
      // 而面板上「源已启动」看起来一切正常——那是最难查的一类。
      ctx.log(
        "warn",
        "宿主未注入 personasSchedules（人格定时未通电）：人格定义里的 drivers[].schedules 不会触发。" +
          "检查拟人模式装配（boot/compose.ts 的 deps.personaSchedules）。",
      )
    }

    const tick = async (): Promise<void> => {
      if (stopped || typeof reader !== "function") return
      let declarations: ReadonlyArray<ScheduleDeclaration>
      try {
        declarations = reader()
      } catch (err) {
        ctx.log("warn", `读取人格定时声明失败: ${String((err as Error)?.message || err)}`)
        return
      }
      const now = Date.now()
      const { events, skipped, errors } = collectDue({ declarations, cursors, now })
      for (const s of skipped) {
        // 落后太久（进程阻塞/休眠）：如实记一笔。不记的话「昨天那段没跑」永远查不出来
        ctx.log("info", `${s.key} 落后超过补跑窗口，已跳过至 ${new Date(s.droppedUntil).toISOString()}`)
      }
      for (const e of errors) ctx.log("warn", `${e.key} 档期计算失败: ${e.error}`)
      if (events.length) await emitDue(ctx, events)
    }

    if (typeof reader === "function") {
      const count = (() => {
        try {
          return reader().length
        } catch {
          return 0
        }
      })()
      ctx.log("info", `人格定时源已启动：${count} 条声明（tick ${TICK_MS / 1000}s）`)
    }
    // 首次立即算一次（进程刚起来时就该把「刚刚到点」的档期放出去），之后周期 tick
    void tick()
    timer = setInterval(() => void tick(), TICK_MS)
    timer.unref?.()
  },

  async stop(): Promise<void> {
    const cleanup = stopQueue.pop()
    cleanup?.()
  },
}
