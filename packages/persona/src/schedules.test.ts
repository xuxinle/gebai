/**
 * 人格定时工作面（`DriverDef.schedules`）专项测试：档期计算、幂等键、加载期校验、内置源 tick。
 *
 * 三条最容易被写错的语义，逐条钉住（它们的失败形态都是「静默不触发」）：
 *
 * 1. **首次不回溯**：加一条 `0 9 * * *` 不该在下午立刻触发一次
 * 2. **落后有上限**：休眠醒来后不能一次喷出成百上千条（队列会被打满，真实消息全被 overflow 丢）
 * 3. **幂等按档期时刻**：同一档期重投必被丢弃，不同分身同一时刻互不干扰
 */
import { describe, expect, test } from "bun:test"
import { validatePersonaDef, type PersonaDef } from "./persona"
import {
  DEFAULT_CATCHUP_MS,
  MAX_SLOTS_PER_TICK,
  collectDue,
  collectScheduleDeclarations,
  scheduleEventId,
  scheduleKey,
  type ScheduleDeclaration,
} from "./schedules"
import type { DriverSchedule } from "./ports"
import { source as scheduleSource } from "./sources/schedule"
import type { EmitResult, EventSourceContext, IngressEvent } from "./source"

const sch = (over: Partial<DriverSchedule> = {}): DriverSchedule => ({
  id: "morning",
  cron: "0 9 * * *",
  prompt: "写今天的早报",
  ...over,
})

const decl = (over: Partial<ScheduleDeclaration> = {}): ScheduleDeclaration => ({
  personaId: "xiaoshuang",
  driverId: "report",
  personaName: "小霜",
  schedule: sch(),
  ...over,
})

/** 本地时间 2026-08-06 08:00（与 cron 同一时区基准，避免时区差异导致断言飘） */
const AT_0800 = new Date(2026, 7, 6, 8, 0, 0, 0).getTime()
const AT_0900 = new Date(2026, 7, 6, 9, 0, 0, 0).getTime()

describe("collectDue：档期推进", () => {
  test("首次见到声明不回溯（游标从此刻起算）", () => {
    const cursors = new Map<string, number>()
    const r = collectDue({ declarations: [decl()], cursors, now: AT_0800 })
    expect(r.events).toEqual([])
    expect(r.skipped).toEqual([])
    // 游标落在下一次 9:00
    expect(cursors.get(scheduleKey(decl()))).toBe(AT_0900)
  })

  test("到点触发一次，且只触发一次（同理由幂等键保证）", () => {
    const cursors = new Map<string, number>()
    collectDue({ declarations: [decl()], cursors, now: AT_0800 })
    const r = collectDue({ declarations: [decl()], cursors, now: AT_0900 })
    expect(r.events).toHaveLength(1)
    expect(r.events[0]!.at).toBe(AT_0900)
    expect(r.events[0]!.id).toBe(scheduleEventId("xiaoshuang", "report", "morning", AT_0900))
    // 同一时刻再算一次：不重复
    expect(collectDue({ declarations: [decl()], cursors, now: AT_0900 }).events).toEqual([])
  })

  test("misfire:run = 首次即补跑一次（游标同样前移，不会每 tick 都补）", () => {
    const cursors = new Map<string, number>()
    const d = decl({ schedule: sch({ misfire: "run" }) })
    const r = collectDue({ declarations: [d], cursors, now: AT_0800 })
    expect(r.events).toHaveLength(1)
    expect(collectDue({ declarations: [d], cursors, now: AT_0800 }).events).toEqual([])
  })

  test("落后窗口内补跑多条（按计划时刻逐个给，幂等键各不同）", () => {
    const cursors = new Map<string, number>()
    const every = decl({ schedule: sch({ cron: "@every 1m" }) })
    collectDue({ declarations: [every], cursors, now: AT_0800 })
    // 2 分钟后：两条档期（0801、0802）
    const r = collectDue({ declarations: [every], cursors, now: AT_0800 + 2 * 60_000 })
    expect(r.events).toHaveLength(2)
    expect(new Set(r.events.map((e) => e.id)).size).toBe(2)
  })

  test("落后超过补跑窗口：整段跳过并如实记账（不喷事件洪流）", () => {
    const cursors = new Map<string, number>()
    const every = decl({ schedule: sch({ cron: "@every 1m" }) })
    collectDue({ declarations: [every], cursors, now: AT_0800 })
    // 一小时后再 tick：只补最近窗口内的档期，其余记为 skipped
    const now = AT_0800 + 60 * 60_000
    const r = collectDue({ declarations: [every], cursors, now })
    expect(r.skipped).toHaveLength(1)
    expect(r.events.length).toBeLessThanOrEqual(Math.ceil(DEFAULT_CATCHUP_MS / 60_000) + 1)
    expect(r.events.length).toBeGreaterThan(0)
  })

  test("单 tick 补发上限（@every 1s 之类不会把队列打满）", () => {
    const cursors = new Map<string, number>()
    const fast = decl({ schedule: sch({ cron: "@every 1s" }) })
    collectDue({ declarations: [fast], cursors, now: AT_0800 })
    // 模拟进程被卡住 120s（仍在 5 分钟补跑窗口内，但积压 120 档 > 单 tick 上限）
    const r = collectDue({ declarations: [fast], cursors, now: AT_0800 + 120_000 })
    expect(r.events.length).toBe(MAX_SLOTS_PER_TICK)
    expect(r.skipped.length).toBeGreaterThan(0)
  })

  test("声明被删掉时游标一并清理（重新加回不会继承旧游标）", () => {
    const cursors = new Map<string, number>()
    collectDue({ declarations: [decl()], cursors, now: AT_0800 })
    expect(cursors.size).toBe(1)
    collectDue({ declarations: [], cursors, now: AT_0800 })
    expect(cursors.size).toBe(0)
  })

  test("解析失败只影响该条（错误如实上报，其余照常触发）", () => {
    const cursors = new Map<string, number>()
    const broken = decl({ driverId: "ops", schedule: sch({ id: "bad", cron: "0 0 30 2 *" }) })
    const ok = decl()
    const r1 = collectDue({ declarations: [broken, ok], cursors, now: AT_0800 })
    expect(r1.errors).toHaveLength(1)
    expect(r1.errors[0]!.error).toContain("无法计算")
    expect(cursors.get(scheduleKey(ok))).toBe(AT_0900)
    const r2 = collectDue({ declarations: [broken, ok], cursors, now: AT_0900 })
    expect(r2.events.map((e) => e.driverId)).toEqual(["report"])
  })
})

