import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { dirname, join, resolve } from "node:path"
import { editTool, readTool, writeTool } from "."
import { decodeTextFile, detectEncoding, dominantEol, encodeText, gbkCharOffsets, normalizeEol } from "../base/file-text"
import { sessionPath, stripTmpPrefix } from "../base/paths"
import type { ToolContext } from "../base/types"

const SID = "abcdef01abcdef01abcdef01abcdef01"

function ctx(home: string): ToolContext {
  const tmp = join(sessionPath(home, "default", SID), "tmp")
  mkdirSync(tmp, { recursive: true })
  return {
    user: "default",
    sessionId: SID,
    workdir: tmp,
    home,
    env: {},
    sandboxed: false,
    resolvePath: (p: string) => resolve(tmp, stripTmpPrefix(p)),
    readFile: async (p: string) => {
      const { readFile } = await import("node:fs/promises")
      return readFile(p, "utf8")
    },
    readBinaryFile: async (p: string) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p: string, content: string) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(dirname(p), { recursive: true })
      await writeFile(p, content)
    },
    writeBinaryFile: async (p: string, data: Uint8Array) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(dirname(p), { recursive: true })
      await writeFile(p, data)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async () => ({ stdout: "", stderr: "", code: 0 }),
    uploadAttachment: async (r: { path: string }) => r.path,
    publish: () => {},
    projects: [],
    resolveProjectPath: () => {
      throw new Error("未知预置项目")
    },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
  } as unknown as ToolContext
}

/** GBK 字节（无 iconv 依赖：直接给已知码位的中文双字节序列）。 */
const GBK_ZHONG = new Uint8Array([0xd6, 0xd0]) // 中
const GBK_WEN = new Uint8Array([0xce, 0xc4]) // 文

describe("file-text 编解码探测", () => {
  test("BOM/UTF-16/UTF-8/GBK 探测与主导行尾判定", () => {
    expect(detectEncoding(new TextEncoder().encode("abc"))?.encoding).toBe("utf-8")
    expect(detectEncoding(new Uint8Array([0xef, 0xbb, 0xbf, 0x61]))?.encoding).toBe("utf-8-bom")
    expect(detectEncoding(new Uint8Array([0xff, 0xfe, 0x61, 0x00]))?.encoding).toBe("utf-16le")
    expect(detectEncoding(new Uint8Array([0xfe, 0xff, 0x00, 0x61]))?.encoding).toBe("utf-16be")
    expect(detectEncoding(new Uint8Array([...GBK_ZHONG, ...GBK_WEN]))?.encoding).toBe("gbk")
    expect(detectEncoding(new Uint8Array([0, 1, 2, 3]))).toBeNull() // NUL 字节 → 二进制
    expect(dominantEol("a\r\nb\r\n")).toBe("\r\n")
    expect(dominantEol("a\nb\n")).toBe("\n")
    expect(dominantEol("a\r\nb\nc\nd")).toBe("\n") // 混合：孤立 LF 多 → LF
  })

  test("编码往返：BOM/UTF-16/GBK 解码后再编码字节一致（GBK 经字符偏移映射）", () => {
    const utf16 = new Uint8Array([0xff, 0xfe, 0x61, 0x00, 0x62, 0x00])
    const d = decodeTextFile(utf16)!
    expect(d.text).toBe("ab")
    expect(Array.from(encodeText(d.text, d.encoding))).toEqual(Array.from(utf16))
    const gbkBytes = new Uint8Array([0x61, ...GBK_ZHONG, ...GBK_WEN, 0x62])
    const g = decodeTextFile(gbkBytes)!
    expect(g.text).toBe("a中文b")
    expect(g.encoding).toBe("gbk")
    const offs = gbkCharOffsets(g.text)!
    expect(offs).toEqual([0, 1, 3, 5, 6])
    // GB18030 四字节字符（代理对）不可映射 → null（edit 据此拒绝而非写坏文件）
    expect(gbkCharOffsets("a\u{20000}b")).toBeNull()
  })

  test("LF 归一映射：CRLF 与裸 CR 都归一并保留源索引", () => {
    const n = normalizeEol("a\r\nb\rc\n")
    expect(n.text).toBe("a\nb\nc\n")
    expect(n.toSource).toEqual([0, 1, 3, 4, 5, 6, 7])
  })
})

