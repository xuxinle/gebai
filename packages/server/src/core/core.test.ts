import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, symlinkSync, writeFileSync, existsSync } from "node:fs"
import { readFile, writeFile } from "node:fs/promises"
import { tmpdir } from "node:os"
import { join, resolve } from "node:path"
import { ToolRegistry } from "./registry"
import { shardPath, sessionPath, resolveInSandbox, sha256Hex } from "./paths"
import { EnvManager, isSensitive, filterEnvInjection, cleanupLegacyUserEnv } from "./env"
import { Sandbox } from "./sandbox"
import { SessionStore, toSessionInfo, estimateCtxTokens, isProtectedMessage } from "./store"
import type { Tool, ToolContext } from "./types"

function tool(name: string, approval = false): Tool {
  return {
    name,
    description: name,
    parameters: { type: "object", properties: {} },
    requiresApproval: approval,
    execute: async () => ({ output: name }),
  }
}

describe("ToolRegistry namespace resolution", () => {
  test("global exact match wins", () => {
    const r = new ToolRegistry()
    r.register(tool("read"))
    expect(r.resolve("read")?.name).toBe("read")
  })

  test("sub-agent tools get {agent}_ prefix and resolve", () => {
    const r = new ToolRegistry()
    r.registerSubAgentTools("web", { search: tool("search") })
    expect(r.resolve("web_search")?.name).toBe("web_search")
    expect(r.resolve("web_search")?.agent).toBe("web")
    expect(r.resolve("search")).toBeUndefined()
  })

  test("sub-agent names must not be prefixes of each other (register-time check)", () => {
    const r = new ToolRegistry()
    r.registerSubAgentTools("web", { get: tool("get") })
    // DESIGN「命名约束」：子Agent 名不得互为前缀（构建时报错）
    expect(() => r.registerSubAgentTools("web_page", { get: tool("get") })).toThrow(/prefix collision/)
  })

  test("global tool name must not collide with sub-agent namespace", () => {
    const r = new ToolRegistry()
    r.registerSubAgentTools("web", { get: tool("get") })
    expect(() => r.register(tool("web_read"))).toThrow(/collides/)
  })

  test("sub-agent tool resolves via {agent}_ prefix", () => {
    const r = new ToolRegistry()
    r.registerSubAgentTools("web", { get: tool("get"), post: tool("post") })
    expect(r.resolve("web_get")?.name).toBe("web_get")
    expect(r.resolve("web_post")?.name).toBe("web_post")
    expect(r.resolve("get")).toBeUndefined()
  })

  test("separator normalization retry (dash/dot/colon)", () => {
    const r = new ToolRegistry()
    r.register(tool("my_tool"))
    expect(r.resolve("my-tool")?.name).toBe("my_tool")
    expect(r.resolve("my.tool")?.name).toBe("my_tool")
  })

  test("disabled tool is not resolved", () => {
    const r = new ToolRegistry()
    r.register(tool("read"))
    r.setEnabled("read", false)
    expect(r.resolve("read")).toBeUndefined()
  })

  test("enable/disable via set", () => {
    const r = new ToolRegistry()
    r.register(tool("a"))
    r.register(tool("b"))
    r.enableSet(["a"], ["b"])
    expect(r.resolve("a")).toBeDefined()
    expect(r.resolve("b")).toBeUndefined()
  })

  test("agent wildcard disable", () => {
    const r = new ToolRegistry()
    r.registerSubAgentTools("web", { get: tool("get"), post: tool("post") })
    r.enableSet(undefined, ["web_*"])
    expect(r.resolve("web_get")).toBeUndefined()
    expect(r.resolve("web_post")).toBeUndefined()
  })
})

