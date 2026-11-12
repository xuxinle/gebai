/**
 * 定时表达式可由环境变量覆盖（`schedules`），未配置时用代码定义里的值。
 *
 * 为什么要这条能力：cron 是「几点跑」的**部署差异**（开发环境想每分钟跑一次、
 * 生产环境要 8 点跑），而声明本身（有没有这条任务、id 叫什么）是**代码事实**。
 * 所以这里只允许**覆盖已有声明的字段**，不允许新增声明——
 * 否则「id 拼错」会变成一个凭空多出的定时任务（静默错），而不是一条可读的报错。
 */
import { describe, expect, test } from "bun:test"
import { applyPersonaEnv } from "./env-config"
import type { PersonaDef } from "./persona"

const DEF: PersonaDef = {
  id: "xiaoshuang",
  name: "小霜",
  identity: "你是小霜",
  role: "运维",
  drivers: [
    {
      id: "report",
      focus: "写日报",
      subscriptions: ["schedule", "message"],
      schedules: [
        { id: "morning", cron: "0 9 * * 1-5", prompt: "写今天的早报", description: "工作日早报" },
        { id: "weekly", cron: "0 10 * * 1", prompt: "写周报", timezone: "Asia/Shanghai" },
      ],
    },
  ],
}

const run = (items: unknown[]) =>
  applyPersonaEnv(new Map([[DEF.id, DEF]]), {
    GEBAI_PERSONA_ENABLE: "xiaoshuang",
    GEBAI_PERSONA_XIAOSHUANG: JSON.stringify(items),
  })

const schedsOf = (personas: Map<string, PersonaDef>) => personas.get("xiaoshuang")!.drivers![0]!.schedules!

describe("定时表达式可被环境变量覆盖", () => {
  test("不配置 → 原样使用代码定义（缺省行为不变）", () => {
    const { personas, report } = run([{ name: "report" }])
    const s = schedsOf(personas)
    console.log(`    实测: ${JSON.stringify(s.map((x) => [x.id, x.cron]))}`)
    expect(s).toEqual(DEF.drivers![0]!.schedules!)
    expect(report.errors).toEqual([])
  })

  test("只改 cron → 其余字段（prompt/description）保持不变", () => {
    const { personas, report } = run([{ name: "report", schedules: { morning: { cron: "0 8 * * *" } } }])
    const s = schedsOf(personas)
    console.log(`    实测: ${JSON.stringify(s)}`)
    const morning = s.find((x) => x.id === "morning")!
    expect(morning.cron).toBe("0 8 * * *")
    // 关键：没写的字段必须是代码里的值，不能被清空
    expect(morning.prompt).toBe("写今天的早报")
    expect(morning.description).toBe("工作日早报")
    expect(report.scheduleOverrides["xiaoshuang/report"]).toBe(1)
    expect(report.errors).toEqual([])
  })

  test("同时改 cron + prompt + timezone", () => {
    const { personas } = run([
      {
        name: "report",
        schedules: { weekly: { cron: "@every 30m", prompt: "巡检", timezone: "UTC" } },
      },
    ])
    const w = schedsOf(personas).find((x) => x.id === "weekly")!
    expect(w).toEqual({ id: "weekly", cron: "@every 30m", prompt: "巡检", timezone: "UTC" })
  })

  test("enabled:false → 该声明不触发，但定义里的其他声明照常", () => {
    const { personas } = run([{ name: "report", schedules: { morning: { enabled: false } } }])
    const s = schedsOf(personas)
    console.log(`    实测: ${JSON.stringify(s.map((x) => x.id))}`)
    expect(s.map((x) => x.id)).toEqual(["weekly"])
  })

  test("引用不存在的声明 id → 报错（不静默忽略）", () => {
    const { report } = run([{ name: "report", schedules: { nope: { cron: "0 1 * * *" } } }])
    console.log(`    实测: ${JSON.stringify(report.errors)}`)
    expect(report.errors.some((e) => e.includes("不存在的声明「nope」"))).toBe(true)
    // 错误里列出可用 id，便于直接照着改
    expect(report.errors.some((e) => e.includes("morning"))).toBe(true)
  })

  test("代码里没有 schedules 却配了覆盖 → 报错（覆盖无处可施）", () => {
    const noSched: PersonaDef = {
      ...DEF,
      drivers: [{ id: "report", focus: "写日报", subscriptions: ["schedule"] }],
    }
    const { report } = applyPersonaEnv(
      new Map([[noSched.id, noSched]]),
      { GEBAI_PERSONA_ENABLE: "xiaoshuang", GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([{ name: "report", schedules: { morning: { cron: "0 8 * * *" } } }]) },
    )
    console.log(`    实测: ${JSON.stringify(report.errors)}`)
    expect(report.errors.some((e) => e.includes("覆盖无处可施"))).toBe(true)
  })
})

describe("schedules 的结构校验", () => {
  test("不是对象 → 报错", () => {
    const { report } = run([{ name: "report", schedules: ["0 9 * * *"] }])
    expect(report.errors.some((e) => e.includes("schedules 必须是对象"))).toBe(true)
  })

  test("未知键 → 报错（拼错的 cron 不能静默失效）", () => {
    const { report } = run([{ name: "report", schedules: { morning: { crontab: "0 8 * * *" } } }])
    console.log(`    实测: ${JSON.stringify(report.errors)}`)
    expect(report.errors.some((e) => e.includes("含未知键「crontab」"))).toBe(true)
  })

  test("空对象 → 报错（写了但什么都没配，属于拼错的高发形态）", () => {
    const { report } = run([{ name: "report", schedules: { morning: {} } }])
    expect(report.errors.some((e) => e.includes("空对象"))).toBe(true)
  })

  test("cron 不是字符串 → 报错", () => {
    const { report } = run([{ name: "report", schedules: { morning: { cron: 9 } } }])
    expect(report.errors.some((e) => e.includes("cron 必须是非空字符串"))).toBe(true)
  })

  test("人格级默认项里的 schedules 会被分身继承", () => {
    const { personas } = applyPersonaEnv(
      new Map([[DEF.id, DEF]]),
      {
        GEBAI_PERSONA_ENABLE: "xiaoshuang",
        GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([
          { schedules: { morning: { cron: "0 7 * * *" } } }, // 不带 name = 人格级默认
          { name: "report" },
        ]),
      },
    )
    const s = schedsOf(personas)
    console.log(`    实测: morning.cron=${s.find((x) => x.id === "morning")!.cron}`)
    expect(s.find((x) => x.id === "morning")!.cron).toBe("0 7 * * *")
  })
})
