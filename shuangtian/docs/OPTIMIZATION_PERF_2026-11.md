# 霜天性能优化轮（2026-11）— 进度与交接

> 分支：`optimize/perf-2026-11`（基于 `260ad1d3`）。目标：系统性分析与优化霜天的性能和资源使用，接近原生 UI。
> 机器：4K 3840×2160 @150% DPI（画布 3533×1921 = 679 万像素），RTX 4080 SUPER，28 核。

## 基线（改动前，release 档）

| 场景 | 指标 |
|---|---|
| headless 1178×736 全量 | 总帧 8.48ms p50（paint 8.4） |
| win32 4K 软件全量 | 总帧 27.4–27.6ms（paint 20 + present 7–8.8） |
| win32 4K GPU | 2.48ms |
| 静止空转 | hover 悬停期间每 16ms 一帧全量重绘（60fps 空转） |
| 冷启动 | 进程 wall ~234ms，首帧 paint 121.8ms |
| 内存 | 95.6MB |

## 成果（改动后）

| 场景 | 基线 | 改后 | 幅度 |
|---|---|---|---|
| headless 1178×736 全量 | 8.48ms | **4.47ms** | **-47%** |
| win32 4K 软件全量 | 27.4ms | **17.95ms** | **-34%** |
| 交互帧（hover 过渡，局部） | 全量 20+7ms | **partial 帧 present 2.66ms**（408×233 脏区） | 送显 -62% |
| 静止悬停 | 60fps 空转全量帧 | **帧数冻结，2s CPU 增量 0** | 空转消除 |
| 4K 送显（全量） | 8.8ms | **5.2ms**（重排 ~2.97→~1ms） | -41% |

全量测试：2272 passed / 0 failed（release）+ 2295 passed（--slow，见下「既有红」）。

## 改动明细（每项独立提交）

### P1 win32 软件路径局部上屏（`60b98bcb`）
- `blit(region)`：按行带只重排 + 只 BitBlt 脏区（物理换算复用 X11 的 `resolve_present_area` 纯函数）；DIB 保留型语义。
- `present()` 消费 `present_region_` 并帧末清零。
- 顺带修复（同提交）：**hint（悬停提示）计时段从 `animation_pending` 拉帧改为唤醒点语义**
  （`next_wake_in_ms` 并入 hint 到期时刻；`needs_frame` 只在到期后要帧）。
  旧路径在 450ms 计时段内每 16ms 一帧「damage 空 → 全量重绘」，纯白画。
  新增回归测试 `tests/ui_hint_wake_test.cpp`（计时段收敛/唤醒点在 (0,delay] 内/到期帧弹出气泡/弹出后收敛）。

### P2 Input/TextArea 双绘与被覆盖的内阴影（`f6a1a478`）
- `paint_content` 不再整块重画底色+边框（通用 `paint_box` 已用 `style_` 画过同一块）；
  状态色（聚焦 primary/悬停 border_strong/禁用 surface_alt）只在与通用色不同时补一道边框环。
- `apply_theme` 移除 `shadow.inset`：旧路径画完内阴影又被 `paint_content` 的 `fill_rect` 盖掉
  （内阴影从未真正可见），白付 4 条渐变带 + 1 次 rounded clip（每输入框 ~0.5ms）。

### P3 DIB 通道重排 AVX2 shuffle（`11fcfea7`）
- 新增 `raster::simd::rotate_bgra_row`：AVX2 单条 `_mm256_shuffle_epi8` 重排 8 像素
  （字节表 1,2,3,0 = 小端循环右移一位）、SSE2 4 像素、标量兜底；运行时检测。
- win32 `blit` 按行带调用（局部帧仍只处理脏区）。
- 新增三路一致性测试 `simd_rotate_bgra_row_matches_reference_across_lengths`
  （全部长度含尾部/原地转换/伪随机全值域）。**首版 shuffle 表写反（3,0,1,2）当场被它抓出**
  ——通道串色类错误有了护栏。

### P4 冷启动分解（量化完毕，优化转 BACKLOG）
- 首帧 paint 121.8ms = 99 元素首绘 + 字形光栅（LCD 亚像素）+ 阴影/精灵缓存首建；layout 仅 1.1ms。
- 进程 wall 234ms ≈ 静态初始化 + 字体解析（~63ms，已 mmap）+ 首帧 + 尾。
- 深入优化（后台预热线程/跨进程字形缓存）工程量大、收益上限 ~100ms，且原生框架同类冷启动同量级 → 记 BACKLOG。

## 验证环境要点（复测时需要）
- 控制通道协议：**大端 4 字节长度前缀 + JSON**（`struct.pack('>I')`），hello 需 token（`--control-file` 落盘）。
- `--perf-frames N` 只开剖析**不设帧限**；帧限是 `--frames N`。bench 跑完不退出（等 shutdown）。
- 控制通道测试模板已验证（py + socket + drain_by_id）。
- PowerShell 无 `&&`；gallery.exe 被占用时先 `Stop-Process`。

## 门禁状态
- `st test --profile release`：2272 / 0 ✓
- `st test --profile release --slow`：**2 个既有红（基线 `260ad1d3` 同样红，非本轮引入）**：
  - `bench_thresholds_have_bounded_headroom`：渐变填充余量 1.13–1.17 < 下限 1.3（本机渐变实测贴近阈值）
  - `scale_gate_select_panel_frame_is_independent_of_option_count`：比值 5.27–5.34 > 上限 5.0（本机状态）
  - 其余 bench 类偶发红为并行分片 CPU 竞争（测试文件头已注明该现象与归档理由）。
- `st lint`：27 项违反**全部在三维模块**（mesh_boolean/scene3d_script/scene_xml，三维轮遗留）；本轮改动文件 0 违反。

## BACKLOG 增补
1. **冷启动后台预热**：字形光栅与启动序列并行（worker 线程预热首屏文本 glyph），上限省 ~100ms。
2. **`bench_thresholds` 渐变余量**：本机渐变填充 3.4× 贴近阈值 4×（余量下限 1.3 卡住）——需在基准机器重标定或查渐变是否退化。
3. **SelectPanel scale-gate 5.27×**：本机稳定超上限 5.0（基线同红）——查 SelectPanel 绘制是否真有随选项数线性增长的部分。
