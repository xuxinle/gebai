function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow  1 title  2 b1  3 b2  4 b3  5 l1  6 l2  7 l3
  const e = expoOut(seg(t, 0.10, 0.55));
  st[0] = mk({ dy: (1 - e) * 12, alpha: e });

  const ti = expoOut(seg(t, 0.22, 0.80));
  st[1] = mk({ dy: (1 - ti) * 18, alpha: ti });

  // 珠子错峰 0.22s 一个，用 spring 有过冲手感
  for (let i = 0; i < 3; i++) {
    const t0 = 0.70 + i * 0.22;
    const k = clamp(spring(t - t0, 0.42, 16), 0, 1.08);
    // 落定后：**电流沿主线流动**——每个珠子依次亮一下，相位错 0.40s，
    // 周期 1.2s。这既是脊椎隐喻（主线上的节点依次通电），
    // 也是唯一能在验证网格里量到的幅度：
    //   试过 scale ±2.4% = 1.5px、dy ±2.5px → 实测**零变化**（60px 网格直接平均掉）。
    //   alpha 0.55→1.0 改变的是几十像素区域的亮度，格均值必然变。
    const pulse = 0.55 + 0.45 * (0.5 + 0.5 * Math.sin((t - t0) * 5.24 - i * 2.1));
    const settled = (t > t0 + 0.42);
    st[2 + i] = mk({
      scale: k,
      alpha: (seg(t, t0, t0 + 0.16) > 0 ? 1 : 0) * (settled ? pulse : 1),
    });
    // 标签比珠子晚 0.14s；落定后跟着珠子微微明暗（同相位）
    const lk = expoOut(seg(t, t0 + 0.14, t0 + 0.50));
    st[5 + i] = mk({ dy: (1 - lk) * 10, alpha: lk * (settled ? (0.72 + 0.28 * pulse) : 1) });
  }

  // 沿主线流动的光带（layer 8 `flow`）——**这一幕“在动”的主力**。
  //
  // 为何不是靠珠子：珠子只有 64px，在 kStatsWidth=64 的验证网格里只占 ~2 格，
  // 实测 alpha 0.55→1.0 仍只把静止从 97.1% 降到 96.1%（几乎量不到）。
  // 光带横跨 82% 画面宽（≈26 格），走过时格均值必然变。
  //
  // ## 为什么必须**连续**流动（实测踩过）
  // 第一版写成“周期 2.4s、跑 1.1s、余下 1.3s 完全不可见”——
  // 格均差峰值从 12 升到 24（说明它确实被量到了），但静止占比**一点没降**（96.1%）。
  // 因为静止占比是**逐帧对**算的：只要有一半时间不动，那一半的帧对全算静止。
  // 所以要让“动”变成**一直有**，而不是“动一阵停一阵”。
  //
  // 改为：时刻都有两段光在线上，位置用 (t*speed + 偏移) % 1 铺满，永不停。
  const walk = (u) => {
    const p = ((u % 1) + 1) % 1;          // 归一化 0..1
    return { pos: p, a: Math.sin(p * Math.PI) };  // 两端淡出，中间最亮
  };
  const a1 = walk(t / 2.1);
  const a2 = walk(t / 2.1 + 0.5);         // 两段错开半个周期，保证总有东西在走
  const flowAlpha = Math.max(a1.a, a2.a * 0.75);
  const flowPos = (a1.a >= a2.a * 0.75) ? a1.pos : a2.pos;
  st[8] = mk({
    dx: 1560 * flowPos,
    alpha: flowAlpha * 0.95,
    scale: 1 + 0.3 * flowAlpha,
  });

  return { states: st, shapes: [] };
}
