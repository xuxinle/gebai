/** LSP 路由测试：闸门（fs 开关 / 沙箱 / GEBAI_LSP 开关 / 服务未注入）与清单契约形状。 */
import { describe, expect, test } from "bun:test"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { createApp, SERVICE_USER, type AppDeps } from "../app"
import type { ServerConfig } from "../core/base/config"
import { LspService } from "../core/lsp/service"

/** 假探测：只有列出的命令算「装了」（不碰真实 PATH）。 */
function fakeLsp(installed: Record<string, string> = { gopls: "/usr/bin/gopls", "rust-analyzer": "/usr/bin/rust-analyzer" }): LspService {
  return new LspService({ which: (cmd) => installed[cmd] ?? null })
}

function makeDeps(opts: { config?: Partial<ServerConfig>; sandboxed?: boolean; lsp?: LspService | null } = {}): AppDeps {
  const config = {
    auth: "local",
    gebaiHome: join(tmpdir(), "gebai-lsp-home"),
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
    store: { getEnv: async () => ({}) },
  } as unknown as AppDeps
  if (opts.lsp !== null) deps.lsp = opts.lsp ?? fakeLsp()
  return deps
}

const URL_SERVERS = "/api/v1/lsp/servers"

describe("LSP 路由：可用性闸门", () => {
  test("GEBAI_FS_ENABLED=false：404（不泄露能力存在性）", async () => {
    const app = createApp(makeDeps({ config: { fsEnabled: false } }))
    const res = await app.request(URL_SERVERS)
    expect(res.status).toBe(404)
  })

  test("沙箱非豁免用户：403", async () => {
    const app = createApp(makeDeps({ sandboxed: true }))
    const res = await app.request(URL_SERVERS)
    expect(res.status).toBe(403)
    expect(((await res.json()) as { error: string }).error).toContain("沙箱")
  })

  test("GEBAI_LSP=false：200 + enabled:false（前端静默降级，不是错误）", async () => {
    const app = createApp(makeDeps({ config: { lspEnabled: false } }))
    const res = await app.request(URL_SERVERS)
    expect(res.status).toBe(200)
    const body = (await res.json()) as { enabled: boolean; reason: string; servers: unknown[] }
    expect(body.enabled).toBe(false)
    expect(body.reason).toContain("GEBAI_LSP")
    expect(body.servers).toEqual([])
  })

  test("服务未注入：200 + enabled:false", async () => {
    const app = createApp(makeDeps({ lsp: null }))
    const body = (await (await app.request(URL_SERVERS)).json()) as { enabled: boolean }
    expect(body.enabled).toBe(false)
  })
})

describe("LSP 路由：清单契约", () => {
  test("探测到的服务器按语言列出，未装的进 missing（不影响使用）", async () => {
    const app = createApp(makeDeps())
    const res = await app.request(URL_SERVERS)
    expect(res.status).toBe(200)
    const body = (await res.json()) as {
      enabled: boolean
      servers: Array<{ language: string; id: string; command: string; args: string[] }>
      missing: Array<{ language: string; id: string; command: string }>
      errors: string[]
    }
    expect(body.enabled).toBe(true)
    expect(body.servers.map((s) => s.language).sort()).toEqual(["go", "rust"])
    expect(body.servers.find((s) => s.language === "go")?.command).toBe("/usr/bin/gopls")
    expect(body.missing.map((m) => m.language)).toContain("python")
    expect(body.errors).toEqual([])
  })

  test("清单不含 Monaco 已覆盖的语言（opt-in 需显式配置）", async () => {
    const app = createApp(makeDeps({ lsp: fakeLsp({ "typescript-language-server": "/usr/bin/tls", gopls: "/usr/bin/gopls" }) }))
    const body = (await (await app.request(URL_SERVERS)).json()) as { servers: Array<{ language: string }> }
    expect(body.servers.map((s) => s.language)).toEqual(["go"])
  })

  test("覆盖表非法项：errors 有说明，其它语言照常", async () => {
    const lsp = new LspService({ overrides: "{坏 JSON", which: (cmd) => (cmd === "gopls" ? "/usr/bin/gopls" : null) })
    const app = createApp(makeDeps({ lsp }))
    const body = (await (await app.request(URL_SERVERS)).json()) as { servers: Array<{ language: string }>; errors: string[] }
    expect(body.errors[0]).toContain("JSON")
    expect(body.servers.map((s) => s.language)).toEqual(["go"])
  })
})
