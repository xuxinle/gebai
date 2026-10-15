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
 * 3. **指纹面显式列出，且每一项都对应「为什么它变了就得重跑」**（`STAMP_ROOTS`）：被改坏的产品
 *    源码、判它的矩阵、变异体清单、执行器。四者任一改动都让上一次的结论失去依据——这正是要的语义。
 * 4. **指纹面 = 那几份根 + 它们的依赖闭包**（`closure.ts`）：根**引用**到的本仓源码同样在判定链上——
 *    把取词表抽成 `status-labels.ts`、把类型抽成 `status-types.ts` 之后，改坏**被 import 的那份**
 *    同样会变行为，而根的字节没变。缺了这半边就是一次**假绿**（闸门不红而矩阵的「绿」已不代表守住），
 *    故选自动收录而非人工登记。边界与豁免见 `closure.ts` 文件头与本文件 `EXEMPT_FROM_FINGERPRINT`。
 *
 * ## 边界（写清楚，免得被当成防伪）
 * 本机制守的是**防忘**，不是**防伪**：人手改 `stamp.json` 也能骗过它。它要解决的是「忘了跑」这件
 * 高频失误；「蓄意绕过」由 code review 守（`stamp.json` 里逐 kind 的实测条数就是给 review 看的：
 * 数字对不上清单，一眼就能发现）。
 *
 * ## 自助定位（闸门红了不必猜）
 * `tools/mutation/status-core/stale.ts`（`bun run mutate:status:why`）读这份指纹、**只报过期项**，不跑
 * 任何变异（毫秒级；退出码 0 = 不必重跑 / 1 = 要重跑，可直接写进脚本）。三层自助定位的完整口径与
 * 「判据面到底是哪四份」见 DESIGN.md「测试策略 → 变异验证 → 判据面 / 防忘闸门」。
 *
 * ## 一处不放进指纹面的东西（有意为之）
 * 本文件（stamp.ts）与闸门测试自身、以及 `closure.ts` / `stale.ts` **不进指纹面**：它们不参与判定，
 * 改它们不需要重跑变异台。而「偷偷把某个文件从指纹面里去掉」这条路是堵死的——判据侧另有一处**独立**
 * 的对账：闸门测试里写死了期望的根（`EXPECTED_ROOTS`）与期望的闭包（`EXPECTED_CLOSURE`），
 * 少一个文件就红（两处独立，不得互推）；闭包算法本身也由闸门侧在同一文件里用**合成依赖树**独立验过
 * （递归 / 环 / 边界 / 注释安全——见 `status-mutation-gate.test.ts` 的「依赖闭包」一组）。
 */
