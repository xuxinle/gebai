/**
 * 歌白托管应用（Hosted Apps）——应用清单解析与 id/路径规则。
 *
 * 一个托管应用 = 一个目录：`app.json` 清单 + 前端静态产物（HTML/JS/CSS…）+ 可选的
 * 后端模块（`server.js` 一类）。清单位于应用目录根部，**全部字段可省略**（省略即目录名做 id、
 * `index.html` 做入口），使「丢一个 index.html 进目录」就是最小可用应用。
 *
 * 这里是纯函数域（不碰文件系统）：清单解析、字段校验、相对路径归一——上层（sources.ts）
 * 负责去哪读、routes/apps.ts 负责怎么发。纯函数化的理由与 `core/fs/mime.ts` 一致：
 * 校验规则是这个能力的安全边界（id 与路径都可能来自 URL 与磁盘），必须能被单测逐条钉住。
 *
 * **作者契约在 `@gebai/sdk`（`hosted-app.ts`）**：清单与后端 ctx 的类型定义只有那一份，
 * 本模块从它取类型——写应用的人看到的签名就是宿主实际调用的签名（见该文件头）。
 */
import { HOSTED_APP_DEFAULT_ENTRY, HOSTED_APP_ID_RE, type AppManifest } from "@gebai/sdk"

export type { AppManifest, AppBackendContext, AppBackendHandler, AppBackendResult, InlineApp, InlineAppFile } from "@gebai/sdk"

/** 应用 id 白名单（SDK 单一来源；此处再导出便于本域内引用与注释就近阅读）。 */
export const APP_ID_RE = HOSTED_APP_ID_RE

/** 清单文件名（应用目录根部）。 */
export const APP_MANIFEST_FILE = "app.json"

/** 默认入口（未在清单里声明 entry 时）。 */
export const APP_DEFAULT_ENTRY = HOSTED_APP_DEFAULT_ENTRY

/** 允许的后端模块扩展名（动态 import 的形态）。 */
export const APP_BACKEND_EXTS = new Set(["js", "mjs", "cjs", "ts", "mts"])

/** 单文件读取上限默认值（16MB）：应用产物通常几百 KB，超限多半是误把素材库当应用目录。 */
export const APP_MAX_FILE_BYTES = 16 * 1024 * 1024

/** 清单解析结果：warnings 用于「清单写错了但应用还能跑」的如实告知（索引页与 REST 都带出）。 */
export interface AppManifestParse {
  manifest: AppManifest
  warnings: string[]
}

/**
 * 相对路径归一（应用根内的 POSIX 相对路径）：拒绝绝对路径、`..`、盘符、NUL；
 * 空段与 `.` 折叠掉。返回 null = 非法（调用方按「不可访问」处理）。
 *
 * 注意这里**不做**「必须存在于磁盘」的判定——那由上层查文件时回答（本函数只管形态合法性）。
 */
export function normalizeAppRelPath(input: string): string | null {
  const raw = String(input ?? "").replace(/\\/g, "/")
  if (!raw || raw.includes("\0")) return null
  if (raw.startsWith("/")) return null
  // Windows 盘符与 UNC（`C:`、`//server/share`）在 POSIX 归一后仍可能以 `C:` 开头
  if (/^[A-Za-z]:/.test(raw)) return null
  const parts: string[] = []
  for (const seg of raw.split("/")) {
    if (!seg || seg === ".") continue
    if (seg === "..") return null
    parts.push(seg)
  }
  return parts.join("/")
}

/** URL 段 → 应用内相对路径（已解码）；根（""/"/"）返回 ""。非法（穿越/编码坏）返回 null。 */
export function appPathFromUrl(urlPath: string): string | null {
  let decoded: string
  try {
    decoded = decodeURIComponent(urlPath)
  } catch {
    return null
  }
  // URL 路径天然带前导斜杠（那正是“应用根”的意思），归一前先剥（剥后再归一，防 `//../x` 这类形态）
  return normalizeAppRelPath(decoded.replace(/^\/+/, ""))
}

