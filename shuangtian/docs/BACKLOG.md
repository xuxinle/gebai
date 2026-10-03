# 霜天 Backlog（2026-09-30 全面审视与「全部优化」会话后的台账）

> 本文件是唯一权威清单，完成后移入「已完成」并在 DESIGN.md 更新里程碑。
> **诚实原则**：写着「待做」却已完成的条目会误导读者；写着「已完成」却没落地的条目更糟。

### 画廊全场景补全（v0.1.5，2026-09-30 第二轮审视）

- [x] **叠加层 z 序修正**（P0 级缺陷）：`paint`/`paint_frame` 中 overlay 先于内容绘制，
  导致 Dialog 遮罩/卡片、Toast 全部被内容盖住（画廊补全时实测发现：dialog-open 截图里
  什么都看不到）。改为内容先画、浮层后画。
- [x] **`find`/`query` 覆盖叠加层**（同源缺陷）：`UiRoot::find(id)`/`query(selector)` 只遍历
  content 树——打开的 Dialog/Toast/Select 面板从选择器里永久 not_found。
- [x] **`TextArea` 属性面补齐**：未实现 `get/set_property`，`set value` 被静默忽略
  （与 Input 踩过的坑同类）。
- [x] **`Toast` 自动消失**：`set_auto_dismiss_ms`（帧时间轴驱动，`on_dismiss` 回调 +
  `expired`/`auto_dismiss_ms` 属性面；到期帧连阴影都不落盘）。
- [x] **`Table` 选中行**：`set_selected_row`（视觉 primary_soft+主色左缘条、语义值
  `sel=N`、属性面读写、越界拒绝、clear_rows 清除）。
- [x] **`SceneView` 逐帧调试 fprintf 移除**（常驻动画下 stderr 被淹、软件腿被拖慢）。
- [x] **画廊新增场景**：浮层与反馈卡（Dialog 三关闭路径/Toast 自动消失）、
  多行文本卡（TextArea 折行/行数回显）、禁用态卡（Button/Input disabled）、
  数据页表格点击选中+真实 label 回读、概览页四张统计卡全部改实时值（消灭占位假数据）。

## P0（当前无——上轮的四项渲染地基已全部落地，见「已完成」）

## P1

- [x] **图标系统改用 SVG（矢量数据源，任意缩放清晰）** —— 2026-10-03 落地
  起因：内置图标表是私有路径格式（M/L/C/Z 简化语法），矢量可缩放但**生态为零**——
  业界图标库（Codicons/Lucide/Feather/Tabler）全是 SVG。自研 SVG 图标子集渲染器
  （零新增第三方依赖，与光栅器/字体同路线）：path d 全指令（A 圆弧/S/T 反射/相对/隐式
  重复/科学计数法）、基本形状、viewBox/preserveAspectRatio、`<g>` 级联 + transform、
  sprite（symbol/use + defs 形状引用，防环）、evenodd 孔环。
  渲染语义对齐图标库惯例（缺省黑填充 / currentColor 描边 / 显式色不被主题色覆盖）；
  `IconSetPainter` 按 (id,物理尺寸,颜色) LRU 缓存——尺寸变化即矢量重栅（任意缩放
  清晰的技术根源），同尺寸零重栅。
  接入：`Icon::draw`「内置表优先、SVG 补位」+ `svg:` 前缀强制源；画廊新增 SVG 图标集卡
  （17 图标 ×16/32/64px 三档对照）；codeeditor 活动栏/标题栏/状态栏图标切 SVG。
  测试：`tests/ui_svg_test.cpp` 19 用例 80 断言全绿——含「缩放清晰度」量化断言
  （16px vs 96px 墨量占比守恒 <35%，位图放大做不到）。开发中被测试/视觉断言抓出的
  真缺陷 10 处（transform 组合顺序反、根样式继承断链、defs 引用缺失、symbol 双绘、
  `Path::reverse` 语义误用、sprite 根属性丢失塌剪影等）全部修复；`st test` 543 用例
  全绿、`st lint` 0 违规。

