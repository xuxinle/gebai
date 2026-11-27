/**
 * 托管应用的文件来源：**三层叠加**（运行时磁盘 → 源码内联 → 构建内嵌）。
 *
 * ## 三层各回答什么问题
 *
 * | 层 | 来源 | 解决 |
 * |---|---|---|
 * | `disk`（运行时动态载入） | `GEBAI_APPS_DIRS` → `{GEBAI_HOME}/data/apps` → `{GEBAI_HOME}/apps` | 改文件刷新即生效；部署方自己塞应用 |
 * | `inline`（源码内联） | 仓库 `apps/*.ts` 的 `export const apps = [...]`（文件内容是字符串、后端是函数） | 几行字就是一个应用，不必建目录 |
 * | `embedded`（构建内嵌） | 构建期把 `apps/` 目录全量 base64 内嵌（`core/apps.bundle.generated.ts`） | 二进制单文件产物没有源码树，仍要自带应用 |
 *
 * ## 合并口径：逐文件叠加，而非「三份并列的应用」
 *
 * 同一 id 出现在多层时**不是并列两三个应用**，而是按文件叠加：任意文件「优先级高的层有就用它」，
 * 清单同理由优先级最高的层给出。于是：
 * - 内置应用只要在 `{GEBAI_HOME}/data/apps/{id}/index.html` 放一个文件，就能就地改掉页面（不必重构建）；
 * - 内置应用只要放一个 `server.js`（并在该 id 的磁盘 app.json 声明 backend）就凭空长出后端；
 * - 源码内联应用写坏了想临时覆盖，也可以在同一 id 的磁盘目录里放文件。
 *
 * 优先级：**disk > inline > embedded**（越「近」越优先：运行时 > 源码 > 构建产物）。
 *
 * ## 动态载入（无重启）
 *
 * 扫描结果带 TTL 缓存（默认 1s），`readFile` 每次 stat 磁盘文件——**新增/删除应用最多延迟一个 TTL
 * 就可见，改文件内容立即生效**。不挂 inotify：应用编辑的节奏是「保存→刷新页面」，1s 的目录缓存
 * 远快于人的动作，而 fs.watch 在容器/网络盘上的可靠性代价不值得付。
 */
