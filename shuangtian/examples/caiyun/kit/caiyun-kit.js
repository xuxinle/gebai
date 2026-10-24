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

// ── ① 基础缓动 ─────────────────────────────────────────────────────────
//
// ## 命名规则：**不能与裁云的宿主函数重名**
//
// 宿主注入的名字是：`ease` `spring` `spring_vel` `settle` `lerp` `clamp` `snap`
// （见 `layout.cpp` 的 register_function 白名单）。
//
// kit 是被**拼在脚本前面**的，所以这里若再声明 `const clamp = ...`，
// 整份 layout 会以 `Identifier 'clamp' has already been declared` **加载失败**——
// 而渲染器对此是**静默降级**：不报错、照常出片、画面只剩背景。
//
// 实测代价：模板因此渲出 94% 静止的空画面，我一度以为是相机或背景没生效。
// 所以 kit 内部一律用 `k` 前缀，导出名也避开这七个。
const kClamp = (v, a, b) => {
  const lo = a === undefined ? 0 : a;
  const hi = b === undefined ? 1 : b;
  return v < lo ? lo : (v > hi ? hi : v);
};
const kLerp = (a, b, k) => a + (b - a) * k;
const seg = (t, t0, t1) => (t1 === t0 ? (t >= t1 ? 1 : 0) : kClamp((t - t0) / (t1 - t0), 0, 1));
const sstep = (e0, e1, x) => { const k = kClamp((x - e0) / (e1 - e0), 0, 1); return k * k * (3 - 2 * k); };
const ssstep = (e0, e1, x) => {
  const k = kClamp((x - e0) / (e1 - e0), 0, 1);
  return k * k * k * (k * (k * 6 - 15) + 10);
};

