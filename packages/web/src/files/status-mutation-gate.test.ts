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
 * 任何东西，只读四个文件算哈希。名字是这里的关键，改动前先想清楚这一点。
 */
import { describe, expect, test } from "bun:test"
import { createHash } from "node:crypto"
import { existsSync, readFileSync } from "node:fs"
import { join } from "node:path"
import {
  MATRIX_TEST,
  MUTANTS,
  TARGET_FILE,
  expectsKill,
} from "../../../../tools/mutation/status-core/mutants"
import { STAMP_FILE, STAMP_INPUTS, computeFiles, readStamp, staleFiles } from "../../../../tools/mutation/status-core/stamp"

/**
 * 指纹面的**独立对账**（不得由 stamp.ts 的 `STAMP_INPUTS` 反推——那等于自己验自己）。
 *
 * 为什么要在这里写死：否则「把某个文件从指纹面里去掉」就成了绕过闸门的最短路径——改一行
 * `STAMP_INPUTS`，那文件从此改坏也不再触发重跑。两处独立列出后，去掉一个就红在这里。
 * 新增指纹面成员时**两处都要加**（这处是判据，stamp.ts 那处是实现，同 AGENTS.md 的口径）。
 */
const EXPECTED_INPUTS: string[] = [
  TARGET_FILE,
  MATRIX_TEST,
  "tools/mutation/status-core/mutants.ts",
  "tools/mutation/status-core/bench.ts",
]

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
    const self = "packages/web/src/files/status-mutation-gate.test.ts"
    expect(EXPECTED_INPUTS.includes(self), "闸门自身进了指纹面 = 改闸门也要重跑那台，无谓的成本").toBe(false)
    expect(readFileSync(import.meta.path, "utf8").length).toBeGreaterThan(0)
  })
})
