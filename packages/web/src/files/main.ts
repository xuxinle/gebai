/**
 * 文件工作台 · 主入口（`/files` 页面）：IDE 式外壳（菜单栏 / 活动栏 / 资源管理器 / 标签页 / 查看器 /
 * Git 面板 / 状态栏）+ 文件标签生命周期 + 保存冲突处理 + 搜索 + 快速打开 + 快捷键。
 *
 * 与主界面的关系：完全独立的页面与入口（vite 多入口 files.html），共用同一端口、同一套 CSS 令牌、
 * 同一 `/api/v1` 契约；轮盘菜单「文件」按钮以新标签打开本页（主界面能力不变，互不干扰）。
 *
 * 状态模型：`tabs` 数组 + 每个标签独立的 DOM 子树（切标签只切显隐，保留滚动位置与编辑器状态）；
 * 文件内容与磁盘一致性用服务端 etag 做乐观锁（保存冲突三选一：覆盖 / 重新加载 / 取消）。
 */
import { normalizeArtifactPath, resolveDeepLink } from "./deeplink"
import { effectiveLanguageOf, languageOfPath } from "@gebai/sdk"
import { wbUrl } from "./url-base"
import { createMergeView, type MergeView } from "./merge-view"
import { createStageView, type StageView } from "./staging"
import { FsApi, ApiError, type FileStat, type GitStatusInfo, type ReadResponse, type RootInfo, type RootsResponse } from "./api"
// 文件工作台自带样式：base.css 提供设计令牌（主题 CSS 只换令牌），files.css 负责本页布局
import "../css/base.css"
import "../css/files.css"
// 动作轮盘（标签栏右侧）用与标题栏轮盘同一套几何与外观
import "../css/wheel.css"
// 快速打开面板（VSCode Quick Open 同款）的样式
import "../css/quick-open.css"
import { createWheel, type WheelHandle, type WheelItem } from "../wheel-core"
import { createEditor, isMinimap, isWordWrap, prewarmMonaco, refreshEditorTheme, monacoLoadFailed, toggleMinimap, toggleWordWrap, type EditorHandle, type EditorMenuGroup, type EditorMenuItem, type BlameLine } from "./editor"
import { readInlineBlame, saveInlineBlame } from "./blame-prefs"
import { gutterEligible } from "./git-gutter"
import { attachDocument, attachedServerOf, hasLsp, initLsp, lspServerDetailOf, notifySaved, setLspNotifier, setLspOpener, setLspSessionProvider, type LspJumpTarget } from "./lsp"
import { wordWrapTitle } from "./wrap"
import { minimapTitle } from "./minimap"
import { branchTitle, changeParts } from "./git-counts"
import { installWorkbenchKeys, workbenchKeymap } from "./keymap-wb"
import { FOCUS_ALL_FIELDS, validateKeymap, helpGroups, popKeyScope, pushEscScope } from "../keymap"
import type { KeyBinding } from "../keymap"
import { loadSession, saveSession, tabKey, type FwSessionState, type FwTabState } from "./session-state"
import { createNavHistory, type NavEntry } from "./nav-history"
import { fingerprint } from "./refresh-guard"
import { absOfRepo, normPath, relWithin, repoPrefixOfAbs, resolveAbsPath, resolveRepoPath as resolveRepoPathPure, rootAbsFromId, toRepoRel, type ResolvedRepoPath } from "./repo-paths"
import { createExplorer } from "./explorer"
import { createFsWatcher } from "./watch"
import { invalidateQuickOpenIndex, isQuickOpenOpen, openQuickOpen } from "./quick-open"
import { recordRecentFile } from "./recents"
import { isSymbolPanelOpen, openSymbolPanel } from "./symbol-panel"
import { createChangesPanel, type ChangesPanel } from "./changes"
import { clampPanelWidth, LEFT_MIN_FLOOR } from "./panel-width"
import { createUrlSync, parseUrlState } from "./url-state"
import { initTheme, setAcrylicLt, setCnyScheme, setTheme, type AcrylicLtId, type CnySchemeId, type ThemeId } from "../theme-core"
import { createGitPanel, diffEndpointsFor, mountDiffView, type DiffSpec, type GitPanel } from "./git"
import { createTerminalPanel, type TerminalPanel } from "./terminal"
import type { DiffNav, EditorSnippet } from "./editor"
import { createCompareView, WORKTREE, type CompareView } from "./compare"
import { renderViewer, downloadUrl, type ViewerCtx } from "./viewers"
import { previewKindOf } from "./preview-kind"
import { blockNativeContextMenu } from "../native-menu"
import { applyWebConfig, awaitCustomBoot } from "../boot-config"
import { h, icon, clear, toast, formatSize, formatTime, extOf, confirmDialog, promptDialog, showMenu, dropdown, closeMenu } from "./ui"

// 二开前端脚本（custom/web/，产物根 gebai.config.js 与 gebai.custom.js）：配置模块加载即应用——先于下面
// 模块级读取 localStorage 的代码；初始化脚本的异步引导在 boot 首行等待（awaitCustomBoot）
applyWebConfig()

/* ------------------------------ 全局状态 ------------------------------ */

const LOCAL_ENV_KEY = "gebai.ui.env"
const SESSION_KEY = "gebai.ui.session"
/**
 * 底部工具窗的当前视图与开合状态（跨会话记忆：上次看的是 Git 还是终端，下次照旧）。
 * 开合记忆 key 取 v2：初版无记忆时按窗口宽度默认展开，改默认收起后换 key 让存量「自动展开」记忆作废。
 */
const DOCK_VIEW_KEY = "gebai.ui.dockView"
const DOCK_VISIBLE_KEY = "gebai.ui.dockVisible2"

/** 底部工具窗的视图（互斥显示，实例各自保留状态）。 */
type DockView = "git" | "terminal"

function readDockView(): DockView {
  try {
    return localStorage.getItem(DOCK_VIEW_KEY) === "terminal" ? "terminal" : "git"
  } catch {
    return "git"
  }
}

/** 工具窗是否展开：优先用上次记忆，没记忆时默认收起（不自动占用编辑区）。 */
function readDockVisible(): boolean {
  try {
    const v = localStorage.getItem(DOCK_VISIBLE_KEY)
    if (v === "1") return true
    if (v === "0") return false
  } catch {
    /* 隐私模式：默认收起 */
  }
  return false
}

function readLocalEnv(): Record<string, string> {
  try {
    const raw = localStorage.getItem(LOCAL_ENV_KEY)
    if (!raw) return {}
    const obj = JSON.parse(raw) as Record<string, unknown>
    const out: Record<string, string> = {}
    for (const [k, v] of Object.entries(obj)) if (typeof v === "string") out[k] = v
    return out
  } catch {
    return {}
  }
}

function resolveSessionId(): string | undefined {
  const fromUrl = new URLSearchParams(location.search).get("session")
  if (fromUrl) return fromUrl
  try {
    return localStorage.getItem(SESSION_KEY) ?? undefined
  } catch {
    return undefined
  }
}

/** 一次「变更文件」遍历的上下文（差异标签内部换文件的游标）。 */
interface ReviewCtx {
  /** 仓库相对路径列表（顺序即差异列表顺序） */
  files: string[]
  /** 当前文件下标；-1 = 当前文件不在清单里（如从文件历史打开的某次提交差异） */
  index: number
}

/** Monaco 文档对象（仅用于跨文件跳转时的语言提示与会话复用传递，不在此处做编辑操作）。 */
type Model = import("monaco-editor").editor.ITextModel

interface Tab {
  id: string
  kind: "file" | "diff"
  root: string
  path: string
  title: string
  preview: boolean
  host: HTMLElement
  /** 文件态 */
  stat?: FileStat
  mode: "view" | "edit"
  dirty: boolean
  baseline: string
  content: string
  encoding: string
  eol: string
  etag: string
  truncated?: boolean
  binary?: boolean
  loadError?: string
  editor?: EditorHandle
  viewDispose?: () => void
  scrollTop?: number
  /** 差异态 */
  diffSpec?: DiffSpec
  diffDispose?: () => void
  /** 差异块导航（F7/Shift+F7 与差异视图头部按钮共用；降级渲染为 null） */
  diffNav?: DiffNav | null
  /** 差异视图头部的导航按钮组（换文件 ←→ / 换差异 ↑↓）：随差异视图重建，与视图同生命周期 */
  diffNavCtl?: DiffHeaderNav
  /** 加载令牌（loadTab 每次自增；`await` 回来后据此判断自己是否已被取代 / 标签已关闭） */
  loadGen?: number
  /**
   * 变更文件清单（与差异同一端点对）：跨文件导航用。
   * 打开差异后异步取，未就绪时标签栏按钮置灰。
   */
  review?: ReviewCtx
  /** 标签图标覆盖（合并视图用 merge 图标，其余按 kind/dirty 推断） */
  icon?: string
  /** 当前是否以**渲染形态**显示（markdown：点「渲染预览」后置位；重建/切回源码时清空） */
  previewKind?: "markdown"
  /** blame 行装饰：两态各自开关（只读查看时可用；编辑器重建后失效）。`blameLines` 是两态共用的数据缓存 */
  blameGutter?: boolean
  blameInline?: boolean
  blameLines?: BlameLine[]
  /** Git 修改标记：HEAD 基线是否已应用到编辑器（false = 未应用，标签就绪/状态到达后补）；切换/重建后重置 */
  gitGutterOn?: boolean
  /** 最近的光标行/列与滚动位置（切标签时记下；状态记忆据此回到刷新前的位置） */
  cursorLine?: number
  cursorColumn?: number
}

const state = {
  env: readLocalEnv(),
  sessionId: resolveSessionId(),
  rootsResp: null as RootsResponse | null,
  roots: [] as RootInfo[],
  gitStatus: null as GitStatusInfo | null,
  /** Git 状态读取失败原因（null = 正常）：与「不是仓库」分开，供面板与状态栏区分展示。 */
  gitStatusError: null as string | null,
  repoPrefix: "",
  tabs: [] as Tab[],
  activeId: null as string | null,
  leftView: "explorer" as "changes" | "explorer" | "search",
  /** 工具窗可见且当前是 Git（多处刷新逻辑据此判断“Git 面板是否真的看得见”） */
  gitViewVisible: readDockVisible() && readDockView() === "git" && window.innerWidth >= 1180,
  /** 底部工具窗开合（与 dockView 一起决定显示哪个面板） */
  dockVisible: readDockVisible() && window.innerWidth >= 1180,
  /** 底部工具窗当前视图（Git / 终端） */
  dockView: readDockView(),
  cursor: { line: 1, column: 1, selected: 0 },
}

const api = new FsApi(() => state.env, () => state.sessionId)

/* ------------------------------ 布局骨架 ------------------------------ */

// 无标题栏（IDEA 新 UI 的做法：去掉传统菜单栏，把入口交给左侧活动栏与工具窗自身）
const railEl = h("div", { class: "fw-rail" })
const leftPanel = h("div", { class: "fw-left" })
const leftResizer = h("div", { class: "fw-resizer", title: "拖动调整宽度" })
/**
 * 标签栏三段：标签条（横向滚动）/ 空白区（双击快速打开）/ 右侧动作区。
 *
 * 分成三段而不是把所有东西塞进一个 flex 行：标签条只占自身内容宽度，**标签超出时在条内滚动**，
 * 空白区与动作区留在原位。整行铺开时（早期做法）超出的标签会把动作按钮一路顶到可视区之外
 * （`.fw-tabbar` 是 `overflow: hidden`，按钮连点都点不到），标签自己也被压成一排省略号。
 */
const tabstrip = h("div", { class: "fw-tabstrip" })
// 标签条不画横向滚动条（见 css/files.css），滚轮就是它唯一的鼠标滚法：
// 纵向滚轮交给标签条（Chromium 不会把 deltaY 自动映射到横向滚动容器上）。
// 横向滚动（触控板双指 / Shift+滚轮）与 Ctrl+滚轮缩放不拦，直接放给浏览器。
tabstrip.addEventListener(
  "wheel",
  (e) => {
    if (e.ctrlKey || e.deltaX || !e.deltaY) return
    const step = e.deltaMode === 1 ? e.deltaY * 16 : e.deltaMode === 2 ? e.deltaY * tabstrip.clientWidth : e.deltaY
    const before = tabstrip.scrollLeft
    tabstrip.scrollLeft = before + step
    if (tabstrip.scrollLeft !== before) e.preventDefault()
  },
  { passive: false },
)
const tabSpacer = h("div", { class: "fw-tabbar-spacer", title: "双击快速打开文件（Ctrl+P，模糊搜文件名）" })
const tabActionsHost = h("div", { class: "fw-tabbar-actions" })
const tabbar = h("div", { class: "fw-tabbar" }, [tabstrip, tabSpacer, tabActionsHost])
// 空白区双击 = 快速打开：标签条只占内容宽度，这块空处才是“标签栏上什么都没有的地方”
tabSpacer.ondblclick = async () => {
  const name = await promptDialog({ title: "快速打开文件", label: "文件路径（相对当前根）", placeholder: "src/main.ts" })
  if (name?.trim()) void openFile(explorer.getRoot(), name.trim(), { preview: false })
}
const views = h("div", { class: "fw-views" })
// 底部工具窗（IDEA 式）：Git 面板停靠在此，可拖拽调高、可整体收起
const gitDock = h("div", { class: "fw-git-dock" })
const gitDockResizer = h("div", { class: "fw-dock-resizer", title: "拖动调整高度" })
const statusbar = h("footer", { class: "fw-statusbar" })

/**
 * 外壳分区（对齐 IDEA 的 tool window 布局）：
 *
 *   ┌──────┬──────────────────────────────────────┐
 *   │      │ 左栏（资源管理器/变更） │ 编辑区      │  ← .fw-body：只占"上部"
 *   │ 活动 ├──────────────────────────────────────┤
 *   │ 栏   │ 底部工具窗（分支 | 日志 | 提交内容）  │  ← 从活动栏右侧开始，横跨左栏下方
 *   ├──────┴──────────────────────────────────────┤
 *   │ 状态栏                                       │
 *   └─────────────────────────────────────────────┘
 *
 * 底部工具窗放在「上部区」**之外**（而不是编辑区里）：它看的是「历史/提交」这类全局信息，
 * 与左边在看哪个目录无关，需要完整横宽才摆得下三栏；左栏则随之上收到上半部分
 * （同 IDEA：打开底部工具窗时左侧 Project 工具窗被压矮，而非并排）。
 *
 * 活动栏是**唯一贯穿全高**的一列：视图切换/工具窗开关这类入口要在任何面板开合时都待在原位，
 * 被底部工具窗截断半截会让"下方那几个按钮"看起来像是工具窗的一部分。
 *
 * 命名提醒：包裹类叫 .fw-main-right 而**不是** .fw-right——后者是已废弃的"右侧 Git 面板"用过的
 * 名字，CSS 里还留过一条针对它的窄屏规则（absolute + top:34px），撞名会让整块包裹在 ≤1080px
 * 时变成一条抽屉、编辑区被压成几十像素宽（已踩过一次，见 files.css 同名注释）。
 */
const rootEl = h("div", { class: "fw-app" }, [
  h("div", { class: "fw-main" }, [
    railEl,
    h("div", { class: "fw-main-right" }, [
      h("div", { class: "fw-body" }, [leftPanel, leftResizer, h("div", { class: "fw-center" }, [tabbar, views])]),
      gitDockResizer,
      gitDock,
    ]),
  ]),
  statusbar,
])

/* ------------------------------ 资源管理器与 Git 面板 ------------------------------ */

const explorer = createExplorer({
  api,
  roots: () => state.roots,
  rootsMeta: () => ({
    writable: !!(state.rootsResp?.writable && (state.rootsResp?.gitWrite ?? true) !== false) && (state.rootsResp?.writable ?? false),
    gitEnabled: !!state.rootsResp?.gitEnabled,
    sandboxed: !!state.rootsResp?.sandboxed,
  }),
  openFile: (root, path, opts) => {
    // 窄屏抽屉：选中文件即收起，把编辑区让出来（否则还要再点一下遮罩）
    setDrawerOpen(false)
    void openFile(root, path, { preview: opts?.preview ?? false, only: opts?.only })
  },
  activeFile: () => {
    const t = activeTab()
    return t && t.kind === "file" ? { root: t.root, path: t.path } : null
  },
  gitStatus: () => state.gitStatus,
  repoPathPrefix: () => state.repoPrefix,
  onFsChanged: () => void refreshGit(),
  onRootChanged: (rootId) => void onRootChanged(rootId),
  // 地址栏同步：进目录记历史（可后退），点文件就地替换
  onNavigate: (_path, isDir) => (isDir ? urlSync.push() : urlSync.replace()),
  // 展开/折叠：把新的目录清单重新交给变更监听（新展开的目录要立刻挂上 watch，不必等这一轮超时）
  onTreeChanged: () => fsWatcher.poke(),
  // 「在 Git 日志中筛选该文件」：宿主负责展开工具窗（面板自己不知道当前是否可见）
  // 树给的是**根相对**路径，日志栏过滤要的是**仓库相对**（同一条路径两套坐标，缺前缀就会静默过滤成空）
  openLogFilter: (path) => void showInGitLog(toRepoRel(state.repoPrefix, path)),
})

/* ------------------------------ 变更面板（左栏工具窗） ------------------------------ */

let changesPanel: ChangesPanel | null = null

/**
 * 左栏宽度下限：**提交框动作行的实测宽度**（面板每次渲染后报过来，见 changes.ts 的 reportMinWidth）。
 *
 * 为什么不写死：这一行里是两个提交按钮 + 修补/历史两个控件，改动文案或主题字体就可能变宽；
 * 写死的数字一旦偏小，拖窄后按钮就会被裁掉半个或挤成两行（不报错，只是点不到）。
 */
let leftMin = LEFT_MIN_FLOOR
function leftMinWidth(): number {
  return Math.max(LEFT_MIN_FLOOR, leftMin)
}

/** 把左栏下限写进 CSS 变量（CSS 与拖动夹取共用同一个值），并把当前宽度顶回下限。 */
function applyLeftMin(px: number): void {
  leftMin = Math.max(LEFT_MIN_FLOOR, Math.ceil(px))
  leftPanel.style.setProperty("--fw-left-min", `${leftMin}px`)
  const cur = leftPanel.getBoundingClientRect().width
  if (cur && cur < leftMin) leftPanel.style.width = `${leftMin}px`
}

/**
 * 变更面板（左栏）：工作区改动 + 提交框。
 * 与底部 Git 工具窗分开挂载，但共用同一份 git 状态与同一套写操作流程（写完全都刷新）。
 */
function ensureChangesPanel(): ChangesPanel {
  if (changesPanel) return changesPanel
  changesPanel = createChangesPanel({
    api,
    root: () => explorer.getRoot(),
    repoPrefix: () => state.repoPrefix,
    status: () => state.gitStatus,
    refreshStatus: () => refreshGit(),
    openDiff: (spec) => void openDiff(spec),
    openFile: (root, path, line) => void openFile(root, path, { preview: false, line }),
    openRepoFile: (repoRel) => openRepoFile(repoRel),
    revealRepoInExplorer: (repoRel) => revealRepoInExplorer(repoRel),
    openCompare: (init) => void openCompare(init),
    openMerge: (repoRel) => void openMergeTab(repoRel),
    openStage: (repoRel) => void openStageTab(repoRel),
    writable: () => !!(state.rootsResp?.writable && state.rootsResp?.gitWrite),
    remoteEnabled: () => !!(state.rootsResp?.gitRemote && state.rootsResp?.writable),
    onFsChanged: () => void explorer.refresh(undefined, { keepSelection: true }),
    openFileHistory: (repoRel) => void showFileHistoryByPath(repoRel),
    showInLog: (repoRel) => void showInGitLog(repoRel),
    // 徽标：改动数变化时只重画 rail（不重画左栏，避免提交框里的输入被打断）
    onCount: () => renderRail(),
    // 提交框动作行的实测宽度 → 左栏下限（拖窄不许窄到把按钮裁掉）
    onMinWidth: (px) => applyLeftMin(px),
  })
  return changesPanel
}

let gitPanel: GitPanel | null = null
function ensureGitPanel(): GitPanel {
  if (gitPanel) return gitPanel
  gitPanel = createGitPanel({
    api,
    root: () => explorer.getRoot(),
    repoPrefix: () => state.repoPrefix,
    status: () => state.gitStatus,
    statusError: () => state.gitStatusError,
    refreshStatus: () => refreshGit(),
    openDiff: (spec) => void openDiff(spec),
    openFile: (root, path, line) => void openFile(root, path, { preview: false, line }),
    openCompare: (init) => void openCompare(init),
    openMerge: (repoRel) => void openMergeTab(repoRel),
    // 关闭按钮在面板标题栏内：收起工具窗（不销毁实例，再开时状态保留）
    close: () => toggleGitPanel(false),
    writable: () => !!(state.rootsResp?.writable && state.rootsResp?.gitWrite),
    remoteEnabled: () => !!(state.rootsResp?.gitRemote && state.rootsResp?.writable),
    onFsChanged: () => void explorer.refresh(undefined, { keepSelection: true }),
  })
  // 初始显隐按当前工具窗状态定（面板创建得晚于状态初始化）
  gitPanel.el.classList.toggle("fw-dock-hidden", !(state.dockVisible && state.dockView === "git"))
  gitDock.appendChild(gitPanel.el)
  return gitPanel
}

/* ------------------------------ 终端面板（同一底部工具窗） ------------------------------ */

let termPanel: TerminalPanel | null = null

/**
 * 终端面板与 Git 面板**共用同一个底部工具窗**：同一停靠位、同一高度变量、同一拖拽条。
 * 两者互斥显示但实例都保留——切来切去不该丢掉 Git 的滚动位置与终端的滚动缓冲/会话。
 */
function ensureTerminalPanel(): TerminalPanel {
  if (termPanel) return termPanel
  termPanel = createTerminalPanel({
    root: () => explorer.getRoot(),
    // cwd 初值：选中目录时用它，否则根目录（根内相对路径）
    cwd: () => {
      const sel = explorer.selected()
      return sel?.type === "dir" ? sel.path : ""
    },
    session: () => state.sessionId,
    env: () => state.env,
    close: () => setDock(false),
  })
  termPanel.el.classList.toggle("fw-dock-hidden", !(state.dockVisible && state.dockView === "terminal"))
  gitDock.appendChild(termPanel.el)
  return termPanel
}

/**
 * 在 Git 工具窗的日志栏按文件过滤（资源管理器 / 变更面板 / 提交内容的入口）。
 * 工具窗收起时先展开——否则用户点完看不到任何变化（数据其实已经过滤好了）。
 */
/** 日志栏按文件过滤：入参为**仓库相对**路径（路径过滤是 git 语义，跨根一致）。 */
async function showInGitLog(path: string): Promise<void> {
  if (!state.gitViewVisible) toggleGitPanel(true)
  await ensureGitPanel().filterByPath(path)
}

/* ------------------------------ 根与 Git 状态 ------------------------------ */

async function loadRoots(): Promise<void> {
  try {
    const res = await api.roots()
    state.rootsResp = res
    state.roots = res.roots
    // 隐藏文件的默认可见性来自服务端配置（GEBAI_FS_HIDDEN，默认显示）；必须在首次列举（setRoot）之前落位
    explorer.applyHiddenDefault(res.showHidden)
    if (!res.enabled) {
      toast("文件工作台未在服务端启用（GEBAI_FS_ENABLED=false）", "error", 8000)
      return
    }
    const target = resolveDeepLink(state.roots, location.search, { isWin: IS_WIN })
    if (target) {
      await explorer.setRoot(target.rootId, target.dir)
      if (target.file) void openFile(target.rootId, target.file, { preview: false, line: target.line })
    } else {
      toast("没有可用根（未注册项目且无会话工作区）", "warn")
    }
  } catch (err) {
    toast(`加载根清单失败：${(err as Error).message}`, "error", 8000)
  }
}

/** Windows 客户端（盘符大小写不敏感；深层链接解析按此选择匹配策略）。 */
const IS_WIN = navigator.userAgent.includes("Windows")

