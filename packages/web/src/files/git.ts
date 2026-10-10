/**
 * 文件工作台 · Git 工具窗（底部停靠，IDEA 式）：**分支 | 日志 | 提交内容** 三栏并排。
 *
 * 三条栏构成一条动线：点分支 → 看它的日志 → 点某条提交 → 看这条提交改了什么。
 * 分工上「变更（工作区改动 + 提交框）」不在这里——那是左栏的**变更面板**（changes.ts），
 * 因为"我改了什么、现在提交"是编码时随时要看的，而"历史"是回顾时才看的，节奏不同。
 *
 * 交互原则（对齐 IDEA 的 VCS 工具窗习惯）：
 * - 三栏边界可拖（宽度记忆在 localStorage，双击分界复位）；
 * - 日志分页加载、支持提交信息过滤与单文件历史（Git log --follow），点条目在右栏看内容；输入哈希串（4~40 位十六进制）即按提交 ID 精确定位，日志行右键也有「按此提交 ID 过滤」；
 * - 破坏性操作（重置 / 强制推送 / 分支删除）统一二次确认，并在有备份能力时提示
 *   （hard reset 自动建备份分支——见服务端 GitService）；
 * - 多步操作（merge/rebase/cherry-pick）的「继续 / 跳过 / 中止」在变更面板顶部（冲突属于工作区状态）。
 */
import type { FsApi, GitBranchInfo, GitCommitInfo, GitFileDiff, GitStatusInfo } from "./api"
import { h, icon, showMenu, toast, confirmDialog, promptDialog, clear, timeAgo, formatTime, formatSize, append } from "./ui"
import { btnIcon, createOpRunner, renderNotRepo as renderNotRepoShared } from "./git-shared"
import { createDiffEditor, type DiffNav } from "./editor"
import { createPartialPanel, type PartialPanelHandle } from "./partial"
import { graphEdgePath, layoutCommitGraph, type GraphGeometry, type GraphRow } from "./git-graph"
import { ALL_REFS, buildRefGroups } from "./git-refs"
import { COL_MIN_COMMIT, COL_MIN_REFS_FLOOR, clampColWidth, toolbarMinWidth } from "./git-cols"
import { openHistoryEditDialog } from "./history-edit"
import { fingerprint, listFingerprint, logFingerprint } from "./refresh-guard"

/** 外部可跳转的引用视图（三栏并排常显，故不含「变更」——工作区改动是左栏工具窗的职责）。 */
export type GitView = "log" | "branches" | "tags" | "stash" | "remotes"

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
  /** 直接以「逐块暂存」态打开（变更面板的「逐块暂存…」入口）；仅对工作区差异有意义 */
  partial?: boolean
}

export const WORKTREE_REF = "WORKTREE"
export const INDEX_REF = "INDEX"

/* ------------------------------ 提交图（日志栏左侧） ------------------------------
 * 泳道布局与连线几何在 files/git-graph.ts（纯逻辑、可单测）；这里只做像素换算与 SVG 落地。
 * 行高必须与 CSS 中 `.fw-log-row.graph` 的内容高度一致——跨行的连线靠它严丝合缝。
 * ------------------------------------------------------------------------------ */

/** 车道宽上限（车道多时按图列总宽等比收窄，线不丢、只是更密） */
const LANE_W = 14
/** 日志行内容高度（px），与 files.css 的 `.fw-log-row.graph` 对齐 */
const ROW_H = 44
/** 图列总宽上限：再宽就该给提交信息让位了 */
const GRAPH_MAX_W = 132
/** 车道配色：**不跟随主题令牌**——图形要靠多色区分并行分支，需要与主题无关的稳定色板 */
const GRAPH_PALETTE = ["#d9534f", "#4a8fe7", "#3fa96a", "#d2903f", "#9b6cd8", "#2fa3b5", "#d4609f", "#8b8b3d", "#6f7fd8", "#c96a4a"]

const SVG_NS = "http://www.w3.org/2000/svg"

/** 两组 refs 是否逐项一致（含顺序）——判「历史未变但引用变了」（打标签/建分支/切 HEAD 不改 hash）。 */
function sameRefs(a: string[], b: string[]): boolean {
  if (a.length !== b.length) return false
  return a.every((r, i) => r === b[i]!)
}

/** 单行提交图（连线 + 节点圆）：一行一个 SVG，宽度统一、高度固定，相邻行自然接成一张图。 */
function commitGraphSvg(row: GraphRow, geo: GraphGeometry & { width: number }): SVGSVGElement {
  const svg = document.createElementNS(SVG_NS, "svg")
  svg.setAttribute("class", "fw-graph-svg")
  svg.setAttribute("width", String(geo.width))
  svg.setAttribute("height", String(geo.rowH))
  svg.setAttribute("viewBox", `0 0 ${geo.width} ${geo.rowH}`)
  svg.setAttribute("aria-hidden", "true")
  for (const e of row.edges) {
    const path = document.createElementNS(SVG_NS, "path")
    path.setAttribute("d", graphEdgePath(e, geo))
    path.setAttribute("fill", "none")
    path.setAttribute("stroke-linecap", "round")
    path.style.stroke = GRAPH_PALETTE[e.color % GRAPH_PALETTE.length]!
    path.style.strokeWidth = "1.6"
    svg.appendChild(path)
  }
  // 合并提交画空心节点（与线性提交区分），颜色随所在车道
  const dot = document.createElementNS(SVG_NS, "circle")
  dot.setAttribute("cx", String(row.lane * geo.laneW + geo.laneW / 2))
  dot.setAttribute("cy", String(geo.rowH / 2))
  dot.setAttribute("r", row.merge ? "3.6" : "3")
  dot.setAttribute("fill", row.merge ? "none" : GRAPH_PALETTE[row.color % GRAPH_PALETTE.length]!)
  if (row.merge) {
    dot.style.stroke = GRAPH_PALETTE[row.color % GRAPH_PALETTE.length]!
    dot.style.strokeWidth = "1.8"
  }
  svg.appendChild(dot)
  return svg
}

/** 写操作动作 → 中文进度文案（标题栏在途提示用；未收录的动作直接显示动作名）。 */
const OP_LABELS: Record<string, string> = {
  fetch: "抓取远程",
  pull: "拉取",
  push: "推送",
  merge: "合并",
  rebase: "变基",
  "cherry-pick": "拣选提交",
  revert: "回滚提交",
  reset: "重置",
  commit: "提交",
  stash: "储存操作",
  branch: "分支操作",
  tag: "标签操作",
  remote: "远程配置",
  checkout: "检出",
  init: "初始化仓库",
  stage: "暂存更改",
  unstage: "取消暂存",
  discard: "放弃更改",
  ignore: "写入忽略规则",
}

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
  /**
   * Git 状态读取失败的原因（null = 未失败）。
   * 与「不是仓库」是两回事：读失败时没有可信状态，面板必须说清是故障，而不是给出「初始化仓库」这种误导入口。
   */
  statusError?: () => string | null
  /** 写操作在途通知（面板据此显示进行中并禁用并发入口）；缺省不通知。 */
  onBusy?: (action: string | null) => void
}

export interface GitPanel {
  el: HTMLElement
  refresh: () => Promise<void>
  /** 跳到某个引用视图（状态栏分支名、外部入口用）。 */
  show: (view: GitView) => void
  /** 日志栏按文件过滤（资源管理器/变更面板的「在日志中筛选」入口）。 */
  filterByPath: (path: string) => Promise<void>
}

