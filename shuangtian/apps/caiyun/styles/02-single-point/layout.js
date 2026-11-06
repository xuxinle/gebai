// 裁云脚本公共前言 —— 宿主只提供这些名字（写错就整份脚本静默降级）
//
// 可用：ease(name,t) spring(tau[,zeta,omega]) spring_vel settle
//       lerp(a,b,k) clamp(v,lo,hi) snap(t,fps) noise(seed)
// 不可用：seg / E.* / D() —— 它们不在宿主里，用了就 ReferenceError →
//       裁云降级为纯 JSON → 画面停在初始帧且**零报错**。
//
// LayerDelta 字段：dx dy dw dh scale rotate alpha tint
function seg(t, t0, t1) {
  if (t1 === t0) return (t >= t1) ? 1 : 0;
  const k = (t - t0) / (t1 - t0);
  return k < 0 ? 0 : (k > 1 ? 1 : k);
}
function expoOut(u) { return (u >= 1) ? 1 : 1 - Math.pow(2, -9 * u); }
function expoIn(u) { return u * u * u; }
function expoInOut(u) {
  if (u <= 0) return 0;
  if (u >= 1) return 1;
  return (u < 0.5) ? expoIn(u * 2) / 2 : 1 - expoIn((1 - u) * 2) / 2;
}
function mk(o) {
  const a = (o.alpha === undefined) ? 1 : o.alpha;
  return {
    dx: o.dx || 0, dy: o.dy || 0,
    dw: o.dw || 0, dh: o.dh || 0,
    scale: (o.scale === undefined) ? 1 : o.scale,
    rotate: o.rotate || 0,
    alpha: a < 0 ? 0 : (a > 1 ? 1 : a),
  };
}

// 02 · 单点身份 —— 一个元素演完全片，全程无替换
//
// ## 承接方式：自身 morph
// 边界上活下来的**永远是这个元素本身**。任何一处「旧的淡出、新的淡入」
// 都会让整片散掉——那是本模板唯一不能犯的错。
//
// ## 六段节拍（每段都是同一条 morph 的延续）
//   0.0–0.9   点出现（弹簧）
//   0.9–1.8   点拉长成横线（dw 40→560，dh 40→8）
//   1.8–2.9   线卷成面（dw 560→760，dh 8→300）—— 这就是"卡"
//   2.9–3.7   卡内长出标题（label/sub 淡入，但卡本身没动）
//   3.7–4.5   卡下方裂出三个点（从卡底边缘长出）
//   4.5–5.6   静止（≥1.1s，rest）
// ## 卡必须**原地对中增长**（踩过）
//
// json 里 being 的框是 (940,400,40,40)；裁云的 dw / dh 是**右下单向**增长，
// 所以成卡时右缘直接跑出画面（实测 2.6s 那一帧只剩右上角一块紫、内容全在画外）。
// "中心不动" 必须自己补一半位移： dx = -dw/2 ， dy = -dh/2 。
const CARD = { w: 720, h: 280 };
const HOME = { y: 500 };   // 主体固定中心 y（主体初始中心是 420）

function scene_body(t, duration, canvas, layers) {
  const st = new Array(layers.length);

  // ── 主体四阶段：点 → 线 → 面 ──
  // 用一个 0..1 的总进度驱动，中间两段用不同插值区间
  const kPop  = clamp(spring(t - 0.15, 0.42, 15), 0, 1);        // 点出现
  const kLine = expoInOut(seg(t, 0.90, 1.80));                  // 拉成线
  const kFace = expoInOut(seg(t, 1.80, 2.90));                  // 卷成面

  // 宽：40 → 520（线） → 720（面）
  const wLine = 40 + (520 - 40) * kLine;
  const w = wLine + (CARD.w - 520) * kFace;
  // 高：40 → 8（线） → 280（面）
  const hLine = 40 + (8 - 40) * kLine;
  const h = hLine + (CARD.h - 8) * kFace;
  // **中心固定**：右下单向增长的补偿量 = 增量的一半
  st[0] = mk({
    dw: w - 40,
    dh: h - 40,
    dx: -(w - 40) * 0.5,
    dy: -(h - 40) * 0.5 + (HOME.y - 420) * kFace,
    scale: kPop,
    alpha: kPop,
  });

  // ── 光晕：与主体同心（中心不动，只改尺度），成卡后衰减 ──
  // json: halo 在 (890,350) 尺寸 140 → 中心 (960,420)，与主体同心
  st[1] = mk({
    dy: (HOME.y - 420) * kFace,
    scale: (0.5 + 1.9 * kLine + 1.4 * kFace) * kPop,
    alpha: kPop * (0.9 - 0.5 * kFace),
  });

  // ── 线：在"线"阶段可见，卷成面时收掉（它被面吸收了，不是消失）──
  const threadA = kLine * (1 - kFace);
  st[2] = mk({
    dw: 560 * kLine - 560 * kFace,
    alpha: threadA * 0.9,
    dy: 0,
  });

  // ── 残影：跟主体同宽、同中心（它是主体的"底座"）──
  // json: trail 在 (940,412) 尺寸 40x16 → 中心 (960,420)，与主体同心
  st[3] = mk({
    dw: w - 40,
    dx: -(w - 40) * 0.5,
    dy: (HOME.y - 420) * kFace,
    // 残影只属于「点→线」时期：成卡后必须衰减到 0。
    // 它是一条水平横条，若留在卡里会被读成"卡上划了一道线"（实测）。
    alpha: kPop * 0.5 * (1 - kFace),
  });

  // ── 标题 / 副标：卡成形之后才出现（它们"住在卡里"）──
  const labK = expoOut(seg(t, 2.95, 3.55));
  st[4] = mk({ alpha: labK, dy: (1 - labK) * 14 });
  const subK = expoOut(seg(t, 3.15, 3.75));
  st[5] = mk({ alpha: subK * 0.9, dy: (1 - subK) * 10 });

  // ── 三个分身：从卡底边缘"裂出"，各自错峰 0.09s ──
  // json 里三点已在 (900/955/1010, 640)；卡底中心是 960
  const sx = [900, 955, 1010];
  for (let i = 0; i < 3; i++) {
    const t0 = 3.75 + i * 0.09;
    const k = clamp(spring(t - t0, 0.46, 15), 0, 1);
    st[6 + i] = mk({
      scale: k,
      alpha: seg(t, t0, t0 + 0.10),
      // 从卡底中心 (960) 出发 → 各自位置（json 已给最终位置）
      dx: (960 - sx[i] + 7) * (1 - k),
    });
  }

  return { states: st, shapes: [] };
}

function layout(t, duration, canvas, layers) {
  return scene_body(t, duration, canvas, layers);
}
