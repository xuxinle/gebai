/**
 * local_infer 的**引擎层**：把「用哪个 llama.cpp、跑在什么设备上、从哪来」从代码里解耦成
 * `infer/config/engines.json` 的引擎矩阵，并负责**跨平台**的引擎获取（下载 + 校验 + 解压 + 安装）。
 *
 * 设计要点：
 *   * 推理不绑定 Windows / 不绑定 GPU——矩阵覆盖 linux/win32/darwin × cpu/cuda/vulkan/metal/rocm/sycl，
 *     按当前平台与实测设备探测结果挑可用引擎；缺引擎时按矩阵里的发行版资产直接下载安装。
 *   * 安装落点 `infer/vendor/<engine-id>/`，标记文件 `.engine.json` 记录来源 tag / 资产名 / exe 相对路径——
 *     安装状态**以文件系统为准**（可重建、可审计、可多引擎并存）。
 *   * 下载全部走 TS（fetch + 断点续传 + 大小/sha256 校验），不依赖平台特定工具；
 *     解压 zip 用 fflate（已有依赖）、tar.gz 用系统 tar（Windows 10+ 自带 bsdtar）。
 *   * 本文件是「在哪里、怎么装」的唯一实现；「怎么起、怎么停」在 launcher.ts，「怎么调」在 api.ts/providers.ts。
 *
 * 实现约定（本层自我约束）：
 *   * **不抛错边界**：矩阵读取、安装状态读取、设备探测一律「失败即降级」（返回 null / false / 记录依据），
 *     错误信息留给工具层组织成人话——模型侧看到的必须是「下一步怎么办」而不是异常栈。
 *   * **纯函数可注入**：平台（`detectPlatform(platform, arch)`）、设备节点根（`detectDevices(ctx, platform, {devDir})`）、
 *     网络（`fetchImpl`）都可注入——测试可完全离线且不随宿主漂移（见 engines.test.ts）。
 *   * **产物原子性**：一切写入都先落临时文件/临时目录，校验通过再 rename 到最终路径；
 *     失败清理半成品，产物路径上永不出现未完成内容。
 */
import { spawn } from "node:child_process"
import { createHash } from "node:crypto"
import {
  chmodSync,
  closeSync,
  existsSync,
  mkdirSync,
  openSync,
  readdirSync,
  readFileSync,
  readSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
  writeSync,
} from "node:fs"
import { cpus } from "node:os"
import { basename, dirname, join, relative } from "node:path"
import { unzipSync } from "fflate"
import type { Tool, ToolContext, ToolSchema } from "@gebai/sdk"
import { downloadCacheDir, engineDir, inferHome, modelsDir, vendorDir } from "./paths"

// ── 引擎矩阵（infer/config/engines.json） ────────────────────────────────

/** 引擎所属平台标识（与 Node process.platform 对齐）。 */
export type EnginePlatform = "linux" | "win32" | "darwin"

/** 引擎设备后端。 */
export type EngineDevice = "cpu" | "cuda" | "vulkan" | "metal" | "rocm" | "sycl" | "openvino"

/** 单个引擎定义（矩阵条目）。 */
export interface EngineDef {
  /** 引擎 id（稳定标识，用作 vendor 子目录名），如 `linux-cpu-x64`、`win-cuda-12.4-x64`。 */
  id: string
  platform: EnginePlatform
  /** CPU 架构：x64 / arm64。 */
  arch: "x64" | "arm64"
  device: EngineDevice
  /** 发行版资产文件名（llama.cpp release 资产）。 */
  asset: string
  /** 归档格式。 */
  archive: "tar.gz" | "zip"
  /** CUDA 引擎需要的 cudart 资产（分开发布，装到同一目录）。 */
  cudart_asset?: string
  /** 该引擎的说明（人读：为什么选它、有什么约束）。 */
  notes?: string
  /** 依赖提示（如 cuda 需要驱动版本、vulkan 需要 ICD）。 */
  requires?: string
}

/** 引擎矩阵文件结构。 */
export interface EngineMatrix {
  version: number
  /** llama.cpp 发行版基准（tag + 下载根）。 */
  release: { llama_cpp: { tag: string; base: string } }
  engines: EngineDef[]
}

/** 已安装引擎（标记文件 `.engine.json` 的内容）。 */
export interface InstalledEngine {
  id: string
  /** llama-server 可执行文件绝对路径。 */
  exe: string
  tag: string
  asset: string
  installed_at: string
  /** 安装目录（vendor/<id>）。 */
  dir: string
}

/** 本机平台探测结果。 */
export interface DeviceInfo {
  cpu_cores: number
  cuda: boolean
  vulkan: boolean
  metal: boolean
  /** 探测依据（人读，落进工具输出便于排障）。 */
  notes: string[]
}

/** 下载进度回调。 */
export type DownloadProgress = (p: { received: number; total?: number; phase: string }) => void

/** 已安装引擎的标记文件内容（可执行文件所在目录外还会带 cudart 标记，供排查 CUDA 运行时是否齐备）。 */
type InstalledMarker = InstalledEngine & { cudart?: boolean }

// ── 矩阵与地址 ────────────────────────────────────────────────────────────

/** 引擎矩阵文件路径。 */
export function enginesConfigPath(home: string): string {
  return join(home, "config", "engines.json")
}

/** 读取引擎矩阵（缺失或损坏返回 null）。 */
export function loadEngineMatrix(home: string): EngineMatrix | null {
  const p = enginesConfigPath(home)
  if (!existsSync(p)) return null
  try {
    const raw = JSON.parse(readFileSync(p, "utf-8")) as EngineMatrix
    if (!raw || typeof raw !== "object") return null
    const rel = raw.release?.llama_cpp
    // 形状校验：地址拼接依赖 tag/base，引擎清单必须是非空数组——不合法一律当「缺失」处理（调用方给提示）
    if (!rel || typeof rel.tag !== "string" || typeof rel.base !== "string") return null
    if (!Array.isArray(raw.engines) || !raw.engines.length) return null
    return raw
  } catch {
    return null
  }
}

/** 按 id 找引擎定义。 */
export function findEngine(matrix: EngineMatrix, id: string): EngineDef | undefined {
  return matrix.engines.find((e) => e && e.id === id)
}

/** 引擎资产下载地址。 */
export function engineAssetUrl(matrix: EngineMatrix, def: EngineDef): string {
  const { base, tag } = matrix.release.llama_cpp
  return `${base.replace(/\/+$/, "")}/${tag}/${def.asset}`
}

/** cudart 资产地址（CUDA 引擎的运行时在上游是独立资产，同样按 {base}/{tag}/{asset} 拼；无则 undefined）。 */
export function cudartAssetUrl(matrix: EngineMatrix, def: EngineDef): string | undefined {
  return def.cudart_asset ? engineAssetUrl(matrix, { ...def, asset: def.cudart_asset }) : undefined
}

/** 本机平台与架构（x64/arm64 归一）。 */
export function detectPlatform(
  platform: string = process.platform,
  arch: string = process.arch,
): { platform: EnginePlatform; arch: "x64" | "arm64" } {
  // 平台只分 linux/win32/darwin（与矩阵发行粒度一致）：其它平台（freebsd/android/…）按 linux 兼容值处理——
  // 与其抛错，不如照常列出矩阵、由人显式点名引擎（矩阵里没有就明确说「无本机条目」）。
  const p: EnginePlatform = platform === "win32" ? "win32" : platform === "darwin" ? "darwin" : "linux"
  const a = String(arch).toLowerCase()
  return { platform: p, arch: a === "arm64" || a === "aarch64" ? "arm64" : "x64" }
}

