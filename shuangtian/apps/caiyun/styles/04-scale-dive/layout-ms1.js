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

// 幕 1：那块已是整页。
//
// ## 本模板的全部
// zoomThrough：这一层的某个元素，就是下一层的整幅画面。
// 靠 carry_out 承担「160px 方块 → 整页框」的连续放大（宽 ×9.6），
// 本脚本负责「框里开始有东西」。
//
// ## 两个必须知道的 carry 行为（实测踩过）
//
// ① **carry 只画过渡那 0.7s**。`at`~`at+dur` 之外它不绘制任何东西，
//    所以目标场景必须**自己画一份最终状态的框**（这里是 `pageframe`，
//    值与 carry_out.to 完全一致：8%,14%,84%,66%）。不画的话，
//    过渡一结束框就消失，画面只剩背景与文字——实测第 150 帧（5.0s）就是这样。
//
// ② **幕首有 0.7s 的交接窗口**。carry 正在从 160px 放大到整页，
//    此时若本幕元素还没入场，会看到「空白 → 框 → 内容」的断裂。
//    所以文字与信息行从 0.15s 就开始（carry 交接完成于 0.7s），
//    让它们与框的放大**同时**发生，而不是等框到位后才开始。
//
// ## 为什么用 sweep 承担"一直在动"
// 四行信息是一次性入场的，落定后整幕会死。让一条 6px 亮线自上而下连续扫
// （周期 1.6s、宽 1480px ≈ 26 格），格均值必然变——这是本模板唯一能稳量到的运动。
// 注意：sweep 只是**视觉巡检**，它不承担承接（承接物是框）。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 pageframe  1 eyebrow1  2 title1  3 row0  4 row1  5 row2  6 row3  7 sweep  8 cap1

  // 框：**入场与 carry 交接**。
  //
  // ## 双框（实测踩过）
  // carry 在 0~0.7s 画一个从 160px 长到整页的框，而本层 `pageframe` 也是同一个框——
  // 两者同时可见时会出现错开 3px 的**双框**（第 96 帧实物就是这样）。
  // 解法：carry_out 加 `fade_out: 0.35`（它自己淡出），本框在**同窗口淡入**（alpha 0→1）。
  // 时间必须对齐：carry 的 at=2.7（成片时刻）跨边界到 3.4s，
  // 本幕 t=0 对应成片 3.0s，所以本框在 t=0.05~0.45 淡入，与 carry 的淡出重叠。
  const fpIn = expoOut(seg(t, 0.05, 0.45));
  // 之后极轻微的 alpha 呼吸（0.94→1.0），让它不是死图
  const fpBreath = 0.94 + 0.06 * (0.5 + 0.5 * Math.sin(t * 2.6));
  st[0] = mk({ alpha: fpIn * fpBreath });

  // 文字：从 0.15s 起就动，与 carry 的放大**同时**发生（消掉交接期的空窗）
  const e = expoOut(seg(t, 0.15, 0.62));
  st[1] = mk({ dy: (1 - e) * 12, alpha: e });

  const ti = expoOut(seg(t, 0.25, 0.78));
  st[2] = mk({ dy: (1 - ti) * 18, alpha: ti });

  // 四行信息：错峰从左侧长出（dw 由负到 0），比淡入更像"加载出来"
  const rowW = [1480, 1290, 1370, 1120];
  for (let i = 0; i < 4; i++) {
    const t0 = 0.55 + i * 0.15;
    const g = expoOut(seg(t, t0, t0 + 0.42));
    st[3 + i] = mk({ dw: -rowW[i] * 0.35 * (1 - g), alpha: g });
  }

  // sweep：自上而下连续扫，永不归零 → 整幕一直在动
  const ph = ((t - 0.90) / 1.7) % 1;
  const p = ph < 0 ? ph + 1 : ph;
  st[7] = mk({
    dy: 292 * p,
    alpha: (t > 0.90) ? (0.30 + 0.70 * Math.sin(p * Math.PI)) : 0,
  });

  const ck = expoOut(seg(t, 1.30, 1.80));
  st[8] = mk({ dy: (1 - ck) * 10, alpha: ck });

  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
