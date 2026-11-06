// 裁云 · 复用内核
//
// 把本文件**整段**粘到 layout 脚本最前面即可使用。
// 三块内容：A 运动学 / B 投影与实体 / C 常用图元。
//
// ## 这个库补的是什么
//
// 裁云自带的 `keys()` 是**分段线性**插值：速度在关键帧处突变，读作"一顿一顿"。
// 而同样一个 8 帧位移，`linear` 读作"被拖动"，`spring` 读作"弹过去的"——
// 这是"机械感"与"生命感"的唯一差别来源。
//
// 运动学部分移植自 `onetake/lib/motion.js` 的纯数学段
// （缓动 / 弹簧 / 样条 / 噪声 / 错峰），去掉了所有 DOM、Canvas、相机相关部分。
//
// ## 一条硬规则
//
// 本库**不含 `Math.random`**。裁云要求同脚本两次出片逐像素一致，
// 需要"随机感"时用 `hash01(seed)` 或 `noise1(x, seed)`。

// ═══════════════════════════════════════════════════════════
// A. 运动学
// ═══════════════════════════════════════════════════════════

function clamp01(v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
function clampV(v, a, b) { return v < a ? a : (v > b ? b : v); }
function lerpN(a, b, k) { return a + (b - a) * k; }

/// 把 t 归一到 [a,b] 的 0..1
function seg(t, a, b) {
  if (b === a) return t >= b ? 1 : 0;
  return clamp01((t - a) / (b - a));
}

/// 平滑阶梯（3 次 / 5 次）
function sstep(e0, e1, x) {
  const k = clamp01((x - e0) / (e1 - e0));
  return k * k * (3 - 2 * k);
}
function ssstep(e0, e1, x) {
  const k = clamp01((x - e0) / (e1 - e0));
  return k * k * k * (k * (k * 6 - 15) + 10);
}

/// 缓动曲线集合（f(u) → 0..1，u 已归一）
const EASE = {
  linear: (u) => u,
  /// 快起慢收 —— "重物落下"、"被吸进来"
  expoOut: (u) => (u >= 1 ? 1 : 1 - Math.pow(2, -10 * u)),
  /// 慢起快收 —— "被抽走"
  expoIn: (u) => (u <= 0 ? 0 : Math.pow(2, 10 * (u - 1))),
  expoInOut: (u) => (u <= 0 ? 0 : (u >= 1 ? 1 :
    (u < 0.5 ? Math.pow(2, 20 * u - 10) / 2 : (2 - Math.pow(2, -20 * u + 10)) / 2))),
  polyOut: (n) => (u) => 1 - Math.pow(1 - u, n),
  polyIn: (n) => (u) => Math.pow(u, n),
  /// 过冲后回落 —— "弹一下"
  backOut: (u, s) => {
    const k = (s === undefined) ? 1.70158 : s;
    const v = u - 1;
    return 1 + (k + 1) * v * v * v + k * v * v;
  },
  /// 物理弹跳
  bounceOut: (u) => {
    const n = 7.5625, d = 2.75;
    if (u < 1 / d) return n * u * u;
    if (u < 2 / d) { const v = u - 1.5 / d; return n * v * v + 0.75; }
    if (u < 2.5 / d) { const v = u - 2.25 / d; return n * v * v + 0.9375; }
    const v = u - 2.625 / d;
    return n * v * v + 0.984375;
  },
};

/// 欠阻尼弹簧位移：tau = 从触发到现在的秒数
///
/// zeta = 阻尼比：0.4 弹很多 / **0.62 一次过冲（常用）** / 1.0 不过冲
/// omega = 角频率：18~28 是"灵敏但不廉价"
/// **返回值可超过 1**（过冲），需要限制时自己 clamp01。
function spring(tau, zeta, omega) {
  const z = (zeta === undefined) ? 0.62 : zeta;
  const w = (omega === undefined) ? 20 : omega;
  if (tau <= 0) return 0;
  if (z < 1) {
    const wd = w * Math.sqrt(1 - z * z);
    return 1 - Math.exp(-z * w * tau) * (Math.cos(wd * tau) + (z * w / wd) * Math.sin(wd * tau));
  }
  if (z === 1) return 1 - Math.exp(-w * tau) * (1 + w * tau);
  const q = w * Math.sqrt(z * z - 1);
  const r1 = -z * w + q, r2 = -z * w - q;
  const c1 = -r2 / (r1 - r2), c2 = r1 / (r1 - r2);
  return 1 - (c1 * Math.exp(r1 * tau) + c2 * Math.exp(r2 * tau));
}

function springVel(tau, zeta, omega) {
  const z = (zeta === undefined) ? 0.62 : zeta;
  const w = (omega === undefined) ? 20 : omega;
  if (tau <= 0) return 0;
  if (z < 1) {
    const wd = w * Math.sqrt(1 - z * z);
    return Math.exp(-z * w * tau) * (w * w / wd) * Math.sin(wd * tau);
  }
  return w * w * tau * Math.exp(-w * tau);
}

/// 衰减振铃：落地 / 碰撞 / 点击的"咔"
function ring(tau, decay, omega) {
  const d = (decay === undefined) ? 9 : decay;
  const w = (omega === undefined) ? 26 : omega;
  return tau <= 0 ? 0 : Math.exp(-d * tau) * Math.sin(w * tau);
}

/// 弹簧大致稳定的时刻（秒）—— 用于安排"落定后静止"
function settleTime(zeta, omega, eps) {
  const z = (zeta === undefined) ? 0.62 : zeta;
  const w = (omega === undefined) ? 20 : omega;
  const e = (eps === undefined) ? 0.02 : eps;
  return Math.log(1 / e) / (z * w);
}

/// Catmull-Rom 样条：knots = [{t, v}, ...]
///
/// 与 `keys()` 的区别：`keys()` 分段线性（速度突变），样条连续可导（读作"被手拨过"）。
function splineKnots(knots, t) {
  const L = knots.length;
  if (L === 0) return 0;
  if (L === 1) return knots[0].v;
  if (t <= knots[0].t) return knots[0].v;
  if (t >= knots[L - 1].t) return knots[L - 1].v;
  let i = 0;
  while (i < L - 2 && t >= knots[i + 1].t) i++;
  const k0 = knots[i], k1 = knots[i + 1];
  const h = k1.t - k0.t;
  if (h <= 1e-9) return k1.v;
  const u = (t - k0.t) / h;
  const km = knots[Math.max(0, i - 1)], kp = knots[Math.min(L - 1, i + 2)];
  const m0 = (k1.v - km.v) / Math.max(1e-9, k1.t - km.t) * h;
  const m1 = (kp.v - k0.v) / Math.max(1e-9, kp.t - k0.t) * h;
  const h00 = 2 * u * u * u - 3 * u * u + 1;
  const h10 = u * u * u - 2 * u * u + u;
  const h01 = -2 * u * u * u + 3 * u * u;
  const h11 = u * u * u - u * u;
  return h00 * k0.v + h10 * m0 + h01 * k1.v + h11 * m1;
}

/// 确定性哈希 → [0,1)。**绝不用 Math.random**
function hash01(a, b) {
  const x = Math.sin(a * 127.1 + (b || 0) * 311.7) * 43758.5453;
  return x - Math.floor(x);
}

/// 第 i 个元素的入场延迟（错峰）
function staggerDelay(i, base, step, wave) {
  const b = (base === undefined) ? 0 : base;
  const s = (step === undefined) ? 0.06 : step;
  const w = (wave === undefined) ? 0 : wave;
  return b + i * s + Math.sin(i * 1.7) * w;
}

/// 从中心向外错峰（散开 / 聚合类动效常用）
function radialDelay(x, y, cx, cy, maxR, base, span) {
  const d = Math.hypot(x - cx, y - cy);
  const k = clamp01(d / Math.max(1, maxR));
  return (base || 0) + k * (span === undefined ? 0.6 : span);
}

/// 一维值噪声（确定性）
function noise1(x, seed) {
  const i = Math.floor(x);
  const f = x - i;
  const a = hash01(i, seed || 0);
  const b = hash01(i + 1, seed || 0);
  const u = f * f * (3 - 2 * f);
  return a + (b - a) * u;
}

// ═══════════════════════════════════════════════════════════
// B. 投影与实体
// ═══════════════════════════════════════════════════════════

const ISO_CX = Math.cos(Math.PI / 6);
const ISO_CY = Math.sin(Math.PI / 6);

/// 等轴测投影（适合装置 / 阵列 / 方块堆）
///
/// ⚠ **不适合人形**：30° 轴倾斜会把左右和前后都投到斜线上，人形被压成柱子。
/// 人形请用 `perspProj`。
function isoProj(ox, oy, scale, yaw) {
  const y = yaw || 0;
  const cy = Math.cos(y), sy = Math.sin(y);
  return (x, yy, z) => {
    const rx = x * cy - yy * sy;
    const ry = x * sy + yy * cy;
    return {
      x: ox + (rx - ry) * ISO_CX * scale,
      y: oy + (rx + ry) * ISO_CY * scale - z * scale,
    };
  };
}

/// 等轴测深度键（**小的先画** = 远的先画）
function isoDepth(x, y, yaw) {
  const yy = yaw || 0;
  return (x * Math.sin(yy) + y * Math.cos(yy));
}

/// 弱透视投影 —— **人形 / 机器人必须用这个**
///
/// x 直接向右、y 向画面内收、z 向上，水平与垂直尺度约 1:1。
/// 这样肩膀是水平的、腿是竖直的，人形才立得住。
function perspProj(ox, oy, scale, yaw, pers) {
  const y = yaw || 0;
  const p = (pers === undefined) ? 0.0016 : pers;
  const cy = Math.cos(y), sy = Math.sin(y);
  return (x, yy, z) => {
    const rx = x * cy - yy * sy;
    const ry = x * sy + yy * cy;
    const depth = 1 + ry * p * 0.5;
    return {
      x: ox + rx * scale * depth,
      y: oy + (ry * 0.42 - z) * scale * depth,
    };
  };
}

/// 弱透视下的深度键（小的先画）
function depthKeyOf(x, y, yaw) {
  const yw = yaw || 0;
  return (x * Math.sin(yw) + y * Math.cos(yw));
}

/// #rrggbb 按系数调亮 / 压暗
function shadeHex(hex, k) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  const f = (v) => Math.min(255, Math.max(0, Math.round(v * k)));
  return '#' + [f(r), f(g), f(b)].map(v => v.toString(16).padStart(2, '0')).join('');
}

