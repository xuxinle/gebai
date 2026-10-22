/**
 * 裁云的**成片质检工具**：把「渲完抽几帧用眼睛看」换成可复算的判据。
 *
 * ## 为什么这不是一个独立子代理
 *
 * 度量能力属于**剪辑软件自己**——它最清楚每个镜头的边界、每帧的时刻、
 * 音频轨从哪来。做成旁挂的子代理只能事后拿 ffmpeg 解码 mp4 再抽帧，
 * 既不精确（要过一遍有损压缩），也拿不到逐镜区间。
 * 所以：C++ 渲染器在渲染时就把画面量好（`frame_stats.hpp`），
 * 这里负责**读出来 + 与音频侧的 WAV 指标合成一份报告 + 给门禁结论**。
 *
 * ## 音频侧为什么不依赖 ffmpeg
 *
 * `@gebai/agents` 已有一份自研 WAV 编解码（`core/tts/audio.ts`：`decodeWav`，
 * 支持 PCM 8/16/24/32 与 IEEE float，多声道下混）。配音产物本来就是 WAV，
 * 直接解就行——不必为了量一下电平去要求用户装 ffmpeg。
 */

import { readFile } from "node:fs/promises"
import { basename, extname } from "node:path"
import type { Tool, ToolContext } from "@gebai/sdk"
import { schema } from "@gebai/sdk/node"
import { decodeWav } from "../../core/tts/audio"

/** 控制通道 `get` 回来的属性表（只列本工具用到的键）。 */
interface DirectorProps {
  render_quality?: string
  render_quality_json?: string
  render_quality_still_ratio?: string
  render_quality_bright_ratio?: string
  render_quality_mean_luma?: string
  render_quality_change_density?: string
  render_quality_worst_still_clip?: string
  render_quality_static_threshold?: string
  render_quality_partial?: string
  render_stage?: string
  render_error?: string
  render_output?: string
  duration?: string
  size?: string
  fps?: string
  scene_count?: string
}

/** C++ 侧 `quality_to_json` 的形状（与 `frame_stats.cpp` 一一对应）。 */
interface QualityShot {
  clip_id: number
  scene_index: number
  label: string
  t_in: number
  t_out: number
  frames: number
  still_frames: number
  still_ratio: number
  mean_luma: number
}

interface QualityJson {
  static_threshold: number
  bright_luma: number
  probe_width: number
  frames: number
  still_frames: number
  bright_frames: number
  still_ratio: number
  bright_ratio: number
  mean_luma: number
  change_density: number
  worst_still_clip_id: number
  partial: boolean
  summary: string
  shots: QualityShot[]
}

/** 音频侧的一条指标（自带口径，和 C++ 侧同样规矩）。 */
interface AudioMetric {
  name: string
  value: number | null
  unit: string
  definition: string
  extra?: Record<string, unknown>
}

interface Finding {
  metric: string
  value: number | string | null
  reason: string
  severity: "fail" | "warn"
  /** 这个数异常意味着该往哪查——比"不达标"有用得多。 */
  hint?: string
}

/** 判据（缺省值来自实测返工；可在参数里逐项覆盖）。 */
const DEFAULTS = {
  stillRatioMax: 40,
  brightRatioMax: 20,
  changeDensityMin: 0.5,
  consonantRatioMin: 2,
  noiseFloorMax: -50,
  clippingMax: 0.001,
  shotStillRatioMax: 70,
} as const

// ---------------------------------------------------------------- 音频度量

/**
 * 音频指标（口径全部写死在这里，与 C++ 侧一样"定义随数走"）。
 *
 * 判据只保留**能直接行动**的几条：吐字（辅音占比）、底噪（归一化前必看）、
 * 削波（不可逆）、以及段连贯。不做 F0/LUFS/回声——那些要么需要更长的信号
 * （F0 要逐帧自相关）、要么需要全片混音后才有意义，放这里会给出误导读数。
 */
