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

// 幕 1：中线已加宽成"带"（由 carry 接住），带里长出对比说明，两态仍在两侧。
//
// ## 承接关键（本幕两处都要对上）
// 进来时：carry 把幕 0 的 12×460 中线展开成 240×600 的带 ——
//   带的实际图层是 `band`（x840 y300 w240 h600），与上一幕 carry_out.to 一致。
//   布局里**不能再画那条中线**，否则和 carry 的带双影。
// 出去时：带的矩形必须与 `carry_out.from` 一致：
//   from = { x:"43.75%", y:"27.78%", w:"12.5%", h:"55.56%", radius:8 }
//   → x = 0.4375 × 1920 = 840，y = 0.2778 × 1080 = 300，w = 240，h = 600
//   所以 band 在这幕**不许改变 x/y/w/h**（只在内部做明暗与文字）。
//   需要"带呼吸"时只能改 alpha / 内部元素——改尺寸就会让 from 失配。
//
// ## 两态仍然同框
// 这是并置模板不可破的一条：本幕左右两侧的 A1/B1 与两列格子**全部在场**。
// 中线的宽度变化只是"比较的轴变粗了"，不是把两态挤走。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow1  1 title1
  // 2 band  3 bt0  4 bt1  5 bt2
  // 6 A1  7 Ah1   8 B1  9 Bh1
  // 10..16  Ld0..Ld6     17..23  Rd0..Rd6

  const e = expoOut(seg(t, 0.10, 0.58));
  st[0] = mk({ dy: (1 - e) * 12, alpha: e });

  const ti = expoOut(seg(t, 0.22, 0.82));
  st[1] = mk({ dy: (1 - ti) * 18, alpha: ti });

  // 带：出现即有（由 carry 交付），只做**内部明暗呼吸**（不改尺寸）。
  // 幅度 0.86→1.0、周期 2.0s：带面积 240×600 = 144000px²，
  // 占全屏 6.9% —— 这一层的变化本身就能被量到。
  const bandBreath = 0.86 + 0.14 * (0.5 + 0.5 * Math.sin((t - 0.9) * 3.14));
  st[2] = mk({ alpha: (t > 0.9) ? bandBreath : 0.86 });

  // 带内三行字：逐行亮起（0.52 / 0.78 / 1.04），落定后跟着带一起明暗。
  // 这是"带里长出对比说明"的落地。
  for (let i = 0; i < 3; i++) {
    const t0 = 0.52 + i * 0.26;
    const k = expoOut(seg(t, t0, t0 + 0.40));
    const dim = (t > 1.56) ? (0.62 + 0.38 * bandBreath) : 1;
    st[3 + i] = mk({ alpha: k * dim, dy: (1 - k) * 12 });
  }

  // 两态容器：两侧淡入（比带来得晚一点，让"带先成立"）
  const ca = expoOut(seg(t, 0.34, 0.92));
  st[6] = mk({ dy: (1 - ca) * 20, alpha: ca });
  st[8] = mk({ dy: (1 - ca) * 20, alpha: ca });

  const ha = expoOut(seg(t, 0.52, 1.06));
  st[7] = mk({ dy: (1 - ha) * 10, alpha: ha });
  st[9] = mk({ dy: (1 - ha) * 10, alpha: ha });

  // 两列格子：本幕改为"反向波"——左列相位正向、右列相位反向，
  // 两波在中线处"对撞"。这是并置语义在动效上的表达：
  // 观众看到两股节奏朝彼此推进，比较一直成立。
  // 幅度 0.40→1.0 比幕 0 更大（这一幕要更明确地"在动"）。
  for (let i = 0; i < 7; i++) {
    const t0 = 0.44 + i * 0.045;
    const up = expoOut(seg(t, t0, t0 + 0.30));
    const waveL = 0.40 + 0.60 * (0.5 + 0.5 * Math.sin((t - 1.10) * 3.49 - i * 0.70));
    const waveR = 0.40 + 0.60 * (0.5 + 0.5 * Math.sin((t - 1.10) * 3.49 + i * 0.70));
    st[10 + i] = mk({ alpha: up * ((t > 1.10) ? waveL : 1), dy: (1 - up) * 14 });
    st[17 + i] = mk({ alpha: up * ((t > 1.10) ? waveR : 1), dy: (1 - up) * 14 });
  }

  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
