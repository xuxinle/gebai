import { describe, expect, test } from "bun:test"
import { existsSync, mkdtempSync, mkdirSync, rmSync, writeFileSync, readFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { CronManager, isOneShotSchedule, parseCronSchedule, type CronTask, type CronTaskType } from "./cron"
import { SessionStore } from "./store"
import { Sandbox } from "./sandbox"
import { EnvManager } from "./env"
import { EventBus } from "./event-bus"
import type { AgentEngine } from "./engine"
import { feishuBotSign, validateNotifyChannel, sendCronNotification, normalizeAtList, type CronResultNotification } from "./notify"

function localDate(ms: number | string): Date {
  return new Date(ms)
}

describe("parseCronSchedule", () => {
  test("5-field cron computes next minute-level time", () => {
    const sched = parseCronSchedule("0 9 * * *")
    const from = localDate("2026-08-06T08:30:00").getTime()
    const next = sched.next(from)
    expect(localDate(next).getHours()).toBe(9)
    expect(localDate(next).getMinutes()).toBe(0)
    expect(next).toBeGreaterThan(from)
    expect(next).toBeLessThan(from + 24 * 3600 * 1000)
  })

  test("next occurrence after the scheduled time rolls to next day", () => {
    const sched = parseCronSchedule("0 9 * * *")
    const from = localDate("2026-08-06T10:00:00").getTime()
    const next = sched.next(from)
    expect(localDate(next).getDate()).toBe(7)
    expect(localDate(next).getHours()).toBe(9)
  })

  test("step and range fields", () => {
    const sched = parseCronSchedule("*/15 9-18 * * 1-5")
    const from = localDate("2026-08-06T00:00:00").getTime() // 周四
    const next = sched.next(from)
    const d = localDate(next)
    expect([0, 15, 30, 45]).toContain(d.getMinutes())
    expect(d.getHours()).toBeGreaterThanOrEqual(9)
    expect(d.getHours()).toBeLessThanOrEqual(18)
    expect(d.getDay()).toBeGreaterThanOrEqual(1)
    expect(d.getDay()).toBeLessThanOrEqual(5)
  })

  test("dow 0 and 7 both mean Sunday", () => {
    const sunday = localDate("2026-08-09T00:00:00").getTime() // 周日
    expect(parseCronSchedule("0 0 * * 0").next(sunday - 3600_000)).toBe(parseCronSchedule("0 0 * * 7").next(sunday - 3600_000))
  })

  test("dom/dow both restricted uses OR semantics", () => {
    const sched = parseCronSchedule("0 0 1 * 0")
    const from = localDate("2026-08-01T12:00:00").getTime() // 8 月 1 日（周六）当天，OR 语义应命中今天 0 点已过 → 下一命中
    const next = sched.next(from)
    // 2026-08-02 是周日（dow=0 命中）→ 下一次 0 点应为 8 月 2 日
    expect(localDate(next).getDate()).toBe(2)
  })

  test("@every interval", () => {
    const sched = parseCronSchedule("@every 30m")
    const from = localDate("2026-08-06T10:00:00").getTime()
    expect(sched.next(from)).toBe(from + 30 * 60 * 1000)
    const off = from + 10 * 60 * 1000
    expect(sched.next(off)).toBe(from + 30 * 60 * 1000)
  })

  test("aliases @daily/@hourly/@weekly/@monthly", () => {
    expect(parseCronSchedule("@daily").next(0)).toBe(parseCronSchedule("0 0 * * *").next(0))
    expect(parseCronSchedule("@hourly").next(0)).toBe(parseCronSchedule("0 * * * *").next(0))
    expect(parseCronSchedule("@weekly").next(0)).toBe(parseCronSchedule("0 0 * * 0").next(0))
    expect(parseCronSchedule("@monthly").next(0)).toBe(parseCronSchedule("0 0 1 * *").next(0))
  })

  test("@at one-shot parses absolute time (ISO 与空格分隔)", () => {
    expect(isOneShotSchedule("@at 2026-09-01T09:00")).toBe(true)
    expect(isOneShotSchedule("0 9 * * *")).toBe(false)
    const sched = parseCronSchedule("@at 2026-09-01T09:00")
    const from = localDate("2026-08-06T10:00:00").getTime()
    expect(sched.next(from)).toBe(localDate("2026-09-01T09:00:00").getTime())
    // next() 与 fromMs 无关（绝对时刻）
    expect(sched.next(0)).toBe(localDate("2026-09-01T09:00:00").getTime())
    const spaced = parseCronSchedule("@at 2026-09-01 09:00")
    expect(spaced.next(from)).toBe(localDate("2026-09-01T09:00:00").getTime())
    expect(() => parseCronSchedule("@at whenever")).toThrow(/@at/)
  })

  test("timezone schedules compute wall-clock in target tz", () => {
    const sched = parseCronSchedule("0 9 * * *", "Asia/Shanghai")
    const from = Date.UTC(2026, 7, 5, 22, 0) // UTC 8/5 22:00 = 上海 8/6 06:00
    const next = sched.next(from)
    const wall = new Intl.DateTimeFormat("en-US", { timeZone: "Asia/Shanghai", hour: "2-digit", minute: "2-digit", hour12: false }).format(new Date(next))
    expect(wall).toBe("09:00")
    // 上海 9:00 = UTC 1:00
    expect(next).toBe(Date.UTC(2026, 7, 6, 1, 0))
  })

  test("invalid timezone throws", () => {
    expect(() => parseCronSchedule("0 9 * * *", "Mars/Olympus")).toThrow(/时区/)
  })

  test("invalid expressions throw", () => {
    expect(() => parseCronSchedule("")).toThrow()
    expect(() => parseCronSchedule("60 * * * *")).toThrow()
    expect(() => parseCronSchedule("0 9 * *")).toThrow()
    expect(() => parseCronSchedule("a b c d e")).toThrow()
    expect(() => parseCronSchedule("@every 5x")).toThrow()
    expect(() => parseCronSchedule("@every 0m")).toThrow()
    expect(() => parseCronSchedule("0 0 30 2 *").next(0)).toThrow() // 2 月 30 日永不触发
  })
})

describe("notify", () => {
  test("validateNotifyChannel accepts webhook/feishu/feishu_chat and rejects bad forms", () => {
    expect(() => validateNotifyChannel({ type: "webhook", target: "https://example.com/hook" })).not.toThrow()
    expect(() => validateNotifyChannel({ type: "webhook", target: "file:///etc/passwd" })).toThrow()
    expect(() => validateNotifyChannel({ type: "webhook", target: "http://127.0.0.1/x" })).toThrow() // SSRF：回环拒绝
    expect(() => validateNotifyChannel({ type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/abc" })).not.toThrow()
    expect(() => validateNotifyChannel({ type: "feishu", target: "https://evil.com/open-apis/bot/v2/hook/abc" })).toThrow()
    expect(() => validateNotifyChannel({ type: "feishu", target: "https://open.feishu.cn/other" })).toThrow()
    expect(() => validateNotifyChannel({ type: "feishu_chat", target: "oc_abcdef1234567890abcdef" })).not.toThrow()
    expect(() => validateNotifyChannel({ type: "feishu_chat", target: "not a chat" })).toThrow()
    expect(() => validateNotifyChannel({ type: "sms", target: "x" } as never)).toThrow()
  })

  test("at 名单：open_id/all 合法（含 {id,name} 形态），非法 id 与 webhook 通道拒绝", () => {
    expect(() => validateNotifyChannel({ type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/abc", at: [{ id: "all" }, { id: "ou_16a8c3be49ab2c1e68e0ddc62e123abc" }] })).not.toThrow()
    expect(() => validateNotifyChannel({ type: "feishu_chat", target: "oc_abcdef1234567890abcdef", at: [{ id: "ou_xxx", name: "张三" }] })).not.toThrow()
    expect(() => validateNotifyChannel({ type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/abc", at: [{ id: "张三" }] })).toThrow(/open_id/)
    expect(() => validateNotifyChannel({ type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/abc", at: [{}] as never })).toThrow(/id/)
    expect(() => validateNotifyChannel({ type: "webhook", target: "https://example.com/hook", at: [{ id: "all" }] })).toThrow(/仅支持飞书/)
    // normalizeAtList：字符串/对象混输入归一 + 去重
    expect(normalizeAtList(["all", { id: "ou_abc", name: "李四" }, "all"])).toEqual([{ id: "all" }, { id: "ou_abc", name: "李四" }])
    expect(normalizeAtList([])).toBeUndefined()
    expect(normalizeAtList(undefined)).toBeUndefined()
  })

  test("feishuBotSign matches spec (HMAC-SHA256(key=ts\\nsecret, msg='') base64)", () => {
    const sign = feishuBotSign("1700000000", "test-secret")
    expect(sign).toMatch(/^[A-Za-z0-9+/=]+$/)
    // 同输入同输出、异输入异输出
    expect(feishuBotSign("1700000000", "test-secret")).toBe(sign)
    expect(feishuBotSign("1700000001", "test-secret")).not.toBe(sign)
  })

  test("sendCronNotification posts feishu markdown card with signature and at tags", async () => {
    const posts: Array<{ url: string; body: Record<string, unknown> }> = []
    const res = await sendCronNotification(
      { type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/abc", secret: "s3cret", at: [{ id: "all" }, { id: "ou_abc123", name: "张三" }] },
      { event: "cron.result", task: { id: "t1", name: "n", type: "script", schedule: "@daily", user: "u" }, ok: false, status: "error", at: 1700000000000, error: "boom" },
      {
        now: () => 1700000000000,
        fetchImpl: async (url, init) => {
          posts.push({ url, body: JSON.parse(String(init.body)) })
          return { ok: true, status: 200 }
        },
      },
    )
    expect(res).toBeUndefined()
    expect(posts).toHaveLength(1)
    expect(posts[0].url).toContain("open.feishu.cn/open-apis/bot/v2/hook/abc")
    // markdown 卡片（interactive）：状态着色头部 + lark_md 正文 + at 标签 + 加签
    expect(posts[0].body.msg_type).toBe("interactive")
    const card = posts[0].body.card as { header: { template: string; title: { content: string } }; elements: Array<{ tag: string; text?: { tag: string; content: string } }> }
    expect(card.header.template).toBe("red")
    expect(card.header.title.content).toContain("「n」")
    const md = card.elements.find((e) => e.tag === "div")!.text!
    expect(md.tag).toBe("lark_md")
    expect(md.content).toContain("**状态：**")
    expect(md.content).toContain("失败")
    expect(md.content).toContain('**错误：**boom')
    expect(md.content).toContain('<at user_id="all">所有人</at>')
    expect(md.content).toContain('<at user_id="ou_abc123">张三</at>')
    expect(posts[0].body.timestamp).toBe("1700000000")
    expect(posts[0].body.sign).toBe(feishuBotSign("1700000000", "s3cret"))
    // 任务输出中的尖括号净化（防输出注入 at/链接标签）
    const injected = await sendCronNotification(
      { type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/abc" },
      { event: "cron.result", task: { id: "t", name: "x", type: "script", schedule: "@daily", user: "u" }, ok: true, status: "success", at: 0, output: '<at user_id="all">假@</at>' },
      { fetchImpl: async (_u, init) => { posts.push({ url: _u, body: JSON.parse(String(init.body)) }); return { ok: true, status: 200 } } },
    )
    expect(injected).toBeUndefined()
    const md2 = ((posts[1].body.card as { elements: Array<{ tag: string; text?: { content: string } }> }).elements.find((e) => e.tag === "div")!.text!).content
    expect(md2).not.toContain('<at user_id="all">假@</at>')
    expect(md2).toContain("＜at")

    // 非 2xx（HTTP 500）抛错
    await expect(
      sendCronNotification(
        { type: "webhook", target: "https://example.com/hook" },
        { event: "cron.result", task: { id: "t", name: "", type: "script", schedule: "@daily", user: "u" }, ok: true, status: "success", at: 0 },
        { fetchImpl: async () => ({ ok: false, status: 500 }) },
      ),
    ).rejects.toThrow(/500/)
  })

  test("sendCronNotification feishu_chat goes through injected sender with card", async () => {
    const sent: Array<{ chatId: string; card: Record<string, unknown> }> = []
    const n: CronResultNotification = { event: "cron.result", task: { id: "t", name: "x", type: "script", schedule: "@daily", user: "u" }, ok: true, status: "success", at: 0 }
    await sendCronNotification({ type: "feishu_chat", target: "oc_123", at: [{ id: "all" }] }, n, { feishuSend: async (chatId, card) => void sent.push({ chatId, card }) })
    expect(sent).toHaveLength(1)
    expect(sent[0].chatId).toBe("oc_123")
    expect((sent[0].card.header as { template: string }).template).toBe("green")
    const md = (sent[0].card.elements as Array<{ tag: string; text?: { content: string } }>).find((e) => e.tag === "div")!.text!.content
    expect(md).toContain("成功")
    expect(md).toContain('<at user_id="all">所有人</at>')
    // 未注入飞书应用凭证时明确报错
    await expect(sendCronNotification({ type: "feishu_chat", target: "oc_123" }, n, {})).rejects.toThrow(/未配置/)
  })
})

interface Harness {
  home: string
  store: SessionStore
  sandbox: Sandbox
  env: EnvManager
  events: EventBus
  cron: CronManager
  executed: string[]
  execCwds: string[]
  runCalls: string[]
  cancelCalls: string[]
  running: boolean
  runHang: boolean
  notifyPosts: Array<{ url: string; body: unknown }>
  feishuSent: Array<Record<string, unknown>>
  agentNames: string[]
}

function setup(now = 1_780_000_000_000, tickIntervalMs = 3600_000, safeMode = false) {
  const home = mkdtempSync(join(tmpdir(), "gebai-cron-"))
  mkdirSync(join(home, "users", "default"), { recursive: true })
  const store = new SessionStore({ home })
  const sandbox = new Sandbox({ home, enabled: false })
  const env = new EnvManager(store)
  const events = new EventBus()
  const h: Harness = {
    home,
    store,
    sandbox,
    env,
    events,
    cron: null as unknown as CronManager,
    executed: [],
    execCwds: [],
    runCalls: [],
    cancelCalls: [],
    running: false,
    runHang: false,
    notifyPosts: [],
    feishuSent: [],
    agentNames: ["explore", "code"],
  }
  const runResolvers: Array<() => void> = []
  const fakeEngine = {
    isRunning: () => h.running,
    cancel: (sid: string) => {
      h.cancelCalls.push(sid)
      // 模拟真实引擎：cancel 使挂起中的 run settle（abort 传播）
      runResolvers.splice(0).forEach((r) => r())
    },
    run: async (_sid: string, _user: string, prompt: string) => {
      h.runCalls.push(prompt)
      if (h.runHang) await new Promise<void>((resolve) => runResolvers.push(resolve))
    },
  } as unknown as AgentEngine
  h.cron = new CronManager({
    home,
    store,
    env,
    sandbox,
    events,
    engine: fakeEngine,
    now: () => now,
    tickIntervalMs,
    safeMode,
    notify: {
      fetchImpl: async (url, init) => {
        h.notifyPosts.push({ url, body: JSON.parse(String(init.body)) })
        return { ok: true, status: 200 }
      },
      feishuSend: async (_chatId, card) => void h.feishuSent.push(card),
    },
    agentExists: (name) => h.agentNames.includes(name),
  })
  return h
}

function cleanup(h: Harness) {
  h.cron.stop()
  rmSync(h.home, { recursive: true, force: true })
}

async function createSession(h: Harness, name = "t"): Promise<{ id: string; user: string }> {
  const s = await h.store.createSession("default", name)
  return { id: s.id, user: "default" }
}

/** 访问调度器内部条目（add 返回克隆，测试触发用需改内部 nextRunAt）。 */
function internal(h: Harness, task: CronTask): CronTask {
  return h.cron["entries"].get(task.id)!
}

/** 把任务拨到立即可触发。 */
async function due(h: Harness, task: CronTask): Promise<void> {
  internal(h, task).nextRunAt = h.cron["now"]() - 1000
}

describe("CronManager", () => {
  test("add persists user-level cron.json and computes nextRunAt", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      const task = await h.cron.add(user, { name: "daily", type: "script", schedule: "0 9 * * *", script: "echo hi" }, id)
      expect(task.nextRunAt).toBeGreaterThan(0)
      expect(task.enabled).toBe(true)
      expect(task.originSessionId).toBe(id)
      expect(internal(h, task).runCount).toBe(0)
      // 用户级存储：users/{user}/cron.json（不在会话目录）
      const userCron = join(h.home, "users", "default", "cron.json")
      expect(existsSync(userCron)).toBe(true)
      expect(existsSync(join(h.home, "users", "default", "sessions", id.slice(0, 2), id.slice(2, 4), id, "cron.json"))).toBe(false)
      const fromDisk = await h.cron.list(user)
      expect(fromDisk).toHaveLength(1)
      expect(fromDisk[0].script).toBe("echo hi")
    } finally {
      cleanup(h)
    }
  })

  test("add validates type fields and schedule", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      await expect(h.cron.add(user, { type: "script", schedule: "0 9 * * *" })).rejects.toThrow(/script/)
      await expect(h.cron.add(user, { type: "prompt", schedule: "0 9 * * *" })).rejects.toThrow(/prompt/)
      await expect(h.cron.add(user, { type: "script", schedule: "bad", script: "echo" })).rejects.toThrow(/无效/)
      await expect(h.cron.add(user, { type: "nope" as CronTaskType, schedule: "0 9 * * *", script: "x" })).rejects.toThrow(/类型/)
      await expect(h.cron.add(user, { type: "prompt", schedule: "0 9 * * *", prompt: "hi", target: "nowhere" as never })).rejects.toThrow(/执行目标/)
      await expect(h.cron.add(user, { type: "prompt", schedule: "0 9 * * *", prompt: "hi", agents: ["ghost"] })).rejects.toThrow(/不存在/)
      await expect(h.cron.add(user, { type: "script", schedule: "0 9 * * *", script: "x", notify: [{ type: "feishu", target: "https://evil.com/hook" }] })).rejects.toThrow(/飞书/)
      await expect(h.cron.add(user, { type: "script", schedule: "0 9 * * *", script: "x", timeoutMs: 10 })).rejects.toThrow(/超时/)
      expect(await h.cron.list(user)).toHaveLength(0)
    } finally {
      cleanup(h)
    }
  })

  test("update and remove with user ownership", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      await h.store.createSession("other", "t2")
      const task = await h.cron.add(user, { type: "script", schedule: "0 9 * * *", script: "echo a" })
      // 其他用户无权修改/删除
      expect(await h.cron.update("other", task.id, { enabled: false })).toBeNull()
      expect(await h.cron.remove("other", task.id)).toBe(false)
      // 本用户可更新
      const updated = await h.cron.update(user, task.id, { enabled: false, schedule: "@every 10m" })
      expect(updated?.enabled).toBe(false)
      const re = await h.cron.update(user, task.id, { enabled: true })
      expect(re?.enabled).toBe(true)
      expect(re?.nextRunAt).toBeGreaterThan(0)
      expect(await h.cron.remove(user, task.id)).toBe(true)
      expect(await h.cron.list(user)).toHaveLength(0)
    } finally {
      cleanup(h)
    }
  })

  test("update validates content after type change", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      const task = await h.cron.add(user, { type: "script", schedule: "0 9 * * *", script: "echo a" })
      await expect(h.cron.update(user, task.id, { type: "prompt" })).rejects.toThrow(/prompt/)
    } finally {
      cleanup(h)
    }
  })

  test("tick fires script task: exec in task workspace + origin message + events + history", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      h.sandbox.exec = async (cmd, opts) => {
        h.executed.push(cmd)
        h.execCwds.push(opts?.cwd ?? "")
        return { stdout: "out-1\nout-2", stderr: "", code: 0 }
      }
      const published: string[] = []
      h.events.subscribe((ev) => published.push(ev.type))
      const task = await h.cron.add(user, { name: "sync", type: "script", schedule: "@every 30m", script: "echo ok" }, id)
      await due(h, task)
      await h.cron.tick()
      expect(h.executed).toEqual(["echo ok"])
      // 工作目录为任务专属（users/{user}/cron-workspace/{taskId}），不再依赖会话 tmp
      expect(h.execCwds[0]).toBe(join(h.home, "users", "default", "cron-workspace", task.id))
      expect(internal(h, task).runCount).toBe(1)
      expect(internal(h, task).lastStatus).toBe("success")
      expect(internal(h, task).nextRunAt).toBeGreaterThan(h.cron["now"]())
      // 来源会话仍收到结果消息（模型可感知）
      const session = await h.store.load(id, user)
      const last = session!.messages.at(-1)!
      expect(last.content).toContain("[定时任务「sync」执行结果（成功）]")
      expect(last.content).toContain("out-2")
      expect(published).toContain("event.cron.run")
      expect(published).toContain("event.cron.result")
      // 运行历史记录
      const entry = internal(h, task)
      expect(entry.runs).toHaveLength(1)
      expect(entry.runs![0].status).toBe("success")
      expect(entry.runs![0].durationMs).toBeGreaterThanOrEqual(0)
      const fromDisk = await h.cron.list(user)
      expect(fromDisk[0].runCount).toBe(1)
      expect(fromDisk[0].runs).toHaveLength(1)
    } finally {
      cleanup(h)
    }
  })

  test("script failure records error and reports exit code", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      h.sandbox.exec = async () => ({ stdout: "", stderr: "boom", code: 3 })
      const task = await h.cron.add(user, { type: "script", schedule: "@every 30m", script: "false" })
      await due(h, task)
      await h.cron.tick()
      expect(internal(h, task).lastStatus).toBe("error")
      expect(internal(h, task).lastError).toBe("exit 3")
      expect(internal(h, task).nextRunAt).toBeGreaterThan(h.cron["now"]())
      expect(internal(h, task).runs![0].status).toBe("error")
    } finally {
      cleanup(h)
    }
  })

  test("safe mode skips script cron tasks (no exec, message persisted, status skipped)", async () => {
    const h = setup(1_780_000_000_000, 3600_000, true) // safeMode=true
    try {
      const { id, user } = await createSession(h)
      h.sandbox.exec = async (cmd) => {
        h.executed.push(cmd)
        return { stdout: "should-not-run", stderr: "", code: 0 }
      }
      const task = await h.cron.add(user, { name: "danger", type: "script", schedule: "@every 30m", script: "echo pwned" }, id)
      await due(h, task)
      await h.cron.tick()
      expect(h.executed).toEqual([])
      expect(internal(h, task).lastStatus).toBe("skipped")
      expect(internal(h, task).nextRunAt).toBeGreaterThan(h.cron["now"]())
      const session = await h.store.load(id, user)
      const last = session!.messages.at(-1)!
      expect(last.content).toContain("已跳过")
      expect(last.content).toContain("安全模式")
    } finally {
      cleanup(h)
    }
  })

  test("prompt default target=ephemeral: fresh session each run, name and agents preload applied", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      const task = await h.cron.add(user, { name: "日报", type: "prompt", schedule: "@every 30m", prompt: "总结今日", agents: ["explore"] })
      await due(h, task)
      await h.cron.tick()
      expect(h.runCalls).toHaveLength(1)
      expect(h.runCalls[0]).toContain("[定时任务「日报」触发]")
      // 新会话名为「定时任务「日报」」且预载名单写入 loadedSubAgents
      const sessions = await h.store.listSessions(user)
      expect(sessions.map((s) => s.name)).toContain("定时任务「日报」")
      const created = sessions.find((s) => s.name === "定时任务「日报」")!
      expect(created.loadedSubAgents).toEqual(["explore"])
      expect(internal(h, task).runs![0].sessionId).toBe(created.id)
      expect(internal(h, task).lastStatus).toBe("success")
      // 再触发一次：又建一个新会话（ephemeral 不复用）
      await due(h, task)
      await h.cron.tick()
      expect((await h.store.listSessions(user)).filter((s) => s.name === "定时任务「日报」")).toHaveLength(2)
    } finally {
      cleanup(h)
    }
  })

  test("prompt target=sticky reuses one dedicated session across runs", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      const task = await h.cron.add(user, { name: "巡检", type: "prompt", schedule: "@every 30m", prompt: "检查", target: "sticky" })
      await due(h, task)
      await h.cron.tick()
      const sid1 = internal(h, task).runs![0].sessionId
      expect(internal(h, task).stickySessionId).toBe(sid1)
      await due(h, task)
      await h.cron.tick()
      const sid2 = internal(h, task).runs![1].sessionId
      expect(sid2).toBe(sid1)
      expect(h.runCalls).toHaveLength(2)
    } finally {
      cleanup(h)
    }
  })

  test("prompt target=session runs in bound session; deleted session self-heals to ephemeral", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      const task = await h.cron.add(user, { type: "prompt", schedule: "@every 30m", prompt: "hi", target: "session" }, id)
      await due(h, task)
      await h.cron.tick()
      expect(internal(h, task).runs![0].sessionId).toBe(id)
      // 绑定会话删除后：自愈降级 ephemeral，任务保留
      await h.store.delete(id, user)
      await due(h, task)
      await h.cron.tick()
      const entry = internal(h, task)
      expect(entry.target).toBe("ephemeral")
      expect(entry.runs![0].sessionId).not.toBe(id)
      expect(entry.runs![0].sessionId).toBeTruthy()
    } finally {
      cleanup(h)
    }
  })

  test("prompt task skipped while session busy, then rescheduled", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      const task = await h.cron.add(user, { type: "prompt", schedule: "@every 30m", prompt: "hi", target: "sticky" })
      await due(h, task)
      h.running = true
      await h.cron.tick()
      expect(h.runCalls).toHaveLength(0)
      expect(internal(h, task).lastStatus).toBe("skipped")
      expect(internal(h, task).nextRunAt).toBeGreaterThan(h.cron["now"]())
      h.running = false
      await due(h, task)
      await h.cron.tick()
      expect(h.runCalls).toHaveLength(1)
    } finally {
      cleanup(h)
    }
  })

  test("prompt timeout cancels the run and records timeout status", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      // 超时下限 1000ms：挂起 fake run，1s 后 cancel
      const task = await h.cron.add(user, { type: "prompt", schedule: "@every 30m", prompt: "slow", target: "sticky", timeoutMs: 1000 })
      h.runHang = true
      await due(h, task)
      const fired = h.cron.tick()
      await new Promise((r) => setTimeout(r, 80))
      expect(h.cancelCalls).toHaveLength(0) // 未到超时不取消
      await fired
      expect(h.cancelCalls).toHaveLength(1)
      const entry = internal(h, task)
      expect(entry.lastStatus).toBe("timeout")
      expect(entry.lastError).toContain("超时")
      expect(entry.runs![0].status).toBe("timeout")
    } finally {
      cleanup(h)
    }
  })

  test("@at one-shot fires once then auto-disables", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      const at = new Date(h.cron["now"]() + 60_000).toISOString().replace(/\.\d{3}Z$/, "")
      const task = await h.cron.add(user, { name: "once", type: "script", schedule: `@at ${at}`, script: "echo once" })
      expect(task.nextRunAt).toBe(new Date(`${at}:00`).getTime() > 0 ? task.nextRunAt : task.nextRunAt) // 形态检查占位
      h.sandbox.exec = async () => ({ stdout: "done", stderr: "", code: 0 })
      await due(h, task)
      await h.cron.tick()
      const entry = internal(h, task)
      expect(entry.runCount).toBe(1)
      expect(entry.enabled).toBe(false)
      expect((await h.cron.list(user))[0].enabled).toBe(false)
      // 过去的 @at 时间创建即拒绝
      const past = new Date(h.cron["now"]() - 60_000).toISOString()
      await expect(h.cron.add(user, { type: "script", schedule: `@at ${past}`, script: "x" })).rejects.toThrow(/过去/)
    } finally {
      cleanup(h)
    }
  })

  test("misfire policy: skip recomputes on load; run keeps stale trigger for one catch-up", async () => {
    const base = 1_780_000_000_000
    const h = setup(base)
    try {
      const { user } = await createSession(h)
      const skip = await h.cron.add(user, { type: "script", schedule: "@every 1h", script: "echo skip" })
      const run = await h.cron.add(user, { type: "script", schedule: "@every 1h", script: "echo run", misfire: "run" })
      h.sandbox.exec = async (cmd) => {
        h.executed.push(cmd)
        return { stdout: "", stderr: "", code: 0 }
      }
      h.cron.stop()
      // 模拟停机错过触发点（nextRunAt 已过期——改内存后落盘，模拟「上次运行后关机」的磁盘状态）
      internal(h, skip).nextRunAt = base - 3600_000
      internal(h, run).nextRunAt = base - 3600_000
      await h.cron["saveUserEntries"](user)
      // 重启：新调度器加载同一 home
      const store = new SessionStore({ home: h.home })
      const cron2 = new CronManager({
        home: h.home,
        store,
        env: new EnvManager(store),
        sandbox: h.sandbox,
        events: new EventBus(),
        now: () => base,
        tickIntervalMs: 3600_000,
      })
      await cron2.start()
      await cron2.tick()
      // skip：错过即跳过（未执行，nextRunAt 推进到未来）
      // run：立即补跑一次
      expect(h.executed).toEqual(["echo run"])
      const listed = await cron2.list(user)
      const skipEntry = listed.find((t) => t.id === skip.id)!
      const runEntry = listed.find((t) => t.id === run.id)!
      expect(skipEntry.runCount).toBe(0)
      expect(skipEntry.nextRunAt).toBeGreaterThan(base)
      expect(runEntry.runCount).toBe(1)
      expect(runEntry.nextRunAt).toBeGreaterThan(base)
      cron2.stop()
    } finally {
      cleanup(h)
    }
  })

  test("start() reloads user tasks and disables corrupted schedules", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      const good = await h.cron.add(user, { type: "script", schedule: "@every 1h", script: "echo g" })
      h.cron.stop()
      // 外部编辑损坏 good 的表达式 + 追加一个过期任务
      const file = join(h.home, "users", "default", "cron.json")
      const raw = JSON.parse(readFileSync(file, "utf8")) as CronTask[]
      raw[0].schedule = "not cron"
      raw.push({ ...good, id: "f".repeat(32), schedule: "@every 5m", script: "echo f", nextRunAt: 1 })
      writeFileSync(file, JSON.stringify(raw))
      const store = new SessionStore({ home: h.home })
      const cron2 = new CronManager({ home: h.home, store, env: new EnvManager(store), sandbox: h.sandbox, events: new EventBus(), now: () => h.cron["now"](), tickIntervalMs: 3600_000 })
      await cron2.start()
      const listed = await cron2.list(user)
      expect(listed).toHaveLength(2)
      expect(listed.find((t) => t.id === good.id)!.enabled).toBe(false)
      expect(listed.find((t) => t.id === "f".repeat(32))!.nextRunAt).toBeGreaterThan(h.cron["now"]())
      cron2.stop()
    } finally {
      cleanup(h)
    }
  })

  test("legacy session cron.json migrated to user store and renamed", async () => {
    const h = setup()
    try {
      const { id, user } = await createSession(h)
      // 旧版布局：sessions/{s0}/{s1}/{id}/cron.json
      const legacy = join(h.home, "users", user, "sessions", id.slice(0, 2), id.slice(2, 4), id, "cron.json")
      mkdirSync(join(legacy, ".."), { recursive: true })
      const t = {
        id: "a".repeat(32),
        sessionId: id,
        user,
        type: "prompt",
        schedule: "@every 10m",
        prompt: "legacy",
        enabled: true,
        createdAt: 1,
        updatedAt: 1,
        nextRunAt: 1,
        runCount: 0,
      }
      writeFileSync(legacy, JSON.stringify([t]))
      const store = new SessionStore({ home: h.home })
      const cron2 = new CronManager({ home: h.home, store, env: new EnvManager(store), sandbox: h.sandbox, events: new EventBus(), engine: { isRunning: () => false, run: async () => {} } as unknown as AgentEngine, now: () => h.cron["now"](), tickIntervalMs: 3600_000 })
      await cron2.start()
      const listed = await cron2.list(user)
      expect(listed).toHaveLength(1)
      expect(listed[0].target).toBe("session")
      expect(listed[0].originSessionId).toBe(id)
      expect(existsSync(`${legacy}.migrated`)).toBe(true)
      expect(existsSync(legacy)).toBe(false)
      // 用户级存储已包含迁移任务
      const raw = JSON.parse(readFileSync(join(h.home, "users", user, "cron.json"), "utf8")) as CronTask[]
      expect(raw).toHaveLength(1)
      cron2.stop()
    } finally {
      cleanup(h)
    }
  })

  test("trigger runs immediately without touching nextRunAt", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      h.sandbox.exec = async () => ({ stdout: "manual", stderr: "", code: 0 })
      const task = await h.cron.add(user, { type: "script", schedule: "@every 1h", script: "echo m" })
      const before = internal(h, task).nextRunAt
      const out = await h.cron.trigger(user, task.id)
      expect(out?.runCount).toBe(1)
      expect(out?.runs![0].manual).toBe(true)
      expect(internal(h, task).nextRunAt).toBe(before)
      expect(await h.cron.trigger(user, "b".repeat(32))).toBeNull()
    } finally {
      cleanup(h)
    }
  })

  test("consecutive failure auto-disable with threshold, reset on re-enable", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      h.sandbox.exec = async () => ({ stdout: "", stderr: "x", code: 1 })
      const task = await h.cron.add(user, { type: "script", schedule: "@every 30m", script: "false", maxConsecutiveErrors: 2 })
      for (let i = 0; i < 3; i++) {
        await due(h, task)
        await h.cron.tick()
      }
      const entry = internal(h, task)
      expect(entry.runCount).toBe(2) // 第 3 次不再触发（已停用）
      expect(entry.enabled).toBe(false)
      expect(entry.lastError).toContain("连续失败 2 次")
      expect(entry.runs![0].status).toBe("error")
      // 重新启用：计数清零恢复执行
      await h.cron.update(user, task.id, { enabled: true })
      expect(internal(h, task).consecutiveErrors).toBe(0)
      await due(h, task)
      await h.cron.tick()
      expect(internal(h, task).runCount).toBe(3)
    } finally {
      cleanup(h)
    }
  })

  test("notify channels deliver results; notifyOn=error filters success; secrets masked", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      h.sandbox.exec = async () => ({ stdout: "ok-out", stderr: "", code: 0 })
      const task = await h.cron.add(user, {
        name: "notify-me",
        type: "script",
        schedule: "@every 30m",
        script: "echo ok",
        notify: [
          { type: "webhook", target: "https://example.com/hook" },
          { type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/abc", secret: "s3cret", at: ["all", { id: "ou_abc123", name: "张三" }] },
          { type: "feishu_chat", target: "oc_abcdef1234567890abcdef" },
        ],
      })
      // 列表回显脱敏 + at 归一（字符串/对象统一为 {id,name?}）
      const listed = (await h.cron.list(user))[0]
      expect(listed.notify![1].secret).toBe("***")
      expect(listed.notify![1].at).toEqual([{ id: "all" }, { id: "ou_abc123", name: "张三" }])
      expect(internal(h, task).notify![1].secret).toBe("s3cret")
      await due(h, task)
      await h.cron.tick()
      // 三通道齐投（always）；飞书通道为 markdown 卡片（含 at 标签与加签）
      expect(h.notifyPosts.map((p) => p.url)).toEqual(["https://example.com/hook", "https://open.feishu.cn/open-apis/bot/v2/hook/abc"])
      const hookBody = h.notifyPosts[0].body as CronResultNotification
      expect(hookBody.event).toBe("cron.result")
      expect(hookBody.ok).toBe(true)
      expect(hookBody.output).toContain("ok-out")
      const feishuBody = h.notifyPosts[1].body as { msg_type: string; card: { header: { template: string }; elements: Array<{ tag: string; text?: { content: string } }> } }
      expect(feishuBody.msg_type).toBe("interactive")
      expect(feishuBody.card.header.template).toBe("green")
      const feishuMd = feishuBody.card.elements.find((e) => e.tag === "div")!.text!.content
      expect(feishuMd).toContain('<at user_id="all">所有人</at>')
      expect(feishuMd).toContain('<at user_id="ou_abc123">张三</at>')
      expect(feishuMd).toContain("ok-out")
      expect(h.feishuSent).toHaveLength(1)
      expect((h.feishuSent[0].header as { template: string }).template).toBe("green")
      expect(internal(h, task).lastNotifyError).toBeUndefined()
      // notifyOn=error：成功运行不再投递
      await h.cron.update(user, task.id, { notifyOn: "error" })
      await due(h, task)
      await h.cron.tick()
      expect(h.notifyPosts).toHaveLength(2)
      expect(h.feishuSent).toHaveLength(1)
      // 更新时 secret 掩码 *** 保持原值
      const kept = await h.cron.update(user, task.id, {
        notify: [
          { type: "webhook", target: "https://example.com/hook" },
          { type: "feishu", target: "https://open.feishu.cn/open-apis/bot/v2/hook/abc", secret: "***" },
        ],
      })
      expect(internal(h, task).notify![1].secret).toBe("s3cret")
      expect(kept?.notify![1].secret).toBe("***")
    } finally {
      cleanup(h)
    }
  })

  test("notify delivery failure recorded without failing the run", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      h.sandbox.exec = async () => ({ stdout: "fine", stderr: "", code: 0 })
      const origImpl = h.cron["deps"].notify
      h.cron["deps"].notify = { ...origImpl, fetchImpl: async () => ({ ok: false, status: 503 }) }
      const task = await h.cron.add(user, { type: "script", schedule: "@every 30m", script: "echo x", notify: [{ type: "webhook", target: "https://example.com/hook" }] })
      await due(h, task)
      await h.cron.tick()
      const entry = internal(h, task)
      expect(entry.lastStatus).toBe("success")
      expect(entry.lastNotifyError).toContain("503")
    } finally {
      cleanup(h)
    }
  })

  test("prompt result publishes event with sessionId and captures assistant summary", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      const results: Array<Record<string, unknown>> = []
      h.events.subscribe((ev) => {
        if (ev.type === "event.cron.result") results.push(ev.payload as Record<string, unknown>)
      })
      // fake run 写入 assistant 消息（结果摘要来源）
      h.cron["engine"] = {
        isRunning: () => false,
        run: async (sid: string) => {
          h.runCalls.push(sid)
          await h.store.appendMessage(sid, { id: "m1", role: "assistant", content: "报告完成：一切正常", createdAt: Date.now() }, user)
        },
      } as unknown as AgentEngine
      const task = await h.cron.add(user, { name: "报告", type: "prompt", schedule: "@every 30m", prompt: "生成报告" })
      await due(h, task)
      await h.cron.tick()
      expect(results).toHaveLength(1)
      expect(results[0].sessionId).toBeTruthy()
      expect(internal(h, task).lastOutput).toContain("一切正常")
    } finally {
      cleanup(h)
    }
  })

  test("tick failure reschedules nextRunAt (no infinite retry loop)", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      const task = await h.cron.add(user, { type: "script", schedule: "@every 30m", script: "x" })
      await due(h, task)
      h.sandbox.exec = async () => {
        throw new Error("disk exploded")
      }
      await h.cron.tick()
      const entry = internal(h, task)
      expect(entry.lastStatus).toBe("error")
      expect(entry.lastError).toContain("disk exploded")
      expect(entry.nextRunAt).toBeGreaterThan(h.cron["now"]())
    } finally {
      cleanup(h)
    }
  })

  test("disabled task is not fired", async () => {
    const h = setup()
    try {
      const { user } = await createSession(h)
      const task = await h.cron.add(user, { type: "script", schedule: "@every 30m", script: "echo x" })
      await h.cron.update(user, task.id, { enabled: false })
      await due(h, task)
      await h.cron.tick()
      expect(internal(h, task).runCount).toBe(0)
    } finally {
      cleanup(h)
    }
  })
})
