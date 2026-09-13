/**
 * `py` 脚本工具（**工具桥，仅本地模式**）：与 `js` 同体验的「工具即函数」编排能力——
 * 脚本内 `read({...})` / `tools.call(name, params)` / `tools.<工具名>(params)` 直接调用会话
 * 注册表中的任意工具，并注入会话上下文 `ctx` 与调用输入 `input`。
 *
 * ## 为什么协议不借用 stdio（本实现的核心取舍）
 *
 * Python 的输出路径分散（`print`/`logging`/C 扩展直写 fd 1/`os.write(1,…)`/子进程继承 fd 1），
 * stdio 无法像 Bun 的 `console.*` 那样被单点接管——沿用 js 的 stdio 行协议必然面对「用户输出混入
 * 协议流」，只能靠非协议行兜底解析（脆弱）。因此本桥**不占 stdout/stderr**：
 *
 * - 协议走**回环 TCP socket**（父进程 `net.createServer` 监听 `127.0.0.1:0` 随机端口 + 一次性
 *   token 认证，只接受首个连接、连接后立即关闭监听）；
 * - 子进程 stdout/stderr 完全归用户输出：`print`、fd 直写、子进程输出一律原样进 stdout
 *   （父进程按纯文本捕获，语义与旧 `runCommand` 路径一致），**不存在污染协议解析的问题**；
 * - stdin 同样不被占用（协议在 socket 上），`input` 参数以同名变量注入（与 js 对齐）。
 *
 * ## 门控与降级（fail-closed）
 *
 * - 桥**仅本地模式**启用：`!ctx.sandboxed && !ctx.safeMode && !bridgeLangs.includes("py")`。
 *   沙箱模式（服务端多用户部署）与安全模式（只读运行时承诺「仅保留文件读取」）不注入桥——
 *   走既有 `runCommand` 纯脚本路径；py 桥重入（链中已含 py）同样不注入（改纯脚本，链断）。
 *   另一种语言首次进入时照常注桥（py→js / js→py 一层混合编排合法）。
 * - socket 不可用（连接未在超时内建立 / 进程未能启动）时**不退回有污染风险的 stdio 桥**，
 *   而是降级为纯脚本执行并在输出首行说明。
 * - 不支持 `defineTool`（Python「源码序列化→新进程求值」不干净）；工具调用为**同步 API**
 *   （`ThreadPoolExecutor` 可并行调用，应答按 id 配对）。
 * - 审批：py 的 code 为任意代码、无法静态判定，`approval:false` 免审标记不生效（恒需审批）；
 *   默认审批**一次覆盖脚本内全部工具调用**（与 js 默认语义一致）。
 *
 * ## 分发层守卫
 *
 * 工具调用经 `core/exec/tool-bridge` 分发（与 js 共用同一套守卫：名称容错/同类桥重入/未知工具/
 * 必填参数/调用上限/安全模式硬阻断/免审拦截/结果封顶）；ctx 携带**脚本桥语言链**（`bridgeLangs`，
 * 逐层追加）——同类桥重入（js→…→js、py→…→py）在分发层**硬拒绝**（脚本侧表现为工具级异常）。
 */
import { spawn } from "node:child_process"
import type { ChildProcess } from "node:child_process"
import type { Socket } from "node:net"
import { randomUUID } from "node:crypto"
import { rm, writeFile } from "node:fs/promises"
import type { ContentBlock, SubSessionArchive } from "@gebai/sdk"
import type { Tool, ToolContext, ToolResult } from "../base/types"
import { PY_SAFE_BOOTSTRAP } from "../security/safety"
import { scriptTimeoutMs } from "../support/exec-opts"
import { truncate } from "../support/truncate"
import { scriptChildEnv, scriptSessionContext } from "./js-tool"
import {
  BRIDGE_FIELD_CAP,
  BRIDGE_TOOL_MAX_CALLS,
  collectBridgeBlocks,
  dispatchBridgeTool,
  type BridgeCallCounter,
} from "./tool-bridge"
import { schema } from "../tools/shared"

