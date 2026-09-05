import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, mkdirSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname } from "node:path"
import { deflateSync } from "node:zlib"
import type { ToolContext } from "../../core/base/types"
import type { RgbaImage } from "../../core/cv/image"
import { setCvRunnerFactory, type CvRunner } from "../../core/cv/cv"
import { ocrTool, locateTool, detectTool } from "./desktop_cv_tools"

const ORIGINAL_PLATFORM = process.platform
beforeAll(() => {
  Object.defineProperty(process, "platform", { value: "win32" })
})
afterAll(() => {
  Object.defineProperty(process, "platform", { value: ORIGINAL_PLATFORM })
  setCvRunnerFactory(null)
})

/* ---------- 微型 PNG 编码器（filter 0，CRC 写零——解码器不校验） ---------- */

function chunk(type: string, data: number[]): number[] {
  const len = [(data.length >>> 24) & 255, (data.length >>> 16) & 255, (data.length >>> 8) & 255, data.length & 255]
  return [...len, ...Array.from(type, (c) => c.charCodeAt(0)), ...data, 0, 0, 0, 0]
}

function pngBytes(w: number, h: number): Uint8Array {
  const be32 = (v: number) => [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255]
  const ihdr = [...be32(w), ...be32(h), 8, 6, 0, 0, 0]
  const raw: number[] = []
  for (let y = 0; y < h; y++) {
    raw.push(0) // filter none
    for (let x = 0; x < w; x++) raw.push(120, 120, 120, 255)
  }
  const idat = Array.from(deflateSync(Buffer.from(raw)))
  return new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10, ...chunk("IHDR", ihdr), ...chunk("IDAT", idat), ...chunk("IEND", [])])
}

/* ---------- ctx 工厂与假 runner ---------- */

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

const seen: { img?: RgbaImage } = {}

function fakeRunner(lines: Array<{ text: string; score: number; box: { x: number; y: number; w: number; h: number } }>, objects: Array<{ label: string; score: number; x: number; y: number; w: number; h: number }> = []): CvRunner {
  return {
    ocr: async (img) => {
      seen.img = img
      return lines
    },
    detect: async () => objects,
  }
}

function decodeCmd(cmd: string): string {
  const m = cmd.match(/-EncodedCommand (\S+)/)
  return m ? Buffer.from(m[1], "base64").toString("utf16le") : cmd
}

const DEFAULT_LINES = [
  { text: "保存", score: 0.95, box: { x: 10, y: 20, w: 60, h: 24 } },
  { text: "取消", score: 0.9, box: { x: 100, y: 20, w: 60, h: 24 } },
  { text: "保存全部", score: 0.8, box: { x: 10, y: 60, w: 90, h: 24 } },
]
const DEFAULT_OBJECTS = [{ label: "icon_save", score: 0.88, x: 12, y: 22, w: 56, h: 20 }]

function installDefaultFake(): void {
  setCvRunnerFactory(() => fakeRunner(DEFAULT_LINES, DEFAULT_OBJECTS))
}

beforeAll(() => installDefaultFake())

