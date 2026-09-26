/**
 * local_infer 的**源码编译安装层**：把「内网/离线拿不到预编译包」变成「有源码就能出引擎」。
 *
 * 与同目录两层的关系（分工是清晰的，互不越界）：
 *   * engines.ts      —— 下载安装层：从发行版拉预编译包 → 解压 → 装到 `vendor/<id>/` → 写 `.engine.json`；
 *   * sourcefind.ts   —— 源码发现层：在 `resources/src/` 等根目录发现可编译源码、探测工具链、按需解压；
 *   * sourcebuild.ts  —— **本文件**：把发现的源码用 cmake 就地编译成引擎，落**同一套 vendor 布局与同一标记格式**，
 *     因此 launcher / status / start 无需区分引擎是「下载的」还是「编出来的」（`installedEngine()` 一视同仁）。
 *
 * 实现约定（与 engines.ts 同一风格）：
 *   * **不抛错边界**：一切失败都返回 `{ ok:false, error }`，error 必须能指导下一步——带**日志尾部**（根因）
 *     与**依赖清单**（补什么），而不是异常栈。
 *   * **纯函数可断言**：`buildPreset` / `cmakeConfigureCmd` / `cmakeBuildCmd` 不碰文件系统与进程，
 *     逐字段可测（见 sourcebuild.test.ts）；真实编译只发生在 `buildFromSource` 里。
 *   * **产物原子性**：安装先落 `vendor/.stage-*` 再整体 rename——`vendor/<id>/` 上永不出现半成品。
 *   * **内网优先**：`-DLLAMA_CURL=OFF` 去掉对 libcurl 的硬依赖；`offline` 再加 `FETCHCONTENT_FULLY_DISCONNECTED=ON`，
 *     缺依赖**立刻失败**而不是挂在网络上。
 *   * **弱耦合发现层**：只 import 其类型，运行期用**动态 import** 取函数——该层独立演进（并行开发/可能尚未就绪），
 *     顶层静态 import 会在 ESM 链接期因「导出不存在」整体炸掉，动态 import 拿不到就降级为本地兜底。
 */
