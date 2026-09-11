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
import { resolveDeepLink } from "./deeplink"
import { createMergeView, type MergeView } from "./merge-view"
import { FsApi, ApiError, type FileStat, type GitStatusInfo, type ReadResponse, type RootInfo, type RootsResponse } from "./api"
// 文件工作台自带样式：base.css 提供设计令牌（主题 CSS 只换令牌），files.css 负责本页布局
import "../css/base.css"
import "../css/files.css"
import { createEditor, refreshEditorTheme, monacoReady, type EditorHandle } from "./editor"
import { createExplorer } from "./explorer"
import { createChangesPanel, type ChangesPanel } from "./changes"
import { createUrlSync, parseUrlState } from "./url-state"
import { initTheme, setAcrylicLt, setCnyScheme, setTheme, type AcrylicLtId, type CnySchemeId, type ThemeId } from "../theme-core"
import { createGitPanel, diffEndpointsFor, mountDiffView, type DiffSpec, type GitPanel } from "./git"
import type { DiffNav } from "./editor"
import { createCompareView, WORKTREE, type CompareView } from "./compare"
import { renderViewer, downloadUrl, diagramKindOf, type ViewerCtx } from "./viewers"
import { h, icon, clear, toast, formatSize, formatTime, extOf, confirmDialog, promptDialog, showMenu, dropdown, closeMenu } from "./ui"

/* ------------------------------ 全局状态 ------------------------------ */

const LOCAL_ENV_KEY = "gebai.ui.env"
const SESSION_KEY = "gebai.ui.session"

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
  /** 差异块导航（F7/Shift+F7 与标签栏按钮共用；降级渲染为 null） */
  diffNav?: DiffNav | null
  /**
   * 变更文件清单（与差异同一端点对）：跨文件导航用。
   * 打开差异后异步取，未就绪时标签栏按钮置灰。
   */
  review?: ReviewCtx
  /** 标签图标覆盖（合并视图用 merge 图标，其余按 kind/dirty 推断） */
  icon?: string
}

const state = {
  env: readLocalEnv(),
  sessionId: resolveSessionId(),
  rootsResp: null as RootsResponse | null,
  roots: [] as RootInfo[],
  gitStatus: null as GitStatusInfo | null,
  repoPrefix: "",
  tabs: [] as Tab[],
  activeId: null as string | null,
  leftView: "explorer" as "changes" | "explorer" | "search",
  gitViewVisible: window.innerWidth >= 1180,
  cursor: { line: 1, column: 1, selected: 0 },
}

const api = new FsApi(() => state.env, () => state.sessionId)

/* ------------------------------ 布局骨架 ------------------------------ */

// 无标题栏（IDEA 新 UI 的做法：去掉传统菜单栏，把入口交给左侧活动栏与工具窗自身）
const railEl = h("div", { class: "fw-rail" })
const leftPanel = h("div", { class: "fw-left" })
const leftResizer = h("div", { class: "fw-resizer", title: "拖动调整宽度" })
const tabbar = h("div", { class: "fw-tabbar" })
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
    showHidden: !!state.rootsResp?.showHidden,
  }),
  openFile: (root, path) => void openFile(root, path, { preview: false }),
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
})

/* ------------------------------ 变更面板（左栏工具窗） ------------------------------ */

let changesPanel: ChangesPanel | null = null

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
    openCompare: (init) => void openCompare(init),
    openMerge: (repoRel) => void openMergeTab(repoRel),
    writable: () => !!(state.rootsResp?.writable && state.rootsResp?.gitWrite),
    remoteEnabled: () => !!(state.rootsResp?.gitRemote && state.rootsResp?.writable),
    onFsChanged: () => void explorer.refresh(undefined, { keepSelection: true }),
    openFileHistory: (path) => void showFileHistoryByPath(path),
    // 徽标：改动数变化时只重画 rail（不重画左栏，避免提交框里的输入被打断）
    onCount: () => renderRail(),
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
  gitDock.appendChild(gitPanel.el)
  return gitPanel
}

/* ------------------------------ 根与 Git 状态 ------------------------------ */

