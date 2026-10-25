// ===== 共享前奏 =====
// 坐标系：**设计像素 1920×1080**。shapes 绘制时按 canvas 宽/1920 缩放，
// 所以这里一律按设计像素算——1920 导出时 1:1，其它分辨率自动等比。
//
// ## 三条实测口径（决定了这里的写法）
//
// 1. `shapes` **不过相机**（框架明文：paint_script_shapes 只乘 canvas/1920）。
//    所以 `cam_zoom` / `cam_rotate` 对脚本图元完全无效——"让画面活着"只能靠自己画。
// 2. 验收口径是 32×18 网格、逐格灰度均值差 <1.2 记静止。**单层几乎都过不了**：
//    实测同口径——落定版面 100% 静止、竖向数据流 100%、扩散环 100%、大块快穿 100%、
//    全屏光带横扫 100%；只有叠成三层（呼吸 + 扩散环 + 快扫）才到 28~37%。
//    横向位移本身几乎测不到（格均值不变），**亮度在变才测得到**。
// 3. 场景级 `grain` 在成片里测不出变化（1100/0.12 与 2400/0.22 都是 100% 静止），
//    不能拿它当"画面在动"的证据。

function clamp01(v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
function seg(t, a, b) { if (b === a) return t >= b ? 1 : 0; return clamp01((t - a) / (b - a)); }
function lerp(a, b, k) { return a + (b - a) * k; }
function sstep(a, b, x) { const k = clamp01((x - a) / (b - a)); return k * k * (3 - 2 * k); }

const EASE = {
  linear: (u) => u,
  expoOut: (u) => (u >= 1 ? 1 : 1 - Math.pow(2, -10 * u)),
  expoIn: (u) => (u <= 0 ? 0 : Math.pow(2, 10 * (u - 1))),
  expoInOut: (u) => (u <= 0 ? 0 : (u >= 1 ? 1 :
    (u < 0.5 ? Math.pow(2, 20 * u - 10) / 2 : (2 - Math.pow(2, -20 * u + 10)) / 2))),
  cubicOut: (u) => 1 - Math.pow(1 - u, 3),
  cubicIn: (u) => u * u * u,
};

/// 欠阻尼弹簧：可过冲（>1）。zeta 0.55 有弹性 / 0.85 干脆。
function spring(tau, zeta, omega) {
  const z = (zeta === undefined) ? 0.62 : zeta;
  const w = (omega === undefined) ? 20 : omega;
  if (tau <= 0) return 0;
  if (z < 1) {
    const wd = w * Math.sqrt(1 - z * z);
    return 1 - Math.exp(-z * w * tau) * (Math.cos(wd * tau) + (z * w / wd) * Math.sin(wd * tau));
  }
  if (z === 1) return 1 - Math.exp(-w * tau) * (1 + w * tau);
  const q = w * Math.sqrt(z * z - 1), r1 = -z * w + q, r2 = -z * w - q;
  const c1 = -r2 / (r1 - r2), c2 = r1 / (r1 - r2);
  return 1 - (c1 * Math.exp(r1 * tau) + c2 * Math.exp(r2 * tau));
}

function hash01(a, b) { const x = Math.sin(a * 127.1 + (b || 0) * 311.7) * 43758.5453; return x - Math.floor(x); }

function hex2rgb(h) {
  return [parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)];
}
function mixHex(a, b, k) {
  const pa = hex2rgb(a), pb = hex2rgb(b), u = clamp01(k);
  const to = (v) => Math.round(v).toString(16).padStart(2, '0');
  return '#' + pa.map((v, i) => to(v + (pb[i] - v) * u)).join('');
}
function shadeHex(h, k) {
  const p = hex2rgb(h), f = (v) => Math.max(0, Math.min(255, Math.round(v * k)));
  return '#' + p.map((v) => f(v).toString(16).padStart(2, '0')).join('');
}

