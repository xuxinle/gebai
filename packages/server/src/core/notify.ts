import { createHmac } from "node:crypto"
import { checkWebhookUrl } from "../webhooks"
import { fetchWithRedirectGuard } from "./tools"

/** 通知正文在通道消息中的保留长度（飞书 text 消息建议 ≤ 4000 字符）。 */
export const NOTIFY_TEXT_MAX = 2000
/** 通知投递超时。 */
export const NOTIFY_TIMEOUT_MS = 10_000

/** 通知通道类型：webhook=通用 HTTP 回调；feishu=飞书群自定义机器人 webhook；feishu_chat=飞书应用消息（chat_id）。 */
export type CronNotifyType = "webhook" | "feishu" | "feishu_chat"

/** 定时任务通知通道（任务内嵌配置，可配多条）。 */
export interface CronNotifyChannel {
  type: CronNotifyType
  /** webhook/feishu：http(s) URL（feishu 须为 open.feishu.cn 群机器人 webhook）；feishu_chat：群 chat_id。 */
  target: string
  /** feishu 加签密钥（可选，群机器人安全设置「签名校验」）。 */
  secret?: string
}

/** 定时任务运行结果通知载荷（webhook 通道 JSON 原样投递，飞书通道格式化为文本）。 */
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
  feishuSend?: (chatId: string, text: string) => Promise<void>
  /** 时钟（加签时间戳用，可注入）。 */
  now?: () => number
}

/** 校验通知通道配置（创建/修改时即拒绝非法配置）。 */
export function validateNotifyChannel(ch: CronNotifyChannel): void {
  if (ch.type !== "webhook" && ch.type !== "feishu" && ch.type !== "feishu_chat") {
    throw new Error(`无效的通知通道类型: ${String(ch.type)}`)
  }
  const target = String(ch.target ?? "").trim()
  if (!target) throw new Error("通知通道缺少 target")
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

/** 通知正文（人类可读多行文本，飞书 text 消息与日志共用）。 */
export function formatNotificationText(n: CronResultNotification): string {
  const name = n.task.name ? `「${n.task.name}」` : n.task.id
  const icon = n.ok ? "✅" : n.status === "skipped" ? "⏭️" : "❌"
  const status = n.ok ? "成功" : n.status === "skipped" ? "跳过" : n.status === "timeout" ? "超时" : "失败"
  const lines = [`${icon} [歌白·定时任务] ${name}`, `状态: ${status}  周期: ${n.task.schedule}`, `时间: ${new Date(n.at).toLocaleString("zh-CN")}`]
  if (n.durationMs !== undefined) lines.push(`耗时: ${Math.round(n.durationMs / 100) / 10}s`)
  if (n.error) lines.push(`错误: ${n.error.slice(0, NOTIFY_TEXT_MAX)}`)
  if (n.output) lines.push(`输出:\n${n.output.slice(0, NOTIFY_TEXT_MAX)}`)
  if (n.disabled) lines.push("⚠️ 任务已自动停用" + (n.manual ? "" : "（如需继续请重新启用）"))
  return lines.join("\n").slice(0, 4000)
}

/** 投递单条通知（尽力而为：失败抛错由调用方记录，不影响任务执行结果）。 */
export async function sendCronNotification(ch: CronNotifyChannel, n: CronResultNotification, deps: NotifyDeps = {}): Promise<void> {
  const now = deps.now ?? Date.now
  if (ch.type === "feishu_chat") {
    if (!deps.feishuSend) throw new Error("飞书应用通知未配置（需 GEBAI_FEISHU_APP_ID/GEBAI_FEISHU_APP_SECRET）")
    await deps.feishuSend(ch.target, formatNotificationText(n))
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
    const payload: Record<string, unknown> = { msg_type: "text", content: { text: formatNotificationText(n) } }
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
