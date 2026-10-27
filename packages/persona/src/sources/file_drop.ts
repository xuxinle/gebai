/**
 * 示例事件源：**文件投递**（最小可用参考实现，也是联调与测试的通用入口）。
 *
 * 用途：
 * 1. 写新事件源的模板——全链路几十行跑通；
 * 2. 把 JSON 丢进目录即产生事件，不必连真实渠道。
 *
 * 约定（新写源照此）：
 * - 只做「订阅 → 转 `IngressEvent` → `ctx.emit`」；不解释人格、不推送回复；
 * - 幂等键尽量给显式 `id`（这里用文件名 + mtime，重写即新事件）；
 * - `start` **必须尽快返回**（订阅后即 resolve），长驻循环放后台；
 * - `stop` 负责清理定时器/连接。
 *
 * **目录归属**：本源独占 `{GEBAI_HOME}/persona-drop/`。内置 `local` 渠道的 drop 监听
 * 默认用 `{GEBAI_HOME}/channel-drop/`（见 `packages/channels/src/channels/local.ts`），
 * 两路刻意分开——共用同一目录会发生双消费竞态（谁抢到算谁的，channel/kind 路由失效，
 * 且计数全绿不报错）。需要自定目录用 `GEBAI_PERSONA_FILEDROP_DIR`。
 *
 * **状态隔离的写法（重要，踩过两次）**：`start` 可能被同一模块函数多次调用（多 manager
 * 实例、热加载、测试并行），因此运行时状态**绝不能放模块级变量**，也不能挂在共享的
 * `def` 对象字段上——两者都会被不同实例互相覆盖，表现为「事件偶发丢失」。
 * 这里的做法：每次 `start` 生成一个**专属的 `stop` 函数**放进 `stopQueue`，
 * `def.stop` 弹出最近一个执行。stopQueue 只存函数（无共享数据），多实例天然隔离。
 *
 * 投递格式（`{dir}/*.json`）：
 * ```json
 * { "kind": "message", "source": "文件投递", "summary": "测试事件",
 *   "body": "帮我看看这个", "refs": { "chatId": "oc_demo" } }
 * ```
 */

import { readdir, readFile, rm, stat } from "node:fs/promises"
import { join } from "node:path"
import type { EventSourceContext, EventSourceDef, IngressEvent } from "../source"

/** 轮询间隔（ms）：文件投递是低频通道，1.5s 足够且几乎无开销。 */
const POLL_MS = 1500

/**
 * 待执行的清理器栈（每次 `start` 压入专属闭包，`stop` 弹出最近一个并调用）。
 *
 * 只存**函数、不存共享数据**：清理动作所需的状态（timer、handled、stopped）全在
 * 闭包里，因此不会跨实例串扰。这解决了「`stop()` 无参数、但状态必须按实例隔离」的矛盾。
 */
const stopQueue: Array<() => void> = []

export const source: EventSourceDef = {
  name: "file_drop",
  description: "文件投递：把 JSON 放进 {GEBAI_HOME}/persona-drop/ 即产生一条事件（联调与测试用）",
  channel: "filedrop",
  envVars: [{ name: "GEBAI_PERSONA_FILEDROP_DIR", description: "投递目录（缺省 {GEBAI_HOME}/persona-drop/）" }],

  async start(ctx: EventSourceContext): Promise<void> {
    const dir = ctx.env("GEBAI_PERSONA_FILEDROP_DIR") || join(ctx.home, "persona-drop")
    // 本实例私有状态（闭包内，不与任何其它实例共享）
    let stopped = false
    const handled = new Set<string>()
    let timer: ReturnType<typeof setInterval> | null = null

    // 注册清理器：stop() 时调用（弹出最近一个）
    stopQueue.push(() => {
      stopped = true
      if (timer !== null) clearInterval(timer)
      timer = null
      handled.clear()
    })

    ctx.log("info", `文件投递源监听中：${dir}`)

    const poll = async (): Promise<void> => {
      if (stopped) return
      let names: string[]
      try {
        names = (await readdir(dir)).filter((n) => n.endsWith(".json"))
      } catch {
        return // 目录不存在：静默（首次投递时自然可见）
      }
      for (const name of names) {
        const full = join(dir, name)
        if (handled.has(full)) continue
        handled.add(full) // 先登记：并发 poll 不重复处理同一文件
        try {
          const info = await stat(full)
          const parsed = JSON.parse(await readFile(full, "utf8")) as Partial<IngressEvent>
          if (!parsed.body || !parsed.summary) {
            ctx.log("warn", `${name} 缺少 body/summary，已跳过`)
          } else {
            const result = await ctx.emit({
              kind: parsed.kind ?? "message",
              channel: parsed.channel ?? "filedrop",
              source: parsed.source ?? "文件投递",
              summary: parsed.summary,
              body: parsed.body,
              ...(parsed.refs ? { refs: parsed.refs } : {}),
              // 幂等键：文件名 + mtime（同一文件被重写视为新事件）
              id: parsed.id ?? `filedrop:${name}@${info.mtimeMs}`,
              ...(parsed.attachments ? { attachments: parsed.attachments } : {}),
              ...(parsed.at !== undefined ? { at: parsed.at } : {}),
            })
            if (result.status !== "accepted") {
              ctx.log("warn", `${name} 未被接管（${result.status}${result.reason ? `：${result.reason}` : ""}）`)
            }
          }
        } catch (err) {
          ctx.log("warn", `处理 ${name} 失败: ${String((err as Error)?.message || err)}`)
        }
        // 处理完即删除（保留会无限重扫；原因已记日志）；失败也删，坏文件不无限重试
        await rm(full, { force: true }).catch(() => {})
      }
    }

    // 首次立即扫一遍（目录里可能已有存量），之后周期轮询
    void poll()
    timer = setInterval(() => void poll(), POLL_MS)
    timer.unref?.()
  },

  async stop(): Promise<void> {
    // 弹出最近一次 start 的清理器（LIFO：后启动的先停）
    const cleanup = stopQueue.pop()
    cleanup?.()
  },
}