// —— 文字（与渲染器同口径：中文 1.0em / ASCII 0.52em）——
function textW(s, size) {
  let w = 0;
  for (const ch of s) w += (ch.charCodeAt(0) > 127 ? size : size * 0.52);
  return w;
}
function pushText(sh, s, x, top, size, color, alpha) {
  sh.push({ kind: 3, text: s, x: x, y: top, h: size, color: color, alpha: alpha });
}
function pushTextC(sh, s, cx, top, size, color, alpha) {
  pushText(sh, s, cx - textW(s, size) * 0.5, top, size, color, alpha);
}
/// 手排字距的一行（居中）。逐字画才能控制字距与逐字入场。
function pushType(sh, s, cx, top, size, tr, color, alpha) {
  let total = 0;
  const chars = [];
  for (const ch of s) {
    const w = (ch.charCodeAt(0) > 127 ? size : size * 0.52);
    chars.push({ ch: ch, w: w });
    total += w + tr;
  }
  total -= tr;
  let x = cx - total * 0.5;
  for (const c of chars) {
    if (c.ch !== ' ') sh.push({ kind: 3, text: c.ch, x: x, y: top, h: size, color: color, alpha: alpha });
    x += c.w + tr;
  }
  return total;
}
/// 逐字入场（每字自己的 delay 与上浮）。
function pushTypeIn(sh, s, cx, top, size, tr, color, t, t0, stagger, rise) {
  let total = 0;
  const chars = [];
  for (const ch of s) {
    const w = (ch.charCodeAt(0) > 127 ? size : size * 0.52);
    chars.push({ ch: ch, w: w });
    total += w + tr;
  }
  total -= tr;
  let x = cx - total * 0.5;
  for (let i = 0; i < chars.length; i++) {
    const c = chars[i];
    const p = EASE.expoOut(seg(t, t0 + i * stagger, t0 + i * stagger + 0.52));
    if (c.ch !== ' ' && p > 0.001) {
      sh.push({ kind: 3, text: c.ch, x: x, y: top + (1 - p) * (rise || 26), h: size,
                color: color, alpha: p });
    }
    x += c.w + tr;
  }
  return total;
}

// —— 图元 ——
function pushRect(sh, x, y, w, h, color, alpha, radius, rot) {
  sh.push({ kind: 1, x: x, y: y, w: w, h: h, color: color, alpha: alpha,
            radius: radius || 0, rotate: rot || 0 });
}
function pushCircle(sh, cx, cy, r, color, alpha) {
  sh.push({ kind: 0, x: cx - r, y: cy - r, w: r * 2, h: r * 2, color: color, alpha: alpha });
}
function pushLine(sh, x1, y1, x2, y2, color, alpha, w) {
  sh.push({ kind: 2, x: x1, y: y1, x2: x2, y2: y2, color: color, alpha: alpha,
            stroke: w === undefined ? 1.5 : w });
}
function pushPoly(sh, pts, color, alpha, rot) {
  sh.push({ kind: 4, points: pts, color: color, alpha: alpha, rotate: rot || 0 });
}

// ═══════════════════════════════════════════════════════════
// 环境层：本片所有幕共用同一套"底"，换幕时只换主体的构图
// ══════════════════════════════════════════════
//
// ## 为何要统一成一个可复用的底
//
// 第一版每幕各写一套背景装饰，结果 7 幕全都在 82~89% 静止。
// 标定证明：**要让画面持续活着，靠的是三层正交的慢运动叠在一起**
// （整屏呼吸给基础量、扩散环给大尺度存在感变化、快扫给跨格位移）。
// 写成一套参数化的函数，既保证每幕都过判据，也保证全片质感一致。

/// 整屏呼吸：alpha 在 lo..hi 之间按 period 正弦摆动（深底用白 = 提亮）。
function envBreathe(sh, W, H, t, lo, hi, period) {
  const p = 0.5 + 0.5 * Math.sin((t / (period || 0.66)) * Math.PI * 2 - Math.PI / 2);
  pushRect(sh, -12, -12, W + 24, H + 24, '#FFFFFF', lo + (hi - lo) * p);
}

