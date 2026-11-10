/**
 * 并行测试运行器（packages/server 全量）。
 *
 * 执行体交给 Bun 原生 `--parallel`：它按**进程池**跑测试文件（每个 worker 独立进程），
 * 并按 `--timings` 记录的**实测每文件耗时**排序（慢文件先跑，避免长尾），比手写分片更准。
 *
 * 为什么不再自己分片：早先的实现把文件按**字节数**LPT 装箱分给 N 个 `bun test` 子进程。字节数
 * 与耗时的相关性很差（实测：`git/service.test.ts` 37s 但文件不大，一些大文件几秒跑完），装箱
 * 只能把长尾摊开而不能消除长尾；而且管道式多子进程的调度、输出交织与失败归因都要自己维护。
 * 原生 `--parallel` 用实测耗时排序 + 动态取下一个文件，天然没有分片长尾问题。
 *
 * 关键参数与约定：
 * - 并发：默认 12（实测最优：与 16 同速但抖动更少；见 DEFAULT_PARALLEL）；`--parallel=N` 或
 *   `GEBAI_TEST_PARALLEL=N` 可覆盖（显式值不做上限钳制）。
 *   不按文件数分批、不限制单分片——worker 满了才排队，空闲即取下一个文件。
 * - 超时余量：`--timeout` 默认 20000ms（`GEBAI_TEST_TIMEOUT` 可覆盖）。bun 默认 5s，而
 *   spawn 子进程 / 真实 HTTP / WS 往返类用例在**并行满载**下会显著变慢，留够余量让「真的卡住」
 *   才超时，而不是把慢当失败。
 * - 耗时采样：`--timings` 指向 `.test-timings.json`（仓内 gitignore）；每次运行结束用实测值更新，
 *   下次运行据此排序。首次运行没有该文件时按默认顺序跑，跑完即建立基线。
 * - 失败复验：任一轮失败时，把**失败的用例**以 `-t` 精确定位重跑（单进程、串行）——重跑通过判定
 *   为并行抖动（跨文件共享资源/时序竞态），以醒目警告列出并附复现命令；复验同样失败才是真失败。
 *   抖动不会静默通过：始终打印告警，但只有真失败才非零退出（避免噪声掩盖真回归，也避免误报）。
 * - 透传模式：带其他参数（文件路径 / `-t` 过滤 / `--coverage` 等）时退回单进程 `bun test` 原样执行
 *   （定向运行规模小，并行无收益且与过滤/覆盖率语义纠缠）。
 */

import { existsSync } from "node:fs"
import { join } from "node:path"

const pkgRoot = join(import.meta.dirname, "..")
/** 耗时采样文件（每次运行更新，下次据此排序；不提交）。 */
const TIMINGS_FILE = join(pkgRoot, ".test-timings.json")

/**
 * 并行 worker 数：取**有收益的上限**，而不是核数。
 *
 * 实测（28 核机器，128 文件 / 2157 用例，同一台机、同一份代码）：
 * | worker | 平均耗时 | 完全干净轮次占比 |
 * |--------|---------|------------------|
 * | 8      | 46.5s   | 高              |
 * | 12     | 40.1s   | 4/5              |
 * | 16     | 41.5s   | 3/4              |
 * | 28     | 40.4s   | 明显下降（多个网络/子进程用例随机失败） |
 *
 * 12 与 16 耗时相当（差在噪声范围内），但 12 的抖动更少；28 把核数跑满后等待型用例的
 * 调度延迟被拉长，开始出现随机失败。故默认取 12：**速度已到平台期，多开只买抖动**。
 * `GEBAI_TEST_PARALLEL` 或 `--parallel=N` 可上调（升级机器后自行验证抖动再调）。
 */
const DEFAULT_PARALLEL = 12

function resolveParallel(args: string[]): { n: number; rest: string[] } {
  let n = 0
  const rest: string[] = []
  for (let i = 0; i < args.length; i++) {
    const eq = args[i].match(/^--parallel=(\d+)$/)
    if (eq) n = parseInt(eq[1], 10)
    else if (args[i] === "--workers" && /^\d+$/.test(args[i + 1] ?? "")) n = parseInt(args[++i], 10)
    else rest.push(args[i])
  }
  if (!n) {
    const env = parseInt(process.env.GEBAI_TEST_PARALLEL ?? "", 10)
    if (Number.isFinite(env) && env > 0) n = env
  }
  // 显式指定（参数/环境变量）尊重原值；缺省用实测最优值
  if (!n) n = DEFAULT_PARALLEL
  return { n: Math.max(1, n), rest }
}

/**
 * 单用例超时（ms）：默认 20s，`GEBAI_TEST_TIMEOUT` 可覆盖。
 *
 * bun 默认 5s；spawn 子进程 / 真实网络往返类用例在并行满载下会显著变慢（实测同一用例并行
 * 触顶 5s、串行 1.8s）。这里等的是「真正的卡住」，不是「慢」。
 */
function testTimeout(): string {
  const env = parseInt(process.env.GEBAI_TEST_TIMEOUT ?? "", 10)
  return String(Number.isFinite(env) && env > 0 ? env : 20000)
}

const args = process.argv.slice(2)
const { n: parallel, rest } = resolveParallel(args)