/** 单条桥协议行上限（防巨对象撑爆父进程内存；超长行丢弃留注）。 */
export const PY_PROTOCOL_LINE_CAP = 2_500_000
/** 桥连接建立超时（未建立即判定桥不可用 → 降级为纯脚本执行）。 */
export const PY_BRIDGE_CONNECT_TIMEOUT_MS = 8_000
/** stdout（用户输出）捕获上限：超出丢弃并留注（防巨量输出撑爆内存）。 */
export const PY_STDOUT_CAP = 2 * 1024 * 1024
/** stderr 保留尾部字符数（与 js 同口径的失败诊断窗口）。 */
export const PY_STDERR_TAIL = 8_000
/** 结构化 data 中 stdout/stderr/result 字段上限（与 sh/py/js 同口径）。 */
export const PY_DATA_TEXT_CAP = 100_000

/** Python 侧桥前导（模板字符串；`\\n` 为 Python 源中的换行转义，注意双层转义）。 */
function pyBridgePreamble(opts: { port: number; token: string; toolNamesJson: string; ctxJson: string; inputJson: string; userSrcJson: string }): string {
  return `# -*- coding: utf-8 -*-
# gebai py 工具桥前导（仅本地模式）：协议走回环 socket——stdout/stderr 完全归用户输出。
import sys as _g_sys, os as _g_os, json as _g_json, socket as _g_socket
import threading as _g_threading, queue as _g_queue, traceback as _g_traceback
import keyword as _g_keyword, builtins as _g_builtins

_G_PORT = ${opts.port}
_G_TOKEN = ${JSON.stringify(opts.token)}
# 注入数据一律经 json.loads 解析（JSON 字面量含 true/false/null，不是合法 Python 字面量）
_G_TOOL_NAMES = _g_json.loads(${opts.toolNamesJson})
_G_CTX = _g_json.loads(${opts.ctxJson})
_G_INPUT = _g_json.loads(${opts.inputJson})
_G_USER_SRC = ${opts.userSrcJson}
_G_FIELD_CAP = ${BRIDGE_FIELD_CAP}

def _g_fatal(_msg):
    _g_sys.stderr.write("[gebai-py-bridge] " + str(_msg) + "\\n")
    _g_sys.stderr.flush()
    _g_sys.exit(3)

class _G_ToolError(RuntimeError):
    """工具调用失败（脚本内可 try/except 捕获后继续）。"""

class _G_Result(dict):
    """工具调用返回值：字典语义 + 属性访问（r["output"] 与 r.output 等价）。"""
    def __getattr__(self, _name):
        try:
            return self[_name]
        except KeyError:
            raise AttributeError(_name)

_g_send_lock = _g_threading.Lock()
_g_pending = {}
_g_pending_lock = _g_threading.Lock()
_g_seq = [0]

try:
    _g_sock = _g_socket.create_connection(("127.0.0.1", _G_PORT), timeout=10)
    _g_sock.settimeout(None)
except Exception as _g_e:
    _g_fatal("脚本桥连接失败: %s" % (_g_e,))

def _g_send(_obj):
    try:
        _line = _g_json.dumps(_obj, ensure_ascii=False, default=str)
    except Exception as _g_e:
        _line = _g_json.dumps({"t": "fail", "error": "消息序列化失败: %s" % (_g_e,)})
    with _g_send_lock:
        _g_sock.sendall((_line + "\\n").encode("utf-8"))

def _g_reader():
    """应答读线程：按 id 派发到各调用方的队列（支持线程池并行调用）。"""
    _buf = b""
    while True:
        try:
            _chunk = _g_sock.recv(65536)
        except Exception:
            _chunk = b""
        if not _chunk:
            break
        _buf += _chunk
        while True:
            _i = _buf.find(b"\\n")
            if _i < 0:
                break
            _raw = _buf[:_i]
            _buf = _buf[_i + 1:]
            try:
                _msg = _g_json.loads(_raw.decode("utf-8", "replace"))
            except Exception:
                continue
            if not isinstance(_msg, dict) or _msg.get("t") != "res":
                continue
            with _g_pending_lock:
                _q = _g_pending.pop(_msg.get("id"), None)
            if _q is not None:
                _q.put(_msg)
    with _g_pending_lock:
        _left = list(_g_pending.values())
        _g_pending.clear()
    for _q in _left:
        _q.put({"t": "res", "ok": False, "error": "脚本桥连接已断开（本次运行已结束）"})

_g_threading.Thread(target=_g_reader, daemon=True).start()
_g_send({"t": "auth", "token": _G_TOKEN})

def _g_call(_name, _params=None):
    _g_seq[0] += 1
    _id = _g_seq[0]
    _q = _g_queue.Queue(maxsize=1)
    with _g_pending_lock:
        _g_pending[_id] = _q
    _g_send({"t": "call", "id": _id, "name": str(_name), "params": {} if _params is None else _params})
    _msg = _q.get()
    if not _msg.get("ok"):
        raise _G_ToolError(_msg.get("error") or ("工具 %s 执行失败" % (_name,)))
    _res = _msg.get("result")
    return _G_Result(_res) if isinstance(_res, dict) else _res

class _G_Tools(object):
    """动态调用面：tools.call(name, params) 与 tools.<工具名>(params)。"""
    def call(self, name, params=None):
        return _g_call(name, params)

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return lambda params=None: _g_call(name, params)

_g_ns = {"__name__": "__main__", "__builtins__": _g_builtins}
_g_ns["tools"] = _G_Tools()
_g_ctx = _G_CTX
_g_ctx["env"] = dict(_g_os.environ)
_g_ns["ctx"] = _g_ctx
_g_ns["input"] = _G_INPUT
for _g_name in _G_TOOL_NAMES:
    if not isinstance(_g_name, str) or not _g_name.isidentifier():
        continue
    if _g_keyword.iskeyword(_g_name) or hasattr(_g_builtins, _g_name):
        continue
    if _g_name in ("tools", "ctx", "input", "result"):
        continue
    _g_ns[_g_name] = (lambda _n: (lambda params=None: _g_call(_n, params)))(_g_name)

# 异常类注入用户命名空间：工具描述承诺「工具失败抛 _G_ToolError（可 try/except 容错继续）」；
# 用户代码的 globals 就是 _g_ns（模块全局不在 exec 的解析链上），不注入则 except _G_ToolError 直接 NameError。
_g_ns["_G_ToolError"] = _G_ToolError

_g_exit = 0
try:
    exec(compile(_G_USER_SRC, "<gebai-py>", "exec"), _g_ns)
except SystemExit as _g_se:
    _g_code = _g_se.code
    if _g_code is None or _g_code is True or (isinstance(_g_code, int) and _g_code == 0):
        _g_exit = 0
    else:
        _g_exit = _g_code if isinstance(_g_code, int) else 1
        _g_send({"t": "fail", "error": "脚本以 SystemExit(%r) 结束" % (_g_se.code,)})
except BaseException:
    _g_exit = 1
    _g_send({"t": "fail", "error": _g_traceback.format_exc()})

if _g_exit == 0:
    _g_val = _g_ns.get("result", None)
    if _g_val is not None:
        try:
            _g_val = _g_json.loads(_g_json.dumps(_g_val, ensure_ascii=False, default=str))
        except Exception:
            _g_val = "<result 无法 JSON 序列化（%s）>" % (type(_g_val).__name__,)
    _g_send({"t": "done", "value": _g_val})

# 优雅关闭：先 shutdown 再 close——Windows 上 close 时若有 pending recv 会触发 RST，
# 对端可能丢弃尚未派发的 done 消息（表现为「脚本返回值丢失」）
try:
    _g_sock.shutdown(_g_socket.SHUT_RDWR)
except Exception:
    pass
try:
    _g_sock.close()
except Exception:
    pass
_g_sys.exit(_g_exit)
`
}

