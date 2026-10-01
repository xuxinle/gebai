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
  默认「有窗口 → normal，无头 → off」；协议 `metrics.text_fit` 可查。
- [ ] **网格拟合的后续打磨**（非阻塞；当前效果已达标）
  ① 同组内按位置排序做**最优平移**（现在是每组独立吸附）——相邻 stem 在小字号下可能互相挤压；
  ② 横笔画（`Normal` 档已生效）的收益尚未单独量化；
  ③ hinting 与 `supersample` 的交互（当前两者独立生效，尚未联合调参）。
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
- [ ] **视觉断言原语**：`visual.diff` / `capture.hash`（区域像素哈希/模板相似度）进协议——
  「改代码→看图→断言」闭环最后一公里。
- [ ] **事件流 TS 端完整消费**：客户端常驻连接与事件队列已做；
  待补：`shuangtian_wait_event` 工具暴露 + 服务端 `ui.changed` 携带变更元素 id 清单。
- [ ] **script_host 提交链仍调 `mark_dirty_all`**（控制通道路径已改损坏区驱动）：
  JS 写入目前仍整帧；待脚本路径补上元素级标脏验证后再同样收敛。

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
  启动日志一行、协议 `metrics.text_renderer`；默认「有窗口→亚像素 / 无头→灰度」。
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
  （mdeditor SourceView::measure），GPU 遮罩按包围盒全量分配 ≈1.3 TB → 崩。
  修复：遮罩与裁剪域取交（软件侧 draw_shadow 同思路）+ 示例固有高度修正 +
  回归测试 `gpu_huge_path_mask_is_bounded_by_canvas`。
- [x] **奇偶测试假绿**：`compare()` 的 `structural` 计数器从未自增，
  `structural_ratio < 0.001` 断言全部空转——上面两个缺陷因此长期潜伏。已补自增。

### 本轮主线落地

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
- [x] **CI 三平台矩阵**（.github/workflows/shuangtian-ci.yml：linux/windows/macos
  test+lint，Linux 附 mingw 交叉编译）
- [x] **文档漂移全修** + check_docs.py v2（§ 引用/路径实存/组件清单/用例数四类自动核对）

> 审视报告「第一梯队」与「第二梯队」（含渲染侧：增量重绘/整形缓存/LRU/渐变快路径）
> 均已在上述条目落地；无头环境无法验证的窗口模式 vsync 如实留在 P1。
