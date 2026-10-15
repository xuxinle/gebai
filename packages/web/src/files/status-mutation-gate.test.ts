/**
 * **变异台防忘闸门**：改了 `status-core` 判据面却忘了跑变异台，常规测试当场红。
 *
 * ## 为什么需要它（这道测试自己就是「防忘机制」）
 * `bun run mutate:status`（33 条变异体、约 30 秒）是一条**得有人想起来才跑**的命令。忘了跑的
 * 代价不是「少跑一次」，而是**判据再被改坏时没人拦得住**——矩阵照旧全绿，而「绿」已经不再
 * 代表守住了（这正是变异验证存在的理由，见 DESIGN.md「测试策略 → 变异验证」）。
 * 把「记得跑」交给纪律，历史证明会失败；交给**结构**才靠得住，做法是：
 *
 *   ① 变异台跑成功时把「指纹面 + 实测结果」写进 `tools/mutation/status-core/stamp.json`（入库）；
 *   ② 本文件每次 `bun test` 都用**当前源码**重算指纹，与 ① 对账——对不上就红，
 *      并在失败信息里直接给出补跑的那条命令。
 *
 * 这不是「把变异台接进 CI」（那件事本仓**明确不做**：CI 满载的抖动会把测试抖动失败记成
 * 「变异被杀」，在 `code` 类上就是一次**假通过**——判定可信度比自动化收益重要）。本闸门很轻：
 * 只做**哈希对账**，不 spawn 任何子进程，毫秒级，也不会受抖动影响（哈希没有抖动）。
 *
 * ## 它报什么（三种红，各自对应一类真实遗忘）
 *   1. **指纹过期**：`status-core.ts` / 矩阵 / 变异体清单 / 执行器 有一样变了而指纹没刷
 *      → 改了判据、或加了变异体、或改了判定逻辑，都没重跑；
 *   2. **没有指纹**：`stamp.json` 缺失/结构不符 → 这台从未在本仓跑过（或结构版本升了），先跑一次；
 *   3. **实测结果与清单对不上**：例如清单里 21 条 `code` 却记着杀掉 20 条 → 有人手改了指纹。
 *
 * ## 边界（写清楚，别把它当防伪）
 * 它守**防忘**，不守**防伪**：手改 `stamp.json` 也能骗过（第 3 条只是让这种改动容易被看出来）。
 * 蓄意绕过交给 code review——`stamp.json` 里逐 kind 的条数就是给 review 看的数字。
 *
 * ## 为什么这个文件叫 `.test.ts`（与变异台文件**刻意相反**）
 * 变异台本体（`bench.ts` / `mutants.ts` / `stamp.ts`）必须**不带** `.test` 标记，否则 `bun test`
 * 会把它当用例跑起来、spawn 几十秒。本文件恰好相反：它**就是要**被 `bun test` 采到——它不 spawn
 * 任何东西，只读判据面那几份文件算哈希（外加沿依赖走一遍闭包，见文末那组）。名字是这里的关键，
 * 改动前先想清楚这一点。
 */
import { describe, expect, test } from "bun:test"
import { createHash } from "node:crypto"
import { existsSync, readFileSync } from "node:fs"
import { join, posix } from "node:path"
import {
  MATRIX_TEST,
  MUTANTS,
  TARGET_FILE,
  expectsKill,
} from "../../../../tools/mutation/status-core/mutants"
import { STAMP_FILE, STAMP_INPUTS, computeFiles, readStamp, staleFiles } from "../../../../tools/mutation/status-core/stamp"

/* ------------------------------------------------------------------ *
 * 判据面的成员（路径常量集中在此，下面几组共用；语义见文末「判据面定义」一组）
 * ------------------------------------------------------------------ */

