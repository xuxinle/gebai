/**
 * 人格定义校验的订阅语法专项（P0 修复钉子）。
 *
 * 为何单独成文件：`validatePersonaDef` 此前对 `drivers[].subscriptions` 只查
 * 「非空字符串数组」，语法（封闭集合）从未验证——`feishu*` 这类写法加载期全过，
 * 运行期永不命中、分身永不响应且零告警。custom/persona/README 承诺「写错会在
 * 加载期报错而不是静默失效」，这里把承诺钉进测试。
 */
import { describe, expect, test } from "bun:test"
import { validatePersonaDef } from "./persona"
import type { PersonaDef } from "./persona"

const DEF = (over: Partial<PersonaDef> = {}): PersonaDef => ({
  id: "xiaoshuang",
  name: "小霜",
  identity: "你是小霜",
  role: "应答",
  drivers: [{ id: "support", focus: "答支持群", subscriptions: ["message"] }],
  ...over,
})

describe("validatePersonaDef：订阅语法（封闭集合，加载期报错）", () => {
  test("全部合法写法通过", () => {
    expect(
      validatePersonaDef(
        DEF({
          drivers: [
            { id: "all", focus: "f", subscriptions: ["*"] },
            { id: "bykind", focus: "f", subscriptions: ["message", "schedule"] },
            { id: "bychannel", focus: "f", subscriptions: ["feishu"] },
            { id: "both", focus: "f", subscriptions: ["feishu:message", "message:feishu"] },
            { id: "three", focus: "f", subscriptions: ["feishu:oc_support:message"] },
          ],
        }),
        "xiaoshuang",
      ),
    ).toBeNull()
  })

  test("通配符前缀（feishu*）被拒——支持正则会让订阅静默失效或全量命中", () => {
    const bad = DEF({ drivers: [{ id: "x", focus: "f", subscriptions: ["feishu*"] }] })
    const err = validatePersonaDef(bad, "xiaoshuang")
    expect(err).toContain("x")
    expect(err).toContain("非法")
    expect(err).toContain("合法写法")
  })

  test("四段以上被拒（最多 channel:chatId:kind 三段）", () => {
    const bad = DEF({ drivers: [{ id: "y", focus: "f", subscriptions: ["a:b:c:d"] }] })
    expect(validatePersonaDef(bad, "xiaoshuang")).toContain("段数")
  })

  test("空串/空白订阅被拒", () => {
    const bad = DEF({ drivers: [{ id: "z", focus: "f", subscriptions: ["  "] as unknown as string[] }] })
    expect(validatePersonaDef(bad, "xiaoshuang")).toContain("subscriptions")
  })

  test("错误信息带人格 id 上下文与 driver id（排查时能直接定位）", () => {
    const bad = DEF({ drivers: [{ id: "support", focus: "f", subscriptions: ["webhook!"] }] })
    expect(validatePersonaDef(bad, "xiaoshuang")).toContain("support")
  })
})

describe("validatePersonaDef：maxConcurrent 整数校验（与动态 driver 同口径）", () => {
  test("非整数（1.5）被拒——否则 PersonaScheduler 构造抛错连带整次投递失败", () => {
    const bad = DEF({ drivers: [{ id: "x", focus: "f", subscriptions: ["*"], maxConcurrent: 1.5 }] })
    expect(validatePersonaDef(bad, "xiaoshuang")).toContain("整数")
  })

  test("整数与缺省仍合法", () => {
    expect(validatePersonaDef(DEF({ drivers: [{ id: "x", focus: "f", subscriptions: ["*"], maxConcurrent: 4 }] }), "xiaoshuang")).toBeNull()
    expect(validatePersonaDef(DEF(), "xiaoshuang")).toBeNull()
  })
})
