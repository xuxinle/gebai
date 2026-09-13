/**
 * 文件工作台 · 终端面板门面：按服务端能力选实现。
 *
 * - PTY 可用（Windows + ConPTY 驱动）→ `terminal-pty.ts`：真伪控制台 + xterm.js；
 * - 不可用 → `terminal-legacy.ts`：管道式持久 shell + 行渲染（命令回显/提示符前端自绘）。
 *
 * 判定来自 `/api/v1/terminal/info` 的 `pty` 能力位（服务端探测驱动可用性，附中文原因）。
 * 选择在首次激活时做一次并缓存：同一页面生命周期内不来回切换实现（避免终端内容重建）。
 */
import { h } from "./ui"
import { createPtyTerminal } from "./terminal-pty"
import { createLegacyTerminalPanel, type TerminalHooks, type TerminalPanel } from "./terminal-legacy"

export type { TerminalHooks, TerminalPanel }

/** 本次页面会话的判定结果（null = 未判定）。 */
let ptyPreferred: boolean | null = null

function basePath(): string {
  return (import.meta.env.BASE_URL || "/").replace(/\/$/, "")
}

/** 读服务端能力位：失败/无响应时保守地用降级实现（终端仍要能用）。 */
async function detectPty(hooks: TerminalHooks): Promise<boolean> {
  if (ptyPreferred !== null) return ptyPreferred
  const url = new URL(`${basePath()}/api/v1/terminal/info`.replace(/\/{2,}/g, "/"), location.origin)
  const session = hooks.session()
  if (session) url.searchParams.set("session", session)
  const env = hooks.env()
  if (env && Object.keys(env).length) url.searchParams.set("env", JSON.stringify(env))
  try {
    const res = await fetch(url.pathname + url.search, { headers: authHeaders() })
    if (res.ok) {
      const body = (await res.json()) as { pty?: boolean }
      ptyPreferred = body.pty === true
    } else {
      ptyPreferred = false
    }
  } catch {
    ptyPreferred = false
  }
  return ptyPreferred
}

function authHeaders(): Record<string, string> {
  try {
    const t = localStorage.getItem("gebai.auth.token")
    return t ? { Authorization: `Bearer ${t}` } : {}
  } catch {
    return {}
  }
}

export function createTerminalPanel(hooks: TerminalHooks): TerminalPanel {
  const host = h("div", { class: "fw-term-host" })
  let inner: TerminalPanel | null = null
  let wantActive = false
  let picking: Promise<void> | null = null

  function ensure(): Promise<void> {
    if (inner) return Promise.resolve()
    if (picking) return picking
    picking = (async () => {
      const usePty = await detectPty(hooks)
      inner = usePty ? createPtyTerminal(hooks) : createLegacyTerminalPanel(hooks)
      host.replaceChildren(inner.el)
      if (wantActive) inner.activate()
    })()
    return picking
  }

  return {
    el: host,
    hasSession: () => inner?.hasSession() ?? false,
    activate() {
      wantActive = true
      void ensure().then(() => {
        if (wantActive) inner?.activate()
      })
    },
    deactivate() {
      wantActive = false
      inner?.deactivate()
    },
    onRootChanged() {
      inner?.onRootChanged()
    },
  }
}