describe("sharding and sandbox", () => {
  test("shardPath returns hex prefix layers", () => {
    const [s0, s1] = shardPath("session-abc")
    expect(s0).toMatch(/^[0-9a-f]{2}$/)
    expect(s1).toMatch(/^[0-9a-f]{2}$/)
  })

  test("sessionPath embeds user and shards", () => {
    const p = sessionPath("/home", "alice", "0123456789abcdef0123456789abcdef")
    expect(p).toContain("users")
    expect(p).toContain("alice")
    expect(p).toContain("sessions")
    expect(p).toContain("0123456789abcdef0123456789abcdef")
  })

  test("sessionPath rejects invalid ids (traversal / malformed)", () => {
    // 路径穿越串与畸形 id 一律拒绝（多用户隔离防线）
    expect(() => sessionPath("/home", "alice", "a/../../bob/sessions/x")).toThrow("invalid session id")
    expect(() => sessionPath("/home", "alice", "..")).toThrow("invalid session id")
    expect(() => sessionPath("/home", "alice", "deadbeef")).toThrow("invalid session id")
    expect(() => sessionPath("/home", "alice", "")).toThrow("invalid session id")
  })

  test("sha256 is 64 hex chars", () => {
    expect(sha256Hex("x")).toMatch(/^[0-9a-f]{64}$/)
  })

  test("resolveInSandbox rejects traversal", () => {
    expect(() => resolveInSandbox("/root", "../secret")).toThrow()
    expect(() => resolveInSandbox("/root", "/etc/passwd")).toThrow()
    expect(resolveInSandbox("/root", "a/b.txt")).toBe(resolve("/root", "a", "b.txt"))
  })

  test("resolveInSandbox rejects symlinks escaping the sandbox", () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-symlink-"))
    const outside = mkdtempSync(join(tmpdir(), "gebai-symlink-out-"))
    writeFileSync(join(outside, "secret.txt"), "top secret")
    mkdirSync(join(home, "sub"), { recursive: true })
    try {
      // Windows 无开发者模式/管理员权限时 symlink 失败：跳过该环境
      try {
        symlinkSync(outside, join(home, "sub", "link"))
      } catch {
        return
      }
      // 直接指向外部
      expect(() => resolveInSandbox(home, "sub/link/secret.txt")).toThrow(/symlink/)
      // 祖先目录是符号链接（新建文件场景）
      expect(() => resolveInSandbox(home, "sub/link/new.txt")).toThrow(/symlink/)
      // 正常路径不受影响
      expect(resolveInSandbox(home, "sub/normal.txt")).toBe(resolve(home, "sub", "normal.txt"))
    } finally {
      rmSync(home, { recursive: true, force: true })
      rmSync(outside, { recursive: true, force: true })
    }
  })
})

describe("SessionStore context tokens", () => {
  test("estimateCtxTokens ignores system role and counts messages/toolCalls", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-ctx-"))
    const store = new SessionStore({ home })
    const session = await store.createSession("default")
    // 旧会话（新版本前创建）：无 ctxTokens 字段 → 列表兜底估算
    await store.appendMessage(session.id, {
      id: "m1",
      role: "user",
      content: "你好，请帮我写一个很长的 Python 脚本用于数据处理。",
      createdAt: Date.now(),
    })
    await store.appendMessage(session.id, {
      id: "m2",
      role: "assistant",
      content: "好的，脚本如下。",
      toolCalls: [{ id: "tc1", name: "write_tool", arguments: { path: "a.py", content: "x".repeat(400) } }],
      createdAt: Date.now(),
    })
    const sessions = await store.listSessions("default")
    expect(sessions[0].ctxTokens).toBeUndefined() // 磁盘上无持久化值
    // toSessionInfo 兜底：按消息估算（>0）
    const info = toSessionInfo(sessions[0])
    expect(info.ctxTokens).toBeGreaterThan(0)
    // estimateCtxTokens 忽略 system 消息
    const withSystem = [{ role: "system", content: "S".repeat(4000) }, ...sessions[0].messages]
    const base = estimateCtxTokens(sessions[0].messages)
    expect(estimateCtxTokens(withSystem)).toBe(base)
    expect(base).toBeGreaterThan(0)
    cleanup(home)
  })

  test("compactMessages 清除真实 usage 基线（消息被替换后索引锚点失效）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-ctx-"))
    const store = new SessionStore({ home })
    const session = await store.createSession("default")
    for (let i = 0; i < 4; i++) {
      await store.appendMessage(session.id, { id: crypto.randomUUID(), role: "assistant", content: `a${i}`, createdAt: Date.now() })
    }
    const loaded = await store.load(session.id)
    loaded!.ctxInputTokens = 1234
    loaded!.ctxAtMessage = 4
    await store.save(loaded!)
    await store.compactMessages(session.id, "default", { from: 0, to: 2, summary: "摘要" })
    const after = await store.load(session.id)
    expect(after!.ctxInputTokens).toBeUndefined()
    expect(after!.ctxAtMessage).toBeUndefined()
    cleanup(home)
  })

  test("compactMessages 区间内仅剩受保护消息时不做改动（不创建摘要、保留 usage 基线）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-ctx-"))
    const store = new SessionStore({ home })
    const session = await store.createSession("default")
    for (let i = 0; i < 4; i++) {
      await store.appendMessage(session.id, { id: crypto.randomUUID(), role: "user", content: `q${i}`, createdAt: Date.now() })
    }
    const loaded = await store.load(session.id)
    loaded!.ctxInputTokens = 1234
    loaded!.ctxAtMessage = 4
    await store.save(loaded!)
    await store.compactMessages(session.id, "default", { from: 0, to: 2, summary: "摘要" })
    const after = await store.load(session.id)
    expect(after!.messages).toHaveLength(4) // 用户输入不改变
    expect(after!.ctxInputTokens).toBe(1234) // 消息未被替换：基线锚点仍有效
    cleanup(home)
  })
})

