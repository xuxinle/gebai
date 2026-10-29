/**
 * 人格出站治理（act 的 feishu 发送前置闸）。
 *
 * 三件事，全部在**工具层**裁决（渠道包不动——channels 包保持零依赖，人格语义
 * 不属于「怎么发消息」，属于「谁在发、以什么身份发、要不要人先看一眼」）：
 *
 * ## 1. 禁止机器人身份回退
 *
 * 人格会话（`SessionData.persona` 存在）里 `as_user=false` 的发送**直接拒绝**。
 * 人格的定义就是「以用户本人在群里说话」——模型发现 as_user=true 被拒后改用
 * 机器人名义发，等于人格悄悄变成了机器人：群里看到的是 app 头像，用户身份
 * 形同虚设，而「为什么突然变成机器人发的」没有任何痕迹可查。
 *
 * 普通会话不受影响（用户自己在对话里让机器人代发是合法用法）。
 *
 * ## 2. 授权不可用 → 相期弹卡
 *
 * 发送因「用户授权不可用」被拒时，推 `card_authorize`（带授权链接）。
 * 不能只靠 5 分钟定时器：用户在群里@人格等回复的**那一刻**就该看到提醒，
 * 而不是等下一次巡检。弹卡失败（相期未连）不阻断拒绝本身——卡降级进 inbox。
 *
 * ## 3. 出站审核（默认开，GEBAI_PERSONA_OUTBOUND_REVIEW=false 关）
 *
 * 人格会话的每次飞书发送前，先推 `card_ask`（choice：批准发送 / 驳回），
 * 正文带目标群与消息全文，等用户点选：
 * - 批准 → 放行本次 act
 * - 驳回 → 拒绝（模型收到「用户已驳回」，不会重试发送）
 * - 超时 → 拒绝（fail-closed：没人看 ≠ 默认可以发）
 * - 相期未连接 → 拒绝（同上，且理由里说清相期没开）
 *
 * 为何默认开：人格以**用户本人**名义发言——发出去的消息就是用户说的话，
 * 错发一条的代价（说错话、发错群、内容不当）由用户本人承担。审核一次
 * 是这个身份绑定的合理成本；嫌烦可显式关（GEBAI_PERSONA_OUTBOUND_REVIEW=false）。
 *
 * ## 实现注记
 *
 * - 身份从 `ctx.sessions.store.load` 反查（与 persona-outbound 同模式，不由模型自报）
 * - 相期卡经 `subAgents.defOf("xiangqi")` 取工具执行（与 compose.syncAuthCard 同模式，
 *   不直接 import 相期源码——内置子Agent 是构建期打包的）
 * - 工具引用经 ToolContext 注入（`ctx.personaOutboundReview`），compose 装配时提供
 */

import type { ToolContext } from "../base/types"

/** 相期 card_ask 工具的最小执行面（注入形态；compose 从 subAgents.defOf 取）。 */
export interface AskCardTool {
  execute(
    args: Record<string, unknown>,
    ctx: unknown,
  ): Promise<{ output: string; data?: { id?: string; delivered?: boolean; answered?: boolean; answer?: { choice?: unknown } } }>
}

/** 出站审核配置（每次调用现读 env，改配置无需重启）。 */
export function outboundReviewEnabled(env: Record<string, string | undefined> = process.env): boolean {
  const raw = (env.GEBAI_PERSONA_OUTBOUND_REVIEW ?? "").trim().toLowerCase()
  if (!raw) return true // 缺省开
  return !/^(off|0|false|no)$/i.test(raw)
}

