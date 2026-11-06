/**
 * 霜天子代理工具集：把原生桌面应用当成"可编排的受控进程"来开发与验证。
 *
 * 分工：本文件是**操作面**（构建/启动/查询/操作/截图），`shuangtian/` 是**被操作面**（框架）。
 * 所有工具经 TCP 控制通道（`st-control/1`）与进程管理落地，不依赖宿主机桌面环境——
 * 无头模式（`--headless`）下 Linux 服务器无显示服务也能完整开发与验证界面。
 */
import { existsSync, readdirSync, statSync } from "node:fs"
import { tmpdir } from "node:os"
import { basename, dirname, isAbsolute, join } from "node:path"
import type { SubAgentDef, Tool, ToolContext, ToolResult } from "@gebai/sdk"
import { artifactBlocks, schema } from "@gebai/sdk/node"
import {
  ControlError,
  call_control,
  control_app_name,
  request,
  resolve_target,
  wait_for_event,
  type ControlResult,
  type ControlTarget,
} from "./shuangtian_client"

/** 框架默认根（`SHUANGTIAN_PROJECT` 未配置时）：歌白仓库根下的 `shuangtian/`。 */
const DEFAULT_FRAMEWORK_DIR = "shuangtian"
/** 运行期状态目录（控制文件/日志/截图），位于会话工作区。 */
const RUNTIME_DIR = ".shuangtian"
const DEFAULT_BUILD_TIMEOUT_MS = 600_000
/** 源码树产物目录：`build/<profile>/bin/`。 */
const SOURCE_BIN_SUBDIR = "build"
/** 构建档探测顺序：dev 主用；quick/release 是发行与快速验证档；debug 兜底。 */
const APP_PROFILES = ["dev", "quick", "release", "debug"] as const
/** 需要源码与 `st` 工具链的动作（二进制分发形态下直接拒绝）。 */
const BUILD_ACTIONS = new Set(["build", "test", "lint", "init"])

/**
 * 各应用的**外部资源**（需要与 exe 一起分发的那部分）。
 *
 * ## 为什么它们没被内嵌
 *
 * 图标/logo/角色图等静态资源已经用 `b::embed` **编进了 exe**（见 st.pkg 的
 * `embed` 声明），不需要源码。但下面这几项**故意不内嵌**：
 *
 * | 资源 | 为何保留为外部文件 |
 * |---|---|
 * | `xiangqi/ui`（main.xml + app.js） | 要能改 XML/JS 即重载（`--watch`） |
 * | `cefu/ui`（main.xml + app.js） | 同上（工具书条目也要能外改） |
 * | `jianmu/scenes`（*.js / *.py） | 三维脚本的核心工作流就是“改一行、画面就变” |
 * | `caiyun` 的 media/剧本 | 素材与剧本是用户内容 |
 *
 * 内嵌它们等于把热重载废掉，所以只能做外部文件。
 *
 * ## 为何记录“参数”而不是只记录路径
 *
 * 这四个应用都支持**显式指定资源目录**（`--ui` / `--dir` / `--script`）。
 * 这比让应用去猜相对路径可靠得多：工作目录一换，相对候选全失效。
 */
interface AppResources {
  /** 资源目录在框架树里的相对路径（也是分发时的推荐子目录名）。 */
  dir: string
  /** 启动时把它传给应用的参数。 */
  flag: string
  /** 目录内必须存在的标志文件（确认真的是那个资源目录）。 */
  marker: string
}

const APP_RESOURCES: Record<string, AppResources> = {
  xiangqi: { dir: "apps/xiangqi/ui", flag: "--ui", marker: "main.xml" },
  cefu: { dir: "apps/cefu/ui", flag: "--ui", marker: "main.xml" },
  jianmu: { dir: "apps/jianmu/scenes", flag: "--dir", marker: "01-product.js" },
}

/**
 * 为分发形态解析应用的**外部资源目录**，返回要追加的参数（找不到返回空）。
 *
 * 顺序：
 *   1. `<exe目录>/<资源子目录名>`（推荐摆法：`<exe目录>/ui` 或 `<exe目录>/scenes`）
 *   2. `<exe目录>/<框架内相对路径>`（保留完整目录结构时，如 `<exe目录>/apps/xiangqi/ui`）
 *   3. `framework` 根下的原位置（用户把整个框架树也放在旁边）
 *
 * 都找不到时**不报错也不乱猜**——让应用自己按它的相对候选去试，
 * 不行就由后面的启动日志报出原因（比工具凭空报错信息更多）。
 */
function resolveResourceArgs(target: string, binary: string, frameworkRoot: string): string[] {
  const spec = APP_RESOURCES[target]
  if (!spec) return []
  const exeDir = dirname(binary)
  const candidates = [
    join(exeDir, basename(spec.dir)),          // <exe>/ui 、<exe>/scenes
    join(exeDir, spec.dir),                    // <exe>/apps/xiangqi/ui（保留完整层级）
    join(frameworkRoot, spec.dir),             // 框架树原位置
  ]
  for (const dir of candidates) {
    if (existsSync(join(dir, spec.marker))) return [spec.flag, dir]
  }
  // 兜底：目录存在（即使标志文件不齐）也传给应用，让它的错报更准确
  for (const dir of candidates) {
    if (existsSync(dir)) return [spec.flag, dir]
  }
  return []
}

/**
 * 执行**形态**：手里是源码树，还是只有分发好的可执行文件。
 *
 * ## 为何要分形态（本工具集原先最强的隐含假设）
 *
 * 旧实现默认“手里一定有**源码树**”：`st.pkg` 必须在、`build/bin/st` 必须能自举、
 * 每次 `start` 都先跑一次 `st build`。但真实交付形态常常只有**构建好的可执行文件**：
 * 用户按需分发单个应用的 exe，机器上没有编译器、没有 `st`、也没有 `st.pkg`。
 * 那时旧实现卡在第一步：`未找到霜天框架（.../st.pkg 不存在）`，
 * 而真正想做的事（启动应用、截图、操控、看日志）**一个都不需要源码**。
 *
 * ## 与 `xiangqi-launch.ts` 同一套探测形状
 *
 * 仓里已有“按应用名找 exe”的成熟做法（源码构建目录 → GEBAI_HOME → PATH），
 * 本函数照它的形**重写一份适配多应用的**，而不是另立一套规矩——两套规矩必然漂移。
 * 差别只在于：那里固定找 `xiangqi`，这里要找**任意 target**。
 *
 * | 形态 | 判据 | 能做什么 |
 * |---|---|---|
 * | `source` | 有 `st.pkg`（能构建） | 全部（构建/测试/lint/启动/操控） |
 * | `binary` | 只找到 exe，无源码 | 启动/操控/截图/日志（**不能**构建/测试/lint） |
 * | `none` | 都没有 | 什么都不行——报清“按应用名找过哪些位置” |
 */
type DistMode = "source" | "binary" | "none"

interface DistShape {
  mode: DistMode
  /** 源码树根（binary 形态下由 exe 位置上溯推算，仅供日志可读性）。 */
  root: string
  /** 源码形态的 `st` 工具链（binary 形态为空串）。 */
  st: string
  /** 本次目标的可执行文件（未找到为空串）。 */
  binary: string
  /** 按顺序找过的全部候选（找不到时必须列全，只报一个会误导排查方向）。 */
  searched: string[]
}

/** 按平台拼可执行文件名。 */
function exeName(name: string): string {
  return IS_WINDOWS ? `${name}.exe` : name
}

/**
 * 探测执行形态，并按**应用名**定位可执行文件。
 *
 * ## 查找顺序（用户指定，不另设环境变量）
 *
 * 1. **源码构建目录**：`<framework>/build/<profile>/bin/<app>`
 * 2. **歌白可执行文件同级目录**：`dirname(process.execPath)`
 * 3. **`{GEBAI_HOME}/bin/`**
 * 4. **PATH**（`Bun.which`）
 * 5. **`framework` 参数指向的目录本身**（按需分发最常见的摆法）
 *
 * 每一层都**四个档位都试**（dev/quick/release/debug）——分发过来的 exe 不会告诉你它当初
 * 是哪个档编的，而源码树里常见的档位不止 dev。
 *
 * 形态判定：命中 ① ⇒ `source`；命中 ②③④⑤ ⇒ `binary`（没有源码就构建不了）；
 * 都没命中时，有 `st.pkg` 仍算 `source`（只是该 target 还没构建）。
 */
function detectShape(ctx: ToolContext, args: Json, target: string): DistShape {
  const root = frameworkDir(ctx, args)
  const st = stBinary(root)
  const candidates: string[] = []
  const push = (p: string): void => {
    if (!candidates.includes(p)) candidates.push(p)
  }

  // ① 源码构建目录（按档位优先；用户显式给的 profile 排最前）
  const preferred = asString(args, "profile")
  const profiles = preferred
    ? [preferred, ...APP_PROFILES.filter((p) => p !== preferred)]
    : [...APP_PROFILES]
  const sourceCandidates: string[] = []
  for (const profile of profiles) {
    const p = join(root, SOURCE_BIN_SUBDIR, profile, "bin", exeName(target))
    sourceCandidates.push(p)
    push(p)
  }

  // ② 歌白二进制同级目录（分发的自然位置：与 gebai 可执行文件放一起）
  const execDir = dirname(process.execPath)
  push(join(execDir, exeName(target)))

  // ③ {GEBAI_HOME}/bin/
  if (ctx.home) push(join(ctx.home, "bin", exeName(target)))

  // ④ PATH
  const onPath = Bun.which(exeName(target)) ?? null
  if (onPath !== null) push(onPath)

  // ⑤ framework 参数指向的目录本身
  //
  // 按需分发时用户自然这么做：“把 exe 放到一个目录，然后把 framework 指过去”。
  // 实测：第一版漏了这条，于是 `dist-test/jianmu.exe` 就摆在那里却报“未找到”
  // （因为只找了它下面的 `build/<档>/bin/`）——而错误信息里列出的候选路径
  // 看起来和用户看到的文件差得很远。
  push(join(root, exeName(target)))

  const sourceLike = existsSync(join(root, "st.pkg"))
  for (let i = 0; i < candidates.length; i++) {
    const candidate = candidates[i]
    if (!existsSync(candidate)) continue
    const fromSource = sourceCandidates.includes(candidate)
    return {
      mode: fromSource ? "source" : "binary",
      root,
      st,
      binary: candidate,
      searched: candidates,
    }
  }
  return { mode: sourceLike ? "source" : "none", root, st, binary: "", searched: candidates }
}

