/**
 * Git 面板与变更面板的公共构件（两处都用、都不该各写一份的东西）。
 *
 * 为什么单独成模块：变更面板（左栏，工作区改动 + 提交）与 Git 工具窗（底部，分支/日志/
 * 提交内容）是**两个独立挂载点**，但都跑同一套写操作流程（统一 toasts、成功后刷新状态与自身）
 * 与非仓库占位。写操作流程若各写一份，很容易出现"一边刷新了另一边没刷新"的不一致。
 */
import type { FsApi, GitStatusInfo } from "./api"
import { h, icon, promptDialog, toast } from "./ui"

/** 跑写操作的最小依赖（两处的 hooks 都满足）。 */
export interface GitOpHooks {
  api: FsApi
  /** 当前根（根切换时面板内容随之切换） */
  root: () => string
  /** 主动刷新状态（写操作后调用） */
  refreshStatus: () => Promise<GitStatusInfo | null>
}

export function btnIcon(name: string, title: string, onClick: () => void, cls = ""): HTMLButtonElement {
  const b = h("button", { class: `fw-icon-btn ${cls}`, title })
  b.appendChild(icon(name, 13))
  b.onclick = onClick
  return b
}

/** 进行中的多步操作 → 对应的继续/中止动作名（统一走同一组端点）。 */
export function operationAction(opName: string | undefined): string {
  if (opName === "rebase" || opName === "am") return "rebase"
  if (opName === "cherry-pick") return "cherry-pick"
  if (opName === "revert") return "revert"
  return "merge"
}

/**
 * 统一写操作执行：捕获错误 → toast；成功后刷新状态与调用方视图。
 * `after` 由调用方提供（各自的渲染入口），保证"写完成 → 两边都刷新"。
 */
export function createOpRunner(hooks: GitOpHooks, after: () => Promise<void>) {
  return async function op(
    action: string,
    body: Record<string, unknown>,
    okMsg?: string,
    opts: { silent?: boolean } = {},
  ): Promise<Record<string, unknown> | null> {
    try {
      const res = (await hooks.api.gitOp(action, hooks.root(), body)) as Record<string, unknown>
      const output = typeof res.output === "string" ? res.output : ""
      if (!opts.silent) {
        if (res.ok === false) {
          toast(`${okMsg ? `${okMsg}失败：` : ""}${output || "操作未成功（可能存在冲突）"}`, "error", 6000)
        } else if (output) toast(output.split("\n").slice(0, 3).join("\n"), "info", 4000)
        else if (okMsg) toast(okMsg, "success")
      }
      await hooks.refreshStatus()
      await after()
      return res
    } catch (err) {
      toast(`${okMsg ? `${okMsg}失败：` : ""}${(err as Error).message}`, "error", 6000)
      return null
    }
  }
}

export type OpRunner = ReturnType<typeof createOpRunner>

/** 非仓库占位（可一键 init）。 */
export function renderNotRepo(hooks: GitOpHooks & { writable: () => boolean }, op: OpRunner): HTMLElement {
  const box = h("div", { class: "fw-placeholder" }, [
    h("div", { class: "fw-placeholder-msg", text: "当前根不是 Git 仓库" }),
    h("div", { class: "fw-placeholder-hint", text: "可以在此初始化仓库，或切换到含 .git 的项目根。" }),
  ])
  if (hooks.writable()) {
    const b = h("button", { class: "fw-btn primary" }, [icon("git"), h("span", { text: "初始化仓库（git init）" })])
    b.onclick = () =>
      void (async () => {
        const branch = await promptDialog({ title: "初始化 Git 仓库", label: "默认分支名", value: "main", okText: "初始化" })
        if (branch === null) return
        await op("init", { initialBranch: branch || "main" }, "已初始化仓库")
      })()
    box.appendChild(h("div", { class: "fw-placeholder-actions" }, [b]))
  }
  return box
}