describe("EnvManager precedence", () => {
  test("session（内存态）> global，会话 env 零落盘（服务端不留存）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-env-"))
    const store = new SessionStore({ home })
    const env = new EnvManager(store)
    const session = await store.createSession("default")
    process.env.FOO_GLOBAL = "global"
    await store.setEnv(session.id, "default", { FOO: "session-foo" })
    const resolved = await env.resolve(session.id, "default")
    expect(resolved.FOO).toBe("session-foo")
    expect(resolved.FOO_GLOBAL).toBe("global")
    // 用户环境变量只存浏览器本地：会话目录不产生 env.json
    expect(existsSync(join(store.getSessionDir(session.id, "default"), "env.json"))).toBe(false)
    delete process.env.FOO_GLOBAL
    cleanup(home)
  })

  test("存量会话 env.json 首次触达惰性清理（迁移，不再读取）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-env-mig-"))
    const store = new SessionStore({ home })
    const session = await store.createSession("default")
    const p = join(store.getSessionDir(session.id, "default"), "env.json")
    writeFileSync(p, JSON.stringify({ LEGACY: "1" }))
    expect(await store.getEnv(session.id, "default")).toEqual({})
    expect(existsSync(p)).toBe(false)
    cleanup(home)
  })

  test("cleanupLegacyUserEnv 启动清理历史用户级 env.json（用户层废弃）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-env-user-"))
    mkdirSync(join(home, "users", "alice"), { recursive: true })
    writeFileSync(join(home, "users", "alice", "env.json"), "{}")
    await cleanupLegacyUserEnv(home)
    expect(existsSync(join(home, "users", "alice", "env.json"))).toBe(false)
    cleanup(home)
  })

  test("sensitive detection", () => {
    expect(isSensitive("API_KEY")).toBe(true)
    expect(isSensitive("OPENAI_API_KEY")).toBe(true)
    expect(isSensitive("GEBAI_ADMIN_PASSWORD_HASH")).toBe(true)
    expect(isSensitive("AWS_ACCESS_KEY_ID")).toBe(true)
    expect(isSensitive("DATABASE_URL")).toBe(true)
    // 裸名形态同样视为敏感（TOKEN 历史漏判已修复）
    expect(isSensitive("TOKEN")).toBe(true)
    expect(isSensitive("PATH")).toBe(false)
    expect(isSensitive("GEBAI_HOME")).toBe(false)
  })
})

