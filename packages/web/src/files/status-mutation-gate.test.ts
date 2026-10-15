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
 *   1. **指纹过期**：判据面（`status-core.ts` / 矩阵 / 变异体清单 / 执行器 **及它们的依赖闭包**）有一样变了
 *      而指纹没刷 → 改了判据、或加了变异体、或改了判定逻辑、或改了**被它们 import 的模块**，都没重跑；
 *   2. **没有指纹**：`stamp.json` 缺失/结构不符 → 这台从未在本仓跑过（或结构版本升了），先跑一次；
 *   3. **实测结果与清单对不上**：例如清单里 21 条 `code` 却记着杀掉 20 条 → 有人手改了指纹。
 *
 * ## 边界（写清楚，别把它当防伪）
 * 它守**防忘**，不守**防伪**：手改 `stamp.json` 也能骗过（第 3 条只是让这种改动容易被看出来）。
 * 蓄意绕过交给 code review——`stamp.json` 里逐 kind 的条数就是给 review 看的数字。
 *
 * ## 为什么这个文件叫 `.test.ts`（与变异台文件**刻意相反**）
 * 变异台本体（`bench.ts` / `mutants.ts` / `stamp.ts` / `closure.ts`）必须**不带** `.test` 标记，否则 `bun test`
 * 会把它当用例跑起来、spawn 几十秒。本文件恰好相反：它**就是要**被 `bun test` 采到——它不 spawn
 * 任何东西，只读判据面（含依赖闭包）算哈希，外加在**临时目录里造合成依赖树**把闭包算法自己验一遍，
 * 并用同一棵树做一条**防退化自检**（指纹键集必须含闭包成员——真仓闭包为空，那半边只有合成树能验）。
 * 名字是这里的关键，改动前先想清楚这一点。
 */
import { describe, expect, test } from "bun:test"
import { createHash } from "node:crypto"
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, posix } from "node:path"
import {
  MATRIX_TEST,
  MUTANTS,
  TARGET_FILE,
  expectsKill,
} from "../../../../tools/mutation/status-core/mutants"
import {
  EXEMPT_FROM_FINGERPRINT,
  STAMP_FILE,
  STAMP_ROOTS,
  computeFiles,
  fingerprintInputs,
  readStamp,
  staleFiles,
} from "../../../../tools/mutation/status-core/stamp"
import { depClosure, importersOf, inFingerprintScope, staticDeps } from "../../../../tools/mutation/status-core/closure"

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
 * 指纹面的**独立对账**（不得由 stamp.ts 的 `STAMP_ROOTS` / `fingerprintInputs()` 反推——那等于自己验自己）。
 *
 * 为什么要在这里写死：否则「把某个文件从指纹面里去掉」就成了绕过闸门的最短路径——改一行
 * `STAMP_ROOTS`，那文件从此改坏也不再触发重跑。两处独立列出后，去掉一个就红在这里。
 * 新增指纹面成员时**两处都要加**（这处是判据，stamp.ts 那处是实现，同 AGENTS.md 的口径）。
 */
const EXPECTED_ROOTS: string[] = [TARGET_FILE, MATRIX_TEST, MUTANTS_FILE, BENCH_FILE]

/**
 * **期望的依赖闭包**（写死的判据侧对账——同样不得由实现反推）。
 *
 * 当前是**空**：四份根本下零依赖——`status-core.ts` 是零依赖纯模块（下面有硬检查）、矩阵只引它、
 * 清单只写路径常量、执行器除了清单只剩一个被豁免的 `stamp.ts`（记录层）。
 *
 * 空数组不是「这条检查在空转」：它是一条**会响的警报线**——四份根里任何一份将来真的 import 了
 * 本仓别的源码（例如把取词表抽成 `status-labels.ts`），`fingerprintInputs()` 会当场多出一份，
 * 而这里写死的是空 → 红，逼人做一次显式决定：让它留在闭包里（于是今后改它必须重跑变异台），
 * 还是列进 `EXEMPT_FROM_FINGERPRINT` 并写明理由。而「被引用的那份改了」这件事本身由指纹自动覆盖
 * （见「依赖闭包」一组里那条端到端用例）。
 */
const EXPECTED_CLOSURE: string[] = []

/** 豁免表也写死在判据侧：把真成员偷偷豁免掉 = 把闸门掏空，这里对账。 */
const EXPECTED_EXEMPT: string[] = [STAMP_TS]

/** 补跑命令（失败信息里逐字给出来——人不该需要记得它长什么样）。 */
const RERUN = "bun run mutate:status"
const REPO = join(import.meta.dir, "..", "..", "..", "..")

