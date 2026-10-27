/**
 * 文件工作台 · 资源管理器（左栏）：根选择、路径面包屑、懒加载目录树、Git 状态装饰、
 * 右键菜单（新建/重命名/复制/粘贴/删除/下载/上传/复制路径/在文件管理器中定位）、拖拽上传与拖拽移动。
 * 复制 / 粘贴走本模块自己持有的剪贴板（落点与命名规则见 `./clipboard-core`）：粘贴**不覆盖**同名条目，
 * 落成「xxx - 副本」。
 *
 * 状态：每个根的目录列表按 `rootId|path` 缓存（切换根不重复请求），展开集合按根隔离；
 * Git 装饰由 main.ts 传入的当前状态快照计算，避免树自己发请求。
 */
import type { DirEntry, FsApi, GitStatusInfo, RootInfo } from "./api"
import { h, icon, iconColorFor, showMenu, toast, formatSize, timeAgo, confirmDialog, promptDialog, clear, type MenuItem } from "./ui"
import { baseName, canPasteInto, parentDir, pickTargetPath, type ClipEntry } from "./clipboard-core"
import { buildRootSections, type RootMenuEntry } from "./root-menu"
import { dirsToRefresh } from "./watch-core"
import { createPreviewClick } from "./preview-click"
import { HIDDEN_INITIAL, toggled, withDefault, type HiddenState } from "./hidden-core"

export interface ExplorerHooks {
  api: FsApi
  roots: () => RootInfo[]
  rootsMeta: () => { writable: boolean; gitEnabled: boolean; sandboxed: boolean }
  /**
   * 打开文件（主区域标签页）。
   *
   * `preview` 缺省 = 真：复用同一个**预览标签**（斜体标题，单击树里的文件的语义）；
   * 传 `false` = 固定为常驻标签（双击条目、右键「打开」等明确动作）。
   * `only` = 先把预览槽里别的文件收掉（双击是「只要这一个」，见 main.ts 的 openFile）。
   */
  openFile: (root: string, path: string, opts?: { preview?: boolean; only?: boolean }) => void
  /** 当前活动文件（用于树高亮） */
  activeFile: () => { root: string; path: string } | null
  /** 当前根的 Git 状态（装饰用；非仓库返回 null） */
  gitStatus: () => GitStatusInfo | null
  /** 仓库根（用于把 Git 变更路径换算成树内路径） */
  repoPathPrefix: () => string
  /** 变更/上传等操作后通知外部刷新 Git 状态 */
  onFsChanged: () => void
  /** 根切换（main.ts 需要据此刷新 Git 面板） */
  onRootChanged: (rootId: string) => void
  /**
   * 树内导航（选中条目 / 定位跳转 / 换根）→ 宿主据此同步地址栏。
   * isDir 决定「值得记一条历史」（进目录）还是「就地替换」（同目录内换文件）。
   */
  onNavigate?: (path: string, isDir: boolean) => void
  /** 在文件管理器中显示（本地模式；桌面端能力，缺省不显示该项） */
  revealInOs?: (root: string, path: string) => void
  /** 在 Git 工具窗的日志栏按该文件过滤（宿主管工具窗的展开；缺省不显示该项） */
  openLogFilter?: (path: string) => void
  /** 展开集合变化（展开/折叠）：宿主据此把新的目录清单重新交给变更监听（重挂 watch）。 */
  onTreeChanged?: () => void
}

export interface Explorer {
  el: HTMLElement
  getRoot: () => string
  setRoot: (rootId: string, path?: string) => Promise<void>
  refresh: (path?: string, opts?: { keepSelection?: boolean }) => Promise<void>
  /** 仅回填 Git 装饰（徽标 + 状态类），不重建树——git 状态晚于首次渲染到达 */
  refreshGitDecorations: () => void
  /** 展开并选中目标路径（从搜索结果/标签页跳转） */
  reveal: (path: string, opts?: { select?: boolean }) => Promise<void>
  /** 当前根下已展开的目录（浅层在前）——变更监听端点按它决定要挂哪些 watch。 */
  expandedDirs: () => string[]
  /**
   * 变更事件驱动的增量刷新（watch 长轮询唤醒时调）：只重列**已缓存**的受影响目录，
   * 与缓存比对后只在**目录内容真的变了**时才重渲染那一块（内容修改不重建整树）。
   * `paths` 为根内相对变更路径；null = 未知/太多（重列根与展开目录）。
   */
  syncDirs: (paths: string[] | null) => Promise<void>
  /** 展开/收起「当前目录过滤」输入行（Ctrl+F）。 */
  toggleSearch: (open?: boolean) => void
  /** 套用服务端配置的默认值（GEBAI_FS_HIDDEN）：列出隐藏文件；用户手动切换过则不再覆盖。 */
  applyHiddenDefault: (on: boolean) => void
  selected: () => { path: string; type: DirEntry["type"] } | null
  /** 复制选中项进剪贴板（右键菜单与 Ctrl+C 共用）；返回是否真的复制了东西。 */
  copySelection: () => boolean
  /** 粘贴剪贴板条目：`dir` 缺省为当前选中目录（选中文件则取其父目录）。 */
  paste: (dir?: string) => Promise<void>
  /** 剪贴板条目（菜单文案与「粘贴」可用性判断用）。 */
  clipboard: () => ClipEntry | null
  /** 能否粘贴到当前根：条目在别的根、或当前只读时为 false。 */
  canPaste: () => boolean
  /** 资源管理器是否为当前活动区——键盘 Ctrl+C/V 的守卫（编辑器/输入框/终端内不接管，文本复制粘贴照旧）。 */
  isActive: () => boolean
  dispose: () => void
}

/** 长按判定：按住不动多久弹出条目菜单（触屏没有右键）。 */
const LONG_PRESS_MS = 520
/** 长按容差：按住期间的位移超过它即作废（滑动列表不该弹出菜单）。 */
const LONG_PRESS_MOVE = 10

/**
 * 粘贴的尝试次数：列目录与落盘之间可能被别人（Agent 正在写文件）插进同名条目，
 * 撞上就重列重试；仍撞则如实报错，不猜名字。
 */
const PLACE_ATTEMPTS = 3

