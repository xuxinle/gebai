// 打字机：用一个 alpha 脚本让两段文字**互斥**出现。
//
// ## 为什么不能靠 `enter`
//
// DSL 的 `enter` 只有入场（fade/rise/scale），**没有出场**。所以
// "先显示 hint，再换成打了一半的字，再换成打完的字"这个最基本的打字机
// 用三个 `text` + 三次 `enter` 是**做不到**的——三者会同时存在，
// 文字叠成一团（实测截图：'Describe a film' 压在 'Dayline' 上）。
//
// 修法：三个图层都常驻，用脚本按时间把**非当前段的 alpha 压到 0**。
// 这不是取巧：`LayoutDelta.alpha` 本就是"乘在基准透明度上"，
// 把它当开关用是完全正当的用法。

function layout(t, duration, canvas, layers) {
  const out = layers.map(() => ({}));

  // 场景 1 的图层顺序（见 make-launch.js）：
  //   0 = prompt bar
  //   1 = typed-1  "/onetake"
  //   2 = typed-2  完整句子
  //   3 = send 按钮
  const T_1 = 0.75;   // typed-1 出现
  const T_2 = 1.70;   // typed-2 取代 typed-1
  const T_SEND = 2.60;

  // 硬切（不做淡入淡出）：打字机的视觉语言就是"字符出现"，
  // 交叉淡化会显得像叠影。
  const on = (start) => (t >= start ? 1 : 0);

  if (layers.length > 1) out[1] = { alpha: on(T_1) * (t < T_2 ? 1 : 0) };
  if (layers.length > 2) out[2] = { alpha: on(T_2) };
  // 发送按钮随最后一段一起出现
  if (layers.length > 3) out[3] = { alpha: on(T_SEND) };

  return out;
}
