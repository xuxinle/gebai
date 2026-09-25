/**
 * tasks 工具测试：用 Bun.serve 起 mock llama-server（/health、/props、/v1/chat/completions），
 * 在**无 GPU 的本机**上跑通 generate / batch / jobs 全链路——真实 HTTP、真实落盘、真实并发调度。
 */
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join } from "node:path"
import { afterAll, beforeAll, describe, expect, test } from "bun:test"
import type { ToolContext } from "@gebai/sdk"
import { tools, pidAliveCmd, pidAliveVerdict } from "./tasks"

// ── mock 端点 ─────────────────────────────────────────────────────────────

let server: ReturnType<typeof Bun.serve>
let port = 0
let requests: Array<Record<string, unknown>> = []
/** 在飞请求数与峰值（校验并发限流：最多 concurrency 条同时打向服务端）。 */
let inFlight = 0
let maxInFlight = 0

/** schema 示例：要求 {name: string, n?: number}——mock 对含 BAD 的输入返回违例结构。 */
const SCHEMA = { type: "object", required: ["name"], properties: { name: { type: "string" }, n: { type: "number" } } }

beforeAll(() => {
  server = Bun.serve({
    port: 0,
    fetch: async (req) => {
      const url = new URL(req.url)
      if (url.pathname === "/health") return Response.json({ status: "ok" })
      if (url.pathname === "/props") {
        return Response.json({ default_generation_settings: { n_ctx: 32768 }, total_slots: 1, model_path: "C:/models/x.gguf" })
      }
      if (url.pathname === "/v1/chat/completions") {
        const body = (await req.json()) as Record<string, unknown>
        requests.push(body)
        const msgs = (body.messages ?? []) as Array<{ content?: string }>
        const text = msgs.map((m) => String(m.content ?? "")).join("\n")
        const rf = body.response_format as { type?: string } | undefined
        if (text.includes("SLOWER")) {
          inFlight++
          maxInFlight = Math.max(maxInFlight, inFlight)
          await new Promise((r) => setTimeout(r, 150))
          inFlight--
        } else if (text.includes("SLOW")) {
          inFlight++
          maxInFlight = Math.max(maxInFlight, inFlight)
          await new Promise((r) => setTimeout(r, 30))
          inFlight--
        }
        let content: string
        if (rf?.type === "json_schema") content = text.includes("BAD") ? '{"nope":1}' : '{"name":"x","n":3}'
        else if (rf?.type === "json_object") content = '{"free":"form"}'
        else if (typeof body.grammar === "string") content = '{"grammar":true}'
        else content = `回复：${text.slice(0, 40)}`
        return Response.json({
          choices: [{ message: { role: "assistant", content, reasoning_content: "think-若干思维链" } }],
          usage: { prompt_tokens: 5, completion_tokens: 7, total_tokens: 12 },
          timings: { predicted_n: 7, predicted_ms: 70 },
        })
      }
      return new Response("not found", { status: 404 })
    },
  })
  port = server.port ?? 0
})

afterAll(() => server?.stop(true))

// ── 夹具 ──────────────────────────────────────────────────────────────────

const tmp = mkdtempSync(join(tmpdir(), "gebai-infer-tasks-"))
afterAll(() => rmSync(tmp, { recursive: true, force: true }))

/** 独立 infer home：带档位定义（parallel=1，用于并发夹取用例）。 */
function makeHome(): string {
  const home = join(tmp, `home-${Math.random().toString(36).slice(2)}`)
  mkdirSync(join(home, "config"), { recursive: true })
  writeFileSync(
    join(home, "config", "profiles.json"),
    JSON.stringify({
      engine_dir: "vendor/engine",
      default_profile: "fast",
      profiles: { fast: { model: "m.gguf", parallel: 1 }, wide: { model: "m.gguf", parallel: 8 } },
    }),
  )
  return home
}

/** 精简 ToolContext：本测试只用 env / signal / home（不碰文件工具与命令执行）。 */
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
    writeFile: async (p, c) => {
      mkdirSync(dirname(p), { recursive: true })
      writeFileSync(p, c)
    },
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

interface GenerateData {
  ok: boolean
  content: string
  json?: unknown
  json_errors?: string[]
  structured_via?: string
  attempts: number
  decode_tps?: number
  usage?: { completion_tokens?: number }
}

interface BatchData {
  job_id: string
  total: number
  processed: number
  ok: number
  failed: number
  skipped: number
  remaining: number
  results_file: string
  concurrency: number
  results: Array<{ id: string; ok: boolean }>
}

