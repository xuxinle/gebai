/**
 * 文件工作台 · 比较视图（DESIGN「文件工作台·Git 图形化」核心理念的一等公民）：
 * **任意两个提交之间、任意提交与工作树（或暂存区）之间**的文件级对比。
 *
 * 端点模型（与服务端 `core/git/service.ts` 的 diffArgs 语义表一一对应）：
 *   `WORKTREE`（工作区） | `INDEX`（暂存区） | 任意 rev（分支名 / 标签 / 短 hash / `HEAD~3` / `main@{1}` / `A^`）
 * 组合覆盖：
 *   · 提交 ↔ 提交        `git diff A B`
 *   · 提交 ↔ 工作树      `git diff A`      （「这个提交之后我又改了什么」——最常查）
 *   · 提交 ↔ 暂存区      `git diff --cached A`
 *   · 分支 ↔ 分支（共同祖先，三点）  `git diff A...B`
 *   · 工作树 ↔ 暂存区    `git diff`        （未暂存改动）
 * 每个文件都能点开为 Monaco 并列差异（两侧真实文本，而非只看 patch 文本）。
 */
import type { FsApi, GitBranchInfo, GitFileDiff, GitTagInfo } from "./api"
import { h, icon, clear, toast, showMenu, formatTime, timeAgo } from "./ui"

/** 端点值（与后端保留字一致）。 */
export const WORKTREE = "WORKTREE"
export const INDEX = "INDEX"

export interface RefOption {
  /** git 可解析的端点值（回传给服务端） */
  value: string
  label: string
  /** 分组：工作区 / 本地分支 / 远程分支 / 标签 / 最近提交 / HEAD */
  group: string
  detail?: string
  time?: number
}

export interface CompareState {
  from: string
  to: string
  mergeBase: boolean
  path: string
  ignoreWhitespace: boolean
}

export interface CompareHooks {
  api: FsApi
  root: () => string
  /** 打开某文件在两端之间的差异标签 */
  openDiff: (spec: {
    title: string
    root: string
    path: string
    source: { type: "range"; from: string; to: string; mergeBase?: boolean; label?: string }
  }) => void
  /** 打开某端点下的文件（工作树端点才可打开） */
  openFile: (root: string, path: string) => void
  onFsChanged: () => void
}

/** 端点展示名（供 UI 与标签标题使用）。 */
export function refLabel(ref: string, opts: { head?: { branch?: string; hash?: string }; branches?: GitBranchInfo[]; tags?: GitTagInfo[] } = {}): string {
    const up = ref.toUpperCase()
  if (!ref || up === WORKTREE) return "工作区"
  if (up === INDEX) return "暂存区"
  if (ref === "HEAD") return opts.head?.branch ? `HEAD（${opts.head.branch}）` : "HEAD"
  const b = opts.branches?.find((x) => x.name === ref)
  if (b) return `${b.name}${b.current ? "（当前）" : ""}`
  const t = opts.tags?.find((x) => x.name === ref)
  if (t) return `标签 ${t.name}`
  return ref.length >= 8 && /^[0-9a-f]+$/i.test(ref) ? ref.slice(0, 8) : ref
}