describe("edit 编码与行尾健壮性", () => {
  test("GBK 文件：read 显示正文与编码注记，edit 纯 ASCII 替换后其余字节不变", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-gbk-"))
    const c = ctx(home)
    const bytes = new Uint8Array([...new TextEncoder().encode("key="), ...GBK_ZHONG, ...GBK_WEN, ...new TextEncoder().encode("\n")])
    writeFileSync(join(c.workdir, "gbk.txt"), bytes)
    const r = await readTool.execute({ path: "gbk.txt" }, c)
    expect(r.output).toContain("key=中文")
    expect(r.output).toContain("编码：GBK")
    const e = await editTool.execute({ path: "gbk.txt", edits: [{ old_string: "key=", new_string: "KEY=" }] }, c)
    expect(e.output).toContain("已对 gbk.txt（GBK）")
    const after = readFileSync(join(c.workdir, "gbk.txt"))
    expect(Array.from(after)).toEqual([...new TextEncoder().encode("KEY="), ...GBK_ZHONG, ...GBK_WEN, 10])
  })

  test("GBK 文件：非 ASCII 替换文本明确拒绝且不落盘（不再静默写坏）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-gbk2-"))
    const c = ctx(home)
    const bytes = new Uint8Array([...GBK_ZHONG, ...GBK_WEN, ...new TextEncoder().encode(" ok\n")])
    writeFileSync(join(c.workdir, "g.txt"), bytes)
    await readTool.execute({ path: "g.txt" }, c)
    await expect(editTool.execute({ path: "g.txt", edits: [{ old_string: " ok", new_string: " 中文" }] }, c)).rejects.toThrow("GBK")
    expect(Array.from(readFileSync(join(c.workdir, "g.txt")))).toEqual(Array.from(bytes))
  })

  test("UTF-16 LE 文件：解码匹配、按原编码写回（BOM 与字节序保持）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-u16-"))
    const c = ctx(home)
    writeFileSync(join(c.workdir, "u16.txt"), encodeText("hello\nworld\n", "utf-16le"))
    await readTool.execute({ path: "u16.txt" }, c)
    const e = await editTool.execute({ path: "u16.txt", edits: [{ old_string: "world", new_string: "WORLD" }] }, c)
    expect(e.output).toContain("UTF-16 LE")
    const back = decodeTextFile(new Uint8Array(readFileSync(join(c.workdir, "u16.txt"))))!
    expect(back.encoding).toBe("utf-16le")
    expect(back.text).toBe("hello\nWORLD\n")
  })

  test("混合行尾文件：未修改区域字节级保留（CRLF/LF 原样），仅替换文本按主导行尾", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-mix-"))
    const c = ctx(home)
    // 首行 CRLF、次行 LF：主导行尾 CRLF
    writeFileSync(join(c.workdir, "mix.txt"), "a\r\nb\nc\r\n")
    await readTool.execute({ path: "mix.txt" }, c)
    const e = await editTool.execute({ path: "mix.txt", edits: [{ old_string: "b", new_string: "B1\nB2" }] }, c)
    expect(e.output).toContain("已对 mix.txt")
    // 未修改的 a/c 行保留各自原行尾；替换文本按主导 CRLF
    expect(readFileSync(join(c.workdir, "mix.txt"), "utf8")).toBe("a\r\nB1\r\nB2\nc\r\n")
  })

  test("裸 CR 行尾：LF old_string 匹配、替换文本按 CR 风格落地、未修改区域保留", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-cr-"))
    const c = ctx(home)
    writeFileSync(join(c.workdir, "cr.txt"), "l1\rl2\rl3\r")
    await readTool.execute({ path: "cr.txt" }, c)
    const e = await editTool.execute({ path: "cr.txt", edits: [{ old_string: "l2", new_string: "L2" }] }, c)
    expect(e.output).toContain("已对 cr.txt")
    expect(readFileSync(join(c.workdir, "cr.txt"), "utf8")).toBe("l1\rL2\rl3\r")
  })

  test("write 对非 UTF-8 目标文件拒绝整体覆盖（防按 UTF-8 落盘破坏原编码）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-write-enc-"))
    const c = ctx(home)
    const bytes = new Uint8Array([...GBK_ZHONG, ...GBK_WEN, ...new TextEncoder().encode("\n")])
    writeFileSync(join(c.workdir, "g.txt"), bytes)
    await readTool.execute({ path: "g.txt" }, c)
    const w = await writeTool.execute({ path: "g.txt", content: "新内容\n" }, c)
    expect(w.output).toContain("GBK")
    expect(Array.from(readFileSync(join(c.workdir, "g.txt")))).toEqual(Array.from(bytes))
    // UTF-8 文件不受影响
    await writeTool.execute({ path: "u.txt", content: "a\n" }, c)
    await writeTool.execute({ path: "u.txt", content: "b\n" }, c)
    expect(readFileSync(join(c.workdir, "u.txt"), "utf8")).toBe("b\n")
  })

  test("二进制文件：edit 明确拒绝（不按 UTF-8 读成乱码后写坏）", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-bin-"))
    const c = ctx(home)
    writeFileSync(join(c.workdir, "b.bin"), new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0, 1, 2, 3]))
    const e = await editTool.execute({ path: "b.bin", edits: [{ old_string: "PNG", new_string: "png" }] }, c)
    expect(e.output).toContain("不是可识别的文本文件")
  })

  test("空白容错：old_string 与原文仅空白差异（多打空格/缩进不一致）时唯一命中自动对齐，多处命中仍报错；失配报首个分歧点", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-fuzzy-"))
    const c = ctx(home)
    // 原文：双空格 + 尾随空格；模型 old_string：单空格、无尾随（空白抄写差异）
    await writeTool.execute({ path: "f.txt", content: "const  value = 1 \nnext=2\n" }, c)
    const e = await editTool.execute({ path: "f.txt", edits: [{ old_string: "const value = 1\nnext=2", new_string: "const v = 1\nnext=3" }] }, c)
    expect(e.output).toContain("空白容错命中")
    expect(readFileSync(join(c.workdir, "f.txt"), "utf8")).toBe("const v = 1\nnext=3\n")
    // 多处空白差异命中（非唯一）→ 仍报错不盲替换：old_string 带缩进差异，两处均容错命中
    await writeTool.execute({ path: "g.txt", content: "a  b\na  b\n" }, c)
    await expect(editTool.execute({ path: "g.txt", edits: [{ old_string: "a b", new_string: "X" }] }, c)).rejects.toThrow("非唯一命中")
    // 非空白字符差异 → 报首个分歧点（长行定位）
    await writeTool.execute({ path: "h.txt", content: "这是很长的一行文字内容包含若干汉字用于定位分歧点位存在这里后面还有更多文字内容若干字" }, c)
    await expect(editTool.execute({ path: "h.txt", edits: [{ old_string: "这是很长的一行文字内容包含若干汉字用于定位分歧点位错在这里后面还有更多文字内容若干字", new_string: "X" }] }, c)).rejects.toThrow("分歧起点")
  })
})

