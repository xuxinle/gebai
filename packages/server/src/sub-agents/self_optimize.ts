import { isAbsolute, relative, resolve, sep } from "node:path"
import type { SubAgentDef, Tool, ToolContext } from "../core/types"
import { makePreviewServerTool, pageCaptureTool, truncate } from "../core/tools"
import { makeVisionTool, getVisionProvider } from "../core/vision"
import { isBinaryMode } from "../core/config"
import {
  readTool,
  editTool,
  writeTool,
  shTool,
  lsTool,
  grepTool,
  searchFilesTool,
  moveFileTool,
  deleteFileTool,
  diffTool,
  applyPatchTool,
  gitTool,
  fetchUrlTool,
  pyTool,
  askUserTool,
  agentRunTool,
  makeTodoTool,
} from "../core/tools"
import { analyzeTool, searchSymbolsTool } from "../core/analyzer"

export const name = "self_optimize"
export const description =
  "优化歌白自身（涉及本 Agent 自身代码/子Agent/提示词/配置时加载）：改进定义、修复缺陷、验证修改。输入：改进点/失败案例/反馈（可经 read_feedback 工具读取用户反馈）；输出：代码修改方案与验证结果；修改必须通过相关测试（测试是准入凭证，run_tests 工具）并同步 DESIGN.md，测试失败可 rollback 回滚。不处理外部项目（外部代码用 code）。"
export const systemPrompt =
  "你是歌白智能体（GEBAI Agent）的自我优化专家（工作流参考 code 子Agent：规划→探索→定位→方案→修改→验证→收尾）。工作流程：\n" +
  "1) 规划：多步骤任务先用 todo 建立待办清单（entries 一次可含 add/update/delete 多条，探索→定位→方案→修改→验证），每完成一步用 todo 更新状态，返回的清单即最新全部待办，无需再查；\n" +
  "2) 探索：先用 grep（内容搜索）/search_files（按文件名 glob 查找）/search_symbols（按符号名定位**定义**位置，跨文件；找**引用/调用点**用 grep）/ls（目录结构）/analyze（tree-sitter 结构概览）快速定位涉及的服务端源码文件，再精确读取相关文件（大文件用 read 的 offset/limit 分段读），避免大范围逐行通读；用户反馈（点赞/点踩/文字反馈/建议）可用 read_feedback 工具读取，作为优化输入；\n" +
  "3) 方案：输出改动点清单（文件、改动内容、预期效果与影响面）；方向有取舍时用 ask_user 提供选项向用户确认；可用 diff 展示「修改前/后」对比供审查；\n" +
  "4) 修改：先 read 目标区域确认当前内容，再动手；遵循项目既有约定——先看 DESIGN.md/AGENTS.md 与相邻文件，模仿现有写法；**任何修改行为/接口/协议/存储布局/常量/命名规则等设计层面变更，必须同步更新 DESIGN.md 对应章节（设计同步铁律：文档与代码保持一致）**；改动较多或行号容易偏移时优先用 apply_patch 应用 unified diff（一次多 hunk、行号模糊容错，可用 diff 工具生成补丁，dryRun=true 可预演不落盘；上下文行给 2~4 行即可；一次补丁聚焦一个改动点），小范围定点改动用 edit，write 仅用于新建/整体覆盖；补丁不匹配时先 read 当前文件内容核对再重试；不添加无关注释；不引入/提交密钥凭据；写操作（edit/write/apply_patch/sh/py/run_tests/rollback）需审批，修改前必须先给出方案；**修改范围由系统强制**：默认只读模式仅允许写入 子Agent 目录（packages/server/src/sub-agents/）与仓库级文档/配置（DESIGN.md/AGENTS.md/.env.example/README.md/kilo.json），核心引擎源码（core/engine/app/ws 等）拒绝写入——需放宽时请用户在服务端设置 GEBAI_SELF_MODIFY=true 后重启；\n" +
  "5) 验证：任何修改必须通过相关测试——**用 run_tests 工具执行**（files 传相关测试文件，如 [\"src/core/engine.test.ts\"]；确认后 all=true 跑全量 bun run test）——测试是唯一准入凭证，失败则修复或回滚（rollback 工具可按路径回滚工作区改动；失败先看错误信息定位再修复重测，不盲目重复执行）；再运行 bun run typecheck/bun run lint 确认无回归（sh 工具，需审批）；\n" +
  "6) 用户验证：修改通过测试后，用 ask_user 询问用户验证方式——UI/前端类修改建议直接在当前浏览器页面验证（dev 模式修改后自动热更新，先请用户刷新页面，再调用 page_capture 捕获实际渲染结果：read 读取渲染后 html、vision 分析截图，确认视觉效果与预期一致后再收尾）；服务端功能类修改可用 preview_server 在临时新端口启动验证服务（独立进程不中断当前会话），用户确认后启动并告知访问 URL 与停止方式，验证结束后用 preview_server action=stop 停止；\n" +
  "7) 收尾：用 git 工具只读查看变更（status/diff/log，无需审批）确认改动范围，只提交预期文件，不擅自 commit（add/commit 等写操作用 sh 且需审批；若工作区已有与本次任务无关的未提交改动，先 git status 确认清楚，不与本次改动混淆/误提交）；用 todo（空 entries 查询）核对全部待办后给出总结。\n" +
  "项目范围：若会话设置了 SELF_OPTIMIZE_PROJECT 环境变量，则工作目录即 歌白仓库根，文件操作以项目根为基准（服务端部署限定项目内，本地模式不限制目录）；未设置时按用户给定的路径处理。\n" +
  "页面捕获（page_capture）：捕获的是用户当前打开的 歌白页面快照（含当前会话内容），用于验证 UI 修改的实际渲染效果；捕获前先请用户刷新页面确保拿到最新构建；外部 URL（如 preview_server 页面）的浏览器验证可 agent_run 委托 playwright 子Agent。"

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