/** 端点选择器对话框（左侧 A / 右侧 B，含工作区与暂存区入口）。 */
export function pickEndpoint(title: string, refs: { branches: GitBranchInfo[]; tags: GitTagInfo[]; recent: Array<{ hash: string; short: string; subject: string; author: string; time: number }>; head: { branch?: string; hash?: string } }, current: string): Promise<string | null> {
  return new Promise((resolve) => {
    const overlay = h("div", { class: "fw-overlay" })
    const list = h("div", { class: "fw-ref-list" })
    const search = h("input", { class: "fw-input", placeholder: "搜索分支 / 标签 / 提交信息，或直接粘贴 rev（如 HEAD~2、v1.2.0、a1b2c3d）" })
    let value = current
    const confirmBtn = h("button", { class: "fw-btn primary", text: "选择" })
    const cancelBtn = h("button", { class: "fw-btn", text: "取消" })
    const done = (v: string | null) => {
      overlay.remove()
      document.removeEventListener("keydown", onKey, true)
      resolve(v)
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.stopPropagation()
        done(null)
      } else if (e.key === "Enter" && !(e.target instanceof HTMLInputElement && list.contains(e.target))) {
        // 输入框内 Enter 用于确认粘贴的 rev；列表内 Enter 由行的 dblclick 承担
        e.stopPropagation()
        done(value)
      }
    }
    document.addEventListener("keydown", onKey, true)
    const renderList = () => {
      clear(list)
      const q = search.value.trim().toLowerCase()
      const match = (s: string) => !q || s.toLowerCase().includes(q)
      const groups: Array<{ label: string; items: Array<{ value: string; label: string; detail?: string; time?: number }> }> = [
        {
          label: "工作副本",
          items: [
            { value: WORKTREE, label: "工作区（Working Tree）", detail: "磁盘上的当前内容" },
            { value: INDEX, label: "暂存区（Index / Staged）", detail: "已 git add 的内容" },
          ],
        },
        {
          label: "引用",
          items: [
            { value: "HEAD", label: "HEAD", detail: refs.head.branch ?? refs.head.hash?.slice(0, 8) ?? "" },
            ...(refs.head.branch ? [{ value: refs.head.branch, label: refs.head.branch, detail: "当前分支" }] : []),
          ],
        },
        {
          label: "本地分支",
          items: refs.branches.filter((b) => !b.remote).map((b) => ({ value: b.name, label: b.name, detail: b.subject ?? "", time: b.time })),
        },
        {
          label: "远程分支",
          items: refs.branches.filter((b) => b.remote).map((b) => ({ value: b.name, label: b.name, detail: b.subject ?? "", time: b.time })),
        },
        { label: "标签", items: refs.tags.map((t) => ({ value: t.name, label: t.name, detail: t.subject ?? "", time: t.time })) },
        {
          label: "最近提交",
          items: refs.recent.map((c) => ({ value: c.hash, label: `${c.short}  ${c.subject}`, detail: c.author, time: c.time })),
        },
      ]
      for (const g of groups) {
        const items = g.items.filter((x) => match(x.label) || match(x.detail ?? "") || match(x.value))
        if (!items.length) continue
        list.appendChild(h("div", { class: "fw-ref-group", text: g.label }))
        for (const it of items) {
          const row = h("button", { class: `fw-ref-row${it.value === value ? " active" : ""}` }, [
            icon(it.value === WORKTREE ? "file" : it.value === INDEX ? "archive" : "branch", 13),
            h("span", { class: "fw-ref-label", text: it.label }),
            it.detail ? h("span", { class: "fw-ref-detail", text: it.detail }) : null,
            it.time ? h("span", { class: "fw-ref-time", text: timeAgo(it.time) }) : null,
          ])
          row.onclick = () => {
            value = it.value
            for (const r of list.querySelectorAll(".fw-ref-row")) r.classList.remove("active")
            row.classList.add("active")
          }
          row.ondblclick = () => done(it.value)
          list.appendChild(row)
        }
      }
      if (!list.children.length) list.appendChild(h("div", { class: "fw-empty", text: "没有匹配的引用" }))
    }
    search.oninput = renderList
    confirmBtn.onclick = () => done(search.value.trim() && /^[\w./@^~\-{}]+$/.test(search.value.trim()) && !list.querySelector(".fw-ref-row.active") ? search.value.trim() : value)
    cancelBtn.onclick = () => done(null)
    const dialog = h("div", { class: "fw-dialog wide" }, [
      h("div", { class: "fw-dialog-title" }, [icon("diff"), h("span", { text: title })]),
      h("div", { class: "fw-dialog-body" }, [search, list]),
      h("div", { class: "fw-dialog-actions" }, [cancelBtn, confirmBtn]),
    ])
    overlay.appendChild(dialog)
    overlay.onclick = (e) => {
      if (e.target === overlay) done(null)
    }
    document.body.appendChild(overlay)
    setTimeout(() => search.focus(), 20)
    renderList()
  })
}

export interface CompareView {
  el: HTMLElement
  setState: (patch: Partial<CompareState>) => void
  refresh: () => Promise<void>
  state: () => CompareState
}

/** 比较视图主体（嵌入主区域的「比较」标签页）。 */
/** 目录限定：root 只是仓库子目录时，比较默认只看该子目录（可点击 chip 清除看整仓库）。 */
export interface CompareInit extends Partial<CompareState> {
  basePath?: string
}

