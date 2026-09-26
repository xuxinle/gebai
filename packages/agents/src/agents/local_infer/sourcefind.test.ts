/**
 * sourcefind.ts（源码发现层）测试：发现根解析、目录/归档识别（peek 不整体解压）、按 id 或路径定位、
 * 归档解压（幂等 + 下钻顶层目录）与工具链探测（各设备就绪判定 + 内网补齐提示）。
 *
 * 环境封闭：临时 GEBAI_HOME（mkdtempSync，放在系统临时目录里，**不在仓库目录写任何文件**）+
 * 现场用系统 `tar -czf` / fflate `zipSync` 造归档；平台与工具链一律以**显式注入的假 runCommand / 平台参数**
 * 断言，不随宿主漂移（只有「本机真实探测」一例走真实命令，断言的是本机确实具备的 cmake/make/gcc）。
 */
import { exec, spawnSync } from "node:child_process"
import { closeSync, existsSync, ftruncateSync, mkdirSync, mkdtempSync, openSync, readdirSync, rmSync, statSync, symlinkSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { basename, join, resolve } from "node:path"
import { afterAll, describe, expect, test } from "bun:test"
import type { ToolContext } from "@gebai/sdk"
import { strToU8, zipSync } from "fflate"
import {
  type SourceCandidate,
  detectToolchain,
  detectToolchainOn,
  discoverSources,
  ensureSourceDir,
  findSource,
  sourceRoots,
} from "./sourcefind"

const tmp = mkdtempSync(join(tmpdir(), "gebai-infer-sourcefind-"))
afterAll(() => rmSync(tmp, { recursive: true, force: true }))

const rnd = (): string => Math.random().toString(36).slice(2, 10)

interface Tree {
  root: string
  home: string
  src: string
  env: Record<string, string>
}

/**
 * 临时 GEBAI_HOME 结构：home = `<root>/gebai/infer`，资源目录在同级的 `<root>/gebai/resources/` 下
 * （与 paths.modelsDir 的 `{GEBAI_HOME}/resources/...` 口径一致）。
 */
function makeTree(): Tree {
  const root = join(tmp, `t-${rnd()}`)
  const home = join(root, "gebai", "infer")
  const src = join(root, "gebai", "resources", "src")
  mkdirSync(join(home, "config"), { recursive: true })
  mkdirSync(src, { recursive: true })
  return { root, home, src, env: {} }
}

/** 像 llama.cpp 的 CMakeLists（含 LLAMA_/GGML_ 关键标志与版本）。 */
const LLAMA_CMAKE = [
  "cmake_minimum_required(VERSION 3.14)",
  'project("llama.cpp" VERSION 0.0.1 LANGUAGES CXX)',
  "set(LLAMA_VERSION 0.0.1)",
  "add_library(llama src/main.cpp)",
  "target_compile_definitions(llama PRIVATE GGML_USE_CUDA)",
  "",
].join("\n")

/** 通用 CMakeLists（无 LLAMA_/GGML_ 标志）。 */
const PLAIN_CMAKE = ["cmake_minimum_required(VERSION 3.14)", "project(demo VERSION 1.2.3 LANGUAGES CXX)", "add_executable(demo src/main.cpp)", ""].join(
  "\n",
)

/** 造一个源码目录（按需带 CMakeLists.txt / Makefile / build.sh / vendor 依赖）。 */
function makeSourceDir(
  parent: string,
  name: string,
  o: { cmake?: string | boolean; make?: boolean; script?: boolean; vendor?: boolean } = {},
): string {
  const dir = join(parent, name)
  mkdirSync(join(dir, "src"), { recursive: true })
  if (o.cmake) writeFileSync(join(dir, "CMakeLists.txt"), typeof o.cmake === "string" ? o.cmake : PLAIN_CMAKE)
  if (o.make) writeFileSync(join(dir, "Makefile"), "all:\n\t@echo ok\n")
  if (o.script) writeFileSync(join(dir, "build.sh"), "#!/bin/sh\necho build\n")
  writeFileSync(join(dir, "src", "main.cpp"), "int main(){return 0;}\n")
  if (o.vendor) {
    mkdirSync(join(dir, "vendor", "nlohmann"), { recursive: true })
    writeFileSync(join(dir, "vendor", "nlohmann", "json.hpp"), "// vendored\n")
  }
  return dir
}

/** 像 llama.cpp 的源码目录（目录名带 b-tag，供版本线索断言）。 */
function makeLlamaDir(where: string, o: { vendor?: boolean; version?: string } = {}): string {
  return makeSourceDir(where, `llama.cpp-${o.version ?? "b11175"}`, { cmake: LLAMA_CMAKE, vendor: o.vendor })
}

/** 现场用系统 tar 造归档（真 tar：验证 peek 与解压的端到端行为）。 */
function makeTarGz(stage: string, entries: string[], out: string): void {
  mkdirSync(join(out, ".."), { recursive: true })
  const r = spawnSync("tar", ["-czf", out, "-C", stage, ...entries], { encoding: "utf-8" })
  if (r.status !== 0) throw new Error(`造 tar.gz 失败：${r.stderr}`)
}

interface CmdCall {
  cmd: string
  opts?: { timeoutMs?: number }
}

/**
 * 精简假 ToolContext：只实现本层用到的字段（runCommand）。
 * run 返回 undefined 表示「命令成功且无输出」；返回的 code 非 0 即视为该命令不可用。
 */
function makeCtx(
  home: string,
  run: (cmd: string) => { stdout?: string; stderr?: string; code?: number } | undefined = () => undefined,
  opts?: { throwOn?: RegExp },
): { ctx: ToolContext; calls: CmdCall[] } {
  const calls: CmdCall[] = []
  const workdir = join(home, "sessions", "s1", "tmp")
  mkdirSync(workdir, { recursive: true })
  const ctx = {
    user: "test",
    sessionId: "s1",
    workdir,
    sessionWorkdir: workdir,
    home,
    env: { LOCAL_INFER_HOME: home },
    sandboxed: false,
    runCommand: async (cmd: string, o?: { timeoutMs?: number }) => {
      calls.push({ cmd, opts: o })
      if (opts?.throwOn?.test(cmd)) throw new Error(`runCommand 拒绝执行：${cmd}`)
      const r = run(cmd) ?? {}
      return { stdout: r.stdout ?? "", stderr: r.stderr ?? "", code: r.code ?? 0 }
    },
  } as unknown as ToolContext
  return { ctx, calls }
}

/** 造一个「内容不是归档」的假归档（用于无效归档的 error 分支）。 */
function makeBadArchive(dir: string, name: string): string {
  const p = join(dir, name)
  writeFileSync(p, "这不是一个 tarball\n")
  return p
}

/** 真执行命令的 ctx（走 shell 管道）：验证 `tar -tzf … | head/grep …` 在本机确实能跑。 */
function makeRealCtx(home: string): { ctx: ToolContext; calls: CmdCall[] } {
  const calls: CmdCall[] = []
  const workdir = join(home, "sessions", "s1", "tmp")
  mkdirSync(workdir, { recursive: true })
  const ctx = {
    user: "test",
    sessionId: "s1",
    workdir,
    sessionWorkdir: workdir,
    home,
    env: { LOCAL_INFER_HOME: home },
    sandboxed: false,
    runCommand: async (cmd: string, o?: { timeoutMs?: number }) => {
      calls.push({ cmd, opts: o })
      return await new Promise<{ stdout: string; stderr: string; code: number }>((res) => {
        exec(cmd, { cwd: workdir, timeout: o?.timeoutMs ?? 60000, windowsHide: true, maxBuffer: 32 * 1024 * 1024 }, (err, stdout, stderr) => {
          const code = err && typeof (err as { code?: unknown }).code === "number" ? (err as { code: number }).code : err ? 1 : 0
          res({ stdout: String(stdout ?? ""), stderr: String(stderr ?? ""), code })
        })
      })
    },
  } as unknown as ToolContext
  return { ctx, calls }
}

// ── sourceRoots ───────────────────────────────────────────────────────────

describe("sourceRoots", () => {
  test("默认顺序：{GEBAI_HOME}/resources/src → {GEBAI_HOME}/resources/engines → <infer>/engine（全部绝对路径）", () => {
    const t = makeTree()
    const roots = sourceRoots(t.home, {})
    expect(roots).toEqual([
      resolve(t.home, "..", "resources", "src"),
      resolve(t.home, "..", "resources", "engines"),
      join(t.home, "engine"),
    ])
    for (const r of roots) expect(resolve(r)).toBe(r) // 已是绝对路径（本地平台口径）
    expect(roots[0]?.endsWith(join("gebai", "resources", "src"))).toBe(true)
  })

  test("LOCAL_INFER_SOURCE_DIRS 优先，`:` 与 `;` 都当分隔符，空段忽略", () => {
    const t = makeTree()
    const defaults = [resolve(t.home, "..", "resources", "src"), resolve(t.home, "..", "resources", "engines"), join(t.home, "engine")]

    const colon = sourceRoots(t.home, { LOCAL_INFER_SOURCE_DIRS: "/opt/src-a::/opt/src-b" })
    expect(colon.slice(0, 2)).toEqual(["/opt/src-a", "/opt/src-b"])
    expect(colon.slice(2)).toEqual(defaults)

    const semi = sourceRoots(t.home, { LOCAL_INFER_SOURCE_DIRS: "; /opt/src-a ;/opt/src-b;" })
    expect(semi.slice(0, 2)).toEqual(["/opt/src-a", "/opt/src-b"])
    expect(semi.slice(2)).toEqual(defaults)
  })

  test("盘符冒号不当分隔（C:\\a:D:\\b → 两条），相对路径按 cwd 解析", () => {
    const t = makeTree()
    const win = sourceRoots(t.home, { LOCAL_INFER_SOURCE_DIRS: "C:\\src-b;D:\\src-c" })
    expect(win.slice(0, 2)).toEqual(["C:\\src-b", "D:\\src-c"])
    const winColon = sourceRoots(t.home, { LOCAL_INFER_SOURCE_DIRS: "C:\\src-b:D:\\src-c" })
    expect(winColon.slice(0, 2)).toEqual(["C:\\src-b", "D:\\src-c"])
    expect(win[0]).toBe(winColon[0])

    const rel = `rel-src-${rnd()}`
    const relRoots = sourceRoots(t.home, { LOCAL_INFER_SOURCE_DIRS: rel })
    expect(relRoots[0]).toBe(resolve(process.cwd(), rel))
  })

  test("去重：与默认根重复（含尾分隔符）只保留一次", () => {
    const t = makeTree()
    const src = resolve(t.home, "..", "resources", "src")
    const roots = sourceRoots(t.home, { LOCAL_INFER_SOURCE_DIRS: `${src}:${src}/` })
    expect(roots).toEqual([src, resolve(t.home, "..", "resources", "engines"), join(t.home, "engine")])
  })
})

// ── discoverSources ───────────────────────────────────────────────────────

describe("discoverSources", () => {
  test("识别 llama.cpp 目录（含 vendor/）：kind/version/evidence/bytes/root 齐备", async () => {
    const t = makeTree()
    const dir = makeLlamaDir(t.src, { vendor: true })
    const res = await discoverSources(t.home, t.env)

    expect(res.scanned.map((s) => s.root)).toEqual(sourceRoots(t.home, t.env))
    expect(res.scanned[0]).toEqual({ root: t.src, exists: true, entries: 1 })
    expect(res.scanned[2]).toEqual({ root: join(t.home, "engine"), exists: false, entries: 0 })

    const c = res.candidates.find((x) => x.id === "llama.cpp-b11175")
    expect(c).toBeDefined()
    expect(c?.kind).toBe("llama_cpp")
    expect(c?.path).toBe(dir)
    expect(c?.root).toBe(t.src)
    expect(c?.version).toBe("b11175")
    expect(c?.archive).toBeUndefined()
    expect(c?.bytes).toBeGreaterThan(0)
    const ev = (c?.evidence ?? []).join(" | ")
    expect(ev).toContain("CMakeLists.txt 含 LLAMA_/GGML_ 关键标志")
    expect(ev).toContain("目录名匹配 /llama[._-]?cpp/i")
    expect(ev).toContain("含 vendor/ 依赖目录（1 项）——离线可编译")
    expect(ev).toContain("版本线索：b11175")
  })

  test("llama.cpp 目录缺 vendor/ 时 evidence 明确提示离线风险（不整体读源码）", async () => {
    const t = makeTree()
    makeLlamaDir(t.src, { vendor: false, version: "b99999" })
    const res = await discoverSources(t.home, t.env)
    const c = res.candidates.find((x) => x.id === "llama.cpp-b99999")
    expect(c?.version).toBe("b99999")
    expect((c?.evidence ?? []).join(" | ")).toContain("未含 vendor/ 依赖——离线编译可能因 FetchContent 联网失败（需完整源码包）")
  })

  test("通用 cmake（与 Makefile 并存按 cmake 优先）/ make / script / unknown 各就各位", async () => {
    const t = makeTree()
    makeSourceDir(t.src, "plain-cmake", { cmake: true, make: true })
    makeSourceDir(t.src, "plain-make", { make: true })
    makeSourceDir(t.src, "plain-script", { script: true })
    makeSourceDir(t.src, "plain-unknown")
    writeFileSync(join(t.src, "README.md"), "# 不是候选\n") // 普通文件：既不报错也不当候选

    const res = await discoverSources(t.home, t.env)
    const by = (id: string): SourceCandidate | undefined => res.candidates.find((c) => c.id === id)

    expect(by("plain-cmake")?.kind).toBe("cmake")
    expect((by("plain-cmake")?.evidence ?? []).join(" | ")).toContain("Makefile")
    expect((by("plain-cmake")?.evidence ?? []).join(" | ")).toContain("按 cmake 优先")
    expect(by("plain-make")?.kind).toBe("make")
    expect(by("plain-script")?.kind).toBe("script")
    expect(by("plain-unknown")?.kind).toBe("unknown")
    expect((by("plain-unknown")?.evidence ?? []).join(" | ")).toContain("无 CMakeLists.txt/Makefile/build.sh")
    expect(res.candidates.map((c) => c.id)).not.toContain("README.md")
    expect(res.scanned[0]?.entries).toBe(4)
  })

  test("tar.gz 归档：peek 出条目数/顶层/CMakeLists/vendor 依赖（不整体解压），kind=llama_cpp", async () => {
    const t = makeTree()
    const stage = join(t.root, "stage")
    makeSourceDir(stage, "llama.cpp-x", { cmake: LLAMA_CMAKE, vendor: true })
    const tgz = join(t.src, "llama.cpp-x.tar.gz")
    makeTarGz(stage, ["llama.cpp-x"], tgz)

    const res = await discoverSources(t.home, t.env)
    const c = res.candidates.find((x) => x.id === "llama.cpp-x")
    expect(c?.kind).toBe("llama_cpp")
    expect(c?.path).toBe(tgz)
    expect(c?.root).toBe(t.src)
    expect(c?.archive).toEqual({ path: tgz, format: "tar.gz", bytes: statSync(tgz).size })
    expect(c?.peek?.has_cmake).toBe(true)
    expect(c?.peek?.has_vendor_deps).toBe(true)
    expect(c?.peek?.top_level).toEqual(["llama.cpp-x"])
    expect(c?.peek?.entries).toBeGreaterThanOrEqual(4)
    expect((c?.peek?.notes ?? []).join(" | ")).toContain("归档内含 CMakeLists.txt")
    // 归档候选未被解压：发现根本身只有一个归档文件
    expect(readdirSync(t.src)).toEqual(["llama.cpp-x.tar.gz"])
  })

  test("归档缺 vendor：notes 明确写「离线编译需含 vendor 的源码包」；顶层含 ggml/ 判为 llama_cpp", async () => {
    const t = makeTree()
    const stage = join(t.root, "stage-novendor")
    mkdirSync(join(stage, "ggml"), { recursive: true })
    writeFileSync(join(stage, "CMakeLists.txt"), LLAMA_CMAKE)
    writeFileSync(join(stage, "ggml", "ggml.c"), "// ggml\n")
    const tgz = join(t.src, "snapshot.tar.gz")
    makeTarGz(stage, ["CMakeLists.txt", "ggml"], tgz)

    const res = await discoverSources(t.home, t.env)
    const c = res.candidates.find((x) => x.id === "snapshot")
    expect(c?.kind).toBe("llama_cpp")
    expect((c?.evidence ?? []).join(" | ")).toContain("顶层含 ggml/ 或 src/")
    expect(c?.peek?.has_cmake).toBe(true)
    expect(c?.peek?.has_vendor_deps).toBe(false)
    expect((c?.peek?.notes ?? []).join(" | ")).toContain("归档内未发现 vendor/ 依赖目录：离线编译需用包含 vendor 的源码包")
    expect((c?.peek?.notes ?? []).join(" | ")).toContain("git clone --recursive")
  })

  test("zip 归档用 fflate 只读目录（不解压），vendor 判定同样生效", async () => {
    const t = makeTree()
    const zip = join(t.src, "llama.cpp-zip.zip")
    writeFileSync(
      zip,
      zipSync({
        "llama.cpp-zip/CMakeLists.txt": strToU8(LLAMA_CMAKE),
        "llama.cpp-zip/vendor/nlohmann/json.hpp": strToU8("// vendored\n"),
        "llama.cpp-zip/src/main.cpp": strToU8("int main(){return 0;}\n"),
      }),
    )
    const res = await discoverSources(t.home, t.env)
    const c = res.candidates.find((x) => x.id === "llama.cpp-zip")
    expect(c?.kind).toBe("llama_cpp")
    expect(c?.archive?.format).toBe("zip")
    expect(c?.peek?.entries).toBe(3)
    expect(c?.peek?.top_level).toEqual(["llama.cpp-zip"])
    expect(c?.peek?.has_cmake).toBe(true)
    expect(c?.peek?.has_vendor_deps).toBe(true)
    expect(readdirSync(t.src)).toEqual(["llama.cpp-zip.zip"])
  })

  test("大归档（>128MB）不预览：notes 说明且 peek 为空（避免整包读入内存）", async () => {
    const t = makeTree()
    const big = join(t.src, "huge-source.tar.gz")
    const fd = openSync(big, "w")
    try {
      ftruncateSync(fd, 200 * 1024 * 1024) // 稀疏文件：st_size 200MB，实际不占盘
    } finally {
      closeSync(fd)
    }
    const res = await discoverSources(t.home, t.env)
    const c = res.candidates.find((x) => x.id === "huge-source")
    expect(c?.archive?.bytes).toBe(200 * 1024 * 1024)
    expect(c?.peek).toBeUndefined()
    expect((c?.evidence ?? []).join(" | ")).toContain("未预览（体积超限）")
    expect(res.notes.join("\n")).toContain("归档较大")
  })

  test("tar 不可用（ctx 报告命令缺失）→ 跳过 peek 并写进 notes，命令带引号走 ctx.runCommand", async () => {
    const t = makeTree()
    const stage = join(t.root, "stage-space")
    makeSourceDir(stage, "with space-src", { cmake: true })
    const tgz = join(t.src, "with space-src.tar.gz")
    makeTarGz(stage, ["with space-src"], tgz)

    const { ctx, calls } = makeCtx(t.home, (cmd) => (/^tar -t/.test(cmd) ? { stdout: "", stderr: "tar: not found", code: 127 } : undefined))
    const res = await discoverSources(t.home, t.env, { ctx })
    const c = res.candidates.find((x) => x.id === "with-space-src")
    expect(c).toBeDefined()
    expect(c?.peek).toBeUndefined()
    expect(res.notes.join("\n")).toContain("系统 tar 不可用")
    const tarCmd = calls.map((x) => x.cmd).find((x) => x.startsWith("tar -t"))
    expect(tarCmd).toBeDefined()
    expect(tarCmd).toContain('tar -tzf "') // 含空格的路径加引号
    // sample 模式的截断：POSIX 经 head；Windows 的 bsdtar 同名可用但未必有 head，故不加管道（见 listTarEntries）
    if (process.platform === "win32") expect(tarCmd).not.toContain("head -n")
    else expect(tarCmd).toContain("head -n")
  })

  test("条目数远超采样上限时 vendor 判定走全量过滤（真实 tar 管道，不被 head 截断成假阴性）", async () => {
    const t = makeTree()
    const stage = join(t.root, "stage-big")
    const dir = join(stage, "big-src")
    mkdirSync(join(dir, "filler"), { recursive: true })
    writeFileSync(join(dir, "CMakeLists.txt"), LLAMA_CMAKE)
    for (let i = 0; i < 2100; i++) writeFileSync(join(dir, "filler", `f${String(i).padStart(4, "0")}.txt`), "x\n")
    // vendor/ 故意最后落盘：真实 llama.cpp 归档里它同样出现在条目列表末尾（采样截断之后）
    mkdirSync(join(dir, "vendor", "nlohmann"), { recursive: true })
    writeFileSync(join(dir, "vendor", "nlohmann", "json.hpp"), "// vendored\n")
    const tgz = join(t.src, "big-src.tar.gz")
    makeTarGz(stage, ["big-src"], tgz)

    const { ctx, calls } = makeRealCtx(t.home)
    const res = await discoverSources(t.home, t.env, { ctx })
    const c = res.candidates.find((x) => x.id === "big-src")
    // 采样：POSIX 经 head 截断到上限；Windows 无 head，列出全量条目
    if (process.platform === "win32") expect(c?.peek?.entries).toBeGreaterThan(2000)
    else expect(c?.peek?.entries).toBe(2000)
    // 关键判定走**全量**过滤（不被截断成假阴性）：POSIX 用 grep、Windows 用 findstr
    const filterCmd = process.platform === "win32" ? "findstr" : "grep -aE"
    expect(calls.some((cmd) => cmd.cmd.includes(filterCmd))).toBe(true)
    expect(c?.peek?.has_cmake).toBe(true)
    expect(c?.peek?.has_vendor_deps).toBe(true)
    const pn = (c?.peek?.notes ?? []).join(" | ")
    // 这条只在「采样被截断、事后再全量修正判定」时出现：POSIX 走 head 会截断；
    // Windows 的 sample 本就是全量，无需修正
    if (process.platform === "win32") expect(pn).not.toContain("关键判定已全量过滤")
    else expect(pn).toContain("关键判定已全量过滤")
    expect(pn).not.toContain("未发现 vendor/ 依赖目录")
    expect((c?.evidence ?? []).join(" | ")).toContain("归档内含 vendor/ 依赖")
  })

  test("异常一律降级为 notes：根不是目录、根不存在、断链条目（不抛错）", async () => {
    const t = makeTree()
    const notADir = join(t.root, "not-a-dir.txt")
    writeFileSync(notADir, "x\n")
    const missingRoot = join(t.root, "no-such", "src")
    makeLlamaDir(t.src, { vendor: true })
    let symlinked = true
    try {
      symlinkSync(join(t.src, "missing-target"), join(t.src, "broken-link"))
    } catch {
      symlinked = false // 无权限建符号链接（部分 Windows 环境）：该分支跳过
    }

    const res = await discoverSources(t.home, { LOCAL_INFER_SOURCE_DIRS: `${notADir}:${missingRoot}` })
    const notes = res.notes.join("\n")
    expect(notes).toContain("发现根不是目录")
    if (symlinked) expect(notes).toContain("条目不可读")
    expect(res.scanned.find((s) => s.root === notADir)).toEqual({ root: notADir, exists: true, entries: 0 })
    expect(res.scanned.find((s) => s.root === missingRoot)).toEqual({ root: missingRoot, exists: false, entries: 0 })
    expect(res.candidates.map((c) => c.id)).toContain("llama.cpp-b11175")
  })

  test("opts.ids 只返回指定候选，未命中的 id 写进 notes", async () => {
    const t = makeTree()
    makeLlamaDir(t.src, { vendor: true })
    makeSourceDir(t.src, "plain-make", { make: true })
    const res = await discoverSources(t.home, t.env, { ids: ["plain-make", "no-such-id"] })
    expect(res.candidates.map((c) => c.id)).toEqual(["plain-make"])
    expect(res.notes.join("\n")).toContain("no-such-id")
  })
})

// ── findSource ────────────────────────────────────────────────────────────

describe("findSource", () => {
  test("按 id 命中已发现候选（目录与归档都行）", async () => {
    const t = makeTree()
    const dir = makeLlamaDir(t.src, { vendor: true })
    const stage = join(t.root, "stage-find")
    makeSourceDir(stage, "pack-src", { cmake: true })
    const tgz = join(t.src, "pack-src.tar.gz")
    makeTarGz(stage, ["pack-src"], tgz)

    const byDir = await findSource(t.home, t.env, "llama.cpp-b11175")
    expect(byDir.error).toBeUndefined()
    expect(byDir.candidate?.path).toBe(dir)
    expect(byDir.candidate?.kind).toBe("llama_cpp")

    const byArc = await findSource(t.home, t.env, "pack-src")
    expect(byArc.candidate?.path).toBe(tgz)
    expect(byArc.candidate?.archive?.format).toBe("tar.gz")
    expect(byArc.candidate?.peek?.has_cmake).toBe(true)
  })

  test("按路径就地识别（目录与归档，不要求位于发现根内；相对路径按 ctx.workdir）", async () => {
    const t = makeTree()
    const outside = join(t.root, "elsewhere", "deep")
    const dir = makeSourceDir(outside, "some-cmake", { cmake: true })
    const r1 = await findSource(t.home, t.env, dir)
    expect(r1.error).toBeUndefined()
    expect(r1.candidate?.id).toBe("some-cmake")
    expect(r1.candidate?.kind).toBe("cmake")
    expect(r1.candidate?.root).toBe(outside) // 就地识别：发现根记为所在目录

    const stage = join(t.root, "stage-loc")
    makeSourceDir(stage, "loc-src", { cmake: LLAMA_CMAKE })
    const tgz = join(outside, "loc-src.tar.gz")
    makeTarGz(stage, ["loc-src"], tgz)
    const r2 = await findSource(t.home, t.env, tgz)
    expect(r2.candidate?.archive?.path).toBe(tgz)
    expect(r2.candidate?.peek?.has_cmake).toBe(true)

    const { ctx } = makeCtx(t.home)
    makeSourceDir(ctx.workdir, "rel-cmake", { cmake: true })
    const r3 = await findSource(t.home, t.env, "rel-cmake", { ctx })
    expect(r3.error).toBeUndefined()
    expect(r3.candidate?.path).toBe(join(ctx.workdir, "rel-cmake"))
  })

  test("未命中：给出可用 id 清单与「源码该放哪儿」的建议", async () => {
    const t = makeTree()
    makeLlamaDir(t.src, { vendor: true })
    makeSourceDir(t.src, "plain-make", { make: true })
    const r = await findSource(t.home, t.env, "no-such")
    expect(r.candidate).toBeUndefined()
    expect(r.error).toContain("llama.cpp-b11175")
    expect(r.error).toContain("plain-make")
    expect(r.error).toContain("{GEBAI_HOME}/resources/src/")
    expect(r.error).toContain("LOCAL_INFER_SOURCE_DIRS")
  })

  test("无任何源码时也给出建议；URL / 空 ref / 普通文件都返回可操作 error", async () => {
    const t = makeTree()
    const empty = await findSource(t.home, t.env, "whatever")
    expect(empty.error).toContain("当前未发现任何源码")
    expect(empty.error).toContain("{GEBAI_HOME}/resources/src/")

    const url = await findSource(t.home, t.env, "https://github.com/ggml-org/llama.cpp/archive/refs/tags/b11175.tar.gz")
    expect(url.candidate).toBeUndefined()
    expect(url.error).toContain("不支持 URL")

    const blank = await findSource(t.home, t.env, "   ")
    expect(blank.error).toContain("需要源码 id")

    const txt = join(t.root, "notes.txt")
    writeFileSync(txt, "hello\n")
    const file = await findSource(t.home, t.env, txt)
    expect(file.candidate).toBeUndefined()
    expect(file.error).toContain("既不是目录，也不是")
  })
})

// ── ensureSourceDir ───────────────────────────────────────────────────────

describe("ensureSourceDir", () => {
  test("目录候选直接返回（extracted:false）；缺构建入口的目录给 error", async () => {
    const t = makeTree()
    const dir = makeLlamaDir(t.src, { vendor: true })
    const c = (await findSource(t.home, t.env, "llama.cpp-b11175")).candidate as SourceCandidate
    const r = await ensureSourceDir(t.home, c)
    expect(r).toEqual({ dir, extracted: false })

    const emptyDir = join(t.src, "just-files")
    mkdirSync(emptyDir, { recursive: true })
    writeFileSync(join(emptyDir, "notes.md"), "x\n")
    const bad = await ensureSourceDir(t.home, { id: "just-files", kind: "unknown", path: emptyDir, root: t.src, evidence: [] })
    expect(bad.dir).toBeUndefined()
    expect(bad.error).toContain("无可识别的构建入口")

    const gone = await ensureSourceDir(t.home, { id: "gone", kind: "unknown", path: join(t.src, "no-such-dir"), root: t.src, evidence: [] })
    expect(gone.error).toContain("源码目录不存在")
  })

  test("tar.gz 归档：解压到 vendor/.cache/src/<id> 并下钻顶层目录（幂等重入复用）", async () => {
    const t = makeTree()
    const stage = join(t.root, "stage-unpack")
    makeSourceDir(stage, "llama.cpp-x", { cmake: LLAMA_CMAKE, vendor: true })
    const tgz = join(t.src, "llama.cpp-x.tar.gz")
    makeTarGz(stage, ["llama.cpp-x"], tgz)

    const c = (await findSource(t.home, t.env, "llama.cpp-x")).candidate as SourceCandidate
    const cache = join(t.home, "vendor", ".cache", "src", c.id)
    const r1 = await ensureSourceDir(t.home, c)
    expect(r1.error).toBeUndefined()
    expect(r1.extracted).toBe(true)
    expect(r1.dir).toBe(join(cache, "llama.cpp-x")) // 自动下钻一层
    expect(basename(r1.dir as string)).toBe("llama.cpp-x")
    expect(existsSync(join(r1.dir as string, "CMakeLists.txt"))).toBe(true)
    expect(existsSync(join(r1.dir as string, "vendor", "nlohmann", "json.hpp"))).toBe(true)
    // 暂存目录已归位：落点上没有 .part 残留
    expect(readdirSync(join(t.home, "vendor", ".cache", "src")).filter((n) => n.includes(".part"))).toEqual([])

    const r2 = await ensureSourceDir(t.home, c) // 幂等：复用已解压结果
    expect(r2).toEqual({ dir: r1.dir, extracted: false })

    const custom = join(t.root, "custom-work")
    const r3 = await ensureSourceDir(t.home, c, { workDir: custom })
    expect(r3.extracted).toBe(true)
    expect(r3.dir).toBe(join(custom, "llama.cpp-x"))
  })

  test("zip 归档：fflate 解压 + 下钻 + 幂等", async () => {
    const t = makeTree()
    const zip = join(t.src, "llama.cpp-zip.zip")
    writeFileSync(
      zip,
      zipSync({
        "llama.cpp-zip/CMakeLists.txt": strToU8(LLAMA_CMAKE),
        "llama.cpp-zip/vendor/nlohmann/json.hpp": strToU8("// vendored\n"),
        "../escape.txt": strToU8("bad\n"), // 路径穿越条目：应被丢弃
      }),
    )
    const c = (await findSource(t.home, t.env, "llama.cpp-zip")).candidate as SourceCandidate
    const r1 = await ensureSourceDir(t.home, c)
    expect(r1.extracted).toBe(true)
    expect(basename(r1.dir as string)).toBe("llama.cpp-zip")
    expect(existsSync(join(r1.dir as string, "CMakeLists.txt"))).toBe(true)
    expect(existsSync(join(t.home, "vendor", ".cache", "src", "escape.txt"))).toBe(false)

    const r2 = await ensureSourceDir(t.home, c)
    expect(r2).toEqual({ dir: r1.dir, extracted: false })
  })

  test("归档无效：tar 非 0 退出 / zip 不可读都返回 error，且清理暂存目录", async () => {
    const t = makeTree()
    const bad = makeBadArchive(t.src, "bad-src.tar.gz")
    const c: SourceCandidate = {
      id: "bad-src",
      kind: "unknown",
      path: bad,
      root: t.src,
      archive: { path: bad, format: "tar.gz", bytes: statSync(bad).size },
      evidence: [],
    }
    const r = await ensureSourceDir(t.home, c)
    expect(r.dir).toBeUndefined()
    expect(r.error).toContain("解压失败")
    expect(r.error).toContain("bad-src.tar.gz")

    const badZip = makeBadArchive(t.src, "bad-src.zip")
    const cz: SourceCandidate = {
      id: "bad-src-zip",
      kind: "unknown",
      path: badZip,
      root: t.src,
      archive: { path: badZip, format: "zip", bytes: statSync(badZip).size },
      evidence: [],
    }
    const rz = await ensureSourceDir(t.home, cz)
    expect(rz.error).toContain("zip 不可读或损坏")

    const cacheRoot = join(t.home, "vendor", ".cache", "src")
    expect(existsSync(cacheRoot)).toBe(true)
    expect(readdirSync(cacheRoot).filter((n) => n.includes(".part"))).toEqual([])

    const gone = await ensureSourceDir(t.home, { ...c, archive: { path: join(t.src, "nope.tar.gz"), format: "tar.gz", bytes: 1 } })
    expect(gone.error).toContain("归档不存在")
  })

  test("解压走 ctx.runCommand（假 ctx 报失败时如实给 error，不假装成功）", async () => {
    const t = makeTree()
    const stage = join(t.root, "stage-ctx")
    makeSourceDir(stage, "ctx-src", { cmake: true })
    const tgz = join(t.src, "ctx-src.tar.gz")
    makeTarGz(stage, ["ctx-src"], tgz)
    const c = (await findSource(t.home, t.env, "ctx-src")).candidate as SourceCandidate

    const { ctx, calls } = makeCtx(t.home, (cmd) => (/^tar -x/.test(cmd) ? { stdout: "", stderr: "tar: Error is not recoverable", code: 2 } : undefined))
    const r = await ensureSourceDir(t.home, c, { ctx })
    expect(r.dir).toBeUndefined()
    expect(r.error).toContain("tar 退出码 2")
    expect(calls.some((x) => /^tar -xzf /.test(x.cmd))).toBe(true)
    expect(calls.some((x) => x.opts?.timeoutMs === 600000)).toBe(true) // 解压用长超时
  })
})

// ── detectToolchain ───────────────────────────────────────────────────────

describe("detectToolchain", () => {
  test("本机真实探测：结构完整、如实报告缺口（不抛错）", async () => {
    const info = await detectToolchain()
    // 结构完整：五个设备后端都有就绪判定与缺口/说明
    for (const name of ["cpu", "cuda", "vulkan", "metal", "rocm"]) {
      expect(info.devices[name]).toBeDefined()
      expect(Array.isArray(info.devices[name]?.missing)).toBe(true)
      expect(Array.isArray(info.devices[name]?.notes)).toBe(true)
    }
    expect(Array.isArray(info.offline_hints)).toBe(true)
    expect(info.offline_hints.length).toBeGreaterThan(0)
    // 探到的工具给版本描述（不凭空编造；宿主没装就是 undefined——探测的职责是如实报告，
    // 而不是保证齐备：Windows 未装 MSVC/MinGW 时 cc 缺失是完全正常的状态）
    if (info.cmake !== undefined) expect(info.cmake).toContain("cmake")
    if (info.cc !== undefined) expect(typeof info.cc).toBe("string")
    // cpu 就绪 ⇔ 三项齐备（cmake + ninja/make + cc）：两个字段必须自洽
    const cpu = info.devices.cpu!
    if (cpu.ready) expect(cpu.missing).toEqual([])
    else expect(cpu.missing.length).toBeGreaterThan(0)
    // 缺什么必须在 offline_hints 里有对应的补齐办法（不只说缺，还要说怎么办）
    if (!info.cc) expect(info.offline_hints.join(" ")).toContain("编译器")
    if (!info.cmake) expect(info.offline_hints.join(" ")).toContain("cmake")
    // 非 macOS 宿主上 Metal 后端不可用
    if (process.platform !== "darwin") expect(info.devices.metal?.ready).toBe(false)
  })

  test("假 runCommand 模拟工具全缺失：cpu 不就绪、cuda 缺 nvcc、hints 给内网补齐办法", async () => {
    const t = makeTree()
    const { ctx } = makeCtx(t.home, () => ({ stdout: "", stderr: "", code: 127 }))
    const info = await detectToolchainOn(ctx, "linux")

    expect(info.cmake).toBeUndefined()
    expect(info.nvcc).toBeUndefined()
    expect(info.devices.cpu?.ready).toBe(false)
    expect(info.devices.cpu?.missing).toEqual(["cmake", "ninja 或 make", "cc"])
    expect(info.devices.cuda?.ready).toBe(false)
    expect(info.devices.cuda?.missing).toContain("nvcc")
    expect(info.devices.cuda?.notes.join(" | ")).toContain("CUDA 编译需 nvcc（CUDA Toolkit）")
    expect(info.devices.rocm?.missing).toContain("hipcc")
    expect(info.devices.metal?.ready).toBe(false)
    expect(info.offline_hints.length).toBeGreaterThan(0)
    const hints = info.offline_hints.join("\n")
    expect(hints).toContain("cmake 缺失")
    expect(hints).toContain("resources/toolchain/")
    expect(hints).toContain("vendor/ 依赖")
    expect(hints).toContain("offline=true")
  })

  test("平台分支显式注入：darwin 需 cmake+xcrun 才 ready；非 darwin 写「仅 macOS 支持」", async () => {
    const t = makeTree()
    const { ctx: allOk } = makeCtx(t.home, () => ({ stdout: "tool 1.0.0\n", stderr: "", code: 0 }))
    const darwin = await detectToolchainOn(allOk, "darwin")
    expect(darwin.devices.cpu?.ready).toBe(true)
    expect(darwin.devices.metal?.ready).toBe(true)
    expect(darwin.devices.metal?.missing).toEqual([])
    expect(darwin.devices.metal?.notes.join(" | ")).toContain("xcrun")

    const linux = await detectToolchainOn(allOk, "linux")
    expect(linux.devices.metal?.ready).toBe(false)
    expect(linux.devices.metal?.notes.join(" | ")).toContain("仅 macOS 支持")
    expect(linux.devices.metal?.notes.join(" | ")).toContain("linux")

    const { ctx: noXcrun } = makeCtx(t.home, (cmd) => (/\bxcrun\b/.test(cmd) ? { stdout: "", stderr: "", code: 127 } : { stdout: "tool 1.0.0\n", stderr: "", code: 0 }))
    const macNoXcode = await detectToolchainOn(noXcrun, "darwin")
    expect(macNoXcode.devices.cpu?.ready).toBe(true)
    expect(macNoXcode.devices.metal?.ready).toBe(false)
    expect(macNoXcode.devices.metal?.missing).toContain("xcrun")
    expect(macNoXcode.devices.metal?.notes.join(" | ")).toContain("xcode-select --install")
  })

  test("win32 平台：cc/cxx 换 cl/gcc 兼容探测（版本取 --version 首行）", async () => {
    const t = makeTree()
    const { ctx, calls } = makeCtx(t.home, (cmd) =>
      /^cmake --version/.test(cmd) ? { stdout: "cmake version 3.30.0\r\n其它行\r\n", stderr: "", code: 0 } : { stdout: "", stderr: "", code: 127 },
    )
    const info = await detectToolchainOn(ctx, "win32")
    expect(info.cmake).toBe("cmake version 3.30.0")
    expect(calls.some((c) => /^cl --version/.test(c.cmd))).toBe(true)
    expect(calls.some((c) => /^gcc --version/.test(c.cmd))).toBe(true)
    expect(calls.some((c) => /^where /.test(c.cmd))).toBe(true) // 版本探不到时退回 where 探存在性
    expect(info.devices.cpu?.ready).toBe(false)
    expect(info.devices.cpu?.missing).toContain("ninja 或 make")
  })

  test("runCommand 抛错 / 无可用平台命令时不抛异常，如实判缺失", async () => {
    const t = makeTree()
    const { ctx } = makeCtx(t.home, () => undefined, { throwOn: /.*/ })
    const info = await detectToolchainOn(ctx, "linux")
    expect(info.cmake).toBeUndefined()
    expect(info.devices.cpu?.ready).toBe(false)
    expect(info.devices.cpu?.missing).toContain("cmake")
    expect(info.offline_hints.length).toBeGreaterThan(0)

    // 环境变量注入：win32 下 VULKAN_SDK 决定 vulkan 后端是否就绪（不依赖宿主是否装了 SDK）
    const { ctx: allOk } = makeCtx(t.home, () => ({ stdout: "1.0\n", stderr: "", code: 0 }))
    const withSdk = await detectToolchainOn(allOk, "win32", { VULKAN_SDK: "C:\\VulkanSDK\\1.3.290.0" })
    expect(withSdk.devices.vulkan?.ready).toBe(true)
    const noSdk = await detectToolchainOn(allOk, "win32", {})
    expect(noSdk.devices.vulkan?.ready).toBe(false)
    expect(noSdk.devices.vulkan?.notes.join(" | ")).toContain("VULKAN_SDK")
  })
})