describe("generate", () => {
  test("结构化输出：schema 约束成功 → json 解析 + 实测吞吐", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    requests = []
    const r = await tools.generate.execute({ prompt: "VALID 请给出结果", schema: SCHEMA, enable_thinking: false }, ctx)
    const d = r.data as GenerateData
    expect(d.ok).toBe(true)
    expect(d.structured_via).toBe("schema")
    expect(d.json).toEqual({ name: "x", n: 3 })
    expect(d.json_errors).toBeUndefined()
    expect(d.usage?.completion_tokens).toBe(7)
    // 服务端 timings：7 token / 70 ms → 100 t/s
    expect(d.decode_tps).toBeCloseTo(100, 3)
    expect(r.output).toContain("结构化输出（约束来源 schema）")
    expect(r.output).toContain("reasoning_content")
    // 请求体确实带上了 JSON Schema 约束与关闭思维链
    expect((requests[0].response_format as { type: string }).type).toBe("json_schema")
    expect(requests[0].chat_template_kwargs).toEqual({ enable_thinking: false })
  })

  test("结构化校验失败 → 回灌错误重试一次后仍失败（attempts=2，保留 json_errors）", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    requests = []
    const r = await tools.generate.execute({ prompt: "BAD 请给出结果", schema: SCHEMA }, ctx)
    const d = r.data as GenerateData
    expect(d.attempts).toBe(2)
    expect(d.json_errors?.length).toBeGreaterThan(0)
    expect(r.output).toContain("结构化输出校验未通过")
    // 第二次请求把校验错误回灌给了模型
    expect(requests.length).toBe(2)
    expect(JSON.stringify(requests[1])).toContain("上一次输出无法作为符合要求的 JSON 使用")
  })

  test("未给 prompt/messages 时给出用法提示（不发请求）", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    requests = []
    const r = await tools.generate.execute({}, ctx)
    expect(r.output).toContain("需要 prompt 或 messages")
    expect(requests.length).toBe(0)
  })

  test("grammar / json_object 两种降级约束路径", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    requests = []
    const g = await tools.generate.execute({ prompt: "语法约束", grammar: 'root ::= "{}"' }, ctx)
    expect((g.data as GenerateData).structured_via).toBe("grammar")
    expect((g.data as GenerateData).json).toEqual({ grammar: true })
    expect(requests[0].grammar).toBe('root ::= "{}"')

    const j = await tools.generate.execute({ prompt: "只要合法 JSON", json_object: true }, ctx)
    expect((j.data as GenerateData).structured_via).toBe("json_object")
    expect((j.data as GenerateData).json).toEqual({ free: "form" })
  })
})

