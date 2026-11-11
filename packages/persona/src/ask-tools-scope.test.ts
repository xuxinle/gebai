/**
 * 审批名单的**逐分身**解析：`GEBAI_PERSONA_<ID>` 数组项里的 `askTools`。
 *
 * 为什么需要逐分身：不同分身干的事不一样——盯告警的只需要读日志（全不必问），
 * 执行运维动作的应该把 `sh` 拦住。全局名单只能取最保守的交集，结果是所有人一起被卡。
 *
 * 优先级：分身 `askTools` > 人格级默认（不带 name 的项）> 全局变量 > []
 *
 * 这里最要紧的是**「显式空」与「没写」必须区分**：
 * - 没写 → 回落全局（我配了全局拦 sh，这个分身就别例外）
 * - 写 `""` → 这个分身什么都不问（覆盖全局）
 * 混了两者的话，就没法表达“全局拦、但盯告警那个不必拦”。
 */
import { describe, expect, test } from "bun:test"
import { askToolsFor, readAskToolsFromPersonaJson } from "./approval-tools"

const P = (items: unknown[]) => ({ GEBAI_PERSONA_XIAOSHUANG: JSON.stringify(items) })

describe("从人格 JSON 抽 askTools", () => {
  test("分身级声明 → 命中该分身", () => {
    const raw = JSON.stringify([
      { name: "support", askTools: "sh,py" },
      { name: "alerts", askTools: ["file.delete"] },
    ])
    expect(readAskToolsFromPersonaJson(raw, "support")).toEqual(["sh", "py"])
    expect(readAskToolsFromPersonaJson(raw, "alerts")).toEqual(["file.delete"])
  })

  test("字符串与数组两种写法都收", () => {
    const raw = JSON.stringify([{ name: "support", askTools: ["sh", "py"] }])
    expect(readAskToolsFromPersonaJson(raw, "support")).toEqual(["sh", "py"])
  })

  test("该分身没写 → 回落到人格级默认（不带 name 的项）", () => {
    const raw = JSON.stringify([{ askTools: "sh" }, { name: "support" }])
    expect(readAskToolsFromPersonaJson(raw, "support")).toEqual(["sh"])
  })

  test("都没写 → undefined（表示“没声明”，交给全局兜底）", () => {
    const raw = JSON.stringify([{ name: "support" }])
    expect(readAskToolsFromPersonaJson(raw, "support")).toBeUndefined()
  })

  test("**显式空** → 空数组（不是 undefined：要覆盖全局）", () => {
    const raw = JSON.stringify([{ name: "support", askTools: "" }])
    expect(readAskToolsFromPersonaJson(raw, "support")).toEqual([])
    const raw2 = JSON.stringify([{ name: "support", askTools: [] }])
    expect(readAskToolsFromPersonaJson(raw2, "support")).toEqual([])
  })

  test("分身级优先于人格级默认", () => {
    const raw = JSON.stringify([
      { askTools: "sh" }, // 人格级默认
      { name: "support", askTools: "py" }, // 这个分身自己说了算
    ])
    expect(readAskToolsFromPersonaJson(raw, "support")).toEqual(["py"])
  })

  test("坏 JSON / 非数组 → undefined（不把审批路径搞崩）", () => {
    expect(readAskToolsFromPersonaJson("{not json", "support")).toBeUndefined()
    expect(readAskToolsFromPersonaJson('{"name":"support"}', "support")).toBeUndefined()
  })
})

describe("askToolsFor：三级优先级", () => {
  test("分身声明 > 全局", () => {
    const env = { ...P([{ name: "support", askTools: "py" }]), GEBAI_PERSONA_APPROVAL_ASK_TOOLS: "sh" }
    expect(askToolsFor(env, "xiaoshuang", "support")).toEqual(["py"])
  })

  test("分身没写 → 全局兜底", () => {
    const env = { ...P([{ name: "support" }]), GEBAI_PERSONA_APPROVAL_ASK_TOOLS: "sh" }
    expect(askToolsFor(env, "xiaoshuang", "support")).toEqual(["sh"])
  })

  test("分身写空 → 覆盖全局（这个分身什么都不问）", () => {
    const env = { ...P([{ name: "support", askTools: "" }]), GEBAI_PERSONA_APPROVAL_ASK_TOOLS: "sh" }
    expect(askToolsFor(env, "xiaoshuang", "support")).toEqual([])
  })

  test("人格未配置 + 无全局 → 空（缺省：全部不审批）", () => {
    expect(askToolsFor({}, "xiaoshuang", "support")).toEqual([])
  })

  test("不传 driverId → 用该人格的默认项", () => {
    const env = P([{ askTools: "sh" }, { name: "support", askTools: "py" }])
    expect(askToolsFor(env, "xiaoshuang")).toEqual(["sh"])
  })

  test("人格 id 大小写不敏感（变量名按大写找）", () => {
    const env = { GEBAI_PERSONA_XIAOSHUANG: JSON.stringify([{ name: "support", askTools: "sh" }]) }
    expect(askToolsFor(env, "XiaoShuang", "support")).toEqual(["sh"])
  })
})
