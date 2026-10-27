// 幕 2：线已成面（由 carry 接住那个矩形），面里长出文字。
//
// ## 本模板最后的落点
// "点 → 线 → 面"三步全程只有一个元素在变形。这一幕面已经在了（carry 交付），
// 本脚本只负责让文字从面里出来——仍然不引入第二个图形元素。
//
// ## 收尾必须静止
// 关键元素落定后留 ≥1s 静止（节奏偏好是单向的：只会嫌快）。
// 所以文字 1.10s 落定后，本幕余下 1.9s 完全不动——这是**有意的**。
// 实测提醒：验证器的「静止占比」对这类收尾给高分是正常的（它本就不该动）；
// 但若整片每一幕都这样，就会读成卡住。三幕里**只有这一幕**允许全静止。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 title2  1 sub2

  const ti = expoOut(seg(t, 0.30, 0.95));
  st[0] = mk({ dy: (1 - ti) * 22, alpha: ti });

  const s = expoOut(seg(t, 0.55, 1.10));
  st[1] = mk({ dy: (1 - s) * 12, alpha: s });

  return { states: st, shapes: [] };
}
