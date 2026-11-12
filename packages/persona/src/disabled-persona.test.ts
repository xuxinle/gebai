/**
 * **未启用的人格零行为**：启用名单是唯一闸门，且闸门在**装配之前**。
 *
 * 为什么需要这条：人格的行为有多个出口，闸门若只挡其中一两个，就会出现“半启用”——
 * 最难查的一类，因为面板上看起来一切正常，而它正在收全部群消息或按 cron 跑。
 *
 * 出口清单（本用例逐个咬住）：
 * 1. **收事件**：`catalog.subscribers()`（广播路由的第一道判据）
 * 2. **跑定时**：`collectScheduleDeclarations(catalog.all())`（schedule 源的声明来源）
 * 3. **被诊断当成在用**：`catalog.ids()` / `get()`（面板与状态快照的数据来源）
 * 4. **定向投递绕过**：事件显式带 `personaId` 也不能把它叫醒
 *
 * 这四处全部同源于「catalog 里有没有它」，而 catalog 由 `applyPersonaEnv` 的输出构建——
 * 所以闸门只有一处，但**每个出口都要有断言**：将来谁在某个出口绕开 catalog 取定义，
 * 这里就会红。
 */
import { describe, expect, test } from "bun:test"
import { PersonaCatalog } from "./catalog"
import { applyPersonaEnv } from "./env-config"
import type { PersonaDef } from "./persona"
import { collectScheduleDeclarations } from "./schedules"

/** 一个「零配置也能跑」的人格：订阅与定时都写在代码定义里（这正是要显式启用的理由）。 */
const BARE: PersonaDef = {
  id: "probe",
  name: "探针",
  identity: "你是探针",
  role: "取证",
  drivers: [
    {
      id: "main",
      focus: "收全部消息 + 每分钟跑一次",
      subscriptions: ["message", "schedule"],
      schedules: [{ id: "tick", cron: "@every 1m", prompt: "报个到" }],
    },
  ],
}

const ENABLED: PersonaDef = {
  id: "xiaoshuang",
  name: "小霜",
  identity: "你是小霜",
  role: "运维",
  drivers: [{ id: "support", focus: "答疑", subscriptions: ["message"] }],
}

/** 走真实装配路径：env → applyPersonaEnv → catalog（compose 里就是这样接的）。 */
function catalogOf(env: Record<string, string>): PersonaCatalog {
  const { personas } = applyPersonaEnv(new Map([[BARE.id, BARE], [ENABLED.id, ENABLED]]), env)
  return new PersonaCatalog({ personas })
}

/** 一条群消息事件（能命中裸 `message` 订阅的最宽形态）。 */
const MESSAGE = { kind: "message", channel: "feishu_in", context: { refs: { chatId: "oc_any" } } } as never

describe("未启用的人格：零行为（启用名单是唯一闸门）", () => {
  test("catalog 里根本没有它（后面三个出口全部同源）", () => {
    const c = catalogOf({ GEBAI_PERSONA_ENABLE: "xiaoshuang" })
    console.log(`    实测: catalog=${JSON.stringify(c.ids())}`)
    expect(c.ids()).toEqual(["xiaoshuang"])
    expect(c.get("probe")).toBeNull()
  })

  test("出口 ①收事件：裸 message 订阅也收不到（不广播给它）", () => {
    const c = catalogOf({ GEBAI_PERSONA_ENABLE: "xiaoshuang" })
    const subs = c.subscribers(MESSAGE)
    console.log(`    实测: subscribers=${JSON.stringify(subs.map((s) => s.personaId))}`)
    expect(subs.map((s) => s.personaId)).toEqual(["xiaoshuang"])
    // 反证：若它被启用，裸 message 会让它收到任何群的消息
    const all = catalogOf({ GEBAI_PERSONA_ENABLE: "xiaoshuang,probe" })
    expect(all.subscribers(MESSAGE).map((s) => s.personaId).sort()).toEqual(["probe", "xiaoshuang"])
  })

  test("出口 ②跑定时：schedules 声明不进源（cron 到点也不会被唤醒）", () => {
    const c = catalogOf({ GEBAI_PERSONA_ENABLE: "xiaoshuang" })
    const decls = collectScheduleDeclarations(
      c.all().map((p) => ({ id: p.id, drivers: (p.drivers ?? []).map((d) => ({ id: d.id, ...(d.schedules ? { schedules: d.schedules } : {}) })) })),
    )
    console.log(`    实测: 声明=${JSON.stringify(decls.map((d) => `${d.personaId}/${d.driverId}/${d.schedule.id}`))}`)
    expect(decls.filter((d) => d.personaId === "probe")).toEqual([])
    // 反证：一旦启用，它的 @every 1m 立刻进源
    const all = catalogOf({ GEBAI_PERSONA_ENABLE: "xiaoshuang,probe" })
    const allDecls = collectScheduleDeclarations(all.all().map((p) => ({ id: p.id, drivers: (p.drivers ?? []).map((d) => ({ id: d.id, ...(d.schedules ? { schedules: d.schedules } : {}) })) })))
    expect(allDecls.map((d) => d.personaId)).toContain("probe")
  })

  test("出口 ③诊断不当它是在用：ids/all 里都没有它", () => {
    const c = catalogOf({ GEBAI_PERSONA_ENABLE: "xiaoshuang" })
    expect(c.all().map((p) => p.id)).toEqual(["xiaoshuang"])
  })

  test("出口 ④定向投递绕不过：事件显式带 personaId 也叫不醒它", () => {
    const c = catalogOf({ GEBAI_PERSONA_ENABLE: "xiaoshuang" })
    const targeted = { ...(MESSAGE as object), personaId: "probe" } as never
    console.log(`    实测: 定向 probe → ${JSON.stringify(c.subscribers(targeted).map((s) => s.personaId))}`)
    // 定向投递只看“这个 persona 在不在 catalog”，不在就无人关注——而不是“按 id 反查定义”
    expect(c.subscribers(targeted)).toEqual([])
  })

  test("**一个都没写名单** → 全部零行为（连已有人格也不干活）", () => {
    const c = catalogOf({})
    expect(c.ids()).toEqual([])
    expect(c.subscribers(MESSAGE)).toEqual([])
    expect(c.get("xiaoshuang")).toBeNull()
  })

  test("名单未命中（拼错 id）→ 同样全禁用，不是“回落到全部”", () => {
    const c = catalogOf({ GEBAI_PERSONA_ENABLE: "xiaoshunag" })
    expect(c.ids()).toEqual([])
  })
})
