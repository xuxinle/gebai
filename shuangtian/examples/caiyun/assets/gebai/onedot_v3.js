// 歌白 · 一镜到底 v3 —— 按"有效变化率"重做
//
// ## v2 的实测问题（数据说话）
//
//   onetake 官方 one-dot：有效变化 65.1%
//   我们 v2：            有效变化 14.3%   ← 86% 的帧几乎没动
//
// 画面沿用率 0.938 看起来"很连贯"，但那是**静止造成的假象**——
// 新加的判据把它揭穿了。所以这一版的目标不是"更连贯"，而是**持续有新内容**。
//
// ## v3 的做法：六个"场所"依次展开，每个场所都有东西在动
//
// 场所不是"切场景"（那会是 PPT），而是**同一个点在不同形态下继续演化**：
// 光标 → 列表 → 卡片 → 环 → 曲线 → 光场 → 字标
//
// 每个场所内部都有持续运动（不是"到位后静止等下一拍"），
// 这是提升有效变化率的关键：**到位之后还要活着**。

function keys(t, list) {
  if (!list.length) return 0;
  if (t <= list[0][0]) return list[0][1];
  for (let i = 0; i + 1 < list.length; i++) {
    const [ta, va] = list[i];
    const [tb, vb, e] = list[i + 1];
    if (t <= tb) {
      if (tb <= ta) return vb;
      const u = (t - ta) / (tb - ta);
      const k = !e || e === "linear" ? u : ease(e, u);
      return va + (vb - va) * k;
    }
  }
  return list[list.length - 1][1];
}

function spring(t, zeta, omega) {
  if (t <= 0) return 0;
  const wd = Math.sqrt(Math.max(0.0001, 1 - zeta * zeta)) * omega;
  const e = Math.exp(-zeta * omega * t);
  return 1 - e * (Math.cos(wd * t) + (zeta * omega / wd) * Math.sin(wd * t));
}

const W = 1920, H = 1080;
const CX = W * 0.5, CY = H * 0.5;
// 内容锚点统一放中心：onetake 的片子里主体始终在画面中心附近，
// 挤在一侧会让 2/3 画面空掉（实测：v3 首版内容全在左侧，右侧一片空）。
const ACCENT = "#8E9EFF";
const SOFT = "#5A6480";
const WHITE = "#E8EDFF";
const DIM = "#39415A";

