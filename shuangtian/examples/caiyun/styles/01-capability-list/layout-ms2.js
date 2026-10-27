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

// 幕 2：硬切收束——光标停在角落，画面安静下来等它。
//
// ## 承接关键
// 幕 1 的 carry_out 把「光标·本体」送到：
//   to = { x:"71.98%", y:"75.93%", w:"1.35%", h:"2.41%", radius:13 }
// 这一幕不画光标（由 carry 绘制），只在它旁边落文字——
// 文字必须落在光标**周围**，否则观众会以为"光标没了、换了一页"。
//
// ## 收尾
// 本模板以"硬切"收束，所以这里**不需要**再补动效铺垫：
// 文字落定后保持静止即可。三幕里只有这一幕允许全静止。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 title2  1 sub2
  // 文字放在光标左上侧（光标在 72%/76%），从右侧滑入，形成"说完就走"的收束感。

  const ti = expoOut(seg(t, 0.20, 0.85));
  st[0] = mk({ dx: (1 - ti) * 26, alpha: ti });

  const s = expoOut(seg(t, 0.45, 1.05));
  st[1] = mk({ dx: (1 - s) * 18, alpha: s });

  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
