/**
 * 会话映射：把「这件事归到哪个上下文」做成**可配置的规则表**。
 *
 * ## 为什么需要规则表（而不是一个枚举 + 一个函数）
 *
 * 会话面向上下文，不面向群——但"上下文怎么切"是**部署事实**，每个部署不同：
 *
 * | 部署形态 | 想要的切法 |
 * |---|---|
 * | 飞书话题群答疑 | **每个群话题一个会话**（一个话题的追问共享上下文，话题之间隔离） |
 * | 普通群值班 | 每个群一个会话 |
 * | 一个人格服务多个客户 | 每个客户（群）一个会话，严格隔离 |
 * | 汇总型人格 | 全部共享一个上下文（"今天所有支持求助"） |
 * | 工单/项目型 | 按工单号、按仓库切（跨群共享） |
 *
 * 三档枚举表达不了这些；写成 ts 函数则**改一次就得改代码 + 过审**，而它明明是部署差异
 * （与 `GEBAI_PERSONA_<ID>_CHATS` / `_OUTPUT` 同一性质）——那个原则是「人格共性放代码、
 * 使用者差异放 env」，本文件就是把会话映射也纳入这条线。
 *
 * ## 形态：规则表（按顺序匹配，第一条命中即用）
 *
 * ```ts
 * [
 *   { when: "feishu:message", key: "{chat}__t_{thread}" },  // 话题群：每话题一会话
 *   { when: "message",        key: "{chat}" },              // 其余群消息：每群一会话
 *   { when: "*",              key: "" },                    // 兜底：常驻上下文
 * ]
 * ```
 *
 * - `when`：订阅语法（`*` / `kind` / `channel` / `channel:chatId:kind`，与人格订阅同一套匹配）
 * - `key`：会话键模板，`{}` 里是占位符；空串 = 该分身的常驻上下文
 * - `onMissing`：模板里的占位符解析不出值时怎么办（见下）
 *
 * ## 占位符
 *
 * | 占位符 | 取值 |
 * |---|---|
 * | `{channel}` | 渠道名（feishu / schedule / webhook…） |
 * | `{kind}` | 事件类别（message / schedule / webhook…） |
 * | `{chat}` | 群/会话 id（飞书 `oc_xxx`） |
 * | `{thread}` | 话题根消息 id（飞书话题群 `omt_xxx`；普通消息无） |
 * | `{sender}` | 发送者 id |
 * | `{senderName}` | 发送者显示名 |
 * | `{task}` | 调度任务 id |
 * | `{msg}` | 消息 id |
 * | `{extra.NAME}` | `refs.extra` 里的自定义字段（工单号、仓库名…由源自己塞） |
 *
 * 未知占位符在**加载期**报错（写错一个名字就让整条规则永不命中，而现象是「配置没生效」）。
 *
 * ## `onMissing`：占位符为空时的三种处理
 *
 * | 策略 | 行为 | 适用 |
 * |---|---|---|
 * | `skip`（**缺省，推荐**） | 本规则不命中，继续下一条 | `feishu:message={chat}__t_{thread}`——普通群没有话题时落到下一条 `{chat}` |
 * | `drop` | 删掉该占位符并折叠分隔符 | 简单的 `{chat}-{thread}` 拼接 |
 * | `literal` | 保留 `{thread}` 原样（调试用） | 排查"为什么这条规则没命中" |
 *
 * **为什么缺省是 skip 而不是 drop**：`{chat}__t_{thread}` 在 drop 下会得到 `oc_x__t_`
 * 这类残键（分隔符还在），而残键**看起来像正常会话**——上下文从此分裂且没人察觉。
 * skip 让它干净地落到下一条规则，语义明确。
 *
 * 这条"话题群 + 普通群混合"是**最常见的真实形态**：话题群每条消息都在某个话题下，
 * 普通群没有话题。所以 per_thread 预设天生写成两条规则。
 */

import type { PersonaEventRefs } from "./event"
import { matchesSubscription } from "./subscription"

/** 占位符缺失时的处理策略。 */
export type MissingPolicy = "skip" | "drop" | "literal"

