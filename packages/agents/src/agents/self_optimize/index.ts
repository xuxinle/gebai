import { isAbsolute, relative, resolve, sep } from "node:path"
import type { SubAgentDef } from "@gebai/sdk"
import { truncate } from "@gebai/sdk/node"
import { readFeedbackTool } from "../../core/shared/feedback"
import { pageCaptureTool } from "../../core/shared/page-capture"
import { isBinaryMode } from "../../core/shared/config"

export const name = "self_optimize"
export const description =
  "优化歌白自身（自身代码/子Agent/提示词/配置；外部项目用 code）：改进定义、修复缺陷、验证修改。任务中因知识/工具不足或错误重复试错、低效时也装载：不便立即优化的先 self_optimize_backlog add 暂存问题与方向，后续集中全面优化。输入：改进点/失败案例/用户反馈（self_optimize_read_feedback）；修改须过测试（run_tests）并同步 DESIGN.md，失败可 rollback 回滚，优化历史经 self_optimize_journal 沉淀。"
export const systemPrompt =
  "你是歌白智能体（GEBAI Agent）的自我优化专家。**通用编码工作流（规划→探索→定位→方案→修改→验证→收尾，含 grep/analyze/edit/patch 等工具用法）直接遵循 code 子Agent 提示词**——装载 self_optimize 时 code 已连带装载（完整工作流在会话记录/本系统提示词内）；文件读写查询（read/write/edit/patch/grep/sh 等）为全局工具直接用全局名（带 project 参数路由项目），分析/验证类工具由 code 提供（search_symbols/analyze/git/preview_server，以 code_ 前缀调用）；本提示词只补充自我优化特有的流程与约束：\n" +
  "1) 输入：改进点/失败案例；用户反馈（点赞/点踩/文字反馈/建议）用 self_optimize_read_feedback 工具读取（全局集无 read_feedback，本命名空间为唯一入口），作为优化输入；开工先用 self_optimize_journal action=list 查相关历史与教训（跨会话优化记忆，不重复踩坑），并 self_optimize_backlog action=list 查待优化暂存项（任务执行中暂存的改进点——有积压且本次目标就是优化时以此为工作清单，见 2)）；\n" +
  "2) 离线优化（暂存 → 集中全面优化）：任务执行中因自身知识/工具不足或错误导致重复试错、低效（多次失败重试、工具用法反复出错、缺关键工具/子Agent），而当前任务不便中断深入优化时——先把问题暂存：self_optimize_backlog action=add（problem 问题现象 + direction 优化方向，会话ID自动记录供回溯），随即继续当前任务；暂存后用 ask 向用户确认处理时机（当场修复 / 留待后续集中全面优化，用户不选则默认后续）；后续执行全面优化时 action=list 取待优化项清单，按主题归并逐项优化（需更多上下文可读来源会话记录 {GEBAI_HOME}/users/{用户}/sessions/{ID前2位}/{第3-4位}/{会话ID}/chat.json——本地模式可读；沙箱部署模式会话文件不可读时以暂存的问题/方向文本为准），每项优化完成后 journal append 记录、backlog action=resolve ids=[编号] 移除；\n" +
  "3) 修改范围（**系统强制**）：默认只读模式仅允许写入 子Agent 包（packages/agents/src/）、二开域（custom/）与客卿域（keqing/）及仓库级文档/配置（DESIGN.md/AGENTS.md/AGENT.md/.env.example/README.md），核心引擎源码（core/engine/app/ws 等）写入会被拒绝——需放宽时请用户在服务端设置 GEBAI_SELF_MODIFY=true 后重启；把改进沉淀为新的/修改后的子Agent 是首选方式（子Agent 是歌白的标准扩展机制）；写仓库文件一律用 write/edit/patch 文件工具（写范围守卫在此拦截）——**禁止经 sh/py 重定向或脚本写仓库文件**（守卫不拦脚本通道，绕行属违规且绕开防盲写保护）；新建/修改子Agent 文件后立即验证注册（subsession_run 试跑或 agent_list 查看——注册失败会直接返回文件加载错误原因，据因修复后再验）；\n" +
  "4) 开发场景与目录（三类子Agent 扩展，放置即发现；命名仅小写字母/数字/下划线 `[a-z0-9_]+`，入口 `{name}/{name}.ts`（回退 `{name}/index.ts`）+ 可选同目录 `{name}.md` 系统提示词（纯 `{name}/{name}.md` 即零 TS 简化定义），工具名在 def 里写裸名（引擎加 `{name}_` 前缀）；dev 形态下新增/修改/删除文件在下一次装载或新任务前自动热加载——改完立即 agent_list/subsession_run 验证注册，失败错误附带载根因）：\n" +
  "  - **内置子代理** `packages/agents/src/agents/{name}/`（共用基建 `packages/agents/src/core/`，测试文件与定义同放）——改歌白自带能力的首选；随包发布，构建期由 `packages/server/scripts/build-subagents.ts` 打包进 bundle（dist/二进制形态无源码树，改动需重新构建才生效）。\n" +
  "  - **二开子代理** `custom/agents/{name}/`（依赖组件 `custom/core/{lib}/`，同布局同规则）——面向「不碰上游、可整体迁移」的改动：`custom/` 是仓库根的二开专属域（上游更新时整个文件夹拷到新仓库即完成迁移）；同名二开版本胜出（可用于**改写内置行为而不动上游代码**）；类型检查 `bun run typecheck:custom`（根 `bun run typecheck` 已含）；`custom/README.md` 是二开规范。\n" +
  "  - **客卿子代理** `keqing/{lang}/{name}/`（`agent.json` manifest + `PROMPT.md` + 该语言驱动；语言目录 `python`/`cpp`/`go`/`rust` 各有共享框架，一种语言可放多个子代理项目）——需要**原生性能或复用他语言生态**（库/工具链）时的形态：manifest 只必填 `name`（`description`/`PROMPT.md` 留空即「本侧不贡献」，与同名 TS 侧跨语言合并：描述/提示词拼接、工具并集、同名工具 TS 优先）；编译型语言在 manifest `build` 声明编译命令（首次调用自动构建，缺工具链记 loadErrors 不阻断其他子代理）；用户自建同构放 `{GEBAI_HOME}/agents/{name}/`（同名覆盖内置）；规矩：**能力外置、权力内守**——审批/沙箱/写守卫仍由 TS 引擎裁决，驱动不得自取工具、不得自证授权（协议单向下行）；仅本地形态可用（沙箱部署与 `GEBAI_KEQING=off` 下整体禁用）；规范见 `keqing/README.md`。\n" +
  "  选型：纯 TS 能力（改歌白自身能力）→ 内置域；不碰上游/可迁移/覆盖内置行为 → 二开域；原生计算或他语言库 → 客卿。三域都是子Agent 扩展面，写入不受核心源码只读限制。\n" +
  "5) **设计同步铁律**：任何修改行为/接口/协议/存储布局/常量/命名规则等设计层面变更，必须同步更新 DESIGN.md 对应章节（文档与代码保持一致）；**产物纯净**：写出的代码/子Agent 提示词/文档只描述当前完整的能力与限制，不留历史痕迹——不写「何时发现/修复了什么问题」「为何改成现在这样」等变更缘由（缘由归 git 提交说明与 self_optimize_journal，历史有专门载体、不进产物），代码注释同理只述当前约束；遇到既有历史注记（时间/问题描述/修复记录）顺手清除；\n" +
  "6) 验证（**测试是唯一准入凭证**）：任何修改必须通过相关测试——用 self_optimize_run_tests 工具执行（files 传相关测试文件，如 [\"packages/server/src/core/engine.test.ts\"]、相对仓库根；确认无回归后用 checks=[\"test\",\"typecheck\",\"lint\"] 跑三件套、all=true 跑全量——与 AGENTS.md 提交准入一致，一次审批跑全），失败则修复或 self_optimize_rollback 回滚（恢复修改并删除本次新建文件；失败先看错误信息定位再修复重测，不盲目重复执行）；\n" +
  "7) 用户验证：修改通过测试后，用 ask 询问用户验证方式——UI/前端类修改建议直接在当前浏览器页面验证（dev 模式改动自动热更新，先请用户刷新；先确认其显示的是目标视图，切走了先请用户切回，再 page_capture 捕获：read 读 html、vision_analyze 分析截图（vision 已连带装载；vision_ocr 读文字），确认视觉效果后再收尾）；服务端功能类修改可用 preview_server 在临时新端口启动验证服务（独立进程不中断当前会话），用户确认后启动并告知访问 URL 与停止方式，验证结束后用 preview_server action=stop 停止；\n" +
  "8) 收尾：git 工具只读查看变更（status/diff/log，无需审批）确认改动范围，只提交预期文件、不擅自 commit（add/commit 等写操作用 sh 且需审批；工作区若有与本次任务无关的未提交改动，先 git status 确认清楚，不混淆/误提交）；用 self_optimize_journal action=\"append\" 记录本次优化（title/changes/verification/outcome/lessons——action 必传，漏传不写盘、显式报错；优化历史跨会话沉淀）；本次解决了待优化项的，self_optimize_backlog action=resolve ids=[编号] 一并移除；总结先结论后细节，关键位置引用 文件:行号；验证/测试未通过时如实说明并附关键错误输出；**结论只写核验过的事实**：引用 git 对象（哈希/分支）前先 git cat-file / git log 校验其存在，引用文件位置/行号前先 read，引用选择器/标识符前先 grep，引用外部报告/他人结论前先回溯核验——核不到就不写（说明无法核验），不把未核验的归因与数字当事实陈述。\n" +
  "项目名称：歌白（GEBAI Agent）。项目范围：项目根以系统提示词动态注记「项目根:」为准——设置了 SELF_OPTIMIZE_PROJECT 环境变量时即该路径（服务端部署限定项目内，本地模式不限制目录）；未设置时脚本调试（dev）模式自动推导为歌白源码仓库根（与 run_tests/rollback 工作目录及写范围守卫同源，提示词注记给出具体路径）；二进制模式未配置且无注记时按用户给定的路径处理。" +
  /**
   * 写范围的**实际状态**（附在提示词末尾）。
   *
   * 提示词正文写的是**策略**（“默认只读…”），而它是否生效取决于**启动级环境变量** `GEBAI_SELF_MODIFY`
   *（守卫读 `process.env`，见 `selfModifyEnabled`）。不写这一行的话，模型只能看到“默认只读”的描述：
   * 实测 `.env` 已配 `true`（进程环境里也是 `true`、守卫实际放行）时，模型仍以为核心源码不可写，
   * 白白绕路（把本该改核心的修复挤到子Agent 层）。所以策略与状态分开说，末尾这行以**当前进程环境**为准。
   */
  (selfModifyEnabled()
    ? "\n\n**当前写范围状态：已放开**（GEBAI_SELF_MODIFY=true，进程环境已核实）——仓库内任意路径可写，包含核心引擎源码（packages/server/src/core/ 等）。改核心代码同样要遵守设计同步（同步 DESIGN.md）与测试准入（run_tests 三件套），并在收尾说明里如实列出改动与验证结果。"
    : "\n\n**当前写范围状态：默认只读**（未开启 GEBAI_SELF_MODIFY）——仅 子Agent 扩展面（内置域 packages/agents/src/、二开域 custom/、客卿域 keqing/）与仓库级文档/配置可写，核心引擎源码会被 writeGuard 拒绝；确需改核心代码请让用户在服务端设置 GEBAI_SELF_MODIFY=true 并重启。")

