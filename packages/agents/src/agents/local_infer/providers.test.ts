/**
 * 推理目标层（providers）测试：环境变量解析的容错、目标解析优先级、鉴权头与密钥掩码、
 * 探活（Bun.serve 假端点）与 `targets` 工具的两种形态——全程零第三方依赖、不写仓库目录
 * （随机端口 + mkdtempSync，用例可并行）。
 */
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { afterAll, describe, expect, test } from "bun:test"
import type { ToolContext } from "@gebai/sdk"
import {
  isInferTarget,
  parseTargetsEnv,
  probeTarget,
  resolveTarget,
  requiresApproval,
  targetHeaders,
  targetSummary,
  tools,
  type InferTarget,
} from "./providers"

// ── 夹具 ──────────────────────────────────────────────────────────────────

const tmp = mkdtempSync(join(tmpdir(), "gebai-infer-providers-"))
afterAll(() => rmSync(tmp, { recursive: true, force: true }))

/** 精简 ToolContext：目标层只用 env（与 tasks.test.ts 同一形态）。 */
function makeCtx(env: Record<string, string>): ToolContext {
  return {
    user: "default",
    sessionId: "s1",
    workdir: tmp,
    sessionWorkdir: tmp,
    home: tmp,
    env,
    sandboxed: false,
    resolvePath: (p) => join(tmp, p),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, c) => writeFileSync(p, c),
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: (r) => Promise.resolve(r.path),
    publish: () => {},
    projects: [],
    resolveProjectPath: () => {
      throw new Error("无预置项目")
    },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
  }
}

/** 独立 infer home（可选写入端口的状态文件）。 */
function makeHome(states: Array<{ port: number; pid?: number; profile?: string; model?: string }> = []): string {
  const home = join(tmp, `home-${Math.random().toString(36).slice(2)}`)
  mkdirSync(join(home, "run"), { recursive: true })
  for (const s of states) {
    writeFileSync(join(home, "run", `server-${s.port}.json`), JSON.stringify({ pid: s.pid ?? 4242, profile: s.profile ?? "fast", model: s.model ?? "m.gguf", ...s }))
  }
  return home
}

/** 解析结果断言辅助：期望是目标（不是 error）。 */
function expectTarget(r: InferTarget | { error: string }): InferTarget {
  if ("error" in r) throw new Error(`期望解析出目标，实际报错：${r.error}`)
  return r
}

// ── parseTargetsEnv ───────────────────────────────────────────────────────

