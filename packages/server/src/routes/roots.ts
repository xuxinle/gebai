/** 文件工作台 · 根清单路由：`GET /api/v1/roots`（前端左栏根选择器与「打开文件夹」入口的数据源）。
 *  返回：会话工作区（最近 N 个）/ 预置项目 / 绑定项目 / 用户目录 / 额外白名单根（本地模式含盘符），
 *  每根附 `writable` 与 git 仓库标记（仓库/分支，供 UI 直接显示 VCS 状态而不额外请求）。 */
import type { RouteCtx } from "./context"
import { existsSync } from "node:fs"
import { dirname, join, posix } from "node:path"
import { rootCatalog, resolveRoot, type FileRoot } from "../core/fs/roots"
import { GitService } from "../core/git/service"
import { buildRootContext, errorResponse, parseEnvInput, requireFsEnabled } from "./fs-shared"

/**
 * 仓库标记的探测结果缓存 TTL。
 *
 * 为什么放宽到 60s（原 5s）：本缓存只喂左栏的「仓库 / 分支」标签，**不是工作区精确状态**——
 * 变更清单、分支列表这些实时信息走 `/git/status`、`/git/branches` 各自取数，与本缓存无关。
 * 而根清单是首屏第一请求，每多跑一次探测都是用户白等；分支在此期间换掉的概率极低，
 * 陈旧的代价仅是标签慢一拍刷新（一次页面刷新/根切换即可纠正）。
 */
const REPO_INFO_TTL_MS = 60_000

/** 探测并发上限：够快，又不至于为几十个根一次 spawn 出几十个 git 进程抢 CPU。 */
const REPO_PROBE_CONCURRENCY = 8

/** 缓存条目上限（防御性：根随项目/会话增长，极端情况下也让它有界）。 */
const REPO_INFO_CACHE_MAX = 1000

/** 根清单附带的仓库信息。 */
interface RepoInfo {
  isRepo: boolean
  branch?: string
  root?: string
}

/** 探测结果缓存（跨请求复用，键为归一化后的目录路径）。 */
const repoInfoCache = new Map<string, { ts: number; info: RepoInfo }>()
/** 进行中的探测：同目录的并发调用共享同一 promise，不重复 spawn。 */
const repoInfoInflight = new Map<string, Promise<RepoInfo>>()

/** 缓存键归一：统一分隔符（Windows 路径可能 反斜杠/正斜杠 混用——git 输出正斜杠，
 *  根目录来自配置则多为反斜杠）并在 Windows 上折叠大小写（盘符大小写不敏感）。
 *  注意：POSIX 下反斜杠是合法文件名字符，**不得**替换，故只在 win32 归一。 */
function cacheKey(dir: string): string {
  if (process.platform !== "win32") return dir
  return dir.replace(/\\/g, "/").toLowerCase()
}

/** 路径深度（归一化后数 `/`）——用于「浅路径优先探测」。 */
function depthOf(key: string): number {
  let n = 0
  for (const ch of key) if (ch === "/") n++
  return n
}

/** 严格位于 root 之下（不含相等；两侧均已归一）。 */
function isUnder(pathKey: string, rootKey: string): boolean {
  if (pathKey === rootKey) return false
  return pathKey.startsWith(rootKey.endsWith("/") ? rootKey : `${rootKey}/`)
}

/**
 * 从 dir 逐级向上到 root（**不含 root 自身**）之间是否存在 `.git` 条目。
 *
 * 这是短路的**验证步骤**：只有「一路无 .git」才能断定 dir 属于 root 那个仓库。
 * 任一层存在 `.git`（目录或**文件**——子模块/worktree 的 `.git` 是文件）就说明
 * dir 处于一个嵌套仓库内，必须交回 git 独立探测，不能拿父仓库的答案充数。
 */
