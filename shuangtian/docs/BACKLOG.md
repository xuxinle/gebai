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

- [x] **智能体×框架协同：把“一次交互意图”变便宜（P0+P1 六项）** —— 2026-10-03
  起因：逐项实测开发循环的摩擦，**先量化再动手**——过程中推翻了自己的两个错结论，也挖出两个真缺陷。

  **① 控制通道被帧节拍量化（最大的那笔）**
  实测：写操作 p50 **15.8 ms** vs 读操作 **4.1 ms**，差值 = 帧节拍 − 距上次命令的间隔
  （间隔 0→15.7、8→8.5、16→3.8、100→2.4 ms）——连续操作（AI 的常态）几乎每次白等一拍。
  服务端 handler 只有 30–100 µs，代价全在**盲睡**：`tick()` 是
  `render_frame() → poll() → pace_loop()`，命令在睡眠期间到达就只能等睡醒。
  修法：把控制通道句柄交给内核等（`platform::wait_any_readable`，一次 `poll`/`WSAPoll`
  覆盖监听套接字 + **所有已连接客户端**，超时上限不变）。
  → `ping` 4.10→**0.06**、`get` 4.14→**0.11**、`find` 4.15→**0.16** ms；
  `set`/`invoke` 15.8→**3.8–5.2 ms**（剩下的是真正绘制那一帧）。
  - 踩坑一：**只等监听套接字没用**——新请求走的是已接受的那个连接（改完延迟纹丝不动）。
  - 踩坑二：挂起 `wait` 的连接要挡后续请求，但门必须**只盖住等待期**——忘了在
    满足/超时后清标志，这条连接以后每次调用都被拒（实测：等完之后连 `get` 都失败）。
    两条都补了回归用例（`wait_connection_is_reusable_after_satisfied`）。
  - 连带修：`remove_overlay` 在**事件分发栈内**析构正在被分发的子树 → use-after-free
    （`ui_select_opens_via_overlay_host` 当场 SIGSEGV；之前靠“下一帧布局才摘除”侥幸不崩，
    等待变快后立即暴露）。改为**延迟析构**（激活列表立即摘掉，对象延到分发结束/下一帧）。

  **② 按 id 点击**（`input.mouse{id}`）：服务端算 `bounds` 中心；找不到/未布局/不可见
  → **明确拒绝**。旧路径要先 `find` 再手算中心，而 `find` 连不可见元素也返回
  （本会话实测因此点空两次）。同时 `find` 结果补上 `visible`/`enabled`。

  **③ `invoke` 回带 `state`**：动作之后几乎总要看“变成什么了”，不带就得再发一次 `get`。

  **④ 列表类通用视口契约**：`Tree`/`List`/`ScrollView` 之前**完全没有属性面**
  （`get_property` 计数 = 0）——而“用户现在看得到哪一块”正是 AI 最常问的。
  统一为 `rows`/`items`/`lines` + `first_visible*` + `visible_rows`/`visible_items`/`visible_lines` + `scroll`。

  **⑤ `wait{for:"property"}`**：等“某元素的某属性等于某值”。旧的 `text` 条件要拼整棵
  语义树且只能匹配文本子串（数字/布尔/复合值表达不了）；新条件只查命中元素。
  并发证明脚本实测：真挂起 801 ms 后在条件成立瞬间返回（不是 0 也不是超时）。

  **⑥ 删掉两个我自己的错误结论**（记录以免后人重踩）：
  - “`find` 比 `get` 慢 6 倍（20.3ms）”——**假象**。热调用下两者都是 4.1ms；
    20.3ms 是首次调用的冷启动代价。改正后 `find` 无需优化。
  - “`capture` 没把 device_scale 契约写清”——**已经写了**（`pixel_size.device_scale`
    + region 逻辑坐标注释）。没动手改，只验证。

  **验证**：`st test` **589 用例全绿**（+6）/ lint 0 违规 /
  `st_visual_check.py` dev+san × 两应用 × 亮/暗 × DPI2.0 **0 失败步、无 sanitizer 报告** /
  mingw 交叉编译通过（顺带修了 `winsock2.h` 必须在 `windows.h` 之前——Linux 本机看不见）。

