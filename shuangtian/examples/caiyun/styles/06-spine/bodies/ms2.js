function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 rule  1 title  2 sub
  // 横线向两侧展开：dw 双向 + dx 反向一半
  const r = expoInOut(seg(t, 0.10, 0.72));
  st[0] = mk({ dw: -600 * (1 - r), dx: 300 * (1 - r), alpha: r });

  const ti = expoOut(seg(t, 0.38, 0.98));
  st[1] = mk({ dy: (1 - ti) * 18, alpha: ti });

  const s = expoOut(seg(t, 0.58, 1.15));
  st[2] = mk({ dy: (1 - s) * 12, alpha: s });

  // 1.15s 之后完全静止（rest ≥ 1s：关键元素落定后必须呼吸）
  return { states: st, shapes: [] };
}
