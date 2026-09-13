/** 文件工作台 · 终端域路由（DESIGN「文件工作台·终端」）：持久 shell 会话的创建/输入/增量读取/中断/关闭。
 *
 * 可用性三道闸：GEBAI_FS_ENABLED=true（否则全部 404）、GEBAI_TERMINAL=true（否则 info 回 enabled:false、
 * 其余 404）、用户不受沙箱约束（沙箱下 403——终端等同任意命令执行，多用户部署不开放）。
 * 只读环境（GEBAI_FS_WRITE=false）拒绝 create/input，info 仍回能力元信息 + 中文原因供前端只读展示。
 *
 * 端点契约（前端按此并行实现，字段不随意增删）：
 *   GET  /api/v1/terminal/info      → { enabled, reason?, sandboxed, writable, shells, defaultShell, maxSessions, idleMs }
 *   POST /api/v1/terminal/create    → { id, shell, shellName, cwd, root, cursor, output, startedAt }
 *   POST /api/v1/terminal/input     → { ok, cursor }
 *   GET  /api/v1/terminal/read      → { cursor, text, exits, alive }
 *   POST /api/v1/terminal/interrupt → { ok, cwd }
 *   POST /api/v1/terminal/close     → { ok }
 *   GET  /api/v1/terminal/list      → { sessions }
 */
import type { Context } from "hono"
import { mkdirSync, statSync } from "node:fs"
import type { RouteCtx } from "./context"
import { fsForbidden, resolveInRoot, resolveRoot } from "../core/fs/roots"
import { TERMINAL_IDLE_MS, TERMINAL_MAX_SESSIONS } from "../core/exec/term-session"
import { buildRootContext, errorResponse, parseEnvInput, pickBool, pickParam, requireFsEnabled } from "./fs-shared"

/** 终端未启用（GEBAI_TERMINAL=false 或服务未注入）。 */
const TERM_OFF = "终端能力未启用（GEBAI_TERMINAL=false）"
/** 只读检视环境：不允许执行命令。 */
const READ_ONLY = "文件工作台为只读模式（GEBAI_FS_WRITE=false）：不允许执行命令"
/** 沙箱非豁免用户：不开放终端。 */
const SANDBOX_DENIED = "沙箱模式下不开放终端（终端等同任意命令执行）：请使用本地模式或沙箱豁免用户"

