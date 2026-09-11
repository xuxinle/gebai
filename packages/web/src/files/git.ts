/**
 * 文件工作台 · Git 面板（右栏，IDEA 式 VCS 工具窗）：变更 / 日志 / 分支 / 标签 / 暂存 / 远程。
 *
 * 交互原则（对齐 IDEA 的 VCS 工具窗习惯）：
 * - 变更列表按「冲突 / 暂存 / 未暂存 / 未跟踪」分组，行内按钮承担 stage/unstage/revert，双击开差异；
 * - 提交区在最下方常驻（消息框 + 提交 / 提交并推送），提交前可勾选「仅提交已暂存」；
 * - 日志分页加载，提交详情列出变更文件（点开 Monaco 并列 diff），支持路径过滤与文件历史；
 * - 破坏性操作（丢弃改动 / 重置 / 强制推送）统一二次确认，并在有备份能力时提示（丢弃自动 stash 备份、
 *   hard reset 自动建备份分支——见服务端 GitService）；
 * - 多步操作（merge/rebase/cherry-pick）冲突时面板顶部出现「继续 / 中止 / 跳过」条，避免用户卡死。
 */
import type { FsApi, GitBranchInfo, GitChange, GitCommitInfo, GitFileDiff, GitStatusInfo } from "./api"
import { h, icon, showMenu, toast, confirmDialog, promptDialog, clear, timeAgo, formatTime } from "./ui"
import { createDiffEditor } from "./editor"

export type GitView = "changes" | "log" | "branches" | "tags" | "stash" | "remotes"

/** 差异视图规格（主区域标签页按此解析出「旧版/新版」两侧文本）。
 *  端点模型与服务端 `diffArgs` 一致：`WORKTREE` / `INDEX` / 任意 rev；
 *  `range` 覆盖「任意两个提交」「提交 ↔ 工作树」「提交 ↔ 暂存区」「分支 ↔ 分支（共同祖先）」。 */
export interface DiffSpec {
  title: string
  root: string
  path: string
  /** 差异来源 */
  source:
    | { type: "worktree"; staged: boolean }
    | { type: "commit"; hash: string }
    | { type: "range"; from: string; to: string; mergeBase?: boolean; label?: string }
  /** 服务端已解析的结构化差异（拿不到两侧文本时回退渲染） */
  fallback?: GitFileDiff
}

export const WORKTREE_REF = "WORKTREE"
export const INDEX_REF = "INDEX"

export interface GitHooks {
  api: FsApi
  /** 当前根（根切换时面板内容随之切换） */
  root: () => string
  /** 当前根在仓库内的相对前缀（root 指向仓库子目录时不为空） */
  repoPrefix: () => string
  /** 状态快照（由 main.ts 统一拉取，面板只读用） */
  status: () => GitStatusInfo | null
  /** 主动刷新状态（写操作后调用） */
  refreshStatus: () => Promise<GitStatusInfo | null>
  /** 在主区域打开差异标签 */
  openDiff: (spec: DiffSpec) => void
  /** 打开文件（可定位行） */
  openFile: (root: string, path: string, line?: number) => void
  /** 打开「比较」标签（任意两端对比：提交↔提交、提交↔工作区/暂存区、分支↔分支；端点由比较视图自选） */
  openCompare: (init?: { from?: string; to?: string; path?: string; mergeBase?: boolean }) => void
  /** 是否可以写（GEBAI_FS_WRITE / GEBAI_GIT_WRITE） */
  writable: () => boolean
  /** 远程操作是否可用（GEBAI_GIT_REMOTE） */
  remoteEnabled: () => boolean
  /** 文件系统变更后通知（重命名/删除等需刷新树） */
  onFsChanged: () => void
}

export interface GitPanel {
  el: HTMLElement
  refresh: () => Promise<void>
  show: (view: GitView) => void
  view: () => GitView
}

const VIEW_LABEL: Record<GitView, string> = {
  changes: "变更",
  log: "日志",
  branches: "分支",
  tags: "标签",
  stash: "暂存",
  remotes: "远程",
}