describe("事件形态（模型据此知道“被定时唤醒”）", () => {
  test("来源/摘要带人格名与声明名，refs 带 taskId 与 cron", () => {
    const cursors = new Map<string, number>()
    collectDue({ declarations: [decl({ schedule: sch({ description: "工作日早报" }) })], cursors, now: AT_0800 })
    const [e] = collectDue({ declarations: [decl({ schedule: sch({ description: "工作日早报" }) })], cursors, now: AT_0900 }).events
    expect(e!.personaId).toBe("xiaoshuang")
    expect(e!.driverId).toBe("report")
    expect(e!.source).toContain("工作日早报")
    expect(e!.source).toContain("小霜")
    expect(e!.summary).toContain("0 9 * * *")
    expect(e!.refs.taskId).toBe("morning")
    expect(e!.refs.extra?.cron).toBe("0 9 * * *")
    expect(e!.body).toBe("写今天的早报")
  })

  test("幂等键按「人格/分身/声明/档期时刻」——不同分身同一时刻互不干扰", () => {
    expect(scheduleEventId("a", "d1", "s", 5)).not.toBe(scheduleEventId("a", "d2", "s", 5))
    expect(scheduleEventId("a", "d1", "s", 5)).not.toBe(scheduleEventId("a", "d1", "s2", 5))
    expect(scheduleEventId("a", "d1", "s", 5)).not.toBe(scheduleEventId("a", "d1", "s", 6))
  })
})

