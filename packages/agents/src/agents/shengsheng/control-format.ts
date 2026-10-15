/**
 * control.md 的**表格格式与版本化解析**——在途（inflight）与 worker 池（pool）两张表。
 *
 * **为什么要单独一个模块**：这两张表在演进中**加过列**（池表加「轮次」、在途表先加「轮次」再加
 * 「会话」），而 `control.md` 是**磁盘上的存量数据**——工具升级不迁移数据，旧行会一直躺到被收藏/
 * 退役（本仓 `.evolve/control.md` 的在途表就长期是旧格式）。兼容逻辑若散在 `parseControl` 的各个
 * 字段里，再加一列就要在 N 处各改一遍，且各处的判据会漂移（本仓已实测两次：`e1e803ce` 的身份键
 * 半途、`fb53b33e` 的旧行误判）。故这里把「**格式版本 + 升级函数**」显式化，两张表各有一份版本序：
 *
 * ```
 * 在途表                                              声明列数   自带会话列
 *   v1  别名 | 方向 | 预期产出 | 派发时间                    4        —
 *   v2  别名 | 方向 | 预期产出 | 派发时间 | 轮次              5        —      （74a2e6b9）
 *   v3  别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次        6        自带   （e1e803ce 起，现行）
 *
 * 池表
 *   v1  别名 | 会话 | 领域 | 任务 | 派发时间 | 状态             6        —
 *   v2  别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态       7        —      （74a2e6b9 起，现行）
 * ```
 *
 * 解析流水线（**版本信息在每一跳都显式可见**，不再是散落的「要不要 +1 下标」）：
 *
 * ```
 * cells(line)                        转义感知的格切分（见 {@link cells}）
 * tableBlock(md, heading)            取一段表：数据行 + 该表**表头声明的列数**（代次标志）
 * selectInflightVer(cells, ctx)      这一行是哪一代——**按证据判**，不看格数（见下）
 * parseInflightRow(cells, ctx)       按该代的**位置声明**读取 → { ver, fields }
 * parseInflightLine(line, ctx)       同上（自带切格）
 * migrateInflight({ ver, fields })   升级为**现行形态**：该代没有的列在此补空、数字在此定型
 * inflightHeader() / inflightCells() 现行形态 → 表头/格（写出侧；与版本声明同源）
 * ```
 *
 * **加一列要改哪里**（本模块存在的理由，逐处可数）：
 * 1. {@link INFLIGHT_VERSIONS} 加一条**版本声明**（列名 + 各字段的位置 {@link FieldSpec}）；
 * 2. 若新列要进域对象：{@link Inflight} 接口加字段 + {@link inflightCells} 加一格（写出侧）；
 * 3. **读入侧零改动**——判代（{@link selectInflightVer}）、位置读取（{@link readFields}）、
 *    升级（{@link migrateInflight}）都是**表驱动**的，不含任何版本号字面量。
 *    （`migrateInflight` 由声明推出「该代没有哪些列」并补空，故它也不需要加分支。）
 *
 * 与 {@link import("./shengsheng")} 的关系：本模块只负责**格式**（版本、位置、升级），
 * 「这条登记是谁的」（身份键 `inflightIdentity`）与去重归一留在原处——那是语义、不是格式。
 */
import type { Inflight, Worker } from "./shengsheng"

// ---------------------------------------------------------------------------
// 表格切分（转义感知）
// ---------------------------------------------------------------------------

/** 表格行拆分（`| a | b |` → ["a","b"]）——**转义感知**：`\|` 是正文里的字面竖线、不是分隔符。
 *
 * 为什么必须认转义：`renderTable` 把正文的 `|` 写成 `\|`（历代渲染器都如此），而切分不认转义时，
 * 含一个竖线的字段就**多切一格**——在「在途」表里这一格足以把旧格式行（5 列）顶成新版行（6 列）：
 * 会话列被填成方向的碎片，读态拿这个伪线索去探一个不存在的会话，可达会话于是被判「中道而废」
 * （正是身份键单一化要堵的那类假阳性，只是从旧格式这一侧重新漏进来）。
 *
 * 只认 `\|` 一种转义、不认 `\\`：历代渲染器只转义竖线，正文里的反斜杠是**原样写下**的——若一律
 * 吃掉反斜杠，`C:\Users` 会被蚀成 `C:Users`。而「只转义 `|`」与「只认 `\|`」恰好互为逆运算，
 * 任何字段（含反斜杠、含竖线）都能无损往返，历史内容也无须改写。 */
