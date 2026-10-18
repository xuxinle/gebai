import { describe, expect, test } from "bun:test"
import {
  SFX_MAX_SECONDS,
  SFX_PRESETS,
  applyEffects,
  audioDuration,
  baseFileName,
  clampNumber,
  concatAudios,
  decodeWav,
  encodeWav,
  findSfxPreset,
  isEmptyEffectSpec,
  limitPeak,
  listSfxPresets,
  mixTracks,
  normalizeSampleRate,
  normalizeWave,
  percentToGain,
  pitchShift,
  resampleAudio,
  safeNamePart,
  silence,
  synthesizeLayers,
  synthesizePreset,
  toSampleRate,
  type AudioBuffer,
  type ToneSpec,
} from "./audio"

/** 正弦测试信号。 */
function tone(freq: number, seconds: number, sampleRate = 44100, gain = 0.5): AudioBuffer {
  const n = Math.round(seconds * sampleRate)
  const samples = new Float32Array(n)
  for (let i = 0; i < n; i++) samples[i] = gain * Math.sin((2 * Math.PI * freq * i) / sampleRate)
  return { sampleRate, samples }
}

/** 峰值。 */
function peak(audio: AudioBuffer): number {
  let max = 0
  for (let i = 0; i < audio.samples.length; i++) max = Math.max(max, Math.abs(audio.samples[i]))
  return max
}

/** 自相关估计主频：取最短周期的相关峰（长滞后是周期的整数倍，同样高，必须选第一个峰）。 */
function estimateFreq(audio: AudioBuffer): number {
  const sr = audio.sampleRate
  const start = Math.round(audio.samples.length * 0.2)
  const avail = audio.samples.length - start - 1
  if (avail < 256) return 0
  const window = Math.max(64, Math.min(Math.round(sr * 0.1), Math.floor(avail / 2)))
  const minLag = Math.max(2, Math.floor(sr / 4000))
  const maxLag = Math.max(minLag + 2, Math.min(window - 1, Math.ceil(sr / 80)))
  const corr = new Float64Array(maxLag + 2)
  for (let lag = minLag; lag <= maxLag; lag++) {
    let sum = 0
    for (let i = 0; i < window; i++) sum += audio.samples[start + i] * audio.samples[start + i + lag]
    corr[lag] = sum
  }
  let best = -Infinity
  let bestLag = minLag
  for (let lag = minLag; lag <= maxLag; lag++) {
    if (corr[lag] > best) {
      best = corr[lag]
      bestLag = lag
    }
  }
  const threshold = best * 0.85
  for (let lag = minLag + 1; lag < maxLag; lag++) {
    if (corr[lag] >= threshold && corr[lag] >= corr[lag - 1] && corr[lag] >= corr[lag + 1]) return sr / lag
  }
  return sr / bestLag
}

/** 手工构造 16bit WAV（用于解码侧用例：立体声、非标准格式）。 */
function handWav(opts: { sampleRate: number; channels: number; bits: number; frames: number[][] }): Uint8Array {
  const { sampleRate, channels, bits, frames } = opts
  const bytesPerSample = bits / 8
  const dataSize = frames.length * channels * bytesPerSample
  const buf = new Uint8Array(44 + dataSize)
  const view = new DataView(buf.buffer)
  const ascii = (at: number, text: string) => {
    for (let i = 0; i < text.length; i++) buf[at + i] = text.charCodeAt(i)
  }
  ascii(0, "RIFF")
  view.setUint32(4, 36 + dataSize, true)
  ascii(8, "WAVE")
  ascii(12, "fmt ")
  view.setUint32(16, 16, true)
  view.setUint16(20, 1, true)
  view.setUint16(22, channels, true)
  view.setUint32(24, sampleRate, true)
  view.setUint32(28, sampleRate * channels * bytesPerSample, true)
  view.setUint16(32, channels * bytesPerSample, true)
  view.setUint16(34, bits, true)
  ascii(36, "data")
  view.setUint32(40, dataSize, true)
  let at = 44
  for (const frame of frames) {
    for (const value of frame) {
      if (bits === 16) view.setInt16(at, value, true)
      else view.setInt8(at, value)
      at += bytesPerSample
    }
  }
  return buf
}