function analyzeAudio(samples: Float32Array, sampleRate: number): AudioMetric[] {
  const out: AudioMetric[] = []
  const n = samples.length
  const dur = n / sampleRate
  out.push({
    name: "duration",
    value: Number(dur.toFixed(4)),
    unit: "s",
    definition: "样本数 / 采样率",
  })

  // —— 电平与削波 ——
  let peak = 0
  let sumSq = 0
  let clipped = 0
  for (let i = 0; i < n; i++) {
    const v = samples[i]
    const a = Math.abs(v)
    if (a > peak) peak = a
    sumSq += v * v
    if (a >= 0.999) clipped++
  }
  const db = (x: number) => (x > 0 ? 20 * Math.log10(x) : -240)
  out.push({
    name: "peak_dbfs",
    value: Number(db(peak).toFixed(3)),
    unit: "dBFS",
    definition: "max(|x|) 相对满刻度；贴 0dBFS 说明后级没有余量",
  })
  out.push({
    name: "clipping_ratio",
    value: Number(((100 * clipped) / Math.max(1, n)).toFixed(6)),
    unit: "%",
    definition:
      "|x|>=0.999 的样本占比 ×100。目标 <0.001%——削掉的波形补不回来，是唯一不可逆的一类",
  })

  // —— 帧级：能量包络 / 有声段 / 底噪 / 频带占比 ——
  const win = Math.max(1, Math.round(sampleRate * 0.025))
  const hop = Math.max(1, Math.round(sampleRate * 0.01))
  const frames = Math.max(0, 1 + Math.floor((n - win) / hop))
  if (frames < 4) {
    out.push({
      name: "consonant_ratio",
      value: null,
      unit: "%",
      definition: "信号短于 4 帧（<~60ms），无法估计频谱指标",
    })
    return out
  }

  const env = new Float64Array(frames)
  // 只对需要的帧做 FFT：2-6kHz / 100-8kHz 能量比
  const fftSize = 1 << Math.ceil(Math.log2(win))
  const bandLo = 2000
  const bandHi = Math.min(6000, sampleRate / 2)
  const totalLo = 100
  const totalHi = Math.min(8000, (sampleRate / 2) * 0.98)
  let bandEnergy = 0
  let totalEnergy = 0

  // 预计算频率索引
  const binHz = sampleRate / fftSize
  const bandFrom = Math.max(0, Math.floor(bandLo / binHz))
  const bandTo = Math.min(fftSize / 2, Math.ceil(bandHi / binHz))
  const totalFrom = Math.max(0, Math.floor(totalLo / binHz))
  const totalTo = Math.min(fftSize / 2, Math.ceil(totalHi / binHz))

  for (let f = 0; f < frames; f++) {
    const off = f * hop
    let acc = 0
    for (let i = 0; i < win; i++) {
      const w = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (win - 1)) // Hann
      const v = samples[off + i] * w
      acc += v * v
    }
    env[f] = Math.sqrt(acc / win)
  }

  // 底噪：最低 20% 分位帧的均值（与 C++ 侧 audio 的口径一致）
  const sorted = Array.from(env).sort((a, b) => a - b)
  const q20 = sorted[Math.floor(sorted.length * 0.2)] ?? 0
  let quietSum = 0
  let quietCount = 0
  for (const v of env) {
    if (v <= q20) {
      quietSum += v
      quietCount++
    }
  }
  const floor = quietCount > 0 ? 20 * Math.log10(Math.max(quietSum / quietCount, 1e-12)) : -240
  out.push({
    name: "noise_floor_db",
    value: Number(floor.toFixed(2)),
    unit: "dBFS",
    definition:
      "帧 RMS(dBFS，25ms 窗/10ms hop) 最低 20% 分位帧的均值。目标 <-50dBFS。"
      + "归一化/增益前必看：静音 -48.9dB 被抬 12.5dB 后 -34.9dB 就听得见了",
  })

  // 频带占比（只统计有声帧：把句前句后静音算进分母会让占比凭空掉一半）
  //
  // **门限必须有回退**：`q20 + 12dB` 只在"有真静音可对比"时成立。若整段都是
  // 稳定有声（合成音、未留头的连续语音），q20 本身就是正常电平，抬 12dB 会高过
  // 所有帧 → **一帧都不算有声** → 辅音占比变成 null（实测：整段 200Hz 正弦得出
  // gate=-5.7dBFS 而帧电平是 -17.7dBFS）。此时回退到绝对门限。
  const gateAbs = -45
  let gate = Math.max(db(q20) + 12, gateAbs)
  let gateFallback: string | null = null
  let activeProbe = 0
  for (let f = 0; f < frames; f++) if (db(env[f]) > gate) activeProbe++
  if (activeProbe === 0) {
    gate = gateAbs
    gateFallback = "no_silence_contrast"
    for (let f = 0; f < frames; f++) if (db(env[f]) > gate) activeProbe++
  }
  if (activeProbe === 0) {
    // 连绝对门限都过不了：退到"中位数以上"（仍是有效信号，只是很轻）
    gate = db(sorted[Math.floor(sorted.length / 2)] ?? 0)
    gateFallback = "below_absolute_gate"
    for (let f = 0; f < frames; f++) if (db(env[f]) > gate) activeProbe++
  }
  let activeFrames = 0
  for (let f = 0; f < frames; f++) {
    if (db(env[f]) <= gate) continue
    activeFrames++
    const off = f * hop
    // 朴素 DFT 只在需要的频带上算（窗长通常 300~1000 点，代价可接受）
    for (let k = totalFrom; k < totalTo; k++) {
      const omega = (2 * Math.PI * k) / fftSize
      let re = 0
      let im = 0
      for (let i = 0; i < win; i++) {
        const w = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (win - 1))
        const v = samples[off + i] * w
        re += v * Math.cos(omega * i)
        im -= v * Math.sin(omega * i)
      }
      const p = re * re + im * im
      totalEnergy += p
      if (k >= bandFrom && k < bandTo) bandEnergy += p
    }
  }
  const ratio = totalEnergy > 0 ? (100 * bandEnergy) / totalEnergy : null
  out.push({
    name: "consonant_ratio",
    value: ratio === null ? null : Number(ratio.toFixed(3)),
    unit: "%",
    definition:
      `有声帧内 ${bandLo}-${Math.round(bandHi)}Hz 能量 / ${totalLo}-${Math.round(totalHi)}Hz 能量 ×100。`
      + "分母只含有声帧（静音不计）；目标 3-8%，<2% 判「吐字糊」。"
      + "该量只在有声段有意义——报数时必须写清分母口径",
    extra: { activeFrames, gateDb: Number(gate.toFixed(1)), gateFallback },
  })

  // 有声段统计（段/秒只在这里给个数，判据在工具侧）
  let segments = 0
  let voicedFrames = 0
  let longest = 0
  let run = 0
  let inRun = false
  for (let f = 0; f < frames; f++) {
    const voiced = db(env[f]) > gate
    if (voiced) {
      voicedFrames++
      run++
      if (!inRun) segments++
      inRun = true
      if (run > longest) longest = run
    } else {
      inRun = false
      run = 0
    }
  }
  const voicedSec = voicedFrames * (hop / sampleRate)
  out.push({
    name: "segment_rate",
    value: voicedSec > 0 ? Number((segments / voicedSec).toFixed(3)) : null,
    unit: "段/秒",
    definition:
      "有声段数 / 有声总时长。目标 <2.0 为连贯。必须与 longest_voiced 同看——"
      + "只看均匀度会把「更碎」误读成「更稳」",
    extra: { segments },
  })
  out.push({
    name: "longest_voiced",
    value: Number((longest * (hop / sampleRate)).toFixed(3)),
    unit: "s",
    definition: "最长连续有声段时长；目标 >0.8s。与 segment_rate 正交：两者只能分别判",
  })
  return out
}