- [x] **小字锐度：笔画网格拟合（hinting）** —— 2026-10-01 落地（`DESIGN.md §4.3.2`）
  起因：13.5px 正文的笔画边缘 **100% 落在分数相位**上（每边一个过渡像素的缓坡占 91.8%），
  这是「字看着糊」的主因，**与抗锯齿模式无关**。
  实现前先量出各条路径的收益（`tools/hinting_gain_probe.cpp`）——三条结论改变了方向：
  ① 读字体自带 hinting 指令 ≈ 什么都没做（拉丁 3.2%→3.2%）；
  ② 中文界面字体是 **CFF**，`TARGET_MONO` 对它无效 → **给中文做 TrueType 指令解释器是白工**
     （原计划的主要工作量因此被证伪，省下几千行）；
  ③ 唯一有效的是 **auto-hinter 式的几何网格拟合**（拉丁 49.0% / 中文 20.6%）。
  落地：`grid_fit`（抽直线笔画 → 两侧各自吸附整数网格 → 曲线控制点按 Bernstein 权重跟随 →
  位移护栏），接入 `TextRenderer`，缓存键含拟合模式位。
  实测（`tools/stem_phase_probe.cpp`，同一把尺子）：中文糊笔画 90.9% → **21.8%**、
  锐笔画 0% → **54.7%**（auto-hinter 基准的 2.8 倍）；拉丁 87.3% → 44.0%；
  中间调占比 71.1% → 49.5%；聚合墨量变化 −0.3%~+2.5%。
  **字宽与位图网格逐字段不变**（测试钉死）。开关 `--text-fit auto|off|light|normal`，
  默认 **`normal`（所有场景；内置通道与桌面同源）**；协议 `metrics.text_fit` 可查。
- [x] **网格拟合在 DPI 缩放下的三处修正** —— 2026-10-02（用户反馈「字体渲染远不如浏览器」）
  抓手是用户指定的两条：**轮廓连续性**与**宽度一致性**（量尺 = 覆盖率矩阵 + 笔画横截面）。
  逐像素取样后定位到三个真缺陷（每个都有量化证据，见 `DESIGN.md §4.3.2`）：
  ① **吸附格点错位**：`TextRenderer` 在**超采样空间**里调用拟合（本机 1.5× DPI → `supersample=2`），
     而 `grid_fit` 在该空间里吸**整数** → 吸附目标是"半个物理像素"，**拟合反而制造糊边**
     （实测「霜」@20.25px 的 0.5 覆盖率像素 25 → **96**）；且阈值按采样单位比较，2 倍采样下
     实际只放行 1.3px 宽的笔画（中文笔画大半漏掉：找到的竖笔画 35 → **7**）。
     修正：新增 `GridFitOptions::grid`（吸附格点锚在物理像素边界 + 阈值物理口径换算），
     拟合改在**输出位图局部坐标**里做（位图原点 `floor(bounds)-1` 与轮廓整数格点差一个附加小数）。
     实测（`tools/grid_fit_sampling_diag.cpp`，ss=2）：物理网格命中 4.2% → **41.2%**。
  ② **护栏量错了东西**：原用"逐点 |位移| 均值"，该量**随笔画密度单调上升** →
     国/回/目/霜 这类最需锐化的 CJK **整体被拒**（实测 0.26~0.33 全部超 0.25）。
     改成量**整体平移**（有符号均值，`max_drift`）——两侧对吸时正负相消，真平移才顶阈值。
     修正后这些字形的 drift 只剩 0.002~0.10，全部生效。
  ③ **宽度不量化**：两侧各自独立吸附会让 1.5px 的笔画在 1px/2px 间**随相位跳**。
     改成**宽度先量化到整数**（下限 1px）、再把位移小的一侧锚到网格，另一侧由宽度推出。
  综合效果（同一把尺子，1.5× DPI）：中间调占着墨 中文 **69.2% → 44.5%**、拉丁 65.2% → 56.7%；
  「一步到位」上升沿 中文 407 → **1603**、拉丁 261 → **781**；墨量变化全部 ≤ ±2.5%（护栏仍有）：
  桌面真实窗口与 Edge 同屏 1:1 比对，21px 标题中间调占比 30.5% vs Edge 27.1%，
  13px 正文 **10.4%** vs Edge 44.2%（更锐且无偏色）。
  新增测试 `grid_fit_snaps_to_physical_pixel_lattice_at_any_supersample`（含负例）。