/**
 * 当前根的**绝对路径**：根清单优先，其次从 `abs:` 根 id 解析。
 *
 * 为什么要能解析 id：本页可能被以**根清单之外**的根打开（`?root=abs:<子目录>`、或在树上
 * 选了「打开文件夹」后动态加入的 `abs:` 根）——只看清单会拿不到路径，进而算不出仓库前缀。
 */
function rootAbsOf(rootId: string): string {
  const info = state.roots.find((r) => r.id === rootId)
  return info?.path ?? rootAbsFromId(rootId) ?? ""
}

/** 当前仓库根的绝对路径：Git 状态接口的 `repoRoot` 最权威（子目录根只有它带得出仓库根），根清单兜底。 */
function repoRootAbsOf(rootId: string): string {
  if (state.gitStatus?.repoRoot) return normPath(state.gitStatus.repoRoot, IS_WIN)
  const info = state.roots.find((r) => r.id === rootId)
  return info?.repoRoot ? normPath(info.repoRoot, IS_WIN) : ""
}

/**
 * root 在仓库内的相对前缀（`""` = 根就是仓库根 / 非仓库 / 还没拿到仓库根）。
 *
 * 两个来源都用：`onRootChanged` 里状态还没到手，只能靠根清单先算一版；状态到达后用
 * `status.repoRoot` 复算（**子目录根往往不在根清单里，只有它带得出仓库根**）。
 * 「不在仓库里」与「根就是仓库根」在上层不需要区分——它们都要先看 `status.isRepo`，
 * 不是仓库时面板/状态栏走的是占位与非仓库分支，前缀不参与判断（区分语义在 repo-paths.ts 里保留给纯函数）。
 */
function repoPrefixNow(rootId = explorer.getRoot()): string | null {
  if (state.gitStatus && !state.gitStatus.isRepo) return null
  return repoPrefixOfAbs(rootAbsOf(rootId), repoRootAbsOf(rootId), IS_WIN)
}

/**
 * 仓库相对路径 → 打开它所需的 (root, 根内相对路径)。
 *
 * 变更面板说的是 **Git 的坐标**（仓库相对），而 fs/编辑器说的是**根相对**——根可能是仓库的
 * 子目录，也可能与改动所在目录毫无包含关系（打开「整仓库」范围时）。后者在根内根本无法表达，
 * 必须换成覆盖它的根（清单里的项目/会话根；都没有就用仓库根建一个 `abs:` 临时根）。
 * 换算全在 `files/repo-paths.ts`（纯函数 + 单测）。
 */
function resolveRepoPath(repoRel: string): ResolvedRepoPath | null {
  const rootId = explorer.getRoot()
  const rootAbs = rootAbsOf(rootId)
  const repoRootAbs = repoRootAbsOf(rootId)
  if (!rootAbs || !repoRootAbs) return null
  const resolved = resolveRepoPathPure({
    repoRel,
    rootId,
    rootAbs,
    repoRootAbs,
    roots: state.roots,
    isWin: IS_WIN,
    writable: !!state.rootsResp?.writable,
  })
  // 临时根要登记进根清单：根选择器的名字、资源管理器的定位都查它，
  // 不登记就会出现「标签页在一个根上、根名字却显示成 id 原文」的割裂
  if (resolved?.create && !state.roots.some((r) => r.id === resolved.create!.id)) state.roots.push(resolved.create)
  return resolved
}

/** 语言服务器「转到定义」时临时建过的**库根**（`abs:` 型）：目标是库根时不动左栏，见 `openLspTarget`。 */
const libraryRoots = new Set<string>()

/**
 * 语言服务器「转到定义」的落地（`LspJumpTarget` 两态）。
 *
 * - **根内**：必要时把左栏切到目标所在的根，然后开标签；
 * - **工作区外**（库文件：`/usr/include/c++/13/string`、GOROOT 标准库、rust-src、site-packages/typeshed）：
 *   按绝对路径找一个能盖住它的根——已有的根优先，否则按**库锚点**建一个 `abs:` 临时根并登记
 *   （库文件不在任何工程里，没有现成根可用）。
 *
 * **两处都不抢左栏**：库文件是「看一眼定义」的只读材料，把资源管理器搬到 `/usr/include`（或从
 * `/usr/include` 下方继续跳时又切一次）不是用户要的——实测踩过：从 `main.cpp` 跳到标准库头，
 * 再在头文件里跳一次，左栏就跑到 `include` 根上、项目树消失了。因此本次跳转**建过的**库根记在
 * `libraryRoots` 里，目标是库根时不切根（库根仍是真根：标签能读、根选择器里选得到）。
 *
 * 两处跳过：沙箱模式（服务端 `abs:` 根一律 403，提前说清楚而不是开个空白标签）与无法定位的路径。
 * 语言提示与会话复用（`languageHint` / `lspReuseFrom`）透传下去：无扩展名的库文件靠它拿到正确语言，
 * 且由**跳转来源那个服务器**继续答语义问题（它已把该文件纳入索引、手里有编译参数）。
 */
function openLspTarget(jump: LspJumpTarget): void {
  const origin = {
    line: jump.line,
    column: jump.column,
    mode: "view" as const,
    languageHint: jump.language || undefined,
    lspReuseFrom: jump.sourceModel ?? undefined,
  }
  if (jump.inside) {
    const { rootId, path } = jump.inside
    if (rootId && rootId !== explorer.getRoot() && !libraryRoots.has(rootId)) void explorer.setRoot(rootId)
    // 列号一并带上：服务器回的是**标识符**的范围（名称本身），不丢列号跳转就会停在行首
    void openFile(rootId, path, origin)
    return
  }
  const abs = jump.absPath
  if (!abs) return
  if (state.rootsResp?.sandboxed) {
    toast(`该定义位于工作区外（${abs}）：沙箱模式下不开放工作区外文件`, "warn", 8000)
    return
  }
  const resolved = resolveAbsPath({ abs, roots: state.roots, isWin: IS_WIN, writable: !!state.rootsResp?.writable })
  if (!resolved) {
    toast(`无法定位该定义：${abs}`, "warn", 6000)
    return
  }
  // 临时根要登记进根清单（与变更面板打开根外文件同一口径）：根选择器/状态栏的根名都查它。
  // 同时记入 libraryRoots：之后在库文件内部继续跳转时，不因“目标在另一个根”而把左栏搬走。
  if (resolved.create) {
    libraryRoots.add(resolved.create.id)
    if (!state.roots.some((r) => r.id === resolved.create!.id)) state.roots.push(resolved.create)
  }
  void openFile(resolved.root, resolved.rel, origin)
}

/** 打开仓库内的任意文件（入参为**仓库相对**路径）：自动选定能打开它的根。 */
function openRepoFile(repoRel: string, opts: { line?: number; preview?: boolean } = {}): void {
  const r = resolveRepoPath(repoRel)
  if (!r) {
    toast(`无法定位文件：${repoRel}（未识别到它所在的 Git 仓库）`, "error")
    return
  }
  void openFile(r.root, r.rel, { preview: opts.preview ?? false, line: opts.line })
}

/**
 * 「在资源管理器中定位」：**切到文件所在的根**再展开定位。
 *
 * 与 `openRepoFile` 分开是有意的：打开文件不该动左栏（连点几个不同目录的文件时树会来回跳），
 * 而「定位」这个菜单项说的就是「带我去看它在树里的位置」——根不同时必须换根，否则那棵树里
 * 根本没有这个文件（跨根时在旧行为下会去列一个不存在的目录）。
 */
function revealRepoInExplorer(repoRel: string): void {
  const r = resolveRepoPath(repoRel)
  if (!r) {
    toast(`无法定位文件：${repoRel}（未识别到它所在的 Git 仓库）`, "error")
    return
  }
  void (async () => {
    if (r.root !== explorer.getRoot()) await explorer.setRoot(r.root)
    await explorer.reveal(r.rel, { select: true })
  })()
}

async function onRootChanged(rootId: string): Promise<void> {
  // 先定「仓库内前缀」再刷新：变更面板的范围芯片、Git 面板的子目录限定都读它，
  // 顺序反了（先刷新、后算前缀）会让它们先按「根 = 仓库根」渲染一次，而之后未必再有渲染。
  state.repoPrefix = repoPrefixNow(rootId) ?? ""
  await refreshGit()
  if (state.gitViewVisible && gitPanel) void gitPanel.refresh()
  // 终端跟随根（开关在面板里；关掉时本调用无副作用）
  termPanel?.onRootChanged()
  renderRail()
  // 换根也是状态记忆的一部分（下一次写回时读的就是新根）
  persistSession()
}

/** 同根进行中的 git 状态请求（启动期 boot 与 onRootChanged 会先后触发，合并为一次往返）。 */
let gitStatusInFlight: { root: string; promise: Promise<GitStatusInfo | null> } | null = null
/** 最近一次成功的拉取（同根 500ms 内不重复请求：两个触发点相邻时真正只发一次）。 */
let lastGitFetch: { root: string; ts: number } | null = null

async function refreshGit(force = false): Promise<GitStatusInfo | null> {
  const root = explorer.getRoot()
  // 启动期 boot 与 onRootChanged（setRoot 内）会先后触发同一根的刷新——复用进行中的请求
  const inflight = gitStatusInFlight
  if (inflight && inflight.root === root) return inflight.promise
  // 刚拉过同一根：调用方要的是「状态就绪」而不是「必须再问一次」（git 状态 500ms 内的陈旧无感知）
  // 写操作后必须 force：否则刚录入的改动会被上一次的缓存状态盖回去（面板上看不到自己刚做的事）
  if (!force && lastGitFetch && lastGitFetch.root === root && Date.now() - lastGitFetch.ts < 500) {
    renderStatus()
    return state.gitStatus
  }
  const promise = (async (): Promise<GitStatusInfo | null> => {
    try {
      const status = await api.gitStatus(root)
      state.gitStatus = status
      state.gitStatusError = null
      lastGitFetch = { root, ts: Date.now() }
      // 回填树的 Git 装饰：树首次渲染时状态还没到（异步），不回填则徽标/下划线永不出现
      explorer.refreshGitDecorations()
      // 仓库内前缀：优先根清单（打开前就能算），状态接口的 repoRoot 是更权威的来源
      if (status.isRepo) {
        // 状态到手后复算前缀：`status.repoRoot` 是仓库根的权威来源（子目录根常常不在根清单里，
        // 只有它带得出仓库根——少了这一步，子目录根会被当成仓库根，变更面板与路径换算全错）
        state.repoPrefix = repoPrefixNow(root) ?? ""
      }
    } catch (err) {
      // 读失败 ≠ 不是仓库：错误要留给状态栏与面板显示（否则会把「初始化仓库」当成正确入口）
      state.gitStatus = null
      state.gitStatusError = (err as Error).message
      explorer.refreshGitDecorations()
    }
    renderStatus()
    // 变更面板与状态栏同源：状态一变就同步（只在它已创建时刷新，避免无谓重渲染）
    changesPanel?.refresh()
    // Git 修改标记：HEAD oid 变了要作废基线缓存（提交/重置后标记跟新基线）
    noteGitOidForGutter()
    return state.gitStatus
  })()
  gitStatusInFlight = { root, promise }
  try {
    return await promise
  } finally {
    if (gitStatusInFlight?.promise === promise) gitStatusInFlight = null
  }
}

/** 差异视图里做了写操作（逐块暂存／取消／丢弃）后的统一刷新：状态、状态栏、变更面板与工具窗。 */
function onDiffChanged(): void {
  void refreshGit(true).then(() => {
    if (state.gitViewVisible && gitPanel) void gitPanel.refresh()
  })
}

/* ------------------------------ Git 修改标记（dirty-diff gutter） ------------------------------ */

/**
 * HEAD 基线缓存：`root|repoRel` → { oid, text }。oid 变（新提交/重置/ amend）即失效重取；
 * 同 oid 下多标签共享（切换标签不重复请求）。文本量与文件本体同量级，只缓存当前文件一项，
 * 标签关闭时清（见 forceClose 处的 gitBaseCache.delete）。
 */
const gitBaseCache = new Map<string, { oid: string; text: string | null }>()
/** 同一标签进行中的基线请求（防抖：编辑/保存/git 事件密集时只发一次）。 */
const gitBaseInflight = new Map<string, Promise<string | null>>()
/** 上次应用基线时的 HEAD oid（refreshGit 后对比，变了才作废缓存——提交/重置后标记自动跟上）。 */
let gitBaseOid = ""

/** 仓库相对路径（当前根内）：不在仓库里返回 null（与路径换算同口径，见 repo-paths.ts）。 */
function repoRelOfTab(tab: Tab): string | null {
  if (!state.gitStatus?.isRepo || !state.gitStatus.repoRoot) return null
  const abs = absOfRepo(rootAbsOf(tab.root), tab.path, IS_WIN)
  const rel = relWithin(state.gitStatus.repoRoot, abs, IS_WIN)
  return rel
}

/** 把 HEAD 基线应用到编辑器（或撒下标记）：标签就绪、git 状态到达、切标签时调。 */
async function applyGitGutter(tab: Tab): Promise<void> {
  if (activeTab()?.id !== tab.id || !tab.editor) return
  const repoRel = tab.kind === "file" ? repoRelOfTab(tab) : null
  const oid = state.gitStatus?.oid ?? ""
  if (!repoRel || !oid) {
    tab.gitGutterOn = false
    tab.editor.setGitBase(null)
    return
  }
  if (tab.gitGutterOn && oid === gitBaseOid) return // 已按当前 HEAD 应用过：无需重复
  const key = `${tab.root}|${repoRel}`
  let cached = gitBaseCache.get(key)
  if (!cached || cached.oid !== oid) {
    let p = gitBaseInflight.get(key)
    if (!p) {
      p = (async () => {
        try {
          const r = await api.gitContent(tab.root, "HEAD", repoRel)
          return r.missing || r.binary || r.tooLarge ? null : r.content
        } catch {
          return undefined as never // 请求失败：用 undefined 区分「不可用」（null）与「未知」（不撒标记，下次再试）
        }
      })()
      gitBaseInflight.set(key, p)
      void p.finally(() => gitBaseInflight.delete(key))
    }
    const text = await p
    if (activeTab()?.id !== tab.id) return // 等待期间切走了：切回来时 activate 会再触发
    if (text === undefined) return
    cached = { oid, text }
    gitBaseCache.set(key, cached)
  }
  // 资格终判（editorKind / truncated 在这里才齐）：不满足时撒标记，但缓存留给下次（切回同文件复用）
  const eligible = gutterEligible({ isRepo: true, headAvailable: cached.text !== null, truncated: !!tab.truncated, editorKind: tab.editor.kind })
  tab.gitGutterOn = eligible
  tab.editor.setGitBase(eligible ? cached.text : null)
}

/** refreshGit 尾部：HEAD oid 变了 → 缓存逐项作废（提交/重置后标记自动跟上新基线）。 */
function noteGitOidForGutter(): void {
  const oid = state.gitStatus?.oid ?? ""
  if (oid && oid !== gitBaseOid) {
    gitBaseOid = oid
    // oid 变化但缓存项的 oid 已不同 → 惰性作废即可（applyGitGutter 里比对），这里只主动清当前标签外的项
    for (const [k, v] of gitBaseCache) if (v.oid !== oid) gitBaseCache.delete(k)
    const t = activeTab()
    if (t && t.kind === "file" && t.editor) {
      t.gitGutterOn = false
      void applyGitGutter(t)
    }
  }
}


/* ------------------------------ 变更监听（长轮询 + 后端 fs.watch） ------------------------------ */

/**
 * 「自己刚写完」的文件在短时间内不回读：保存本身就是一次磁盘写入，watch 会把它当成外部变更。
 * 不回读的原因是回读会把编辑器重建/覆盖一遍——用户刚敲完字就被重载，光标与撤销栈都会乱。
 */
const SELF_WRITE_GRACE_MS = 3_000
const selfWrites = new Map<string, number>()

/**
 * 长轮询唤醒后的刷新调度。
 *
 * 为什么要合并：一次保存、一次 git 操作、一次 Agent 批量写文件都会触发**一串**事件（同一批里
 * 可能有几十条路径）。逐条刷新会让目录树与状态栏在一秒里重画几十次；这里按 400ms 窗口把
 * 一批事件合成一次「目录增量刷新 + 变更面板刷新」。
 */
const FS_DEBOUNCE_MS = 400
let pendingPaths = new Set<string>()
let pendingAll = false
let pendingTimer: number | null = null

/**
 * Git 状态/工具窗刷新：与 fs 刷新分开（它们的成本与可见性条件不同）——
 * 只在 Git 面板真的看得见时才连带刷它内部三栏（分支/日志查询比一次 status 贵得多）。
 * 用**尾沿合并**：事件密集时（Agent 连续写文件）最多每 GIT_REFRESH_MAX_WAIT 刷一次。
 */
const GIT_REFRESH_DEBOUNCE_MS = 600
const GIT_REFRESH_MAX_WAIT_MS = 3_000
let gitTimer: number | null = null
let gitWindowStart = 0

function scheduleGitRefresh(): void {
  const now = Date.now()
  if (gitTimer !== null) {
    if (now - gitWindowStart < GIT_REFRESH_MAX_WAIT_MS) {
      window.clearTimeout(gitTimer)
      gitTimer = null
    } else return // 已到最长等待：让 已排队的这一拍先跑
  }
  if (gitTimer === null) {
    gitWindowStart = now
    gitTimer = window.setTimeout(() => {
      gitTimer = null
      void refreshGit(true).then(() => {
        if (state.gitViewVisible && gitPanel) void gitPanel.refresh()
      })
    }, GIT_REFRESH_DEBOUNCE_MS)
  }
}

/** 要监听的根内目录：根 + 已展开目录 + 打开文件的父目录（含上限，由前端 watch-core 与后端各夹一次）。 */
function watchedDirs(): string[] {
  const out = [""]
  out.push(...explorer.expandedDirs())
  for (const t of state.tabs) {
    if (t.kind !== "file" || t.root !== explorer.getRoot()) continue
    const idx = t.path.lastIndexOf("/")
    out.push(idx > 0 ? t.path.slice(0, idx) : "")
  }
  return out
}

/** 记录一批变更（watch 回调）：按窗口合并后统一刷新。 */
function noteFsChange(paths: string[] | null): void {
  // 文件索引跟着失效：新建/删掉的文件应立即反映在快速打开里（否则得等 TTL，用户会以为搜不到）
  invalidateQuickOpenIndex()
  if (!paths || !paths.length) pendingAll = true
  else for (const p of paths) pendingPaths.add(p)
  if (pendingTimer !== null) return
  pendingTimer = window.setTimeout(flushFsChanges, FS_DEBOUNCE_MS)
}

function flushFsChanges(): void {
  pendingTimer = null
  const all = pendingAll
  const paths = all ? null : [...pendingPaths]
  pendingAll = false
  pendingPaths = new Set()
  // 目录树：只刷新「已缓存且真的变了」的那几块（见 explorer.syncDirs）
  void explorer.syncDirs(paths)
  // 已打开且未修改的文件：磁盘内容变了就地重载（正在编辑/有未保存改动的标签不动，保存时自有三选一）
  if (paths) void reloadChangedTabs(paths)
  // 工作区变了，Git 状态与变更面板大概率也变了（新文件=未跟踪、改文件=已修改）
  scheduleGitRefresh()
}

/**
 * 变更路径命中已打开的文件时，把「干净」的标签从磁盘重载（保留滚动位置，不重建编辑器）。
 *
 * 比较用**绝对路径**：监听端点给的是「当前根」的相对路径，而标签可能属于别的根
 * （变更面板里点开根之外的文件 → 自动换根打开）——按 (根, 路径) 逐字段比会漏掉它们
 * （`tab.root !== 当前根` 直接被跳过），于是那些文件永远不会自动重载。
 */
async function reloadChangedTabs(paths: string[]): Promise<void> {
  const currentRootAbs = rootAbsOf(explorer.getRoot())
  const changed = new Set(paths.map((p) => normPath(absOfRepo(currentRootAbs, p), IS_WIN)))
  /**
   * 「这一轮不该被自动重载」：有未保存改动 / 已在编辑态 / 不是编辑器标签（查看器的重载会丢播放与缩放位置，
   * 交给用户手动刷新）。写成函数而不是内联条件，是为了 `await` 之后再判一次——两次判定之间用户随时可能开始打字。
   */
  const busy = (t: Tab): boolean => t.dirty || t.mode === "edit" || !t.editor
  for (const tab of [...state.tabs]) {
    if (tab.kind !== "file") continue
    const tabAbs = normPath(absOfRepo(rootAbsOf(tab.root), tab.path), IS_WIN)
    if (!tabAbs || !changed.has(tabAbs) || busy(tab)) continue
    const key = `${tab.root}|${tab.path}`
    const selfTs = selfWrites.get(key)
    if (selfTs && Date.now() - selfTs < SELF_WRITE_GRACE_MS) continue
    try {
      const read = await api.read(tab.root, tab.path, { maxBytes: Math.min(state.rootsResp?.maxRead ?? 10 * 1024 * 1024, 10 * 1024 * 1024) })
      if (findTab(tab.id) !== tab || busy(tab)) continue
      const editor = tab.editor
      if (!editor) continue
      if (read.etag === tab.etag) continue // 只是 mtime 抖了一下：不重建
      const top = editor.getScrollTop()
      tab.content = read.content
      tab.baseline = read.content
      tab.etag = read.etag
      tab.encoding = read.encoding
      tab.eol = read.eol
      editor.setValue(read.content)
      editor.markClean()
      editor.setScrollTop(top)
      renderStatus()
      /**
       * 后台标签**静默重载**：内容确实变了（etag 不同）才走到这里，但只有用户正在看的那一个
       * 需要说一声（文字在他眼皮下变了，不解释会以为是自己误操作）；后台标签换了内容没人看见，
       * 弹提示反而是自动刷新在刷存在感。
       */
      if (state.activeId === tab.id && !document.hidden) toast(`${tab.title} 已在磁盘上更新，已重新加载`, "info", 4000)
    } catch {
      // 文件被删/被移动：不打扰用户（下一次保存会给出明确错误）
    }
  }
}

/** 变更监听实例：目录清单来自当前展开态，git 事件与 fs 事件分别走两条刷新路径。 */
const fsWatcher = createFsWatcher({
  api,
  root: () => explorer.getRoot(),
  dirs: watchedDirs,
  git: () => state.gitStatus?.isRepo !== false,
  onGitChange: () => scheduleGitRefresh(),
  onFsChange: (paths) => noteFsChange(paths),
})

/* ------------------------------ 地址栏同步 ------------------------------ */

/**
 * 地址栏 = 界面状态（root + 定位路径 + 行号）。
 * 目录切换走 pushState（可后退回上一个目录），同目录内开文件走 replaceState；
 * 刷新/前进后退经 restoreFromUrl 回到原处。
 */
const urlSync = createUrlSync({
  read: () => {
    const t = activeTab()
    const sel = explorer.selected()
    const path = t && t.kind === "file" ? t.path : (sel?.path ?? "")
    const line = t?.kind === "file" ? state.cursor.line : undefined
    // 地址栏说的是「当前打开的是什么」，所以文件标签优先用**它自己的根**：
    // 跨根打开（变更面板里点开根之外的改动）时若沿用左栏的根，这条 path 就指错了地方——
    // 刷新/前进后退会按当前根重新解释它，打开的是另一个文件（或干脆打不开）。
    const root = t && t.kind === "file" ? t.root : explorer.getRoot()
    return { root, path, line, session: state.sessionId }
  },
  onPop: (st) => restoreFromUrlState(st),
})

/** 从地址栏恢复（启动与浏览器前进后退共用）。 */
async function restoreFromUrlState(st: Partial<{ root: string; path: string; line?: number }>): Promise<void> {
  const rootId = st.root ?? explorer.getRoot()
  if (st.root && st.root !== explorer.getRoot()) {
    await explorer.setRoot(st.root, undefined)
  }
  if (!st.path) return
  // 路径归一必须走与深层链接同一份规则：消息流产物路径带 `tmp/`（服务端逻辑路径），
  // 而会话根本身就指向 `…/tmp`——直接当根内路径用会多出一级（`tmp/tmp/…`，服务端 404），
  // 同时项目根下的 `tmp/` 是正当目录名、不该被剥（规则按根类型分叉，见 normalizeArtifactPath）。
  const path = normalizeArtifactPath(state.roots.find((r) => r.id === rootId)?.kind, st.path)
  // 目录 → 展开并在树中定位；文件 → 打开（浅层链接解析已在 deeplink.ts 完成根推断）
  const isLikelyDir = !path.includes(".")
  if (isLikelyDir) {
    await explorer.reveal(path, { select: true })
  } else {
    await openFile(rootId, path, { preview: false, line: st.line })
  }
}

