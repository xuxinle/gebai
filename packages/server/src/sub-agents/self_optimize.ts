import { isAbsolute, relative, resolve, sep } from "node:path"
import type { SubAgentDef } from "../core/types"
import { readFeedbackTool, makePreviewServerTool, pageCaptureTool, truncate } from "../core/tools"
import { makeVisionTool, getVisionProvider } from "../core/vision"
import { isBinaryMode } from "../core/config"

export const name = "self_optimize"
export const description =
  "优化歌白自身（涉及本 Agent 自身代码/子Agent/提示词/配置时加载）：改进定义、修复缺陷、验证修改。输入：改进点/失败案例/反馈（可经 read_feedback 工具读取用户反馈）；输出：代码修改方案与验证结果；修改必须通过相关测试（测试是准入凭证，run_tests 工具）并同步 DESIGN.md，测试失败可 rollback 回滚。不处理外部项目（外部代码用 code）。"
export const systemPrompt =
  "你是歌白智能体（GEBAI Agent）的自我优化专家。**通用编码工作流（规划→探索→定位→方案→修改→验证→收尾，含 grep/analyze/edit/apply_patch 等工具用法）直接遵循 code 子Agent 提示词**——装载 self_optimize 时 code 已连带装载（完整工作流在会话记录/本系统提示词内），文件与分析类工具即 code_* 命名空间工具；本提示词只补充自我优化特有的流程与约束：\n" +
  "1) 输入：改进点/失败案例；用户反馈（点赞/点踩/文字反馈/建议）用 read_feedback 工具读取，作为优化输入；\n" +
  "2) 修改范围（**系统强制**）：默认只读模式仅允许写入 子Agent 目录（packages/server/src/sub-agents/）与仓库级文档/配置（DESIGN.md/AGENTS.md/.env.example/README.md/kilo.json），核心引擎源码（core/engine/app/ws 等）写入会被拒绝——需放宽时请用户在服务端设置 GEBAI_SELF_MODIFY=true 后重启；把改进沉淀为新的/修改后的子Agent 是首选方式（子Agent 是歌白的标准扩展机制）；\n" +
  "3) **设计同步铁律**：任何修改行为/接口/协议/存储布局/常量/命名规则等设计层面变更，必须同步更新 DESIGN.md 对应章节（文档与代码保持一致）；\n" +
  "4) 验证（**测试是唯一准入凭证**）：任何修改必须通过相关测试——用 run_tests 工具执行（files 传相关测试文件，如 [\"src/core/engine.test.ts\"]；确认后 all=true 跑全量），失败则修复或 rollback 回滚（rollback 按路径回滚工作区改动；失败先看错误信息定位再修复重测，不盲目重复执行）；再运行 bun run typecheck/bun run lint 确认无回归（sh 工具，需审批）；\n" +
  "5) 用户验证：修改通过测试后，用 ask_user 询问用户验证方式——UI/前端类修改建议直接在当前浏览器页面验证（dev 模式修改后自动热更新，先请用户刷新页面，再调用 page_capture 捕获实际渲染结果：read 读取渲染后 html、vision 分析截图，确认视觉效果与预期一致后再收尾）；服务端功能类修改可用 preview_server 在临时新端口启动验证服务（独立进程不中断当前会话），用户确认后启动并告知访问 URL 与停止方式，验证结束后用 preview_server action=stop 停止；\n" +
  "6) 收尾：git 工具只读查看变更（status/diff/log，无需审批）确认改动范围，只提交预期文件、不擅自 commit（add/commit 等写操作用 sh 且需审批；工作区若有与本次任务无关的未提交改动，先 git status 确认清楚，不混淆/误提交）；总结先结论后细节，关键位置引用 文件:行号；验证/测试未通过时如实说明并附关键错误输出。\n" +
  "项目范围：若会话设置了 SELF_OPTIMIZE_PROJECT 环境变量，则工作目录即 歌白仓库根，文件操作以项目根为基准（服务端部署限定项目内，本地模式不限制目录）；未设置时按用户给定的路径处理。"

