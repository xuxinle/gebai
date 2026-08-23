import { composer, el, input } from "./state"
import { uuid } from "./uuid"
import { confirmDialog, promptDialog, tip, toast } from "./ui"

/**
 * 快捷按钮（浏览器本地 localStorage）：标题胶囊展示在空白页，点击直接发送该内容。
 * 空白页直接支持「＋ 添加」弹窗新增与 hover ✕ 删除；仅存本浏览器，不上传服务端。
 */
export interface Shortcut {
  id: string
  title: string
  content: string
}

const SHORTCUTS_KEY = "gebai.ui.shortcuts"

/** 内置快捷（与用户快捷同等待遇渲染，不可删除）：点击直接发送提示词，由模型编排工具作答。
 *  按学段分块（小学/初中/高中，各块内数→理→化排序），措辞与 tutor 内置公式定理速查库（demo reference 模板）的学科/主题对齐；
 *  分组按当前构建注册的子Agent 过滤（小学版 exe 只显小学块、中学版只显初中/高中块，见 visibleGroups）。 */
interface ShortcutGroup {
  label: string
  /** 分组归属的子Agent：按 /api/v1/sub-agents 名单过滤；null = 恒可见。 */
  agent: "tutor_primary" | "tutor_secondary" | null
  items: Shortcut[]
}

