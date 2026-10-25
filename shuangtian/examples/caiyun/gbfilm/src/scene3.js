// 幕 3「工具面」5.8s —— 23 张工具卡弹簧错峰弹入。
// 讲一件事：**面**（不是一个功能，而是一整片可调用的面）。
// 手法：弹簧（ζ0.55/ω19，可过冲）+ 径向错峰；落定后逐卡微起伏。
const TOOLS = [
  ['caiyun', '裁云 · 视频'], ['shuangtian', '霜天 · 原生 UI'], ['imagegen', '文生图'],
  ['tts', '语音与音效'], ['louyue', '镂月 · 海报'], ['code', '编码与修复'],
  ['explore', '代码探索'], ['playwright', '浏览器自动化'], ['desktop', '桌面操控'],
  ['wps', '文档表格'], ['feishu', '飞书协同'], ['llm_infer', '本地推理'],
  ['torch', 'PyTorch 剖析'], ['nsight', 'GPU 性能'], ['vision', '视觉识别'],
  ['triage', '大小模型分诊'], ['task', '无人值守任务'], ['session', '会话管控'],
  ['memory', '跨会话记忆'], ['approve', '审批与留痕'], ['sandbox', '沙箱隔离'],
  ['git', '版本与回滚'], ['shell', '任意进程']
];

function scene_tools(t, duration, canvas, layers) {
  const W = 1920, H = 1080;
  const sh = [];

  envBackdrop(sh, W, H, t, 1, W * 0.5, H * 0.55);
  envGrid(sh, W, H, t, 120, '#16223A', 0.30, 8);

  // 标题（`pushTypeIn` 收的是**中心 x**，不是左边界——传左边界会把标题左半推出画面）
  const titleW = textW('二十三种能力', 46) + 3 * 9;
  pushTypeIn(sh, '二十三种能力', 150 + titleW * 0.5, 128, 46, 3, '#EAF0FF', t, 0.10, 0.035, 22);
  const subW = textW('一次装载，全部并入工具面', 26) + 1.5 * 10;
  pushTypeIn(sh, '一次装载，全部并入工具面', 560 + subW * 0.5, 146, 26, 1.5, '#7C93B4',
             t, 0.55, 0.016, 14);

  // 计数 0 → 23（数字在变 = 明暗在跳）
  const cnt = Math.min(Math.floor(EASE.expoOut(seg(t, 0.55, 2.35)) * TOOLS.length + 0.0001), TOOLS.length);
  pushText(sh, String(cnt).padStart(2, '0'), 1688, 114, 62, '#3B82F6', 0.92);
  pushText(sh, '/ 23', 1758, 146, 22, '#5E7A99', 0.8);

  // 卡片网格：总宽必须留出两侧安全区（实测 6×268+16 会顶到画面边缘，标题被裁）
  const COLS = 6, ROWS = 4;
  const CW = 244, CH = 112, GX = 15, GY = 15;
  const totalW = COLS * CW + (COLS - 1) * GX;
  const X0 = (W - totalW) * 0.5;
  const Y0 = 252;
  const CXg = W * 0.5, CYg = Y0 + (ROWS * CH + (ROWS - 1) * GY) * 0.5;

  for (let i = 0; i < TOOLS.length; i++) {
    const col = i % COLS, row = Math.floor(i / COLS);
    const x = X0 + col * (CW + GX);
    const y = Y0 + row * (CH + GY);
    const cx = x + CW * 0.5, cy = y + CH * 0.5;

    // 径向错峰：从中心向外依次弹出
    const d = Math.hypot(cx - CXg, cy - CYg) / 700;
    const delay = 0.18 + d * 0.78 + hash01(i, 7) * 0.16;
    const tau = t - delay;
    if (tau <= 0) continue;
    const s = spring(tau, 0.55, 19);
    const a = clamp01(tau / 0.22);
    // 落定后极缓起伏 + 斑马纹亮度波（逐卡相位不同 → 整片不会像一块板）
    const wob = Math.sin(t * 0.9 + i * 0.7) * 2.6 * clamp01((t - 1.6) / 0.8);
    // 亮度灏：一行一行掠过的亮带（存在感在变，验收网格测得到）
    const sweepY = -200 + ((t * 1250) % (ROWS * CH + 600));
    const cardCy = cy - Y0;
    const band = clamp01(1 - Math.abs(cardCy - sweepY) / 190);
    const lit = 0.90 + band * 0.55;

    const sc = 0.84 + 0.16 * s;
    const w = CW * sc, h = CH * sc;
    const bx = cx - w * 0.5, by = cy - h * 0.5 + wob;

    pushRect(sh, bx, by, w, h, mixHex('#101827', '#1B2C4A', band * 0.9), 0.92 * a * Math.min(lit, 1), 12);
    pushRect(sh, bx, by, w, 1.5, mixHex('#233450', '#5B8CFF', band), 0.75 * a, 0);
    pushRect(sh, bx, by, 2.5, h, '#3B82F6', Math.min(0.95, 0.55 * a + band * 0.4), 1.5);

    pushText(sh, String(i + 1).padStart(2, '0'), bx + 16, by + 16, 16, '#3B82F6', 0.75 * a);
    pushText(sh, TOOLS[i][0], bx + 16, by + 40, 26, '#EAF0FF', 0.95 * a);
    pushText(sh, TOOLS[i][1], bx + 16, by + 78, 18, '#7C93B4', 0.85 * a);

    const blink = 0.35 + 0.65 * Math.abs(Math.sin(t * 1.7 + i * 0.5));
    pushCircle(sh, bx + w - 18, by + 20, 3.2, '#4ADE80', 0.7 * a * blink);
  }

  const footP = EASE.expoOut(seg(t, 2.4, 3.2));
  if (footP > 0.01) {
    pushText(sh, '装载即并入 —— 不需要注册、不需要重启、不需要改调用方', 150, 972, 22,
             '#6F86A6', 0.9 * footP);
  }

  envStream(sh, W, H, t, '#6FA8FF', 38, 185, 0.22);
  // 覆盖层必须在**所有内容之后**（呼吸作用于整帧，才与验收格差 1:1）
  envOverlay(sh, W, H, t, 1);

  return { states: layers.map(() => ({})), shapes: sh };
}
