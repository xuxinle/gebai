/**
 * 研判任务配置（`{GEBAI_HOME}/triage/preset.json`）测试。
 *
 * 核心断言两件事：① **优先级**「显式参数 > 预设 > 内置缺省」不可动摇（否则加了预设反而会改掉既有调用方的行为）；
 * ② **配置错误要响**——JSON 写坏、填了不该填的字段、字段值越界都必须报错，不能让调用方以为配上了、实际没生效。
 */
import { afterAll, describe, expect, test } from "bun:test"
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { loadTriagePreset, parseTriagePreset, presetFieldCoverage, resolvePresetPath, TRIAGE_PRESET_FIELDS, TRIAGE_PRESET_REL } from "./preset"
import { normalizeTriageParams, TRIAGE_PARAM_NAMES } from "./params"

const dirs: string[] = []
function tempHome(): string {
  const d = mkdtempSync(join(tmpdir(), "triage-preset-"))
  dirs.push(d)
  return d
}
afterAll(() => {
  for (const d of dirs) rmSync(d, { recursive: true, force: true })
})

/** 在 home 下写一份预设文件（父目录自动建）。 */
function writePreset(home: string, content: unknown, rel = TRIAGE_PRESET_REL): string {
  const p = join(home, rel)
  mkdirSync(join(p, ".."), { recursive: true })
  writeFileSync(p, typeof content === "string" ? content : JSON.stringify(content, null, 2), "utf-8")
  return p
}

const ITEMS = [{ id: "a", features: "命令 exit 1" }]
const norm = (raw: Record<string, unknown>, preset?: Record<string, unknown>) =>
  normalizeTriageParams(raw, { itemsBaseDir: "/tmp", preset })

// ── 从文件载入 ────────────────────────────────────────────────────────────

describe("loadTriagePreset：从文件载入", () => {
  test("文件不存在 → 空预设且不报错（没配就是没配，旧调用方行为不变）", () => {
    const home = tempHome()
    const r = loadTriagePreset({ home })
    expect(r.error).toBeUndefined()
    expect(r.fields).toEqual({})
    expect(r.path).toBe(join(home, TRIAGE_PRESET_REL))
  })

  test("缺省位置就是 {GEBAI_HOME}/triage/preset.json", () => {
    const home = tempHome()
    writePreset(home, { threshold: 0.7 })
    const r = loadTriagePreset({ home })
    expect(r.error).toBeUndefined()
    expect(r.fields.threshold).toBe(0.7)
    expect(r.path?.replaceAll("\\", "/")).toBe(join(home, "triage", "preset.json").replaceAll("\\", "/"))
  })

  test("**每次调用重读**：改完即生效，不必重启服务", () => {
    const home = tempHome()
    writePreset(home, { threshold: 0.7 })
    expect(loadTriagePreset({ home }).fields.threshold).toBe(0.7)
    writePreset(home, { threshold: 0.9 })
    expect(loadTriagePreset({ home }).fields.threshold).toBe(0.9) // 不是进程启动时缓存的那份
  })

  test("可指定别的文件（路径/名字/绝对路径），供多套预设按名选用", () => {
    const home = tempHome()
    writePreset(home, { threshold: 0.5 }, join("triage", "log-check.json"))
    expect(loadTriagePreset({ home, ref: "triage/log-check.json" }).fields.threshold).toBe(0.5)
    expect(loadTriagePreset({ home, ref: "triage/log-check" }).fields.threshold).toBe(0.5)
    const abs = writePreset(home, { threshold: 0.4 }, "other.json")
    expect(loadTriagePreset({ home, ref: abs }).fields.threshold).toBe(0.4) // 绝对路径
    expect(resolvePresetPath("/x/y.json", home)).toBe("/x/y.json")
    // 相对路径一律相对 {GEBAI_HOME}（不相对 cwd——否则换个启动目录就找不到配置）
    expect(resolvePresetPath("triage/log-check", home).replaceAll("\\", "/")).toContain("/triage/log-check.json")
  })

  test("文件读不到/内容写坏 → 报错并带上文件路径（可定位）", () => {
    const home = tempHome()
    const p = writePreset(home, '{"threshold":0.7,}')
    const r = loadTriagePreset({ home })
    expect(r.error).toContain(p)
    expect(r.error).toContain("不是合法 JSON")
  })
})

// ── 解析与校验 ────────────────────────────────────────────────────────────

