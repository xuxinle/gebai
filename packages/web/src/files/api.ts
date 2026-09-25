/**
 * 文件工作台 · 服务端接口客户端（`/api/v1/{roots,fs,git}`，见 DESIGN「文件工作台」）。
 *
 * 约定：
 * - 所有请求自动带上当前根的会话上下文（`session`）与浏览器本地 env（`env`），
 *   使服务端的「预置项目」解析与主界面 / 会话看到的是同一份注册表；
 * - 非 2xx 统一抛出 `ApiError`（携带 status 与结构化 detail，如保存冲突的磁盘内容）；
 * - 下载/原样字节流不走 fetch（交给浏览器直连 URL，原生支持 Range 与断点续传）。
 */
import { wireDirs } from "./watch-core"
import { appBase } from "@gebai/sdk"


export interface RootInfo {
  id: string
  kind: "sess" | "proj" | "user" | "abs"
  name: string
  path: string
  description?: string
  writable: boolean
  sessionId?: string
  isRepo?: boolean
  branch?: string
  repoRoot?: string
}

export interface RootsResponse {
  enabled: boolean
  writable: boolean
  gitEnabled: boolean
  gitWrite: boolean
  gitRemote: boolean
  sandboxed: boolean
  showHidden: boolean
  maxRead: number
  maxWrite: number
  maxUpload: number
  roots: RootInfo[]
}

export interface DirEntry {
  name: string
  path: string
  type: "file" | "dir" | "symlink" | "other"
  size: number
  mtime: number
  kind?: string
  language?: string
  editable?: boolean
}

export interface ListResponse {
  root: string
  path: string
  entries: DirEntry[]
  truncated: boolean
  total: number
  showHidden: boolean
}

/** 变更监听（`GET /api/v1/fs/watch`，长轮询 + 后端 fs.watch）。
 *  `paths` 为根内相对路径；null = 变化太多/未知（前端做一次「可见部分全刷」）。 */
export interface WatchResponse {
  /** 服务端是否开了 fs.watch（GEBAI_FS_WATCH=false 时为 false，前端退化为纯轮询）。 */
  enabled: boolean
  changed: boolean
  rev: number
  paths: string[] | null
  /** 变化是否涉及 git 元数据（index / HEAD / refs）。 */
  git: boolean
}

export interface FileStat {
  path: string
  type: "file" | "dir" | "symlink" | "other"
  size: number
  mtime: number
  ctime: number
  mode: number
  etag: string
  kind: string
  language: string
  mime: string
  editable: boolean
  symlink: boolean
}

export interface ReadResponse {
  root: string
  path: string
  content: string
  encoding: string
  eol: string
  size: number
  mtime: number
  etag: string
  truncated: boolean
  binary: boolean
  kind: string
  language: string
}

export interface SearchHit {
  path: string
  line: number
  column: number
  lineText: string
  fileOnly?: boolean
  size: number
  mtime: number
}

export interface SearchResponse {
  root: string
  hits: SearchHit[]
  truncated: boolean
  engine: string
}

/** 文件索引（快速打开）：`files` 为 root 内相对路径，`truncated` 为真时名单不全（如实告知）。 */
export interface FileIndexResponse {
  root: string
  files: string[]
  truncated: boolean
  engine: "ripgrep" | "builtin"
}
export interface ArchiveEntry {
  name: string
  size: number
  compressedSize: number
  isDir: boolean
  mtime: number
  method: number
}

export interface GitChange {
  path: string
  origPath?: string
  index: string
  worktree: string
  untracked: boolean
  conflicted: boolean
  renamed: boolean
  staged: boolean
  unstaged: boolean
  kind: string
}

export interface GitStatusInfo {
  /** 请求时给的**根 id**（回显：同一个根的状态可被多个面板共用，用它判断"这份状态是谁的"） */
  root: string
  isRepo: boolean
  /** **仓库根的绝对路径**（子目录根只有它能往上定位仓库；非仓库时缺省） */
  repoRoot?: string
  branch?: string
  oid?: string
  upstream?: string
  ahead: number
  behind: number
  detached: boolean
  unborn: boolean
  changes: GitChange[]
  counts: { staged: number; unstaged: number; untracked: number; conflicted: number }
  stashCount: number
  operation?: string
}

