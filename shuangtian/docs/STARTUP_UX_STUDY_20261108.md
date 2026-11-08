# 启动体验调研：窗口创建 / 首帧前空白 / 启动动画（2026-11-08）

> 目标（用户原话）：**提升窗口创建速度 + 加启动动画，优化首帧绘制前的体验**。
> 本文只做调研：把现状量准、把可下手的地方按代价排开、把哪些地方"现在没有量具"标出来。
> **本机口径**：Windows / RTX 4080 SUPER / 显示器 3533×1921 @1.5（工作区逻辑 2355×1280）/ dev 档。
> 涉及的"首帧"数字都来自 `gallery`（它的窗口会被 `fill_work_area` 铺满工作区 ⇒ 画布 3533×1921，
> 是**最重的**那类应用）。小窗口应用（相期 690×930）会小得多，不要拿 gallery 的数当通用值。

---

## 0. 一页结论

| 结论 | 证据 |
|---|---|
| **用户真正空等的是"一块纯底色 380~470ms"**，不是窗口创建 | 墙钟：窗口可见 52~65ms → 内容出现 440~530ms（§1.1） |
| 窗口创建本身只有 **25~35ms**（含首次画布分配），不是瓶颈 | 同上；`window.create` 进程内 5.5~13ms（§1.2） |
| 首帧冷启动 **390ms**，而稳态同帧只要 **2.2ms** ⇒ 是**一次性建表**，不是每帧成本 | `paint` 冷 `cold_total 389ms` vs `--bench p50 2.2ms`（§1.3） |
| 首帧这 390ms 里，**约 230ms 是软件光栅路径专有**（GPU 显式路径同场景只有 134~168ms） | 显式 `--renderer gpu` 的 `first_frame 134~168ms`（§1.3） |
| 已落地的 `defer_gpu`（先软件后 GPU）**买到的不是"更快看见内容"，而是"65ms 就有底色"**——方向对，但底色之后完全是静止的 | 显式 gpu：`window.create 390~458ms` ⇒ 窗口要到 ~457ms wall 才出现（§1.4） |
| **发现一个真缺陷：启动底色硬编码亮色 `0xF7F7FA`**，暗色主题启动会白闪一下 | `chrome_edge_rgb_{0xF7F7FAU}`（`platform_win32.cpp:2733`），实测 dark 主题下采样到 `F7F7FA`（§2.1） |
| **发现一处白干：启动期画布分配 3 次**（1920×1080 → 3533×1921 → 3533×1921，合计 ~62MB 内存写） | 日志 `缓冲区原地换` 出现两次 + `窗口已创建 物理 1920×1080`（§2.2） |
| **`show_early` 只有 win32 实现**，x11/wayland 仍是"首帧画完才映射窗口" ⇒ 启动动画的落点必须按平台分 | `platform_x11.cpp:787` / `platform_wayland.cpp:1251` 只有 `show_when_ready`（§2.3） |
| **量具缺口**：首帧的绘制分解现在拿不到（`ST_PAINT_PROFILE` 只在 `--bench` 分支打印稳态帧），首帧是"389ms 无分解" | `apps/gallery/main.cpp:373` + `src/perf/frame.cpp:177`（冷启动帧被排除出元素统计） |

**建议的动手顺序**（详细见 §4）：① 补首帧量具 → ② 三件低风险修正（底色跟主题 / 一次分配画布 / x11+wayland 补 `show_early`）→
③ 启动动画（GDI 层可中断的自绘，覆盖那 380ms）→ ④ 首帧瘦身（阴影/毛玻璃延后到第二帧）——④ 是省时间最大的一笔，但属观感类改动，要按 `CONVENTIONS.md` §9.2 的七条硬约束走。

---

## 1. 现状时间线（实测账本）

### 1.1 墙钟（父进程侧打表，含进程启动）—— 用户真正感受到的那条线

