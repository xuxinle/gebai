// 幕 1：角色逐行下移，**划到哪行、哪行才亮**（本模板的唯一语法）
//
// ## 因果顺序是写死在时间轴上的，不是"配得好"
// 每一行的点亮时刻都从**同一个** `ARRIVE` 数组派生（+0.08s 滞后）。
// 这样即使之后调整角色速度，因果顺序也不会被破坏——
// 如果两边各写一个时间，改一个忘一个，就会出现"界面自己亮了"（模板立刻散）。
//
//   0.30       角色由 carry 送到第一行（这是幕 0 的 to，不是我画的）
//   0.63       第一行亮（= 0.55 + 0.08）
//   1.05→1.39  角色移到第二行 / 1.13 第二行亮
//   1.55→1.89  角色移到第三行 / 1.63 第三行亮
//   1.89–3.10  角色停住，三行呼吸（等待 carry 接管）
//
// ## 承接关键（两处都必须吻合，否则 carry_qa 判 bare）
// · 角色**起点** = 幕 0 的 to = (580, 380) = row0 的角色位（行内垂直居中：350+30）
// · 角色**末位** = 本幕 from = (580, 640) = row2 的角色位（610+30）
// 所以第三行到位后**不能再移动**——后面的"活着"只能靠 alpha，不能靠位移。
//
// ## 为何点亮层用 dw 展开而不是整体 alpha
// `dw` 从左向右长出来，读作"被划过"；整体 alpha 渐亮读作"它自己亮了"。
// 一字之差就是本模板成不成立的分界。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow1  1 title1  2 panel
  // 3 row0  4 rowLit0 | 5 row1  6 rowLit1 | 7 row2  8 rowLit2
  // 9 shadow1  10 hero1

  const e = expoOut(seg(t, 0.10, 0.55));
  st[0] = mk({ dy: (1 - e) * 12, alpha: e });

  const ti = expoOut(seg(t, 0.22, 0.80));
  st[1] = mk({ dy: (1 - ti) * 18, alpha: ti });

  // 面板：玻璃板抬起入场（它是背景容器，不是"界面反应"，允许自行出现）
  const pk = expoOut(seg(t, 0.28, 0.90));
  st[2] = mk({ dy: (1 - pk) * 16, alpha: pk });

  // —— 角色走过的三行：所有时序都从这一个数组派生 ——
  const ARRIVE = [0.55, 1.05, 1.55];
  const LAG = 0.08;                         // 响应滞后：界面不得早于动作
  const TRAVEL = 0.34;                      // 行间移动耗时

  // 角色 y：row0 位（380）→ row1（510）→ row2（640），每段 130px
  let heroY = 0;
  for (let i = 1; i < 3; i++) {
    heroY += 130 * expoInOut(seg(t, ARRIVE[i], ARRIVE[i] + TRAVEL));
  }

  // 角色落定后的微呼吸：只改 alpha。
  // 角色 40px 在 64 宽网格里约 1.3 格，位移量（1~3px）会被块均值抹平；
  // 而 alpha 改的是这一格的亮度，量得到。
  const settled = (t > 1.95);
  const breath = 0.80 + 0.20 * (0.5 + 0.5 * Math.sin((t - 1.95) * 3.93));

  st[10] = mk({
    dy: heroY,
    alpha: (t > 0.28 ? 1 : 0) * (settled ? breath : 1),
  });
  st[9] = mk({
    dy: heroY + 14,
    alpha: (t > 0.28 ? 0.50 : 0) * (settled ? (0.34 + 0.16 * breath) : 1),
  });

  // —— 三行：到达 → 0.08s 后从左向右划亮 ——
  for (let i = 0; i < 3; i++) {
    const litStart = ARRIVE[i] + LAG;
    const wipe = expoInOut(seg(t, litStart, litStart + 0.26));
    // 行容器：入场略早于点亮（它是"待处理"的存在，不是反应）
    const rowIn = expoOut(seg(t, 0.32 + i * 0.05, 0.72 + i * 0.05));
    st[3 + i * 2] = mk({ dy: (1 - rowIn) * 10, alpha: rowIn * 0.95 });
    // 点亮层：dw 从 0 长到满（向左收 920），alpha 同步跟上
    const hold = (t > litStart + 0.26)
      ? (0.78 + 0.22 * (0.5 + 0.5 * Math.sin((t - litStart) * 2.24 + i * 0.7)))
      : 1;
    st[4 + i * 2] = mk({
      dw: -920 * (1 - wipe),
      alpha: (wipe > 0 ? 1 : 0) * hold,
    });
  }

  return { states: st, shapes: [] };
}