describe("parseTargetsEnv", () => {
  test("正常解析：可选字段、尾斜杠归一；未配置时为空且无错误", () => {
    const env = {
      LOCAL_INFER_TARGETS: JSON.stringify([
        { name: "lan", base_url: "http://192.168.1.20:8080/", api_key: "sk-lan-123456", model: "qwen2.5", note: "备机" },
        { name: "cloud", base_url: "https://api.example.com" },
      ]),
    }
    const { targets, errors } = parseTargetsEnv(env)
    expect(errors).toEqual([])
    expect(targets).toEqual([
      { name: "lan", base_url: "http://192.168.1.20:8080", api_key: "sk-lan-123456", model: "qwen2.5", note: "备机" },
      { name: "cloud", base_url: "https://api.example.com" },
    ])

    expect(parseTargetsEnv({})).toEqual({ targets: [], errors: [] })
    expect(parseTargetsEnv({ LOCAL_INFER_TARGETS: "   " })).toEqual({ targets: [], errors: [] })
  })

  test("非 JSON / 非数组：容错返回 errors，不抛异常", () => {
    const bad = parseTargetsEnv({ LOCAL_INFER_TARGETS: "[{name:lan}]" })
    expect(bad.targets).toEqual([])
    expect(bad.errors.length).toBe(1)
    expect(bad.errors[0]).toContain("不是合法 JSON")

    const notArray = parseTargetsEnv({ LOCAL_INFER_TARGETS: '{"name":"lan"}' })
    expect(notArray.targets).toEqual([])
    expect(notArray.errors[0]).toContain("必须是 JSON 数组")

    expect(parseTargetsEnv({ LOCAL_INFER_TARGETS: "null" }).errors[0]).toContain("当前是 null")
  })

  test("坏条目逐条报错并跳过，好条目保留", () => {
    const { targets, errors } = parseTargetsEnv({
      LOCAL_INFER_TARGETS: JSON.stringify([
        "lan",
        { base_url: "http://a:1" },
        { name: "no_base" },
        { name: "bad_url", base_url: "192.168.1.20:8080" },
        { name: "ok", base_url: "http://ok:8080" },
      ]),
    })
    expect(targets.map((t) => t.name)).toEqual(["ok"])
    expect(errors.length).toBe(4)
    expect(errors[0]).toContain("第 1 项不是对象")
    expect(errors[1]).toContain("第 2 项缺少 name")
    expect(errors[2]).toContain("目标 no_base 缺少 base_url")
    expect(errors[3]).toContain("不是 http(s) 端点")
  })

  test("重复名保留先声明的一个；local 是保留名", () => {
    const { targets, errors } = parseTargetsEnv({
      LOCAL_INFER_TARGETS: JSON.stringify([
        { name: "dup", base_url: "http://first:1" },
        { name: "dup", base_url: "http://second:2" },
        { name: "local", base_url: "http://l:9" },
      ]),
    })
    expect(targets).toEqual([{ name: "dup", base_url: "http://first:1" }])
    expect(errors.some((e) => e.includes("目标名 dup 重复"))).toBe(true)
    expect(errors.some((e) => e.includes("保留名"))).toBe(true)
  })
})

// ── resolveTarget ─────────────────────────────────────────────────────────

describe("resolveTarget（缺省 local）", () => {
  test("无状态文件：退回 LOCAL_INFER_PORT / 8080", () => {
    const home = makeHome()
    const withEnv = expectTarget(resolveTarget(undefined, makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: "18080" })))
    expect(withEnv.kind).toBe("local")
    expect(withEnv.name).toBe("local")
    expect(withEnv.baseUrl).toBe("http://127.0.0.1:18080")
    expect(withEnv.apiKey).toBeUndefined()
    expect(String(withEnv.note)).toContain("无服务实例状态文件")

    // 显式 "local" 与 undefined 等价；端口未配置时用 8080
    const noPort = expectTarget(resolveTarget("local", makeCtx({ LOCAL_INFER_HOME: makeHome() })))
    expect(noPort.baseUrl).toBe("http://127.0.0.1:8080")
  })

  test("有状态文件：状态文件端口优先于 LOCAL_INFER_PORT，带档位/模型；多实例取端口最小并列出其余", () => {
    const home = makeHome([
      { port: 8081, profile: "quality", model: "q.gguf" },
      { port: 8090, profile: "fast", model: "f.gguf" },
    ])
    const t = expectTarget(resolveTarget("local", makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: "9999" })))
    expect(t.baseUrl).toBe("http://127.0.0.1:8081")
    expect(t.model).toBe("q.gguf")
    expect(String(t.note)).toContain("本机受管实例")
    expect(String(t.note)).toContain("档位 quality")
    expect(String(t.note)).toContain("另有 1 个实例在跑（端口 8090）")
  })
})

