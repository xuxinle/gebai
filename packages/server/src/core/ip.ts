import { isIP } from "node:net"

/**
 * 主机名/IP 判定工具（SSRF 防护共用）：fetch_url/http_request（全私网拒绝）与
 * webhook 注册校验（回环/链路本地/云元数据拒绝）复用同一套主机名分类逻辑，
 * 避免两处实现漂移遗漏绕过形式。
 *
 * 已覆盖的绕过形式：
 * - 整数/十六进制/八进制 IPv4（`2130706433`/`0x7f000001`/`0177.0.0.1`）——WHATWG URL
 *   解析后 hostname 已规范化为点分十进制，按规范地址判定即可
 * - IPv4-mapped IPv6（`[::ffff:127.0.0.1]`、`[::ffff:7f00:1]`、完整形式）——展开后复检内嵌 IPv4
 * - IPv4-compatible IPv6（`[::127.0.0.1]`，已弃用但部分实现仍解析）
 * - IPv6 压缩/完整形式回环（`[::1]`/`[0:0:0:0:0:0:0:1]`）与链路本地（`fe80::/10`）
 * - ULA 私网（`fc00::/7`，仅全私网拒绝模式）
 * - 尾点 FQDN（`localhost.`）——去除尾点后判定
 * - 域名（非 IP 字面量）的 DNS 重绑定无法静态防护，由调用方重定向守卫/超时兜底
 */

/** 去除 IPv6 括号与尾点，小写归一化；返回可判定的主机名。 */
export function normalizeHost(raw: string): string {
  return raw.toLowerCase().replace(/^\[|\]$/g, "").replace(/\.$/, "")
}

/** 将 IPv6 地址展开为 8 组十六进制（`::` 压缩还原）；非 IPv6 形式返回空数组。 */
export function expandIPv6(host: string): string[] {
  if (!host.includes(":")) return []
  const parts = host.split("::")
  const left = parts[0] ? parts[0].split(":") : []
  const right = parts[1] ? parts[1].split(":") : []
  const missing = 8 - left.length - right.length
  return [...left, ...Array(Math.max(0, missing)).fill("0"), ...right]
}

/** 点分十进制 IPv4 → 四段数字；非法返回 null。 */
export function parseIPv4(host: string): number[] | null {
  const parts = host.split(".")
  if (parts.length !== 4) return null
  const nums = parts.map((p) => Number(p))
  if (nums.some((n) => !Number.isInteger(n) || n < 0 || n > 255)) return null
  return nums
}

/** 回环/链路本地/未指定地址判定（IPv4）。 */
function isLoopbackV4(p: number[]): boolean {
  return p[0] === 127 || (p[0] === 169 && p[1] === 254) || (p[0] === 0 && p[1] === 0 && p[2] === 0 && p[3] === 0)
}

/** 私网（RFC1918）判定（IPv4）。 */
function isPrivateV4(p: number[]): boolean {
  return p[0] === 10 || (p[0] === 172 && p[1] >= 16 && p[1] <= 31) || (p[0] === 192 && p[1] === 168)
}

/** 主机名分类结果：blockPrivate=false 时私网地址不拦截（webhook 内网回调场景）。 */
export function hostBlockReason(host: string, opts: { blockPrivate: boolean }): string | null {
  const h = normalizeHost(host)
  if (!h) return "无效的主机名"
  if (h === "localhost" || h === "0.0.0.0") return "回环/链路本地地址"
  const ip = isIP(h)
  if (ip === 4) {
    const p = parseIPv4(h)
    if (!p) return null
    if (isLoopbackV4(p)) return "回环/链路本地地址"
    if (isPrivateV4(p)) return opts.blockPrivate ? "私网地址" : null
    return null
  }
  if (ip === 6) {
    const groups = expandIPv6(h)
    if (groups.length !== 8) return null
    const nums = groups.map((g) => parseInt(g || "0", 16))
    if (nums.some((n) => Number.isNaN(n))) return null
    // 未指定 :: 与回环 ::1（任意表示形式）
    if (nums.every((n) => n === 0)) return "回环/链路本地地址"
    if (nums.slice(0, 7).every((n) => n === 0) && nums[7] === 1) return "回环/链路本地地址"
    // 链路本地 fe80::/10
    if ((nums[0] & 0xffc0) === 0xfe80) return "回环/链路本地地址"
    // IPv4-mapped ::ffff:a.b.c.d（含完整形式）与 IPv4-compatible ::a.b.c.d：复检内嵌 IPv4
    const mapped = nums.slice(0, 5).every((n) => n === 0) && nums[5] === 0xffff
    const compatible = nums.slice(0, 6).every((n) => n === 0) && nums[6] !== 0
    if (mapped || compatible) {
      const v4 = `${nums[6] >> 8}.${nums[6] & 0xff}.${nums[7] >> 8}.${nums[7] & 0xff}`
      const p = parseIPv4(v4)
      if (p && isLoopbackV4(p)) return "回环/链路本地地址"
      if (p && isPrivateV4(p)) return opts.blockPrivate ? "私网地址" : null
      return null
    }
    // ULA fc00::/7（私网）：仅全私网拒绝模式拦截
    if (opts.blockPrivate && (nums[0] & 0xfe00) === 0xfc00) return "私网地址"
    return null
  }
  return null // 域名：DNS 重绑定无法静态防护
}
