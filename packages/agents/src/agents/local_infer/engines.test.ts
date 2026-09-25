/**
 * engines.ts（跨平台引擎层）测试：矩阵读取与地址拼接、平台/设备探测、安装状态与引擎视图、
 * 下载（断点续传 / 大小与 sha256 校验 / 原子落盘 / 失败清理）、引擎安装（解压 + 递归定位 exe +
 * 标记 + 幂等）以及三个工具（engines / engine_fetch / model_fetch）的契约。
 *
 * 环境封闭：临时 infer home（mkdtempSync）+ 本地 HTTP 服务（Bun.serve，随机端口）——**不访问外网**；
 * 平台与设备探测一律以注入参数或假 runCommand 断言，不随宿主平台漂移；一切落盘都在系统临时目录内，
 * 不在仓库目录写任何文件。
 */
import { spawnSync } from "node:child_process"
import { createHash } from "node:crypto"
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { basename, join } from "node:path"
import { afterAll, describe, expect, test } from "bun:test"
import type { ToolContext } from "@gebai/sdk"
import { strToU8, zipSync } from "fflate"
import { engineDir, inferHome, loadProfiles, profilesPath, vendorDir } from "./paths"
import {
  type EngineDef,
  type EngineMatrix,
  MODEL_PRESETS,
  cudartAssetUrl,
  detectDevices,
  detectPlatform,
  downloadFile,
  engineAssetUrl,
  findEngine,
  findLlamaServer,
  installEngine,
  installedEngine,
  listEngineViews,
  loadEngineMatrix,
  pickEngineForPlatform,
  requiresApproval,
  tools,
} from "./engines"

const tmp = mkdtempSync(join(tmpdir(), "gebai-infer-engines-"))
afterAll(() => rmSync(tmp, { recursive: true, force: true }))

const rnd = (): string => Math.random().toString(36).slice(2, 10)

/** 测试用矩阵（不含真实下载源；真实矩阵另有断言，见「仓库内真实矩阵」用例）。 */
const TEST_MATRIX: EngineMatrix = {
  version: 1,
  release: { llama_cpp: { tag: "b11175", base: "https://example.invalid/download" } },
  engines: [
    { id: "linux-cpu-x64", platform: "linux", arch: "x64", device: "cpu", asset: "linux-cpu.tar.gz", archive: "tar.gz", notes: "无 GPU 默认" },
    {
      id: "linux-cuda-12.8-x64",
      platform: "linux",
      arch: "x64",
      device: "cuda",
      asset: "linux-cuda.tar.gz",
      archive: "tar.gz",
      cudart_asset: "cudart-linux.tar.gz",
      requires: "NVIDIA 驱动 ≥ 570",
    },
    { id: "linux-vulkan-x64", platform: "linux", arch: "x64", device: "vulkan", asset: "linux-vulkan.tar.gz", archive: "tar.gz" },
    { id: "win-cuda-12.4-x64", platform: "win32", arch: "x64", device: "cuda", asset: "win-cuda.zip", archive: "zip" },
    { id: "macos-metal-arm64", platform: "darwin", arch: "arm64", device: "metal", asset: "macos.tar.gz", archive: "tar.gz" },
  ],
}

/** 临时子项目根（config/engines.json 可选写入）。 */
function makeHome(matrix: EngineMatrix | null = TEST_MATRIX): string {
  const home = join(tmp, `home-${rnd()}`)
  mkdirSync(join(home, "config"), { recursive: true })
  if (matrix) writeFileSync(join(home, "config", "engines.json"), JSON.stringify(matrix, null, 2))
  return home
}

/** 写入矩阵并复用给定的下载根（把 asset 指向本地假发行版服务器）。 */
function writeMatrix(home: string, engines: EngineDef[], base: string, tag = "b11175"): EngineMatrix {
  const matrix: EngineMatrix = { version: 1, release: { llama_cpp: { tag, base } }, engines }
  writeFileSync(join(home, "config", "engines.json"), JSON.stringify(matrix, null, 2))
  return matrix
}

// ── 本地假发行版/模型服务器（支持 Range；离线） ────────────────────────────

interface FileServer {
  base: string
  requests: Array<{ path: string; range: string | null }>
  stop: () => void
}

/** 本地静态文件服务：随机端口、按路径或文件名取内容、真支持 Range（206 + Content-Range）。
 *  请求头全量记录，供「断点续传是否发出 Range」断言。 */
