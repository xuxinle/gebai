/** 文件工作台 · 文件域路由（DESIGN「文件工作台」）：根内目录/文件的列举、读取、流式原样输出、下载、
 *  搜索、归档浏览、写入与结构操作（新建/改名/移动/复制/删除/上传/回收站）。
 *
 *  契约：所有端点以 `(root, path)` 寻址；`root` 见 core/fs/roots.ts（sess:/proj:/user:/abs:）。
 *  只读端点（list/stat/read/raw/office/download/search/archive）不写审计；写端点一律过写开关 + 审计留痕。
 */
import type { Context } from "hono"
import type { RouteCtx } from "./context"
import type { RootContext, ResolvedRoot } from "../core/fs/roots"
import { FsError, fsForbidden, resolveInRoot, resolveRoot, type FileRoot } from "../core/fs/roots"
import {
  asSortKey,
  listDirectory,
  listFilesInRoot,
  rawResponse,
  readTextFile,
  relOf,
  searchInRoot,
  statPath,
  zipPaths,
} from "../core/fs/service"
import { listZip, readZipEntry, safeEntryName } from "../core/fs/archive"
import {
  copyEntry,
  createDirectory,
  deleteEntries,
  isSaveConflict,
  moveEntry,
  renameEntry,
  saveText,
  uploadFiles,
} from "../core/fs/write"
import { buildRootContext, errorResponse, parseEnvInput, pickBool, pickBoolMaybe, pickParam, requireFsEnabled } from "./fs-shared"
import { FsWatchHub, gitWatchDirs, type WatchTarget } from "../core/fs/watch"
import { existsSync } from "node:fs"
import { relative } from "node:path"

/** 单次订阅最多监听的目录数（前端已先夹一次，这里是兜底）。 */
const MAX_WATCH_DIRS = 64
/** 在途长轮询上限：超出后退化为即时检查（背压），不让连接/fd 被吃光。 */
const MAX_WATCH_WAITERS = 16
/** 仓库根探测缓存 TTL（毫秒）：git 元数据目录清单不必每轮都 spawn 一次 git。 */
const WATCH_REPO_TTL = 30_000

/** 变更绝对路径 → 根内相对路径（根本身 / 根外 → null，调用方丢弃）。 */
function relWatchPath(rootAbs: string, abs: string): string | null {
  const rel = relative(rootAbs, abs).replace(/\\/g, "/")
  if (!rel || rel.startsWith("..")) return null
  return rel
}

