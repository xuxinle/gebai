// 裁云脚本公共前言 —— 宿主只提供这些名字（写错就整份脚本静默降级）
//
// 可用：ease(name,t) spring(tau[,zeta,omega]) spring_vel settle
//       lerp(a,b,k) clamp(v,lo,hi) snap(t,fps) noise(seed)
// 不可用：seg / E.* / D() —— 它们不在宿主里，用了就 ReferenceError →
//       裁云降级为纯 JSON → 画面停在初始帧且**零报错**。
//
// LayerDelta 字段：dx dy dw dh scale rotate alpha tint
function seg(t, t0, t1) {
  if (t1 === t0) return (t >= t1) ? 1 : 0;
  const k = (t - t0) / (t1 - t0);
  return k < 0 ? 0 : (k > 1 ? 1 : k);
}
function expoOut(u) { return (u >= 1) ? 1 : 1 - Math.pow(2, -9 * u); }
function expoIn(u) { return u * u * u; }
function expoInOut(u) {
  if (u <= 0) return 0;
  if (u >= 1) return 1;
  return (u < 0.5) ? expoIn(u * 2) / 2 : 1 - expoIn((1 - u) * 2) / 2;
}
function mk(o) {
  const a = (o.alpha === undefined) ? 1 : o.alpha;
  return {
    dx: o.dx || 0, dy: o.dy || 0,
    dw: o.dw || 0, dh: o.dh || 0,
    scale: (o.scale === undefined) ? 1 : o.scale,
    rotate: o.rotate || 0,
    alpha: a < 0 ? 0 : (a > 1 ? 1 : a),
  };
}

// 05 · 对照分裂 —— 两态并置，让观众**自己看出**差异
//
// ## 承接方式：比较
// 活下来的是"同一件事的两个版本"。两态**永远同框**，
// 一旦拆成"先看 A 再切到 B"，比较就断了。
//
// ## 本镜的差异做在图形里（不是标注）
// 左七根柱：每次从零开始 → 高度在**同一基线附近抖动**（每次重来，没有累积）
// 右七根柱：结论入文件 → 高度**单调递增**（第 n 次站在第 n-1 次上）
//
// 加箭头/文字说明去解释"区别是什么"会更差（实测：那会让观众读字而不看图）。
//
// ## 节拍
//   0.0–0.8   两边同时入场（同一套入场语汇，只是颜色不同）
//   0.8–4.0   七对柱子逐对升起（每对 0.42s，左右同时）
//             左边升到同一高度附近，右边每次比上次高
//   4.0–4.6   结论落定
//   4.6–5.6   静止（rest）
const N = 7;
const BASE_Y = 700;    // 基线
const H0 = 60;         // 初始高度（json 里都是 60）

function scene_body(t, duration, canvas, layers) {
  const st = new Array(layers.length);

  const tp = expoOut(seg(t, 0.00, 0.46));
  st[0] = mk({ dy: (1 - tp) * -18, alpha: tp });
  const sp = expoOut(seg(t, 0.08, 0.54));
  st[1] = mk({ dy: (1 - sp) * -14, alpha: sp * 0.9 });

  // 两个容器：同一语汇入场（让观众先把它们看成"一对"）
  const bk = expoOut(seg(t, 0.10, 0.70));
  st[2] = mk({ alpha: bk, dy: (1 - bk) * 20 });
  st[5] = mk({ alpha: bk, dy: (1 - bk) * 20 });

  // 两个标题
  const lh = expoOut(seg(t, 0.24, 0.80));
  st[3] = mk({ alpha: lh });
  st[6] = mk({ alpha: expoOut(seg(t, 0.30, 0.86)) });

  // 基线
  const lb = expoInOut(seg(t, 0.34, 0.90));
  st[4] = mk({ dw: lb * 580 - 580, alpha: lb });
  st[7] = mk({ dw: lb * 580 - 580, alpha: lb });

  // ── 七对柱子 ──
  // 左：无记忆 —— 高度在 90±25 抖动（用固定的、无随机的错位）
  // 右：有记忆 —— 高度单调递增 90 → 300
  const L_H = [96, 62, 104, 74, 112, 68, 98];
  const R_H = [92, 124, 158, 194, 232, 268, 304];
  for (let i = 0; i < N; i++) {
    const t0 = 0.80 + i * 0.42;
    const k = clamp(spring(t - t0, 0.50, 14), 0, 1);

    // 左柱：高度趋近 L_H[i]（从 60 长上去；低于 60 时就缩短）
    const lh2 = H0 + (L_H[i] - H0) * k;
    st[8 + i] = mk({
      dh: lh2 - H0,
      // 长度向上长：底边不动 → dy 要补负值
      dy: -(lh2 - H0),
      alpha: seg(t, 0.60 + i * 0.10, 0.86 + i * 0.10),
    });

    // 右柱：同一时刻、同样的弹簧，只是目标更高（这是"对照"的关键——
    // 两边用**同一条**动效曲线，差异只来自数值）
    const rh = H0 + (R_H[i] - H0) * k;
    st[15 + i] = mk({
      dh: rh - H0,
      dy: -(rh - H0),
      alpha: seg(t, 0.60 + i * 0.10, 0.86 + i * 0.10),
    });
  }

  // 结论
  const vk = expoOut(seg(t, 4.05, 4.60));
  st[22] = mk({ alpha: vk, dy: (1 - vk) * 14 });

  return { states: st, shapes: [] };
}

function layout(t, duration, canvas, layers) {
  return scene_body(t, duration, canvas, layers);
}
