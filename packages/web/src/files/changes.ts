/**
 * 文件工作台 · 变更面板（左栏工具窗，与「资源管理器 / 搜索」互斥）。
 *
 * 为什么从底部 Git 工具窗里拆出来：IDEA 的「Commit」工具窗就在**左侧**，
 * 而"改了什么 / 提交"是编码时最频繁看的，放左侧随时可见；底部留给
 * 「分支 | 日志 | 提交内容」——那是**回顾历史**时才看的，两者节奏不同。
 *
 * 内容：按「冲突 / 已暂存 / 未暂存 / 未跟踪」分组的改动列表（行内 stage/unstage/
 * 丢弃/差异/历史），底部常驻提交框（消息 + 修补 + 署名 + 提交 / 提交并推送），
 * 多步操作（merge/rebase/cherry-pick）进行中时顶部出现「继续 / 跳过 / 中止」条。
 *
 * 视野范围：root 可能只是仓库的子目录（典型：会话工作区在项目仓库内）——
 * 默认只看该子目录内的改动（IDEA 的 Commit 窗同理），可一键切整仓库。
 */
import type { GitChange, GitStatusInfo } from "./api"
import type { DiffSpec } from "./git"
import { confirmDialog, h, icon, showMenu, toast } from "./ui"
import { btnIcon, createOpRunner, operationAction, renderNotRepo, type GitOpHooks } from "./git-shared"

export interface ChangesHooks extends GitOpHooks {
  /** 当前根在仓库内的相对前缀（root 指向仓库子目录时不为空） */
  repoPrefix: () => string
  /** 状态快照（由 main.ts 统一拉取，面板只读用） */
  status: () => GitStatusInfo | null
  /** 在主区域打开差异标签 */
  openDiff: (spec: DiffSpec) => void
  /** 打开文件（可定位行） */
  openFile: (root: string, path: string, line?: number) => void
  /** 打开比较标签 */
  openCompare: (init?: { from?: string; to?: string; path?: string; mergeBase?: boolean }) => void
  /** 打开冲突合并标签（三窗格） */
  openMerge: (repoRel: string) => void
  /** 是否可以写 */
  writable: () => boolean
  /** 远程操作是否可用 */
  remoteEnabled: () => boolean
  /** 文件系统变更后通知（树/状态栏刷新） */
  onFsChanged: () => void
  /** 打开某文件的 Git 历史（Git log --follow） */
  openFileHistory: (path: string) => void
  /** 在 Git 工具窗的日志栏按该文件过滤（宿主管工具窗的展开） */
  showInLog: (path: string) => void
  /** 改动总数变化（rail 上的「变更」按钮徽标） */
  onCount: (n: number) => void
}

export interface ChangesPanel {
  el: HTMLElement
  refresh: () => void
  dispose: () => void
}

