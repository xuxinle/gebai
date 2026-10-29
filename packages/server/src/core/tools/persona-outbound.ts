/**
 * 人格出站约束（act/observe 的按人格目标过滤）。
 *
 * ## 与入站对称
 *
 * 入站侧（compose `applyPersonaEnv`）把 `GEBAI_PERSONA_<ID>_CHATS` 注入人格订阅：
 * 人格只**收**这些群的消息。出站侧在这里补上另一半——人格会话只**发**到这些群。
 *
 * 只拦入站不拦出站的后果：A 人格收到群甲的消息，却能 act(reply) 发到群乙
 * （群乙在渠道全局白名单内，但这个人格本不该碰它）。双向收窄才算「这个人格
 * 只管这些群」。
 *
 * ## 判定链（fail-closed）
 *
 * 1. 非人格会话（普通对话/子会话无 persona 归属）→ 不过滤（沿用渠道全局白名单）
 * 2. 人格会话但未配 `GEBAI_PERSONA_<ID>_CHATS` → 不过滤（未配置 = 不限群，与入站同语义）
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

/** 目标地址里取飞书群 id：`feishu:oc_xxx` → `oc_xxx`。非飞书目标返回 null。 */
export function feishuChatIdOf(target: string): string | null {
  if (!target) return null
  const idx = target.indexOf(":")
  if (idx < 0) return null
  const channel = target.slice(0, idx).trim().toLowerCase()
  const addr = target.slice(idx + 1).trim()
  if (channel !== "feishu" || !addr) return null
  // 出站到群地址形如 oc_xxx（chat_id）；p2p 单聊是 ou_ 开头用户 id——也当群地址处理（白名单按实际 chat_id 配）
  return addr
}

/** 读 `GEBAI_PERSONA_<ID>_CHATS`（与入站同变量同语义）。返回已配置的群 id 集合；未配置返回 null。 */
export function personaChatWhitelist(personaId: string, env: Record<string, string | undefined>): Set<string> | null {
  const raw = env[`GEBAI_PERSONA_${personaId.toUpperCase()}_CHATS`] ?? ""
  const chats = raw.split(/[,\s]+/).map((s) => s.trim()).filter(Boolean)
  return chats.length ? new Set(chats) : null
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
 * @param ctx 工具上下文（取 sessionId / user / sessions.store.load）
 * @param target act/observe 的 `channel:address`
 * @param env 环境变量（进程 env 即可；测试注入）
 */
export async function personOutboundDecision(
  ctx: Pick<ToolContext, "sessionId" | "user" | "sessions">,
  target: string,
  env: Record<string, string | undefined> = process.env as Record<string, string | undefined>,
): Promise<PersonaOutboundDecision> {
  // 非飞书目标：人格约束只针对飞书群（其它渠道走自身白名单）
  const chatId = feishuChatIdOf(target)
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

/** act/observe 共用的出站拦截：不通过时返回拒绝文案；通过返回 null。 */
export async function personaOutboundBlock(
  ctx: Pick<ToolContext, "sessionId" | "user" | "sessions">,
  target: string,
  env: Record<string, string | undefined> = process.env as Record<string, string | undefined>,
): Promise<string | null> {
  const d = await personOutboundDecision(ctx, target, env)
  return d.allowed ? null : d.reason ?? "出站目标不在该人格允许的群名单内"
}