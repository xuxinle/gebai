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

// 07 · 在场操作 —— 角色身体驱动每一个界面反应
//
// ## 唯一语法：角色即指针
// **每一个**界面反应都必须能追溯到角色身体的一个动作。
// 不能出现"界面自己亮了"——那是本模板唯一不能犯的错。
//
// ## 本镜的三次操作（一次任务的三步）
//   第 1 跳 → 「训练任务 / 失败」闪一下（它看到了失败）
//   第 2 跳 → 「日志 / 1,240 条」亮起（它去读了日志）
//   第 3 跳 → 「结论 / 待生成」变成「显存分配失败」（它给出了结论）
//
// ## 成对反应（实测）
// 落 = 角色压扁（dw 鼓 / dh 缩，体积守恒）+ 目标亮起，**两者必须同时**。
// 只让目标亮、角色不压，会读成"界面自己动的"。
//
// ## 节拍
//   0.0–0.8   三面板入场 + 角色落在中间
//   0.8–1.6   第 1 跳（左）
//   1.6–2.6   第 2 跳（中）
//   2.6–3.8   第 3 跳（右）+ 结论改写
//   3.8–5.6   静止（rest）
const HOME_X = 960;                 // hero 中心（json x=937 + 23，落在中间面板下方）
const LAND_X = [430, 960, 1490];    // 三个面板中心
const LAND_Y = 520;   // 角色落在面板内部，这样它推动面板的因果才读得出
const T_JUMP = [0.85, 1.65, 2.65];

function scene_body(t, duration, canvas, layers) {
  const st = new Array(layers.length);

  // 标题
  const tp = expoOut(seg(t, 0.00, 0.50));
  st[0] = mk({ dx: (1 - tp) * -26, alpha: tp });

  // 三个面板：同时入场（它们是"场景"，不是"反应"）
  for (let i = 0; i < 3; i++) {
    const k = expoOut(seg(t, 0.10 + i * 0.08, 0.66 + i * 0.08));
    st[1 + i * 3] = mk({ alpha: k, dy: (1 - k) * 24 });
    st[2 + i * 3] = mk({ alpha: k * 0.9, dy: (1 - k) * 24 });
    st[3 + i * 3] = mk({ alpha: k * 0.9, dy: (1 - k) * 24 });
  }

  // ── 角色：在三个落点之间跳（每跳 0.34s 抛物线，落地后停 0.5s）──
  let hx = HOME_X, hy = LAND_Y;  // hero 中心（json 已摆在落点高度）
  let squash = 0;               // 落地压缩 0..1
  for (let i = 0; i < 3; i++) {
    const t0 = T_JUMP[i];
    const prevX = (i === 0) ? HOME_X : LAND_X[i - 1];
    const prevY = LAND_Y;
    if (t >= t0 && t < t0 + 0.34) {
      // 跳跃中：横向缓动 + 纵向抛物线
      const k = (t - t0) / 0.34;
      hx = prevX + (LAND_X[i] - prevX) * expoInOut(k);
      // 抛物线：最高抬 90px
      hy = prevY + (LAND_Y - prevY) * k - Math.sin(k * Math.PI) * 90;
    } else if (t >= t0 + 0.34) {
      hx = LAND_X[i];
      hy = LAND_Y;
      // 落地压缩：0.16s 内一个脉冲
      const s = seg(t, t0 + 0.34, t0 + 0.50);
      const local = 1 - Math.abs(s - 0.5) * 2;
      if (local > squash) squash = local;
    }
  }

  // hero（json 中心 891,683）
  // 成对压缩：dw 鼓 18%（向右长）、dh 缩 18%（向下压），
  // 同时 dx 补半个鼓出量 → 视觉上"原地被压扁"而不是"往右下长大"。
  // （LayerDelta 的 dw/dh 都是右下单向，所以对中必须自己补一半）
  const bulge = 46 * 0.18 * squash;
  st[13] = mk({
    dx: (hx - HOME_X) - bulge * 0.5,
    dy: hy - LAND_Y,
    dw: bulge,
    dh: -bulge,
    scale: 1,
    alpha: expoOut(seg(t, 0.30, 0.70)),
  });
  // shadow（json 中心 891,739）：跟着角色横移，落地时变大变实
  st[12] = mk({
    dx: hx - HOME_X,
    dy: 0,
    dw: 70 * 0.5 * squash,
    scale: 1 + 0.25 * squash,
    alpha: expoOut(seg(t, 0.30, 0.70)) * (0.35 + 0.45 * squash),
  });

  // ── 三个落点标记：角色落上去才亮（成对）──
  for (let i = 0; i < 3; i++) {
    const tLand = T_JUMP[i] + 0.34;
    const k = seg(t, tLand, tLand + 0.10) * (1 - 0.55 * expoInOut(seg(t, tLand + 0.30, tLand + 0.70)));
    st[14 + i] = mk({ alpha: k, dw: 200 * 0.3 * (1 - k) });
  }

  // ── 第 3 跳的额外反应：结论面板的值变了 ──
  // LayerDelta 改不了 text，所以用"待生成"淡出 + 结论淡入（两层，json 只给了 p2v）
  // → 这里让 p2v 在结论生成后**换一种读法**：先淡到 0.25 再回到 1（一次强调脉冲）
  // 真实改文需要两个图层，本示例只演示"成对反馈"的时序
  const tLand3 = T_JUMP[2] + 0.34;
  const pulse = 1 - Math.abs(seg(t, tLand3, tLand3 + 0.60) * 2 - 1);
  st[11] = mk({ alpha: 0.55 + 0.45 * pulse });

  return { states: st, shapes: [] };
}

function layout(t, duration, canvas, layers) {
  return scene_body(t, duration, canvas, layers);
}