/** 已安装引擎（标记文件缺失或 exe 不存在返回 null）。 */
export function installedEngine(home: string, id: string): InstalledEngine | null {
  const dir = engineDir(home, id)
  const marker = join(dir, ".engine.json")
  if (!existsSync(marker)) return null
  try {
    const j = JSON.parse(readFileSync(marker, "utf-8")) as InstalledMarker
    if (!j || typeof j.exe !== "string" || !j.exe) return null
    // exe 被删/目录被清 → 视为未安装（安装状态以文件系统为准，标记只是索引）
    if (!existsSync(j.exe)) return null
    return { ...j, id: j.id || id, dir: j.dir || dir }
  } catch {
    return null
  }
}

// ── 本机设备探测 ──────────────────────────────────────────────────────────

interface ProbeResult {
  code: number
  stdout: string
  stderr: string
  /** 命令本身不存在（ENOENT）——与「命令存在但失败」区分，notes 里说法不同。 */
  missing: boolean
}

/** 进程外只读命令探测：ctx.runCommand 优先（宿主统一审记/超时）；无 ctx 时直连 spawn。两条路径都不抛错。 */
async function probeCommand(cmd: string, args: string[], ctx?: ToolContext, timeoutMs = 15000): Promise<ProbeResult> {
  if (ctx) {
    try {
      const r = await ctx.runCommand([cmd, ...args].join(" "), { timeoutMs })
      const err = r.stderr ?? ""
      return { code: r.code, stdout: r.stdout ?? "", stderr: err, missing: /ENOENT|not recognized|not found|不是内部或外部命令/i.test(err) }
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      return { code: 127, stdout: "", stderr: msg, missing: /ENOENT|not found|不存在|找不到/i.test(msg) }
    }
  }
  return spawnCapture(cmd, args, timeoutMs)
}

/** 直接 spawn 采集输出（无 ctx 时的探测路径；命令缺失走 'error' 事件，不抛错）。 */
function spawnCapture(cmd: string, args: string[], timeoutMs: number): Promise<ProbeResult> {
  return new Promise((resolve) => {
    let child: ReturnType<typeof spawn>
    try {
      child = spawn(cmd, args, { stdio: ["ignore", "pipe", "pipe"], windowsHide: true })
    } catch (e) {
      resolve({ code: 127, stdout: "", stderr: e instanceof Error ? e.message : String(e), missing: true })
      return
    }
    let stdout = ""
    let stderr = ""
    let settled = false
    let timer: ReturnType<typeof setTimeout> | undefined
    const done = (r: ProbeResult): void => {
      if (settled) return
      settled = true
      if (timer) clearTimeout(timer)
      resolve(r)
    }
    timer = setTimeout(() => {
      try {
        child.kill()
      } catch {
        /* 进程可能已退出 */
      }
      done({ code: -1, stdout, stderr: `${stderr}（超时 ${timeoutMs} ms）`, missing: false })
    }, timeoutMs)
    child.stdout?.on("data", (d: Buffer) => {
      stdout += String(d)
    })
    child.stderr?.on("data", (d: Buffer) => {
      stderr += String(d)
    })
    child.on("error", (e) => done({ code: 127, stdout, stderr: stderr || e.message, missing: true }))
    child.on("close", (code) => done({ code: code ?? -1, stdout, stderr, missing: false }))
  })
}

/** POSIX 风格拼接（设备节点路径跨平台展示一致：Windows 上这些节点本就不存在，无需 win32 语义）。 */
function posixJoin(dir: string, name: string): string {
  return `${dir.replace(/[\\/]+$/, "")}/${name}`
}

/** 首行/短摘（探测依据只留一行，避免把整段驱动输出灌进工具结果）。 */
function firstLine(s: string, max = 120): string {
  const line = String(s ?? "").split(/\r?\n/).find((l) => l.trim()) ?? ""
  const t = line.trim()
  return t.length > max ? `${t.slice(0, max)}…` : t
}

/** DRM render 节点（Vulkan 可用性的必要条件之一）。 */
function renderNodes(devDir: string): string[] {
  const dri = posixJoin(devDir, "dri")
  try {
    if (!existsSync(dri)) return []
    return readdirSync(dri)
      .filter((f) => /^renderD\d+$/.test(f))
      .map((f) => `dri/${f}`)
  } catch {
    return []
  }
}

/**
 * 本机设备探测（nvidia-smi / 设备节点 / vulkaninfo / sysctl）。
 * 每条结论都在 notes 里附探测依据（命令 + 结果），便于排障时判断「是没装还是没驱动」。
 * platform 与设备节点根可注入（测试用；默认取本机平台与 /dev）。
 */
export async function detectDevices(
  ctx?: ToolContext,
  platform: EnginePlatform = detectPlatform().platform,
  opts?: { devDir?: string },
): Promise<DeviceInfo> {
  const devDir = opts?.devDir ?? "/dev"
  const notes: string[] = []
  const cores = Math.max(1, cpus().length || 1)
  notes.push(`CPU：${cores} 核（os.cpus().length）`)

  // CUDA：驱动命令为主证据、设备节点为旁证（容器里常有节点无驱动，反之显卡直通也可能 smi 缺失）
  let cuda = false
  const smi = await probeCommand("nvidia-smi", ["--query-gpu=name", "--format=csv,noheader"], ctx)
  if (smi.code === 0) {
    cuda = true
    notes.push(`CUDA：nvidia-smi 可用（${firstLine(smi.stdout) || "无输出"}）`)
  } else if (smi.missing) {
    notes.push("nvidia-smi 不可用：not found")
  } else {
    notes.push(`nvidia-smi 退出码 ${smi.code}：${firstLine(smi.stderr) || "无输出"}`)
  }
  const nvidiaNode = posixJoin(devDir, "nvidia0")
  if (existsSync(nvidiaNode)) {
    cuda = true
    notes.push(`CUDA：设备节点 ${nvidiaNode} 存在`)
  } else {
    notes.push(`设备节点 ${nvidiaNode} 不存在`)
  }

  // Vulkan：Windows 下不探（本层不引入平台特有的只读查询）；Linux 用 vulkaninfo，再看 DRM render 节点
  let vulkan = false
  if (platform === "win32") {
    notes.push("Vulkan：Windows 下不探测（缺可靠只读命令）——有显卡驱动时按厂商 ICD 自检，或直接用档位实测")
  } else {
    const vi = await probeCommand("vulkaninfo", ["--summary"], ctx)
    if (vi.code === 0 && /Vulkan|deviceName|apiVersion/i.test(vi.stdout)) {
      vulkan = true
      notes.push(`Vulkan：vulkaninfo --summary 可用（${firstLine(vi.stdout)}）`)
    } else if (vi.missing) {
      notes.push("vulkaninfo 不可用：not found")
    } else {
      notes.push(`vulkaninfo 退出码 ${vi.code}：${firstLine(vi.stderr) || "无输出"}`)
    }
    const renders = renderNodes(devDir)
    if (renders.length) {
      vulkan = true
      notes.push(`Vulkan：存在 DRM render 节点（${renders.join(", ")}）——必要条件，最终取决于 Vulkan ICD 是否就位`)
    } else {
      notes.push(`DRM render 节点：无（${posixJoin(devDir, "dri")} 不存在或无 renderD*）`)
    }
  }

  const metal = platform === "darwin"
  notes.push(metal ? "Metal：darwin 平台自带 Metal（macOS 引擎归档即 Metal 构建）" : `Metal：当前平台 ${platform} 不支持`)
  return { cpu_cores: cores, cuda, vulkan, metal, notes }
}