export function createChangesPanel(hooks: ChangesHooks): ChangesPanel {
  let showWholeRepo = false
  let commitMessage = ""
  let commitAmend = false
  let committing = false

  /** 提交信息跨刷新保留（提交框每次重渲染，不保留会丢用户输入）。 */
  const el = h("div", { class: "fw-changes-panel" })
  const listHost = h("div", { class: "fw-changes-list" })
  const op = createOpRunner(hooks, async () => {
    render()
  })

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

  /** Git 状态里的路径是仓库相对；转成当前根内的相对路径（root 可能指向仓库子目录）。 */
  function prefixPath(repoRel: string): string {
    const prefix = hooks.repoPrefix()
    return prefix && repoRel.startsWith(`${prefix}/`) ? repoRel.slice(prefix.length + 1) : repoRel === prefix ? "" : repoRel
  }

  /** 丢弃改动（服务端自动 stash 备份，可恢复）。 */
  async function discard(path: string): Promise<void> {
    const ok = await confirmDialog({
      title: "丢弃改动",
      message: `丢弃「${path}」的未暂存改动？\n会自动创建 stash 备份，可从「暂存」栏恢复。`,
      okText: "丢弃",
      danger: true,
    })
    if (!ok) return
    const res = await op("discard", { paths: [path] }, "已丢弃改动")
    if (res?.backupRef) toast(`已备份到 ${res.backupRef}`, "info", 6000)
    hooks.onFsChanged()
  }

  function changeRow(c: GitChange, group: "staged" | "unstaged" | "untracked" | "conflicted"): HTMLElement {
    const name = c.path.split("/").pop() ?? c.path
    const dir = c.path.slice(0, Math.max(0, c.path.length - name.length - 1))
    const mark = c.conflicted ? "!" : c.untracked ? "U" : c.kind === "added" ? "A" : c.kind === "deleted" ? "D" : c.kind === "renamed" ? "R" : c.staged && !c.unstaged ? "S" : "M"
    const row = h("div", { class: `fw-change-row${c.conflicted ? " conflict" : ""}`, title: c.path }, [
      h("span", { class: `fw-change-mark ${mark}`, text: mark }),
      h("span", { class: "fw-change-path" }, [
        dir ? h("span", { class: "fw-change-dir", text: `${dir}/` }) : null,
        h("span", { class: "fw-change-name", text: name }),
      ]),
      h("span", { class: "fw-grow" }),
      ...(group === "conflicted"
        ? [
            (() => {
              const b = btnIcon("git", "打开冲突解决（三窗格合并）", () => hooks.openMerge(c.path))
              b.classList.add("fw-hover-only")
              return b
            })(),
          ]
        : []),
      // 行内动作：hover 才显形（常态保持列表干净，IDEA 的改动列表同理）
      ...(group === "staged"
        ? [
            (() => {
              const b = btnIcon("undo", "取消暂存", () => void op("unstage", { paths: [c.path] }, undefined, { silent: true }))
              b.classList.add("fw-hover-only")
              return b
            })(),
          ]
        : [
            (() => {
              const b = btnIcon("check", "暂存", () => void op("stage", { paths: [c.path] }, undefined, { silent: true }))
              b.classList.add("fw-hover-only")
              return b
            })(),
          ]),
      ...(group !== "staged" && !c.untracked && !c.conflicted
        ? [
            (() => {
              const b = btnIcon("undo", "丢弃改动（自动 stash 备份）", () => void discard(c.path), "danger")
              b.classList.add("fw-hover-only")
              return b
            })(),
          ]
        : []),
    ])
    row.onclick = (e) => {
      if ((e.target as HTMLElement).closest("button")) return
      // 单击开差异：变更列表的第一诉求是"我改了什么"
      if (c.untracked) hooks.openFile(hooks.root(), prefixPath(c.path))
      else
        hooks.openDiff({
          title: `${name}（${group === "staged" ? "已暂存" : "工作区"}）`,
          root: hooks.root(),
          path: c.path,
          source: { type: "worktree", staged: group === "staged" },
        })
    }
    row.ondblclick = () => {
      if (c.conflicted) hooks.openMerge(c.path)
      else if (c.untracked || c.kind === "added") hooks.openFile(hooks.root(), prefixPath(c.path))
    }
    row.oncontextmenu = (e) => {
      e.preventDefault()
      showMenu(e.clientX, e.clientY, [
        {
          label: c.untracked ? "打开文件" : "打开差异",
          icon: "diff",
          onClick: () => (c.untracked ? hooks.openFile(hooks.root(), prefixPath(c.path)) : row.click()),
        },
        ...(c.conflicted ? [{ label: "冲突解决（三窗格合并）", icon: "git", onClick: () => hooks.openMerge(c.path) }] : []),
        { separator: true },
        group === "staged"
          ? { label: "取消暂存", icon: "undo", onClick: () => void op("unstage", { paths: [c.path] }, undefined, { silent: true }) }
          : { label: "暂存", icon: "check", onClick: () => void op("stage", { paths: [c.path] }, undefined, { silent: true }) },
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
        { label: "文件历史（Git log --follow）", icon: "history", onClick: () => hooks.openFileHistory(prefixPath(c.path)) },
        { label: "在日志栏中筛选该文件", icon: "history", onClick: () => hooks.showInLog(prefixPath(c.path)) },
        { label: "与任一提交比较…", icon: "sync", onClick: () => hooks.openCompare({ from: "HEAD", to: "WORKTREE" }) },
        { separator: true },
        { label: "复制路径", icon: "copy", onClick: () => void navigator.clipboard.writeText(c.path).then(() => toast("已复制路径", "success")) },
        { label: "在资源管理器中定位", icon: "folder", onClick: () => hooks.openFile(hooks.root(), prefixPath(c.path)) },
        ...(group !== "staged"
          ? [
              { separator: true },
              { label: "丢弃改动（自动 stash 备份）", icon: "undo", danger: true, disabled: c.untracked || c.conflicted, onClick: () => void discard(c.path) },
            ]
          : []),
      ])
    }
    return row
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
        const pushRes = (res as { push?: { ok?: boolean; output?: string } }).push
        if (pushRes && pushRes.ok === false) {
          toast(`提交成功但推送失败：${pushRes.output?.slice(0, 300)}`, "error", 8000)
        } else {
          toast(push ? "已提交并推送" : "已提交", "success")
        }
        commitMessage = ""
        commitAmend = false
        await hooks.refreshStatus()
        render()
        hooks.onFsChanged()
      } catch (err) {
        toast(`提交失败：${(err as Error).message}`, "error", 8000)
      } finally {
        committing = false
        commitBtn.removeAttribute("disabled")
      }
    }

    const commitBtn = h("button", { class: "fw-btn primary", title: stagedCount ? "" : "没有已暂存的变更（将提交工作区全部改动）" }, [
      icon("check"),
      h("span", { text: commitAmend ? "修补提交" : "提交" }),
    ])
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
      h("div", { class: "fw-commit-actions" }, [
        amendToggle,
        signoff,
        h("span", { class: "fw-grow" }),
        h("span", { class: "fw-hint", text: stagedCount ? `${stagedCount} 个文件已暂存` : "未暂存（提交将包含全部改动）" }),
        pushBtn,
        commitBtn,
      ]),
    ])
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

  function render(): void {
    const s = hooks.status()
    const cnt = scopedChanges()
    const dirty = cnt.staged + cnt.unstaged + cnt.untracked + cnt.conflicted
    hooks.onCount(dirty)
    if (!s?.isRepo) {
      listHost.replaceChildren(renderNotRepo(hooks, op))
      return
    }
    const prefix = hooks.repoPrefix()
    const scoped = cnt.changes
    const groups: Array<{ key: "conflicted" | "staged" | "unstaged" | "untracked"; label: string; items: GitChange[] }> = [
      { key: "conflicted", label: "冲突", items: scoped.filter((c) => c.conflicted) },
      { key: "staged", label: "已暂存", items: scoped.filter((c) => c.staged) },
      { key: "unstaged", label: "未暂存", items: scoped.filter((c) => c.unstaged && !c.conflicted) },
      { key: "untracked", label: "未跟踪", items: scoped.filter((c) => c.untracked) },
    ]
    const list = h("div", { class: "fw-git-list" })
    if (prefix) {
      const chip = h("button", { class: "fw-chip fw-scope-chip", title: `当前限定：${prefix}` }, [
        icon(showWholeRepo ? "git" : "folder", 12),
        h("span", { text: showWholeRepo ? "整仓库（点击仅看当前目录）" : `仅当前目录：${prefix.split("/").pop() ?? prefix}` }),
      ])
      chip.onclick = () => {
        showWholeRepo = !showWholeRepo
        render()
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
    if (s.operation) list.prepend(renderOperationBanner(s))
    listHost.replaceChildren(list, renderCommitBox())
  }

  el.appendChild(listHost)
  return { el, refresh: render, dispose: () => el.remove() }
}
