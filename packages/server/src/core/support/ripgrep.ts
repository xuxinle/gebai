/**
 * 内置 ripgrep（rg）定位与执行——grep/glob 工具与 fs 内容搜索共用。
 *
 * 背景：内置遍历引擎要把候选文件**全文读进内存**再匹配（gebai 仓库根目录搜索实测 107MB / 16s），
 * rg 同查询 <100ms。rg 来源只有两个真实渠道，按下列顺序解析（首个可用者胜出）：
 *
 *  1. `GEBAI_RG_PATH` —— 显式覆盖（运维/内网自备/调试）
 *  2. `{GEBAI_HOME}/vendor/ripgrep/<平台>/rg[.exe]` —— 二进制（`bun --compile`）形态：运行时物化点，
 *     内容来自构建期内嵌产物（`rg.embedded.generated.json`，scripts/build-rg-embed.ts 生成）——
 *     单二进制形态在用户机器上既无 node_modules 也不保证装了 rg，故随产物内嵌（与 d2js /
 *     playwright driver / CV 模型同一套「内嵌 + 物化」闭环）。**源码/dev 形态不走此路**（无需物化）
 *  3. node_modules 的 `@vscode/ripgrep` —— npm 依赖形态（`optionalDependencies`）：实现上问主包要它自己
 *     导出的 `rgPath`（1.18+ 二进制在平台子包 `@vscode/ripgrep-<platform>-<arch>/bin/rg[.exe]`，
 *     ≤1.15.x 在主包 `bin/` 下）；现版经 npm registry 分发，拉不到不影响 `bun install`（optional 语义）
 *  4. 系统 PATH 上的 rg（`Bun.which`）
 *
 * **不依赖 models/ 资源子仓库**（刻意不存第二份二进制副本：来源收敛为「npm 包」与「系统」两条，
 * npm 包已覆盖跨平台分发与版本管理，自存副本徒增体积与失同步风险）。
 * 全部缺失时返回 null：调用方回退内置遍历引擎，**功能不降级、只降速**。
 */
import { chmodSync, existsSync, mkdirSync, statSync, writeFileSync } from "node:fs"
import { dirname, join } from "node:path"
import { pathToFileURL } from "node:url"
import { isBinaryMode, resolveGebaiHome } from "../base/config"

/** 平台目录名（与构建脚本产出约定一致）。 */
export function rgPlatformDir(): string {
  return `${process.platform}-${process.arch}`
}

/** rg 可执行体文件名（Windows 带 .exe）。 */
export function rgBinaryName(): string {
  return process.platform === "win32" ? "rg.exe" : "rg"
}

/** 内嵌产物结构（scripts/build-rg-embed.ts 生成，gitignore；缺失即视为未内嵌）。 */
interface RgEmbedded {
  version: string
  platform: string
  /** 单平台 rg 可执行体的 gzip base64。 */
  data: string
}

/** 是否为可执行文件（存在 + 是文件 + POSIX 下带执行位）。 */
function isExecutableFile(p: string): boolean {
  try {
    const st = statSync(p)
    if (!st.isFile()) return false
    if (process.platform === "win32") return true
    return (st.mode & 0o111) !== 0
  } catch {
    return false
  }
}

/**
 * 二进制形态：把内嵌 rg 物化到 `{GEBAI_HOME}/vendor/ripgrep/<平台>/`（幂等——已存在即直接用）。
 * 非二进制形态或无内嵌产物返回 null（走源码/资源仓库路径）。
 */
async function materializeEmbeddedRg(): Promise<string | null> {
  if (!isBinaryMode()) return null
  const dir = join(resolveGebaiHome(), "vendor", "ripgrep", rgPlatformDir())
  const file = join(dir, rgBinaryName())
  if (existsSync(file)) return file
  // 生成产物可能不存在（未跑构建脚本）：动态 import 失败即视为未内嵌
  const embedded = await import("../rg.embedded.generated.json")
    .then((m) => m.default as RgEmbedded)
    .catch(() => null)
  // 内嵌产物只含构建平台的可执行体（bun --compile 亦为平台相关产物），平台不符则不物化
  if (!embedded?.data || embedded.platform !== rgPlatformDir()) return null
  mkdirSync(dir, { recursive: true })
  writeFileSync(file, Bun.gunzipSync(Buffer.from(embedded.data, "base64")))
  if (process.platform !== "win32") chmodSync(file, 0o755)
  return file
}

