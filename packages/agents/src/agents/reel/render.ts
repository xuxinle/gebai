/**
 * reel_render：渲染与实测调优（进程内直连 @remotion/renderer，不经 CLI）。
 * - still 静帧（逐镜 QA 主力，秒级）/ preview 低清预览段 / video 成片（可帧段、可 props 变体）
 * - bench 实测并发与硬件编码探针（结论写入调优缓存，后续渲染自动采用）
 * - status / log / stop 查询与中止
 * 长任务一律后台作业：工具调用立即返回作业 ID，用 status 轮询，不必干等。
 * 例外是送审主帧：`still wait=true` 同步等这一帧渲完并把帧图直接附在结果里（用户当场可见、模型也可自查）——
 * 一次调用只给一帧，正好对上一次「一节一送」的确认动作。
 */
import { appendFileSync, mkdirSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import type { Tool, ToolContext, ToolResult } from "@gebai/sdk"
import { artifactBlocks, mimeFor, previewLogicalPath, schema } from "@gebai/sdk/node"
import { collectProbe, effectiveCpuCount } from "./detect"
import { browserReadiness, expectedChromeVersion, resolveBinariesDirectory, resolveBrowserExecutable, BROWSER_EXECUTABLE_ENV, BINARIES_DIR_ENV, type BrowserReadiness } from "./external"
import { DRAFT, X264_PRESETS, asX264Preset, resolveOutputScale, resolveVideoSize } from "./output"
import {
  cancelJob,
  createJob,
  defaultBenchCandidates,
  describeJob,
  getJob,
  listJobs,
  parseFrameRange,
  pickThroughput,
  pickTuned,
  readJobLog,
  readTuning,
  runBench,
  runMediaRender,
  runShardedRender,
  runStill,
  startJob,
  waitJob,
  writeThroughput,
} from "./jobs"
import { averageCoresUsed, startCpuSampling } from "./cpu-sampler"
import { coresUsedOf, decideProfile, defaultGlCandidates, describeProfile, profileKey, type ProfileOverride, type RenderProfile, type ThroughputEntry } from "./profile"
import { bundleProject, declaredEntryOutsideProject, detectEntryPoint, listCompositions, loadNativeLibs, openSharedBrowser, resolveComposition, type NativeBrowser } from "./runtime"
import { isRuntimeReady, resolveOutputPath, resolveProjectDir, runtimeDir, stateDir, uniqueOutputPath } from "./paths"
import { BROWSER_BACKENDS, BACKEND_ENV, isBrowserBackend, runBrowserRender } from "./browser-render"
import { CPU_BOUND_RATIO, MIN_FRAMES_PER_SHARD, planShards, resolveShardFfmpeg } from "./shards"

/** 解析 props 参数：对象直传、JSON 文本、或指向 JSON 文件的路径。 */
async function parseProps(raw: unknown): Promise<Record<string, unknown>> {
  if (raw === undefined || raw === null) return {}
  if (typeof raw === "object") return raw as Record<string, unknown>
  const text = String(raw).trim()
  if (!text) return {}
  if (text.startsWith("{")) {
    try {
      return JSON.parse(text) as Record<string, unknown>
    } catch (err) {
      throw new Error(`props 不是合法 JSON：${(err as Error).message}`)
    }
  }
  const file = Bun.file(text)
  if (!(await file.exists())) throw new Error(`props 文件不存在：${text}`)
  try {
    return JSON.parse(await file.text()) as Record<string, unknown>
  } catch (err) {
    throw new Error(`props 文件不是合法 JSON（${text}）：${(err as Error).message}`)
  }
}

/** 从参数收集渲染档覆盖（未给的项不覆盖自动决策）。 */
function overrideFrom(args: Record<string, unknown>): ProfileOverride {
  const override: ProfileOverride = {}
  if (typeof args.concurrency === "number") override.concurrency = args.concurrency
  if (args.gl !== undefined) override.gl = args.gl === "off" || args.gl === "none" ? null : String(args.gl)
  if (args.chrome_mode !== undefined) override.chromeMode = String(args.chrome_mode)
  if (args.hardware_acceleration !== undefined) override.hardwareAcceleration = String(args.hardware_acceleration) as RenderProfile["hardwareAcceleration"]
  if (args.video_bitrate !== undefined) override.videoBitrate = args.video_bitrate === "" ? null : String(args.video_bitrate)
  if (args.crf !== undefined) override.crf = typeof args.crf === "number" ? args.crf : null
  if (process.env.REEL_GPU === "off") override.gpu = "off"
  return override
}

/**
 * 浏览器来源行：渲染前把"用哪来的浏览器、会不会触发下载"讲清楚——
 * 未就绪时打警示（内网环境下这一步即是失败根因，不必等崩了再查）。
 */
function browserLine(state: BrowserReadiness): string {
  return `浏览器：${state.ready && !state.versionMismatch ? "" : "⚠ "}${state.note}`
}

/**
 * 准备阶段的时限：打包与浏览器**各自一道**，环境变量可调。
 *
 * 为什么需要：这一步发生在 `createJob` **之前**——既没有作业记录、也没有进度，外部只看到“工具没反应”；
 * 而本地无缓存又连不上网时，Remotion 的浏览器下载（chrome-headless-shell 约 150MB）会在内网长时间挂住。
 * 两道独立时限把“静默挂死”变成“哪一步、为什么、怎么修”的明确失败，慢网/大工程可按环境变量放宽。
 */
const BROWSER_TIMEOUT_MS = 3 * 60 * 1000
const BUNDLE_TIMEOUT_MS = 5 * 60 * 1000

/** 时限取值：环境变量（毫秒）优先，取不到用默认值（测试可注入极短值覆盖超时分支）。 */
function deadlineOf(envKey: string, fallbackMs: number): number {
  const raw = Number(process.env[envKey])
  return Number.isFinite(raw) && raw > 0 ? raw : fallbackMs
}

const secs = (ms: number): string => `${Math.round(ms / 1000)}s`

/** 给不带超时的异步阶段加一道硬时限（超时抛带修复指引的错误）。 */
async function withDeadline<T>(work: Promise<T>, ms: number, onTimeout: () => string): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined
  try {
    return await Promise.race([
      work,
      new Promise<T>((_, rej) => {
        timer = setTimeout(() => rej(new Error(onTimeout())), ms)
      }),
    ])
  } finally {
    if (timer) clearTimeout(timer)
  }
}