/** 一条会话映射规则。 */
export interface SessionMapRule {
  /** 何时适用（订阅语法；缺省 `*` = 全部）。 */
  when?: string
  /** 会话键模板（`{}` 占位符）；空串 = 常驻上下文。 */
  key: string
  /** 占位符为空时的处理（缺省 `skip`）。 */
  onMissing?: MissingPolicy
}

/** 渲染会话键时的输入（与 `PersonaEvent` 同形的最小子集）。 */
export interface SessionMapContext {
  channel: string
  kind: string
  refs?: PersonaEventRefs
}

/** 已知占位符（加载期校验用：写错一个名字 = 该规则永不命中）。 */
export const KNOWN_PLACEHOLDERS = [
  "channel",
  "kind",
  "chat",
  "thread",
  "sender",
  "senderName",
  "task",
  "msg",
] as const

/** 占位符是否合法（`extra.*` 允许任意键名——工单号字段很可能就叫「工单」）。 */
export function isKnownPlaceholder(name: string): boolean {
  if ((KNOWN_PLACEHOLDERS as readonly string[]).includes(name)) return true
  // 键名不限 ASCII：事件源的 extra 字段完全可能用中文键（`extra.工单`），
  // 限制成 ASCII 会逼部署方把字段名改成拼音（然后模型、日志、配置三处各写一种）。
  // 只需排除“空键”与“带空白/花括号的怪形”（后者会破坏模板语法）。
  if (!name.startsWith("extra.")) return false
  const key = name.slice(6)
  return key.length > 0 && !/[\s{}]/.test(key)
}

const PLACEHOLDER_RE = /\{([A-Za-z][A-Za-z0-9_.]*)\}/g

/** 取占位符的值（空串与缺失同义：都算"没有这个信息"）。 */
function valueOf(name: string, ctx: SessionMapContext): string | undefined {
  const nonEmpty = (v: string | undefined): string | undefined => {
    const t = v?.trim()
    return t ? t : undefined
  }
  switch (name) {
    case "channel":
      return nonEmpty(ctx.channel)
    case "kind":
      return nonEmpty(ctx.kind)
    case "chat":
      return nonEmpty(ctx.refs?.chatId)
    case "thread":
      return nonEmpty(ctx.refs?.threadId)
    case "sender":
      return nonEmpty(ctx.refs?.senderId)
    case "senderName":
      return nonEmpty(ctx.refs?.senderName)
    case "task":
      return nonEmpty(ctx.refs?.taskId)
    case "msg":
      return nonEmpty(ctx.refs?.messageId)
    default:
      if (name.startsWith("extra.")) return nonEmpty(ctx.refs?.extra?.[name.slice(6)])
      return undefined
  }
}

/**
 * 折叠因删占位符留下的多余分隔符（`drop` 策略用）。
 *
 * **只做两件事**：连续分隔符压成一个、去掉首尾分隔符。刻意不做“智能识别标记段”的启发式——
 * 试过一条“分隔符 + 短字母 = 形态标记就整段删除”的规则，结果把 `oc_a` 的 `_a` 当标记删了，
 * 得到 `oc`（一个看起来正常、实则完全不同的会话键）。凡是靠猜的折叠都会在某类真实 id 上出错，
 * 而错的代价是上下文静静分裂。
 *
 * ## 因此：带形态标记的键请用 `skip`（缺省）而不是 `drop`
 *
 * `{chat}__t_{thread}` 缺 thread 时：
 * - `skip`（缺省）→ 本规则不命中，落到下一条 `{chat}` → 拿到干净的 `oc_a`
 * - `drop` → `oc_a_t`（`__t` 被压成 `_t`）——能用，但会带上标记残留
 *
 * 所以 `per_thread` 预设天生写成两条规则，而不是靠 `drop` 一条蛇到底。
 */
function collapseSeparators(s: string): string {
  return s
    .replace(/[_.:-]{2,}/g, (m) => m[0]!)
    .replace(/^[_.:-]+|[_.:-]+$/g, "")
}

/**
 * 渲染一条规则的会话键。
 *
 * @returns `matched=false` 表示本规则不适用（占位符缺失且策略为 skip），调用方应继续下一条
 */
