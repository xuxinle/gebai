/**
 * 教辅（tutor）场景裁剪构建样例（根 scripts/ 目录）——产出一个面向中小学学习辅导的
 * **桌面端**精简单文件二进制（bun --compile 编译 desktop 入口：固定端口 47896、启动自动
 * 打开系统浏览器、数据目录 ~/.gebai；与 packages/desktop 的完整桌面端同形态）。
 * 模板出处见 scripts/build-code-agent.ts（code 场景样例）；本脚本是其桌面端变体：
 * 编译 desktop 入口（而非 server 入口）+ 教辅裁剪清单 + 构建后自动还原生成文件。
 *
 * 三层裁剪维度（DESIGN「构建期裁剪与预加载指定」）：
 * 1. `GEBAI_BUILD_SUBAGENTS`（子Agent 包含清单）——只打包 tutor_primary + tutor_secondary，
 *    体积收益主来源：code/playwright/feishu_docs 等模块整体摇出产物；
 *    连带跳过重型内嵌产物生成（analyzer-wasm/tree-sitter、playwright driver 与 playwright-core）；
 * 2. `GEBAI_BUILD_PRELOAD`（预加载清单）——两个学段 Agent 烘焙 preload=true，开箱即辅导
 *    （新会话即有引导式解题与错题本能力，无需模型先 agent_load）；
 * 3. `GEBAI_BUILD_EXCLUDE_TOOLS`（全局工具排除清单）——教辅设备用不到的能力不注册不暴露：
 *    文件类（read/write/edit/patch/ls/grep/glob/file/diff）、执行类（sh/sh_task/py/js）、
 *    编排类（flow/tool_schemas）、网络抓取（fetch_url）、待办（todo）；
 *    保留 show（练习页/图表展示）、ask（询问/计划）、vision（作业拍照识别）、
 *    agent_load/agent_run（引擎机制）、full_mode（极简模式逃生口）。
 *
 * 4. `GEBAI_BUILD_EMBED_ENV`（模型配置内置）——从仓库根 .env 烘焙 GEBAI_LLM_ 与 GEBAI_VISION_ 前缀
 *    变量为启动默认值（接收方开箱即用；运行时环境变量与前端/任务级 env 仍可覆盖）。**密钥明文随二进制
 *    分发、可被持有者提取**——仅限受信任小范围分发，建议低额度专用 Key；编译后生成文件还原空态，
 *    密钥绝不入库。
 *
 * 产物（Windows 两个）：
 * - packages/desktop/dist/gebai-tutor[.exe]：浏览器形态（双击启动自动开浏览器）
 * - packages/desktop/dist/gebai-tutor-desktop[.exe]：原生 WebView 窗口形态（tao/wry 启动器内嵌
 *   本产物；独立应用身份：物化数据目录 gebai-tutor、固定端口 47897、窗口标题「歌白教辅」——
 *   与完整桌面端（gebai/47896）互不冲突可同时运行；launcher 变体参数见 launcher/build.rs）
 *
 * 注意：浏览器形态产物与完整桌面端共用固定端口 47896 与 ~/.gebai 数据目录——两者不可同时运行，
 * 教辅数据（档案/错题本/掌握度）与完整安装互通（用户=学习者模型）。
 * 生成文件（subagents.bundle / tools-excluded）在编译完成后**自动还原全量**，工作区不留裁剪态；
 * launcher 的 cargo target 同样还原为完整版内嵌（防后续 copy-launcher 误取变体产物）。
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

// —— 裁剪清单（定制场景产物改这三处；未知名字构建直接失败并列出可用名单）——
/** 子Agent 包含清单：tutor_primary=小学（具象化教学）、tutor_secondary=中学（引导式解题）。 */
const SUB_AGENTS = ["tutor_primary", "tutor_secondary"]
/** 预加载清单：教辅设备开箱即辅导——两个学段 Agent 均启动即装载。 */
const PRELOAD = ["tutor_primary", "tutor_secondary"]
/** 全局工具排除清单：教辅用不到的文件/执行/编排/抓取能力（保留 show/ask/vision/agent_load/agent_run/full_mode）。 */
const EXCLUDE_TOOLS = [
  "read", "write", "edit", "patch", "ls", "grep", "glob", "file", "diff",
  "sh", "sh_task", "py", "js",
  "flow", "tool_schemas", "fetch_url", "todo",
]

const run = (cmd: string, args: string[], cwd: string = repoRoot) => {
  console.log(`$ ${cmd} ${args.join(" ")}`)
  const r = spawnSync(cmd, args, { stdio: "inherit", cwd, env: { ...process.env } })
  if (r.status !== 0) {
    console.error(`[build-tutor-agent] 步骤失败（exit ${r.status ?? "?"}）: ${cmd} ${args.join(" ")}`)
    process.exit(r.status ?? 1)
  }
}

// 环境变量须先设置再跑生成脚本（子进程继承）；GEBAI_BUILD_EMBED_ENV=1 内置模型配置
// （GEBAI_LLM_ 与 GEBAI_VISION_ 前缀，来源仓库根 .env）——接收方开箱即用，运行时同名变量仍可覆盖
process.env.GEBAI_BUILD_SUBAGENTS = SUB_AGENTS.join(",")
process.env.GEBAI_BUILD_PRELOAD = PRELOAD.join(",")
process.env.GEBAI_BUILD_EXCLUDE_TOOLS = EXCLUDE_TOOLS.join(",")
process.env.GEBAI_BUILD_EMBED_ENV = "1"

