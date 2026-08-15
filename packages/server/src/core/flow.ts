/**
 * flow 数据流编排引擎（DESIGN「flow 数据流编排工具」）：
 * - 步骤（工具步骤 / 分组步骤）按声明顺序执行，`when` 条件分支、`foreach`/`while` 循环、`input` 显式映射；
 * - 引用表达式 `{{步骤id.data.xxx}}` 在 params 中插值（单引用保类型），`when`/`while`/`foreach` 用裸表达式；
 * - 工具步骤执行结果记录 { output, data }，供后续步骤引用（结构化 data 不进模型上下文，仅编排消费）。
 * 表达式求值器为纯函数（无 eval/Function），可独立单测。
 */
import type { ToolContext, ToolResult } from "./types"
import { isRiskyToolName, safeModeRestrictionMsg } from "./safety"

/** 单次 flow 执行的工具调用总数上限（含循环迭代展开）。 */
export const FLOW_MAX_STEPS = 100
/** foreach 展开上限：数组长度 / 计数不得超过。 */
export const FLOW_FOREACH_MAX = 50
/** while 循环默认轮数上限（maxLoops 可调，硬上限同值）。 */
export const FLOW_WHILE_DEFAULT_MAX = 10
export const FLOW_WHILE_HARD_MAX = 50
/** 分组嵌套深度上限。 */
export const FLOW_MAX_DEPTH = 4
/** 报告中单步输出保留字符数（完整内容仍在 data / 工具自身截断文件中）。 */
export const FLOW_REPORT_STEP_CHARS = 2000
/** 报告中单轮（循环迭代）输出保留字符数。 */
export const FLOW_REPORT_ROUND_CHARS = 500

/** 工具步骤：执行单个工具调用。 */
export interface FlowToolStep {
  id?: string
  tool: string
  params?: Record<string, unknown>
  /** 显式映射（参数名 → 表达式/模板字符串）：解析结果覆盖 params 同名字段，并抑制自动注入。 */
  input?: Record<string, unknown>
  /** 条件分支：裸表达式，求值为假时跳过本步（status=skipped，不影响 prev 自动注入）。 */
  when?: string
  /** 容错：true 时本步失败不中断管道（status=error，data={error}，继续后续步骤）。 */
  optional?: boolean
}

/** 分组步骤：循环体（foreach 数据驱动 / while 条件驱动）内含子步骤数组。 */
export interface FlowGroupStep {
  id?: string
  /** 数据驱动循环：表达式求值为数组（逐项执行，`item`/`index` 可引用）或正整数（按次数执行）。 */
  foreach?: string
  /** 条件驱动循环：每轮执行前求值（含首轮，为假则整组跳过），为真执行一轮子步骤后重新求值。 */
  while?: string
  /** while 轮数上限（默认 10，硬上限 50）。 */
  maxLoops?: number
  when?: string
  steps: FlowStep[]
}

export type FlowStep = FlowToolStep | FlowGroupStep

function isGroupStep(s: FlowStep): s is FlowGroupStep {
  return "steps" in s && Array.isArray((s as FlowGroupStep).steps)
}

/** 步骤执行结果（表达式根对象：`s1.data.path` / `s1.output` / `s1.status` / `s1.runs`）。 */
export interface FlowStepResult {
  id: string
  tool?: string
  kind: "tool" | "group"
  status: "done" | "skipped" | "blocked" | "error"
  output: string
  data?: unknown
  /** 分组：实际执行轮数。 */
  runs?: number
}

// ---------------------------------------------------------------------------
// 表达式：词法 + 递归下降解析 + 求值
// ---------------------------------------------------------------------------

type Token =
  | { t: "num"; v: number }
  | { t: "str"; v: string }
  | { t: "id"; v: string }
  | { t: "op"; v: string }

const OPS = ["==", "!=", ">=", "<=", "&&", "||", ">", "<", "!", "(", ")", "[", "]", ".", ","]

