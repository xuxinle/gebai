/**
 * 定时任务启停控制面专项测试。
 *
 * 这里的每条断言都对应一个**静默失败**形态——它们不会报错，只会让人以为"已经设好了"：
 *
 * 1. 缺省启用：上这个功能不能把正在跑的定时停掉（一次重启后"我的定时没了"是最糟的回归）
 * 2. 开关持久：重启后仍生效（否则"临时关停"等于没关）
 * 3. 坏文件当缺省：手抖的 JSON 不该把整套定时全停掉
 * 4. 手动运行不改启用状态：点一次"立即跑"不该把停用的变成启用
 * 5. key 与 scheduleKey 逐字一致：两边各拼一次会出现"关的是这条、生效的是另一条"
 */
import { describe, expect, test } from "bun:test"
import {
  MemoryScheduleControlPersistence,
  ScheduleControl,
  manualRunEventId,
  parseScheduleAction,
  type ScheduleControlPersistence,
  type ScheduleControlState,
} from "./schedule-control"
import { scheduleKey, type ScheduleDeclaration } from "./schedules"

const decl = (over: Partial<ScheduleDeclaration> = {}): ScheduleDeclaration => ({
  personaId: "xiaoshuang",
  driverId: "report",
  personaName: "小霜",
  schedule: { id: "morning", cron: "0 9 * * *", prompt: "写今天的早报" },
  ...over,
})

const KEY = "xiaoshuang/report/morning"

const make = async (initial: ScheduleControlState = {}) => {
  const ctl = new ScheduleControl(new MemoryScheduleControlPersistence(initial))
  await ctl.load()
  return ctl
}

describe("缺省启用（语义 1）", () => {
  test("无记录 → 启用", async () => {
    expect((await make()).isEnabled(KEY)).toBe(true)
  })

  test("记录里 enabled:false → 停用", async () => {
    const ctl = await make({ [KEY]: { enabled: false, updatedAt: 1 } })
    expect(ctl.isEnabled(KEY)).toBe(false)
  })

  test("记录里缺 enabled 字段 → 按启用（坏数据不静默停摆）", async () => {
    // 手写文件 / 旧版本残留都可能缺这个字段
    const ctl = await make({ [KEY]: { updatedAt: 1 } as never })
    expect(ctl.isEnabled(KEY)).toBe(true)
  })

  test("filterEnabled：只剪被停用的，返回新数组（不原地改）", async () => {
    const ctl = await make({ [KEY]: { enabled: false, updatedAt: 1 } })
    const a = decl()
    const b = decl({ driverId: "other" })
    const input = [a, b]
    const out = ctl.filterEnabled(input)
    expect(out).toHaveLength(1)
    expect(out[0]!.driverId).toBe("other")
    // 上游的声明视图不能被就地改动（热加载 diff 会误报"少了定时"）
    expect(input).toHaveLength(2)
    expect(out).not.toBe(input)
  })

  test("空 key 不当作停用（isEnabled 只查表，不做业务判断）", async () => {
    expect((await make()).isEnabled("")).toBe(true)
    // 但写操作必须拒绝空 key
    const ctl = await make()
    expect((await ctl.setEnabled("   ", false)).ok).toBe(false)
  })
})

describe("开关持久（语义 2）", () => {
  test("关闭后重新 load 仍是关闭", async () => {
    const p = new MemoryScheduleControlPersistence()
    const a = new ScheduleControl(p)
    await a.load()
    await a.setEnabled(KEY, false)
    // 模拟重启：同一份持久化，新的控制面实例
    const b = new ScheduleControl(p)
    await b.load()
    expect(b.isEnabled(KEY)).toBe(false)
  })

  test("setEnabled 返回新状态", async () => {
    const ctl = await make()
    expect(await ctl.setEnabled(KEY, false)).toMatchObject({ ok: true, key: KEY, enabled: false })
    expect(await ctl.setEnabled(KEY, true)).toMatchObject({ ok: true, enabled: true })
    expect(ctl.isEnabled(KEY)).toBe(true)
  })

  test("并发点两个开关不丢记录（整文件覆写的经典坑）", async () => {
    const ctl = await make()
    await Promise.all([
      ctl.setEnabled("a/x/1", false),
      ctl.setEnabled("b/y/2", false),
      ctl.setEnabled("c/z/3", false),
    ])
    const snap = ctl.snapshot()
    expect(Object.keys(snap).sort()).toEqual(["a/x/1", "b/y/2", "c/z/3"])
    expect(Object.values(snap).every((e) => e.enabled === false)).toBe(true)
  })

  test("写盘失败不让接口报错（只影响重启后记不记得）", async () => {
    const failing: ScheduleControlPersistence = {
      load: async () => ({}),
      save: async () => {
        throw new Error("磁盘只读")
      },
    }
    const ctl = new ScheduleControl(failing)
    await ctl.load()
    // 内存态照旧生效：这次进程内关掉了就是关掉了
    expect(await ctl.setEnabled(KEY, false)).toMatchObject({ ok: true })
    expect(ctl.isEnabled(KEY)).toBe(false)
  })
})

