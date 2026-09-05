import { describe, expect, test, afterAll } from "bun:test"
import { mkdtempSync, mkdirSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname } from "node:path"
import { deflateSync } from "node:zlib"
import type { ToolContext } from "../../core/base/types"
import type { RgbaImage } from "../../core/cv/image"
import { setCvRunnerFactory, type CvRunner } from "../../core/cv/cv"
import type { BridgeLike } from "../../core/browser/bridge"
import { createPlaywrightCvTools } from "./playwright_cv_tools"

afterAll(() => {
  setCvRunnerFactory(null)
})

/* ---------- 微型 PNG 编码器（filter 0，CRC 写零——解码器不校验） ---------- */

function chunk(type: string, data: number[]): number[] {
  const len = [(data.length >>> 24) & 255, (data.length >>> 16) & 255, (data.length >>> 8) & 255, data.length & 255]
  return [...len, ...Array.from(type, (c) => c.charCodeAt(0)), ...data, 0, 0, 0, 0]
}

function encodeRgba(img: RgbaImage): Uint8Array {
  const be32 = (v: number) => [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255]
  const ihdr = [...be32(img.width), ...be32(img.height), 8, 6, 0, 0, 0]
  const raw: number[] = []
  for (let y = 0; y < img.height; y++) {
    raw.push(0)
    for (let x = 0; x < img.width; x++) {
      const o = (y * img.width + x) * 4
      raw.push(img.data[o], img.data[o + 1], img.data[o + 2], img.data[o + 3])
    }
  }
  const idat = Array.from(deflateSync(Buffer.from(raw)))
  return new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10, ...chunk("IHDR", ihdr), ...chunk("IDAT", idat), ...chunk("IEND", [])])
}

function pngBytes(w: number, h: number, gray = 120): Uint8Array {
  return encodeRgba({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4).fill(gray, 0, w * h * 4).map((v, i) => ((i & 3) === 3 ? 255 : v)) })
}

function mulberry32(seed: number): () => number {
  let a = seed
  return () => {
    a |= 0
    a = (a + 0x6d2b79f5) | 0
    let t = Math.imul(a ^ (a >>> 15), 1 | a)
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
}

/** 渐变背景 + 伪随机纹理 patch 的合成搜索图（供模板匹配用例）。 */
function makeSearchWithPatch(w: number, h: number, patch: RgbaImage, px: number, py: number, seed = 42): RgbaImage {
  const rnd = mulberry32(seed)
  const data = new Uint8ClampedArray(w * h * 4)
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const base = 60 + (x * 120) / w + (y * 40) / h + rnd() * 8
      const o = (y * w + x) * 4
      data[o] = base
      data[o + 1] = base * 0.8 + rnd() * 6
      data[o + 2] = 160 - base * 0.5
      data[o + 3] = 255
    }
  }
  for (let y = 0; y < patch.height; y++) {
    for (let x = 0; x < patch.width; x++) {
      const d = (py + y) * w + px + x
      const s = y * patch.width + x
      data[d * 4] = patch.data[s * 4]
      data[d * 4 + 1] = patch.data[s * 4 + 1]
      data[d * 4 + 2] = patch.data[s * 4 + 2]
    }
  }
  return { width: w, height: h, data }
}

function makePatch(size: number, seed: number): RgbaImage {
  const rnd = mulberry32(seed)
  const data = new Uint8ClampedArray(size * size * 4)
  for (let i = 0; i < size * size; i++) {
    const v = 40 + rnd() * 200
    data[i * 4] = v
    data[i * 4 + 1] = 200 - v * 0.7
    data[i * 4 + 2] = v * 0.5 + 30
    data[i * 4 + 3] = 255
  }
  return { width: size, height: size, data }
}

/* ---------- ctx 工厂 / 假桥接 / 假 runner ---------- */

function ctx(home: string, overrides: Partial<ToolContext> = {}): ToolContext {
  const tmp = join(home, "users", "default", "sessions", "s1", "tmp")
  mkdirSync(tmp, { recursive: true })
  const base: ToolContext = {
    user: "default",
    sessionId: "s1",
    workdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => join(tmp, p),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, content) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(dirname(p), { recursive: true })
      await writeFile(p, content)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 1 }),
    uploadAttachment: async (r) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => { throw new Error("未知预置项目") },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    runNewSession: async () => ({ output: "ok", archive: { runId: "r", agents: ["x"], input: "", output: "ok", messages: [] } }),
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
    waitForCapture: async () => null,
  }
  return { ...base, ...overrides }
}

/** 假桥接：记录 op 调用；screenshot 落盘指定 PNG（可注入失败）。 */
function fakeBridge(png: () => Uint8Array, opts: { fail?: boolean } = {}): { bridge: BridgeLike; ops: Array<{ op: string; args: Record<string, unknown> }> } {
  const ops: Array<{ op: string; args: Record<string, unknown> }> = []
  return {
    ops,
    bridge: {
      request: async (op, args) => {
        ops.push({ op, args: { ...args } })
        if (op === "screenshot") {
          if (opts.fail) throw new Error("No page open")
          await Bun.write(String(args.path), png())
        }
        return {}
      },
    },
  }
}

const seen: { img?: RgbaImage } = {}

const DEFAULT_LINES = [
  { text: "登录", score: 0.95, box: { x: 10, y: 20, w: 60, h: 24 } },
  { text: "注册", score: 0.9, box: { x: 100, y: 20, w: 60, h: 24 } },
]