describe("resolveTarget（直连与命名目标）", () => {
  test("http(s):// 直连：归一尾斜杠、kind=remote、显式密钥生效", () => {
    const ctx = makeCtx({ LOCAL_INFER_HOME: makeHome() })
    const t = expectTarget(resolveTarget("http://192.168.1.20:8080/", ctx, " sk-direct-123456 "))
    expect(t).toMatchObject({ name: "http://192.168.1.20:8080", kind: "remote", baseUrl: "http://192.168.1.20:8080", apiKey: "sk-direct-123456" })
    expect(targetHeaders(t)).toEqual({ authorization: "Bearer sk-direct-123456" })

    const https = expectTarget(resolveTarget("https://api.example.com", ctx))
    expect(https.baseUrl).toBe("https://api.example.com")
    expect(https.apiKey).toBeUndefined()
  })

  test("命名目标：取 base_url/model/note", () => {
    const ctx = makeCtx({
      LOCAL_INFER_HOME: makeHome(),
      LOCAL_INFER_TARGETS: JSON.stringify([{ name: "lan", base_url: "http://192.168.1.20:8080", model: "qwen2.5", note: "备机" }]),
    })
    const t = expectTarget(resolveTarget("lan", ctx))
    expect(t).toMatchObject({ name: "lan", kind: "remote", baseUrl: "http://192.168.1.20:8080", model: "qwen2.5", note: "备机" })
  })

  test("api_key 三级优先级：显式参数 > 命名目标字段 > LOCAL_INFER_REMOTE_API_KEY", () => {
    const env = {
      LOCAL_INFER_HOME: makeHome(),
      LOCAL_INFER_REMOTE_API_KEY: "sk-env-999999",
      LOCAL_INFER_TARGETS: JSON.stringify([
        { name: "lan", base_url: "http://lan:1", api_key: "sk-named-111111" },
        { name: "bare", base_url: "http://bare:2" },
      ]),
    }
    const ctx = makeCtx(env)
    expect(expectTarget(resolveTarget("lan", ctx)).apiKey).toBe("sk-named-111111")
    expect(expectTarget(resolveTarget("lan", ctx, "sk-explicit-222222")).apiKey).toBe("sk-explicit-222222")
    // 命名目标没配 key → 环境变量兜底
    expect(expectTarget(resolveTarget("bare", ctx)).apiKey).toBe("sk-env-999999")
    // 直连 URL 同理；local 也吃这一优先级（显式参数最优先）
    expect(expectTarget(resolveTarget("http://direct:3", ctx)).apiKey).toBe("sk-env-999999")
    expect(expectTarget(resolveTarget("local", ctx)).apiKey).toBe("sk-env-999999")
  })

  test("未知名：返回 error（不抛），含可用名清单与配置指引", () => {
    const ctx = makeCtx({
      LOCAL_INFER_HOME: makeHome(),
      LOCAL_INFER_TARGETS: JSON.stringify([{ name: "lan", base_url: "http://lan:1" }, { name: "cloud", base_url: "https://c" }]),
    })
    const r = resolveTarget("nope", ctx)
    expect("error" in r).toBe(true)
    const msg = (r as { error: string }).error
    expect(msg).toContain('未知推理目标 "nope"')
    expect(msg).toContain("lan")
    expect(msg).toContain("cloud")
    expect(msg).toContain("LOCAL_INFER_TARGETS")
    expect(msg).toContain("target=\"http://192.168.1.20:8080\"")

    // 没有命名目标时也给出同样的指引（不抛）
    const empty = resolveTarget("nope", makeCtx({ LOCAL_INFER_HOME: makeHome() })) as { error: string }
    expect(empty.error).toContain("当前没有命名目标")
  })
})

// ── 鉴权头 / 掩码 / 结构判定 ──────────────────────────────────────────────

