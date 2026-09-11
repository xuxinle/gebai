/** 文件工作台 · 根清单路由：`GET /api/v1/roots`（前端左栏根选择器与「打开文件夹」入口的数据源）。
 *  返回：会话工作区（最近 N 个）/ 预置项目 / 绑定项目 / 用户目录 / 额外白名单根（本地模式含盘符），
 *  每根附 `writable` 与 git 仓库标记（仓库/分支，供 UI 直接显示 VCS 状态而不额外请求）。 */
import type { RouteCtx } from "./context"
import { rootCatalog, resolveRoot, type FileRoot } from "../core/fs/roots"
import { GitService } from "../core/git/service"
import { buildRootContext, errorResponse, parseEnvInput, requireFsEnabled } from "./fs-shared"

/** 根清单附带的仓库信息（探测结果短缓存，避免每次刷新都 spawn git）。 */
const repoInfoCache = new Map<string, { ts: number; isRepo: boolean; branch?: string; root?: string }>()

async function repoInfo(git: GitService, dir: string): Promise<{ isRepo: boolean; branch?: string; root?: string }> {
  const hit = repoInfoCache.get(dir)
  if (hit && Date.now() - hit.ts < 5000) return hit
  let info: { isRepo: boolean; branch?: string; root?: string } = { isRepo: false }
  try {
    const root = await git.repoRoot(dir)
    if (root) {
      const status = await git.status(root)
      info = { isRepo: true, branch: status.branch ?? (status.detached ? "(detached)" : undefined), root }
    }
  } catch {
    /* 非仓库/探测失败：按非仓库处理 */
  }
  repoInfoCache.set(dir, { ts: Date.now(), ...info })
  return info
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
      // 附 git 仓库信息（非仓库根跳过探测成本：先判目录内是否有 .git 线索交给 GitService 缓存）
      const enriched: Array<FileRoot & { isRepo?: boolean; branch?: string; repoRoot?: string }> = []
      for (const r of roots) {
        if (!d.git) {
          enriched.push(r)
          continue
        }
        const info = await repoInfo(d.git, r.path)
        enriched.push({ ...r, isRepo: info.isRepo, branch: info.branch, repoRoot: info.root })
      }
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
