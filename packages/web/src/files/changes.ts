/**
 * 文件工作台 · 变更面板（左栏工具窗，与「资源管理器 / 搜索」互斥）。
 *
 * 为什么从底部 Git 工具窗里拆出来：IDEA 的「Commit」工具窗就在**左侧**，
 * 而"改了什么 / 提交"是编码时最频繁看的，放左侧随时可见；底部留给
 * 「分支 | 日志 | 提交内容」——那是**回顾历史**时才看的，两者节奏不同。
 *
 * 内容：头部一行（视野范围芯片 + 刷新）、按「冲突 / 已暂存 / 未暂存 / 未跟踪」分组的改动列表（行内动作
 * 按 VSCode 的改动列表：打开文件 / 放弃更改 / 暂存更改；逐块暂存与三向暂存编辑器收在右键菜单），底部常驻提交框（消息 + 修补 + 提交 / 推送）——**提交框的动作全在一行**，
 * 选项在左、按钮在右；多步操作（merge/rebase/cherry-pick）进行中时顶部出现「继续 / 跳过 / 中止」条。
 *
 * 视野范围：root 可能只是仓库的子目录（典型：会话工作区在项目仓库内）——
 * 默认只看该子目录内的改动（IDEA 的 Commit 窗同理），可一键切整仓库。
 */
import type { GitChange, GitStatusInfo } from "./api"
import type { DiffSpec } from "./git"
import { confirmDialog, h, icon, showMenu, toast } from "./ui"
import { btnIcon, createOpRunner, operationAction, renderNotRepo, type GitOpHooks } from "./git-shared"
import { rowMinWidth } from "./panel-width"
import { buildChangeTree, collectDirPaths, treeRows, type TreeRow } from "./changes-tree"
import { changesFingerprint, fingerprint } from "./refresh-guard"

export interface ChangesHooks extends GitOpHooks {
  /** 当前根在仓库内的相对前缀（root 指向仓库子目录时不为空） */
  repoPrefix: () => string
  /** 状态快照（由 main.ts 统一拉取，面板只读用） */
  status: () => GitStatusInfo | null
  /** 在主区域打开差异标签 */
  openDiff: (spec: DiffSpec) => void
  /** 打开文件（可定位行） */
  openFile: (root: string, path: string, line?: number) => void
  /**
   * 打开**仓库内任意位置**的文件（入参为仓库相对路径，即本面板全部条目的 `path` 坐标）。
   *
   * 为什么不直接用 `openFile(root, prefixPath(path))`：根可能只是仓库的子目录，而「整仓库」范围下
   * 的改动可能落在**根之外**——那种路径在根内根本无法表达（剥前缀 / 原样返回都会拼出一条 404 的路径）。
   * 由宿主按仓库根与实际根清单换算出真正能打开它的根（见 `files/repo-paths.ts`）。
   */
  openRepoFile: (repoRel: string) => void
  /** 在资源管理器中定位该文件（跨根时由宿主换根再展开；与「打开文件」不是一件事） */
  revealRepoInExplorer: (repoRel: string) => void
  /** 打开比较标签 */
  openCompare: (init?: { from?: string; to?: string; path?: string; mergeBase?: boolean }) => void
  /** 打开冲突合并标签（三窗格） */
  openMerge: (repoRel: string) => void
  /** 打开三向暂存编辑器（HEAD ｜ 暂存结果 ｜ 工作区） */
  openStage: (repoRel: string) => void
  /** 是否可以写 */
  writable: () => boolean
  /** 远程操作是否可用 */
  remoteEnabled: () => boolean
  /** 文件系统变更后通知（树/状态栏刷新） */
  onFsChanged: () => void
  /** 打开某文件的 Git 历史（入参为**仓库相对**路径：git 侧统一用仓库坐标） */
  openFileHistory: (repoRel: string) => void
  /** 在 Git 工具窗的日志栏按该文件过滤（入参为**仓库相对**路径；宿主管工具窗的展开） */
  showInLog: (repoRel: string) => void
  /** 改动总数变化（rail 上的「变更」按钮徽标） */
  onCount: (n: number) => void
  /**
   * 提交框动作行所需的最小宽度（左栏不得比它窄）。
   * 每次渲染提交框后按实测值回调，宿主据此定左栏下限（CSS 变量 + 拖动夹取）——
   * 宽度是**渲染后**才有的事实，而宿主看不见面板内部何时重渲染，所以由面板推、不靠宿主拉。
   */
  onMinWidth: (px: number) => void
}

/** 改动列表的两种视图：平铺（按路径）与按目录收拢的树。 */
export type ChangesView = "list" | "tree"

export interface ChangesPanel {
  el: HTMLElement
  refresh: () => void
  dispose: () => void
}

const VIEW_KEY = "gebai.ui.changesView"

/** 视图记忆：未存过 / 存了不认识的值时都回列表（缺省列表）。 */
function readView(): ChangesView {
  try {
    return localStorage.getItem(VIEW_KEY) === "tree" ? "tree" : "list"
  } catch {
    return "list"
  }
}

function saveView(v: ChangesView): void {
  try {
    localStorage.setItem(VIEW_KEY, v)
  } catch {
    /* 隐私模式忽略 */
  }
}

