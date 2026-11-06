// 幕 1：点已成线（由 carry 接住），线在这里**开始长出内容**——
//       但仍然只有这一个元素，不能"再出现一个新东西"。
//
// ## 本模板的硬约束
// 02-single-point 的全部就是 morph：**一旦有一处是替换而非变形，整片就散了**。
// 所以这一幕不能加卡片、不能加图标——只能在线上做文章：
// 线从中间"鼓起"（dh 长大）变成一条粗带，这是纯 morph 的延续。
//
// ## 承接关键
// 幕末线的矩形必须与 carry_out.from 吻合：
//   from = { x:"34.4%", y:"64.8%", w:"31.3%", h:"0.56%", radius:3 }
// 即 bar（x660 y700 w600 h6）在推进结束后要回到这个位置与尺寸。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow  1 title  2 bar  3 cap1

  const e = expoOut(seg(t, 0.12, 0.62));
  st[0] = mk({ dy: (1 - e) * 12, alpha: e });

  const ti = expoOut(seg(t, 0.26, 0.88));
  st[1] = mk({ dy: (1 - ti) * 18, alpha: ti });

  // 线：先保持（由 carry 接住），然后**鼓起**成带 —— dh 从 6 长到 44，
  // 同时 dy 上移一半保持中心不动（这是"变形"而非"移动"的关键）。
  //
  // ## 必须在幕末收回（否则承接断掉）
  // `carry_out.from` 写的是 h:"0.56%"（即 6px），而鼓起后是 44px。
  // 若带着 44px 进入边界，carry 的起始矩形与画面实景不符 →
  // 观众看到"一条粗带忽然消失、一个盒子出现"，承接分掉下来。
  // 所以 2.55s 开始收回，3.10s 前回到 6px——正好赶上 3.4s 的边界。
  const back = expoInOut(seg(t, 2.55, 3.10));
  const bulgeBase = 38 * expoInOut(seg(t, 1.10, 2.00));
  const bulge = bulgeBase * (1 - back);
  // 鼓起之后持续"流动"：alpha 沿带扫过（周期 1.8s）——
  // 让这条线在等待下一幕时仍是活的，且不引入新元素。
  const flow = 0.70 + 0.30 * (0.5 + 0.5 * Math.sin((t - 2.00) * 3.49));
  const settled = (t > 2.00);
  st[2] = mk({
    dh: -6 + bulge,
    dy: -(bulge / 2),
    alpha: settled ? flow : 1,
  });

  const ck = expoOut(seg(t, 0.90, 1.45));
  st[3] = mk({ dy: (1 - ck) * 10, alpha: ck * (settled ? (0.6 + 0.4 * flow) : 1) });

  return { states: st, shapes: [] };
}
