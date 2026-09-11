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
import { createGitPanel, mountDiffView, type DiffSpec, type GitPanel } from "./git"
import { createCompareView, WORKTREE, type CompareView } from "./compare"
import { renderViewer, downloadUrl, diagramKindOf, type ViewerCtx } from "./viewers"
import { h, icon, clear, toast, formatSize, formatTime, extOf, confirmDialog, promptDialog, showMenu, dropdown, closeMenu } from "./ui"

/* ------------------------------ 全局状态 ------------------------------ */

const LOCAL_ENV_KEY = "gebai.ui.env"
const SESSION_KEY = "gebai.ui.session"
const STYLE_KEY = "gebai.ui.style"
const THEMES = ["acrylic", "aether", "cyberpunk", "aurora", "synthwave", "matrix", "tokyo-night", "ink", "cny", "qinhan"]

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
  leftView: "explorer" as "explorer" | "search",
  gitViewVisible: window.innerWidth >= 1180,
  cursor: { line: 1, column: 1, selected: 0 },
}

const api = new FsApi(() => state.env, () => state.sessionId)

/* ------------------------------ 布局骨架 ------------------------------ */

const menubar = h("header", { class: "fw-menubar" })
const railEl = h("div", { class: "fw-rail" })
const leftPanel = h("div", { class: "fw-left" })
const leftResizer = h("div", { class: "fw-resizer", title: "拖动调整宽度" })
const tabbar = h("div", { class: "fw-tabbar" })
const toolbar = h("div", { class: "fw-toolbar" })
const views = h("div", { class: "fw-views" })
const rightPanel = h("div", { class: "fw-right" })
const rightResizer = h("div", { class: "fw-resizer", title: "拖动调整宽度" })
const statusbar = h("footer", { class: "fw-statusbar" })

const rootEl = h("div", { class: "fw-app" }, [
  menubar,
  h("div", { class: "fw-body" }, [railEl, leftPanel, leftResizer, h("div", { class: "fw-center" }, [tabbar, toolbar, views]), rightResizer, rightPanel]),
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
})

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
    writable: () => !!(state.rootsResp?.writable && state.rootsResp?.gitWrite),
    remoteEnabled: () => !!(state.rootsResp?.gitRemote && state.rootsResp?.writable),
    onFsChanged: () => void explorer.refresh(undefined, { keepSelection: true }),
  })
  rightPanel.appendChild(gitPanel.el)
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
    if (status.isRepo && status.rootPath) {
      const info = state.roots.find((r) => r.id === explorer.getRoot())
      state.repoPrefix = status.rootPath
        ? ((info?.path ?? "").replace(/[\\/]+$/, "").replace(/\\/g, "/").startsWith(status.rootPath) ? (info?.path ?? "").replace(/[\\/]+$/, "").replace(/\\/g, "/").slice(status.rootPath.length).replace(/^\//, "") : "")
        : ""
    }
  } catch {
    state.gitStatus = null
  }
  renderStatus()
  return state.gitStatus
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
          renderToolbar()
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
    renderToolbar()
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
  tab.diffDispose = await mountDiffView(host, api, spec, { repoRootPath: info?.repoRoot ?? "", language: languageOf(spec.path) })
  renderTabbar()
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
  renderToolbar()
  renderStatus()
  if (tab.kind === "file") void explorer.reveal(tab.path, { select: true })
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
      renderToolbar()
      renderStatus()
    }
  } else renderTabbar()
}

/* ------------------------------ 渲染：标签栏 / 工具条 / 状态栏 ------------------------------ */

function renderTabbar(): void {
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
  const actions = h("div", { class: "fw-tabbar-actions" }, [
    btn("copy", "复制当前文件路径", () => {
      const t = activeTab()
      if (t) void navigator.clipboard.writeText(t.path).then(() => toast("已复制路径", "success"))
    }),
    btn("history", "当前文件的 Git 历史", () => {
      const t = activeTab()
      if (!t?.path) return
      if (!gitPanel) ensureGitPanel()
      gitPanel?.show("log")
      toast("已切换到日志视图（可在日志中过滤该文件）", "info")
    }),
    btn("refresh", "重新加载当前文件", () => {
      const t = activeTab()
      if (t?.kind === "file") void loadTab(t)
    }),
    btn("close", "关闭右侧 Git 面板", () => toggleGitPanel(!state.gitViewVisible)),
  ])
  tabbar.appendChild(actions)
}