- [x] **codeeditor「平替 VSCode」第三批：编辑手感 + 视口可驱动 + 三个真缺陷** —— 2026-10-03

  **先说缺陷（都是实测出来的，不是推演）**：
  - **光标漂移（用户报的）**：`CodeEditor::x_for_index()` 漏传 `text::FontRole::Monospace`
    —— 它是光标/选择/查找高亮/缩进线/鼠标命中的共同量尺，而正文按等宽**绘制**。
    于是“量宽用比例字体、绘字用等宽字体”，x 误差随列号线性累积：
    实测量尺 `tools/caret_ink_probe.cpp` 给出 **45 列处 7.6992px**（比例字体 vs 等宽的逐字累加差）。
    修后新增 3 条回归用例（逐列步进 / 光标↔命中回环 / DPI 不变），
    并**逆向验证**：把 role 改回 Proportional，其中 2 条当即失败（含“点在第 n 列附近却落到别的字符”）。
  - **协议动作白名单误拒组件动作**：`kActions` 名单把 `CodeEditor` 的
    `undo`/`redo`/`set_text`/`goto_line`/`select_all`/`copy`/`paste`/`scroll_to_line`… 全挡在门外
    ——能力存在却报“未知动作”（**反向假阴性**）；名单还要跟着每个新组件手改。
    改为“动作直接交给元素，`handled` 即权威答复”（拼错动作名仍是 `handled=false`）。
    e2e `tools/input_action_e2e.py` 新增第 ④ 步（insert/undo 可达）实证。
  - **`CommandPalette` 三个交互缺陷**（迁移示例调用点时逐项暴露）：
    ① **鼠标点条目不执行**——执行链曾挂在元素层 `ListItem::on_activate` 上，而那一槽位
       被 `List` 用来“点击即选中本容器”，两者争用 ⇒ 命令被静默覆盖（只有 Enter 能用）。
       改为数据层 `List::Entry::on_activate`（`sync_items` 建项时接上）。
    ② **打开后面板打不了字**——焦点仍在底层编辑器，敲的字跑进了代码里；
       新增 `grab_focus()`（经 `Element::owner_as<UiRoot>()` → `UiRoot::set_focus`）。
    ③ **点与回车效果不等价**——两条路径各拼一份执行链（点只发外部通知、回车才调 handler），
       宿主把通知当“已执行”就出现“状态栏说执行了、文件其实没打开”。统一收敛到 `run_command()`。
    新增 3 条回归用例（含“点≡回车逐字段等价比对”）。
  - **`MenuBar` 下拉面板选完不消失 / 再开就叠两张**：`make_panel` 把面板的 `on_close`
    接到自己的 `set_open_index`，**调用方永远收不到关闭通知** → overlay 摘不掉。
    新增 `on_menu_close` 通道 + 打开状态下悬停其它标题即切换（VSCode/浏览器惯例）；
    点菜单栏空白也视为关闭请求。新增 3 条用例。
  - **`scroll_to_line` 不夹取** → 超界时把视口推成**空白屏**（示例“跳转到行”与控制通道都踩得到）；
    现统一经 `clamp_scroll`。

  **编辑手感**：自动配对（补右半 / 选中包裹 / 敲闭符**跳过**不重复 / 空对一次退格全删 /
  引号仅在词外配对）、**智能 Home**（两段式）、**Ctrl+Backspace/Delete 按词删除**
  （含“只剩空白时清空白”这条不留死按键的细节）、**Shift+滚轮**水平滚动。

  **视觉细节**：**缩进参考线**（Tab 按 `tab_width` 展开到**视觉列**对齐；颜色用
  `border_strong`——实测 `border` 只带来 ~7% 亮度变化，AA 后基本看不见，“看不见就等于没有”；
  像素级断言 `sum|Δ| ≥ 120` 守住这条）、悬停行底纹、行号槽内当前行高亮、
  竖向 + 横向**两条可拖拽滚动条**（悬停加宽）。

  **视口可驱动**（智能体可断言）：属性 `first_visible_line`/`last_visible_line`/`visible_lines`/
  `scroll`/`indent_guides`/`auto_pairs`，动作 `scroll_to_line`/`reveal_line`。
  > 注：视口属性**刻意不缓存 `RenderContext` 指针**——`UiRoot::render_context()` 按值返回，
  > 缓存它的指针会悬垂（实测：控制通道 `get` 当场 SIGSEGV，崩在字体度量里）。
  > 改用行高缓存（绘制过一次就有值）推算，拿不到就如实退化。

  **示例侧（codeeditor）UX**：私有 `CommandPalette` 退役改用框架组件（−130 行）；
  **Ctrl+P 真实快速打开**（把面板表换成文件表，88 个文件里打字过滤）；
  搜索面板改**真实递归搜工作区**（跳隐藏目录/二进制/大文件，命中给「文件:行 + 片段」，
  点一下**跳到该行并选中命中**）；问题面板接**真实轻量检查**（行尾空白 / Tab 缩进 /
  TODO·FIXME / 超长行，点问题跳行），状态栏错误·警告计数与面板**同源**；
  状态栏补选区计数；标题栏加脏点（`window_title()` 单一来源，7 处赋值收敛）；
  终端补 `ls`/`find <词>`/`goto <行>`/`stats` 真实命令；查找条 Esc 可关（此前关不掉）。

  **验证**：`st test` **583 用例全绿**（+26）/ `st lint` 0 违规 /
  `tools/st_visual_check.py` **dev+san × gallery+codeeditor × 亮/暗 × DPI 2.0 全部 0 失败步、
  无 sanitizer 报告** / mingw 交叉编译通过 / `tools/caret_ink_probe.cpp` 作为量尺入库。

