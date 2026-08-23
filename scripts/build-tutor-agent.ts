/**
 * 教辅（tutor）场景裁剪构建（根 scripts/ 目录）——按学段产出**两份专属桌面端**：
 * 小学版（tutor_primary）与中学版（tutor_secondary），各只打包并预载本学段子Agent
 * （另一学段不进二进制，零路由面；学生小升初换用另一版 exe 即可，~/.gebai 数据共享延续）。
 * 每版两个形态：浏览器形态（编译 desktop 入口，固定端口 47896 自动开浏览器）+
 * 原生 WebView 窗口形态（launcher 变体参数：独立应用身份/物化目录/端口/标题，可与完整桌面端
 * 及另一学段版同时运行）。模板出处见 scripts/build-code-agent.ts。
 *
 * 三层裁剪维度（DESIGN「构建期裁剪与预加载指定」）：
 * 1. `GEBAI_BUILD_SUBAGENTS`：每版只打包本学段子Agent（体积与上下文双收益）；
 * 2. `GEBAI_BUILD_PRELOAD`：本学段预装载，开箱即辅导（无另一学段提示词/工具 schema 占上下文）；
 * 3. `GEBAI_BUILD_EXCLUDE_TOOLS`：排除教辅用不到的全局工具 19 个（文件/执行/编排/抓取/待办，
 *    以及 agent_load/agent_run——唯一子Agent 已预载，装载/新会话执行无对象；保留 show/ask/vision/full_mode）。
 * 4. `GEBAI_BUILD_EMBED_ENV`（模型配置内置）：从仓库根 .env 烘焙 GEBAI_LLM_ 与 GEBAI_VISION_ 前缀
 *    变量为启动默认值（接收方开箱即用；运行时同名变量仍可覆盖）。**密钥明文随二进制分发、可被
 *    持有者提取**——仅限受信任小范围分发，建议低额度专用 Key；编译后生成文件还原空态，密钥绝不入库。
 *
 * 产物（packages/desktop/dist/，每学段两个）：
 * - gebai-tutor-primary[.exe] / gebai-tutor-secondary[.exe]：浏览器形态
 * - gebai-tutor-primary-desktop.exe / gebai-tutor-secondary-desktop.exe：WebView 窗口形态
 *   （小学 47897 / 中学 47898 独立端口，应用身份与物化目录隔离，可同时运行）
 *
 * 生成文件（subagents.bundle / tools-excluded / env-embedded）与 launcher cargo target 在全部
 * 产物编译完成后统一还原全量/完整版态，工作区不留裁剪残留。
 *
 * 运行：bun run scripts/build-tutor-agent.ts（或 bun run build:tutor）
 */
import { spawnSync } from "node:child_process"
import { copyFileSync, statSync } from "node:fs"
import { join } from "node:path"

const repoRoot = join(import.meta.dirname, "..")
const serverDir = join(repoRoot, "packages", "server")
const webDir = join(repoRoot, "packages", "web")
const desktopDir = join(repoRoot, "packages", "desktop")
const bin = process.execPath // bun

interface TutorVariant {
  /** 学段名（日志用）。 */
  stage: string
  /** 本版打包并预载的子Agent。 */
  agent: string
  /** 产物主名（浏览器形态 exe 与 WebView 同目录文件名）。 */
  out: string
  /** WebView 启动器应用身份（物化目录/WebView 配置/非 Windows 同目录服务端文件名）。 */
  app: string
  /** WebView 窗口标题与 exe 资源 ProductName。 */
  title: string
  /** WebView 形态独立固定端口（与完整桌面端 47896、另一学段互不冲突，origin 各自稳定）。 */
  port: string
}

const VARIANTS: TutorVariant[] = [
  { stage: "小学", agent: "tutor_primary", out: "gebai-tutor-primary", app: "gebai-tutor-primary", title: "歌白教辅·小学", port: "47897" },
  { stage: "中学", agent: "tutor_secondary", out: "gebai-tutor-secondary", app: "gebai-tutor-secondary", title: "歌白教辅·中学", port: "47898" },
]

/** 全局工具排除清单：教辅用不到的文件/执行/编排/抓取能力；agent_load/agent_run 同样排除——
 *  每版只打包本学段唯一子Agent 且已预载，无第二 Agent 可装载/执行（保留 show/ask/vision/full_mode）。 */
const EXCLUDE_TOOLS = [
  "read", "write", "edit", "patch", "ls", "grep", "glob", "file", "diff",
  "sh", "sh_task", "py", "js",
  "flow", "tool_schemas", "fetch_url", "todo",
  "agent_load", "agent_run",
]

const run = (cmd: string, args: string[], cwd: string = repoRoot) => {
  console.log(`$ ${cmd} ${args.join(" ")}`)
  const r = spawnSync(cmd, args, { stdio: "inherit", cwd, env: { ...process.env } })
  if (r.status !== 0) {
    console.error(`[build-tutor-agent] 步骤失败（exit ${r.status ?? "?"}）: ${cmd} ${args.join(" ")}`)
    process.exit(r.status ?? 1)
  }
}
const mb = (p: string): string => `${(statSync(p).size / 1024 / 1024).toFixed(1)} MB`

