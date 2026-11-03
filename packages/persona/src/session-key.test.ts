import { describe, expect, test } from "bun:test"
import { validatePersonaDef } from "./persona"
import { PersonaRuntime } from "./runtime"
import { DEFAULT_SESSION_KEY, sanitizeSessionKey, sessionDriverKey, driverKey } from "./runtime"
import type { PersonaEvent } from "./event"
import type { PersonaLookup, PersonaRunner, SessionResolver, ResolvedSession } from "./ports"

/** 会话路由键（sessionKeyOf）：声明、校验与事件→会话的多路分流。 */

const VALID_BASE = {
  id: "p1",
  name: "测试人格",
  identity: "你是测试人格",
  role: "测试",
}

describe("sessionKeyOf：加载期校验", () => {
  test("合法：函数且返回字符串", () => {
    const bad = validatePersonaDef({ ...VALID_BASE, drivers: [{ id: "d", focus: "f", subscriptions: ["message"], sessionKeyOf: () => "g1" }] }, "p1")
    expect(bad).toBeNull()
  })

  test("拒绝：非函数", () => {
    const bad = validatePersonaDef({ ...VALID_BASE, drivers: [{ id: "d", focus: "f", subscriptions: ["message"], sessionKeyOf: "chatId" }] }, "p1")
    expect(bad).toContain("sessionKeyOf 必须是函数")
  })

  test("拒绝：试运行抛错", () => {
    const bad = validatePersonaDef({ ...VALID_BASE, drivers: [{ id: "d", focus: "f", subscriptions: ["message"], sessionKeyOf: () => { throw new Error("炸了") } }] }, "p1")
    expect(bad).toContain("试运行抛错")
  })

  test("拒绝：返回非字符串", () => {
    const bad = validatePersonaDef({ ...VALID_BASE, drivers: [{ id: "d", focus: "f", subscriptions: ["message"], sessionKeyOf: () => 42 }] }, "p1")
    expect(bad).toContain("必须返回字符串")
  })

  test("不声明：完全合法（单会话缺省形态）", () => {
    expect(validatePersonaDef({ ...VALID_BASE, drivers: [{ id: "d", focus: "f", subscriptions: ["message"] }] }, "p1")).toBeNull()
  })
})

describe("sessionKey：键工具", () => {
  test("默认键退回二元形状（与历史缓存键兼容）", () => {
    expect(sessionDriverKey("p", "d", DEFAULT_SESSION_KEY)).toBe(driverKey("p", "d"))
    expect(sessionDriverKey("p", "d", "g1")).toBe("p/d#g1")
  })

  test("sanitize：非法字符压下划线、限长 128、空串回落默认键", () => {
    expect(sanitizeSessionKey("oc_abc/def 中文")).toBe("oc_abc_def___")
    expect(sanitizeSessionKey("x".repeat(300)).length).toBe(128)
    expect(sanitizeSessionKey("   ")).toBe(DEFAULT_SESSION_KEY)
  })
})

// ── 端到端：同一 driver 按事件分流到不同会话 ──

function ev(id: string, chatId?: string): PersonaEvent {
  return {
    id,
    kind: "message",
    personaId: "",
    channel: "feishu",
    context: { source: "s", summary: "x", ...(chatId ? { refs: { chatId } } : {}) },
    body: "b",
    receivedAt: Date.now(),
  }
}

describe("PersonaRuntime：sessionKeyOf 分流", () => {
  function setup(sessionKeyOf?: (e: PersonaEvent) => string) {
    const resolvedKeys: Array<string | undefined> = []
    const sessionIds = new Map<string, string>()
    let seq = 0
    const sessions: SessionResolver = {
      async resolveDriver(input): Promise<ResolvedSession> {
        resolvedKeys.push(input.sessionKey)
        const k = input.sessionKey ?? ""
        if (!sessionIds.has(k)) sessionIds.set(k, `sess-${k || "default"}-${++seq}`)
        return { sessionId: sessionIds.get(k)!, created: false, name: `n-${k}` }
      },
    }
    const lookup: PersonaLookup = {
      get: () => ({ id: "p1", name: "测试", identity: "你是测试", role: "r" }),
      drivers: () => [{ id: "d", focus: "f", subscriptions: ["message"], ...(sessionKeyOf ? { sessionKeyOf } : {}) }],
      maxConcurrent: () => 1,
      subscribers: (e: PersonaEvent) => (e.kind === "message" ? [{ personaId: "p1", driverIds: ["d"], user: "admin" }] : []),
    } as never
    const runner: PersonaRunner = { isRunning: () => false, run: async () => {} }
    const rt = new PersonaRuntime({ lookup, runner, sessions })
    rt.start() // dispatch 前必须启动（未启动直接 unavailable，不进投递链）
    return { rt, resolvedKeys, sessionIds }
  }

  test("声明 chatId 路由：不同群的事件解析到不同会话、同群复用", async () => {
    const { rt, resolvedKeys } = setup((e) => e.context.refs?.chatId ?? "")
    await rt.dispatch(ev("m1", "oc_a"))
    await rt.dispatch(ev("m2", "oc_a"))
    await rt.dispatch(ev("m3", "oc_b"))
    // 缓存生效：同键只解析一次（resolvedKeys 记录的是 resolver 被调的次数，不是事件数）
    expect(resolvedKeys).toEqual(["oc_a", "oc_b"])
  })

  test("路由函数返回空 → 默认键（单会话兼容路径）", async () => {
    const { rt, resolvedKeys } = setup(() => "")
    await rt.dispatch(ev("m1", "oc_a"))
    await rt.dispatch(ev("m2", "oc_b"))
    expect(resolvedKeys.every((k) => k === undefined || k === "")).toBe(true)
  })

  test("路由函数抛错 → 降级默认键、事件不被丢弃（accepted）", async () => {
    let called = 0
    const { rt } = setup(() => {
      called++
      throw new Error("路由炸了")
    })
    const r = await rt.dispatch(ev("m1", "oc_a"))
    expect(called).toBeGreaterThanOrEqual(1)
    expect(r.status).toBe("accepted")
  })

  test("不声明 sessionKeyOf：行为与历史一致（键恒为默认）", async () => {
    const { rt, resolvedKeys } = setup(undefined)
    await rt.dispatch(ev("m1", "oc_a"))
    await rt.dispatch(ev("m2", "oc_b"))
    // 默认键下两事件共用同一缓存条目：resolver 只被调一次，键为空串（默认，
    // runtime 传的是 DEFAULT_SESSION_KEY=""；旧绑定键形状兼容）
    expect(resolvedKeys).toEqual([""])
  })
})
