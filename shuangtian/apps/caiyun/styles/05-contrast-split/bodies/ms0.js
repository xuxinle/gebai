// 幕 0：竖中线 + 左右两态同框（并置的第一步）
//
// ## 本模板的全部：两态永远同框
// 05-contrast-split 的语法是 **并置（compare）**——观众的眼在两态之间自动比较。
// 一旦做成「先看 A 再切到 B」，比较就断了，整片失去意义。
// 所以这一幕两态**同时**出现，中线始终在中间，谁都不许离场。
//
// ## 承接关键
// 幕末中线的矩形必须与 `carry_out.from` 完全吻合：
//   from = { x:"49.69%", y:"37.04%", w:"0.625%", h:"42.59%", radius:6 }
// 即 mid（x954 y400 w12 h460）在幕末**回到原位、原尺寸**。
// 中线在这幕可以做"宽度呼吸"，但必须在 2.62s 前收回 12px——
// 否则观众看到的是"一条粗线消失、一条窄线出现"，承接分直接掉。
//
// ## 为什么"动"要落在两列格子上（而不是只做中线脉冲）
// 实测（02-single-point 那一轮）：48px 的小元素做 alpha 呼吸，
// 在 64 宽验证网格里只占不到 2 格，几乎量不到。
// 这里每列 **7 个 44×62 的格子、横跨 464px**（≈15 格），
// 逐个错峰亮起→暗下，格均值必然变——这才是能量到的「动」。
// 而且它符合模板语义：两态各自的格子节奏不同，正是"两态"的可见证据。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow0  1 title0
  // 2 A0  3 Ah0   4 B0  5 Bh0
  // 6 mid
  // 7..13  Lc0..Lc6     14..20  Rc0..Rc6

  const e = expoOut(seg(t, 0.10, 0.62));
  st[0] = mk({ dy: (1 - e) * 14, alpha: e });

  const ti = expoOut(seg(t, 0.24, 0.86));
  st[1] = mk({ dy: (1 - ti) * 20, alpha: ti });

  // 两态容器：同时上浮入场（并置：不许错开到"先后"）
  const ca = expoOut(seg(t, 0.44, 1.02));
  st[2] = mk({ dy: (1 - ca) * 24, alpha: ca });
  st[4] = mk({ dy: (1 - ca) * 24, alpha: ca });

  const ha = expoOut(seg(t, 0.62, 1.16));
  st[3] = mk({ dy: (1 - ha) * 12, alpha: ha });
  st[5] = mk({ dy: (1 - ha) * 12, alpha: ha });

  // 中线：先"长高"（h 0→460，双向），再进入宽度呼吸。
  // 呼吸只改 dw/dx（保持中心不动），幅度 4px、周期 1.4s——细幅度是刻意的：
  // 这条线是"比较的轴"，它不该抢戏；主要运动交给两列格子。
  const grow = expoInOut(seg(t, 0.56, 1.30));
  const breathe = Math.sin((t - 1.30) * 4.49) * 2.0;
  const back = expoInOut(seg(t, 2.62, 2.95));   // 幕末收回原宽 12px
  const w = 12 + (breathe * (1 - back) * grow);
  st[6] = mk({
    dh: -460 * (1 - grow),
    dy: 230 * (1 - grow),
    dw: w - 12,
    dx: -(w - 12) / 2,
    alpha: grow > 0 ? 1 : 0,
  });

  // 左列（A 态）：慢、沉——每格错峰 0.05s，只做幅度 0.45→1.0 的明暗，
  // 相位沿列递增，读作"一波在左态里慢慢推过去"。
  for (let i = 0; i < 7; i++) {
    const t0 = 0.78 + i * 0.05;
    const up = expoOut(seg(t, t0, t0 + 0.34));
    const wave = 0.45 + 0.55 * (0.5 + 0.5 * Math.sin((t - 1.30) * 2.99 - i * 0.62));
    const on = up * ((t > 1.30) ? wave : 1);
    st[7 + i] = mk({ alpha: on, dy: (1 - up) * 16 });
  }

  // 右列（B 态）：快、亮——错峰更密（0.045s）、亮度更高、相位反向。
  // 两列的**节奏差**就是"两种做法"的可见证据，比任何文字都直接。
  for (let i = 0; i < 7; i++) {
    const t0 = 0.82 + i * 0.045;
    const up = expoOut(seg(t, t0, t0 + 0.30));
    const wave = 0.55 + 0.45 * (0.5 + 0.5 * Math.sin((t - 1.30) * 4.19 + i * 0.78));
    const on = up * ((t > 1.30) ? wave : 1);
    st[14 + i] = mk({ alpha: on, dy: (1 - up) * 16 });
  }

  return { states: st, shapes: [] };
}