// ---------------------------------------------------------------- 读取属性

async function readProps(ctx: ToolContext, id: string): Promise<DirectorProps> {
  // 优先用已装载的 shuangtian 工具（复用装载门控/审批/ctx 组装，不绕过治理层）
  const registry = (ctx as unknown as { registry?: { has(n: string): boolean; call(n: string, a: unknown): Promise<unknown> } }).registry
  if (!registry?.has("shuangtian_get")) {
    throw new Error(
      "需要 shuangtian 子代理的 get 工具（读取渲染质检属性）。本工具按依赖装载，出现此提示说明依赖未装载",
    )
  }
  const raw = (await registry.call("shuangtian_get", { id })) as { output?: string; data?: unknown }
  const data = raw?.data as { props?: DirectorProps } | undefined
  if (data?.props) return data.props
  // 退路：从 output 的文本里取（某些形态只回文本）
  throw new Error(`无法从 shuangtian_get 取到属性面（${JSON.stringify(raw?.output ?? "").slice(0, 200)}）`)
}

// ---------------------------------------------------------------- 工具

export const verifyTool: Tool = {
  name: "verify",
  safeMode: false,
  description:
    "成片质检：把「渲完抽几帧用眼睛看」换成可复算的判据。读渲染器在渲染时量好的"
    + "**画面质量**（逐镜静止占比 / 过亮帧 / 平均亮度 / 变化密度），并可选地量配音 WAV"
    + "（辅音占比 / 底噪 / 削波 / 段连贯）。返回违规清单，每条带实测值、判据与**该往哪查**。 "
    + "判据可逐项覆盖（默认值来自实测返工）。"
    + "**必须在 render 完成（render_stage=done）后调用**——渲染中读到的是上一轮的数。",
  parameters: schema(
    {
      id: { type: "string", description: "导演控制面元素 id（缺省 director）" },
      audio: {
        type: "string",
        description: "可选：配音/成片 WAV 路径（相对会话工作目录）；给了就一并量音频指标",
      },
      still_ratio_max: { type: "number", description: "全片静止占比上限（%，缺省 40）" },
      bright_ratio_max: { type: "number", description: "过亮帧占比上限（%，缺省 20）" },
      change_density_min: { type: "number", description: "变化密度下限（变化帧/秒，缺省 0.5）" },
      shot_still_ratio_max: {
        type: "number",
        description: "单镜静止占比上限（%，缺省 70）——比全片更严，因为「慢」通常是某一镜",
      },
      consonant_ratio_min: { type: "number", description: "辅音占比下限（%，缺省 2）" },
      include_warnings: { type: "boolean", description: "true = 连警告项一起列出（默认只列失败）" },
    },
    ["id"],
  ),
  async execute(args, ctx) {
    const id = String(args.id ?? "director").trim() || "director"
    const wantWarn = args.include_warnings === true
    const stillMax = num(args.still_ratio_max, DEFAULTS.stillRatioMax)
    const brightMax = num(args.bright_ratio_max, DEFAULTS.brightRatioMax)
    const changeMin = num(args.change_density_min, DEFAULTS.changeDensityMin)
    const shotMax = num(args.shot_still_ratio_max, DEFAULTS.shotStillRatioMax)
    const consonantMin = num(args.consonant_ratio_min, DEFAULTS.consonantRatioMin)

    let props: DirectorProps
    try {
      props = await readProps(ctx, id)
    } catch (err) {
      return { output: `读渲染状态失败：${(err as Error).message}` }
    }

    const stage = props.render_stage ?? "idle"
    if (stage === "failed") {
      return { output: `渲染失败，先修渲染：${props.render_error || "(无错误说明)"}` }
    }
    if (stage !== "done" && stage !== "idle") {
      return {
        output:
          `渲染未结束（render_stage=${stage}）——质检只能在完成后做。`
          + "渲染中读到的是上一轮的数；先在渲染循环里等 done 再调本工具。",
      }
    }

    const findings: Finding[] = []
    const lines: string[] = []

    // —— 画面 ——
    const quality = parseQuality(props)
    if (!quality) {
      lines.push(
        "画面质量读数缺失（render_quality_json 为空）——这一版渲染没有产出质检数据，"
        + "可能是旧二进制或 render 未真正跑过帧阶段。",
      )
    } else {
      const partialNote = quality.partial ? "（局部渲染，结论只覆盖该区间）" : ""
      lines.push(`画面：${quality.frames} 帧${partialNote}`)
      lines.push(`  ${quality.summary}`)
      if (quality.still_ratio > stillMax) {
        findings.push({
          metric: "static_ratio",
          value: quality.still_ratio,
          reason: `> ${stillMax}%`,
          severity: "fail",
          hint:
            "「慢」通常是**镜内大量时间静止**，不是镜头太短。先缩短时长或在落定后加持续微动"
            + "（缓推 1.0→1.025、底光呼吸），不要先减镜头数",
        })
      }
      if (quality.bright_ratio > brightMax) {
        findings.push({
          metric: "bright_frame_ratio",
          value: quality.bright_ratio,
          reason: `> ${brightMax}%`,
          severity: "fail",
          hint: "暗场片里嵌了白底素材？需要多层融合（降亮 + 压色 + 暗角），单纯调透明度不够",
        })
      }
      if (quality.change_density < changeMin) {
        findings.push({
          metric: "change_density",
          value: quality.change_density,
          reason: `< ${changeMin}/s`,
          severity: "warn",
          hint: "变化太少：要么镜内几乎不动，要么切点太稀。与静止占比正交，需分开看",
        })
      }
      // 逐镜：比全片平均更能指出问题在哪
      const worst = worstShot(quality.shots)
      if (worst && worst.still_ratio > shotMax) {
        findings.push({
          metric: `shot_static_ratio:${worst.label}`,
          value: worst.still_ratio,
          reason: `> ${shotMax}%（镜头 ${worst.t_in.toFixed(2)}–${worst.t_out.toFixed(2)}s）`,
          severity: "fail",
          hint: "这一镜拖慢了全片。优先改它，比全片统一加速更有效",
        })
      }
      if (quality.shots.length > 0) {
        lines.push("  逐镜（静止占比 / 亮度）：")
        for (const s of quality.shots) {
          lines.push(
            `    ${s.label.padEnd(12).slice(0, 12)} ${s.t_in.toFixed(2)}–${s.t_out.toFixed(2)}s  `
            + `静止 ${s.still_ratio.toFixed(1).padStart(5)}%  亮度 ${s.mean_luma.toFixed(0).padStart(3)}`
            + `  ${s.frames} 帧`,
          )
        }
      }
    }

    // —— 音频（可选）——
    const audioPath = String(args.audio ?? "").trim()
    if (audioPath) {
      const resolved = ctx.resolvePath(audioPath)
      try {
        const bytes = new Uint8Array(await readFile(resolved))
        const decoded = decodeWav(bytes)
        if (!decoded) {
          findings.push({
            metric: "audio_decode",
            value: null,
            reason: `${basename(audioPath)} 解不出 PCM（只支持 WAV：PCM 8/16/24/32 或 IEEE float）`,
            severity: "fail",
            hint: "用 ffmpeg 转：ffmpeg -i in -c:a pcm_s16le out.wav",
          })
        } else {
          const metrics = analyzeAudio(decoded.samples, decoded.sampleRate)
          const flat = new Map(metrics.map((m) => [m.name, m.value]))
          lines.push(
            `\n音频：${basename(audioPath)}（${decoded.sampleRate}Hz，`
            + `${Number(flat.get("duration") ?? 0).toFixed(2)}s）`,
          )
          for (const m of metrics) {
            lines.push(`  ${m.name.padEnd(18)} ${fmt(m.value).padStart(10)} ${m.unit}`)
          }
          const cons = flat.get("consonant_ratio") ?? null
          if (cons !== null && cons < consonantMin) {
            findings.push({
              metric: "consonant_ratio",
              value: cons,
              reason: `< ${consonantMin}%`,
              severity: "fail",
              hint:
                "吐字糊：2-6kHz 辅音能量不足。先量辅音占比再决定——靠后期 EQ 补是事倍功半，"
                + "除非确认是单一模型问题",
            })
          }
          const nf = flat.get("noise_floor_db") ?? null
          if (nf !== null && nf > DEFAULTS.noiseFloorMax) {
            findings.push({
              metric: "noise_floor_db",
              value: nf,
              reason: `> ${DEFAULTS.noiseFloorMax}dBFS`,
              severity: "fail",
              hint: "底噪会在归一化时被一起抬起来（-48.9dB +12.5dB → -34.9dB 就听得见）。先做门限压缩再归一化",
            })
          }
          const clip = flat.get("clipping_ratio") ?? null
          if (clip !== null && clip > DEFAULTS.clippingMax) {
            findings.push({
              metric: "clipping_ratio",
              value: clip,
              reason: `> ${DEFAULTS.clippingMax}%`,
              severity: "fail",
              hint: "削波不可逆——必须降低上游增益后重新合成，不要试图修波形",
            })
          }
        }
      } catch (err) {
        findings.push({
          metric: "audio_read",
          value: null,
          reason: `读不到 ${basename(audioPath)}：${(err as Error).message}`,
          severity: "fail",
          hint: "确认路径相对会话工作目录，且文件确实存在（0 字节≠合成失败，是没产出）",
        })
      }
    } else {
      lines.push("\n音频：未提供（audio 参数省略）")
    }

    // —— 结论 ——
    const fails = findings.filter((f) => f.severity === "fail")
    const warns = findings.filter((f) => f.severity === "warn")
    const shown = wantWarn ? findings : fails
    lines.push("")
    if (findings.length === 0) {
      lines.push("质检通过：画面与音频指标均在判据内。")
    } else {
      lines.push(`质检结论：${fails.length} 项不达标${wantWarn ? `，${warns.length} 项警告` : ""}`)
      for (const f of shown) {
        lines.push(`  [${f.severity === "fail" ? "FAIL" : "WARN"}] ${f.metric} = ${fmtNum(f.value)}　${f.reason}`)
        if (f.hint) lines.push(`         → ${f.hint}`)
      }
      if (!wantWarn && warns.length > 0) {
        lines.push(`（另有 ${warns.length} 项警告，传 include_warnings=true 查看）`)
      }
    }
    lines.push(
      "\n判据不是禁令：先确认这个数是不是你以为的那个量（每条都带口径），"
      + "再决定改片子还是改阈值；改阈值要在剧本里写明理由。",
    )
    return {
      output: lines.join("\n"),
      data: {
        ok: fails.length === 0,
        stage,
        quality,
        findings,
        // 报告形状稳定：智能体与 e2e 都按它取数
        report: {
          kind: "caiyun-verify/1",
          ok: fails.length === 0,
          thresholds: { stillMax, brightMax, changeMin, shotMax, consonantMin },
          quality,
          findings,
        },
      },
    }
  },
}

