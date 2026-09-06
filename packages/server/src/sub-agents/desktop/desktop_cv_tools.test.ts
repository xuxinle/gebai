import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, mkdirSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname } from "node:path"
import { deflateSync } from "node:zlib"
import type { ToolContext } from "../../core/base/types"
import type { RgbaImage } from "../../core/cv/image"
import { setCvRunnerFactory, type CvRunner } from "../../core/cv/cv"
import { ocrTool, locateTool, locateImageTool, detectTool, waitForTool } from "./desktop_cv_tools"

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

/** encodeRgba：RGBA 图 → PNG 字节；pngBytes(w,h)：统一灰；pngBytes(w,h,fill)：逐像素填充。 */
function encodeRgba(img: RgbaImage): Uint8Array {
  const be32 = (v: number) => [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255]
  const ihdr = [...be32(img.width), ...be32(img.height), 8, 6, 0, 0, 0]
  const raw: number[] = []
  for (let y = 0; y < img.height; y++) {
    raw.push(0) // filter none
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

const seen: { img?: RgbaImage; detectOpts?: { conf: number; iou?: number } } = {}

function fakeRunner(
  lines: Array<{ text: string; score: number; box: { x: number; y: number; w: number; h: number } }>,
  objects: Array<{ label: string; score: number; x: number; y: number; w: number; h: number }> = [],
): CvRunner {
  return {
    ocr: async (img) => {
      seen.img = img
      return { lines, backend: "wasm-cpu" }
    },
    detect: async (img, opts) => {
      seen.img = img
      seen.detectOpts = opts
      return { objects, backend: "wasm-cpu" }
    },
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

describe("detect 工具描述契约", () => {
  test("覆盖元数据自适应 / 分层后端 / iou / pair_text / 定位纪律 / drop-in 约定", () => {
    const d = detectTool.description ?? ""
    expect(d).toContain("GEBAI_CV_DETECT_MODEL")
    expect(d).toContain("models/detect") // 约定目录 drop-in 自动发现
    expect(d).toContain("ultralytics") // 导出 ONNX 即插即用（元数据自适应）
    expect(d).toContain("GEBAI_CV_DETECT_BACKEND") // 分层后端入口
    expect(d).toContain("sidecar")
    expect(d).toContain("pair_text") // 检测×OCR 配对开关
    expect(d).toContain("iou") // NMS 阈值可调（密集 UI 场景）
    expect(d).toContain("desktop_locate") // 找特定文字按钮的定位纪律
  })
})

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

  test("ocr：现截全屏（runCommand 落盘 PNG，固定文件名复用 + 虚拟屏幕 CAP 原点）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    let seenScript = ""
    const c = ctx(home, {
      runCommand: async (cmd) => {
        if (cmd.includes("powershell")) {
          const script = decodeCmd(cmd)
          seenScript = script
          const m = script.match(/'([^']+\.png)'/)
          if (m) await Bun.write(m[1], pngBytes(300, 200))
        }
        return { stdout: "CAP 0,0", stderr: "", code: 0 }
      },
    })
    const r = await ocrTool.execute({}, c)
    // 固定文件名复用（不再 cv_capture_<时间戳> 只增不减）；全屏=虚拟屏幕 + DPI 感知
    expect(seenScript).toContain("cv_capture.png")
    expect(seenScript).not.toMatch(/cv_capture_\d+/)
    expect(seenScript).toContain("VirtualScreen")
    expect(seenScript).toContain("SetProcessDPIAware")
    expect(r.output).toContain("全屏=虚拟屏幕")
    expect(seen.img?.width).toBe(300)
    expect((r.data as { lines: unknown[] }).lines.length).toBe(3)
  })

  test("ocr：现截全屏 CAP 负原点 → 坐标映射回主屏原点（副屏负坐标）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home, {
      runCommand: async (cmd) => {
        if (cmd.includes("powershell")) {
          const m = decodeCmd(cmd).match(/'([^']+\.png)'/)
          if (m) await Bun.write(m[1], pngBytes(300, 200))
        }
        return { stdout: "CAP -2560,0", stderr: "", code: 0 }
      },
    })
    const r = await ocrTool.execute({}, c)
    const data = r.data as { lines: Array<{ x: number; y: number }> }
    // 虚拟屏幕原点 (-2560,0)：PNG 内 (10,20) 的行映射回主屏原点坐标 (-2550, 20)
    expect(data.lines[0].x).toBe(-2550)
    expect(data.lines[0].y).toBe(20)
  })

  test("ocr：现截 + region 不双重裁剪（区域截图直接使用，坐标偏移=区域原点）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home, {
      runCommand: async (cmd) => {
        if (cmd.includes("powershell")) {
          const script = decodeCmd(cmd)
          const m = script.match(/'([^']+\.png)'/)
          // 模拟区域截图：落盘的 PNG 就是区域尺寸（100x60），不应再被裁小
          if (m) await Bun.write(m[1], pngBytes(100, 60))
        }
        return { stdout: "CAP 50,40", stderr: "", code: 0 }
      },
    })
    const r = await ocrTool.execute({ region: "50,40,100,60" }, c)
    expect(seen.img?.width).toBe(100)
    expect(seen.img?.height).toBe(60)
    const data = r.data as { lines: Array<{ x: number; y: number }> }
    expect(data.lines[0].x).toBe(60) // 10 + 50（区域原点偏移）
    expect(data.lines[0].y).toBe(60) // 20 + 40
  })

  test("ocr：region 支持负坐标（副屏区域）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const r = await ocrTool.execute({ image: "shot.png", region: "-100,0,200,100" }, c)
    expect(r.output).toContain("区域 -100,0,200,100")
    const data = r.data as { lines: Array<{ x: number }> }
    expect(data.lines[0].x).toBe(-90) // 10 + (-100)
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

  test("detect：未配置环境变量 → 指引；配置后返回对象（默认配对 OCR 文本 + 后端标注）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    setCvRunnerFactory(() => ({
      ocr: async () => { throw new Error("未配置") },
      detect: async () => { throw new Error("目标检测未配置：需设置 GEBAI_CV_DETECT_MODEL（YOLO ONNX 模型路径）") },
    }))
    let c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    let r = await detectTool.execute({ image: "shot.png" }, c)
    expect(r.output).toContain("GEBAI_CV_DETECT_MODEL")
    // 配置后（fake runner 正常返回；icon_save 框 [12,22,56,20] 与 OCR 行「保存」中心 (40,32) 命中配对）
    setCvRunnerFactory(() => fakeRunner(DEFAULT_LINES, [{ label: "icon_save", score: 0.88, x: 12, y: 22, w: 56, h: 20 }]))
    c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    r = await detectTool.execute({ image: "shot.png" }, c)
    expect(r.output).toContain("icon_save")
    expect(r.output).toContain("1 个对象")
    expect(r.output).toContain("后端 wasm-cpu")
    expect(r.output).toContain("文本: 保存") // 「取消」(130,32)、「保存全部」(55,72) 不在框内
    const data = r.data as { backend: string; objects: Array<{ label: string; x: number; text?: string }> }
    expect(data.backend).toBe("wasm-cpu")
    expect(data.objects[0].label).toBe("icon_save")
    expect(data.objects[0].x).toBe(12)
    expect(data.objects[0].text).toBe("保存")
  })

  test("detect：pair_text=false 跳过配对；conf/iou 透传（非法值取缺省）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    let r = await detectTool.execute({ image: "shot.png", pair_text: false, conf: 0.1, iou: 0.15 }, c)
    expect(r.output).not.toContain("文本:")
    expect(seen.detectOpts?.conf).toBe(0.1)
    expect(seen.detectOpts?.iou).toBe(0.15)
    r = await detectTool.execute({ image: "shot.png", conf: 2, iou: -1 }, c)
    expect(seen.detectOpts?.conf).toBe(0.25)
    expect(seen.detectOpts?.iou).toBeUndefined()
  })

  test("detect：OCR 不可用时跳过配对不报错（仅检测框）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    setCvRunnerFactory(() => ({
      ocr: async () => { throw new Error("本地识别模型未配置：请设置 GEBAI_CV_MODELS_DIR") },
      detect: async () => ({ objects: [{ label: "icon", score: 0.9, x: 0, y: 0, w: 10, h: 10 }], backend: "sidecar:dml" }),
    }))
    try {
      const c = ctx(home)
      await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
      const r = await detectTool.execute({ image: "shot.png" }, c)
      expect(r.output).toContain("icon")
      expect(r.output).not.toContain("文本:")
      expect(r.output).toContain("sidecar:dml")
    } finally {
      installDefaultFake()
    }
  })

  test("locate_image：template PNG 路径定位图标，返回中心坐标", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home)
    const patch = makePatch(24, 99)
    await Bun.write(join(c.workdir!, "shot.png"), encodeRgba(makeSearchWithPatch(200, 100, patch, 60, 40)))
    await Bun.write(join(c.workdir!, "icon.png"), encodeRgba(patch))
    const r = await locateImageTool.execute({ template: "icon.png", image: "shot.png" }, c)
    expect(r.output).toContain("中心 (72,52)") // 60+12, 40+12
    expect(r.output).toContain("mouse_click(72, 52)")
    const data = r.data as { found: boolean; best: { center: number[]; score: number } }
    expect(data.found).toBe(true)
    expect(data.best.center).toEqual([72, 52])
    expect(data.best.score).toBeGreaterThan(0.9)
  })

  test("locate_image：template_region 在搜索图内取模板 + region 偏移映射", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home)
    const patch = makePatch(24, 55)
    // patch 贴在全图 (60,40)；搜索 region "50,0,150,100" 裁剪后 patch 在裁剪系 (10,40)
    await Bun.write(join(c.workdir!, "shot.png"), encodeRgba(makeSearchWithPatch(200, 100, patch, 60, 40)))
    const r = await locateImageTool.execute({ template_region: "10,40,24,24", image: "shot.png", region: "50,0,150,100" }, c)
    expect(r.output).toContain("中心 (72,52)") // 10+12+50（region 原点偏移）, 40+12
    const data = r.data as { found: boolean }
    expect(data.found).toBe(true)
  })

  test("locate_image：模板与 template_region 二选一必填；越界报错；未找到给建议", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), encodeRgba(makeSearchWithPatch(200, 100, makePatch(24, 1), 60, 40)))
    let r = await locateImageTool.execute({ image: "shot.png" }, c)
    expect(r.output).toContain("二者之一")
    r = await locateImageTool.execute({ template_region: "180,80,24,24", image: "shot.png" }, c)
    expect(r.output).toContain("超出搜索图范围")
    // 不存在的模板：独立随机 patch（与搜索图内容无关）→ 未找到给建议
    await Bun.write(join(c.workdir!, "absent.png"), encodeRgba(makePatch(24, 777)))
    r = await locateImageTool.execute({ template: "absent.png", image: "shot.png" }, c)
    expect(r.output).toContain("未找到")
    expect(r.output).toContain("desktop_locate")
  })

  test("wait_for：text 模式轮询至文字出现", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    let calls = 0
    setCvRunnerFactory(() => ({
      ocr: async () => {
        calls++
        return {
          lines: calls <= 1
            ? [{ text: "加载中", score: 0.9, box: { x: 0, y: 0, w: 50, h: 10 } }]
            : [{ text: "完成", score: 0.9, box: { x: 0, y: 0, w: 50, h: 10 } }],
          backend: "wasm-cpu",
        }
      },
      detect: async () => ({ objects: [], backend: "wasm-cpu" }),
    }))
    try {
      const c = ctx(home, {
        runCommand: async (cmd) => {
          if (cmd.includes("powershell")) {
            const m = decodeCmd(cmd).match(/'([^']+\.png)'/)
            if (m) await Bun.write(m[1], pngBytes(300, 200))
          }
          return { stdout: "CAP 0,0", stderr: "", code: 0 }
        },
      })
      const r = await waitForTool.execute({ mode: "text", text: "完成", interval_s: 0.5, timeout_s: 10 }, c)
      expect(r.output).toContain("已满足")
      expect(r.output).toContain("「完成」")
      expect(r.output).toContain("次轮询")
    } finally {
      installDefaultFake()
    }
  })

  test("wait_for：text_gone 等待文字消失；缺 text 报错", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    let calls = 0
    setCvRunnerFactory(() => ({
      ocr: async () => {
        calls++
        return { lines: calls <= 1 ? [{ text: "保存中", score: 0.9, box: { x: 0, y: 0, w: 50, h: 10 } }] : [], backend: "wasm-cpu" }
      },
      detect: async () => ({ objects: [], backend: "wasm-cpu" }),
    }))
    try {
      const c = ctx(home, {
        runCommand: async (cmd) => {
          if (cmd.includes("powershell")) {
            const m = decodeCmd(cmd).match(/'([^']+\.png)'/)
            if (m) await Bun.write(m[1], pngBytes(300, 200))
          }
          return { stdout: "CAP 0,0", stderr: "", code: 0 }
        },
      })
      const r = await waitForTool.execute({ mode: "text_gone", text: "保存中", interval_s: 0.5, timeout_s: 10 }, c)
      expect(r.output).toContain("已消失")
      const bad = await waitForTool.execute({ mode: "text" }, ctx(home))
      expect(bad.output).toContain("text 参数")
    } finally {
      installDefaultFake()
    }
  })

  test("wait_for：超时返回最后观察状态（不视为错误）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    setCvRunnerFactory(() => fakeRunner([{ text: "加载中", score: 0.9, box: { x: 0, y: 0, w: 50, h: 10 } }]))
    try {
      const c = ctx(home, {
        runCommand: async (cmd) => {
          if (cmd.includes("powershell")) {
            const m = decodeCmd(cmd).match(/'([^']+\.png)'/)
            if (m) await Bun.write(m[1], pngBytes(300, 200))
          }
          return { stdout: "CAP 0,0", stderr: "", code: 0 }
        },
      })
      const r = await waitForTool.execute({ mode: "text", text: "完成", interval_s: 0.5, timeout_s: 1 }, c)
      expect(r.output).toContain("等待超时")
      expect(r.output).toContain("不在当前画面中")
    } finally {
      installDefaultFake()
    }
  })

  test("wait_for：change 模式检测画面变化（灰度采样差）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-dcv-"))
    let captures = 0
    const c = ctx(home, {
      runCommand: async (cmd) => {
        if (cmd.includes("powershell")) {
          const m = decodeCmd(cmd).match(/'([^']+\.png)'/)
          if (m) await Bun.write(m[1], pngBytes(300, 200, ++captures <= 2 ? 120 : 200))
        }
        return { stdout: "CAP 0,0", stderr: "", code: 0 }
      },
    })
    const r = await waitForTool.execute({ mode: "change", interval_s: 0.5, timeout_s: 10 }, c)
    expect(r.output).toContain("已满足")
    expect(r.output).toContain("画面已变化")
  })
})
