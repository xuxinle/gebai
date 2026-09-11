/**
 * 文件工作台核心服务测试（P0 契约）：
 * 目录列举与自然序、二进制判定与编码/换行探测、Range 解析、根解析（sess:/proj:/abs:）、
 * 越界与符号链接逃逸防护、ZIP 打包（UTF-8 文件名 + 系统 unzip 交叉验证）、内容搜索、编码回环。
 *
 * 这些是「查看/编辑/下载/上传」四类能力的地基：一旦破约，前端表现是白屏、乱码或写坏文件，
 * 因此与 routes 层解耦、直接对 core/fs 把契约钉死。
 */
import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, symlinkSync, rmSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import {
  collectFiles,
  decodeBuffer,
  etagOf,
  globToRegExp,
  isHiddenName,
  listDirectory,
  looksBinary,
  naturalCompare,
  parseRange,
  searchInRoot,
  statPath,
  zipPaths,
} from "./service"
import { FsError, isInside, normalizeRel, parseRootId, resolveInRoot, resolveRoot, rootCatalog, type RootContext } from "./roots"

function tmpdirWith(files: Record<string, string | Uint8Array>, dirs: string[] = []): string {
  const dir = mkdtempSync(join(tmpdir(), "gebai-fs-"))
  for (const d of dirs) mkdirSync(join(dir, d), { recursive: true })
  for (const [p, content] of Object.entries(files)) {
    const abs = join(dir, p)
    mkdirSync(join(abs, ".."), { recursive: true })
    writeFileSync(abs, content)
  }
  return dir
}

