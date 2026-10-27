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