export function registerTerminalRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  /** 终端是否启用（配置与服务实例双判：服务未注入视为未启用）。 */
  const termEnabled = () => d.config.terminalEnabled !== false && !!d.terminal
  /** 只读环境（GEBAI_FS_WRITE=false）。 */
  const readOnly = () => d.config.fsWrite === false

  /** 通用闸门：fs 关闭 404 → 沙箱非豁免 403 → 终端关闭 404（info 单列，见其处理器）。 */
  async function gate(c: Context): Promise<Response | null> {
    const off = requireFsEnabled(c, d)
    if (off) return off
    const user = await userOf(c)
    if (d.sandbox.enforcedFor(user.id)) return c.json({ error: SANDBOX_DENIED }, 403)
    if (!termEnabled()) return c.json({ error: TERM_OFF }, 404)
    return null
  }

  async function jsonBody(c: Context): Promise<Record<string, unknown>> {
    try {
      const b = await c.req.json()
      return b && typeof b === "object" ? (b as Record<string, unknown>) : {}
    } catch {
      return {}
    }
  }

  /** 命令执行留痕：终端等同任意命令执行，每条执行过的命令都进审计（被拒/失败的记 ok:false）。
   *  只记首行截断的摘要——多行粘贴不必整段入审计。 */
  function auditExec(user: string, c: Context, entry: { root: string; path?: string; cmd: string; shell: string; ok: boolean; error?: string }): void {
    d.fsAudit?.record({
      ts: Date.now(),
      user,
      source: "web",
      action: "term.exec",
      root: entry.root,
      path: entry.path,
      detail: { cmd: entry.cmd.split(/\r?\n/)[0].slice(0, 200), shell: entry.shell },
      ok: entry.ok,
      error: entry.error,
      ip: d.config.trustProxy ? c.req.header("x-forwarded-for") : undefined,
    })
  }

  /** cwd 目录保证存在（会话 tmp/ 这类目录并非必然创建，缺失目录 spawn 直接 ENOENT）。 */
  function ensureDir(abs: string): void {
    try {
      if (statSync(abs).isDirectory()) return
    } catch {
      /* 不存在：建之 */
    }
    try {
      mkdirSync(abs, { recursive: true })
    } catch {
      /* 尽力而为：失败按原样交给 shell 报错 */
    }
  }

  /** 能力与元信息：不可用状态仍回 200（前端据此展示中文原因与只读态，而不是当成网络错误）。 */
  app.get("/api/v1/terminal/info", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const user = await userOf(c)
      const sandboxed = d.sandbox.enforcedFor(user.id)
      if (sandboxed) return c.json({ error: SANDBOX_DENIED }, 403)
      const writable = !readOnly()
      const pty = d.terminalPty?.available()
      const meta = d.terminal?.info()
      const enabled = termEnabled() && writable
      const reason = !meta || d.config.terminalEnabled === false ? TERM_OFF : writable ? undefined : READ_ONLY
      return c.json({
        enabled,
        reason,
        sandboxed,
        writable,
        shells: meta?.shells ?? [],
        defaultShell: meta?.defaultShell ?? "",
        maxSessions: meta?.maxSessions ?? TERMINAL_MAX_SESSIONS,
        idleMs: meta?.idleMs ?? TERMINAL_IDLE_MS,
        // PTY（真伪控制台）能力位：可用时前端用 xterm + WS 交互式终端，否则降级管道式会话
        pty: !!pty?.ok,
        ptyReason: pty?.ok ? undefined : pty?.reason,
      })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** 创建会话：`root` 定位根（core/fs/roots.ts），`cwd` 为根内相对路径（越界 403）。 */
  app.post("/api/v1/terminal/create", async (c) => {
    const denied = await gate(c)
    if (denied) return denied
    try {
      const svc = d.terminal
      if (!svc) return c.json({ error: TERM_OFF }, 404)
      const user = await userOf(c)
      const body = await jsonBody(c)
      const envInput = c.req.query("env") ?? body.env
      const ctx = await buildRootContext(d, user, { sessionId: pickParam(c, "session", body) || undefined, envInput })
      const root = resolveRoot(pickParam(c, "root", body), ctx)
      if (!root.writable) throw fsForbidden(READ_ONLY)
      const cwdAbs = resolveInRoot(root.abs, pickParam(c, "cwd", body), { allowAbsolute: root.kind === "abs" })
      ensureDir(cwdAbs)
      const created = svc.create({
        rootId: root.id,
        rootAbs: root.abs,
        cwdAbs,
        shell: pickParam(c, "shell", body) || undefined,
        // 请求携带的环境变量（浏览器本地/会话内存态）注入 shell 进程：与 fs/git 端点同一份 env 口径
        env: parseEnvInput(envInput),
      })
      return c.json(created)
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** 写入会话 stdin：`exec` 缺省 true（命令 + 哨兵行），显式 false 时原样写（交互输入/控制字符）。 */
  app.post("/api/v1/terminal/input", async (c) => {
    const denied = await gate(c)
    if (denied) return denied
    try {
      const svc = d.terminal
      if (!svc) return c.json({ error: TERM_OFF }, 404)
      const user = await userOf(c)
      const body = await jsonBody(c)
      const id = pickParam(c, "id", body)
      const data = pickParam(c, "data", body)
      const execRaw = c.req.query("exec") ?? body.exec
      const exec = execRaw === undefined || execRaw === "" ? true : pickBool(c, "exec", body)
      if (readOnly()) {
        if (exec) auditExec(user.id, c, { root: "", cmd: data, shell: "", ok: false, error: READ_ONLY })
        throw fsForbidden(READ_ONLY)
      }
      try {
        const rec = svc.input({ id, data, exec })
        if (exec) auditExec(user.id, c, { root: rec.root, path: rec.cwd, cmd: data, shell: rec.shell, ok: true })
        return c.json({ ok: true, cursor: rec.cursor })
      } catch (err) {
        if (exec) auditExec(user.id, c, { root: "", cmd: data, shell: "", ok: false, error: err instanceof Error ? err.message : String(err) })
        throw err
      }
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** 增量读取：`since` 之后的输出（哨兵行已剥离）+ 新游标 + 事件 + 存活标记（会话不存在 404）。 */
  app.get("/api/v1/terminal/read", async (c) => {
    const denied = await gate(c)
    if (denied) return denied
    try {
      const svc = d.terminal
      if (!svc) return c.json({ error: TERM_OFF }, 404)
      const since = Number(c.req.query("since"))
      return c.json(svc.read(c.req.query("id") || "", Number.isFinite(since) ? since : 0))
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** 中断当前命令（Ctrl+C）：终止进程树并以原 cwd 重建 shell。 */
  app.post("/api/v1/terminal/interrupt", async (c) => {
    const denied = await gate(c)
    if (denied) return denied
    try {
      const svc = d.terminal
      if (!svc) return c.json({ error: TERM_OFF }, 404)
      const body = await jsonBody(c)
      return c.json(svc.interrupt(pickParam(c, "id", body)))
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.post("/api/v1/terminal/close", async (c) => {
    const denied = await gate(c)
    if (denied) return denied
    try {
      const svc = d.terminal
      if (!svc) return c.json({ error: TERM_OFF }, 404)
      const body = await jsonBody(c)
      return c.json(svc.close(pickParam(c, "id", body)))
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** 会话清单（重启页面后接管已有会话用）。 */
  app.get("/api/v1/terminal/list", async (c) => {
    const denied = await gate(c)
    if (denied) return denied
    try {
      const svc = d.terminal
      if (!svc) return c.json({ error: TERM_OFF }, 404)
      return c.json({ sessions: svc.list() })
    } catch (err) {
      return errorResponse(c, err)
    }
  })
}