export interface GitDiffLine {
  type: "context" | "add" | "del"
  text: string
  oldLine: number | null
  newLine: number | null
}

export interface GitDiffHunk {
  header: string
  oldStart: number
  oldLines: number
  newStart: number
  newLines: number
  lines: GitDiffLine[]
}

export interface GitFileDiff {
  path: string
  oldPath?: string
  binary: boolean
  hunks: GitDiffHunk[]
  additions: number
  deletions: number
  status?: string
  /** 逐行内容被省略（文件或整次 diff 超体量上限）——与「这个文件没改动」区分开 */
  truncated?: boolean
}

/** 部分暂存（逐块 / 逐行）的选中项：`lines` 缺省表示整块；下标与服务端 `GitFileDiff.hunks[].lines` 一致。 */
export interface HunkSelectionInput {
  hunk: number
  lines?: number[]
}

export interface GitCommitInfo {
  hash: string
  short: string
  parents: string[]
  author: string
  authorEmail: string
  authorTime: number
  committer: string
  commitTime: number
  refs: string[]
  subject: string
  body?: string
}

export interface GitBranchInfo {
  name: string
  remote: boolean
  current: boolean
  hash: string
  upstream?: string
  ahead?: number
  behind?: number
  subject?: string
  time?: number
}

export interface GitTagInfo {
  name: string
  hash: string
  time?: number
  subject?: string
}

export interface GitRemoteInfo {
  name: string
  fetchUrl: string
  pushUrl: string
}

export interface GitBlameLine {
  line: number
  hash: string
  author: string
  authorTime?: number
  time: number
  summary: string
  uncommitted: boolean
}

export class ApiError extends Error {
  constructor(
    public status: number,
    message: string,
    public detail?: unknown,
  ) {
    super(message)
    this.name = "ApiError"
  }
}

/** 浏览器本地 env（与服务端会话 env 合并的最小集合；键值均为字符串）。 */
export type EnvMap = Record<string, string>

export class FsApi {
  /** env 提供器（本地模式可空）：把浏览器本地 env 透传给服务端的项目解析。 */
  constructor(
    private env: () => EnvMap,
    private session: () => string | undefined,
  ) {}

  private withCtx(url: string, extra: Record<string, string | undefined> = {}): string {
    const u = new URL(`${appBase()}${url}`.replace(/\/{2,}/g, "/"), location.origin)
    const session = this.session()
    if (session) u.searchParams.set("session", session)
    const env = this.env()
    if (env && Object.keys(env).length) u.searchParams.set("env", JSON.stringify(env))
    for (const [k, v] of Object.entries(extra)) if (v !== undefined && v !== "") u.searchParams.set(k, v)
    return u.pathname + u.search
  }

  /** 直连 URL（下载 / 原样字节流 / iframe src）：不经 fetch，浏览器原生 Range 与缓存生效。 */
  url(endpoint: string, params: Record<string, string | number | boolean | undefined> = {}): string {
    const extra: Record<string, string | undefined> = {}
    for (const [k, v] of Object.entries(params)) {
      if (v === undefined || v === false) continue
      extra[k] = v === true ? "1" : String(v)
    }
    return this.withCtx(endpoint, extra)
  }

