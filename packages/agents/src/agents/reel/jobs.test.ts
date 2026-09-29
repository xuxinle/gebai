/**
 * 作业层单测：帧段/并发上限解析、作业登记-运行-收尾（状态与索引）、日志读写、队列分档（静帧预览并行 2、
 * 成片与实测独占 1）、调优缓存读写、bench 探针与并发实测、并发超上限的自愈重试、取消落到 cancelled。
 * 原生库全部用注入假实现驱动：不联网、不装依赖、不依赖本机 GPU 与 ffmpeg。
 */
import { describe, expect, test } from "bun:test"
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { jobIndexPath, jobLogPath, tuningPath } from "./paths"
import {
  cancelJob,
  createJob,
  defaultBenchCandidates,
  describeJob,
  getJob,
  jobLog,
  listJobs,
  parseConcurrencyLimit,
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
  writeThroughput,
  writeTuning,
  type Job,
  type Tuning,
} from "./jobs"
import { decideProfile, profileKey, type RenderProfile } from "./profile"
import type { ProbeInput } from "./detect"
import type { NativeLibs, VideoConfig } from "./runtime"
import { clearReelEnv, makeCtx } from "./test-ctx"

const COMPOSITION: VideoConfig = { id: "Promo", width: 1080, height: 1920, fps: 30, durationInFrames: 300 }

/** bench 的硬件探测输入（探针失败诊断靠它区分"无 GPU"与"驱动/ffmpeg 缺 NVENC"）。 */
const PROBE_INPUT: ProbeInput = {
  platform: "linux",
  arch: "x64",
  cpuCount: 8,
  memoryMB: 16384,
  nvidia: { name: "RTX 4090", driver: "550.54", memoryMB: 24564 },
  renderNodes: [],
  appleSilicon: false,
  remotionVersion: "4.0.484",
  webglContent: false,
}

const SOFTWARE_PROFILE: RenderProfile = decideProfile({
  platform: "linux",
  arch: "x64",
  cpuCount: 4,
  memoryMB: 8192,
  nvidia: null,
  renderNodes: [],
  appleSilicon: false,
  remotionVersion: "4.0.484",
  webglContent: false,
})

const NVENC_PROFILE: RenderProfile = decideProfile({
  platform: "linux",
  arch: "x64",
  cpuCount: 4,
  memoryMB: 32768,
  nvidia: { name: "NVIDIA GeForce RTX 4090", driver: "550.54", memoryMB: 24564 },
  renderNodes: ["/dev/dri/renderD128"],
  appleSilicon: false,
  remotionVersion: "4.0.484",
  webglContent: false,
})

/** 假原生库：只记录调用参数，不碰真实渲染。 */
function makeLibs(overrides: Partial<NativeLibs> = {}): NativeLibs {
  return {
    rendererDir: "/fake/@remotion/renderer",
    version: "4.0.484",
    bundle: async () => "serve-url",
    getCompositions: async () => [COMPOSITION],
    selectComposition: async () => COMPOSITION,
    renderStill: async () => ({}),
    renderMedia: async () => ({}),
    openBrowser: async () => ({ close: async () => {} }),
    ensureBrowser: async () => ({}),
    makeCancelSignal: () => ({ cancelSignal: {}, cancel: () => {} }),
    ...overrides,
  }
}

function tempHome(): string {
  clearReelEnv()
  return mkdtempSync(join(tmpdir(), "reel-jobs-"))
}

async function waitForStatus(job: Job, statuses: Job["status"][], timeoutMs = 3000): Promise<void> {
  const started = Date.now()
  while (!statuses.includes(job.status)) {
    if (Date.now() - started > timeoutMs) throw new Error(`等待作业状态超时（当前 ${job.status}）`)
    await new Promise((resolve) => setTimeout(resolve, 5))
  }
}