// ————————————————————————————————————————————————————————————————————
// 宿主平台差异（**集中在这里**，不撒到各处）
//
// 这些命令串最终交给 shell 执行：POSIX 走 `sh -c`，Windows 走 PowerShell。
// 两边至少有四处硬差异（实测在 Windows 上直接报“意外的标记 build”，工具完全不可用）：
//   ① 可执行文件名：`st` / `st.exe`；
//   ② 调用运算符：PowerShell 里 `"路径" 参数` 是**语法错误**，必须 `& "路径" 参数`；
//   ③ 自举脚本：`bootstrap.sh` / `bootstrap.ps1`；
//   ④ 进程管理：`setsid nohup … &` / `Start-Process -PassThru`，`kill -0` / `Get-Process`。
// ————————————————————————————————————————————————————————————————————
const IS_WINDOWS = process.platform === "win32"

/** 框架自带的 `st` 工具链可执行文件（Windows 上是 `st.exe`）。 */
function stBinary(root: string): string {
  const windows = join(root, "build/bin/st.exe")
  if (IS_WINDOWS) return windows
  return join(root, "build/bin/st")
}

/** 拼一条“调用可执行文件 + 参数”的命令串（Windows 需要 `&` 调用运算符）。 */
function invoke(binary: string, args: string): string {
  return IS_WINDOWS ? `& "${binary}" ${args}` : `"${binary}" ${args}`
}

/** 自举工具链的命令串（平台各自的脚本）。 */
async function bootstrapCommand(
  root: string,
  run: (command: string, options: { workdir: string; timeoutMs: number }) => Promise<{ code: number; stdout: string; stderr: string }>,
  timeoutMs: number,
): Promise<{ ok: boolean; text: string }> {
  const command = IS_WINDOWS ? `pwsh -NoProfile -File "${join(root, "bootstrap.ps1")}"` : `"${join(root, "bootstrap.sh")}"`
  const result = await run(command, { workdir: root, timeoutMs })
  return { ok: result.code === 0, text: `${result.stdout}${result.stderr}`.trim().slice(-2000) }
}

/** 构建产物路径（Windows 带 `.exe`）：覆盖工具链与宿主平台两种拼接。 */
function appBinary(appDir: string, buildSubdir: string, target: string): string {
  const base = join(appDir, `build/${buildSubdir}/bin/${target}`)
  return IS_WINDOWS ? `${base}.exe` : base
}

type Json = Record<string, unknown>

function asString(args: Json, key: string, fallback = ""): string {
  const value = args[key]
  return typeof value === "string" ? value : fallback
}

function asNumber(args: Json, key: string, fallback: number): number {
  const value = args[key]
  if (typeof value === "number" && Number.isFinite(value)) return value
  if (typeof value === "string" && value.trim() !== "") {
    const parsed = Number(value)
    if (Number.isFinite(parsed)) return parsed
  }
  return fallback
}

function asBool(args: Json, key: string, fallback = false): boolean {
  const value = args[key]
  if (typeof value === "boolean") return value
  if (typeof value === "string") return ["1", "true", "yes", "on"].includes(value.toLowerCase())
  return fallback
}

/**
 * 框架根：显式参数 > 环境变量 > 仓库默认位置 > GEBAI_HOME 的随包分发位置。
 *
 * ⚠ **不再要求必须有 `st.pkg`**：那是**源码形态**的特征。纯二进制分发时
 * 目录里只有 `bin/<app>`，用它本来就不需要 `st.pkg`（不需要构建）。
 * 判据改为“这里有东西”——`st.pkg`、`bin/`、或 `build/` 任一存在即认为是框架/分发根；
 * 全都没有才如实报“找不到”（错误信息里会列出找过哪些位置）。
 */
function frameworkDir(ctx: ToolContext, args: Json): string {
  const explicit = asString(args, "framework") || String(ctx.env.SHUANGTIAN_PROJECT ?? "").trim()
  if (explicit) return ctx.resolvePath(explicit)
  const looksLikeRoot = (dir: string): boolean =>
    existsSync(join(dir, "st.pkg")) || existsSync(join(dir, "build")) || existsSync(join(dir, "bin"))
  const candidate = ctx.resolvePath(DEFAULT_FRAMEWORK_DIR)
  if (looksLikeRoot(candidate)) return candidate
  // 兜底：GEBAI_HOME 下的随包分发位置（二进制形态也允许只是 `bin/`）
  const vendored = join(ctx.home, "vendor", "shuangtian")
  return looksLikeRoot(vendored) ? vendored : candidate
}

/**
 * 工程根（含 `st.pkg`）——被构建/运行/测试的**那个工程**。
 *
 * 与 `framework` 的区别：`framework` 是"提供框架与工具链的霜天仓库"，
 * `project` 是"你要构建的应用"。独立工程通过清单里的 `framework.path` 引用框架，
 * 因此这两个根**可以是不同目录**（歌白全程负责独立项目时就是如此）。
 * 不传 `project` 时退化为框架根本身（框架自身的示例/测试即此情形）。
 */
function projectDir(ctx: ToolContext, args: Json): string {
  const explicit = asString(args, "project") || String(ctx.env.SHUANGTIAN_APP ?? "").trim()
  if (explicit) return ctx.resolvePath(explicit)
  return frameworkDir(ctx, args)
}

/** 在工程目录里找清单（工程根可能直接给的就是清单目录）。 */
function projectManifest(ctx: ToolContext, args: Json): string {
  return join(projectDir(ctx, args), "st.pkg")
}

function runtimeDir(ctx: ToolContext): string {
  return ctx.resolvePath(RUNTIME_DIR)
}

function controlFileFor(ctx: ToolContext, target: string): string {
  return join(runtimeDir(ctx), `${target}-control.json`)
}

function logFileFor(ctx: ToolContext, target: string): string {
  return join(runtimeDir(ctx), `${target}.log`)
}

/** 控制文件里我们真正需要的三个字段（其余字段由应用自用）。 */
interface ControlInfo {
  port: number
  pid: number
  token: string
}

/**
 * 读控制文件（形状校验）；不存在/损坏/正在写入返回 `null`。
 *
 * 单独抽出来的理由：控制文件是**跨进程的共享状态**，它的"陈旧"是本工具最隐蔽的故障源
 * （见 `start` 里的长注释）——读它的地方必须用**同一套**判定。
 */
async function readControlFile(path: string): Promise<ControlInfo | null> {
  if (!existsSync(path)) return null
  try {
    const info = (await Bun.file(path).json()) as { port?: unknown; pid?: unknown; token?: unknown }
    return {
      port: typeof info.port === "number" && info.port > 0 ? info.port : 0,
      pid: typeof info.pid === "number" && info.pid > 0 ? info.pid : 0,
      token: typeof info.token === "string" ? info.token : "",
    }
  } catch {
    return null  // 写了一半/内容损坏：按"不可用"处理（不要把半截 JSON 当端口用）
  }
}

/**
 * 解析截图落盘路径。
 *
 * 两道约束都要满足：
 * ① 工具契约是“返回的路径文件系统里可直接读”，所以**相对路径必须按会话目录解析**；
 * ② 服务端有**落盘白名单**（temp / 可执行文件目录 / 控制文件所在目录），
 *    且是**前缀比较不递归**——控制文件在 `<会话>/.shuangtian/`，所以“会话目录下”
 *    这个概念只有 `.shuangtian` 子树真的可写。
 *
 * 自然写法（如 `qa/shot.png`）按 ① 解析出来、却过不了 ②。与其让调用方在一条
 * “路径不在白名单”的报错里自己拼出可行的绝对路径，不如在工具层就把它落到
 * **控制文件目录里的 shots/**（也是 start 内定并已通过 `--shots` 告知应用的位置）。
 */
function resolveShotPath(ctx: ToolContext, requested: string): string {
  const shots = join(runtimeDir(ctx), "shots")
  if (requested === "") return join(shots, `shot-${Date.now()}.png`)
  const resolved = ctx.resolvePath(requested)
  const runtime = runtimeDir(ctx)
  const folded = (value: string): string => value.replace(/\\/g, "/")
  const inRuntime = folded(resolved).startsWith(`${folded(runtime)}/`)
  const inTemp = folded(resolved).startsWith(`${folded(tmpdir()).replace(/\/$/, "")}/`)
  // ⚠ “是否绝对路径”不能用 `resolved.includes("/")` 判：Windows 的 `C:\Users\x.png`
  // 不含正斜杠，会被当成相对路径 → 原样返回、白名单重写整段被跳过
  // （实测：Windows 上这条用例恒红，而 POSIX 下绿——只在 CI 跑 Linux 时看不见）。
  // 用 isAbsolute 才是跨平台口径；它同时也覆盖 POSIX 的 `/…`。
  const is_absolute = isAbsolute(resolved) || folded(resolved).startsWith("/")
  if (inRuntime || inTemp || !is_absolute) return resolved
  return join(shots, basename(resolved))
}

/** 文件修改时间（毫秒）；取不到返回 0。 */
function mtimeMs(path: string): number {
  try {
    return statSync(path).mtimeMs
  } catch {
    return 0
  }
}

/** 端口上是否真有控制通道在应答（`ping` 不需 token，协议门允许探测）。 */
async function controlResponsive(port: number, timeout_ms = 1500): Promise<boolean> {
  if (port <= 0) return false
  try {
    await request({ host: "127.0.0.1", port }, "ping", {}, timeout_ms)
    return true
  } catch {
    return false
  }
}

