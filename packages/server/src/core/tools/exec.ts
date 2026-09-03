/** 脚本执行类全局工具（sh/py；js 见 core/js-tool.ts，本文件一并登记注册条目）——自 core/tools.ts 按域拆分。 */
import { randomUUID } from "node:crypto"
import type { Tool, ToolContext } from "../base/types"
import { jsTool } from "../exec/js-tool"
import { PY_SAFE_BOOTSTRAP, shApprovalFreeAllowed, validateShCommandSafeMode } from "../security/safety"
import { scriptTimeoutMs } from "../support/exec-opts"
import { shTaskLifetimeMs } from "../exec/sh-tasks"
import { truncate } from "../support/truncate"
import { schema, type GlobalToolEntry } from "./shared"

/** sh/py 结构化 data 中 stdout/stderr 字符上限：data 供编排引用（分支判定/字段映射），超长截断防映射膨胀；完整文本以 output（及截断文件）为准。 */
const SCRIPT_DATA_TEXT_CAP = 100000

/** sh/py 共用结构化输出：{ stdout, stderr, exitCode }（编排可按 exitCode 分支、按 stdout 映射）。 */
const scriptOutputSchema = schema({
  stdout: { type: "string", description: "标准输出（超长截断至 100k 字符）" },
  stderr: { type: "string", description: "标准错误（超长截断至 100k 字符）" },
  exitCode: { type: "integer", description: "退出码（0=成功）" },
}, ["stdout", "stderr", "exitCode"])

function scriptData(stdout: string, stderr: string, exitCode: number): Record<string, unknown> {
  return {
    stdout: stdout.length > SCRIPT_DATA_TEXT_CAP ? stdout.slice(0, SCRIPT_DATA_TEXT_CAP) : stdout,
    stderr: stderr.length > SCRIPT_DATA_TEXT_CAP ? stderr.slice(0, SCRIPT_DATA_TEXT_CAP) : stderr,
    exitCode,
  }
}

/** sh/py 免审参数（approval:false 跳过本次审批）的动态审批判定：缺省/true 需审批；显式 false 时
 *  **强制白名单校验**——仅只读（validateShCommandSafeMode）或测试/静态检查类命令（shApprovalFreeAllowed）
 *  放行免审，其余仍需审批（防提示词注入借免审标记执行任意命令）；py 的 code 为任意代码、无法静态判定，
 *  免审标记不生效。 */
function scriptRequiresApproval(args: Record<string, unknown>, ctx?: ToolContext): boolean {
  if (args.approval !== false) return true
  // 安全模式：sh 在 execute 内按只读白名单降级（非白名单命令直接被拒并回提示），风险已由白名单
  // 约束——审批层不再重复拦截，免审标记直接生效（降级语义与审批语义分层）
  if (ctx?.safeMode) return false
  if (ctx && typeof args.command === "string" && shApprovalFreeAllowed(args.command, { sandboxed: ctx.sandboxed, home: ctx.home, user: ctx.user, workdir: ctx.workdir })) return false
  return true
}

const SCRIPT_APPROVAL_PARAM = {
  approval: { type: "boolean", description: "可选：本次调用是否需要用户审批（默认 true 需审批）；仅对明确安全的只读命令（cat/ls/git status 等）或测试/静态检查类（bun test、pytest、tsc、eslint 等）可设 false 跳过审批（服务端强制白名单校验，不满足仍会弹审批）；风险命令勿关闭" },
}