describe("Sandbox exec", () => {
  test("runs a command and captures output", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox-"))
    const sandbox = new Sandbox({ home, enabled: false })
    const res = await sandbox.exec(`echo hello`)
    expect(res.stdout.trim()).toBe("hello")
    cleanup(home)
  })

  test("enabled sandbox restricts path to session tmp dir", () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox2-"))
    const sandbox = new Sandbox({ home, enabled: true })
    const session = "0123456789abcdef0123456789abcdef"
    const root = sessionPath(home, "alice", session)
    const inside = sandbox.resolvePath("alice", session, "tmp/a.txt")
    expect(inside.startsWith(join(root, "tmp"))).toBe(true)
    // 相对路径基准 = 会话 tmp/：a.txt 与 tmp/a.txt 同一目标（tmp/ 前缀剥离兼容）
    expect(sandbox.resolvePath("alice", session, "a.txt")).toBe(join(root, "tmp", "a.txt"))
    expect(() => sandbox.resolvePath("alice", session, "../x")).toThrow()
    expect(() => sandbox.resolvePath("alice", session, "/etc/passwd")).toThrow()
    cleanup(home)
  })

  test("disabled sandbox allows absolute and parent paths (local run)", () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox5-"))
    const sandbox = new Sandbox({ home, enabled: false })
    const session = "0123456789abcdef0123456789abcdef"
    const abs = sandbox.resolvePath("alice", session, "/tmp/anywhere.txt")
    expect(abs).toBe(resolve("/tmp/anywhere.txt"))
    // 相对路径仍基于会话 tmp/ 解析（与 sh/py 工作目录一致），允许越界
    const parent = sandbox.resolvePath("alice", session, "../x.txt")
    expect(parent).toBe(resolve(join(sessionPath(home, "alice", session), "tmp"), "../x.txt"))
    cleanup(home)
  })

  test("exec times out and kills the child", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox3-"))
    const sandbox = new Sandbox({ home, enabled: false })
    const res = await sandbox.exec(`node -e "setTimeout(()=>{},5000)"`, { timeoutMs: 200 })
    expect(res.code).toBe(124)
    expect(res.stderr).toContain("timed out")
    cleanup(home)
  })

  test("exec surfaces spawn errors", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox4-"))
    const sandbox = new Sandbox({ home, enabled: false })
    const res = await sandbox.exec("definitely-not-a-real-cmd-xyz")
    expect(res.code).not.toBe(0)
    cleanup(home)
  })

  test("sandboxed exec scrubs sensitive env vars from child process (server deployment)", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-sandbox6-"))
    const prevSecret = process.env.GEBAI_TEST_SECRET_KEY
    const prevPlain = process.env.GEBAI_TEST_PLAIN
    process.env.GEBAI_TEST_SECRET_KEY = "supersecret"
    process.env.GEBAI_TEST_PLAIN = "visible"
    try {
      // 沙箱（服务端部署）：敏感变量（*_KEY 等）不进入脚本子进程，防任意用户经 sh/py 外泄服务端密钥
      const sandboxOn = new Sandbox({ home, enabled: true })
      const probe = process.platform === "win32" ? "set" : "env"
      const on = await sandboxOn.exec(probe)
      expect(on.stdout).not.toContain("supersecret")
      expect(on.stdout).toContain("GEBAI_TEST_PLAIN=visible")
      // 本地模式（沙箱关闭）：不脱敏（用户本机环境，密钥归用户自己）
      const sandboxOff = new Sandbox({ home, enabled: false })
      const off = await sandboxOff.exec(probe)
      expect(off.stdout).toContain("supersecret")
    } finally {
      if (prevSecret === undefined) delete process.env.GEBAI_TEST_SECRET_KEY
      else process.env.GEBAI_TEST_SECRET_KEY = prevSecret
      if (prevPlain === undefined) delete process.env.GEBAI_TEST_PLAIN
      else process.env.GEBAI_TEST_PLAIN = prevPlain
      cleanup(home)
    }
  })
})

describe("filterEnvInjection (prompt 注入通道宽容过滤)", () => {
  test("非 string/null 值丢弃，合法变量保留", () => {
    expect(filterEnvInjection({ A: "1", B: null, C: 2 as never })).toEqual({ A: "1" })
  })

  test("非法标识符名与 __proto__ 丢弃（原型污染防御）", () => {
    const out = filterEnvInjection({ "a-b": "x", "__proto__": "p", OK_VAR: "1" })
    expect(out).toEqual({ OK_VAR: "1" })
    expect(({} as Record<string, string>).polluted).toBeUndefined()
  })

  test("GEBAI_APPROVAL_SKIP 不再按角色过滤（会话级审批跳过对所有用户开放，注入通道保留）", () => {
    expect(filterEnvInjection({ GEBAI_APPROVAL_SKIP: "true", CODE_PROJECT: "/x" })).toEqual({ GEBAI_APPROVAL_SKIP: "true", CODE_PROJECT: "/x" })
  })
})