export function renderSessionKey(
  rule: SessionMapRule,
  ctx: SessionMapContext,
): { matched: boolean; key: string; missing: string[] } {
  const policy: MissingPolicy = rule.onMissing ?? "skip"
  const missing: string[] = []
  // literal：原样保留缺失的占位符（其余照常替换）
  if (policy === "literal") {
    const key = rule.key.replace(PLACEHOLDER_RE, (whole, name: string) => {
      const v = valueOf(name, ctx)
      if (v === undefined) missing.push(name)
      return v === undefined ? whole : v
    })
    return { matched: true, key, missing }
  }
  const rendered = rule.key.replace(PLACEHOLDER_RE, (_whole, name: string) => {
    const v = valueOf(name, ctx)
    if (v === undefined) {
      missing.push(name)
      return ""
    }
    return v
  })
  if (missing.length === 0) return { matched: true, key: rendered, missing }
  if (policy === "skip") return { matched: false, key: "", missing }
  // drop
  const dropped = collapseSeparators(rendered)
  // 全被删光 = 本规则实质上不适用（不要产出一个空键冒充"常驻"）
  return dropped ? { matched: true, key: dropped, missing } : { matched: false, key: "", missing }
}

/**
 * 按规则表求会话键。
 *
 * @returns `undefined` = 没有任何规则命中（调用方回落到自己的默认）；
 *          `""` = 命中了且明确要求"常驻上下文"（这两者必须区分）
 */
export function matchSessionMap(
  rules: readonly SessionMapRule[] | undefined,
  ctx: SessionMapContext,
): string | undefined {
  if (!rules?.length) return undefined
  const coords = { kind: ctx.kind, channel: ctx.channel, ...(ctx.refs?.chatId ? { chatId: ctx.refs.chatId } : {}) }
  for (const rule of rules) {
    if (!matchesSubscription(rule.when ?? "*", coords)) continue
    const r = renderSessionKey(rule, ctx)
    if (!r.matched) continue
    return r.key
  }
  return undefined
}

/** 模板里引用的占位符（去重保序；加载期校验用）。 */
export function placeholdersOf(template: string): string[] {
  const out: string[] = []
  for (const m of template.matchAll(PLACEHOLDER_RE)) {
    const name = m[1]!
    if (!out.includes(name)) out.push(name)
  }
  return out
}

/**
 * 内置预设（`GEBAI_PERSONA_<ID>_SESSION_MAP=per_thread` 这样用）。
 *
 * 预设的价值：90% 的部署只是从这几种里选一种，不该为它写规则表语法。
 */
export const SESSION_MAP_PRESETS: Record<string, SessionMapRule[]> = {
  /** 一个上下文：全部事件共享该分身的常驻会话。 */
  single: [{ when: "*", key: "" }],
  /** 每个群一个会话（无群信息的落到常驻）。 */
  per_chat: [{ when: "message", key: "{chat}" }],
  /**
   * 每个**群话题**一个会话（飞书话题群）；无话题的消息退回按群。
   *
   * 这是"飞书群话题映射会话"的标准形态：话题内追问共享上下文，话题之间互不污染；
   * 普通群（无 threadId）自然落到第二条。
   */
  per_thread: [
    { when: "message", key: "{chat}__t_{thread}" },
    { when: "message", key: "{chat}" },
  ],
  /** 每个发言者一个会话（客服型人格：同一客户的追问都进他自己的上下文）。 */
  per_sender: [{ when: "message", key: "sender:{sender}" }],
}

/**
 * 解析 `GEBAI_PERSONA_<ID>_SESSION_MAP` 的值。
 *
 * 三种写法都支持（按顺序探测，因为部署方的心智不同）：
 *
 * ```ini
 * # ① 预设名
 * GEBAI_PERSONA_XIAOSHUANG_SESSION_MAP=per_thread
 *
 * # ② 紧凑 DSL：`when=key`，分号分隔；when 省略即 `*`
 * GEBAI_PERSONA_XIAOSHUANG_SESSION_MAP=feishu:message={chat}__t_{thread};message={chat};*=
 *
 * # ③ JSON（需要 onMissing 等高级选项时）
 * GEBAI_PERSONA_XIAOSHUANG_SESSION_MAP=[{"when":"message","key":"{chat}__t_{thread}"},{"when":"*","key":""}]
 * ```
 *
 * 解析失败**不静默**：进 `errors`，由调用方如实上报（一条配错的映射 = 上下文悄悄分裂）。
 */
