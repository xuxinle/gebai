/** 终端路由测试：闸门（fs 开关 / 沙箱 / 终端开关 / 只读环境）、REST 契约字段形状与执行审计。
 *  注入假 spawner 的真实 TerminalService——路由层与会话层一起验，不真起 shell。 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { createApp, SERVICE_USER, type AppDeps } from "../app"
import type { ServerConfig } from "../core/base/config"
import type { FsAuditEntry } from "../core/fs/audit"
import { TerminalService, type ShellSpec, type TermSpawner } from "../core/exec/term-session"

const SHELLS: ShellSpec[] = [{ id: "bash", name: "Bash", path: "/bin/bash", available: true }]

/** 假终端服务：spawner 记录启动参数与写入，测试直接驱动输出。 */
function fakeTerminal() {
  const spawned: Array<{ cwd: string; writes: string[]; out: (text: string) => void }> = []
  let pid = 700
  const spawner: TermSpawner = (opts) => {
    const rec = { cwd: opts.cwd, writes: [] as string[], out: (text: string) => opts.onData(new TextEncoder().encode(text)) }
    spawned.push(rec)
    return { pid: pid++, write: (data) => rec.writes.push(data) }
  }
  const terminal = new TerminalService({ spawner, shells: SHELLS, killTree: () => {} })
  return { terminal, spawned }
}

function makeDeps(opts: { config?: Partial<ServerConfig>; sandboxed?: boolean; terminal?: TerminalService; audited?: FsAuditEntry[] } = {}): AppDeps {
  const config = {
    auth: "local",
    basePath: "/",
    gebaiHome: join(tmpdir(), "gebai-term-home"),
    fsEnabled: true,
    fsWrite: true,
    terminalEnabled: true,
    ...opts.config,
  } as unknown as ServerConfig
  const deps = {
    config,
    auth: { defaultUser: () => SERVICE_USER },
    sandbox: { enforcedFor: () => opts.sandboxed === true, isExempt: () => false },
    engine: { workbenchProjects: () => ({ projects: [], binds: [] }) },
    store: { getEnv: async () => ({}) },
    terminal: opts.terminal,
  } as unknown as AppDeps
  const audited = opts.audited
  if (audited) deps.fsAudit = { record: (e: FsAuditEntry) => audited.push(e) } as unknown as AppDeps["fsAudit"]
  return deps
}

/** POST 请求构造（JSON body）。 */
const post = (body: Record<string, unknown>) =>
  ({ method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(body) }) as const

/** 终端全部端点的探针（闸门测试逐个确认状态码）。 */
function probes(id = "t1"): Array<[string, RequestInit]> {
  return [
    ["/api/v1/terminal/info", {}],
    ["/api/v1/terminal/list", {}],
    ["/api/v1/terminal/read?id=t1&since=0", {}],
    ["/api/v1/terminal/create", post({ root: "user:" })],
    ["/api/v1/terminal/input", post({ id, data: "ls" })],
    ["/api/v1/terminal/interrupt", post({ id })],
    ["/api/v1/terminal/close", post({ id })],
  ]
}

describe("终端路由：可用性闸门", () => {
  test("GEBAI_FS_ENABLED=false：全部端点 404（不泄露能力存在性）", async () => {
    const app = createApp(makeDeps({ config: { fsEnabled: false }, terminal: fakeTerminal().terminal }))
    for (const [path, init] of probes()) {
      const res = await app.request(path, init)
      expect(`${path} → ${res.status}`).toBe(`${path} → 404`)
    }
  })

  test("沙箱非豁免用户：全部端点 403（终端等同任意命令执行）", async () => {
    const app = createApp(makeDeps({ sandboxed: true, terminal: fakeTerminal().terminal }))
    for (const [path, init] of probes()) {
      const res = await app.request(path, init)
      expect(`${path} → ${res.status}`).toBe(`${path} → 403`)
    }
    const body = (await (await app.request("/api/v1/terminal/info")).json()) as { error: string }
    expect(body.error).toContain("沙箱")
  })

  test("GEBAI_TERMINAL=false：info 回 200 + enabled:false + 中文原因，其余端点 404", async () => {
    const app = createApp(makeDeps({ config: { terminalEnabled: false }, terminal: fakeTerminal().terminal }))
    const res = await app.request("/api/v1/terminal/info")
    expect(res.status).toBe(200)
    const body = (await res.json()) as { enabled: boolean; reason?: string; writable: boolean; shells: unknown[] }
    expect(body.enabled).toBe(false)
    expect(body.reason).toContain("GEBAI_TERMINAL=false")
    expect(body.writable).toBe(true)
    expect((await app.request("/api/v1/terminal/list")).status).toBe(404)
    expect((await app.request("/api/v1/terminal/create", post({ root: "user:" }))).status).toBe(404)
  })

  test("服务未注入（d.terminal 缺省）：info 回 200 + enabled:false，端点 404", async () => {
    const app = createApp(makeDeps())
    const body = (await (await app.request("/api/v1/terminal/info")).json()) as { enabled: boolean; reason?: string; defaultShell: string; maxSessions: number }
    expect(body.enabled).toBe(false)
    expect(body.reason).toBeTruthy()
    expect(body.defaultShell).toBe("")
    expect(body.maxSessions).toBeGreaterThan(0)
    expect((await app.request("/api/v1/terminal/list")).status).toBe(404)
  })

  test("GEBAI_FS_WRITE=false：create/input 被拒（403），info 回不可用字段", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-term-ro-"))
    try {
      const audited: FsAuditEntry[] = []
      const app = createApp(makeDeps({ config: { fsWrite: false }, terminal: fakeTerminal().terminal, audited }))
      const info = (await (await app.request("/api/v1/terminal/info")).json()) as {
        enabled: boolean
        reason?: string
        writable: boolean
        sandboxed: boolean
        shells: ShellSpec[]
        defaultShell: string
        maxSessions: number
        idleMs: number
      }
      expect(info.enabled).toBe(false)
      expect(info.writable).toBe(false)
      expect(info.sandboxed).toBe(false)
      expect(info.reason).toContain("GEBAI_FS_WRITE=false")
      expect(info.shells.map((s) => s.id)).toEqual(["bash"])
      expect(info.defaultShell).toBe("bash")
      expect(info.idleMs).toBeGreaterThan(0)
      // 只读环境不允许执行命令
      expect((await app.request("/api/v1/terminal/create", post({ root: `abs:${dir}` }))).status).toBe(403)
      const input = await app.request("/api/v1/terminal/input", post({ id: "t1", data: "ls" }))
      expect(input.status).toBe(403)
      // 被拒的执行也留痕（ok:false + 原因）
      expect(audited).toHaveLength(1)
      expect(audited[0].action).toBe("term.exec")
      expect(audited[0].ok).toBe(false)
      expect(audited[0].error).toContain("只读")
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })
})

