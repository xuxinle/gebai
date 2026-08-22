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
 * 产物：packages/desktop/dist/gebai-tutor[.exe]（浏览器形态、内嵌 Web UI 与应用图标）。
 * 注意：与完整桌面端共用固定端口 47896 与 ~/.gebai 数据目录——两者不可同时运行，
 * 教辅数据（档案/错题本/掌握度）与完整安装互通（用户=学习者模型）。
 * 生成文件（subagents.bundle / tools-excluded）在编译完成后**自动还原全量**，工作区不留裁剪态。
 *
 * 运行：bun run scripts/build-tutor-agent.ts（或 bun run build:tutor）
 */
import { spawnSync } from "node:child_process"
import { statSync } from "node:fs"
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

// 环境变量须先设置再跑生成脚本（子进程继承）
process.env.GEBAI_BUILD_SUBAGENTS = SUB_AGENTS.join(",")
process.env.GEBAI_BUILD_PRELOAD = PRELOAD.join(",")
process.env.GEBAI_BUILD_EXCLUDE_TOOLS = EXCLUDE_TOOLS.join(",")

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

// 5) 还原生成文件为全量态（subagents.bundle / tools-excluded 裁剪态不留工作区；与 build:code 样例
//    「下次常规构建自动恢复」不同，本脚本编译后立即还原，bun run test 等不受裁剪态影响）
delete process.env.GEBAI_BUILD_SUBAGENTS
delete process.env.GEBAI_BUILD_PRELOAD
delete process.env.GEBAI_BUILD_EXCLUDE_TOOLS
run(bin, ["run", join(serverDir, "scripts", "build-subagents.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-tools.ts")])

const size = statSync(outfile).size
console.log(
  `\n[build-tutor-agent] 产物: ${outfile}（${(size / 1024 / 1024).toFixed(1)} MB）\n` +
  `  子Agent: ${SUB_AGENTS.join(", ")}（预加载: ${PRELOAD.join(", ")}）｜排除全局工具: ${EXCLUDE_TOOLS.length} 个（文件/执行/编排/抓取/待办）\n` +
  "  运行形态: 桌面端浏览器形态（双击启动，自动打开浏览器；GEBAI_NO_OPEN=1 关闭）；固定端口 47896；数据目录 ~/.gebai\n" +
  "  注意: 与完整桌面端同端口同数据目录，不可同时运行；教辅数据与完整安装互通\n" +
  "  已还原: 生成文件（subagents.bundle / tools-excluded）恢复全量态，工作区无裁剪残留",
)
