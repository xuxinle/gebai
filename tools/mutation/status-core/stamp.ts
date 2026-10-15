/**
 * 变异台的**证据指纹**：把「这台变异台是在什么源码上跑过的、跑出了什么」落成一个可核对的文件。
 *
 * ## 为什么需要它（本模块存在的唯一理由）
 * `bun run mutate:status` 是一条**得有人想起来才跑**的命令。忘了跑的代价不是「少跑一次」，而是
 * **下一次把判据改坏时没人拦得住**——矩阵照旧全绿，而「绿」已经不再代表守住了（理由见
 * DESIGN.md「测试策略 → 变异验证」：变异验证存在的全部意义就是「测试绿 ≠ 守住了」）。
 *
 * 指纹把「记得跑」换成一个**结构**：
 *   ① 执行器跑完且**全部符合期望**时，把「指纹面 + 实测结果」写进 `stamp.json`（**入库**，随提交走）；
 *   ② `packages/web/src/files/status-mutation-gate.test.ts` 每次常规测试都拿**当前源码**重算一遍，
 *      对不上就红——红的信息里直接给出补跑的那一条命令（人不必记得，测试会提醒）。
 * 于是「改了 status-core 却忘了跑变异台」从一个**静默的隐患**变成一个**当场的失败**。
 *
 * ## 三个刻意的设计点
 * 1. **内容确定（无时间戳、无 HEAD）**：同一份源码跑两次得到**逐字节相同**的 `stamp.json`——
 *    跑一次变异台不会平白弄脏工作区（本仓对构建产物有同款纪律：内容不变不写盘）；反过来，
 *    `git diff` 里**出现** stamp.json 的改动本身是一句可读信号：判据面变了、证据刚重立。
 * 2. **只在「跑全集 且 全部符合期望」时写**：`--only` 的部分跑、失败跑**都不许**刷新指纹——
 *    否则一次只跑一条的调试跑会把闸门刷绿，闸门当场变成摆设。写入口径由执行器把守（见 bench.ts）。
 * 3. **指纹面显式列出，且每一项都对应「为什么它变了就得重跑」**（`STAMP_INPUTS`）：被改坏的产品
 *    源码、判它的矩阵、变异体清单、执行器。四者任一改动都让上一次的结论失去依据——这正是要的语义。
 *
 * ## 边界（写清楚，免得被当成防伪）
 * 本机制守的是**防忘**，不是**防伪**：人手改 `stamp.json` 也能骗过它。它要解决的是「忘了跑」这件
 * 高频失误；「蓄意绕过」由 code review 守（`stamp.json` 里逐 kind 的实测条数就是给 review 看的：
 * 数字对不上清单，一眼就能发现）。
 *
 * ## 一处不放进指纹面的东西（有意为之）
 * 本文件（stamp.ts）与闸门测试自身**不进指纹面**：它们不参与判定，改它们不需要重跑变异台。
 * 而「偷偷把某个文件从指纹面里去掉」这条路是堵死的——判据侧另有一处**独立**的对账：
 * 闸门测试里写死了期望的指纹面（`EXPECTED_INPUTS`），少一个文件就红（两处独立，不得互推）。
 */
import { createHash } from "node:crypto"
import { readFileSync, writeFileSync } from "node:fs"
import { join } from "node:path"

/** 仓根：由本文件位置推出（`tools/mutation/status-core/` → 上溯三层），**不依赖 cwd**。 */
const REPO = join(import.meta.dir, "..", "..", "..")

/**
 * **指纹面**：这四份文件里任何一份变了，上一次的变异结论就不再成立，必须重跑。
 *
 * - `status-core.ts`：**被改坏的产品源码**——它就是被审的对象，动了当然要重量；
 * - `status-matrix.test.ts`：**判官**——判官换了，杀/活的判据就换了（同一批变异体可能换结论）；
 * - `mutants.ts`：**变异体清单**——新增一条变异体而没跑，等于新规则**没人审过**（这条最容易忘）；
 * - `bench.ts`：**执行器**——判定逻辑（怎么算「被杀」、基线怎么算）在它手里。
 */
export const STAMP_INPUTS = [
  "packages/web/src/files/status-core.ts",
  "packages/web/src/files/status-matrix.test.ts",
  "tools/mutation/status-core/mutants.ts",
  "tools/mutation/status-core/bench.ts",
] as const

/** 证据指纹的落盘位置（相对仓根；**入库**——它要随提交走，才谈得上「这份提交的证据」）。 */
export const STAMP_FILE = "tools/mutation/status-core/stamp.json"

/** 结构版本：将来改指纹算法或字段含义时递增（旧文件会被判为「结构不符」→ 按没证据处理）。 */
export const STAMP_VERSION = 1

/** 一个 `kind` 的实测结果：杀了几条 / 共几条。 */
export interface KindResult {
  /** 被杀死（判据/类型层认出来了）的条数。 */
  killed: number
  /** 该 kind 的条数。 */
  total: number
}