describe("batch", () => {
  const ITEMS = [
    { id: "q1", prompt: "VALID 一" },
    { id: "q2", prompt: "VALID 二" },
    { id: "q3", prompt: "BAD 三" },
  ]

  test("批量提交：三件套落盘 + 逐条结果 + 失败计数", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    const r = await tools.batch.execute({ items: ITEMS, schema: SCHEMA, job_id: "job-t1" }, ctx)
    const d = r.data as BatchData
    expect(d.ok).toBe(2)
    expect(d.failed).toBe(1)
    expect(d.skipped).toBe(0)

    const dir = join(home, "bench", "runs", "job-t1")
    const itemsLines = readFileSync(join(dir, "items.jsonl"), "utf-8").trim().split("\n")
    const resultLines = readFileSync(join(dir, "results.jsonl"), "utf-8").trim().split("\n")
    expect(itemsLines.length).toBe(3)
    expect(resultLines.length).toBe(3)
    expect(JSON.parse(resultLines[0]).id).toBe("q1")
    expect(JSON.parse(resultLines[0]).json).toEqual({ name: "x", n: 3 })
    expect(JSON.parse(resultLines[2]).ok).toBe(false)

    const job = JSON.parse(readFileSync(join(dir, "job.json"), "utf-8")) as Record<string, unknown>
    expect(job.done).toBe(3)
    expect(job.failed).toBe(1)
    expect(job.state).toBe("done")
    expect(job.structured).toBe(true)
    expect(job.results_file).toBe(join(dir, "results.jsonl"))
  })

  test("同一 job_id 续跑：跳过已成功条目，只处理剩余", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    await tools.batch.execute({ items: ITEMS, schema: SCHEMA, job_id: "job-r" }, ctx)
    const r2 = await tools.batch.execute({ items: ITEMS, schema: SCHEMA, job_id: "job-r" }, ctx)
    const d2 = r2.data as BatchData
    expect(d2.skipped).toBe(2)
    expect(d2.processed).toBe(1) // 只剩失败的 q3
    expect(r2.output).toContain("跳过 2")
  })

  test("items_file（JSONL）作为条目来源", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    mkdirSync(join(home, "bench"), { recursive: true })
    const file = join(home, "bench", "questions.jsonl")
    writeFileSync(file, ['{"id":"a","prompt":"VALID 甲"}', "", "// 注释行", '{"id":"b","prompt":"VALID 乙"}'].join("\n"))

    const r = await tools.batch.execute({ items_file: "bench/questions.jsonl", schema: SCHEMA, job_id: "job-file" }, ctx)
    const d = r.data as BatchData
    expect(d.processed).toBe(2)
    expect(d.ok).toBe(2)
    const lines = readFileSync(join(home, "bench", "runs", "job-file", "results.jsonl"), "utf-8").trim().split("\n")
    expect(lines.length).toBe(2)
    expect(JSON.parse(lines[1]).id).toBe("b")
  })

  test("items_file 带 BOM / CRLF（Windows PowerShell 写出的文件）也能读", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    mkdirSync(join(home, "bench"), { recursive: true })
    const file = join(home, "bench", "bom.jsonl")
    writeFileSync(file, "\uFEFF" + ['{"id":"a","prompt":"VALID 甲"}', '{"id":"b","prompt":"VALID 乙"}'].join("\r\n"))
    const r = await tools.batch.execute({ items_file: "bench/bom.jsonl", schema: SCHEMA, job_id: "job-bom" }, ctx)
    const d = r.data as BatchData
    expect(d.total).toBe(2)
    expect(d.ok).toBe(2)
  })

  test("max_items 分片：本次只处理前 N 条，输出提示剩余量", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    const r = await tools.batch.execute({ items: ITEMS, schema: SCHEMA, job_id: "job-slice", max_items: 2 }, ctx)
    const d = r.data as BatchData
    expect(d.processed).toBe(2)
    expect(d.remaining).toBe(1)
    expect(r.output).toContain("仍有 1 条未处理")
  })

  test("并发夹取：超过档位 parallel 时按档位值下调并说明", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    const r = await tools.batch.execute({ items: [{ id: "c1", prompt: "VALID 并发" }], schema: SCHEMA, job_id: "job-conc", concurrency: 4 }, ctx)
    const d = r.data as BatchData
    expect(d.concurrency).toBe(1)
    expect(r.output).toContain("已夹取到 1")
  })

  test("LOCAL_INFER_BATCH_MAX_ITEMS 兜底分片（未给 max_items 时防一次提交占死会话）", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port), LOCAL_INFER_BATCH_MAX_ITEMS: "1" })
    const r = await tools.batch.execute({ items: ITEMS, schema: SCHEMA, job_id: "job-envcap" }, ctx)
    const d = r.data as BatchData
    expect(d.processed).toBe(1)
    expect(d.remaining).toBe(2)
  })

  test("条目缺失时给出用法提示", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    const r = await tools.batch.execute({}, ctx)
    expect(r.output).toContain("需要 items 数组或 items_file")
  })

  test("并发限流：concurrency=2 时同时最多 2 条在飞", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port), LOCAL_INFER_PROFILE: "wide" })
    maxInFlight = 0
    const items = [1, 2, 3, 4].map((i) => ({ id: `p${i}`, prompt: "VALID SLOW" }))
    const r = await tools.batch.execute({ items, schema: SCHEMA, job_id: "job-par", concurrency: 2 }, ctx)
    expect((r.data as BatchData).concurrency).toBe(2)
    expect(maxInFlight).toBe(2)
    expect((r.data as BatchData).ok).toBe(4)
  })

  test("输入数据集与任务目录留存不同时给出提示（仍按 id 续跑）", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    await tools.batch.execute({ items: [{ id: "q1", prompt: "VALID" }], schema: SCHEMA, job_id: "job-chg" }, ctx)
    const r = await tools.batch.execute(
      { items: [{ id: "q1", prompt: "VALID" }, { id: "q9", prompt: "VALID 新增" }], schema: SCHEMA, job_id: "job-chg" },
      ctx,
    )
    expect(r.output).toContain("与任务目录中留存的 items.jsonl 不同")
    expect((r.data as BatchData).skipped).toBe(1)
    expect((r.data as BatchData).processed).toBe(1)
  })
})

