/**
 * 人物目录与定义发现：`PersonaDef` + `PersonaCatalog`。
 *
 * 重点验「静态定义权威、动态 driver 不越权」——这是两级模型的信任边界：
 * 模型能自己造分身（提效），但不能靠造同名分身来改写人手写的部署配置。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { PersonaCatalog } from "./catalog"
import { discoverPersonas, validatePersonaDef } from "./persona"
import type { PersonaDef } from "./persona"
import { DriverRegistry } from "./driver"

const DEF = (over: Partial<PersonaDef> = {}): PersonaDef => ({
  id: "xiaoshuang",
  name: "小霜",
  identity: "你是小霜，训练平台的值班工程师。",
  role: "运维",
  drivers: [
    { id: "support", focus: "答支持群", subscriptions: ["message"] },
    { id: "alerts", focus: "盯告警", subscriptions: ["webhook:alert"] },
  ],
  ...over,
})

describe("validatePersonaDef：定义校验（错就报错，不静默降级）", () => {
  test("最小合法定义", () => {
    expect(validatePersonaDef(DEF(), "xiaoshuang")).toBeNull()
  })

  test("id 与文件名不一致 → 拒绝（防改名后订阅/会话映射失配）", () => {
    expect(validatePersonaDef(DEF(), "other")).toContain("不一致")
  })

  test("缺 identity → 拒绝（身份是提示词主体）", () => {
    const bad = { ...DEF(), identity: "" }
    expect(validatePersonaDef(bad, "xiaoshuang")).toContain("identity")
  })

  test("未知字段 → 拒绝（拼错的字段会让配置静默失效）", () => {
    const bad = { ...DEF(), driver: [] } // 应为 drivers
    expect(validatePersonaDef(bad, "xiaoshuang")).toContain("未知字段")
  })

  test("driver 缺 focus → 拒绝（这个分身在关注什么是必填）", () => {
    const bad = DEF({ drivers: [{ id: "x", focus: "", subscriptions: ["*"] }] })
    expect(validatePersonaDef(bad, "xiaoshuang")).toContain("focus")
  })

  test("driver id 重复 → 拒绝", () => {
    const bad = DEF({
      drivers: [
        { id: "dup", focus: "a", subscriptions: ["*"] },
        { id: "dup", focus: "b", subscriptions: ["*"] },
      ],
    })
    expect(validatePersonaDef(bad, "xiaoshuang")).toContain("重复")
  })

  test("driver 未知字段 → 拒绝（subcription 拼错会被抓住）", () => {
    const bad = DEF({ drivers: [{ id: "x", focus: "f", subscriptions: ["*"], subcription: [] } as never] })
    expect(validatePersonaDef(bad, "xiaoshuang")).toContain("未知字段")
  })
})

describe("discoverPersonas：丢文件即注册", () => {
  test("扫出定义文件并正确加载", async () => {
    const home = mkdtempSync(join(tmpdir(), "pd-"))
    const builtin = join(home, "builtin")
    const custom = join(home, "custom")
    mkdirSync(builtin, { recursive: true })
    writeFileSync(
      join(builtin, "xiaoshuang.ts"),
      `export const persona = ${JSON.stringify(DEF())}`,
    )
    const r = await discoverPersonas({ builtin, custom })
    expect(r.personas.size).toBe(1)
    expect(r.personas.get("xiaoshuang")?.name).toBe("小霜")
    expect(r.errors.size).toBe(0)
    rmSync(home, { recursive: true, force: true })
  })

  test("二开同名覆盖内置", async () => {
    const home = mkdtempSync(join(tmpdir(), "pd2-"))
    const builtin = join(home, "builtin")
    const custom = join(home, "custom")
    mkdirSync(builtin, { recursive: true })
    mkdirSync(custom, { recursive: true })
    writeFileSync(join(builtin, "xiaoshuang.ts"), `export const persona = ${JSON.stringify(DEF())}`)
    writeFileSync(join(custom, "xiaoshuang.ts"), `export const persona = ${JSON.stringify(DEF({ name: "小霜(二开)" }))}`)
    const r = await discoverPersonas({ builtin, custom })
    expect(r.personas.get("xiaoshuang")?.name).toBe("小霜(二开)")
    rmSync(home, { recursive: true, force: true })
  })

  test("单个坏定义只记 errors，不连带其它（失败隔离）", async () => {
    const home = mkdtempSync(join(tmpdir(), "pd3-"))
    const builtin = join(home, "builtin")
    mkdirSync(builtin, { recursive: true })
    writeFileSync(join(builtin, "good.ts"), `export const persona = ${JSON.stringify({ ...DEF(), id: "good" })}`)
    writeFileSync(join(builtin, "bad.ts"), `throw new Error("坏了")`)
    const r = await discoverPersonas({ builtin, custom: join(home, "none") })
    expect(r.personas.has("good")).toBe(true)
    expect(r.errors.get("bad")?.error).toContain("坏了")
    rmSync(home, { recursive: true, force: true })
  })

  test("目录不存在不报错（未二开是正常形态）", async () => {
    const r = await discoverPersonas({ builtin: join(tmpdir(), "__nope1__"), custom: join(tmpdir(), "__nope2__") })
    expect(r.personas.size).toBe(0)
    expect(r.errors.size).toBe(0)
  })
})

describe("PersonaCatalog：静态权威 + 动态合流", () => {
  function makeCatalog(personas: PersonaDef[], registry?: DriverRegistry): PersonaCatalog {
    return new PersonaCatalog({ personas: new Map(personas.map((p) => [p.id, p])), ...(registry ? { drivers: registry } : {}) })
  }

  test("get 只返回身份四件套（不泄露内部字段）", () => {
    const c = makeCatalog([DEF()])
    const id = c.get("xiaoshuang")!
    expect(Object.keys(id).sort()).toEqual(["id", "identity", "name", "role"])
    expect(c.get("nope")).toBeNull()
  })

  test("静态 driver 全可见", () => {
    const c = makeCatalog([DEF()])
    expect(c.drivers("xiaoshuang").map((d) => d.id)).toEqual(["support", "alerts"])
  })

  test("动态 driver 追加在后", () => {
    const reg = new DriverRegistry({ statics: [] })
    const c = makeCatalog([DEF()], reg)
    void reg.create({ personaId: "xiaoshuang", id: "weekly", focus: "写周报", subscriptions: ["schedule"], reason: "经理要求" })
    expect(c.drivers("xiaoshuang").map((d) => d.id)).toEqual(["support", "alerts", "weekly"])
  })

  test("动态 driver 不能覆盖同名静态（防模型改写部署配置）", () => {
    const reg = new DriverRegistry({ statics: [] })
    const c = makeCatalog([DEF()], reg)
    // 模型试图用同名 "support" 把关注面改成 "*"
    void reg.create({ personaId: "xiaoshuang", id: "support", focus: "模型想接管一切", subscriptions: ["*"], reason: "我想更快响应" })
    const d = c.drivers("xiaoshuang").find((x) => x.id === "support")!
    console.log(`    实测: support.focus=${JSON.stringify(d.focus)} subs=${JSON.stringify(d.subscriptions)}`)
    expect(d.focus).toBe("答支持群") // 静态胜出
    expect(d.subscriptions).toEqual(["message"])
  })

  test("两级订阅：只唤醒关注该事件的 driver", () => {
    const c = makeCatalog([DEF()])
    const subs = c.subscribers({ kind: "message", channel: "feishu", context: { source: "s", summary: "x" } })
    console.log(`    实测 message: ${JSON.stringify(subs)}`)
    expect(subs).toEqual([{ personaId: "xiaoshuang", user: "admin", driverIds: ["support"] }])

    const alert = c.subscribers({ kind: "webhook", channel: "alert", context: { source: "s", summary: "x" } })
    expect(alert[0]?.driverIds).toEqual(["alerts"])
  })

  test("无人关注 → 空数组（上游据此返回 unrouted）", () => {
    const c = makeCatalog([DEF()])
    expect(c.subscribers({ kind: "idle", channel: "system", context: { source: "s", summary: "x" } })).toEqual([])
  })

  test("定向 personaId → 只给它", () => {
    const other: PersonaDef = { ...DEF(), id: "laozhang", name: "老张" }
    const c = makeCatalog([DEF(), { ...other, drivers: [{ id: "all", focus: "全干", subscriptions: ["message"] }] }])
    const subs = c.subscribers({ kind: "message", channel: "feishu", context: { source: "s", summary: "x" }, personaId: "laozhang" } as never)
    expect(subs.map((s) => s.personaId)).toEqual(["laozhang"])
  })

  test("定向 driverId → 只给它（worker 回报回派它的分身）", () => {
    const c = makeCatalog([DEF({ drivers: [{ id: "a", focus: "x", subscriptions: ["message"] }, { id: "b", focus: "y", subscriptions: ["message"] }] })])
    const subs = c.subscribers({ kind: "message", channel: "feishu", context: { source: "s", summary: "x" }, driverId: "b" } as never)
    expect(subs[0]?.driverIds).toEqual(["b"])
  })

  test("一个事件命中同角色多个 driver（各自独立处理，不是二选一）", () => {
    const c = makeCatalog([DEF({ drivers: [
      { id: "a", focus: "x", subscriptions: ["message"] },
      { id: "b", focus: "y", subscriptions: ["*"] },
    ] })])
    const subs = c.subscribers({ kind: "message", channel: "feishu", context: { source: "s", summary: "x" } })
    expect(subs[0]?.driverIds.sort()).toEqual(["a", "b"])
  })
})
