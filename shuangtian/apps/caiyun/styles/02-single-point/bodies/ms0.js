// 幕 0：点从极小弹到正常，然后开始"呼吸"（为幕 2 的拉长做预备——同一颗点）
//
// ## 承接关键
// 这一幕的**最后一帧**，点的位置/尺寸必须与 `carry_out.from` 完全吻合：
//   from = { x:"48.75%", y:"59.3%", w:"2.5%", h:"4.44%", radius:24 }
// carry 会从这个矩形开始拉长成线——所以点在这幕**不能被移动**，
// 否则观众看到的是"一个点消失 + 一条线出现"（承接分直接 0）。
//
// ## 为什么点要带呼吸
// 单点身份模板的核心风险是"一帧画面里什么都没动"。实测：只弹一次的话，
// 落定后整幕零变化。呼吸幅度取 alpha 0.62→1.0（不是 scale）：
// 元素只有 48px，在 64 宽验证网格里占不到 2 格，缩放变化量不到 1.5px 会被平均掉；
// 而 alpha 变的是这 2 格的亮度，能被量到。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow  1 title  2 dot

  const e = expoOut(seg(t, 0.15, 0.70));
  st[0] = mk({ dy: (1 - e) * 14, alpha: e });

  const ti = expoOut(seg(t, 0.30, 1.00));
  st[1] = mk({ dy: (1 - ti) * 20, alpha: ti });

  // 点：从 0.2 倍弹到 1 倍（spring 有过冲）
  const k = clamp(spring(t - 0.85, 0.45, 15), 0, 1.12);
  // 落定后持续呼吸（周期 1.5s，幅度 0.62→1.0）
  const breath = 0.62 + 0.38 * (0.5 + 0.5 * Math.sin((t - 1.60) * 4.19));
  const settled = (t > 1.60);
  st[2] = mk({
    scale: k,
    alpha: (t > 0.85 ? 1 : 0) * (settled ? breath : 1),
  });

  return { states: st, shapes: [] };
}