describe("jobs", () => {
  test("list → status → results（含 only=failed 过滤）", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    await tools.batch.execute(
      { items: [{ id: "k1", prompt: "VALID" }, { id: "k2", prompt: "BAD" }], schema: SCHEMA, job_id: "job-j1" },
      ctx,
    )

    const list = await tools.jobs.execute({ action: "list" }, ctx)
    expect(list.output).toContain("job-j1")
    expect(list.output).toContain("2/2 条")

    const status = await tools.jobs.execute({ action: "status", job_id: "job-j1" }, ctx)
    expect(status.output).toContain("已产出 2 条（成功 1 / 失败 1）")
    expect(status.output).toContain("结构化解析/校验失败 1 条")

    const failed = await tools.jobs.execute({ action: "results", job_id: "job-j1", only: "failed" }, ctx)
    expect(failed.output).toContain("k2")
    expect(failed.output).not.toContain("── k1")

    const all = await tools.jobs.execute({ action: "results", job_id: "job-j1", limit: 10 }, ctx)
    const d = all.data as { results: Array<{ id: string }> }
    expect(d.results.map((r) => r.id)).toEqual(["k1", "k2"])
  })

  test("cancel 写回状态；未知 job_id 给出可操作提示", async () => {
    const home = makeHome()
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    await tools.batch.execute({ items: [{ id: "z1", prompt: "VALID" }], schema: SCHEMA, job_id: "job-cancel" }, ctx)
    const r = await tools.jobs.execute({ action: "cancel", job_id: "job-cancel" }, ctx)
    expect(r.output).toContain("已标记为 cancelled")
    const job = JSON.parse(readFileSync(join(home, "bench", "runs", "job-cancel", "job.json"), "utf-8")) as { state: string }
    expect(job.state).toBe("cancelled")

    const missing = await tools.jobs.execute({ action: "status", job_id: "nope" }, ctx)
    expect(missing.output).toContain("未找到任务 nope")

    const noId = await tools.jobs.execute({ action: "results" }, ctx)
    expect(noId.output).toContain("需要 job_id")
  })
})

// ── 后台批次：存活判定与取消 ─────────────────────────────────────────────

const jobFile = (home: string, jobId: string): string => join(home, "bench", "runs", jobId, "job.json")
const resultsOf = (home: string, jobId: string): string => join(home, "bench", "runs", jobId, "results.jsonl")

function readJob(home: string, jobId: string): Record<string, unknown> {
  return JSON.parse(readFileSync(jobFile(home, jobId), "utf-8")) as Record<string, unknown>
}

function writeJob(home: string, jobId: string, patch: Record<string, unknown>): void {
  writeFileSync(jobFile(home, jobId), JSON.stringify({ ...readJob(home, jobId), ...patch }, null, 2))
}

/** 结果行数（只数非空行）。 */
function resultLines(home: string, jobId: string): number {
  if (!existsSync(resultsOf(home, jobId))) return 0
  const text = readFileSync(resultsOf(home, jobId), "utf-8").trim()
  return text ? text.split("\n").length : 0
}

/** 轮询到任务离开 running（后台批次收尾）。 */
async function pollJob(home: string, jobId: string, timeoutMs = 20000): Promise<Record<string, unknown>> {
  const deadline = Date.now() + timeoutMs
  for (;;) {
    const j = readJob(home, jobId)
    if (j.state !== "running" || Date.now() > deadline) return j
    await new Promise((r) => setTimeout(r, 50))
  }
}

/**
 * 带存活探活的 ctx：`alivePids` 里的 PID 判为存活，其余判为已退出。
 * 应答同时满足两个平台分支的判定（POSIX 退出码 0；win32 tasklist 输出含引号包裹的 PID），
 * 因此断言不随宿主平台漂移。
 */
function livenessCtx(env: Record<string, string>, alivePids: number[]): ToolContext {
  const ctx = makeCtx(env)
  ctx.runCommand = async (cmd: string) => {
    const m = cmd.match(/PID eq (\d+)/)
    const pid = m ? Number(m[1]) : Number.NaN
    if (Number.isFinite(pid) && alivePids.includes(pid)) {
      return { stdout: `"llama-server.exe","${pid}","Console","1","1,234 K"\r\n`, stderr: "", code: 0 }
    }
    return { stdout: "", stderr: "", code: 1 }
  }
  return ctx
}