- [ ] **网格拟合的后续打磨**（非阻塞；当前效果已达标）
  ① 同组内按位置排序做**最优平移**（现在是每组独立吸附）——相邻 stem 在小字号下可能互相挤压；
  ② 横笔画（`Normal` 档已生效）的收益尚未单独量化；
  ③ 可选：把量化宽度后的**单边锚定**换成保字面中心（CJK 字身前倾观感，待截图评估）。
- [ ] **窗口模式 vsync / 空闲阻塞**（帧调度的最后一截）：`present()` 仍 `Present(0,0)`
  不等 vsync（无头验证不了撕裂/节奏，标为待真机窗口测试）；空闲仍 4ms 轮询
  （实测 0.57% 单核，已够低；`MsgWaitForMultipleObjects` 事件阻塞属锦上添花）。
  另：空闲 CPU 回归护栏（60s 窗口量进程 CPU）——验收脚本已有（会话 tmp/st_idle_cpu.py）。
- [ ] **x11 窗口后端**：backend.cpp 现仅探测+Unsupported，Linux 真桌面硬阻塞。
- [ ] **IME 输入法**：win32 无 WM_IME_* 处理，中文输入在窗口模式不可用
  （无头 AI 注入不受影响）。EventKind 需加合成事件。
- [ ] **停摆竞态根因定位**：2026-09-30 实测两次「单连接永久停摆」（服务端健康、
  仅旧连接饿死、>60s 不自愈；两次命中后 ~4800 次调用未再复现）。已加固：
  错误码平台化（头号嫌疑：winsock 不设 errno）、发送失败不静默、心跳可探。
  若再复现：用 tools/ 下探针的取证路径继续查（表征见审视报告 §4.1）。
- [x] **视觉断言原语**（2026-10-02 落地）：`capture.hash`（区域像素 FNV-1a 64——同区域两次一致
  即逐像素完全一致）与 `visual.diff`（与基线 PNG 逐像素比对：diff_pixels/diff_ratio/mean_diff/
  max_diff/diff_bounds；`write_baseline=true` 存基线、`threshold`/`tolerance` 双旋钮；
  尺寸不一致即报错不缩放对齐）进协议——「改代码→看图→断言」闭环最后一公里。
  Host 新增 `capture_pixels`（RGBA8 原始字节，不编码 PNG）与共享区域解析（id/region 同口径）；
  测试：`control_protocol_test.cpp` 3 用例 + `tools/visual_assert_e2e.py` 真实应用 7 项端到端；
  TS 侧 `capture_hash` / `visual_diff` 工具（免审批）+ 3 用例。
- [x] **事件流完整消费**（2026-10-02 落地）：`ui.changed` 事件携带**变更元素 id 清单**
  （`UiRoot::note_changed` 登记：set/apply_properties、invoke、输入命中/焦点、ui.create/ui.remove 五路；
  去重 + 单帧上限 64；不报悬浮过渡/光标闪烁这类逐帧噪声）；
  TS 侧新增 `wait_event` 工具（单次调用内订阅收集，无跨调用状态）——
  客户端此前 `keep_events` 是死参数（BACKLOG 写「已做」是文档漂移，本次补实）。
  测试：`ui_changed_event_carries_changed_ids` 协议用例 + `tools/events_e2e.py` 三路
  （set/invoke/input.text）+ TS 两用例。
- [ ] **script_host 提交链仍调 `mark_dirty_all`**（控制通道路径已改损坏区驱动）：
  JS 写入目前仍整帧；待脚本路径补上元素级标脏验证后再同样收敛。
