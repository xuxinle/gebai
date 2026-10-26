import { describe, expect, test } from "bun:test"
import { readdirSync } from "node:fs"
import { join } from "node:path"
import { STYLE_DOCS, TEMPLATE_DOCS, readStyleDoc, styleDocList } from "./index"

describe("裁云风格模板 SKILL 资源", () => {
  test("总纲内容到位（选型表 + 硬门槛 + 承接判据 + 反模式）", () => {
    const doc = readStyleDoc("overview")
    expect(doc?.content).toContain("选型表")
    expect(doc?.content).toContain("承接判据")
    expect(doc?.content).toContain("反模式")
    // 七种模板名都要在总纲的选型表里出现（否则选型无从下手）
    for (const name of TEMPLATE_DOCS.map((d) => d.name)) {
      expect(doc?.content).toContain(name)
    }
  })

  test("每份模板都有「用裁云怎么落地」（这是本库区别于纯影评的地方）", () => {
    for (const d of TEMPLATE_DOCS) {
      expect(d.content).toContain("用裁云怎么落地")
      // 落地段要给出可照着写的东西，不能只有形容词
      expect(d.content.length).toBeGreaterThan(800)
    }
  })

  test("templates 目录与登记表一一对应（防漏登记/死登记）", () => {
    const dir = join(import.meta.dirname, "templates")
    const files = readdirSync(dir)
      .filter((f) => f.endsWith(".md"))
      .map((f) => f.replace(/\.md$/, ""))
      .sort()
    expect(TEMPLATE_DOCS.map((d) => d.name).sort()).toEqual(files)
  })

  test("全部文档都以原始 Markdown 文本载入（非 html loader 的 HTML）", () => {
    for (const d of styleDocList()) {
      expect(d.content.trim().length).toBeGreaterThan(800)
      expect(d.content.startsWith("# ")).toBe(true) // html loader 会变成 <h1>…
      expect(d.content).not.toContain("<p>")
    }
  })

  test("按名读取：大小写与 .md 扩展名归一，未知名字返回 undefined", () => {
    expect(STYLE_DOCS.map((d) => d.name)).toEqual(["overview"])
    expect(readStyleDoc("03-causal-chain")?.name).toBe("03-causal-chain")
    expect(readStyleDoc("03-Causal-Chain.MD")?.name).toBe("03-causal-chain")
    expect(readStyleDoc("no-such-template")).toBeUndefined()
  })

  test("总数是 1 总纲 + 7 模板（数字变化必须是有意的）", () => {
    expect(styleDocList()).toHaveLength(8)
  })
})