describe("后台批次（background=true）", () => {
  test("立即返回并在进程内跑完：终态与产物齐全", async () => {
    const home = makeHome()
    const ctx = livenessCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) }, [process.pid])
    const items = [1, 2, 3, 4].map((i) => ({ id: `bg${i}`, prompt: "VALID SLOWER" }))

    const t0 = Date.now()
    const r = await tools.batch.execute({ items, schema: SCHEMA, job_id: "job-bg", background: true }, ctx)
    const callMs = Date.now() - t0

    expect(r.output).toContain("已在后台启动")
    expect(r.output).toContain('action="status"')
    const d = r.data as BatchData & { background: boolean; queued: number }
    expect(d.background).toBe(true)
    expect(d.queued).toBe(4)
    // 立即返回：远快于「4 条 × 150 ms」的批次总时长（同步执行至少要 600 ms）
    expect(callMs).toBeLessThan(400)

    // 返回时批次尚未跑完，job.json 已记下后台与属主字段
    const early = readJob(home, "job-bg")
    expect(early.state).toBe("running")
    expect(early.background).toBe(true)
    expect(early.owner_pid).toBe(process.pid)

    const final = await pollJob(home, "job-bg")
    expect(final.state).toBe("done")
    expect(final.done).toBe(4)
    expect(resultLines(home, "job-bg")).toBe(4)

    const status = await tools.jobs.execute({ action: "status", job_id: "job-bg" }, ctx)
    expect(status.output).toContain("后台执行")
    expect(status.output).toContain("已产出 4 条")
  })

  test("同一 job_id 重复提交被拒（防两个执行体交叉写产物）", async () => {
    const home = makeHome()
    const ctx = livenessCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) }, [process.pid])
    const items = [1, 2, 3].map((i) => ({ id: `r${i}`, prompt: "VALID SLOWER" }))

    await tools.batch.execute({ items, schema: SCHEMA, job_id: "job-dup", background: true }, ctx)
    // 带不同数据集的重复提交：不仅要被拒，还不能改写在跑任务的输入副本
    const bg = await tools.batch.execute({ items: [{ id: "zz", prompt: "VALID 另一份数据" }], schema: SCHEMA, job_id: "job-dup", background: true }, ctx)
    expect(bg.output).toContain("正在本进程执行中")
    expect((bg.data as { rejected?: string }).rejected).toBe("already_running")
    const sync = await tools.batch.execute({ items, schema: SCHEMA, job_id: "job-dup" }, ctx)
    expect(sync.output).toContain("正在本进程执行中")
    const itemsOnDisk = readFileSync(join(home, "bench", "runs", "job-dup", "items.jsonl"), "utf-8")
    expect(itemsOnDisk.trim().split("\n").length).toBe(3)
    expect(itemsOnDisk).not.toContain("另一份数据")

    // 批次结束后不再拦截；已成功的条目被 resume 跳过
    await pollJob(home, "job-dup")
    const after = await tools.batch.execute({ items, schema: SCHEMA, job_id: "job-dup" }, ctx)
    expect(after.output).not.toContain("正在本进程执行中")
    expect(after.output).toContain("没有待处理条目")
  })

  test("服务重启语义：属主进程已退出 → 改判已中断并给出续跑指引（幂等）", async () => {
    const home = makeHome()
    // 探活一律判「已退出」：模拟另一个已消失的服务进程
    const ctx = livenessCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) }, [])
    await tools.batch.execute({ items: [{ id: "d1", prompt: "VALID" }], schema: SCHEMA, job_id: "job-dead" }, ctx)
    writeJob(home, "job-dead", { state: "running", background: true, owner_pid: 999999, finished_at: undefined, phase: "running（后台）" })

    const r = await tools.jobs.execute({ action: "status", job_id: "job-dead" }, ctx)
    expect(r.output).toContain("已中断（执行进程 PID 999999 已退出）")
    expect(r.output).toContain("resume=true")
    expect(readJob(home, "job-dead").state).toBe("interrupted")

    // 幂等：再查一次不再改写、结论一致
    const again = await tools.jobs.execute({ action: "status", job_id: "job-dead" }, ctx)
    expect(again.output).toContain("状态 interrupted")
  })

  test("list 标出后台/同步与存活状态", async () => {
    const home = makeHome()
    const ctx = livenessCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) }, [])
    await tools.batch.execute({ items: [{ id: "s1", prompt: "VALID" }], schema: SCHEMA, job_id: "job-sync" }, ctx)
    writeJob(home, "job-sync", { state: "running", background: true, owner_pid: 999999, finished_at: undefined })

    const list = await tools.jobs.execute({ action: "list" }, ctx)
    expect(list.output).toContain("后台")
    expect(list.output).toContain("已中断（执行进程已退出，可 resume 续跑）")
    const d = list.data as { jobs: Array<{ job_id: string }> }
    expect(d.jobs.map((j) => j.job_id)).toContain("job-sync")
  })

  test("心跳过期：状态 running 且超过 10 分钟未更新 → 提示可能已卡住", async () => {
    const home = makeHome()
    const ctx = livenessCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) }, [process.pid])
    await tools.batch.execute({ items: [{ id: "h1", prompt: "VALID" }], schema: SCHEMA, job_id: "job-hb" }, ctx)
    writeJob(home, "job-hb", {
      state: "running",
      background: true,
      owner_pid: process.pid,
      phase: "running（后台）",
      finished_at: undefined,
      updated_at: new Date(Date.now() - 20 * 60 * 1000).toISOString(),
    })

    const r = await tools.jobs.execute({ action: "status", job_id: "job-hb" }, ctx)
    expect(r.output).toContain("心跳")
    expect(r.output).toContain("可能已卡住")
  })

  test("旧版 job.json（无 background/owner_pid）查询不报错，running 时明示无法确认", async () => {
    const home = makeHome()
    const ctx = livenessCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) }, [process.pid])
    await tools.batch.execute({ items: [{ id: "l1", prompt: "VALID" }], schema: SCHEMA, job_id: "job-old" }, ctx)
    const jp = jobFile(home, "job-old")
    const j = JSON.parse(readFileSync(jp, "utf-8")) as Record<string, unknown>
    delete j.background
    delete j.owner_pid
    writeFileSync(jp, JSON.stringify(j, null, 2))

    const done = await tools.jobs.execute({ action: "status", job_id: "job-old" }, ctx)
    expect(done.output).toContain("已产出 1 条")
    expect(done.output).toContain("同步执行")

    // 旧记录声称在跑但没有属主 → 如实说明无法确认，不误判为中断
    writeFileSync(jp, JSON.stringify({ ...j, state: "running", finished_at: undefined }, null, 2))
    const running = await tools.jobs.execute({ action: "status", job_id: "job-old" }, ctx)
    expect(running.output).toContain("无属主进程")
  })
})