describe("edit 正则匹配与参数健壮性", () => {
  test("pattern 替换：捕获组引用与 replace_all；old_string 与 pattern 互斥校验", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-regex-"))
    const c = ctx(home)
    await writeTool.execute({ path: "r.ts", content: "const a = foo(1)\nconst b = foo(2)\n" }, c)
    // 捕获组 + replace_all：一次改写全部调用（无需重发整段原文）
    const e = await editTool.execute(
      { path: "r.ts", edits: [{ pattern: "foo\\((\\d+)\\)", new_string: "bar($1, opts)", replace_all: true }] }, c,
    )
    expect(e.output).toContain("已对 r.ts")
    expect(readFileSync(join(c.workdir, "r.ts"), "utf8")).toBe("const a = bar(1, opts)\nconst b = bar(2, opts)\n")
    // $& 整段匹配
    const e2 = await editTool.execute({ path: "r.ts", edits: [{ pattern: "bar\\(\\d+, opts\\)", new_string: "[$&]", replace_all: true }] }, c)
    expect(e2.output).toContain("已对 r.ts")
    expect(readFileSync(join(c.workdir, "r.ts"), "utf8")).toBe("const a = [bar(1, opts)]\nconst b = [bar(2, opts)]\n")
    // 二选一：同时给 old_string 与 pattern → 拒绝
    const both = await editTool.execute({ path: "r.ts", edits: [{ old_string: "a", pattern: "a", new_string: "b" }] }, c)
    expect(both.output).toContain("只能选其一")
  })

  test("pattern 多处命中且未 replace_all 时整体失败并列行号；无匹配给出引导", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-regex2-"))
    const c = ctx(home)
    await writeTool.execute({ path: "p.txt", content: "x1\ny1\n" }, c)
    await expect(editTool.execute({ path: "p.txt", edits: [{ pattern: "\\d", new_string: "9" }] }, c)).rejects.toThrow("匹配 2 处")
    await expect(editTool.execute({ path: "p.txt", edits: [{ pattern: "zzz\\d+", new_string: "9" }] }, c)).rejects.toThrow("未在文件中匹配")
    // 零长度匹配拒绝
    await expect(editTool.execute({ path: "p.txt", edits: [{ pattern: "a*", new_string: "9", replace_all: true }] }, c)).rejects.toThrow("零长度")
    expect(readFileSync(join(c.workdir, "p.txt"), "utf8")).toBe("x1\ny1\n")
  })

  test("参数健壮性：空 edits / 非对象项 / 缺 new_string / replace_all 类型 / old==new 均拒绝且不落盘", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-args-"))
    const c = ctx(home)
    await writeTool.execute({ path: "a.txt", content: "hello\n" }, c)
    expect((await editTool.execute({ path: "a.txt", edits: [] }, c)).output).toContain("edits 为空")
    expect((await editTool.execute({ path: "a.txt", edits: ["x"] }, c)).output).toContain("不是对象")
    expect((await editTool.execute({ path: "a.txt", edits: [{ old_string: "hello" }] }, c)).output).toContain("缺少 new_string")
    expect((await editTool.execute({ path: "a.txt", edits: [{ old_string: "hello", new_string: "x", replace_all: "yes" }] }, c)).output).toContain("布尔值")
    expect((await editTool.execute({ path: "a.txt", edits: [{ old_string: "hello", new_string: "hello" }] }, c)).output).toContain("无改动")
    // 文件不存在 → 明确引导（不再抛原始 ENOENT）
    expect((await editTool.execute({ path: "nope.txt", edits: [{ old_string: "a", new_string: "b" }] }, c)).output).toContain("无法读取")
    expect(readFileSync(join(c.workdir, "a.txt"), "utf8")).toBe("hello\n")
  })

  test("多编辑项：按原文位置应用、行号回报基于原文；区间交叠时整体失败", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-multi2-"))
    const c = ctx(home)
    await writeTool.execute({ path: "m.ts", content: "aaa\nbbb\nccc\n" }, c)
    const ok = await editTool.execute(
      { path: "m.ts", edits: [{ old_string: "ccc", new_string: "CCC" }, { old_string: "aaa", new_string: "AAA" }] }, c,
    )
    expect(ok.output).toContain("1) 行 3")
    expect(ok.output).toContain("2) 行 1")
    expect(readFileSync(join(c.workdir, "m.ts"), "utf8")).toBe("AAA\nbbb\nCCC\n")
    // 与前面编辑项重叠：整体失败
    await expect(
      editTool.execute({ path: "m.ts", edits: [{ old_string: "AAA\nbbb", new_string: "X" }, { old_string: "bbb", new_string: "Y" }] }, c),
    ).rejects.toThrow("重叠")
    expect(readFileSync(join(c.workdir, "m.ts"), "utf8")).toBe("AAA\nbbb\nCCC\n")
  })

  test("pattern 匹配数超上限（1000）时拒绝：无法安全判定替换范围", async () => {
    const home = mkdtempSync(join(tmpdir(), "gebai-edit-regex3-"))
    const c = ctx(home)
    writeFileSync(join(c.workdir, "many.txt"), "x\n".repeat(1200))
    await readTool.execute({ path: "many.txt" }, c)
    await expect(editTool.execute({ path: "many.txt", edits: [{ pattern: "x", new_string: "y", replace_all: true }] }, c)).rejects.toThrow("超过")
    expect(readFileSync(join(c.workdir, "many.txt"), "utf8")).toBe("x\n".repeat(1200))
  }, 60_000)
})
