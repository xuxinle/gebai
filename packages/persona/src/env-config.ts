/**
 * 部署配置面：**人格共性写代码、使用者差异写环境变量**。
 *
 * ## 为什么这些解析必须在内核里，而不是装配层
 *
 * 「哪些群归这个人格」「它能发到哪」「这个角色谁来扮演」都是**人格语义**，
 * 不是某一种宿主（server / 桌面 / 测试）的装配细节。写在 compose 里会有三个后果：
 *
 * 1. 无法用纯函数单测（要起服务端才能验一条解析规则）
 * 2. 入站收窄在 compose、出站白名单在 core/tools、身份渲染在第三个地方——三处各写一遍
 *    必然分叉（同一个 `GEBAI_PERSONA_<ID>_CHATS` 被两处按不同口径解释过，是真实踩过的坑）
 * 3. 换宿主（桌面形态、CLI）就得重写一遍
 *
 * 所以这里收敛成**纯函数**：输入 `Map<id, PersonaDef>` + env 视图，输出解析后的定义。
 * 装配层只负责"把 process.env 传进来"。
 *
 * ## 变量表
 *
 * | 变量 | 语义 |
 * |---|---|
 * | `GEBAI_PERSONA_ENABLE` | 人格启用白名单（逗号分隔 id；非空仅启用名单内） |
 * | `GEBAI_PERSONA_<ID>_CHATS` | 该人格只监听这些群（替换其 feishu 消息订阅） |
 * | `GEBAI_PERSONA_<ID>_<DRIVER>_CHATS` | 该**分身**只监听这些群（比人格级更细） |
 * | `GEBAI_PERSONA_<ID>_OUTPUT` | 该人格的输出目标（`channel:address`，可带 `#话题`，逗号分隔） |
 * | `GEBAI_PERSONA_<ID>_<DRIVER>_OUTPUT` | 该分身的输出目标（覆盖人格级） |
 * | `GEBAI_PERSONA_<ID>[_<DRIVER>]_SESSION_MAP` | 会话映射规则（预设名 / `when=key;…` / JSON） |
 * | `GEBAI_PERSONA_SESSION_ROUTING` | 兜底规则：`driver`（缺省，一个上下文）/ `chat`（群×话题） |
 * | `GEBAI_PERSONA_APPROVAL` | 审批姿态：`ask`（缺省，相期弹窗问人）/ `auto` / `deny` |
 *
 * ## 命名歧义的处理（`<ID>_<DRIVER>_CHATS` 与 `<ID>_CHATS`）
 *
 * 人格 id 与 driver id 都可能含下划线，`GEBAI_PERSONA_FIN_OPS_ALERT_CHATS` 到底是
 * 「人格 fin_ops 的分身 alert」还是「人格 fin 的分身 ops_alert」在字符串层面无解。
 * 处理方式：**按已知定义逐条比对**（我们知道有哪些 id、哪些 driver），而不是解析变量名。
 */

import type { PersonaDef } from "./persona"
import type { DriverDef, OutputTarget } from "./ports"
import { parseSessionMap } from "./session-map"
import { matchesSubscription } from "./subscription"

/** env 视图（缺省读 process.env；测试注入）。 */
export type EnvView = Record<string, string | undefined>

const listOf = (raw: string | undefined): string[] =>
  (raw ?? "")
    .split(/[,\s]+/)
    .map((s) => s.trim())
    .filter(Boolean)

/** 变量名片段：id 大写、非字母数字压成下划线（与既有口径一致）。 */
const upper = (s: string): string => s.toUpperCase()

/**
 * 解析输出目标写法。
 *
 * 支持三种形态（一个变量里逗号分隔多条）：
 * - `feishu:oc_xxx` —— 发到群
 * - `feishu:oc_xxx#om_yyy` —— 发到该群里的某个话题（`#` 后是话题根消息 id）
 * - `feishu:oc_xxx=训练平台支持群` —— 带标签（`=` 后是给人看的名字）
 *
 * 首条默认标记为 default（模型没理由时用它）——**除非**某条显式写了 `*` 前缀：
 * `feishu:oc_a,*feishu:oc_b` 表示 oc_b 是默认目标。
 *
 * 解析失败（缺冒号、渠道名为空）**计入 errors 而不静默丢弃**：
 * 一条配错的输出目标等于「人格以为能发但发不出去」，静默丢会让部署方永远查不到原因。
 */
