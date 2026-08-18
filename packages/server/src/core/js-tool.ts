/**
 * js 脚本工具（DESIGN「js 脚本工具：工具动态编程」）：以 Bun 子进程执行 JS/TS 代码，
 * 进程内注入 tools 工具调用桥（直接调用会话注册表中任意工具）与会话上下文 ctx
 * （user/sessionId/workdir/env/projects/messages），把「工具即函数」升级为真正的动态编程——
 * 完整语言能力（变量/函数/循环/条件/字符串处理/await）编排工具链，弥补 flow 声明式
 * 表达式语言的表达力上限。
 *
 * 执行模型：生成脚本文件（运行时桥前导 + 用户代码）→ 子进程运行（脚本调试 = bun 直跑，
 * 二进制 = `gebai exec` 隐藏子命令复用内嵌 Bun 运行时）→ stdio JSON 行协议桥接：
 * 子进程 tools.call 发 {t:"call"} 到 stdout、父进程回 {t:"res"} 到 stdin；
 * console.* 输出与返回值经 {t:"log"}/{t:"done"} 回传。超时/取消按进程树终止（同 sh）。
 *
 * 三条分发层守卫（runJsScript）：
 * - 免审拦截：免审运行（approval:false / 免审动态工具）时内部调用需审批的工具按剥离免审标记后的
 *   审批姿态拒绝（与引擎无交互硬门槛同规则）；默认审批运行的 js 一次审批覆盖内部调用。
 * - 嵌套封死：RPC 执行工具统一携带 fromJsBridge 标记，js/动态工具 execute 见标记即拒——
 *   js→flow→js 交替递归无通道；动态工具运行器（depth 1）内不可再 defineTool。
 * - env 不落盘：ctx.env 改为子进程内运行时引用 process.env（spawn 已传同源环境），
 *   脚本文件不再明文嵌入密钥；messages 等会话数据仍嵌入（任务数据契约）。
 */
import { spawn } from "node:child_process"
import { randomUUID } from "node:crypto"
import { rm, writeFile } from "node:fs/promises"
import { join } from "node:path"
import type { DynamicToolDef, Tool, ToolContext } from "./types"
import { isSensitive } from "./env"
import { isToolBlockedInSafeMode, safeModeRestrictionMsg, scanJsReadOnly, stripApprovalFlags } from "./safety"
import type { ContentBlock } from "@gebai/sdk"
import { isBinaryMode } from "./config"
import { truncate, scriptTimeoutMs } from "./tools"

/** 单次脚本执行内工具调用总数上限（与 flow FLOW_MAX_STEPS 对齐）。 */
export const JS_TOOL_MAX_CALLS = 100
/** 动态工具 execute 源码长度上限（防巨源码撑爆 chat.json——源码随会话持久化）。 */
export const JS_DYNAMIC_SOURCE_CAP = 100_000
/** 单条 RPC 日志/结果字段字符上限（防巨对象撑爆协议行与内存）。 */
export const JS_RPC_FIELD_CAP = 100_000
/** 结构化 data 中 logs/result 字段上限（与 sh/py SCRIPT_DATA_TEXT_CAP 对齐）。 */
export const JS_DATA_TEXT_CAP = 100_000
/** 输出中返回值预览保留字符数（完整值在 data.result）。 */
export const JS_RESULT_PREVIEW_CHARS = 2000
/** 内层工具 blocks 透传上限（去重后；图片/图表等重内容限量，防巨量 blocks 撑爆结果）。 */
export const JS_BLOCKS_CAP = 10
/** 会话上下文 messages 注入：最多条数与单条内容字符上限。 */
export const JS_CONTEXT_MESSAGES_MAX = 50
export const JS_CONTEXT_MESSAGE_CHARS = 2000
/** 非 JSON 输出行的日志透传长度上限（更长——多为子进程侧 2MB 截断产生的非法 JSON——丢弃留注）。 */
const JS_NON_PROTOCOL_LINE_CAP = 100_000

/** 子进程运行时桥前导：工具调用（stdio JSON 行协议）+ console 重定向 + 引导执行用户代码。
 *  纯 ES5 风格（无模板字符串/箭头函数），避免嵌入转义问题；不 import 任何模块（模块格式无关）。 */
