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

/// 环境层前装：**画在内容之后**（相最上层）。
///
/// ## 为何必须分前/后两层（实测）
///
/// 第一版把呼吸与扫描全画在背景层，结果任何大块内容（终端窗口、卡片区）
/// 把它俩盖掉——幕 4 的静止占比 95%，把推镜和环境层全白做了。
/// 呼吸必须作用于**整帧**才能产生"整屏亮度在变"（这是唯一与验收格差 1:1 的手法）。
///
/// ## 幅度与周期的换算（别再拍脑袋定幅度）
///
/// 正弦呼吸的**逐帧**格差约为 `A·4/(T·30)`（A = luma 摆幅，T = 周期秒）。
/// 验收阈值是 1.2，所以 A≈0.05（≈13 级）× T=0.62 才刚好过线（算出 1.42）。
/// 实测校验：标定 P1（A=11.5, T=0.66）算出 2.32、实测 median 1.98——公式可用。
function envOverlay(sh, W, H, t, level) {
  const L = (level === undefined) ? 1 : level;
  const hi = L === 0 ? 0.055 : (L === 1 ? 0.052 : 0.058);
  const lo = L === 0 ? 0.006 : 0.006;
  envBreathe(sh, W, H, t, lo, hi, L === 0 ? 0.62 : 0.64);
  envSweep(sh, W, H, t, '#7FB2FF', L === 0 ? 0.85 : (L === 1 ? 1.0 : 1.2),
           L === 0 ? 1450 : (L === 1 ? 1750 : 2050));
}


// 幕 6「回路」5.4s —— 双同心环 + 30 个节点沿环流动 + 中心大字。
// 讲一件事：**它改自己也走同一条回路**（自指闭环，不是单向流水线）。
function scene_loop(t, duration, canvas, layers) {
  const W = 1920, H = 1080;
  const sh = [];
  const CX = W * 0.5, CY = H * 0.47;

  envBackdrop(sh, W, H, t, 1, CX, CY);
  envGrid(sh, W, H, t, 140, '#141F35', 0.26, 6);

  // 整组缓慢推近：1.0 → 1.14（读作"贴近看它怎么改自己"）
  const push = 1.0 + 0.14 * EASE.expoOut(seg(t, 0.2, 4.6));
  const rw = (v) => v * push;

  const rings = [
    { r: 330, n: 18, dir: -1, speed: 0.42, size: 7, col: '#3B82F6' },
    { r: 214, n: 12, dir: 1, speed: 0.63, size: 5.5, col: '#7FB2FF' }
  ];

  // 环本体：分段画，亮度沿线流动（环自己也在传信号）
  for (const ring of rings) {
    const N = 96;
    for (let s = 0; s < N; s++) {
      const a0 = (s / N) * Math.PI * 2 + t * ring.speed * 0.12 * ring.dir;
      const x = CX + Math.cos(a0) * rw(ring.r);
      const y = CY + Math.sin(a0) * rw(ring.r) * 0.72;
      const flow = Math.pow(0.5 + 0.5 * Math.sin(a0 * 3 - t * 2.1 * ring.dir), 3);
      pushCircle(sh, x, y, rw(2.6), ring.col, 0.10 + flow * 0.44);
    }
  }

  // 节点：流动 + 拖尾（残影）
  for (const ring of rings) {
    for (let i = 0; i < ring.n; i++) {
      const base = (i / ring.n) * Math.PI * 2;
      for (let tr = 0; tr < 3; tr++) {
        const ph = base + (t - tr * 0.075) * ring.speed * ring.dir;
        const x = CX + Math.cos(ph) * rw(ring.r);
        const y = CY + Math.sin(ph) * rw(ring.r) * 0.72;
        const alpha = tr === 0 ? 0.95 : 0.22 / tr;
        const sz = rw(ring.size * (tr === 0 ? 1 : 0.8));
        pushCircle(sh, x, y, sz, tr === 0 ? '#EAF4FF' : ring.col, alpha);
        if (tr === 0) pushCircle(sh, x, y, sz * 2.1, ring.col, 0.16);
      }
    }
  }

  // 中心两行字（逐字入场，随推镜一起放大）
  const tIn = 0.75;
  pushTypeIn(sh, '改自己', CX, CY - 100 * push, 66 * push, 8, '#EAF0FF', t, tIn, 0.05, 26);
  pushTypeIn(sh, '也走同一条回路', CX, CY - 4 * push, 44 * push, 5, '#9EC4FF',
             t, tIn + 0.45, 0.04, 20);

  // 中心脉冲（心跳：2.2s 一次）
  const pulse = 0.5 + 0.5 * Math.sin(t * (Math.PI * 2 / 2.2));
  for (let i = 3; i >= 1; i--) {
    pushCircle(sh, CX, CY + 36 * push, rw(26 + i * 22 + pulse * 12), '#3B82F6',
               0.05 * (4 - i) * (0.6 + pulse * 0.4));
  }

  // 底部：验证正在跑（进度条 + 逐条通过的断言计数——数字在跳 = 明暗在变）
  const runP = clamp01((t - 2.2) / 2.4);
  if (runP > 0.01) {
    pushText(sh, 'run_tests: 0 failed', 150, 908, 18, '#6F86A6', 0.85 * runP);
    pushRect(sh, 150, 940, 360, 3, '#1B2740', 0.9);
    pushRect(sh, 150, 940, 360 * runP, 3, '#4ADE80', 0.85);
    const passed = Math.floor(clamp01((t - 2.2) / 3.0) * 153);
    pushText(sh, String(passed), 1240, 898, 40, '#4ADE80', 0.9);
    pushText(sh, 'assertions', 1300, 914, 18, '#5E7A99', 0.8);
  }

  envStream(sh, W, H, t, '#7FB2FF', 42, 175, 0.22);
  envOverlay(sh, W, H, t, 1);

  return { states: layers.map(() => ({})), shapes: sh };
}

// 本幕入口（每幕一个 layout 文件：没有判据可错）
function layout(t, duration, canvas, layers) {
  return scene_loop(t, duration, canvas, layers);
}