function tokenize(src: string): Token[] {
  const tokens: Token[] = []
  let i = 0
  while (i < src.length) {
    const c = src[i]
    if (/\s/.test(c)) {
      i++
      continue
    }
    if (c === "'" || c === '"') {
      let j = i + 1
      let s = ""
      while (j < src.length && src[j] !== c) {
        if (src[j] === "\\" && j + 1 < src.length) {
          s += src[j + 1]
          j += 2
        } else {
          s += src[j]
          j++
        }
      }
      if (j >= src.length) throw new Error(`表达式字符串未闭合: ${src}`)
      tokens.push({ t: "str", v: s })
      i = j + 1
      continue
    }
    if (/[0-9]/.test(c)) {
      let j = i
      while (j < src.length && /[0-9.]/.test(src[j])) j++
      const n = Number(src.slice(i, j))
      if (!Number.isFinite(n)) throw new Error(`表达式非法数字: ${src.slice(i, j)}`)
      tokens.push({ t: "num", v: n })
      i = j
      continue
    }
    if (/[A-Za-z_]/.test(c)) {
      let j = i
      while (j < src.length && /[A-Za-z0-9_]/.test(src[j])) j++
      tokens.push({ t: "id", v: src.slice(i, j) })
      i = j
      continue
    }
    const op = OPS.find((o) => src.startsWith(o, i))
    if (!op) throw new Error(`表达式含非法字符 "${c}": ${src}`)
    tokens.push({ t: "op", v: op })
    i += op.length
  }
  return tokens
}

type PathSeg = { k: string } | { i: number }

export type ExprNode =
  | { t: "lit"; v: unknown }
  | { t: "ref"; root: string; path: PathSeg[] }
  | { t: "un"; a: ExprNode }
  | { t: "bin"; op: string; a: ExprNode; b: ExprNode }
  | { t: "call"; name: string; args: ExprNode[] }

class ExprParser {
  private pos = 0
  constructor(private tokens: Token[]) {}
  private peek(): Token | undefined {
    return this.tokens[this.pos]
  }
  private eat(op: string): boolean {
    const t = this.peek()
    if (t && t.t === "op" && t.v === op) {
      this.pos++
      return true
    }
    return false
  }
  private expect(op: string): void {
    if (!this.eat(op)) throw new Error(`表达式缺少 "${op}"（位置 ${this.pos}）`)
  }
  parse(): ExprNode {
    const node = this.parseOr()
    if (this.pos !== this.tokens.length) throw new Error(`表达式存在多余内容（位置 ${this.pos}）`)
    return node
  }
  private parseOr(): ExprNode {
    let a = this.parseAnd()
    while (this.eat("||")) a = { t: "bin", op: "||", a, b: this.parseAnd() }
    return a
  }
  private parseAnd(): ExprNode {
    let a = this.parseNot()
    while (this.eat("&&")) a = { t: "bin", op: "&&", a, b: this.parseNot() }
    return a
  }
  private parseNot(): ExprNode {
    if (this.eat("!")) return { t: "un", a: this.parseNot() }
    return this.parseCmp()
  }
  private parseCmp(): ExprNode {
    const a = this.parsePrim()
    const t = this.peek()
    if (t && t.t === "op" && ["==", "!=", ">", "<", ">=", "<="].includes(t.v)) {
      this.pos++
      return { t: "bin", op: t.v, a, b: this.parsePrim() }
    }
    return a
  }
  private parsePrim(): ExprNode {
    if (this.eat("(")) {
      const node = this.parseOr()
      this.expect(")")
      return this.parsePath(node)
    }
    const t = this.peek()
    if (!t) throw new Error("表达式意外结束")
    if (t.t === "num" || t.t === "str") {
      this.pos++
      return { t: "lit", v: t.v }
    }
    if (t.t === "id") {
      this.pos++
      if (t.v === "true") return { t: "lit", v: true }
      if (t.v === "false") return { t: "lit", v: false }
      if (t.v === "null") return { t: "lit", v: null }
      if (this.eat("(")) {
        const args: ExprNode[] = []
        if (!this.eat(")")) {
          do {
            args.push(this.parseOr())
          } while (this.eat(","))
          this.expect(")")
        }
        return { t: "call", name: t.v, args }
      }
      return this.parsePath({ t: "ref", root: t.v, path: [] })
    }
    throw new Error(`表达式意外符号 "${(t as { v: string }).v}"`)
  }
  /** 引用后路径访问：`.field` / `[0]`（括号子表达式同样支持路径，便于 len(...) 等直接对结果取字段）。 */
  private parsePath(node: ExprNode): ExprNode {
    for (;;) {
      if (this.eat(".")) {
        const k = this.peek()
        if (!k || k.t !== "id") throw new Error("路径访问缺少字段名")
        this.pos++
        node = { t: "bin", op: "[]", a: node, b: { t: "lit", v: k.v } }
      } else if (this.eat("[")) {
        const idx = this.parseOr()
        this.expect("]")
        node = { t: "bin", op: "[]", a: node, b: idx }
      } else return node
    }
  }
}

