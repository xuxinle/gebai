// 幕 4「钻穿」5.4s —— 终端窗口 + 推镜。
// 讲一件事：**它能自己跑，也能让你钻进去看**。
//
// ## 实测：`shapes` 不过相机，推镜必须自己算
//
// 场景字段 `cam_vector`/`cam_zoom` 只作用于图层（文字/图片/三维），
// 对 `shapes` 完全无效。第一版把推镜交给场景字段，结果窗口只跟着**平移**了一半
// （锚点对齐造成）而没有放大——静止占比 95%，几乎整幕不动。
//
// 这里改成把"相机"写进脚本：所有坐标先送进 `P()` 变换，锚点选窗口左上。
// 同时窗口**不能铺满全屏**：它盖住了环境层，让扩散环/快扫完全看不见
// （这也是本幕静止占比最高的直接原因）。
function scene_terminal(t, duration, canvas, layers) {
  const W = 1920, H = 1080;
  const sh = [];

  envBackdrop(sh, W, H, t, 1, W * 0.5, H * 0.46);

  // 推镜：0.25 → 3.10s 推到 1.30；锚点 = 窗口左上角
  const zoom = 1.0 + 0.30 * EASE.expoOut(seg(t, 0.25, 3.10));
  const AX = 330, AY = 250;
  // 画出区域限制在窗口内（模拟裁剪）：窗口外的图元直接不 push
  const inWin = (x, y) => (x >= AX + 4 && x <= AX + 880 + 4 && y >= AY + 4 && y <= AY + 600 + 4);
  const P = (x, y) => ({ x: AX + (x - AX) * zoom, y: AY + (y - AY) * zoom, s: zoom });

  const WX = 330, WY = 250, WW = 880, WH = 600;
  const x0 = AX + (WX - AX) * zoom, y0 = AY + (WY - AY) * zoom;
  const w0 = WW * zoom, h0 = WH * zoom;

  // 窗口底 + 标题栏
  pushRect(sh, x0, y0, w0, h0, '#0B1220', 0.94, 14 * zoom);
  pushRect(sh, x0, y0, w0, 32 * zoom, '#111A2B', 0.97, 12 * zoom);
  const R = P(WX + 22, WY + 16);
  pushCircle(sh, R.x, R.y, 5 * zoom, '#FF5F57', 0.85);
  const Y1 = P(WX + 40, WY + 16);
  pushCircle(sh, Y1.x, Y1.y, 5 * zoom, '#FEBC2E', 0.85);
  const G1 = P(WX + 58, WY + 16);
  pushCircle(sh, G1.x, G1.y, 5 * zoom, '#28C840', 0.85);
  const T0 = P(WX + 82, WY + 6);
  pushText(sh, 'gebai — agent session', T0.x, T0.y, 16 * zoom, '#5E7A99', 0.9);

  // 输出流：逐行淡入
  const lines = [
    ['>', 'agent_load caiyun', '#7C93B4'],
    ['=', '工具面并入：18 tools', '#8FD3A0'],
    ['>', 'run(action=build, target=caiyun)', '#7C93B4'],
    ['=', '构建完成 [dev] 单元 153 · 1.52s', '#8FD3A0'],
    ['>', 'capture → qa/frame-120.png', '#7C93B4'],
    ['=', 'caiyun_verify: 静止 12.4% · 变化 3.2/s', '#8FD3A0']
  ];
  for (let i = 0; i < lines.length; i++) {
    const t0 = 0.28 + i * 0.28;
    const p = EASE.expoOut(seg(t, t0, t0 + 0.40));
    if (p <= 0.01) continue;
    const ly = WY + 66 + i * 78;
    const bar = P(WX + 30, ly + 2);
    if (inWin(WX + 30, ly + 2)) {
      pushRect(sh, bar.x, bar.y, 3 * zoom, 30 * zoom * p, lines[i][2], 0.8 * p);
    }
    const A = P(WX + 52, ly);
    const B = P(WX + 90, ly);
    if (inWin(WX + 52, ly)) pushText(sh, lines[i][0], A.x, A.y, 26 * zoom, lines[i][2], 0.95 * p);
    if (inWin(WX + 90, ly)) pushText(sh, lines[i][1], B.x, B.y, 26 * zoom, '#D6E2F5', 0.92 * p);
  }

  // 光标
  if (Math.sin(t * 6.9) > -0.2) {
    const C = P(WX + 90, WY + 66 + (lines.length - 1) * 78 + 40);
    pushRect(sh, C.x, C.y, 13 * zoom, 30 * zoom, '#8FD3A0', 0.9);
  }

  // 扫描线：窗口内上下走一程（速度提上去，跨格）
  const scanY = WY + 36 + ((t * 700) % (WH - 50));
  const S = P(WX + 2, scanY);
  pushRect(sh, S.x, S.y, w0 - 4 * zoom, 2.5 * zoom, '#2F6BFF', 0.13);

  // 右下角：一块实时指标（明暗在跳，给"在跑"的持续证据）
  const mx = 1320, my = 690;
  pushRect(sh, mx, my, 460, 190, '#0B1220', 0.88, 12);
  pushRect(sh, mx, my, 460, 1.6, '#233450', 0.7);
  pushText(sh, 'LIVE  METRICS', mx + 22, my + 18, 17, '#5E7A99', 0.85);
  const bars = ['build', 'render', 'verify', 'carry'];
  for (let i = 0; i < bars.length; i++) {
    const v = 0.35 + 0.55 * Math.abs(Math.sin(t * (1.1 + i * 0.37) + i));
    const by = my + 56 + i * 32;
    pushText(sh, bars[i], mx + 22, by - 4, 17, '#7C93B4', 0.85);
    pushRect(sh, mx + 120, by + 4, 300, 8, '#16223A', 0.9, 4);
    pushRect(sh, mx + 120, by + 4, 300 * v, 8, '#4D7DFF', 0.9, 4);
  }

  pushText(sh, 'headless backend · 无显示服务也能构建 / 运行 / 验证', 150, 972, 20, '#5E7A99', 0.78);
  envStream(sh, W, H, t, '#6FA8FF', 36, 210, 0.20);
  // 本幕是**这一部里唯一真需要环境层的镜头**：终端窗口是个大块静止内容，
  // 推镜只在前 3.1s 推进，之后必须靠呼吸带住“它还在跑”。
  // 但**不开横向快扫**——终端里已有扫描线与实时指标条，再横扫就是噪声。
  envOverlay(sh, W, H, t, 1, { sweep: false });

  return { states: layers.map(() => ({})), shapes: sh };
}