function btn(iconName: string, title: string, onClick: () => void, cls = ""): HTMLButtonElement {
  const b = h("button", { class: `fw-icon-btn ${cls}`, title })
  b.appendChild(icon(iconName, 13))
  b.onclick = onClick
  return b
}

function renderToolbar(): void {
  clear(toolbar)
  const tab = activeTab()
  if (!tab) {
    toolbar.appendChild(h("span", { class: "fw-hint", text: "从左侧资源管理器打开文件，或用 Ctrl+P 快速打开" }))
    return
  }
  const info = state.roots.find((r) => r.id === tab.root)
  const crumbs = h("div", { class: "fw-toolbar-path" })
  const rootCrumb = h("button", { class: "fw-link", text: info?.name ?? tab.root })
  rootCrumb.onclick = () => void explorer.reveal("")
  crumbs.appendChild(rootCrumb)
  const parts = tab.path.split("/")
  let acc = ""
  for (const p of parts) {
    acc = acc ? `${acc}/${p}` : p
    const target = acc
    crumbs.append(icon("chevronRight", 11))
    const seg = h("button", { class: "fw-link", text: p })
    seg.onclick = () => void explorer.reveal(target)
    crumbs.appendChild(seg)
  }
  toolbar.appendChild(crumbs)

  const actions = h("div", { class: "fw-toolbar-actions" })
  if (tab.kind === "file" && tab.stat) {
    const editable = tab.stat.editable && !tab.truncated && !state.rootsResp?.writable === false
    const modeBtn = h("button", { class: `fw-btn sm${tab.mode === "edit" ? " primary" : ""}`, title: editable ? "切换查看 / 编辑（Ctrl+E）" : "该文件类型不支持编辑" }, [icon(tab.mode === "edit" ? "eye" : "edit"), h("span", { text: tab.mode === "edit" ? "查看" : "编辑" })])
    modeBtn.disabled = !editable
    modeBtn.onclick = () => toggleMode(tab)
    actions.appendChild(modeBtn)

    const saveBtn = h("button", { class: "fw-btn sm", title: "保存（Ctrl+S）" }, [icon("save"), h("span", { text: "保存" })])
    saveBtn.disabled = !tab.dirty || !state.rootsResp?.writable
    saveBtn.onclick = () => void saveTab(tab)
    actions.appendChild(saveBtn)

    if (diagramKindOf(extOf(tab.path))) {
      const b = h("button", { class: "fw-btn sm", title: "源码 / 渲染预览切换" }, [icon("diff"), h("span", { text: "渲染预览" })])
      b.onclick = () => {
        const host = viewHosts.get(tab.id)
        if (!host) return
        const hasPreview = host.querySelector(".fw-diagram-wrap")
        if (hasPreview) {
          void loadTab(tab)
        } else {
          host.appendChild(document.createElement("div"))
          renderViewer(host, viewerCtx(tab))
        }
      }
      actions.appendChild(b)
    }

    const dl = h("button", { class: "fw-btn sm", title: "下载" }, [icon("download"), h("span", { text: "下载" })])
    dl.onclick = () => window.open(downloadUrl({ api, root: tab.root, path: tab.path }), "_blank")
    actions.appendChild(dl)

    if (state.gitStatus?.isRepo) {
      const hist = h("button", { class: "fw-btn sm", title: "文件历史（Git log --follow）" }, [icon("history"), h("span", { text: "历史" })])
      hist.onclick = () => void showFileHistory(tab)
      actions.appendChild(hist)
    }
  }
  if (tab.kind === "diff") {
    actions.appendChild(h("span", { class: "fw-hint", text: "只读差异视图" }))
  }
  toolbar.appendChild(actions)
}

