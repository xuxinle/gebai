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

// 幕 2：线已成面（由 carry 接住那个矩形），面里长出文字。
//
// ## 本模板最后的落点
// "点 → 线 → 面"三步全程只有一个元素在变形。这一幕面已经在了（carry 交付），
// 本脚本只负责让文字从面里出来——仍然不引入第二个图形元素。
//
// ## 收尾必须静止
// 关键元素落定后留 ≥1s 静止（节奏偏好是单向的：只会嫌快）。
// 所以文字 1.10s 落定后，本幕余下 1.9s 完全不动——这是**有意的**。
// 实测提醒：验证器的「静止占比」对这类收尾给高分是正常的（它本就不该动）；
// 但若整片每一幕都这样，就会读成卡住。三幕里**只有这一幕**允许全静止。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 title2  1 sub2

  const ti = expoOut(seg(t, 0.30, 0.95));
  st[0] = mk({ dy: (1 - ti) * 22, alpha: ti });

  const s = expoOut(seg(t, 0.55, 1.10));
  st[1] = mk({ dy: (1 - s) * 12, alpha: s });

  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
