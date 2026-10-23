/**
 * 企业内网 TLS 拦截的根证书发现（Windows 证书库 → 进程可信根）。
 *
 * ## 为什么需要这件事
 *
 * Bun/Node 的 fetch **不读 Windows 证书库**，只认自带的 Mozilla 根证书集。
 * 而很多企业网里有 TLS 出口检查（本机实测：飞书 `accounts.feishu.cn` 的证书
 * 链被换成字节内部的 `TENANT-RESTRICTION-CA` 签发），浏览器因为读系统库所以
 * 能正常访问，**服务端进程却报 `unable to verify the first certificate`**。
 *
 * 表现极具误导性：
 * - 浏览器里点授权完全正常（授权页能开、能扫码）
 * - 回调到了服务端才失败（兑换令牌那一步是服务端发的 HTTPS）
 * - 错误文案与"证书坏了/被攻击"很像，很难联想到"进程看不到系统根"
 *
 * ## 做法
 *
 * 启动时把系统根证书库里的非 Mozilla 内置根（企业自签的那批）导出成一个
 * PEM bundle，供 `NODE_EXTRA_CA_CERTS` 使用。
 *
 * **只在检测到问题时才动作**：先拿实际要访问的域探一次，能通就什么都不做
 *（绝大多数环境没有拦截，不该为此付出启动期导出几十张证书的代价）。
 *
 * ## 为什么不是"让浏览器代发那次请求"
 *
 * `oauth/v3/token` 的请求体里有 `client_secret`。把请求体交给浏览器等于把
 * 应用密钥交给页面——**任何能读页面的人都拿到**。所以只能在服务端修好
 * TLS 信任，不能把这一步外包出去。
 */

import { execFileSync } from "node:child_process"
import { existsSync, mkdirSync, writeFileSync } from "node:fs"
import { join } from "node:path"

export interface TrustFixOptions {
  /** 歌白的 home（证书 bundle 落在这里）。 */
  home: string
  /** 用来探测"是否真的需要修"的 URL（缺省飞书账号域）。 */
  probeUrl?: string
  log?: (level: "info" | "warn", msg: string) => void
  /** 探测超时（毫秒）。 */
  timeoutMs?: number
}

export interface TrustFixResult {
  /** 是否需要（并已）导出企业根证书。 */
  applied: boolean
  /** bundle 路径（applied=false 时无意义）。 */
  bundlePath?: string
  /** 导出到多少张证书。 */
  count?: number
  /** 人类可读说明。 */
  message: string
}

const DEFAULT_PROBE = "https://accounts.feishu.cn/open-apis/authen/v1/authorize?client_id=probe"

/** 探测某个 URL 能否被当前进程正常 TLS 校验。 */
async function canReach(url: string, timeoutMs: number): Promise<{ ok: boolean; error?: string }> {
  try {
    const ctl = new AbortController()
    const timer = setTimeout(() => ctl.abort(), timeoutMs)
    // redirect: manual —— 只要 TLS 握手成功就够了，不关心跳到哪
    const res = await fetch(url, { redirect: "manual", signal: ctl.signal })
    clearTimeout(timer)
    void res
    return { ok: true }
  } catch (err) {
    const e = err as Error & { cause?: { code?: string; message?: string } }
    const detail = e.cause?.code ?? e.cause?.message ?? e.message
    return { ok: false, error: String(detail) }
  }
}

/**
 * 从 Windows 证书库导出企业根证书（非内置的那批）。
 *
 * 只走 PowerShell 的 `Cert:\` 提供程序——它同时覆盖
 * `LocalMachine\Root` 与 `CurrentUser\Root`，且不需要额外依赖。
 */
function exportWindowsRoots(): { pems: string[]; subjects: string[] } {
  // 只挑"根"（Root 存储里的就是根），不做额外过滤：
  // 多带几张无害（它们都是本机已信任的），漏掉才致命。
  const script = [
    "$out = @()",
    "foreach ($store in @('Cert:\\LocalMachine\\Root','Cert:\\CurrentUser\\Root')) {",
    "  if (-not (Test-Path $store)) { continue }",
    "  foreach ($c in Get-ChildItem $store -ErrorAction SilentlyContinue) {",
    "    if (-not $c.RawData) { continue }",
    "    $b64 = [Convert]::ToBase64String($c.RawData, 'InsertLineBreaks')",
    "    $out += \"-----BEGIN CERTIFICATE-----`n$b64`n-----END CERTIFICATE-----\"",
    "    $out += \"#SUBJECT#\" + $c.Subject",
    "  }",
    "}",
    "$out -join \"`n\"",
  ].join("\n")

  const stdout = execFileSync(
    "powershell.exe",
    ["-NoProfile", "-NonInteractive", "-Command", script],
    { encoding: "utf-8", timeout: 60_000, windowsHide: true, maxBuffer: 32 * 1024 * 1024 },
  )

  const pems: string[] = []
  const subjects: string[] = []
  const lines = stdout.split(/\r?\n/)
  let buf: string[] = []
  for (const line of lines) {
    const t = line.trim()
    if (t.startsWith("#SUBJECT#")) {
      subjects.push(t.slice("#SUBJECT#".length))
      continue
    }
    if (t === "-----BEGIN CERTIFICATE-----") {
      buf = [t]
      continue
    }
    if (t === "-----END CERTIFICATE-----") {
      buf.push(t)
      pems.push(buf.join("\n"))
      buf = []
      continue
    }
    if (buf.length > 0 && t) buf.push(t)
  }
  return { pems, subjects }
}