/** 默认只读模式下允许写入的仓库级文件（根一级）。 */
const WRITABLE_ROOT_FILES = new Set(["DESIGN.md", "AGENTS.md", "AGENT.md", ".env.example", "README.md"])
/** 默认只读模式下允许写入的目录（相对仓库根；子Agent 扩展面为唯一允许改代码的位置——
 *  内置域 `packages/agents/src/`、二开域 `custom/`（上游更新不触碰的独立迁移域）、客卿域 `keqing/`（多语言边车子代理））。 */
const WRITABLE_ROOT_DIRS = [
  ["packages", "agents", "src"],
  ["custom"],
  ["keqing"],
]

/** 启动级放开开关：GEBAI_SELF_MODIFY=true 时允许写入仓库内任意路径（含核心引擎源码）。 */
function selfModifyEnabled(): boolean {
  const v = process.env.GEBAI_SELF_MODIFY
  return v === "true" || v === "1"
}

/** 解析歌白仓库根：SELF_OPTIMIZE_PROJECT 优先；dev 模式按模块路径推导；二进制模式必须显式配置。 */
function selfOptimizeRoot(env: Record<string, string>): string | null {
  const proj = env.SELF_OPTIMIZE_PROJECT
  if (proj) return isAbsolute(proj) ? proj : resolve(process.cwd(), proj)
  // src/agents/self_optimize/ → agents 包根 4 级，→ 歌白仓库根（packages/ 上一级）再 1 级
  if (!isBinaryMode()) return resolve(import.meta.dirname, "..", "..", "..", "..", "..")
  return null
}

