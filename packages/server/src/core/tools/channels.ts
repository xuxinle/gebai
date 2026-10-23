/**
 * 对外动作工具（`act` / `observe`）与渠道清单工具（`channels_list`）。
 *
 * ## 为什么是三个工具，而不是每个渠道一个
 *
 * 工具数量本身就是上下文税。按渠道分化（`send_message` / `create_issue` / `write_memory`…）
 * 会让每个新渠道都往**全部会话**的系统提示里加一段；而它们的调用形状几乎一样，
 * 只是「目标 + 动词」不同。
 *
 * 所以收敛成两个动作面 + 一个发现面：
 *
 * | 工具 | 阻塞 | 用途 |
 * |---|---|---|
 * | `act` | 写面需审批 | 做一件事（发消息 / 提交工单 / 写记忆…），0~N 次 |
 * | `observe` | 否（只读） | 查一件事（读消息 / 看成员 / 读记忆…） |
 * | `channels_list` | 否 | 先看见有哪些渠道与动词，再决定怎么调 |
 *
 * **`submit_*` 不需要单独框架**：它就是 `act(verb:"submit")`。任何渠道声明了新动词，
 * 模型都能立刻用——这就是「抽象是性质而不是名单」的直接结果。
 *
 * ## 治理边界（这条不能破）
 *
 * `act` 的审批与白名单**全部在工具层与服务端注册时**裁决：
 * - 白名单越界 → **直接拒绝**，不弹审批（发错群是泄露，不是「让用户决定」的事）
 * - `writes: true` 的动词 → 走审批
 * - 只读观测 → 免审（复用引擎既有审批语义）
 *
 * 渠道插件只能回答「怎么发」，拿不到任何绕过治理的能力。
 */

import { ActionExecutor, ChannelRegistry } from "@gebai/channels"
import type { ActOutcome, ObserveOutcome } from "@gebai/channels"
import type { Tool } from "../base/types"
import { schema, type GlobalToolEntry } from "./shared"

/** 渠道注册表在 ToolContext 上的注入键（服务端启动装配时写入）。 */
export const CHANNEL_REGISTRY_KEY = "channels" as const

/**
 * 解析本会话可用的执行器。
 *
 * 注册表是**进程级服务**（渠道连接不该每会话一份），经 ToolContext 注入——
 * 与 `sessions` / `subSessions` 等服务同款。未装配时返回 null，调用方给明确说明
 * （不假装成功，也不抛栈）。
 */
function executorOf(ctx: { channels?: ChannelRegistry }): ActionExecutor | null {
  const reg = ctx.channels
  if (!reg) return null
  return new ActionExecutor(reg, {
    log: {
      info: () => {}, // 动作结果已进工具 output，避免重复刷日志
      warn: () => {},
    },
  })
}

/** 未装配时的统一说明（与「渠道不可用」区分开，便于定位是装配问题还是配置问题）。 */
const NO_REGISTRY =
  "渠道注册表未装配（服务端启动时未注入 channels 服务）。" +
  "这是装配层问题而不是调用参数问题——请确认服务端已初始化 ChannelRegistry。"

