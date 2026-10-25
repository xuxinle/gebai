// 幕 2「躯体」5.2s —— 三维共享世界 + 左侧标识。
// 讲一件事：**智能有了身体**。三维负责实体感，二维负责标识。
//
// 两条实测约束：
// · 字标**只用二维文字**画（`kintext` 图层与手绘文字会叠印——实测同位置出现两份 GEBAI）；
// · 三维物体要**稀疏**（三个就够），彼此留空才有"展台"感，挤在一起读作杂物堆。
function scene_body(t, duration, canvas, layers) {
  const W = 1920, H = 1080;
  const sh = [];

  envBackdrop(sh, W, H, t, 1, W * 0.66, H * 0.46);

  // 左侧竖向支撑线：0 → 100% 长开
  const barP = EASE.expoOut(seg(t, 0.25, 1.5));
  if (barP > 0.01) pushRect(sh, 150, 248, 2, 588 * barP, '#3B82F6', 0.55 * barP);

  // 标识：逐字入场（只用一套字，不再叠 kintext 图层）
  const tIn = 0.72;
  pushTypeIn(sh, 'GEBAI', 476, 348, 104, 14, '#EAF0FF', t, tIn, 0.075, 34);
  pushTypeIn(sh, 'AGENT', 470, 478, 40, 18, '#6FA8FF', t, tIn + 0.42, 0.06, 22);

  // 释义三行：错峰上浮
  const rows = [
    '收编任意语言、任意进程',
    '一个工具面，装载即并入',
    '无状态智能，责任在智体'
  ];
  for (let i = 0; i < rows.length; i++) {
    const p = EASE.expoOut(seg(t, 1.30 + i * 0.16, 1.30 + i * 0.16 + 0.60));
    if (p > 0.01) {
      pushCircle(sh, 166, 700 + i * 54 + (1 - p) * 16, 3.4, '#3B82F6', 0.9 * p);
      pushText(sh, rows[i], 192, 680 + i * 54 + (1 - p) * 16, 28, '#9DB2CE', 0.94 * p);
    }
  }

  // 规格行（技术感，克制）
  pushText(sh, '23 SUB-AGENTS   ·   ORDERS: 80+ TOOLS', 152, 268, 18, '#5E7A99', 0.72);

  envStream(sh, W, H, t, '#6FA8FF', 44, 160, 0.24);
  // 覆盖层必须在**所有内容之后**（呼吸作用于整帧，才与验收格差 1:1）
  envOverlay(sh, W, H, t, 1);

  return { states: layers.map(() => ({})), shapes: sh };
}