function scriptPreamble(): string {
  return [
    '"use strict"',
    "var __G_out = process.stdout.write.bind(process.stdout)",
    "var __G_seq = 0",
    "var __G_pending = new Map()",
    "var __G_buf = ''",
    "function __G_send(obj) {",
    "  var s = JSON.stringify(obj)",
    "  if (s.length > 2000000) s = s.slice(0, 2000000)",
    "  __G_out(s + '\\n')",
    "}",
    "function __G_finish(obj, code) {",
    "  try {",
    "    process.stdout.write(JSON.stringify(obj) + '\\n', function () { process.exit(code) })",
    "    setTimeout(function () { process.exit(code) }, 500).unref()",
    "  } catch (e) { process.exit(code) }",
    "}",
    "function __G_fmt(v) {",
    "  if (typeof v === 'string') return v",
    "  try { var s = JSON.stringify(v); return s === undefined ? String(v) : s } catch (e) { return String(v) }",
    "}",
    "['log', 'info', 'warn', 'error', 'debug'].forEach(function (lv) {",
    "  var orig = console[lv] ? console[lv].bind(console) : function () {}",
    "  console[lv] = function () {",
    "    var args = Array.prototype.slice.call(arguments)",
    "    var level = (lv === 'info' || lv === 'debug') ? 'log' : lv",
    "    __G_send({ t: 'log', level: level, text: args.map(__G_fmt).join(' ') })",
    "  }",
    "})",
    "process.stdin.setEncoding('utf8')",
    "process.stdin.on('data', function (chunk) {",
    "  __G_buf += chunk",
    "  var idx",
    "  while ((idx = __G_buf.indexOf('\\n')) >= 0) {",
    "    var line = __G_buf.slice(0, idx)",
    "    __G_buf = __G_buf.slice(idx + 1)",
    "    if (!line.trim()) continue",
    "    var msg = null",
    "    try { msg = JSON.parse(line) } catch (e) { continue }",
    "    if (!msg || msg.t !== 'res') continue",
    "    var p = __G_pending.get(msg.id)",
    "    if (!p) continue",
    "    __G_pending.delete(msg.id)",
    "    if (msg.ok) p.resolve(msg.result)",
    "    else p.reject(new Error(msg.error))",
    "  }",
    "})",
    "function __G_call(name, params) {",
    "  __G_seq++",
    "  var id = __G_seq",
    "  return new Promise(function (resolve, reject) {",
    "    __G_pending.set(id, { resolve: resolve, reject: reject })",
    "    __G_send({ t: 'call', id: id, name: String(name), params: params || {} })",
    "  })",
    "}",
    "var tools = { call: __G_call }",
    "tools = new Proxy(tools, { get: function (target, name) {",
    "  if (typeof name !== 'string' || name in target) return target[name]",
    "  return function (params) { return __G_call(name, params) }",
    "}})",
    "function defineTool(def) {",
    "  if (!def || typeof def !== 'object') return Promise.reject(new Error('defineTool 需要工具定义对象 { name, description, parameters, execute }'))",
    "  if (typeof def.execute !== 'function') return Promise.reject(new Error('defineTool: execute 必须为函数 async (args, ctx) => ({ output })（与子Agent 工具同签名）'))",
    "  __G_seq++",
    "  var id = __G_seq",
    "  var name = String(def.name || '')",
    "  return new Promise(function (resolve, reject) {",
    "    __G_pending.set(id, { resolve: resolve, reject: reject })",
    "    __G_send({ t: 'def', id: id, name: name, description: String(def.description || ''), parameters: def.parameters || { type: 'object', properties: {} }, requiresApproval: def.requiresApproval, source: def.execute.toString() })",
    "  }).then(function (res) {",
    // 注册成功后注入脚本全局（同脚本内立即可像内置函数一样调用；后续脚本经服务端声明生成）
    "    try { if (/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(name)) globalThis[name] = function (params) { return __G_call(name, params) } } catch (e) {}",
    "    return res",
    "  })",
    "}",
  ].join("\n")
}

/** 安全模式运行时 shim（注入子进程脚本，用户代码执行前生效；纯 ES5 同前导）：
 *  屏蔽 Bun 写/进程/网络/数据库 API、字符串代码执行通道与运行时模块加载入口，仅保留文件读取。
 *  Bun 全局绑定不可替换（non-configurable），改为**对象属性覆写**（属性可写：write/spawn/$ 等覆写为抛错桩，
 *  Bun.file 包装为返回拦截 write/writer/sink/truncate 的 Proxy）；个别不可覆写的 getter（Bun.fetch/sqlite）
 *  由静态扫描（scanJsReadOnly）前置拒绝。eval/Function/fetch/Worker 等全局删除、Function.prototype.constructor
 *  中性化防 (fn).constructor 回收；process 仅拦 binding/dlopen/getBuiltinModule/kill（桥协议依赖 stdin/stdout/exit）；
 *  字符串定时器拒绝。import()/require() 等动态加载通道同样由静态扫描前置拒绝——模块实例可拿到未受控的全新全局环境。 */
function safeModeShim(): string {
  return [
    "function __G_safe() {",
    "  var die = function (what) { throw new Error('安全模式：' + what + ' 已屏蔽（仅保留文件读取；写文件用 write 工具、执行命令用 sh 白名单、网络用 fetch_url 工具）') }",
    "  try { delete globalThis.eval } catch (e) {}",
    "  try { delete globalThis.Function } catch (e) {}",
    "  try { delete globalThis.fetch } catch (e) {}",
    "  try { delete globalThis.WebSocket } catch (e) {}",
    "  try { delete globalThis.EventSource } catch (e) {}",
    "  try { delete globalThis.Worker } catch (e) {}",
    "  try { delete globalThis.SharedWorker } catch (e) {}",
    "  try { Function.prototype.constructor = function () { die('Function 构造器') } } catch (e) {}",
    "  try { process.binding = function () { die('process.binding') } } catch (e) {}",
    "  try { process.dlopen = function () { die('process.dlopen') } } catch (e) {}",
    "  try { if (typeof process.getBuiltinModule === 'function') process.getBuiltinModule = function (n) { die('process.getBuiltinModule(' + n + ')') } } catch (e) {}",
    "  try { process.kill = function () { die('process.kill') } } catch (e) {}",
    "  var _st = setTimeout, _si = setInterval",
    "  setTimeout = function (fn) { if (typeof fn !== 'function') die('字符串定时器代码'); return _st.apply(null, arguments) }",
    "  setInterval = function (fn) { if (typeof fn !== 'function') die('字符串定时器代码'); return _si.apply(null, arguments) }",
    "  var _Bun = globalThis.Bun",
    "  if (_Bun) {",
    "    var blocked = function (what) { return function () { die(what) } }",
    "    var denyList = ['write','spawn','spawnSync','spawnViaNode','serve','listen','connect','udpSocket','dns','build','plugin','preload','$','Shell','ShellError','ShellSyntaxError','generateHeapSnapshot','sql','SQL','SQLite','TCP','TCPSocket','UDPSocket','unix','NodeFS','auto','add','remove','install','update','publish','link','unlink','pm','upgrade']",
    "    for (var i = 0; i < denyList.length; i++) {",
    "      try { _Bun[denyList[i]] = blocked('Bun.' + denyList[i]) } catch (e) {} // 非可写属性（fetch/sqlite 等 getter）由静态扫描前置拒绝",
    "    }",
    "    var _file = _Bun.file",
    "    try {",
    "      _Bun.file = function (p, o) {",
    "        var f = _file(p, o)",
    "        return new Proxy(f, { get: function (ft, fk) {",
    "          if (fk === 'write' || fk === 'writer' || fk === 'sink' || fk === 'truncate') die('BunFile.' + fk + '（写文件）')",
    "          var v = ft[fk]",
    "          return typeof v === 'function' ? v.bind(f) : v",
    "        }})",
    "      }",
    "    } catch (e) {}",
    "  }",
    "}",
    "__G_safe()",
  ].join("\n")
}