// ── 引擎视图与推荐 ────────────────────────────────────────────────────────

/** 设备优先级（挑「本机缺省引擎」时的降序；cpu 恒为兜底）。 */
const DEVICE_PRIORITY: EngineDevice[] = ["cuda", "metal", "vulkan", "rocm", "sycl", "openvino", "cpu"]

/**
 * 本机缺省引擎 id：在「同平台同架构」条目里按设备优先级取第一个可用者。
 * 不给 devices（不做实测）时只认**无需探测即可确定**的设备：cpu 与 darwin 的 metal——
 * 这样缺省推荐永远是保守可用的（Linux/Windows 无探测 → cpu 引擎；macOS → Metal 归档）。
 * 给了实测结果（devices）才可能选中 cuda/vulkan；rocm/sycl/openvino 不在探测范围内，只能显式点名。
 */
export function pickEngineForPlatform(
  matrix: EngineMatrix,
  platform: { platform: EnginePlatform; arch: "x64" | "arm64" },
  devices?: Pick<DeviceInfo, "cuda" | "vulkan" | "metal">,
): string | undefined {
  const usable = (dev: EngineDevice): boolean => {
    if (dev === "cpu") return true
    if (!devices) return dev === "metal" && platform.platform === "darwin"
    if (dev === "cuda") return devices.cuda
    if (dev === "vulkan") return devices.vulkan
    if (dev === "metal") return devices.metal
    return false
  }
  const candidates = matrix.engines.filter(
    (e) => e && e.platform === platform.platform && e.arch === platform.arch && Boolean(e.asset),
  )
  for (const dev of DEVICE_PRIORITY) {
    const hit = candidates.find((e) => e.device === dev && usable(dev))
    if (hit) return hit.id
  }
  return undefined
}

/** 列出引擎视图：矩阵 × 本机平台 × 已安装状态（工具输出与选择建议用）。 */
export function listEngineViews(
  home: string,
  matrix: EngineMatrix,
  platform: { platform: EnginePlatform; arch: "x64" | "arm64" },
): Array<Record<string, unknown>> {
  const views = matrix.engines.map((def) => {
    const inst = installedEngine(home, def.id)
    const view: Record<string, unknown> = {
      id: def.id,
      platform: def.platform,
      arch: def.arch,
      device: def.device,
      installed: Boolean(inst),
      downloadable: Boolean(def.asset),
      is_current_platform: def.platform === platform.platform && def.arch === platform.arch,
    }
    if (inst) {
      view.exe = inst.exe
      view.dir = inst.dir
      view.installed_at = inst.installed_at
      view.tag = inst.tag
    }
    if (def.notes) view.notes = def.notes
    if (def.requires) view.requires = def.requires
    if (def.cudart_asset) view.cudart_asset = def.cudart_asset
    return view
  })
  // 已安装优先 → 本机平台优先 → id 字典序（矩阵顺序只作稳定兜底）
  return views.sort((a, b) => {
    const ai = Number(a.installed)
    const bi = Number(b.installed)
    if (ai !== bi) return bi - ai
    const ac = Number(a.is_current_platform)
    const bc = Number(b.is_current_platform)
    if (ac !== bc) return bc - ac
    return String(a.id).localeCompare(String(b.id))
  })
}

/** 引擎与本机设备的匹配评定（人读一句话：能不能跑、缺什么）。 */
function engineAvailability(def: EngineDef, devices: DeviceInfo): { ok: boolean; why: string } {
  switch (def.device) {
    case "cpu":
      return { ok: true, why: `纯 CPU 后端，${devices.cpu_cores} 核可用` }
    case "cuda":
      return devices.cuda
        ? { ok: true, why: "已探测到 NVIDIA CUDA（nvidia-smi 或设备节点）" }
        : { ok: false, why: "需要 NVIDIA CUDA，但本机未探测到（nvidia-smi 不可用且无 /dev/nvidia0）" }
    case "vulkan":
      return devices.vulkan
        ? { ok: true, why: "已探测到 Vulkan（vulkaninfo 或 DRM render 节点）" }
        : { ok: false, why: "需要 Vulkan ICD，但本机未探测到" }
    case "metal":
      return devices.metal ? { ok: true, why: "darwin 平台自带 Metal" } : { ok: false, why: "需要 macOS（Metal）" }
    default:
      return { ok: false, why: `设备 ${def.device} 不在本层探测范围内——能否使用以上游运行时为准，请显式点名并看启动日志` }
  }
}

// ── 下载（断点续传 + 校验 + 原子落盘） ─────────────────────────────────────

/** 文件大小（不存在/不可读记 0）。 */
function fileSize(p: string): number {
  try {
    return existsSync(p) ? statSync(p).size : 0
  } catch {
    return 0
  }
}

/** 流式计算 sha256（大文件不整文件入内存）。 */
function hashFile(file: string): string | null {
  try {
    const h = createHash("sha256")
    const fd = openSync(file, "r")
    try {
      const buf = Buffer.allocUnsafe(1024 * 1024)
      for (;;) {
        const n = readSync(fd, buf, 0, buf.length, null)
        if (n <= 0) break
        h.update(buf.subarray(0, n))
      }
    } finally {
      closeSync(fd)
    }
    return h.digest("hex")
  } catch {
    return null
  }
}

/** 响应里的总字节数（206 看 Content-Range 的总量，200 看 Content-Length）。 */
function responseTotal(res: Response, startAt: number): number | undefined {
  const cr = res.headers.get("content-range")
  if (cr) {
    const m = cr.match(/\/(\d+)\s*$/)
    if (m) return Number(m[1])
  }
  const cl = res.headers.get("content-length")
  if (cl) {
    const n = Number(cl)
    if (Number.isFinite(n)) return startAt + n
  }
  return undefined
}

/**
 * 通用下载（断点续传 + 大小/sha256 校验 + 进度）。
 *
 * 原子性与续传的统一处理：
 *   * 写入一律落在 `<dest>.part`，校验通过才 rename 到 `dest`——**产物路径上不出现未完成内容**；
 *     若 `dest` 上残留半成品（上次中断遗留）则先并入 `.part` 作续传起点。
 *   * 已有字节 → 带 `Range: bytes=<n>-`：206 追加、200（服务端忽略区间）截断重写；416 区间越界按情况收尾或重下。
 *   * expect_bytes 达标即幂等跳过（模型/引擎资产动辄数百 MB~GB，重下代价高）。
 *   * 任何失败都清理 `.part` 并返回可读原因（不抛错）；只有进程被强杀才会留下 `.part`（下次自然续传）。
 *   * fetchImpl / signal 可注入（测试与取消）。
 */
