import { describe, expect, test } from "bun:test"
import { createPlantUmlRenderer, type PlantUmlRenderer } from "./plantuml"

/** 真实引擎渲染（离线：viz WASM 内嵌 + TeaVM 引擎本地执行，零网络）。 */
const renderer = createPlantUmlRenderer()

/** 假引擎渲染器（bot 等模块的注入替身；不触发真实引擎加载）。 */
export function fakeRenderer(impl: { ok?: (svg: string) => string; error?: string; delayMs?: number } = {}): PlantUmlRenderer {
  return {
    renderPng: async (code) => {
      if (impl.error) throw new Error(impl.error)
      if (impl.delayMs) await new Promise((r) => setTimeout(r, impl.delayMs))
      const svg = impl.ok ? impl.ok(code) : `<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"/>`
      return new TextEncoder().encode(svg)
    },
  }
}

describe("createPlantUmlRenderer", () => {
  test("渲染简单时序图为 PNG（真实引擎 + resvg，零网络）", async () => {
    const png = await renderer.renderPng("@startuml\nAlice -> Bob: hello\n@enduml")
    expect(png.length).toBeGreaterThan(500)
    // PNG 魔数
    expect(png[0]).toBe(0x89)
    expect(png[1]).toBe(0x50)
    expect(png[2]).toBe(0x4e)
    expect(png[3]).toBe(0x47)
  }, 30000)

  test("语法错误抛错并携带可读原因", async () => {
    const p = createPlantUmlRenderer()
    let msg = ""
    try {
      await p.renderPng("@startuml\nAlice -> Bob: hello\n!!!\n@enduml")
    } catch (err) {
      msg = String((err as Error).message)
    }
    expect(msg).toContain("PlantUML 渲染错误")
    expect(msg.length).toBeGreaterThan(0)
  }, 30000)

  test("无需 @startuml 包裹自动补全（fake 引擎验证 normalize，避免真实引擎重复慢渲染）", async () => {
    const calls: string[] = []
    const p = createPlantUmlRenderer({
      engine: async () => ({
        renderToString: (lines, ok) => {
          calls.push(lines.join("\n"))
          ok(`<svg width="10" height="10"/>`)
        },
      }),
      rasterize: async (svg) => new TextEncoder().encode(svg),
    })
    const png = await p.renderPng("Alice -> Bob: hi")
    expect(png.length).toBeGreaterThan(0)
    // 补全 @startuml 包裹与浅色主题
    expect(calls[0]).toContain("@startuml")
    expect(calls[0]).toContain("@enduml")
  }, 5000)

  test("rasterize 注入：SVG 栅格化可替换（fake 引擎）", async () => {
    const calls: string[] = []
    const r = createPlantUmlRenderer({
      engine: async () => ({
        renderToString: (lines, ok) => ok(`<svg width="100" height="50">${lines.join(",")}</svg>`),
      }),
      rasterize: async (svg, o) => {
        calls.push(svg)
        return new TextEncoder().encode(`PNG:${o.background ?? "none"}`)
      },
    })
    const out = await r.renderPng("A -> B")
    expect(new TextDecoder().decode(out)).toContain("PNG:#ffffff")
    expect(calls.length).toBe(1)
    expect(calls[0]).toContain("@startuml")
  })

  test("渲染错误页 SVG（引擎不回调 onError 时）→ 提取错误文本", async () => {
    const r = createPlantUmlRenderer({
      engine: async () => ({
        renderToString: (_lines, ok) =>
          ok(`<svg><text>PlantUML Syntax Error</text><text>Bad line 3</text><text>Some diagram description contains errors</text></svg>`),
      }),
      rasterize: async (svg) => new TextEncoder().encode(svg),
    })
    let msg = ""
    try {
      await r.renderPng("bad code")
    } catch (err) {
      msg = String((err as Error).message)
    }
    expect(msg).toContain("PlantUML 渲染错误")
    expect(msg).toMatch(/error|line 3/i)
  })

  test("onError 回调错误原样透传", async () => {
    const r = createPlantUmlRenderer({
      engine: async () => ({
        renderToString: (_lines, _ok, onError) => onError("unknown type @startfoo"),
      }),
      rasterize: async (svg) => new TextEncoder().encode(svg),
    })
    let msg = ""
    try {
      await r.renderPng("x")
    } catch (err) {
      msg = String((err as Error).message)
    }
    expect(msg).toContain("PlantUML 渲染错误：unknown type @startfoo")
  })

  test("并发渲染串行化（引擎不支持并发）", async () => {
    const inFlight = { n: 0, max: 0 }
    const r = createPlantUmlRenderer({
      engine: async () => ({
        renderToString: (lines, ok) => {
          inFlight.n++
          inFlight.max = Math.max(inFlight.max, inFlight.n)
          setTimeout(() => {
            inFlight.n--
            ok(`<svg width="1" height="1">${lines[0]}</svg>`)
          }, 30)
        },
      }),
      rasterize: async (svg) => new TextEncoder().encode(svg),
    })
    await Promise.all([r.renderPng("a"), r.renderPng("b"), r.renderPng("c")])
    expect(inFlight.max).toBe(1)
  })
})

/** 渲染超时兜底（引擎挂起不回调）。 */
describe("渲染超时", () => {
  test("引擎挂起时超时抛错（注入小超时加速）", async () => {
    const r = createPlantUmlRenderer({
      engine: async () => ({ renderToString: () => {} }),
      rasterize: async (svg) => new TextEncoder().encode(svg),
      timeoutMs: 200,
    })
    let msg = ""
    try {
      await r.renderPng("x")
    } catch (err) {
      msg = String((err as Error).message)
    }
    expect(msg).toContain("渲染超时")
  }, 5000)
})