// `ease` 是宿主函数名 → 内部叫 kEase，导出也避开
const kEase = {
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
      const fn = kEase[name] || kEase.linear;
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
  // ## 幅度按"能不能被测到"定，不是按好看定
  //
  // 评测网格一格 60×60px，静止阈值 1.2（0~255）。相机平移 s 像素时，
  // 一个直径 d 的参照物造成的格均值变化 ≈ 2·d·s/3600。
  //
  // 取 d=14（kit 的 lattice 默认），要 Δ>1.2 需 s ≳ 150px。
  // 第一版给的是 26px 幅度 → Δ≈0.2，**测不到**（模板渲出 94% 静止）。
  //
  // 所以这里给 110px 级的慢漂移：周期 6~10 秒，观感是"镜头在呼吸"，
  // 而每帧位移够大。两层不同频率是为了避免整周期处同时回零。
  const creepX = t =>
      creepAmp * (Math.sin(t * 0.62) * 110 + Math.sin(t * 1.71 + 1.1) * 42);
  const creepY = t =>
      creepAmp * (Math.cos(t * 0.47 + 0.7) * 78 + Math.sin(t * 1.39) * 30);

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
function lattice(opts, t) {
  const o = opts || {};
  // ## `t` 缺省必须是 0，不能是 undefined
  //
  // `undefined * flow = NaN`，而 NaN 坐标会让**整个形状不画**（渲染器不报错）。
  // 实测：忘记传 t 时点阵整层消失，看起来像"背景没生成"。
  const time = t === undefined ? 0 : t;
  const step = o.step || 60;
  // ## 默认 26px，不是 14
  //
  // 判据再算一次（这次用**逐帧**位移，不是总位移）：
  // 评测网格一格 60px、静止阈值 1.2，逐帧位移 s_f，直径 d：
  //     Δ ≈ 2·d·s_f / 3600
  // creep 的 110px 幅度摊在 1/30s 上是 s_f ≈ 3px，取 d=14 → Δ ≈ 0.023（测不到）。
  //
  // 要过 1.2 需 d·s_f ≥ 2200 → d=26 时 s_f 仍需 85px/帧，那已经是"甩镜头"了。
  // 所以真正的解法不是无限加大 d，而是**让点阵自己每帧都在动**（见 lattice 的
  // `flow` 参数）：每帧整体平移 20~30px，点阵就会持续扫过格子边界。
  const size = o.size || 18;
  const alpha = o.alpha === undefined ? 0.6 : o.alpha;
  const fill = o.fill || "#2E2E2E";
  const w = o.width || 1920;
  const h = o.height || 1080;
  // 让开文字所在的横向带（文字由**图层**画，而图层在 shapes 之下——
  // 不让开就会被点阵盖住，实测抽帧可见）
  const skipBands = o.skipBands || [];
  const flow = o.flow === undefined ? 780 : o.flow;   // px/s；逐帧位移 = flow/30   // px/s
  const out = [];
  let i = 0;
  for (let gy = -step * 2; gy <= h + step * 2; gy += step) {
    for (let gx = -step * 2; gx <= w + step * 2; gx += step) {
      ++i;
      // 疏密不均：严格网格会像坐标纸
      if (hash01(i) < (o.drop || 0.09)) continue;
      let inBand = false;
      for (let b = 0; b < skipBands.length; b++) {
        if (gy > skipBands[b][0] && gy < skipBands[b][1]) { inBand = true; break; }
      }
      if (inBand) continue;
      // ## 字段名是 `color`，不是 `fill`
      //
      // 渲染器（layout.cpp 第 303 行）只读 `color`/`stroke`，**没有 `fill` 字段**。
      // 写 `fill` 不会报错、只是用默认色（等于不生效）——这类静默失败最难查。
      // 实测：kit 第一版用了 `fill`，模板渲出来背景点阵完全不见，静止占比 94%。
      // ## 自流：点阵**自己**每帧平移（不依赖相机）
      //
      // 这是"让整屏可测"的关键：相机 creep 的逐帧位移只有 3px 量级，
      // 而点阵自己按 `flow` px/s 流动时，逐帧位移 = flow/30，
      // 取 flow=780 即 26px/帧 —— 每帧都有格子被扫过。
      //
      // 观感上是缓慢的"底纹在走"，不抢主体（点阵本来就在文字之下）。
      const fx = flow > 0 ? (time * flow) % step : 0;
      const fy = flow > 0 ? (time * flow * 0.37) % step : 0;
      out.push({ kind: 0, x: gx + fx, y: gy + fy, w: size, h: size, radius: size / 2,
                 color: fill, alpha: alpha });
    }
  }
  return out;
}

// ── ⑤ 文字/形状的小工厂（裁云的图元只有 kind 0~3）─────────────────────
//
// kind: 0 圆  1 矩形  2 线  3 文本
// ## 字段名的唯一真值源
//
// 渲染器的 shape 字段是：`kind/x/y/w/h/radius/color/stroke/strokeWidth/text/alpha/x2/y2`。
// 没有 `fill`（那是 SVG 的写法）。写错不报错、只是静默不生效——kit 第一版就踩了。
const shapes = {
  circle: (x, y, r, color, alpha) => ({ kind: 0, x: x, y: y, w: r * 2, h: r * 2,
                                        radius: r, color: color,
                                        alpha: alpha === undefined ? 1 : alpha }),
  rect: (x, y, w, h, color, alpha) => ({ kind: 1, x: x, y: y, w: w, h: h,
                                         color: color, alpha: alpha === undefined ? 1 : alpha }),
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
  const k = kEase.expoOut(seg(t, start, start + (dur || 0.6)));
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
  // 导出名避开宿主白名单（`kClamp`/`kLerp` 而不是 `clamp`/`lerp`）
  kClamp: kClamp, kLerp: kLerp, seg: seg, sstep: sstep, ssstep: ssstep,
  kEase: kEase, keys: keys, hash01: hash01,
  camera: camera, lattice: lattice, shapes: shapes,
  typeOn: typeOn, wordRise: wordRise, stagger: stagger, breathe: breathe,
};
if (typeof module === "object" && module.exports) module.exports = KIT;