- [x] **codeeditor「平替 VSCode」第二批：查找替换 + 真实文件工作区 + 命令面板内置** —— 2026-10-03
  - `CodeEditor` find/replace 组件能力：`set_find`（全部命中高亮，主题新增 find_highlight/
    find_active 两枚 token）/`find_next`（环绕、就近起步）/`replace_current`（替换后跳下一
    命中）/`replace_all`（走撤销栈可回滚）；编辑后命中表保持重建；动作面
    find/find_next/find_prev/replace/replace_all/replace 进协议白名单；属性面
    find_needle/find_matches/find_active。7 用例。
  - `CommandPalette` 内置为框架组件（此前 backlog 挂账）：数据驱动（Command 表）+
    大小写不敏感过滤（title/detail）+ 键盘环绕导航 + Esc/遮罩关闭 + 属性面/动作面
    （select 支持序号与命令 id）；7 用例。codeeditor 示例的私有实现退役（下批迁移调用点）。
  - codeeditor `--workspace <dir>`：真实文件模式——资源树 `fs::list_dir` 扫描（目录增量
    展开）、点文件 `fs::read_text` 打开（语言按扩展名推断）、Ctrl+S `fs::write_text`
    真实写盘（脏标记/标题栏/状态栏全联动）；缺省回退内置样例工作区（行为不变）。
  - 查找替换浮条（Ctrl+F/Ctrl+H）：输入即查（预填选中文本）、计数 n/m、↑↓ 环绕、
    替换/全部替换。
  - e2e 实证：协议改文本 → Ctrl+S → 磁盘内容更新；find 高亮命中 → replace_all →
    保存落盘（视觉验证：黄色命中 + 浮条完整 + 真实文件树）。
  - 顺带修 Input 程序 `set_text` 不触发 `on_change` 的语义澄清（保持——外部赋值不算
    用户输入，e2e 走 invoke 动作面/真实键入）。
  - `st test` 557 用例全绿（+14）；lint 0 违规。

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
- [x] **字形位图的采样格锚定（真正的「还不清晰」元凶）** —— 2026-10-04（`DESIGN.md §4.3.3`）
  起因：用户再次反馈「字体渲染还是不够清晰」。先立**与浏览器同口径**的量尺再动手
  （`tools/text_sharpness_probe.cpp` 全组合 + `text_ab_probe.cpp`/`text_ab_report.py` 逐像素 A/B），
  把观感拆成可分别修的几何项与色调项——本条的发现是**与拟合无关**的那一项。
  缺陷：位图原点 `floor(bounds) - padding` 只锚在**采样单位**上，而 1.5× DPI → `supersample=2`
  时它多半不是 2 的整数倍 → 输出像素平均的采样窗口**横跨两个物理像素**（横向糊墨）；
  且 `offset_x = min_x / supersample` 是整数**截断**，负奇数原点再错 1 像素。
  症状：同一字形的锐度随字号**奇偶交替**（实测「三」21.5px 中间调占比 **1.788** vs
  22.0px **0.037**；相位抖动 0.348），与抗锯齿模式、网格拟合全都无关。
  修法：原点上下对齐到 `supersample` 整数倍（负数按 floor 语义），并报出
  `GlyphBitmap::origin_x/origin_y`。
  **为什么必须报出内部值**：最初写的用例断言「渲染随笔位整数平移而平移」——把修复
  **临时回退**后它**依然全绿**（`CONVENTIONS.md §7.1` 警告的恒绿测试）。加诊断字段后
  `origin % supersample == 0` 才是可断言的抓手（回退后当场变红，已实测）。
  实测收益（同一口径，1.5× DPI）：与 Edge 的 `mean|Δ|` 13.5px 正文 0.345 → **0.295**（−14.5%）；
  实心像素均值 0.761/0.772（霜天/浏览器）→ **0.809/0.819**。
  量尺入 `tools/README.md`（含「画布 DPI 口径必须与实际一致」这条踩坑说明）。