/** 启动恢复：优先用已有的 ?root/?path（deeplink 已在 loadRoots 里落位），否则按根清单推断。 */
async function restoreFromUrl(): Promise<void> {
  const st = parseUrlState(location.search)
  if (st.path) await restoreFromUrlState(st)
  else if (st.root && st.root !== explorer.getRoot()) await explorer.setRoot(st.root, undefined)
}

/* ------------------------------ 状态记忆（刷新保留） ------------------------------ */

/**
 * 工作台状态记忆：打开的标签 / 活动标签 / 当前根 / 左栏视图写进 `sessionStorage`
 * （见 `files/session-state.ts`），刷新（含 dev-reload）后回到原处。
 *
 * 为何不与用户级偏好共用 localStorage：这是**本标签页的会话状态**——独立打开的 `/files` 标签页
 * 与分屏 iframe 里的工作台（同源 iframe，与宿主共享同一份 sessionStorage）应该各记各的；
 * 用 localStorage 会互相覆盖，关掉一个标签页还会把另一处的记忆一起带走。
 *
 * 只记普通文件标签：差异 / 合并 / 暂存 / 比较标签各自需要打开时的上下文（端点对、冲突文件、
 * 比较两端），拿一个路径恢复不出来——它们本就是由文件派生的临时视图，刷新后重新打开即可。
 */
let lastFileTabId: string | null = null
let persistTimer: number | null = null

/** 记忆里的「最近活动的文件标签」已不在清单里时，回落到当前最后一个文件标签（关标签后调用）。 */
function refreshLastFileTab(): void {
  if (lastFileTabId && state.tabs.some((t) => t.id.startsWith("file:") && tabKey(t.root, t.path) === lastFileTabId)) return
  const last = [...state.tabs].reverse().find((t) => t.id.startsWith("file:"))
  lastFileTabId = last ? tabKey(last.root, last.path) : null
}

/** 收集当前状态（「哪些标签值得记」的判据只在这里）。 */
function collectSession(): FwSessionState {
  const tabs: FwTabState[] = []
  for (const t of state.tabs) {
    if (!t.id.startsWith("file:")) continue
    tabs.push({
      root: t.root,
      path: t.path,
      // 有未保存修改的标签按查看态记：内容跨不了刷新，别让人以为改动还在（恢复时另给一次提示）
      mode: t.dirty ? "view" : t.mode,
      line: t.id === state.activeId ? state.cursor.line : t.cursorLine,
      dirty: t.dirty || undefined,
    })
  }
  return { root: explorer.getRoot(), tabs, active: lastFileTabId ?? undefined, leftView: state.leftView, leftVisible: leftPanelShown() }
}

/** 节流写回（切标签、移动光标都在调它，同期内的多次调用合并成一次写）。 */
function persistSession(): void {
  if (persistTimer !== null) return
  persistTimer = window.setTimeout(() => {
    persistTimer = null
    saveSession(collectSession())
  }, 200)
}

/** 立即写回（页面卸载前兜底：节流窗口内离开也不能把这次状态丢了）。 */
function flushSession(): void {
  if (persistTimer !== null) {
    window.clearTimeout(persistTimer)
    persistTimer = null
  }
  saveSession(collectSession())
}

window.addEventListener("pagehide", flushSession)

/* ------------------------------ 编辑位置历史（后退 / 前进） ------------------------------ */

/**
 * 位置栈（见 files/nav-history.ts）：一条线性回溯线 + 指针，VSCode navigateBack/Forward 同款。
 *
 * `navGuard` 的两个用途：
 * ① **历史回放不回写**——后退/前进本身调的是 openFile/activate，不抑制的话每次回放又把自己
 *    push 进栈，指针永远追着尾巴跑，连退几步退不动；
 * ② **启动恢复静默**——restoreSession/restoreFromUrl 把上次的标签逐个重开，那是「回到原处」
 *    而不是「一次导航」，灌进栈的话第一件事就是后退回启动页。
 */
const nav = createNavHistory()
let navGuard = 0

/** 记一次导航（openFile 的出口处调，见 nav-history 的「记录点」注释）。恢复/回放期间静默。 */
function navRecord(e: NavEntry): void {
  if (navGuard) return
  nav.jump(e)
}

/** 光标移动修正栈顶（onCursor 出口）：后退/前进回到的是「离开时最后看的位置」。 */
function navTrack(): void {
  const t = activeTab()
  if (!t || t.kind !== "file" || navGuard) return
  const pos = t.editor?.getCursorPos()
  if (pos) nav.updateTop({ root: t.root, path: t.path, line: pos.line, column: pos.column })
}

/** 历史回放期间的包裹器（同步段与异步段都在内）：期间 navRecord/navTrack 静默。 */
async function withNavGuard(fn: () => Promise<void> | void): Promise<void> {
  navGuard++
  try {
    await fn()
  } finally {
    navGuard--
  }
}

/** 后退（dir=-1）/ 前进（dir=1）：落地目标位置；标签已关则重新打开。 */
async function navGo(dir: 1 | -1): Promise<void> {
  const target = dir === -1 ? nav.back() : nav.forward()
  if (!target) return
  await withNavGuard(async () => {
    const id = tabId("file", target.root, target.path)
    const exist = findTab(id)
    if (exist) {
      activate(id)
      exist.editor?.revealLine(target.line - 1, target.column)
    } else {
      // 标签已关：重新打开（预览槽不占用——这是回溯，不该顺手顶掉用户正在看的预览）
      await openFile(target.root, target.path, { preview: false, line: target.line, column: target.column })
    }
  })
  renderTabActionsOnly()
}

/** 只重绘动作区（后退/前进后按钮可用态变了，不必重建整条标签栏）。 */
function renderTabActionsOnly(): void {
  clear(tabActionsHost)
  renderTabActions(tabActionsHost)
}

/**
 * 刷新后恢复：按记忆逐个打开上次的标签（回到刷新前的位置与编辑态），最后落回记忆里的活动标签。
 * 根不在清单里的标签跳过（项目被移除 / 换了会话）——照常打开只会得到一串打不开的错误页。
 */
async function restoreSession(s: FwSessionState): Promise<void> {
  // 恢复不是导航：重开上次的标签不该进位置历史（否则首动作就是「后退回启动页」）
  await withNavGuard(async () => {
    const known = new Set(state.roots.map((r) => r.id))
    if (s.root && known.has(s.root) && s.root !== explorer.getRoot()) await explorer.setRoot(s.root)
    let dirty = 0
    for (const ref of s.tabs) {
      if (!known.has(ref.root)) continue
      if (ref.dirty) dirty++
      await openFile(ref.root, ref.path, { preview: false, line: ref.line, mode: ref.mode })
    }
    if (s.active) {
      const t = state.tabs.find((x) => x.id.startsWith("file:") && tabKey(x.root, x.path) === s.active)
      if (t) activate(t.id)
    }
    // 左栏：视图 + 显隐（阶段一建的是资源管理器且保持隐藏，这里按记忆落位）
    const lv = s.leftView ?? "explorer"
    if (lv === "explorer") showLeftView("explorer", { keepHidden: s.leftVisible === false })
    else {
      showLeftView(lv)
      if (s.leftVisible === false) setLeftVisible(false)
    }
    if (dirty) toast(`有 ${dirty} 个文件未保存的修改未能保留（已按磁盘内容打开）`, "warn", 6000)
  })
}

/* ------------------------------ 标签页 ------------------------------ */

function tabId(kind: string, root: string, path: string, extra = ""): string {
  return `${kind}:${root}:${path}${extra}`
}

function activeTab(): Tab | null {
  return state.tabs.find((t) => t.id === state.activeId) ?? null
}

function findTab(id: string): Tab | undefined {
  return state.tabs.find((t) => t.id === id)
}

const viewHosts = new Map<string, HTMLElement>()

/**
 * 标签栏右侧的**动作轮盘**（容器挂 body，不随 clear(tabActionsHost) 消失）。
 * 标签栏每次重建都会换一个，所以重建前必须把上一个 destroy 掉——否则每重建一次就多留一份
 * 扇形 DOM 与一组 document 监听（典型症状：点了菜单里的一项，同一次交互触发好几次）。
 */
let tabWheel: WheelHandle | null = null

async function openFile(root: string, path: string, opts: { preview?: boolean; line?: number; column?: number; forceText?: boolean; mode?: "view" | "edit"; only?: boolean; languageHint?: string; lspReuseFrom?: Model | null } = {}): Promise<void> {
  if (!path) return
  recordRecentFile(root, path) // 「快速打开」空查询时的「最近打开」列表
  const id = tabId("file", root, path)
  const exist = findTab(id)
  /**
   * 「只要这一个」（双击树里的文件，见 explorer.ts）：
   * 双击是一个**明确的“我要看这个”**动作，它开出来的标签是常驻；而“就位替换”那条规则本来只适用于
   * 单击预览（旧预览没价值、顶掉即可）。双击时点到的文件往往是自己刚排过的一个预览（连点两下），
   * 而浏览器在双击时只给**一次 click + 一次 dblclick**（第二发 click 被吞），于是走的是“标签已存在”
   * 分支、只把它钉住——先前那个无关的预览（A）就留下来了（实测：双击 B 后树上多出 A、B 两个标签）。
   * 所以：双击路径先把预览槽清掉，再把目标标签建/钉成常驻。
   */
  if (opts.only) {
    for (const t of [...state.tabs]) {
      if (t.preview && t.id !== id) forceClose(t.id)
    }
  }
  if (exist) {
    // 已开着的标签：预览态只归双击树行 / 标签栏双击 / 右键菜单管——命中已有标签时**不**在这里改它的
    // 预览态（否则“单击一个已固定的文件”会把它悄悄降回预览，接着双击的第一发又把第二发钉住——
    // 两次交互互相抵消，用户看到的是“双击没反应”）。固定与取消固定都是显式动作（见下面标签栏的
    // ondblclick 与右键菜单）。
    if (opts.preview === false) exist.preview = false
    activate(id)
    if (opts.line && exist.editor) {
      exist.editor.revealLine(opts.line - 1, opts.column ?? 1)
      // 同文件显式跳行（转定义落在已开标签、符号面板）也是一次导航；无 line 的命中已有标签不算
      navRecord({ root, path, line: opts.line, column: opts.column ?? 1 })
    }
    return
  }
  // 预览标签：单击树里的文件时复用同一个预览标签（VSCode 行为），双击/固定时转为常驻
  // 恢复标签（见 restoreSession）时 preview 传 false、mode 传记忆值（未保存修改的标签已归一为查看态）
  if (opts.preview !== false) {
    const prev = state.tabs.find((t) => t.preview && t.kind === "file")
    if (prev) {
      const sameId = prevId(prev, root, path)
      if (sameId) {
        // 就位替换：被顶掉的旧预览**不再算预览**（否则它会被 activate 的“离开预览就落定”当成
        // “用户切走了”，从斜体预览被转成常驻标签永久留下来——实测症状：双击 B 之后，
        // 先前的预览 A 变成第二个常驻标签，同时双击的那个文件自己反而只是预览）。
        prev.preview = false
        state.tabs = state.tabs.filter((t) => t !== prev)
        viewHosts.get(prev.id)?.remove()
        viewHosts.delete(prev.id)
        prev.diffDispose?.()
        prev.editor?.dispose()
        prev.viewDispose?.()
      }
    }
  }
  const host = h("div", { class: "fw-tab-view" })
  const tab: Tab = {
    id,
    kind: "file",
    root,
    path,
    title: path.split("/").pop() ?? path,
    preview: opts.preview !== false,
    host,
    mode: opts.mode === "edit" ? "edit" : "view",
    dirty: false,
    baseline: "",
    content: "",
    encoding: "utf-8",
    eol: "lf",
    etag: "",
  }
  state.tabs.push(tab)
  views.appendChild(host)
  viewHosts.set(id, host)
  activate(id)
  // 位置历史：打开/切换文件 = 一次导航（启动恢复与历史回放被 navGuard 静默）。
  // 新开标签的 line 可能后到（loadTab 完成后 reveal），栈里先记目标行，没有则记 1
  navRecord({ root, path, line: opts.line ?? 1, column: opts.column ?? 1 })
  await loadTab(tab, { line: opts.line, column: opts.column, forceText: opts.forceText, languageHint: opts.languageHint, lspReuseFrom: opts.lspReuseFrom })
  renderTabbar()
  persistSession()
}

/**
 * 语言服务器挂载：本机有该语言的服务器时把文档交给它（补全 / 悬停 / 跳转 / 诊断）。
 * 无服务器（未安装 / GEBAI_LSP=false / 沙箱）时内部直接返回——不建连接、不注册任何 provider。
 */
function attachLsp(tab: Tab, editor: EditorHandle, language: string, reuseFrom?: Model | null): void {
  const model = editor.model()
  if (!model) return
  void attachDocument({ model, rootId: tab.root, path: tab.path, language, reuseFrom }).then((server) => {
    // 挂载是异步的（首次要拉起服务器进程）：就绪后补绘状态栏，把服务器名显示出来
    if (server) renderStatus()
  })
}

function prevId(prev: Tab, root: string, path: string): boolean {
  return prev.root === root && prev.path !== path
}

/** 加载标签内容：文本/图表走 Monaco；其它走对应查看器。 */
async function loadTab(tab: Tab, opts: { line?: number; column?: number; forceText?: boolean; languageHint?: string; lspReuseFrom?: Model | null } = {}): Promise<void> {
  const host0 = viewHosts.get(tab.id)
  if (!host0) return
  /**
   * 每次加载取一个令牌：`await` 回来后标签可能已被关闭、或被新一轮加载接管。
   * 没有这道守卫时（打开大文件后立刻 Alt+W、连点「重新加载」），异步回来的代码会把新编辑器
   * 挂到已从文档移除的 host 上——它永远不会被释放（model 还捐着 MB 级字符串）。
   */
  const gen = (tab.loadGen ?? 0) + 1
  tab.loadGen = gen
  const stale = (): boolean => tab.loadGen !== gen || findTab(tab.id) !== tab
  // 重建视图前先释放上一轮的编辑器/查看器（「重新加载」、「以文本打开」、图表源码↔预览切换
  // 都会走到这里，早期每点一次就漏一个 Monaco 实例 + model）
  tab.editor?.dispose()
  tab.editor = undefined
  tab.viewDispose?.()
  tab.viewDispose = undefined
  clear(host0)
  host0.appendChild(h("div", { class: "fw-loading", text: `正在打开 ${tab.title}…` }))
  tab.previewKind = undefined // 重建即回到源码态（点「渲染预览」时再置位）
  /** 脏标记复核计时器（见 onChange） */
  let dirtyTimer: number | null = null
  try {
    const statRes = await api.stat(tab.root, [tab.path])
    if (stale()) return
    const stat = statRes.items[0]
    if (!stat) throw new Error("文件不存在")
    tab.stat = stat
    if (stat.type === "dir") {
      clear(host0)
      host0.appendChild(placeholderFor(`「${tab.title}」是目录`, "目录请在左侧资源管理器中展开浏览。"))
      return
    }
    const useText = opts.forceText || stat.kind === "text" || stat.kind === "diagram" || !["image", "video", "audio", "pdf", "office", "archive", "font", "binary"].includes(stat.kind)
    if (useText) {
      const read: ReadResponse = await api.read(tab.root, tab.path, { maxBytes: Math.min(state.rootsResp?.maxRead ?? 10 * 1024 * 1024, 10 * 1024 * 1024), forceText: opts.forceText })
      if (stale()) return
      tab.content = read.content
      tab.baseline = read.content
      tab.encoding = read.encoding
      tab.eol = read.eol
      tab.etag = read.etag
      tab.truncated = read.truncated
      tab.binary = read.binary
      clear(host0)
      if (read.binary) {
        tab.viewDispose = renderViewer(host0, viewerCtx(tab))
        return
      }
      const editorHost = h("div", { class: "fw-editor-host" })
      host0.appendChild(editorHost)
      if (read.truncated) {
        host0.appendChild(h("div", { class: "fw-banner warn" }, [icon("warning"), h("span", { text: `文件较大（${formatSize(read.size)}），仅加载前 ${formatSize(read.content.length)}。为保护浏览器与避免误保存，编辑已禁用，请下载后编辑。` })]))
      }
      // 生效语言：路径判定为 plaintext 或 C 家族歧义时用**跳转来源文档的语言**兜底
      // （库文件常无扩展名：`/usr/include/c++/13/string`；`.h` 在 C++ 库里其实是 C++）
      const language = effectiveLanguageOf(read.language, opts.languageHint)
      // 回写进 stat：状态栏「当前语言」、符号面板提示与 provider 选择器都读它，一处生效处处一致
      tab.stat = { ...stat, language }
      const editor = await createEditor(editorHost, {
        value: read.content,
        language,
        readOnly: tab.mode !== "edit" || read.truncated,
        menu: {
          // 绝对路径**取时现算**：根清单/临时 abs 根（变更面板里点开根之外的文件时会登记）都可能后到
          absPath: () => absOfRepo(rootAbsOf(tab.root), tab.path, IS_WIN),
          // 「发送会话」**仅分屏（被主界面嵌入）时给**：独立标签页里没有对话输入框可发
          sendToChat: EMBEDDED ? requestSendToChat : undefined,
          // 自定义组（Git 历史）随标签状态变文案，句柄要用来判断编辑器能力：见 gitMenuGroup
          groups: (ed) => gitMenuGroup(tab, ed.kind),
        },
      })
      if (stale()) {
        // 内核加载期间标签被关了：当场回收刚建好的实例（否则连 host 一起永久漏掉）
        editor.dispose()
        return
      }
      tab.editor = editor
      tab.dirty = false
      attachLsp(tab, editor, language, opts.lspReuseFrom)
      // blame 数据与上一轮的编辑器绑定（/git/blame 是按当时的行号算的）：重建后清掉，由下面的偏好恢复重取
      tab.blameLines = undefined
      tab.blameGutter = false
      tab.blameInline = false
      // Git 修改标记同理（diff 引擎挂在旧编辑器的 model 上）：重建后重置，就绪后重新应用
      tab.gitGutterOn = false
      void applyGitGutter(tab)
      // 行内溯源状态刚清零：右键菜单里的溯源项文案带开关态，重装一次让它跟上
      tab.editor?.refreshMenu()
      void autoBlame(tab) // 行尾态按本地偏好自动恢复（查看态与编辑态都给）
      editor.onChange(() => {
        /*
         * 脏标记：早期实现每次击键都 `getValue() !== baseline` 做**全文比对**——大文件上是
         * 每键物化一次 MB 级字符串。现在击键路径上零字符串分配：先乐观置脏 + 刷新标签栏，
         * 再防抖复核一次（内容被改回原样时要能变回干净）。
         */
        if (!tab.dirty) {
          tab.dirty = true
          renderTabbar()
        }
        if (dirtyTimer !== null) window.clearTimeout(dirtyTimer)
        dirtyTimer = window.setTimeout(() => {
          dirtyTimer = null
          if (stale() || !tab.editor) return
          const dirty = tab.editor.getValue() !== tab.baseline
          if (dirty !== tab.dirty) {
            tab.dirty = dirty
            renderTabbar()
          }
        }, 300)
      })
      editor.onCursor((info) => {
        if (activeTab()?.id !== tab.id) return
        state.cursor = info
        // 状态栏走按帧合并：拖选时每个 mousemove 都会回调，而状态栏是全量重建的（见 renderStatus）
        scheduleStatus()
        // 光标行进状态记忆（内部节流）：刷新后回到同一行
        persistSession()
        // 位置历史：普通光标移动修正栈顶（不产生新记录）——后退/前进回到「离开时最后看的位置」
        navTrack()
      })
      if (opts.line) {
        setTimeout(() => {
          if (!stale()) editor.revealLine(opts.line! - 1, opts.column ?? 1)
        }, 60)
      } else if (tab.cursorLine || tab.scrollTop) {
        /*
         * 重建编辑器后把**光标与滚动位置**放回原处。
         *
         * 为什么在 loadTab 而不是只在 activate：编辑器实例是**常驻**的（切标签只是 `display:none`，
         * 位置本来就不会丢），真正会丢的是**重建**——「重新加载当前文件」、「以文本打开」、
         * 图表源码↔渲染预览互切、以及大文件在 Monaco/降级之间切换，都会走 loadTab 把编辑器
         * 拆了重装。这些路径都发生在同一个标签内，不经过 activate。
         *
         * 带 `opts.line` 时不还原：那是深链接/定位给出的明确目标，比“上次看到哪儿”优先。
         */
        restoreView(tab)
      }
    } else {
      clear(host0)
      tab.viewDispose = renderViewer(host0, viewerCtx(tab))
    }
    // 标签栏右侧动作依赖 tab.stat（能否编辑/是否图表…）与 kind，加载完才齐
    renderTabbar()
    renderStatus()
  } catch (err) {
    // 已被新加载取代 / 标签已关：不要往（已卸下的）host 里写错误页
    if (stale()) return
    clear(host0)
    tab.loadError = (err as Error).message
    const box = h("div", { class: "fw-placeholder" }, [
      h("div", { class: "fw-placeholder-msg", text: `无法打开：${tab.loadError}` }),
      h("div", { class: "fw-placeholder-hint", text: "可尝试以文本方式强制打开，或直接下载。" }),
      h("div", { class: "fw-placeholder-actions" }, [
        (() => {
          const b = h("button", { class: "fw-btn primary" }, [icon("eye"), h("span", { text: "以文本打开" })])
          b.onclick = () => void loadTab(tab, { forceText: true })
          return b
        })(),
        (() => {
          const b = h("button", { class: "fw-btn" }, [icon("download"), h("span", { text: "下载" })])
          b.onclick = () => window.open(downloadUrl({ api, root: tab.root, path: tab.path }), "_blank")
          return b
        })(),
      ]),
    ])
    host0.appendChild(box)
  }
}

function placeholderFor(msg: string, hint?: string): HTMLElement {
  return h("div", { class: "fw-placeholder" }, [h("div", { class: "fw-placeholder-msg", text: msg }), hint ? h("div", { class: "fw-placeholder-hint", text: hint }) : null])
}

function viewerCtx(tab: Tab): ViewerCtx {
  return {
    api,
    root: tab.root,
    path: tab.path,
    name: tab.title,
    kind: tab.stat?.kind ?? "binary",
    stat: tab.stat ?? ({ size: 0, mime: "", kind: "binary" } as unknown as FileStat),
    notify: (msg, kind) => toast(msg, kind ?? "info"),
    preview: tab.previewKind,
  }
}

/** 差异视图头部导航的控制句柄：变更文件清单就绪/变更时重设，差异视图卸载时 dispose（退计数订阅）。 */
interface DiffHeaderNav {
  /** 变更文件清单（跨文件导航）就绪/变化时重设文件组；清单缺失时按钮置灰 */
  setReview(review: ReviewCtx | undefined): void
  /** 退订差异块计数订阅并移除容器（差异视图卸载/换文件重建时调） */
  dispose(): void
}

/**
 * 差异视图头部的两组导航按钮：← → 换变更文件（跨文件）、↑ ↓ 换差异块（文件内）。
 *
 * 放在差异视图自己的头部而不是标签栏：按钮描述的是「这块差异视图」的内容（当前差异块/总块数、
 * 第几个变更文件），跟着被导航的内容走；标签栏只承担标签生命周期（关/切/预览），不随差异内容变化。
 * 订阅与退订也在同一处（挂载时订、dispose 时退），不再依赖标签栏的重建节奏。
 */