/** 生成工具内置函数声明（模块顶层作用域）：每个已启用工具一个 `async function {name}(params)`，
 *  用户代码在 __G_main 内直接 `await read({...})` 像内置函数一样调用（无需 tools. 前缀）；
 *  声明在模块作用域，用户代码内同名 let/const/function 遮蔽不冲突。
 *  过滤：非合法标识符/保留字/与注入全局（tools/ctx/input/console/__G_*）冲突的名字跳过（仍可经 tools.call 动态调用）。 */
const JS_RESERVED = new Set([
  "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else", "enum",
  "export", "extends", "false", "finally", "for", "function", "if", "import", "in", "instanceof", "new", "null",
  "return", "super", "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while", "with", "yield",
  "let", "static", "await", "implements", "interface", "package", "private", "protected", "public", "eval", "arguments",
])
const JS_GLOBAL_DENY = new Set(["tools", "ctx", "input", "console", "process", "globalThis", "defineTool"])

export function toolFnDecls(toolNames: string[]): string {
  const seen = new Set<string>()
  const lines: string[] = []
  for (const raw of toolNames) {
    const name = String(raw)
    if (!/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(name)) continue
    if (JS_RESERVED.has(name) || JS_GLOBAL_DENY.has(name) || name.startsWith("__G_")) continue
    // JS 全局名（JSON/Promise/fetch/Bun/Date 等子Agent 大写或撞名工具）不声明：模块作用域函数声明会遮蔽全局，
    // 用户代码 `JSON.parse` 会莫名变成工具调用（仍可经 tools.call 动态调用）
    if (name in globalThis) continue
    if (seen.has(name)) continue
    seen.add(name)
    lines.push(`async function ${name}(params) { return __G_call(${JSON.stringify(name)}, params) }`)
  }
  return lines.join("\n")
}

/** 生成完整子进程脚本：前导 + 工具内置函数声明（模块作用域）+ 用户代码（包进 __G_main，
 *  支持顶层 await 与 return 返回值；内部 let/const 同名遮蔽顶层函数声明，无冲突）+ 引导。 */
/** 生成完整子进程脚本：前导 + 安全模式 shim（可选）+ 工具内置函数声明（模块作用域，用户代码同名 let/const/function 遮蔽无冲突）
 *  + __G_main 开头 + 用户代码（支持顶层 await 与 return 返回值）+ 引导。
 *  prelude：__G_main 之前追加的额外声明（动态工具运行器注入 `var __TOOL_FN = (源码)` 用）；
 *  safeMode：注入只读运行时 shim（写/进程/网络 API 屏蔽，调用方须先过 scanJsReadOnly 静态扫描）。 */
export function buildChildScript(userCode: string, injected: { ctx: unknown; input: unknown }, toolNames: string[] = [], prelude = "", safeMode = false): string {
  // env 不落盘：ctx 历来整体明文嵌入 workdir 下的临时脚本文件（非沙箱模式含合并后的全部 process.env 密钥）——
  // 子进程本就从 spawn env 拿到同源环境（沙箱脱敏同规则），文件内嵌一份纯冗余且有崩溃残留风险，改为运行时引用。
  // messages 等会话数据仍嵌入（ctx 注入契约，属任务数据而非密钥）。
  const ctxObj = { ...((injected.ctx ?? {}) as Record<string, unknown>) }
  delete ctxObj.env
  const ctxJson = JSON.stringify(ctxObj)
  const inputJson = JSON.stringify(injected.input ?? null)
  return (
    scriptPreamble() +
    (safeMode ? `\n${safeModeShim()}\n` : "") +
    `\n${toolFnDecls(toolNames)}\n` +
    (prelude ? `${prelude}\n` : "") +
    "async function __G_main() {\n" +
    `var ctx = ${ctxJson}\nctx.env = process.env\nvar input = ${inputJson}\n` +
    userCode +
    "\n}\n__G_main().then(\n" +
    "  function (value) {\n" +
    "    var v = null\n" +
    "    try { v = value === undefined ? null : JSON.parse(JSON.stringify(value)) } catch (e) { v = __G_fmt(value) }\n" +
    "    __G_finish({ t: 'done', value: v }, 0)\n" +
    "  },\n" +
    "  function (err) {\n" +
    "    __G_finish({ t: 'fail', error: err && err.stack ? String(err.stack) : String(err) }, 1)\n" +
    "  }\n" +
    ")\n"
  )
}

/** 子进程命令：脚本调试模式 = bun 直跑；二进制模式 = `gebai exec` 隐藏子命令（复用内嵌 Bun
 *  运行时自执行），统一带入口段 [execPath, 入口, "exec", script]：
 *  - 编译单文件（desktop `gebai.exe`）：子进程 argv[1] 自动为内嵌虚拟入口，入口段仅占位，
 *    index.ts 按「exec 在 argv[2] 或 argv[3]」双位置路由；
 *  - 容器形态（execPath=bun 跑 dist 打包产物，包内各模块 import.meta.path 一致指向打包产物）：
 *    入口段是真实 dist 入口——缺失时 bun 会把 exec 当命令名、script 当其参数直接报错。
 *  binary 参数仅供测试注入（默认取 isBinaryMode()）。 */
export function jsRuntimeCommand(scriptPath: string, binary = isBinaryMode()): string[] {
  return binary ? [process.execPath, import.meta.path, "exec", scriptPath] : [process.execPath, scriptPath]
}