/// 两色线性混合
function mixHex(a, b, k) {
  const pa = [parseInt(a.slice(1, 3), 16), parseInt(a.slice(3, 5), 16), parseInt(a.slice(5, 7), 16)];
  const pb = [parseInt(b.slice(1, 3), 16), parseInt(b.slice(3, 5), 16), parseInt(b.slice(5, 7), 16)];
  const u = clamp01(k);
  return '#' + pa.map((v, i) => {
    const xx = Math.round(v + (pb[i] - v) * u);
    return xx.toString(16).padStart(2, '0');
  }).join('');
}

/// 立方体：**三个可见面** + 顶棱高光
///
/// ⚠ 三面顶点索引必须选**可见面**：
///   顶面 `[4,5,6,7]` / x+w 面 `[1,5,6,2]` / y+d 面 `[3,2,6,7]`
/// 画成 x=0 的背面会让相邻方块**互相穿透**（出现大片直角三角，实测踩过）。
function isoBox(shapes, P, x, y, z, w, d, h, hex, alpha) {
  const v = [P(x, y, z), P(x + w, y, z), P(x + w, y + d, z), P(x, y + d, z),
             P(x, y, z + h), P(x + w, y, z + h), P(x + w, y + d, z + h), P(x, y + d, z + h)];
  const poly = (idx, c, a) => {
    const pts = [];
    for (const i of idx) { pts.push(v[i].x, v[i].y); }
    shapes.push({ kind: 4, points: pts, color: c, alpha: a });
  };
  poly([4, 5, 6, 7], shadeHex(hex, 1.00), alpha);
  poly([1, 5, 6, 2], shadeHex(hex, 0.68), alpha);
  poly([3, 2, 6, 7], shadeHex(hex, 0.45), alpha);
  shapes.push({ kind: 2, x: v[4].x, y: v[4].y, x2: v[5].x, y2: v[5].y,
                stroke: 1.6, color: shadeHex(hex, 1.38), alpha: alpha * 0.72 });
}

