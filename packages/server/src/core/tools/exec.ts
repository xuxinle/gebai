/** 脚本执行类全局工具（sh；py 见 core/exec/py-tool.ts（工具桥，仅本地模式）、js 见 core/exec/js-tool.ts）
 *  ——本文件一并登记注册条目，自 core/tools.ts 按域拆分。 */
import type { Tool, ToolContext } from "../base/types"
import { jsTool } from "../exec/js-tool"
import { pyTool } from "../exec/py-tool"
import { shApprovalFreeAllowed, validateShCommandSafeMode } from "../security/safety"
import { shWaitMs } from "../support/exec-opts"
import { shTaskLifetimeMs } from "../exec/sh-tasks"
import { truncate } from "../support/truncate"
import { schema, type GlobalToolEntry } from "./shared"

/** sh/py 结构化 data 中 stdout/stderr 字符上限：data 供编排引用（分支判定/字段映射），超长截断防映射膨胀；完整文本以 output（及截断文件）为准。 */
const SCRIPT_DATA_TEXT_CAP = 100000

/** 转后台时随结果附上的已产出输出尾部字符数（stdout/stderr 各留一段，供模型判断进展、决定是否继续等）。 */
const SH_PARTIAL_TAIL_CHARS = 2000

/** sh 结构化输出：同步返回与转后台共用（转后台时附 taskId/pid，exitCode 为 null、退出码后续经 bg_task 查询）。 */
const shOutputSchema = schema({
  stdout: { type: "string", description: "标准输出（超长截断至 100k 字符）" },
  stderr: { type: "string", description: "标准错误（超长截断至 100k 字符）" },
  exitCode: { type: "integer", description: "退出码（0=成功）；同步等待超时转后台时为 null（任务仍在运行）" },
  taskId: { type: "string", description: "后台任务 id（async:true 启动或同步等待超时转后台时返回；用 bg_task 查询/等待/终止）" },
  pid: { type: "integer", description: "后台任务进程 id（未知为 null）" },
  status: { type: "string", description: "转后台时的任务状态（running）" },
}, ["stdout", "stderr", "exitCode"])

function scriptData(stdout: string, stderr: string, exitCode: number | null): Record<string, unknown> {
  return {
    stdout: stdout.length > SCRIPT_DATA_TEXT_CAP ? stdout.slice(0, SCRIPT_DATA_TEXT_CAP) : stdout,
    stderr: stderr.length > SCRIPT_DATA_TEXT_CAP ? stderr.slice(0, SCRIPT_DATA_TEXT_CAP) : stderr,
    exitCode,
  }
}

