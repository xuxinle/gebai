// 歌白 · 一镜到底 —— "一个点变成一切"
//
// 对标 onetake 的 one-dot。学的是它的**方法论**，不是照搬代码：
// · 元素小、画面大（70% 是空场）
// · 动作克制，靠时序而非幅度取胜
// · **必须有一段接近静止**（停顿才让动作落得下来）
// · 每个接缝有东西活下来（全片就是那个点）
//
// ## 与 onetake 的关键差异（技术不同）
//
// 它用 HTML+CSS+Playwright 截图；我们用 C++ 自绘渲染器 + 这个 JS 编排层。
// 它的动作用 39 个实测动作库；我们用缓动函数 + 关键帧，并刚补上逐字排版。

// 多段关键帧插值：list = [[t, v, ease], ...]
function keys(t, list) {
  if (!list.length) return 0;
  if (t <= list[0][0]) return list[0][1];
  for (let i = 0; i + 1 < list.length; i++) {
    const [ta, va] = list[i];
    const [tb, vb, e] = list[i + 1];
    if (t <= tb) {
      if (tb <= ta) return vb;
      const u = (t - ta) / (tb - ta);
      // ease() 是宿主函数（C++ 侧的实测曲线）
      const k = !e || e === "linear" ? u : ease(e, u);
      return va + (vb - va) * k;
    }
  }
  return list[list.length - 1][1];
}

// 弹簧：二阶阻尼振子。onetake 的 spring(t, ζ, ω) 同构。
// 为什么不用 ease("back")：back 是固定过冲量，而弹簧的过冲随 ζ 变，
// 能精确控制"冲多远、多久停"——这是"有质量"的关键。
function spring(t, zeta, omega) {
  if (t <= 0) return 0;
  const wd = Math.sqrt(Math.max(0.0001, 1 - zeta * zeta)) * omega;
  const e = Math.exp(-zeta * omega * t);
  return 1 - e * (Math.cos(wd * t) + (zeta * omega / wd) * Math.sin(wd * t));
}

const W = 1920, H = 1080;
const CX = W * 0.5, CY = H * 0.5;

