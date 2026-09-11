/**
 * 文件工作台 · Git 工具窗（底部停靠，IDEA 式）：**分支 | 日志 | 提交内容** 三栏并排。
 *
 * 三条栏构成一条动线：点分支 → 看它的日志 → 点某条提交 → 看这条提交改了什么。
 * 分工上「变更（工作区改动 + 提交框）」不在这里——那是左栏的**变更面板**（changes.ts），
 * 因为"我改了什么、现在提交"是编码时随时要看的，而"历史"是回顾时才看的，节奏不同。
 *
 * 交互原则（对齐 IDEA 的 VCS 工具窗习惯）：
 * - 三栏边界可拖（宽度记忆在 localStorage，双击分界复位）；
 * - 日志分页加载、支持提交信息过滤与单文件历史（Git log --follow），点条目在右栏看内容；
 * - 破坏性操作（重置 / 强制推送 / 分支删除）统一二次确认，并在有备份能力时提示
 *   （hard reset 自动建备份分支——见服务端 GitService）；
 * - 多步操作（merge/rebase/cherry-pick）的「继续 / 跳过 / 中止」在变更面板顶部（冲突属于工作区状态）。
 */
import type { FsApi, GitBranchInfo, GitCommitInfo, GitFileDiff, GitStatusInfo } from "./api"
import { h, icon, showMenu, toast, confirmDialog, promptDialog, clear, timeAgo, formatTime, formatSize, append } from "./ui"
import { btnIcon, createOpRunner, renderNotRepo as renderNotRepoShared } from "./git-shared"
import { createDiffEditor, type DiffNav } from "./editor"

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
  /** 打开冲突合并标签（三窗格：我方 / 结果 / 对方）；入参为仓库相对路径 */
  openMerge: (repoRel: string) => void
  /** 关闭工具窗（标题栏关闭按钮；由宿主收起面板） */
  close: () => void
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