export const actTool: Tool = {
  name: "act",
  description:
    "做一件事（对外动作，0~N 次）。统一入口：发消息、引用回复、提交工单、改状态、写记忆、落盘产物……都走这里。\n" +
    "**目标格式 `channel:address`**（如 `feishu:oc_xxx`、`local:memory`、`jira:PROJ`）；\n" +
    "**动词由渠道声明**——先用 `channels_list` 查看有哪些渠道、每个渠道支持哪些动词与参数。\n" +
    "写操作需审批；目标越界会被**直接拒绝**（不进审批）。返回 ref（外部系统的稳定引用）与结果摘要。\n" +
    "一次处理里可以不做、做一次、做多次；**不出站是合法的**（判断无需处理就直接结束）。",
  card: { titleParams: ["to", "verb"], args: "kv" },
  // 审批按动词的 writes 声明动态判定：读类免审，写类需审。
  // 未知渠道/动词返回 false（让执行期直接拒绝——不问「是否允许发给无关群」）。
  requiresApproval: async (args, ctx) => {
    const ex = executorOf(ctx as { channels?: ChannelRegistry })
    if (!ex) return false
    return ex.needsApproval({
      target: String(args.to ?? ""),
      verb: String(args.verb ?? ""),
      args: (args.args && typeof args.args === "object" ? args.args : {}) as Record<string, unknown>,
    })
  },
  parameters: schema(
    {
      to: {
        type: "string",
        description: "目标 `channel:address`（如 feishu:oc_xxx / local:memory / jira:PROJ）。用 channels_list 查可用渠道。",
      },
      verb: { type: "string", description: "要做的动作（由渠道声明，如 send / reply / submit / update / append / put）" },
      args: {
        type: "object",
        description: "动作参数（键由渠道的动词声明决定，见 channels_list）",
      },
      why: {
        type: "string",
        description: "为什么做这个动作（审计用；对人有解释价值，如「回复张三关于 pending 的问题」）",
      },
    },
    ["to", "verb"],
  ),
  outputSchema: schema({
    ref: { type: "string", description: "外部系统的稳定引用（消息 id / 工单号 / 文件路径）" },
    summary: { type: "string", description: "人类可读的结果摘要" },
    data: { type: "object", description: "渠道自定义结构化数据" },
  }),
  async execute(args, ctx) {
    const ex = executorOf(ctx as { channels?: ChannelRegistry })
    if (!ex) return { output: NO_REGISTRY }

    const to = String(args.to ?? "").trim()
    const verb = String(args.verb ?? "").trim()
    if (!to) return { output: "act 缺少 to（目标格式 channel:address）" }
    if (!verb) return { output: "act 缺少 verb（动作名，见 channels_list）" }
    const payload = args.args && typeof args.args === "object" ? (args.args as Record<string, unknown>) : {}

    const r = await ex.act({ target: to, verb, args: payload })
    if (!r.ok) {
      const head = r.kind === "rejected" ? "动作被拒绝" : "动作失败"
      return { output: `${head}：${r.reason}`, data: { ok: false, kind: r.kind, reason: r.reason } }
    }
    const o: ActOutcome = r.outcome
    const why = String(args.why ?? "").trim()
    const lines = [o.summary]
    if (o.ref) lines.push(`ref: ${o.ref}`)
    if (why) lines.push(`（缘由：${why}）`)
    return { output: lines.join("\n"), data: { ok: true, ref: o.ref ?? null, summary: o.summary, ...(o.data ? { channelData: o.data } : {}) } }
  },
}

export const observeTool: Tool = {
  name: "observe",
  description:
    "查一件事（只读，无副作用）。统一入口：读群消息、看群成员、读长期记忆、列产物……都走这里。\n" +
    "**目标格式 `channel:address`**；**观测项由渠道声明**——先用 `channels_list` 查看有哪些可观测项与参数。\n" +
    "只读观测免审批；目标越界会被直接拒绝。",
  card: { titleParams: ["to", "what"], args: "kv" },
  parameters: schema(
    {
      to: { type: "string", description: "目标 `channel:address`（如 feishu:oc_xxx / local:memory）" },
      what: { type: "string", description: "要观测什么（由渠道声明，如 messages / members / all / list）" },
      args: { type: "object", description: "观测参数（键由渠道声明决定）" },
    },
    ["to", "what"],
  ),
  outputSchema: schema({
    summary: { type: "string", description: "人类可读的结果" },
    data: { type: "object", description: "渠道自定义结构化数据" },
  }),
  async execute(args, ctx) {
    const ex = executorOf(ctx as { channels?: ChannelRegistry })
    if (!ex) return { output: NO_REGISTRY }

    const to = String(args.to ?? "").trim()
    const what = String(args.what ?? "").trim()
    if (!to) return { output: "observe 缺少 to（目标格式 channel:address）" }
    if (!what) return { output: "observe 缺少 what（观测项，见 channels_list）" }
    const payload = args.args && typeof args.args === "object" ? (args.args as Record<string, unknown>) : {}

    const r = await ex.observe({ target: to, what, args: payload })
    if (!r.ok) {
      const head = r.kind === "rejected" ? "观测被拒绝" : "观测失败"
      return { output: `${head}：${r.reason}`, data: { ok: false, kind: r.kind, reason: r.reason } }
    }
    const o: ObserveOutcome = r.outcome
    return { output: o.summary, data: { ok: true, ...(o.data ? o.data : {}) } }
  },
}

