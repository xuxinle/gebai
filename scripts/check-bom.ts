#!/usr/bin/env bun
/**
 * 仓库 BOM 自检：**入库的文件不得带 UTF-8 BOM**。
 *
 * 为什么需要这条检查：BOM 不报错、只污染——首行带一个零宽字符，于是 JSON 解析器偶发
 * 「Unexpected token」、shebang 不再被 shell 认作第一行、`git diff` 首行看着没改却显示改动。
 * 而它最容易从 PowerShell 5.1（`-Encoding utf8` 固有一行为）与 Windows 带的编辑器进来。
 * 工具侧的写入口径已在 `core/tools/fs.ts` 统一为「不带 BOM」，本闸门守仓库自身。
 *
 * 判定核在 check-bom-core.ts（纯逻辑 + 单测）：本文件只做 I/O —— 跑 git 拿入库文件清单、
 * 读每个文件的**前 3 字节**（不为查文件头把大文件读进来）、打印、退出码。
 *
 * 用法：`bun run check:bom`（已接进 `bun run build` 前置）。
 * 退出码：0 = 无 BOM；1 = 发现带 BOM 的入库文件（逐条打印，附修法）。
 */
import { spawnSync } from "node:child_process"
import { closeSync, openSync, readSync } from "node:fs"
import { join } from "node:path"
import { findBomOffenders, needsBomCheck } from "./check-bom-core"

const root = join(import.meta.dirname, "..")

const run = spawnSync("git", ["ls-files", "-z"], {
  cwd: root,
  encoding: "buffer",
  maxBuffer: 64 * 1024 * 1024,
})
if (run.status !== 0) {
  console.error("[check:bom] 无法运行 git（不是仓库？）：", run.stderr?.toString().slice(0, 400))
  process.exit(1)
}

const files = run.stdout
  .toString("utf8")
  .split("\0")
  .filter((entry) => entry.length > 0)

/** 只读前 3 字节（文件不存在/不可读返回 null，交给判定核跳过）。 */
function readHead(rel: string): Uint8Array | null {
  let fd: number | null = null
  try {
    fd = openSync(join(root, rel), "r")
    const buf = new Uint8Array(3)
    const n = readSync(fd, buf, 0, 3, 0)
    return n < 3 ? null : buf
  } catch {
    return null
  } finally {
    if (fd !== null) closeSync(fd)
  }
}

const offenders = findBomOffenders(files, readHead)
const checked = files.filter(needsBomCheck).length

if (offenders.length > 0) {
  console.error(`[check:bom] 发现 ${offenders.length} 个带 UTF-8 BOM 的入库文件——首字符被零宽字符污染：\n`)
  for (const file of offenders.slice(0, 40)) console.error(`  ${file}`)
  if (offenders.length > 40) console.error(`  …另有 ${offenders.length - 40} 个`)
  console.error(
      `\n修法（逐个去掉前三个字节）：` +
      `\n  · 在歌白里对每个文件跑一次 \`edit\`（任何一处改动都会按无 BOM 写回），或` +
      `\n  · \`bun -e "const f=require('fs');for(const p of process.argv.slice(1)){const b=f.readFileSync(p);if(b[0]===0xef&&b[1]===0xbb&&b[2]===0xbf)f.writeFileSync(p,b.subarray(3))}" <文件…>\`` +
      `\nPS1/BAT/CMD 不该出现在这个列表里（它们必须带 BOM，已排除在检出范围外）；若出现了，改 BOM_REQUIRED_EXTENSIONS。` +
      `\n写入工具别再选「UTF-8 with BOM」：PowerShell 5.1 的 \`Set-Content/Out-File -Encoding utf8\` 必带 BOM，用 pwsh 7 或不写编码参数。`,
  )
  process.exit(1)
}

console.log(`[check:bom] 通过：入库文件无 UTF-8 BOM（已检文本类 ${checked} / 共 ${files.length} 个入库文件）`)