量法：`CreateProcess` 之前打 t0，每 3~8ms 轮询 `EnumWindows` 找 `ShuangtianWindow`
（跳过 `WS_EX_LAYERED` 的阴影层）+ `IsWindowVisible` + 对客户区中心 160×160 做一次
`BitBlt`+`GetDIBits` 统计颜色数（"出现内容" = 颜色数 ≥3 且主导色占比 <90%）。

| 事件 | 3 轮中位 | 备注 |
|---|---|---|
| 窗口对象存在（`EnumWindows` 找得到） | **25~35ms** | `CreateWindowExW` 已返回 |
| 窗口**可见**（`IsWindowVisible`） | **52~65ms** | = `show_early()` 那一下（填底色） |
| **内容出现**（采样到 ≥3 种颜色） | **440~530ms** | = 首帧 `present` |
| **纯底色停顿** | **380~470ms** | ← **启动动画要填的就是这一段** |

补充观察（同一批采样）：
- 底色是**一种颜色、占比 100%**（`F7F7FA`），期间没有任何像素变化——即"一块完全静止的浅色"。
- 首帧出现后立刻是 `6` 种颜色、主导色 `1A1515` 占 48%（= 深色文字+卡片）——**一步到位，没有中间态**。
- 这条链上**没有"转圈"**：建窗后主线程最长不回消息已由 `tools/startup_responsiveness_probe.py`
  的判据（<400ms）与 `app.cpp:1095-1115` 的多处 `pump_events()` 覆盖。（`startup_responsiveness_probe.py`
  的默认 exe 仍是 `build/debug/bin/gbcode.exe`，要量别处请传 `--exe`。）

### 1.2 进程内 `ST_PERF_STARTUP=1` 分段（`perf::startup_mark`）

| 场景 | `window.create` | `before_first_frame` | `first_frame` | 总 |
|---|---|---|---|---|
| win32 · scale 1.0 | 5.5~6.5ms | 22~24ms | **267~270ms** | 303ms |
| win32 · scale 1.25 | 6.2~7.1ms | — | **321~324ms** | — |
| win32 · scale 1.5（默认） | 7.0~8.6ms | 22~23ms | **390~402ms** | 420~425ms |
| win32 · scale 1.5 · `--renderer gpu`（不 defer） | **388~458ms** | 15~17ms | **134~168ms** | 541ms |
| headless · scale 1.5 | 2.6ms | 11ms | 148ms | 148ms |

三条读法：

1. **scale 1.0 → 1.5 多 120ms，与面积无关**：两种 scale 下画布物理尺寸都是 3533×1921
   （`fill_work_area` 铺满工作区），唯一差别是字形超采样 `supersample = max(1, lround(scale))`
   ⇒ 2 倍采样、约 4 倍光栅工作量。**首帧里字形光栅至少占 ~120ms 量级**。
2. **GPU 路径首帧只要 134~168ms**（同画布、同内容）。差出来的 ~230ms 是
   **软件光栅专有的一次性建表**（阴影遮罩、覆盖率遮罩、圆角/路径缓存）。
3. `window.create` 的 7~13ms 里含**首次画布分配**（见 §2.2），`CreateWindowExW` 本身远小于此。

### 1.3 首帧内部：`paint` 冷 389ms / 热 2.2ms

`--perf-frames 4` 的报告（`sections.phases[].cold_total_ms`）：

| 阶段 | 冷启动那一帧 | 稳态 |
|---|---|---|
| layout | 1.4ms | 0.26ms |
| **paint** | **389ms** | 2.2ms（p50） |
| present | 5.5ms | 0.5ms |

`--bench 20` 的绘制分解（**稳态帧**）：`text 0.60ms / clip_mask(PaintOp) / shadow 0.01ms / clear 0.002ms`
——即**稳态一个原语都不贵**，389ms 全是"第一次"。

