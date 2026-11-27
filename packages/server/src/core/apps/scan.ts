/**
 * 托管应用的**源码扫描城**（零注册表）：仓库根 `apps/` 下的两类来源——
 *
 * ```
 * apps/
 *   hello.ts            # 源码内联应用（export const apps = [{ id, files: { "index.html": "…" }, backend }]）
 *   demo/               # 目录应用（app.json + index.html + 可选 server.js）
 *     app.json
 *     index.html
 *     server.js
 * ```
 *
 * ## 两类来源的分工（同一目录里共存，互不干扰）
 *
 * - `apps/*.ts|.js`（文件）= **代码即应用**：文件内容是字符串，后端是函数——零散小功能最省事；
 * - `apps/<id>/`（目录）= **产物即应用**：可以是任意构建工具的输出，改文件刷新即生效。
 *
 * 目录应用**不需要**构建期内嵌进产物（构建期只扫 `.ts/.js` 单文件与目录应用里的静态文件，
 * 见 `scripts/build-apps-bundle.ts`）——源码形态下目录应用本来就从磁盘读，内嵌一份只是为二进制形态
 * 带上出厂示例。
 *
 * ## 为什么源码内联层是「进程启动时扫一次」
 *
 * 源码内联应用的后端是**函数**，热替换一个函数意味着重新执行一个模块并让旧闭包失效——这在
 * Bun 里只能靠改 import URL（与 `server.js` 的 mtime 版本号同法），但源码形态下 `apps/*.ts` 是
 * 部署方自己的代码，重启一次的代价远小于「半个旧闭包还在跑」的排查代价。所以：
 * **源码内联层重启生效；磁盘目录层改文件即生效**。这条差异写进 DESIGN，并由 `layers` 字段在 REST 里透出。
 */
import { readdirSync, statSync } from "node:fs"
import { join } from "node:path"
import { pathToFileURL } from "node:url"
import { collectInlineApps, type InlineApp } from "./inline"
import { log } from "@gebai/sdk/node"

/** 源码内联应用的目录名（仓库根）：`{仓库根}/apps`。 */
export const SOURCE_APPS_DIRNAME = "apps"

/** 内联模块文件匹配：`apps/*.ts`、`apps/*.mts`、`apps/*.js`、`apps/*.mjs`（不扫子目录）。 */
const INLINE_MODULE_RE = /\.(ts|mts|js|mjs)$/

/** 是否是内联模块文件（排除 .d.ts 与测试）。 */
export function isInlineAppModule(name: string): boolean {
  if (!INLINE_MODULE_RE.test(name)) return false
  if (name.endsWith(".d.ts") || name.endsWith(".test.ts") || name.endsWith(".test.js")) return false
  return true
}

export interface LoadInlineAppsResult {
  apps: InlineApp[]
  errors: string[]
}

/**
 * 扫描并 import 源码内联应用（dev 形态；构建产物形态由 `apps.bundle.generated.ts` 静态 import 得到）。
 *
 * 失败隔离：单个模块 import 抛错只记 errors，不影响其余模块（与子 Agent 扫描同一纪律）。
 */
export async function loadInlineApps(dir: string): Promise<LoadInlineAppsResult> {
  const apps: InlineApp[] = []
  const errors: string[] = []
  let names: string[]
  try {
    names = readdirSync(dir)
  } catch {
    return { apps, errors } // 目录不存在：没有源码内联应用，正常
  }
  for (const name of names.sort()) {
    if (!isInlineAppModule(name)) continue
    const abs = join(dir, name)
    try {
      if (!statSync(abs).isFile()) continue
    } catch {
      continue
    }
    try {
      const mod = await import(pathToFileURL(abs).href)
      const found = collectInlineApps(mod)
      if (!found.length) continue
      apps.push(...found)
    } catch (err) {
      const msg = `${name}: ${String((err as Error)?.message || err)}`
      errors.push(msg)
      log.warn(`[apps] 内联应用模块加载失败（已跳过）：${msg}`)
    }
  }
  return { apps, errors }
}
