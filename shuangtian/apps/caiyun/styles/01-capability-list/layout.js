// 风格模板 01 · 能力清单 —— 一镜 5.4s
//
// ════════════════════════════════════════════════════════════════════
// ## 这个模板的读法
//
// 观众已经认同品类，需要被「它真的有用」说服。所以这一镜不叙事，
// **报能力**：四项并列，每项一次独立动效（勾选），最后给一个判定。
//
// ## 承接方式（模板 01 的判据）
//
// 手是开关：手走到某一行 → 那一行亮 → 打勾。因果是单向可见的。
// 边界上活下来的是**那只手**（以及它留下的勾）。
//
// ## 节拍
//
//   0.00–0.70  标题 / 副标 滑入
//   0.70–1.20  分隔线横向展开
//   1.20–3.40  四行：每行 0.55s 一轮（手到位 0.22 + 行亮 0.12 + 勾弹 0.18）
//              ← 这里的 0.55s 间隔是「密集小项」，故意快
//   3.36–4.06  判定块出现（蓄力后的一次落定）
//   4.10–5.40  **完全静止** 1.3s（本模板必须有 rest，否则动作落不下来）
//
// ════════════════════════════════════════════════════════════════════
// ## ⚠ 裁云脚本的可用名字（写错就整份脚本静默降级）
//
// 宿主注入的只有：`ease(name, t)` `spring(tau[,zeta,omega])` `spring_vel`
// `settle` `lerp(a,b,k)` `clamp(v,lo,hi)` `snap(t,fps)`，再加 `noise`。
// **没有 `seg`，没有 `E.*`，没有 `D()`**。
//
// 本文件早先照搬了别的工程的写法（`E.expoOut(seg(...))` / `D({...})`），
// 于是运行期抛 ReferenceError → 裁云静默降级为纯 JSON → 画面停在初始帧、
// **零报错**。为此白渲了三轮才定位。所以这里自带 `seg` / `expoOut` / `mk`。
//
// ## 图层下标（与 film.json 严格同序）
// 0=title 1=sub 2=rule 3..6=四行 7..10=四个勾 11=verdict 12=verdict2
// 13=hand 14=handHalo
// ════════════════════════════════════════════════════════════════════

// ── 本地小工具（只依赖宿主函数，不依赖 kit 的拼接顺序）──────────
// seg：把区间映射到 0..1（宿主没有这个函数）
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
// mk：把增量补成裁云要的完整形状（alpha 缺省 1，其余 0）
// LayerDelta 字段：dx dy dw dh scale rotate alpha tint
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

const TICK_Y = [371, 467, 563, 659];   // 四个勾的**中心** y（json y+11）
const HAND_START_Y = 520;
// 手停在勾的**右侧**一段距离（不能压到勾上：踩过，手圆盖住了第三个勾）
const HAND_X = 1075;
const T_ROW0 = 1.20;
const ROW_GAP = 0.55;

function scene_body(t, duration, canvas, layers) {
  const st = new Array(layers.length);

  // ── 标题 / 副标：从左侧滑入 ──────────────────────────────
  const tp = expoOut(seg(t, 0.00, 0.56));
  st[0] = mk({ dx: (1 - tp) * -34, alpha: tp });
  const sp = expoOut(seg(t, 0.10, 0.66));
  st[1] = mk({ dx: (1 - sp) * -26, alpha: sp * 0.92 });

  // ── 分隔线：横向展开（dw 0 → 800）────────────────────────
  const ruleK = expoInOut(seg(t, 0.70, 1.18));
  st[2] = mk({ dw: ruleK * 800, alpha: ruleK });

  // ── 四行 + 四勾：逐行「手到位 → 行亮 → 勾弹」──────────────
  for (let i = 0; i < 4; i++) {
    const t0 = T_ROW0 + i * ROW_GAP;
    const rowIn = expoOut(seg(t, t0 - 0.10, t0 + 0.26));
    const rowBase = seg(t, 0.86, 1.16);
    st[3 + i] = mk({
      alpha: (0.28 + 0.72 * rowIn) * rowBase,
      dx: (1 - rowIn) * -10,
    });
    // 勾：手到位后 0.12s 才弹（因果顺序不能反）
    const tickK = clamp(spring(t - (t0 + 0.12), 0.46, 16), 0, 1);
    const tickP = seg(t, t0 + 0.10, t0 + 0.17);
    st[7 + i] = mk({
      alpha: tickP,
      // dw/dh 从 -22 长到 0：勾「从左下长出来」，不是淡入
      dw: -(1 - tickK) * 22,
      dh: -(1 - tickK) * 22,
      dx: (1 - tickK) * -11,
      dy: (1 - tickK) * 11,
    });
  }

  // ── 判定块：四行都点完（≈3.30）才出现 ─────────────────────
  const vk = expoOut(seg(t, 3.36, 3.90));
  st[11] = mk({ alpha: vk, dy: (1 - vk) * 18 });
  const v2 = expoOut(seg(t, 3.54, 4.04));
  st[12] = mk({ alpha: v2 * 0.92, dy: (1 - v2) * 12 });

  // ── 手：沿四行向下走，每行停一下（停才是「按」）──────────
  let hy = HAND_START_Y;
  let press = 0;
  for (let i = 0; i < 4; i++) {
    const t0 = T_ROW0 + i * ROW_GAP;
    const moveK = expoInOut(seg(t, t0 - 0.26, t0 - 0.02));
    const fromY = (i === 0) ? HAND_START_Y : TICK_Y[i - 1];
    hy = fromY + (TICK_Y[i] - fromY) * moveK;
    const p = seg(t, t0 - 0.02, t0 + 0.12);
    const local = 1 - Math.abs(p - 0.5) * 2;
    if (local > press) press = local;
  }
  // 收尾：手向右上退出
  if (t >= 3.30) {
    const exitK = expoInOut(seg(t, 3.30, 3.96));
    hy = TICK_Y[3] - exitK * 260;
    press = 0;
  }
  const handAlpha = seg(t, 0.55, 0.85) * (1 - expoOut(seg(t, 3.60, 4.06)));

  // json：hand 在 (300,470) 尺寸 26 → 中心 (313,483)
  st[13] = mk({
    dx: HAND_X - 13 - 300,
    dy: hy - 13 - 470,
    scale: 1 - 0.20 * press,
    alpha: handAlpha,
  });
  // 光晕：json 在 (288,458) 尺寸 50 → 中心 (313,483)，与 hand 同心
  st[14] = mk({
    dx: HAND_X - 25 - 288,
    dy: hy - 25 - 458,
    scale: 1 + 0.55 * press,
    alpha: handAlpha * (0.14 + 0.30 * press),
  });

  return { states: st, shapes: [] };
}

// ── 裁云要求的固定入口（名字必须是 layout）──────────────────
function layout(t, duration, canvas, layers) {
  return scene_body(t, duration, canvas, layers);
}
