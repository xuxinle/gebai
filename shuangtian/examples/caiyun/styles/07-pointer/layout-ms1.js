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

// 幕 1：角色滑到面板前，**划过一行，那一行才亮** —— 逐行的因果，不是整体亮起。
//
// ## 本模板的语法延续
// 幕 0 是"落=按下"，这一幕是"划过=依次点亮"。
// 时序必须严格逐行：角色先到这一行的位置，该行才亮起（晚 0.06~0.10s）。
// 三行不能同时亮——同时亮就等于"界面自己亮了"，本模板立刻散。
//
// ## 承接关键
//   幕 0 的 to   = { x:"27.08%", y:"33.33%" }  → 角色从左上进入
//   幕 1 的 from = { x:"27.08%", y:"57.41%" }  → **注意 y 不同**
// 这两个数不一致：观众会看到"角色在空中跳了一下"。修正方式不是改 JSON，
// 而是在幕 1 开头让角色从承接位置**平滑滑到**第一行（0.15s 内），
// 这样观感上是"它飞过来落在第一行"，而不是突变。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow1  1 title1  2 panel
  // 3 row0  4 rowLit0 | 5 row1  6 rowLit1 | 7 row2  8 rowLit2 | 9 hero1

  const e = expoOut(seg(t, 0.10, 0.58));
  st[0] = mk({ dy: (1 - e) * 12, alpha: e });

  const ti = expoOut(seg(t, 0.22, 0.82));
  st[1] = mk({ dy: (1 - ti) * 18, alpha: ti });

  // 面板整体淡入（它是背景容器，不是"界面反应"，允许自行出现）
  const pn = expoOut(seg(t, 0.35, 0.95));
  st[2] = mk({ alpha: pn, scale: 0.985 + 0.015 * pn });

  // —— 角色：从承接位置 (520,360) 出发，逐行下移 ——
  // 三行的 y 中心：360+50=410? 不，行矩形的 y 是顶边，角色停在行左侧、垂直居中：
  //   row0 y=360 h=100 → 角色中心 y ≈ 410 → 角色顶边 y ≈ 390
  //   row1 y=490 → 顶边 520；row2 y=620 → 顶边 650
  // 起始由 carry 交付在 (520,360)，先滑到 row0 的 (540,390)，再逐行下移。
  const toRow0 = expoOut(seg(t, 0.15, 0.55));
  const toRow1 = expoOut(seg(t, 1.20, 1.60));
  const toRow2 = expoOut(seg(t, 2.10, 2.50));

  // 基准 = carry 交付位置 (520, 360)；目标逐级累加
  let dx = 20 * toRow0;                       // x: 520 → 540
  let dy = 30 * toRow0                        // y: 360 → 390
         + 130 * toRow1                       // → 520
         + 130 * toRow2;                      // → 650
  // 落定后轻微悬浮（同幕 0 的手法，幅度小）
  const settled = (t > 2.55);
  const bob = settled ? Math.sin((t - 2.55) * 3.93) * 2.5 : 0;

  // —— 逐行点亮：必须晚于角色到达该行 0.08s ——
  const lit0 = expoOut(seg(t, 0.63, 0.92));   // 角色 0.55 到 row0 → 0.63 亮
  const lit1 = expoOut(seg(t, 1.68, 1.97));   // 角色 1.60 到 row1 → 1.68 亮
  const lit2 = expoOut(seg(t, 2.58, 2.87));   // 角色 2.50 到 row2 → 2.58 亮

  // 亮起后缓慢呼吸（行条 920×100，占 64 宽网格约 30 格，量得到）
  const br = (t0) => 0.80 + 0.20 * (0.5 + 0.5 * Math.sin((t - t0) * 3.14));
  st[4] = mk({ alpha: lit0 * ((t > 0.92) ? br(0.92) : 1) });
  st[6] = mk({ alpha: lit1 * ((t > 1.97) ? br(1.97) : 1) });
  st[8] = mk({ alpha: lit2 * ((t > 2.87) ? br(2.87) : 1) });

  // 未点亮的行：极淡地存在（是"待处理"，不是"没有"）
  st[3] = mk({ alpha: 0.92 });
  st[5] = mk({ alpha: 0.92 });
  st[7] = mk({ alpha: 0.92 });

  // 幕末角色必须回到 carry_out.from：{ x:"48.96%", y:"64.81%" } → (940, 700)
  // 此刻它在 (540,650)。所以 2.70s 起把它送回 (940,700)，赶在 3.40s 边界前到位。
  const back = expoInOut(seg(t, 2.70, 3.25));
  dx += (400 - dx) * back;     // 540 → 940
  dy += (50 - dy) * back;      // 650 → 700

  st[9] = mk({ dx, dy, alpha: t > 0.05 ? 1 : 0 });

  return { states: st, shapes: [] };
}
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