/**
 * 渠道清单（先看见再决定）。
 *
 * 没有它，模型只能猜渠道名与动词——猜错的表现是「工具调用失败」，而错误信息
 * 会把它引向错误的排查方向（以为是自己参数写错）。
 */
export const channelsListTool: Tool = {
  name: "channels_list",
  description:
    "查看可用渠道、每个渠道支持的动词（写）与观测项（读）、以及参数形状。**调用 act/observe 前先看这里**，避免猜错渠道名或能力名。",
  parameters: schema({
    channel: { type: "string", description: "只看某个渠道（省略 = 全部）" },
    verbose: { type: "boolean", description: "是否展开参数细节（默认 true）" },
  }),
  async execute(args, ctx) {
    const reg = (ctx as { channels?: ChannelRegistry }).channels
    if (!reg) return { output: NO_REGISTRY }
    // 热加载：清单查询前顺带比对目录签名（成本约一次目录遍历），新放的渠道立即可见
    await reg.refreshIfChanged().catch(() => false)

    const only = String(args.channel ?? "").trim()
    const verbose = args.verbose !== false
    const states = reg.state().filter((s) => !only || s.name === only)

    if (states.length === 0) {
      return {
        output: only
          ? `没有名为「${only}」的渠道（已注册：${reg.list().map((c) => c.name).join("、") || "无"}）`
          : "当前没有可用渠道。在内置 packages/channels/src/channels/ 或 custom/channels/ 放一个渠道定义文件即可注册。",
      }
    }

    const lines: string[] = []
    const available = states.filter((s) => s.enabled && s.started)
    const unusable = states.filter((s) => !s.enabled || !s.started)
    lines.push(`可用渠道 ${available.length} 个${unusable.length ? `（另有 ${unusable.length} 个不可用）` : ""}：`)

    for (const s of available) {
      const def = reg.get(s.name)!
      lines.push("")
      lines.push(`■ ${s.name}｜${def.description}`)
      const verbs = Object.entries(def.verbs ?? {})
      const obs = Object.entries(def.observes ?? {})
      if (verbs.length) {
        lines.push("  写（act，需审批）：")
        for (const [name, spec] of verbs) {
          lines.push(`    - ${name}：${spec.summary}${verbose ? `\n      参数：${argList(spec)}` : ""}`)
        }
      } else {
        lines.push("  写（act）：无")
      }
      if (obs.length) {
        lines.push("  读（observe，免审批）：")
        for (const [name, spec] of obs) {
          lines.push(`    - ${name}：${spec.summary}${verbose ? `\n      参数：${argList(spec)}` : ""}`)
        }
      } else {
        lines.push("  读（observe）：无")
      }
      lines.push(`  目标：${s.restricted ? "有地址白名单（越界直接拒绝）" : "无地址级限制"}`)
    }

    if (unusable.length) {
      lines.push("")
      lines.push("不可用渠道：")
      for (const s of unusable) {
        lines.push(`  - ${s.name}${s.error ? `：${s.error}` : "（未启用）"}`)
      }
    }

    return {
      output: lines.join("\n"),
      data: {
        channels: states.map((s) => ({
          name: s.name,
          enabled: s.enabled,
          started: s.started,
          verbs: s.verbs,
          observes: s.observes,
          restricted: s.restricted,
          ...(s.error ? { error: s.error } : {}),
        })),
      },
    }
  },
}

/** 参数清单（供 channels_list 的 verbose 输出）。 */
function argList(spec: { args?: Record<string, { type: string; description: string; required?: boolean; enum?: string[] }> }): string {
  const decl = spec.args
  if (!decl || !Object.keys(decl).length) return "（无）"
  return Object.entries(decl)
    .map(([k, a]) => `${k}:${a.type}${a.required ? "*" : ""}${a.enum ? `[${a.enum.join("|")}]` : ""}`)
    .join("、")
}

export const globalTools: GlobalToolEntry[] = [
  { name: "act", tool: actTool },
  { name: "observe", tool: observeTool },
  { name: "channels_list", tool: channelsListTool },
]
