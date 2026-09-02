import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, dirname } from "node:path"
import { diffLines, unifiedDiff, inferLang, DIFF_MAX_LINES } from "./diff"
import { diffTool, writeTool } from "../tools"
import type { ToolContext } from "./types"

describe("diffLines", () => {
  test("identical text: all equal", () => {
    const lines = diffLines("a\nb\nc", "a\nb\nc")
    expect(lines).toEqual([
      { kind: "equal", text: "a" },
      { kind: "equal", text: "b" },
      { kind: "equal", text: "c" },
    ])
  })

  test("trailing newline ignored: `a\\n` == `a`", () => {
    expect(diffLines("a\n", "a")).toEqual([{ kind: "equal", text: "a" }])
    expect(diffLines("", "")).toEqual([])
  })

  test("appended lines are add", () => {
    const lines = diffLines("a\nb", "a\nb\nc\nd")
    expect(lines.map((l) => l.kind)).toEqual(["equal", "equal", "add", "add"])
    expect(lines[2]).toEqual({ kind: "add", text: "c" })
  })

  test("removed lines are del", () => {
    const lines = diffLines("a\nb\nc", "a")
    expect(lines.map((l) => l.kind)).toEqual(["equal", "del", "del"])
  })

  test("modified line = del + add", () => {
    const lines = diffLines("a\nold\nc", "a\nnew\nc")
    expect(lines).toEqual([
      { kind: "equal", text: "a" },
      { kind: "del", text: "old" },
      { kind: "add", text: "new" },
      { kind: "equal", text: "c" },
    ])
  })

  test("interleaved changes keep order", () => {
    const lines = diffLines("1\n2\n3\n4", "1\n3\nx\n4")
    expect(lines.map((l) => l.kind)).toEqual(["equal", "del", "equal", "add", "equal"])
    expect(lines.map((l) => l.text)).toEqual(["1", "2", "3", "x", "4"])
  })

  test("empty side becomes pure add/del", () => {
    expect(diffLines("", "a\nb").map((l) => l.kind)).toEqual(["add", "add"])
    expect(diffLines("a\nb", "").map((l) => l.kind)).toEqual(["del", "del"])
  })

  test("oversize input degrades to all del + all add (no memory blowup)", () => {
    const big = Array.from({ length: DIFF_MAX_LINES + 1 }, (_, i) => `line${i}`).join("\n")
    const lines = diffLines(big, "")
    expect(lines).toHaveLength(DIFF_MAX_LINES + 1)
    expect(lines.every((l) => l.kind === "del")).toBe(true)
  })
})

describe("unifiedDiff", () => {
  test("no difference", () => {
    expect(unifiedDiff("a\nb", "a\nb")).toBe("--- old\n+++ new\n（无差异）")
  })

  test("hunk header with context lines", () => {
    const out = unifiedDiff("a\nb\nc\nd", "a\nb\nx\nd", "old.ts", "new.ts")
    const lines = out.split("\n")
    expect(lines[0]).toBe("--- old.ts")
    expect(lines[1]).toBe("+++ new.ts")
    expect(lines[2]).toBe("@@ -1,4 +1,4 @@")
    expect(lines).toContain("-c")
    expect(lines).toContain("+x")
  })

  test("change at start: hunk starts at line 1", () => {
    const out = unifiedDiff("old\nb\nc", "new\nb\nc")
    expect(out.split("\n")[2]).toBe("@@ -1,3 +1,3 @@")
  })

  test("change at end: trailing context completes the hunk", () => {
    const out = unifiedDiff("a\nb\nc", "a\nb\nd")
    const lines = out.split("\n")
    expect(lines[2]).toBe("@@ -1,3 +1,3 @@")
    expect(lines).toContain("-c")
    expect(lines).toContain("+d")
  })

  test("empty new side uses /dev/null and +0,0", () => {
    const out = unifiedDiff("a\nb", "")
    const lines = out.split("\n")
    expect(lines[0]).toBe("--- old")
    expect(lines[1]).toBe("+++ /dev/null")
    expect(lines[2]).toBe("@@ -1,2 +0,0 @@")
    expect(lines).toContain("-a")
    expect(lines).toContain("-b")
  })

  test("empty old side uses /dev/null and -0,0", () => {
    const out = unifiedDiff("", "a\nb")
    expect(out.split("\n")[0]).toBe("--- /dev/null")
    expect(out.split("\n")[2]).toBe("@@ -0,0 +1,2 @@")
    expect(out.split("\n")).toContain("+a")
    expect(out.split("\n")).toContain("+b")
  })

  test("separate hunks: distant changes produce two @@ headers", () => {
    const oldText = Array.from({ length: 20 }, (_, i) => `l${i + 1}`).join("\n")
    const newText = Array.from({ length: 20 }, (_, i) => (i === 2 ? "CHANGED" : i === 17 ? "CHANGED2" : `l${i + 1}`)).join("\n")
    const out = unifiedDiff(oldText, newText)
    expect((out.match(/^@@/gm) ?? []).length).toBe(2)
  })

  test("hunks carry trailing context lines (patch-applicable)", () => {
    // 中间变化：hunk 尾部应带满 3 行上下文（变化行不直接结尾，保证 patch 可应用）
    const oldText = Array.from({ length: 20 }, (_, i) => `l${i + 1}`).join("\n")
    const newText = oldText.replace("l3", "L3").replace("l20", "L20")
    const lines = unifiedDiff(oldText, newText).split("\n")
    const h1 = lines.indexOf("@@ -1,6 +1,6 @@")
    expect(h1).toBeGreaterThan(-1)
    expect(lines[h1 + 5]).toBe(" l4")
    expect(lines[h1 + 6]).toBe(" l5")
    expect(lines[h1 + 7]).toBe(" l6")
    // 文件末尾的变化允许以变化行结尾（EOF 场景）
    expect(lines).toContain("@@ -17,4 +17,4 @@")
    // 近间隔变化合并进同一 hunk（间隔 ≤ 2×上下文）
    const oldNear = Array.from({ length: 20 }, (_, i) => `l${i + 1}`).join("\n")
    const newNear = oldNear.replace("l3", "L3").replace("l6", "L6")
    const outNear = unifiedDiff(oldNear, newNear)
    expect((outNear.match(/^@@/gm) ?? []).length).toBe(1)
  })

  test("EOF trailing context is capped at 3 lines (GNU behavior)", () => {
    // 首行变化、其后 10 行相等到文件尾：hunk 尾部上下文只取 3 行，不并入全部剩余行
    const oldText = Array.from({ length: 11 }, (_, i) => `l${i + 1}`).join("\n")
    const newText = oldText.replace("l1", "L1")
    const lines = unifiedDiff(oldText, newText).split("\n")
    expect(lines[2]).toBe("@@ -1,4 +1,4 @@")
    expect(lines[3]).toBe("-l1")
    expect(lines[4]).toBe("+L1")
    expect(lines[5]).toBe(" l2")
    expect(lines[6]).toBe(" l3")
    expect(lines[7]).toBe(" l4")
    expect(lines).not.toContain(" l11")
    expect(lines).toHaveLength(8)
  })
})

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

