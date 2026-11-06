// 幕 2：硬切收束——光标停在角落，画面安静下来等它。
//
// ## 承接关键
// 幕 1 的 carry_out 把「光标·本体」送到：
//   to = { x:"71.98%", y:"75.93%", w:"1.35%", h:"2.41%", radius:13 }
// 这一幕不画光标（由 carry 绘制），只在它旁边落文字——
// 文字必须落在光标**周围**，否则观众会以为"光标没了、换了一页"。
//
// ## 收尾
// 本模板以"硬切"收束，所以这里**不需要**再补动效铺垫：
// 文字落定后保持静止即可。三幕里只有这一幕允许全静止。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 title2  1 sub2
  // 文字放在光标左上侧（光标在 72%/76%），从右侧滑入，形成"说完就走"的收束感。

  const ti = expoOut(seg(t, 0.20, 0.85));
  st[0] = mk({ dx: (1 - ti) * 26, alpha: ti });

  const s = expoOut(seg(t, 0.45, 1.05));
  st[1] = mk({ dx: (1 - s) * 18, alpha: s });

  return { states: st, shapes: [] };
}
