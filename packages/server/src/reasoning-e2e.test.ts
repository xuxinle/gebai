import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { mkdtempSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { startServer, type ServerHandle } from "./index"
import { GebaiClient } from "@gebai/sdk"
import type { LLMProvider, LLMChunk, ChatOptions } from "./core/llm/llm"
import type { LLMCapabilities, MessageLike } from "@gebai/sdk"

class ReasoningFake implements LLMProvider {
  readonly id = "fake"
  calls = 0
  capabilities(): LLMCapabilities {
    return { streaming: true, toolCalling: true, multimodal: false, maxContextTokens: 1000 }
  }
  async *chat(_msgs: MessageLike[], _opts?: ChatOptions): AsyncIterable<LLMChunk> {
    this.calls++
    if (this.calls === 1) {
      yield { type: "reasoning", text: "这是推理内容" }
      yield { type: "text", text: "这是正文" }
      yield { type: "done" }
      return
    }
    yield { type: "text", text: "done" }
    yield { type: "done" }
  }
}

const home = mkdtempSync(joinTmp())
function joinTmp() {
  return `${tmpdir()}/gebai-reasoning-${Date.now()}`
}
let handle: ServerHandle

beforeAll(async () => {
  handle = await startServer({ gebaiHome: home, auth: "local", sandbox: "off", binaryMode: false, preloadSubAgents: [], port: 0 })
  ;(handle.engine as unknown as { opts: { provider: LLMProvider } }).opts.provider = new ReasoningFake()
})
afterAll(() => {
  handle.gc?.stop()
  handle.server.stop(true)
  rmSync(home, { recursive: true, force: true })
})

describe("reasoning through WS channel", () => {
  test("SDK sendPrompt yields reasoning chunk from event.message.reasoning", async () => {
    const s = (await (
      await fetch(`http://127.0.0.1:${handle.server.port}/api/v1/sessions`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({}),
      })
    ).json()) as { id: string }

    const client = new GebaiClient({ baseUrl: `http://127.0.0.1:${handle.server.port}` })
    const kinds: string[] = []
    const texts: string[] = []
    for await (const chunk of client.sendPrompt(s.id, "hi")) {
      kinds.push(chunk.kind)
      if (chunk.kind === "text") texts.push(chunk.text ?? "")
      if (chunk.kind === "reasoning") texts.push(`[推理]${chunk.text}`)
    }
    console.log("KINDS:", kinds.join(","))
    console.log("TEXTS:", texts.join("|"))
    expect(kinds).toContain("reasoning")
    expect(texts.join("|")).toContain("[推理]这是推理内容")
  })
})
