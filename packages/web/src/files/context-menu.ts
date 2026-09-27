/**
 * 编辑器右键菜单的**剪枝**：把 Monaco 自带、工作台用不到的条目隐藏起来，并收拾因此留下的空分隔线。
 *
 * 为什么在 DOM 层做：这些条目由 Monaco 注册在自己的菜单注册表里（`MenuRegistry` 不在 `monaco-editor`
 * 的公开 API 内，公开的 `editor.addAction` **只能加不能删**），而且它们渲染在编辑器自己的
 * **shadow root**（`.shadow-root-host`）里——页面 CSS 够不到 shadow 边界，所以只能等菜单出现时处理。
 *
 * 三条被剔除的取值理由：`Peek`（预览定义，与工作台的跳转方式重叠且更绕）、`Copy`（与工作台自绘的
 * 「复制路径 / 发送会话」并排时容易点错，而 Monaco 菜单里它只复制选区）、`Command Palette`
 * （Monaco 的命令面板，与工作台自己的 `Ctrl+P` / `Ctrl+K` 两套入口重复）。
 * **「转到定义 / 转到引用 / 转到符号」保留**——LSP 跳转是刚需，它们也正是这个菜单存在的理由。
 *
 * 匹配用 `aria-label` 的英文标题（Monaco 单机构建不带中文 NLS）；对不上的最坏结果是条目照旧出现，
 * 不会坏功能——所以这里不做任何断言式的容错。
 *
 * ## 关键约束：**只能隐藏，不能删除**
 *
 * Monaco 的 `ActionBar` 把焦点记成**子元素序号**，再用这个序号去索引它自己的视图表：
 *
 * ```js
 * setFocusedItem(el) { for (let i = 0; i < this.actionsList.children.length; i++) if (el === this.actionsList.children[i]) { this.focusedItem = i; break } }
 * updateFocus() { ... this.viewItems[this.focusedItem] ... }   // ← 用 DOM 序号索引内部表
 * ```
 *
 * 删掉 DOM 元素、不动内部表，两边序号就错开了：悬浮会**高亮到另一个条目**（实测：悬浮「侧边溯源」
 * 高亮「复制路径」，悬浮被删项原本占的序号则毫无高亮），键盘 `focusNext` 同理会停在没有高亮的空槽上。
 * 所以这里只把元素 `display: none`（元素仍在 `children` 里，序号不变），随后**键盘导航由本模块接管**
 *（见 `installMenuNav`）——Monaco 的方向键会走到被隐藏的项上。
 *
 * 纯逻辑（`menuPrunePlan`）与 DOM 装配分开：前者可单测，后者只管接线。
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
 * 算出要隐藏的行下标（纯函数）。
 *
 * 两步，顺序不能反：
 * ① 先挑出标题在 `MENU_DROP_LABELS` 里的条目；
 * ② 再收拾分隔线——**首行、末行、以及与相邻行同为分隔线的那些都隐藏**。第②步必须建立在①之后：
 *    `Peek` 与 `Copy` 之间本来夹着一条分隔线，两条条目都隐藏后那两条分隔线就贴在一起了
 *    （实测会变成两条 11px 的空行，看着像菜谱里多划了一道）；`Command Palette` 在末位，
 *    隐藏它就会把最后那条分隔线悬空。
 *
 * 返回的是**相对传入 rows 的下标**，由调用方按同一顺序取下标的元素隐藏。
 */
export function menuPrunePlan(rows: readonly MenuRow[]): number[] {
  const drop: number[] = []
  /** 保留下来的行下标（按原顺序）——判断“相邻”时只能看**已保留**的那条，不能看相邻原行：
   *  两条相邻分隔线若互相参照对方，会一起被隐藏（组边界整条消失）——实测踩过。 */
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
    // 分隔线：首行、或紧跟另一条已保留的分隔线 → 隐藏（同一段里只留第一条）
    const prev = kept[kept.length - 1]
    if (prev === undefined || rows[prev]!.kind === "sep") {
      drop.push(i)
      return
    }
    kept.push(i)
  })
  // 末条分隔线是悬空的（它后面没有条目了）——一并隐藏
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

/** 按计划隐藏（幂等：处理干净后再调用不会重复动手）。
 *  用 `display: none` 而**不是** `remove()`：DOM 序号必须与 Monaco 内部视图表的序号保持一致，见文件头。 */
export function pruneContextMenu(root: ParentNode): void {
  const menu = root.querySelector(".monaco-menu")
  if (!menu) return
  const { rows, els } = readMenuRows(menu)
  for (const index of menuPrunePlan(rows)) {
    const el = els[index] as HTMLElement | undefined
    if (el) el.style.display = "none"
  }
}

