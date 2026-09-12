/**
 * 文件工作台 · 资源管理器（左栏）：根选择、路径面包屑、懒加载目录树、Git 状态装饰、
 * 右键菜单（新建/重命名/删除/下载/上传/复制路径/在文件管理器中定位）、拖拽上传与拖拽移动。
 *
 * 状态：每个根的目录列表按 `rootId|path` 缓存（切换根不重复请求），展开集合按根隔离；
 * Git 装饰由 main.ts 传入的当前状态快照计算，避免树自己发请求。
 */
import type { DirEntry, FsApi, GitStatusInfo, RootInfo } from "./api"
import { h, icon, iconColorFor, showMenu, toast, formatSize, timeAgo, confirmDialog, promptDialog, clear } from "./ui"

export interface ExplorerHooks {
  api: FsApi
  roots: () => RootInfo[]
  rootsMeta: () => { writable: boolean; gitEnabled: boolean; sandboxed: boolean; showHidden: boolean }
  /** 打开文件（主区域标签页） */
  openFile: (root: string, path: string) => void
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
  selected: () => { path: string; type: DirEntry["type"] } | null
  dispose: () => void
}

export function createExplorer(hooks: ExplorerHooks): Explorer {
  const cache = new Map<string, DirEntry[]>()
  const expanded = new Map<string, Set<string>>()
  let rootId = ""
  let selectedPath: string | null = null
  let filterText = ""
  let sortKey: "name" | "mtime" | "size" | "type" = "name"
  let showHidden = false

  const treeHost = h("div", { class: "fw-tree" })
  const crumbHost = h("div", { class: "fw-crumbs" })
  const filterInput = h("input", { class: "fw-input sm", placeholder: "按名称过滤（当前目录）", type: "search" })
  const rootBtn = h("button", { class: "fw-root-btn" }, [icon("folderOpen"), h("span", { class: "fw-root-name", text: "选择根" }), icon("chevronDown")])

  const el = h("div", { class: "fw-explorer" }, [
    h("div", { class: "fw-explorer-head" }, [rootBtn]),
    h("div", { class: "fw-explorer-tools" }, [
      filterInput,
      (() => {
        const b = h("button", { class: "fw-icon-btn", title: "排序（名称/时间/大小/类型）" }, [icon("settings")])
        b.onclick = () => {
          const r = b.getBoundingClientRect()
          showMenu(r.left, r.bottom + 4, [
            { label: "名称", icon: "file", onClick: () => setSort("name") },
            { label: "修改时间", icon: "history", onClick: () => setSort("mtime") },
            { label: "大小", icon: "archive", onClick: () => setSort("size") },
            { label: "类型", icon: "diff", onClick: () => setSort("type") },
          ])
        }
        return b
      })(),
      (() => {
        const b = h("button", { class: "fw-icon-btn", title: "显示/隐藏隐藏文件" }, [icon("eye")])
        b.onclick = () => {
          showHidden = !showHidden
          b.classList.toggle("active", showHidden)
          void refresh("")
        }
        return b
      })(),
      (() => {
        const b = h("button", { class: "fw-icon-btn", title: "刷新" }, [icon("refresh")])
        b.onclick = () => void refresh("")
        return b
      })(),
    ]),
    crumbHost,
    treeHost,
  ])

  function setSort(key: typeof sortKey): void {
    sortKey = key
    cache.clear()
    void refresh("")
  }

  function entriesOf(path: string): DirEntry[] | undefined {
    return cache.get(`${rootId}|${path}`)
  }

  async function loadDir(path: string): Promise<DirEntry[]> {
    const key = `${rootId}|${path}`
    const cached = cache.get(key)
    if (cached) return cached
    const res = await hooks.api.list(rootId, path, { showHidden, sort: sortKey })
    cache.set(key, res.entries)
    if (res.truncated) toast(`目录条目过多（共 ${res.total}），仅显示前 ${res.entries.length} 项`, "warn")
    return res.entries
  }

  async function setRoot(id: string, path = ""): Promise<void> {
    rootId = id
    selectedPath = path || null
    const info = hooks.roots().find((r) => r.id === id)
    clear(rootBtn)
    rootBtn.append(icon("folderOpen"), h("span", { class: "fw-root-name", text: info ? info.name : id }), icon("chevronDown"))
    rootBtn.title = info ? `${info.name}\n${info.path}` : id
    hooks.onRootChanged(id)
      if (path) {
    await reveal(path)
    return
  }
  await refresh("")
  hooks.onNavigate?.("", true)
}

  function updateCrumbs(): void {
    clear(crumbHost)
    const info = hooks.roots().find((r) => r.id === rootId)
    const parts = (selectedPath ?? "").split("/").filter(Boolean)
    const rootCrumb = h("button", { class: "fw-crumb" }, [icon("folderOpen"), h("span", { text: info?.name ?? rootId })])
    rootCrumb.onclick = () => void refresh("")
    crumbHost.appendChild(rootCrumb)
    let acc = ""
    for (const p of parts) {
      acc = acc ? `${acc}/${p}` : p
      const target = acc
      const seg = h("button", { class: "fw-crumb" }, [icon("chevronRight"), h("span", { text: p })])
      seg.onclick = () => void reveal(target)
      crumbHost.appendChild(seg)
    }
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

  function renderEntry(entry: DirEntry, depth: number): HTMLElement {
    const isDir = entry.type === "dir"
    const exp = isDir && (expanded.get(rootId)?.has(entry.path) ?? false)
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
      selectedPath = entry.path
      updateCrumbs()
      refreshSelection()
      if (!isDir) hooks.openFile(rootId, entry.path)
      // 地址栏同步：进目录记一条历史（可后退），点文件就地替换
      hooks.onNavigate?.(entry.path, isDir)
    }
    row.ondblclick = () => {
      if (isDir) toggleDir(entry.path, true)
    }
    row.oncontextmenu = (e) => {
      e.preventDefault()
      selectedPath = entry.path
      refreshSelection()
      openEntryMenu(e.clientX, e.clientY, entry)
    }
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
    updateCrumbs()
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
    try {
      await loadDir(path)
    } catch (err) {
      toast(`无法展开：${(err as Error).message}`, "error")
      set.delete(path)
      return
    }
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
        { label: "重命名…", icon: "edit", shortcut: "F2", disabled: !writable, onClick: () => void doRename(entry.path) },
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

  async function doDelete(paths: string[], label: string): Promise<void> {
    const ok = await confirmDialog({
      title: "删除确认",
      message: `确定删除「${label}」？`,
      hint: "文件将移入回收站（可在「文件 → 回收站」恢复），不会立即从磁盘抹除。",
      okText: "移入回收站",
      danger: true,
    })
    if (!ok) return
    try {
      const res = await hooks.api.del(rootId, paths)
      toast(`已移入回收站（${res.trashed} 项）`, "success")
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
    const roots = hooks.roots()
    const groups: Array<{ label: string; kind: string }> = [
      { label: "会话工作区", kind: "sess" },
      { label: "预置项目", kind: "proj" },
      { label: "绑定项目", kind: "bind" },
      { label: "其它", kind: "other" },
    ]
    const items: Array<Record<string, unknown>> = []
    for (const g of groups) {
      const list = roots.filter((x) => (g.kind === "other" ? !["sess", "proj", "bind"].includes(x.kind) : x.kind === g.kind))
      if (!list.length) continue
      items.push({ label: g.label.split("").join(""), disabled: true })
      for (const x of list) {
        items.push({
          label: `${x.name}${x.isRepo ? `  ⑂${x.branch ?? ""}` : ""}`,
          icon: x.kind === "sess" ? "history" : "folder",
          onClick: () => void setRoot(x.id),
        })
      }
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

  treeHost.oncontextmenu = (e) => {
    if (e.target === treeHost) {
      e.preventDefault()
      selectedPath = null
      updateCrumbs()
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
      updateCrumbs()
    }
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
    reveal,
    selected: () => (selectedPath ? { path: selectedPath, type: (entriesOf(selectedPath.includes("/") ? selectedPath.slice(0, selectedPath.lastIndexOf("/")) : "")?.find((x) => x.path === selectedPath)?.type ?? "file") as DirEntry["type"] } : null),
    dispose: () => {
      cache.clear()
      rowByPath.clear()
      entryByPath.clear()
      decoTable = null
      if (filterTimer !== null) window.clearTimeout(filterTimer)
    },
  }
}

/** 格式化条目信息（状态栏/悬浮提示复用）。 */
export function entryInfo(entry: DirEntry): string {
  return `${entry.type === "dir" ? "目录" : formatSize(entry.size)} · ${timeAgo(entry.mtime)}`
}