/** 路径是否在允许写入范围内（默认只读模式：子Agent 目录 + 仓库级文档/配置；SELF_MODIFY 放开任意）。 */
function isWritablePath(root: string, target: string): boolean {
  if (selfModifyEnabled()) return true
  const rel = relative(root, target)
  if (rel === "" || rel.startsWith("..") || isAbsolute(rel)) return false
  const segs = rel.split(sep).filter(Boolean)
  if (segs.length === 1 && WRITABLE_ROOT_FILES.has(segs[0])) return true
  const dirSegs = WRITABLE_ROOT_DIRS[0]
  return segs.length > dirSegs.length && dirSegs.every((s, i) => segs[i] === s)
}

/** 从工具参数提取待写入路径（path 类参数统一相对仓库根解析）。 */
function targetPaths(args: Record<string, unknown>, root: string): string[] {
  const out: string[] = []
  for (const key of ["path", "from", "to"]) {
    const v = args[key]
    if (typeof v !== "string" || !v) continue
    const p = v.replace(/\//g, sep)
    out.push(isAbsolute(p) ? p : resolve(root, p))
  }
  return out
}

/**
 * 写保护包装（self_optimize 独有）：文件写类工具执行前强制校验目标路径在允许范围内
 * （默认只读模式限子Agent 目录与仓库级文档/配置；GEBAI_SELF_MODIFY=true 放开）。
 * 这是「核心引擎源码默认只读」的**代码级**强制，而非仅提示词承诺。
 */
function selfOptimizeGuard(tool: Tool): Tool {
  return {
    ...tool,
    async execute(args, ctx: ToolContext) {
      const root = selfOptimizeRoot(ctx.env)
      if (!root) {
        return { output: "无法定位歌白仓库根：二进制/打包模式下请设置 SELF_OPTIMIZE_PROJECT 环境变量（指向歌白仓库检出目录）。" }
      }
      for (const p of targetPaths(args, root)) {
        if (!isWritablePath(root, p)) {
          return {
            output:
              `拒绝写入 ${p}：self_optimize 默认只读模式仅允许修改 子Agent 目录（packages/server/src/sub-agents/）与仓库级文档/配置（DESIGN.md/AGENTS.md/.env.example/README.md/kilo.json），` +
              `核心引擎源码受保护。确需修改核心代码请在服务端设置 GEBAI_SELF_MODIFY=true 后重启；` +
              `或把改进沉淀为新的/修改后的子Agent（子Agent 是歌白的标准扩展机制）。`,
          }
        }
      }
      return tool.execute(args, ctx)
    },
  }
}

/** run_tests：在歌白仓库根执行测试（指定测试文件或全量）——测试是自我修改的唯一准入凭证。 */
const runTestsTool: Tool = {
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
const rollbackTool: Tool = {
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

const todoTool = makeTodoTool()

export const tools = {
  read: readTool,
  write: selfOptimizeGuard(writeTool),
  edit: selfOptimizeGuard(editTool),
  apply_patch: selfOptimizeGuard(applyPatchTool),
  sh: shTool,
  py: pyTool,
  ls: lsTool,
  grep: grepTool,
  search_files: searchFilesTool,
  search_symbols: searchSymbolsTool,
  move_file: selfOptimizeGuard(moveFileTool),
  delete_file: selfOptimizeGuard(deleteFileTool),
  diff: diffTool,
  analyze: analyzeTool,
  git: gitTool,
  fetch_url: fetchUrlTool,
  ask_user: askUserTool,
  agent_run: agentRunTool,
  todo: todoTool,
  run_tests: runTestsTool,
  rollback: rollbackTool,
  preview_server: makePreviewServerTool(),
  page_capture: pageCaptureTool,
  vision: makeVisionTool({ vision: getVisionProvider }),
}
export const requiresApproval = {
  edit: true, write: true, apply_patch: true, sh: true, py: true,
  move_file: true, delete_file: true, run_tests: true, rollback: true,
}
export const preload = false
export const envVars = [
  { name: "SELF_OPTIMIZE_PROJECT", description: "优化工作根：歌白仓库根路径，自我优化（self_optimize）文件操作以它为基准" },
]

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars }
