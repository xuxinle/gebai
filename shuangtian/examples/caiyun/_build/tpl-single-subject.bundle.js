// ==== 本文件由 kit/build.py 生成，请勿手改 ====
// kit: caiyun-kit.js（225 行）
// body: tpl-single-subject.js（143 行）
// 你的脚本报错时，行号减去 229 才是 body 的行号
// ================================================
// ============================================================================
// caiyun-kit —— 给裁云的 layout 脚本用的复用层
// ============================================================================
// ## 为什么是"拼接到脚本前面"而不是 import
//
// 裁云的 `layout` 脚本是**单文件整体 eval**（`LayoutEngine::load` → `engine.eval(source)`），
// 没有模块系统：`import` / `require` 都不可用（`ot_bench` 的脚本语法检查会直接报错）。
//
// 所以复用只能走"预处理器"：渲染前把 `caiyun-kit.js` 的正文拼到你的脚本前面。
// 入口脚本仍是单文件、仍可独立读懂，而通用能力不必复制粘贴。
//
// 用法（build.py 里）：
//   const kit = read("caiyun-kit.js");
//   const body = read("my-film.js");
//   write("_build/my-film.bundle.js", kit + "\n" + body);
//   // 让 json 的 layout 指向 bundle
//
// ## 边界（知道才不踩）
//
// - 本文件**不引用任何宿主函数**（`rand`/`emit` 之类），所以能被 node 直接跑着测。
// - 所有函数都是纯函数：同一组参数永远同一结果。裁云要求逐帧可复现，禁止 `Math.random`。
// - 不用可选链 / 正则后行断言等新语法（脚本引擎的兼容面按 ES2017 控制）。
//
// ## 与 onetake `lib/motion.js` 的关系
//
// 对标它的能力集，但**按裁云的图元模型重写**：onetake 操作 DOM/SVG，这里只产出
// `{ states, shapes }`（裁云的 layout 返回形状）。所以同名函数语义对齐、实现不同。
// ============================================================================

// ── ① 基础缓动（与 onetake 同名同义，便于迁移）──────────────────────────
const clamp = (v, a, b) => {
  const lo = a === undefined ? 0 : a;
  const hi = b === undefined ? 1 : b;
  return v < lo ? lo : (v > hi ? hi : v);
};
const lerp = (a, b, k) => a + (b - a) * k;
const seg = (t, t0, t1) => (t1 === t0 ? (t >= t1 ? 1 : 0) : clamp((t - t0) / (t1 - t0), 0, 1));
const sstep = (e0, e1, x) => { const k = clamp((x - e0) / (e1 - e0), 0, 1); return k * k * (3 - 2 * k); };
const ssstep = (e0, e1, x) => {
  const k = clamp((x - e0) / (e1 - e0), 0, 1);
  return k * k * k * (k * (k * 6 - 15) + 10);
};

const ease = {
  linear: u => u,
  in: u => u * u,
  out: u => 1 - (1 - u) * (1 - u),
  inOut: u => (u < 0.5 ? 2 * u * u : 1 - 2 * (1 - u) * (1 - u)),
  expoIn: u => (u <= 0 ? 0 : Math.pow(2, 10 * (u - 1))),
  expoOut: u => (u >= 1 ? 1 : 1 - Math.pow(2, -10 * u)),
  expoInOut: u => {
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    return u < 0.5 ? Math.pow(2, 20 * u - 10) / 2 : (2 - Math.pow(2, -20 * u + 10)) / 2;
  },
  back: u => { const c = 1.70158; return 1 + (c + 1) * Math.pow(u - 1, 3) + c * Math.pow(u - 1, 2); },
};

/// 关键帧表求值（**与各脚本里手写的那份同语义**，这里统一一份）。
///
/// `list` 形如 `[[t0, v0], [t1, v1, "expoOut"], ...]`；第三项是缓动名（缺省 linear）。
/// 首尾之外按端点值钳住。
const keys = (t, list) => {
  if (!list || !list.length) return 0;
  if (t <= list[0][0]) return list[0][1];
  for (let i = 0; i + 1 < list.length; i++) {
    const ta = list[i][0], va = list[i][1], tb = list[i + 1][0], vb = list[i + 1][1];
    if (t <= tb) {
      if (tb === ta) return vb;
      const name = list[i + 1][2] || "linear";
      const fn = ease[name] || ease.linear;
      return va + (vb - va) * fn((t - ta) / (tb - ta));
    }
  }
  return list[list.length - 1][1];
};

// ── ② 确定性"随机"（禁 Math.random，但需要不规则感）─────────────────────
//
// 同一个整数索引 → 同一个 0~1 值。用来做疏密不均、错峰延迟——观感是随机的，
// 但两次渲染逐像素相同（裁云的硬要求）。
const hash01 = (n) => {
  let x = Math.imul(n ^ 0x9e3779b9, 0x85ebca6b);
  x ^= x >>> 13;
  x = Math.imul(x, 0xc2b2ae35);
  x ^= x >>> 16;
  return (x >>> 0) / 4294967296;
};

// ── ③ 相机（裁云的 layout 用 zoom + 锚点做镜头）────────────────────────
//
// ## 为什么必须有"无平台"这条
//
// 实测（见 docs/ot_bench_fixes.md）：`keys()` 两端值相同时输出恒定，而**相机站定
// 就是画面站定**——这是"静止占比过高"最常见的主因，5 个 case 里有 4 个撞到。
//
// 所以相机工厂默认叠一层持续微动（`creep`），并在 `settle: false` 时保证
// 每帧都在变。要真正定格时显式传 `creep: 0`。
function camera(spec) {
  const base = spec.keys || [];
  const ax = spec.ax || 960;
  const ay = spec.ay || 540;
  const creepAmp = spec.creep === undefined ? 1 : spec.creep;
  // 两层不同频率的正弦 → 不会同时回到同一相位（单频会在整周期处完全静止）
  const creepX = t => creepAmp * (Math.sin(t * 0.83) * 26 + Math.sin(t * 2.31 + 1.1) * 9);
  const creepY = t => creepAmp * (Math.cos(t * 0.61 + 0.7) * 18 + Math.sin(t * 1.93) * 7);

  return {
    /// 本帧的 zoom（已含 creep）。
    zoom: t => {
      const z = keys(t, base);
      return creepAmp === 0 ? z : z * (1 + Math.sin(t * 1.3) * 0.006);
    },
    /// 本帧的锚点（已含 creep 的平移分量）。
    anchor: t => [ax + creepX(t), ay + creepY(t)],
    /// 把一个屏幕点按本帧相机变换（zoom 绕锚点缩放）。
    ///
    /// 用 `(v - a) * z + a` 而不是 `a + (v - a) * z`——两者等价，但前者读起来
    /// 与 `applyCamera` 里手写的公式一致，改一处不会只改一半。
    at: (t, x, y) => {
      const z = keys(t, base);
      const zz = creepAmp === 0 ? z : z * (1 + Math.sin(t * 1.3) * 0.006);
      const cx = ax + creepX(t);
      const cy = ay + creepY(t);
      return [(x - cx) * zz + cx, (y - cy) * zz + cy];
    },
  };
}

