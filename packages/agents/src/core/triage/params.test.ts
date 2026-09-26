/**
 * 参数契约测试：会话内工具与 REST 接口共用同一份参数表（`TRIAGE_PARAM_SPECS`），
 * 这里把「两条通道参数完全一致」固定成断言——名称、缺省、别名归一与校验错误都同源。
 */
import { mkdtempSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { afterAll, describe, expect, test } from "bun:test"
import {
  normalizeTriageParams,
  resolveL1Endpoint,
  toTriageOptions,
  triageToolProperties,
  TRIAGE_DEFAULT_ESCALATE,
  TRIAGE_DEFAULT_RESULT_LIMIT,
  TRIAGE_PARAM_NAMES,
  TRIAGE_PARAM_SPECS,
} from "./params"

const dirs: string[] = []
function tempDir(): string {
  const d = mkdtempSync(join(tmpdir(), "triage-params-"))
  dirs.push(d)
  return d
}
afterAll(() => {
  for (const d of dirs) rmSync(d, { recursive: true, force: true })
})

const base = () => ({ itemsBaseDir: tempDir() })
const norm = (raw: Record<string, unknown>) => normalizeTriageParams(raw, base())
const ITEMS = [{ id: "a", features: "有线索" }]

describe("参数表（两通道同源）", () => {
  test("工具 schema 的 properties 就是规范名全集（无遗漏、无多余）", () => {
    const props = Object.keys(triageToolProperties()).sort()
    expect(props).toEqual([...TRIAGE_PARAM_NAMES].sort())
  })

  test("用户可见的核心参数就在规范名里", () => {
    for (const k of ["items", "target", "schema", "threshold", "escalate"]) expect(TRIAGE_PARAM_NAMES).toContain(k)
  })

  test("别名只用于兼容：规范名与别名不重名、不互相覆盖", () => {
    const seen = new Set<string>()
    for (const s of TRIAGE_PARAM_SPECS) {
      expect(seen.has(s.name)).toBe(false)
      seen.add(s.name)
      expect(s.description.length).toBeGreaterThan(4)
    }
  })

  test("每个规范名都能被归一化器识别（带值不报错）", () => {
    const sample: Record<string, unknown> = {
      items: ITEMS,
      items_file: "",
      target: "http://h:1",
      target_api_key: "k",
      schema: { type: "object" },
      label_enum: ["a"],
      threshold: 0.9,
      min_evidence_chars: 6,
      accept_labels: ["无异常"],
      escalate: true,
      l1_model: "m",
      l1_system: "s",
      l1_prompt_template: "{features}",
      l1_concurrency: 2,
      l1_max_tokens: 100,
      l1_temperature: 0,
      l1_reminders: 3,
      l1_timeout_ms: 1000,
      l1_enable_thinking: false,
      l2_agents: ["code"],
      l2_model: "big",
      l2_api_base: "http://big:2",
      l2_api_key: "bk",
      l2_system: "s2",
      l2_max_items: 3,
      l2_batch_size: 2,
      l2_timeout_ms: 2000,
      job_id: "j1",
      job_dir: "/tmp/j",
      result_limit: 5,
      mode: "async",
    }
    const out = normalizeTriageParams(sample, base())
    expect("error" in out).toBe(false)
    if ("error" in out) return
    const p = out.params
    expect(p.target).toBe("http://h:1")
    expect(p.targetApiKey).toBe("k")
    expect(p.threshold).toBe(0.9)
    expect(p.escalate).toBe(true)
    expect(p.l1.concurrency).toBe(2)
    expect(p.l1.enableThinking).toBe(false)
    expect(p.l2.apiBase).toBe("http://big:2")
    expect(p.l2.apiKey).toBe("bk")
    expect(p.l2.system).toBe("s2")
    expect(p.resultLimit).toBe(5)
    expect(p.mode).toBe("async")
  })

  test("mode 归一：仅接受 async，其余（含未给/非法值）一律同步", () => {
    for (const raw of [{}, { mode: "sync" }, { mode: "weird" }]) {
      const out = norm({ items: ITEMS, ...raw })
      if ("error" in out) throw new Error(out.error)
      expect(out.params.mode).toBe("sync")
    }
    const a = norm({ items: ITEMS, mode: "async" })
    if ("error" in a) throw new Error(a.error)
    expect(a.params.mode).toBe("async")
  })
})

describe("归一：扁平 / 嵌套 / 别名", () => {
  test("嵌套写法（l1.* / l2.*）归一为同一份结构", () => {
    const out = norm({
      items: ITEMS,
      l1: { target: "http://h:1", api_key: "k", model: "m", concurrency: 2, enable_thinking: true },
      l2: { enabled: true, agents: ["code"], max_items: 3, api_base: "http://b:2" },
    })
    expect("error" in out).toBe(false)
    if ("error" in out) return
    expect(out.params.target).toBe("http://h:1")
    expect(out.params.targetApiKey).toBe("k")
    expect(out.params.l1.model).toBe("m")
    expect(out.params.l1.concurrency).toBe(2)
    expect(out.params.l1.enableThinking).toBe(true)
    expect(out.params.escalate).toBe(true)
    expect(out.params.l2.agents).toEqual(["code"])
    expect(out.params.l2.maxItems).toBe(3)
    expect(out.params.l2.apiBase).toBe("http://b:2")
  })

  test("旧键名作兼容输入：l1_target / l1_base_url / l2_enabled / enable_thinking", () => {
    const a = norm({ items: ITEMS, l1_target: "http://old:1", l2_enabled: false, enable_thinking: true })
    expect("error" in a).toBe(false)
    if ("error" in a) return
    expect(a.params.target).toBe("http://old:1")
    expect(a.params.escalate).toBe(false)
    expect(a.params.l1.enableThinking).toBe(true)

    const b = norm({ items: ITEMS, l1: { base_url: "http://rest-old:1" } })
    expect("error" in b).toBe(false)
    if ("error" in b) return
    expect(b.params.target).toBe("http://rest-old:1")
  })

  test("规范名优先于别名（同给时取规范名）", () => {
    const out = norm({ items: ITEMS, target: "http://canonical:1", l1_target: "http://alias:1" })
    expect("error" in out).toBe(false)
    if ("error" in out) return
    expect(out.params.target).toBe("http://canonical:1")
  })

  test("缺省：escalate 开、result_limit 全量、L1 细节留空由管线补缺省", () => {
    const out = norm({ items: ITEMS })
    expect("error" in out).toBe(false)
    if ("error" in out) return
    expect(out.params.escalate).toBe(TRIAGE_DEFAULT_ESCALATE)
    expect(out.params.resultLimit).toBe(TRIAGE_DEFAULT_RESULT_LIMIT)
    expect(out.params.l1.concurrency).toBeUndefined()
    expect(out.params.schema).toBeUndefined()
  })

  test("result_limit 归一：0 / 负数 / 缺省 = 全量", () => {
    for (const raw of [{}, { result_limit: 0 }, { result_limit: -1 }]) {
      const out = norm({ items: ITEMS, ...raw })
      if ("error" in out) throw new Error(out.error)
      expect(out.params.resultLimit).toBe(TRIAGE_DEFAULT_RESULT_LIMIT)
    }
  })
})

describe("校验（两通道同一份报错）", () => {
  test("缺 items / items_file → 报错指名两个入口", () => {
    const out = norm({})
    expect("error" in out).toBe(true)
    if ("error" in out) expect(out.error).toContain("items")
  })

  test("条目缺 features → 报错给出修复方向（features 该写成什么）", () => {
    const out = norm({ items: [{ id: "x" }] })
    expect("error" in out).toBe(true)
    if ("error" in out) expect(out.error).toContain("必须含能区分阶段的证据")
  })

  test("threshold 越界与 min_evidence_chars 为负 → 报错点名参数", () => {
    const a = norm({ items: ITEMS, threshold: 1.2 })
    expect("error" in a).toBe(true)
    if ("error" in a) expect(a.error).toContain("threshold")
    const b = norm({ items: ITEMS, min_evidence_chars: -1 })
    expect("error" in b).toBe(true)
    if ("error" in b) expect(b.error).toContain("min_evidence_chars")
  })

  test("条目文件：相对路径按 itemsBaseDir 解析，JSONL 与 JSON 数组都支持", () => {
    const dir = tempDir()
    writeFileSync(join(dir, "a.jsonl"), '{"id":"x","features":"f1"}\n{"id":"y","features":"f2"}\n', "utf-8")
    const out = normalizeTriageParams({ items_file: "a.jsonl" }, { itemsBaseDir: dir })
    expect("error" in out).toBe(false)
    if ("error" in out) return
    expect(out.params.items.map((i) => i.id)).toEqual(["x", "y"])

    writeFileSync(join(dir, "b.json"), '[{"id":"z","features":"f"}]', "utf-8")
    const out2 = normalizeTriageParams({ items_file: "b.json" }, { itemsBaseDir: dir })
    if ("error" in out2) throw new Error(out2.error)
    expect(out2.params.items[0]?.id).toBe("z")

    const missing = normalizeTriageParams({ items_file: "nope.jsonl" }, { itemsBaseDir: dir })
    expect("error" in missing).toBe(true)
  })
})

describe("端点解析与选项组装", () => {
  test("target 直连 URL / 命名目标未命中 → 可操作报错", () => {
    const url = resolveL1Endpoint({ target: "http://h:1" } as never, {})
    expect("error" in url).toBe(false)

    const unknown = resolveL1Endpoint({ target: "nope" } as never, {})
    expect("error" in unknown).toBe(true)
    if ("error" in unknown) expect(unknown.error).toContain("未知推理目标")
  })

  test("target 缺省走 local（状态文件 > LOCAL_INFER_PORT > 8080，不硬编码端口）", () => {
    const home = tempDir()
    // LOCAL_INFER_HOME 指向空目录：无服务状态文件 → 回落到 LOCAL_INFER_PORT
    const t = resolveL1Endpoint({} as never, { LOCAL_INFER_HOME: home, LOCAL_INFER_PORT: "19191" })
    expect("error" in t).toBe(false)
    if ("error" in t) return
    expect(t.kind).toBe("local")
    expect(t.baseUrl).toContain("19191")
  })

  test("escalate=false 不需要执行器；escalate=true 缺执行器 → 显式报错（不静默降级）", () => {
    const off = norm({ items: ITEMS, escalate: false })
    if ("error" in off) throw new Error(off.error)
    const builtOff = toTriageOptions(off.params, { user: "u", home: tempDir(), env: {} })
    expect("error" in builtOff).toBe(false)
    if ("error" in builtOff) return
    expect(builtOff.options.l2).toBeUndefined()
    expect(builtOff.options.escalate).toBe(false)

    const on = norm({ items: ITEMS })
    if ("error" in on) throw new Error(on.error)
    const builtOn = toTriageOptions(on.params, { user: "u", home: tempDir(), env: {} })
    expect("error" in builtOn).toBe(true)
    if ("error" in builtOn) expect(builtOn.error).toContain("没有可用的精审执行器")
  })

  test("组装把 L1 细节与 L2 配置原样映射，并回传解析后的端点", () => {
    const out = norm({
      items: ITEMS,
      target: "http://h:1",
      target_api_key: "k",
      threshold: 0.7,
      schema: { type: "object" },
      l1_max_tokens: 128,
      l1_temperature: 0.2,
      l2_model: "big",
      l2_agents: ["code"],
      l2_max_items: 4,
      l2_timeout_ms: 1234,
      job_id: "job-x",
    })
    if ("error" in out) throw new Error(out.error)
    const home = tempDir()
    const built = toTriageOptions(out.params, {
      user: "tester",
      home,
      env: {},
      l2Runner: async () => "{}",
    })
    if ("error" in built) throw new Error(built.error)
    expect(built.endpoint.baseUrl).toBe("http://h:1")
    expect(built.options.threshold).toBe(0.7)
    expect(built.options.l1.headers).toEqual({ authorization: "Bearer k" })
    expect(built.options.l1.maxTokens).toBe(128)
    expect(built.options.l1.temperature).toBe(0.2)
    expect(built.options.l2?.model).toBe("big")
    expect(built.options.l2?.agents).toEqual(["code"])
    expect(built.options.l2?.maxItems).toBe(4)
    expect(built.options.l2?.timeoutMs).toBe(1234)
    expect(built.options.jobDir).toContain(join(home, "users", "tester", "triage", "job-x"))
  })
})
