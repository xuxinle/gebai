// 一帧里有 12 个图标，用循环错峰入场 + 波浪起伏 —— JSON 要写 12 遍，这里一个 for。
//
// 这就是「编排」相对「字段」的差别：意图（错峰、波浪）直接写在代码里，
// 而不是靠几十个字段的组合去逼近。
function layout(t, duration, canvas, layers) {
  const out = layers.slice();
  // layers[0] 是桌面块，保持不动
  for (let i = 1; i < out.length; i++) {
    const delay = (i - 1) * 0.12;          // 错峰：每个比前一个晚 0.12s
    const local = Math.max(0, t - delay);
    // 入场：从下方升上来（ease-out），带一点点过冲
    const p = Math.min(1, local / 0.7);
    const ease = 1 - Math.pow(1 - p, 3);
    const overshoot = Math.sin(Math.PI * p) * 0.04 * (1 - p);

    // 起伏：入场后按各自相位做正弦浮动
    const wave = Math.sin(t * 2.2 + i * 0.9) * 26 * Math.min(1, local / 0.9);

    out[i] = {
      dx: 0,
      dy: (1 - ease - overshoot) * 220 + wave,
      scale: 0.82 + 0.18 * ease,
      alpha: 0.15 + 0.85 * ease,
    };
  }
  return out;
}