/** 脚本 stdin 序列化：对象/数组转 JSON 文本（双引号，Python json.loads 可直接解析），其余按字符串。 */
function scriptInput(v: unknown): string | undefined {
  if (v == null) return undefined
  if (typeof v === "object") return JSON.stringify(v)
  return String(v)
}
export const shTool: Tool = {
  name: "sh",
  description: "执行 Shell 命令，输出以 stdout 为准。Windows 下经 cmd.exe 执行：命令串联用 &&/||/换行（; 非分隔符会被并入参数），引号与变量展开以 cmd 为准（无 $? / $VAR，环境变量用 %VAR%）；退出码直接读返回结果的 exitCode 字段（无需 echo $? / %errorlevel%）。指定工作目录用 workdir 参数（免 cd X && cmd 串联）或 project 参数（项目根为工作目录；非默认工作目录时输出末尾标注实际目录）。安全模式下降级为只读命令白名单（cat/grep/find/git 读类等），输出重定向限定用户目录内。长耗时命令（构建/测试/安装等）可传 async:true 后台执行——立即返回 taskId，先做其他事再用 bg_task 回头查询/等待/终止。",
  requiresApproval: scriptRequiresApproval,
  card: { args: "code", codeField: "command", codeLang: "bash" },
  parameters: schema(
    {
      command: { type: "string" },
      workdir: { type: "string", description: "可选：命令工作目录（相对路径基于会话工作目录/项目根解析，绝对路径本地模式可用）——替代 cd X && cmd 串联（Windows cmd 下引号语义更稳），不传用默认工作目录" },
      input: { type: "string", description: "可选：作为命令 stdin 的输入数据" },
      timeout: { type: "number", description: "可选：执行超时秒数（同步默认 300、上限 540，超时进程被终止并返回超时结果；async:true 时为任务生命周期上限，默认 1800、上限 3600）" },
      strict: { type: "boolean", description: "可选：true 时退出码非 0 抛工具级错误（js 编排「非 0 即中断」语义）；默认 false 非 0 退出作为正常结果返回" },
      async: { type: "boolean", description: "可选：true 后台异步执行——立即返回 taskId 不等待完成（适合构建/测试等长命令，期间可处理其他任务）；后续用 bg_task（action=status/wait/stop/list）查询输出、等待完成或终止" },
      ...SCRIPT_APPROVAL_PARAM,
    },
    ["command"],
  ),
  outputSchema: scriptOutputSchema,
  async execute(args, ctx) {
    const input = scriptInput(args.input)
    const workdir = args.workdir ? ctx.resolvePath(String(args.workdir)) : ctx.workdir
    // 工作目录注记：执行目录非会话默认目录（workdir 参数 / project 参数路由 / 项目绑定会话）时标注——
    // 「命令在哪个目录执行」一目了然（bun test 等按 cwd 发现目标的工具，目录不对是最常见根因）
    const cwdNote = workdir !== (ctx.sessionWorkdir ?? workdir) ? `\n（工作目录: ${workdir}）` : ""
    // 安全模式：只读命令白名单 + 输出重定向限用户目录（降级而非禁用；解析 fail-closed）
    if (ctx.safeMode) {
      const deny = validateShCommandSafeMode(String(args.command), ctx)
      if (deny) return { output: deny }
    }
    // 异步后台执行（DESIGN「sh 异步后台任务」）：spawn 进后台 + 落盘会话 tmp/sh-tasks/，立即返回 taskId
    if (args.async === true) {
      if (!ctx.shTasks) return { output: "当前环境不支持后台任务执行（shTasks 服务未注入）。" }
      const rec = await ctx.shTasks.start(String(args.command), { cwd: workdir, env: ctx.env, input, maxMs: shTaskLifetimeMs(args.timeout) })
      return {
        output: `[后台任务已启动] taskId: ${rec.id}\n命令: ${args.command}${cwdNote}\n（后台执行中不阻塞会话——可先处理其他任务，之后用 bg_task action=status id=${rec.id} 查询输出，action=wait 阻塞等待完成，action=stop 终止；输出日志 tmp/sh-tasks/${rec.id}.log）`,
        data: { taskId: rec.id, pid: rec.pid },
      }
    }
    const { stdout, stderr, code } = await ctx.runCommand(String(args.command), { workdir, env: ctx.env, input, timeoutMs: scriptTimeoutMs(args.timeout) })
    // strict：非 0 退出码转工具级异常（js 编排内未捕获即中断整个脚本，try/catch 可容错继续）
    if (args.strict === true && code !== 0) {
      throw new Error(`命令执行失败（exit ${code}）${stderr ? `：\n${stderr.slice(0, 2000)}` : ""}`)
    }
    const out = code === 0 ? stdout : `${stdout}\n${stderr}\n[exit ${code}]`
    // 成功但无输出：明确提示（区分「命令成功无输出」与「输出捕获失败/静默吞掉」）
    const final = code === 0 && !stdout.trim() ? "（命令执行成功，无输出）" : out
    return { ...(await truncate(final + cwdNote, "sh", ctx)), data: scriptData(stdout, stderr, code) }
  },
}

/** python 可执行文件探测缓存：undefined=未探测，null=已探测但未命中候选。 */
let pythonCmdCache: string | null | undefined

/** 测试用：重置探测缓存。 */
export function _resetPythonCmdCache(): void {
  pythonCmdCache = undefined
}

