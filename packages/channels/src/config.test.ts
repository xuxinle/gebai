/**
 * `GEBAI_CHANNEL_<TYPE>` 声明式配置：解析、校验、实例展开。
 *
 * 这套用例咬住的是**这次改造真正要解决的三件事**，而不是“JSON 能 parse”：
 * 1. 同类可以配两份（旧形态因变量名把类型焊死，做不到）
 * 2. 键拼错要**报到实例级**（拼错若不报错 = 静默拿默认值跑，最难查）
 * 3. 未配置的语义要分明（必需配置的类型：没有实例；可选配置：一个缺省实例）
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { materializeInstance, parseChannelInstances } from "./config"
import type { ConfigKeySpec } from "./channel"
import { ChannelRegistry } from "./registry"
import { channel as feishu } from "./channels/feishu"
import { channel as local } from "./channels/local"

const SCHEMA: Record<string, ConfigKeySpec> = {
  appId: { type: "string", description: "应用 ID", required: true },
  appSecret: { type: "string", description: "密钥", required: true, secret: true },
  chats: { type: "string[]", description: "群白名单" },
  ignoreSelf: { type: "boolean", description: "忽略自己" },
}

const parse = (raw: string | undefined, schema = SCHEMA) => parseChannelInstances("feishu", raw, schema)

describe("解析：一份配置一个实例", () => {
  test("同类两份 → 两个具名实例（旧形态做不到这件事）", () => {
    const r = parse(
      JSON.stringify([
        { name: "feishu_main", appId: "cli_a", appSecret: "sa", chats: ["oc_1"] },
        { name: "feishu_ops", appId: "cli_b", appSecret: "sb", chats: ["oc_2", "oc_3"] },
      ]),
    )
    console.log(`    实测: 实例=${r.instances.map((i) => i.name).join(",")}`)
    expect(r.errors).toEqual([])
    expect(r.instances.map((i) => i.name)).toEqual(["feishu_main", "feishu_ops"])
    // 关键：两份配置各自独立，不共享凭据
    expect(r.instances[0]!.settings.appId).toBe("cli_a")
    expect(r.instances[1]!.settings.appId).toBe("cli_b")
    expect(r.instances[1]!.settings.chats).toEqual(["oc_2", "oc_3"])
  })

  test("未配置 → 空实例 + 无错误（由调用方决定报错还是用缺省）", () => {
    expect(parse(undefined)).toEqual({ instances: [], errors: [] })
    expect(parse("   ")).toEqual({ instances: [], errors: [] })
  })

  test("非法 JSON → 指出变量名，不静默当未配置", () => {
    const r = parse("{not json")
    expect(r.instances).toEqual([])
    // 变量名带方向段（IN/OUT）——报错必须指向使用者真正写的那个变量
    expect(r.errors[0]).toContain("GEBAI_CHANNEL_OUT_FEISHU")
    expect(r.errors[0]).toContain("不是合法 JSON")
  })

  test("不是数组 → 明确要求 JSON 数组", () => {
    const r = parse(JSON.stringify({ name: "x" }))
    expect(r.errors[0]).toContain("必须是 JSON 数组")
  })
})

describe("校验：键拼错必须报到实例级", () => {
  test("未知键 → 报错并列出可用键（拼错即静默失效，最危险）", () => {
    const r = parse(JSON.stringify([{ name: "feishu_main", appID: "cli_a", appSecret: "s" }]))
    console.log(`    实测: ${r.errors[0]}`)
    expect(r.errors[0]).toContain("FEISHU[0]（feishu_main）")
    expect(r.errors[0]).toContain("不认识的键「appID」")
    expect(r.errors[0]).toContain("appId") // 可用键里给出正确写法
  })

  test("缺必填项 → 指名道姓（而不是带着空凭据起跑）", () => {
    const r = parse(JSON.stringify([{ name: "feishu_main", appSecret: "s" }]))
    expect(r.errors.some((e) => e.includes("缺少必填项「appId」"))).toBe(true)
  })

  test("类型不对 → 不隐式转换（\"500\" 当数字用会掩盖配置错误）", () => {
    const r = parse(JSON.stringify([{ name: "feishu_main", appId: "a", appSecret: "s", ignoreSelf: "yes" }]))
    expect(r.errors[0]).toContain("必须是布尔值")
  })

  test("string[] 里混了非字符串 → 报错", () => {
    const r = parse(JSON.stringify([{ name: "feishu_main", appId: "a", appSecret: "s", chats: ["oc_1", 2] }]))
    expect(r.errors[0]).toContain("每项必须是非空字符串")
  })

  test("name 缺失 / 非法 / 重复 → 三种都拦住", () => {
    expect(parse(JSON.stringify([{ appId: "a", appSecret: "s" }])).errors[0]).toContain("缺少 name")
    expect(parse(JSON.stringify([{ name: "Feishu Main", appId: "a" }])).errors[0]).toContain("非法")
    const dup = parse(
      JSON.stringify([
        { name: "dup", appId: "a", appSecret: "s" },
        { name: "dup", appId: "b", appSecret: "t" },
      ]),
    )
    expect(dup.errors[0]).toContain("重复")
  })
})

describe("实例展开：注册表按配置造实例", () => {
  /** 造一个只做程序化注册的注册表（不依赖仓库文件）。 */
  function makeReg(home: string, env: Record<string, string>) {
    const reg = new ChannelRegistry({
      scanDirs: { builtin: join(home, "__n1__"), custom: join(home, "__n2__") },
      home,
      envReader: (n) => env[n],
      log: { info: () => {}, warn: () => {}, error: () => {} },
    })
    return reg
  }

  test("feishu 同类两份（两个出站实例）→ 各自拿到自己的凭据（凭据不串）", () => {
    const home = mkdtempSync(join(tmpdir(), "cfg-1-"))
    const reg = makeReg(home, {
      GEBAI_CHANNEL_OUT_FEISHU: JSON.stringify([
        { name: "feishu_main", appId: "cli_a", appSecret: "sa", chats: ["oc_1"] },
        { name: "feishu_ops", appId: "cli_b", appSecret: "sb", chats: ["oc_2"] },
      ]),
    })
    reg.register(feishu)
    const names = reg.list().map((d) => d.name).sort()
    console.log(`    实测: 实例=${names.join(",")}`)
    expect(names).toEqual(["feishu_main", "feishu_ops"])
    // ctx.config 是实例级：这是“同类两份”能成立的根本
    const cfgA = reg.context(reg.list().find((d) => d.name === "feishu_main")!).config
    const cfgB = reg.context(reg.list().find((d) => d.name === "feishu_ops")!).config
    expect(cfgA.appId).toBe("cli_a")
    expect(cfgB.appId).toBe("cli_b")
    rmSync(home, { recursive: true, force: true })
  })

  test("同一类型可同时有入站与出站实例（名字不同、能力面不同）", () => {
    const home = mkdtempSync(join(tmpdir(), "cfg-1b-"))
    const reg = makeReg(home, {
      GEBAI_CHANNEL_IN_FEISHU: JSON.stringify([{ name: "feishu_in", appId: "cli_x", appSecret: "sx" }]),
      GEBAI_CHANNEL_OUT_FEISHU: JSON.stringify([{ name: "feishu_out", appId: "cli_x", appSecret: "sx" }]),
    })
    reg.register(feishu)
    const dirs = reg.instanceDirections()
    console.log(`    实测: dirs=${JSON.stringify([...dirs])}`)
    expect([...dirs.keys()].sort()).toEqual(["feishu_in", "feishu_out"])
    expect(dirs.get("feishu_in")).toBe("in")
    expect(dirs.get("feishu_out")).toBe("out")
    // 能力面隔离：入站没有 act，出站没有 startListen
    const inDef = reg.list().find((d) => d.name === "feishu_in")!
    const outDef = reg.list().find((d) => d.name === "feishu_out")!
    expect(inDef.act).toBeUndefined()
    expect(outDef.startListen).toBeUndefined()
    rmSync(home, { recursive: true, force: true })
  })

  test("两个方向用同一个实例名 → 报重名，只留第一个", () => {
    const home = mkdtempSync(join(tmpdir(), "cfg-1c-"))
    const reg = makeReg(home, {
      GEBAI_CHANNEL_IN_FEISHU: JSON.stringify([{ name: "same", appId: "cli_x", appSecret: "sx" }]),
      GEBAI_CHANNEL_OUT_FEISHU: JSON.stringify([{ name: "same", appId: "cli_y", appSecret: "sy" }]),
    })
    reg.register(feishu)
    console.log(`    实测: 实例=${reg.list().map((d) => d.name).join(",")} errs=${JSON.stringify(reg.loadErrors())}`)
    expect(reg.list().map((d) => d.name)).toEqual(["same"])
    // 留下的是先展开的那一组（in）
    expect(reg.instanceDirections().get("same")).toBe("in")
    rmSync(home, { recursive: true, force: true })
  })

  test("requiresConfig 的类型未配置 → 不造实例，但把原因记成可查询的错误", () => {
    const home = mkdtempSync(join(tmpdir(), "cfg-2-"))
    const reg = makeReg(home, {})
    reg.register(feishu)
    expect(reg.list()).toEqual([])
    const errs = reg.loadErrors()
    console.log(`    实测: ${JSON.stringify(errs)}`)
    expect(errs.feishu).toContain("GEBAI_CHANNEL_IN_FEISHU")
    rmSync(home, { recursive: true, force: true })
  })

  test("可选配置的类型未配置 → 一个缺省实例（行为与改造前一致）", () => {
    const home = mkdtempSync(join(tmpdir(), "cfg-3-"))
    const reg = makeReg(home, {})
    reg.register(local)
    console.log(`    实测: 实例=${reg.list().map((d) => d.name).join(",")}`)
    expect(reg.list().map((d) => d.name)).toEqual(["local"])
    expect(reg.context(local).config).toEqual({})
    rmSync(home, { recursive: true, force: true })
  })

  test("配置有误 → 该类型不起实例（带半份配置跑比不跑更难查）", () => {
    const home = mkdtempSync(join(tmpdir(), "cfg-4-"))
    const reg = makeReg(home, { GEBAI_CHANNEL_IN_FEISHU: JSON.stringify([{ name: "x", appID: "typo" }]) })
    reg.register(feishu)
    expect(reg.list()).toEqual([])
    expect(reg.loadErrors()["in:feishu"]).toContain("配置有误")
    rmSync(home, { recursive: true, force: true })
  })

  test("没有启停名单：配置里写几个实例就几个实例", () => {
    const home = mkdtempSync(join(tmpdir(), "cfg-5-"))
    const reg = new ChannelRegistry({
      scanDirs: { builtin: join(home, "__n1__"), custom: join(home, "__n2__") },
      home,
      envReader: (n) =>
        n === "GEBAI_CHANNEL_OUT_FEISHU"
          ? JSON.stringify([
              { name: "feishu_main", appId: "a", appSecret: "s" },
              { name: "feishu_ops", appId: "b", appSecret: "t" },
            ])
          : undefined,
      log: { info: () => {}, warn: () => {}, error: () => {} },
    })
    reg.register(feishu)
    console.log(`    实测: 实例=${reg.list().map((d) => d.name).join(",")}`)
    // 不存在“名单”这回事：删掉一个实例就是从配置里删掉那一项（存在即启用）
    expect(reg.list().map((d) => d.name).sort()).toEqual(["feishu_main", "feishu_ops"])
    expect(reg.state().every((s) => s.enabled)).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })
})