// —— 1) 学段无关的一次性准备：图标 / Web UI / web bundle / 学段无关生成文件 ——
process.env.GEBAI_BUILD_EXCLUDE_TOOLS = EXCLUDE_TOOLS.join(",")
process.env.GEBAI_BUILD_EMBED_ENV = "1"
run(bin, ["run", "--cwd", desktopDir, "scripts/gen-icon.ts"])
run(bin, ["run", "--cwd", webDir, "build"])
run(bin, ["run", join(serverDir, "scripts", "build-web-bundle.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-tools.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-d2js.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-env-embed.ts")])

// —— 2) 逐学段：子Agent bundle（单 Agent + 预载）→ 双形态编译 → 图标/启动器 ——
const results: string[] = []
const launcherManifest = join(desktopDir, "launcher", "Cargo.toml")
const needLauncher = process.platform === "win32"
if (needLauncher) {
  const cargoCheck = spawnSync("cargo", ["--version"], { stdio: "ignore" })
  if (cargoCheck.status !== 0) {
    console.error("[build-tutor-agent] 未找到 cargo（WebView 启动器为 Rust 构建）：请先安装 Rust 工具链")
    process.exit(1)
  }
}
for (const v of VARIANTS) {
  console.log(`\n===== 学段构建：${v.stage}（${v.agent}，预载，端口 WebView ${v.port}）=====`)
  process.env.GEBAI_BUILD_SUBAGENTS = v.agent
  process.env.GEBAI_BUILD_PRELOAD = v.agent
  run(bin, ["run", join(serverDir, "scripts", "build-subagents.ts")])

  // 浏览器形态：编译 desktop 入口（固定端口 47896 + 自动开浏览器）
  const exeName = process.platform === "win32" ? `${v.out}.exe` : v.out
  const outfile = join(desktopDir, "dist", exeName)
  run(bin, ["build", join(desktopDir, "src", "index.ts"), "--compile", `--outfile=${outfile}`, "--external", "@terrastruct/d2"])
  if (process.platform === "win32") {
    run(bin, ["run", "--cwd", desktopDir, "scripts/embed-icon.ts", `dist/${exeName}`])
  }
  results.push(`${outfile}（${mb(outfile)}，浏览器形态）`)

  // WebView 窗口形态：launcher 变体参数内嵌本产物（独立身份/端口/标题）
  if (needLauncher) {
    process.env.GEBAI_LAUNCHER_SERVER_EXE = outfile
    process.env.GEBAI_LAUNCHER_APP_NAME = v.app
    process.env.GEBAI_LAUNCHER_TITLE = v.title
    process.env.GEBAI_LAUNCHER_PORT = v.port
    run("cargo", ["build", "--release", "--manifest-path", launcherManifest])
    const launcherOut = join(desktopDir, "dist", `${v.out}-desktop.exe`)
    copyFileSync(join(desktopDir, "launcher", "target", "release", "gebai-desktop.exe"), launcherOut)
    for (const key of ["GEBAI_LAUNCHER_SERVER_EXE", "GEBAI_LAUNCHER_APP_NAME", "GEBAI_LAUNCHER_TITLE", "GEBAI_LAUNCHER_PORT"]) {
      delete process.env[key]
    }
    results.push(`${launcherOut}（${mb(launcherOut)}，WebView 形态：身份 ${v.app}、端口 ${v.port}、标题「${v.title}」）`)
  }
}

// —— 3) 统一还原：生成文件回全量/空态，launcher target 回完整版内嵌（防 copy-launcher 误取变体）——
delete process.env.GEBAI_BUILD_SUBAGENTS
delete process.env.GEBAI_BUILD_PRELOAD
delete process.env.GEBAI_BUILD_EXCLUDE_TOOLS
delete process.env.GEBAI_BUILD_EMBED_ENV
run(bin, ["run", join(serverDir, "scripts", "build-subagents.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-tools.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-env-embed.ts")])
if (needLauncher) run("cargo", ["build", "--release", "--manifest-path", launcherManifest])

console.log(
  `\n[build-tutor-agent] 产物（${VARIANTS.length} 个学段专属桌面端，各只打包并预载本学段子Agent）：\n  ${results.join("\n  ")}` +
  `\n  排除全局工具: ${EXCLUDE_TOOLS.length} 个（文件/执行/编排/抓取/待办）｜模型配置: 从 .env 内置为启动默认（GEBAI_LLM_ / GEBAI_VISION_ 前缀）` +
  "\n  浏览器形态共用端口 47896（与完整桌面端互斥）；WebView 形态小学 47897 / 中学 47898，各自可与完整桌面端及另一学段同时运行" +
  "\n  数据目录 ~/.gebai 全部共享：小升初换用另一学段版即可，档案/错题本/掌握度延续" +
  "\n  已还原: 生成文件与 launcher cargo target 恢复全量/完整版态，工作区无裁剪残留",
)
