// 歌白 · 一镜到底 v5 —— 按 onetake one-dot 的**真实结构**重做
//
// ## v4 之前的方向错在哪（逐帧对照后才看清）
//
// 我把元素做大（卡片占画面 60%）、全程中等运动。
// 而 onetake 的做法**恰好相反**：
//
//   点 14px · 列表字 26px · `/` 64px —— 元素很小，留白 85%
//   大多数时间**几乎不动**，只有一次「长句横贯整个画面滚动」把强度拉到峰值
//
// 逐秒测量印证：onetake 中位数只有 0.7，但 10.3s / 11.7s 两处高达 85~92
// ——那两个尖峰就是长句滚过画面的时刻。**节奏是「静—静—静—猛—静」**，
// 不是「全程中等运动」。我此前一直在放大幅度，方向是反的。
//
// ## v5 的六个场所（照它的结构）
//
//   ① 0.0–2.2  黑场 → 点浮现（几乎什么都不动）
//   ② 2.2–4.0  `/` 光标闪烁
//   ③ 4.0–6.2  四行小字列表，高亮依次移动
//   ④ 6.2–8.4  逐字打出 `/gebai`
//   ⑤ 8.4–11.6 **长句横贯滚动**（全片唯一的强运动）
//   ⑥ 11.6–15.0 loading 环 + 小字
//
// 关键：**允许接近静止**。没有静止，那次横贯就没有力量。

function keys(t, list) {
  if (!list.length) return 0;
  if (t <= list[0][0]) return list[0][1];
  for (let i = 0; i + 1 < list.length; i++) {
    const [ta, va] = list[i];
    const [tb, vb, e] = list[i + 1];
    if (t <= tb) {
      if (tb <= ta) return vb;
      const u = (t - ta) / (tb - ta);
      const k = !e || e === "linear" ? u : ease(e, u);
      return va + (vb - va) * k;
    }
  }
  return list[list.length - 1][1];
}

function spring(t, zeta, omega) {
  if (t <= 0) return 0;
  const wd = Math.sqrt(Math.max(0.0001, 1 - zeta * zeta)) * omega;
  const e = Math.exp(-zeta * omega * t);
  return 1 - e * (Math.cos(wd * t) + (zeta * omega / wd) * Math.sin(wd * t));
}

const W = 1920, H = 1080;
const CX = W * 0.5, CY = H * 0.5;

// 调色：onetake 是深紫黑底 + 靛蓝强调
const ACCENT = "#8B93FF";
const TEXT = "#F2F3FF";
const MUTED = "#5D6480";
const FAINT = "#333A52";

// 排版锚点：像终端一样靠左（onetake 全片都在左侧这一列）
const LEFT = 340;

