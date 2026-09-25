/**
 * local_infer 的**源码发现层**：解决「内网/离线环境下拿不到预编译包」——把源码放进 `resources/src/`
 * （或既有约定目录），由本层发现、识别、探测工具链、按需解压，交给 sourcebuild.ts 就地编译成引擎。
 *
 * 与 engines.ts 的分工：
 *   * engines.ts   —— 从发行版**下载**预编译引擎（需要外网）；
 *   * sourcefind.ts —— 在**本机**发现可编译源码（内网可用，零外网）；
 *   * sourcebuild.ts —— 把发现的源码编译成引擎并安装到 `vendor/<engine-id>/`（与下载路径同一落点、
 *     同一标记文件格式，因此 launcher/status/start 无需区分来源）。
 *
 * 发现根（按优先级）：
 *   1. `LOCAL_INFER_SOURCE_DIRS` 环境变量（路径分隔符分隔，`:` 与 `;` 都认，便于跨平台配置）；
 *   2. `{GEBAI_HOME}/resources/src/`（**内网预置源码的主目录**：把源码目录或源码归档放这里）；
 *   3. `{GEBAI_HOME}/resources/engines/`；
 *   4. `<infer>/engine/`（仓库既有约定，`engine/llama.cpp-*` 在这里）。
 *
 * 实现约定（与 engines.ts 同一套自我约束）：
 *   * **不抛错边界**：任何一个根/条目/外部命令失败都降级为 `notes` 或 `error` 字段，绝不向上扔异常——
 *     模型侧拿到的必须是「下一步怎么办」而不是异常栈。
 *   * **不整读大文件**：CMakeLists/README 只读前 64KB 判定关键字；目录大小统计有文件数/深度上限；
 *     归档只 **peek**（列条目）不整体解压，zip 预览前先看体积，tar 预览走 `tar -tzf | head`。
 *   * **外部命令双路径**：有 ctx 时走 `ctx.runCommand`（宿主统一审记/超时）；无 ctx 时走 `node:child_process`
 *     的 `execFile`——两条路径都不抛错，命令缺失与命令失败在 notes 里说法不同。
 *   * **产物原子性**：解压先落 `<workDir>.part-*` 暂存目录，成功后再 rename 到最终路径（与 engines.ts 的
 *     `.part` 约定同源），因此解压落点上永不出现半成品；重复调用幂等复用（`extracted:false`）。
 */
import { execFile } from "node:child_process"
import {
  closeSync,
  existsSync,
  mkdirSync,
  openSync,
  readdirSync,
  readFileSync,
  readSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs"
import { basename, dirname, join, relative, resolve } from "node:path"
import type { Stats } from "node:fs"
import { unzipSync } from "fflate"
import type { ToolContext } from "@gebai/sdk"
import { downloadCacheDir, isAbsolutePath } from "./paths"

/** 可编译源码的种类（决定 sourcebuild 用哪套构建预设）。 */
export type SourceKind = "llama_cpp" | "cmake" | "make" | "script" | "unknown"

/** 发现到的源码候选（目录或归档）。 */
export interface SourceCandidate {
  /** 稳定标识（目录名/归档名归一），供 source_build 的 source 参数引用。 */
  id: string
  kind: SourceKind
  /** 源码所在目录；归档候选为其归档文件路径（解压后目录由 ensureSourceDir 给出）。 */
  path: string
  /** 发现根（便于报告「从哪儿找到的」）。 */
  root: string
  /** 归档信息（目录候选为 undefined）。 */
  archive?: { path: string; format: "tar.gz" | "zip" | "tar"; bytes: number }
  /** 判定依据（人读：命中的文件/字段，如 "CMakeLists.txt 含 LLAMA_"）。 */
  evidence: string[]
  /** 版本线索（目录名/CMakeLists 里的版本号/README 行）。 */
  version?: string
  /** 目录大小（字节；归档为归档体积）。 */
  bytes?: number
  /** 归档内容预览（不整体解压；顶层若干条目 + 关键文件是否在内）。 */
  peek?: { entries: number; top_level: string[]; has_cmake: boolean; has_vendor_deps: boolean; notes: string[] }
}

/** 工具链探测结果（内网编译的可行性依据）。 */
export interface ToolchainInfo {
  cmake?: string
  ninja?: string
  make?: string
  cc?: string
  cxx?: string
  nvcc?: string
  hipcc?: string
  python?: string
  git?: string
  ccache?: string
  /**
   * 工具的实际可执行文件路径（探测到才填）。用途：内网常见做法是把便携工具链放在
   * `{GEBAI_HOME}/resources/toolchain/bin`（非系统默认位置）——编译层据此可直接用绝对路径调用，
   * 不依赖 PATH 解析。上面各字段是**版本描述**（如 `cmake version 3.28.3`），本字段才是路径。
   */
  paths?: Partial<Record<"cmake" | "ninja" | "make" | "cc" | "cxx" | "nvcc" | "hipcc" | "python" | "git" | "ccache", string>>
  /** 各设备后端的就绪判定与缺口（cpu/cuda/vulkan/metal/rocm）。 */
  devices: Record<string, { ready: boolean; missing: string[]; notes: string[] }>
  /** 内网提示：缺什么、怎么离线补齐。 */
  offline_hints: string[]
}

/** 发现结果汇总。 */
export interface DiscoverResult {
  candidates: SourceCandidate[]
  /** 实际扫描过的根（含不存在的，便于用户核对配置）。 */
  scanned: Array<{ root: string; exists: boolean; entries: number }>
  notes: string[]
}

// ── 边界常量（性能与安全：一律「宁可降级，不卡死」） ────────────────────────

/** 关键字判定只读文件头 64KB（CMakeLists/README 判关键字足够，不把源码整文件拉进内存）。 */
const HEAD_BYTES = 64 * 1024
/** 目录大小统计的文件数上限（超大源码树/误指根目录时截断而非卡死）。 */
const MAX_COUNT_FILES = 20000
/** 目录大小统计的深度上限（防符号链接环）。 */
const MAX_COUNT_DEPTH = 12
/** 归档预览的条目上限（`tar -tzf … | head -n`；zip 同一口径按行截断）。 */
const MAX_PEEK_ENTRIES = 2000
/** 全量过滤（只留 CMakeLists/vendor 行）的输出上限——过滤后行数很少，这里只是防御性上限。 */
const MAX_SCAN_LINES = 2000
/** 归档预览的体积上限：超过就不预览（zip 预览要把整包读进内存，tar 列表也可能极大）。 */
const MAX_PEEK_BYTES = 128 * 1024 * 1024
/** zip 解压的体积上限（unzipSync 需整包入内存；超大 zip 请改 tar.gz 或手工解压）。 */
const MAX_UNZIP_BYTES = 1024 * 1024 * 1024
/** 只读探测命令（--version / tar -tzf）的超时。 */
const PROBE_TIMEOUT_MS = 15000
/** 解压命令的超时（大源码包解压可能几分钟）。 */
const EXTRACT_TIMEOUT_MS = 10 * 60 * 1000
/** 归档扩展名（目录候选之外的第二类候选）。 */
const ARCHIVE_RE = /\.(tar\.gz|tgz|tar|zip)$/i

// ── 小工具 ────────────────────────────────────────────────────────────────

/** 错误信息归一（不抛错的边界里到处要用）。 */
function errMsg(e: unknown): string {
  return e instanceof Error ? e.message : String(e)
}

/** 首行/短摘（探测依据只留一行，避免把整段输出灌进结果）。 */
function firstLine(s: string, max = 160): string {
  const line = String(s ?? "")
    .split(/\r?\n/)
    .find((l) => l.trim())
  const t = (line ?? "").trim()
  return t.length > max ? `${t.slice(0, max)}…` : t
}

/** 人读体积。 */
function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`
  const mb = n / 1024 ** 2
  if (mb < 1) return `${(n / 1024).toFixed(1)} KB`
  if (mb < 1024) return `${mb.toFixed(1)} MB`
  return `${(mb / 1024).toFixed(2)} GB`
}

/** 读文件头若干字节（文件不足则读全部；读不到返回空串）。 */
function readHead(file: string, maxBytes = HEAD_BYTES): string {
  try {
    const st = statSync(file)
    if (!st.isFile()) return ""
    const len = Math.min(st.size, maxBytes)
    if (len <= 0) return ""
    const buf = Buffer.allocUnsafe(len)
    const fd = openSync(file, "r")
    try {
      readSync(fd, buf, 0, len, 0)
    } finally {
      closeSync(fd)
    }
    return buf.toString("utf-8")
  } catch {
    return ""
  }
}

/** 文件大小（读不到按 0）。 */
function fileSize(file: string): number {
  try {
    const st = statSync(file)
    return st.isFile() ? st.size : 0
  } catch {
    return 0
  }
}

/** 去重（保序）。 */
function uniq(list: string[]): string[] {
  const seen = new Set<string>()
  const out: string[] = []
  for (const s of list) {
    if (seen.has(s)) continue
    seen.add(s)
    out.push(s)
  }
  return out
}

/** 目录递归小计（限文件数/深度；用 statSync 遍历，超限标注 truncated）。 */
function dirBytes(root: string): { bytes: number; files: number; truncated: boolean } {
  let bytes = 0
  let files = 0
  let truncated = false
  const stack: Array<{ dir: string; depth: number }> = [{ dir: root, depth: 0 }]
  while (stack.length) {
    const cur = stack.pop() as { dir: string; depth: number }
    if (cur.depth > MAX_COUNT_DEPTH) {
      truncated = true
      continue
    }
    let names: string[]
    try {
      names = readdirSync(cur.dir)
    } catch {
      continue // 不可读子目录：跳过（不中断统计）
    }
    for (const name of names) {
      if (files >= MAX_COUNT_FILES) break
      const p = join(cur.dir, name)
      try {
        const st = statSync(p)
        if (st.isDirectory()) stack.push({ dir: p, depth: cur.depth + 1 })
        else if (st.isFile()) {
          bytes += st.size
          files++
        }
      } catch {
        /* 断链符号链接等：跳过 */
      }
    }
    if (files >= MAX_COUNT_FILES) {
      truncated = true
      break
    }
  }
  return { bytes, files, truncated }
}

/** shell 参数引用（路径可能含空格；只对需要的加引号，便于人工核对命令形态）。 */
function shellQuote(s: string): string {
  return /^[A-Za-z0-9_@%+=:,./\\-]+$/.test(s) ? s : `"${s.replace(/(["\\$`])/g, "\\$1")}"`
}