describe("参数归一", () => {
  test("clampNumber：非数字回落、越界钳制", () => {
    expect(clampNumber(5, 0, 10)).toBe(5)
    expect(clampNumber(-3, 0, 10)).toBe(0)
    expect(clampNumber(99, 0, 10)).toBe(10)
    expect(clampNumber("abc", 0, 10, 3)).toBe(3)
    expect(clampNumber(undefined, 0, 10, 3)).toBe(3)
    expect(clampNumber("7", 0, 10)).toBe(7)
  })

  test("normalizeSampleRate：缺省、非法回落、取整与钳制", () => {
    expect(normalizeSampleRate(undefined)).toBe(44100)
    expect(normalizeSampleRate(16000)).toBe(16000)
    expect(normalizeSampleRate(44100.4)).toBe(44100)
    expect(normalizeSampleRate(1)).toBe(8000)
    expect(normalizeSampleRate(999999)).toBe(192000)
    expect(normalizeSampleRate("x")).toBe(44100)
  })

  test("normalizeWave：合法值直取、非法回落 sine", () => {
    expect(normalizeWave("noise")).toBe("noise")
    expect(normalizeWave("SQUARE")).toBe("square")
    expect(normalizeWave("nope")).toBe("sine")
    expect(normalizeWave(undefined)).toBe("sine")
  })

  test("percentToGain：-100 → 0、0 → 1、+100 → 2、越界钳制", () => {
    expect(percentToGain(-100)).toBe(0)
    expect(percentToGain(0)).toBe(1)
    expect(percentToGain(100)).toBeCloseTo(2, 6)
    expect(percentToGain(1000)).toBeCloseTo(2, 6)
    expect(percentToGain(-1000)).toBe(0)
  })
})

describe("音效合成", () => {
  test("预设库完整：非空、每项有说明与层", () => {
    const presets = listSfxPresets()
    expect(presets.length).toBeGreaterThanOrEqual(20)
    for (const { name, label } of presets) {
      expect(SFX_PRESETS[name].layers.length).toBeGreaterThan(0)
      expect(label.length).toBeGreaterThan(0)
    }
    expect(Object.keys(SFX_PRESETS)).toContain("ding")
    expect(Object.keys(SFX_PRESETS)).toContain("explosion")
    // 视频收尾句式 riser → impact → sparkle 所需的三件套必须齐备（视频子代理直接按名取用）
    expect(Object.keys(SFX_PRESETS)).toContain("riser")
    expect(Object.keys(SFX_PRESETS)).toContain("impact")
    expect(Object.keys(SFX_PRESETS)).toContain("sparkle")
  })

  test("findSfxPreset：精确名 / 名称包含 / 中文说明 / 未命中", () => {
    expect(findSfxPreset("ding")?.name).toBe("ding")
    expect(findSfxPreset("DING")?.name).toBe("ding")
    expect(findSfxPreset("叮咚")?.name).toBe("ding")
    expect(findSfxPreset("whoosh")?.name).toBe("whoosh")
    expect(findSfxPreset("不存在的音效")).toBeNull()
    expect(findSfxPreset("")).toBeNull()
  })

  test("synthesizePreset：时长与采样率、峰值不越界", () => {
    const found = synthesizePreset("ding")
    expect(found?.name).toBe("ding")
    const audio = found!.audio
    expect(audio.sampleRate).toBe(44100)
    // ding = 0.13 + 0.7 秒
    expect(audioDuration(audio)).toBeCloseTo(0.83, 2)
    expect(peak(audio)).toBeLessThanOrEqual(0.98)
  })

  test("合成确定性：同参数两次输出逐样本一致", () => {
    const a = synthesizePreset("explosion")!.audio
    const b = synthesizePreset("explosion")!.audio
    expect(a.samples.length).toBe(b.samples.length)
    for (let i = 0; i < a.samples.length; i += 97) expect(a.samples[i]).toBe(b.samples[i])
  })

  test("自定义单音：时长/频率/衰减生效", () => {
    const audio = synthesizeLayers([{ wave: "sine", freq: 440, duration: 0.25, gain: 0.5 }], { sampleRate: 16000 })
    expect(audio.sampleRate).toBe(16000)
    expect(audioDuration(audio)).toBeCloseTo(0.25, 2)
    expect(estimateFreq(audio)).toBeCloseTo(440, -1)
    // decay 让尾部明显小于峰值
    const tail = Math.abs(audio.samples[audio.samples.length - 2])
    expect(tail).toBeLessThan(peak(audio) * 0.5)
  })

  test("极短时长被抬到可合成下限，超长时长被钳制", () => {
    const tiny = synthesizeLayers([{ duration: 0.0001 }])
    expect(audioDuration(tiny)).toBeGreaterThan(0)
    expect(audioDuration(tiny)).toBeLessThan(0.02)
    const long = synthesizeLayers([{ wave: "noise", freq: 1000, duration: 9999, decay: 50 }], { sampleRate: 8000 })
    expect(audioDuration(long)).toBeLessThanOrEqual(SFX_MAX_SECONDS + 0.01)
  })

  test("volume 百分比影响幅度：-100 静音、+100 更响", () => {
    const layers: ToneSpec[] = [{ wave: "sine", freq: 440, duration: 0.1, gain: 0.3 }]
    const muted = synthesizeLayers(layers, { volumePercent: -100 })
    expect(peak(muted)).toBe(0)
    const normal = synthesizeLayers(layers)
    const loud = synthesizeLayers(layers, { volumePercent: 100 })
    expect(peak(loud)).toBeGreaterThan(peak(normal))
  })

  test("叠加超峰时限制到 ceiling 而非削波", () => {
    const hot: AudioBuffer = {
      sampleRate: 8000,
      samples: new Float32Array([0.9, -1.4, 2.2, -0.3]),
    }
    const limited = limitPeak(hot)
    expect(peak(limited)).toBeLessThanOrEqual(0.98 + 1e-6)
    // 等比缩放，波形形状保持
    expect(limited.samples[1] / limited.samples[0]).toBeCloseTo(-1.4 / 0.9, 4)
  })

  test("静音生成：长度与采样率", () => {
    const s = silence(0.25, 16000)
    expect(s.samples.length).toBe(4000)
    expect(peak(s)).toBe(0)
  })
})