/** 目标名 → 进程名比对（去路径与 `.exe`）；`comm` 在 Linux 上截断到 15 字符，故前缀也算匹配。 */
function processMatchesTarget(name: string, target: string): boolean {
  if (!name) return false
  const base = (target.split(/[\\/]/).pop() ?? target).replace(/\.exe$/i, "").toLowerCase()
  if (!base) return true
  const actual = name.toLowerCase()
  return actual.includes(base) || base.startsWith(actual)
}

/** 结束进程（stop 与启动超时回收共用；Windows 用 Stop-Process，POSIX 先 TERM 后 KILL）。 */
async function terminateProcess(ctx: ToolContext, pid: number): Promise<void> {
  if (pid <= 0) return
  if (IS_WINDOWS) {
    await ctx.runCommand(`Stop-Process -Id ${pid} -Force -ErrorAction SilentlyContinue`, { timeoutMs: 8000 })
  } else {
    await ctx.runCommand(
      `kill -TERM ${pid} 2>/dev/null; sleep 0.3; kill -0 ${pid} 2>/dev/null && kill -KILL ${pid} 2>/dev/null; true`,
      { timeoutMs: 8000 },
    )
  }
}

/** 取进程名（PID 复用防护：应用崩溃后 PID 可能被系统分配给无关进程）；拿不到返回空串。 */
async function processName(ctx: ToolContext, pid: number): Promise<string> {
  if (pid <= 0) return ""
  const result = IS_WINDOWS
    ? await ctx.runCommand(`(Get-Process -Id ${pid} -ErrorAction SilentlyContinue).ProcessName`, { timeoutMs: 5000 })
    : await ctx.runCommand(`ps -o comm= -p ${pid} 2>/dev/null`, { timeoutMs: 5000 })
  return result.stdout.trim()
}

/**
 * 解析控制目标：显式 `target`（`host:port` / 控制文件路径 / 纯端口）> 环境变量 >
 * 会话运行期控制文件（由 `run` 的 start 动作写入）。
 */
/** 在运行期目录里找目标对应的控制文件（精确名优先，其次扫描唯一的控制文件）。 */
function resolveControlFileSync(ctx: ToolContext, target: string): string {
  const dir = runtimeDir(ctx)
  const exact = join(dir, `${target}-control.json`)
  if (existsSync(exact)) return exact
  return scanControlFilesSync(ctx).find((file) => control_app_name(file) === target.toLowerCase()) ?? ""
}

/**
 * 扫描运行期目录里的全部控制文件（同步；只读目录，无副作用）。
 *
 * 为何不用 `ctx.listDir`：它是异步的，而 `controlTarget` 的角色是多处要拿
 * “当前目标是哪个”的地方同步调用。同步实现也让 `apps` 与寻址走同一份真源。
 */
function scanControlFilesSync(ctx: ToolContext): string[] {
  try {
    return readdirSync(runtimeDir(ctx))
      .filter((name) => name.endsWith("-control.json"))
      .map((name) => join(runtimeDir(ctx), name))
      .sort()
  } catch {
    return []  // 目录不存在 → 无实例
  }
}

async function controlTarget(ctx: ToolContext, args: Json, target?: string): Promise<ControlTarget> {
  const explicit = target ?? asString(args, "target")
  if (explicit) return resolve_target(explicit, scanControlFilesSync(ctx))
  const fromEnv = String(ctx.env.SHUANGTIAN_TARGET ?? "").trim()
  if (fromEnv) return resolve_target(fromEnv, scanControlFilesSync(ctx))

  // 发现顺序：① 调用方显式给了 app 名 → 就用它；② 否则 **扫描全部控制文件**。
  //
  // 为何要扫描：本工具曾只试 `mdeditor` / `gallery` 两个名字，于是
  // `run(target="gbcode")` 启动的实例**永远发现不了**——省略 target 时直接报
  // “未找到运行中的应用”，而唯一的实例就在那儿。配合下方提示词推荐的
  // `target="host:port"` 写法也不带 token（被握手拒），工具于是“看起来完全不可用”：
  // 调用方只能改为手写 TCP 客户端，把 token/帧/结构重新踩一遍。
  //
  // 有实例时的选择规则：**唯一存活的那个直接用**（最常见情形，省钱省往返）；
  // 多个存活则列出来要求指定——静默猜错落到另一个应用上比报错危险得多。
  const candidates: Array<{ file: string; info: ControlInfo; app: string }> = []
  for (const file of scanControlFilesSync(ctx)) {
    const info = await readControlFile(file)
    if (info && info.port > 0) candidates.push({ file, info, app: control_app_name(file) })
  }
  const requestedApp = asString(args, "app").trim()
  const requestedFile = requestedApp ? resolveControlFileSync(ctx, requestedApp) : ""
  const ordered = requestedFile
    ? [...candidates.filter((item) => item.file === requestedFile), ...candidates.filter((item) => item.file !== requestedFile)]
    : candidates

  const alive: typeof candidates = []
  const stale: string[] = []
  for (const item of ordered) {
    // **先判活再连**：应用退出/崩溃后控制文件会滞留，连上去只能得到一条
    // “连接被关闭”的传输错误。而这条错误里真正该说清的是
    // “这个实例已经不在了，去重新 start”——处理成一句可行动的话。
    if (item.info.pid > 0 && !processMatchesTarget(await processName(ctx, item.info.pid), item.app)) {
      stale.push(item.app)
      continue
    }
    alive.push(item)
  }
  if (alive.length === 1 || (alive.length > 1 && requestedFile && alive[0]?.file === requestedFile)) {
    return resolve_target(alive[0].file)
  }
  if (alive.length === 0) {
    throw new ControlError(
      "not_found",
      stale.length > 0
        ? `${stale.join("、")} 的实例已不在运行（控制文件滞留）：run(action=stop) 清理后重新 start；` +
          `或显式传 target="<控制文件路径>"。运行期目录：${runtimeDir(ctx)}`
        : `未找到运行中的应用：请先 shuangtian_run（action=start）启动，或用 target 参数指定 ` +
          `控制文件路径 / "host:port"；也可设置环境变量 SHUANGTIAN_TARGET（当前会话目录：${runtimeDir(ctx)}）`,
    )
  }
  throw new ControlError(
    "ambiguous",
    `有 ${alive.length} 个运行中的应用（${alive.map((item) => item.app).join("、")}）：` +
      `请用 target="<控制文件路径>" 或 app="<名字>" 指定要看哪一个`,
  )
}

/** 统一调用入口：把控制通道错误转成可读文本（工具不抛异常，错误经 output 反馈）。 */
async function controlCall(
  ctx: ToolContext,
  args: Json,
  method: string,
  params: Json = {},
  timeout_ms = 15_000,
  target?: string,
): Promise<{ ok: boolean; text: string; data?: unknown }> {
  try {
    const resolved = await controlTarget(ctx, args, target)
    // 注意：`request()` 内部已做 ok/error 解包（成功返回 result，失败抛 ControlError）。
    // 这里不要再 `unwrap` 一次——对已解包的结果二次解包会把成功误判为失败。
    const result = await request<Json>(resolved, method, params, timeout_ms)
    return { ok: true, text: JSON.stringify(result, null, 2), data: result }
  } catch (error) {
    const message = error instanceof ControlError ? `${error.code}: ${error.message}` : String(error)
    return { ok: false, text: `控制通道调用失败（${method}）：${message}` }
  }
}

/** 便捷构造只读工具。 */
function readTool(
  name: string,
  description: string,
  parameters: ReturnType<typeof schema>,
  run: (args: Json, ctx: ToolContext) => Promise<ToolResult>,
  outputSchema?: ReturnType<typeof schema>,
): Tool {
  return { name, description, parameters, outputSchema, execute: run }
}

// ————————————————————————————————————————————————————————————————————————————
// run：构建 / 测试 / lint / 启动 / 停止 / 状态 / 日志（框架生命周期）
// —————————————————————————————————————————————————————————————————————————————

/// 启动命令的等待上限（**不是应用就绪的上限**，那个另有 30s 轮询）。
///
/// 为何要短：常驻子进程会把祖先的 stdout 管道一路继承下来（Windows 句柄继承是
/// 传递的），`runCommand` 等 `close` 就永远等不到。3s 足够 PowerShell 完成 spawn
/// （实测 `Start-Process` 本身 205ms），又不白等。超时后照常轮询控制文件即可。
const kLaunchTimeoutMs = 3_000

