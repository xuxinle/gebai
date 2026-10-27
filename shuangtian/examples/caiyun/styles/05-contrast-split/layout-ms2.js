// 裁云可编排脚本（layout）—— 宿主只提供这些名字，写错就整份**静默降级**为纯 JSON。
//
// 可用：ease(name,t) spring(tau[,zeta,omega]) spring_vel settle
//       lerp(a,b,k) clamp(v,lo,hi) snap(t,fps) noise(seed)
// 不可用：seg / E.* / D() —— 用了就 ReferenceError → 降级 → 画面停在初始帧且零报错。
//
// LayerDelta 字段：dx dy dw dh scale rotate alpha tint
//   —— **改不了 fill**。要"点亮"必须底下垫一层亮色块，改它的 alpha（实测坑）。
//
// 返回必须是 { states, shapes }，不是裸数组。
function seg(t, t0, t1) {
  if (t1 === t0) return (t >= t1) ? 1 : 0;
  const k = (t - t0) / (t1 - t0);
  return k < 0 ? 0 : (k > 1 ? 1 : k);
}
function expoOut(u) { return (u >= 1) ? 1 : 1 - Math.pow(2, -9 * u); }
function expoInOut(u) {
  if (u <= 0) return 0;
  if (u >= 1) return 1;
  return (u < 0.5) ? (u * 2) * (u * 2) * (u * 2) / 2 : 1 - Math.pow(2, -9 * (1 - u)) / 2;
}
function mk(o) {
  const a = (o.alpha === undefined) ? 1 : o.alpha;
  return {
    dx: o.dx || 0, dy: o.dy || 0,
    dw: o.dw || 0, dh: o.dh || 0,
    scale: (o.scale === undefined) ? 1 : o.scale,
    rotate: o.rotate || 0,
    alpha: a < 0 ? 0 : (a > 1 ? 1 : a),
  };
}

// 幕 2：中线收缩成一条细横线，两态上下排布 —— 收束。
//
// ## 承接终点
// carry 把幕 1 那条 240×600 的竖带，变形（旋转感的宽高互换）成 1320×12 的横线。
// 它落在 `hr`（x300 y534 w1320 h12）的位置——所以布局里**又画了一次**
// `hr`，这是有意的：`carry_out.fade_out: 0.35` 会让 carry 的图元在
// 变形完成后淡出，由本幕自己的 `hr` 接力。两者在交接瞬间**位置尺寸完全一致**，
// 所以看不出接缝（若不一致，会出现一帧"跳动"）。
//
// ## 两态上下排布的语义
// 竖着并置（左右）→ 横着并置（上下），**并置本身没有变**。
// 这是本模板唯一允许的"布局变化"：轴的朝向可以变，两态永远同框不能变。
//
// ## 本幕的运动
// `hr` 从中间向两端展开（dw 双向 + dx 反向一半），
// 然后两态卡片**逐个从 hr 上下浮出**（先 A 后 B，间隔 0.18s）。
// 落定后允许完全静止——三幕里只有这一幕可以（收尾留 ≥1s 静止）。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 hr  1 title2
  // 2 A2c  3 Ah2
  // 4 B2c  5 Bh2
  // 6 sub2

  // hr：从中间向两端展开（这是上一幕竖带"转正"的延续动作）
  const r = expoInOut(seg(t, 0.10, 0.78));
  st[0] = mk({
    dw: -1320 * (1 - r),
    dx: 660 * (1 - r),
    alpha: r > 0 ? 1 : 0,
  });

  const ti = expoOut(seg(t, 0.20, 0.80));
  st[1] = mk({ dy: (1 - ti) * 18, alpha: ti });

  // A 态卡片：从 hr 向上浮出
  const a = expoOut(seg(t, 0.62, 1.24));
  st[2] = mk({ dy: (1 - a) * 40, alpha: a });
  st[3] = mk({ dy: (1 - a) * 40, alpha: a });

  // B 态卡片：比 A 晚 0.18s 向下浮出（"上下并置"的建立动作）
  const b = expoOut(seg(t, 0.80, 1.42));
  st[4] = mk({ dy: (1 - b) * 40, alpha: b });
  st[5] = mk({ dy: (1 - b) * 40, alpha: b });

  const s = expoOut(seg(t, 1.02, 1.62));
  st[6] = mk({ dy: (1 - s) * 12, alpha: s });

  // 1.62s 之后完全静止（关键元素落定后留 ≥1s 呼吸）。
  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
