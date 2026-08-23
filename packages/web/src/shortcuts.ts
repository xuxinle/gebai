import { el, input } from "./state"
import { uuid } from "./uuid"
import { autosize } from "./composer"
import { confirmDialog, promptDialog, tip, toast } from "./ui"

/**
 * 快捷按钮（浏览器本地 localStorage）：标题胶囊展示在空白页，点击把内容填入输入框。
 * 空白页直接支持「＋ 添加」弹窗新增与 hover ✕ 删除；仅存本浏览器，不上传服务端。
 */
export interface Shortcut {
  id: string
  title: string
  content: string
}

const SHORTCUTS_KEY = "gebai.ui.shortcuts"

/** 内置快捷（与用户快捷同等待遇渲染，不可删除）：点击填入提示词，由模型编排工具作答。
 *  数理化重要知识入口——措辞与 tutor 内置公式定理速查库（demo reference 模板）的学科/主题对齐。 */
const BUILT_INS: Shortcut[] = [
  {
    id: "__builtin_math_trig__",
    title: "三角函数公式",
    content: "数学三角函数公式速查：特殊角三角函数值、诱导公式、和差角与倍角公式、图象与性质，标注常见易错点",
  },
  {
    id: "__builtin_math_quadratic__",
    title: "求根与韦达定理",
    content: "一元二次方程知识速查：求根公式、判别式、韦达定理及常见变形应用，配典型例题",
  },
  {
    id: "__builtin_math_mul_formula__",
    title: "乘法公式与运算律",
    content: "乘法公式与运算律速查：平方差、完全平方、立方公式与小学运算律，附简便运算技巧",
  },
  {
    id: "__builtin_math_geometry__",
    title: "几何定理速查",
    content: "三角形与圆的几何定理速查：勾股定理、内角和、全等与相似、圆周角定理，配图示说明",
  },
  {
    id: "__builtin_math_shapes__",
    title: "面积体积与单位换算",
    content: "平面图形面积、立体图形体积公式与常用单位换算速查表",
  },
  {
    id: "__builtin_phys_mechanics__",
    title: "力与运动",
    content: "物理力学基础速查：牛顿三定律、重力与摩擦力、二力平衡与惯性，标注易错点",
  },
  {
    id: "__builtin_phys_pressure__",
    title: "压强与浮力",
    content: "物理压强与浮力速查：固体与液体压强、大气压、阿基米德原理与浮沉条件，配典型例题",
  },
  {
    id: "__builtin_phys_energy__",
    title: "功和能与简单机械",
    content: "物理功和能速查：功、功率、动能势能与机械能转化，附杠杆滑轮等简单机械要点",
  },
  {
    id: "__builtin_phys_electricity__",
    title: "电学公式",
    content: "物理电学公式速查：欧姆定律、电功率、串并联电路特点，配典型电路例题",
  },
  {
    id: "__builtin_chem_equations__",
    title: "化学方程式速查",
    content: "化学常考方程式速查：按金属、酸碱盐、氧气制取分类，含配平方法与质量守恒要点",
  },
  {
    id: "__builtin_chem_mole__",
    title: "摩尔与溶液",
    content: "化学摩尔与溶液速查：摩尔质量、物质的量浓度、溶解度与稀释计算，配典型例题",
  },
]

export function loadShortcuts(): Shortcut[] {
  try {
    const raw = localStorage.getItem(SHORTCUTS_KEY)
    if (!raw) return []
    const parsed = JSON.parse(raw) as Shortcut[]
    if (!Array.isArray(parsed)) return []
    return parsed.filter((s) => s && typeof s.title === "string" && typeof s.content === "string")
  } catch {
    return []
  }
}

export function saveShortcuts(list: Shortcut[]): void {
  try {
    localStorage.setItem(SHORTCUTS_KEY, JSON.stringify(list))
  } catch {
    /* 存储不可用（隐私模式/配额满）时静默忽略，与主题/环境变量等模块一致 */
  }
  document.dispatchEvent(new CustomEvent("gebai:shortcuts-change"))
}

/**
 * 渲染空白页快捷按钮区（只替换 .es-shortcut / .es-shortcut-hint 元素）：
 * 内置数理化知识快捷居首（与用户快捷同级样式，不可删），
 * 用户胶囊点击填入输入框、hover 显现 ✕ 删除（确认弹窗），
 * 末尾「＋ 添加」弹窗新增。
 */
export function renderShortcutButtons(container: HTMLElement): void {
  container.querySelectorAll(".es-shortcut, .es-shortcut-add").forEach((b) => b.remove())
  const list = loadShortcuts()
  for (const s of BUILT_INS) renderPill(container, s, false)
  for (const s of list) renderPill(container, s, true)
  const add = el("button", "es-shortcut-add", "＋ 添加")
  add.onclick = () => void addShortcutFlow(container)
  container.appendChild(add)
}

function renderPill(container: HTMLElement, s: Shortcut, deletable: boolean): void {
  const btn = el("button", "es-shortcut", s.title)
  tip(btn, s.content)
  btn.onclick = () => {
    input.value = s.content
    input.focus()
    autosize()
  }
  if (deletable) {
    const del = el("span", "es-shortcut-del", "✕")
    del.onclick = async (ev) => {
      ev.stopPropagation()
      if (!(await confirmDialog({ title: "删除快捷按钮", text: `删除「${s.title}」？` }))) return
      saveShortcuts(loadShortcuts().filter((x) => x.id !== s.id))
      toast("已删除", "ok")
      renderShortcutButtons(container)
    }
    btn.appendChild(del)
  }
  container.appendChild(btn)
}

async function addShortcutFlow(container: HTMLElement): Promise<void> {
  const values = await promptDialog({
    title: "添加快捷按钮",
    fields: [
      { placeholder: "标题（显示在空白页）" },
      { placeholder: "内容（点击填入输入框）", multiline: true },
    ],
  })
  if (!values) return
  const title = (values[0] ?? "").trim()
  const content = (values[1] ?? "").trim()
  if (!title || !content) {
    toast("标题与内容均必填")
    return
  }
  saveShortcuts([...loadShortcuts(), { id: uuid(), title, content }])
  toast("已添加", "ok")
  renderShortcutButtons(container)
}