describe("批次取消（真中止）", () => {
  test("取消在飞的后台批次：状态置 cancelled、剩余条目可 resume 续跑", async () => {
    const home = makeHome()
    const ctx = livenessCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) }, [process.pid])
    const items = [1, 2, 3, 4, 5, 6].map((i) => ({ id: `c${i}`, prompt: "VALID SLOWER" }))

    await tools.batch.execute({ items, schema: SCHEMA, job_id: "job-cancel-bg", background: true }, ctx)
    await new Promise((r) => setTimeout(r, 250))
    const producedBefore = resultLines(home, "job-cancel-bg")

    const cr = await tools.jobs.execute({ action: "cancel", job_id: "job-cancel-bg" }, ctx)
    expect(cr.output).toContain("已请求取消")
    const after = await pollJob(home, "job-cancel-bg")
    expect(after.state).toBe("cancelled")
    const produced = resultLines(home, "job-cancel-bg")
    expect(produced).toBeGreaterThan(0)
    expect(produced).toBeLessThan(6)
    expect(produced).toBeGreaterThanOrEqual(producedBefore)
    // 取消占位项（未开始就被取消，attempts=0）不落盘：产物只含真实产出的条目
    const rows = readFileSync(resultsOf(home, "job-cancel-bg"), "utf-8").trim().split("\n").map((l) => JSON.parse(l) as { id: string; ok: boolean; attempts: number })
    // 取消占位项（未开始就被取消，attempts=0）不落盘；在飞条目被中止则如实记为一次失败
    expect(rows.every((r) => r.attempts >= 1)).toBe(true)
    const okBefore = rows.filter((r) => r.ok).length
    expect(String(after.phase)).toContain("已取消")

    // 续跑：未成功条目继续处理，最终每个 id 都有成功产出
    const rr = await tools.batch.execute({ items, schema: SCHEMA, job_id: "job-cancel-bg", resume: true }, ctx)
    const d = rr.data as BatchData
    expect(d.skipped).toBe(okBefore)
    expect(d.ok).toBe(6 - okBefore)
    const finalRows = readFileSync(resultsOf(home, "job-cancel-bg"), "utf-8").trim().split("\n").map((l) => JSON.parse(l) as { id: string; ok: boolean })
    expect(new Set(finalRows.filter((r) => r.ok).map((r) => r.id)).size).toBe(6)
    expect(readJob(home, "job-cancel-bg").state).toBe("done")
  })

  test("已完成的任务：仍按「仅标记」语义（不误报为中止）", async () => {
    const home = makeHome()
    const ctx = livenessCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) }, [process.pid])
    await tools.batch.execute({ items: [{ id: "f1", prompt: "VALID" }], schema: SCHEMA, job_id: "job-done-cancel" }, ctx)
    const r = await tools.jobs.execute({ action: "cancel", job_id: "job-done-cancel" }, ctx)
    expect(r.output).toContain("已标记为 cancelled")
    expect(r.output).toContain("未在本进程运行")
    expect(readJob(home, "job-done-cancel").state).toBe("cancelled")
  })
})

describe("批次属主探活（平台纯函数）", () => {
  test("win32：tasklist 精确过滤，按输出判定", () => {
    expect(pidAliveCmd(4321, "win32")).toContain("PID eq 4321")
    expect(pidAliveVerdict(4321, { stdout: '"llama-server.exe","4321","Console","1","1,234 K"', code: 0 }, "win32")).toBe(true)
    expect(pidAliveVerdict(4321, { stdout: "信息: 没有运行的任务匹配指定标准。", code: 1 }, "win32")).toBe(false)
    // 输出里出现其它 PID 不算命中
    expect(pidAliveVerdict(4321, { stdout: '"llama-server.exe","43210","Console"', code: 0 }, "win32")).toBe(false)
  })

  test("POSIX：kill -0 不发信号，按退出码判定", () => {
    expect(pidAliveCmd(4321, "linux")).toBe("kill -0 4321 2>/dev/null")
    expect(pidAliveVerdict(4321, { stdout: "", code: 0 }, "linux")).toBe(true)
    expect(pidAliveVerdict(4321, { stdout: "", code: 1 }, "linux")).toBe(false)
  })
})