/** 变异体清单（**数据**：33 条变异体；新增一条而没跑台 = 新规则没人审过）。 */
const MUTANTS_FILE = "tools/mutation/status-core/mutants.ts"
/** 执行器（**判定逻辑**：基线怎么算、怎么判「被杀」、行尾归一在哪一行）。 */
const BENCH_FILE = "tools/mutation/status-core/bench.ts"
/** 指纹实现自身（**只记录结论、不参与判定**——故刻意不在指纹面内）。 */
const STAMP_TS = "tools/mutation/status-core/stamp.ts"
/** 本文件（闸门自身——同样刻意不在面内：改闸门不该要求重跑那台 30 秒的台）。 */
const GATE_FILE = "packages/web/src/files/status-mutation-gate.test.ts"

/**
 * 指纹面的**独立对账**（不得由 stamp.ts 的 `STAMP_INPUTS` 反推——那等于自己验自己）。
 *
 * 为什么要在这里写死：否则「把某个文件从指纹面里去掉」就成了绕过闸门的最短路径——改一行
 * `STAMP_INPUTS`，那文件从此改坏也不再触发重跑。两处独立列出后，去掉一个就红在这里。
 * 新增指纹面成员时**两处都要加**（这处是判据，stamp.ts 那处是实现，同 AGENTS.md 的口径）。
 */
const EXPECTED_INPUTS: string[] = [TARGET_FILE, MATRIX_TEST, MUTANTS_FILE, BENCH_FILE]

/** 补跑命令（失败信息里逐字给出来——人不该需要记得它长什么样）。 */
const RERUN = "bun run mutate:status"
const REPO = join(import.meta.dir, "..", "..", "..", "..")

describe("变异台防忘闸门：判据面一变，常规测试就该红", () => {
  test("指纹面与判据侧写死的清单逐字一致（少一项 = 那条路径的改动将静默逃过闸门）", () => {
    const declared: string[] = [...STAMP_INPUTS]
    expect(declared.sort(), "指纹面被改动过：新增/删除成员必须同步本文件的 EXPECTED_INPUTS").toEqual(
      [...EXPECTED_INPUTS].sort(),
    )
  })

  test("指纹文件入库且结构合法（缺失 = 本仓从未跑过那台，或结构版本升了）", () => {
    expect(existsSync(join(REPO, STAMP_FILE)), `缺少 ${STAMP_FILE}：先跑一次 \`${RERUN}\` 把证据立起来`).toBe(true)
    const stamp = readStamp()
    expect(stamp, `${STAMP_FILE} 读不出（坏 JSON / 结构版本不符）：跑 \`${RERUN}\` 重新生成`).not.toBeNull()
    expect(Object.keys(stamp!.files).length, "指纹面为空 = 这份证据什么也没覆盖").toBe(EXPECTED_INPUTS.length)
  })

  test("指纹未过期：判据面四份文件与那一次跑时逐字节同源", () => {
    const stamp = readStamp()
    expect(stamp, `缺少可用的 ${STAMP_FILE}：跑 \`${RERUN}\``).not.toBeNull()
    const stale = staleFiles(stamp!.files)
    expect(
      stale,
      `判据面在变异台跑过之后被改动了，而变异台没有重跑——上一次的「21/21 杀」不再代表当前源码。\n` +
        `  改了什么：\n${stale.map((s) => `    - ${s}`).join("\n")}\n` +
        `  怎么办：跑 \`${RERUN}\`（约 30 秒，退出码 0 = 全部符合期望），它会自动刷新指纹。\n` +
        `  为什么不能只是「看着没问题就算了」：这次改动可能正好让某条规则**没有任何断言在守**，` +
        `而矩阵全绿恰恰是那种情况的样子——变异台是唯一能把它试出来的东西。`,
    ).toEqual([])
  })

  test("指纹里的清单条数与当前清单一致（加了变异体却没重跑，也会被这里抓住）", () => {
    const stamp = readStamp()
    expect(stamp).not.toBeNull()
    const current: Record<string, number> = {}
    for (const m of MUTANTS) current[m.kind] = (current[m.kind] ?? 0) + 1
    expect(stamp!.mutants, `变异体清单的条数变了而指纹没刷：跑 \`${RERUN}\``).toEqual(current)
  })

  test("指纹里的实测结果与清单自洽（杀掉数不得超过条数、条数不得为 0）", () => {
    const stamp = readStamp()
    expect(stamp).not.toBeNull()
    const mutants = stamp!.mutants
    for (const [kind, n] of Object.entries(mutants)) {
      const r = stamp!.result[kind]
      expect(r, `指纹里 \`${kind}\` 有清单条数却没有实测结果——手改过？跑 \`${RERUN}\` 重立`).toBeDefined()
      expect(n, `\`${kind}\` 的清单条数为 0：没有变异体的类别不该出现在指纹里`).toBeGreaterThan(0)
      expect(r!.total, `\`${kind}\` 的实测条数（${r!.total}）与清单条数（${n}）不一致`).toBe(n)
      expect(r!.killed, `\`${kind}\` 杀掉数（${r!.killed}）超过条数（${r!.total}）`).toBeLessThanOrEqual(r!.total)
    }
  })

  test("实测结果与「期望」一致（这是指纹里唯一能被独立判对错的一项，不是自说自话）", () => {
    const stamp = readStamp()
    expect(stamp).not.toBeNull()
    for (const [kind, r] of Object.entries(stamp!.result)) {
      const want = expectsKill(kind as (typeof MUTANTS)[number]["kind"])
      const got = r.killed === r.total
      // expectations 只有两种：全杀 / 全存活（`benign` 被误杀与 `code` 逃逸都不许藏在「大多数」里）
      expect(
        want === got,
        `指纹记着 \`${kind}\` 杀 ${r.killed}/${r.total}，与该 kind 的期望不符` +
          `（期望${want ? "全杀" : "全存活"}）——这份指纹是失败状态下写出来的？跑 \`${RERUN}\` 重立。`,
      ).toBe(true)
    }
  })
})