async function loadRoots(): Promise<void> {
  try {
    const res = await api.roots()
    state.rootsResp = res
    state.roots = res.roots
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

async function onRootChanged(rootId: string): Promise<void> {
  await refreshGit()
  if (state.gitViewVisible && gitPanel) void gitPanel.refresh()
  const info = state.roots.find((r) => r.id === rootId)
  state.repoPrefix = ""
  if (info?.isRepo && info.repoRoot) {
    const rel = info.path.replace(/[\\/]+$/, "").replace(/\\/g, "/")
    const repo = info.repoRoot.replace(/[\\/]+$/, "").replace(/\\/g, "/")
    state.repoPrefix = rel.startsWith(repo) ? rel.slice(repo.length).replace(/^\//, "") : ""
  }
  renderRail()
}

async function refreshGit(): Promise<GitStatusInfo | null> {
  try {
    const status = await api.gitStatus(explorer.getRoot())
    state.gitStatus = status
    // 回填树的 Git 装饰：树首次渲染时状态还没到（异步），不回填则徽标/下划线永不出现
    explorer.refreshGitDecorations()
    if (status.isRepo && status.rootPath) {
      const info = state.roots.find((r) => r.id === explorer.getRoot())
      state.repoPrefix = status.rootPath
        ? ((info?.path ?? "").replace(/[\\/]+$/, "").replace(/\\/g, "/").startsWith(status.rootPath) ? (info?.path ?? "").replace(/[\\/]+$/, "").replace(/\\/g, "/").slice(status.rootPath.length).replace(/^\//, "") : "")
        : ""
    }
  } catch {
    state.gitStatus = null
    explorer.refreshGitDecorations()
  }
  renderStatus()
  // 变更面板与状态栏同源：状态一变就同步（只在它已创建时刷新，避免无谓重渲染）
  changesPanel?.refresh()
  return state.gitStatus
}

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
    return { root: explorer.getRoot(), path, line, session: state.sessionId }
  },
  onPop: (st) => restoreFromUrlState(st),
})

/** 从地址栏恢复（启动与浏览器前进后退共用）。 */
async function restoreFromUrlState(st: Partial<{ root: string; path: string; line?: number }>): Promise<void> {
  if (st.root && st.root !== explorer.getRoot()) {
    await explorer.setRoot(st.root, undefined)
  }
  if (!st.path) return
  // 目录 → 展开并在树中定位；文件 → 打开（浅层链接解析已在 deeplink.ts 完成根推断）
  const isLikelyDir = !st.path.includes(".")
  if (isLikelyDir) {
    await explorer.reveal(st.path, { select: true })
  } else {
    await openFile(explorer.getRoot(), st.path, { preview: false, line: st.line })
  }
}

/** 启动恢复：优先用已有的 ?root/?path（deeplink 已在 loadRoots 里落位），否则按根清单推断。 */
async function restoreFromUrl(): Promise<void> {
  const st = parseUrlState(location.search)
  if (st.path) await restoreFromUrlState(st)
  else if (st.root && st.root !== explorer.getRoot()) await explorer.setRoot(st.root, undefined)
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

/** 标签栏对「差异块计数」的订阅退订函数（标签栏每次重建都换一个）。 */
let diffNavUnsub: (() => void) | null = null

async function openFile(root: string, path: string, opts: { preview?: boolean; line?: number; forceText?: boolean } = {}): Promise<void> {
  if (!path) return
  const id = tabId("file", root, path)
  const exist = findTab(id)
  if (exist) {
    if (opts.preview === false) exist.preview = false
    activate(id)
    if (opts.line && exist.editor) exist.editor.revealLine(opts.line - 1)
    return
  }
  // 预览标签：单击树里的文件时复用同一个预览标签（VSCode 行为），双击/固定时转为常驻
  if (opts.preview !== false) {
    const prev = state.tabs.find((t) => t.preview && t.kind === "file")
    if (prev) {
      const sameId = prevId(prev, root, path)
      if (sameId) {
        // 复用：把它切到新路径
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
  await loadTab(tab, { line: opts.line, forceText: opts.forceText })
  renderTabbar()
}

function prevId(prev: Tab, root: string, path: string): boolean {
  return prev.root === root && prev.path !== path
}

/** 加载标签内容：文本/图表走 Monaco；其它走对应查看器。 */
async function loadTab(tab: Tab, opts: { line?: number; forceText?: boolean } = {}): Promise<void> {
  const host0 = viewHosts.get(tab.id)
  if (!host0) return
  clear(host0)
  host0.appendChild(h("div", { class: "fw-loading", text: `正在打开 ${tab.title}…` }))
  try {
    const statRes = await api.stat(tab.root, [tab.path])
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
      let read: ReadResponse
      try {
        read = await api.read(tab.root, tab.path, { maxBytes: Math.min(state.rootsResp?.maxRead ?? 10 * 1024 * 1024, 10 * 1024 * 1024), forceText: opts.forceText })
      } catch (err) {
        throw err
      }
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
      const editor = await createEditor(editorHost, {
        value: read.content,
        language: read.language,
        readOnly: tab.mode !== "edit" || read.truncated,
      })
      tab.editor = editor
      editor.onChange(() => {
        const dirty = editor.getValue() !== tab.baseline
        if (dirty !== tab.dirty) {
          tab.dirty = dirty
          renderTabbar()
        }
      })
      editor.onCursor((info) => {
        if (activeTab()?.id !== tab.id) return
        state.cursor = info
        renderStatus()
      })
      if (opts.line) setTimeout(() => editor.revealLine(opts.line! - 1), 60)
    } else {
      clear(host0)
      tab.viewDispose = renderViewer(host0, viewerCtx(tab))
    }
    // 标签栏右侧动作依赖 tab.stat（能否编辑/是否图表…）与 kind，加载完才齐
    renderTabbar()
    renderStatus()
  } catch (err) {
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
  const view = await mountDiffView(host, api, spec, { repoRootPath: info?.repoRoot ?? "", language: languageOf(spec.path) })
  tab.diffDispose = view.dispose
  tab.diffNav = view.nav
  renderTabbar()
  // 变更文件清单异步取（不挡首屏）：拿到后标签栏的跨文件导航才可用
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

/** 填充标签的 review 上下文（失败静默：只是没有跨文件导航，差异本身照常看）。 */
async function prepareReview(tab: Tab, spec: DiffSpec): Promise<void> {
  try {
    const files = await fetchReviewFiles(spec)
    if (!files.length) return
    // 已销毁的标签不再回填（异步期间用户可能已关掉）
    if (!findTab(tab.id)) return
    tab.review = { files, index: files.indexOf(spec.path) }
    if (state.activeId === tab.id) renderTabbar()
  } catch {
    /* 清单拿不到就不给跨文件导航 */
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
  diffNavUnsub?.()
  diffNavUnsub = null
  rekeyTab(tab, diffTabId(spec, spec.path))
  tab.path = spec.path
  tab.title = `◧ ${spec.title}`
  tab.diffSpec = spec
  if (tab.review) tab.review = { files: tab.review.files, index: reviewIndex }
  clear(host)
  renderTabbar()
  const info = state.roots.find((r) => r.id === spec.root)
  const view = await mountDiffView(host, api, spec, { repoRootPath: info?.repoRoot ?? "", language: languageOf(spec.path) })
  tab.diffDispose = view.dispose
  tab.diffNav = view.nav
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

/** 仓库相对路径 → 当前根相对路径（root 是仓库子目录时剥离前缀；不在子树内则原样返回，避免误开）。 */
function toRootPath(repoRel: string): string {
  const prefix = state.repoPrefix
  if (!prefix) return repoRel
  if (repoRel === prefix) return ""
  return repoRel.startsWith(`${prefix}/`) ? repoRel.slice(prefix.length + 1) : repoRel
}

/** 文件路径 → Monaco 语言 id（差异视图与编辑器共用）。 */
function languageOf(path: string): string {
  const ext = extOf(path)
  const map: Record<string, string> = {
    ts: "typescript", tsx: "typescript", js: "javascript", jsx: "javascript", mjs: "javascript", cjs: "javascript",
    json: "json", md: "markdown", css: "css", scss: "scss", less: "less", html: "html", htm: "html", xml: "xml", svg: "xml",
    yml: "yaml", yaml: "yaml", py: "python", sh: "shell", bash: "shell", ps1: "powershell", go: "go", rs: "rust",
    java: "java", kt: "kotlin", c: "c", h: "c", cpp: "cpp", hpp: "cpp", cs: "csharp", php: "php", rb: "ruby",
    sql: "sql", toml: "ini", ini: "ini", vue: "html", svelte: "html", puml: "plaintext", d2: "plaintext", mmd: "plaintext",
  }
  return map[ext] ?? "plaintext"
}

function activate(id: string): void {
  const tab = findTab(id)
  if (!tab) return
  // 保存上一个标签的滚动位置
  const prev = activeTab()
  if (prev?.editor && prev.id !== id) prev.scrollTop = prev.editor.getScrollTop()
  state.activeId = id
  for (const [tid, host] of viewHosts) host.classList.toggle("active", tid === id)
  if (tab.editor && tab.scrollTop) tab.editor.setScrollTop(tab.scrollTop)
  setTimeout(() => tab.editor?.layout(), 20)
  renderTabbar()
  renderStatus()
  if (tab.kind === "file") void explorer.reveal(tab.path, { select: true })
  // 当前文件变了 → 地址栏就地替换（不新增历史：连开多个文件不该要按多次后退）
  urlSync.replace()
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
  tab.viewDispose?.()
  tab.diffDispose?.()
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
}

/* ------------------------------ 渲染：标签栏 / 工具条 / 状态栏 ------------------------------ */

function renderTabbar(): void {
  // 退掉上一轮对差异计数的订阅（DOM 马上被清空，留着就是野订阅）
  diffNavUnsub?.()
  diffNavUnsub = null
  clear(tabbar)
  for (const t of state.tabs) {
    const el = h("div", { class: `fw-tab${t.id === state.activeId ? " active" : ""}${t.preview ? " preview" : ""}` }, [
      t.icon ? icon(t.icon, 12) : t.kind === "diff" ? icon("diff", 12) : icon(t.dirty ? "edit" : "file", 12),
      h("span", { class: "fw-tab-title", text: t.title, title: t.kind === "diff" ? t.title : `${t.root} :: ${t.path}` }),
      t.dirty ? h("span", { class: "fw-tab-dot", title: "未保存" }) : null,
      (() => {
        const b = h("button", { class: "fw-tab-close", title: "关闭（Ctrl+W）" }, [icon("close", 11)])
        b.onclick = (e) => {
          e.stopPropagation()
          closeTab(t.id)
        }
        return b
      })(),
    ])
    el.onclick = () => activate(t.id)
    el.onmousedown = (e) => {
      if (e.button === 1) {
        e.preventDefault()
        closeTab(t.id)
      }
    }
    el.oncontextmenu = (e) => {
      e.preventDefault()
      showMenu(e.clientX, e.clientY, [
        { label: "关闭", icon: "close", shortcut: "Ctrl+W", onClick: () => closeTab(t.id) },
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
    tabbar.appendChild(el)
  }
  const spacer = h("div", { class: "fw-tabbar-spacer" })
  spacer.ondblclick = async () => {
    const name = await promptDialog({ title: "快速打开文件", label: "文件路径（相对当前根）", placeholder: "src/main.ts" })
    if (name?.trim()) void openFile(explorer.getRoot(), name.trim(), { preview: false })
  }
  tabbar.appendChild(spacer)
  tabbar.appendChild(tabActions())
}

/**
 * 标签栏右侧动作区：**只放当前标签相关的图标按钮**（无文字，靠 title 提示）。
 *
 * 为什么收进标签栏而不是单独一行工具条：那行工具条有一半宽度被面包屑占着，
 * 而面包屑的信息（在哪、什么文件）标签与资源管理器已经分别表达了；按钮归到标签栏后
 * 省下一整行纵向空间给代码，且"当前标签能做什么"就在标签旁边，不用跨行找。
 */
function tabActions(): HTMLElement {
  const box = h("div", { class: "fw-tabbar-actions" })
  const t = activeTab()
  if (!t) return box

  if (t.kind === "diff") {
    // 两组导航，箭头方向区分语义：左右 = 换文件（跨文件），上下 = 换差异（文件内）
    const review = t.review
    const hasList = !!review && review.files.length > 1
    const prevFile = btn("chevronLeft", "上一个变更文件（Ctrl+Alt+↑）", () => void navigateReview(t, -1))
    const nextFile = btn("chevronRight", "下一个变更文件（Ctrl+Alt+↓）", () => void navigateReview(t, 1))
    prevFile.disabled = !hasList
    nextFile.disabled = !hasList
    const fileCount = h("span", {
      class: "fw-nav-count",
      text: review ? `${review.index < 0 ? "–" : review.index + 1} / ${review.files.length}` : "…",
      title: review ? `变更文件：第 ${review.index < 0 ? "?" : review.index + 1} 个，共 ${review.files.length} 个` : "正在获取变更文件清单…",
    })
    box.appendChild(h("span", { class: "fw-nav-group" }, [prevFile, fileCount, nextFile]))

    if (t.diffNav) {
      const nav = t.diffNav
      const prevDiff = btn("chevronUp", "上一处差异（Shift+F7）", () => nav.prev())
      const nextDiff = btn("chevronDown", "下一处差异（F7）", () => nav.next())
      const diffCount = h("span", { class: "fw-nav-count", text: "—", title: "当前差异块 / 总差异块" })
      // 计数由差异视图驱动（滚动也会变）；标签栏每次重建都要退订，故留着退订函数
      diffNavUnsub?.()
      diffNavUnsub = nav.onChange((s) => {
        diffCount.textContent = s.total ? `${s.index || 1} / ${s.total}` : "无差异"
        prevDiff.disabled = !s.total
        nextDiff.disabled = !s.total
      })
      box.appendChild(h("span", { class: "fw-nav-group" }, [prevDiff, diffCount, nextDiff]))
    }
    return box
  }

  // 合并视图自带工具条（且没有 stat）——不重复给按钮
  if (t.kind !== "file" || !t.stat) return box

  const editable = !!t.stat.editable && !t.truncated && state.rootsResp?.writable !== false
  const modeBtn = btn(t.mode === "edit" ? "eye" : "edit", t.mode === "edit" ? "切换为查看（Ctrl+E）" : editable ? "编辑（Ctrl+E）" : "该文件类型不支持编辑", () => toggleMode(t), t.mode === "edit" ? "active" : "")
  modeBtn.disabled = !editable
  box.appendChild(modeBtn)

  const saveBtn = btn("save", t.dirty ? "保存（Ctrl+S）· 有未保存的修改" : "保存（Ctrl+S）", () => void saveTab(t), t.dirty ? "primary" : "")
  saveBtn.disabled = !t.dirty || !state.rootsResp?.writable
  box.appendChild(saveBtn)

  if (diagramKindOf(extOf(t.path))) {
    box.appendChild(
      btn("diff", "源码 / 渲染预览切换", () => {
        const host = viewHosts.get(t.id)
        if (!host) return
        if (host.querySelector(".fw-diagram-wrap")) void loadTab(t)
        else renderViewer(host, viewerCtx(t))
      }),
    )
  }

  box.appendChild(btn("download", "下载", () => window.open(downloadUrl({ api, root: t.root, path: t.path }), "_blank")))
  if (state.gitStatus?.isRepo) box.appendChild(btn("history", "文件历史（Git log --follow）", () => void showFileHistoryByPath(t.path, t.root)))
  box.appendChild(btn("refresh", "重新加载当前文件", () => void loadTab(t)))
  box.appendChild(btn("copy", "复制路径", () => void navigator.clipboard.writeText(t.path).then(() => toast("已复制路径", "success"))))
  return box
}

function btn(iconName: string, title: string, onClick: () => void, cls = ""): HTMLButtonElement {
  const b = h("button", { class: `fw-icon-btn ${cls}`, title })
  b.appendChild(icon(iconName, 13))
  b.onclick = onClick
  return b
}


/** 按路径看文件历史（工具栏与变更面板右键共用）。 */
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

function renderStatus(): void {
  clear(statusbar)
  const tab = activeTab()
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
  const root = state.roots.find((r) => r.id === explorer.getRoot())
  const rel = h("button", { class: "fw-status-item", title: root?.path ?? "" }, [icon(root?.isRepo ? "git" : "folderOpen", 12), h("span", { text: root?.name ?? "-" })])
  rel.onclick = () => showMenu(...menuAt(rel, state.roots.map((r) => ({ label: r.name, icon: "folder", onClick: () => void explorer.setRoot(r.id) }))))
  statusbar.appendChild(btn(rel, 1))

  if (state.gitStatus?.isRepo) {
    const s = state.gitStatus
    const branch = h("button", { class: "fw-status-item git", title: "切换分支" }, [
      icon("branch", 12),
      h("span", { text: s.branch ?? (s.detached ? "(detached)" : "-") }),
      s.ahead ? h("span", { class: "fw-ahead", text: `↑${s.ahead}` }) : null,
      s.behind ? h("span", { class: "fw-behind", text: `↓${s.behind}` }) : null,
      s.counts.staged + s.counts.unstaged + s.counts.untracked + s.counts.conflicted > 0
        ? h("span", { class: "fw-status-changes", text: `${s.counts.staged}± ${s.counts.unstaged}± ${s.counts.untracked}?` })
        : null,
    ])
    branch.onclick = () => {
      if (!state.gitViewVisible) toggleGitPanel(true)
      gitPanel?.show("branches")
    }
    statusbar.appendChild(btn(branch, 1))
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
    const mtime = statInfo?.mtime ?? 0
    statusbar.appendChild(item("", { pri: 3, title: formatTime(mtime), text: new Date(mtime).toLocaleString("zh-CN", { hour12: false }) }))
  }
  statusbar.appendChild(item("", { pri: 3, title: `编辑器内核：${monacoReady() ? "Monaco（VSCode 同款）" : "轻量降级模式"}`, text: monacoReady() ? "Monaco" : "轻量模式" }))
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

function renderRail(): void {
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
      const b = h("button", { class: `fw-rail-btn${state.gitViewVisible ? " active" : ""}`, title: "源代码管理工具窗（Ctrl+Alt+G）" })
      b.appendChild(icon("git", 18))
      b.onclick = () => toggleGitPanel(!state.gitViewVisible)
      return b
    })(),
    // 「打开文件夹（切换根）」已移除：切根在资源管理器顶部的根选择按钮里（那里还带根清单与面包屑语义）
    (() => {
      const b = h("button", { class: "fw-rail-btn", title: "回收站" })
      b.appendChild(icon("trash", 18))
      b.onclick = () => void showTrash()
      return b
    })(),
    (() => {
      // 菜单栏移除后，菜单里的杂项收进这一个入口（新建/上传/比较/快捷键/服务端开关/全屏/回主界面）
      const b = h("button", { class: "fw-rail-btn", title: "更多（新建 / 比较 / 重新加载 / 快捷键 / 服务端开关 / 全屏 / 在新标签打开）" })
      b.appendChild(icon("settings", 18))
      b.onclick = () => {
        const r = b.getBoundingClientRect()
        showMenu(r.left, r.top - 6, [
          { label: "新建文件…", icon: "plus", disabled: !state.rootsResp?.writable, onClick: () => void newQuick("file") },
          { label: "新建文件夹…", icon: "plus", disabled: !state.rootsResp?.writable, onClick: () => void newQuick("dir") },
          { label: "上传文件…", icon: "upload", disabled: !state.rootsResp?.writable, onClick: () => pickUpload() },
          { separator: true },
          { label: "比较任意两端…", icon: "diff", shortcut: "Ctrl+Shift+D", onClick: () => void openCompare() },
          { label: "刷新根清单与 Git 状态", icon: "refresh", onClick: () => void loadRoots().then(() => explorer.refresh("")) },
          { separator: true },
          { label: "快捷键一览", icon: "info", onClick: () => showShortcuts() },
          { label: "服务端开关（GEBAI_FS_* / GEBAI_GIT_*）", icon: "settings", onClick: () => showEnvHelp() },
          // 页面级动作归页面自己：分屏面板已经没有标题栏了，重新加载/在新标签打开都在这里
          { label: "重新加载工作台", icon: "refresh", onClick: () => location.reload() },
          ...(EMBEDDED ? [{ label: "在新标签打开", icon: "external", onClick: () => requestOpenInTab() }] : []),
          { label: "全屏", icon: "expand", onClick: () => void (document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen()) },
          { separator: true },
          // 嵌入态（分屏）下"返回主界面"= 关掉分屏容器；独立标签页才是整页跳回
          EMBEDDED
            ? { label: "关闭分屏", icon: "back", onClick: () => requestCloseSplit() }
            : { label: "返回歌白主界面", icon: "back", onClick: () => { location.href = `${(import.meta.env.BASE_URL || "/").replace(/\/$/, "")}/` } },
        ])
      }
      return b
    })(),
  )
  /*
   * 分屏（嵌入）时在活动栏**最下方**给一个「关闭分屏」。
   * 为什么放这里：标题栏的 ✕ 已按反馈移除，而嵌入态下面板自己没有顶栏——鼠标用户要关分屏，
   * 只剩「更多」菜单里的那一项（两步）。站在最下方、图标与「更多」里的那一项同款（back），
   * 既好找又不占编辑区。独立标签页时不存在“分屏”，故仅 EMBEDDED 渲染。
   * （单独 append：railEl.append 不收 null，上面那串是定长列表。）
   */
  if (EMBEDDED) {
    const close = h("button", { class: "fw-rail-btn", title: "关闭分屏（Ctrl+Shift+E / Esc）" })
    close.appendChild(icon("back", 18))
    close.onclick = () => requestCloseSplit()
    railEl.appendChild(close)
  }
}

/* ------------------------------ 查看/编辑与保存 ------------------------------ */

function toggleMode(tab: Tab): void {
  tab.mode = tab.mode === "edit" ? "view" : "edit"
  tab.editor?.setReadOnly(tab.mode !== "edit" || !!tab.truncated)
  if (tab.mode === "edit") {
    tab.editor?.focus()
    toast("已进入编辑模式（Ctrl+S 保存）", "info", 2200)
  }
  renderTabbar()
  renderStatus()
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
    tab.etag = res.etag
    tab.baseline = content
    tab.content = content
    tab.dirty = false
    tab.mode = "edit"
    tab.editor.setReadOnly(false)
    renderTabbar()
    toast("已保存", "success", 1600)
    if (state.gitStatus?.isRepo) void refreshGit().then(() => gitPanel?.refresh())
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

/* ------------------------------ 回收站 ------------------------------ */

async function showTrash(): Promise<void> {
  const overlay = h("div", { class: "fw-overlay" })
  const listHost = h("div", { class: "fw-history-list" })
  const dialog = h("div", { class: "fw-dialog wide" }, [
    h("div", { class: "fw-dialog-title" }, [icon("trash"), h("span", { text: "回收站（软删除的文件）" })]),
    h("div", { class: "fw-dialog-body" }, [listHost]),
    h("div", { class: "fw-dialog-actions" }, []),
  ])
  overlay.appendChild(dialog)
  overlay.onclick = (e) => {
    if (e.target === overlay) overlay.remove()
  }
  document.body.appendChild(overlay)

  const render = async () => {
    clear(listHost)
    listHost.appendChild(h("div", { class: "fw-loading", text: "读取回收站…" }))
    try {
      const res = await api.trash()
      clear(listHost)
      if (!res.batches.length) {
        listHost.appendChild(h("div", { class: "fw-empty", text: "回收站是空的" }))
        return
      }
      for (const b of res.batches) {
        const row = h("div", { class: "fw-trash-batch" }, [
          h("div", { class: "fw-trash-head" }, [
            icon("archive"),
            h("span", { text: `${b.items.length} 项 · ${formatTime(b.createdAt)}` }),
            h("span", { class: "fw-grow" }),
            (() => {
              const btn2 = h("button", { class: "fw-btn sm", text: "恢复" })
              btn2.onclick = async () => {
                try {
                  const r = await api.restoreTrash(b.batch, false)
                  toast(`已恢复 ${r.restored} 项${r.skipped.length ? `（${r.skipped.length} 项因已存在而跳过）` : ""}`, "success")
                  await explorer.refresh(undefined, { keepSelection: true })
                  await render()
                } catch (err) {
                  toast(`恢复失败：${(err as Error).message}`, "error")
                }
              }
              return btn2
            })(),
            (() => {
              const btn2 = h("button", { class: "fw-btn sm danger", text: "彻底删除" })
              btn2.onclick = async () => {
                const ok = await confirmDialog({ title: "彻底删除", message: "该批次将从磁盘永久删除，无法恢复。确定？", okText: "永久删除", danger: true })
                if (!ok) return
                await api.purgeTrash(b.batch)
                await render()
              }
              return btn2
            })(),
          ]),
          h("div", { class: "fw-trash-items" }, b.items.slice(0, 20).map((it) => h("div", { class: "fw-trash-item", text: `${it.root} :: ${it.path}` }))),
        ])
        listHost.appendChild(row)
      }
    } catch (err) {
      clear(listHost)
      listHost.appendChild(h("div", { class: "fw-error", text: `读取失败：${(err as Error).message}` }))
    }
  }
  void render()
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
 * 是否被嵌在宿主页面里（主界面「分屏打开」把本页放进 iframe）。
 * 两个跨界动作靠 postMessage 桥接：主题同步、返回主界面。
 */
const EMBEDDED = window.self !== window.top

if (EMBEDDED) {
  window.addEventListener("message", (e: MessageEvent) => {
    // 只认同源且来自宿主窗口的消息
    if (e.origin !== location.origin || e.source !== window.parent) return
    const data = e.data as { type?: string; theme?: string | null; cnyScheme?: string | null; acrylicLt?: string | null } | null
    if (data?.type !== "gebai:theme") return
    // 宿主侧改主题时同步过来（工作台是独立文档，不会自己跟着变）
    if (data.theme) void setTheme(data.theme as ThemeId)
    setCnyScheme((data.cnyScheme as CnySchemeId | null) ?? null)
    setAcrylicLt((data.acrylicLt as AcrylicLtId | null) ?? null)
  })

  /*
   * 嵌入态的 Esc：宿主那侧收不到 iframe 里的按键，这里转发一次。
   *
   * 用**捕获阶段 + 事前检查浮层**，而不是“捕获阶段 stopPropagation 后再转发”：
   * 菜单/对话框的 Escape 处理器都在冒泡阶段（且不 stopPropagation），等它们关完菜单再判断，
   * DOM 里已经看不到浮层了，分屏会跟着一起关——用户只想关个菜单，却把整个工作台也关了。
   * 在捕获阶段先看一眼“现在有没有浮层”：有就说明这一下 Esc 是冲着它去的，直接放手。
   */
  document.addEventListener(
    "keydown",
    (e) => {
      if (e.key !== "Escape") return
      if (document.querySelector(".fw-overlay, .fw-menu-pop")) return
      requestCloseSplit()
    },
    true,
  )
}

/** 通知宿主关闭分屏（嵌入态下"返回主界面"的正确语义：关掉容器，而不是把 iframe 导航走）。 */
function requestCloseSplit(): void {
  window.parent.postMessage({ type: "gebai:files-close-split" }, location.origin)
}

/** 通知宿主把当前工作台另开一个标签页（嵌入态下自己 window.open 会丢宿主侧的参数上下文）。 */
function requestOpenInTab(): void {
  window.parent.postMessage({ type: "gebai:files-open-tab" }, location.origin)
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
      toRootPath,
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
      openFile: (r, path) => void openFile(r, toRootPath(path), { preview: false }),
      onFsChanged: () => void refreshGit(),
    },
    { from, to, path: init.path, mergeBase: init.mergeBase ?? false, basePath: state.repoPrefix },
  )
  host.appendChild(view.el)
  compareTabs.set(id, { view, state: { from, to, mergeBase: init.mergeBase ?? false, path: init.path ?? "" } })
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
  clear(leftPanel)
  if (view === "explorer") leftPanel.appendChild(explorer.el)
  else if (view === "search") {
    if (!searchView) searchView = { el: buildSearchView() }
    leftPanel.appendChild(searchView.el)
  } else {
    const panel = ensureChangesPanel()
    leftPanel.appendChild(panel.el)
    // 先渲染（用上一份状态），状态刷新到位后再渲染一次——否则首次打开是空面板
    panel.refresh()
    void refreshGit().then(() => panel.refresh())
  }
  if (!opts.keepHidden) setLeftVisible(true)
  renderRail()
}

/** 左栏是否展开（隐藏后编辑区占满——IDEA 的 Ctrl+B 行为）。 */
function leftVisible(): boolean {
  return leftPanel.style.display !== "none"
}

function setLeftVisible(visible: boolean): void {
  leftPanel.style.display = visible ? "" : "none"
  leftResizer.style.display = visible ? "" : "none"
  for (const t of state.tabs) setTimeout(() => t.editor?.layout(), 0)
  renderRail()
}

/** 点当前视图按钮 = 收起左栏；点其它视图 = 切换（并展开）。 */
function toggleLeftView(view: "changes" | "explorer" | "search"): void {
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

function toggleGitPanel(visible: boolean): void {
  state.gitViewVisible = visible
  gitDock.classList.toggle("collapsed", !visible)
  gitDockResizer.classList.toggle("collapsed", !visible)
  if (visible) {
    ensureGitPanel()
    void gitPanel?.refresh()
    // 展开后 Monaco 可视高度变化，重排编辑器（否则出现空白/裁切）
    for (const t of state.tabs) setTimeout(() => t.editor?.layout(), 30)
  }
  renderRail()
}

function showShortcuts(): void {
  const rows: Array<[string, string]> = [
    ["Ctrl+P", "快速打开文件（相对当前根）"],
    ["Ctrl+S", "保存当前文件"],
    ["Ctrl+E", "切换查看 / 编辑模式"],
    ["Ctrl+W", "关闭当前标签"],
    ["Ctrl+B", "显示/隐藏左侧栏"],
    ["Ctrl+Shift+E", "资源管理器"],
    ["Ctrl+Shift+F", "搜索"],
    ["Ctrl+Shift+G", "左侧变更面板"],
    ["Ctrl+Alt+G", "底部 Git 工具窗"],
    ["Ctrl+K", "更多（新建 / 比较 / 服务端开关）"],
    ["Ctrl+Shift+D", "比较（任意两个提交 / 提交与工作区）"],
    ["F2", "重命名选中项"],
    ["Delete", "删除选中项（移入回收站）"],
    ["F7 / Shift+F7", "差异视图：下一处 / 上一处差异（Alt+↑↓ 同效）"],
    ["Ctrl+Alt+↓ / ↑", "差异视图：下一个 / 上一个变更文件"],
    ["F9 / F8", "合并视图：下一个 / 上一个冲突"],
    ["F5", "刷新资源管理器与 Git 状态"],
    ["Ctrl+Enter（提交框内）", "提交"],
  ]
  const overlay = h("div", { class: "fw-overlay" })
  const dialog = h("div", { class: "fw-dialog" }, [
    h("div", { class: "fw-dialog-title" }, [icon("info"), h("span", { text: "快捷键" })]),
    h("div", { class: "fw-dialog-body" }, [h("div", { class: "fw-kbd-list" }, rows.map(([k, d]) => h("div", { class: "fw-kbd-row" }, [h("kbd", { text: k }), h("span", { text: d })])))]),
    h("div", { class: "fw-dialog-actions" }, [(() => {
      const b = h("button", { class: "fw-btn primary", text: "知道了" })
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

function showEnvHelp(): void {
  const lines = [
    "GEBAI_FS_ENABLED    文件工作台总开关（默认 true）",
    "GEBAI_FS_WRITE      写开关（false = 纯只读检视）",
    "GEBAI_FS_ROOTS      额外白名单根（JSON 数组）",
    "GEBAI_FS_MAX_READ   单次读取上限（字节，默认 10MB）",
    "GEBAI_FS_MAX_WRITE  单次写入上限（默认 10MB）",
    "GEBAI_FS_MAX_UPLOAD 上传单文件上限（默认 100MB）",
    "GEBAI_FS_MAX_ZIP    打包下载上限（默认 500MB）",
    "GEBAI_FS_HIDDEN     默认显示隐藏文件",
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

/**
 * 跨文件导航单独用**捕获阶段**监听（Ctrl+Alt+↓ / Ctrl+Alt+↑）。
 *
 * 为什么不能放在下面那个冒泡阶段的全局处理器里：这两个组合是 **Monaco 的多光标快捷键**
 * （insertCursorBelow/Above），编辑器获焦时事件到不了冒泡阶段。跨文件导航是"看代码"时的
 * 高频动作，不该因为焦点在编辑器里就失灵——捕获先于 Monaco 自己的 keybinding 服务。
 *
 * 不用 Alt+←/→（更顺手）：那是浏览器前进/后退，会把工作台整页导航走。
 */
document.addEventListener(
  "keydown",
  (e) => {
    if (!(e.ctrlKey || e.metaKey) || !e.altKey) return
    if (e.key !== "ArrowDown" && e.key !== "ArrowUp") return
    const t = activeTab()
    if (t?.kind !== "diff" || !t.review || t.review.files.length < 2) return
    e.preventDefault()
    e.stopPropagation()
    void navigateReview(t, e.key === "ArrowDown" ? 1 : -1)
  },
  true,
)

document.addEventListener("keydown", (e) => {
  const ctrl = e.ctrlKey || e.metaKey
  const target = e.target as HTMLElement
  const inInput = target instanceof HTMLInputElement || target instanceof HTMLTextAreaElement || target.isContentEditable
  if (ctrl && !e.shiftKey && e.key.toLowerCase() === "s") {
    e.preventDefault()
    const t = activeTab()
    if (t?.kind === "file") void saveTab(t)
    return
  }
  if (ctrl && !e.shiftKey && e.key.toLowerCase() === "e") {
    e.preventDefault()
    const t = activeTab()
    if (t?.kind === "file") toggleMode(t)
    return
  }
  if (ctrl && !e.shiftKey && e.key.toLowerCase() === "p") {
    e.preventDefault()
    void (async () => {
      const name = await promptDialog({ title: "快速打开文件", label: "文件路径（相对当前根）", placeholder: "src/main.ts" })
      if (name?.trim()) void openFile(explorer.getRoot(), name.trim(), { preview: false })
    })()
    return
  }
  if (ctrl && !e.shiftKey && e.key.toLowerCase() === "w") {
    e.preventDefault()
    if (state.activeId) closeTab(state.activeId)
    return
  }
  if (ctrl && e.shiftKey && e.key.toLowerCase() === "e") {
    e.preventDefault()
    showLeftView("explorer")
    return
  }
  if (ctrl && e.shiftKey && e.key.toLowerCase() === "f") {
    e.preventDefault()
    showLeftView("search")
    return
  }
  // Ctrl+Shift+G = 左侧变更面板（与 IDEA 的 Git 工具窗语义一致）；Ctrl+Alt+G = 底部工具窗
  if (ctrl && e.shiftKey && e.key.toLowerCase() === "g") {
    e.preventDefault()
    toggleLeftView("changes")
    return
  }
  if (ctrl && e.altKey && e.key.toLowerCase() === "g") {
    e.preventDefault()
    toggleGitPanel(!state.gitViewVisible)
    return
  }
  if (ctrl && !e.shiftKey && !e.altKey && e.key.toLowerCase() === "k") {
    e.preventDefault()
    ;(railEl.querySelector('.fw-rail-btn[title^="更多"]') as HTMLElement | null)?.click()
    return
  }
  if (ctrl && e.shiftKey && e.key.toLowerCase() === "d") {
    e.preventDefault()
    void openCompare()
    return
  }
  if (ctrl && !e.shiftKey && e.key.toLowerCase() === "b") {
    e.preventDefault()
    setLeftVisible(!leftVisible())
    return
  }
  // 差异块导航（只在差异标签上生效）：F7/Shift+F7 同 IDEA；Alt+↑↓ 是编辑器习惯的别名。
  // 冲突合并标签的 F8/F9 由 merge-view 自己接管（那边导航的是冲突块，语义不同）。
  const navKey = e.key === "F7" || (e.altKey && (e.key === "ArrowDown" || e.key === "ArrowUp"))
  if (navKey) {
    const nav = activeTab()?.diffNav
    if (nav) {
      e.preventDefault()
      if (e.shiftKey || e.key === "ArrowUp") nav.prev()
      else nav.next()
      return
    }
  }
  if (e.key === "F5") {
    e.preventDefault()
    void explorer
      .refresh(undefined, { keepSelection: true })
      .then(() => refreshGit())
      .then(() => gitPanel?.refresh())
      .then(() => changesPanel?.refresh())
    return
  }
  if (e.key === "F2" && !inInput) {
    const sel = explorer.selected()
    if (sel) {
      e.preventDefault()
      const row = explorer.el.querySelector<HTMLElement>(`[data-path="${CSS.escape(sel.path)}"]`)
      const evt = new MouseEvent("contextmenu", { bubbles: true, clientX: row?.getBoundingClientRect().left ?? 100, clientY: row?.getBoundingClientRect().bottom ?? 100 })
      row?.dispatchEvent(evt)
    }
    return
  }
  if (e.key === "Escape") closeMenu()
})

/* ------------------------------ 面板拖拽调宽 ------------------------------ */

function bindResizer(resizer: HTMLElement, panel: HTMLElement, side: "left" | "right"): void {
  let dragging = false
  resizer.addEventListener("mousedown", (e) => {
    dragging = true
    e.preventDefault()
    document.body.classList.add("fw-resizing")
  })
  window.addEventListener("mousemove", (e) => {
    if (!dragging) return
    const width = side === "left" ? Math.max(180, Math.min(560, e.clientX)) : Math.max(240, Math.min(680, window.innerWidth - e.clientX))
    panel.style.width = `${width}px`
  })
  window.addEventListener("mouseup", () => {
    if (!dragging) return
    dragging = false
    document.body.classList.remove("fw-resizing")
    window.dispatchEvent(new Event("resize"))
  })
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
  for (const t of state.tabs) setTimeout(() => t.editor?.layout(), 0)
}

/** 拖动工具窗上沿调高（向上拖 = 变高），松手落盘高度并重排编辑器。 */
function bindDockResizer(): void {
  applyDockHeight(readDockHeight())
  let dragging = false
  gitDockResizer.addEventListener("mousedown", (e) => {
    dragging = true
    e.preventDefault()
    document.body.classList.add("fw-dock-resizing")
  })
  window.addEventListener("mousemove", (e) => {
    if (!dragging) return
    // 工具窗底边固定在状态栏上沿（不是视口底：状态栏在工具窗下面，用 innerHeight 反推会差一个状态栏高度，
    // 表现为拖动时工具窗比指针慢一拍）
    const bottom = statusbar.getBoundingClientRect().top
    const h = Math.max(120, Math.min(window.innerHeight * 0.8, bottom - e.clientY))
    applyDockHeight(Math.round(h))
  })
  window.addEventListener("mouseup", () => {
    if (!dragging) return
    dragging = false
    document.body.classList.remove("fw-dock-resizing")
    const cur = parseInt(getComputedStyle(document.documentElement).getPropertyValue("--git-dock-h"), 10)
    try {
      if (cur) localStorage.setItem(GIT_DOCK_H_KEY, String(cur))
    } catch {
      /* 隐私模式忽略 */
    }
    window.dispatchEvent(new Event("resize"))
    for (const t of state.tabs) t.editor?.layout()
  })
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

async function boot(): Promise<void> {
  // 主题：与主界面共用同一引擎（含人民币面额配色 / 默认主题黑白变体 / 品牌化变量）
  initTheme()
  // 启动遮罩：首屏（根清单 + 目录树 + 编辑器就绪）后淡出移除；异常也移除，不让遮罩卡住页面
  const splash = document.getElementById("gb-splash")
  const hideSplash = (): void => {
    if (!splash) return
    splash.classList.add("gb-splash-done")
    setTimeout(() => splash.remove(), 340)
  }
  try {
    showLeftView("explorer", { keepHidden: true }) // 先建好左栏，可见性随后由 URL/默认值决定
    toggleGitPanel(state.gitViewVisible && window.innerWidth >= 1180)
    if (!state.gitViewVisible) toggleGitPanel(false) // 同步 collapsed class（避免先渲染后收起闪动）
    bindResizer(leftResizer, leftPanel, "left")
    bindDockResizer()
    document.body.appendChild(rootEl)
    // 全局拖拽上传：拖文件到页面任意处即上传到当前选中目录
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
    await loadRoots()
    // URL 恢复：进过哪个目录/打开过哪个文件，刷新或前进后退都回到原处（见 restoreFromUrl）
    await restoreFromUrl()
    await refreshGit()
    renderTabbar()
    renderStatus()
    renderRail()
    // 深层链接：?root=proj:gebai&path=src/main.ts&line=10&diff=1
    const params = new URLSearchParams(location.search)
    const diffRoot = params.get("diffRoot")
    if (diffRoot) {
      const from = params.get("from") ?? "HEAD"
      const to = params.get("to") ?? WORKTREE
      void openCompare({ from, to, path: params.get("path") ?? "", mergeBase: params.get("mergeBase") === "1" })
    }
    // 首个文件标签的编辑器布局就绪后再抹遮罩（避免看到空壳布局）
    setTimeout(hideSplash, 120)
  }
  // boot 内部任何异常：仍移除遮罩（页面可见，错误以 toast/占位页表现），避免白屏无反馈
  catch (err) {
    toast(`初始化失败：${(err as Error).message}`, "error", 8000)
    hideSplash()
  }
}

void boot()