export function parseExpr(src: string): ExprNode {
  return new ExprParser(tokenize(src)).parse()
}

/** 表达式可见变量：步骤结果（按 id）+ prev/item/index/iteration/input。 */
export type ExprScope = Record<string, unknown>

function evalNode(node: ExprNode, scope: ExprScope): unknown {
  switch (node.t) {
    case "lit":
      return node.v
    case "ref":
      return pathAccess(scope[node.root], node.path.map((p) => ("k" in p ? p.k : p.i)))
    case "un":
      return !truthy(evalNode(node.a, scope))
    case "bin": {
      if (node.op === "&&") return truthy(evalNode(node.a, scope)) ? evalNode(node.b, scope) : false
      if (node.op === "||") {
        const a = evalNode(node.a, scope)
        return truthy(a) ? a : evalNode(node.b, scope)
      }
      if (node.op === "[]") {
        const a = evalNode(node.a, scope)
        const b = evalNode(node.b, scope)
        return pathAccess(a, [b])
      }
      const a = evalNode(node.a, scope)
      const b = evalNode(node.b, scope)
      return compare(node.op, a, b)
    }
    case "call": {
      const args = node.args.map((x) => evalNode(x, scope))
      if (node.name === "len") {
        const v = args[0]
        if (v == null) return 0
        if (Array.isArray(v) || typeof v === "string") return v.length
        if (typeof v === "object") return Object.keys(v).length
        return 0
      }
      if (node.name === "contains") {
        const [a, b] = args
        if (Array.isArray(a)) return a.some((x) => looseEq(x, b))
        if (typeof a === "string") return typeof b === "string" ? a.includes(b) : a.includes(String(b ?? ""))
        return false
      }
      if (node.name === "exists") return args[0] !== undefined && args[0] !== null
      throw new Error(`未知函数: ${node.name}（可用 len/contains/exists）`)
    }
  }
}

/** 路径访问：对象按字段名，数组/字符串的 length 取长度，数组按下标；越界/缺失返回 undefined。 */
function pathAccess(target: unknown, segs: unknown[]): unknown {
  let cur = target
  for (const seg of segs) {
    if (cur == null) return undefined
    if (Array.isArray(cur)) {
      if (String(seg) === "length") {
        cur = cur.length
        continue
      }
      const i = Number(seg)
      cur = Number.isInteger(i) ? cur[i] : undefined
    } else if (typeof cur === "object") {
      cur = (cur as Record<string, unknown>)[String(seg)]
    } else if (typeof cur === "string" && String(seg) === "length") {
      cur = cur.length
    } else {
      return undefined
    }
  }
  return cur
}

/** 真值判定：JS 语义 + 空数组视为假（`when: "s1.data.items"` 判「有内容」更直观）。 */
function truthy(v: unknown): boolean {
  if (Array.isArray(v)) return v.length > 0
  return Boolean(v)
}

/** 宽松相等：null/undefined 等价；数字与数字字符串按数值比；对象/数组按 JSON 文本比；其余按类型严格比。 */
function looseEq(a: unknown, b: unknown): boolean {
  const an = a == null ? null : a
  const bn = b == null ? null : b
  if (an === null && bn === null) return true
  if (an === null || bn === null) return false
  if (typeof an === "object" || typeof bn === "object") return JSON.stringify(an) === JSON.stringify(bn)
  if (typeof an === typeof bn) return an === bn
  const na = Number(an)
  const nb = Number(bn)
  if (an !== "" && bn !== "" && Number.isFinite(na) && Number.isFinite(nb)) return na === nb
  return String(an) === String(bn)
}