describe("坏文件不影响运行（语义 3）", () => {
  test("load 抛错 → 当作全缺省（全部启用）", async () => {
    const broken: ScheduleControlPersistence = {
      load: async () => {
        throw new Error("JSON 坏了")
      },
      save: async () => {},
    }
    const ctl = new ScheduleControl(broken)
    await ctl.load()
    expect(ctl.isEnabled(KEY)).toBe(true)
    expect(ctl.filterEnabled([decl()])).toHaveLength(1)
  })

  test("重复 load 是幂等的（不会把手上的改动冲掉）", async () => {
    const ctl = await make()
    await ctl.setEnabled(KEY, false)
    await ctl.load() // 第二次：应直接返回，不重新读盘
    expect(ctl.isEnabled(KEY)).toBe(false)
  })
})

describe("手动运行（语义 4）", () => {
  test("markRun 不改变启用状态：停用的仍是停用", async () => {
    const ctl = await make({ [KEY]: { enabled: false, updatedAt: 1 } })
    await ctl.markRun(KEY)
    expect(ctl.isEnabled(KEY)).toBe(false)
    expect(ctl.snapshot()[KEY]?.lastRunAt).toBeGreaterThan(0)
  })

  test("markRun 不改 enabled 字段（启用仍是启用）", async () => {
    const ctl = await make()
    await ctl.markRun(KEY)
    expect(ctl.isEnabled(KEY)).toBe(true)
    expect(ctl.snapshot()[KEY]?.lastRunAt).toBeGreaterThan(0)
  })

  test("setEnabled 保留已有的 lastRunAt（两个字段互不覆盖）", async () => {
    const ctl = await make()
    await ctl.markRun(KEY)
    const first = ctl.snapshot()[KEY]!.lastRunAt
    await ctl.setEnabled(KEY, false)
    expect(ctl.snapshot()[KEY]!.lastRunAt).toBe(first)
    expect(ctl.snapshot()[KEY]!.enabled).toBe(false)
  })

  test("manualRunEventId：同一毫秒同 id（重复点击可接受），不同毫秒不同 id", () => {
    const a = manualRunEventId("p", "d", "s", 1000)
    const b = manualRunEventId("p", "d", "s", 1000)
    const c = manualRunEventId("p", "d", "s", 1001)
    expect(a).toBe(b)
    expect(a).not.toBe(c)
    // 前缀与真实档期事件区分开（否则手动触发会被幂等去重吃掉）
    expect(a).toContain("@manual:")
  })
})

describe("key 一致性（语义 5）", () => {
  test("控制面用的 key 与 scheduleKey() 逐字一致", async () => {
    const ctl = await make()
    const d = decl()
    const key = scheduleKey(d)
    expect(key).toBe(KEY)
    await ctl.setEnabled(key, false)
    expect(ctl.isEnabled(scheduleKey(d))).toBe(false)
    // 且 filterEnabled 用的是同一个 key（关掉后确实被剪掉）
    expect(ctl.filterEnabled([d])).toHaveLength(0)
  })

  test("同名人格不同分身互不干扰（key 必须含 driverId）", async () => {
    const ctl = await make()
    await ctl.setEnabled("p1/a/s", false)
    expect(ctl.isEnabled("p1/b/s")).toBe(true)
  })
})

describe("parseScheduleAction", () => {
  test("认识三个动作", () => {
    expect(parseScheduleAction("enable")).toBe("enable")
    expect(parseScheduleAction("disable")).toBe("disable")
    expect(parseScheduleAction("run")).toBe("run")
  })

  test("未知/大小写不符 → null（不猜）", () => {
    expect(parseScheduleAction("Enable")).toBeNull()
    expect(parseScheduleAction("restart")).toBeNull()
    expect(parseScheduleAction("")).toBeNull()
  })
})
