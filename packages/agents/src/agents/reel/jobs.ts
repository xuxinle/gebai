/**
 * 渲染作业：长时渲染（成片可达数分钟）一律后台执行——工具调用立即返回作业 ID，进度经原生库 onProgress
 * 回调汇入（帧数 / 百分比 / fps / 预计剩余），取消走 cancelSignal，日志按行落盘，索引按 JSONL 追加。
 * 队列分档：静帧/预览并行 2（逐镜头 QA 要能连着发），成片/实测调优独占 1（抢 CPU/GPU 反而更慢）。
 * 附带实测调优（bench）：硬件编码强制探针（hardwareAcceleration=required）+ 并发候选实测，结论写入调优缓存。
 *
 * 坑（真机经验，必须照做）：
 * - `binariesDirectory` 默认不传：Remotion 用项目内 @remotion/compositor-* 的 compositor 与 ffmpeg，
 *   只给一个 ffmpeg 的目录会让 compositor 查找失败（调用方配置的目录须含三件套）。
 * - 并发与 Remotion 同规则（见 detect.effectiveCpuCount）；仍被拒时按报错里的上限自愈重试一次。
 */
import { appendFileSync, copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { effectiveCpuCount, type ProbeInput } from "./detect"
import { jobIndexPath, jobLogPath, jobsDir, stateDir, tuningPath } from "./paths"
import { chromiumOf, profileKey, type HardwareAcceleration, type RenderProfile, type ThroughputEntry, type TunedEntry } from "./profile"
import { diagnoseEncoderProbe, type EncoderDiagnosis } from "./encoder-diagnosis"
import { concatVideoSegments, muxAudioVideo, splitFrameRange } from "./shards"
import type { NativeBrowser, NativeLibs, VideoConfig } from "./runtime"

export type JobKind = "still" | "preview" | "video" | "bench"
export type JobStatus = "queued" | "running" | "done" | "failed" | "cancelled"

export interface JobProgress {
  stage: string
  percent?: number
  renderedFrames?: number
  totalFrames?: number
  fps?: number
  etaSec?: number
}

export interface Job {
  id: string
  kind: JobKind
  project: string
  composition: string
  status: JobStatus
  output?: string
  /** 登记时刻（排队开始）；实际开跑时刷新为运行时刻。 */
  startedAt: string
  endedAt?: string
  error?: string
  summary?: string
  progress?: JobProgress
}

/** 机器级硬件编码探针结论（bench 以 hardwareAcceleration=required 实测所得）。 */
interface EncoderProbe {
  hardware: boolean
  checkedAt: string
  error?: string
  /** 失败时的**可操作诊断**（结论 + 建议）：只存原生库报错等于把「为什么」交给用户去猜。 */
  diagnosis?: EncoderDiagnosis
}

/** 本机调优缓存：机器级硬件编码探针结论 + 按 profileKey 记账的实测档位与实测吞吐。 */
export interface Tuning {
  encoderProbe?: EncoderProbe
  entries: Record<string, TunedEntry>
  /** 整片渲染的实测吞吐（按键 = profileKey）：分片该不该切看它，不看核数。 */
  throughput?: Record<string, ThroughputEntry>
}

// —— 作业注册表与队列 ——

const jobs = new Map<string, Job>()
const logs = new Map<string, string[]>()
interface CancelSlot {
  cancel: (() => void) | null
  cancelled: boolean
}
const cancelSlots = new Map<string, CancelSlot>()

type Lane = "light" | "heavy"
const LANE_OF: Record<JobKind, Lane> = { still: "light", preview: "light", video: "heavy", bench: "heavy" }
const LANE_LIMIT: Record<Lane, number> = { light: 2, heavy: 1 }
const running: Record<Lane, number> = { light: 0, heavy: 0 }
const waiters: Array<{ lane: Lane; resolve: () => void }> = []

/** 释放名额后按先进先出把名额发下去（同档保序，不同档互不阻塞）。 */
function pump(): void {
  let i = 0
  while (i < waiters.length) {
    const waiter = waiters[i]
    if (running[waiter.lane] < LANE_LIMIT[waiter.lane]) {
      running[waiter.lane]++
      waiters.splice(i, 1)
      waiter.resolve()
    } else {
      i++
    }
  }
}

async function acquireSlot(kind: JobKind): Promise<() => void> {
  const lane = LANE_OF[kind]
  if (running[lane] < LANE_LIMIT[lane]) {
    running[lane]++
  } else {
    await new Promise<void>((resolve) => waiters.push({ lane, resolve }))
  }
  return () => {
    running[lane]--
    pump()
  }
}

let seq = 0
function nextJobId(kind: JobKind): string {
  seq++
  return `${kind}-${Date.now().toString(36)}-${seq}`
}

function appendIndex(ctx: ToolContext, entry: Record<string, unknown>): void {
  try {
    mkdirSync(jobsDir(ctx), { recursive: true })
    appendFileSync(jobIndexPath(ctx), `${JSON.stringify(entry)}\n`)
  } catch {
    /* 索引落盘失败不影响渲染 */
  }
}

export function createJob(opts: {
  ctx: ToolContext
  kind: JobKind
  project: string
  composition: string
  output?: string
}): Job {
  const job: Job = {
    id: nextJobId(opts.kind),
    kind: opts.kind,
    project: opts.project,
    composition: opts.composition,
    output: opts.output,
    status: "queued",
    startedAt: new Date().toISOString(),
    progress: { stage: "排队中" },
  }
  jobs.set(job.id, job)
  logs.set(job.id, [])
  appendIndex(opts.ctx, {
    id: job.id,
    kind: job.kind,
    project: job.project,
    composition: job.composition,
    output: job.output,
    status: job.status,
    startedAt: job.startedAt,
  })
  return job
}

export function getJob(id: string): Job | null {
  return jobs.get(id) ?? null
}

/** 作业已到终态（不再变化）。 */
function isSettled(job: Job): boolean {
  return job.status === "done" || job.status === "failed" || job.status === "cancelled"
}

/**
 * 同步等待作业到终态（供 still 的 `wait=true` 送审路径用：静帧秒级，等它比再轮询一次省一个往返）。
 * 超时返回当前作业——调用方按 `status` 判定，仍未终态就提示用 action=status 继续查。
 */
export async function waitJob(id: string, timeoutMs = 180_000, intervalMs = 150): Promise<Job | null> {
  const deadline = Date.now() + timeoutMs
  for (;;) {
    const job = getJob(id)
    if (!job) return null
    if (isSettled(job)) return job
    if (Date.now() >= deadline) return job
    await new Promise((resolve) => setTimeout(resolve, intervalMs))
  }
}

/** 作业列表：索引 JSONL 回放（跨进程可见的历史）叠加内存中的最新状态，按开始时间倒序。 */
export function listJobs(ctx: ToolContext, limit = 20): Job[] {
  const byId = new Map<string, Job>()
  let text = ""
  try {
    text = readFileSync(jobIndexPath(ctx), "utf8")
  } catch {
    text = ""
  }
  for (const line of text.split("\n")) {
    if (!line.trim()) continue
    try {
      const rec = JSON.parse(line) as Partial<Job>
      if (!rec.id || !rec.kind) continue
      const prev = byId.get(rec.id)
      byId.set(rec.id, {
        id: rec.id,
        kind: rec.kind,
        project: rec.project ?? prev?.project ?? "",
        composition: rec.composition ?? prev?.composition ?? "",
        status: rec.status ?? prev?.status ?? "queued",
        output: rec.output ?? prev?.output,
        startedAt: rec.startedAt ?? prev?.startedAt ?? "",
        endedAt: rec.endedAt ?? prev?.endedAt,
        error: rec.error ?? prev?.error,
        summary: rec.summary ?? prev?.summary,
        progress: prev?.progress,
      })
    } catch {
      /* 跳过损坏行 */
    }
  }
  for (const [id, job] of jobs) byId.set(id, { ...byId.get(id), ...job })
  return [...byId.values()]
    .sort((a, b) => (a.startedAt < b.startedAt ? 1 : -1))
    .slice(0, Math.max(1, limit))
}

/** 作业状态人读描述（进度条 + 帧数 + 速率 + 预计剩余）。 */
export function describeJob(job: Job): string {
  const parts: string[] = [`${job.id} · ${job.kind} · ${job.status}`]
  const progress = job.progress
  if (progress) {
    const total = progress.totalFrames
    const done = progress.renderedFrames
    if (total && done !== undefined) {
      const percent = progress.percent ?? Math.round((done / total) * 100)
      const filled = Math.round((percent / 100) * 20)
      parts.push(`[${"#".repeat(filled)}${"-".repeat(Math.max(0, 20 - filled))}] ${percent}% · 帧 ${done}/${total}`)
      if (progress.fps && progress.fps > 0) {
        const etaSec = progress.etaSec ?? Math.max(0, total - done) / progress.fps
        parts.push(`${progress.fps.toFixed(1)} fps · 预计剩余 ${etaSec < 60 ? `${etaSec.toFixed(0)}s` : `${(etaSec / 60).toFixed(1)}min`}`)
      }
    } else if (progress.percent !== undefined) {
      parts.push(`${progress.stage} ${progress.percent}%`)
    } else {
      parts.push(progress.stage)
    }
  }
  if (job.output) parts.push(`产物 ${job.output}`)
  if (job.summary) parts.push(job.summary.split("\n")[0]!)
  if (job.error) parts.push(`错误 ${job.error}`)
  return parts.join(" · ")
}

/** 追加一行作业日志：内存保留最近 200 行（进程内查询），同时落盘（跨进程/事后排查）。 */
export function jobLog(job: Job, ctx: ToolContext, line: string): void {
  const stamped = `[${new Date().toISOString().slice(11, 19)}] ${line}`
  const buf = logs.get(job.id) ?? []
  buf.push(stamped)
  if (buf.length > 200) buf.splice(0, buf.length - 200)
  logs.set(job.id, buf)
  try {
    appendFileSync(jobLogPath(ctx, job.id), `${stamped}\n`)
  } catch {
    /* 日志落盘失败不影响渲染 */
  }
}

/** 读取作业日志尾部若干行（落盘优先，落盘不可读时回落到内存缓冲）。 */
export function readJobLog(ctx: ToolContext, id: string, tailLines = 50): string {
  const mem = logs.get(id) ?? []
  try {
    const lines = readFileSync(jobLogPath(ctx, id), "utf8").split("\n").filter(Boolean)
    const source = lines.length ? lines : mem
    return source.slice(-Math.max(1, tailLines)).join("\n")
  } catch {
    return mem.slice(-Math.max(1, tailLines)).join("\n")
  }
}

/** 运行期把原生库的取消触发器登记到作业上（startJob 已建槽位；未登记的运行期作业取消返回 false）。 */
function bindCancel(jobId: string, cancel: () => void): void {
  const slot = cancelSlots.get(jobId) ?? { cancel: null, cancelled: false }
  slot.cancel = cancel
  cancelSlots.set(jobId, slot)
}

export function cancelJob(id: string): boolean {
  const slot = cancelSlots.get(id)
  const job = jobs.get(id)
  if (job && job.status === "queued") {
    job.status = "cancelled"
    job.endedAt = new Date().toISOString()
    job.progress = { stage: "已取消（尚未开始）" }
    return true
  }
  if (!slot || !slot.cancel) return false
  slot.cancelled = true
  try {
    slot.cancel()
  } catch {
    /* 原生库取消失败不阻断状态更新 */
  }
  return true
}

/**
 * 后台执行作业：排队 → 运行 → 收尾（状态/摘要/错误/索引）。异常一律写进 job 不外抛——
 * 调用方（工具）拿到的是作业 ID，失败只能从状态与日志里读。
 */
export function startJob(job: Job, ctx: ToolContext, run: (log: (line: string) => void) => Promise<string>): void {
  const log = (line: string) => jobLog(job, ctx, line)
  const slot: CancelSlot = { cancel: null, cancelled: false }
  cancelSlots.set(job.id, slot)
  void (async () => {
    const release = await acquireSlot(job.kind)
    try {
      if (job.status === "cancelled") {
        log("作业在开始前被取消")
        return
      }
      job.status = "running"
      job.startedAt = new Date().toISOString()
      job.progress = { stage: "启动中" }
      appendIndex(ctx, { id: job.id, kind: job.kind, project: job.project, composition: job.composition, output: job.output, status: "running", startedAt: job.startedAt })
      try {
        const summary = await run(log)
        job.status = "done"
        job.summary = summary
      } catch (err) {
        const message = (err as Error)?.message ?? String(err)
        if (slot.cancelled || /cancell?ed|aborted/i.test(message)) {
          job.status = "cancelled"
          job.error = message
          log(`作业已取消：${message}`)
        } else {
          job.status = "failed"
          job.error = message
          log(`作业失败：${message}`)
        }
      }
    } finally {
      job.endedAt = new Date().toISOString()
      const finalStage =
        job.status === "done" ? "完成" : job.status === "cancelled" ? "已取消" : job.status === "failed" ? "失败" : "结束"
      job.progress = { ...(job.progress ?? {}), stage: finalStage }
      cancelSlots.delete(job.id)
      appendIndex(ctx, {
        id: job.id,
        kind: job.kind,
        project: job.project,
        composition: job.composition,
        output: job.output,
        status: job.status,
        startedAt: job.startedAt,
        endedAt: job.endedAt,
        error: job.error,
        summary: job.summary,
      })
      release()
    }
  })()
}

// —— 调优缓存 ——

export function readTuning(ctx: ToolContext): Tuning {
  try {
    const parsed = JSON.parse(readFileSync(tuningPath(ctx), "utf8")) as Partial<Tuning>
    return { encoderProbe: parsed.encoderProbe, entries: parsed.entries ?? {}, throughput: parsed.throughput ?? {} }
  } catch {
    return { entries: {}, throughput: {} }
  }
}

export function writeTuning(ctx: ToolContext, tuning: Tuning): void {
  mkdirSync(stateDir(ctx), { recursive: true })
  writeFileSync(
    tuningPath(ctx),
    JSON.stringify({ encoderProbe: tuning.encoderProbe, entries: tuning.entries, throughput: tuning.throughput ?? {} }, null, 2),
  )
}

/** 记一次整片实测吞吐（读改写，保留其他键）。 */
export function writeThroughput(ctx: ToolContext, key: string, entry: ThroughputEntry): void {
  const tuning = readTuning(ctx)
  writeTuning(ctx, { ...tuning, throughput: { ...(tuning.throughput ?? {}), [key]: entry } })
}

/** 取某 `项目|合成` 的上一次实测吞吐（无记录则 null）。 */
export function pickThroughput(tuning: Tuning, key: string): ThroughputEntry | null {
  return tuning.throughput?.[key] ?? null
}

export function pickTuned(tuning: Tuning, key: string): TunedEntry | null {
  return tuning.entries[key] ?? null
}

// —— 帧段与并发上限解析 ——

/** 解析帧段文本：`0-29` → [0,29]；`300-` → [300,null]（到片尾）；`120` → [120,120]（单帧）；非法返回 null。 */
export function parseFrameRange(text: string | undefined): [number, number | null] | null {
  if (typeof text !== "string") return null
  const raw = text.trim()
  if (!raw) return null
  const single = /^(\d+)$/.exec(raw)
  if (single) {
    const frame = Number(single[1])
    return [frame, frame]
  }
  const from = /^(\d+)\s*-\s*$/.exec(raw)
  if (from) return [Number(from[1]), null]
  const range = /^(\d+)\s*-\s*(\d+)$/.exec(raw)
  if (range) {
    const start = Number(range[1])
    const end = Number(range[2])
    return start <= end ? [start, end] : null
  }
  return null
}

/** 从 Remotion 的并发拒绝错误里取本机上限（`Maximum for --concurrency is N`）。 */
export function parseConcurrencyLimit(message: string): number | null {
  const m = /Maximum for --concurrency is (\d+)/.exec(message)
  if (!m) return null
  const limit = Number(m[1])
  return limit >= 1 ? limit : null
}

/**
 * bench 默认并发候选：有效核数与其一半（去重、降序）——只测两档，实测预算可控。
 * 调用方显式给了 candidates 就以调用方为准。
 */
export function defaultBenchCandidates(cpuCount: number): number[] {
  const cpu = Math.max(1, Math.floor(Number.isFinite(cpuCount) && cpuCount > 0 ? cpuCount : 1))
  return [...new Set([cpu, Math.max(1, Math.ceil(cpu / 2))])].sort((a, b) => b - a)
}

// —— 渲染执行 ——

interface RenderBaseArgs {
  ctx: ToolContext
  job: Job
  libs: NativeLibs
  profile: RenderProfile
  composition: VideoConfig
  serveUrl: string
  browser: NativeBrowser
  inputProps?: Record<string, unknown>
  /** 入口点（工具层透传，仅用于日志定位；打包由调用方完成）。 */
  entryPoint?: string
  /** 日志出口：外部注入（startJob 注入的 log）优先，缺省直接写作业日志。 */
  log?: (line: string) => void
  /** 原生二进制目录（含 remotion/ffmpeg/ffprobe）；null = 用 Remotion 项目内的 compositor 包。 */
  binariesDirectory?: string | null
}

export interface StillArgs extends RenderBaseArgs {
  frame: number
  output: string
  imageFormat?: string
  jpegQuality?: number
  scale?: number
}

export interface MediaArgs extends RenderBaseArgs {
  output: string
  /** [起, 止]；止为 null 表示到片尾；省略 = 全片。 */
  frameRange?: [number, number | null] | null
  scale?: number
  codec?: string
  videoBitrate?: string | null
  crf?: number | null
  imageFormat?: string
  jpegQuality?: number
  concurrency?: number
  /** x264 编码速度档（ultrafast…placebo；不传用 Remotion 内置默认）。 */
  x264Preset?: string | null
  /** 强制硬件编码档（bench 探针用 required）。 */
  hardwareAcceleration?: HardwareAcceleration
}

export interface BenchArgs extends RenderBaseArgs {
  projectDir: string
  candidates: number[]
  /** 光栅化后端候选（实测用；null = 交给 Chrome 自选）。由调用方按主机形态给出，空则只测当前档位。 */
  glCandidates: Array<string | null>
  /** 浏览器可执行文件（实测其他 gl 时要另开浏览器，需同一来源）。 */
  browserExecutable?: string | null
  /** [起, 止]；止为 null 表示到片尾。 */
  frameRange: [number, number | null]
  /** 实测临时产物目录；缺省落在库根 state/bench（测完即删）。 */
  benchDir?: string
  /** 硬件探测输入（`collectProbe().input`）：探针失败时的诊断要靠它区分"无 GPU"与"驱动/ffmpeg 缺 NVENC"，
   *  不能只拿一行原生库报错去猜。 */
  probeInput: ProbeInput
}

/** 日志出口：外部注入（startJob 注入的 log）优先，缺省直接写作业日志。 */
function logTo(args: RenderBaseArgs): (line: string) => void {
  if (args.log) return args.log
  return (line: string) => jobLog(args.job, args.ctx, line)
}

/** 质量参数：硬件档用 videoBitrate（不支持 crf），软件档用 crf（null = 原生库内置值，不传）。 */
function qualityParams(
  profile: RenderProfile,
  opts: { videoBitrate?: string | null; crf?: number | null; hardwareAcceleration?: HardwareAcceleration },
): Record<string, unknown> {
  const hardware = opts.hardwareAcceleration ?? profile.hardwareAcceleration
  const params: Record<string, unknown> = {}
  if (hardware !== "disable") {
    params.hardwareAcceleration = hardware
    params.videoBitrate = opts.videoBitrate ?? profile.videoBitrate ?? "8M"
    return params
  }
  const crf = opts.crf ?? profile.crf
  if (crf !== null && crf !== undefined) params.crf = crf
  return params
}

interface RenderProgress {
  renderedFrames?: number
  encodedFrames?: number
  progress?: number
  stitchStage?: string
}

/** 进度回调 → job.progress（帧数/百分比/fps/预计剩余）+ 节流日志（默认 30s 一行）。 */
function progressReporter(job: Job, log: (line: string) => void, totalFrames: number, started: number, intervalMs = 30_000) {
  let lastLog = 0
  return (p: RenderProgress): void => {
    const elapsedSec = Math.max((Date.now() - started) / 1000, 0.001)
    const fps = p.renderedFrames ? p.renderedFrames / elapsedSec : undefined
    const remaining = p.renderedFrames !== undefined ? Math.max(0, totalFrames - p.renderedFrames) : undefined
    job.progress = {
      stage: p.stitchStage ?? "渲染中",
      renderedFrames: p.renderedFrames,
      totalFrames,
      percent: p.progress !== undefined ? Math.round(p.progress * 100) : undefined,
      fps,
      etaSec: fps && fps > 0 && remaining !== undefined ? remaining / fps : undefined,
    }
    const now = Date.now()
    if (now - lastLog > intervalMs) {
      lastLog = now
      log(describeJob(job))
    }
  }
}

function resolveFrameSpan(args: MediaArgs): [number, number] {
  const total = args.composition.durationInFrames
  const range = args.frameRange
  if (!range) return [0, Math.max(0, total - 1)]
  const start = Math.max(0, range[0])
  const end = range[1] === null || range[1] === undefined ? total - 1 : Math.min(range[1], total - 1)
  if (start > end) throw new Error(`帧段 ${range[0]}-${range[1] ?? "片尾"} 超出合成 ${args.composition.id} 的时长（共 ${total} 帧），请调整 frameRange`)
  return [start, end]
}

/** 静帧：直传原生库（帧号/格式/缩放/热浏览器/chromeMode/chromiumOptions），返回人读摘要。 */
export async function runStill(args: StillArgs): Promise<string> {
  const { job, profile } = args
  const started = Date.now()
  const log = logTo(args)
  const { cancelSignal, cancel } = args.libs.makeCancelSignal()
  bindCancel(job.id, cancel)
  job.progress = { stage: "渲染静帧", renderedFrames: 0, totalFrames: 1 }
  log(
    `静帧渲染：合成 ${args.composition.id} · 帧 ${args.frame} · ${args.imageFormat ?? "png"} · Chrome ${profile.chromeMode}${profile.gl ? ` gl=${profile.gl}` : ""} · 输出 ${args.output}`,
  )
  await args.libs
    .renderStill({
      composition: args.composition,
      serveUrl: args.serveUrl,
      output: args.output,
      frame: args.frame,
      imageFormat: args.imageFormat ?? "png",
      // 质量参数只在 jpeg 帧图下合法（png 传它会被原生库直接拒绝）
      ...((args.imageFormat ?? "png") === "jpeg" && args.jpegQuality !== undefined ? { jpegQuality: args.jpegQuality } : {}),
      scale: args.scale,
      inputProps: args.inputProps,
      puppeteerInstance: args.browser,
      chromeMode: profile.chromeMode,
      chromiumOptions: chromiumOf(profile),
      binariesDirectory: args.binariesDirectory ?? null,
      logLevel: "error",
      overwrite: true,
      cancelSignal,
    })
  job.output = args.output
  job.progress = { stage: "完成", renderedFrames: 1, totalFrames: 1, percent: 100 }
  const ms = Date.now() - started
  log(`静帧完成：${ms}ms`)
  return `静帧已渲染：${args.output}（合成 ${args.composition.id} 帧 ${args.frame}，${(ms / 1000).toFixed(1)}s）`
}

/** 单次 renderMedia 调用的全部输入（整段与分片两条路径共用，差别只在帧段/浏览器/输出/进度出口）。 */
interface SegmentRender {
  libs: NativeLibs
  profile: RenderProfile
  composition: VideoConfig
  serveUrl: string
  browser: NativeBrowser
  inputProps?: Record<string, unknown>
  output: string
  frameRange: [number, number]
  scale?: number
  codec?: string
  videoBitrate?: string | null
  crf?: number | null
  imageFormat?: string
  jpegQuality?: number
  x264Preset?: string | null
  concurrency: number
  hardwareAcceleration?: HardwareAcceleration
  binariesDirectory?: string | null
  /** 只出画面不出声（分片路径用；音轨整段单独渲染后合回）。 */
  muted?: boolean
  cancelSignal: unknown
  onProgress: (progress: RenderProgress) => void
  /** 并发超本机上限、已按上限重试时的通知（作业日志与进度复位）。 */
  onConcurrencyClamp?: (limit: number) => void
}

/** 渲染一段，含「并发超上限自愈重试一次」——本模块唯一的 renderMedia 出口。 */
async function renderSegment(args: SegmentRender): Promise<void> {
  const mediaFormat = args.imageFormat ?? "jpeg"
  const codec = args.codec ?? "h264"
  // 音轨段不吃视频质量参数（码率/crf/hardwareAcceleration 对音频编码器无意义，传了只会报警告）
  const audioOnly = codec === "aac" || codec === "mp3" || codec === "wav"
  const params: Record<string, unknown> = {
    composition: args.composition,
    serveUrl: args.serveUrl,
    codec,
    outputLocation: args.output,
    inputProps: args.inputProps,
    concurrency: args.concurrency,
    frameRange: [args.frameRange[0], args.frameRange[1]],
    imageFormat: mediaFormat,
    ...(mediaFormat === "jpeg" ? { jpegQuality: args.jpegQuality ?? 80 } : {}),
    scale: args.scale,
    puppeteerInstance: args.browser,
    chromeMode: args.profile.chromeMode,
    chromiumOptions: chromiumOf(args.profile),
    binariesDirectory: args.binariesDirectory ?? null,
    logLevel: "error",
    overwrite: true,
    cancelSignal: args.cancelSignal,
    muted: args.muted ?? false,
    ...(args.x264Preset ? { x264Preset: args.x264Preset } : {}),
    onProgress: args.onProgress,
    ...(audioOnly
      ? {}
      : qualityParams(args.profile, {
          videoBitrate: args.videoBitrate,
          crf: args.crf,
          hardwareAcceleration: args.hardwareAcceleration,
        })),
  }
  await args.libs.renderMedia(params).catch(async (err: unknown) => {
    // 自愈：并发超过本机上限时 Remotion 直接拒绝——按报错里的上限重试一次
    const limit = parseConcurrencyLimit((err as Error)?.message ?? "")
    if (limit === null || limit >= args.concurrency) throw err
    args.onConcurrencyClamp?.(limit)
    params.concurrency = limit
    await args.libs.renderMedia(params)
  })
}

/** 预览/成片（单浏览器整段渲染）：帧段/缩放/编码/码率或 crf/并发/硬件档/props。 */
export async function runMediaRender(args: MediaArgs): Promise<string> {
  const { job, profile } = args
  const log = logTo(args)
  const codec = args.codec ?? "h264"
  const hardwareAcceleration = args.hardwareAcceleration ?? profile.hardwareAcceleration
  const [start, end] = resolveFrameSpan(args)
  const totalFrames = end - start + 1
  const { cancelSignal, cancel } = args.libs.makeCancelSignal()
  bindCancel(job.id, cancel)
  job.output = args.output
  job.progress = { stage: "渲染中", renderedFrames: 0, totalFrames, percent: 0 }
  const started = Date.now()
  const first = args.concurrency ?? profile.concurrency
  log(
    `视频渲染：合成 ${args.composition.id} · 帧段 ${start}-${end}（${totalFrames} 帧）· 编码 ${codec}${args.x264Preset ? `（preset ${args.x264Preset}）` : ""} · 并发 ${first} · 硬件编码 ${hardwareAcceleration} · Chrome ${profile.chromeMode}${profile.gl ? ` gl=${profile.gl}` : ""} · 输出 ${args.output}`,
  )
  await renderSegment({
    libs: args.libs,
    profile,
    composition: args.composition,
    serveUrl: args.serveUrl,
    browser: args.browser,
    inputProps: args.inputProps,
    output: args.output,
    frameRange: [start, end],
    scale: args.scale,
    codec,
    videoBitrate: args.videoBitrate,
    crf: args.crf,
    imageFormat: args.imageFormat,
    jpegQuality: args.jpegQuality,
    x264Preset: args.x264Preset,
    concurrency: first,
    hardwareAcceleration,
    binariesDirectory: args.binariesDirectory,
    cancelSignal,
    onProgress: progressReporter(job, log, totalFrames, started),
    onConcurrencyClamp: (limit) => {
      log(`并发 ${first} 超本机上限（${limit}），按上限重试一次`)
      job.progress = { stage: "渲染中（并发已按上限调整）", renderedFrames: 0, totalFrames, percent: 0 }
    },
  })
  const ms = Date.now() - started
  const fps = totalFrames / Math.max(ms / 1000, 0.001)
  job.progress = { ...(job.progress ?? { stage: "完成" }), stage: "完成", percent: 100, renderedFrames: totalFrames, totalFrames, fps }
  log(`渲染完成：${totalFrames} 帧 / ${(ms / 1000).toFixed(1)}s（${fps.toFixed(1)} fps）`)
  const encoding = hardwareAcceleration === "disable" ? "软件编码" : `硬件编码 ${hardwareAcceleration}`
  return `已渲染 ${totalFrames} 帧 → ${args.output}（${(ms / 1000).toFixed(1)}s，${fps.toFixed(1)} fps；${codec} · ${encoding}）`
}

/** 分片路径的附加输入：帧段的切分结果、每片的浏览器、拼接与合轨工具。 */
export interface ShardedArgs extends MediaArgs {
  /** 分片数（≥2）；切分结果由 splitFrameRange 得出，段与段首尾相接不重叠。 */
  shards: number
  pagesPerShard: number
  /** 分片临时目录（分段视频/音轨/拼接清单都落这里，无论成败都清理）。 */
  workDir: string
  /** 拼接与合轨用的 ffmpeg（由调用方解析，缺失就不应进入分片路径）。 */
  ffmpeg: string
  /** 浏览器可执行文件（null = 交给 Remotion 缓存/下载）。 */
  browserExecutable?: string | null
  /** 拼接与合轨的外部件（缺省用 ffmpeg 实现；注入点供单测不走真实外部进程）。 */
  tools?: {
    concat: (opts: { segments: string[]; output: string; listPath: string }) => void
    mux: (opts: { video: string; audio: string; output: string }) => void
  }
}

/**
 * 分片并行渲染：每片一个独立 Chrome 并行出无声视频段，无损拼接后再合回整段音轨。
 *
 * 失败一律回退到单浏览器整段渲染（runMediaRender 的同一条 renderMedia 路径）：
 * 分片是提速手段，不该成为新的失败来源；回退会把分片失败原因写进日志，不静默。
 * 取消不触发回退（用户要的是停下来）。
 */
export async function runShardedRender(args: ShardedArgs): Promise<string> {
  const { job, profile } = args
  const log = logTo(args)
  const codec = args.codec ?? "h264"
  const hardwareAcceleration = args.hardwareAcceleration ?? profile.hardwareAcceleration
  const [start, end] = resolveFrameSpan(args)
  const totalFrames = end - start + 1
  const segments = splitFrameRange(start, end, args.shards)
  const { cancelSignal, cancel } = args.libs.makeCancelSignal()
  bindCancel(job.id, cancel)
  job.output = args.output
  job.progress = { stage: "分片渲染中", renderedFrames: 0, totalFrames, percent: 0 }
  const started = Date.now()
  const pages = Math.max(1, Math.min(args.pagesPerShard, profile.concurrency))

  const segmentPaths = segments.map((_range, i) => join(args.workDir, `seg-${String(i).padStart(2, "0")}.mp4`))
  const audioPath = join(args.workDir, "audio.aac")
  const silentPath = join(args.workDir, "video-only.mp4")
  const listPath = join(args.workDir, "concat.txt")
  const browsers: NativeBrowser[] = []
  const tools =
    args.tools ??
    ({
      concat: (o) => concatVideoSegments({ ffmpeg: args.ffmpeg, segments: o.segments, output: o.output, listPath: o.listPath }),
      mux: (o) => muxAudioVideo({ ffmpeg: args.ffmpeg, video: o.video, audio: o.audio, output: o.output }),
    } satisfies NonNullable<ShardedArgs["tools"]>)

  try {
    mkdirSync(args.workDir, { recursive: true })
    log(
      `分片并行渲染：${segments.length} 片 × ≤${pages} 页 · 帧段 ${start}-${end}（${totalFrames} 帧）· 编码 ${codec}${args.x264Preset ? `（preset ${args.x264Preset}）` : ""} · 硬件编码 ${hardwareAcceleration} · Chrome ${profile.chromeMode}${profile.gl ? ` gl=${profile.gl}` : ""}`,
    )
    log(`分片帧段：${segments.map(([a, b], i) => `${i}:${a}-${b}`).join(" ")}`)

    // 每片一个独立浏览器（分片并行的前提）。这里自己开而不收工厂：
    // 浏览器是**按档位启动**的（gl/chromeMode 为启动参数），交给外部开就可能与 profile 对不上。
    const opened = await Promise.all(
      Array.from({ length: segments.length + 1 }, () =>
        args.libs.openBrowser({
          chromeMode: profile.chromeMode,
          chromiumOptions: chromiumOf(profile),
          ...(args.browserExecutable ? { browserExecutable: args.browserExecutable } : {}),
          logLevel: "error",
        }),
      ),
    )
    browsers.push(...opened)

    const perShard = new Array<number>(segments.length).fill(0)
    const report = (): void => {
      const done = perShard.reduce((a, b) => a + b, 0)
      const elapsed = Math.max((Date.now() - started) / 1000, 0.001)
      const fps = done / elapsed
      job.progress = {
        stage: `分片渲染中（${segments.length} 片并行）`,
        renderedFrames: done,
        totalFrames,
        percent: Math.min(99, Math.round((done / totalFrames) * 100)),
        fps,
        etaSec: fps > 0 && done < totalFrames ? (totalFrames - done) / fps : undefined,
      }
    }

    const videoTasks = segments.map((range, i) =>
      renderSegment({
        libs: args.libs,
        profile,
        composition: args.composition,
        serveUrl: args.serveUrl,
        browser: opened[i]!,
        inputProps: args.inputProps,
        output: segmentPaths[i]!,
        frameRange: range,
        scale: args.scale,
        codec,
        videoBitrate: args.videoBitrate,
        crf: args.crf,
        imageFormat: args.imageFormat,
        jpegQuality: args.jpegQuality,
        x264Preset: args.x264Preset,
        concurrency: pages,
        hardwareAcceleration,
        binariesDirectory: args.binariesDirectory,
        // 分片只出画面：音轨整段渲一次再合回（逐段拼 AAC 会在接缝留下编码器延迟）
        muted: true,
        cancelSignal,
        onProgress: (progress) => {
          perShard[i] = progress.renderedFrames ?? 0
          report()
        },
      }),
    )

    // 音轨：整段一次，与视频分片并行；并发可给足（不做截帧，瓶颈不在画面）
    const audioTask = renderSegment({
      libs: args.libs,
      profile,
      composition: args.composition,
      serveUrl: args.serveUrl,
      browser: opened[opened.length - 1]!,
      inputProps: args.inputProps,
      output: audioPath,
      frameRange: [start, end],
      codec: "aac",
      concurrency: profile.concurrency,
      binariesDirectory: args.binariesDirectory,
      cancelSignal,
      onProgress: () => {},
      muted: false,
    })

    // 等全部结束再判失败：单个分片报错时不能让其余分片与音轨变成悬空任务
    const settled = await Promise.allSettled([...videoTasks, audioTask])
    const failure = settled.find((r) => r.status === "rejected") as PromiseRejectedResult | undefined
    if (failure) throw failure.reason
    const renderedMs = Date.now() - started
    const renderedFps = totalFrames / Math.max(renderedMs / 1000, 0.001)
    log(`分片渲染完成：${totalFrames} 帧 / ${(renderedMs / 1000).toFixed(1)}s（${renderedFps.toFixed(1)} fps）`)

    job.progress = { stage: "拼接中", renderedFrames: totalFrames, totalFrames, percent: 99 }
    tools.concat({ segments: segmentPaths, output: silentPath, listPath })
    log(`已无损拼接 ${segments.length} 段 → ${silentPath}`)

    let merged = silentPath
    let audioNote = "无声（音轨渲染无输出，按无声视频交付）"
    if (existsSync(audioPath) && statSync(audioPath).size > 0) {
      job.progress = { stage: "合轨中", renderedFrames: totalFrames, totalFrames, percent: 99 }
      try {
        tools.mux({ video: silentPath, audio: audioPath, output: args.output })
        merged = args.output
        audioNote = "已合回整段音轨"
        log(`已合轨：${audioPath} → ${args.output}`)
      } catch (err) {
        // 合轨失败不能连视频一起丢：回退到无声视频，并如实说明
        log(`合轨失败，按无声视频交付：${(err as Error).message}`)
        audioNote = "合轨失败，按无声视频交付"
      }
    }
    if (merged !== args.output) {
      rmSync(args.output, { force: true })
      copyFileSync(merged, args.output)
    }

    const ms = Date.now() - started
    const fps = totalFrames / Math.max(ms / 1000, 0.001)
    job.progress = { ...(job.progress ?? { stage: "完成" }), stage: "完成", percent: 100, renderedFrames: totalFrames, totalFrames, fps }
    log(`渲染完成：${totalFrames} 帧 / ${(ms / 1000).toFixed(1)}s（${fps.toFixed(1)} fps）`)
    const encoding = hardwareAcceleration === "disable" ? "软件编码" : `硬件编码 ${hardwareAcceleration}`
    return (
      `已渲染 ${totalFrames} 帧 → ${args.output}（${(ms / 1000).toFixed(1)}s，${fps.toFixed(1)} fps；${codec} · ${encoding}）\n` +
      `分片并行：${segments.length} 片 × ≤${pages} 页 · ${audioNote}`
    )
  } catch (err) {
    const message = (err as Error)?.message ?? String(err)
    if (/cancell?ed|aborted/i.test(message)) throw err
    log(`分片并行渲染失败，回退单浏览器整段渲染：${message}`)
    job.progress = { stage: "回退整段渲染", renderedFrames: 0, totalFrames, percent: 0 }
    const fallback = await runMediaRender(args)
    return `${fallback}\n（分片并行失败已回退整段渲染：${message.slice(0, 200)}）`
  } finally {
    await Promise.all(browsers.map((browser) => browser.close({ silent: true }).catch(() => undefined)))
    rmSync(args.workDir, { recursive: true, force: true })
  }
}

/** 硬件编码强制探针：以 hardwareAcceleration=required 渲染 2 帧——成功即本机原生编码器确实可用。 */
async function probeEncoder(args: BenchArgs, benchDir: string): Promise<EncoderProbe> {
  const checkedAt = new Date().toISOString()
  const output = join(benchDir, "encoder-probe.mp4")
  const { cancelSignal, cancel } = args.libs.makeCancelSignal()
  bindCancel(args.job.id, cancel)
  try {
    await args.libs.renderMedia({
      composition: args.composition,
      serveUrl: args.serveUrl,
      codec: "h264",
      outputLocation: output,
      inputProps: args.inputProps,
      concurrency: 1,
      frameRange: [0, 1],
      imageFormat: "jpeg",
      jpegQuality: 60,
      hardwareAcceleration: "required",
      videoBitrate: "4M",
      puppeteerInstance: args.browser,
      chromeMode: args.profile.chromeMode,
      chromiumOptions: chromiumOf(args.profile),
      binariesDirectory: args.binariesDirectory ?? null,
      logLevel: "error",
      overwrite: true,
      cancelSignal,
      onProgress: () => {},
    })
    return { hardware: true, checkedAt }
  } catch (err) {
    const message = ((err as Error)?.message ?? String(err)).slice(0, 400)
    // 只存原生库报错等于把「为什么失败」交给用户猜：附上**可操作诊断**（结论 + 建议）
    const diagnosis = diagnoseEncoderProbe({
      probeError: message,
      platform: args.probeInput.platform,
      // 诊断依据用**探测事实**（不是字符串噭探）：有 NVIDIA 才谈驱动/ffmpeg，没 NVIDIA 就直接下结论
      gpuForm: args.probeInput.nvidia ? `${args.probeInput.platform} + NVIDIA ${args.probeInput.nvidia.name}` : null,
      remotionVersion: args.probeInput.remotionVersion,
      binariesDirectory: args.binariesDirectory ?? null,
    })
    return { hardware: false, checkedAt, error: message, diagnosis }
  } finally {
    rmSync(output, { force: true })
  }
}

/**
 * bench 的一次实测（固定光栅化后端与并发，量吞吐）。
 * 走 renderSegment（唯一 renderMedia 出口），不带上层作业语义，避免实测日志混入成片日志。
 */
async function benchOnce(
  args: BenchArgs,
  opts: { browser: NativeBrowser; gl: string | null; concurrency: number; benchRange: [number, number]; output: string },
): Promise<number> {
  const { cancelSignal } = args.libs.makeCancelSignal()
  const total = opts.benchRange[1] - opts.benchRange[0] + 1
  const started = Date.now()
  await renderSegment({
    libs: args.libs,
    profile: { ...args.profile, gl: opts.gl as RenderProfile["gl"] },
    composition: args.composition,
    serveUrl: args.serveUrl,
    browser: opts.browser,
    inputProps: args.inputProps,
    output: opts.output,
    frameRange: opts.benchRange,
    concurrency: opts.concurrency,
    imageFormat: "jpeg",
    jpegQuality: 80,
    binariesDirectory: args.binariesDirectory,
    cancelSignal,
    onProgress: () => {},
  })
  const seconds = Math.max((Date.now() - started) / 1000, 0.001)
  return total / seconds
}

/**
 * 实测调优：硬件编码强制探针（required）+ 光栅化后端实测 + 并发候选实测，结论写入调优缓存（键 = profileKey）。
 *
 * 三组结论对应三个可调旋钮：硬件编码（本机能不能用）、gl（画面栅格化走哪个后端）、并发（单浏览器内页数）——
 * 都与机器强相关、只能量，不写死结论。
 */
export async function runBench(args: BenchArgs): Promise<string> {
  const { job, profile } = args
  const log = logTo(args)
  const benchDir = args.benchDir ?? join(stateDir(args.ctx), "bench")
  mkdirSync(benchDir, { recursive: true })
  const lines: string[] = []
  const candidates = args.candidates.length ? args.candidates : defaultBenchCandidates(effectiveCpuCount())

  const encoderProbe = await probeEncoder(args, benchDir)
  const tuning = readTuning(args.ctx)
  tuning.encoderProbe = encoderProbe
  writeTuning(args.ctx, tuning)
  const probeLine = encoderProbe.hardware
    ? "硬件编码探针：通过（hardwareAcceleration=required 渲染成功，本机编码器可用）"
    : `硬件编码探针：未通过——${encoderProbe.diagnosis?.verdict ?? "原因未知"}`
  lines.push(probeLine)
  log(probeLine)
  if (!encoderProbe.hardware) {
    // 建议逐条进日志与结论："未通过"本身不可行动，"缺什么/怎么补"才可行动
    if (encoderProbe.error) {
      lines.push(`  探针报错：${encoderProbe.error}`)
      log(`  探针报错：${encoderProbe.error}`)
    }
    for (const hint of encoderProbe.diagnosis?.hints ?? []) {
      lines.push(`  → ${hint}`)
      log(`  → ${hint}`)
    }
  }

  const benchRange: [number, number] = [
    args.frameRange[0],
    args.frameRange[1] ?? Math.max(args.frameRange[0], args.composition.durationInFrames - 1),
  ]
  const totalFrames = benchRange[1] - benchRange[0] + 1

  // —— 光栅化后端实测：后端挂在浏览器上，所以每个候选各开一台（当前档位那台复用调用方传入的浏览器）。
  // 候选为空 = 不测后端（只用当前档位），调用方按主机形态决定要不要测。
  const glCandidates = args.glCandidates
  const glMeasured: Array<{ gl: string | null; fps: number; owned: NativeBrowser | null }> = []
  /** 跳过的候选也要在人读结论里出现——否则「没测到」会被读成「测了结果是当前档位」。 */
  const glNotes: string[] = []
  for (const gl of glCandidates) {
    const reuse = gl === profile.gl
    let browser = args.browser
    let owned: NativeBrowser | null = null
    if (!reuse) {
      try {
        owned = await args.libs.openBrowser({
          chromeMode: profile.chromeMode,
          chromiumOptions: gl ? { gl } : {},
          ...(args.browserExecutable ? { browserExecutable: args.browserExecutable } : {}),
          logLevel: "error",
        })
        browser = owned
      } catch (err) {
        const note = `gl=${gl ?? "auto"} 实测跳过（浏览器启动失败：${(err as Error)?.message ?? String(err)}）`
        log(note)
        glNotes.push(note)
        continue
      }
    }
    const output = join(benchDir, `bench-gl-${gl ?? "auto"}.mp4`)
    job.progress = { stage: `实测 gl=${gl ?? "auto"}`, renderedFrames: 0, totalFrames, percent: 0 }
    try {
      const fps = await benchOnce(args, { browser, gl, concurrency: profile.concurrency, benchRange, output })
      glMeasured.push({ gl, fps, owned })
      log(`gl=${gl ?? "auto（Chrome 自选）"}：${fps.toFixed(1)} fps`)
    } catch (err) {
      const note = `gl=${gl ?? "auto"} 实测失败：${(err as Error)?.message ?? String(err)}`
      log(note)
      glNotes.push(note)
      if (owned) await owned.close({ silent: true }).catch(() => undefined)
    } finally {
      rmSync(output, { force: true })
    }
  }
  glMeasured.sort((a, b) => b.fps - a.fps)
  const bestGl = glMeasured.length ? glMeasured[0]!.gl : profile.gl
  const benchProfile: RenderProfile = { ...profile, gl: bestGl as RenderProfile["gl"] }
  // 胜出后端的浏览器继续供并发实测使用，其余关掉
  const benchBrowser = glMeasured[0]?.owned ?? args.browser
  for (const measuredGl of glMeasured) {
    if (measuredGl.owned && measuredGl.owned !== benchBrowser) await measuredGl.owned.close({ silent: true }).catch(() => undefined)
  }

  const measured: Array<{ concurrency: number; fps: number; seconds: number }> = []
  for (const concurrency of candidates) {
    const output = join(benchDir, `bench-${concurrency}.mp4`)
    job.progress = { stage: `实测并发 ${concurrency}`, renderedFrames: 0, totalFrames, percent: 0 }
    const started = Date.now()
    try {
      await benchOnce(args, { browser: benchBrowser, gl: bestGl, concurrency, benchRange, output })
      const seconds = Math.max((Date.now() - started) / 1000, 0.001)
      const fps = totalFrames / seconds
      measured.push({ concurrency, fps, seconds })
      log(`并发 ${concurrency}：${seconds.toFixed(1)}s（${fps.toFixed(1)} fps）`)
    } catch (err) {
      log(`并发 ${concurrency} 实测失败：${(err as Error)?.message ?? String(err)}`)
    } finally {
      rmSync(output, { force: true })
    }
  }
  if (benchBrowser !== args.browser) await benchBrowser.close({ silent: true }).catch(() => undefined)
  if (!measured.length) throw new Error("并发实测全部失败，未写入调优缓存：请先确认项目可正常渲染（reel_render action=still）")
  measured.sort((a, b) => b.fps - a.fps)
  const best = measured[0]!
  const entry: TunedEntry = {
    concurrency: best.concurrency,
    gl: bestGl,
    chromeMode: benchProfile.chromeMode,
    // 探针未通过则如实记软件档，不以推测冒充"已启用 GPU"
    hardwareAcceleration: encoderProbe.hardware ? profile.hardwareAcceleration : "disable",
    fps: best.fps,
    measuredAt: new Date().toISOString(),
  }
  const key = profileKey(args.projectDir, args.composition.id)
  const file = readTuning(args.ctx)
  file.encoderProbe = encoderProbe
  file.entries[key] = entry
  writeTuning(args.ctx, file)

  if (glMeasured.length) {
    lines.push(
      `光栅化后端实测：${glMeasured.map((m) => `${m.gl ?? "auto"}（${m.fps.toFixed(1)} fps）`).join("；")} → 采用 ${bestGl ?? "auto"}`,
    )
  }
  lines.push(...glNotes)
  lines.push(`并发实测：${measured.map((m) => `${m.concurrency}（${m.fps.toFixed(1)} fps）`).join("；")}`)
  lines.push(`已写入调优缓存 ${key}：并发 ${best.concurrency} · gl ${bestGl ?? "auto"} · 硬件档 ${entry.hardwareAcceleration}（后续同项目同合成自动采用）`)
  lines.push(`缓存文件：${tuningPath(args.ctx)}（按库根隔离——换 REEL_LIBRARY_DIR 即另一份缓存，不跨库根共享）`)
  log(lines[lines.length - 1]!)
  return lines.join("\n")
}