describe("WAV 编解码", () => {
  test("往返：采样率与长度保持、样本在量化误差内一致", () => {
    const src = tone(440, 0.05)
    const decoded = decodeWav(encodeWav(src))
    expect(decoded).not.toBeNull()
    expect(decoded!.sampleRate).toBe(src.sampleRate)
    expect(decoded!.samples.length).toBe(src.samples.length)
    let maxErr = 0
    for (let i = 0; i < src.samples.length; i++) maxErr = Math.max(maxErr, Math.abs(decoded!.samples[i] - src.samples[i]))
    expect(maxErr).toBeLessThan(1 / 32000)
  })

  test("头部字段：RIFF/WAVE/fmt/data 与长度字段", () => {
    const bytes = encodeWav(tone(440, 0.01))
    const ascii = (at: number) => String.fromCharCode(bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3])
    expect(ascii(0)).toBe("RIFF")
    expect(ascii(8)).toBe("WAVE")
    expect(ascii(12)).toBe("fmt ")
    expect(ascii(36)).toBe("data")
    const view = new DataView(bytes.buffer)
    expect(view.getUint32(4, true)).toBe(bytes.byteLength - 8)
    expect(view.getUint32(40, true)).toBe(bytes.byteLength - 44)
    expect(view.getUint16(22, true)).toBe(1)
    expect(view.getUint16(34, true)).toBe(16)
  })

  test("非 WAV 数据 / 过短数据 / 截断 data 返回 null", () => {
    expect(decodeWav(new Uint8Array(10))).toBeNull()
    expect(decodeWav(new TextEncoder().encode("这不是音频".repeat(20)))).toBeNull()
    const truncated = encodeWav(tone(440, 0.05)).slice(0, 60)
    expect(decodeWav(truncated)).toBeNull()
  })

  test("立体声下混取平均、8bit 无符号按偏移解码", () => {
    const stereo = handWav({
      sampleRate: 8000,
      channels: 2,
      bits: 16,
      frames: [
        [16384, -16384],
        [32767, -32768],
      ],
    })
    const decoded = decodeWav(stereo)
    expect(decoded?.sampleRate).toBe(8000)
    expect(decoded!.samples.length).toBe(2)
    expect(decoded!.samples[0]).toBeCloseTo(0, 3)
    expect(decoded!.samples[1]).toBeCloseTo(0, 3)

    const eight = handWav({ sampleRate: 8000, channels: 1, bits: 8, frames: [[192], [64]] })
    const decoded8 = decodeWav(eight)
    expect(decoded8!.samples[0]).toBeCloseTo(0.5, 2)
    expect(decoded8!.samples[1]).toBeCloseTo(-0.5, 2)
  })
})

