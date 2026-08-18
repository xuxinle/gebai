import { resolve } from "node:path"
import type { SubAgentDef, Tool, ToolContext } from "../core/types"
import {
  readTool,
  editTool,
  writeTool,
  shTool,
  lsTool,
  grepTool,
  globTool,
  fileTool,
  diffTool,
  patchTool,
  gitTool,
  fetchUrlTool,
  pyTool,
  askUserTool,
  agentRunTool,
  makeTodoTool,
  makePreviewServerTool,
  systemInfoTool,
  envDetectTool,
  walkDirFiles,
} from "../core/tools"
import { analyzeTool, searchSymbolsTool } from "../core/analyzer"
import { resolveInSandbox } from "../core/paths"

export const name = "code"
export const description =
  "涉及代码编写与源码分析时装载本子Agent（不处理歌白自身代码，自我优化用 self_optimize）：新建/修改项目与功能实现、代码分析、问题定位修复；装载后按 探索→方案→修改→验证 流程执行，改动较多时优先 patch；写操作需审批。输入：需求/问题描述；输出：代码修改方案与验证结果。"
export const systemPrompt =
  "你是源码分析与修改专家（工作流参考 opencode 编码助手）。工作流程：\n" +
  "0) 环境确认：开工前先读本提示词开头注入的项目环境注记（项目根/工作目录、预置项目清单、受限模式说明）——目标代码所在项目与 project 参数取值由此确定；分析某系统/服务源码时，先在预置项目清单（名称/说明/路径）中定位系统本体，警惕与目标同名的 API 封装/适配层（网关封装 ≠ 被管理系统源码），不要在其上浪费时间；清单确无对应项目时才用 glob/ls 在自由路径探索；\n" +
  "1) 规划：多步骤任务先用 todo 建立待办清单（entries 一次可含 add/update/delete 多条，探索→定位→方案→修改→验证），eta 参数给出每步预计耗时（分钟）让用户有耗时预期；每完成一步用 todo 更新状态，返回的清单即最新全部待办，无需再查；\n" +
  "2) 探索：grep（内容搜索）/glob（按文件名查找）/search_symbols（按符号名定位**定义**位置，跨文件；找**引用/调用点**用 grep）/ls（目录结构）/analyze（tree-sitter 结构概览）快速定位，再精确读取相关文件（大文件分段读，避免整读超长输出），不大范围逐行通读；grep 宽泛摸底先 output=files，锁定文件后再 content 模式；对独立的目标可一次发起多个并行工具调用；查阅第三方库/框架文档用 fetch_url；跨大量文件的摸底/架构梳理（只要结论不要过程）可 agent_run 委托 explore 子Agent（只读探索，返回结论与 文件:行号 清单，中间过程不占本会话上下文）；\n" +
  "3) 定位：梳理问题/需求涉及的代码位置、调用链与依赖关系；\n" +
  "4) 方案：输出改动点清单（文件、改动内容、预期效果与影响面）；方向有取舍时用 ask_user 提供选项向用户确认（如实现方案、测试框架、改动范围）；可用 diff 展示「修改前/后」对比供审查；\n" +
  "5) 修改：先 read 目标区域确认当前内容，再动手；遵循项目既有约定——先看 README/package.json/AGENTS.md 与相邻文件，了解技术栈、风格与依赖，模仿现有写法（新代码的命名/注释密度/习惯与周围代码保持一致，不引入无关改动）；改动较多或行号容易偏移时优先 patch（上下文行给 2~4 行即可——过多易不匹配、过少定位不稳；一次补丁聚焦一个改动点，不相关的改动分批提交），小范围定点改动用 edit，write 仅用于新建/整体覆盖；edit/patch 成功即已按原文校验落盘，无需重读验证；补丁不匹配时先 read 当前文件内容核对再重试；不添加无关注释；不引入/提交密钥凭据；写操作（edit/write/patch/sh/py）需审批，修改前必须先给出方案；重复性/批量操作（批量替换、批量跑测试等）优先用 sh/py 脚本一次执行，避免大量单步工具调用；明确安全的只读/幂等命令（如 git status、跑测试）可给 sh/py 传 approval:false 跳过本次审批，其余命令勿免审；\n" +
  "6) 验证：先跑与改动相关的测试文件（如 bun test 指定文件），通过后再跑全量与类型检查/lint（bun run typecheck/bun run lint 等）确认无回归；失败先看错误信息定位（grep 错误关键字找断言/堆栈位置）再修复重测，不盲目重复执行；Python 项目用 py；Web 项目需要浏览器端验证时可 agent_run 委托 playwright 子Agent；服务端功能类改动可让用户用 preview_server 在临时新端口启动独立验证服务确认（不中断当前会话，验证完 action=stop 停止）；环境/依赖异常（工具链缺失、PATH 问题）用 env_detect 探测（平台/PATH/关键工具链版本/缺失组件），系统基础信息用 system_info；\n" +
  "7) 收尾：用 git 工具只读查看变更（status/diff/log，无需审批）确认改动范围，只提交预期文件，不擅自 commit（add/commit 等写操作用 sh 且需审批；与本次任务无关的既有改动不要误动）；用 todo（空 entries 查询）核对全部待办后给出总结——**先结论后细节**（第一句话回答做了什么/结果如何），关键改动位置引用 文件:行号，改动理由与影响面随后展开；验证/测试未通过时如实说明并附关键错误输出，不粉饰、不略过失败项；\n" +
  "项目与环境变量配置（项目相关配置经进程环境变量或前端本地注入进任务 env）：\n" +
  "- CODE_PROJECTS：预置项目注册表（JSON 数组 [{name,path,description}]），文件工具用 project 参数传**项目名**（清单见本提示词开头「预置项目」注记）；\n" +
  "- CODE_PROJECT：默认项目根绑定——未指定 project 时以该项目根为基准；\n" +
  "- CODE_RESTRICT_PROJECTS：受限模式（true 开启）——仅允许操作预配置项目，自由路径（不带 project）被拒绝；\n" +
  "未设置预置项目时直接用 path 参数传项目/文件路径（自由项目）。"

