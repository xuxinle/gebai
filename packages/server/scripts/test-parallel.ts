/**
 * 并行测试运行器：把 packages/server 的全部 *.test.ts 按轮转（round-robin）分成 N 个分片，
 * 并行启动 N 个 `bun test` 子进程执行（bun test 单进程内测试文件串行执行，是全套件耗时的主因；
 * 各测试文件相互独立、端口/临时目录均动态分配，并行安全）。
 *
 * - 默认分片数：min(8, max(2, CPU 逻辑核数))；`--shards=N` 参数或 `GEBAI_TEST_SHARDS` 环境变量可覆盖
 * - 透传模式：带任何其他参数（文件路径 / `-t` 过滤 / `--coverage` 等）时退回单进程 `bun test` 原样执行
 *   （定向运行本身规模小，分片无收益且与过滤/覆盖率语义纠缠）
 * - 输出：每个子进程的输出逐行加 `[i/N]` 前缀流式透传；任一分片失败即以非零码退出
 */

import { readdirSync } from "node:fs"
import { join, relative } from "node:path"

const pkgRoot = join(import.meta.dirname, "..")

/** 递归收集 src 下全部测试文件（排序保证分片确定性：新增/删除文件只影响受影响的轮转位）。 */
function collectTestFiles(): string[] {
  const out: string[] = []
  const walk = (dir: string) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, e.name)
      if (e.isDirectory()) walk(p)
      else if (e.isFile() && e.name.endsWith(".test.ts")) out.push(relative(pkgRoot, p).replace(/\\/g, "/"))
    }
  }
  walk(join(pkgRoot, "src"))
  return out.sort()
}

function defaultShards(): number {
  const cores = navigator.hardwareConcurrency || 4
  return Math.max(2, Math.min(8, cores))
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

/** 逐行流式透传子进程输出，行首加分片前缀（部分行缓冲到换行再输出，避免并行交叉截断行）。 */
async function pump(tag: string, stream: ReadableStream<Uint8Array> | undefined): Promise<void> {
  if (!stream) return
  const reader = stream.getReader()
  const dec = new TextDecoder()
  let buf = ""
  const flush = (line: string) => process.stdout.write(`${tag} ${line}\n`)
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

async function runShard(tag: string, files: string[]): Promise<number> {
  const proc = Bun.spawn({ cmd: [process.execPath, "test", ...files], cwd: pkgRoot, stdout: "pipe", stderr: "pipe" })
  await Promise.all([pump(tag, proc.stdout as ReadableStream<Uint8Array>), pump(tag, proc.stderr as ReadableStream<Uint8Array>)])
  await proc.exited // 流关闭与进程退出在 Windows 上不同步：必须显式等待，否则 exitCode 为 null
  return proc.exitCode ?? 1
}

const args = process.argv.slice(2)
const { shards, rest } = resolveShards(args)

// 带其他参数（文件路径 / -t 过滤 / --coverage 等）：退回单进程原样执行
if (rest.length) {
  const proc = Bun.spawn({ cmd: [process.execPath, "test", ...rest], cwd: pkgRoot, stdout: "inherit", stderr: "inherit" })
  await proc.exited
  process.exit(proc.exitCode ?? 1)
}

const files = collectTestFiles()
if (files.length <= 1) {
  const proc = Bun.spawn({ cmd: [process.execPath, "test", ...files], cwd: pkgRoot, stdout: "inherit", stderr: "inherit" })
  await proc.exited
  process.exit(proc.exitCode ?? 1)
}

const n = Math.min(shards, files.length)
const buckets: string[][] = Array.from({ length: n }, () => [])
files.forEach((f, i) => buckets[i % n].push(f))

const codes = await Promise.all(buckets.map((fs, i) => runShard(`[${i + 1}/${n}]`, fs)))
const failed = codes.map((c, i) => (c !== 0 ? i + 1 : null)).filter((x): x is number => x !== null)
if (failed.length) {
  console.error(`\n✘ 测试失败：分片 ${failed.join(", ")} 存在失败用例（见上方 [i/${n}] 前缀输出）`)
  process.exit(1)
}
console.log(`\n✔ 全部 ${files.length} 个测试文件通过（${n} 分片并行）`)
