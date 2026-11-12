/**
 * 人格出站约束（act/observe 的按人格目标过滤）。
 *
 * ## 与入站对称
 *
 * 入站侧（compose `applyPersonaEnv`）把 `GEBAI_PERSONA_<ID>` 数组里的 `chats` 注入人格订阅：
 * 人格只**收**这些群的消息。出站侧在这里补上另一半——人格会话只**发**到这些群。
 *
 * 只拦入站不拦出站的后果：A 人格收到群甲的消息，却能 act(reply) 发到群乙
 * （群乙在渠道全局白名单内，但这个人格本不该碰它）。双向收窄才算「这个人格
 * 只管这些群」。
 *
 * ## 判定链（fail-closed）
 *
 * 1. 非人格会话（普通对话/子会话无 persona 归属）→ 不过滤（沿用渠道全局白名单）
 * 2. 人格会话但 `GEBAI_PERSONA_<ID>` 里没写 `chats` → 不过滤（未配置 = 不限群，与入站同语义）
 * 3. 人格会话 + 已配置 → 目标群必须在名单内，否则**直接拒绝**（不进审批）
 *
 * 身份从会话归属读（`SessionData.persona`），不由模型自报——与 driver_tools 同一原则。
 *
 * ## 实现注记
 *
 * act/observe 的 ToolContext 无 store 直柄，但 `ctx.sessions.store.load` 已注入
 * （session 管控服务），用它反查归属。env 用进程变量直读（与入站的进程级一致）。
 */

import type { ToolContext } from "../base/types"
import { dirRefsOf, parsePersonaEnv } from "@gebai/persona"

/**
 * 目标前缀对应的渠道**类型名**（实例名 → 类型；未登记则按名字本身兜底）。
 *
 * 为何必须经这层映射而不能直接比对前缀字串：实例名是部署方起的
 * （`feishu_in` / `feishu_out` / `ops_bot`），而“这是不是一个飞书渠道”是类型事实。
 * 写死 `=== "feishu"` 的后果是**按实例名配置的治理整段静默失效**——人格出站审核、
 * 群白名单、@全体整形全部跳过，日志里一个字都没有。
 *
 * 未登记时回落成名字本身：类型名与实例名同形（可选类型的缺省实例、测试桩）时
 * 行为与旧写法完全一致。
 */
export function channelTypeOf(channel: string, instanceTypes?: ReadonlyMap<string, string>): string {
  const name = String(channel ?? "").trim().toLowerCase()
  if (!name) return ""
  return (instanceTypes?.get(name) ?? name).toLowerCase()
}

/** 该目标的渠道**类型**是否为飞书（`feishu` / `feishu_out` / 自定义实例名都算）。 */
export function isFeishuChannel(channel: string, instanceTypes?: ReadonlyMap<string, string>): boolean {
  return channelTypeOf(channel, instanceTypes) === "feishu"
}

/** 该 target 是否是飞书侧目标（`<实例名>:<地址>`，实例名映射到类型 feishu 即算）。 */
export function isFeishuTarget(target: string, instanceTypes?: ReadonlyMap<string, string>): boolean {
  const idx = String(target ?? "").indexOf(":")
  if (idx < 0) return false
  return isFeishuChannel(target.slice(0, idx), instanceTypes)
}

/** 目标地址里取飞书群 id：`feishu:oc_xxx` / `feishu_out:oc_xxx` → `oc_xxx`。非飞书目标返回 null。 */
export function feishuChatIdOf(target: string, instanceTypes?: ReadonlyMap<string, string>): string | null {
  if (!target) return null
  const idx = target.indexOf(":")
  if (idx < 0) return null
  const channel = target.slice(0, idx).trim()
  const addr = target.slice(idx + 1).trim()
  if (!isFeishuChannel(channel, instanceTypes) || !addr) return null
  // 出站到群地址形如 oc_xxx（chat_id）；p2p 单聊是 ou_ 开头用户 id——也当群地址处理（白名单按实际 chat_id 配）
  return addr
}

/**
 * 读 `GEBAI_PERSONA_<ID>` 数组里的群白名单（与入站同源）。返回已配置的群 id 集合；未配置返回 null。
 *
 * 三个来源**并集**：`listen.chats`（它在哪些群说话）、`send.chats`（它往哪些群发）、
 * 以及旧的扁平 `chats`（`dirRefsOf` 已把回落关系处理掉）。
 *
 * 为何不再只读扁平 `chats`：`listen` / `send` 是现行写法，只读扁平字段会让
 * 「按新写法配了 send.chats」的人格**出站白名单为空**——{@link personOutboundDecision}
 * 把空集当“未配置 → 不限群”，于是人格能把消息发到任何群。那是双向收窄少了一半。
 *
 * 取并集而非只看 send 的原因：会话归属只带 personaId、不带 driverId，出站判定
 * 不知道具体是哪个分身；只要这个群在该人格任一配置项里，就允许发（宽松但一致）。
 */