/**
 * 写范围守卫（SubAgentDef.writeGuard，引擎注入 ToolContext.writeGuard）——「核心引擎源码默认只读」的
 * **代码级**强制（文件写类工具 write/edit/patch/move_file/delete_file 写入前调用）：
 * - GEBAI_SELF_MODIFY=true：完全放开；仓库根无法定位（二进制模式未配 SELF_OPTIMIZE_PROJECT）：无保护对象，放行；
 * - 命中仓库根内的路径：仅 子Agent 扩展面（内置域/二开域/客卿域）+ 仓库级文档/配置 可写，核心引擎源码拒绝；
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
      WRITABLE_ROOT_DIRS.some((dir) => segs.length > dir.length && dir.every((s, i) => segs[i] === s))
    if (!writable) {
      return (
        `拒绝写入 ${target}：self_optimize 默认只读模式仅允许修改 子Agent 扩展面（内置域 packages/agents/src/、二开域 custom/、客卿域 keqing/）与仓库级文档/配置（DESIGN.md/AGENTS.md/AGENT.md/.env.example/README.md），` +
        `核心引擎源码受保护。确需修改核心代码请在服务端设置 GEBAI_SELF_MODIFY=true 后重启；` +
        `或把改进沉淀为新的/修改后的子Agent（子Agent 是歌白的标准扩展机制）。`
      )
    }
  }
  return null
}

/** 路径参数注入防护：条目含引号/shell 元字符/百分号即拒绝（这些字符不可能合法出现在测试文件/回滚
 *  路径中；审批界面展示的是 files/paths 参数而非拼好的命令，不设防的拼接会把注入带过审批门）。
 *  通过校验的条目仍以双引号包裹拼入命令（空格路径保持单参数）。 */