describe("desktop cv tools", () => {
  test("沙箱模式拒绝", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    await expect(ocrTool.execute({}, ctx(home, { sandboxed: true }))).rejects.toThrow(/本地\/桌面/)
    await expect(locateTool.execute({ target: "x" }, ctx(home, { sandboxed: true }))).rejects.toThrow(/本地\/桌面/)
  })

  test("ocr：image 参数读 PNG，输出行文本+坐标，data 带行", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const r = await ocrTool.execute({ image: "shot.png" }, c)
    expect(r.output).toContain("保存")
    expect(r.output).toContain("中心 (40,32)") // 10+30, 20+12
    const data = r.data as { lines: Array<{ text: string; x: number }> }
    expect(data.lines.length).toBe(3)
    expect(data.lines[0].x).toBe(10)
    expect(seen.img?.width).toBe(200)
  })

  test("ocr：region 裁剪后识别，坐标映射回原点偏移", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const r = await ocrTool.execute({ image: "shot.png", region: "50,40,100,60" }, c)
    expect(seen.img?.width).toBe(100) // 裁剪后传入 runner
    expect(seen.img?.height).toBe(60)
    const data = r.data as { lines: Array<{ x: number; y: number }> }
    expect(data.lines[0].x).toBe(60) // 10 + 50
    expect(data.lines[0].y).toBe(60) // 20 + 40
    expect(r.output).toContain("中心 (90,72)")
  })

  test("ocr：find 过滤", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const r = await ocrTool.execute({ image: "shot.png", find: "保存" }, c)
    expect(r.output).toContain("保存")
    expect(r.output).not.toContain("取消")
  })

  test("ocr：现截全屏（runCommand 落盘 PNG）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home, {
      runCommand: async (cmd) => {
        if (cmd.includes("powershell")) {
          const m = decodeCmd(cmd).match(/'([^']+\.png)'/)
          if (m) await Bun.write(m[1], pngBytes(300, 200))
        }
        return { stdout: "OK", stderr: "", code: 0 }
      },
    })
    const r = await ocrTool.execute({}, c)
    expect(r.output).toContain("当前屏幕（全屏）")
    expect(seen.img?.width).toBe(300)
    expect((r.data as { lines: unknown[] }).lines.length).toBe(3)
  })

  test("ocr：超过 200 行截断提示", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    setCvRunnerFactory(() => fakeRunner(Array.from({ length: 250 }, (_, i) => ({ text: `行${i}`, score: 0.9, box: { x: 0, y: i, w: 50, h: 10 } }))))
    try {
      const c = ctx(home)
      await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
      const r = await ocrTool.execute({ image: "shot.png" }, c)
      expect(r.output).toContain("250 行")
      expect(r.output).toContain("find")
    } finally {
      installDefaultFake()
    }
  })

  test("ocr：runner 抛指引错误 → 输出错误而非抛出", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    setCvRunnerFactory(() => ({
      ocr: async () => { throw new Error("本地识别模型未配置：请设置 GEBAI_CV_MODELS_DIR") },
      detect: async () => { throw new Error("x") },
    }))
    try {
      const c = ctx(home)
      await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
      const r = await ocrTool.execute({ image: "shot.png" }, c)
      expect(r.output).toContain("GEBAI_CV_MODELS_DIR")
    } finally {
      installDefaultFake()
    }
  })

  test("ocr：非 PNG 明确报错", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home)
    const r = await ocrTool.execute({ image: "photo.jpg" }, c)
    expect(r.output).toContain("PNG")
  })

  test("locate：完全匹配优先，返回中心坐标与 mouse_click 提示", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const r = await locateTool.execute({ target: "保存", image: "shot.png" }, c)
    expect(r.output).toContain("中心 (40,32)")
    expect(r.output).toContain("mouse_click(40, 32)")
    expect(r.output).toContain("保存全部") // 包含匹配的弱候选也列出
    const data = r.data as { found: boolean; best: { text: string; center: number[] } }
    expect(data.found).toBe(true)
    expect(data.best.text).toBe("保存")
    expect(data.best.center).toEqual([40, 32])
  })

  test("locate：未找到给出多通道建议", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const r = await locateTool.execute({ target: "不存在", image: "shot.png" }, c)
    expect(r.output).toContain("未找到")
    expect(r.output).toContain("desktop_ocr")
    expect((r.data as { found: boolean }).found).toBe(false)
  })

  test("detect：未配置环境变量 → 指引；配置后返回对象", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    setCvRunnerFactory(() => ({
      ocr: async () => { throw new Error("未配置") },
      detect: async () => { throw new Error("目标检测未配置：需设置 GEBAI_CV_DETECT_MODEL（YOLO ONNX 模型路径）与 GEBAI_CV_DETECT_LABELS") },
    }))
    let c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    let r = await detectTool.execute({ image: "shot.png" }, c)
    expect(r.output).toContain("GEBAI_CV_DETECT_MODEL")
    // 配置后（fake runner 正常返回）
    setCvRunnerFactory(() => fakeRunner([], [{ label: "icon_save", score: 0.88, x: 12, y: 22, w: 56, h: 20 }]))
    c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    r = await detectTool.execute({ image: "shot.png" }, c)
    expect(r.output).toContain("icon_save")
    expect(r.output).toContain("1 个对象")
    const data = r.data as { objects: Array<{ label: string; x: number }> }
    expect(data.objects[0].label).toBe("icon_save")
    expect(data.objects[0].x).toBe(12)
  })
})