export function createChangesPanel(hooks: ChangesHooks): ChangesPanel {
  let showWholeRepo = false
  /** 列表 / 树：缺省列表（逐条改动看状态与名字最直接），首次切换后记住选择。 */
  let view: ChangesView = readView()
  /** 树视图里已折叠的目录（键 = 分组 + 目录路径；不持久化，刷新即展开）。 */
  const collapsedDirs = new Set<string>()
  /** 已折叠的分组（键 = 分组名）：跨重绘保留——刷新一下就把分组展开回去等于没记住用户的操作。 */
  const collapsedGroups = new Set<string>()
  /** 上次渲染的数据指纹（见 `panelKey`）：相同就什么都不做（自动刷新的静默）。 */
  let renderedKey = ""
  /* 下面三个都在提交框创建时赋值（提交框在下面那行创建），因此必须先声明：提前到调用点之前。 */
  /** 待测的一对节点（提交框与它的动作行）：量「动作行要多宽」用，见 reportMinWidth。 */
  let minWidthProbe: { box: HTMLElement; actions: HTMLElement } | null = null
  /** 提交信息输入区（提交后由调用方显式清空——节点常驻，不会再靠重建来“重置”）。 */
  let commitArea!: HTMLTextAreaElement
  /** 提交框的定点更新（与状态有关的那几处；见 createCommitBox）。 */
  let updateCommitBox: () => void = () => {}
  let commitMessage = ""
  let commitAmend = false
  let committing = false
  /** 未完成的编辑历史（计划落在仓库里，面板只在值变化时重渲染，否则会自激循环）。 */
  let historyPlan: { branch: string; index: number; steps: unknown[] } | null = null
  let historyPlanLoading = false

  function resetHistoryPlan(): void {
    historyPlan = null
  }

  async function syncHistoryPlan(): Promise<void> {
    if (historyPlanLoading || !hooks.status()?.isRepo) return
    historyPlanLoading = true
    try {
      const res = await hooks.api.gitHistoryEditPlan(hooks.root())
      const next = res.plan ?? null
      const changed = JSON.stringify(next) !== JSON.stringify(historyPlan)
      historyPlan = next
      if (changed) renderNow()
    } catch {
      // 读不到计划不影响主流程（例如刚切到非仓库根）
    } finally {
      historyPlanLoading = false
    }
  }

  /** 提交信息跨刷新保留（提交框**常驻**，不再随每次渲染重建——见 createCommitBox）。 */
  const el = h("div", { class: "fw-changes-panel" })
  const listHost = h("div", { class: "fw-changes-list" })
  const listEl = h("div", { class: "fw-git-list" })
  const commitBox = createCommitBox()
  updateCommitBox()
  const op = createOpRunner(hooks, async () => {
    renderNow()
  })

  /* ------------------------------ 头部（一行控件） ------------------------------
   * 头部行高 34px（与资源管理器头部、编辑器标签栏同高：左栏首行与标签栏本是同一条横线），
   * 放三件东西：**视野范围**（当前目录 / 整仓库，是本面板唯一的范围开关）、
   * **视图**（树 / 列表，两个图标按钮互斥）、**刷新**。
   * 刷新是必需的：状态由宿主统一拉，但“我改完文件想立刻看结果”的预期落在面板自己的按钮上——
   * 它直接重取状态并重渲染（与 F5 同一条路），不依赖宿主的下一次刷新时机。
   * ---------------------------------------------------------------------------- */
  const scopeChip = h("button", { class: "fw-chip fw-scope-chip", hidden: true })
  scopeChip.onclick = () => {
    showWholeRepo = !showWholeRepo
    renderNow()
  }
  /**
   * 视图切换：**一个按钮，点击在两个视图之间切**。
   *
   * 不用一对互斥按钮：那两个按钮里总有一个是“当前状态”（不可点），而一个开关按钮
   * 每次点击都有意义。图标显示**当前视图**（与标题一致），title 说清点下去会变成什么。
   */
  const viewBtn = h("button", { class: "fw-icon-btn sm" })
  viewBtn.onclick = () => setView(view === "list" ? "tree" : "list")
  viewBtn.appendChild(icon("listView", 14))
  /** 同步按钮的图标与文案（图标 = 当前视图，title = 点击后的视图）。 */
  function syncViewButton(): void {
    const next = view === "list" ? "树视图" : "列表视图"
    viewBtn.title = view === "list" ? "当前：列表（按路径平铺）——点击切到树视图" : "当前：树（按目录收拢）——点击切到列表视图"
    viewBtn.setAttribute("aria-label", `切换到${next}`)
    viewBtn.replaceChildren(icon(view === "list" ? "listView" : "treeView", 14))
  }
  syncViewButton()
  const refreshBtn = h("button", { class: "fw-icon-btn sm", title: "刷新改动列表（F5）" })
  refreshBtn.appendChild(icon("refresh", 14))
  refreshBtn.onclick = () => void hooks.refreshStatus()
  const headHost = h("div", { class: "fw-changes-head" }, [scopeChip, h("span", { class: "fw-grow" }), viewBtn, refreshBtn])
  el.appendChild(headHost)

  /**
   * 切视图（列表 ⇄ 树）+ 重渲染。
   * 带**滚动位置**：两个视图看的是同一批改动，切过去回到开头等于把“我看到哪了”扔掉——
   * 列表滚动容器每次渲染都会重建，所以自己记下来再写回去。
   */
  function setView(v: ChangesView): void {
    if (v === view) return
    view = v
    saveView(v)
    syncViewButton()
      const prevTop = el.querySelector<HTMLElement>(".fw-git-list")?.scrollTop ?? 0
  renderNow()
    const next = el.querySelector<HTMLElement>(".fw-git-list")
    if (next) next.scrollTop = prevTop
  }

  /** 同步头部里的范围芯片（无前缀 = 根就是仓库根，没有“当前目录”这回事，芯片整块不显示）。 */
  function renderScopeChip(): void {
    const prefix = hooks.repoPrefix()
    scopeChip.hidden = !prefix
    if (!prefix) {
      scopeChip.replaceChildren()
      return
    }
    scopeChip.title = `当前限定：${prefix}（点击${showWholeRepo ? "仅看当前目录" : "看整仓库"}）`
    scopeChip.replaceChildren(
      icon(showWholeRepo ? "git" : "folder", 12),
      h("span", { text: showWholeRepo ? "整仓库" : `仅当前目录：${prefix.split("/").pop() ?? prefix}` }),
    )
  }

  /** 当前目录范围内的改动（root 为仓库子目录时；showWholeRepo 开启后为整仓库）。 */
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

  /**
   * 放弃更改（**可一次多个路径**：树视图的目录行就是拿它做「放弃整支」）。
   *
   * 工作区不可回退：服务端不自动建 stash 备份——「储存」是用户的显式动作（见 git 面板「储存」栏），
   * 所以确认框必须把话说明白（几条 / 哪些 / 不可恢复）。未跟踪走删除，其余走「恢复到版本库版本」，
   * 这两种语义在同一次调用里各自对应各自的路径（服务端按 `git status` 自行分流，混在一批也没关系）。
   */
  async function discard(paths: string[], opts: { untracked?: boolean; dir?: string } = {}): Promise<void> {
    const n = paths.length
    if (!n) return
    const one = n === 1 ? paths[0]! : ""
    const where = opts.dir ? `「${opts.dir}/」下的 ` : ""
    const ok = await confirmDialog({
      title: opts.untracked ? "删除未跟踪文件" : "放弃更改",
      message: opts.untracked
        ? n === 1
          ? `删除未跟踪文件「${one}」？\n该文件不在 Git 版本控制里，删除后无法恢复。`
          : `删除${where}${n} 个未跟踪文件？\n这些文件不在 Git 版本控制里，删除后无法恢复。`
        : n === 1
          ? `放弃「${one}」的更改？\n工作区会恢复到版本库的版本，此操作不可恢复。`
          : `放弃${where}${n} 个文件的更改？\n工作区会恢复到版本库的版本，此操作不可恢复。`,
      okText: opts.untracked ? "删除" : "放弃",
      danger: true,
    })
    if (!ok) return
    const okMsg = opts.untracked ? (n === 1 ? "已删除" : `已删除 ${n} 个文件`) : n === 1 ? "已放弃更改" : `已放弃 ${n} 个文件的更改`
    // backup:false —— 不自动创建 stash：丢弃就是丢弃（要留存改动请在「储存」栏显式储存），
    // 否则每次放弃都会往储存清单里塞一条备份，把「储存」变成垃圾堆。
    await op("discard", { paths, backup: false }, okMsg)
    hooks.onFsChanged()
  }

  /** 单条目的放弃（文件行用）：包一层只为了调用点读起来直白。 */
  async function discardOne(path: string, opts: { untracked?: boolean } = {}): Promise<void> {
    await discard([path], opts)
  }

  /**
   * 一行改动。`depth`/`hideDir` 只对**树视图**有意义：
   * 树里目录已由目录行表达，行内不再重复写一遍目录前缀，宽度全留给文件名（缩进由 depth）。
   */
  function changeRow(c: GitChange, group: "staged" | "unstaged" | "untracked" | "conflicted", opts: { depth?: number; hideDir?: boolean } = {}): HTMLElement {
    const name = c.path.split("/").pop() ?? c.path
    const dir = c.path.slice(0, Math.max(0, c.path.length - name.length - 1))
    const mark = c.conflicted ? "!" : c.untracked ? "U" : c.kind === "added" ? "A" : c.kind === "deleted" ? "D" : c.kind === "renamed" ? "R" : c.staged && !c.unstaged ? "S" : "M"
    /** 文件是否还在工作区：已删除的条目没有文件可开（「打开文件」按钮与右键项都不给）。 */
    const canOpenFile = c.kind !== "deleted"
    /**
     * 打开文件本身（文件标签）。与「打开差异」是两件事，入口也分开——按钮开文件、点行看改动。
     * 路径交给宿主换算（条目说的是仓库坐标，宿主决定用哪个根打开它，含根之外的改动）。
     */
    const openFileNow = (): void => hooks.openRepoFile(c.path)
    /** 打开差异（工作区 ↔ 暂存区/HEAD）。 */
    const openDiffNow = (): void =>
      hooks.openDiff({
        title: `${name}（${group === "staged" ? "已暂存" : "工作区"}）`,
        root: hooks.root(),
        path: c.path,
        source: { type: "worktree", staged: group === "staged" },
      })
    /** 单击行的动作：未跟踪文件没有可比较的另一侧，退化为打开文件。 */
    const openTarget = (): void => (c.untracked ? openFileNow() : openDiffNow())
    const row = h("div", { class: `fw-change-row${c.conflicted ? " conflict" : ""}`, title: c.path }, [
      h("span", { class: `fw-change-mark ${mark}`, text: mark }),
      h("span", { class: "fw-change-path" }, [
        dir && !opts.hideDir ? h("span", { class: "fw-change-dir", text: `${dir}/` }) : null,
        h("span", { class: "fw-change-name", text: name }),
      ]),
      h("span", { class: "fw-grow" }),
      // 行内动作按 VSCode 的改动列表**从左到右**：打开文件 → 放弃更改 → 暂存更改（hover 才显形）。
      // **第一个按钮是「打开文件」（开文件本身），不是「打开差异」**——看改动是点整行的事（见 row.onclick），
      // 两者混在一个入口上会让人以为按钮只是"另一种看差异的方式"。
      // 逐块暂存与三向暂存编辑器不进条目：它们不是「一行一个动作」的粒度（前者要勾选、后者是独立编辑器），
      // 摆到行上既撑宽度又多一个误点，统一收进右键菜单。
      //
      // 按钮包在 .fw-change-actions 这一个 flex:none 的整体里：间距 2px（比行内 gap 的 5px 省一半），
      // 且栏拖到最窄时被牺牲的是文件名（省略号）而不是按钮（见 files.css 的 .fw-change-path 一节）。
      h("span", { class: "fw-change-actions" }, [
        canOpenFile ? btnIcon("file", "打开文件", openFileNow) : null,
        group === "conflicted" ? btnIcon("git", "打开冲突解决（三窗格合并）", () => hooks.openMerge(c.path)) : null,
        // 放弃更改：已暂存的条目不给（IDEA/VSCode 同口径：先取消暂存再谈放弃）；冲突文件也不给
        // （`git checkout --` 对 unmerged 文件报错，得先解决冲突或中止合并）
        group !== "staged" && !c.conflicted ? btnIcon("undo", c.untracked ? "删除未跟踪文件" : "放弃更改", () => void discardOne(c.path, { untracked: c.untracked }), "danger") : null,
        // 暂存 / 取消暂存（暂存 = 进 index，与「储存 = stash」是两件事，文案不混用）
        group === "staged"
          ? btnIcon("minus", "取消暂存", () => void op("unstage", { paths: [c.path] }, undefined, { silent: true }))
          : btnIcon("plus", "暂存更改", () => void op("stage", { paths: [c.path] }, undefined, { silent: true })),
      ]),
    ])
    row.onclick = (e) => {
      if ((e.target as HTMLElement).closest("button")) return
      // 单击开差异：变更列表的第一诉求是“我改了什么”
      openTarget()
    }
    row.ondblclick = () => {
      if (c.conflicted) hooks.openMerge(c.path)
      else if (c.untracked || c.kind === "added") openFileNow()
    }
    row.oncontextmenu = (e) => {
      e.preventDefault()
      showMenu(e.clientX, e.clientY, [
        // 「打开文件」与「打开差异」分成两项（与行内按钮、点整行三个入口语义一致）：
        // 未跟踪的文件没有 HEAD 侧可对比、已删除的文件没有工作区侧，两项各自按可行性隐藏。
        ...(canOpenFile ? [{ label: "打开文件", icon: "file", onClick: openFileNow }] : []),
        ...(c.untracked || c.kind === "deleted" ? [] : [{ label: "打开差异", icon: "diff", onClick: openDiffNow }]),
        ...(c.conflicted ? [{ label: "冲突解决（三窗格合并）", icon: "git", onClick: () => hooks.openMerge(c.path) }] : []),
        { separator: true },
        group === "staged"
          ? { label: "取消暂存", icon: "minus", onClick: () => void op("unstage", { paths: [c.path] }, undefined, { silent: true }) }
          : { label: "暂存更改", icon: "plus", onClick: () => void op("stage", { paths: [c.path] }, undefined, { silent: true }) },
        ...(c.untracked || c.conflicted
          ? []
          : [
              {
                label: group === "staged" ? "逐块取消暂存…" : "逐块暂存…",
                icon: "diff",
                onClick: () => openPartial(c.path, group === "staged" ? "staged" : "unstaged"),
              },
              {
                label: "三向暂存编辑器…（HEAD ｜ 暂存结果 ｜ 工作区）",
                icon: "git",
                onClick: () => hooks.openStage(c.path),
              },
            ]),
        {
          label: "与 HEAD 比较",
          icon: "diff",
          onClick: () =>
            hooks.openDiff({
              title: `${name} ↔ HEAD`,
              root: hooks.root(),
              path: c.path,
              source: { type: "range", from: "HEAD", to: "WORKTREE" },
            }),
        },
        { label: "文件历史（Git log --follow）", icon: "history", onClick: () => hooks.openFileHistory(c.path) },
        { label: "在日志栏中筛选该文件", icon: "history", onClick: () => hooks.showInLog(c.path) },
        { label: "与任一提交比较…", icon: "sync", onClick: () => hooks.openCompare({ from: "HEAD", to: "WORKTREE" }) },
        { separator: true },
        { label: "复制路径", icon: "copy", onClick: () => void navigator.clipboard.writeText(c.path).then(() => toast("已复制路径", "success")) },
        { label: "在资源管理器中定位", icon: "folder", onClick: () => hooks.revealRepoInExplorer(c.path) },
        ...(group !== "staged"
          ? [
              { separator: true },
              {
                label: c.untracked ? "删除未跟踪文件" : "放弃更改",
                icon: "undo",
                danger: true,
                disabled: c.conflicted,
                onClick: () => void discardOne(c.path, { untracked: c.untracked }),
              },
            ]
          : []),
      ])
    }
    // 树视图的缩进：层级写进 CSS 变量（值 0 不写，省得每行都带一个无意义的行内样式）
    if (opts.depth) row.style.setProperty("--fw-depth", String(opts.depth))
    return row
  }

  /**
   * 树视图的目录行（可点击折叠）：箭头 + 目录名 + 该目录下的改动数 + **整支动作**。
   *
   * 为什么目录行也要有动作按钮：树视图一收拢，一个目录就代表它下面那一批文件——
   * 想「先把 packages/web 这一支暂存了」时，逐行点文件名既慢又容易漏；目录行上的一键就是对着
   * **整棵子树**（`paths` 由 `collectDirPaths` 给出，含深层子目录里的改动）执行，与文件行同一套写流程。
   *
   * 按钮集合与**文件行同口径**（含两侧都缺省的情形），只是没有「打开文件」——
   * 目录没有可打开的文件，它的行内动作就是「展开/折叠」（点行即做，不需要按钮）：
   *   未暂存 / 未跟踪：**放弃更改**（未跟踪为「删除未跟踪文件」）+ 暂存更改；
   *   已暂存：只有取消暂存（与文件行一致：先取消暂存再谈放弃）；
   *   冲突：都不给（要先解决冲突）。
   *
   * 缩进同样走 `--fw-depth`，与文件行同一增量，层级才能对齐。
   */
  function dirRow(
    row: Extract<TreeRow<GitChange>, { kind: "dir" }>,
    groupKey: "conflicted" | "staged" | "unstaged" | "untracked",
    paths: string[],
  ): HTMLElement {
    const key = `${groupKey}/${row.path}`
    const n = paths.length
    const untracked = groupKey === "untracked"
    // 冲突组与空目录行不给动作：与文件行同一口径（冲突要先解决再谈暂存/放弃）
    const actionable = groupKey !== "conflicted" && n > 0
    // 放弃整支：确认框里带上目录名与实际条数——一次抹掉一批文件是不可逆动作，必须说清范围
    const discardBtn = !actionable || groupKey === "staged"
      ? null
      : btnIcon(
          "undo",
          untracked ? `删除该目录下 ${n} 个未跟踪文件` : `放弃该目录下的更改（${n} 个文件）`,
          () => void discard(paths, { untracked, dir: row.path }),
          "danger",
        )
    // 冲突组不给：与文件行同一口径（冲突要先解决再谈暂存），组头也不给
    const stageBtn =
      groupKey === "conflicted" || !n
        ? null
        : groupKey === "staged"
          ? btnIcon("minus", `取消暂存该目录（${n} 个文件）`, () => void op("unstage", { paths }, undefined, { silent: true }))
          : btnIcon("plus", `暂存该目录（${n} 个文件）`, () => void op("stage", { paths }, `已暂存 ${n} 个文件`, { silent: true }))
    const el = h("div", { class: `fw-change-dirrow${collapsedDirs.has(key) ? " collapsed" : ""}`, title: row.path }, [
      icon("chevronDown", 11),
      h("span", { class: "fw-change-dirname", text: row.name }),
      h("span", { class: "fw-change-count", text: String(row.fileCount) }),
      h("span", { class: "fw-grow" }),
      // 与文件行同一个容器类：hover 才显形（常态保持列表干净）、flex:none 不可压缩、触屏常显
      // 顺序对齐文件行（打开文件 → 放弃更改 → 暂存更改）：放弃在前、暂存在后，跨行读起来一致
      h("span", { class: "fw-change-actions" }, [discardBtn, stageBtn]),
    ])
    if (row.depth) el.style.setProperty("--fw-depth", String(row.depth))
    el.onclick = (e) => {
      // 点按钮不等于点行（否则「暂存」会顺带把目录折叠起来，还得再展开一次看结果）
      if ((e.target as HTMLElement).closest("button")) return
        if (collapsedDirs.has(key)) collapsedDirs.delete(key)
  else collapsedDirs.add(key)
  renderNow()
    }
    return el
  }

  /** 逐块暂存：打开差异标签并直接进「逐块操作」态（Monaco 差异仍可切回）。 */
  function openPartial(path: string, side: "staged" | "unstaged"): void {
    const name = path.split("/").pop() ?? path
    hooks.openDiff({
      title: `${name}（逐块${side === "staged" ? "取消暂存" : "暂存"}）`,
      root: hooks.root(),
      path,
      source: { type: "worktree", staged: side === "staged" },
      partial: true,
    })
  }

  /** 最近的提交信息：写提交信息时最常用的参考（IDEA 提交框右侧的时钟按钮）。 */
  async function showMessageHistory(e: MouseEvent, area: HTMLTextAreaElement): Promise<void> {
    try {
      const res = await hooks.api.gitLog(hooks.root(), { limit: 30 })
      const items = [...new Set(res.commits.map((c) => c.subject).filter(Boolean))]
      if (!items.length) {
        toast("还没有可作为参考的提交信息", "info")
        return
      }
      showMenu(
        e.clientX,
        e.clientY,
        items.map((s) => ({
          label: s.length > 70 ? `${s.slice(0, 70)}…` : s,
          onClick: () => {
            area.value = s
            commitMessage = s
          },
        })),
      )
    } catch (err) {
      toast(`读取提交信息历史失败：${(err as Error).message}`, "error", 8000)
    }
  }

  /**
   * 提交框：**只建一次**（`listHost` 里的常驻节点），与状态有关的部分交给 `updateCommitBox()` 定点更新。
   *
   * 为什么不随每次渲染重建（原先的做法）：自动刷新一旦重建"提交框，正在敲的提交信息会丢焦点与光标，
   * 输入法组合中的字更会直接断掉——“刷新是静默的”在这里的具体含义就是**不碰用户正在输入的节点**。
   */
  function createCommitBox(): HTMLElement {
    const area = h("textarea", { class: "fw-commit-msg", placeholder: "提交信息（Ctrl+Enter 提交）", rows: 3 })
    area.value = commitMessage
    commitArea = area
    area.oninput = () => {
      commitMessage = area.value
    }
    // 修补：栏窄（左栏 ~286px），文字缩到“修补”两字，完整语义与 --amend 写进 title
    const amendInput = h("input", { type: "checkbox" })
    const amendToggle = h("label", { class: "fw-check", title: "修补上次提交（git commit --amend）：不新建提交，改写 HEAD" }, [amendInput])
    amendInput.checked = commitAmend
    amendInput.onchange = () => {
      commitAmend = amendInput.checked
      updateCommitBox()
    }
    amendToggle.append(h("span", { text: "修补" }))

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
          push,
          setUpstream: push,
        })
        const pushRes = (res as { push?: { ok?: boolean; output?: string } }).push
        if (pushRes && pushRes.ok === false) {
          toast(`提交成功但推送失败：${pushRes.output?.slice(0, 300)}`, "error", 8000)
        } else {
          toast(push ? "已提交并推送" : "已提交", "success")
        }
        commitMessage = ""
        commitAmend = false
        commitArea.value = ""
        await hooks.refreshStatus()
        renderNow()
        hooks.onFsChanged()
      } catch (err) {
        toast(`提交失败：${(err as Error).message}`, "error", 8000)
      } finally {
        committing = false
        commitBtn.removeAttribute("disabled")
      }
    }

    // 文案随「修补」开关变（updateCommitBox 里定点改这一行，不重建按钮）
    const commitLabel = h("span", { text: commitAmend ? "修补提交" : "提交" })
    const commitBtn = h("button", { class: "fw-btn primary", title: "" }, [icon("check"), commitLabel])
    commitBtn.onclick = () => void doCommit(false)
    // 「推送」而非「提交并推送」：按钮组必须在**最窄左栏**（180px，内容宽 164）里完整排下——
    // “提交并推送”六字会把这一组撑到 185px，栏一拖窄主按钮就被顶出可视区（实测 286px 时就已经折行）。
    // 完整语义写进 title，图标与分支栏/远程栏的推送同一枚（upload）。
    const pushBtn = h("button", { class: "fw-btn", title: "提交并推送当前分支（未设置上游时自动发布）" }, [icon("upload"), h("span", { text: "推送" })])
    pushBtn.onclick = () => void doCommit(true)
    pushBtn.disabled = !hooks.remoteEnabled()

    area.onkeydown = (e) => {
      if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
        e.preventDefault()
        void doCommit(false)
      }
    }

    /* 一行到底：左 = 选项（修补 / 取历史信息），右 = 动作（推送 / 提交）
       ——两个按钮永远在同一行、靠右（`.fw-commit-btns` 是不可折的整体 + margin-left:auto）；
       栏宽不够时折在两组**之间**（正常不会发生：宿主把左栏下限定在**这一行的实测宽度**上，
       见下面的 reportMinWidth——两行只是字体/缩放异常时的兼底）。 */
    const opts = h("div", { class: "fw-commit-opts" }, [
      amendToggle,
      (() => {
        const b = h("button", { class: "fw-icon-btn", title: "最近的提交信息（点选即填入）" })
        b.appendChild(icon("history", 13))
        b.onclick = (e) => void showMessageHistory(e as MouseEvent, area)
        return b
      })(),
    ])
    const btns = h("div", { class: "fw-commit-btns" }, [pushBtn, commitBtn])
    const actions = h("div", { class: "fw-commit-actions" }, [opts, btns])
    const box = h("div", { class: "fw-commit-box" }, [area, actions])
    // 待测的两个节点：此刻 box 还没进文档（调用方拿到后才会挂上去），量出来是 0
    minWidthProbe = { box, actions }
    /* 与状态有关的那几处（其余都是固定结构）：刷新时就地更新，不重建节点、不动正在输入的内容。 */
    updateCommitBox = (): void => {
      const stagedCount = scopedChanges().staged
      commitBtn.title = stagedCount ? `${stagedCount} 个文件已暂存` : "没有已暂存的变更（将提交工作区全部改动）"
      commitLabel.textContent = commitAmend ? "修补提交" : "提交"
      amendInput.checked = commitAmend
      pushBtn.disabled = !hooks.remoteEnabled()
    }
    return box
  }

  /**
   * 把「提交框动作行要多宽」报给宿主（左栏下限）。
   *
   * 为什么不在 CSS 里用 `min-width: max-content` 之类代替：左栏宽度是**拖动设定**的，
   * CSS 下限只能阻止它变窄，不能把已有宽度顶回来；而且它还要参与拖动的夹取（不然拖到一半卡住），
   * 那一步在 JS 里。
   *
   * 量的是两个**不可折组**的自身宽度（它们 `flex: none`，量到的是“需要多宽”而非“被压成多宽”），
   * 加上间隙与容器内边距。必须在节点**进文档之后**量（否则是 0），面板隐藏（display: none）
   * 或未挂载时量不到——那就不报，保持宿主上一次的值。
   */
  function reportMinWidth(): void {
    const p = minWidthProbe
    if (!p || !p.box.isConnected) return
    if (!p.box.getBoundingClientRect().width) return
    const cs = getComputedStyle(p.box)
    const aCs = getComputedStyle(p.actions)
    hooks.onMinWidth(
      rowMinWidth({
        widths: [...p.actions.children].map((k) => k.getBoundingClientRect().width),
        gaps: parseFloat(aCs.columnGap) || 0,
        paddingX: (parseFloat(cs.paddingLeft) || 0) + (parseFloat(cs.paddingRight) || 0),
      }),
    )
  }

  /** 冲突/多步操作横幅（merge/rebase/cherry-pick 进行中）。 */
  function renderOperationBanner(s: GitStatusInfo): HTMLElement {
    return h("div", { class: "fw-git-banner warn" }, [
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
        b.onclick = () => void op(operationAction(s.operation), {}, "已继续")
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
        b.onclick = () => void op(operationAction(s.operation), { abort: true }, "已中止")
        return b
      })(),
    ])
  }

  async function openConflicts(): Promise<void> {
    try {
      const res = await hooks.api.gitConflicts(hooks.root())
      if (!res.files.length) {
        toast("没有冲突文件", "info")
        return
      }
      const first = res.files[0]
      hooks.openMerge(first.path)
    } catch (err) {
      toast(`读取冲突失败：${(err as Error).message}`, "error")
    }
  }

  /** 未完成的「编辑历史」横幅（继续 / 中止）——状态里看不出来，故单独取一次计划。 */
  function renderHistoryBanner(): HTMLElement | null {
    const plan = historyPlan
    if (!plan) return null
    const box = h("div", { class: "fw-git-banner warn" }, [
      icon("history"),
      h("span", { text: `编辑历史进行中（${plan.branch}，已完成 ${plan.index}/${plan.steps.length} 条）` }),
      h("span", { class: "fw-grow" }),
    ])
    const cont = h("button", { class: "fw-btn sm primary", text: "继续" })
    cont.onclick = () =>
      void (async () => {
        try {
          const res = await hooks.api.gitHistoryEditContinue(hooks.root())
          if (res.halted === "conflict") toast(`仍有冲突未解决：${res.conflicts.join("、")}`, "error", 9000)
          else toast("历史已改写完成", "success")
        } catch (err) {
          toast(`继续失败：${(err as Error).message}`, "error", 9000)
        }
        resetHistoryPlan()
        hooks.refreshStatus()
        hooks.onFsChanged()
      })()
    const abort = h("button", { class: "fw-btn sm danger", text: "中止" })
    abort.onclick = () =>
      void (async () => {
        const ok = await confirmDialog({
          title: "中止编辑历史",
          message: `放弃本次改写，把「${plan.branch}」恢复到改写前？`,
          okText: "中止并恢复",
          danger: true,
        })
        if (!ok) return
        try {
          const res = await hooks.api.gitHistoryEditAbort(hooks.root())
          toast(`已恢复到 ${res.restored.slice(0, 8)}${res.backupBranch ? `（备份分支 ${res.backupBranch}）` : ""}`, "success", 6000)
        } catch (err) {
          toast(`中止失败：${(err as Error).message}`, "error", 9000)
        }
        resetHistoryPlan()
        hooks.refreshStatus()
        hooks.onFsChanged()
      })()
    box.append(cont, abort)
    return box
  }

  /**
   * 渲染指纹：本面板渲染所依赖的**全部**输入。
   *
   * 为什么列得比看起来需要的更全：漏一个的后果是「界面停在旧数据上」，而这类错误只在特定时序下出现、
   * 很难复现；多列几个至多多算一次指纹（几十次字符串拼接，微秒级）。
   */
  function panelKey(): string {
    const s = hooks.status()
    return fingerprint([
      s
        ? [
            s.isRepo,
            s.branch ?? "",
            !!s.detached,
            s.ahead ?? 0,
            s.behind ?? 0,
            s.operation ?? "",
            s.upstream ?? "",
            [s.counts.staged, s.counts.unstaged, s.counts.untracked, s.counts.conflicted],
          ]
        : null,
      changesFingerprint(s?.changes ?? []),
      hooks.repoPrefix(),
      view,
      showWholeRepo,
      [...collapsedDirs].sort(),
      [...collapsedGroups].sort(),
      historyPlan ? JSON.stringify(historyPlan) : "",
      hooks.writable(),
      hooks.remoteEnabled(),
      commitAmend,
    ])
  }

  /**
   * 公开刷新入口（宿主在每次 Git 状态到达时调用）：**数据没变就不重绘**（自动刷新的静默）。
   *
   * 面板自身状态变了（切视图 / 折叠分组 / 换范围 / 操作完成）走 `renderNow()` 直接重绘——
   * 那些都是一次性的用户动作或已知的本地状态变更，不必绕指纹判断。
   */
  function render(): void {
    /*
     * 编辑历史计划的状态不在指纹里（它由服务端持，值靠自比较触发重绘），所以这一行必须先跑：
     * 计划出现/消失/推进一步时要能自己发现，而不是等“别的数据也变了”才跟着刷新。
     */
    void syncHistoryPlan()
    const key = panelKey()
    if (key === renderedKey) return
    renderedKey = key
    renderNow()
  }

  function renderNow(): void {
    const s = hooks.status()
    const cnt = scopedChanges()
    const dirty = cnt.staged + cnt.unstaged + cnt.untracked + cnt.conflicted
    hooks.onCount(dirty)
    // 提交框常驻：只定点更新与状态有关的那几处（绝不重建节点，见 createCommitBox）
    updateCommitBox()
    if (!s?.isRepo) {
      // 非仓库时头部只剩刷新（范围芯片说的是“仓库内的当前目录”，此时无意义）
      scopeChip.hidden = true
      listHost.replaceChildren(renderNotRepo(hooks, op))
      return
    }
    const scoped = cnt.changes
    const groups: Array<{ key: "conflicted" | "staged" | "unstaged" | "untracked"; label: string; items: GitChange[] }> = [
      { key: "conflicted", label: "冲突", items: scoped.filter((c) => c.conflicted) },
      { key: "staged", label: "已暂存", items: scoped.filter((c) => c.staged) },
      { key: "unstaged", label: "未暂存", items: scoped.filter((c) => c.unstaged && !c.conflicted) },
      { key: "untracked", label: "未跟踪", items: scoped.filter((c) => c.untracked) },
    ]
    // 列表容器**常驻**（`.fw-git-list` 是滚动容器，也是 setView 记滚动位置的节点）：只换它的子节点
    const list = listEl
    const scrollTop = list.scrollTop
    list.replaceChildren()
    renderScopeChip()
    for (const g of groups) {
      if (!g.items.length) continue
      const groupEl = h("div", { class: "fw-change-group" })
      // 折叠态跨重绘保留（collapsedGroups），**箭头靠折叠类旋转**（转 → 向右）：图标节点不重建，
      // 只切类才能让过渡动画成立，也免得再写一套「换图标」的同步逻辑。
      const collapsed = collapsedGroups.has(g.key)
      const head = h("div", { class: `fw-change-group-head${collapsed ? " collapsed" : ""}` }, [
        icon("chevronDown", 12),
        h("span", { text: `${g.label}（${g.items.length}）` }),
        h("span", { class: "fw-grow" }),
        g.key !== "staged" && g.key !== "conflicted"
          ? btnIcon("check", "全部暂存", () => void op("stage", { paths: g.items.map((c) => c.path) }, "已全部暂存", { silent: true }))
          : null,
        g.key === "staged" ? btnIcon("undo", "全部取消暂存", () => void op("unstage", { paths: g.items.map((c) => c.path) }, undefined, { silent: true })) : null,
      ])
      const inner = h(
        "div",
        { class: `fw-change-group-body${collapsed ? " collapsed" : ""}` },
        view === "tree"
          ? (() => {
              // 目录行的一键动作用的是「这一支下的全部改动路径」：建树前先算一次（O(改动数 × 深度)），
              // 免得每个目录行各扫一遍整个分组
              const byDir = collectDirPaths(g.items)
              return treeRows(buildChangeTree(g.items), (p) => collapsedDirs.has(`${g.key}/${p}`)).map((r) =>
                r.kind === "dir" ? dirRow(r, g.key, byDir.get(r.path) ?? []) : changeRow(r.item, g.key, { depth: r.depth, hideDir: true }),
              )
            })()
          : g.items.map((c) => changeRow(c, g.key)),
      )
      head.onclick = (e) => {
        if ((e.target as HTMLElement).closest("button")) return
        if (collapsedGroups.has(g.key)) collapsedGroups.delete(g.key)
        else collapsedGroups.add(g.key)
        // 体与箭头同步切：只切 body 的话箭头一直是「展开」的样子（看不出组已收起）
        inner.classList.toggle("collapsed")
        head.classList.toggle("collapsed")
      }
      groupEl.append(head, inner)
      list.appendChild(groupEl)
    }
    if (!s.changes.length) list.appendChild(h("div", { class: "fw-empty", text: "工作区干净，没有未提交的变更" }))
    if (s.operation) list.prepend(renderOperationBanner(s))
    const historyBanner = renderHistoryBanner()
    if (historyBanner) list.prepend(historyBanner)
    // 提交框常驻：只在它不在位时整体挂一次（整体挂载会移动节点，正在输入时绝不能做）；
    // 顺序为「列表 → 提交框」（提交框 sticky 在滚动容器底部，必须排在末位）。
    if (listHost.firstChild !== list || list.nextSibling !== commitBox) listHost.replaceChildren(list, commitBox)
    list.scrollTop = scrollTop
    // 量下限：必须等节点进文档之后（上一步刚挂上），否则量出来是 0
    reportMinWidth()
  }

  el.appendChild(listHost)
  // 字体/缩放变化会改动作行宽度（系统缩放、浏览器缩放）：重报一次下限
  window.addEventListener("resize", () => reportMinWidth())
  return { el, refresh: render, dispose: () => el.remove() }
}