export function parseOutputTargets(raw: string | undefined): { targets: OutputTarget[]; errors: string[] } {
  const targets: OutputTarget[] = []
  const errors: string[] = []
  for (const item of listOf(raw)) {
    let text = item
    let isDefault = false
    if (text.startsWith("*")) {
      isDefault = true
      text = text.slice(1)
    }
    // 标签：末段 `=` 之后（地址里不会出现 `=`；`#` 留给话题）
    let label: string | undefined
    const eq = text.lastIndexOf("=")
    if (eq > 0) {
      label = text.slice(eq + 1).trim() || undefined
      text = text.slice(0, eq)
    }
    let thread: string | undefined
    const hash = text.indexOf("#")
    if (hash > 0) {
      thread = text.slice(hash + 1).trim() || undefined
      text = text.slice(0, hash)
    }
    const idx = text.indexOf(":")
    if (idx <= 0 || idx === text.length - 1) {
      errors.push(`输出目标「${item}」格式非法（应为 channel:address，如 feishu:oc_xxx）`)
      continue
    }
    const channel = text.slice(0, idx).trim().toLowerCase()
    const address = text.slice(idx + 1).trim()
    if (!channel || !address) {
      errors.push(`输出目标「${item}」渠道名或地址为空`)
      continue
    }
    targets.push({
      channel,
      address,
      ...(thread ? { thread } : {}),
      ...(label ? { label } : {}),
      ...(isDefault ? { default: true } : {}),
    })
  }
  // 没有显式 `*` 时把首条当默认（模型不必每次自己挑）
  if (targets.length && !targets.some((t) => t.default)) targets[0] = { ...targets[0]!, default: true }
  return { targets, errors }
}

/** 收窄结果（诊断用：部署方要能看见「哪些群真的生效了」）。 */
export interface PersonaEnvReport {
  /** 被启用白名单排除的人格。 */
  disabled: string[]
  /** 收窄了入站群的人格/分身（`personaId` 或 `personaId/driverId` → 群列表）。 */
  narrowed: Record<string, string[]>
  /** 输出目标（`personaId` 或 `personaId/driverId` → 条数）。 */
  outputs: Record<string, number>
  /** 会话映射已配置的分身（`personaId/driverId` → 规则条数）。 */
  sessionMaps: Record<string, number>
  /** 解析错误（配错了但不该阻断启动——如实上报）。 */
  errors: string[]
}

/**
 * 能匹配「feishu 消息事件」的订阅（收窄时被替换掉的那几类）。
 *
 * 用真实订阅匹配函数判定而不是字符串枚举（`feishu` / `message` / `feishu:message` / `*`）：
 * 枚举法会在订阅语法演进（例如加了三段式、大小写、顺序无关）后**静默漏掉**一类，
 * 表现为「设置了 CHATS 但某些人格还是收到别的群」。这里用一个不可能真实存在的群 id 探测。
 */
function coversFeishuMessages(sub: string): boolean {
  return matchesSubscription(sub, { kind: "message", channel: "feishu", chatId: "__probe__" })
}

/** 收窄某组订阅到指定群（替换覆盖 feishu 消息的那几条，其余原样保留）。 */
export function narrowSubscriptions(subs: readonly string[], chats: readonly string[]): string[] {
  const kept = subs.filter((s) => !coversFeishuMessages(s))
  const chatSubs = chats.map((c) => `feishu:${c}:message`)
  return [...kept, ...chatSubs.filter((s) => !kept.includes(s))]
}

/**
 * 把环境变量应用到人格定义上（纯函数，返回新 Map）。
 *
 * 三件事，各自独立，失败互不影响：
 * 1. 启用白名单过滤
 * 2. 入站收窄（人格级 + 分身级）
 * 3. 输出目标注入（人格级 + 分身级）
 */