const runTool: Tool = {
  name: "run",
  description:
    "构建与运行霜天应用（框架生命周期入口）。**先看 action=shape**：它只探测不执行，告诉你当前是源码树还是只有分发的 exe、目标在哪、能不能构建。action=build 构建（自动自举 stpm 工具链）；action=test 跑框架单元测试（可选 san=true 开 ASan/UBSan）；action=lint 跑禁用特性静态扫描；action=start 启动应用并等待控制通道**验证就绪**（文件新鲜 + 握手成功才算；返回 port 与 PID；已有存活实例时明确拒绝，先 stop）。**默认以无头模式启动**（开发/回归用）；需要**真实窗口**让用户亲手操作（鼠标缩放/拖动/键盘）时传 window=true——那时不注入 --headless，后端会自动落到 win32/x11 真窗口（无显示服务时框架自己回退无头）。action=stop 结束进程；action=status 查看进程与端口；action=logs 取运行日志尾部。target 为应用名（gallery/mdeditor 或自带工程的目标名），profile 为构建档（dev/debug/release/san）。**按需分发形态**（只有 exe、无源码）下：build/test/lint 会被明确拒绝，start 直接跑已有 exe，exe 按 源码构建目录 → 歌白 exe 同级 → {GEBAI_HOME}/bin → PATH → framework 目录 依次查找。",
  parameters: schema(
    {
      action: {
        type: "string",
        enum: ["init", "build", "test", "lint", "start", "stop", "status", "logs", "shape"],
        description:
          "动作（init 新建独立工程；其余作用于 project 或框架自身）。shape = **只探测不执行**：报告这个根是源码形态还是纯二进制分发、找过哪些位置、能做什么不能做什么（无法用错动作时先查它）",
      },
      target: { type: "string", description: "应用/目标名（默认 mdeditor）" },
      profile: { type: "string", description: "构建档：dev（默认，-O1）/debug/release/san" },
      toolchain: {
        type: "string",
        description:
          "交叉编译工具链名（在 st.pkg 的 toolchains 段声明，如 mingw）。产物落在 build/<档位>-<工具链>/bin/，Windows 目标带 .exe；产物无法在本机执行（action=start 不适用）",
      },
      framework: { type: "string", description: "框架工程根（默认仓库根 shuangtian/，可用 SHUANGTIAN_PROJECT 覆盖）" },
      project: {
        type: "string",
        description:
          "被操作的应用工程根（含 st.pkg）。独立工程与框架是不同目录，用本参数指定；不传则操作框架自身（可用 SHUANGTIAN_APP 覆盖）",
      },
      bin_dir: {
        type: "string",
        description:
          "（已废弃，保留兼容）应用 exe 的搜索已改为固定顺序：源码构建目录 → 歌白可执行文件同级目录 → {GEBAI_HOME}/bin → PATH。通常不需要本参数",
      },
      name: { type: "string", description: "action=init 时的工程名（缺省取目录名）" },
      args: { type: "string", description: "start 时附加命令行参数（如 --demo-stream）" },
      window: {
        type: "boolean",
        description:
          "start 时为 true 则不注入 --headless，开启**真实窗口**（用户可亲手用鼠标缩放/拖动/键盘）。用于「让用户实际上手」的场景；自动化回归仍用默认无头（截图与控制通道两者都能用，但无头不依赖桌面环境）",
      },
      scale: { type: "number", description: "DPI 缩放（start 时透传 --scale，2.0 = 200%）" },
      theme: { type: "string", enum: ["light", "dark"], description: "主题" },
      timeout_ms: { type: "number", description: "构建/测试超时（默认 600000）" },
    },
    ["action"],
  ),
  outputSchema: schema(
    {
      ok: { type: "boolean" },
      action: { type: "string" },
      target: { type: "string" },
      pid: { type: "number" },
      port: { type: "number" },
      control_file: { type: "string" },
      log_file: { type: "string" },
      output: { type: "string", description: "命令输出尾部（构建/测试/lint）" },
    },
    ["ok", "action"],
  ),
  execute: async (args, ctx) => {
    const action = asString(args, "action", "build")
    const target = asString(args, "target", "mdeditor")
    const profile = asString(args, "profile", action === "start" ? "dev" : "dev")
    const toolchain = asString(args, "toolchain")
    const toolchainFlag = toolchain ? ` --toolchain=${toolchain}` : ""
    // 交叉编译产物目录带工具链后缀（与 stpm 的 profile_directory 规则一致）
    const buildSubdir = toolchain ? `${profile}-${toolchain}` : profile
    const root = frameworkDir(ctx, args as Json)
    const timeoutMs = asNumber(args as Json, "timeout_ms", DEFAULT_BUILD_TIMEOUT_MS)
    // ## 形态探测（源码 / 纯二进制分发 / 都没有）
    //
    // 必须在**前置校验**之前做：旧实现开头就查 `st.pkg`，于是纯二进制分发
    // 连 `start`（最不需要源码的那个动作）都过不去。
    const shape = detectShape(ctx, args as Json, target)

    // ## `shape`：只探测不执行
    //
    // 它存在的理由：调用方经常不知道该用 build 还是直接 start。猜错的代价是一轮
    // 无用的失败（甚至误以为“环境坏了”）。先问一句就能得到“这里是源码还是分发、
    // 目标 exe 在哪、能不能构建”的**确定答案**。
    if (action === "shape") {
      const lines: string[] = []
      lines.push(`执行形态：${shape.mode}`)
      lines.push(
        `  源码树根：${shape.root}${existsSync(join(shape.root, "st.pkg")) ? "（有 st.pkg）" : "（无 st.pkg）"}`,
      )
      lines.push(
        `  ${IS_WINDOWS ? "st.exe" : "st"} 工具链：${existsSync(shape.st) ? shape.st : "无（不能构建）"}`,
      )
      lines.push(
        `  目标 ${target}：${shape.binary || "未找到"}` +
          (shape.binary && shape.mode === "binary" ? "（纯二进制分发）" : ""),
      )
      const can = shape.mode === "source"
      lines.push("")
      lines.push(`  能做的：action=start / stop / status / logs（启动与操控永远不需要源码）`)
      lines.push(
        `  ${can ? "构建/测试/lint：可（有源码与 st 工具链）" : "构建/测试/lint：不可（纯二进制分发，无源码）"}`,
      )
      lines.push("")
      lines.push("  搜索顺序（命中即止）：")
      for (const p of shape.searched) lines.push(`    · ${p}`)
      return {
        output: lines.join("\n"),
        data: {
          ok: shape.mode !== "none",
          action,
          target,
          mode: shape.mode,
          binary: shape.binary,
          searched: shape.searched,
        },
      }
    }

    if (shape.mode === "none") {
      return {
        output:
          `未找到应用 \`${target}\`：既没有源码树（${root}），分发目录里也没有它。\n` +
          `  按应用名找过：${shape.searched.map((p) => `\n    · ${p}`).join("")}\n` +
          `\n  两种用法：\n` +
          `    · 源码形态：把 framework 指向含 st.pkg 的霜天仓库，先 action=build\n` +
          `    · 按需分发：把 ${exeName(target)} 放到任一处即可直接 action=start（**不需要构建**）：\n` +
          `        — 歌白可执行文件同级目录（${dirname(process.execPath)}）\n` +
          `        — ${join(ctx.home, "bin")}\n` +
          `        — 或放进 PATH`,
        data: { ok: false, action, target, mode: shape.mode, searched: shape.searched },
      }
    }
    // 构建类动作必须有源码（binary 形态下没有 st 工具链）
    //
    // `build` 也算进来：既然没有源码，就不存在“构建”这件事。把 build 做成静默
    // “跳过”会让人以为“改了源码并构建了”（而实际跑的还是旧 exe）——
    // 那种误会在真实开发里要花几轮才查得清。宁可明确拒绝，并指出“直接 start”。
    if (shape.mode === "binary" && BUILD_ACTIONS.has(action)) {
      return {
        output:
          `当前是**纯二进制分发**形态（${shape.root}），没有 ${IS_WINDOWS ? "st.exe" : "st"} 工具链与源码，无法执行 action=${action}。\n` +
          `  可用：action=start / stop / status / logs（启动与操控不需要源码）\n` +
          `  已定位到可执行文件：${shape.binary}\n` +
          `  想用源码构建：用 framework 参数指向含 st.pkg 的霜天仓库`,
        data: { ok: false, action, target, mode: shape.mode },
      }
    }
    // 工程清单只在**构建/测试类**动作里有意义：纯分发根本不构建，
    // 也就不存在“工程根”这回事（应用已经在 bin/ 里了）。
    if (
      shape.mode === "source" &&
      action !== "init" &&
      !existsSync(projectManifest(ctx, args as Json))
    ) {
      return {
        output:
          `未找到工程清单（${projectManifest(ctx, args as Json)}）：` +
          `用 project 参数指定工程根；若还没有工程，先用 action=init 创建`,
      }
    }

      const ensureToolchain = async (): Promise<{ ok: boolean; text: string }> => {
    const st = stBinary(root)
    if (existsSync(st)) return { ok: true, text: "已有 st" }
    return bootstrapCommand(root, ctx.runCommand, timeoutMs)
  }

    if (action === "init") {
      // 生成新的独立工程（歌白全链路的起点）。`project` 是目标目录。
      const directory = asString(args, "project")
      if (!directory) return { output: "action=init 需要 project 参数（目标目录）" }
      const absolute = ctx.resolvePath(directory)
      const name = asString(args, "name") || basename(absolute)
          const st = stBinary(root)
    if (!existsSync(st)) {
      const boot = await bootstrapCommand(root, ctx.runCommand, timeoutMs)
      if (!boot.ok) return { output: `引导工具链失败：\n${boot.text}` }
    }
    const created = await ctx.runCommand(
      invoke(st, `init "${absolute}" --name ${name} --framework "${root}"`),
      { timeoutMs },
    )
      const text = `${created.stdout}${created.stderr}`.trim()
      if (created.code !== 0) return { output: `创建工程失败（exit ${created.code}）：\n${text}` }
      return {
        output: `${text}\n\n下一步：写 src/*.cpp（用文件工具），再 run(action=build, project="${absolute}")`,
        data: { ok: true, action, project: absolute, name },
      }
    }

    if (action === "build" || action === "start") {
      // ## 纯二进制分发：**跳过构建**，直接用已有的可执行文件
      //
      // 这是整个“脱离源码”改造的关键分支。旧实现无条件走 `st build`，
      // 于是一个只需要“把应用跑起来”的请求在二进制形态下会卡在“引导工具链失败”。
      //
      // 跳过构建是有意为之的语义，而不是“容错”：用户手动分发的二进制就是**当前产物**，
      // 重建它既不可能（没编译器）也不应该（可能覆盖掉用户验过的版本）。
      const prebuilt = shape.mode === "binary"
      let build_ms = 0
      let buildTail = ""
      if (!prebuilt) {
        // 变量名不要叫 toolchain：那会遮蔽上面的「交叉编译工具链名」参数
        // （实测导致 `--toolchain` 变成 `[object Object]` 传下去）
        const bootstrap = await ensureToolchain()
        if (!bootstrap.ok) return { output: `引导工具链失败：\n${bootstrap.text}` }
        // 被构建的是**工程**（可能与框架不同目录）；`st` 始终用框架自己的那份
        const build_started_at = Date.now()
        const st = stBinary(root)
        const app = projectDir(ctx, args as Json)
        const build = await ctx.runCommand(
          invoke(st, `build ${target} --profile ${profile}${toolchainFlag}`),
          { workdir: app, timeoutMs },
        )
        // 构建耗时**单独量**：`start` 把「构建 + 启动」捆在一起跑，而首次（冷缓存）
        // 要编整个框架静态库，是本仓库最耗时的一步。不分开报告时，调用方只能看到
        // `start` 很慢，无从判断"是不是构建在吃时间"（实测：Windows 冷构建 30s 级，
        // 而缓存命中态的启动只要 0.2s）。
        build_ms = Date.now() - build_started_at
        buildTail = `${build.stdout}${build.stderr}`.trim().split("\n").slice(-12).join("\n")
        if (build.code !== 0) return { output: `构建失败（exit ${build.code}）：\n${buildTail}` }
      }
      if (action === "build") {
        if (prebuilt) {
          // 显式告知“没构建”，而不是假装构建成功（后者会让人以为改了源码已生效）
          return {
            output:
              `纯二进制分发形态：**跳过构建**，直接使用已有可执行文件\n  ${shape.binary}`,
            data: { ok: true, action, target, mode: shape.mode, binary: shape.binary, build_ms: 0 },
          }
        }
        return {
          output: toolchain
            ? `交叉编译完成 · ${target} [${profile}] 工具链 ${toolchain}\n` +
              `产物: build/${buildSubdir}/bin/${target}.exe（Windows 目标，需在目标平台运行）\n${buildTail}`
            : `构建完成 · ${target} [${profile}]（耗时 ${build_ms}ms）\n${buildTail}`,
          data: { ok: true, action, target, output: buildTail, build_ms },
        }
      }

      // —— start：无头启动 + 等待控制通道就绪 ——
      if (toolchain) {
        return {
          output: `交叉编译产物（${toolchain}）无法在本机启动：请用 action=build 取产物，再到目标平台运行`,
        }
      }
      const control_file = controlFileFor(ctx, target)
      const log_file = logFileFor(ctx, target)
      // 二进制形态用探测到的路径；源码形态按档位现拼（与旧行为一致）
      const app = projectDir(ctx, args as Json)
      const binary = prebuilt ? shape.binary : appBinary(app, buildSubdir, target)
      if (!existsSync(binary)) {
        return {
          output:
            `构建产物不存在：${binary}\n` +
            (prebuilt
              ? `  按需分发形态下，把 ${exeName(target)} 放到：歌白可执行文件同级目录、${join(ctx.home, "bin")}、或 PATH 中`
              : `  先执行 action=build`),
        }
      }
      // ## 启动时的工作目录（分发形态下的一个真坑）
      //
      // 应用的**资源探测是相对工作目录的**（例：建木按 `apps/jianmu/scenes` 找场景）。
      // 源码形态下这件事自然成立（`st` 在工程根跑）；而分发形态下如果从会话目录或其他
      // 位置启动，应用能找到、控制通道能连、截图也有——就是**画面空**（objects=0）。
      // 实测：从 dist 目录启的 jianmu 报 `objects=0`，因为工作目录里没有场景。
      //
      // 所以分发形态一律**以 exe 所在目录为工作目录**（用户把场景/资源跟 exe 放一起即可）。
      // 源码形态保持原行为（工作目录 = 框架根），否则会改变既有开发习惯。
      const launchDir = prebuilt ? dirname(binary) : root
      // 外部资源参数在拼 flagArgs 时才需要（`extra` 在下面定义），这里只先算候选
      const resourceArgs = prebuilt ? resolveResourceArgs(target, binary, root) : []
      if (IS_WINDOWS) {
        await ctx.runCommand(
          `New-Item -ItemType Directory -Force -Path "${dirname(log_file)}" | Out-Null`,
          { workdir: launchDir },
        )
      } else {
        await ctx.runCommand(`mkdir -p "${dirname(log_file)}"`, { workdir: launchDir })
      }

        const extra = asString(args as Json, "args")
  const wantWindow = (args as Json).window === true
  const scale = asNumber(args as Json, "scale", 0)
      const theme = asString(args as Json, "theme")
      // ⚠ **启动前必须处理陈旧控制文件**（本条由一次真实误连换来）。
      //
      // 就绪轮询的判据曾是"文件存在 + 端口 > 0"——而上一轮遗留的文件**立刻满足它**：
      // 刚起的进程还没来得及写自己的端口，工具已经把**上一个实例的端口**当成本次结果返回。
      // 实测症状分两级：轻则握手失败（旧实例 token 与新文件里的不一致），
      // 重则**连到旧实例并"成功"**——之后的 capture/tree 操作的全是另一个应用实例。
      // 而旧客户端把握手被拒的响应静默丢掉，最终只报一句"未收到 capture 的响应"。
      //
      // 所以：① 通道还活着 → 明确拒绝（不许静默双实例）；② 通道不应答 → 那是陈旧文件，删掉。
      const existing = await readControlFile(control_file)
      if (existing) {
        if (await controlResponsive(existing.port)) {
      return {
        output:
          `已有一个存活的 ${target} 实例：PID ${existing.pid || "未知"} · 控制通道 127.0.0.1:${existing.port}\n` +
          `  想重开：先 run(action=stop, target="${target}")；\n` +
          `  想用它：tree/capture/set 等工具省略 target 即可（自动选中本会话唯一存活实例），` +
          `也可显式传 target="${control_file}"（注意：纯端口/host:port 写法不带 token，会被拒）`,
        data: { ok: false, action, target, pid: existing.pid, port: existing.port, already_running: true },
      }
        }
        // 通道不应答（进程已退出/崩溃）→ 文件陈旧：清掉，否则下面会拿到它的端口
        await ctx.deleteFile(control_file).catch(() => {})
      }
      const launched_at = Date.now()
      // 参数用**数组**拼（不经 shell 的引号规则）：两条路径对引号的要求相反——
      // POSIX 需要手动加引号防词分割，PowerShell 的 `-ArgumentList` 数组则不能带引号
      // （带了就会把引号当作参数内容传过去，控制文件路径变成 `"C:\…"`）。
        const flagArgs = [
    // ## 默认无头，但 `window:true` 时不注入
    //
    // 无头是**自动化回归**的正确默认（不依赖桌面环境、截图与控制通道齐全）。
    // 但“让用户亲手缩放/拖动”这个需求它满足不了：无头没有窗口，用户看不见也摸不着。
    // 曾经这里无条件写死 `--headless`，于是从工具层根本起不出真实窗口——
    // 调用方只能放弃使用工具、自己拼命令，而那会把控制文件/令牌/就绪探测全部重做一遍。
    ...(wantWindow ? [] : ["--headless"]),
    "--control-port",
    "0",
        "--control-file",
        control_file,
        "--shots",
        join(runtimeDir(ctx), "shots"),
        ...(scale > 0 ? ["--scale", String(scale)] : []),
        ...(theme ? ["--theme", theme] : []),
        // ## 外部资源目录（分发形态的关键）
        //
        // 图标/logo 已内嵌进 exe（`b::embed`），但 UI/场景/剧本是**故意外置**的
        // （要能热重载：改 XML/JS/脚本即生效）。这些应用都支持显式指定路径
        // （`--ui` / `--dir`），所以由工具**找到并告知**，而不指望应用从相对路径里猜对
        // ——工作目录一换，相对候选就全失效。
        //
        // 用户在 `args` 里已写同名 flag 时不重复追加（那是显式意图）。
        ...(resourceArgs.length === 2 && !(extra ?? "").includes(resourceArgs[0]) ? resourceArgs : []),
        ...(extra ? extra.split(" ").filter(Boolean) : []),
      ]
      // POSIX 单引号内不能出现单引号，需拆成 `'\''`（结束-转义-重新开始）
      const shellQuote = (value: string): string => `'${value.replace(/'/g, "'\\''")}'`
      const posixFlags = flagArgs.map(shellQuote).join(" ")
      const powershellFlags = flagArgs.map((piece) => `'${piece.replace(/'/g, "''")}'`).join(", ")
      // 脱离当前进程组，父进程退出后仍存活（无头常驻应用）。
      // Windows：Start-Process -PassThru 拿 PID；stdout/stderr 必须分开重定向到不同文件
      // （PowerShell 不允许两者指向同一个文件），读日志时按先后拼接。
      //
      // ⚠ **启动命令超时是预期行为，不是失败**（`kLaunchTimeoutMs` 为什么必须短）：
      // Windows 的句柄继承是**传递的**——常驻的 gbcode 会把祖先（PowerShell、cmd）
      // 的 stdout 管道一路继承下来，而 `runCommand` 等的是 `child.on("close")`
      // （stdout/stderr 都关）。于是命令永远不关闭，只能等到 `timeoutMs` 被杀。
      //
      // 实测（本机逐项验过）：
      //   · `spawn(stdio:"ignore")` → spawn 返回 **100ms**、控制文件就绪 **307ms**；
      //   · 经 shell 的任何写法（`cmd /c start /b` / `Start-Process` /
      //     甚至 `Stop-Process -Id $PID -Force` 强退 PowerShell）→ 控制文件照样
      //     300ms 就绪，但 `close` **15s 内不触发**——强退也治不了，因为句柄在孙子手里。
      //
      // 所以：启动命令**不 await**（它会挂到 `kLaunchTimeoutMs`），
      // 拉起与等就绪**并行**；真正的就绪判据是下面的“控制文件 + 握手”。
      // 旧值 20_000 就是用户看到的“启动要 20 来秒”的全部来源（现在约 300ms）。
      const err_file = `${log_file}.err`
      const launchPromise = (IS_WINDOWS
        ? ctx.runCommand(
            `$process = Start-Process -FilePath "${binary}" -ArgumentList @(${powershellFlags}) -PassThru -WindowStyle Hidden -RedirectStandardOutput "${log_file}" -RedirectStandardError "${err_file}"; ` +
              `$process.Id`,
            { workdir: launchDir, timeoutMs: kLaunchTimeoutMs },
          )
        : ctx.runCommand(
            `setsid nohup "${binary}" ${posixFlags} > "${log_file}" 2>&1 < /dev/null & echo $!`,
            { workdir: launchDir, timeoutMs: kLaunchTimeoutMs },
          )
      ).catch(() => ({ stdout: "", stderr: "" }))   // 超时是预期：子进程仍常驻

      // PID 先给 0：POSIX 分支很快有 stdout（不阻塞地回填）；
      // Windows 分支拿不到，由控制文件里应用自报的 pid 补齐。
      let pid = 0
      void launchPromise.then((l) => {
        const parsed = Number(l.stdout.trim().split("\n").pop() ?? "0")
        if (Number.isFinite(parsed) && parsed > 0 && pid <= 0) pid = parsed
      })

      // 轮询控制文件（应用启动后写入 port；token 随文件下发，握手必须携带）。
      //
      // 判据有三条，缺一不可：① 文件是**本次启动之后**写的（pid 相符或 mtime 新于启动时刻）；
      // ② 端口可用；③ **握手真的成功**——只有第③条能证明"这个端口是我们在跟它说话"，
      // 也就是说本工具承诺的"等待控制通道就绪"是**验证过的**，而不是"文件出现了"。
      let port = 0
      let handshake = ""
      const deadline = Date.now() + 30_000
      while (Date.now() < deadline) {
        const info = await readControlFile(control_file)
        if (info && info.port > 0) {
          // 启动命令超时（拿不到 stdout）时，用应用自报的 pid 补齐，供 stop/回收使用。
          if (pid <= 0 && info.pid > 0) pid = info.pid
          const fresh = pid <= 0 || info.pid === pid || mtimeMs(control_file) >= launched_at
          if (fresh) {
            try {
              const hello = await request<Json>(
                { host: "127.0.0.1", port: info.port, token: info.token || undefined },
                "hello",
                {},
                2000,
              )
              port = info.port
              handshake =
                `已连接：${JSON.stringify(hello.app ?? {})} 后端=${String(hello.backend)} 无头=${String(hello.headless)}`
              break
            } catch {
              // 还没就绪（文件先于通道就绪）或 token 尚未写入：继续等，不要把它当成功。
            }
          }
        }
        // 轮询间隔 200ms → **20ms**：应用实测 **321ms** 就写好控制文件，
        // 而 200ms 的睡会把就绪探测量化到 200/400/600…上（首轮未命中的话
        // 白付 200ms）。20ms 足够轻（只是读一个小 JSON 文件）且把量化误差压到小量级。
        await Bun.sleep(20)
      }
      if (port === 0) {
        const log = existsSync(log_file) ? (await Bun.file(log_file).text()).slice(-1200) : "(无日志)"
        // 启动失败不留下孤儿进程（进程可能起来了但控制通道没就绪）
        if (pid > 0) await terminateProcess(ctx, pid)
        return {
          output:
            `启动超时（30s 内控制通道未就绪：需文件新鲜 + 端口可用 + 握手成功）。` +
            `进程 PID=${pid} 已回收，日志尾部：\n${log}`,
          data: { ok: false, action, target, pid },
        }
      }
      // 启动（从拉起进程到握手成功）的耗时：与 build_ms 分开报告，便于区分
      // “这次花在编译上”还是“花在启动上”。
        const startup_ms = Date.now() - launched_at
  // 真实窗口与否以**握手回报的后端**为准（不信我们的意图）：
  // 传了 window=true 但机器无显示服务时，框架会自己回退 headless——如实转述它。
  const reportedHeadless = /无头=true/.test(handshake)
  const modeText = reportedHeadless ? "headless 无头" : "真实窗口"
  // 构建耗时在二进制形态下不适用：写出“跳过构建”而不是“构建 0ms”——
  // 后者会让人以为“构建了但很快”，进而误以为改了源码已生效。
  const buildNote = prebuilt ? "跳过构建（纯二进制分发）" : `构建 ${build_ms}ms`
  return {
    output:
      `已启动 ${target}（${modeText}）\n  PID      : ${pid} · 端口 ${port} · ${buildNote} · 启动 ${startup_ms}ms\n` +
      (prebuilt ? `  可执行文件 : ${binary}\n` : "") +
      `  控制文件 : ${control_file}\n  日志     : ${log_file}\n  ${handshake}\n` +
      (wantWindow && reportedHeadless
        ? "⚠ 请求了真实窗口但后端回退为无头（本机无可用显示服务）；请改用截图与控制通道验证。\n"
        : "") +
      `后续调用 target 二选一：① 省略 target（自动选中本会话唯一存活实例）；` +
      `② 传控制文件路径 target="${control_file}"` +
      `（注意：纯端口 / host:port 写法不带 token，会在握手阶段被拒）`,
    data: {
      ok: true,
      action,
      target,
      pid,
      port,
      control_file,
      log_file,
      build_ms,
      startup_ms,
      mode: shape.mode,
      binary,
    },
  }
    }

    if (action === "stop" || action === "status" || action === "logs") {
      const control_file = controlFileFor(ctx, target)
      const log_file = logFileFor(ctx, target)
      const info = await readControlFile(control_file)
      const pid = info?.pid ?? 0
      const port = info?.port ?? 0
      const token = info?.token ?? ""
      if (action === "logs") {
        const text = existsSync(log_file) ? await Bun.file(log_file).text() : "(无日志)"
        const errFile = `${log_file}.err`
        const errors = existsSync(errFile) ? await Bun.file(errFile).text() : ""
        return {
          output: `${text}${errors}`.slice(-4000),
          data: { ok: true, action, target, log_file },
        }
      }
      if (action === "status") {
        const alive =
          pid > 0
            ? IS_WINDOWS
              ? (await ctx.runCommand(
                  `if (Get-Process -Id ${pid} -ErrorAction SilentlyContinue) { '0' } else { '1' }`,
                  { timeoutMs: 5000 },
                )).stdout.trim() === "0"
              : (await ctx.runCommand(`kill -0 ${pid} 2>/dev/null; echo $?`, { timeoutMs: 5000 }))
                  .stdout.trim() === "0"
            : false
        let responsive = false
        if (port > 0) {
          try {
            await request({ host: "127.0.0.1", port }, "ping", {}, 1500)
            responsive = true
          } catch {
            responsive = false
          }
        }
        return {
          output: `${target}: 进程${alive ? "存活" : "未运行"} · 端口 ${port || "(未知)"} · 控制通道${responsive ? "响应正常" : "无响应"}`,
          data: { ok: alive && responsive, action, target, pid, port, control_file, log_file },
        }
      }
      // stop：优先优雅退出（shutdown），再兜底终止（含 PID 复用防护）。
      // PID 复用场景：应用崩溃后控制文件滞留，PID 可能已被系统分配给无关进程——
      // 先核进程名，明显不匹配时不终止（宁可不清理，不误杀）。
      let graceful = false
      if (port > 0) {
        try {
          await request({ host: "127.0.0.1", port, token: token || undefined }, "shutdown", {}, 3000)
          graceful = true
        } catch {
          graceful = false
        }
      }
      if (pid > 0) {
        const name = await processName(ctx, pid)
        const expected = (target.split(/[\\/]/).pop() ?? target).replace(/\.exe$/i, "")
        // 比对规则与“实例是否在跑”共用一处（`comm` 在 Linux 上截断到 15 字符，
        // 朴素 `includes` 会把长名字的**活实例**判成 PID 复用而拒绝终止）。
        if (name && expected && !processMatchesTarget(name, target)) {
          return {
            output: `未终止 PID ${pid}：进程名为 "${name}"，与目标 "${expected}" 不符（PID 已被复用？已跳过，避免误杀）`,
            data: { ok: false, action, target, pid, port, pid_reused: true },
          }
        }
        await terminateProcess(ctx, pid)
      }
      if (existsSync(control_file)) await ctx.deleteFile(control_file).catch(() => {})
      return { output: `已停止 ${target}（${graceful ? "优雅退出" : "信号终止"}）`, data: { ok: true, action, target, pid, port } }
    }

    if (action === "test" || action === "lint") {
      // 变量名不要叫 toolchain：那会遮蔽上面的「交叉编译工具链名」参数
      // （实测导致 `--toolchain` 变成 `[object Object]` 传下去）
      const bootstrap = await ensureToolchain()
      if (!bootstrap.ok) return { output: `引导工具链失败：\n${bootstrap.text}` }
          const san = asBool(args as Json, "san")
    const st = stBinary(root)
    const app = projectDir(ctx, args as Json)
    const command =
      action === "test"
        ? invoke(st, `test ${san ? "--san" : ""} ${asString(args as Json, "filter")}`.trim())
        : invoke(st, "lint")
      const result = await ctx.runCommand(command, { workdir: app, timeoutMs })
      const text = `${result.stdout}${result.stderr}`.trim()
      const tail = text.split("\n").slice(-25).join("\n")
      return {
        output: `${action === "test" ? "测试" : "禁令扫描"}完成（exit ${result.code}）：\n${tail}`,
        data: { ok: result.code === 0, action, target, output: tail },
      }
    }

    return { output: `未知 action：${action}` }
  },
}