function mountDiffHeaderNav(host: HTMLElement, tab: Tab): DiffHeaderNav {
  /* host 是差异视图头部的导航容器（git.ts 里的 `.fw-diff-nav`），这里直接往里装两组按钮，
     不再自建同名容器——否则两层嵌套，外层的 flex/gap 作用在错误的层级上。 */
  const container = h("span", { class: "fw-diff-nav-groups" })
  host.appendChild(container)

  /* ---------- 文件组（← → 换变更文件，清单异步就绪后由 setReview 填充） ---------- */
  const prevFile = btn("chevronLeft", "上一个变更文件（Shift+F8）", () => void navigateReview(tab, -1))
  const nextFile = btn("chevronRight", "下一个变更文件（F8）", () => void navigateReview(tab, 1))
  const fileCount = h("span", { class: "fw-nav-count", text: "…", title: "正在获取变更文件清单…" })
  container.appendChild(h("span", { class: "fw-nav-group" }, [prevFile, fileCount, nextFile]))

  /* ---------- 差异块组（↑ ↓；降级渲染没有 nav，整组不给——置灰会让人以为坏了） ---------- */
  let unsub: (() => void) | null = null
  if (tab.diffNav) {
    const nav = tab.diffNav
    const prevDiff = btn("chevronUp", "上一处差异（Shift+F7）", () => nav.prev())
    const nextDiff = btn("chevronDown", "下一处差异（F7）", () => nav.next())
    const diffCount = h("span", { class: "fw-nav-count", text: "—", title: "当前差异块 / 总差异块" })
    unsub = nav.onChange((s) => {
      diffCount.textContent = s.total ? `${s.index || 1} / ${s.total}` : "无差异"
      prevDiff.disabled = !s.total
      nextDiff.disabled = !s.total
    })
    container.appendChild(h("span", { class: "fw-nav-group" }, [prevDiff, diffCount, nextDiff]))
  }

  return {
    setReview(review) {
      const hasList = !!review && review.files.length > 1
      prevFile.disabled = !hasList
      nextFile.disabled = !hasList
      fileCount.textContent = review ? `${review.index < 0 ? "–" : review.index + 1} / ${review.files.length}` : "–"
      fileCount.title = review ? `变更文件：第 ${review.index < 0 ? "?" : review.index + 1} 个，共 ${review.files.length} 个` : "变更文件清单不可用"
    },
    dispose() {
      unsub?.()
      unsub = null
      container.remove()
    },
  }
}

async function openDiff(spec: DiffSpec): Promise<void> {
  const id = tabId("diff", spec.root, spec.path, `:${JSON.stringify(spec.source)}`)
  const exist = findTab(id)
  if (exist) {
    activate(id)
    return
  }
  const host = h("div", { class: "fw-tab-view" })
  const tab: Tab = {
    id,
    kind: "diff",
    root: spec.root,
    path: spec.path,
    title: `◧ ${spec.title}`,
    preview: false,
    host,
    mode: "view",
    dirty: false,
    baseline: "",
    content: "",
    encoding: "utf-8",
    eol: "lf",
    etag: "",
    diffSpec: spec,
  }
  state.tabs.push(tab)
  views.appendChild(host)
  viewHosts.set(id, host)
  activate(id)
  const info = state.roots.find((r) => r.id === spec.root)
  const view = await mountDiffView(host, api, spec, {
    repoRootPath: info?.repoRoot ?? "",
    language: languageOf(spec.path),
    onChanged: onDiffChanged,
    onOpenStage: (p) => void openStageTab(p),
  })
  tab.diffDispose = view.dispose
  tab.diffNav = view.nav
  tab.diffNavCtl = mountDiffHeaderNav(view.navHost, tab)
  tab.diffNavCtl.setReview(tab.review)
  renderTabbar()
  // 变更文件清单异步取（不挡首屏）：拿到后头部导航的跨文件组才可用
  void prepareReview(tab, spec)
}

/* ------------------------------ 变更文件遍历（跨文件导航） ------------------------------ */

/**
 * 取「与当前差异同一端点对」的变更文件清单——**由 DiffSpec 推导端点**，
 * 所以工作区改动、某次提交、任意两端比较都能用同一套按钮遍历。
 */
async function fetchReviewFiles(spec: DiffSpec): Promise<string[]> {
  const ep = diffEndpointsFor(spec)
  // 用 compare 端点的参数（与取内容用的端点约定不同，见 DiffEndpoints 注释）
  const res = await api.gitCompare(spec.root, ep.compare)
  return res.files.map((f) => f.path)
}

/**
 * 填充标签的 review 上下文（跨文件导航）。
 *
 * 失败**不再完全静默**：原先的 `catch {}` 会让「服务端报错」与「这个端点对下只有一个文件」
 * 在界面上长得一模一样（都是“没有导航按钮”）——实测踩过：根提交时 compare 返回 422
 * （`<hash>^` 在根提交上不存在），导航按钮凭空消失，而用户无法知道发生了什么。
 * 现在提示一句（warn，短时），差异视图本身照常打开。
 */
async function prepareReview(tab: Tab, spec: DiffSpec): Promise<void> {
  try {
    const files = await fetchReviewFiles(spec)
    if (!files.length) return
    // 已销毁的标签不再回填（异步期间用户可能已关掉）
    if (!findTab(tab.id)) return
    tab.review = { files, index: files.indexOf(spec.path) }
    // 清单就绪：直达差异视图头部的导航组（不经标签栏重绘——按钮不在那里了）
    if (state.activeId === tab.id || findTab(tab.id)) tab.diffNavCtl?.setReview(tab.review)
  } catch (err) {
    toast(`无法获取变更文件清单（${(err as Error).message}）：跨文件导航不可用`, "warn", 5000)
  }
}

/**
 * 在当前差异标签内换到上/下一个变更文件。
 *
 * 语义：**同一个标签内换文件**（不是每个文件开一个标签）——遍历 20 个文件不该堆 20 个标签。
 * 因此换文件时要给标签改 key（标签 id 里含路径），否则「按 id 查已有标签」会指错文件。
 * 若目标文件的差异标签已开着，则直接切过去（复用，不重复开）。
 */
async function navigateReview(tab: Tab, delta: 1 | -1): Promise<void> {
  const review = tab.review
  const spec = tab.diffSpec
  if (!review || !spec) return
  const n = review.files.length
  if (n < 2) return
  const next = review.index < 0 ? (delta === 1 ? 0 : n - 1) : (review.index + delta + n) % n
  const path = review.files[next]
  if (path === tab.path) return
  const exist = findTab(diffTabId(spec, path))
  if (exist && exist !== tab) {
    // 已开着该文件的差异：切过去，并把游标对齐，之后的「下一个」从那继续
    exist.review = { files: review.files, index: next }
    activate(exist.id)
    renderTabbar()
    return
  }
  await loadDiffInto(tab, { ...spec, path, title: diffTitleFor(spec, path) }, next)
}

/** 差异标签 id：路径进 id（同一文件的差异只开一个标签）。 */
function diffTabId(spec: DiffSpec, path: string): string {
  return tabId("diff", spec.root, path, `:${JSON.stringify(spec.source)}`)
}

/**
 * 换文件后的标签标题：沿用原标题的「来源后缀」（如「（工作区）」/「 @ 3a43d50b」），
 * 只替换文件名部分——原来的标题格式由各调用方精心取过，不该被导航改掉。
 */
function diffTitleFor(spec: DiffSpec, path: string): string {
  const base = (p: string) => p.split("/").pop() ?? p
  const oldBase = base(spec.path)
  const suffix = spec.title.startsWith(oldBase) ? spec.title.slice(oldBase.length) : ""
  return suffix ? `${base(path)}${suffix}` : `${base(path)}（${diffEndpointsFor(spec).note}）`
}

/** 把另一个文件的差异装载进**同一个标签**（重建视图 + 给标签改 key）。 */
async function loadDiffInto(tab: Tab, spec: DiffSpec, reviewIndex: number): Promise<void> {
  const host = viewHosts.get(tab.id)
  if (!host) return
  tab.diffDispose?.()
  tab.diffDispose = undefined
  tab.diffNav = null
  tab.diffNavCtl?.dispose()
  tab.diffNavCtl = undefined
  rekeyTab(tab, diffTabId(spec, spec.path))
  tab.path = spec.path
  tab.title = `◧ ${spec.title}`
  tab.diffSpec = spec
  if (tab.review) tab.review = { files: tab.review.files, index: reviewIndex }
  clear(host)
  renderTabbar()
  const info = state.roots.find((r) => r.id === spec.root)
  const view = await mountDiffView(host, api, spec, {
    repoRootPath: info?.repoRoot ?? "",
    language: languageOf(spec.path),
    onChanged: onDiffChanged,
    onOpenStage: (p) => void openStageTab(p),
  })
  tab.diffDispose = view.dispose
  tab.diffNav = view.nav
  tab.diffNavCtl = mountDiffHeaderNav(view.navHost, tab)
  tab.diffNavCtl.setReview(tab.review)
  renderTabbar()
  urlSync.replace()
}

/** 给标签改 key（id 变了，viewHosts / activeId 都要跟着）。 */
function rekeyTab(tab: Tab, newId: string): void {
  if (tab.id === newId) return
  const old = tab.id
  viewHosts.delete(old)
  viewHosts.set(newId, tab.host)
  tab.id = newId
  if (state.activeId === old) state.activeId = newId
}

/**
 * 文件路径 → Monaco 语言 id（差异视图、比较视图、合并视图共用）。
 *
 * 映射表在 `@gebai/sdk` 的 `file-language.ts`——与服务端 `core/fs/mime.ts` **同一份真相**。
 * 早先这里是手写的第二份表：`x.mts`、`Cargo.toml`、`Dockerfile` 这类路径在编辑器里有语言、
 * 在差异视图里却是 `plaintext`（丢高亮与符号），两处不一致就是这么来的。
 */
function languageOf(path: string): string {
  return languageOfPath(path)
}

/**
 * 回到标签时把**光标与滚动位置**放回原处（切标签不再是「回到文件头」）。
 *
 * 顺序要紧：**先落光标、再回滚动**。反过来的话，Monaco 会因为「光标仍在原处」而在落光标那一步
 * 把视口扭回去（它保证光标可见），刚回的滚动位置白记。
 *
 * 与 sessionStorage 里那份刷新记忆分工不同：那份只记**行**（刷新后回到原处），
 * 这份记**行 + 列 + 滚动**，只管同一页面内切标签；两者共用 `cursorLine` 这个字段没关系——
 * 它们想表达的就是同一件事：“这个标签看到哪儿了”。
 */
function restoreView(tab: Tab): void {
  const ed = tab.editor
  if (!ed) return
  if (tab.cursorLine) ed.setCursorPos({ line: tab.cursorLine, column: tab.cursorColumn ?? 1 })
  if (tab.scrollTop) ed.setScrollTop(tab.scrollTop)
  /*
   * 光标状态**一律**从**这个**编辑器取：`state.cursor` 是“上一次光标事件”留下的值，
   * 而 `onCursor` 只认活动标签、`setCursorPos` 又只在位置真的变化时才触发事件——
   * 不校正的话，切到一个光标恰好在原位的文件，状态栏会继续显示**上一个文件**的行号。
   */
  state.cursor = ed.getCursor()
}

function activate(id: string): void {
  const tab = findTab(id)
  if (!tab) return
  // 保存上一个标签的位置（光标行/列 + 滚动）：切回来时放回原处，光标行还供状态记忆回到刷新前的位置
  const prev = activeTab()
  if (prev?.editor && prev.id !== id) {
    prev.scrollTop = prev.editor.getScrollTop()
    const pos = prev.editor.getCursorPos()
    prev.cursorLine = pos.line
    prev.cursorColumn = pos.column
  }
  // 离开一个**预览标签**（斜体标题、唯一一个预览槽）就把它落成常驻：预览槽只在“当前正在看”时才有意义，
  // 切走还留斜体的话，下一次单击别的文件会把这个早就不在眼前的标签顶掉（VSCode 同此行为）。
  // 注意：openFile 的“就位替换”路径也会走到这里（上一个预览即将被新标签取代）——那里是**故意**的：
  // 被顶掉的标签本来就不该留，而这里去清它的 preview 只是顺手，对它没有别的影响。
  if (prev && prev.id !== id && prev.preview) prev.preview = false
  state.activeId = id
  if (tab.id.startsWith("file:")) lastFileTabId = tabKey(tab.root, tab.path)
  for (const [tid, host] of viewHosts) host.classList.toggle("active", tid === id)
  restoreView(tab)
  // 行尾 blame 的本地偏好：编辑器就绪或 git 状态后到（启动期）时补上
  void autoBlame(tab)
  // Git 修改标记：切到哪个标签，diff 引擎就跟哪个 model（同一时间只一个）
  void applyGitGutter(tab)
  scheduleEditorLayout()
  renderTabbar()
  renderStatus()
  // 只有**标签所属的根就是当前根**时才在树里定位：跨根打开（变更面板里点开根之外的改动，
  // 见 resolveRepoPath）时那条路径在当前根的树里根本不存在——照旧 reveal 会让资源管理器
  // 去列一个不存在的目录（404）并把树的展开态带偏。
  if (tab.kind === "file" && tab.root === explorer.getRoot()) void explorer.reveal(tab.path, { select: true })
  // 当前文件变了 → 地址栏就地替换（不新增历史：连开多个文件不该要按多次后退）
  urlSync.replace()
  persistSession()
}

function closeTab(id: string): void {
  const idx = state.tabs.findIndex((t) => t.id === id)
  if (idx < 0) return
  const tab = state.tabs[idx]
  if (tab.dirty) {
    void (async () => {
      const ok = await confirmDialog({ title: "未保存的修改", message: `「${tab.title}」有未保存的修改，确定关闭？`, okText: "放弃修改并关闭", danger: true })
      if (ok) forceClose(id)
    })()
    return
  }
  forceClose(id)
}

function forceClose(id: string): void {
  const idx = state.tabs.findIndex((t) => t.id === id)
  if (idx < 0) return
  const tab = state.tabs[idx]
  tab.editor?.dispose()
  // Git 修改标记的 HEAD 基线缓存只留当前文件一项：关标签时顺手清（重开时按需重取）
  if (tab.kind === "file") {
    const rel = repoRelOfTab(tab)
    if (rel) gitBaseCache.delete(`${tab.root}|${rel}`)
  }
  tab.viewDispose?.()
  tab.diffDispose?.()
  tab.diffNavCtl?.dispose()
  tab.diffNavCtl = undefined
  viewHosts.get(id)?.remove()
  viewHosts.delete(id)
  state.tabs.splice(idx, 1)
  if (state.activeId === id) {
    const next = state.tabs[Math.min(idx, state.tabs.length - 1)]
    state.activeId = next?.id ?? null
    if (next) activate(next.id)
    else {
      renderTabbar()
      renderStatus()
    }
  } else renderTabbar()
  refreshLastFileTab()
  persistSession()
}

/* ------------------------------ 渲染：标签栏 / 工具条 / 状态栏 ------------------------------ */

function renderTabbar(): void {
  // 同理：动作轮盘的容器挂在 body 上（不随 clear(tabstrip) 消失），必须显式销毁
  tabWheel?.destroy()
  tabWheel = null
  clear(tabstrip)
  for (const t of state.tabs) {
    const el = h("div", { class: `fw-tab${t.id === state.activeId ? " active" : ""}${t.preview ? " preview" : ""}` }, [
      t.icon ? icon(t.icon, 12) : t.kind === "diff" ? icon("diff", 12) : icon(t.dirty ? "edit" : "file", 12),
      h("span", { class: "fw-tab-title", text: t.title, title: t.kind === "diff" ? t.title : `${t.root} :: ${t.path}` }),
      t.dirty ? h("span", { class: "fw-tab-dot", title: "未保存" }) : null,
      (() => {
        const b = h("button", { class: "fw-tab-close", title: "关闭（Alt+W）" }, [icon("close", 11)])
        b.onclick = (e) => {
          e.stopPropagation()
          closeTab(t.id)
        }
        return b
      })(),
    ])
    el.onclick = () => activate(t.id)
    el.ondblclick = () => {
      // 双击标签 = 固定 ⇄ 取消预览（与右键菜单「固定/取消预览」同一动作、同一份状态）。
      // 单击已经激活过它，这里只反转预览态并重绘（激活态不受影响，光标/滚动位置也不会丢）。
      t.preview = !t.preview
      renderTabbar()
    }
    el.onmousedown = (e) => {
      if (e.button === 1) {
        e.preventDefault()
        closeTab(t.id)
      }
    }
    el.oncontextmenu = (e) => {
      e.preventDefault()
      showMenu(e.clientX, e.clientY, [
        { label: "关闭", icon: "close", shortcut: "Alt+W", onClick: () => closeTab(t.id) },
        { label: "关闭其它标签", icon: "close", onClick: () => state.tabs.filter((x) => x.id !== t.id).forEach((x) => forceClose(x.id)) },
        { label: "关闭右侧标签", icon: "close", onClick: () => {
          const i = state.tabs.findIndex((x) => x.id === t.id)
          state.tabs.slice(i + 1).forEach((x) => forceClose(x.id))
        } },
        { label: "关闭全部", icon: "close", onClick: () => [...state.tabs].forEach((x) => forceClose(x.id)) },
        { separator: true },
        { label: "固定/取消预览", icon: "check", onClick: () => {
          t.preview = !t.preview
          renderTabbar()
        } },
        { label: "复制路径", icon: "copy", onClick: () => void navigator.clipboard.writeText(t.path) },
        { label: "在资源管理器中定位", icon: "folder", onClick: () => void explorer.reveal(t.path) },
      ])
    }
    tabstrip.appendChild(el)
  }
  clear(tabActionsHost)
  renderTabActions(tabActionsHost)
  scrollActiveTabIntoView()
}

/**
 * 把活动标签滚进标签条的可视区（每次重渲染后调用）。
 *
 * 新开的标签总长在最右、Ctrl+Tab 也可能切到视野外的那个：不把它带进来，
 * 界面看起来像“没切成”或“新标签没打开”。只在真的越界时动 scrollLeft，其余情况不干扰用户已滚到的位置。
 */
function scrollActiveTabIntoView(): void {
  const el = tabstrip.querySelector<HTMLElement>(".fw-tab.active")
  if (!el) return
  const strip = tabstrip.getBoundingClientRect()
  const tab = el.getBoundingClientRect()
  if (tab.left < strip.left) tabstrip.scrollLeft -= strip.left - tab.left
  else if (tab.right > strip.right) tabstrip.scrollLeft += tab.right - strip.right
}

/**
 * 标签栏右侧动作区：**只留当前标签最高频的两个动作**，其余收进轮盘（与标题栏轮盘同一套交互）。
 *
 * 为何收：早期这里一排铺了 8 个图标按钮（编辑/保存/blame/预览/下载/历史/重载/复制路径），
 * 它们分属两个完全不同的频率档——「切换编辑态」「保存」是编码动线上每一步都要碰的，
 * 剩下那些是「偶尔用一次」。平铺的后果是高频动作淹没在按钮墙里，且它们本来就占着编辑区右上角。
 * （另外按钮全在标签栏而不是单独一行工具条：面包屑那行已被标签标题与资源管理器表达，
 *   省下一整行纵向空间给代码，且“当前标签能做什么”就在标签旁边。）
 *
 * 轮盘只有**一弧**：文件本身的动作与显示开关（保存 / 重载 / 下载 / 复制路径 / 自动换行）。
 * 历史相关三项（行尾溯源 / 侧边溯源 / 文件历史）原先占着内弧，现已移入**编辑器右键菜单**
 * 自成一组（见 gitMenuGroup）——人看着代码时右键就在手边，比扇形更贴手，轮盘也不必为剩项撑第二圈。
 */
function renderTabActions(box: HTMLElement): void {
  // 后退/前进（编辑位置历史，VSCode Alt+←/→）：全局能力，不随标签类型变——
  // 位置栈属于工作台会话而非某个标签，放在动作区最左（与后续动作隔一个组的宽度）。
  // 无历史时整组隐藏（不是置灰：启动后一次没跳过时，两颗灰按钮只是噪声）
  if (nav.peek().stack.length) {
    const backBtn = btn("back", "后退（Alt+←）", () => void navGo(-1))
    const fwdBtn = btn("forward", "前进（Alt+→）", () => void navGo(1))
    backBtn.disabled = !nav.canBack()
    fwdBtn.disabled = !nav.canForward()
    box.appendChild(h("span", { class: "fw-nav-group" }, [backBtn, fwdBtn]))
  }

  const t = activeTab()
  if (!t) return

  if (t.kind === "diff") return // 差异标签的动作在差异视图自己的头部（见 mountDiffHeaderNav），标签栏不再放

  // 合并视图自带工具条（且没有 stat）——不重复给按钮
  if (t.kind !== "file" || !t.stat) return

  // 常驻动作：切编辑态（每步编码都要用）；存盘在轮盘里（Ctrl+S 与脏标记 ● 已足够高频提示）
  const editable = !!t.stat.editable && !t.truncated && state.rootsResp?.writable !== false
  const modeBtn = btn(t.mode === "edit" ? "eye" : "edit", t.mode === "edit" ? "切换为查看（Ctrl+E）" : editable ? "编辑（Ctrl+E）" : "该文件类型不支持编辑", () => toggleMode(t), t.mode === "edit" ? "active" : "")
  modeBtn.disabled = !editable
  box.appendChild(modeBtn)

  /* 可渲染文件（图表源码 / markdown）：源码 ⇄ 渲染预览**常驻**——它决定看到的是图/文档还是文本，
     比其它动作都要紧（其余动作仍在轮盘里）。 */
  if (previewKindOf(extOf(t.path))) {
    const rendered = !!viewHosts.get(t.id)?.querySelector(".fw-preview")
    box.appendChild(btn("diff", rendered ? "切换到源码" : "切换到渲染预览", () => toggleRendered(t), rendered ? "active" : ""))
  }

  /* ---------- 其余动作：收进轮盘（单弧） ---------- */
  const items: WheelItem[] = []

  // 文件本身的动作（保存 / 重载 / 下载 / 复制路径）+ 显示开关（自动换行）
  items.push({ el: wheelBtn("save", t.dirty ? "保存（Ctrl+S）· 有未保存的修改" : "保存（Ctrl+S）", () => void saveTab(t), t.dirty ? "primary" : "", !t.dirty || !state.rootsResp?.writable) })
  items.push({ el: wheelBtn("refresh", "重新加载当前文件", () => void loadTab(t)) })
  items.push({ el: wheelBtn("download", "下载", () => window.open(downloadUrl({ api, root: t.root, path: t.path }), "_blank")) })
  items.push({ el: wheelBtn("copy", "复制路径", () => void navigator.clipboard.writeText(t.path).then(() => toast("已复制路径", "success"))) })
  // 自动换行是**全局显示开关**（不是这一个文件的属性）：按钮态即当前开关，Alt+Z 同效
  const wrapOn = isWordWrap()
  items.push({ el: wheelBtn("wrap", wordWrapTitle(wrapOn), () => toggleWrapAndReport(), wrapOn ? "active" : "") })
  // 同理：小地图也是全局显示开关（默认开启，轮盘里能关掉）
  const minimapOn = isMinimap()
  items.push({ el: wheelBtn("minimap", minimapTitle(minimapOn), () => toggleMinimapAndReport(), minimapOn ? "active" : "") })

  /* 轮盘入口：**不用 btn() 的 13px 图标**——它与标题栏轮盘入口是同一个动作，在那边是 16px + `--text`
     （见 css/overlays.css 的 `#wheel-btn.icon-btn`），13px/--text-muted 的标签栏密度下九宫格点阵会糊成
     一团（实测 8 倍放大对比：字形的锅大于字号的锅，两者一起修）。形状只在标签栏里大一号（.fw-wheel-btn，
     见 css/files.css），与兄弟按钮保持同一个视觉重心。

     **不挂 title**：展开后每个扇形按钮都自带 tooltip，入口再弹一条八项清单是重复且碍事（入口一悬停
     就展开，提示正好盖在刚弹出的扇形上）；可访问名称改用 `aria-label`——它是无障碍名称，
     **不产生视觉提示**。要定位这个入口请用 `.fw-tabbar-actions .fw-wheel-btn`（脚本/测试同理，不依赖文案）。 */
  const trigger = h("button", { class: "fw-icon-btn fw-wheel-btn", "aria-label": "更多操作" })
  trigger.appendChild(icon("apps", 16))
  trigger.onclick = () => {}
  box.appendChild(trigger)
  tabWheel = createWheel({ trigger, items, containerClass: "wheel fw-wheel" })
}

