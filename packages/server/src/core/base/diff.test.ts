import { describe, expect, test } from "bun:test"
import { inferLang, splitLines } from "./diff"

describe("inferLang", () => {
  test("known extensions", () => {
    expect(inferLang("src/main.ts")).toBe("typescript")
    expect(inferLang("a.JSON")).toBe("json")
    expect(inferLang("x.py")).toBe("python")
    expect(inferLang("run.sh")).toBe("bash")
    expect(inferLang("page.html")).toBe("xml")
  })
  test("unknown extension or no dot", () => {
    expect(inferLang("Makefile")).toBe("")
    expect(inferLang("data.xyz")).toBe("")
  })
})

describe("splitLines", () => {
  test("empty text yields no lines", () => {
    expect(splitLines("")).toEqual([])
  })

  test("trailing newline ignored: `a\\n` == `a`", () => {
    expect(splitLines("a\n")).toEqual(["a"])
    expect(splitLines("a")).toEqual(["a"])
  })

  test("single trailing newline does not add an empty line", () => {
    expect(splitLines("a\nb\nc")).toEqual(["a", "b", "c"])
  })
})