const BUILT_IN_GROUPS: ShortcutGroup[] = [
  {
    label: "小学",
    agent: "tutor_primary",
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
        id: "__builtin_p_math_fraction__",
        title: "分数与小数",
        content:
          "小学分数与小数速查：分数的意义与基本性质、约分与通分、分数加减法、小数的意义与小数点移动规律、分数与小数百分数互化，配可视化演示与典型例题",
      },
      {
        id: "__builtin_p_math_column__",
        title: "竖式计算规范",
        content:
          "小学竖式计算要点：加法减法进位借位、多位数乘法、除法试商与验算方法，分步示范书写规范与易错检查点，配竖式分步动画演示",
      },
      {
        id: "__builtin_p_math_units__",
        title: "单位换算表",
        content:
          "小学常用单位换算速查：长度（千米/米/分米/厘米/毫米）、面积（平方千米/公顷/平方米/平方分米/平方厘米）、体积与容积（立方米/立方分米/升/毫升）、质量（吨/千克/克）、时间的进率表与换算方法，附典型换算例题",
      },
      {
        id: "__builtin_p_math_wordprob__",
        title: "行程与工程应用题",
        content:
          "小学经典应用题总结：行程问题（路程=速度×时间、相遇与追及）、工程问题（工作总量=效率×时间）、鸡兔同笼、植树问题，配线段图分析与分步解题示范",
      },
      {
        id: "__builtin_p_math_stat__",
        title: "统计与平均数",
        content:
          "小学统计速查：平均数计算（总数÷份数）、条形统计图与折线统计图的读法与绘制要点、可能性大小判断，配典型例题",
      },
    ],
  },
  {
    label: "初中",
    agent: "tutor_secondary",
    items: [
      {
        id: "__builtin_j_math_formula__",
        title: "乘法公式与因式分解",
        content:
          "初中数学乘法公式与因式分解速查：平方差、完全平方（含常见变形式 a²+b²=(a+b)²-2ab）、立方和差公式；因式分解四法（提公因式/公式法/十字相乘/分组），配典型例题与易错点",
      },
      {
        id: "__builtin_j_math_surd__",
        title: "二次根式",
        content:
          "初中二次根式速查：二次根式有意义的条件、性质 √(a²)=|a|、最简二次根式与同类二次根式、乘除运算 √a·√b=√(ab)（a,b≥0）、加减（先化简再合并同类）、分母有理化，配典型例题与易错点",
      },
      {
        id: "__builtin_j_math_fraction__",
        title: "分式与分式方程",
        content:
          "初中分式速查：分式有意义与值为 0 的条件、基本性质与约分通分、分式加减乘除混合运算顺序、分式方程解法（去分母化整式方程）与验根（增根产生原因），配典型例题",
      },
      {
        id: "__builtin_j_math_quadratic__",
        title: "一元二次方程",
        content:
          "初中数学一元二次方程速查：四种解法（直接开平方/配方法/公式法/因式分解法）与各自适用场景、判别式与根的情况、韦达定理及常见应用（不解方程求对称式的值），配典型例题",
      },
      {
        id: "__builtin_j_math_trig__",
        title: "锐角三角函数",
        content:
          "初中锐角三角函数速查：sinA/cosA/tanA 定义（对边、邻边与斜边的关系）、30°45°60° 特殊角三角函数值表、解直角三角形的基本类型、仰角俯角与坡度坡角应用，配典型例题",
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
        id: "__builtin_j_phys_light__",
        title: "光与声",
        content:
          "初中物理声与光速查：声的产生传播与三特性（音调/响度/音色）、噪声控制途径、光的直线传播（影/小孔成像）、反射定律与平面镜成像特点、折射现象、凸透镜成像规律及应用，配光路图与例题",
      },
      {
        id: "__builtin_j_phys_heat__",
        title: "热学",
        content:
          "初中物理热学速查：温度与温度计使用、六种物态变化及吸放热（熔化凝固/汽化液化/升华凝华）、水的三态循环、比热容与热量计算 Q=cmΔt、热值与热机效率，配典型例题与晶体熔化图象题",
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
        id: "__builtin_j_chem_metal__",
        title: "金属与活动性顺序",
        content:
          "初中化学金属速查：金属共性与特性、金属活动性顺序表及应用（置换反应判断、滤液滤渣问题）、铁生锈条件与防锈措施、工业炼铁原理（CO 还原 Fe₂O₃），配典型例题",
      },
      {
        id: "__builtin_j_chem_eq__",
        title: "化学方程式",
        content:
          "初中化学常考方程式速查：氧气制取（高锰酸钾/氯酸钾/过氧化氢）、金属与酸、金属与盐溶液、酸碱中和、碳酸盐与酸的反应方程式，按化合/分解/置换/复分解分类，含配平步骤、反应条件标注与质量守恒定律要点",
      },
      {
        id: "__builtin_j_chem_acid__",
        title: "酸碱盐",
        content:
          "初中化学酸碱盐速查：常见酸（盐酸/硫酸）与碱（氢氧化钠/氢氧化钙）的性质用途、酸碱指示剂与 pH、中和反应实质、盐的性质与化肥、复分解反应发生条件（沉淀/气体/水）、常见物质检验与除杂，配典型例题与推断题思路",
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
    agent: "tutor_secondary",
    items: [
      {
        id: "__builtin_s_math_func__",
        title: "函数性质",
        content:
          "高中函数性质速查：单调性定义法与导数法判定、奇偶性判定与图象对称性、周期性与对称性的常用结论、复合函数单调性（同增异减），配典型例题与抽象函数问题思路",
      },
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
        id: "__builtin_s_math_seq__",
        title: "数列",
        content:
          "高中数列速查：等差数列通项 an=a₁+(n-1)d 与前 n 项和公式、等比数列通项与求和（含公比 q=1 讨论）、an 与 Sn 的关系（n≥2 时 an=Sn−Sₙ₋₁）、错位相减法与裂项相消法，配典型例题",
      },
      {
        id: "__builtin_s_math_ineq__",
        title: "均值不等式",
        content:
          "高中不等式速查：基本不等式 √(ab)≤(a+b)/2 及取等条件、常见变形（乘一法、1 的代换）、一元二次不等式与分式不等式解法、恒成立问题思路，配典型例题与取等条件易错警示",
      },
      {
        id: "__builtin_s_math_deriv__",
        title: "导数",
        content:
          "高中导数速查：常用导数公式表（幂/指数/对数/正余弦）、四则运算法则与复合函数链式法则、切线方程求法、单调区间与极值最值的通用解题步骤、含参分类讨论思路，配典型例题",
      },
      {
        id: "__builtin_s_math_conic__",
        title: "解析几何",
        content:
          "高中解析几何速查：直线方程五种形式与位置关系判定、圆的标准方程与一般方程、椭圆/双曲线/抛物线的标准方程与几何性质（焦点/离心率/准线/渐近线）、弦长公式与点差法（中点弦），配典型例题",
      },
      {
        id: "__builtin_s_math_solid__",
        title: "立体几何",
        content:
          "高中立体几何速查：线面平行与垂直的判定及性质定理、异面直线所成角/线面角/二面角的求法、空间向量法标准步骤（建系/求法向量/算角）、常见几何体外接球与内切球思路，配图示与典型例题",
      },
      {
        id: "__builtin_s_math_prob__",
        title: "概率与统计",
        content:
          "高中概率统计速查：分类加法与分步乘法计数原理、排列组合常用方法（捆绑/插空/隔板）、古典概型与条件概率、独立事件与二项分布、离散型随机变量的期望与方差、正态分布对称性，配典型例题",
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
        id: "__builtin_s_chem_organic__",
        title: "有机化学",
        content:
          "高中有机化学速查：常见官能团识别（碳碳双键/卤素原子/羟基/醛基/羧基/酯基）及其性质、典型反应类型（取代/加成/氧化/酯化/水解）、常见有机物的检验（溴水/酸性高锰酸钾/银镜反应）、同分异构体书写要点，配典型例题",
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

/* ---------- 分组可见性：按当前构建注册的子Agent 过滤（小学版/中学版专属产物） ---------- */

/** 当前构建的子Agent名单（REST /api/v1/sub-agents，进程内拉取一次）。null = 未知（首帧全显兜底，名单到达后按需重渲染）。 */
let agentNamesCache: string[] | null = null
let agentNamesPromise: Promise<string[]> | null = null

function fetchAgentNames(): Promise<string[]> {
  if (agentNamesCache) return Promise.resolve(agentNamesCache)
  if (!agentNamesPromise) {
    agentNamesPromise = fetch("/api/v1/sub-agents")
      .then((r) => (r.ok ? r.json() : []))
      .then((list: Array<{ name?: string }>) => {
        agentNamesCache = list.map((x) => x.name ?? "").filter(Boolean)
        return agentNamesCache
      })
      .catch(() => {
        agentNamesCache = []
        return agentNamesCache
      })
  }
  return agentNamesPromise
}

/** 可见分组：名单只含 tutor_primary → 仅小学块；只含 tutor_secondary → 仅初中/高中块；
 *  名单未知/为空/两学段皆有/皆无（全量构建、非教辅环境、接口不可用）→ 全显兜底。 */
function visibleGroups(names: string[] | null): ShortcutGroup[] {
  if (!names || !names.length) return BUILT_IN_GROUPS
  const hasP = names.includes("tutor_primary")
  const hasS = names.includes("tutor_secondary")
  if (hasP && hasS) return BUILT_IN_GROUPS
  if (hasP) return BUILT_IN_GROUPS.filter((g) => !g.agent || g.agent === "tutor_primary")
  if (hasS) return BUILT_IN_GROUPS.filter((g) => !g.agent || g.agent === "tutor_secondary")
  return BUILT_IN_GROUPS
}

/**
 * 渲染空白页快捷按钮区（只替换 .es-shortcut / .es-shortcut-group / .es-shortcut-add 元素）：
 * 内置数理化知识快捷按学段分块并按当前构建的子Agent过滤（小学版只显小学块、中学版只显初中/高中块；
 * 名单未到位时首帧全显，REST 返回后可见块数变化则重渲染一次），不可删；
 * 用户快捷有则归入「自定义」块，点击直接发送（悬停可预览完整内容）、hover 显现 ✕ 删除（确认弹窗），
 * 末尾「＋ 添加」弹窗新增。
 */
export function renderShortcutButtons(container: HTMLElement): void {
  container.querySelectorAll(".es-shortcut, .es-shortcut-group, .es-shortcut-add").forEach((b) => b.remove())
  const list = loadShortcuts()
  const groups = visibleGroups(agentNamesCache)
  for (const g of groups) {
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
  if (agentNamesCache === null) {
    void fetchAgentNames().then(() => {
      if (container.isConnected && visibleGroups(agentNamesCache).length !== groups.length) {
        renderShortcutButtons(container)
      }
    })
  }
}

function renderPill(container: HTMLElement, s: Shortcut, deletable: boolean): void {
  const btn = el("button", "es-shortcut", s.title)
  tip(btn, s.content)
  btn.onclick = () => {
    input.value = s.content
    // 直接发送：走 composer 表单 submit（含空白页懒建会话/隐藏空状态/粘底锁定等完整链路）
    composer.requestSubmit()
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
      { placeholder: "内容（点击直接发送）", multiline: true },
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
