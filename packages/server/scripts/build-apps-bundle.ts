/**
 * 构建期生成托管应用的内嵌表（`src/core/apps.bundle.generated.ts`）。
 *
 * 产物两段：
 * - `files`：仓库 `apps/<id>/` 目录应用的**全量静态文件**（base64），二进制单文件形态自带出厂应用；
 * - `inline`：仓库 `apps/*.ts` 源码内联应用的**定义数据**（`files` 里的字符串内容与清单字段）。
 *   函数型后端（`backend: (ctx) => …`）**不进表**——它不是数据、无法序列化，只能随源码树编译。
 *   二进制形态要后端就用目录应用的 `server.js`，其源码由 `files` 段带上、运行时落地到缓存目录再 import
 *   （见 core/apps/sources.ts 的 materialize 与 backend.ts 的加载口径）——或者显式把该应用改成目录应用。
 *   内联应用的作者契约在 `@gebai/sdk`（`hosted-app.ts` 的 `InlineApp`），本脚本只取它的数据面。
 *
 * 幂等：由 server/desktop 构建前置调用。
 *
 * **产物必须入库（空态）**：`boot/compose.ts` 对它做**静态 import**——生成产物闸门
 * （`core/generated-artifacts.test.ts`）要求这种模块必须随仓库分发，否则新克隆环境启动即崩
 * （同 `tools-excluded.generated.ts` 策略）；策略与 `web.bundle.generated.ts`（动态 require，可 gitignore）相反。
 */
import { existsSync, readdirSync, readFileSync, statSync, writeFileSync } from "node:fs"
import { join, relative } from "node:path"
import { writeFileIfChanged } from "./write-if-changed"

const root = join(import.meta.dirname, "..", "..", "..") // packages/server/scripts → 仓库根
const appsDir = join(root, "apps")
const outFile = join(import.meta.dirname, "..", "src", "core", "apps.bundle.generated.ts")

/** 目录应用内跳过的路径段（依赖与版本库不进产物）。 */
const SKIP_SEGMENTS = new Set(["node_modules", ".git", ".turbo", "dist-cache"])

/** 单文件上限（GB 级素材不该进二进制产物；超限告警跳过）。 */
const MAX_FILE_BYTES = 8 * 1024 * 1024

/** 内联模块文件匹配（与 core/apps/scan.ts 的 isInlineAppModule 同口径）。 */
const INLINE_MODULE_RE = /\.(ts|mts|js|mjs)$/
function isInlineModule(name: string): boolean {
  if (!INLINE_MODULE_RE.test(name)) return false
  if (name.endsWith(".d.ts") || name.endsWith(".test.ts") || name.endsWith(".test.js")) return false
  return true
}

interface InlineRecord {
  id: string
  title?: string
  description?: string
  version?: string
  entry?: string
  spa?: boolean
  listed?: boolean
  env?: Record<string, string>
  files: Record<string, string>
  /** 该应用带函数型后端（二进制形态下不可用：如实标记，运行时据此说明原因）。 */
  hasFunctionBackend?: boolean
}

/** 空态产物文本（**入库版本**：新克隆/未构建环境的静态 import 目标）。 */
const EMPTY_TEXT = `// 由 scripts/build-apps-bundle.ts 生成（**入库版本为空态**；构建后为真内容，构建链收尾会还原）
// 空态入库的理由：boot/compose.ts 对本模块做静态 import，缺文件会让新克隆环境启动即崩
// （策略同 core/session/env-embedded.generated.ts）——它不是“产物目录里的缓存”，而是“带默认值的源码”。
import type { InlineApp } from "./apps/inline"

export const BUNDLED_APP_FILES: Record<string, Record<string, string>> = {}
export const BUNDLED_INLINE_APPS: InlineApp[] = []
export const BUNDLED_APPS: { files: Record<string, Record<string, string>>; inline: InlineApp[]; errors: string[] } = { files: {}, inline: [], errors: [] }
`

const emptyRecord = (reason: string): void => {
  writeFileIfChanged(outFile, EMPTY_TEXT)
  console.log(`[build-apps] ${reason}：写空表 -> ${outFile}`)
}

// `--restore`：还原空态（构建链收尾调用；与 build-env-embed.ts 同款约定）
if (process.argv.includes("--restore")) {
  emptyRecord("--restore")
  process.exit(0)
}

if (!existsSync(appsDir)) {
  emptyRecord("apps/ 目录不存在")
  process.exit(0)
}

// ── ① 目录应用：全量静态文件 ──
const filesTable: Record<string, Record<string, string>> = {}
const warnings: string[] = []
for (const name of readdirSync(appsDir)) {
  if (name.startsWith(".") || name === "node_modules") continue
  const dir = join(appsDir, name)
  let st
  try {
    st = statSync(dir)
  } catch {
    continue
  }
  if (!st.isDirectory()) continue // 单文件走下面的内联段
  const entry: Record<string, string> = {}
  const walk = (d: string): void => {
    for (const child of readdirSync(d)) {
      if (SKIP_SEGMENTS.has(child)) continue
      const p = join(d, child)
      const s = statSync(p)
      if (s.isDirectory()) {
        walk(p)
        continue
      }
      if (s.size > MAX_FILE_BYTES) {
        warnings.push(`${relative(appsDir, p)} 超过单文件上限（${s.size} 字节），已跳过`)
        continue
      }
      const rel = relative(dir, p).replace(/\\/g, "/")
      // 后端模块源码也要带上：二进制形态下由运行时写到临时文件再 import（见 core/apps/backend.ts）
      entry[rel] = readFileSync(p).toString("base64")
    }
  }
  walk(dir)
  if (Object.keys(entry).length) filesTable[name] = entry
}