/// 扩散环：从画面中心向外扩散的细环，存在感随时间变（半径与 alpha 反向）。
function envRings(sh, W, H, t, cx, cy, color, gain, period) {
  const P = period || 1.55;
  for (let k = 0; k < 3; k++) {
    const ph = ((t / P) + k / 3) % 1;
    const r = 40 + ph * 900;
    pushCircle(sh, cx, cy, r, color, 0.20 * (1 - ph) * (gain === undefined ? 1 : gain));
  }
}

/// 快扫：几道宽带横向掠过（每帧 ~60px，跨整格；这是唯一"位移测得到"的手法）。
function envSweep(sh, W, H, t, color, gain, speed) {
  const v = speed || 1750;
  const bx = ((t * v) % (W + 700)) - 350;
  for (let g = 0; g < 4; g++) {
    pushRect(sh, bx + g * 78, 0, 78, H, color, (0.065 - g * 0.013) * (gain === undefined ? 1 : gain));
  }
}

/// 竖向细流：低频、低压差的竖向短线缓慢下落（给"数据在走"的质感，
/// 不指望它撑静止占比——实测它单独存在时网格差 0.35，远低于阈值）。
function envStream(sh, W, H, t, color, count, speed, alpha) {
  const n = count || 46;
  for (let i = 0; i < n; i++) {
    const lane = (i * 157.3 + 40) % W;
    const v = (speed || 150) * (0.7 + (i % 5) * 0.14);
    const y = ((t * v + i * 197) % (H + 240)) - 120;
    pushRect(sh, lane, y, 1.6, 42, color, alpha === undefined ? 0.30 : alpha);
  }
}

/// 网格纹：给"结构感"（低速漂移；同样不指望它撑判据）。
function envGrid(sh, W, H, t, gap, color, alpha, drift) {
  const off = ((t * (drift || 10)) % gap);
  for (let x = -gap + off; x < W + gap; x += gap) pushRect(sh, x, 0, 1, H, color, alpha);
  for (let y = -gap + ((t * (drift || 10) * 0.7) % gap); y < H + gap; y += gap) {
    pushRect(sh, 0, y, W, 1, color, alpha);
  }
}

/// 环境层总装：按"强度档"挂上背景层（环）。
///
/// `ring_gain` 可选：标题/正文压上来时给 0.4 以内，让环退到背景。
///
/// ## 为何用"降环强度"而不是盖一个半透明底（实测）
///
/// 第一版为了护住标题可读性，在标题区叠了 3 层深色矩形——结果成片里是一个
/// 边缘硬、颜色跳的**黑方地**（比原本要修的"环穿字"更难看）。
/// 环是带 alpha 的细线叠层，直接降低它的强度即可，无需造新的形状。
function envBackdrop(sh, W, H, t, level, cx, cy, ring_gain) {
  const L = (level === undefined) ? 1 : level;
  const cyy = (cy === undefined) ? H * 0.48 : cy;
  const cxx = (cx === undefined) ? W * 0.5 : cx;
  const gain = (ring_gain === undefined)
      ? (L === 0 ? 0.7 : (L === 1 ? 1.0 : 1.15))
      : ring_gain;
  envRings(sh, W, H, t, cxx, cyy, '#4D7DFF', gain, L === 0 ? 1.9 : 1.55);
}

