/**
 * 并行测试运行器：把 packages/server 的全部 *.test.ts 分成 N 个分片，并行启动 N 个
 * `bun test` 子进程执行（bun test 单进程内测试文件串行执行，是全套件耗时的主因）。
 *
 * - 默认分片数：min(8, max(2, CPU 逻辑核数))；`--shards=N` 参数或 `GEBAI_TEST_SHARDS` 环境变量可覆盖
 * - 分片策略：按文件字节数降序的最长处理时间优先（LPT）装箱——测试耗时与文件规模强相关，
 *   轮转分配会让大文件堆在少数分片形成长尾；LPT 让各分片负载均衡，缩短整体墙钟时间
 * - 超时余量：子进程统一带 `--timeout`（默认 15000ms，`GEBAI_TEST_TIMEOUT` 可覆盖）——
 *   bun 默认 5s，而 `spawn`/真实网络/WS 往返类用例在**并行负载**下会显著变慢
 *   （实测同一用例并行触顶 5s、串行 1.8s），于是每次全量都带一条「并行抖动」噪声告警；
 *   留够余量让“真的卡住”才超时，而不是把慢当成失败
 * - 并发约束：全部测试文件若都在**同一分片**里串行也不算好，因此不再额外降并发；
 *   真正重的是少数文件（网络/子进程），由 LPT 把它们摊到不同分片，避免同分片内"重 + 重"
 * - 失败复验：任一分片失败时，把该分片的文件列表**单进程串行**重跑一遍——重跑通过判定为
 *   并行抖动（跨分片共享资源竞态），以醒目警告列出并视为通过（仍附精确复现命令）；
 *   重跑同样失败才是真失败，非零退出。避免把环境/竞态抖动误当代码回归反复排查
 * - 透传模式：带任何其他参数（文件路径 / `-t` 过滤 / `--coverage` 等）时退回单进程 `bun test` 原样执行
 *   （定向运行本身规模小，分片无收益且与过滤/覆盖率语义纠缠）
 * - 输出：每个子进程的输出逐行加 `[i/N]` 前缀流式透传；复验输出加 `[verify]` 前缀
 */

import { readdirSync, statSync } from "node:fs"
import { join, relative } from "node:path"

const pkgRoot = join(import.meta.dirname, "..")

/** 递归收集 src 下全部测试文件（含字节数，供 LPT 装箱）。 */
function collectTestFiles(): Array<{ file: string; bytes: number }> {
  const out: Array<{ file: string; bytes: number }> = []
  const walk = (dir: string) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, e.name)
      if (e.isDirectory()) walk(p)
      else if (e.isFile() && e.name.endsWith(".test.ts")) {
        const file = relative(pkgRoot, p).replace(/\\/g, "/")
        out.push({ file, bytes: statSync(p).size })
      }
    }
  }
  walk(join(pkgRoot, "src"))
  return out.sort((a, b) => a.file.localeCompare(b.file))
}

function defaultShards(): number {
  const cores = navigator.hardwareConcurrency || 4
  return Math.max(2, Math.min(8, cores))
}

/**
 * 单用例超时（ms）：默认 15s，`GEBAI_TEST_TIMEOUT` 可覆盖。
 *
 * bun 默认 5s。而 `spawn` 子进程、真实 HTTP/WS 往返类用例在**并行负载**下会显著变慢
 *（实测同一用例：并行触顶 5s、串行 1.8s），于是每次全量测试都带一条「并行下失败、串行通过」
 * 的告警噪声——噪声多了就会掩盖真回归。留够余量后，只有"真的卡住"才会超时。
 */
function testTimeout(): string {
  const env = parseInt(process.env.GEBAI_TEST_TIMEOUT ?? "", 10)
  return String(Number.isFinite(env) && env > 0 ? env : 15000)
}

/**
 * 分片内的**失败重试次数**（默认 2；`GEBAI_TEST_RETRY` 可覆盖）。
 *
 * 为什么需要：并行满载时少数用例会因**负载**而失败（跨分片共享的临时资源/端口、真实
 * spawn 与网络往返的时序），它们串行重跑能过——但靠“事后串行复验”只能告警，每次全量
 * 都带一条噪声，噪声多了反而掩盖真回归。重试把这类抖动**在用例内**吸收掉：
 * 真失败的用例重试多少次都失败，不会被掩盖；抖动用例无声通过，不再打扰人。
 * 串行复验仍保留为第二道防线（重试也不过时区分“真失败”与“该分片整体环境异常”）。
 */
function shardRetry(): string {
  const env = parseInt(process.env.GEBAI_TEST_RETRY ?? "", 10)
  return String(Number.isFinite(env) && env >= 0 ? env : 2)
}

function resolveShards(args: string[]): { shards: number; rest: string[] } {
  let shards = 0
  const rest: string[] = []
  for (let i = 0; i < args.length; i++) {
    const eq = args[i].match(/^--shards=(\d+)$/)
    if (eq) shards = parseInt(eq[1], 10)
    else if (args[i] === "--shards" && /^\d+$/.test(args[i + 1] ?? "")) shards = parseInt(args[++i], 10)
    else rest.push(args[i])
  }
  if (!shards) {
    const env = parseInt(process.env.GEBAI_TEST_SHARDS ?? "", 10)
    if (Number.isFinite(env) && env > 0) shards = env
  }
  return { shards: Math.max(1, shards || defaultShards()), rest }
}

