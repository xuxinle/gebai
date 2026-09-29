/**
 * 霜天子代理工具集：把原生桌面应用当成"可编排的受控进程"来开发与验证。
 *
 * 分工：本文件是**操作面**（构建/启动/查询/操作/截图），`shuangtian/` 是**被操作面**（框架）。
 * 所有工具经 TCP 控制通道（`st-control/1`）与进程管理落地，不依赖宿主机桌面环境——
 * 无头模式（`--headless`）下 Linux 服务器无显示服务也能完整开发与验证界面。
 */
import { existsSync } from "node:fs"
import { basename, dirname, join } from "node:path"
import type { SubAgentDef, Tool, ToolContext, ToolResult } from "@gebai/sdk"
import { artifactBlocks, schema } from "@gebai/sdk/node"
import {
  ControlError,
  call_control,
  request,
  resolve_target,
  type ControlResult,
  type ControlTarget,
} from "./shuangtian_client"

/** 框架默认根（`SHUANGTIAN_PROJECT` 未配置时）：歌白仓库根下的 `shuangtian/`。 */
const DEFAULT_FRAMEWORK_DIR = "shuangtian"
/** 运行期状态目录（控制文件/日志/截图），位于会话工作区。 */
const RUNTIME_DIR = ".shuangtian"
const DEFAULT_BUILD_TIMEOUT_MS = 600_000

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