describe("SessionStore ownership", () => {
  test("ownerOf tracks loaded/created sessions and clears on delete", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-owner-"))
    const store = new SessionStore({ home })
    const s = await store.createSession("alice")
    expect(store.ownerOf(s.id)).toBe("alice")
    // 缓存失效后仍可从最近记录解析（webhook 事件过滤场景）
    store.evict(s.id)
    expect(store.ownerOf(s.id)).toBe("alice")
    await store.delete(s.id)
    expect(store.ownerOf(s.id)).toBeNull()
    cleanup(home)
  })

  test("legacy session without userId cannot be resolved cross-user via index", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-legacy-"))
    const store = new SessionStore({ home })
    const s = await store.createSession("alice")
    // 模拟旧版会话（无 userId 字段）：直接改写 chat.json 并失效缓存
    const chatPath = join(sessionPath(home, "alice", s.id), "chat.json")
    const parsed = JSON.parse(await readFile(chatPath, "utf8")) as Record<string, unknown>
    delete parsed.userId
    await writeFile(chatPath, JSON.stringify(parsed))
    store.evict(s.id)
    // alice 自己的索引可命中（legacy 归属兜底），bob 的索引为空不可经索引跨用户命中
    await store.listSessions("alice")
    expect(await store.load(s.id, "alice")).not.toBeNull()
    expect(await store.load(s.id, "bob")).toBeNull()
    cleanup(home)
  })

  test("traversal session ids are rejected and never touch other users' data", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-traversal-"))
    const store = new SessionStore({ home })
    const victim = await store.createSession("victim")
    const attack = `a/../../../${victim.id}/x` // 若未拦截将解析进 victim 目录
    // load：穿越串视为不存在，不返回任何会话
    expect(await store.load(attack, "attacker")).toBeNull()
    // getEnv/setEnv/delete 同样拒绝（路径构造抛错或安全返回，绝不写他人目录）
    expect(() => store.getTmpDir(attack, "attacker")).toThrow()
    expect(() => store.getSessionDir(attack, "attacker")).toThrow()
    await expect(store.setEnv(attack, "attacker", { A: "1" })).rejects.toThrow()
    await expect(store.delete(attack, "attacker")).resolves.toBeUndefined() // load 前置返回 null，不删任何东西
    // 受害者数据完好
    const victimChat = join(sessionPath(home, "victim", victim.id), "chat.json")
    expect(JSON.parse(await readFile(victimChat, "utf8"))).toHaveProperty("id", victim.id)
    expect(await store.load(victim.id, "victim")).not.toBeNull()
    cleanup(home)
  })
})

function cleanup(home: string) {
  rmSync(home, { recursive: true, force: true })
  void (null as unknown as ToolContext)
}