/** 解析 npm 包根目录：先试 `package.json` 子路径（部分包含 exports 限制会失败），
 *  退回包入口后向上找含 package.json 的目录（入口常在 lib/ 下）。未安装返回 null。 */
function resolvePackageDir(name: string): string | null {
  const dirs: string[] = []
  try {
    dirs.push(dirname(Bun.resolveSync(`${name}/package.json`, import.meta.dirname)))
  } catch {
    /* exports 未导出 package.json 子路径 */
  }
  try {
    let cur = dirname(Bun.resolveSync(name, import.meta.dirname))
    for (let i = 0; i < 3; i++) {
      if (existsSync(join(cur, "package.json"))) break
      cur = dirname(cur)
    }
    dirs.push(cur)
  } catch {
    /* 未安装该包 */
  }
  return dirs.find((d) => existsSync(join(d, "package.json"))) ?? null
}

/** npm 依赖形态路径（`@vscode/ripgrep`）；未装返回 null。
 *  两代布局都要认：1.18+ 二进制在**平台子包**（`@vscode/ripgrep-<platform>-<arch>/bin/rg[.exe]`），
 *  ≤1.15.x 由 postinstall 下载到**主包** `bin/` 下。
 *  首选「问主包要它自己导出的 `rgPath`」——跨包管理器布局（bun 的 `.bun` store / pnpm 嵌套）由包自身
 *  保证；自拼平台包路径在 bun workspace 下会解析不到（平台子包位于主包同级 node_modules，只对主包可见）。 */
export async function npmRipgrepPath(): Promise<string | null> {
  const pkgDir = resolvePackageDir("@vscode/ripgrep")
  if (!pkgDir) return null
  try {
    const mod = (await import(pathToFileURL(join(pkgDir, "lib", "index.js")).href)) as { rgPath?: string }
    if (typeof mod.rgPath === "string" && isExecutableFile(mod.rgPath)) return mod.rgPath
  } catch {
    /* 导入失败（旧版结构 / 环境限制）→ 退回目录推导 */
  }
  const arch = process.env.npm_config_arch || process.arch
  const fallbacks = [
    // 1.18+：平台子包与主包同级（scope 目录下）
    join(dirname(pkgDir), `ripgrep-${process.platform}-${arch}`, "bin", rgBinaryName()),
    // ≤1.15.x：postinstall 下载到主包 bin/
    join(pkgDir, "bin", rgBinaryName()),
  ]
  return fallbacks.find((p) => isExecutableFile(p)) ?? null
}

/** 未缓存解析：按解析链取首个可用项。 */
async function resolveUncached(): Promise<string | null> {
  const explicit = process.env.GEBAI_RG_PATH?.trim()
  if (explicit) return isExecutableFile(explicit) ? explicit : null
  const materialized = await materializeEmbeddedRg().catch(() => null)
  if (materialized) return materialized
  const npm = await npmRipgrepPath()
  if (npm) return npm
  // 系统 PATH 兜底（既有部署可能已装：不再是必需项，但装了就直接用）
  return Bun.which("rg") ?? null
}

/** 解析结果缓存（null=尚未解析；解析后 path 为 null 表示不可用）。 */
let cachedRg: { path: string | null } | null = null

/** 解析内置 rg 可执行体绝对路径；不可用返回 null（调用方回退内置引擎）。首次解析后缓存。 */
export async function resolveRipgrep(): Promise<string | null> {
  if (!cachedRg) cachedRg = { path: await resolveUncached() }
  return cachedRg.path
}

/** rg 是否可用（不抛错）。 */
export async function ripgrepAvailable(): Promise<boolean> {
  return (await resolveRipgrep()) !== null
}

/** 清空解析缓存（测试用：改 GEBAI_RG_PATH 后需重新解析）。 */
export function resetRipgrepCache(): void {
  cachedRg = null
}

/**
 * grep 引擎偏好（`GEBAI_GREP_ENGINE`）：`auto`（默认）/ `rg` / `builtin`。
 * - `rg`：强制 rg（不可用时由调用方给出明确报错，便于部署期发现内置缺失）
 * - `builtin`：强制内置遍历引擎（不依赖 rg 的逃生口，含性能回归排查）
 */
