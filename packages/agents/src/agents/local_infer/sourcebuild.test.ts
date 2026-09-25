/**
 * sourcebuild.ts（源码编译安装层）测试：构建预设矩阵与命令组装、编译安装端到端（假源码树 + 假 cmake 可执行脚本）、
 * 幂等重入、clean 重配、失败路径（configure/build/找不到产物）与半成品清理、工具链门禁、参数校验。
 *
 * 环境封闭：一切落盘都在 mkdtemp 临时目录内（**不写仓库目录**）、不联网、不调用真实编译工具链——
 * 命令执行走「假 cmake 脚本 + 记录式 runCommand（代跑真子进程）」；安装结果用 engines.ts 的 `installedEngine`
 * 断言（同一落点、同一标记格式，这是本层与下载安装层最重要的契约）。
 */
import { spawnSync } from "node:child_process"
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { afterAll, describe, expect, test } from "bun:test"
import type { ToolContext } from "@gebai/sdk"
import { installedEngine } from "./engines"
import { engineDir, vendorDir } from "./paths"
import {
  BUILD_DEVICES,
  type BuildDevice,
  type BuildSpec,
  buildFromSource,
  buildPreset,
  cmakeBuildCmd,
  cmakeConfigureCmd,
  defaultJobs,
  hasOnPath,
  locateBuiltServer,
} from "./sourcebuild"

const tmp = mkdtempSync(join(tmpdir(), "gebai-infer-sourcebuild-"))
afterAll(() => rmSync(tmp, { recursive: true, force: true }))

const rnd = (): string => Math.random().toString(36).slice(2, 10)

/** 临时 infer 子项目根（引擎装到 <home>/vendor/<id>）。 */
function makeHome(): string {
  const home = join(tmp, `home-${rnd()}`)
  mkdirSync(home, { recursive: true })
  return home
}

function readdirSafe(dir: string): string[] {
  try {
    return readdirSync(dir)
  } catch {
    return []
  }
}

function statSafe(p: string): ReturnType<typeof statSync> | null {
  try {
    return statSync(p)
  } catch {
    return null
  }
}

// ── 假源码树 ──────────────────────────────────────────────────────────────

/** 假 llama.cpp 源码树（本层只校验「是不是 CMake 工程」，内容不必真）。 */
function makeSource(o: { version?: string; cmakeVersion?: string } = {}): { dir: string; candidate: any } {
  const dir = join(tmp, `src-${rnd()}`)
  mkdirSync(dir, { recursive: true })
  writeFileSync(
    join(dir, "CMakeLists.txt"),
    `cmake_minimum_required(VERSION 3.14)\nproject("llama.cpp" VERSION ${o.cmakeVersion ?? "0.0.1"} LANGUAGES C CXX)\n`,
    "utf-8",
  )
  const candidate: any = {
    id: `llama-src-${rnd()}`,
    kind: "llama_cpp",
    path: dir,
    root: tmp,
    evidence: ["CMakeLists.txt 含 project(llama.cpp)"],
    ...(o.version ? { version: o.version } : {}),
  }
  return { dir, candidate }
}

// ── 假 cmake（可执行脚本；行为由环境变量控制，不依赖真实工具链） ──────────

/**
 * 假 cmake：configure 造出 `build-<device>/bin/llama-server`（shell 脚本，exit 0）与 `libllama.so`；
 * `--build` 直接返回 0。环境变量开关：
 *   FAKE_CMAKE_CONFIGURE_EXIT / FAKE_CMAKE_BUILD_EXIT —— 退出码（默认 0）
 *   FAKE_CMAKE_NO_EXE=1 —— configure 不产出可执行文件（验证「构建完成但没产物」）
 *   FAKE_CMAKE_EXE_AT_ROOT=1 —— 可执行文件落在构建目录根，且造 CMakeFiles/ 与 *.o（验证只抄运行时依赖）
 *   FAKE_CMAKE_LOG_LINES=N —— 先吐 N 行日志（验证 error 里的「日志尾部 60 行」）
 */
function makeFakeCmake(): string {
  const dir = join(tmp, `fake-bin-${rnd()}`)
  mkdirSync(dir, { recursive: true })
  const script = `#!/bin/sh
# 假 cmake（测试专用）：configure 造产物，--build 直接成功
build_dir=""
prev=""
for a in "$@"; do
  if [ "$prev" = "-B" ]; then build_dir="$a"; fi
  prev="$a"
done

noise() {
  i=1
  while [ "$i" -le "\${FAKE_CMAKE_LOG_LINES:-0}" ]; do
    echo "log-line-$i"
    i=$((i+1))
  done
}

case "$*" in
  *--version*)
    echo "cmake version 3.28.0"
    exit 0
    ;;
  *--build*)
    noise
    echo "fake cmake: build ok"
    exit "\${FAKE_CMAKE_BUILD_EXIT:-0}"
    ;;
esac

noise
if [ -z "$build_dir" ]; then
  echo "fake cmake: missing -B" >&2
  exit 2
fi
if [ "\${FAKE_CMAKE_NO_EXE:-0}" = "1" ]; then
  echo "fake cmake: configured (no exe) in $build_dir"
  exit "\${FAKE_CMAKE_CONFIGURE_EXIT:-0}"
fi
if [ "\${FAKE_CMAKE_EXE_AT_ROOT:-0}" = "1" ]; then
  mkdir -p "$build_dir/CMakeFiles"
  echo "junk" > "$build_dir/CMakeFiles/junk.txt"
  echo "obj" > "$build_dir/junk.o"
  echo "lib" > "$build_dir/libllama.so"
  printf '#!/bin/sh\\nexit 0\\n' > "$build_dir/llama-server"
  chmod +x "$build_dir/llama-server"
else
  mkdir -p "$build_dir/bin"
  echo "lib" > "$build_dir/bin/libllama.so"
  echo "lib0" > "$build_dir/bin/libllama.so.0"
  printf '#!/bin/sh\\nexit 0\\n' > "$build_dir/bin/llama-server"
  chmod +x "$build_dir/bin/llama-server"
fi
echo "fake cmake: configured in $build_dir"
exit "\${FAKE_CMAKE_CONFIGURE_EXIT:-0}"
`
  writeFileSync(join(dir, "cmake"), script, "utf-8")
  chmodSync(join(dir, "cmake"), 0o755)
  return dir
}