describe("变异台防忘闸门：变异体清单在**任意行尾**的工作区里都能命中（潜伏缺陷回归锁）", () => {
  /**
   * 为何单列这一组：变异台的 `find` 串是写在 `mutants.ts` 里的**字面量**（其中的 `\n` 是裸 LF、
   * 不可能是 CRLF），而本仓 `core.autocrlf=true`——**Windows 工作区里 `status-core.ts` 是 CRLF**
   * （见 `.gitattributes`）。于是不归一时的后果：**所有跳行的 `find` 一律未命中**——实测 33 条里
   * 有 **9 条** 这样失效（本单收尾时用 `--only A8` 才暴露出来）。更坏的形态是「新加一条跳行
   * 变异体，`--only` 调试时报“未命中”」——人很容易把它读成「源码改过了」而不是「工具坏了」。
   *
   * 这里用**行尾无关**的判据把它锁住：① 把源码与 `find` 都归一到 LF 后逐条对账（守「清单与源码一致」）；
   * ② 断言「不归一时真的会对不上」（证明这件事真在发生）；③ 归一后必须全中（证明归一这个做法够用）；
   * ④ 源码级契约看住 `bench.ts` 里那一行归一（删了它，测试**仍然全绿**，只有真在 Windows 上跑才会发现）。
   */
  const norm = (s: string): string => s.replace(/\r\n/g, "\n")
  const core = readFileSync(join(REPO, TARGET_FILE), "utf8")

  test("每条 `find` 在 status-core.ts 里恰好命中一次（归一后）——源码改了而清单没跟，这里先报", () => {
    const bad: string[] = []
    for (const m of MUTANTS) {
      const n = norm(core).split(norm(m.find)).length - 1
      if (n !== 1) bad.push(`${m.name}：命中 ${n} 次`)
    }
    expect(bad, "这些变异体的 `find` 不再恰好命中一次（源改了没跟清单，或清单写了不存在的串）").toEqual([])
  })

  test("归一前的对照：**不归一**时确实有跳行条目对不上（证明本组测试在守一件真事）", () => {
    /* 本机工作区是 CRLF（`core.autocrlf=true`）时不归一会成片失效；LF 工作区（WSL/Linux）则不。
     * 两种都合法——所以只断言「若工作区是 CRLF，就必须有对不上的」，避免在 LF 机上假红。 */
    const rawCount = MUTANTS.filter((m) => core.split(m.find).length - 1 === 1).length
    const isCrlf = /\r\n/.test(core)
    if (isCrlf) {
      expect(rawCount, "CRLF 工作区却不归一也能全命中？这条测试的前提错了，请核 `git ls-files --eol`").toBeLessThan(
        MUTANTS.length,
      )
    }
    expect(rawCount, "不归一时的命中数不得超过总数（计数口径错了）").toBeLessThanOrEqual(MUTANTS.length)
  })

  test("归一是**充分**的：把 CRLF 工作区归一后，全部 `find` 都命中", () => {
    /* 口径：造一份 CRLF 版源码（模拟 Windows 检出），断言归一后 33 条全命中。
     * 这条守的是「归一这个做法够用」——实测 9 条跳行的 `find` 靠它才能对上。 */
    const lf = norm(core)
    const crlf = lf.replace(/\n/g, "\r\n")
    expect(crlf, "构造 CRLF 副本失败（本就没 LF？）").not.toBe(lf)
    const count = (hay: string, needle: string): number => hay.split(needle).length - 1
    const naive = MUTANTS.filter((m) => count(crlf, m.find) === 1).length
    const normalized = MUTANTS.filter((m) => count(norm(crlf), m.find) === 1).length
    expect(normalized, "归一后仍有 find 未命中（清单与源码不一致，不是行尾问题）").toBe(MUTANTS.length)
    expect(naive, "CRLF 工作区下**不归一就全命中**？那本仓的工作区不是 CRLF，这条测试失去意义——请核 `git ls-files --eol`").toBeLessThan(MUTANTS.length)
  })

  test("执行器确实对**读入的源码**做了行尾归一（源码级契约：删了这一行，Windows 上跳行变异体会成片失效）", () => {
    /* 为何要看 bench.ts 的源码：那行归一发生在脚本内部，外部看不到。删了它，测试**仍然全绿**
     * （因为清单与源码本身是一致的）——只有真在 Windows 工作区跑那台才会发现。本仓已在
     * style-contract.test.ts 里用同款「源码级契约」守过类似的事（静默退化的形态）。 */
    const bench = readFileSync(join(REPO, "tools/mutation/status-core/bench.ts"), "utf8")
    expect(bench, "取不到 bench.ts = 这道契约会空转").toContain("countOccurrences")
    expect(
      /const\s+origCore\s*=\s*norm\(readFileSync\(CORE_SRC, "utf8"\)\)/.test(bench),
      "bench.ts 读入 `status-core.ts` 后没有做行尾归一——Windows 工作区（CRLF）下所有跳行的 `find` 会未命中",
    ).toBe(true)
    expect(
      /replace\(\/\\r\\n\/g,\s*"\\n"\)/.test(bench),
      "归一函数不见了（应为 `\\r\\n` → `\\n`）——它与 stamp.ts 的 hashFile 必须同口径",
    ).toBe(true)
  })

  test("变异体清单里确实存在跳行的 `find`（否则上面四条是在空转）", () => {
    const multiline = MUTANTS.filter((m) => m.find.includes("\n"))
    expect(multiline.length, "没有跳行的 find = 看不出行尾问题，需换个例子来验这件事").toBeGreaterThan(0)
  })
})

