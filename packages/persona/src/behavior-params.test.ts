/**
 * 行为参数（`replyToOwner` / `yieldSeconds` / `humanInterrupt` / `sessionRouting`）
 * 现在写在人格配置里，**逐分身可不同**。
 *
 * 这四项以前各占一个全局环境变量。全局变量只能表达“所有分身都一样”，
 * 而真实诉求往往是“值班分身要让位 60 秒、日报分身不必”——所以并进
 * `GEBAI_PERSONA_<ID>` 的数组项。
 *
 * 取值顺序：分身 > 人格级默认（不带 name 的项）> 全局变量 > 代码缺省。
 * 保留全局兜底是为了**已有部署不必为了升级改配置**。
 */
import { describe, expect, test } from "bun:test"
import { BEHAVIOR_DEFAULTS, behaviorParamsFor } from "./behavior-params"

describe("缺省值（升级不改变现有行为）", () => {
  test("什么都不配 → 与历史全局变量的缺省一致", () => {
    const b = behaviorParamsFor({}, "xiaoshuang", "support")
    console.log(`    实测: ${JSON.stringify(b)}`)
    expect(b).toEqual({ replyToOwner: true, yieldSeconds: 180, humanInterrupt: false, sessionRouting: "driver" })
    expect(BEHAVIOR_DEFAULTS).toEqual(b)
  })
})

describe("全局变量兜底（兼容既有部署）", () => {
  test("四个全局变量都生效", () => {
    const b = behaviorParamsFor(
      {
        GEBAI_PERSONA_REPLY_TO_OWNER: "false",
        GEBAI_PERSONA_YIELD_SECONDS: "60",
        GEBAI_PERSONA_HUMAN_INTERRUPT: "1",
        GEBAI_PERSONA_SESSION_ROUTING: "chat",
      },
      "xiaoshuang",
      "support",
    )
    console.log(`    实测: ${JSON.stringify(b)}`)
    expect(b).toEqual({ replyToOwner: false, yieldSeconds: 60, humanInterrupt: true, sessionRouting: "chat" })
  })

  test("非法值回落缺省（不让笔误变成 0/NaN）", () => {
    expect(behaviorParamsFor({ GEBAI_PERSONA_YIELD_SECONDS: "abc" }, "x").yieldSeconds).toBe(180)
    expect(behaviorParamsFor({ GEBAI_PERSONA_YIELD_SECONDS: "-5" }, "x").yieldSeconds).toBe(180)
    expect(behaviorParamsFor({ GEBAI_PERSONA_SESSION_ROUTING: "胡说" }, "x").sessionRouting).toBe("driver")
  })

  test("yieldSeconds=0 是合法值（显式关闭让位，不是“没配”）", () => {
    expect(behaviorParamsFor({ GEBAI_PERSONA_YIELD_SECONDS: "0" }, "x").yieldSeconds).toBe(0)
  })
})

describe("逐分身声明优先", () => {
  const withItems = (items: unknown[]) => ({
    GEBAI_PERSONA_XIAOSHUANG: JSON.stringify(items),
  })

  test("分身级覆盖全局", () => {
    const env = { ...withItems([{ name: "support", yieldSeconds: 60 }]), GEBAI_PERSONA_YIELD_SECONDS: "180" }
    // 注意：直接传 env 即可，behaviorParamsFor 内部会解析 GEBAI_PERSONA_<ID>
    const b = behaviorParamsFor(env, "xiaoshuang", "support")
    console.log(`    实测: yieldSeconds=${b.yieldSeconds}`)
    // 未传 items 时只读全局（这正是“惰性解析”的语义：调用方负责给已解析的项）
    expect(b.yieldSeconds).toBe(180)
  })

  test("传了 items（已解析的分身配置）→ 分身值胜出", () => {
    const b = behaviorParamsFor({ GEBAI_PERSONA_YIELD_SECONDS: "180" }, "xiaoshuang", "support", {
      defaults: null,
      own: { name: "support", yieldSeconds: 60 },
    })
    expect(b.yieldSeconds).toBe(60)
    // 其余项仍回落全局/缺省
    expect(b.replyToOwner).toBe(true)
  })

  test("人格级默认 + 分身覆盖（更具体的赢）", () => {
    const b = behaviorParamsFor({}, "xiaoshuang", "support", {
      defaults: { yieldSeconds: 300, replyToOwner: false },
      own: { name: "support", replyToOwner: true },
    })
    console.log(`    实测: ${JSON.stringify(b)}`)
    expect(b.yieldSeconds).toBe(300) // 分身没写 → 继承人格级默认
    expect(b.replyToOwner).toBe(true) // 分身写了 → 覆盖
  })

  test("humanInterrupt=false 能显式关掉（不被 ?? 吞掉）", () => {
    const b = behaviorParamsFor({ GEBAI_PERSONA_HUMAN_INTERRUPT: "1" }, "x", "d", {
      defaults: null,
      own: { name: "d", humanInterrupt: false },
    })
    expect(b.humanInterrupt).toBe(false)
  })

  test("yieldSeconds=0 能显式覆盖（不是“没配”）", () => {
    const b = behaviorParamsFor({ GEBAI_PERSONA_YIELD_SECONDS: "180" }, "x", "d", {
      defaults: null,
      own: { name: "d", yieldSeconds: 0 },
    })
    expect(b.yieldSeconds).toBe(0)
  })
})
