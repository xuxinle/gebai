/**
 * WS 语言服务器域测试：门禁（fs 开关 / GEBAI_LSP / 沙箱 / 服务未注入）与消息契约。
 * 不起真实语言服务器——门禁拒绝与「无可用服务器」都在到达进程层之前返回。
 */
import { describe, expect, test } from "bun:test"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { SERVICE_USER, type AppDeps } from "../app"
import type { ServerConfig } from "../core/base/config"
import { LspService } from "../core/lsp/service"
import { handleWsMessage, type WsConn, type WsSink } from "../ws"

function makeDeps(opts: { config?: Partial<ServerConfig>; sandboxed?: boolean; lsp?: LspService | null } = {}): AppDeps {
  const config = {
    auth: "local",
    gebaiHome: join(tmpdir(), "gebai-lsp-ws-home"),
    fsEnabled: true,
    fsWrite: true,
    lspEnabled: true,
    ...opts.config,
  } as unknown as ServerConfig
  const deps = {
    config,
    auth: { defaultUser: () => SERVICE_USER },
    sandbox: { enforcedFor: () => opts.sandboxed === true, isExempt: () => false },
    engine: { workbenchProjects: () => [] },
    env: { resolve: async () => ({}) },
    store: { getEnv: async () => ({}), listSessionInfos: async () => [] },
  } as unknown as AppDeps
  if (opts.lsp !== null) deps.lsp = opts.lsp ?? new LspService({ which: () => null })
  return deps
}

interface Sent {
  type: string
  id?: string
  ok?: boolean
  payload?: Record<string, unknown>
  error?: string
}

/** 假连接：把回复收进数组；conn 恒为本地默认用户。 */
function fakeConn(): { ws: WsSink; sent: Sent[]; conn: WsConn } {
  const sent: Sent[] = []
  return {
    sent,
    ws: { send: (data: string) => sent.push(JSON.parse(data) as Sent) },
    conn: { get: () => SERVICE_USER, set: () => {}, getCurrent: () => undefined, setCurrent: () => {} },
  }
}

async function call(deps: AppDeps, type: string, payload: Record<string, unknown> = {}) {
  const { ws, sent, conn } = fakeConn()
  await handleWsMessage(deps, ws, { type, id: "r1", payload }, conn)
  return sent[0] as Sent
}

describe("WS 语言服务器域：门禁", () => {
  test("文件工作台关闭：拒绝", async () => {
    const reply = await call(makeDeps({ config: { fsEnabled: false } }), "lsp.open", { root: "user:", path: "a.rs", language: "rust" })
    expect(reply.ok).toBe(false)
    expect(reply.error).toContain("文件工作台未启用")
  })

  test("GEBAI_LSP=false：拒绝", async () => {
    const reply = await call(makeDeps({ config: { lspEnabled: false } }), "lsp.open", { root: "user:", path: "a.rs", language: "rust" })
    expect(reply.ok).toBe(false)
    expect(reply.error).toContain("GEBAI_LSP")
  })

  test("服务未注入：拒绝", async () => {
    const reply = await call(makeDeps({ lsp: null }), "lsp.request", { docId: "d1", method: "textDocument/hover" })
    expect(reply.ok).toBe(false)
    expect(reply.error).toContain("语言服务器未启用")
  })

  test("沙箱非豁免用户：拒绝（不开放常驻子进程）", async () => {
    const reply = await call(makeDeps({ sandboxed: true }), "lsp.open", { root: "user:", path: "a.rs", language: "rust" })
    expect(reply.ok).toBe(false)
    expect(reply.error).toContain("沙箱")
  })

  test("只读环境（GEBAI_FS_WRITE=false）**不**拦 LSP（只读语义信息）", async () => {
    const reply = await call(makeDeps({ config: { fsWrite: false }, lsp: new LspService({ which: () => null }) }), "lsp.open", {
      root: "user:",
      path: "a.rs",
      language: "rust",
    })
    // 门禁放行：语言没有可用服务器 → 正常路径返回 available:false，而不是权限拒绝
    expect(reply.ok).toBe(true)
    expect(reply.payload?.available).toBe(false)
  })
})

describe("WS 语言服务器域：消息契约", () => {
  test("没有可用服务器：ok + available:false（正常降级，不是错误）", async () => {
    const reply = await call(makeDeps({ lsp: new LspService({ which: () => null }) }), "lsp.open", { root: "user:", path: "a.rs", language: "rust" })
    expect(reply.ok).toBe(true)
    expect(reply.payload?.available).toBe(false)
    expect(String(reply.payload?.reason)).toContain("rust")
  })

  test("plaintext / 空语言直接回 available:false，不去探测也不建会话", async () => {
    const deps = makeDeps({ lsp: new LspService({ which: () => "/usr/bin/whatever" }) })
    for (const language of ["", "plaintext"]) {
      const reply = await call(deps, "lsp.open", { root: "user:", path: "a.txt", language })
      expect(reply.ok).toBe(true)
      expect(reply.payload?.available).toBe(false)
    }
  })

  test("lsp.request 缺 method：显式报错", async () => {
    const reply = await call(makeDeps(), "lsp.request", { docId: "d1" })
    expect(reply.ok).toBe(false)
    expect(reply.error).toContain("method")
  })

  test("lsp.request 指向未打开的文档：显式报错（不静默）", async () => {
    const reply = await call(makeDeps(), "lsp.request", { docId: "nope", method: "textDocument/hover" })
    expect(reply.ok).toBe(false)
    expect(reply.error).toContain("未打开")
  })

  test("lsp.close 在文件工作台关闭时拒绝，其余情况幂等成功", async () => {
    const off = await call(makeDeps({ config: { fsEnabled: false } }), "lsp.close", { docId: "d1" })
    expect(off.ok).toBe(false)
    const ok = await call(makeDeps(), "lsp.close", { docId: "不存在" })
    expect(ok.ok).toBe(true)
  })
})