export function applyPersonaEnv(
  personas: ReadonlyMap<string, PersonaDef>,
  env: EnvView,
): { personas: Map<string, PersonaDef>; report: PersonaEnvReport } {
  const report: PersonaEnvReport = { disabled: [], narrowed: {}, outputs: {}, sessionMaps: {}, errors: [] }
  const enable = new Set(listOf(env.GEBAI_PERSONA_ENABLE))
  const out = new Map<string, PersonaDef>()

  for (const [id, def] of personas) {
    if (enable.size && !enable.has(id)) {
      report.disabled.push(id)
      continue
    }
    const ID = upper(id)
    // 人格级：入站群 + 输出目标
    const personaChats = listOf(env[`GEBAI_PERSONA_${ID}_CHATS`])
    const personaOutputs = parseOutputTargets(env[`GEBAI_PERSONA_${ID}_OUTPUT`])
    report.errors.push(...personaOutputs.errors.map((e) => `人格 ${id}：${e}`))
    if (personaChats.length) report.narrowed[id] = personaChats
    if (personaOutputs.targets.length) report.outputs[id] = personaOutputs.targets.length

    const drivers = (def.drivers ?? []).map((d) => {
      const DRIVER = `${ID}_${upper(d.id)}`
      const driverChats = listOf(env[`GEBAI_PERSONA_${DRIVER}_CHATS`])
      const driverOutputs = parseOutputTargets(env[`GEBAI_PERSONA_${DRIVER}_OUTPUT`])
      report.errors.push(...driverOutputs.errors.map((e) => `人格 ${id} 分身 ${d.id}：${e}`))

      const chats = driverChats.length ? driverChats : personaChats
      // 输出目标：分身级**覆盖**人格级（分身是更具体的声明）；都空则不注入
      const outputs = driverOutputs.targets.length ? driverOutputs.targets : personaOutputs.targets
      if (driverChats.length) report.narrowed[`${id}/${d.id}`] = driverChats
      if (driverOutputs.targets.length) report.outputs[`${id}/${d.id}`] = driverOutputs.targets.length

      // 会话映射：分身级优先，其次人格级（部署差异，不需改代码）
      const mapRaw = env[`GEBAI_PERSONA_${DRIVER}_SESSION_MAP`] ?? env[`GEBAI_PERSONA_${ID}_SESSION_MAP`]
      const parsedMap = parseSessionMap(mapRaw)
      report.errors.push(...parsedMap.errors.map((e) => `人格 ${id} 分身 ${d.id}：${e}`))
      const sessionMap = parsedMap.rules.length ? parsedMap.rules : d.sessionMap
      if (parsedMap.rules.length) report.sessionMaps[`${id}/${d.id}`] = parsedMap.rules.length

      const next: DriverDef = {
        ...d,
        subscriptions: chats.length ? narrowSubscriptions(d.subscriptions, chats) : d.subscriptions,
        ...(outputs.length ? { outputs } : {}),
        ...(sessionMap?.length ? { sessionMap } : {}),
      }
      return next
    })

    out.set(id, { ...def, drivers })
  }
  return { personas: out, report }
}

/**
 * 会话关联规则（`GEBAI_PERSONA_SESSION_ROUTING`）。
 *
 * ## 会话面向的是**上下文**，不是群
 *
 * 这是本内核最容易理解错的一条，写在这里当作契约：
 *
 * > 会话是**一段连续工作的上下文**，不是“一个群”。渠道（群）只是事件的一个属性，
 * > 不参与会话身份。因此同一个会话里可以同时有来自多个群的消息——它们都归这个
 * > 分身当前的关注面（“我盯告警”就是一个上下文）。
 *
 * 反过来说也对：同一个群里的不同话题/工单可以是不同上下文（那要用 `sessionKeyOf`
 * 声明“按什么分”，例如按工单号、按仓库、按话题根）。
 *
 * ## 两档
 *
 * | 值 | 含义 | 适用 |
 * |---|---|---|
 * | `driver`（缺省） | 一个分身一个常驻会话；**所有群的消息都注入这个上下文** | 绝大多数场景 |
 * | `chat` | 按「群 × 话题」分会话 | 部署方**显式要求**群间上下文隔离时 |
 *
 * 为何缺省是 `driver`：把群当会话身份会把“一个人”拆成几份互不知情的助手——
 * A 群里排查了一小时，B 群里问同一件事时它又从头问起；而人不是这样工作的。
 * 真正的分割应来自**上下文**（不同事务），而不是来自**渠道**（消息从哪里来）。
 */
