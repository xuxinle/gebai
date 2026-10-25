/**
 * `caiyun_verify` 的纯函数测试：音频指标、quality 解析、最静镜头选取。
 *
 * ## 为什么测这些而不是测工具整体
 *
 * 工具整体的路径（读控制通道属性）依赖真实渲染，属于 e2e
 * （`shuangtian/tools/caiyun_quality_e2e.py`）。这里钉住的是**纯计算部分**——
 * 它们错了不会崩，只会给出看起来合理的错数，然后有人照它改片子或宣布质检通过。
 *
 * 音频侧尤其要测「分母口径」：辅音占比若把静音算进分母，同一段语音会凭空掉一半，
 * 于是"其实没改好"看起来像"变差了"——这类口径错误在数字上完全看不出来。
 */

import { describe, expect, test } from "bun:test"
import { _internal } from "./verify"

const { analyzeAudio, parseQuality, worstShot, DEFAULTS } = _internal

const SR = 16000

/** 生成一段合成语音：浊音（低频）+ 可选高频辅音段 + 可选静音。 */
function makeAudio(opts: { seconds: number; lowHz: number; highAmp: number; silentHead: number }) {
  const n = Math.round(SR * opts.seconds)
  const head = Math.round(SR * opts.silentHead)
  const out = new Float32Array(n)
  for (let i = 0; i < n; i++) {
    if (i < head) continue
    const t = i / SR
    let v = 0.3 * Math.sin(2 * Math.PI * opts.lowHz * t)
    if (opts.highAmp > 0) v += opts.highAmp * Math.sin(2 * Math.PI * 4000 * t)
    out[i] = v
  }
  return out
}

describe("analyzeAudio", () => {
  test("每条指标都带口径定义（否则那个数字不能用于决策）", () => {
    const metrics = analyzeAudio(makeAudio({ seconds: 0.6, lowHz: 200, highAmp: 0.05, silentHead: 0 }), SR)
    expect(metrics.length).toBeGreaterThan(4)
    for (const m of metrics) {
      expect(m.name).toBeTruthy()
      expect(m.unit).toBeTruthy()
      expect(String(m.definition ?? "").length).toBeGreaterThan(8)
    }
  })

  test("峰值与时长按样本数算准", () => {
    const samples = makeAudio({ seconds: 0.6, lowHz: 200, highAmp: 0, silentHead: 0 })
    const flat = new Map(analyzeAudio(samples, SR).map((m) => [m.name, m.value]))
    expect(flat.get("duration")).toBeCloseTo(0.6, 2)
    // 0.3 幅度 → -10.46 dBFS
    expect(Number(flat.get("peak_dbfs"))).toBeGreaterThan(-12)
    expect(Number(flat.get("peak_dbfs"))).toBeLessThan(-9)
  })

  test("削波为 0（干净）与非 0（过载），且不可逆地单调", () => {
    const clean = new Float32Array(SR).fill(0.5)
    const clipped = new Float32Array(SR).fill(0.5)
    for (let i = 0; i < 2000; i++) clipped[i] = 1.0
    const c0 = new Map(analyzeAudio(clean, SR).map((m) => [m.name, m.value]))
    const c1 = new Map(analyzeAudio(clipped, SR).map((m) => [m.name, m.value]))
    expect(c0.get("clipping_ratio")).toBe(0)
    expect(Number(c1.get("clipping_ratio"))).toBeGreaterThan(0.001)
  })

  test("辅音占比：高频越强值越高（已知方向）", () => {
    const low = analyzeAudio(makeAudio({ seconds: 0.6, lowHz: 200, highAmp: 0.0, silentHead: 0 }), SR)
    const high = analyzeAudio(makeAudio({ seconds: 0.6, lowHz: 200, highAmp: 0.4, silentHead: 0 }), SR)
    const a = Number(new Map(low.map((m) => [m.name, m.value])).get("consonant_ratio") ?? -1)
    const b = Number(new Map(high.map((m) => [m.name, m.value])).get("consonant_ratio") ?? -1)
    expect(a).toBeGreaterThanOrEqual(0)
    expect(b).toBeGreaterThan(a)
    expect(b).toBeGreaterThan(5)
  })

  test("辅音占比的分母只含有声帧：加静音头不改变读数", () => {
    // 这条守着实测踩过的坑：把静音算进分母会让占比凭空掉一半。
    const bare = analyzeAudio(makeAudio({ seconds: 0.6, lowHz: 200, highAmp: 0.3, silentHead: 0 }), SR)
    const padded = analyzeAudio(makeAudio({ seconds: 1.6, lowHz: 200, highAmp: 0.3, silentHead: 0.5 }), SR)
    const a = Number(new Map(bare.map((m) => [m.name, m.value])).get("consonant_ratio") ?? -1)
    const b = Number(new Map(padded.map((m) => [m.name, m.value])).get("consonant_ratio") ?? -1)
    expect(a).toBeGreaterThan(0)
    expect(Math.abs(a - b)).toBeLessThan(1.5)
  })

  test("底噪随噪声升高（更能分开安静与嘶声）", () => {
    const quiet = new Float32Array(SR).fill(0)
    for (let i = 0; i < SR; i++) quiet[i] = 0.2 * Math.sin((2 * Math.PI * 200 * i) / SR)
    const hissy = Float32Array.from(quiet, (v, i) => v + 0.01 * Math.sin(i * 12.9898) * 43758.5453 % 1)
    const a = Number(new Map(analyzeAudio(quiet, SR).map((m) => [m.name, m.value])).get("noise_floor_db"))
    const b = Number(new Map(analyzeAudio(hissy, SR).map((m) => [m.name, m.value])).get("noise_floor_db"))
    expect(b).toBeGreaterThan(a)
  })

  test("信号过短时明说无定义，不编数字", () => {
    const metrics = analyzeAudio(new Float32Array(50), SR)
    const cons = metrics.find((m) => m.name === "consonant_ratio")
    expect(cons).toBeTruthy()
    expect(cons!.value).toBeNull()
    expect(String(cons!.definition)).toContain("无法估计")
  })
})