function compare(op: string, a: unknown, b: unknown): boolean {
  if (op === "==") return looseEq(a, b)
  if (op === "!=") return !looseEq(a, b)
  if (a == null || b == null) return false
  let x: number | string
  let y: number | string
  if (typeof a === "string" && typeof b === "string") {
    x = a
    y = b
  } else {
    x = Number(a)
    y = Number(b)
    if (!Number.isFinite(x) || !Number.isFinite(y)) return false
  }
  if (op === ">") return x > y
  if (op === "<") return x < y
  if (op === ">=") return x >= y
  return x <= y
}

/** 求值裸表达式（when/while/foreach）。求值异常原样抛出（步骤级错误信息）。 */
export function evalExpr(src: string, scope: ExprScope): unknown {
  return evalNode(parseExpr(src), scope)
}

/** 条件求值（when/while/foreach）：兼容裸表达式与整值 `{{表达式}}` 包裹两种写法。 */
function evalCond(src: string, scope: ExprScope): unknown {
  const m = src.trim().match(/^\{\{([^{}]+)\}\}$/)
  return evalExpr(m ? m[1].trim() : src.trim(), scope)
}

// ---------------------------------------------------------------------------
// 模板插值（params/input 值）：整值 `{{expr}}` 保类型，混排字符串按文本拼接
// ---------------------------------------------------------------------------

function stringifyValue(v: unknown): string {
  if (v == null) return ""
  if (typeof v === "string") return v
  if (typeof v === "object") return JSON.stringify(v)
  return String(v)
}

export function resolveTemplate(value: unknown, scope: ExprScope): unknown {
  if (typeof value === "string") {
    const full = value.match(/^\{\{([^{}]+)\}\}$/)
    if (full) return evalExpr(full[1].trim(), scope)
    if (value.includes("{{")) {
      return value.replace(/\{\{([^{}]+)\}\}/g, (_, src) => stringifyValue(evalExpr(String(src).trim(), scope)))
    }
    return value
  }
  if (Array.isArray(value)) return value.map((v) => resolveTemplate(v, scope))
  if (value && typeof value === "object") {
    const out: Record<string, unknown> = {}
    for (const [k, v] of Object.entries(value as Record<string, unknown>)) out[k] = resolveTemplate(v, scope)
    return out
  }
  return value
}

/** 尝试解析文本为 JSON（自动注入的 JSON 字段映射用）。解析失败返回 null。 */
function tryParseJson(text: string): unknown {
  const trimmed = text.trim()
  if (!trimmed.startsWith("{") && !trimmed.startsWith("[")) return null
  try {
    return JSON.parse(trimmed)
  } catch {
    return null
  }
}

// ---------------------------------------------------------------------------
// 步骤执行器
// ---------------------------------------------------------------------------

interface FlowState {
  ctx: ToolContext
  results: Map<string, FlowStepResult>
  order: FlowStepResult[]
  prev?: FlowStepResult
  executed: number
}

const ID_RE = /^[A-Za-z_][A-Za-z0-9_]*$/
const RESERVED_ROOTS = new Set(["prev", "item", "index", "iteration", "input"])

/** 校验并规范化 steps 参数：结构校验 + 为无 id 步骤按声明顺序分配自动 id（s1/s2…，跳过与显式 id 冲突的编号），
 *  返回的每个步骤均带唯一 id。类型/结构错误抛带定位信息异常。 */