  private async req<T>(method: string, endpoint: string, opts: { params?: Record<string, string | number | boolean | undefined>; body?: unknown; raw?: boolean } = {}): Promise<T> {
    const extra: Record<string, string | undefined> = {}
    for (const [k, v] of Object.entries(opts.params ?? {})) {
      if (v === undefined || v === false) continue
      extra[k] = v === true ? "1" : String(v)
    }
    const url = this.withCtx(endpoint, extra)
    const init: RequestInit = { method, headers: {} }
    if (opts.body !== undefined) {
      init.body = typeof opts.body === "string" ? opts.body : JSON.stringify(opts.body)
      ;(init.headers as Record<string, string>)["Content-Type"] = "application/json"
    }
    const res = await fetch(url, init)
    if (opts.raw) {
      if (!res.ok) throw new ApiError(res.status, `请求失败（${res.status}）`)
      return res as unknown as T
    }
    const text = await res.text()
    let parsed: unknown = null
    try {
      parsed = text ? JSON.parse(text) : null
    } catch {
      parsed = null
    }
    if (!res.ok) {
      const msg = (parsed as { error?: string } | null)?.error ?? text.slice(0, 300) ?? `HTTP ${res.status}`
      throw new ApiError(res.status, msg, parsed)
    }
    return parsed as T
  }

  /* --------------------------- 根与文件 --------------------------- */

  roots(): Promise<RootsResponse> {
    return this.req<RootsResponse>("GET", "/api/v1/roots")
  }

  /**
   * 布尔开关按 **三态** 透传：`true` → `1`、`false` → `0`、不给（`undefined`）→ 不带该参数。
   * 不能直接塞布尔值——`req()` 会把 `false` 参数整条丢掉，「不显示隐藏文件」于是在请求里退化成
   * 「没给」、被服务端配置的默认值接管（菜单里点了关，树里照旧列着 `.env`/`.git`）。
   */
  private flag(v: boolean | undefined): "1" | "0" | undefined {
    return v === undefined ? undefined : v ? "1" : "0"
  }

  list(root: string, path: string, opts: { showHidden?: boolean; sort?: string; dirsFirst?: boolean; limit?: number } = {}): Promise<ListResponse> {
    return this.req<ListResponse>("GET", "/api/v1/fs/list", {
      params: { root, path, sort: opts.sort, limit: opts.limit, showHidden: this.flag(opts.showHidden), dirsFirst: this.flag(opts.dirsFirst) },
    })
  }

  tree(root: string, path: string, depth = 1, showHidden = false): Promise<{ children: TreeNode[] }> {
    return this.req<{ children: TreeNode[] }>("GET", "/api/v1/fs/tree", { params: { root, path, depth, showHidden: this.flag(showHidden) } })
  }

  /** 文件索引（快速打开用）：一次拿回 root 下全部文件相对路径，模糊匹配在前端做（逐键请求不现实）。 */
  files(root: string, opts: { showHidden?: boolean; limit?: number } = {}): Promise<FileIndexResponse> {
    return this.req<FileIndexResponse>("GET", "/api/v1/fs/files", { params: { root, limit: opts.limit, showHidden: this.flag(opts.showHidden) } })
  }

  /**
   * 变更监听长轮询：把「当前关心的目录」带上去，服务端有变化立即返回，没变化挂到 `wait` 秒再回心跳。
   * `rev` 不传 = 只取一次基线（立即返回，`changed:false`）。
   */
  async fsWatch(
    root: string,
    dirs: string[],
    opts: { rev?: number; wait?: number; git?: boolean; signal?: AbortSignal } = {},
  ): Promise<WatchResponse> {
    const url = this.withCtx("/api/v1/fs/watch", {
      root,
      // 线上记号：根目录（内部空串）会被逗号分隔丢掉，故与 watch-core.wireDirs 同口径传 "."
      dirs: wireDirs(dirs),
      rev: opts.rev === undefined ? undefined : String(opts.rev),
      wait: opts.wait === undefined ? undefined : String(opts.wait),
      git: opts.git === false ? "0" : undefined,
    })
    const res = await fetch(url, { signal: opts.signal })
    if (!res.ok) throw new ApiError(res.status, (await res.text()).slice(0, 200))
    return (await res.json()) as WatchResponse
  }

  stat(root: string, paths: string[]): Promise<{ items: FileStat[] }> {
    const u = this.withCtx("/api/v1/fs/stat", { root })
    const q = paths.map((p) => `path=${encodeURIComponent(p)}`).join("&")
    return fetch(`${u}${u.includes("?") ? "&" : "?"}${q}`).then(async (r) => {
      if (!r.ok) throw new ApiError(r.status, (await r.text()).slice(0, 200))
      return (await r.json()) as { items: FileStat[] }
    })
  }

