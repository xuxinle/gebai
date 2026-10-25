// 幕 1「收编」4.6s —— 210 个字块向中心塌缩，末段浮出标题。
// 一镜只讲一件事：**收拢**。
function scene_converge(t, duration, canvas, layers) {
  const W = 1920, H = 1080;
  const CX = W * 0.5, CY = H * 0.47;
  const sh = [];

  // 标题出现时把环降下去（0.45）：直接在背景层降环强度，而不是盖一个黑底
  const ringFade = 1 - 0.55 * EASE.expoOut(seg(t, 2.75, 3.5));
  envBackdrop(sh, W, H, t, 1, CX, CY, ringFade);
  envGrid(sh, W, H, t, 96, '#1B2740', 0.26, 12);

  // 塌缩：0.12s 起、2.45s 到 1
  const p = EASE.expoInOut(seg(t, 0.12, 2.45));
  const fieldOut = 1 - EASE.cubicIn(seg(t, 2.60, 3.40));

  const COLS = 21, ROWS = 10;
  for (let gy = 0; gy < ROWS; gy++) {
    for (let gx = 0; gx < COLS; gx++) {
      const i = gy * COLS + gx;
      const bx = -60 + gx * (W + 120) / (COLS - 1);
      const by = -40 + gy * (H + 80) / (ROWS - 1);

      const dx0 = bx - CX, dy0 = by - CY;
      const dist = Math.hypot(dx0, dy0);
      // 波前：远处先动，近处后动（分层推进，不是齐步走）
      const wave = clamp01((p - 0.28 * (1 - dist / 1200)) / 0.72);
      const k = EASE.expoIn(clamp01(wave));

      const ang = Math.atan2(dy0, dx0);
      const rot = k * 0.55;
      const nx = Math.cos(ang + rot) * dist * (1 - k * 0.985);
      const ny = Math.sin(ang + rot) * dist * (1 - k * 0.985);
      const x = CX + nx, y = CY + ny;

      const size0 = 44 + (hash01(i, 3) * 26);
      const size = size0 * (1 - k * 0.72);
      const near = clamp01(1 - dist / 1150);
      const lit = (0.18 + near * 0.30 + k * 0.42) * fieldOut;
      const col = mixHex('#1E3A6E', '#7FB2FF', clamp01(k * 0.85 + near * 0.35));
      pushRect(sh, x - size * 0.5, y - size * 0.5, size, size, col, clamp01(lit * 0.9),
               Math.max(0, 4 * (1 - k)), k * 24);
    }
  }

  // 中心核：先聚能，再交给标题。
  // 中心那一点落在标题位置，必须**先消退**（实测：它白团压在"收编"两个字上）
  const core = EASE.expoOut(seg(t, 1.55, 2.85)) * (1 - EASE.cubicOut(seg(t, 2.95, 3.45)));
  if (core > 0.01) {
    for (let i = 5; i >= 1; i--) {
      pushCircle(sh, CX, CY, (18 + i * 34) * (0.35 + core * 1.05) * (1 - 0.12 * i),
                 '#6FA8FF', 0.05 * core * (6 - i));
    }
    pushCircle(sh, CX, CY, 16 * core + 6, '#EAF4FF', 0.85 * core);
  }

  // 标题（末段）——一镜只讲一件事
  const tIn = 3.05;
  pushTypeIn(sh, '旧世界的一切技术', CX, CY - 112, 62, 6, '#EAF0FF', t, tIn, 0.045, 30);
  pushTypeIn(sh, '都是可以收编的躯体', CX, CY + 2, 62, 6, '#9EC4FF', t, tIn + 0.34, 0.045, 30);
  const ruleP = EASE.expoOut(seg(t, tIn + 0.95, tIn + 1.60));
  if (ruleP > 0.01) pushRect(sh, CX - 210 * ruleP, CY + 96, 420 * ruleP, 2, '#3B82F6', 0.75);

  envStream(sh, W, H, t, '#6FA8FF', 40, 170, 0.26);
  // 覆盖层必须在**所有内容之后**（呼吸作用于整帧，才与验收格差 1:1）
  envOverlay(sh, W, H, t, 1);

  return { states: layers.map(() => ({})), shapes: sh };
}
