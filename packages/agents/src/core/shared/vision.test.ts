import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname, resolve } from "node:path"
import { makeVisionTool, collectChatText, VISION_MAX_IMAGE_BYTES, VISION_ANALYZE_TIMEOUT_SEC, analyzeTimeoutNote } from "./vision"
import type { LLMChunk, VisionLLMProvider as LLMProvider } from "./vision"
import type { ToolContext } from "@gebai/sdk"

class FakeVisionProvider implements LLMProvider {
  readonly id = "fake-vision"
  seen: Array<{ messages: Array<{ role: string; content: unknown }>; opts?: unknown }> = []
  constructor(private reply: (signal?: AbortSignal) => AsyncIterable<LLMChunk> | Error) {}
  capabilities() {
    return { streaming: true, toolCalling: true, multimodal: true, maxContextTokens: 10000 }
  }
  async *chat(messages: unknown[], opts?: { signal?: AbortSignal }) {
    this.seen.push({ messages: messages as Array<{ role: string; content: unknown }>, opts })
    const signal = opts?.signal
    const r = this.reply(signal)
    if (r instanceof Error) throw r
    // 尊重中止信号（与真实 provider 一致）：abort 时以 signal.reason 拒绝，打断挂起的生成器
    if (!signal) {
      yield* r
      return
    }
    const onAbort = new Promise<never>((_, rej) => {
      if (signal.aborted) rej(signal.reason)
      else signal.addEventListener("abort", () => rej(signal.reason), { once: true })
    })
    const iter = r[Symbol.asyncIterator]()
    while (true) {
      const next = await Promise.race([iter.next(), onAbort])
      if (next.done) return
      yield next.value
    }
  }
}

function ctx(home: string): ToolContext {
  const base = home
  const sid = "0123456789abcdef0123456789abcdef" // 合法会话 id（32 位 hex）
  const tmp = join(base, "users", "default", "sessions", sid, "tmp")
  mkdirSync(tmp, { recursive: true })
  return {
    user: "default",
    sessionId: sid,
    workdir: tmp,
    home: base,
    env: {},
    sandboxed: false,
    resolvePath: (p) => resolve(tmp, p),
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
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
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
  }
}

async function writeImage(home: string, name: string, bytes: Uint8Array): Promise<string> {
  const p = join(home, "users", "default", "sessions", "0123456789abcdef0123456789abcdef", "tmp", name)
  const { mkdir, writeFile } = await import("node:fs/promises")
  await mkdir(dirname(p), { recursive: true })
  await writeFile(p, bytes)
  return p
}

/** 构造最小合法 PNG（宽高可指定，rgb 像素色，zlib 存储）——与 image-resize.test 同构。 */
function makePng(width: number, height: number, rgb: [number, number, number] = [200, 60, 60]): Buffer {
  const { deflateSync } = require("node:zlib")
  const crc32 = (buf: Buffer) => {
    let c = ~0
    for (const b of buf) {
      c ^= b
      for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1))
    }
    return ~c >>> 0
  }
  const chunk = (type: string, data: Buffer) => {
    const len = Buffer.alloc(4)
    len.writeUInt32BE(data.length)
    const body = Buffer.concat([Buffer.from(type, "ascii"), data])
    const crc = Buffer.alloc(4)
    crc.writeUInt32BE(crc32(body))
    return Buffer.concat([len, body, crc])
  }
  const ihdr = Buffer.alloc(13)
  ihdr.writeUInt32BE(width, 0)
  ihdr.writeUInt32BE(height, 4)
  ihdr[8] = 8
  ihdr[9] = 2
  const raw = Buffer.alloc((width * 3 + 1) * height)
  for (let y = 0; y < height; y++) {
    const off = y * (width * 3 + 1) + 1
    for (let x = 0; x < width; x++) {
      raw[off + x * 3] = rgb[0]
      raw[off + x * 3 + 1] = rgb[1]
      raw[off + x * 3 + 2] = rgb[2]
    }
  }
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw)),
    chunk("IEND", Buffer.alloc(0)),
  ])
}