/** 框架根：显式参数 > 环境变量 > 仓库默认位置（含 `st.pkg` 校验）。 */
function frameworkDir(ctx: ToolContext, args: Json): string {
  const explicit = asString(args, "framework") || String(ctx.env.SHUANGTIAN_PROJECT ?? "").trim()
  if (explicit) return ctx.resolvePath(explicit)
  const candidate = ctx.resolvePath(DEFAULT_FRAMEWORK_DIR)
  if (existsSync(join(candidate, "st.pkg"))) return candidate
  // 兜底：GEBAI_HOME 下的随包分发位置
  const vendored = join(ctx.home, "vendor", "shuangtian")
  return existsSync(join(vendored, "st.pkg")) ? vendored : candidate
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

/**
 * 解析控制目标：显式 `target`（`host:port` / 控制文件路径 / 纯端口）> 环境变量 >
 * 会话运行期控制文件（由 `run` 的 start 动作写入）。
 */
async function controlTarget(ctx: ToolContext, args: Json, target?: string): Promise<ControlTarget> {
  const explicit = target ?? asString(args, "target")
  if (explicit) return resolve_target(explicit)
  const fromEnv = String(ctx.env.SHUANGTIAN_TARGET ?? "").trim()
  if (fromEnv) return resolve_target(fromEnv)
  const fallback = controlFileFor(ctx, asString(args, "app", "mdeditor"))
  if (existsSync(fallback)) return resolve_target(fallback)
  const gallery = controlFileFor(ctx, "gallery")
  if (existsSync(gallery)) return resolve_target(gallery)
  throw new ControlError(
    "not_found",
    `未找到运行中的应用：请先 shuangtian_run（action=start）启动，或用 target 参数指定 "host:port"/控制文件路径；` +
      `也可设置环境变量 SHUANGTIAN_TARGET（当前会话目录：${runtimeDir(ctx)}）`,
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
const runTool: Tool = {
  name: "run",
  description:
    "构建与运行霜天应用（框架生命周期入口）。action=build 构建（自动自举 stpm 工具链）；action=test 跑框架单元测试（可选 san=true 开 ASan/UBSan）；action=lint 跑禁用特性静态扫描；action=start 以无头模式启动应用并等待控制通道就绪（返回 port 与 PID）；action=stop 结束进程；action=status 查看进程与端口；action=logs 取运行日志尾部。target 为应用名（gallery/mdeditor 或自带工程的目标名），profile 为构建档（dev/debug/release/san）。",
  parameters: schema(
    {
      action: {
        type: "string",
        enum: ["init", "build", "test", "lint", "start", "stop", "status", "logs"],
        description: "动作（init 新建独立工程；其余作用于 project 或框架自身）",
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
      name: { type: "string", description: "action=init 时的工程名（缺省取目录名）" },
      args: { type: "string", description: "start 时附加命令行参数（如 --demo-stream）" },
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

    if (!existsSync(join(root, "st.pkg"))) {
      return { output: `未找到霜天框架（${root}/st.pkg 不存在）：请用 framework 参数指定框架根，或设置 SHUANGTIAN_PROJECT` }
    }
    if (action !== "init" && !existsSync(projectManifest(ctx, args as Json))) {
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
      // 变量名不要叫 toolchain：那会遮蔽上面的「交叉编译工具链名」参数
      // （实测导致 `--toolchain` 变成 `[object Object]` 传下去）
      const bootstrap = await ensureToolchain()
      if (!bootstrap.ok) return { output: `引导工具链失败：\n${bootstrap.text}` }
      // 被构建的是**工程**（可能与框架不同目录）；`st` 始终用框架自己的那份
          const st = stBinary(root)
    const app = projectDir(ctx, args as Json)
    const build = await ctx.runCommand(
      invoke(st, `build ${target} --profile ${profile}${toolchainFlag}`),
      { workdir: app, timeoutMs },
    )
      const tail = `${build.stdout}${build.stderr}`.trim().split("\n").slice(-12).join("\n")
      if (build.code !== 0) return { output: `构建失败（exit ${build.code}）：\n${tail}` }
      if (action === "build") {
        return {
          output: toolchain
            ? `交叉编译完成 · ${target} [${profile}] 工具链 ${toolchain}\n` +
              `产物: build/${buildSubdir}/bin/${target}.exe（Windows 目标，需在目标平台运行）\n${tail}`
            : `构建完成 · ${target} [${profile}]\n${tail}`,
          data: { ok: true, action, target, output: tail },
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
      const binary = appBinary(app, buildSubdir, target)
      if (!existsSync(binary)) return { output: `构建产物不存在：${binary}` }
      if (IS_WINDOWS) {
        await ctx.runCommand(
          `New-Item -ItemType Directory -Force -Path "${dirname(log_file)}" | Out-Null`,
          { workdir: root },
        )
      } else {
        await ctx.runCommand(`mkdir -p "${dirname(log_file)}"`, { workdir: root })
      }

      const extra = asString(args as Json, "args")
      const scale = asNumber(args as Json, "scale", 0)
      const theme = asString(args as Json, "theme")
      // 参数用**数组**拼（不经 shell 的引号规则）：两条路径对引号的要求相反——
      // POSIX 需要手动加引号防词分割，PowerShell 的 `-ArgumentList` 数组则不能带引号
      // （带了就会把引号当作参数内容传过去，控制文件路径变成 `"C:\…"`）。
      const flagArgs = [
        "--headless",
        "--control-port",
        "0",
        "--control-file",
        control_file,
        "--shots",
        join(runtimeDir(ctx), "shots"),
        ...(scale > 0 ? ["--scale", String(scale)] : []),
        ...(theme ? ["--theme", theme] : []),
        ...(extra ? extra.split(" ").filter(Boolean) : []),
      ]
      // POSIX 单引号内不能出现单引号，需拆成 `'\''`（结束-转义-重新开始）
      const shellQuote = (value: string): string => `'${value.replace(/'/g, "'\\''")}'`
      const posixFlags = flagArgs.map(shellQuote).join(" ")
      const powershellFlags = flagArgs.map((piece) => `'${piece.replace(/'/g, "''")}'`).join(", ")
      // 脱离当前进程组，父进程退出后仍存活（无头常驻应用）。
      // Windows：Start-Process -PassThru 拿 PID；stdout/stderr 必须分开重定向到不同文件
      // （PowerShell 不允许两者指向同一个文件），读日志时按先后拼接。
      const err_file = `${log_file}.err`
      let launch: { stdout: string; stderr: string }
      if (IS_WINDOWS) {
        launch = await ctx.runCommand(
          `$process = Start-Process -FilePath "${binary}" -ArgumentList @(${powershellFlags}) -PassThru -WindowStyle Hidden -RedirectStandardOutput "${log_file}" -RedirectStandardError "${err_file}"; ` +
            `$process.Id`,
          { workdir: root, timeoutMs: 20_000 },
        )
      } else {
        launch = await ctx.runCommand(
          `setsid nohup "${binary}" ${posixFlags} > "${log_file}" 2>&1 < /dev/null & echo $!`,
          { workdir: root, timeoutMs: 20_000 },
        )
      }
      const pid = Number(launch.stdout.trim().split("\n").pop() ?? "0")

      // 轮询控制文件（应用启动后写入 port）
      let port = 0
      const deadline = Date.now() + 30_000
      while (Date.now() < deadline) {
        if (existsSync(control_file)) {
          try {
            const info = (await Bun.file(control_file).json()) as { port?: number }
            if (typeof info.port === "number" && info.port > 0) {
              port = info.port
              break
            }
          } catch {
            // 文件正在写入，稍后重试
          }
        }
        await Bun.sleep(200)
      }
      if (port === 0) {
        const log = existsSync(log_file) ? (await Bun.file(log_file).text()).slice(-1200) : "(无日志)"
        return { output: `启动超时（未取得控制端口）。进程 PID=${pid}，日志尾部：\n${log}` }
      }
      // 握手确认
      let handshake = ""
      try {
        const hello = await request<Json>({ host: "127.0.0.1", port }, "hello", {}, 5000)
        handshake = `已连接：${JSON.stringify(hello.app ?? {})} 后端=${String(hello.backend)} 无头=${String(hello.headless)}`
      } catch (error) {
        handshake = `握手失败：${String(error)}`
      }
      return {
        output:
          `已启动 ${target}（headless）\n  PID      : ${pid}\n  控制通道 : 127.0.0.1:${port}\n` +
          `  控制文件 : ${control_file}\n  日志     : ${log_file}\n  ${handshake}\n` +
          `下一步：shuangtian_tree / shuangtian_capture / shuangtian_input_* 皆可指定 target="${port}"`,
        data: { ok: true, action, target, pid, port, control_file, log_file },
      }
    }

    if (action === "stop" || action === "status" || action === "logs") {
      const control_file = controlFileFor(ctx, target)
      const log_file = logFileFor(ctx, target)
      let pid = 0
      let port = 0
      if (existsSync(control_file)) {
        try {
          const info = (await Bun.file(control_file).json()) as { port?: number; pid?: number }
          port = typeof info.port === "number" ? info.port : 0
          pid = typeof info.pid === "number" ? info.pid : 0
        } catch {
          // 忽略
        }
      }
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
      // stop：优先优雅退出（shutdown），再兜底 SIGTERM
      let graceful = false
      if (port > 0) {
        try {
          await request({ host: "127.0.0.1", port }, "shutdown", {}, 3000)
          graceful = true
        } catch {
          graceful = false
        }
      }
      if (pid > 0) {
        if (IS_WINDOWS) {
          await ctx.runCommand(
            `Stop-Process -Id ${pid} -Force -ErrorAction SilentlyContinue`,
            { timeoutMs: 8000 },
          )
        } else {
          await ctx.runCommand(
            `kill -TERM ${pid} 2>/dev/null; sleep 0.3; kill -0 ${pid} 2>/dev/null && kill -KILL ${pid} 2>/dev/null; true`,
            { timeoutMs: 8000 },
          )
        }
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
    if (ctx.listDir) {
      try {
        const files = await ctx.listDir(dir)
        for (const file of files) {
          const name = (file as { name?: string }).name ?? ""
          if (!name.endsWith("-control.json")) continue
          const path = join(dir, name)
          try {
            const info = (await Bun.file(path).json()) as Record<string, unknown>
            const port = typeof info.port === "number" ? info.port : 0
            let responsive = false
            let screen: unknown = null
            if (port > 0) {
              try {
                const hello = await request<Json>({ host: "127.0.0.1", port }, "hello", {}, 2000)
                responsive = true
                screen = hello.screen ?? null
              } catch {
                responsive = false
              }
            }
            entries.push({ app: name.replace("-control.json", ""), port, responsive, screen, info })
          } catch {
            entries.push({ app: name.replace("-control.json", ""), error: "控制文件不可读" })
          }
        }
      } catch {
        // 目录不存在 → 无实例
      }
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
  "选择器查询组件（比 tree 更适合大界面）：支持 #id、Type、[text~=关键词]、:focused 等；返回匹配节点的 id/bounds/text。",
  schema(
    {
      selector: { type: "string", description: "选择器，如 'Button[text~=保存]'、'#btn-save'、'Input:focused'" },
      limit: { type: "number", description: "最多返回条数（默认 20）" },
      target: { type: "string", description: "host:port 或控制文件路径" },
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
  "截图（PNG）。无头模式下这是唯一的「看见界面」通道——回归截图会作为图片直接呈现，可直接判断美观与布局。可传 id 截单个组件，或 region 截指定区域；scale/DPI 不影响截图分辨率（截的是物理像素）。",
  schema(
    {
      id: { type: "string", description: "可选：只截该组件区域" },
      x: { type: "number", description: "region 左上 X（逻辑坐标）" },
      y: { type: "number", description: "region 左上 Y" },
      width: { type: "number", description: "region 宽" },
      height: { type: "number", description: "region 高" },
      path: { type: "string", description: "保存路径（默认落会话 .shuangtian/shots/）" },
      target: { type: "string", description: "host:port 或控制文件路径" },
    },
    [],
  ),
  async (args, ctx) => {
    const requested = asString(args, "path")
    const fallback = join(runtimeDir(ctx), "shots", `shot-${Date.now()}.png`)
    const path = requested ? ctx.resolvePath(requested) : fallback
    const params: Json = { encode: "file", path }
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
    const saved = String((result.data as Json | undefined)?.path ?? path)
    const meta = (result.data as Json | undefined)?.region
      ? `（区域 ${JSON.stringify((result.data as Json).region)}）`
      : ""
    return {
      output: `截图已保存: ${saved}${meta}`,
      blocks: artifactBlocks(saved),
      data: result.data,
    }
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

const callTool: Tool = {
  name: "call",
  description:
    "通用控制通道调用（未封装方法的逃生门）：method 如 app/theme/events/shutdown，params 为对象。协议 `st-control/1`，坐标一律逻辑像素。",
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
  visual: visualTool,
  wait: waitTool,
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

export { call_control, request, resolve_target, ControlError }
export type { ControlResult, ControlTarget, SubAgentDef, ToolContext }
export { basename }
