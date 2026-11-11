/**
 * `GEBAI_PERSONA_<ID>` 声明式配置：解析、合并、通道引用。
 *
 * 咬住三件事（都是旧形态的真实痛点）：
 * 1. **下划线歧义消失**：`{name:"alerts", ...}` 明确写在一个项里，不再靠猜
 *    `GEBAI_PERSONA_FIN_OPS_ALERT_CHATS` 是「人格 fin_ops / 分身 alert」还是「人格 fin_ops_alert」
 * 2. **一个分身一处配置**：不再散成 CHATS / OUTPUT / SESSION_MAP 三个变量
 * 3. **通道引用**：`channels:["feishu_ops"]` 能解析成该实例的群，引用不存在的实例必须报错
 */
import { describe, expect, test } from "bun:test"
import { mergeDriverEnv, outputsFromChannels, parsePersonaEnv } from "./persona-config"

const P = (raw: unknown) => parsePersonaEnv("xiaoshuang", JSON.stringify(raw))

describe("解析：每项一个分身", () => {
  test("分身项按 name 收进 Map（下划线歧义从此消失）", () => {
    const r = P([
      { name: "support", chats: ["oc_a"] },
      { name: "alerts", chats: ["oc_b"], maxConcurrent: 2 },
    ])
    console.log(`    实测: drivers=${[...r.drivers.keys()].join(",")}`)
    expect(r.errors).toEqual([])
    expect([...r.drivers.keys()]).toEqual(["support", "alerts"])
    expect(r.drivers.get("alerts")!.maxConcurrent).toBe(2)
    expect(r.personaDefaults).toBeNull()
  })

  test("不带 name 的项 = 人格级默认（最多一个）", () => {
    const r = P([{ chats: ["oc_a"], sessionMap: "per_thread" }, { name: "support", chats: ["oc_b"] }])
    expect(r.errors).toEqual([])
    expect(r.personaDefaults).toEqual({ chats: ["oc_a"], sessionMap: "per_thread" })
  })

  test("两个默认项 → 报错（歧义必须拦住）", () => {
    const r = P([{ chats: ["oc_a"] }, { chats: ["oc_b"] }])
    expect(r.errors[0]).toContain("人格级默认项")
  })

  test("未配置 → 全部为空且无错误", () => {
    expect(parsePersonaEnv("x", undefined)).toEqual({ personaDefaults: null, drivers: new Map(), errors: [] })
  })

  test("非法 JSON → 指出变量名", () => {
    const r = parsePersonaEnv("xiaoshuang", "[oops")
    expect(r.errors[0]).toContain("GEBAI_PERSONA_XIAOSHUANG")
    expect(r.errors[0]).toContain("不是合法 JSON")
  })

  test("未知键 → 报错并给可用键（拼错=静默失效，最危险）", () => {
    const r = P([{ name: "support", chat: ["oc_a"] }])
    console.log(`    实测: ${r.errors[0]}`)
    expect(r.errors[0]).toContain("未知键「chat」")
    expect(r.errors[0]).toContain("chats")
  })

  test("name 非法 / 重复", () => {
    expect(P([{ name: "Bad Name" }]).errors[0]).toContain("非法")
    expect(P([{ name: "dup" }, { name: "dup" }]).errors[0]).toContain("重复")
  })

  test("maxConcurrent 非正数 → 报错", () => {
    expect(P([{ name: "s", maxConcurrent: 0 }]).errors[0]).toContain("maxConcurrent")
  })

  test("reviewOutbound：布尔值可解析，非布尔报错", () => {
    expect(P([{ name: "s", reviewOutbound: false }]).drivers.get("s")!.reviewOutbound).toBe(false)
    expect(P([{ name: "s", reviewOutbound: "false" }]).errors[0]).toContain("reviewOutbound 必须是布尔值")
  })

  test("reviewOutbound 写在人格级默认项里也能保留（false 不能被当成“没写”）", () => {
    const r = P([{ reviewOutbound: false }])
    console.log(`    实测: ${JSON.stringify(r.personaDefaults)}`)
    expect(r.personaDefaults!.reviewOutbound).toBe(false)
  })
})

describe("合并：人格级默认 → 分身级覆盖", () => {
  test("分身没配的项继承默认，配了的覆盖", () => {
    const merged = mergeDriverEnv(
      { chats: ["oc_default"], sessionMap: "per_chat", signature: "——小霜" },
      { name: "support", chats: ["oc_own"], outbound: "shape" },
    )
    console.log(`    实测: ${JSON.stringify(merged)}`)
    expect(merged.chats).toEqual(["oc_own"]) // 覆盖
    expect(merged.outbound).toBe("shape") // 新增
    expect(merged.sessionMap).toBe("per_chat") // 继承
    expect(merged.signature).toBe("——小霜") // 继承
  })

  test("没有任何默认也没有自身配置 → 只留 name", () => {
    expect(mergeDriverEnv(null, { name: "s" })).toEqual({ name: "s" })
  })

  test("reviewOutbound:false 能覆盖默认的 true（布尔值不能用真值判断）", () => {
    const merged = mergeDriverEnv({ reviewOutbound: true }, { name: "s", reviewOutbound: false })
    console.log(`    实测: ${JSON.stringify(merged)}`)
    expect(merged.reviewOutbound).toBe(false)
  })
})

describe("通道引用 → 输出目标", () => {
  const CHATS = new Map<string, readonly string[]>([
    ["feishu_main", ["oc_a", "oc_b"]],
    ["feishu_ops", ["oc_ops"]],
  ])

  test("只有 channels → 取该实例的全部群，首条为默认", () => {
    const r = outputsFromChannels({ name: "s", channels: ["feishu_main"] }, CHATS)
    console.log(`    实测: ${JSON.stringify(r.targets)}`)
    expect(r.errors).toEqual([])
    expect(r.targets.map((t) => `${t.channel}:${t.address}`)).toEqual(["feishu_main:oc_a", "feishu_main:oc_b"])
    expect(r.targets[0]!.default).toBe(true)
  })

  test("channels + chats → 只取交集（越界不发，不静默发到别的群）", () => {
    const r = outputsFromChannels({ name: "s", channels: ["feishu_main"], chats: ["oc_b"] }, CHATS)
    expect(r.targets.map((t) => t.address)).toEqual(["oc_b"])
  })

  test("chats 里有该实例不允许的群 → 报错（但仍保留合法的那部分）", () => {
    const r = outputsFromChannels({ name: "s", channels: ["feishu_main"], chats: ["oc_b", "oc_x"] }, CHATS)
    expect(r.errors[0]).toContain("不在通道实例「feishu_main」的白名单里")
    expect(r.targets.map((t) => t.address)).toEqual(["oc_b"])
  })

  test("引用不存在的实例 → 报错（不静默丢弃）", () => {
    const r = outputsFromChannels({ name: "s", channels: ["feishu_nope"] }, CHATS)
    expect(r.errors[0]).toContain("未配置的通道实例「feishu_nope」")
    expect(r.targets).toEqual([])
  })

  test("多实例 → 每个实例一条目标（同类多份的直接收益）", () => {
    const r = outputsFromChannels({ name: "s", channels: ["feishu_main", "feishu_ops"] }, CHATS)
    expect(r.targets.map((t) => t.channel)).toEqual(["feishu_main", "feishu_main", "feishu_ops"])
  })

  test("不写 channels → 不产生目标（出站规则不变：该分身只能回事件来源）", () => {
    expect(outputsFromChannels({ name: "s", chats: ["oc_a"] }, CHATS)).toEqual({ targets: [], errors: [] })
  })
})