/** 转后台结果附带已产出输出尾部（stdout/stderr 分段，空流省略）。 */
function partialOutput(stdout: string, stderr: string): string {
  const tail = (s: string) => (s.length > SH_PARTIAL_TAIL_CHARS ? s.slice(-SH_PARTIAL_TAIL_CHARS) : s)
  const o = tail(stdout)
  const e = tail(stderr)
  if (!o.trim() && !e.trim()) return "\n（尚未产出输出）"
  return `${o.trim() ? `\n已产出 stdout（尾部 ${o.length} 字符）:\n${o}` : ""}${e.trim() ? `\n已产出 stderr（尾部 ${e.length} 字符）:\n${e}` : ""}`
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

/** 脚本 stdin 序列化：对象/数组转 JSON 文本（双引号，Python json.loads 可直接解析），其余按字符串。 */
function scriptInput(v: unknown): string | undefined {
  if (v == null) return undefined
  if (typeof v === "object") return JSON.stringify(v)
  return String(v)
}

export const shTool: Tool = {
  name: "sh",
  description: "执行 Shell 命令（Windows 经 PowerShell，POSIX 经 bash -c），按平台 shell 语法书写；stdout 为输出，退出码读 exitCode 字段。两种模式：① 同步（默认）——timeout 窗口（默认 60、上限 120 秒）内结束直接返回；超窗不终止命令、转后台返回 taskId。② 后台（async:true）——长耗时命令（构建/测试/安装）直接后台执行，立即返回 taskId。后台任务用 bg_task 查询/等待/终止。strict:true 时非 0 退出抛工具级错误（默认 false）。",
  requiresApproval: scriptRequiresApproval,
  card: { args: "code", codeField: "command", codeLang: "bash" },
  parameters: schema(
    {
      command: { type: "string" },
      workdir: { type: "string", description: "工作目录（相对路径基于会话工作目录/项目根解析），替代在命令里串联 cd" },
      input: { type: "string", description: "作为命令 stdin 的输入数据" },
      timeout: { type: "number", description: "超时秒数" },
      strict: { type: "boolean" },
      async: { type: "boolean" },
      approval: { type: "boolean", description: "安全命令设为 false 跳过用户审批" },
    },
    ["command"],
  ),
  outputSchema: shOutputSchema,
  async execute(args, ctx) {
    const command = String(args.command)
    const input = scriptInput(args.input)
    const workdir = args.workdir ? ctx.resolvePath(String(args.workdir)) : ctx.workdir
    // 工作目录注记：执行目录非会话默认目录（workdir 参数 / project 参数路由 / 项目绑定会话）时标注——
    // 「命令在哪个目录执行」一目了然（bun test 等按 cwd 发现目标的工具，目录不对是最常见根因）
    const cwdNote = workdir !== (ctx.sessionWorkdir ?? workdir) ? `\n（工作目录: ${workdir}）` : ""
    // 安全模式：只读命令白名单 + 输出重定向限用户目录（降级而非禁用；解析 fail-closed）
    if (ctx.safeMode) {
      const deny = validateShCommandSafeMode(command, ctx)
      if (deny) return { output: deny }
    }
    // 异步后台执行（DESIGN「sh 执行」）：spawn 进后台 + 落盘会话 tmp/sh-tasks/，立即返回 taskId
    if (args.async === true) {
      if (!ctx.shTasks) return { output: "当前环境不支持后台任务执行（shTasks 服务未注入）。" }
      const rec = await ctx.shTasks.start(command, { cwd: workdir, env: ctx.env, input, maxMs: shTaskLifetimeMs(args.timeout) })
      return {
        output: `[后台任务已启动] taskId: ${rec.id}\n命令: ${command}${cwdNote}\n（后台执行中不阻塞会话——可先处理其他任务，之后用 bg_task action=status id=${rec.id} 查询输出，action=wait 阻塞等待完成，action=stop 终止；输出日志 sh-tasks/${rec.id}.log，相对会话工作目录）`,
        data: { taskId: rec.id, pid: rec.pid, status: "running" },
      }
    }
    // 统一执行路径（DESIGN「sh 执行」）：命令一律经后台任务机制执行，同步调用只等一个等待窗口——窗口内结束
    // 按同步语义返回（stdout/stderr 分离、退出码照旧）；窗口到期命令仍在运行则**不终止**，转后台返回 taskId
    // 由 bg_task 继续跟踪（慢命令不再因同步等待超时而白跑一场，模型也不必凭猜测重跑）
    const waitMs = shWaitMs(args.timeout)
    if (ctx.shTasks && !ctx.safeMode) {
      const r = await ctx.shTasks.run(command, { cwd: workdir, env: ctx.env, input, waitMs, signal: ctx.signal })
      if (r.started) {
        if (r.aborted) {
          const stderr = r.stderr ? `${r.stderr}\n[interrupted by user]` : "[interrupted by user]"
          return { ...(await truncate(`${r.stdout}\n${stderr}\n[exit 124]` + cwdNote, "sh", ctx)), data: scriptData(r.stdout, stderr, 124) }
        }
        if (!r.finished) {
          const truncNote = r.truncated ? `\n（输出超过捕获上限，完整输出见日志 sh-tasks/${r.record.id}.log）` : ""
          const strictNote = args.strict === true ? "\n（strict: true 未生效——任务尚未结束、退出码未知，可用 bg_task 查询终态）" : ""
          return {
            output:
              `[同步等待超时 → 已转后台继续执行] taskId: ${r.record.id}\n命令: ${command}${cwdNote}` +
              `\n（等待 ${Math.round(waitMs / 1000)} 秒未结束，命令**仍在后台运行、未被终止**——用 bg_task action=status id=${r.record.id} 查询进度、action=wait 阻塞等待完成、action=stop 终止；输出日志 sh-tasks/${r.record.id}.log，相对会话工作目录）${truncNote}${strictNote}${partialOutput(r.stdout, r.stderr)}`,
            data: { ...scriptData(r.stdout, r.stderr, null), taskId: r.record.id, pid: r.record.pid, status: "running" },
          }
        }
        const code = r.record.exitCode ?? 1
        // strict：非 0 退出码转工具级异常（js 编排内未捕获即中断整个脚本，try/catch 可容错继续）
        if (args.strict === true && code !== 0) {
          throw new Error(`命令执行失败（exit ${code}）${r.stderr ? `：\n${r.stderr.slice(0, 2000)}` : ""}`)
        }
        const out = code === 0 ? r.stdout : `${r.stdout}\n${r.stderr}\n[exit ${code}]`
        // 成功但无输出：明确提示（区分「命令成功无输出」与「输出捕获失败/静默吞掉」）
        const final = code === 0 && !r.stdout.trim() ? "（命令执行成功，无输出）" : out
        return { ...(await truncate(final + cwdNote, "sh", ctx)), data: scriptData(r.stdout, r.stderr, code) }
      }
      // 后台启动失败（并发超限等）：回退下面的同步执行，命令照旧能跑
    }
    // 同步回退路径：同一 `timeout` 在此为**杀进程上限**（无后台服务可用，只有同步执行一条路）
    const { stdout, stderr, code } = await ctx.runCommand(command, { workdir, env: ctx.env, input, timeoutMs: waitMs })
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

/** py 工具（工具桥，仅本地模式）与解释器解析的实现见 core/exec/py-tool.ts：此处 re-export
 *  保持既有引用路径（tools/index.ts、测试）稳定。 */
export { pyTool, resolvePythonCmd, _resetPythonCmdCache } from "../exec/py-tool"

export const globalTools: GlobalToolEntry[] = [
  // sh/py 统一 projectAware({ workdir: true }) 包装：workdir 参数切换执行目录（项目机制）
  { name: "sh", tool: shTool, project: "workdir" },
  { name: "py", tool: pyTool, project: "workdir" },
  { name: "js", tool: jsTool },
]