describe("vision tool", () => {
  test("发送 target + 图片（base64 内联）给视觉模型并返回分析文本", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    // 1x1 红色像素 PNG：未超压缩阈值，resized=false 原样发送，附尺寸说明
    const png = makePng(1, 1, [255, 0, 0])
    await writeImage(home, "shot.png", png)
    const fake = new FakeVisionProvider(() => ({
      async *[Symbol.asyncIterator]() {
        yield { type: "text", text: "图中有一只猫。" }
        yield { type: "done", stopReason: "stop" }
      },
    }))
    const tool = makeVisionTool({ vision: () => fake })
    const r = await tool.execute({ target: "描述图片内容", image: "shot.png" }, ctx(home))
    expect(r.output).toContain("图中有一只猫")
    expect(r.blocks).toEqual([{ type: "image", path: "shot.png", mime: "image/png" }])
    const content = fake.seen[0].messages[0].content as Array<Record<string, unknown>>
    // 尺寸说明附在 target 文本尾部（告知模型图片原始尺寸）
    expect(content[0]).toMatchObject({ type: "text" })
    expect(String((content[0] as { text: string }).text)).toContain("描述图片内容")
    expect(String((content[0] as { text: string }).text)).toContain("1×1")
    expect(String((content[0] as { text: string }).text)).toContain("未压缩")
    expect(content[1]).toEqual({ type: "image", mime: "image/png", data: Buffer.from(png).toString("base64") })
    rmSync(home, { recursive: true, force: true })
  })

  test("provider getter 收到任务级 env（前端/会话配置的视觉模型可生效）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    const png = makePng(1, 1)
    await writeImage(home, "shot.png", png)
    const fake = new FakeVisionProvider(() => ({
      async *[Symbol.asyncIterator]() {
        yield { type: "text", text: "ok" }
        yield { type: "done", stopReason: "stop" }
      },
    }))
    let getterEnv: Record<string, string> | undefined
    const tool = makeVisionTool({
      vision: (env) => {
        getterEnv = env
        return fake
      },
    })
    const c = ctx(home)
    c.env = { GEBAI_VISION_MODEL: "gpt-vision" }
    await tool.execute({ target: "看", image: "shot.png" }, c)
    expect(getterEnv).toEqual({ GEBAI_VISION_MODEL: "gpt-vision" })
    rmSync(home, { recursive: true, force: true })
  })

  test("未配置视觉模型时返回配置提示", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    const tool = makeVisionTool({ vision: () => null })
    const r = await tool.execute({ target: "看", image: "x.png" }, ctx(home))
    expect(r.output).toContain("GEBAI_VISION")
    rmSync(home, { recursive: true, force: true })
  })

  test("不支持的图片格式报错", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    const fake = new FakeVisionProvider(() => new Error("should not be called"))
    const tool = makeVisionTool({ vision: () => fake })
    await writeImage(home, "doc.pdf", new Uint8Array([0x25, 0x50, 0x44, 0x46]))
    const r = await tool.execute({ target: "看", image: "doc.pdf" }, ctx(home))
    expect(r.output).toContain("不支持的图片格式")
    expect(fake.seen.length).toBe(0)
    rmSync(home, { recursive: true, force: true })
  })

  test("图片超过大小上限时拒绝发送", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    const fake = new FakeVisionProvider(() => new Error("should not be called"))
    const tool = makeVisionTool({ vision: () => fake })
    await writeImage(home, "big.jpg", new Uint8Array(VISION_MAX_IMAGE_BYTES + 1))
    const r = await tool.execute({ target: "看", image: "big.jpg" }, ctx(home))
    expect(r.output).toContain("图片过大")
    expect(fake.seen.length).toBe(0)
    rmSync(home, { recursive: true, force: true })
  })

  test("图片文件不存在时报错", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    const fake = new FakeVisionProvider(() => new Error("should not be called"))
    const tool = makeVisionTool({ vision: () => fake })
    await expect(tool.execute({ target: "看", image: "missing.png" }, ctx(home))).rejects.toThrow()
    rmSync(home, { recursive: true, force: true })
  })

  test("模型调用异常原样上抛", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    await writeImage(home, "a.png", makePng(2, 2))
    const fake = new FakeVisionProvider(() => new Error("模型接口超时"))
    const tool = makeVisionTool({ vision: () => fake })
    await expect(tool.execute({ target: "看", image: "a.png" }, ctx(home) as never)).rejects.toThrow(/模型接口超时/)
    rmSync(home, { recursive: true, force: true })
  })

  test("模型无输出时抛中文错误", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    await writeImage(home, "a.png", makePng(2, 2))
    const fake = new FakeVisionProvider(() => ({
      async *[Symbol.asyncIterator]() {
        yield { type: "done", stopReason: "stop" }
      },
    }))
    const tool = makeVisionTool({ vision: () => fake })
    await expect(tool.execute({ target: "看", image: "a.png" }, ctx(home) as never)).rejects.toThrow(/未返回任何内容/)
    rmSync(home, { recursive: true, force: true })
  })

  test("长输出走截断保护", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    await writeImage(home, "a.png", makePng(2, 2))
    const fake = new FakeVisionProvider(() => ({
      async *[Symbol.asyncIterator]() {
        yield { type: "text", text: "答".repeat(15000) }
        yield { type: "done", stopReason: "stop" }
      },
    }))
    const tool = makeVisionTool({ vision: () => fake })
    const r = await tool.execute({ target: "看", image: "a.png" }, ctx(home) as never)
    expect(r.truncated).toBe(true)
    expect(r.filePath).toMatch(/^tmp\/truncated\/vision_[0-9a-f]{64}\.txt$/)
    rmSync(home, { recursive: true, force: true })
  })

  test("超时：模型挂起不返回时返回超时提示（引导本地视觉工具）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    await writeImage(home, "a.png", makePng(2, 2))
    // 挂起不返回的 provider；AbortSignal.timeout 触发后 fake 随 signal 打断（真实 provider 同理）
    const fake = new FakeVisionProvider(() =>
      (async function* () {
        await new Promise(() => {})
      })(),
    )
    const tool = makeVisionTool({ vision: () => fake })
    // 显式传短时限验证超时链路（默认 30 秒由 parseTimeout 单测覆盖，避免真等）
    const r = await tool.execute({ target: "看", image: "a.png", timeout: 1 }, ctx(home))
    expect(r.output).toContain("视觉分析超时（1 秒")
    expect(r.output).toContain("vision_ocr")
    expect(r.output).toContain("vision_locate")
    expect(fake.seen.length).toBe(1)
    // 超时信号透传给了 provider
    expect((fake.seen[0].opts as { signal?: AbortSignal } | undefined)?.signal instanceof AbortSignal).toBe(true)
    rmSync(home, { recursive: true, force: true })
  })

  test("timeout 超出钳制范围时被钳到 1~300（默认 30）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    await writeImage(home, "a.png", makePng(2, 2))
    const fake = new FakeVisionProvider(() =>
      (async function* () {
        await new Promise(() => {})
      })(),
    )
    const tool = makeVisionTool({ vision: () => fake })
    // 0.2 秒被钳到最小 1 秒——仍会超时返回提示
    const r = await tool.execute({ target: "看", image: "a.png", timeout: 0.2 }, ctx(home))
    expect(r.output).toContain("视觉分析超时（1 秒")
    rmSync(home, { recursive: true, force: true })
  })

  test("超限大图发送前压缩：模型收到压缩后图片与原始/压缩尺寸说明", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    const bigPng = makePng(2000, 1000, [180, 40, 40])
    await writeImage(home, "big.png", bigPng)
    const fake = new FakeVisionProvider(() => ({
      async *[Symbol.asyncIterator]() {
        yield { type: "text", text: "ok" }
        yield { type: "done", stopReason: "stop" }
      },
    }))
    const tool = makeVisionTool({ vision: () => fake })
    const r = await tool.execute({ target: "看", image: "big.png" }, ctx(home) as never)
    expect(r.output).toContain("ok")
    const content = fake.seen[0].messages[0].content as Array<Record<string, unknown>>
    const text = String((content[0] as { text: string }).text)
    expect(text).toContain("原始 2000×1000")
    expect(text).toContain("→ 1280×640")
    const imgBlock = content[1] as { data: string }
    expect(imgBlock.data).not.toBe(bigPng.toString("base64")) // 发送的是压缩后图片
    expect(Buffer.from(imgBlock.data, "base64").length).toBeLessThan(bigPng.length)
    rmSync(home, { recursive: true, force: true })
  })
})