describe("终端路由：REST 契约", () => {
  test("create/input/read/list/interrupt/close 字段形状与审计留痕", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-term-"))
    try {
      const audited: FsAuditEntry[] = []
      const term = fakeTerminal()
      const app = createApp(makeDeps({ terminal: term.terminal, audited }))

      // create：根 + cwd（根内相对路径）
      const created = await app.request("/api/v1/terminal/create", post({ root: `abs:${dir}` }))
      expect(created.status).toBe(200)
      const session = (await created.json()) as { id: string; shell: string; shellName: string; cwd: string; root: string; cursor: number; output: string; startedAt: number }
      expect(session.id).toMatch(/^t[0-9a-f]{8}$/)
      expect(session.shell).toBe("bash")
      expect(session.shellName).toBe("Bash")
      expect(session.root).toBe(`abs:${dir}`)
      expect(session.cwd).toBe("")
      expect(session.cursor).toBe(0)
      expect(typeof session.output).toBe("string")
      expect(typeof session.startedAt).toBe("number")
      expect(term.spawned[0].cwd).toBe(dir)

      // input：exec 默认 true → 写命令 + 哨兵行
      const input = await app.request("/api/v1/terminal/input", post({ id: session.id, data: "ls -a" }))
      expect(input.status).toBe(200)
      const inputBody = (await input.json()) as { ok: boolean; cursor: number }
      expect(inputBody.ok).toBe(true)
      expect(inputBody.cursor).toBe(0)
      const commands = term.spawned[0].writes.filter((w) => !w.startsWith("chcp 65001"))
      expect(commands[0]).toBe("ls -a\n")
      const token = commands[1].match(/__GBEND_[0-9a-f]{6}__/)?.[0] ?? ""
      expect(token).not.toBe("")
      expect(audited).toHaveLength(1)
      expect(audited[0]).toMatchObject({ action: "term.exec", source: "web", ok: true, root: `abs:${dir}`, path: "" })
      expect(audited[0].detail).toMatchObject({ cmd: "ls -a", shell: "bash" })

      // read：增量输出（哨兵行已剥离）+ 事件 + 存活标记
      term.spawned[0].out("a.txt\nb.txt\n")
      term.spawned[0].out(`${token}0|${dir}\n`)
      const read = await app.request(`/api/v1/terminal/read?id=${session.id}&since=${inputBody.cursor}`)
      expect(read.status).toBe(200)
      const readBody = (await read.json()) as { cursor: number; text: string; exits: Array<{ token: string; code: number; cwd: string }>; alive: boolean }
      expect(readBody.text).toBe("a.txt\nb.txt\n")
      expect(readBody.cursor).toBeGreaterThan(inputBody.cursor)
      expect(readBody.alive).toBe(true)
      expect(readBody.exits).toEqual([{ token, code: 0, cwd: dir }])

      // list：会话清单（busy 随哨兵收尾回落）
      const list = (await (await app.request("/api/v1/terminal/list")).json()) as { sessions: Array<Record<string, unknown>> }
      expect(list.sessions).toHaveLength(1)
      expect(list.sessions[0]).toMatchObject({ id: session.id, shell: "bash", shellName: "Bash", root: `abs:${dir}`, busy: false })
      expect(typeof list.sessions[0].startedAt).toBe("number")

      // interrupt：重建 shell（保留 cwd）
      const interrupted = await app.request("/api/v1/terminal/interrupt", post({ id: session.id }))
      expect(interrupted.status).toBe(200)
      expect(await interrupted.json()).toEqual({ ok: true, cwd: "" })
      expect(term.spawned).toHaveLength(2)

      // close：幂等
      expect(await (await app.request("/api/v1/terminal/close", post({ id: session.id }))).json()).toEqual({ ok: true })
      expect(await (await app.request("/api/v1/terminal/close", post({ id: session.id }))).json()).toEqual({ ok: true })

      // 会话不存在：read 404
      expect((await app.request("/api/v1/terminal/read?id=tmissing&since=0")).status).toBe(404)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("create 越界 cwd 与非法 root：403/400（路径边界仍由 roots 层把关）", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-term-esc-"))
    try {
      const app = createApp(makeDeps({ terminal: fakeTerminal().terminal }))
      expect((await app.request("/api/v1/terminal/create", post({ root: `abs:${dir}`, cwd: "../outside" }))).status).toBe(400)
      expect((await app.request("/api/v1/terminal/create", post({ root: "proj:missing" }))).status).toBe(404)
      expect((await app.request("/api/v1/terminal/create", post({ root: "bogus:x" }))).status).toBe(400)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })
})