/**
 * 需要时把企业根证书导出为 PEM bundle，并**就地注入当前进程**。
 *
 * 注入方式说明：`NODE_EXTRA_CA_CERTS` 只在**进程启动时**被读取，运行期设置
 * 对已创建的 TLS 上下文无效。所以这里做两件事：
 *
 * 1. 写文件并设环境变量（**下次启动**自动生效，无需再次导出）
 * 2. 对 Bun：`NODE_TLS_REJECT_UNAUTHORIZED=0` 是本模块**不做**的全局降级；
 *    真正让"当次进程"生效靠调用方在启动早期调用本函数（bun 在首次 TLS
 *    之前读环境变量）。
 *
 * 因此**必须在任何 HTTPS 请求之前调用**（本项目的调用点在 compose 起步处）。
 */
export async function ensureCorporateTrust(opts: TrustFixOptions): Promise<TrustFixResult> {
  const probeUrl = opts.probeUrl ?? DEFAULT_PROBE
  const timeoutMs = opts.timeoutMs ?? 8000
  const log = opts.log ?? (() => {})

  // 已经能用 → 什么都不做（绝大多数环境走这里）
  const probe = await canReach(probeUrl, timeoutMs)
  if (probe.ok) {
    return { applied: false, message: "TLS 校验正常，无需导入企业根证书" }
  }

  // 之前已经导出过 → 直接复用（并提示需要重启才生效）
  const dir = join(opts.home, "trust")
  const bundlePath = join(dir, "corporate-roots.pem")
  if (existsSync(bundlePath)) {
    process.env.NODE_EXTRA_CA_CERTS = bundlePath
    return {
      applied: true,
      bundlePath,
      message:
        `TLS 校验失败（${probe.error ?? "未知"}），已找到既有企业根证书包：${bundlePath}。\n` +
        `若本次仍失败，需重启进程（NODE_EXTRA_CA_CERTS 只在启动时读取）。`,
    }
  }

  if (process.platform !== "win32") {
    return {
      applied: false,
      message:
        `TLS 校验失败（${probe.error ?? "未知"}）。当前平台为 ${process.platform}，` +
        `本模块只实现 Windows 证书库导出；请手动设置 SSL_CERT_FILE 指向企业根证书。`,
    }
  }

  log("info", `[trust] TLS 校验失败（${probe.error ?? "未知"}），尝试从 Windows 证书库导出根证书…`)
  let exported: { pems: string[]; subjects: string[] }
  try {
    exported = exportWindowsRoots()
  } catch (err) {
    return {
      applied: false,
      message: `TLS 校验失败，且导出 Windows 根证书时出错：${String((err as Error)?.message || err)}`,
    }
  }

  if (exported.pems.length === 0) {
    return { applied: false, message: "TLS 校验失败，但未能从 Windows 证书库导出任何证书" }
  }

  mkdirSync(dir, { recursive: true })
  const header =
    "# 由歌白自动导出（Windows 根证书库）——用于通过企业内网 TLS 出口检查的校验\n" +
    `# 导出时间：${new Date().toISOString()}\n` +
    `# 证书数：${exported.pems.length}\n`
  writeFileSync(bundlePath, `${header}${exported.pems.join("\n")}\n`, "utf-8")
  process.env.NODE_EXTRA_CA_CERTS = bundlePath

  // 记录这些根里有没有"企业自签"的（Subject 里带公司名的那些），便于排查
  const corp = exported.subjects.filter((s) => /TENANT-RESTRICTION|BYTEDANCE|O=.*CORP|INTERNAL/i.test(s))
  log(
    "info",
    `[trust] 已导出 ${exported.pems.length} 张根证书到 ${bundlePath}` +
      (corp.length > 0 ? `（含企业自签：${corp.slice(0, 3).join(" / ")}）` : ""),
  )

  return {
    applied: true,
    bundlePath,
    count: exported.pems.length,
    message:
      `TLS 校验失败（${probe.error ?? "未知"}），已从 Windows 证书库导出 ${exported.pems.length} 张根证书到：\n${bundlePath}\n` +
      `环境变量 NODE_EXTRA_CA_CERTS 已指向它；**需重启进程后生效**（该变量只在启动时读取）。`,
  }
}