describe("采样率与重采样", () => {
  test("resampleAudio：factor 2 长度减半，factor 1 原样返回", () => {
    const src = tone(440, 0.2)
    expect(resampleAudio(src, 1)).toBe(src)
    const fast = resampleAudio(src, 2)
    expect(fast.samples.length).toBe(Math.floor(src.samples.length / 2))
    expect(fast.sampleRate).toBe(src.sampleRate)
  })

  test("toSampleRate：16k → 44.1k 长度放大、采样率标注更新", () => {
    const src = tone(440, 0.2, 16000)
    const up = toSampleRate(src, 44100)
    expect(up.sampleRate).toBe(44100)
    expect(up.samples.length).toBeCloseTo(src.samples.length * (44100 / 16000), -2)
    expect(toSampleRate(src, 16000)).toBe(src)
  })

  test("变速：factor 2 使频率翻倍、时长减半", () => {
    const src = tone(300, 0.4)
    const out = resampleAudio(src, 2)
    expect(audioDuration(out)).toBeCloseTo(0.2, 2)
    expect(estimateFreq(out)).toBeCloseTo(600, -2)
  })
})

describe("变调（OLA）与效果链", () => {
  test("pitchShift：+12 半音频率翻倍而时长基本不变", () => {
    const src = tone(440, 0.5, 22050)
    const up = pitchShift(src, 12)
    expect(audioDuration(up)).toBeCloseTo(0.5, 1)
    expect(Math.abs(estimateFreq(up) - 880) / 880).toBeLessThan(0.06)
    expect(peak(up)).toBeGreaterThan(0.05)
  })

  test("pitchShift：0 半音原样返回、越界钳制不崩", () => {
    const src = tone(440, 0.1)
    expect(pitchShift(src, 0)).toBe(src)
    expect(audioDuration(pitchShift(src, 99))).toBeGreaterThan(0)
    expect(audioDuration(pitchShift(src, -99))).toBeGreaterThan(0)
  })

  test("applyEffects：增益/归一化/淡入/反转/裁剪生效且如实记录", () => {
    const src = tone(440, 0.5, 44100, 0.5)
    const quieter = applyEffects(src, { gainDb: -6 })
    expect(peak(quieter.audio)).toBeCloseTo(0.5 * 0.5012, 2)
    expect(quieter.applied.length).toBe(1)

    const normalized = applyEffects(src, { normalize: true })
    expect(peak(normalized.audio)).toBeCloseTo(0.891, 2)

    const faded = applyEffects(src, { fadeIn: 0.1, fadeOut: 0.1 })
    expect(Math.abs(faded.audio.samples[0])).toBeLessThan(0.01)
    expect(Math.abs(faded.audio.samples[faded.audio.samples.length - 1])).toBeLessThan(0.01)
    expect(peak(faded.audio)).toBeGreaterThan(0.4)

    const reversed = applyEffects(src, { reverse: true })
    expect(reversed.audio.samples[0]).toBeCloseTo(src.samples[src.samples.length - 1], 5)

    const trimmed = applyEffects(src, { trimStart: 0.1, trimEnd: 0.1 })
    expect(audioDuration(trimmed.audio)).toBeCloseTo(0.3, 2)
  })

  test("applyEffects：滤波/调制/回声/混响产出有效音频（无 NaN 且幅度受限）", () => {
    const src = tone(440, 0.3, 44100, 0.4)
    const out = applyEffects(src, {
      lowpass: 3000,
      highpass: 100,
      robot: 60,
      echo: { delayMs: 120, feedback: 0.4, mix: 0.4 },
      reverb: { size: 1, mix: 0.3 },
      fadeOut: 0.05,
    })
    expect(out.applied.length).toBe(6)
    expect(out.audio.samples.length).toBe(src.samples.length)
    for (let i = 0; i < out.audio.samples.length; i += 31) {
      expect(Number.isFinite(out.audio.samples[i])).toBe(true)
      expect(Math.abs(out.audio.samples[i])).toBeLessThan(2)
    }
  })

  test("isEmptyEffectSpec：全空为 true，任一给出为 false", () => {
    expect(isEmptyEffectSpec({})).toBe(true)
    expect(isEmptyEffectSpec({ normalize: false })).toBe(true)
    expect(isEmptyEffectSpec({ gainDb: 0 })).toBe(false)
    expect(isEmptyEffectSpec({ tempo: 1.5 })).toBe(false)
    expect(isEmptyEffectSpec({ echo: { mix: 0.2 } })).toBe(false)
  })
})

