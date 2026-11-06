function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow  1 title  2 spine  3 bead
  const e = expoOut(seg(t, 0.10, 0.62));
  st[0] = mk({ dy: (1 - e) * 14, alpha: e });

  const ti = expoOut(seg(t, 0.28, 0.95));
  st[1] = mk({ dy: (1 - ti) * 22, alpha: ti });

  // 主线：从 0 宽长到满（长出来，不是淡入——竖向/横向"长"读作生长）
  const g = expoInOut(seg(t, 0.95, 1.95));
  st[2] = mk({ dw: -1320 * (1 - g), alpha: g > 0 ? 1 : 0 });

  // 珠子：沿线上轨滑到右端（位移 ≥ 对角线 2% ≈ 44px，满足承接判据）
  const b = expoInOut(seg(t, 1.55, 2.75));
  st[3] = mk({ dx: 1320 * b, alpha: b > 0 ? 1 : 0, scale: 1 - 0.12 * Math.sin(b * Math.PI) });

  return { states: st, shapes: [] };
}