// ————————————————————————————————————————————————————————————————————————————
// 只读查询：apps / tree / find / get / visual / metrics
// —————————————————————————————————————————————————————————————————————————————
const appsTool = readTool(
  "apps",
  "列出本会话已知的霜天应用实例（运行期控制文件 + 端口连通性校验），返回应用名/PID/端口/后端/主题。用于在操作前确认目标。",
  schema({}, []),
  async (_args, ctx) => {
    const entries: Array<Record<string, unknown>> = []
    const dir = runtimeDir(ctx)
    // 用同步扫描而不是 `ctx.listDir`：真运行时的 listDir 返回的字段是 `path`
    // （不是 `name`），旧实现读 `name` 拿到恒为空的字符串 → **每个实例都被跳过**，
    // 明明在跑也应答“当前没有运行中的霜天应用”。而既有测试用的是返回 `name` 的
    // 假 ctx，两侧口径不同 ⇒ 缺陷被测试完全盖住（CONVENTIONS §7.2 那类假绿）。
    // 现在 `apps` 与寻址（controlTarget）走同一个真源。
    for (const file of scanControlFilesSync(ctx)) {
      const name = (file.split(/[\\/]/).pop() ?? file).replace(/-control\.json$/, "")
      const info = await readControlFile(file)
      if (info === null) {
        entries.push({ app: name, error: "控制文件不可读" })
        continue
      }
      let responsive = false
      let screen: unknown = null
      if (info.port > 0) {
        try {
          const hello = await request<Json>(
            { host: "127.0.0.1", port: info.port, token: info.token || undefined },
            "hello",
            {},
            2000,
          )
          responsive = true
          screen = hello.screen ?? null
        } catch {
          responsive = false
        }
      }
      entries.push({ app: name, port: info.port, pid: info.pid, responsive, screen, control_file: file })
    }
    if (entries.length === 0) {
      return {
        output: `当前没有运行中的霜天应用（运行期目录：${dir}）。用 shuangtian_run（action=start）启动。`,
        data: { ok: false, apps: [] },
      }
    }
    const lines = entries.map(
      (item) =>
        `- ${String(item.app)} · 端口 ${String(item.port ?? "?")} · ${item.responsive ? "可控制" : "无响应"}`,
    )
    return { output: `运行中的应用：\n${lines.join("\n")}`, data: { ok: true, apps: entries } }
  },
)

