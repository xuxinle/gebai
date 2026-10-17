/**
 * 治理：**人对演进的三个控制点**——授权面、收单门禁、定期简报。
 *
 * ## 为什么这三件事在一个模块里
 *
 * 它们回答的是同一个问题的三个侧面：**演进跑得对不对，人怎么知道、怎么管**。
 *
 * | 控制点 | 回答的问题 | 失效后果 |
 * |---|---|---|
 * | 授权面（`judgeScope`） | 它能动哪里？ | AI 自己扩大行动面，人很久后才发现 |
 * | 收单门禁（`summarize` 等） | 这一单够格入账吗？ | 纪律靠自觉，「三缺一」变成口号 |
 * | 定期简报（`buildBriefing`） | 人还掌握情况吗？ | 人不再看台账，等于失控 |
 *
 * 三者的判据口径必须**同源**，否则会出现「review 说没事、简报说有事」的两把尺子
 * （本仓已有同型缺陷）。放在一个模块里是让这件事在结构上做不到：改一处口径就是改全部。
 *
 * ## 一条贯穿三者的口径：**三态，且「没核」不冒充任何一侧**
 *
 * 授权面：`in-scope` / `out-of-scope` / `unverified`
 * 门禁：`pass` / `fail` / `unknown`
 * 简报：待决项 / 没核成（`gaps`）
 *
 * 三条线都在同一个地方踩过同一个坑：把「没核成」当阻断，结果是**系统变成不干活**，
 * 而人看到的第一件事就是它停了，接着就学会绕过——那比没有约束更坏。
 * 所以统一口径：**只拦「明确越界」，说不清的一律留痕照走，把判断交回人。**
 */

// ===========================================================================
// 一、授权面：AI 不得扩大自己的行动面
// ===========================================================================

/** 授权范围（由人工写在 `.evolve/authority.json`）。 */
export interface Authority {
  /** 允许改动/新建的路径前缀（仓内相对路径）。**空数组 = 明确不授权任何写入**。 */
  allowPaths: string[]
  /** 明确禁止的路径前缀（优先于 allowPaths）。支持 `*` 通配（`.env*` = 文件族）。 */
  denyPaths?: string[]
  /** 允许的动作类别（自由串，仅在方向声明了类别时校验；缺省 = 不校验）。 */
  allowKinds?: string[]
  /** 授权由谁给、何时给（审计用）。 */
  grantedBy?: string
  grantedAt?: number
  /** 授权到期时间（毫秒）。`null`/缺失 = 不过期。过期即视为**无授权**。 */
  expiresAt?: number | null
  /** 文件是否真的存在（由读取方置位）。`false` = 从未声明过授权面。 */
  declared?: boolean
}

/** 一条方向的判定结果（三态）。 */
export type ScopeVerdict =
  | { kind: "in-scope"; matched: string }
  | { kind: "out-of-scope"; reason: string; offending: string[] }
  | { kind: "unverified"; reason: string }

/** 授权面是否被**显式声明**过。
 *  读不到文件时不得凭空造一个空授权：空授权会让所有方向都「越界」。 */
export function isDeclared(a: Authority): boolean {
  return a.declared === true
}

/** 从一条方向的自由文本里抽**可判定路径候选**：
 *  ① 带 `/` 的路径（`tools/a.py`、`packages/x/y`）；
 *  ② **裸文件名**（`.env`、`main.ts`）——顶层配置文件恰恰最该禁、也最容易没有 `/`。
 *
 *  注意：**抽不到路径是常态**——大多数方向写的是自然语言（「修一下并发丢写」）。
 *  这种情况必须走 `unverified` 而**不是**越界。 */
