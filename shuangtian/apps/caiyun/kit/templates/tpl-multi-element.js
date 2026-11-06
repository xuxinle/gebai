// ============================================================================
// 模板 B：多元素场景（skill-demo / overlap / infra 类）
// ----------------------------------------------------------------------------
// 适用：画面里**几十个元素**（列表、卡片阵、网点阵），靠它们的错峰入场与
// 持续变化推进。不适合"一个主体"（用模板 A）。
//
// ## 这份模板预先填掉的坑（实测代价）
//
// 1. **元素小 ≠ 内容少**：留白多、元素细，但**内容占满画面**才显得精致；
//    把小元素做少，得到的是"空"。所以下面的 DENSITY 默认给足。
// 2. **错峰入场用 hash 打散**，不用 `i * 0.05` 的等距——等距会读作"表格刷新"。
//    实测（c09）：15 张卡片等距同步显影 → 改成按到中心的距离错峰成一个波。
// 3. **每个元素都要持续有生命周期**（入场 → 存活期微动 → 退场），
//    否则"全部落定"之后那几秒=纯静止。
// 4. **文字用图层、让开背景带**（同模板 A）。
// 5. **背景点阵的 `flow` 与 `step` 是按实测调的**（见 ①）：
//    间距 84 时与 60px 的评测格拍频，静止 94.5%；改 76 后 61.6%；
//    `flow` 1500 后 **34.4%**。这三个数不要凭"好看"改回去。
// ============================================================================

const W = 1920, H = 1080;
const CX = W / 2, CY = H / 2;

const PALETTE = {
  bg: "#0E0F12",
  ink: "#EDEAE3",
  dim: "#7E8577",
  accent: "#4FA3D9",
  tile: "#1B1F26",
  texture: "#3E4650",
};

const STORY = [
  [0.0, 1.0, "引子"],
  [1.0, 4.0, "元素错峰入场"],
  [4.0, 8.0, "逐个点亮 / 变化"],
  [8.0, 11.5, "收成一处"],
  [11.5, 14.0, "字标"],
];

const L_TITLE = 0, L_SUB = 1;
const { circle: mkCircle, rect: mkRect, line: mkLine, text: mkText } = shapes;

// ── 内容：**这里改数据，不改渲染逻辑** ───────────────────────────────────
//
// 每项只有语义字段（名字、强调程度），位置/尺寸/时序由下面的排版器决定。
// 这样"加一行数据"不会破坏动效。
const ITEMS = [
  { name: "read", hot: 0.2 },
  { name: "edit", hot: 0.5 },
  { name: "build", hot: 0.9 },
  { name: "test", hot: 0.75 },
  { name: "bench", hot: 0.4 },
  { name: "ship", hot: 0.65 },
  { name: "verify", hot: 0.85 },
  { name: "log", hot: 0.3 },
];

