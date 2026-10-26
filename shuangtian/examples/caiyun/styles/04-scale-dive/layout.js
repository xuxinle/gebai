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

// 04 · 尺度一镜 —— 一次连续运动穿越所有尺度，中间不剪断
//
// ## 承接方式：同一个主体（船锚式跟随）
// 活下来的永远是"那一行日志"。它同时在三个尺度上存在，只是哪个在焦。
//
// ## 本镜的做法（裁云没有真三维相机）
// 裁云的相机是**逐场景**的（cam_zoom_from/to + 锚点），一个场景只能一段推镜。
// 所以"穿越三层"用**图层的 scale + 位移**自己算：每一层按同一个 zoom 参数缩放，
// 远的层缩得快（离开画面），近的层缩得慢（成为主体）。这样在同一个场景里
// 就能读出"我们在往一层里钻"。
//
// ## 节拍（相机从不剪断）
//   0.0–1.2   三层都在，L3 在焦（大标题）
//   1.2–3.0   钻进第 2 层：L3 放大离开，L2box 成为画面主体
//   3.0–4.6   再钻进第 1 层：L2 放大离开，L1 那行日志充满
//   4.6–6.0   落定 + 静止（rest）
//
// ## 一条硬约束（实测）
// 三层**同时在场**，不要做"淡出上一层、淡入下一层"——那不是穿越，是换镜。
// 淡出只发生在某层已经大到出画之后。
function scene_body(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  const p1 = expoInOut(seg(t, 1.20, 3.00));
  const p2 = expoInOut(seg(t, 3.00, 4.60));
  const z = p1 + p2;

  // ## 三层的"可读性"必须错开（踩过）
  //
  // 第一版让三层都半透明同框 → 三组文字叠在画面中心糊成一团，
  // 读不出任何一层（实测）。三层同框是**物理事实**，不是**视觉重点**：
  //   · 当前层：alpha 1.0，清晰可读
  //   · 外一层：alpha ≤ 0.16，只作为"边框感"存在，不可读
  //   · 更外：0
  //
  // 标题：固定层，在最外层清晰，随推进退场
  const tk = expoOut(seg(t, 0.00, 0.50));
  st[0] = mk({ alpha: tk * Math.max(0, 1 - z * 1.4), dx: (1 - tk) * -24 });

  const G = 2.8;

  // 第 3 层（整体）：z=0 清晰，z>0.55 迅速退成不可读的底纹，z>0.95 出画
  const s3 = Math.pow(G, z);
  let a3;
  if (z < 0.45) a3 = 1;
  else if (z < 0.95) a3 = 0.16 * (1 - (z - 0.45) / 0.50) / 0.16 * 0.16 + 0.0;  // → 0
  else a3 = 0;
  // 简化：0.45 之前 1，之后线性降到 0（0.45→0.95）
  a3 = (z < 0.45) ? 1 : Math.max(0, 1 - (z - 0.45) / 0.50);
  st[1] = mk({ scale: s3, alpha: a3 });
  st[2] = mk({ scale: s3, alpha: a3 });

  // 第 2 层：z=0 只是底纹(0.14)，z=1 清晰(1.0)，z>1.5 退场
  const s2 = Math.pow(G, z - 1);
  let a2;
  if (z < 0.55) a2 = 0.14;                                  // 远处底纹，不可读
  else if (z < 1.05) a2 = 0.14 + 0.86 * ((z - 0.55) / 0.50); // 成为当前层
  else if (z < 1.55) a2 = 1;
  else a2 = Math.max(0, 1 - (z - 1.55) / 0.40);
  st[3] = mk({ scale: s2, alpha: a2 * 0.55 });   // 容器更淡，它是框不是内容
  st[4] = mk({ scale: s2, alpha: a2 });
  st[5] = mk({ scale: s2, alpha: a2 * 0.9 });

  // 第 1 层（最终主体）：z<0.9 不可见（它当时"还不存在"），z=2 满屏
  const s1 = Math.pow(G, z - 2) * (1 + 0.05 * expoOut(seg(t, 4.6, 5.6)));
  let a1;
  if (z < 0.85) a1 = 0;
  else if (z < 1.55) a1 = 0.20 * ((z - 0.85) / 0.70);       // 先出现为底纹
  else if (z < 2.00) a1 = 0.20 + 0.80 * ((z - 1.55) / 0.45);
  else a1 = 1;
  st[6] = mk({ scale: s1, alpha: a1 * 0.7 });
  st[7] = mk({ scale: s1, alpha: a1 });
  st[8] = mk({ scale: s1, alpha: a1 });

  // 光标：落定后才出现
  const ck = expoOut(seg(t, 4.70, 5.10));
  st[9] = mk({ dw: -240 * (1 - ck), alpha: ck });

  return { states: st, shapes: [] };
}

function layout(t, duration, canvas, layers) {
  return scene_body(t, duration, canvas, layers);
}
