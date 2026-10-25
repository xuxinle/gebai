import { describe, expect, test } from "bun:test"
import { cleanScriptOutput } from "./script-output"

describe("cleanScriptOutput（脚本输出纯净化）", () => {
  test("CRLF → LF、去行尾空白、去首尾空行", () => {
    expect(cleanScriptOutput("\r\n\r\nfoo  \r\nbar\t\r\n\r\n")).toBe("foo\nbar")
  })

  test("连续空行压成一个空行（段落分隔保留）", () => {
    expect(cleanScriptOutput("a\n\n\n\n\nb")).toBe("a\n\nb")
    expect(cleanScriptOutput("a\n\nb")).toBe("a\n\nb")
  })

  test("去 ANSI 控制序列（彩色/光标控制输出）", () => {
    const colored = "\u001b[32mPASS\u001b[0m 3 tests\n\u001b[1m\u001b[31mFAIL\u001b[0m 1"
    expect(cleanScriptOutput(colored)).toBe("PASS 3 tests\nFAIL 1")
  })

  test("正文内容（行号、路径、缩进、错误文本）原样保留", () => {
    const src = "src/a.ts\n  12: const x = 1\n    at fn (file.js:3:7)\n[exit 1]"
    expect(cleanScriptOutput(src)).toBe(src)
  })

  test("空/纯空白输入", () => {
    expect(cleanScriptOutput("")).toBe("")
    expect(cleanScriptOutput("\r\n \n\t\r\n")).toBe("")
  })
})