历史逐层量准过一次（`docs/BACKLOG.md:498-525`，"冷启动首帧：阴影占 62%"）：阴影 66ms（其中两张卡片
遮罩构建 29.7+22.7ms）、字形 23ms（150 字形）、clear 0.4ms、layout 1.2ms；`ST_DBG_NO_SHADOW=1` 后 106.6→40.2ms。
**那份数据是 1280×800 逻辑 @1.5x ⇒ 1920×1200 物理 ≈2.3MP，现在 gallery 是 3533×1921（6.8MP，2.95 倍面积）**，
面积相关项（阴影遮罩、clear、圆角填充）按比例放大后落在 300ms 量级，与 389ms 同量级——但**不是相等**，
所以只能当"量级自洽"，不能当结论。
> ⚠ **这是换算，不是本次重测**。当年的 `ST_DBG_NO_SHADOW` 已随代码删除（grep 无匹配），
> 现在没有"跳过阴影"的开关 ⇒ 这条必须靠新量具重测（§3）。

### 1.4 `defer_gpu` 到底买到了什么

`WindowOptions::defer_gpu` 默认 true（`shell.hpp:63`、`app.hpp:228`）：窗口先用**软件画布**开出来，
后台线程建 D3D11 设备 + 预热着色器（实测 **410.6ms**），主线程在帧边界换成 GPU 画布（实测 **4.6ms**）。

| 策略 | 窗口可见（wall） | 内容出现（wall） | 进程内总 |
|---|---|---|---|
| `defer_gpu=true`（默认） | **52~65ms** | 440~530ms | 424ms |
| 显式 `--renderer gpu` | ~457ms（`window.create` 388~458ms） | *未直接采样*（进程内 388+15+134~458+17+168 ⇒ 推算 540~640ms） | 541ms |

**结论**：`defer_gpu` 是对的（先给反馈），但它优化的是"**看见窗口**"，不是"**看见内容**"；
首帧仍然被钉在慢的软件路径上（390ms vs GPU 160ms），而 GPU 恰好在这段时间里就绪
（日志：建窗 27.394 → 上屏 27.437 → GPU 就绪 27.805 → 升级 27.846 → 首帧结束 ~27.83x）。

---

## 2. 已定位的具体问题

### 2.1 ✅ 缺陷：启动底色硬编码亮色（暗色主题会白闪）

```cpp
// src/shell/platform_win32.cpp:2733
std::uint32_t chrome_edge_rgb_{0xF7F7FAU};   // 硬编码，从未被主题改过
```

`show_early()`（`platform_win32.cpp:860`）与 `blit()` 的空画布分支（`:1897`）都用它填底色。
**实测 `--theme dark` 与 `--theme light` 的采样结果都是 `F7F7FA`**（浅色）。即：暗色主题用户
启动时会看到一块浅色底，~380ms 后突然变成深色界面。

- **修法**：给 `Backend` 加一个"窗口底色"入口（`set_window_background(Color)`），由 `app.cpp`
  在主题合成之后、`show_early()` 之前调用；`headless` 空实现。
- **代价**：极小（一次 `CreateSolidBrush`）。**验收**：暗色主题下采样底色 = 主题 `colors().bg`。
- 顺带：`show_early()` 里每次 `CreateSolidBrush`+`DeleteObject`，可缓存。

### 2.2 ⚠️ 白干：启动期画布被分配 3 次

日志（一次默认启动，时间戳原样）：

```
27.394  win32 窗口已创建（逻辑 1280x720 · 缩放 1.50 · 物理 1920x1080）   ← 分配 #1：1920×1080 (8MB)
27.407  缓冲区原地换：3533x1921（缩放未变 1.50）                          ← #2：fit_work_area 之后
27.437  上屏：画布尚无内容…（show_early）
27.440  缓冲区原地换：3533x1921（缩放未变 1.50）                          ← #3：center/尺寸二次同步
```

成因链条：`create_window` 先按**请求尺寸**（1280×720×1.5 = 1920×1080）分配画布
（`platform_win32.cpp:616 sync_buffers_to_client`）→ 回到 `app.cpp:1193` 才
`fit_window_to_work_area()`（因为"工作区尺寸只有建窗之后才拿得到"）→ 画布按客户区重分配
（3533×1921）→ 之后 `center_window`/`WM_SIZE` 再同步一次。