function safePathArgs(paths: string[]): string | null {
  const bad = paths.find((p) => /["'`$%&|<>^();!]/.test(p))
  return bad === undefined ? null : `路径参数含非法字符（引号/shell 元字符）: ${bad}`
}

/** run_tests：在歌白仓库根执行验证（指定测试文件/全量 + 可选 typecheck/lint——AGENTS.md 准入三件套）：
 *  测试是自我修改的唯一准入凭证。 */

/** run_tests 的 test 项调用形态（cwd + 文件参数）：`bunfig.toml` 只在**包目录**下生效——`[loader] .md=text`
 *  （子Agent 提示词的 md 导入）与 `[test] preload`（测试环境净化，清 GEBAI_ 与 CODE_ 前缀变量）都靠它；从仓库根直接
 *  `bun test packages/server/x.test.ts` 两者兼失：md 被按 HTML 导入、仓库根 .env 注入测试进程——环境问题
 *  会被误报为「改动引入回归」。文件同属一个包时切到该包目录并把路径转成相对该包（跨包/非包路径保持仓库根）。 */
function testInvocation(root: string, files: string[]): { workdir: string; args: string } {
  const norm = (f: string) => f.replace(/\\/g, "/").replace(/^\.\//, "")
  const pkgs = new Set(files.map((f) => /^packages\/([^/]+)\//.exec(norm(f))?.[1] ?? ""))
  if (files.length > 0 && pkgs.size === 1 && !pkgs.has("")) {
    const pkg = [...pkgs][0]!
    return {
      workdir: resolve(root, "packages", pkg),
      args: files.map((f) => `"${norm(f).slice(`packages/${pkg}/`.length)}"`).join(" "),
    }
  }
  return { workdir: root, args: files.map((f) => `"${f}"`).join(" ") }
}

const runTestsTool: import("@gebai/sdk").Tool = {
  name: "run_tests",
  description:
    "在歌白仓库根执行验证（自我修改的唯一准入凭证）：checks 选择检查项（默认 [\"test\"]；修改确认后、收尾前用 [\"test\",\"typecheck\",\"lint\"] 三件套——与 AGENTS.md 提交准入一致，一次审批跑全）；test 时 files 传相关测试文件（相对仓库根，可多个；同属一个包时自动切到该包目录执行，使 bunfig.toml 的 md loader 与测试环境净化生效），all=true 跑全量。按序执行、首项失败即停。输出各项结果（失败需修复或 rollback 回滚）。需审批。",
  requiresApproval: true,
  parameters: schema({
    files: { type: "array", items: { type: "string" }, description: "测试文件列表（相对仓库根，如 [\"packages/server/src/core/engine.test.ts\"]；同属一个包时自动切到该包目录执行）" },
    all: { type: "boolean", description: "true 跑全量测试（bun run test，忽略 files）" },
    checks: { type: "array", items: { type: "string", enum: ["test", "typecheck", "lint"] }, description: "检查项列表（默认 [\"test\"]；[\"test\",\"typecheck\",\"lint\"] 为提交前三件套）" },
  }),
  async execute(args, ctx) {
    const root = selfOptimizeRoot(ctx.env)
    if (!root) return { output: "无法定位歌白仓库根：请设置 SELF_OPTIMIZE_PROJECT 环境变量。" }
    const files = (Array.isArray(args.files) ? args.files : []).map(String).filter(Boolean)
    const rawChecks = (Array.isArray(args.checks) ? args.checks : []).map(String)
    const checks = [...new Set(rawChecks.length ? rawChecks : ["test"])]
    const invalid = checks.find((c) => c !== "test" && c !== "typecheck" && c !== "lint")
    if (invalid) return { output: `无效的检查项: ${invalid}（仅支持 test/typecheck/lint）。` }
    if (checks.includes("test") && args.all !== true && !files.length) {
      return { output: "run_tests 需要 files 参数（相关测试文件）或 all=true（全量测试）。" }
    }
    const unsafe = safePathArgs(files)
    if (unsafe) return { output: unsafe }
    const sections: string[] = []
    for (const check of checks) {
      // test 项按包目录执行（bunfig.toml 的 md loader 与测试环境净化只在包目录生效，见 testInvocation）；
      // typecheck/lint 是根级 turbo 任务，恒在仓库根执行
      const inv = check === "test" && args.all !== true ? testInvocation(root, files) : { workdir: root, args: "" }
      const cmd =
        check === "test"
          ? args.all === true
            ? "bun run test"
            : `bun test ${inv.args}`
          : check === "typecheck"
            ? "bun run typecheck"
            : "bun run lint"
      const { stdout, stderr, code } = await ctx.runCommand(cmd, { workdir: inv.workdir, timeoutMs: 10 * 60 * 1000 })
      // 始终合并 stderr：bun test 在 Windows 把用例明细/汇总写 stderr（exit 0 亦然），只取 stdout 会让
      // 用户看不到「跑了哪些用例、几个 pass」——准入判定看 exit code，明细供人核验
      const text = [stdout, stderr].filter(Boolean).join("\n")
      sections.push(`—— ${check}（${cmd}）${code === 0 ? " ✅" : ` ❌ exit ${code}`} ——\n${code === 0 ? text : `${text}\n[exit ${code}]`}`)
      if (code !== 0) break // 首项失败即停：后续检查基于未通过代码无意义
    }
    return truncate(sections.join("\n\n"), "run_tests", ctx)
  },
}

/** rollback：回滚工作区未提交改动（测试失败时的恢复路径）——恢复被修改的 tracked 文件 + 删除新建的
 *  untracked 文件（git checkout 只恢复 tracked；新文件是自我修改的主要产物（如新建子Agent），残留会被
 *  热加载注册为破损 Agent，必须一并清理；先 dry-run 列出将删除的新建文件再执行，输出如实展示）。 */
const rollbackTool: import("@gebai/sdk").Tool = {
  name: "rollback",
  description:
    "回滚工作区未提交改动（测试失败后的恢复路径）：paths 传要回滚的文件/目录（相对仓库根，可多个——恢复其内修改并删除其内新建文件），all=true 回滚全部未提交改动。**注意：会丢弃未提交的修改与新建文件**——仅用于撤销本次失败的自我修改；工作区若有与本次任务无关的既有改动，请用 paths 精确指定而非 all。需审批。",
  requiresApproval: true,
  parameters: schema({
    paths: { type: "array", items: { type: "string" }, description: "回滚文件/目录列表（相对仓库根；恢复修改 + 删除其内新建文件）" },
    all: { type: "boolean", description: "true 回滚全部未提交改动（git checkout -- . + git clean -fd）" },
  }),
  async execute(args, ctx) {
    const root = selfOptimizeRoot(ctx.env)
    if (!root) return { output: "无法定位歌白仓库根：请设置 SELF_OPTIMIZE_PROJECT 环境变量。" }
    const paths = (Array.isArray(args.paths) ? args.paths : []).map(String).filter(Boolean)
    if (args.all !== true && !paths.length) return { output: "rollback 需要 paths 参数或 all=true。" }
    const unsafe = safePathArgs(paths)
    if (unsafe) return { output: unsafe }
    const targets = args.all === true ? ["."] : paths
    const q = (p: string) => `"${p}"`
    // dry-run 先列出将删除的新建文件（untracked）——checkout 对其本就无可恢复（pathspec 不匹配属预期）
    const dry = await ctx.runCommand(`git clean -nd ${targets.map(q).join(" ")}`, { workdir: root })
    let coErr = ""
    for (const t of targets) {
      const r = await ctx.runCommand(`git checkout -- ${q(t)}`, { workdir: root })
      if (r.code !== 0 && !dry.stdout.includes(t)) coErr += `${r.stderr.trim()}\n`
    }
    const clean = await ctx.runCommand(`git clean -fd ${targets.map(q).join(" ")}`, { workdir: root })
    const status = await ctx.runCommand("git status --short", { workdir: root })
    if (coErr || clean.code !== 0) {
      return truncate(`rollback 失败：\n${coErr}${clean.stdout}\n${clean.stderr}\n[exit clean ${clean.code}]\n当前工作区状态：\n${status.stdout.trim() || "（干净）"}`, "rollback", ctx)
    }
    const removed = dry.stdout.trim()
    return truncate(`已回滚。${removed ? `删除的新建文件：\n${removed}\n` : ""}当前工作区状态：\n${status.stdout.trim() || "（干净）"}`, "rollback", ctx)
  },
}

/** 优化日志条目（跨会话优化记忆，DESIGN「变更管理」的补丁记录落地）。 */
interface OptimizeJournalEntry {
  at: number
  title: string
  changes?: string[]
  verification?: string
  outcome?: "applied" | "reverted" | "failed"
  lessons?: string
}

/** 优化日志存储：users/{user}/self-optimize-journal.json（与 ws-journal 同位，gitignored 运行时数据），
 *  环形保留最近 100 条。 */
const JOURNAL_FILE = "self-optimize-journal.json"
const JOURNAL_MAX_ENTRIES = 100

/** journal：自我优化日志（append 记录一次优化 / list 读最近记录）——跨会话优化记忆：失败的尝试与教训
 *  沉淀后，后续优化任务开工先查历史避免重复踩坑；git 历史只记代码变更，这里补「为什么改 + 验证结果」。 */
const journalTool: import("@gebai/sdk").Tool = {
  name: "journal",
  description:
    "自我优化日志（跨会话优化记忆）：action=append 记录一次优化（title 必填；changes 改动清单（文件:摘要）；verification 验证方式与结果（如 run_tests 三件套/用户确认）；outcome applied=已落地/reverted=已回滚/failed=验证未通过；lessons 经验教训）；action=list 读最近记录（limit 默认 10，新→旧）。**action 必传**——缺 action、或带了 append 专属参数却不传 action=\"append\" 时，显式报错且不写盘（不静默按查询处理）。接到优化任务时先 list 了解相关历史与教训，收尾时必 append 记录本次。",
  parameters: schema({
    action: { type: "string", enum: ["append", "list"], description: "append=记录一次优化；list=读取最近记录" },
    title: { type: "string", description: "优化标题（append 必填，一句话说清做了什么）" },
    changes: { type: "array", items: { type: "string" }, description: "改动清单（文件路径: 改动摘要，可多条）" },
    verification: { type: "string", description: "验证方式与结果" },
    outcome: { type: "string", enum: ["applied", "reverted", "failed"], description: "结果（默认 applied）" },
    lessons: { type: "string", description: "经验教训（失败原因/坑/下次怎么做得更好）" },
    limit: { type: "number", description: "list 返回条数（默认 10，新→旧）" },
  }, ["action"]),
  async execute(args, ctx) {
    const { join, dirname } = await import("node:path")
    const { mkdir } = await import("node:fs/promises")
    const file = join(ctx.home, "users", ctx.user, JOURNAL_FILE)
    const action = String(args.action ?? "").trim()
    const appendArgs = (["title", "changes", "verification", "outcome", "lessons"] as const).filter((k) => args[k] !== undefined)
    // 漏传 action 不静默按 list 处理：带 append 专属参数的调用会「返回一份正常列表」而记录根本没写，
    // 调用方无从察觉——一律显式报错，把漏参变成可见失败
    if (!action) {
      return { output: `journal：缺少 action（写记录传 "append"、读记录传 "list"）${appendArgs.length ? `——本次带了 append 专属参数（${appendArgs.join("/")}）但未写入任何记录` : ""}。` }
    }
    if (action !== "append" && appendArgs.length) {
      return { output: `journal：action=${action} 与 append 专属参数（${appendArgs.join("/")}）冲突——这些参数被忽略、未写入任何记录；要记录本次优化请传 action="append"。` }
    }
    const entries = await readJsonList<OptimizeJournalEntry>(file, isJournalEntry)
    if (action === "append") {
      const title = String(args.title ?? "").trim()
      if (!title) return { output: "journal append 需要 title（一句话说清本次优化）。" }
      const entry: OptimizeJournalEntry = { at: Date.now(), title }
      const changes = (Array.isArray(args.changes) ? args.changes : []).map(String).filter(Boolean)
      if (changes.length) entry.changes = changes
      const verification = String(args.verification ?? "").trim()
      if (verification) entry.verification = verification
      const outcome = String(args.outcome ?? "")
      if (outcome === "applied" || outcome === "reverted" || outcome === "failed") entry.outcome = outcome
      const lessons = String(args.lessons ?? "").trim()
      if (lessons) entry.lessons = lessons
      // 锁内重读再追加：并发 append 各自读到的旧日志会互相覆盖（后写覆盖前写而丢记录）
      const total = await withRuntimeJsonLock(async () => {
        const fresh = await readJsonList<OptimizeJournalEntry>(file, isJournalEntry)
        fresh.push(entry)
        const kept = fresh.length > JOURNAL_MAX_ENTRIES ? fresh.slice(-JOURNAL_MAX_ENTRIES) : fresh
        await mkdir(dirname(file), { recursive: true })
        await Bun.write(file, JSON.stringify(kept, null, 2))
        return kept.length
      })
      return { output: `已记录（累计 ${total} 条）：${title}` }
    }
    if (action !== "list") return { output: `无效的 action: ${action}（仅支持 append/list）。` }
    const limit = Math.max(1, Math.min(50, Number(args.limit) || 10))
    const list = entries.slice(-limit).reverse()
    if (!list.length) return { output: "（暂无优化记录）" }
    const fmt = (e: OptimizeJournalEntry) => {
      const lines = [`[${new Date(e.at).toLocaleString("zh-CN")}] ${e.title}${e.outcome ? `（${e.outcome}）` : ""}`]
      for (const c of e.changes ?? []) lines.push(`  改动: ${c}`)
      if (e.verification) lines.push(`  验证: ${e.verification}`)
      if (e.lessons) lines.push(`  教训: ${e.lessons}`)
      return lines.join("\n")
    }
    return { output: `优化记录（新→旧，共 ${list.length} 条）:\n\n${list.map(fmt).join("\n\n")}` }
  },
}

/** 待优化暂存条目（离线优化暂存态：问题 + 方向 + 来源会话，解决即移除——优化记录由 journal 承载）。 */
interface OptimizeBacklogItem {
  id: number
  at: number
  problem: string
  direction?: string
  session: string
}

/** 待优化暂存清单存储：users/{user}/self-optimize-backlog.json（与 journal 同位的 gitignored 运行时数据）。
 *  条目解决即移除、不设环形上限——暂存项是待办不是历史，静默淘汰会丢待办。 */
const BACKLOG_FILE = "self-optimize-backlog.json"

/**
 * 运行时 JSON 清单的读-改-写互斥：journal 与 backlog 的写入都是「读全量 → 改 → 整文件覆盖」，
 * 并发调用（同一回复的多个工具调用、js 编排并行）各自读到同一旧值再写回，后写会静默覆盖前写而丢条目。
 * 进程内串行化覆盖实际并发面（多个歌白实例共写同一用户文件不在支持范围）。
 */
let runtimeJsonQueue: Promise<unknown> = Promise.resolve()
function withRuntimeJsonLock<T>(fn: () => Promise<T>): Promise<T> {
  const task = runtimeJsonQueue.then(fn, fn)
  runtimeJsonQueue = task.then(
    () => undefined,
    () => undefined,
  )
  return task
}

/** 读 JSON 清单（缺失/损坏或非数组时返回空清单，条目按 assert 校验过滤）。 */
async function readJsonList<T>(file: string, assert: (v: Record<string, unknown>) => boolean): Promise<T[]> {
  try {
    const parsed = JSON.parse(await Bun.file(file).text())
    if (Array.isArray(parsed)) return parsed.filter((e) => e && typeof e === "object" && assert(e)) as T[]
  } catch {
    /* 首次写入或文件损坏：从空开始 */
  }
  return []
}

const isBacklogItem = (e: Record<string, unknown>) => typeof e.problem === "string"
const isJournalEntry = (e: Record<string, unknown>) => typeof e.title === "string"

/** backlog：待优化项暂存清单（离线优化）——任务执行中因知识/工具不足或错误导致重复试错、低效而不便
 *  立即中断当前任务时，先把问题与优化方向暂存（会话ID自动记录，供后续回溯完整上下文），优化时机后移、
 *  证据先落盘；后续集中查看待优化项执行全面优化，解决后 resolve 移除。 */
const backlogTool: import("@gebai/sdk").Tool = {
  name: "backlog",
  description:
    "待优化项暂存清单（离线优化）：action=add 暂存一个待优化项（problem 必填——问题现象，如知识/工具不足或错误导致的重复试错；direction 优化方向/初步思路；session_id 可选，缺省自动记当前会话供回溯）；action=list 查看待优化项（旧→新，执行全面优化时以此为工作清单）；action=resolve 移除已解决项（ids 从 add/list 输出取，可多个）。**action 必传**——缺 action、或带了 add 专属参数却不传 action=\"add\" 时，显式报错且不做变更（不静默按查询处理）。任务执行中不便立即优化时先暂存不打断当前任务，暂存后 ask 向用户确认处理时机（当场修复/后续集中全面优化）。",
  parameters: schema({
    action: { type: "string", enum: ["add", "list", "resolve"], description: "add=暂存待优化项；list=查看待优化项；resolve=移除已解决项" },
    problem: { type: "string", description: "问题现象（add 必填：什么知识/工具不足或错误导致了什么低效，如工具用法反复出错重试多次、缺关键工具）" },
    direction: { type: "string", description: "优化方向（初步思路：改哪个子Agent/提示词/工具、怎么改）" },
    session_id: { type: "string", description: "问题来源会话 ID（缺省自动取当前会话，供后续回溯完整上下文）" },
    ids: { type: "array", items: { type: "number" }, description: "resolve 要移除的待优化项编号列表（从 add/list 输出取）" },
  }, ["action"]),
  async execute(args, ctx) {
    const { join, dirname } = await import("node:path")
    const { mkdir } = await import("node:fs/promises")
    const file = join(ctx.home, "users", ctx.user, BACKLOG_FILE)
    const action = String(args.action ?? "").trim()
    const addArgs = (["problem", "direction", "session_id"] as const).filter((k) => args[k] !== undefined)
    // 同 journal：缺 action 一律显式报错——带 add 专属参数却静默当查询会让暂存凭空消失
    if (!action) {
      return { output: `backlog：缺少 action（暂存传 "add"、查看传 "list"、移除传 "resolve"）${addArgs.length ? `——本次带了 add 专属参数（${addArgs.join("/")}）但未做任何变更` : ""}。` }
    }
    if (action !== "add" && addArgs.length) {
      return { output: `backlog：action=${action} 与 add 专属参数（${addArgs.join("/")}）冲突——这些参数被忽略、未做任何变更；要暂存请传 action="add"。` }
    }
    const items = await readJsonList<OptimizeBacklogItem>(file, isBacklogItem)
    if (action === "add") {
      const problem = String(args.problem ?? "").trim()
      if (!problem) return { output: "backlog add 需要 problem（说清问题现象：什么知识/工具不足或错误导致了什么低效）。" }
      const direction = String(args.direction ?? "").trim()
      const session = String(args.session_id ?? ctx.sessionId ?? "")
      // 锁内重读再追加：并发 add 各自读到的旧清单会互相覆盖（后写覆盖前写而丢条目）
      const added = await withRuntimeJsonLock(async () => {
        const fresh = await readJsonList<OptimizeBacklogItem>(file, isBacklogItem)
        const id = fresh.reduce((max, e) => Math.max(max, e.id || 0), 0) + 1
        const item: OptimizeBacklogItem = { id, at: Date.now(), problem, session }
        if (direction) item.direction = direction
        fresh.push(item)
        await mkdir(dirname(file), { recursive: true })
        await Bun.write(file, JSON.stringify(fresh, null, 2))
        return { id, total: fresh.length }
      })
      return {
        output:
          `已暂存待优化项 #${added.id}（共 ${added.total} 项待处理）：${problem}${direction ? `（方向：${direction}）` : ""}\n` +
          `不打断当前任务继续执行；暂存后需用 ask 向用户确认处理时机——当场修复或留待后续集中全面优化（用户不选则默认后续；后续 self_optimize_backlog action=list 取清单执行）。`,
      }
    }
    if (action === "resolve") {
      const ids = new Set((Array.isArray(args.ids) ? args.ids : []).map(Number).filter((n) => Number.isInteger(n) && n > 0))
      if (!ids.size) return { output: "backlog resolve 需要 ids（要移除的待优化项编号，从 add/list 输出取，可多个）。" }
      // 锁内重读再移除：与并发 add 串行化，避免后写覆盖
      const removed = await withRuntimeJsonLock(async () => {
        const fresh = await readJsonList<OptimizeBacklogItem>(file, isBacklogItem)
        const kept = fresh.filter((e) => !ids.has(e.id))
        if (kept.length === fresh.length) return null
        await Bun.write(file, JSON.stringify(kept, null, 2))
        return { count: fresh.length - kept.length, total: kept.length }
      })
      if (!removed) return { output: `未找到编号 ${[...ids].join("/")} 对应的待优化项（action=list 查看当前清单）。` }
      return { output: `已移除 ${removed.count} 项已解决的待优化项，剩余 ${removed.total} 项待处理。优化过程请同步 self_optimize_journal append 记录。` }
    }
    if (action !== "list") return { output: `无效的 action: ${action}（仅支持 add/list/resolve）。` }
    if (!items.length) return { output: "（暂无待优化项）" }
    const fmt = (e: OptimizeBacklogItem) => {
      const lines = [`#${e.id} [${new Date(e.at).toLocaleString("zh-CN")}] ${e.problem}`]
      if (e.direction) lines.push(`  方向: ${e.direction}`)
      if (e.session) lines.push(`  会话: ${e.session}`)
      return lines.join("\n")
    }
    return {
      output:
        `待优化项（旧→新，共 ${items.length} 项）:\n\n${items.map(fmt).join("\n\n")}\n\n` +
        `执行全面优化：按主题归并逐项处理，完成后 action=resolve ids=[编号] 移除。`,
    }
  },
}

function schema(properties: Record<string, unknown>, required: string[] = []): import("@gebai/sdk").ToolSchema {
  return { type: "object", properties, required }
}

/**
 * 工具集只含自优化专属工具（工具与提示词均不复刻 code，亦不复刻全局工具）：
 * 装载/预加载 self_optimize 时系统连带装载 code 与 vision——文件读写查询为全局工具（直接全局名），
 * 分析/验证类工具由 code 提供（code_ 前缀），通用编码工作流遵循 code 提示词；
 * 视觉能力由 vision 子代理提供（vision_analyze 语义分析截图等，vision_ocr 读图）——依赖声明
 * 复用（subsession_run 新会话即使不继承全局工具也有视觉能力）；
 * 本 Agent 只声明自优化专属能力与写范围守卫。
 */
export const tools: Record<string, import("@gebai/sdk").Tool> = {
  read_feedback: readFeedbackTool,
  run_tests: runTestsTool,
  rollback: rollbackTool,
  journal: journalTool,
  backlog: backlogTool,
  page_capture: pageCaptureTool,
}
export const requiresApproval = { run_tests: true, rollback: true }
export const preload = false
export const envVars = [
  { name: "SELF_OPTIMIZE_PROJECT", description: "优化工作根：歌白仓库根路径，自我优化（self_optimize）文件操作以它为基准（写范围守卫亦按它界定仓库边界）；未设置时脚本调试（dev）模式自动推导源码仓库根（提示词「项目根」注记同步注入），二进制模式须显式配置" },
]
export const def: SubAgentDef = {
  name,
  description,
  systemPrompt,
  tools,
  requiresApproval,
  preload,
  envVars,
  writeGuard,
  // 依赖自动装载（DESIGN「子Agent 依赖与自动装载」）：装载/预加载 self_optimize 时系统连带装载
  // code 与 vision——文件/分析类工具直接用 code_* 命名空间，通用编码工作流提示词由 code 提供，
  // 不重复定义；视觉能力用 vision_*（截图语义分析/读图——新会话不继承全局工具时仍可用）
  dependencies: ["code", "vision"],
  // 默认项目根兜底（{AGENT}_PROJECT 未配置时）：dev 模式自动推导歌白仓库根——提示词「项目根」注记、
  // subsession_run 新会话工作目录与项目 AGENTS.md 注入随绑定生效（二进制模式无兜底，须显式配置）
  projectRoot: (env) => selfOptimizeRoot(env) ?? undefined,
}
