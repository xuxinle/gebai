import { describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { loadConfig } from "./core/config"
import { DevReloadManager } from "./dev-reload"

/** 模拟构建进程：输出 "built" 行（stdout），delayMs = 本行输出前等待（行间隔）。 */
function fakeBuilder(lines: Array<{ text: string; delayMs?: number }>): string[] {
  const parts = lines.map((l) => `await Bun.sleep(${l.delayMs ?? 0});console.log(${JSON.stringify(l.text)})`)
  return [process.execPath, "-e", parts.join(";")]
}

function waitFor(fn: () => boolean, timeoutMs: number): Promise<void> {
  return new Promise((resolve, reject) => {
    const start = Date.now()
    const t = setInterval(() => {
      if (fn()) {
        clearInterval(t)
        resolve()
      } else if (Date.now() - start > timeoutMs) {
        clearInterval(t)
        reject(new Error("timeout waiting for condition"))
      }
    }, 50)
  })
}

describe("DevReloadManager", () => {
  test("stdout built 触发 onChange；连续 built 防抖合并为一次", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-devrel-"))
    let fired = 0
    const m = new DevReloadManager(dir, () => fired++, fakeBuilder([{ text: "built" }, { text: "built" }]))
    m.start()
    try {
      // 两个连续 built（间隔 < 200ms 防抖窗口）→ 合并为 1 次 onChange
      await waitFor(() => fired === 1, 3000)
      // 等待足够久确认没有第二次触发
      await Bun.sleep(500)
      expect(fired).toBe(1)
    } finally {
      m.stop()
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("间隔超过防抖窗口的两次 built 分别触发", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-devrel2-"))
    let fired = 0
    const m = new DevReloadManager(
      dir,
      () => fired++,
      fakeBuilder([
        { text: "built" },
        { text: "built", delayMs: 400 },
      ]),
    )
    m.start()
    try {
      await waitFor(() => fired >= 2, 3000)
      expect(fired).toBe(2)
    } finally {
      m.stop()
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("stop 终止子进程且不再触发", async () => {
    const dir = mkdtempSync(join(tmpdir(), "gebai-devrel3-"))
    let fired = 0
    const m = new DevReloadManager(dir, () => fired++, fakeBuilder([{ text: "built" }]))
    m.start()
    await waitFor(() => fired === 1, 3000)
    m.stop()
    const before = fired
    await Bun.sleep(300)
    expect(fired).toBe(before)
    rmSync(dir, { recursive: true, force: true })
  })
})

describe("config devReload flag", () => {
  test("默认 false；overrides 可强制开启", () => {
    const cfg = loadConfig({ devReload: process.argv.includes("--reload") || process.env.GEBAI_DEV_RELOAD === "1" })
    expect(cfg.devReload).toBe(false)
    expect(loadConfig({ devReload: true }).devReload).toBe(true)
  })
})
