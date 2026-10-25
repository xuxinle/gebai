// 幕 5「扩张」6.0s —— 336 点阵从微距拉到全景（尺度揭示）。
// 讲一件事：**量级**。冲击来自"数量 + 尺度反差"，不来自颜色。
// 峰值幕：环境层用 level 2；末段中心方块承接进下一幕。
function scene_field(t, duration, canvas, layers) {
  const W = 1920, H = 1080;
  const sh = [];
  const CX = W * 0.5, CY = H * 0.5;

  // 尺度：0.10 → 1.0（expoOut：起步快、末端收住）
  const sp = EASE.expoOut(seg(t, 0.15, 3.30));
  const scale = 0.10 + (1.0 - 0.10) * sp;

  envBackdrop(sh, W, H, t, 2, CX, CY);

  const COLS = 24, ROWS = 14;
  const GX = 78, GY = 62;
  const totalW = (COLS - 1) * GX;

  // 波前：从左向右扫过整个点阵（亮带逐点经过）
  const waveX = -400 + (totalW + 800) * clamp01((t - 0.2) / 3.6);

  const COLD = '#152A4E', WARM = '#7FB2FF', HOT = '#EAF4FF';
  for (let gy = 0; gy < ROWS; gy++) {
    for (let gx = 0; gx < COLS; gx++) {
      const i = gy * COLS + gx;
      const bx = CX + ((gx - (COLS - 1) * 0.5) * GX) * scale;
      const by = CY + ((gy - (ROWS - 1) * 0.5) * GY) * scale;
      const size = (16 + hash01(i, 11) * 14) * scale;

      // 整块缓慢旋转（同一块内相对位置在变，跨格）
      const ang = t * 0.10;
      const dx = bx - CX, dy = by - CY;
      const rx = CX + dx * Math.cos(ang) - dy * Math.sin(ang) * 0.16;
      const ry = CY + dx * Math.sin(ang) * 0.16 + dy * Math.cos(ang);

      const d = Math.abs((gx * GX) - (waveX + 400));
      const band = clamp01(1 - d / 340);
      const base = 0.20 + hash01(i, 5) * 0.16;
      const rd = Math.hypot(rx - CX, ry - CY) / (760 * scale + 1);
      const core = clamp01(1 - rd);

      let col = mixHex(COLD, WARM, clamp01(base + band * 0.8 + core * 0.35));
      if (band > 0.55) col = mixHex(col, HOT, (band - 0.55) * 1.6);
      const alpha = clamp01(base + band * 0.62 + core * 0.10) * 0.95;

      pushRect(sh, rx - size * 0.5, ry - size * 0.5, size, size, col, alpha,
               Math.max(0, size * 0.18));
    }
  }

  // 数字标：给尺度一个可读的锚
  const numP = EASE.expoOut(seg(t, 1.15, 2.35));
  if (numP > 0.01) {
    const ny = CY + 250 * scale + 44;
    pushText(sh, '336', CX - textW('336', 74) * 0.5, ny, 74, '#EAF0FF', 0.9 * numP);
    pushTextC(sh, 'ELEMENT NODES', CX, ny + 86, 20, '#5E7A99', 0.85 * numP);
  }

  envStream(sh, W, H, t, '#7FB2FF', 52, 220, 0.24);
  // 本幕 336 个点阵有波前扫过 + 整块旋转 + 尺度揭示，**自身就是全片最活的镜头**。
  // 不再叠环境层（叠上去只会把点阵的强弱对比抹平）。

  return { states: layers.map(() => ({})), shapes: sh };
}