/** 会话的人格归属（缓存模块级——store.load 每会话首查后 memo；同 persona-outbound 共用逻辑的本地实现）。 */
async function personaOwnerOf(
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

export interface OutboundGateResult {
  /** false = 拒绝（reason 有文案）。 */
  allowed: boolean
  reason?: string
  /** 已弹出相期卡（审核卡/授权卡）。 */
  cardPushed?: boolean
  /** 审核状态（allowed=true 时才有意义）。 */
  review?: "approved" | "not-required" | "disabled"
}

export interface OutboundGateDeps {
  /** 相期 card_ask 工具（compose 注入；缺省 = 审核环节跳过并记日志——不能因装配缺失把发送全堵死）。 */
  askCard?: AskCardTool
  /** 授权卡推送（compose 注入 syncAuthCard；缺省跳过弹卡）。 */
  pushAuthCard?: () => Promise<void>
  /** 相期重启（compose 注入 launchXiangqi；缺省不重启）。 */
  restartXiangqi?: () => Promise<void>
  /** 相期是否已连上（compose 注入 xiangqiController().isConnected；缺省视为已连）。 */
  isXiangqiConnected?: () => boolean
  /** 重启后等待重连的超时（毫秒；缺省 15s；测试注入小值）。 */
  reconnectWaitMs?: number
  env?: Record<string, string | undefined>
}

/** 重启后等待相期重连的超时（毫秒）。 */
const RECONNECT_WAIT_MS = 15_000
/** 同一段时间内只重启一次相期（防抖动风暴：相期反复崩溃时别每张卡都拉一次）。 */
const RESTART_COOLDOWN_MS = 60_000

let lastRestartAt = 0
let restartInflight: Promise<boolean> | null = null

/** 仅测试用：重置恢复闭环的模块级状态（冷却/单飞），避免用例间串扰。 */
export function _resetXiangqiRecoveryForTest(): void {
  lastRestartAt = 0
  restartInflight = null
}

/**
 * 送达前检查：相期不在 → 自动重启 + 等重连。返回 true = 现在可以送卡。
 *
 * 为什么不把重启放在「失败后」：那是把一次送卡分成两步（先送、失败、再重启、再送），
 * 状态复杂且中间隔了用户等待；在送之前确认通道是活的，一步到位。
 * 重启成本低（秒级），对「必须送到用户眼前」的审核卡值得。
 */
export async function ensureXiangqiReady(deps: Pick<OutboundGateDeps, "restartXiangqi" | "isXiangqiConnected" | "reconnectWaitMs">): Promise<{ ok: boolean; restarted: boolean }> {
  const isConnected = deps.isXiangqiConnected
  // 未注入探针 = 视为已连（不阻塞审核；送卡本身还有 delivered 兜底）
  if (!isConnected || isConnected()) return { ok: true, restarted: false }
  if (!deps.restartXiangqi) return { ok: false, restarted: false } // 没重启能力：如实不可用

  const waitMs = deps.reconnectWaitMs ?? RECONNECT_WAIT_MS
  const now = Date.now()
  if (now - lastRestartAt < RESTART_COOLDOWN_MS) {
    // 冷却期内不重复重启：等它自己连（可能正在重启中）
    return { ok: await waitConnected(isConnected, waitMs), restarted: false }
  }
  lastRestartAt = now
  restartInflight = restartInflight ?? (async () => {
    try {
      await deps.restartXiangqi?.()
      return await waitConnected(isConnected, waitMs)
    } catch {
      return false
    } finally {
      restartInflight = null
    }
  })()
  const ok = await restartInflight
  return { ok, restarted: true }
}

/** 轮询等待相期重连。 */
async function waitConnected(isConnected: () => boolean, timeoutMs: number): Promise<boolean> {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (isConnected()) return true
    await new Promise((r) => setTimeout(r, 300))
  }
  return isConnected()
}

/** 审核卡等待时限（毫秒）：5 分钟对齐 card_ask 缺省。 */
const REVIEW_WAIT_MS = 5 * 60_000

/** 消息全文截断（审核卡正文别太长——人要在小卡片上读完）。 */
const REVIEW_BODY_MAX = 800

/**
 * 人格出站闸：act 发 feishu 消息（send/reply）前的三重检查。
 *
 * @param ctx 工具上下文
 * @param input { target, verb, args }（act 的原始入参）
 * @param deps 注入面（相期卡工具 / 授权卡推送 / env）
 */
