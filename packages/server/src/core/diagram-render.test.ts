import { describe, expect, test } from "bun:test"
import { existsSync, mkdtempSync, readdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { gzipSync } from "node:zlib"
import { createDiagramRenderer, estimateGeometry, materializeD2Files, normalizeSvgRoot, parseEchartsInput, svgLogicalSize, textWidthEstimate } from "./diagram-render"

describe("diagram-render（后端四语言图表渲染）", () => {
  test("svgLogicalSize: width/height 属性优先，非数值回退 viewBox（mermaid 输出形态），缺失为 0", () => {
    expect(svgLogicalSize('<svg width="100" height="50"></svg>')).toEqual({ width: 100, height: 50 })
    expect(svgLogicalSize('<svg width="100%" viewBox="0 0 320 240"></svg>')).toEqual({ width: 320, height: 240 })
    expect(svgLogicalSize('<svg style="max-width: 16px;" viewBox="-8 -8 16 16"></svg>')).toEqual({ width: 16, height: 16 })
    expect(svgLogicalSize('<svg width="100%"></svg>')).toEqual({ width: 0, height: 0 })
    expect(svgLogicalSize('<svg></svg>')).toEqual({ width: 0, height: 0 })
  })

  test("normalizeSvgRoot: 显式化根尺寸、负原点 viewBox 平移归一、style/defs 保持 svg 直接子元素", () => {
    const out = normalizeSvgRoot('<svg width="100%" viewBox="-42.8 -32.5 174.4 73"><style>#t{font-size:16px;}</style><g><rect x="-10" y="0" width="5" height="5"/></g></svg>', { width: 174.4, height: 73 })
    // 根宽高显式化 + viewBox 移除
    expect(out).toContain('<svg width="174.4" height="73">')
    expect(out).not.toContain("viewBox")
    // style 不被平移组包裹（svgdom 要求保持 svg 直接子元素），内容整体平移
    expect(out.indexOf("<style>")).toBeLessThan(out.indexOf('<g transform="translate(42.8,32.5)">'))
    // 结构平衡：单根 + 平移组闭合
    expect(out.endsWith("</g></svg>")).toBe(true)
    expect((out.match(/<g[\s>]/g) ?? []).length).toBe(2)
    expect((out.match(/<\/g>/g) ?? []).length).toBe(2)
    // 非负原点 viewBox：不包裹平移组
    const zero = normalizeSvgRoot('<svg width="100%" viewBox="0 0 100 50"><rect width="10" height="10"/></svg>', { width: 100, height: 50 })
    expect(zero).not.toContain("translate(")
    expect(zero.endsWith("</svg>")).toBe(true)
  })

  test("materializeD2Files: gzip 还原落盘、幂等、旧版本目录清理", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-d2js-"))
    const parent = join(home, "vendor", "d2js")
    const embedded = {
      version: "0.1.33",
      files: { "index.js": gzipSync(Buffer.from('export const x = 1')).toString("base64"), "d2.wasm": gzipSync(Buffer.from([1, 2, 3, 4])).toString("base64") },
    }
    const dir = join(parent, embedded.version)
    materializeD2Files(dir, embedded)
    expect(existsSync(join(dir, "index.js"))).toBe(true)
    const wasm = new Uint8Array(await Bun.file(join(dir, "d2.wasm")).arrayBuffer())
    expect(wasm).toEqual(new Uint8Array([1, 2, 3, 4]))
    // 幂等：重复物化不重写
    const mtime = Bun.file(join(dir, ".version")).lastModified
    materializeD2Files(dir, embedded)
    expect(Bun.file(join(dir, ".version")).lastModified).toBe(mtime)
    // 旧版本清理：新版本物化后旧目录删除
    materializeD2Files(join(parent, "9.9.9"), { version: "9.9.9", files: {} })
    expect(readdirSync(parent)).toEqual(["9.9.9"])
    rmSync(home, { recursive: true, force: true })
  })

  test("createDiagramRenderer: 按 format 分派引擎、白底默认、错误带语言前缀", async () => {
    const calls: Array<{ format: string; code: string }> = []
    const svgFor = (fmt: string, code: string) => `<svg width="10" height="10">${fmt}:${code}</svg>`
    const renderer = createDiagramRenderer({
      plantuml: async () => ({
        renderPng: async (code) => {
          calls.push({ format: "plantuml", code })
          return new TextEncoder().encode(`PNG:puml:${code}`)
        },
      }),
      mermaid: async (code) => {
        calls.push({ format: "mermaid", code })
        return svgFor("mermaid", code)
      },
      d2: async (code) => {
        calls.push({ format: "d2", code })
        return svgFor("d2", code)
      },
      rasterize: async (svg, opts) => new TextEncoder().encode(`PNG:${svg}|bg=${opts.background ?? ""}`),
    })
    const puml = await renderer.renderPng("Alice -> Bob", { format: "plantuml" })
    expect(new TextDecoder().decode(puml)).toBe("PNG:puml:Alice -> Bob")
    expect(calls[0]).toEqual({ format: "plantuml", code: "Alice -> Bob" })
    const mmd = await renderer.renderPng("flowchart LR\nA --> B", { format: "mermaid" })
    expect(new TextDecoder().decode(mmd)).toContain("mermaid:flowchart")
    expect(new TextDecoder().decode(mmd)).toContain("bg=#ffffff")
    const d2 = await renderer.renderPng("a -> b", { format: "d2", background: "#000" })
    expect(new TextDecoder().decode(d2)).toContain("d2:a -> b")
    expect(new TextDecoder().decode(d2)).toContain("bg=#000")
    expect(calls.map((c) => c.format)).toEqual(["plantuml", "mermaid", "d2"])
    // 缺省 format = plantuml
    await renderer.renderPng("x", {})
    expect(calls[3]).toEqual({ format: "plantuml", code: "x" })
  })

  test("createDiagramRenderer: 引擎错误包装为「{语言} 渲染错误」并截断", async () => {
    const renderer = createDiagramRenderer({
      mermaid: async () => {
        throw new Error("Parse error on line 3".repeat(50))
      },
      rasterize: async () => new Uint8Array([1]),
    })
    const err = await renderer.renderPng("bad", { format: "mermaid" }).then(
      () => null,
      (e) => e as Error,
    )
    expect(err).toBeInstanceOf(Error)
    expect(err!.message).toContain("Mermaid 渲染错误：")
    expect(err!.message.length).toBeLessThan(230)
    // 非 Error 抛错（d2 编译错误为 JSON 字符串）同样归一
    const renderer2 = createDiagramRenderer({
      d2: async () => {
        // eslint-disable-next-line no-throw-literal
        throw '[{"range":"index,0:0:0","errmsg":"connection missing destination"}]'
      },
      rasterize: async () => new Uint8Array([1]),
    })
    const err2 = await renderer2.renderPng("x ->", { format: "d2" }).then(
      () => null,
      (e) => e as Error,
    )
    expect(err2!.message).toContain("D2 渲染错误：")
    expect(err2!.message).toContain("connection missing destination")
  })

  test("textWidthEstimate: 全角字符 1.0em / 半角 0.6em 逐字符估算（中文标签防整图偏窄）", () => {
    expect(textWidthEstimate("ABC", 16)).toBe(3 * 16 * 0.6)
    expect(textWidthEstimate("数据处理", 16)).toBe(4 * 16)
    expect(textWidthEstimate("A：B", 16)).toBe(2 * 16 * 0.6 + 16) // 全角冒号按 1.0em
    expect(textWidthEstimate("ｄａｔａ", 16)).toBe(4 * 16) // 全角形式字母同样 1.0em
    expect(textWidthEstimate("", 16)).toBe(0)
  })

  test("estimateGeometry: text 全角宽度、line 包围盒、未知元素返回 null", () => {
    const text = estimateGeometry({ tagName: "text", getAttribute: () => null, textContent: "节点" })
    expect(text!.width).toBe(2 * 16)
    expect(text!.height).toBe(16 * 1.2)
    const line = estimateGeometry({
      tagName: "line",
      getAttribute: (n: string) => ({ x1: "10", y1: "20", x2: "30", y2: "5" }[n] ?? null) as string | null,
    })
    expect(line).toEqual({ x: 10, y: 5, width: 20, height: 15 })
    expect(estimateGeometry({ tagName: "use", getAttribute: () => null })).toBeNull()
  })

  test("createDiagramRenderer: 真实 mermaid 引擎中文标签图宽于同字符数 ASCII 标签（viewBox 防回归偏窄）", async () => {
    let svg = ""
    const renderer = createDiagramRenderer({
      rasterize: async (s) => {
        svg = s
        return new Uint8Array([1])
      },
    })
    const widthOf = async (code: string) => {
      await renderer.renderPng(code, { format: "mermaid" })
      const { width, height } = svgLogicalSize(svg)
      expect(height).toBeGreaterThan(0)
      return width
    }
    const cjk = await widthOf('flowchart LR\nA["数据处理"] --> B["结果输出"]')
    const ascii = await widthOf('flowchart LR\nA["data"] --> B["outp"]')
    expect(cjk).toBeGreaterThan(ascii)
  }, 30000)

  test("createDiagramRenderer: 各语言渲染走串行队列（前一个失败不阻塞后续）", async () => {
    const order: string[] = []
    const renderer = createDiagramRenderer({
      mermaid: async (code) => {
        order.push(`start:${code}`)
        await Bun.sleep(20)
        order.push(`end:${code}`)
        if (code === "bad") throw new Error("boom")
        return "<svg/>"
      },
      rasterize: async () => new Uint8Array([1]),
    })
    const results = await Promise.allSettled([
      renderer.renderPng("a", { format: "mermaid" }),
      renderer.renderPng("bad", { format: "mermaid" }),
      renderer.renderPng("b", { format: "mermaid" }),
    ])
    expect(results[0].status).toBe("fulfilled")
    expect(results[1].status).toBe("rejected")
    expect(results[2].status).toBe("fulfilled")
    // 串行执行：a 完成后才开始 bad，bad 失败后 b 仍执行
    expect(order).toEqual(["start:a", "end:a", "start:bad", "end:bad", "start:b", "end:b"])
  })

  test("parseEchartsInput: 严格/宽松 JSON、信封尺寸钳制、非对象拒绝、SSR 默认注入", () => {
    // 整体 JSON 即 option；注入 animation:false 与 darkMode 缺省值；默认画布 960×600
    const opt = parseEchartsInput('{"series":[{"type":"pie","data":[1,2]}]}')
    expect((opt.option.series as unknown[]).length).toBe(1)
    expect(opt.option.animation).toBe(false)
    expect(opt.option.darkMode).toBe(false)
    expect(opt.width).toBe(960)
    expect(opt.height).toBe(600)
    // dark 参数注入（前端暗色主题）
    expect(parseEchartsInput('{"xAxis":{}}', true).option.darkMode).toBe(true)
    // 用户显式 darkMode 保留
    expect(parseEchartsInput('{"darkMode":true}').option.darkMode).toBe(true)
    // 宽松解析：注释 + 尾逗号
    expect(parseEchartsInput('{\n  // 注释\n  "series": [1],\n}').option.series).toEqual([1])
    // 信封：option + width/height
    const env = parseEchartsInput('{"option":{"series":[1]},"width":800,"height":500}')
    expect(env.width).toBe(800)
    expect(env.height).toBe(500)
    expect(env.option.series).toEqual([1])
    // 尺寸钳制 200-4000（非法值回退默认）
    const clamped = parseEchartsInput('{"option":{},"width":99999,"height":10}')
    expect(clamped.width).toBe(4000)
    expect(clamped.height).toBe(200)
    expect(parseEchartsInput('{"option":{},"width":"wide"}').width).toBe(960)
    // 非法输入给出可读错误（回传模型修正）
    expect(() => parseEchartsInput("{bad json")).toThrow("合法 JSON")
    expect(() => parseEchartsInput("[1,2,3]")).toThrow("JSON 对象")
    expect(() => parseEchartsInput('"str"')).toThrow("JSON 对象")
  })

  test("parseEchartsInput: 标题/图例防重叠（置顶图例与顶部标题冲突时下移 + grid.top 联动，其余不干预）", () => {
    const opt = (code: string) => parseEchartsInput(code).option as Record<string, any>
    // v5 习惯 legend.top:0 与顶部标题（默认带 15–46.6）冲突 → 下移至 53，grid.top 由默认 65 联动下调至 89
    const fixed = opt('{"title":{"text":"周报"},"legend":{"top":0},"series":[{"name":"a","type":"bar"}]}')
    expect(fixed.legend.top).toBe(53)
    expect(fixed.grid.top).toBe(89)
    // 显式小数值仍冲突（30 < 46.6+6）→ 同样下移；title.top:0 显式时标题带更浅（0–31.6）→ 下移至 38、grid 74
    expect(opt('{"title":{"text":"周报"},"legend":{"top":30}}').legend.top).toBe(53)
    const t0 = opt('{"title":{"text":"周报","top":0},"legend":{"top":0}}')
    expect(t0.legend.top).toBe(38)
    expect(t0.grid.top).toBe(74)
    // 副标题加高标题带（15–71）→ 下移至 77、grid 113
    const sub = opt('{"title":{"text":"周报","subtext":"本周"},"legend":{"top":0}}')
    expect(sub.legend.top).toBe(77)
    expect(sub.grid.top).toBe(113)
    // 不干预：图例 top 未设置（echarts 6 默认底部）/bottom/middle/已避开标题（60 ≥ 46.6+6）
    for (const lg of ['{}', '{"top":"bottom"}', '{"top":"middle"}', '{"top":60}']) {
      const o = opt(`{"title":{"text":"周报"},"legend":${lg}}`)
      expect(o.legend).toEqual(JSON.parse(lg))
      expect(o.grid).toBeUndefined()
    }
    // 不干预：标题不在顶部 / 图例隐藏 / 二者水平分居两侧
    expect(opt('{"title":{"text":"周报","top":"bottom"},"legend":{"top":0}}').legend.top).toBe(0)
    expect(opt('{"title":{"text":"周报"},"legend":{"top":0,"show":false}}').legend.top).toBe(0)
    expect(opt('{"title":{"text":"周报","left":"left"},"legend":{"top":0,"left":"right"}}').legend.top).toBe(0)
    // 尊重显式 grid 布局：grid.top/height 已设置时不联动下调
    expect(opt('{"title":{"text":"周报"},"legend":{"top":0},"grid":{"top":80}}').grid.top).toBe(80)
    const gh = opt('{"title":{"text":"周报"},"legend":{"top":0},"grid":{"height":400}}')
    expect(gh.grid.height).toBe(400)
    expect(gh.grid.top).toBeUndefined()
    // 标题为数组（多标题）取冲突条目最大底边
    expect(opt('{"title":[{"text":"a","top":0},{"text":"b"}],"legend":{"top":0}}').legend.top).toBe(53)
    // 图例条目名缺省取 series 名，折行估算加高（窄画布 300 宽 → 两行 → grid 下调更深）
    const wide = opt('{"option":{"title":{"text":"周报"},"legend":{"top":0},"series":[{"name":"每日任务执行量统计"},{"name":"模拟数据完成数汇总"},{"name":"第三季度累计口径"}]},"width":300}')
    expect(wide.grid.top).toBe(117)
  })

  test("createDiagramRenderer: echarts 走注入引擎并栅格化，错误带 ECharts 前缀", async () => {
    const calls: string[] = []
    const renderer = createDiagramRenderer({
      echarts: async (code) => {
        calls.push(code)
        return '<svg width="10" height="10">ech</svg>'
      },
      rasterize: async (svg, opts) => new TextEncoder().encode(`PNG:${svg}|bg=${opts.background ?? ""}`),
    })
    const png = await renderer.renderPng('{"series":[]}', { format: "echarts" })
    expect(new TextDecoder().decode(png)).toContain("ech")
    expect(new TextDecoder().decode(png)).toContain("bg=#ffffff")
    expect(calls).toEqual(['{"series":[]}'])
    const bad = createDiagramRenderer({
      echarts: async () => {
        throw new Error("Unknown series nope")
      },
      rasterize: async () => new Uint8Array([1]),
    })
    const err = await bad.renderPng("{}", { format: "echarts" }).then(
      () => null,
      (e) => e as Error,
    )
    expect(err).toBeInstanceOf(Error)
    expect(err!.message).toContain("ECharts 渲染错误：")
  })

  test("createDiagramRenderer: 真实 echarts SSR 引擎渲染 PNG（JSON option → SVG → PNG 字节）", async () => {
    const renderer = createDiagramRenderer()
    const code = JSON.stringify({
      title: { text: "季度营收" },
      xAxis: { type: "category", data: ["Q1", "Q2", "Q3"] },
      yAxis: { type: "value" },
      series: [
        { type: "bar", data: [10, 20, 15] },
        { type: "line", data: [8, 18, 12] },
      ],
    })
    const png = await renderer.renderPng(code, { format: "echarts" })
    expect(png.byteLength).toBeGreaterThan(1000)
    expect(png[0]).toBe(0x89) // PNG 魔数
    expect(png[1]).toBe(0x50)
  }, 20000)
})
