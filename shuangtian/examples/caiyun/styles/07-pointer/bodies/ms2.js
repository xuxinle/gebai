// 幕 2：手停了，界面才静。角色由 carry 送到标题下方（(700,660)），从此不再动。
//
// ## 本模板的收束语法
// "手停了，界面才静"不是一句文案，而是这一幕的**时间结构**：
//   · 角色一来就不再移动（它由 carry 交付在原地，本脚本连 dy 都不给）
//   · 角色自身的"活着"只保留 1.6s，之后连呼吸也停
//   · 文字 1.10s 落定，之后到幕末 1.9s **完全静止**
// 这与前两幕形成对照：前两幕一直在动（角色在跑），这一幕停给你看。
//
// ## 为何允许本幕全静止
// 关键元素落定后留 ≥1s 静止是硬要求（节奏偏好单向：只会嫌快）。
// 三幕里只有这一幕允许全静止——若每一幕都这样，整片就读成卡住了。
//
// ## 承接
// 角色位置与尺寸由 carry 交付，**布局里画的那个 hero2 只作为落点参考**
// （它在 (700,660) 40×40，与 carry 的 to 完全一致，所以即便同时可见也看不出双影；
//  实际上 carry 结束后本层的 alpha 才升上来，见下）。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 title2  1 sub2  2 shadow2  3 hero2

  const ti = expoOut(seg(t, 0.20, 0.85));
  st[0] = mk({ dy: (1 - ti) * 20, alpha: ti });

  const s = expoOut(seg(t, 0.45, 1.10));
  st[1] = mk({ dy: (1 - s) * 12, alpha: s });

  // 角色：carry 在 6.1+0.7=6.8s 结束，本幕从 6.4s 开始 →
  // 6.4~6.8 期间 carry 还在画，所以本地角色层**先不出现**（alpha 0），
  // 6.8 之后才由本层接管。这就是"同位置交接"，看不到接缝。
  // 注意 t 是幕内时间：6.8s 全片 = 6.4s 本幕起点 + 0.4s。
  const handoff = (t > 0.42) ? 1 : 0;
  // 角色"活着"到 1.60s，之后连呼吸也停 —— 字面落实"手停了，界面才静"。
  const breath = 0.82 + 0.18 * (0.5 + 0.5 * Math.sin((t - 0.42) * 3.93));
  const stillAlive = (t > 0.42 && t < 1.60);
  st[3] = mk({ alpha: handoff * (stillAlive ? breath : (t >= 1.60 ? 1 : 0)) });

  st[2] = mk({ alpha: handoff * (stillAlive ? (0.34 + 0.16 * breath) : (t >= 1.60 ? 0.5 : 0)) });

  return { states: st, shapes: [] };
}