export interface Stamp {
  version: number
  /** 指纹面：文件（仓根相对路径）→ `sha256:<hex>`。 */
  files: Record<string, string>
  /** 清单条数（按 kind）——与当前 `MUTANTS` 对账，能发现「加了变异体没重跑」。 */
  mutants: Record<string, number>
  /** 实测结果（按 kind）——随指纹一起留在仓库里，读的人不必重跑就能看到那次数。 */
  result: Record<string, KindResult>
}

/**
 * 单个文件的 sha256（写成 `sha256:<hex>`；**缺失记为 `missing` 而不是抛错**——闸门要能报出「文件不见了」）。
 *
 * **哈希前先归一换行**（`CRLF` → `LF`）。这一条不是洁癖，缺了它闸门会在别的平台上**假红**：
 * 本仓 `core.autocrlf=true`（索引一律 LF、Windows 工作区检出为 CRLF，见 `.gitattributes`），
 * 于是同一份提交在 Windows 上读到的字节与在 WSL/Linux 上读到的**不同**——直接哈希内容，
 * 一次 Windows 上的跑会把指纹钉成 CRLF 版本，Linux 侧一 checkout 就判「文件已变」。
 * 归一后哈希的是**语义内容**（与行尾无关），跨平台同一份源码得到同一个值。
 */
export function hashFile(rel: string): string {
  try {
    const body = readFileSync(join(REPO, rel)).toString("utf8").replace(/\r\n/g, "\n")
    return `sha256:${createHash("sha256").update(body).digest("hex")}`
  } catch {
    return "missing"
  }
}

/** 当前指纹面（执行器与闸门共用这一份计算，避免「写的一处、读的另一处」两处实现漂移）。 */
export function computeFiles(): Record<string, string> {
  const out: Record<string, string> = {}
  for (const p of STAMP_INPUTS) out[p] = hashFile(p)
  return out
}

/** 读现有指纹；文件不存在、坏 JSON、结构非法（缺字段/版本不符）→ `null`（闸门按「没有证据」报，而不是崩）。 */
export function readStamp(): Stamp | null {
  let parsed: unknown
  try {
    parsed = JSON.parse(readFileSync(join(REPO, STAMP_FILE), "utf8"))
  } catch {
    return null
  }
  const s = parsed as Partial<Stamp> | null
  if (!s || typeof s !== "object") return null
  if (s.version !== STAMP_VERSION) return null
  if (!s.files || !s.mutants || !s.result) return null
  if (typeof s.files !== "object" || typeof s.mutants !== "object" || typeof s.result !== "object") return null
  return s as Stamp
}

/**
 * 指纹对不上的**逐文件**人话清单（全对 → 空数组）。
 *
 * 三个方向都要报，理由各不同：
 *   - 记录里有、当前算出不同 → 文件被改了（最常见：改了 `status-core.ts` 没重跑）；
 *   - 记录里有、当前集合里没有 → 指纹面被缩小了（旧证据覆盖的面比现在大，等于证据变弱）；
 *   - 当前集合里有、记录里没有 → 指纹面被扩大了（新纳入的文件从未被那次跑覆盖）。
 */
export function staleFiles(recorded: Record<string, string>): string[] {
  const current = computeFiles()
  const out: string[] = []
  for (const [p, h] of Object.entries(recorded)) {
    if (!(p in current)) out.push(`${p}：已不在指纹面内（旧证据覆盖的范围与现在不一致）`)
    else if (current[p] !== h) out.push(`${p}：内容已变（记录 ${short(h)} → 现在 ${short(current[p]!)}）`)
  }
  for (const p of Object.keys(current)) {
    if (!(p in recorded)) out.push(`${p}：新纳入指纹面，从未被那次跑覆盖过`)
  }
  return out
}

/** 指纹的人类可读短形态（前 8 位；报告里用）。 */
function short(h: string): string {
  return h.startsWith("sha256:") ? h.slice(7, 15) : h
}

/**
 * 写盘（**内容不变则不写**）：跑一次「源码没变」的变异台不该刷新 mtime、也不该弄脏工作区。
 * 返回是否真的写了——调用方把它打进输出（「已刷新」/「已是最新」），免得读者以为每次都写了。
 */
export function writeStamp(s: Omit<Stamp, "version">): { written: boolean } {
  // 键序固定（指纹面按 STAMP_INPUTS 顺序、kind 按传入顺序）→ 同样输入得到同样字节，可用 diff 读
  const files: Record<string, string> = {}
  for (const p of STAMP_INPUTS) files[p] = s.files[p]!
  const next: Stamp = { version: STAMP_VERSION, files, mutants: s.mutants, result: s.result }
  const body = `${JSON.stringify(next, null, 2)}\n`
  const path = join(REPO, STAMP_FILE)
  try {
    if (readFileSync(path, "utf8") === body) return { written: false }
  } catch {
    // 不存在 → 直接写
  }
  writeFileSync(path, body, "utf8")
  return { written: true }
}