/** 命令缺失的判定（与 engines.ts 同一口径：区分「没装」与「装了但失败」）。 */
function isMissingText(s: string): boolean {
  return /ENOENT|not recognized|not found|command not found|不是内部或外部命令|无法将|找不到|不存在/i.test(s)
}

/** 归档格式（非归档返回 undefined）。 */
function archiveFormat(name: string): "tar.gz" | "tar" | "zip" | undefined {
  if (/\.zip$/i.test(name)) return "zip"
  if (/\.(tar\.gz|tgz)$/i.test(name)) return "tar.gz"
  if (/\.tar$/i.test(name)) return "tar"
  return undefined
}

/** 候选 id：目录名/归档名归一（去归档后缀、其它字符归一为 `-`）——同一份源码的目录与归档共用 id。 */
function candidateId(name: string): string {
  const base = String(name ?? "").replace(ARCHIVE_RE, "")
  const norm = base
    .replace(/[^A-Za-z0-9._-]+/g, "-")
    .replace(/^-+/, "")
    .replace(/-+$/, "")
  return norm || base || "source"
}

/** 从名字里取 llama.cpp 版本线索（`llama.cpp-b11175` → `b11175`）。 */
function versionFromName(name: string): string | undefined {
  return /b\d{4,}/i.exec(String(name ?? ""))?.[0]
}

