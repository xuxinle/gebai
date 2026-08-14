import { describe, expect, test } from "bun:test"
import { analyzeCode, _setWasmLoaderForTest } from "./analyzer"

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

describe("wasm 资源加载（二进制回退与诚实报错）", () => {
  test("wasm 加载失败报「语法分析不可用」而非误导的「不支持的语言」", async () => {
    // 模拟二进制模式资源缺失：加载源返回 null（内嵌产物缺失/损坏）
    _setWasmLoaderForTest(async () => null)
    try {
      const out = await analyzeCode("let a = 1", "js", "x.js")
      expect(out).toContain("语法分析不可用")
      expect(out).not.toContain("不支持的语言")
    } finally {
      _setWasmLoaderForTest(null)
    }
  })

  test("加载源注入合法 wasm 字节（内嵌回退路径）可正常解析", async () => {
    // 直接从 node_modules 读真实语法 wasm，模拟内嵌注册表还原出的字节
    const { readFileSync } = await import("node:fs")
    const { join } = await import("node:path")
    const wasmPath = require.resolve("tree-sitter-wasms/out/tree-sitter-go.wasm")
    const bytes = new Uint8Array(readFileSync(wasmPath))
    void join
    _setWasmLoaderForTest(async () => bytes)
    try {
      const out = await analyzeCode("package main\n\nfunc main() {}\n", "go", "main.go")
      expect(out).toContain("[go] main.go")
      expect(out).toContain("function_declaration main")
    } finally {
      _setWasmLoaderForTest(null)
    }
  })

  test("未知语言仍报「不支持的语言」并附支持列表", async () => {
    const out = await analyzeCode("x", "unknown", "a.xyz")
    expect(out).toContain("不支持的语言")
    expect(out).toContain("ts")
  })
})
