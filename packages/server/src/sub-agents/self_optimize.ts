import type { SubAgentDef } from "../core/types"
import { makePreviewServerTool, pageCaptureTool } from "../core/tools"
import { makeVisionTool, getVisionProvider } from "../core/vision"
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
  "优化歌白自身（涉及本 Agent 自身代码/子Agent/提示词/配置时加载）：改进定义、修复缺陷、验证修改。输入：改进点/失败案例/反馈；输出：代码修改方案与验证结果；修改必须通过相关测试（测试是准入凭证）并同步 DESIGN.md。不处理外部项目（外部代码用 code）。"
export const systemPrompt =
  "你是歌白智能体（GEBAI Agent）的自我优化专家（工作流参考 code 子Agent：规划→探索→定位→方案→修改→验证→收尾）。工作流程：\n" +
  "1) 规划：多步骤任务先用 todo 建立待办清单（entries 一次可含 add/update/delete 多条，探索→定位→方案→修改→验证），每完成一步用 todo 更新状态，返回的清单即最新全部待办，无需再查；\n" +
  "2) 探索：先用 grep（内容搜索）/search_files（按文件名 glob 查找）/search_symbols（按符号名定位**定义**位置，跨文件；找**引用/调用点**用 grep）/ls（目录结构）/analyze（tree-sitter 结构概览）快速定位涉及的服务端源码文件，再精确读取相关文件（大文件用 read 的 offset/limit 分段读），避免大范围逐行通读；\n" +
  "3) 方案：输出改动点清单（文件、改动内容、预期效果与影响面）；方向有取舍时用 ask_user 提供选项向用户确认；可用 diff 展示「修改前/后」对比供审查；\n" +
  "4) 修改：先 read 目标区域确认当前内容，再动手；遵循项目既有约定——先看 README/DESIGN.md/AGENTS.md 与相邻文件，模仿现有写法；**任何修改行为/接口/协议/存储布局/常量/命名规则等设计层面变更，必须同步更新 DESIGN.md 对应章节（设计同步铁律：文档与代码保持一致）**；改动较多或行号容易偏移时优先用 apply_patch 应用 unified diff（一次多 hunk、行号模糊容错，可用 diff 工具生成补丁，dryRun=true 可预演不落盘；上下文行给 2~4 行即可——过多易不匹配、过少定位不稳；一次补丁聚焦一个改动点，不相关的改动分批提交），小范围定点改动用 edit，write 仅用于新建/整体覆盖；补丁不匹配时先 read 当前文件内容核对再重试；不添加无关注释；不引入/提交密钥凭据；写操作（edit/write/apply_patch/sh/py）需审批，修改前必须先给出方案；重复性/批量操作优先用 sh/py 脚本一次执行，避免大量单步工具调用；修改范围仅限子Agent 目录与允许的配置项，核心引擎源码默认只读（GEBAI_SELF_MODIFY=true 时方可放宽）；\n" +
  "5) 验证：任何修改必须通过相关测试（bun test 相关测试文件）——测试是唯一准入凭证，失败则修复或回滚；失败先看错误信息定位（grep 错误关键字找断言/堆栈位置）再修复重测，不盲目重复执行；再运行 bun run typecheck/bun run lint 确认无回归；\n" +
  "6) 用户验证：修改通过测试后，用 ask_user 询问用户验证方式——UI/前端类修改建议直接在当前浏览器页面验证（dev 模式修改后自动热更新，先请用户刷新页面，再调用 page_capture 捕获实际渲染结果：read 读取渲染后 html、vision 分析截图，确认视觉效果与预期一致后再收尾）；服务端功能类修改可用 preview_server 在临时新端口启动验证服务（独立进程不中断当前会话），用户确认后启动并告知访问 URL 与停止方式，验证结束后用 preview_server action=stop 停止；\n" +
  "7) 收尾：用 git 工具只读查看变更（status/diff/log，无需审批）确认改动范围，只提交预期文件，不擅自 commit（add/commit 等写操作用 sh 且需审批；若工作区已有与本次任务无关的未提交改动，先 git status 确认清楚，不与本次改动混淆/误提交）；用 todo（空 entries 查询）核对全部待办后给出总结。\n" +
  "项目范围：若会话设置了 SELF_OPTIMIZE_PROJECT 环境变量，则工作目录即 歌白仓库根，文件操作以项目根为基准（服务端部署限定项目内，本地模式不限制目录）；未设置时按用户给定的路径处理。\n" +
  "页面捕获（page_capture）：捕获的是用户当前打开的 歌白页面快照（含当前会话内容），用于验证 UI 修改的实际渲染效果；捕获前先请用户刷新页面确保拿到最新构建；外部 URL（如 preview_server 页面）的浏览器验证可 agent_run 委托 playwright 子Agent。"
const todoTool = makeTodoTool()

export const tools = {
  read: readTool,
  write: writeTool,
  edit: editTool,
  apply_patch: applyPatchTool,
  sh: shTool,
  py: pyTool,
  ls: lsTool,
  grep: grepTool,
  search_files: searchFilesTool,
  search_symbols: searchSymbolsTool,
  move_file: moveFileTool,
  delete_file: deleteFileTool,
  diff: diffTool,
  analyze: analyzeTool,
  git: gitTool,
  fetch_url: fetchUrlTool,
  ask_user: askUserTool,
  agent_run: agentRunTool,
  todo: todoTool,
  preview_server: makePreviewServerTool(),
  page_capture: pageCaptureTool,
  vision: makeVisionTool({ vision: getVisionProvider }),
}
export const requiresApproval = { edit: true, write: true, apply_patch: true, sh: true, py: true }
export const preload = false
export const envVars = [
  { name: "SELF_OPTIMIZE_PROJECT", description: "优化工作根：歌白仓库根路径，自我优化（self_optimize）文件操作以它为基准" },
]

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars }
