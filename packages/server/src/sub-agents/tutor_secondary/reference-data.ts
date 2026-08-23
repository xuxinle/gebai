/* 内置公式与定理速查库（demo 工具 reference 模板的数据源）——静态知识条目，
 * 随二进制分发（版本化、零 LLM 开销）；条目按学科→主题组织，模型按 subject/topic
 * 子串过滤或 id 取单条。新增条目直接在此追加（id 全局唯一）。 */

export type RefStage = "primary" | "secondary" | "both"

export interface RefEntry {
  id: string
  stage: RefStage
  subject: string
  topic: string
  name: string
  /** 公式（Unicode 数学文本，大字展示；多条用 <br> 分行）。 */
  formula?: string
  /** 定理/结论表述。 */
  statement?: string
  /** 使用要点与易错点。 */
  notes?: string
  /** 证明思路（定理类）。 */
  proof?: string
  /** 图示 SVG（可选）。 */
  svg?: string
}

export const REF_ENTRIES: RefEntry[] = [
  /* ---------- 小学数学 ---------- */
  {
    id: "pm-rect",
    stage: "primary",
    subject: "数学",
    topic: "平面图形",
    name: "长方形与正方形",
    formula: "长方形：C = 2(a+b)，S = ab<br>正方形：C = 4a，S = a²",
    notes: "先求周长还是面积要看清问的是「一圈多长」还是「占多大面」，单位一个是长度一个是面积。",
  },
  {
    id: "pm-parallelogram",
    stage: "primary",
    subject: "数学",
    topic: "平面图形",
    name: "平行四边形面积",
    formula: "S = ah（底 × 高）",
    notes: "高必须与所选的底垂直——底和斜边相乘是最常见错误；割补法可拼成长方形帮助理解。",
  },
  {
    id: "pm-triangle",
    stage: "primary",
    subject: "数学",
    topic: "平面图形",
    name: "三角形面积",
    formula: "S = ah ÷ 2",
    notes: "两个完全相同的三角形可拼成平行四边形，所以记得 ÷2；底和高要「配套」（对应的）。",
  },
  {
    id: "pm-trapezoid",
    stage: "primary",
    subject: "数学",
    topic: "平面图形",
    name: "梯形面积",
    formula: "S = (a+b) × h ÷ 2（上底+下底 的和 × 高 ÷ 2）",
    notes: "两个完全相同的梯形可拼成平行四边形，底是 (a+b)；别把上底下底加成乘。",
  },
  {
    id: "pm-circle",
    stage: "primary",
    subject: "数学",
    topic: "平面图形",
    name: "圆的周长与面积",
    formula: "C = 2πr = πd，S = πr²",
    notes: "π 通常取 3.14；半径扩大 2 倍，周长扩大 2 倍、面积扩大 4 倍（平方关系）。",
  },
  {
    id: "pm-cuboid",
    stage: "primary",
    subject: "数学",
    topic: "立体图形",
    name: "长方体与正方体",
    formula: "长方体：V = abh，S表 = 2(ab+ah+bh)<br>正方体：V = a³，S表 = 6a²",
    notes: "表面积是六个面之和，实际问题常缺面（水池无盖、通风管只有侧面）——先想清楚有几个面。",
  },
  {
    id: "pm-cylinder",
    stage: "primary",
    subject: "数学",
    topic: "立体图形",
    name: "圆柱与圆锥",
    formula: "圆柱：V = Sh = πr²h<br>圆锥：V = Sh ÷ 3 = πr²h ÷ 3",
    notes: "等底等高的圆锥体积是圆柱的三分之一；圆锥别忘 ÷3，圆柱别多除。",
  },
  {
    id: "pm-oplaws",
    stage: "primary",
    subject: "数学",
    topic: "运算律",
    name: "运算律（交换·结合·分配）",
    formula: "a+b = b+a，（a+b)+c = a+(b+c)<br>ab = ba，(ab)c = a(bc)<br>a(b+c) = ab+ac",
    notes: "分配律可以用「面积模型」理解：宽 a、长 (b+c) 的大矩形面积 = 两个小矩形面积之和。简算题先找「凑整」与「相同因数」。",
    svg:
      '<svg viewBox="0 0 360 200" style="max-width:100%;height:auto">' +
      '<rect x="30" y="50" width="240" height="100" fill="#dbe7ff" stroke="#345" stroke-width="2"/>' +
      '<line x1="150" y1="50" x2="150" y2="150" stroke="#e5534b" stroke-width="2" stroke-dasharray="6 4"/>' +
      '<text x="80" y="110" font-size="16" fill="#345">b</text><text x="185" y="110" font-size="16" fill="#345">c</text>' +
      '<text x="14" y="105" font-size="16" fill="#345">a</text>' +
      '<text x="60" y="175" font-size="14" fill="#888">a×(b+c) = a×b + a×c</text></svg>',
  },
  {
    id: "pm-units",
    stage: "primary",
    subject: "数学",
    topic: "单位换算",
    name: "常用单位换算",
    formula:
      "长度：1km=1000m，1m=10dm=100cm<br>面积：1m²=100dm²，1dm²=100cm²，1公顷=10000m²<br>体积/容积：1m³=1000dm³，1L=1dm³，1mL=1cm³<br>质量：1t=1000kg，1kg=1000g<br>时间：1时=60分，1分=60秒",
    notes: "面积单位进率是 100、体积是 1000（相邻）；高级→低级乘进率，低级→高级除以进率。",
  },

  /* ---------- 中学数学 ---------- */
  {
    id: "sm-mul",
    stage: "secondary",
    subject: "数学",
    topic: "乘法公式",
    name: "乘法公式",
    formula: "(a+b)² = a²+2ab+b²<br>(a−b)² = a²−2ab+b²<br>(a+b)(a−b) = a²−b²",
    notes: "完全平方别漏中间项 2ab（「首平方、尾平方、积二倍居中央」）；符号变化可先化为标准形式。",
  },
  {
    id: "sm-quadratic",
    stage: "secondary",
    subject: "数学",
    topic: "一元二次方程",
    name: "求根公式与判别式",
    formula: "ax²+bx+c=0（a≠0）：<br>x = (−b ± √(b²−4ac)) ÷ 2a<br>Δ = b²−4ac",
    notes: "Δ>0 两不相等实根；Δ=0 两相等实根；Δ<0 无实根。用公式前必须先化成一般形式并确认 a≠0。",
  },
  {
    id: "sm-vieta",
    stage: "secondary",
    subject: "数学",
    topic: "一元二次方程",
    name: "韦达定理（根与系数关系）",
    formula: "x₁+x₂ = −b/a，x₁·x₂ = c/a",
    notes: "前提 Δ≥0（有实根）；求「两根之和/之差/平方和」等对称式不用解方程，先转化成 x₁+x₂ 与 x₁x₂ 的组合：x₁²+x₂²=(x₁+x₂)²−2x₁x₂。",
  },
  {
    id: "sm-mean2",
    stage: "secondary",
    subject: "数学",
    topic: "不等式",
    name: "基本（均值）不等式",
    formula: "a+b ≥ 2√(ab)（a,b＞0，当且仅当 a=b 取等号）<br>ab ≤ ((a+b)/2)²",
    notes: "「和定积最大，积定和最小」；用其求最值必须验证取等条件（a=b 能成立）。",
  },
  {
    id: "sm-func",
    stage: "secondary",
    subject: "数学",
    topic: "函数性质",
    name: "一次·二次·反比例函数要点",
    formula:
      "一次 y=kx+b：k>0 增、k<0 减<br>二次 y=ax²+bx+c：顶点 (−b/2a, (4ac−b²)/4a)，对称轴 x=−b/2a，a>0 开口向上<br>反比例 y=k/x：k>0 在一、三象限，k<0 在二、四象限",
    notes: "二次函数配方 y=a(x+h)²+k 是求顶点/最值的通用路径；交点问题联立方程。",
  },
  {
    id: "sm-pyth",
    stage: "secondary",
    subject: "数学",
    topic: "三角形",
    name: "勾股定理",
    formula: "直角三角形：a² + b² = c²（c 为斜边）",
    statement: "直角三角形两直角边的平方和等于斜边的平方。",
    proof: "常见证法：四个全等直角三角形拼大正方形（弦图），用面积关系推出 a²+b²=c²；配图可用 demo 的 geometry 模板（pythagorean）。",
  },
  {
    id: "sm-trisum",
    stage: "secondary",
    subject: "数学",
    topic: "三角形",
    name: "三角形内角和定理",
    formula: "∠A + ∠B + ∠C = 180°",
    statement: "三角形三个内角的和等于 180°；外角等于与它不相邻的两个内角之和。",
    proof: "过顶点作对边的平行线，三个角拼成平角（剪拼演示见配图）。",
    svg:
      '<svg viewBox="0 0 380 200" style="max-width:100%;height:auto">' +
      '<polygon points="60,160 300,160 150,40" fill="#dbe7ff" stroke="#345" stroke-width="2"/>' +
      '<text x="42" y="155" font-size="15">∠B</text><text x="304" y="155" font-size="15">∠C</text><text x="150" y="32" font-size="15">∠A</text>' +
      '<line x1="150" y1="40" x2="150" y2="160" stroke="#e5534b" stroke-width="1.5" stroke-dasharray="5 4"/>' +
      '<text x="160" y="100" font-size="14" fill="#888">高（辅助线）</text>' +
      '<text x="90" y="190" font-size="14" fill="#888">把 ∠A、∠B、∠C 剪下拼在一起，正好拼成 180° 的平角</text></svg>',
  },
  {
    id: "sm-triside",
    stage: "secondary",
    subject: "数学",
    topic: "三角形",
    name: "三角形三边关系",
    formula: "两边之和 > 第三边，两边之差 < 第三边",
    notes: "判断能否构成三角形只需检验「较小两边之和 > 最大边」。",
  },
  {
    id: "sm-special-ang",
    stage: "secondary",
    subject: "数学",
    topic: "三角函数",
    name: "特殊角三角函数值",
    formula:
      "　　sin　cos　tan<br>30°：1/2　√3/2　√3/3<br>45°：√2/2　√2/2　1<br>60°：√3/2　1/2　√3",
    notes: "结合含 30°/45°/60° 的特殊直角三角形（1:√3:2 与 1:1:√2）记忆，别死背表。",
  },
  {
    id: "sm-trig-rel",
    stage: "secondary",
    subject: "数学",
    topic: "三角函数",
    name: "同角三角函数基本关系",
    formula: "sin²α + cos²α = 1，tanα = sinα ÷ cosα",
    notes: "知一求二常用；注意角的范围决定正负号。",
  },
  {
    id: "sm-sine-law",
    stage: "secondary",
    subject: "数学",
    topic: "解三角形",
    name: "正弦定理与余弦定理",
    formula: "a/sinA = b/sinB = c/sinC = 2R<br>a² = b² + c² − 2bc·cosA",
    notes: "已知「两角一边」或「两边及对角」用正弦定理；已知「两边夹角」或「三边」用余弦定理；余弦定理可判断锐角/直角/钝角三角形。",
  },
  {
    id: "sm-circle-angle",
    stage: "secondary",
    subject: "数学",
    topic: "圆",
    name: "圆周角定理",
    formula: "圆周角 = 同弧所对圆心角的一半",
    statement: "同弧或等弧所对的圆周角相等；直径所对的圆周角是直角（90°）。",
    proof: "分圆心在角内部/边上/外部三种情形，都归结为等腰三角形外角性质。",
    svg:
      '<svg viewBox="0 0 380 210" style="max-width:100%;height:auto">' +
      '<circle cx="190" cy="110" r="85" fill="none" stroke="#345" stroke-width="2"/>' +
      '<circle cx="190" cy="110" r="4" fill="#345"/><text x="198" y="106" font-size="14">O</text>' +
      '<polygon points="190,110 110,60 262,52" fill="none" stroke="#e5534b" stroke-width="2"/>' +
      '<polygon points="110,60 262,52 120,178" fill="none" stroke="#4f7cff" stroke-width="2"/>' +
      '<text x="118" y="52" font-size="14" fill="#e5534b">圆心角</text>' +
      '<text x="165" y="120" font-size="14" fill="#4f7cff">圆周角</text>' +
      '<text x="60" y="200" font-size="14" fill="#888">同弧上的圆周角 = 圆心角的一半（红色角是蓝色角的两倍）</text></svg>',
  },
  {
    id: "sm-sector",
    stage: "secondary",
    subject: "数学",
    topic: "圆",
    name: "弧长与扇形面积",
    formula: "l = nπr ÷ 180，S = nπr² ÷ 360 = lr ÷ 2",
    notes: "n 是圆心角度数；扇形占整圆的比例 = n/360，很多题按比例算更直观。",
  },
  {
    id: "sm-stats",
    stage: "secondary",
    subject: "数学",
    topic: "统计",
    name: "平均数·中位数·方差",
    formula: "x̄ = (x₁+x₂+…+xₙ) ÷ n<br>s² = [(x₁−x̄)² + … + (xₙ−x̄)²] ÷ n",
    notes: "方差越小数据越稳定；中位数先排序再取中间（偶数个取中间两数平均）。",
  },

  /* ---------- 物理 ---------- */
  {
    id: "ph-speed-density",
    stage: "secondary",
    subject: "物理",
    topic: "力学基础",
    name: "速度与密度",
    formula: "v = s/t（m/s），ρ = m/V（kg/m³）",
    notes: "1 m/s = 3.6 km/h；密度是物质本身的性质，不随 m、V 改变；单位换算 1 g/cm³ = 1000 kg/m³。",
  },
  {
    id: "ph-pressure",
    stage: "secondary",
    subject: "物理",
    topic: "压强",
    name: "压强（固体与液体）",
    formula: "p = F/S（Pa），液体：p = ρgh",
    notes: "液体压强只与深度和液体密度有关，与容器形状、液重无关；h 是到自由液面的竖直深度。",
  },
  {
    id: "ph-buoyancy",
    stage: "secondary",
    subject: "物理",
    topic: "浮力",
    name: "阿基米德原理",
    formula: "F浮 = ρ液 · g · V排",
    statement: "浸在液体中的物体所受浮力等于它排开液体所受的重力。",
    notes: "V排 是排开液体的体积（浸没时等于物体体积、部分浸入时只是浸入部分）；漂浮时 F浮 = G。轮船从河驶入海，浮力不变、排开体积变小。",
  },
  {
    id: "ph-work-power",
    stage: "secondary",
    subject: "物理",
    topic: "功和能",
    name: "功·功率·机械效率",
    formula: "W = Fs（J），P = W/t = Fv（W），η = W有用/W总 × 100%",
    notes: "功的两要素：有力且沿力方向移动距离；匀速直线时 F 与 v 同向才可用 P=Fv。机械效率永远小于 1（额外功不可避免）。",
  },
  {
    id: "ph-lever",
    stage: "secondary",
    subject: "物理",
    topic: "简单机械",
    name: "杠杆平衡条件",
    formula: "F₁ · L₁ = F₂ · L₂（动力×动力臂 = 阻力×阻力臂）",
    notes: "力臂是支点到力的作用线的垂直距离（不是到作用点！）；省力杠杆费距离，费力杠杆省距离。",
  },
  {
    id: "ph-ohm",
    stage: "secondary",
    subject: "物理",
    topic: "电学",
    name: "欧姆定律与串并联",
    formula: "I = U/R<br>串联：I 处处相等，U=U₁+U₂，R=R₁+R₂<br>并联：U 相等，I=I₁+I₂，1/R=1/R₁+1/R₂",
    notes: "公式对同一段电路使用（I、U、R 必须对应同一导体同一时刻）；并联总电阻小于任一支路电阻。",
  },
  {
    id: "ph-electric-power",
    stage: "secondary",
    subject: "物理",
    topic: "电学",
    name: "电能与电功率·焦耳定律",
    formula: "W = UIt（kW·h=度），P = UI = I²R = U²/R<br>Q = I²Rt（焦耳定律）",
    notes: "额定功率是额定电压下的功率，实际功率随实际电压变化；纯电阻电路 W=Q，非纯电阻（如电动机）W>Q。",
  },
  {
    id: "ph-heat",
    stage: "secondary",
    subject: "物理",
    topic: "热学",
    name: "比热容与热值",
    formula: "Q吸 = cmΔt（升温），Q放 = cm(t₀−t)，燃料：Q = mq",
    notes: "水的比热容大（4.2×10³ J/(kg·℃)）——常考应用：调节气候、做冷却剂；Δt 是温度变化量不是温度。",
  },
  {
    id: "ph-energy",
    stage: "secondary",
    subject: "物理",
    topic: "功和能",
    name: "动能·势能·机械能守恒",
    formula: "E动 = mv²/2，E重势 = mgh",
    statement: "只有动能与势能相互转化（不计摩擦与空气阻力）时，机械能总量保持不变。",
    notes: "动能看质量与速度，重力势能看质量与高度；滚摆/单摆上下摆动是动能与势能相互转化的典型。",
  },

  /* ---------- 化学 ---------- */
  {
    id: "ch-mole",
    stage: "secondary",
    subject: "化学",
    topic: "摩尔",
    name: "物质的量核心公式",
    formula: "n = m/M，n = N/Nᴀ（Nᴀ=6.02×10²³/mol）<br>气体（标况）：V = 22.4 L/mol × n",
    notes: "n 是桥梁：连接质量（g）、微粒个数、气体体积（标况）；计算题先统一单位再代入。",
  },
  {
    id: "ch-solution",
    stage: "secondary",
    subject: "化学",
    topic: "溶液",
    name: "溶质质量分数",
    formula: "w = m质/m液 × 100%（m液 = m质 + m剂）",
    notes: "稀释前后溶质质量不变（m₁w₁ = m₂w₂）；溶解度是特定温度下 100g 水里达到饱和时的质量。",
  },
  {
    id: "ch-equations",
    stage: "secondary",
    subject: "化学",
    topic: "常考方程式",
    name: "常考化学方程式（精选）",
    formula:
      "电解水：2H₂O —通电→ 2H₂↑ + O₂↑<br>实验室制 CO₂：CaCO₃ + 2HCl = CaCl₂ + H₂O + CO₂↑<br>铁与硫酸铜：Fe + CuSO₄ = FeSO₄ + Cu<br>碳酸钠与盐酸：Na₂CO₃ + 2HCl = 2NaCl + H₂O + CO₂↑<br>甲烷燃烧：CH₄ + 2O₂ —点燃→ CO₂ + 2H₂O<br>CO 还原氧化铁：3CO + Fe₂O₃ —高温→ 2Fe + 3CO₂",
    notes: "条件（通电/点燃/高温）、气体与沉淀符号（↑↓）是得分点；配平遵循质量守恒（原子种类与数目不变）。",
  },
  {
    id: "ch-conservation",
    stage: "secondary",
    subject: "化学",
    topic: "守恒定律",
    name: "质量守恒定律",
    statement: "参加化学反应的各物质质量总和，等于反应后生成的各物质质量总和。",
    notes: "微观解释：化学反应前后原子的种类、数目、质量都不变。「六不变、两一定变（物质种类、分子种类）」；有气体参加/逸出时须在密闭容器中验证。",
  },

  /* ---------- 英语 ---------- */
  {
    id: "en-tenses",
    stage: "secondary",
    subject: "英语",
    topic: "时态",
    name: "五大时态结构表",
    formula:
      "一般现在：do / does（三单）—— often, usually, every day<br>现在进行：am/is/are + doing —— now, look!<br>一般过去：did —— yesterday, last week, ago<br>一般将来：will do / be going to do —— tomorrow, next…<br>现在完成：have/has + 过去分词 —— already, yet, ever, since, for",
    notes: "先看时间状语定时态，再看主语定形式（三单、be 动词选择）；since+时间点、for+时间段。",
  },
]

/** 按学科分组的条目索引（索引页渲染用，学科→条目）。 */
export function refIndexBySubject(entries: RefEntry[]): Array<[string, RefEntry[]]> {
  const map = new Map<string, RefEntry[]>()
  for (const e of entries) {
    const list = map.get(e.subject) ?? []
    list.push(e)
    map.set(e.subject, list)
  }
  return [...map.entries()]
}
