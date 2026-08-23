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
    formula: "a/sinA = b/sinB = c/sinC = 2R<br>a² = b² + c² − 2bc·cosA<br>面积 S = ½ab·sinC（已知两边及夹角）",
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

  /* ---------- 小学数学（分数小数/竖式/应用题/统计/简算——快捷知识点补充） ---------- */
  {
    id: "pm-fraction-basic",
    stage: "primary",
    subject: "数学",
    topic: "分数与小数",
    name: "分数的意义与基本性质",
    formula:
      "分数：把单位「1」平均分成若干份，取其中一份或几份<br>基本性质：分子分母同乘（或同除）一个不为 0 的数，分数大小不变<br>约分：化成最简分数（分子分母互质）；通分：化成同分母",
    notes: "比较大小：同分母比分子、同分子分母大的反而小、都不是先通分；带分数 = 整数 + 真分数；假分数可化带分数或整数。",
  },
  {
    id: "pm-fraction-dec",
    stage: "primary",
    subject: "数学",
    topic: "分数与小数",
    name: "分数、小数与百分数互化",
    formula:
      "小数化分数：写成十分之几/百分之几再约分；分数化小数：分子 ÷ 分母<br>小数化百分数：小数点右移两位加 %；百分数化小数：去 % 左移两位<br>小数点移动：右移一位 ×10、右移两位 ×100；左移一位 ÷10",
    notes: "常用值：1/2=0.5=50%，1/4=0.25，3/4=0.75，1/5=0.2，1/8=0.125；最简分数的分母只含质因数 2 和 5 才能化成有限小数。",
  },
  {
    id: "pm-column-rules",
    stage: "primary",
    subject: "数学",
    topic: "竖式计算",
    name: "竖式计算规范",
    formula:
      "加法：相同数位对齐，满十向前一位进 1（标小「1」）<br>减法：不够减向前一位借 1 当 10（标退位点）<br>乘法：用第二个因数每一位分别去乘，积的末位与那一位对齐再相加<br>除法：从最高位除起，每次余数必须比除数小，商与被除数数位对齐",
    notes: "验算：加法逆算减法、乘法交换因数或用除法、除法用商×除数＋余数；抄错数位与漏进位是最常见失分，算完先估算数量级再核对一遍。",
  },
  {
    id: "pm-app-trip",
    stage: "primary",
    subject: "数学",
    topic: "应用题",
    name: "行程问题",
    formula: "路程 = 速度 × 时间（s = vt）<br>相遇：路程和 = 速度和 × 相遇时间<br>追及：路程差 = 速度差 × 追及时间",
    notes: "画线段图是关键——标出出发位置、方向与速度；相向而行速度相加、同向追及速度相减；往返平均速度 = 总路程 ÷ 总时间（不是两次速度的平均）。",
  },
  {
    id: "pm-app-work",
    stage: "primary",
    subject: "数学",
    topic: "应用题",
    name: "工程问题",
    formula: "工作总量 = 工作效率 × 工作时间<br>合作：总量 = 效率和 × 合作时间（常把总量看作 1）",
    notes: "总量设为 1 时效率 = 1/天数；甲 a 天、乙 b 天单独完成，合作需 1÷(1/a+1/b) 天；「先做一段再合作」按剩余量 ÷ 效率和计算。",
  },
  {
    id: "pm-app-typical",
    stage: "primary",
    subject: "数学",
    topic: "应用题",
    name: "鸡兔同笼与植树问题",
    formula:
      "假设全是鸡：兔数 = (脚数 − 2×头数) ÷ 2<br>两端都栽：棵数 = 路长 ÷ 间距 + 1；一端栽/环形：棵数 = 间隔数；两端不栽：棵数 = 间隔数 − 1",
    notes: "鸡兔同笼可用列表法与方程法验证；锯木头、上楼梯、敲钟都是植树模型（次数 = 间隔数相关）；审题先判断两端是否栽树。",
  },
  {
    id: "pm-stat",
    stage: "primary",
    subject: "数学",
    topic: "统计",
    name: "平均数与统计图（小学）",
    formula: "平均数 = 总数量 ÷ 总份数<br>条形图：直观比多少；折线图：看变化趋势；扇形图：看部分占整体的比例",
    notes: "平均数反映整体水平但会被极端数据拉偏；折线图越陡变化越快；可能性大小 = 符合条件的情况数 ÷ 所有等可能情况数。",
  },
  {
    id: "pm-quick-calc",
    stage: "primary",
    subject: "数学",
    topic: "运算律",
    name: "简便计算常见模型",
    formula:
      "凑整：25×4=100、125×8=1000、5×2=10<br>提取公因数：a×c + b×c = (a+b)×c<br>拆分：32×25 = 8×(4×25)；99×a = (100−1)×a = 100a − a",
    notes: "减法性质 a−b−c = a−(b+c)；连除 a÷b÷c = a÷(b×c)；看见相同因数就考虑提取、看见 25/125 就找 4/8 凑整；分配律要「正用反用」都熟。",
  },

  /* ---------- 初中数学（因式分解/二次根式/分式/解法/锐角三角函数/全等相似/切线） ---------- */
  {
    id: "sm-factor",
    stage: "secondary",
    subject: "数学",
    topic: "因式分解",
    name: "因式分解四法",
    formula:
      "提公因式：ma+mb = m(a+b)<br>公式法：a²−b²=(a+b)(a−b)、a²±2ab+b²=(a±b)²<br>十字相乘：x²+(p+q)x+pq = (x+p)(x+q)<br>分组分解：ac+ad+bc+bd = (a+b)(c+d)",
    notes: "顺序：一提（公因式）二套（公式）三十字、四分组；必须分解到每个因式都不能再分；结果是「积」的形式，乘开不是分解。",
  },
  {
    id: "sm-surd-prop",
    stage: "secondary",
    subject: "数学",
    topic: "二次根式",
    name: "二次根式的概念与性质",
    formula: "√a 要求 a≥0；(√a)² = a（a≥0）<br>√(a²) = |a|（a 为任意实数）<br>√(ab) = √a·√b（a≥0，b≥0）；√(a/b) = √a/√b（a≥0，b>0）",
    notes: "被开方数 ≥0 才有意义（分母还需 ≠0）；√(a²)=|a| 是最易错点，字母范围不明必须带绝对值；最简二次根式：被开方数不含分母、不含能开得尽的因数。",
  },
  {
    id: "sm-surd-op",
    stage: "secondary",
    subject: "数学",
    topic: "二次根式",
    name: "二次根式运算与分母有理化",
    formula: "乘除：先乘除被开方数再化简<br>加减：先化最简，再合并同类二次根式（被开方数相同）<br>分母有理化：1/√a = √a/a；1/(√a+√b) = (√a−√b)/(a−b)",
    notes: "√2+√3 不能合并（不是同类）；混合运算顺序与有理数一致；运算结果须化为最简二次根式。",
  },
  {
    id: "sm-fraction-prop",
    stage: "secondary",
    subject: "数学",
    topic: "分式",
    name: "分式的性质与运算",
    formula:
      "有意义：分母 ≠ 0；值为 0：分子 = 0 且分母 ≠ 0<br>基本性质：分子分母同乘（或同除）一个不为 0 的整式，分式值不变<br>乘除：除法乘倒数、先约分再相乘；加减：通分后加减",
    notes: "通分找最简公分母（系数最小公倍数 × 相同字母最高次幂）；混合运算顺序同分数；运算结果化成最简分式。",
  },
  {
    id: "sm-fraction-eq",
    stage: "secondary",
    subject: "数学",
    topic: "分式",
    name: "分式方程的解法与验根",
    formula: "两边同乘最简公分母 → 化为整式方程 → 解出 x<br>验根：代入最简公分母，使公分母 = 0 的是增根（舍去）",
    notes: "增根来自去分母这一步——满足整式方程但使原分母为 0；应用题除验增根还要检验是否符合实际意义。",
  },
  {
    id: "sm-quadratic-methods",
    stage: "secondary",
    subject: "数学",
    topic: "一元二次方程",
    name: "四种解法与选择",
    formula:
      "① 直接开平方：(x+m)² = n（n≥0）→ x = −m±√n<br>② 配方法：化为 (x+m)² = n<br>③ 公式法：x = (−b±√Δ)/2a（万能）<br>④ 因式分解：化为 A·B = 0 → A=0 或 B=0",
    notes: "选择顺序：能开平方/能分解最快，否则公式法；缺一次项用开平方、缺常数项用分解；配方是公式法的来源，也是二次函数求顶点的基本功。",
  },
  {
    id: "sm-sharp-trig",
    stage: "secondary",
    subject: "数学",
    topic: "锐角三角函数",
    name: "锐角三角函数与解直角三角形",
    formula:
      "sinA = 对边/斜边，cosA = 邻边/斜边，tanA = 对边/邻边<br>依据：两锐角互余 + 勾股定理 + 三角函数定义<br>坡度 i = 垂直高度 : 水平宽度 = tanθ；仰角/俯角是与水平线的夹角",
    notes: "对边/邻边是相对所讨论的角而言；已知一边一角或两边即可解直角三角形；非直角三角形作高化归；特殊角数值见「特殊角三角函数值」条目。",
  },
  {
    id: "sm-congruent-similar",
    stage: "secondary",
    subject: "数学",
    topic: "三角形",
    name: "全等与相似判定",
    formula:
      "全等：SSS、SAS、ASA、AAS、HL（直角三角形）<br>相似：两角对应相等 / 两边成比例且夹角相等 / 三边成比例<br>相似性质：对应高、中线、角平分线之比 = 相似比；周长比 = 相似比；面积比 = 相似比²",
    notes: "SSA 不能判定全等（HL 是直角三角形的例外）；书写按对应顶点顺序；「A 字型」「8 字型」是找相似的基本模型；位似是特殊的相似。",
  },
  {
    id: "sm-tangent",
    stage: "secondary",
    subject: "数学",
    topic: "圆",
    name: "切线的判定与性质",
    formula:
      "判定：经过半径外端且垂直于这条半径的直线是圆的切线<br>性质：切线垂直于过切点的半径<br>切线长定理：从圆外一点引的两条切线长相等，该点与圆心连线平分两切线夹角",
    notes: "证切线两路线：有公共点——连半径证垂直；无公共点——作垂线证半径（d=r）；见切点连半径是常用辅助线；三角形内切圆与面积关系 S = r·(周长/2)。",
  },

  /* ---------- 初中物理（力与运动/光与声/物态变化） ---------- */
  {
    id: "ph-newton1",
    stage: "secondary",
    subject: "物理",
    topic: "力与运动",
    name: "牛顿第一定律·惯性·二力平衡·摩擦力",
    formula:
      "牛顿第一定律：不受力（合力为 0）时保持静止或匀速直线运动<br>惯性：一切物体都有，大小只与质量有关<br>二力平衡条件：同体、等大、反向、共线<br>滑动摩擦力大小与压力和接触面粗糙程度有关",
    notes: "惯性不是力——不能说「受到惯性作用」；物体匀速直线/静止时可由二力平衡用一个力求另一个；摩擦力方向与相对运动方向相反，有时反而是动力（人走路）。",
  },
  {
    id: "ph-sound",
    stage: "secondary",
    subject: "物理",
    topic: "光与声",
    name: "声现象",
    formula:
      "产生：振动；传播：需要介质（真空不能传声），15℃ 空气中约 340 m/s<br>三特性：音调—频率、响度—振幅、音色—发声体本身<br>噪声控制：声源处、传播过程中、人耳处",
    notes: "「声音大」指响度、「声音尖/高」常指音调，分清语境；超声波与次声波都听不到；回声测距 s = vt/2（时间除以 2）；振动停止发声停止，但声音可以继续传播。",
  },
  {
    id: "ph-light",
    stage: "secondary",
    subject: "物理",
    topic: "光与声",
    name: "光的反射折射与透镜成像",
    formula:
      "反射定律：三线共面、法线居中、反射角=入射角<br>平面镜成像：等大、等距、虚像、左右相反<br>折射：斜射入另一介质方向改变、垂直入射方向不变<br>凸透镜：u>2f 倒立缩小实像（照相机）；f<u<2f 倒立放大实像（投影仪）；u<f 正立放大虚像（放大镜）",
    notes: "入射角/反射角是与法线的夹角（不是与镜面）；镜面反射与漫反射都遵守反射定律；一倍焦距分虚实、二倍焦距分大小；光路可逆；白光经三棱镜色散成七色光。",
  },
  {
    id: "ph-state-change",
    stage: "secondary",
    subject: "物理",
    topic: "热学",
    name: "物态变化",
    formula:
      "熔化（吸）↔凝固（放）；汽化（吸：蒸发/沸腾）↔液化（放）；升华（吸）↔凝华（放）<br>晶体有固定熔点（冰 0℃），熔化过程温度不变；非晶体没有熔点<br>沸腾条件：达到沸点且继续吸热，沸腾中温度不变",
    notes: "「白气」不是水蒸气，是水蒸气液化成的小水滴；影响蒸发快慢：液体温度、表面积、空气流动；晶体熔化/水沸腾图象都有水平段（吸热温度不变）；干冰升华致冷用于人工降雨。",
  },

  /* ---------- 初中化学（金属/酸碱盐/溶解度） ---------- */
  {
    id: "ch-metal",
    stage: "secondary",
    subject: "化学",
    topic: "金属",
    name: "金属活动性顺序与应用",
    formula:
      "K Ca Na Mg Al Zn Fe Sn Pb (H) Cu Hg Ag Pt Au（钾钙钠镁铝锌铁锡铅（氢）铜汞银铂金）<br>氢前金属能与稀酸反应放出 H₂；前面的金属能把后面的金属从其盐溶液中置换出来<br>工业炼铁：3CO + Fe₂O₃ —高温→ 2Fe + 3CO₂",
    notes: "K/Ca/Na 会先与水反应，不能直接置换盐溶液中的金属；滤液滤渣题按反应先后逐个推理；铁生锈需氧气与水同时参与，防锈即断其一（刷漆、镀层、制合金、保持干燥）。",
  },
  {
    id: "ch-acid-base",
    stage: "secondary",
    subject: "化学",
    topic: "酸碱盐",
    name: "常见的酸与碱",
    formula:
      "盐酸 HCl、硫酸 H₂SO₄（浓硫酸吸水可做干燥剂）；NaOH（潮解、溶解放热）、Ca(OH)₂<br>指示剂：石蕊遇酸红遇碱蓝；酚酞遇碱红、遇酸不变色<br>pH：<7 酸性、=7 中性、>7 碱性<br>中和反应：酸 + 碱 → 盐 + 水（放热）",
    notes: "酸使酚酞不变色、使石蕊变红——两处别记反；pH 试纸不能用水湿润（测酸性偏大、碱性偏小）；浓硫酸沾皮肤先用干布擦再大量水冲，稀释时酸入水沿器壁缓慢倒入并搅拌；改良酸性土壤用熟石灰。",
  },
  {
    id: "ch-salt",
    stage: "secondary",
    subject: "化学",
    topic: "酸碱盐",
    name: "盐的性质与复分解反应",
    formula:
      "复分解反应发生条件：生成物中有沉淀、气体或水<br>常见沉淀：BaSO₄、AgCl（不溶于稀硝酸）；CaCO₃、BaCO₃（溶于酸）；Mg(OH)₂、Cu(OH)₂（蓝色）、Fe(OH)₃（红褐色）<br>检验：Cl⁻ 用 AgNO₃ + 稀硝酸（白色沉淀）；SO₄²⁻ 用含 Ba²⁺ 溶液 + 稀硝酸",
    notes: "除杂原则：不引入新杂质、不消耗主体、操作可行；碳酸盐检验：加稀酸生成使澄清石灰水变浑浊的气体；化肥记忆：氮促叶、磷促根（抗旱抗寒）、钾促茎（抗倒伏）。",
  },
  {
    id: "ch-solubility",
    stage: "secondary",
    subject: "化学",
    topic: "溶液",
    name: "溶解度与溶解度曲线",
    formula:
      "溶解度：一定温度下 100g 水达到饱和时溶解的溶质质量（g）<br>曲线交点：该温度下两物质溶解度相等<br>陡升型（如 KNO₃）适合降温结晶；缓升型（如 NaCl）适合蒸发结晶",
    notes: "曲线上方饱和、下方不饱和；Ca(OH)₂ 溶解度随温度升高而减小（特殊）；气体溶解度随温度升高减小、随压强增大增大；饱和与不饱和转化：加溶剂/升温 ↔ 加溶质/蒸发/降温。",
  },

  /* ---------- 高中数学 ---------- */
  {
    id: "sm-func-hs",
    stage: "secondary",
    subject: "数学",
    topic: "函数性质",
    name: "高中函数性质（单调·奇偶·周期）",
    formula:
      "单调性：x₁<x₂ ⇒ f(x₁)<f(x₂) 为增（定义法）；可导时 f′>0 增、f′<0 减<br>奇偶性：定义域关于原点对称为前提；f(−x)=f(x) 偶（关于 y 轴对称）、f(−x)=−f(x) 奇（关于原点对称）<br>周期：f(x+T)=f(x)；常用 f(x+a)=−f(x) ⇒ T=2a<br>复合函数单调性：同增异减",
    notes: "判断奇偶先看定义域是否对称；抽象函数用赋值法；f(a+x)=f(a−x) ⇒ 对称轴 x=a；单调性用于比大小、解抽象不等式（同增异减脱去 f）与最值。",
  },
  {
    id: "sm-trig-ident",
    stage: "secondary",
    subject: "数学",
    topic: "三角函数",
    name: "诱导公式与和差倍角公式",
    formula:
      "诱导公式：奇变偶不变、符号看象限（k·90°±α）<br>和差：sin(α±β)=sinαcosβ±cosαsinβ；cos(α±β)=cosαcosβ∓sinαsinβ；tan(α±β)=(tanα±tanβ)/(1∓tanαtanβ)<br>二倍角：sin2α=2sinαcosα；cos2α=cos²α−sin²α=2cos²α−1=1−2sin²α<br>辅助角：a sinx + b cosx = √(a²+b²)·sin(x+φ)，tanφ = b/a",
    notes: "降幂公式 sin²α=(1−cos2α)/2、cos²α=(1+cos2α)/2；给值求值先找已知角与目标角的关系（和差/倍半/互余）；化简方向：切化弦、异名化同名、异角化同角。",
  },
  {
    id: "sm-trig-graph",
    stage: "secondary",
    subject: "数学",
    topic: "三角函数",
    name: "正弦余弦图象与 y=Asin(ωx+φ)",
    formula:
      "y=sinx：值域 [−1,1]、周期 2π、奇函数；增区间 [−π/2+2kπ, π/2+2kπ]<br>y=cosx：偶函数、周期 2π<br>y=Asin(ωx+φ)：振幅 A、周期 T=2π/ω、初相 φ<br>图象变换：y=sinx 向左(+)右(−)平移 |φ| → 横坐标 ×1/ω → 纵坐标 ×A",
    notes: "五点法取 ωx+φ = 0、π/2、π、3π/2、2π；求单调区间把 ωx+φ 整体放入 sinx 的单调区间（注意 ω>0，否则先用诱导公式化正）；由图象反求：A 看最高点、ω 由周期反求、φ 代最值点。",
  },
  {
    id: "sm-seq-ap",
    stage: "secondary",
    subject: "数学",
    topic: "数列",
    name: "等差数列",
    formula:
      "通项 aₙ = a₁ + (n−1)d；推广 aₙ = aₘ + (n−m)d<br>求和 Sₙ = n(a₁+aₙ)/2 = na₁ + n(n−1)d/2<br>等差中项 2A = a+b；若 m+n=p+q 则 aₘ+aₙ = aₚ+a_q",
    notes: "aₙ 是 n 的一次函数（d 为斜率）——由两点可求；Sₙ 是 n 的二次式（无常数项）；五量 a₁、d、n、aₙ、Sₙ 知三求二；Sₙ/n = a₁+(n−1)d/2 仍是等差。",
  },
  {
    id: "sm-seq-gp",
    stage: "secondary",
    subject: "数学",
    topic: "数列",
    name: "等比数列与求和方法",
    formula:
      "通项 aₙ = a₁·qⁿ⁻¹（a₁、q 均不为 0）<br>求和：q=1 时 Sₙ = na₁；q≠1 时 Sₙ = a₁(1−qⁿ)/(1−q)<br>aₙ 与 Sₙ：n≥2 时 aₙ = Sₙ − Sₙ₋₁（n=1 单独验证 a₁ = S₁）<br>错位相减（等差×等比型）；裂项相消：1/[n(n+1)] = 1/n − 1/(n+1)",
    notes: "等比中项满足 a² = bc（a、b、c 同号）；用求和公式必须先讨论 q=1；错位相减相减后别漏掉最后一项；Sn 已知求 an 时 n=1 的验证是得分点。",
  },
  {
    id: "sm-quad-ineq",
    stage: "secondary",
    subject: "数学",
    topic: "不等式",
    name: "一元二次与分式不等式解法",
    formula:
      "ax²+bx+c>0（a>0，Δ>0）：大于取两边（x<x₁ 或 x>x₂）、小于取中间<br>含等号则带上根；Δ≤0 时结合开口与端点讨论<br>分式不等式：f(x)/g(x)>0 ⇔ f(x)·g(x)>0；f(x)/g(x)≥0 ⇔ f·g≥0 且 g(x)≠0",
    notes: "先把二次项系数化正；分式不等式严禁两边直接乘分母（符号未知）；恒成立问题：参变分离或用根的分布（开口方向 + 判别式 + 端点符号）。",
  },
  {
    id: "sm-deriv",
    stage: "secondary",
    subject: "数学",
    topic: "导数",
    name: "导数公式与应用步骤",
    formula:
      "公式：C′=0，(xⁿ)′=nxⁿ⁻¹，(sinx)′=cosx，(cosx)′=−sinx，(eˣ)′=eˣ，(aˣ)′=aˣlna，(lnx)′=1/x，(logₐx)′=1/(x·lna)<br>法则：(uv)′=u′v+uv′，(u/v)′=(u′v−uv′)/v²，复合 [f(g(x))]′=f′(g(x))·g′(x)<br>切线：y = f(x₀) + f′(x₀)(x−x₀)",
    notes: "单调区间解 f′>0 / f′<0；极值点须导数为 0 且两侧变号；闭区间最值比较极值与端点值；含参讨论按临界值分区间；恒成立常转化为 a ≥ f(x)max 或 a ≤ f(x)min。",
  },
  {
    id: "sm-line-circle",
    stage: "secondary",
    subject: "数学",
    topic: "解析几何",
    name: "直线与圆",
    formula:
      "点斜式 y−y₀=k(x−x₀)；一般式 Ax+By+C=0（k = −A/B）<br>点到直线距离 d = |Ax₀+By₀+C| / √(A²+B²)<br>圆：(x−a)²+(y−b)²=r²；一般式 x²+y²+Dx+Ey+F=0<br>线圆位置：d>r 相离、d=r 相切、d<r 相交；弦长 = 2√(r²−d²)",
    notes: "斜率不存在的直线（x=常数）必须单独讨论；两圆位置看圆心距与 r₁+r₂、|r₁−r₂| 的关系；过圆内一点的最短弦与该点和圆心连线垂直；相切时圆心到直线距离 = r 建方程。",
  },
  {
    id: "sm-conic",
    stage: "secondary",
    subject: "数学",
    topic: "解析几何",
    name: "圆锥曲线（椭圆·双曲线·抛物线）",
    formula:
      "椭圆 x²/a²+y²/b²=1（a>b>0）：c²=a²−b²，e=c/a<1<br>双曲线 x²/a²−y²/b²=1：c²=a²+b²，e=c/a>1，渐近线 y=±(b/a)x<br>抛物线 y²=2px（p>0）：焦点 (p/2, 0)、准线 x=−p/2<br>弦长 |AB| = √(1+k²)·|x₁−x₂|；点差法（中点弦）：k = −b²x₀/(a²y₀)",
    notes: "定义优先：椭圆到两焦点距离之和 = 2a、双曲线之差 = 2a、抛物线到焦点距离 = 到准线距离；联立方程 + 韦达定理「设而不求」是通法；焦点三角形结合余弦定理；离心率问题找 a、c 的齐次不等式。",
  },
  {
    id: "sm-solid-line-plane",
    stage: "secondary",
    subject: "数学",
    topic: "立体几何",
    name: "线面平行与垂直（判定与性质）",
    formula:
      "线面平行判定：平面外一条线与平面内一条线平行 ⇒ 线∥面；性质：线∥面 ⇒ 线与过它的截面交线平行<br>线面垂直判定：垂直于平面内两条相交直线；性质：垂直于同一平面的两条直线平行<br>面面平行：一平面内两条相交线平行于另一平面；面面垂直：面内有直线垂直于交线（性质：两面垂直时，一面内垂直交线的直线垂直另一面）",
    notes: "证明题主线是「线线 ⇄ 线面 ⇄ 面面」互推；找垂线常用三线合一（等腰底边中线）、勾股逆定理、面面垂直性质；中点连中点出中位线（平行）是高频辅助线。",
  },
  {
    id: "sm-solid-vector",
    stage: "secondary",
    subject: "数学",
    topic: "立体几何",
    name: "空间向量法与三种角",
    formula:
      "法向量：n·AB = 0 且 n·AC = 0（设分量解方程组）<br>异面直线所成角：cosθ = |cos⟨a,b⟩|<br>线面角：sinθ = |cos⟨a,n⟩|<br>二面角：cosθ = ±cos⟨n₁,n₂⟩（按图形定正负）<br>点到平面距离 d = |AB·n| / |n|",
    notes: "向量法流程：建系（原点选三线交汇处）→ 写坐标 → 求法向量 → 套公式；线线角带绝对值、二面角不带（由图判断锐钝）；传统几何法（作角证角）与向量法互为校验。",
  },
  {
    id: "sm-comb",
    stage: "secondary",
    subject: "数学",
    topic: "概率统计",
    name: "排列组合常用方法",
    formula:
      "分类加法原理、分步乘法原理<br>排列 Aₙᵐ = n!/(n−m)!（有序）；组合 Cₙᵐ = n!/[m!(n−m)!]（无序）；Cₙᵐ = Cₙⁿ⁻ᵐ<br>捆绑法（相邻）、插空法（不相邻）、隔板法（相同元素分组）、正难则反",
    notes: "先分类后分步、先选（组合）后排（排列）；「至少/至多」优先考虑补集；平均分组要除以组数的全排列防重复；涂色、排座位、分组分配是三大经典模型。",
  },
  {
    id: "sm-prob-dist",
    stage: "secondary",
    subject: "数学",
    topic: "概率统计",
    name: "概率分布与期望方差",
    formula:
      "古典概型 P = 事件包含的结果数 / 总结果数<br>条件概率 P(B|A) = P(AB)/P(A)；相互独立 P(AB) = P(A)P(B)<br>二项分布 X~B(n,p)：P(X=k) = Cₙᵏpᵏ(1−p)ⁿ⁻ᵏ，E=np，D=np(1−p)<br>E(X) = Σxᵢpᵢ；D(X) = E(X²) − (EX)²；E(aX+b)=aE+b，D(aX+b)=a²D<br>正态分布：关于 μ 对称，σ 越小数据越集中",
    notes: "分布列先自查「概率和 = 1」；独立重复试验的判定是「同一试验重复 n 次、每次概率不变」；正态概率计算用对称转化 P(X<μ)=0.5；期望线性性质可拆 E(X+Y)=EX+EY。",
  },

  /* ---------- 高中物理 ---------- */
  {
    id: "ph-newton-hs",
    stage: "secondary",
    subject: "物理",
    topic: "力学",
    name: "牛顿三定律与受力分析（力学综合）",
    formula:
      "牛顿第二定律 F合 = ma（合力与加速度瞬时对应）<br>牛顿第三定律：作用力与反作用力等大、反向、异体、同性质<br>超重：加速度向上（视重 > mg）；失重：加速度向下；完全失重 a = g<br>整体法求外力/加速度，隔离法求内力",
    notes: "受力分析顺序：重力→弹力→摩擦力→其他力；建轴沿加速度方向正交分解；连接体问题先整体后隔离；两类动力学问题：已知力求运动（先求 a 再用运动学）、已知运动求力（先反推 a）。",
  },
  {
    id: "ph-circle-grav",
    stage: "secondary",
    subject: "物理",
    topic: "力学",
    name: "圆周运动与万有引力",
    formula:
      "向心加速度 a = v²/r = ω²r；向心力 F = mv²/r = mω²r = 4π²mr/T²<br>万有引力 F = GMm/r²；卫星环绕 GMm/r² = mv²/r ⇒ v = √(GM/r)<br>第一宇宙速度 7.9 km/s（最大环绕速度 = 最小发射速度）<br>黄金代换 GM = gR²；开普勒第三定律 T² ∝ r³",
    notes: "向心力是效果力——由重力/弹力/摩擦力提供，受力分析切勿再另加；最高点临界：绳/内轨模型 v≥√(gr)，杆/双轨模型 v 可为 0；赤道上随地球自转的物体不是卫星（向心力来自万有引力与支持力之差）。",
  },
  {
    id: "ph-field-circuit",
    stage: "secondary",
    subject: "物理",
    topic: "电磁学",
    name: "电场与闭合电路",
    formula:
      "场强 E = F/q（定义式）；点电荷 E = kQ/r²<br>电势差 U = W/q；匀强场 E = U/d；电容 C = Q/U<br>闭合电路 E = U外 + Ir；电源总功率 P=EI、输出功率 P出=UI、效率 η=U/E<br>串联分压、并联分流；U-I 图象斜率大小 = 内阻 r",
    notes: "电势沿电场线方向降低；正电荷受力方向与场强同向；输出功率最大条件 R=r（P=E²/4r）；非纯电阻电路（电动机）欧姆定律不适用，用能量守恒 UI = I²R + P机械；电容器在直流电路中稳定后断路（串联段无电流）。",
  },
  {
    id: "ph-mag-force",
    stage: "secondary",
    subject: "物理",
    topic: "电磁学",
    name: "磁场与安培力·洛伦兹力",
    formula:
      "安培力 F = BIL（B⊥I 时最大，左手定则判方向）<br>洛伦兹力 f = qvB（方向始终垂直速度，对电荷不做功）<br>磁场中圆周运动：qvB = mv²/r ⇒ 半径 r = mv/(qB)，周期 T = 2πm/(qB)（与速率无关）",
    notes: "左手定则：磁感线穿掌心、四指指正电荷运动方向（电流方向），拇指即力方向（负电荷四指反向）；速度选择器 qE=qvB 做匀速直线运动；回旋加速器电场加速、磁场偏转，交变电压周期 = 圆周周期。",
  },

  /* ---------- 高中化学 ---------- */
  {
    id: "ch-mole-c",
    stage: "secondary",
    subject: "化学",
    topic: "摩尔",
    name: "物质的量浓度",
    formula:
      "c(B) = n(B) / V（mol/L），V 为溶液体积（不是溶剂体积）<br>稀释/混合守恒：c₁V₁ = c₂V₂（溶质物质的量不变）<br>与质量分数换算：c = 1000ρw/M（ρ 单位 g/mL）",
    notes: "容量瓶配制流程：计算→称量→溶解冷却→转移洗涤→定容摇匀；误差分析看 n 与 V 的相对变化：俯视定容 V 偏小 c 偏大、未洗涤烧杯 n 偏小 c 偏小；气体溶于水后体积按溶液算。",
  },
  {
    id: "ch-organic-fg",
    stage: "secondary",
    subject: "化学",
    topic: "有机化学",
    name: "官能团与典型反应",
    formula:
      "碳碳双键 C=C：加成、加聚（使溴水与酸性 KMnO₄ 褪色）<br>羟基 −OH：与 Na 放 H₂、催化氧化成醛、与羧酸酯化<br>醛基 −CHO：银镜反应（水浴）、与新制 Cu(OH)₂ 加热出砖红色沉淀、加氢还原成醇<br>羧基 −COOH：酸的通性、与醇酯化；酯基 −COO−：水解（酸性可逆、碱性彻底）",
    notes: "鉴别链：溴水褪色→含 C=C；银镜→含醛基；加 Na₂CO₃ 冒气→含羧基；官能团决定性质，同系物官能团相同、组成相差 CH₂；乙醇催化氧化、乙酸乙酯制备是必考实验。",
  },
  {
    id: "ch-organic-rx",
    stage: "secondary",
    subject: "化学",
    topic: "有机化学",
    name: "有机反应类型与同分异构体",
    formula:
      "取代（卤代/硝化/酯化/水解）、加成（烯/炔 + H₂、H₂O、HX）、氧化（催化氧化、使酸性 KMnO₄ 褪色）、还原、消去（醇→烯）<br>同分异构：碳骨架异构、位置异构、官能团异构（如 C₂H₆O：乙醇与二甲醚）",
    notes: "同分异构书写按「碳骨架 → 不饱和度 → 官能团位置」有序枚举防漏防重；等效氢判断一元取代产物种数；醇消去需羟基碳的邻位有氢；加聚/缩聚由重复单元反推单体。",
  },
  {
    id: "ch-charge-electron",
    stage: "secondary",
    subject: "化学",
    topic: "守恒定律",
    name: "电荷守恒与电子守恒",
    formula:
      "电荷守恒：溶液中阳离子正电荷总数 = 阴离子负电荷总数<br>（如 Na₂CO₃ 溶液：c(Na⁺)+c(H⁺) = 2c(CO₃²⁻)+2c(HCO₃⁻)+c(OH⁻)）<br>电子守恒：氧化剂得电子总数 = 还原剂失电子总数<br>三大守恒联用：物料守恒 + 电荷守恒 + 质子守恒",
    notes: "离子浓度比较写「两个守恒 + 电离/水解程度」；电子守恒直接列得失电子相等（如向 FeBr₂ 溶液通 Cl₂：少量先氧化 Fe²⁺，过量再氧化 Br⁻）；守恒法绕开中间过程，是快速计算与配平的统一依据。",
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