describe("变异台防忘闸门：判据面一变，常规测试就该红", () => {
  test("指纹面与判据侧写死的清单逐字一致（少一项 = 那条路径的改动将静默逃过闸门）", () => {
    const declared: string[] = [...STAMP_ROOTS]
    expect(declared.sort(), "指纹面的根被改动过：新增/删除成员必须同步本文件的 EXPECTED_ROOTS").toEqual(
      [...EXPECTED_ROOTS].sort(),
    )
  })

  test("指纹文件入库且结构合法（缺失 = 本仓从未跑过那台，或结构版本升了）", () => {
    expect(existsSync(join(REPO, STAMP_FILE)), `缺少 ${STAMP_FILE}：先跑一次 \`${RERUN}\` 把证据立起来`).toBe(true)
    const stamp = readStamp()
    expect(stamp, `${STAMP_FILE} 读不出（坏 JSON / 结构版本不符）：跑 \`${RERUN}\` 重新生成`).not.toBeNull()
    expect(
      Object.keys(stamp!.files).sort(),
      "指纹记录的键集与「根 + 依赖闭包」不一致（少一个 = 那份改坏也静默逃过闸门）",
    ).toEqual([...fingerprintInputs()].sort())
  })

  test("指纹未过期：判据面（四份根 + 依赖闭包）与那一次跑时逐字节同源", () => {
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
    for (const p of fingerprintInputs()) {
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
    expect(fingerprintInputs().includes(GATE_FILE), "闸门自身进了指纹面 = 改闸门也要重跑那台，无谓的成本").toBe(false)
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
 * - `status-mutation-gate.test.ts`（本文件）、`stamp.ts` 与 `closure.ts`：它们**不参与判定**，
 *   只记录、对账、算闭包；改一句失败文案就要重跑 30 秒的台，没人受得了（且很快就会被绕过）。
 * - `status-core.test.ts`（语义契约测试）、`style-contract.test.ts`（接线契约）：变异台**不跑它们**
 *   （它只跑矩阵 + `tsc`），故它们变了不影响这台台的结论。
 * - `DESIGN.md` / `AGENTS.md`：文档随判据一起改是**正常**的，要文档一变就重跑，闸门就成噪声源了。
 *
 * ## 指纹 = 根 + 依赖闭包（**这是本条改动补的那半边**）
 * 「判据面」不只是那四份根文件本身：判定链并不止那几份。矩阵判 `status-core.ts`，而一旦
 * `status-core.ts` 开始引用别的模块（把取词表抽成 `status-labels.ts`、把类型抽成 `status-types.ts`），
 * **被引用的那段代码就成了判据的一部分**——它被改坏时根的字节没变，缺了闭包就是一次**假绿**。
 * 故指纹面 = `STAMP_ROOTS` ∪ 它们的**递归 import 闭包**（`closure.ts` 算，只收本仓 `packages/` 与
 * `tools/` 下的源码），并在 `EXEMPT_FROM_FINGERPRINT` 里逐条写明不参与判定的记录层。
 *
 * ## 两个方向的价都写过：过宽 vs 过窄
 * - **过窄 = 假绿**（比过宽严重得多）：漏进去的那份改了却不红，让人误以为「刚验过」；
 * - **过宽 = 噪声红**：把不影响判定的文件算进来，每改一次无关文件都要等 30 秒——而会无故报红的
 *   门禁很快就会被绕过（本仓已在 DESIGN「为什么不进 CI」里写过这个代价）。
 *
 * 下列四条把这个定义钉在**可自动核验的判据**上（否则它就只是文档里的一句话，而没人会在改代码时去翻）：
 * ① **闭包可复算、键集对得上**：`fingerprintInputs()`（根 ∪ 闭包）与 `stamp.json` 记的键集逐项一致——
 *    少一个就是「那份改坏也静默逃过闸门」；
 * ② **写死的期望不得随源码漂移**：`KNOWN_DEPS`（逐份直接依赖）与 `EXPECTED_CLOSURE`（当前闭包为空）
 *    两条独立列出，源码新增依赖就红，逼一次显式决定；
 * ③ **闭包算法自己可测**：在**临时目录里造合成依赖树**验递归传递、豁免剪枝、边界（`node_modules` /
 *    `vendor` / `dist` / `public` / `node:` / 裸包名 / 出仓 / 环）与注释安全——把 `closure.ts` 掏空
 *    （恒返回 `[]`）会当场红，而不是静默丢掉覆盖；
 * ④ **端到端**：拿合成树走 `staleFiles`，断言「被引用的那份被改了」真的被报成过期（接口级，不碰真仓库）。
 */
describe("判据面定义：依赖闭包（过窄=假绿，这里先红）", () => {
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
   * **豁免表**：闭包收得到、但经判断**不需要**跟着重跑的文件——逐条写明理由（对象本体在 `stamp.ts`，
   * 这里只对账它的成员，两边独立列出）。
   *
   * 为什么要有它而不是直接放行：放行 = 「过窄」无声发生（新增依赖照样全绿）。豁免要求写出理由，
   * 于是「这台变异台的判据面为什么是这些」在代码里就是可复核的，而不是散在文档里的一句话。
   *
   * 目前唯一一条 `STAMP_TS`：`stamp.ts` 是**记录层**（算哈希 / 写 JSON / 算闭包），它不参与
   * 「杀掉 / 存活」的判定——改它的哈希口径不会让任何一条变异体换结论。反过来把它放进面内还会
   * 制造**噪声红**：改一句指纹的失败文案就要重跑 30 秒。代价如实写着：若它真被改成与闸门不一致，
   * 闸门会自己报「内容已变」（带补跑命令），方向是**假红**（多跑一次），不是假绿。
   * 这个方向的错是本仓明确选的那一侧（假红可忍、假绿不可忍，同 DESIGN「为什么不进 CI」）。
   */
  test("豁免表成员逐字对账（把真成员偷偷豁免掉 = 把闸门掏空）", () => {
    expect(
      Object.keys(EXEMPT_FROM_FINGERPRINT).sort(),
      "豁免表被改动过：新增/删除豁免必须同步本文件的 EXPECTED_EXEMPT 并写明理由",
    ).toEqual([...EXPECTED_EXEMPT].sort())
    for (const p of Object.keys(EXEMPT_FROM_FINGERPRINT)) {
      expect(fingerprintInputs().includes(p), `${p} 既在豁免表里又在指纹面里（两张表互相矛盾）`).toBe(false)
    }
  })

  test("面内每份文件引用的文件，要么也在面内、要么在豁免表里写明理由（过窄 = 假绿）", () => {
    const inputs = new Set(fingerprintInputs())
    const exempt = new Set([...Object.keys(EXEMPT_FROM_FINGERPRINT), ...EXPECTED_EXEMPT])
    const leaked: string[] = []
    for (const p of fingerprintInputs()) {
      for (const dep of staticDeps(p)) {
        /* 闭包**本来就是递归**收的，故这里真正的判据是：面内成员引用到的东西，
         * 必须要么已在这个面里（说明它被收进来了）、要么进了豁免表（说明有人为它写下了理由）。 */
        if (inputs.has(dep) || exempt.has(dep)) continue
        leaked.push(`${p} → ${dep}`)
      }
      /* `status-core.ts` 有一条硬性质：**它自己零依赖**（纯函数模块、无 import）。
       * 它一旦开始引用别的源码，就把新文件拉进了判据面——那条依赖必须先被显式决定
       * （闭包会自动收录它，于是这里会从 `KNOWN_DEPS` 那一关先红）。 */
      if (p === TARGET_FILE) {
        expect(staticDeps(p), `${TARGET_FILE} 出现了依赖——它本是零依赖纯模块，新增依赖必须先进指纹面`).toEqual([])
      }
    }
    expect(
      leaked,
      `这些依赖既不在指纹面内、也不在豁免表里：改了它们闸门不会红（假绿）。\n` +
        `  请二选一：① 让它留在闭包里（跑一次 \`${RERUN}\`，并把 EXPECTED_CLOSURE 同步进去）；\n` +
        `            ② 确与判定无关 → 写进 EXEMPT_FROM_FINGERPRINT 与 EXPECTED_EXEMPT 并注明理由。`,
    ).toEqual([])
  })

  test("直接依赖与 KNOWN_DEPS 逐项相同（防它随源码漂移而变成一句空话）", () => {
    for (const p of EXPECTED_ROOTS) {
      const got = staticDeps(p).sort()
      const want = [...new Set(KNOWN_DEPS[p] ?? [])].sort()
      expect(
        got,
        `${p} 的依赖与 KNOWN_DEPS 里写死的不符——源码新增/删除了对文件的依赖。\n` +
          `  这不是「测试过时」，而是一个必须做的决定：新依赖进不进判据面？（见本组头注释）`,
      ).toEqual(want)
    }
  })

  test("指纹面 = 根 + 闭包，且闭包成员与判据侧写死的 EXPECTED_CLOSURE 相同", () => {
    const inputs = fingerprintInputs()
    const closure = inputs.filter((p) => !(EXPECTED_ROOTS as readonly string[]).includes(p))
    expect(
      [...inputs].sort(),
      "指纹面被改动过：根集合须同步 EXPECTED_ROOTS，闭包集合须同步 EXPECTED_CLOSURE",
    ).toEqual([...EXPECTED_ROOTS, ...EXPECTED_CLOSURE].sort())
    expect([...closure].sort(), "依赖闭包与判据侧写死的期望不符（新增/删除了被引用的文件）").toEqual([...EXPECTED_CLOSURE].sort())
    /* 根与闭包不得重叠、也不得重复：重叠说明实现在「去重」上破了，重复会让计数类判据失去意义。 */
    expect(new Set(inputs).size, "指纹面里出现重复项").toBe(inputs.length)
    for (const r of EXPECTED_ROOTS) {
      expect(closure.includes(r), `${r} 同时出现在根与闭包里`).toBe(false)
    }
    /* 闭包成员必须真的**由**面内某份文件引用而来（否则它是从哪进来的？）——顺带把「为什么在面内」变成可读的。 */
    for (const p of closure) {
      expect(
        importersOf(p, inputs).length,
        `${p} 在闭包里、却没有任何面内成员引用它——收错了？`,
      ).toBeGreaterThan(0)
    }
  })

  test("面外文件不参与指纹：改「不该进面」的文件不会把闸门弄红（过宽侧的边界）", () => {
    /* 「过宽」方向：判据面是**精确的这几份**（根 + 闭包），不多不少。以下文件的改动必须**不影响**指纹——它们改变的是
     * 「怎么记」（stamp.ts / 本文件）或「这台变异台之外还守了什么」（语义契约与接线契约测试、文档），
     * 不是「这台变异台的结论」。若哪天它们进了面，每改一句文案都要等 30 秒，闸门就成了噪声源。 */
    const outside = [
      "DESIGN.md",
      "AGENTS.md",
      STAMP_TS,
      GATE_FILE,
      "tools/mutation/status-core/closure.ts", // 闭包算法：只有闸门侧用（实现处，不参与判定）
      "tools/mutation/status-core/stale.ts", // 自助定位：只读指纹、只报过期项
      "packages/web/src/files/status-core.test.ts",
      "packages/web/src/files/style-contract.test.ts",
      "packages/web/src/files/status-matrix.test.ts.bak", // 不存在的邻居也不例外
    ]
    for (const p of outside) {
      expect(fingerprintInputs().includes(p), `${p} 不该进指纹面（它不参与判定；进去 = 每改一次无关文件都要重跑 30 秒）`).toBe(false)
    }
    /* 反面：确参与判定的根**必须**在面内（同一件事的另一半——少一份即假绿）。 */
    for (const p of EXPECTED_ROOTS) {
      expect(fingerprintInputs().includes(p), `${p} 必须在指纹面内（它是判定链上的一环）`).toBe(true)
    }
    /* 面内**有且仅有矩阵这一份测试文件**（它是判官）；否则说明有人把别的测试也算了进来（过宽）。 */
    expect(
      fingerprintInputs().filter((p) => p.endsWith(".test.ts")),
      "指纹面里应只有矩阵这一份测试文件",
    ).toEqual([MATRIX_TEST])
    /* 面内**不得**出现第三方/随包资源/构建产物（闭包边界：否则第一个 import 就会把 6908 份 node_modules 拖进来）。
     * 注意这条**不重**：边界算法本身由下一组的合成树独立验（这里只验真指纹没被越界的成员污染）。 */
    const BLOCK = ["node_modules", "vendor", "dist", "build", "public"]
    for (const p of fingerprintInputs()) {
      const hit = BLOCK.find((seg) => p.split("/").includes(seg))
      expect(hit, `${p} 命中边界排除段 \`${hit}\`——第三方/构建产物不该进指纹面（纯噪声红）`).toBeUndefined()
    }
    /* 结构判据（写在这里是因为它是**闭包边界**的性质，不是「根该是哪四份」的性质）：
     * 面里唯一的测试文件就是矩阵；`tools/` 下的成员只能是那两个变异的**输入与执行器**。 */
    expect(
      fingerprintInputs().filter((p) => p.startsWith("tools/")),
      "tools/ 下只该有变异体清单与执行器进面（别的 tools 源码进来 = 边界失控）",
    ).toEqual([MUTANTS_FILE, BENCH_FILE])
  })
})

/* ================================================================== *
 * 闭包算法自身：在临时目录里造合成依赖树（唯一能独立验它的办法）
 * ================================================================== */

/**
 * ## 为什么要造树（而不是拿真仓库的文件验）
 * 真仓库当下**几乎没有闭包成员**（四份根本下零依赖，只有一条被豁免的记录层），于是拿它当靶子只验到
 * 「空集也是集」。而这条机制要防的恰恰是**将来**那次「根的 import 图里多出一份」——那件事在真仓库
 * 里没法安全地造（要么改真源码、要么放过它）。故这里用**临时目录 + 合成源码**：写得出的依赖形态
 * 全都造一遍（相对路径 / 裸文件名 / 动态 import / 只副作用 import / TS 扩展名 / 目录 index / 注释里的
 * 假依赖 / 环），再断言闭包逐份收对。
 *
 * **这也让「把 `closure.ts` 掏空」当场红**（恒返回 `[]` 时下面多条会失败）：本仓的分工是
 * **实现一处、判据另一处**——`closure.ts` 自己不进指纹面（改它不该要人重跑 30 秒），
 * 但它的行为被这里独立钉住，不会静默退化。
 */
describe("依赖闭包算法：在合成依赖树上验（递归 / 边界 / 豁免 / 环 / 注释安全）", () => {
  /** 一份合成树：`文件 → 源码`。写入临时目录后即是「（迷你）仓根」。 */
  const TREE: Record<string, string> = {
    "tools/app/main.ts": [
      `import { a } from "./mid"`, // 相对路径 + 扩展名补全
      `import "side/x.ts"`, // 只副作用 import + 显式扩展名
      `const dyn = await import("./deep/leaf")`, // 动态 import
      `import { s } from "./stringy"`, // 这份里的路径字面量长在字符串里（注释安全的反证）
      `import { t } from "./loopy-a"`, // 环的入口
      // 下面六份各自引用一个「不该进闭包的标的」——用来验边界只挡**被引用的目标**，
      // 不株连「引用了它的文件」（否则一次无关引用会把整份文件从覆盖里挤出去）
      `import "./to-node-modules"`,
      `import "./to-vendor"`,
      `import "./to-public"`,
      `import "./to-outside"`,
      `import "./to-bare"`,
      `import "./to-builtin"`,
    ].join("\n"),
    "tools/app/mid.ts": `import { g } from "./deep/grand"`, // 下标文件：验递归的**第二层**
    "tools/app/deep/grand.ts": `export const g = 1`,
    "tools/app/deep/leaf.ts": `export const l = 1`,
    "tools/app/side/x.ts": `export const sx = 1`,
    // 注释安全：`from` 出现在块注释里（旧的非贪婪正则会吞掉它**之后**的真代码，把 mid 看漏）
    "tools/app/stringy.ts": [
      `/* 例子：import { x } from "./never" */ import { a } from "./mid"`,
      `const P = "tools/app/mid.ts"`,
      `// import { y } from "./also-never"`,
    ].join("\n"),
    "tools/app/loopy-a.ts": `import { b } from "./loopy-b"`,
    "tools/app/loopy-b.ts": `import { a } from "./loopy-a"`, // 环：不得挂死、不得重复收
    // 边界：这四份都不该进闭包
    "node_modules/pkg/index.ts": `export const n = 1`,
    "vendor/third/a.ts": `export const v = 1`,
    "dist/build.ts": `export const d = 1`,
    "packages/web/public/vendor/a.ts": `export const p = 1`,
    // 引用「不该进闭包」的两份（各从面内文件引一次，验边界真的挡住了）
    "tools/app/to-node-modules.ts": `import { n } from "../../node_modules/pkg/index"`,
    "tools/app/to-vendor.ts": `import { v } from "../../vendor/third/a"`,
    "tools/app/to-public.ts": `import { p } from "../../packages/web/public/vendor/a"`,
    "tools/app/to-outside.ts": `import { o } from "../../../outside/o"`, // 出仓
    "tools/app/to-bare.ts": `import { r } from "react"`, // 裸包名
    "tools/app/to-builtin.ts": `import { readFileSync } from "node:fs"`, // 内建
  }
  /** 入口根（与真仓库同形：一份 `tools/` 下的、一份 `packages/` 下的）。 */
  const ROOTS = ["tools/app/main.ts", "packages/web/src/root.ts"]

  /** 把合成树落到临时目录（两级 `../../../../` 的距离要与真仓库一致，才验得到同一套路径解析）。 */
  const dir = (() => {
    const d = mkdtempSync(join(tmpdir(), "gebai-closure-"))
    const roots: Record<string, string> = {
      ...TREE,
      "packages/web/src/root.ts": `import { a } from "../../../../tools/app/mid"\n`,
    }
    for (const [rel, body] of Object.entries(roots)) {
      // 目录用**仓根相对**的那个片段去建（不能拿绝对路径喂 `posix.dirname`：Windows 绝对路径里没有 `/`）
      const relDir = posix.dirname(rel)
      if (relDir !== ".") mkdirSync(join(d, relDir), { recursive: true })
      writeFileSync(join(d, rel), body, "utf8")
    }
    return d
  })()

  /** 清场（本文件是全仓第一个「往磁盘写东西」的测试，故刻意写在文件末尾那一条的尾巴上）。 */
  const cleanup = (): void => rmSync(dir, { recursive: true, force: true })

  test("递归收录：直接 + 传递依赖（含扩展名补全、目录 index、动态/副作用 import）", () => {
    expect(depClosure(ROOTS, [], dir).sort()).toEqual(
      [
        "tools/app/mid.ts",
        "tools/app/side/x.ts",
        "tools/app/deep/grand.ts", // ← 第二层：mid 的依赖
        "tools/app/deep/leaf.ts",
        "tools/app/stringy.ts",
        "tools/app/loopy-a.ts",
        "tools/app/loopy-b.ts",
        // 六份边界探针：它们**自己**在 `tools/` 下、是正常成员，被它们引用的标的不是
        "tools/app/to-node-modules.ts",
        "tools/app/to-vendor.ts",
        "tools/app/to-public.ts",
        "tools/app/to-outside.ts",
        "tools/app/to-bare.ts",
        "tools/app/to-builtin.ts",
      ].sort(),
    )
  })

  test("递归是**真递**的（拿掉第一层，第二层就看不见——证明上面那棵树不是一次性拍平）", () => {
    const flat = depClosure(["tools/app/main.ts"], [], dir)
    expect(flat.includes("tools/app/deep/grand.ts"), "grand 是靠 mid 才被收进来的").toBe(true)
    expect(staticDeps("tools/app/main.ts", dir).includes("tools/app/deep/grand.ts"), "grand 不是 main 的直接依赖").toBe(false)
  })

  test("豁免是**连子树一起剪**的（记录层引用的东西仍是记录层）", () => {
    const all = depClosure(ROOTS, [], dir)
    const cut = depClosure(ROOTS, ["tools/app/mid.ts"], dir)
    expect(cut.includes("tools/app/mid.ts"), "被豁免的文件本身不该进闭包").toBe(false)
    expect(cut.includes("tools/app/deep/grand.ts"), "被豁免文件的依赖（更深的层）也不该进——整棵子树都剪").toBe(false)
    /* 但剪掉的只是那一支：别的分支照收（否则「豁免」就成了「停表」）。 */
    expect(cut.length).toBeGreaterThan(0)
    expect(cut.includes("tools/app/loopy-a.ts")).toBe(true)
    expect(all.length, "豁免应当**严格减少**闭包（否则这条测试在空转）").toBeGreaterThan(cut.length)
  })

  test("边界：第三方/随包资源/构建产物/出仓/内建/裸包名一个都不进（过宽侧的假红防线）", () => {
    const hit = depClosure(ROOTS, [], dir)
    for (const p of [
      "node_modules/pkg/index.ts",
      "vendor/third/a.ts",
      "dist/build.ts",
      "packages/web/public/vendor/a.ts",
    ]) {
      expect(hit.includes(p), `${p} 不该进闭包（它是第三方 / 随包资源 / 构建产物）`).toBe(false)
    }
    /* 另三类（出仓 / 裸包名 / 内建）连**目标文件都不存在**，故用「引用它们的文件自己有没有进来」
     * 反证：入口确实被读到了（六份探针全在 `tools/` 下、都是正常成员）——否则上面那几条是在空转。 */
    for (const p of [
      "tools/app/to-node-modules.ts",
      "tools/app/to-vendor.ts",
      "tools/app/to-public.ts",
      "tools/app/to-outside.ts",
      "tools/app/to-bare.ts",
      "tools/app/to-builtin.ts",
    ]) {
      expect(hit.includes(p), `${p} 本身在 tools/ 下，是正常成员（探针文件不得被株连）`).toBe(true)
    }
  })

  test("边界判据本身可测（纯函数：逐类路径给结论）", () => {
    const cases: Array<[string, boolean]> = [
      ["packages/web/src/files/status-core.ts", true],
      ["tools/mutation/status-core/bench.ts", true],
      ["node_modules/pkg/index.ts", false],
      ["packages/web/node_modules/pkg/index.ts", false],
      ["vendor/reel/runtime/x.ts", false],
      ["packages/web/dist/a.ts", false],
      ["packages/web/build/a.ts", false],
      ["packages/web/public/vendor/a.ts", false],
      ["shuangtian/src/a.ts", false], // 本仓另一棵树：不在闭包范围内
      ["../outside/a.ts", false],
      ["DESIGN.md", false],
    ]
    for (const [p, want] of cases) expect(inFingerprintScope(p), `${p} 的边界判定错了`).toBe(want)
  })

  test("环安全：互为依赖不得挂死、不得重复收", () => {
    const out = depClosure(["tools/app/loopy-a.ts"], [], dir)
    expect(out).toEqual(["tools/app/loopy-b.ts"]) // a → b → a：环上的另一份收进来，闭环不再展开
  })

  test("注释安全：注释里的假依赖不算，注释**之后**的真依赖不许被吞（假绿侧）", () => {
    const got = staticDeps("tools/app/stringy.ts", dir)
    expect(got, "注释里那两条 `from` 不该被当成依赖").not.toContain("tools/app/never.ts")
    expect(got, "注释里那两条 `from` 不该被当成依赖").not.toContain("tools/app/also-never.ts")
    /* 这条是**真判据**：旧的非贪婪块注释正则会把 `/* … *​/` **之后**的真代码一起吞掉，
     * 于是 `stringy.ts` 对 `mid.ts` 的真依赖会被看漏——那正是「假绿」那一侧的错。 */
    expect(got, "行内块注释之后的真 import 被吞了（去注释实现有假绿风险）").toContain("tools/app/mid.ts")
    expect(staticDeps("tools/app/stringy.ts", dir)).toContain("tools/app/mid.ts")
  })

  test("顶层文件不存在 / 不是源码时不抛错（返回空，由「成员必须存在」那条查）", () => {
    expect(staticDeps("tools/app/nope.ts", dir)).toEqual([])
    expect(depClosure(["tools/app/nope.ts"], [], dir)).toEqual([])
  })

  /**
   * **端到端**：拿合成树走一遍 `staleFiles`（闸门判「过期」的那个函数），断言
   * 「**被引用的那份**（闭包成员）被改了」真的会被报成过期——这正是本单要补的那半边。
   *
   * 用 `staleFiles(recorded, current)` 的注入参数，所以**不碰真仓库**：`recorded` 用改前的哈希，
   * `current` 用改后的——两者之差即是「闭包成员改了而没重跑」。
   */
  test("端到端：闭包成员被改了 → staleFiles 报出它（这就是原来漏掉的那种假绿）", () => {
    const inputs = [...ROOTS, ...depClosure(ROOTS, [], dir)]
    const h = (s: string): string => `sha256:${createHash("sha256").update(s).digest("hex")}`
    const snapshot = (): Record<string, string> => {
      const out: Record<string, string> = {}
      for (const p of inputs) out[p] = h(readFileSync(join(dir, p), "utf8"))
      return out
    }
    const before = snapshot()
    // 改坏**被引用**的那份（不是根）
    writeFileSync(join(dir, "tools/app/deep/grand.ts"), "export const g = 2 // 改坏了\n", "utf8")
    const stale = staleFiles(before, snapshot())
    expect(stale.length, "闭包成员被改了却报「不过期」——这就是要修的那种假绿").toBe(1)
    expect(stale[0], "报出来的应当是那份被改的闭包成员").toContain("tools/app/deep/grand.ts")
    cleanup()
  })
})

/* ================================================================== *
 * 防退化自检：指纹的键集必须真的含**闭包成员**（真仓闭包为空，只能拿合成树验）
 * ================================================================== */

/**
 * ## 为什么单列这一组（这是一条**防退化**锁，守的是一段真实的失败历史）
 *
 * 指纹原先**只哈希四份根文件本身**（`dfe18f67^` 的 `stamp.ts`：`computeFiles()` 里
 * `for (const p of STAMP_INPUTS)`——面就是那四份字符串，没有第二处来源）。后果是：判据一旦被
 * 拆成多份模块（把取词表抽成 `status-labels.ts`、把类型抽成 `status-types.ts`），**被引用的那份
 * 就成了判据的一部分、却不在指纹里**——它被改坏时根的字节没变、指纹没脏、闸门不红，而矩阵照旧
 * 全绿。这就是「过窄 = 假绿」，也是「防忘」这台机器唯一不能出的错。
 *
 * `dfe18f67` 把它修成「根 ∪ 依赖闭包」，但**真仓当下的闭包是空集**（四份根本下零依赖）——
 * 于是有个反直觉的后果：**把实现退回只哈希根文件，真仓里一点反应都没有**。`EXPECTED_ROOTS` /
 * `EXPECTED_CLOSURE`（写死为空）/ 「指纹面 = 根 + 闭包」三条对账全都会照旧绿，没人会发现。
 * 换言之，修复机制本身**没有防退化的锁**——而这正是本任务要补的那一环。
 *
 * ## 做法：拿合成树（依赖链非空）走**真入口** `fingerprintInputs()`
 * 断言两件事：
 *   1. **键集含闭包成员**（不止根）——特别是 `status-core` 那一层的依赖，对应「把取词表拆出去」；
 *   2. **旧的退化形态当场红**：拿 `staleFiles` 拿「只哈希根」的面去对，它**看不见**闭包成员的改动
 *      （报空），而正确的面**看得见**。两条并排写，`false` 的那条才说明另一条不是空转。
 *
 * ## 为何不碰真仓库、为何不引入新变量
 * 用 `fingerprintInputs({ roots, exempt, base })` 的注入参数（见 `stamp.ts` 的 `FingerprintFaceInput`），
 * 合成树写在临时目录、验完即删。注入的是**输入**、不是判据：拼装逻辑（根 ∪ 闭包、去重、排序）仍在
 * `stamp.ts` 那一处实现，因而这只是「拿真实现跑一遍合成输入」，不是自己验自己（同 `staleFiles`）。
 *
 * ## 它守不住什么（如实记）
 * 闭包为空时「根 ∪ 空 = 根」，故这种退化在**真仓当下**是**行为等价**的（不是当场能看到的错）。
 * 这组测试把「等价」变成「**必须显式改掉一个红测试**」：想去掉闭包收录，就得先删掉这里两条用例
 * （而它们是**入判据**的、会出现在 diff 里），于是「无声退回」变成「有人当着面删锁」。
 */
describe("防退化自检：指纹键集必须含闭包成员（否则退回「只哈希根文件」无人察觉）", () => {
  /** 合成仓根 = 一对「四份根」的缩影：`status-core` 那侧的根 + `tools/` 那侧的根。 */
  const ROOTS_PROBE = [
    "packages/web/src/status-labels-probe.ts", // 相当于 status-core.ts 拆出取词表之后的形态
    "tools/app/bench-probe.ts", // 相当于执行器（工具侧判据）
  ]
  const dir = mkdtempSync(join(tmpdir(), "gebai-anti-regress-"))
  const TREE: Record<string, string> = {
    // 被审对象那一侧：根 → 取词表 → 更下一层（**传递**依赖，不只是直接依赖）
    "packages/web/src/status-labels-probe.ts": `import { L } from "./status-labels"\nexport const x = L\n`,
    "packages/web/src/status-labels.ts": `import { t } from "./status-types"\nexport const L = t\n`,
    "packages/web/src/status-types.ts": `export const t = 1\n`,
    // 工具侧：根 → 清单 → 清单又被更下一层引用
    "tools/app/bench-probe.ts": `import { M } from "./mutants-probe"\nexport const b = M\n`,
    "tools/app/mutants-probe.ts": `import { d } from "./data/names"\nexport const M = d\n`,
    "tools/app/data/names.ts": `export const d = 1\n`,
    // 干扰项：面外（不该进闭包）——证明下面收到的那几份是**按判据**收的，不是把临时目录全扫进来
    "shuangtian/not-in-scope.ts": `export const n = 1\n`,
  }
  for (const [rel, body] of Object.entries(TREE)) {
    mkdirSync(join(dir, posix.dirname(rel)), { recursive: true })
    writeFileSync(join(dir, rel), body, "utf8")
  }
  const cleanup = (): void => rmSync(dir, { recursive: true, force: true })

  /** 闭包成员（= 面减去根）——**这就是「只哈希根文件」会漏掉的那一批**。 */
  const EXPECTED_CLOSURE_PROBE = [
    "packages/web/src/status-labels.ts",
    "packages/web/src/status-types.ts",
    "tools/app/mutants-probe.ts",
    "tools/app/data/names.ts",
  ]
  /** 期望面的**顺序**：根按声明序在前、闭包排序在后（与 `fingerprintInputs()` 的确定性口径同款）。 */
  const EXPECTED_FACE_ORDERED = [...ROOTS_PROBE, ...[...EXPECTED_CLOSURE_PROBE].sort()]

  test("指纹键集含闭包成员（不只是根）——退回「只哈希根文件」这里先红", () => {
    const face = fingerprintInputs({ roots: ROOTS_PROBE, exempt: [], base: dir })
    /* ① 形状：键序确定（指纹文件要能逐字节稳定、可 git diff 读）。 */
    expect(face, "指纹键集不对：应为「根（按声明序）∪ 闭包（排序）」").toEqual(EXPECTED_FACE_ORDERED)
    /* ② 这一条**直接**钉住「只哈希根文件」那种退化：根集合本身是不够的。 */
    expect(
      face.length,
      "指纹键集只有根——实现已退回「只哈希四份根文件」（dfe18f67 修掉的那个坑）",
    ).toBeGreaterThan(ROOTS_PROBE.length)
    /* ③ 逐份点名：`status-core` 那一侧的依赖必须在键集里（任务要的那一条），含**传递**层。 */
    for (const p of [
      "packages/web/src/status-labels.ts", // status-core 拆出取词表之后，它就是判据的一部分
      "packages/web/src/status-types.ts", // 再下一层（递归，不只是直接依赖）
      "tools/app/mutants-probe.ts", // 执行器读的清单
      "tools/app/data/names.ts", // 清单再下一层
    ]) {
      expect(face.includes(p), `${p} 未进指纹键集——被引用的那份改了不会被察觉（假绿）`).toBe(true)
    }
    /* ④ 反向：面外的干扰项一个都不许进（否则这条测试会为「面被放大」而绿）。 */
    expect(face.includes("shuangtian/not-in-scope.ts"), "面外源码进了指纹面（过宽侧）").toBe(false)
    /* ⑤ 面与「根 ∪ 闭包」逐项相等（判据侧写死，不由实现反推）。 */
    expect([...face].sort(), "合成树上的指纹面 ≠ 根 ∪ 闭包（与判据侧写死的期望不符）").toEqual(
      [...EXPECTED_FACE_ORDERED].sort(),
    )
    /* 不在这里 cleanup：临时树由本组**末尾那一条**统一清（同文件开头那组的约定）——那条用 `finally`，
     * 故本组在失败跑（反向验证）时也不泄漏临时目录。 */
  })

  test("对照：旧的「只哈希根」面对同一份改动**看不见**（证明上面那条不是空转）", () => {
    /* 这条是上面那条的**反面**：把「只哈希根」当作旧实现来跑，它必须**看不见**闭包成员的改动——
     * 若它也能看见，那说明「闭包」这件事在这棵树上根本不起作用，上面那条就是在空转。 */
    try {
      const h = (p: string): string => `sha256:${createHash("sha256").update(readFileSync(join(dir, p), "utf8")).digest("hex")}`
      const face = fingerprintInputs({ roots: ROOTS_PROBE, exempt: [], base: dir })
      const oldFace = [...ROOTS_PROBE] // ← dfe18f67^ 的 `STAMP_INPUTS` 面：只有根
      const before = (ps: readonly string[]): Record<string, string> => Object.fromEntries(ps.map((p) => [p, h(p)]))

      const oldBefore = before(oldFace)
      const newBefore = before(face)
      // 改坏「被引用、但不是根」的那份（对应「把取词表抽出去之后又把它改坏」）
      writeFileSync(join(dir, "packages/web/src/status-labels.ts"), `import { t } from "./status-types"\nexport const L = t + 1 // 改坏了\n`, "utf8")

      const oldReport = staleFiles(oldBefore, before(oldFace))
      const newReport = staleFiles(newBefore, before(face))
      expect(
        oldReport,
        "「只哈希根」的面竟然看见了闭包成员的改动？那这棵合成树没在验「闭包」这件事（上面那条在空转）",
      ).toEqual([])
      expect(newReport.length, "正确的面必须看见闭包成员的改动（看不见 = 就是那个假绿）").toBe(1)
      expect(newReport[0], "报出来的应当是那份被改的闭包成员").toContain("packages/web/src/status-labels.ts")
    } finally {
      /* `finally` 而不是「末尾一行」：**这条正好是被反向验证时会红的那一条**（临时把实现退回旧口径），
       * 而断言一红就会跳过末尾那行——失败跑把临时树留在 `%TEMP%` 里（实测：一次反向验证泄漏两份）。 */
      cleanup()
    }
  })
})