export function personaChatWhitelist(
  personaId: string,
  env: Record<string, string | undefined>,
): Set<string> | null {
  const parsed = parsePersonaEnv(personaId, env[`GEBAI_PERSONA_${personaId.toUpperCase()}`])
  const all = new Set<string>()
  const add = (item: Parameters<typeof dirRefsOf>[0]) => {
    const refs = dirRefsOf(item)
    for (const c of refs.listen.chats) all.add(c)
    for (const c of refs.send.chats) all.add(c)
  }
  if (parsed.personaDefaults) add(parsed.personaDefaults as Parameters<typeof dirRefsOf>[0])
  for (const item of parsed.drivers.values()) add(item)
  return all.size ? all : null
}

/** 详情（供拒绝信息带出，方便排查）。 */
export interface PersonaOutboundDecision {
  allowed: boolean
  /** 本会话归属的人格（非人格会话无）。 */
  personaId?: string
  /** 是否命中了某人格的群白名单（未配置 = undefined）。 */
  restricted?: boolean
  /** 拒绝原因（allowed=false 时）。 */
  reason?: string
}

/**
 * 判定一次出站目标是否被人格约束放行。
 *
 * @param ctx 工具上下文（取 sessionId / user / sessions.store.load / channels 实例类型表）
 * @param target act/observe 的 `channel:address`
 * @param env 环境变量（进程 env 即可；测试注入）
 */
export async function personOutboundDecision(
  ctx: Pick<ToolContext, "sessionId" | "user" | "sessions" | "channels">,
  target: string,
  env: Record<string, string | undefined> = process.env as Record<string, string | undefined>,
): Promise<PersonaOutboundDecision> {
  // 非飞书目标：人格约束只针对飞书群（其它渠道走自身白名单）。
  // 判“是不是飞书”按**渠道类型**（实例名 → 类型），不看前缀字串。
  const types = instanceTypesOf(ctx)
  const chatId = feishuChatIdOf(target, types)
  if (!chatId) return { allowed: true }

  // 会话归属（普通会话 → 无 persona）
  let personaId: string | undefined
  try {
    const session = await ctx.sessions?.store?.load(ctx.sessionId, ctx.user)
    // store 的 SessionData 含 persona（base 类型未声明，结构化访问）
    const p = (session as { persona?: { personaId?: string } } | null)?.persona
    if (p?.personaId) personaId = p.personaId
  } catch {
    personaId = undefined // 读不到归属默认放行（与入站未配置 = 不限群一致）
  }
  if (!personaId) return { allowed: true }

  const whitelist = personaChatWhitelist(personaId, env)
  if (!whitelist) return { allowed: true, personaId }

  if (whitelist.has(chatId)) return { allowed: true, personaId, restricted: true }

  return {
    allowed: false,
    personaId,
    restricted: true,
    reason: `该会话属于人格「${personaId}」（GEBAI_PERSONA_${personaId.toUpperCase()}_CHATS 只允许 ${[...whitelist].join("、")}），目标 ${target} 不在其内——出站被拒（与入站同源收窄）。如确需发送，请人工放行该群到配置或换会话。`,
  }
}

/** 渠道注册表上的「实例名 → 类型名」表（未装配注册表时为空表 → 前缀按名字本身兜底）。 */
export function instanceTypesOf(ctx: { channels?: ToolContext["channels"] }): ReadonlyMap<string, string> {
  try {
    return ctx.channels?.instanceTypes() ?? new Map<string, string>()
  } catch {
    return new Map<string, string>()
  }
}

/** act/observe 共用的出站拦截：不通过时返回拒绝文案；通过返回 null。 */
export async function personaOutboundBlock(
  ctx: Pick<ToolContext, "sessionId" | "user" | "sessions" | "channels">,
  target: string,
  env: Record<string, string | undefined> = process.env as Record<string, string | undefined>,
): Promise<string | null> {
  const d = await personOutboundDecision(ctx, target, env)
  return d.allowed ? null : d.reason ?? "出站目标不在该人格允许的群名单内"
}

/** 会话归属的人格 id（非人格会话返回 null）。 */
export async function personaIdOfSession(
  ctx: Pick<ToolContext, "sessionId" | "user" | "sessions">,
): Promise<string | null> {
  if (!ctx.sessionId) return null
  try {
    const session = await ctx.sessions?.store?.load(ctx.sessionId, ctx.user)
    const p = (session as { persona?: { personaId?: string } } | null)?.persona
    return p?.personaId ?? null
  } catch {
    return null
  }
}