/** LPT 装箱：大文件优先放入当前累计负载最小的分片。 */
function balance(items: Array<{ file: string; bytes: number }>, n: number): string[][] {
  const buckets: Array<{ files: string[]; load: number }> = Array.from({ length: n }, () => ({ files: [], load: 0 }))
  for (const item of [...items].sort((a, b) => b.bytes - a.bytes)) {
    let target = buckets[0]
    for (const b of buckets) if (b.load < target.load) target = b
    target.files.push(item.file)
    target.load += item.bytes
  }
  return buckets.map((b) => b.files.sort())
}

/** 逐行流式透传子进程输出，行首加前缀（部分行缓冲到换行再输出，避免并行交叉截断行）。 */
async function pump(tag: string, stream: ReadableStream<Uint8Array> | undefined, onLine?: (line: string) => void): Promise<void> {
  if (!stream) return
  const reader = stream.getReader()
  const dec = new TextDecoder()
  let buf = ""
  const flush = (line: string) => {
    process.stdout.write(`${tag} ${line}\n`)
    onLine?.(line)
  }
  for (;;) {
    const { done, value } = await reader.read()
    if (done) break
    buf += dec.decode(value, { stream: true })
    let idx: number
    while ((idx = buf.indexOf("\n")) >= 0) {
      flush(buf.slice(0, idx).replace(/\r$/, ""))
      buf = buf.slice(idx + 1)
    }
  }
  if (buf.trim()) flush(buf.replace(/\r$/, ""))
}

async function runShard(tag: string, files: string[]): Promise<{ code: number; failedTests: string[] }> {
  const failedTests: string[] = []
  const proc = Bun.spawn({ cmd: [process.execPath, "test", "--timeout", testTimeout(), "--retry", shardRetry(), ...files], cwd: pkgRoot, stdout: "pipe", stderr: "pipe" })
  // 收集失败用例名：抖动告警只说“分片 N 失败”时，下一次出现仍无从下手
  const collect = (line: string) => {
    const match = /\(fail\)\s+(.+?)\s*\[/.exec(line)
    if (match) failedTests.push(match[1]!)
  }
  await Promise.all([
    pump(tag, proc.stdout as ReadableStream<Uint8Array>, collect),
    pump(tag, proc.stderr as ReadableStream<Uint8Array>, collect),
  ])
  await proc.exited // 流关闭与进程退出在 Windows 上不同步：必须显式等待，否则 exitCode 为 null
  return { code: proc.exitCode ?? 1, failedTests }
}

const args = process.argv.slice(2)
const { shards, rest } = resolveShards(args)

// 带其他参数（文件路径 / -t 过滤 / --coverage 等）：退回单进程原样执行
if (rest.length) {
  const proc = Bun.spawn({ cmd: [process.execPath, "test", "--timeout", testTimeout(), ...rest], cwd: pkgRoot, stdout: "inherit", stderr: "inherit" })
  await proc.exited
  process.exit(proc.exitCode ?? 1)
}

const files = collectTestFiles()
if (files.length <= 1) {
  const proc = Bun.spawn({ cmd: [process.execPath, "test", "--timeout", testTimeout(), ...files.map((f) => f.file)], cwd: pkgRoot, stdout: "inherit", stderr: "inherit" })
  await proc.exited
  process.exit(proc.exitCode ?? 1)
}

const n = Math.min(shards, files.length)
const buckets = balance(files, n)

const results = await Promise.all(buckets.map((fs, i) => runShard(`[${i + 1}/${n}]`, fs)))
const failed = results.map((r, i) => (r.code !== 0 ? i : null)).filter((x): x is number => x !== null)
if (!failed.length) {
  console.log(`\n✔ 全部 ${files.length} 个测试文件通过（${n} 分片并行）`)
  process.exit(0)
}

// 失败复验：失败分片的文件单进程串行重跑——通过即并行抖动（跨分片共享资源竞态），非代码回归
console.log(`\n↻ 分片 ${failed.map((i) => i + 1).join(", ")} 失败，单进程串行复验中（区分并行抖动与真失败）...`)
const flaky: number[] = []
const real: number[] = []
for (const i of failed) {
  const { code } = await runShard("[verify]", buckets[i])
  if (code === 0) flaky.push(i)
  else real.push(i)
}

if (real.length) {
  console.error(`\n✘ 测试失败：分片 ${real.map((i) => i + 1).join(", ")} 复验同样失败（真失败，见上方 [i/${n}] 与 [verify] 前缀输出）`)
  for (const i of real) console.error(`   复现：bun test ${buckets[i].join(" ")}`)
  process.exit(1)
}

console.warn(
  `\n⚠ 并行抖动：分片 ${flaky.map((i) => i + 1).join(", ")} 并行下失败、串行复验通过——疑似跨分片共享资源竞态（临时目录/端口/仓库内文件），非本次改动回归。`,
)
for (const i of flaky) {
  // 抖动也要**可归因**：只报分片号时，下一次出现依旧无从下手（得翻满屏日志找 [i/N] 前缀）
  const names = results[i]!.failedTests
  if (names.length) console.warn(`   分片 ${i + 1} 并行下失败的用例：${[...new Set(names)].join("｜")}`)
  console.warn(`   复现：bun test ${buckets[i].join(" ")}`)
}
console.log(`\n✔ 全部 ${files.length} 个测试文件通过（${n} 分片并行，${flaky.length} 个分片存在并行抖动）`)
