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

// 幕 0：因果段——光标先到位，面板才生成。
//
// ## 本模板的唯一语法
// 01-capability-list 区别于"纯 UI 录像"的唯一理由就是**手**：
// 每一次界面反应都必须看得见手。所以时序只能是
//   **光标动作 → 界面响应**
// 反过来（面板先出现、光标才移过去）就退化成 PPT 转场。
// 实测口径：响应不得早于动作开始，这里让面板严格在「点击挤压结束」之后才长出来。
//
// ## 承接关键（两层含义）
// 1. 幕末光标的矩形必须与 `carry_out.from` 完全重合：
//      from = { x:"44.79%", y:"52.69%", w:"1.35%", h:"2.41%", radius:13 }
//      = 860.0 × 569.1，26 × 26 —— 正是本幕 `cursor0` 的落点。
//    重合的意义：carry 开始推进的那 0.3s 里，两个矩形叠在一起，观众看不出接缝。
// 2. 光标必须在 2.70s **淡出**（0.25s 内到 0）。
//    不淡出的话，carry 一边变形、`cursor0` 一边原样留在原地，画面里会有两个光标。
//
// ## 为什么面板与文字都要"从光标处生长"
// 遮蔽不可用（LayerDelta 改不了 fill），所以"生成"只能靠 **scale + alpha**：
// 面板从光标位置以 0.94 倍弹开，读作"从这里长出来的"，而不是"凭空出现"。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow0  1 title0  2 panel0  3 row0a  4 row0b  5 cursor0

  const e = expoOut(seg(t, 0.10, 0.62));
  st[0] = mk({ dy: (1 - e) * 14, alpha: e });

  const ti = expoOut(seg(t, 0.22, 0.92));
  st[1] = mk({ dy: (1 - ti) * 20, alpha: ti });

  // —— 光标：0.30–0.90 从画面下方移入落点 ——
  // 起点 (860, 900) → 落点 (860, 569)，纵向 331px（远大于 44px 的位移判据）
  const mv = expoInOut(seg(t, 0.30, 0.90));
  const cy = 900 + (569 - 900) * mv;
  // 点击挤压：0.90–1.00 按下（dw 鼓 dx 缩），1.00–1.12 回弹
  const press = (t >= 0.90 && t < 1.12)
      ? Math.sin((t - 0.90) / 0.22 * Math.PI) * 5.5
      : 0;
  // 2.70s 起淡出，让位给 carry 绘制的承接物
  const fadeAway = 1 - expoOut(seg(t, 2.70, 2.95));
  st[5] = mk({
    dy: cy - 569,
    dw: press,
    dh: -press,
    alpha: (t > 0.30 ? 1 : 0) * fadeAway,
  });

  // —— 面板：点击之后才长出来（1.02 起，晚于动作 0.12s）——
  const pk = expoOut(seg(t, 1.02, 1.42));
  st[2] = mk({
    scale: 0.94 + 0.06 * pk,
    alpha: pk,
  });

  // —— 两行内容：面板出现后才逐行落定 ——
  const r1 = expoOut(seg(t, 1.30, 1.72));
  st[3] = mk({ dy: (1 - r1) * 10, alpha: r1 });
  const r2 = expoOut(seg(t, 1.48, 1.90));
  st[4] = mk({ dy: (1 - r2) * 10, alpha: r2 });

  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