describe("加载期校验（validatePersonaDef）", () => {
  const DEF = (driver: Record<string, unknown>): PersonaDef => ({
    id: "xiaoshuang",
    name: "小霜",
    identity: "你是小霜",
    role: "应答",
    drivers: [driver as never],
  })

  test("合法声明通过", () => {
    expect(
      validatePersonaDef(
        DEF({ id: "report", focus: "写日报", subscriptions: ["schedule", "message"], schedules: [sch()] }),
        "xiaoshuang",
      ),
    ).toBeNull()
  })

  test("声明了 schedules 但订阅不含 schedule → 加载期报错（否则永不触发且零告警）", () => {
    const err = validatePersonaDef(DEF({ id: "report", focus: "写日报", subscriptions: ["message"], schedules: [sch()] }), "xiaoshuang")
    expect(err).toContain("schedule")
    expect(err).toContain("永不执行")
  })

  test("通配订阅 * 也算覆盖", () => {
    expect(validatePersonaDef(DEF({ id: "report", focus: "f", subscriptions: ["*"], schedules: [sch()] }), "xiaoshuang")).toBeNull()
  })

  test("id 形如 [a-z0-9_]+ 且分身内唯一", () => {
    expect(validatePersonaDef(DEF({ id: "r", focus: "f", subscriptions: ["schedule"], schedules: [sch({ id: "Morning" })] }), "xiaoshuang")).toContain("[a-z0-9_]+")
    expect(
      validatePersonaDef(DEF({ id: "r", focus: "f", subscriptions: ["schedule"], schedules: [sch(), sch()] }), "xiaoshuang"),
    ).toContain("重复")
  })

  test("cron / timezone 非法在加载期报错（不让它落到“运行期永不触发”）", () => {
    expect(validatePersonaDef(DEF({ id: "r", focus: "f", subscriptions: ["schedule"], schedules: [sch({ cron: "0 9 * *" })] }), "xiaoshuang")).toContain("表达式无效")
    expect(
      validatePersonaDef(DEF({ id: "r", focus: "f", subscriptions: ["schedule"], schedules: [sch({ timezone: "Mars/Olympus" })] }), "xiaoshuang"),
    ).toContain("表达式无效")
  })

  test("未知字段拒绝（拼错的字段会静默失效）", () => {
    expect(
      validatePersonaDef(DEF({ id: "r", focus: "f", subscriptions: ["schedule"], schedules: [{ ...sch(), cronExpr: "0 9 * * *" }] }), "xiaoshuang"),
    ).toContain("未知字段")
  })

  test("prompt 必填（到点不知道干什么）", () => {
    expect(validatePersonaDef(DEF({ id: "r", focus: "f", subscriptions: ["schedule"], schedules: [sch({ prompt: " " })] }), "xiaoshuang")).toContain("prompt")
  })

  test("一次性 @at 已过去 → 拒绝（放行等于加进去立刻触发一次）", () => {
    const past = "2020-01-01T09:00"
    expect(validatePersonaDef(DEF({ id: "r", focus: "f", subscriptions: ["schedule"], schedules: [sch({ cron: `@at ${past}` })] }), "xiaoshuang")).toContain("已过去")
  })
})

describe("collectScheduleDeclarations：从人格定义收声明", () => {
  test("逐分身展开，带人格名", () => {
    const out = collectScheduleDeclarations([
      { id: "p1", name: "小霜", drivers: [{ id: "a", schedules: [sch(), sch({ id: "evening", cron: "0 21 * * *" })] }, { id: "b" }] },
      { id: "p2", drivers: [{ id: "c", schedules: [sch()] }] },
    ])
    expect(out).toHaveLength(3)
    expect(out[0]).toMatchObject({ personaId: "p1", driverId: "a", personaName: "小霜" })
    expect(out[2]).toMatchObject({ personaId: "p2", driverId: "c" })
    expect(out[2]!.personaName).toBeUndefined()
  })
})

describe("内置源 schedule：tick 行为", () => {
  /** 最小 ctx 替身：记录 emit 与日志。 */
  function ctxWith(
    reader: unknown,
    reply: EmitResult = { status: "accepted" },
  ): { ctx: EventSourceContext; emitted: IngressEvent[]; logs: string[] } {
    const emitted: IngressEvent[] = []
    const logs: string[] = []
    const ctx: EventSourceContext = {
      name: "schedule",
      home: "/tmp",
      deps: reader ? { personaSchedules: reader } : {},
      emit: async (e: IngressEvent): Promise<EmitResult> => {
        emitted.push(e)
        return reply
      },
      log: (level, msg) => logs.push(`${level}:${msg}`),
      env: () => undefined,
    }
    return { ctx, emitted, logs }
  }

  test("未注入 personaSchedules 时如实 warn（不静默空转）", async () => {
    const { ctx, logs } = ctxWith(undefined)
    await scheduleSource.start(ctx)
    await scheduleSource.stop?.()
    expect(logs.some((l) => l.includes("未通电"))).toBe(true)
  })

  test("首次 tick 就投递（misfire:run），事件带定向 driverId 与幂等键", async () => {
    const reader = () => [decl({ schedule: sch({ misfire: "run" }) })]
    const { ctx, emitted, logs } = ctxWith(reader)
    await scheduleSource.start(ctx)
    // 首次 tick 是同步链上的 `void tick()`，给它一个微任务窗口把 emit 跑完
    await new Promise((r) => setTimeout(r, 50))
    await scheduleSource.stop?.()
    expect(logs.some((l) => l.includes("1 条声明"))).toBe(true)
    expect(emitted).toHaveLength(1)
    expect(emitted[0]).toMatchObject({ kind: "schedule", channel: "schedule", personaId: "xiaoshuang", driverId: "report" })
    expect(emitted[0]!.id).toStartWith("task:persona:xiaoshuang/report/morning@")
  })

  test("非 accepted 结果留痕（overflow/unavailable 不被静默吞掉）", async () => {
    const reader = () => [decl({ schedule: sch({ misfire: "run" }) })]
    const { ctx, logs } = ctxWith(reader, { status: "overflow", reason: "队列已满" })
    await scheduleSource.start(ctx)
    await new Promise((r) => setTimeout(r, 50))
    await scheduleSource.stop?.()
    expect(logs.some((l) => l.includes("队列已满"))).toBe(true)
  })
})