function hasNestedGit(dir: string, dirKey: string, rootKey: string): boolean {
  let cur = dir
  const steps = depthOf(dirKey) - depthOf(rootKey)
  for (let i = 0; i < steps; i++) {
    if (existsSync(join(cur, ".git"))) return true
    const parent = process.platform === "win32" ? posix.dirname(cur.replace(/\\/g, "/")) : dirname(cur)
    if (parent === cur || !parent) return false
    cur = parent
  }
  return false
}

/**
 * 目录是否落在「本次请求已确认的仓库根」之内：是则复用它的仓库信息，**不再 spawn git**。
 *
 * 为何安全：答案的权威来源仍是 git（先探测到的那个根是 `rev-parse` 给的），
 * 这里只做「该目录与那个根之间没有嵌套仓库」的**验证**（见 hasNestedGit），
 * 而「目录向上一直找不到 .git ⇒ 属于祖先那个仓库」正是 git 自己的查找规则。
 *
 * 取**最深的**匹配祖先（而非第一个命中）：已确认的根之间可能存在父子关系
 * （大仓库内又探测到一个小仓库），此时最深的那个才是 dir 的真正归属；
 * 若拿外层根做验证，路径上会撞到内层仓库的 `.git` 而白白回退去 spawn。
 */
function insideResolvedRepo(dir: string, dirKey: string, resolved: Map<string, RepoInfo>): RepoInfo | null {
  let bestKey = ""
  let best: RepoInfo | null = null
  for (const [rootKey, info] of resolved) {
    if (!isUnder(dirKey, rootKey)) continue
    if (rootKey.length <= bestKey.length) continue
    bestKey = rootKey
    best = info
  }
  if (!best) return null
  if (hasNestedGit(dir, dirKey, bestKey)) return null
  return best
}

/** 单目录探测（进行中的探测去重；任何异常按非仓库处理，与旧行为一致）。
 *  `key` 为归一化键（仅用于去重/缓存），`dir` 为原始路径（交给 git，不做任何改写）。 */
function probeOne(git: GitService, key: string, dir: string): Promise<RepoInfo> {
  const inflight = repoInfoInflight.get(key)
  if (inflight) return inflight
  const job = (async (): Promise<RepoInfo> => {
    try {
      const probe = await git.probeRepo(dir)
      if (!probe) return { isRepo: false }
      return { isRepo: true, branch: probe.branch ?? (probe.detached ? "(detached)" : undefined), root: probe.root }
    } catch {
      /* 非仓库 / 探测失败：按非仓库处理 */
      return { isRepo: false }
    } finally {
      repoInfoInflight.delete(key)
    }
  })()
  repoInfoInflight.set(key, job)
  return job
}

/** 缓存写入（带容量防御）。 */
function rememberRepoInfo(key: string, info: RepoInfo): void {
  if (repoInfoCache.size >= REPO_INFO_CACHE_MAX) repoInfoCache.clear()
  repoInfoCache.set(key, { ts: Date.now(), info })
}

/**
 * 批量取一批根目录的仓库信息。
 *
 * 四层省成本（旧实现是「逐根串行、每根跑一次全量 git status」，25 个根要 5s+）：
 * 1. **缓存**：命中直接返回（含非仓库结果），重复刷新页面近乎零开销；
 * 2. **浅路径优先 + 父仓库短路**：先探浅的目录，一旦摸清某个仓库根，
 *    落在它里面的其他根（会话工作区就在仓库目录内，是常态）直接复用，不再 spawn——
 *    实测单次 git 进程启动在 Windows 上就要 ~65ms，这是本路由的主要成本，
 *    而根清单里 20+ 个根同属一个仓库，短路把它们压成一次探测；
 * 3. **限并发**：成波探测（≤8 并发），把串行的 N 次 spawn 压成几波；
 * 4. **取消昂贵命令**：探测用 `GitService.probeRepo`（一次 `rev-parse` 同时取仓库根与分支名），
 *    不再走 `status()`（后者要跑全量 porcelain + stash list，是旧实现的主要耗时来源）。
 */
