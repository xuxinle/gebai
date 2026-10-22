// 歌白 · 一镜到底 v2 —— 补齐「结构变化」后的版本
//
// ## 相对 v1 改了什么（v1 的问题：中段 8 秒只有点和光晕）
//
// v1 用固定图层做片子，而 JSON 图层是**静态集合**——只能改属性，不能增删元素。
// 所以"点分裂成四个""一排珠子沿曲线跑"这些**结构变化**做不出来，
// 中段只好让点慢慢变大，观感几乎是静止的。
//
// v2 用 `shapes` 出口：每帧返回一组临时图元（圆/线/短形/文字）。
// 于是这一版点会**分裂、排队、沿曲线跑、撞成粒子**——画面内容真的在变。
//
// ## 学 onetake 的三条硬规则
//
// ① 镜头长短差 ≥ 4 倍（本片最长 2.6s、最短 0.5s）
// ② 必须有一段接近静止（2.6–3.9s）
// ③ 每个接缝有东西活下来（那个点，以及它分裂出的子点）

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

// 弹簧：二阶阻尼振子（和 onetake 的 spring(t,ζ,ω) 同一物理模型）
function spring(t, zeta, omega) {
  if (t <= 0) return 0;
  const wd = Math.sqrt(Math.max(0.0001, 1 - zeta * zeta)) * omega;
  const e = Math.exp(-zeta * omega * t);
  return 1 - e * (Math.cos(wd * t) + (zeta * omega / wd) * Math.sin(wd * t));
}

const W = 1920, H = 1080;
const CX = W * 0.5, CY = H * 0.5;
const ACCENT = "#8E9EFF";
const DIM = "#5A6480";

