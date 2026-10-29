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
import { CEFU_EXE_NAME, makeFrameReader, resolveCefuBin } from "./controller"

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
    fakeExists(repo, ["shuangtian", "examples", "cefu", "ui", "main.xml"])
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