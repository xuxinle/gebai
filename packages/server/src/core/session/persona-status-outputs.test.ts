/**
 * 面板可见性：分身改环境变量后的**结果**必须能在状态快照里看见。
 *
 * 为什么这几条值得单测：`GEBAI_PERSONA_<ID>` 配错（实例名拼错、群不在白名单）
 * 的失败形态是**静默**的——模型不去发那个群，就永远没有报错。
 * 面板上「这个分身到底会发到哪」是唯一能提前发现它的地方。
 */
import { describe, expect, test } from "bun:test"
import { collectPersonaState } from "./persona-status"

/** 最小替身：只要 collectPersonaState 用到的接口。 */
function makeDeps(defOutputs: unknown, sessionMapRules: number | undefined) {
  const driver = {
    id: "support",
    focus: "答疑",
    subscriptions: ["message"],
    ...(defOutputs ? { outputs: defOutputs } : {}),
    ...(sessionMapRules !== undefined
      ? { sessionMap: Array.from({ length: sessionMapRules }, (_, i) => ({ when: "message", key: `k${i}` })) }
      : {}),
  }
  return {
    catalog: {
      all: () => [{ id: "xiaoshuang", name: "小霜", role: "运维", drivers: [driver] }],
      drivers: () => [driver],
      get: () => ({ id: "xiaoshuang", name: "小霜", role: "运维" }),
    },
    runtime: {
      personaStates: () => [],
      bindings: () => [],
    },
    drivers: { get: () => undefined },
    sessionRunning: () => false,
  } as never
}

describe("快照：分身的出站目标与会话映射规则可见", () => {
  test("有 outputs → 原样带出（含实例名；这是排「发错群」的第一手依据）", async () => {
    const snap = await collectPersonaState(
      makeDeps([{ channel: "feishu_main", address: "oc_a", label: "feishu_main", default: true }], 2),
    )
    const d = snap.personas[0]!.drivers[0]!
    console.log(`    实测: ${JSON.stringify(d.outputs)} / sessionMapRules=${d.sessionMapRules}`)
    expect(d.outputs).toEqual([{ channel: "feishu_main", address: "oc_a", label: "feishu_main", default: true }])
    expect(d.sessionMapRules).toBe(2)
  })

  test("没有 outputs → 不编造空数组（未配置与配置了空是两件事）", async () => {
    const snap = await collectPersonaState(makeDeps(null, undefined))
    const d = snap.personas[0]!.drivers[0]!
    expect(d.outputs).toBeUndefined()
    expect(d.sessionMapRules).toBeUndefined()
  })
})