export function normalizeSteps(raw: unknown): FlowStep[] {
  if (!Array.isArray(raw)) throw new Error(`flow: steps 必须为数组（得到 ${typeof raw}）`)
  if (!raw.length) throw new Error("flow: steps 不能为空")
  const explicit = new Set<string>()
  const validate = (list: unknown[], depth: number): void => {
    list.forEach((s, i) => {
      if (!s || typeof s !== "object") throw new Error(`flow: steps[${i}] 必须为对象`)
      const step = s as Record<string, unknown>
      if (step.id != null) {
        const id = String(step.id)
        if (!ID_RE.test(id)) throw new Error(`flow: 步骤 id 非法: ${id}（须为标识符）`)
        if (RESERVED_ROOTS.has(id)) throw new Error(`flow: 步骤 id 与保留名冲突: ${id}`)
        if (explicit.has(id)) throw new Error(`flow: 步骤 id 重复: ${id}`)
        explicit.add(id)
      }
      if (Array.isArray(step.steps)) {
        if (depth >= FLOW_MAX_DEPTH) throw new Error(`flow: 分组嵌套深度超限（>${FLOW_MAX_DEPTH}）`)
        if (step.foreach == null && step.while == null) {
          throw new Error(`flow: 分组步骤（steps 数组）必须声明 foreach 或 while 之一（步骤 ${String(step.id ?? i)}）`)
        }
        validate(step.steps, depth + 1)
      } else if (typeof step.tool !== "string" || !step.tool) {
        throw new Error(`flow: steps[${i}] 缺少 tool 字段`)
      }
    })
  }
  validate(raw, 0)
  let counter = 0
  const nextAuto = (): string => {
    for (;;) {
      counter++
      const id = `s${counter}`
      if (!explicit.has(id)) return id
    }
  }
  const fill = (list: unknown[], depth: number): FlowStep[] =>
    (list as Array<Record<string, unknown>>).map((s) => {
      if (Array.isArray(s.steps)) return { ...s, id: s.id ?? nextAuto(), steps: fill(s.steps, depth + 1) } as FlowGroupStep
      return { ...s, id: s.id ?? nextAuto() } as FlowToolStep
    })
  return fill(raw, 0)
}

function capReport(text: string, limit: number): string {
  return text.length > limit ? `${text.slice(0, limit)}\n…（已截断，完整内容见本步 data/截断文件）` : text
}

const STATUS_LABEL: Record<FlowStepResult["status"], string> = { done: "✓", skipped: "跳过", blocked: "受限", error: "失败" }

/** 执行一组步骤（顶层或循环体）：每步结果写入 scope（按 id 可引用）并更新 prev。返回本组内实际执行的末步结果。 */
async function runStepList(
  steps: FlowStep[],
  state: FlowState,
  scope: ExprScope,
  report: string[],
  depth: number,
): Promise<FlowStepResult | undefined> {
  let last: FlowStepResult | undefined
  for (const step of steps) {
    const id = step.id!
    const res = isGroupStep(step)
      ? await runGroup(step as FlowGroupStep, id, state, scope, report, depth)
      : await runToolStep(step as FlowToolStep, id, state, scope)
    state.results.set(id, res)
    scope[id] = res
    state.order.push(res)
    if (res.status !== "skipped") {
      state.prev = res
      scope.prev = res
    }
    last = res.status !== "skipped" ? res : last
    report.push(formatStepReport(res))
  }
  return last
}

function formatStepReport(res: FlowStepResult): string {
  const name = res.kind === "group" ? `${res.runs ?? 0} 轮循环` : res.tool ?? ""
  return `### ${res.id} · ${name}（${STATUS_LABEL[res.status]}）\n${capReport(res.output, FLOW_REPORT_STEP_CHARS)}`
}

