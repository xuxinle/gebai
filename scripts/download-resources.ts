/**
 * 资源目录下载器：按 `scripts/resources.manifest.json` 清单把模型等大体积资源拉到 `<仓库根>/resources/`
 * （布局与资源子仓库一致，运行时 drop-in 即生效；资源目录约定见 packages/agents/src/core/cv/resources.ts）。
 *
 * 用法：
 *   bun run resources:download                                下载缺失资源（已存在且校验通过则跳过）
 *   bun run resources:download --check                        只校验现状不下载（缺失必选资源时非零退出）
 *   bun run resources:download --force                        忽略现有文件重新下载
 *   bun run resources:download --only "models/cv/ocr/*"       只处理匹配条目（可多次，glob）
 *   bun run resources:download --source modelscope,hf-mirror  指定来源 kind 优先级
 *   bun run resources:download --skip-vendor                  跳过 vendor 依赖（packages 段）
 *
 * 环境变量：GEBAI_RESOURCES_DIR 资源目录（缺省 <仓库根>/resources）；GEBAI_RESOURCE_SOURCE 同 --source。
 *
 * 单文件按清单 sources 顺序尝试（失败换下一个源，.part 临时文件 + Range 断点续传），完成后按
 * size/sha256 校验；清单条目的 derive 说明备用获取途径（从模型元数据提取字典、从权重导出 ONNX）。
 */
import { existsSync, mkdirSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from "node:fs"
import { dirname, join } from "node:path"
import { parseOnnxMetadata } from "../packages/agents/src/core/cv/onnx-meta"

interface ResourceSource {
  kind: string
  url: string
}

interface ResourceEntry {
  path: string
  size?: number
  sha256?: string
  required?: boolean
  license?: string
  description?: string
  sources?: ResourceSource[]
  derive?: { method?: string; from?: string; command?: string; note?: string }
}

interface ResourcePackage {
  dir: string
  manager?: string
  required?: boolean
  description?: string
  dependencies?: Record<string, string>
}

interface Manifest {
  version: number
  entries: ResourceEntry[]
  packages?: ResourcePackage[]
}

const repoRoot = join(import.meta.dirname, "..")
const manifestPath = join(import.meta.dirname, "resources.manifest.json")

interface Options {
  check: boolean
  force: boolean
  skipVendor: boolean
  only: string[]
  sources: string[]
}

function parseArgs(argv: string[]): Options {
  const opts: Options = {
    check: false,
    force: false,
    skipVendor: false,
    only: [],
    sources: String(process.env.GEBAI_RESOURCE_SOURCE ?? "")
      .split(",")
      .map((s) => s.trim())
      .filter(Boolean),
  }
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]!
    if (a === "--check") opts.check = true
    else if (a === "--force") opts.force = true
    else if (a === "--skip-vendor") opts.skipVendor = true
    else if (a === "--only") opts.only.push(String(argv[++i] ?? ""))
    else if (a.startsWith("--only=")) opts.only.push(a.slice(7))
    else if (a === "--source") opts.sources.push(...String(argv[++i] ?? "").split(",").map((s) => s.trim()).filter(Boolean))
    else if (a.startsWith("--source=")) opts.sources.push(...a.slice(9).split(",").map((s) => s.trim()).filter(Boolean))
    else if (a === "--help" || a === "-h") {
      console.log(readFileSync(import.meta.filename, "utf8").split("*/")[0]!.replace(/^\/\*\*?/, "").trim())
      process.exit(0)
    } else {
      console.error(`未知参数: ${a}（--help 查看用法）`)
      process.exit(2)
    }
  }
  return opts
}

/** glob → 正则（`**` 跨目录、`*` 单层、`?` 单字符）。 */
function globToRegExp(glob: string): RegExp {
  const body = glob
    .replace(/[.+^${}()|[\]\\]/g, "\\$&")
    .replace(/\*\*/g, "\u0000")
    .replace(/\*/g, "[^/]*")
    .replace(/\?/g, "[^/]")
    .replace(/\u0000/g, ".*")
  return new RegExp(`^${body}$`)
}

function selected(entry: ResourceEntry, opts: Options): boolean {
  if (opts.only.length === 0) return true
  return opts.only.some((g) => globToRegExp(g).test(entry.path))
}

/** 来源排序：--source/GEBAI_RESOURCE_SOURCE 指定的 kind 优先（按给定次序），其余按清单顺序。 */
function orderedSources(entry: ResourceEntry, prefer: string[]): ResourceSource[] {
  const sources = entry.sources ?? []
  if (prefer.length === 0) return sources
  const rank = (s: ResourceSource) => {
    const i = prefer.indexOf(s.kind)
    return i < 0 ? prefer.length : i
  }
  return sources.map((s, i) => ({ s, i })).sort((a, b) => rank(a.s) - rank(b.s) || a.i - b.i).map((x) => x.s)
}

