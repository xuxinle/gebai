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

// 06 · 脊椎串联 —— 一条主线产出多个产物，每个产物有自己的世界
//
// ## 承接方式：主线变形
// 活下来的是**那条线本身**。它每次都是"从同一个入口长出去"，
// 而不是"三个卡片各自淡入"（那是能力清单，不是脊椎）。
//
// ## 核心手法：morphRect
//   1. 主线出现，光标在框里打字（"从这段日志里找出根因"）
//   2. 主线**向上分叉**出第一条枝 → 结论卡在枝头长出来
//   3. 回到主线（枝保留）→ 第二条 → 第三条
//   4. 全部完成后，主线自己收缩成一句总结（脊椎收拢）
//
// ## 为什么"回到主线"必须可见
// 观众每次都要知道"我们又回到那条线上了"。这是本模板唯一的节奏来源——
// 三个产物如果各自独立出现，就变成了并列（那是模板 01）。
//
// ## 节拍（三枝首尾相接，总量比"一支一支来"短得多）
//   0.00–0.80  主线淡入
//   0.95–1.30  文字淡入
//   0.85–1.09  轮 1：主线脉冲（心跳）
//   1.09–1.53  枝 1 长成
//   1.33–1.93  卡 1 长出
//   2.15–2.39  轮 2：主线脉冲
//   2.39–2.83  枝 2
//   2.63–3.23  卡 2
//   3.45–3.69  轮 3：主线脉冲
//   3.69–4.13  枝 3
//   3.93–4.53  卡 3
//   4.55–5.40  落定 + 静止（rest 0.85s）
const TYPE = "从这段日志里找出根因";
// 与 film.json 保持一致：三卡 x = 100/700/1300（宽 520），
// 三枝在各自卡的中心 x = 360/960/1560，从主线顶边 y=780 向上到卡片底边 y=540
const BR_X = [360, 960, 1560];
const CARD_X = [100, 700, 1300];
const CARD_W = 520;
const CARD_CY = 420;      // 卡中心 y（300 + 240/2）
const BR_TOP = 540;       // 卡片底边
const BR_BOT = 780;       // 主线顶边

function scene_body(t, duration, canvas, layers) {
  const st = new Array(layers.length);

  // ── 主线：从中间向两侧展开（它"打开"的准备动作）──
  const sk = expoInOut(seg(t, 0.10, 0.80));
  // 主线：整体淡入即可。
  //
  // 踩过两次：① 用 dw 从 -1200 长到 0 想"从中心向两侧展开"，
  // 但 dw 是右向增长，必须配 dx = -dw/2；② 误给 dx = +dw/2，
  // 主线整个被推到画面右外侧，帧里根本看不见（而 alpha=1、逻辑全对）。
  // 主线是全片的脊椎，它"稳定在场"比任何入场花活都重要。
  st[0] = mk({ alpha: sk });

  // 文字：主线里的 prompt（一次给出，不做逐字打字——
  // 裁云的 LayerDelta 改不了 text，逐字需要 N 个图层）
  const txtK = expoOut(seg(t, 0.95, 1.30));
  // 文字不能逐帧改内容（LayerDelta 没有 text），
  // 所以打字用**光标推进**表示：文字一次给出，靠 alpha 从 0 渐显 + 光标右移
  st[1] = mk({ alpha: txtK * (1 - 0.30 * expoInOut(seg(t, 4.55, 5.05))) });

  // ── 三条枝 + 三张卡：**一次一枝，且每次都回到主线** ──
  //
  // ## 这一版为什么推翻重做（量测 + 看帧的结论）
  //
  // 先是把三枝改成"首尾相接连续生长"，静止从 69% 降到 50%——但抽帧一看：
  // 1.2s 时**三张卡已经全在**了，后面 2.6s 全在等。也就是说数字好看了，
  // 而分镜本身没变（还是一次性铺满）。
  //
  // 真问题在**设计层**，不是时长与速率：脊椎串联的精髓是
  // 「一条主线 → 一次分叉 → **回到主线** → 再分叉」的**往复**。
  // 三枝同时生长就等于把"串联"做成了"并列"（那是模板 01）。
  //
  // 所以改成严格的往复：每轮 = 主线脉冲(0.24s) → 枝长(0.44s) → 卡出(0.60s)。
  // 主线脉冲是每轮的**起点信号**，观众靠它知道"又回到那条线上了"。
  const ROUND = [0.85, 2.15, 3.45];     // 每轮起点
  const PULSE = 0.24, BR_GROW = 0.44, CARD_GROW = 0.60;

  // 主线脉冲（脊柱的"心跳"）：每轮起点给一次亮度与轻微横向拉伸
  let spinePulse = 0;
  for (let i = 0; i < 3; i++) {
    const t0 = ROUND[i];
    const k = seg(t, t0, t0 + PULSE);
    const local = (k > 0 && k < 1) ? Math.sin(k * Math.PI) : 0;
    if (local > spinePulse) spinePulse = local;
  }
  st[0] = mk({
    alpha: sk * (0.88 + 0.12 * spinePulse),
    dw: 16 * spinePulse,
    dx: -8 * spinePulse,
  });

  for (let i = 0; i < 3; i++) {
    const t0 = ROUND[i];
    const tBr = t0 + PULSE;
    const tCard = tBr + BR_GROW * 0.55;

    // 枝：0.44s 长成（5px 宽，生长中略亮）
    const bk = expoInOut(seg(t, tBr, tBr + BR_GROW));
    st[11 + i] = mk({
      dh: (BR_BOT - BR_TOP) * bk,
      dy: -(BR_BOT - BR_TOP) * bk,
      alpha: bk * (0.75 + 0.45 * (1 - bk)),
    });

    // 卡：从枝头顶端长出来（0.60s）；上一轮的卡保持在位（这是"累积"）
    // **裁云的 spring 在负 tau 上不为 0**（它按解析式算，负时间会给一个接近 1 的值）。
    // 直接 clamp(spring(t - tCard, ...)) 会让所有还没开始的卡一开始就是"已完成"——
    // 实测：1.0s 时三张卡已经全在，往复结构完全读不出来。
    // 所以必须自己用 seg 把"还没到"挡住。
    const ckRaw = spring(t - tCard, CARD_GROW, 12);
    const ck = (t < tCard) ? 0 : clamp(ckRaw, 0, 1);
    const base = 2 + i * 3;
    const appear = (1 - ck);
    const dxa = (BR_X[i] - (CARD_X[i] + CARD_W * 0.5)) * appear;
    const dya = (BR_TOP - CARD_CY) * appear;
    st[base] = mk({
      scale: 0.18 + 0.82 * ck,
      alpha: seg(t, tCard, tCard + 0.18),
      dx: dxa, dy: dya,
    });
    st[base + 1] = mk({
      scale: 0.18 + 0.82 * ck,
      alpha: seg(t, tCard + 0.10, tCard + 0.28),
      dx: dxa, dy: dya,
    });
    st[base + 2] = mk({
      scale: 0.18 + 0.82 * ck,
      alpha: seg(t, tCard + 0.20, tCard + 0.38),
      dx: dxa, dy: dya,
    });
  }

  return { states: st, shapes: [] };
}

function layout(t, duration, canvas, layers) {
  return scene_body(t, duration, canvas, layers);
}