- **修法**：把 `fill_work_area` / 工作区铺开交给**后端在建窗时一次做完**
  （`WindowOptions` 加字段；后端本来就能拿到 `MonitorFromWindow`+`GetMonitorInfo`），
  `create_window` 里只分配一次画布；`app.cpp:1193` 那段退化成"后端不支持时兜底"。
- **收益**：省一次 8MB 分配 + 一次 27MB 的重建（这两笔都在"窗口可见"之前）；
  也少一次 `SetWindowPos`（会触发 `WM_SIZE` → 再一次同步）。
- **代价/风险**：改的是**启动期时序**，而那段有 5 处 `pump_events()` 与"建窗不显"的约定
  （`DESIGN.md` §8.3.1、`BACKLOG.md:6396`）。要保证：建窗仍然不 `ShowWindow`、DPI 仍然以窗口实际值为准。
  **验收**：`show_early` 之前的画布分配次数 = 1（可打点断言），且"启动黑框/白闪"两类历史缺陷不复发。

### 2.3 ⚠️ 跨平台不一致：`show_early` 只有 win32 实现

| 平台 | 提前显窗（出底色） | 首帧后显示 |
|---|---|---|
| win32 | ✅ `platform_win32.cpp:860` | ✅ `:742` |
| x11 | ❌ 默认空实现 | ✅ `platform_x11.cpp:787`（`map_window` 在首帧后） |
| wayland | ❌ 默认空实现 | ✅ `platform_wayland.cpp:1251`（"首次带内容的 commit 后出现"） |
| headless | 无需 | 无需 |

⇒ 若启动动画只在 win32 做，Linux 侧的用户体验会分叉。**"该平台能不能提前显窗"本身应成为后端能力**
（例如 `virtual bool can_show_early() const`），app 层据此决定"播动画"还是"直接等首帧"。

### 2.4 ⚠️ 首帧是一整块不可分割的工作（动画无处安放）

`app.cpp:1598-1619` 的启动帧序：

```
pump_events() → show_early() → pump_events() → render_frame() → show_when_ready() → render_frame()
```

`show_early()` 与 `render_frame()` 之间**只隔一次泵消息**，而 `render_frame()` 内部 389ms 不泵消息
（`app.cpp:1111-1114` 已写明"单靠边界泵消息不足以消除等待圈，首帧内部是一整块"）。
⇒ 想在等待期播动画，只有三条路：

1. **在 `show_early()` 与首帧之间插一段可中断的"等待循环"**（自绘动画 + 每帧 `pump_messages()`）；
   但这段最多覆盖到首帧开始，首帧那 389ms 仍然是静止的。
2. **在首帧内部插泵点**（把 `render_frame` 的 paint 分段，段间泵 + 画一帧动画）——最直接，但会把
   `paint` 的原子性打破，需要仔细评估"半帧画面"是否可接受（增量重绘/损坏区的账会乱）。
3. **做"廉价首帧"**：首帧只画必要内容（跳过阴影/毛玻璃/部分装饰），先 present 一次"像样但简"的画面，
   随后 1~2 帧补齐 → 用户 200ms 内看到接近最终的界面，动画与内容过渡天然融合。（推荐，见 §4-D）

### 2.5 ℹ️ 已解决、别再重查的

- **`auto` 渲染器选型**导致的启动抖动已删（现在是"有 GPU 就用"），`BACKLOG.md:2217-2251`。
- **D3D11 建设备 ~358~410ms** 的处置方向已经定了（`defer_gpu`），`BACKLOG.md:2201-2206`。
- **创建后黑框/黑屏**：已由"建窗不显 + `show_early` 填底色 + `canvas_painted_` 守卫"三层修过
  （`platform_win32.cpp:2736-2746`、`DESIGN.md` §8.3.1、git `d8a860cf`）。**再做启动动画时这三条都必须保住**：
  全零画布一旦被贴屏就是整窗纯黑（历史实测黑占 89%）。

---