async function showFileHistory(tab: Tab): Promise<void> {
  try {
    const res = await api.gitFileHistory(tab.root, tab.path, 50)
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
        void openDiff({ title: `${tab.title} @ ${c.short}`, root: tab.root, path: tab.path, source: { type: "commit", hash: c.hash } })
      }
      list.appendChild(row)
    }
    const dialog = h("div", { class: "fw-dialog wide" }, [
      h("div", { class: "fw-dialog-title" }, [icon("history"), h("span", { text: `文件历史：${tab.title}` })]),
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
  const root = state.roots.find((r) => r.id === explorer.getRoot())
  const rel = h("button", { class: "fw-status-item", title: root?.path ?? "" }, [icon(root?.isRepo ? "git" : "folderOpen", 12), h("span", { text: root?.name ?? "-" })])
  rel.onclick = () => showMenu(...menuAt(rel, state.roots.map((r) => ({ label: r.name, icon: "folder", onClick: () => void explorer.setRoot(r.id) }))))
  statusbar.appendChild(rel)

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
    statusbar.appendChild(branch)
  }

  statusbar.appendChild(h("span", { class: "fw-grow" }))

  if (tab?.kind === "file" && tab.stat) {
    const t = tab
    statusbar.appendChild(h("span", { class: "fw-status-item", title: "字符编码（点击切换保存编码）" }, []))
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
    statusbar.appendChild(encBtn)

    const eolBtn = h("button", { class: "fw-status-item", title: "行尾序列（保存时按此写回；keep 表示保持编辑器内容原样）" }, [h("span", { text: t.eol === "crlf" ? "CRLF" : t.eol === "mixed" ? "混合" : "LF" })])
    eolBtn.onclick = () =>
      dropdown(eolBtn, [
        { label: "LF（Unix）", icon: t.eol === "lf" ? "check" : undefined, onClick: () => setEol(t, "lf") },
        { label: "CRLF（Windows）", icon: t.eol === "crlf" ? "check" : undefined, onClick: () => setEol(t, "crlf") },
        { label: "保持内容原样", icon: t.eol === "keep" ? "check" : undefined, onClick: () => setEol(t, "keep") },
      ])
    statusbar.appendChild(eolBtn)

    const statInfo = t.stat
    statusbar.appendChild(h("span", { class: "fw-status-item", text: statInfo?.language || "plaintext" }))
    statusbar.appendChild(h("span", { class: "fw-status-item", text: t.mode === "edit" ? "编辑" : "只读" }))
    if (t.editor) {
      statusbar.appendChild(h("span", { class: "fw-status-item", text: `行 ${state.cursor.line}，列 ${state.cursor.column}${state.cursor.selected ? `（选中 ${state.cursor.selected}）` : ""}` }))
    }
    statusbar.appendChild(h("span", { class: "fw-status-item", text: formatSize(statInfo?.size ?? 0) }))
    const mtime = statInfo?.mtime ?? 0
    statusbar.appendChild(h("span", { class: "fw-status-item", title: formatTime(mtime), text: new Date(mtime).toLocaleString("zh-CN", { hour12: false }) }))
  }
  statusbar.appendChild(h("span", { class: "fw-status-item", title: `编辑器内核：${monacoReady() ? "Monaco（VSCode 同款）" : "轻量降级模式"}`, text: monacoReady() ? "Monaco" : "轻量模式" }))
  if (state.rootsResp && !state.rootsResp.writable) statusbar.appendChild(h("span", { class: "fw-status-item warn", text: "只读模式" }))
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
      renderToolbar()
    }
  }
}

function menuAt(anchor: HTMLElement, items: Parameters<typeof showMenu>[2]): [number, number, Parameters<typeof showMenu>[2]] {
  const r = anchor.getBoundingClientRect()
  return [r.left, r.top - Math.min(320, items.length * 30) - 6, items]
}