  read(root: string, path: string, opts: { maxBytes?: number; forceText?: boolean } = {}): Promise<ReadResponse> {
    return this.req<ReadResponse>("GET", "/api/v1/fs/read", { params: { root, path, ...opts } })
  }

  search(root: string, query: string, opts: { mode?: "content" | "name"; glob?: string; ignoreCase?: boolean; regex?: boolean; maxResults?: number } = {}): Promise<SearchResponse> {
    return this.req<SearchResponse>("GET", "/api/v1/fs/search", { params: { root, q: query, ...opts } })
  }

  /** 解压 ZIP 到目标目录（写操作；目标为根内相对路径）。 */
  archiveExtract(root: string, path: string, to: string, entries?: string[]): Promise<{ extracted: number }> {
    return this.req("POST", "/api/v1/fs/archive/extract", { body: { root, path, to, entries } })
  }

  archive(root: string, path: string): Promise<{ entries: ArchiveEntry[]; size: number }> {
    return this.req("GET", "/api/v1/fs/archive", { params: { root, path } })
  }

  write(root: string, path: string, body: { content: string; encoding?: string; eol?: string; expectedEtag?: string; createDirs?: boolean }): Promise<{ ok: boolean; path: string; etag: string; size: number; mtime: number }> {
    return this.req("PUT", "/api/v1/fs/write", { params: { root, path }, body })
  }

  mkdir(root: string, path: string): Promise<{ ok: boolean; path: string }> {
    return this.req("POST", "/api/v1/fs/mkdir", { params: { root }, body: { path } })
  }

  rename(root: string, path: string, newName: string): Promise<{ ok: boolean; path: string }> {
    return this.req("POST", "/api/v1/fs/rename", { params: { root }, body: { path, newName } })
  }

  move(root: string, path: string, to: string, overwrite = false): Promise<{ ok: boolean; path: string }> {
    return this.req("POST", "/api/v1/fs/move", { params: { root }, body: { path, to, overwrite } })
  }

  copy(root: string, path: string, to: string, overwrite = false): Promise<{ ok: boolean; path: string }> {
    return this.req("POST", "/api/v1/fs/copy", { params: { root }, body: { path, to, overwrite } })
  }

  /** 删除（物理删除，不可恢复）。 */
  del(root: string, paths: string[]): Promise<{ ok: boolean; deleted: number }> {
    return this.req("POST", "/api/v1/fs/delete", { params: { root }, body: { paths } })
  }

  /** 上传（FormData）：`paths` 为与文件顺序一致的目标相对路径数组。 */
  async upload(root: string, files: Array<{ file: File; path: string }>, overwrite = false): Promise<{ ok: boolean; saved: Array<{ path: string; size: number }>; skipped: string[] }> {
    const form = new FormData()
    form.set("root", root)
    const session = this.session()
    if (session) form.set("session", session)
    const env = this.env()
    if (env && Object.keys(env).length) form.set("env", JSON.stringify(env))
    form.set("overwrite", overwrite ? "1" : "0")
    form.set("paths", JSON.stringify(files.map((f) => f.path)))
    for (const f of files) form.append(f.path, f.file, f.file.name)
    const res = await fetch(`${appBase()}/api/v1/fs/upload`, { method: "POST", body: form })
    const text = await res.text()
    const parsed = text ? JSON.parse(text) : null
    if (!res.ok) throw new ApiError(res.status, parsed?.error ?? `上传失败（${res.status}）`, parsed)
    return parsed
  }

  /* --------------------------- Git --------------------------- */

  gitStatus(root: string): Promise<GitStatusInfo> {
    return this.req<GitStatusInfo>("GET", "/api/v1/git/status", { params: { root } })
  }

