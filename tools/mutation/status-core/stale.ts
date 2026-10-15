#!/usr/bin/env bun
/**
 * **自助定位**：判据面到底变了没有、要不要重跑那台 30 秒的变异台——一条命令问清楚，不跑任何变异。
 *
 * 用途：`bun run mutate:status:why`（仓根；等价于 `bun run tools/mutation/status-core/stale.ts`）。
 * 退出码：**0 = 指纹新鲜（不必重跑）**；**1 = 指纹过期或缺失（要跑 `bun run mutate:status`）**。
 * 这个语义刻意与常规命令相反（`0` 在这里是「没事可做」），为的是能直接写进脚本的 `if` 里。
 *
 * ## 它回答什么、不回答什么
 * 它**只**回答「该不该重跑」，不回答「守没守住」——后者只有真跑那台才知道（会把源码改坏在临时副本上
 * 跑矩阵 + `tsc`）。故它是**分流器**：红的时候先问它，是「指纹过期」（跑一次就好）还是「结论不该信」
 * （那才要人去看）。
 *
 * ## 三种输出，各对应一类真实处境
 *   ① **指纹新鲜**：判据面四份与那一次跑逐字节同源，且清单条数对得上 → 不必跑；
 *   ② **指纹过期**：逐份列出「记录值 → 当前值」，并说明哪些文件在面外被动了（面被缩/被扩也报）；
 *   ③ **没有指纹**：`stamp.json` 缺失/结构不符 → 本仓从未跑过（或结构版本升了），先跑一次立证据。
 *
 * ## 为什么它不进判据面
 * 它**只读**指纹、不参与判定：改了它的输出措辞不该要人重跑那台（同 `stamp.ts` 与闸门测试，
 * 见 `stamp.ts` 文件头「一处不放进指纹面的东西」）。把它加进面内只会制造噪声红。
 */
import { STAMP_FILE, STAMP_INPUTS, computeFiles, readStamp, staleFiles } from "./stamp"
import { MUTANTS } from "./mutants"

/** 补跑命令（失败信息里逐字给出来——人不该需要记得它长什么样）。 */
const RERUN = "bun run mutate:status"

/** 指纹的人类可读短形态（前 8 位，与 stamp.ts 的报告口径一致）。 */
const short = (h: string): string => (h.startsWith("sha256:") ? h.slice(7, 15) : h)

const stamp = readStamp()

if (!stamp) {
  console.log(`❌ 没有可用的证据指纹（${STAMP_FILE} 缺失或结构不符）`)
  console.log(`   含义：本仓从未跑过这台变异台（或指纹结构版本升了），当前**没有任何可对账的结论**。`)
  console.log(`   怎么办：跑 \`${RERUN}\`（约 30 秒，退出码 0 = 全部符合期望），它会写下指纹。`)
  process.exit(1)
}

/* ---- ① 判据面逐份对账（含「面被缩小/扩大」两个方向） ---- */
const stale = staleFiles(stamp.files)
const current = computeFiles()

/* ---- ② 清单条数对账：容易忘的一条是「发现了新缺陷、补了一条变异体」 ---- */
const currentCounts: Record<string, number> = {}
for (const m of MUTANTS) currentCounts[m.kind] = (currentCounts[m.kind] ?? 0) + 1
const kinds = [...new Set([...Object.keys(stamp.mutants), ...Object.keys(currentCounts)])].sort()
const countDrift = kinds.filter((k) => (stamp.mutants[k] ?? 0) !== (currentCounts[k] ?? 0))

if (stale.length === 0 && countDrift.length === 0) {
  console.log("✅ 指纹新鲜：判据面四份与那次跑逐字节同源，变异体条数也对得上——不必重跑。")
  console.log(`   指纹记着：${kinds.map((k) => `${k} 杀 ${stamp.result[k]?.killed ?? "?"}/${stamp.result[k]?.total ?? "?"}`).join("、")}`)
  console.log(`   判据面（${STAMP_INPUTS.length} 份）：`)
  for (const p of STAMP_INPUTS) console.log(`     - ${p}  ${short(current[p]!)}`)
  process.exit(0)
}

console.log("❌ 指纹过期：上一次的结论不再代表当前源码——请重跑那台。")
console.log(`   命令：\`${RERUN}\`（约 30 秒，退出码 0 = 全部符合期望并自动刷新指纹）\n`)

if (stale.length > 0) {
  console.log("   判据面（改了其中任何一份都要重跑；改的是注释也算）：")
  for (const s of stale) console.log(`     - ${s}`)
  console.log("")
}
if (countDrift.length > 0) {
  console.log("   变异体条数变了（最常见的原因：发现了新缺陷、补了一条变异体 —— 它还没被审过）：")
  for (const k of countDrift) console.log(`     - ${k}：记录 ${stamp.mutants[k] ?? 0} 条 → 现在 ${currentCounts[k] ?? 0} 条`)
  console.log("")
}
console.log("   为什么不能「看着没问题就算了」：这次改动可能正好让某条规则**没有任何断言在守**，")
console.log("   而矩阵全绿恰恰是那种情况的样子——变异台是唯一能把它试出来的东西。")
process.exit(1)