// 1) Web UI 构建并内嵌（桌面端浏览器形态的前端）+ 应用图标
run(bin, ["run", "--cwd", desktopDir, "scripts/gen-icon.ts"])
run(bin, ["run", "--cwd", webDir, "build"])
run(bin, ["run", join(serverDir, "scripts", "build-web-bundle.ts")])

// 2) 生成脚本链（消费裁剪清单环境变量，烘焙进生成文件）
//    跳过重型内嵌：analyzer-wasm（tree-sitter，code 子Agent 专属）、driver/pwcore（playwright 专属）——
//    对应子Agent 未打包，内嵌产物无人消费，纯体积浪费
run(bin, ["run", join(serverDir, "scripts", "build-subagents.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-tools.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-d2js.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-env-embed.ts")])

// 3) 单文件编译 desktop 入口（固定端口 47896 + 自动开浏览器；--external @terrastruct/d2 同完整桌面端）
const exeName = process.platform === "win32" ? "gebai-tutor.exe" : "gebai-tutor"
const outfile = join(desktopDir, "dist", exeName)
run(bin, [
  "build", join(desktopDir, "src", "index.ts"),
  "--compile", `--outfile=${outfile}`, "--external", "@terrastruct/d2",
])

// 4) Windows exe 图标嵌入（同完整桌面端；非 Windows 跳过）
if (process.platform === "win32") {
  run(bin, ["run", "--cwd", desktopDir, "scripts/embed-icon.ts", `dist/${exeName}`])
}

// 5) 原生 WebView 启动器（仅 Windows：变体参数经 launcher/build.rs 烘焙——内嵌本产物、
//    独立应用身份 gebai-tutor（物化目录/WebView 配置隔离）、固定端口 47897、标题「歌白教辅」）
let launcherOut: string | null = null
if (process.platform === "win32") {
  const cargoCheck = spawnSync("cargo", ["--version"], { stdio: "ignore" })
  if (cargoCheck.status !== 0) {
    console.error("[build-tutor-agent] 未找到 cargo（WebView 启动器为 Rust 构建）：请先安装 Rust 工具链")
    process.exit(1)
  }
  const manifest = join(desktopDir, "launcher", "Cargo.toml")
  process.env.GEBAI_LAUNCHER_SERVER_EXE = outfile
  process.env.GEBAI_LAUNCHER_APP_NAME = "gebai-tutor"
  process.env.GEBAI_LAUNCHER_TITLE = "歌白教辅"
  process.env.GEBAI_LAUNCHER_PORT = "47897"
  run("cargo", ["build", "--release", "--manifest-path", manifest])
  launcherOut = join(desktopDir, "dist", "gebai-tutor-desktop.exe")
  copyFileSync(join(desktopDir, "launcher", "target", "release", "gebai-desktop.exe"), launcherOut)
  // 还原默认 launcher 构建（target 恢复完整版内嵌，防后续 copy-launcher 误取变体产物）
  for (const key of ["GEBAI_LAUNCHER_SERVER_EXE", "GEBAI_LAUNCHER_APP_NAME", "GEBAI_LAUNCHER_TITLE", "GEBAI_LAUNCHER_PORT"]) {
    delete process.env[key]
  }
  run("cargo", ["build", "--release", "--manifest-path", manifest])
}

// 6) 还原生成文件为全量态（subagents.bundle / tools-excluded 裁剪态不留工作区；与 build:code 样例
//    「下次常规构建自动恢复」不同，本脚本编译后立即还原，bun run test 等不受裁剪态影响）；
//    env-embedded 还原为空态——**非空态含 .env 密钥明文，绝不允许残留/提交**
delete process.env.GEBAI_BUILD_SUBAGENTS
delete process.env.GEBAI_BUILD_PRELOAD
delete process.env.GEBAI_BUILD_EXCLUDE_TOOLS
delete process.env.GEBAI_BUILD_EMBED_ENV
run(bin, ["run", join(serverDir, "scripts", "build-subagents.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-tools.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-env-embed.ts")])

const size = statSync(outfile).size
const launcherSize = launcherOut ? statSync(launcherOut).size : 0
console.log(
  `\n[build-tutor-agent] 产物: ${outfile}（${(size / 1024 / 1024).toFixed(1)} MB，浏览器形态）` +
  (launcherOut ? `\n  产物: ${launcherOut}（${(launcherSize / 1024 / 1024).toFixed(1)} MB，WebView 窗口形态：独立身份 gebai-tutor、端口 47897、标题「歌白教辅」）` : "") +
  `\n  子Agent: ${SUB_AGENTS.join(", ")}（预加载: ${PRELOAD.join(", ")}）｜排除全局工具: ${EXCLUDE_TOOLS.length} 个（文件/执行/编排/抓取/待办）\n` +
  "  浏览器形态: 双击启动自动打开浏览器；固定端口 47896，与完整桌面端同端口同数据目录（~/.gebai）不可同时运行\n" +
  "  WebView 形态: 独立端口 47897 与物化目录，可与完整桌面端同时运行；教辅数据与完整安装互通\n" +
  "  已还原: 生成文件与 launcher cargo target 恢复全量/完整版态，工作区无裁剪残留",
)