  gitDiff(root: string, opts: { path?: string; staged?: boolean; from?: string; to?: string; mergeBase?: boolean; context?: number; ignoreWhitespace?: boolean }): Promise<{ files: GitFileDiff[]; raw: string; truncated?: boolean }> {
    return this.req("GET", "/api/v1/git/diff", { params: { root, ...opts } })
  }

  /** 任意两端对比总览（端点：`WORKTREE`/`INDEX`/任意 rev）。 */
  gitCompare(
    root: string,
    opts: { from?: string; to?: string; mergeBase?: boolean; path?: string; context?: number; ignoreWhitespace?: boolean } = {},
  ): Promise<{ files: GitFileDiff[]; additions: number; deletions: number; from: string; to: string; stats: string; mergeBaseOf?: string }> {
    return this.req("GET", "/api/v1/git/compare", { params: { root, ...opts } })
  }

  /** 引用清单（比较视图的端点选择器）：分支 + 标签 + HEAD + 最近提交。 */
  gitRefs(root: string, recent = 40): Promise<{
    head: { branch?: string; hash?: string; detached: boolean; unborn: boolean }
    user?: { name: string; email: string }
    branches: GitBranchInfo[]
    tags: GitTagInfo[]
    recent: Array<{ hash: string; short: string; subject: string; author: string; time: number; refs: string[] }>
  }> {
    return this.req("GET", "/api/v1/git/refs", { params: { root, recent } })
  }

  /** 某端点（`WORKTREE`/`INDEX`/rev）下的文件内容：并列 diff 的两侧真实文本。 */
  gitContent(root: string, ref: string, path: string): Promise<{ content: string; binary: boolean; missing: boolean; size: number; ref: string; tooLarge?: boolean }> {
    return this.req("GET", "/api/v1/git/content", { params: { root, ref, path } })
  }

  gitFileDiff(root: string, path: string, opts: { from?: string; to?: string; mergeBase?: boolean; context?: number } = {}): Promise<GitFileDiff> {
    return this.req<GitFileDiff>("GET", "/api/v1/git/file-diff", { params: { root, path, ...opts } })
  }

  gitLog(root: string, opts: { limit?: number; skip?: number; path?: string; ref?: string; all?: boolean; grep?: string; author?: string; since?: string; until?: string; grepRegex?: boolean; grepIgnoreCase?: boolean } = {}): Promise<{ commits: GitCommitInfo[]; hasMore: boolean }> {
    return this.req("GET", "/api/v1/git/log", { params: { root, ...opts } })
  }

  gitCommit(root: string, hash: string): Promise<{ commit: GitCommitInfo; files: GitFileDiff[]; stats: string; truncated?: boolean }> {
    return this.req("GET", "/api/v1/git/commit", { params: { root, hash } })
  }

  gitCommitFileDiff(root: string, hash: string, path: string): Promise<GitFileDiff> {
    return this.req<GitFileDiff>("GET", "/api/v1/git/commit-file-diff", { params: { root, hash, path } })
  }

  gitFileHistory(root: string, path: string, limit = 50): Promise<{ commits: GitCommitInfo[] }> {
    return this.req("GET", "/api/v1/git/file-history", { params: { root, path, limit } })
  }

  gitBranches(root: string): Promise<{ branches: GitBranchInfo[] }> {
    return this.req("GET", "/api/v1/git/branches", { params: { root } })
  }

  gitTags(root: string): Promise<{ tags: GitTagInfo[] }> {
    return this.req("GET", "/api/v1/git/tags", { params: { root } })
  }

  gitRemotes(root: string): Promise<{ remotes: GitRemoteInfo[] }> {
    return this.req("GET", "/api/v1/git/remotes", { params: { root } })
  }

  gitBlame(root: string, path: string, ref?: string): Promise<{ lines: GitBlameLine[] }> {
    return this.req("GET", "/api/v1/git/blame", { params: { root, path, ref } })
  }

  gitStash(root: string): Promise<{ stashes: Array<{ index: number; ref: string; message: string; time?: number }> }> {
    return this.req("GET", "/api/v1/git/stash", { params: { root } })
  }