- [x] **字重（FontWeight）从「只是个属性」到「落到像素」** —— 2026-10-04（`DESIGN.md §4.3.4`）
  缺陷：`Element::paint_text` **完全不读** `style_.font_weight`——界面里所有
  `SemiBold`/`Bold`/`Medium`（Heading/卡片标题/按钮/统计卡大数字/标签栏）与 `Regular`
  **逐像素相同**，标题不显眼、层次全靠字号与颜色撑。
  修法：合成加粗（Skia `setEmbolden`/FreeType `FT_GlyphSlot_Embolden` 取向），
  在**字形位图内部**按采样格平移叠填（不在贴图层做——那会让**灰度模式静默失效**，
  因为两种模式的采样格宽度不同）；换算链 `embolden_radius`（物理字号×档位比例）→
  `TextRenderer::embolden_steps`（按模式与超采样倍率换成采样格步数）；
  步数**进缓存键**（否则 Regular 与加粗命中同一份位图，症状是「一会儿生效一会儿不生效」）。
  幅度校准（`tools/text_weight_probe.cpp`，20.25px 中英混排）：Medium +17% / SemiBold +30% /
  Bold +42% 墨量，**峰值覆盖率不变**（这条判据把「变粗」与「变糊」分开）。
  参照：Chrome 切真 Bold 字体面是 +55%~+79%——背后是另一套轮廓，合成加粗不该追那个数。
  代价：稳态 **+2%**（一屏约 2000 字形；量这个数必须用同一渲染器连量多帧，
  反复新建画布会把缓存抖掉、量出 +264% 的假数）。
  测试：`tests/ui_text_weight_test.cpp` **走 `Element::paint` 真实路径**——
  缺陷在 UI 层，只测 `TextRenderer` 的用例在缺陷存在时依然全绿（已实测确认，
  回退后该用例变红）。
- [x] **网格拟合的护栏预算：小字号“发虚”的真因** —— 2026-10-04（`DESIGN.md §4.3.5`）
  起因：用户反馈「编辑器字体效果很好，UI 字体还差点意思」——
  同帧里等宽（代码）与比例（UI）走同一条 `TextRenderer`，唯一差别是字号与字体面。
  先把四个假设逐个量掉（等宽字体面更锐 ✗；拟合在 UI 字号是负收益 ✗——
  实测 12px 中文中间调占比 2.54→0.73，拟合是大正收益；浏览器做了色调映射 ✗——
  identity 即最优），最后落在：**宽度量化与位移护栏不自洽**。
  代码对笔画两侧**各自独立**判 `|delta| <= max_shift`（默认 0.5），而量化必然让两侧
  位移不等（远边额外叠上宽度取整量，最坏到 1.0）——近边通过、远边被拒，
  笔画被**平移了却没被改宽**：没拿到网格对齐，还把字形推歪。
  修法：① 两侧要么一起动、要么都不动；② 量化时 `budget = max_shift + grid/2`
  （半个像素是 `round` 的最大偏差，即几何必需量）；③ 新增 `quantize_width` 开关，
  `TextRenderer` 按**物理尺寸**分档（> 21px 关掉——UI 正文最大 14 逻辑 px × 1.5 DPI；
  大字号自己就有满黑像素，量化只剩墨量代价）。
  实测（ss=2）：CJK 13.5px 半覆盖像素 310 → **171**（−45%）、中间调占比 1.04 → **0.549**；
  拉丁 12px 半覆盖 243 → **114**（−52%）；22px 中文墨量回到 **+0.37%**（不关量化是 +3.5%）。
  端到端（真实 codeeditor 截图逐区域量）：侧栏 12px 半覆盖 **−9%**、实心 **+10%**、墨量 +1.7%。
  测试 `tests/text_fit_guard_test.cpp`（三条断言均经**回退验证**）。
  新增量尺：`fit_shift_probe.cpp`（扫 `max_shift` 取值）、`text_ink_conserve.cpp`
  （与超采样 8 档的真值比墨量）、`text_fit_by_size.cpp`（拟合逐字号收益）、
  `text_role_compare.py`（真实截图逐区域归一）。