describe("targetHeaders / targetSummary / isInferTarget", () => {
  test("无密钥 → 空对象；有密钥 → authorization: Bearer", () => {
    expect(targetHeaders({ name: "local", kind: "local", baseUrl: "http://127.0.0.1:8080" })).toEqual({})
    expect(targetHeaders({ name: "x", kind: "remote", baseUrl: "http://x", apiKey: "sk-abc" })).toEqual({ authorization: "Bearer sk-abc" })
  })

  test("targetSummary：密钥被掩码（原文绝不出现），信息单行可读", () => {
    const raw = "sk-live-abcdef123456789"
    const line = targetSummary({ name: "cloud", kind: "remote", baseUrl: "https://api.example.com", apiKey: raw, model: "gpt-4o", note: "云端" })
    expect(line).not.toContain(raw)
    expect(line).toContain("sk-***789")
    expect(line).toContain("cloud")
    expect(line).toContain("远端")
    expect(line).toContain("https://api.example.com")
    expect(line).toContain("模型 gpt-4o")
    expect(line).toContain("云端")

    // 短密钥全掩（前后缀都会被还原成原文，不冒险）
    const short = targetSummary({ name: "s", kind: "remote", baseUrl: "http://s", apiKey: "sk-abc" })
    expect(short).not.toContain("sk-abc")
    expect(short).toContain("key ***")

    // 本机目标无密钥，不出现 key 字样
    const local = targetSummary({ name: "local", kind: "local", baseUrl: "http://127.0.0.1:8080", note: "本机受管实例，端口 8080" })
    expect(local).toContain("本机 ｜ http://127.0.0.1:8080")
    expect(local).not.toContain("key ")
  })

  test("isInferTarget：形态判定（正例/反例）", () => {
    expect(isInferTarget({ name: "local", kind: "local", baseUrl: "http://127.0.0.1:8080" })).toBe(true)
    expect(isInferTarget({ name: "x", kind: "remote", baseUrl: "http://x", apiKey: "k", model: "m", note: "n" })).toBe(true)
    expect(isInferTarget(null)).toBe(false)
    expect(isInferTarget("local")).toBe(false)
    expect(isInferTarget({ name: "x", kind: "other", baseUrl: "http://x" })).toBe(false)
    expect(isInferTarget({ name: "x", kind: "remote" })).toBe(false)
    expect(isInferTarget({ name: "x", kind: "remote", baseUrl: "http://x", apiKey: 1 })).toBe(false)
  })
})

// ── probeTarget ───────────────────────────────────────────────────────────

describe("probeTarget", () => {
  test("假端点：解析 /health + /props 的 n_ctx、slots 与模型名，并带上 Authorization 头", async () => {
    const seen: Array<{ path: string; auth: string | null }> = []
    const server = Bun.serve({
      port: 0,
      fetch: (req) => {
        const url = new URL(req.url)
        seen.push({ path: url.pathname, auth: req.headers.get("authorization") })
        if (url.pathname === "/health") return Response.json({ status: "ok" })
        if (url.pathname === "/props") {
          return Response.json({
            default_generation_settings: { n_ctx: 32768 },
            total_slots: 4,
            model_path: "C:/models/qwen-agentworld.gguf",
          })
        }
        return new Response("not found", { status: 404 })
      },
    })
    try {
      const t: InferTarget = { name: "fake", kind: "remote", baseUrl: `http://127.0.0.1:${server.port}`, apiKey: "sk-probe-123456" }
      const p = await probeTarget(t)
      expect(p.ok).toBe(true)
      expect(p.health_status).toBe(200)
      expect(p.n_ctx).toBe(32768)
      expect(p.total_slots).toBe(4)
      expect(p.model_path).toBe("C:/models/qwen-agentworld.gguf")
      expect(seen.map((s) => s.path)).toEqual(["/health", "/props"])
      expect(seen.every((s) => s.auth === "Bearer sk-probe-123456")).toBe(true)
    } finally {
      server.stop(true)
    }
  })

  test("端点不可达：返回 ok=false 与错误说明（不抛异常）", async () => {
    // 占一个端口后立刻停掉，得到一个大概率无人监听的端口
    const tmpServer = Bun.serve({ port: 0, fetch: () => new Response("x") })
    const deadPort = tmpServer.port
    tmpServer.stop(true)
    const p = await probeTarget({ name: "dead", kind: "remote", baseUrl: `http://127.0.0.1:${deadPort}` }, { timeoutMs: 800 })
    expect(p.ok).toBe(false)
    expect(String(p.error).length).toBeGreaterThan(0)
  })
})

// ── targets 工具 ──────────────────────────────────────────────────────────

