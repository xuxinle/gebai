# 场景配方（recipes）

> 每条配方 = **一个可直接套用的段落**，不是完整片子。
> 把配方粘进模板（`templates/tpl-*.js`）的对应位置，改 `PARAMS` 即可。
>
> 所有配方都遵守同一组硬约束（这些不是建议，是实测换来的）：
> 1. 不留关键帧平台（两端同值 = 画面站定）
> 2. 任何"停留"都用持续位移代替（幅度 ≥ 20px，否则 60px 的格里测不到）
> 3. 背景点阵放在相机变换**之前**（屏幕空间固定 = 推镜时纹丝不动）
> 4. 文字用图层，且背景用 `skipBands` 让开
> 5. 不用 `Math.random`（用 `hash01`），不用 `Date.now`

---

## R1 产品界面演示（终端 / 编辑器 / 应用窗口）

**用途**：让观众看清一个界面的结构，并跟着"操作"走。
**对应 case**：pith（终端）、skill-demo（工具流）、motion-web。

### 结构

```
外层窗口框（圆角矩形）→ 标题栏（三点 + 标题）→ 内容区（逐行）
内容区的每一行：入场(错峰) → 存活期(呼吸) → 按需高亮
```

### 关键参数

```js
const PARAMS = {
  winW: 1180, winH: 640,
  chromeH: 46,          // 标题栏高度
  lineH: 34,            // 行高
  fontH: 22,            // 字号（shapes 的 kind=3 用 h 表示）
  cps: 28,              // 打字速度（字符/秒）
  lineStagger: 0.11,    // 行间错峰
};
```

### 实现要点

```js
// 窗口框：先画，作为所有内容的底
shapesOut.push(mkRect(winX, winY, PARAMS.winW, PARAMS.winH, "#141416", 0.97));
shapesOut.push(mkRect(winX, winY, PARAMS.winW, PARAMS.chromeH, "#1E1E22", 1));

// 三个窗口点（装饰，但也是"这一帧还在"的视觉锚）
for (let d = 0; d < 3; d++) {
  shapesOut.push(mkCircle(winX + 26 + d * 22, winY + PARAMS.chromeH / 2, 6,
                          d === 0 ? "#E0605A" : (d === 1 ? "#E0B45A" : "#5AC07A"), 0.9));
}

// 内容行：错峰入场 + 打字
for (let i = 0; i < LINES.length; i++) {
  const at = 1.2 + i * PARAMS.lineStagger + hash01(i * 977) * 0.05;
  const k = ease.expoOut(seg(t, at, at + 0.45));
  if (k <= 0) continue;
  const ly = winY + PARAMS.chromeH + 40 + i * PARAMS.lineH;
  // 打字：每行自己的进度
  const shown = typeOn(LINES[i], t, at, PARAMS.cps);
  if (shown.length > 0) {
    shapesOut.push(mkText(winX + 32, ly + (1 - k) * 30, PARAMS.fontH,
                          shown, "#C8C4BA", 0.95 * k));
  }
}
```

### 这个场景最容易错的两处

1. **窗口内一片空白**：终端窗口如果只有上方几行文字、下方全空，那几秒的
   格均值几乎为 0（实测 pith 的 1–5 秒 `mean=0.00`）。
   → 让内容**填满**内容区，或让窗口本身持续缩放/平移。
2. **光标不动**：`zoom=440` 时一个光标宽度就有 1320px，它不动 = 整屏不动。
   → 光标进场要滑动，并且带一点持续起伏（`breathe`）。

---

## R2 数据 / 指标呈现（大数字、进度、时间轴）

**用途**：把一组数说清楚，且不让观众觉得"幻灯片"。
**对应 case**：clearing（日历热力）、ebb（计时器）、infra（阵列规模）。

### 结构

```
主指标（超大字号，数字滚动）→ 辅助说明（小字）→ 背景刻度/网格（持续流动）
```

### 参数

```js
const PARAMS = {
  numH: 220,            // 主数字高度（px）
  rollDur: 1.2,         // 数字滚动时长
  labelH: 30,
  tickCount: 12,        // 背景刻度条数
  tickSpeed: 26,        // 刻度流动速度（px/s）——必须有，否则背景是死的
};
```

### 实现要点

```js
// 主数字：**滚动**而不是直接换（换数只有一帧在变，剩下的帧是静止的）
const target = 2184;
const shownNum = Math.round(target * ease.expoOut(seg(t, 1.0, 1.0 + PARAMS.rollDur)));
shapesOut.push(mkText(CX, CY - PARAMS.numH / 2, PARAMS.numH,
                      String(shownNum), "#EDEAE3", 0.98));

// 背景刻度：**持续流动**。这是这条配方里唯一保证"不静止"的东西。
for (let i = 0; i < PARAMS.tickCount; i++) {
  const y = ((i * 90 + t * PARAMS.tickSpeed) % (H + 90)) - 45;
  shapesOut.push(mkRect(0, y, W, 1, "#2A2F36", 0.5));
}
```

### 踩过的坑

- **数字滚完就静止**：滚动结束后那几秒没有任何变化。
  → 让刻度/网格一直流（上面那段），或让主数字有极小的持续缩放（1.00→1.03）。
- **改参数没效果要先算映射层数**：clearing 里我把 `dayH` 26→58，
  以为会放大，实际经过 `zoomIn=(dayH-24)/1536` 归一化后只从 1.00 到 1.05。
  → 改参数前先读它被谁用、经过几层换算。

---

## R3 图标 / 小元素的错峰入场与循环