/** 单个清单字段的字符串取值（非字符串/空白 → 缺省值）。 */
function strField(raw: Record<string, unknown>, key: string, fallback = ""): string {
  const v = raw[key]
  return typeof v === "string" && v.trim() ? v.trim() : fallback
}

/**
 * 解析清单（`raw` 为 app.json 的 JSON 对象；非法形态按「空清单 + 警告」处理，绝不抛错——
 * 一个写坏的 app.json 不该让整个应用列表消失）。
 *
 * `fallbackId` 来自目录名：清单里的 id 与目录名不一致时**以目录名为准**（URL 与磁盘是一对
 * 天然的真理来源；允许清单改写 id 会让「按 id 找目录」失去意义），并记一条警告。
 */
export function parseAppManifest(raw: unknown, fallbackId: string): AppManifestParse {
  const warnings: string[] = []
  const manifest: AppManifest = {
    id: fallbackId,
    title: fallbackId,
    description: "",
    version: "",
    entry: APP_DEFAULT_ENTRY,
    spa: true,
    backend: "",
    listed: true,
    env: {},
  }
  if (raw === null || typeof raw !== "object" || Array.isArray(raw)) {
    if (raw !== undefined) warnings.push("app.json 不是 JSON 对象：按默认清单处理")
    return { manifest, warnings }
  }
  const obj = raw as Record<string, unknown>

  const declaredId = strField(obj, "id")
  if (declaredId && declaredId !== fallbackId) {
    warnings.push(`清单 id「${declaredId}」与目录名「${fallbackId}」不一致：以目录名为准`)
  }
  manifest.title = strField(obj, "title", fallbackId)
  manifest.description = strField(obj, "description")
  manifest.version = strField(obj, "version")

  const entryRaw = strField(obj, "entry")
  if (entryRaw) {
    const entry = normalizeAppRelPath(entryRaw)
    if (!entry) warnings.push(`entry「${entryRaw}」非法（越界或绝对路径）：回落到 ${APP_DEFAULT_ENTRY}`)
    else manifest.entry = entry
  }

  if (typeof obj.spa === "boolean") manifest.spa = obj.spa
  if (typeof obj.listed === "boolean") manifest.listed = obj.listed

  const backendRaw = strField(obj, "backend")
  if (backendRaw) {
    const backend = normalizeAppRelPath(backendRaw)
    const ext = (backend ?? "").split(".").pop()?.toLowerCase() ?? ""
    if (!backend) warnings.push(`backend「${backendRaw}」非法（越界或绝对路径）：按无后端处理`)
    else if (!APP_BACKEND_EXTS.has(ext)) warnings.push(`backend「${backendRaw}」扩展名不在白名单（${[...APP_BACKEND_EXTS].join("/")}）：按无后端处理`)
    else manifest.backend = backend
  }

  const envRaw = obj.env
  if (envRaw && typeof envRaw === "object" && !Array.isArray(envRaw)) {
    const env: Record<string, string> = {}
    for (const [k, v] of Object.entries(envRaw as Record<string, unknown>)) {
      if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(k)) {
        warnings.push(`env 键「${k}」非法：忽略`)
        continue
      }
      if (typeof v !== "string" && typeof v !== "number" && typeof v !== "boolean") {
        warnings.push(`env 值「${k}」不是字符串/数字/布尔：忽略`)
        continue
      }
      env[k] = String(v)
    }
    manifest.env = env
  } else if (envRaw !== undefined) {
    warnings.push("env 不是对象：忽略")
  }

  return { manifest, warnings }
}

/** 请求路径是否像「页面」（用于 SPA 回退判据）：无扩展名的路径才回退到 entry。 */
export function looksLikePagePath(rel: string): boolean {
  const last = rel.split("/").pop() ?? ""
  return !last.includes(".")
}
