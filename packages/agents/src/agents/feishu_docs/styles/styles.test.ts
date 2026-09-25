import { describe, expect, test } from "bun:test"
import { readdirSync } from "node:fs"
import { join } from "node:path"
import { GENRE_DOCS, STYLE_DOCS, readStyleDoc, styleDocList } from "./index"

describe("排版 SKILL 资源", () => {
  test("总纲与语法文档内容到位（指向工具与工作流）", () => {
    expect(readStyleDoc("style")?.content).toContain("工作流")
    expect(readStyleDoc("style")?.content).toContain("lint_doc")
    expect(readStyleDoc("xml")?.content).toContain("import_xml")
    expect(readStyleDoc("xml")?.content).toContain("width-ratio")
  })

  test("genres 目录与登记表一一对应（防漏登记/死登记）", () => {
    const dir = join(import.meta.dirname, "genres")
    const files = readdirSync(dir).filter((f) => f.endsWith(".md")).map((f) => f.replace(/\.md$/, "")).sort()
    expect(GENRE_DOCS.map((d) => d.name).sort()).toEqual(files)
  })

  test("全部文档都以原始 Markdown 文本载入（非 html loader 的 HTML）", () => {
    for (const d of styleDocList()) {
      expect(d.content.trim().length).toBeGreaterThan(100)
      expect(d.content.startsWith("# ")).toBe(true) // 原始 Markdown 首行（html loader 会变成 <h1>…）
    }
  })

  test("按名读取：大小写与 .md 扩展名归一，未知名字返回 undefined", () => {
    expect(STYLE_DOCS.map((d) => d.name)).toEqual(["style", "xml"])
    expect(readStyleDoc("weekly-report")?.name).toBe("weekly-report")
    expect(readStyleDoc("Weekly-Report.MD")?.name).toBe("weekly-report")
    expect(readStyleDoc("no-such-genre")).toBeUndefined()
  })
})
