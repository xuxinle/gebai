// 日历 UI 生成器（照 onetake launch 的 Dayline 周视图重建）
//
// ## 为什么要用脚本而不是手写 JSON
//
// 周视图是 **7 列 × 多行的事件块**（20+ 个块，各有颜色/位置/文字）。
// 手写 JSON 要几百行且改一个块要动三处（块 + 文字 + 行）；脚本里它是
// 一个 `for` 循环 + 一份数据表——**加一个事件只加一行数据**。
//
// 这正是"脚本编排层"该做的事：结构化的重复内容。
//
// ## 坐标口径
//
// 全部用**设计像素**（1920×1080 画布）。Delta 里 `dx/dy` 是增量，
// `dw/dh` 是尺寸增量且会被夹到 >=0，所以"从无到有长开"用 `scale`。

function layout(t, duration, canvas, layers) {
  const out = layers.map(() => ({}));

  // ── 时间轴（秒）──
  const T_GRID = 0.15;    // 网格线出现
  const T_EVENT = 0.55;   // 事件块逐个长出来
  const T_POP = 0.42;     // "Launch day" 块被抬起（2.6s 那一下）

  // 事件块数据：[列(0-6), 起始小时, 持续小时, 颜色, 标题, 副文]
  // 列宽 = 1920 * 0.78 / 7，行高 = 每半小时 44px
  const EVENTS = [
    [0, 9.0, 1.5, "#2E6B5E", "Standup * plan", "9:00—10:30"],
    [1, 10.0, 2.0, "#D95B43", "Design review", "10:00—12:00"],
    [2, 9.0, 2.0, "#7B8B3A", "Deep work", "9:00—11:00"],
    [3, 10.0, 3.0, "#D95B43", "Launch day", "10:00—13:00"],
    [4, 9.0, 3.0, "#7B8B3A", "Climb", "10:00—12:00"],
    [5, 11.0, 1.0, "#5B8BA8", "Retro", "11:00—12:00"],
    [6, 11.0, 1.0, "#D9A03A", "Call mom", "11:00—12:00"],
    [0, 13.0, 2.0, "#7B8B3A", "Deep work", "13:00—15:00"],
    [2, 13.0, 1.5, "#C97B7B", "Studio shoot", "14:00—15:30"],
    [1, 15.0, 1.0, "#D9A03A", "1:1 Maya", "15:00—16:00"],
    [3, 15.0, 2.0, "#E8944A", "Press calls", "15:00—17:00"],
    [4, 15.0, 1.0, "#8B8560", "Drinks", "15:00—16:00"],
  ];

  // layers 的顺序约定（由 JSON 保证）：
  //   [0] 面板底板  [1..] 事件块（每个块 2 层：rect + 文字）
  // 这里只对"事件块 rect"（奇数下标 3,5,7...）做错峰入场。
  const eventStart = 3;
  for (let i = 0; i < EVENTS.length; ++i) {
    const rectIndex = eventStart + i * 2;
    if (rectIndex >= layers.length) break;
    // 错峰：每个块晚 0.055s 起跳（20 个块铺开约 1.1s，不显得齐步走）
    const k = spring(t - (T_EVENT + i * 0.055), 0.62, 21);
    out[rectIndex] = {
      scale: Math.max(0.001, 0.86 + 0.14 * k),
      alpha: k <= 0 ? 0 : Math.min(1, k * 2.2),
    };
  }

  // ── "Launch day" 块被抬起：整块上移 + 微放大（吸引注意）──
  {
    const k = spring(t - T_POP, 0.55, 18);
    const lift = t > T_POP ? Math.max(0, 1 - k) * 0 + Math.min(1, k) : 0;
    const rectIndex = eventStart + 3 * 2;   // 第 4 个事件 = Launch day
    if (rectIndex < layers.length && t > T_POP) {
      out[rectIndex] = Object.assign(out[rectIndex] || {}, {
        dy: -8 * lift,
        scale: (0.86 + 0.14 * 1.0) * (1 + 0.06 * lift),
      });
    }
  }

  return out;
}