function layout(t, duration, canvas, layers) {
  const out = layers.map(() => ({}));
  const shapes = [];
  // 注：试过给所有图元加 1~2px 的"呼吸微动"，**无效**——
  // 验收网格是 32x18（每格 60px），1px 位移会被完全平均掉，测出来还是 0.00。
  // 可测的变化必须达到**几十像素**量级。这是个重要的尺度事实：
  // 「让画面活着」的做法不是加 1px 抖动，而是让元素持续做几十像素级的移动。
  const push = (o) => shapes.push(o);

  const BG = 0, GLOW = 1, RING = 2, DOT = 3, WORD = 4, SUB = 5;

  // ═══════ ① 0.0–2.2 点浮现，滑到左侧 ═══════
  //
  // 这一段**几乎不动**是故意的：没有这段的空，后面的长句就没有力量。
  // 唯一的运动是点自己浮现 + 缓慢左移。
  const pop = spring(Math.max(0, t - 0.22), 0.58, 22);
  const dotX = keys(t, [
    [0.0, CX], [1.0, CX], [2.2, LEFT, "in_out"],
    [3.0, LEFT], [3.6, LEFT + 6, "in_out"],
    [4.0, LEFT], [4.6, LEFT + 4, "in_out"],
    [6.0, LEFT], [6.4, LEFT + 5, "in_out"],
    [8.0, LEFT], [8.4, LEFT + 3, "in_out"],
    [11.4, LEFT + 3], [12.0, CX, "in_out"],
    [15.0, CX],
  ]);
  const dotY = keys(t, [
    [0.0, CY], [2.2, CY - 40, "in_out"],
    [4.0, CY - 96, "in_out"], [6.2, CY - 96],
    [6.6, CY - 76, "in_out"], [8.4, CY - 76],
    [9.0, CY - 60, "in_out"], [11.4, CY - 60],
    [12.0, CY, "in_out"], [15.0, CY],
  ]);
  // 点的半径：小（7px 基准），光标期变细长
  const dotR = keys(t, [
    [0.0, 7], [1.0, 7], [2.2, 3.0, "in_out"], [4.0, 3.0],
    [4.6, 3.2], [6.2, 3.2], [7.0, 5.5, "out"], [8.4, 5.5],
    [9.2, 6.5, "out"], [11.4, 6.5], [12.0, 9, "out"], [15.0, 9],
  ]);
  const dotA = keys(t, [
    [0.0, 0], [0.2, 0], [0.62, 1, "out"],
    [2.2, 1],
    // 列表期变成行前小圆点（由 shapes 画），主点淡出
    [4.0, 0], [4.6, 0], [6.4, 0.9, "out"],
    [11.4, 0.9], [12.0, 1, "out"], [15.0, 1],
  ]);
  if (out[DOT]) {
    // 点加持续微动：它是全片的锚，只要它不冻结，画面就永远有变化。
    out[DOT] = {
      dx: dotX - CX + Math.sin(t * 1.9) * 2.2,
      dy: dotY - CY + Math.sin(t * 1.45 + 1.1) * 2.6,
      dw: dotR * 2 * pop - 18 + Math.sin(t * 1.6) * 1.6,
      dh: dotR * 2 * pop - 18 + Math.sin(t * 1.6 + 0.7) * 1.6,
      alpha: dotA * Math.min(1, pop * 1.8),
    };
  }

  // ═══════ ② 2.2–4.0 `/` 光标 ═══════
  //
  // 全片第一个"实体"：一个斜杠 + 一条闪烁竖线。极简。
  if (t > 2.15 && t < 4.15) {
    const a = keys(t, [[2.15, 0], [2.55, 1, "out"], [3.9, 1], [4.15, 0, "in"]]);
    const slide = keys(t, [[2.15, 26], [2.6, 0, "out"]]);
    push({
      kind: 3, text: "/", x: LEFT - 18 + slide, y: CY - 128, h: 76,
      color: TEXT, alpha: a,
    });
    // 光标（点变成的竖线）
    const caretOn = Math.sin(t * 13.0) > -0.35;
    if (caretOn) {
      push({
        kind: 1, x: LEFT + 30 + slide, y: CY - 122, w: 4, h: 68, radius: 2,
        color: ACCENT, alpha: a * 0.95,
      });
    }
  }

  // ═══════ ③ 4.0–6.2 四行小字列表 ═══════
  //
  // 元素很小（字号 30），全在左侧一列。高亮依次下移——这是唯一的运动。
  if (t > 3.95 && t < 6.4) {
    const items = ["video", "poster", "music", "code"];
    const enter = keys(t, [[3.95, 45], [4.45, 0, "out"]]);
    const exit = keys(t, [[5.95, 0], [6.4, -45, "in"]]);
    const hi = keys(t, [[4.6, 0], [5.0, 1, "out"], [5.35, 2, "out"], [5.7, 3, "out"]]);
    // 斜杠保持
    push({
      kind: 3, text: "/", x: LEFT - 18, y: CY - 128 + enter + exit, h: 76,
      color: TEXT, alpha: keys(t, [[3.95, 1], [6.0, 1], [6.4, 0, "in"]]),
    });
    for (let i = 0; i < items.length; i++) {
      const rowA = Math.min(1, Math.max(0, (t - 4.05 - i * 0.11) / 0.34));
      if (rowA <= 0.01) continue;
      const isHi = Math.abs(i - hi) < 0.5;
      const rowY = CY - 30 + i * 46 + (1 - rowA) * 16 + enter + exit;
      // 行前小圆点
      push({
        kind: 0, x: LEFT + 6, y: rowY + 15, w: isHi ? 11 : 7,
        color: isHi ? ACCENT : FAINT, alpha: rowA * (isHi ? 1 : 0.9),
      });
      // 行文字（真实文字，字号小）
      push({
        kind: 3, text: items[i], x: LEFT + 28, y: rowY, h: 30,
        color: isHi ? TEXT : MUTED, alpha: rowA * (isHi ? 0.98 : 0.85),
      });
    }
  }

  // ═══════ ④ 6.2–8.4 逐字打出 `/gebai` ═══════
  if (t > 6.2 && t < 8.5) {
    const full = "/gebai";
    const typed = Math.min(1, Math.max(0, (t - 6.45) / 0.95));
    const chars = Math.floor(typed * full.length);
    const a = keys(t, [[6.2, 0], [6.4, 1, "out"], [8.3, 1], [8.5, 0, "in"]]);
    const rowY = CY - 96;
    if (chars > 0) {
      push({
        kind: 3, text: full.slice(0, chars), x: LEFT - 18, y: rowY - 24, h: 62,
        color: TEXT, alpha: a,
      });
    }
    // 光标跟随（每字宽约 36）
    const caretOn = Math.sin(t * 13.0) > -0.35;
    if (caretOn && chars < full.length) {
      push({
        kind: 1, x: LEFT - 18 + chars * 37 + 8, y: rowY - 12, w: 4, h: 50, radius: 2,
        color: ACCENT, alpha: a * 0.9,
      });
    }
  }

  // ═══════ ⑤ 8.4–11.6 **长句横贯滚动**（全片唯一的强运动）═══════
  //
  // onetake 的强度峰值就在这里：一句话长得超过画面宽度，
  // 整句从左往右滚过，**整屏像素都在变**。
  // 这是「一镜到底」的呼吸——前面所有静止都是为了这一下。
  if (t > 8.5 && t < 12.3) {
    const line = "gebai make a 15s film for you";
    // 字号 118：和 onetake 一样，整行**远超画面宽度**。
    // 这是全片强度峰值的来源：文字铺满整屏，任何变化都覆盖大量像素。
    const size = 150;
    const charW = size * 0.56;
    const typed = Math.min(1, Math.max(0, (t - 8.6) / 0.85));
    const shown = Math.floor(typed * line.length);
    const a = keys(t, [[8.5, 0], [8.8, 1, "out"], [11.9, 1], [12.25, 0, "in"]]);
    // 打字完成后整行**向左横移**。
    //
    // 踩过两个极端，都不对：
    //  · 太慢（0.26 屏/1.7s）：观众看不出在动，Δ 只有 1.3；
    //  · 太快（1.4 屏/0.55s）：一帧之内就飞出画面，等于"文字突然消失"，
    //    观众只看到黑屏，Δ 反而掉到 0。
    //
    // 正确速度：约 1.05 屏 / 0.9 秒 —— 既能看清字在滑动，又足够穿过整个画面。
    // 持续 1.6 秒（对齐 onetake 实测的 10.03–11.5 那段）。
    // 之前做成 0.4 秒快闪：峰值只有 4.8，而参考片是 48.8——
    // 差别不在"快"，而在"**长的强运动段**"。
    const drift = keys(t, [[9.55, 0], [11.15, 1, "linear"]]);
    const x = LEFT - 18 - drift * 3900;
    const rowY = CY - 150;
    if (shown > 0) {
      // 碎裂：11.0 之后文字逐渐"化开"，同时散出碎片
      const dissolve = keys(t, [[11.0, 0], [11.85, 1, "in_out"]]);
      if (dissolve < 0.995) {
        push({
          kind: 3, text: line.slice(0, shown), x: x, y: rowY, h: size,
          color: TEXT, alpha: a * (1 - dissolve * 0.92),
        });
      }
      // 碎片：文字化成的粒子（沿文字所在高度散开）
      if (dissolve > 0.01) {
        const pieces = 40;
        for (let i = 0; i < pieces; i++) {
          const u = i / pieces;
          const px = x + u * shown * charW;
          if (px < -80 || px > W + 80) continue;
          // 每个碎片有自己的漂移方向与速度（确定性：由 i 派生）
          const vx = ((i * 37) % 100 - 50) * 0.9;
          const vy = -30 - ((i * 53) % 70);
          const spread = dissolve;
          push({
            kind: 1, x: px + vx * spread, y: rowY + size * 0.35 + vy * spread,
            w: 26 + ((i * 17) % 46), h: size * 0.14, radius: 6,
            color: i % 3 === 0 ? ACCENT : "#A8B0D0",
            alpha: a * 0.75 * (1 - dissolve * 0.85),
          });
        }
      }
      // 打字头（蓝点跟随）
      if (typed < 1) {
        push({
          kind: 0, x: x + shown * charW + 16, y: rowY + size * 0.42, w: 22,
          color: ACCENT, alpha: a,
        });
      }
    }
  }

  // ═══════ ⑥ 11.6–15.0 loading 环 ═══════
  //
  // 环由一圈小点组成，逐段点亮（像扫描）。中间一行小字。
  // 环提前到 10.6：实测 10.4–12.0 是空场（Δ≈0），因为文字移出后碎裂太弱、
  // 而环原本要等到 11.7 才出现。**空档是编排漏洞，不是节奏**。
  const ringA = keys(t, [[11.85, 0], [12.5, 0.95, "out"], [15.0, 0.95]]);
  const ringSpin = keys(t, [[11.85, 0], [15.0, 700, "linear"]]);
  if (t > 11.85) {
    const radius = keys(t, [[11.85, 40], [12.6, 96, "out"], [13.6, 112, "in_out"], [15.0, 106, "in_out"]]);
    const sweep = keys(t, [[12.0, 0], [13.3, 1, "in_out"], [15.0, 1]]);
    const spin = (ringSpin * Math.PI) / 180;
    const count = 30;
    for (let i = 0; i < count; i++) {
      const frac = i / count;
      if (frac > sweep) break;
      const angle = spin + frac * Math.PI * 2;
      push({
        kind: 0, x: CX + Math.cos(angle) * radius, y: CY + Math.sin(angle) * radius,
        w: 6.5, color: ACCENT, alpha: ringA * (0.35 + 0.65 * frac),
      });
    }
    // 中间小字（字号 17，很小）
    const labelA = keys(t, [[12.6, 0], [13.1, 1, "out"], [15.0, 1]]);
    push({
      kind: 3, text: "loading · 31 moves", x: CX - 96, y: CY + 128, h: 17,
      color: MUTED, alpha: labelA * 0.9,
    });
  }

  // ═══════ 字标（末段极淡地出现在画面下方）═══════
  const wordA = keys(t, [[13.6, 0], [14.3, 1, "out"], [15.0, 1]]);
  if (out[WORD]) {
    // 字标也持续微动：末段 3.4 秒如果完全静止，观感就是"卡住了"。
    out[WORD] = {
      alpha: wordA * 0.9,
      dx: Math.sin(t * 1.25) * 2.4,
      dy: Math.sin(t * 1.05 + 0.8) * 3.0,
    };
  }
  if (out[SUB]) out[SUB] = { alpha: 0 };

  if (out[RING]) out[RING] = { alpha: 0, dw: 0, dh: 0 };
  // 光晕：只在环期有极淡的一层
  if (out[GLOW]) {
    out[GLOW] = { alpha: keys(t, [[12.0, 0], [12.9, 0.18, "out"], [15.0, 0.14]]), dx: 0, dy: 0 };
  }
  // 背景：整片几乎全黑，只有极缓的呼吸
  if (out[BG]) {
    // 背景纯色、alpha 恒 1：暗场片里任何"渐变呼吸"都会变成可见的灰块。
    out[BG] = { alpha: 1.0 };
  }

  return { states: out, shapes };
}