async function runToolStep(
  step: FlowToolStep,
  id: string,
  state: FlowState,
  scope: ExprScope,
): Promise<FlowStepResult> {
  const ctx = state.ctx
  // 条件分支：为假跳过（不更新 prev，不阻断后续）
  if (step.when !== undefined) {
    let ok: boolean
    try {
      ok = truthy(evalCond(step.when, scope))
    } catch (err) {
      throw new Error(`flow: 步骤 ${id}（${step.tool}）when 表达式错误: ${(err as Error).message}`)
    }
    if (!ok) return { id, tool: step.tool, kind: "tool", status: "skipped", output: "（when 条件为假，已跳过）" }
  }
  const rt = ctx.registry.resolve(step.tool)
  if (!rt) throw new Error(`flow: 未知工具 ${step.tool}`)
  // 安全模式（DESIGN「安全模式」）：flow 直接执行工具、不经引擎拦截点，step 层同规则拦截风险工具
  if (ctx.safeMode && isRiskyToolName(step.tool)) {
    const blocked = safeModeRestrictionMsg(step.tool)
    return { id, tool: step.tool, kind: "tool", status: "blocked", output: blocked }
  }
  if (state.executed >= FLOW_MAX_STEPS) {
    throw new Error(`flow: 工具调用总数超上限（>${FLOW_MAX_STEPS}），已执行 ${state.executed} 次；请拆分为多次 flow 或减少循环规模`)
  }
  const params = { ...(resolveTemplate(step.params ?? {}, scope) as Record<string, unknown>) }
  if (step.input !== undefined) {
    // 显式映射：解析后覆盖同名字段，并抑制自动注入
    const mapped = resolveTemplate(step.input, scope) as Record<string, unknown>
    Object.assign(params, mapped)
  } else if (state.prev !== undefined) {
    autoInject(params, rt.tool, state.prev.output)
  }
  state.executed++
  let result: ToolResult
  try {
    result = await rt.tool.execute(params, ctx)
  } catch (err) {
    if (step.optional) {
      const msg = (err as Error).message ?? String(err)
      return { id, tool: step.tool, kind: "tool", status: "error", output: `（optional 步骤失败，已继续）${msg}`, data: { error: msg } }
    }
    throw new Error(`flow: 步骤 ${id}（${step.tool}）失败: ${(err as Error).message ?? String(err)}`)
  }
  return { id, tool: step.tool, kind: "tool", status: "done", output: result.output, data: result.data }
}

/** 自动注入（旧版线性管道语义，DESIGN「flow 数据流编排工具」数据传递规则）：
 *  脚本工具（sh/py）上一步输出经 stdin（input 参数）传入；其余工具 JSON 字段按 schema 名映射，兜底 input。 */
function autoInject(params: Record<string, unknown>, tool: { name: string; parameters?: { properties?: Record<string, unknown> } }, prevText: string): void {
  if (tool.name === "sh" || tool.name === "py") {
    if (params.input === undefined) params.input = prevText
    return
  }
  const schemaProps = Object.keys(tool.parameters?.properties ?? {})
  let injected = false
  const parsed = tryParseJson(prevText)
  if (parsed !== null && typeof parsed === "object" && !Array.isArray(parsed)) {
    for (const [k, v] of Object.entries(parsed as Record<string, unknown>)) {
      if (schemaProps.includes(k) && params[k] === undefined) {
        params[k] = v
        injected = true
      }
    }
  }
  if (!injected && params.input === undefined) params.input = prevText
}

async function runGroup(
  group: FlowGroupStep,
  id: string,
  state: FlowState,
  scope: ExprScope,
  report: string[],
  depth: number,
): Promise<FlowStepResult> {
  if (group.when !== undefined && !truthy(evalCond(group.when, scope))) {
    return { id, kind: "group", status: "skipped", output: "（when 条件为假，已跳过）" }
  }
  // 组内子步骤引用本组结果：先占位（循环期间引用到的是未完成的占位对象，结束后为完整结果）
  const result: FlowStepResult = { id, kind: "group", status: "done", output: "" }
  state.results.set(id, result)
  scope[id] = result
  const lines: string[] = []
  if (group.foreach !== undefined) {
    let list: unknown[]
    const raw = evalCond(group.foreach, scope)
    if (Array.isArray(raw)) list = raw
    else if (typeof raw === "number" && Number.isInteger(raw) && raw > 0) list = Array.from({ length: raw }, (_, i) => i)
    else throw new Error(`flow: 分组 ${id} foreach 表达式须为数组或正整数（得到 ${typeof raw}）`)
    if (list.length > FLOW_FOREACH_MAX) {
      throw new Error(`flow: 分组 ${id} foreach 规模超上限（${list.length} > ${FLOW_FOREACH_MAX}），请分批处理`)
    }
    const rounds: unknown[] = []
    for (let i = 0; i < list.length; i++) {
      const sub = await runRound(group.steps, state, scope, { item: list[i], index: i }, report, depth, lines, `#${i}`)
      rounds.push(sub?.data ?? null)
    }
    result.data = rounds
    result.runs = list.length
    result.output = lines.join("\n") || "（foreach 完成）"
    return result
  }
  // while 条件循环（do-while 语义：先执行一轮再判断，重试直到成功的场景条件可引用本组最新结果 g.data.xxx）
  const max = Math.min(Math.max(Number(group.maxLoops ?? FLOW_WHILE_DEFAULT_MAX) || FLOW_WHILE_DEFAULT_MAX, 1), FLOW_WHILE_HARD_MAX)
  let rounds = 0
  let last: FlowStepResult | undefined
  for (;;) {
    if (rounds >= max) {
      lines.push(`（已达轮数上限 ${max} 停止；如需继续请再调用 flow）`)
      break
    }
    scope.iteration = rounds
    last = await runRound(group.steps, state, scope, {}, report, depth, lines, `轮${rounds}`)
    result.data = last?.data ?? null
    rounds++
    if (!truthy(evalCond(group.while!, scope))) break
  }
  delete scope.iteration
  result.runs = rounds
  result.output = lines.join("\n") || "（循环完成）"
  return result
}