export function createGitPanel(hooks: GitHooks): GitPanel {
  let view: GitView = "changes"
  let logItems: GitCommitInfo[] = []
  let logHasMore = true
  let logLoading = false
  let logFilterPath = ""
  let logFilterText = ""
  let branches: GitBranchInfo[] = []
  let localTags: Array<{ name: string; hash: string; time?: number; subject?: string }> = []
  let stashes: Array<{ index: number; ref: string; message: string; time?: number }> = []
  let remotes: Array<{ name: string; fetchUrl: string; pushUrl: string }> = []
  let commitMessage = ""
  let commitAmend = false
  let committing = false
  /** 变更列表是否显示整仓库（root 只是仓库子目录时默认只看当前目录，见 renderChanges）。 */
  let showWholeRepo = false

  const tabsHost = h("div", { class: "fw-git-tabs" })
  const body = h("div", { class: "fw-git-body" })
  const el = h("div", { class: "fw-git-panel" }, [tabsHost, body])

  /** 当前目录范围内的变更（root 为仓库子目录时；showWholeRepo 开启后为整仓库）。 */
  function scopedChanges(): { changes: GitChange[]; staged: number; unstaged: number; untracked: number; conflicted: number } {
    const s = hooks.status()
    const prefix = hooks.repoPrefix()
    const all = s?.changes ?? []
    const changes = prefix && !showWholeRepo ? all.filter((c) => c.path === prefix || c.path.startsWith(`${prefix}/`)) : all
    return {
      changes,
      staged: changes.filter((c) => c.staged).length,
      unstaged: changes.filter((c) => c.unstaged).length,
      untracked: changes.filter((c) => c.untracked).length,
      conflicted: changes.filter((c) => c.conflicted).length,
    }
  }

  function renderTabs(): void {
    clear(tabsHost)
    const s = hooks.status()
    const scoped = scopedChanges()
    const dirty = scoped.staged + scoped.unstaged + scoped.untracked + scoped.conflicted
    for (const v of Object.keys(VIEW_LABEL) as GitView[]) {
      const badge =
        v === "changes" && dirty > 0
          ? h("span", { class: "fw-badge", text: String(dirty) })
          : v === "stash" && s?.stashCount
            ? h("span", { class: "fw-badge", text: String(s.stashCount) })
            : null
      const btn = h("button", { class: `fw-git-tab${v === view ? " active" : ""}` }, [h("span", { text: VIEW_LABEL[v] }), badge])
      btn.onclick = () => show(v)
      tabsHost.appendChild(btn)
    }
  }

  function show(v: GitView): void {
    view = v
    void refresh()
  }

  /** 统一写操作执行：捕获错误 → toast；成功后刷新状态与视图。 */
  async function op(action: string, body: Record<string, unknown>, okMsg?: string, opts: { silent?: boolean } = {}): Promise<Record<string, unknown> | null> {
    try {
      const res = (await hooks.api.gitOp(action, hooks.root(), body)) as Record<string, unknown>
      const output = typeof res.output === "string" ? res.output : ""
      if (!opts.silent) {
        if (res.ok === false) {
          toast(`${okMsg ? `${okMsg}失败：` : ""}${output || "操作未成功（可能存在冲突）"}`, "error", 6000)
        } else if (output) toast(output.split("\n").slice(0, 3).join("\n"), "info", 4000)
        else if (okMsg) toast(okMsg, "success")
      }
      await hooks.refreshStatus()
      await refresh()
      return res
    } catch (err) {
      toast(`${okMsg ? `${okMsg}失败：` : "操作失败："}${(err as Error).message}`, "error", 6000)
      return null
    }
  }

  /* ------------------------------ 变更视图 ------------------------------ */

  function changeRow(c: GitChange, group: "staged" | "unstaged" | "untracked" | "conflicted"): HTMLElement {
    const name = c.path.split("/").pop() ?? c.path
    const dir = c.path.includes("/") ? c.path.slice(0, c.path.lastIndexOf("/")) : ""
    const mark = c.conflicted ? "!" : c.untracked ? "U" : c.kind === "added" ? "A" : c.kind === "deleted" ? "D" : c.kind === "renamed" ? "R" : c.kind === "modified" ? "M" : "?"
    const cls = c.conflicted ? "conflict" : mark
    const row = h("div", { class: `fw-change-row ${cls}` }, [
      h("span", { class: `fw-change-mark ${cls}`, text: mark, title: c.kind }),
      h("span", { class: "fw-change-name", title: `${c.path}${c.origPath ? ` （原 ${c.origPath}）` : ""}` }, [
        h("span", { text: name }),
        dir ? h("span", { class: "fw-change-dir", text: `  ${dir}` }) : null,
        c.origPath ? h("span", { class: "fw-change-orig", text: ` ← ${c.origPath}` }) : null,
      ]),
      h("span", { class: "fw-change-actions" }, [
        group === "unstaged" || group === "untracked" || group === "conflicted"
          ? btnIcon("check", "暂存", () => void op("stage", { paths: [c.path] }, "已暂存", { silent: true }))
          : null,
        group === "staged" ? btnIcon("undo", "取消暂存", () => void op("unstage", { paths: [c.path] }, undefined, { silent: true })) : null,
        group !== "untracked"
          ? btnIcon("diff", "查看差异", () =>
              hooks.openDiff({
                title: `${name}${group === "staged" ? "（已暂存）" : ""}`,
                root: hooks.root(),
                // path 用仓库相对（git 语义）：服务端 contentAt 以仓库根为基准取两侧内容
                path: c.path,
                source: { type: "worktree", staged: group === "staged" },
              }),
            )
          : null,
        group !== "staged"
          ? btnIcon("undo", "丢弃改动（自动 stash 备份）", () => void discard(c.path), "danger")
          : null,
        btnIcon("diff", "与 HEAD 比较", () =>
          hooks.openCompare({ from: "HEAD", to: "WORKTREE", path: c.path }),
        ),
        btnIcon("history", "文件历史（Git log --follow）", () => {
          logFilterPath = c.path
          logItems = []
          logHasMore = true
          show("log")
        }),
      ]),
    ])
    row.ondblclick = () => hooks.openFile(hooks.root(), prefixPath(c.path))
    row.oncontextmenu = (e) => {
      e.preventDefault()
      showMenu(e.clientX, e.clientY, [
        { label: "打开文件", icon: "file", onClick: () => hooks.openFile(hooks.root(), prefixPath(c.path)) },
        {
          label: "查看差异",
          icon: "diff",
          onClick: () =>
            hooks.openDiff({
              title: name,
              root: hooks.root(),
              path: c.path,
              source: { type: "worktree", staged: group === "staged" },
            }),
        },
        { separator: true },
        { label: "暂存", icon: "check", disabled: group === "staged", onClick: () => void op("stage", { paths: [c.path] }, "已暂存", { silent: true }) },
        { label: "取消暂存", icon: "undo", disabled: group !== "staged", onClick: () => void op("unstage", { paths: [c.path] }, undefined, { silent: true }) },
        { label: "丢弃改动", icon: "trash", danger: true, disabled: group === "staged", onClick: () => void discard(c.path) },
        { separator: true },
        { label: "复制路径", icon: "copy", onClick: () => void navigator.clipboard.writeText(c.path) },
        { label: "加入 .gitignore", icon: "git", onClick: () => void op("ignore", { entries: [c.untracked ? c.path : `/${c.path}`] }, "已忽略") },
      ])
    }
    return row
  }

  /** Git 状态里的路径是仓库相对；转成当前根内的相对路径（root 可能指向仓库子目录）。 */
  function prefixPath(repoRel: string): string {
    const prefix = hooks.repoPrefix()
    if (!prefix) return repoRel
    return repoRel.startsWith(`${prefix}/`) ? repoRel.slice(prefix.length + 1) : repoRel
  }

  function btnIcon(name: string, title: string, onClick: () => void, cls = ""): HTMLButtonElement {
    const b = h("button", { class: `fw-icon-btn sm ${cls}`, title })
    b.appendChild(icon(name, 13))
    b.onclick = (e) => {
      e.stopPropagation()
      onClick()
    }
    return b
  }

  async function discard(path: string): Promise<void> {
    const ok = await confirmDialog({
      title: "丢弃改动",
      message: `确定丢弃「${path}」的工作区改动？`,
      hint: "操作前会自动 stash 备份（可在「暂存」视图恢复），但请确认不再需要这些修改。",
      okText: "丢弃",
      danger: true,
    })
    if (!ok) return
    const res = await op("discard", { paths: [path], backup: true }, "已丢弃改动")
    if (res?.backupRef) toast(`已备份到 ${res.backupRef}`, "info")
    hooks.onFsChanged()
  }

  function renderChanges(): void {
    const s = hooks.status()
    if (!s?.isRepo) {
      body.replaceChildren(renderNotRepo())
      return
    }
    // 路径过滤：root 可能只是仓库的子目录（如会话工作区）——默认只显示当前目录内的变更
    // （IDEA 的 VCS 工具窗也只看项目根范围内）；可一键切到整仓库。
    const prefix = hooks.repoPrefix()
    const scoped = scopedChanges().changes
    const groups: Array<{ key: "conflicted" | "staged" | "unstaged" | "untracked"; label: string; items: GitChange[] }> = [
      { key: "conflicted", label: "冲突", items: scoped.filter((c) => c.conflicted) },
      { key: "staged", label: "已暂存", items: scoped.filter((c) => c.staged) },
      { key: "unstaged", label: "未暂存", items: scoped.filter((c) => c.unstaged && !c.conflicted) },
      { key: "untracked", label: "未跟踪", items: scoped.filter((c) => c.untracked) },
    ]
    const list = h("div", { class: "fw-git-list" })
    if (prefix) {
      const chip = h("button", { class: "fw-chip fw-scope-chip", title: prefix ? `当前限定：${prefix}` : "整仓库" }, [
        icon(showWholeRepo ? "git" : "folder", 12),
        h("span", { text: showWholeRepo ? "整仓库（点击仅看当前目录）" : `仅当前目录：${prefix.split("/").pop() ?? prefix}` }),
      ])
      chip.onclick = () => {
        showWholeRepo = !showWholeRepo
        renderChanges()
      }
      list.appendChild(chip)
    }
    for (const g of groups) {
      if (!g.items.length) continue
      const groupEl = h("div", { class: "fw-change-group" })
      const head = h("div", { class: "fw-change-group-head" }, [
        icon("chevronDown", 12),
        h("span", { text: `${g.label}（${g.items.length}）` }),
        h("span", { class: "fw-grow" }),
        g.key !== "staged" && g.key !== "conflicted"
          ? btnIcon("check", "全部暂存", () => void op("stage", { paths: g.items.map((c) => c.path) }, "已全部暂存", { silent: true }))
          : null,
        g.key === "staged" ? btnIcon("undo", "全部取消暂存", () => void op("unstage", { paths: g.items.map((c) => c.path) }, undefined, { silent: true })) : null,
      ])
      const inner = h("div", { class: "fw-change-group-body" }, g.items.map((c) => changeRow(c, g.key)))
      head.onclick = (e) => {
        if ((e.target as HTMLElement).closest("button")) return
        inner.classList.toggle("collapsed")
      }
      groupEl.append(head, inner)
      list.appendChild(groupEl)
    }
    if (!s.changes.length) list.appendChild(h("div", { class: "fw-empty", text: "工作区干净，没有未提交的变更" }))

    // 多步操作态：继续 / 中止 / 跳过
    if (s.operation) {
      const banner = h("div", { class: "fw-git-banner warn" }, [
        icon("warning"),
        h("span", { text: `${s.operation} 进行中${s.counts.conflicted ? `（${s.counts.conflicted} 个冲突待解决）` : ""}` }),
        h("span", { class: "fw-grow" }),
        s.counts.conflicted
          ? (() => {
              const b = h("button", { class: "fw-btn sm", text: "打开冲突解决" })
              b.onclick = () => void openConflicts()
              return b
            })()
          : null,
        (() => {
          const b = h("button", { class: "fw-btn sm primary", text: "继续" })
          b.onclick = () => void op(operationAction(s.operation, "continue"), {}, "已继续")
          return b
        })(),
        (() => {
          const b = h("button", { class: "fw-btn sm", text: "跳过" })
          b.disabled = s.operation !== "rebase"
          b.onclick = () => void op("rebase", { skip: true }, "已跳过")
          return b
        })(),
        (() => {
          const b = h("button", { class: "fw-btn sm danger", text: "中止" })
          b.onclick = () => void op(operationAction(s.operation, "abort"), {}, "已中止")
          return b
        })(),
      ])
      list.prepend(banner)
    }

    body.replaceChildren(list, renderCommitBox())
  }

  /** 进行中的多步操作 → 对应的继续/中止动作名（统一走同一组端点）。 */
  function operationAction(opName: string | undefined, _mode: "abort" | "continue"): string {
    if (opName === "rebase" || opName === "am") return "rebase"
    if (opName === "cherry-pick") return "cherry-pick"
    if (opName === "revert") return "revert"
    return "merge"
  }

  function renderCommitBox(): HTMLElement {
    const area = h("textarea", { class: "fw-commit-msg", placeholder: "提交信息（Ctrl+Enter 提交）", rows: 3 })
    area.value = commitMessage
    area.oninput = () => {
      commitMessage = area.value
    }
    const stagedCount = scopedChanges().staged
    const amendToggle = h("label", { class: "fw-check" }, [h("input", { type: "checkbox" })])
    ;(amendToggle.querySelector("input") as HTMLInputElement).checked = commitAmend
    ;(amendToggle.querySelector("input") as HTMLInputElement).onchange = (e) => {
      commitAmend = (e.target as HTMLInputElement).checked
    }
    amendToggle.append(h("span", { text: "修补上次提交" }))
    const signoff = h("label", { class: "fw-check" }, [h("input", { type: "checkbox" }), h("span", { text: "署名" })])

    const doCommit = async (push: boolean) => {
      if (committing) return
      const msg = commitMessage.trim()
      if (!msg && !commitAmend) {
        toast("请填写提交信息", "error")
        return
      }
      committing = true
      commitBtn.setAttribute("disabled", "")
      try {
        const res = await hooks.api.gitOp<{ hash?: string; subject?: string }>("commit", hooks.root(), {
          message: msg,
          amend: commitAmend,
          signoff: (signoff.querySelector("input") as HTMLInputElement).checked,
          push,
          setUpstream: push,
        })
        if ((res as { push?: { ok?: boolean; output?: string } }).push && (res as { push: { ok?: boolean; output?: string } }).push.ok === false) {
          toast(`提交成功但推送失败：${(res as { push: { output: string } }).push.output?.slice(0, 300)}`, "error", 8000)
        } else {
          toast(push ? "已提交并推送" : "已提交", "success")
        }
        commitMessage = ""
        commitAmend = false
        await hooks.refreshStatus()
        await refresh()
        hooks.onFsChanged()
      } catch (err) {
        toast(`提交失败：${(err as Error).message}`, "error", 8000)
      } finally {
        committing = false
        commitBtn.removeAttribute("disabled")
      }
    }

    const commitBtn = h("button", { class: "fw-btn primary", title: stagedCount ? "" : "没有已暂存的变更（将提交工作区全部改动）" }, [icon("check"), h("span", { text: commitAmend ? "修补提交" : "提交" })])
    commitBtn.onclick = () => void doCommit(false)
    const pushBtn = h("button", { class: "fw-btn", title: "提交并推送当前分支" }, [icon("upload"), h("span", { text: "提交并推送" })])
    pushBtn.onclick = () => void doCommit(true)
    pushBtn.disabled = !hooks.remoteEnabled()

    area.onkeydown = (e) => {
      if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
        e.preventDefault()
        void doCommit(false)
      }
    }

    return h("div", { class: "fw-commit-box" }, [
      area,
      h("div", { class: "fw-commit-actions" }, [amendToggle, signoff, h("span", { class: "fw-grow" }), h("span", { class: "fw-hint", text: stagedCount ? `${stagedCount} 个文件已暂存` : "未暂存（提交将包含全部改动）" }), pushBtn, commitBtn]),
    ])
  }

  function renderNotRepo(): HTMLElement {
    const box = h("div", { class: "fw-placeholder" }, [
      h("div", { class: "fw-placeholder-msg", text: "当前根不是 Git 仓库" }),
      h("div", { class: "fw-placeholder-hint", text: "可以在此初始化仓库，或切换到含 .git 的项目根。" }),
    ])
    if (hooks.writable()) {
      const b = h("button", { class: "fw-btn primary" }, [icon("git"), h("span", { text: "初始化仓库（git init）" })])
      b.onclick = () =>
        void (async () => {
          const branch = await promptDialog({ title: "初始化 Git 仓库", label: "默认分支名", value: "main", okText: "初始化" })
          if (branch === null) return
          await op("init", { initialBranch: branch || "main" }, "已初始化仓库")
        })()
      box.appendChild(h("div", { class: "fw-placeholder-actions" }, [b]))
    }
    return box
  }

  /* ------------------------------ 冲突解决 ------------------------------ */

  async function openConflicts(): Promise<void> {
    try {
      const res = await hooks.api.gitConflicts(hooks.root())
      if (!res.files.length) {
        toast("没有冲突文件", "info")
        return
      }
      const first = res.files[0]
      hooks.openDiff({
        title: `冲突：${first.path.split("/").pop()}`,
        root: hooks.root(),
        path: first.path,
        source: { type: "worktree", staged: false },
      })
      toast(`共 ${res.files.length} 个冲突文件，其余可在变更列表逐个打开`, "info", 5000)
    } catch (err) {
      toast(`读取冲突失败：${(err as Error).message}`, "error")
    }
  }

  /* ------------------------------ 日志视图 ------------------------------ */

  async function loadLog(reset = false): Promise<void> {
    if (logLoading) return
    logLoading = true
    try {
      if (reset) {
        logItems = []
        logHasMore = true
      }
      const res = await hooks.api.gitLog(hooks.root(), { limit: 60, skip: logItems.length, path: logFilterPath || undefined, grep: logFilterText || undefined, all: true })
      // 去重（--all 下多分支有交集）
      const seen = new Set(logItems.map((c) => c.hash))
      for (const c of res.commits) if (!seen.has(c.hash)) logItems.push(c)
      logHasMore = res.hasMore
    } catch (err) {
      toast(`读取日志失败：${(err as Error).message}`, "error")
      logHasMore = false
    } finally {
      logLoading = false
      renderLog()
    }
  }

  function renderLog(): void {
    const search = h("input", { class: "fw-input sm", placeholder: "按提交信息过滤…" })
    search.value = logFilterText
    search.onkeydown = (e) => {
      if (e.key === "Enter") {
        logFilterText = search.value.trim()
        void loadLog(true)
      }
    }
    const head = h("div", { class: "fw-git-subbar" }, [
      search,
      logFilterPath
        ? (() => {
            const chip = h("button", { class: "fw-chip", title: "清除路径过滤" }, [icon("file", 12), h("span", { text: logFilterPath }), icon("close", 12)])
            chip.onclick = () => {
              logFilterPath = ""
              void loadLog(true)
            }
            return chip
          })()
        : null,
      h("span", { class: "fw-grow" }),
      btnIcon("refresh", "刷新", () => void loadLog(true)),
    ])

    const list = h("div", { class: "fw-log-list" })
    if (!logItems.length && !logLoading) list.appendChild(h("div", { class: "fw-empty", text: logFilterPath ? "该文件暂无提交历史" : "暂无提交记录" }))
    for (const c of logItems) {
      const row = h("div", { class: "fw-log-row" }, [
        h("div", { class: "fw-log-graph" }, [h("span", { class: "fw-commit-dot" + (c.parents.length > 1 ? " merge" : "") })]),
        h("div", { class: "fw-log-main" }, [
          h("div", { class: "fw-log-subject", text: c.subject || "(无提交信息)", title: c.subject }),
          h("div", { class: "fw-log-meta" }, [
            h("span", { class: "fw-log-hash", text: c.short }),
            h("span", { text: c.author }),
            h("span", { text: timeAgo(c.commitTime) }),
            ...c.refs.slice(0, 3).map((r) => h("span", { class: "fw-ref-chip", text: r.replace(/^HEAD -> /, "").replace(/^tag: /, "🏷 ") })),
          ]),
        ]),
      ])
      row.onclick = () => void openCommit(c)
      row.oncontextmenu = (e) => {
        e.preventDefault()
        showMenu(e.clientX, e.clientY, [
          { label: "查看变更文件", icon: "diff", onClick: () => void openCommit(c) },
          { label: "此提交 ↔ 工作区（之后改了什么）", icon: "edit", onClick: () => hooks.openCompare({ from: c.hash, to: "WORKTREE" }) },
          { label: "此提交 ↔ 暂存区", icon: "archive", onClick: () => hooks.openCompare({ from: c.hash, to: "INDEX" }) },
          { label: "与另一提交/分支比较…", icon: "sync", onClick: () => hooks.openCompare({ from: c.hash, to: "HEAD" }) },
          { separator: true },
          { label: "复制提交哈希", icon: "copy", onClick: () => void navigator.clipboard.writeText(c.hash).then(() => toast("已复制哈希", "success")) },
          { label: "复制提交信息", icon: "copy", onClick: () => void navigator.clipboard.writeText(c.subject).then(() => toast("已复制", "success")) },
          { separator: true },
          { label: "拣选到当前分支（cherry-pick）", icon: "diff", disabled: !hooks.writable(), onClick: () => void confirmer("拣选提交", `将 ${c.short} 拣选到当前分支？`, () => op("cherry-pick", { ref: c.hash }, "已拣选")) },
          { label: "回滚此提交（revert）", icon: "undo", disabled: !hooks.writable(), onClick: () => void confirmer("回滚提交", `将创建一个反向提交以撤销 ${c.short}？`, () => op("revert", { ref: c.hash }, "已回滚")) },
          { label: "重置到此提交…", icon: "warning", danger: true, disabled: !hooks.writable(), onClick: () => void resetTo(c) },
          { separator: true },
          { label: "在此提交打标签…", icon: "tag", onClick: () => void createTag(c.hash) },
          { label: "新建分支…", icon: "branch", onClick: () => void createBranch(c.hash) },
        ])
      }
      list.appendChild(row)
    }
    if (logHasMore) {
      const more = h("button", { class: "fw-btn ghost sm fw-more", text: logLoading ? "加载中…" : "加载更多" })
      more.onclick = () => void loadLog(false)
      // 滚动到底自动加载
      list.onscroll = () => {
        if (list.scrollTop + list.clientHeight > list.scrollHeight - 60 && !logLoading) void loadLog(false)
      }
      list.appendChild(more)
    }
    body.replaceChildren(head, list)
  }

  async function confirmer(title: string, message: string, fn: () => Promise<unknown>): Promise<void> {
    const ok = await confirmDialog({ title, message, okText: "执行" })
    if (ok) await fn()
  }

  async function resetTo(c: GitCommitInfo): Promise<void> {
    const mode = await promptDialog({ title: `重置到 ${c.short}`, label: "模式（soft / mixed / hard）", value: "mixed", hint: "hard 会丢弃工作区改动（自动创建 gebai/backup-* 备份分支）；mixed 仅重置索引；soft 保留全部改动。" })
    if (!mode) return
    if (!["soft", "mixed", "hard"].includes(mode.trim())) {
      toast("模式必须是 soft / mixed / hard", "error")
      return
    }
    const res = await op("reset", { ref: c.hash, mode: mode.trim(), backup: true }, `已重置（${mode.trim()}）`)
    if (res?.backupBranch) toast(`已创建备份分支 ${res.backupBranch}`, "info", 6000)
    hooks.onFsChanged()
  }

  async function createBranch(ref?: string): Promise<void> {
    const name = await promptDialog({ title: "新建分支", label: "分支名", placeholder: "feature/xxx", hint: ref ? `基于 ${ref.slice(0, 8)} 创建` : "基于当前 HEAD 创建" })
    if (!name?.trim()) return
    await op("branch", { action: "create", name: name.trim(), startPoint: ref }, "已创建分支")
  }

  async function createTag(ref?: string): Promise<void> {
    const name = await promptDialog({ title: "新建标签", label: "标签名", value: "v", hint: ref ? `打在 ${ref.slice(0, 8)} 上` : "打在当前 HEAD 上" })
    if (!name?.trim()) return
    const msg = await promptDialog({ title: "标签说明（可选）", label: "注释", placeholder: "轻量标签可留空", multiline: true })
    if (msg === null) return
    await op("tag", { action: "create", name: name.trim(), ref, message: msg.trim() || undefined }, "已创建标签")
  }

  /** 提交详情：概览 + 变更文件列表（点开并列 diff）。 */
  async function openCommit(c: GitCommitInfo): Promise<void> {
    const host = h("div", { class: "fw-commit-detail" })
    host.appendChild(h("div", { class: "fw-loading", text: "加载提交详情…" }))
    body.replaceChildren(host)
    try {
      const res = await hooks.api.gitCommit(hooks.root(), c.hash)
      const files = h("div", { class: "fw-commit-files" })
      for (const f of res.files) {
        const name = f.path.split("/").pop() ?? f.path
        const row = h("div", { class: "fw-commit-file" }, [
          h("span", { class: `fw-change-mark ${f.status === "added" ? "A" : f.status === "deleted" ? "D" : f.status === "renamed" ? "R" : "M"}`, text: f.status === "added" ? "A" : f.status === "deleted" ? "D" : f.status === "renamed" ? "R" : "M" }),
          h("span", { class: "fw-change-name", text: name, title: f.path }),
          h("span", { class: "fw-grow" }),
          h("span", { class: "fw-diff-stat add", text: `+${f.additions}` }),
          h("span", { class: "fw-diff-stat del", text: `-${f.deletions}` }),
        ])
        row.onclick = () =>
          hooks.openDiff({
            title: `${name} @ ${c.short}`,
            root: hooks.root(),
            path: f.path,
            source: { type: "commit", hash: c.hash },
            fallback: f,
          })
        files.appendChild(row)
      }
      const back = h("button", { class: "fw-btn ghost sm" }, [icon("back"), h("span", { text: "返回日志" })])
      back.onclick = () => show("log")
      const diffBtn = h("button", { class: "fw-btn sm" }, [icon("diff"), h("span", { text: "整提交差异" })])
      diffBtn.onclick = () =>
        hooks.openDiff({ title: `提交 ${c.short}`, root: hooks.root(), path: "", source: { type: "range", from: `${c.hash}^`, to: c.hash } })
      const workBtn = h("button", { class: "fw-btn sm", title: "此提交之后工作区又改了什么" }, [icon("edit"), h("span", { text: "与工作区比较" })])
      workBtn.onclick = () => hooks.openCompare({ from: c.hash, to: "WORKTREE" })
      host.replaceChildren(
        h("div", { class: "fw-git-subbar" }, [back, h("span", { class: "fw-info" }, [h("span", { class: "fw-log-hash", text: c.short })]), h("span", { class: "fw-grow" }), workBtn, diffBtn]),
        h("div", { class: "fw-commit-head" }, [
          h("div", { class: "fw-commit-subject", text: c.subject }),
          h("div", { class: "fw-log-meta" }, [h("span", { text: c.author }), h("span", { text: c.authorEmail }), h("span", { text: formatTime(c.commitTime) }), ...c.refs.map((r) => h("span", { class: "fw-ref-chip", text: r }))]),
          c.body ? h("pre", { class: "fw-commit-body", text: c.body }) : null,
        ]),
        h("div", { class: "fw-section-title", text: `变更文件（${res.files.length}）` }),
        files,
      )
    } catch (err) {
      host.replaceChildren(h("div", { class: "fw-error", text: `加载失败：${(err as Error).message}` }))
    }
  }

  /* ------------------------------ 分支视图 ------------------------------ */

  async function loadBranches(): Promise<void> {
    try {
      const res = await hooks.api.gitBranches(hooks.root())
      branches = res.branches
    } catch (err) {
      toast(`读取分支失败：${(err as Error).message}`, "error")
    }
    renderBranches()
  }

  function renderBranches(): void {
    const s = hooks.status()
    const toolbar = h("div", { class: "fw-git-subbar" }, [
      h("span", { class: "fw-info", text: s?.branch ? `当前 ${s.branch}${s.upstream ? ` → ${s.upstream}` : ""}` : "（无分支）" }),
      h("span", { class: "fw-grow" }),
      (() => {
        const b = h("button", { class: "fw-btn ghost sm" }, [icon("plus"), h("span", { text: "新建" })])
        b.onclick = () => void createBranch()
        return b
      })(),
      btnIcon("refresh", "刷新", () => void loadBranches()),
    ])
    const local = branches.filter((b) => !b.remote)
    const remote = branches.filter((b) => b.remote)
    const list = h("div", { class: "fw-branch-list" })
    const group = (title: string, items: GitBranchInfo[]) => {
      if (!items.length) return
      list.appendChild(h("div", { class: "fw-section-title", text: `${title}（${items.length}）` }))
      for (const b of items) {
        const row = h("div", { class: `fw-branch-row${b.current ? " current" : ""}` }, [
          icon(b.current ? "check" : "branch", 13),
          h("span", { class: "fw-branch-name", text: b.name, title: b.subject }),
          b.ahead ? h("span", { class: "fw-ahead", text: `↑${b.ahead}`, title: "领先上游提交数" }) : null,
          b.behind ? h("span", { class: "fw-behind", text: `↓${b.behind}`, title: "落后上游提交数" }) : null,
          h("span", { class: "fw-grow" }),
          h("span", { class: "fw-log-hash", text: b.hash.slice(0, 7) }),
        ])
        row.onclick = () => {
          if (b.current) return
          void confirmer("切换分支", `检出「${b.name}」？未提交的改动会随工作区一起携带。`, async () => {
            await op(b.remote ? "checkout" : "branch", b.remote ? { ref: b.name.replace(/^([^/]+)\//, "$1/") , detach: false } : { action: "checkout", name: b.name }, "已切换分支")
            hooks.onFsChanged()
          })
        }
        row.oncontextmenu = (e) => {
          e.preventDefault()
          showMenu(e.clientX, e.clientY, [
            { label: "检出", icon: "check", disabled: b.current, onClick: () => void op("branch", { action: "checkout", name: b.name }, "已切换") },
            { label: "与当前分支比较（共同祖先）", icon: "diff", disabled: b.current, onClick: () => hooks.openCompare({ from: "HEAD", to: b.name, mergeBase: true }) },
            { label: "与当前分支比较（含各自新提交）", icon: "diff", disabled: b.current, onClick: () => hooks.openCompare({ from: "HEAD", to: b.name }) },
            { label: "与工作区比较", icon: "edit", onClick: () => hooks.openCompare({ from: b.name, to: "WORKTREE" }) },
            { label: "合并到当前分支", icon: "git", disabled: b.current, onClick: () => void confirmer("合并分支", `将「${b.name}」合并到当前分支？`, () => op("merge", { ref: b.name }, "已合并")) },
            { label: "将当前分支变基到它", icon: "sync", disabled: b.current, onClick: () => void confirmer("变基", `将当前分支变基到「${b.name}」？`, () => op("rebase", { ref: b.name }, "已变基")) },
            { separator: true },
            { label: "重命名…", icon: "edit", disabled: b.remote || !hooks.writable(), onClick: () => void (async () => {
              const nn = await promptDialog({ title: "重命名分支", label: "新名称", value: b.name })
              if (nn?.trim()) await op("branch", { action: "rename", name: b.name, newName: nn.trim() }, "已重命名")
            })() },
            { label: "设为当前跟踪（set upstream）…", icon: "sync", disabled: !b.remote, onClick: () => void op("branch", { action: "upstream", name: hooks.status()?.branch, startPoint: b.name }, "已设置上游") },
            { separator: true },
            { label: "删除分支", icon: "trash", danger: true, disabled: b.current || !hooks.writable(), onClick: () => void confirmer("删除分支", `删除本地分支「${b.name}」？未合并的提交会丢失。`, () => op("branch", { action: "delete", name: b.name, force: true }, "已删除")) },
            { label: "复制分支名", icon: "copy", onClick: () => void navigator.clipboard.writeText(b.name) },
          ])
        }
        list.appendChild(row)
      }
    }
    group("本地分支", local)
    group("远程分支", remote)
    body.replaceChildren(toolbar, list)
  }

  /* ------------------------------ 标签 / 暂存 / 远程 ------------------------------ */

  async function loadTags(): Promise<void> {
    try {
      localTags = (await hooks.api.gitTags(hooks.root())).tags
    } catch (err) {
      toast(`读取标签失败：${(err as Error).message}`, "error")
    }
    const toolbar = h("div", { class: "fw-git-subbar" }, [
      h("span", { class: "fw-info", text: `${localTags.length} 个标签` }),
      h("span", { class: "fw-grow" }),
      (() => {
        const b = h("button", { class: "fw-btn ghost sm" }, [icon("plus"), h("span", { text: "新建标签" })])
        b.onclick = () => void createTag()
        return b
      })(),
      btnIcon("refresh", "刷新", () => void loadTags()),
    ])
    const list = h("div", { class: "fw-branch-list" })
    for (const t of localTags) {
      const row = h("div", { class: "fw-branch-row" }, [icon("tag", 13), h("span", { class: "fw-branch-name", text: t.name }), h("span", { class: "fw-grow" }), h("span", { class: "fw-log-hash", text: t.hash.slice(0, 7) })])
      row.oncontextmenu = (e) => {
        e.preventDefault()
        showMenu(e.clientX, e.clientY, [
          { label: "检出该标签（分离 HEAD）", icon: "check", onClick: () => void op("checkout", { ref: t.name, detach: true }, "已检出标签") },
          { label: "删除标签", icon: "trash", danger: true, onClick: () => void confirmer("删除标签", `删除「${t.name}」？`, () => op("tag", { action: "delete", name: t.name }, "已删除")) },
        ])
      }
      list.appendChild(row)
    }
    if (!localTags.length) list.appendChild(h("div", { class: "fw-empty", text: "暂无标签" }))
    body.replaceChildren(toolbar, list)
  }

  async function loadStash(): Promise<void> {
    try {
      stashes = (await hooks.api.gitStash(hooks.root())).stashes
    } catch (err) {
      toast(`读取暂存失败：${(err as Error).message}`, "error")
    }
    const toolbar = h("div", { class: "fw-git-subbar" }, [
      h("span", { class: "fw-info", text: `${stashes.length} 条暂存` }),
      h("span", { class: "fw-grow" }),
      (() => {
        const b = h("button", { class: "fw-btn ghost sm" }, [icon("plus"), h("span", { text: "暂存当前改动" })])
        b.onclick = () => void (async () => {
          const msg = await promptDialog({ title: "暂存改动（git stash）", label: "备注", placeholder: "例如：临时切换分支" })
          if (msg === null) return
          await op("stash", { action: "push", message: msg || undefined }, "已暂存")
          hooks.onFsChanged()
        })()
        return b
      })(),
      btnIcon("refresh", "刷新", () => void loadStash()),
    ])
    const list = h("div", { class: "fw-branch-list" })
    for (const st of stashes) {
      const row = h("div", { class: "fw-branch-row" }, [icon("archive", 13), h("span", { class: "fw-branch-name", text: st.message || st.ref }), h("span", { class: "fw-grow" }), h("span", { class: "fw-log-hash", text: st.ref })])
      row.onclick = () => void confirmer("恢复暂存", `弹出「${st.message || st.ref}」并应用到工作区？`, async () => {
        await op("stash", { action: "pop", index: st.index }, "已恢复暂存")
        hooks.onFsChanged()
      })
      row.oncontextmenu = (e) => {
        e.preventDefault()
        showMenu(e.clientX, e.clientY, [
          { label: "弹出（pop，成功后删除记录）", icon: "upload", onClick: () => void op("stash", { action: "pop", index: st.index }, "已弹出").then(() => hooks.onFsChanged()) },
          { label: "应用（apply，保留记录）", icon: "download", onClick: () => void op("stash", { action: "apply", index: st.index }, "已应用").then(() => hooks.onFsChanged()) },
          { separator: true },
          { label: "删除该暂存", icon: "trash", danger: true, onClick: () => void confirmer("删除暂存", "删除后无法恢复，确定？", () => op("stash", { action: "drop", index: st.index }, "已删除")) },
        ])
      }
      list.appendChild(row)
    }
    if (!stashes.length) list.appendChild(h("div", { class: "fw-empty", text: "暂无暂存记录" }))
    body.replaceChildren(toolbar, list)
  }

  async function loadRemotes(): Promise<void> {
    try {
      remotes = (await hooks.api.gitRemotes(hooks.root())).remotes
    } catch (err) {
      toast(`读取远程失败：${(err as Error).message}`, "error")
    }
    const s = hooks.status()
    const toolbar = h("div", { class: "fw-git-subbar" }, [
      h("span", { class: "fw-info", text: s?.upstream ? `跟踪 ${s.upstream}` : "未设置上游" }),
      h("span", { class: "fw-grow" }),
      btnIcon("refresh", "刷新", () => void loadRemotes()),
    ])
    const actions = h("div", { class: "fw-remote-actions" })
    const mkBtn = (label: string, iconName: string, fn: () => void, disabled = false) => {
      const b = h("button", { class: "fw-btn sm" }, [icon(iconName), h("span", { text: label })])
      b.disabled = disabled || !hooks.remoteEnabled()
      b.onclick = fn
      return b
    }
    actions.append(
      mkBtn("抓取 fetch", "download", () => void op("fetch", { prune: true }, "抓取完成")),
      mkBtn("拉取 pull", "sync", () => void confirmer("拉取", "从远程拉取当前分支并合并？", () => op("pull", { ffOnly: false }, "拉取完成"))),
      mkBtn("拉取（仅快进）", "sync", () => void op("pull", { ffOnly: true }, "拉取完成")),
      mkBtn("变基拉取", "sync", () => void op("pull", { rebase: true }, "拉取完成")),
      mkBtn("推送 push", "upload", () => void op("push", { setUpstream: true }, "推送完成")),
      mkBtn("强制推送（含租约）", "upload", () =>
        void confirmer("强制推送", "使用 --force-with-lease 覆盖远程分支？请确认远程没有他人新提交。", () => op("push", { forceWithLease: true }, "已强制推送")),
      ),
      (() => {
        const b = h("button", { class: "fw-btn sm" }, [icon("plus"), h("span", { text: "添加远程" })])
        b.onclick = () => void (async () => {
          const name = await promptDialog({ title: "添加远程", label: "名称", value: "origin" })
          if (!name?.trim()) return
          const url = await promptDialog({ title: "远程地址", label: "URL", placeholder: "git@github.com:user/repo.git" })
          if (!url?.trim()) return
          await op("remote", { action: "add", name: name.trim(), url: url.trim() }, "已添加远程")
          void loadRemotes()
        })()
        return b
      })(),
    )
    const list = h("div", { class: "fw-branch-list" })
    for (const r of remotes) {
      const row = h("div", { class: "fw-branch-row" }, [icon("git", 13), h("span", { class: "fw-branch-name", text: r.name }), h("span", { class: "fw-grow" }), h("span", { class: "fw-remote-url", text: r.fetchUrl, title: `${r.fetchUrl}\n推送：${r.pushUrl}` })])
      row.oncontextmenu = (e) => {
        e.preventDefault()
        showMenu(e.clientX, e.clientY, [
          { label: "复制地址", icon: "copy", onClick: () => void navigator.clipboard.writeText(r.fetchUrl) },
          { label: "修改地址…", icon: "edit", onClick: () => void (async () => {
            const url = await promptDialog({ title: `修改 ${r.name}`, label: "URL", value: r.fetchUrl })
            if (url?.trim()) {
              await op("remote", { action: "set-url", name: r.name, url: url.trim() }, "已修改")
              void loadRemotes()
            }
          })() },
          { label: "移除远程", icon: "trash", danger: true, onClick: () => void confirmer("移除远程", `移除「${r.name}」？`, async () => {
            await op("remote", { action: "remove", name: r.name }, "已移除")
            void loadRemotes()
          }) },
        ])
      }
      list.appendChild(row)
    }
    if (!remotes.length) list.appendChild(h("div", { class: "fw-empty", text: "未配置远程仓库" }))
    body.replaceChildren(toolbar, actions, list)
  }

  /* ------------------------------ 主流程 ------------------------------ */

  async function refresh(): Promise<void> {
    renderTabs()
    if (view === "changes") renderChanges()
    else if (view === "log") await loadLog(logItems.length === 0)
    else if (view === "branches") await loadBranches()
    else if (view === "tags") await loadTags()
    else if (view === "stash") await loadStash()
    else if (view === "remotes") await loadRemotes()
  }

  return { el, refresh, show, view: () => view }
}

/** 并列差异视图（主区域内容）：解析两侧文本 → Monaco diff；失败回退结构化 hunks。 */
export async function mountDiffView(
  host: HTMLElement,
  api: FsApi,
  spec: DiffSpec,
  ctx: { repoRootPath: string; language: string },
): Promise<() => void> {
  const { root, path, source } = spec
  /** 取某端点下的文件内容（WORKTREE / INDEX / rev 统一入口）。 */
  const side = async (ref: string): Promise<string> => {
    const res = await api.gitContent(root, ref, path)
    return res.binary ? "（二进制文件，无法按文本显示）" : res.content
  }
  /** 端点对（A=原侧，B=改侧）+ 人类可读标题。 */
  const endpoints = async (): Promise<{ originalRef: string; modifiedRef: string; note: string }> => {
    if (source.type === "worktree") {
      return source.staged
        ? { originalRef: "HEAD", modifiedRef: INDEX_REF, note: "HEAD ↔ 暂存区" }
        : { originalRef: INDEX_REF, modifiedRef: WORKTREE_REF, note: "暂存区 ↔ 工作区" }
    }
    if (source.type === "commit") {
      return { originalRef: `${source.hash}^`, modifiedRef: source.hash, note: `${source.hash.slice(0, 8)} 本次提交` }
    }
    if (source.mergeBase) {
      // 三点：A 侧用共同祖先（与服务端 git diff A...B 语义一致）
      return { originalRef: source.from, modifiedRef: source.to, note: source.label ?? `${source.from}...${source.to}（共同祖先）` }
    }
    return { originalRef: source.from, modifiedRef: source.to, note: source.label ?? `${source.from} ↔ ${source.to}` }
  }

  try {
    const ep = await endpoints()
    let originalRef = ep.originalRef
    // 三点语义（A...B）：先解析共同祖先，A 侧用祖先内容（与服务端 git diff A...B 一致）
    if (source.type === "range" && source.mergeBase) {
      const cmp = await api.gitCompare(root, { from: source.from, to: source.to, mergeBase: true, path }).catch(() => null)
      if (cmp?.mergeBaseOf) originalRef = cmp.mergeBaseOf
    }
    const [original, modified] = await Promise.all([side(originalRef), side(ep.modifiedRef)])
    const wrap = h("div", { class: "fw-diff-wrap" }, [
      h("div", { class: "fw-viewer-bar" }, [
        h("span", { class: "fw-viewer-info", text: `${ep.note}` }),
        h("span", { class: "fw-viewer-spacer" }),
        h("span", { class: "fw-hint", text: `A：${source.type === "range" ? (source.mergeBase ? "共同祖先" : source.from) : source.type === "commit" ? `${source.hash.slice(0, 8)}^` : "HEAD"} ｜ B：${source.type === "range" ? source.to : source.type === "commit" ? source.hash.slice(0, 8) : "工作区/暂存区"}` }),
      ]),
      (() => {
        const box = h("div", { class: "fw-diff-host" })
        return box
      })(),
    ])
    host.appendChild(wrap)
    const diffHost = wrap.querySelector(".fw-diff-host") as HTMLElement
    const handle = await createDiffEditor(diffHost, { original, modified, language: ctx.language })
    return () => {
      handle.dispose()
      wrap.remove()
    }
  } catch (err) {
    // 回退：结构化 hunks（服务端已解析）
    const wrap = h("div", { class: "fw-hunks" })
    if (spec.fallback?.binary) {
      wrap.appendChild(h("div", { class: "fw-empty", text: "二进制文件差异，无法逐行显示" }))
    } else if (spec.fallback?.hunks.length) {
      for (const hk of spec.fallback.hunks) {
        wrap.appendChild(h("div", { class: "fw-hunk-head", text: hk.header }))
        for (const line of hk.lines) {
          wrap.appendChild(
            h("div", { class: `fw-hunk-line ${line.type}` }, [
              h("span", { class: "fw-hunk-no", text: line.oldLine ? String(line.oldLine) : "" }),
              h("span", { class: "fw-hunk-no", text: line.newLine ? String(line.newLine) : "" }),
              h("span", { class: "fw-hunk-sign", text: line.type === "add" ? "+" : line.type === "del" ? "-" : " " }),
              h("span", { class: "fw-hunk-text", text: line.text }),
            ]),
          )
        }
      }
    } else {
      wrap.appendChild(h("div", { class: "fw-error", text: `无法显示差异：${(err as Error).message}` }))
    }
    host.appendChild(wrap)
    return () => wrap.remove()
  }
}