describe("collectChatText", () => {
  test("仅收集 text chunk 并按序拼接", async () => {
    const out = await collectChatText(
      (async function* () {
        yield { type: "text", text: "你好" }
        yield { type: "reasoning", text: "（思考中）" }
        yield { type: "text", text: "，世界" }
        yield { type: "done", stopReason: "stop" }
      })(),
    )
    expect(out).toBe("你好，世界")
  })
})

describe("parseTimeout", () => {
  test("缺省/非法值回落默认 30 秒，合法值钳制在 1~300", async () => {
    const { parseTimeout } = await import("./vision")
    expect(parseTimeout(undefined)).toBe(VISION_ANALYZE_TIMEOUT_SEC)
    expect(parseTimeout(null)).toBe(VISION_ANALYZE_TIMEOUT_SEC)
    expect(parseTimeout("abc")).toBe(VISION_ANALYZE_TIMEOUT_SEC)
    expect(parseTimeout(0.2)).toBe(1)
    expect(parseTimeout(1)).toBe(1)
    expect(parseTimeout(120)).toBe(120)
    expect(parseTimeout(99999)).toBe(300)
    expect(analyzeTimeoutNote(30)).toContain("视觉分析超时（30 秒")
    expect(analyzeTimeoutNote(30)).toContain("vision_ocr")
  })
})