/** 默认只读模式下允许写入的仓库级文件（根一级）。 */
const WRITABLE_ROOT_FILES = new Set(["DESIGN.md", "AGENTS.md", "AGENT.md", ".env.example", "README.md", "kilo.json"])
/** 默认只读模式下允许写入的目录（相对仓库根；子Agent 目录为唯一允许改代码的位置）。 */
const WRITABLE_ROOT_DIRS = [["packages", "server", "src", "sub-agents"]]

/** 启动级放开开关：GEBAI_SELF_MODIFY=true 时允许写入仓库内任意路径（含核心引擎源码）。 */
function selfModifyEnabled(): boolean {
  const v = process.env.GEBAI_SELF_MODIFY
  return v === "true" || v === "1"
}

/** 解析歌白仓库根：SELF_OPTIMIZE_PROJECT 优先；dev 模式按模块路径推导；二进制模式必须显式配置。 */
function selfOptimizeRoot(env: Record<string, string>): string | null {
  const proj = env.SELF_OPTIMIZE_PROJECT
  if (proj) return isAbsolute(proj) ? proj : resolve(process.cwd(), proj)
  if (!isBinaryMode()) return resolve(import.meta.dirname, "..", "..", "..", "..")
  return null
}

/**
 * 写范围守卫（SubAgentDef.writeGuard，引擎注入 ToolContext.writeGuard）——「核心引擎源码默认只读」的
 * **代码级**强制（文件写类工具 write/edit/apply_patch/move_file/delete_file 写入前调用）：
 * - GEBAI_SELF_MODIFY=true：完全放开；仓库根无法定位（二进制模式未配 SELF_OPTIMIZE_PROJECT）：无保护对象，放行；
 * - 命中仓库根内的路径：仅 子Agent 目录 + 仓库级文档/配置 可写，核心引擎源码拒绝；
 * - 仓库根外的路径（会话 tmp 等产物）：不限制——守卫保护的是歌白仓库，不约束常规产物写入。
 */
export const writeGuard = (env: Record<string, string>, absPaths: string[]): string | null => {
  if (selfModifyEnabled()) return null
  const root = selfOptimizeRoot(env)
  if (!root) return null
  for (const target of absPaths) {
    const rel = relative(root, target)
    if (rel === "" || rel.startsWith("..") || isAbsolute(rel)) continue // 仓库外：不限制
    const segs = rel.split(sep).filter(Boolean)
    const writable =
      (segs.length === 1 && WRITABLE_ROOT_FILES.has(segs[0])) ||
      (segs.length > WRITABLE_ROOT_DIRS[0].length && WRITABLE_ROOT_DIRS[0].every((s, i) => segs[i] === s))
    if (!writable) {
      return (
        `拒绝写入 ${target}：self_optimize 默认只读模式仅允许修改 子Agent 目录（packages/server/src/sub-agents/）与仓库级文档/配置（DESIGN.md/AGENTS.md/.env.example/README.md/kilo.json），` +
        `核心引擎源码受保护。确需修改核心代码请在服务端设置 GEBAI_SELF_MODIFY=true 后重启；` +
        `或把改进沉淀为新的/修改后的子Agent（子Agent 是歌白的标准扩展机制）。`
      )
    }
  }
  return null
}