export function cells(line: string): string[] {
  const body = line.trim().replace(/^\|/, "").replace(/\|$/, "")
  const out: string[] = []
  let cur = ""
  for (let i = 0; i < body.length; i++) {
    const ch = body[i]
    if (ch === "\\" && body[i + 1] === "|") {
      cur += "|"
      i += 1
      continue
    }
    if (ch === "|") {
      out.push(cur.trim())
      cur = ""
      continue
    }
    cur += ch
  }
  out.push(cur.trim())
  return out
}

/** markdown 表格分隔行（`| --- | --- |`）。 */
export const isSeparator = (line: string): boolean => /^\|[\s:|-]+\|$/.test(line.trim())

/** 取 `## <标题>` 段落内的表格：数据行（跳过表头与分隔行；`（空）` 视为零条目）＋该表**表头声明的列数**。
 *
 * 为什么要把表头列数一并取出：它是这张表的**代次标志**（旧版在途表 5 列、新版 6 列；旧版池表 6 列、
 * 新版 7 列）。行自身的格数在旧表里**不可靠**——旧渲染器不转义竖线（见 {@link cells}），正文里一个
 * 裸竖线就让旧行多切一格、长得像新版行。表头是同一个渲染器写的，与行的代次同源，比格数稳。 */
export function tableBlock(md: string, heading: string): { rows: string[][]; declared?: number } {
  const lines = md.split(/\r?\n/)
  const start = lines.findIndex((l) => l.trim() === `## ${heading}`)
  if (start < 0) return { rows: [] }
  const rows: string[][] = []
  let declared: number | undefined
  for (let i = start + 1; i < lines.length; i++) {
    const line = lines[i]
    if (/^##\s/.test(line.trim())) break
    if (!line.trim().startsWith("|")) continue
    if (isSeparator(line)) continue
    const c = cells(line)
    if (c[0] === "别名" || c[0] === "优先级" || c[0] === "键") {
      declared = c.length // 表头
      continue
    }
    if (c[0] === "（空）") continue
    rows.push(c)
  }
  return { rows, declared }
}

/** 取 `## <标题>` 段落内的表格数据行（只有行、不看表头——种子/待办两表用）。
 *  种子/待办两表列数从未变过（盘上不存在多代），故未纳入版本化。 */
export function tableRows(md: string, heading: string): string[][] {
  return tableBlock(md, heading).rows
}

/** 会话 id 的形状（在途表「会话」列的**证据之一**）：存储层白名单就是 `^[0-9a-f]{32}$`
 *  （见 `packages/server/src/core/base/paths.ts` 的 `isValidSessionId`——会话 id 是 `randomUUID()` 去连字符，
 *  飞书桥接的确定性 id 是 sha256 前 32 位 hex，两路都落在这个形状里）。
 *
 * 用它挡「旧格式行被误当新版行」：旧行没有会话列，而自由文本（方向/交付）里一个**裸竖线**（旧渲染器
 * 不转义）就能让旧行多切一格、把方向碎片顶到会话位上。形状不符时**选保守一侧**——不认这一格是会话：
 * 认错（把碎片当 id）= 拿伪线索去探一个不存在的会话 → 可达会话被判「中道而废」（假阳性）；
 * 不认（丢掉真 id）= 回落到池按别名解析同一会话，**值一模一样**。代价不对称，故形状可据。 */
export function isSessionIdShape(v: string): boolean {
  return /^[0-9a-f]{32}$/.test(v.trim())
}

// ---------------------------------------------------------------------------
// 字段位置声明（表驱动的读入侧）
// ---------------------------------------------------------------------------

/** 一个字段在行里的位置：**三者只取其一**——自左定址（`at`）、自尾定址（`fromEnd`，1 = 行尾那格）、
 *  或「自由文本列」（`at` + `overflow`：把溢出的格并回本列，见 {@link readFields}）。 */
export interface FieldSpec {
  /** 自左定位（0 基；越界 → 空串）。 */
  at?: number
  /** 自尾定位（1 = 行尾那格）。**尾列一律自尾锚定**：行越长（被裸竖线撑开）越不能按固定下标取，
   *  否则取到的是正文——锚点在行尾才稳。 */
  fromEnd?: number
  /** 自由文本列：溢格只可能落在它身上（方向/预期产出、池的任务），多余的格并回本列、不丢。 */
  overflow?: boolean
}

/** 泛化的行：字段名 → 该行里的取数位置。 */
export type FieldMap = Record<string, FieldSpec>

/** 一行的读取实现——**表驱动，不含任何版本号字面量**（加列时无需改这里）：只要给一份
 *  `FieldMap`（该代的位置声明）就能把一行读成「字段名 → 字符串」。 */
export function readFields(c: readonly string[], map: FieldMap): Record<string, string> {
  // 「溢出的格」的右边界：该行**自尾定址**里最靠左的那个字段（即尾部定长列的起点）。
  const anchors = Object.values(map).map((s) => s.fromEnd).filter((n): n is number => typeof n === "number")
  const overflowEnd = anchors.length ? c.length - Math.max(...anchors) : c.length
  const out: Record<string, string> = {}
  for (const [name, spec] of Object.entries(map)) {
    if (typeof spec.fromEnd === "number") {
      out[name] = c[c.length - spec.fromEnd] ?? ""
      continue
    }
    const at = spec.at ?? 0
    // 溢格并回：只有自由文本列吃中间的碎格（`slice` 到尾部定长列之前）；无溢格时就是它自己那一格。
    out[name] = spec.overflow && overflowEnd > at ? c.slice(at, overflowEnd).join(" | ") : c[at] ?? ""
  }
  return out
}

// ---------------------------------------------------------------------------
// 在途表：版本声明
// ---------------------------------------------------------------------------

/** 在途表格式版本。**只加列、不改既有列的含义**，故版本号单调递增即可。 */
export type InflightVer = 1 | 2 | 3

export interface InflightVersion {
  ver: InflightVer
  /** 该代的列（同时是**表头声明**的权威来源，写出侧也从这里取）。 */
  columns: readonly string[]
  /** 该代是否**自带**「会话」列；不自带的代会话线索只能经池按别名解析。 */
  ownSession: boolean
  /** 该代的字段位置（{@link FieldSpec}）——读入侧的唯一依据。 */
  read: FieldMap
}

export const INFLIGHT_VERSIONS: readonly InflightVersion[] = [
  {
    ver: 1,
    columns: ["别名", "方向", "预期产出", "派发时间"],
    ownSession: false,
    // 最早一版：无轮次列。派发时间就在第 4 格（**定址**而非自尾——4 格行的自尾两位落在「预期产出」上）。
    read: { alias: { at: 0 }, direction: { at: 1 }, deliverable: { at: 2 }, dispatchedAt: { at: 3 } },
  },
  {
    ver: 2,
    columns: ["别名", "方向", "预期产出", "派发时间", "轮次"],
    ownSession: false,
    read: {
      alias: { at: 0 },
      direction: { at: 1 },
      deliverable: { at: 2, overflow: true },
      dispatchedAt: { fromEnd: 2 },
      round: { fromEnd: 1 },
    },
  },
  {
    ver: 3,
    columns: ["别名", "会话", "方向", "预期产出", "派发时间", "轮次"],
    ownSession: true,
    read: {
      alias: { at: 0 },
      sessionId: { at: 1 },
      direction: { at: 2 },
      deliverable: { at: 3, overflow: true },
      dispatchedAt: { fromEnd: 2 },
      round: { fromEnd: 1 },
    },
  },
]

/** 现行在途版本（写出侧永远用这一代；读入侧识别到的旧代经 {@link migrateInflight} 升级到它）。 */
export const INFLIGHT_CURRENT: InflightVer = 3

const inflightVer = (ver: InflightVer, versions: readonly InflightVersion[] = INFLIGHT_VERSIONS): InflightVersion => {
  const v = versions.find((x) => x.ver === ver)
  if (!v) throw new Error(`未知的在途格式版本：${ver}`)
  return v
}

/** 现行在途表头（渲染用：与 {@link INFLIGHT_VERSIONS} 同一份声明，不会各写各的）。 */
export const inflightHeader = (): string[] => [...inflightVer(INFLIGHT_CURRENT).columns]

// ---------------------------------------------------------------------------
// 在途表：版本选择（**证据优先于格数**）
// ---------------------------------------------------------------------------

export interface InflightCtx {
  /** 该表**表头声明的列数**（{@link tableBlock} 给）；缺省 = 无表头（按证据判）。 */
  declared?: number
  /** 同文件池表里出现过的会话 id——「同源」这一档证据（池是在途的登记源，两侧同源写入）。 */
  poolSessions?: ReadonlySet<string>
}

/**
 * 这一行是哪一代在途行——**按证据判，不按格数**。
 *
 * 为什么不能只看列数：方向/交付是自由文本，**旧版渲染器不转义竖线**（见 {@link cells}），正文里一个
 * 裸竖线就让旧行多切一格、长得像新版行，会话列被填成方向的碎片。误判的代价**不对称**：
 * 把旧行读成新版 → 拿伪会话 id 去探一个不存在的会话 → 可达会话被判「中道而废」（假阳性，
 * 且会误导控制面去退役它在跑的 worker）；把新版行读成旧行 → 丢掉真会话 id，但可经池按别名解析
 * 回来（同会话并列两条会被归一，不误判）。故保守一侧是**旧版**。
 *
 * 「自带会话列」那一族的**证据四档**（任一成立即认，从容纳列数**最高**的一代起试）：
 *   ① 形状——`^[0-9a-f]{32}$`（{@link isSessionIdShape}）；
 *   ② 同源——这一格**就是同文件池里某个 worker 的会话**；
 *   ③ 对位——表头声明的列数与该代列数一致、且本行恰那么多格（无溢格可言，「裸竖线把方向顶上来」
 *      没有发生的余地）；
 *   ④ 空值——新版行的会话列可以为空（此时方向不会顶到会话格上）。
 * 其余（表头说旧版、会话格既非 id 也非空、少格）一律按**旧版**读——旧版读法也是新版行缺列时的正确
 * 读法（会话列丢了，方向就落到第 2 格，恰好对位）。旧版一族再按**本行格数**取「装得下」的最高一代
 * （5 格以上 → v2；4 格 → v1；更短的残行 → v1）。
 *
 * 本函数**不含任何版本号字面量**：四档证据、旧版上界、候选次序全部由 {@link INFLIGHT_VERSIONS} 推出
 * ——加一列时它无需改动（这是「加列只需加一条声明」的关键一环）。
 */
export function selectInflightVer(c: readonly string[], ctx: InflightCtx = {}, versions: readonly InflightVersion[] = INFLIGHT_VERSIONS): InflightVer {
  const declared = ctx.declared
  // 表头声明不超过「无会话列」那一族的列数上界 → 表头说这张表没有会话列，不认任何自带会话列的一代。
  const maxLegacy = Math.max(...versions.filter((v) => !v.ownSession).map((v) => v.columns.length))
  const legacyHeader = declared !== undefined && declared <= maxLegacy
  if (!legacyHeader) {
    // 自带会话列的一族：列数多的先试（越晚的版本列越多），证据任一成立即认。
    const own = versions.filter((v) => v.ownSession).sort((a, b) => b.columns.length - a.columns.length)
    for (const v of own) {
      if (c.length < v.columns.length) continue
      const sessionAt = v.read.sessionId?.at
      const sessionCell = sessionAt === undefined ? "" : c[sessionAt] ?? ""
      const aligned = declared === v.columns.length && c.length === v.columns.length
      if (sessionCell === "" || isSessionIdShape(sessionCell) || (ctx.poolSessions?.has(sessionCell) ?? false) || aligned) return v.ver
    }
  }
  // 旧版一族：取「装得下本行」的最高一代（装不下则退到最低一代——半截残行仍按最保守的读法读）。
  const legacy = versions.filter((v) => !v.ownSession).sort((a, b) => b.columns.length - a.columns.length)
  return (legacy.find((v) => v.columns.length <= c.length) ?? legacy[legacy.length - 1]).ver
}

// ---------------------------------------------------------------------------
// 在途表：位置读取 → 升级
// ---------------------------------------------------------------------------

const INFLIGHT_FIELDS = ["alias", "sessionId", "direction", "deliverable", "dispatchedAt", "round"] as const
type InflightField = (typeof INFLIGHT_FIELDS)[number]

/** 一行的**位置读取结果**：字符串原样、字段名按该代的声明；该代没有的列**不出现**（升级时补空）。 */
export type InflightFields = Partial<Record<InflightField, string>>

export interface ParsedInflight {
  /** 这一行的**代次**（显式、可断言）。 */
  ver: InflightVer
  fields: InflightFields
}

/** 解析一行在途的**格**（{@link tableBlock} 已切好格的路径）。返回 `{ ver, fields }`。
 *  `versions` 缺省即现行声明表——可传**扩表版**用于「加一列只动声明」的演练（见单测）。 */
export function parseInflightRow(c: readonly string[], ctx: InflightCtx = {}, versions: readonly InflightVersion[] = INFLIGHT_VERSIONS): ParsedInflight {
  const ver = selectInflightVer(c, ctx, versions)
  return { ver, fields: readFields(c, inflightVer(ver, versions).read) as InflightFields }
}

/** 解析**一整行**在途（先按 {@link cells} 切格）。ctx 缺省时按证据判（无表头声明、无池）——
 *  单测可直接喂一行。 */
export function parseInflightLine(line: string, ctx: InflightCtx = {}): ParsedInflight {
  return parseInflightRow(cells(line), ctx)
}

/** `Number(x) || 0` 的规范形（空串/缺列/非数字一律 0——半截写入的残行不阻断解析）。 */
const num = (v: string | undefined): number => Number(v) || 0

/**
 * **升级函数**：把某一代的行 `{ ver, fields }` 升级为现行形态（{@link Inflight}）。
 *
 * 这里是「新增列只需加一条版本声明」的落点：**该代没有的列在此补空**（如 v1/v2 无「会话」列 →
 * `sessionId: ""`，会话线索交由 `inflightSession` 经池解析）、未定型的字符串在此定型（数字）。
 * 因为「有哪些列」是从版本声明推出来的，本函数**不需要随新版本加分支**——新列若进了域对象，
 * 在 {@link INFLIGHT_FIELDS} 与 {@link inflightCells} 各加一处即可。
 */
export function migrateInflight(parsed: ParsedInflight): Inflight {
  // 该代没有这一列 → 字段不存在于 `parsed.fields` → 补空（而非 undefined）：对象形状与现行版一致，
  // 渲染→解析→渲染往返恒等。（`readFields` 只输出该代声明过的字段，故不需要再查一遍声明表。）
  const get = (f: InflightField): string => parsed.fields[f] ?? ""
  // 键序与历史实现一致（序列化结果逐字节同源，见等价性基线用例）
  return {
    alias: get("alias"),
    sessionId: get("sessionId"),
    direction: get("direction"),
    deliverable: get("deliverable"),
    dispatchedAt: num(get("dispatchedAt")),
    round: num(get("round")),
  }
}

/** 现行形态 → 格（写出侧）。与 {@link inflightHeader} 的列序**一一对应**——两处同源于
 *  {@link INFLIGHT_VERSIONS} 的现行条目，不会各写各的。 */
export function inflightCells(i: Inflight): string[] {
  return [i.alias, i.sessionId ?? "", i.direction, i.deliverable, String(i.dispatchedAt), String(i.round)]
}

// ---------------------------------------------------------------------------
// worker 池表：版本声明
// ---------------------------------------------------------------------------

/** 池表格式版本（与 {@link InflightVer} 同规：只加列）。 */
export type PoolVer = 1 | 2

/** 池表字段位置（两代**共用**——见 {@link POOL_TAILS_NOTE}）。 */
const POOL_READ: FieldMap = {
  alias: { at: 0 },
  sessionId: { at: 1 },
  domain: { at: 2 },
  // 任务列是自由文本：溢格只可能落在它身上（同类病也在在途表的方向/预期产出）
  task: { at: 3, overflow: true },
  // 时间/轮次/状态一律**自尾锚定**：行越长（被裸竖线撑开）越不能按固定下标取，否则取到的是正文。
  dispatchedAt: { fromEnd: 3 },
  round: { fromEnd: 2 },
  status: { fromEnd: 1 },
}

export interface PoolVersion {
  ver: PoolVer
  columns: readonly string[]
  /** 该代是否有「轮次」列（v1 没有——但**读取位置两代相同**，见下注）。 */
  hasRound: boolean
  read: FieldMap
}

/**
 * **池表两代共用同一组尾锚点**——这是**历史读法**，不是「按列名取」：
 * v1 没有「轮次」列，同一组锚点于是让 6 格 v1 行的「状态」位落到第 6 格、「派发时间」位落到第 5 格。
 * 本单只做**抽取**、不改语义，故 v1 的位置声明原样保留（已由用例「池表 v1 行的历史读法」钉死；
 * 要修正须单独一单 + 专门用例，且必须先想清「v1 6 列行」与「v2 行被截断」在这一格上无从区分）。
 * 对**现行版（v2）**而言这组锚点是正确的（7 列行：时间 c[len-3]、轮次 c[len-2]、状态 c[len-1]）。
 */
export const POOL_VERSIONS: readonly PoolVersion[] = [
  { ver: 1, columns: ["别名", "会话", "领域", "任务", "派发时间", "状态"], hasRound: false, read: POOL_READ },
  { ver: 2, columns: ["别名", "会话", "领域", "任务", "派发时间", "轮次", "状态"], hasRound: true, read: POOL_READ },
]

/** 现行池版本（写出侧永远用这一代）。 */
export const POOL_CURRENT: PoolVer = 2

const poolVer = (ver: PoolVer): PoolVersion => {
  const v = POOL_VERSIONS.find((x) => x.ver === ver)
  if (!v) throw new Error(`未知的池格式版本：${ver}`)
  return v
}

/** 现行池表头（渲染用）。 */
export const poolHeader = (): string[] => [...poolVer(POOL_CURRENT).columns]

/** 池表版本选择：**表头声明**即代次（缺表头时退回本行格数）。两代的差异只在「轮次」列的有无，
 *  而位置声明两代相同，故读出的值一致；版本号仍显式带出，供结果断言与日后按版本分支读取。 */
export function selectPoolVer(c: readonly string[], ctx: { declared?: number } = {}): PoolVer {
  const declared = ctx.declared ?? c.length
  return declared === POOL_VERSIONS.find((v) => !v.hasRound)!.columns.length ? 1 : 2
}

// ---------------------------------------------------------------------------
// worker 池表：位置读取 → 升级
// ---------------------------------------------------------------------------

const POOL_FIELDS = ["alias", "sessionId", "domain", "task", "dispatchedAt", "round", "status"] as const
type PoolField = (typeof POOL_FIELDS)[number]

export type PoolFields = Partial<Record<PoolField, string>>

export interface ParsedPool {
  ver: PoolVer
  fields: PoolFields
}

/** 解析一行池条目的**格** → `{ ver, fields }`。 */
export function parsePoolRow(c: readonly string[], ctx: { declared?: number } = {}): ParsedPool {
  const ver = selectPoolVer(c, ctx)
  return { ver, fields: readFields(c, poolVer(ver).read) as PoolFields }
}

/** 解析**一整行**池条目（先按 {@link cells} 切格）。 */
export function parsePoolLine(line: string, ctx: { declared?: number } = {}): ParsedPool {
  return parsePoolRow(cells(line), ctx)
}

/** **升级函数**：把某一代的池行升级为现行形态（{@link Worker}）。
 *  「状态」是**语义映射**（盘上是文本、域里是二值枚举）——缺省即 `running`（半截行不误报 idle）。 */
export function migratePool(parsed: ParsedPool): Worker {
  const f = parsed.fields
  return {
    alias: f.alias ?? "",
    sessionId: f.sessionId ?? "",
    domain: f.domain ?? "",
    task: f.task ?? "",
    dispatchedAt: num(f.dispatchedAt),
    round: num(f.round),
    status: f.status === "idle" ? "idle" : "running",
  }
}

/** 现行形态 → 格（写出侧）。 */
export function poolCells(w: Worker): string[] {
  return [w.alias, w.sessionId, w.domain, w.task, String(w.dispatchedAt), String(w.round), w.status]
}