describe("parseTriagePreset：解析与报错", () => {
  test("未配置 / 空串 → 空预设", () => {
    expect(parseTriagePreset(undefined)).toEqual({ fields: {}, warnings: [] })
    expect(parseTriagePreset("   ")).toEqual({ fields: {}, warnings: [] })
  })

  test("不是对象（数组/字符串）→ 报错", () => {
    expect(parseTriagePreset("[1,2]").error).toContain("必须是 JSON 对象")
    expect(parseTriagePreset("x").error).toContain("不是合法 JSON")
  })

  test("填了调用侧字段 → 报错并说明为什么（不静默忽略）", () => {
    const r = parseTriagePreset({ items: ITEMS })
    expect(r.error).toContain("items")
    expect(r.error).toContain("不放预设")
    expect(parseTriagePreset({ mode: "async" }).error).toContain("REST 通道专属")
  })

  test("未知键 → 忽略但给告警（别让配置错得无声无息）", () => {
    const r = parseTriagePreset({ threshold: 0.7, treshold: 0.5 })
    expect(r.error).toBeUndefined()
    expect(r.fields.threshold).toBe(0.7)
    expect(r.warnings[0]).toContain("treshold")
    expect(r.warnings[0]).toContain("threshold") // 告警里列出可用字段
  })

  test("编排键（name/description/$comment）不参与字段校验（文件要能带说明）", () => {
    const r = parseTriagePreset({ name: "日志研判", description: "生产日志粗筛", $comment: "改完即生效", threshold: 0.7 })
    expect(r.error).toBeUndefined()
    expect(r.warnings).toEqual([])
    expect(r.fields).toEqual({ threshold: 0.7 })
  })

  test("显式 null 视作未设（便于用 null 占位）", () => {
    const r = parseTriagePreset({ l1_system: null, threshold: 0.6 })
    expect(r.error).toBeUndefined()
    expect("l1_system" in r.fields).toBe(false)
    expect(r.fields.threshold).toBe(0.6)
  })

  test("字段覆盖：参数表里的字段要么可配、要么明确拒收（不留灰色地带）", () => {
    expect(presetFieldCoverage().missing).toEqual([])
    for (const f of TRIAGE_PRESET_FIELDS) expect(TRIAGE_PARAM_NAMES).toContain(f)
  })
})

// ── 优先级 ────────────────────────────────────────────────────────────────

