// 幕 0：节点被撞亮 → 火花（动量块）从节点弹出去，飞向画面右侧
//
// ## 本模板最硬的一条
// **撞击发生之前，被撞的东西必须严格为零**（不可见 / 完全静止 / 无预备动作）。
// 一旦给「提前预备」的动作，因果感立刻消失——观众读成"它自己动的"。
// 所以 lit0（亮面）在撞击时刻之前 alpha 恒为 0，火花在撞击前也不存在。
//
// ## 承接关键（两处必须对齐）
//   ① 本幕末帧 spark 的实景 = (380,606,56,56) = from { x:19.792%, y:56.111%, … }
//   ② carry 会从这一刻开始把它送到 next 幕的 (1100,460)
// 因此 spark **不能自己动**（它的运动全部由 carry 负责）。
// 唯一允许的是 alpha：进入 carry 前必须仍可见（carry_qa 会跳过 alpha<0.15 的元素）。
//
// ## 承接物的手交（避免双影）
// 下一幕也有一个同名图层 `spark`。若它在 carry 结束前就可见，
// 画面里会出现**两个火花**（carry 画一个、图层画一个）。
// 所以下幕的 spark 由下幕脚本在 carry 结束之后再点亮（见 ms1.js）。
function shot(t, duration, canvas, layers) {
  const st = new Array(layers.length);
  // 0 eyebrow0  1 title0  2 node0  3 lit0  4 cap0  5 spark

  const e = expoOut(seg(t, 0.15, 0.70));
  st[0] = mk({ dy: (1 - e) * 14, alpha: e });

  const ti = expoOut(seg(t, 0.30, 1.00));
  st[1] = mk({ dy: (1 - ti) * 20, alpha: ti });

  // 节点：先在场（空壳），撞击时刻 1.20s
  const nk = expoOut(seg(t, 0.55, 1.10));
  st[2] = mk({ dy: (1 - nk) * 12, alpha: nk });

  // 撞击 1.20s：亮面从 0 起，且**之前严格为零**（因果感的全部）
  const hit = clamp(spring(t - 1.20, 0.40, 16), 0, 1);
  // 落定后节点缓慢呼吸（周期 1.7s）——让"已通电"的节点在等待时仍活着
  const breath = 0.55 + 0.45 * (0.5 + 0.5 * Math.sin((t - 1.65) * 3.70));
  const settled = (t > 1.65);
  st[3] = mk({
    alpha: hit * (settled ? breath : 1),
    scale: 1 + 0.02 * clamp(spring(t - 1.20, 0.30, 18), 0, 1),
  });

  const ck = expoOut(seg(t, 0.80, 1.35));
  st[4] = mk({ dy: (1 - ck) * 10, alpha: ck });

  // 火花：撞击后 0.12s 才出现（**不得早于撞击**），出现后**完全不动**
  // （它的位移由 carry 负责；自己动会让 from 与实际位置不符 → 承接断掉）。
  //
  // ## carry 起点必须让位（实测踩到双影）
  // `carry_out.at = 2.7`：从帧 81 起 carry 自己开始画一个火花。
  // 而本图层当时仍然亮着（实测 frame 40..89 的 x 恒为 380，alpha 1.00）——
  // 两者重叠 9 帧 = **双影**。所以 2.60s 开始淡出，2.70s 前归零。
  const sparkOn = seg(t, 1.32, 1.50);
  const handoff = 1 - seg(t, 2.58, 2.70);
  st[5] = mk({ alpha: (sparkOn > 0 ? 1 : 0) * handoff });

  // 轨道线（layer 6 `rail0`）：从节点向右长出去——
  // 这是“动量将往哪个方向去”的预告，也是填补右侧空白的主体。
  // 它**在撞击之后**才长（因果：先撞，才有方向）。
  const railK = expoInOut(seg(t, 1.35, 2.30));
  st[6] = mk({ dw: -520 + 520 * railK, alpha: railK * 0.85 });

  return { states: st, shapes: [] };
}