// ---------------------------------------------------------------- 小工具

function num(v: unknown, fallback: number): number {
  const n = typeof v === "number" ? v : Number(v)
  return Number.isFinite(n) ? n : fallback
}

function fmt(v: number | null): string {
  if (v === null || !Number.isFinite(v)) return "n/a"
  return Math.abs(v) >= 100 ? v.toFixed(1) : v.toFixed(3)
}

function fmtNum(v: number | string | null): string {
  return typeof v === "number" ? fmt(v) : String(v ?? "n/a")
}

/** 解析 C++ 侧写的 quality_json；解析失败返回 null（不编造数字）。 */
function parseQuality(props: DirectorProps): QualityJson | null {
  const raw = (props.render_quality_json ?? "").trim()
  if (!raw) return null
  try {
    const parsed = JSON.parse(raw) as QualityJson
    if (typeof parsed.still_ratio !== "number") return null
    if (!Array.isArray(parsed.shots)) parsed.shots = []
    return parsed
  } catch {
    return null
  }
}

/** 静止占比最高的那一镜（单帧段不参与比较）。 */
function worstShot(shots: QualityShot[]): QualityShot | null {
  let best: QualityShot | null = null
  for (const s of shots) {
    if (s.frames <= 1) continue
    if (!best || s.still_ratio > best.still_ratio) best = s
  }
  return best
}

/** 供测试直接验证纯函数（不经控制通道）。 */
export const _internal = { analyzeAudio, parseQuality, worstShot, DEFAULTS }

/** 从路径取扩展名（测试用；导出避免未使用告警）。 */
export const _ext = (p: string) => extname(p)