/** 从 `(fail) 名字` 行取出可用于 `-t` 的**用例名**。
 *
 *  bun 的失败行是 `(fail) <describe 链> > <用例名> [耗时]`，而 `-t` 只匹配 `test()` 自己的
 *  名字（不匹配 describe 前缀）——直接拿整行去 -t 会「matched 0 tests」，复验形同虚设
 *  （反而把真抖动误判成真失败）。故只取最后一段 ` > ` 之后的部分。 */
function parseFailedTestName(line: string): string | null {
  const m = /\(fail\)\s+(.+?)(?:\s*\[\d|\s*\(attempt|\s*$)/.exec(line)
  if (!m) return null
  const full = m[1]!.trim()
  const cut = full.lastIndexOf(" > ")
  return cut >= 0 ? full.slice(cut + 3).trim() : full
}

/** 跑一轮测试，返回退出码与失败用例名（从输出里解析）。 */
async function runOnce(extra: string[]): Promise<{ code: number; failedTests: string[] }> {
  const cmd = [process.execPath, "test"]
  // 只有全量运行才用并行与耗时采样；定向复验走单进程串行，避免再次触发并行竞态
  if (!extra.length) cmd.push(`--parallel=${parallel}`, `--timings=${TIMINGS_FILE}`, "--update-timings")
  cmd.push("--timeout", testTimeout(), ...extra)
  const proc = Bun.spawn({ cmd, cwd: pkgRoot, stdout: "pipe", stderr: "pipe" })
  const failedTests: string[] = []
  const pump = async (stream: ReadableStream<Uint8Array> | undefined) => {
    if (!stream) return
    const reader = stream.getReader()
    const dec = new TextDecoder()
    let buf = ""
    const emit = (line: string) => {
      process.stdout.write(`${line}\n`)
      const name = parseFailedTestName(line)
      if (name) failedTests.push(name)
    }
    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      buf += dec.decode(value, { stream: true })
      let idx: number
      while ((idx = buf.indexOf("\n")) >= 0) {
        emit(buf.slice(0, idx).replace(/\r$/, ""))
        buf = buf.slice(idx + 1)
      }
    }
    if (buf.trim()) emit(buf.replace(/\r$/, ""))
  }
  await Promise.all([pump(proc.stdout as ReadableStream<Uint8Array>), pump(proc.stderr as ReadableStream<Uint8Array>)])
  await proc.exited // Windows 上流关闭与进程退出不同步：必须显式等待，否则 exitCode 为 null
  return { code: proc.exitCode ?? 1, failedTests }
}

// 带其他参数（文件路径 / -t 过滤 / --coverage 等）：单进程原样执行
if (rest.length) {
  const proc = Bun.spawn({ cmd: [process.execPath, "test", "--timeout", testTimeout(), ...rest], cwd: pkgRoot, stdout: "inherit", stderr: "inherit" })
  await proc.exited
  process.exit(proc.exitCode ?? 1)
}

const t0 = Date.now()
const first = await runOnce([])
const wall = ((Date.now() - t0) / 1000).toFixed(1)

if (first.code === 0) {
  console.log(`\n✔ 全量通过（${parallel} 个 worker 并行，${wall}s${existsSync(TIMINGS_FILE) ? "，耗时采样已更新" : ""}）`)
  process.exit(0)
}

// 失败复验：用 -t 精确定位失败的用例，单进程串行重跑——通过即并行抖动，仍失败才是真失败
const names = [...new Set(first.failedTests)]
if (!names.length) {
  // 没有可解析的用例名不等于“无需复验”：加载期错误（如某文件 import 就抛）/未捕获异常也会让
  // 整轮失败，但它们不属于任何用例，**不能归为抖动**——直接如实失败。
  console.error(`\n✘ 测试失败（${wall}s），但失败不属于任何测试用例（加载期错误或未捕获异常），不进入抖动复验`)
  console.error(`   查看上方日志中以 “# Unhandled error” / “error:” 开头的段落`)
  process.exit(1)
}

console.log(`\n↻ ${names.length} 个用例在并行下失败（${wall}s），单进程串行复验中（区分并行抖动与真失败）...`)
const flaky: string[] = []
const real: string[] = []
for (const name of names) {
  // 转义正则元字符：用例名里常含 ()/[]/* 等，直接拼进 -t 会被当正则
  const pattern = name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
  const r = await runOnce(["-t", pattern])
  if (r.code === 0) flaky.push(name)
  else real.push(name)
}

if (real.length) {
  console.error(`\n✘ 真失败 ${real.length} 个（串行复验同样失败）：`)
  for (const name of real) console.error(`   · ${name}`)
  console.error(`\n   复现：bun test -t "${real[0]!.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}"`)
  process.exit(1)
}

console.warn(`\n⚠ 并行抖动 ${flaky.length} 个：并行下失败、串行复验通过（跨文件共享资源或时序竞态，非本次改动回归）：`)
for (const name of flaky) console.warn(`   · ${name}`)
console.warn(`   复现：bun test -t "${flaky[0]!.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}"`)
console.log(`\n✔ 全量通过（${parallel} 个 worker 并行，${wall}s，${flaky.length} 个用例存在并行抖动）`)
process.exit(0)