function btn(iconName: string, title: string, onClick: () => void, cls = ""): HTMLButtonElement {
  const b = h("button", { class: `fw-icon-btn ${cls}`, title })
  b.appendChild(icon(iconName, 13))
  b.onclick = onClick
  return b
}

/** 源码 ⇄ 渲染预览（图表/图片类查看器）。两态互切都得先把上一态**彻底卸掉**：
 * 只 append 不清 host 会源码与预览同屏叠着，且旧 dispose 被覆盖后再无人调用（编辑器/查看器各漏一份）。 */
function toggleRendered(t: Tab): void {
  const host = viewHosts.get(t.id)
  if (!host) return
  // 预览→源码：走 loadTab（它开头会 dispose 旧查看器、清空 host 后重建编辑器，并清 previewKind）
  if (host.querySelector(".fw-preview")) {
    void loadTab(t)
    return
  }
  // 源码→预览：未保存的修改会被丢掉（预览态没有编辑器承载它），先拦住
  if (t.dirty) {
    toast("有未保存的修改，请先保存再切换视图", "warn")
    return
  }
  const pk = previewKindOf(extOf(t.path))
  // markdown 的 kind 是 text（图表源码是 diagram）：渲染形态靠 tab.previewKind 传达给查看器
  t.previewKind = pk === "markdown" ? "markdown" : undefined
  t.editor?.dispose()
  t.editor = undefined
  t.viewDispose?.()
  clear(host)
  t.viewDispose = renderViewer(host, viewerCtx(t))
  renderTabbar()
  renderStatus()
}

/** 轮盘里的动作按钮：图标略大（扇形按钮边长统一 32px，13px 图标在里面显小）——
 * 16px 与标题栏轮盘的扇形按钮（index.html 里写 16）一致，两个入口的扇形长得一样。 */
function wheelBtn(iconName: string, title: string, onClick: () => void, cls = "", disabled = false): HTMLButtonElement {
  const b = h("button", { class: `fw-icon-btn ${cls}`, title })
  b.appendChild(icon(iconName, 16))
  b.disabled = disabled
  b.onclick = onClick
  return b
}

/**
 * 编辑器右键菜单里的自定义组：目前是 **Git 溯源**一组（行尾溯源 / 侧边溯源 / 文件历史）。
 *
 * 为何挂这里：这三项原先占着标签栏动作轮盘的**内弧**——同属「这行、这文件是什么时候谁改的」，
 * 低频，但真要用时人正看着代码，右键就在手边（菜单里自成一段，与「复制路径 / 发送会话」
 * 用分隔线分开，见 editor.ts 的组名规则），轮盘因此回到单弧。
 *
 * **两项行内溯源在前、文件历史在后**：右键是“对着某一行”发出的手势，默认意图多是问“这行谁写的”；
 * 文件级历史退一位（它也是三项里唯一会弹窗的一个）。
 *
 * **文案只留三个词，不带括号说明**：组内部的三个项都短、也没有同名前缀混淆，“行尾/侧边/文件”
 * 三个限定词已足够区分（何时需要长说明，看 `行尾溯源（编辑态不可用）`——那是**状态**不是说明，去不得）。
 *
 * **每次装配/重装菜单时现取**（见 EditorMenuHooks.groups）：文案带状态（`✓ 行尾溯源`），
 * 所以开关或模式一变就调 `editor.refreshMenu()`（调用点：applyBlame / toggleMode / loadTab）。
 */
function gitMenuGroup(tab: Tab, kind: EditorHandle["kind"]): EditorMenuGroup[] {
  if (!state.gitStatus?.isRepo) return [] // 非仓库：整组不给（与原来轮盘三项同一个条件）
  const items: EditorMenuItem[] = []
  // 降级编辑器画不出行内溯源（setBlame 是空实现）：那两项不给，免点了没反应
  if (kind === "monaco") {
    items.push({
      id: "blameInline",
      label: `${tab.blameInline ? "✓ " : ""}行尾溯源`,
      run: () => void toggleBlame(tab, "inline"),
    })
    // 编辑态下侧边列不可用（行号随编辑漂移，整列作者会指到别的行，比不显示更糟）：
    // 菜单里**保留位置并写明原因**，而不是让它整个消失（消失会让人以为功能没了）。
    // 这里的括号是**状态**不是说明（去掉就等于一个“看着可点、点了只弹提示”的项）
    const gutterOK = tab.mode !== "edit"
    items.push({
      id: "blameGutter",
      label: gutterOK ? `${tab.blameGutter ? "✓ " : ""}侧边溯源` : "侧边溯源（编辑态不可用）",
      run: () => {
        if (gutterOK) void toggleBlame(tab, "gutter")
        else toast("编辑态下不可用侧边溯源（行号会随编辑漂移）", "warn")
      },
    })
  }
  items.push({ id: "fileHistory", label: "文件历史", run: () => void showFileHistoryByPath(tab.path, tab.root) })
  return [{ id: "git", items }]
}

/** 按路径看文件历史（工具栏与变更面板右键共用）。 */
/** 文件历史（`git log --follow`）：入参为**仓库相对**路径（git 侧统一用仓库坐标，与当前根无关）。 */
async function showFileHistoryByPath(path: string, root = explorer.getRoot()): Promise<void> {
  try {
    const res = await api.gitFileHistory(root, path, 50)
    const overlay = h("div", { class: "fw-overlay" })
    const list = h("div", { class: "fw-history-list" })
    if (!res.commits.length) list.appendChild(h("div", { class: "fw-empty", text: "该文件暂无提交历史（可能是未跟踪文件）" }))
    for (const c of res.commits) {
      const row = h("div", { class: "fw-log-row" }, [
        h("div", { class: "fw-log-main" }, [
          h("div", { class: "fw-log-subject", text: c.subject }),
          h("div", { class: "fw-log-meta" }, [h("span", { class: "fw-log-hash", text: c.short }), h("span", { text: c.author }), h("span", { text: formatTime(c.commitTime) })]),
        ]),
      ])
      row.onclick = () => {
        overlay.remove()
        void openDiff({ title: `${path.split("/").pop() ?? path} @ ${c.short}`, root, path, source: { type: "commit", hash: c.hash } })
      }
      list.appendChild(row)
    }
    const dialog = h("div", { class: "fw-dialog wide" }, [
      h("div", { class: "fw-dialog-title" }, [icon("history"), h("span", { text: `文件历史：${path.split("/").pop() ?? path}` })]),
      h("div", { class: "fw-dialog-body" }, [list]),
      h("div", { class: "fw-dialog-actions" }, [
        (() => {
          const b = h("button", { class: "fw-btn", text: "关闭" })
          b.onclick = () => overlay.remove()
          return b
        })(),
      ]),
    ])
    overlay.appendChild(dialog)
    overlay.onclick = (e) => {
      if (e.target === overlay) overlay.remove()
    }
    document.body.appendChild(overlay)
  } catch (err) {
    toast(`读取文件历史失败：${(err as Error).message}`, "error")
  }
}

/** 状态栏刷新按帧合并（拖选/移动光标每个事件都会请求刷新一次）。 */
let statusRaf = 0
function scheduleStatus(): void {
  if (statusRaf) return
  statusRaf = requestAnimationFrame(() => {
    statusRaf = 0
    renderStatus()
  })
}

/** 上一次渲染的状态快照（用于跳过「什么都没变」的整条重建）。 */
let statusSig = ""

function renderStatus(): void {
  const tab = activeTab()
  const g = state.gitStatus
  const root = state.roots.find((r) => r.id === explorer.getRoot())
  /*
   * 状态栏是**整条重建**的（清空 + ~10 个节点 + 图标 + 时间串本地化），而它的调用点极密：
   * 光标移动/拖选（拖选时每个 mousemove 一次）、保存、切标签、git 刷新……绝大多数时候什么都没变。
   * 先做一次 O(1) 快照比对，未变直接返回（`formatSize`、图标解析、逐项建节点都不便宜）。
   * 快照必须覆盖所有影响渲染的输入——新增状态栏条目时同步补字段。
   */
  const sig = [
    tab?.id ?? "-", tab?.kind ?? "", tab?.mode ?? "", tab?.encoding ?? "", tab?.eol ?? "",
    tab?.stat?.language ?? "", tab?.stat?.size ?? "", tab?.editor ? 1 : 0,
    state.cursor.line, state.cursor.column, state.cursor.selected,
    explorer.getRoot(), root?.name ?? "", root?.path ?? "", root?.isRepo ? 1 : 0,
    g?.isRepo ? 1 : 0, g?.branch ?? "", g?.detached ? 1 : 0, g?.ahead ?? 0, g?.behind ?? 0,
    g?.counts ? `${g.counts.staged}/${g.counts.unstaged}/${g.counts.untracked}/${g.counts.conflicted}` : "",
    state.rootsResp?.writable ? 1 : 0,
    monacoLoadFailed() ? 1 : 0,
    attachedServerOf(tab?.editor?.model?.() ?? null),
  ].join("|")
  if (sig === statusSig) return
  statusSig = sig
  clear(statusbar)
  /*
   * 状态栏条目按**优先级**标注（data-pri，1 最要）：窄面板（分屏常在 640px 上下）里状态栏条目
   * 排不下时，CSS 按优先级从低到高逐级隐藏——而不是让整条状态栏把文档顶出横向滚动。
   * 为什么要显式标：条目是按当前文件动态增删的（编码/行列/大小…只在文件标签下出现），
   * 用 :nth-child 猜“哪几个能藏”会随文件类型变化而错位。
   */
  const item = (cls: string, opts: { title?: string; pri: 1 | 2 | 3 } & Record<string, unknown> = { pri: 2 }): HTMLElement => {
    const { pri, ...rest } = opts
    const el = h("span", { class: `fw-status-item ${cls}`.trim(), ...rest } as Parameters<typeof h>[1])
    el.dataset.pri = String(pri)
    return el
  }
  const btn = (el: HTMLElement, pri: 1 | 2 | 3): HTMLElement => {
    el.dataset.pri = String(pri)
    return el
  }
  /* 当前根（工作区目录）格。**图标恒用文件夹**，不再按“是不是 git 仓库”切成 git 图标——
     那一格的语义是“我现在在哪个目录下”，而 git 属性紧挨着就有专门的表示（右侧分支格只在
     仓库下出现，已把「是不是仓库 / 在哪条分支」说清楚了）；在仓库根上挂 git 图标反而像“点它看 git”。
     图标与资源管理器头部那个根按钮（`.fw-root-btn` 的 `folderOpen`）保持一致。
     名字走 `.fw-status-name`（限宽 + 省略号）：会话根的名字就是**会话标题**，可以很长，
     而它是最左的 pri1 格、窄面板下也不会被隐掉，不设上限会把整条状态栏顶开；完整路径在悬浮里。 */
  const rel = h("button", { class: "fw-status-item", title: root?.path ?? "" }, [icon("folderOpen", 12), h("span", { class: "fw-status-name", text: root?.name ?? "-" })])
  rel.onclick = () => showMenu(...menuAt(rel, state.roots.map((r) => ({ label: r.name, icon: "folder", onClick: () => void explorer.setRoot(r.id) }))))
  statusbar.appendChild(btn(rel, 1))

  // 状态读失败：给一个能重试的明确提示，而不是让状态栏这块直接什么都不显示（读失败与「不是仓库」不是一回事）
  if (state.gitStatusError) {
    const warn = h("button", { class: "fw-status-item warn", title: `Git 状态读取失败：${state.gitStatusError}（点击重试）` }, [icon("warning", 12), h("span", { text: "Git 状态不可用" })])
    warn.onclick = () => void refreshGit()
    statusbar.appendChild(btn(warn, 1))
  }

  if (state.gitStatus?.isRepo) {
    const s = state.gitStatus
    /* 变更计数：**非零才显示**、符号各自表意、冲突最先（规则与理由见 git-counts.ts）。
       旧实现把三个计数一律印出来，于是“一个只有冲突的仓库”会显示成 `0± 0± 0?`（看着像干净）。 */
    const branch = h("button", { class: "fw-status-item git", title: branchTitle(s) }, [
      icon("branch", 12),
      h("span", { text: s.branch ?? (s.detached ? "(detached)" : "-") }),
      s.ahead ? h("span", { class: "fw-ahead", text: `↑${s.ahead}` }) : null,
      s.behind ? h("span", { class: "fw-behind", text: `↓${s.behind}` }) : null,
      ...changeParts(s.counts).map((p) =>
        h("span", { class: p.kind === "conflicted" ? "fw-status-conflict" : "fw-status-changes", text: p.text }),
      ),
    ])
    branch.onclick = () => {
      if (!state.gitViewVisible) toggleGitPanel(true)
      gitPanel?.show("branches")
    }
    statusbar.appendChild(btn(branch, 1))

    // 多步操作（merge/rebase/cherry-pick/revert）进行中：继续/中止在左栏「变更」面板顶部，
    // 但状态栏也要能一眼看出“仓库正处在中间状态”——否则很容易在半途提交或切分支。
    if (s.operation) {
      const opItem = h("button", { class: "fw-status-item warn", title: "多步操作进行中：继续 / 跳过 / 中止在左栏「变更」面板顶部" }, [
        icon("sync", 12),
        h("span", { text: `${s.operation} 进行中` }),
      ])
      opItem.onclick = () => toggleLeftView("changes")
      statusbar.appendChild(btn(opItem, 1))
    }
  }

  statusbar.appendChild(h("span", { class: "fw-grow" }))

  if (tab?.kind === "file" && tab.stat) {
    const t = tab
    const encBtn = h("button", { class: "fw-status-item", title: "字符编码（保存时按此编码回写）" }, [h("span", { text: t.encoding === "binary" ? "二进制" : t.encoding.toUpperCase() })])
    encBtn.onclick = () => {
      dropdown(encBtn, ["utf-8", "utf-8-bom", "utf-16le", "utf-16be", "gbk", "gb18030", "big5", "latin1"].map((e) => ({
        label: e,
        icon: e === t.encoding ? "check" : undefined,
        onClick: () => {
          t.encoding = e
          renderStatus()
          toast(`保存编码已设为 ${e}`, "info")
        },
      })))
    }
    statusbar.appendChild(btn(encBtn, 3))

    const eolBtn = h("button", { class: "fw-status-item", title: "行尾序列（保存时按此写回；keep 表示保持编辑器内容原样）" }, [h("span", { text: t.eol === "crlf" ? "CRLF" : t.eol === "mixed" ? "混合" : "LF" })])
    eolBtn.onclick = () =>
      dropdown(eolBtn, [
        { label: "LF（Unix）", icon: t.eol === "lf" ? "check" : undefined, onClick: () => setEol(t, "lf") },
        { label: "CRLF（Windows）", icon: t.eol === "crlf" ? "check" : undefined, onClick: () => setEol(t, "crlf") },
        { label: "保持内容原样", icon: t.eol === "keep" ? "check" : undefined, onClick: () => setEol(t, "keep") },
      ])
    statusbar.appendChild(btn(eolBtn, 3))

    const statInfo = t.stat
    // 语言 / 只读态、行列坐标：窄屏下比编码、大小、时间更常看，优先级高一级
    statusbar.appendChild(item("", { pri: 2, text: statInfo?.language || "plaintext" }))
    statusbar.appendChild(item(t.mode === "edit" ? "" : "warn", { pri: 1, text: t.mode === "edit" ? "编辑" : "只读" }))
    if (t.editor) {
      statusbar.appendChild(item("", { pri: 2, text: `行 ${state.cursor.line}，列 ${state.cursor.column}${state.cursor.selected ? `（选中 ${state.cursor.selected}）` : ""}` }))
    }
    statusbar.appendChild(item("", { pri: 3, text: formatSize(statInfo?.size ?? 0) }))
    /* 这里曾有一个「文件最后修改时间（磁盘 mtime）」格，已移除：它只在“想知道这份文件什么时候被改过”时
       有用，而那个问题在页面里有更直接的载体——Git 工具窗的提交时间（带提交者与 diff）、
       以及标签行的脏标记（改没改、存没存）。常驻一个到秒的时间戳占了状态栏最宽的一格，
       却回答不了一个新问题（看一眼就知道“文件是旧的”，但旧于哪一版要看 Git）。 */
  }
  /*
   * 引擎/语义能力格：**只在“与默认不同”时出现**——默认（Monaco + 该文件没挂语言服务器）什么都不显示：
   * 编辑器长什么样一眼就知道是 Monaco，常驻一个「Monaco」标签只是噪声（原先那条还兼作 LSP 指示，
   * 没挂上时就只重复“是 Monaco”这件事）。两种情况才值得占位：
   *   ① 挂上了语言服务器（如 `rust-analyzer`）→ 显示服务器名，一眼看出该文件有语义能力在支撑；
   *   ② Monaco **真加载失败/超时**、落到轻量降级模式 → 明确告知（此时高亮/符号等能力受限，不说会以为坏了）。
   * 注意判断用 `monacoLoadFailed()` 而**不是** `!monacoReady()`：后者在“还在预热”时也为假，
   * 会在页面刚开、Monaco 那 1MB 还在下的时候闪一个「轻量模式」（实测：只开目录根、未打开任何文件时
   * 状态栏就挂着它）。没加载完就不说话，只有确实失败才提示。
   */
  const lspServer = attachedServerOf(tab?.editor?.model?.() ?? null)
  if (monacoLoadFailed()) {
    statusbar.appendChild(
      item("warn", { pri: 3, title: "编辑器内核：轻量降级模式（Monaco 加载失败或 vendor 缺失）——语法高亮/符号跳转等能力受限", text: "轻量模式" }),
    )
  } else if (lspServer) {
    statusbar.appendChild(
      item("", { pri: 3, title: `语言服务器：${lspServerDetailOf(tab?.editor?.model?.() ?? null) || lspServer}`, text: lspServer }),
    )
  }
  if (state.rootsResp && !state.rootsResp.writable) statusbar.appendChild(item("warn", { pri: 1, text: "只读模式" }))
}

function setEol(tab: Tab, eol: "lf" | "crlf" | "keep"): void {
  tab.eol = eol
  renderStatus()
  if (eol !== "keep" && tab.editor) {
    const cur = tab.editor.getValue()
    const unified = cur.replace(/\r\n/g, "\n").replace(/\r/g, "\n")
    const next = eol === "crlf" ? unified.replace(/\n/g, "\r\n") : unified
    if (next !== cur) {
      tab.baseline = tab.baseline.replace(/\r\n/g, "\n").replace(/\r/g, "\n")
      if (eol === "crlf") tab.baseline = tab.baseline.replace(/\n/g, "\r\n")
      tab.editor.setValue(next)
      tab.dirty = tab.editor.getValue() !== tab.baseline
      renderTabbar()
    }
  }
}

function menuAt(anchor: HTMLElement, items: Parameters<typeof showMenu>[2]): [number, number, Parameters<typeof showMenu>[2]] {
  const r = anchor.getBoundingClientRect()
  return [r.left, r.top - Math.min(320, items.length * 30) - 6, items]
}

/** 上次绘制的活动栏指纹（无变化不重建，见 renderRail）。 */
let railKey = ""

function renderRail(): void {
  /*
   * 静默判据：活动栏渲染只依赖这几个值（视图、显隐、改动数、面板开关、嵌入态与停靠侧）。
   * 为何需要它：自动刷新（Git 状态到达 → 变更面板报计数、切根、主题变更……）都会调到里，
   * 而重建会把 hover/焦点与图标全抖一遍——自动刷新的每一次心跳都不该碰到活动栏。
   */
  const key = fingerprint([state.leftView, leftVisible(), dirtyCount(), state.dockVisible, state.dockView, state.gitViewVisible, EMBEDDED, splitSide, hostMode, canSplit])
  if (key === railKey) return
  railKey = key
  clear(railEl)
  /** 左栏视图按钮（变更 / 资源管理器 / 搜索）——点当前视图 = 收起左栏（IDEA 活动栏习惯）。 */
  const mkView = (id: "changes" | "explorer" | "search", iconName: string, title: string) => {
    const b = h("button", { class: `fw-rail-btn${leftVisible() && state.leftView === id ? " active" : ""}`, title })
    b.appendChild(icon(iconName, 18))
    if (id === "changes") {
      const n = dirtyCount()
      if (n) b.appendChild(h("span", { class: "fw-rail-badge", text: String(n) }))
    }
    b.onclick = () => toggleLeftView(id)
    return b
  }
  railEl.append(
    mkView("explorer", "folder", "资源管理器（Ctrl+Shift+E）"),
    mkView("search", "search", "搜索（Ctrl+Shift+F）"),
    // 变更排在最后：前两个是"找文件"，变更面板是"看待提交的改动"，从导航到动作的顺序
    mkView("changes", "diff", "变更：工作区改动与提交（Ctrl+Shift+G）"),
    h("div", { class: "fw-rail-spacer" }),
    // 底部组：工具窗开关 + 全局入口（原菜单栏的功能补位）
    (() => {
      const active = state.dockVisible && state.dockView === "terminal"
      const b = h("button", { class: `fw-rail-btn${active ? " active" : ""}`, title: "终端（Ctrl+`）" })
      b.appendChild(icon("terminal", 18))
      b.onclick = () => toggleTerminalPanel(!active)
      return b
    })(),
    (() => {
      const b = h("button", { class: `fw-rail-btn${state.gitViewVisible ? " active" : ""}`, title: "源代码管理工具窗（Alt+G）" })
      b.appendChild(icon("git", 18))
      b.onclick = () => toggleGitPanel(!state.gitViewVisible)
      return b
    })(),
    // 「打开文件夹（切换根）」已移除：切根在资源管理器顶部的根选择按钮里（那里还带根清单与面包屑语义）
    (() => {
      // 菜单栏移除后，菜单里的杂项收进这一个入口（新建/上传/比较/快捷键/服务端开关/全屏/回主界面）
      const b = h("button", { class: "fw-rail-btn", title: "更多（Ctrl+K）：新建 / 比较 / 重新加载 / 快捷键 / 服务端开关 / 浏览器全屏 / 关闭文件工作台" })
      b.appendChild(icon("settings", 18))
      b.onclick = () => {
        const r = b.getBoundingClientRect()
        showMenu(r.left, r.top - 6, [
          { label: "新建文件…", icon: "plus", disabled: !state.rootsResp?.writable, onClick: () => void newQuick("file") },
          { label: "新建文件夹…", icon: "plus", disabled: !state.rootsResp?.writable, onClick: () => void newQuick("dir") },
          { label: "上传文件…", icon: "upload", disabled: !state.rootsResp?.writable, onClick: () => pickUpload() },
          { separator: true },
          { label: "比较任意两端…", icon: "diff", shortcut: "Ctrl+Shift+D", onClick: () => void openCompare() },
          { label: "快速打开文件…（模糊搜）", icon: "search", shortcut: "Ctrl+P", onClick: () => void quickOpen() },
          { label: "转到符号…（当前文件内）", icon: "symbols", shortcut: "Ctrl+Shift+O", onClick: openSymbols },
          { label: "刷新根清单与 Git 状态", icon: "refresh", onClick: () => void loadRoots().then(() => explorer.refresh("")) },
          { separator: true },
          { label: "快捷键一览", icon: "info", onClick: () => showShortcuts() },
          { label: "服务端开关（GEBAI_FS_* / GEBAI_GIT_*）", icon: "settings", onClick: () => showEnvHelp() },
          // 页面级动作归页面自己：分屏面板已经没有标题栏了，重新加载/换停靠侧/回到会话工作台都在这里
          { label: "重新加载工作台", icon: "refresh", onClick: () => location.reload() },
          ...(EMBEDDED
            ? [
                // 左右互换：面板在左则在右，反之亦然（换的是宿主布局，工作台自己不搬家）。
                // 全屏态下两栏都归面板，这里没有“停靠侧”可言，那一项收起来。
                ...(soloHost()
                  ? []
                  : [{ label: splitSide === "left" ? "分屏停靠改到右侧" : "分屏停靠改到左侧", icon: "swap", onClick: () => requestSplitSwap() }]),
              ]
            : []),
          { label: "浏览器全屏", icon: "expand", onClick: () => void (document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen()) },
          { separator: true },
          // 嵌入态下"返回主界面"= 关掉同窗容器（并列 / 全屏都适用）；独立标签页才是整页跳回
          // （左箭头 = 把窗口还给会话工作台，与活动栏最下方那颗同一个图标、同一个动作）
          // shortcut 标 `Ctrl+\`：面板内这个键就是这一项（与活动栏那颗按钮同一个动作、同一标注）
          EMBEDDED
            ? { label: "关闭文件工作台", icon: "arrowLeft", shortcut: "Ctrl+\\", onClick: () => requestCloseSplit() }
            : { label: "返回歌白主界面", icon: "back", onClick: () => { location.href = wbUrl("/") } },
        ])
      }
      return b
    })(),
  )
  /*
   * 嵌入态在活动栏**最下方**给一对同窗切换按钮（独立标签页没有同窗形态，不渲染）。
   *
   * 主按钮 = **关闭文件工作台**：嵌入态下面板自己没有顶栏，鼠标用户要关它本来只剩「更多」菜单里的
   * 那一项（两步），于是把这一颗常驻在最下方；图标是**左箭头**——向左 = 把窗口还给会话工作台，
   * 与宿主入口那颗「全屏文件工作台」的右箭头互为反向（箭头指方向，不在停靠侧上做文章）。
   * 辅助按钮 = **进入分屏**：悬浮时从它右侧弹出（触屏常显，同宿主入口副按钮那套交互语言），
   * 只有**全屏态**才给——并列态它就是「再进一次并列」，没有去处；窗口窄到分屏放不下时也不给
   * （宿主经 `gebai:files-mode` 带上 `canSplit`）。
   *
   * （单独 append：railEl.append 不收 null，上面那串是定长列表。）
   */
  if (EMBEDDED) {
    const wrap = h("div", { class: "fw-rail-exit" })
    const close = h("button", { class: "fw-rail-btn", title: "关闭文件工作台（Ctrl+\\）" })
    close.appendChild(icon("arrowLeft", 18))
    close.onclick = () => requestCloseSplit()
    wrap.appendChild(close)
    if (soloHost() && canSplit) {
      const split = h("button", { class: "fw-rail-btn fw-rail-split", title: "进入分屏（与会话并列）" })
      split.appendChild(icon("split", 18))
      split.onclick = () => requestEnterSplit()
      wrap.appendChild(split)
    }
    railEl.appendChild(wrap)
  }
}

