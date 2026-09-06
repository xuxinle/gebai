import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, mkdirSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname } from "node:path"
import type { ToolContext } from "../../core/base/types"
import type { RgbaImage } from "../../core/cv/image"
import { setCvRunnerFactory, type CvRunner } from "../../core/cv/cv"
import { setVisionProviderGetter } from "../../core/tools/vision"
import { def } from "./vision"

afterAll(() => {
  setCvRunnerFactory(null)
})

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

function fakeRunner(
  lines: Array<{ text: string; score: number; box: { x: number; y: number; w: number; h: number } }>,
  objects: Array<{ label: string; score: number; x: number; y: number; w: number; h: number }> = [],
): CvRunner {
  return {
    ocr: async () => ({ lines, backend: "wasm-cpu" }),
    detect: async () => ({ objects, backend: "wasm-cpu" }),
  }
}

/** 灰度 PNG 字节（微型编码器：filter 0，CRC 写零）。 */
function pngBytes(w: number, h: number, gray = 120): Uint8Array {
  const { deflateSync } = require("node:zlib") as typeof import("node:zlib")
  const chunk = (type: string, data: number[]): number[] => {
    const len = [(data.length >>> 24) & 255, (data.length >>> 16) & 255, (data.length >>> 8) & 255, data.length & 255]
    return [...len, ...Array.from(type, (c) => c.charCodeAt(0)), ...data, 0, 0, 0, 0]
  }
  const be32 = (v: number) => [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255]
  const img: RgbaImage = { width: w, height: h, data: new Uint8ClampedArray(w * h * 4).map((_v, i) => ((i & 3) === 3 ? 255 : gray)) }
  const ihdr = [...be32(w), ...be32(h), 8, 6, 0, 0, 0]
  const raw: number[] = []
  for (let y = 0; y < h; y++) {
    raw.push(0)
    for (let x = 0; x < w; x++) {
      const o = (y * w + x) * 4
      raw.push(img.data[o], img.data[o + 1], img.data[o + 2], img.data[o + 3])
    }
  }
  const idat = Array.from(deflateSync(Buffer.from(raw)))
  return new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10, ...chunk("IHDR", ihdr), ...chunk("IDAT", idat), ...chunk("IEND", [])])
}

const DEFAULT_LINES = [
  { text: "保存", score: 0.95, box: { x: 10, y: 20, w: 60, h: 24 } },
  { text: "取消", score: 0.9, box: { x: 100, y: 20, w: 60, h: 24 } },
]
const DEFAULT_OBJECTS = [{ label: "icon_save", score: 0.88, x: 12, y: 22, w: 56, h: 20 }]

beforeAll(() => {
  setCvRunnerFactory(() => fakeRunner(DEFAULT_LINES, DEFAULT_OBJECTS))
})

