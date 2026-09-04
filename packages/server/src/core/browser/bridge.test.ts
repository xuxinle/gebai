import { describe, expect, test } from "bun:test"
import { mkdtempSync, writeFileSync, readFileSync, existsSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { Bridge, createLazyBridge, lazyBridge, materializePwCore, resolveChannel } from "./bridge"

/** 简易 fake driver（node 行协议 echo server），供 Bridge 协议测试。 */
function fakeDriverScript(): string {
  const dir = mkdtempSync(join(tmpdir(), "gebai-pw-driver-"))
  const file = join(dir, "fake-driver.mjs")
  const nl = JSON.stringify("\n") // 源码中的 "\n" 字面量（避免模板转义歧义）
  writeFileSync(
    file,
    `import { createInterface } from "node:readline"
const rl = createInterface({ input: process.stdin, crlfDelay: Infinity })
rl.on("line", (line) => {
  const req = JSON.parse(line)
  if (req.op === "init") process.stdout.write(JSON.stringify({ id: req.id, ok: true, result: {} }) + ${nl})
  else if (req.op === "ping") process.stdout.write(JSON.stringify({ id: req.id, ok: true, result: { pong: true } }) + ${nl})
  else if (req.op === "hang") { /* 故意不响应，测超时 */ }
  else if (req.op === "err") process.stdout.write(JSON.stringify({ id: req.id, ok: false, error: "boom" }) + ${nl})
  else process.stdout.write(JSON.stringify({ id: req.id, ok: false, error: "unknown op" }) + ${nl})
})
`
  )
  return file
}

describe("lazy bridge", () => {
  test("lazyBridge defers construction to first request and reuses instance", async () => {
    let constructed = 0
    let inner!: Bridge
    const lazy = lazyBridge(() => {
      constructed++
      inner = new Bridge({ driverPath: fakeDriverScript(), playwrightModule: "file:///fake/index.mjs", requestTimeoutMs: 5_000 })
      return inner
    })
    expect(constructed).toBe(0) // 构造零副作用：创建代理不触发 playwright 解析/进程启动
    expect(await lazy.request("ping", {})).toEqual({ pong: true })
    await lazy.request("ping", {})
    expect(constructed).toBe(1) // 复用同一实例
    inner.kill()
  })

  test("createLazyBridge returns process-wide shared singleton", () => {
    expect(createLazyBridge()).toBe(createLazyBridge())
  })

  test("Bridge construction has no side effects (no playwright resolution)", () => {
    expect(() => new Bridge()).not.toThrow()
  })

  test("resolveChannel: env override wins, win32 defaults to msedge", () => {
    expect(resolveChannel("win32", undefined)).toBe("msedge")
    expect(resolveChannel("linux", undefined)).toBe("")
    expect(resolveChannel("linux", "chrome")).toBe("chrome")
    expect(resolveChannel("win32", "")).toBe("")
  })

  test("materializePwCore writes files, guards traversal, rebuilds on version change", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-pw-vendor-"))
    const prevHome = process.env.GEBAI_HOME
    process.env.GEBAI_HOME = home
    try {
      const gz = (s: string) => Buffer.from(Bun.gzipSync(Buffer.from(s))).toString("base64")
      const mk = (version: string) => ({
        version,
        entry: "index.mjs",
        files: [
          { path: "index.mjs", data: gz(`export const v = "${version}"`) },
          { path: "lib/a.js", data: gz("module.exports = 1") },
          { path: "../evil.js", data: gz("bad" ) },
        ],
      })
      await materializePwCore(mk("v1"))
      const dir = join(home, "vendor", "playwright-core")
      expect(readFileSync(join(dir, "index.mjs"), "utf8")).toContain('"v1"')
      expect(existsSync(join(dir, "lib", "a.js"))).toBe(true)
      expect(existsSync(join(home, "vendor", "evil.js"))).toBe(false) // 穿越条目被拒
      expect(readFileSync(join(home, "vendor", "playwright-core.version"), "utf8")).toBe("v1")
      // 版本一致：跳过重建（marker 未变）
      await materializePwCore(mk("v1"))
      expect(readFileSync(join(home, "vendor", "playwright-core.version"), "utf8")).toBe("v1")
      // 版本变化：整目录重建
      await materializePwCore(mk("v2"))
      expect(readFileSync(join(dir, "index.mjs"), "utf8")).toContain('"v2"')
      expect(readFileSync(join(home, "vendor", "playwright-core.version"), "utf8")).toBe("v2")
    } finally {
      if (prevHome === undefined) delete process.env.GEBAI_HOME
      else process.env.GEBAI_HOME = prevHome
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("Bridge protocol", () => {
  test("spawns node driver, init + ping roundtrip", async () => {
    const bridge = new Bridge({ driverPath: fakeDriverScript(), playwrightModule: "file:///fake/index.mjs", requestTimeoutMs: 5_000 })
    const pong = await bridge.request("ping", {})
    expect(pong).toEqual({ pong: true })
    // init 已在启动时自动发送；重复请求复用进程
    const pong2 = await bridge.request("ping", {})
    expect(pong2).toEqual({ pong: true })
    bridge.kill()
  })

  test("driver error surfaces as rejection", async () => {
    const bridge = new Bridge({ driverPath: fakeDriverScript(), playwrightModule: "file:///fake/index.mjs", requestTimeoutMs: 5_000 })
    await expect(bridge.request("err", {})).rejects.toThrow("boom")
    bridge.kill()
  })

  test("request timeout kills and restarts the process", async () => {
    const bridge = new Bridge({ driverPath: fakeDriverScript(), playwrightModule: "file:///fake/index.mjs", requestTimeoutMs: 150 })
    await expect(bridge.request("hang", {})).rejects.toThrow(/超时/)
    // 超时后进程被重置，下一请求自动重启并可正常服务
    const pong = await bridge.request("ping", {})
    expect(pong).toEqual({ pong: true })
    bridge.kill()
  })

  test("missing driver file reports clear error", async () => {
    const bridge = new Bridge({ driverPath: join(mkdtempSync(join(tmpdir(), "gebai-pw-none-")), "nope.mjs"), playwrightModule: "file:///fake/index.mjs", requestTimeoutMs: 1_000 })
    await expect(bridge.request("ping", {})).rejects.toThrow(/驱动缺失/)
  })
})