function renderRail(): void {
  clear(railEl)
  const mk = (id: "explorer" | "search" | "git", iconName: string, title: string, onClick?: () => void) => {
    const b = h("button", { class: `fw-rail-btn${state.leftView === id && !onClick ? " active" : ""}`, title })
    b.appendChild(icon(iconName, 18))
    if (state.gitStatus?.isRepo && id === "git") {
      const n = state.gitStatus.counts.staged + state.gitStatus.counts.unstaged + state.gitStatus.counts.untracked + state.gitStatus.counts.conflicted
      if (n) b.appendChild(h("span", { class: "fw-rail-badge", text: String(n) }))
    }
    b.onclick = onClick ?? (() => showLeftView(id as "explorer" | "search"))
    return b
  }
  railEl.append(
    mk("explorer", "folder", "资源管理器（Ctrl+Shift+E）"),
    mk("search", "search", "搜索（Ctrl+Shift+F）"),
    mk("git", "git", "源代码管理（Ctrl+Shift+G）", () => toggleGitPanel(true)),
    h("div", { class: "fw-rail-spacer" }),
    (() => {
      const b = h("button", { class: "fw-rail-btn", title: "打开文件夹" })
      b.appendChild(icon("folderOpen", 18))
      b.onclick = () => (document.querySelector(".fw-root-btn") as HTMLElement | null)?.click()
      return b
    })(),
    (() => {
      const b = h("button", { class: "fw-rail-btn", title: "回收站" })
      b.appendChild(icon("trash", 18))
      b.onclick = () => void showTrash()
      return b
    })(),
    (() => {
      const b = h("button", { class: "fw-rail-btn", title: `切换主题（当前 ${document.documentElement.dataset.theme ?? "acrylic"}）` })
      b.appendChild(icon("settings", 18))
      b.onclick = () => {
        dropdown(b, THEMES.map((t) => ({ label: t, icon: document.documentElement.dataset.theme === t ? "check" : undefined, onClick: () => void applyTheme(t) })))
      }
      return b
    })(),
  )
}

/* ------------------------------ 查看/编辑与保存 ------------------------------ */