export function createGitPanel(hooks: GitHooks): GitPanel {
  let view: GitView = "changes"
  let logItems: GitCommitInfo[] = []
  /** 当前选中的提交（右栏「提交内容」显示它）。 */
  let logHasMore = true
  let logLoading = false
  let logFilterPath = ""
  let logFilterText = ""
  let branches: GitBranchInfo[] = []
  let localTags: Array<{ name: string; hash: string; time?: number; subject?: string }> = []
  let stashes: Array<{ index: number; ref: string; message: string; time?: number }> = []
  let remotes: Array<{ name: string; fetchUrl: string; pushUrl: string }> = []

  /* ------------------------------ 工具窗骨架（IDEA 式） ------------------------------
   * 底部工具窗 = 标题栏 + 三栏并排：变更内容 / 日志 / 分支。
   * 为什么拆三栏而不是标签页：面板在**底部横向**展开，横向空间足够——变更、日志、分支
   * 是提交前后最常互相参照的三块信息，能同屏看到比来回切标签有用（IDEA 工具窗同理）。
   * 标签/暂存/远程收进「分支」栏内的小切换（同属「引用与远程」语义，用得不频繁）。
   * ------------------------------------------------------------------------------ */

  const titleBar = h("div", { class: "fw-git-titlebar" })
  /**
   * 三栏并排：分支（引用） | 日志 | 提交内容——**从左到右是一条动线**：
   * 点分支 → 看它的日志 → 点某条提交 → 看这条提交改了什么。
   * 日志栏只放日志（不再就地展开提交详情，否则"看列表"和"看内容"互相打断、返回后又丢滚动位置）。
   */
  const colRefs = h("div", { class: "fw-git-col-body" })
  const colLog = h("div", { class: "fw-git-col-body" })
  const colCommit = h("div", { class: "fw-git-col-body" })
  /** 「分支」栏内部的小切换（分支 / 标签 / 暂存 / 远程）。 */
  let refsTab: "branches" | "tags" | "stash" | "remotes" = "branches"
  /** 日志当前限定的分支/引用（点击分支栏设置）；空 = 全部分支（--all）。 */
  let logBranch = ""

  const colHead = (title: string, extra: Array<Node | null> = []): HTMLElement =>
    h("div", { class: "fw-git-col-head" }, [h("span", { class: "fw-git-col-title", text: title }), ...extra])

  const refsTabsHost = h("div", { class: "fw-git-refs-tabs" })
  /** 日志栏标题上的「当前分支过滤」芯片（点了分支才有）。 */
  const logScope = h("span", { class: "fw-git-count-scope" })
  const commitHint = h("span", { class: "fw-git-col-hint", text: "点击日志查看" })
  const colRefsEl = h("div", { class: "fw-git-col", "data-col": "refs" }, [colHead("分支", [refsTabsHost]), colRefs])
  const colLogEl = h("div", { class: "fw-git-col", "data-col": "log" }, [colHead("日志", [logScope]), colLog])
  const colCommitEl = h("div", { class: "fw-git-col", "data-col": "commit" }, [colHead("提交内容", [commitHint]), colCommit])

  // 分界可拖：宽度存 CSS 变量，三栏共享（拖动左界只改左栏、右界改中栏）
  const sp1 = h("div", { class: "fw-col-resizer", title: "拖动调整栏宽" })
  const sp2 = h("div", { class: "fw-col-resizer", title: "拖动调整栏宽" })
  const colsHost = h("div", { class: "fw-git-cols" }, [colRefsEl, sp1, colLogEl, sp2, colCommitEl])
  const el = h("div", { class: "fw-git-panel" }, [titleBar, colsHost])

  /**
   * 分界拖动：
   *   a（左界）= 分支栏宽度，指针向右 = 变宽；
   *   c（右界）= **提交内容栏**宽度，指针向左 = 变宽（拖的是右栏的左边缘）。
   * 中间日志栏吃掉剩余空间（flex: 1），所以只需记两个数。宽度存 localStorage，跨会话记忆。
   */
  const COL_KEY = "gebai.ui.gitCols"
  type ColW = { a: number | null; c: number | null }
  function readCols(): ColW {
    try {
      const raw = localStorage.getItem(COL_KEY)
      if (!raw) return { a: null, c: null }
      const o = JSON.parse(raw) as { a?: number; c?: number }
      return { a: typeof o.a === "number" ? o.a : null, c: typeof o.c === "number" ? o.c : null }
    } catch {
      return { a: null, c: null }
    }
  }
  function applyCols(): void {
    const w = readCols()
    if (w.a) colsHost.style.setProperty("--git-col-a", `${w.a}px`)
    if (w.c) colsHost.style.setProperty("--git-col-c", `${w.c}px`)
  }
  function bindColResizer(handle: HTMLElement, which: "a" | "c"): void {
    const varName = which === "a" ? "--git-col-a" : "--git-col-c"
    let dragging = false
    let startX = 0
    let startW = 0
    handle.addEventListener("mousedown", (e) => {
      dragging = true
      startX = e.clientX
      startW = (which === "a" ? colRefsEl : colCommitEl).getBoundingClientRect().width
      handle.classList.add("active")
      document.body.classList.add("fw-col-resizing")
      e.preventDefault()
    })
    window.addEventListener("mousemove", (e) => {
      if (!dragging) return
      const total = colsHost.getBoundingClientRect().width
      // 分支/提交栏 140px 起（还要放得下分支名与提交主题），并给中间日志栏留 240px
      const max = Math.max(140, which === "a" ? total - 380 : total - 380)
      const delta = which === "a" ? e.clientX - startX : startX - e.clientX
      const w = Math.max(140, Math.min(max, startW + delta))
      colsHost.style.setProperty(varName, `${w}px`)
    })
    const persist = (): void => {
      const cur = readCols()
      const w = parseInt(getComputedStyle(colsHost).getPropertyValue(varName), 10)
      const next: ColW = { a: which === "a" ? w || cur.a : cur.a, c: which === "c" ? w || cur.c : cur.c }
      try {
        localStorage.setItem(COL_KEY, JSON.stringify(next))
      } catch {
        /* 隐私模式忽略 */
      }
    }
    window.addEventListener("mouseup", () => {
      if (!dragging) return
      dragging = false
      handle.classList.remove("active")
      document.body.classList.remove("fw-col-resizing")
      persist()
    })
    // 双击分界 = 复位该栏（回到 CSS 默认比例）
    handle.addEventListener("dblclick", () => {
      colsHost.style.removeProperty(varName)
      const cur = readCols()
      try {
        localStorage.setItem(COL_KEY, JSON.stringify(which === "a" ? { a: null, c: cur.c } : { a: cur.a, c: null }))
      } catch {
        /* 忽略 */
      }
    })
  }

  /** 「分支」栏内部切换渲染（分支/标签/暂存/远程）。 */
  function renderRefsTabs(): void {
    clear(refsTabsHost)
    const labels: Record<string, string> = { branches: "分支", tags: "标签", stash: "暂存", remotes: "远程" }
    for (const k of ["branches", "tags", "stash", "remotes"] as const) {
      const b = h("button", { class: `fw-git-refs-tab${refsTab === k ? " active" : ""}`, text: labels[k] })
      b.onclick = () => {
        refsTab = k
        renderRefsTabs()
        void refresh()
      }
      refsTabsHost.appendChild(b)
    }
  }

  /** 标题栏：面板身份 + 当前分支 + 全局动作（刷新 / 范围切换 / 关闭）。 */
  function renderTitleBar(): void {
    clear(titleBar)
    const s = hooks.status()
    append(titleBar, [
      icon("git", 14),
      h("span", { class: "fw-git-title", text: "源代码管理" }),
      s?.isRepo && s.branch ? h("span", { class: "fw-git-branch", title: "当前分支" }, [icon("branch", 12), h("span", { text: s.branch })]) : null,
      s?.operation
        ? h("span", { class: "fw-git-opbar" }, [
            h("span", { text: `${s.operation} 进行中${s.counts.conflicted ? `（${s.counts.conflicted} 个冲突）` : ""}` }),
          ])
        : null,
      h("span", { class: "fw-grow" }),
      // 比较入口：菜单栏移除后挪到工具窗标题栏（与 Git 语义同处）
      (() => {
        const b = h("button", { class: "fw-btn ghost sm", title: "比较任意两个端点（提交/分支 ↔ 提交/分支/工作区/暂存区）Ctrl+Shift+D" }, [
          icon("diff"),
          h("span", { text: "比较" }),
        ])
        b.onclick = () => hooks.openCompare()
        return b
      })(),
      (() => {
        const b = h("button", { class: "fw-icon-btn", title: "刷新" })
        b.appendChild(icon("refresh", 13))
        b.onclick = () => void refresh()
        return b
      })(),
      (() => {
        // 关闭按钮在面板内（工具窗自己的标题栏），与 IDEA 工具窗一致
        const b = h("button", { class: "fw-icon-btn", title: "关闭 Git 面板" })
        b.appendChild(icon("close", 13))
        b.onclick = () => hooks.close()
        return b
      })(),
    ])
  }

  /**
   * 切换到某视图：三栏并排常显，「分支」栏内的小切换（标签/暂存/远程）随之切换。
   * 保留该方法是因为状态栏分支名、菜单等外部入口要能「跳到某个视图」。
   */
  function show(v: GitView): void {
    view = v
    if (v === "tags" || v === "stash" || v === "remotes") refsTab = v
    void refresh()
  }

  /** 写操作（共享实现：toast + 刷新状态 + 刷新本面板）。 */
  const op = createOpRunner(hooks, async () => {
    await refresh()
  })

  const renderNotRepo = (): HTMLElement => renderNotRepoShared(hooks, op)

  /* ------------------------------ 日志视图 ------------------------------ */

  async function loadLog(reset = false): Promise<void> {
    if (logLoading) return
    logLoading = true
    try {
      if (reset) {
        logItems = []
        logHasMore = true
      }
      const res = await hooks.api.gitLog(hooks.root(), {
        limit: 60,
        skip: logItems.length,
        path: logFilterPath || undefined,
        grep: logFilterText || undefined,
        // 点了分支就只看该分支的日志；否则看全部分支（--all）
        ref: logBranch || undefined,
        all: !logBranch,
      })
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
      // 日志过滤切换后同步分支栏高亮（"我正在看哪个分支的日志"要看得出来）
      if (refsTab === "branches") renderBranches()
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
      logBranch
        ? (() => {
            const chip = h("button", { class: "fw-chip", title: "改为查看全部分支的日志" }, [icon("branch", 12), h("span", { text: logBranch }), icon("close", 12)])
            chip.onclick = () => {
              logBranch = ""
              void loadLog(true)
            }
            return chip
          })()
        : null,
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
      const row = h("div", { class: "fw-log-row", "data-hash": c.hash }, [
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
      list.appendChild(more)
    }
    colLog.replaceChildren(head, list)
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
    // 提交内容固定渲染在**右栏**（日志栏只放日志：列表不被打断、从右栏回看时滚动位置还在）
    colCommit.replaceChildren(host)
    commitHint.textContent = c.short
    for (const row of colLog.querySelectorAll<HTMLElement>(".fw-log-row")) row.classList.toggle("active", row.dataset.hash === c.hash)
    try {
      const res = await hooks.api.gitCommit(hooks.root(), c.hash)
      const files = h("div", { class: "fw-commit-files" })
      // 超大提交（超 diff 体量上限）：清单仍然完整（服务端用 --name-status/-numstat 补齐），
      // 但部分文件没有逐行内容——**说一声**，否则用户会以为“这个文件没改”。
      // 注意：提示条不能在这里 append——下方 host.replaceChildren(...) 会把早期子节点全清掉，
      // 它必须作为其中一个子节点一起交出去（这类“写了但被覆盖”的 bug 肉眼很难发现）。
      const truncHint = res.truncated
        ? h("div", { class: "fw-hint-bar" }, [icon("info", 13), h("span", { text: "该提交过大，已省略部分文件的逐行内容（清单与 +N/-N 仍然完整）" })])
        : null
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
      const diffBtn = h("button", { class: "fw-btn sm" }, [icon("diff"), h("span", { text: "整提交差异" })])
      diffBtn.onclick = () =>
        hooks.openDiff({ title: `提交 ${c.short}`, root: hooks.root(), path: "", source: { type: "range", from: `${c.hash}^`, to: c.hash } })
      const workBtn = h("button", { class: "fw-btn sm", title: "此提交之后工作区又改了什么" }, [icon("edit"), h("span", { text: "与工作区比较" })])
      workBtn.onclick = () => hooks.openCompare({ from: c.hash, to: "WORKTREE" })
      host.replaceChildren(
        h("div", { class: "fw-git-subbar" }, [
          h("span", { class: "fw-info" }, [h("span", { class: "fw-log-hash", text: c.short })]),
          h("span", { class: "fw-grow" }),
          workBtn,
          diffBtn,
        ]),
        h("div", { class: "fw-commit-head" }, [
          h("div", { class: "fw-commit-subject", text: c.subject }),
          h("div", { class: "fw-log-meta" }, [h("span", { text: c.author }), h("span", { text: c.authorEmail }), h("span", { text: formatTime(c.commitTime) }), ...c.refs.map((r) => h("span", { class: "fw-ref-chip", text: r }))]),
          c.body ? h("pre", { class: "fw-commit-body", text: c.body }) : null,
        ]),
        h("div", { class: "fw-section-title", text: `变更文件（${res.files.length}）` }),
        ...(truncHint ? [truncHint] : []),
        files,
      )
    } catch (err) {
      host.replaceChildren(h("div", { class: "fw-error", text: `加载失败：${(err as Error).message}` }))
    }
  }

  /* ------------------------------ 分支视图 ------------------------------ */

  /**
   * 推送某个分支（分支右键菜单）。
   *
   * 为什么不能简单地 `git push <branch>`：git 把无仓库参数时的第一个参数当成**仓库**（远程名或 URL），
   * 不是 refspec——`git push feat` 会报 "'feat' does not appear to be a git repository"。
   * 所以必须显式给出远程，并用 `本地:远程` 形式的 refspec：
   *   · 分支已配上游（`b.upstream` 形如 `origin/feat-x`）→ 推给**它的上游**，且远端分支名按上游取
   *     （不能想当然用同名：上游可以叫别的名字）；
   *   · 没有上游 → 这是「发布分支」动作：挑一个远程，推同名分支并 `--set-upstream` 建立跟踪。
   */
  async function pushBranch(b: GitBranchInfo, at?: { x: number; y: number }): Promise<void> {
    const slash = b.upstream?.indexOf("/") ?? -1
    if (b.upstream && slash > 0) {
      const remote = b.upstream.slice(0, slash)
      const branch = b.upstream.slice(slash + 1)
      await op("push", { remote, refspec: `${b.name}:${branch}` }, `已推送 ${b.name}`)
      return
    }
    // 无上游：需要挑一个远程（远程清单可能还没加载过，先取一次）
    let list = remotes
    try {
      list = (await hooks.api.gitRemotes(hooks.root())).remotes
    } catch {
      /* 取不到就用已有缓存 */
    }
    if (!list.length) {
      toast("未配置远程仓库：先在引用栏的「远程」里添加一个", "error", 5000)
      return
    }
    const publish = (remote: string) => {
      void op("push", { remote, refspec: `${b.name}:${b.name}`, setUpstream: true }, `已推送 ${b.name} 并设为跟踪 ${remote}/${b.name}`)
    }
    // 唯一远程与 origin 都不用问；多个远程且无 origin 时让用户选一个（网络写操作不替用户猜）
    const only = list.find((r) => r.name === "origin") ?? (list.length === 1 ? list[0] : undefined)
    if (only) {
      publish(only.name)
      return
    }
    if (!at) return
    showMenu(
      at.x,
      at.y,
      list.map((r) => ({ label: `推送到 ${r.name}（并设为上游）`, icon: "upload", onClick: () => publish(r.name) })),
    )
  }

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
        const row = h("div", { class: `fw-branch-row${b.current ? " current" : ""}${logBranch === b.name ? " log-active" : ""}` }, [
          icon(b.current ? "check" : "branch", 13),
          h("span", { class: "fw-branch-name", text: b.name, title: b.subject }),
          b.ahead ? h("span", { class: "fw-ahead", text: `↑${b.ahead}`, title: "领先上游提交数" }) : null,
          b.behind ? h("span", { class: "fw-behind", text: `↓${b.behind}`, title: "落后上游提交数" }) : null,
          h("span", { class: "fw-grow" }),
          h("span", { class: "fw-log-hash", text: b.hash.slice(0, 7) }),
        ])
        // 单击 = 看这个分支的日志（分支栏 → 日志栏的动线）；检出在右键菜单里
        row.onclick = () => {
          logBranch = b.name
          void loadLog(true)
        }
        row.onclick = () => {
          logBranch = b.name
          void loadLog(true)
        }
        row.oncontextmenu = (e) => {
          e.preventDefault()
          showMenu(e.clientX, e.clientY, [
            { label: "检出", icon: "check", disabled: b.current, onClick: () => void op("branch", { action: "checkout", name: b.name }, "已切换") },
            // 推送：远程分支没法再推（b.remote），故只给本地分支；领先上游时把数字写进标签，
            // 与分支行上的 ↑N 同一口径——不用点开就知道有没有东西要推。
            {
              label: b.ahead ? `推送（↑${b.ahead}）` : "推送",
              icon: "upload",
              disabled: b.remote || !hooks.writable() || !hooks.remoteEnabled(),
              onClick: () => void pushBranch(b, { x: e.clientX, y: e.clientY }),
            },
            { separator: true },
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
    colRefs.replaceChildren(toolbar, list)
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
    colRefs.replaceChildren(toolbar, list)
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
    colRefs.replaceChildren(toolbar, list)
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
    colRefs.replaceChildren(toolbar, actions, list)
  }

  /* ------------------------------ 主流程 ------------------------------ */

  /** 刷新中的 promise：同刻重复刷新合并（切根 + 保存 + F5 常在同一拍里触发）。 */
  let refreshing: Promise<void> | null = null

  function refresh(): Promise<void> {
    if (refreshing) return refreshing
    const p = doRefresh().finally(() => {
      if (refreshing === p) refreshing = null
    })
    refreshing = p
    return p
  }

  async function doRefresh(): Promise<void> {
    renderTitleBar()
    renderRefsTabs()
    applyCols()
    const s = hooks.status()
    if (!s?.isRepo) {
      colRefs.replaceChildren(renderNotRepo())
      colLog.replaceChildren()
      colCommit.replaceChildren()
      return
    }
    // 三栏各自渲染（并排常显，不互相覆盖）
    if (refsTab === "branches") await loadBranches()
    else if (refsTab === "tags") await loadTags()
    else if (refsTab === "stash") await loadStash()
    else await loadRemotes()
    // 日志按需加载：首次展开或显式刷新时拉一页，之后由「加载更多」续（避免每次刷新都全量重拉）
    if (view === "log" || !logItems.length) await loadLog(logItems.length === 0)
    else renderLog()
  }

  /*
   * 日志「滚动到底自动加载」：监听**真正的滚动容器** colLog（`.fw-git-col-body`）。
   * 早期把 onscroll 挂在 `.fw-log-list` 上，而该元素没有 overflow（滚动在父级 colLog 上）——
   * 非滚动元素不产生 scroll 事件，自动加载实际是死代码（只剩「加载更多」按钮）。
   * 容器常驻，挂一次即可；只在日志视图与有待续页时响应。
   */
  colLog.addEventListener(
    "scroll",
    () => {
      if (view !== "log" || logLoading || !logHasMore) return
      if (colLog.scrollTop + colLog.clientHeight > colLog.scrollHeight - 60) void loadLog(false)
    },
    { passive: true },
  )

  bindColResizer(sp1, "a")
  bindColResizer(sp2, "c")

  return { el, refresh, show, view: () => view }
}

/** 差异端点对（A=原侧 / B=改侧）。 */
export interface DiffEndpoints {
  /** 取**内容**用的端点（gitContent：WORKTREE / INDEX / 任意 rev） */
  originalRef: string
  modifiedRef: string
  /**
   * 取**变更文件清单**用的参数（gitCompare）。
   *
   * 与内容端点的约定**不同**，别混用：内容端点里 INDEX 是「暂存区内容」，
   * 而 compare 的端点语义是一张表（见服务端 diffArgs）——
   * 「未暂存」要表达成 from 空、to=WORKTREE（`git diff`），
   * 写成 from=INDEX&to=WORKTREE 会被解释成 `--cached`（HEAD↔暂存区），拿到的清单是错的
   * （不报错，只是静默返回别的文件集合，所以这里必须显式分开写）。
   */
  compare: { from?: string; to?: string; mergeBase: boolean }
  /** 人类可读的来源说明，如「暂存区 ↔ 工作区」 */
  note: string
  labelA: string
  labelB: string
}

/**
 * 由 DiffSpec 推导端点对——**只此一份**，三处共用：
 * ① 差异视图取两侧文本；② 变更文件清单（上一个/下一个变更文件）；③ 工具条上的 A/B 说明。
 * 各写一份很容易出现「视图按 A...B 取、清单却按 A..B 取」这类对不上的偏差。
 */
export function diffEndpointsFor(spec: DiffSpec): DiffEndpoints {
  const { source } = spec
  if (source.type === "worktree") {
    return source.staged
      ? {
          originalRef: "HEAD",
          modifiedRef: INDEX_REF,
          compare: { to: INDEX_REF, mergeBase: false }, // 空 → INDEX == git diff --cached
          note: "HEAD ↔ 暂存区",
          labelA: "HEAD",
          labelB: "暂存区",
        }
      : {
          originalRef: INDEX_REF,
          modifiedRef: WORKTREE_REF,
          compare: { to: WORKTREE_REF, mergeBase: false }, // 空 → WORKTREE == git diff（索引↔工作区）
          note: "暂存区 ↔ 工作区",
          labelA: "暂存区",
          labelB: "工作区",
        }
  }
  if (source.type === "commit") {
    const short = source.hash.slice(0, 8)
    return {
      originalRef: `${source.hash}^`,
      modifiedRef: source.hash,
      compare: { from: `${source.hash}^`, to: source.hash, mergeBase: false },
      note: `${short} 本次提交`,
      labelA: `${short}^`,
      labelB: short,
    }
  }
  const mb = !!source.mergeBase
  return {
    originalRef: source.from,
    modifiedRef: source.to,
    compare: { from: source.from, to: source.to, mergeBase: mb },
    note: source.label ?? (mb ? `${source.from}...${source.to}（共同祖先）` : `${source.from} ↔ ${source.to}`),
    labelA: mb ? "共同祖先" : source.from,
    labelB: source.to,
  }
}
/**
 * 并列差异视图（主区域内容）：解析两侧文本 → Monaco diff；失败回退结构化 hunks。
 *
 * 返回差异块导航句柄（`nav`）：工具条上的「上一处/下一处差异」按钮与全局快捷键（F7/Shift+F7）
 * 都走它；结构化 hunks 降级渲染与降级编辑器一样没有导航（返回 null）。
 */
export interface DiffViewHandle {
  dispose: () => void
  nav: DiffNav | null
}

export async function mountDiffView(
  host: HTMLElement,
  api: FsApi,
  spec: DiffSpec,
  ctx: { repoRootPath: string; language: string },
): Promise<DiffViewHandle> {
  const { root, path, source } = spec
  /** 取某端点下的文件内容（WORKTREE / INDEX / rev 统一入口）。 */
  const side = async (ref: string): Promise<{ text: string; tooLarge: boolean; missing: boolean; size: number }> => {
    const res = await api.gitContent(root, ref, path)
    if (res.binary) return { text: "（二进制文件，无法按文本显示）", tooLarge: false, missing: false, size: res.size }
    // 超体量上限：服务端没拉正文（tooLarge），此时**不能**继续建 Monaco model——
    // 这正是「点开大文件变成卡死/空白」的源头；改走结构化 hunks + 一行说明
    if (res.tooLarge) return { text: "", tooLarge: true, missing: false, size: res.size }
    return { text: res.content, tooLarge: false, missing: !!res.missing, size: res.size }
  }

  /**
   * 结构化 hunks 降级渲染（Monaco 不可用 / 两侧超体量 / Monaco 抛错时走它）。
   *
   * `headNote` 用来把「为什么不是并列 Monaco」说清楚——降级本身不可怕，
   * **不说一声的降级**才可怕（用户会以为文件就是这样/没改动）。
   */
  const renderHunks = (fallback: GitFileDiff | null | undefined, headNote?: string): DiffViewHandle => {
    const wrap = h("div", { class: "fw-hunks" })
    if (headNote) wrap.appendChild(h("div", { class: "fw-hint-bar" }, [icon("info", 13), h("span", { text: headNote })]))
    if (fallback?.truncated && !fallback.hunks.length) {
      wrap.appendChild(h("div", { class: "fw-empty", text: `逐行差异已省略：+${fallback.additions} / -${fallback.deletions} 行（超出体量上限）` }))
    } else if (fallback?.binary) {
      wrap.appendChild(h("div", { class: "fw-empty", text: "二进制文件差异，无法逐行显示" }))
    } else if (fallback?.hunks.length) {
      /*
       * 行数上限：单文件 hunks 的服务端字符上限（maxFileChars）能装下**数万行**，
       * 而每行是 4 个 span —— 十万级节点会让主线程停机数秒（“点开大 diff 就卡死”）。
       * 超出就只渲染前 MAX_LINES 行并明确告知行数，不让用户以为“差异就这么点”。
       */
      const MAX_LINES = 3000
      let rendered = 0
      let skipped = 0
      let full = false
      for (const hk of fallback.hunks) {
        if (full) {
          skipped += hk.lines.length
          continue
        }
        wrap.appendChild(h("div", { class: "fw-hunk-head", text: hk.header }))
        for (const line of hk.lines) {
          if (rendered >= MAX_LINES) {
            full = true
            skipped++
            continue
          }
          rendered++
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
      if (skipped) wrap.appendChild(h("div", { class: "fw-empty", text: `差异过大：仅渲染前 ${MAX_LINES} 行，另有 ${skipped} 行未显示（可下载查看完整差异）` }))
    } else {
      wrap.appendChild(h("div", { class: "fw-empty", text: headNote ? "无逐行差异可显示" : "该端点对下此文件无内容差异（可能只是重命名或权限变更）" }))
    }
    host.appendChild(wrap)
    return { dispose: () => wrap.remove(), nav: null }
  }

  /**
   * 结构化差异的**兜底取数**：spec.fallback 未必带（各调用点给不给不一致），
   * 拿不到就现取一份单文件差异——降级路径不该依赖调用点是否记得传参。
   */
  const fetchFallback = async (): Promise<GitFileDiff | null> => {
    if (spec.fallback) return spec.fallback
    try {
      const ep = diffEndpointsFor(spec)
      return await api.gitFileDiff(root, path, { from: ep.compare.from, to: ep.compare.to, mergeBase: ep.compare.mergeBase })
    } catch {
      return null
    }
  }

  try {
    const ep = diffEndpointsFor(spec)
    let originalRef = ep.originalRef
    // 三点语义（A...B）：先解析共同祖先，A 侧用祖先内容（与服务端 git diff A...B 一致）
    if (source.type === "range" && source.mergeBase) {
      const cmp = await api.gitCompare(root, { from: source.from, to: source.to, mergeBase: true, path }).catch(() => null)
      if (cmp?.mergeBaseOf) originalRef = cmp.mergeBaseOf
    }
    const [a, b] = await Promise.all([side(originalRef), side(ep.modifiedRef)])
    // 两侧都不存在：与其给两片空白（看起来像“坏了”），不如说清楚为什么
    if (a.missing && b.missing) {
      return renderHunks(await fetchFallback(), `两个端点下都不存在该文件（${ep.labelA} / ${ep.labelB}）——可能路径不对，或它在这段历史里被删除后又未重建`)
    }
    // 任一侧超体量 → 不建 Monaco model（见 side 的注释），直接降级
    if (a.tooLarge || b.tooLarge) {
      const who = [a.tooLarge ? `A（${ep.labelA}）${formatSize(a.size)}` : "", b.tooLarge ? `B（${ep.labelB}）${formatSize(b.size)}` : ""].filter(Boolean).join(" ｜ ")
      return renderHunks(await fetchFallback(), `${who} 超过体量上限，已降级为逐行差异（不建编辑器，避免卡死）`)
    }
    const wrap = h("div", { class: "fw-diff-wrap" }, [
      h("div", { class: "fw-viewer-bar" }, [
        h("span", { class: "fw-viewer-info", text: `${ep.note}` }),
        h("span", { class: "fw-viewer-spacer" }),
        // 只留信息不放按钮：导航按钮统一在标签栏（跨文件一组 + 文件内一组，见 main.ts）
        h("span", { class: "fw-hint", text: `A：${ep.labelA} ｜ B：${ep.labelB}` }),
      ]),
      (() => {
        const box = h("div", { class: "fw-diff-host" })
        return box
      })(),
    ])
    host.appendChild(wrap)
    const diffHost = wrap.querySelector(".fw-diff-host") as HTMLElement
    const handle = await createDiffEditor(diffHost, { original: a.text, modified: b.text, language: ctx.language })
    // 导航按钮由标签栏渲染（handle.nav 交给调用方）
    return { dispose: () => {
      handle.dispose()
      wrap.remove()
    }, nav: handle.nav ?? null }
  } catch (err) {
    // 回退：结构化 hunks（服务端已解析；没带就现取）
    return renderHunks(await fetchFallback(), `并列视图不可用（${(err as Error).message}），已降级为逐行差异`)
  }
}