function ctx(home: string, sessionId = "s1"): ToolContext {
  const tmp = join(home, "users", "default", "sessions", sessionId, "tmp")
  mkdirSync(tmp, { recursive: true })
  return {
    user: "default",
    sessionId,
    workdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p) => join(tmp, p),
    readFile: async (p) => await Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, content) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(dirname(p), { recursive: true })
      await writeFile(p, content)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (r) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => { throw new Error("未知预置项目") },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    runNewSession: async () => ({ output: "ok", archive: { runId: "r", agents: ["x"], input: "", output: "ok", messages: [] } }),
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => null,
    waitForCapture: async () => null,
  }
}

describe("diff tool", () => {
  test("text diff returns unified output + diff block with lines", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-diff-"))
    const r = await diffTool.execute({ oldText: "a\nb\nc", newText: "a\nB\nc", language: "typescript", name: "重构", oldName: "重构前", newName: "重构后" }, ctx(home))
    expect(r.output).toContain("--- old")
    expect(r.output).toContain("@@")
    expect(r.blocks?.[0].type).toBe("diff")
    const b = r.blocks![0]
    if (b.type !== "diff") throw new Error("expected diff block")
    expect(b.language).toBe("typescript")
    expect(b.name).toBe("重构")
    expect(b.oldName).toBe("重构前")
    expect(b.newName).toBe("重构后")
    expect(b.oldText).toBe("a\nb\nc")
    expect(b.lines.map((l) => l.kind)).toEqual(["equal", "del", "add", "equal"])
    cleanup(home)
  })

  test("file diff infers language from path; title falls back to file name", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-diff-file-"))
    const c = ctx(home)
    await writeTool.execute({ path: "src/old.ts", content: "const x = 1" }, c)
    await writeTool.execute({ path: "src/new.ts", content: "const x = 2" }, c)
    const r = await diffTool.execute({ oldPath: "src/old.ts", newPath: "src/new.ts" }, c)
    expect(r.output).toContain("--- src/old.ts")
    expect(r.output).toContain("+++ src/new.ts")
    const b = r.blocks![0]
    if (b.type !== "diff") throw new Error("expected diff block")
    expect(b.language).toBe("typescript")
    // 标题默认取文件名（不含目录路径），windows/posix 分隔符均可
    expect(b.name).toBe("new.ts")
    cleanup(home)
  })

  test("missing args reports usage error", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-diff-err-"))
    const r = await diffTool.execute({}, ctx(home))
    expect(r.output).toContain("需要提供")
    expect(r.blocks).toBeUndefined()
    cleanup(home)
  })

  test("mixing paths and texts reports error", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-diff-mix-"))
    const r = await diffTool.execute({ oldText: "a", newPath: "b.ts" }, ctx(home))
    expect(r.output).toContain("混用")
    cleanup(home)
  })

  test("oversize text reports error", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-diff-big-"))
    const big = "x\n".repeat(DIFF_MAX_LINES + 10)
    const r = await diffTool.execute({ oldText: big, newText: "y" }, ctx(home))
    expect(r.output).toContain("文本过大")
    cleanup(home)
  })

  test("missing file read propagates error", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-diff-nofile-"))
    expect(diffTool.execute({ oldPath: "nope.txt", newPath: "nope2.txt" }, ctx(home))).rejects.toThrow()
    cleanup(home)
  })
})

function cleanup(home: string) {
  rmSync(home, { recursive: true, force: true })
}