- [ ] **声明式 UI 后续里程碑**（M1–M4 主体已落地，设计与路线见 `docs/declarative.md`）：
  - [x] M1 元素工厂：`dsl::make_element`（27 个内置类型）。
  - [x] M2 C++ 宿主：`dsl.hpp/cpp` + 重组器 + `examples/counter`。
  - [x] M3 JS 宿主：`declarative.js` + `ui::DeclarativeHost` 窄桥 + `examples/counter-js`
    + 双宿主一致性 fixture（结构签名逐字节相等）。
  - [x] M4 补全：ArkTS 风格链式修饰（大写组件 + `.padding().onClick()`）、`useResource`
    异步状态（引擎 `pump_jobs` 泵微任务）、`ForEach` 按 key 复用（`__d_move` 重排）、
    `examples/todo-js`（异步 + 列表 + 过滤，`tools/todo_js_e2e.py` 端到端验收）。
  - [x] **`custom<T>` 逃生舱**：任意组件 + 一等接口的通用入口（`type_name<T>` 特化表，
    工厂补齐全部 32 类型）；`icon`/`overlay`/`dsl::tabs` 数据驱动。
  - [x] **IDE 形态界面声明式重写**：`examples/codeeditor-dsl`（五层布局 + 多标签 +
    菜单栏下拉 + 命令面板 + 全局快捷键 + 终端 + 主题，~700 行 vs 命令式 1417 行；
    `tools/codeeditor_dsl_e2e.py` 八项端到端）。
  - [x] **浮层生命周期**：`overlay(c, key, ...)` / `menu_panel_overlay`——按 key 认领，
    未声明即回收（重组末尾 sweep）；`menu_bar` 数据驱动 + `Composer::register_shortcut`。
  - [x] **多作用域细粒度重组**：`sub_component` 子组件独立作用域（子状态变化只重跑子，
    父不重跑）；依赖按 scope 分流 + 宿主位置/子树游标恢复。
  - [x] **C++ 侧异步 `resource<T>`**：工作线程执行 + `pump_async` 回主线程写状态，
    输入指纹节流 + 代次计数丢旧结果。
  - [x] **构造期属性组件包装**：`dsl::select`/`dsl::table`/`dsl::tree`。
  - [x] **协议 `ui.create`/`ui.remove`**：在线建删元素（与声明式共用工厂）。
  - [x] 旧版 `examples/codeeditor` 编译修复（三处 `-Werror=shadow`）。
  - [x] **线程池**：固定工作线程（`hardware_concurrency` 夹取 1~8）+ 任务队列，
    替代「每任务一线程」（密集场景不再瞬间开几十个线程）；懒建——不跑异步的应用不付代价。
  - [x] **异步取消语义**：`AsyncCancel` 取消牌（≈ JS `AbortSignal`）——输入变化翻旧代的牌
    （fetcher 可在耗时点提前退出）+ 主线程写回前代次校验（旧代结果丢弃）；
    析构时翻全部牌并丢弃未开始的任务（不是死等）。
    **修正一个真 bug**：原实现的 `async_begin` 只换 token 不翻牌——旧代结果仍能通过校验写回。
  - [x] **嵌套作用域树**：作用域可任意嵌套（页 → 面板 → 叶），每层独立失效；
    父重跑后子树标脏（对齐）+ 未重新声明的作用域被剪枝（条件分支去掉了它）。

## P2

- [ ] 多窗口抽象（Window/WindowManager 进 shell，control::Host 带窗口维度）。
- [ ] 统一动画系统（现每组件手工状态机：advance_hover/last_hover_time_）。
- [ ] DisplayList（立即模式→保留模式）：解锁多线程分帧、录制回放测试、GPU 图层缓存。
- [ ] 协议命令注册表机制（server.cpp 分发链较长，新命令多处同步）。
- [ ] `st add/fetch/vendor/audit/outdated` CLI 入口（内核已就绪）+ TLS + 归档解包。
- [ ] 无障碍桥（UIA/AT-SPI/AXUIElement）：SemanticsNode 数据已在，建议列入规划。
- [ ] GPOS/RTL/bidi/HarfBuzz 类 shaping 层（当前仅 kern format0）。
- [ ] Scene3D GPU 腿（软件腿已可用；触发条件见 DESIGN §8.4.2）+ 多线程光栅化。
- [ ] 文件对话框/全局快捷键/系统托盘等桌面系统能力。
- [ ] check_docs.py 纳入 CI 常跑（本轮已升级为事实核对；防复发机制已建）。