const PROJECT_PARAM = {
  project: { type: "string", description: "预置项目名（CODE_PROJECTS 清单项）；传入时路径参数相对该项目根解析" },
}

/**
 * 为工具添加可选 project 参数（预置项目名）：传入时把路径解析基准与工作目录
 * 切换到该预置项目根（沙箱模式限定项目内，本地模式不限制）；未传时行为不变。
 * code/explore 等以预置项目为操作域的子Agent 复用（ctx.projects/resolveProjectPath 由引擎聚合注入）。
 */
export function projectAware(tool: Tool, opts: { workdir?: boolean } = {}): Tool {
  const parameters = { ...tool.parameters, properties: { ...tool.parameters.properties, ...PROJECT_PARAM } }
  return {
    ...tool,
    parameters,
    async execute(args, ctx) {
      const project = args.project ? String(args.project) : ""
      // 受限模式（CODE_RESTRICT_PROJECTS=true）：仅允许操作预配置项目——未传 project 时，
      // 仅当处于项目绑定根内（boundProjectRoot，子Agent 新会话执行模式 + {AGENT}_PROJECT）才放行，否则拒绝自由路径
      if (!project && ctx.env?.CODE_RESTRICT_PROJECTS === "true" && !ctx.boundProjectRoot) {
        return { output: "受限模式（CODE_RESTRICT_PROJECTS=true）：仅允许操作预配置项目（CODE_PROJECTS 清单中的项目），请用 project 参数指定项目名后重试。" }
      }
      if (!project) return tool.execute(args, ctx)
      const root = ctx.resolveProjectPath(project)
      const rest = { ...args }
      delete rest.project
      const pctx: ToolContext = {
        ...ctx,
        workdir: opts.workdir ? root : ctx.workdir,
        resolvePath: (p) => (ctx.sandboxed ? resolveInSandbox(root, p) : resolve(root, p)),
        listFiles: () => walkDirFiles(root),
      }
      return tool.execute(rest, pctx)
    },
  }
}

const todoTool = makeTodoTool()

export const tools = {
  read: projectAware(readTool),
  write: projectAware(writeTool),
  edit: projectAware(editTool),
  patch: projectAware(patchTool),
  sh: projectAware(shTool, { workdir: true }),
  py: projectAware(pyTool, { workdir: true }),
  ls: projectAware(lsTool),
  grep: projectAware(grepTool),
  glob: projectAware(globTool),
  search_symbols: projectAware(searchSymbolsTool),
  file: projectAware(fileTool),
  diff: projectAware(diffTool),
  analyze: projectAware(analyzeTool),
  git: projectAware(gitTool, { workdir: true }),
  fetch_url: fetchUrlTool,
  ask_user: askUserTool,
  agent_run: agentRunTool,
  todo: todoTool,
  preview_server: makePreviewServerTool(),
  env_detect: envDetectTool,
  system_info: systemInfoTool,
}
// sh/py 不在此静态覆盖：保留工具自身的动态审批判定（默认需审批，approval 参数支持按次免审——静态 true 会盖掉动态函数致参数失效）
export const requiresApproval = { edit: true, write: true, patch: true }
export const preload = false
export const envVars = [
  { name: "CODE_PROJECTS", description: "预置项目清单（JSON 数组 [{name,path,description}]）；文件工具用 project 参数传项目名" },
  { name: "CODE_PROJECT", description: "绑定项目根路径：会话默认工作目录即该项目根，文件操作以项目根为基准" },
  { name: "CODE_RESTRICT_PROJECTS", description: "受限模式 true/false：true 时仅允许操作预配置项目，自由路径被拒绝" },
]

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload, envVars }
