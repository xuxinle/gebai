/**
 * 文件类型图标判定：完整文件名优先、再扩展名、最后兜底（与 VSCode 同一顺序）。
 *
 * 为什么值得钉：这张表是「看着对不对」类改动里最容易静默退化的地方——`ts` 与 `tsx` 分到同一张图、
 * `Dockerfile` 落成通用文件、`README.md` 不再是 readme 图标，页面都不报错，只是「不像 IDE 了」。
 * 判定逻辑读的是生成产物 `file-icons.generated.ts`（由 `scripts/build-file-icons.ts` 从
 * material-icon-theme 生成），因此这里同时守着「产物里这些常用类型确实都有映射」。
 */
import { describe, expect, test } from "bun:test"
import { fileIconId } from "./file-icons"
import { FILE_ICON_BY_EXT, FILE_ICON_BY_NAME, FILE_ICON_SVG } from "./file-icons.generated"

describe("文件类型图标", () => {
  test("常用代码类型各有一张图（不是全落成通用文件）", () => {
    const cases: [string, string][] = [
      ["a.ts", "typescript"],
      ["a.tsx", "react_ts"],
      ["a.js", "javascript"],
      ["a.jsx", "react"],
      ["a.py", "python"],
      ["a.go", "go"],
      ["a.rs", "rust"],
      ["a.java", "java"],
      ["a.kt", "kotlin"],
      ["a.swift", "swift"],
      ["a.c", "c"],
      ["a.cpp", "cpp"],
      ["a.cs", "csharp"],
      ["a.php", "php"],
      ["a.rb", "ruby"],
      ["a.vue", "vue"],
      ["a.svelte", "svelte"],
      ["a.html", "html"],
      ["a.css", "css"],
      ["a.scss", "sass"],
    ]
    for (const [name, icon] of cases) expect(`${name} → ${fileIconId(name)}`).toBe(`${name} → ${icon}`)
  })

  test("常用文件类型：配置 / 文档 / 媒体 / 归档各有其形", () => {
    const cases: [string, string][] = [
      ["a.json", "json"],
      ["a.yaml", "yaml"],
      ["a.toml", "toml"],
      ["a.xml", "xml"],
      ["a.md", "markdown"],
      ["a.txt", "document"],
      ["a.pdf", "pdf"],
      ["a.docx", "word"],
      ["a.xlsx", "table"],
      ["a.pptx", "powerpoint"],
      ["a.png", "image"],
      ["a.svg", "svg"],
      ["a.mp4", "video"],
      ["a.mp3", "audio"],
      ["a.zip", "zip"],
      ["a.sqlite", "database"],
      ["a.sh", "console"],
      ["a.ps1", "powershell"],
    ]
    for (const [name, icon] of cases) expect(`${name} → ${fileIconId(name)}`).toBe(`${name} → ${icon}`)
  })

  test("完整文件名优先于扩展名（Dockerfile / package.json / README.md）", () => {
    expect(fileIconId("Dockerfile")).toBe("docker")
    expect(fileIconId("dockerfile")).toBe("docker")
    expect(fileIconId("package.json")).toBe("nodejs")
    expect(fileIconId("tsconfig.json")).toBe("tsconfig")
    expect(fileIconId("README.md")).toBe("readme")
    expect(fileIconId("LICENSE")).toBe("license")
  })

  test("未知类型与无扩展名落到通用文件图标（不是空白）", () => {
    for (const name of ["a.zzz", "Makefile2", "noext"]) expect(`${name} → ${fileIconId(name)}`).toBe(`${name} → file`)
    expect(FILE_ICON_SVG.file).toBeDefined()
  })

  test("映射表指向的图标都有 SVG（缺一条就是「按名字找不到图」）", () => {
    for (const [key, icon] of Object.entries(FILE_ICON_BY_EXT)) expect(`${key} → ${icon}:${FILE_ICON_SVG[icon] ? "有" : "缺"}`).toBe(`${key} → ${icon}:有`)
    for (const [key, icon] of Object.entries(FILE_ICON_BY_NAME)) expect(`${key} → ${icon}:${FILE_ICON_SVG[icon] ? "有" : "缺"}`).toBe(`${key} → ${icon}:有`)
  })

  test("图标自带配色（依赖上游 hex，不再由 CSS 类上色）", () => {
    expect(FILE_ICON_SVG.typescript!.body).toContain("#0288d1")
  })
})