/** 执行一轮循环体：绑定 item/index/iteration（外层同名绑定保存恢复），报告单轮摘要。 */
async function runRound(
  steps: FlowStep[],
  state: FlowState,
  scope: ExprScope,
  binds: Record<string, unknown>,
  report: string[],
  depth: number,
  lines: string[],
  label: string,
): Promise<FlowStepResult | undefined> {
  const saved: Record<string, unknown> = {}
  for (const k of Object.keys(binds)) {
    saved[k] = scope[k]
    scope[k] = binds[k]
  }
  try {
    const roundReport: string[] = []
    const last = await runStepList(steps, state, scope, roundReport, depth + 1)
    lines.push(`- ${label}（${last ? STATUS_LABEL[last.status] : "无步骤"}）: ${capReport(last?.output ?? "", FLOW_REPORT_ROUND_CHARS).replace(/\n/g, " ")}`)
    report.push(...roundReport)
    return last
  } finally {
    for (const k of Object.keys(binds)) {
      if (saved[k] === undefined) delete scope[k]
      else scope[k] = saved[k]
    }
  }
}

/** flow 主入口：执行数据流编排，返回模型报告文本 + 步骤摘要结构化数据。 */
export async function runFlow(args: { steps: unknown; input?: unknown }, ctx: ToolContext): Promise<ToolResult> {
  const steps = normalizeSteps(args.steps)
  const state: FlowState = { ctx, results: new Map(), order: [], executed: 0 }
  const scope: ExprScope = { input: args.input }
  const report: string[] = []
  await runStepList(steps, state, scope, report, 0)
  return {
    output: report.join("\n\n"),
    data: {
      steps: state.order.map((r) => ({ id: r.id, tool: r.tool, status: r.status, runs: r.runs, data: r.data ?? null })),
    },
  }
}

/** 递归扫描步骤内全部工具的审批要求（flow 的动态 requiresApproval 用）：任一需审批则整体需审批。
 *  安全模式下风险工具在 step 层会被拦截（不执行），不计入审批（不弹审批卡）。 */
export async function scanFlowApprovals(rawSteps: unknown, ctx: ToolContext): Promise<boolean> {
  let steps: FlowStep[]
  try {
    steps = normalizeSteps(rawSteps)
  } catch {
    return true // 结构非法：按需审批处理（执行时会给出具体错误）
  }
  const scan = async (list: FlowStep[]): Promise<boolean> => {
    for (const step of list) {
      if (isGroupStep(step)) {
        if (await scan(step.steps)) return true
        continue
      }
      if (ctx.safeMode && isRiskyToolName(step.tool)) continue
      const rt = ctx.registry.resolve(step.tool)
      if (!rt) continue
      const ra = rt.tool.requiresApproval
      if (typeof ra === "function" ? await ra((step.params ?? {}) as Record<string, unknown>, ctx) : ra) return true
    }
    return false
  }
  return scan(steps)
}