function startFileServer(files: Record<string, Uint8Array | undefined>): FileServer {
  const requests: FileServer["requests"] = []
  const server = Bun.serve({
    port: 0,
    fetch(req) {
      const url = new URL(req.url)
      const key = decodeURIComponent(url.pathname.replace(/^\//, ""))
      const range = req.headers.get("range")
      requests.push({ path: url.pathname, range })
      const body = files[key] ?? files[key.split("/").pop() ?? ""]
      if (!body) return new Response("not found", { status: 404 })
      if (range) {
        const m = range.match(/^bytes=(\d+)-$/)
        const start = m ? Number(m[1]) : -1
        if (start < 0 || start >= body.length) {
          return new Response("range not satisfiable", { status: 416, headers: { "content-range": `bytes */${body.length}` } })
        }
        const slice = body.subarray(start)
        return new Response(slice, {
          status: 206,
          headers: { "content-range": `bytes ${start}-${body.length - 1}/${body.length}`, "content-length": String(slice.length) },
        })
      }
      return new Response(body, { headers: { "content-length": String(body.length) } })
    },
  })
  return { base: `http://127.0.0.1:${server.port}`, requests, stop: () => server.stop(true) }
}

/** 半截响应后直接断开的原始 TCP 服务（模拟网络中断）：客户端 fetch 报错、下载器应清理半成品。
 *  用裸 socket 而非 Bun.serve 的流错误——后者会让服务端打一条无关的 error 日志、弄脏测试输出。 */
function startDroppingServer(partial: Uint8Array, declaredLength: number): { base: string; stop: () => void } {
  const server = Bun.listen({
    hostname: "127.0.0.1",
    port: 0,
    socket: {
      open() {},
      data(socket) {
        socket.write(`HTTP/1.1 200 OK\r\nContent-Length: ${declaredLength}\r\nConnection: close\r\n\r\n`)
        socket.write(partial)
        socket.end()
      },
    },
  })
  return { base: `http://127.0.0.1:${server.port}`, stop: () => server.stop(true) }
}

// ── 假 ToolContext（与 server.test.ts 同构；runCommand 缺省「命令不存在」） ──

interface CmdCall {
  cmd: string
  opts?: { workdir?: string; timeoutMs?: number }
}

function makeCtx(
  home: string,
  run: (cmd: string) => { stdout: string; stderr: string; code: number } | undefined = () => undefined,
): { ctx: ToolContext; calls: CmdCall[] } {
  const calls: CmdCall[] = []
  const workdir = join(home, "sessions", "s1", "tmp")
  mkdirSync(workdir, { recursive: true })
  const ctx: ToolContext = {
    user: "default",
    sessionId: "s1",
    workdir,
    sessionWorkdir: workdir,
    home,
    env: { LOCAL_INFER_HOME: home, LOCAL_INFER_MODELS_DIR: join(home, "models") },
    sandboxed: false,
    resolvePath: (p) => join(workdir, p),
    readFile: async (p) => Bun.file(p).text(),
    readBinaryFile: async (p) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p, c) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(join(p, ".."), { recursive: true })
      await writeFile(p, c)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async (cmd, opts) => {
      calls.push({ cmd, opts: opts as CmdCall["opts"] })
      // 缺省回放「命令不存在」（模拟无 nvidia-smi/vulkaninfo 的机器）
      return { stdout: "", stderr: "", code: 127, ...(run(cmd) ?? {}) }
    },
    uploadAttachment: (r) => Promise.resolve(r.path),
    publish: () => {},
    projects: [],
    resolveProjectPath: () => {
      throw new Error("无预置项目")
    },
    getTodos: async () => [],
    setTodos: async () => {},
    registry: { schemas: () => [], resolve: () => undefined, getAgentNames: () => [] },
    listSubAgentDefs: () => [],
    loadSubAgent: async () => {},
    waitForChoice: async () => null,
    waitForEnv: async () => false,
    waitForDraw: async () => ({ ok: true }),
  }
  return { ctx, calls }
}

/** 空设备节点目录（让探测不依赖宿主真实的 /dev）。 */
function emptyDevDir(): string {
  const d = join(tmp, `dev-${rnd()}`)
  mkdirSync(d, { recursive: true })
  return d
}

// ── 引擎矩阵 ──────────────────────────────────────────────────────────────

describe("引擎矩阵读取", () => {
  test("正常读取：版本 / 发行版基准 / 引擎清单", () => {
    const home = makeHome()
    const m = loadEngineMatrix(home)!
    expect(m.version).toBe(1)
    expect(m.release.llama_cpp.tag).toBe("b11175")
    expect(m.release.llama_cpp.base).toContain("example.invalid")
    expect(m.engines.length).toBe(TEST_MATRIX.engines.length)
    expect(m.engines.map((e) => e.id)).toContain("linux-cpu-x64")
    // 注释键（"//…"）不影响解析
    writeFileSync(
      join(home, "config", "engines.json"),
      JSON.stringify({ "//": "注释键合法", ...TEST_MATRIX }),
    )
    expect(loadEngineMatrix(home)?.engines.length).toBe(TEST_MATRIX.engines.length)
  })

  test("缺失 / 损坏 JSON / 结构不合法 → null（不抛错）", () => {
    const home = makeHome(null)
    expect(loadEngineMatrix(home)).toBeNull() // 文件缺失

    writeFileSync(join(home, "config", "engines.json"), "{ 这不是 JSON")
    expect(loadEngineMatrix(home)).toBeNull() // 损坏

    writeFileSync(join(home, "config", "engines.json"), JSON.stringify({ version: 1, engines: TEST_MATRIX.engines }))
    expect(loadEngineMatrix(home)).toBeNull() // 缺 release.llama_cpp

    writeFileSync(
      join(home, "config", "engines.json"),
      JSON.stringify({ version: 1, release: { llama_cpp: { tag: "t", base: "b" } }, engines: [] }),
    )
    expect(loadEngineMatrix(home)).toBeNull() // 空清单

    writeFileSync(join(home, "config", "engines.json"), JSON.stringify({ version: 1, release: { llama_cpp: { tag: "t", base: "b" } }, engines: "x" }))
    expect(loadEngineMatrix(home)).toBeNull() // 清单类型不对
  })

  test("仓库内真实矩阵：≥10 条、含各平台必需 id、asset 与 archive 自洽", () => {
    const m = loadEngineMatrix(inferHome({}))
    expect(m).not.toBeNull()
    const matrix = m!
    expect(matrix.release.llama_cpp.tag).toBe("b11175")
    expect(matrix.engines.length).toBeGreaterThanOrEqual(10)
    for (const id of [
      "linux-cpu-x64",
      "linux-cuda-12.8-x64",
      "linux-vulkan-x64",
      "linux-rocm-x64",
      "win-cpu-x64",
      "win-cuda-12.4-x64",
      "win-cuda-13.4-x64",
      "win-vulkan-x64",
      "macos-metal-arm64",
      "macos-metal-x64",
    ]) {
      expect(findEngine(matrix, id)).toBeDefined()
    }
    for (const e of matrix.engines) {
      expect(["linux", "win32", "darwin"]).toContain(e.platform)
      expect(["x64", "arm64"]).toContain(e.arch)
      expect(e.asset.length).toBeGreaterThan(0)
      // 归档扩展名与 archive 声明一致（解压分支按 archive 走，不一致会在安装时炸）
      expect(e.archive === "zip" ? /\.zip$/i.test(e.asset) : /\.tar\.gz$/i.test(e.asset)).toBe(true)
      if (e.cudart_asset) {
        expect(e.device).toBe("cuda")
        // cudart 资产同样有 archive 语义（按文件名推断）
        expect(/\.(zip|tar\.gz)$/i.test(e.cudart_asset)).toBe(true)
      }
    }
    // 上游 b11175 的归档路径格式固定为 {base}/{tag}/{asset}
    const def = findEngine(matrix, "linux-cpu-x64")!
    expect(engineAssetUrl(matrix, def)).toBe(`https://github.com/ggml-org/llama.cpp/releases/download/b11175/${def.asset}`)
  })
})

describe("地址拼接与引擎查找", () => {
  test("engineAssetUrl：{base}/{tag}/{asset}，base 尾斜杠归一", () => {
    const def = findEngine(TEST_MATRIX, "linux-cpu-x64")!
    expect(engineAssetUrl(TEST_MATRIX, def)).toBe("https://example.invalid/download/b11175/linux-cpu.tar.gz")
    const trailing: EngineMatrix = { ...TEST_MATRIX, release: { llama_cpp: { tag: "b11175", base: "https://x.test/dl//" } } }
    expect(engineAssetUrl(trailing, def)).toBe("https://x.test/dl/b11175/linux-cpu.tar.gz")
  })

  test("cudartAssetUrl：CUDA 引擎有、其它引擎无", () => {
    const cuda = findEngine(TEST_MATRIX, "linux-cuda-12.8-x64")!
    expect(cudartAssetUrl(TEST_MATRIX, cuda)).toBe("https://example.invalid/download/b11175/cudart-linux.tar.gz")
    expect(cudartAssetUrl(TEST_MATRIX, findEngine(TEST_MATRIX, "linux-cpu-x64")!)).toBeUndefined()
  })

  test("findEngine：命中与未命中", () => {
    expect(findEngine(TEST_MATRIX, "win-cuda-12.4-x64")?.platform).toBe("win32")
    expect(findEngine(TEST_MATRIX, "nope")).toBeUndefined()
  })
})

// ── 平台与设备探测 ────────────────────────────────────────────────────────

describe("平台与设备探测", () => {
  test("detectPlatform：平台注入 + arch 归一（未知平台按 linux 兼容值）", () => {
    expect(detectPlatform("win32", "x64")).toEqual({ platform: "win32", arch: "x64" })
    expect(detectPlatform("darwin", "arm64")).toEqual({ platform: "darwin", arch: "arm64" })
    expect(detectPlatform("linux", "aarch64")).toEqual({ platform: "linux", arch: "arm64" })
    expect(detectPlatform("linux", "x86_64")).toEqual({ platform: "linux", arch: "x64" })
    expect(detectPlatform("freebsd", "ia32")).toEqual({ platform: "linux", arch: "x64" })
    expect(detectPlatform("android", "ARM64")).toEqual({ platform: "linux", arch: "arm64" })
    // 缺省参数取本机（不抛错，且与本机平台一致）
    const auto = detectPlatform()
    expect(auto.platform).toBe(detectPlatform(process.platform).platform)
    expect(["x64", "arm64"]).toContain(auto.arch)
  })

  test("detectDevices：无 nvidia-smi / vulkaninfo 的机器不抛错，notes 给出探测依据", async () => {
    const home = makeHome()
    const devDir = emptyDevDir()
    const missing = (cmd: string): { stdout: string; stderr: string; code: number } =>
      cmd.startsWith("nvidia-smi")
        ? { stdout: "", stderr: "nvidia-smi: command not found", code: 127 }
        : { stdout: "", stderr: "vulkaninfo: command not found", code: 127 }
    const { ctx, calls } = makeCtx(home, missing)
    const d = await detectDevices(ctx, "linux", { devDir })

    expect(d.cpu_cores).toBeGreaterThan(0) // os.cpus().length
    expect(d.cuda).toBe(false)
    expect(d.vulkan).toBe(false)
    expect(d.metal).toBe(false)
    const notes = d.notes.join("\n")
    expect(notes).toContain("nvidia-smi 不可用：not found")
    expect(notes).toContain("vulkaninfo 不可用：not found")
    expect(notes).toContain(`设备节点 ${devDir}/nvidia0 不存在`)
    expect(notes).toContain("Metal：当前平台 linux 不支持")
    expect(notes).toContain("CPU：")
    // 探测确实走了宿主的命令通道（依据可追溯）
    expect(calls.some((c) => c.cmd.startsWith("nvidia-smi"))).toBe(true)
  })

  test("detectDevices：命令可用 / 设备节点存在 / Windows 不探 Vulkan / darwin 认 Metal", async () => {
    const home = makeHome()

    // ① nvidia-smi 可用 → cuda=true，依据带 GPU 名
    const { ctx: ctxCuda } = makeCtx(home, (cmd) =>
      cmd.startsWith("nvidia-smi")
        ? { stdout: "NVIDIA GeForce RTX 4080 SUPER\n", stderr: "", code: 0 }
        : { stdout: "", stderr: "vulkaninfo: command not found", code: 127 },
    )
    const d1 = await detectDevices(ctxCuda, "linux", { devDir: emptyDevDir() })
    expect(d1.cuda).toBe(true)
    expect(d1.notes.join("\n")).toContain("RTX 4080 SUPER")

    // ② 只有设备节点（无命令）→ 仍判为可用（容器/Jetson 一类环境）
    const devDir = join(tmp, `dev-${rnd()}`)
    mkdirSync(join(devDir, "dri"), { recursive: true })
    writeFileSync(join(devDir, "nvidia0"), "")
    writeFileSync(join(devDir, "dri", "renderD128"), "")
    writeFileSync(join(devDir, "dri", "card0"), "")
    const { ctx: ctxNone } = makeCtx(home, (cmd) => ({ stdout: "", stderr: `${cmd.split(" ")[0]}: command not found`, code: 127 }))
    const d2 = await detectDevices(ctxNone, "linux", { devDir })
    expect(d2.cuda).toBe(true)
    expect(d2.vulkan).toBe(true)
    expect(d2.notes.join("\n")).toContain("renderD128")
    expect(d2.notes.join("\n")).not.toContain("DRM render 节点：无") // 有 render 节点时不走「无」分支
    // 有 render 节点时不出现「DRM render 节点：无」

    // ③ Windows 下不探 Vulkan（无可靠只读命令）
    const d3 = await detectDevices(ctxNone, "win32", { devDir })
    expect(d3.notes.join("\n")).toContain("Windows 下不探测")

    // ④ darwin 认 Metal
    const d4 = await detectDevices(ctxNone, "darwin", { devDir: emptyDevDir() })
    expect(d4.metal).toBe(true)
    expect(d4.notes.join("\n")).toContain("darwin 平台自带 Metal")
  })

  test("detectDevices：无 ctx（直连 spawn）也不抛错，返回完整结构", async () => {
    const d = await detectDevices(undefined, "linux")
    expect(d.cpu_cores).toBeGreaterThan(0)
    expect(typeof d.cuda).toBe("boolean")
    expect(typeof d.vulkan).toBe("boolean")
    expect(typeof d.metal).toBe("boolean")
    expect(d.notes.length).toBeGreaterThan(0)
    expect(d.notes.join("\n")).toContain("os.cpus().length")
  })
})

// ── 安装状态与引擎视图 ────────────────────────────────────────────────────

describe("安装状态与引擎视图", () => {
  test("installedEngine：标记缺失 / 损坏 / exe 不存在 → null；正常 → 返回标记内容", () => {
    const home = makeHome()
    expect(installedEngine(home, "linux-cpu-x64")).toBeNull() // 目录与标记都不存在

    const dir = engineDir(home, "linux-cpu-x64")
    mkdirSync(dir, { recursive: true })
    expect(installedEngine(home, "linux-cpu-x64")).toBeNull() // 无标记

    const exe = join(dir, "build", "bin", "llama-server")
    mkdirSync(join(dir, "build", "bin"), { recursive: true })
    writeFileSync(exe, "#!/bin/sh\necho fake\n")
    const marker = { id: "linux-cpu-x64", exe, tag: "b11175", asset: "a.tar.gz", installed_at: new Date().toISOString(), dir }
    writeFileSync(join(dir, ".engine.json"), JSON.stringify(marker))
    expect(installedEngine(home, "linux-cpu-x64")?.exe).toBe(exe)
    expect(installedEngine(home, "linux-cpu-x64")?.tag).toBe("b11175")

    writeFileSync(join(dir, ".engine.json"), "{ 坏 JSON")
    expect(installedEngine(home, "linux-cpu-x64")).toBeNull()

    // exe 被删 → 视为未安装（安装状态以文件系统为准）
    writeFileSync(join(dir, ".engine.json"), JSON.stringify({ ...marker, exe: join(dir, "gone") }))
    expect(installedEngine(home, "linux-cpu-x64")).toBeNull()
  })

  test("listEngineViews：字段齐备，已安装优先、本机平台优先", () => {
    const home = makeHome()
    // 只在「非本机平台」条目上放一个已安装标记，验证排序优先级
    const dir = engineDir(home, "win-cuda-12.4-x64")
    mkdirSync(dir, { recursive: true })
    const exe = join(dir, "llama-server.exe")
    writeFileSync(exe, "")
    writeFileSync(
      join(dir, ".engine.json"),
      JSON.stringify({ id: "win-cuda-12.4-x64", exe, tag: "b11175", asset: "win-cuda.zip", installed_at: "2026-01-01T00:00:00.000Z", dir }),
    )

    const views = listEngineViews(home, TEST_MATRIX, { platform: "linux", arch: "x64" })
    expect(views.length).toBe(TEST_MATRIX.engines.length)
    expect(views[0].id).toBe("win-cuda-12.4-x64") // 已安装 → 置顶
    expect(views[0].installed).toBe(true)
    expect(views[0].exe).toBe(exe)
    expect(views[0].downloadable).toBe(true)
    expect(views[0].is_current_platform).toBe(false)
    expect(views[0].dir).toBe(dir)

    const cpu = views.find((v) => v.id === "linux-cpu-x64")!
    expect(cpu.installed).toBe(false)
    expect(cpu.is_current_platform).toBe(true)
    expect(cpu.device).toBe("cpu")
    expect(cpu.notes).toBe("无 GPU 默认")
    // 本机平台（未安装）条目排在非本机平台（未安装）条目前
    expect(views.findIndex((v) => v.id === "linux-cpu-x64")).toBeLessThan(views.findIndex((v) => v.id === "macos-metal-arm64"))
    expect(views.findIndex((v) => v.id === "macos-metal-arm64")).toBeGreaterThan(views.findIndex((v) => v.id === "linux-vulkan-x64"))
  })

  test("pickEngineForPlatform：无探测时保守取 cpu（darwin 取 metal），有探测按设备优先级", () => {
    const linux = { platform: "linux" as const, arch: "x64" as const }
    expect(pickEngineForPlatform(TEST_MATRIX, linux)).toBe("linux-cpu-x64")
    expect(pickEngineForPlatform(TEST_MATRIX, linux, { cuda: true, vulkan: true, metal: false })).toBe("linux-cuda-12.8-x64")
    expect(pickEngineForPlatform(TEST_MATRIX, linux, { cuda: false, vulkan: true, metal: false })).toBe("linux-vulkan-x64")
    expect(pickEngineForPlatform(TEST_MATRIX, linux, { cuda: false, vulkan: false, metal: false })).toBe("linux-cpu-x64")
    expect(pickEngineForPlatform(TEST_MATRIX, { platform: "darwin", arch: "arm64" })).toBe("macos-metal-arm64")
    expect(pickEngineForPlatform(TEST_MATRIX, { platform: "win32", arch: "arm64" })).toBeUndefined() // 矩阵无本机条目
  })
})

// ── 下载（本地服务器，离线） ──────────────────────────────────────────────

describe("downloadFile（断点续传 + 校验 + 原子落盘）", () => {
  test("全量下载：进度回调、大小校验、.part 清理、内容逐字节一致", async () => {
    const body = Buffer.from("0123456789".repeat(500)) // 5000 字节
    const srv = startFileServer({ "m.bin": body })
    const dest = join(tmp, `dl-${rnd()}.bin`)
    const phases: string[] = []
    let received = 0
    let total: number | undefined
    const r = await downloadFile(`${srv.base}/m.bin`, dest, {
      expect_bytes: body.length,
      onProgress: (p) => {
        phases.push(p.phase)
        received = p.received
        total = p.total ?? total
      },
    })
    expect(r.ok).toBe(true)
    expect(r.bytes).toBe(body.length)
    expect(readFileSync(dest).equals(body)).toBe(true)
    expect(existsSync(`${dest}.part`)).toBe(false)
    expect(phases[0]).toBe("start")
    expect(phases).toContain("download")
    expect(phases[phases.length - 1]).toBe("done")
    expect(received).toBe(body.length)
    expect(total).toBe(body.length)
    expect(srv.requests.length).toBe(1)
    expect(srv.requests[0].range).toBeNull() // 无半成品 → 不带 Range
    srv.stop()
  })

  test("断点续传：目标已有前 N 字节 → 发出 Range 头、206 追加、最终字节数正确", async () => {
    const body = Buffer.from("abcdefghij".repeat(1000)) // 10000 字节
    const srv = startFileServer({ "r.bin": body })
    const dest = join(tmp, `resume-${rnd()}.bin`)
    writeFileSync(dest, body.subarray(0, 4096)) // 上次中断留在目标路径上的半成品

    const r = await downloadFile(`${srv.base}/r.bin`, dest, { expect_bytes: body.length })
    expect(r.ok).toBe(true)
    expect(r.bytes).toBe(body.length)
    expect(readFileSync(dest).equals(body)).toBe(true) // 前 4096 字节是原样的，后段追加
    expect(srv.requests.length).toBe(1)
    expect(srv.requests[0].range).toBe("bytes=4096-")
    expect(existsSync(`${dest}.part`)).toBe(false)
    srv.stop()
  })

  test("断点续传：`.part` 半成品同样作为续传起点（进程被强杀后的恢复路径）", async () => {
    const body = Buffer.from("QQQQ".repeat(700)) // 2800 字节
    const srv = startFileServer({ "p.bin": body })
    const dest = join(tmp, `resume-part-${rnd()}.bin`)
    writeFileSync(`${dest}.part`, body.subarray(0, 1200))

    const r = await downloadFile(`${srv.base}/p.bin`, dest, { expect_bytes: body.length })
    expect(r.ok).toBe(true)
    expect(readFileSync(dest).equals(body)).toBe(true)
    expect(srv.requests[0].range).toBe("bytes=1200-")
    srv.stop()
  })

  test("sha256：不匹配则失败并清理半成品；匹配则通过", async () => {
    const body = Buffer.from("z".repeat(2048))
    const srv = startFileServer({ "s.bin": body })
    const dest = join(tmp, `sha-${rnd()}.bin`)

    const bad = await downloadFile(`${srv.base}/s.bin`, dest, { expect_bytes: body.length, sha256: "0".repeat(64) })
    expect(bad.ok).toBe(false)
    expect(String(bad.error)).toContain("sha256 校验失败")
    expect(existsSync(dest)).toBe(false)
    expect(existsSync(`${dest}.part`)).toBe(false)

    const good = await downloadFile(`${srv.base}/s.bin`, dest, {
      expect_bytes: body.length,
      sha256: createHash("sha256").update(body).digest("hex"),
    })
    expect(good.ok).toBe(true)
    expect(readFileSync(dest).equals(body)).toBe(true)
    srv.stop()
  })

  test("大小不匹配 → 失败并清理半成品（不把残缺文件当成品）", async () => {
    const body = Buffer.from("k".repeat(1000))
    const srv = startFileServer({ "x.bin": body })
    const dest = join(tmp, `size-${rnd()}.bin`)
    const r = await downloadFile(`${srv.base}/x.bin`, dest, { expect_bytes: body.length + 10 })
    expect(r.ok).toBe(false)
    expect(String(r.error)).toContain("大小校验失败")
    expect(existsSync(dest)).toBe(false)
    expect(existsSync(`${dest}.part`)).toBe(false)
    srv.stop()
  })

  test("中途断流 → 失败且临时文件不存在（半成品清理）", async () => {
    const body = Buffer.from("m".repeat(4096))
    const srv = startDroppingServer(body.subarray(0, 512), body.length)
    const dest = join(tmp, `broken-${rnd()}.bin`)
    const r = await downloadFile(`${srv.base}/b.bin`, dest, { expect_bytes: body.length })
    expect(r.ok).toBe(false)
    expect(String(r.error)).toContain("下载失败")
    expect(r.bytes).toBeLessThan(body.length)
    expect(existsSync(dest)).toBe(false)
    expect(existsSync(`${dest}.part`)).toBe(false)
    srv.stop()
  })

  test("HTTP 错误（404）→ 失败且带状态码说明", async () => {
    const srv = startFileServer({})
    const dest = join(tmp, `404-${rnd()}.bin`)
    const r = await downloadFile(`${srv.base}/nope.bin`, dest, {})
    expect(r.ok).toBe(false)
    expect(String(r.error)).toContain("HTTP 404")
    expect(existsSync(dest)).toBe(false)
    srv.stop()
  })

  test("幂等：目标已存在且大小达标 → 不再发请求", async () => {
    const body = Buffer.from("i".repeat(777))
    const srv = startFileServer({ "i.bin": body })
    const dest = join(tmp, `idem-${rnd()}.bin`)
    writeFileSync(dest, body)
    const r = await downloadFile(`${srv.base}/i.bin`, dest, { expect_bytes: body.length })
    expect(r.ok).toBe(true)
    expect(r.bytes).toBe(body.length)
    expect(srv.requests.length).toBe(0)
    srv.stop()
  })
})

// ── 引擎安装（本地假发行版） ──────────────────────────────────────────────

/** 假发行版 zip：含 build/bin/llama-server（模拟 llama.cpp 归档内的嵌套路径）。 */
function fakeEngineZip(): Uint8Array {
  return zipSync({
    "build/bin/llama-server": strToU8("#!/bin/sh\necho fake-llama-server\n"),
    "build/bin/llama-server-impl.dll": strToU8("not really a dll"),
    "README.md": strToU8("fake distribution"),
  })
}

describe("installEngine", () => {
  test("zip 归档：解压 → 递归定位 llama-server → chmod 0o755 → 写标记 → 幂等重入不重复下载", async () => {
    const home = makeHome(null)
    const srv = startFileServer({ "fake-engine.zip": fakeEngineZip() })
    const def: EngineDef = { id: "test-cpu-x64", platform: "linux", arch: "x64", device: "cpu", asset: "fake-engine.zip", archive: "zip" }
    const matrix = writeMatrix(home, [def], `${srv.base}/dl`)

    const r = await installEngine(home, def, matrix)
    expect(r.ok).toBe(true)
    const eng = r.engine!
    expect(eng.id).toBe("test-cpu-x64")
    expect(eng.tag).toBe("b11175")
    expect(eng.asset).toBe("fake-engine.zip")
    expect(eng.installed_at).toMatch(/^\d{4}-\d{2}-\d{2}T/)
    expect(eng.dir).toBe(engineDir(home, "test-cpu-x64"))
    expect(basename(eng.exe)).toBe("llama-server")
    expect(eng.exe).toContain(join("build", "bin")) // 归档层级被保留
    expect(eng.exe.startsWith(eng.dir)).toBe(true)
    expect(existsSync(eng.exe)).toBe(true)
    // 解压内容齐备
    expect(existsSync(join(eng.dir, "README.md"))).toBe(true)
    expect(existsSync(join(eng.dir, "build", "bin", "llama-server-impl.dll"))).toBe(true)
    // POSIX 下执行位就绪（zip 归档不带 mode，必须由安装器补）
    if (process.platform !== "win32") expect(statSync(eng.exe).mode & 0o777).toBe(0o755)
    // 标记文件内容齐全，且 installedEngine 可见
    const marker = JSON.parse(readFileSync(join(eng.dir, ".engine.json"), "utf-8")) as Record<string, unknown>
    expect(marker.id).toBe("test-cpu-x64")
    expect(marker.exe).toBe(eng.exe)
    expect(marker.tag).toBe("b11175")
    expect(marker.asset).toBe("fake-engine.zip")
    expect(marker.dir).toBe(eng.dir)
    expect(marker.installed_at).toBe(eng.installed_at)
    expect(installedEngine(home, "test-cpu-x64")?.exe).toBe(eng.exe)
    // 无临时目录残留
    expect(readdirSync(vendorDir(home)).filter((n) => n.startsWith(".stage-"))).toEqual([])

    // 幂等重入：第二次不再下载（请求数不变），且返回同一 exe
    const before = srv.requests.length
    const again = await installEngine(home, def, matrix)
    expect(again.ok).toBe(true)
    expect(again.engine?.exe).toBe(eng.exe)
    expect(srv.requests.length).toBe(before)

    // force=true 会重新下载并整体替换
    const forced = await installEngine(home, def, matrix, { force: true })
    expect(forced.ok).toBe(true)
    expect(srv.requests.length).toBeGreaterThan(before)
    srv.stop()
  })

  test("cudart 资产一并安装并在标记里标 cudart: true", async () => {
    const home = makeHome(null)
    const srv = startFileServer({
      "cuda-engine.zip": fakeEngineZip(),
      "cudart.zip": zipSync({ "cudart/libcublasLt.so": strToU8("fake-cublas") }),
    })
    const def: EngineDef = {
      id: "test-cuda-x64",
      platform: "linux",
      arch: "x64",
      device: "cuda",
      asset: "cuda-engine.zip",
      archive: "zip",
      cudart_asset: "cudart.zip",
    }
    const matrix = writeMatrix(home, [def], `${srv.base}/dl`)

    const r = await installEngine(home, def, matrix)
    expect(r.ok).toBe(true)
    const eng = r.engine!
    expect(existsSync(join(eng.dir, "cudart", "libcublasLt.so"))).toBe(true)
    const marker = JSON.parse(readFileSync(join(eng.dir, ".engine.json"), "utf-8")) as Record<string, unknown>
    expect(marker.cudart).toBe(true)
    // 两条资产都下载了
    expect(srv.requests.filter((q) => q.path.endsWith(".zip")).length).toBe(2)
    srv.stop()
  })

  test("失败（归档内没有 llama-server）→ 返回可操作错误、不留残目录/临时目录", async () => {
    const home = makeHome(null)
    const srv = startFileServer({ "no-exe.zip": zipSync({ "bin/other-tool": strToU8("x") }) })
    const def: EngineDef = { id: "test-broken", platform: "linux", arch: "x64", device: "cpu", asset: "no-exe.zip", archive: "zip" }
    const matrix = writeMatrix(home, [def], `${srv.base}/dl`)

    const r = await installEngine(home, def, matrix)
    expect(r.ok).toBe(false)
    expect(String(r.error)).toContain("llama-server")
    expect(existsSync(engineDir(home, "test-broken"))).toBe(false)
    expect(readdirSync(vendorDir(home)).filter((n) => n.startsWith(".stage-"))).toEqual([])
    // 下载失败（404）同样不留残目录
    const missing: EngineDef = { ...def, id: "test-404", asset: "does-not-exist.zip" }
    const r2 = await installEngine(home, missing, writeMatrix(home, [missing], `${srv.base}/dl`))
    expect(r2.ok).toBe(false)
    expect(String(r2.error)).toContain("下载引擎资产失败")
    expect(existsSync(engineDir(home, "test-404"))).toBe(false)
    srv.stop()
  })

  test("findLlamaServer：递归定位与平台命名（找不到返回 null）", () => {
    const root = join(tmp, `find-${rnd()}`)
    mkdirSync(join(root, "outer", "inner"), { recursive: true })
    expect(findLlamaServer(root)).toBeNull() // 空目录
    writeFileSync(join(root, "outer", "sqlite3"), "")
    expect(findLlamaServer(root)).toBeNull() // 有别的二进制但无 llama-server
    const exe = join(root, "outer", "inner", "llama-server")
    writeFileSync(exe, "")
    expect(findLlamaServer(root, "linux")).toBe(exe)
    // Windows 优先 .exe：只有 POSIX 命名时仍能兜底找到
    expect(findLlamaServer(root, "win32")).toBe(exe)
    const winExe = join(root, "llama-server.exe")
    writeFileSync(winExe, "")
    expect(findLlamaServer(root, "win32")).toBe(winExe)
    expect(findLlamaServer(root, "linux")).toBe(exe)
  })

  const hasTar = (() => {
    try {
      return spawnSync("tar", ["--version"], { stdio: "ignore" }).status === 0
    } catch {
      return false
    }
  })()

  if (hasTar) {
    test("tar.gz 归档：系统 tar 解压（Windows 10+ 自带 bsdtar 同名）并定位 exe", async () => {
      const home = makeHome(null)
      // 现场造一个 tar.gz（含 build/bin/llama-server）
      const src = join(tmp, `tarsrc-${rnd()}`)
      mkdirSync(join(src, "build", "bin"), { recursive: true })
      writeFileSync(join(src, "build", "bin", "llama-server"), "#!/bin/sh\necho fake-tar\n")
      writeFileSync(join(src, "build", "bin", "llama-bench"), "")
      const pack = join(tmp, `pack-${rnd()}.tar.gz`)
      const made = spawnSync("tar", ["-czf", pack, "-C", src, "build"])
      expect(made.status).toBe(0)

      const srv = startFileServer({ "fake-engine.tar.gz": new Uint8Array(readFileSync(pack)) })
      const def: EngineDef = { id: "test-tar-x64", platform: "linux", arch: "x64", device: "cpu", asset: "fake-engine.tar.gz", archive: "tar.gz" }
      const matrix = writeMatrix(home, [def], `${srv.base}/dl`)

      const r = await installEngine(home, def, matrix)
      expect(r.ok).toBe(true)
      const eng = r.engine!
      expect(basename(eng.exe)).toBe("llama-server")
      expect(existsSync(join(eng.dir, "build", "bin", "llama-bench"))).toBe(true)
      if (process.platform !== "win32") expect(statSync(eng.exe).mode & 0o777).toBe(0o755)
      expect(installedEngine(home, "test-tar-x64")?.exe).toBe(eng.exe)
      srv.stop()
    })
  } else {
    test.skip("tar.gz 归档：系统 tar 解压（本机无 tar，跳过）", () => {})
  }
})

// ── 工具契约与执行 ────────────────────────────────────────────────────────

describe("tools 契约", () => {
  test("三个工具齐备：参数蛇形、描述提到跨平台、outputSchema 齐备、safeMode/requiresApproval 声明正确", () => {
    expect(Object.keys(tools).sort()).toEqual(["engine_fetch", "engines", "model_fetch"])
    for (const t of Object.values(tools)) {
      expect(t.description).toContain("跨平台")
      expect(t.description.length).toBeGreaterThan(30)
      expect(t.outputSchema).toBeDefined()
      expect(t.outputSchema?.type).toBe("object")
      for (const k of Object.keys(t.parameters.properties ?? {})) expect(k).toMatch(/^[a-z][a-z0-9_]*$/)
    }
    // engines：只读 → 安全模式可用且免审批
    expect(tools.engines.safeMode).toBe(true)
    expect(tools.engines.requiresApproval).toBeUndefined()
    // 下载/落盘类：安全模式不可用 + 需审批
    for (const name of ["engine_fetch", "model_fetch"]) {
      expect(tools[name].safeMode).toBe(false)
      expect(tools[name].requiresApproval).toBe(true)
    }
    expect(requiresApproval).toEqual({ engine_fetch: true, model_fetch: true })
    expect(tools.engine_fetch.parameters.required).toEqual(["id"])
    expect(tools.engines.parameters.properties?.action).toBeDefined()
  })

  test("engines(list)：矩阵 × 本机平台表格（含已安装/可下载/是否本机平台）+ 结构化输出", async () => {
    const home = makeHome()
    const { ctx } = makeCtx(home)
    const r = await tools.engines.execute({ action: "list" }, ctx)
    const plat = detectPlatform()
    expect(r.output).toContain("引擎矩阵")
    expect(r.output).toContain(`${plat.platform}/${plat.arch}`)
    expect(r.output).toContain("linux-cpu-x64")
    expect(r.output).toContain("已安装") // 图例与状态列
    const data = r.data as Record<string, unknown>
    expect(data.action).toBe("list")
    expect(data.platform).toBe(plat.platform)
    expect(data.tag).toBe("b11175")
    expect(data.count).toBe(TEST_MATRIX.engines.length)
    expect(data.installed).toBe(0)
    expect(data.recommended).toBe(pickEngineForPlatform(TEST_MATRIX, plat))
    expect(Array.isArray(data.engines)).toBe(true)

    // list 带 id → 单引擎详情（含资产与拼出的地址）
    const detail = await tools.engines.execute({ action: "list", id: "win-cuda-12.4-x64" }, ctx)
    expect(detail.output).toContain("引擎 win-cuda-12.4-x64")
    expect(detail.output).toContain("https://example.invalid/download/b11175/win-cuda.zip")
    expect(detail.output).toContain("本机平台匹配")

    const unknown = await tools.engines.execute({ action: "list", id: "nope" }, ctx)
    expect(unknown.output).toContain("未知引擎 id")
  })

  test("engines(list)：矩阵缺失时给可操作提示（不抛错）", async () => {
    const home = makeHome(null)
    const { ctx } = makeCtx(home)
    const r = await tools.engines.execute({ action: "list" }, ctx)
    expect(r.output).toContain("引擎矩阵缺失或损坏")
    expect(r.output).toContain("engines.json")
  })

  test("engines(detect)：设备探测输出（CPU 核数 + 依据 + 推荐），带 id 时评定该引擎可用性", async () => {
    const home = makeHome()
    const { ctx } = makeCtx(home, (cmd) => ({ stdout: "", stderr: `${cmd.split(" ")[0]}: command not found`, code: 127 }))
    const r = await tools.engines.execute({ action: "detect" }, ctx)
    expect(r.output).toContain("本机设备探测")
    expect(r.output).toContain("CPU 核数")
    expect(r.output).toContain("探测依据")
    const data = r.data as Record<string, unknown>
    const devices = data.devices as { cpu_cores: number; notes: string[] }
    expect(devices.cpu_cores).toBeGreaterThan(0)
    expect(devices.notes.length).toBeGreaterThan(0)
    expect(data.action).toBe("detect")

    const withId = await tools.engines.execute({ action: "detect", id: "linux-cuda-12.8-x64" }, ctx)
    expect(withId.output).toContain("引擎 linux-cuda-12.8-x64 在本机")
    const avail = (withId.data as Record<string, unknown>).available as { ok: boolean; why: string }
    expect(typeof avail.ok).toBe("boolean")
    expect(avail.why.length).toBeGreaterThan(0)
  })

  test("engine_fetch：本地假发行版安装成功 → 输出 exe/下一步提示；重复调用直接返回；未知 id 给清单", async () => {
    const home = makeHome(null)
    const srv = startFileServer({ "fake-engine.zip": fakeEngineZip() })
    const def: EngineDef = { id: "test-cpu-x64", platform: "linux", arch: "x64", device: "cpu", asset: "fake-engine.zip", archive: "zip" }
    writeMatrix(home, [def], `${srv.base}/dl`)
    const { ctx } = makeCtx(home, (cmd) => ({ stdout: "", stderr: `${cmd.split(" ")[0]}: command not found`, code: 127 }))

    const r = await tools.engine_fetch.execute({ id: "test-cpu-x64" }, ctx)
    expect(r.output).toContain("安装完成")
    expect(r.output).toContain("llama-server")
    expect(r.output).toContain("local_infer_start")
    const data = r.data as Record<string, unknown>
    expect(data.ok).toBe(true)
    expect(data.cached).toBe(false)
    expect(existsSync(String(data.exe))).toBe(true)
    expect(String(data.exe).endsWith("llama-server")).toBe(true)

    const before = srv.requests.length
    const again = await tools.engine_fetch.execute({ id: "test-cpu-x64" }, ctx)
    expect(again.output).toContain("已安装")
    expect((again.data as Record<string, unknown>).cached).toBe(true)
    expect(srv.requests.length).toBe(before)

    const unknown = await tools.engine_fetch.execute({ id: "nope" }, ctx)
    expect(unknown.output).toContain("未知引擎 id")
    const noId = await tools.engine_fetch.execute({}, ctx)
    expect(noId.output).toContain("缺少参数 id")
    srv.stop()
  })

  test("model_fetch：预置注册表契约（高价值直链 + 字节数 + sha256）", () => {
    const p = MODEL_PRESETS["qwen2.5-0.5b"]
    expect(p).toBeDefined()
    expect(p.name).toBe("Qwen2.5-0.5B-Instruct-Q4_K_M.gguf")
    expect(p.bytes).toBe(397808192)
    expect(p.url).toBe(
      "https://huggingface.co/bartowski/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf",
    )
    expect(p.sha256).toMatch(/^[0-9a-f]{64}$/)
    expect(MODEL_PRESETS["qwen2.5-1.5b"].name).toBe("Qwen2.5-1.5B-Instruct-Q4_K_M.gguf")
    for (const [key, v] of Object.entries(MODEL_PRESETS)) {
      expect(key.length).toBeGreaterThan(0)
      expect(v.url.startsWith("https://")).toBe(true)
      expect(v.name.endsWith(".gguf")).toBe(true)
      expect(v.bytes).toBeGreaterThan(1024 * 1024) // 直链模型至少几 MB
      expect(v.sha256).toMatch(/^[0-9a-f]{64}$/)
    }
  })

  test("model_fetch：下载到模型目录、内容一致、幂等跳过；非法文件名/未知预置给提示", async () => {
    const home = makeHome()
    const body = Buffer.from("GGUF-fake".repeat(500))
    const srv = startFileServer({ "mini.gguf": body })
    const { ctx } = makeCtx(home)

    const r = await tools.model_fetch.execute({ url: `${srv.base}/mini.gguf`, name: "mini.gguf", expect_bytes: body.length }, ctx)
    expect(r.output).toContain("模型就绪")
    const dest = join(home, "models", "mini.gguf")
    expect(existsSync(dest)).toBe(true)
    expect(readFileSync(dest).equals(body)).toBe(true)
    expect((r.data as Record<string, unknown>).skipped).toBe(false)

    const before = srv.requests.length
    const again = await tools.model_fetch.execute({ url: `${srv.base}/mini.gguf`, name: "mini.gguf", expect_bytes: body.length }, ctx)
    expect(again.output).toContain("已就绪")
    expect((again.data as Record<string, unknown>).skipped).toBe(true)
    expect(srv.requests.length).toBe(before)

    // 文件名从 url 推断（缺省 name）
    const inferred = await tools.model_fetch.execute({ url: `${srv.base}/mini2.gguf`, expect_bytes: body.length }, ctx)
    expect(inferred.output).toContain("mini2.gguf")

    const badName = await tools.model_fetch.execute({ url: `${srv.base}/mini.gguf`, name: "../evil.gguf" }, ctx)
    expect(badName.output).toContain("不合法")

    const badPreset = await tools.model_fetch.execute({ preset: "gpt-9" }, ctx)
    expect(badPreset.output).toContain("未知预置")

    const noArg = await tools.model_fetch.execute({}, ctx)
    expect(noArg.output).toContain("需要 preset 或 url")
    srv.stop()
  })
})

// ── 配置契约（profiles.json 的引擎字段与跨平台档位） ──────────────────────

describe("profiles.json 配置契约", () => {
  test("七档指向 Windows CUDA 引擎；cpu-small 跨平台档就位；cpu-baseline 由引擎层按平台解析", () => {
    const home = inferHome({})
    const p = loadProfiles(home)
    expect(p).not.toBeNull()
    const prof = p!
    const raw = JSON.parse(readFileSync(profilesPath(home), "utf-8")) as { profiles: Record<string, Record<string, unknown>> }

    for (const key of ["fast", "balanced", "quality", "long-context", "concurrent", "concurrent-max", "throughput"]) {
      expect(raw.profiles[key]).toBeDefined()
      expect(raw.profiles[key].engine).toBe("win-cuda-12.4-x64")
      expect(prof.profiles[key].model).toBeTruthy()
    }
    // 现有档位数值未被改动（抽查实测参数）
    expect(prof.profiles.fast.ctx).toBe(32768)
    expect(prof.profiles.fast.n_cpu_moe).toBe(0)
    expect(prof.profiles.throughput.parallel).toBe(16)
    expect(prof.profiles["concurrent-max"].parallel).toBe(4)

    // 新增：跨平台 CPU 小模型档位
    const cs = raw.profiles["cpu-small"]
    expect(cs).toBeDefined()
    expect(cs.model).toBe("Qwen2.5-0.5B-Instruct-Q4_K_M.gguf")
    expect(cs.ctx).toBe(8192)
    expect(cs.n_gpu_layers).toBe(0)
    expect(cs.n_cpu_moe).toBeNull()
    expect(cs.flash_attn).toBe("off")
    expect(cs.threads).toBe(6)
    expect(cs.batch).toBe(512)
    expect(cs.ubatch).toBe(128)
    expect(cs.parallel).toBe(2)
    expect(cs.extra_args).toEqual(["--jinja"])
    expect(cs.engine).toBeUndefined() // 平台不固定：由引擎层解析
    expect(String(raw.profiles["cpu-small"]["//"] ?? "")).toContain("互不影响")

    // cpu-baseline 不固定引擎（注释说明由引擎层按平台解析），数值保持原样
    expect(raw.profiles["cpu-baseline"].engine).toBeUndefined()
    expect(String(raw.profiles["cpu-baseline"]["//engine"] ?? "")).toContain("引擎层")
    expect(prof.profiles["cpu-baseline"].n_cpu_moe).toBeNull() // InferProfile 只声明公共字段，细节以 raw 为准
    expect(raw.profiles["cpu-baseline"].threads).toBe(20)
    expect(raw.profiles["cpu-baseline"].n_gpu_layers).toBe(0)
  })
})
