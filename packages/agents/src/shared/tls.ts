/**
 * 飞书接口 TLS 策略（机器人桥接 + feishu_docs 子 Agent + OAuth 回调共用）：
 * 内网代理（HTTPS 中间人证书不受信任）场景设 GEBAI_FEISHU_INSECURE_TLS=true，
 * 所有飞书出站请求（REST fetch 与长连接 WebSocket）均不校验 TLS 证书。
 */

/** GEBAI_FEISHU_INSECURE_TLS 是否开启（true/1）。 */
export function feishuTlsInsecure(): boolean {
  const v = process.env.GEBAI_FEISHU_INSECURE_TLS
  return v === "true" || v === "1"
}

/** 飞书 fetch 实现：开启时不校验 TLS 证书（注入 tls.rejectUnauthorized=false）。 */
export const feishuFetch = ((input: string | URL | Request, init?: RequestInit): Promise<Response> => {
  if (!feishuTlsInsecure()) return fetch(input, init)
  return fetch(input, { ...init, tls: { rejectUnauthorized: false } })
}) as typeof fetch

/** 飞书长连接 WebSocket 附加选项：开启时禁用证书校验（Bun WebSocket 客户端）。 */
export function feishuWsOptions(): { tls: { rejectUnauthorized: boolean } } | undefined {
  return feishuTlsInsecure() ? { tls: { rejectUnauthorized: false } } : undefined
}