- [ ] **文字渲染的后续打磨**（非阻塞；本轮已把三处确凿缺陷修完）
  ① **色调映射已量过、结论是不做**：ClearType/Skia 的 `a^gamma` + 对比度预混合，
     施加到霜天现有覆盖率上做 5×7 参数寻优（`tools/text_preblend_fit.py`），
     最优只比 identity 好 0.7~1.8%——提升全在几何。若将来几何再次变好，
     可以重跑这个脚本确认结论是否还成立（而不是重头再追一遍）。
  ② **横向子像素定位（Skia 的 1/4 像素量化）**：现在笔位取整到整数物理像素后贴图，
     字距因此有 ±0.5 物理像素的量化误差（浏览器用分档位图消掉它）。
     代价是字形位图缓存 ×4（或 ×3）；本机实测的**锐度**相位敏感度极低
     （见 `tools/text_phase_sensitivity.cpp` 的结论），所以收益主要在**字距均匀度**
     而非锐度——要不要做取决于「排版精度」是否成为下一个反馈点。
  ③ `embolden` 目前只外扩水平方向（Skia 亦如此）。CJK 竖笔画多的字形，
     纵向外扩没有对应处理——斜向观感是否够，待真机窗口目视验收。
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
  - [x] **状态系统高层原语（双宿主）**：C++ `memo`/`effect`/`ref`/`persisted`
    与 JS `useMemo`/`useEffect`/`useRef`/`usePersisted`（§4 的契约全部落实）。
    实现中挖出两个真缺陷（均由新回归测试拓住）：
    - **`memo` 命中缓存那帧丢了依赖订阅**：指纹只在“要重算”时登记依赖，于是
      命中缓存的一帧作用域“忘了”依赖 → 依赖下一次变化无人订阅，界面**静默停在旧值**
      （实测：`calls` 值停在 1 不动）。修法：指纹计算里一并 `subscribe()`。
    - **`useEffect` 里写的界面永远滞后一帧**：effect 原在首次重组末尾跑、但结果要等
      下一次 tick 才重组（实测 `log=` 而不是 `log=1`）。修法：重组末尾跑 effect +
      **≤4 轮收敛循环**（effect 里的写在本帧内收敛，又不会把一帧拖成死循环）。
  - [x] M1 元素工厂：`dsl::make_element`（27 个内置类型）。
  - [x] M2 C++ 宿主：`dsl.hpp/cpp` + 重组器（示例已随整合删去，语义由 `tests/ui_dsl_test.cpp` 钉住）。
  - [x] M3 JS 宿主：`declarative.js` + `ui::DeclarativeHost` 窄桥
    + 双宿主一致性 fixture（结构签名逐字节相等）。
  - [x] M4 补全：ArkTS 风格链式修饰（大写组件 + `.padding().onClick()`）、`useResource`
    异步状态（引擎 `pump_jobs` 泵微任务）、`ForEach` 按 key 复用（`__d_move` 重排）。
  - [x] **`custom<T>` 逃生舱**：任意组件 + 一等接口的通用入口（`type_name<T>` 特化表，
    工厂补齐全部 32 类型）；`icon`/`overlay`/`dsl::tabs` 数据驱动。
  - [x] **IDE 形态界面声明式重写**（2026-10-03 示例整合）：`examples/codeeditor` 整份界面
    改为声明式组装（五层布局 + 多标签 + 真实文件工作区 + 查找替换 + 菜单栏下拉 +
    命令面板 + 全局快捷键 + 终端 + 主题），与原本的 `examples/codeeditor-dsl` 分身**合并为一份**
    （两份合计 2758 行 → 1605 行，−42%）；端到端 `tools/codeeditor_e2e.py` 九项。
    同一批整合把 `counter` / `counter-js` / `todo-js` 三个小示例删除（语义已由单测完整覆盖）、
    gallery 新增「声明式」页（`mount_into` 子树挂载）；示例收敛为 gallery + codeeditor 两个。
  - [x] **子树挂载 `dsl::mount_into(host, component)`**：声明式树挂到既有元素的子位
    （`mount` 是单根语义，会替掉 `UiRoot::content()`）——宿主界面里的一页用声明式描述。
    整合中顺带修三个真缺陷：① 根作用域脏标记未清 → 根构建被跑两次、第二次**销毁重建根元素**
    （抹掉前一次 build 存下的元素指针，子树挂载一踩即非法访问）；② `ScrollView` 的滚动条是
    它自己的子元素，被声明式的末尾裁剪当成「残留」移除 → `bar_` 悬垂（引入
    `Element::content_child_count()`，对齐/裁剪只看得见调用方的子元素）；
    ③ `MenuBar` 把 `Click` 与 `MouseDown` 合在一个 case——一次物理点击触发两次
    `on_open_menu`（打开又立即切回关闭），菜单面板永远不出现。
  - [x] **浮层生命周期**：`overlay(c, key, ...)` / `menu_panel_overlay`——按 key 认领，
    未声明即回收（重组末尾 sweep）；`menu_bar` 数据驱动 + `Composer::register_shortcut`。
  - [x] **多作用域细粒度重组**：`sub_component` 子组件独立作用域（子状态变化只重跑子，
    父不重跑）；依赖按 scope 分流 + 宿主位置/子树游标恢复。
  - [x] **C++ 侧异步 `resource<T>`**：工作线程执行 + `pump_async` 回主线程写状态，
    输入指纹节流 + 代次计数丢旧结果。
  - [x] **构造期属性组件包装**：`dsl::select`/`dsl::table`/`dsl::tree`。
  - [x] **协议 `ui.create`/`ui.remove`**：在线建删元素（与声明式共用工厂）。
  - [x] 旧版 `examples/codeeditor` 编译修复（三处 `-Werror=shadow`）。
  - [x] **`SplitView` 分隔线偏移修正**（用户报修）：`paint_content` 里 `handle_rect()`
    返回的已是**绝对坐标**，旧代码又加了一次 `bounds_.x`——线整整偏出一个 `bounds_.x`，
    在 codeeditor 里表现为“标签下方一条穿过代码行号栏的竖线”（分栏 x=44 时线落到
    361.66，而两面板交界只在 314..322）。纵向分支、`grip_rect` 同错。
    回归测试 `ui_split_view_divider_paints_between_panes`：**宿主必须把分栏推到
    非原点**——`bounds_.x == 0` 时“多加一次”与不加等价，缺陷不会显形
    （第一版测试就落在原点，回退修复后仍然绿，是个假绿）。
  - [x] **codeeditor 标题栏的窗口控制未右对齐**：重写时把原实现的
    「`grow=true` 的空 Panel」误用成了 `dsl::spacer(c, 0.0f)`（固定 `width=0`，
    不参与弹性伸缩）——`— □ ×` 紧跟在标题文字后面（x=209）而非最右（x=1268）。
  - [x] **启动黑框消除**（真窗口）：`create_window` 原先 `CreateWindowExW` 后立即
    `ShowWindow`，而画布分配（含渲染器实测基准 ~590ms）与 DPI 尺寸调整还没做——
    用户看到「白区 1280×800 + 黑框」约 0.6 秒。改为**建窗不显 + 首帧画完才
    `show_when_ready()`**（`Backend` 新增接口，无头为空实现）；
    同时把 `auto` 选型改为**缩尺负载**（选型只需序关系，不需绝对帧耗时）——
    启动到可见 680ms → 440ms，露面即完整（黑占 0.00%）。
  - [x] **线程池**：固定工作线程（`hardware_concurrency` 夹取 1~8）+ 任务队列，
    替代「每任务一线程」（密集场景不再瞬间开几十个线程）；懒建——不跑异步的应用不付代价。
  - [x] **异步取消语义**：`AsyncCancel` 取消牌（≈ JS `AbortSignal`）——输入变化翻旧代的牌
    （fetcher 可在耗时点提前退出）+ 主线程写回前代次校验（旧代结果丢弃）；
    析构时翻全部牌并丢弃未开始的任务（不是死等）。
    **修正一个真 bug**：原实现的 `async_begin` 只换 token 不翻牌——旧代结果仍能通过校验写回。
  - [x] **嵌套作用域树**：作用域可任意嵌套（页 → 面板 → 叶），每层独立失效；
    父重跑后子树标脏（对齐）+ 未重新声明的作用域被剪枝（条件分支去掉了它）。
  - [x] **打磨轮：四个「文档写了、代码没跟上」的点（2026-10-04）**——均先探针实测、
    再做修复与回归，并逐个**反向验证**（回退修复看用例转红）：
    - **JS `forEach` 塞进 kids 数组 → 整段静默失效**：`column({}, [text('头'), forEach(...)])`
      的实际结果是顶层 **0 个元素、Text 计数 0**，日志只有一行 `__d_create 需要类型名`；
      同写法大写 `Column([ForEach(...)])` 正常。根因：小写 `forEach` 返回**裸数组**，
      而 `normalizeAll` 只摊平 `__fragment` 节点，数组被当成 VNode（`type === undefined`）。
      修法：数组也摊平（递归，兼容嵌套数组）。
    - **JS 事件每帧全树注销重绑**：实测三帧的绑定 id 为 `b1..b3 → b4..b6 → b7..b9`
      （codeeditor 整页声明式每帧在付这笔钱）。根因：复用分支无条件
      `releaseEvents(old)` + `ensureEventBinding(new)`，而回调闭包每帧重建。
      修法：绑定按**元素 id** 持有（`eventHolders`），宿主处理器只把事件转给「本帧的 VNode」
      ——绑定只在首次出现回调时登记、卸载时反注册。断言：跨帧绑定 id 逐项不变。
    - **C++ `for_each` 是半成品**：模板两个参数声明未用（任何真实调用
      `-Werror=unused-parameter` **编译不过**），且每帧重建整个 items 区段。
      修法：按 key 跨位置复用（命中 key 就把元素挪到当前游标位）。
      实现中踩到一个必须记住的坑：新建项顶到游标位时，那个位置上的兄弟可能只是
      **「还没轮到」**（它的 key 在本区段里，待会儿会被复用）——不分清这点就会把它当
      残留销毁（实测：头插一项后其余项全部重建、id 漂移）。故 `for_each` 先交**整份
      key 集合**给重组器（`begin_keyed_region`）再逐项声明。
    - **两块护栏只有文档没有实现**：`Guardrails::max_depth` 全仓**无任何读取点**
      （递归 build 实际是撞栈，实测去掉判断即 0xC00000FD）；`frame_budget_ms` 只事后置标志。
      修法：深度超限**拒绝声明** + 写 `stats.error`（配谬误注入用例）；预算按作用域计，
      超了停手、剩下的**留在树上顺延下一帧**（`dirty()` 仍为真，不是丢弃）。
  - [x] **JS 场景卸载**：`compose()` 再挂一页会**叠在旧页下面**、hook 槽从旧页第 N+1 个往后取
    （新页第一个 hook 拿到旧页缓存）、被卸掉界面的 effect 清理永不跑。
    修法：`compose` 先卸整层（元素树 + hook 槽 + 事件登记）；新增
    `DeclarativeHost::unmount_declarative()`（析构只释放桥与引擎，不会回 JS 跑清理）。
  - [x] **桥名纪律**（写 JS 运行时入口时必须知道）：宿主窄桥与本层入口同处一个全局命名空间，
    **本文件的同名赋值会盖掉宿主桥**。实测踩到：整棵卸载曾取名 `__d_unmount`，于是
    `unmountVNode` 里那行 `__d_unmount(node.el)` 变成对「整棵卸载」的递归调用
    ——条件分支收不回、换页叠树、绑定计数归零（四处用例同时红）。现已改为 `__d_dispose`
    并在源文件里立了「新增全局名前先查宿主桥」的注释。
  - [x] **双宿主一致性 fixture 扩展**（四组）：静态结构 / 状态推进 / 列表与条件裁剪 /
    key 身份跨头部插入保持。
  - [ ] **`for_each` 的 item 级子作用域**：现在每个 item 不建独立作用域（避免为简单列表
    逼用户把 item 抽成组件）；需要 item 级细粒度失效时用 `sub_component`。
  - [ ] **JS 侧细粒度重组**：JS scope 粒度仍是根组件（依赖收集已就位）——
    C++ 侧已有作用域树，JS 侧待对齐（否则大页面任一状态写都重跑整树）。
  - [ ] **`for_each` 的 key 重排代价**：现在按 key 逐个「找到→挪位」（每项一次
    `remove_child` + `insert_child`）；大量重排（如整表排序）是 O(n × 子元素数)。
    量级真成为瓶颈时再换「先算目标序、再一次遍历重排」。

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
- [x] **命令面板通用组件**（2026-10-03 落地）：`ui::CommandPalette`（FillViewport 遮罩 +
  顶部居中卡片 + 过滤列表 + 键盘环绕导航 + Esc/点遮罩关闭 + `query`/`command_count`/
  `match_count`/`active` 属性面与 `activate`/`select` 动作面）。codeeditor 示例已迁移
  （退役 130 行私有实现），并新增 `grab_focus()`——**打开面板后必须调它**，否则全局
  快捷键只把面板显示出来、焦点仍在底层编辑器上（敲的字跑进代码里）。
  迁移中把鼠标链路也提上来了：点条目走数据层 `List::Entry::on_activate`
  （与 Enter 共用 `run_command`），不再与 `List` 的“点击即选中”争同一个槽位。