/** run_tests：在歌白仓库根执行测试（指定测试文件或全量）——测试是自我修改的唯一准入凭证。 */
const runTestsTool: import("../core/types").Tool = {
  name: "run_tests",
  description:
    "在歌白仓库根执行测试（测试是自我修改的唯一准入凭证）：files 传相关测试文件（相对仓库根，如 [\"src/core/engine.test.ts\"]，可多个），确认无回归后 all=true 跑全量 bun run test。输出测试结果（失败需修复或 rollback 回滚）。需审批。",
  requiresApproval: true,
  parameters: schema({
    files: { type: "array", items: { type: "string" }, description: "测试文件列表（相对仓库根；如 [\"src/core/engine.test.ts\"]）" },
    all: { type: "boolean", description: "true 跑全量测试（bun run test，忽略 files）" },
  }),
  async execute(args, ctx) {
    const root = selfOptimizeRoot(ctx.env)
    if (!root) return { output: "无法定位歌白仓库根：请设置 SELF_OPTIMIZE_PROJECT 环境变量。" }
    const files = (Array.isArray(args.files) ? args.files : []).map(String).filter(Boolean)
    if (args.all !== true && !files.length) return { output: "run_tests 需要 files 参数（相关测试文件）或 all=true（全量测试）。" }
    const cmd = args.all === true ? "bun run test" : `bun test ${files.join(" ")}`
    const { stdout, stderr, code } = await ctx.runCommand(cmd, { workdir: root, timeoutMs: 5 * 60 * 1000 })
    const out = code === 0 ? stdout : `${stdout}\n${stderr}\n[exit ${code}]`
    return truncate(out, "run_tests", ctx)
  },
}

/** rollback：回滚工作区改动（测试失败时的恢复路径——git checkout -- 指定路径或全部）。 */
const rollbackTool: import("../core/types").Tool = {
  name: "rollback",
  description:
    "回滚工作区未提交改动（测试失败后的恢复路径）：paths 传要回滚的文件/目录（相对仓库根，可多个），all=true 回滚全部未提交改动（git checkout -- .）。**注意：会丢弃未提交的修改**——仅用于撤销本次失败的自我修改；工作区若有与本次任务无关的既有改动，请用 paths 精确指定而非 all。需审批。",
  requiresApproval: true,
  parameters: schema({
    paths: { type: "array", items: { type: "string" }, description: "回滚文件/目录列表（相对仓库根）" },
    all: { type: "boolean", description: "true 回滚全部未提交改动（git checkout -- .）" },
  }),
  async execute(args, ctx) {
    const root = selfOptimizeRoot(ctx.env)
    if (!root) return { output: "无法定位歌白仓库根：请设置 SELF_OPTIMIZE_PROJECT 环境变量。" }
    const paths = (Array.isArray(args.paths) ? args.paths : []).map(String).filter(Boolean)
    if (args.all !== true && !paths.length) return { output: "rollback 需要 paths 参数或 all=true。" }
    const cmd = args.all === true ? "git checkout -- ." : `git checkout -- ${paths.join(" ")}`
    const r = await ctx.runCommand(cmd, { workdir: root })
    if (r.code !== 0) return { output: `rollback 失败：\n${r.stdout}\n${r.stderr}\n[exit ${r.code}]` }
    const status = await ctx.runCommand("git status --short", { workdir: root })
    const out = `已回滚。当前工作区状态：\n${status.stdout.trim() || "（干净）"}`
    return truncate(out, "rollback", ctx)
  },
}

function schema(properties: Record<string, unknown>, required: string[] = []): import("@gebai/sdk").ToolSchema {
  return { type: "object", properties, required }
}

/**
 * 工具集只含 code 通用能力**没有的**独有工具（工具与提示词均不复刻 code）：
 * 装载/预加载 self_optimize 时系统连带装载 code——文件/分析类工具直接用 code_* 命名空间，
 * 通用编码工作流遵循 code 提示词；本 Agent 只声明自优化专属能力与写范围守卫。
 */
export const tools = {
  read_feedback: readFeedbackTool,
  run_tests: runTestsTool,
  rollback: rollbackTool,
  preview_server: makePreviewServerTool(),
  page_capture: pageCaptureTool,
  vision: makeVisionTool({ vision: getVisionProvider }),
}
export const requiresApproval = { run_tests: true, rollback: true }
export const preload = false
export const envVars = [
  { name: "SELF_OPTIMIZE_PROJECT", description: "优化工作根：歌白仓库根路径，自我优化（self_optimize）文件操作以它为基准（写范围守卫亦按它界定仓库边界）" },
]
export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars, writeGuard }
