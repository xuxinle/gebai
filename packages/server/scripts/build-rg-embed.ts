/**
 * 构建期生成内置 ripgrep（rg）内嵌产物 `../src/core/rg.embedded.generated.json`（gzip base64）。
 *
 * 用途：`bun --compile` 单二进制形态在用户机器上**既没有 node_modules 也不保证装了 rg**，故把 rg 随产物
 * 内嵌，运行时物化到 `{GEBAI_HOME}/vendor/ripgrep/<平台>/`（与 d2js / playwright driver / CV 模型同一套
 * 「内嵌 + 物化」闭环，见 core/support/ripgrep.ts）。源码/dev 形态不需要本产物——直接走 node_modules 或系统 rg。
 *
 * rg 来源（**只有两个真实来源**，首个可用者胜出）：
 *   1. node_modules 的 `@vscode/ripgrep`（`optionalDependencies`，经 npm registry 分发平台子包，
 *      版本随 lockfile 可控，不要求构建机装过 rg）
 *   2. 系统 PATH 上的 rg（构建机已装则直接用）
 * 另可 `GEBAI_RG_PATH` 显式指定（运维/内网自备/调试）——与运行时解析链同一优先级语义。
 *
 * **不落盘任何资源文件**（不留 models/ 副本、不从网络下载）：rg 二进制只随 npm 包或系统存在，
 * 内嵌产物本身是 gitignore 的构建生成物。
 * 取不到时生成空清单——构建不失败；运行时 grep 回退内置遍历引擎（功能不降级、只降速）。
 * 该文件为生成产物，已 gitignore，勿手改。
 */
import { gzipSync } from "node:zlib"
import { statSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { npmRipgrepPath } from "../src/core/support/ripgrep"

const root = join(import.meta.dirname, "..") // scripts/ 上一级 = packages/server
const outFile = join(root, "src", "core", "rg.embedded.generated.json")
const platformDir = `${process.platform}-${process.arch}`

function isFile(p: string): boolean {
  try {
    return statSync(p).isFile()
  } catch {
    return false
  }
}

/** rg 版本串（首行，如 `ripgrep 15.0.0`）；读取失败返回空串（仅供日志与产物标识）。 */
function rgVersion(bin: string): string {
  try {
    const p = Bun.spawnSync([bin, "--version"], { stdout: "pipe", stderr: "ignore" })
    if (p.exitCode !== 0) return ""
    return new TextDecoder().decode(p.stdout ?? new Uint8Array()).split("\n")[0]?.trim() ?? ""
  } catch {
    return ""
  }
}

/** 取得 rg 本体路径：npm 包优先（版本可控、无需构建机预装），其次系统 PATH。 */
async function resolveRgBinary(): Promise<{ path: string; source: string } | null> {
  const explicit = process.env.GEBAI_RG_PATH?.trim()
  if (explicit) {
    if (!isFile(explicit)) {
      console.warn(`[build-rg-embed] GEBAI_RG_PATH 指向的文件不存在：${explicit}`)
      return null
    }
    return { path: explicit, source: "GEBAI_RG_PATH" }
  }
  const npm = await npmRipgrepPath()
  if (npm) return { path: npm, source: "node_modules @vscode/ripgrep" }
  const which = Bun.which("rg")
  if (which) return { path: which, source: "系统 PATH" }
  return null
}

async function main(): Promise<void> {
  const found = await resolveRgBinary()
  if (!found) {
    writeFileSync(outFile, JSON.stringify({ version: "", platform: "", data: "" }))
    console.log(
      `[build-rg-embed] 空清单（未取得 ${platformDir} 的 rg）-> ${outFile}\n` +
        `  运行时 grep 将回退内置遍历引擎（功能不降级、只降速）。\n` +
        `  补齐方式（二选一）：① 在仓库根 bun install（拉取 optionalDependencies 的 @vscode/ripgrep）；\n` +
        `  ② 构建机装 rg 后重跑本脚本；亦可置 GEBAI_RG_PATH 临时指定。`,
    )
    return
  }
  const version = rgVersion(found.path) || "ripgrep（未知版本）"
  const data = gzipSync(Buffer.from(new Uint8Array(await Bun.file(found.path).arrayBuffer()))).toString("base64")
  writeFileSync(outFile, JSON.stringify({ version, platform: platformDir, data }))
  console.log(
    `[build-rg-embed] embedded ${version} (${platformDir}, ${Math.round(data.length / 1024)} KB base64) -> ${outFile}\n` +
      `  来源: ${found.source} → ${found.path}`,
  )
}

await main()
