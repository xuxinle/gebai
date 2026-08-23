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
 *  按学段分块（小学/初中/高中），措辞与 tutor 内置公式定理速查库（demo reference 模板）的学科/主题对齐。 */
interface ShortcutGroup {
  label: string
  items: Shortcut[]
}

const BUILT_IN_GROUPS: ShortcutGroup[] = [
  {
    label: "小学",
    items: [
      {
        id: "__builtin_p_math_plane__",
        title: "周长与面积公式",
        content:
          "小学数学平面图形速查：长方形、正方形、平行四边形、三角形、梯形、圆的周长与面积公式，每个公式配图形示意与推导思路，附单位说明和 2 道典型例题",
      },
      {
        id: "__builtin_p_math_solid__",
        title: "表面积与体积公式",
        content:
          "小学数学立体图形速查：长方体、正方体、圆柱、圆锥的表面积与体积公式，配图形示意、公式的直观理解（底面积×高）与典型例题",
      },
      {
        id: "__builtin_p_math_laws__",
        title: "运算律与简便计算",
        content:
          "小学数学运算律速查：加法交换律与结合律、乘法交换律/结合律/分配律，配凑整、提取公因数、拆项等简便计算技巧，每种配典型例题与常见易错",
      },
      {
        id: "__builtin_p_math_units__",
        title: "单位换算表",
        content:
          "小学常用单位换算速查：长度（千米/米/分米/厘米/毫米）、面积（平方千米/公顷/平方米/平方分米/平方厘米）、体积与容积（立方米/立方分米/升/毫升）、质量（吨/千克/克）、时间的进率表与换算方法，附典型换算例题",
      },
    ],
  },
  {
    label: "初中",
    items: [
      {
        id: "__builtin_j_math_formula__",
        title: "乘法公式与因式分解",
        content:
          "初中数学乘法公式与因式分解速查：平方差、完全平方（含常见变形式 a²+b²=(a+b)²-2ab）、立方和差公式；因式分解四法（提公因式/公式法/十字相乘/分组），配典型例题与易错点",
      },
      {
        id: "__builtin_j_math_quadratic__",
        title: "一元二次方程",
        content:
          "初中数学一元二次方程速查：四种解法（直接开平方/配方法/公式法/因式分解法）与各自适用场景、判别式与根的情况、韦达定理及常见应用（不解方程求对称式的值），配典型例题",
      },
      {
        id: "__builtin_j_math_geometry__",
        title: "几何定理（三角形与圆）",
        content:
          "初中几何定理速查：全等五种判定（SSS/SAS/ASA/AAS/HL）与相似判定、勾股定理及逆定理、三角形内角和与外角、中位线、圆周角定理、切线的判定与性质，每条配图示说明与典型证明思路",
      },
      {
        id: "__builtin_j_math_function__",
        title: "函数图象性质",
        content:
          "初中函数速查：一次函数、反比例函数、二次函数的解析式、图象特征、增减性、对称性（二次函数含顶点/对称轴/开口方向），参数 k、b、a 对图象的影响，配对比表与典型例题",
      },
      {
        id: "__builtin_j_phys_force__",
        title: "力与运动",
        content:
          "初中物理力与运动速查：牛顿第一定律与惯性、二力平衡条件、重力 G=mg、滑动摩擦力影响因素、参照物与速度 v=s/t（含平均速度），配典型例题与易错辨析（如惯性不是力）",
      },
      {
        id: "__builtin_j_phys_pressure__",
        title: "压强与浮力",
        content:
          "初中物理压强与浮力速查：固体压强 p=F/S、液体压强 p=ρgh 及适用条件、连通器与大气压强应用、阿基米德原理 F浮=ρ液gV排、浮沉条件与漂浮问题，配典型例题与易错点",
      },
      {
        id: "__builtin_j_phys_energy__",
        title: "功和能与简单机械",
        content:
          "初中物理功和能速查：功 W=Fs（做功两要素）、功率 P=W/t、机械效率及其影响因素、动能/势能影响因素与相互转化、杠杆平衡条件 F₁L₁=F₂L₂、滑轮与滑轮组（省力与费距离），配典型例题",
      },
      {
        id: "__builtin_j_phys_elec__",
        title: "电学公式",
        content:
          "初中物理电学速查：欧姆定律 I=U/R 及适用条件、串并联电路电流/电压/电阻规律对比、电功 W=UIt、电功率 P=UI（含额定与实际功率）、焦耳定律 Q=I²Rt，配典型电路分析与例题",
      },
      {
        id: "__builtin_j_chem_eq__",
        title: "化学方程式",
        content:
          "初中化学常考方程式速查：氧气制取（高锰酸钾/氯酸钾/过氧化氢）、金属与酸、金属与盐溶液、酸碱中和、碳酸盐与酸的反应方程式，按化合/分解/置换/复分解分类，含配平步骤、反应条件标注与质量守恒定律要点",
      },
      {
        id: "__builtin_j_chem_solution__",
        title: "溶液计算",
        content:
          "初中化学溶液速查：饱和与不饱和溶液的转化、溶解度与溶解度曲线解读、溶质质量分数计算（含稀释问题——稀释前后溶质质量不变），配典型计算题与易错点",
      },
    ],
  },
  {
    label: "高中",
    items: [
      {
        id: "__builtin_s_math_trig__",
        title: "三角函数",
        content:
          "高中三角函数速查：弧度制换算、特殊角三角函数值表、同角基本关系式、诱导公式（奇变偶不变、符号看象限）、和差角公式、二倍角公式、辅助角公式 asinα+bcosα 化一，正弦/余弦/正切函数的图象与性质（周期/单调/奇偶/对称轴），配典型例题与化简求值技巧",
      },
      {
        id: "__builtin_s_math_triangle__",
        title: "解三角形",
        content:
          "高中解三角形速查：正弦定理及其变形（a/sinA=b/sinB=c/sinC=2R）、余弦定理及其变形、三角形面积公式（含 ½absinC），配边角互化、解的个数判断、周长与面积范围等典型题型",
      },
      {
        id: "__builtin_s_math_ineq__",
        title: "均值不等式",
        content:
          "高中不等式速查：基本不等式 √(ab)≤(a+b)/2 及取等条件、常见变形（乘一法、1 的代换）、一元二次不等式与分式不等式解法、恒成立问题思路，配典型例题与取等条件易错警示",
      },
      {
        id: "__builtin_s_math_func__",
        title: "函数性质",
        content:
          "高中函数性质速查：单调性定义法与导数法判定、奇偶性判定与图象对称性、周期性与对称性的常用结论、复合函数单调性（同增异减），配典型例题与抽象函数问题思路",
      },
      {
        id: "__builtin_s_phys_mech__",
        title: "力学综合",
        content:
          "高中物理力学速查：牛顿三大定律与适用条件、受力分析步骤（先重力/弹力/摩擦力）、整体法与隔离法选用、超重失重、匀速圆周运动向心力来源分析、万有引力与卫星（含第一宇宙速度推导），配典型例题与受力图示",
      },
      {
        id: "__builtin_s_phys_em__",
        title: "电磁学",
        content:
          "高中物理电磁速查：电场强度与电势、电容、闭合电路欧姆定律（E=U+Ir）与电源功率、磁感应强度、安培力与洛伦兹力（含带电粒子圆周运动半径周期公式），配典型例题与常见模型",
      },
      {
        id: "__builtin_s_chem_mole__",
        title: "物质的量",
        content:
          "高中化学物质的量速查：摩尔质量、气体摩尔体积（标准状况 22.4 L/mol）、物质的量浓度、阿伏加德罗常数，各量换算关系（n=m/M=V/Vm=cV），配混合溶液与稀释计算典型例题",
      },
      {
        id: "__builtin_s_chem_conserve__",
        title: "守恒定律",
        content:
          "高中化学三大守恒速查：质量守恒（反应前后原子种类与数目不变）、电荷守恒（溶液电中性）、电子守恒（氧化还原电子转移相等）在化学计算与离子方程式配平中的应用，配典型例题",
      },
    ],
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
 * 渲染空白页快捷按钮区（只替换 .es-shortcut / .es-shortcut-group / .es-shortcut-add 元素）：
 * 内置数理化知识快捷按学段分块（小学/初中/高中，分组标签独占一行），不可删；
 * 用户快捷有则归入「自定义」块，点击填入输入框、hover 显现 ✕ 删除（确认弹窗），
 * 末尾「＋ 添加」弹窗新增。
 */
export function renderShortcutButtons(container: HTMLElement): void {
  container.querySelectorAll(".es-shortcut, .es-shortcut-group, .es-shortcut-add").forEach((b) => b.remove())
  const list = loadShortcuts()
  for (const g of BUILT_IN_GROUPS) {
    container.appendChild(el("div", "es-shortcut-group", g.label))
    for (const s of g.items) renderPill(container, s, false)
  }
  if (list.length) {
    container.appendChild(el("div", "es-shortcut-group", "自定义"))
    for (const s of list) renderPill(container, s, true)
  }
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