export async function downloadFile(
  url: string,
  dest: string,
  opts: { expect_bytes?: number; sha256?: string; onProgress?: DownloadProgress; fetchImpl?: typeof fetch; signal?: AbortSignal } = {},
): Promise<{ ok: boolean; bytes: number; error?: string }> {
  const { expect_bytes, sha256, onProgress, fetchImpl = fetch, signal } = opts
  const part = `${dest}.part`
  const cleanup = (): void => {
    try {
      rmSync(part, { force: true })
    } catch {
      /* 清理失败不覆盖主错误 */
    }
  }
  try {
    mkdirSync(dirname(dest), { recursive: true })

    // 幂等：目标已存在且大小达标 → 不重复下载
    if (expect_bytes != null && fileSize(dest) === expect_bytes) {
      onProgress?.({ received: expect_bytes, total: expect_bytes, phase: "skip" })
      return { ok: true, bytes: expect_bytes }
    }

    // 半成品统一收敛到 .part（可能是上次中断留下的 dest 或 .part）
    if (existsSync(dest) && !existsSync(part)) renameSync(dest, part)
    let resumeFrom = fileSize(part)
    if (expect_bytes != null && resumeFrom > expect_bytes) {
      rmSync(part, { force: true }) // 超长半成品（换来源/损坏）→ 丢弃重下
      resumeFrom = 0
    }

    onProgress?.({ received: resumeFrom, total: expect_bytes, phase: "start" })
    let res = await fetchImpl(url, resumeFrom > 0 ? { headers: { Range: `bytes=${resumeFrom}-` }, signal } : { signal })

    if (res.status === 416 && resumeFrom > 0) {
      // 服务端拒绝续传：半成品若已达标即视为完成，否则清理后从头重下一次
      if (expect_bytes != null && fileSize(part) === expect_bytes) {
        renameSync(part, dest)
        onProgress?.({ received: expect_bytes, total: expect_bytes, phase: "done" })
        return { ok: true, bytes: expect_bytes }
      }
      rmSync(part, { force: true })
      resumeFrom = 0
      res = await fetchImpl(url, { signal })
    }

    if (!res.ok) throw new Error(`HTTP ${res.status}${res.statusText ? ` ${res.statusText}` : ""}`)
    if (!res.body) throw new Error("响应无正文（服务端未返回数据流）")

    // 206 视作续传追加；200 视作服务端忽略区间（截断重写）
    const append = res.status === 206 && resumeFrom > 0
    const startAt = append ? resumeFrom : 0
    const total = expect_bytes ?? responseTotal(res, startAt)
    const fd = openSync(part, append ? "a" : "w")
    let received = startAt
    try {
      for await (const chunk of res.body as unknown as AsyncIterable<Uint8Array>) {
        if (signal?.aborted) throw new Error("已取消")
        writeSync(fd, chunk)
        received += chunk.length
        onProgress?.({ received, total, phase: "download" })
      }
    } finally {
      closeSync(fd)
    }

    const bytes = fileSize(part)
    if (expect_bytes != null && bytes !== expect_bytes) {
      cleanup()
      return { ok: false, bytes, error: `大小校验失败：期望 ${expect_bytes} 字节，实际 ${bytes} 字节（已清理半成品，可重试续传）` }
    }
    if (sha256) {
      onProgress?.({ received: bytes, total, phase: "verify" })
      const got = hashFile(part)
      if (!got || got.toLowerCase() !== sha256.toLowerCase()) {
        cleanup()
        return { ok: false, bytes, error: `sha256 校验失败：期望 ${sha256}，实际 ${got ?? "无法计算"}（已清理半成品）` }
      }
    }
    renameSync(part, dest)
    onProgress?.({ received: bytes, total, phase: "done" })
    return { ok: true, bytes }
  } catch (e) {
    cleanup()
    const msg = e instanceof Error ? e.message : String(e)
    return { ok: false, bytes: fileSize(dest), error: `下载失败（${url}）：${msg}` }
  }
}

// ── 解压与 exe 定位 ──────────────────────────────────────────────────────

/** 归档格式（从文件名推断——cudart 资产在矩阵里只给名字）。 */
function archiveKind(name: string): "zip" | "tar.gz" {
  return /\.zip$/i.test(name) ? "zip" : "tar.gz"
}