function human(bytes: number): string {
  if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(2)} GB`
  if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(1)} MB`
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${bytes} B`
}

function sha256OfFile(path: string): string {
  const hasher = new Bun.CryptoHasher("sha256")
  const buf = readFileSync(path)
  hasher.update(buf)
  return hasher.digest("hex")
}

type VerifyResult = { ok: true } | { ok: false; reason: string }

/** 校验目标文件是否满足清单约定（大小/sha256；未声明则不校验该项）。 */
function verifyFile(path: string, entry: ResourceEntry): VerifyResult {
  if (!existsSync(path)) return { ok: false, reason: "缺失" }
  const size = statSync(path).size
  if (entry.size && size !== entry.size) return { ok: false, reason: `大小不符（本地 ${human(size)} / 预期 ${human(entry.size)}）` }
  if (entry.sha256) {
    const sha = sha256OfFile(path)
    if (sha !== entry.sha256) return { ok: false, reason: `sha256 不符（${sha.slice(0, 12)}… / 预期 ${entry.sha256.slice(0, 12)}…）` }
  }
  return { ok: true }
}

/** 流式下载（.part + Range 续传）：返回 "ok" / "size-mismatch" / 抛错。 */
async function downloadFrom(url: string, dest: string, expectedSize: number | undefined, label: string): Promise<void> {
  const part = `${dest}.part`
  mkdirSync(dirname(part), { recursive: true })
  let offset = existsSync(part) ? statSync(part).size : 0
  const headers: Record<string, string> = offset > 0 ? { Range: `bytes=${offset}-` } : {}
  const res = await fetch(url, { headers, signal: AbortSignal.timeout(1_800_000) })
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  if (offset > 0 && res.status !== 206) offset = 0 // 源不支持续传 → 从头写
  if (!res.body) throw new Error("响应无 body")
  const total = expectedSize ?? (Number(res.headers.get("content-length")) || 0) + offset
  const writer = Bun.file(part).writer({ highWaterMark: 1 << 20 })
  let written = offset
  let lastPrint = 0
  try {
    for await (const chunk of res.body) {
      writer.write(chunk)
      written += chunk.length
      const now = Date.now()
      if (now - lastPrint > 200) {
        lastPrint = now
        const pct = total ? ` ${Math.floor((written / total) * 100)}%` : ""
        process.stdout.write(`\r  ↓ ${label} ${human(written)}${total ? ` / ${human(total)}` : ""}${pct}   `)
      }
    }
    await writer.end()
  } catch (e) {
    try {
      await writer.end() // 关闭缓冲（失败不覆盖原始错误）
    } catch { /* 关闭异常忽略，保留下载错误 */ }
    throw e
  }
  process.stdout.write(`\r  ↓ ${label} ${human(written)}${total ? ` / ${human(total)}` : ""}    \n`)
  if (offset > 0 && written !== total && expectedSize && written !== expectedSize) {
    throw new Error(`传输不完整（${written}/${expectedSize}）`)
  }
  rmSync(dest, { force: true })
  renameSync(part, dest)
}

/** 备用获取：从 rec 模型内嵌 character 元数据提取字典（RapidOCR 约定）。 */
function deriveRecMetadata(entry: ResourceEntry, resourcesDir: string): boolean {
  const from = entry.derive?.from
  if (!from) return false
  const srcPath = join(resourcesDir, from)
  if (!existsSync(srcPath)) {
    console.log(`  · 无法派生（缺少 ${from}）`)
    return false
  }
  const meta = parseOnnxMetadata(new Uint8Array(readFileSync(srcPath)))
  const chars = meta.character
  if (!chars) {
    console.log(`  · 无法派生（${from} 无 character 元数据）`)
    return false
  }
  const target = join(resourcesDir, entry.path)
  mkdirSync(dirname(target), { recursive: true })
  writeFileSync(target, chars, "utf8")
  console.log(`  · 已从 ${from} 提取字典 → ${entry.path}`)
  return true
}

/** vendor 依赖安装（缺失即检测回落 wasm，故失败不阻断流程）。 */
function installVendor(pkg: ResourcePackage, resourcesDir: string): boolean {
  const dir = join(resourcesDir, pkg.dir)
  const deps = Object.entries(pkg.dependencies ?? {}).map(([n, v]) => `${n}@${v}`)
  if (deps.length === 0) return true
  mkdirSync(dir, { recursive: true })
  const pkgFile = join(dir, "package.json")
  if (!existsSync(pkgFile)) {
    writeFileSync(pkgFile, `${JSON.stringify({ name: "gebai-resources-vendor", private: true }, null, 2)}\n`)
  }
  const hasBun = Bun.which("bun") !== null
  const cmd = hasBun ? ["bun", "add", ...deps] : ["npm", "install", ...deps, "--no-audit", "--no-fund"]
  if (!hasBun && !Bun.which("npm")) {
    console.log("  ✗ bun/npm 均不可用——请手工安装依赖:", deps.join(" "))
    return false
  }
  console.log(`  ↓ ${hasBun ? "bun add" : "npm install"} ${deps.join(" ")}（${pkg.dir}）`)
  const proc = Bun.spawnSync(cmd, { cwd: dir, stdout: "pipe", stderr: "pipe" })
  if (proc.exitCode !== 0) {
    console.log(`  ✗ 依赖安装失败: ${proc.stderr.toString().trim().split("\n").slice(-3).join(" ")}`)
    return false
  }
  console.log("  ✓ 依赖就绪")
  return true
}

async function main(): Promise<void> {
  const opts = parseArgs(process.argv.slice(2))
  const manifest = JSON.parse(readFileSync(manifestPath, "utf8")) as Manifest
  const resourcesDir = process.env.GEBAI_RESOURCES_DIR?.trim() || join(repoRoot, "resources")
  console.log(`资源目录: ${resourcesDir}`)
  console.log(`清单: ${manifestPath}${opts.sources.length ? `（来源优先 ${opts.sources.join(" > ")}）` : ""}`)
  mkdirSync(resourcesDir, { recursive: true })

  let missing = 0
  let failed = 0
  for (const entry of manifest.entries) {
    if (!selected(entry, opts)) continue
    const target = join(resourcesDir, entry.path)
    const tag = entry.required === false ? "可选" : "必选"
    console.log(`\n[${tag}] ${entry.path}${entry.description ? ` — ${entry.description}` : ""}`)

    if (!opts.force) {
      const cur = verifyFile(target, entry)
      if (cur.ok) {
        console.log(`  ✓ 已存在${entry.sha256 ? "（校验通过）" : ""}`)
        continue
      }
      if (existsSync(target)) console.log(`  ! 现有文件不可用：${cur.reason}`)
    }
    if (opts.check) {
      console.log("  ✗ 缺失（--check 模式不下载）")
      if (entry.required === false) failed++
      else missing++
      continue
    }

    const sources = orderedSources(entry, opts.sources)
    let done = false
    for (const src of sources) {
      try {
        await downloadFrom(src.url, target, entry.size, src.kind)
        const check = verifyFile(target, entry)
        if (check.ok) {
          console.log(`  ✓ 完成（来源 ${src.kind}）`)
          done = true
          break
        }
        console.log(`  ✗ 校验失败：${check.reason}`)
        rmSync(target, { force: true })
      } catch (e) {
        rmSync(`${target}.part`, { force: true })
        console.log(`  ✗ 来源 ${src.kind} 失败：${e instanceof Error ? e.message : e}`)
      }
    }
    if (!done && entry.derive?.method === "rec-metadata") {
      if (deriveRecMetadata(entry, resourcesDir)) {
        const check = verifyFile(target, entry)
        if (check.ok) {
          console.log("  ✓ 完成（派生）")
          done = true
        } else {
          console.log(`  ✗ 派生结果不匹配：${check.reason}`)
        }
      }
    }
    if (!done) {
      if (entry.derive?.command) console.log(`  · 可自行制备：${entry.derive.command}${entry.derive.note ? `（${entry.derive.note}）` : ""}`)
      else if (entry.derive?.note) console.log(`  · ${entry.derive.note}`)
      if (entry.required === false) failed++
      else missing++
    }
  }

  if (manifest.packages?.length && !opts.check && !opts.skipVendor) {
    for (const pkg of manifest.packages) {
      console.log(`\n[依赖] ${pkg.dir} — ${pkg.description ?? ""}`)
      if (!installVendor(pkg, resourcesDir) && pkg.required !== false) failed++
    }
  } else if (manifest.packages?.length && opts.skipVendor) {
    console.log("\n[依赖] 已按 --skip-vendor 跳过（GPU 原生推理依赖；缺失时检测回落 wasm CPU）")
  }

  console.log(
    `\n汇总：必选缺失 ${missing}，可选未就绪 ${failed}${opts.check ? "（--check 模式）" : ""}`,
  )
  if (missing > 0) {
    console.log("提示：源码形态可运行 scripts/build-cv-embed.ts 下载并内嵌三件套；构建/运行也可用 GEBAI_CV_MODELS_DIR 指向其它目录。")
    process.exit(1)
  }
}

await main()
