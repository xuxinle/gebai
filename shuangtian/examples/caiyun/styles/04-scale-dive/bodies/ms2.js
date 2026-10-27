// 幕 2：整页缩成一枚胶囊。文字在它下方落定。
//
// ## 承接物必须自己画（实测踩过）
// `capsule` 这一层是为了让**胶囊常在**——carry 只在 6.1~6.8s 那段画它，
// 之后什么都不画。第一版没画它，第 275 帧（9.2s）整屏只剩两行字，
// 胶囊凭空消失，"它一直在"这句文案就变成了反讽。
// 它的值与 carry_out.to 完全一致：42%,46%,16%,8%,radius 999。
//
// ## 本模板的落点
// "小方块 → 整页 → 胶囊"三段都是**同一个矩形换尺寸**，全片没有第二个图形
// 元素参与承接。这是 scale-dive 的全部。
//
// ## 收尾允许全静止
// 关键元素落定后要留 ≥1s 静止（节奏偏好单向：只会嫌快）。
// 本幕文字 1.10s 落定后余下 1.9s 不动——这是有意的，
// 三幕里只有这一幕允许这样（幕 0/1 都有持续运动）。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 capsule  1 title2  2 sub2

  // 胶囊：同样需要**避双影**——carry_out 淡出（fade_out 0.35），本层淡入。
  // 时间：carry 的 at=6.1（成片时刻）跨边界到 6.8s，本幕 t=0 对应 6.4s，
  // 所以本层 t=0.05~0.45 淡入，与 carry 的淡出重叠。
  const cpIn = expoOut(seg(t, 0.05, 0.45));
  const cpBreath = 0.93 + 0.07 * (0.5 + 0.5 * Math.sin(t * 2.2));
  st[0] = mk({ alpha: cpIn * cpBreath });

  const ti = expoOut(seg(t, 0.30, 0.95));
  st[1] = mk({ dy: (1 - ti) * 22, alpha: ti });

  const s = expoOut(seg(t, 0.55, 1.10));
  st[2] = mk({ dy: (1 - s) * 12, alpha: s });

  return { states: st, shapes: [] };
}