function layout(t, duration, canvas, layers) {
  const out = layers.map(() => ({}));
  // 索引对应 JSON layers：
  // 0 bg 1 glow 2 curve 3 ring 4 dot 5 cmd 6 word 7 sub
  const BG = 0, GLOW = 1, CURVE = 2, RING = 3, DOT = 4, CMD = 5, WORD = 6, SUB = 7;

  // ══════ 节拍 1｜0.0–1.1 点浮现（弹簧弹出）══════
  // 弹簧 ζ=0.5 / ω=26：快、有过冲、约 0.35s 稳。
  const pop = spring(Math.max(0, t - 0.12), 0.5, 26);

  // ══════ 节拍 2｜1.1–2.4 点滑向左，拉成光标 ══════
  // 注意：位移幅度**小**（约 260px），这是克制的关键。
  const dotX = keys(t, [
    [0.0, CX], [1.1, CX], [2.3, CX - 250, "in_out"],
    [2.4, CX - 250], [4.6, CX - 250],
    [5.6, CX - 40, "in_out"], [6.6, CX, "in_out"],
    [10.6, CX], [11.0, CX - 30, "out"], [12.2, CX],
    [13.4, CX], [14.6, CX + 6, "in_out"],
  ]);
  const dotY = keys(t, [
    [0.0, CY], [2.3, CY - 6, "in_out"], [4.6, CY - 6],
    [5.6, CY + 14, "in_out"], [6.6, CY + 26, "in_out"],
    [10.6, CY + 26], [11.0, CY + 34, "out"], [12.2, CY + 20],
    [13.4, CY + 20], [14.6, CY + 24, "in_out"],
  ]);

  // 尺寸：18px 起（小！）。光标期变细长，球期变大。
  const dotW = keys(t, [
    [0.0, 18], [1.1, 18], [2.3, 5, "in_out"], [4.6, 5],
    [5.6, 16, "out"], [6.6, 26, "out"], [10.6, 26],
    [11.0, 34, "out"], [12.2, 18, "in_out"], [14.6, 20],
  ]);
  const dotH = keys(t, [
    [0.0, 18], [1.1, 18], [2.3, 46, "in_out"], [4.6, 46],
    [5.6, 16, "out"], [6.6, 26, "out"], [10.6, 26],
    [11.0, 34, "out"], [12.2, 18, "in_out"], [14.6, 20],
  ]);

  // 落地压扁：体积守恒（Y 压则 X 鼓）
  const squash = keys(t, [[10.55, 0], [10.68, 1, "in"], [10.95, 0, "out"]]);
  const sy = 1 - 0.26 * squash;
  const sx = 1 / Math.sqrt(Math.max(0.55, sy));

  // ══════ 点：主体（几乎全片都在）══════
  const dotA = keys(t, [
    [0.0, 0], [0.12, 0], [0.5, 1, "out"],
    [6.0, 1], [6.5, 0],          // 环接管
    [6.9, 0], [7.3, 1, "out"],   // 珠子出现
    [12.6, 1], [13.2, 0.85, "out"], [14.6, 0.9],
  ]);
  if (out[DOT]) {
    out[DOT] = {
      dx: dotX - CX,
      dy: dotY - CY,
      dw: dotW * sx * pop - 18,
      dh: dotH * sy * pop - 18,
      alpha: dotA * Math.min(1, pop * 2.2),
    };
  }

  // ══════ 节拍 3｜4.6–6.6 点变环（加载）══════
  const ringA = keys(t, [[4.9, 0], [5.5, 0.9, "out"], [6.2, 0.9], [6.6, 0, "in"]]);
  const ringSpin = keys(t, [[5.1, 0], [6.4, 300, "in_out"]]);
  if (out[RING]) {
    out[RING] = {
      alpha: ringA,
      dw: (dotW * 2.2) - 115,
      dh: (dotH * 1.5) - 108,
      dx: keys(t, [[4.9, 0], [5.5, -8, "out"]]),
      dy: keys(t, [[4.9, 0], [5.5, 6, "out"]]),
      rotate: ringSpin,
    };
  }

  // ══════ 节拍 4｜6.6–10.5 曲线 + 珠子（点沿曲线跑）══════
  // 曲线是一段衰减正弦（spring 的响应形状），从环展开
  const curveA = keys(t, [[6.5, 0], [7.0, 1, "out"], [10.4, 1], [10.8, 0, "in"]]);
  if (out[CURVE]) {
    out[CURVE] = { alpha: curveA };
  }

  // ══════ 节拍 5｜10.5 第一次重击：球砸下、压扁、弹起 ══════
  // 已在 dot 的 sy/sx 里处理压扁；这里给一个额外的"下沉再回"。
  const hitDip = keys(t, [[10.45, 0], [10.62, 16, "in"], [10.94, 0, "out"]]);
  if (out[DOT]) out[DOT].dy += hitDip;

  // ══════ 节拍 6｜11.0–12.2 球变太阳（光晕）══════
  const glowA = keys(t, [[10.7, 0], [11.4, 0.6, "out"], [12.0, 0.5], [12.8, 0.12, "in"]]);
  if (out[GLOW]) {
    out[GLOW] = { alpha: glowA, dx: dotX - CX, dy: dotY - CY };
  }

  // ══════ 节拍 7｜2.4–4.6 静态段：/ 命令 + 光标 ══════
  // onetake 的硬规则：必须有接近静止的段落。这里 2.2 秒只有光标闪。
  const cmdA = keys(t, [[2.2, 0], [2.6, 1, "out"], [4.7, 1], [5.1, 0, "in"]]);
  if (out[CMD]) out[CMD] = { alpha: cmdA };

  // ══════ 节拍 8｜12.6–15.0 字标逐字入场 ══════
  const wordA = keys(t, [[12.4, 0], [12.7, 1, "out"], [15.0, 1]]);
  const subA = keys(t, [[13.4, 0], [13.7, 1, "out"], [15.0, 1]]);
  if (out[WORD]) out[WORD] = { alpha: wordA };
  if (out[SUB]) out[SUB] = { alpha: subA };

  // ══════ 背景：极缓的明暗呼吸（防止全黑死板）══════
  if (out[BG]) {
    out[BG] = { alpha: keys(t, [[0.0, 0.6], [1.0, 1, "in_out"], [10.5, 1], [11.6, 0.86, "in_out"], [15.0, 0.92]]) };
  }

  return out;
}