export function registerFsRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  /** 变更监听中枢（进程内共享：多个工作台标签页共用同一批 watcher），见 core/fs/watch.ts。 */
  const watchHub = new FsWatchHub()
  /** 在途长轮询计数（背压）。 */
  let watchWaiters = 0
  /** 仓库根探测缓存（root 绝对路径 → repo）：git 元数据目录清单不必每轮都 spawn 一次 git。 */
  const watchRepoCache = new Map<string, { repo: string | null; ts: number }>()

  /** git 元数据监听项（仓库根缓存 TTL 内复用；非仓库/未启用 git 返回空）。 */
  async function repoWatchTargets(rootAbs: string): Promise<WatchTarget[]> {
    if (!d.git) return []
    const now = Date.now()
    const hit = watchRepoCache.get(rootAbs)
    if (hit && now - hit.ts < WATCH_REPO_TTL) return hit.repo ? gitWatchDirs(hit.repo) : []
    const repo = await d.git.repoRoot(rootAbs).catch(() => null)
    watchRepoCache.set(rootAbs, { repo, ts: now })
    return repo ? gitWatchDirs(repo) : []
  }

  /** 请求级上下文：身份 + RootContext（项目注册表 / 开关 / 沙箱）。 */
  async function ctxFor(c: Context, body?: Record<string, unknown>): Promise<{ user: string; ctx: RootContext; envInput: unknown }> {
    const user = await userOf(c)
    const sessionId = pickParam(c, "session", body) || undefined
    const envInput = c.req.query("env") ?? body?.env
    const ctx = await buildRootContext(d, user, { sessionId, envInput })
    return { user: user.id, ctx, envInput }
  }

  /** 解析 root + 目标绝对路径。 */
  function target(ctx: RootContext, rootId: string, relPath: string, opts: { allowAbsolute?: boolean } = {}): { root: ResolvedRoot; abs: string; rel: string } {
    const root = resolveRoot(rootId, ctx)
    const abs = resolveInRoot(root.abs, relPath, { allowAbsolute: opts.allowAbsolute ?? root.kind === "abs" })
    return { root, abs, rel: relOf(root.abs, abs) }
  }

  function assertWritable(root: ResolvedRoot): void {
    if (!root.writable) throw fsForbidden("文件工作台为只读模式（GEBAI_FS_WRITE=false）")
  }

  function audit(user: string, c: Context, entry: { action: string; root: string; path?: string; detail?: Record<string, unknown>; ok: boolean; error?: string }): void {
    if (!d.fsAudit) return
    d.fsAudit.record({
      ts: Date.now(),
      user,
      source: c.req.path.startsWith("/api/v1/fs") || c.req.path.startsWith("/api/v1/git") ? "web" : "api",
      ip: d.config.trustProxy ? c.req.header("x-forwarded-for") : undefined,
      ...entry,
    })
  }

  async function jsonBody(c: Context): Promise<Record<string, unknown>> {
    try {
      const b = await c.req.json()
      return b && typeof b === "object" ? (b as Record<string, unknown>) : {}
    } catch {
      return {}
    }
  }

  /* --------------------------- 只读：列举与元信息 --------------------------- */

  app.get("/api/v1/fs/list", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const { ctx } = await ctxFor(c)
      const rootId = c.req.query("root") || ""
      const rel = c.req.query("path") || ""
      // 显式参数优先（前端「更多」菜单可随时切换），缺省才用服务端配置的默认值
      const showHidden = pickBoolMaybe(c, "showHidden") ?? (d.config.fsHidden === true)
      const root = resolveRoot(rootId, ctx)
      const result = await listDirectory(root.abs, rel, {
        showHidden,
        sort: asSortKey(c.req.query("sort")),
        dirsFirst: c.req.query("dirsFirst") !== "0",
        limit: Number(c.req.query("limit")) || undefined,
      })
      return c.json({ root: rootId, path: rel, ...result, showHidden })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** 递归树（深度受限，前端首屏一次性展开若干层；更深处继续走 list 懒加载）。 */
  app.get("/api/v1/fs/tree", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const { ctx } = await ctxFor(c)
      const rootId = c.req.query("root") || ""
      const rel = c.req.query("path") || ""
      const depth = Math.max(0, Math.min(Number(c.req.query("depth")) || 1, 5))
      // 显式参数优先（前端「更多」菜单可随时切换），缺省才用服务端配置的默认值
      const showHidden = pickBoolMaybe(c, "showHidden") ?? (d.config.fsHidden === true)
      const root = resolveRoot(rootId, ctx)
      const absRoot = resolveInRoot(root.abs, rel, { allowAbsolute: root.kind === "abs" })
      interface TreeNode {
        name: string
        path: string
        type: "file" | "dir" | "symlink" | "other"
        size: number
        mtime: number
        children?: TreeNode[]
        hasChildren?: boolean
        truncated?: boolean
      }
      const build = async (absDir: string, level: number): Promise<TreeNode[]> => {
        const { entries, truncated } = await listDirectory(root.abs, relOf(root.abs, absDir), { showHidden, dirsFirst: true })
        const nodes: TreeNode[] = []
        for (const e of entries) {
          const node: TreeNode = { name: e.name, path: e.path, type: e.type, size: e.size, mtime: e.mtime }
          if (e.type === "dir") {
            if (level < depth) node.children = await build(resolveInRoot(root.abs, e.path), level + 1)
            else node.hasChildren = true
          }
          nodes.push(node)
        }
        if (truncated) nodes.push({ name: "…（条目过多，请用过滤缩小范围）", path: "", type: "other", size: 0, mtime: 0 })
        return nodes
      }
      const children = await build(absRoot, 1)
      return c.json({ root: rootId, path: rel, depth, children })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/fs/stat", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const { ctx } = await ctxFor(c)
      const rootId = c.req.query("root") || ""
      const root = resolveRoot(rootId, ctx)
      const paths = c.req.queries("path") ?? (c.req.query("path") ? [c.req.query("path") as string] : [])
      if (!paths.length) throw new FsError(400, "缺少 path 参数")
      const out = []
      for (const p of paths) out.push(await statPath(root.abs, p))
      return c.json({ root: rootId, items: out })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /* --------------------------- 只读：内容 --------------------------- */

  app.get("/api/v1/fs/read", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const { ctx } = await ctxFor(c)
      const rootId = c.req.query("root") || ""
      const { abs, rel } = target(ctx, rootId, c.req.query("path") || "", { allowAbsolute: true })
      const maxBytes = Math.min(Number(c.req.query("maxBytes")) || d.config.fsMaxRead, d.config.fsMaxRead)
      const result = await readTextFile(abs, { maxBytes, forceText: pickBool(c, "forceText") })
      return c.json({ root: rootId, path: rel, ...result })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** 原样字节流（图片/音视频/PDF/字体/二进制）：支持 Range（拖动进度、跳页、断点续传的前提）。 */
  app.get("/api/v1/fs/raw", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const { ctx } = await ctxFor(c)
      const { abs } = target(ctx, c.req.query("root") || "", c.req.query("path") || "", { allowAbsolute: true })
      return await rawResponse(abs, {
        rangeHeader: c.req.header("range"),
        ifNoneMatch: c.req.header("if-none-match"),
        download: pickBool(c, "download"),
        forceMime: c.req.query("mime") || undefined,
      })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** Office 阅读视图（复用 wps 子Agent 的结构化 HTML 渲染；与消息流文件卡同一真相源）。 */
  app.get("/api/v1/fs/office", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const { ctx } = await ctxFor(c)
      const { abs } = target(ctx, c.req.query("root") || "", c.req.query("path") || "", { allowAbsolute: true })
      const { renderOfficeReadingView } = await import("@gebai/agents")
      const html = await renderOfficeReadingView(abs)
      return new Response(html, { headers: { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-cache" } })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /* --------------------------- 下载 / 打包 --------------------------- */

  app.get("/api/v1/fs/download", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const { ctx, user } = await ctxFor(c)
      const { root, abs, rel } = target(ctx, c.req.query("root") || "", c.req.query("path") || "", { allowAbsolute: true })
      const stat = await statPath(root.abs, rel)
      if (stat.type === "dir") {
        const zip = await zipPaths(root.abs, [rel || "."], d.config.fsMaxZip)
        audit(user, c, { action: "fs.download", root: root.id, path: rel, detail: { zip: true }, ok: true })
        return new Response(zip, {
          headers: {
            "Content-Type": "application/zip",
            "Content-Disposition": `attachment; filename*=UTF-8''${encodeURIComponent(`${(rel || "root").split("/").pop() || "root"}.zip`)}`,
          },
        })
      }
      audit(user, c, { action: "fs.download", root: root.id, path: rel, ok: true })
      return await rawResponse(abs, {
        rangeHeader: c.req.header("range"),
        ifNoneMatch: c.req.header("if-none-match"),
        download: true,
      })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.post("/api/v1/fs/download", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const body = await jsonBody(c)
      const { ctx, user } = await ctxFor(c, body)
      const rootId = pickParam(c, "root", body)
      const paths = Array.isArray(body.paths) ? body.paths.map(String) : []
      if (!paths.length) throw new FsError(400, "缺少 paths")
      const root = resolveRoot(rootId, ctx)
      const zip = await zipPaths(root.abs, paths, d.config.fsMaxZip)
      audit(user, c, { action: "fs.download", root: rootId, detail: { count: paths.length, zip: true }, ok: true })
      return new Response(zip, { headers: { "Content-Type": "application/zip", "Content-Disposition": 'attachment; filename="files.zip"' } })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /* --------------------------- 搜索 --------------------------- */

  /**
   * 文件索引（快速打开用）：列 root 下全部文件相对路径。
   * 与 `/fs/search` 分开：那条是「搜关键字」，这条是「拿一份可模粗匹配的名单」，语义与结果规模都不同。
   */
  app.get("/api/v1/fs/files", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const { ctx } = await ctxFor(c)
      const rootId = c.req.query("root") || ""
      const root = resolveRoot(rootId, ctx)
      // 与 list 同口径：显式参数优先，缺省用服务端配置的默认值
      const showHidden = pickBoolMaybe(c, "showHidden") ?? (d.config.fsHidden === true)
      const result = await listFilesInRoot(root.abs, { showHidden, limit: Number(c.req.query("limit")) || undefined })
      return c.json({ root: rootId, showHidden, ...result })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/fs/search", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const { ctx } = await ctxFor(c)
      const rootId = c.req.query("root") || ""
      const root = resolveRoot(rootId, ctx)
      const result = await searchInRoot(root.abs, {
        query: c.req.query("q") || c.req.query("query") || "",
        mode: c.req.query("mode") === "name" ? "name" : "content",
        glob: c.req.query("glob") || undefined,
        ignoreCase: c.req.query("ignoreCase") !== "0",
        regex: pickBool(c, "regex"),
        maxResults: Number(c.req.query("maxResults")) || undefined,
        maxFileSize: Number(c.req.query("maxFileSize")) || undefined,
      })
      return c.json({ root: rootId, ...result })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /* --------------------------- 归档浏览 --------------------------- */

  app.get("/api/v1/fs/archive", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const { ctx } = await ctxFor(c)
      const { abs, rel } = target(ctx, c.req.query("root") || "", c.req.query("path") || "", { allowAbsolute: true })
      const buf = new Uint8Array(await Bun.file(abs).arrayBuffer())
      const entries = listZip(buf)
      const entryName = c.req.query("entry")
      if (entryName) {
        const entry = entries.find((e) => e.name === entryName)
        if (!entry) throw new FsError(404, `归档内条目不存在: ${entryName}`)
        const data = readZipEntry(buf, entry)
        const mime = c.req.query("download") ? "application/octet-stream" : undefined
        return new Response(data, {
          headers: {
            "Content-Type": mime ?? "application/octet-stream",
            ...(c.req.query("download") ? { "Content-Disposition": `attachment; filename*=UTF-8''${encodeURIComponent(entryName.split("/").pop() ?? "entry")}` } : {}),
          },
        })
      }
      return c.json({
        root: c.req.query("root") || "",
        path: rel,
        size: buf.length,
        entries: entries.map((e) => ({ name: e.name, size: e.size, compressedSize: e.compressedSize, isDir: e.isDir, mtime: e.mtime, method: e.method })),
      })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.post("/api/v1/fs/archive/extract", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const body = await jsonBody(c)
      const { ctx, user } = await ctxFor(c, body)
      const rootId = pickParam(c, "root", body)
      const source = pickParam(c, "path", body)
      const destRel = pickParam(c, "to", body)
      const { root, abs } = target(ctx, rootId, source, { allowAbsolute: true })
      assertWritable(root)
      const destAbs = resolveInRoot(root.abs, destRel)
      const buf = new Uint8Array(await Bun.file(abs).arrayBuffer())
      const entries = listZip(buf)
      const only = Array.isArray(body.entries) ? new Set(body.entries.map(String)) : null
      let count = 0
      for (const e of entries) {
        if (e.isDir) continue
        if (only && !only.has(e.name)) continue
        const safe = safeEntryName(e.name)
        if (!safe) continue
        const out = resolveInRoot(destAbs, safe)
        await createDirectory(out.replace(/[/\\][^/\\]*$/, ""), true).catch(() => {})
        const data = readZipEntry(buf, e)
        await Bun.write(out, data)
        count++
      }
      audit(user, c, { action: "fs.extract", root: rootId, path: source, detail: { to: destRel, count }, ok: true })
      return c.json({ ok: true, extracted: count, to: destRel })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /* --------------------------- 写入与结构操作 --------------------------- */

  app.put("/api/v1/fs/write", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    const body = await jsonBody(c)
    let auditRef: { user: string; root: string; path: string } | null = null
    try {
      const { ctx, user } = await ctxFor(c, body)
      const rootId = pickParam(c, "root", body)
      const rel = pickParam(c, "path", body)
      if (!rel) throw new FsError(400, "缺少 path")
      const root = resolveRoot(rootId, ctx)
      assertWritable(root)
      const abs = resolveInRoot(root.abs, rel)
      auditRef = { user, root: rootId, path: rel }
      const content = typeof body.content === "string" ? body.content : ""
      const result = await saveText(abs, {
        content,
        encoding: body.encoding,
        eol: (body.eol as "keep" | "lf" | "crlf" | "cr") ?? "keep",
        expectedEtag: typeof body.expectedEtag === "string" ? body.expectedEtag : undefined,
        createDirs: body.createDirs !== false,
        maxBytes: d.config.fsMaxWrite,
      })
      audit(user, c, { action: "fs.write", root: rootId, path: rel, detail: { size: result.size, encoding: result.encoding }, ok: true })
      return c.json({ ok: true, path: rel, ...result })
    } catch (err) {
      if (isSaveConflict(err)) {
        if (auditRef) audit(auditRef.user, c, { action: "fs.write", root: auditRef.root, path: auditRef.path, ok: false, error: "etag_mismatch" })
        return c.json({ error: (err as Error).message, code: 409, reason: "etag_mismatch", current: (err as { current: unknown }).current }, 409)
      }
      if (auditRef) audit(auditRef.user, c, { action: "fs.write", root: auditRef.root, path: auditRef.path, ok: false, error: String((err as Error).message) })
      return errorResponse(c, err)
    }
  })

  app.post("/api/v1/fs/mkdir", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const body = await jsonBody(c)
      const { ctx, user } = await ctxFor(c, body)
      const rootId = pickParam(c, "root", body)
      const rel = pickParam(c, "path", body)
      const root = resolveRoot(rootId, ctx)
      assertWritable(root)
      const abs = resolveInRoot(root.abs, rel)
      await createDirectory(abs, body.parents !== false)
      audit(user, c, { action: "fs.mkdir", root: rootId, path: rel, ok: true })
      return c.json({ ok: true, path: rel })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.post("/api/v1/fs/rename", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const body = await jsonBody(c)
      const { ctx, user } = await ctxFor(c, body)
      const rootId = pickParam(c, "root", body)
      const rel = pickParam(c, "path", body)
      const newName = pickParam(c, "newName", body)
      if (!newName) throw new FsError(400, "缺少 newName")
      const root = resolveRoot(rootId, ctx)
      assertWritable(root)
      const abs = resolveInRoot(root.abs, rel)
      const dest = await renameEntry(abs, newName)
      audit(user, c, { action: "fs.rename", root: rootId, path: rel, detail: { newName }, ok: true })
      return c.json({ ok: true, path: relOf(root.abs, dest) })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.post("/api/v1/fs/move", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const body = await jsonBody(c)
      const { ctx, user } = await ctxFor(c, body)
      const rootId = pickParam(c, "root", body)
      const rel = pickParam(c, "path", body)
      const to = pickParam(c, "to", body)
      const root = resolveRoot(rootId, ctx)
      assertWritable(root)
      const src = resolveInRoot(root.abs, rel)
      const dest = resolveInRoot(root.abs, to)
      await moveEntry(src, dest, { overwrite: pickBool(c, "overwrite", body) })
      audit(user, c, { action: "fs.move", root: rootId, path: rel, detail: { to }, ok: true })
      return c.json({ ok: true, path: relOf(root.abs, dest) })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.post("/api/v1/fs/copy", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const body = await jsonBody(c)
      const { ctx, user } = await ctxFor(c, body)
      const rootId = pickParam(c, "root", body)
      const rel = pickParam(c, "path", body)
      const to = pickParam(c, "to", body)
      const root = resolveRoot(rootId, ctx)
      assertWritable(root)
      const src = resolveInRoot(root.abs, rel)
      const dest = resolveInRoot(root.abs, to)
      await copyEntry(src, dest, { overwrite: pickBool(c, "overwrite", body) })
      audit(user, c, { action: "fs.copy", root: rootId, path: rel, detail: { to }, ok: true })
      return c.json({ ok: true, path: relOf(root.abs, dest) })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.post("/api/v1/fs/delete", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const body = await jsonBody(c)
      const { ctx, user } = await ctxFor(c, body)
      const rootId = pickParam(c, "root", body)
      const paths = Array.isArray(body.paths) ? body.paths.map(String) : pickParam(c, "path", body) ? [pickParam(c, "path", body)] : []
      if (!paths.length) throw new FsError(400, "缺少 paths")
      const root = resolveRoot(rootId, ctx)
      assertWritable(root)
      const items = paths.map((p) => {
        const abs = resolveInRoot(root.abs, p)
        return { rootId, rootAbs: root.abs, abs, rel: relOf(root.abs, abs) }
      })
      const result = await deleteEntries(items)
      audit(user, c, { action: "fs.delete", root: rootId, detail: { paths, ...result }, ok: true })
      return c.json({ ok: true, ...result })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /* --------------------------- 变更监听（长轮询） --------------------------- */

  /**
   * 工作台变更监听：**前端长轮询 + 后端 fs.watch**（成本控制详见 core/fs/watch.ts）。
   *
   * 入参：`?root=&dirs=a,b&rev=N&wait=20&git=0`——`dirs` 是「当前关心的根内目录」
   * （`.` 代表根本身；越界/不存在的项静默丢弃）；`rev` 不传 = 只取一次基线（立即返回）；
   * `wait` 为无变化时的挂起秒数（0 = 即时检查，上限 30）；`git=0` 关掉 git 元数据监听。
   * 出参：`{rev, changed, paths, git}`——`paths` 为根内相对变更路径（null = 太多/未知 → 前端全量刷）。
   * 无变化时本端点不 spawn 任何进程、不做任何列举：只是挂着的 Promise。
   */
  app.get("/api/v1/fs/watch", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const { ctx } = await ctxFor(c)
      const rootId = c.req.query("root") || ""
      const root = resolveRoot(rootId, ctx)
      if (d.config.fsWatch === false) return c.json({ enabled: false, rev: 0, changed: false, paths: null, git: false })
      const sinceRaw = c.req.query("rev")
      const sinceRev = sinceRaw === undefined || sinceRaw === "" ? null : Number(sinceRaw)
      const wantWait = Math.min(Math.max(Number(c.req.query("wait")) || 0, 0), 30) * 1000
      // 背压：在途长轮询过多时退化为即时检查（宁可多几次往返，也不把 fd/连接吃光）
      const waitMs = watchWaiters >= MAX_WATCH_WAITERS ? 0 : wantWait
      const targets: WatchTarget[] = []
      for (const token of new Set((c.req.query("dirs") || "").split(",").map((s) => s.trim()).filter(Boolean).slice(0, MAX_WATCH_DIRS))) {
        // 线上记号：`.` / 空串 = 根本身（空串在逗号分隔里会被丢掉，故前端传 `.`，见 watch-core.wireDirs）
        const rel = token === "." ? "" : token
        try {
          const abs = resolveInRoot(root.abs, rel, { allowAbsolute: root.kind === "abs" })
          if (existsSync(abs)) targets.push({ path: abs })
        } catch {
          // 越界/软链逃逸的项直接丢弃：清单是前端推导出来的，不该因其中一项出错整轮失败
        }
      }
      if (c.req.query("git") !== "0") targets.push(...(await repoWatchTargets(root.abs)))
      const release = watchHub.subscribe(targets)
      try {
        // 首次（无 rev）：只给基线不挂起——前端拿到基线后立刻带 rev 再来一轮
        if (sinceRev === null) {
          const snap = watchHub.snapshot()
          return c.json({ enabled: true, changed: false, rev: snap.rev, git: snap.git, paths: null })
        }
        watchWaiters += 1
        let snap: { rev: number; paths: string[] | null; git: boolean }
        try {
          snap = watchHub.revision !== sinceRev ? watchHub.snapshot() : await watchHub.wait(sinceRev, waitMs)
        } finally {
          watchWaiters -= 1
        }
        const paths = snap.paths ? snap.paths.map((p) => relWatchPath(root.abs, p)).filter((p): p is string => p !== null) : null
        return c.json({ enabled: true, changed: snap.rev !== sinceRev, rev: snap.rev, git: snap.git, paths })
      } finally {
        release()
      }
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** 上传（multipart）：字段名为目标相对路径（或 `paths` JSON 数组按序对应）；单文件超限拒绝。 */
  app.post("/api/v1/fs/upload", async (c) => {
    const off = requireFsEnabled(c, d)
    if (off) return off
    try {
      const form = await c.req.formData()
      const rootId = String(form.get("root") ?? "")
      const sessionId = String(form.get("session") ?? "") || undefined
      const envRaw = form.get("env")
      const user = await userOf(c)
      const ctx = await buildRootContext(d, user, { sessionId, envInput: parseEnvInput(envRaw) })
      const root = resolveRoot(rootId, ctx)
      assertWritable(root)
      const overwrite = String(form.get("overwrite") ?? "") === "1" || String(form.get("overwrite") ?? "") === "true"
      const pathsField = form.get("paths")
      let declared: string[] | null = null
      if (typeof pathsField === "string" && pathsField.trim()) {
        try {
          const parsed = JSON.parse(pathsField)
          if (Array.isArray(parsed)) declared = parsed.map(String)
        } catch {
          declared = null
        }
      }
      const items: Array<{ path: string; data: Uint8Array }> = []
      let idx = 0
      for (const [key, value] of form.entries()) {
        if (key === "root" || key === "session" || key === "env" || key === "paths" || key === "overwrite") continue
        if (typeof value === "string") continue
        const file = value as File
        const rel = declared?.[idx] ?? (key && key !== "file" ? key : file.name)
        idx++
        if (file.size > d.config.fsMaxUpload) throw new FsError(413, `文件超过上传上限（${Math.round(d.config.fsMaxUpload / 1024 / 1024)}MB）: ${rel}`)
        items.push({ path: rel, data: new Uint8Array(await file.arrayBuffer()) })
      }
      if (!items.length) throw new FsError(400, "没有收到文件")
      const result = await uploadFiles(root.abs, items, { overwrite, maxBytes: d.config.fsMaxUpload })
      audit(user.id, c, { action: "fs.upload", root: rootId, detail: { saved: result.saved.length, skipped: result.skipped.length }, ok: true })
      return c.json({ ok: true, ...result })
    } catch (err) {
      return errorResponse(c, err)
    }
  })
}

/** 未使用：保持 FileRoot 类型导入（根清单结构与路由返回体对齐）。 */
export type { FileRoot }