function layout(t, duration, canvas, layers) {
  const out = layers.map(() => ({}));
  const shapes = [];
  // JSON 图层索引（必须与 onedot_v2.json 的 layers 顺序一致）：
  // 0 bg 1 glow 2 ring 3 dot 4 word 5 sub
  const BG = 0, GLOW = 1, RING = 2, DOT = 3, WORD = 4, SUB = 5;

  const push = (o) => shapes.push(o);

  // ════════ 节拍 1｜0.0–0.5 点弹出（最短拍 0.5s）════════
  const pop = spring(Math.max(0, t - 0.10), 0.5, 26);

  // ════════ 节拍 2｜0.5–2.2 点滑左 → 变光标 ════════
  // 位移幅度保持克制（250px），这是"高级感"的来源。
  const dotX = keys(t, [
    [0.0, CX], [1.0, CX], [2.2, CX - 230, "in_out"],
    [4.4, CX - 230], [5.4, CX - 20, "in_out"],
    [6.2, CX, "in_out"], [10.0, CX],
    [10.6, CX - 40, "out"], [11.6, CX],
    [12.6, CX], [14.8, CX + 8, "in_out"],
  ]);
  const dotY = keys(t, [
    [0.0, CY], [2.2, CY - 4], [4.4, CY - 4],
    [5.4, CY + 10, "in_out"], [6.2, CY + 22, "in_out"],
    [10.0, CY + 22], [10.6, CY + 30, "out"], [11.6, CY + 16],
    [12.6, CY + 16], [14.8, CY + 20, "in_out"],
  ]);
  const dotR = keys(t, [
    [0.0, 9], [1.0, 9], [2.2, 2.5, "in_out"], [4.4, 2.5],
    [5.4, 8, "out"], [6.2, 13, "out"], [10.0, 13],
    [10.6, 16, "out"], [11.6, 9, "in_out"], [14.8, 10],
  ]);

  // ════════ 节拍 3｜2.2–4.4 **静止段**（2.2s，全片最静的拍）════════
  // onetake 的硬规则：没有静止，动作就落不下来。
  // 这里只有一个光标在极缓慢地呼吸。
  const breath = Math.sin(t * 1.15) * 0.5 + 0.5;

  // ════════ 节拍 4｜4.4–6.2 点**分裂成 5 个**（结构变化！）════════
  // 这是 v1 做不出来的东西：子点数量随时间变化。
  if (t > 4.55 && t < 7.0) {
    const splitT = Math.min(1, Math.max(0, (t - 4.55) / 0.55));
    const s = spring(splitT * 0.6, 0.62, 18);
    // 5 个子点从中心散开，呈扇形
    for (let i = 0; i < 5; i++) {
      const angle = -Math.PI * 0.5 + (i - 2) * 0.42;
      const dist = 90 * s + (i - 2) * 6;
      // 6.2 之后它们收拢（第二次结构变化：数量不变但位置聚回）
      const converge = keys(t, [[6.35, 0], [7.0, 1, "in_out"]]);
      const shrink = 1 - converge * 0.85;
      const px = dotX + Math.cos(angle) * dist * shrink;
      const py = dotY + Math.sin(angle) * dist * shrink;
      const fade = keys(t, [[4.5, 0], [4.9, 1, "out"], [6.6, 1], [7.0, 0, "in"]]);
      if (fade > 0.01) {
        push({ kind: 0, x: px, y: py, w: 9 * s * fade, color: ACCENT, alpha: fade });
      }
    }
  }

  // ════════ 节拍 5｜6.2–7.0 点变环（加载）════════
  const ringA = keys(t, [[6.15, 0], [6.55, 0.95, "out"], [7.0, 0.85], [7.5, 0, "in"]]);
  const ringSpin = keys(t, [[6.2, 0], [7.4, 320, "in_out"]]);

  // ════════ 节拍 6｜7.0–9.6 **珠子沿曲线跑**（结构变化！）════════
  // 曲线用圆点密集排布画出来（不是一条线）——onetake 的片子里就是这样。
  if (t > 7.0 && t < 10.2) {
    const drawIn = keys(t, [[7.0, 0], [7.9, 1, "out"]]);
    const count = 46;
    for (let i = 0; i < count; i++) {
      const u = i / (count - 1);
      if (u > drawIn) break;
      // 衰减正弦（spring 的响应形状）
      const amp = 120 * Math.exp(-1.9 * u);
      const bx = 380 + u * 1150;
      const by = 640 - amp * Math.sin(u * Math.PI * 3.1);
      push({ kind: 0, x: bx, y: by, w: 4.2, color: DIM, alpha: 0.85 });
    }
    // 珠子（一个亮点沿同一曲线跑，领先/滞后制造"跟随"感）
    const beadT = Math.min(1, Math.max(0, (t - 7.35) / 1.9));
    const bu = beadT;
    const bamp = 120 * Math.exp(-1.9 * bu);
    const beadX = 380 + bu * 1150;
    const beadY = 640 - bamp * Math.sin(bu * Math.PI * 3.1);
    const beadA = keys(t, [[7.3, 0], [7.6, 1, "out"], [9.3, 1], [9.7, 0, "in"]]);
    if (beadA > 0.01) {
      // 拖尾：3 个递减的圆跟着
      for (let k = 1; k <= 3; k++) {
        const tu = Math.max(0, bu - k * 0.018);
        const tamp = 120 * Math.exp(-1.9 * tu);
        push({
          kind: 0, x: 380 + tu * 1150, y: 640 - tamp * Math.sin(tu * Math.PI * 3.1),
          w: 11 - k * 2.4, color: ACCENT, alpha: beadA * (0.5 - k * 0.12),
        });
      }
      push({ kind: 0, x: beadX, y: beadY, w: 15, color: "#C9D3FF", alpha: beadA });
    }
  }

  // ════════ 节拍 7｜9.6–10.6 落地：**撞散成粒子**（结构变化！）════════
  //
  // 这是 causal carry：珠子落下 → 炸开 → 碎片汇聚成光晕。
  // 粒子不是装饰，而是"撞击的结果"（onetake 说"因果必须可见"）。
  if (t > 9.55 && t < 11.2) {
    const bt = Math.max(0, t - 9.55);
    const spread = spring(bt, 0.72, 9);
    const fade = keys(t, [[9.55, 1], [10.7, 0.25, "in"], [11.2, 0, "in"]]);
    const count = 18;
    for (let i = 0; i < count; i++) {
      const angle = (i / count) * Math.PI * 2 + 0.3;
      const dist = 150 * spread * (0.7 + 0.3 * ((i * 7) % 5) / 5);
      push({
        kind: 0, x: CX + Math.cos(angle) * dist, y: CY + 30 + Math.sin(angle) * dist * 0.6,
        w: 7 * (1 - bt * 0.28), color: ACCENT, alpha: fade,
      });
    }
  }

  // ════════ 节拍 8｜10.6–12.6 光晕升起（球变太阳）════════
  const glowA = keys(t, [[10.3, 0], [11.2, 0.62, "out"], [12.0, 0.45], [12.8, 0.1, "in"]]);

  // ════════ 节拍 9｜12.6–15.0 **字标**（最长拍 2.6s）════════
  const wordA = keys(t, [[12.4, 0], [12.7, 1, "out"], [15.0, 1]]);
  const subA = keys(t, [[13.5, 0], [13.8, 1, "out"], [15.0, 1]]);

  // ════════ 点本体 ════════
  const dotA = keys(t, [
    [0.0, 0], [0.10, 0], [0.45, 1, "out"],
    [6.2, 1], [6.6, 0],
    [7.0, 0], [7.35, 1, "out"],
    [12.5, 1], [13.0, 0.75, "out"], [14.8, 0.85],
  ]);
  if (out[DOT]) {
    out[DOT] = {
      dx: dotX - CX,
      dy: dotY - CY,
      dw: dotR * 2 * pop - 18,
      dh: dotR * 2 * pop - 18,
      alpha: dotA * Math.min(1, pop * 2.2),
    };
  }
  if (out[RING]) {
    out[RING] = {
      alpha: ringA,
      dw: dotR * 4.6 - 115,
      dh: dotR * 3.4 - 108,
      rotate: ringSpin,
    };
  }
  if (out[GLOW]) {
    out[GLOW] = { alpha: glowA, dx: dotX - CX, dy: dotY - CY };
  }
  if (out[WORD]) out[WORD] = { alpha: wordA };
  if (out[SUB]) out[SUB] = { alpha: subA };
  if (out[BG]) {
    out[BG] = {
      alpha: keys(t, [[0.0, 0.55], [1.2, 1, "in_out"], [10.0, 1], [11.2, 0.84, "in_out"], [15.0, 0.9]]),
    };
  }

  return { states: out, shapes };
}