/// 环境层前装：**画在内容之后**（可上）。
///
/// ## 什么时候用、什么时候别用（比参数量更重要）
///
/// 这层是为了"一个本来就有持续运动的镜头，别让它中途断气"，
/// **不是为了把每一幕都拉到同一个数值**。
///
/// | 镜头性质 | 该不该上这层 |
/// |---|---|
/// | 主体自己在持续运动（点阵波前、沿环流动、卡片错峰入场） | **不必**——主体已经带住画面 |
/// | 大块内容长期静止（终端窗口、已停的版面、字标落定） | **需要**，且只盖到“活着”而不抢主体 |
/// | 收尾/需要安静 | **宁少勿多**：不加横向快扫（那会把安静的收尾读成广告） |
///
/// 实测代价：第一版为了把全片静止占比从 82% 压到 28%，给**每一幕**都上了
/// 同样的呼吸 + 快扫（包括收尾字标）——数字好看了，但那是**指标牵引设计**：
/// 字标镜头每 2.9s 扫过一道光，读起来是"不知道为什么要动"。
/// 静止占比是**参考量**，不是收尾镜头必须达标的硬指标。
///
/// @param opts.sweep  是否带横向快扫（**默认 false**）
///
/// 默认关的理由：横扫是“整屏都在变”最省的手法，但它会**盖过主体自己的运动**，
/// 而且很容易读成“莫名地动”（尤其产品/三维/收尾镜头）。
/// 只有“大块内容长期完全静止、又没有任何自身运动”的镜头才值得显式开它。
function envOverlay(sh, W, H, t, level, opts) {
  const L = (level === undefined) ? 1 : level;
  const sweep = !!(opts && opts.sweep === true);
  const hi = L === 0 ? 0.055 : (L === 1 ? 0.052 : 0.058);
  const lo = 0.006;
  envBreathe(sh, W, H, t, lo, hi, L === 0 ? 0.62 : 0.64);
  if (sweep) {
    envSweep(sh, W, H, t, '#7FB2FF', L === 0 ? 0.85 : (L === 1 ? 1.0 : 1.2),
             L === 0 ? 1450 : (L === 1 ? 1750 : 2050));
  }
}


// 幕 2「躯体」5.2s —— 三维共享世界 + 左侧标识。
// 讲一件事：**智能有了身体**。三维负责实体感，二维负责标识。
//
// 两条实测约束：
// · 字标**只用二维文字**画（`kintext` 图层与手绘文字会叠印——实测同位置出现两份 GEBAI）；
// · 三维物体要**稀疏**（三个就够），彼此留空才有"展台"感，挤在一起读作杂物堆。
function scene_body(t, duration, canvas, layers) {
  const W = 1920, H = 1080;
  const sh = [];

  envBackdrop(sh, W, H, t, 1, W * 0.66, H * 0.46);

  // 左侧竖向支撑线：0 → 100% 长开
  const barP = EASE.expoOut(seg(t, 0.25, 1.5));
  if (barP > 0.01) pushRect(sh, 150, 248, 2, 588 * barP, '#3B82F6', 0.55 * barP);

  // 标识：逐字入场（只用一套字，不再叠 kintext 图层）
  const tIn = 0.72;
  pushTypeIn(sh, 'GEBAI', 476, 348, 104, 14, '#EAF0FF', t, tIn, 0.075, 34);
  pushTypeIn(sh, 'AGENT', 470, 478, 40, 18, '#6FA8FF', t, tIn + 0.42, 0.06, 22);

  // 释义三行：错峰上浮
  const rows = [
    '收编任意语言、任意进程',
    '一个工具面，装载即并入',
    '无状态智能，责任在智体'
  ];
  for (let i = 0; i < rows.length; i++) {
    const p = EASE.expoOut(seg(t, 1.30 + i * 0.16, 1.30 + i * 0.16 + 0.60));
    if (p > 0.01) {
      pushCircle(sh, 166, 700 + i * 54 + (1 - p) * 16, 3.4, '#3B82F6', 0.9 * p);
      pushText(sh, rows[i], 192, 680 + i * 54 + (1 - p) * 16, 28, '#9DB2CE', 0.94 * p);
    }
  }

  // 规格行（技术感，克制）
  pushText(sh, '23 SUB-AGENTS   ·   ORDERS: 80+ TOOLS', 152, 268, 18, '#5E7A99', 0.72);

  envStream(sh, W, H, t, '#6FA8FF', 44, 160, 0.24);
  // 本幕主体是三维实物（自转 + 浮动）+ 逐字入场，自身就在动——不再叠环境层。
  // 三维镜头叠上横向扫描线会读成"泡在带扫描线的玻璃缸里"，反而摄镜头。

  return { states: layers.map(() => ({})), shapes: sh };
}

// 本幕入口（每幕一个 layout 文件：没有判据可错）
function layout(t, duration, canvas, layers) {
  return scene_body(t, duration, canvas, layers);
}