async function probeRoots(git: GitService, dirs: string[]): Promise<Map<string, RepoInfo>> {
  const out = new Map<string, RepoInfo>()
  /** 键 → 原始路径（键供比较/缓存，原始路径供 git 与文件系统调用，不改写传入值）。 */
  const byKey = new Map<string, string>()
  for (const dir of dirs) {
    const key = cacheKey(dir)
    if (!byKey.has(key)) byKey.set(key, dir)
  }
  const missing: string[] = []
  for (const key of byKey.keys()) {
    const hit = repoInfoCache.get(key)
    if (hit && Date.now() - hit.ts < REPO_INFO_TTL_MS) out.set(key, hit.info)
    else missing.push(key)
  }
  if (!missing.length) return out

  // 浅路径优先：让仓库根尽早进入已知集合，深处的根随后就能走短路
  missing.sort((a, b) => depthOf(a) - depthOf(b) || (a < b ? -1 : a > b ? 1 : 0))

  /** 本次请求已确认的仓库根（权威答案来自 git）。 */
  const resolved = new Map<string, RepoInfo>()
  for (let i = 0; i < missing.length; i += REPO_PROBE_CONCURRENCY) {
    const wave = missing.slice(i, i + REPO_PROBE_CONCURRENCY)
    const results = await Promise.all(
      wave.map(async (key) => {
        const dir = byKey.get(key) ?? key
        const shared = insideResolvedRepo(dir, key, resolved)
        return { key, info: shared ?? (await probeOne(git, key, dir)) }
      }),
    )
    for (const { key, info } of results) {
      rememberRepoInfo(key, info)
      out.set(key, info)
      if (info.isRepo && info.root) resolved.set(cacheKey(info.root), info)
    }
  }
  return out
}

export function registerRootRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  app.get("/api/v1/roots", async (c) => {
    const disabled = requireFsEnabled(c, d)
    if (disabled) return disabled
    try {
      const user = await userOf(c)
      const sessionId = c.req.query("session") || undefined
      const ctx = await buildRootContext(d, user, { sessionId, envInput: parseEnvInput(c.req.query("env")), withSessions: true })
      const roots = rootCatalog(ctx)
      // 附 git 仓库信息（批量探测：缓存 + 限并发 + 同仓库复用；未启用 git 时原样返回）
      const infos = d.git ? await probeRoots(d.git, roots.map((r) => r.path)) : null
      const enriched: Array<FileRoot & { isRepo?: boolean; branch?: string; repoRoot?: string }> = roots.map((r) => {
        if (!infos) return { ...r }
        const info = infos.get(cacheKey(r.path)) ?? { isRepo: false }
        return { ...r, isRepo: info.isRepo, branch: info.branch, repoRoot: info.root }
      })
      return c.json({
        enabled: true,
        fsEnabled: true,
        writable: ctx.writable,
        gitEnabled: !!d.git,
        gitWrite: !!d.git && d.config.gitWrite !== false,
        gitRemote: !!d.git && d.config.gitRemote !== false,
        sandboxed: ctx.sandboxed,
        showHidden: d.config.fsHidden === true,
        maxRead: d.config.fsMaxRead,
        maxWrite: d.config.fsMaxWrite,
        maxUpload: d.config.fsMaxUpload,
        roots: enriched,
      })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** 单根详情（深链/校验用）：`GET /api/v1/roots/:id`——id 形如 `proj:gebai`（含冒号，用 query 传更稳）。 */
  app.get("/api/v1/roots/resolve", async (c) => {
    const disabled = requireFsEnabled(c, d)
    if (disabled) return disabled
    try {
      const user = await userOf(c)
      const rootId = c.req.query("root") || ""
      const ctx = await buildRootContext(d, user, { sessionId: c.req.query("session") || undefined, envInput: parseEnvInput(c.req.query("env")) })
      const resolved = resolveRoot(rootId, ctx)
      return c.json({ id: resolved.id, kind: resolved.kind, path: resolved.abs, writable: resolved.writable })
    } catch (err) {
      return errorResponse(c, err)
    }
  })
}