export function pathsInDirection(text: string): string[] {
  const out = new Set<string>()
  for (const m of text.matchAll(/[\w@.+-]+(?:\/[\w@.+*-]+)+\/?/g)) {
    out.add(m[0].replace(/[.,;:、）)]+$/, "").replace(/\/+$/, "").toLowerCase())
  }
  for (const m of text.matchAll(/(?:^|[\s（(、「",：:；;])((?:\.[\w-]+)+|[\w@.+-]+\.[A-Za-z0-9]+)(?=$|[\s）)、,，。：:；;"'])/g)) {
    const v = m[1].toLowerCase()
    if (v.length > 1 && !out.has(v)) out.add(v)
  }
  return [...out]
}

/** 归一：反斜杠转正斜杠、去 `./`、小写、去尾斜杠。 */
function norm(p: string): string {
  return p.replace(/\\/g, "/").replace(/^\.\//, "").replace(/\/+$/, "").trim().toLowerCase()
}

/** 路径 `p` 是否落在前缀 `prefix` 之下。
 *
 *  **按段比较**：`toolshed/x` 不算 `tools` 之下（否则「禁 tools」会误禁一片）。
 *  **支持 `*` 通配**：`.env*` 表「文件族」。为何需要它：按段比较对**文件**不适用——
 *  `.env.local` 不以 `.env/` 开头，用段判会漏禁；而把 `.env` 当「文件名前缀」又会误禁
 *  `.envrc`。两种语义不能靠猜，必须由授权方写清：目录用 `tools`，文件族用 `.env*`。 */
export function underPrefix(p: string, prefix: string): boolean {
  const a = norm(p)
  const b = norm(prefix)
  if (!b) return false
  if (b.includes("*")) {
    const re = new RegExp(`^${b.split("*").map((s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")).join(".*")}$`)
    return re.test(a)
  }
  if (a === b) return true
  return a.startsWith(`${b}/`)
}

/**
 * 判一条方向是否在授权范围内。
 *
 * 顺序即优先级：**先声明 → 再过期 → 再禁区 → 再允许区 → 最后「没写路径」**。
 * 反过来会让一条落在禁区的方向因为「allowPaths 里有更宽的前缀」而被放行。
 */
export function judgeScope(direction: string, authority: Authority, now = Date.now()): ScopeVerdict {
  // **未声明授权面** → 未核（照派 + 留痕）。不能当越界：没声明不等于拒绝。
  if (!isDeclared(authority)) {
    return {
      kind: "unverified",
      reason: "项目未声明授权面（`.evolve/authority.json` 不存在）——照派，但人应当在简报里看到这条",
    }
  }
  if (authority.expiresAt != null && now > authority.expiresAt) {
    return { kind: "out-of-scope", reason: `授权已于 ${new Date(authority.expiresAt).toISOString()} 到期——过期即无授权`, offending: [] }
  }
  const paths = pathsInDirection(direction)
  if (paths.length === 0) {
    return {
      kind: "unverified",
      reason: "方向里没有可判定路径（只写了自然语言）——授权面无法核对，需人看一眼，不当作通过",
    }
  }
  const deny = (authority.denyPaths ?? []).map(norm).filter(Boolean)
  const hitDeny = paths.filter((p) => deny.some((d) => underPrefix(p, d)))
  if (hitDeny.length) {
    return { kind: "out-of-scope", reason: "落在显式禁区", offending: hitDeny }
  }
  const allow = (authority.allowPaths ?? []).map(norm).filter(Boolean)
  if (allow.length === 0) {
    return {
      kind: "out-of-scope",
      reason: "尚未授与任何写入路径（allowPaths 为空）——默认拒绝，需人显式授权",
      offending: paths,
    }
  }
  const outside = paths.filter((p) => !allow.some((a) => underPrefix(p, a)))
  if (outside.length) {
    return { kind: "out-of-scope", reason: "不在授权路径前缀内", offending: outside }
  }
  return { kind: "in-scope", matched: paths.find((p) => allow.some((a) => underPrefix(p, a))) ?? paths[0] }
}

/** 派工侧动作：`accept` 派；`hold` 不派但留种子等人批；`reject` 拒（落在禁区）。 */
export type ScopeAction = "accept" | "hold" | "reject"

export interface ScopeDecision {
  action: ScopeAction
  verdict: ScopeVerdict
  /** 进台账的一句话（**必须能说清是哪条方向、为什么**）。 */
  note: string
}

/**
 * 把判定映射成派工动作。
 *
 * **`unverified` 照派**——这是本模块最容易写错的一处。大多数方向就是自然语言，
 * 把「没核」当阻断 ⇒ 所有正常方向的单都停派，围栏从「限制越权」变成「停止演进」。
 * 只能拦住「明确越界」，拦不住「说不清」；说不清的部分交给简报报到人面前。
 */
export function decideScope(direction: string, authority: Authority, now = Date.now()): ScopeDecision {
  const v = judgeScope(direction, authority, now)
  if (v.kind === "in-scope") return { action: "accept", verdict: v, note: `在授权内（${v.matched}）` }
  if (v.kind === "unverified") {
    return { action: "accept", verdict: v, note: `授权面未核（照派）：${v.reason}` }
  }
  const isDeny = v.reason.includes("禁区")
  return {
    action: isDeny ? "reject" : "hold",
    verdict: v,
    // 落在禁区 → 直接拒；只是「没在授权内」→ 留种子等人批（可能是该扩授权，而不是方向错）
    note: `越界（${v.reason}）：${v.offending.join("、") || "（无路径）"}`,
  }
}

// ===========================================================================
// 二、收单门禁：把「建议」接成「必经路径」
// ===========================================================================

/** 一门的结果。 */
export type GateState =
  | "pass" // 核过，没问题
  | "fail" // 核过，有问题（拒绝入账）
  | "unknown" // 没核成（缺失/没跑/读不到）——既不能入账，也不算发现缺陷

export interface GateOutcome {
  /** 门名（进回显/台账）。 */
  gate: "anchors" | "commit-scope" | "reverse-verify"
  state: GateState
  /** 一句话结论（人读）。 */
  detail: string
  /** 该项是否具备强制跳过的能力（reverse-verify 缺 spec 时可跳过，锚点不行）。 */
  forceable: boolean
}

/** 全部门的结果 + 总判。 */
export interface GateReport {
  outcomes: GateOutcome[]
  /** 只有全部 pass 才是 true；任一 fail/unknown 都为 false。 */
  ok: boolean
  /** 拒绝入账的理由（人读，逐条）。 */
  blockers: string[]
}

/** 汇总裁决：**全 pass 才 ok**（不是「没有 fail 就 ok」）。 */
export function summarize(outcomes: GateOutcome[]): GateReport {
  const blockers: string[] = []
  for (const o of outcomes) {
    if (o.state === "fail") blockers.push(`[${o.gate}] ${o.detail}`)
    else if (o.state === "unknown") blockers.push(`[${o.gate}] 未核成：${o.detail}`)
  }
  return { outcomes, ok: blockers.length === 0, blockers }
}

/** 强制跳过的形态：每一项被跳过都必须带理由，理由为空即不成立。 */
export interface ForceSkip {
  gate: GateOutcome["gate"]
  reason: string
}

/**
 * 应用强制跳过：被跳过的项从 fail 改为 pass，但**另记一条**进 `skipped`。
 * 理由必填——没有理由的跳过与没有门禁等价，而它还会让人误以为门禁在工作。
 */
export function applyForce(report: GateReport, skips: readonly ForceSkip[]): GateReport & { skipped: string[] } {
  const skipped: string[] = []
  const outcomes = report.outcomes.map((o) => {
    const hit = skips.find((s) => s.gate === o.gate && s.reason.trim())
    if (!hit) return o
    if (o.state === "pass") return o
    skipped.push(`[${o.gate}] 已强制跳过：${hit.reason.trim()}（原判 ${o.state}：${o.detail}）`)
    return { ...o, state: "pass" as GateState, detail: `${o.detail}｜**已强制跳过**（${hit.reason.trim()}）` }
  })
  return { ...summarize(outcomes), skipped }
}

/**
 * 门①：修复锚点。任一 `present:false` 即 fail。
 *
 * 为何这一门**不可跳过**：锚点缺失只有一个成因——某处修复被回退了。
 * 而「拿不到清单」报 `unknown`（能力缺口）而非 `fail`：仓外/沙箱里判定不了，
 * 那是「没能力核」，不是「核出了问题」。
 */
export async function gateAnchors(
  run: ((cmd: string, o?: { workdir?: string; timeoutMs?: number }) => Promise<{ code: number; stdout?: string; stderr?: string }>) | undefined,
  repoRoot: string,
  python = "python3",
): Promise<GateOutcome> {
  const base = { gate: "anchors" as const, forceable: false }
  if (typeof run !== "function") return { ...base, state: "unknown", detail: "宿主不提供 runCommand，锚点无法核" }
  let r
  try {
    r = await run(`${python} tools/shengsheng_sentinel.py --anchors`, { workdir: repoRoot, timeoutMs: 60_000 })
  } catch (e) {
    return { ...base, state: "unknown", detail: `锚点脚本跑不起来（${String(e).slice(0, 80)}）` }
  }
  const out = `${r.stdout ?? ""}${r.stderr ?? ""}`
  // **先看输出能不能解析，再看退出码**：仓外/沙箱里脚本往往根本不在，
  // 此时各种退出码都可能出现，拿它当「锚点缺失」会把没能力核的环境报成「修复被回退」。
  let parsed: { ok?: boolean; missing?: string[]; anchors?: unknown[] } | null = null
  try {
    parsed = JSON.parse(out.slice(out.indexOf("{"), out.lastIndexOf("}") + 1))
  } catch {
    parsed = null
  }
  if (!parsed || !Array.isArray(parsed.anchors)) {
    return {
      ...base,
      state: "unknown",
      detail: `拿不到锚点清单（哨兵不在 / 输出不可解析 / code=${r.code}）——这是**能力缺口**，不等于修复被回退`,
    }
  }
  if (parsed.missing && parsed.missing.length) {
    return { ...base, state: "fail", detail: `修复锚点缺失：${parsed.missing.join("、")}` }
  }
  if (parsed.ok === true) return { ...base, state: "pass", detail: `${parsed.anchors.length} 条修复锚点全在` }
  return { ...base, state: "unknown", detail: `锚点清单格式异常（code=${r.code}）` }
}

/**
 * 门②：提交范围。`outOfScope` 非空 ⇒ fail；**读不到 ⇒ unknown**（不是 pass）。
 * 这是本仓「0 命中与读不到必须是两个数」那条纪律的落地。
 */
export function gateCommitScope(
  audit: { outOfScope: { path: string; added: number | null; deleted: number | null }[]; inScope: number } | null,
  readError?: string,
): GateOutcome {
  const base = { gate: "commit-scope" as const, forceable: false }
  if (!audit) {
    return { ...base, state: "unknown", detail: `读不到 git 顶层或最近一次提交（${readError ?? "未知原因"}）——这不等于「没有宽范围暂存」` }
  }
  if (audit.outOfScope.length === 0) {
    return { ...base, state: "pass", detail: `核过 ${audit.inScope} 个文件，均在工单范围内` }
  }
  const list = audit.outOfScope.map((f) => `${f.path}（+${f.added ?? 0} −${f.deleted ?? 0}）`).join("、")
  return { ...base, state: "fail", detail: `疑似宽范围暂存带走了别人的改动：${list}` }
}

/**
 * 门③：反向验证。判据由 `verify-gate.judge` 给出，本函数只做「有没有 spec」与三态映射。
 * **没声明 spec ⇒ unknown（可跳），不是 pass**：把「没声明」静默当 pass，
 * 等于让这条闸对所有不声明的单永久失效。
 */
export function gateReverseVerify(
  specDeclared: boolean,
  judged: { verdict: string; reason: string } | null,
): GateOutcome {
  const base = { gate: "reverse-verify" as const, forceable: true }
  if (!specDeclared) {
    return { ...base, state: "unknown", detail: "本单未声明反向验证 spec——若是机制类交付必须补（`verify` 段）；非机制类可带理由强制跳过" }
  }
  if (!judged) return { ...base, state: "unknown", detail: "声明了 spec 但未执行（没跑出观察集）" }
  if (judged.verdict === "genuine") return { ...base, state: "pass", detail: judged.reason }
  if (judged.verdict === "not-run" || judged.verdict === "unstable") {
    return { ...base, state: "unknown", detail: `${judged.verdict}：${judged.reason}` }
  }
  return { ...base, state: "fail", detail: `[${judged.verdict}] ${judged.reason}` }
}

// ===========================================================================
// 三、定期简报：把判断所需的事实摆到人面前，且优先摆异常
// ===========================================================================

/** 一条待决项：需要人做决定的事（不是 AI 能自己定的）。 */
export interface DecisionItem {
  /** 类别：越权请求 / 长期停滞 / 反复失败 / 方向枯 / 通知不可达。 */
  kind: "out-of-scope" | "stalled" | "repeated" | "no-direction" | "unreachable"
  /** 一句话事实（带数字）。 */
  fact: string
  /** 为什么这需要人决定（而不是 AI 自己决定）。 */
  why: string
  /** 可选的处置选项（人挑一个）。 */
  options?: string[]
}

/** 简报主体。 */
export interface Briefing {
  window: { from: number; to: number }
  counts: {
    /** 台账新增节数（= 收了几单）。`null` = 台账读不到（**不是 0**）。 */
    ledgerSections: number | null
    workers: number
    inflight: number
    seeds: number
  }
  /** 需要人拍板的事（**空数组 = 没有待决项，但不是「一切良好」**）。 */
  decisions: DecisionItem[]
  /** 数据缺口（读不到的东西）。这些**不得**被当作「没问题」。 */
  gaps: string[]
  /** 一句话状态（只描述客观量，不含评价）。 */
  headline: string
}

/** 简报输入（由调用方从磁盘读出后传入——本函数不读盘，便于穷举测试）。 */
export interface BriefingInput {
  now: number
  since?: number
  state: {
    workers: { alias: string; status?: string }[]
    inflight: { alias: string; dispatchedAt?: number }[]
    seeds: { direction: string; outOfScope?: boolean }[]
  }
  /** 台账原文；`null` = 读不到（**不等于台账为空**）。 */
  ledger: string | null
  /** 通知通道是否可达（`null` = 未核）。 */
  notifyReachable?: boolean | null
}

/** 窗口缺省长度：一天。 */
export const DEFAULT_WINDOW_MS = 24 * 60 * 60 * 1000

/** 停滞判定：在途超过这么久没有任何推进，就报到人面前。 */
export const STALL_MS = 48 * 60 * 60 * 1000

/** 台账节切分（`## <iso>｜<别名>｜<领域>`；也认不带时间的旧形态）。 */
export function splitLedgerSections(ledger: string): { alias: string; domain: string; at: number | null; text: string }[] {
  const out: { alias: string; domain: string; at: number | null; text: string }[] = []
  for (const p of ledger.split(/\n(?=## )/)) {
    const head = /^##\s+([^\n]*)/.exec(p)?.[1] ?? ""
    if (!head) continue
    const cells = head.split("｜").map((s) => s.trim())
    const atRaw = cells[0] ?? ""
    const at = /^\d{4}-\d{2}-\d{2}T/.test(atRaw) ? Date.parse(atRaw) : null
    out.push({
      alias: cells[1] ?? "",
      domain: cells[2] ?? "",
      at: Number.isFinite(at as number) ? (at as number) : null,
      text: p,
    })
  }
  return out
}

/**
 * 生成简报。**纯函数**：同样的输入必得同样的输出（可穷举测试，也可回放历史）。
 *
 * 判据集中在这里而不是散在工具里：「什么算需要人决定」必须只有一份定义。
 */
export function buildBriefing(input: BriefingInput): Briefing {
  const now = input.now
  const since = input.since ?? now - DEFAULT_WINDOW_MS
  const gaps: string[] = []
  const decisions: DecisionItem[] = []

  // ── 台账：读不到 ≠ 没有进展 ────────────────────────────────────
  let ledgerSections: number | null = null
  if (input.ledger == null) {
    gaps.push("台账读不到（不等于本周无进展——这一项**没核**）")
  } else {
    ledgerSections = splitLedgerSections(input.ledger).filter((s) => s.at != null && s.at >= since && s.at <= now).length
  }

  const { workers, inflight, seeds } = input.state

  // ── 停滞：在途挂太久（真值判据，不看「有没有在跑」）──────────────
  const stale = inflight.filter((i) => now - (i.dispatchedAt ?? 0) > STALL_MS)
  if (stale.length) {
    decisions.push({
      kind: "stalled",
      fact: `${stale.length} 条在途已挂超过 ${Math.round(STALL_MS / 3_600_000)} 小时未收：${stale
        .slice(0, 3)
        .map((i) => `${i.alias}（${Math.round((now - (i.dispatchedAt ?? 0)) / 3_600_000)}h）`)
        .join("、")}`,
      why: "在途不销，下一息会把它当成「还在做」——要么收、要么退回种子，但这需要判断它到底值不值得继续",
      options: ["收（先 review 抢救结论）", "退回种子队列", "再给一个窗口"],
    })
  }

  // ── 有动作零进展 ──────────────────────────────────────────────
  if (ledgerSections === 0 && (workers.length > 0 || inflight.length > 0)) {
    decisions.push({
      kind: "repeated",
      fact: `过去 ${Math.round((now - since) / 3_600_000)} 小时**没有任何一单收进台账**，而池里还有 ${workers.length} 个 worker、${inflight.length} 条在途`,
      why: "有动作不等于有进展。停在这里可能是方向选错了、也可能被门禁挡着——两者处置相反，得人看一眼",
      options: ["看门禁拒绝原因", "换方向", "暂停演进"],
    })
  }

  // ── 越权请求：AI 无权扩授权，只能提出 ──────────────────────────
  const oos = seeds.filter((s) => s.outOfScope === true)
  if (oos.length) {
    decisions.push({
      kind: "out-of-scope",
      fact: `${oos.length} 条方向超出当前授权范围、未派工：${oos.slice(0, 3).map((s) => `「${s.direction.slice(0, 40)}」`).join("、")}`,
      why: "AI 无权扩大自己的授权面。要么你批准（把它加进授权），要么这条方向作废",
      options: ["批准并入授权", "作废", "改成范围内的替代方向"],
    })
  }

  // ── 方向枯 ────────────────────────────────────────────────────
  if (seeds.length === 0 && inflight.length === 0) {
    decisions.push({
      kind: "no-direction",
      fact: "种子队列与在途都空——下一息无向可择",
      why: "这是需要人给方向的时刻（或明确说「暂时不要推进」）。AI 自己编方向会开始做没人要的事",
      options: ["给一个新方向", "暂停演进", "让它自己探索一轮（问卦）"],
    })
  }

  // ── 汇报通道本身是否可达 ──────────────────────────────────────
  if (input.notifyReachable === false) {
    decisions.push({
      kind: "unreachable",
      fact: "通知通道不可达——这份简报**送不到你手上**",
      why: "无人值守汇报的前提是通道通。不通时演进仍在跑，只是你收不到任何消息",
      options: ["配置通知通道", "改为只看简报文件", "暂停演进"],
    })
  } else if (input.notifyReachable == null) {
    gaps.push("通知通道可达性未核（`null` = 没核，不是「可达」）")
  }

  const headline = [
    `窗口 ${new Date(since).toISOString().slice(0, 16)} → ${new Date(now).toISOString().slice(0, 16)}`,
    ledgerSections == null ? "台账未核" : `收 ${ledgerSections} 单`,
    `池 ${workers.length}｜在途 ${inflight.length}｜种子 ${seeds.length}`,
    `待你决定 ${decisions.length} 项`,
  ].join("｜")

  return {
    window: { from: since, to: now },
    counts: { ledgerSections, workers: workers.length, inflight: inflight.length, seeds: seeds.length },
    decisions,
    gaps: [...new Set(gaps)],
    headline,
  }
}

/**
 * 简报的交付形态（给人看的 markdown）。
 *
 * 刻意**短**：超过一屏的汇报人不看，而「不看」比「不报」更糟——
 * 它会让人以为自己掌握了情况。
 */
export function renderBriefing(b: Briefing): string {
  const lines: string[] = [`**演进简报**｜${b.headline}`]
  if (b.decisions.length === 0) {
    lines.push("", "本轮**没有需要你决定的事**（这只说明没有异常，不等于方向正确）。")
  } else {
    lines.push("", "## 需要你决定", "")
    b.decisions.forEach((d, i) => {
      lines.push(`${i + 1}. ${d.fact}`)
      lines.push(`   - 为什么得你定：${d.why}`)
      if (d.options?.length) lines.push(`   - 可选：${d.options.join(" / ")}`)
    })
  }
  if (b.gaps.length) {
    lines.push("", "## 没核成的（不能当成「没问题」）", "")
    for (const g of b.gaps) lines.push(`- ${g}`)
  }
  return lines.join("\n")
}

/** 是否到了该出简报的时候（距上一份超过窗口）。 */
export function dueForBriefing(lastAt: number | null | undefined, now: number, windowMs = DEFAULT_WINDOW_MS): boolean {
  if (!lastAt) return true // 从没出过 = 该出
  return now - lastAt >= windowMs
}
