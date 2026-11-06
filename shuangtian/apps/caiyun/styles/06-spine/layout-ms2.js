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

function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 rule  1 title  2 sub
  // 横线向两侧展开：dw 双向 + dx 反向一半
  const r = expoInOut(seg(t, 0.10, 0.72));
  st[0] = mk({ dw: -600 * (1 - r), dx: 300 * (1 - r), alpha: r });

  const ti = expoOut(seg(t, 0.38, 0.98));
  st[1] = mk({ dy: (1 - ti) * 18, alpha: ti });

  const s = expoOut(seg(t, 0.58, 1.15));
  st[2] = mk({ dy: (1 - s) * 12, alpha: s });

  // 1.15s 之后完全静止（rest ≥ 1s：关键元素落定后必须呼吸）
  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
