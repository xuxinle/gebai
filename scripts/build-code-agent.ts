/**
 * code 场景裁剪构建样例（根 scripts/ 目录）——产出一个面向编码场景的精简 GEBAI 服务端
 * 单文件二进制（bun --compile，浏览器形态、内嵌 Web UI，GEBAI_HOME=~/.gebai）。
 * **作为其他裁剪构建的模板**：复制本脚本，改「裁剪清单」三处即可产出不同场景产物
 * （如 reverse_site 站点逆向、feishu 文档、只读分析等）。
 *
 * 三层裁剪维度（DESIGN「构建期裁剪与预加载指定」）：
 * 1. `GEBAI_BUILD_SUBAGENTS`（子Agent 包含清单）——只打包场景所需子Agent。**体积收益主来源**：
 *    生成注册表不 import 未选子Agent，playwright/feishu_docs/widgets 等模块整体摇出产物；
 * 2. `GEBAI_BUILD_PRELOAD`（预加载清单）——烘焙 preload=true，启动即装载，用户开箱即用；
 * 3. `GEBAI_BUILD_EXCLUDE_TOOLS`（全局工具排除清单）——场景用不到的全局工具不注册不暴露
 *    （schema 不可见、调用报未知工具；agent_run 内建编排工具 tool_schemas/js 与 vision 同规则）。
 *    语义注意：全局工具排除是**能力裁剪**——实现与工具表同模块仍会打包（无法摇树）。
 *
 * 产物：packages/server/dist/gebai-code[.exe]。生成文件（subagents.bundle / tools-excluded /
 * web.bundle / 各内嵌产物）已按裁剪清单重写——下次常规构建（bun run build）自动恢复全量，无须手工还原。
 *
 * 运行：bun run scripts/build-code-agent.ts（或 bun run build:code）
 */
import { spawnSync } from "node:child_process"
import { statSync } from "node:fs"
import { join } from "node:path"

const repoRoot = join(import.meta.dirname, "..")
const serverDir = join(repoRoot, "packages", "server")
const webDir = join(repoRoot, "packages", "web")
const bin = process.execPath // bun

// —— 裁剪清单（定制场景产物改这三处；未知名字构建直接失败并列出可用名单）——
/** 子Agent 包含清单：code=编码工作流（read/edit/grep/git/search_symbols 等 + 项目绑定），explore=只读代码库探索。 */
const SUB_AGENTS = ["code", "explore"]
/** 预加载清单：启动即装载 code（浏览器打开即编码就绪）；explore 按需 agent_load。 */
const PRELOAD = ["code"]
/** 全局工具排除清单：编码场景用不到的能力——show（内容展示：图表/HTML/文件）/ fetch_url（网页抓取）。 */
const EXCLUDE_TOOLS = ["show", "fetch_url"]

// 环境变量须先设置再跑生成脚本（子进程继承）
process.env.GEBAI_BUILD_SUBAGENTS = SUB_AGENTS.join(",")
process.env.GEBAI_BUILD_PRELOAD = PRELOAD.join(",")
process.env.GEBAI_BUILD_EXCLUDE_TOOLS = EXCLUDE_TOOLS.join(",")

const run = (cmd: string, args: string[], cwd: string = repoRoot) => {
  console.log(`$ ${cmd} ${args.join(" ")}`)
  const r = spawnSync(cmd, args, { stdio: "inherit", cwd, env: { ...process.env } })
  if (r.status !== 0) {
    console.error(`[build-code-agent] 步骤失败（exit ${r.status ?? "?"}）: ${cmd} ${args.join(" ")}`)
    process.exit(r.status ?? 1)
  }
}

// 1) Web UI 构建并内嵌（浏览器形态产物的前端）
run(bin, ["run", "--cwd", webDir, "build"])
run(bin, ["run", join(serverDir, "scripts", "build-web-bundle.ts")])

// 2) 生成脚本链（消费上面三个环境变量，把裁剪清单烘焙进生成文件）
run(bin, ["run", join(serverDir, "scripts", "build-subagents.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-tools.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-d2js.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-analyzer-wasm.ts")])
run(bin, ["run", join(serverDir, "scripts", "build-driver-embed.ts")])

// 3) 单文件编译（与 desktop server:build 同参数：--external @terrastruct/d2 为动态依赖，show 未排除时亦可运行时装载）
const outfile = join(serverDir, "dist", process.platform === "win32" ? "gebai-code.exe" : "gebai-code")
run(bin, ["build", join(serverDir, "src", "index.ts"), "--compile", `--outfile=${outfile}`, "--external", "@terrastruct/d2"])

const size = statSync(outfile).size
console.log(
  `\n[build-code-agent] 产物: ${outfile}（${(size / 1024 / 1024).toFixed(1)} MB）\n` +
    `  子Agent: ${SUB_AGENTS.join(", ")}（预加载: ${PRELOAD.join(", ")}）｜排除全局工具: ${EXCLUDE_TOOLS.join(", ") || "无"}\n` +
    "  运行形态: 浏览器自动打开（GEBAI_NO_OPEN=1 关闭）；数据目录 ~/.gebai（GEBAI_HOME 覆盖）\n" +
    "  注意: 生成文件已按裁剪清单重写，下次常规构建（bun run build）自动恢复全量",
)