/** 宿主侧是否处于**全屏**态（文件工作台独占整个窗口）：文案跟着它变，见 renderRail 与「更多」菜单。 */
function soloHost(): boolean {
  return EMBEDDED && hostMode === "solo"
}

/* ------------------------------ 查看/编辑与保存 ------------------------------ */

/**
 * 切换 blame 的某一态（数据来自 `/git/blame`，两态共用一份；编辑器负责画）。
 * 编辑态不可用——行号会随编辑漂移，注释会指到别的行上，反而误导。
 * 行尾态的开关**记在浏览器本地**（`blame-prefs.ts`）：打开文件就自动恢复；侧边列较重，不跨文件记忆。
 */
async function toggleBlame(tab: Tab, which: "gutter" | "inline"): Promise<void> {
  const ed = tab.editor
  if (!ed || tab.kind !== "file") return
  const cur = which === "gutter" ? tab.blameGutter : tab.blameInline
  if (cur) {
    if (which === "gutter") tab.blameGutter = false
    else {
      tab.blameInline = false
      saveInlineBlame(false)
    }
    applyBlame(tab)
    renderTabbar()
    return
  }
  if (tab.blameLines === undefined) {
    try {
      const res = await api.gitBlame(tab.root, tab.path)
      tab.blameLines = res.lines
      if (!res.lines.length) toast("该文件没有可用的溯源信息（未跟踪 / 历史为空）", "info")
    } catch (err) {
      toast(`读取溯源信息失败：${(err as Error).message}`, "error")
      return
    }
  }
  if (which === "gutter") tab.blameGutter = true
  else {
    tab.blameInline = true
    saveInlineBlame(true)
  }
  applyBlame(tab)
  renderTabbar()
}

/** 把两态开关与数据一起交给编辑器（唯一渲染入口，切模式/重载也走它）。 */
function applyBlame(tab: Tab): void {
  tab.editor?.setBlame(tab.blameLines ?? [], { gutter: !!tab.blameGutter, inline: !!tab.blameInline })
  // 右键菜单里的溯源项文案带开关态（`✓ 行尾溯源`）：开关一变就得重装一次菜单
  tab.editor?.refreshMenu()
}

/** 按本地偏好自动开行尾态（打开文件/切回查看态时调；侧边列不自动开）。 */
async function autoBlame(tab: Tab): Promise<void> {
  if (!readInlineBlame() || tab.blameInline || !tab.editor) return
  if (tab.blameLines === undefined) {
    try {
      tab.blameLines = (await api.gitBlame(tab.root, tab.path)).lines
    } catch {
      return // 自动恢复失败不打扰用户（手动点击时才有提示）
    }
  }
  tab.blameInline = true
  applyBlame(tab)
  renderTabbar()
}

function toggleMode(tab: Tab): void {
  tab.mode = tab.mode === "edit" ? "view" : "edit"
  // 进编辑态撤掉侧边列（行号会随编辑漂移，整列作者指到别的行比不显示更糟）；行尾态保留
  // ——它是跟随光标的一行淡色批注，不进模型、不影响保存，编辑时同样有用
  if (tab.mode === "edit" && tab.blameGutter) {
    tab.blameGutter = false
    applyBlame(tab)
  }
  // 侧边溯源在编辑态不可用（菜单项文案随模式变）：重装一次菜单
  tab.editor?.refreshMenu()
  tab.editor?.setReadOnly(tab.mode !== "edit" || !!tab.truncated)
  if (tab.mode === "edit") {
    tab.editor?.focus()
    toast("已进入编辑模式（Ctrl+S 保存）", "info", 2200)
  } else {
    // 回到查看态：行尾态按本地偏好恢复（编辑期间可能一直没开过）
    void autoBlame(tab)
  }
  renderTabbar()
  renderStatus()
  persistSession()
}

async function saveTab(tab: Tab, opts: { force?: boolean } = {}): Promise<boolean> {
  if (tab.kind !== "file" || !tab.editor) return false
  if (!state.rootsResp?.writable) {
    toast("当前为只读模式（GEBAI_FS_WRITE=false），无法保存", "error")
    return false
  }
  const content = tab.editor.getValue()
  try {
    const res = await api.write(tab.root, tab.path, { content, encoding: tab.encoding, eol: tab.eol === "keep" ? undefined : tab.eol, expectedEtag: opts.force ? undefined : tab.etag })
    // 记一笔「这个文件是刚由本页写的」：watch 的回声不触发自动重载（否则保存完立刻被回读覆盖一遍）
    selfWrites.set(`${tab.root}|${tab.path}`, Date.now())
    tab.etag = res.etag
    tab.baseline = content
    tab.content = content
    tab.dirty = false
    tab.mode = "edit"
    tab.editor.setReadOnly(false)
    // 语言服务器：通知保存（重新诊断；未挂载时为空操作）
    const savedModel = tab.editor.model()
    if (savedModel) notifySaved(savedModel)
    renderTabbar()
    toast("已保存", "success", 1600)
    if (state.gitStatus?.isRepo) void refreshGit().then(() => { if (state.gitViewVisible) void gitPanel?.refresh() })
    return true
  } catch (err) {
    if (err instanceof ApiError && err.status === 409) {
      const detail = err.detail as { current?: { content: string; etag: string; encoding: string } } | null
      const choice = await choiceDialog("保存冲突", "磁盘上的文件已被外部修改（可能是 Agent 或其它工具写入）。请选择处理方式：", [
        { label: "用我的内容覆盖", danger: true, value: "overwrite" },
        { label: "重新加载磁盘内容（放弃我的修改）", value: "reload" },
        { label: "取消", value: "cancel" },
      ])
      if (choice === "overwrite") return await saveTab(tab, { force: true })
      if (choice === "reload" && detail?.current) {
        tab.editor.setValue(detail.current.content)
        tab.baseline = detail.current.content
        tab.etag = detail.current.etag
        tab.encoding = detail.current.encoding
        tab.dirty = false
        renderTabbar()
        toast("已重新加载磁盘内容", "info")
        return false
      }
      return false
    }
    toast(`保存失败：${(err as Error).message}`, "error", 6000)
    return false
  }
}

/** 三选一对话框（保存冲突等场景）。 */
function choiceDialog(title: string, message: string, options: Array<{ label: string; value: string; danger?: boolean }>): Promise<string> {
  return new Promise((resolve) => {
    const overlay = h("div", { class: "fw-overlay" })
    const done = (v: string) => {
      overlay.remove()
      resolve(v)
    }
    const actions = h("div", { class: "fw-dialog-actions" })
    for (const o of options) {
      const b = h("button", { class: `fw-btn${o.danger ? " danger" : o.value === "cancel" ? "" : " primary"}` })
      b.textContent = o.label
      b.onclick = () => done(o.value)
      actions.appendChild(b)
    }
    const dialog = h("div", { class: "fw-dialog" }, [h("div", { class: "fw-dialog-title" }, [icon("warning"), h("span", { text: title })]), h("div", { class: "fw-dialog-body" }, [h("div", { class: "fw-dialog-msg", text: message })]), actions])
    overlay.appendChild(dialog)
    overlay.onclick = (e) => {
      if (e.target === overlay) done("cancel")
    }
    document.body.appendChild(overlay)
  })
}

/* ------------------------------ 主题 ------------------------------ */

// 主题引擎与主界面**共用**（theme-core）：主题是用户级偏好，两个入口必须完全一致，
// 否则从主界面切到工作台会换一套配色（曾如此：工作台只读了 localStorage 的 id，
// 没应用人民币面额配色与默认主题黑白变体）。工作台不再有自己的主题设置入口。
document.addEventListener("gebai:theme-change", () => {
  refreshEditorTheme()
  renderRail()
})

/* ------------------------------ 被主界面分屏嵌入时 ------------------------------ */

/**
 * 分屏面板停在窗口哪一侧（宿主经 postMessage 告知，见 files-split.ts）。
 * 只影响两处表达：活动栏/菜单里「关闭」的**箭头朝向**，与「停靠改到左/右」那一项的文案。
 * 缺省 left——与宿主缺省停靠侧一致，消息到达前的首帧也不至于指反。
 */
let splitSide: "left" | "right" = "left"

/**
 * 宿主当前的**同窗形态**（`split` = 与会话并列，`solo` = 文件工作台独占整个窗口，见 files-split.ts）。
 * 只影响活动栏最下方那对按钮：全屏态才给「进入分屏」（并列态它没有去处）。
 * 缺省 split——消息到达前的首帧先按并列写，与快照“嵌入就是分屏”一致。
 */
let hostMode: "split" | "solo" = "split"

/** 宿主窗口是否容得下分屏（`gebai:files-mode` 随形态一起带过来）：容不下就不给「进入分屏」。 */
let canSplit = true

/**
 * 是否被嵌在宿主页面里（主界面「分屏打开」把本页放进 iframe）。
 * 五个跨界动作靠 postMessage 桥接：主题同步、停靠侧同步、形态同步、进分屏、关闭同窗。
 */
const EMBEDDED = window.self !== window.top

if (EMBEDDED) {
  window.addEventListener("message", (e: MessageEvent) => {
    // 只认同源且来自宿主窗口的消息
    if (e.origin !== location.origin || e.source !== window.parent) return
    const data = e.data as { type?: string; theme?: string | null; cnyScheme?: string | null; acrylicLt?: string | null; side?: string | null; mode?: string | null; canSplit?: boolean } | null
    // 宿主侧的停靠侧：换侧时活动栏与「更多」菜单里的箭头/文案要跟着翻（工作台自己不知道面板贴哪边）
    if (data?.type === "gebai:files-split-side") {
      const next = data.side === "right" ? "right" : "left"
      if (next !== splitSide) {
        splitSide = next
        renderRail()
      }
      return
    }
    // 宿主侧的形态与可否分屏：决定活动栏最下方那颗按钮的文案，以及要不要给「进入分屏」
    if (data?.type === "gebai:files-mode") {
      const next = data.mode === "solo" ? "solo" : "split"
      const fits = data.canSplit !== false
      if (next !== hostMode || fits !== canSplit) {
        hostMode = next
        canSplit = fits
        renderRail()
      }
      return
    }
    if (data?.type !== "gebai:theme") return
    // 宿主侧改主题时同步过来（工作台是独立文档，不会自己跟着变）
    if (data.theme) void setTheme(data.theme as ThemeId)
    setCnyScheme((data.cnyScheme as CnySchemeId | null) ?? null)
    setAcrylicLt((data.acrylicLt as AcrylicLtId | null) ?? null)
  })
}

/**
 * 通知宿主关掉文件工作台（嵌入态下"关掉我"的正确语义：关掉同窗容器，而不是把 iframe 导航走）。
 * 并列与全屏都是同一件事——回到会话工作台；想并列另有 requestEnterSplit。
 */
function requestCloseSplit(): void {
  window.parent.postMessage({ type: "gebai:files-close-split" }, location.origin)
}

/** 通知宿主从全屏切到并列（会话与文件并排）；切完宿主会回一条 `gebai:files-mode`。 */
function requestEnterSplit(): void {
  window.parent.postMessage({ type: "gebai:files-enter-split" }, location.origin)
}

/** 通知宿主把分屏停靠侧左右互换（面板在左 ↔ 在右）；换完宿主会回一条 gebai:files-split-side。 */
function requestSplitSwap(): void {
  window.parent.postMessage({ type: "gebai:files-split-swap" }, location.origin)
}

/**
 * 把编辑器选中的一段代码送进对话输入框（右键「发送会话」，**仅嵌入态可用**）。
 *
 * 两件事分得清楚：
 * - `text` 是已经组装好的 Markdown（引用行 + 代码块，见 `editor-ref.ts`）——**拼装在工作台侧做**，
 *   宿主只负责往输入框里放；宿主拿到的是一段现成文本，不需要知道编辑器与行号的任何细节。
 * - `ref`（绝对路径:行号）随消息带上，宿主据此提示与回显，不必再去解析 Markdown。
 *
 * 提示（toast）在这儿给而不是等宿主回执：这是用户右击动作的直接反馈，
 * 而消息本身是同步发出去的（同源 iframe，宿主必然在同一个事件循环里收到）。
 */
function requestSendToChat(snippet: EditorSnippet): void {
  window.parent.postMessage({ type: "gebai:files-send-to-chat", text: snippet.markdown, ref: snippet.ref }, location.origin)
  toast(`已发送会话：${snippet.ref}`, "success")
}

/* ------------------------------ 冲突合并标签（三窗格） ------------------------------ */

/**
 * 打开冲突合并标签（仓库相对路径入参——git 侧路径语义）。
 * 结果窗格可编辑、可逐块采纳，保存走 fs（etag 乐观锁），标记为解决走 git stage。
 */
async function openMergeTab(repoRel: string): Promise<void> {
  const root = explorer.getRoot()
  const id = tabId("merge", root, repoRel)
  const exist = findTab(id)
  if (exist) {
    activate(id)
    const view = mergeViews.get(id)
    if (view) void view.refresh()
    return
  }
  const host = h("div", { class: "fw-tab-view" })
  const tab: Tab = {
    id,
    kind: "file",
    root,
    path: repoRel,
    title: "⑃ " + (repoRel.split("/").pop() ?? repoRel),
    icon: "merge",
    preview: false,
    host,
    mode: "view",
    dirty: false,
    baseline: "",
    content: "",
    encoding: "utf-8",
    eol: "lf",
    etag: "",
  }
  state.tabs.push(tab)
  views.appendChild(host)
  viewHosts.set(id, host)
  activate(id)
  const view = await createMergeView(
    {
      api,
      root: () => explorer.getRoot(),
      // 冲突文件的落点由宿主换算（可能在当前根之外）——合并视图保存时按它定位
      resolvePath: (repoRel) => resolveRepoPath(repoRel),
      language: languageOf(repoRel),
      onSaved: () => void explorer.refresh(undefined, { keepSelection: true }),
      onResolved: () => {
        // 先拉新状态再重渲染面板——反过来会用旧 status 渲染（冲突行不消失）
        void refreshGit().then(() => {
          if (state.gitViewVisible && gitPanel) void gitPanel.refresh()
        })
      },
    },
    { repoRel },
  )
  host.appendChild(view.el)
  mergeViews.set(id, view)
  tab.viewDispose = () => {
    mergeViews.delete(id)
    view.dispose()
  }
  await view.refresh()
  renderTabbar()
  renderStatus()
}

/**
 * 打开三向暂存编辑器标签（HEAD ｜ 暂存结果 ｜ 工作区）。
 * 与「逐块操作」同一件事的另一个入口：适合要的既不是 HEAD 也不是工作区、而是介于两者之间的内容。
 */
async function openStageTab(repoRel: string): Promise<void> {
  const root = explorer.getRoot()
  const id = tabId("stage", root, repoRel)
  const exist = findTab(id)
  if (exist) {
    activate(id)
    const view = stageViews.get(id)
    if (view) void view.refresh()
    return
  }
  const host = h("div", { class: "fw-tab-view" })
  const tab: Tab = {
    id,
    kind: "file",
    root,
    path: repoRel,
    title: "⊞ " + (repoRel.split("/").pop() ?? repoRel),
    icon: "git",
    preview: false,
    host,
    mode: "view",
    dirty: false,
    baseline: "",
    content: "",
    encoding: "utf-8",
    eol: "lf",
    etag: "",
  }
  state.tabs.push(tab)
  views.appendChild(host)
  viewHosts.set(id, host)
  activate(id)
  const view = await createStageView({
    api,
    root: () => explorer.getRoot(),
    repoRel,
    language: languageOf(repoRel),
    onStaged: () => onDiffChanged(),
  })
  host.appendChild(view.el)
  stageViews.set(id, view)
  tab.viewDispose = () => {
    stageViews.delete(id)
    view.dispose()
  }
  await view.refresh()
  renderTabbar()
  renderStatus()
}

/* ------------------------------ 比较标签（任意两端对比） ------------------------------ */

interface CompareTabState {
  from: string
  to: string
  mergeBase: boolean
  path: string
}

const compareTabs = new Map<string, { view: CompareView; state: CompareTabState }>()

/** 合并标签视图句柄（关标签时 dispose；重复打开时 refresh）。 */
const mergeViews = new Map<string, MergeView>()

/** 三向暂存编辑器标签（HEAD ｜ 暂存结果 ｜ 工作区）。 */
const stageViews = new Map<string, StageView>()

async function openCompare(init: { from?: string; to?: string; path?: string; mergeBase?: boolean } = {}): Promise<void> {
  const root = explorer.getRoot()
  const from = init.from ?? "HEAD"
  const to = init.to ?? WORKTREE
  const id = tabId("compare", root, "", `:${from}:${to}:${init.mergeBase ? "mb" : ""}`)
  const exist = findTab(id)
  if (exist) {
    activate(id)
    const st = compareTabs.get(id)
    if (st && (st.state.from !== from || st.state.to !== to)) st.view.setState({ from, to, path: init.path ?? "" })
    return
  }
  const host = h("div", { class: "fw-tab-view" })
  const tab: Tab = {
    id,
    kind: "file",
    root,
    path: "",
    title: `⇄ ${from === "WORKTREE" ? "工作区" : from.slice(0, 18)} → ${to === "WORKTREE" ? "工作区" : to.slice(0, 18)}`,
    preview: false,
    host,
    mode: "view",
    dirty: false,
    baseline: "",
    content: "",
    encoding: "utf-8",
    eol: "lf",
    etag: "",
  }
  state.tabs.push(tab)
  views.appendChild(host)
  viewHosts.set(id, host)
  activate(id)
  const view = createCompareView(
    {
      api,
      root: () => explorer.getRoot(),
      // 比较视图里的路径均为**仓库相对**（git 语义）：差异取数保持原样，打开文件需换算成根相对
      openDiff: (spec) => void openDiff({ ...spec, root: explorer.getRoot() }),
      // 提交文件清单给的是**仓库相对**路径：打开时按当下的根/仓库换算出真正的落点
      openFile: (_r, path) => openRepoFile(path),
      onFsChanged: () => void refreshGit(),
    },
    { from, to, path: init.path, mergeBase: init.mergeBase ?? false, basePath: state.repoPrefix },
  )
  host.appendChild(view.el)
  compareTabs.set(id, { view, state: { from, to, mergeBase: init.mergeBase ?? false, path: init.path ?? "" } })
  // 标签关闭时释放：早期比较标签从不设 viewDispose，compareTabs 里的视图与状态一直留着
  tab.viewDispose = () => compareTabs.delete(id)
  await view.refresh()
  renderTabbar()
  renderStatus()
}

/* ------------------------------ 搜索视图（左栏） ------------------------------ */

let searchView: { el: HTMLElement } | null = null

function buildSearchView(): HTMLElement {
  const q = h("input", { class: "fw-input sm", placeholder: "搜索内容（Enter 搜索）" })
  const glob = h("input", { class: "fw-input sm", placeholder: "文件过滤 glob（如 src/**/*.ts）" })
  const modeSel = h("select", { class: "fw-input sm" }, [h("option", { value: "content", text: "按内容" }), h("option", { value: "name", text: "按文件名" })])
  const caseCb = h("label", { class: "fw-check" }, [h("input", { type: "checkbox" }), h("span", { text: "忽略大小写" })])
  const regexCb = h("label", { class: "fw-check" }, [h("input", { type: "checkbox" }), h("span", { text: "正则" })])
  ;(caseCb.querySelector("input") as HTMLInputElement).checked = true
  const results = h("div", { class: "fw-search-results" })
  const summary = h("div", { class: "fw-hint" })
  const el = h("div", { class: "fw-search-view" }, [
    h("div", { class: "fw-search-form" }, [q, glob, h("div", { class: "fw-search-row" }, [modeSel, caseCb, regexCb])]),
    summary,
    results,
  ])

  const run = async () => {
    const query = q.value.trim()
    if (!query) {
      toast("请输入搜索关键字", "warn")
      return
    }
    results.replaceChildren(h("div", { class: "fw-loading", text: "搜索中…" }))
    summary.textContent = ""
    try {
      const res = await api.search(explorer.getRoot(), query, {
        mode: (modeSel.value as "content" | "name") ?? "content",
        glob: glob.value.trim() || undefined,
        ignoreCase: (caseCb.querySelector("input") as HTMLInputElement).checked,
        regex: (regexCb.querySelector("input") as HTMLInputElement).checked,
        maxResults: 800,
      })
      clear(results)
      summary.textContent = `${res.hits.length} 处命中${res.truncated ? "（已截断，请缩小范围）" : ""} · 引擎 ${res.engine}`
      if (!res.hits.length) results.appendChild(h("div", { class: "fw-empty", text: "没有匹配结果" }))
      for (const hit of res.hits) {
        const name = hit.path.split("/").pop() ?? hit.path
        const row = h("div", { class: "fw-search-hit" }, [
          h("div", { class: "fw-search-hit-path" }, [icon("file", 12), h("span", { text: name }), h("span", { class: "fw-change-dir", text: hit.path.includes("/") ? `  ${hit.path.slice(0, hit.path.lastIndexOf("/"))}` : "" })]),
          hit.fileOnly ? null : h("div", { class: "fw-search-hit-line" }, [h("span", { class: "fw-search-ln", text: String(hit.line) }), h("span", { class: "fw-search-text", text: hit.lineText })]),
        ])
        row.onclick = () => void openFile(explorer.getRoot(), hit.path, { preview: false, line: hit.line || undefined })
        row.oncontextmenu = (e) => {
          e.preventDefault()
          showMenu(e.clientX, e.clientY, [
            { label: "打开文件", icon: "file", onClick: () => void openFile(explorer.getRoot(), hit.path, { preview: false, line: hit.line || undefined }) },
            { label: "在资源管理器中定位", icon: "folder", onClick: () => void explorer.reveal(hit.path) },
            { label: "复制路径", icon: "copy", onClick: () => void navigator.clipboard.writeText(hit.path) },
            { separator: true },
            { label: "只看此文件的内容", icon: "search", onClick: () => {
              glob.value = hit.path
              void run()
            } },
          ])
        }
        results.appendChild(row)
      }
    } catch (err) {
      clear(results)
      results.appendChild(h("div", { class: "fw-error", text: `搜索失败：${(err as Error).message}` }))
    }
  }
  q.onkeydown = (e) => {
    if (e.key === "Enter") void run()
  }
  ;(glob as HTMLInputElement).onkeydown = (e) => {
    if (e.key === "Enter") void run()
  }
  return el
}