describe("混音与拼接", () => {
  test("concatAudios：长度 = 各段之和 + 段间静音（首段 gap 忽略）", () => {
    const a = tone(440, 0.1)
    const b = tone(660, 0.2)
    const joined = concatAudios([{ audio: a, gapSec: 0.5 }, { audio: b, gapSec: 0.05 }], { sampleRate: 16000 })
    expect(joined.sampleRate).toBe(16000)
    expect(audioDuration(joined)).toBeCloseTo(0.35, 2)
  })

  test("mixTracks：同起点叠加、延迟定位、采样率统一", () => {
    const a = tone(440, 0.1, 16000, 0.4)
    const b = tone(660, 0.1, 44100, 0.4)
    const stacked = mixTracks([{ audio: a }, { audio: b }])
    expect(stacked.sampleRate).toBe(44100)
    expect(audioDuration(stacked)).toBeCloseTo(0.1, 2)
    expect(peak(stacked)).toBeGreaterThan(0.7)

    const delayed = mixTracks([{ audio: a }, { audio: b, delaySec: 0.2 }])
    expect(audioDuration(delayed)).toBeCloseTo(0.3, 2)
  })

  test("mixTracks：循环铺底按 totalSec 铺满、非循环轨不被截断", () => {
    const pad = tone(220, 0.1, 16000, 0.2)
    const voice = tone(440, 0.5, 16000, 0.4)
    const withPad = mixTracks([{ audio: pad, loop: true, gain: 0.2 }, { audio: voice, delaySec: 0.1 }], { totalSec: 1 })
    expect(audioDuration(withPad)).toBeCloseTo(1, 2)

    const noTruncate = mixTracks([{ audio: voice }], { totalSec: 0.2 })
    expect(audioDuration(noTruncate)).toBeCloseTo(0.5, 2)
  })

  test("mixTracks：全循环且无 totalSec 时退化为一个周期；空轨返回短静音", () => {
    const pad = tone(220, 0.2, 16000)
    const oneCycle = mixTracks([{ audio: pad, loop: true }])
    expect(audioDuration(oneCycle)).toBeCloseTo(0.2, 2)
    expect(audioDuration(mixTracks([]))).toBeCloseTo(0.1, 2)
  })
})

describe("名称辅助", () => {
  test("baseFileName：逻辑路径与绝对路径取末段", () => {
    expect(baseFileName("tmp/tts/a.wav")).toBe("a.wav")
    expect(baseFileName("C:\\x\\y\\b.wav")).toBe("b.wav")
    expect(baseFileName("")).toBe("audio.wav")
  })

  test("safeNamePart：清洗非法字符、超长截断、空回落", () => {
    expect(safeNamePart("ding", "sfx")).toBe("ding")
    expect(safeNamePart("叮咚 1!", "sfx")).toBe("1")
    expect(safeNamePart("a".repeat(40), "sfx")).toHaveLength(24)
    expect(safeNamePart("", "sfx")).toBe("sfx")
    expect(safeNamePart(undefined, "sfx")).toBe("sfx")
  })
})