/** 会话上下文快照（注入脚本 ctx）：user/session/workdir/env（沙箱模式脱敏同 sh 子进程）/projects/messages。 */
export function scriptSessionContext(ctx: ToolContext): Record<string, unknown> {
  const mergedEnv = { ...process.env, ...ctx.env }
  const env = ctx.sandboxed ? Object.fromEntries(Object.entries(mergedEnv).filter(([k]) => !isSensitive(k))) : mergedEnv
  let messages: Array<{ role: string; name?: string; content: string }> = []
  try {
    messages = (ctx.recentMessages?.() ?? []).slice(-JS_CONTEXT_MESSAGES_MAX).map((m) => ({
      role: m.role,
      ...(m.name ? { name: m.name } : {}),
      content: m.content.length > JS_CONTEXT_MESSAGE_CHARS ? `${m.content.slice(0, JS_CONTEXT_MESSAGE_CHARS)}…` : m.content,
    }))
  } catch {
    /* 上下文不可用（测试桩）：注入空列表 */
  }
  return {
    user: ctx.user,
    sessionId: ctx.sessionId,
    workdir: ctx.workdir,
    home: ctx.home,
    sandboxed: ctx.sandboxed,
    env,
    projects: ctx.projects,
    messages,
  }
}

interface JsRunResult {
  exitCode: number
  logs: string[]
  result: unknown
  calls: Array<{ name: string; ok: boolean; error?: string }>
  /** 内层工具 blocks 汇集（去重限量，JS_BLOCKS_CAP 封顶）——透传到 js 结果供 UI/模型消费。 */
  blocks: ContentBlock[]
  error?: string
  timedOut: boolean
  interrupted: boolean
  spawnError?: string
}

/** 执行子进程脚本并桥接工具调用。结构/约定见模块注释；失败信息聚合进 error 供调用方组装输出。
 *  depth：子进程层级（0=js 工具本体，1=动态工具运行器）——动态工具（runtimeDefined）仅 depth 0 可调用、
 *  depth>0 不可 defineTool（防递归注册与嵌套子进程）。
 *  approvalFree：本次运行为免审（js 的 approval:false / 免审动态工具）——脚本体未经用户审阅，
 *  内部调用需审批的工具在 RPC 分发层拦截（按剥离免审标记后的审批姿态解析，与引擎无交互硬门槛同规则）。 */