export function grepEnginePreference(): "auto" | "rg" | "builtin" {
  const v = (process.env.GEBAI_GREP_ENGINE ?? "").trim().toLowerCase()
  return v === "rg" || v === "builtin" ? v : "auto"
}

/** rg 执行结果。 */
export interface RipgrepRun {
  /** 退出码（rg 语义：0=有命中 1=无命中 2=错误）；启动失败/超时/主动停止为 null。 */
  code: number | null
  /** 失败原因（不可用/启动失败/超时/读流异常）——非空即视为引擎不可用，调用方应回退。 */
  error?: string
  /** 因 `onLine` 返回 false 主动停止（已达结果上限，非错误）。 */
  stopped: boolean
}

export interface RipgrepRunOpts {
  /** 工作目录（rg 输出的相对路径以此为准）。 */
  cwd: string
  /** rg 参数（不含可执行体本身）。 */
  args: string[]
  /** 逐行消费 stdout；返回 false 表示已够用——立即杀掉 rg 提前收工（对应 §结果上限）。 */
  onLine?: (line: string) => boolean | void
  /** 超时（毫秒，默认 20s）：超时强杀并返回 error（防病态输入拖垮会话）。 */
  timeoutMs?: number
  /** 外部取消信号（会话/任务取消）。 */
  signal?: AbortSignal
}

const RG_TIMEOUT_MS = 20_000

/**
 * 执行 rg（异步 spawn，不阻塞事件循环；超时/取消按进程树强杀）。
 * 只做进程编排与逐行回传，参数拼装与结果解释留在调用方（grep 工具/fs 服务各自需要不同的输出格式）。
 */
export async function runRipgrep(rgPath: string, opts: RipgrepRunOpts): Promise<RipgrepRun> {
  const timeoutMs = opts.timeoutMs ?? RG_TIMEOUT_MS
  const isWin = process.platform === "win32"
  let child: ReturnType<typeof Bun.spawn>
  try {
    child = Bun.spawn([rgPath, ...opts.args], { cwd: opts.cwd, stdout: "pipe", stderr: "pipe", stdin: "ignore" })
  } catch (e) {
    return { code: null, error: `rg 启动失败：${e instanceof Error ? e.message : String(e)}`, stopped: false }
  }
  let stopped = false
  let timedOut = false
  let aborted = false
  const kill = () => {
    try {
      if (isWin) Bun.spawn(["taskkill", "/pid", String(child.pid), "/T", "/F"], { stdout: "ignore", stderr: "ignore" })
      else child.kill("SIGKILL")
    } catch {
      try {
        child.kill("SIGKILL")
      } catch {
        /* 已退出 */
      }
    }
  }
  const timer = setTimeout(() => {
    timedOut = true
    kill()
  }, timeoutMs)
  const onAbort = () => {
    aborted = true
    kill()
  }
  opts.signal?.addEventListener("abort", onAbort, { once: true })
  try {
    // 逐行消费（行分隔 JSON / 文本输出均为行式）：onLine 返回 false 即提前收工
    const decoder = new TextDecoder()
    let buf = ""
    const reader = (child.stdout as ReadableStream<Uint8Array>).getReader()
    let wantStop = false
    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      buf += decoder.decode(value, { stream: true })
      let nl = buf.indexOf("\n")
      while (nl >= 0) {
        const line = buf.slice(0, nl)
        buf = buf.slice(nl + 1)
        if (opts.onLine && opts.onLine(line) === false) {
          wantStop = true
          break
        }
        nl = buf.indexOf("\n")
      }
      if (wantStop) break
    }
    if (wantStop) {
      stopped = true
      kill()
    } else if (buf && opts.onLine) {
      // 末行无换行符（rg 正常以换行结尾，防御性处理）
      if (opts.onLine(buf) === false) {
        stopped = true
        kill()
      }
    }
    const code = await child.exited.catch(() => null)
    if (timedOut) return { code: null, error: `rg 执行超时（> ${Math.round(timeoutMs / 1000)}s）`, stopped }
    if (aborted) return { code: null, error: "rg 执行已取消", stopped }
    return { code, stopped }
  } catch (e) {
    kill()
    return { code: null, error: `rg 执行异常：${e instanceof Error ? e.message : String(e)}`, stopped }
  } finally {
    clearTimeout(timer)
    opts.signal?.removeEventListener("abort", onAbort)
  }
}
