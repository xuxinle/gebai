// 幕 0：角色从左滑入 → 落到按钮上 → 按钮才亮 → 抬起停住
//
// ## 本模板的唯一语法：角色即指针
// **每一个**界面反应都必须能追溯到角色身体的一个动作。不能出现「界面自己亮了」。
// 所以这一幕的时序被写死成四拍，**顺序不可换**：
//
//   0.55–1.25  角色从画面左侧滑入（这是「动作」，界面上还什么都没发生）
//   1.25–1.42  角色下压（落到按钮上）—— 压的**同时**按钮亮起（±0.08s 内，成对）
//   1.42–1.75  按钮回弹（体积守恒：dw +6%、dh −6%，与角色抬起同步）
//   1.75–2.10  角色抬起，停在按钮正上方（这是 carry_out.from 的位置）
//   2.10–3.00  角色轻微悬浮（呼吸），按钮保持点亮后缓慢回落
//
// ## 承接关键
// 幕末角色矩形必须与 carry_out.from 完全吻合：
//   from = { x:48.96% y:62.04% w:2.08% h:3.70% radius:20 }
// 即 hero0 在 (940, 670) 40x40 —— 所以抬起到 670 后**不能再动位置**，
// 否则 carry 起来时会出现"先跳一下再飞"（实测观感很怪）。
//
// ## 为什么用 alpha 而不只靠位移做"活着"
// 角色只有 40px，在 64 宽验证网格里占不到 2 格，1–2px 的悬浮会被平均掉。
// 落定后让 alpha 在 0.72↔1.0 之间呼吸（周期 1.6s），才量得到。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow  1 title  2 btn0  3 btnLit0  4 shadow0  5 hero0

  const e = expoOut(seg(t, 0.10, 0.58));
  st[0] = mk({ dy: (1 - e) * 14, alpha: e });

  const ti = expoOut(seg(t, 0.24, 0.86));
  st[1] = mk({ dy: (1 - ti) * 20, alpha: ti });

  // —— 角色（先动）——
  // 从画面外 (-60) 滑到按钮中心上方 (940, 670)
  const slide = expoOut(seg(t, 0.55, 1.25));
  const press = expoOut(seg(t, 1.25, 1.42));      // 下压 42px（700 → 742，嵌进按钮）
  const lift = expoOut(seg(t, 1.42, 1.80));       // 抬起回到 670
  const heroX = -60 + 1000 * slide;
  // 悬浮（落定后）：只在 y 上 ±3px + alpha 呼吸
  const settled = (t > 1.85);
  const bob = settled ? Math.sin((t - 1.85) * 3.93) * 3 : 0;
  const heroAlpha = (t > 0.55 ? 1 : 0) * (settled ? (0.86 + 0.14 * (0.5 + 0.5 * Math.sin((t - 1.85) * 3.93))) : 1);
  st[5] = mk({
    dx: heroX + 60,
    dy: -30 + (42 * press) * (1 - lift) + bob,
    alpha: heroAlpha,
  });

  // 影子跟随（角色抬起时影子变小、变淡；下压时变大、变深）
  st[4] = mk({
    dx: heroX + 60,
    dy: -30 + (42 * press) * (1 - lift) + 22,
    alpha: (t > 0.55 ? 0.55 : 0) * (settled ? (0.42 + 0.16 * (0.5 + 0.5 * Math.sin((t - 1.85) * 3.93))) : 0.55),
    scale: 1 - 0.18 * lift,
  });

  // —— 按钮响应（必须晚于动作 0.08s：press 在 1.25 开始，亮起在 1.33）——
  const lit = expoOut(seg(t, 1.33, 1.52));
  // 按下时按钮被压低（体积守恒：dh 缩、dw 鼓），与角色下压同步
  const squash = 1 - Math.abs(seg(t, 1.33, 1.75) * 2 - 1);
  st[3] = mk({
    alpha: lit * (settled ? (0.85 + 0.15 * (0.5 + 0.5 * Math.sin((t - 1.85) * 3.93))) : 1),
    dh: -6 * squash,
    dw: 8 * squash,
    dy: 3 * squash,
  });
  st[2] = mk({ dh: -6 * squash, dw: 8 * squash, dy: 3 * squash });

  return { states: st, shapes: [] };
}