describe("变异台防忘闸门：证据指纹机制自身可复核", () => {
  test("哈希口径是**行尾归一后**的内容（跨平台一致：Windows 工作区 CRLF / WSL·Linux LF 得同一个值）", () => {
    /* 为什么单测这一条：本仓 `core.autocrlf=true`，同一份提交在两个平台上读出的**字节不同**。
     * 若哈希直接吃原始字节，Windows 上钉的指纹会在 Linux checkout 后**假红**——而一个会无故报红
     * 的门禁很快就会被绕过（DESIGN「为什么不进 CI」说的就是这个代价）。这条不是复述实现：
     * 它拿指纹面里的一份文件，分别算「原始字节」与「行尾归一后」两个哈希，断言记录的是后者。 */
    const raw = readFileSync(join(REPO, MATRIX_TEST), "utf8")
    const rawHash = `sha256:${createHash("sha256").update(raw).digest("hex")}`
    const normalizedHash = `sha256:${createHash("sha256").update(raw.replace(/\r\n/g, "\n")).digest("hex")}`
    const recorded = computeFiles()[MATRIX_TEST]
    expect(recorded, "指纹面里应含判据矩阵").toBeDefined()
    expect(recorded, "指纹口径必须是行尾归一后的内容（否则另一平台 checkout 即假红）").toBe(normalizedHash)
    if (rawHash !== normalizedHash) {
      // 本机工作区确实是 CRLF：归一没生效时这条会红，故它真的在守这件事
      expect(recorded, "CRLF 工作区下未归一（直接哈了原始字节）").not.toBe(rawHash)
    }
  })

  test("指纹面里的每一份文件都真实存在于仓库（路径写错会让闸门空转）", () => {
    for (const p of EXPECTED_INPUTS) {
      expect(existsSync(join(REPO, p)), `指纹面成员不存在：${p}`).toBe(true)
    }
  })

  test("指纹只记语义内容、不记时间与 HEAD（同一份源码跑两次得逐字节相同的文件）", () => {
    /* 为什么守这条：指纹文件是**入库**的。若它带时间戳/HEAD，跑一次变异台就平白弄脏工作区，
     * 回回都得提交一次——这种噪声会让人很快就不耐烦看它。内容确定后，`stamp.json` 出现在
     * `git diff` 里本身就是一句可读信号：「判据面变了，证据刚重立」。 */
    const text = readFileSync(join(REPO, STAMP_FILE), "utf8")
    const parsed = JSON.parse(text) as Record<string, unknown>
    expect(Object.keys(parsed), "指纹里出现了预期外的字段（多办是时间戳/HEAD 这类非确定字段）").toEqual([
      "version",
      "files",
      "mutants",
      "result",
    ])
    expect(JSON.stringify(JSON.parse(JSON.stringify(parsed)))).toBe(JSON.stringify(parsed))
  })

  test("本文件自己不进指纹面（闸门改动不该要求重跑变异台）", () => {
    expect(EXPECTED_INPUTS.includes(GATE_FILE), "闸门自身进了指纹面 = 改闸门也要重跑那台，无谓的成本").toBe(false)
    expect(readFileSync(import.meta.path, "utf8").length).toBeGreaterThan(0)
  })
})