/** 生成完整桥脚本：前导 + 常量注入（ctx 不落盘 env——与 js 同规则，子进程内引用 `os.environ`）。 */
export function buildPyBridgeScript(opts: {
  port: number
  token: string
  toolNames: string[]
  ctx: Record<string, unknown>
  input: unknown
  userCode: string
}): string {
  const ctxObj: Record<string, unknown> = { ...(opts.ctx ?? {}) }
  delete ctxObj.env
  // 双重 stringify：生成 Python 字符串字面量（内层 JSON 文本由 Python 侧 json.loads 解析）
  return pyBridgePreamble({
    port: opts.port,
    token: opts.token,
    toolNamesJson: JSON.stringify(JSON.stringify(opts.toolNames)),
    ctxJson: JSON.stringify(JSON.stringify(ctxObj)),
    inputJson: JSON.stringify(JSON.stringify(opts.input ?? null)),
    userSrcJson: JSON.stringify(opts.userCode),
  })
}

/** 输出中返回值预览保留字符数（完整值在 data.result；与 js 同口径）。 */
export const PY_RESULT_PREVIEW_CHARS = 2000

interface PyRunResult {
  exitCode: number
  stdout: string
  stderr: string
  result: unknown
  calls: Array<{ name: string; ok: boolean; error?: string }>
  blocks: ContentBlock[]
  subSessionArchive?: SubSessionArchive
  error?: string
  timedOut: boolean
  interrupted: boolean
  spawnError?: string
  /** 桥连接未在超时内建立 / 监听失败：调用方据此降级为纯脚本执行（绝不退回 stdio 桥）。 */
  bridgeUnavailable?: boolean
}