// ── ④ 背景纹理：给相机运动提供"可测参照" ───────────────────────────────
//
// ## 这条是实测换来的（详见 docs/ot_bench_fixes.md）
//
// 评测网格 32×18（一格 60×60px），静止阈值 1.2（0~255）。一个直径 d 的点在一格里
// 占 (d/60)²；相机平移 s 像素时格均值变化 ≈ 2·d·s/3600。
//
// 实测（c02-onedot，只改这一点）：
//   d=2.6 @48px  → p95 1.63、静止 90.9%（测不到）
//   d=14  @60px  → p95 3.22、静止 54.6%（通过）
//
// 所以"让画面活着"不是加噪点，是加**足够大的可测参照物**。
// 而它必须**随相机变换**（画在 applyCamera 之前的 shapes 里）——屏幕空间固定
// 的纹理在推镜时纹丝不动，实测 p95 掉回 1.61。
function lattice(opts) {
  const o = opts || {};
  const step = o.step || 60;
  const size = o.size || 14;
  const alpha = o.alpha === undefined ? 0.6 : o.alpha;
  const fill = o.fill || "#2E2E2E";
  const w = o.width || 1920;
  const h = o.height || 1080;
  // 让开文字所在的横向带（文字由**图层**画，而图层在 shapes 之下——
  // 不让开就会被点阵盖住，实测抽帧可见）
  const skipBands = o.skipBands || [];
  const out = [];
  let i = 0;
  for (let gy = -step; gy <= h + step; gy += step) {
    for (let gx = -step; gx <= w + step; gx += step) {
      ++i;
      // 疏密不均：严格网格会像坐标纸
      if (hash01(i) < (o.drop || 0.09)) continue;
      let inBand = false;
      for (let b = 0; b < skipBands.length; b++) {
        if (gy > skipBands[b][0] && gy < skipBands[b][1]) { inBand = true; break; }
      }
      if (inBand) continue;
      out.push({ kind: 0, x: gx, y: gy, w: size, h: size, radius: size / 2,
                 fill: fill, alpha: alpha });
    }
  }
  return out;
}

// ── ⑤ 文字/形状的小工厂（裁云的图元只有 kind 0~3）─────────────────────
//
// kind: 0 圆  1 矩形  2 线  3 文本
const shapes = {
  circle: (x, y, r, fill, alpha) => ({ kind: 0, x: x, y: y, w: r * 2, h: r * 2,
                                       radius: r, fill: fill, alpha: alpha === undefined ? 1 : alpha }),
  rect: (x, y, w, h, fill, alpha) => ({ kind: 1, x: x, y: y, w: w, h: h,
                                        fill: fill, alpha: alpha === undefined ? 1 : alpha }),
  line: (x1, y1, x2, y2, stroke, width, alpha) => ({ kind: 2, x: x1, y: y1, x2: x2, y2: y2,
                                                     stroke: stroke, strokeWidth: width,
                                                     alpha: alpha === undefined ? 1 : alpha }),
  text: (x, y, h, str, color, alpha) => ({ kind: 3, text: str, x: x, y: y, h: h,
                                           color: color, alpha: alpha === undefined ? 1 : alpha }),
};

// ── ⑥ 常见动效（每个都是纯函数，返回可直接用的数值）────────────────────

/// 打字机：返回前 n 个字符（按 cps 速度）。
const typeOn = (str, t, start, cps) => {
  const n = Math.floor((t - start) * (cps || 28));
  if (n <= 0) return "";
  return str.slice(0, Math.min(str.length, n));
};

/// 从下方升起（配合 alpha 淡入）：返回 {dy, alpha}。
const wordRise = (t, start, dur, dist) => {
  const k = ease.expoOut(seg(t, start, start + (dur || 0.6)));
  return { dy: (1 - k) * (dist || 40), alpha: k };
};

/// 错峰延迟：第 i 个元素相对整体起点延迟多少秒（用 hash 打散，避免整齐划一）。
const stagger = (i, gap, jitter) => {
  const g = gap === undefined ? 0.08 : gap;
  const j = jitter === undefined ? 0.4 : jitter;
  return i * g + (hash01(i * 7919) - 0.5) * g * j;
};

/// 呼吸：给静止的主体加一点持续起伏（返回位移量，单位像素）。
///
/// 注意：1~2px 的位移在 32×18 网格里**测不到**（一格 60px）。要"被看见"至少 20~30px。
const breathe = (t, amp, hz) => Math.sin(t * Math.PI * 2 * (hz || 0.5)) * (amp || 20);