async function runJsScript(
  scriptPath: string,
  ctx: ToolContext,
  opts: { timeoutMs: number; depth?: number; approvalFree?: boolean },
): Promise<JsRunResult> {
  const mergedEnv = { ...process.env, ...ctx.env }
  const env = ctx.sandboxed
    ? Object.fromEntries(Object.entries(mergedEnv).filter(([k]) => !isSensitive(k)))
    : mergedEnv
  const isWin = process.platform === "win32"
  const cmd = jsRuntimeCommand(scriptPath)
  // detached（Unix）：进程组组长，超时/取消按进程组整体终止（同 sh）；Windows 不 detached（管道输出兼容），taskkill /T 杀树
  const child = spawn(cmd[0], cmd.slice(1), {
    cwd: ctx.workdir,
    env,
    shell: false,
    stdio: ["pipe", "pipe", "pipe"],
    detached: !isWin,
  })
  const out: JsRunResult = { exitCode: 0, logs: [], result: null, calls: [], blocks: [], timedOut: false, interrupted: false }
  const seenBlockKeys = new Set<string>()
  let stdoutBuf = ""
  let callCount = 0
  let settled = false
  let timer: ReturnType<typeof setTimeout> | undefined

  const killAll = () => {
    try {
      if (!child.pid) return
      if (isWin) {
        try {
          spawn("taskkill", ["/pid", String(child.pid), "/T", "/F"], { stdio: "ignore" }).on("error", () => {})
        } catch {
          /* taskkill 不可用 */
        }
        try {
          child.kill("SIGKILL")
        } catch {
          /* 已退出 */
        }
      } else {
        try {
          process.kill(-child.pid, "SIGKILL")
        } catch {
          /* 进程组不存在（已退出） */
        }
        try {
          child.kill("SIGKILL")
        } catch {
          /* 已退出 */
        }
      }
    } catch {
      /* 进程已退出 */
    }
  }

  const dispatchLine = async (line: string): Promise<void> => {
    let msg: { t?: string; id?: number; name?: string; params?: Record<string, unknown>; level?: string; text?: string; value?: unknown; error?: string; description?: string; parameters?: Record<string, unknown>; source?: string; requiresApproval?: boolean }
    try {
      msg = JSON.parse(line)
    } catch {
      // 非 JSON 行按日志透传（绕过 console 补丁的直写输出）；超长行多为子进程侧 2MB 截断产生的非法 JSON——丢弃留注，防垃圾文本刷屏
      out.logs.push(line.length > JS_NON_PROTOCOL_LINE_CAP ? "（超长非协议输出行已丢弃）" : line)
      return
    }
    if (msg.t === "log") {
      const level = msg.level === "warn" || msg.level === "error" ? msg.level : "log"
      out.logs.push(level === "log" ? String(msg.text ?? "") : `[${level}] ${String(msg.text ?? "")}`)
      return
    }
    if (msg.t === "done") {
      out.result = msg.value ?? null
      return
    }
    if (msg.t === "fail") {
      out.error = String(msg.error ?? "脚本执行失败")
      return
    }
    // id 应答类消息（call/def）：统一回写 stdin
    if (typeof msg.id !== "number") return
    const respond = (ok: boolean, payload: unknown) => {
      if (settled || child.stdin.destroyed) return
      try {
        child.stdin.write(`${JSON.stringify(ok ? { t: "res", id: msg.id, ok: true, result: payload } : { t: "res", id: msg.id, ok: false, error: String(payload) })}\n`)
      } catch {
        /* 子进程已退出 */
      }
    }
    // 运行时工具定义（defineTool）：注册会话级动态工具
    if (msg.t === "def") {
      if ((opts.depth ?? 0) > 0) {
        // 动态工具运行器内不可再注册（防递归注册/嵌套子进程——注册了 depth>0 也调不了，前置拒绝给清晰错误）
        out.calls.push({ name: `defineTool:${String(msg.name ?? "")}`, ok: false, error: "动态工具内不可 defineTool" })
        respond(false, "动态工具运行器内不能再 defineTool（防递归注册与嵌套子进程）")
        return
      }
      if (!ctx.defineDynamicTool) {
        respond(false, "当前环境不支持运行时工具定义（defineTool）")
        return
      }
      const def = {
        name: String(msg.name ?? ""),
        description: String(msg.description ?? ""),
        parameters: msg.parameters && typeof msg.parameters === "object" ? msg.parameters : { type: "object", properties: {} },
        source: String(msg.source ?? ""),
        // 原样透传（undefined = 未声明 → makeDynamicTool 默认 true 需审批；false 保持免审）：
        // 审批默认语义在 makeDynamicTool 处统一，序列化层不做归一化
        requiresApproval: msg.requiresApproval,
      }
      try {
        await ctx.defineDynamicTool(def)
        out.calls.push({ name: `defineTool:${def.name}`, ok: true })
        respond(true, { registered: def.name })
      } catch (err) {
        out.calls.push({ name: `defineTool:${def.name}`, ok: false, error: (err as Error).message })
        respond(false, `defineTool 失败: ${(err as Error).message}`)
      }
      return
    }
    if (msg.t === "call") {
      const name = String(msg.name ?? "")
      const params = (msg.params && typeof msg.params === "object" ? msg.params : {}) as Record<string, unknown>
      // 防嵌套：js 内不能再起 js（嵌套 RPC 桥子进程失控风险）；动态工具仅 depth 0 可调用（防递归子进程）
      if (name === "js" || name.endsWith("_js")) {
        out.calls.push({ name, ok: false, error: "嵌套 js 不可用" })
        respond(false, "js 脚本内不能再调用 js（防嵌套子进程）；需要 shell 用 tools.sh")
        return
      }
      const rt = ctx.registry.resolve(name)
      if (!rt) {
        out.calls.push({ name, ok: false, error: `未知工具 ${name}` })
        respond(false, `未知工具: ${name}`)
        return
      }
      if (rt.tool.runtimeDefined && (opts.depth ?? 0) > 0) {
        out.calls.push({ name: rt.name, ok: false, error: "动态工具嵌套受限" })
        respond(false, `动态工具 ${rt.name} 不能在 js/动态工具内调用（防递归嵌套子进程）`)
        return
      }
      // 安全模式：硬阻断工具（cron 调度类）在 RPC 分发层同规则拦截（与 flow step 层/引擎一致，无绕过通道）；
      // sh/py/write 等风险工具不再拦截——各自在 execute 内降级（白名单/审计钩子/写范围）
      if (ctx.safeMode && isToolBlockedInSafeMode(rt.name)) {
        const msg2 = safeModeRestrictionMsg(rt.name)
        out.calls.push({ name: rt.name, ok: false, error: "安全模式限制" })
        respond(false, msg2)
        return
      }
      if (++callCount > JS_TOOL_MAX_CALLS) {
        out.calls.push({ name: rt.name, ok: false, error: "调用总数超上限" })
        respond(false, `工具调用总数超上限（>${JS_TOOL_MAX_CALLS}），请精简脚本或拆分执行`)
        return
      }
      // 必填参数校验（类型检查的即时反馈近似）：缺参即拒绝并指出参数名，模型/脚本可当场修正
      const required = Array.isArray((rt.tool.parameters as { required?: unknown } | undefined)?.required) ? ((rt.tool.parameters as { required: string[] }).required) : []
      const missing = required.filter((k) => params[k] === undefined)
      if (missing.length) {
        const props = Object.keys((rt.tool.parameters as { properties?: Record<string, unknown> }).properties ?? {})
        out.calls.push({ name: rt.name, ok: false, error: `缺少必填参数 ${missing.join(", ")}` })
        respond(false, `工具 ${rt.name} 缺少必填参数: ${missing.join(", ")}${props.length ? `（参数: ${props.join(", ")}）` : ""}，请修正后重试`)
        return
      }
      // 免审运行（approval:false）时内部审批工具拦截：脚本体未经用户审阅，需审批工具不得经 RPC 免审执行——
      // 按剥离免审标记后的审批姿态解析（防脚本内再传 approval:false 自我免审，与引擎无交互硬门槛同规则；
      // requiresApproval 函数异常按需审批处理，同引擎）。默认审批运行的 js 一次审批覆盖内部调用（代码用户已审）
      if (opts.approvalFree) {
        const ra = rt.tool.requiresApproval
        let needs: boolean
        if (typeof ra === "function") {
          try {
            needs = !!(await ra(stripApprovalFlags(params) as Record<string, unknown>, ctx))
          } catch {
            needs = true
          }
        } else {
          needs = !!ra
        }
        if (needs) {
          out.calls.push({ name: rt.name, ok: false, error: "免审运行禁用审批工具" })
          respond(false, `本次 js 以免审模式（approval:false）运行，内部调用需审批的工具 ${rt.name} 被拒绝。请去掉 approval:false（整体审批覆盖内部调用），或改用只读/免审工具`)
          return
        }
      }
      try {
        // fromJsBridge 标记：js/动态工具 execute 见标记即拒——封死经 flow 等直执行工具回到 js 的所有嵌套路径
        const r = await rt.tool.execute(params, { ...ctx, fromJsBridge: true })
        const cap = (s: unknown): string | null => {
          const t = s == null ? "" : String(s)
          return t.length > JS_RPC_FIELD_CAP ? `${t.slice(0, JS_RPC_FIELD_CAP)}…` : t
        }
        let data: unknown = null
        try {
          data = JSON.parse(JSON.stringify(r.data ?? null, (_k, v) => (typeof v === "string" && v.length > JS_RPC_FIELD_CAP ? `${v.slice(0, JS_RPC_FIELD_CAP)}…` : v)))
        } catch {
          data = null // 结构化 data 不可序列化（BigInt/循环等）：丢弃，output 仍完整
        }
        // 内层 blocks 汇集（去重限量）：js 编排图片/图表/文件类工具时产物块透传到 js 结果，供 UI/模型消费
        for (const b of r.blocks ?? []) {
          if (seenBlockKeys.size >= JS_BLOCKS_CAP) break
          const key = `${b.type}:${(b as { path?: string; name?: string }).path ?? (b as { name?: string }).name ?? ""}`
          if (seenBlockKeys.has(key)) continue
          seenBlockKeys.add(key)
          out.blocks.push(b)
        }
        out.calls.push({ name: rt.name, ok: true })
        respond(true, {
          output: cap(r.output),
          data,
          blocks: r.blocks ?? [],
          truncated: !!r.truncated,
          filePath: r.filePath ?? null,
        })
      } catch (err) {
        const e = (err as Error).message ?? String(err)
        out.calls.push({ name: rt.name, ok: false, error: e })
        respond(false, `工具 ${rt.name} 执行失败: ${e}`)
      }
      return
    }
  }

  return new Promise<JsRunResult>((resolve) => {
    const finish = () => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      ctx.signal?.removeEventListener("abort", onAbort)
      resolve(out)
    }
    const onAbort = () => {
      out.interrupted = true
      out.exitCode = 124
      killAll()
      finish()
    }
    if (ctx.signal) {
      if (ctx.signal.aborted) {
        onAbort()
        return
      }
      ctx.signal.addEventListener("abort", onAbort, { once: true })
    }
    timer = setTimeout(() => {
      out.timedOut = true
      out.exitCode = 124
      killAll()
      finish()
    }, opts.timeoutMs)
    child.stdout.setEncoding("utf8")
    child.stdout.on("data", (chunk: string) => {
      stdoutBuf += chunk
      let idx: number
      while ((idx = stdoutBuf.indexOf("\n")) >= 0) {
        // 超长行（子进程侧已限 ~2MB，此处兜底）跳过解析并注明（防巨对象撑爆内存）
        if (idx > 2_500_000) {
          out.logs.push("（超长输出行已截断）")
          stdoutBuf = stdoutBuf.slice(idx + 1)
          continue
        }
        const line = stdoutBuf.slice(0, idx)
        stdoutBuf = stdoutBuf.slice(idx + 1)
        // 行级并发分发（脚本可 Promise.all 并行调用工具；应答带 id，顺序无关）
        void dispatchLine(line).catch((e) => out.logs.push(`(分发错误: ${(e as Error).message})`))
      }
    })
    let stderrTail = ""
    child.stderr.setEncoding("utf8")
    child.stderr.on("data", (d: string) => {
      stderrTail = `${stderrTail}${d}`.slice(-4000)
    })
    child.on("error", (err) => {
      out.spawnError = String(err)
      finish()
    })
    child.on("close", (code) => {
      // 超时/中断按 124 记（与 sh 子进程约定一致）；异常退出无 fail 消息时按 1
      out.exitCode = out.timedOut || out.interrupted ? 124 : (code ?? (out.error ? 1 : 0))
      if (stderrTail.trim()) {
        const note = out.error && stderrTail.includes(out.error.slice(0, 80)) ? "" : stderrTail.trim()
        if (note) out.logs.push(`[stderr] ${note}`)
      }
      finish()
    })
  })
}