## 已完成（本轮「全部优化」落地，备查）

### 文字亚像素渲染（LCD / ClearType 类）—— 2026-10-01（vsedit 反馈 P1）

- [x] **`CoverageFormat` 进 `Surface` 接口**：覆盖率位图两种通道布局（`Grayscale` 1 项/像素、
  `Lcd` 3 项/像素），`blend_coverage_bitmap` 按布局选混合公式；既有调用零改动。
- [x] **`TextRenderer::set_subpixel`**：字形按 **3× 水平超采样**（`3·supersample 列 × supersample 行`）
  光栅化后逐子像素聚合 → 每像素 R/G/B 三个覆盖率（= `FT_RENDER_MODE_LCD` 口径）；
  包围盒由灰度口径 ×3 推导 ⇒ **两种模式位图网格逐像素重合（不挪字）**；
  5-tap 低通滤波（`FT_LCD_FILTER_DEFAULT` 权重，可 `ST_TEXT_LCD_FILTER=0` 关）；
  **缓存键含渲染模式位**（两种位图共存不混用）。
- [x] **软件混合**：逐通道 α 的 src-over（`out_c = S_c·α_c + D_c·(1-a_s·α_c)`），
  非 `SrcOver` 模式如实退化为三通道均值。
- [x] **GPU（D3D11）**：`R8G8B8A8` 覆盖率纹理（缓存项带三通道标志）+ 着色器 `CoverageMaskLcd`
  + **两遍混合**（`ZERO/INV_SRC_COLOR` 逐通道衰减目标，`ONE/ONE` 加性加回源项）；
  `Capabilities::lcd_text` 如实上报；顺带把「管线建不起来」变成显式失败
  （`capabilities()`/`create_canvas()` 挡住着色器编译失败与混合状态创建失败）。
- [x] **开关与可观测**：`--text-lcd auto|on|off`（三个示例 + 通用命令行都接）、`ST_TEXT_LCD`、
  启动日志一行、协议 `metrics.text_renderer`；默认 **亚像素（所有场景；内置通道与桌面同源）**。
- [x] **验证**：新增 `tests/text_subpixel_test.cpp`（6 用例：网格重合 / 墨量守恒 / 亮度 profile 守恒 /
  彩边只在边缘 / 滤波取舍 / 缓存不混用）+ `gpu_parity` 的亚像素对比用例（Windows 侧真跑）；
  实测数据与**诚实的边界**（亚像素不缩小过渡带）记在 `DESIGN.md §4.3.1`。

### 渲染性能地基（本轮会话后半段落地）

- [x] **增量重绘（损坏区机制）**：元素级 damage（bounds + 绘制外扩）上报到树根，
  UiRoot 帧首收集；软件画布走「清损坏区 + 推裁剪 + 整树剔除重绘」（z 序天然正确）；
  `request_repaint` 去掉 `mark_dirty_all` 锤子（控制 set/invoke/输入改损坏区驱动，
  空损坏区回落整帧兜底）。实测：悬停过渡帧 2×DPI 软件 paint **38ms → 1.86ms**；
  像素级等价测试（tests/ui_damage_test.cpp）+ `metrics.partial_frame/dirty_rect` 可观测。
- [x] **文本整形结果缓存** + **字形缓存真 LRU**（均按内存预算：整形 16MiB、
  字形 24MiB+条目上限；淘汰增量、不再全清尖峰）。
- [x] **渐变快路径**：竖向线性整行同色 + SIMD 整行混合；倾斜线性位置递推。
  debug bench：6.67 ms → **0.122 ms（54×）**，从清屏 162× 降到 1.13×。
- [x] **控制通道「固定 ~30ms 节拍」消除**：高精度睡眠（CREATE_WAITABLE_TIMER_
  HIGH_RESOLUTION）+ 统一 `Application::pace_loop`（示例的 16ms 硬编码睡眠删除）。
  ping p50 **31.2ms → 4.5ms**。