## 3. 量具缺口（这是当前最该先补的东西）

| 想要的东西 | 现在能不能拿到 | 缺什么 |
|---|---|---|
| 进程内各阶段 | ✅ `ST_PERF_STARTUP=1` + `perf::startup_mark` | `gallery` 打了 `log_startup_summary`（`main.cpp:314`）；**相期/gbcode 等应用没有打**（实测 `--frames 2` 无输出）⇒ 小窗口应用的启动成本目前是黑箱 |
| 首帧三阶段（layout/paint/present） | ✅ `--perf-frames N` 的 `cold_total_ms` | 只有三段，答不了"389ms 花在哪个原语" |
| **首帧的原语分解**（阴影/字形/clear/圆角…） | ❌ | `ST_PAINT_PROFILE` 只在 `--bench` 分支打印末帧（`main.cpp:373`）；`FrameProfiler::record_element` 明确把冷启动帧排除（`frame.cpp:177`） |
| 墙钟"看见底色/看见内容" | ❌（本文临时写的探针，未入仓） | 建议把本次的父进程采样脚本固化成 `tools/startup_ux_probe.py`（§1.1 的量法） |
| 主线程最长无响应 | ✅ `tools/startup_responsiveness_probe.py` | 默认 exe 指向 `build/debug/bin/gbcode.exe`，需传参 |

**建议补的第一件量具**：`Application` 侧对**第 1 帧**也挂 `raster::PaintProfiler`，把首帧的
按原语分解暴露到 `metrics`（或一个新的控制通道项 `metrics.first_frame_paint`），
`gallery --frames 1` 直接打印。有了它，"阴影占多少 / 字形占多少 / clear 占多少"就是读数，而不是换算。

---

## 4. 优化候选（按"收益 ÷ 风险"排序，含代价与验收）

### A. 启动底色跟随主题（修 §2.1）—— **低风险，必做**

`Backend::set_window_background(ui::Color)`；`app.cpp` 在 `compose_theme` 之后、`show_early` 之前调；
`headless` 空实现；`x11/wayland` 可以设 WM 背景色（X11 `XSetWindowBackground`）。验收：暗色主题启动采样 = `bg`。

### B. 建窗一次到位（修 §2.2）—— **低风险，收益在"可见之前"**

把"铺开工作区"的决策下移给后端（`WindowOptions::fill_work_area`），`create_window` 内
"定尺寸 → 分配画布"只做一次；`app.cpp:1193` 保留为兜底分支（后端不支持时）。
预期：省两次画布重建（8MB + 27MB 写）+ 一次 `SetWindowPos`；`window.create` 7~13ms 里能砍掉一部分。
**风险点**：这一段是"黑框/白闪"历史缺陷的高发区，改动必须在真窗口上跑
`tools/startup_responsiveness_probe.py` + 截图对照。

### C. 启动动画（本需求的正题）—— **中风险，观感类改动**

三层可独立开关的动效，从便宜到贵：

| 层 | 做法 | 覆盖时段 | 代价 |
|---|---|---|---|
| C1 底色 + 状态 | `show_early` 之后自绘一个**呼吸圆点/进度条**（GDI 直接画进 DIB，与底色同一通道，不碰 UI 树、不碰字体） | 65ms → 首帧 | 中：需要一个"可中断等待循环"，每个 tick `pump_messages()` + `blit()`；**不引入额外线程** |
| C2 内容入场 | 首帧 `present` 后 `AnimateWindow(hwnd, 140, AW_BLEND)` 或 DWM 过渡；配合 C1 的收尾 | 首帧后 140ms | 低（一次系统调用），但要确认**不与"首帧后补一帧"的时序打架**（`app.cpp:1613-1619`） |
| C3 logo 形变/缩放入场 | 需要遮罩/缩放合成；主窗不能走 `WS_EX_LAYERED`（`BACKLOG.md:616-629` 实测每帧 +2.8ms，且把上屏通道从异步变同步） | 同上 | 高：要么改上屏通道，要么另开一个小 layered 窗做 splash（两个窗口 = 帧偏差，`BACKLOG.md:473-487` 的同一教训） |