import { spawnSync } from "node:child_process"
import {
  appendFileSync,
  chmodSync,
  cpSync,
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs"
import { cpus } from "node:os"
import { basename, dirname, join } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { findLlamaServer, installedEngine, type InstalledEngine } from "./engines"
import { engineDir, reportsDir, tailLines, vendorDir } from "./paths"
import type { SourceCandidate, ToolchainInfo } from "./sourcefind"

// ── 类型（签名即契约） ────────────────────────────────────────────────────

/** 目标设备后端（与 devices 探测的键名一致）。 */
export type BuildDevice = "cpu" | "cuda" | "vulkan" | "metal" | "rocm"

/** 可选设备清单（校验与提示共用）。 */
export const BUILD_DEVICES: readonly BuildDevice[] = ["cpu", "cuda", "vulkan", "metal", "rocm"]

/** 编译安装规格（工具层解析参数后传进来）。 */
export interface BuildSpec {
  /** 安装到的引擎 id（= `vendor/<engine_id>`）。 */
  engine_id: string
  device: BuildDevice
  /** 并行度（缺省 = max(1, cpus*0.75)）。 */
  jobs?: number
  /** 是否删除已有 build 目录重配（同时是「强制重建」开关——已安装时缺省幂等跳过）。 */
  clean?: boolean
  /** 内网：禁止联网 FetchContent（缺依赖立即失败并给可操作错误）。 */
  offline?: boolean
  /** 追加的 cmake 定义（同名的 `-D` 以**后者**为准，可用来覆盖 CMAKE_CUDA_ARCHITECTURES 等）。 */
  extra_cmake_args?: string[]
}

/** 构建预设（纯函数产物：生成器 + cmake 定义 + 依赖清单 + 人读说明）。 */
export interface BuildPreset {
  generator: string
  /** 只含 `-D…` 定义；`-S/-B/-G` 由 cmakeConfigureCmd 组装（顺序稳定，便于逐字段断言）。 */
  cmakeArgs: string[]
  requirements: string[]
  notes: string[]
}

/** 进度节点（工具层据此向模型输出进度）。 */
export interface BuildStep {
  /** extract / configure / build / install / done（compile 阶段细分为开始与完成两条同相位节点）。 */
  phase: string
  message: string
  /** 该阶段实际耗时（秒，1 位小数）；起始节点不带。 */
  secs?: number
}

/** 标记文件里的「编译来源」信息（`.engine.json` 的 `build` 字段，额外字段不影响 engines.ts 的宽松解析）。 */
export interface SourceBuildInfo {
  from_source: true
  source_dir: string
  build_dir: string
  device: BuildDevice
  offline: boolean
  /** 完整构建日志（configure + build 的 stdout/stderr 与退出码）。 */
  log: string
  /** 构建主机（平台-架构；与人读的「哪台机器编的」对应）。 */
  host: string
  jobs: number
  generator: string
  cmake: string
  /** 可重放的完整命令行（config/构建各一条）。 */
  configure_cmd: string
  build_cmd: string
  /** 源码版本线索（git HEAD；无 .git 时缺失）。 */
  commit?: string
}

/** 编译安装出来的引擎（与 InstalledEngine 同构：launcher/status 无需区分来源）。 */
export type SourceBuiltEngine = InstalledEngine & { build?: SourceBuildInfo }

/** 编译安装结果。 */
export interface BuildFromSourceResult {
  ok: boolean
  engineId?: string
  /** 引擎视图（字段与 InstalledEngine 兼容；额外字段见 build）。 */
  engine?: { id: string; exe: string; dir: string; [k: string]: unknown }
  buildDir?: string
  logPath?: string
  steps: BuildStep[]
  error?: string
}

/** configure 超时（cmake 探测 + 生成构建系统；大工程也够）。 */
export const CONFIGURE_TIMEOUT_MS = 300_000
/** build 超时（llama.cpp 全量编译在慢机器上可达 1 小时以上）。 */
export const BUILD_TIMEOUT_MS = 3_600_000
/** 失败时放进 error 的日志尾部行数（模型要能直接看到根因）。 */
export const ERROR_TAIL_LINES = 60
/** 缺省 cmake 可执行文件（工具链探测未给出绝对路径时按 PATH 解析）。 */
export const DEFAULT_CMAKE = "cmake"

// ── 预设（纯函数：设备 → llama.cpp CMake 定义） ───────────────────────────

/** 并行度缺省：CPU 核数的 75%（留出余量给其它进程），至少 1。 */
export function defaultJobs(cores: number = cpus().length): number {
  const n = Number.isFinite(cores) && cores > 0 ? cores : 1
  return Math.max(1, Math.floor(n * 0.75))
}

/** PATH 目录列表（env 未给 PATH 时回落到进程环境；分隔符按目标平台判定，便于跨平台断言）。 */
function pathDirs(env: Record<string, string> | undefined, platform: NodeJS.Platform): string[] {
  const raw = env?.PATH ?? env?.Path ?? (platform === process.platform ? (process.env.PATH ?? process.env.Path ?? "") : "")
  return String(raw).split(platform === "win32" ? ";" : ":").filter(Boolean)
}

/** PATH 上是否存在该可执行文件（只看文件、不执行——探测必须离线安全）。 */
export function hasOnPath(cmd: string, o: { env?: Record<string, string>; platform?: NodeJS.Platform } = {}): boolean {
  const platform = o.platform ?? process.platform
  const names = platform === "win32" ? [`${cmd}.exe`, `${cmd}.cmd`, `${cmd}.bat`, cmd] : [cmd]
  for (const dir of pathDirs(o.env, platform)) {
    for (const n of names) {
      try {
        if (statSync(join(dir, n)).isFile()) return true
      } catch {
        /* 不存在/不可读：继续找 */
      }
    }
  }
  return false
}

/**
 * 后端开关：**只开目标后端，其它显式 OFF**——避免因缺依赖（Vulkan SDK / HIP / CUDA）在 configure 阶段失败。
 * 顺序固定（cuda / vulkan / metal / hip），便于逐字段断言与人工核对。
 */
function backendSwitches(device: BuildDevice): string[] {
  const on = { cuda: false, vulkan: false, metal: false, hip: false }
  if (device === "cuda") on.cuda = true
  if (device === "vulkan") on.vulkan = true
  if (device === "metal") on.metal = true
  if (device === "rocm") on.hip = true
  const f = (v: boolean): string => (v ? "ON" : "OFF")
  return [`-DGGML_CUDA=${f(on.cuda)}`, `-DGGML_VULKAN=${f(on.vulkan)}`, `-DGGML_METAL=${f(on.metal)}`, `-DGGML_HIP=${f(on.hip)}`]
}

/** 预设选项（`ninja` 显式注入供测试与工具链探测结果使用；缺省按 PATH 探测，可用 `env` 注入探测基）。 */
export interface BuildPresetOptions {
  offline?: boolean
  platform?: NodeJS.Platform
  jobs?: number
  buildDir?: string
  sourceDir?: string
  ninja?: boolean
  /** ninja 探测用的环境（内网便携工具链不在进程 PATH 上时由调用方传入；缺省用进程环境）。 */
  env?: Record<string, string>
}

/**
 * 设备 → llama.cpp CMake 预设（纯函数，逐字段可断言）。
 *
 * 通用定义与实测取舍：
 *   * `-DLLAMA_CURL=OFF`——不引 libcurl（模型下载由 TS 层负责），内网/离线必需；
 *   * `-DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF`——只要 `llama-server`，省大量编译时间；
 *   * `-DGGML_NATIVE=ON`——CPU 档按构建机指令集优化（跨机分发改成 OFF 并显式指定变体）；
 *   * `offline` → `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`——FetchContent 不再尝试联网，
 *     缺依赖在 configure 阶段立刻报错（而不是挂网络直到超时）。
 */
export function buildPreset(device: BuildDevice, o: BuildPresetOptions = {}): BuildPreset {
  const platform = o.platform ?? process.platform
  const ninja = o.ninja ?? hasOnPath("ninja", { platform, env: o.env })
  const generator = ninja ? "Ninja" : platform === "win32" ? "MinGW Makefiles" : "Unix Makefiles"
  const jobs = o.jobs != null && Number.isFinite(o.jobs) && o.jobs > 0 ? Math.floor(o.jobs) : defaultJobs()

  const cmakeArgs: string[] = [
    "-DCMAKE_BUILD_TYPE=Release",
    "-DLLAMA_BUILD_TESTS=OFF",
    "-DLLAMA_BUILD_EXAMPLES=OFF",
    "-DLLAMA_BUILD_SERVER=ON",
    "-DLLAMA_CURL=OFF",
    ...(device === "cpu" ? ["-DGGML_NATIVE=ON"] : []),
    ...backendSwitches(device),
    // CUDA 算力：native 由 CMake 探测构建机（本机无 GPU 时不验证这条路径；交叉分发请显式 sm_XX 覆盖）
    ...(device === "cuda" ? ["-DCMAKE_CUDA_ARCHITECTURES=native"] : []),
    ...(o.offline ? ["-DFETCHCONTENT_FULLY_DISCONNECTED=ON"] : []),
  ]

  const requirements: string[] = [
    device === "cuda"
      ? "cmake ≥ 3.24（-DCMAKE_CUDA_ARCHITECTURES=native 需要；更早版本请显式传 sm_XX）"
      : "cmake ≥ 3.14（llama.cpp 最低要求）",
    ninja
      ? "ninja（生成器，已探测到）"
      : platform === "win32"
        ? "MinGW（mingw32-make）——未探测到 ninja 时的生成器"
        : "make（GNU make）——未探测到 ninja 时的生成器（装 ninja 通常明显更快）",
    platform === "win32"
      ? "MSVC（cl.exe，需先执行 vcvars64）或 MinGW gcc/g++"
      : platform === "darwin"
        ? "clang/clang++（Xcode Command Line Tools）"
        : "gcc/g++（或 clang/clang++）",
  ]
  if (device === "cuda") requirements.push("NVIDIA CUDA Toolkit（nvcc，建议与驱动匹配的 12.x）+ 可用驱动")
  if (device === "vulkan") requirements.push("Vulkan SDK（glslc 着色器编译器）+ 可用 Vulkan ICD（驱动）")
  if (device === "metal") requirements.push("macOS + Xcode Command Line Tools（Metal 工具链；非 darwin 无法构建）")
  if (device === "rocm") requirements.push("ROCm / HIP（hipcc + 运行时）")

  const notes: string[] = [
    "只开目标后端、其余后端显式 OFF：避免因缺 Vulkan SDK / HIP / CUDA 在 configure 阶段失败。",
    `生成器 ${generator}${ninja ? "（已探测到 ninja）" : "（未探测到 ninja，退回 Makefiles）"}；并行 ${jobs} 线程（cmake --build --parallel ${jobs}）。`,
    "不引 libcurl（-DLLAMA_CURL=OFF）：模型下载由 TS 层负责，编译期不需要网络栈。",
  ]
  if (device === "cpu") {
    notes.push("GGML_NATIVE=ON：按构建机 CPU 指令集优化；跨机分发请改 -DGGML_NATIVE=OFF 并显式指定指令集变体。")
  } else {
    notes.push("GGML_NATIVE 未显式设置（上游默认 ON）：交叉分发请用 extra_cmake_args 传 -DGGML_NATIVE=OFF。")
  }
  if (device === "cuda") {
    notes.push(
      "CUDA：-DCMAKE_CUDA_ARCHITECTURES=native 由 CMake 探测构建机算力；本机无 GPU / 交叉分发请用 extra_cmake_args 显式覆盖（如 -DCMAKE_CUDA_ARCHITECTURES=89——同名的 -D 以最后一条为准）。",
    )
    notes.push("Windows 上 nvcc 与 MSVC 版本有硬约束（VS2026 的 14.5x 工具集会被 nvcc 拒绝，需退回 14.44 或改用 clang-cl）。")
  }
  if (device === "vulkan") notes.push("Vulkan：首次构建要编译着色器（需要 glslc），只开 GGML_VULKAN。")
  if (device === "metal") notes.push("Metal：仅 darwin 可用；其它平台开启会在 configure/编译期失败。")
  if (device === "rocm") notes.push("ROCm：CMake 开关名是 GGML_HIP（不是 GGML_ROCM），需要 hipcc。")
  if (o.buildDir) notes.push(`构建目录 ${o.buildDir}（约定 build-<device>，与 infer/scripts/build-engine.ps1 一致）。`)
  if (o.sourceDir) notes.push(`源码目录 ${o.sourceDir}（就地编译，不改动源码树）。`)
  if (o.offline) {
    notes.push(
      "内网/离线：已加 -DFETCHCONTENT_FULLY_DISCONNECTED=ON——若报缺依赖，把对应依赖放进源码 vendor/ 下（内网预置）或改用离线完整源码包（含 FetchContent 的 _deps 目录）。",
    )
  }
  return { generator, cmakeArgs, requirements, notes }
}

// ── 命令组装（纯函数） ────────────────────────────────────────────────────

/** 需要引号的参数（空格/引号/重定向符等）：统一用双引号（POSIX shell 与 cmd/PowerShell 都认）。 */
function shellArg(s: string): string {
  const v = String(s)
  return /^[A-Za-z0-9_@%+=:.,/\-]+$/.test(v) ? v : `"${v.replace(/"/g, '\\"')}"`
}

/** configure 命令行（完整一条：`cmake -S <src> -B <build> -G <generator> -D…`）。 */
export function cmakeConfigureCmd(o: { cmake: string; sourceDir: string; buildDir: string; preset: BuildPreset }): string {
  const parts = [o.cmake, "-S", o.sourceDir, "-B", o.buildDir, "-G", o.preset.generator, ...o.preset.cmakeArgs]
  return parts.filter((p) => p != null && String(p) !== "").map(shellArg).join(" ")
}

/** build 命令行（完整一条：`cmake --build <build> --config Release --parallel <jobs>`）。 */
export function cmakeBuildCmd(o: { cmake: string; buildDir: string; jobs: number }): string {
  const jobs = Number.isFinite(o.jobs) && o.jobs > 0 ? Math.floor(o.jobs) : 1
  return [o.cmake, "--build", o.buildDir, "--config", "Release", "--parallel", String(jobs)].map(shellArg).join(" ")
}

// ── 与发现层（sourcefind）的弱耦合 ────────────────────────────────────────

/** 发现层的运行期形态：该层导出可能缺失/未就绪——一律按「未实现」降级，不崩。 */
interface SourcefindModule {
  ensureSourceDir?: (
    home: string,
    candidate: SourceCandidate,
    opts?: { ctx?: ToolContext; workDir?: string },
  ) => Promise<{ dir?: string; extracted?: boolean; error?: string }>
  detectToolchain?: (ctx?: ToolContext) => Promise<ToolchainInfo>
}

/**
 * 取发现层模块：**动态 import**（而非顶层静态 import）。
 * 发现层与本源解耦演进，其导出可能尚不存在或不完整：顶层 `import { ensureSourceDir } from "./sourcefind"`
 * 在 ESM 链接期就会整体报错（连测试文件都加载不了），动态 import 只会拿到 `undefined`，可安全降级到本地兜底。
 */
async function loadSourcefind(): Promise<SourcefindModule> {
  try {
    return ((await import("./sourcefind")) as unknown as SourcefindModule) ?? {}
  } catch {
    return {}
  }
}

function errText(e: unknown): string {
  return e instanceof Error ? e.message : String(e)
}

function statSafe(p: string): ReturnType<typeof statSync> | null {
  try {
    return statSync(p)
  } catch {
    return null
  }
}

/**
 * 拿到可编译的源码目录：优先发现层的 `ensureSourceDir`（归档解压/幂等复用由它负责）；
 * 它不可用（未实现/抛错）时本地兜底——候选本身就是带 CMakeLists.txt 的目录可直接编译，归档则给可操作错误。
 */
async function resolveSourceDir(
  home: string,
  candidate: SourceCandidate | undefined,
  ctx?: ToolContext,
): Promise<{ dir?: string; extracted?: boolean; error?: string; note?: string }> {
  const hints: string[] = []
  const sf = await loadSourcefind()
  if (typeof sf.ensureSourceDir === "function") {
    try {
      const r = await sf.ensureSourceDir(home, candidate as SourceCandidate, ctx ? { ctx } : undefined)
      if (r?.dir) return { dir: r.dir, extracted: !!r.extracted }
      if (r?.error) hints.push(`发现层：${r.error}`)
    } catch (e) {
      hints.push(`发现层 ensureSourceDir 不可用（${errText(e)}）`)
    }
  } else {
    hints.push("发现层 ensureSourceDir 尚未实现")
  }

  const cand = candidate as SourceCandidate | undefined
  if (!cand || typeof cand !== "object") {
    return { error: `源码候选为空：请先用发现层确认可编译源码（候选 id 或路径）。已尝试：${hints.join("；")}` }
  }
  const p = typeof cand.path === "string" ? cand.path : ""
  const st = p ? statSafe(p) : null
  if (st?.isDirectory() && existsSync(join(p, "CMakeLists.txt"))) {
    return { dir: p, extracted: false, note: hints.join("；") || undefined }
  }
  if (st?.isDirectory()) {
    return { error: `源码目录 ${p} 里没有 CMakeLists.txt——不是 CMake 工程（llama.cpp 需要）。请核对候选 ${cand.id ?? "(无 id)"}。` }
  }
  if (st?.isFile()) {
    return {
      error: `候选 ${cand.id ?? "(无 id)"} 是归档文件（${p}）：解压由发现层 ensureSourceDir 负责，当前不可用（${hints.join("；")}）——可先手工解压，再把目录路径交给编译安装。`,
    }
  }
  return { error: `源码路径不存在或不可用：${p || "(空)"}（${hints.join("；") || "候选无 path 字段"}）` }
}

/** 工具链探测：发现层提供就用它的（结果更全）；否则返回空，由最小探测兜底。 */
async function toolchainFor(ctx?: ToolContext): Promise<{ tool?: ToolchainInfo; via: string }> {
  const sf = await loadSourcefind()
  if (typeof sf.detectToolchain === "function") {
    try {
      const t = await sf.detectToolchain(ctx)
      if (t) return { tool: t, via: "发现层 detectToolchain" }
    } catch {
      /* 降级到最小探测 */
    }
  }
  return { via: "未探测（发现层 detectToolchain 不可用）" }
}

// ── 执行与日志 ────────────────────────────────────────────────────────────

interface RunResult {
  code: number
  stdout: string
  stderr: string
}

/**
 * 执行一条命令：有 ctx 走 `ctx.runCommand`（审批/沙箱/取消信号由引擎统一负责），
 * 否则本地 spawn 兜底（脚本/测试直调本层时仍可用）。**任何异常都转成非 0 + stderr**，不向外抛。
 */
async function runCmd(o: { ctx?: ToolContext; cmd: string; workdir: string; timeoutMs: number; env?: Record<string, string> }): Promise<RunResult> {
  if (o.ctx?.runCommand) {
    try {
      const r = await o.ctx.runCommand(o.cmd, { workdir: o.workdir, env: o.env, timeoutMs: o.timeoutMs })
      return { code: typeof r?.code === "number" ? r.code : 1, stdout: String(r?.stdout ?? ""), stderr: String(r?.stderr ?? "") }
    } catch (e) {
      return { code: 127, stdout: "", stderr: `命令执行失败：${errText(e)}` }
    }
  }
  try {
    const r = spawnSync(o.cmd, {
      shell: true,
      cwd: o.workdir,
      env: o.env ? { ...process.env, ...o.env } : process.env,
      timeout: o.timeoutMs,
      encoding: "utf-8",
      maxBuffer: 64 * 1024 * 1024,
      windowsHide: true,
    })
    return {
      code: r.status ?? (r.error ? 127 : 1),
      stdout: String(r.stdout ?? ""),
      stderr: String(r.stderr ?? (r.error ? r.error.message : "")),
    }
  } catch (e) {
    return { code: 127, stdout: "", stderr: `命令无法启动：${errText(e)}` }
  }
}

/** 追加写日志（日志本身失败不影响构建：它是观测手段，不是产物）。 */
function appendLog(logPath: string, text: string): void {
  try {
    mkdirSync(dirname(logPath), { recursive: true })
    appendFileSync(logPath, text, "utf-8")
  } catch {
    /* 忽略 */
  }
}

/** 日志分段头（含命令行原文，便于事后重放）。 */
function sectionHeader(phase: string, cmd: string): string {
  return `\n===== [${new Date().toISOString()}] ${phase} =====\n$ ${cmd}\n`
}

/** 跑一个阶段：计时 + 落盘（stdout/stderr 与退出码全进日志），返回结果与耗时。 */
async function runPhase(o: {
  ctx?: ToolContext
  cmd: string
  workdir: string
  timeoutMs: number
  env?: Record<string, string>
  logPath: string
  phase: string
}): Promise<RunResult & { secs: number }> {
  const t0 = Date.now()
  const r = await runCmd(o)
  const secs = Math.round((Date.now() - t0) / 100) / 10
  const body = [r.stdout, r.stderr].filter((s) => s && s.length).join("\n")
  appendLog(o.logPath, `${sectionHeader(o.phase, o.cmd)}${body ? `${body}\n` : ""}exit=${r.code}（${secs}s）\n`)
  return { ...r, secs }
}

/** 失败错误的统一组织：日志尾部（根因）+ 依赖清单（补什么）+ 内网提示。 */
function phaseError(phase: string, code: number, logPath: string, requirements: string[]): string {
  let tail = { text: "", truncated: false }
  try {
    tail = tailLines(logPath, ERROR_TAIL_LINES)
  } catch {
    /* 日志不可读：仍给出路径 */
  }
  return [
    `${phase} 失败（exit ${code}）——完整日志：${logPath}`,
    `日志尾部 ${ERROR_TAIL_LINES} 行${tail.truncated ? "（已截断）" : ""}：`,
    tail.text.trim() || "（日志为空——命令可能根本没跑起来，检查 cmake 是否在 PATH 上）",
    "",
    `依赖清单：${requirements.join("；")}`,
    "提示：离线/内网报缺依赖时，把对应依赖放进源码 vendor/ 下（内网预置）或改用完整源码包；构建重试前可先 clean 重配。",
  ].join("\n")
}

/**
 * 子进程环境：把 PATH 显式传给构建命令。
 *
 * 注意：**不要**从 `ToolchainInfo` 的字段里推路径——那些字段是「版本描述」（如 `"cmake version 3.28.0"`，
 * 由发现层的 `probeVersion` 写成），不是可执行文件路径；拿它当路径会让整条命令解析失败。
 * 工具链探测在本层的用途只有两个：就绪判定（门禁）与生成器选择（有无 ninja）。
 * PATH 基准优先取 `ctx.env`（引擎已按会话/用户收敛过），否则进程环境。
 */
function runEnv(ctx: ToolContext | undefined): Record<string, string> | undefined {
  const isWin = process.platform === "win32"
  const sep = isWin ? ";" : ":"
  const base = ctx?.env?.PATH ?? ctx?.env?.Path ?? process.env.PATH ?? process.env.Path ?? ""
  const entries = [...new Set(String(base).split(sep).filter(Boolean))]
  if (!entries.length) return undefined
  // 键名按平台惯例（Windows 是 `Path`）：与进程环境同名才能覆盖它——否则子进程会同时收到
  // `Path`（宿主）与 `PATH`（收敛后），而 Windows 环境变量名不区分大小写，取到哪个取决于实现。
  return { [isWin ? "Path" : "PATH"]: entries.join(sep) }
}

// ── 工具链门禁（缺什么先失败，不硬上） ────────────────────────────────────

interface GateResult {
  ok: boolean
  error?: string
  message: string
  missing: string[]
}

/** 设备所需的额外编译器探测（发现层的 detectToolchain 未实现时的最小门禁）。 */
function deviceProbes(device: BuildDevice): Array<[string, string]> {
  if (device === "cuda") return [["nvcc", "nvcc --version"]]
  if (device === "rocm") return [["hipcc", "hipcc --version"]]
  if (device === "vulkan") return [["glslc", "glslc --version"]]
  return []
}

/**
 * 工具链门禁：不满足就**先失败**并给出 requirements 清单（而不是编译到一半崩）。
 * 两条路径：① 发现层 detectToolchain 有结果 → 看 cmake 与 devices[device].ready；
 *          ② 只有 ctx → 最小探测（cmake --version + 设备编译器）。
 */
async function gateToolchain(o: {
  device: BuildDevice
  tool?: ToolchainInfo
  ctx?: ToolContext
  cmake: string
  preset: BuildPreset
  env?: Record<string, string>
  platform: NodeJS.Platform
}): Promise<GateResult> {
  const req = o.preset.requirements
  const missing: string[] = []

  if (o.tool) {
    if (!o.tool.cmake) missing.push("cmake")
    const dev = o.tool.devices?.[o.device]
    if (dev && dev.ready === false) missing.push(...(dev.missing ?? []).filter(Boolean))
    if (o.device === "metal" && o.platform !== "darwin") missing.push("macOS(darwin)")
    if (missing.length) {
      return {
        ok: false,
        missing,
        message: `工具链不满足（${o.device}）：缺 ${missing.join("、")}`,
        error: `工具链不满足：${o.device} 后端缺 ${missing.join("、")}。需要：${req.join("；")}。内网可先离线装齐再重试；无 GPU/无 CUDA 时改用 device="cpu"。`,
      }
    }
    const notes = o.tool.devices?.[o.device]?.notes ?? []
    return {
      ok: true,
      missing: [],
      message: `工具链就绪（${o.device}）：cmake=${o.tool.cmake}${o.tool.ninja ? ` · ninja=${o.tool.ninja}` : ""}${notes.length ? ` · ${notes.join("；")}` : ""}`,
    }
  }

  if (!o.ctx?.runCommand) {
    return {
      ok: true,
      missing: [],
      message: "未做工具链探测（无 ctx 且发现层未提供 detectToolchain）——cmake 按 PATH 解析，缺什么由 configure 日志给出。",
    }
  }

  const probes: Array<[string, string]> = [["cmake", `${o.cmake} --version`], ...deviceProbes(o.device)]
  const found: string[] = []
  for (const [name, cmd] of probes) {
    const r = await runCmd({ ctx: o.ctx, cmd, workdir: o.ctx.workdir ?? process.cwd(), timeoutMs: 30_000, env: o.env })
    if (r.code === 0) found.push(name)
    else missing.push(name)
  }
  if (o.device === "metal" && o.platform !== "darwin") missing.push("macOS(darwin)")
  if (missing.length) {
    return {
      ok: false,
      missing,
      message: `工具链不满足（${o.device}）：缺 ${missing.join("、")}`,
      error: `工具链不满足：${o.device} 后端缺 ${missing.join("、")}（已探测：${found.join("、") || "无"}）。需要：${req.join("；")}。内网可先离线装齐再重试；无 GPU/无 CUDA 时改用 device="cpu"。`,
    }
  }
  return { ok: true, missing: [], message: `工具链就绪（${o.device}）：已探测到 ${found.join("、")}` }
}

// ── 定位与安装 ────────────────────────────────────────────────────────────

/** 定位构建产物里的 llama-server：先递归找（Ninja 的 build/bin、MSVC 的 bin/Release 都在内），再兜底查 build/bin/。 */
export function locateBuiltServer(buildDir: string): string | null {
  try {
    const hit = findLlamaServer(buildDir)
    if (hit) return hit
  } catch {
    /* 遍历异常：走兜底 */
  }
  for (const n of ["llama-server", "llama-server.exe"]) {
    const p = join(buildDir, "bin", n)
    if (existsSync(p)) return p
  }
  return null
}

/** 构建系统产物/中间文件：不进安装目录（可执行文件所在目录通常只有运行时依赖，这里防「exe 落在构建根」时把整个构建树抄进去）。 */
const BUILD_JUNK_NAMES = new Set([
  "CMakeFiles",
  "CMakeCache.txt",
  "cmake_install.cmake",
  "build.ninja",
  "rules.ninja",
  "Makefile",
  "install_manifest.txt",
  ".ninja_log",
  ".ninja_deps",
  ".ninja_lock",
])
const BUILD_JUNK_RE = /\.(o|obj|a|lib|pdb|d|cmake)$/i

/**
 * 复制可执行文件所在目录的全部内容到 destDir——**同目录的 `*.so*` / `*.dll` 是运行时依赖，缺了跑不起来**。
 * 符号链接优先解引用（安装目录必须自包含、可整体搬走）；解引用失败（断链）退回按链接复制。
 */
function copyRuntimeDir(srcDir: string, destDir: string): { copied: number; failed: string[] } {
  const failed: string[] = []
  let copied = 0
  mkdirSync(destDir, { recursive: true })
  const walk = (from: string, to: string, depth: number): void => {
    if (depth > 4) return
    let entries: string[]
    try {
      entries = readdirSync(from)
    } catch {
      failed.push(from)
      return
    }
    for (const name of entries) {
      if (BUILD_JUNK_NAMES.has(name) || BUILD_JUNK_RE.test(name)) continue
      const s = join(from, name)
      const d = join(to, name)
      try {
        cpSync(s, d, { recursive: true, force: true, dereference: true })
        copied++
      } catch {
        try {
          cpSync(s, d, { recursive: true, force: true })
          copied++
        } catch {
          failed.push(s)
        }
      }
    }
  }
  walk(srcDir, destDir, 0)
  return { copied, failed }
}

/** 本地时间戳（日志名用：肉眼可读且可排序，与其它 local_infer 日志同一形态）。 */
function stamp(d = new Date()): string {
  const p = (n: number, w = 2): string => String(n).padStart(w, "0")
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`
}

/** 源码版本线索：发现层给的版本 > CMakeLists 的 project VERSION > "local-source"。 */
function sourceVersion(dir: string, candidate: SourceCandidate | undefined): string {
  const cv = candidate?.version
  if (typeof cv === "string" && cv.trim()) return cv.trim()
  try {
    const txt = readFileSync(join(dir, "CMakeLists.txt"), "utf-8")
    const m = txt.match(/project\s*\([^)]*?VERSION\s+([0-9][0-9A-Za-z.+\-]*)/i)
    if (m) return m[1]
  } catch {
    /* 读不到就用兜底值 */
  }
  return "local-source"
}

/** 源码 commit（有 .git 才给；支持分离头与 packed-refs，全失败则 undefined）。 */
function sourceCommit(dir: string): string | undefined {
  const sha = (s: string): string | undefined => {
    const t = s.trim()
    return /^[0-9a-f]{7,40}$/i.test(t) ? t.slice(0, 12) : undefined
  }
  try {
    const head = join(dir, ".git", "HEAD")
    if (!existsSync(head)) return undefined
    const raw = readFileSync(head, "utf-8")
    if (!/^ref:/.test(raw.trim())) return sha(raw)
    const ref = raw.trim().replace(/^ref:\s*/, "")
    const refFile = join(dir, ".git", ref)
    if (existsSync(refFile)) {
      const hit = sha(readFileSync(refFile, "utf-8"))
      if (hit) return hit
    }
    const packed = join(dir, ".git", "packed-refs")
    if (existsSync(packed)) {
      for (const line of readFileSync(packed, "utf-8").split("\n")) {
        const m = line.match(/^([0-9a-f]{7,40})\s+(.+)$/i)
        if (m && m[2].trim() === ref) return m[1].slice(0, 12)
      }
    }
    return undefined
  } catch {
    return undefined
  }
}

// ── 编译安装主流程 ────────────────────────────────────────────────────────

/**
 * 编译 + 安装（工具 source_build 的核心）。
 *
 * 步骤：① 幂等检查（已安装且未 clean → 直接返回）→ ② 源码目录（发现层，未实现则本地兜底，校验 CMakeLists.txt）
 * → ③ 工具链门禁（缺什么先失败，给 requirements）→ ④ configure（日志落盘 `bench/reports/build-<id>-<stamp>.log`）
 * → ⑤ build（失败把日志尾部 60 行放进 error）→ ⑥ 定位 llama-server → ⑦ 复制可执行文件所在目录到
 * `vendor/<id>/bin/`（同目录的 .so/.dll 是运行时依赖）+ 写 `.engine.json` → ⑧ 整体 rename 落位。
 *
 * 约定：除编程错误外**不抛错**（统一返回 `{ ok:false, error }`）；安装走阶段目录 + 整体 rename，
 * 失败清理阶段目录——`vendor/<id>/` 上永不出现半成品，也不会因为一次失败把已装好的引擎弄丢。
 */
export async function buildFromSource(o: {
  home: string
  /** SourceCandidate（sourcefind 的类型；此处按契约松散接收，运行时逐字段校验）。 */
  candidate: any
  spec: BuildSpec
  ctx?: ToolContext
  onStep?: (step: { phase: string; message: string; secs?: number }) => void | Promise<void>
}): Promise<BuildFromSourceResult> {
  const steps: BuildStep[] = []
  const emit = async (step: BuildStep): Promise<void> => {
    steps.push(step)
    try {
      await o.onStep?.(step)
    } catch {
      /* 回调异常不影响构建 */
    }
  }
  const home = String(o?.home ?? "")
  const spec = (o?.spec ?? {}) as BuildSpec
  const engineId = String(spec.engine_id ?? "").trim()
  const device = spec.device
  const fail = (error: string, extra: { buildDir?: string; logPath?: string } = {}): BuildFromSourceResult => ({
    ok: false,
    engineId: engineId || undefined,
    steps,
    error,
    ...extra,
  })

  // ① 参数校验（配置错误也走返回错误，不抛）
  if (!home) return fail("home 为空：无法确定 infer 子项目根（应由 LOCAL_INFER_HOME 或引擎注入）")
  if (!engineId) return fail("engine_id 为空：请指定要安装到的引擎 id（安装落点 vendor/<engine_id>）")
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(engineId)) {
    return fail(`engine_id 不合法：${engineId}——只允许字母/数字/._-（它同时是 vendor 下的目录名，不能含路径分隔符）`)
  }
  if (!BUILD_DEVICES.includes(device)) {
    return fail(`未知设备后端：${String(device)}——可选 ${BUILD_DEVICES.join(" / ")}`)
  }
  const jobs = spec.jobs != null && Number.isFinite(spec.jobs) && spec.jobs > 0 ? Math.floor(spec.jobs) : defaultJobs()
  const offline = !!spec.offline

  // ② 幂等：已安装（且未要求重建）直接返回——重复调用不重复 configure/build
  if (!spec.clean) {
    try {
      const cur = installedEngine(home, engineId)
      if (cur) {
        const b = (cur as SourceBuiltEngine).build
        await emit({
          phase: "done",
          message: `引擎已安装，跳过编译：${cur.exe}${b?.from_source ? `（源码构建 · ${b.device} · ${b.source_dir}）` : ""}；clean=true 可强制重建`,
        })
        return { ok: true, engineId, engine: { ...cur }, buildDir: b?.build_dir, logPath: b?.log, steps }
      }
    } catch {
      /* 标记损坏：按未安装继续 */
    }
  }

  // ③ 源码目录
  const candidate = o.candidate as SourceCandidate | undefined
  const src = await resolveSourceDir(home, candidate, o.ctx)
  if (!src.dir) return fail(`源码不可用：${src.error ?? "未知原因"}`)
  const sourceDir = src.dir
  if (!existsSync(join(sourceDir, "CMakeLists.txt"))) {
    return fail(`源码目录 ${sourceDir} 里没有 CMakeLists.txt——不是 CMake 工程（llama.cpp 需要）`)
  }
  await emit({
    phase: "extract",
    message: `源码就绪：${sourceDir}${src.extracted ? "（已解压归档）" : ""}${src.note ? `；${src.note}` : ""}`,
  })

  // ④ 工具链门禁
  const { tool, via } = await toolchainFor(o.ctx)
  const buildDir = join(sourceDir, `build-${device}`)
  const ninjaAvailable = tool ? !!tool.ninja : hasOnPath("ninja", { env: o.ctx?.env })
  const preset = buildPreset(device, {
    offline,
    jobs,
    buildDir,
    sourceDir,
    platform: process.platform,
    ninja: ninjaAvailable,
  })
  // cmake 可执行文件：优先用探测到的**绝对路径**（内网便携工具链可能不在默认 PATH 位置），否则按 PATH 解析
  const cmakeExe = tool?.paths?.cmake ?? DEFAULT_CMAKE
  const env = runEnv(o.ctx)
  const gate = await gateToolchain({ device, tool, ctx: o.ctx, cmake: cmakeExe, preset, env, platform: process.platform })
  if (!gate.ok) {
    await emit({ phase: "extract", message: gate.message })
    return fail(gate.error ?? `工具链不满足：${gate.missing.join("、")}`, { buildDir })
  }
  await emit({ phase: "extract", message: `${gate.message}；探测方式：${via}` })

  // ⑤ 构建目录与日志（clean 只在门禁通过后执行——工具链不满足时不动已有构建目录）
  if (spec.clean) {
    try {
      rmSync(buildDir, { recursive: true, force: true })
    } catch {
      /* 删不掉则按增量构建继续（cmake 自己会处理缓存） */
    }
  }
  const logPath = join(reportsDir(home), `build-${engineId.replace(/[^A-Za-z0-9._-]+/g, "-")}-${stamp()}.log`)
  const extraArgs = (spec.extra_cmake_args ?? []).map(String).filter((s) => s.trim())
  const effective: BuildPreset = { ...preset, cmakeArgs: [...preset.cmakeArgs, ...extraArgs] }
  const configureCmd = cmakeConfigureCmd({ cmake: cmakeExe, sourceDir, buildDir, preset: effective })
  const buildCmd = cmakeBuildCmd({ cmake: cmakeExe, buildDir, jobs })
  appendLog(
    logPath,
    [
      "===== local_infer 源码编译安装 =====",
      `引擎 ${engineId} · 设备 ${device} · 并行 ${jobs}${offline ? " · 离线模式" : ""} · 主机 ${process.platform}-${process.arch}`,
      `源码 ${sourceDir}`,
      `构建 ${buildDir}`,
      `依赖清单：${preset.requirements.join("；")}`,
      `说明：${preset.notes.join(" / ")}`,
      "",
    ].join("\n"),
  )

  // ⑥ configure
  await emit({ phase: "configure", message: `configure 开始（超时 ${CONFIGURE_TIMEOUT_MS / 1000}s）：${configureCmd}` })
  const c1 = await runPhase({ ctx: o.ctx, cmd: configureCmd, workdir: sourceDir, timeoutMs: CONFIGURE_TIMEOUT_MS, env, logPath, phase: "configure" })
  if (c1.code !== 0) {
    await emit({ phase: "configure", message: `configure 失败（exit ${c1.code}，${c1.secs}s）——见 ${logPath}`, secs: c1.secs })
    return fail(phaseError("configure", c1.code, logPath, preset.requirements), { buildDir, logPath })
  }
  await emit({ phase: "configure", message: `configure 完成（${c1.secs}s）`, secs: c1.secs })

  // ⑦ build
  await emit({ phase: "build", message: `build 开始（超时 ${BUILD_TIMEOUT_MS / 1000}s）：${buildCmd}` })
  const b1 = await runPhase({ ctx: o.ctx, cmd: buildCmd, workdir: sourceDir, timeoutMs: BUILD_TIMEOUT_MS, env, logPath, phase: "build" })
  if (b1.code !== 0) {
    await emit({ phase: "build", message: `build 失败（exit ${b1.code}，${b1.secs}s）——见 ${logPath}`, secs: b1.secs })
    return fail(phaseError("build", b1.code, logPath, preset.requirements), { buildDir, logPath })
  }
  await emit({ phase: "build", message: `build 完成（${b1.secs}s）`, secs: b1.secs })

  // ⑧ 定位可执行文件
  const exe = locateBuiltServer(buildDir)
  if (!exe) {
    const tail = (() => {
      try {
        return tailLines(logPath, ERROR_TAIL_LINES).text.trim()
      } catch {
        return ""
      }
    })()
    await emit({ phase: "build", message: `构建完成但未找到 llama-server（${buildDir}）`, secs: b1.secs })
    return fail(
      [
        `构建完成，但没找到 llama-server（已找遍 ${buildDir} 与 ${buildDir}/bin）。`,
        "可能原因：LLAMA_BUILD_SERVER 未生效、构建被中断、或生成器把产物放到了其它目录（可用 clean=true 重配）。",
        `完整日志：${logPath}；日志尾部：`,
        tail || "（日志为空）",
      ].join("\n"),
      { buildDir, logPath },
    )
  }

  // ⑨ 安装（阶段目录 → 整体 rename：vendor/<id> 上不出现半成品）
  const dir = engineDir(home, engineId)
  const stage = join(vendorDir(home), `.stage-${engineId}-${Math.random().toString(36).slice(2, 8)}`)
  const version = sourceVersion(sourceDir, candidate)
  const commit = sourceCommit(sourceDir)
  let copied = 0
  let marker: SourceBuiltEngine
  try {
    rmSync(stage, { recursive: true, force: true })
    mkdirSync(join(stage, "bin"), { recursive: true })
    const cp = copyRuntimeDir(dirname(exe), join(stage, "bin"))
    copied = cp.copied
    if (cp.failed.length) {
      throw new Error(`运行时依赖复制失败：${cp.failed.slice(0, 3).join("、")}${cp.failed.length > 3 ? ` 等 ${cp.failed.length} 项` : ""}`)
    }
    const exeName = basename(exe)
    const stagedExe = join(stage, "bin", exeName)
    if (!existsSync(stagedExe)) throw new Error(`复制后缺少可执行文件 ${exeName}（源 ${exe}）`)
    if (process.platform !== "win32") {
      try {
        chmodSync(stagedExe, 0o755) // 构建产物通常已可执行，这里兜底（某些文件系统/复制方式会丢执行位）
      } catch {
        /* 不支持 chmod 的文件系统：不影响以显式路径启动 */
      }
    }
    marker = {
      id: engineId,
      exe: join(dir, "bin", exeName),
      tag: version,
      asset: "(source build)",
      installed_at: new Date().toISOString(),
      dir,
      build: {
        from_source: true,
        source_dir: sourceDir,
        build_dir: buildDir,
        device,
        offline,
        log: logPath,
        host: `${process.platform}-${process.arch}`,
        jobs,
        generator: effective.generator,
        cmake: cmakeExe,
        configure_cmd: configureCmd,
        build_cmd: buildCmd,
        ...(commit ? { commit } : {}),
      },
    }
    writeFileSync(join(stage, ".engine.json"), `${JSON.stringify(marker, null, 2)}\n`, "utf-8")
    rmSync(dir, { recursive: true, force: true }) // 重装/换设备：旧目录整体替换
    renameSync(stage, dir)
  } catch (e) {
    try {
      rmSync(stage, { recursive: true, force: true }) // 半成品不落位
    } catch {
      /* 清理失败不覆盖主错误 */
    }
    return fail(`安装引擎 ${engineId} 失败：${errText(e)}（已清理临时目录；构建产物仍在 ${buildDir}）`, { buildDir, logPath })
  }

  await emit({ phase: "install", message: `已安装到 ${dir}（bin/ 内 ${copied} 项，含同目录运行时依赖）` })
  await emit({
    phase: "done",
    message: `引擎就绪：${engineId} → ${marker.exe}（${device}${offline ? " · 离线" : ""} · 源码 ${sourceDir}）`,
  })
  return { ok: true, engineId, engine: { ...marker }, buildDir, logPath, steps }
}