describe("materializeInstance：方向裁剪能力面", () => {
  test("出站实例：保留写/读，**没有**收的一面（不建长连接）", () => {
    const inst = materializeInstance(feishu, { name: "feishu_out", dir: "out", settings: { appId: "b" } })
    console.log(`    实测: act=${!!inst.act} observes=${!!inst.observes} startListen=${!!inst.startListen}`)
    expect(inst.name).toBe("feishu_out")
    // 实现面（函数）共用，不复制
    expect(inst.act).toBe(feishu.act)
    expect(inst.verbs).toBe(feishu.verbs)
    // 裁剪：出站实例没有收的能力，模型也不会在 channels_list 里看到它“能收”
    expect(inst.startListen).toBeUndefined()
    expect(inst.listens).toBeUndefined()
  })

  test("入站实例：只保留收的一面，没有写/读", () => {
    const inst = materializeInstance(feishu, { name: "feishu_in", dir: "in", settings: { appId: "a" } })
    console.log(`    实测: act=${!!inst.act} verbs=${!!inst.verbs} startListen=${!!inst.startListen}`)
    expect(inst.startListen).toBe(feishu.startListen)
    expect(inst.listens).toBe(feishu.listens)
    expect(inst.act).toBeUndefined()
    expect(inst.verbs).toBeUndefined()
    expect(inst.observes).toBeUndefined()
    expect(inst.targets).toBeUndefined()
  })
})