export const jsTool: Tool = {
  name: "js",
  description:
    "执行 JS/TS 脚本（Bun 运行时，支持 TS/await/fetch/Bun API），可直接调用其他工具并注入会话上下文——**工具的动态编程**：完整语言能力（变量/函数/循环/条件/异常处理）编排工具链，表达 flow 声明式编排写不出的逻辑（复杂变换/动态参数/错误分支重试/跨步骤聚合）。\n" +
    "- **工具即内置函数**：`const r = await read({ path: \"a.txt\" })`（当前已启用的每个工具名都是一个可直接 await 的函数，无需前缀）；动态名字用 `await tools.call(name, params)` 或 `await tools.xxx(params)`。返回 `{ output, data, blocks, truncated, filePath }`（data 为结构化输出，结构可先用 tool_schemas 查询）；工具抛错 = Promise reject（可 try/catch 容错）。并行用 `Promise.all`；调用总数上限 100 次。\n" +
    "- **会话上下文**：`ctx` = `{ user, sessionId, workdir, home, sandboxed, env, projects, messages }`（messages 为最近会话消息快照）；flow/编排传入的 `input` 参数可直接引用（JSON 文本需自行 JSON.parse）。\n" +
    "- **输出与返回值**：console.log 输出即工具输出；脚本 `return` 的值进结构化 data.result（并附输出预览）。\n" +
    "- **运行时定义工具（defineTool）**：`await defineTool({ name, description, parameters, execute: async (args, ctx) => ({ output: \"...\" }) })`——与子Agent 工具同签名/同写法，把脚本能力固化为**会话内新工具**：注册后模型后续轮次可直接调用、脚本内也可像内置函数一样调用；execute 源码经序列化保存、每次调用在子进程执行（体内可用工具函数/ctx，须自包含不闭包外部变量）；重复劳动的逻辑（多轮要复用的加工/查询流程）写成 defineTool 而非每轮重贴整个脚本。`requiresApproval` 可选（**默认 true 需审批**——固化后的每次调用与 sh 同姿态，仅明确安全的只读/幂等工具显式传 false）。\n" +
    "- 其余同 sh：`timeout` 超时秒数（默认 300、上限 540，超时按进程树终止）、`strict`（true 时失败抛工具级错误，供 flow 中断编排）、`approval`（默认需审批，只读/幂等脚本可 false 免审）。\n" +
    "- 注意：import 语句不可用（代码包在函数体内），模块加载用 `await import(\"...\")`；写文件可用 write 工具或 Bun.write。\n" +
    "- 安全模式：自动降级为只读运行时——动态 import/eval/Function 等加载与代码执行通道拒绝，写文件/子进程/网络 API 屏蔽（仅保留文件读取；写文件用 write 工具、网络用 fetch_url）。",
  requiresApproval: (args) => args.approval !== false,
  card: { args: "code", codeField: "code", codeLang: "javascript" },
  parameters: {
    type: "object",
    properties: {
      code: { type: "string", description: "JS/TS 脚本源码（顶层 await 可用；return 返回值进 data.result）" },
      input: { description: "可选：任意输入（flow/编排传入），脚本内经 input 引用" },
      timeout: { type: "number", description: "可选：执行超时秒数（默认 300，上限 540）" },
      strict: { type: "boolean", description: "可选：true 时脚本失败抛工具级错误（flow 编排「失败即中断」）；默认 false 失败作为正常结果返回" },
      approval: { type: "boolean", description: "可选：本次调用是否需要用户审批（默认 true 需审批）；仅对明确安全的只读/幂等脚本可设 false" },
    },
    required: ["code"],
  },
  outputSchema: {
    type: "object",
    properties: {
      logs: { type: "string", description: "console 输出汇总（超长截断至 100k 字符）" },
      result: { description: "脚本返回值（return 的值，超长截断至 100k 字符）" },
      exitCode: { type: "integer", description: "退出码（0=成功）" },
      calls: {
        type: "array",
        description: "内部工具调用记录",
        items: {
          type: "object",
          properties: { name: { type: "string" }, ok: { type: "boolean" }, error: { type: "string" } },
          required: ["name", "ok"],
        },
      },
    },
    required: ["exitCode"],
  },
  async execute(args, ctx) {
    const userCode = String(args.code ?? "")
    if (!userCode.trim()) return { output: "js 拒绝：code 不能为空。" }
    // 嵌套守卫（一刀切）：经 js RPC 桥执行的任何工具（flow 步骤、编排工具等）再调 js 时携带 fromJsBridge
    // 标记——见标记即拒，封死 js→flow→js→… 交替递归与桥内失控子进程（直呼 js 已在 RPC 分发层拦截）
    if (ctx.fromJsBridge) {
      return { output: "js 拒绝：不能在经 js 桥调用的工具（如 flow 步骤）内嵌套执行 js（防递归子进程）。请把编排逻辑写进当前脚本，或在顶层会话直接调用 js。" }
    }
    // 安全模式：静态扫描（动态加载/字符串代码执行通道 shim 拦不住，前置拒绝）+ 子进程只读 shim
    if (ctx.safeMode) {
      const deny = scanJsReadOnly(userCode)
      if (deny) return { output: deny }
    }
    const scriptPath = join(ctx.workdir, `.gebai_js_${randomUUID().replace(/-/g, "")}.ts`)
    // 已启用工具名列表 → 生成内置函数声明（脚本内直接 await read(...) 调用）
    const toolNames = ctx.registry.schemas().map((s) => s.name)
    await writeFile(scriptPath, buildChildScript(userCode, { ctx: scriptSessionContext(ctx), input: args.input }, toolNames, "", ctx.safeMode === true))
    try {
      // approvalFree：免审运行的脚本体未经用户审阅，内部需审批工具在 RPC 分发层拦截
      const run = await runJsScript(scriptPath, ctx, { timeoutMs: scriptTimeoutMs(args.timeout), approvalFree: args.approval === false })
      const success = !run.error && !run.timedOut && !run.interrupted && !run.spawnError && run.exitCode === 0
      const parts: string[] = []
      if (run.logs.length) parts.push(run.logs.join("\n"))
      if (success && run.result != null) {
        const preview = JSON.stringify(run.result)
        parts.push(`[返回值] ${preview.length > JS_RESULT_PREVIEW_CHARS ? `${preview.slice(0, JS_RESULT_PREVIEW_CHARS)}…（已截断，完整值在 data.result）` : preview}`)
      }
      if (!success) {
        const notes: string[] = []
        if (run.error) notes.push(run.error)
        if (run.spawnError) notes.push(`子进程启动失败: ${run.spawnError}`)
        if (run.timedOut) notes.push(`[timed out after ${Math.round(scriptTimeoutMs(args.timeout) / 1000)}s]`)
        if (run.interrupted) notes.push("[interrupted]")
        if (run.exitCode !== 0 && !run.error) notes.push(`[exit ${run.exitCode}]`)
        parts.push(`${parts.length ? "\n" : ""}[脚本失败] ${notes.join("\n")}`)
      }
      let output = parts.join("\n")
      if (success && !run.logs.length && run.result == null) output = "（脚本执行成功，无输出）"
      const capData = (v: unknown): unknown => {
        const s = typeof v === "string" ? v : JSON.stringify(v ?? null)
        return s.length > JS_DATA_TEXT_CAP ? `${s.slice(0, JS_DATA_TEXT_CAP)}…` : v
      }
      if (args.strict === true && !success) {
        throw new Error(`脚本执行失败（exit ${run.exitCode}）${run.error ? `：\n${run.error.slice(0, 2000)}` : ""}`)
      }
      return {
        ...(await truncate(output, "js", ctx)),
        data: {
          exitCode: run.exitCode,
          logs: capData(run.logs.join("\n")) as string,
          result: capData(run.result),
          calls: run.calls,
          ...(run.timedOut ? { timedOut: true } : {}),
          ...(run.interrupted ? { interrupted: true } : {}),
        },
        // 内层工具 blocks 透传（去重限量）：js 编排图片/图表/文件类工具时产物块直达 UI/模型
        ...(run.blocks.length ? { blocks: run.blocks } : {}),
      }
    } finally {
      await rm(scriptPath, { force: true }).catch(() => {})
    }
  },
}

