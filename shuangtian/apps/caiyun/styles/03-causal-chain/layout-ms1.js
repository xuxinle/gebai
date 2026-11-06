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

// 幕 1：火花（由 carry 送到 x=1100）继续向右飞，砸在第二环 node1 上，node1 亮起
//
// ## 因果感的时序（本模板的全部）
//   0.00–0.55  标题入场（火花还在 carry 手里，本幕 spark 图层**不可见**）
//   0.70       carry 结束 → spark 图层接管，接着自己飞
//   1.55       撞上 node1
//   1.67       node1 亮（**晚于撞击，不得早于**）
//   2.10+      落定，火花停在 node1 左侧
//
// ## 承接关键
// carry 已把火花送到 (1100,460)。本幕 spark 图层声明的位置就是 (1100,460)，
// 且 `from` 也写的是它 → 接管瞬间**零跳变**。
// 之后它自己 dx 前进 190px 到 (1290,460) 附近停下（撞上节点左缘）。
//
// 末帧必须回到 from 处吗？**不用**——carry_qa 取边界窗口内「最早 vs 最晚」，
// 只要有 ≥2% 对角线的位移就算 carried。本幕火花从 1100 走到 1290（190px = 9.5% 对角），
// 远超 2% 判据。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow1  1 title1  2 node1  3 lit1  4 cap1  5 rail  6 spark

  const e = expoOut(seg(t, 0.10, 0.60));
  st[0] = mk({ dy: (1 - e) * 12, alpha: e });

  const ti = expoOut(seg(t, 0.22, 0.88));
  st[1] = mk({ dy: (1 - ti) * 18, alpha: ti });

  // 节点：**一开场就在场**（不是 0.45s 才入场）
  //
  // ## 为何必须开场即在（实测抽帧发现）
  // 第一版写 `seg(t, 0.45, 1.05)`，结果转场后 0.17s 那一帧：
  // 火花在飞、标题只有 15% 透明、**节地不在**——观众看到的是一个方块
  // 在空屏上飘。这不是“带入场感的出现”，而是“画面还没搭好”。
  // 因果感需要“目标先存在、然后被撞”，目标不在现场就没有“撞”可言。
  const nk = expoOut(seg(t, 0.00, 0.22));
  st[2] = mk({ dy: (1 - nk) * 6, alpha: nk });

  // 撞击 1.55s —— **之前 lit1 严格为 0**
  const hit = clamp(spring(t - 1.55, 0.42, 16), 0, 1);
  // 撞击后节点持续呼吸（本幕剩下的时间都在动，避免 96% 静止）
  const breath = 0.52 + 0.48 * (0.5 + 0.5 * Math.sin((t - 2.00) * 3.49));
  const settled = (t > 2.00);
  st[3] = mk({ alpha: hit * (settled ? breath : 1) });

  const ck = expoOut(seg(t, 1.72, 2.20));
  st[4] = mk({ dy: (1 - ck) * 10, alpha: ck * (settled ? (0.65 + 0.35 * breath) : 1) });

  // 轨道线：撞击后从左向右长出来（表示"动量沿这条线过去"）
  const railK = expoInOut(seg(t, 0.75, 1.50));
  st[5] = mk({ dw: -240 + 240 * railK, alpha: railK });

  // 火花：carry 在 0.70s 结束 → 之后由本图层接管，接着飞到 node1 左缘
  //   1100 → 1290 = 190px
  const fly = expoInOut(seg(t, 0.70, 1.50));
  // carry 期间（<0.70s）本图层必须不可见，否则双影
  const takeover = (t >= 0.70) ? 1 : 0;
  // ## 幕末必须让位给下一段 carry（实测踩到双影）
  //
  // `carry_out` 的契约（s04 独立实测确认）：**它只在 `at`~`at+dur` 那段时间绘制**。
  // 本幕的 carry 是 `at=6.1 dur=0.7` → 绘制区间 [6.10, 6.80]，
  // 即**幕内 3.10s 起** carry 会自己画一个火花从 (1100,592) 飞到 (1240,560)。
  // 而本图层当时仍 alpha=1 → 实测有 9 帧双影。
  // 所以 3.00s 开始淡出，3.10s 归零（正好在 carry 落笔前让开）。
  const handoff = 1 - seg(t, 3.00, 3.10);
  // 落下后停住，只留极轻的悬浮（1px 级位移不会被误判成"承接"，但让画面活着）
  const idle = Math.sin((t - 1.55) * 3.2) * 1.5;
  st[6] = mk({
    dx: 190 * fly,
    dy: (t > 1.55 ? idle : 0),
    alpha: takeover * handoff,
  });

  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