// ── ② 源码内联应用：只取数据面（函数型后端不可序列化，标记后如实告知） ──
const inline: InlineRecord[] = []
for (const name of readdirSync(appsDir)) {
  if (!isInlineModule(name)) continue
  const abs = join(appsDir, name)
  try {
    if (!statSync(abs).isFile()) continue
  } catch {
    continue
  }
  let mod: { apps?: unknown; default?: unknown }
  try {
    mod = (await import(abs)) as { apps?: unknown; default?: unknown }
  } catch (err) {
    warnings.push(`${name} 加载失败：${String((err as Error)?.message || err)}`)
    continue
  }
  const list: unknown[] = []
  if (Array.isArray(mod.apps)) list.push(...mod.apps)
  if (Array.isArray(mod.default)) list.push(...mod.default)
  else if (mod.default && typeof mod.default === "object") list.push(mod.default)
  for (const raw of list) {
    const a = raw as Record<string, unknown> & { id?: string; files?: Record<string, string | Uint8Array>; backend?: unknown }
    if (!a || typeof a.id !== "string") continue
    const files: Record<string, string> = {}
    for (const [rel, content] of Object.entries(a.files ?? {})) {
      if (typeof content === "string") files[rel] = Buffer.from(content, "utf8").toString("base64")
      else if (content instanceof Uint8Array) files[rel] = Buffer.from(content).toString("base64")
      else warnings.push(`${a.id} 文件 ${rel} 不是字符串/字节：跳过`)
    }
    inline.push({
      id: a.id,
      ...(typeof a.title === "string" ? { title: a.title } : {}),
      ...(typeof a.description === "string" ? { description: a.description } : {}),
      ...(typeof a.version === "string" ? { version: a.version } : {}),
      ...(typeof a.entry === "string" ? { entry: a.entry } : {}),
      ...(typeof a.spa === "boolean" ? { spa: a.spa } : {}),
      ...(typeof a.listed === "boolean" ? { listed: a.listed } : {}),
      ...(a.env && typeof a.env === "object" ? { env: a.env as Record<string, string> } : {}),
      files,
      ...(typeof a.backend === "function" ? { hasFunctionBackend: true } : {}),
    })
  }
}

for (const w of warnings) console.warn(`[build-apps] ${w}`)

// 内联表里 files 的内容是 base64：运行时需解码成字符串。为免运行时再造一套解码，
// 这里直接生成「已解码的字符串文件表」——内联应用内容本来就小（几十 KB 级）。
const inlineLiteral = inline
  .map((a) => {
    const files: Record<string, string> = {}
    for (const [rel, b64] of Object.entries(a.files)) files[rel] = Buffer.from(b64, "base64").toString("utf8")
    const data = { ...a, files }
    return `  ${JSON.stringify(data)},`
  })
  .join("\n")

const out = `// 由 scripts/build-apps-bundle.ts 生成（**入库版本为空态**；本文件为构建态，收尾脚本会还原空态）
// 见该脚本文件头的策略说明。
import type { InlineApp } from "./apps/inline"

/** 目录应用：appId → 相对路径 → base64（含 \`server.js\` 源码）。 */
export const BUNDLED_APP_FILES: Record<string, Record<string, string>> = {
${Object.entries(filesTable)
  .map(([id, files]) => `  ${JSON.stringify(id)}: {\n${Object.entries(files).map(([rel, b64]) => `    ${JSON.stringify(rel)}: ${JSON.stringify(b64)},`).join("\n")}\n  },`)
  .join("\n")}
}

/**
 * 源码内联应用的数据面（\`export const apps = [...]\` 去掉函数后的形态）：
 * 函数型后端不进表（不可序列化），\`hasFunctionBackend\` 标记该应用在二进制形态下缺后端。
 */
export const BUNDLED_INLINE_APPS = [
${inlineLiteral}
] as unknown as InlineApp[]

/** 统一入口：compose 只读这一个常量（dev 形态下它同样导入，但内容为空——源码由 loadInlineApps 现扫）。 */
export const BUNDLED_APPS: { files: Record<string, Record<string, string>>; inline: InlineApp[]; errors: string[] } = {
  files: BUNDLED_APP_FILES,
  inline: BUNDLED_INLINE_APPS,
  errors: ${JSON.stringify(warnings)},
}
`

writeFileIfChanged(outFile, out)
console.log(
  `[build-apps] 目录应用 ${Object.keys(filesTable).length} 个、内联应用 ${inline.length} 个` +
    `${warnings.length ? `、告警 ${warnings.length} 条` : ""} -> ${outFile}`,
)