// ---------------------------------------------------------------------------
// 运行时工具定义（defineTool）：脚本以子Agent 同签名（execute(args, ctx)）定义新工具，
// 函数源码经 fn.toString() 序列化保存，后续调用在子进程中求值执行（与子Agent 工具同体验）
// ---------------------------------------------------------------------------

/** 动态工具名：与全局工具命名约定一致（小写开头，[a-z0-9_]，≤40 字符）。 */
export const DYNAMIC_TOOL_NAME_RE = /^[a-z][a-z0-9_]{0,39}$/

export interface DynamicToolDefInput extends DynamicToolDef {}

/** execute 源码形态归一化（fn.toString() 的三种产物都要能求值）：
 *  箭头函数 `async (args) => {}` 与具名函数 `async function fn() {}` 原样；
 *  方法简写 `async execute(args) {}`（与子Agent 示例同写法）无 function 关键字、不能作表达式，转为 `async function (args) {}`。 */
export function normalizeFnSource(src: string): string {
  const s = src.trim()
  if (/\bfunction\b/.test(s) || s.includes("=>")) return s
  const header = /^(async\s+)?[A-Za-z_$][A-Za-z0-9_$]*\s*\(/.exec(s)
  if (!header) return s
  return `${header[1] ?? ""}function (${s.slice(header[0].length)}`
}

/** 校验 defineTool 定义并构造 Tool 包装（注册与执行分离：引擎校验重名后注册，execute 才 spawn 子进程）。
 *  execute 源码须自包含（不闭包脚本局部变量——源码在全新子进程求值，体内可用 tools/内置函数/ctx/input）。 */
export function makeDynamicTool(def: DynamicToolDefInput): Tool {
  if (!DYNAMIC_TOOL_NAME_RE.test(def.name)) {
    throw new Error(`工具名 ${def.name || "(空)"} 非法：须匹配 [a-z][a-z0-9_] 且 ≤40 字符（与全局工具命名一致，小写开头）`)
  }
  if (!def.description.trim()) throw new Error("description 不能为空（工具选择依据）")
  if (def.description.length > 2000) throw new Error("description 超长（>2000 字符）")
  if (def.source.length > JS_DYNAMIC_SOURCE_CAP) throw new Error(`execute 源码超长（>${JS_DYNAMIC_SOURCE_CAP} 字符，源码随会话持久化）`)
  const src = def.source.trim()
  if (!src || !/(=>|\bfunction\b|^\s*async\s+[A-Za-z_$][A-Za-z0-9_$]*\s*\()/.test(src)) {
    throw new Error("execute 源码非法：须为函数（async (args, ctx) => ({ output }) 或 async function 形式，fn.toString() 序列化）")
  }
  const parameters = def.parameters && typeof def.parameters === "object" && (def.parameters.type === "object" || def.parameters.properties)
    ? def.parameters
    : { type: "object", properties: {} }
  const tool: Tool = {
    name: def.name,
    description: `${def.description}${def.description.endsWith("。") ? "" : "。"}（运行时定义工具：js 脚本内 defineTool 注册）`,
    parameters: parameters as unknown as Tool["parameters"],
    requiresApproval: def.requiresApproval !== false,
    runtimeDefined: true,
    async execute(args, ctx) {
      // 嵌套守卫说明：动态工具**不经** fromJsBridge 标记拒绝——depth 0（js 脚本内调用，含 defineTool 后
      // 同脚本立即调用）是合法路径，动态嵌套由 RPC 分发层 depth 守卫拦截（depth>0 拒调动态工具）；
      // js→flow→dyn 至多一层动态子进程（runJsScript depth:1），无递归通道；js 重入由 jsTool.execute 的
      // 标记拒绝封死。
      // 安全模式：动态工具与 js 同规则降级（源码静态扫描 + 子进程只读 shim），而非整体禁用——
      // 持久化的只读动态工具（数据处理/查询类）在安全模式下保持可用
      const safe = ctx.safeMode === true
      if (safe) {
        const deny = scanJsReadOnly(src)
        if (deny) throw new Error(deny)
      }
      const scriptPath = join(ctx.workdir, `.gebai_dt_${randomUUID().replace(/-/g, "").slice(0, 12)}.ts`)
      const toolNames = ctx.registry.schemas().map((s) => s.name)
      const script = buildChildScript(
        "return await __TOOL_FN(input, ctx)",
        { ctx: scriptSessionContext(ctx), input: args },
        toolNames,
        `var __TOOL_FN = (${normalizeFnSource(src)}\n)\n`,
        safe,
      )
      await writeFile(scriptPath, script)
      try {
        // approvalFree：免审动态工具（requiresApproval:false）的 execute 体未经审批执行，内部需审批工具在 RPC 分发层拦截
        const run = await runJsScript(scriptPath, ctx, { timeoutMs: scriptTimeoutMs(undefined), depth: 1, approvalFree: def.requiresApproval === false })
        if (run.error) throw new Error(`工具 ${def.name} 执行失败：\n${run.error.slice(0, 2000)}`)
        if (run.spawnError) throw new Error(`工具 ${def.name} 子进程启动失败: ${run.spawnError}`)
        if (run.timedOut) throw new Error(`工具 ${def.name} 执行超时（300 秒）已终止`)
        if (run.interrupted) throw new Error(`工具 ${def.name} 已取消（任务已停止）`)
        if (run.exitCode !== 0) {
          const diag = [run.error, ...run.logs].filter(Boolean).join("\n").slice(0, 2000)
          throw new Error(`工具 ${def.name} 执行失败（exit ${run.exitCode}）${diag ? `：\n${diag}` : ""}`)
        }
        // 返回值映射（子Agent 工具语义）：{output, data?} 原样；字符串 → output；其余 JSON 序列化
        const v = run.result
        if (v == null) return { output: `（工具 ${def.name} 执行成功，无返回值——execute 须 return { output: string }）` }
        if (typeof v === "object" && !Array.isArray(v) && typeof (v as { output?: unknown }).output === "string") {
          const r = v as { output: string; data?: unknown }
          return { ...(await truncate(r.output, def.name, ctx)), ...(r.data !== undefined ? { data: r.data } : {}) }
        }
        return { ...(await truncate(typeof v === "string" ? v : JSON.stringify(v), def.name, ctx)) }
      } finally {
        await rm(scriptPath, { force: true }).catch(() => {})
      }
    },
  }
  return tool
}