**建议先做 C1 + C2**，C3 只在用户明确要"闪屏动效"时再评估。
对 C1 的两条硬约束（都来自历史实测）：
1. **不能出现"未画过的画布被贴屏"**（`canvas_painted_` 那套守卫要跟着动画路径一起走）；
2. **不能每帧整窗 BitBlt 27MB**：动画区域要小（例如居中 320×320），走 `present_region_` 局部上屏。

**观感纪律**（`CONVENTIONS.md` §9.2）：动画属"验收标准长在用户身上"的类别 ⇒
先给候选（有动画 / 只淡入 / 不动画）× 数据（首帧前空白时长、CPU 占用），由用户拍默认档。

### D. 首帧瘦身（收益最大，风险最高）—— **观感类**

方向：首帧**跳过阴影、毛玻璃、渐变**（只画底色 + 文字 + 实心块），立刻 present；随后 1~2 帧补齐。
依据：GPU 路径首帧 134~168ms vs 软件 390ms，差的那 230ms 正是这些"建表型"装饰；而它们**不承载信息**，
晚 2 帧（≈30ms）出现人眼基本无感。**注意**：这是观感判断，必须截图对照 + 用户验收，不能只看数字。
预期："看见内容" 440ms → 150~200ms（含进程启动段）。

### E. 字形预热（可并行的小项）

首帧里字形光栅 ~120ms 量级（scale 1.0 vs 1.5 的差）。可选：`show_early` 之后在后台线程
**预热首批字形**（把 shaper/rasterizer 的首次开销挪出主线程）。但 `BACKLOG.md:527-530` 已记录
"字体解析并行已否决（真窗口实测只差 20ms）"——**注意那条讲的是"解析字体文件"，不是"光栅字形"**，
两者不要混。这项能省的量目前**未量**（需要 §3 的量具）。

---

## 5. 建议的下一步（可直接开工的顺序）

1. **补量具**：首帧也挂 `PaintProfiler` + `gallery --frames 1` 打印（半天）；把 §1.1 的父进程采样固化为
   `tools/startup_ux_probe.py`（`--exe/--runs` 参数化，输出"可见/内容/纯底色停顿"三个数）。
2. **A + B 两件低风险修正**（各半天），跑 `startup_responsiveness_probe` + 一次真窗口截图对照。
3. **用新量具重测 §1.3**，给出首帧的原语分解（阴影 / 字形 / clear / 圆角 / 路径），**再决定 D 做不做、怎么做**。
4. **C1 + C2 出两个候选**给用户拍（"呼吸点" vs "只淡入"），产物为 GIF/短录屏 + 空白时长数据。
5. 以上做完再把结论回写 `DESIGN.md`（启动帧序那一节，§8.3.1）与 `docs/BACKLOG.md`（把本文的
   "待量"项替换成实测项），**不要只留本文**。

---

## 6. 复现命令

```powershell
# 进程内分段（gallery 会打 log_startup_summary）
$env:ST_PERF_STARTUP="1"
.\build\dev\bin\gallery.exe --frames 2                       # scale 1.5（默认，跟随系统 DPI）
.\build\dev\bin\gallery.exe --scale 1.0 --frames 2           # 对照：字形超采样差异
.\build\dev\bin\gallery.exe --renderer gpu --frames 2        # 对照：GPU 首帧（但建窗 +390ms）
.\build\dev\bin\gallery.exe --headless --frames 2            # 对照：无窗口基线

# 首帧三阶段（cold_total_ms）
.\build\dev\bin\gallery.exe --frames 6 --perf-frames 4 --perf-report perf-cold.json

# 稳态绘制分解（注意：不是首帧）
$env:ST_PAINT_PROFILE="1"; .\build\dev\bin\gallery.exe --bench 20

# 主线程响应性
py -3 tools/startup_responsiveness_probe.py --exe build/dev/bin/gallery.exe
```