/**
 * 左栏视图切换（三视图互斥：变更 | 资源管理器 | 搜索）。
 * 「变更」与目录树互斥是刻意的：同一时刻只看一件事——要么在找文件，要么在看改动。
 */
function showLeftView(view: "changes" | "explorer" | "search", opts: { keepHidden?: boolean } = {}): void {
  state.leftView = view
  if (view === "explorer") mountLeftView(explorer.el)
  else if (view === "search") {
    if (!searchView) searchView = { el: buildSearchView() }
    mountLeftView(searchView.el)
  } else {
    const panel = ensureChangesPanel()
    mountLeftView(panel.el)
    // 先渲染（用上一份状态），状态刷新到位后再渲染一次——否则首次打开是空面板
    panel.refresh()
    void refreshGit().then(() => panel.refresh())
  }
  if (!opts.keepHidden) setLeftVisible(true)
  renderRail()
  persistSession()
}

/**
 * 挂上并显示某个左栏视图（其余三个视图只置隐藏类）。
 *
 * 为什么不沿用早期的 `clear(leftPanel) + appendChild`：那会把离开的视图**从文档里摘下来**，
 * 每次来回切都要重排整栏，而且被摘下的子树会丢失滚动位置（看目录树到一半去「变更」再回来，
 * 树回到了顶部）——IDE 里这是最不能接受的“帮倒忙”。三视图体量都不大，常驻更划算。
 */
function mountLeftView(el: HTMLElement): void {
  if (el.parentElement !== leftPanel) leftPanel.appendChild(el)
  for (const child of leftPanel.children) {
    if (child !== el) child.classList.add("fw-view-hidden")
  }
  el.classList.remove("fw-view-hidden")
}

/**
 * 左栏是否展开（隐藏后编辑区占满——IDEA 的 Ctrl+B 行为）。
 * 窄屏（≤700px）下左栏是抽屉：展开与否由抽屉开关类决定。
 */
function leftVisible(): boolean {
  if (compact()) return drawerOpen()
  return leftPanelShown()
}

/**
 * 左栏自身的显隐（桌面语义）。
 *
 * 与 leftVisible() 分开：窄屏下左栏是抽屉（开合是临时动作），不该把桌面那份「左栏收起」
 * 记忆改写掉——否则在手机上翻一次文件，回到宽屏就发现左栏被永久收起了。
 */
function leftPanelShown(): boolean {
  return leftPanel.style.display !== "none"
}

/** 窄屏（≤700px）：左栏改抽屉、底部工具窗改整屏面板（见 files.css 手机端形态）。 */
function compact(): boolean {
  return window.matchMedia("(max-width: 700px)").matches
}

function drawerOpen(): boolean {
  return document.body.classList.contains("fw-drawer-open")
}

/**
 * 抽屉开关（仅窄屏生效）。
 * 开：先确保左栏未被收起（桌面那份「收起左栏」的记忆不该让抽屉空着），再滑入。
 * 关：只收抽屉，不改桌面的「左栏隐藏」记忆——回到宽屏还是原来的样子。
 */
function setDrawerOpen(open: boolean): void {
  if (!compact()) return
  applyDrawer(open)
}

/** 抽屉的 Esc 作用域（开着时才有；关掉即摘） */
let drawerScope: string | null = null

/** 抽屉态的写入（不含窄屏判定）：窄屏开关与断点变化时的强制收起共用一份。 */
function applyDrawer(open: boolean): void {
  if (open && leftPanel.style.display === "none") {
    leftPanel.style.display = ""
    leftResizer.style.display = ""
  }
  document.body.classList.toggle("fw-drawer-open", open)
  // Esc 关抽屉（键位走全局作用域栈：抽屉关着就不该占一个 Esc 优先级）
  if (open && !drawerScope) drawerScope = pushEscScope("wb.leftDrawer", "关闭左侧栏", () => setDrawerOpen(false), "wb.view")
  else if (!open && drawerScope) {
    popKeyScope(drawerScope)
    drawerScope = null
  }
  renderRail()
  scheduleEditorLayout()
}

function setLeftVisible(visible: boolean): void {
  leftPanel.style.display = visible ? "" : "none"
  leftResizer.style.display = visible ? "" : "none"
  if (!visible) applyDrawer(false)
  scheduleEditorLayout()
  renderRail()
  persistSession()
}

/** 点当前视图按钮 = 收起；点其它视图 = 切换。窄屏下收起/展开的是抽屉本身。 */
function toggleLeftView(view: "changes" | "explorer" | "search"): void {
  if (compact()) {
    if (state.leftView === view && drawerOpen()) setDrawerOpen(false)
    else {
      showLeftView(view)
      setDrawerOpen(true)
    }
    return
  }
  if (state.leftView === view && leftVisible()) setLeftVisible(false)
  else showLeftView(view)
}

/** 工作区改动数（rail 上「变更」按钮的徽标）。 */
function dirtyCount(): number {
  const c = state.gitStatus?.counts
  if (!c) return 0
  return c.staged + c.unstaged + c.untracked + c.conflicted
}

async function newQuick(kind: "file" | "dir"): Promise<void> {
  const sel = explorer.selected()
  const baseDir = sel?.type === "dir" ? sel.path : sel?.path.includes("/") ? sel.path.slice(0, sel.path.lastIndexOf("/")) : ""
  const name = await promptDialog({ title: kind === "file" ? "新建文件" : "新建文件夹", label: "名称", placeholder: kind === "file" ? "index.ts" : "components", hint: baseDir ? `创建于：${baseDir}` : "创建于当前根目录" })
  if (!name?.trim()) return
  const path = baseDir ? `${baseDir}/${name.trim()}` : name.trim()
  try {
    if (kind === "dir") await api.mkdir(explorer.getRoot(), path)
    else await api.write(explorer.getRoot(), path, { content: "", createDirs: true })
    toast("已创建", "success")
    await explorer.refresh(baseDir, { keepSelection: true })
    void refreshGit()
    if (kind === "file") void openFile(explorer.getRoot(), path, { preview: false })
  } catch (err) {
    toast(`创建失败：${(err as Error).message}`, "error")
  }
}

function pickUpload(): void {
  const input = document.createElement("input")
  input.type = "file"
  input.multiple = true
  input.onchange = () => {
    const files = Array.from(input.files ?? [])
    if (!files.length) return
    const sel = explorer.selected()
    const dir = sel?.type === "dir" ? sel.path : ""
    void api
      .upload(explorer.getRoot(), files.map((f) => ({ file: f, path: dir ? `${dir}/${f.name}` : f.name })), false)
      .then(async (res) => {
        toast(`已上传 ${res.saved.length} 个文件${res.skipped.length ? `（${res.skipped.length} 个同名已跳过）` : ""}`, res.skipped.length ? "warn" : "success")
        await explorer.refresh(dir, { keepSelection: true })
        void refreshGit()
      })
      .catch((err) => toast(`上传失败：${(err as Error).message}`, "error"))
  }
  input.click()
}

/**
 * 展开/收起底部 Git 工具窗。
 *
 * `deferData`：启动期专用——只同步可见性（先把骨架摆好），**不建面板也不取数据**。
 * 此时根清单还没到（explorer 根为空），请求会得出「当前根不是 Git 仓库」这种误导结论
 * （还会把「初始化仓库」按钮摆到误点位置），也白跑一轮请求；数据由根确定后的调用补上。
 */
function setDock(visible: boolean, view: DockView = state.dockView, opts: { deferData?: boolean } = {}): void {
  state.dockVisible = visible
  state.dockView = view
  state.gitViewVisible = visible && view === "git"
  gitDock.classList.toggle("collapsed", !visible)
  gitDockResizer.classList.toggle("collapsed", !visible)
  // 视图只挂一次，靠类切换显隐——用 hidden 属性不行：面板自身是 display:flex，会把它压过去
  gitPanel?.el.classList.toggle("fw-dock-hidden", !(visible && view === "git"))
  termPanel?.el.classList.toggle("fw-dock-hidden", !(visible && view === "terminal"))
  try {
    localStorage.setItem(DOCK_VIEW_KEY, view)
    localStorage.setItem(DOCK_VISIBLE_KEY, visible ? "1" : "0")
  } catch {
    /* 隐私模式忽略 */
  }
  if (visible && view === "git" && !opts.deferData) {
    ensureGitPanel()
    // 根为空时不请求（等 onRootChanged / 启动阶段二补刷）：否则会把「根未知」误当「不是仓库」
    if (explorer.getRoot()) void gitPanel?.refresh()
  }
  if (visible && view === "terminal" && !opts.deferData) ensureTerminalPanel().activate()
  else termPanel?.deactivate()
  // 展开/切换后 Monaco 可视高度变化，重排编辑器（否则出现空白/裁切）
  if (visible && !opts.deferData) scheduleEditorLayout()
  renderRail()
}

/** 展开/收起底部 Git 工具窗（可见时同时把工具窗切到 Git 视图）。 */
function toggleGitPanel(visible: boolean, opts: { deferData?: boolean } = {}): void {
  setDock(visible, visible ? "git" : state.dockView, opts)
}

/** 展开/收起底部终端工具窗。 */
function toggleTerminalPanel(visible: boolean): void {
  setDock(visible, visible ? "terminal" : state.dockView)
}

/** 快捷键一览：内容直接来自键位表（本文件的 bindings 与 keymap-wb.ts 的登记），不再手写第二份。 */
function showShortcuts(): void {
  const overlay = h("div", { class: "fw-overlay" })
  const dialog = h("div", { class: "fw-dialog" }, [
    h("div", { class: "fw-dialog-title" }, [icon("info"), h("span", { text: "快捷键" })]),
    h(
      "div",
      { class: "fw-dialog-body" },
      helpGroups(workbenchKeymap.bindings()).map((g) =>
        h("div", { class: "fw-kbd-group" }, [
          h("div", { class: "fw-kbd-group-title", text: g.title }),
          h(
            "div",
            { class: "fw-kbd-list" },
            g.rows.map((r) => {
              const row = h("div", { class: "fw-kbd-row" }, [
                h("kbd", { text: r.keys.join(" / ") }),
                h("span", { text: r.note ? `${r.label}（${r.note}）` : r.label }),
              ])
              // 接管了浏览器默认行为的键位标出来（如「接管 打印」），免得看着像普通键
              if (r.takesOver) row.appendChild(h("span", { class: "fw-kbd-takeover", text: `接管 ${r.takesOver}` }))
              return row
            }),
          ),
        ]),
      ),
    ),
    h("div", { class: "fw-dialog-actions" }, [
      (() => {
        const b = h("button", { class: "fw-btn primary", text: "知道了" })
        b.onclick = () => overlay.remove()
        return b
      })(),
    ]),
  ])
  overlay.appendChild(dialog)
  overlay.onclick = (e) => {
    if (e.target === overlay) overlay.remove()
  }
  document.body.appendChild(overlay)
}

function showEnvHelp(): void {
  const lines = [
    "GEBAI_FS_ENABLED    文件工作台总开关（默认 true）",
    "GEBAI_FS_WRITE      写开关（false = 纯只读检视）",
    "GEBAI_FS_ROOTS      额外白名单根（JSON 数组）",
    "GEBAI_FS_MAX_READ   单次读取上限（字节，默认 10MB）",
    "GEBAI_FS_MAX_WRITE  单次写入上限（默认 10MB）",
    "GEBAI_FS_MAX_UPLOAD 上传单文件上限（默认 100MB）",
    "GEBAI_FS_MAX_ZIP    打包下载上限（默认 500MB）",
    "GEBAI_FS_HIDDEN     默认显示隐藏文件（默认 true）",
    "GEBAI_FS_AUDIT      写操作审计（默认 true）",
    "GEBAI_GIT_WRITE     Git 写操作开关（默认 true）",
    "GEBAI_GIT_REMOTE    Git 远程操作开关（默认 true）",
  ]
  const overlay = h("div", { class: "fw-overlay" })
  const dialog = h("div", { class: "fw-dialog" }, [
    h("div", { class: "fw-dialog-title" }, [icon("settings"), h("span", { text: "服务端开关" })]),
    h("div", { class: "fw-dialog-body" }, [h("pre", { class: "fw-env-list", text: lines.join("\n") })]),
    h("div", { class: "fw-dialog-actions" }, [(() => {
      const b = h("button", { class: "fw-btn primary", text: "关闭" })
      b.onclick = () => overlay.remove()
      return b
    })()]),
  ])
  overlay.appendChild(dialog)
  overlay.onclick = (e) => {
    if (e.target === overlay) overlay.remove()
  }
  document.body.appendChild(overlay)
}

/* ------------------------------ 快捷键 ------------------------------ */



/** 切自动换行并同步界面（轮盘按钮态 + 轻提示）；轮盘项与 Alt+Z 共用。 */
function toggleWrapAndReport(): void {
  const on = toggleWordWrap()
  renderTabbar() // 轮盘按钮的图标/高亮与提示文案随之更新
  toast(on ? "已开启自动换行" : "已关闭自动换行", "info", 1600)
}

/** 切小地图并同步界面（轮盘按钮态 + 轻提示）；模块级开关会应用到全部代码编辑器。
 * 注：**大文件标签**不受此开关影响（超过降级阈值时本来就不给小地图，那是性能取舍、偏好不覆盖）；
 * 那种标签上本来就看着“文件较大”的横幅，不再另做一套状态回传。 */
function toggleMinimapAndReport(): void {
  const on = toggleMinimap()
  renderTabbar() // 轮盘按钮高亮与提示文案随之更新
  toast(on ? "已开启小地图" : "已关闭小地图", "info", 1600)
}

/* ------------------------------ 键位表 ------------------------------ */

/**
 * 键位声明即全部（分发与守卫在 `../keymap.ts`，元素级键位登记在 `./keymap-wb.ts`）。
 *
 * 键位取**常用键**：`Ctrl+S` 保存、`Ctrl+P` 快速打开、`Ctrl+F` 过滤、`F5` 刷新、`Ctrl+Shift+E/F/G`
 * 切面板、`Alt+W` 关标签。Chromium 里按键先到页面、浏览器加速器在后（源码依据见 `keymap.ts` 的
 * `browserConflict()`），所以「浏览器也有默认行为」的键由歌白直接接管（`preventDefault`）。
 *
 * **不用页面拿不到的保留键**（`Ctrl+N/T/W`、`Ctrl+Tab`…）：那类键在浏览器形态下按不动，而键位只有一套
 *——关标签因此取 `Alt+W`（对应主界面的 `Alt+N`）。
 *
 * 两处例外：
 * ① `Ctrl+F` 只在焦点不在 Monaco 编辑器时接管（编辑器内保留 Monaco 查找，见 `./keymap-wb.ts`）；
 * ② 接管类绑定走**捕获阶段**——Monaco 自己的快捷键服务会先吃掉一部分组合（`Ctrl+K` 系列、`F7`），
 *   冒泡阶段来不及；Alt 组合还会被浏览器菜单栏/输入法先碰，同样要捕获。
 */
const bindings: KeyBinding[] = [
  {
    id: "wb.save",
    keys: "Ctrl+S",
    label: "保存（文件 / 合并结果 / 暂存结果）",
    group: "wb.file",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    run: saveActive,
  },
  {
    id: "wb.toggleMode",
    keys: "Ctrl+E",
    label: "查看 ↔ 编辑模式",
    group: "wb.file",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    when: () => activeTab()?.kind === "file",
    run: toggleActiveMode,
  },
  {
    id: "wb.quickOpen",
    keys: "Ctrl+P",
    label: "快速打开文件（模糊搜文件名，VSCode 式）",
    group: "wb.file",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    // 面板已打开时让位：那一下 Ctrl+P 属于面板自己（VSCode 里 = 往上选一项），不该又去开一层
    when: () => !isQuickOpenOpen(),
    run: () => void quickOpen(),
  },
  {
    id: "wb.symbols",
    keys: "Ctrl+Shift+O",
    label: "转到符号（当前文件内，VSCode 式）",
    group: "wb.file",
    browser: "override",
    phase: "capture",
    /*
      * 只声明「焦点不在编辑器时」接管：编辑器内那一下留给 Monaco 自己（内置语言服务覆盖的语言由它出符号，
 * 有语言服务器的语言由它出服务器的文档符号；两者都由 `symbols.ts` 的 provider 供数据）——
 * 两种焦点环境都有的走，不占编辑器内的键。
 */
    focus: ["other"],
    when: () => !isSymbolPanelOpen() && activeTab()?.kind === "file",
    run: openSymbols,
  },
  {
    id: "wb.closeTab",
    keys: "Alt+W",
    label: "关闭当前标签",
    group: "wb.file",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    note: "浏览器把 Ctrl+W 拿去关标签页了（页面收不到该按键），所以用 Alt+W",
    run: () => {
      if (state.activeId) closeTab(state.activeId)
    },
  },
  { id: "wb.rename", keys: "F2", label: "重命名选中项", group: "wb.file", run: renameSelected },
  {
    id: "wb.copyEntry",
    keys: "Ctrl+C",
    label: "复制选中项（资源管理器）",
    group: "wb.file",
    focus: ["other"],
    when: () => explorer.isActive() && !!explorer.selected(),
    note: "只在资源管理器为活动区时接管；编辑器/输入框/终端里的 Ctrl+C 仍是文本复制",
    run: () => void explorer.copySelection(),
  },
  {
    id: "wb.pasteEntry",
    keys: "Ctrl+V",
    label: "粘贴到选中目录（资源管理器）",
    group: "wb.file",
    focus: ["other"],
    when: () => explorer.isActive() && explorer.canPaste(),
    note: "同上；粘贴不覆盖：同名时落成「xxx - 副本」",
    run: () => void explorer.paste(),
  },
  {
    id: "wb.wordWrap",
    keys: "Alt+Z",
    label: "切换自动换行",
    group: "wb.file",
    phase: "capture",
    focus: ["other", "editor"],
    note: "捕获阶段接管：Monaco 自己也绑了它，但只改编辑器实例选项、不动偏好",
    run: toggleWrapAndReport,
  },
  {
    id: "wb.exitSplit",
    keys: "Ctrl+\\",
    label: "关闭文件工作台（回到会话工作台）",
    group: "wb.view",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    when: () => EMBEDDED,
    note: "与宿主主界面的 Ctrl+\\ 同一个键：那个键管“开关”，面板里这个管“退出”（面板里没有“开”可言）",
    run: () => requestCloseSplit(),
  },
  {
    id: "wb.explorer",
    keys: "Ctrl+Shift+E",
    label: "显示资源管理器",
    group: "wb.view",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    run: () => showLeftView("explorer"),
  },
  {
    id: "wb.search",
    keys: "Ctrl+Shift+F",
    label: "显示搜索视图",
    group: "wb.view",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    run: () => showLeftView("search"),
  },
  {
    id: "wb.changes",
    keys: "Ctrl+Shift+G",
    label: "左侧变更面板",
    group: "wb.view",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    run: () => toggleLeftView("changes"),
  },
  {
    id: "wb.filterDir",
    keys: "Ctrl+F",
    label: "资源管理器：在当前目录过滤",
    group: "wb.view",
    browser: "override",
    phase: "capture",
    // 刻意只声明 other：
    // ① 焦点在 Monaco 编辑器时不接管——编辑器里的 Ctrl+F 是 Monaco 自己的查找（见 keymap-wb.ts）；
    // ② 焦点在输入框时不接管——过滤框/提交框里保留浏览器查找语义。
    focus: ["other"],
    run: () => {
      showLeftView("explorer")
      explorer.toggleSearch()
    },
  },
  {
    id: "wb.gitPanel",
    keys: "Alt+G",
    label: "底部 Git 工具窗（G = Git）",
    group: "wb.view",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    run: () => toggleGitPanel(!state.gitViewVisible),
  },
  {
    id: "wb.terminal",
    keys: "Ctrl+`",
    label: "底部终端工具窗",
    group: "wb.view",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    run: () => toggleTerminalPanel(!(state.dockVisible && state.dockView === "terminal")),
  },
  {
    id: "wb.toggleLeft",
    keys: "Ctrl+B",
    label: "显示 / 隐藏左侧栏",
    group: "wb.view",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    run: () => setLeftVisible(!leftVisible()),
  },
  {
    id: "wb.compare",
    keys: "Ctrl+Shift+D",
    label: "比较（任意两个提交 / 提交与工作区）",
    group: "wb.view",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    run: () => void openCompare(),
  },
  {
    id: "wb.moreMenu",
    keys: "Ctrl+K",
    label: "「更多」菜单（新建 / 比较 / 服务端开关）",
    group: "wb.view",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    note: "捕获阶段接管：Monaco 把 Ctrl+K 当多键组合的前缀（Ctrl+K Ctrl+C 等），冒泡阶段抢不过来",
    run: () => (railEl.querySelector('.fw-rail-btn[title^="更多"]') as HTMLElement | null)?.click(),
  },
  {
    id: "wb.refresh",
    keys: "F5",
    label: "刷新资源管理器与 Git 状态",
    group: "wb.view",
    browser: "override",
    phase: "capture",
    focus: FOCUS_ALL_FIELDS,
    note: "Ctrl+R 仍留给浏览器做整页刷新，两条路各归各的",
    run: refreshAll,
  },
  {
    id: "wb.diffNext",
    keys: "F7",
    label: "差异视图：下一处差异（同一文件内）",
    group: "wb.diff",
    browser: "override",
    phase: "capture",
    when: () => !!activeTab()?.diffNav,
    note: "与 Monaco diff 自带的 F7 同义（捕获阶段接管，避免被编辑器先吃掉）",
    run: () => activeTab()?.diffNav?.next(),
  },
  {
    id: "wb.diffPrev",
    keys: "Shift+F7",
    label: "差异视图：上一处差异（同一文件内）",
    group: "wb.diff",
    browser: "override",
    phase: "capture",
    when: () => !!activeTab()?.diffNav,
    run: () => activeTab()?.diffNav?.prev(),
  },
  {
    id: "wb.issueNext",
    keys: "F8",
    label: "下一处：下一个变更文件 / 下一处冲突",
    group: "wb.diff",
    phase: "capture",
    when: issueNavigable,
    note: "VSCode「下一个问题」同款：合并视图里跳冲突，多文件差异审视里切下一个变更文件",
    run: () => stepIssue(1),
  },
  {
    id: "wb.issuePrev",
    keys: "Shift+F8",
    label: "上一处：上一个变更文件 / 上一处冲突",
    group: "wb.diff",
    phase: "capture",
    when: issueNavigable,
    run: () => stepIssue(-1),
  },
  {
    id: "wb.markResolved",
    keys: "Alt+M",
    label: "合并视图：标记为解决（git add；M = 标记）",
    group: "wb.diff",
    when: mergeViewable,
    run: () => void mergeViewOf()?.markResolved(),
  },
  {
    id: "wb.navBack",
    keys: ["Alt+←", "Ctrl+-"],
    label: "后退（编辑位置历史，VSCode 同款）",
    group: "wb.file",
    browser: "override",
    phase: "capture",
    // 终端焦点不在内：Ctrl+- 在终端里是「缩小字号」（term-keys 后注册、按声明序命中会被这里抢走）；
    // Alt+← 也留给终端（工作台全局键在终端内一律让位的既有约定）
    focus: ["other", "editor", "input"],
    note: "VSCode：Windows/Linux 是 Alt+←，macOS 是 Ctrl+-；两个都接。Ctrl+- 接管浏览器「缩小页面」；终端内让位给字号键",
    run: () => void navGo(-1),
  },
  {
    id: "wb.navForward",
    keys: ["Alt+→", "Ctrl+Shift+-"],
    label: "前进（编辑位置历史，VSCode 同款）",
    group: "wb.file",
    browser: "override",
    phase: "capture",
    focus: ["other", "editor", "input"],
    note: "VSCode：Windows/Linux 是 Alt+→，macOS 是 Ctrl+Shift+-。Ctrl+Shift+- 接管浏览器「恢复页面缩放」",
    run: () => void navGo(1),
  },
  { id: "wb.menuClose", keys: "Esc", label: "关闭菜单", group: "wb.ui", focus: ["other", "editor", "input"], run: () => closeMenu() },
]

