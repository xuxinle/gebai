// 幕 1：尺度跳变段——光标仍在同一位置（由 carry 送到），但周围已是卡片级细节。
//
// ## 图层下标（json 顺序，改动时必须同步）
//   0 eyebrow1  1 title1
//   2 cardA  3 cardB  4 cardC          ← 三张空卡（容器）
//   5 txtA   6 txtB   7 txtC           ← 卡内文案
//   8 tickA  9 tickB 10 tickC          ← 勾选点（"光标碰过"的可见证据）
//  11 sweep1                           ← 底线
//  12 cursor1                          ← **必须是最后一个**（最上层）
//
// ## 为什么光标必须放最后（抽帧才发现）
// 图元按数组顺序叠，先列出的在下。原写法把 cursor1 放在第 3 位，
// 于是它被 cardA/B/C（第 4~6 位）**盖住**——整幕都看不见光标。
// 指标全绿（轨迹里光标在动、承接分 1.00），但画面里根本看不到手，
// 而"手是开关"正是本模板存在的唯一理由。只有抽帧能发现这类错。
//
// ## 双影的处理
// `carry_out` 从幕 0 的 2.70s 画到 3.40s，而本幕 3.00s 就开场了：
// 3.00–3.40 这段 carry 还在飞，本幕 cursor1 也在。
// 解法是让 cursor1 在 carry 抵达之后才现身、并让 carry 飞走之后才起步：
//   · cursorOn = seg(t, 0.45, 0.60)  → 成片 3.45s 起可见（carry 3.40s 抵达）
//   · 路径 0.52s 才起步              → 否则实测 frame 102 两光标距 17.8px（双影）
//   · 2.70s 起淡出                    → 否则实测帧 189–191 分离到 31.6px（双影）
//
// ## 因果语法：光标先到，勾选才亮
// 模板硬约束是"每一次界面反应都能追溯到角色的一个动作"。
// 所以每个 tick 的点亮时刻 = 光标抵达该处之后 +0.08s，绝不允许提前。
function shot(t, duration, canvas, layers) {
  // ## 每一项都必须赋值（实测踩过，代价最大的一条）
  // `new Array(n)` 的每一项是 **undefined**。只要有任意一项没被赋值，
  // 宿主就报 `layout 返回的第 N 项不是对象` 并**把整幕降级为纯 JSON**——
  // 症状是“动效全没了”，但脚本看起来完全正常；且日志里只说“第 13 项”，
  // 不会告诉你是哪个图层。所以先用 `mk({})` 把全部填成零增量，再逐个覆盖。
  const st = new Array(layers.length);
  for (let i = 0; i < layers.length; i++) st[i] = mk({});

  const e = expoOut(seg(t, 0.34, 0.86));
  st[0] = mk({ dy: (1 - e) * 12, alpha: e });

  const ti = expoOut(seg(t, 0.44, 1.04));
  st[1] = mk({ dy: (1 - ti) * 18, alpha: ti });

  // —— 三张卡：错峰浮起（与光标路径错开，避免"一起动"）——
  const c1 = expoOut(seg(t, 0.10, 0.58));
  st[2] = mk({ dy: (1 - c1) * 24, alpha: c1 });
  const c2 = expoOut(seg(t, 0.20, 0.68));
  st[3] = mk({ dy: (1 - c2) * 24, alpha: c2 });
  const c3 = expoOut(seg(t, 0.30, 0.78));
  st[4] = mk({ dy: (1 - c3) * 24, alpha: c3 });

  // —— 卡内文案：卡落定后才起（避免"框和字一起冒出来"）——
  const tx1 = expoOut(seg(t, 0.42, 0.90));
  st[5] = mk({ dy: (1 - tx1) * 8, alpha: tx1 });
  const tx2 = expoOut(seg(t, 0.52, 1.00));
  st[6] = mk({ dy: (1 - tx2) * 8, alpha: tx2 });
  const tx3 = expoOut(seg(t, 0.62, 1.10));
  st[7] = mk({ dy: (1 - tx3) * 8, alpha: tx3 });

  // —— 光标路径：起点 (119,219) → A(285,505) → B(785,505) → C(1285,505) ——
  const pA = expoInOut(seg(t, 0.52, 0.94));
  const pB = expoInOut(seg(t, 1.06, 1.48));
  const pC = expoInOut(seg(t, 1.60, 2.02));
  const cx = 119 + (285 - 119) * pA + (785 - 285) * pB + (1285 - 785) * pC;
  const cy = 219 + (505 - 219) * pA;
  const cursorOn = seg(t, 0.45, 0.60);
  // 点击挤压：每次抵达后 0–0.16s 内一个小压缩（因果的"按下"）
  const pressAt = (t0) => {
    const u = seg(t, t0, t0 + 0.16);
    return (u > 0 && u < 1) ? Math.sin(u * Math.PI) : 0;
  };
  const press = Math.max(pressAt(0.94), pressAt(1.48), pressAt(2.02));
  st[12] = mk({
    dx: cx - 119,
    dy: cy - 219,
    dw: 5 * press,
    dh: -5 * press,
    alpha: cursorOn * (1 - expoOut(seg(t, 2.70, 2.90))),
  });

  // —— 勾选：光标抵达之后才亮（+0.08s）——
  const kA = expoOut(seg(t, 1.02, 1.16));
  st[8] = mk({ scale: 0.4 + 0.6 * kA, alpha: kA });
  const kB = expoOut(seg(t, 1.56, 1.70));
  st[9] = mk({ scale: 0.4 + 0.6 * kB, alpha: kB });
  const kC = expoOut(seg(t, 2.10, 2.24));
  st[10] = mk({ scale: 0.4 + 0.6 * kC, alpha: kC });

  // —— 底线：逐段点亮（光标走过 → 线变长），落定后持续有光沿它流动 ——
  //
  // ## 为何要同时动 dx（抽帧才发现）
  // `dw` 只改宽度，**原点不动**。写 `dw: -1440*(1-k)` 会让线从 1440 缩到 0，
  // 但左端始终在 x=240 —— 于是它向右收缩，画面右下留一段孤线（实测）。
  // 要“从左往右长出来”，必须同时把左端往右推：dx = 240*(1-k)。
  const lineK = Math.max(kA * 0.34, kB * 0.67, kC);
  const flow = 0.55 + 0.45 * (0.5 + 0.5 * Math.sin((t - 2.24) * 3.14));
  const lineOn = (t > 2.24) ? flow : 1;
  /* sweep 探针：完全不给 delta */
  st[11] = mk({
    dx: 240 * (1 - lineK),
    dw: -1440 * (1 - lineK),
    alpha: lineK * lineOn,
  });

  return { states: st, shapes: [] };
}