/// 接触阴影：底部压扁的柔椭圆
///
/// **没有它，等轴测方块会读作"贴在纸上的多边形"。**
/// 三层叠加做柔边（单层有硬边）。
function isoShadow(shapes, P, x, y, z, w, d, alpha, tint) {
  const c = P(x + w / 2, y + d / 2, z);
  const r = Math.max(w, d);
  const col = tint || '#000000';
  for (let i = 3; i >= 1; i--) {
    shapes.push({ kind: 0, x: c.x + r * 0.40, y: c.y + r * 0.24,
                  w: r * (1.5 + i * 0.55), h: r * (0.50 + i * 0.16),
                  color: col, alpha: alpha * 0.26 / i });
  }
}

/// 球：用同心圆叠出梯度（读作球，而不是圆盘）
function isoBall(shapes, P, x, y, z, r, hex, alpha) {
  const c = P(x, y, z - r);
  const R = r * ISO_CX * 2;
  const layers = 7;
  for (let i = 0; i < layers; i++) {
    const f = i / (layers - 1);
    const rr = R * (1 - f * 0.32);
    const off = f * R * 0.16;
    shapes.push({
      kind: 0, x: c.x - off, y: c.y - off, w: rr * 2, h: rr * 2,
      color: i === layers - 1 ? shadeHex(hex, 1.32)
             : (f < 0.5 ? shadeHex(hex, 0.72) : shadeHex(hex, 1.0)),
      alpha: alpha * (i === layers - 1 ? 0.95 : 0.9),
    });
  }
}