const treeTool = readTool(
  "tree",
  "取应用组件树快照（语义树）：节点 id/type/role/bounds/text/value/状态位。自动化定位的起点——先 tree 或 find 拿到 id，再 get/set/invoke。depth 控制深度（0=不限）。",
  schema({ depth: { type: "number", description: "最大深度（0=不限）" }, target: { type: "string", description: "host:port 或控制文件路径" } }, []),
  async (args, ctx) => {
    const result = await controlCall(ctx, args, "tree", { depth: asNumber(args, "depth", 3) }, 10_000)
    return { output: result.text, data: result.data }
  },
)

const findTool = readTool(
  "find",
  "选择器查询组件（比 tree 更适合大界面）：支持 #id、Type、[text~=关键词]、:focused 等；返回匹配节点的 id/bounds/text。" +
    "**返回结构是 `{selector, count, total, matches[]}`**（不是裸数组）——取 id 要读 `matches[i].id`；" +
    "`count` = 本次返回条数（受 limit 限制），`total` = 命中总数（不受 limit 影响）——两者不等就是被截断了。" +
    "动态列表的 id 形如 `tasks/ListItem@task-42`（业务 key 在 `@` 后，与位置无关）：刷新后按 key 定位，别按索引。",
  schema(
    {
      selector: { type: "string", description: "选择器，如 'Button[text~=保存]'、'#btn-save'、'Input:focused'" },
      limit: { type: "number", description: "最多返回条数（默认 20）" },
      target: { type: "string", description: "host:port 或控制文件路径（缺 token 的写法会自动从控制文件补齐）" },
    },
    ["selector"],
  ),
  async (args, ctx) => {
    const result = await controlCall(
      ctx,
      args,
      "find",
      { selector: asString(args, "selector"), limit: asNumber(args, "limit", 20) },
      10_000,
    )
    return { output: result.text, data: result.data }
  },
)

