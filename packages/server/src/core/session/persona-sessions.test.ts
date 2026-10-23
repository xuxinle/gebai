/**
 * 人格会话映射：`(personaId, driverId)` → 稳定常驻会话。
 *
 * 这些用例对应「稳定又可灵活扩展」的三条硬要求：
 * 幂等（同 key 永远同会话）、可恢复（重启后仍找回）、不悬空（会话被删则重建）。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { SessionStore } from "./store"
import { PersonaSessionResolver } from "./persona-sessions"

async function makeStore(): Promise<{ home: string; store: SessionStore }> {
  const home = mkdtempSync(join(tmpdir(), "ps-"))
  return { home, store: new SessionStore({ home }) }
}

describe("PersonaSessionResolver：稳定映射", () => {
  test("首次解析 → 新建并绑定（名字含人格·分身）", async () => {
    const { home, store } = await makeStore()
    const r = new PersonaSessionResolver({ store })
    r.nameOf = () => "小霜"
    const got = await r.resolveDriver({ personaId: "xiaoshuang", driverId: "support", user: "admin", event: {} })
    console.log(`    实测: ${JSON.stringify(got)}`)
    expect(got.created).toBe(true)
    expect(got.name).toBe("小霜·support")

    const saved = await store.load(got.sessionId, "admin")
    expect(saved?.persona?.personaId).toBe("xiaoshuang")
    expect(saved?.persona?.driverId).toBe("support")
    expect(saved?.persona?.boundAt).toBeGreaterThan(0)
    rmSync(home, { recursive: true, force: true })
  })

  test("幂等：同 key 多次调用返回同一 sessionId", async () => {
    const { home, store } = await makeStore()
    const r = new PersonaSessionResolver({ store })
    const a = await r.resolveDriver({ personaId: "p", driverId: "d", user: "admin", event: {} })
    const b = await r.resolveDriver({ personaId: "p", driverId: "d", user: "admin", event: {} })
    expect(b.sessionId).toBe(a.sessionId)
    expect(b.created).toBe(false)
    rmSync(home, { recursive: true, force: true })
  })

  test("并发调用只建一个会话（同 key 并发合并）", async () => {
    const { home, store } = await makeStore()
    const r = new PersonaSessionResolver({ store })
    const results = await Promise.all(
      Array.from({ length: 5 }, () => r.resolveDriver({ personaId: "p", driverId: "d", user: "admin", event: {} })),
    )
    const ids = new Set(results.map((x) => x.sessionId))
    console.log(`    实测: 5 次并发 → ${ids.size} 个会话 id`)
    expect(ids.size).toBe(1)
    const all = await store.listSessions("admin")
    expect(all.length).toBe(1)
    rmSync(home, { recursive: true, force: true })
  })

  test("不同 driver 各自独立会话", async () => {
    const { home, store } = await makeStore()
    const r = new PersonaSessionResolver({ store })
    const a = await r.resolveDriver({ personaId: "p", driverId: "support", user: "admin", event: {} })
    const b = await r.resolveDriver({ personaId: "p", driverId: "alerts", user: "admin", event: {} })
    expect(a.sessionId).not.toBe(b.sessionId)
    rmSync(home, { recursive: true, force: true })
  })

  test("重启恢复：新实例扫盘找回同一会话（不新建）", async () => {
    const { home, store } = await makeStore()
    const r1 = new PersonaSessionResolver({ store })
    const first = await r1.resolveDriver({ personaId: "p", driverId: "d", user: "admin", event: {} })

    // 模拟重启：全新实例（memo 为空）
    const r2 = new PersonaSessionResolver({ store: new SessionStore({ home }) })
    const again = await r2.resolveDriver({ personaId: "p", driverId: "d", user: "admin", event: {} })
    console.log(`    实测: 重启前 ${first.sessionId} → 重启后 ${again.sessionId}`)
    expect(again.sessionId).toBe(first.sessionId)
    expect(again.created).toBe(false)
    rmSync(home, { recursive: true, force: true })
  })

  test("warm() 预热后第一轮不必扫盘", async () => {
    const { home, store } = await makeStore()
    const r1 = new PersonaSessionResolver({ store })
    const first = await r1.resolveDriver({ personaId: "p", driverId: "d", user: "admin", event: {} })
    const n = await r1.warm("admin")
    console.log(`    实测: warm 恢复 ${n} 条绑定`)
    expect(n).toBe(1)
    expect(r1.bindings()[0]?.sessionId).toBe(first.sessionId)
    rmSync(home, { recursive: true, force: true })
  })

  test("绑定会话被删 → 重建（不返回死 id）", async () => {
    const { home, store } = await makeStore()
    const r = new PersonaSessionResolver({ store })
    const first = await r.resolveDriver({ personaId: "p", driverId: "d", user: "admin", event: {} })
    // 删掉该会话（模拟用户在界面上删除）
    await store.delete(first.sessionId, "admin")
    const again = await r.resolveDriver({ personaId: "p", driverId: "d", user: "admin", event: {} })
    console.log(`    实测: 删除后 ${first.sessionId} → 重建 ${again.sessionId}（created=${again.created}）`)
    expect(again.sessionId).not.toBe(first.sessionId)
    expect(again.created).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })

  test("unbind：解绑不删会话（历史仍可查）", async () => {
    const { home, store } = await makeStore()
    const r = new PersonaSessionResolver({ store })
    const got = await r.resolveDriver({ personaId: "p", driverId: "d", user: "admin", event: {} })
    expect(await r.unbind("p", "d", "admin")).toBe(true)
    const after = await store.load(got.sessionId, "admin")
    console.log(`    实测: 会话仍在=${!!after} 绑定已清=${after?.persona === undefined}`)
    expect(after).not.toBeNull() // 会话还在
    expect(after?.persona).toBeUndefined() // 绑定清了
    rmSync(home, { recursive: true, force: true })
  })

  test("多个会话绑同一 key → 取最早绑定的（行为稳定）", async () => {
    const { home, store } = await makeStore()
    const older = await store.createSession("admin", "旧")
    older.persona = { personaId: "p", driverId: "d", boundAt: 1000 }
    await store.save(older, { touch: false })
    const newer = await store.createSession("admin", "新")
    newer.persona = { personaId: "p", driverId: "d", boundAt: 2000 }
    await store.save(newer, { touch: false })

    const r = new PersonaSessionResolver({ store })
    const got = await r.resolveDriver({ personaId: "p", driverId: "d", user: "admin", event: {} })
    console.log(`    实测: 取到 ${got.sessionId}（旧=${older.id}）`)
    expect(got.sessionId).toBe(older.id) // 最早绑定的胜出
    rmSync(home, { recursive: true, force: true })
  })

  test("普通会话（无绑定字段）不被误当成人格会话", async () => {
    const { home, store } = await makeStore()
    const normal = await store.createSession("admin", "普通对话")
    const r = new PersonaSessionResolver({ store })
    // 无任何人格绑定时，warm 不报数
    expect(await r.warm("admin")).toBe(0)
    const got = await r.resolveDriver({ personaId: "p", driverId: "d", user: "admin", event: {} })
    expect(got.sessionId).not.toBe(normal.id) // 不会误用普通会话
    // 现在盘上有 1 条人格绑定（warm 口径：盘上可恢复的绑定总数）
    expect(await r.warm("admin")).toBe(1)
    rmSync(home, { recursive: true, force: true })
  })
})