// ── 记录式 ToolContext（runCommand 用真子进程代跑假 cmake） ────────────────

interface CmdCall {
  cmd: string
  opts?: { workdir?: string; env?: Record<string, string>; timeoutMs?: number }
}

/**
 * 假 ctx：`runCommand` 把命令原样交给真 shell 执行（PATH 由 env 控制，所以能命中假 cmake），
 * 同时记录每次调用供断言（「第二次没重复 configure」这类幂等断言全靠它）。
 */
function makeCtx(home: string, env: Record<string, string> = {}): { ctx: ToolContext; calls: CmdCall[] } {
  const calls: CmdCall[] = []
  const workdir = join(home, "sessions", "s1", "tmp")
  mkdirSync(workdir, { recursive: true })
  const ctxEnv: Record<string, string> = { LOCAL_INFER_HOME: home, ...env }
  const ctx = {
    user: "default",
    sessionId: "s1",
    workdir,
    sessionWorkdir: workdir,
    home,
    env: ctxEnv,
    sandboxed: false,
    resolvePath: (p: string) => join(workdir, p),
    readFile: async (p: string) => Bun.file(p).text(),
    readBinaryFile: async (p: string) => new Uint8Array(await Bun.file(p).arrayBuffer()),
    writeFile: async (p: string, c: string) => {
      const { mkdir, writeFile } = await import("node:fs/promises")
      await mkdir(join(p, ".."), { recursive: true })
      await writeFile(p, c)
    },
    listFiles: async () => [],
    listDir: async () => [],
    deleteFile: async () => {},
    moveFile: async () => {},
    runCommand: async (cmd: string, o?: { workdir?: string; env?: Record<string, string>; timeoutMs?: number }) => {
      calls.push({ cmd, opts: o })
      const merged = { ...process.env, ...ctxEnv, ...(o?.env ?? {}) }
      const r = spawnSync(cmd, { shell: true, cwd: o?.workdir, env: merged, encoding: "utf-8", timeout: o?.timeoutMs ?? 60_000 })
      return {
        stdout: String(r.stdout ?? ""),
        stderr: String(r.stderr ?? (r.error ? r.error.message : "")),
        code: r.status ?? (r.error ? 127 : 1),
      }
    },
    uploadAttachment: (r: { path: string }) => Promise.resolve(r.path),
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
  return { ctx: ctx as unknown as ToolContext, calls }
}

/**
 * 一次端到端的脚手架：临时 home + 假 cmake + 假源码树 + 假 ctx。
 * PATH 里假 cmake 在最前（压过真实 cmake），真实 PATH 追加在后（假脚本里的 mkdir/chmod 要用）。
 */
function setup(o: { env?: Record<string, string> } = {}): {
  home: string
  fake: string
  sourceDir: string
  candidate: any
  ctx: ToolContext
  calls: CmdCall[]
} {
  const home = makeHome()
  const fake = makeFakeCmake()
  const { dir: sourceDir, candidate } = makeSource({ version: "b11100-test" })
  const path = `${fake}${process.platform === "win32" ? ";" : ":"}${process.env.PATH ?? ""}`
  const { ctx, calls } = makeCtx(home, { PATH: path, ...(o.env ?? {}) })
  return { home, fake, sourceDir, candidate, ctx, calls }
}

const cpuSpec = (engineId: string, extra: Partial<BuildSpec> = {}): BuildSpec => ({
  engine_id: engineId,
  device: "cpu",
  ...extra,
})

// ── 构建预设（纯函数） ────────────────────────────────────────────────────

describe("buildPreset：设备 → llama.cpp CMake 预设", () => {
  test("cpu：通用定义齐全 + 四个后端全 OFF + GGML_NATIVE=ON", () => {
    const p = buildPreset("cpu", { ninja: true, platform: "linux", jobs: 8 })
    expect(p.generator).toBe("Ninja")
    for (const flag of ["-DCMAKE_BUILD_TYPE=Release", "-DLLAMA_BUILD_TESTS=OFF", "-DLLAMA_BUILD_EXAMPLES=OFF", "-DLLAMA_BUILD_SERVER=ON", "-DLLAMA_CURL=OFF"]) {
      expect(p.cmakeArgs).toContain(flag)
    }
    // 内网必需：不引 libcurl（模型下载由 TS 层负责）
    expect(p.cmakeArgs).toContain("-DGGML_NATIVE=ON")
    expect(p.cmakeArgs).toEqual(expect.arrayContaining(["-DGGML_CUDA=OFF", "-DGGML_VULKAN=OFF", "-DGGML_METAL=OFF", "-DGGML_HIP=OFF"]))
    expect(p.cmakeArgs.some((a) => a.includes("CUDA_ARCHITECTURES"))).toBe(false)
    expect(p.cmakeArgs.some((a) => a.includes("FETCHCONTENT"))).toBe(false)
    expect(p.requirements.join(" ")).toContain("cmake ≥ 3.14")
    expect(p.requirements.join(" ")).toContain("ninja")
    expect(p.requirements.join(" ")).toContain("g++")
    expect(p.notes.join(" ")).toContain("GGML_NATIVE=ON")
  })

  test("后端开关互斥：目标后端 ON、其它 OFF，且四个后端全部显式表态", () => {
    const expectFlag: Record<BuildDevice, string> = {
      cpu: "-DGGML_CUDA=OFF",
      cuda: "-DGGML_CUDA=ON",
      vulkan: "-DGGML_VULKAN=ON",
      metal: "-DGGML_METAL=ON",
      rocm: "-DGGML_HIP=ON",
    }
    for (const device of BUILD_DEVICES) {
      const p = buildPreset(device, { ninja: true, platform: "linux" })
      const backendFlags = p.cmakeArgs.filter((a) => /^-DGGML_(CUDA|VULKAN|METAL|HIP)=(ON|OFF)$/.test(a))
      expect(backendFlags.length).toBe(4)
      expect(backendFlags.filter((a) => a.endsWith("=ON")).length).toBe(device === "cpu" ? 0 : 1)
      expect(p.cmakeArgs).toContain(expectFlag[device])
    }
  })

  test("cuda：CUDA ON + 算力 native + 其它 OFF + nvcc/cmake 版本要求", () => {
    const p = buildPreset("cuda", { ninja: true, platform: "linux" })
    expect(p.cmakeArgs).toContain("-DGGML_CUDA=ON")
    expect(p.cmakeArgs).toContain("-DCMAKE_CUDA_ARCHITECTURES=native")
    expect(p.cmakeArgs).toEqual(expect.arrayContaining(["-DGGML_VULKAN=OFF", "-DGGML_METAL=OFF", "-DGGML_HIP=OFF"]))
    expect(p.cmakeArgs).not.toContain("-DGGML_NATIVE=ON")
    expect(p.requirements.join(" ")).toContain("nvcc")
    expect(p.requirements.join(" ")).toContain("cmake ≥ 3.24")
    // 无 GPU / 交叉分发要能显式覆盖算力档位
    expect(p.notes.join(" ")).toContain("extra_cmake_args")
    expect(p.notes.join(" ")).toContain("native")
  })

  test("vulkan / metal / rocm：各自的后端开关与依赖清单", () => {
    const vk = buildPreset("vulkan", { ninja: true, platform: "linux" })
    expect(vk.cmakeArgs).toContain("-DGGML_VULKAN=ON")
    expect(vk.requirements.join(" ")).toContain("Vulkan SDK")
    expect(vk.requirements.join(" ")).toContain("ICD")

    const mt = buildPreset("metal", { ninja: true, platform: "darwin" })
    expect(mt.cmakeArgs).toContain("-DGGML_METAL=ON")
    expect(mt.requirements.join(" ")).toContain("Xcode")
    expect(mt.notes.join(" ")).toContain("darwin")

    const rocm = buildPreset("rocm", { ninja: true, platform: "linux" })
    expect(rocm.cmakeArgs).toContain("-DGGML_HIP=ON")
    expect(rocm.cmakeArgs).toContain("-DGGML_CUDA=OFF")
    expect(rocm.requirements.join(" ")).toContain("hipcc")
    // llama.cpp 的 ROCm 开关名是 GGML_HIP，notes 要写明（否则模型容易照着直觉写 GGML_ROCM）
    expect(rocm.notes.join(" ")).toContain("GGML_HIP")
  })

  test("offline：追加 FETCHCONTENT_FULLY_DISCONNECTED=ON 并说明缺依赖怎么办", () => {
    const on = buildPreset("cpu", { ninja: true, platform: "linux", offline: true })
    expect(on.cmakeArgs).toContain("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
    expect(on.cmakeArgs[on.cmakeArgs.length - 1]).toBe("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
    expect(on.notes.join(" ")).toContain("若报缺依赖")
    expect(on.notes.join(" ")).toContain("vendor/")
    const off = buildPreset("cpu", { ninja: true, platform: "linux" })
    expect(off.cmakeArgs.some((a) => a.includes("FETCHCONTENT"))).toBe(false)
  })

  test("generator 选择：注入 ninja 存在/不存在；PATH 探测与 win32 回落", () => {
    expect(buildPreset("cpu", { ninja: true, platform: "linux" }).generator).toBe("Ninja")
    expect(buildPreset("cpu", { ninja: false, platform: "linux" }).generator).toBe("Unix Makefiles")
    expect(buildPreset("cpu", { ninja: false, platform: "win32" }).generator).toBe("MinGW Makefiles")
    // 缺 ninja 的 notes 要如实说明退回原因
    expect(buildPreset("cpu", { ninja: false, platform: "linux" }).notes.join(" ")).toContain("未探测到 ninja")
    // 真实 PATH 探测：临时目录里放个 ninja，挂到 PATH 上应被探测到
    const ninjaDir = join(tmp, `fake-ninja-${rnd()}`)
    mkdirSync(ninjaDir, { recursive: true })
    writeFileSync(join(ninjaDir, "ninja"), "#!/bin/sh\nexit 0\n", "utf-8")
    expect(hasOnPath("ninja", { env: { PATH: ninjaDir }, platform: "linux" })).toBe(true)
    expect(hasOnPath("ninja", { env: { PATH: join(tmp, `empty-${rnd()}`) }, platform: "linux" })).toBe(false)
    const oldPath = process.env.PATH
    try {
      process.env.PATH = `${ninjaDir}${process.platform === "win32" ? ";" : ":"}${oldPath ?? ""}`
      expect(buildPreset("cpu", { platform: "linux" }).generator).toBe("Ninja")
      process.env.PATH = join(tmp, `empty-${rnd()}`)
      expect(buildPreset("cpu", { platform: "linux" }).generator).toBe("Unix Makefiles")
    } finally {
      process.env.PATH = oldPath
    }
  })

  test("requirements 随生成器与设备变化；notes 记录构建/源码目录", () => {
    const withNinja = buildPreset("cpu", { ninja: true, platform: "linux" })
    const noNinja = buildPreset("cpu", { ninja: false, platform: "linux" })
    expect(withNinja.requirements[1]).toContain("已探测到")
    expect(noNinja.requirements[1]).toContain("GNU make")
    expect(buildPreset("cpu", { ninja: false, platform: "win32" }).requirements[1]).toContain("mingw32-make")
    expect(buildPreset("cpu", { ninja: true, platform: "win32" }).requirements[2]).toContain("MSVC")
    expect(buildPreset("cpu", { ninja: true, platform: "darwin" }).requirements[2]).toContain("Xcode")
    const p = buildPreset("cpu", { ninja: true, platform: "linux", jobs: 5, buildDir: "/s/build-cpu", sourceDir: "/s" })
    expect(p.notes.join(" ")).toContain("/s/build-cpu")
    expect(p.notes.join(" ")).toContain("build-<device>")
    expect(p.notes.join(" ")).toContain("并行 5")
  })

  test("defaultJobs：CPU 核数的 75%，至少 1", () => {
    expect(defaultJobs(8)).toBe(6)
    expect(defaultJobs(4)).toBe(3)
    expect(defaultJobs(1)).toBe(1)
    expect(defaultJobs(0)).toBe(1)
    expect(defaultJobs(Number.NaN)).toBe(1)
    expect(defaultJobs()).toBeGreaterThanOrEqual(1)
  })
})

// ── 命令组装（纯函数） ────────────────────────────────────────────────────

describe("cmake 命令组装", () => {
  test("configure：-S/-B/-G 与全部 -D 定义，且能透传 extra args", () => {
    const preset = buildPreset("cpu", { ninja: true, platform: "linux", jobs: 8 })
    const cmd = cmakeConfigureCmd({ cmake: "cmake", sourceDir: "/opt/src/llama", buildDir: "/opt/src/llama/build-cpu", preset })
    expect(cmd.startsWith("cmake -S /opt/src/llama -B /opt/src/llama/build-cpu -G Ninja ")).toBe(true)
    expect(cmd).toContain("-DCMAKE_BUILD_TYPE=Release")
    expect(cmd).toContain("-DLLAMA_BUILD_SERVER=ON")
    expect(cmd).toContain("-DGGML_CUDA=OFF")
    // extra args（工具层追加）在末尾——同名的 -D 以最后一条为准，可覆盖预设
    const withExtra = { ...preset, cmakeArgs: [...preset.cmakeArgs, "-DCMAKE_CUDA_ARCHITECTURES=89", "-DGGML_NATIVE=OFF"] }
    const c2 = cmakeConfigureCmd({ cmake: "cmake", sourceDir: "/s", buildDir: "/s/build-cpu", preset: withExtra })
    expect(c2.endsWith("-DCMAKE_CUDA_ARCHITECTURES=89 -DGGML_NATIVE=OFF")).toBe(true)
  })

  test("configure：含空格/带空格的生成器自动加引号", () => {
    const preset = buildPreset("cpu", { ninja: true, platform: "linux" })
    const cmd = cmakeConfigureCmd({ cmake: "cmake", sourceDir: "/tmp/a b/src", buildDir: "/tmp/a b/build", preset })
    expect(cmd).toContain('-S "/tmp/a b/src" -B "/tmp/a b/build"')
    const mingw = cmakeConfigureCmd({ cmake: "cmake", sourceDir: "/s", buildDir: "/b", preset: buildPreset("cpu", { ninja: false, platform: "win32" }) })
    expect(mingw).toContain('-G "MinGW Makefiles"')
  })

  test("build：--build + --config Release + --parallel N", () => {
    expect(cmakeBuildCmd({ cmake: "cmake", buildDir: "/s/build-cpu", jobs: 7 })).toBe("cmake --build /s/build-cpu --config Release --parallel 7")
    expect(cmakeBuildCmd({ cmake: "cmake", buildDir: "/b", jobs: 0 })).toContain("--parallel 1")
    expect(cmakeBuildCmd({ cmake: "cmake", buildDir: "/b", jobs: 2.9 })).toContain("--parallel 2")
  })
})

// ── 端到端：假源码树 + 假 cmake ───────────────────────────────────────────

describe("buildFromSource：编译 + 安装（假 cmake）", () => {
  test("成功路径：装到 vendor/<id>/bin、标记字段兼容 engines.ts、日志与进度节点齐全", async () => {
    const s = setup()
    const seen: Array<{ phase: string; message: string; secs?: number }> = []
    const r = await buildFromSource({
      home: s.home,
      candidate: s.candidate,
      spec: cpuSpec("linux-cpu-srcbuild"),
      ctx: s.ctx,
      onStep: (step) => {
        seen.push(step)
      },
    })

    expect(r.ok).toBe(true)
    expect(r.error).toBeUndefined()
    expect(r.engineId).toBe("linux-cpu-srcbuild")
    const dir = engineDir(s.home, "linux-cpu-srcbuild")
    // 安装落点：bin/ 下带可执行文件 + 同目录运行时依赖（*.so*）
    expect(existsSync(join(dir, "bin", "llama-server"))).toBe(true)
    expect(existsSync(join(dir, "bin", "libllama.so"))).toBe(true)
    expect(existsSync(join(dir, "bin", "libllama.so.0"))).toBe(true)
    expect(r.engine?.exe).toBe(join(dir, "bin", "llama-server"))
    expect(r.engine?.dir).toBe(dir)
    expect(r.engine?.tag).toBe("b11100-test")
    expect(r.engine?.asset).toBe("(source build)")
    if (process.platform !== "win32") {
      expect(Number(statSafe(join(dir, "bin", "llama-server"))?.mode ?? 0) & 0o111).toBeGreaterThan(0)
    }

    // 标记文件：InstalledEngine 字段 + build 来源信息
    const marker = JSON.parse(readFileSync(join(dir, ".engine.json"), "utf-8")) as any
    expect(marker.id).toBe("linux-cpu-srcbuild")
    expect(marker.dir).toBe(dir)
    expect(typeof marker.installed_at).toBe("string")
    expect(marker.build.from_source).toBe(true)
    expect(marker.build.source_dir).toBe(s.sourceDir)
    expect(marker.build.build_dir).toBe(join(s.sourceDir, "build-cpu"))
    expect(marker.build.device).toBe("cpu")
    expect(marker.build.offline).toBe(false)
    expect(marker.build.log).toBe(r.logPath)
    expect(marker.build.host).toBe(`${process.platform}-${process.arch}`)
    expect(marker.build.jobs).toBeGreaterThanOrEqual(1)
    expect(marker.build.configure_cmd).toContain(`-S ${s.sourceDir} -B ${join(s.sourceDir, "build-cpu")}`)
    expect(marker.build.build_cmd).toContain("--parallel")
    // 假源码树没有 .git：不写 commit
    expect(marker.build.commit).toBeUndefined()

    // 关键契约：下载安装层（engines.ts）能读出这个引擎（同一落点 + 同一标记格式）
    const inst = installedEngine(s.home, "linux-cpu-srcbuild")
    expect(inst).not.toBeNull()
    expect(inst?.exe).toBe(join(dir, "bin", "llama-server"))
    expect(inst?.tag).toBe("b11100-test")
    expect(inst?.asset).toBe("(source build)")

    // 日志：命令行原文 + 退出码都在
    expect(r.logPath).toBeDefined()
    expect(existsSync(r.logPath!)).toBe(true)
    const log = readFileSync(r.logPath!, "utf-8")
    expect(log).toContain("local_infer 源码编译安装")
    // 日志：命令行原文 + 退出码都在（cmake 可能是 PATH 解析名，也可能是探测到的绝对路径）
    expect(log).toMatch(/\$ \S*cmake -S /)
    expect(log).toContain("configure")
    expect(log).toContain("build")
    expect(log).toContain("exit=0")

    // 进度节点：相位齐全、末节点 done、完成节点带耗时、onStep 与返回的 steps 一致
    const phases = r.steps.map((x) => x.phase)
    expect(phases[0]).toBe("extract")
    for (const p of ["configure", "build", "install", "done"]) expect(phases).toContain(p)
    expect(phases[phases.length - 1]).toBe("done")
    expect(r.steps.filter((x) => x.secs != null).length).toBe(2) // configure/build 的完成节点带耗时
    expect(r.steps.filter((x) => x.secs != null).every((x) => typeof x.secs === "number" && x.secs >= 0)).toBe(true)
    expect(seen.map((x) => x.phase)).toEqual(phases)

    // 真跑过的命令：configure 一条 + build 一条
    expect(s.calls.some((c) => c.cmd.includes("-S ") && c.cmd.includes("-B ") && !c.cmd.includes("--build"))).toBe(true)
    expect(s.calls.some((c) => c.cmd.includes("--build") && c.cmd.includes("--parallel"))).toBe(true)
    // 临时阶段目录不留残留（安装走 rename，半成品不落位）
    expect(readdirSafe(vendorDir(s.home)).filter((n) => n.startsWith(".stage-"))).toEqual([])
  })

  test("extra_cmake_args 透传到 configure 且排在预设之后（可覆盖算力档位）", async () => {
    const s = setup()
    const r = await buildFromSource({
      home: s.home,
      candidate: s.candidate,
      spec: cpuSpec("linux-cpu-extra", { extra_cmake_args: ["-DGGML_NATIVE=OFF", "-DCMAKE_CUDA_ARCHITECTURES=89"] }),
      ctx: s.ctx,
    })
    expect(r.ok).toBe(true)
    const configure = s.calls.find((c) => c.cmd.includes("-B ") && !c.cmd.includes("--build"))
    expect(configure).toBeDefined()
    expect(configure!.cmd).toContain("-DGGML_NATIVE=OFF")
    expect(configure!.cmd).toContain("-DCMAKE_CUDA_ARCHITECTURES=89")
    expect(configure!.cmd.indexOf("-DGGML_NATIVE=OFF")).toBeGreaterThan(configure!.cmd.indexOf("-DGGML_NATIVE=ON"))
    expect(configure!.cmd.indexOf("-DCMAKE_CUDA_ARCHITECTURES=89")).toBeGreaterThan(configure!.cmd.indexOf("-DGGML_HIP=OFF"))
  })

  test("offline=true：断网开关进 configure 命令与标记（内网缺依赖立即失败）", async () => {
    const s = setup()
    const r = await buildFromSource({ home: s.home, candidate: s.candidate, spec: cpuSpec("linux-cpu-offline", { offline: true }), ctx: s.ctx })
    expect(r.ok).toBe(true)
    const configure = s.calls.find((c) => c.cmd.includes("-B ") && !c.cmd.includes("--build"))
    expect(configure).toBeDefined()
    expect(configure!.cmd).toContain("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
    expect((r.engine as any)?.build?.offline).toBe(true)
  })

  test("幂等重入：已安装则第二次跳过编译、不重复 configure", async () => {
    const s = setup()
    const spec = cpuSpec("linux-cpu-idem")
    const first = await buildFromSource({ home: s.home, candidate: s.candidate, spec, ctx: s.ctx })
    expect(first.ok).toBe(true)
    const callsAfterFirst = s.calls.length
    expect(callsAfterFirst).toBeGreaterThan(0)

    const second = await buildFromSource({ home: s.home, candidate: s.candidate, spec, ctx: s.ctx })
    expect(second.ok).toBe(true)
    expect(second.engine?.exe).toBe(first.engine?.exe)
    expect(second.logPath).toBe(first.logPath)
    expect(second.buildDir).toBe(first.buildDir)
    expect(second.steps.map((x) => x.phase)).toEqual(["done"])
    expect(second.steps[0]?.message).toContain("跳过编译")
    expect(s.calls.length).toBe(callsAfterFirst) // 没有重新 configure/build
  })

  test("clean=true：删掉旧构建目录重配，并整体替换旧安装目录", async () => {
    const s = setup()
    const spec = cpuSpec("linux-cpu-clean")
    await buildFromSource({ home: s.home, candidate: s.candidate, spec, ctx: s.ctx })
    // 造残留：构建目录哨兵 + 旧安装目录哨兵
    writeFileSync(join(s.sourceDir, "build-cpu", "stale.txt"), "old", "utf-8")
    writeFileSync(join(engineDir(s.home, "linux-cpu-clean"), "stale-install.txt"), "old", "utf-8")

    const r = await buildFromSource({ home: s.home, candidate: s.candidate, spec: { ...spec, clean: true }, ctx: s.ctx })
    expect(r.ok).toBe(true)
    expect(existsSync(join(s.sourceDir, "build-cpu", "stale.txt"))).toBe(false)
    expect(existsSync(join(engineDir(s.home, "linux-cpu-clean"), "stale-install.txt"))).toBe(false)
    expect(existsSync(join(engineDir(s.home, "linux-cpu-clean"), "bin", "llama-server"))).toBe(true)
  })

  test("exe 落在构建目录根时：只抄运行时依赖，不带构建垃圾", async () => {
    const s = setup({ env: { FAKE_CMAKE_EXE_AT_ROOT: "1" } })
    const r = await buildFromSource({ home: s.home, candidate: s.candidate, spec: cpuSpec("linux-cpu-root"), ctx: s.ctx })
    expect(r.ok).toBe(true)
    const dir = engineDir(s.home, "linux-cpu-root")
    expect(existsSync(join(dir, "bin", "llama-server"))).toBe(true)
    expect(existsSync(join(dir, "bin", "libllama.so"))).toBe(true)
    // 构建系统产物不进安装目录
    expect(existsSync(join(dir, "bin", "CMakeFiles"))).toBe(false)
    expect(existsSync(join(dir, "bin", "junk.o"))).toBe(false)
    expect(existsSync(join(dir, "build-cpu"))).toBe(false)
  })

  test("commit：源码带 .git 时记录 HEAD（分离头/ref 文件两种形态）", async () => {
    const s = setup()
    mkdirSync(join(s.sourceDir, ".git", "refs", "heads"), { recursive: true })
    writeFileSync(join(s.sourceDir, ".git", "HEAD"), "ref: refs/heads/main\n", "utf-8")
    writeFileSync(join(s.sourceDir, ".git", "refs", "heads", "main"), "0123456789abcdef0123456789abcdef01234567\n", "utf-8")
    const r = await buildFromSource({ home: s.home, candidate: s.candidate, spec: cpuSpec("linux-cpu-git"), ctx: s.ctx })
    expect(r.ok).toBe(true)
    expect((r.engine as any)?.build?.commit).toBe("0123456789ab")

    const s2 = setup()
    mkdirSync(join(s2.sourceDir, ".git"), { recursive: true })
    writeFileSync(join(s2.sourceDir, ".git", "HEAD"), "fedcba9876543210fedcba9876543210fedcba98\n", "utf-8")
    const r2 = await buildFromSource({ home: s2.home, candidate: s2.candidate, spec: cpuSpec("linux-cpu-git2"), ctx: s2.ctx })
    expect((r2.engine as any)?.build?.commit).toBe("fedcba987654")
  })

  test("tag：候选没给版本时用 CMakeLists 的 project VERSION 兜底", async () => {
    const home = makeHome()
    const fake = makeFakeCmake()
    const { dir, candidate } = makeSource({ cmakeVersion: "3.7.2" })
    const { ctx } = makeCtx(home, { PATH: `${fake}${process.platform === "win32" ? ";" : ":"}${process.env.PATH ?? ""}` })
    const r = await buildFromSource({ home, candidate, spec: cpuSpec("linux-cpu-ver"), ctx })
    expect(r.ok).toBe(true)
    expect(r.engine?.tag).toBe("3.7.2")
    expect(dir).toBe(candidate.path)
  })
})

// ── 失败路径与半成品清理 ──────────────────────────────────────────────────

describe("buildFromSource：失败路径", () => {
  test("configure 失败：error 带日志尾部 60 行；不留任何半成品", async () => {
    const s = setup({ env: { FAKE_CMAKE_CONFIGURE_EXIT: "1", FAKE_CMAKE_LOG_LINES: "100" } })
    const r = await buildFromSource({ home: s.home, candidate: s.candidate, spec: cpuSpec("linux-cpu-cfgfail"), ctx: s.ctx })

    expect(r.ok).toBe(false)
    expect(r.error).toContain("configure 失败（exit 1）")
    expect(r.error).toContain("日志尾部 60 行")
    expect(r.error).toContain("log-line-100") // 尾部在
    expect(r.error).not.toContain("log-line-1\n") // 头部被截掉
    expect(r.error).toContain("依赖清单")
    expect(r.error).toContain("cmake")
    expect(r.error).not.toMatch(/\n\s+at\s+\S+/) // 不是异常栈
    expect(r.logPath).toBeDefined()
    expect(existsSync(r.logPath!)).toBe(true)
    expect(r.steps.map((x) => x.phase)).toContain("configure")
    expect(r.steps.map((x) => x.phase)).not.toContain("done")
    // 半成品：vendor/<id> 不存在、无 .stage-* 残留、engines 层判未安装
    expect(existsSync(engineDir(s.home, "linux-cpu-cfgfail"))).toBe(false)
    expect(readdirSafe(vendorDir(s.home)).filter((n) => n.startsWith(".stage-"))).toEqual([])
    expect(installedEngine(s.home, "linux-cpu-cfgfail")).toBeNull()
  })

  test("build 失败：error 带尾部；configure 已产出的可执行文件不会被安装", async () => {
    const s = setup({ env: { FAKE_CMAKE_BUILD_EXIT: "2", FAKE_CMAKE_LOG_LINES: "100" } })
    const r = await buildFromSource({ home: s.home, candidate: s.candidate, spec: cpuSpec("linux-cpu-bdfail"), ctx: s.ctx })

    expect(r.ok).toBe(false)
    expect(r.error).toContain("build 失败（exit 2）")
    expect(r.error).toContain("log-line-100")
    expect(r.steps.map((x) => x.phase)).toContain("build")
    expect(existsSync(join(s.sourceDir, "build-cpu", "bin", "llama-server"))).toBe(true) // 构建产物还在
    expect(existsSync(engineDir(s.home, "linux-cpu-bdfail"))).toBe(false) // 但没被安装
    expect(readdirSafe(vendorDir(s.home)).filter((n) => n.startsWith(".stage-"))).toEqual([])
  })

  test("构建成功但没产出 llama-server：明确报出来，且不写标记", async () => {
    const s = setup({ env: { FAKE_CMAKE_NO_EXE: "1" } })
    const r = await buildFromSource({ home: s.home, candidate: s.candidate, spec: cpuSpec("linux-cpu-noexe"), ctx: s.ctx })
    expect(r.ok).toBe(false)
    expect(r.error).toContain("没找到 llama-server")
    expect(r.error).toContain("LLAMA_BUILD_SERVER")
    expect(existsSync(engineDir(s.home, "linux-cpu-noexe"))).toBe(false)
    expect(installedEngine(s.home, "linux-cpu-noexe")).toBeNull()
  })

  test("源码候选不可用（归档 + 发现层解压失败）：不崩、给可读错误、不跑编译", async () => {
    const s = setup()
    const archive = join(tmp, `llama-src-${rnd()}.tar.gz`)
    writeFileSync(archive, "not really an archive", "utf-8")
    const r = await buildFromSource({
      home: s.home,
      candidate: { id: "archived-src", kind: "llama_cpp", path: archive, root: tmp, evidence: [], archive: { path: archive, format: "tar.gz", bytes: 20 } },
      spec: cpuSpec("linux-cpu-archive"),
      ctx: s.ctx,
    })
    expect(r.ok).toBe(false)
    expect(typeof r.error).toBe("string")
    expect((r.error ?? "").length).toBeGreaterThan(0)
    expect(r.error).not.toMatch(/\n\s+at\s+\S+/)
    expect(s.calls.some((c) => c.cmd.includes("-S ") || c.cmd.includes("--build"))).toBe(false)
    expect(existsSync(engineDir(s.home, "linux-cpu-archive"))).toBe(false)
  })

  test("源码目录没有 CMakeLists.txt：拒绝并说明", async () => {
    const s = setup()
    const empty = join(tmp, `empty-src-${rnd()}`)
    mkdirSync(empty, { recursive: true })
    const r = await buildFromSource({
      home: s.home,
      candidate: { id: "empty", kind: "unknown", path: empty, root: tmp, evidence: [] },
      spec: cpuSpec("linux-cpu-nocmake"),
      ctx: s.ctx,
    })
    expect(r.ok).toBe(false)
    expect(r.error).toContain("CMakeLists.txt")
    expect(s.calls.some((c) => c.cmd.includes("-S ") || c.cmd.includes("--build"))).toBe(false)
  })

  test("参数校验：空 engine_id / 非法 engine_id / 未知设备都在跑命令前失败", async () => {
    const s = setup()
    const noId = await buildFromSource({ home: s.home, candidate: s.candidate, spec: { engine_id: "  ", device: "cpu" }, ctx: s.ctx })
    expect(noId.ok).toBe(false)
    expect(noId.error).toContain("engine_id 为空")

    const escape = await buildFromSource({ home: s.home, candidate: s.candidate, spec: { engine_id: "../escape", device: "cpu" }, ctx: s.ctx })
    expect(escape.ok).toBe(false)
    expect(escape.error).toContain("engine_id 不合法")

    const badDevice = await buildFromSource({ home: s.home, candidate: s.candidate, spec: { engine_id: "x", device: "tpu" as BuildDevice }, ctx: s.ctx })
    expect(badDevice.ok).toBe(false)
    expect(badDevice.error).toContain("未知设备后端")

    const noHome = await buildFromSource({ home: "", candidate: s.candidate, spec: cpuSpec("x"), ctx: s.ctx })
    expect(noHome.ok).toBe(false)
    expect(s.calls.length).toBe(0)
  })

  test("工具链门禁：cuda 缺 nvcc 先失败并给 requirements，不跑 configure/build", async () => {
    // 只把假 cmake 留在 PATH 上（真机装了 nvcc/gcc，这里一并隐藏，才能走「缺工具链」分支）
    const home = makeHome()
    const fake = makeFakeCmake()
    const { dir: sourceDir, candidate } = makeSource({ version: "b11100-cuda" })
    const { ctx, calls } = makeCtx(home, { PATH: fake })
    const r = await buildFromSource({ home, candidate, spec: { engine_id: "linux-cuda-srcbuild", device: "cuda" }, ctx })
    expect(r.ok).toBe(false)
    expect(r.error).toContain("工具链不满足")
    expect(r.error).toContain("nvcc")
    expect(r.error).toContain("NVIDIA CUDA Toolkit")
    expect(calls.some((c) => c.cmd.includes("nvcc"))).toBe(true)
    expect(calls.some((c) => c.cmd.includes("--build"))).toBe(false)
    expect(existsSync(join(sourceDir, "build-cuda"))).toBe(false) // 门禁不过不动构建目录
    expect(existsSync(engineDir(home, "linux-cuda-srcbuild"))).toBe(false)
  })

  test("工具链门禁：cmake 不在 PATH 上先失败（含内网补齐提示）", async () => {
    const s = setup({ env: { PATH: join(tmp, `no-tools-${rnd()}`) } })
    const r = await buildFromSource({ home: s.home, candidate: s.candidate, spec: cpuSpec("linux-cpu-nocmake-bin"), ctx: s.ctx })
    expect(r.ok).toBe(false)
    expect(r.error).toContain("工具链不满足")
    expect(r.error).toContain("cmake")
    expect(r.error).toContain("需要：")
    expect(r.steps.map((x) => x.phase)).toContain("extract")
    expect(existsSync(engineDir(s.home, "linux-cpu-nocmake-bin"))).toBe(false)
  })
})

// ── 纯函数：产物定位 ──────────────────────────────────────────────────────

describe("locateBuiltServer", () => {
  test("递归找（含 MSVC 的 bin/Release）与 build/bin 兜底", () => {
    const root = join(tmp, `build-${rnd()}`)
    mkdirSync(join(root, "bin", "Release"), { recursive: true })
    expect(locateBuiltServer(root)).toBeNull()
    const exe = process.platform === "win32" ? "llama-server.exe" : "llama-server"
    writeFileSync(join(root, "bin", "Release", exe), "x", "utf-8")
    expect(locateBuiltServer(root)).toBe(join(root, "bin", "Release", exe))

    const root2 = join(tmp, `build-${rnd()}`)
    mkdirSync(join(root2, "bin"), { recursive: true })
    writeFileSync(join(root2, "bin", exe), "x", "utf-8")
    expect(locateBuiltServer(root2)).toBe(join(root2, "bin", exe))
    expect(locateBuiltServer(join(tmp, `nope-${rnd()}`))).toBeNull()
  })
})

// ── 无 ctx 兜底（本地子进程） ─────────────────────────────────────────────

describe("buildFromSource：无 ctx 时的本地兜底", () => {
  test.skipIf(process.platform === "win32")("不传 ctx（脚本直调）也能完成编译安装", async () => {
    const home = makeHome()
    const fake = makeFakeCmake()
    const { candidate } = makeSource({ version: "b11100-noc ctx" })
    const oldPath = process.env.PATH
    process.env.PATH = `${fake}${process.platform === "win32" ? ";" : ":"}${oldPath ?? ""}`
    try {
      const r = await buildFromSource({ home, candidate, spec: cpuSpec("noctx-cpu") })
      expect(r.ok).toBe(true)
      const dir = engineDir(home, "noctx-cpu")
      expect(existsSync(join(dir, "bin", "llama-server"))).toBe(true)
      expect(installedEngine(home, "noctx-cpu")?.exe).toBe(join(dir, "bin", "llama-server"))
      // 无 ctx：工具链探测跳过（不假报失败），但 configure/build 真跑了
      expect(r.steps.map((x) => x.phase)).toContain("done")
      const log = readFileSync(r.logPath!, "utf-8")
      expect(log).toContain("exit=0")
    } finally {
      process.env.PATH = oldPath
    }
  })
})