/** 归档内的安全相对路径（防路径穿越：绝对路径/盘符/`..` 一律丢弃）。 */
function safeRel(name: string): string | null {
  const norm = String(name).replace(/\\/g, "/").replace(/^\.?\//, "")
  if (!norm || /^[a-zA-Z]:/.test(norm)) return null
  const parts = norm.split("/").filter((p) => p && p !== ".")
  if (!parts.length || parts.includes("..")) return null
  return parts.join("/")
}

/** 解压归档到目录（zip → fflate；tar.gz → 系统 tar，Windows 10+ 自带 bsdtar 同名可直接调）。 */
async function extractArchive(archive: string, kind: "zip" | "tar.gz", destDir: string): Promise<void> {
  if (kind === "zip") {
    const files = unzipSync(readFileSync(archive))
    for (const [name, data] of Object.entries(files)) {
      const rel = safeRel(name)
      if (!rel) continue
      const out = join(destDir, rel)
      if (name.endsWith("/")) {
        mkdirSync(out, { recursive: true })
        continue
      }
      mkdirSync(dirname(out), { recursive: true })
      writeFileSync(out, data)
    }
    return
  }
  await new Promise<void>((resolve, reject) => {
    const child = spawn("tar", ["-xzf", archive, "-C", destDir], { stdio: ["ignore", "pipe", "pipe"], windowsHide: true })
    let err = ""
    child.stderr?.on("data", (d: Buffer) => {
      err += String(d)
    })
    child.on("error", (e) => reject(new Error(`tar 不可用：${e.message}`)))
    child.on("close", (code) => {
      if (code === 0) resolve()
      else reject(new Error(`tar 退出码 ${code}：${firstLine(err) || "无输出"}`))
    })
  })
}

/** 递归查找 llama-server（归档内路径各异，如 build/bin/llama-server）。 */
export function findLlamaServer(root: string, platform: string = process.platform): string | null {
  const want = platform === "win32" ? "llama-server.exe" : "llama-server"
  const alt = platform === "win32" ? "llama-server" : "llama-server.exe"
  const hits: string[] = []
  walkFiles(root, hits, 0)
  return hits.find((p) => basename(p) === want) ?? hits.find((p) => basename(p) === alt) ?? null
}

/** 目录遍历（深度上限防符号链接环；不可读条目跳过）。 */
function walkFiles(dir: string, out: string[], depth: number): void {
  if (depth > 8) return
  let entries: string[]
  try {
    entries = readdirSync(dir)
  } catch {
    return
  }
  for (const name of entries) {
    const p = join(dir, name)
    try {
      const st = statSync(p)
      if (st.isDirectory()) walkFiles(p, out, depth + 1)
      else if (st.isFile() && /^llama-server(\.exe)?$/i.test(name)) out.push(p)
    } catch {
      /* 断链符号链接等：跳过 */
    }
  }
}

// ── 安装 ──────────────────────────────────────────────────────────────────

/**
 * 下载 + 校验 + 解压 + 定位 exe + 写标记（工具 engine_fetch 的核心）。
 * 步骤：① 已安装且未 force → 直接返回；② 下载主资产（+ cudart）到 vendor/.cache/；
 * ③ 解压到同级临时目录 → 递归定位 llama-server → chmod 0o755（POSIX）→ 写 .engine.json → 整体 rename 落位；
 * ④ 任何一步失败返回 { ok:false, error } 并清理临时目录（不留残目录）。
 */
export async function installEngine(
  home: string,
  def: EngineDef,
  matrix: EngineMatrix,
  opts: { onProgress?: DownloadProgress; force?: boolean; fetchImpl?: typeof fetch } = {},
): Promise<{ ok: boolean; engine?: InstalledEngine; error?: string }> {
  const { onProgress, force, fetchImpl } = opts
  if (!def?.asset) return { ok: false, error: `引擎 ${def?.id ?? "?"} 未配置发行版资产（asset 为空）——检查 config/engines.json` }
  if (!force) {
    const cur = installedEngine(home, def.id)
    if (cur) return { ok: true, engine: cur } // 幂等：已安装即返回（工具层补「已安装」提示）
  }

  const cache = downloadCacheDir(home)
  const dir = engineDir(home, def.id)
  const stage = join(vendorDir(home), `.stage-${def.id}-${Math.random().toString(36).slice(2, 8)}`)
  const cleanup = (): void => {
    try {
      rmSync(stage, { recursive: true, force: true })
    } catch {
      /* 清理失败不覆盖主错误 */
    }
  }

  try {
    mkdirSync(cache, { recursive: true })
    mkdirSync(vendorDir(home), { recursive: true })

    // ① 下载（资产带缓存：同 tag 的重复安装不重下）
    const archive = join(cache, def.asset)
    const dl = await downloadFile(engineAssetUrl(matrix, def), archive, { onProgress, fetchImpl })
    if (!dl.ok) return { ok: false, error: `下载引擎资产失败：${dl.error}` }

    let cudart = false
    const cudartUrl = cudartAssetUrl(matrix, def)
    if (def.cudart_asset && cudartUrl) {
      const cu = await downloadFile(cudartUrl, join(cache, def.cudart_asset), { onProgress, fetchImpl })
      if (!cu.ok) return { ok: false, error: `下载 CUDA 运行时（cudart）失败：${cu.error}` }
      cudart = true
    }

    // ② 解压到同级临时目录（同级才能整体 rename，避免半成品出现在 vendor/<id>）
    onProgress?.({ received: fileSize(archive), phase: "extract" })
    rmSync(stage, { recursive: true, force: true })
    mkdirSync(stage, { recursive: true })
    await extractArchive(archive, def.archive, stage)
    if (cudart && def.cudart_asset) await extractArchive(join(cache, def.cudart_asset), archiveKind(def.cudart_asset), stage)

    // ③ 递归定位 llama-server（归档内路径各异）
    const exe = findLlamaServer(stage)
    if (!exe) {
      cleanup()
      return {
        ok: false,
        error: `解压后未找到 llama-server（资产 ${def.asset}）——归档结构可能变化，请核对上游资产名；已清理临时目录 ${stage}`,
      }
    }
    if (process.platform !== "win32") {
      try {
        chmodSync(exe, 0o755) // 归档可能丢掉执行位（zip 无 mode）
      } catch {
        /* 某些文件系统不支持 chmod：不影响后续以显式路径启动 */
      }
    }

    // ④ 写标记（相对路径基于临时目录，落位后语义不变）→ 整体替换到 vendor/<id>
    const marker: InstalledMarker = {
      id: def.id,
      exe: join(dir, relative(stage, exe)),
      tag: matrix.release.llama_cpp.tag,
      asset: def.asset,
      installed_at: new Date().toISOString(),
      dir,
      ...(cudart ? { cudart: true } : {}),
    }
    writeFileSync(join(stage, ".engine.json"), `${JSON.stringify(marker, null, 2)}\n`, "utf-8")
    rmSync(dir, { recursive: true, force: true }) // 重装/换 tag：旧目录整体替换
    renameSync(stage, dir)
    onProgress?.({ received: dl.bytes, phase: "done" })
    return { ok: true, engine: marker }
  } catch (e) {
    cleanup()
    const msg = e instanceof Error ? e.message : String(e)
    return { ok: false, error: `安装引擎 ${def.id} 失败：${msg}（已清理临时目录；下载缓存保留在 ${cache}）` }
  }
}

// ── 模型获取（跨平台小模型；CPU 也能跑的验证链路） ────────────────────────

/** 预置模型（直链 + 字节数 + sha256，均已核对存在；字节/sha256 用于下载后校验完整性）。 */
export interface ModelPreset {
  name: string
  url: string
  bytes: number
  sha256?: string
  notes?: string
}

/** 预置小模型注册表：无 GPU 的跨平台验证与轻量任务（配套档位 cpu-small）。 */
export const MODEL_PRESETS: Record<string, ModelPreset> = {
  "qwen2.5-0.5b": {
    name: "Qwen2.5-0.5B-Instruct-Q4_K_M.gguf",
    url: "https://huggingface.co/bartowski/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf",
    bytes: 397808192,
    sha256: "6eb923e7d26e9cea28811e1a8e852009b21242fb157b26149d3b188f3a8c8653",
    notes: "0.5B Q4_K_M（约 380 MiB）：纯 CPU 秒级加载，跨平台验证与轻量任务（档位 cpu-small）",
  },
  "qwen2.5-1.5b": {
    name: "Qwen2.5-1.5B-Instruct-Q4_K_M.gguf",
    url: "https://huggingface.co/bartowski/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/Qwen2.5-1.5B-Instruct-Q4_K_M.gguf",
    bytes: 986048768,
    sha256: "1adf0b11065d8ad2e8123ea110d1ec956dab4ab038eab665614adba04b6c3370",
    notes: "1.5B Q4_K_M（约 940 MiB）：CPU 上仍可交互，带完整工具调用模板（--jinja）",
  },
}

// ── 工具 ──────────────────────────────────────────────────────────────────

function schema(properties: Record<string, unknown>, required: string[] = []): ToolSchema {
  return { type: "object", properties, required }
}

const pad = (s: string, n: number): string => (s.length >= n ? s : s + " ".repeat(n - s.length))

/** 引擎矩阵表格（列宽按 ASCII 计算，中文只在行尾，避免 CJK 宽度错位）。 */
function formatEngineTable(
  home: string,
  views: Array<Record<string, unknown>>,
  plat: { platform: EnginePlatform; arch: string },
  tag: string,
  recommended?: string,
): string {
  const installedCount = views.filter((v) => v.installed).length
  const lines: string[] = [
    `引擎矩阵：${views.length} 条（已安装 ${installedCount}）· llama.cpp ${tag} · 本机 ${plat.platform}/${plat.arch}${recommended ? ` · 缺省推荐 ${recommended}` : ""}`,
    "图例：● 已安装　↓ 未安装·可下载　× 矩阵无资产（非本机平台条目同样列出，便于交叉搬运/分发）",
    "",
    `  ${pad("id", 26)}${pad("platform/arch", 15)}${pad("device", 10)}状态`,
  ]
  for (const v of views) {
    const marker = v.installed ? "●" : v.downloadable ? "↓" : "×"
    const status = v.installed
      ? v.is_current_platform
        ? "已安装"
        : "已安装（非本机平台）"
      : v.is_current_platform
        ? v.downloadable
          ? "未安装（可下载）"
          : "未安装（无资产）"
        : "—（非本机平台）"
    lines.push(`  ${marker} ${pad(String(v.id), 24)}${pad(`${v.platform}/${v.arch}`, 15)}${pad(String(v.device), 10)}${status}`)
  }

  const cur = views.filter((v) => v.is_current_platform)
  lines.push("", `本机平台（${plat.platform}/${plat.arch}）条目：`)
  if (!cur.length) {
    lines.push("  （无——该平台/架构在矩阵里没有条目；可 id 交叉安装用于搬运，或补 config/engines.json）")
  }
  // 已安装但**不在矩阵里**的引擎（典型：源码就地编译产物，id 如 linux-cpu-src）——矩阵列表看不到它们，
  // 但它们是真实可用的引擎（start 能直接用），必须一并列出，否则“编译完了找不到”成为实际障碍。
  const extra = collectInstalledExtras(home, views)
  if (extra.length) {
    lines.push("", "已安装但不在矩阵中的引擎（源码编译产物等，可被 start 的 engine 参数直接引用）：")
    for (const e of extra) {
      lines.push(`  ${e.id}${e.device ? `（${e.device}）` : ""}${e.from_source ? " 源码编译" : ""}`)
      lines.push(`      exe: ${e.exe}`)
    }
    lines.push("  重新编译：local_infer_source_build(source=\"<候选 id>\", device=\"…\")")
  }
  lines.push("", '安装：local_infer_engine_fetch(id="…")；内网源码编译：local_infer_source_build；实测设备与推荐：local_infer_engines(action="detect")')
  return lines.join("\n")
}

/** 单个引擎详情（工具 list 带 id 时）。 */
function formatEngineDetail(
  def: EngineDef,
  view: Record<string, unknown> | undefined,
  matrix: EngineMatrix,
  plat: { platform: EnginePlatform; arch: string },
): string {
  const lines = [
    `引擎 ${def.id}`,
    `  平台/架构：${def.platform}/${def.arch}　设备：${def.device}`,
    `  资产：${def.asset}（${def.archive}）`,
    `  地址：${engineAssetUrl(matrix, def)}`,
    def.cudart_asset ? `  cudart：${def.cudart_asset} → ${cudartAssetUrl(matrix, def)}` : undefined,
    `  本机平台匹配：${view?.is_current_platform ? "是" : `否（本机 ${plat.platform}/${plat.arch}）`}`,
    `  安装状态：${view?.installed ? `已安装（exe: ${view.exe}）` : "未安装"}`,
    view?.notes ? `  说明：${view.notes}` : undefined,
    view?.requires ? `  依赖：${view.requires}` : undefined,
    "",
    `安装：local_infer_engine_fetch(id="${def.id}")`,
  ].filter((l): l is string => l !== undefined)
  return lines.join("\n")
}

const enginesTool: Tool = {
  name: "engines",
  safeMode: true, // 只读：读矩阵/安装标记 + 只读设备探测命令，不下载不落盘，安全模式可用
  description:
    "查看**跨平台**推理引擎矩阵与安装状态（llama.cpp 各平台发行版：linux/win32/darwin × cpu/cuda/vulkan/metal/rocm/sycl/openvino）。action=list 列出引擎清单（平台/架构/设备/是否已安装/可否下载/是否本机平台 + 本机平台条目详情）；action=detect 输出本机设备探测（CPU 核数、CUDA/Vulkan/Metal 与每条结论的探测依据）并给出按实测结果推荐的引擎 id。只读、免审批；要安装引擎用 local_infer_engine_fetch。",
  parameters: schema({
    action: { type: "string", enum: ["list", "detect"], description: "list=引擎矩阵清单（缺省）；detect=本机设备探测 + 推荐引擎" },
    id: { type: "string", description: "list 时只看该引擎（详情）；detect 时评定该引擎在本机的可用性" },
  }),
  outputSchema: schema({
    action: { type: "string", description: "实际执行的动作：list | detect" },
    platform: { type: "string", description: "本机平台（linux/win32/darwin）" },
    arch: { type: "string", description: "本机架构（x64/arm64）" },
    tag: { type: "string", description: "llama.cpp 发行版 tag（矩阵 release.llama_cpp.tag）" },
    count: { type: "number", description: "矩阵引擎条目数" },
    installed: { type: "number", description: "已安装引擎数" },
    recommended: { type: "string", description: "按平台（有设备探测结果则按实测结果）推荐的引擎 id" },
    engines: {
      type: "array",
      items: { type: "object" },
      description: "引擎视图：id/platform/arch/device/installed/exe/downloadable/is_current_platform/notes/requires",
    },
    devices: { type: "object", description: "detect 动作的设备探测结果：cpu_cores/cuda/vulkan/metal/notes" },
    available: { type: "object", description: "detect 且带 id 时该引擎的可用性评定：{ ok, why }" },
  }),
  async execute(args, ctx) {
    const home = inferHome(ctx.env)
    const plat = detectPlatform()
    const action = args.action === "detect" ? "detect" : "list"

    if (action === "detect") {
      const devices = await detectDevices(ctx)
      const matrix = loadEngineMatrix(home)
      const recommended = matrix ? pickEngineForPlatform(matrix, plat, devices) : undefined
      const lines: string[] = [
        `本机设备探测（${plat.platform}/${plat.arch}）`,
        `  CPU 核数：${devices.cpu_cores}`,
        `  CUDA   ：${devices.cuda ? "可用" : "不可用"}`,
        `  Vulkan ：${devices.vulkan ? "可用" : "不可用"}`,
        `  Metal  ：${devices.metal ? "可用" : `不支持（非 darwin）`}`,
        recommended ? `  推荐引擎：${recommended}（按实测设备按下述优先级选出）` : "  推荐引擎：无（引擎矩阵缺失，或本机平台/架构无条目）",
        "",
        "探测依据：",
        ...devices.notes.map((n) => `  · ${n}`),
      ]
      let available: { ok: boolean; why: string } | undefined
      if (args.id != null && String(args.id)) {
        const id = String(args.id)
        const def = matrix ? findEngine(matrix, id) : undefined
        if (!def) {
          lines.push("", `引擎 ${id}：${matrix ? `矩阵里没有该 id（可用：${matrix.engines.map((e) => e.id).join(", ")}）` : "引擎矩阵缺失或损坏"}`)
        } else {
          available = engineAvailability(def, devices)
          lines.push("", `引擎 ${id} 在本机：${available.ok ? "可用" : "不可用"}——${available.why}`)
        }
      }
      const data: Record<string, unknown> = {
        action,
        platform: plat.platform,
        arch: plat.arch,
        ...(matrix ? { tag: matrix.release.llama_cpp.tag, count: matrix.engines.length } : {}),
        recommended,
        devices,
        ...(available ? { available } : {}),
      }
      return { output: lines.join("\n"), data }
    }

    const matrix = loadEngineMatrix(home)
    if (!matrix) {
      return {
        output:
          `引擎矩阵缺失或损坏：${enginesConfigPath(home)}\n` +
          "应含 release.llama_cpp.{tag,base} 与 engines[]（见 infer/config/engines.json）；" +
          "确认 LOCAL_INFER_HOME 指向子项目 infer/ 后重试。",
        data: { action, platform: plat.platform, arch: plat.arch },
      }
    }
    const views = listEngineViews(home, matrix, plat)
    const recommended = pickEngineForPlatform(matrix, plat)

    if (args.id != null && String(args.id)) {
      const id = String(args.id)
      const def = findEngine(matrix, id)
      if (!def) {
        return {
          output: `未知引擎 id：${id}（可用：${matrix.engines.map((e) => e.id).join(", ")}）`,
          data: { action, platform: plat.platform, arch: plat.arch, count: matrix.engines.length, engines: views },
        }
      }
      return {
        output: formatEngineDetail(def, views.find((v) => v.id === id), matrix, plat),
        data: { action, platform: plat.platform, arch: plat.arch, tag: matrix.release.llama_cpp.tag, count: matrix.engines.length, engines: [views.find((v) => v.id === id)] },
      }
    }

    const installedExtra = collectInstalledExtras(home, views)
    return {
      output: formatEngineTable(home, views, plat, matrix.release.llama_cpp.tag, recommended),
      data: {
        action,
        platform: plat.platform,
        arch: plat.arch,
        tag: matrix.release.llama_cpp.tag,
        count: matrix.engines.length,
        installed: views.filter((v) => v.installed).length,
        // 编排侧（js 脚本/冒烟脚本）需要「本机现成可用的引擎 id」：矩阵内已安装 + 源码编译等矩阵外产物
        installed_ids: [...views.filter((v) => v.installed).map((v) => String(v.id)), ...installedExtra.map((e) => e.id)],
        installed_extra: installedExtra,
        recommended,
        engines: views,
      },
    }
  },
}

/** 已安装但**不在矩阵里**的引擎（典型：源码就地编译产物，id 如 smoke-linux-cpu）——
 *  矩阵视图看不到它们，但它们是真实可用引擎，编排侧（如冒烟脚本选引擎）需要其 id。 */
function collectInstalledExtras(home: string, views: Array<Record<string, unknown>>): Array<{ id: string; exe: string; from_source: boolean; device?: string }> {
  const matrixIds = new Set(views.map((v) => String(v.id)))
  const out: Array<{ id: string; exe: string; from_source: boolean; device?: string }> = []
  const vdir = vendorDir(home)
  if (!existsSync(vdir)) return out
  for (const name of readdirSync(vdir)) {
    if (matrixIds.has(name) || name.startsWith(".")) continue
    const inst = installedEngine(home, name)
    if (!inst) continue
    const build = (inst as { build?: { from_source?: boolean; device?: string } }).build
    out.push({ id: name, exe: inst.exe, from_source: build?.from_source === true, device: build?.device })
  }
  return out
}

const engineFetch: Tool = {
  name: "engine_fetch",
  safeMode: false, // 安全模式下不提供：会下载资产并写入 vendor/
  requiresApproval: true,
  description:
    "按 id 下载并安装**跨平台**推理引擎（llama.cpp 发行版）：下载资产（断点续传 + 大小校验；CUDA 档一并装 cudart）→ 解压 → 递归定位 llama-server（chmod 可执行）→ 写入安装标记 .engine.json，落 vendor/<id>/。已安装则直接返回（force=true 强制重装）。跨平台：linux/win32/darwin × cpu/cuda/vulkan/metal/rocm/sycl/openvino 均由 config/engines.json 声明，先 local_infer_engines 看清单。需审批（下载与落盘）。",
  parameters: schema(
    {
      id: { type: "string", description: "引擎 id（见 local_infer_engines 的清单，如 linux-cpu-x64 / win-cuda-12.4-x64）" },
      force: { type: "boolean", description: "已安装时强制重装（缺省 false：已安装即直接返回）" },
    },
    ["id"],
  ),
  outputSchema: schema({
    ok: { type: "boolean", description: "是否安装成功" },
    id: { type: "string", description: "引擎 id" },
    cached: { type: "boolean", description: "true = 已安装过，本次未下载" },
    bytes: { type: "number", description: "资产字节数（下载完成后的归档大小）" },
    dir: { type: "string", description: "安装目录（vendor/<id>）" },
    exe: { type: "string", description: "llama-server 可执行文件绝对路径" },
    cudart: { type: "boolean", description: "是否同时安装了 CUDA 运行时（cudart 资产）" },
    available: { type: "object", description: "该引擎与本机设备的匹配评定：{ ok, why }" },
    error: { type: "string", description: "失败原因（可操作说明）" },
  }),
  async execute(args, ctx) {
    const home = inferHome(ctx.env)
    const id = args.id != null ? String(args.id).trim() : ""
    if (!id) {
      return {
        output: '缺少参数 id——先 local_infer_engines(action="list") 看可用引擎清单（如 linux-cpu-x64）。',
        data: { ok: false, error: "缺少 id" },
      }
    }
    const matrix = loadEngineMatrix(home)
    if (!matrix) {
      return {
        output: `引擎矩阵缺失或损坏：${enginesConfigPath(home)}（应含 release.llama_cpp.{tag,base} 与 engines[]）`,
        data: { ok: false, id, error: "引擎矩阵缺失或损坏" },
      }
    }
    const def = findEngine(matrix, id)
    if (!def) {
      return {
        output: `未知引擎 id：${id}\n可用：${matrix.engines.map((e) => e.id).join(", ")}`,
        data: { ok: false, id, error: "未知引擎 id" },
      }
    }
    const plat = detectPlatform()
    const notes: string[] = []
    const samePlatform = def.platform === plat.platform && def.arch === plat.arch
    if (!samePlatform) {
      notes.push(
        `⚠ 该引擎面向 ${def.platform}/${def.arch}，本机是 ${plat.platform}/${plat.arch}——交叉安装只用于搬运/分发，本机启动需匹配平台的引擎。`,
      )
    }

    const existing = installedEngine(home, id)
    if (existing && !args.force) {
      const avail = engineAvailability(def, await detectDevices(ctx, plat.platform))
      return {
        output: [
          `引擎 ${id} 已安装（本次未下载）：`,
          `  exe: ${existing.exe}`,
          `  目录: ${existing.dir}`,
          `  资产: ${existing.asset} @ ${existing.tag}`,
          `  设备匹配：${avail.ok ? "可用" : "不可用"}——${avail.why}`,
          "",
          `下一步：local_infer_start 的 engine 参数（或档位 engine 字段 = "${id}"）启用；确需重装加 force=true。`,
        ].join("\n"),
        data: { ok: true, id, cached: true, dir: existing.dir, exe: existing.exe, available: avail },
      }
    }

    let lastPhase = "download"
    let received = 0
    let total: number | undefined
    const out = await installEngine(home, def, matrix, {
      force: Boolean(args.force),
      onProgress: (p) => {
        lastPhase = p.phase
        received = p.received
        if (p.total) total = p.total
      },
    })
    if (!out.ok || !out.engine) {
      return {
        output: [`引擎 ${id} 安装失败：`, `  ${out.error ?? "未知错误"}`, "", "排查：local_infer_engines(action=\"list\") 核对 id/平台；网络受限时先手工放好资产再重试。"].join("\n"),
        data: { ok: false, id, error: out.error },
      }
    }

    const engine = out.engine
    const archiveBytes = fileSize(join(downloadCacheDir(home), def.asset))
    const cudart = Boolean((engine as InstalledMarker).cudart)
    const devices = await detectDevices(ctx, plat.platform)
    const avail = engineAvailability(def, devices)
    notes.push(`设备匹配：${avail.ok ? "可用" : "提示"}——${avail.why}`)
    return {
      output: [
        `引擎 ${id} 安装完成`,
        `  下载：${archiveBytes} 字节${total ? `（总计 ${total} 字节，末阶段 ${lastPhase}，最后进度 ${received} 字节）` : ""} → 缓存 ${join(downloadCacheDir(home), def.asset)}`,
        `  解压目录：${engine.dir}`,
        `  可执行文件：${engine.exe}`,
        `  来源：llama.cpp ${engine.tag} · ${engine.asset}${cudart ? " · cudart 已一并安装" : ""}`,
        "",
        ...notes,
        "下一步：",
        `  · 启动服务：local_infer_start（engine="${id}"；配合档位/模型，如 CPU 用 profile="cpu-small"）`,
        `  · 看状态与矩阵：local_infer_status / local_infer_engines(action="list")`,
        `  · 缺模型：local_infer_model_fetch(preset="qwen2.5-0.5b")（CPU 小模型档位）`,
      ].join("\n"),
      data: {
        ok: true,
        id,
        cached: false,
        bytes: archiveBytes,
        dir: engine.dir,
        exe: engine.exe,
        cudart,
        available: avail,
      },
    }
  },
}

const modelFetch: Tool = {
  name: "model_fetch",
  safeMode: false, // 安全模式下不提供：会下载大文件并写入模型目录
  requiresApproval: true,
  description:
    '下载 GGUF 模型到本地模型目录（**跨平台**，纯 TS 下载：断点续传 + 大小/sha256 校验 + 原子落盘）。preset 取预置小模型（qwen2.5-0.5b / qwen2.5-1.5b——无 GPU 也能跑的跨平台验证档 cpu-small 配套）；或用 url + name 直连任意 GGUF 直链（可选 expect_bytes/sha256 校验）。已存在且大小达标则跳过（幂等）。需审批（下载与落盘）。',
  parameters: schema({
    preset: { type: "string", description: "预置模型名：qwen2.5-0.5b | qwen2.5-1.5b（含直链/字节数/sha256）" },
    url: { type: "string", description: "GGUF 直链（与 preset 二选一；preset 给出时本字段可覆盖其 url）" },
    name: { type: "string", description: "落盘文件名（缺省按 url 末段推断；须以 .gguf 结尾）" },
    expect_bytes: { type: "number", description: "期望字节数（缺省取预置值；给则可校验下载完整性且支持幂等跳过）" },
    sha256: { type: "string", description: "期望 sha256（缺省取预置值；给则流式校验）" },
  }),
  outputSchema: schema({
    ok: { type: "boolean", description: "是否就绪（下载成功或已存在且大小达标）" },
    name: { type: "string", description: "落盘文件名" },
    path: { type: "string", description: "模型文件绝对路径" },
    bytes: { type: "number", description: "文件字节数" },
    skipped: { type: "boolean", description: "true = 已存在且大小达标，本次未下载" },
    dir: { type: "string", description: "模型目录（LOCAL_INFER_MODELS_DIR 或 {GEBAI_HOME}/resources/models/infer）" },
    error: { type: "string", description: "失败原因（可操作说明）" },
  }),
  async execute(args, ctx) {
    const home = inferHome(ctx.env)
    const presetName = args.preset != null ? String(args.preset) : undefined
    let url = args.url != null ? String(args.url) : undefined
    let name = args.name != null ? String(args.name) : undefined
    let expect = typeof args.expect_bytes === "number" && Number.isFinite(args.expect_bytes) ? args.expect_bytes : undefined
    let sha = args.sha256 != null ? String(args.sha256) : undefined
    let fromPreset: ModelPreset | undefined

    if (presetName) {
      fromPreset = MODEL_PRESETS[presetName]
      if (!fromPreset) {
        const list = Object.entries(MODEL_PRESETS)
          .map(([k, v]) => `${k} → ${v.name}（${v.bytes} 字节）`)
          .join("；")
        return {
          output: `未知预置：${presetName}\n可用预置：${list}\n也可用 url + name 直连任意 GGUF 直链。`,
          data: { ok: false, error: "未知预置" },
        }
      }
      url = url ?? fromPreset.url
      name = name ?? fromPreset.name
      expect = expect ?? fromPreset.bytes
      sha = sha ?? fromPreset.sha256
    }
    if (!url) {
      const list = Object.entries(MODEL_PRESETS)
        .map(([k, v]) => `${k} → ${v.name}`)
        .join("；")
      return { output: `需要 preset 或 url 之一。预置：${list}`, data: { ok: false, error: "缺少 preset/url" } }
    }
    name = name ?? basename(new URL(url).pathname)
    if (!/^[^\\/]+\.gguf$/i.test(name)) {
      return {
        output: `模型文件名不合法：${name}——须是以 .gguf 结尾的纯文件名（不含路径分隔符），可用 name 参数显式指定。`,
        data: { ok: false, error: "模型文件名不合法" },
      }
    }

    const dir = modelsDir(home, ctx.env)
    const dest = join(dir, name)
    if (existsSync(dest) && expect != null && fileSize(dest) === expect) {
      return {
        output: `模型已就绪（本次未下载）：${dest}（${expect} 字节）`,
        data: { ok: true, name, path: dest, bytes: expect, skipped: true, dir },
      }
    }

    let lastPhase = "download"
    let received = 0
    const r = await downloadFile(url, dest, {
      expect_bytes: expect,
      sha256: sha,
      onProgress: (p) => {
        lastPhase = p.phase
        received = p.received
      },
    })
    if (!r.ok) {
      return {
        output: [
          `模型下载失败：${name}`,
          `  ${r.error ?? "未知错误"}`,
          `  来源：${url}`,
          `  目标：${dest}`,
          "",
          "提示：断点续传可重试同一命令（已下载部分会续传）；网络受限时也可手工放置 .gguf 后重试。",
        ].join("\n"),
        data: { ok: false, name, path: dest, bytes: r.bytes, error: r.error },
      }
    }
    return {
      output: [
        `模型就绪：${dest}`,
        `  ${r.bytes} 字节${sha ? "（sha256 校验通过）" : ""}${expect ? `，与期望 ${expect} 字节一致` : ""}（末阶段 ${lastPhase}，最后进度 ${received} 字节）`,
        fromPreset?.notes ? `  ${fromPreset.notes}` : "",
        "",
        `下一步：local_infer_start（profile="cpu-small"，model="${name}"）——纯 CPU 可跑；GPU 机器可换 35B 档位。`,
      ]
        .filter(Boolean)
        .join("\n"),
      data: { ok: true, name, path: dest, bytes: r.bytes, skipped: false, dir },
    }
  },
}

/** 工具集：engines（只读）/ engine_fetch（下载安装引擎）/ model_fetch（下载模型）。 */
export const tools: Record<string, Tool> = {
  engines: enginesTool,
  engine_fetch: engineFetch,
  model_fetch: modelFetch,
}

/** 需审批的工具（下载安装 + 落盘写文件）。 */
export const requiresApproval: Record<string, boolean> = { engine_fetch: true, model_fetch: true }