describe("SessionStore 装载提示词消息保护", () => {
  test("appendMessage 超限截断保留系统提示词与用户输入且不调整顺序（不压缩不改变）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-store-load-"))
    const store = new SessionStore({ home })
    const session = await store.createSession("default")
    // 模拟会话中途装载：先有历史消息，再 append 装载提示词（末尾），再大量普通消息触发截断
    for (let i = 0; i < 5; i++) {
      await store.appendMessage(session.id, { id: `m-${i}`, role: "user", content: `msg ${i}`, createdAt: i + 1 } as never)
    }
    await store.appendMessage(session.id, { id: "load-1", role: "system", loadedAgent: "code", content: "### code（提示词）", createdAt: 6 } as never)
    for (let i = 5; i < 355; i++) {
      await store.appendMessage(session.id, { id: `m-${i}`, role: "assistant", content: `msg ${i}`, createdAt: i + 2 } as never)
    }
    const loaded = await store.load(session.id)
    expect(loaded!.messages.length).toBeLessThanOrEqual(300)
    // 用户输入与系统提示词全部保留（截断只丢可压缩的 assistant 消息）
    for (let i = 0; i < 5; i++) expect(loaded!.messages.some((m) => m.id === `m-${i}`)).toBe(true)
    const loadIdx = loaded!.messages.findIndex((m) => m.loadedAgent === "code")
    expect(loadIdx).toBeGreaterThan(-1)
    // 顺序保持：截断为「原数组子序列」（从最早的非 protected 丢弃），装载提示词后的消息仍是原序后续（未被重排/穿插）
    const ids = loaded!.messages.map((m) => m.id)
    expect(ids[ids.indexOf("load-1") + 1]).toBe("m-61") // 丢最早 56 条 assistant 后，装载消息后紧跟原序后续
    rmSync(home, { recursive: true, force: true })
  })

  test("appendMessage 超限截断按 tool_call 配对原子丢弃（不产生孤儿 tool 消息）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-store-trim-pair-"))
    const store = new SessionStore({ home })
    const session = await store.createSession("default")
    await store.appendMessage(session.id, { id: "u-0", role: "user", content: "q", createdAt: 1 } as never)
    // 150 轮工具调用（assistant(toolCalls) + tool 结果交替）+ 1 条用户输入 = 301 条，恰超 300 上限 1 条：
    // 预算只允许丢 1 条——首轮 assistant 被丢后其 tool 结果必须连带丢弃（配对原子性的关键场景）
    for (let i = 0; i < 150; i++) {
      await store.appendMessage(session.id, { id: `a-${i}`, role: "assistant", toolCalls: [{ id: `tc-${i}`, name: "read", arguments: {} }], content: "", createdAt: 2 + i * 2 } as never)
      await store.appendMessage(session.id, { id: `t-${i}`, role: "tool", toolCallId: `tc-${i}`, name: "read", content: "ok", createdAt: 3 + i * 2 } as never)
    }
    const msgs = (await store.load(session.id))!.messages
    expect(msgs.length).toBe(299) // 301 - 首轮整对 2 条（软上限允许略低于 300）
    // 首轮 assistant 与其 tool 结果一起消失（而非只丢 assistant 留下孤儿 tool）
    expect(msgs.some((m) => m.id === "a-0" || m.id === "t-0")).toBe(false)
    // 每条 tool 消息的前一条都是包含其 toolCallId 的 assistant（配对完整，无孤儿）
    for (let i = 1; i < msgs.length; i++) {
      if (msgs[i].role !== "tool") continue
      const prev = msgs[i - 1]
      expect(prev?.role === "assistant" && prev.toolCalls?.some((tc) => tc.id === msgs[i].toolCallId)).toBe(true)
    }
    // 首个非保护消息是某轮的 assistant（而非被拆散出来的 tool）
    expect(msgs.find((m) => !isProtectedMessage(m))?.role).toBe("assistant")
    rmSync(home, { recursive: true, force: true })
  })

  test("compactMessages 区间内系统提示词与用户输入原位保留（不随压缩替换丢失）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-store-compact-load-"))
    const store = new SessionStore({ home })
    const session = await store.createSession("default")
    for (let i = 0; i < 6; i++) {
      await store.appendMessage(session.id, { id: `m-${i}`, role: "user", content: `msg ${i}`, createdAt: i + 1 } as never)
    }
    await store.appendMessage(session.id, { id: "load-1", role: "system", loadedAgent: "code", content: "### code（提示词）", createdAt: 7 } as never)
    for (let i = 6; i < 10; i++) {
      await store.appendMessage(session.id, { id: `m-${i}`, role: "assistant", content: `msg ${i}`, createdAt: i + 2 } as never)
    }
    // 压缩中间区间（含系统提示词与用户输入）：原位保留，仅 assistant 消息并入摘要
    await store.compactMessages(session.id, "default", { from: 1, to: 9, summary: "压缩摘要" })
    const loaded = await store.load(session.id)
    expect(loaded!.messages.some((m) => m.loadedAgent === "code")).toBe(true)
    // 区间内的用户输入全部原位保留（不压缩不改变）
    for (let i = 1; i < 6; i++) expect(loaded!.messages.some((m) => m.id === `m-${i}` && m.content === `msg ${i}`)).toBe(true)
    // 区间内的 assistant 消息被摘要替换（m-6/m-7 在区间内；m-8/m-9 区间外保留）
    expect(loaded!.messages.some((m) => m.compacted)).toBe(true)
    expect(loaded!.messages.some((m) => m.id === "m-6" || m.id === "m-7")).toBe(false)
    expect(loaded!.messages.some((m) => m.id === "m-8")).toBe(true)
    // 摘要计数 = 实际移除条数（2 条 assistant）
    expect(loaded!.messages.find((m) => m.compacted)!.summary).toContain("已压缩 2 条")
    rmSync(home, { recursive: true, force: true })
  })

  test("compactMessages 边界切在 assistant(toolCalls)/tool 配对中间时不产生孤儿（配对修复）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-store-compact-pair-"))
    const store = new SessionStore({ home })
    const session = await store.createSession("default")
    // [A(tc), T, A(tc), T, U] 压缩 [0,2)：第一条配对整体入摘要，第二条保留——不产生孤儿
    await store.appendMessage(session.id, { id: "a1", role: "assistant", content: "call1", toolCalls: [{ id: "tc1", name: "sh", arguments: {} }], createdAt: 1 } as never)
    await store.appendMessage(session.id, { id: "t1", role: "tool", content: "r1", toolCallId: "tc1", name: "sh", createdAt: 2 } as never)
    await store.appendMessage(session.id, { id: "a2", role: "assistant", content: "call2", toolCalls: [{ id: "tc2", name: "sh", arguments: {} }], createdAt: 3 } as never)
    await store.appendMessage(session.id, { id: "t2", role: "tool", content: "r2", toolCallId: "tc2", name: "sh", createdAt: 4 } as never)
    await store.appendMessage(session.id, { id: "u1", role: "user", content: "next", createdAt: 5 } as never)
    await store.compactMessages(session.id, "default", { from: 0, to: 2, summary: "第一对已压缩" })
    const loaded = await store.load(session.id)
    const tools = loaded!.messages.filter((m) => m.role === "tool")
    const assistants = loaded!.messages.filter((m) => m.role === "assistant")
    // 剩余 tool 消息（t2）必有发起 assistant（a2）；孤儿 tool（tc1 随 a1 被压缩）不残留
    expect(tools.every((t) => assistants.some((a) => a.toolCalls?.some((tc) => tc.id === t.toolCallId)))).toBe(true)
    expect(loaded!.messages.some((m) => m.role === "tool" && m.toolCallId === "tc1")).toBe(false)
    rmSync(home, { recursive: true, force: true })
  })

  test("repairToolPairing：孤儿 tool 丢弃/受保护补桩、中途未应答补占位（磁盘装载自愈）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-store-repair-"))
    const store = new SessionStore({ home })
    const session = await store.createSession("default")
    // 构造历史损坏：中途未应答 toolCalls（a1 无结果即被 u1 跟随）、孤儿普通 tool（t-x）、孤儿受保护 tool（t-run）
    await store.appendMessage(session.id, { id: "a1", role: "assistant", content: "x", toolCalls: [{ id: "tc-1", name: "sh", arguments: {} }], createdAt: 1 } as never)
    await store.appendMessage(session.id, { id: "u1", role: "user", content: "q", createdAt: 2 } as never)
    await store.appendMessage(session.id, { id: "t-x", role: "tool", content: "orphan", toolCallId: "tc-none", name: "sh", createdAt: 3 } as never)
    await store.appendMessage(session.id, { id: "t-run", role: "tool", content: "archive", toolCallId: "tc-run", name: "agent_run", sessionRun: { entries: [] }, createdAt: 4 } as never)
    // 新实例（无内存缓存）从磁盘装载：readFileByPath 修复
    const store2 = new SessionStore({ home })
    const loaded = await store2.load(session.id, "default")
    // a1 的 tc-1 得到占位结果（在 u1 之前，保持 assistant→tool 相邻）
    const placeholder = loaded!.messages.find((m) => m.role === "tool" && m.toolCallId === "tc-1")
    expect(placeholder).toBeTruthy()
    expect(loaded!.messages.indexOf(placeholder!)).toBeLessThan(loaded!.messages.findIndex((m) => m.id === "u1"))
    // 孤儿普通 tool 丢弃；受保护 t-run 前补最小 assistant 桩（紧邻其后）
    expect(loaded!.messages.some((m) => m.id === "t-x")).toBe(false)
    const stubIdx = loaded!.messages.findIndex((m) => m.role === "assistant" && m.toolCalls?.some((tc) => tc.id === "tc-run"))
    expect(stubIdx).toBeGreaterThan(-1)
    expect(loaded!.messages.findIndex((m) => m.id === "t-run")).toBe(stubIdx + 1)
    // 全序列两两配对完整（OpenAI/Anthropic 校验前提）
    const seen = new Set<string>()
    for (const m of loaded!.messages) {
      if (m.role === "tool") expect(seen.has(m.toolCallId!)).toBe(true)
      if (m.role === "assistant" && m.toolCalls) for (const tc of m.toolCalls) seen.add(tc.id)
    }
    rmSync(home, { recursive: true, force: true })
  })
})