describe("parseQuality", () => {
  test("正常 JSON 解析出全部字段", () => {
    const q = parseQuality({
      render_quality_json: JSON.stringify({
        static_threshold: 1.2,
        still_ratio: 42.5,
        bright_ratio: 8,
        mean_luma: 96.3,
        change_density: 2.1,
        frames: 100,
        shots: [{ clip_id: 1, label: "a", t_in: 0, t_out: 1, frames: 10, still_ratio: 90, mean_luma: 50 }],
      }),
    })
    expect(q).not.toBeNull()
    expect(q!.still_ratio).toBe(42.5)
    expect(q!.shots.length).toBe(1)
  })

  test("空值/坏 JSON 返回 null（不编造数字）", () => {
    expect(parseQuality({})).toBeNull()
    expect(parseQuality({ render_quality_json: "" })).toBeNull()
    expect(parseQuality({ render_quality_json: "{not json" })).toBeNull()
    // 缺关键字段也算无效
    expect(parseQuality({ render_quality_json: JSON.stringify({ frames: 3 }) })).toBeNull()
  })

  test("shots 缺失时补成空数组（消费者不必做空值判断）", () => {
    const q = parseQuality({ render_quality_json: JSON.stringify({ still_ratio: 0 }) })
    expect(q).not.toBeNull()
    expect(Array.isArray(q!.shots)).toBe(true)
    expect(q!.shots.length).toBe(0)
  })
})

describe("worstShot", () => {
  const mk = (id: number, still: number, frames: number) => ({
    clip_id: id,
    scene_index: 0,
    label: `s${id}`,
    t_in: 0,
    t_out: 1,
    frames,
    still_frames: 0,
    still_ratio: still,
    mean_luma: 0,
  })

  test("取静止占比最高的那一镜", () => {
    expect(worstShot([mk(1, 10, 10), mk(2, 80, 10), mk(3, 30, 10)])!.clip_id).toBe(2)
  })

  test("单帧段不参与比较（单帧无法判动静，报 0 而不是 100）", () => {
    // 若把单帧段算进去，它会被误判为"最静"（本例里 99 > 80）
    expect(worstShot([mk(1, 80, 10), mk(2, 99, 1)])!.clip_id).toBe(1)
  })

  test("全为空/全单帧时返回 null", () => {
    expect(worstShot([])).toBeNull()
    expect(worstShot([mk(1, 100, 1)])).toBeNull()
  })
})

describe("判据默认值", () => {
  test("与提示词里写的数一致（两处不一致会让模型与实际判据打架）", () => {
    expect(DEFAULTS.stillRatioMax).toBe(40)
    expect(DEFAULTS.brightRatioMax).toBe(20)
    expect(DEFAULTS.shotStillRatioMax).toBe(70)
    expect(DEFAULTS.consonantRatioMin).toBe(2)
    expect(DEFAULTS.noiseFloorMax).toBe(-50)
  })
})

describe("readProps：取导演属性面的调用入口", () => {
  // ## 为何要钉这一条
  //
  // 早先 `readProps` 按 `ctx.registry.has(n)` + `ctx.registry.call(n, a)` 调用，
  // 而**真实契约里没有这两个方法**（`ToolContext.registry` 只有
  // `schemas` / `resolve` / `getAgentNames`）。于是工具一调就报
  // `registry?.has is not a function`，整条验片路径完全不可用——
  // 而报错信息看着像"依赖未装载"，把人往装载配置上带。
  //
  // 所以这里钉的不是"能取到值"，而是**走了哪个入口**：只有 `resolve`+`execute`
  // 才是合规调用；写错形状必须当场红，而不是等到跑片时才现形。

  const mkCtx = (tool: { execute: (a: Record<string, unknown>, c: unknown) => Promise<unknown> } | null) =>
    ({
      registry: {
        schemas: () => [],
        getAgentNames: () => [],
        resolve: (name: string) => (tool && name === "shuangtian_get" ? { name, tool } : undefined),
      },
    }) as never

  test("通过 registry.resolve 拿到工具，再用 tool.execute 调用", async () => {
    let calledWith: Record<string, unknown> | null = null
    const ctx = mkCtx({
      execute: async (args) => {
        calledWith = args
        return { output: "ok", data: { props: { render_quality_json: '{"still_ratio":1}' } } }
      },
    })
    const props = await _internal.readProps(ctx, "director")
    expect(calledWith as Record<string, unknown> | null).toEqual({ id: "director" })
    expect(props.render_quality_json).toBe('{"still_ratio":1}')
  })

  test("依赖未装载时给出可操作的提示（而不是 TypeError）", async () => {
    await expect(_internal.readProps(mkCtx(null), "director")).rejects.toThrow(/依赖未装载/)
  })

  test("工具返回但无 props 时如实报错，不编造数字", async () => {
    const ctx = mkCtx({ execute: async () => ({ output: "没有这个属性" }) })
    await expect(_internal.readProps(ctx, "director")).rejects.toThrow(/无法从 shuangtian_get 取到属性面/)
  })
})