describe("core/fs 目录列举（资源管理器地基）", () => {
  test("目录优先、隐藏文件可过滤、条目带 kind/language/editable", async () => {
    const root = tmpdirWith({ "b.txt": "b", "a.ts": "export const a = 1", ".hidden": "h", "sub/c.txt": "c", "pic.png": "x" })
    try {
      const all = await listDirectory(root, "", { showHidden: false })
      expect(all.entries.map((e) => e.name)).toEqual(["sub", "a.ts", "b.txt", "pic.png"])
      expect(all.entries[0].type).toBe("dir")
      const ts = all.entries.find((e) => e.name === "a.ts")
      expect(ts?.language).toBe("typescript")
      expect(ts?.editable).toBe(true)
      // 二进制（图片）可查看但不可文本编辑
      const png = all.entries.find((e) => e.name === "pic.png")
      expect(png?.editable).toBe(false)
      const hidden = await listDirectory(root, "", { showHidden: true })
      expect(hidden.entries.map((e) => e.name)).toContain(".hidden")
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  test("排序：自然序（file2 先于 file10）、大小倒序、时间倒序", async () => {
    const root = tmpdirWith({ "file10.txt": "x".repeat(10), "file2.txt": "x", "file1.txt": "xx" })
    try {
      const byName = await listDirectory(root, "", { sort: "name" })
      expect(byName.entries.map((e) => e.name)).toEqual(["file1.txt", "file2.txt", "file10.txt"])
      const bySize = await listDirectory(root, "", { sort: "size" })
      expect(bySize.entries[0].name).toBe("file10.txt")
      expect(naturalCompare("file2", "file10")).toBeLessThan(0)
      expect(naturalCompare("a", "B")).toBeLessThan(0)
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  test("不存在的目录 404、文件当目录 400、越界 400", async () => {
    const root = tmpdirWith({ "a.txt": "a" })
    try {
      await expect(listDirectory(root, "nope", {})).rejects.toMatchObject({ status: 404 })
      await expect(listDirectory(root, "a.txt", {})).rejects.toMatchObject({ status: 400 })
      await expect(listDirectory(root, "../..", {})).rejects.toMatchObject({ status: 400 })
      expect(isHiddenName(".git")).toBe(true)
      expect(isHiddenName("git")).toBe(false)
      expect(normalizeRel("./a//b")).toBe("a/b")
      expect(normalizeRel("a/b/")).toBe("a/b")
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })
})

describe("core/fs 路径防护（写操作的安全底线）", () => {
  test("resolveInRoot 拒绝 ../ 逃逸与绝对路径（默认），根内路径正常解析", () => {
    const root = "/tmp/gebai-root"
    expect(() => resolveInRoot(root, "../../etc/passwd")).toThrow(FsError)
    expect(() => resolveInRoot(root, "/etc/passwd")).toThrow(FsError)
    expect(resolveInRoot(root, "a/b.txt")).toBe("/tmp/gebai-root/a/b.txt")
    // 显式 allowAbsolute（下载打包场景，路径来自服务端自身校验结果）
    expect(resolveInRoot(root, "/tmp/gebai-root/a.txt", { allowAbsolute: true })).toBe("/tmp/gebai-root/a.txt")
    expect(isInside(root, "/tmp/gebai-root/a")).toBe(true)
    expect(isInside(root, "/tmp/gebai-root2/a")).toBe(false)
    // 前缀相似的兄弟目录不能算「根内」（否则 /root2 会被当成 /root 的子路径）
    expect(isInside(root, "/tmp/gebai-root2")).toBe(false)
  })

  test("符号链接指向根外时被拒绝（realpath 校验，防越狱读取）", () => {
    const outside = mkdtempSync(join(tmpdir(), "gebai-out-"))
    writeFileSync(join(outside, "secret.txt"), "s")
    const root = tmpdirWith({ "a.txt": "a" })
    try {
      symlinkSync(outside, join(root, "escape"), "dir")
      // 词法上 escape/secret.txt 在根内，但 realpath 落在根外 → 直接拒绝
      expect(() => resolveInRoot(root, "escape/secret.txt")).toThrow(/symlink outside sandbox/)
      expect(() => resolveInRoot(root, "a.txt")).not.toThrow()
      expect(isInside(root, join(outside, "secret.txt"))).toBe(false)
    } finally {
      rmSync(outside, { recursive: true, force: true })
      rmSync(root, { recursive: true, force: true })
    }
  })
})

describe("core/fs 文本判定与解码（查看不乱码、保存不损坏）", () => {
  test("二进制判定：NUL 即二进制；UTF-8 中文不误判", () => {
    expect(looksBinary(new TextEncoder().encode("hello\nworld"))).toBe(false)
    expect(looksBinary(new Uint8Array([0x68, 0x00, 0x69]))).toBe(true)
    expect(looksBinary(new TextEncoder().encode("中文内容，含全角标点。"))).toBe(false)
  })

  test("解码：UTF-8/BOM、CRLF 探测、GBK 回退、UTF-16LE", () => {
    expect(decodeBuffer(new TextEncoder().encode("中文\n第二行\n"))).toMatchObject({ encoding: "utf-8", eol: "lf" })
    const bomDecoded = decodeBuffer(new Uint8Array([0xef, 0xbb, 0xbf, 0x68, 0x69]))
    expect(bomDecoded.encoding).toBe("utf-8")
    expect(bomDecoded.text).toBe("hi") // BOM 已剥离，编辑器里不出现零宽字符
    expect(decodeBuffer(new TextEncoder().encode("a\r\nb")).eol).toBe("crlf")
    // GBK 的「中文」= D6 D0 CE C4（非法 UTF-8 → 回退 GBK）
    const gbk = decodeBuffer(new Uint8Array([0xd6, 0xd0, 0xce, 0xc4]))
    expect(gbk.encoding).toBe("gbk")
    expect(gbk.text).toBe("中文")
    expect(decodeBuffer(new Uint8Array([0xff, 0xfe, 0x2d, 0x4e])).text).toBe("中")
  })

  test("GBK 文件解码 → iconv 编码回写：字节完全一致（保存不破坏原文）", async () => {
    const root = tmpdirWith({})
    const gbkBytes = new Uint8Array([0xd6, 0xd0, 0xce, 0xc4, 0x0a]) // 「中文\n」
    writeFileSync(join(root, "gbk.txt"), gbkBytes)
    try {
      const { text, encoding } = decodeBuffer(new Uint8Array(readFileSync(join(root, "gbk.txt"))))
      expect(encoding).toBe("gbk")
      const iconv = await import("iconv-lite")
      expect(Array.from(new Uint8Array(iconv.encode(text, "gbk")))).toEqual(Array.from(gbkBytes))
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })

  test("statPath：目录/文件区分，etag 随内容变化（乐观锁基础）", async () => {
    const root = tmpdirWith({ "a.txt": "abc", "d/x": "x" })
    try {
      const f = await statPath(root, "a.txt")
      expect(f).toMatchObject({ type: "file", size: 3, editable: true })
      expect(f.etag).toBe(etagOf(join(root, "a.txt"), 3, f.mtime))
      expect((await statPath(root, "d")).type).toBe("dir")
      writeFileSync(join(root, "a.txt"), "abcd")
      expect((await statPath(root, "a.txt")).etag).not.toBe(f.etag)
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })
})

describe("core/fs HTTP Range（视频/大文件拖进度条）", () => {
  test("常规/开放式/后缀式/越界收敛/不可满足/非法头", () => {
    expect(parseRange("bytes=0-99", 1000)).toEqual({ start: 0, end: 99, partial: true })
    expect(parseRange("bytes=500-", 1000)).toEqual({ start: 500, end: 999, partial: true })
    expect(parseRange("bytes=-100", 1000)).toEqual({ start: 900, end: 999, partial: true })
    expect(parseRange("bytes=900-2000", 1000)).toEqual({ start: 900, end: 999, partial: true })
    // 覆盖整个文件的请求不算 partial（响应码 200 而非 206）
    expect(parseRange("bytes=0-999", 1000)).toEqual({ start: 0, end: 999, partial: false })
    expect(parseRange("bytes=1000-", 1000)).toBe("unsatisfiable")
    expect(parseRange("bytes=abc", 1000)).toBeNull()
    expect(parseRange(null, 1000)).toBeNull()
  })
})

describe("core/fs ZIP 打包（下载文件夹）", () => {
  test("zipPaths 产出合法 ZIP，中文名以 UTF-8 存储，且能被系统 unzip 解出", async () => {
    const root = tmpdirWith({ "中文.txt": "内容", "sub/b.txt": "BBB" }, ["sub"])
    const zipPath = join(tmpdir(), `gebai-zip-${Date.now()}.zip`)
    try {
      const bytes = await zipPaths(root, ["中文.txt", "sub"], 10 * 1024 * 1024)
      expect(bytes[0]).toBe(0x50) // 'P'
      expect(bytes[1]).toBe(0x4b) // 'K'
      const needle = new TextEncoder().encode("中文.txt")
      let found = false
      for (let i = 0; i + needle.length <= bytes.length && !found; i++) found = needle.every((b, j) => bytes[i + j] === b)
      expect(found).toBe(true)

      writeFileSync(zipPath, bytes)
      const listing = Bun.spawnSync(["unzip", "-l", zipPath])
      if (listing.exitCode === 0) {
        const out = listing.stdout.toString()
        expect(out).toContain("2 files") // 目录递归打包：两个文件都在
        expect(out).toContain("sub/b.txt") // ASCII 名直接可读
        // 内容可被系统 unzip 原样解出
        expect(Bun.spawnSync(["unzip", "-p", zipPath, "sub/b.txt"]).stdout.toString()).toBe("BBB")
      }
    } finally {
      rmSync(root, { recursive: true, force: true })
      rmSync(zipPath, { force: true })
    }
  })

  test("打包目录会递归收集子文件；超过上限抛 413", async () => {
    const root = tmpdirWith({ "d/a.txt": "a", "d/e/b.txt": "b" }, ["d/e"])
    try {
      const collected = collectFiles(root, join(root, "d"), {})
      expect(collected.files.map((f) => f.name).sort()).toEqual(["d/a.txt", "d/e/b.txt"])
      // 单文件体积超上限 → 413（路由层转「请改用下载单个文件」提示）
      const big = tmpdirWith({ "big.bin": new Uint8Array(4096) })
      try {
        await expect(zipPaths(big, ["big.bin"], 1024)).rejects.toMatchObject({ status: 413 })
      } finally {
        rmSync(big, { recursive: true, force: true })
      }
      // 目录里没有任何可打包文件 → 404（而不是产出空 zip）
      await expect(zipPaths(root, ["c.ts"], 1024)).rejects.toMatchObject({ status: 404 })
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })
})

describe("core/fs 搜索（名称/内容）", () => {
  test("内容搜索命中行号与片段；glob 过滤生效", async () => {
    const root = tmpdirWith({ "a.ts": "const foo = 1\n// 查找我\n", "b.md": "查找我\n", "c.ts": "nothing\n" })
    try {
      const res = await searchInRoot(root, { query: "查找我", mode: "content", maxResults: 20 })
      expect(res.hits.some((h) => h.path === "b.md")).toBe(true)
      const tsOnly = await searchInRoot(root, { query: "查找我", mode: "content", glob: "*.ts" })
      expect(tsOnly.hits.every((h) => h.path.endsWith(".ts"))).toBe(true)
      const byName = await searchInRoot(root, { query: "^a", mode: "name", regex: true })
      expect(byName.hits.map((h) => h.path)).toContain("a.ts")
      expect(globToRegExp("*.ts").test("a.ts")).toBe(true)
      expect(globToRegExp("*.ts").test("src/a.ts")).toBe(false) // * 不跨目录（与 .gitignore 语义一致）
      expect(globToRegExp("**/*.ts").test("src/deep/a.ts")).toBe(true)
      expect(globToRegExp("src/**/*.{ts,tsx}").test("src/a.tsx")).toBe(true)
      await expect(searchInRoot(root, { query: "  " })).rejects.toMatchObject({ status: 400 })
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  })
})

describe("core/fs 根解析（sess:/proj:/bind:/user:/abs:）", () => {
  const ctx = (over: Partial<RootContext> = {}): RootContext => ({
    home: "/gebai-home",
    user: "admin",
    sandboxed: false,
    writable: true,
    projects: [{ name: "gebai", path: "/workspaces/gebai" }],
    binds: [{ agent: "code", root: "/workspaces/proj" }],
    extraRoots: [],
    sessions: [{ id: "abcdef1234567890abcdef1234567890", name: "会话甲" }],
    ...over,
  })

  test("parseRootId：合法/非法与错误提示", () => {
    expect(parseRootId("sess:abcdef1234567890abcdef1234567890")).toMatchObject({ kind: "sess" })
    expect(parseRootId("proj:gebai")).toMatchObject({ kind: "proj", id: "gebai" })
    expect(parseRootId("user:")).toMatchObject({ kind: "user" })
    expect(() => parseRootId("bogus:x")).toThrow(FsError)
    expect(() => parseRootId("sess:短")).toThrow(FsError)
  })

  test("resolveRoot：会话/项目/绑定/用户/绝对路径；沙箱下拒绝 abs:", () => {
    // 项目/绑定根的目录存在性由 ctx.isDir 注入（测试不依赖真实文件系统）
    const c = ctx({ isDir: () => true })
    // 会话目录按 id 前两段分片（ab/cd/<id>/tmp），避免单目录堆积
    expect(resolveRoot("sess:abcdef1234567890abcdef1234567890", c).abs).toBe("/gebai-home/users/admin/sessions/ab/cd/abcdef1234567890abcdef1234567890/tmp")
    expect(resolveRoot("proj:gebai", c).abs).toBe("/workspaces/gebai")
    expect(resolveRoot("bind:code", c).abs).toBe("/workspaces/proj")
    expect(resolveRoot("user:", c).abs).toBe("/gebai-home/users/admin")
    expect(resolveRoot("abs:/tmp", c).abs).toBe("/tmp")
    // 沙箱（服务模式）下绝对路径根一律拒绝
    expect(() => resolveRoot("abs:/tmp", ctx({ sandboxed: true, isDir: () => true }))).toThrow(FsError)
    expect(() => resolveRoot("proj:nope", c)).toThrow(FsError)
    // 目录不存在的项目/绑定根 → 404（前端表现为该根不可用，而非界面崩溃）
    expect(() => resolveRoot("bind:code", ctx({ isDir: () => false }))).toThrow(FsError)
  })

  test("写开关：writable=false 时根为只读（前端据此隐藏写操作）", () => {
    expect(resolveRoot("proj:gebai", ctx({ writable: false })).writable).toBe(false)
    expect(resolveRoot("proj:gebai", ctx()).writable).toBe(true)
  })

  test("rootCatalog：会话 + 项目 + 绑定 + 用户目录，按 id 去重", () => {
    const list = rootCatalog(ctx())
    const ids = list.map((r) => r.id)
    expect(ids).toContain("proj:gebai")
    expect(ids).toContain("bind:code")
    expect(ids).toContain("user:")
    expect(new Set(ids).size).toBe(ids.length)
    expect(list.find((r) => r.id === "proj:gebai")?.name).toBe("gebai")
  })
})