export async function personaOutboundGate(
  ctx: Pick<ToolContext, "sessionId" | "user" | "sessions">,
  input: { target: string; verb: string; args: Record<string, unknown> },
  deps: OutboundGateDeps = {},
): Promise<OutboundGateResult> {
  // 只治理 feishu 发送类动词（observe / 其它渠道不走这里）
  if (!input.target.startsWith("feishu:")) return { allowed: true, review: "not-required" }
  if (input.verb !== "send" && input.verb !== "reply") return { allowed: true, review: "not-required" }

  const personaId = await personaOwnerOf(ctx)
  if (!personaId) return { allowed: true, review: "not-required" } // 普通会话不治理

  const env = deps.env ?? (process.env as Record<string, string | undefined>)

  // ① 机器人身份禁止：人格必须 as_user=true
  if (input.args.as_user !== true) {
    return {
      allowed: false,
      reason:
        `人格「${personaId}」不可用机器人身份发消息——人格的定义就是以用户本人在群里说话。` +
        `请改用 as_user=true 重发（需飞书用户授权有效；若授权失效会收到明确指引）。`,
    }
  }

  // ② 出站审核（默认开）：推 card_ask 等批准
  if (!outboundReviewEnabled(env)) return { allowed: true, review: "disabled" }

  // 送卡前确保相期通道就绪：不在 → 自动重启 + 等重连（防抖/single-flight 在 ensureXiangqiReady 内）
  const ready = await ensureXiangqiReady(deps)
  if (!ready.ok) {
    return {
      allowed: false,
      reason: ready.restarted
        ? "相期窗口不可用，已尝试自动重启仍未连上——审核卡送不到（fail-closed 不发送）。请检查相期窗口或稍后重试。"
        : "相期窗口不可用且无法自动重启（未装配重启能力）——审核卡送不到（fail-closed 不发送）。",
    }
  }

  const text = String(input.args.text ?? "")
  const bodyText = text.length > REVIEW_BODY_MAX ? text.slice(0, REVIEW_BODY_MAX) + `…（共 ${text.length} 字符，已截断）` : text
  const ask = deps.askCard
  if (!ask) {
    // 装配缺失（相期子Agent 未注册）：不能静默放行（审核是默认安全语义），
    // 也不能静默全堵（一次装配问题变成人格全哑）。如实拒绝并说明原因。
    return {
      allowed: false,
      reason: "出站审核已启用但相期卡工具未装配（xiangqi 子Agent 未注册）——请联系管理员或设 GEBAI_PERSONA_OUTBOUND_REVIEW=false 临时关闭审核。",
      review: undefined,
    }
  }

  const res = await ask.execute(
    {
      kind: "choice",
      title: `人格「${personaId}」请求向飞书群发送消息`,
      from: "出站审核",
      body:
        `**目标**：${input.target}${input.verb === "reply" ? `（引用回复 ${String(input.args.reply_to ?? "?")}）` : ""}\n\n` +
        `**正文**：\n\n${bodyText}\n\n` +
        (input.args.why ? `**缘由**：${String(input.args.why)}\n\n` : "") +
        `批准将以**你的名义**发出；驳回则不发送。`,
      options: [
        { label: "批准发送", value: "approve" },
        { label: "驳回", value: "reject" },
      ],
      wait_ms: REVIEW_WAIT_MS,
    },
    {},
  )

  const data = res.data ?? {}
  if (data.delivered === false) {
    return {
      allowed: false,
      cardPushed: false,
      reason:
        `出站审核卡未送达（相期未连接）——fail-closed 不发送。` +
        `请打开相期窗口后重试，或让管理员设 GEBAI_PERSONA_OUTBOUND_REVIEW=false 关闭审核。`,
    }
  }
  if (data.answered !== true) {
    return {
      allowed: false,
      cardPushed: true,
      reason: `出站审核等待 ${Math.round(REVIEW_WAIT_MS / 60000)} 分钟无人答复（卡片仍在相期窗口上）——fail-closed 不发送。`,
    }
  }
  const choice = String((data.answer as { choice?: unknown } | undefined)?.choice ?? "")
  if (choice !== "approve") {
    return {
      allowed: false,
      cardPushed: true,
      reason: "用户已在相期驳回本次发送——请勿重试同一条消息；若有疑问先在会话里向用户说明。",
    }
  }
  return { allowed: true, review: "approved", cardPushed: true }
}

/**
 * 授权弹卡钩子：act 失败原因是「用户授权不可用」时调用（在 act 工具的失败路径接入）。
 *
 * 返回 true = 已尝试弹卡。弹卡本身失败不抛——授权指引已随拒绝文案给了模型。
 */
export async function notifyAuthNeeded(pushAuthCard: (() => Promise<void>) | undefined): Promise<boolean> {
  if (!pushAuthCard) return false
  try {
    await pushAuthCard()
    return true
  } catch {
    return false
  }
}

/** act 输出里是否为「授权不可用」类失败（据此触发授权弹卡）。 */
export function isAuthFailure(output: string): boolean {
  return (
    output.includes("用户授权不可用") ||
    output.includes("as_user=true 但飞书用户授权") ||
    // 拟人未启用/未装配 personaCredentials 的同族失败（授权态问题，非网络/权限码）
    (output.includes("as_user=true") && output.includes("需要拟人模式已启用") && output.includes("personaCredentials"))
  )
}