/** 保存：按活动标签分派到对应视图（文件内容 / 合并结果 / 暂存结果）。 */
function saveActive(): void {
  const t = activeTab()
  if (!t) return
  const merge = mergeViews.get(t.id)
  if (merge) {
    void merge.save()
    return
  }
  const stage = stageViews.get(t.id)
  if (stage) {
    void stage.save()
    return
  }
  if (t.kind === "file") void saveTab(t)
}

function toggleActiveMode(): void {
  const t = activeTab()
  if (t?.kind === "file") toggleMode(t)
}

/**
 * 快速打开文件：VSCode Quick Open 同款——弹出面板边打边模糊筛（↑↓ 选、Enter 开预览标签、
 * Ctrl+Enter 固定为常驻标签），空查询显示最近打开的文件。索引一次取回、之后全部在前端筛。
 */
async function quickOpen(): Promise<void> {
  openQuickOpen({
    api,
    root: () => explorer.getRoot(),
    open: (r, p, o) => openFile(r, p, { preview: o.preview, line: o.line }),
  })
}

/**
 * 由 Monaco **内置语言服务**提供符号的语言（本地 worker，非 LSP）：这些语言不在 `symbols-core` 的
 * 覆盖表里（与词法规则互补），它们的符号与定义跳转由编辑器自己的大纲动作给出。
 */
const BUILTIN_SYMBOL_LANGS = new Set(["typescript", "javascript", "json", "css", "scss", "less", "html"])

/**
 * 转到符号（当前文件内）。按符号来源分工，最终都是「列出当前文件的符号并跳过去」：
 * - **语义级来源**（内置语言服务覆盖的 TS/JS/JSON/CSS/HTML，以及本机有语言服务器的其它语言）：
 *   把焦点交给编辑器、触发 Monaco 自己的大纲动作——符号 provider 在 `symbols.ts` 里统一仲裁
 *   （LSP → tree-sitter → 词法），因此大纲能列出服务器给的符号（含工作台自己接的 LSP 文档符号）；
 *   触发不了（降级编辑器等）就退到下面的工作台面板（**同一份数据**）。
 * - **其余语言**（有语法文件走 tree-sitter，否则词法规则）：工作台符号面板，状态栏标出来源路径。
 */
function openSymbols(): void {
  const t = activeTab()
  if (t?.kind !== "file" || !t.editor) {
    toast("「转到符号」用于文件标签（差异与合并视图没有符号数据）", "warn")
    return
  }
  const lang = t.stat?.language ?? "plaintext"
  if (hasLsp(lang) || BUILTIN_SYMBOL_LANGS.has(lang)) {
    t.editor.focus()
    if (t.editor.showOutline()) return
  }
  const capable = t.editor.supportsSymbols()
  openSymbolPanel({
    symbols: () => t.editor?.listSymbols() ?? Promise.resolve([]),
    hint: () => (capable ? `当前语言：${lang}` : `${lang} 暂无符号提取`),
    source: () => t.editor?.symbolSource() ?? Promise.resolve("lexical"),
    jump: (sym) => t.editor?.revealLine(sym.line, sym.column + 1),
  })
}

/**
 * 刷新资源管理器 → Git 状态 →（面板可见时）Git 面板 → 变更面板。
 * Git 工具窗收起时不刷它的内部三栏：否则每次刷新都要付一遍分支/日志查询（分支栏还含 ≤N 次
 * rev-list），而面板根本看不见——展开时 toggleGitPanel 会补刷。
 */
function refreshAll(): void {
  void explorer
    .refresh(undefined, { keepSelection: true })
    .then(() => refreshGit())
    .then(() => {
      if (state.gitViewVisible) void gitPanel?.refresh()
    })
    .then(() => changesPanel?.refresh())
}

/** F2：对资源管理器选中项派发 contextmenu，弹出重命名菜单（与右键等价）。 */
function renameSelected(): void {
  const sel = explorer.selected()
  if (!sel) return
  const row = explorer.el.querySelector<HTMLElement>(`[data-path="${CSS.escape(sel.path)}"]`)
  const evt = new MouseEvent("contextmenu", {
    bubbles: true,
    clientX: row?.getBoundingClientRect().left ?? 100,
    clientY: row?.getBoundingClientRect().bottom ?? 100,
  })
  row?.dispatchEvent(evt)
}

/** 活动标签是否为多文件差异审视（跨文件导航的前提）。 */
function reviewable(): boolean {
  const t = activeTab()
  return t?.kind === "diff" && !!t.review && t.review.files.length >= 2
}

async function navigateActiveReview(dir: 1 | -1): Promise<void> {
  const t = activeTab()
  if (t) await navigateReview(t, dir)
}

/** 活动标签对应的合并视图（无则空）。 */
function mergeViewOf(): MergeView | undefined {
  const t = activeTab()
  return t ? mergeViews.get(t.id) : undefined
}

function mergeViewable(): boolean {
  return !!mergeViewOf()
}

/** F8/Shift+F8 的适用条件：合并视图（跳冲突）或多文件差异审视（切变更文件）在台上。 */
function issueNavigable(): boolean {
  return mergeViewable() || reviewable()
}

/**
 * F8/Shift+F8 的动作：按当前视图分派——合并视图跳冲突，差异审视切下一个/上一个变更文件。
 * 两者都是「下一处需要处理的地方」，合成一条键（VSCode 的 F8 语义），不再各占一个组合。
 */
function stepIssue(dir: 1 | -1): void {
  if (mergeViewable()) {
    mergeViewOf()?.gotoConflict(dir)
    return
  }
  if (reviewable()) void navigateActiveReview(dir)
}

/* ------------------------------ 编辑器重排（合并到一帧） ------------------------------ */

/**
 * 面板尺寸变化后重排编辑器。Monaco 虽然开了 automaticLayout，但容器显隐/尺寸切换存在时序差
 * （隐藏标签在 `display:none` 下量到 0 宽），所以仍需显式 layout 一次。
 *
 * 两点收敛：
 *   ① 同帧内的多次调用合并为一次——拖分界条时每个 mousemove 都会走到这里，早期实现是给**每个**
 *      标签各排一个 `setTimeout`，8 个标签 = 一帧内 8 次全量 relayout；
 *   ② 默认只排**活动标签**（非活动标签在下次 `activate` 时会重排），“整体变换”这类一次性动作
 *      用 `layoutAllEditors()` 补齐——拖动过程中的每帧给看不见的标签重排纯属浪费。
 */
let layoutRaf: number | null = null
function scheduleEditorLayout(): void {
  if (layoutRaf !== null) return
  layoutRaf = requestAnimationFrame(() => {
    layoutRaf = null
    activeTab()?.editor?.layout()
  })
}

/** 一次性重排全部标签（拖动结束这类“最终态”动作；先撤销待执行的合并帧避免重复）。 */
function layoutAllEditors(): void {
  if (layoutRaf !== null) {
    cancelAnimationFrame(layoutRaf)
    layoutRaf = null
  }
  for (const t of state.tabs) t.editor?.layout()
}

/* ------------------------------ 左侧栏抽屉（窄屏） ------------------------------ */

/**
 * 窄屏左栏抽屉：遮罩点击关闭、Esc 关闭、窗口变宽时自动退出抽屉态。
 *
 * 遮罩挂在 body（不在 .fw-app 内）：启动入场动画期间 .fw-app 带 transform，
 * 会成为 fixed 后代的包含块，遮罩会被 6px 位移拖出去。
 */
function bindLeftDrawer(): void {
  const scrim = h("div", { class: "fw-drawer-scrim" })
  scrim.onclick = () => setDrawerOpen(false)
  document.body.appendChild(scrim)
  // 宽度跨过断点：抽屉态只管窄屏，回到宽屏要回到桌面布局
  // （用 applyDrawer 而不是 setDrawerOpen：此刻媒体查询已变，后者会早退、把类与 Esc 作用域留成僵尸）
  window.matchMedia("(max-width: 700px)").addEventListener("change", () => applyDrawer(false))
}

/* ------------------------------ 面板拖拽调宽 ------------------------------ */

/**
 * 拖动分隔条改面板尺寸（鼠标与触屏同一套路径）。
 *
 * 用 Pointer Events 而不是 mousedown/mousemove：触屏上鼠标事件要等“单击或长按”的判定才发，
 * 拖动全程收不到中间事件。拖动期间指针可能离开拖条（甚至移出窗口），所以捕获指针，
 * 并同时监听 pointercancel（浏览器把这次手势接管为滚动时）。
 */
function bindResizer(resizer: HTMLElement, panel: HTMLElement, side: "left" | "right"): void {
  let dragPid: number | null = null
  /** 待写入宽度（拖动期间按帧合并：直接写 style 会每事件强制一次布局，而每帧只需最后一次值） */
  let pending: number | null = null
  let raf = 0
  const flush = (): void => {
    raf = 0
    if (pending === null) return
    panel.style.width = `${pending}px`
    pending = null
    scheduleEditorLayout()
  }
  resizer.addEventListener("pointerdown", (e) => {
    if (e.pointerType === "mouse" && e.button !== 0) return
    dragPid = e.pointerId
    e.preventDefault()
    resizer.setPointerCapture(e.pointerId)
    document.body.classList.add("fw-resizing")
  })
  resizer.addEventListener("pointermove", (e) => {
    if (dragPid === null || e.pointerId !== dragPid) return
    e.preventDefault()
    pending =
      side === "left"
        ? clampPanelWidth({ want: e.clientX, min: leftMinWidth() })
        : clampPanelWidth({ want: window.innerWidth - e.clientX, min: 240, max: 680 })
    if (!raf) raf = requestAnimationFrame(flush)
  })
  const end = (e: PointerEvent): void => {
    if (dragPid === null || e.pointerId !== dragPid) return
    dragPid = null
    document.body.classList.remove("fw-resizing")
    if (raf) cancelAnimationFrame(raf)
    flush()
    window.dispatchEvent(new Event("resize"))
    layoutAllEditors()
  }
  resizer.addEventListener("pointerup", end)
  resizer.addEventListener("pointercancel", end)
}

/** 底部工具窗高度（localStorage 记忆；双击拖条复位默认）。 */
const GIT_DOCK_H_KEY = "gebai.ui.gitDockH"
const GIT_DOCK_H_DEFAULT = 300

function readDockHeight(): number {
  try {
    const v = Number(localStorage.getItem(GIT_DOCK_H_KEY))
    return v >= 120 && v <= window.innerHeight * 0.8 ? v : GIT_DOCK_H_DEFAULT
  } catch {
    return GIT_DOCK_H_DEFAULT
  }
}

function applyDockHeight(h: number): void {
  document.documentElement.style.setProperty("--git-dock-h", `${h}px`)
  scheduleEditorLayout()
}

/** 拖动工具窗上沿调高（向上拖 = 变高），松手落盘高度并重排编辑器。 */
function bindDockResizer(): void {
  applyDockHeight(readDockHeight())
  let dragPid: number | null = null
  /** 工具窗底边（状态栏上沿）：拖动期间恒定，起手量一次（每帧量一次要连带强制布局） */
  let dockBottom = 0
  let pending: number | null = null
  let raf = 0
  const flush = (): void => {
    raf = 0
    if (pending === null) return
    applyDockHeight(pending)
    pending = null
  }
  gitDockResizer.addEventListener("pointerdown", (e) => {
    if (e.pointerType === "mouse" && e.button !== 0) return
    dragPid = e.pointerId
    e.preventDefault()
    gitDockResizer.setPointerCapture(e.pointerId)
    document.body.classList.add("fw-dock-resizing")
    dockBottom = statusbar.getBoundingClientRect().top
  })
  gitDockResizer.addEventListener("pointermove", (e) => {
    if (dragPid === null || e.pointerId !== dragPid) return
    e.preventDefault()
    // 工具窗底边固定在状态栏上沿（不是视口底：状态栏在工具窗下面，用 innerHeight 反推会差一个状态栏高度，
    // 表现为拖动时工具窗比指针慢一拍）
    const h = Math.max(120, Math.min(window.innerHeight * 0.8, dockBottom - e.clientY))
    pending = Math.round(h)
    if (!raf) raf = requestAnimationFrame(flush)
  })
  const end = (e: PointerEvent): void => {
    if (dragPid === null || e.pointerId !== dragPid) return
    dragPid = null
    document.body.classList.remove("fw-dock-resizing")
    if (raf) cancelAnimationFrame(raf)
    flush()
    const cur = parseInt(getComputedStyle(document.documentElement).getPropertyValue("--git-dock-h"), 10)
    try {
      if (cur) localStorage.setItem(GIT_DOCK_H_KEY, String(cur))
    } catch {
      /* 隐私模式忽略 */
    }
    window.dispatchEvent(new Event("resize"))
    layoutAllEditors()
  }
  gitDockResizer.addEventListener("pointerup", end)
  gitDockResizer.addEventListener("pointercancel", end)
  // 双击复位默认高度（与 IDEA 工具窗「重置布局」同理）
  gitDockResizer.addEventListener("dblclick", () => {
    applyDockHeight(GIT_DOCK_H_DEFAULT)
    try {
      localStorage.removeItem(GIT_DOCK_H_KEY)
    } catch {
      /* 忽略 */
    }
  })
}

/* ------------------------------ 启动 ------------------------------ */

/**
 * 等一帧（双 rAF）：刚挂载的外壳完成首次布局与绘制后再抹遮罩。
 * 为什么不用固定延时：延时值无法适配所有机器（慢了白等、快了看到空壳）；
 * 而「首帧已绘制」正是「外壳可看」的准确判据。
 */
function nextFrame(): Promise<void> {
  return new Promise((resolve) => {
    let done = false
    const finish = (): void => {
      if (done) return
      done = true
      resolve()
    }
    requestAnimationFrame(() => requestAnimationFrame(finish))
    // 后台标签页的 rAF 会被暂停/强节流（新标签后台打开的场景）——加超时兜底，
    // 保证后续启动步骤照常推进（宁可早抹遮罩，也不卡在半渡）
    setTimeout(finish, 200)
  })
}

/**
 * 启动占位（挂中央视图区）：根清单到达前给「正在准备工作区」的明确反馈。
 * 返回撤销函数——**按数据而不是按时间**撤（慢机器上不会残留，快机器上不白等）。
 */
function mountBootPlaceholder(): () => void {
  const box = h("div", { class: "fw-boot-placeholder" }, [h("div", { class: "fw-boot-orb" }), h("div", { class: "fw-boot-text", text: "正在准备工作区…" })])
  views.appendChild(box)
  return () => box.remove()
}

/** 全局拖拽上传：拖文件到页面任意处即上传到当前选中目录。 */
function bindDragUpload(): void {
  document.addEventListener("dragover", (e) => {
    if (e.dataTransfer?.types.includes("Files")) {
      e.preventDefault()
      document.body.classList.add("fw-dragging")
    }
  })
  document.addEventListener("dragleave", (e) => {
    if (e.relatedTarget === null) document.body.classList.remove("fw-dragging")
  })
  document.addEventListener("drop", (e) => {
    document.body.classList.remove("fw-dragging")
    if (!e.dataTransfer?.files.length) return
    const target = (e.target as HTMLElement).closest(".fw-tree-row, .fw-tree")
    if (target) return // 交给资源管理器自己的处理（带目标目录语义）
    e.preventDefault()
    const sel = explorer.selected()
    const dir = sel?.type === "dir" ? sel.path : ""
    void api
      .upload(explorer.getRoot(), Array.from(e.dataTransfer.files).map((f) => ({ file: f, path: dir ? `${dir}/${f.name}` : f.name })), false)
      .then(async (res) => {
        toast(`已上传 ${res.saved.length} 个文件`, "success")
        await explorer.refresh(dir)
        void refreshGit()
      })
      .catch((err) => toast(`上传失败：${(err as Error).message}`, "error"))
  })
}

/**
 * 离开确认：工作台里有未保存的改动时，关标签页 / 刷新 / 跳转先问一句。
 *
 * 为什么必须有：`Alt+W` 关的是工作台标签，但浏览器自己的 `Ctrl+W`（关闭标签页）页面拿不到、
 * 拦不住（Chromium 保留命令，见 `../keymap.ts` 的 `browserConflict()`）——误按就丢掉未保存的编辑内容。
 * 这道守卫补上那一步：有改动先确认，无改动不打扰（标签与会话状态本来就在 localStorage 里）。
 */
function bindUnsavedGuard(): void {
  const unsaved = (): number => {
    let n = state.tabs.filter((t) => t.dirty).length
    for (const v of mergeViews.values()) if (v.isDirty()) n++
    for (const v of stageViews.values()) if (v.isDirty()) n++
    return n
  }
  window.addEventListener("beforeunload", (e) => {
    if (unsaved() === 0) return
    e.preventDefault()
    e.returnValue = ""
  })
}

async function boot(): Promise<void> {
  // 二开初始化脚本（gebai.custom.js）：等其本地存储初始化 / 注册登录完成再继续（超时兜底，见 boot-config）
  await awaitCustomBoot()
  blockNativeContextMenu() // 全局禁掉浏览器原生右键菜单（自绘菜单不受影响，见 native-menu.ts）
  bindUnsavedGuard() // 有未保存改动时离开先确认（浏览器自己的 Ctrl+W 拦不住，这是兵底）
  installWorkbenchKeys(bindings) // 键盘快捷键：接管 document keydown（键位族与守卫见 ../keymap.ts）
  // 自检：新增键位若重复登记或撞上浏览器保留组合，控制台直接点名（同一张表在 keymap.test.ts 里也有断言）
  const keyIssues = validateKeymap(bindings)
  if (keyIssues.length) console.warn("[keymap] 工作台键位问题", keyIssues)
  // 主题：与主界面共用同一引擎（含人民币面额配色 / 默认主题黑白变体）。
  // urlPrefs:false —— 工作台不读 URL 上的主题参数：主题只认 localStorage（两页共享的用户级偏好，
  // 另有跨标签页 storage 同步），否则带旧 gb_style 的链接会把两页拆成两套配色。
  initTheme({ urlPrefs: false })
  const splash = document.getElementById("gb-splash")
  /**
   * 抹遮罩：**外壳已挂载并完成首帧**即可调用，不再等根清单/目录树/git 状态。
   * 为什么要提前：根清单在服务端要逐个探测仓库（实测首次 5s+），而外壳本身不依赖任何入参——
   * 把「看不清的空窗」换成「可看可交互的骨架 + 明确占位」，数据到达后自然填充（各视图本就有异步刷新路径）。
   * 同时给外壳一个极轻的入场衔接（fw-enter，纯 opacity/transform，不引发布局抖动）。
   */
  let splashGone = false
  const hideSplash = (): void => {
    if (splashGone) return
    splashGone = true
    rootEl.classList.add("fw-enter")
    if (!splash) return
    splash.classList.add("gb-splash-done")
    setTimeout(() => splash.remove(), 340)
  }
  /** 启动占位撤销函数（异常路径也要撤，不让占位残留）。 */
  let unmountPlaceholder: (() => void) | null = null
  try {
    // ── 阶段一：同步搭好外壳（不依赖任何网络往返）──
    showLeftView("explorer", { keepHidden: true }) // 先建好左栏，可见性随后由 URL/默认值决定
      // 工具窗此刻只摆骨架（deferData）——数据等根确定后再取（空根会误判「不是仓库」）
  setDock(state.dockVisible, state.dockView, { deferData: true })
  // dock 展开但无数据时给一行加载提示（否则启动空窗期是一块无信息的大空框）
  const dockLoading = state.dockVisible ? h("div", { class: "fw-dock-loading", text: state.dockView === "terminal" ? "正在准备终端…" : "正在加载 Git 信息…" }) : null
  if (dockLoading) gitDock.appendChild(dockLoading)
    bindResizer(leftResizer, leftPanel, "left")
    bindDockResizer()
    document.body.appendChild(rootEl)
    bindLeftDrawer()
    bindDragUpload()
    unmountPlaceholder = mountBootPlaceholder()
    // 外壳首次绘制即抹遮罩（首屏不等根清单往返）
    await nextFrame()
    hideSplash()

    // ── 阶段二：数据装配（不阻塞首屏可见性）──
    // 语言服务器清单与首屏数据并行拉取（deep link / 记忆恢复会立刻打开文件，清单未就绪时由 attachDocument 等它）；
    // 跨文件跳转（转到定义）折算回工作台路径后交给工作台开标签，而不是让 Monaco 静默失败
    void initLsp()
    setLspSessionProvider(() => state.sessionId)
    setLspNotifier((msg, kind) => toast(msg, kind ?? "info", 6000))
    setLspOpener((jump) => openLspTarget(jump))
    await loadRoots()
    // 状态记忆与 URL **取并集**：先按记忆把上次的标签恢复出来，再让 URL 落位（它决定活动标签）。
    // 为什么不是「URL 带 path 就整段跳过记忆」：普通 F5 的地址栏里总带着当前文件（activate 会同步
    // 地址栏），跳过记忆就变成「一次刷新只剩那一个文件」，而随后的写回会把记忆也改成缩水状态
    // ——另一个标签从此再也回不来。新建标签页的深链接不受影响：新标签页的 sessionStorage 本就是空的。
    const savedSession = loadSession()
    if (savedSession) await restoreSession(savedSession)
    // URL 恢复：进过哪个目录/打开过哪个文件，刷新或前进后退都回到原处（见 restoreFromUrl）；
    // 该文件已在记忆里时 openFile 命中已有标签、只把它激活并跳行，不会重复打开。
    // 与记忆恢复同因：这是「回到原处」不是导航，不进位置历史
    await withNavGuard(() => restoreFromUrl())
    // git 状态先就绪（未就绪就建面板会把「状态未知」画成「当前根不是 Git 仓库」）；
    // 与 onRootChanged 的触发合并，不会多跑一轮往返
    await refreshGit()
    renderTabbar()
    renderStatus()
    renderRail()
    unmountPlaceholder()
    unmountPlaceholder = null
      // 根与状态都就绪：现在才建面板并取数据（阶段一只摆了骨架，见 setDock 的 deferData）
  setDock(state.dockVisible, state.dockView)
  dockLoading?.remove()
    // 深层链接：?root=proj:gebai&path=src/main.ts&line=10&diff=1
    const params = new URLSearchParams(location.search)
    const diffRoot = params.get("diffRoot")
    if (diffRoot) {
      const from = params.get("from") ?? "HEAD"
      const to = params.get("to") ?? WORKTREE
      void openCompare({ from, to, path: params.get("path") ?? "", mergeBase: params.get("mergeBase") === "1" })
    }
    // Monaco 空闲预热放在**数据装配之后**：启动期真正在等的是根清单/状态/读取这些请求，
    // 把 1MB 编辑器内核的下载排在它们前面只会互相抢带宽（预热本身仍是 idle 调度）。
    // 语言服务器清单先落地再预热 Monaco：符号 provider 的「有 LSP 的语言让位」依赖这份清单；
    // 探测失败/未启用时 initLsp 内部已收敛为空集，预热照常。
    void initLsp().then(() => prewarmMonaco())
    // 变更监听：目录树/变更面板/已打开文件「自己变」的通道（长轮询 + 后端 fs.watch）。
    // 排在最后启动——它是一条常驻请求，不值得与首屏数据争带宽；后台标签页里它自己会按需退场。
    fsWatcher.start()
  }
  // boot 内部任何异常：仍移除遮罩（页面可见，错误以 toast/占位页表现），遄免白屏无反馈
  catch (err) {
    toast(`初始化失败：${(err as Error).message}`, "error", 8000)
    hideSplash()
    unmountPlaceholder?.()
  }
}

void boot()