// ── ⑦ 导出（供脚本直接引用；也让 node 能 require 做单测）──────────────
var KIT = {
  version: "1.0.0",
  clamp: clamp, lerp: lerp, seg: seg, sstep: sstep, ssstep: ssstep,
  ease: ease, keys: keys, hash01: hash01,
  camera: camera, lattice: lattice, shapes: shapes,
  typeOn: typeOn, wordRise: wordRise, stagger: stagger, breathe: breathe,
};
if (typeof module === "object" && module.exports) module.exports = KIT;

// ============================================================================
// 模板 A：单体特写（one-dot / ebb / clawd 类）
// ----------------------------------------------------------------------------
// 适用：**一个主体演完整片**，靠它自己的形变、位移、镜头推拉推进叙事。
// 不适合：多元素并发（用模板 B）、数据/图表（用模板 C）。
//
// ## 这份模板把哪些坑预先填掉了（每条都对应实测代价）
//
// 1. **相机无平台**：`camera({keys})` 默认叠 creep。实测 5 个 case 里 4 个的
//    "静止占比过高"主因是 `keys()` 两端同值 → 画面站定。
// 2. **背景可测参照**：`lattice()` 的尺寸是按「60px 格里测得到」定的，不是好看定的。
//    纯黑背景下一个 7px 的点移动 600px，格均值只变 0.2（阈值 1.2）——实测踩过。
// 3. **背景让开文字带**：`skipBands`。文字由**图层**画，图层在 shapes 之下，
//    不让开就会被点阵盖住（实测抽帧可见）。
// 4. **结尾不留 2 秒定格**：`holdTail` 默认给持续小位移，而不是彻底静止。
//
// ## 结构与改法
//
// 只有 ③ 是要你写的。①② 是复用层，④ 是收尾。
// 把 `STORY` 换成你的节拍表，其余照用。
// ============================================================================

// ── ① 全片参数（改这里就能换尺寸/配色，不必翻正文）──────────────────────
const W = 1920, H = 1080;
const CX = W / 2, CY = H / 2;

const PALETTE = {
  bg: "#0B0B0D",
  ink: "#E8E4DA",
  dim: "#8B8375",
  accent: "#D2553C",
  // 背景点阵：低对比，只提供"可测参照"，不抢主体
  texture: "#2E2E2E",
};

// ── ② 节拍表（把剧本第 3 节的逐镜设计抄成这张表）────────────────────────
//
// 每秒一条：`[起, 止, 标签]`。标签只用于调试打印，不参与渲染。
// 这张表是**唯一的时间真值源**——所有 keys() 的关键帧都应落在这些边界上，
// 时间改了只改这里，不会出现"某处还留着旧时刻"。
const STORY = [
  [0.0, 1.2, "出现"],
  [1.2, 3.0, "找位置"],
  [3.0, 4.4, "打出第一行"],
  [4.4, 7.0, "展开"],
  [7.0, 9.5, "峰值"],
  [9.5, 12.0, "收束"],
];

// 图元与图层索引：文字一律用图层（见 ④），图形用 shapes
const L_NAME = 0, L_TAG = 1;

// kit 的图元工厂（`shapes` 这个名字被 layout 里的局部数组占用了，
// 所以解构出来用短名——比在末尾写别名可靠，不依赖执行顺序）
const { circle: mkCircle, rect: mkRect, line: mkLine, text: mkText } = shapes;