const getTool = readTool(
  "get",
  "读取单个组件属性（props 含 text/value/checked/enabled/visible 等）。",
  schema({ id: { type: "string", description: "组件 id（来自 tree/find）" }, target: { type: "string", description: "host:port 或控制文件路径" } }, ["id"]),
  async (args, ctx) => {
    const result = await controlCall(ctx, args, "get", { id: asString(args, "id") }, 8000)
    return { output: result.text, data: result.data }
  },
)

const visualTool = readTool(
  "visual",
  "取视觉树（实际绘制层：bounds/填充色/圆角/文本/命中目标）——用于「看起来对不对」的核对，与语义树互补。",
  schema({ target: { type: "string", description: "host:port 或控制文件路径" } }, []),
  async (args, ctx) => {
    const result = await controlCall(ctx, args, "visual", {}, 10_000)
    return { output: result.text, data: result.data }
  },
)

const metricsTool = readTool(
  "metrics",
  "取运行指标：后端/无头标记/DPI(device_scale)/物理尺寸/帧数/帧耗时分位/节点数/请求数/最近控制通道日志。用于确认「确实在高 DPI 下渲染」「无头模式生效」等。",
  schema({ target: { type: "string", description: "host:port 或控制文件路径" } }, []),
  async (args, ctx) => {
    const result = await controlCall(ctx, args, "metrics", {}, 8000)
    return { output: result.text, data: result.data }
  },
)

// ————————————————————————————————————————————————————————————————————————————
// 操作：set / invoke / input(mouse/key/text) / capture / wait
// —————————————————————————————————————————————————————————————————————————————
const setTool: Tool = {
  name: "set",
  description:
    "设置组件属性（text/value/checked/enabled/visible/focused 等）并触发重绘。参数 props 为对象，如 {\"text\":\"新标题\",\"checked\":true}。",
  parameters: schema(
    {
      id: { type: "string", description: "组件 id" },
      props: { type: "object", description: "要设置的属性键值对" },
      target: { type: "string", description: "host:port 或控制文件路径" },
    },
    ["id", "props"],
  ),
  requiresApproval: true,
  execute: async (args, ctx) => {
    const result = await controlCall(
      ctx,
      args,
      "set",
      { id: asString(args, "id"), props: (args.props as Json) ?? {} },
      8000,
    )
    return { output: result.text, data: result.data }
  },
}

const invokeTool: Tool = {
  name: "invoke",
  description: '触发组件动作（不走真实鼠标）：action=click/activate/focus/blur/toggle/select/submit 等。常用于"点按钮看看回调做了什么"。',
  parameters: schema(
    {
      id: { type: "string", description: "组件 id" },
      action: { type: "string", description: "动作名（默认 click）" },
      argument: { type: "string", description: "可选参数（如选项值）" },
      target: { type: "string", description: "host:port 或控制文件路径" },
    },
    ["id"],
  ),
  requiresApproval: true,
  execute: async (args, ctx) => {
    const result = await controlCall(
      ctx,
      args,
      "invoke",
      { id: asString(args, "id"), action: asString(args, "action", "click"), argument: asString(args, "argument") },
      8000,
    )
    return { output: result.text, data: result.data }
  },
}

const clickTool: Tool = {
  name: "click",
  description:
    "在逻辑坐标处注入真实鼠标事件（坐标与 capture 截图一致；无头模式下这是唯一的点击通道）。kind=click/dblclick/down/up/move，支持 ctrl/shift/alt 修饰键。",
  parameters: schema(
    {
      x: { type: "number", description: "逻辑坐标 X" },
      y: { type: "number", description: "逻辑坐标 Y" },
      kind: { type: "string", description: "click（默认）/dblclick/down/up/move/triple" },
      button: { type: "number", description: "1=左键（默认）/2=中键/3=右键" },
      ctrl: { type: "boolean" },
      shift: { type: "boolean" },
      alt: { type: "boolean" },
      meta: { type: "boolean" },
      target: { type: "string", description: "host:port 或控制文件路径" },
    },
    ["x", "y"],
  ),
  requiresApproval: true,
  execute: async (args, ctx) => {
    const result = await controlCall(
      ctx,
      args,
      "input.mouse",
      {
        kind: asString(args, "kind", "click"),
        x: asNumber(args, "x", 0),
        y: asNumber(args, "y", 0),
        button: asNumber(args, "button", 1),
        ctrl: asBool(args, "ctrl"),
        shift: asBool(args, "shift"),
        alt: asBool(args, "alt"),
        meta: asBool(args, "meta"),
      },
      10_000,
    )
    return { output: result.text, data: result.data }
  },
}

const typeTool: Tool = {
  name: "type",
  description:
    "向当前焦点组件输入文本（无头模式下无系统键盘，经控制通道注入 TextInput 事件）。可先 invoke/focus 指定组件，或用 id 参数直接聚焦。",
  parameters: schema(
    {
      text: { type: "string", description: "要输入的文本" },
      id: { type: "string", description: "可选：先聚焦该组件再输入" },
      target: { type: "string", description: "host:port 或控制文件路径" },
    },
    ["text"],
  ),
  requiresApproval: true,
  execute: async (args, ctx) => {
    const result = await controlCall(
      ctx,
      args,
      "input.text",
      { text: asString(args, "text"), id: asString(args, "id") },
      10_000,
    )
    return { output: result.text, data: result.data }
  },
}

const keyTool: Tool = {
  name: "key",
  description: '发送按键（如 Tab/Enter/Escape/Backspace/ArrowDown/Ctrl+S）。kind=press（默认）/down/up，支持修饰键。',
  parameters: schema(
    {
      key: { type: "string", description: "键名" },
      kind: { type: "string", description: "press（默认）/down/up" },
      ctrl: { type: "boolean" },
      shift: { type: "boolean" },
      alt: { type: "boolean" },
      meta: { type: "boolean" },
      target: { type: "string", description: "host:port 或控制文件路径" },
    },
    ["key"],
  ),
  requiresApproval: true,
  execute: async (args, ctx) => {
    const result = await controlCall(
      ctx,
      args,
      "input.key",
      {
        kind: asString(args, "kind", "press"),
        key: asString(args, "key"),
        ctrl: asBool(args, "ctrl"),
        shift: asBool(args, "shift"),
        alt: asBool(args, "alt"),
        meta: asBool(args, "meta"),
      },
      10_000,
    )
    return { output: result.text, data: result.data }
  },
}

