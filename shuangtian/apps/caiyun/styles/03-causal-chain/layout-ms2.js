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

// 幕 2：火花（由 carry 送到 x=1700）停在结论旁边，减速落定
//
// ## 因果链的收束方式
// 前两幕是"撞"，这一幕是**余势**：火花已经没劲了，减速停住。
// 所以这里不再是 expo_out（快速冲出），而是 inOut（先快后慢、最后停）。
// 台阶感：用一个"刹车"曲线，最后 0.5s 几乎不动。
//
// ## 承接关键
// carry 已把它送到 (1700,540)。本幕 spark2 图层声明在同一位置，接管零跳变。
// 之后它只做很小的减速位移（60px），最后完全停住——
// 这一幕的"承接"由 carry 完成，图层自己不需要承担位移判据。
//
// ## 收尾静止
// 结论 1.30s 落定后本幕余下 1.7s 完全静止——这是**有意的**（关键元素落定后要呼吸）。
// 三幕里只有这一幕允许全静止（ms0/ms1 都靠节点呼吸保持"活着"）。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow2  1 title2  2 sub2  3 spark2

  const e = expoOut(seg(t, 0.18, 0.75));
  st[0] = mk({ dy: (1 - e) * 12, alpha: e });

  const ti = expoOut(seg(t, 0.32, 1.00));
  st[1] = mk({ dy: (1 - ti) * 20, alpha: ti });

  const s = expoOut(seg(t, 0.62, 1.30));
  st[2] = mk({ dy: (1 - s) * 14, alpha: s, scale: 1 + 0.04 * clamp(spring(t - 0.62, 0.40, 15), 0, 1) });

  // 火花：减速停下（60px），carry 在幕内 0.40s（=6.80s）结束 → 本图层当场接管
  //
  // ## 接管时刻必须紧贴 carry 结束（实测空窗 0.33s）
  // 第一版写 `t >= 0.70`，而 carry 绘制区间是 [6.10, 6.80]，
  // 即幕内 [−0.30, 0.40]。于是 **0.40~0.70 这 0.33s 承接物凭空消失**
  // （轨迹实测：6.80s 之后 spark2 首个可见帧是 7.13s）。
  // 所以接管点定在 0.40s，与 carry 落笔无缝衔接。
  const brake = expoInOut(seg(t, 0.40, 1.10));
  const takeover = (t >= 0.40) ? 1 : 0;
  st[3] = mk({
    dx: -60 * brake,
    alpha: takeover,
  });

  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
