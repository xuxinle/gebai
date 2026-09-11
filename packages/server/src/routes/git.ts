/** 文件工作台 · Git 域路由（DESIGN「文件工作台·Git 图形化」）：以 `?root=` 定位仓库（root 见 core/fs/roots.ts），
 *  只读端点覆盖 status/diff/log/commit/branches/tags/remotes/blame/stash/conflicts，写端点覆盖
 *  暂存/提交/分支/检出/合并/变基/拣选/回滚/重置/stash/标签/远程/网络同步。
 *  写操作开关 GEBAI_GIT_WRITE、远程开关 GEBAI_GIT_REMOTE 在 GitService 内校验（此处兜底提示）。 */
import type { Context } from "hono"
import type { RouteCtx } from "./context"
import type { RootContext } from "../core/fs/roots"
import { FsError, resolveRoot } from "../core/fs/roots"
import { buildRootContext, errorResponse, parseEnvInput, pickBool, pickParam, requireFsEnabled, requireGit } from "./fs-shared"

export function registerGitRoutes(rc: RouteCtx): void {
  const { app, d } = rc
  const userOf = rc.userOf

  async function ctxFor(c: Context, body?: Record<string, unknown>): Promise<{ ctx: RootContext; userId: string }> {
    const user = await userOf(c)
    const sessionId = pickParam(c, "session", body) || undefined
    const envInput = c.req.query("env") ?? body?.env
    const ctx = await buildRootContext(d, user, { sessionId, envInput })
    return { ctx, userId: user.id }
  }

  /** 目标仓库目录（root 解析后的绝对路径；`path` 可选，指向仓库内子目录）。 */
  /**
   * 仓库定位：默认取根目录本身，可选 `dir` 指定根内的仓库子目录。
   * **不复用 `path`**——在 diff/compare/content/file-diff 等端点里 `path` 是路径过滤器
   * （git pathspec，如 `packages/server/src/app.ts`），拿它当目录会直接报「不是 Git 仓库」。
   */
  async function repoDir(c: Context, ctx: RootContext, body?: Record<string, unknown>): Promise<{ dir: string; rootId: string }> {
    const rootId = pickParam(c, "root", body)
    const root = resolveRoot(rootId, ctx)
    const sub = pickParam(c, "dir", body)
    const dir = sub ? resolveInRootSafe(root.abs, sub) : root.abs
    return { dir, rootId }
  }

  function resolveInRootSafe(rootAbs: string, rel: string): string {
    // 轻量校验（Git 操作目标是目录，越界一律拒绝；完整符号链接检查由 fs 层承担）
    const norm = rel.replace(/\\/g, "/")
    if (norm.includes("..") || norm.startsWith("/")) throw new FsError(400, `非法子路径: ${rel}`)
    return `${rootAbs.replace(/[\\/]+$/, "")}/${norm}`
  }

  const git = () => {
    if (!d.git) throw new FsError(503, "Git 能力未启用")
    return d.git
  }

  function audit(userId: string, c: Context, action: string, rootId: string, ok: boolean, detail?: Record<string, unknown>, error?: string): void {
    d.fsAudit?.record({
      ts: Date.now(),
      user: userId,
      source: "web",
      action,
      root: rootId,
      detail,
      ok,
      error,
      ip: d.config.trustProxy ? c.req.header("x-forwarded-for") : undefined,
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

  async function guard(c: Context): Promise<Response | null> {
    const off = requireFsEnabled(c, d)
    if (off) return off
    return requireGit(c, d)
  }

  /* ------------------------------ 只读 ------------------------------ */

  /** 仓库概览（根清单中所有仓库的聚合状态；多仓工作区一次拉齐）。 */
  app.get("/api/v1/git/repos", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const dirs: Array<{ rootId: string; dir: string }> = []
      for (const p of ctx.projects) dirs.push({ rootId: `proj:${p.name}`, dir: p.path })
      for (const b of ctx.binds) dirs.push({ rootId: `bind:${b.agent}`, dir: b.root })
      for (const e of ctx.extraRoots) dirs.push({ rootId: e.id, dir: e.path })
      const out = []
      for (const item of dirs) {
        const status = await git().status(item.dir)
        if (status.isRepo) out.push({ root: item.rootId, ...status })
      }
      return c.json({ repos: out })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/status", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir, rootId } = await repoDir(c, ctx)
      const status = await git().status(dir)
      return c.json({ root: rootId, ...status })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/diff", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      const result = await git().diff(dir, {
        path: c.req.query("path") || undefined,
        staged: pickBool(c, "staged"),
        from: c.req.query("from") || undefined,
        to: c.req.query("to") || undefined,
        mergeBase: pickBool(c, "mergeBase"),
        context: Number(c.req.query("context")) || 3,
        ignoreWhitespace: pickBool(c, "ignoreWhitespace"),
      })
      return c.json(result)
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /**
   * 任意两端对比总览（DESIGN「文件工作台·Git 图形化」）：A/B 两端各可为提交/分支/标签/工作树/暂存区。
   * 查询参数：`from`、`to`（端点，缺省工作树；`WORKTREE`/`INDEX` 为保留值）、
   * `mergeBase=1`（按共同祖先对比，分支对比默认语义）、`path`（限单文件）、`context`。
   */
  app.get("/api/v1/git/compare", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      const result = await git().compare(dir, {
        from: c.req.query("from") || undefined,
        to: c.req.query("to") || undefined,
        mergeBase: pickBool(c, "mergeBase"),
        path: c.req.query("path") || undefined,
        context: Number(c.req.query("context")) || 3,
        ignoreWhitespace: pickBool(c, "ignoreWhitespace"),
      })
      return c.json(result)
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** 引用清单（比较视图的端点选择器）：分支 + 标签 + HEAD + 最近提交，一次拿齐。 */
  app.get("/api/v1/git/refs", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      return c.json(await git().refs(dir, { recent: Number(c.req.query("recent")) || 40 }))
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /**
   * 某端点下的文件内容（并列 diff 取数）：`ref` 为 `WORKTREE` / `INDEX` / 任意 rev。
   * 前端拿两侧真实文本交给 Monaco 差异编辑器（带语法高亮的并列视图），而不是只看 unified patch。
   */
  app.get("/api/v1/git/content", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      const ref = c.req.query("ref") || "WORKTREE"
      const path = c.req.query("path") || ""
      if (!path) throw new FsError(400, "缺少 path")
      return c.json(await git().contentAt(dir, ref, path))
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/file-diff", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      const path = c.req.query("path") || ""
      if (!path) throw new FsError(400, "缺少 path")
      const result = await git().fileDiff(dir, path, {
        staged: pickBool(c, "staged"),
        from: c.req.query("from") || undefined,
        to: c.req.query("to") || undefined,
        mergeBase: pickBool(c, "mergeBase"),
        context: Number(c.req.query("context")) || 3,
        ignoreWhitespace: pickBool(c, "ignoreWhitespace"),
      })
      return c.json(result)
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/log", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      const result = await git().log(dir, {
        limit: Number(c.req.query("limit")) || 50,
        skip: Number(c.req.query("skip")) || 0,
        path: c.req.query("path") || undefined,
        ref: c.req.query("ref") || undefined,
        all: pickBool(c, "all"),
        since: c.req.query("since") || undefined,
        until: c.req.query("until") || undefined,
        author: c.req.query("author") || undefined,
        grep: c.req.query("grep") || undefined,
        firstParent: pickBool(c, "firstParent"),
      })
      return c.json(result)
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/commit", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      const hash = c.req.query("hash") || ""
      if (!hash) throw new FsError(400, "缺少 hash")
      return c.json(await git().commitDetail(dir, hash))
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/commit-file-diff", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      const hash = c.req.query("hash") || ""
      const path = c.req.query("path") || ""
      if (!hash || !path) throw new FsError(400, "缺少 hash 或 path")
      return c.json(await git().commitFileDiff(dir, hash, path, Number(c.req.query("context")) || 3))
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/file-history", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      const path = c.req.query("path") || ""
      if (!path) throw new FsError(400, "缺少 path")
      return c.json({ commits: await git().fileHistory(dir, path, Number(c.req.query("limit")) || 50) })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /** 读取某 ref 下文件内容（前端构造「旧版 vs 新版」并列 diff 的数据源）。 */
  app.get("/api/v1/git/show", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      const ref = c.req.query("ref") || "HEAD"
      const path = c.req.query("path") || ""
      if (!path) throw new FsError(400, "缺少 path")
      return c.json(await git().showFile(dir, ref, path))
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/branches", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      return c.json({ branches: await git().branches(dir) })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/tags", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      return c.json({ tags: await git().tags(dir) })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/remotes", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      return c.json({ remotes: await git().remotes(dir) })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/blame", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      const path = c.req.query("path") || ""
      if (!path) throw new FsError(400, "缺少 path")
      return c.json({ lines: await git().blame(dir, path, c.req.query("ref") || undefined) })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/stash", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      return c.json({ stashes: await git().stashList(dir) })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  app.get("/api/v1/git/conflicts", async (c) => {
    const g = await guard(c)
    if (g) return g
    try {
      const { ctx } = await ctxFor(c)
      const { dir } = await repoDir(c, ctx)
      const paths = c.req.queries("path") ?? undefined
      return c.json({ files: await git().conflicts(dir, paths) })
    } catch (err) {
      return errorResponse(c, err)
    }
  })

  /* ------------------------------ 写操作 ------------------------------ */

  /** 统一写操作包装：审计 + 错误映射（写开关在 GitService 内校验）。 */
  async function writeOp(c: Context, action: string, fn: (g: ReturnType<typeof git>, dir: string, rootId: string, body: Record<string, unknown>) => Promise<unknown>): Promise<Response> {
    const g = await guard(c)
    if (g) return g
    const body = await jsonBody(c)
    let rootId = ""
    let userId = ""
    try {
      const { ctx, userId: uid } = await ctxFor(c, body)
      userId = uid
      const t = await repoDir(c, ctx, body)
      rootId = t.rootId
      const result = await fn(git(), t.dir, t.rootId, body)
      audit(userId, c, action, rootId, true, { ...(result as Record<string, unknown>) })
      return c.json({ ok: true, ...(result as Record<string, unknown>) })
    } catch (err) {
      audit(userId, c, action, rootId, false, undefined, (err as Error).message)
      return errorResponse(c, err)
    }
  }

  app.post("/api/v1/git/stage", (c) =>
    writeOp(c, "git.stage", async (g, dir, _r, body) => {
      const paths = Array.isArray(body.paths) ? body.paths.map(String) : []
      await g.stage(dir, paths)
      return { staged: paths.length }
    }),
  )

  app.post("/api/v1/git/unstage", (c) =>
    writeOp(c, "git.unstage", async (g, dir, _r, body) => {
      const paths = Array.isArray(body.paths) ? body.paths.map(String) : []
      await g.unstage(dir, paths)
      return { unstaged: paths.length }
    }),
  )

  app.post("/api/v1/git/discard", (c) =>
    writeOp(c, "git.discard", async (g, dir, _r, body) => {
      const paths = Array.isArray(body.paths) ? body.paths.map(String) : []
      return g.discard(dir, paths, { backup: body.backup !== false })
    }),
  )

  app.post("/api/v1/git/commit", (c) =>
    writeOp(c, "git.commit", async (g, dir, _r, body) => {
      const message = String(body.message ?? "")
      const paths = Array.isArray(body.paths) ? body.paths.map(String) : undefined
      const result = await g.commit(dir, {
        message,
        paths,
        amend: body.amend === true,
        signoff: body.signoff === true,
        author: typeof body.author === "string" && body.author.trim() ? body.author.trim() : undefined,
        allowEmpty: body.allowEmpty === true,
      })
      let push: { ok: boolean; output: string } | undefined
      if (body.push === true) {
        push = await g.network(dir, "push", { remote: typeof body.remote === "string" ? body.remote : undefined, branch: typeof body.branch === "string" ? body.branch : undefined })
      }
      return { ...result, push }
    }),
  )

  app.post("/api/v1/git/branch", (c) =>
    writeOp(c, "git.branch", async (g, dir, _r, body) => {
      await g.branchOp(dir, {
        action: String(body.action ?? "create") as "create" | "checkout" | "delete" | "rename" | "track" | "upstream",
        name: String(body.name ?? ""),
        startPoint: typeof body.startPoint === "string" ? body.startPoint : undefined,
        newName: typeof body.newName === "string" ? body.newName : undefined,
        force: body.force === true,
        remote: body.remote === true,
      })
      return {}
    }),
  )

  app.post("/api/v1/git/checkout", (c) =>
    writeOp(c, "git.checkout", async (g, dir, _r, body) => {
      await g.checkoutRef(dir, String(body.ref ?? ""), { detach: body.detach === true })
      return {}
    }),
  )

  app.post("/api/v1/git/merge", (c) =>
    writeOp(c, "git.merge", async (g, dir, _r, body) => {
      return g.merge(dir, String(body.ref ?? ""), { noFf: body.noFf === true, squash: body.squash === true, message: typeof body.message === "string" ? body.message : undefined })
    }),
  )

  app.post("/api/v1/git/rebase", (c) =>
    writeOp(c, "git.rebase", async (g, dir, _r, body) => {
      return g.rebase(dir, String(body.ref ?? ""), { abort: body.abort === true, continue: body.continue === true, skip: body.skip === true })
    }),
  )

  app.post("/api/v1/git/cherry-pick", (c) =>
    writeOp(c, "git.cherry-pick", async (g, dir, _r, body) => {
      return g.cherryPick(dir, String(body.ref ?? ""), { abort: body.abort === true, continue: body.continue === true })
    }),
  )

  app.post("/api/v1/git/revert", (c) =>
    writeOp(c, "git.revert", async (g, dir, _r, body) => {
      return g.revert(dir, String(body.ref ?? ""), { abort: body.abort === true, continue: body.continue === true, noCommit: body.noCommit === true })
    }),
  )

  app.post("/api/v1/git/reset", (c) =>
    writeOp(c, "git.reset", async (g, dir, _r, body) => {
      const mode = (body.mode === "soft" || body.mode === "mixed" || body.mode === "hard" ? body.mode : "mixed") as "soft" | "mixed" | "hard"
      return g.reset(dir, String(body.ref ?? "HEAD"), mode, { backup: body.backup !== false })
    }),
  )

  app.post("/api/v1/git/stash", (c) =>
    writeOp(c, "git.stash", async (g, dir, _r, body) => {
      const action = (["push", "pop", "apply", "drop", "show", "clear"].includes(String(body.action)) ? body.action : "push") as "push" | "pop" | "apply" | "drop" | "show" | "clear"
      return g.stashOp(dir, action, {
        message: typeof body.message === "string" ? body.message : undefined,
        index: body.index === undefined ? undefined : Number(body.index),
        staged: body.staged === true,
        path: typeof body.path === "string" ? body.path : undefined,
      })
    }),
  )

  app.post("/api/v1/git/tag", (c) =>
    writeOp(c, "git.tag", async (g, dir, _r, body) => {
      await g.tagOp(dir, body.action === "delete" ? "delete" : "create", String(body.name ?? ""), {
        ref: typeof body.ref === "string" ? body.ref : undefined,
        message: typeof body.message === "string" ? body.message : undefined,
        force: body.force === true,
      })
      return {}
    }),
  )

  app.post("/api/v1/git/remote", (c) =>
    writeOp(c, "git.remote", async (g, dir, _r, body) => {
      const action = (["add", "remove", "set-url"].includes(String(body.action)) ? body.action : "add") as "add" | "remove" | "set-url"
      await g.remoteOp(dir, action, String(body.name ?? ""), typeof body.url === "string" ? body.url : undefined)
      return {}
    }),
  )

  app.post("/api/v1/git/fetch", (c) =>
    writeOp(c, "git.fetch", async (g, dir, _r, body) => {
      return g.network(dir, "fetch", { remote: typeof body.remote === "string" ? body.remote : undefined, prune: body.prune === true, tags: body.tags === true })
    }),
  )

  app.post("/api/v1/git/pull", (c) =>
    writeOp(c, "git.pull", async (g, dir, _r, body) => {
      return g.network(dir, "pull", { remote: typeof body.remote === "string" ? body.remote : undefined, branch: typeof body.branch === "string" ? body.branch : undefined, rebase: body.rebase === true, ffOnly: body.ffOnly === true })
    }),
  )

  app.post("/api/v1/git/push", (c) =>
    writeOp(c, "git.push", async (g, dir, _r, body) => {
      return g.network(dir, "push", {
        remote: typeof body.remote === "string" ? body.remote : undefined,
        branch: typeof body.branch === "string" ? body.branch : undefined,
        refspec: typeof body.refspec === "string" ? body.refspec : undefined,
        forceWithLease: body.forceWithLease === true,
        setUpstream: body.setUpstream === true,
        tags: body.tags === true,
      })
    }),
  )

  app.post("/api/v1/git/init", (c) =>
    writeOp(c, "git.init", async (g, dir, _r, body) => {
      await g.init(dir, { bare: body.bare === true, initialBranch: typeof body.initialBranch === "string" ? body.initialBranch : undefined })
      return {}
    }),
  )

  /** 忽略（追加 .gitignore 条目）。 */
  app.post("/api/v1/git/ignore", (c) =>
    writeOp(c, "git.ignore", async (g, dir, _r, body) => {
      const entries = Array.isArray(body.entries) ? body.entries.map(String) : typeof body.entry === "string" ? [body.entry] : []
      await g.addIgnore(dir, dir, entries)
      return { added: entries.length }
    }),
  )

  /** 冲突解决落盘（写文件 + git add；前端冲突解决器的提交动作）。 */
  app.post("/api/v1/git/conflicts/resolve", (c) =>
    writeOp(c, "git.conflicts.resolve", async (g, dir, _r, body) => {
      const items = Array.isArray(body.items) ? (body.items as Array<{ path?: unknown; content?: unknown }>) : []
      const root = await g.requireRepo(dir)
      const done: string[] = []
      for (const it of items) {
        const rel = String(it.path ?? "")
        if (!rel) continue
        if (typeof it.content === "string") {
          const abs = `${root.replace(/[\\/]+$/, "")}/${rel.replace(/\\/g, "/").replace(/^\/+/, "")}`
          await Bun.write(abs, it.content)
        }
        await g.stage(dir, [rel])
        done.push(rel)
      }
      const status = await g.status(dir)
      return { resolved: done, remaining: status.changes.filter((x) => x.conflicted).map((x) => x.path) }
    }),
  )

  /** 打开该文件的编辑器/终端不需要服务端；留占位便于后续「在文件管理器中显示」（桌面端能力）。 */
  void resolveInRootSafe
  void parseEnvInput
}
