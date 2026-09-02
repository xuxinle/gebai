/** fetch 防护（自 core/tools.ts 抽取 + webhooks.ts 的 checkWebhookUrl 迁入）：重定向逐跳 SSRF 校验，
 *  回环/链路本地/私网拒绝（DNS 解析复查防内网域名）。core 层公共依赖，勿反向依赖顶层模块。 */
import { lookup } from "node:dns/promises"
import { hostBlockReason } from "./ip"

/** 重定向跳数上限（防重定向循环与超长跳板链）。 */
const REDIRECT_MAX_HOPS = 5

/**
 * 带逐跳校验的 fetch（服务端部署模式 SSRF 防护）：`redirect: "manual"` 拿到每跳
 * 3xx 响应后重新校验 Location 目标（相对地址按当前 URL 解析），全部通过才继续；
 * 跳数超限或 Location 非法时报错。`guard` 由调用方按沙箱开关注入（本地模式放行）。
 */
export async function fetchWithRedirectGuard(
  rawUrl: string,
  init: RequestInit,
  guard: (url: string) => void | Promise<void>,
): Promise<Response> {
  let url = rawUrl
  for (let hop = 0; ; hop++) {
    await guard(url)
    const res = await fetch(url, { ...init, redirect: "manual" })
    const status = res.status
    if (status >= 300 && status < 400) {
      const loc = res.headers.get("location")
      if (!loc) return res
      if (hop >= REDIRECT_MAX_HOPS) {
        res.body?.cancel().catch(() => {})
        throw new Error(`重定向次数超限（>${REDIRECT_MAX_HOPS}）: ${rawUrl}`)
      }
      try {
        url = new URL(loc, url).toString()
      } catch {
        res.body?.cancel().catch(() => {})
        throw new Error(`无效的重定向地址: ${loc}`)
      }
      res.body?.cancel().catch(() => {})
      continue
    }
    return res
  }
}
/** 服务端部署模式的 URL 安全校验：拒绝回环/链路本地/私网（防 SSRF）。
 * 主机名判定统一走 `core/ip.ts`（覆盖 IPv4-mapped IPv6、ULA、尾点 FQDN、规范化的
 * 整数/十六进制 IPv4 等绕过形式）；**域名做 DNS 解析复查**——字面量之外，「解析到私网/回环的域名」
 * （内网 DNS 名 kubernetes.default.svc、A 记录指向内网的攻击域名）同样拒绝，解析失败放行
 * （fetch 自会失败）；逐跳校验防重定向跳板（解析时刻与 fetch 时刻仍有窗口，纵深依赖出口网络一致性）。 */
export async function assertPublicHttpUrl(raw: string): Promise<void> {
  let url: URL
  try {
    url = new URL(raw)
  } catch {
    throw new Error(`无效的 URL: ${raw}`)
  }
  if (url.protocol !== "http:" && url.protocol !== "https:") throw new Error(`无效的 URL（仅支持 http/https）: ${raw}`)
  const hostname = url.hostname.replace(/\.$/, "") // 尾点 FQDN 归一（ip.ts 同款）
  const reason = hostBlockReason(hostname, { blockPrivate: true })
  if (reason === "私网地址") throw new Error(`URL 不允许（私网地址）: ${raw}`)
  if (reason) throw new Error(`URL 不允许（回环/链路本地地址）: ${raw}`)
  // DNS 解析复查（3s 超时放行——挂起的解析器不应额外阻断，fetch 层自有超时）
  let addrs: Array<{ address: string }>
  try {
    addrs = await Promise.race([
      lookup(hostname, { all: true }),
      new Promise<never>((_, reject) => setTimeout(() => reject(new Error("dns-timeout")), 3000)),
    ])
  } catch {
    return
  }
  for (const a of addrs) {
    const r = hostBlockReason(a.address, { blockPrivate: true })
    if (r) throw new Error(`URL 不允许（域名解析到${r === "私网地址" ? "私网" : "回环/链路本地"}地址 ${a.address}）: ${raw}`)
  }
}

/** webhook URL 私网放行开关（自 webhooks.ts 迁入；本地开发内网回调常见场景默认放行私网，全拒须网关管控）。 */
const ALLOW_PRIVATE = process.env.GEBAI_WEBHOOK_ALLOW_PRIVATE === "true"

/** webhook URL 校验（注册/投递共用；自 webhooks.ts 迁入，消除 core/notify → 顶层 webhooks 越层依赖）。 */
export function checkWebhookUrl(raw: string): void {
  let url: URL
  try {
    url = new URL(raw)
  } catch {
    throw new Error(`invalid webhook url: ${raw}`)
  }
  if (url.protocol !== "http:" && url.protocol !== "https:") throw new Error(`invalid webhook url: ${raw}`)
  if (ALLOW_PRIVATE) return
  const reason = hostBlockReason(url.hostname, { blockPrivate: false })
  if (reason) throw new Error(`webhook url not allowed: ${raw}`)
}