export function sessionRoutingMode(env: EnvView): "driver" | "chat" {
  const raw = (env.GEBAI_PERSONA_SESSION_ROUTING ?? "").trim().toLowerCase()
  return raw === "chat" ? "chat" : "driver"
}

/**
 * `chat` 模式下的会话键推导（**只在部署显式要求群间隔离时使用**）。
 *
 * 强调：这不是默认行为。会话面向上下文，不面向群——把这个函数当默认会把
 * “一个人”拆成几份互不知情的助手（见 `sessionRoutingMode` 的注释）。
 *
 * 规则：message → `{chatId}` 或 `{chatId}__t_{threadId}`；定时/自省 → `anchor`。
 */
export function chatSessionKeyOf(event: { kind: string; channel: string; context: { refs?: { chatId?: string; threadId?: string } } }): string {
  if (event.kind === "schedule" || event.kind === "idle") return "anchor"
  const chat = event.context.refs?.chatId ?? ""
  if (!chat) return ""
  const thread = event.context.refs?.threadId ?? ""
  return thread ? `${chat}__t_${thread}` : chat
}

/**
 * 审批姿态（`GEBAI_PERSONA_APPROVAL`）。
 *
 * ## 关键区分：**工具执行** vs **对外输出**
 *
 * 这两件事被审的程度不该一样，而早期实现把它们混为一谈（每个工具调用都推卡问人）——
 * 结果是人格在无人值守场景里几乎寸步难行：查一个日志要等人点卡，而人可能在睡觉。
 *
 * | | 工具执行（读文件/跑命令/查接口） | 对外输出（以用户本人名义发消息） |
 * |---|---|---|
 * | 审什么 | 默认不审 | **总是审**（独立开关） |
 * | 为何 | 它是“自己动手查/算”，错了能改，且不改外部状态 | 发出去就是用户说的话，错发的代价由用户本人承担 |
 * | 入口 | 本函数 | `persona-outbound-review.ts` |
 *
 * 所以缺省是 `readonly`：**工具执行不弹卡，输出照旧过出站审核**。
 *
 * ## 四档
 *
 * | 值 | 工具执行 | 适用 |
 * |---|---|---|
 * | `readonly`（**缺省**） | 一律放行（不弹卡） | 生产默认：人格能干活，但发出去的话仍需人批准 |
 * | `ask` | 需审批工具才推卡问人 | 高敏环境（删除/提交单等需要人点头） |
 * | `auto` | 一律放行（旧名，等价 readonly） | 兼容旧配置 |
 * | `deny` | 一律拒绝 | 只读人格：不执行任何需审批操作 |
 */
export type PersonaApprovalMode = "readonly" | "ask" | "auto" | "deny"

/**
 * 解析审批姿态。
 *
 * 缺省 `readonly`：**工具执行不审，输出才审**（见类型注释里的区分）。
 *
 * 兼容旧值：`auto`/`off`/`true` → `auto`（与 readonly 同效）；`deny`/`false` → `deny`；
 * 只有显式 `ask` 才回到“每个需审批工具都推卡”。
 */
export function personaApprovalMode(env: EnvView): PersonaApprovalMode {
  const raw = (env.GEBAI_PERSONA_APPROVAL ?? "").trim().toLowerCase()
  if (raw === "ask") return "ask"
  if (raw === "auto" || raw === "off" || raw === "true" || raw === "1") return "auto"
  if (raw === "readonly" || raw === "read") return "readonly"
  if (raw === "deny" || raw === "false" || raw === "0" || raw === "no") return "deny"
  if (raw) return "readonly"
  // 向后兼容：旧变量 `GEBAI_PERSONA_AUTO_APPROVE` 只在**显式设为 false** 时收紧。
  const legacy = (env.GEBAI_PERSONA_AUTO_APPROVE ?? "").trim().toLowerCase()
  if (/^(false|0|no|off)$/.test(legacy)) return "deny"
  return "readonly"
}
