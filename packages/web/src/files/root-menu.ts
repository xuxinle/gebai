/**
 * 根选择菜单的分组与折叠（纯逻辑，无 DOM，资源管理器的根按钮用它拼菜单）。
 *
 * 分两类决策：
 *
 * - **分组顺序与标题**：项目（`proj`）排最上——它是最常用的落脚点，其余按「会话工作区 → 其它」。
 *   标题「项目」而不是「预置项目」：菜单里说「项目」就够，`proj` 这个名字是服务端注册表的叫法。
 * - **会话折叠**：会话根按最近使用排（服务端已这样排）且数量可以很多（上限 20），全铺出来会把这个
 *   菜单拉成一长条，而其中绝大多数是「顺手点一下看看」的旧会话。默认只列「最近若干个 + 当前所在的那个」，
 *   其余收进「更多会话…」子菜单（子菜单 hover 展开，想找的仍在两层之内）。
 */

/** 菜单里的一项（数据形态：取根清单里菜单用得着的字段；点击回调由调用方补）。 */
export interface RootMenuEntry {
  id: string
  /** 显示名 */
  name: string
  kind: string
  /** 是否仓库（菜单里给分支后缀用） */
  isRepo?: boolean
  branch?: string
}

export interface RootMenuSection {
  /** 分类标题（菜单里渲染成不可点的分组行） */
  title: string
  entries: RootMenuEntry[]
  /** 折叠出去的其余项（会话组专属；为空表示不折叠） */
  more?: { label: string; entries: RootMenuEntry[] }
}

/** 会话组默认折叠阈值：超过它就收进「更多」（会话名往往很长，多列几条就把菜单拉成一长条）。 */
export const SESSION_VISIBLE = 5

const GROUP_ORDER: Array<{ title: string; kind: string; collapse?: boolean }> = [
  { title: "项目", kind: "proj" },
  { title: "会话工作区", kind: "sess", collapse: true },
  { title: "其它", kind: "other" },
]

const KNOWN_KINDS = ["sess", "proj"]

/** 分组 + 折叠（`roots` 的顺序按调用方给的来；会话组保持服务端的最近使用序）。 */
export function buildRootSections(roots: RootMenuEntry[], currentRootId: string, sessVisible = SESSION_VISIBLE): RootMenuSection[] {
  const sections: RootMenuSection[] = []
  for (const g of GROUP_ORDER) {
    const list = roots.filter((x) => (g.kind === "other" ? !KNOWN_KINDS.includes(x.kind) : x.kind === g.kind))
    if (!list.length) continue
    if (!g.collapse || list.length <= sessVisible) {
      sections.push({ title: g.title, entries: list })
      continue
    }
    // 当前所在的会话必须始终可见（否则「我在哪个会话」要靠猜），其余按最近使用取前几个
    const head = list.slice(0, sessVisible)
    if (!head.some((x) => x.id === currentRootId)) {
      const cur = list.find((x) => x.id === currentRootId)
      if (cur) head[head.length - 1] = cur
    }
    const rest = list.filter((x) => !head.some((h) => h.id === x.id))
    sections.push({ title: g.title, entries: head, more: { label: `更多会话（${rest.length}）`, entries: rest } })
  }
  return sections
}