export function parseSessionMap(raw: string | undefined): { rules: SessionMapRule[]; errors: string[] } {
  const text = (raw ?? "").trim()
  if (!text) return { rules: [], errors: [] }

  // ① 预设
  const preset = SESSION_MAP_PRESETS[text.toLowerCase()]
  if (preset) return { rules: preset.map((r) => ({ ...r })), errors: [] }

  // ③ JSON
  if (text.startsWith("[")) {
    let parsed: unknown
    try {
      parsed = JSON.parse(text)
    } catch (err) {
      return { rules: [], errors: [`SESSION_MAP 不是合法 JSON：${String((err as Error)?.message || err)}`] }
    }
    const bad = validateSessionMap(parsed)
    if (bad) return { rules: [], errors: [bad] }
    return { rules: parsed as SessionMapRule[], errors: [] }
  }

  // ② 紧凑 DSL
  const errors: string[] = []
  const rules: SessionMapRule[] = []
  for (const part of text.split(";")) {
    const item = part.trim()
    if (!item) continue
    const eq = item.indexOf("=")
    if (eq < 0) {
      errors.push(`规则「${item}」缺少 = （写法：when=key，如 message={chat}）`)
      continue
    }
    const when = item.slice(0, eq).trim() || "*"
    const key = item.slice(eq + 1).trim()
    const rule: SessionMapRule = { when, key }
    const badWhen = validateWhen(when)
    if (badWhen) {
      errors.push(badWhen)
      continue
    }
    const badKey = validateKey(key)
    if (badKey) {
      errors.push(badKey)
      continue
    }
    rules.push(rule)
  }
  if (errors.length) return { rules: [], errors }
  return { rules, errors: [] }
}

/** `when` 校验（复用订阅语法，容忍 `*`）。 */
function validateWhen(when: string): string | null {
  if (when === "*") return null
  const parts = when.split(":")
  if (parts.length > 3) return `SESSION_MAP 的 when「${when}」段数过多（最多 channel:chatId:kind）`
  for (const p of parts) {
    if (!/^[a-z0-9_]+$/i.test(p.trim())) {
      return `SESSION_MAP 的 when「${when}」含非法字符（每段须为字母/数字/下划线）`
    }
  }
  return null
}

/** `key` 校验：占位符必须已知（写错一个名字 = 该规则永不命中）。 */
function validateKey(key: string): string | null {
  for (const name of placeholdersOf(key)) {
    if (!isKnownPlaceholder(name)) {
      return (
        `SESSION_MAP 的 key「${key}」含未知占位符 {${name}}` +
        `（可用：${KNOWN_PLACEHOLDERS.map((p) => `{${p}}`).join(" ")} {extra.名字}）`
      )
    }
  }
  return null
}

/** 规则表整体校验（加载期用；返回错误文案，合法返回 null）。 */
export function validateSessionMap(raw: unknown): string | null {
  if (!Array.isArray(raw)) return "sessionMap 必须是数组（规则表）"
  if (raw.length === 0) return "sessionMap 不能为空数组（要么不写，要么至少一条规则）"
  for (const [i, item] of raw.entries()) {
    if (!item || typeof item !== "object") return `sessionMap[${i}] 必须是对象`
    const r = item as Record<string, unknown>
    const allowed = new Set(["when", "key", "onMissing"])
    for (const k of Object.keys(r)) {
      if (!allowed.has(k)) return `sessionMap[${i}] 含未知字段「${k}」（可用：when / key / onMissing）`
    }
    if (typeof r.key !== "string") return `sessionMap[${i}].key 必须是字符串（会话键模板，可为空串）`
    if (r.when !== undefined) {
      if (typeof r.when !== "string") return `sessionMap[${i}].when 必须是字符串`
      const bad = validateWhen(r.when)
      if (bad) return bad
    }
    if (r.onMissing !== undefined && r.onMissing !== "skip" && r.onMissing !== "drop" && r.onMissing !== "literal") {
      return `sessionMap[${i}].onMissing 必须是 skip / drop / literal（当前：${String(r.onMissing)}）`
    }
    const badKey = validateKey(r.key)
    if (badKey) return badKey
  }
  return null
}
