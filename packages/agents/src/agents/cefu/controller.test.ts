/**
 * 册府控制器用例：exe 探测顺序（GEBAI_HOME 直查/PATH 后备）与协议帧读写。
 *
 * 不真 spawn cefu.exe（无头进程依赖构建产物与运行环境）——探测与协议
 * 是纯函数/可注入部分，测它们就够；端到端留给人工验证（见 controller.ts 头注释）。
 */
import { afterAll, describe, expect, test } from "bun:test"
import { existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { CEFU_EXE_NAME, buildCefuArgs, makeFrameReader, resolveCefuBin, resolveCefuMode } from "./controller"

const roots: string[] = []
function tmp(tag: string): string {
  const d = mkdtempSync(join(tmpdir(), `gebai-cefu-${tag}-`))
  roots.push(d)
  return d
}

function fakeExists(root: string, rel: string[]) {
  const dir = join(root, ...rel.slice(0, -1))
  mkdirSync(dir, { recursive: true })
  writeFileSync(join(root, ...rel), "fake")
}

// 测试统一真实 fs:existsSync（fake 文件是真实落盘的）
const exists = existsSync

afterAll(() => {
  for (const r of roots) rmSync(r, { recursive: true, force: true })
})

describe("resolveCefuBin 探测顺序", () => {
  test("CEFU_BIN 覆盖最高优先级", () => {
    const home = tmp("ovr")
    const override = join(home, "custom", CEFU_EXE_NAME)
    mkdirSync(join(home, "custom"), { recursive: true })
    writeFileSync(override, "x")
    const r = resolveCefuBin({ gebaiHome: home, moduleDir: join(home, "x"), override })
    expect(r.bin).toBe(override)
    expect(r.fromOverride).toBe(true)
  })

  test("GEBAI_HOME 直查：{GEBAI_HOME}/cefu.exe 被找到（无仓库树）", () => {
    const home = tmp("home")
    fakeExists(home, [CEFU_EXE_NAME])
    const r = resolveCefuBin({
      gebaiHome: home,
      moduleDir: join(home, "nowhere", "deep"),
      exists: (p) => exists(p),
    })
    expect(r.bin).toBe(join(home, CEFU_EXE_NAME))
  })

  test("PATH 后备：which 命中（非仓库/非 GEBAI_HOME）", () => {
    const home = tmp("path")
    const onPath = join(home, "bin", CEFU_EXE_NAME)
    mkdirSync(join(home, "bin"), { recursive: true })
    writeFileSync(onPath, "x")
    const r = resolveCefuBin({
      gebaiHome: home,
      moduleDir: join(home, "nowhere"),
      which: () => onPath,
    })
    expect(r.bin).toBe(onPath)
  })

  test("仓库树优先于 GEBAI_HOME 直查（同根时）", () => {
    const repo = tmp("repo")
    fakeExists(repo, ["shuangtian", "apps", "cefu", "ui", "main.xml"])
    fakeExists(repo, ["shuangtian", "build", "dev", "bin", CEFU_EXE_NAME])
    // 同时放 GEBAI_HOME 直查：仓库树应优先
    fakeExists(repo, [CEFU_EXE_NAME])
    const r = resolveCefuBin({
      gebaiHome: repo,
      moduleDir: join(repo, "packages", "server", "src", "boot"),
      exists: (p) => exists(p),
    })
    expect(r.bin).toBe(join(repo, "shuangtian", "build", "dev", "bin", CEFU_EXE_NAME))
  })

  test("全部找不到：candidates 列全", () => {
    const home = tmp("none")
    const r = resolveCefuBin({ gebaiHome: home, moduleDir: join(home, "x") })
    expect(r.bin).toBeNull()
    expect(r.candidates.length).toBeGreaterThan(0)
  })
})

describe("resolveCefuMode：启动姿态", () => {
  test("缺省是窗口（不是无头）——用户报的就是这个", () => {
    // 为何捕这条：旧实现把 `--headless` 写死在参数字符串里，读代码时很容易
    // 以为“它本来就这样”而没意识到那是个默认值。
    expect(resolveCefuMode({})).toBe("window")
    expect(resolveCefuMode({ CEFU_HEADLESS: "" })).toBe("window")
    expect(resolveCefuMode({ CEFU_HEADLESS: "0" })).toBe("window")
    expect(resolveCefuMode({ CEFU_HEADLESS: "no" })).toBe("window")
  })

  test("CEFU_HEADLESS 的肯定值（无桌面环境逃生口）", () => {
    for (const v of ["1", "true", "TRUE", "yes", "on", " 1 "]) {
      expect(resolveCefuMode({ CEFU_HEADLESS: v })).toBe("headless")
    }
  })
})

describe("buildCefuArgs：命令行拼接", () => {
  const base = { dbPath: "/home/x/cefu.db", noSeedPath: "/home/x/no-seed.md", ctlFile: "/home/x/ctl.json" }

  test("窗口模式**不得出现** `--headless`", () => {
    const args = buildCefuArgs({ ...base, mode: "window" })
    expect(args).not.toContain("--headless")
    expect(args[0]).toBe("--db")
  })

  test("无头模式才带 `--headless`（且只在开头一个）", () => {
    const args = buildCefuArgs({ ...base, mode: "headless" })
    expect(args.filter((a) => a === "--headless").length).toBe(1)
    expect(args[0]).toBe("--headless")
  })

  test("四个必需参数一个都不能漏（漏 seed 污染生产库、漏 ctl 连不上）", () => {
    const args = buildCefuArgs({ ...base, mode: "window" })
    expect(args).toContain("--db")
    expect(args[args.indexOf("--db") + 1]).toBe(base.dbPath)
    expect(args).toContain("--seed")
    expect(args[args.indexOf("--seed") + 1]).toBe(base.noSeedPath)
    expect(args).toContain("--control-port")
    expect(args[args.indexOf("--control-port") + 1]).toBe("0")
    expect(args).toContain("--control-file")
    expect(args[args.indexOf("--control-file") + 1]).toBe(base.ctlFile)
  })

  test("uiDir：给了才追加（空值不产生孤立的 `--ui`）", () => {
    expect(buildCefuArgs({ ...base, mode: "window", uiDir: "/repo/ui" })).toContain("--ui")
    expect(buildCefuArgs({ ...base, mode: "window", uiDir: null })).not.toContain("--ui")
    expect(buildCefuArgs({ ...base, mode: "window" })).not.toContain("--ui")
  })
})

describe("协议帧读写（makeFrameReader）", () => {
  test("读出一帧（4 字节大端长度 + body）", async () => {
    const listeners: Array<(d: Buffer) => void> = []
    const fakeSocket = { on: (_: string, cb: (d: Buffer) => void) => listeners.push(cb) }
    const read = makeFrameReader(fakeSocket as never)
    const p = read(1000)
    const body = Buffer.from('{"id":1,"method":"hello"}', "utf8")
    const frame = Buffer.alloc(4 + body.length)
    frame.writeUInt32BE(body.length, 0)
    body.copy(frame, 4)
    listeners[0](frame)
    const got = await p
    expect(got.toString("utf8")).toBe('{"id":1,"method":"hello"}')
  })

  test("跨 chunk 拼帧：分两次投递", async () => {
    const listeners: Array<(d: Buffer) => void> = []
    const fakeSocket = { on: (_: string, cb: (d: Buffer) => void) => listeners.push(cb) }
    const read = makeFrameReader(fakeSocket as never)
    const p = read(1000)
    const body = Buffer.from('{"ok":true}', "utf8")
    const frame = Buffer.alloc(4 + body.length)
    frame.writeUInt32BE(body.length, 0)
    body.copy(frame, 4)
    // 第一半：长度头 + body 前 2 字节
    listeners[0](frame.subarray(0, 6))
    // 第二半：剩余
    listeners[0](frame.subarray(6))
    const got = await p
    expect(got.toString("utf8")).toBe('{"ok":true}')
  })
})