function pyCwdNote(ctx: ToolContext): string {
  return ctx.workdir !== (ctx.sessionWorkdir ?? ctx.workdir) ? `\n（工作目录: ${ctx.workdir}）` : ""
}

function capText(s: string): string {
  return s.length > PY_DATA_TEXT_CAP ? s.slice(0, PY_DATA_TEXT_CAP) : s
}

/** 旧路径（沙箱/安全模式/经 js 桥调用、以及桥不可用降级）的纯脚本执行：临时文件 + `runCommand`，
 *  语义与历史实现一致（stdout 即输出、stderr 并入失败诊断、工作目录注记）。 */
async function runLegacyPy(code: string, input: string | undefined, timeoutMs: number, ctx: ToolContext): Promise<ToolResult> {
  // 安全模式：前置审计钩子引导段（sys.addaudithook 拦写模式 open 与进程/网络/文件变更系统调用，仅保留文件读取）
  const finalCode = ctx.safeMode ? `${PY_SAFE_BOOTSTRAP}\n${code}` : code
  const scriptPath = `${ctx.workdir}/.gebai_py_${randomUUID().replace(/-/g, "")}.py`
  await writeFile(scriptPath, finalCode)
  try {
    // -X utf8 / PYTHONUTF8=1：强制 UTF-8 输出（Windows 默认 GBK 会造成乱码）
    const py = await resolvePythonCmd(ctx)
    const { stdout, stderr, code: exit } = await ctx.runCommand(`${py} -X utf8 "${scriptPath}"`, {
      workdir: ctx.workdir,
      env: { ...ctx.env, PYTHONUTF8: "1" },
      input,
      timeoutMs,
    })
    const out = exit === 0 ? stdout : `${stdout}\n${stderr}\n[exit ${exit}]`
    const cwdNote = pyCwdNote(ctx)
    const final = exit === 0 && !stdout.trim() ? `（程序执行成功，无输出）${cwdNote}` : out + cwdNote
    return { ...(await truncate(final, "py", ctx)), data: { stdout: capText(stdout), stderr: capText(stderr), exitCode: exit } }
  } finally {
    await rm(scriptPath, { force: true }).catch(() => {})
  }
}