export function createCompareView(hooks: CompareHooks, initial: CompareInit = {}): CompareView {
  const basePath = initial.basePath ?? ""
  const state: CompareState = {
    from: initial.from ?? "HEAD",
    to: initial.to ?? WORKTREE,
    mergeBase: initial.mergeBase ?? false,
    // 默认带上 root 在仓库内的相对前缀（git pathspec 语义），避免会话工作区根下看到整仓库差异
    path: initial.path ?? basePath,
    ignoreWhitespace: initial.ignoreWhitespace ?? false,
  }
  type RecentRef = { hash: string; short: string; subject: string; author: string; time: number; refs: string[] }
  type RefBundle = { branches: GitBranchInfo[]; tags: GitTagInfo[]; recent: RecentRef[]; head: { branch?: string; hash?: string } }
  let refs: RefBundle = { branches: [], tags: [], recent: [], head: {} }
  let files: GitFileDiff[] = []
  let additions = 0
  let deletions = 0
  let mergeBaseOf: string | undefined
  let loading = false

  const fromBtn = h("button", { class: "fw-ref-btn" })
  const toBtn = h("button", { class: "fw-ref-btn" })
  const swapBtn = h("button", { class: "fw-icon-btn", title: "交换左右端点" })
  swapBtn.appendChild(icon("sync"))
  const mergeBaseToggle = h("label", { class: "fw-check" }, [h("input", { type: "checkbox" }), h("span", { text: "按共同祖先（…）" })])
  const wsToggle = h("label", { class: "fw-check" }, [h("input", { type: "checkbox" }), h("span", { text: "忽略空白" })])
  const pathInput = h("input", { class: "fw-input sm", placeholder: "限单文件/目录（可选，如 src/main.ts）" })
  const summary = h("span", { class: "fw-compare-summary" })
  const listHost = h("div", { class: "fw-compare-list" })
  const head = h("div", { class: "fw-compare-head" }, [
    h("div", { class: "fw-compare-row" }, [
      h("span", { class: "fw-compare-caption", text: "基准（A）" }),
      fromBtn,
      swapBtn,
      h("span", { class: "fw-compare-caption", text: "对比（B）" }),
      toBtn,
    ]),
    h("div", { class: "fw-compare-row" }, [
      mergeBaseToggle,
      wsToggle,
      pathInput,
      h("span", { class: "fw-grow" }),
      summary,
      (() => {
        const b = h("button", { class: "fw-btn sm" }, [icon("refresh"), h("span", { text: "重新比较" })])
        b.onclick = () => void refresh()
        return b
      })(),
      (() => {
        const b = h("button", { class: "fw-btn sm" }, [icon("download"), h("span", { text: "导出补丁" })])
        b.onclick = () => {
          const url = hooks.api.url("/api/v1/git/diff", { root: hooks.root(), from: state.from, to: state.to, mergeBase: state.mergeBase, path: state.path || undefined })
          void fetch(url)
            .then((r) => r.json())
            .then((res: { raw?: string }) => {
              const blob = new Blob([res.raw ?? ""], { type: "text/x-patch" })
              const a = document.createElement("a")
              a.href = URL.createObjectURL(blob)
              a.download = `${refLabel(state.from, refs)}..${refLabel(state.to, refs)}.patch`.replace(/[\\/:*?"<>|]/g, "_")
              a.click()
              URL.revokeObjectURL(a.href)
            })
        }
        return b
      })(),
    ]),
  ])
  const el = h("div", { class: "fw-compare" }, [head, listHost])

  function syncControls(): void {
    fromBtn.replaceChildren(icon("file"), h("span", { text: refLabel(state.from, refs) }), icon("chevronDown"))
    toBtn.replaceChildren(icon("file"), h("span", { text: refLabel(state.to, refs) }), icon("chevronDown"))
    fromBtn.title = `基准端点：${state.from}`
    toBtn.title = `对比端点：${state.to}`
    ;(mergeBaseToggle.querySelector("input") as HTMLInputElement).checked = state.mergeBase
    ;(wsToggle.querySelector("input") as HTMLInputElement).checked = state.ignoreWhitespace
    if (pathInput.value !== state.path) pathInput.value = state.path
  }

  fromBtn.onclick = () =>
    void pickEndpoint("选择基准端点（A）", refs, state.from).then((v) => {
      if (v) {
        state.from = v
        syncControls()
        void refresh()
      }
    })
  toBtn.onclick = () =>
    void pickEndpoint("选择对比端点（B）", refs, state.to).then((v) => {
      if (v) {
        state.to = v
        syncControls()
        void refresh()
      }
    })
  swapBtn.onclick = () => {
    const t = state.from
    state.from = state.to
    state.to = t
    syncControls()
    void refresh()
  }
  ;(mergeBaseToggle.querySelector("input") as HTMLInputElement).onchange = (e) => {
    state.mergeBase = (e.target as HTMLInputElement).checked
    void refresh()
  }
  ;(wsToggle.querySelector("input") as HTMLInputElement).onchange = (e) => {
    state.ignoreWhitespace = (e.target as HTMLInputElement).checked
    void refresh()
  }
  pathInput.onkeydown = (e) => {
    if (e.key === "Enter") {
      state.path = pathInput.value.trim()
      void refresh()
    }
  }

  async function loadRefs(): Promise<void> {
    try {
      const res = await hooks.api.gitRefs(hooks.root(), 50)
      refs = { branches: res.branches, tags: res.tags, recent: res.recent as RecentRef[], head: res.head }
      if (state.from === "HEAD" && res.head.branch) state.from = "HEAD"
    } catch (err) {
      toast(`读取引用失败：${(err as Error).message}`, "error")
    }
  }

  async function refresh(): Promise<void> {
    if (loading) return
    loading = true
    listHost.replaceChildren(h("div", { class: "fw-loading", text: "比较中…" }))
    try {
      const res = await hooks.api.gitCompare(hooks.root(), {
        from: state.from,
        to: state.to,
        mergeBase: state.mergeBase,
        path: state.path || undefined,
        ignoreWhitespace: state.ignoreWhitespace,
      })
      files = res.files
      additions = res.additions
      deletions = res.deletions
      mergeBaseOf = res.mergeBaseOf
      renderList()
    } catch (err) {
      listHost.replaceChildren(h("div", { class: "fw-error", text: `比较失败：${(err as Error).message}` }))
      summary.textContent = ""
    } finally {
      loading = false
    }
  }

  function statusMark(f: GitFileDiff): { mark: string; cls: string } {
    const s = f.status
    if (s === "added") return { mark: "A", cls: "A" }
    if (s === "deleted") return { mark: "D", cls: "D" }
    if (s === "renamed") return { mark: "R", cls: "R" }
    if (s === "copied") return { mark: "C", cls: "R" }
    if (s === "typechange") return { mark: "T", cls: "M" }
    return { mark: "M", cls: "M" }
  }

  /** 目录限定 chip：清楚展示「当前比较范围」并一键切换。 */
  function scopeChip(): HTMLElement | null {
    if (!basePath) return null
    const limited = state.path === basePath
    const chip = h("button", { class: "fw-chip", title: limited ? basePath : "整仓库" }, [
      icon(limited ? "folder" : "git", 12),
      h("span", { text: limited ? `仅当前目录：${basePath.split("/").pop() ?? basePath}` : "整仓库" }),
    ])
    chip.onclick = () => {
      state.path = limited ? "" : basePath
      pathInput.value = state.path
      void refresh()
    }
    return chip
  }

  function renderList(): void {
    const a = refLabel(state.from, refs)
    const b = refLabel(state.to, refs)
    const summaryNodes: Array<Node | null> = [
      h("span", { class: "fw-info", text: `${a} → ${b}` }),
      scopeChip(),
      h("span", { class: "fw-diff-stat add", text: `+${additions}` }),
      h("span", { class: "fw-diff-stat del", text: `-${deletions}` }),
      h("span", { class: "fw-hint", text: `${files.length} 个文件` }),
    ]
    if (mergeBaseOf) summaryNodes.splice(1, 0, h("span", { class: "fw-ref-chip", text: `共同祖先 ${mergeBaseOf.slice(0, 8)}`, title: `git diff ${state.from}...${state.to}` }))
    summary.replaceChildren(...(summaryNodes.filter(Boolean) as Node[]))
    clear(listHost)
    if (!files.length) {
      // 限定到子目录时无差异：给出「整仓库有差异」的引导（避免用户误以为没改动）
      if (basePath && state.path) {
        const hint = h("div", { class: "fw-placeholder" }, [
          h("div", { class: "fw-placeholder-msg", text: `当前目录范围内两端一致（${a} → ${b}）` }),
          h("div", { class: "fw-placeholder-hint", text: "仓库其它目录可能有差异。" }),
          h("div", { class: "fw-placeholder-actions" }, [
            (() => {
              const b2 = h("button", { class: "fw-btn primary" }, [icon("git"), h("span", { text: "查看整仓库差异" })])
              b2.onclick = () => {
                state.path = ""
                pathInput.value = ""
                void refresh()
              }
              return b2
            })(),
          ]),
        ])
        listHost.appendChild(hint)
        return
      }
      listHost.appendChild(h("div", { class: "fw-empty", text: `两端内容一致（${a} 与 ${b}）` }))
      return
    }
    for (const f of files) {
      const st = statusMark(f)
      const name = f.path.split("/").pop() ?? f.path
      const dir = f.path.includes("/") ? f.path.slice(0, f.path.lastIndexOf("/")) : ""
      const row = h("div", { class: "fw-compare-file" }, [
        h("span", { class: `fw-change-mark ${st.cls}`, text: st.mark }),
        h("span", { class: "fw-change-name", title: f.path }, [h("span", { text: name }), dir ? h("span", { class: "fw-change-dir", text: `  ${dir}` }) : null, f.oldPath ? h("span", { class: "fw-change-orig", text: ` ← ${f.oldPath}` }) : null]),
        h("span", { class: "fw-grow" }),
        f.binary ? h("span", { class: "fw-hint", text: "二进制" }) : null,
        h("span", { class: "fw-diff-stat add", text: `+${f.additions}` }),
        h("span", { class: "fw-diff-stat del", text: `-${f.deletions}` }),
      ])
      row.onclick = () =>
        hooks.openDiff({
          title: `${name}（${a} → ${b}）`,
          root: hooks.root(),
          path: f.path,
          source: { type: "range", from: state.from, to: state.to, mergeBase: state.mergeBase, label: `${a} → ${b}` },
        })
      row.oncontextmenu = (e) => {
        e.preventDefault()
        showMenu(e.clientX, e.clientY, [
          { label: "打开并列差异", icon: "diff", onClick: () => row.click() },
          { label: "在 A 端查看此文件", icon: "eye", onClick: () => openSide(state.from, f.oldPath ?? f.path) },
          { label: "在 B 端查看此文件", icon: "eye", onClick: () => openSide(state.to, f.path) },
          { separator: true },
          { label: "在工作区打开此文件", icon: "file", onClick: () => hooks.openFile(hooks.root(), f.path) },
          { label: "复制路径", icon: "copy", onClick: () => void navigator.clipboard.writeText(f.path).then(() => toast("已复制", "success")) },
          { separator: true },
          { label: "只看此文件的比较", icon: "search", onClick: () => {
            state.path = f.path
            syncControls()
            void refresh()
          } },
        ])
      }
      listHost.appendChild(row)
    }
  }

  function openSide(ref: string, path: string): void {
    const up = ref.toUpperCase()
    if (up === WORKTREE) {
      hooks.openFile(hooks.root(), path)
      return
    }
    void hooks.api
      .gitContent(hooks.root(), ref, path)
      .then((res) => {
        if (res.missing) {
          toast(`该端（${refLabel(ref, refs)}）不存在此文件`, "warn")
          return
        }
        const blob = new Blob([res.content], { type: "text/plain;charset=utf-8" })
        window.open(URL.createObjectURL(blob), "_blank")
      })
      .catch((err) => toast(`读取失败：${(err as Error).message}`, "error"))
  }

  return {
    el,
    state: () => ({ ...state }),
    setState: (patch) => {
      Object.assign(state, patch)
      syncControls()
      void refresh()
    },
    refresh: async () => {
      await loadRefs()
      syncControls()
      await refresh()
    },
  }
}

/** 从若干入口（日志右键 / 文件历史 / 分支右键）发起比较：返回可直接打开的差异标签规格。 */
export function compareSpecBetween(a: string, b: string, path = "", mergeBase = false): { title: string; root: string; path: string; source: { type: "range"; from: string; to: string; mergeBase?: boolean } } {
  return { title: `${a} → ${b}`, root: "", path, source: { type: "range", from: a, to: b, mergeBase } }
}

/** 提交与工作树对比（「这个提交之后我又改了什么」）。 */
export function compareCommitWithWorktree(hash: string, short: string, path = ""): { title: string; root: string; path: string; source: { type: "range"; from: string; to: string } } {
  return { title: `${short} → 工作区`, root: "", path, source: { type: "range", from: hash, to: WORKTREE } }
}

export { formatTime }