/** 一次可渲染环境：bundle + 浏览器，以及两段的耗时与浏览器来源。 */
interface Prepared {
  serveUrl: string
  browser: NativeBrowser
  bundleCached: boolean
  bundleMs: number
  browserMs: number
  browserState: BrowserReadiness
}

/**
 * 准备阶段轨迹（内存 + `state/prepare.log`）：准备期没有作业记录，这里就是“卡在哪一步”的唯一线索——
 * `action=status` 会报出来，包括上层调用被超时打断后仍在进行中的准备。
 */
interface PrepareTrail {
  ctx: ToolContext
  project: string
  startedAt: number
  stage: string
  progress: string
  lines: string[]
  ended?: string
}

let prepareTrail: PrepareTrail | null = null

function prepareLogPath(ctx: ToolContext): string {
  return join(stateDir(ctx), "prepare.log")
}

function beginPrepare(ctx: ToolContext, project: string): PrepareTrail {
  const trail: PrepareTrail = { ctx, project, startedAt: Date.now(), stage: "打包工程", progress: "", lines: [] }
  prepareTrail = trail
  try {
    mkdirSync(stateDir(ctx), { recursive: true })
    writeFileSync(prepareLogPath(ctx), `${new Date().toISOString()} 准备开始：${project}\n`)
  } catch {
    /* 日志落盘失败不阻断渲染 */
  }
  return trail
}

function trailLog(trail: PrepareTrail, line: string): void {
  trail.lines.push(line)
  if (trail.lines.length > 200) trail.lines.shift()
  try {
    appendFileSync(prepareLogPath(trail.ctx), `${new Date().toISOString()} ${line}\n`)
  } catch {
    /* 同上 */
  }
}

/** 准备阶段摘要（status 用）：进行中/已完成/失败 + 当前阶段与进度 + 最近几行日志。 */
function describePrepare(): string | null {
  const trail = prepareTrail
  if (!trail) return null
  const elapsed = Math.round((Date.now() - trail.startedAt) / 1000)
  const head = `准备阶段（${trail.ended ?? "进行中"} · ${trail.stage} · ${elapsed}s）：${trail.project}${trail.progress ? ` · ${trail.progress}` : ""}`
  const tail = trail.lines.slice(-4)
  return tail.length ? `${head}\n${tail.map((l) => `  ${l}`).join("\n")}` : head
}

/** 准备耗时与浏览器来源：每次渲染都报出来，慢在哪一步、用的是哪来的浏览器一眼可见。 */
function prepareLine(prepared: Prepared, entryWarning?: string | null): string {
  const source =
    prepared.browserState.source === "configured" ? "配置的可执行文件" : prepared.browserState.source === "local-cache" ? "本地缓存" : "Remotion 缓存/下载"
  const line = `准备：打包 ${prepared.bundleCached ? "复用已打包产物" : `${prepared.bundleMs}ms`} · 浏览器 ${prepared.browserMs}ms（${source}）`
  return entryWarning ? `${line}\n${entryWarning}` : line
}

/** 准备失败的统一出口：阶段错误 + 已发生的准备日志尾部（这一步不进作业系统，故必须自带上下文与修复指引）。 */
function prepareFailure(message: string, trail: PrepareTrail): string {
  return [
    `⚠ 渲染准备失败：${message}`,
    ...(trail.lines.length ? [`准备日志（最近 ${Math.min(8, trail.lines.length)} 行）：`, ...trail.lines.slice(-8).map((l) => `  ${l}`)] : []),
    `查询：reel_render action=status（准备阶段轨迹）｜日志文件：${prepareLogPath(trail.ctx)}`,
  ].join("\n")
}

/**
 * 上次整片的实测吞吐（人读）：讲清“多快、CPU 有没有余量”——这决定了分片等提速手段有没有意义。
 * 只认 cgroup 口径：宿主全局读数含其他租户，不拿它下结论。
 */
function throughputLine(entry: ThroughputEntry | null): string | null {
  if (!entry || entry.frames <= 0 || entry.wallMs <= 0) return null
  const fps = (entry.frames * 1000) / entry.wallMs
  if (entry.cpuSource !== "cgroup") {
    return `性能：上次整片实测 ${fps.toFixed(1)} fps（CPU 采样口径不可用于判定：${entry.cpuSource}）`
  }
  const cores = entry.cores > 0 ? entry.cores : 0
  const used = coresUsedOf(entry)
  const bound = cores > 0 && used >= CPU_BOUND_RATIO * cores
  return `性能：上次整片实测 ${fps.toFixed(1)} fps · CPU 在用 ${used.toFixed(1)}/${cores} 核${bound ? "（配额已吃满）" : "（有余量）"}`
}