function installFake(lines = DEFAULT_LINES): void {
  setCvRunnerFactory(
    (): CvRunner => ({
      ocr: async (img) => {
        seen.img = img
        return lines
      },
      detect: async () => [],
    }),
  )
}

describe("playwright cv tools", () => {
  test("ocr：image 省略 → 经桥接截当前页视口（fullPage=false 固定文件复用）+ 本地识别", async () => {
    installFake()
    const home = mkdtempSync(join(tmpdir(), "gebai-pwcv-"))
    const fb = fakeBridge(() => pngBytes(300, 200))
    const { ocr } = createPlaywrightCvTools({ bridge: fb.bridge })
    const r = await ocr.execute({}, ctx(home))
    expect(fb.ops.length).toBe(1)
    expect(fb.ops[0].op).toBe("screenshot")
    expect(fb.ops[0].args.fullPage).toBe(false)
    expect(String(fb.ops[0].args.path)).toContain("pw_cv_capture.png")
    expect(r.output).toContain("登录")
    expect(r.output).toContain("视口")
    expect(seen.img?.width).toBe(300)
    expect((r.data as { lines: unknown[] }).lines.length).toBe(2)
  })

  test("ocr：沙箱（服务端部署）可用——无 desktop 本地模式闸门", async () => {
    installFake()
    const home = mkdtempSync(join(tmpdir(), "gebai-pwcv-"))
    const fb = fakeBridge(() => pngBytes(300, 200))
    const { ocr } = createPlaywrightCvTools({ bridge: fb.bridge })
    const r = await ocr.execute({ find: "登录" }, ctx(home, { sandboxed: true }))
    expect(r.output).toContain("登录")
    expect(r.output).not.toContain("注册")
  })

  test("ocr：image 给 PNG 不走桥接；region 裁剪识别、坐标映射回视口像素系", async () => {
    installFake()
    const home = mkdtempSync(join(tmpdir(), "gebai-pwcv-"))
    const fb = fakeBridge(() => pngBytes(300, 200))
    const { ocr } = createPlaywrightCvTools({ bridge: fb.bridge })
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const r = await ocr.execute({ image: "shot.png", region: "50,40,100,60" }, c)
    expect(fb.ops.length).toBe(0) // 显式 image 不触桥接
    expect(seen.img?.width).toBe(100)
    expect(seen.img?.height).toBe(60)
    const data = r.data as { lines: Array<{ x: number; y: number }> }
    expect(data.lines[0].x).toBe(60) // 10 + 50（区域原点偏移回加 → 视口坐标）
    expect(data.lines[0].y).toBe(60) // 20 + 40
  })

  test("ocr：桥接失败（未打开页面）→ 友好错误提示先 open", async () => {
    installFake()
    const home = mkdtempSync(join(tmpdir(), "gebai-pwcv-"))
    const fb = fakeBridge(() => pngBytes(300, 200), { fail: true })
    const { ocr } = createPlaywrightCvTools({ bridge: fb.bridge })
    const r = await ocr.execute({}, ctx(home))
    expect(r.output).toContain("页面截图失败")
    expect(r.output).toContain("open")
  })

  test("locate：命中返回中心坐标与 elementFromPoint 消费提示；未找到建议 DOM 通道", async () => {
    installFake()
    const home = mkdtempSync(join(tmpdir(), "gebai-pwcv-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const { locate } = createPlaywrightCvTools({ bridge: fakeBridge(() => pngBytes(1, 1)).bridge })
    const hit = await locate.execute({ target: "登录", image: "shot.png" }, c)
    expect(hit.output).toContain("中心 (40,32)")
    expect(hit.output).toContain("document.elementFromPoint(40, 32)")
    const miss = await locate.execute({ target: "不存在", image: "shot.png" }, c)
    expect(miss.output).toContain("未找到")
    expect(miss.output).toContain("playwright_ocr")
    expect(miss.output).toContain("content")
    expect((miss.data as { found: boolean }).found).toBe(false)
  })

  test("locate_image：缺省源=当前页截图，模板命中返回视口坐标；未找到给建议", async () => {
    installFake()
    const home = mkdtempSync(join(tmpdir(), "gebai-pwcv-"))
    const patch = makePatch(24, 99)
    const fb = fakeBridge(() => encodeRgba(makeSearchWithPatch(300, 200, patch, 60, 40)))
    const { locate_image } = createPlaywrightCvTools({ bridge: fb.bridge })
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "icon.png"), encodeRgba(patch))
    const r = await locate_image.execute({ template: "icon.png" }, c)
    expect(fb.ops.length).toBe(1) // 缺省源经桥接截当前页
    expect(r.output).toContain("中心 (72,52)") // 60+12, 40+12（视口坐标）
    expect(r.output).toContain("document.elementFromPoint(72, 52)")
    const data = r.data as { found: boolean; best: { center: number[] } }
    expect(data.found).toBe(true)
    expect(data.best.center).toEqual([72, 52])
    // 不存在的模板 → 未找到建议
    await Bun.write(join(c.workdir!, "absent.png"), encodeRgba(makePatch(24, 777)))
    const miss = await locate_image.execute({ template: "absent.png" }, c)
    expect(miss.output).toContain("未找到")
    expect(miss.output).toContain("playwright_locate")
  })
})
