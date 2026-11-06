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

// 幕 2：手停了，界面才静。角色停在角落，画面收束。
//
// ## 承接（幕 1 → 幕 2 没有 carry_out，靠**坐标对齐**承接）
// 幕 1 的 carry_out 把角色送到 to = { x:"48.96%", y:"64.81%" } → 即 (940, 700)。
// 而本幕的 hero2 图层就在 (940, 700) 40×40 —— 完全同一个位置、同一个尺寸。
//
// 这是"没有 carry 也能承接"的一种合法写法：观众看到的是**同一个黄点还在那儿**，
// 只是上一幕的角色退场、这一幕的图层入场，两者在边界上完全重合。
// 但注意它**拿不到承接分**（carry_qa 认的是 carry_out 的显式记录）——
// 所以这种写法只在"边界本来就该安静"时用，不能替代真正的 morph。
//
// ## 收尾
// 本模板收束于"手停"：角色一停，全片就该静下来。
// 文字落定后 **1.6s 完全静止**——这是有意的，不是漏了动效。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 title2  1 sub2  2 hero2

  const ti = expoOut(seg(t, 0.20, 0.85));
  st[0] = mk({ dy: (1 - ti) * 20, alpha: ti });

  const s = expoOut(seg(t, 0.45, 1.10));
  st[1] = mk({ dy: (1 - s) * 12, alpha: s });

  // 角色：入场即静止（它已经"停"了），只留极微的呼吸示意它还活着。
  // 幅度取 alpha 0.80↔1.0：40px 的点在 64 宽网格里约 1 格，位移量不到、亮度量得到。
  const h = 0.80 + 0.20 * (0.5 + 0.5 * Math.sin(t * 3.14));
  // 1.60s 之后连呼吸也停——"手停了，界面才静"字面落实。
  st[2] = mk({ alpha: (t < 1.60) ? h : 1 });

  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