export function createGitPanel(hooks: GitHooks): GitPanel {
  let logItems: GitCommitInfo[] = []
  let logHasMore = true
  let logLoading = false
  /** 上一次日志加载的失败信息（失败时列表区给出可重试的错误条，而不是伪装成「没有提交」）。 */
  let logError = ""
  /** 日志过滤：路径（单文件历史）/ 作者 / 提交信息关键字 / 提交 ID；各自独立清除。 */
let logFilterPath = ""
let logFilterAuthor = ""
let logFilterText = ""
/** 提交 ID 过滤（完整/短哈希）：服务端 `git log <rev> --max-count=1` 精确定位那一条；与提交信息过滤同框互斥。 */
let logFilterRev = ""
/** 时间范围：`since` 为 git 可识别的日期串（如 `7 days ago`），`sinceLabel` 只用于芯片展示。 */
let logSince = ""
let logSinceLabel = ""
/** 过滤模式：正则（--extended-regexp）与大小写不敏感（-i），对应 IDEA 搜索框右侧的 `.*` / `Cc`。 */
let logRegex = false
let logICase = false
  /** 上一次日志查询的过滤/范围条件（unchanged 短路只对「同一查询的刷新」成立：
   *  过滤变了结果集必然变，前缀相同不能当作未变——否则过滤后旧列表残留、过滤不生效）。 */
  let logQueryKey = ""
  /** 写操作在途的动作名（null = 空闲）：标题栏据此显示进度，远程动作按钮据此禁用。 */
  let busyAction: string | null = null
  let branches: GitBranchInfo[] = []
  let localTags: Array<{ name: string; hash: string; time?: number; subject?: string }> = []
  let stashes: Array<{ index: number; ref: string; message: string; time?: number }> = []
  let remotes: Array<{ name: string; fetchUrl: string; pushUrl: string }> = []

  /* ------------------------------ 工具窗骨架（IDEA 式） ------------------------------
   * 底部工具窗 = 标题栏 + 三栏并排：变更内容 / 日志 / 分支。
   * 为什么拆三栏而不是标签页：面板在**底部横向**展开，横向空间足够——变更、日志、分支
   * 是提交前后最常互相参照的三块信息，能同屏看到比来回切标签有用（IDEA 工具窗同理）。
   * 标签/储存/远程收进「分支」栏内的小切换（同属「引用与远程」语义，用得不频繁）。
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
  /** 「分支」栏内部的小切换（分支 / 标签 / 储存 / 远程）。 */
  let refsTab: "branches" | "tags" | "stash" | "remotes" = "branches"
  /** 日志当前限定的引用（分支栏/标签栏单击、日志栏范围选择器设置）；ALL_REFS = 全部分支（--all）。 */
  let logBranch: string = ALL_REFS
  /** 提交内容栏正在展示的提交（hash；空 = 无选中）：刷新后校验它是否还在日志里。 */
  let currentCommitHash = ""

  const colHead = (title: string, extra: Array<Node | null> = []): HTMLElement =>
    h("div", { class: "fw-git-col-head" }, [h("span", { class: "fw-git-col-title", text: title }), ...extra])

  const refsTabsHost = h("div", { class: "fw-git-refs-tabs" })
  /** 提交内容栏头部右侧的短哈希（**仅选中提交时出现**）。
   *  不再有「点击日志查看」这类空态引导文案——空态不需要教用户怎么用，那是噪声。 */
  const commitHint = h("span", { class: "fw-git-col-hint" })
  /** 提交内容栏头部的动作按钮区（选中提交后出现「与工作区比较 / 整提交差异」，随选中更新/复位）。 */
  const commitActions = h("span", { class: "fw-git-col-actions" })
  const colRefsEl = h("div", { class: "fw-git-col", "data-col": "refs" }, [h("div", { class: "fw-git-col-head" }, [refsTabsHost]), colRefs])
  /** 日志栏头部一行摆平：标题 + 过滤输入 + 生效芯片 + 刷新（logSearch/logChips 在日志视图一节补入）。 */
  const colLogHeadEl = h("div", { class: "fw-git-col-head" })
  const colLogEl = h("div", { class: "fw-git-col", "data-col": "log" }, [colLogHeadEl, colLog])
  /**
   * 面板右上角的关闭按钮。
   *
   * Git 面板没有常驻标题栏（常驻标题栏已降为按需状态带，见 renderTitleBar），关闭入口就搁在
   * **面板右上角**——即第三栏头部的末端（最后一栏的头部右缘就是面板的右上角），参与布局、不遮挡内容。
   * 活动栏按钮与 Alt+G 仍在，这只是“就地关闭”的那个入口。
   */
  const panelClose = btnIcon("close", "关闭 Git 面板（Alt+G）", () => hooks.close())
  const colCommitEl = h("div", { class: "fw-git-col", "data-col": "commit" }, [colHead("提交内容", [commitHint, commitActions, panelClose]), colCommit])

  // 分界可拖：宽度存 CSS 变量，三栏共享（拖动左界只改左栏、右界改中栏）
  const sp1 = h("div", { class: "fw-col-resizer", title: "拖动调整栏宽（双击复位；聚焦后 ←/→ 微调）" })
  const sp2 = h("div", { class: "fw-col-resizer", title: "拖动调整栏宽（双击复位；聚焦后 ←/→ 微调）" })
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
  /** 分支栏的宽度下限：**本栏工具条（按钮组）的实测宽度**，每次渲染重建工具条时重新量
   *  （见 syncRefsMinWidth）。默认取兜底值，首次渲染完成即被真实值取代。 */
  let colMinA = COL_MIN_REFS_FLOOR

  /** 把栏宽夹进可用范围（窗口变化后也要重新夹，否则固定 px 会把日志栏挤没）。 */
  function clampCol(w: number, which: "a" | "c"): number {
    return clampColWidth({
      want: w,
      min: which === "a" ? colMinA : COL_MIN_COMMIT,
      otherMin: which === "a" ? COL_MIN_COMMIT : colMinA,
      total: colsHost.getBoundingClientRect().width,
    })
  }

  /**
   * 分支栏的宽度下限 = 工具条里**全部可见子项**的固有宽度（工具条每次重建后调一次）。
   *
   * 为什么不写死：按钮随当前 tab 变（分支 / 标签 / 储存 / 远程），还会增减——写死的数字在加按钮后
   * 静默失效（栏被拖窄时按钮被裁掉半个，看不出是设计如此还是坏了）。**计数文本（「N 个标签 / N 条储存」）
   * 也算在内**：栏宽不够时先被省略的正是它，而只算出按钮组的宽度会让栏拖到「按钮与计数挤在一起」的宽度。
   * 唯一不计的是 `.fw-grow` 占位空白（它是弹性的，宽度不代表需要多宽）。
   */
  function syncRefsMinWidth(bar: HTMLElement): void {
    // 面板收起 / 切到终端时工具条是 display:none，各子项量出来是 0——那不是一个“需要多宽”，
    // 而是“量不到”。此时候保留上一次的值（面板重新展开会再量一遍：setDock 会 refresh）。
    if (!bar.getBoundingClientRect().width) return
    const cs = getComputedStyle(bar)
    const kids = [...bar.children] as HTMLElement[]
    colMinA = toolbarMinWidth({
      gap: parseFloat(cs.columnGap) || 0,
      paddingX: (parseFloat(cs.paddingLeft) || 0) + (parseFloat(cs.paddingRight) || 0),
      fixedWidths: kids.filter((k) => !k.classList.contains("fw-grow")).map((k) => k.getBoundingClientRect().width),
      slots: kids.length,
    })
    setVar(colsHost, "--git-col-a-min", `${colMinA}px`)
    // 下限变大后存着的宽度可能已不合规（如从「标签」切到「分支」）：就地夹一次，
    // 避免「界面上是 234、CSS 变量还是 204」——下次拖动读的是实际矩形，起点会跳。
    const saved = readCols()
    if (saved.a) setVar(colsHost, "--git-col-a", `${clampCol(saved.a, "a")}px`)
  }

  /**
   * 写 CSS 变量，**值相同就不写**。
   *
   * 为何在意：写同一个值也会产生一次 style 属性变更（MutationObserver 看得见）并触发样式重算，
   * 而 `applyCols` 挂在**每次** Git 刷新上——拖过分界的用户会每个心跳都白写一次。
   */
  function setVar(host: HTMLElement, name: string, value: string): void {
    if (host.style.getPropertyValue(name) === value) return
    host.style.setProperty(name, value)
  }

  function applyCols(): void {
    const w = readCols()
    if (w.a) setVar(colsHost, "--git-col-a", `${clampCol(w.a, "a")}px`)
    if (w.c) setVar(colsHost, "--git-col-c", `${clampCol(w.c, "c")}px`)
  }
  function bindColResizer(handle: HTMLElement, which: "a" | "c"): void {
    const varName = which === "a" ? "--git-col-a" : "--git-col-c"
    let dragging = false
    let startX = 0
    let startW = 0
    let pending: number | null = null
    let raf = 0
    /** 落盘该栏宽度（CSS 变量 → localStorage）。 */
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
    // 拖动的每一帧直接写 style 会与重排叠加（长拖掉帧）：与左栏/dock 拖拽一致用 rAF 合并
    const flush = (): void => {
      raf = 0
      if (pending === null) return
      colsHost.style.setProperty(varName, `${pending}px`)
      pending = null
    }
    // 键盘微调：分界条可聚焦，←/→ 按 8px（Shift 40px）调宽——全键盘可操作
    handle.setAttribute("role", "separator")
    handle.setAttribute("aria-orientation", "vertical")
    handle.tabIndex = 0
    handle.addEventListener("keydown", (e) => {
      if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return
      e.preventDefault()
      const step = (e.shiftKey ? 40 : 8) * (e.key === "ArrowRight" ? 1 : -1)
      const dir = which === "a" ? 1 : -1
      const cur = (which === "a" ? colRefsEl : colCommitEl).getBoundingClientRect().width
      colsHost.style.setProperty(varName, `${clampCol(cur + step * dir, which)}px`)
      persist()
    })
    handle.addEventListener("pointerdown", (e) => {
      dragging = true
      startX = e.clientX
      startW = (which === "a" ? colRefsEl : colCommitEl).getBoundingClientRect().width
      handle.classList.add("active")
      document.body.classList.add("fw-col-resizing")
      // 指针捕获：窗口外松手/指针离开面板也能收到 pointerup。
      // 否则 dragging 会卡在 true，之后鼠标一动就继续改宽度。
      handle.setPointerCapture(e.pointerId)
      e.preventDefault()
    })
    handle.addEventListener("pointermove", (e) => {
      if (!dragging) return
      const delta = which === "a" ? e.clientX - startX : startX - e.clientX
      pending = clampCol(startW + delta, which)
      if (!raf) raf = requestAnimationFrame(flush)
    })
    const finish = (): void => {
      if (!dragging) return
      dragging = false
      handle.classList.remove("active")
      document.body.classList.remove("fw-col-resizing")
      if (raf) cancelAnimationFrame(raf)
      flush()
      persist()
    }
    handle.addEventListener("pointerup", finish)
    handle.addEventListener("pointercancel", finish)
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

  // 窗口尺寸变化后重新夹一次已保存的栏宽（固定 px 在窄窗口下会把日志栏挤到看不见）
window.addEventListener("resize", () => applyCols())

  /** 上次渲染的「分支栏内小切换」标签（切了才重建那四个按钮）。 */
let refsTabsKey = ""

/**
 * 「分支」栏内部切换渲染（分支/标签/储存/远程）。
 * 自动刷新时它会被反复调到：内容没变（同一个 tab）就不碰 DOM。
 */
function renderRefsTabs(): void {
  if (refsTab === refsTabsKey) return
  refsTabsKey = refsTab
  clear(refsTabsHost)
    const labels: Record<string, string> = { branches: "分支", tags: "标签", stash: "储存", remotes: "远程" }
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

  /**
 * 面板标题栏：**只在有事要说时才出现**（无事则隐藏、不占一行）。
 *
 * 常驻标题栏里的每一件都有别的载体：面板身份＝活动栏高亮（「源代码管理」四字本身不是信息）、
 * 当前分支＝状态栏的分支项（以及分支列表的 ✓）、「比较」＝活动栏「更多」与 Ctrl+Shift+D、
 * 刷新＝各栏自己的刷新、关闭＝活动栏按钮 / Alt+G。**只有两件事别处说不了**：
 * 多步操作进行中（merge/rebase 与冲突数）与在途写操作（fetch/pull/push 耗时以秒计，无提示就只能靠猜）。
 * 于是这一行不再常驻，只在这两件事发生时亮出来。
 */
/** 上次渲染的标题栏指纹（它在空闲时是隐藏的，但每次自动刷新都会被清空重建）。 */
let titleBarKey = ""

function renderTitleBar(): void {
  const s = hooks.status()
  const key = fingerprint([s?.operation ?? "", s?.counts.conflicted ?? 0, busyAction ?? ""])
  if (key === titleBarKey) return
  titleBarKey = key
  clear(titleBar)
  const op = s?.operation
    ? h("span", { class: "fw-git-opbar" }, [h("span", { text: `${s.operation} 进行中${s.counts.conflicted ? `（${s.counts.conflicted} 个冲突）` : ""}` })])
    : null
  // 在途写操作：网络动作（fetch/pull/push）耗时以秒计，没有进度提示就只能靠猜
  const busy = busyAction
    ? h("span", { class: "fw-git-opbar busy", title: "正在执行 Git 操作" }, [icon("sync", 12), h("span", { text: `${OP_LABELS[busyAction] ?? busyAction}…` })])
    : null
  titleBar.hidden = !op && !busy
  if (titleBar.hidden) return
  append(titleBar, [icon("git", 14), op, busy, h("span", { class: "fw-grow" })])
}

  /**
   * 跳到某个视图：三栏并排常显，所以这里只切「分支栏内的小切换」并刷新。
   * 「日志」视图无需切栏（日志栏始终在位），只保证有一页数据。
   */
  function show(v: GitView): void {
    if (v === "tags" || v === "stash" || v === "remotes" || v === "branches") refsTab = v
    void refresh()
  }

  /**
   * 日志栏按文件过滤（单文件历史）。
   * 过滤时清掉分支范围：看某个文件的历史时再被分支范围裁一刀，多数时候只能得到空列表。
   */
  async function filterByPath(path: string): Promise<void> {
    logFilterPath = path
    logBranch = ALL_REFS
    renderLogRef()
    await loadLog(true)
  }

  /** 写操作（共享实现：toast + 刷新状态 + 刷新本面板）；在途期间通知标题栏显示进度。 */
  const op = createOpRunner(
    {
      ...hooks,
      onBusy: (action) => {
        busyAction = action
        renderTitleBar()
        applyRemoteBusy()
      },
    },
    async () => {
      await refresh()
    },
  )

  const renderNotRepo = (): HTMLElement => renderNotRepoShared(hooks, op)

  /** 状态读取失败态（与「不是仓库」区分）：说明是故障，并给一个重试入口。 */
  function renderStatusError(msg: string): HTMLElement {
    const box = h("div", { class: "fw-placeholder" }, [
      h("div", { class: "fw-placeholder-msg", text: "Git 状态读取失败" }),
      h("div", { class: "fw-placeholder-hint", text: msg }),
    ])
    const b = h("button", { class: "fw-btn sm" }, [icon("refresh"), h("span", { text: "重试" })])
    b.onclick = () => void hooks.refreshStatus().then(() => refresh())
    box.appendChild(h("div", { class: "fw-placeholder-actions" }, [b]))
    return box
  }

  /** 右栏（提交内容）的占位：说明这里会显示什么，而不是留一块空白。 */
  function renderCommitPlaceholder(text: string): HTMLElement {
    return h("div", { class: "fw-empty fw-commit-empty", text })
  }

  /** 提交内容栏头部复位：无选中提交时不残留上一个根的「与工作区比较」等动作，短哈希也一并清空。 */
  function resetCommitActions(): void {
    commitHint.textContent = ""
    clear(commitActions)
  }

  /** 刷新后兜底：正展示的提交已不在日志里（历史被重写 / 硬重置丢弃 / 换了过滤），收回到占位态。 */
  function resetCommitViewIfStale(): void {
    // 日志读取失败（logError）时列表被清空，不代表提交没了——保留详情，别误清
    if (logError) return
    if (currentCommitHash && logItems.some((c) => c.hash === currentCommitHash)) return
    currentCommitHash = ""
    resetCommitActions()
    colCommit.replaceChildren(renderCommitPlaceholder("点击「日志」中的提交，这里显示它对文件的改动"))
  }

  /** 写操作在途时禁用远程动作按钮（网络操作不该被连点两次）。 */
  function applyRemoteBusy(): void {
    const disabled = busyAction !== null || !hooks.remoteEnabled()
    for (const b of colRefs.querySelectorAll<HTMLButtonElement>(".fw-remote-actions button")) b.disabled = disabled
  }

  /* ------------------------------ 日志视图 ------------------------------ */

  /**
   * 日志栏的过滤控件**常驻**（挂在栏头部，与标题同一行）：输入框与芯片不随每次加载重建。
   * 每次重建的话，正在输入的过滤词与被聚焦的输入框会在一次后台刷新后一起消失（“打字打一半光标没了”）。
   */
  const logSearch = h("input", { class: "fw-input sm", placeholder: "按提交信息过滤…", title: "回车按提交信息过滤" })
/** `.*`：把过滤词按**扩展正则**解释（关闭即字面文本）；`Cc`：大小写不敏感（两者都直接下传给 `git log`）。 */
const logRegexBtn = h("button", { class: "fw-chip", title: "按正则解释过滤词（git log --extended-regexp；关闭时按字面文本）", text: ".*" })
const logICaseBtn = h("button", { class: "fw-chip", title: "忽略大小写（git log -i）", text: "Cc" })
/** 开关态：类名管视觉，`aria-pressed` 管语义（两者一起改，否则屏幕阅读器读到的还是「未按下」）。 */
function setToggle(btn: HTMLButtonElement, on: boolean): void {
  btn.classList.toggle("active", on)
  btn.setAttribute("aria-pressed", on ? "true" : "false")
}
setToggle(logRegexBtn, false)
setToggle(logICaseBtn, false)
logRegexBtn.onclick = () => {
  logRegex = !logRegex
  setToggle(logRegexBtn, logRegex)
  if (logFilterText) void loadLog(true)
}
logICaseBtn.onclick = () => {
  logICase = !logICase
  setToggle(logICaseBtn, logICase)
  if (logFilterText || logFilterAuthor) void loadLog(true)
}
/** 日期范围：预设 + 自定义（git 的 `--since` 自己认日期串，故直接传文本）。 */
const logDateBtn = h("button", { class: "fw-chip", title: "按时间范围过滤提交" }, [icon("history", 12), h("span", { text: "日期" })])
logDateBtn.onclick = (e) => {
  const at = e as MouseEvent
  const pick = (since: string, label: string) => () => {
    logSince = since
    logSinceLabel = label
    void loadLog(true)
  }
  showMenu(at.clientX, at.clientY, [
    { label: "不限时间", icon: "close", onClick: pick("", "") },
    { label: "今天", onClick: pick("midnight", "今天") },
    { label: "最近 7 天", onClick: pick("7 days ago", "最近 7 天") },
    { label: "最近 30 天", onClick: pick("30 days ago", "最近 30 天") },
    { label: "最近一年", onClick: pick("1 year ago", "最近一年") },
    {
      label: "自定义…",
      onClick: () =>
        void (async () => {
          const v = await promptDialog({ title: "时间范围", label: "起始时间", value: logSince, placeholder: "如 2024-01-01 或 3 weeks ago", hint: "直接交给 git log --since，支持绝对日期与相对描述" })
          if (v === null) return
          logSince = v.trim()
          logSinceLabel = logSince || ""
          void loadLog(true)
        })(),
    },
  ])
}
  const logChips = h("span", { class: "fw-git-chips" })
  const logList = h("div", { class: "fw-log-list" })
  /**
   * 过滤输入框 + 内置清除按钮（有内容时才出现）。
   * 清除 = 把过滤条件与输入框一并置空并重查：空着的输入框与「没有过滤」是同一件事，
   * 两者必须同步（否则会残留一个看不见的过滤条件，日志少了却不知道为何）。
   */
  const logSearchClear = h("button", { class: "fw-filter-clear", title: "清除过滤", "aria-label": "清除过滤" }, [icon("close", 11)])
  const logSearchBox = h("div", { class: "fw-filter-box" }, [logSearch, logSearchClear])
  /** 清除按钮的显隐：有内容才显示（空框里摆一个 × 是无意义动作）。 */
  const syncSearchClear = (): void => {
    logSearchClear.hidden = !logSearch.value
  }
  /** 哈希判定：4~40 位十六进制串（git 哈希长度 40 / SHA-256 仓44 64，取安全子集）——回车即切换为提交 ID 过滤。 */
  const HASH_RE = /^[0-9a-f]{4,40}$/i
  /**
   * 回车即执行过滤。同一个输入框承载两类互斥条件：
   *  · 哈希串 → **提交 ID 过滤**（精确到那一条，芯片展示并可单独清除——40 位哈希在输入框里看不全）；
   *  · 其余 → 提交信息关键字（不生成芯片：输入框本身就是这个条件的载体，再摆一个标签只占宽度）。
   */
  const applySearchFilter = (): void => {
    const v = logSearch.value.trim()
    logFilterRev = HASH_RE.test(v) ? v.toLowerCase() : ""
    logFilterText = logFilterRev ? "" : v
    void loadLog(true)
  }
  logSearch.onkeydown = (e) => {
    if (e.key !== "Enter") return
    applySearchFilter()
  }
  logSearch.oninput = syncSearchClear
  logSearchClear.onclick = () => {
    logSearch.value = ""
    syncSearchClear()
    applySearchFilter()
    logSearch.focus()
  }
  syncSearchClear()

  /* 日志范围选择器（栏头部常驻，IDEA 的 `Log: <branch>` 口径）：
   * 默认「全部分支」（--all），点开是带搜索的引用清单；已限定范围时按钮旁带一键清除。
   * 分支栏 / 标签栏的单击落进同一入口（setLogRef）——三处显示的永远是同一个范围。
   * ------------------------------------------------------------------------------ */
  const logRefHost = h("span", { class: "fw-log-ref" })
  const logRefBtn = h("button", { class: "fw-log-ref-btn", "aria-haspopup": "menu", "aria-expanded": "false" })
  const logRefClear = h("button", { class: "fw-log-ref-clear", title: "改为查看全部分支的日志" }, [icon("close", 11)])
  logRefBtn.onclick = () => toggleRefPicker()
  logRefClear.onclick = () => setLogRef(ALL_REFS)

  /** 切换日志范围（空 = 全部分支）：分支栏 / 标签栏 / 选择器三处共用同一入口。 */
  function setLogRef(ref: string): void {
    logBranch = ref
    renderLogRef()
    void loadLog(true)
  }

  /** 上次绘制的日志范围选择器 / 过滤芯片指纹（头部的两个小部件也不该在无变化时重建）。 */
  let logRefKey = ""
  let logChipsKey = ""

  /** 选择器外观：范围名 + 展开箭头；限定在某个引用上时附一键回到全部分支。 */
  function renderLogRef(): void {
    if (logBranch === logRefKey) return
    logRefKey = logBranch
    const scoped = !!logBranch
    logRefHost.classList.toggle("scoped", scoped)
    logRefBtn.replaceChildren(
      icon(scoped ? "branch" : "git", 12),
      h("span", { class: "fw-log-ref-label", text: scoped ? logBranch : "全部分支" }),
      icon("chevronDown", 11),
    )
    logRefBtn.title = scoped ? `日志范围：${logBranch}（点击更换）` : "日志范围：全部分支（--all；点击选择分支 / 标签）"
    logRefHost.replaceChildren(...(scoped ? [logRefBtn, logRefClear] : [logRefBtn]))
  }
  renderLogRef()

  /** 范围选择浮层（搜索 + 分组行）；同一时刻只开一个。 */
  let refPicker: { el: HTMLElement; close: () => void } | null = null
  /** 选择器用的引用清单拉到的时间（短时间内复用：每开一次就拉分支 + 标签两枪太费）。 */
  let refsAt = 0

  function toggleRefPicker(): void {
    if (refPicker) closeRefPicker()
    else openRefPicker()
  }

  function closeRefPicker(): void {
    refPicker?.close()
    refPicker = null
  }

  /**
   * 打开范围选择浮层（锚在按钮下方）。
   * 键盘：↑/↓ 移动高亮、Enter 选中、Esc 关闭——选择器在栏头部（不是模态），全键盘可操作。
   */
  function openRefPicker(): void {
    const pop = h("div", { class: "fw-log-ref-pop", role: "dialog", "aria-label": "选择日志范围" })
    const search = h("input", { class: "fw-input sm", placeholder: "搜索分支 / 标签…", "aria-label": "搜索分支 / 标签" })
    const list = h("div", { class: "fw-log-ref-list" })
    let rows: Array<{ el: HTMLButtonElement; value: string }> = []
    let cursor = -1

    const paint = (): void => {
      clear(list)
      rows = []
      cursor = -1
      for (const group of buildRefGroups({ branches, tags: localTags, query: search.value })) {
        list.appendChild(h("div", { class: "fw-ref-group", text: group.label }))
        for (const o of group.options) {
          const scoped = o.value === logBranch
          const row = h("button", { class: `fw-ref-row${scoped ? " active" : ""}`, title: o.detail ?? "" }, [
            icon(scoped ? "check" : o.icon, 13),
            h("span", { class: "fw-ref-label", text: o.label }),
            o.detail ? h("span", { class: "fw-ref-detail", text: o.detail }) : null,
          ])
          row.onclick = () => {
            closeRefPicker()
            setLogRef(o.value)
          }
          rows.push({ el: row, value: o.value })
          list.appendChild(row)
        }
      }
      if (!rows.length) list.appendChild(h("div", { class: "fw-empty", text: "没有匹配的分支 / 标签" }))
    }

    const highlight = (): void => rows.forEach((r, i) => r.el.classList.toggle("active", i === cursor || r.value === logBranch))
    const step = (delta: number): void => {
      if (!rows.length) return
      cursor = (cursor + delta + rows.length) % rows.length
      rows[cursor]!.el.scrollIntoView({ block: "nearest" })
      highlight()
    }

    search.oninput = () => paint()
    search.onkeydown = (e) => {
      if (e.key === "ArrowDown") {
        e.preventDefault()
        step(1)
      } else if (e.key === "ArrowUp") {
        e.preventDefault()
        step(-1)
      } else if (e.key === "Enter") {
        e.preventDefault()
        // 没动过方向键时：留着当前范围（或首行「全部分支」），不把一次误按变成换范围
        const pick = cursor >= 0 ? rows[cursor] : rows.find((r) => r.value === logBranch) ?? rows[0]
        if (pick) {
          closeRefPicker()
          setLogRef(pick.value)
        }
      } else if (e.key === "Escape") {
        e.preventDefault()
        closeRefPicker()
      }
    }

    /** 点浮层外关闭（含点回按钮：按钮自己会 toggle，不关掉就成开-关-开）。 */
    const onOutside = (e: PointerEvent): void => {
      const t = e.target as Node | null
      if (t && (pop.contains(t) || logRefHost.contains(t))) return
      closeRefPicker()
    }
    const close = (): void => {
      document.removeEventListener("pointerdown", onOutside, true)
      window.removeEventListener("resize", close)
      dockWatcher.disconnect()
      const hadFocus = pop.contains(document.activeElement)
      pop.remove()
      logRefBtn.setAttribute("aria-expanded", "false")
      if (refPicker?.el === pop) refPicker = null
      if (hadFocus) logRefBtn.focus()
    }
    // 工具窗被收起（活动栏切换、快捷键、窗口变窄）时同步关闭：浮层挂在 body 上，不随面板一起隐藏
    const dockWatcher = new MutationObserver(() => {
      if (el.classList.contains("fw-dock-hidden")) closeRefPicker()
    })
    dockWatcher.observe(el, { attributes: true, attributeFilter: ["class"] })

    pop.append(search, list)
    document.body.appendChild(pop)
    refPicker = { el: pop, close }
    const box = logRefBtn.getBoundingClientRect()
    pop.style.left = `${Math.max(8, Math.min(box.left, window.innerWidth - pop.offsetWidth - 8))}px`
    pop.style.top = `${box.bottom + 4}px`
    logRefBtn.setAttribute("aria-expanded", "true")
    document.addEventListener("pointerdown", onOutside, true)
    window.addEventListener("resize", close)
    paint()
    search.focus()
    // 打开时清单可能还没拉过（分支栏没打开过的根）：拉一次再重画
    void ensurePickerRefs().then(() => {
      if (refPicker?.el === pop) paint()
    })
  }

  /** 选择器用的引用清单（分支 + 标签）：取不到就用已有缓存，清单为空时浮层自己给空态。 */
  async function ensurePickerRefs(): Promise<void> {
    if (Date.now() - refsAt < 10_000) return
    const root = hooks.root()
    try {
      const [b, t] = await Promise.all([hooks.api.gitBranches(root), hooks.api.gitTags(root)])
      if (root !== hooks.root()) return
      branches = b.branches
      localTags = t.tags
      refsAt = Date.now()
    } catch {
      /* 静默：清单空时浮层给空态，不在这里弹错 */
    }
  }

      colLogHeadEl.append(
      h("span", { class: "fw-git-col-title", text: "日志" }),
      logRefHost,
      logSearchBox,
      logRegexBtn,
      logICaseBtn,
      logDateBtn,
      logChips,
      btnIcon("refresh", "刷新日志", () => void loadLog(true)),
    )
  colLog.replaceChildren(logList)

  /**
   * 生效中的过滤条件芯片：文件路径 / 作者 / 时间范围 / 提交 ID。
   *
   * 这几项的当前值在头部别处看不见（路径来自资源管理器右键、作者来自日志行右键、时间来自日期菜单、
   * 提交 ID 来自日志行右键或输入框），所以必须摆出来并自带清除。**提交信息过滤不在这里**——它的载体就是那个输入框
   * （写着什么就是在过滤什么），再摆一个标签只占宽度；清空输入框回车即取消。
   * 范围（分支 / 标签）同理不入芯片：头部选择器已经显示它并自带清除。
   */
  function renderLogChips(): void {
    /*
     * 无变化不重建：它挂在日志头部，每次自动刷新都会走到——重建会把芯片的 hover 与输入框的
     * 清除按钮抹一遍（输入框本体不重建，但芯片区一空一满仍是一次可见的抖动）。
     */
    const key = fingerprint([logFilterPath, logFilterAuthor, logSince, logSinceLabel, logFilterText, logFilterRev])
    if (key === logChipsKey) return
    logChipsKey = key
    clear(logChips)
    // 输入框的过滤态：生效中亮边框（去掉标签后，靠它表达「这个条件是生效的」）
    logSearch.classList.toggle("filtering", !!(logFilterText || logFilterRev))
    syncSearchClear()
    const chip = (iconName: string, label: string, title: string, onClear: () => void): HTMLElement => {
      const b = h("button", { class: "fw-chip", title }, [icon(iconName, 12), h("span", { text: label }), icon("close", 12)])
      b.onclick = onClear
      return b
    }
    if (logFilterPath) {
      logChips.appendChild(chip("file", logFilterPath, "清除文件过滤", () => {
        logFilterPath = ""
        void loadLog(true)
      }))
    }
    if (logFilterAuthor) {
      logChips.appendChild(chip("search", `作者 ${logFilterAuthor}`, "清除作者过滤", () => {
        logFilterAuthor = ""
        void loadLog(true)
      }))
    }
    if (logFilterRev) {
      logChips.appendChild(chip("git", `提交 ${logFilterRev.slice(0, 8)}`, `清除提交 ID 过滤（${logFilterRev}）`, () => {
        logFilterRev = ""
        logSearch.value = ""
        syncSearchClear()
        void loadLog(true)
      }))
    }
    if (logSince) {
      logChips.appendChild(chip("history", logSinceLabel || logSince, "清除时间范围", () => {
        logSince = ""
        logSinceLabel = ""
        void loadLog(true)
      }))
    }
  }

  /**
   * 加载日志页。
   *
   * `reset` = 从最新一页重来（刷新 / 过滤变更 / 切根），否则是「加载更多」续页。
   * 并发保护分两层：续页不做并发（同一游标重复请求只会拉回重复数据），
   * 而 reset 一律放行——否则切根时旧根的请求还在途，新根的首页会被直接丢弃，
   * 面板停在上一个根的数据上而看不出异常。
   * 结果落地前用代际号 + 根比对判活：过期响应直接丢弃。
   */
  let logGen = 0
  async function loadLog(reset = false): Promise<void> {
    if (!reset && logLoading) return
    const gen = ++logGen
    const root = hooks.root()
    const prev = logItems
    // 本次查询的过滤/范围条件：unchanged 短路的前提是「同一查询」（仅刷新），条件变了必须重建
    const queryKey = [logFilterPath, logFilterAuthor, logFilterText, logFilterRev, logBranch, logSince, String(logRegex), String(logICase)].join("\u0000")
    logLoading = true
    logError = ""
    if (reset) {
      logItems = []
      logHasMore = true
    }
    try {
      const res = await hooks.api.gitLog(root, {
        limit: 60,
        skip: reset ? 0 : logItems.length,
        path: logFilterPath || undefined,
        grep: logFilterText || undefined,
        author: logFilterAuthor || undefined,
        since: logSince || undefined,
        grepRegex: logRegex || undefined,
        grepIgnoreCase: logICase || undefined,
        rev: logFilterRev || undefined,
        // 点了分支就只看该分支的日志；否则看全部分支（--all）
        ref: logBranch || undefined,
        all: !logBranch,
      })
      if (gen !== logGen || root !== hooks.root()) return
      // 首页与已加载的前 N 条完全一致（刷新了但历史没变）：保留现有列表，
      // 免得每次 F5 / 提交后都把用户翻了几页的列表拽回第一页。
      // 两个前提缺一不可：
      //  · 过滤/范围条件未变（queryKey 相同）——条件变了结果集必然变（如过滤缩小），
      //    前缀相同不能当作未变，否则旧列表残留、过滤不生效；
      //  · 「完全一致」必须连 refs 一起比：打标签 / 建分支 / 切 HEAD 不产生新提交（hash 全同），
      //    但提交行上的分支/标签芯片已经变了——只比 hash 会把这些变化吞掉（刷新后标签不更新）。
      const unchanged =
        queryKey === logQueryKey &&
        reset &&
        res.commits.length > 0 &&
        prev.length >= res.commits.length &&
        res.commits.every((c, i) => prev[i]?.hash === c.hash && sameRefs(prev[i]!.refs, c.refs))
      if (unchanged) {
        // 回填原列表：reset 分支开头清空了 logItems（避免新旧混合），未变时得把它放回去，否则列表会变空
        logItems = prev
        logHasMore = logHasMore || res.hasMore
      } else {
        const base = reset ? [] : logItems
        const seen = new Set(base.map((c) => c.hash))
        // 去重（--all 下多分支有交集）
        for (const c of res.commits) if (!seen.has(c.hash)) { base.push(c); seen.add(c.hash) }
        logItems = base
        logHasMore = res.hasMore
      }
      // 本次查询条件已落地，后续同条件的 reset 才可能走 unchanged 短路
      logQueryKey = queryKey
    } catch (err) {
      if (gen === logGen) {
        logError = (err as Error).message
        logHasMore = false
      }
    } finally {
      if (gen === logGen) {
        logLoading = false
        renderLog()
        // 日志范围切换后同步引用栏高亮（"我正在看哪个分支/标签的日志"要看得出来）
        if (refsTab === "branches") renderBranches()
        else if (refsTab === "tags") renderTags()
      }
    }
  }

  /** 上次绘制的日志指纹（数据没变就不重建列表，见 renderLog）。 */
  let renderedLogKey = ""

  function renderLog(): void {
    renderLogRef()
    renderLogChips()
    /*
     * 静默判据：这份日志与上次画的是否一样。刷新（含自动刷新的每一次心跳）在数据未变时
     * 不该碰列表 DOM——整列重建即使还原了滚动位置，也会把 hover、键盘焦点与“正在看的提交”
     * 高亮全抖掉。相对时间（timeAgo）刻意不进指纹：那等于永远判定为“变了”（见 refresh-guard）。
     */
    const key = fingerprint([
      logFingerprint(logItems, {
        queryKey: logQueryKey,
        loading: logLoading,
        error: logError,
        hasMore: logHasMore,
        remoteNames: remotes.map((r) => r.name),
      }),
      currentCommitHash,
    ])
    if (key === renderedLogKey) return
    renderedLogKey = key
    // 重建列表前记下滚动位置：后台刷新（F5 / 提交后 / 写操作后）不该把正在看的提交滚走
    const scrollTop = colLog.scrollTop
    clear(logList)
    // 提交图：车道多时收窄车道宽（图列总宽封顶）；complete 决定「挂不到实处的线」留不留
    const graph = layoutCommitGraph(logItems, { complete: !logHasMore })
    const laneW = Math.max(7, Math.min(LANE_W, Math.floor(GRAPH_MAX_W / Math.max(1, graph.cols))))
    const geo = { laneW, rowH: ROW_H, width: Math.max(laneW, graph.cols * laneW) }
    logList.style.setProperty("--fw-graph-w", `${geo.width}px`)
    if (logError) {
      const bar = h("div", { class: "fw-error-bar" }, [icon("warning", 13), h("span", { text: `读取日志失败：${logError}` }), h("span", { class: "fw-grow" })])
      const retry = h("button", { class: "fw-btn sm", text: "重试" })
      retry.onclick = () => void loadLog(true)
      bar.appendChild(retry)
      logList.appendChild(bar)
    }
    if (!logItems.length && !logLoading) {
      const filtered = !!(logFilterPath || logFilterAuthor || logFilterText || logSince || logFilterRev)
      logList.appendChild(h("div", { class: "fw-empty", text: filtered ? "没有匹配的提交记录" : "暂无提交记录" }))
    }
    for (let i = 0; i < logItems.length; i++) {
      const c = logItems[i]!
      const graphRow = graph.rows[i]
      const graphCell = h("div", { class: "fw-log-graph" })
      if (graphRow) graphCell.appendChild(commitGraphSvg(graphRow, geo))
      const isHead = c.refs.some((r) => r.startsWith("HEAD"))
      const row = h(
        "div",
        { class: logRowClass(isHead, currentCommitHash === c.hash), "data-hash": c.hash, tabindex: "0", role: "button", "aria-label": `${c.short} ${c.subject}` },
        [
          graphCell,
          h("div", { class: "fw-log-main" }, [
            h("div", { class: "fw-log-subject", text: c.subject || "(无提交信息)", title: c.subject }),
            h("div", { class: "fw-log-meta" }, [
              h("span", { class: "fw-log-hash", text: c.short }),
              h("span", { text: c.author }),
              h("span", { text: timeAgo(c.commitTime) }),
              // 引用标签带语义色（黄=当前分支头 / 绿=本地分支 / 紫=远程 / 标签）——
              // 多分支同屏时靠颜色就能分清「这是本地工作还是别人推上来的」。
              // **只摆前几个**，其余并成「+N」（IDEA 同）：一个提交上挂十几条分支时，
              // 全铺出来会把标题行挤成半行、而且一眼看不出重点是哪个。
              ...c.refs.slice(0, 3).map((r) => refChip(r)),
              c.refs.length > 3 ? refChip(`+${c.refs.length - 3}`, c.refs.slice(3).join("、")) : null,
            ]),
          ]),
        ],
      )
      row.onclick = () => void openCommit(c)
      // 日志列表是面板的主要导航面：Enter/Space 与点击等价，否则键盘用户进不了提交详情
      row.onkeydown = (e) => {
        if (e.key !== "Enter" && e.key !== " ") return
        e.preventDefault()
        void openCommit(c)
      }
      row.oncontextmenu = (e) => {
        e.preventDefault()
        showMenu(e.clientX, e.clientY, [
          { label: "查看变更文件", icon: "diff", onClick: () => void openCommit(c) },
          { label: "此提交 ↔ 工作区（之后改了什么）", icon: "edit", onClick: () => hooks.openCompare({ from: c.hash, to: "WORKTREE" }) },
          { label: "此提交 ↔ 暂存区", icon: "archive", onClick: () => hooks.openCompare({ from: c.hash, to: "INDEX" }) },
          { label: "与当前 HEAD 比较", icon: "sync", onClick: () => hooks.openCompare({ from: c.hash, to: "HEAD" }) },
          { separator: true },
          { label: "检出此提交（分离 HEAD）", icon: "check", disabled: !hooks.writable(), onClick: () => void confirmer("检出提交", `检出 ${c.short}？将进入分离 HEAD 状态。`, () => op("checkout", { ref: c.hash, detach: true }, "已检出提交")) },
          { label: `只看 ${c.author} 的提交`, icon: "search", onClick: () => { logFilterAuthor = c.author; void loadLog(true) } },
          { label: "按此提交 ID 过滤", icon: "git", onClick: () => { logFilterRev = c.hash; logFilterText = ""; logSearch.value = c.hash; syncSearchClear(); void loadLog(true) } },
          { separator: true },
          { label: "复制提交哈希", icon: "copy", onClick: () => void navigator.clipboard.writeText(c.hash).then(() => toast("已复制哈希", "success")) },
          { label: "复制提交信息", icon: "copy", onClick: () => void navigator.clipboard.writeText(c.subject).then(() => toast("已复制", "success")) },
          { separator: true },
          { label: "拣选到当前分支（cherry-pick）", icon: "diff", disabled: !hooks.writable(), onClick: () => void confirmer("拣选提交", `将 ${c.short} 拣选到当前分支？`, () => op("cherry-pick", { ref: c.hash }, "已拣选")) },
          { label: "回滚此提交（revert）", icon: "undo", disabled: !hooks.writable(), onClick: () => void confirmer("回滚提交", `将创建一个反向提交以撤销 ${c.short}？`, () => op("revert", { ref: c.hash }, "已回滚")) },
          { label: "重置到此提交…", icon: "warning", danger: true, disabled: !hooks.writable(), onClick: () => void resetTo(c) },
          { separator: true },
          {
            label: "编辑历史（从这条之后改写）…",
            icon: "history",
            disabled: !hooks.writable(),
            onClick: () => void openHistoryEdit(c),
          },
          { separator: true },
          { label: "在此提交打标签…", icon: "tag", onClick: () => void createTag(c.hash) },
          { label: "新建分支…", icon: "branch", onClick: () => void createBranch(c.hash) },
        ])
      }
      logList.appendChild(row)
    }
    if (logHasMore) {
      const more = h("button", { class: "fw-btn ghost sm fw-more", text: logLoading ? "加载中…" : "加载更多" })
      more.onclick = () => void loadLog(false)
      logList.appendChild(more)
    }
    colLog.scrollTop = scrollTop
  }

  async function confirmer(title: string, message: string, fn: () => Promise<unknown>): Promise<void> {
    const ok = await confirmDialog({ title, message, okText: "执行" })
    if (ok) await fn()
  }

  /**
   * 编辑历史：把「这条提交之后」的提交拉到对话框里改（新→旧展示，提交时翻成应用顺序）。
   * 包含合并提交时直接拦下——重放合并提交要指定主线父，不在本能力的语义里。
   */
  async function openHistoryEdit(c: GitCommitInfo): Promise<void> {
    try {
      const res = await hooks.api.gitLog(hooks.root(), { ref: `${c.hash}..HEAD`, limit: 200 })
      if (!res.commits.length) {
        toast("这条提交之后没有可编辑的提交", "warn")
        return
      }
      const merges = res.commits.filter((x) => x.parents.length > 1)
      if (merges.length) {
        toast(`范围内含合并提交（${merges.map((m) => m.short).join("、")}），暂不支持编辑历史`, "error", 9000)
        return
      }
      await openHistoryEditDialog({
        api: hooks.api,
        root: hooks.root(),
        base: c.hash,
        commits: res.commits,
        onDone: () => {
          void doRefresh()
          hooks.onFsChanged()
        },
      })
    } catch (err) {
      toast(`读取提交范围失败：${(err as Error).message}`, "error", 8000)
    }
  }

  /**
   * 日志行的类名：布局类 + `current`（当前分支头）+ `active`（正在右栏查看的提交）。
   * 两个状态可同时成立，各自独立追加；写成函数是因为在 `class:` 模板里塞两段条件拼接后，
   * 样式契约测试（style-contract）无法静态判定宿主类——而它是「点亮的类必须有样式」的守门人。
   */
  function logRowClass(isHead: boolean, active: boolean): string {
    return ["fw-log-row", "graph", isHead ? "current" : "", active ? "active" : ""].filter(Boolean).join(" ")
  }

  /** 引用标签：带语义色（黄=当前分支头 / 绿=本地分支 / 紫=远程 / 标签）——
   *  多分支同屏时靠颜色就能分清「这是本地工作还是别人推上来的」。
   *  `full` 用于「+N」这类聚合标签：把被折叠的引用清单摆进 tooltip，信息不丢但不占宽度。 */
  function refChip(raw: string, full?: string): HTMLElement {
    if (full !== undefined) return h("span", { class: "fw-ref-chip more", title: full, text: raw })
    const t = raw.replace(/^HEAD -> /, "").replace(/^tag: /, "")
    let kind: "head" | "tag" | "remote" | "local" = "local"
    if (raw.startsWith("HEAD")) kind = "head"
    else if (raw.startsWith("tag:")) kind = "tag"
    else if (remotes.some((x) => x.name === t.split("/")[0])) kind = "remote"
    return h("span", { class: `fw-ref-chip ${kind}`, title: raw, text: kind === "tag" ? `🏷 ${t}` : t })
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
    currentCommitHash = c.hash
    commitHint.textContent = c.short
    // 动作按钮上移到栏头部（与标题同一行）：只依赖提交本身，详情取数失败也照常可用；
    // 旧按钮指向旧提交的 hash，选中变化时必须换掉
    clear(commitActions)
    commitActions.append(
      btnIcon("edit", "与工作区比较（此提交之后工作区又改了什么）", () => hooks.openCompare({ from: c.hash, to: "WORKTREE" })),
      btnIcon("diff", "整提交差异", () =>
        hooks.openDiff({ title: `提交 ${c.short}`, root: hooks.root(), path: "", source: { type: "range", from: `${c.hash}^`, to: c.hash } })),
    )
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
        const row = h("div", { class: "fw-commit-file", tabindex: "0", role: "button", "aria-label": `变更文件 ${f.path}` }, [
          h("span", { class: `fw-change-mark ${f.status === "added" ? "A" : f.status === "deleted" ? "D" : f.status === "renamed" ? "R" : "M"}`, text: f.status === "added" ? "A" : f.status === "deleted" ? "D" : f.status === "renamed" ? "R" : "M" }),
          h("span", { class: "fw-change-name", text: name, title: f.path }),
          h("span", { class: "fw-grow" }),
          h("span", { class: "fw-diff-stat add", text: `+${f.additions}` }),
          h("span", { class: "fw-diff-stat del", text: `-${f.deletions}` }),
        ])
        const openAt = (): void =>
          hooks.openDiff({
            title: `${name} @ ${c.short}`,
            root: hooks.root(),
            path: f.path,
            source: { type: "commit", hash: c.hash },
            fallback: f,
          })
        row.onclick = openAt
        row.onkeydown = (e) => {
          if (e.key !== "Enter" && e.key !== " ") return
          e.preventDefault()
          openAt()
        }
        row.oncontextmenu = (e) => {
          e.preventDefault()
          showMenu(e.clientX, e.clientY, [
            { label: "打开该文件在此提交中的差异", icon: "diff", onClick: openAt },
            { label: "看该文件的历史（日志栏筛选）", icon: "history", onClick: () => void filterByPath(f.path) },
            { separator: true },
            { label: "复制路径", icon: "copy", onClick: () => void navigator.clipboard.writeText(f.path).then(() => toast("已复制路径", "success")) },
          ])
        }
        files.appendChild(row)
      }
      host.replaceChildren(
        h("div", { class: "fw-commit-head" }, [
          h("div", { class: "fw-commit-subject", text: c.subject }),
          h("div", { class: "fw-log-meta" }, [
            h("span", { text: c.author }),
            h("span", { text: c.authorEmail }),
            h("span", { text: `作者时间 ${formatTime(c.authorTime)}` }),
            // 提交时间与作者时间不同时才多显示一行（重写/重放过的提交才会不同，同值重复摆纯属噪声）
            Math.abs(c.commitTime - c.authorTime) > 1000 ? h("span", { text: `提交时间 ${formatTime(c.commitTime)}` }) : null,
            ...c.refs.map((r) => refChip(r)),
          ]),
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

  let branchesLoading = false
  let branchesError = ""

  async function loadBranches(): Promise<void> {
    branchesLoading = true
    branchesError = ""
    renderBranches()
    try {
      branches = (await hooks.api.gitBranches(hooks.root())).branches
      refsAt = Date.now()
    } catch (err) {
      // 失败时保留旧数据但必须说明没读到（静默沿用会让人把陈旧分支清单当现状）
      branchesError = (err as Error).message
    } finally {
      branchesLoading = false
      renderBranches()
    }
  }

  /** 栏内加载/失败提示行（三栏共用：加载中有反馈，失败可重试）。 */
  function refsStatusLine(loading: boolean, error: string, retry: () => void): HTMLElement | null {
    if (!loading && !error) return null
    const bar = h("div", { class: `fw-bar${error ? " error" : ""}` }, [
      h("span", { text: error ? `读取失败：${error}` : "加载中…" }),
      h("span", { class: "fw-grow" }),
    ])
    if (error) {
      const b = h("button", { class: "fw-btn sm", text: "重试" })
      b.onclick = retry
      bar.appendChild(b)
    }
    return bar
  }

  /**
 * 重建某栏内容并保持它的滚动位置（切 tab / 刷新不该把长列表拽回顶部）。
 */
function replaceKeepScroll(host: HTMLElement, ...nodes: Array<Node | null>): void {
  const top = host.scrollTop
  host.replaceChildren(...(nodes.filter(Boolean) as Node[]))
  host.scrollTop = top
}

  /** 上次绘制的引用栏清单指纹（分支 / 标签 / 储存 / 远程四选一，切 tab 时用各自的键）。 */
  const refsKeys = { branches: "", tags: "", stash: "", remotes: "" }

  function renderBranches(): void {
    const s = hooks.status()
    /* 静默判据：分支清单 / 加载态 / 日志范围（行底色）/ 与上游相关的按钮可用性 / 工具栏的写权限。 */
    const key = fingerprint([
      listFingerprint(branches, (b) => [b.name, b.remote, b.current, b.ahead, b.behind, b.hash, b.subject ?? ""]),
      // 加载提示**只在还没有分支清单时**出现（首屏 / 失败后重试）；已有清单时刷新是静默的，
      // 否则每次自动刷新心跳都会先闪一下“加载中…”再换回同一样的列表
      branchesLoading && !branches.length,
      branchesError,
      logBranch,
      s ? [s.branch ?? "", s.upstream ?? "", s.ahead, s.behind] : null,
      hooks.writable(),
      hooks.remoteEnabled(),
    ])
    if (key === refsKeys.branches) return
    refsKeys.branches = key
    // 检出：当前分支点不出自己（菜单项自己就是 disabled），给出分支选择菜单；
    // 拉取：有上游才可拉（无上游时报错，不如置灰说明白）；推送：无上游时是「发布」语义（服务端 setUpstream）
    const branchNames = branches.filter((b) => !b.remote).map((b) => b.name)
    const checkoutMenu = (x: number, y: number): void =>
      showMenu(
        x,
        y,
        branchNames.map((name) => ({
          label: name,
          icon: hooks.status()?.branch === name ? "check" : "branch",
          disabled: hooks.status()?.branch === name,
          onClick: () => void op("branch", { action: "checkout", name }, `已切换到 ${name}`),
        })),
      )
    const cur = branches.find((b) => b.current)
    const local = branches.filter((b) => !b.remote)
    const remote = branches.filter((b) => b.remote)
    // 抓取直接抓全部远程（git fetch --all --prune），与当前分支的上游无关；
    // 拉取/同步作用于当前分支，需有上游（无则置灰并在 tooltip 说明）；推送：无上游时是「发布」语义（pushBranch 自动 setUpstream）。
    // 远程四按钮包在 .fw-remote-actions 里：在途禁用由 applyRemoteBusy 统一管（与远程栏同一口径）
    const noUp = !s?.upstream
    const fetchBtn = btnIcon("fetch", "抓取全部远程（--all --prune）", () => void op("fetch", { all: true, prune: true }, "抓取完成"))
    fetchBtn.disabled = !hooks.remoteEnabled()
    const pullBtn = btnIcon("download", noUp ? "拉取（当前分支未设置上游）" : `拉取（${s.upstream}）`, () => void confirmer("拉取", "从远程拉取当前分支并合并？", () => op("pull", { ffOnly: false }, "拉取完成")))
    pullBtn.disabled = noUp || !hooks.remoteEnabled()
    const syncBtn = btnIcon("sync", noUp ? "同步：拉取后推送（当前分支未设置上游）" : `同步：先拉取后推送（${s.upstream}）`, () => void (async () => {
      const ok = await op("pull", { ffOnly: false }, "同步：拉取完成，准备推送")
      if (ok === null) return // 拉取失败已 toast，不再推
      if (cur) await pushBranch(cur)
    })())
    syncBtn.disabled = noUp || !hooks.remoteEnabled()
    const pushBtn = btnIcon("upload", cur?.ahead ? `推送（↑${cur.ahead}）` : "推送", () => {
      if (cur) void pushBranch(cur)
    })
    pushBtn.disabled = !hooks.remoteEnabled()
    // 除刷新外全部左对齐（刷新贴右缘，与三栏头部刷新同一视觉锚点）
    const toolbar = h("div", { class: "fw-git-subbar" }, [
      btnIcon("plus", "新建分支…", () => void createBranch()),
      (() => {
        // 闭包捕获按钮自身：点击时取它的屏幕位置弹分支清单菜单（当前分支勾选并置灰）
        const b = btnIcon("check", "检出分支", () => checkoutMenu(b.getBoundingClientRect().right, b.getBoundingClientRect().bottom + 4))
        return b
      })(),
      (() => {
        // 删除分支：与「检出」同一形态（一个入口 + 分支清单菜单），不必到列表行右键里找。
        // 当前分支排除在外（git 不允许删自己）；远程分支不在本地删除的语义内（在远程栏里删）。
        const b = btnIcon("trash", "删除分支…", () => {
          const targets = local.filter((x) => !x.current)
          if (!targets.length) {
            toast("没有可删除的本地分支", "warn")
            return
          }
          const r = b.getBoundingClientRect()
          showMenu(
            r.right,
            r.bottom + 4,
            targets.map((x) => ({
              label: x.name,
              icon: "trash",
              danger: true,
              onClick: () => void confirmer("删除分支", `删除本地分支「${x.name}」？未合并的提交会丢失。`, () => op("branch", { action: "delete", name: x.name, force: true }, "已删除")),
            })),
          )
        })
        b.disabled = !hooks.writable()
        return b
      })(),
      h("span", { class: "fw-remote-actions" }, [fetchBtn, pullBtn, syncBtn, pushBtn]),
      h("span", { class: "fw-grow" }),
      btnIcon("refresh", "刷新", () => void loadBranches()),
    ])
    const list = h("div", { class: "fw-branch-list" })
    const group = (title: string, items: GitBranchInfo[]) => {
      if (!items.length) return
      list.appendChild(h("div", { class: "fw-section-title", text: `${title}（${items.length}）` }))
      for (const b of items) {
        // 两种状态各自一个类，**各管一件事**（视觉上：current = ✓ 图标，log-active = 行底色）
        const row = h(
          "div",
          {
            class: `fw-branch-row${b.current ? " current" : ""}${logBranch === b.name ? " log-active" : ""}`,
            tabindex: "0",
            role: "button",
            // 视觉靠图标/底色区分，读屏只能靠文字——两种状态都写进名称
            "aria-label": `分支 ${b.name}${b.current ? "（当前检出）" : ""}${logBranch === b.name ? "（正在看它的日志）" : ""}`,
          },
          [
            // 只有当前分支带 ✓：逐行都摆一个相同的分支图标只是噪声（列表本来就在「分支」栏里），
            // 而「我站在哪个分支上」是另一回事，需要一行一个记号
            b.current ? icon("check", 13) : null,
            h("span", { class: "fw-branch-name", text: b.name, title: b.subject }),
            b.ahead ? h("span", { class: "fw-ahead", text: `↑${b.ahead}`, title: "领先上游提交数" }) : null,
            b.behind ? h("span", { class: "fw-behind", text: `↓${b.behind}`, title: "落后上游提交数" }) : null,
            h("span", { class: "fw-grow" }),
            h("span", { class: "fw-log-hash", text: b.hash.slice(0, 7) }),
          ],
        )
        // 单击 = 把日志切到这个分支（分支栏 → 日志栏的动线）；检出在右键菜单里
        // 键盘用户同样要能进列表：Enter/Space 等价于点击
        row.onclick = () => setLogRef(b.name)
        row.onkeydown = (e) => {
          if (e.key !== "Enter" && e.key !== " ") return
          e.preventDefault()
          row.click()
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
            b.remote
              ? { label: "删除远程分支", icon: "trash", danger: true, disabled: !hooks.writable() || !hooks.remoteEnabled(), onClick: () => void confirmer("删除远程分支", `删除远程分支「${b.name}」？需要推送权限。`, () => op("branch", { action: "delete", name: b.name, remote: true, force: true }, "已删除远程分支")) }
              : { label: "删除分支", icon: "trash", danger: true, disabled: b.current || !hooks.writable(), onClick: () => void confirmer("删除分支", `删除本地分支「${b.name}」？未合并的提交会丢失。`, () => op("branch", { action: "delete", name: b.name, force: true }, "已删除")) },
            { separator: true },
            { label: "复制分支名", icon: "copy", onClick: () => void navigator.clipboard.writeText(b.name).then(() => toast("已复制", "success")) },
            { label: "复制提交哈希", icon: "copy", onClick: () => void navigator.clipboard.writeText(b.hash).then(() => toast("已复制", "success")) },
          ])
        }
        list.appendChild(row)
      }
    }
    group("本地分支", local)
    group("远程分支", remote)
    if (!local.length && !remote.length && !branchesLoading && !branchesError) {
      list.appendChild(h("div", { class: "fw-empty", text: "暂无分支（仓库可能还没有任何提交）" }))
    }
    replaceKeepScroll(colRefs, toolbar, refsStatusLine(branchesLoading && !branches.length, branchesError, () => void loadBranches()), list)
    syncRefsMinWidth(toolbar)
  }

  /* ------------------------------ 标签 / 储存 / 远程 ------------------------------ */

  let tagsLoading = false
  let tagsError = ""

  async function loadTags(): Promise<void> {
    tagsLoading = true
    tagsError = ""
    renderTags()
    try {
      localTags = (await hooks.api.gitTags(hooks.root())).tags
      refsAt = Date.now()
    } catch (err) {
      tagsError = (err as Error).message
    } finally {
      tagsLoading = false
      renderTags()
    }
  }

  function renderTags(): void {
    /* 静默判据：标签清单 / 加载态 / 日志范围（行底色）/ 写权限（工具栏动作）。 */
    const key = fingerprint([listFingerprint(localTags, (t) => [t.name, t.hash, t.time ?? 0]), tagsLoading && !localTags.length, tagsError, logBranch, hooks.writable()])
    if (key === refsKeys.tags) return
    refsKeys.tags = key
    const toolbar = h("div", { class: "fw-git-subbar" }, [
      h("span", { class: "fw-info", text: tagsError ? "读取失败" : tagsLoading ? "加载中…" : `${localTags.length} 个标签`, title: tagsError || undefined }),
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
      const row = h("div", { class: `fw-branch-row${logBranch === t.name ? " log-active" : ""}`, tabindex: "0", role: "button", "aria-label": `标签 ${t.name}` }, [icon("tag", 13), h("span", { class: "fw-branch-name", text: t.name }), h("span", { class: "fw-grow" }), h("span", { class: "fw-log-hash", text: t.hash.slice(0, 7) })])
      // 单击 = 看这个标签的日志（与分支行同一动线）
      row.onclick = () => setLogRef(t.name)
      row.onkeydown = (e) => {
        if (e.key !== "Enter" && e.key !== " ") return
        e.preventDefault()
        row.click()
      }
      row.oncontextmenu = (e) => {
        e.preventDefault()
        showMenu(e.clientX, e.clientY, [
          { label: "在日志中查看", icon: "history", onClick: () => setLogRef(t.name) },
          { label: "检出该标签（分离 HEAD）", icon: "check", onClick: () => void op("checkout", { ref: t.name, detach: true }, "已检出标签") },
          // 标签推送是整批动作（git push --tags），故文案写明“全部”
          { label: "推送全部标签到远程", icon: "upload", disabled: !hooks.writable() || !hooks.remoteEnabled(), onClick: () => void op("push", { tags: true }, "已推送标签") },
          { separator: true },
          { label: "复制标签名", icon: "copy", onClick: () => void navigator.clipboard.writeText(t.name).then(() => toast("已复制", "success")) },
          { label: "复制提交哈希", icon: "copy", onClick: () => void navigator.clipboard.writeText(t.hash).then(() => toast("已复制", "success")) },
          { separator: true },
          { label: "删除标签", icon: "trash", danger: true, onClick: () => void confirmer("删除标签", `删除「${t.name}」？`, () => op("tag", { action: "delete", name: t.name }, "已删除")) },
        ])
      }
      list.appendChild(row)
    }
    if (!localTags.length && !tagsLoading && !tagsError) list.appendChild(h("div", { class: "fw-empty", text: "暂无标签" }))
    replaceKeepScroll(colRefs, toolbar, refsStatusLine(tagsLoading && !localTags.length, tagsError, () => void loadTags()), list)
    syncRefsMinWidth(toolbar)
  }

  let stashLoading = false
  let stashError = ""

  async function loadStash(): Promise<void> {
    stashLoading = true
    stashError = ""
    renderStash()
    try {
      stashes = (await hooks.api.gitStash(hooks.root())).stashes
    } catch (err) {
      stashError = (err as Error).message
    } finally {
      stashLoading = false
      renderStash()
    }
  }

  function renderStash(): void {
    /* 静默判据：储存清单 / 加载态（行上再没有别的可变输入）。 */
    const key = fingerprint([listFingerprint(stashes, (s) => [s.index, s.ref, s.message, s.time ?? 0]), stashLoading && !stashes.length, stashError])
    if (key === refsKeys.stash) return
    refsKeys.stash = key
    const toolbar = h("div", { class: "fw-git-subbar" }, [
      h("span", { class: "fw-info", text: stashError ? "读取失败" : stashLoading ? "加载中…" : `${stashes.length} 条储存`, title: stashError || undefined }),
      h("span", { class: "fw-grow" }),
      (() => {
        const b = h("button", { class: "fw-btn ghost sm" }, [icon("plus"), h("span", { text: "储存当前更改" })])
        b.onclick = () => void (async () => {
          const msg = await promptDialog({ title: "储存更改（git stash）", label: "备注", placeholder: "例如：临时切换分支" })
          if (msg === null) return
          await op("stash", { action: "push", message: msg || undefined }, "已储存")
          hooks.onFsChanged()
        })()
        return b
      })(),
      btnIcon("refresh", "刷新", () => void loadStash()),
    ])
    const list = h("div", { class: "fw-branch-list" })
    for (const st of stashes) {
      const row = h("div", { class: "fw-branch-row", tabindex: "0", role: "button", "aria-label": `储存 ${st.message || st.ref}`, title: "双击恢复（pop）；其他动作用右键" }, [icon("archive", 13), h("span", { class: "fw-branch-name", text: st.message || st.ref }), h("span", { class: "fw-grow" }), h("span", { class: "fw-log-hash", text: st.ref })])
      // 双击才恢复：单击弹确认框在列表里太容易误触（恢复会改工作区）——破坏性动作走双击或右键
      row.ondblclick = () => void confirmer("恢复储存", `弹出「${st.message || st.ref}」并应用到工作区？`, async () => {
        await op("stash", { action: "pop", index: st.index }, "已恢复储存")
        hooks.onFsChanged()
      })
      row.onkeydown = (e) => {
        if (e.key !== "Enter") return
        e.preventDefault()
        row.dispatchEvent(new MouseEvent("dblclick"))
      }
      row.oncontextmenu = (e) => {
        e.preventDefault()
        showMenu(e.clientX, e.clientY, [
          { label: "查看内容差异（与父提交逐文件对比）", icon: "diff", onClick: () => hooks.openCompare({ from: `${st.ref}^`, to: st.ref }) },
          { label: "弹出（pop，成功后删除记录）", icon: "upload", onClick: () => void op("stash", { action: "pop", index: st.index }, "已弹出").then(() => hooks.onFsChanged()) },
          { label: "应用（apply，保留记录）", icon: "download", onClick: () => void op("stash", { action: "apply", index: st.index }, "已应用").then(() => hooks.onFsChanged()) },
          { separator: true },
          { label: "复制引用", icon: "copy", onClick: () => void navigator.clipboard.writeText(st.ref).then(() => toast("已复制", "success")) },
          { separator: true },
          { label: "删除该储存", icon: "trash", danger: true, onClick: () => void confirmer("删除储存", "删除后无法恢复，确定？", () => op("stash", { action: "drop", index: st.index }, "已删除")) },
        ])
      }
      list.appendChild(row)
    }
    if (!stashes.length && !stashLoading && !stashError) list.appendChild(h("div", { class: "fw-empty", text: "暂无储存记录" }))
    replaceKeepScroll(colRefs, toolbar, refsStatusLine(stashLoading, stashError, () => void loadStash()), list)
    syncRefsMinWidth(toolbar)
  }

  let remotesLoading = false
  let remotesError = ""

  async function loadRemotes(): Promise<void> {
    remotesLoading = true
    remotesError = ""
    renderRemotes()
    try {
      remotes = (await hooks.api.gitRemotes(hooks.root())).remotes
    } catch (err) {
      remotesError = (err as Error).message
    } finally {
      remotesLoading = false
      renderRemotes()
    }
  }

  function renderRemotes(): void {
    const s = hooks.status()
    /* 静默判据：远程清单 / 加载态 / 上游（拉取按钮的 tooltip 与可用性）/ 写与远程开关。 */
    const key = fingerprint([
      listFingerprint(remotes, (r) => [r.name, r.fetchUrl, r.pushUrl]),
      remotesLoading && !remotes.length,
      remotesError,
      s ? [s.upstream ?? "", s.branch ?? ""] : null,
      hooks.writable(),
      hooks.remoteEnabled(),
    ])
    if (key === refsKeys.remotes) return
    refsKeys.remotes = key
    /* 按钮：三个高频动作各占一个图标（与分支栏同一形态），变体与低频管理动作收进「更多」菜单——
       原先这里平铺 7 个带文字的按钮（抓取 / 拉取 / 拉取（仅快进）/ 变基拉取 / 推送 / 强制推送 / 添加远程），
       在 200px 宽的栏里折成 169px 高的一堆，把远程列表挤到看不见；而其中四个是同一动作的变体，
       高频的只有抓取/拉取/推送。状态文本与刷新留在同一行（原先多占一行）。 */
    const pullBtn = btnIcon("sync", s?.upstream ? `拉取（pull，合并到当前分支；上游 ${s.upstream}）` : "拉取（pull；当前分支未设置上游）", () => void confirmer("拉取", "从远程拉取当前分支并合并？", () => op("pull", { ffOnly: false }, "拉取完成")))
    const pushBtn = btnIcon("upload", "推送（push，未设置上游时自动发布）", () => void op("push", { setUpstream: true }, "推送完成"))
    const fetchBtn = btnIcon("fetch", "抓取（fetch --prune，更新远程跟踪分支）", () => void op("fetch", { prune: true }, "抓取完成"))
    const moreBtn = btnIcon("more", "更多远程操作（仅快进的拉取 / 变基拉取 / 强制推送 / 添加远程）", () => {
      const r = moreBtn.getBoundingClientRect()
      showMenu(r.right, r.bottom + 4, [
        { label: "拉取（仅快进）", icon: "sync", onClick: () => void op("pull", { ffOnly: true }, "拉取完成") },
        { label: "变基拉取（pull --rebase）", icon: "sync", onClick: () => void op("pull", { rebase: true }, "拉取完成") },
        { label: "强制推送（--force-with-lease）", icon: "upload", danger: true, onClick: () => void confirmer("强制推送", "使用 --force-with-lease 覆盖远程分支？请确认远程没有他人新提交。", () => op("push", { forceWithLease: true }, "已强制推送")) },
        { separator: true },
        { label: "添加远程…", icon: "plus", onClick: () => void addRemote() },
      ])
    })
    // 三个远程动作 + 「更多」包在 .fw-remote-actions 里：在途禁用由 applyRemoteBusy 统一管（与分支栏同一口径）
    const actions = h("span", { class: "fw-remote-actions" }, [fetchBtn, pullBtn, pushBtn, moreBtn])
    const toolbar = h("div", { class: "fw-git-subbar" }, [
      // 按钮组左对齐（与分支栏一致）：右对齐时按钮跟在一段空白后面，眼睛要在整栏宽里找它；
      // 只有刷新贴右缘（三栏头部刷新同一视觉锚点）。
      actions,
      // 状态文本只留「别处说不了」的那一种：**未设置上游**（拉取/同步不可用、推送是首次发布），
      // 跟在按钮后面（它说的正是这组按钮当前被限制在哪），也就是栏宽不够时先被省略的那一项。
      // 已设上游时上游名写进拉取按钮的 tooltip（与分支栏的拉取/同步同一口径）——
      // 栏宽下限是按钮组宽度（见 syncRefsMinWidth），固定占一句“跟踪 origin/…”会把栏白白顶宽。
      s?.upstream ? null : h("span", { class: "fw-info", text: "未设置上游" }),
      h("span", { class: "fw-grow" }),
      btnIcon("refresh", "刷新", () => void loadRemotes()),
    ])
    const list = h("div", { class: "fw-branch-list" })
    for (const r of remotes) {
      // 名称与地址分行：两者都长（地址尤其），挤在一行时地址只能截成看不出是哪个平台的半截；
      // 分行后名称为主行、地址为次行（灰色小字），行高变高但信息完整
      const row = h("div", { class: "fw-branch-row fw-remote-row", tabindex: "0", role: "button", "aria-label": `远程 ${r.name}`, title: "双击抓取该远程；其余动作用右键" }, [
        icon("git", 13),
        h("span", { class: "fw-remote-main" }, [
          h("span", { class: "fw-branch-name", text: r.name }),
          h("span", { class: "fw-remote-url", text: r.fetchUrl, title: `${r.fetchUrl}\n推送：${r.pushUrl}` }),
        ]),
      ])
      // 抓取是无损动作，但仍不放在单击上：远程列表点击用于查看，网络动作留给双击与菜单
      row.ondblclick = () => void op("fetch", { remote: r.name, prune: true }, `已抓取 ${r.name}`)
      row.oncontextmenu = (e) => {
        e.preventDefault()
        showMenu(e.clientX, e.clientY, [
          { label: "抓取该远程（fetch）", icon: "download", disabled: !hooks.remoteEnabled(), onClick: () => void op("fetch", { remote: r.name, prune: true }, `已抓取 ${r.name}`) },
          { separator: true },
          { label: "复制地址", icon: "copy", onClick: () => void navigator.clipboard.writeText(r.fetchUrl).then(() => toast("已复制", "success")) },
          { label: "复制推送地址", icon: "copy", onClick: () => void navigator.clipboard.writeText(r.pushUrl).then(() => toast("已复制", "success")) },
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
    if (!remotes.length && !remotesLoading && !remotesError) list.appendChild(h("div", { class: "fw-empty", text: "未配置远程仓库" }))
    replaceKeepScroll(colRefs, toolbar, refsStatusLine(remotesLoading && !remotes.length, remotesError, () => void loadRemotes()), list)
    syncRefsMinWidth(toolbar)
  }

  /** 添加远程（远程栏「更多」菜单与空态共用）：名称 + URL 两次输入，成功后重拉清单。 */
  async function addRemote(): Promise<void> {
    const name = await promptDialog({ title: "添加远程", label: "名称", value: "origin" })
    if (!name?.trim()) return
    const url = await promptDialog({ title: "远程地址", label: "URL", placeholder: "git@github.com:user/repo.git" })
    if (!url?.trim()) return
    await op("remote", { action: "add", name: name.trim(), url: url.trim() }, "已添加远程")
    void loadRemotes()
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
    // 「状态读取失败」与「不是仓库」必须分开：前者没有可信状态，
    // 摆出「初始化仓库」这类入口会把人引到错误操作上（仓库其实好好的）。
    const statusErr = hooks.statusError?.() ?? null
    if (statusErr) {
      colRefs.replaceChildren(renderStatusError(statusErr))
      currentCommitHash = ""
      resetCommitActions()
      colCommit.replaceChildren(renderCommitPlaceholder("Git 状态不可用：先解决状态读取失败"))
      logItems = []
      logError = statusErr
      renderLog()
      return
    }
    const s = hooks.status()
    if (!s?.isRepo) {
      colRefs.replaceChildren(renderNotRepo())
      currentCommitHash = ""
      resetCommitActions()
      colCommit.replaceChildren(renderCommitPlaceholder("当前根不是 Git 仓库"))
      logItems = []
      logError = ""
      renderLog()
      return
    }
    // 三栏各自渲染（并排常显，不互相覆盖）
    if (refsTab === "branches") await loadBranches()
    else if (refsTab === "tags") await loadTags()
    else if (refsTab === "stash") await loadStash()
    else await loadRemotes()
    // 日志每次都重置到第一页：否则提交后 / F5 之后日志停在旧历史（只有日志栏自己的刷新按钮才更新）。
    // 历史未变时 loadLog 不重建列表（见其 unchanged 分支），因此不会把翻了几页的位置拽回去。
    await loadLog(true)
    // 正展示的提交已不在当前日志里（历史被重写 / 硬重置丢弃 / 换了过滤）→ 收回到占位态并复位头部
    resetCommitViewIfStale()
    applyRemoteBusy()
  }

  /*
   * 日志「滚动到底自动加载」：监听**真正的滚动容器** colLog（`.fw-git-col-body`）。
   * 早期把 onscroll 挂在 `.fw-log-list` 上，而该元素没有 overflow（滚动在父级 colLog 上）——
   * 非滚动元素不产生 scroll 事件，自动加载实际是死代码（只剩「加载更多」按钮）。
   * 容器常驻，挂一次即可；有待续页且不在加载中才响应。
   */
  colLog.addEventListener(
    "scroll",
    () => {
      if (logLoading || !logHasMore) return
      if (colLog.scrollTop + colLog.clientHeight > colLog.scrollHeight - 60) void loadLog(false)
    },
    { passive: true },
  )

  bindColResizer(sp1, "a")
  bindColResizer(sp2, "c")

  return { el, refresh, show, filterByPath }
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
  ctx: { repoRootPath: string; language: string; onChanged?: () => void; onOpenStage?: (repoRel: string) => void },
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
    const diffHost = h("div", { class: "fw-diff-host" })
    const partialHost = h("div", { class: "fw-partial-host" })
    partialHost.hidden = true
    let partial: PartialPanelHandle | null = null

    /* 逐块暂存：Monaco 并列差异是只读阅读器，放不进逐块控件，
     * 故换一张可勾选的清单（见 partial.ts）——只在工作区差异上提供
     * （历史提交、任意两端对比没有「暂存」一说）。 */
    const canPartial = source.type === "worktree"
    const partialBtn = canPartial
      ? h("button", { class: "fw-btn ghost sm", "aria-pressed": "false", title: "逐块／逐行选择要暂存（index）或放弃的更改" }, [icon("check", 12), h("span", { text: "逐块操作" })])
      : null

    const wrap = h("div", { class: "fw-diff-wrap" }, [
      h("div", { class: "fw-viewer-bar" }, [
        h("span", { class: "fw-viewer-info", text: `${ep.note}` }),
        h("span", { class: "fw-viewer-spacer" }),
        partialBtn,
        // 只留信息不放按钮：导航按钮统一在标签栏（跨文件一组 + 文件内一组，见 main.ts）
        h("span", { class: "fw-hint", text: `A：${ep.labelA} ｜ B：${ep.labelB}` }),
      ]),
      diffHost,
      partialHost,
    ])
    host.appendChild(wrap)

    if (partialBtn) {
      let on = !!spec.partial
      const ensurePanel = (): void => {
        if (partial) return
        partial = createPartialPanel(
          partialHost,
          api,
          { root, path, side: spec.source.type === "worktree" && spec.source.staged ? "staged" : "unstaged" },
          { onChanged: () => ctx.onChanged?.(), onOpenStage: ctx.onOpenStage ? () => ctx.onOpenStage?.(path) : undefined },
        )
      }
      /** 开 = 按下态（`.fw-btn.active` 在 `.fw-btn.ghost` 之后，同特异度后者胜，故无需摘 ghost 类）。 */
      const applyMode = (): void => {
        partialHost.hidden = !on
        diffHost.hidden = on
        partialBtn.classList.toggle("active", on)
        partialBtn.setAttribute("aria-pressed", on ? "true" : "false")
        if (on) ensurePanel()
      }
      partialBtn.onclick = () => {
        on = !on
        applyMode()
      }
      // 以「逐块暂存」态打开时（变更面板的入口）直接进该模式（走同一条路径，不模拟点击）
      if (on) applyMode()
    }

    const handle = await createDiffEditor(diffHost, { original: a.text, modified: b.text, language: ctx.language })
    // 导航按钮由标签栏渲染（handle.nav 交给调用方）
    return { dispose: () => {
      partial?.dispose()
      handle.dispose()
      wrap.remove()
    }, nav: handle.nav ?? null }
  } catch (err) {
    // 回退：结构化 hunks（服务端已解析；没带就现取）
    return renderHunks(await fetchFallback(), `并列视图不可用（${(err as Error).message}），已降级为逐行差异`)
  }
}
