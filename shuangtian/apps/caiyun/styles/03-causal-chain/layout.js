// 裁云脚本公共前言 —— 宿主只提供这些名字（写错就整份脚本静默降级）
//
// 可用：ease(name,t) spring(tau[,zeta,omega]) spring_vel settle
//       lerp(a,b,k) clamp(v,lo,hi) snap(t,fps) noise(seed)
// 不可用：seg / E.* / D() —— 它们不在宿主里，用了就 ReferenceError →
//       裁云降级为纯 JSON → 画面停在初始帧且**零报错**。
//
// LayerDelta 字段：dx dy dw dh scale rotate alpha tint
function seg(t, t0, t1) {
  if (t1 === t0) return (t >= t1) ? 1 : 0;
  const k = (t - t0) / (t1 - t0);
  return k < 0 ? 0 : (k > 1 ? 1 : k);
}
function expoOut(u) { return (u >= 1) ? 1 : 1 - Math.pow(2, -9 * u); }
function expoIn(u) { return u * u * u; }
function expoInOut(u) {
  if (u <= 0) return 0;
  if (u >= 1) return 1;
  return (u < 0.5) ? expoIn(u * 2) / 2 : 1 - expoIn((1 - u) * 2) / 2;
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

// 03 · 因果链 —— 每个画面由上一次**撞**出来
//
// ## 承接方式：contact（接触）
// 活下来的不是某个物体，是**动量**。
//
// ## 最硬的一条
// **撞击发生之前，被撞的东西必须严格为零**（不可见 / 完全静止 / 无预备动作）。
// 一旦给「提前预备」的动作，因果感立刻消失——观众读成"它自己动的"。
//
// ## 节拍（每次撞击 0.9s 一轮）
//   0.0–0.7   标题 + 四个空节点入场
//   0.7–1.6   撞击 1：n0 亮 → e0 长出来 → n1 被撞亮（挤压成对）
//   1.6–2.5   撞击 2
//   2.5–3.4   撞击 3
//   3.4–4.0   结论落定
//   4.0–5.4   静止（rest）
//
// ## 挤压必须成对（体积守恒）
// 撞上去时 dh 缩、dw 同时鼓；只压不鼓会读成"图被拉变形了"。
// 撞击时刻：间隔 0.62s（改前 0.90s）。
//
// 量测证据（改前）：0.5s 档里出现 2.0s 与 4.0~4.5s 的 **零变化**空白，
// 全片静止 47%。真因不是"镜头太长"，而是每次撞击的动效只占 0.4s、
// 之后空等 0.5s —— 链条读起来是"一下一下戳"，不是"一路推下去"。
const T_HIT = [0.62, 1.24, 1.86, 2.48];

function scene_body(t, duration, canvas, layers) {
  const st = new Array(layers.length);

  // 图层下标（json 顺序）：
  //  0 title  1 sub
  //  2 lit0  3 n0 | 4 lit1  5 n1 | 6 lit2  7 n2 | 8 lit3  9 n3
  // 10..13 四个标签  14..16 三条连接线  17 火花  18 结论
  //
  // ## 为什么每个节点要两层（lit + n）
  // LayerDelta **改不了 fill**（只有 dx/dy/dw/dh/scale/rotate/alpha/tint）。
  // 所以"节点通电发亮"只能做成：底下垫一层亮色块，用 alpha 点亮它。
  // 直接给 n 改 scale 是不够的——暗底放大还是暗的（实测整帧看起来"什么都没发生"）。

  // 标题 / 副标
  const tp = expoOut(seg(t, 0.00, 0.52));
  st[0] = mk({ dx: (1 - tp) * -30, alpha: tp });
  const sp = expoOut(seg(t, 0.08, 0.60));
  st[1] = mk({ dx: (1 - sp) * -22, alpha: sp * 0.9 });

  // 四个节点（空壳 + 亮层）
  for (let i = 0; i < 4; i++) {
    const on = expoOut(seg(t, 0.16 + i * 0.06, 0.62 + i * 0.06));
    // 被撞亮的时刻：n0 由 0.70 点燃，n1..n3 由各自的撞击点燃
    const hit = clamp(spring(t - T_HIT[i], 0.44, 17), 0, 1);
    // 挤压（成对，体积守恒）：撞击后 0.22s 内 dh 缩、dw 鼓
    let squeeze = 0;
    if (t >= T_HIT[i]) {
      squeeze = 1 - Math.abs(seg(t, T_HIT[i], T_HIT[i] + 0.22) * 2 - 1);
    }
    // 空壳：始终在场，被撞时挤压（在下层）
    // 被撞之后加一个**缓慢呼吸**（±1.2% 尺度，周期 1.6s）——
    // 它让"已经通电的节点"在等待下一次撞击时不至于完全静止，
    // 也让整条链读作"电流在流"，而不是"四个独立开关"。
    const breath = (hit > 0.5) ? Math.sin((t - T_HIT[i]) * Math.PI * 2 / 1.6) * 0.012 : 0;
    st[2 + i * 2] = mk({
      alpha: on,
      dw: 12 * squeeze,
      dh: -12 * squeeze,
      scale: 1 + breath,
    });
    // 亮面：被撞之后点亮（在上层，内缩 3px）—— "通电"的唯一可见信号。
    //
    // 踩过：亮面放在空壳**下面**时完全看不见（被暗底盖住），
    // 整帧看起来"什么都没发生"，而脚本逻辑全对、零报错。
    st[3 + i * 2] = mk({
      alpha: on * hit * 0.95,
      scale: 1 + 0.02 * squeeze,
    });
  }

  // 四个标签：跟节点同步点亮（文字不能改色，只能提亮 alpha 与微抬）
  for (let i = 0; i < 4; i++) {
    const on = expoOut(seg(t, 0.20 + i * 0.06, 0.66 + i * 0.06));
    const hit = clamp(spring(t - T_HIT[i], 0.46, 16), 0, 1);
    st[10 + i] = mk({ alpha: on * (0.5 + 0.5 * hit), dy: -3 * hit });
  }

  // 三条连接线：撞击后 0.10s 开始长（固定 0.24s 长满到 160px）
  for (let i = 0; i < 3; i++) {
    const t0 = T_HIT[i] + 0.10;
    const k = expoInOut(seg(t, t0, t0 + 0.24));
    st[14 + i] = mk({ dw: k * 160, alpha: k });
  }

  // 火花：每次撞击后 0.06s 亮起，0.30s 内放大并淡出（一个脉冲）
  let sparkA = 0, sparkX = 0;
  for (let i = 0; i < 3; i++) {
    const t0 = T_HIT[i] + 0.06;
    const k = seg(t, t0, t0 + 0.46);
    const a = (k > 0 && k < 1) ? (1 - k) : 0;
    if (a > sparkA) { sparkA = a; sparkX = 600 + i * 360; }
  }
  st[17] = mk({
    alpha: sparkA,
    dx: sparkX - 603,
    scale: 0.6 + 1.8 * (1 - sparkA),
  });

  // 结论：三次撞击都完成之后才落定
  const vk = expoOut(seg(t, 3.00, 3.56));
  st[18] = mk({ alpha: vk, dy: (1 - vk) * 16 });

  return { states: st, shapes: [] };
}

function layout(t, duration, canvas, layers) {
  return scene_body(t, duration, canvas, layers);
}