export function createExplorer(hooks: ExplorerHooks): Explorer {
  const cache = new Map<string, DirEntry[]>()
  const expanded = new Map<string, Set<string>>()
  let rootId = ""
  let selectedPath: string | null = null
  let filterText = ""
  let sortKey: "name" | "mtime" | "size" | "type" = "name"
  let hidden: HiddenState = HIDDEN_INITIAL
  /** 复制 / 粘贴的剪贴板：只记「从哪个根的哪一条复制」，落盘发生在粘贴时。 */
  let clip: ClipEntry | null = null
  /** 资源管理器是不是当前活动区（键盘 Ctrl+C/V 的守卫，见 Explorer.isActive）。 */
  let treeActive = false

  const treeHost = h("div", { class: "fw-tree" })
  const filterInput = h("input", { class: "fw-input sm", placeholder: "按名称过滤（当前目录）", type: "search" })

  /** 头部小图标按钮（title 即提示；与标签栏动作区同款 .fw-icon-btn）。 */
  function headBtn(name: string, title: string, onClick: () => void, cls = ""): HTMLButtonElement {
    const b = h("button", { class: `fw-icon-btn sm ${cls}`.trim(), title })
    b.appendChild(icon(name, 14))
    b.onclick = onClick
    return b
  }

  const rootBtn = h("button", { class: "fw-root-btn" }, [icon("folderOpen"), h("span", { class: "fw-root-name", text: "选择根" }), icon("chevronDown")])
  const writableNow = () => hooks.rootsMeta().writable
  const moreBtn = headBtn("more", "更多操作（过滤 / 排序 / 隐藏文件 / 上传 / 折叠）", () => openMoreMenu(moreBtn))
  const searchRow = h("div", { class: "fw-explorer-search", hidden: true }, [filterInput, headBtn("close", "关闭过滤（Esc）", () => toggleSearch(false))])

  const el = h("div", { class: "fw-explorer" }, [
    h("div", { class: "fw-explorer-head" }, [
      rootBtn,
      // 只留刷新与「更多」：新建文件/文件夹是**低频**动作（建一次用完很久不碰），且树右键菜单里本就有
      // （选中在哪就在哪建，比头部按钮更准）；常驻两个「+」图标只是把头部挤窄，也让高频动作失去重点。
      h("div", { class: "fw-head-actions" }, [headBtn("refresh", "刷新（F5）", () => void refresh("")), moreBtn]),
    ]),
    searchRow,
    treeHost,
  ])

  function setSort(key: typeof sortKey): void {
    sortKey = key
    cache.clear()
    void refresh("", { keepSelection: true })
  }

  /** 当前「新建 / 上传」的目标目录：选中目录用目录本身，选中文件用其父目录，未选中用根目录。 */
  function selectedDir(): string {
    if (!selectedPath) return ""
    if (entryByPath.get(selectedPath)?.type === "dir") return selectedPath
    return selectedPath.includes("/") ? selectedPath.slice(0, selectedPath.lastIndexOf("/")) : ""
  }

  /** 折叠当前根的全部展开项。 */
  function collapseAll(): void {
    expanded.get(rootId)?.clear()
    hooks.onTreeChanged?.()
    render()
  }

  /** 「当前目录过滤」输入行：默认隐藏，Ctrl+F 或「更多」菜单展开；Esc / 关闭按钮收起并清空。 */
  function toggleSearch(open?: boolean): void {
    const next = open ?? searchRow.hidden
    searchRow.hidden = !next
    if (next) filterInput.focus()
    else if (filterText) {
      filterInput.value = ""
      filterText = ""
      render()
    }
  }

  /** 显示/隐藏隐藏文件：服务端按此过滤，缓存键不含该开关，故整体失效重取。 */
  function toggleHidden(): void {
    hidden = toggled(hidden)
    cache.clear()
    void refresh("", { keepSelection: true })
  }

  /**
   * 套用服务端配置的默认值（GEBAI_FS_HIDDEN，默认列出隐藏文件）：根清单到达时由宿主喂进来。
   * 用户手动切换过就不覆盖——「默认」只管首次；根还没选定时只记下状态，首次列举自然用新值。
   */
  function applyHiddenDefault(on: boolean): void {
    const next = withDefault(hidden, on)
    if (next === hidden) return
    hidden = next
    cache.clear()
    if (rootId) void refresh("", { keepSelection: true })
  }

  /** 头部「更多」菜单：过滤 / 排序 / 隐藏文件 / 上传 / 折叠全部收在一处，头部只留高频图标。  *
 * 新建文件/文件夹不在此（也不在头部）：它们是低频动作，且树右键菜单里本就有（选中在哪就在哪建，比头部更准）。
 */
function openMoreMenu(anchor: HTMLElement): void {
    const r = anchor.getBoundingClientRect()
    const mark = (on: boolean) => (on ? "✓ " : "")
    showMenu(r.left, r.bottom + 4, [
      { label: "在当前目录过滤…", icon: "search", shortcut: "Ctrl+F", onClick: () => toggleSearch(true) },
      { separator: true },
      { label: `${mark(sortKey === "name")}按名称排序`, icon: "file", onClick: () => setSort("name") },
      { label: `${mark(sortKey === "mtime")}按修改时间排序`, icon: "history", onClick: () => setSort("mtime") },
      { label: `${mark(sortKey === "size")}按大小排序`, icon: "archive", onClick: () => setSort("size") },
      { label: `${mark(sortKey === "type")}按类型排序`, icon: "diff", onClick: () => setSort("type") },
      { separator: true },
      { label: `${mark(hidden.on)}显示隐藏文件`, icon: "eye", onClick: () => toggleHidden() },
      { label: "上传文件…", icon: "upload", disabled: !writableNow(), onClick: () => pickAndUpload(selectedDir()) },
      { label: "折叠全部", icon: "collapseAll", onClick: () => collapseAll() },
    ])
  }

  function entriesOf(path: string): DirEntry[] | undefined {
    return cache.get(`${rootId}|${path}`)
  }

  async function loadDir(path: string): Promise<DirEntry[]> {
    const key = `${rootId}|${path}`
    const cached = cache.get(key)
    if (cached) return cached
    const res = await hooks.api.list(rootId, path, { showHidden: hidden.on, sort: sortKey })
    cache.set(key, res.entries)
    if (res.truncated) toast(`目录条目过多（共 ${res.total}），仅显示前 ${res.entries.length} 项`, "warn")
    return res.entries
  }

  /**
   * 预取一个目录的**直接子目录**（展开时调用）：下一次展开子目录时直接命中缓存，不再等一次往返。
   *
   * 边界：① 只取前 {@link MAX_PREFETCH} 个（大目录里几十个请求会与用户正在看的请求抢带宽，
   * 而用户一次只能展开一个）；② 后台标签页不预取（反正没人在看）；③ 并发 ≤4；
   * ④ 根切换后丢弃结果（`rid !== rootId`）；⑤ 失败静默——预取只是提速，正式展开会再试并报错。
   */
  const MAX_PREFETCH = 24
  function prefetchChildren(dir: string): void {
    if (document.hidden) return
    const entries = entriesOf(dir)
    if (!entries) return
    const rid = rootId
    const kids = entries.filter((e) => e.type === "dir" && !cache.has(`${rid}|${e.path}`)).slice(0, MAX_PREFETCH)
    if (!kids.length) return
    void (async () => {
      let i = 0
      const worker = async (): Promise<void> => {
        while (i < kids.length) {
          const kid = kids[i++]!
          if (rid !== rootId) return
          try {
            const res = await hooks.api.list(rid, kid.path, { showHidden: hidden.on, sort: sortKey })
            if (rid !== rootId) return
            cache.set(`${rid}|${kid.path}`, res.entries)
          } catch {
            /* 预取失败不打扰用户 */
          }
        }
      }
      await Promise.all(Array.from({ length: Math.min(4, kids.length) }, worker))
    })()
  }

  async function setRoot(id: string, path = ""): Promise<void> {
    previewClick.cancel() // 换根：上一个根里待开的预览已经没有意义
    rootId = id
    selectedPath = path || null
    const info = hooks.roots().find((r) => r.id === id)
    clear(rootBtn)
    rootBtn.append(icon("folderOpen"), h("span", { class: "fw-root-name", text: info ? info.name : id }), icon("chevronDown"))
    rootBtn.title = info ? `${info.name}\n${info.path}` : id
    hooks.onRootChanged(id)
    if (path) {
      // 先列根再定位：整棵树是从**根缓存**渲染的（`render()` 读 `entriesOf("")`），所以直接 `reveal(path)`
      // 会留下一个“根没列过”的状态——实测：深链接到**子目录里的文件**
      // （`?root=…&path=imgproc/main.cpp`）时左栏一直停在「加载中…」，而顶层文件（`dir` 为空）不触发。
      try {
        await loadDir("")
      } catch (err) {
        clear(treeHost)
        treeHost.appendChild(h("div", { class: "fw-error", text: (err as Error).message }))
        return
      }
      await reveal(path)
      return
    }
    await refresh("")
    hooks.onNavigate?.("", true)
  }

  /* --------------------------- 树渲染 --------------------------- */

  /** 行索引：path → 行元素 / path → 条目。选中态切换、装饰刷新、reveal 定位都靠它，
   * 省掉每次交互遍历整棵树（早期 `querySelectorAll(".fw-tree-row")` 是 O(行数)）。 */
  const rowByPath = new Map<string, HTMLElement>()
  const entryByPath = new Map<string, DirEntry>()

  /** 行上的 Git 状态类全集（刷新前先清除，避免旧状态残留）。 */
  const DECO_CLASSES = ["conflict", "untracked", "added", "deleted", "renamed", "staged", "modified", "child", "ignored"] as const

  interface Deco {
    mark: string
    cls: string
    title: string
  }

  /** 「子项有变更」的目录装饰（中性灰点）。 */
  const CHILD_DECO: Deco = { mark: "•", cls: "child", title: "该目录下有未提交变更" }

  /**
   * Git 装饰查询表 + 指纹。
   *
   * 为什么要建表：装饰是**按行**算的，早期实现每行都要在 `status.changes` 上做两轮线性扫描
   * （精确命中 + 子项命中）—— 2000 行树 × 500 条变更 = 每次刷新百万级字符串比较，而且
   * **渲染路径上就会调它**（每次展开目录/点文件/刷新状态都要重跑一次）。改成一次 O(变更数) 建表：
   * `files` 是路径精确命中，`dirs` 是“有变更的祖先目录”集合，行内只剩 Map/Set 查询。
   * 指纹（仓库前缀 + 每条变更的路径/类型/三态）未变则直接复用上次的表。
   */
  let decoTable: { fp: string; files: Map<string, Deco>; dirs: Set<string> } | null = null

  function decoFor(): { files: Map<string, Deco>; dirs: Set<string> } {
    const status = hooks.gitStatus()
    const prefix = hooks.repoPathPrefix()
    const parts: string[] = []
    if (status?.isRepo) {
      for (const c of status.changes) parts.push(`${c.path}\u0001${c.kind}\u0001${c.staged ? 1 : 0}${c.unstaged ? 1 : 0}${c.untracked ? 1 : 0}${c.conflicted ? 1 : 0}`)
    }
    const fp = `${status?.isRepo ? "1" : "0"}\u0002${prefix}\u0002${parts.join("\u0003")}`
    if (decoTable?.fp === fp) return decoTable
    const files = new Map<string, Deco>()
    const dirs = new Set<string>()
    if (status?.isRepo) {
      for (const c of status.changes) {
        const repoRel = prefix ? (c.path.startsWith(prefix) ? c.path.slice(prefix.length + 1) : "") : c.path
        if (!repoRel) continue
        if (!files.has(repoRel)) {
          const mark = c.conflicted ? "!" : c.untracked ? "U" : c.kind === "added" ? "A" : c.kind === "deleted" ? "D" : c.kind === "renamed" ? "R" : c.staged && !c.unstaged ? "S" : "M"
          const cls = c.conflicted ? "conflict" : c.untracked ? "untracked" : c.kind === "deleted" ? "deleted" : c.staged && !c.unstaged ? "staged" : "modified"
          files.set(repoRel, { mark, cls, title: `Git: ${c.kind}${c.staged ? "（已暂存）" : ""}` })
        }
        // 该变更的各级祖先目录都算「子项有变更」（与旧实现里 c.path.startsWith(dir + "/") 等价）
        for (let i = repoRel.lastIndexOf("/"); i > 0; i = repoRel.lastIndexOf("/", i - 1)) dirs.add(repoRel.slice(0, i))
      }
    }
    decoTable = { fp, files, dirs }
    return decoTable
  }

  /** 一行的装饰（非仓库 / 仓库外 / 根行 → null）。 */
  function decoOf(path: string, isDir: boolean): Deco | null {
    const status = hooks.gitStatus()
    if (!status?.isRepo) return null
    const prefix = hooks.repoPathPrefix()
    const repoRel = prefix ? (path.startsWith(prefix) ? path.slice(prefix.length + 1) : "") : path
    if (!repoRel) return null
    const table = decoFor()
    return table.files.get(repoRel) ?? (isDir && table.dirs.has(repoRel) ? CHILD_DECO : null)
  }

  /**
   * 就地把 Git 装饰应用到一行（徽标 + 状态类）；**装饰未变则完全不碰 DOM**。
   *
   * 为什么不重渲染整树：树的展开状态、滚动位置、选中项都在 DOM 里，
   * 每次 git 状态变化就重建会「折叠回去 + 滚动跳顶」。这里只换装饰元素。
   */
  function applyDecoration(row: HTMLElement): void {
    const path = row.dataset.path ?? ""
    const isDir = row.classList.contains("dir")
    const deco = decoOf(path, isDir)
    const key = deco ? `${deco.cls}\u0001${deco.title}` : ""
    if (row.dataset.deco === key) return
    row.dataset.deco = key
    for (const c of DECO_CLASSES) row.classList.remove(`git-${c}`)
    if (deco) row.classList.add(`git-${deco.cls}`)
    const old = row.querySelector(".fw-git-mark, .fw-git-mark-gap")
    const node = deco
      ? h("span", { class: `fw-git-mark ${deco.cls}`, text: deco.mark, title: deco.title })
      : h("span", { class: "fw-git-mark-gap" })
    if (old) old.replaceWith(node)
    else row.appendChild(node)
  }

  /** Git 装饰刷新（git 状态到达/变化后由宿主调用——状态到达晚于首次渲染，必须回填）。 */
  function refreshGitDecorations(): void {
    for (const row of rowByPath.values()) applyDecoration(row)
  }

  /**
   * 「单击 = 预览、双击 = 固定」的时序判定（纯逻辑在 `files/preview-click.ts`，带单测）。
   *
   * 为什么要延后：双击的第一发与单击在事件层一模一样，而“开预览”在标签层是**就位替换**（唯一预览槽
   * 里的文件会被顶掉，见 main.ts 的 openFile）——立即打开的话，双击 B 就变成“第一发把上一个预览 A
   * 顶掉 → dblclick 把 B 钉住”，A 已经默默没了。这一层把预览推到双击窗口之后落地，窗口内等到
   * dblclick 就作废（预览那一发连请求都不会发）。
   */
  const PREVIEW_DELAY = 220
  const previewClick = createPreviewClick({
    openPreview: (t) => hooks.openFile(t.root, t.path, { preview: true }),
    // only：双击是“只要这一个”——把预览槽里那个无关的文件收掉（见 main.ts 的 openFile）
    openPinned: (t) => hooks.openFile(t.root, t.path, { preview: false, only: true }),
    // 等待期间这一行可能已被重画（增量刷新换了元素）——行还在文档里才开，否则这次单击已经无效
    isAlive: (t) => (t.el as HTMLElement | undefined)?.isConnected ?? true,
    delay: PREVIEW_DELAY,
  })

  function renderEntry(entry: DirEntry, depth: number): HTMLElement {
    const isDir = entry.type === "dir"
    const exp = isDir && (expanded.get(rootId)?.has(entry.path) ?? false)
    /** 长按已弹出菜单：尾随的那次 click 不该再打开文件/目录 */
    let suppressNextClick = false
    // 选中/活动态不在建行时写死：统一由 refreshSelection() 落位（见那里为何）
    const row = h("div", {
      class: `fw-tree-row ${isDir ? "dir" : "file"}`,
      "data-path": entry.path,
      "data-depth": depth,
      draggable: "true",
    })
    row.style.paddingLeft = `${6 + depth * 13}px`
    const twisty = isDir
      ? (() => {
          const b = h("button", { class: "fw-twisty", title: exp ? "折叠" : "展开" }, [icon(exp ? "chevronDown" : "chevronRight", 12)])
          b.onclick = (e) => {
            e.stopPropagation()
            toggleDir(entry.path)
          }
          return b
        })()
      : h("span", { class: "fw-twisty-empty" })
    const ic = h("span", { class: `fw-file-icon c-${iconColorFor(entry.name, entry.type)}` }, [icon(isDir ? (exp ? "folderOpen" : "folder") : "file")])
    row.append(twisty, ic, h("span", { class: "fw-tree-name", text: entry.name, title: entry.path }))
    // 装饰统一经 applyDecoration 落位（与刷新路径同源，避免两处逻辑漂移）
    applyDecoration(row)
    row.onclick = () => {
      // 长按已弹出菜单：这一次 click 是长按的尾随事件，不再打开文件
      if (suppressNextClick) {
        suppressNextClick = false
        return
      }
      selectedPath = entry.path
      refreshSelection()
      // 单击文件 = 预览标签（斜体、会被下一个预览复用）；双击 = 固定（见下面的 ondblclick）。
      //
      // 预览的打开**延后一拍**（时序逻辑见 `preview-click.ts`）：双击在标签层是“就位替换”（唯一预览槽
      // 里的文件会被新文件顶掉），而双击的第一发与单击长得一样——立即打开会把上一个预览默默顶掉。
      // 延后到双击窗口之后，双击就是“作废预览那一发 + 开一个常驻”这一个结果。
      if (!isDir) previewClick.click({ key: `${rootId}|${entry.path}`, root: rootId, path: entry.path, el: row })
      // 地址栏同步：进目录记一条历史（可后退），点文件就地替换
      hooks.onNavigate?.(entry.path, isDir)
    }
    row.ondblclick = () => {
      // 文件：固定为常驻标签（第一发的预览若还没落地，在这里一并作废，只按固定打开一次）
      if (isDir) toggleDir(entry.path, true)
      else previewClick.dblclick({ key: `${rootId}|${entry.path}`, root: rootId, path: entry.path })
    }
    row.oncontextmenu = (e) => {
      e.preventDefault()
      selectedPath = entry.path
      refreshSelection()
      openEntryMenu(e.clientX, e.clientY, entry)
    }
    /*
     * 长按 = 打开条目菜单（触屏没有右键，而原生右键菜单整站屏蔽）。
     * 只在非鼠标指针上启用：鼠标右键已走 oncontextmenu，长按没有额外含义。
     * 手指滑动（滚动列表）会先收到 pointercancel，指针移开超过阈值也主动作废——
     * 两者都只是撤销定时器，不 preventDefault，滚动体验不变。
     */
    let press: { pid: number; x: number; y: number; timer: number } | null = null
    const cancelPress = (): void => {
      if (!press) return
      clearTimeout(press.timer)
      press = null
    }
    /** 按住结束/作废：连同恢复 HTML5 拖放开关（触屏按下期间临时关掉它）。 */
    const endPress = (): void => {
      cancelPress()
      row.draggable = true
    }
    row.addEventListener("pointerdown", (e) => {
      if (e.pointerType === "mouse") return
      // 触屏上浏览器的原生长按拖拽会与长按弹菜单争夺同一个手势，按住期间先关掉它
      row.draggable = false
      const x = e.clientX
      const y = e.clientY
      press = {
        pid: e.pointerId,
        x,
        y,
        timer: window.setTimeout(() => {
          press = null
          suppressNextClick = true
          selectedPath = entry.path
          refreshSelection()
          openEntryMenu(x, y, entry)
          navigator.vibrate?.(10)
        }, LONG_PRESS_MS),
      }
    })
    row.addEventListener("pointermove", (e) => {
      if (!press || e.pointerId !== press.pid) return
      if (Math.abs(e.clientX - press.x) > LONG_PRESS_MOVE || Math.abs(e.clientY - press.y) > LONG_PRESS_MOVE) endPress()
    })
    row.addEventListener("pointerup", endPress)
    row.addEventListener("pointercancel", endPress)
    row.addEventListener("pointerleave", endPress)
    row.ondragstart = (e) => {
      e.dataTransfer?.setData("text/x-gebai-path", entry.path)
      e.dataTransfer?.setData("text/plain", entry.path)
      if (e.dataTransfer) e.dataTransfer.effectAllowed = "move"
    }
    if (isDir) {
      row.ondragover = (e) => {
        if (e.dataTransfer?.types.includes("Files") || e.dataTransfer?.types.includes("text/x-gebai-path")) {
          e.preventDefault()
          row.classList.add("drop-target")
        }
      }
      row.ondragleave = () => row.classList.remove("drop-target")
      row.ondrop = (e) => {
        e.preventDefault()
        row.classList.remove("drop-target")
        void handleDrop(e, entry.path)
      }
    }
    rowByPath.set(entry.path, row)
    entryByPath.set(entry.path, entry)
    return row
  }

  function renderChildren(container: HTMLElement | DocumentFragment, path: string, depth: number): void {
    const entries = entriesOf(path)
    if (!entries) return
    const needle = depth === 0 && filterText ? filterText.toLowerCase() : ""
    for (const e of entries) {
      if (needle && !e.name.toLowerCase().includes(needle)) continue
      container.appendChild(renderEntry(e, depth))
      if (e.type === "dir" && expanded.get(rootId)?.has(e.path)) {
        renderChildren(container, e.path, depth + 1)
      }
    }
  }

  let lastSelectedRow: HTMLElement | null = null
  let lastActiveRow: HTMLElement | null = null

  /** 整树重建（换根/刷新/排序/过滤/展开失败等需要重排整树的场合；日常展开收起走局部增删）。 */
  function render(): void {
    rowByPath.clear()
    entryByPath.clear()
    lastSelectedRow = null
    lastActiveRow = null
    const entries = entriesOf("")
    if (!entries) {
      treeHost.replaceChildren(h("div", { class: "fw-loading", text: "加载中…" }))
      return
    }
    // 先在片段上拼好整棵树再一次性提交：逐条 appendChild 到活动 DOM 会催生 N 次布局
    const frag = document.createDocumentFragment()
    if (!entries.length) frag.appendChild(h("div", { class: "fw-empty", text: "空目录" }))
    renderChildren(frag, "", 0)
    // 过滤后顶层一条都没命中：给个明确空态（否则是一块纯空白面板，看着像还没加载出来）
    if (entries.length && filterText && !frag.querySelector(".fw-tree-row")) {
      frag.appendChild(h("div", { class: "fw-empty", text: `当前目录无匹配「${filterText}」的条目` }))
    }
    treeHost.replaceChildren(frag)
    refreshSelection()
  }

  /** 目录行之后、属于它子树的连续行（扁平渲染下「深度大于本行」的行恰好就是它的子树）。 */
  function subtreeRows(dirRow: HTMLElement): HTMLElement[] {
    const depth = Number(dirRow.dataset.depth ?? 0)
    const out: HTMLElement[] = []
    for (let n = dirRow.nextElementSibling as HTMLElement | null; n && n.classList.contains("fw-tree-row") && Number(n.dataset.depth ?? 0) > depth; n = n.nextElementSibling as HTMLElement | null) {
      out.push(n)
    }
    return out
  }

  /** 目录行的展开态：只换 twisty 图标与提示（CSS 靠图标区分开合，行本身不重建）。 */
  function setDirOpen(row: HTMLElement, open: boolean): void {
    const t = row.querySelector(".fw-twisty")
    if (!t) return
    t.setAttribute("title", open ? "折叠" : "展开")
    t.replaceChildren(icon(open ? "chevronDown" : "chevronRight", 12))
  }

  /**
   * 展开 / 收起目录。
   *
   * 早期实现无论开合都 `render()` 整树重建——于是在大目录里「展开一个子目录」也要重建
   * 已展开的全部行（含每行的图标与命令闭包），这是树上最贵的操作。现在：
   * 收起 = 删掉该行之后的子树行；展开 = 只把新子树插到该行之后，其余行（含滚动位置、
   * 选中项、已有编辑器的行）原封不动。只有目标行已不在（发生过重建）时才回退到整树重建。
   */
  async function toggleDir(path: string, forceOpen = false): Promise<void> {
    const set = expanded.get(rootId) ?? new Set<string>()
    expanded.set(rootId, set)
    const row = rowByPath.get(path)
    if (set.has(path) && !forceOpen) {
      set.delete(path)
      hooks.onTreeChanged?.()
      if (!row) {
        render()
        return
      }
      for (const r of subtreeRows(row)) {
        rowByPath.delete(r.dataset.path ?? "")
        entryByPath.delete(r.dataset.path ?? "")
        r.remove()
      }
      setDirOpen(row, false)
      return
    }
    set.add(path)
    hooks.onTreeChanged?.()
    try {
      await loadDir(path)
    } catch (err) {
      toast(`无法展开：${(err as Error).message}`, "error")
      set.delete(path)
      return
    }
    // 展开成功即预取下一层（下次展开子目录时命中缓存，不用等往返）
    prefetchChildren(path)
    const dirRow = rowByPath.get(path)
    if (dirRow && !subtreeRows(dirRow).length) {
      const frag = document.createDocumentFragment()
      renderChildren(frag, path, Number(dirRow.dataset.depth ?? 0) + 1)
      dirRow.after(frag)
      setDirOpen(dirRow, true)
      refreshSelection()
      return
    }
    render()
  }

  /**
   * 选中/活动态刷新：只动「上一个 / 当前」两行。
   * 早期实现每次点击/右键都遍历全部行做 `classList.toggle`（大树上每次点击 ~O(行数)），
   * 而且活动态只在整树重建时更新（切标签后高亮会滞后）。
   */
  function refreshSelection(): void {
    const selRow = selectedPath ? rowByPath.get(selectedPath) ?? null : null
    if (selRow !== lastSelectedRow) {
      lastSelectedRow?.classList.remove("selected")
      selRow?.classList.add("selected")
      lastSelectedRow = selRow
    }
    const act = hooks.activeFile()
    const actRow = act && act.root === rootId ? rowByPath.get(act.path) ?? null : null
    if (actRow !== lastActiveRow) {
      lastActiveRow?.classList.remove("active")
      actRow?.classList.add("active")
      lastActiveRow = actRow
    }
  }

  /* --------------------------- 拖拽 --------------------------- */

  async function handleDrop(e: DragEvent, targetDir: string): Promise<void> {
    const internal = e.dataTransfer?.getData("text/x-gebai-path")
    if (internal) {
      if (internal === targetDir || targetDir.startsWith(`${internal}/`)) {
        toast("不能移动到自身或其子目录", "error")
        return
      }
      const name = internal.split("/").pop() ?? ""
      const dest = targetDir ? `${targetDir}/${name}` : name
      try {
        await hooks.api.move(rootId, internal, dest, false)
        toast(`已移动 ${name}`, "success")
        invalidate(internal, targetDir)
        hooks.onFsChanged()
      } catch (err) {
        const msg = (err as Error).message
        if (msg.includes("已存在")) {
          const ok = await confirmDialog({ title: "目标已存在", message: `「${dest}」已存在，是否覆盖？`, okText: "覆盖", danger: true })
          if (ok) {
            await hooks.api.move(rootId, internal, dest, true).then(() => {
              toast("已覆盖移动", "success")
              invalidate(internal, targetDir)
              hooks.onFsChanged()
            })
          }
        } else toast(`移动失败：${msg}`, "error")
      }
      return
    }
    const files = Array.from(e.dataTransfer?.files ?? [])
    if (!files.length) return
    await uploadFiles(files, targetDir)
  }

  function invalidate(...paths: string[]): void {
    for (const p of paths) {
      cache.delete(`${rootId}|${p}`)
      const parent = p.includes("/") ? p.slice(0, p.lastIndexOf("/")) : ""
      cache.delete(`${rootId}|${parent}`)
    }
    void refresh("")
  }

  async function uploadFiles(files: File[], dir: string): Promise<void> {
    if (!hooks.rootsMeta().writable) {
      toast("当前为只读模式（GEBAI_FS_WRITE=false）", "error")
      return
    }
    // 目录上传：webkitRelativePath 形如 "folder/sub/file.txt"，保留相对结构
    const payload = files.map((f) => {
      const rel = (f as File & { webkitRelativePath?: string }).webkitRelativePath
      const name = rel && rel.includes("/") ? rel : f.name
      return { file: f, path: dir ? `${dir}/${name}` : name }
    })
    try {
      const res = await hooks.api.upload(rootId, payload, false)
      const skipped = res.skipped ?? []
      if (skipped.length) {
        const ok = await confirmDialog({ title: "存在同名文件", message: `${skipped.length} 个文件已存在，是否覆盖？\n${skipped.slice(0, 8).join("\n")}`, okText: "覆盖", danger: true })
        if (ok) {
          const retry = payload.filter((p) => skipped.includes(p.path))
          const res2 = await hooks.api.upload(rootId, retry, true)
          toast(`已上传 ${res2.saved.length} 个文件`, "success")
        }
      }
      if (res.saved.length) toast(`已上传 ${res.saved.length} 个文件`, "success")
      cache.clear()
      await refresh("")
      hooks.onFsChanged()
    } catch (err) {
      toast(`上传失败：${(err as Error).message}`, "error")
    }
  }

  /* --------------------------- 右键菜单 --------------------------- */

  function openEntryMenu(x: number, y: number, entry?: DirEntry): void {
    const meta = hooks.rootsMeta()
    const writable = meta.writable
    const isDir = entry?.type === "dir"
    const targetDir = isDir ? entry?.path ?? "" : entry ? (entry.path.includes("/") ? entry.path.slice(0, entry.path.lastIndexOf("/")) : "") : ""
    const items = []
    if (entry) {
      items.push(
        { label: isDir ? "展开/折叠" : "打开", icon: "eye", onClick: () => (isDir ? void toggleDir(entry.path) : hooks.openFile(rootId, entry.path)) },
        { label: "下载", icon: "download", onClick: () => window.open(hooks.api.url("/api/v1/fs/download", { root: rootId, path: entry.path }), "_blank") },
        { separator: true },
        { label: "复制路径", icon: "copy", onClick: () => void navigator.clipboard.writeText(entry.path).then(() => toast("已复制相对路径", "success")) },
      )
      const abs = (() => {
        const root = hooks.roots().find((r) => r.id === rootId)
        return root ? `${root.path.replace(/[\\/]+$/, "")}/${entry.path}` : entry.path
      })()
      items.push({ label: "复制绝对路径", icon: "copy", onClick: () => void navigator.clipboard.writeText(abs).then(() => toast("已复制绝对路径", "success")) })
      if (hooks.revealInOs) items.push({ label: "在文件管理器中显示", icon: "expand", onClick: () => hooks.revealInOs?.(rootId, entry.path) })
      items.push(
        { separator: true },
        { label: "复制", icon: "copy", shortcut: "Ctrl+C", onClick: () => void copyEntryToClipboard({ path: entry.path, isDir }) },
        { label: "复制到…", icon: "expand", disabled: !writable, onClick: () => void doCopyTo(entry.path) },
      )
      // 「粘贴」只给目录行：目录才是「贴进去」的落点，文件行的落点是它的父目录（走空白处菜单）
      if (isDir) items.push(pasteItem(entry.path))
      items.push(
        { label: "重命名…", icon: "edit", shortcut: "F2", disabled: !writable, onClick: () => void doRename(entry.path) },
        { label: "移动到…", icon: "expand", disabled: !writable, onClick: () => void doMove(entry.path) },
        { label: "删除", icon: "trash", shortcut: "Del", danger: true, disabled: !writable, onClick: () => void doDelete([entry.path], entry.path) },
        { separator: true },
        { label: "新建文件…", icon: "plus", disabled: !writable, onClick: () => void doNewFile(targetDir) },
        { label: "新建文件夹…", icon: "plus", disabled: !writable, onClick: () => void doNewDir(targetDir) },
        { label: "上传文件…", icon: "upload", disabled: !writable, onClick: () => pickAndUpload(targetDir) },
      )
      if (meta.gitEnabled) {
        const repos = hooks.roots().find((r) => r.id === rootId)?.isRepo
        if (repos) {
          items.push({ separator: true })
          // 只对文件给出：目录的历史过滤语义是“该目录下的提交”，与「单文件历史」不是一回事
          if (!isDir && hooks.openLogFilter) items.push({ label: "在 Git 日志中筛选该文件", icon: "history", onClick: () => hooks.openLogFilter?.(entry.path) })
          items.push({ label: "忽略此条目（.gitignore）", icon: "git", disabled: !writable, onClick: () => void doIgnore(entry.path, isDir) })
        }
      }
    } else {
      items.push(
        pasteItem(""),
        { separator: true },
        { label: "新建文件…", icon: "plus", disabled: !writable, onClick: () => void doNewFile("") },
        { label: "新建文件夹…", icon: "plus", disabled: !writable, onClick: () => void doNewDir("") },
        { label: "上传文件…", icon: "upload", disabled: !writable, onClick: () => pickAndUpload("") },
        { separator: true },
        { label: "刷新", icon: "refresh", onClick: () => void refresh("") },
      )
    }
    showMenu(x, y, items)
  }

  function pickAndUpload(dir: string): void {
    const input = document.createElement("input")
    input.type = "file"
    input.multiple = true
    input.onchange = () => {
      const files = Array.from(input.files ?? [])
      if (files.length) void uploadFiles(files, dir)
    }
    input.click()
  }

  async function doNewFile(dir: string): Promise<void> {
    const name = await promptDialog({ title: "新建文件", label: "文件名", placeholder: "例如 index.ts", value: dir ? `${dir}/` : "" })
    if (!name?.trim()) return
    const path = name.trim()
    try {
      await hooks.api.write(rootId, path, { content: "", createDirs: true })
      toast("已创建文件", "success")
      invalidate(path)
      hooks.openFile(rootId, path)
    } catch (err) {
      toast(`创建失败：${(err as Error).message}`, "error")
    }
  }

  async function doNewDir(dir: string): Promise<void> {
    const name = await promptDialog({ title: "新建文件夹", label: "文件夹名", value: dir ? `${dir}/` : "" })
    if (!name?.trim()) return
    try {
      await hooks.api.mkdir(rootId, name.trim())
      toast("已创建文件夹", "success")
      invalidate(name.trim())
    } catch (err) {
      toast(`创建失败：${(err as Error).message}`, "error")
    }
  }

  async function doRename(path: string): Promise<void> {
    const oldName = path.split("/").pop() ?? path
    const name = await promptDialog({ title: "重命名", label: "新名称", value: oldName })
    if (!name?.trim() || name === oldName) return
    try {
      const res = await hooks.api.rename(rootId, path, name.trim())
      toast("已重命名", "success")
      invalidate(path, res.path)
      hooks.onFsChanged()
    } catch (err) {
      toast(`重命名失败：${(err as Error).message}`, "error")
    }
  }

  /**
   * 移动到目标目录（触屏没有 HTML5 拖放，移动这件事必须在菜单里有一条路）。
   * 目标按根内相对路径填写，父目录不存在时不自动创建——写错路径比默默建一串目录好查。
   */
  async function doMove(path: string): Promise<void> {
    const name = path.split("/").pop() ?? path
    const curDir = path.includes("/") ? path.slice(0, path.lastIndexOf("/")) : ""
    const dir = await promptDialog({
      title: "移动到…",
      label: "目标目录（相对当前根；留空表示根目录）",
      placeholder: "例如 src/components",
      value: curDir,
    })
    if (dir === null) return
    const to = `${dir.trim().replace(/^\/+|\/+$/g, "")}${dir.trim() ? "/" : ""}${name}`
    if (to === path) return
    if (to.startsWith(`${path}/`)) {
      toast("不能移动到自身或其子目录", "error")
      return
    }
    try {
      const res = await hooks.api.move(rootId, path, to)
      toast(`已移动到 ${res.path}`, "success")
      invalidate(path, res.path)
      hooks.onFsChanged()
    } catch (err) {
      toast(`移动失败：${(err as Error).message}`, "error")
    }
  }

  /* --------------------------- 复制 / 粘贴 --------------------------- */

  /**
   * 复制（右键 / Ctrl+C）：只记进本页剪贴板，落盘发生在粘贴时。
   * 不写系统剪贴板——那里能装的是路径文本（「复制路径」已有该入口），文件内容跨不进浏览器。
   */
  function copyEntryToClipboard(entry: { path: string; isDir: boolean }): void {
    clip = { root: rootId, path: entry.path, isDir: entry.isDir }
    toast(`已复制「${baseName(entry.path)}」，到目标目录粘贴即可`, "success")
  }

  /** 「粘贴」菜单项：条目在别的根时把话说在前面（不给一个点了才报错的入口）。 */
  function pasteItem(dir: string): MenuItem {
    const label = !clip ? "粘贴" : clip.root === rootId ? `粘贴「${baseName(clip.path)}」` : "粘贴（跨根不支持）"
    return { label, icon: "paste", shortcut: "Ctrl+V", disabled: !canPasteInto(clip, rootId, writableNow()), onClick: () => void paste(dir) }
  }

  /** 目标目录现有条目名（含隐藏项：`.env` 这类同名条目若没列进来，会被当成空位而白撞一次）。 */
  async function namesIn(dir: string): Promise<string[]> {
    const res = await hooks.api.list(rootId, dir, { showHidden: true, sort: "name" })
    return res.entries.map((e) => e.name)
  }

  /** 落盘后刷新目标目录：已展开就只重画那一块（整树重建会把展开态与滚动位置推倒）。 */
  async function reloadDir(dir: string): Promise<void> {
    cache.delete(`${rootId}|${dir}`)
    try {
      await loadDir(dir)
    } catch {
      void refresh("")
      return
    }
    if (dir === "") render()
    else if (rowByPath.has(dir)) replaceDirChildren(dir)
  }

  /**
   * 把条目复制到目标目录。
   *
   * **不覆盖**：撞名就落成「xxx - 副本」（与系统文件管理器的粘贴同一语义）——粘贴是本页最高频的写操作，
   * 每次都弹一次「是否覆盖」比偶尔多出一个副本更烦人，而覆盖是不可逆的。
   */
  async function placeEntry(src: ClipEntry, dir: string): Promise<void> {
    if (!hooks.rootsMeta().writable) {
      toast("当前为只读模式（GEBAI_FS_WRITE=false）", "error")
      return
    }
    if (src.root !== rootId) {
      toast("跨根复制暂不支持：请切到条目所在的根内粘贴", "error")
      return
    }
    const name = baseName(src.path)
    if (src.isDir && (dir === src.path || dir.startsWith(`${src.path}/`))) {
      toast("不能粘贴到自身或其子目录", "error")
      return
    }
    for (let attempt = 1; attempt <= PLACE_ATTEMPTS; attempt++) {
      let taken: string[]
      try {
        taken = await namesIn(dir)
      } catch (err) {
        toast(`粘贴失败：${(err as Error).message}`, "error")
        return
      }
      const target = pickTargetPath(dir, name, src.isDir, taken)
      if (!target) {
        toast("粘贴失败：目标目录下同名副本过多", "error")
        return
      }
      try {
        await hooks.api.copy(rootId, src.path, target.path, false)
        toast(target.renamed ? `已粘贴为「${baseName(target.path)}」（目标已有同名项）` : `已粘贴「${baseName(target.path)}」`, "success")
        await reloadDir(dir)
        hooks.onFsChanged()
        return
      } catch (err) {
        const msg = (err as Error).message
        // 撞名 = 刚才那次列举已经过时（别人刚写进同名条目）：重列后重试
        if (attempt < PLACE_ATTEMPTS && msg.includes("已存在")) continue
        toast(`粘贴失败：${msg}`, "error")
        return
      }
    }
  }

  async function paste(dir?: string): Promise<void> {
    if (!clip) return
    await placeEntry(clip, dir ?? selectedDir())
  }

  /** 复制到指定目录（一次即成，不进剪贴板）——与「移动到…」对称的入口。 */
  async function doCopyTo(path: string): Promise<void> {
    const isDir = entryByPath.get(path)?.type === "dir"
    const dir = await promptDialog({
      title: "复制到…",
      label: "目标目录（相对当前根；留空表示根目录）",
      placeholder: "例如 src/components",
      value: parentDir(path),
      hint: "同名时自动改名「xxx - 副本」，不覆盖已有文件。",
    })
    if (dir === null) return
    await placeEntry({ root: rootId, path, isDir }, dir.trim().replace(/^\/+|\/+$/g, ""))
  }

  async function doDelete(paths: string[], label: string): Promise<void> {
    const ok = await confirmDialog({
      title: "删除确认",
      message: `确定删除「${label}」？`,
      hint: "文件将从磁盘永久删除，不可恢复。",
      okText: "永久删除",
      danger: true,
    })
    if (!ok) return
    try {
      const res = await hooks.api.del(rootId, paths)
      toast(`已删除 ${res.deleted} 项`, "success")
      for (const p of paths) invalidate(p)
      hooks.onFsChanged()
    } catch (err) {
      toast(`删除失败：${(err as Error).message}`, "error")
    }
  }

  async function doIgnore(path: string, isDir: boolean): Promise<void> {
    const entry = isDir ? `${path}/` : path
    try {
      await hooks.api.gitOp("ignore", rootId, { entries: [entry] })
      toast(`已加入 .gitignore：${entry}`, "success")
      hooks.onFsChanged()
    } catch (err) {
      toast(`忽略失败：${(err as Error).message}`, "error")
    }
  }

  /* --------------------------- 根选择 --------------------------- */

  rootBtn.onclick = () => {
    const r = rootBtn.getBoundingClientRect()
    const sections = buildRootSections(hooks.roots(), rootId)
    /** 一项的菜单形态（分组/折叠只决定“摆在哪”，渲染统一走这里）。 */
    const asItem = (x: RootMenuEntry) => ({
      label: `${x.name}${x.isRepo ? `  ⑂${x.branch ?? ""}` : ""}`,
      icon: x.kind === "sess" ? "history" : "folder",
      onClick: () => void setRoot(x.id),
    })
    const items: Array<Record<string, unknown>> = []
    for (const g of sections) {
      // 空标题 = 不渲染分组行（歌白自身直接露出，再顶一行不可点的分类标题与它下面那项重复）
      if (g.title) items.push({ label: g.title, disabled: true })
      for (const x of g.entries) items.push(asItem(x))
      // 会话组的其余项收进子菜单：hover 才展开，不让菜单一开就是几十条。
      // 不传 icon：带 submenu 的项本来就会在右端出一个右箭头（再传一个会变成两个）。
      if (g.more) items.push({ label: g.more.label, submenu: g.more.entries.map(asItem) })
      items.push({ separator: true })
    }
    if (!hooks.rootsMeta().sandboxed) {
      items.push({
        label: "打开任意文件夹…",
        icon: "folderOpen",
        onClick: () =>
          void (async () => {
            const p = await promptDialog({ title: "打开文件夹", label: "绝对路径", placeholder: "/workspaces/gebai 或 D:\\project", hint: "本地模式可直接访问本机任意目录；服务模式仅限已授权根。" })
            if (!p?.trim()) return
            const id = `abs:${p.trim().replace(/[\\/]+$/, "")}`
            const exist = hooks.roots().find((r) => r.id === id)
            if (!exist) {
              // 动态加入根清单（仅当前会话前端持有；服务端会二次校验路径存在性）
              ;(hooks.roots() as RootInfo[]).push({ id, kind: "abs", name: p.trim().split(/[\\/]/).pop() || p.trim(), path: p.trim(), writable: true })
            }
            await setRoot(id)
          })(),
      })
    }
    showMenu(r.left, r.bottom + 4, items as never)
  }

  // 过滤输入防抖：每敲一个字符就整树重建（整树 = 全量行重建）会直接把输入拖卡
  let filterTimer: number | null = null
  filterInput.oninput = () => {
    if (filterTimer !== null) window.clearTimeout(filterTimer)
    filterTimer = window.setTimeout(() => {
      filterTimer = null
      filterText = filterInput.value.trim()
      render()
    }, 140)
  }
  filterInput.onkeydown = (e) => {
    if (e.key === "Escape") {
      e.stopPropagation()
      toggleSearch(false)
    }
  }

  treeHost.oncontextmenu = (e) => {
    if (e.target === treeHost) {
      e.preventDefault()
      selectedPath = null
      refreshSelection()
      openEntryMenu(e.clientX, e.clientY)
    }
  }
  treeHost.ondragover = (e) => {
    if (e.dataTransfer?.types.includes("Files")) {
      e.preventDefault()
      treeHost.classList.add("drop-root")
    }
  }
  treeHost.ondragleave = () => treeHost.classList.remove("drop-root")
  treeHost.ondrop = (e) => {
    if (e.target === treeHost) {
      e.preventDefault()
      treeHost.classList.remove("drop-root")
      void handleDrop(e, "")
    }
  }

  // 活动区跟踪：键盘 Ctrl+C/V 只在「刚在树里操作过」时接管——焦点在编辑器/终端/输入框里时，
  // 那两个键是文本复制粘贴，不能让文件剪贴板抢走
  el.addEventListener("pointerdown", () => {
    treeActive = true
  })
  const onDocPointerDown = (e: Event): void => {
    if (!el.contains(e.target as Node)) treeActive = false
  }
  document.addEventListener("pointerdown", onDocPointerDown)

  /** 两份目录列举是否一致（只比影响列表显示的字段：名字 / 类型；大小与时间只影响 tooltip，不值得为它重建行）。 */
  function sameEntries(a: DirEntry[], b: DirEntry[]): boolean {
    if (a.length !== b.length) return false
    for (let i = 0; i < a.length; i++) {
      const x = a[i]!
      const y = b[i]!
      if (x.name !== y.name || x.path !== y.path || x.type !== y.type) return false
    }
    return true
  }

  /**
   * 「未知变更」一轮最多重列多少个目录（兜底轮询路径）。
   * 事件驱动的路径不需要上限：那是「真的变了」的精确清单，条数天然有限。
   */
  const SYNC_ALL_CAP = 24

  /**
   * 变更事件驱动的增量刷新（watch 长轮询唤醒时由宿主调用）。
   *
   * 为什么不直接重建整树：外部写入（Agent 边跑边改文件）很密，整树重建会把展开态之外的
   * 一切（滚动位置、已渲染的行、选中态）都推倒重来。这里的顺序是：
   * ① 只处理**已缓存**的目录（没展示过的目录不感兴趣）；② 重新列举这些目录；
   * ③ 与缓存比对，**列表真的变了**（新增/删除/改名）才换缓存并重渲染那一块；
   * ④ 只是内容修改时什么都不重建（行的名字/图标没变，改动本身由 Git 装饰与变更面板表达）。
   */
  async function syncDirs(paths: string[] | null): Promise<void> {
    const rid = rootId
    if (!rid) return
    const set = expanded.get(rid) ?? new Set<string>()
    const candidates = new Set<string>()
    if (paths === null) {
      // 未知 / 太多（或监听被关、退化为纯轮询）：重列根与已展开的目录。
      // 浅层优先并夹到 SYNC_ALL_CAP：这条路径每次兜底轮询都会走一遍，不能让它变成「几十个请求一发」。
      candidates.add("")
      const open = [...set].sort((a, b) => a.split("/").length - b.split("/").length || a.localeCompare(b))
      for (const d of open.slice(0, SYNC_ALL_CAP)) candidates.add(d)
      // 折叠着的目录（缓存还在但不显示）：直接作废缓存——零请求，下次展开时自然取新的
      for (const key of [...cache.keys()]) {
        if (!key.startsWith(`${rid}|`)) continue
        const dir = key.slice(rid.length + 1)
        if (dir === "" || set.has(dir)) continue
        cache.delete(key)
      }
    } else {
      for (const d of dirsToRefresh(paths)) candidates.add(d)
    }
    let full = false
    for (const dir of candidates) {
      // 不展开的目录其列表并不显示：**作废缓存而不是重列**——重列是把用户已离开的目录又拉一遍（白花请求），
      // 作废则零成本，且保证「折叠期间变过、之后展开」看到的不是旧列表
      if (dir !== "" && !set.has(dir)) {
        cache.delete(`${rid}|${dir}`)
        continue
      }
      const key = `${rid}|${dir}`
      const cached = cache.get(key)
      if (!cached) continue
      let next: DirEntry[]
      try {
        next = (await hooks.api.list(rid, dir, { showHidden: hidden.on, sort: sortKey })).entries
      } catch {
        continue // 目录被删/被关权限：下一次展开会给明确错误，这里静默跳过
      }
      if (rid !== rootId) return // 中途换根：整轮结果作废
      if (sameEntries(cached, next)) continue
      cache.set(key, next)
      if (dir === "") full = true
      else replaceDirChildren(dir)
    }
    if (full) render()
  }

  /** 局部重渲染一个已展开目录的子树（行不存在/已脱离视图时不做任何事）。 */
  function replaceDirChildren(dir: string): void {
    const row = rowByPath.get(dir)
    if (!row) return
    for (const r of subtreeRows(row)) {
      rowByPath.delete(r.dataset.path ?? "")
      entryByPath.delete(r.dataset.path ?? "")
      r.remove()
    }
    const frag = document.createDocumentFragment()
    renderChildren(frag, dir, Number(row.dataset.depth ?? 0) + 1)
    row.after(frag)
    setDirOpen(row, true)
    refreshSelection()
  }

  /* --------------------------- 公共接口 --------------------------- */

  async function refresh(path = "", opts: { keepSelection?: boolean } = {}): Promise<void> {
    if (!opts.keepSelection) selectedPath = null
    cache.delete(`${rootId}|${path}`)
    try {
      await loadDir(path)
    } catch (err) {
      clear(treeHost)
      treeHost.appendChild(h("div", { class: "fw-error", text: (err as Error).message }))
      return
    }
    render()
    // 根目录总是展开的：它的下一层在空闲时预取（启动期先让真实首屏请求跑完，不与它抢带宽）
    if (!path) {
      const idle = (window as unknown as { requestIdleCallback?: (cb: () => void, o?: { timeout: number }) => number }).requestIdleCallback
      if (idle) idle(() => prefetchChildren(""), { timeout: 2000 })
      else window.setTimeout(() => prefetchChildren(""), 600)
    }
  }

  async function reveal(path: string, opts: { select?: boolean } = {}): Promise<void> {
    const parts = path.split("/").filter(Boolean)
    let acc = ""
    const set = expanded.get(rootId) ?? new Set<string>()
    expanded.set(rootId, set)
    // 展开集合或选中项真的变了、或树里还没有这一行时才重建整树：
    // 早期无条件 render()，于是每次切标签（activate → reveal）都重建一次整树
    let changed = !rowByPath.has(path)
    for (let i = 0; i < parts.length - 1; i++) {
      acc = acc ? `${acc}/${parts[i]}` : parts[i]
      if (!set.has(acc)) changed = true
      set.add(acc)
      try {
        await loadDir(acc)
      } catch {
        break
      }
    }
    if (opts.select !== false && selectedPath !== path) {
      selectedPath = path
      changed = true
    }
    if (changed) {
      render()
    } else {
      refreshSelection()
    }
    // 定位到的目录同样预取下一层（面包屑/从搜索结果跳进来后接着往下展开是常规动作）
    if (acc) prefetchChildren(acc)
    rowByPath.get(path)?.scrollIntoView({ block: "nearest" })
    // 定位跳转（面包屑 / 深层链接 / 前进后退）同样要同步地址栏
    hooks.onNavigate?.(path, parts.length === 0 ? true : !path.includes("."))
  }

  return {
    el,
    getRoot: () => rootId,
    setRoot,
    refresh,
    refreshGitDecorations,
    syncDirs,
    expandedDirs: () => [...(expanded.get(rootId) ?? [])].sort((a, b) => a.split("/").length - b.split("/").length || a.localeCompare(b)),
    reveal,
    toggleSearch,
    applyHiddenDefault,
    selected: () => (selectedPath ? { path: selectedPath, type: (entriesOf(selectedPath.includes("/") ? selectedPath.slice(0, selectedPath.lastIndexOf("/")) : "")?.find((x) => x.path === selectedPath)?.type ?? "file") as DirEntry["type"] } : null),
    copySelection: () => {
      if (!selectedPath) return false
      copyEntryToClipboard({ path: selectedPath, isDir: entryByPath.get(selectedPath)?.type === "dir" })
      return true
    },
    paste,
    clipboard: () => clip,
    canPaste: () => canPasteInto(clip, rootId, writableNow()),
    isActive: () => treeActive,
    dispose: () => {
      cache.clear()
      rowByPath.clear()
      entryByPath.clear()
      decoTable = null
      document.removeEventListener("pointerdown", onDocPointerDown)
      if (filterTimer !== null) window.clearTimeout(filterTimer)
    },
  }
}

/** 格式化条目信息（状态栏/悬浮提示复用）。 */
export function entryInfo(entry: DirEntry): string {
  return `${entry.type === "dir" ? "目录" : formatSize(entry.size)} · ${timeAgo(entry.mtime)}`
}