describe("targets 工具", () => {
  const envWithTargets = (home: string): Record<string, string> => ({
    LOCAL_INFER_HOME: home,
    LOCAL_INFER_PORT: "18081",
    LOCAL_INFER_TARGETS: JSON.stringify([
      { name: "lan", base_url: "http://192.168.1.20:8080", api_key: "sk-lan-abcdef123", model: "qwen2.5", note: "备机" },
      { name: "broken", base_url: "192.168.1.21:8080" },
    ]),
  })

  test("列出形态：local + 命名目标，密钥掩码，坏条目进解析告警", async () => {
    const ctx = makeCtx(envWithTargets(makeHome()))
    const r = await tools.targets.execute({}, ctx)
    expect(r.output).toContain("推理目标（2 个：1 本机 + 1 远端）")
    expect(r.output).toContain("· local ｜ 本机 ｜ http://127.0.0.1:18081")
    expect(r.output).toContain("· lan ｜ 远端 ｜ http://192.168.1.20:8080")
    expect(r.output).toContain("key sk-***123")
    expect(r.output).not.toContain("sk-lan-abcdef123")
    expect(r.output).toContain("LOCAL_INFER_TARGETS 解析告警（1 条）")
    expect(r.output).toContain("不是 http(s) 端点")
    expect(r.output).not.toContain("探活：")

    const d = r.data as { count: number; targets: Array<Record<string, unknown>>; parse_errors: string[]; probes?: unknown }
    expect(d.count).toBe(2)
    expect(d.parse_errors.length).toBe(1)
    expect(d.probes).toBeUndefined()
    const lan = d.targets.find((t) => t.name === "lan")
    expect(lan).toMatchObject({ kind: "remote", base_url: "http://192.168.1.20:8080", has_api_key: true, api_key_masked: "sk-***123" })
    expect(JSON.stringify(d)).not.toContain("sk-lan-abcdef123")
  })

  test("probe=true：逐个探活并给出一行结论（n_ctx / slots / 模型）", async () => {
    const server = Bun.serve({
      port: 0,
      fetch: (req) => {
        const url = new URL(req.url)
        if (url.pathname === "/health") return Response.json({ status: "ok" })
        if (url.pathname === "/props") return Response.json({ default_generation_settings: { n_ctx: 8192 }, total_slots: 2, model_path: "/models/live.gguf" })
        return new Response("nope", { status: 404 })
      },
    })
    // 直连 URL 形态（临时端口）：用一个「活的」和一个「死的」目标验证两种结论
    const deadServer = Bun.serve({ port: 0, fetch: () => new Response("x") })
    const deadPort = deadServer.port
    deadServer.stop(true)

    try {
      const ctx = makeCtx({
        LOCAL_INFER_HOME: makeHome(),
        LOCAL_INFER_TARGETS: JSON.stringify([{ name: "live", base_url: `http://127.0.0.1:${server.port}` }, { name: "dead", base_url: `http://127.0.0.1:${deadPort}` }]),
      })
      const r = await tools.targets.execute({ probe: true }, ctx)
      expect(r.output).toContain("n_ctx=8192")
      expect(r.output).toContain("slots=2")
      expect(r.output).toContain("模型 live.gguf")
      expect(r.output).toContain("不可用（")

      const d = r.data as { probes: Array<{ name: string; ok: boolean; n_ctx?: number; total_slots?: number }> }
      expect(d.probes.find((p) => p.name === "live")).toMatchObject({ ok: true, n_ctx: 8192, total_slots: 2 })
      expect(d.probes.find((p) => p.name === "dead")?.ok).toBe(false)
    } finally {
      server.stop(true)
    }
  })

  test("声明：只读工具（safeMode=true）、免审批、参数 schema 只含 probe", () => {
    const t = tools.targets
    expect(t.name).toBe("targets")
    expect(t.safeMode).toBe(true)
    expect(requiresApproval).toEqual({})
    expect(t.parameters.type).toBe("object")
    expect(Object.keys(t.parameters.properties)).toEqual(["probe"])
    expect(String(t.description)).toContain("只读")
  })
})