### 会话后半段挖出的预存缺陷（由本会话修复）

- [x] **GPU 路径遮罩坐标符号错误**（渲染缺陷，影响一切描边/图标/勾选）：
  `path_mask_texture` 给 `rasterize_mask` 传了 `-area.xy`（该函数语义是「路径减去 origin」，
  与 canvas.cpp 调用同约定）→ 图形被画到 `path + 2·area` 处、大半被裁掉。
  **表现**：GPU 渲染下画廊 72 个图标、复选框勾选、导航图标、输入框边框全部缺失。
  修复：传正 `area`。前后对比截图 5 页留证。
- [x] **越界几何导致 bad_alloc 崩进程**：元素把 `kUnbounded`（1e9）当作自身高度时
  （早期 mdeditor 示例的 SourceView::measure；该示例已删除，教训保留），GPU 遮罩按包围盒全量分配 ≈1.3 TB → 崩。
  修复：遮罩与裁剪域取交（软件侧 draw_shadow 同思路）+ 示例固有高度修正 +
  回归测试 `gpu_huge_path_mask_is_bounded_by_canvas`。
- [x] **奇偶测试假绿**：`compare()` 的 `structural` 计数器从未自增，
  `structural_ratio < 0.001` 断言全部空转——上面两个缺陷因此长期潜伏。已补自增。

### 本轮主线落地

- [x] **D3D11 fill_path 缺 DPI 缩放**（真窗口 1.5x 实测发现）：`fill_path` 直接光栅化逻辑坐标路径，
  非 1x DPI 下一切路径类绘制（菜单面板/勾选/图标）整体缩成 1/scale——hit_test 按逻辑算、
  画出来缩小错位，表现为"菜单高亮与光标错位、面板残缺"。修复：与软件 `Canvas::fill_path`
  同口径，内部 `path.scaled(scale_)`（缓存键仍用逻辑路径）。
- [x] **CodeEditor 鼠标拖选失效**（真窗口实测发现）：拖选判定用 `event.button != 0`，
  而 Win32 `WM_MOUSEMOVE` 不携带按键（后端恒传 0）→ 真窗口永远不扩选（协议驱动的 move
  默认 button=1 才"能用"，掩盖了缺陷）。修复：改 `selecting_` 状态机（Down 置位/Move 扩选/Up 清除）；
  同时 `UiRoot` 的 MouseMove 分派增加**拖拽归属**（pressed_ 元素在释放前持续接收 move，
  拖出边界不断选——与 ScrollBar 拖滑块同一受益）。
- [x] **CodeEditor 代码正文未用等宽字体**：`port.draw/measure_width` 全部走默认 Proportional
  角色（比例字体），代码编辑器字形宽度不齐且小字号发糊。修复：全部调用点传
  `FontRole::Monospace`（等宽栈 Cascadia/Consolas 本就存在，只是没接上）。
- [x] **示例精简与 VSCode 式重写**（2026-10）：删除 mdeditor 示例；codeeditor 按 VSCode 信息架构
  重写（标题栏/菜单栏/活动栏+侧栏/多标签编辑区/底部面板/状态栏，全内置组件零自绘）。
  重写中反推出的框架缺口（详见 DESIGN.md §8.1.1）：
  - [x] **SplitView 内置化**（2026-10-02 落地）：框架组件 `SplitView`（拖拽分栏，
  见 DESIGN §4.5 v0.1.5 组件能力）；codeeditor 侧栏/编辑区已迁移；测试 7 用例
  （`ui_split_view_test.cpp`）+ `tools/split_view_e2e.py`（拖拽/夹取/动作面/截图）。
  迁移中发现并修复框架交互缺口：`MouseUp` 未按拖拽归属投递（拖出手柄后释放丢失）。
- [ ] **命令面板通用组件**：CommandPalette（FillViewport + 过滤列表 + 键盘导航）值得内置；
- [x] **单行 Input 动作面**（2026-10-02 落地）：`invoke submit/activate/clear` 与 TextArea 对齐；
  测试 `ui_input_invoke_*` 3 用例 + `tools/input_action_e2e.py` 真实应用端到端。
  - [ ] **虚拟化长列表**：终端/输出面板的 ScrollView+Text 累积全文，日志长了退化；
  - [ ] **桌面窗框**：平台 shell 层的系统标题栏融入/自绘窗框；
  - [ ] **编辑器分组**：VSCode 式左右分屏各持独立标签组需容器级支持。
