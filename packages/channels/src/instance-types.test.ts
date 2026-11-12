/**
 * 实例 → 类型名：**按渠道类型做治理**的基础设施。
 *
 * 为什么这一层必须存在：实例名是部署方起的（`feishu_in` / `feishu_out` / `ops_bot`），
 * 而“这是不是一个飞书渠道”是类型事实。任何按名字字串（`startsWith("feishu:")`）
 * 的判断都会在部署方给实例起别名时**整段静默失效**——出站审核、群白名单、
 * @全体整形全部跳过，日志里一个字都没有。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { ChannelRegistry } from "./registry"

function makeRegistry(home: string, env: Record<string, string>) {
  return new ChannelRegistry({
    scanDirs: { builtin: join(import.meta.dirname, "channels"), custom: join(home, "none") },
    home,
    envReader: (n) => env[n],
    log: { info: () => {}, warn: () => {}, error: () => {} },
  })
}

describe("instanceTypes：实例名 → 类型名", () => {
  test("同为 feishu 类型可以展开成 feishu_in / feishu_out 两个实例", async () => {
    const home = mkdtempSync(join(tmpdir(), "itypes-"))
    const reg = makeRegistry(home, {
      GEBAI_CHANNEL_IN_FEISHU: JSON.stringify([{ name: "feishu_in", appId: "cli_a", appSecret: "sa", chats: ["oc_a"] }]),
      GEBAI_CHANNEL_OUT_FEISHU: JSON.stringify([{ name: "feishu_out", appId: "cli_a", appSecret: "sa", chats: ["oc_b"] }]),
    })
    try {
      await reg.discoverOnly()
      const types = reg.instanceTypes()
      console.log(`    实测: ${JSON.stringify([...types])}`)
      expect(types.get("feishu_in")).toBe("feishu")
      expect(types.get("feishu_out")).toBe("feishu")
      // 方向仍然独立（类型相同不等于能力面相同）
      expect(reg.instanceDirections().get("feishu_in")).toBe("in")
      expect(reg.instanceDirections().get("feishu_out")).toBe("out")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("可选类型的缺省实例：类型名 = 实例名", async () => {
    const home = mkdtempSync(join(tmpdir(), "itypes-def-"))
    const reg = makeRegistry(home, {})
    try {
      await reg.discoverOnly()
      const types = reg.instanceTypes()
      console.log(`    实测: ${JSON.stringify([...types])}`)
      // local 未配置 → 缺省出站实例，名字与类型同为 local
      expect(types.get("local")).toBe("local")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("自定义实例名也映射回类型（ops_bot 仍是 feishu）", async () => {
    const home = mkdtempSync(join(tmpdir(), "itypes-alias-"))
    const reg = makeRegistry(home, {
      GEBAI_CHANNEL_OUT_FEISHU: JSON.stringify([{ name: "ops_bot", appId: "cli_c", appSecret: "sc", chats: ["oc_c"] }]),
    })
    try {
      await reg.discoverOnly()
      expect(reg.instanceTypes().get("ops_bot")).toBe("feishu")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
