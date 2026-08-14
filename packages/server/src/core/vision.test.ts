import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname, resolve } from "node:path"
import { makeVisionTool, collectChatText, VISION_MAX_IMAGE_BYTES } from "./vision"
import type { LLMChunk, LLMProvider } from "./llm"
import type { ToolContext } from "./types"

class FakeVisionProvider implements LLMProvider {
  readonly id = "fake-vision"
  seen: Array<{ messages: Array<{ role: string; content: unknown }> }> = []
  constructor(private reply: () => AsyncIterable<LLMChunk> | Error) {}
  capabilities() {
    return { streaming: true, toolCalling: true, multimodal: true, maxContextTokens: 10000 }
  }
  async *chat(messages: unknown[]) {
    this.seen.push({ messages: messages as Array<{ role: string; content: unknown }> })
    const r = this.reply()
    if (r instanceof Error) throw r
    yield* r
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
    waitForCapture: async () => null,
  }
}

async function writeImage(home: string, name: string, bytes: Uint8Array): Promise<string> {
  const p = join(home, "users", "default", "sessions", "0123456789abcdef0123456789abcdef", "tmp", name)
  const { mkdir, writeFile } = await import("node:fs/promises")
  await mkdir(dirname(p), { recursive: true })
  await writeFile(p, bytes)
  return p
}

describe("vision tool", () => {
  test("发送 target + 图片（base64 内联）给视觉模型并返回分析文本", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    const png = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x01, 0x02])
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
    expect(content[0]).toEqual({ type: "text", text: "描述图片内容" })
    expect(content[1]).toEqual({ type: "image", mime: "image/png", data: Buffer.from(png).toString("base64") })
    rmSync(home, { recursive: true, force: true })
  })

  test("provider getter 收到任务级 env（前端/会话配置的视觉模型可生效）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    const png = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x01, 0x02])
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
    await writeImage(home, "a.png", new Uint8Array([1]))
    const fake = new FakeVisionProvider(() => new Error("模型接口超时"))
    const tool = makeVisionTool({ vision: () => fake })
    await expect(tool.execute({ target: "看", image: "a.png" }, ctx(home))).rejects.toThrow(/模型接口超时/)
    rmSync(home, { recursive: true, force: true })
  })

  test("模型无输出时抛中文错误", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    await writeImage(home, "a.png", new Uint8Array([1]))
    const fake = new FakeVisionProvider(() => ({
      async *[Symbol.asyncIterator]() {
        yield { type: "done", stopReason: "stop" }
      },
    }))
    const tool = makeVisionTool({ vision: () => fake })
    await expect(tool.execute({ target: "看", image: "a.png" }, ctx(home))).rejects.toThrow(/未返回任何内容/)
    rmSync(home, { recursive: true, force: true })
  })

  test("长输出走截断保护", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-vision-"))
    await writeImage(home, "a.png", new Uint8Array([1]))
    const fake = new FakeVisionProvider(() => ({
      async *[Symbol.asyncIterator]() {
        yield { type: "text", text: "答".repeat(15000) }
        yield { type: "done", stopReason: "stop" }
      },
    }))
    const tool = makeVisionTool({ vision: () => fake })
    const r = await tool.execute({ target: "看", image: "a.png" }, ctx(home))
    expect(r.truncated).toBe(true)
    expect(r.filePath).toMatch(/^tmp\/truncated\/vision_[0-9a-f]{64}\.txt$/)
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
