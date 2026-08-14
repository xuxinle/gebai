import { describe, expect, test } from "bun:test"
import { analyzeCode } from "./analyzer"

describe("tree-sitter analyze", () => {
  test("javascript: top-level declarations and class methods", async () => {
    const js = `import { serve } from "hono"\n\nexport function start() {\n  return 1\n}\n\nclass Engine {\n  run() {}\n  stop() {}\n}\n\nconst x = 5\n`
    const out = await analyzeCode(js, "js", "src/index.ts")
    expect(out).toContain("[js] src/index.ts")
    expect(out).toContain("import_statement")
    expect(out).toContain("function_declaration start (3-5)")
    expect(out).toContain("class_declaration Engine (7-10)")
    expect(out).toContain("method_definition run")
    expect(out).toContain("method_definition stop")
  })

  test("typescript: interface / type alias / export class", async () => {
    const ts = `interface User { id: string }\ntype Id = string\nexport class A {}\n`
    const out = await analyzeCode(ts, "ts", "src/types.ts")
    expect(out).toContain("interface_declaration User")
    expect(out).toContain("type_alias_declaration Id")
    expect(out).toContain("class_declaration A")
  })

  test("python: import / function / class method", async () => {
    const py = `import os\n\ndef main():\n    pass\n\nclass Foo:\n    def bar(self):\n        pass\n`
    const out = await analyzeCode(py, "py", "main.py")
    expect(out).toContain("import_statement")
    expect(out).toContain("function_definition main")
    expect(out).toContain("class_definition Foo")
    expect(out).toContain("function_definition bar")
  })

  test("unsupported language returns hint with supported list", async () => {
    const out = await analyzeCode("x", "unknown", "a.xyz")
    expect(out).toContain("不支持的语言")
    expect(out).toContain("ts")
  })
})
