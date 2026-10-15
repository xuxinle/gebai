#!/usr/bin/env bun
/**
 * 变异台执行器：对 `status-core.ts` 逐条施加变异体，看**判据矩阵（`status-matrix.test.ts`）**是不是真守得住。
 *
 * 用途：`bun run mutate:status`（仓根；等价于 `bun run tools/mutation/status-core/bench.ts`）。
 * 退出码：0 = 全部符合期望；1 = 有不符合（回归变异逃逸 / 良性重构被误杀 / 新分支未被拦 / 变异体失效 / 环境异常）。
 *
 * ## 与「测试全绿」的关系（这是本工具存在的唯一理由）
 * `bun test` 全绿只说明「**没有断言在抱怨**」，不说明「断言覆盖了它该守的每一格」。变异验证拿这个差集开刀：
 * 故意把产品代码改坏，看测试认不认得出来。**存活 = 那条规则没有任何断言在守**（回归失败），
 * 而不是「测试通过」。判据与四类期望的定义见 `mutants.ts` 文件头与 `DESIGN.md`「测试策略 → 变异验证」。
 *
 * ## 三条工程纪律（都不是洁癖，各对应一类真实事故）
 * ① **产品源码只读**：执行器**从不写** `packages/web/src/files/status-core.ts`。它把「源码 + 矩阵测试」
 *    复制到系统临时目录，**只在副本上**改坏并跑测。上一版（开发期手写脚本）用「改真文件 → 跑 → `finally` 还原」，
 *    风险是中断（Ctrl-C / OOM / 进程被 kill）后工作区留下一个**改坏的产品源码**——而它看着像一次正常的本地修改，
 *    极易被误提交。只读原文件后，这类事故在结构上不可能发生（代价：每次跑多花几十毫秒复制两个文件）。
 * ② **串行执行，不并行**：并行会引入跨载荷的资源竞争，让「测试因抖动而失败」被记成**变异被杀死**——
 *    对 `benign` 就是一次假告警，对 `code` 则是**假通过**（最危险的一类错误：以为守住了）。串行的总成本
 *    约 20 秒（21 条 × ~0.5s），远低于为此引入的判定不可信。
 * ③ **基线先在跑**：施加任何变异之前先跑一次未变异的矩阵；基线不绿就直接退出（否则「全杀」是假的——
 *    任何变异都会"被杀"）。
 */
import { spawnSync } from "node:child_process"
import { copyFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join } from "node:path"
import { MATRIX_TEST, MUTANTS, TARGET_FILE, expectsKill, type Mutant } from "./mutants"

/* ------------------------------------------------------------------ *
 * 路径与运行参数
 * ------------------------------------------------------------------ */

/** 仓库根：由本文件位置推出（`tools/mutation/status-core/` → 上溯三层），**不依赖 cwd**。 */
const REPO = join(import.meta.dir, "..", "..", "..")
/** 前端包目录：矩阵测试按包内口径跑（`bun test` 的常规姿势），故以此为子进程 cwd。 */
const WEB_PKG = join(REPO, "packages/web")
const CORE_SRC = join(REPO, TARGET_FILE)
const MATRIX_SRC = join(REPO, MATRIX_TEST)

/** 单条「跑矩阵」与「跑 tsc」的超时（防某条变异把测试挂死时整台跑不完）。 */
const TEST_TIMEOUT_MS = 120_000
const TSC_TIMEOUT_MS = 300_000

const argv = process.argv.slice(2)
/** `--only <子串>`：只跑名字含该子串的变异体（调试单条时用，**不作为验收口径**）。 */
const only = argv.includes("--only") ? argv[argv.indexOf("--only") + 1] : undefined
/** `--print`：不符合期望时打印该条的 find/replace 与原始输出尾部（排查用）。 */
const printRaw = argv.includes("--print")

/* ------------------------------------------------------------------ *
 * 清理 ANSI：bun test 在非 TTY 下也偶带色码，干扰 `N fail` 的解析
 * ------------------------------------------------------------------ */