/** 菜单里**可选**的项（跳过被隐藏的与分隔线——分隔线没有 `a.action-menu-item`）。 */
function selectableItems(menu: HTMLElement): HTMLElement[] {
  return [...menu.querySelectorAll<HTMLElement>(".action-item")].filter(
    (li) => getComputedStyle(li).display !== "none" && li.querySelector("a.action-menu-item"),
  )
}

/**
 * 接管菜单的方向键 / Home / End：只在**可见**的条目之间移动（退订函数由调用方持有）。
 *
 * 为什么必须接管：Monaco 的 `focusNext`/`focusPrevious` 走的是它自己的视图表（含被本模块隐藏的项），
 * 于是方向键会停在没有高亮的“空槽”上，那一下回车还会触发那个看不见的条目。
 *
 * 移动方式**复用 Monaco 自己的路径**：往目标条目派发 `mouseover`——Monaco 的处理器会
 * `setFocusedItem(元素)` + `updateFocus()`，高亮与滚动都由它照常处理（不必自己维护焦点状态，
 * 也不用猜它的内部字段）。只拦方向类键：回车 / Esc / 快捷键一律留给 Monaco，菜单其它行为不受影响。
 */
export function installMenuNav(menu: HTMLElement): () => void {
  const onKeyDown = (e: KeyboardEvent): void => {
    const key = e.key
    if (key !== "ArrowDown" && key !== "ArrowUp" && key !== "Home" && key !== "End") return
    const items = selectableItems(menu)
    if (!items.length) return
    const current = menu.querySelector<HTMLElement>(".action-item.focused")
    const at = current ? items.indexOf(current) : -1
    let next: HTMLElement
    if (key === "Home") next = items[0]!
    else if (key === "End") next = items[items.length - 1]!
    else if (key === "ArrowDown") next = at < 0 ? items[0]! : items[(at + 1) % items.length]!
    // ArrowUp：没有当前项时落在末项（与 Monaco 的 focusNext 同口径：从末尾绕回开头）
    else next = at < 0 ? items[items.length - 1]! : items[(at - 1 + items.length) % items.length]!
    e.preventDefault()
    e.stopPropagation()
    next.querySelector("a")?.dispatchEvent(new MouseEvent("mouseover", { bubbles: true }))
  }
  // 捕获阶段：Monaco 自己的键盘处理挂在菜单内部，捕获先行，且不依赖它的实现细节
  menu.addEventListener("keydown", onKeyDown, true)
  return () => menu.removeEventListener("keydown", onKeyDown, true)
}

/**
 * 给一个编辑器 DOM 挂上剪枝与键盘导航接管。
 *
 * **shadow host 是懒创建的**（实测：编辑器创建后 `.monaco-editor .shadow-root-host` 一直不存在，
 * 第一次弹浮层时才出现），所以先盯着编辑器 DOM，等它出现再挂到它的 shadow root 上。
 *
 * Observer 回调是微任务，在下一帧绘制之前跑完——隐藏发生在首帧之前，看不到“先闪出 Peek 再消失”。
 * 内层回调只对**新增**节点作出反应（我们自己的改动也会触发回调，不过滤会在每次剪枝后再跑一遍；
 * 虽然幂等，但没必要）。
 *
 * 导航监听挂在**菜单元素**上，且菜单元素每次打开都换（旧的随 context-view 销毁），所以这里记住
 * 上一次挂过的元素：换了才重挂（否则同一元素上会叠多份监听）。
 */
export function installContextMenuPrune(dom: HTMLElement): { dispose(): void } {
  let inner: MutationObserver | null = null
  let detachNav: (() => void) | null = null
  let navOn: HTMLElement | null = null
  const sync = (sr: ShadowRoot): void => {
    pruneContextMenu(sr)
    const menu = sr.querySelector<HTMLElement>(".monaco-menu")
    if (menu && menu !== navOn) {
      detachNav?.()
      detachNav = installMenuNav(menu)
      navOn = menu
    }
  }
  const attach = (): void => {
    if (inner) return
    const sr = dom.querySelector(".shadow-root-host")?.shadowRoot
    if (!sr) return
    inner = new MutationObserver((records) => {
      if (!records.some((r) => r.addedNodes.length)) return
      sync(sr)
    })
    inner.observe(sr, { childList: true, subtree: true })
    sync(sr)
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
      detachNav?.()
      detachNav = null
      navOn = null
    },
  }
}
