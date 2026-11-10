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
 * 交给判定核 check-repo-core.ts（源码扩展名 → 显式豁免 → 产物目录段；判定表与豁免理由都在那里）。
 *
 * 本文件只做 I/O：跑 git、打印、退出码。
 *
 * 用法：`bun run check:repo`（已接进 `bun run build` 前置）。
 * 退出码：0 = 自包含；1 = 发现被忽略的源码（逐条打印，附排查提示）。
 */
import { spawnSync } from "node:child_process"
import { join } from "node:path"
import { EXEMPT_PREFIXES, findOffenders } from "./check-repo-core"

const root = join(import.meta.dirname, "..")

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
const offenders = findOffenders(ignored)

if (offenders.length > 0) {
  console.error(`[check:repo] 发现 ${offenders.length} 个**被 gitignore 忽略的源文件**——仓库不自包含：\n`)
  for (const file of offenders.slice(0, 40)) console.error(`  ${file}`)
  if (offenders.length > 40) console.error(`  …另有 ${offenders.length - 40} 个`)
  console.error(
    "\n这些文件存在于工作树但从未入库：干净克隆会缺少它们，而本地一切正常（最难发现的一类问题）。" +
      "\n排查：多半是 .gitignore 里某条**过宽的模式**（例如为转储写的裸 `core` 会连 `src/core/` 一起忽略）——" +
      "\n用 `git check-ignore -v <路径>` 看是哪条规则命中的，再把该规则收紧到具体形态。",
  )
  console.error(
    "\n若该路径确实**由生成器产出或由资产清单托管**（源码可重建，不该入库），" +
      "\n在 scripts/check-repo-core.ts 的 EXEMPT_PREFIXES 加一条**精确路径前缀 + 理由**" +
      "（写清谁生成/谁托管、重建入口在哪）。不要放宽 IGNORED_SEGMENTS——那是全局段名，会盖住真实漏项。",
  )
  process.exit(1)
}

console.log(
  `[check:repo] 通过：仓库自包含（被忽略的源码文件 0 个；扫描忽略项 ${ignored.length} 条；` +
    `显式豁免 ${EXEMPT_PREFIXES.length} 条）`,
)
