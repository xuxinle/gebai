// 幕 7「字标」6.0s —— 120 个粒子汇聚 → 字标弹簧落定 → 静止呼吸。
// 讲一件事：**记住这个名字**。落定后留 ≥1s 静止（节奏偏好是单向的）。
function scene_wordmark(t, duration, canvas, layers) {
  const W = 1920, H = 1080;
  const CX = W * 0.5, CY = H * 0.46;
  const sh = [];

  // 收尾段环境档降到 0（呼吸更慢、环更疏）——读作"平静下来"；
  // 但落定后仍需持续微动：实测 level 0 单独存在时静止 78%，所以额外加一层缓慢的亮度流。
  envBackdrop(sh, W, H, t, 0, CX, CY);

  // 粒子汇聚（0.15 → 2.55s），到位后消失
  const converge = EASE.expoInOut(seg(t, 0.15, 2.55));
  const vanish = 1 - EASE.cubicOut(seg(t, 2.35, 2.95));
  for (let i = 0; i < 120; i++) {
    const ang = hash01(i, 21) * Math.PI * 2;
    const rad = 620 + hash01(i, 33) * 520;
    const sx = CX + Math.cos(ang) * rad;
    const sy = CY + Math.sin(ang) * rad * 0.62;
    const k = EASE.expoIn(clamp01((converge - hash01(i, 7) * 0.28) / 0.72));
    const x = lerp(sx, CX, k);
    const y = lerp(sy, CY, k);
    const size = (2.2 + hash01(i, 13) * 4.2) * (1 - k * 0.55);
    const col = mixHex('#2C4C86', '#EAF4FF', clamp01(k * 1.15));
    const a = (0.25 + hash01(i, 3) * 0.55) * (0.35 + k * 0.65) * vanish;
    if (a > 0.01) pushCircle(sh, x, y, size, col, clamp01(a));
  }

  // 汇聚核：先亮后散（把注意力交给字标）
  const coreA = EASE.expoOut(seg(t, 1.6, 2.5)) * (1 - EASE.cubicOut(seg(t, 2.45, 3.05)));
  if (coreA > 0.01) {
    for (let i = 4; i >= 1; i--) pushCircle(sh, CX, CY, 16 + i * 26, '#7FB2FF', 0.05 * coreA * (5 - i));
    pushCircle(sh, CX, CY, 14, '#EAF4FF', 0.8 * coreA);
  }

  // 字标：弹簧落定（ζ0.58/ω14 → 一次过冲后停住），逐字错峰
  const tIn = 2.35;
  const SIZE = 120, GAP = 24;
  const totalW = textW('歌白', SIZE) + GAP;
  let bx = CX - totalW * 0.5;
  const chars = ['歌', '白'];
  for (let i = 0; i < chars.length; i++) {
    const tau = t - (tIn + i * 0.13);
    if (tau > 0) {
      const s = spring(tau, 0.58, 14);
      const a = clamp01(tau / 0.30);
      const size = SIZE * (0.90 + 0.10 * s);
      const y = CY - size * 0.5 + (1 - s) * 26;
      pushText(sh, chars[i], bx + (1 - s) * 10, y, size, '#EAF0FF', a);
    }
    bx += SIZE + GAP;
  }

  // 拉丁副标 + 细线（落定时"锁住"构图）
  const subP = EASE.expoOut(seg(t, 2.95, 3.55));
  if (subP > 0.01) {
    pushType(sh, 'GEBAI', CX, CY + 92, 44, 16, '#9EC4FF', 0.92 * subP);
    const rw = 300 * EASE.expoOut(seg(t, 3.05, 3.75));
    pushRect(sh, CX - rw * 0.5, CY + 66, rw, 1.6, '#3B82F6', 0.7 * subP);
  }

  // 落点那句话
  const lineP = EASE.expoOut(seg(t, 3.50, 4.30));
  if (lineP > 0.01) pushTextC(sh, '智能无状态，责任在智体', CX, CY + 188, 30, '#8FA6C4', 0.95 * lineP);

  // 页脚
  const footP = EASE.expoOut(seg(t, 3.90, 4.60));
  if (footP > 0.01) pushText(sh, 'GEBAI AGENT · 能力演示', 150, 972, 19, '#4E6A8C', 0.85 * footP);

  // 落定后的"活着"：只留极轻的呼吸环，**不加横向扫光**。
  //
  // 这里曾经为了把静止占比压到硬指标以下，加了一道每 2.9s 扫过全屏的亮带。
  // 数字确实从 75% 降到了 39%，但**收尾镜头本来就不该这样动**——
  // 字标落定后观众的注意力在名字上，一道定时扫过的光只会把它读成片头广告。
  // 静止占比是参考量；这一幕宁可"静"。
  const hal = 0.5 + 0.5 * Math.sin(t * (Math.PI * 2 / 3.4));
  for (let i = 3; i >= 1; i--) {
    pushCircle(sh, CX, CY + 40, 232 + i * 46 + hal * 14, '#3B82F6', 0.014 * (4 - i) * 0.5);
  }

  envStream(sh, W, H, t, '#7FB2FF', 34, 170, 0.20);
  // 呼吸只到"不冻结"的程度（不开快扫）：收尾要能静得住。
  envOverlay(sh, W, H, t, 0, { sweep: false });

  return { states: layers.map(() => ({})), shapes: sh };
}