describe("帧段解析", () => {
  test("区间 / 开到片尾 / 单帧 / 带空格", () => {
    expect(parseFrameRange("0-29")).toEqual([0, 29])
    expect(parseFrameRange("300-")).toEqual([300, null])
    expect(parseFrameRange("120")).toEqual([120, 120])
    expect(parseFrameRange(" 5 - 9 ")).toEqual([5, 9])
  })

  test("非法输入返回 null（不抛错）", () => {
    expect(parseFrameRange(undefined)).toBeNull()
    expect(parseFrameRange("")).toBeNull()
    expect(parseFrameRange("abc")).toBeNull()
    expect(parseFrameRange("1.5-3")).toBeNull()
    expect(parseFrameRange("9-5")).toBeNull()
    expect(parseFrameRange("-5")).toBeNull()
  })
})

describe("并发上限解析", () => {
  test("从 Remotion 拒绝信息里取上限", () => {
    expect(parseConcurrencyLimit("Maximum for --concurrency is 4 (number of cores on this system)")).toBe(4)
    expect(parseConcurrencyLimit("Error: Maximum for --concurrency is 16")).toBe(16)
  })

  test("其他错误与非法上限返回 null", () => {
    expect(parseConcurrencyLimit("boom")).toBeNull()
    expect(parseConcurrencyLimit("Maximum for --concurrency is 0")).toBeNull()
  })
})