const captureTool = readTool(
  "capture",
  "截图（PNG）。无头模式下这是唯一的「看见界面」通道——回归截图会作为图片直接呈现，可直接判断美观与布局。可传 id 截单个组件，或 region 截指定区域；scale/DPI 不影响截图分辨率（截的是物理像素）。固定 encode=file 落盘返回路径（大图 base64 回传会被工具输出护栏截断，不要改用 encode=base64）。",
  schema(
    {
      id: { type: "string", description: "可选：只截该组件区域" },
      x: { type: "number", description: "region 左上 X（逻辑坐标）" },
      y: { type: "number", description: "region 左上 Y" },
      width: { type: "number", description: "region 宽" },
      height: { type: "number", description: "region 高" },
      path: {
        type: "string",
        description:
          "保存路径（默认落会话 .shuangtian/shots/）。应用有落盘白名单（temp / 可执行文件目录 / 控制文件目录，不递归），" +
          "所以给 <会话>/qa/x.png 这类路径会被自动落到 .shuangtian/shots/x.png",
      },
      target: { type: "string", description: "host:port 或控制文件路径（缺 token 的写法会自动从控制文件补齐）" },
    },
    [],
  ),
  async (args, ctx) => {
    const path = resolveShotPath(ctx, asString(args, "path"))
    const params: Json = { encode: "file", path }
    // 路径被改写过就明说（否则调用方会去自己传的路径找文件，找不到还不知道为什么）
    const rewritten = asString(args, "path") !== "" && ctx.resolvePath(asString(args, "path")) !== path
    if (asString(args, "id")) params.id = asString(args, "id")
    if (typeof args.width === "number" && typeof args.height === "number") {
      params.region = {
        x: asNumber(args, "x", 0),
        y: asNumber(args, "y", 0),
        width: asNumber(args, "width", 0),
        height: asNumber(args, "height", 0),
      }
    }
    const result = await controlCall(ctx, args, "capture", params, 60_000)
    if (!result.ok) return { output: result.text }
    // `path` 只在它**确实是字符串**时用：服务端非 `file` 分支（回 base64）不返回 path，
    // 而旧写法 `?? path` 遇到 `path: null/数字` 会把请求路径当成结果报出去。
    const raw = (result.data as Json | undefined)?.path
    const saved = typeof raw === "string" && raw !== "" ? raw : path
    // 服务端回的是它**实际写入**的路径；连它都不在磁盘上就只能明说，
    // 而不是照常报“截图已保存”——下游会拿着一个不存在的路径去读图。
    if (!existsSync(saved)) {
      return {
        output: `截图疑似未落盘：服务端返回路径 ${saved}，但该文件不存在。原始响应：\n${result.text.slice(0, 800)}`,
        data: result.data,
      }
    }
    const meta = (result.data as Json | undefined)?.region
      ? `（区域 ${JSON.stringify((result.data as Json).region)}）`
      : ""
    const note = rewritten ? "（请求路径不在应用的落盘白名单内，已改落 .shuangtian/shots/）" : ""
    return {
      output: `截图已保存: ${saved}${meta}${note}`,
      blocks: artifactBlocks(saved),
      data: result.data,
    }
  },
)

const captureHashTool = readTool(
  "capture_hash",
  "像素哈希：对指定区域的**物理像素 RGBA 字节**算 FNV-1a 64（比传回 PNG 便宜几个量级）。用途：「画面变了没有」的快速断言——同区域两次调用哈希一致即逐像素完全一致。只回答「完全一致吗」；「差多少」用 visual_diff。默认全屏，可传 id 或 region。",
  schema(
    {
      id: { type: "string", description: "可选：只算该组件区域" },
      x: { type: "number", description: "region 左上 X（逻辑坐标）" },
      y: { type: "number", description: "region 左上 Y" },
      width: { type: "number", description: "region 宽" },
      height: { type: "number", description: "region 高" },
      target: { type: "string", description: "host:port 或控制文件路径" },
    },
    [],
  ),
  async (args, ctx) => {
    const params: Json = {}
    if (asString(args, "id")) params.id = asString(args, "id")
    if (typeof args.width === "number" && typeof args.height === "number") {
      params.region = {
        x: asNumber(args, "x", 0),
        y: asNumber(args, "y", 0),
        width: asNumber(args, "width", 0),
        height: asNumber(args, "height", 0),
      }
    }
    const result = await controlCall(ctx, args, "capture.hash", params, 20_000)
    return { output: result.text, data: result.data }
  },
)

const visualDiffTool = readTool(
  "visual_diff",
  "视觉回归断言：把当前画面与基线 PNG 逐像素比对，返回 diff_pixels/diff_ratio/mean_diff/max_diff/diff_bounds（可做量化断言，而非只看图）。首次用 write_baseline=true 存基线（落在白名单目录内）；基线缺失且未写时明确报错。threshold 忽略微小差异不计入差异像素；tolerance 允许的差异像素占比上限（超限才 changed=true）——典型用法：tolerance=0.001 吸收抗锯齿抖动。基线尺寸与当前截图不一致会报错（不做缩放对齐）。",
  schema(
    {
      path: { type: "string", description: "基线 PNG 路径（需在截图白名单目录内才能写入）" },
      write_baseline: { type: "boolean", description: "true=把当前帧写为基线（覆盖路径）" },
      threshold: { type: "number", description: "像素差异阈值 0-255（默认 0；≤ 阈值的通道差不计入差异像素）" },
      tolerance: { type: "number", description: "允许的差异像素占比（默认 0；超过才 changed=true）" },
      id: { type: "string", description: "可选：只比对组件区域（需与基线写入时同区域）" },
      x: { type: "number", description: "region 左上 X（逻辑坐标）" },
      y: { type: "number", description: "region 左上 Y" },
      width: { type: "number", description: "region 宽" },
      height: { type: "number", description: "region 高" },
      target: { type: "string", description: "host:port 或控制文件路径" },
    },
    ["path"],
  ),
  async (args, ctx) => {
    const params: Json = { path: ctx.resolvePath(asString(args, "path")) }
    if (asBool(args, "write_baseline")) params.write_baseline = true
    if (typeof args.threshold === "number") params.threshold = args.threshold
    if (typeof args.tolerance === "number") params.tolerance = args.tolerance
    if (asString(args, "id")) params.id = asString(args, "id")
    if (typeof args.width === "number" && typeof args.height === "number") {
      params.region = {
        x: asNumber(args, "x", 0),
        y: asNumber(args, "y", 0),
        width: asNumber(args, "width", 0),
        height: asNumber(args, "height", 0),
      }
    }
    const result = await controlCall(ctx, args, "visual.diff", params, 30_000)
    return { output: result.text, data: result.data }
  },
)

const waitTool = readTool(
  "wait",
  "等待界面条件（比轮询截图高效）：for=element/gone（按选择器）· text/text_gone（文本出现/消失）· stable（画面稳定）。超时返回 satisfied=false，不报错。",
  schema(
    {
      for: { type: "string", enum: ["element", "gone", "text", "text_gone", "stable"], description: "等待条件" },
      selector: { type: "string", description: "for=element/gone 时的选择器" },
      text: { type: "string", description: "for=text/text_gone 时的文本" },
      timeout_ms: { type: "number", description: "超时（默认 5000）" },
      target: { type: "string", description: "host:port 或控制文件路径" },
    },
    ["for"],
  ),
  async (args, ctx) => {
    const timeout = asNumber(args, "timeout_ms", 5000)
    const result = await controlCall(
      ctx,
      args,
      "wait",
      {
        for: asString(args, "for", "element"),
        selector: asString(args, "selector"),
        text: asString(args, "text"),
        timeout_ms: timeout,
      },
      timeout + 5000,
    )
    return { output: result.text, data: result.data }
  },
)

const waitEventTool = readTool(
  "wait_event",
  '等待控制通道事件（推送语义，与 wait 的条件轮询互补）：连上订阅后收集事件直到凑满 count 个或超时。最常用 kinds=["ui.changed"]——事件 data 含 version 与 changed（变更元素 id 清单），用于「等界面真的变了」而不是靠轮询。超时返回空数组（不报错）；返回的 events 数组每项 {event, seq, data}。',
  schema(
    {
      kinds: {
        type: "array",
        description: "订阅的事件类型（缺省全部；常用 [\"ui.changed\"]）",
        items: { type: "string" },
      },
      count: { type: "number", description: "收集多少个事件后返回（默认 1）" },
      timeout_ms: { type: "number", description: "超时（默认 5000；超时返回已收集的事件，不报错）" },
      target: { type: "string", description: "host:port 或控制文件路径" },
    },
    [],
  ),
  async (args, ctx) => {
    const timeout = asNumber(args, "timeout_ms", 5000)
    try {
      const resolved = await controlTarget(ctx, args)
      const kinds = Array.isArray(args.kinds) ? args.kinds.filter((k): k is string => typeof k === "string") : []
      const events = await wait_for_event(resolved, {
        kinds: kinds.length > 0 ? kinds : undefined,
        count: asNumber(args, "count", 1),
        timeout_ms: timeout,
      })
      return {
        output: JSON.stringify({ count: events.length, events }, null, 2),
        data: { count: events.length, events },
      }
    } catch (error) {
      const message = error instanceof ControlError ? `${error.code}: ${error.message}` : String(error)
      return { output: `等待事件失败：${message}` }
    }
  },
)

const callTool: Tool = {
  name: "call",
  description:
    "通用控制通道调用（未封装方法的逃生门）：method 如 app/theme/events/shutdown，params 为对象。协议 `st-control/1`，坐标一律逻辑像素。注意：响应文本超过 12K 字符会被工具输出护栏截断（头尾各 4K）——capture 的 base64 大图请改用 capture 工具（encode=file 落盘，不回传 base64）。",
  parameters: schema(
    {
      method: { type: "string", description: "方法名（tree/find/get/set/invoke/input.mouse/input.key/input.text/capture/visual/wait/metrics/events/theme/app/hello/ping/shutdown）" },
      params: { type: "object", description: "参数对象" },
      target: { type: "string", description: "host:port 或控制文件路径" },
      timeout_ms: { type: "number", description: "超时（默认 15000）" },
    },
    ["method"],
  ),
  requiresApproval: true,
  execute: async (args, ctx) => {
    const method = asString(args, "method")
    const result = await controlCall(ctx, args, method, (args.params as Json) ?? {}, asNumber(args, "timeout_ms", 15_000))
    return { output: result.text, data: result.data }
  },
}

/** 工具表（合并型只读工具免审批；写与输入类需审批见 def.requiresApproval）。 */
export const tools = {
  run: runTool,
  apps: appsTool,
  tree: treeTool,
  find: findTool,
  get: getTool,
  set: setTool,
  invoke: invokeTool,
  click: clickTool,
  type: typeTool,
  key: keyTool,
  capture: captureTool,
  capture_hash: captureHashTool,
  visual: visualTool,
  visual_diff: visualDiffTool,
  wait: waitTool,
  wait_event: waitEventTool,
  metrics: metricsTool,
  call: callTool,
}

export const requiresApproval = {
  run: true,
  set: true,
  invoke: true,
  click: true,
  type: true,
  key: true,
  call: true,
}

export { call_control, request, resolve_target, control_app_name, ControlError }
export type { ControlResult, ControlTarget, SubAgentDef, ToolContext }
export { basename }
