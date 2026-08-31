import { createHmac } from "node:crypto"
import { checkWebhookUrl } from "../webhooks"
import { fetchWithRedirectGuard } from "./tools"

/** 通知正文在通道消息中的保留长度（卡片 lark_md 内容限 4000 字符）。 */
export const NOTIFY_TEXT_MAX = 2000
/** 通知投递超时。 */
export const NOTIFY_TIMEOUT_MS = 10_000

/** 通知通道类型：webhook=通用 HTTP 回调；feishu=飞书群自定义机器人 webhook；feishu_chat=飞书应用消息（chat_id）。 */
export type CronNotifyType = "webhook" | "feishu" | "feishu_chat"

/** @ 人配置：id 为 open_id（ou_/un_/on_ 前缀）或 "all"（@所有人）；name 为展示名（缺省由客户端解析真实姓名）。 */
export interface FeishuAtTarget {
  id: string
  name?: string
}

/** 定时任务通知通道（任务内嵌配置，可配多条）。 */
export interface CronNotifyChannel {
  type: CronNotifyType
  /** webhook/feishu：http(s) URL（feishu 须为 open.feishu.cn 群机器人 webhook）；feishu_chat：群 chat_id。 */
  target: string
  /** feishu 加签密钥（可选，群机器人安全设置「签名校验」）。 */
  secret?: string
  /** @ 人名单（仅 feishu/feishu_chat 通道；卡片内以 lark_md at 标签渲染）。 */
  at?: FeishuAtTarget[]
}

/** 定时任务运行结果通知载荷（webhook 通道 JSON 原样投递，飞书通道格式化为 markdown 卡片）。 */
export interface CronResultNotification {
  event: "cron.result"
  task: { id: string; name: string; type: string; schedule: string; user: string }
  ok: boolean
  status: "success" | "error" | "skipped" | "timeout"
  at: number
  durationMs?: number
  output?: string
  error?: string
  /** 本次运行使用的会话（prompt 型执行轨迹）。 */
  sessionId?: string
  /** 运行后任务被自动停用（一次性任务完成 / 连续失败阈值）。 */
  disabled?: boolean
  /** 手动触发（cron_trigger / REST run）。 */
  manual?: boolean
}

export interface NotifyDeps {
  /** 注入 HTTP 客户端（默认全局 fetch；测试用）。 */
  fetchImpl?: (url: string, init: RequestInit) => Promise<{ ok: boolean; status: number }>
  /** 飞书开放平台消息发送（feishu_chat 通道用；未配置时该通道报错）。 */
  feishuSend?: (chatId: string, card: Record<string, unknown>) => Promise<void>
  /** 时钟（加签时间戳用，可注入）。 */
  now?: () => number
}

/** at 名单输入归一：条目为字符串（id）或 {id,name}；去重、非法 id 拒绝。 */
export function normalizeAtList(raw: unknown): FeishuAtTarget[] | undefined {
  if (raw === undefined || raw === null) return undefined
  if (!Array.isArray(raw)) throw new Error("at 须为 @ 人名单数组（open_id 或 \"all\"）")
  const out: FeishuAtTarget[] = []
  for (const item of raw) {
    let entry: FeishuAtTarget
    if (typeof item === "string") entry = { id: item.trim() }
    else if (item && typeof item === "object") {
      const id = String((item as { id?: unknown }).id ?? "").trim()
      if (!id) throw new Error("at 条目缺少 id（open_id 或 \"all\"）")
      const name = (item as { name?: unknown }).name
      entry = { id, name: name != null && String(name).trim() ? String(name).trim() : undefined }
    } else throw new Error("at 条目须为 open_id 字符串或 {id,name} 对象")
    if (!/^(all|(ou|un|on)_[0-9a-zA-Z]+)$/.test(entry.id)) {
      throw new Error(`无效的 @ 对象 id: ${entry.id}（须为 open_id（ou_/un_/on_ 前缀）或 "all"）`)
    }
    if (!out.some((e) => e.id === entry.id)) out.push(entry)
  }
  return out.length ? out : undefined
}

/** 校验通知通道配置（创建/修改时即拒绝非法配置）。 */
export function validateNotifyChannel(ch: CronNotifyChannel): void {
  if (ch.type !== "webhook" && ch.type !== "feishu" && ch.type !== "feishu_chat") {
    throw new Error(`无效的通知通道类型: ${String(ch.type)}`)
  }
  const target = String(ch.target ?? "").trim()
  if (!target) throw new Error("通知通道缺少 target")
  if (ch.at !== undefined && ch.at.length > 0 && ch.type === "webhook") {
    throw new Error("at @ 人配置仅支持飞书通道（feishu/feishu_chat）")
  }
  if (ch.at !== undefined) normalizeAtList(ch.at)
  if (ch.type === "feishu_chat") {
    if (!/^(oc_[a-f0-9]+|[0-9a-f-]{16,})$/i.test(target)) throw new Error(`无效的飞书 chat_id: ${target}`)
    return
  }
  let url: URL
  try {
    url = new URL(target)
  } catch {
    throw new Error(`无效的通知 URL: ${target}`)
  }
  if (url.protocol !== "http:" && url.protocol !== "https:") throw new Error(`通知 URL 仅支持 http(s): ${target}`)
  if (ch.type === "feishu") {
    if (url.hostname !== "open.feishu.cn" || !/^\/open-apis\/bot\/v2\/hook\//.test(url.pathname)) {
      throw new Error(`飞书群机器人通知地址须为 open.feishu.cn 的 /open-apis/bot/v2/hook/… webhook: ${target}`)
    }
    return
  }
  // 通用 webhook：与事件 Webhook 同规则（回环/链路本地/元数据地址默认拒绝，SSRF 防护）
  checkWebhookUrl(target)
}