**用途**：展示一组能力的"涌现感"。
**对应 case**：overlap（网点）、domino（骨牌）、clawd（图标阵）。

### 结构

```
一组同构元素 → 按"到中心的距离"排波入场 → 各自持续呼吸 → 按需点亮/变色
```

### 参数

```js
const PARAMS = {
  cols: 4, rows: 3,
  gapX: 260, gapY: 200,
  enterDur: 0.55,
  waveSpan: 0.9,        // 从中心到边缘的总错峰时长
  bobAmp: 18,           // 存活期呼吸幅度（**至少 15px**）
  bobHz: 1.1,
};
```

### 实现要点

```js
for (let i = 0; i < N; i++) {
  const col = i % PARAMS.cols, row = Math.floor(i / PARAMS.cols);
  const bx = x0 + col * PARAMS.gapX;
  const by = y0 + row * PARAMS.gapY;

  // 按距离排波（不是按索引等距——等距读作"表格刷新"）
  const dx = bx - CX, dy = by - CY;
  const dist = Math.sqrt(dx * dx + dy * dy) / Math.sqrt(CX * CX + CY * CY);
  const at = 1.0 + dist * PARAMS.waveSpan + hash01(i * 131) * 0.1;
  const k = ease.expoOut(seg(t, at, at + PARAMS.enterDur));

  // 呼吸相位按 i 打散，避免整屏同步起伏
  const bob = Math.sin(t * Math.PI * 2 * PARAMS.bobHz + i * 0.7) * PARAMS.bobAmp * k;
  // ...画元素
}
```

---

## R4 收尾字标（所有片子都要用）

**用途**：品牌/名字落定。
**纪律**：落定后**不再完全静止**——用极慢推镜代替定格。

```js
// 落定：从下方升起 + 淡入
const inK = ease.expoOut(seg(t, T_LAND, T_LAND + 0.7));
out[L_NAME] = { alpha: 0.98 * inK, dy: (1 - inK) * 60 };

// 落定之后**继续推镜**（不是定格）
// 实测：结尾 1.6 秒不动 → 静止占比多 10pp
const cam = camera({
  keys: [[T_LAND, 1.06], [DURATION, 1.26]],   // 两端必须不同值
  creep: 1,
});
```

**反例**（我最初写的，踩了）：
```js
keys: [[12.6, 1.06], [13.4, 1.05], [15.0, 1.10]]  // 13.4→15.0 只动 5%，测不到
```

---

## R5 转场（硬切 / 遮罩揭示）

**用途**：两段之间的切换。
**纪律**：**硬切要卡在拍上**（`bpm` 给定时间隔 = 60/bpm），且全片硬切 ≤ 3 处，否则读作闪烁。

```js
// 硬切：在某时刻整屏换内容（一帧到位，不做渐变）
const CUTS = [4.0, 8.0, 11.0];   // 必须落在拍上
let segIdx = 0;
for (let i = 0; i < CUTS.length; i++) if (t >= CUTS[i]) segIdx = i + 1;
// 按 segIdx 取对应的数据/配色
```

**不要**用渐变模拟转场（`alpha` 交叉淡入）来做"换段"——那会让中段的格均值
掉到接近 0（两段都不透明），实测反而是静止的来源。

---

## R6 「整屏都在动」的唯一可靠做法：让底纹自己流

> 这是本轮最贵的一条（模板因此从 94% 静止做到 2.2%）。

### 为什么相机微动不够

```
评测网格一格 60×60px，静止阈值 1.2（0~255）
相机逐帧位移 s_f，参照物直径 d：
    Δ ≈ 2 · d · s_f / 3600
```

相机 creep 的逐帧位移只有 **3px** 量级（110px 幅度摊在 1/30s 上），
取 d=18 → Δ ≈ 0.03。**远低于 1.2**，所以"相机明明在动，指标说全静止"。

### 三档实测（同一个模板，只改这一处）

| 做法 | 静止占比 | 变化密度 |
|---|---|---|
| 只有相机 creep | **94.4%** | 1.67/s |
| 点做大做亮（d=26, step=60） | 0.0% 但像**波点壁纸** | 29.9/s |
| **底纹自身 flow=780 px/s** | **2.2%** | 29.3/s |
| 卡片占屏多的场景 flow=1500 | **34.4%** | 19.6/s |

### 实现（`lattice` 里已内置）

```js
const fx = (t * flow) % step;          // 每帧平移 flow/30 px
const fy = (t * flow * 0.37) % step;   // 两个方向不同速，避免对角整齐
shapes.push({ kind: 0, x: gx + fx, y: gy + fy, ... });
```

`flow=780` 时逐帧位移 **26px**，每帧都有格子被扫过边界 → Δ 上到 1.2 以上。

### 三个配套注意

1. **`step` 不能与评测格 60px 成简单倍频**：用 84 时与 60 拍频，
   静止 94.5%；改 76 后 61.6%。**别用 60 的整数倍**。
2. **主体占屏越多，flow 要越大**（模板 B 的卡片盖住大半屏 → 1500）。
3. **必须传 `t`**：`lattice({...}, t)`。忘了传时 `t=undefined` → `NaN` 坐标
   → **整个形状不画**（渲染器不报错，看起来像"背景没生成"）。
   kit 里已兜底成 0，但那会让流动失效。

### 代价（诚实说）

底纹流动是**为可测性付的美术代价**。它观感上是"背景在缓慢走"，
在极静的收尾镜头里可能显得不安分。若某几秒确实要完全静（品牌定格），
显式 `flow: 0` —— 那时静止是设计意图。