  gitConflicts(root: string): Promise<{ files: Array<{ path: string; base: string | null; ours: string | null; theirs: string | null; current: string | null }> }> {
    return this.req("GET", "/api/v1/git/conflicts", { params: { root } })
  }

  /** 某 ref 下文件内容（保留接口：等价于 gitContent，历史调用方兼容）。 */
  gitShow(root: string, ref: string, path: string): Promise<{ content: string; binary: boolean; missing: boolean; size: number }> {
    return this.gitContent(root, ref, path)
  }

  gitOp<T = unknown>(action: string, root: string, body: Record<string, unknown> = {}): Promise<T & { ok?: boolean; output?: string }> {
    return this.req<T & { ok?: boolean; output?: string }>("POST", `/api/v1/git/${action}`, { params: { root }, body })
  }

  /* ------------------------------ 部分暂存（逐块 / 逐行） ------------------------------
   * `selections` 的 `hunk` 是差异里的块序号、`lines` 是块内**改动行**下标，
   * 与服务端 buildPartialPatch 共用同一套行序约定（即 GitFileDiff.hunks[].lines 的下标）。
   * ---------------------------------------------------------------------------------- */

  gitStageHunks(root: string, path: string, selections: HunkSelectionInput[]): Promise<{ hunks: number[]; changed: number }> {
    return this.req("POST", "/api/v1/git/stage-hunks", { params: { root }, body: { path, selections } })
  }

  gitUnstageHunks(root: string, path: string, selections: HunkSelectionInput[]): Promise<{ hunks: number[]; changed: number }> {
    return this.req("POST", "/api/v1/git/unstage-hunks", { params: { root }, body: { path, selections } })
  }

  gitDiscardHunks(
    root: string,
    path: string,
    selections: HunkSelectionInput[],
    backup = true,
  ): Promise<{ hunks: number[]; changed: number; backupRef?: string }> {
    return this.req("POST", "/api/v1/git/discard-hunks", { params: { root }, body: { path, selections, backup } })
  }

  /** 把一份**任意内容**写入暂存区（三向暂存编辑器的保存动作；不碰工作区与 HEAD）。 */
  gitStageContent(root: string, path: string, content: string): Promise<{ hash: string }> {
    return this.req("POST", "/api/v1/git/stage-content", { params: { root }, body: { path, content } })
  }

  /* ------------------------------ 编辑历史（交互式变基） ------------------------------ */

  /** 改写历史：`steps` 按**应用顺序（旧 → 新）**给出。 */
  gitHistoryEdit(
    root: string,
    base: string,
    steps: Array<{ commit: string; action: string; message?: string }>,
  ): Promise<{ ok: boolean; applied: number; conflicts: string[]; output: string; halted: null | "conflict" | "edit"; backupBranch?: string; branch?: string; head?: string }> {
    return this.req("POST", "/api/v1/git/history-edit", { params: { root }, body: { base, steps } })
  }

  /** 未完成的编辑历史（横幅显示「继续 / 中止」的依据）。 */
  gitHistoryEditPlan(root: string): Promise<{ plan: { branch: string; originalHead: string; index: number; steps: unknown[] } | null }> {
    return this.req("GET", "/api/v1/git/history-edit", { params: { root } })
  }

  gitHistoryEditContinue(root: string): Promise<{ ok: boolean; conflicts: string[]; output: string; halted: null | "conflict" | "edit" }> {
    return this.req("POST", "/api/v1/git/history-edit/continue", { params: { root }, body: {} })
  }

  gitHistoryEditAbort(root: string): Promise<{ branch: string; restored: string; backupBranch?: string }> {
    return this.req("POST", "/api/v1/git/history-edit/abort", { params: { root }, body: {} })
  }
}

export interface TreeNode {
  name: string
  path: string
  type: "file" | "dir" | "symlink" | "other"
  size: number
  mtime: number
  children?: TreeNode[]
  hasChildren?: boolean
  truncated?: boolean
}