/** 探测可用的 python 命令（跨平台：Linux/macOS 多为 python3，Windows 多为 python/py），结果缓存。 */
export async function resolvePythonCmd(ctx: ToolContext): Promise<string> {
  if (pythonCmdCache != null) return pythonCmdCache
  for (const cand of ["python3", "python", "py"]) {
    const r = await ctx.runCommand(`${cand} --version`).catch(() => ({ stdout: "", stderr: "", code: 1 }))
    if (r.code === 0) {
      pythonCmdCache = cand
      return cand
    }
  }
  pythonCmdCache = "python"
  return "python"
}

export const pyTool: Tool = {
  name: "py",
  description: "执行 Python 代码（经临时文件），stdout 为输出。安全模式下审计钩子屏蔽写文件/进程/网络（仅保留文件读取）。",
  requiresApproval: scriptRequiresApproval,
  card: { args: "code", codeField: "code", codeLang: "python" },
  parameters: schema(
    {
      code: { type: "string", description: "Python 程序源码" },
      input: { type: "string", description: "可选：作为程序 stdin 的输入数据" },
      timeout: { type: "number", description: "可选：执行超时秒数（默认 300，上限 540；超时进程被终止并返回超时结果）" },
      strict: { type: "boolean", description: "可选：true 时退出码非 0 抛工具级错误（js 编排「非 0 即中断」语义）；默认 false 非 0 退出作为正常结果返回" },
      approval: { type: "boolean", description: "兼容参数：py 的 code 为任意代码、无法静态判定安全性，免审标记不生效（默认且恒需审批）" },
    },
    ["code"],
  ),
  outputSchema: scriptOutputSchema,
  async execute(args, ctx) {
    const code = String(args.code ?? "")
    const input = scriptInput(args.input)
    // 代码写临时文件执行：stdin 留给管道数据（原实现 code 走 stdin，无法同时传输入）；
    // 安全模式：前置审计钩子引导段（sys.addaudithook 拦写模式 open 与进程/网络/文件变更系统调用，仅保留文件读取）
    const finalCode = ctx.safeMode ? `${PY_SAFE_BOOTSTRAP}\n${code}` : code
    const { writeFile, rm } = await import("node:fs/promises")
    const scriptPath = `${ctx.workdir}/.gebai_py_${randomUUID().replace(/-/g, "")}.py`
    await writeFile(scriptPath, finalCode)
    try {
      // -X utf8 / PYTHONUTF8=1：强制 UTF-8 输出（Windows 默认 GBK 会造成乱码）
      const py = await resolvePythonCmd(ctx)
      const { stdout, stderr, code: exit } = await ctx.runCommand(`${py} -X utf8 "${scriptPath}"`, { workdir: ctx.workdir, env: { ...ctx.env, PYTHONUTF8: "1" }, input, timeoutMs: scriptTimeoutMs(args.timeout) })
      // strict：非 0 退出码转工具级异常（js 编排内未捕获即中断整个脚本，try/catch 可容错继续）
      if (args.strict === true && exit !== 0) {
        throw new Error(`程序执行失败（exit ${exit}）${stderr ? `：\n${stderr.slice(0, 2000)}` : ""}`)
      }
      const out = exit === 0 ? stdout : `${stdout}\n${stderr}\n[exit ${exit}]`
      // 成功但无输出：明确提示（区分「程序成功无输出」与「stdout 捕获失败」）；
      // 工作目录注记（与 sh 同规则）：project 参数路由/项目绑定时标注实际执行目录
      const cwdNote = ctx.workdir !== (ctx.sessionWorkdir ?? ctx.workdir) ? `\n（工作目录: ${ctx.workdir}）` : ""
      const final = exit === 0 && !stdout.trim() ? `（程序执行成功，无输出）${cwdNote}` : out + cwdNote
      return { ...(await truncate(final, "py", ctx)), data: scriptData(stdout, stderr, exit) }
    } finally {
      await rm(scriptPath, { force: true }).catch(() => {})
    }
  },
}

export const globalTools: GlobalToolEntry[] = [
  // sh/py 统一 projectAware({ workdir: true }) 包装：workdir 参数切换执行目录（项目机制）
  { name: "sh", tool: shTool, project: "workdir" },
  { name: "py", tool: pyTool, project: "workdir" },
  { name: "js", tool: jsTool },
]