function toggleMode(tab: Tab): void {
  tab.mode = tab.mode === "edit" ? "view" : "edit"
  tab.editor?.setReadOnly(tab.mode !== "edit" || !!tab.truncated)
  if (tab.mode === "edit") {
    tab.editor?.focus()
    toast("已进入编辑模式（Ctrl+S 保存）", "info", 2200)
  }
  renderToolbar()
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
    renderToolbar()
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
        renderToolbar()
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

async function applyTheme(id: string): Promise<void> {
  document.documentElement.dataset.theme = id
  try {
    localStorage.setItem(STYLE_KEY, id)
  } catch {
    /* 隐私模式忽略 */
  }
  const linkId = "fw-theme-css"
  document.getElementById(linkId)?.remove()
  await new Promise<void>((resolve) => {
    const link = document.createElement("link")
    link.id = linkId
    link.rel = "stylesheet"
    link.href = new URL(`../themes/${id}.css`, import.meta.url).href
    link.onload = () => resolve()
    link.onerror = () => resolve()
    document.head.appendChild(link)
  })
  refreshEditorTheme()
  renderRail()
}

function bootstrapTheme(): void {
  const fromUrl = new URLSearchParams(location.search).get("gb_style")
  let id = fromUrl && THEMES.includes(fromUrl) ? fromUrl : null
  if (!id) {
    try {
      const saved = localStorage.getItem(STYLE_KEY)
      if (saved && THEMES.includes(saved)) id = saved
    } catch {
      /* ignore */
    }
  }
  if (!id) {
    const globalStyle = (window as unknown as Record<string, unknown>).__GEBAI_UI_STYLE__
    if (typeof globalStyle === "string" && THEMES.includes(globalStyle)) id = globalStyle
  }
  void applyTheme(id ?? "acrylic")
}

document.addEventListener("gebai:theme-change", () => refreshEditorTheme())

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
  renderToolbar()
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
  renderToolbar()
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

function showLeftView(view: "explorer" | "search"): void {
  state.leftView = view
  clear(leftPanel)
  if (view === "explorer") leftPanel.appendChild(explorer.el)
  else {
    if (!searchView) searchView = { el: buildSearchView() }
    leftPanel.appendChild(searchView.el)
  }
  renderRail()
}

/* ------------------------------ 菜单栏 ------------------------------ */

function buildMenubar(): void {
  clear(menubar)
  const brand = h("a", { class: "fw-brand", href: `${(import.meta.env.BASE_URL || "/").replace(/\/$/, "")}/`, title: "返回歌白主界面" }, [icon("wheel", 16), h("span", { text: "歌白文件" })])
  const mkMenu = (label: string, items: Parameters<typeof showMenu>[2]) => {
    const b = h("button", { class: "fw-menu-btn", text: label })
    b.onclick = () => {
      const r = b.getBoundingClientRect()
      showMenu(r.left, r.bottom + 2, items)
    }
    return b
  }
  menubar.append(
    brand,
    mkMenu("文件", [
      { label: "新建文件…", icon: "plus", disabled: !state.rootsResp?.writable, onClick: () => void newQuick("file") },
      { label: "新建文件夹…", icon: "plus", disabled: !state.rootsResp?.writable, onClick: () => void newQuick("dir") },
      { separator: true },
      { label: "上传文件…", icon: "upload", disabled: !state.rootsResp?.writable, onClick: () => pickUpload() },
      { label: "打开文件夹…", icon: "folderOpen", onClick: () => (document.querySelector(".fw-root-btn") as HTMLElement | null)?.click() },
      { separator: true },
      { label: "保存", icon: "save", shortcut: "Ctrl+S", onClick: () => {
        const t = activeTab()
        if (t) void saveTab(t)
      } },
      { label: "重新加载当前文件", icon: "refresh", onClick: () => {
        const t = activeTab()
        if (t?.kind === "file") void loadTab(t)
      } },
      { separator: true },
      { label: "回收站…", icon: "trash", onClick: () => void showTrash() },
    ]),
    mkMenu("编辑", [
      { label: "进入/退出编辑模式", icon: "edit", shortcut: "Ctrl+E", onClick: () => {
        const t = activeTab()
        if (t && t.kind === "file") toggleMode(t)
      } },
      { label: "在当前文件中查找", icon: "search", shortcut: "Ctrl+F", onClick: () => {
        const t = activeTab()
        t?.editor?.focus()
        document.dispatchEvent(new KeyboardEvent("keydown", { key: "f", ctrlKey: true }))
      } },
      { separator: true },
      { label: "复制路径", icon: "copy", onClick: () => {
        const t = activeTab()
        if (t) void navigator.clipboard.writeText(t.path).then(() => toast("已复制", "success"))
      } },
      { label: "下载当前文件", icon: "download", onClick: () => {
        const t = activeTab()
        if (t?.kind === "file") window.open(downloadUrl({ api, root: t.root, path: t.path }), "_blank")
      } },
    ]),
    mkMenu("视图", [
      { label: "资源管理器", icon: "folder", shortcut: "Ctrl+Shift+E", onClick: () => showLeftView("explorer") },
      { label: "搜索", icon: "search", shortcut: "Ctrl+Shift+F", onClick: () => showLeftView("search") },
      { separator: true },
      { label: state.gitViewVisible ? "隐藏 Git 面板" : "显示 Git 面板", icon: "git", shortcut: "Ctrl+Shift+G", onClick: () => toggleGitPanel(!state.gitViewVisible) },
      { separator: true },
      ...THEMES.map((t) => ({ label: `主题：${t}`, icon: document.documentElement.dataset.theme === t ? "check" : undefined, onClick: () => void applyTheme(t) })),
      { separator: true },
      { label: "全屏", icon: "expand", onClick: () => void (document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen()) },
    ]),
    mkMenu("Git", [
      { label: "比较（任意两端）…", icon: "diff", shortcut: "Ctrl+Shift+D", onClick: () => void openCompare() },
      { label: "刷新状态", icon: "refresh", onClick: () => void refreshGit().then(() => gitPanel?.refresh()) },
      { separator: true },
      { label: "提交…", icon: "check", onClick: () => {
        if (!state.gitViewVisible) toggleGitPanel(true)
        gitPanel?.show("changes")
      } },
      { label: "日志", icon: "history", onClick: () => {
        if (!state.gitViewVisible) toggleGitPanel(true)
        gitPanel?.show("log")
      } },
      { label: "分支", icon: "branch", onClick: () => {
        if (!state.gitViewVisible) toggleGitPanel(true)
        gitPanel?.show("branches")
      } },
      { separator: true },
      { label: "抓取 / 拉取 / 推送", icon: "sync", onClick: () => {
        if (!state.gitViewVisible) toggleGitPanel(true)
        gitPanel?.show("remotes")
      } },
    ]),
    mkMenu("帮助", [
      { label: "快捷键一览", icon: "info", onClick: () => showShortcuts() },
      { label: "服务端开关（GEBAI_FS_* / GEBAI_GIT_*）", icon: "settings", onClick: () => showEnvHelp() },
    ]),
    h("span", { class: "fw-grow" }),
  )
  const refresh = h("button", { class: "fw-icon-btn", title: "刷新根清单与 Git 状态" }, [icon("refresh", 14)])
  refresh.onclick = () => void loadRoots().then(() => explorer.refresh(""))
  const back = h("a", { class: "fw-btn sm", href: `${(import.meta.env.BASE_URL || "/").replace(/\/$/, "")}/`, title: "返回歌白主界面" }, [icon("back"), h("span", { text: "主界面" })])
  menubar.append(refresh, back)
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
  rightPanel.style.display = visible ? "" : "none"
  rightResizer.style.display = visible ? "" : "none"
  if (visible) {
    ensureGitPanel()
    void gitPanel?.refresh()
  }
  renderToolbar()
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
    ["Ctrl+Shift+G", "Git 面板"],
    ["Ctrl+Shift+D", "比较（任意两个提交 / 提交与工作区）"],
    ["F2", "重命名选中项"],
    ["Delete", "删除选中项（移入回收站）"],
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
  if (ctrl && e.shiftKey && e.key.toLowerCase() === "g") {
    e.preventDefault()
    toggleGitPanel(true)
    return
  }
  if (ctrl && e.shiftKey && e.key.toLowerCase() === "d") {
    e.preventDefault()
    void openCompare()
    return
  }
  if (ctrl && !e.shiftKey && e.key.toLowerCase() === "b") {
    e.preventDefault()
    const hidden = leftPanel.style.display === "none"
    leftPanel.style.display = hidden ? "" : "none"
    leftResizer.style.display = hidden ? "" : "none"
    return
  }
  if (e.key === "F5") {
    e.preventDefault()
    void explorer.refresh(undefined, { keepSelection: true }).then(() => refreshGit().then(() => gitPanel?.refresh()))
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
    for (const ed of document.querySelectorAll<HTMLElement>(".fw-editor-host")) void ed
  })
  window.addEventListener("mouseup", () => {
    if (!dragging) return
    dragging = false
    document.body.classList.remove("fw-resizing")
    window.dispatchEvent(new Event("resize"))
  })
}

/* ------------------------------ 启动 ------------------------------ */

async function boot(): Promise<void> {
  bootstrapTheme()
  // 启动遮罩：首屏（根清单 + 目录树 + 编辑器就绪）后淡出移除；异常也移除，不让遮罩卡住页面
  const splash = document.getElementById("gb-splash")
  const hideSplash = (): void => {
    if (!splash) return
    splash.classList.add("gb-splash-done")
    setTimeout(() => splash.remove(), 340)
  }
  try {
    buildMenubar()
    showLeftView("explorer")
    toggleGitPanel(state.gitViewVisible && window.innerWidth >= 1180)
    bindResizer(leftResizer, leftPanel, "left")
    bindResizer(rightResizer, rightPanel, "right")
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
    await refreshGit()
    renderTabbar()
    renderToolbar()
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