// ═══════════════════════════════════════════════════════════
// C. 常用图元
// ═══════════════════════════════════════════════════════════

/// 估字宽：中文 1.0em、ASCII 0.52em（与 motion.cpp 的 spaced_text 同口径）
function textWidth(txt, size) {
  let w = 0;
  for (const ch of txt) w += (ch.charCodeAt(0) > 127 ? size : size * 0.52);
  return w;
}

/// 画一行居中的文字
///
/// ⚠ **不要依赖 `align: 1`** —— 实测它的行为与预期不符（文字被推到左边）。
/// 手算左边界最稳。
function textCentered(shapes, W, txt, y, size, color, alpha) {
  const tw = textWidth(txt, size);
  shapes.push({
    kind: 3, text: txt, x: (W - tw) * 0.5, y,
    w: tw + 40, h: size, size: size,
    color: color, alpha: alpha, align: 0,
  });
}

/// 扫描光带：一条很淡的亮带持续横穿
///
/// 放在**最后**画（它在最上层）。浅底用深色，深底用亮色。
function scanBand(shapes, W, H, lt, color, alphaPeak, period) {
  const p = (period === undefined) ? 2.8 : period;
  const phase = (lt % p) / p;
  const bandW = 340;
  const bx = -bandW + (W + bandW * 2) * phase;
  const col = color || '#FFFFFF';
  const peak = (alphaPeak === undefined) ? 0.018 : alphaPeak;
  for (let g = 0; g < 6; g++) {
    const f = g / 5;
    const edge = 1 - Math.abs(f - 0.5) * 2;
    shapes.push({ kind: 1, x: bx + (f - 0.5) * bandW, y: 0,
                  w: bandW / 6 + 2, h: H, color: col, alpha: peak * edge });
  }
}

/// 整屏呼吸：保证每一帧画面都在变（验收口径 32×18 网格）
///
/// **dL 与网格差是 1:1** —— 这是所有手法里最省的（见 caiyun_layout_script.md §6.1.8）。
/// 深色底用 `#FFFFFF`（alpha 0.004~0.03），**浅色底用暗色**（用亮色会让画面发灰）。
function breatheLayer(shapes, W, H, lt, color, lo, hi, period) {
  const p = (period === undefined) ? 0.36 : period;
  const pulse = 0.5 + 0.5 * Math.sin((lt / p) * Math.PI * 2);
  const a0 = (lo === undefined) ? 0.004 : lo;
  const a1 = (hi === undefined) ? 0.018 : hi;
  shapes.push({ kind: 1, x: 0, y: 0, w: W, h: H, color: color || '#FFFFFF',
                alpha: a0 + (a1 - a0) * pulse });
}
