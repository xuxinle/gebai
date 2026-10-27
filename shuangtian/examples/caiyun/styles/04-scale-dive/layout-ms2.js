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

// 幕 2：整页缩成一枚胶囊。文字在它下方落定。
//
// ## 承接物必须自己画（实测踩过）
// `capsule` 这一层是为了让**胶囊常在**——carry 只在 6.1~6.8s 那段画它，
// 之后什么都不画。第一版没画它，第 275 帧（9.2s）整屏只剩两行字，
// 胶囊凭空消失，"它一直在"这句文案就变成了反讽。
// 它的值与 carry_out.to 完全一致：42%,46%,16%,8%,radius 999。
//
// ## 本模板的落点
// "小方块 → 整页 → 胶囊"三段都是**同一个矩形换尺寸**，全片没有第二个图形
// 元素参与承接。这是 scale-dive 的全部。
//
// ## 收尾允许全静止
// 关键元素落定后要留 ≥1s 静止（节奏偏好单向：只会嫌快）。
// 本幕文字 1.10s 落定后余下 1.9s 不动——这是有意的，
// 三幕里只有这一幕允许这样（幕 0/1 都有持续运动）。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 capsule  1 title2  2 sub2

  // 胶囊：同样需要**避双影**——carry_out 淡出（fade_out 0.35），本层淡入。
  // 时间：carry 的 at=6.1（成片时刻）跨边界到 6.8s，本幕 t=0 对应 6.4s，
  // 所以本层 t=0.05~0.45 淡入，与 carry 的淡出重叠。
  const cpIn = expoOut(seg(t, 0.05, 0.45));
  const cpBreath = 0.93 + 0.07 * (0.5 + 0.5 * Math.sin(t * 2.2));
  st[0] = mk({ alpha: cpIn * cpBreath });

  const ti = expoOut(seg(t, 0.30, 0.95));
  st[1] = mk({ dy: (1 - ti) * 22, alpha: ti });

  const s = expoOut(seg(t, 0.55, 1.10));
  st[2] = mk({ dy: (1 - s) * 12, alpha: s });

  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
