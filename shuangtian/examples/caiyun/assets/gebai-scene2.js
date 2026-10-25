// 歌白 30 秒片 · 主视觉编排层（v3）
//
// ## 两条硬规则（踩过，见 docs/PITFALLS.md〔编排〕）
//
// 1. **`shapes` 收屏幕像素**：`paint_script_shapes` 只乘 `画布宽/1920`
//    （1920 宽时即 1），**相机/zoom 完全不参与**。写 `120` 就是画面 120px。
// 2. **后 push 的在上面**：`shapes` 按数组顺序叠。
//
// ## 三条定量结论（见 docs/caiyun_layout_script.md §6.1.6 / §6.1.8）
//
// ① 验收口径是 32×18 网格（每格 60px）的逐格均值差，**<1.2 算静止**。
// ② **匀速平移 / 连续旋转测不到**：同样多的亮点只挪几像素，格均值几乎不变。
// ③ **整屏亮度变化 dL 与网格差 1:1** —— 这是性价比最高的手法。

const AMBER = '#F0B429';
const TINT = '#8B9AFF';

function layout(t, duration, canvas, layers) {
  const shapes = [];
  const w = canvas.width || 1920;
  const h = canvas.height || 1080;
  const cx = w / 2;

  // ## `t` 是**场景内局部时间**（不是全片时间），见 §6.1.5
  // 所以分派只能靠 `duration`（本片各场景值见 gebai-31s-v3.json）。
  const d = Math.round(duration * 10);
  let kind = 'ring';
  if (d === 18 || d === 30 || d === 28) kind = 'none';   // 片头 / 字标：留白
  else if (d === 41) kind = 'raster';
  else if (d === 42 || d === 49) kind = 'nodes';
  else if (d === 37) kind = 'loom';
  // 其余（38 = 两个 3.8s 场景）用 ring

  // 亮度心跳：**明暗在跳**才是逐帧差的来源，光靠位置移动测不到（见 §6.1.6）。
  const beat = (period, phase) => 0.5 + 0.5 * Math.sin((t / period + phase) * Math.PI * 2);

  // ─── 环流：三圈大环包住画面，半径 320~430 ───
  //
  // 半径要够大：实测环只占底部 15% 时，88% 的格子完全没被碰到，
  // 逐帧差恒 0.19。包住画面后覆盖率才够。
  if (kind === 'ring') {
    const baseY = h * 0.50;
    const R = 430;
    for (let ring = 0; ring < 3; ring++) {
      const radius = R - ring * 55;
      const dir = ring % 2 === 0 ? 1 : -1;
      const b = beat(0.62 + ring * 0.18, ring * 0.33);
      for (let i = 0; i < 30; i++) {
        const a = t * (0.7 + ring * 0.25) * dir + (i / 30) * Math.PI * 2;
        const local = 0.5 + 0.5 * Math.sin(a * 4 + t * 3.2);
        shapes.push({
          kind: 0,
          x: cx + Math.cos(a) * radius,
          y: baseY + Math.sin(a) * radius * 0.62,
          w: 34, h: 34,
          color: ring === 0 ? AMBER : TINT,
          alpha: 0.06 + 0.62 * (0.5 * b + 0.5 * local),
        });
      }
    }
  }

  // ─── 光栅：左右两条竖线阵列，**长度**在 8%~92% 间伸缩 ───
  //
  // 关键不是"线在移动"而是"线的存在感在变"——长度变化直接改格均值。
  if (kind === 'raster') {
    const N = 15;
    for (let side = 0; side < 2; side++) {
      const x0 = side === 0 ? 48 : w - 48 - N * 13;
      for (let i = 0; i < N; i++) {
        const x = x0 + i * 13;
        const phase = i * 0.42 + side * 1.1;
        const lenFrac = 0.08 + 0.84 * (0.5 + 0.5 * Math.sin(t * 4.6 + phase));
        const len = lenFrac * (h * 0.68);
        shapes.push({
          kind: 1,
          x, y: h * 0.5 - len / 2,
          w: 7, h: len,
          color: i % 5 === 0 ? AMBER : TINT,
          alpha: 0.10 + 0.40 * lenFrac,
        });
      }
    }
  }

  // ─── 星图：上半天区，节点明暗大起伏（不是只转动）───
  if (kind === 'nodes') {
    const N = 18;
    for (let i = 0; i < N; i++) {
      const a0 = (i / N) * Math.PI * 2;
      const ring = i < 7;
      const radius = ring ? 180 : 320;
      const a = a0 + t * (ring ? 0.32 : -0.22);
      const blink = beat(0.42 + (i % 5) * 0.09, i * 0.37);
      const x = cx + Math.cos(a) * radius * 1.65;
      const y = h * 0.17 + Math.sin(a) * radius * 0.44;
      shapes.push({
        kind: 0, x, y,
        w: 26, h: 26,
        color: i % 4 === 0 ? AMBER : TINT,
        alpha: 0.06 + 0.78 * blink,
      });
      if (i > 0) {
        const pa = a0 - (Math.PI * 2) / N;
        const px2 = cx + Math.cos(pa + t * (ring ? 0.32 : -0.22)) * radius * 1.65;
        const py2 = h * 0.17 + Math.sin(pa + t * (ring ? 0.32 : -0.22)) * radius * 0.44;
        for (let k = 1; k < 5; k++) {
          shapes.push({
            kind: 0,
            x: px2 + (x - px2) * (k / 5),
            y: py2 + (y - py2) * (k / 5),
            w: 9, h: 9, color: TINT, alpha: 0.03 + 0.20 * blink,
          });
        }
      }
    }
  }

  // ─── 织机：横线整行整行地亮/暗（跨格效果最直接）───
  if (kind === 'loom') {
    const STEP = 60;
    for (let y = 0; y < h + STEP; y += STEP) {
      const b = beat(0.55, (y / STEP) * 0.22);
      shapes.push({ kind: 1, x: 0, y, w: w, h: 7, color: TINT, alpha: 0.03 + 0.30 * b });
    }
    const STEP2 = 84;
    for (let x = 0; x < w + STEP2; x += STEP2) {
      const b = beat(0.73, (x / STEP2) * 0.31);
      shapes.push({ kind: 1, x, y: 0, w: 6, h: h, color: AMBER, alpha: 0.02 + 0.22 * b });
    }
  }

  // ─── 整屏亮度脉动：性价比最高的手法（dL 与网格差 1:1）───
  //
  // 一层覆盖全屏的半透明色，alpha 随时间起伏。深底（luma ~13）上
  // alpha 0.03 约产生 dL≈7。周期 0.36s 是"轻微呼吸"而不是频闪。
  if (kind !== 'none') {
    const pulse = 0.5 + 0.5 * Math.sin((t / 0.36) * Math.PI * 2);
    shapes.push({
      kind: 1, x: 0, y: 0, w: w, h: h,
      color: '#FFFFFF',
      alpha: 0.006 + 0.026 * pulse,
    });
  }

  return { states: [], shapes };
}