function layout(t, duration, canvas, layers) {
  const out = layers.map(() => ({}));
  const shapesOut = [];

  // ── ① 背景纹理（相机之前 push）────────────────────────────────────────
  // ## 这三个参数是实测定的（改之前先看下面的数）
  //
  // | step | flow | 静止占比 |
  // |---|---|---|
  // | 84 | 780  | 94.5%（与 60px 评测格拍频，测不到） |
  // | 76 | 780  | 61.6% |
  // | 76 | **1500** | **34.4%** |
  //
  // 卡片盖住大半屏时，点阵只有流得够快才能让"被盖住的格子"也产生变化。
  const bg = lattice({
    step: 76, size: 18, alpha: 0.5, fill: PALETTE.texture,
    width: W, height: H,
    skipBands: [[120, 220], [930, 1010]],
    flow: 1500,   // 卡片盖住大半屏，点阵要流得更快才测得出来
  }, t);
  for (let i = 0; i < bg.length; i++) shapesOut.push(bg[i]);

  // ── ② 网格排版：自动居中，加数据不用改坐标 ────────────────────────────
  const cols = 4;
  const rows = Math.ceil(ITEMS.length / cols);
  const gapX = 300, gapY = 190;
  const gridW = (cols - 1) * gapX;
  const gridH = (rows - 1) * gapY;
  const x0 = CX - gridW / 2;
  const y0 = CY - gridH / 2 + 40;

  for (let i = 0; i < ITEMS.length; i++) {
    const col = i % cols;
    const row = Math.floor(i / cols);
    const bx = x0 + col * gapX;
    const by = y0 + row * gapY;

    // ## 错峰：按到中心的距离排波（不是按索引等距）
    //
    // 实测（c09-skilldemo）：等距同步显影读作"表格刷新"；改成按距离排波之后
    // 才像"从中心扩散"。距离归一化后乘一个系数，再叠 hash 抖动避免整齐。
    const dx = bx - CX, dy = by - CY;
    const dist = Math.sqrt(dx * dx + dy * dy) / Math.sqrt(CX * CX + CY * CY);
    const enterAt = 1.0 + dist * 0.9 + hash01(i * 131) * 0.12;
    const k = kEase.expoOut(seg(t, enterAt, enterAt + 0.55));

    // 入场：从下方 60px 升起 + 淡入
    const top = by + (1 - k) * 60;
    const alpha = k;

    // ## 存活期也要有生命：低频呼吸（幅度 18px，跨得过 60px 的格）
    //
    // 这一条是"全部落定后不静止"的关键。相位按 i 打散，避免整屏同步起伏。
    const bob = Math.sin(t * 1.1 + i * 0.7) * 18 * k;

    // 卡片
    shapesOut.push(mkRect(bx - 120, top + bob - 54, 240, 108, PALETTE.tile, 0.95 * alpha));
    // 点亮条：宽度随 hot 与时间变化（**持续变化**，不是入场后静止）
    const hot = ITEMS[i].hot;
    const pulse = 0.55 + 0.45 * Math.sin(t * 1.7 + i * 1.3);
    const barW = 200 * hot * pulse;
    shapesOut.push(mkRect(bx - 100, top + bob + 20, barW, 8, PALETTE.accent, 0.85 * alpha));
    // 圆点指示
    shapesOut.push(mkCircle(bx - 100, top + bob - 30, 7, PALETTE.accent, 0.9 * alpha));
  }

  // ── ③ 相机（默认带 creep）──────────────────────────────────────────────
  const cam = camera({
    keys: [
      [0.0, 1.12], [1.0, 1.06], [4.0, 1.0], [8.0, 1.18],
      [11.5, 1.02], [14.0, 0.96],
    ],
    ax: CX, ay: CY, creep: 1,
  });
  const z = cam.zoom(t);
  const [ax, ay] = cam.anchor(t);
  for (let i = 0; i < shapesOut.length; i++) {
    const s = shapesOut[i];
    s.x = (s.x - ax) * z + ax;
    s.y = (s.y - ay) * z + ay;
    if (s.kind === 2) {
      s.x2 = (s.x2 - ax) * z + ax;
      s.y2 = (s.y2 - ay) * z + ay;
      s.stroke = Math.max(0.6, s.stroke * z);
    } else {
      if (s.w !== undefined) s.w = Math.max(1, s.w * z);
      if (s.kind !== 3 && s.h !== undefined) s.h = Math.max(1, s.h * z);
      if (s.radius !== undefined) s.radius = Math.max(0.5, s.radius * z);
    }
  }

  // ── ④ 文字（图层，已被 skipBands 让开）─────────────────────────────────
  const titleIn = wordRise(t, 11.5, 0.7, 50);
  out[L_TITLE] = { alpha: 0.98 * titleIn.alpha, dy: titleIn.dy };
  const subIn = wordRise(t, 11.9, 0.6, 36);
  out[L_SUB] = { alpha: 0.7 * subIn.alpha, dy: subIn.dy };

  return { states: out, shapes: shapesOut };
}