/** 桥路径执行：起回环监听 → 写桥脚本 → spawn python → 行协议分发工具调用 → 收集输出/返回值。 */
export async function runPythonBridge(
  ctx: ToolContext,
  opts: { userCode: string; input?: unknown; timeoutMs: number },
): Promise<PyRunResult> {
  const net = await import("node:net")
  const out: PyRunResult = { exitCode: 0, stdout: "", stderr: "", result: null, calls: [], blocks: [], timedOut: false, interrupted: false }
  const seenBlocks = new Set<string>()
  const counter: BridgeCallCounter = { n: 0, max: BRIDGE_TOOL_MAX_CALLS }
  const token = randomUUID().replace(/-/g, "")
  const server = net.createServer()
  try {
    await new Promise<void>((res, rej) => {
      server.once("error", (e) => rej(e))
      server.listen(0, "127.0.0.1", () => res())
    })
  } catch (e) {
    return { ...out, bridgeUnavailable: true, spawnError: `桥监听失败: ${(e as Error).message}` }
  }
  const port = (server.address() as { port: number }).port
  const toolNames = ctx.registry.schemas().map((s) => s.name)
  const script = buildPyBridgeScript({ port, token, toolNames, ctx: scriptSessionContext(ctx), input: opts.input, userCode: opts.userCode })
  const scriptPath = `${ctx.workdir}/.gebai_py_${randomUUID().replace(/-/g, "")}.py`
  await writeFile(scriptPath, script)
  const pythonCmd = await resolvePythonCmd(ctx)
  const env = { ...scriptChildEnv(ctx), PYTHONUTF8: "1" }
  const isWin = process.platform === "win32"

  return new Promise<PyRunResult>((resolve) => {
    let child: ChildProcess | undefined
    let conn: Socket | undefined
    let connEnded = false
    let lastDataAt = 0
    let inFlight = 0
    let settled = false
    let connected = false
    let timer: ReturnType<typeof setTimeout> | undefined
    let connectTimer: ReturnType<typeof setTimeout> | undefined

    const killAll = () => {
      try {
        if (!child?.pid) return
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
    const finish = () => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      clearTimeout(connectTimer)
      ctx.signal?.removeEventListener("abort", onAbort)
      try {
        server.close()
      } catch {
        /* 已关闭 */
      }
      void rm(scriptPath, { force: true }).catch(() => {})
      resolve(out)
    }
    /** 收尾排空：等 in-flight 工具调用结束与 socket 尾部数据——子进程退出（child close 事件）与 socket
     *  data 事件的派发顺序不保证，直接收尾会丢掉 done 消息（表现为「脚本返回值丢失」）。 */
    const finishAfterDrain = () => {
      const started = Date.now()
      const tick = () => {
        if (settled) return
        const idle = Date.now() - lastDataAt
        // 已连接则等 socket 数据安静下来（最后一帧数据与 close 事件可能同批到达，提前收尾会丢 done 消息）
        if (inFlight === 0 && (!conn || (connEnded && idle > 60)) && idle > 60) {
          finish()
          return
        }
        if (Date.now() - started > 500) {
          finish()
          return
        }
        setTimeout(tick, 15)
      }
      tick()
    }
    const onAbort = () => {
      out.interrupted = true
      out.exitCode = 124
      killAll()
      finish()
    }

    // 连接：仅接受首个连接 + token 认证（认证失败即断开），之后关闭监听
    server.on("connection", (sock) => {
      if (connected) {
        sock.destroy()
        return
      }
      connected = true
      conn = sock
      clearTimeout(connectTimer)
      try {
        server.close()
      } catch {
        /* 已关闭 */
      }
      sock.setEncoding("utf8")
      let buf = ""
      let authed = false
      const send = (obj: unknown) => {
        try {
          sock.write(`${JSON.stringify(obj)}\n`)
        } catch {
          /* 子进程已退出 */
        }
      }
      const handle = async (msg: { t?: string; id?: number; name?: unknown; params?: unknown; value?: unknown; error?: string }): Promise<void> => {
        if (msg.t === "call") {
          const reply = await dispatchBridgeTool({ name: msg.name, params: msg.params }, ctx, { script: "py", counter })
          if (reply.ok) {
            out.calls.push({ name: reply.name, ok: true })
            collectBridgeBlocks(seenBlocks, out.blocks, reply.result.blocks)
            if (reply.result.subSessionArchive) out.subSessionArchive = reply.result.subSessionArchive
            send({ t: "res", id: msg.id, ok: true, result: reply.result })
          } else {
            out.calls.push({ name: reply.name, ok: false, error: reply.error })
            send({ t: "res", id: msg.id, ok: false, error: reply.error })
          }
          return
        }
        if (msg.t === "done") {
          out.result = msg.value ?? null
          return
        }
        if (msg.t === "fail") out.error = String(msg.error ?? "脚本执行失败")
      }
      sock.on("data", (d: string) => {
        lastDataAt = Date.now()
        buf += d
        let i: number
        while ((i = buf.indexOf("\n")) >= 0) {
          const line = buf.slice(0, i)
          buf = buf.slice(i + 1)
          if (!line.trim()) continue
          if (line.length > PY_PROTOCOL_LINE_CAP) continue // 超长行丢弃留注（防巨对象撑爆内存）
          let msg: { t?: string; id?: number; name?: unknown; params?: unknown; token?: string; value?: unknown; error?: string }
          try {
            msg = JSON.parse(line) as typeof msg
          } catch {
            continue
          }
          if (!authed) {
            if (msg.t !== "auth" || String(msg.token ?? "") !== token) {
              sock.destroy()
              return
            }
            authed = true
            send({ t: "auth_res", ok: true })
            continue
          }
          inFlight += 1
          void handle(msg)
            .catch(() => {})
            .finally(() => {
              inFlight -= 1
            })
        }
      })
      sock.on("end", () => {
        connEnded = true
      })
      sock.on("close", () => {
        connEnded = true
      })
      sock.on("error", () => {
        connEnded = true
      })
    })

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
    connectTimer = setTimeout(() => {
      if (connected) return
      out.bridgeUnavailable = true
      killAll()
      finish()
    }, PY_BRIDGE_CONNECT_TIMEOUT_MS)

    try {
      child = spawn(pythonCmd, ["-X", "utf8", "-u", scriptPath], {
        cwd: ctx.workdir,
        env,
        shell: false,
        stdio: ["ignore", "pipe", "pipe"],
        detached: !isWin,
      })
    } catch (e) {
      out.spawnError = String(e)
      finish()
      return
    }
    child.stdout?.setEncoding("utf8")
    child.stdout?.on("data", (d: string) => {
      const room = PY_STDOUT_CAP - out.stdout.length
      if (room <= 0) return
      if (d.length <= room) {
        out.stdout += d
        return
      }
      out.stdout += `${d.slice(0, room)}\n（输出超 ${PY_STDOUT_CAP} 字符上限，超出部分已丢弃）`
    })
    child.stderr?.setEncoding("utf8")
    child.stderr?.on("data", (d: string) => {
      out.stderr = `${out.stderr}${d}`.slice(-PY_STDERR_TAIL)
    })
    child.on("error", (err) => {
      // 子进程未能启动（解释器不可用等）：桥通道未建立 → 标记不可用，调用方降级为纯脚本执行
      out.spawnError = String(err)
      out.bridgeUnavailable = true
      finish()
    })
    child.on("close", (code) => {
      // 未建立桥连接即退出（解释器不可用/脚本无法启动等）：标记桥不可用，调用方降级为纯脚本执行取得真实诊断
      if (!connected && !out.timedOut && !out.interrupted) out.bridgeUnavailable = true
      // 超时/中断按 124 记（与 sh/js 子进程约定一致）；异常退出无 fail 消息时按 1
      out.exitCode = out.timedOut || out.interrupted ? 124 : (code ?? (out.error ? 1 : 0))
      // 未连接（桥启动失败）或已由超时/中断收尾：无需排空
      if (!connected || out.timedOut || out.interrupted) {
        finish()
        return
      }
      finishAfterDrain()
    })
  })
}

/** 桥路径结果 → 工具结果（output：用户输出 + 返回值预览 + 失败诊断；data：stdout/stderr/exitCode/result/calls）。 */
async function formatBridgeRun(run: PyRunResult, timeoutMs: number, ctx: ToolContext): Promise<ToolResult> {
  const success = !run.error && !run.timedOut && !run.interrupted && !run.spawnError && run.exitCode === 0
  const parts: string[] = []
  const stdoutText = run.stdout.replace(/\s+$/, "")
  if (stdoutText) parts.push(stdoutText)
  if (success && run.result != null) {
    const preview = JSON.stringify(run.result)
    parts.push(`[返回值] ${preview.length > PY_RESULT_PREVIEW_CHARS ? `${preview.slice(0, PY_RESULT_PREVIEW_CHARS)}…（已截断，完整值在 data.result）` : preview}`)
  }
  if (!success) {
    const notes: string[] = []
    if (run.error) notes.push(run.error)
    if (run.spawnError) notes.push(`子进程启动失败: ${run.spawnError}`)
    if (run.timedOut) notes.push(`[timed out after ${Math.round(timeoutMs / 1000)}s]`)
    if (run.interrupted) notes.push("[interrupted]")
    if (run.exitCode !== 0 && !run.error) notes.push(`[exit ${run.exitCode}]`)
    if (run.stderr.trim()) notes.push(run.stderr.trim())
    parts.push(`${parts.length ? "\n" : ""}[脚本失败] ${notes.join("\n")}`)
  } else if (run.stderr.trim()) {
    parts.push(`[stderr] ${run.stderr.trim()}`)
  }
  let output = parts.length ? parts.join("\n") : "（脚本执行成功，无输出）"
  output += pyCwdNote(ctx)
  return {
    ...(await truncate(output, "py", ctx)),
    data: {
      stdout: capText(run.stdout),
      stderr: capText(run.stderr),
      exitCode: run.exitCode,
      ...(run.result != null ? { result: run.result } : {}),
      ...(run.calls.length ? { calls: run.calls } : {}),
      ...(run.timedOut ? { timedOut: true } : {}),
      ...(run.interrupted ? { interrupted: true } : {}),
    },
    ...(run.blocks.length ? { blocks: run.blocks } : {}),
    ...(run.subSessionArchive ? { subSessionArchive: run.subSessionArchive } : {}),
  }
}

/** py 的 stdin 输入序列化（旧路径用；对象/数组转 JSON 文本，其余按字符串）。 */
function pyScriptInput(v: unknown): string | undefined {
  if (v == null) return undefined
  if (typeof v === "object") return JSON.stringify(v)
  return String(v)
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
  return pythonCmdCache
}

/** py 工具输出 schema：sh/py 的 `{stdout, stderr, exitCode}` 基础上，本地模式工具桥路径附带
 *  `result`（顶层变量 `result`）与 `calls`（内部工具调用记录）。 */
const pyOutputSchema = schema(
  {
    stdout: { type: "string", description: "标准输出（超长截断至 100k 字符）" },
    stderr: { type: "string", description: "标准错误（超长截断至 100k 字符）" },
    exitCode: { type: "integer", description: "退出码（0=成功）" },
    result: { description: "脚本返回值（顶层变量 `result`；仅本地模式工具桥路径）" },
    calls: {
      type: "array",
      description: "内部工具调用记录（仅本地模式工具桥路径）",
      items: { type: "object", properties: { name: { type: "string" }, ok: { type: "boolean" }, error: { type: "string" } }, required: ["name", "ok"] },
    },
  },
  ["stdout", "stderr", "exitCode"],
)

export const pyTool: Tool = {
  name: "py",
  description:
    "执行 Python 代码（经临时文件），stdout 为输出。**本地模式下代码可调用其他工具并注入会话上下文（工具桥）**：已启用的工具名即函数——`r = read({\"path\": \"a.txt\"})`，返回 dict（`r[\"output\"]` / `r.output` 等价，含 data/blocks/truncated；**属性访问仅作用于顶层**——嵌套字段按下标，如 `r[\"data\"][\"exitCode\"]`）；动态名字用 `tools.call(name, params)` 或 `tools.<工具名>(params)`；工具调用为同步 API（`ThreadPoolExecutor` 可并行，应答按 id 配对）；工具失败抛 `_G_ToolError`（可 try/except 容错继续）。脚本设置顶层变量 `result = ...` 即作为结构化返回值（进 data.result）；注入 `ctx`（user/sessionId/workdir/home/sandboxed/env/projects/messages）与 `input`（调用入参，对象/数组原样注入为 dict/list，与 js 侧一致；注意该变量遮蔽内建 `input()`）。\n" +
    "- **仅本地模式**：沙箱模式（服务端部署）与安全模式（只读运行时：审计钩子屏蔽写/进程/网络，仅保留文件读取）下不注入工具桥，纯脚本执行。不支持 defineTool。\n" +
    "- 协议走回环 socket，stdout/stderr 完全归脚本输出（`print`/fd 直写/子进程输出均照常进 stdout），stdin 不被占用。\n" +
    "- 嵌套：py 桥不可重入（py 内不能再调 py，硬拒抛 `_G_ToolError`）；可调 js（首次进入）；JS 已在本链中（如 `js→py→js`）时同样硬拒。\n" +
    "- 审批：`code` 为任意代码、无法静态判定安全性，`approval:false` 免审标记不生效（恒需审批）；默认审批一次覆盖脚本内全部工具调用。",
  // py 恒需审批（任意代码执行面，无可静态判定的免审形态）；返回函数形态以保持 requiresApproval 语义可被引擎/dispatch 层解析
  requiresApproval: () => true,
  card: { args: "code", codeField: "code", codeLang: "python" },
  parameters: schema(
    {
      code: { type: "string", description: "Python 程序源码（本地模式下可用工具桥：工具名即函数、tools.call、ctx/input 注入；`result = ...` 作为返回值）" },
      input: { description: "可选：任意输入，脚本内经 `input` 引用（本地模式工具桥下对象/数组原样注入为 dict/list，与 js 一致；纯脚本降级路径按 JSON 文本走 stdin）" },
      timeout: { type: "number", description: "可选：执行超时秒数（默认 300，上限 540；超时进程被终止并返回超时结果）" },
      strict: { type: "boolean", description: "可选：true 时退出码非 0 抛工具级错误（js 编排「非 0 即中断」语义）；默认 false 非 0 退出作为正常结果返回" },
      approval: { type: "boolean", description: "兼容参数：py 的 code 为任意代码、无法静态判定安全性，免审标记不生效（默认且恒需审批）" },
    },
    ["code"],
  ),
  outputSchema: pyOutputSchema,
  async execute(args, ctx) {
    const code = String(args.code ?? "")
    if (!code.trim()) return { output: "py 拒绝：code 不能为空。" }
    const timeoutMs = scriptTimeoutMs(args.timeout)
    // 门控：桥仅本地模式、且本语言未在链中重入（`bridgeLangs` 已含 py = 经 py 桥再次进入，
    // 此时不注桥改纯脚本执行；沙箱/安全模式同理走纯脚本路径）。
    // 另一种语言首次进入时照常注桥（py→js / js→py 一层混合编排合法，重入由链封死）。
    const chain = ctx.bridgeLangs ?? []
    const useBridge = !ctx.sandboxed && ctx.safeMode !== true && !chain.includes("py")
    let result: ToolResult
    if (useBridge) {
      const run = await runPythonBridge(ctx, { userCode: code, input: args.input, timeoutMs })
      if (run.bridgeUnavailable) {
        // 降级（fail-closed）：桥不可用时不退回 stdio 桥，改纯脚本执行并说明
        const legacy = await runLegacyPy(code, pyScriptInput(args.input), timeoutMs, ctx)
        result = { ...legacy, output: `（脚本桥不可用，本次降级为纯脚本执行：无工具调用与 ctx 注入）\n${legacy.output}` }
      } else {
        result = await formatBridgeRun(run, timeoutMs, ctx)
      }
    } else {
      result = await runLegacyPy(code, pyScriptInput(args.input), timeoutMs, ctx)
    }
    const data = result.data as { exitCode?: number; stderr?: string } | undefined
    const exit = data?.exitCode
    // strict：非 0 退出码转工具级异常（js 编排内未捕获即中断整个脚本，try/catch 可容错继续）
    if (args.strict === true && exit !== 0) {
      const diag = (data?.stderr?.trim() ? data.stderr : String(result.output ?? "")).slice(0, 2000)
      throw new Error(`程序执行失败（exit ${exit}）${diag ? `：\n${diag}` : ""}`)
    }
    return result
  },
}