/** 从 CMakeLists 里取版本线索（LLAMA_VERSION 优先，其次 project(... VERSION x.y)）。 */
function versionFromCmake(text: string): string | undefined {
  const m =
    /LLAMA_VERSION\s+"?([0-9][0-9.]*)/i.exec(text) ??
    /project\s*\([^)]*?VERSION\s+([0-9][0-9.]*)/is.exec(text) ??
    /set\s*\(\s*LLAMA_VERSION\s+([0-9][0-9.]*)/i.exec(text)
  return m?.[1]
}

/** 归档内是否含 vendor/ 依赖（vendor 下至少 1 个子目录）——内网离线编译的关键。 */
function hasVendorDep(name: string): boolean {
  const segs = String(name ?? "")
    .replace(/^\.?\//, "")
    .replace(/\/+$/, "")
    .split("/")
  const i = segs.indexOf("vendor")
  if (i < 0) return false
  const after = segs.slice(i + 1)
  if (after.length >= 2) return true
  // 目录条目 `…/vendor/<sub>/`：vendor 后恰有一段但带尾斜杠（说明是目录而非文件）
  return after.length === 1 && /\/$/.test(name)
}

/** 归档内的安全相对路径（防路径穿越：绝对路径/盘符/`..` 一律丢弃）。 */
function safeRel(name: string): string | null {
  const norm = String(name).replace(/\\/g, "/").replace(/^\.?\//, "")
  if (!norm || /^[a-zA-Z]:/.test(norm)) return null
  const parts = norm.split("/").filter((p) => p && p !== ".")
  if (!parts.length || parts.includes("..")) return null
  return parts.join("/")
}

// ── 发现根 ────────────────────────────────────────────────────────────────

/**
 * 切分 `LOCAL_INFER_SOURCE_DIRS`：`;` 与 `:` 都当分隔符（跨平台配置同一份环境变量），空段忽略。
 * 但带盘符的 Windows 路径（`C:\src`、`D:/s`）里的冒号属于路径本身，不作分隔——
 * 否则 `C:\src` 会被切成 `C` 与 `\src` 两个无意义段。
 */
function splitSourceDirs(raw: string): string[] {
  const out: string[] = []
  let buf = ""
  for (let i = 0; i < raw.length; i++) {
    const ch = raw[i]
    if (ch === ";") {
      out.push(buf)
      buf = ""
      continue
    }
    if (ch === ":") {
      const next = raw[i + 1] ?? ""
      if (/^[A-Za-z]$/.test(buf) && (next === "\\" || next === "/")) {
        buf += ch // 盘符形态：冒号归路径
        continue
      }
      out.push(buf)
      buf = ""
      continue
    }
    buf += ch
  }
  out.push(buf)
  return out.map((s) => s.trim()).filter(Boolean)
}

/** 路径归一：绝对路径（含 Windows 形态）保持原样只去尾分隔符；相对路径按 cwd 解析。 */
function normalizeRoot(p: string): string {
  if (isAbsolutePath(p)) return p.replace(/[\\/]+$/, "") || p
  return resolve(process.cwd(), p)
}

/** 发现根列表（绝对路径，按优先级）。 */
export function sourceRoots(home: string, env: Record<string, string>): string[] {
  const out: string[] = []
  const seen = new Set<string>()
  const push = (raw: string): void => {
    const abs = normalizeRoot(raw)
    if (!abs) return
    const key = process.platform === "win32" ? abs.toLowerCase() : abs
    if (seen.has(key)) return
    seen.add(key)
    out.push(abs)
  }
  for (const p of splitSourceDirs(env.LOCAL_INFER_SOURCE_DIRS ?? "")) push(p)
  // 与 paths.modelsDir 同口径：{GEBAI_HOME} = home/..，资源目录都在 {GEBAI_HOME}/resources/ 下
  push(resolve(home, "..", "resources", "src"))
  push(resolve(home, "..", "resources", "engines"))
  push(resolve(home, "engine"))
  return out
}

// ── 外部命令（只读探测 / 列归档 / 解压） ──────────────────────────────────

interface CmdResult {
  code: number
  stdout: string
  stderr: string
  /** 命令本身不存在（与「命令存在但失败」区分，notes 里说法不同）。 */
  missing: boolean
}

/** 无 ctx 时的直连执行（execFile：不经过 shell，参数不经二次解析）。 */
function execFileCapture(cmd: string, args: string[], timeoutMs: number): Promise<CmdResult> {
  return new Promise((resolvePromise) => {
    try {
      execFile(cmd, args, { timeout: timeoutMs, windowsHide: true, maxBuffer: 8 * 1024 * 1024 }, (err, stdout, stderr) => {
        const out = String(stdout ?? "")
        const errText = String(stderr ?? "")
        if (!err) {
          resolvePromise({ code: 0, stdout: out, stderr: errText, missing: false })
          return
        }
        const e = err as Error & { code?: unknown; killed?: unknown }
        const rc = typeof e.code === "number" ? e.code : e.killed === true ? -1 : 127
        const line = errText || e.message
        resolvePromise({ code: rc, stdout: out, stderr: line, missing: e.code === "ENOENT" || isMissingText(line) })
      })
    } catch (e) {
      resolvePromise({ code: 127, stdout: "", stderr: errMsg(e), missing: true })
    }
  })
}

/** 执行外部命令：ctx.runCommand 优先（宿主统一审记/超时）；无 ctx 时直连 execFile。两条路径都不抛错。 */
async function runCmd(cmd: string, args: string[], ctx?: ToolContext, timeoutMs = PROBE_TIMEOUT_MS): Promise<CmdResult> {
  if (ctx) {
    try {
      const r = await ctx.runCommand([cmd, ...args].map(shellQuote).join(" "), { timeoutMs })
      const stderr = r.stderr ?? ""
      return { code: r.code, stdout: r.stdout ?? "", stderr, missing: isMissingText(stderr) }
    } catch (e) {
      const msg = errMsg(e)
      return { code: 127, stdout: "", stderr: msg, missing: isMissingText(msg) }
    }
  }
  return execFileCapture(cmd, args, timeoutMs)
}

/**
 * 列 tar 归档条目（**不解压**，只读条目名）。两种模式：
 *   * `sample`——POSIX 走 `tar -tzf <file> | head -n N`（只留前若干条，供「条目数/顶层目录」预览）；
 *     Windows 下 bsdtar 同名可用但未必有 head，故不加管道。
 *   * `filter`——**全量**过滤，只把含 `CMakeLists.txt` 或 `vendor` 的行留下（输出很小，但扫的是整个归档）：
 *     `tar -tzf <file> | grep -aE '…'`（Windows 用 findstr）。为什么不靠采样判定？真实案例：
 *     `llama.cpp-b11175.tar.gz` 的顶层 `vendor/` 出现在 3999 条里的最后几十条，按前 2000 条采样判会
 *     **假阴性**——而 vendor/ 恰是「离线能不能编译」的关键信号，不能靠截断后的样本下结论。
 * 无 ctx 时 execFile 无法管道：直接列出全部条目，由调用方在 JS 侧按行处理。
 * **注意**：管道形态下退出码是 head/grep 的（grep 无匹配即退出码 1），因此「tar 是否成功」以 stdout 是否为空、
 * 管道里的命令是否缺失（stderr）来判，而不是看退出码。
 */
async function listTarEntries(
  file: string,
  format: "tar" | "tar.gz",
  ctx: ToolContext | undefined,
  mode: "sample" | "filter",
): Promise<CmdResult & { capped: boolean }> {
  const args = format === "tar" ? ["-tf", file] : ["-tzf", file]
  const win = process.platform === "win32"
  if (!ctx) return { ...(await execFileCapture("tar", args, PROBE_TIMEOUT_MS)), capped: false }

  const base = ["tar", ...args].map(shellQuote).join(" ")
  const pipeline =
    mode === "sample"
      ? win
        ? base
        : `${base} | head -n ${MAX_PEEK_ENTRIES}`
      : win
        ? `${base} | findstr /I /C:"CMakeLists.txt" /C:"vendor"`
        : `${base} | grep -aE '(^|/)vendor(/|$)|(^|/)CMakeLists\\.txt$' | head -n ${MAX_SCAN_LINES}`
  try {
    const r = await ctx.runCommand(pipeline, { timeoutMs: PROBE_TIMEOUT_MS })
    const stderr = r.stderr ?? ""
    return { code: r.code, stdout: r.stdout ?? "", stderr, missing: isMissingText(stderr), capped: mode === "sample" && !win }
  } catch (e) {
    const msg = errMsg(e)
    return { code: 127, stdout: "", stderr: msg, missing: isMissingText(msg), capped: false }
  }
}

// ── 目录候选识别 ──────────────────────────────────────────────────────────

/** 目录内的构建入口（cmake/make/脚本任一即视为可编译）。 */
function buildEntry(dir: string): string | undefined {
  const names = ["CMakeLists.txt", "Makefile", "makefile", "GNUmakefile", "build.sh", "build.bat"]
  return names.map((n) => join(dir, n)).find((p) => existsSync(p))
}

/** 目录内 vendor/ 下的依赖子目录数（不存在返回 undefined，存在但为空返回 0）。 */
function vendorSubdirs(dir: string): number | undefined {
  const v = join(dir, "vendor")
  try {
    if (!existsSync(v) || !statSync(v).isDirectory()) return undefined
    return readdirSync(v, { withFileTypes: true }).filter((e) => e.isDirectory() && !e.name.startsWith(".")).length
  } catch {
    return undefined
  }
}

interface DirInfo {
  kind: SourceKind
  evidence: string[]
  version?: string
}

/**
 * 判定一个目录的可编译种类（evidence 写清依据，便于人工核对为什么这么判）。
 * 优先级：llama_cpp（CMakeLists 含 LLAMA_/GGML_ 标志，或目录名含 llama.cpp）→ cmake → make → script → unknown。
 * 同时有 CMakeLists.txt 与 Makefile 时按 cmake 优先（证据里写明两者都在）。
 */
function classifyDir(dir: string, name: string): DirInfo {
  const evidence: string[] = []
  const cmakeFile = join(dir, "CMakeLists.txt")
  const hasCmake = existsSync(cmakeFile)
  const makeFile = ["Makefile", "makefile", "GNUmakefile"].map((f) => join(dir, f)).find((p) => existsSync(p))
  const scriptFile = ["build.sh", "build.bat"].map((f) => join(dir, f)).find((p) => existsSync(p))
  const nameHit = /llama[._-]?cpp/i.test(name)

  let version = versionFromName(name)
  let cmakeKeyHit = false
  if (hasCmake) {
    const head = readHead(cmakeFile)
    cmakeKeyHit = /LLAMA_|GGML_|llama/i.test(head)
    version = version ?? versionFromCmake(head)
    evidence.push(cmakeKeyHit ? "CMakeLists.txt 含 LLAMA_/GGML_ 关键标志" : "存在 CMakeLists.txt（未见 LLAMA_/GGML_ 标志）")
  }
  if (nameHit) evidence.push("目录名匹配 /llama[._-]?cpp/i")
  if (makeFile) evidence.push(`同时存在 ${basename(makeFile)}${hasCmake ? "（cmake 与 make 都在，按 cmake 优先）" : ""}`)
  if (scriptFile) evidence.push(`存在构建脚本 ${basename(scriptFile)}`)

  let kind: SourceKind
  if ((hasCmake && cmakeKeyHit) || nameHit) kind = "llama_cpp"
  else if (hasCmake) kind = "cmake"
  else if (makeFile) kind = "make"
  else if (scriptFile) kind = "script"
  else {
    kind = "unknown"
    evidence.push("无 CMakeLists.txt/Makefile/build.sh——仅列出以便人工确认")
  }

  // llama.cpp / cmake 类要提示 vendor/ 依赖：离线编译缺它时 CMake 会 FetchContent 联网
  if (kind === "llama_cpp" || kind === "cmake") {
    const subs = vendorSubdirs(dir)
    if (subs === undefined) evidence.push("未含 vendor/ 依赖——离线编译可能因 FetchContent 联网失败（需完整源码包）")
    else if (subs === 0) evidence.push("vendor/ 为空（无依赖子目录）——离线编译仍可能联网失败")
    else evidence.push(`含 vendor/ 依赖目录（${subs} 项）——离线可编译`)
  }
  if (version) evidence.push(`版本线索：${version}`)
  return { kind, evidence, version }
}

/** 目录 → 候选。 */
function candidateFromDir(dir: string, root: string): SourceCandidate {
  const name = basename(dir)
  const cls = classifyDir(dir, name)
  const size = dirBytes(dir)
  const evidence = [...cls.evidence]
  if (size.truncated) evidence.push(`大小统计截断（仅计入前 ${MAX_COUNT_FILES} 个文件）`)
  return { id: candidateId(name), kind: cls.kind, path: dir, root, evidence, version: cls.version, bytes: size.bytes }
}

// ── 归档候选（只 peek，不整体解压） ────────────────────────────────────────

/**
 * 预览归档内容：条目数、顶层条目、是否含 CMakeLists.txt、是否含 vendor/ 依赖。
 * 拿不到就返回 undefined 并把原因写进 notes（绝不为了预览把大归档整包读入内存）。
 */
async function peekArchive(
  file: string,
  format: "tar.gz" | "tar" | "zip",
  bytes: number,
  ctx: ToolContext | undefined,
  notes: string[],
  evidence: string[],
): Promise<SourceCandidate["peek"] | undefined> {
  if (bytes > MAX_PEEK_BYTES) {
    notes.push(`归档较大（${formatBytes(bytes)}），未预览：${file}——解压阶段会正常处理`)
    evidence.push("未预览（体积超限）")
    return undefined
  }

  let names: string[]
  let keyNames: string[]
  let capped = false
  const pnotes: string[] = []
  if (format === "zip") {
    try {
      names = Object.keys(unzipSync(readFileSync(file)))
      keyNames = names // zip 的名字列表本就是全量（无截断），关键判定直接用全量
    } catch (e) {
      notes.push(`归档预览失败（${file}）：zip 不可读（${errMsg(e)}）——解压阶段会再报一次`)
      evidence.push("未预览（zip 不可读）")
      return undefined
    }
  } else {
    const sample = await listTarEntries(file, format, ctx, "sample")
    if (!sample.stdout.trim()) {
      const why = sample.missing
        ? "系统 tar 不可用（Windows 10+ 自带 bsdtar；Linux 需装 tar）"
        : `tar 未列出条目（退出码 ${sample.code}：${firstLine(sample.stderr) || "无输出"}）`
      notes.push(`归档预览失败（${file}）：${why}——可手工解压后把目录放进发现根，或改用 zip/已解压目录`)
      evidence.push("未预览（tar 列条目失败）")
      return undefined
    }
    names = sample.stdout.split(/\r?\n/)
    capped = sample.capped
    // 关键判定（CMakeLists / vendor）走**全量过滤**：采样被 head 截断时 vendor/ 可能落在截断之后
    keyNames = names
    const scan = await listTarEntries(file, format, ctx, "filter")
    if (scan.missing || isMissingText(scan.stderr)) {
      pnotes.push(`全量条目过滤不可用（grep/findstr 缺失）：vendor/ 与 CMakeLists 判定只基于已列出的前 ${names.length} 条`)
      evidence.push("关键判定未全量过滤")
    } else {
      keyNames = scan.stdout.split(/\r?\n/)
    }
  }

  const all = names.map((n) => n.trim()).filter(Boolean)
  const keys = keyNames.map((n) => n.trim()).filter(Boolean)
  const truncated = capped && all.length >= MAX_PEEK_ENTRIES
  const examined = truncated ? all.slice(0, MAX_PEEK_ENTRIES) : all
  const topLevel = uniq(
    examined
      .map((n) => n.replace(/^\.?\//, "").split("/")[0] ?? "")
      .filter(Boolean),
  ).slice(0, 20)
  const cmakeHits = keys.filter((n) => /(^|\/)CMakeLists\.txt$/i.test(n))
  const hasCmake = cmakeHits.length > 0
  const hasVendor = keys.some(hasVendorDep)

  if (hasCmake) {
    pnotes.push(`归档内含 CMakeLists.txt：${cmakeHits.slice(0, 3).join("、")}${cmakeHits.length > 3 ? ` 等 ${cmakeHits.length} 个` : ""}`)
  } else {
    pnotes.push("归档内未发现 CMakeLists.txt")
  }
  if (!hasVendor) {
    pnotes.push(
      "归档内未发现 vendor/ 依赖目录：离线编译需用包含 vendor 的源码包（如 `git clone --recursive` 或完整源码归档）",
    )
  }
  if (truncated) pnotes.push(`归档条目较多：仅列出前 ${MAX_PEEK_ENTRIES} 条（entries 为已列出条数，非总数；关键判定已全量过滤）`)
  evidence.push(hasVendor ? "归档内含 vendor/ 依赖" : "归档内无 vendor/ 依赖")

  return { entries: examined.length, top_level: topLevel, has_cmake: hasCmake, has_vendor_deps: hasVendor, notes: pnotes }
}

/** 归档 → 候选（peek 结果决定 kind：归档名有 llama.cpp 或内含 CMakeLists + ggml/src 顶层）。 */
async function candidateFromArchive(
  file: string,
  root: string,
  format: "tar.gz" | "tar" | "zip",
  bytes: number,
  ctx: ToolContext | undefined,
  notes: string[],
): Promise<SourceCandidate> {
  const name = basename(file)
  const evidence: string[] = [`归档 ${format}（${formatBytes(bytes)}）`]
  const peek = await peekArchive(file, format, bytes, ctx, notes, evidence)
  const nameHit = /llama[._-]?cpp/i.test(name)
  const version = versionFromName(name)

  let kind: SourceKind
  if (nameHit) {
    kind = "llama_cpp"
    evidence.push("归档名匹配 /llama[._-]?cpp/i")
  } else if (peek?.has_cmake && peek.top_level.some((t) => /^(ggml|src)$/i.test(t))) {
    kind = "llama_cpp"
    evidence.push("归档内含 CMakeLists.txt 且顶层含 ggml/ 或 src/")
  } else if (peek?.has_cmake) {
    kind = "cmake"
    evidence.push("归档内含 CMakeLists.txt")
  } else {
    kind = "unknown"
    evidence.push("归档内未见 CMakeLists.txt（或未预览）——仅列出以便人工确认")
  }
  if (version) evidence.push(`版本线索：${version}`)

  return {
    id: candidateId(name),
    kind,
    path: file,
    root,
    archive: { path: file, format, bytes },
    evidence,
    version,
    bytes,
    peek,
  }
}

// ── 发现 ──────────────────────────────────────────────────────────────────

/**
 * 扫描发现根，识别可编译源码（目录与归档；归档只 peek 不整体解压）。
 * 逐根只扫**一层**（源码目录/归档都直接放在根下；不递归，避免误扫整个家目录），
 * 任何单点失败（根不存在/不可读、条目断链、tar 不可用、命令超时）都降级为 notes。
 */
export async function discoverSources(
  home: string,
  env: Record<string, string>,
  opts?: { ctx?: ToolContext; ids?: string[] },
): Promise<DiscoverResult> {
  const notes: string[] = []
  const candidates: SourceCandidate[] = []
  const scanned: DiscoverResult["scanned"] = []
  const seenIds = new Map<string, string>() // id → 首个命中路径（去重：先发现者优先）

  for (const root of sourceRoots(home, env)) {
    let exists = false
    let names: string[] = []
    try {
      const st = statSync(root)
      exists = true
      if (st.isDirectory()) {
        try {
          names = readdirSync(root)
        } catch (e) {
          notes.push(`发现根不可读，已跳过：${root}（${errMsg(e)}）`)
        }
      } else {
        notes.push(`发现根不是目录，已跳过：${root}`)
      }
    } catch (e) {
      const code = (e as { code?: unknown }).code
      if (code === "ENOENT") {
        exists = false // 未配置的默认根就长这样：不算异常，只在 scanned 里标 exists:false
      } else {
        notes.push(`发现根不可读，已跳过：${root}（${errMsg(e)}）`)
      }
    }

    let entries = 0
    for (const name of names.sort()) {
      if (name.startsWith(".")) continue // 隐藏条目（.cache/.git 等）不当作源码候选
      const p = join(root, name)
      let st: Stats
      try {
        st = statSync(p)
      } catch (e) {
        notes.push(`条目不可读，已跳过：${p}（${errMsg(e)}）`)
        continue
      }
      let cand: SourceCandidate | undefined
      if (st.isDirectory()) {
        entries++
        cand = candidateFromDir(p, root)
      } else if (st.isFile()) {
        const fmt = archiveFormat(name)
        if (!fmt) continue // 普通文件（README/文本）不是候选
        entries++
        cand = await candidateFromArchive(p, root, fmt, st.size, opts?.ctx, notes)
      } else {
        continue
      }
      const dup = seenIds.get(cand.id)
      if (dup) {
        notes.push(`id 重复，已保留先发现者：${cand.id}（保留 ${dup}，忽略 ${cand.path}）`)
        continue
      }
      seenIds.set(cand.id, cand.path)
      candidates.push(cand)
    }

    scanned.push({ root, exists, entries })
  }

  const want = opts?.ids && opts.ids.length ? new Set(opts.ids) : undefined
  const filtered = want ? candidates.filter((c) => want.has(c.id)) : candidates
  if (want && filtered.length < candidates.length) {
    const miss = opts?.ids?.filter((id) => !candidates.some((c) => c.id === id)) ?? []
    if (miss.length) notes.push(`按 ids 过滤：未命中 ${miss.join("、")}`)
  }
  return { candidates: filtered, scanned, notes }
}

/**
 * 按 id 或路径找候选（路径可直接给目录/归档，便于「就地编译别处的源码」——不要求它在发现根内）。
 * 命中失败时返回 `{ error }`，里面带**可用 id 清单**与「源码该放哪儿」的建议。
 */
export async function findSource(
  home: string,
  env: Record<string, string>,
  ref: string,
  opts?: { ctx?: ToolContext },
): Promise<{ candidate?: SourceCandidate; error?: string }> {
  const raw = String(ref ?? "").trim()
  if (!raw) return { error: "需要源码 id 或目录/归档路径：先用 local_infer_source_list 看已发现清单。" }
  if (/^https?:\/\//i.test(raw)) {
    return {
      error: `本层只处理本地源码（内网/离线）：不支持 URL ${raw}——请先下载并解压到 {GEBAI_HOME}/resources/src/，或用 LOCAL_INFER_SOURCE_DIRS 指向已解压目录。`,
    }
  }

  const base = opts?.ctx?.workdir ?? process.cwd()
  const abs = isAbsolutePath(raw) ? raw : resolve(base, raw)
  if (existsSync(abs)) {
    try {
      const st = statSync(abs)
      if (st.isDirectory()) return { candidate: candidateFromDir(abs, dirname(abs)) }
      const fmt = archiveFormat(basename(abs))
      if (!fmt) return { error: `不是可识别的源码：${abs} 既不是目录，也不是 .tar.gz/.tgz/.tar/.zip 归档。` }
      return { candidate: await candidateFromArchive(abs, dirname(abs), fmt, st.size, opts?.ctx, []) }
    } catch (e) {
      return { error: `读取源码失败：${abs}（${errMsg(e)}）` }
    }
  }

  const found = await discoverSources(home, env, opts)
  const hit = found.candidates.find((c) => c.id === raw)
  if (hit) return { candidate: hit }
  const ids = found.candidates.map((c) => c.id)
  const list = ids.length ? `已发现的 id：${ids.join("、")}` : "当前未发现任何源码"
  return {
    error: `${raw} 既不是存在的路径，也不是已发现的源码 id。${list}。把源码（目录或 .tar.gz/.zip）放到 {GEBAI_HOME}/resources/src/，或用 LOCAL_INFER_SOURCE_DIRS 指定扫描根。`,
  }
}

// ── 归档解压（幂等 + 下钻顶层目录） ────────────────────────────────────────

/**
 * 解压落点 → 真正的源码目录：根本身有构建入口就用它；否则唯一顶层目录且它含构建入口就用它
 * （多数源码归档有一个顶层目录，如 `llama.cpp-b11175/`）。顶层多个目录/找不到入口返回 undefined。
 */
function adoptedDir(root: string): string | undefined {
  try {
    if (!existsSync(root) || !statSync(root).isDirectory()) return undefined
  } catch {
    return undefined
  }
  if (buildEntry(root)) return root
  let kids: string[]
  try {
    kids = readdirSync(root, { withFileTypes: true })
      .filter((e) => e.isDirectory() && !e.name.startsWith("."))
      .map((e) => e.name)
  } catch {
    return undefined
  }
  if (kids.length === 1) {
    const child = join(root, kids[0] as string)
    return buildEntry(child) ? child : undefined
  }
  return undefined
}

/** 解压归档到目录；成功返回 undefined，失败返回可操作的中文原因（不抛错）。 */
async function extractArchive(
  archive: string,
  format: "tar.gz" | "tar" | "zip",
  destDir: string,
  ctx?: ToolContext,
): Promise<string | undefined> {
  if (format === "zip") {
    const bytes = fileSize(archive)
    if (bytes > MAX_UNZIP_BYTES) {
      return `解压失败（${archive}）：zip 解压需整包读入内存，该归档 ${formatBytes(bytes)} 过大——请改用 .tar.gz，或手工解压后把目录放进发现根。`
    }
    try {
      const files = unzipSync(readFileSync(archive))
      for (const [name, data] of Object.entries(files)) {
        const rel = safeRel(name)
        if (!rel) continue
        const out = join(destDir, rel)
        if (name.endsWith("/")) {
          mkdirSync(out, { recursive: true })
          continue
        }
        mkdirSync(dirname(out), { recursive: true })
        writeFileSync(out, data)
      }
      return undefined
    } catch (e) {
      return `解压失败（${archive}）：zip 不可读或损坏（${errMsg(e)}）——请确认归档完整（可重新拷贝/校验 sha256）后重试。`
    }
  }

  const r = await runCmd("tar", [format === "tar" ? "-xf" : "-xzf", archive, "-C", destDir], ctx, EXTRACT_TIMEOUT_MS)
  if (r.code === 0) return undefined
  const why = r.missing
    ? "系统 tar 不可用（Windows 10+ 自带 bsdtar；Linux 需装 tar）"
    : `tar 退出码 ${r.code}：${firstLine(r.stderr) || "无输出"}`
  return `解压失败（${archive}）：${why}——可先在本机手工解压，再把顶层目录放进 {GEBAI_HOME}/resources/src/（或把源码目录路径直接交给 source_build）。`
}

/**
 * 确保候选可编译：目录直接返回；归档解压到工作目录（默认 `{home}/vendor/.cache/src/<id>`）。
 * 幂等：落点已有解压结果（含构建入口）就直接复用，`extracted:false`；解压走暂存目录 + rename，
 * 因此最终路径上不会出现半成品。
 */
export async function ensureSourceDir(
  home: string,
  candidate: SourceCandidate,
  opts?: { ctx?: ToolContext; workDir?: string },
): Promise<{ dir?: string; extracted?: boolean; error?: string }> {
  if (!candidate.archive) {
    const dir = candidate.path
    if (!existsSync(dir)) return { error: `源码目录不存在：${dir}（可能已被移动/删除；重跑 local_infer_source_list 确认）` }
    // 构建入口：CMakeLists.txt / Makefile 是硬要求；script 类候选只有构建脚本，同样放行（否则 discovery 发现的它无路可走）
    if (!buildEntry(dir)) {
      return { error: `目录无可识别的构建入口：${dir}——需含 CMakeLists.txt、Makefile 或 build.sh/build.bat 之一。` }
    }
    return { dir, extracted: false }
  }

  const archive = candidate.archive
  const workDir = opts?.workDir ?? join(downloadCacheDir(home), "src", candidate.id)

  const reuse = adoptedDir(workDir)
  if (reuse) return { dir: reuse, extracted: false }
  if (!existsSync(archive.path)) return { error: `归档不存在：${archive.path}（可能已被移动/删除；重跑 local_infer_source_list 确认）` }

  const staging = `${workDir}.part-${process.pid.toString(36)}-${Date.now().toString(36)}`
  try {
    rmSync(staging, { recursive: true, force: true })
    mkdirSync(staging, { recursive: true })
    const failed = await extractArchive(archive.path, archive.format, staging, opts?.ctx)
    if (failed) {
      rmSync(staging, { recursive: true, force: true })
      return { error: failed }
    }
    const adopted = adoptedDir(staging)
    rmSync(workDir, { recursive: true, force: true }) // 清掉可能残留的半成品解压结果
    mkdirSync(dirname(workDir), { recursive: true })
    renameSync(staging, workDir)
    if (!adopted) {
      // 解压成功了但没有构建入口：保留现场（在 workDir 里）便于人工核对，同时给出可操作错误
      return {
        error: `归档已解压但未找到构建入口（CMakeLists.txt/Makefile/build.sh）：${workDir}——请确认放的是完整源码包（llama.cpp 需含 vendor/ 依赖）。`,
      }
    }
    const rel = relative(staging, adopted)
    return { dir: rel ? join(workDir, rel) : workDir, extracted: true }
  } catch (e) {
    rmSync(staging, { recursive: true, force: true })
    return { error: `归档解压失败（${archive.path}）：${errMsg(e)}` }
  }
}

// ── 工具链探测 ────────────────────────────────────────────────────────────

/** 编译链上要探的工具（ToolchainInfo 的字段名）。 */
type ToolKey = "cmake" | "ninja" | "make" | "cc" | "cxx" | "nvcc" | "hipcc" | "python" | "git" | "ccache"

/** 工具名候选（平台感知：Windows 上 cc/cxx 换 cl/gcc 兼容探测，POSIX 用 cc/c++ 优先）。 */
function toolProbes(platform: string): Array<{ key: ToolKey; names: string[] }> {
  const win = platform === "win32"
  return [
    { key: "cmake", names: ["cmake"] },
    { key: "ninja", names: ["ninja"] },
    { key: "make", names: win ? ["make", "mingw32-make", "nmake"] : ["make", "gmake"] },
    { key: "cc", names: win ? ["cl", "gcc", "clang"] : ["cc", "gcc", "clang"] },
    { key: "cxx", names: win ? ["cl", "g++", "clang++"] : ["c++", "g++", "clang++"] },
    { key: "nvcc", names: ["nvcc"] },
    { key: "hipcc", names: ["hipcc"] },
    { key: "python", names: win ? ["python", "python3"] : ["python3", "python"] },
    { key: "git", names: ["git"] },
    { key: "ccache", names: ["ccache"] },
  ]
}

/** 命令是否存在（版本探不到时的兜底；只用系统自带的 command -v / where，不引第三方）。 */
async function commandExists(name: string, platform: string, ctx?: ToolContext): Promise<boolean> {
  const r = platform === "win32" ? await runCmd("where", [name], ctx) : await runCmd("sh", ["-c", `command -v ${name}`], ctx)
  return r.code === 0 && r.stdout.trim().length > 0
}

/** 工具可执行文件的**绝对路径**（`command -v` / `where` 首行）：内网便携工具链不在默认 PATH 位置时，
 *  编译层可直接拿它调用。探不到返回 undefined（不抛错）。 */
async function resolveToolPath(names: string[], platform: string, ctx?: ToolContext): Promise<string | undefined> {
  for (const name of names) {
    const r = platform === "win32" ? await runCmd("where", [name], ctx) : await runCmd("sh", ["-c", `command -v ${name}`], ctx)
    if (r.code === 0) {
      const line = firstLine(r.stdout)
      if (line) return line
    }
  }
  return undefined
}

/**
 * 取工具版本：`<tool> --version` 首行；退出码非 0 时退回「命令是否存在」（部分工具 --version 非 0 退出），
 * 存在但取不到版本 → `名称（版本未知：…）`；探测不到返回 undefined（不抛错）。
 */
async function probeVersion(names: string[], platform: string, ctx?: ToolContext): Promise<string | undefined> {
  for (const name of names) {
    const r = await runCmd(name, ["--version"], ctx)
    if (r.code === 0) {
      const line = firstLine(r.stdout) || firstLine(r.stderr)
      return line || `${name}（版本未知）`
    }
    if (await commandExists(name, platform, ctx)) {
      return `${name}（版本未知：${firstLine(r.stderr) || `退出码 ${r.code}`}）`
    }
  }
  return undefined
}

/** Vulkan 开发包探测（Linux 头文件 / pkg-config；Windows 看 VULKAN_SDK）。 */
async function vulkanDevReady(
  platform: string,
  ctx: ToolContext | undefined,
  env: Record<string, string | undefined>,
): Promise<{ ok: boolean; evidence: string[] }> {
  if (platform === "win32") {
    const sdk = env.VULKAN_SDK
    return sdk
      ? { ok: true, evidence: [`VULKAN_SDK=${sdk}`] }
      : { ok: false, evidence: ["Windows 需 Vulkan SDK（未设置 VULKAN_SDK 环境变量）"] }
  }
  const headers = ["/usr/include/vulkan/vulkan.h", "/usr/local/include/vulkan/vulkan.h"]
  const hit = headers.find((h) => existsSync(h))
  if (hit) return { ok: true, evidence: [`Vulkan 头文件 ${hit}`] }
  const r = await runCmd("pkg-config", ["--exists", "vulkan"], ctx)
  if (r.code === 0) return { ok: true, evidence: ["pkg-config --exists vulkan 通过"] }
  return {
    ok: false,
    evidence: [
      `缺少 Vulkan 开发包（${headers.join(" / ")} 均不存在，pkg-config --exists vulkan 退出码 ${r.code}）——运行时有驱动还不够，编译需要 headers/loader`,
    ],
  }
}

/** 各设备后端的就绪判定（cpu 是基准，其它后端在 cpu 之上加各自工具链/开发包）。 */
interface DeviceCheck {
  ready: boolean
  missing: string[]
  notes: string[]
}

/** 平台/环境可注入版本的工具链探测（测试与跨平台断言用：平台分支不随宿主漂移）。 */
export async function detectToolchainOn(
  ctx: ToolContext | undefined,
  platform: string,
  env?: Record<string, string | undefined>,
): Promise<ToolchainInfo> {
  const e = env ?? ctx?.env ?? process.env
  const emptyDevices = (reason: string): Record<string, DeviceCheck> => {
    const d: Record<string, DeviceCheck> = {}
    for (const name of ["cpu", "cuda", "vulkan", "metal", "rocm"]) d[name] = { ready: false, missing: [], notes: [reason] }
    return d
  }

  try {
    // 并行探测（各命令相互独立；单条超时/失败都只影响自己）
    const probed = await Promise.all(
      toolProbes(platform).map(async (p) => [p.key, await probeVersion(p.names, platform, ctx)] as const),
    )
    const found: Partial<Record<ToolKey, string>> = {}
    for (const [k, v] of probed) if (v) found[k] = v

    // 工具**可执行文件路径**（与上面的版本描述分开）：内网把便携工具链放在 resources/toolchain/bin 时，
    // 编译层据此用绝对路径调用；只对探测到的工具解析，单条失败不影响其它。
    const paths: ToolchainInfo["paths"] = {}
    await Promise.all(
      toolProbes(platform).map(async (p) => {
        if (!found[p.key]) return
        const abs = await resolveToolPath(p.names, platform, ctx)
        if (abs) paths[p.key] = abs
      }),
    )

    const vk = await vulkanDevReady(platform, ctx, e)
    const xcrun = platform === "darwin" ? await probeVersion(["xcrun"], platform, ctx) : undefined

    // 基准缺口：cmake 生成构建文件、ninja/make 执行构建、cc 编译源码
    const cpuMissing: string[] = []
    if (!found.cmake) cpuMissing.push("cmake")
    if (!found.ninja && !found.make) cpuMissing.push("ninja 或 make")
    if (!found.cc) cpuMissing.push("cc")

    const devices: Record<string, DeviceCheck> = {}
    devices.cpu = {
      ready: cpuMissing.length === 0,
      missing: [...cpuMissing],
      notes: cpuMissing.length
        ? [`CPU 后端还需：${cpuMissing.join("、")}——三者缺一不可（cmake 生成构建文件 / ninja 或 make 执行构建 / cc 编译源码）`]
        : [`CPU 后端就绪：cmake + ${found.ninja ? "ninja" : "make"} + ${found.cc}`],
    }

    const cudaMissing = [...cpuMissing, ...(found.nvcc ? [] : ["nvcc"])]
    devices.cuda = {
      ready: cudaMissing.length === 0,
      missing: cudaMissing,
      notes: found.nvcc
        ? [`nvcc 可用：${found.nvcc}`]
        : ["CUDA 编译需 nvcc（CUDA Toolkit）；无 GPU 机器可改用 cpu 后端"],
    }

    const vulkanMissing = [...cpuMissing, ...(vk.ok ? [] : ["vulkan 开发包"])]
    devices.vulkan = {
      ready: vulkanMissing.length === 0,
      missing: vulkanMissing,
      notes: [...vk.evidence],
    }

    if (platform !== "darwin") {
      devices.metal = {
        ready: false,
        missing: ["非 macOS 平台"],
        notes: [`仅 macOS 支持（当前平台 ${platform}）——Metal 后端不适用，请按平台选 cpu/cuda/vulkan 后端`],
      }
    } else {
      const metalMissing = [...cpuMissing, ...(xcrun ? [] : ["xcrun"])]
      devices.metal = {
        ready: metalMissing.length === 0,
        missing: metalMissing,
        notes: xcrun
          ? [`xcrun 可用：${xcrun}（Metal 后端由 Xcode 命令行工具提供编译器）`]
          : ["macOS 编译需 Xcode 命令行工具（xcrun 不可用：`xcode-select --install`）"],
      }
    }

    const rocmMissing = [...cpuMissing, ...(found.hipcc ? [] : ["hipcc"])]
    devices.rocm = {
      ready: rocmMissing.length === 0,
      missing: rocmMissing,
      notes: found.hipcc ? [`hipcc 可用：${found.hipcc}`] : ["ROCm 编译需 hipcc（HIP SDK）；无 AMD GPU 可改用 cpu/vulkan 后端"],
    }

    // 内网补齐提示：逐条给「怎么办」，而不是只说缺了什么
    const hints: string[] = []
    if (!found.cmake) {
      hints.push(
        "cmake 缺失：在联网机器下载 CMake 预编译包（cmake-*-linux-x86_64.tar.gz / -windows-x86_64.zip），整体放入 {GEBAI_HOME}/resources/toolchain/ 并把其 bin 目录加入 PATH（本层只探 PATH，不需要安装器）。",
      )
    }
    if (!found.ninja && !found.make) {
      hints.push(
        "ninja 或 make 缺失：ninja 是单文件可执行程序（联网机器下载 ninja / ninja.exe 放进 resources/toolchain/ 即可）；Linux 也可用发行版的离线 rpm/deb 装 make。",
      )
    }
    if (!found.cc) {
      hints.push(
        "C/C++ 编译器缺失：Linux 放 gcc/clang 的离线包或便携工具链；Windows 装 MSVC Build Tools 或 MinGW-w64（把 cl.exe/gcc.exe 所在目录加入 PATH）。",
      )
    }
    if (!found.nvcc) hints.push("nvcc 缺失：CUDA 编译需 CUDA Toolkit（离线用 runfile 安装包）；无 GPU 的机器请改用 cpu 后端。")
    if (!found.hipcc) hints.push("hipcc 缺失：ROCm 编译需 HIP SDK；无 AMD GPU 时改用 cpu/vulkan 后端。")
    if (!found.python) hints.push("python3 缺失：llama.cpp 的构建脚本与转换工具需要 Python 3（离线可放嵌入式 Python 并加入 PATH）。")
    if (!found.git) {
      hints.push("git 缺失：vendor/ 依赖（git submodule）需要 git；内网建议直接用已含 vendor/ 的完整源码归档，免去 git。")
    }
    if (!found.ccache) hints.push("ccache 缺失（可选）：不影响编译正确性，只影响重复构建的速度。")
    hints.push(
      "离线编译须确保源码包含 vendor/ 依赖：llama.cpp 的 CMake 会用 FetchContent 拉取依赖，缺 vendor 时会尝试联网（内网表现为长时间挂起或失败）——请用 `git clone --recursive` 生成的完整归档或官方源码包。",
    )
    hints.push("source_build 可用 offline=true 让 CMake 在缺依赖时立即失败而非挂死网络（等价于 -DFETCHCONTENT_FULLY_DISCONNECTED=ON）。")
    hints.push(
      "内网补工具链的通用做法：联网机器下载预编译包 → 放入 {GEBAI_HOME}/resources/toolchain/ → 其 bin 目录加入 PATH（无需安装器、无需联网）。",
    )

    return { ...found, paths, devices, offline_hints: hints }
  } catch (err) {
    return {
      devices: emptyDevices(`工具链探测异常：${errMsg(err)}`),
      offline_hints: [`工具链探测异常：${errMsg(err)}——请检查 runCommand/execFile 是否可用，或手工确认 cmake/gcc 是否在 PATH。`],
    }
  }
}

/** 工具链探测（cmake/ninja/gcc/clang/nvcc/hipcc/python/git/ccache + 各设备就绪判定 + 内网补齐提示）。 */
export async function detectToolchain(ctx?: ToolContext): Promise<ToolchainInfo> {
  return detectToolchainOn(ctx, process.platform, ctx?.env)
}