// ── 推理目标（target）：命名目标 / 直连 URL / 未知目标 ────────────────────

describe("推理目标（target）", () => {
  /** mock「远端」OpenAI 兼容端点：记录 Authorization 头，供鉴权透传断言（模拟局域网/云端端点）。 */
  let remote: ReturnType<typeof Bun.serve>
  let remotePort = 0
  let auths: Array<string | null> = []

  beforeAll(() => {
    remote = Bun.serve({
      port: 0,
      fetch: async (req) => {
        const url = new URL(req.url)
        if (url.pathname === "/health") return Response.json({ status: "ok" })
        if (url.pathname === "/props") {
          return Response.json({ default_generation_settings: { n_ctx: 4096 }, total_slots: 2, model_path: "/models/remote.gguf" })
        }
        if (url.pathname === "/v1/chat/completions") {
          auths.push(req.headers.get("authorization"))
          const body = (await req.json()) as Record<string, unknown>
          const msgs = (body.messages ?? []) as Array<{ content?: string }>
          const rf = body.response_format as { type?: string } | undefined
          const content = rf?.type === "json_schema" ? '{"name":"remote","n":1}' : `远端回复：${msgs.map((m) => String(m.content ?? "")).join(" ")}`
          return Response.json({
            choices: [{ message: { role: "assistant", content } }],
            usage: { prompt_tokens: 2, completion_tokens: 3, total_tokens: 5 },
            timings: { predicted_n: 3, predicted_ms: 30 },
          })
        }
        return new Response("not found", { status: 404 })
      },
    })
    remotePort = remote.port ?? 0
  })
  afterAll(() => remote?.stop(true))

  /** 带一个命名目标（lan → mock 远端）的 ctx 环境。 */
  const lanEnv = (home: string, extra: Record<string, string> = {}): Record<string, string> => ({
    LOCAL_INFER_HOME: home,
    LOCAL_INFER_PORT: String(port),
    LOCAL_INFER_TARGETS: JSON.stringify([
      { name: "lan", base_url: `http://127.0.0.1:${remotePort}`, api_key: "sk-lan-abcdef123456", note: "模拟局域网" },
    ]),
    ...extra,
  })

  test("generate 指定命名目标：请求打到该端点、带上 Authorization 头、输出掩码密钥", async () => {
    const home = makeHome()
    auths = []
    const ctx = makeCtx(lanEnv(home))
    const r = await tools.generate.execute({ prompt: "你好远端", target: "lan" }, ctx)
    const d = r.data as GenerateData & { target?: { name: string; kind: string; base_url: string }; target_summary?: string }

    expect(d.ok).toBe(true)
    expect(d.content).toContain("远端回复")
    expect(auths).toEqual(["Bearer sk-lan-abcdef123456"])
    expect(d.target).toMatchObject({ name: "lan", kind: "remote", base_url: `http://127.0.0.1:${remotePort}` })
    // 输出里是掩码形态，密钥原文绝不出现在人读文本里
    expect(r.output).toContain("目标 lan ｜ 远端")
    expect(r.output).toContain("key sk-***456")
    expect(r.output).not.toContain("sk-lan-abcdef123456")
    // 探活（远端 /props n_ctx=4096）→ 条目长度预算提示
    expect(r.output).toContain("n_ctx=4096")
    expect(r.output).toContain("单条 prompt 约可用")
  })

  test("api_key 参数覆盖命名目标的密钥；直连 URL 也能直接用（不配 LOCAL_INFER_TARGETS）", async () => {
    const home = makeHome()
    auths = []
    const ctx = makeCtx(lanEnv(home))

    await tools.generate.execute({ prompt: "覆盖密钥", target: "lan", api_key: "sk-explicit-999" }, ctx)
    expect(auths).toEqual(["Bearer sk-explicit-999"])

    // 直连目标 + 显式密钥：环境里不配任何命名目标也能跑通
    auths = []
    const bare = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    const r = await tools.generate.execute({ prompt: "直连测试", target: `http://127.0.0.1:${remotePort}`, api_key: "sk-direct-123456" }, bare)
    expect(auths).toEqual(["Bearer sk-direct-123456"])
    expect((r.data as GenerateData).content).toContain("远端回复")
    expect(r.output).toContain(`远端 ｜ http://127.0.0.1:${remotePort}`)
  })

  test("batch 在命名目标上跑：job.json 落 target/target_base_url（不含密钥），输出显示目标", async () => {
    const home = makeHome()
    auths = []
    const ctx = makeCtx(lanEnv(home))
    const r = await tools.batch.execute(
      { items: [{ id: "r1", prompt: "甲" }, { id: "r2", prompt: "乙" }], job_id: "job-remote", target: "lan" },
      ctx,
    )
    const d = r.data as BatchData & { target?: string; target_base_url?: string }

    expect(d.ok).toBe(2)
    expect(d.target).toBe("lan")
    expect(d.target_base_url).toBe(`http://127.0.0.1:${remotePort}`)
    expect(auths).toEqual(["Bearer sk-lan-abcdef123456", "Bearer sk-lan-abcdef123456"])
    expect(r.output).toContain("目标 lan")

    const job = readJob(home, "job-remote")
    expect(job.target).toBe("lan")
    expect(job.target_base_url).toBe(`http://127.0.0.1:${remotePort}`)
    expect(JSON.stringify(job)).not.toContain("sk-lan-abcdef123456")
    // 结果也真的来自远端端点（正文是远端 mock 的固定格式）
    expect(readFileSync(resultsOf(home, "job-remote"), "utf-8")).toContain("远端回复")

    // 远端目标不受本机档位夹取（fast 档 parallel=1，但远端并发由服务端 slots 决定）
    const r2 = await tools.batch.execute({ items: [{ id: "c1", prompt: "并发" }], job_id: "job-remote-conc", target: "lan", concurrency: 2 }, ctx)
    expect((r2.data as BatchData).concurrency).toBe(2)
    expect(r2.output).not.toContain("已夹取")
  })

  test("target 缺省或显式 local：仍打本机端点，job.json 记 target=local", async () => {
    const home = makeHome()
    requests = []
    const ctx = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port) })
    const r = await tools.generate.execute({ prompt: "本机测试", target: "local" }, ctx)
    expect((r.data as GenerateData).ok).toBe(true)
    expect(r.output).toContain("目标 local ｜ 本机")
    expect(requests.length).toBe(1)

    await tools.batch.execute({ items: [{ id: "l1", prompt: "甲" }], job_id: "job-local-tgt" }, ctx)
    expect(readJob(home, "job-local-tgt").target).toBe("local")
    expect(String(readJob(home, "job-local-tgt").target_base_url)).toBe(`http://127.0.0.1:${port}`)
  })

  test("jobs 显示目标名；换目标续跑给出提示但仍按本次目标执行", async () => {
    const home = makeHome()
    auths = []
    const ctx = makeCtx(lanEnv(home))
    await tools.batch.execute({ items: [{ id: "t1", prompt: "甲" }], job_id: "job-tgt", target: "lan" }, ctx)

    const list = await tools.jobs.execute({ action: "list" }, ctx)
    expect(list.output).toContain("目标 lan")
    const status = await tools.jobs.execute({ action: "status", job_id: "job-tgt" }, ctx)
    expect(status.output).toContain("目标 lan")
    expect(status.output).toContain(`（http://127.0.0.1:${remotePort}）`)
    const results = await tools.jobs.execute({ action: "results", job_id: "job-tgt" }, ctx)
    expect(results.output).toContain("（目标 lan）")

    // 同一 job_id 换成本机目标续跑：提示目标已变（结果会混用不同端点），但仍按本次 target 执行
    auths = []
    const again = await tools.batch.execute({ items: [{ id: "t1", prompt: "甲" }, { id: "t2", prompt: "乙" }], job_id: "job-tgt", target: "local" }, ctx)
    expect(again.output).toContain("记录的目标是 lan")
    expect(again.output).toContain("本次按 local")
    expect((again.data as BatchData).skipped).toBe(1)
    expect((again.data as BatchData).ok).toBe(1)
    expect(auths.length).toBe(0) // 这次走本机 mock，不再打远端
    expect(readJob(home, "job-tgt").target).toBe("local")
  })

  test("未知 target：可读错误（含可用名与配置指引），不发请求、不落盘", async () => {
    const home = makeHome()
    auths = []
    requests = []
    const ctx = makeCtx(lanEnv(home))

    const g = await tools.generate.execute({ prompt: "x", target: "typo" }, ctx)
    expect(g.output).toContain("推理目标解析失败")
    expect(g.output).toContain('未知推理目标 "typo"')
    expect(g.output).toContain("lan")
    expect(g.output).toContain("LOCAL_INFER_TARGETS")
    expect(g.output).toContain("local_infer_targets")
    expect(requests.length).toBe(0)

    const b = await tools.batch.execute({ items: [{ id: "x1", prompt: "甲" }], job_id: "job-typo", target: "typo" }, ctx)
    expect(b.output).toContain("推理目标解析失败")
    expect(existsSync(join(home, "bench", "runs", "job-typo"))).toBe(false)
    expect(auths.length).toBe(0)

    // 环境变量本身是坏 JSON 时同样只给可读错误（不抛异常）
    const bad = makeCtx({ LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: String(port), LOCAL_INFER_TARGETS: "[{oops}]" })
    const g2 = await tools.generate.execute({ prompt: "x", target: "lan" }, bad)
    expect(g2.output).toContain("不是合法 JSON")
    expect(g2.output).toContain("当前没有有效的命名目标")
  })
})