- [x] **text_subpixel_ink_matches_grayscale 长期红修复**（2026-10-02；此前归因有误）：
  此前台账写"hinting 提交（a34699b）后超阈"——本次对照实验证明**与 hinting 无关**
  （把 text.cpp 回退到 LCD 提交可复现同样的 0.1083）。真因：超阈来自 **LCD 5-tap 滤波的
  横向摊墨**（'I'@11px 窄笔画：未滤波 0.0163、滤波后 0.1083），而摊墨是滤波存在的目的
  （压彩边），不是字形走样——旧断言把"设计行为"当成了"缺陷"。
  修复：把"形状保真"（未滤波 vs 灰度）与"滤波摊墨有界"与"墨量守恒"拆成三个互不混同的
  断言（实测 0.0163 / 0.1083 / 聚合 1.34%，滤波对未滤波墨量差 0.00%）；新增
  `tools/lcd_ink_probe.cpp` 量尺（逐字形偏差分解，滤波开/关两栏）。

- [x] **网络层错误码平台化**（WSAGetLastError/errno 分流）+ **发送失败不静默** + 帧合并单发 +
  accept 关 Nagle + 接收缓冲偏移游标（A1）
- [x] **鉴权 token 最小集**（自动生成→控制文件→hello 校验→未握手只允许 hello/ping）+
  **capture 落盘白名单**（A1）
- [x] **协议补全**：input.mouse drag / wait for=frames / invoke 动作白名单 /
  key press 补 KeyUp / wait elapsed_ms 修正（A1）
- [x] **崩溃自愈**：platform_crash.cpp（SEH/信号 → stderr 现场）+ ST_MAIN 自动安装 +
  退出清理控制文件（A1）
- [x] **协议一致性测试**（control_protocol_test.cpp，11 用例）+ TS 客户端**自动握手**
  （同批写出不增往返）+ ping 判活/孤儿回收/PID 复用防护 + TS 测试平台化修复（38/38 绿）（A1）
- [x] **AVX2 运行时分派**（simd.cpp 16 通道 + __cpuid 门控 + 单文件 /arch:AVX2）（A2）；
  附修两个预存缺陷：MSVC 因宏缺失**静默退化标量**、SIMD 除 255 近似 **off-by-one**
  （改精确公式 `(x*0x8081)>>23`）
- [x] **链接指纹缓存**（无改动 dev 构建 3.3s→1.6s，链接直接跳过）；**MSVC 预编译头**；
  **Windows 内存探测**（GlobalMemoryStatusEx）；**st clean**；**测试框架** per-case 超时
  + `--list` + `--format junit`（并完成 CLI 接线）；lint 正则一次性编译；
  bootstrap.ps1 读 st.pkg 去双写（A3）
- [x] **Windows 默认编译器统一 g++（MinGW-w64）**（MSVC 可回退）+ **内置通道与桌面一致性**
  （文本/DPI 默认同源；`tools/st_consistency_check.py`）——含四处真实缺陷修复：
  测试注册表静态初始化顺序（g++ 下启动即崩）、MSVC PCH 创建静默空转（C1083 假阴/假阳）、
  第三方运行时的 STL 弃用警告被 `/WX` 拦截、PCH 身份未入编译指纹与缓存键（LNK2011）。
- [x] **CI 三平台矩阵**（.github/workflows/shuangtian-ci.yml：linux/windows/macos
  test+lint，Linux 附 mingw 交叉编译）
- [x] **文档漂移全修** + check_docs.py v2（§ 引用/路径实存/组件清单/用例数四类自动核对）

> 审视报告「第一梯队」与「第二梯队」（含渲染侧：增量重绘/整形缓存/LRU/渐变快路径）
> 均已在上述条目落地；无头环境无法验证的窗口模式 vsync 如实留在 P1。