- [x] **单行 Input 动作面**（2026-10-02 落地）：`invoke submit/activate/clear` 与 TextArea 对齐；
  测试 `ui_input_invoke_*` 3 用例 + `tools/input_action_e2e.py` 真实应用端到端。
  - [ ] **虚拟化长列表**：终端/输出面板的 ScrollView+Text 累积全文，日志长了退化；
  - [x] **桌面窗框：自绘标题栏（跨平台无差异）**（**2026-10-04 落地，Win32 先行**）：
    ① **平台层去装饰建窗** ✅：`WS_POPUP|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX`（去
    `WS_CAPTION`/`WS_SYSMENU`，**保留** `WS_THICKFRAME`），`WM_NCCALCSIZE` 客户区覆盖整窗，
    `WM_NCHITTEST` 用 `ui::resize_edge_at` 接管八向缩放与拖动（**Aero Snap 随之保留**——
    它就是挂在 `WS_THICKFRAME` 上的）；最大化在 `WM_SYSCOMMAND`/`WM_SIZE` 两处按 `rcWork`
    校正（**不盖任务栏**）、`WM_GETMINMAXINFO` 给最小尺寸护栏（否则能拖到窗框按钮被裁、
    且再也拖不回来）；
    ② **`st::shell` 窗口控制接口** ✅：`supports_window_control/minimize/toggle_maximize/
    request_close/begin_move/begin_resize/maximized`（契约方言“不支持就如实报 `Unsupported`”），
    与 `ui::WindowEdge`/`resize_edge_at` 判定（**定义住 ui 层**：`shell.hpp` 已包含 ui 头，
    反向成环；而两端都要用这份判定）；
    ③ **窗框做成框架组件** ✅：`ui::TitleBar`（图标/标题/三按钮/拖动/双击最大化 + **附属槽**
    `add_leading`/`add_trailing`：菜单·主题·DPI 这类宿主控件本就长在标题栏那一行）
    + `ui::WindowFrame`（**组件化的窗口**：标题栏置顶 + 内容槽 + 八向缩放边缘条；
    它解决的是通用缺陷——无边框窗口**只能**靠界面提供拖动/缩放区域，只有 TitleBar 时
    窗口拖不动）+ `ui::WindowControl` 端口（依赖倒置，与 `TextPort` 同一手法）+ 应用装配
    （`Application` 实现端口并转发后端）；**两个示例都已换壳**：codeeditor 的 `#titlebar`
    换成窗框内置标题栏（旧三个装饰图标退役），gallery 的“60px 顶部栏 + 无标题栏外壳”
    换成 `WindowFrame`（品牌名挂前部槽、当前页名作标题、主题/DPI/截图挂尾部槽）；
    gallery 组件页保留「窗框」巡检卡；
    ④ **x11/wayland** ⏳ 仍待补（当下是探测 + `Unsupported`，`supports_window_control()` 返回
    `false`、动作如实拒绝）；补的活只剩“起无装饰窗口 + 实现那几个动作”，**组件与应用无需改动**。
    顺带清账 ✅：`WindowOptions::resizable` 那个**静默失效字段**已接上（`false` → 不带
    `WS_THICKFRAME` 且边缘不参与缩放命中），并新增 `decorations`（默认 false = 自绘）；
    新增 `fill_width/fill_height`——“铺满父级”的组件不能拿行布局的 `max_width`（1e9）当尺寸。
    验证：`tests/ui_title_bar_test.cpp` **18 用例**（判定纯函数 3 + 几何/命中/动作面/无宿主降级
    + 附属槽 2 + 窗框容器 4 + 还原图标 1）、`tools/title_bar_probe.py`（无头，
    **动作如实拒绝**）、`tools/title_bar_win_check.py`（真窗口：**客户区 == 窗口矩形**、
    最大化 == 工作区、三动作生效、`--decorations` 对照分支）、`tools/gallery_frame_shot.py`
    （示例换壳几何取证）；`st test` 667 用例全绿 / lint 0 违规 /
    mingw 交叉编译通过 / `st_visual_check.py` 全序列 0 失败步。
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
