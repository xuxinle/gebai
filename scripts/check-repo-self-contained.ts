#!/usr/bin/env bun
/**
 * 仓库自检：确认**仓库自包含**——没有"存在于工作树、却因 .gitignore 而从未入库"的源文件。
 *
 * 为什么需要这条检查：gitignore 的模式匹配路径的**任意一段**，所以一个为崩溃转储写的
 * 裸模式 `core` 会把 `include/st/core/`、`src/core/` 一起忽略掉——整个模块从未进过库，
 * 干净克隆根本编不过，而本地工作树看起来一切正常（文件就在那儿）。更糟的是提交信息会
 * 声称"新增 src/core/entry.cpp"，因为 `git add -A` 静默跳过被忽略的文件。
 *
 * 这条检查只在"克隆下来能用"这个语义上给出回答：**要么源文件全都在库里，要么报错**。
 * 判定方式：`git ls-files --others --ignored --exclude-standard` 列出被忽略的未跟踪文件，
 * 过滤出源码类扩展名，再排除已知的产物/外部依赖目录。
 *
 * 用法：`bun run check:repo`（已接进 `bun run build` 前置）。
 * 退出码：0 = 自包含；1 = 发现被忽略的源码（逐条打印，附排查提示）。
 */
import { spawnSync } from "node:child_process"
import { join } from "node:path"

const root = join(import.meta.dirname, "..")

/** 源码类扩展名：这些文件**必须**入库（生成物不走这些扩展名，故不会误报）。 */
const SOURCE_EXTENSIONS = new Set([
  ".ts",
  ".tsx",
  ".mts",
  ".cts",
  ".js",
  ".mjs",
  ".cjs",
  ".jsx",
  ".py",
  ".cpp",
  ".cc",
  ".cxx",
  ".hpp",
  ".hh",
  ".h",
  ".c",
  ".rs",
  ".go",
  ".java",
  ".kt",
  ".swift",
  ".rb",
  ".lua",
])

/**
 * 路径中出现这些**目录段**即跳过：构建产物、依赖缓存、外部依赖源码（由各自的下载/固化流程管理）。
 * 采用"段名"判定而非具体路径，是因为这些目录会出现在各处的层级里（<pkg>/build、<pkg>/dist…）。
 */
const IGNORED_SEGMENTS = new Set([
  "node_modules",
  "dist",
  "build",
  "out",
  "target",
  ".turbo",
  ".git",
  ".cache",
  "cache",
  "coverage",
  ".venv",
  "venv",
  "__pycache__",
  "vendor",
  ".next",
  ".pnpm-store",
  "tmp",
  "temp",
])

/**
 * 文件名含 `.generated.` 的跳过：构建期生成物的仓库内约定
 * （`.gitignore` 里逐条列出，如 `subagents.bundle.generated.ts` / `web.bundle.generated.ts`），
 * 由 `scripts/build-*.ts` 在构建时重建，本来就不该入库。
 */
const GENERATED_MARKER = ".generated."

const run = spawnSync("git", ["ls-files", "--others", "--ignored", "--exclude-standard", "-z"], {
  cwd: root,
  encoding: "buffer",
  maxBuffer: 64 * 1024 * 1024,
})
if (run.status !== 0) {
  console.error("[check:repo] 无法运行 git（不是仓库？）：", run.stderr?.toString().slice(0, 400))
  process.exit(1)
}

const ignored = run.stdout.toString("utf8").split("\0").filter((entry) => entry.length > 0)
const offenders: string[] = []
for (const relative of ignored) {
  const segments = relative.split("/")
  const fileName = segments.at(-1) ?? ""
  if (fileName.includes(GENERATED_MARKER)) continue
  const dot = fileName.lastIndexOf(".")
  if (dot < 0 || !SOURCE_EXTENSIONS.has(fileName.slice(dot).toLowerCase())) continue
  // 目录段判定：末段（文件名）不参与，只看它所在的目录链
  if (segments.slice(0, -1).some((segment) => IGNORED_SEGMENTS.has(segment))) continue
  offenders.push(relative)
}

if (offenders.length > 0) {
  console.error(`[check:repo] 发现 ${offenders.length} 个**被 gitignore 忽略的源文件**——仓库不自包含：\n`)
  for (const file of offenders.slice(0, 40)) console.error(`  ${file}`)
  if (offenders.length > 40) console.error(`  …另有 ${offenders.length - 40} 个`)
  console.error(
    "\n这些文件存在于工作树但从未入库：干净克隆会缺少它们，而本地一切正常（最难发现的一类问题）。" +
      "\n排查：多半是 .gitignore 里某条**过宽的模式**（例如为转储写的裸 `core` 会连 `src/core/` 一起忽略）——" +
      "\n用 `git check-ignore -v <路径>` 看是哪条规则命中的，再把该规则收紧到具体形态。",
  )
  process.exit(1)
}

console.log(`[check:repo] 通过：仓库自包含（被忽略的源码文件 0 个；扫描忽略项 ${ignored.length} 条）`)
