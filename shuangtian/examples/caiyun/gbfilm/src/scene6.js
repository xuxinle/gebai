// 幕 6「回路」5.4s —— 双同心环 + 30 个节点沿环流动 + 中心大字。
// 讲一件事：**它改自己也走同一条回路**（自指闭环，不是单向流水线）。
function scene_loop(t, duration, canvas, layers) {
  const W = 1920, H = 1080;
  const sh = [];
  const CX = W * 0.5, CY = H * 0.47;

  envBackdrop(sh, W, H, t, 1, CX, CY);
  envGrid(sh, W, H, t, 140, '#141F35', 0.26, 6);

  // 整组缓慢推近：1.0 → 1.14（读作"贴近看它怎么改自己"）
  const push = 1.0 + 0.14 * EASE.expoOut(seg(t, 0.2, 4.6));
  const rw = (v) => v * push;

  const rings = [
    { r: 330, n: 18, dir: -1, speed: 0.42, size: 7, col: '#3B82F6' },
    { r: 214, n: 12, dir: 1, speed: 0.63, size: 5.5, col: '#7FB2FF' }
  ];

  // 环本体：分段画，亮度沿线流动（环自己也在传信号）
  for (const ring of rings) {
    const N = 96;
    for (let s = 0; s < N; s++) {
      const a0 = (s / N) * Math.PI * 2 + t * ring.speed * 0.12 * ring.dir;
      const x = CX + Math.cos(a0) * rw(ring.r);
      const y = CY + Math.sin(a0) * rw(ring.r) * 0.72;
      const flow = Math.pow(0.5 + 0.5 * Math.sin(a0 * 3 - t * 2.1 * ring.dir), 3);
      pushCircle(sh, x, y, rw(2.6), ring.col, 0.10 + flow * 0.44);
    }
  }

  // 节点：流动 + 拖尾（残影）
  for (const ring of rings) {
    for (let i = 0; i < ring.n; i++) {
      const base = (i / ring.n) * Math.PI * 2;
      for (let tr = 0; tr < 3; tr++) {
        const ph = base + (t - tr * 0.075) * ring.speed * ring.dir;
        const x = CX + Math.cos(ph) * rw(ring.r);
        const y = CY + Math.sin(ph) * rw(ring.r) * 0.72;
        const alpha = tr === 0 ? 0.95 : 0.22 / tr;
        const sz = rw(ring.size * (tr === 0 ? 1 : 0.8));
        pushCircle(sh, x, y, sz, tr === 0 ? '#EAF4FF' : ring.col, alpha);
        if (tr === 0) pushCircle(sh, x, y, sz * 2.1, ring.col, 0.16);
      }
    }
  }

  // 中心两行字（逐字入场，随推镜一起放大）
  const tIn = 0.75;
  pushTypeIn(sh, '改自己', CX, CY - 100 * push, 66 * push, 8, '#EAF0FF', t, tIn, 0.05, 26);
  pushTypeIn(sh, '也走同一条回路', CX, CY - 4 * push, 44 * push, 5, '#9EC4FF',
             t, tIn + 0.45, 0.04, 20);

  // 中心脉冲（心跳：2.2s 一次）
  const pulse = 0.5 + 0.5 * Math.sin(t * (Math.PI * 2 / 2.2));
  for (let i = 3; i >= 1; i--) {
    pushCircle(sh, CX, CY + 36 * push, rw(26 + i * 22 + pulse * 12), '#3B82F6',
               0.05 * (4 - i) * (0.6 + pulse * 0.4));
  }

  // 底部：验证正在跑（进度条 + 逐条通过的断言计数——数字在跳 = 明暗在变）
  const runP = clamp01((t - 2.2) / 2.4);
  if (runP > 0.01) {
    pushText(sh, 'run_tests: 0 failed', 150, 908, 18, '#6F86A6', 0.85 * runP);
    pushRect(sh, 150, 940, 360, 3, '#1B2740', 0.9);
    pushRect(sh, 150, 940, 360 * runP, 3, '#4ADE80', 0.85);
    const passed = Math.floor(clamp01((t - 2.2) / 3.0) * 153);
    pushText(sh, String(passed), 1240, 898, 40, '#4ADE80', 0.9);
    pushText(sh, 'assertions', 1300, 914, 18, '#5E7A99', 0.8);
  }

  envStream(sh, W, H, t, '#7FB2FF', 42, 175, 0.22);
  // 30 个节点沿双环反向流动 + 环流向 + 推近，主体自身就在持续运动——不叠环境层。

  return { states: layers.map(() => ({})), shapes: sh };
}