/* ================================================================== *
 * 判据面定义：它是「哪些文件算在内」+「为何过宽/过窄都不可接受」
 * ================================================================== */

/**
 * ## 判据面 = 指纹面（`EXPECTED_INPUTS` 那四份）
 *
 * 「判据面」是**四份文件的并集**，每份各自对应一类「改了它，上一次的结论就作废」：
 *
 * | # | 文件 | 在判据里充当什么 | 改了不重跑会怎样 |
 * |---|------|----------------|----------------|
 * | ① | `packages/web/src/files/status-core.ts` | **被审对象**（全部 33 条变异的施加目标） | 改坏判据而无人发现：矩阵照旧全绿，而「绿」不再代表守住了 |
 * | ② | `packages/web/src/files/status-matrix.test.ts` | **判官**（杀/活的唯一判据来源，34 用例 / 511 断言） | 判官换了判据也换了，同一批变异体可能换结论——旧结论不再可比 |
 * | ③ | `tools/mutation/status-core/mutants.ts` | **变异体清单**（33 条，本身就是历史缺陷清单） | 新增一条变异体而没跑 = 那条新规则**从未被审过**（最容易忘的一条） |
 * | ④ | `tools/mutation/status-core/bench.ts` | **执行器**（基线怎么算、怎么判「被杀」、行尾归一在哪一行） | 判定逻辑变了，写进 stamp 的旧数字不再由同一套判法产出的 |
 *
 * **按内容判定，不按文件名判定**：判据面成员只要**字节**变了就算（哈希不区分注释与代码）——改一句
 * 注释也得重跑。这看着笨，但「哪几行算语义」本身要维护一张口径表，而口径每宽/窄一寸都要付错价的
 * 代价（见下）。
 *
 * ## 改完判据面怎么自助定位该跑什么
 * 闸门红了**不用猜**：失败信息里逐条给出「哪份文件、记录什么值、现在算出什么值」，并直接附上补跑
 * 命令；拿不准时还有一条零依赖的自查命令（读指纹、只报过期项，不跑任何变异）——两处都写在
 * `DESIGN.md`「测试策略 → 防忘闸门 → 自助定位」里。
 *
 * ## 为何刻意**不**放进面的
 * - `status-mutation-gate.test.ts`（本文件）与 `stamp.ts`：它们**不参与判定**，只记录与对账结论；
 *   改一句失败文案就要重跑 30 秒的台，没人受得了（且很快就会被绕过）。
 * - `status-core.test.ts`（语义契约测试）、`style-contract.test.ts`（接线契约）：变异台**不跑它们**
 *   （它只跑矩阵 + `tsc`），故它们变了不影响这台台的结论。
 * - `DESIGN.md` / `AGENTS.md`：文档随判据一起改是**正常**的，要文档一变就重跑，闸门就成噪声源了。
 *
 * ## 两个方向的价都写过：过宽 vs 过窄
 * - **过窄 = 假绿**（比过宽严重得多）：漏进去的那份改了却不红，让人误以为「刚验过」；
 * - **过宽 = 噪声红**：把不影响判定的文件算进来，每改一次无关文件都要等 30 秒——而会无故报红的
 *   门禁很快就会被绕过（本仓已在 DESIGN「为什么不进 CI」里写过这个代价）。
 *
 * 下列三条把这个定义钉在**可自动核验的判据**上（否则它就只是文档里的一句话，而没人会在改代码时去翻）：
 * ① **依赖闭包自足**：面内每份文件真正引用的文件，要么也在面内、要么在**显式豁免表**里写明理由——
 *    「过窄」就此从「事后才发现」变成「新增依赖当场红」；
 * ② **清单与实算一致**：写死的依赖表 `KNOWN_DEPS` 与源码实算逐项相同（防它随源码漂移而变空话）；
 * ③ **面外文件不影响闸门**：面是精确的这四份、不多不少（过宽侧的边界，内含「四份都必须真在面内」的反向检查）。
 */