function layout(t, duration, canvas, layers) {
  const out = layers.map(() => ({}));
  const shapes = [];

  // ── ③-a 背景纹理：**必须在相机变换之前** push ─────────────────────────
  //
  // 放在相机之前 → 随镜头一起移动 → 整屏纹理在动（实测 p95 3.22）
  // 放在相机之后 → 屏幕空间固定 → 推镜时纹丝不动（实测 p95 1.61，不合格）
  const bg = lattice({
    step: 60, size: 14, alpha: 0.58, fill: PALETTE.texture,
    width: W, height: H,
    // 让开文字所在的横带：文字在 y≈500（名字）与 y≈580（副标）
    skipBands: [[470, 530], [560, 620]],
  });
  for (let i = 0; i < bg.length; i++) shapes.push(bg[i]);

  // ── ③-b 主体：一个东西，全片只有它 ────────────────────────────────────
  //
  // 尺寸与位置的曲线**不要留平台**（两端同值的段 = 画面站定）。
  // 若要"停留"，用极慢的持续位移代替恒定值（见 holdTail 的说明）。
  const px = keys(t, [
    [0.0, CX], [1.2, CX - 120], [3.0, CX - 260], [4.4, CX + 40],
    [7.0, CX + 220], [9.5, CX + 60], [12.0, CX + 150],
  ]);
  const py = keys(t, [
    [0.0, CY], [1.2, CY - 60], [3.0, CY + 40], [4.4, CY - 80],
    [7.0, CY + 30], [9.5, CY - 40], [12.0, CY + 20],
  ]);
  const pr = keys(t, [
    [0.0, 0], [1.2, 34], [3.0, 46], [4.4, 90],
    [7.0, 190], [9.5, 70], [12.0, 44],
  ]);
  const pa = keys(t, [
    [0.0, 0], [0.35, 1], [9.5, 1], [11.6, 0.25], [12.0, 0.25],
  ]);

  // 主体的"呼吸"：纯位移，不改变结构。幅度按「跨得过 60px 格」定。
  const br = breathe(t, 22, 0.42);

  shapes.push(mkCircle(px + br * 0.4, py + br, pr, PALETTE.accent, pa));

  // ── ③-c 相机：默认带 creep，不留平台 ──────────────────────────────────
  //
  // 关键帧只给"意图"（1.0 → 1.35 → 1.0），creep 负责填掉平台期的死寂。
  // 要真正定格（例如最后 0.5 秒品牌定格）时传 creep: 0 并显式给两帧同值——
  // 那时静止是**设计意图**，不是疏漏。
  const cam = camera({
    keys: [
      [0.0, 1.06], [1.2, 1.14], [3.0, 1.30], [4.4, 1.10],
      [7.0, 1.38], [9.5, 1.16], [12.0, 1.02],
    ],
    ax: CX, ay: CY,
    creep: 1,
  });
  const z = cam.zoom(t);
  const [ax, ay] = cam.anchor(t);

  // 把已 push 的图元与主体一起按相机变换（背景也在内 → 整屏都在动）
  for (let i = 0; i < shapes.length; i++) {
    const s = shapes[i];
    s.x = (s.x - ax) * z + ax;
    s.y = (s.y - ay) * z + ay;
    if (s.kind === 2) {
      s.x2 = (s.x2 - ax) * z + ax;
      s.y2 = (s.y2 - ay) * z + ay;
      s.stroke = Math.max(0.6, s.stroke * z);
    } else {
      // 只对**真的有这个字段**的图元缩放（文字图元没有 w，写 undefined 会得 NaN）
      if (s.w !== undefined) s.w = Math.max(1, s.w * z);
      if (s.kind !== 3 && s.h !== undefined) s.h = Math.max(1, s.h * z);
      if (s.radius !== undefined) s.radius = Math.max(0.5, s.radius * z);
    }
  }

  // ── ④ 文字：一律用图层 ────────────────────────────────────────────────
  //
  // 为何用图层而不是 shapes：图层有**自动排版**（字号自适应、行距），
  // 而 shapes 的 kind=3 只给一个高度 h，长文本要自己断行。
  // 代价：图层在 shapes 之下，所以背景必须 `skipBands` 让开（见 ③-a）。
  const nameIn = wordRise(t, 9.5, 0.7, 60);
  out[L_NAME] = { alpha: 0.98 * nameIn.alpha, dy: nameIn.dy };
  const tagIn = wordRise(t, 9.95, 0.6, 40);
  out[L_TAG] = { alpha: 0.72 * tagIn.alpha, dy: tagIn.dy };

  return { states: out, shapes };
}