import { createHash } from "node:crypto"
import { readFileSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { REPO_ROOT, depClosure } from "./closure"

/** 仓根：由本文件位置推出（`tools/mutation/status-core/` → 上溯三层），**不依赖 cwd**。 */
const REPO = REPO_ROOT

/**
 * **判据面的四份根**：这几份里任何一份变了，上一次的变异结论就不再成立，必须重跑。
 *
 * - `status-core.ts`：**被改坏的产品源码**——它就是被审的对象，动了当然要重量；
 * - `status-matrix.test.ts`：**判官**——判官换了，杀/活的判据就换了（同一批变异体可能换结论）；
 * - `mutants.ts`：**变异体清单**——新增一条变异体而没跑，等于新规则**没人审过**（这条最容易忘）；
 * - `bench.ts`：**执行器**——判定逻辑（怎么算「被杀」、基线怎么算）在它手里。
 *
 * 这是**根**不是全部：指纹面 = 根 ∪ 它们的依赖闭包（`fingerprintInputs()`）。
 */
export const STAMP_ROOTS = [
  "packages/web/src/files/status-core.ts",
  "packages/web/src/files/status-matrix.test.ts",
  "tools/mutation/status-core/mutants.ts",
  "tools/mutation/status-core/bench.ts",
] as const

/**
 * **闭包成员的豁免表**（逐条写明理由；不是「放行表」——放行会让「过窄」无声发生）。
 *
 * 为什么需要它：闭包是**自动**递归收录的，而「记录层」这类文件确实被根引用、却**不参与判定**——
 * 把它们的改动也算成「结论作废」只会制造噪声红（改一句指纹文案就要等 30 秒），而会无故报红的门禁
 * 很快就会被绕过。豁免的代价如实写着：**方向是假红、不是假绿**（记录层真被改成与闸门不一致时，
 * 闸门会自己报「内容已变」）——这个方向是本仓明确选的那一侧（假红可忍、假绿不可忍）。
 *
 * 目前唯一一条 `→ stamp.ts`：算哈希、写 JSON、算闭包，**不参与「杀掉 vs 存活」的判定**。
 * 豁免是**逐文件**的判断，**不是把依赖图剪短的许可**：它引用的 `closure.ts`（闭包算法）不在豁免之列、
 * 照常进面。上一版的「豁免连子树一起剪」正是真仓闭包**恒为空集**的原因——`closure.ts` 被当成
 * `stamp.ts` 的子树剪掉，而它已是执行器 `bench.ts` 唯一能收到的本仓依赖（逐条可核：`bench.ts` 的两条
 * 依赖是 `mutants.ts`（根）与 `stamp.ts`（豁免），再无第三条）。依赖图短了、判据面就小了，
 * 而判据面小了是**假绿**那一侧——比豁免想省的那点噪声红严重得多。
 * 而「剪错了」不是靠信任拦住：闸门侧有**独立**的对账（豁免表逐字写死 + 闭包算法用合成树验），
 * 它一旦把真成员剪掉就红。
 */
export const EXEMPT_FROM_FINGERPRINT: Record<string, string> = {
  "tools/mutation/status-core/stamp.ts":
    "记录层（算哈希 / 写 JSON / 算依赖闭包），不参与「杀掉 vs 存活」的判定；进面只会让改一句指纹文案也要重跑 30 秒",
}

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
 *
 * `base` 可注入（同 `fingerprintInputs(input)` / `staleFiles(recorded, current)` / `depClosure(roots, exempt, base)`
 * 一脉的「拿合成树/副本走**真实现**」口子）：闸门侧复制一份**本仓副本**到临时目录、在副本上**真改文件**，
 * 断言「改一个被 import 的模块 → 指纹过期」（见 `status-mutation-gate.test.ts` 的「闭包拦截真生效」一组）
 * ——那里要的正是**生产口径**的哈希，而不是在测试里另写一份（口径分两处 = 迟早漂移）。
 * 缺省 `REPO`，故生产路径（`computeFiles()` / `writeStamp()`）行为逐字节不变。
 */
export function hashFile(rel: string, base: string = REPO): string {
  try {
    const body = readFileSync(join(base, rel)).toString("utf8").replace(/\r\n/g, "\n")
    return `sha256:${createHash("sha256").update(body).digest("hex")}`
  } catch {
    return "missing"
  }
}

/**
 * `fingerprintInputs()` 的**可注入输入**——只给闸门侧用：拿一棵**合成依赖树**走一遍**真实现的那个入口**。
 *
 * 为什么要留这个口子（对应一次真实存在的**退化路径**）：真仓当下的闭包是**空集**（四份根本下零依赖），
 * 于是「把实现退回只哈希根文件」在真仓里**看不见**——`EXPECTED_ROOTS` / `EXPECTED_CLOSURE`（写死为空）/
 * 「指纹面 = 根 + 闭包」三条对账全都会照旧绿。没有这个口子，那条退化就只能等到「有人再把判据拆出一份新
 * 模块」时才显形，而那时没人会想到来看这里（详见 `status-mutation-gate.test.ts` 的「防退化自检」一组）。
 *
 * 注入的是**输入**、不是判据：拼装逻辑（根 ∪ 闭包、去重、排序）仍只有下面 `fingerprintInputs()` 这一处实现，
 * 故不构成「自己验自己」。同一脉的既有做法见 `staleFiles(recorded, current)` 与 `closure.ts` 的
 * `depClosure(roots, exempt, base)`——那两处也是为「拿合成树验真实现」而留的口子。
 */
export interface FingerprintFaceInput {
  /** 根（默认 `STAMP_ROOTS`）。 */
  roots?: readonly string[]
  /** 豁免成员（默认 `EXEMPT_FROM_FINGERPRINT` 的键）。 */
  exempt?: Iterable<string>
  /** 基准目录（默认本仓根）；闸门侧传合成树所在目录。 */
  base?: string
}

/**
 * **当前指纹面**：根（`STAMP_ROOTS`）∪ 它们的依赖闭包（递归 import 图），减去豁免表里的成员。
 *
 * 这是**唯一**的指纹面计算处（执行器、闸门、自助定位三处都调它），故不存在「写的一处、读的另一处」漂移。
 * 顺序确定：根按声明序在前，闭包成员排序在后——指纹文件的键序因此逐字节稳定，可 `git diff` 读。
 *
 * `input` 缺省即「真仓当下的面」；闸门侧传合成树的 `{ roots, exempt, base }`（见 `FingerprintFaceInput`）。
 */
export function fingerprintInputs(input: FingerprintFaceInput = {}): string[] {
  const roots = input.roots ?? STAMP_ROOTS
  const exempt = [...(input.exempt ?? Object.keys(EXEMPT_FROM_FINGERPRINT))]
  const skip = new Set(exempt)
  const closure = depClosure(roots, exempt, input.base ?? REPO)
  return [...roots, ...closure.filter((p) => !skip.has(p))]
}

/**
 * 当前指纹面（执行器与闸门共用这一份计算，避免「写的一处、读的另一处」两处实现漂移）。
 *
 * `base` 可注入（同上 `hashFile(rel, base)`）——闸门侧拿它把一份**仓副本**走一遍**生产口径**的快照计算。
 * 缺省 `REPO`，故执行器 / 自助定位那两处调用行为逐字节不变。
 */
export function computeFiles(base: string = REPO): Record<string, string> {
  const out: Record<string, string> = {}
  for (const p of fingerprintInputs({ base })) out[p] = hashFile(p, base)
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
 *   - 当前集合里有、记录里没有 → 指纹面被扩大了（新纳入的文件从未被那次跑覆盖）——**依赖闭包的新成员走的正是这一支**：
 *     根的 import 图里多出一份本仓源码（或被引用那份本身刚变），不管它是新加的、先前被豁免、
 *     还是**在面外被改了**，都在这里当场红。闸门不必区分是哪一种，它只知道「这个集合与那次跑不一致」。
 *
 * `current` 可注入（默认现算）：闸门侧要拿**合成依赖图**验证这一支真的覆盖了「被引用文件改了」这件事。
 */
export function staleFiles(recorded: Record<string, string>, current: Record<string, string> = computeFiles()): string[] {
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
  // 键序固定（指纹面按 `fingerprintInputs()` 顺序、kind 按传入顺序）→ 同样输入得到同样字节，可用 diff 读
  const files: Record<string, string> = {}
  for (const p of fingerprintInputs()) files[p] = s.files[p] ?? hashFile(p)
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