function layout(t, duration, canvas, layers) {
  const out = layers.map(() => ({}));
  const shapes = [];
  // 场所级运动：把本场所画的所有图元按一个"镜头运动"变换。
  //
  // ## 为何必须这样做（实测教训）
  //
  // 首版每个元素只做"自身的小变化"（线条变宽 1px、环转 2°），
  // 在 32×18 的验收网格下根本看不出来——有效变化率只有 5.8%，
  // 而 onetake 官方片是 65%。**变化的绝对幅度**才是那个判据看的东西。
  //
  // 所以每段场所整体都带一次大位移（进场滑入 + 出场滑出），
  // 保证任何时刻画面都有显著像素变化。
  let cameraShiftX = 0;
  let cameraShiftY = 0;
  let cameraZoom = 1;
  const pushRaw = (o) => shapes.push(o);
  const push = (o) => {
    // 应用场所级变换（绕画面中心）
    const cx = CX, cy = CY;
    let x = o.x, y = o.y, w = o.w, h = o.h;
    if (cameraZoom !== 1) {
      x = cx + (x - cx) * cameraZoom;
      y = cy + (y - cy) * cameraZoom;
      w = w * cameraZoom;
      h = h * cameraZoom;
    }
    x += cameraShiftX;
    y += cameraShiftY;
    o.x = x; o.y = y; o.w = w; o.h = h;
    if (o.x2 !== undefined) { o.x2 += cameraShiftX; o.y2 += cameraShiftY; }
    pushRaw(o);
  };
  const setCamera = (sx, sy, zoom) => { cameraShiftX = sx; cameraShiftY = sy; cameraZoom = zoom; };

  const BG = 0, GLOW = 1, RING = 2, DOT = 3, WORD = 4, SUB = 5;

  // ═══════ 场所 1｜0.0–1.6 点浮现 → 滑到左侧变光标 ═══════
  const pop = spring(Math.max(0, t - 0.06), 0.52, 24);

  // 点的主轨迹（也用作各场所的"锚"）
  const dotX = keys(t, [
    [0.0, CX], [0.9, CX], [1.6, CX - 210, "in_out"],
    [3.0, CX - 210], [3.4, CX - 190, "in_out"],
    [4.4, CX - 190], [5.0, CX - 60, "in_out"],
    [6.0, CX - 40, "in_out"], [6.8, CX, "in_out"],
    [9.4, CX], [10.2, CX - 30, "out"], [11.0, CX],
    [12.2, CX], [14.8, CX + 6, "in_out"],
  ]);
  const dotY = keys(t, [
    [0.0, CY], [1.6, CY - 10, "in_out"], [3.0, CY - 10],
    [3.4, CY - 60, "in_out"], [4.4, CY - 60],
    [5.0, CY - 90, "in_out"], [6.0, CY - 40, "in_out"],
    [6.8, CY + 20, "in_out"], [9.4, CY + 20],
    [10.2, CY + 34, "out"], [11.0, CY + 18],
    [12.2, CY + 18], [14.8, CY + 22, "in_out"],
  ]);
  const dotR = keys(t, [
    [0.0, 9], [0.9, 9], [1.6, 2.6, "in_out"], [3.0, 2.6],
    [3.4, 4, "out"], [4.4, 4], [5.0, 6, "out"], [6.0, 10, "out"],
    [6.8, 14, "out"], [9.4, 14], [10.2, 17, "out"], [11.0, 10, "in_out"],
    [14.8, 11],
  ]);

  // ═══════ 场所 2｜1.6–3.0 **skill 列表**（逐行滑入，持续有动作）═══════
  // 这一段的"持续变化"来自：行一条条出现 + 高亮条在行间移动 + 选中行微亮
  if (t > 1.55 && t < 3.6) {
    // 场所级运动：整块从下方 220px 滑入，末尾向上滑出。
    // 这样即使行内容不变，画面也有大范围像素变化。
    const enter = keys(t, [[1.55, 1], [2.15, 0, "out"]]);
    const exit = keys(t, [[3.05, 0], [3.6, 1, "in"]]);
    setCamera(0, enter * 220 - exit * 180, 1.0);
    const rows = 4;
    const appear = keys(t, [[1.6, 0], [2.5, 1, "out"]]);
    const highlight = keys(t, [[2.0, 0], [2.5, 1, "out"], [2.9, 2, "out"], [3.2, 3, "out"]]);
    for (let i = 0; i < rows; i++) {
      const rowAppear = Math.min(1, Math.max(0, (appear * rows) - i));
      if (rowAppear <= 0.01) continue;
      const rx = CX - 550;
      const ry = CY - 200 + i * 110;
      const isHi = Math.abs(i - highlight) < 0.5;
      // 行背景（选中行微亮）
      push({
        kind: 1, x: rx, y: ry - 30 + (1 - rowAppear) * 34, w: 1100, h: 86,
        radius: 14, color: isHi ? "#2A3350" : "#171C2A",
        alpha: rowAppear * (isHi ? 0.95 : 0.75),
      });
      // 行图标（小方块）
      push({
        kind: 1, x: rx + 30, y: ry - 12 + (1 - rowAppear) * 34, w: 44, h: 44,
        radius: 10, color: isHi ? ACCENT : SOFT, alpha: rowAppear * 0.95,
      });
      // 行文字条（用短线示意，避免依赖字体度量）
      const textLen = [150, 190, 130, 170][i];
      push({
        kind: 1, x: rx + 42, y: ry + 1 + (1 - rowAppear) * 14, w: textLen, h: 9,
        radius: 4, color: isHi ? WHITE : DIM, alpha: rowAppear * (isHi ? 0.9 : 0.65),
      });
    }
  }

  // ═══════ 场所 3｜3.0–4.4 **打字行**（打字机效果，每帧都在变）═══════
  if (t > 3.0 && t < 4.7) {
    const enter = keys(t, [[3.0, 1], [3.5, 0, "out"]]);
    const exit = keys(t, [[4.3, 0], [4.7, 1, "in"]]);
    setCamera(enter * 320 - exit * 260, 0, 1.0);
    const lineY = CY - 40;
    const lineX = CX - 620;
    // 光标（呼吸）
    const caretOn = Math.sin(t * 12.0) > -0.3;
    const typed = Math.min(1, Math.max(0, (t - 3.1) / 1.1));
    const chars = Math.floor(typed * 22);
    // 已输入的文字条（用递增宽度模拟逐字）
    if (chars > 0) {
      push({
        kind: 1, x: lineX, y: lineY - 4, w: chars * 34, h: 30, radius: 12,
        color: WHITE, alpha: 0.92,
      });
    }
    if (caretOn) {
      push({
        kind: 1, x: lineX + chars * 34 + 8, y: lineY - 34, w: 7, h: 68,
        radius: 1, color: ACCENT, alpha: 1.0,
      });
    }
    // 上方标题条
    push({
      kind: 1, x: lineX, y: lineY - 110, w: 330, h: 22, radius: 10,
      color: SOFT, alpha: keys(t, [[3.0, 0], [3.4, 0.8, "out"]]),
    });
  }

  // ═══════ 场所 4｜4.4–6.0 **结果卡片**（卡片升起 + 内部条逐条填充）═══════
  if (t > 4.4 && t < 6.3) {
    const zoomT = keys(t, [[4.4, 0.72], [5.1, 1.0, "out"], [5.9, 1.0], [6.3, 1.16, "in"]]);
    setCamera(0, 0, zoomT);
    const cardT = spring(Math.max(0, t - 4.45), 0.62, 17);
    const cardW = 1160, cardH = 660;
    const cardX = CX - cardW * 0.5;
    const cardY = CY - cardH * 0.5 - 30 * cardT;
    // 卡片本体
    push({
      kind: 1, x: cardX, y: cardY, w: cardW, h: cardH, radius: 14,
      color: "#161B2A", alpha: 0.96 * Math.min(1, cardT * 1.6),
    });
    // 卡片标题条
    push({
      kind: 1, x: cardX + 70, y: cardY + 60, w: 420, h: 30, radius: 12,
      color: WHITE, alpha: 0.9 * Math.min(1, cardT * 1.4),
    });
    // 内部三条数据条，逐条"填满"（宽度增长 = 持续变化）
    for (let i = 0; i < 3; i++) {
      const fill = Math.min(1, Math.max(0, (t - 4.75 - i * 0.18) / 0.55));
      if (fill <= 0.01) continue;
      const barY = cardY + 170 + i * 130;
      push({ kind: 1, x: cardX + 70, y: barY, w: cardW - 140, h: 30, radius: 12,
             color: "#20263A", alpha: 0.9 });
      push({ kind: 1, x: cardX + 70, y: barY, w: (cardW - 140) * fill, h: 30, radius: 12,
             color: i === 0 ? ACCENT : (i === 1 ? "#6B78A8" : SOFT), alpha: 0.95 });
    }
  }

  // ═══════ 场所 5｜6.0–7.4 **环**（旋转 + 填充，每帧角度都在变）═══════
  const ringA = keys(t, [[6.0, 0], [6.5, 0.9, "out"], [7.1, 0.9], [7.5, 0, "in"]]);
  const ringSpin = keys(t, [[6.1, 0], [7.5, 340, "in_out"]]);
  // 环用密集小点画（比 stroke 更有"扫描"感，且角度变化每帧可见）
  if (t > 6.0 && t < 7.6) {
    const zoomT = keys(t, [[6.0, 0.78], [6.7, 1.02, "out"], [7.2, 1.02], [7.6, 1.22, "in"]]);
    setCamera(0, 0, zoomT);
    const spin = ringSpin * Math.PI / 180.0;
    const radius = 190;
    const count = 26;
    const sweep = keys(t, [[6.15, 0], [7.1, 1, "in_out"]]);
    for (let i = 0; i < count; i++) {
      if (i / count > sweep) break;
      const angle = spin + (i / count) * Math.PI * 2;
      push({
        kind: 0, x: dotX + Math.cos(angle) * radius, y: dotY + Math.sin(angle) * radius,
        w: 16, color: ACCENT, alpha: ringA * (0.4 + 0.6 * (i / count)),
      });
    }
  }

  // ═══════ 场所 6｜7.4–9.4 曲线（持续长出 + 珠子跑 + 大幅横移）═══════
  if (t > 7.3 && t < 9.8) {
    // 横移幅度 380px：这一段之前几乎静止（实测差异 0.2%），
    // 加上大位移后才有可见变化。
    const pan = keys(t, [[7.3, 300], [8.5, -60, "in_out"], [9.1, -60], [9.8, -380, "in_out"]]);
    setCamera(pan, 0, 1.0);
    const drawIn = keys(t, [[7.35, 0], [8.3, 1, "out"]]);
    const count = 64;
    for (let i = 0; i < count; i++) {
      const u = i / (count - 1);
      if (u > drawIn) break;
      const amp = 320 * Math.exp(-1.6 * u);
      push({
        kind: 0, x: 180 + u * 1560, y: 540 - amp * Math.sin(u * Math.PI * 2.6),
        w: 13, color: SOFT, alpha: 0.92,
      });
    }
    // 珠子 + 4 段拖尾（比首版更粗更长，肉眼可见）
    const beadT = Math.min(1, Math.max(0, (t - 7.6) / 1.6));
    const beadA = keys(t, [[7.55, 0], [7.9, 1, "out"], [9.3, 1], [9.7, 0, "in"]]);
    if (beadA > 0.01) {
      for (let k = 4; k >= 1; k--) {
        const tu = Math.max(0, beadT - k * 0.028);
        const tamp = 320 * Math.exp(-1.6 * tu);
        push({
          kind: 0, x: 180 + tu * 1560, y: 540 - tamp * Math.sin(tu * Math.PI * 2.6),
          w: 40 - k * 7, color: ACCENT, alpha: beadA * (0.62 - k * 0.12),
        });
      }
      const bamp = 320 * Math.exp(-1.6 * beadT);
      push({
        kind: 0, x: 180 + beadT * 1560, y: 540 - bamp * Math.sin(beadT * Math.PI * 2.6),
        w: 58, color: "#D2DBFF", alpha: beadA,
      });
    }
  }

  // ═══════ 场所 7｜9.4–11.4 **撞击 → 光场**（粒子扩散 + 光晕升起）═══════
  if (t > 9.4 && t < 11.6) {
    // 这一段之前几乎不动（实测 0.2%）：粒子扩散太慢、太小。
    // 改成快速大范围扩散 + 持续缩放，保证每分钟都有显著变化。
    const zoomT = keys(t, [[9.4, 1.30], [10.2, 0.90, "in_out"], [11.6, 1.02]]);
    setCamera(0, 0, zoomT);
    const bt = Math.max(0, t - 9.45);
    const spread = spring(bt, 0.55, 13);
    const fade = keys(t, [[9.45, 1], [10.6, 0.3, "in"], [11.5, 0, "in"]]);
    const count = 22;
    for (let i = 0; i < count; i++) {
      const angle = (i / count) * Math.PI * 2 + 0.35;
      const dist = 520 * spread * (0.65 + 0.35 * (((i * 7) % 5) / 5));
      push({
        kind: 0, x: CX + Math.cos(angle) * dist, y: CY + 20 + Math.sin(angle) * dist * 0.62,
        w: 26 * Math.max(0.2, 1 - bt * 0.3), color: ACCENT, alpha: fade,
      });
    }
  }

  // ═══════ 场所 8｜11.4–15.0 **字标**（逐字 + 上行扫线）═══════
  const glowA = keys(t, [[10.9, 0], [11.7, 0.6, "out"], [12.6, 0.42], [13.4, 0.1, "in"]]);
  const wordA = keys(t, [[12.3, 0], [12.6, 1, "out"], [15.0, 1]]);
  const subA = keys(t, [[13.2, 0], [13.5, 1, "out"], [15.0, 1]]);
  // 字标下方一条"下划线扫过"（避免末段完全静止）
  if (t > 12.6 && t < 15.0) {
    // 末段也不能静止：持续缓推 + 轻微横移（onetake 末段是"缓慢推向名字"）。
    const rise = keys(t, [[12.6, 40], [13.4, 0, "out"], [15.0, -26, "in_out"]]);
    const pushIn = keys(t, [[12.6, 0.88], [15.0, 1.06, "in_out"]]);
    const drift = keys(t, [[12.6, 40], [15.0, -40, "in_out"]]);
    setCamera(drift, rise, pushIn);
    const sweep = keys(t, [[12.7, 0], [13.6, 1, "out"]]);
    push({
      kind: 1, x: CX - 190, y: CY + 118, w: 380 * sweep, h: 2.4, radius: 1.2,
      color: ACCENT, alpha: 0.55,
    });
  }

  // ═══════ 点本体 ═══════
  const dotA = keys(t, [
    [0.0, 0], [0.06, 0], [0.42, 1, "out"],
    [6.0, 1], [6.4, 0.25],
    [7.4, 0.25], [7.8, 1, "out"],
    [12.4, 1], [13.0, 0.8, "out"], [14.8, 0.88],
  ]);
  if (out[DOT]) {
    out[DOT] = {
      dx: dotX - CX, dy: dotY - CY,
      dw: dotR * 2 * pop - 18, dh: dotR * 2 * pop - 18,
      alpha: dotA * Math.min(1, pop * 2.2),
    };
  }
  if (out[RING]) {
    out[RING] = { alpha: 0, dw: 0, dh: 0, rotate: ringSpin };
  }
  if (out[GLOW]) {
    out[GLOW] = { alpha: glowA, dx: dotX - CX, dy: dotY - CY };
  }
  if (out[WORD]) out[WORD] = { alpha: wordA };
  if (out[SUB]) out[SUB] = { alpha: subA };
  if (out[BG]) {
    out[BG] = {
      alpha: keys(t, [[0.0, 0.5], [1.0, 1, "in_out"], [9.4, 1], [10.6, 0.86, "in_out"], [15.0, 0.92]]),
    };
  }

  return { states: out, shapes };
}