import { existsSync, mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from "node:fs"
import { readFile } from "node:fs/promises"
import { createHash } from "node:crypto"
import { join, resolve, sep } from "node:path"
import { APP_ID_RE, APP_MANIFEST_FILE, APP_DEFAULT_ENTRY, normalizeAppRelPath, parseAppManifest, type AppManifest } from "./manifest"
import type { AppBackendHandler } from "@gebai/sdk"
import { normalizeInlineApp, type InlineApp } from "./inline"

/** 内嵌应用表（构建期 `scripts/build-apps-bundle.ts` 生成）：appId → 相对路径 → base64。 */
export type EmbeddedApps = Record<string, Record<string, string>>

/** 扫描缓存 TTL（毫秒）：目录结构变化的最大可见延迟（文件内容变化不受此限，逐请求 stat）。 */
const SCAN_TTL_MS = 1000

/** 应用文件来源层。 */
export type AppLayer = "disk" | "inline" | "embedded"

/** 层优先级（数字大者胜）：运行时 > 源码内联 > 构建内嵌。 */
const LAYER_RANK: Record<AppLayer, number> = { embedded: 0, inline: 1, disk: 2 }

/** 一个可托管的应用（三层合并后的最终形态）。 */
export interface AppEntry {
  manifest: AppManifest
  /** 清单来源层（= 该 id 的最高优先级层；诊断与列表展示用）。 */
  layer: AppLayer
  /** 磁盘层应用目录绝对路径（有磁盘层时为该目录；纯内联/纯内嵌应用为 null）。 */
  dir: string | null
  /** 是否可跑后端：磁盘层声明了 backend 文件，或源码内联层给了函数，或内嵌层可落地。 */
  hasBackend: boolean
  /** 源码内联层给的后端函数（同进程直接调用，不需要落文件）。 */
  inlineBackend?: AppBackendHandler
  /**
   * 取后端模块所在目录（需 import 文件型后端时用）。
   *
   * 磁盘层直接给目录；**内嵌层（二进制形态）没有源码树**，于是把该应用的文件
   * 幂等地落地到 `{GEBAI_HOME}/data/apps-cache/{id}/` 再 import——这是「单文件二进制自带含后端的应用」
   * 唯一可行的路（Bun 不支持从内存字符串 import ES 模块）。落地只在该应用真被请求后端的首次发生。
   */
  backendDir: () => Promise<string | null>
  /** 清单解析警告（写错的字段，不影响可用性）。 */
  warnings: string[]
  /** 致命问题（如入口文件缺失）：仍会列出，但请求时返回 503 说明原因。 */
  error?: string
  /** 该 id 存在哪些层（诊断用：一眼看出「为什么改了不生效」——改的是低优先级层）。 */
  layers: AppLayer[]
}

/** 文件读取结果。 */
export interface AppFile {
  body: Uint8Array
  /** mtime（磁盘层）或 0（内联/内嵌层）——缓存键用。 */
  mtime: number
  /** 强 ETag（磁盘层按 size+mtime，内联/内嵌按内容哈希）。 */
  etag: string
  layer: AppLayer
}

/** 一层应用（扫描产物）。 */
interface LayerEntry {
  layer: AppLayer
  manifest: AppManifest
  warnings: string[]
  dir: string | null
  hasBackend: boolean
  inlineBackend?: AppBackendHandler
  /** 取后端模块目录（磁盘层直接给；内嵌层幂等落地到缓存目录）。 */
  backendDir: () => Promise<string | null>
  /** 该层是否有某个文件（已归一的 POSIX 相对路径）。 */
  has: (rel: string) => boolean
  /** 读该层文件（不存在返回 null）。 */
  read: (rel: string) => Promise<AppFile | null>
}

export interface AppsStoreOptions {
  /** `{GEBAI_HOME}`：出厂磁盘根 `{home}/apps` 与用户数据根 `{home}/data/apps` 由它派生。 */
  home: string
  /** 额外运行时目录（`GEBAI_APPS_DIRS`）：顺序即优先级（排最前）。 */
  dirs?: string[]
  /** 源码内联层（仓库 `apps/*.ts` 扫描结果；构建产物形态为空数组）。 */
  inline?: InlineApp[]
  /** 构建内嵌层（二进制形态；源码形态为空表）。 */
  embedded?: EmbeddedApps | null
  /** 单文件读取上限（超过即视为不存在——防止误把巨大目录当应用）。 */
  maxFileBytes?: number
  /** 扫描缓存 TTL 覆盖（测试用）。 */
  scanTtlMs?: number
}

/** 内容 → 强 ETag（内联/内嵌层没有 mtime，只能按内容算；字节量小，一次 sha1 可忽略）。 */
function contentEtag(body: Uint8Array): string {
  return `"${createHash("sha1").update(body).digest("hex").slice(0, 16)}"`
}

/** 磁盘文件的 ETag（size+mtime，与文件工作台同口径：跨请求稳定且不读内容）。 */
function statEtag(size: number, mtimeMs: number): string {
  return `W/"${Math.round(mtimeMs)}-${size}"`
}

export class AppsStore {
  private cache: { at: number; entries: Map<string, AppEntry> } | null = null
  private layerCache: { at: number; byId: Map<string, LayerEntry[]> } | null = null
  /** 内存层字节解码缓存（base64 → bytes）：同一条目只解码一次。 */
  private readonly decoded = new Map<string, Uint8Array>()
  /** 磁盘文件字节缓存（key = 绝对路径 + size + mtime）：命中条件严格，改文件必失效。 */
  private readonly diskCache = new Map<string, AppFile>()
  /** 源码内联层（构造时归一一次：文件内容编码、id/路径校验都在这里做完）。 */
  private readonly inline: Map<string, LayerEntry> = new Map()
  private readonly inlineWarnings: string[] = []

  constructor(private readonly opts: AppsStoreOptions) {
    const max = opts.maxFileBytes ?? Infinity
    for (const raw of opts.inline ?? []) {
      const norm = normalizeInlineApp(raw)
      if ("error" in norm) {
        this.inlineWarnings.push(`内联应用定义非法（${String((raw as { id?: string })?.id ?? "?")}）：${norm.error}`)
        continue
      }
      const files = norm.files
      this.inline.set(norm.manifest.id, {
        layer: "inline",
        manifest: norm.manifest,
        warnings: norm.warnings,
        dir: null,
        hasBackend: !!norm.backend,
        ...(norm.backend ? { inlineBackend: norm.backend } : {}),
        // 内联应用是函数本体：不需要落盘 import
        backendDir: async () => null,
        has: (rel) => {
          const b = files.get(rel)
          return !!b && b.byteLength <= max
        },
        read: async (rel) => {
          const body = files.get(rel)
          if (!body || body.byteLength > max) return null
          return { body, mtime: 0, etag: contentEtag(body), layer: "inline" as const }
        },
      })
    }
  }

  /** 磁盘层根（顺序即优先级）：显式目录 → 用户数据目录 → 出厂目录。 */
  roots(): string[] {
    const home = this.opts.home
    const out: string[] = []
    for (const d of this.opts.dirs ?? []) if (d && d.trim()) out.push(resolve(d.trim()))
    out.push(join(home, "data", "apps"))
    out.push(join(home, "apps"))
    return [...new Set(out)]
  }

  /** 源码内联层的加载告警（启动期日志与 REST 列表如实带出）。 */
  inlineErrors(): string[] {
    return [...this.inlineWarnings]
  }

  /** 扫描（带 TTL 缓存）；force=true 立即重扫（REST refresh、索引页与测试用）。 */
  list(force = false): AppEntry[] {
    const ttl = this.opts.scanTtlMs ?? SCAN_TTL_MS
    if (!force && this.cache && Date.now() - this.cache.at < ttl) return [...this.cache.entries.values()]
    const entries = this.scan()
    this.cache = { at: Date.now(), entries }
    return [...entries.values()]
  }

  /** 取单个应用（id 非法直接 null——不查文件系统）。 */
  get(id: string): AppEntry | null {
    if (!APP_ID_RE.test(id)) return null
    // 单取走全量扫描缓存：应用数量是「人手工管理的个位数~几十」，逐 id 单独 stat 反而更贵
    return this.list().find((e) => e.manifest.id === id) ?? null
  }

  /** 显式失效（应用目录变更后想立刻可见时调用；REST refresh 走这里）。 */
  invalidate(): void {
    this.cache = null
    this.layerCache = null
  }

  /**
   * 读应用内文件（按层优先级逐个试）。不存在/越界/超限/是目录 → null。
   *
   * 磁盘路径必须以**应用目录**为界（`resolve` 后必须仍在 dir 内）：`normalizeAppRelPath` 已挡掉
   * `..`/绝对路径，这里再按解析结果复核一次——防的是符号链接指到应用目录外。
   */
  async readFile(entry: AppEntry, rel: string): Promise<AppFile | null> {
    const norm = normalizeAppRelPath(rel)
    if (!norm) return null
    for (const layer of this.layersOf(entry.manifest.id)) {
      const file = await layer.read(norm)
      if (file) return file
    }
    return null
  }

  /** 文件是否存在（不读内容：SPA 回退判据用，省一次大文件读取）。 */
  hasFile(entry: AppEntry, rel: string): boolean {
    const norm = normalizeAppRelPath(rel)
    if (!norm) return false
    return this.layersOf(entry.manifest.id).some((l) => l.has(norm))
  }

  /** 某 id 的全部层（按优先级从高到低）。 */
  private layersOf(id: string): LayerEntry[] {
    const list = this.scanLayers().get(id) ?? []
    return list.slice().sort((a, b) => LAYER_RANK[b.layer] - LAYER_RANK[a.layer])
  }

  /** 分层视图（懒建，同一 TTL）。 */
  private scanLayers(): Map<string, LayerEntry[]> {
    const ttl = this.opts.scanTtlMs ?? SCAN_TTL_MS
    if (this.layerCache && Date.now() - this.layerCache.at < ttl) return this.layerCache.byId
    const byId = new Map<string, LayerEntry[]>()
    const push = (id: string, layer: LayerEntry): void => {
      const list = byId.get(id) ?? []
      list.push(layer)
      byId.set(id, list)
    }
    for (const [id, layer] of this.embeddedLayers()) push(id, layer)
    for (const [id, layer] of this.inline) push(id, layer)
    for (const [id, layer] of this.diskLayers()) push(id, layer)
    this.layerCache = { at: Date.now(), byId }
    return byId
  }

  /** 构建内嵌层（base64 表 → 层视图）。 */
  private embeddedLayers(): Map<string, LayerEntry> {
    const out = new Map<string, LayerEntry>()
    const max = this.opts.maxFileBytes ?? Infinity
    for (const [id, files] of Object.entries(this.opts.embedded ?? {})) {
      if (!APP_ID_RE.test(id)) continue
      const parsed = this.manifestOf(undefined, files[APP_MANIFEST_FILE], id)
      const read = async (rel: string): Promise<AppFile | null> => {
        const b64 = files[rel]
        if (!b64) return null
        const key = `embedded:${id}:${rel}`
        let body = this.decoded.get(key)
        if (!body) {
          body = new Uint8Array(Buffer.from(b64, "base64"))
          this.decoded.set(key, body)
        }
        if (body.byteLength > max) return null
        return { body, mtime: 0, etag: contentEtag(body), layer: "embedded" as const }
      }
      out.set(id, {
        layer: "embedded",
        manifest: parsed.manifest,
        warnings: parsed.warnings,
        dir: null,
        hasBackend: !!parsed.manifest.backend && !!files[parsed.manifest.backend],
        // 内嵌层（二进制形态）没有源码树：把该应用文件幂等落地到缓存目录再 import
        backendDir: async () => this.materialize(id),
        has: (rel) => !!files[rel],
        read,
      })
    }
    return out
  }

  /** 磁盘层（每个根下的子目录）。 */
  private diskLayers(): Map<string, LayerEntry> {
    const out = new Map<string, LayerEntry>()
    const max = this.opts.maxFileBytes ?? Infinity
    for (const root of this.roots()) {
      let names: string[]
      try {
        names = readdirSync(root)
      } catch {
        continue // 目录不存在：正常形态（默认根本来就可能没建）
      }
      for (const name of names.sort()) {
        if (name.startsWith(".") || name === "node_modules") continue
        if (!APP_ID_RE.test(name)) continue
        const dir = join(root, name)
        try {
          if (!statSync(dir).isDirectory()) continue
        } catch {
          continue
        }
        const exists = (rel: string): boolean => {
          try {
            return statSync(join(dir, rel)).isFile()
          } catch {
            return false
          }
        }
        const hasManifest = exists(APP_MANIFEST_FILE)
        // 无清单也可成应用：只要目录里有默认入口（丢一个 index.html 就是最小应用）
        if (!hasManifest && !exists(APP_DEFAULT_ENTRY)) continue
        const parsed = hasManifest ? this.manifestOf(join(dir, APP_MANIFEST_FILE), undefined, name) : { manifest: parseAppManifest(undefined, name).manifest, warnings: [] }
        const warnings = [...parsed.warnings]
        const hasBackend = !!parsed.manifest.backend && exists(parsed.manifest.backend)
        if (parsed.manifest.backend && !hasBackend) warnings.push(`backend「${parsed.manifest.backend}」未在应用目录中找到（后端不可用）`)
        out.set(name, {
          layer: "disk",
          manifest: parsed.manifest,
          warnings,
          dir,
          hasBackend,
          backendDir: async () => dir,
          has: (rel) => {
            const abs = resolve(dir, rel)
            const base = resolve(dir)
            if (abs !== base && !abs.startsWith(base + sep)) return false
            try {
              const st = statSync(abs)
              return st.isFile() && st.size <= max
            } catch {
              return false
            }
          },
          read: async (rel) => {
            const abs = resolve(dir, rel)
            const base = resolve(dir)
            if (abs !== base && !abs.startsWith(base + sep)) return null
            try {
              const st = statSync(abs)
              if (!st.isFile() || st.size > max) return null
              const etag = statEtag(st.size, st.mtimeMs)
              const cacheKey = `${abs}|${st.size}|${st.mtimeMs}`
              const hit = this.diskCache.get(cacheKey)
              if (hit) return hit
              // 缓存为「同一次页面加载请求几十个静态资源」服务；上限按条目数兜底
              if (this.diskCache.size > 256) this.diskCache.clear()
              const body = new Uint8Array(await readFile(abs))
              const file: AppFile = { body, mtime: st.mtimeMs, etag, layer: "disk" }
              this.diskCache.set(cacheKey, file)
              return file
            } catch {
              return null
            }
          },
        })
      }
    }
    return out
  }

  /** 全量合并：清单取最高优先级层，文件读取按层优先级逐个试。 */
  private scan(): Map<string, AppEntry> {
    const out = new Map<string, AppEntry>()
    const byId = this.scanLayers()
    for (const [id, layers] of byId) {
      const sorted = layers.slice().sort((a, b) => LAYER_RANK[b.layer] - LAYER_RANK[a.layer])
      const top = sorted[0]!
      const warnings = [...top.warnings]
      // 低优先级层的问题也要说出来：否则「内置清单写错但被磁盘覆盖」会完全静默
      for (const l of sorted.slice(1)) for (const w of l.warnings) if (!warnings.includes(w)) warnings.push(w)
      const inlineBackend = sorted.find((l) => l.inlineBackend)?.inlineBackend
      const entry: AppEntry = {
        manifest: top.manifest,
        layer: top.layer,
        dir: top.dir,
        hasBackend: sorted.some((l) => l.hasBackend),
        warnings,
        layers: sorted.map((l) => l.layer),
        // 后端模块目录：按层优先级取第一个能给出的（内联函数层给 null——它不需要目录）
        backendDir: async () => {
          if (inlineBackend) return null
          for (const l of sorted) {
            if (!l.hasBackend) continue
            const d = await l.backendDir()
            if (d) return d
          }
          return null
        },
        ...(inlineBackend ? { inlineBackend } : {}),
      }
      if (!sorted.some((l) => l.has(top.manifest.entry))) entry.error = `入口文件缺失：${top.manifest.entry}`
      out.set(id, entry)
    }
    return out
  }

  /**
   * 把内嵌应用的**全部文件**幂等落地到磁盘缓存目录（`{GEBAI_HOME}/data/apps-cache/{id}/`），
   * 返回该目录：二进制形态下要 import `server.js` 只能走文件。
   *
   * 不叫“解包到应用目录”：应用目录（`data/apps`）是**用户地盘**，写进去会变成“用户既有磁盘应用覆盖内嵌”——
   * 而下一次想恢复内置版本时反而要猜哪个文件是自己改的。缓存目录语义清楚：**可删可重建**。
   */
  private async materialize(id: string): Promise<string | null> {
    const files = this.opts.embedded?.[id]
    if (!files) return null
    const dir = join(this.opts.home, "data", "apps-cache", id)
    try {
      mkdirSync(dir, { recursive: true })
      for (const [rel, b64] of Object.entries(files)) {
        const norm = normalizeAppRelPath(rel)
        if (!norm) continue
        const abs = join(dir, norm)
        const body = Buffer.from(b64, "base64")
        try {
          if (statSync(abs).size === body.byteLength && readFileSync(abs).equals(body)) continue
        } catch {
          /* 不存在：写 */
        }
        mkdirSync(join(abs, ".."), { recursive: true })
        writeFileSync(abs, body)
      }
      return dir
    } catch {
      return null
    }
  }

  /** 清单解析：磁盘文件优先、其次内嵌串、都没有用默认；JSON 坏掉按空清单 + 警告。 */
  private manifestOf(diskPath: string | undefined, embeddedB64: string | undefined, id: string): { manifest: AppManifest; warnings: string[] } {
    let raw: unknown
    try {
      if (diskPath && existsSync(diskPath)) raw = JSON.parse(readFileSync(diskPath, "utf8"))
      else if (embeddedB64) raw = JSON.parse(Buffer.from(embeddedB64, "base64").toString("utf8"))
    } catch (err) {
      const parsed = parseAppManifest(undefined, id)
      return { manifest: parsed.manifest, warnings: [`${APP_MANIFEST_FILE} 解析失败（${String((err as Error).message || err)}）：按默认清单处理`] }
    }
    return parseAppManifest(raw, id)
  }
}