export const renderTool: Tool = {
  name: "render",
  description:
    "渲染与实测调优（进程内直连 @remotion/renderer，非 CLI；热打包 + 热浏览器复用）：still 静帧（逐镜 QA）/ preview 低清预览段 / video 成片（可帧段与 props 变体，帧段够长时默认分片并行）/ bench 实测光栅化后端、并发与硬件编码探针（写调优缓存，后续渲染自动采用）/ status 进度 / log 日志 / stop 中止。长任务后台执行，立即返回作业 ID。",
  parameters: schema(
    {
      action: { type: "string", enum: ["still", "preview", "video", "bench", "status", "log", "stop"], description: "渲染动作或作业管理" },
      project: { type: "string", description: "视频工程目录（缺省用 REEL_PROJECT）" },
      composition: { type: "string", description: "合成 ID（缺省取工程内第一个合成）" },
      frame: { type: "number", description: "still：渲染哪一帧（默认 0；-1 = 末帧）" },
      frame_range: { type: "string", description: "preview/video/bench：帧段 `起始-结束`（含端点；`0-` 表示到片尾）" },
      height: { type: "number", description: "目标高度（按合成长宽比换算输出尺寸，如 720 / 540）：1080p 合成取 720 得 1280×720。与 scale 二选一（推荐它——自动满足 h264 的偶数尺寸要求）" },
      scale: { type: "number", description: "缩放比例（preview 默认 0.5；video 默认 1）。注意必须是能得出整数且偶数宽高的值：1080p 用 0.667 会得到 1281×720 而被编码器拒绝，请改用 height 或 2/3" },
      quality: { type: "string", description: "preview/video 画质档：final（默认，全质量交付）/ draft（快速草稿：半分辨率 + ultrafast 编码 + 帧图质量 70——只用于确认动效与节奏，不用于交付）。注意：半分辨率仅在 remotion 通道生效（实测该通道 540p 比 1080p 快 1.47×）；浏览器通道降分辨率实测只有 1.04×，故 draft 在那些通道保持全分辨率，快速确认请用 frame_range 缩片段" },
      codec: { type: "string", description: "视频编码：h264（默认）/ h265 / vp9 / prores" },
      x264_preset: { type: "string", description: `软件编码速度档：${X264_PRESETS.join(" / ")}（缺省 Remotion 内置 medium）；ultrafast 省约 80% 编码时间、体积约 +1.4 倍（合成画面近无损，实拍素材慎用）` },
      video_bitrate: { type: "string", description: "视频码率（硬件编码下必用其控质量，默认 8M；与 crf 互斥）" },
      crf: { type: "number", description: "软件编码质量因子（不传则用 Remotion 内置值）" },
      image_format: { type: "string", description: "帧图格式：still 默认 png；video 默认 jpeg（更快）" },
      jpeg_quality: { type: "number", description: "jpeg 质量 0-100（默认 82）" },
      props: { type: "string", description: "输入属性：JSON 文本或 JSON 文件路径（如 {\"bgm\":false} 渲无音乐版）" },
      out: { type: "string", description: "输出路径（默认 <工程>/out/<合成>-<类型>.<扩展名>；相对路径以工程目录为基准，绝对路径直通）" },
      wait: { type: "boolean", description: "同步等作业完成并把产物附在结果里（still 附帧图、preview/video 附视频文件；送审用，用户当场可见）；默认 false 保持后台作业语义（长片段建议仍走后台）" },
      chrome_executable: { type: "string", description: `浏览器可执行文件（Chrome/Chromium 路径；缺省用 ${BROWSER_EXECUTABLE_ENV}、.reel.json 的 browserExecutable，都没有则交给 Remotion 缓存/下载）` },
      binaries_directory: { type: "string", description: `原生二进制目录（含 remotion/ffmpeg/ffprobe，用于换内置 ffmpeg；缺省用 ${BINARIES_DIR_ENV}、.reel.json 的 binariesDirectory）` },
      concurrency: { type: "number", description: "并发数（默认按 CPU 与实测调优决策）" },
      shards: { type: "number", description: "分片并行度：帧段切 K 片、每片一个独立浏览器并行渲染，再无损拼接并合回整轨（默认按帧数与核数自动决定、上限 6；1 = 强制整段单浏览器渲染）。帧段足够长时约 2x 吞吐" },
      gl: { type: "string", description: "Chromium 光栅化后端（auto 默认 / off 不指定 / angle / vulkan / egl / swangle）" },
      chrome_mode: { type: "string", description: "Chrome 形态（headless-shell 默认 / chrome-for-testing）" },
      hardware_acceleration: { type: "string", description: "硬件编码（disable / if-possible / required；auto 默认由档位决策）" },
      candidates: { type: "string", description: "bench：并发候选（逗号分隔；默认 [有效核数, 其一半]）" },
      job: { type: "string", description: "status/log/stop：作业 ID（status 省略则列出全部）" },
      backend: {
        type: "string",
        description:
          `渲染通道（preview / video 适用）：remotion（默认——DOM + CDP 截帧，保真度最高）/ dom-canvas（保留 DOM 与镜头原语不动，改走 canvas 直捕 + 浏览器内编码；注意该通道下 backdrop-filter 与大 blur 会拖慢每帧重栅格化）/ record（captureStream + MediaRecorder 实时录制，用于交互与实时内容——不可能快于实时、时序跟墙钟且不可复现）/ canvas（需 canvas 版原语，暂未实现）。缺省取环境变量 ${BACKEND_ENV}，都没有则 remotion`,
      },
      tail: { type: "number", description: "log：返回日志尾部行数（默认 60）" },
    },
    ["action"],
  ),
  outputSchema: {
    type: "object",
    properties: {
      jobId: { type: "string" },
      kind: { type: "string" },
      output: { type: "string" },
      composition: { type: "string" },
      profile: { type: "object" },
      jobs: { type: "array", items: { type: "object" } },
    },
  },
  requiresApproval: true,
  async execute(args, ctx): Promise<ToolResult> {
    const action = String(args.action ?? "")

    /* ── 作业管理（无工程依赖） ── */
    if (action === "status") {
      const id = args.job ? String(args.job) : ""
      if (!id) {
        const jobs = listJobs(ctx, 12)
        const prepare = describePrepare()
        if (!jobs.length) {
          return { output: ["本进程内没有渲染作业记录。可用：still / preview / video / bench。", ...(prepare ? [prepare] : [])].join("\n") }
        }
        return {
          output: ["最近渲染作业（新→旧）：", ...jobs.map((j) => `  ${describeJob(j)}`), ...(prepare ? [prepare] : [])].join("\n"),
          data: { jobs },
        }
      }
      const job = getJob(id)
      if (!job) return { output: `未找到作业：${id}` }
      const logTail = readJobLog(ctx, id, 12)
      return {
        output: [describeJob(job), job.output ? `输出：${job.output}` : "", logTail ? `日志尾部：\n${logTail}` : ""].filter(Boolean).join("\n"),
        data: { jobId: id, kind: job.kind },
      }
    }
    if (action === "log") {
      const id = args.job ? String(args.job) : listJobs(ctx, 1)[0]?.id
      if (!id) return { output: "没有可查看的作业（先发起 still / preview / video / bench）" }
      const tail = typeof args.tail === "number" ? Math.max(1, Math.min(400, args.tail)) : 60
      const text = readJobLog(ctx, id, tail)
      return { output: text || `作业 ${id} 暂无日志`, data: { jobId: id } }
    }
    if (action === "stop") {
      const id = args.job ? String(args.job) : ""
      if (!id) {
        const running = listJobs(ctx, 20).find((j) => j.status === "running" || j.status === "queued")
        if (!running) return { output: "没有正在运行的作业" }
        const okRunning = cancelJob(running.id)
        return { output: okRunning ? `已中止作业 ${running.id}` : `作业 ${running.id} 无法中止（可能已结束）` }
      }
      const ok = cancelJob(id)
      return { output: ok ? `已中止作业 ${id}` : `作业 ${id} 未在运行（或已结束）` }
    }

    if (!["still", "preview", "video", "bench"].includes(action)) {
      return { output: `未知动作：${action}（可用：still / preview / video / bench / status / log / stop）` }
    }

    /* ── 渲染类动作：解析工程与档位 → 登记作业 → 后台执行 ── */
    const projectDir = resolveProjectDir(ctx, args.project ? String(args.project) : undefined)
    // 外部件先解析（配置了但路径不可用时立即报错，不把问题留到渲染中途）
    const browserExec = resolveBrowserExecutable({ ctx, projectDir, arg: args.chrome_executable })
    const binaries = resolveBinariesDirectory({ ctx, projectDir, arg: args.binaries_directory })
    if (!isRuntimeReady(ctx)) {
      return { output: `共享运行时依赖未就绪（${ctx.home}/vendor/reel/runtime）——先执行 reel_project action=install，或用 reel_project action=init 一并准备。` }
    }
    const entryPoint = (() => {
      try {
        return detectEntryPoint(projectDir)
      } catch (err) {
        throw new Error(`入口点探测失败：${(err as Error).message}（先 reel_project action=init）`)
      }
    })()
    // 清单里的入口若指向工程之外（工程被复制/移动过），已降级用本工程入口——这类不一致极难发现，必须报出
    const entryWarning = declaredEntryOutsideProject(projectDir)
      ? `⚠ 工程清单的入口点指向本工程之外（工程可能被复制/移动过），已改用本工程入口：${entryPoint}——建议 reel_project action=init 重写清单`
      : null

    let libs
    try {
      libs = await loadNativeLibs(projectDir)
    } catch (err) {
      return { output: `原生渲染库加载失败：${(err as Error).message}\n\n→ 修复：reel_project action=install（重建依赖）` }
    }

    const probe = await collectProbe(ctx, { projectDir })
    const tuning = readTuning(ctx)
    const inputProps = await parseProps(args.props)
    const override = overrideFrom(args as Record<string, unknown>)

    // 浏览器是**按档位启动**的：gl 与 chromeMode 都是启动参数，渲染时再传 chromiumOptions 对已启动的浏览器无效。
    // 所以先定档（合成 ID 已知就直接查调优条目）、再准备浏览器；只有拿不到合成 ID 时才先准备一次去列合成。
    let compositionId = args.composition ? String(args.composition) : ""
    let profile = decideProfile(probe.input, override, compositionId ? pickTuned(tuning, profileKey(projectDir, compositionId)) : null)

    // 浏览器三选一（配置的可执行文件 / 本机缓存 / 交给 Remotion 下载）：本机有可执行文件就**显式指定**——
    // 交给 Remotion 自行判定时，缓存 VERSION 与当前版本不一致会被“先删掉缓存再联网下载”，内网等于自毁可用浏览器。
    const browserStateOf = (forProfile: RenderProfile): BrowserReadiness =>
      browserReadiness({
        mode: forProfile.chromeMode,
        browserExecutable: browserExec.path,
        expectedVersion: expectedChromeVersion(runtimeDir(ctx)),
        alsoFrom: [projectDir, runtimeDir(ctx)],
      })

    let browserState = browserStateOf(profile)
    const trail = beginPrepare(ctx, projectDir)
    const browserDeadline = deadlineOf("GEBAI_REEL_BROWSER_TIMEOUT_MS", BROWSER_TIMEOUT_MS)
    const bundleDeadline = deadlineOf("GEBAI_REEL_BUNDLE_TIMEOUT_MS", BUNDLE_TIMEOUT_MS)

    /** 取（或热复用）该档位的浏览器：单独一道时限，超时报出浏览器现状与修复动作。 */
    const openBrowser = async (forProfile: RenderProfile): Promise<{ browser: NativeBrowser; state: BrowserReadiness; ms: number }> => {
      const state = browserStateOf(forProfile)
      trail.stage = "打开浏览器"
      trail.progress = ""
      trailLog(trail, `浏览器：${state.note}`)
      const started = Date.now()
      const browser = await withDeadline(
        openSharedBrowser({
          libs,
          projectDir,
          profile: forProfile,
          browserExecutable: state.executablePath,
          onLog: (line) => trailLog(trail, line),
          onDownloadProgress: (percent) => {
            trail.progress = `下载浏览器 ${Math.round(percent)}%`
          },
        }),
        browserDeadline,
        () =>
          `浏览器阶段超时（${secs(browserDeadline)}）：${state.note}。` +
          `修复：配置 chrome_executable（或 ${BROWSER_EXECUTABLE_ENV} / .reel.json 的 browserExecutable）指向本机 Chrome/Chromium，` +
          `或先执行 reel_setup install=true 准备浏览器；慢网可放宽 GEBAI_REEL_BROWSER_TIMEOUT_MS。`,
      )
      return { browser, state, ms: Date.now() - started }
    }

    /**
     * 准备一次可渲染环境：打包 → 浏览器。两步各自限时、各自报错；
     * 换档（gl/chromeMode 变化）只需换浏览器，bundle 与档位无关可继续复用。
     */
    const prepareFor = async (forProfile: RenderProfile): Promise<Prepared> => {
      trail.stage = "打包工程"
      trail.progress = ""
      const bundled = await withDeadline(
        bundleProject({
          ctx,
          libs,
          projectDir,
          entryPoint,
          onLog: (line) => trailLog(trail, line),
          onBundleProgress: (percent) => {
            trail.progress = `打包 ${Math.round(percent)}%`
          },
        }).catch((err: unknown) => {
          throw new Error(`打包失败：${(err as Error).message}`)
        }),
        bundleDeadline,
        () => `打包超时（${secs(bundleDeadline)}）——工程过大或依赖异常：可放宽 GEBAI_REEL_BUNDLE_TIMEOUT_MS；依赖缺失时先 reel_project action=install`,
      )
      const opened = await openBrowser(forProfile)
      return { ...bundled, browser: opened.browser, browserMs: opened.ms, browserState: opened.state }
    }

    let prepared: Prepared
    try {
      prepared = await prepareFor(profile)
      browserState = prepared.browserState
      if (!compositionId) {
        trail.stage = "解析合成"
        const comps = await listCompositions({ libs, serveUrl: prepared.serveUrl, profile, browser: prepared.browser })
        if (!comps.length) throw new Error("工程内没有注册任何合成（Composition）——检查 src/Root.tsx")
        compositionId = comps[0].id
        const refined = decideProfile(probe.input, override, pickTuned(tuning, profileKey(projectDir, compositionId)))
        // 调优条目可能把 gl/chromeMode 换掉：那就得换一台对应档位的浏览器，否则那份调优形同虚设（bundle 与档位无关，继续复用）
        if (refined.gl !== profile.gl || refined.chromeMode !== profile.chromeMode) {
          profile = refined
          const opened = await openBrowser(profile)
          prepared = { ...prepared, browser: opened.browser, browserMs: opened.ms, browserState: opened.state }
          browserState = opened.state
        } else {
          profile = refined
        }
      }
      trail.ended = "完成"
    } catch (err) {
      trail.ended = "失败"
      return { output: prepareFailure((err as Error).message, trail) }
    }
    const composition = await resolveComposition({
      libs,
      serveUrl: prepared.serveUrl,
      compositionId,
      inputProps,
      profile,
      browser: prepared.browser,
    })

    if (action === "bench") {
      const parsed = String(args.candidates ?? "")
        .split(",")
        .map((s) => Number(s.trim()))
        .filter((n) => Number.isFinite(n) && n > 0)
      // 默认候选（有效核数与其一半）：不显式指定也要有档可测，否则实测等于空跑
      const candidates = parsed.length ? parsed : defaultBenchCandidates(effectiveCpuCount())
      const range = parseFrameRange(args.frame_range ? String(args.frame_range) : undefined) ?? [0, Math.min(11, composition.durationInFrames - 1)]
      const job = createJob({ ctx, kind: "bench", project: projectDir, composition: compositionId })
      startJob(job, ctx, (log) =>
        runBench({
          ctx, libs, serveUrl: prepared.serveUrl, projectDir, entryPoint, profile, composition, inputProps,
          frameRange: [range[0], range[1] ?? null], candidates, browser: prepared.browser, job, log,
          glCandidates: defaultGlCandidates(probe.input, profile.gl),
          browserExecutable: browserExec.path,
          binariesDirectory: binaries.path,
          probeInput: probe.input,
        }),
      )
      return {
        output: [
          `已启动实测调优作业：${job.id}（帧段 ${range[0]}-${range[1] ?? "片尾"} · 并发候选 ${candidates.join(", ")}）`,
          browserLine(browserState),
          prepareLine(prepared, entryWarning),
          `说明：bench 先用 hardware_acceleration=required 做硬件编码强制探针（实测本机原生编码器是否可用），再实测各光栅化后端（gl）吞吐，最后逐并发候选实测；最优档写入缓存供后续渲染自动采用。`,
          `查询进度：reel_render action=status job=${job.id}｜日志：action=log job=${job.id}`,
        ].join("\n"),
        data: { jobId: job.id, kind: "bench", composition: compositionId, profile },
      }
    }

    if (action === "still") {
      const frameArg = typeof args.frame === "number" ? args.frame : 0
      const frame = frameArg < 0 ? Math.max(0, composition.durationInFrames + frameArg) : frameArg
      // 产物唯一化：同路径重渲改为 `-v2/-v3`（对话里按路径引用产物，历史不得被覆盖）
      const stillUnique = uniqueOutputPath(resolveOutputPath(projectDir, args.out, join("out", `${compositionId}-frame${frame}.png`)))
      const out = stillUnique.path
      if (args.height !== undefined && args.scale !== undefined) {
        return { output: "height 与 scale 二选一：height=<目标高>（按合成长宽比换算，推荐）或 scale=<比例>" }
      }
      // 帧图无 h264 的偶数尺寸约束，仍接受 height（换算成等价 scale）
      const stillScale = args.height !== undefined ? Number(args.height) / composition.height : typeof args.scale === "number" ? args.scale : 1
      if (!Number.isFinite(stillScale) || stillScale <= 0) {
        return { output: `输出尺寸参数非法：height=${String(args.height)} scale=${String(args.scale)}（都须为正数）` }
      }
      // png 帧图下不能携带质量参数（原生库会直接拒绝）
      const stillFormat = args.image_format ? String(args.image_format) : "png"
      const stillQuality = typeof args.jpeg_quality === "number" ? args.jpeg_quality : 82
      const job = createJob({ ctx, kind: "still", project: projectDir, composition: compositionId, output: out })
      startJob(job, ctx, (log) =>
        runStill({
          ctx, libs, serveUrl: prepared.serveUrl, composition, output: out, frame,
          imageFormat: stillFormat,
          ...(stillFormat === "jpeg" ? { jpegQuality: stillQuality } : {}),
          scale: stillScale,
          profile, browser: prepared.browser, inputProps, job, log,
          binariesDirectory: binaries.path,
        }),
      )
      const startLines = [
        `已启动静帧渲染作业：${job.id}`,
        `输出：${out}`,
        // 改名必须明说：否则模型看到路径与请求不符会误以为出错
        ...(stillUnique.renamedFrom ? [`（原路径 ${stillUnique.renamedFrom} 已存在，为避免覆盖历史产物自动改名——对话里按路径引用图片，同名覆盖会让旧消息里的图变成新图）`] : []),
        `计划档位：${describeProfile(profile, probe.input)[0]} · 合成 ${compositionId}（${composition.width}×${composition.height} · ${composition.fps}fps）`,
        browserLine(browserState),
        prepareLine(prepared, entryWarning),
      ]
      if (args.wait !== true) {
        return {
          output: [...startLines, `查询进度：reel_render action=status job=${job.id}｜日志：action=log job=${job.id}`].join("\n"),
          data: { jobId: job.id, kind: "still", output: out, composition: compositionId, profile },
        }
      }
      // 送审主帧：等这一帧落地，把图直接附进结果（blocks→UI 用户可见；images→模型多模态内联自查）
      const settled = await waitJob(job.id)
      if (!settled || settled.status !== "done") {
        const status = settled?.status ?? "unknown"
        return {
          output: [
            ...startLines,
            `⚠ 等待渲染未成功（状态 ${status}）：${settled?.error ?? "详见作业日志"}`,
            `排查：reel_render action=log job=${job.id}｜查询：action=status job=${job.id}`,
          ].join("\n"),
          data: { jobId: job.id, kind: "still", output: out, composition: compositionId, status },
        }
      }
      return {
        output: [
          ...startLines,
          `静帧已渲染：${out}（帧 ${frame}）—— 帧图已附在本条结果里，直接交给用户看，再用 ask 送审。`,
        ].join("\n"),
        data: { jobId: job.id, kind: "still", output: out, composition: compositionId, status: "done", frame },
        blocks: artifactBlocks(previewLogicalPath(out, ctx)),
        images: [{ path: out, display: out, mime: mimeFor(out) ?? "image/png" }],
      }
    }

    // preview / video
    const isVideo = action === "video"
    const range = parseFrameRange(args.frame_range ? String(args.frame_range) : undefined)
    if (args.quality !== undefined && args.quality !== "final" && args.quality !== "draft") {
      return { output: `quality 只支持 final / draft（收到 ${String(args.quality)}）——draft 是确认动效与节奏用的快速档，不用于交付` }
    }
    const draft = args.quality === "draft"
    if (args.height !== undefined && args.scale !== undefined) {
      return { output: "height 与 scale 二选一：height=<目标高>（按合成长宽比换算，推荐）或 scale=<比例>" }
    }
    // 渲染通道先定（分辨率默认值要按通道给）：参数 > 环境变量 > remotion
    const requestedBackend = args.backend !== undefined ? String(args.backend) : (process.env[BACKEND_ENV] ?? "remotion")
    if (requestedBackend !== "remotion" && requestedBackend !== "canvas" && !isBrowserBackend(requestedBackend)) {
      return { output: `未知 backend：${requestedBackend}（可用：remotion / ${BROWSER_BACKENDS.join(" / ")} / canvas）` }
    }
    if (requestedBackend === "canvas") {
      return {
        output: [
          `backend=canvas 尚未实现：该通道要求把镜头原语改写为 canvas 绘制（现为 DOM/CSS）。`,
          `可选：remotion（默认，保真度最高）/ dom-canvas（不动原语、只换捕获与编码通道）/ record（实时录制，用于交互内容）`,
        ].join("\n"),
      }
    }
    const browserBackend = isBrowserBackend(requestedBackend)
    // 草稿档的低分辨率只在**分辨率真是瓶颈**的通道才有收益——实测：remotion（CDP 截帧，成本随像素线性）
    // 540p 比 1080p 快 1.47×；浏览器通道（抓帧仅 3.2ms/帧）只有 1.04×（噪声内），降分辨率是白丢画质。
    const size = resolveVideoSize({
      width: composition.width,
      height: composition.height,
      scale: resolveOutputScale({
        argScale: typeof args.scale === "number" ? args.scale : undefined,
        draft,
        browserBackend,
        isVideo,
      }),
      targetHeight: typeof args.height === "number" ? args.height : null,
    })
    if ("error" in size) return { output: size.error }
    if (args.x264_preset !== undefined && !asX264Preset(args.x264_preset)) {
      return { output: `x264_preset 非法：${String(args.x264_preset)}（可用：${X264_PRESETS.join(" / ")}）` }
    }
    const x264Preset = asX264Preset(args.x264_preset) ?? (draft ? DRAFT.x264Preset : null)
    const jpegQuality = typeof args.jpeg_quality === "number" ? args.jpeg_quality : draft ? DRAFT.jpegQuality : 82
    // 产物唯一化：同路径重渲改为 `-v2/-v3`（对话里按路径引用产物，历史不得被覆盖）
    const mediaUnique = uniqueOutputPath(resolveOutputPath(projectDir, args.out, join("out", `${compositionId}-${isVideo ? "reel" : "preview"}.mp4`)))
    const out = mediaUnique.path
    const job = createJob({ ctx, kind: isVideo ? "video" : "preview", project: projectDir, composition: compositionId, output: out })
    // 帧段总长：分片规划、浏览器通道记账、进度都要它，故在选路前算好
    const spanStart = range ? Math.max(0, range[0]) : 0
    const spanEnd = !range || range[1] === null ? composition.durationInFrames - 1 : Math.min(range[1], composition.durationInFrames - 1)
    const spanFrames = Math.max(0, spanEnd - spanStart + 1)
    if (isBrowserBackend(requestedBackend)) {
      const evenDim = (n: number): number => Math.max(2, Math.round(n / 2) * 2)
      const bw = evenDim(composition.width * size.scale)
      const bh = evenDim(composition.height * size.scale)
      const backendLines = [
        `已启动${isVideo ? "成片" : "预览"}渲染作业（浏览器通道）：${job.id}`,
        `输出：${out}（${bw}×${bh}）`,
        ...(mediaUnique.renamedFrom ? [`（原路径 ${mediaUnique.renamedFrom} 已存在，已自动改名以免覆盖历史产物）`] : []),
        `通道：${requestedBackend}${args.backend === undefined && process.env[BACKEND_ENV] ? `（来自环境变量 ${BACKEND_ENV}）` : ""}`,
        ...(draft ? [`草稿档：本通道降分辨率无收益（实测 1.04×），已保持全分辨率；快速确认请用 frame_range 缩片段`] : []),
        `合成 ${compositionId}：${composition.width}×${composition.height} · ${composition.fps}fps · ${composition.durationInFrames} 帧${range ? ` · 帧段 ${range[0]}-${range[1] ?? "片尾"}` : " · 全片"}`,
        `音频：音轨单独渲染（Remotion 音频通道出 AAC）后与画面合轨，帧段一致；合成内无音频标签时保持无声并在日志说明`,
        ...(requestedBackend === "record"
          ? ["⚠ record 通道按实时录制：耗时≈片长，时序跟墙钟且不可复现；且录制的帧率握不上合成 fps 时产物会拉长（慢放）——仅用于交互/实时内容"]
          : ["提示：dom-canvas 下 backdrop-filter 与大 blur 会拖慢每帧重栅格化（实测可差 2.5 倍）"]),
      ]
      const stopSampling = startCpuSampling()
      const startedAt = Date.now()
      const sampleCores = effectiveCpuCount()
      startJob(job, ctx, async (log) => {
        try {
          const r = await runBrowserRender({
            ctx,
            backend: requestedBackend,
            serveUrl: prepared.serveUrl,
            composition,
            width: bw,
            height: bh,
            fps: composition.fps,
            frameRange: range,
            output: out,
            browserExecutable: browserState.executablePath,
            // 音轨走 Remotion 的音频通道（能单独出 AAC），与自驱帧的 CDP 浏览器无关
            audio: { libs, browser: prepared.browser, inputProps, composition },
            shouldStop: () => false,
            onProgress: (rendered, total) => {
              job.progress = { stage: "浏览器通道渲染中", renderedFrames: rendered, totalFrames: total, percent: (rendered / total) * 100 }
            },
            log,
          })
          log(
            `通道 ${requestedBackend}：${r.frames} 帧 · 码流 ${r.streamBytes} 字节 · 抓帧均 ${r.captureMsPerFrame.toFixed(1)}ms · 音轨 ${r.audio.ok ? `已合入（${r.audio.bytes} 字节）` : `无（${r.audio.reason}）`}`,
          )
          return `${r.frames} 帧 → ${out}（${r.container} · 浏览器通道 ${requestedBackend}${r.audio.ok ? " · 含音轨" : " · 无声"}）`
        } catch (err) {
          const msg = (err as Error).message
          throw new Error(`${msg}\n回退建议：改传 backend=remotion（DOM + CDP 截帧，保真度最高）`)
        } finally {
          const wallMs = Date.now() - startedAt
          const sample = stopSampling()
          if (sample.source !== "none" && wallMs > 0 && wallMs > 3000 && job.status !== "cancelled") {
            const avg = averageCoresUsed(sample, wallMs)
            if (avg > 0) {
              writeThroughput(ctx, profileKey(projectDir, compositionId), {
                frames: spanFrames,
                wallMs,
                cpuSeconds: sample.cpuSeconds,
                cpuSource: sample.source,
                cores: sampleCores,
                measuredAt: new Date().toISOString(),
              })
            }
          }
        }
      })
      if (args.wait !== true) {
        return {
          output: [...backendLines, `查询进度：reel_render action=status job=${job.id}｜日志：action=log job=${job.id}`].join("\n"),
          data: { jobId: job.id, kind: isVideo ? "video" : "preview", output: out, width: bw, height: bh, backend: requestedBackend, composition: compositionId },
        }
      }
      const settled = await waitJob(job.id, isVideo ? 1_800_000 : 600_000)
      if (!settled || settled.status !== "done") {
        return {
          output: [...backendLines, `⚠ 等待渲染未成功（状态 ${settled?.status ?? "unknown"}）：${settled?.error ?? "详见作业日志"}`, `排查：reel_render action=log job=${job.id}`].join("\n"),
          data: { jobId: job.id, output: out, status: settled?.status ?? "unknown", backend: requestedBackend },
        }
      }
      return {
        output: [...backendLines, `${isVideo ? "成片" : "预览"}已渲染：${out}——产物已附在本条结果里，直接交给用户看，再用 ask 送审。`].join("\n"),
        data: { jobId: job.id, kind: isVideo ? "video" : "preview", output: out, width: bw, height: bh, backend: requestedBackend, status: "done", composition: compositionId },
        blocks: artifactBlocks(previewLogicalPath(out, ctx)),
      }
    }

    // 分片并行：帧段较长时切 K 片、每片一个独立浏览器并行渲染（实测吞吐随片数上升）。
    // 分片该不该切由**上次整片的实测 CPU 占用**决定（而非核数）：CPU 配额已吃满时加片只会更慢。
    const throughputKey = profileKey(projectDir, compositionId)
    const measured = pickThroughput(tuning, throughputKey)
    const shardPlan = planShards({
      totalFrames: spanFrames,
      cpuCount: effectiveCpuCount(),
      override: typeof args.shards === "number" ? args.shards : null,
      measured,
    })
    // 拼接/合轨要 ffmpeg：解析不到就不进分片路径（不能让渲完才发现拼不起来）
    const shardFfmpeg = shardPlan.count > 1 ? resolveShardFfmpeg({ binariesDirectory: binaries.path, roots: [runtimeDir(ctx)] }) : null
    const useShards = shardPlan.count > 1 && shardFfmpeg !== null
    const common = {
      ctx, libs, serveUrl: prepared.serveUrl, composition, output: out,
      frameRange: range, scale: size.scale,
      codec: args.codec ? String(args.codec) : "h264",
      videoBitrate: isVideo && args.video_bitrate ? String(args.video_bitrate) : profile.videoBitrate,
      crf: typeof args.crf === "number" ? args.crf : profile.crf,
      imageFormat: args.image_format ? String(args.image_format) : "jpeg",
      jpegQuality,
      x264Preset,
      profile, browser: prepared.browser, inputProps, job,
      binariesDirectory: binaries.path,
    }
    // 渲染期间采样本容器的 CPU 用量，完成后写回记账：下次的选址判据就是它（实测优先于按核数推断）。
    const stopSampling = startCpuSampling()
    const sampleStartedAt = Date.now()
    const sampleCores = effectiveCpuCount()
    startJob(job, ctx, async (log) => {
      try {
        return await (useShards
          ? runShardedRender({
              ...common,
              shards: shardPlan.count,
              pagesPerShard: shardPlan.pagesPerShard,
              workDir: join(stateDir(ctx), "shards", job.id),
              ffmpeg: shardFfmpeg!,
              browserExecutable: browserExec.path,
              log,
            })
          : runMediaRender({ ...common, log }))
      } finally {
        // 失败也记：它同样反映本机的真实负担（但失败样本可能不完整，故未跑满帧时不入账）
        const wallMs = Date.now() - sampleStartedAt
        const sample = stopSampling()
        // 太短的样本会被浏览器启动等固定开销稀释（不代表稳态占用），故只在帧数足够时入账
        if (sample.source !== "none" && wallMs > 0 && spanFrames >= MIN_FRAMES_PER_SHARD && job.status !== "cancelled") {
          const avg = averageCoresUsed(sample, wallMs)
          if (avg > 0) {
            writeThroughput(ctx, throughputKey, {
              frames: spanFrames,
              wallMs,
              cpuSeconds: sample.cpuSeconds,
              cpuSource: sample.source,
              cores: sampleCores,
              measuredAt: new Date().toISOString(),
            })
          }
        }
      }
    })
    const kind = isVideo ? "video" : "preview"
    const startLines = [
      `已启动${isVideo ? "成片" : "预览"}渲染作业：${job.id}`,
      `输出：${out}（${size.width}×${size.height}${draft ? " · 草稿档" : ""}${x264Preset ? ` · preset ${x264Preset}` : ""}）`,
      // 改名必须明说：否则模型看到路径与请求不符会误以为出错
      ...(mediaUnique.renamedFrom ? [`（原路径 ${mediaUnique.renamedFrom} 已存在，为避免覆盖历史产物自动改名——对话里按路径引用产物，同名覆盖会让旧消息里的产物变成新内容）`] : []),
      `计划档位：${describeProfile(profile, probe.input).join(" · ")}`,
      `分片：${shardPlan.reason}${shardPlan.count > 1 && !shardFfmpeg ? "（未找到 ffmpeg，改为整段渲染）" : ""}`,
      ...(throughputLine(measured) ? [throughputLine(measured) as string] : []),
      `合成 ${compositionId}：${composition.width}×${composition.height} · ${composition.fps}fps · ${composition.durationInFrames} 帧${range ? ` · 帧段 ${range[0]}-${range[1] ?? "片尾"}` : " · 全片"}`,
      browserLine(browserState),
      prepareLine(prepared, entryWarning),
    ]
    if (args.wait !== true) {
      return {
        output: [...startLines, `查询进度：reel_render action=status job=${job.id}｜日志：action=log job=${job.id}`].join("\n"),
        data: { jobId: job.id, kind, output: out, width: size.width, height: size.height, quality: draft ? "draft" : "final", composition: compositionId, profile },
      }
    }
    // 送审片段：等它渲完，把视频产物附进结果（成片时长不受工具超时约束，故给更长的等待窗口）
    const settled = await waitJob(job.id, isVideo ? 600_000 : 180_000)
    if (!settled || settled.status !== "done") {
      const status = settled?.status ?? "unknown"
      return {
        output: [
          ...startLines,
          `⚠ 等待渲染未成功（状态 ${status}）：${settled?.error ?? "详见作业日志"}`,
          `排查：reel_render action=log job=${job.id}｜查询：action=status job=${job.id}`,
        ].join("\n"),
        data: { jobId: job.id, kind, output: out, status },
      }
    }
    return {
      output: [
        ...startLines,
        `${isVideo ? "成片" : "预览"}已渲染：${out}——产物已附在本条结果里，直接交给用户看，再用 ask 送审。`,
      ].join("\n"),
      data: { jobId: job.id, kind, output: out, width: size.width, height: size.height, quality: draft ? "draft" : "final", status: "done", composition: compositionId },
      blocks: artifactBlocks(previewLogicalPath(out, ctx)),
    }
  },
}
