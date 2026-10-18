import { afterAll, beforeAll, describe, expect, test } from "bun:test"
import { mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import type { ToolContext } from "@gebai/sdk"
import { audioDuration, decodeWav, encodeWav } from "../../core/tts/audio"
import { effectTool, mixTool, sfxTool } from "./sfx"
import { def } from "./tts"
import { makeCtx } from "./test-ctx"

let home: string
beforeAll(() => {
  home = mkdtempSync(join(tmpdir(), "gebai-tts-sfx-"))
})
afterAll(() => {
  rmSync(home, { recursive: true, force: true })
})

function newCtx(env: Record<string, string> = {}): ToolContext {
  return makeCtx(home, () => ({}), env).ctx
}

/** 造一个源 WAV（正弦，默认 16kHz）。 */
function makeSource(ctx: ToolContext, rel: string, freq = 440, seconds = 0.4, sampleRate = 16000): string {
  const n = Math.round(seconds * sampleRate)
  const samples = new Float32Array(n)
  for (let i = 0; i < n; i++) samples[i] = 0.4 * Math.sin((2 * Math.PI * freq * i) / sampleRate)
  const abs = ctx.resolvePath(rel)
  writeFileSync(abs, encodeWav({ sampleRate, samples }))
  return rel
}

/** 读回产物并解码（校验真的写成了可解析的 WAV）。 */
async function readProduct(ctx: ToolContext, rel: string) {
  return decodeWav(await ctx.readBinaryFile(ctx.resolvePath(rel)))
}

describe("tts_sfx 工具契约", () => {
  test("list=true：列出全部预设（不合成、不产生块）", async () => {
    const ctx = newCtx()
    const res = await sfxTool.execute({ list: true }, ctx)
    expect(res.output).toContain("预设音效")
    expect(res.output).toContain("ding")
    expect(res.output).toContain("叮咚")
    const data = res.data as { presets: Array<{ name: string; label: string }> }
    expect(data.presets.length).toBeGreaterThanOrEqual(20)
    expect(res.blocks ?? []).toHaveLength(0)
  })

  test("preset 合成：落盘可解码的 WAV + file 块 + data", async () => {
    const ctx = newCtx()
    const res = await sfxTool.execute({ preset: "ding", out: "tmp/tts/sfx-ding.wav" }, ctx)
    expect(res.output).toContain("已生成音效")
    expect(res.output).toContain("预设 ding")
    expect(res.blocks?.[0]).toMatchObject({ type: "file", path: "tmp/tts/sfx-ding.wav", mime: "audio/wav" })
    const data = res.data as { path: string; name: string; durationSec: number; sampleRate: number; bytes: number }
    expect(data.path).toBe("tmp/tts/sfx-ding.wav")
    expect(data.name).toBe("ding")
    expect(data.sampleRate).toBe(44100)
    expect(data.bytes).toBeGreaterThan(1000)
    const audio = await readProduct(ctx, data.path)
    expect(audio).not.toBeNull()
    expect(audioDuration(audio!)).toBeCloseTo(data.durationSec, 2)
  })

  test("中文关键词命中预设；缺省路径落在 tmp/tts 下", async () => {
    const ctx = newCtx()
    const res = await sfxTool.execute({ preset: "叮咚", sampleRate: 16000 }, ctx)
    const data = res.data as { path: string; name: string; sampleRate: number }
    expect(data.name).toBe("ding")
    expect(data.sampleRate).toBe(16000)
    expect(data.path.startsWith("tmp/tts/sfx-")).toBe(true)
    expect(data.path.endsWith(".wav")).toBe(true)
  })

  test("预设未命中：给出清单与自定义提示，不写产物", async () => {
    const ctx = newCtx()
    const res = await sfxTool.execute({ preset: "不存在的音效" }, ctx)
    expect(res.output).toContain("没有找到音效预设")
    expect(res.output).toContain("ding")
    expect(res.blocks ?? []).toHaveLength(0)
  })

  test("自定义单音 + repeat：时长 = 单次 × 次数 + 间隔", async () => {
    const ctx = newCtx()
    const res = await sfxTool.execute(
      { wave: "square", freq: 880, duration: 0.1, repeat: 3, repeatGapMs: 50, sampleRate: 16000, out: "tmp/tts/sfx-rep.wav" },
      ctx,
    )
    expect(res.output).toContain("已重复 3 次")
    const audio = await readProduct(ctx, "tmp/tts/sfx-rep.wav")
    // 0.1 × 3 + 0.05 × 2 = 0.4 秒
    expect(audioDuration(audio!)).toBeCloseTo(0.4, 2)
  })

  test("既无 preset 也无自定义参数：提示用法", async () => {
    const ctx = newCtx()
    const res = await sfxTool.execute({}, ctx)
    expect(res.output).toContain("请给 preset")
    expect(res.blocks ?? []).toHaveLength(0)
  })
})

describe("tts_effect 工具契约", () => {
  test("speed：时长按倍率缩短，applied 如实记录", async () => {
    const ctx = newCtx()
    makeSource(ctx, "tmp/tts/src-a.wav", 440, 0.4)
    const res = await effectTool.execute({ input: "tmp/tts/src-a.wav", speed: 2, out: "tmp/tts/fx-speed.wav" }, ctx)
    expect(res.output).toContain("已处理音频")
    const data = res.data as { applied: string[]; durationSec: number; sourceDurationSec: number }
    expect(data.applied[0]).toContain("变速")
    expect(data.sourceDurationSec).toBeCloseTo(0.4, 2)
    expect(data.durationSec).toBeCloseTo(0.2, 2)
    const audio = await readProduct(ctx, "tmp/tts/fx-speed.wav")
    expect(audioDuration(audio!)).toBeCloseTo(0.2, 2)
  })

  test("多效果组合：按固定顺序生效并全部记入 applied", async () => {
    const ctx = newCtx()
    makeSource(ctx, "tmp/tts/src-b.wav", 440, 0.4)
    const res = await effectTool.execute(
      {
        input: "tmp/tts/src-b.wav",
        pitch: 5,
        lowpass: 3000,
        echoDelayMs: 100,
        reverbMix: 0.2,
        fadeOut: 0.05,
        normalize: true,
        out: "tmp/tts/fx-combo.wav",
      },
      ctx,
    )
    const data = res.data as { applied: string[] }
    expect(data.applied).toHaveLength(6)
    expect(data.applied[0]).toContain("变调")
    expect(data.applied.at(-1)).toContain("归一化")
    const audio = await readProduct(ctx, "tmp/tts/fx-combo.wav")
    expect(audio).not.toBeNull()
  })

  test("sampleRate 指定：产物重采样到目标率（时长不变）", async () => {
    const ctx = newCtx()
    makeSource(ctx, "tmp/tts/src-c.wav", 440, 0.3, 16000)
    const res = await effectTool.execute(
      { input: "tmp/tts/src-c.wav", gainDb: -3, sampleRate: 8000, out: "tmp/tts/fx-rate.wav" },
      ctx,
    )
    expect(res.output).toContain("已处理音频")
    const audio = await readProduct(ctx, "tmp/tts/fx-rate.wav")
    expect(audio?.sampleRate).toBe(8000)
    expect(audioDuration(audio!)).toBeCloseTo(0.3, 1)
  })

  test("缺 input / 源不存在 / 源不是 WAV：各自给出可行动说明", async () => {
    const ctx = newCtx()
    expect((await effectTool.execute({ speed: 2 }, ctx)).output).toContain("请给 input")
    expect((await effectTool.execute({ input: "tmp/tts/nope.wav", speed: 2 }, ctx)).output).toContain("读不到音频文件")
    writeFileSync(ctx.resolvePath("tmp/tts/not-wav.wav"), "这不是音频")
    const res = await effectTool.execute({ input: "tmp/tts/not-wav.wav", speed: 2 }, ctx)
    expect(res.output).toContain("不是可解析的 WAV")
  })

  test("没给任何效果参数：提示可用效果", async () => {
    const ctx = newCtx()
    makeSource(ctx, "tmp/tts/src-d.wav")
    const res = await effectTool.execute({ input: "tmp/tts/src-d.wav" }, ctx)
    expect(res.output).toContain("没有给任何效果参数")
    expect(res.output).toContain("tempo")
    expect(res.blocks ?? []).toHaveLength(0)
  })

  test("tempo：applied 记录变速不变调、时长按倍率缩短、附处理耗时", async () => {
    const ctx = newCtx()
    makeSource(ctx, "tmp/tts/tempo-src.wav", 440, 0.6, 16000)
    const res = await effectTool.execute(
      { input: "tmp/tts/tempo-src.wav", tempo: 1.5, out: "tmp/tts/fx-tempo.wav" },
      ctx,
    )
    const data = res.data as { applied: string[]; durationSec: number; sourceDurationSec: number }
    expect(data.applied).toHaveLength(1)
    expect(data.applied[0]).toContain("变速不变调")
    expect(data.sourceDurationSec).toBeCloseTo(0.6, 2)
    expect(data.durationSec / data.sourceDurationSec).toBeCloseTo(1 / 1.5, 1)
    expect(res.output).toContain("相位声码器处理耗时")
    const audio = await readProduct(ctx, "tmp/tts/fx-tempo.wav")
    expect(audio).not.toBeNull()
    expect(audioDuration(audio!)).toBeCloseTo(data.durationSec, 2)
  })

  test("tempo 与裁剪组合：effect 链顺序为裁剪在前、变速在后", async () => {
    const ctx = newCtx()
    makeSource(ctx, "tmp/tts/tempo-trim.wav", 440, 0.5, 16000)
    const res = await effectTool.execute(
      { input: "tmp/tts/tempo-trim.wav", tempo: 0.8, trimEnd: 0.1, out: "tmp/tts/fx-tempo-trim.wav" },
      ctx,
    )
    expect(res.output).toContain("已处理音频")
    const data = res.data as { applied: string[]; durationSec: number }
    expect(data.applied.map((a) => (a.includes("变速不变调") ? "tempo" : "trim"))).toEqual(["trim", "tempo"])
    // 先裁到 0.4 秒，再按 0.8 倍放慢 → 0.5 秒
    expect(data.durationSec).toBeCloseTo(0.5, 1)
  })
})

describe("tts_mix 工具契约", () => {
  test("mix 模式：提示音排在语音前（delayMs 定位）", async () => {
    const ctx = newCtx()
    const voice = makeSource(ctx, "tmp/tts/voice.wav", 440, 0.4)
    await sfxTool.execute({ preset: "ding", out: "tmp/tts/d.wav" }, ctx)
    const res = await mixTool.execute(
      { tracks: [{ path: "tmp/tts/d.wav" }, { path: voice, delayMs: 900 }], out: "tmp/tts/mixed.wav" },
      ctx,
    )
    expect(res.output).toContain("已完成混音")
    const data = res.data as { mode: string; trackCount: number; durationSec: number }
    expect(data.mode).toBe("mix")
    expect(data.trackCount).toBe(2)
    // 叮咚 0.83 秒，语音从 0.9 秒起 0.4 秒 → 总长 1.3 秒
    expect(data.durationSec).toBeCloseTo(1.3, 1)
  })

  test("sequence 模式：按顺序拼接 + gapMs 段间静音", async () => {
    const ctx = newCtx()
    makeSource(ctx, "tmp/tts/seq-a.wav", 440, 0.3)
    makeSource(ctx, "tmp/tts/seq-b.wav", 660, 0.3)
    const res = await mixTool.execute(
      { tracks: [{ path: "tmp/tts/seq-a.wav" }, { path: "tmp/tts/seq-b.wav" }], mode: "sequence", gapMs: 200, out: "tmp/tts/seq.wav" },
      ctx,
    )
    const data = res.data as { mode: string; durationSec: number }
    expect(data.mode).toBe("sequence")
    expect(data.durationSec).toBeCloseTo(0.8, 2)
    const audio = await readProduct(ctx, "tmp/tts/seq.wav")
    expect(audioDuration(audio!)).toBeCloseTo(0.8, 2)
  })

  test("mix 模式：循环铺底 + durationSec 决定总长", async () => {
    const ctx = newCtx()
    makeSource(ctx, "tmp/tts/pad.wav", 220, 0.1)
    makeSource(ctx, "tmp/tts/v2.wav", 440, 0.4)
    const res = await mixTool.execute(
      {
        tracks: [
          { path: "tmp/tts/pad.wav", loop: true, gain: 0.2 },
          { path: "tmp/tts/v2.wav" },
        ],
        durationSec: 1.2,
        out: "tmp/tts/padded.wav",
      },
      ctx,
    )
    expect((res.data as { durationSec: number }).durationSec).toBeCloseTo(1.2, 2)
  })

  test("全循环且无 durationSec：提示产物只有一周期", async () => {
    const ctx = newCtx()
    makeSource(ctx, "tmp/tts/pad2.wav", 220, 0.2)
    const res = await mixTool.execute({ tracks: [{ path: "tmp/tts/pad2.wav", loop: true }], out: "tmp/tts/loop.wav" }, ctx)
    expect(res.output).toContain("durationSec")
  })

  test("轨数/路径校验：空、缺 path、超上限都被拦下", async () => {
    const ctx = newCtx()
    expect((await mixTool.execute({ tracks: [] }, ctx)).output).toContain("请给 tracks")
    expect((await mixTool.execute({ tracks: [{ delayMs: 100 }] }, ctx)).output).toContain("轨缺少 path")
    const many = Array.from({ length: 17 }, () => ({ path: "tmp/tts/x.wav" }))
    expect((await mixTool.execute({ tracks: many }, ctx)).output).toContain("轨数过多")
  })
})

describe("工具姿态与 def 契约", () => {
  test("新工具写产物：显式 safeMode:false，短名注册", () => {
    for (const tool of [sfxTool, effectTool, mixTool]) {
      expect(tool.safeMode).toBe(false)
      expect(tool.name).not.toContain("tts_")
    }
    expect(sfxTool.name).toBe("sfx")
    expect(effectTool.name).toBe("effect")
    expect(mixTool.name).toBe("mix")
    expect(effectTool.parameters.required).toEqual(["input"])
    expect(mixTool.parameters.required).toEqual(["tracks"])
  })

  test("def 注册全部工具（语音 / 音色 / 神经模型 / 安装 + 音效三件）；提示词说明音效用法", () => {
    // 工具集是**能力边界**：少一个就少一条能力路径，所以逐个点名而不是只数数量。
    expect(Object.keys(def.tools ?? {}).sort()).toEqual([
      "effect",
      "install",
      "mix",
      "models",
      "sfx",
      "speak",
      "voices",
    ])
    expect(def.systemPrompt).toContain("tts_sfx")
    expect(def.systemPrompt).toContain("tts_mix")
    expect(def.description).toContain("音效")
  })
})