describe("vision 子代理 def 契约（视觉能力收拢与复用边界，DESIGN「视觉能力分层与子代理复用边界」）", () => {
  test("工具集：analyze + ocr/locate/locate_image + detect（识别实现复用共享工厂）", () => {
    expect(Object.keys(def.tools ?? {}).sort()).toEqual(["analyze", "detect", "locate", "locate_image", "ocr"].sort())
    expect(def.name).toBe("vision")
    expect(def.dependencies).toBeUndefined() // 零依赖（被依赖方）
    expect(def.requiresApproval).toEqual({}) // 全只读免审批
    expect(def.preload).toBe(false)
  })

  test("系统提示词：硬性决策序（OCR 先行禁用 analyze 回答可 OCR 问题）与被依赖方职责（不复述工具 schema 细节）", () => {
    const p = def.systemPrompt
    expect(p).toContain("禁止用 analyze 回答 ocr 能答的问题")
    expect(p).toContain("被依赖方")
    expect(p).toContain("图片像素系")
    // 提示词不重复工具 schema 已有的参数级细节（PNG/threshold 等——模型可见工具描述）
    expect(p).not.toContain("threshold")
    expect(p).not.toContain("## 工具用法")
  })

  test("无缺省图像源：ocr/locate/detect 缺 image 引导（省略不截图）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vis-"))
    const c = ctx(home)
    const r = await def.tools!.ocr.execute({}, c)
    expect(r.output).toContain("image")
    const d = await def.tools!.detect.execute({}, c)
    expect(d.output).toContain("image")
  })

  test("ocr：读 PNG 输出行+图片像素坐标；沙箱可用（无 desktop 本地模式闸门）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vis-"))
    const c = ctx(home, { sandboxed: true })
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const r = await def.tools!.ocr.execute({ image: "shot.png" }, c)
    expect(r.output).toContain("保存")
    expect(r.output).toContain("中心 (40,32)")
    const data = r.data as { lines: Array<{ x: number }> }
    expect(data.lines[0].x).toBe(10)
  })

  test("ocr：region 裁剪坐标映射回图片像素系；find 过滤", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vis-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const r = await def.tools!.ocr.execute({ image: "shot.png", region: "50,40,100,60" }, c)
    const data = r.data as { lines: Array<{ x: number; y: number }> }
    expect(data.lines[0].x).toBe(60)
    expect(data.lines[0].y).toBe(60)
    const f = await def.tools!.ocr.execute({ image: "shot.png", find: "取消" }, c)
    expect(f.output).toContain("取消")
    expect(f.output).not.toContain("保存\n")
  })

  test("locate：定位文字中心坐标（图片像素系），未找到引导 vision_analyze", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vis-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const r = await def.tools!.locate.execute({ target: "保存", image: "shot.png" }, c)
    expect(r.output).toContain("中心 (40,32)")
    const data = r.data as { found: boolean; best: { center: number[] } }
    expect(data.found).toBe(true)
    expect(data.best.center).toEqual([40, 32])
    const miss = await def.tools!.locate.execute({ target: "不存在", image: "shot.png" }, c)
    expect(miss.output).toContain("未找到")
    expect(miss.output).toContain("vision_ocr")
    expect(miss.output).toContain("vision_analyze")
  })

  test("detect：image 必填；返回对象+配对文本+后端", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vis-"))
    const c = ctx(home)
    await Bun.write(join(c.workdir!, "shot.png"), pngBytes(200, 100))
    const r = await def.tools!.detect.execute({ image: "shot.png" }, c)
    expect(r.output).toContain("icon_save")
    expect(r.output).toContain("后端 wasm-cpu")
    expect(r.output).toContain("文本: 保存")
    const params = def.tools!.detect.parameters as { required?: string[] }
    expect(params.required).toContain("image")
  })

  test("analyze：无视觉 provider 时给出配置指引（复用全局同源实现，provider 检查先行与全局一致）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vis-"))
    // 模块级 vision provider 可能被其他测试（如集成启动真实服务）注册污染：本用例语义即「未配置时降级」，
    // 显式清空后断言（与 self_optimize.test.ts 同款处理），测毕在 afterAll 统一还原
    const saved = (def.tools!.analyze as { name: string })
    setVisionProviderGetter(null)
    try {
      const c = ctx(home)
      const r = await def.tools!.analyze.execute({ target: "图里有什么", image: "shot.png" }, c)
      expect(r.output).toContain("视觉能力不可用")
      expect(r.output).toContain("GEBAI_VISION_MODEL")
      expect(saved.name).toBe("analyze") // 注册后即 vision_analyze（命名空间隔离）
    } finally {
      setVisionProviderGetter(null)
    }
  })

  test("非 PNG 明确报错（识别仅 PNG；analyze 支持更多格式）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vis-"))
    const c = ctx(home)
    const r = await def.tools!.ocr.execute({ image: "photo.jpg" }, c)
    expect(r.output).toContain("PNG")
  })
})

describe("依赖复用（方式一：dependencies 声明——self_optimize 依赖 vision）", () => {
  test("self_optimize def 声明依赖 vision（agent_run 新会话级联预加载，截图分析不依赖全局 vision 继承）", async () => {
    const { def: selfOptDef } = await import("../self_optimize")
    expect(selfOptDef.dependencies).toContain("code")
    expect(selfOptDef.dependencies).toContain("vision")
  })
})
