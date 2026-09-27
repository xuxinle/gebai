/**
 * 编辑器右键菜单的**剪枝**：剔除 Monaco 自带、工作台用不到的条目，并收拾因此留下的空分隔线。
 *
 * 为什么在 DOM 层做：这些条目由 Monaco 注册在它自己的菜单注册表里（`MenuRegistry` 不在 `monaco-editor`
 * 的公开 API 内，公开的 `editor.addAction` **只能加不能删**），而且它们渲染在编辑器自己的
 * **shadow root**（`.shadow-root-host`）里——页面 CSS 够不到 shadow 边界，所以只能等菜单出现时剪一遍。
 *
 * 三条被剔除的取值理由：`Peek`（预览定义，与工作台的跳转方式重叠且更绕）、`Copy`（与工作台自绘的
 * 「复制路径 / 发送会话」并排时容易点错，而 Monaco 菜单里它只复制选区）、`Command Palette`
 * （Monaco 的命令面板，与工作台自己的 `Ctrl+P` / `Ctrl+K` 两套入口重复）。
 * **「转到定义 / 转到引用 / 转到符号」保留**——LSP 跳转是刚需，它们也正是这个菜单存在的理由。
 *
 * 匹配用 `aria-label` 的英文标题（Monaco 单机构建不带中文 NLS）；对不上的最坏结果是条目照旧出现，
 * 不会坏功能——所以这里不做任何断言式的容错（没有「找不到就报错」）。
 *
 * 纯逻辑（`menuPrunePlan`）与 DOM 装配（`installContextMenuPrune`）分开：前者可单测，后者只管接线。
 */

/** 要剔除的内置菜单条目（`.action-label` 的 `aria-label`）。 */
export const MENU_DROP_LABELS: ReadonlySet<string> = new Set(["Peek", "Copy", "Command Palette"])

/** 菜单里的一行：普通条目（带标题）或分隔线。 */
export interface MenuRow {
  kind: "item" | "sep"
  /** 条目的标题（分隔线为空串） */
  label: string
}

/**
 * 算出要删掉的行下标（纯函数）。
 *
 * 两步，顺序不能反：
 * ① 先删掉标题在 `MENU_DROP_LABELS` 里的条目；
 * ② 再收拾分隔线——**首行、末行、以及与相邻行同为分隔线的那些都删掉**。第②步必须建立在①之后：
 *    `Peek` 与 `Copy` 之间本来夹着一条分隔线，两条条目都删掉后那两条分隔线就贴在一起了
 *    （实测会变成两条 11px 的空行，看着像菜谱里多划了一道）；`Command Palette` 在末位，
 *    删掉它就会把最后那条分隔线悬空。
 *
 * 返回的是**相对传入 rows 的下标**，由调用方按同一顺序取下标的元素删除。
 */
export function menuPrunePlan(rows: readonly MenuRow[]): number[] {
  const drop: number[] = []
  /** 保留下来的行下标（按原顺序）——判断“相邻”时只能看**已保留**的那条，不能看相邻原行：
   *  两条相邻分隔线若互相参照对方，会一起被删掉（组边界整条消失）——实测踩过。 */
  const kept: number[] = []
  rows.forEach((row, i) => {
    if (row.kind === "item") {
      if (MENU_DROP_LABELS.has(row.label)) {
        drop.push(i)
        return
      }
      kept.push(i)
      return
    }
    // 分隔线：首行、或紧跟另一条已保留的分隔线 → 删（同一段里只留第一条）
    const prev = kept[kept.length - 1]
    if (prev === undefined || rows[prev]!.kind === "sep") {
      drop.push(i)
      return
    }
    kept.push(i)
  })
  // 末条分隔线是悬空的（它后面没有条目了）——一并删掉
  const last = kept[kept.length - 1]
  if (last !== undefined && rows[last]!.kind === "sep") drop.push(last)
  return drop.sort((a, b) => a - b)
}

/** 从菜单根元素读出各行（分隔线在 DOM 里是 `.action-item.disabled > .action-label.separator` 的空行）。 */
export function readMenuRows(menu: ParentNode): { rows: MenuRow[]; els: Element[] } {
  const els = [...menu.querySelectorAll(".action-item")]
  const rows = els.map((el) => {
    const label = el.querySelector(".action-label")
    if (label?.classList.contains("separator")) return { kind: "sep" as const, label: "" }
    return { kind: "item" as const, label: (label?.getAttribute("aria-label") ?? label?.textContent ?? "").trim() }
  })
  return { rows, els }
}

/** 按计划剪一次（幂等：剪干净后再调用不会删任何东西）。 */
export function pruneContextMenu(root: ParentNode): void {
  const menu = root.querySelector(".monaco-menu")
  if (!menu) return
  const { rows, els } = readMenuRows(menu)
  for (const index of menuPrunePlan(rows)) els[index]?.remove()
}

/**
 * 给一个编辑器 DOM 挂上剪枝。
 *
 * **shadow host 是懒创建的**（实测：编辑器创建后 `.monaco-editor .shadow-root-host` 一直不存在，
 * 第一次弹浮层时才出现），所以先盯着编辑器 DOM，等它出现再挂到它的 shadow root 上。
 *
 * Observer 回调是微任务，在下一帧绘制之前跑完——剪枝发生在首帧之前，看不到“先闪出 Peek 再消失”。
 * 内层回调只对**新增**节点作出反应（我们自己的删除也会触发回调，不过滤会在每次剪枝后再跑一遍；
 * 虽然幂等，但没必要）。
 */
export function installContextMenuPrune(dom: HTMLElement): { dispose(): void } {
  let inner: MutationObserver | null = null
  const attach = (): void => {
    if (inner) return
    const sr = dom.querySelector(".shadow-root-host")?.shadowRoot
    if (!sr) return
    inner = new MutationObserver((records) => {
      if (!records.some((r) => r.addedNodes.length)) return
      pruneContextMenu(sr)
    })
    inner.observe(sr, { childList: true, subtree: true })
    pruneContextMenu(sr)
  }
  /* 外层只看**直接子级**（不用 subtree）：`.shadow-root-host` 就是 `.monaco-editor` 的直接子元素（实测），
     而编辑器内部的 DOM 变更极其频繁（每次击键/滚动/光标移动都改子树）——带 subtree 的话，
     在用户首次弹右键菜单之前（可能整个会话都不弹），每个突变都要跑一次 querySelector。 */
  const outer = new MutationObserver(attach)
  outer.observe(dom, { childList: true })
  attach()
  return {
    dispose: () => {
      outer.disconnect()
      inner?.disconnect()
      inner = null
    },
  }
}
