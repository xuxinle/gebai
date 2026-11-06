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

// 幕 0：一小块（160×160）居中偏右下，等待被放大。
//
// ## 承接关键
// 幕末这一块的矩形必须与 `carry_out.from` 完全吻合：
//   from = { x:"45.833%", y:"53.704%", w:"8.333%", h:"14.815%", radius:12 }
// 即 frame 的静态值（x880 y580 w160 h160, 半径 12）**不能有任何 dx/dw**。
// 所以本幕只让它 scale 呼吸 + alpha 脉冲，绝不移动它——
// 位置一漂，carry 的起点就与实景不符，观众看到"一块消失、一页出现"。
//
// ## 入场要从 0.0s 开始（实测踩过）
// 第一版让方块 0.80s 才弹入，结果第 18 帧（0.6s）整屏只有两行字、方块不存在——
// 开变 0.8 秒是空白。改为 0.08s 就起弹：文字入场（0.15/0.30）与它**重叠**，
// 观感上是“标题与方块一起出来”，而不是“先出标题、等一块”。
//
// ## core 是视网膜
// 里面的 core（80×80，x920 y620）必须跟着 frame 一起缩放，否则两者会错位。
// 因为 carry 只能带一个矩形过去，core 在幕末应当回到与 frame 相同的相对位置
// （这里让它同步弹入，相对关系不变）。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow  1 title  2 frame  3 core  4 cap0

  // frame：0.08s 就起弹，落定后**完全静止**（位置与尺寸都不动，只为承接）
  const k = clamp(spring(t - 0.08, 0.50, 15), 0, 1.08);
  st[2] = mk({ scale: k, alpha: t > 0.08 ? 1 : 0 });
  st[3] = mk({ scale: k, alpha: t > 0.08 ? 1 : 0 });

  const e = expoOut(seg(t, 0.15, 0.70));
  st[0] = mk({ dy: (1 - e) * 14, alpha: e });

  const ti = expoOut(seg(t, 0.30, 1.00));
  st[1] = mk({ dy: (1 - ti) * 20, alpha: ti });

  const ck = expoOut(seg(t, 1.10, 1.60));
  st[4] = mk({ dy: (1 - ck) * 10, alpha: ck });

  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