describe("预设的优先级：显式参数 > 预设 > 内置缺省", () => {
  const preset = {
    threshold: 0.7,
    label_enum: ["无异常", "网络"],
    l1_system: "预设的系统提示",
    l1_model: "preset-model",
    target: "small",
    escalate: false,
  }

  test("只传 items 就能跑：其余全从预设来（这就是要解决的那件事）", () => {
    const got = norm({ items: ITEMS }, preset)
    if ("error" in got) throw new Error(got.error)
    const p = got.params
    expect(p.threshold).toBe(0.7)
    expect(p.labelEnum).toEqual(["无异常", "网络"])
    expect(p.l1.system).toBe("预设的系统提示")
    expect(p.l1.model).toBe("preset-model")
    expect(p.target).toBe("small")
    expect(p.escalate).toBe(false)
  })

  test("显式参数覆盖预设（同名字段以请求为准）", () => {
    const got = norm({ items: ITEMS, threshold: 0.95, target: "http://x:1", l1_model: "call-model" }, preset)
    if ("error" in got) throw new Error(got.error)
    expect(got.params.threshold).toBe(0.95)
    expect(got.params.target).toBe("http://x:1")
    expect(got.params.l1.model).toBe("call-model")
    expect(got.params.l1.system).toBe("预设的系统提示") // 未覆盖的仍来自预设
  })

  test("别名写法也能覆盖预设（嵌套写法不被预设打乱）", () => {
    const got = norm({ items: ITEMS, l1: { model: "nested-model" } }, preset)
    if ("error" in got) throw new Error(got.error)
    expect(got.params.l1.model).toBe("nested-model")
  })

  test("预设存在时，接口仍能传每一个字段（逐个覆盖，不留死角）", () => {
    // 预设把每个字段都占上（值故意与传入的不同），再用接口参数逐个覆盖——预设的存在不得让任何字段变得"传不进去"
    const full = {
      target: "preset-target",
      target_api_key: "preset-key",
      schema: { type: "object", properties: { x: { type: "string" } }, required: ["x"] },
      label_enum: ["预设标签"],
      threshold: 0.1,
      min_evidence_chars: 1,
      accept_labels: ["预设白名单"],
      escalate: false,
      l1_model: "preset-model",
      l1_system: "预设系统提示",
      l1_prompt_template: "预设模板 {features}",
      l1_concurrency: 1,
      l1_max_tokens: 10,
      l1_temperature: 0.5,
      l1_reminders: 0,
      l1_timeout_ms: 1000,
      l1_retries: 0,
      l1_retry_delay_ms: 1,
      l1_gate: 1,
      l1_enable_thinking: false,
      l2_agents: ["预设agent"],
      l2_model: "preset-l2",
      l2_api_base: "http://preset-l2",
      l2_api_key: "preset-l2-key",
      l2_system: "预设精审提示",
      l2_max_items: 1,
      l2_batch_size: 1,
      l2_timeout_ms: 1000,
    }
    const call = {
      items: ITEMS,
      target: "call-target",
      target_api_key: "call-key",
      schema: { type: "object", properties: { y: { type: "number" } }, required: ["y"] },
      label_enum: ["调用标签"],
      threshold: 0.9,
      min_evidence_chars: 9,
      accept_labels: ["调用白名单"],
      escalate: true,
      l1_model: "call-model",
      l1_system: "调用系统提示",
      l1_prompt_template: "调用模板 {features}",
      l1_concurrency: 8,
      l1_max_tokens: 300,
      l1_temperature: 0,
      l1_reminders: 3,
      l1_timeout_ms: 60000,
      l1_retries: 2,
      l1_retry_delay_ms: 500,
      l1_gate: 16,
      l1_enable_thinking: true,
      l2_agents: ["call-agent"],
      l2_model: "call-l2",
      l2_api_base: "http://call-l2",
      l2_api_key: "call-l2-key",
      l2_system: "调用精审提示",
      l2_max_items: 20,
      l2_batch_size: 5,
      l2_timeout_ms: 600000,
    }
    const got = norm(call, full)
    if ("error" in got) throw new Error(got.error)
    const p = got.params
    expect(p.target).toBe("call-target")
    expect(p.targetApiKey).toBe("call-key")
    expect(p.schema).toEqual(call.schema)
    expect(p.labelEnum).toEqual(["调用标签"])
    expect(p.threshold).toBe(0.9)
    expect(p.minEvidenceChars).toBe(9)
    expect(p.acceptLabels).toEqual(["调用白名单"])
    expect(p.escalate).toBe(true)
    expect(p.l1).toEqual({
      model: "call-model",
      system: "调用系统提示",
      promptTemplate: "调用模板 {features}",
      concurrency: 8,
      maxTokens: 300,
      temperature: 0,
      reminders: 3,
      timeoutMs: 60000,
      retries: 2,
      retryDelayMs: 500,
      gateCap: 16,
      enableThinking: true,
    })
    expect(p.l2).toEqual({
      agents: ["call-agent"],
      model: "call-l2",
      apiBase: "http://call-l2",
      apiKey: "call-l2-key",
      system: "调用精审提示",
      maxItems: 20,
      batchSize: 5,
      timeoutMs: 600000,
    })
  })

  test("预设存在时，接口仍能传 items_file / job_id / mode 等调用侧字段", () => {
    // 预设拒收这些字段，但它们从来都属于调用侧——有预设不妨碍调用侧继续用
    const got = norm({ items: ITEMS, job_id: "j1", result_limit: 5 }, { threshold: 0.7 })
    if ("error" in got) throw new Error(got.error)
    expect(got.params.jobId).toBe("j1")
    expect(got.params.resultLimit).toBe(5)
    expect(got.params.threshold).toBe(0.7)
  })

  test("无预设时一切照旧（内置缺省生效）", () => {
    const got = norm({ items: ITEMS })
    if ("error" in got) throw new Error(got.error)
    expect(got.params.threshold).toBeUndefined() // 缺省值在管线层兜（0.85）
    expect(got.params.escalate).toBe(true)
    expect(got.params.l1.system).toBeUndefined()
  })

  test("预设里的值同样过校验（预设不是绕过校验的后门）", () => {
    const bad = norm({ items: ITEMS }, { threshold: 1.5 })
    expect("error" in bad && bad.error).toContain("threshold 必须在 0~1")
  })

  test("预设里的 schema / 标签 / 证据下限都直接生效", () => {
    const schema = { type: "object", properties: { label: { type: "string" } }, required: ["label"] }
    const got = norm({ items: ITEMS }, { schema, accept_labels: ["无异常"], min_evidence_chars: 12 })
    if ("error" in got) throw new Error(got.error)
    expect(got.params.schema).toEqual(schema)
    expect(got.params.acceptLabels).toEqual(["无异常"])
    expect(got.params.minEvidenceChars).toBe(12)
  })
})