const stripAnsi = (s: string): string => s.replace(/\u001b\[[0-9;]*m/g, "")

interface RunOutcome {
  /** 解析到的失败用例数；`-1` = 输出没解析出来（视为**环境异常**，不当成「被杀」）。 */
  fail: number
  pass: number
  /** 子进程是否超时/未能启动。 */
  crashed: boolean
  raw: string
}

/** 跑一次矩阵测试（`cwd` = packages/web，与文档记载的命令同口径）。 */
function runMatrix(testPath: string): RunOutcome {
  const r = spawnSync("bun", ["test", testPath], { cwd: WEB_PKG, encoding: "utf8", timeout: TEST_TIMEOUT_MS })
  const raw = stripAnsi(`${r.stdout ?? ""}\n${r.stderr ?? ""}`)
  const fail = Number(raw.match(/(\d+)\s+fail/)?.[1] ?? -1)
  const pass = Number(raw.match(/(\d+)\s+pass/)?.[1] ?? -1)
  // 解析不出 `N fail` 行 = 子进程没跑起来或被超时杀掉：这不是「变异被杀」，不许当成证据
  return { fail, pass, crashed: fail < 0 || r.error !== undefined, raw }
}

/** 跑一次 tsc（只看退出码；用来判 `branch` 类是否被类型层拦下）。 */
function runTypecheck(tsconfigPath: string): { status: number | null; raw: string } {
  const r = spawnSync("bunx", ["tsc", "--noEmit", "-p", tsconfigPath], {
    cwd: WEB_PKG,
    encoding: "utf8",
    timeout: TSC_TIMEOUT_MS,
  })
  return { status: r.status, raw: stripAnsi(`${r.stdout ?? ""}\n${r.stderr ?? ""}`).trim() }
}

/* ------------------------------------------------------------------ *
 * 临时副本：产品源码在这里被改坏，真文件全程只读
 * ------------------------------------------------------------------ */

const workDir = mkdtempSync(join(tmpdir(), "gebai-mutation-"))
const corePath = join(workDir, "status-core.ts")
const matrixPath = join(workDir, "status-matrix.test.ts")
const tsconfigPath = join(workDir, "tsconfig.json")
const origCore = readFileSync(CORE_SRC, "utf8")

/**
 * 临时副本的 tsconfig：把 tsc 指向**副本目录**（否则它只看得见真 src，看不见被改坏的副本，
 * `branch` 类就永远"存活"）。`typeRoots` 指到本仓 `packages/web/node_modules/@types`——
 * 仓根的 `node_modules/@types` 在 bun 的隔离式布局下不存在（`@types/bun` 只装在各包下），
 * 直接用 tsc 默认查找会报 TS2688。
 */
const TSCONFIG = {
  compilerOptions: {
    target: "ESNext",
    lib: ["ESNext", "DOM", "DOM.Iterable"],
    module: "ESNext",
    moduleResolution: "bundler",
    noEmit: true,
    strict: true,
    skipLibCheck: true,
    types: ["bun"],
    typeRoots: [join(WEB_PKG, "node_modules/@types")],
    forceConsistentCasingInFileNames: true,
    isolatedModules: true,
    verbatimModuleSyntax: true,
    // 变异体可能造出未使用的局部变量（如 B1 提局部变量后改用）——这不该成为「被杀」的理由，
    // 故关掉这两条；判据/文案/禁用态的变异仍会被矩阵或类型错误抓住。
    noUnusedLocals: false,
    noUnusedParameters: false,
  },
  include: ["*.ts"],
}

/* ------------------------------------------------------------------ *
 * 变异体清单自检（在跑任何一条之前）：清单写错必须报错，不许静默通过
 * ------------------------------------------------------------------ */

/** 出现次数（用于「`find` 必须恰好命中一次」的对账）。 */
function countOccurrences(haystack: string, needle: string): number {
  if (needle === "") return 0
  return haystack.split(needle).length - 1
}

interface InvalidMutant {
  name: string
  why: string
}

function validate(): InvalidMutant[] {
  const bad: InvalidMutant[] = []
  const seen = new Set<string>()
  for (const m of MUTANTS) {
    if (seen.has(m.name)) bad.push({ name: m.name, why: "名字重复（报告里无法区分是哪一条）" })
    seen.add(m.name)
    if (m.find === "") bad.push({ name: m.name, why: "`find` 为空" })
    if (m.find === m.replace) bad.push({ name: m.name, why: "`replace` 与 `find` 相同（不是变异）" })
    const n = countOccurrences(origCore, m.find)
    if (n === 0) bad.push({ name: m.name, why: "`find` 未在 status-core.ts 中命中（多半是源码已改而清单没跟）" })
    if (n > 1) bad.push({ name: m.name, why: `\`find\` 命中 ${n} 次（不唯一，施加结果不确定）` })
  }
  return bad
}

/* ------------------------------------------------------------------ *
 * 主流程
 * ------------------------------------------------------------------ */

/** 单条变异的结果记录。 */
interface Result {
  mutant: Mutant
  killed: boolean
  /** 被谁杀的：`test` / `typecheck` / 空（存活） */
  by: string
  /** 人类可读的证据：`pass=34 fail=0` 这种。 */
  detail: string
  /** 环境异常（超时/无法解析输出）——既不算杀也不算存活，直接判整台失败。 */
  harnessError?: string
}

const selected = only ? MUTANTS.filter((m) => m.name.includes(only)) : MUTANTS
const results: Result[] = []
let exitCode = 0

/**
 * 主流程包在函数里：**提前退出不用 `process.exit()`**——那会跳过 `finally`、把临时目录留在
 * `%TEMP%` 里（跑一次泄漏一份源码副本）。改为 `return`，控制流自然流过 `finally` 的清理，
 * 末尾再统一 `process.exit(exitCode)`。
 */
function main(): void {
  // `--only` 匹配不到任何变异体时**不能静默通过**：一条都没跑却退出 0，会被读成「全过」
  if (selected.length === 0) {
    console.error(`❌ --only ${only} 未匹配任何变异体（清单共 ${MUTANTS.length} 条）——没有跑任何变异，不构成证据`)
    exitCode = 1
    return
  }

  /* ---- ① 清单自检 ---- */
  const invalid = validate()
  if (invalid.length > 0) {
    console.error("❌ 变异体清单失效（未跑任何变异，先修清单）：")
    for (const v of invalid) console.error(`   - ${v.name}：${v.why}`)
    exitCode = 1
    return
  }

  /* ---- ② 基线：未变异的矩阵必须绿 ---- */
  writeFileSync(corePath, origCore, "utf8")
  copyFileSync(MATRIX_SRC, matrixPath)
  writeFileSync(tsconfigPath, JSON.stringify(TSCONFIG, null, 2), "utf8")
  const baseline = runMatrix(matrixPath)
  if (baseline.crashed || baseline.fail !== 0) {
    console.error(
      `❌ 基线不绿（未变异的矩阵本身就没过：pass=${baseline.pass} fail=${baseline.fail}）——` +
        `\n   此时任何变异都会"被杀"，结论无意义。先修好矩阵再跑变异台。`,
    )
    if (printRaw) console.error(baseline.raw.slice(-4000))
    exitCode = 1
    return
  }
  console.log(
    `基线：矩阵未变异时 pass=${baseline.pass} fail=${baseline.fail}（绿）；` +
      `本次 ${selected.length}/${MUTANTS.length} 条变异体\n`,
  )

  /* ---- ③ 逐条施加变异（串行；理由见文件头②） ---- */
  for (const m of selected) {
    writeFileSync(corePath, origCore.replace(m.find, m.replace), "utf8")
    const t = runMatrix(matrixPath)
    if (t.crashed) {
      results.push({ mutant: m, killed: false, by: "", detail: "测试进程异常（无法解析输出/超时）", harnessError: t.raw.slice(-600) })
      continue
    }
    let by = t.fail > 0 ? "test" : ""
    let detail = `pass=${t.pass} fail=${t.fail}`
    if (!by) {
      const tc = runTypecheck(tsconfigPath)
      if (tc.status === null) {
        results.push({ mutant: m, killed: false, by: "", detail: "typecheck 进程异常", harnessError: tc.raw.slice(-600) })
        continue
      }
      if (tc.status !== 0) {
        by = "typecheck"
        detail += ` / tsc 报错（${(tc.raw.split("\n")[0] ?? "").slice(0, 90)}）`
      }
    }
    results.push({ mutant: m, killed: by !== "", by: by || "存活", detail })
  }
}

try {
  main()
} finally {
  // 副本用完即删。**真源码从未被写入**——本执行器不产生脏工作区的可能。
  if (!process.env.GEBAI_MUTATION_KEEP) rmSync(workDir, { recursive: true, force: true })
  else console.log(`[keep] 临时副本保留在 ${workDir}（GEBAI_MUTATION_KEEP 已设）`)
}

/* ------------------------------------------------------------------ *
 * 报告与判定
 * ------------------------------------------------------------------ */

/*
 * 提前退出（清单失效 / 基线不绿）时 `results` 是空的——**此时不打印结果汇总**：一张空的汇总表
 * 紧跟着「❌ 清单失效」会让人以为「跑了但全过」，而真实情况是**一条都没跑**。
 */
if (results.length === 0) process.exit(exitCode)

const byKind = (k: Mutant["kind"]): Result[] => results.filter((r) => r.mutant.kind === k)
const A = byKind("code")
const B = byKind("benign")
const C = byKind("branch")
const Dblind = byKind("blind")
const Dres = byKind("residual")

console.log("==================== 变异验证结果 ====================")
for (const r of results) {
  const want = expectsKill(r.mutant.kind) ? "应杀" : "应活"
  const got = r.killed ? "杀死" : "存活"
  const mark = got === (want === "应杀" ? "杀死" : "存活") ? "  " : "!!"
  console.log(`${mark}${got}（${want}）[${r.mutant.kind.padEnd(8)}] ${r.mutant.name}\n      ${r.detail}${r.killed ? `  被 ${r.by} 检出` : ""}`)
}

const count = (rs: Result[], killed: boolean): number => rs.filter((r) => r.killed === killed).length
console.log("")
console.log(`A 回归变异（应全杀）      : ${count(A, true)}/${A.length}`)
console.log(`B 良性重构（应全存活）    : ${count(B, false)}/${B.length}`)
console.log(`C 类型化新分支（应全拦）  : ${count(C, true)}/${C.length}`)
console.log(`D 闭集机关（应扫出）      : ${count(Dblind, true)}/${Dblind.length}`)
console.log(`  已知盲区（应存活，如实记录）: ${count(Dres, false)}/${Dres.length}`)

const problems: string[] = [
  ...A.filter((r) => !r.killed).map((r) => `回归变异逃逸（该规则没有断言在守）：${r.mutant.name}`),
  ...B.filter((r) => r.killed).map((r) => `良性重构被误杀（下一个人会去删断言）：${r.mutant.name}`),
  ...C.filter((r) => !r.killed).map((r) => `类型化新分支未被拦（矩阵会静默漏掉它）：${r.mutant.name}`),
  ...Dblind.filter((r) => !r.killed).map((r) => `闭集机关未扫出（应已堵）：${r.mutant.name}`),
  ...Dres.filter((r) => r.killed).map((r) => `残余盲区已堵住——可把它改归 blind（好消息，同步 DESIGN.md「已知盲区」）：${r.mutant.name}`),
  ...results.filter((r) => r.harnessError).map((r) => `环境异常（既非杀也非活，结论不可信）：${r.mutant.name}`),
]
if (problems.length > 0) {
  exitCode = 1
  console.log(`\n❌ 不符期望 ${problems.length} 条：`)
  for (const p of problems) console.log(`   - ${p}`)
  if (printRaw) {
    for (const r of results.filter((x) => x.harnessError)) console.error(`\n[raw] ${r.mutant.name}\n${r.harnessError}`)
  }
} else {
  console.log("\n✅ 全部符合期望（回归变异全杀、良性重构全存活、新分支被拦、盲区如实记录）")
}
console.log("\n产品源码未被写入（变异只发生在临时副本）——`git status` 不应出现任何产品文件改动。")
process.exit(exitCode)
