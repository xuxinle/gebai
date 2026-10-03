import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { startServer, type ServerHandle } from "./index"
import type { Task } from "@gebai/sdk"
import { unseal } from "./core/support/crypto"

let handle: ServerHandle
const home = mkdtempSync(join(tmpdir(), "gebai-tasks-rest-"))

beforeAll(async () => {
  handle = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
})

afterAll(() => {
  handle.gc?.stop()
  handle.tasks?.stop()
  handle.todos?.stop()
  handle.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

/** 删除临时目录（重试至句柄释放）。
 *  被终止的**脚本型**任务停不掉（脚本不经会话，只能等自身结束），其子进程以任务资源目录为 cwd——
 *  Windows 下 rm 会报 EBUSY，需等脚本退出（`rmSync` 的 maxRetries 在 Bun 下不生效，自行重试）。 */
async function removeDirRetry(dir: string, tries = 30, delayMs = 200): Promise<void> {
  for (let i = 0; i < tries; i++) {
    try {
      rmSync(dir, { recursive: true, force: true })
      return
    } catch (err) {
      if ((err as { code?: string }).code !== "EBUSY" || i === tries - 1) throw err
      await new Promise((r) => setTimeout(r, delayMs))
    }
  }
}

function base(h: ServerHandle = handle) {
  return `http://127.0.0.1:${h.server.port}`
}

function req(method: string, body?: unknown): RequestInit {
  return body === undefined
    ? { method }
    : { method, headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }
}

async function createTask(body: Record<string, unknown>, h: ServerHandle = handle): Promise<Task> {
  const res = await fetch(`${base(h)}/api/v1/tasks`, req("POST", body))
  expect(res.status).toBe(201)
  return (await res.json()) as Task
}

async function getTask(id: string, h: ServerHandle = handle): Promise<Task> {
  const res = await fetch(`${base(h)}/api/v1/tasks/${id}`)
  expect(res.status).toBe(200)
  return (await res.json()) as Task
}

/** 轮询等待（队列执行是异步的：创建/入队返回后任务在后台跑）。 */
async function waitFor<T>(fn: () => Promise<T | null | undefined | false>, timeoutMs = 15_000, stepMs = 50): Promise<T> {
  const deadline = Date.now() + timeoutMs
  for (;;) {
    const v = await fn()
    if (v) return v
    if (Date.now() > deadline) throw new Error("waitFor 超时")
    await new Promise((r) => setTimeout(r, stepMs))
  }
}

describe("tasks REST（统一任务管理面）", () => {
  test("初始为空；创建定时任务落盘用户级 tasks.json", async () => {
    expect(await (await fetch(`${base()}/api/v1/tasks`)).json()).toEqual([])
    const created = await createTask({ kind: "scheduled", runner: "script", name: "rest-task", schedule: "@every 10m", script: "echo rest" })
    expect(String(created.id)).toMatch(/^[a-f0-9]{32}$/)
    expect([created.kind, created.runner, created.enabled, created.state]).toEqual(["scheduled", "script", true, "idle"])
    expect(Number(created.nextRunAt)).toBeGreaterThan(0)
    expect(existsSync(join(home, "users", "admin", "tasks.json")) || existsSync(join(home, "users", "default", "tasks.json"))).toBe(true)
    const listed = (await (await fetch(`${base()}/api/v1/tasks`)).json()) as Array<{ id: string; name?: string; kind: string }>
    expect(listed.map((t) => t.id)).toEqual([created.id])
    expect(listed[0]).toMatchObject({ name: "rest-task", kind: "scheduled" })
    // 类别/状态过滤
    expect(((await (await fetch(`${base()}/api/v1/tasks?kind=manual`)).json()) as unknown[]).length).toBe(0)
    expect(((await (await fetch(`${base()}/api/v1/tasks?kind=scheduled`)).json()) as unknown[]).length).toBe(1)
    expect(((await (await fetch(`${base()}/api/v1/tasks?state=running`)).json()) as unknown[]).length).toBe(0)
    expect(((await (await fetch(`${base()}/api/v1/tasks?state=idle&kind=scheduled`)).json()) as unknown[]).length).toBe(1)
  })

  test("创建校验：非法执行体/缺内容/缺表达式/@at 已过去/非法通知 → 400", async () => {
    const cases: Array<{ body: Record<string, unknown>; expect: string }> = [
      { body: { runner: "wat" }, expect: "无效的执行体" },
      { body: { kind: "scheduled", runner: "script", schedule: "@every 10m" }, expect: "需要 script" },
      { body: { runner: "prompt" }, expect: "需要 prompt" },
      { body: { kind: "scheduled", runner: "script", script: "echo x" }, expect: "缺少执行表达式" },
      { body: { kind: "scheduled", runner: "script", script: "echo x", schedule: "bad expr" }, expect: "无效的 cron 表达式" },
      { body: { kind: "scheduled", runner: "script", script: "echo x", schedule: "@at 2020-01-01T00:00" }, expect: "已过去" },
      { body: { runner: "script", script: "echo x", schedule: "@every 10m", notify: "nope" }, expect: "notify 须为通知通道数组" },
      { body: { runner: "script", script: "echo x", schedule: "@every 10m", notify: [{ type: "feishu", target: "https://evil.example.com/x" }] }, expect: "open.feishu.cn" },
      { body: { runner: "prompt", prompt: "x", agents: ["Bad Name"] }, expect: "无效的子Agent 名" },
    ]
    for (const c of cases) {
      const res = await fetch(`${base()}/api/v1/tasks`, req("POST", c.body))
      expect(res.status).toBe(400)
      expect(String(((await res.json()) as { error: string }).error)).toContain(c.expect)
    }
  })

  test("任务级环境变量：敏感值加密落盘、回显掩码、掩码回传保留原值、非法名 400", async () => {
    const bad = await fetch(`${base()}/api/v1/tasks`, req("POST", { runner: "script", script: "echo x", env: { "1BAD": "v" } }))
    expect(bad.status).toBe(400)
    expect(String(((await bad.json()) as { error: string }).error)).toContain("无效的环境变量名")

    const created = await createTask({
      kind: "manual",
      runner: "script",
      script: "echo $API_TOKEN",
      runNow: false,
      env: { API_TOKEN: "secret-value", PLAIN: "ok" },
    })
    const id = String(created.id)
    // 回显：敏感键固定占位（可往返），非敏感键原样
    const got = await getTask(id)
    expect(got.env!.PLAIN).toBe("ok")
    expect(got.env!.API_TOKEN).toBe("***")
    // 磁盘：敏感键为密文（不回读真值），非敏感键明文
    const diskFile = [join(home, "users", "default", "tasks.json"), join(home, "users", "admin", "tasks.json")].find((f) => existsSync(f))!
    const onDisk = JSON.parse(readFileSync(diskFile, "utf8")) as Array<{ id: string; env?: Record<string, string> }>
    const stored = onDisk.find((t) => t.id === id)!.env!
    expect(stored.API_TOKEN).toMatch(/^enc:v1:/)
    expect(stored.API_TOKEN).not.toContain("secret-value")
    expect(stored.PLAIN).toBe("ok")
    // 掩码回传（前端表单原样回传未改动项）→ 保留原值，真值仍可用
    const echoed = (await (await fetch(`${base()}/api/v1/tasks/${id}`, req("PATCH", { env: { API_TOKEN: "***", PLAIN: "changed" } }))).json()) as { env?: Record<string, string> }
    expect(echoed.env!.PLAIN).toBe("changed")
    const afterEcho = JSON.parse(readFileSync(diskFile, "utf8")) as Array<{ id: string; env?: Record<string, string> }>
    const kept = afterEcho.find((t) => t.id === id)!.env!
    // 原值未被占位覆盖：解封回真值仍是 secret-value（密文每次随机 IV 不同，故比明文而非密文）
    expect(unseal(kept.API_TOKEN)).toBe("secret-value")
    // 修改：空对象清除（未提供则不改动）
    const cleared = (await (await fetch(`${base()}/api/v1/tasks/${id}`, req("PATCH", { env: {} }))).json()) as { env?: Record<string, string> }
    expect(cleared.env).toBeUndefined()
  })

  test("查询/修改/删除；通知密钥脱敏；非法 id 400", async () => {
    const created = await createTask({ kind: "scheduled", runner: "script", script: "echo patch", schedule: "@every 20m" })
    const id = String(created.id)

    // 启用状态下修改表达式严格校验（非法表达式直接拒绝）
    expect((await fetch(`${base()}/api/v1/tasks/${id}`, req("PATCH", { schedule: "bad expr" }))).status).toBe(400)

    const patched = (await (
      await fetch(`${base()}/api/v1/tasks/${id}`, req("PATCH", { enabled: false, name: "renamed", notify: [{ type: "webhook", target: "https://example.com/hook", secret: "s3cr3t" }] }))
    ).json()) as { enabled: boolean; name?: string; notify?: Array<{ secret?: string }>; nextRunAt?: number }
    expect(patched.enabled).toBe(false)
    expect(patched.name).toBe("renamed")
    // 回显脱敏（不泄露 secret），但确实配置成功
    expect(patched.notify![0].secret).toBe("***")

    expect((await fetch(`${base()}/api/v1/tasks/${"0".repeat(32)}`, req("PATCH", { enabled: true }))).status).toBe(404)
    expect((await fetch(`${base()}/api/v1/tasks/${"0".repeat(32)}`)).status).toBe(404)

    expect(await getTask(id)).toMatchObject({ id, name: "renamed", enabled: false })

    expect((await fetch(`${base()}/api/v1/tasks/${id}`, req("DELETE"))).status).toBe(200)
    expect((await fetch(`${base()}/api/v1/tasks/${id}`, req("DELETE"))).status).toBe(404)

    // 非法 id：格式白名单拒绝（32 位 hex 之外的畸形形态），且 /queue 保留路径不受影响
    expect((await fetch(`${base()}/api/v1/tasks/not-hex`)).status).toBe(400)
    expect((await fetch(`${base()}/api/v1/tasks/not-hex`, req("DELETE"))).status).toBe(400)
    expect((await fetch(`${base()}/api/v1/tasks/not-hex/run`, req("POST", {}))).status).toBe(400)
  })

  test("手动执行走统一队列：入队 → 后台执行 → 记录结果与历史", async () => {
    const created = await createTask({ kind: "manual", runner: "script", name: "run-once", script: "echo task-ok" })
    const id = String(created.id)
    const done = await waitFor(async () => {
      const t = await getTask(id)
      return t.state === "idle" && Number(t.runCount) >= 1 ? t : null
    })
    expect(done.lastStatus).toBe("success")
    expect(String(done.lastOutput)).toContain("task-ok")
    // 执行记录不在任务定义里（runs 字段已移出），经独立端点读取（新→旧）
    expect(done).not.toHaveProperty("runs")
    const runsRes = await fetch(`${base()}/api/v1/tasks/${id}/runs`)
    expect(runsRes.status).toBe(200)
    const runs = (await runsRes.json()) as Array<{ status: string; manual?: boolean; durationMs: number }>
    expect(runs).toHaveLength(1)
    expect(runs[0].status).toBe("success")
    expect(runs[0].manual).toBe(true)
    // limit 与非法值
    expect(((await (await fetch(`${base()}/api/v1/tasks/${id}/runs?limit=1`)).json()) as unknown[])).toHaveLength(1)
    expect((await fetch(`${base()}/api/v1/tasks/${id}/runs?limit=abc`)).status).toBe(400)
    expect((await fetch(`${base()}/api/v1/tasks/not-hex/runs`)).status).toBe(400)
    await fetch(`${base()}/api/v1/tasks/${id}`, req("DELETE"))
  })

  test("队列语义：额度 1 时排队等待、置顶插队、出队、终止运行", async () => {
    const home2 = mkdtempSync(join(tmpdir(), "gebai-tasks-queue-"))
    const h2 = await startServer({ gebaiHome: home2, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], taskMaxConcurrent: 1, port: 0 })
    try {
      const a = await createTask({ kind: "scheduled", runner: "script", name: "slow", schedule: "@every 1h", script: "sleep 2" }, h2)
      const m1 = await createTask({ kind: "manual", runner: "script", name: "m1", script: "echo m1", runNow: false }, h2)
      const m2 = await createTask({ kind: "manual", runner: "script", name: "m2", script: "echo m2", runNow: false }, h2)

      // A 入队即占满唯一额度（运行中的任务不会被后续入队中断）
      const ranA = (await (await fetch(`${base(h2)}/api/v1/tasks/${a.id}/run`, req("POST", {}))).json()) as { queued: boolean; position?: number }
      expect(ranA).toMatchObject({ queued: true, position: 1 })
      expect((await getTask(String(a.id), h2)).state).toBe("running")

      const q1 = (await (await fetch(`${base(h2)}/api/v1/tasks/queue`)).json()) as {
        limit: number
        running: Array<{ taskId: string }>
        entries: Array<{ taskId: string; position: number; front?: boolean; kind: string }>
        busy: boolean
      }
      expect(q1.limit).toBe(1)
      expect(q1.running.map((r) => r.taskId)).toEqual([a.id])
      expect(q1.entries).toEqual([])
      expect(q1.busy).toBe(true)

      // 普通任务排队（额度已满）：置顶插到同类别之前；定时任务恒在普通任务之前
      expect((await (await fetch(`${base(h2)}/api/v1/tasks/${m1.id}/run`, req("POST", {}))).json())).toMatchObject({ queued: true, position: 1 })
      expect((await (await fetch(`${base(h2)}/api/v1/tasks/${m2.id}/run`, req("POST", {}))).json())).toMatchObject({ queued: true, position: 2 })
      expect((await (await fetch(`${base(h2)}/api/v1/tasks/${m2.id}/front`, req("POST", {}))).json())).toMatchObject({ queued: true, position: 1 })
      const q2 = (await (await fetch(`${base(h2)}/api/v1/tasks/queue`)).json()) as { entries: Array<{ taskId: string; position: number; front?: boolean }> }
      expect(q2.entries.map((e) => e.taskId)).toEqual([m2.id, m1.id])
      expect(q2.entries[0].front).toBe(true)

      const s = await createTask({ kind: "scheduled", runner: "script", name: "s", schedule: "@every 1h", script: "echo s" }, h2)
      expect((await (await fetch(`${base(h2)}/api/v1/tasks/${s.id}/run`, req("POST", {}))).json())).toMatchObject({ queued: true, position: 1 })
      const q2b = (await (await fetch(`${base(h2)}/api/v1/tasks/queue`)).json()) as { entries: Array<{ taskId: string; kind: string }> }
      expect(q2b.entries.map((e) => `${e.kind}:${e.taskId}`)).toEqual([`scheduled:${s.id}`, `manual:${m2.id}`, `manual:${m1.id}`])

      // 出队：定时任务出队后普通任务按置顶优先级递补队首
      expect((await (await fetch(`${base(h2)}/api/v1/tasks/${s.id}/queue`, req("DELETE"))).json())).toEqual({ ok: true })
      expect((await getTask(String(s.id), h2)).state).toBe("idle")
      const q3 = (await (await fetch(`${base(h2)}/api/v1/tasks/queue`)).json()) as { entries: Array<{ taskId: string }> }
      expect(q3.entries.map((e) => e.taskId)).toEqual([m2.id, m1.id])
      expect((await (await fetch(`${base(h2)}/api/v1/tasks/${m2.id}/queue`, req("DELETE"))).json())).toEqual({ ok: true })
      const q4 = (await (await fetch(`${base(h2)}/api/v1/tasks/queue`)).json()) as { entries: Array<{ taskId: string }> }
      expect(q4.entries.map((e) => e.taskId)).toEqual([m1.id])

      // 终止运行中的任务；排队中的终止 = 出队；空闲/不存在的返回 ok:false
      expect((await (await fetch(`${base(h2)}/api/v1/tasks/${a.id}/stop`, req("POST", {}))).json())).toEqual({ ok: true })
      expect((await (await fetch(`${base(h2)}/api/v1/tasks/${m1.id}/stop`, req("POST", {}))).json())).toEqual({ ok: true })
      expect((await (await fetch(`${base(h2)}/api/v1/tasks/${m1.id}/stop`, req("POST", {}))).json())).toEqual({ ok: false })
      expect((await fetch(`${base(h2)}/api/v1/tasks/${"0".repeat(32)}/run`, req("POST", {}))).status).toBe(404)
      expect((await fetch(`${base(h2)}/api/v1/tasks/${"0".repeat(32)}/front`, req("POST", {}))).status).toBe(404)
    } finally {
      h2.gc?.stop()
      h2.tasks?.stop()
      h2.server.stop(true)
      await removeDirRetry(home2)
    }
  })

  test("任务资源文件：列/读/写/删与路径越界拒绝", async () => {
    const created = await createTask({ kind: "scheduled", runner: "script", name: "files", schedule: "@every 30m", script: "bash run.sh" })
    const id = String(created.id)
    expect(await (await fetch(`${base()}/api/v1/tasks/${id}/files`)).json()).toEqual([])

    const written = (await (
      await fetch(`${base()}/api/v1/tasks/${id}/files/content`, req("PUT", { path: "run.sh", content: "echo hi\n" }))
    ).json()) as { path: string; size: number; dir: boolean }
    expect(written).toMatchObject({ path: "run.sh", dir: false })
    expect(written.size).toBeGreaterThan(0)

    const nested = (await (
      await fetch(`${base()}/api/v1/tasks/${id}/files/content`, req("PUT", { path: "docs/note.md", content: "# 说明\n" }))
    ).json()) as { path: string }
    expect(nested.path).toBe("docs/note.md")

    const listed = (await (await fetch(`${base()}/api/v1/tasks/${id}/files`)).json()) as Array<{ path: string; dir: boolean }>
    expect(listed.map((f) => `${f.dir ? "d" : "f"}:${f.path}`)).toEqual(["d:docs", "f:docs/note.md", "f:run.sh"])

    const read = (await (await fetch(`${base()}/api/v1/tasks/${id}/files/content?path=run.sh`)).json()) as { path: string; content: string }
    expect(read).toMatchObject({ path: "run.sh", content: "echo hi\n" })

    // 路径越界（跳出任务资源目录）拒绝
    const escape = await fetch(`${base()}/api/v1/tasks/${id}/files/content`, req("PUT", { path: "../escape.txt", content: "x" }))
    expect(escape.status).toBe(400)
    expect(String(((await escape.json()) as { error: string }).error)).toContain("越界")

    // 缺失文件 404；非法 id 400
    expect((await fetch(`${base()}/api/v1/tasks/${id}/files/content?path=missing.sh`)).status).toBe(404)
    expect((await fetch(`${base()}/api/v1/tasks/not-hex/files`)).status).toBe(400)
    // 缺 path（PUT）400；不存在任务 404
    expect((await fetch(`${base()}/api/v1/tasks/${id}/files/content`, req("PUT", { content: "x" }))).status).toBe(400)
    expect((await fetch(`${base()}/api/v1/tasks/${"0".repeat(32)}/files`)).status).toBe(404)
    expect((await fetch(`${base()}/api/v1/tasks/${"0".repeat(32)}/files/content?path=a`)).status).toBe(404)

    expect((await (await fetch(`${base()}/api/v1/tasks/${id}/files?path=run.sh`, req("DELETE"))).json())).toEqual({ ok: true })
    expect((await (await fetch(`${base()}/api/v1/tasks/${id}/files?path=run.sh`, req("DELETE"))).json())).toEqual({ ok: false })

    // 资源目录随任务删除保留（文件仍在磁盘上，仅任务条目消失）
    expect((await fetch(`${base()}/api/v1/tasks/${id}`, req("DELETE"))).status).toBe(200)
  })

  test("能力关闭（GEBAI_TASKS_ENABLED=false）返回 503", async () => {
    const home2 = mkdtempSync(join(tmpdir(), "gebai-tasks-rest-off-"))
    const h2 = await startServer({ gebaiHome: home2, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], tasksEnabled: false, port: 0 })
    try {
      expect((await fetch(`http://127.0.0.1:${h2.server.port}/api/v1/tasks`)).status).toBe(503)
      expect((await fetch(`http://127.0.0.1:${h2.server.port}/api/v1/tasks`, req("POST", { runner: "script", script: "echo x" }))).status).toBe(503)
      expect((await fetch(`http://127.0.0.1:${h2.server.port}/api/v1/tasks/queue`)).status).toBe(503)
      expect(h2.tasks).toBeNull()
    } finally {
      h2.gc?.stop()
      h2.server.stop(true)
      await removeDirRetry(home2)
    }
  })
})