describe("作业登记、状态与日志", () => {
  test("createJob 登记排队态并写索引；getJob/listJobs 可查、describeJob 可读", () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const job = createJob({ ctx, kind: "still", project: "/p", composition: "Promo", output: "/p/out.png" })
      expect(job.status).toBe("queued")
      expect(job.id.startsWith("still-")).toBe(true)
      expect(job.startedAt).toBeTruthy()
      expect(getJob(job.id)?.id).toBe(job.id)
      expect(getJob("nope")).toBeNull()
      const listed = listJobs(ctx)
      expect(listed.map((j) => j.id)).toContain(job.id)
      expect(describeJob(job)).toContain("queued")
      expect(describeJob(job)).toContain("排队中")
      expect(readFileSync(jobIndexPath(ctx), "utf8").trim().split("\n").length).toBe(1)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("startJob 成功后写 done 与摘要；索引含起止两条", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const job = createJob({ ctx, kind: "video", project: "/p", composition: "Promo" })
      startJob(job, ctx, async (log) => {
        log("开始渲染")
        return "已完成 300 帧"
      })
      await waitForStatus(job, ["done"])
      expect(job.status).toBe("done")
      expect(job.summary).toBe("已完成 300 帧")
      expect(job.endedAt).toBeTruthy()
      expect(describeJob(job)).toContain("已完成 300 帧")
      const index = readFileSync(jobIndexPath(ctx), "utf8").trim().split("\n")
      expect(index.length).toBe(3)
      expect(index[1]).toContain("\"running\"")
      expect(index[2]).toContain("\"done\"")
      expect(readJobLog(ctx, job.id)).toContain("开始渲染")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("startJob 失败写 failed 与错误，不外抛", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const job = createJob({ ctx, kind: "still", project: "/p", composition: "Promo" })
      startJob(job, ctx, async () => {
        throw new Error("合成不存在")
      })
      await waitForStatus(job, ["failed"])
      expect(job.error).toContain("合成不存在")
      expect(describeJob(job)).toContain("failed")
      expect(readJobLog(ctx, job.id)).toContain("作业失败")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("jobLog 按行追加，readJobLog 取尾部 N 行", () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const job = createJob({ ctx, kind: "still", project: "/p", composition: "Promo" })
      for (const line of ["一", "二", "三"]) jobLog(job, ctx, line)
      expect(readJobLog(ctx, job.id, 2).split("\n").length).toBe(2)
      expect(readJobLog(ctx, job.id)).toContain("三")
      expect(readFileSync(jobLogPath(ctx, job.id), "utf8").trim().split("\n").length).toBe(3)
      expect(readJobLog(ctx, "不存在")).toBe("")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("队列分档", () => {
  test("still/preview 并行 2：第 3 个静帧等前者释放", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      let active = 0
      let peak = 0
      const run = async (): Promise<string> => {
        active++
        peak = Math.max(peak, active)
        await new Promise((resolve) => setTimeout(resolve, 25))
        active--
        return "静帧完成"
      }
      const created = (["still", "preview", "still"] as const).map((kind) =>
        createJob({ ctx, kind, project: "/p", composition: "Promo" }),
      )
      for (const job of created) startJob(job, ctx, run)
      for (const job of created) await waitForStatus(job, ["done"])
      expect(peak).toBe(2)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("video/bench 独占 1：两个成片不会同时跑", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      let active = 0
      let peak = 0
      const run = async (): Promise<string> => {
        active++
        peak = Math.max(peak, active)
        await new Promise((resolve) => setTimeout(resolve, 20))
        active--
        return "成片完成"
      }
      const first = createJob({ ctx, kind: "video", project: "/p", composition: "Promo" })
      const second = createJob({ ctx, kind: "bench", project: "/p", composition: "Promo" })
      startJob(first, ctx, run)
      startJob(second, ctx, run)
      await waitForStatus(first, ["done"])
      await waitForStatus(second, ["done"])
      expect(peak).toBe(1)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("排队中的作业可取消：状态置 cancelled 且不进入运行", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      let ran = 0
      const blocker = createJob({ ctx, kind: "video", project: "/p", composition: "Promo" })
      startJob(blocker, ctx, async () => {
        await new Promise((resolve) => setTimeout(resolve, 40))
        return "占位完成"
      })
      const queued = createJob({ ctx, kind: "video", project: "/p", composition: "Promo" })
      startJob(queued, ctx, async () => {
        ran++
        return "不应执行"
      })
      expect(cancelJob(queued.id)).toBe(true)
      await waitForStatus(blocker, ["done"])
      await new Promise((resolve) => setTimeout(resolve, 20))
      expect(queued.status).toBe("cancelled")
      expect(ran).toBe(0)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("调优缓存", () => {
  test("writeTuning/readTuning 往返；pickTuned 命中与未命中；文件缺失返回空", () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      expect(readTuning(ctx)).toEqual({ entries: {}, throughput: {} })
      const tuning: Tuning = {
        encoderProbe: { hardware: true, checkedAt: "2026-01-01T00:00:00.000Z" },
        entries: {
          abc: {
            concurrency: 6,
            gl: "vulkan",
            chromeMode: "chrome-for-testing",
            hardwareAcceleration: "if-possible",
            fps: 33.2,
            measuredAt: "2026-01-01T00:00:00.000Z",
          },
        },
      }
      writeTuning(ctx, tuning)
      const reloaded = readTuning(ctx)
      expect(reloaded.encoderProbe?.hardware).toBe(true)
      expect(pickTuned(reloaded, "abc")?.concurrency).toBe(6)
      expect(pickTuned(reloaded, "missing")).toBeNull()
      expect(JSON.parse(readFileSync(tuningPath(ctx), "utf8")).entries.abc.fps).toBe(33.2)
      // 实测吞吐单独记账：写回后可读回，用作下次分片判据
      writeThroughput(ctx, "abc", { frames: 830, wallMs: 138_000, cpuSeconds: 512, cpuSource: "cgroup", cores: 4, measuredAt: "2026-01-02T00:00:00.000Z" })
      expect(pickThroughput(readTuning(ctx), "abc")?.cpuSeconds).toBe(512)
      expect(pickThroughput(readTuning(ctx), "missing")).toBeNull()
      // 写吞吐不得抹掉已记账的实测调优条目
      expect(pickTuned(readTuning(ctx), "abc")?.concurrency).toBe(6)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("渲染执行", () => {
  test("runStill 直传帧号/格式/缩放/热浏览器/chromeMode，返回人读摘要", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const calls: Array<Record<string, unknown>> = []
      const libs = makeLibs({
        renderStill: async (params) => {
          calls.push({ ...params })
          return {}
        },
      })
      const job = createJob({ ctx, kind: "still", project: "/p", composition: "Promo", output: "/p/f30.png" })
      const summary = await runStill({
        ctx,
        job,
        libs,
        profile: SOFTWARE_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        frame: 30,
        output: "/p/f30.png",
        imageFormat: "jpeg",
        jpegQuality: 70,
        scale: 0.5,
      })
      expect(summary).toContain("静帧已渲染")
      expect(summary).toContain("帧 30")
      expect(calls.length).toBe(1)
      expect(calls[0]!.frame).toBe(30)
      expect(calls[0]!.imageFormat).toBe("jpeg")
      expect(calls[0]!.scale).toBe(0.5)
      expect(calls[0]!.chromeMode).toBe("headless-shell")
      expect(calls[0]!.chromiumOptions).toEqual({})
      expect((calls[0]!.binariesDirectory as unknown) ?? null).toBeNull()
      expect(job.progress?.totalFrames).toBe(1)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("runMediaRender：帧段换算成总帧数、软件档不传 crf、硬件档传 videoBitrate", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const softCalls: Array<Record<string, unknown>> = []
      const softJob = createJob({ ctx, kind: "preview", project: "/p", composition: "Promo", output: "/p/soft.mp4" })
      await runMediaRender({
        ctx,
        job: softJob,
        libs: makeLibs({
          renderMedia: async (params) => {
            softCalls.push({ ...params })
            return {}
          },
        }),
        profile: SOFTWARE_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        output: "/p/soft.mp4",
        frameRange: [10, 39],
      })
      expect(softCalls[0]!.frameRange).toEqual([10, 39])
      expect(softCalls[0]!.hardwareAcceleration).toBeUndefined()
      expect(softCalls[0]!.crf).toBeUndefined()
      expect(softCalls[0]!.videoBitrate).toBeUndefined()
      expect(softJob.progress?.totalFrames).toBe(30)

      const hardCalls: Array<Record<string, unknown>> = []
      const hardJob = createJob({ ctx, kind: "video", project: "/p", composition: "Promo", output: "/p/hard.mp4" })
      await runMediaRender({
        ctx,
        job: hardJob,
        libs: makeLibs({
          renderMedia: async (params) => {
            hardCalls.push({ ...params })
            return {}
          },
        }),
        profile: NVENC_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        output: "/p/hard.mp4",
        frameRange: [0, null],
      })
      expect(hardCalls[0]!.frameRange).toEqual([0, 299])
      expect(hardCalls[0]!.hardwareAcceleration).toBe("if-possible")
      expect(hardCalls[0]!.videoBitrate).toBe("8M")
      expect(hardCalls[0]!.crf).toBeUndefined()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("帧段超出合成时长：给出可操作错误", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const job = createJob({ ctx, kind: "preview", project: "/p", composition: "Promo" })
      await expect(
        runMediaRender({
          ctx,
          job,
          libs: makeLibs(),
          profile: SOFTWARE_PROFILE,
          composition: COMPOSITION,
          serveUrl: "serve-url",
          browser: { close: async () => {} },
          output: "/p/x.mp4",
          frameRange: [400, 420],
        }),
      ).rejects.toThrow(/超出合成/)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("并发超本机上限：按报错上限自愈重试一次", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const calls: Array<Record<string, unknown>> = []
      const libs = makeLibs({
        renderMedia: async (params) => {
          calls.push({ ...params })
          if (calls.length === 1) throw new Error("Maximum for --concurrency is 2 (number of cores on this system)")
          return {}
        },
      })
      const job = createJob({ ctx, kind: "video", project: "/p", composition: "Promo", output: "/p/out.mp4" })
      const summary = await runMediaRender({
        ctx,
        job,
        libs,
        profile: NVENC_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        output: "/p/out.mp4",
        concurrency: 4,
      })
      expect(calls.length).toBe(2)
      expect(calls[0]!.concurrency).toBe(4)
      expect(calls[1]!.concurrency).toBe(2)
      expect(summary).toContain("已渲染 300 帧")
      expect(readJobLog(ctx, job.id)).toContain("超本机上限（2）")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("报错上限不低于当前并发时不重试，原样抛出", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      let calls = 0
      const libs = makeLibs({
        renderMedia: async () => {
          calls++
          throw new Error("Maximum for --concurrency is 8")
        },
      })
      const job = createJob({ ctx, kind: "video", project: "/p", composition: "Promo" })
      await expect(
        runMediaRender({
          ctx,
          job,
          libs,
          profile: SOFTWARE_PROFILE,
          composition: COMPOSITION,
          serveUrl: "serve-url",
          browser: { close: async () => {} },
          output: "/p/out.mp4",
          concurrency: 4,
        }),
      ).rejects.toThrow(/Maximum/)
      expect(calls).toBe(1)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("取消：cancelJob 触发 cancelSignal，作业落 cancelled 状态", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      let cancelCalled = false
      const libs = makeLibs({
        renderMedia: async (params) => await (params.cancelSignal as Promise<never>),
        makeCancelSignal: () => {
          let fire: () => void = () => {}
          const cancelSignal = new Promise<never>((_, reject) => {
            fire = () => reject(new Error("render cancelled"))
          })
          return {
            cancelSignal,
            cancel: () => {
              cancelCalled = true
              fire()
            },
          }
        },
      })
      const job = createJob({ ctx, kind: "video", project: "/p", composition: "Promo" })
      startJob(job, ctx, async () =>
        runMediaRender({
          ctx,
          job,
          libs,
          profile: SOFTWARE_PROFILE,
          composition: COMPOSITION,
          serveUrl: "serve-url",
          browser: { close: async () => {} },
          output: "/p/out.mp4",
        }),
      )
      await waitForStatus(job, ["running"])
      expect(cancelJob(job.id)).toBe(true)
      await waitForStatus(job, ["cancelled"])
      expect(cancelCalled).toBe(true)
      expect(job.error).toContain("cancel")
      expect(readJobLog(ctx, job.id)).toContain("已取消")
      expect(cancelJob("不存在")).toBe(false)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("实测调优 runBench", () => {
  test("required 探针通过 + 并发候选实测 → 写 encoderProbe 与最优条目", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const calls: Array<Record<string, unknown>> = []
      const libs = makeLibs({
        renderMedia: async (params) => {
          calls.push({ ...params })
          const frameRange = params.frameRange as [number, number]
          const slow = params.concurrency === 2 && frameRange[1] !== 1
          await new Promise((resolve) => setTimeout(resolve, slow ? 40 : 5))
          return {}
        },
      })
      const job = createJob({ ctx, kind: "bench", project: "/p", composition: "Promo" })
      const summary = await runBench({
        ctx,
        job,
        libs,
        profile: NVENC_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        projectDir: "/p",
        candidates: [2, 4],
        glCandidates: [],
        frameRange: [0, 29],
        benchDir: join(home, "bench"),
        probeInput: PROBE_INPUT,
      })
      expect(calls.length).toBe(3)
      expect(calls[0]!.hardwareAcceleration).toBe("required")
      expect(calls[0]!.frameRange).toEqual([0, 1])
      expect(summary).toContain("硬件编码探针：通过")
      const tuning = readTuning(ctx)
      expect(tuning.encoderProbe?.hardware).toBe(true)
      const entry = pickTuned(tuning, profileKey("/p", "Promo"))
      expect(entry?.concurrency).toBe(4)
      expect(entry?.gl).toBe(NVENC_PROFILE.gl)
      expect(entry?.chromeMode).toBe(NVENC_PROFILE.chromeMode)
      expect(entry?.hardwareAcceleration).toBe("if-possible")
      expect(entry?.fps).toBeGreaterThan(0)
      expect(summary).toContain("已写入调优缓存")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("required 探针失败：记 error，条目如实落软件档", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const libs = makeLibs({
        renderMedia: async (params) => {
          if (params.hardwareAcceleration === "required") throw new Error("No compatible hardware encoder found")
          return {}
        },
      })
      const job = createJob({ ctx, kind: "bench", project: "/p", composition: "Promo" })
      const summary = await runBench({
        ctx,
        job,
        libs,
        profile: NVENC_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        projectDir: "/p",
        candidates: [4],
        glCandidates: [],
        frameRange: [0, 29],
        benchDir: join(home, "bench"),
        probeInput: PROBE_INPUT,
      })
      const tuning = readTuning(ctx)
      expect(tuning.encoderProbe?.hardware).toBe(false)
      expect(tuning.encoderProbe?.error).toContain("hardware encoder")
      expect(pickTuned(tuning, profileKey("/p", "Promo"))?.hardwareAcceleration).toBe("disable")
      expect(summary).toContain("硬件编码探针：未通过")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("并发候选全部失败：明确报错且不写条目", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const libs = makeLibs({
        renderMedia: async (params) => {
          if (params.hardwareAcceleration === "required") return {}
          throw new Error("渲染失败")
        },
      })
      const job = createJob({ ctx, kind: "bench", project: "/p", composition: "Promo" })
      await expect(
        runBench({
          ctx,
          job,
          libs,
          profile: SOFTWARE_PROFILE,
          composition: COMPOSITION,
          serveUrl: "serve-url",
          browser: { close: async () => {} },
          projectDir: "/p",
          candidates: [4],
          glCandidates: [],
          frameRange: [0, 29],
          benchDir: join(home, "bench"),
        probeInput: PROBE_INPUT,
        }),
      ).rejects.toThrow(/并发实测全部失败/)
      expect(pickTuned(readTuning(ctx), profileKey("/p", "Promo"))).toBeNull()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("未给候选：按有效核数与其一采用默认档（不是空跑）", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const calls: Array<Record<string, unknown>> = []
      const libs = makeLibs({
        renderMedia: async (params) => {
          calls.push({ ...params })
          return {}
        },
      })
      const job = createJob({ ctx, kind: "bench", project: "/p", composition: "Promo" })
      const summary = await runBench({
        ctx,
        job,
        libs,
        profile: SOFTWARE_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        projectDir: "/p",
        candidates: [],
        glCandidates: [],
        frameRange: [0, 29],
        benchDir: join(home, "bench"),
        probeInput: PROBE_INPUT,
      })
      // 探针 1 次 + 默认候选（有效核数与其一半，至少 1 档）
      expect(calls.length).toBeGreaterThanOrEqual(2)
      expect(summary).toContain("已写入调优缓存")
      expect(summary).toContain(tuningPath(ctx))
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("bench 默认并发候选", () => {
  test("有效核数与其一半（去重降序）；非法/极小值兜底 1", () => {
    expect(defaultBenchCandidates(16)).toEqual([16, 8])
    expect(defaultBenchCandidates(4)).toEqual([4, 2])
    expect(defaultBenchCandidates(1)).toEqual([1])
    expect(defaultBenchCandidates(0)).toEqual([1])
    expect(defaultBenchCandidates(Number.NaN)).toEqual([1])
  })
})

describe("bench 光栅化后端实测", () => {
  test("逐候选各开一台浏览器（当前档位复用传入的），取实测最快者写缓存", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const openedChromiumOptions: Array<Record<string, unknown>> = []
      const libs = makeLibs({
        openBrowser: async (opts) => {
          openedChromiumOptions.push({ ...((opts as { chromiumOptions?: Record<string, unknown> }).chromiumOptions ?? {}) })
          return { close: async () => {} }
        },
        renderMedia: async (params) => {
          if (params.hardwareAcceleration === "required") return {}
          const gl = (params.chromiumOptions as Record<string, unknown> | undefined)?.gl
          await new Promise((resolve) => setTimeout(resolve, gl === "angle" ? 1 : 30))
          return {}
        },
      })
      const job = createJob({ ctx, kind: "bench", project: "/p", composition: "Promo" })
      const summary = await runBench({
        ctx,
        job,
        libs,
        profile: SOFTWARE_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        projectDir: "/p",
        candidates: [4],
        glCandidates: [null, "angle"],
        frameRange: [0, 29],
        benchDir: join(home, "bench"),
        probeInput: PROBE_INPUT,
      })
      // 当前档位（SOFTWARE_PROFILE 非 WebGL → gl 为 null）复用传入浏览器，只给 angle 另开一台
      expect(openedChromiumOptions).toEqual([{ gl: "angle" }])
      expect(summary).toContain("光栅化后端实测")
      expect(summary).toContain("采用 angle")
      expect(pickTuned(readTuning(ctx), profileKey("/p", "Promo"))?.gl).toBe("angle")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("某候选启动失败：跳过该档且不阻断其余实测", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const libs = makeLibs({
        openBrowser: async () => {
          throw new Error("no browser")
        },
        renderMedia: async () => ({}),
      })
      const job = createJob({ ctx, kind: "bench", project: "/p", composition: "Promo" })
      const summary = await runBench({
        ctx,
        job,
        libs,
        profile: SOFTWARE_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        projectDir: "/p",
        candidates: [4],
        glCandidates: [null, "angle"],
        frameRange: [0, 29],
        benchDir: join(home, "bench"),
        probeInput: PROBE_INPUT,
      })
      expect(summary).toContain("gl=angle 实测跳过")
      expect(pickTuned(readTuning(ctx), profileKey("/p", "Promo"))?.gl).toBeNull()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("分片并行渲染 runShardedRender", () => {
  interface ShardHarness {
    media: Array<Record<string, unknown>>
    concat: Array<{ segments: string[]; output: string; listPath: string }>
    mux: Array<{ video: string; audio: string; output: string }>
  }

  /** 一套分片编排的完整假件：分片/音轨落盘、拼接与合轨用文件拷贝代替；并记录每台浏览器的启动参数。 */
  function harness(opts: { failVideo?: boolean; failMux?: boolean } = {}): {
    libs: NativeLibs
    tools: NonNullable<Parameters<typeof runShardedRender>[0]["tools"]>
    calls: ShardHarness
    openedBrowsers: Array<Record<string, unknown>>
  } {
    const calls: ShardHarness = { media: [], concat: [], mux: [] }
    const openedBrowsers: Array<Record<string, unknown>> = []
    const libs = makeLibs({
      openBrowser: async (params) => {
        openedBrowsers.push({ ...((params as Record<string, unknown>) ?? {}) })
        return { close: async () => {} }
      },
      renderMedia: async (params) => {
        calls.media.push({ ...params })
        if (params.muted === true && opts.failVideo) throw new Error("浏览器崩了")
        writeFileSync(params.outputLocation as string, "x")
        return {}
      },
    })
    return {
      libs,
      calls,
      openedBrowsers,
      tools: {
        concat: (o) => {
          calls.concat.push(o)
          copyFileSync(o.segments[0]!, o.output)
        },
        mux: (o) => {
          calls.mux.push(o)
          if (opts.failMux) throw new Error("合轨器挂了")
          copyFileSync(o.video, o.output)
        },
      },
    }
  }

  function baseArgs(ctx: ReturnType<typeof makeCtx>["ctx"], home: string, overrides: Record<string, unknown>): Parameters<typeof runShardedRender>[0] {
    const out = join(home, "out.mp4")
    const job = createJob({ ctx, kind: "video", project: "/p", composition: "Promo", output: out })
    return {
      ctx,
      job,
      libs: makeLibs(),
      profile: SOFTWARE_PROFILE,
      composition: COMPOSITION,
      serveUrl: "serve-url",
      browser: { close: async () => {} },
      output: out,
      frameRange: [0, 299],
      shards: 4,
      pagesPerShard: 4,
      workDir: join(home, "shards"),
      ffmpeg: "ffmpeg",
      log: () => {},
      browserExecutable: null,
      ...overrides,
    } as Parameters<typeof runShardedRender>[0]
  }

  test("四片 → 四段无声视频 + 音轨整段一次 + 无损拼接 + 合轨", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const { libs, tools, calls, openedBrowsers } = harness()
      const args = baseArgs(ctx, home, { libs, tools })
      const summary = await runShardedRender(args)

      // 每片一个浏览器 + 音轨一台；且都按 profile 的档位启动（gl/chromeMode 是启动参数，必须与渲染一致）
      expect(openedBrowsers.length).toBe(5)
      expect(openedBrowsers.every((o) => o.chromeMode === SOFTWARE_PROFILE.chromeMode)).toBe(true)

      const videos = calls.media.filter((m) => m.muted === true)
      const audios = calls.media.filter((m) => m.codec === "aac")
      // 帧段等分且首尾相接（闭区间、不重叠）
      expect(videos.map((v) => v.frameRange)).toEqual([[0, 74], [75, 149], [150, 224], [225, 299]])
      expect(audios.length).toBe(1)
      expect(audios[0]!.frameRange).toEqual([0, 299])
      // 分片与音轨都不带音频：音轨只在整段那一次出现
      expect(audios[0]!.muted).toBe(false)
      expect(calls.concat.length).toBe(1)
      expect(calls.concat[0]!.segments.length).toBe(4)
      expect(calls.mux.length).toBe(1)
      expect(existsSync(args.output)).toBe(true)
      expect(summary).toContain("分片并行：4 片")
      expect(summary).toContain("已合回整段音轨")
      // 临时目录无论成败都清理
      expect(existsSync(args.workDir)).toBe(false)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("分片失败：回退整段单浏览器渲染（提速手段不作为新的失败来源）", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const { libs, tools, calls } = harness({ failVideo: true })
      const args = baseArgs(ctx, home, { libs, tools })
      const summary = await runShardedRender(args)

      expect(summary).toContain("回退整段渲染")
      // 回退那次是整段、带声、非分片
      const whole = calls.media.filter(
        (m) => Array.isArray(m.frameRange) && (m.frameRange as number[])[0] === 0 && (m.frameRange as number[])[1] === 299 && m.muted === false && m.codec === "h264",
      )
      expect(whole.length).toBe(1)
      expect(calls.concat.length).toBe(0)
      expect(existsSync(args.workDir)).toBe(false)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("合轨失败：仍交出无声视频并如实说明，不连画面一起丢", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const { libs, tools } = harness({ failMux: true })
      const args = baseArgs(ctx, home, { libs, tools, shards: 2 })
      const summary = await runShardedRender(args)

      expect(summary).toContain("合轨失败，按无声视频交付")
      expect(existsSync(args.output)).toBe(true)
      expect(readFileSync(args.output, "utf8")).toBe("x")
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("原生二进制目录透传", () => {
  test("runStill / runMediaRender 把 binariesDirectory 传给原生库", async () => {
    const home = tempHome()
    try {
      const { ctx } = makeCtx(home, { REEL_LIBRARY_DIR: join(home, "vendor", "reel") })
      const calls: Array<Record<string, unknown>> = []
      const libs = makeLibs({
        renderStill: async (params) => {
          calls.push({ ...params })
          return {}
        },
        renderMedia: async (params) => {
          calls.push({ ...params })
          return {}
        },
      })
      const binariesDirectory = join(home, "bin")

      const stillJob = createJob({ ctx, kind: "still", project: "/p", composition: "Promo", output: "/p/f.png" })
      await runStill({
        ctx,
        job: stillJob,
        libs,
        profile: SOFTWARE_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        output: "/p/f.png",
        frame: 0,
        binariesDirectory,
      })
      expect(calls[0]!.binariesDirectory).toBe(binariesDirectory)

      const mediaJob = createJob({ ctx, kind: "video", project: "/p", composition: "Promo", output: "/p/v.mp4" })
      await runMediaRender({
        ctx,
        job: mediaJob,
        libs,
        profile: SOFTWARE_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        output: "/p/v.mp4",
        binariesDirectory,
      })
      expect(calls[1]!.binariesDirectory).toBe(binariesDirectory)

      // 未配置：显式传 null（Remotion 用项目内 compositor 包）
      const autoJob = createJob({ ctx, kind: "still", project: "/p", composition: "Promo", output: "/p/g.png" })
      await runStill({
        ctx,
        job: autoJob,
        libs,
        profile: SOFTWARE_PROFILE,
        composition: COMPOSITION,
        serveUrl: "serve-url",
        browser: { close: async () => {} },
        output: "/p/g.png",
        frame: 0,
      })
      expect(calls[2]!.binariesDirectory).toBeNull()
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})