describe("判据面定义：依赖闭包自足（过窄=假绿，这里先红）", () => {
  const normNL = (s: string): string => s.replace(/\r\n/g, "\n")
  /** 去注释后再扫，避免注释里的示例串被当成真依赖。 */
  const strip = (s: string): string => normNL(s).replace(/\/\*[\s\S]*?\*\//g, "").replace(/^[ \t]*\/\/.*$/gm, "")

  /**
   * 一份文件**真正引用的仓内文件**（判定相关依赖的实算口径）。
   *
   * 证据只认两种，因为本仓只存在这两种形态，且它们都要求写出**可解析的字面量**：
   *   - `from "…"`——ESM 引用（判官导入被审源码、执行器导入清单与指纹）；
   *   - 源码里的 `"….ts"` 字面量——`readFileSync(join(import.meta.dir, "status-core.ts"))` 这种读法。
   *
   * 不做完整 AST：这两条已盖住本仓全部真实依赖，而「认一切像路径的串」漏口更大（路径一旦装进变量
   * 就漏，等于自欺）。解析不到文件的一律不算（`bun:test` / `node:fs` / 包名——改了它们不会让本仓
   * 某份源码变样）。**新增依赖时会以「清单与实算不一致」红**，那时人工把它写进 `KNOWN_DEPS`
   * （要判「进面并跑台」还是「豁免并写理由」）——那份写死的表就是「想过了」的证据。
   */
  function staticDeps(abs: string, relPath: string): string[] {
    const src = strip(readFileSync(abs, "utf8"))
    const dir = posix.dirname(relPath)
    const specs = new Set<string>()
    for (const m of src.matchAll(/\bfrom\s*"([^"]+)"/g)) specs.add(m[1]!)
    for (const m of src.matchAll(/"([^"]*\.ts)"/g)) specs.add(m[1]!)

    const out: string[] = []
    for (const spec of specs) {
      /* 相对说明符（`./status-core`）与裸文件名（`"status-core.ts"`）都按**本文件所在目录**解析——
       * 这正是判官读被审源码的方式（`join(import.meta.dir, "status-core.ts")`）。 */
      const joined = posix.join(dir, spec)
      const hit = [joined, `${joined}.ts`, `${joined}/index.ts`].find(
        (c) => !c.startsWith("..") && existsSync(join(REPO, c)),
      )
      if (hit) out.push(posix.normalize(hit))
    }
    return [...new Set(out)]
  }

  /**
   * 依赖清单（**写死的期望**，逐份对应上面的「判据面定义」表）。
   *
   * - `MATRIX_TEST → TARGET_FILE`：判官引用被审对象（`import … from "./status-core"` 加源码字面量）；
   * - `BENCH_FILE → MUTANTS_FILE`：执行器读清单（判定逻辑与数据的接口）；
   * - `BENCH_FILE → STAMP_TS`：执行器把结论写进指纹——**这条是豁免**（见下 `OUT_OF_FACE_OK`）。
   */
  const KNOWN_DEPS: Record<string, string[]> = {
    [TARGET_FILE]: [],
    [MATRIX_TEST]: [TARGET_FILE],
    [MUTANTS_FILE]: [],
    [BENCH_FILE]: [MUTANTS_FILE, STAMP_TS],
  }

  /**
   * **显式豁免**：依赖在面外、但经判断**不需要**跟着重跑，逐条写明理由。
   *
   * 为什么要有这张表而不是直接放行：放行 = 「过窄」无声发生（新增依赖照样全绿）。豁免要求写出理由，
   * 于是「这台变异台的判据面为什么是这四份」在代码里就是可复核的，而不是散在文档里的一句话。
   *
   * 目前唯一一条 `BENCH_FILE → STAMP_TS`：`stamp.ts` 是**记录层**（算哈希、写 JSON），它不参与
   * 「杀掉 / 存活」的判定——改它的哈希口径不会让任何一条变异体换结论。反过来把它放进面内还会
   * 制造**噪声红**：改一句指纹的失败文案就要重跑 30 秒。代价如实写着：若 `stamp.ts` 的哈希口径真被
   * 改成与闸门不一致，闸门会以「内容已变」报红（带补跑命令），方向是**假红**（多跑一次），不是假绿。
   * 这个方向的错是本仓明确选的那一侧（假红可忍、假绿不可忍，同 DESIGN「为什么不进 CI」）。
   */
  const OUT_OF_FACE_OK: Record<string, string> = {
    [STAMP_TS]: "记录层（算哈希/写 JSON），不参与「杀掉 vs 存活」的判定；进面只会让改指纹文案也要重跑 30 秒",
  }

  test("面内每份文件引用的文件，要么也在面内、要么在豁免表里写明理由（过窄 = 假绿）", () => {
    const inside = new Set(EXPECTED_INPUTS)
    const leaked: string[] = []
    for (const p of EXPECTED_INPUTS) {
      for (const dep of staticDeps(join(REPO, p), p)) {
        if (inside.has(dep) || dep in OUT_OF_FACE_OK) continue
        leaked.push(`${p} → ${dep}`)
      }
      /* `status-core.ts` 有一条硬性质：**它自己零依赖**（纯函数模块、无 import）。
       * 它一旦开始引用别的源码，就把新文件拉进了判据面——那条依赖必须先被显式决定。 */
      if (p === TARGET_FILE) {
        const imports = [...strip(readFileSync(join(REPO, p), "utf8")).matchAll(/\bfrom\s*"([^"]+)"/g)].map((m) => m[1]!)
        expect(imports, `${TARGET_FILE} 出现了 import——它本是零依赖纯模块，新增依赖必须先进指纹面`).toEqual([])
      }
    }
    expect(
      leaked,
      `这些依赖既不在指纹面内、也不在豁免表里：改了它们闸门不会红（假绿）。\n` +
        `  请二选一：① 加进 STAMP_INPUTS + EXPECTED_INPUTS 并跑一次 \`${RERUN}\`；\n` +
        `            ② 确与判定无关 → 写进本组 OUT_OF_FACE_OK 并注明理由。`,
    ).toEqual([])
  })

  test("闭包结论与指纹面一一对应（写死的期望与实算逐项相同，防它随源码漂移）", () => {
    for (const p of EXPECTED_INPUTS) {
      const got = staticDeps(join(REPO, p), p).sort()
      const want = [...new Set(KNOWN_DEPS[p] ?? [])].sort()
      expect(
        got,
        `${p} 的依赖与 KNOWN_DEPS 里写死的不符——源码新增/删除了对文件的依赖。\n` +
          `  这不是「测试过时」，而是一个必须做的决定：新依赖进不进判据面？（见本组头注释）`,
      ).toEqual(want)
    }
  })

  test("面外文件不参与指纹：改「不该进面」的文件不会把闸门弄红（过宽侧的边界）", () => {
    /* 「过宽」方向：判据面是**精确的这四份**，不多不少。以下文件的改动必须**不影响**指纹——它们改变的是
     * 「怎么记」（stamp.ts / 本文件）或「这台变异台之外还守了什么」（语义契约与接线契约测试、文档），
     * 不是「这台变异台的结论」。若哪天它们进了面，每改一句文案都要等 30 秒，闸门就成了噪声源。 */
    const outside = [
      "DESIGN.md",
      "AGENTS.md",
      STAMP_TS,
      GATE_FILE,
      "packages/web/src/files/status-core.test.ts",
      "packages/web/src/files/style-contract.test.ts",
    ]
    for (const p of outside) {
      expect(EXPECTED_INPUTS.includes(p), `${p} 不该进指纹面（它不参与判定；进去 = 每改一次无关文件都要重跑 30 秒）`).toBe(false)
    }
    /* 反面：确参与判定的四份**必须**在面内（同一件事的另一半——少一份即假绿）。 */
    for (const p of [TARGET_FILE, MATRIX_TEST, MUTANTS_FILE, BENCH_FILE]) {
      expect(EXPECTED_INPUTS.includes(p), `${p} 必须在指纹面内（它是判定链上的一环）`).toBe(true)
    }
    /* 面内成员不得重复：重复项不增加覆盖，却会让「四份」这个计数失去意义（上面第 75 行拿它当判据）。 */
    expect(new Set(EXPECTED_INPUTS).size, "指纹面里出现重复项").toBe(EXPECTED_INPUTS.length)
    /* 面内**有且仅有矩阵这一份测试文件**（它是判官）；否则说明有人把别的测试也算了进来（过宽）。 */
    expect(EXPECTED_INPUTS.filter((p) => p.endsWith(".test.ts")), "指纹面里应只有矩阵这一份测试文件").toEqual([MATRIX_TEST])
  })
})