/** 飞书自定义机器人加签：sign = base64(HMAC-SHA256(key=`${timestamp}\n${secret}`, message=""))。 */
export function feishuBotSign(timestamp: string, secret: string): string {
  return createHmac("sha256", `${timestamp}\n${secret}`).update("").digest("base64")
}

/** lark_md 内容净化：输出/错误中的尖括号全角化，防任务输出注入 `<at>`/`<a>` 等标签（@ 人/链接仅由配置产生）。 */
function sanitizeLarkMd(text: string): string {
  return text.replace(/</g, "＜")
}

/** @ 人 lark_md 标签（"all" 缺省展示「所有人」；open_id 无 name 时空内文由客户端解析真实姓名）。 */
function atTags(at: FeishuAtTarget[] | undefined): string {
  if (!at?.length) return ""
  return at
    .map((a) => (a.id === "all" ? `<at user_id="all">${a.name ?? "所有人"}</at>` : `<at user_id="${a.id}">${a.name ?? ""}</at>`))
    .join(" ")
}

/** 通知卡片头部模板色（success=绿 / error=红 / timeout=橙 / skipped=灰）。 */
function cardTemplate(n: CronResultNotification): string {
  if (n.ok) return "green"
  if (n.status === "timeout") return "orange"
  if (n.status === "skipped") return "grey"
  return "red"
}

/** 飞书 markdown 卡片（msg_type=interactive）：头部按状态着色，正文 lark_md（**粗体** 标签行 + at 标签 + 输出摘要）。
 *  自定义机器人 webhook（body.card）与应用消息（sendMessage content）共用同一卡片结构。 */
export function buildFeishuCard(n: CronResultNotification, at?: FeishuAtTarget[]): Record<string, unknown> {
  const status = n.ok ? "✅ 成功" : n.status === "skipped" ? "⏭️ 跳过" : n.status === "timeout" ? "⏱️ 超时" : "❌ 失败"
  const lines: string[] = []
  const tags = atTags(at)
  if (tags) lines.push(tags)
  lines.push(`**状态：**${status}　**周期：**${sanitizeLarkMd(n.task.schedule)}`)
  lines.push(`**时间：**${new Date(n.at).toLocaleString("zh-CN")}${n.durationMs !== undefined ? `　**耗时：**${Math.round(n.durationMs / 100) / 10}s` : ""}${n.manual ? "　**（手动触发）**" : ""}`)
  if (n.error) lines.push(`**错误：**${sanitizeLarkMd(n.error.slice(0, NOTIFY_TEXT_MAX))}`)
  if (n.output) lines.push(`**输出：**\n${sanitizeLarkMd(n.output.slice(0, NOTIFY_TEXT_MAX))}`)
  if (n.sessionId) lines.push(`**执行会话：**${n.sessionId}`)
  if (n.disabled) lines.push(`⚠️ **任务已自动停用**${n.manual ? "" : "（如需继续请重新启用）"}`)
  return {
    config: { wide_screen_mode: true },
    header: {
      template: cardTemplate(n),
      title: { tag: "plain_text", content: `⏰ 歌白·定时任务${n.task.name ? `「${sanitizeLarkMd(n.task.name)}」` : `（${n.task.id.slice(0, 8)}）`}` },
    },
    elements: [
      { tag: "div", text: { tag: "lark_md", content: lines.join("\n").slice(0, 4000) } },
      { tag: "note", elements: [{ tag: "plain_text", content: "GEBAI 定时任务 · cron.result" }] },
    ],
  }
}

/** 投递单条通知（尽力而为：失败抛错由调用方记录，不影响任务执行结果）。 */
export async function sendCronNotification(ch: CronNotifyChannel, n: CronResultNotification, deps: NotifyDeps = {}): Promise<void> {
  const now = deps.now ?? Date.now
  if (ch.type === "feishu_chat") {
    if (!deps.feishuSend) throw new Error("飞书应用通知未配置（需 GEBAI_FEISHU_APP_ID/GEBAI_FEISHU_APP_SECRET）")
    await deps.feishuSend(ch.target, buildFeishuCard(n, ch.at))
    return
  }
  const url = String(ch.target).trim()
  const fetchImpl =
    deps.fetchImpl ??
    (async (u: string, init: RequestInit) => {
      const res = await fetchWithRedirectGuard(u, init, checkWebhookUrl)
      return { ok: res.ok, status: res.status }
    })
  let body: Record<string, unknown>
  if (ch.type === "feishu") {
    const payload: Record<string, unknown> = { msg_type: "interactive", card: buildFeishuCard(n, ch.at) }
    if (ch.secret) {
      const ts = Math.floor(now() / 1000).toString()
      payload.timestamp = ts
      payload.sign = feishuBotSign(ts, ch.secret)
    }
    body = payload
  } else {
    body = n as unknown as Record<string, unknown>
  }
  const res = await fetchImpl(url, {
    method: "POST",
    headers: { "Content-Type": "application/json; charset=utf-8" },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(NOTIFY_TIMEOUT_MS),
  })
  if (!res.ok) throw new Error(`通知投递失败: HTTP ${res.status}`)
}
