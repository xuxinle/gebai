/**
 * 托管应用的**源码内联**形态：应用不落成目录，而是直接写成一个 TS/JS 模块。
 *
 * ```ts
 * // apps/hello.ts —— 丢文件即注册（零注册表），构建期随产物分发
 * import type { InlineApp } from "@gebai/sdk"
 *
 * export const apps: InlineApp[] = [{
 *   id: "hello",
 *   title: "你好",
 *   files: { "index.html": "<!doctype html><h1>hi</h1>" },
 *   backend: (ctx) => ({ ok: true, path: ctx.path }),   // 函数本体：同进程调用
 * }]
 * ```
 *
 * ## 为什么要有这一形态（与「目录 + server.js」并存）
 *
 * 两种写法各自回答一个不同的问题：
 * - **目录形态**（`apps/{id}/` + 可选 `server.js`）回答「我有一份现成的前端产物」——改文件刷新即生效；
 * - **源码内联形态**（本文件）回答「我就想写几行字」——一段 HTML 字符串、一个返回对象的函数，
 *   不用建目录、不用建文件、不用管静态资源路径。它也顺带解决二进制形态的一个真实缺口：
 *   内嵌的文件型后端要靠「落地到磁盘再 import」才能跑（见 `sources.ts: materialize`），
 *   而内联后端是函数本身，同进程直接调用。
 *
 * ## 契约与校验口径
 *
 * 类型来自 `@gebai/sdk`（`hosted-app.ts`）——写应用的人只依赖那一份；id 与所有文件路径走与
 * 目录形态完全相同的规则（`APP_ID_RE` / `normalizeAppRelPath`），一个应用不论从哪一层来，
 * 进路由分发时形态必须一致，否则「内联的能跑、磁盘的不行」这类双标准缺陷会立刻出现。
 */
import type { AppBackendHandler, InlineApp } from "@gebai/sdk"
import { APP_DEFAULT_ENTRY, APP_ID_RE, normalizeAppRelPath, parseAppManifest, type AppManifest, type AppManifestParse } from "./manifest"

export type { InlineApp, InlineAppFile } from "@gebai/sdk"

/** 归一后的内联应用（文件内容已编码为字节，路径与 id 已校验）。 */
export interface NormalizedInlineApp {
  manifest: AppManifest
  files: Map<string, Uint8Array>
  backend?: AppBackendHandler
  warnings: string[]
}

/** 把内联应用定义归一（非法定义返回 `{ error }`，由上层跳过并告警——一个写坏的应用不该拖垮其余）。 */
export function normalizeInlineApp(raw: InlineApp): NormalizedInlineApp | { error: string } {
  if (!raw || typeof raw !== "object") return { error: "不是对象" }
  const id = String(raw.id ?? "")
  if (!APP_ID_RE.test(id)) return { error: `id「${id}」非法（需匹配 ${APP_ID_RE}）` }
  const files = new Map<string, Uint8Array>()
  const warnings: string[] = []
  const declared = raw.files && typeof raw.files === "object" ? raw.files : {}
  for (const [rel, content] of Object.entries(declared)) {
    const norm = normalizeAppRelPath(rel)
    if (!norm) {
      warnings.push(`文件路径「${rel}」非法：跳过`)
      continue
    }
    if (typeof content === "string") files.set(norm, new TextEncoder().encode(content))
    else if (content instanceof Uint8Array) files.set(norm, content)
    else warnings.push(`文件「${rel}」内容既不是字符串也不是字节数组：跳过`)
  }
  // 清单走与目录形态同一套解析器（title/entry/spa/listed/env 的默认值与非法值处理完全一致）
  const parsed: AppManifestParse = parseAppManifest(
    {
      ...(raw.title !== undefined ? { title: raw.title } : {}),
      ...(raw.description !== undefined ? { description: raw.description } : {}),
      ...(raw.version !== undefined ? { version: raw.version } : {}),
      ...(raw.entry !== undefined ? { entry: raw.entry } : {}),
      ...(raw.spa !== undefined ? { spa: raw.spa } : {}),
      ...(raw.listed !== undefined ? { listed: raw.listed } : {}),
      ...(raw.env !== undefined ? { env: raw.env } : {}),
      // 内联后端是函数：清单里的 backend 字段留空（函数不走文件路径），由 backend 属性承载
    },
    id,
  )
  warnings.push(...parsed.warnings)
  const manifest = parsed.manifest
  if (!files.size) warnings.push("files 为空：应用没有可托管的内容")
  if (!files.has(manifest.entry) && files.size) warnings.push(`入口文件不存在：${manifest.entry}（缺省入口为 ${APP_DEFAULT_ENTRY}）`)
  const backend = typeof raw.backend === "function" ? raw.backend : undefined
  if (raw.backend !== undefined && !backend) warnings.push("backend 不是函数：忽略")
  return { manifest, files, warnings, ...(backend ? { backend } : {}) }
}

/**
 * 收集模块导出里的内联应用（容忍 `export const apps` / `export default` / 单个对象三种写法——
 * 与子 Agent 的定义形态同风格，不强求一种导出名）。
 */
export function collectInlineApps(mod: unknown): InlineApp[] {
  const m = (mod ?? {}) as Record<string, unknown>
  const list: unknown[] = []
  if (Array.isArray(m.apps)) list.push(...m.apps)
  if (m.default) {
    if (Array.isArray(m.default)) list.push(...m.default)
    else list.push(m.default)
  }
  return list.filter((x): x is InlineApp => !!x && typeof x === "object" && typeof (x as InlineApp).id === "string")
}
