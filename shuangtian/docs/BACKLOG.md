# 霜天 Backlog（待办唯一权威清单 + 已完成考古层）

> 本文件是**待办的唯一权威清单**：未完成项全部集中在 `## P0` / `## P1` / `## P2` 三章内，
> 完成后移入「已完成」段并在 `DESIGN.md` 更新里程碑。
> **诚实原则**：写着「待做」却已完成的条目会误导读者；写着「已完成」却没落地的条目更糟。
>
> **结构约定（2026-10-09 整理，别再散开）**：
> 1. **未完成项一律写成 `- [ ]` 列表项**，且必须位于 `## P0`/`## P1`/`## P2` 章内。
>    历史章（「已完成（…）」「（某轮）新发现」）里**不得再放**新待做——
>    散在历史章里的待做项肉眼扫不到，等于不存在（本轮实测：39 条散落在 5 个章下）。
> 2. 优先级章名 **`P0`/`P1`/`P2` 是接口**：源码注释按它们引用
>    （如 `include/st/app/app.hpp` 的「见 `docs/BACKLOG.md` P1」）——**不要改名**。
> 3. 条目写完在下面留一句「为什么值得做 / 已量到什么」，否则下个人得重查一遍。
> 4. 自检：`python tools/check_docs.py` 会把「待做项落在优先级章之外」报成问题。

> 📋 2026-10-09 **「镂月裁云」两个示例已实现**：`examples/louyue`（图像编辑器——像素/光栅
> 创作面）与 `examples/caiyun`（视频剪辑器——时间/动效应用面）。权威现状见 `README.md`
> 「示例」节与 `DESIGN.md` §8.1；实现中反推出的框架缺口已进下方 P1/P2。

> 📋 2026-10-08 终端能力修复轮（光标错位/输入回显/多会话串台等 15 项）已完成，
> 详见 `docs/TERMINAL_FIXES_20261008.md`（根因表 + 修复对照 + 测试策略）。

> 📋 2026-10-08 续：上述修复轮**漏了真窗口的输入契约**——用户报「终端输入一个字符回显了 2 个」。
> 根因与修复见下方「已完成」段（含 `tests/ui_terminal_input_test.cpp` 与
> `tools/terminal_input_reverse_verify.py`）。

> 📋 2026-10-18 **gbcode 真正可用轮（实测驱动，9 项缺陷 + 3 项能力缺口）**：
> 新增 `tools/gbcode_usability_e2e.py`（真实磁盘工作区 + 真 git 仓 + 真 shell 的
> 使用者路径走查，≈60 条断言）——它钉住的正是“功能面全绿但用户照样翻车”的那些点。
> 查询全文见下条「已完成」章（或 grep `2026-10-18 gbcode 可用性`）。
> 一句话结论：**实测比读代码有效**——本轮 9 个缺陷里，8 个只有真跑才看得见
> （BOM/CRLF/Shift+Tab/终端忙闲/二进制写坏/逐行暂存只删不加/按钮恒灰/树行不可寻址）。

> 📋 2026-10-18 续：**“打开 DESIGN.md 整页变卡”**（用户报告）已定位并修复。
> 根因不在 Markdown 也不在语法高亮，而在**文本宽度缓存的淘汰策略**：
> `measure_width` 曾是 `if (size > 4096) clear();`，而 4096 与“正常文件的行数”同量级；
> 5 千行文档于是持续落在“全清 → 全部重新整形”的抖动区。
> 实测：单帧 paint 从 4.5ms 涨到 **90ms**，打字 86.7ms/字符；
> 修复后 12.7ms / 15.5ms（**约 7×**），且不再随行数增长。
> 同时把 `max_line_width` 从几何重建里拿出（改惰性 + 世代缓存）。
> 回归：`text_width_cache_evicts_incrementally_not_all_at_once`（命中率判据，
> 已做过**反向验证**——改回全清实现即时变红）+ `code_editor_edit_frame_cost_…`
> + `gbcode_usability_e2e.py` 里的“5 千行编辑帧 < 40ms”断言。

> 📋 2026-10-18 再续：**“Markdown 预览滚一点就大片内容没渲染”**（用户截图报告，
> 也是本轮最严重的一个）已定位并修复。
> 根因：代码块/引用块的**组背景**（跨整组的不透明大面板）与文字曾在**同一个
> 逐行循环**里画。代码块首行滚出视口后，旧代码对**每一行**都重画整块背景——
> 行 2 的背景把行 1 的字盖掉，依次类推，屏幕上只剩最后一行那点字
>（真机截图：30 行 ASCII 图代码块只剩底部一条框线，内部全空）。
> 修法：背景是**比行更底的一层**——先把本帧可见组的背景全部画完，再逐行画文字
>（两遍只对可见组跑，代价可忽）。
> 顺带堵上两个可观测性缺口：`MarkdownView` 属性面新增 `first_visible`/`last_visible`/
> `draw_stats`/`painted_groups`——排查时最关键的信息是“行到了但字没画”
>（`drawn=21` 而屏幕全空），没有这对数只能靠截图猜（当时查了十几轮）。
> 回归：`md_code_block_text_survives_scrolling`（判据选型踩了两次坑：
> 统计背景像素测不出、数 `draw()` 次数也测不出，最后用**绘制次数结构性约束**
> `painted_groups ≤ 组数`；已做**反向验证**——改回逐行重画即时变红）
> + `gbcode_usability_e2e.py` 里的三档滚动断言。

> 📋 2026-10-18 三续：**“Ctrl+P 文件查找面板配色不对”**（用户截图）已修复。
> 表象是暗色界面里输入框发白、选中项反而更暗，看着像“亮底套暗底”。
> 根因两层，都属“**缓存/字段算对了但没落到真值上**”这一类：
> ① `CommandPalette::apply_theme` 只把主题色算进 `card_bg_` / `card_border_`
>   **成员字段**，而绘制读的是 `card->style().background`——字段与画面分家，
>   切主题时卡片颜色纹理不动。修：存下 `card` 指针，把色**写回 style**。
> ② `Input` **没有** `apply_theme`，底色/描边只在 `measure` 里做一次
>   “未设则填”（`a == 0` 才写）。首次填完就永不更新，`style().background`
>   永久停在首主题。而 `paint_content` 直读 `colors.surface`，所以**屏幕看着对、
>   语义树/`visual`/控制通道快照是旧的**——嵌在暗色面板里的输入框便显错。
>   修：补 `apply_theme`，用“上次是我写的值”判据保住“显式设置优先”。
>
> ⚠ **踩到的两个坑（写下来免重犯）**：
> ● 前几轮复现都没传 `--theme dark`，而 gbcode 默认是 light——测的其实是浅色！
>   症状就变成“面板是暗的、按钮说亮色”，白查了好几轮。**复现用户场景先对轴环境**。
> ● 给 `apply_theme` 加 `mark_dirty()` 会让主题传播每帧标脏，把增量重绘整体作废
>   （`script_write_takes_partial_damage_path` 当场变红）。主题变化的标脏归
>   `UiRoot::set_theme → mark_dirty_all()`，组件内不要自己标。
>
> 同时做的加固（**诚实记录：它没修上面那个 bug**）：`UiRoot::layout` 里的
> `apply_theme_tree` 移到了 `dirty_/tree_layout_dirty()` 守卫**之前**。
> 反向验证证明回退它面板颜色依旧正确——保留是因为它把一条隐式耦合（“新元素能否
> 拿到主题取决于当帧恰好有没有别的东西在要求重排”）改成了显式契约。

> 📋 2026-10-18 四续：**配色体系系统性重整**（用户：“按 VSCode / IDEA 之类的主流 IDE 设计”）。
>
> ## 实测到的两个具体病
>
> | 症状 | 实测数据 |
> |---|---|
> | 资源管理器列表底色与面板不统一 | 侧栏容器 `(8,8,10)` vs 列表 `(21,21,26)`（1.19）——而侧栏容器没声明 `Surface`，落到了页面 `bg` 上 |
> | 编辑器不够下沉、与 UI 无区分 | 编辑器 = 页面 `bg` = 标签条，两两对比度 **1.000**（完全同色） |
>
> 根源：**只有活动栏/状态栏声明了 `Surface::Alt`**，侧栏/编辑器/终端/标签条全都透明。
>
> ## 新体系（对齐 VSCode Dark+ / IDEA Darcula / Light+）
>
> | 语义 | 暗色 | 亮色 |
> |---|---|---|
> | 外壳（标题/活动/状态/标签条） | `#2A2A33` 最亮 | `#EEEEF3` 压灰 |
> | 侧栏/面板 | `#1E1E24` | `#FAFAFC` |
> | **编辑内容区** | `#0D0D10` 最沉 | `#FFFFFF` **最亮** |
> | 凹槽（输入框/代码块） | 再深一档 | 再沉一档 |
>
> ⚠ 关键设计发现：**两个模式的“围白方向”相反**。暗色里内容要往内沉，
> 亮色里白到顶了、只能让**外壳压灰**把内容托起来。单一 `Surface::Sunken`
> 无法同时服务两者——若强行用，亮色下编辑器会发灰显脏。
> 因此新增语义位 **`Surface::Content`**，它在两个模式**解析到不同的 token**
>（暗→`surface_sunken`，亮→`surface_raised`）。
>
> ## 踩到的坑
>
> 调色不是自由发挥——`ui_theme_test.cpp` 已经锁了一套 WCAG 与阶梯护栏，
> 改一个值就会连锁触发 4~5 条断言。真实约束是个**不等式方程组**（举例）：
> `sunken < bg`、`c(surface,bg) ≥ 1.08`、`c(subtle,alt) ≥ 1.10`、
> `subtle < border < strong`（深色下“更淡 = 更暗”）、`faint ≥ 4.0` 对三档同时成立。
> 靠“看着调”必然撞墙，我是把候选值丢进脚本算对比度、二分出可行区再去改代码的。
>
> 回归：`theme_content_surface_is_distinct_from_shell_and_panel`
> + `theme_content_surface_resolves_differently_per_mode`（已反向验证：
> 把 `Content` 退化为单一 `sunken` 后亮色断言立即变红）。

> 📋 2026-10-18 五续：**gbcode 从演示转为正式使用：清掉演示壳**。
>
> 用户的诉求：“纯演示的功能和代码都去掉，要正式使用了”。
> 判定标准：**数据从哪里来**。从磁盘来 → 真功能；凭空造 → 演示。
>
> ## 删掉的
> | 项 | 量 | 为何算演示 |
> |---|---|---|
> | 内置样例工作区 `samples()` | 165 行（5 份假文件） | 无 `--workspace` 时拿内存假数据把界面填满 |
> | `open_sample` / `open_by_name` | 2 个函数 | 只能打开上述假文件 |
> | `open_stlog()` + `kStlogSample` | 1 个入口 | 凭空造一份不存在的 `stlog.log` |
> | `assets/logic.js` + 启动装载 | 40 行 JS | 每 2 秒刷心跳、改写状态栏 |
> | 资源管理器 `#sample-tree` 分支 | 1 段 | 假目录树 `logs/app.stlog` |
> | 命令面板 `file.open.<名>` / `lang.stlog` | 2 条 | 指向假文件的入口 |
>
> ## 保留但改口径的
> - `register_custom_language()`（stlog 语言）——**保留**：它注册的是真语言规则，
>   用户打开真实 `.stlog` 文件就能高亮。演示的是“能力”不是“数据”，而能力是产品特性。
> - `--enable-script` 控制通道——**保留**：AI 读写界面是产品能力；只是不再自动跑内置脚本。
> - `untitled-N` 内存缓冲——**保留**：VSCode/IDEA 同款（先写后存），不是演示占位。
>
> ## 行为变更（用户可感知）
> 1. **启动不再自动打开文件**：过去无参数启动会装一份样例，现在空启动就是空编辑器。
> 2. **`--workspace` 缺省 = 当前目录**（不再回退内存样例）。
>
> ## 踩到的坑
> “是否恢复上次会话”原本靠 `options.workspace.empty()` 判断——而缺省值改成当前目录后
> 这句会**永远为真**，导致无参数启动不再恢复会话。必须区分“用户显式指定”与
> “事后填的缺省值”：引入 `explicit_workspace` 单独记意图。
>
> ## 同步修的回归资产（删数据会连带弄红它们）
> - `tools/gbcode_e2e.py`：新增 `make_workspace()` 自建真临时工作区（a_renderer.cpp / b_deploy.py），
>   并改断言：空启动**应无** `#editor`（`has_editor = !buffers.empty()`），从资源管理器点开后再查。
> - `tools/gbcode_e2e_session.py`：标签集从 3 个变 2 个，Ctrl+Tab 按一次就回归（旧断言写死两次）。
> - `gbcode_blame_e2e.py` / `gbcode_usability_e2e.py`：本就自建工作区，无需改。
>
> 验证：`st test` 全量 0 失败；`gbcode_e2e` / `gbcode_e2e_session` / `gbcode_blame_e2e` /
> `gbcode_usability_e2e` 全绿；`check_docs` ok。

>
> ## 实测到的两个具体病
>
> | 症状 | 实测数据 |
> |---|---|
> | 资源管理器列表底色与面板不统一 | 侧栏容器 `(8,8,10)` vs 列表 `(21,21,26)`（1.19）——而侧栏容器没声明 `Surface`，落到了页面 `bg` 上 |
> | 编辑器不够下沉、与 UI 无区分 | 编辑器 = 页面 `bg` = 标签条，两两对比度 **1.000**（完全同色） |
>
> 根源：**只有活动栏/状态栏声明了 `Surface::Alt`**，侧栏/编辑器/终端/标签条全部透明。
>
> ## 新体系（对齐 VSCode Dark+ / IDEA Darcula / Light+）
>
> | 语义 | 暗色 | 亮色 |
> |---|---|---|
> | 外壳（标题/活动/状态/标签条） | `#2A2A33` 最亮 | `#EEEEF3` 压灰 |
> | 侧栏/面板 | `#1E1E24` | `#FAFAFC` |
> | **编辑内容区** | `#0D0D10` 最沉 | `#FFFFFF` **最亮** |
> | 凹槽（输入框/代码块） | 再深一档 | 再沉一档 |
>
> ⚠ 关键设计发现：**两个模式的“围白方向”相反**。暗色里内容要往内沉，
> 亮色里白到顶了、只能让**外壳压灰**把内容托起来。单一 `Surface::Sunken`
> 无法同时服务两者——若强行用，亮色下编辑器会发灰显脏。
> 因此新增语义位 **`Surface::Content`**，它在两个模式**解析到不同的 token**
>（暗→`surface_sunken`，亮→`surface_raised`）。
>
> ## 踩到的坑
>
> 调色不是自由发挥——`ui_theme_test.cpp` 已经锁了一套 WCAG 与阶梯护栏，
> 改一个值就会连锁触发 4~5 条断言。真实约束是个**不等式方程组**（举例）：
> `sunken < bg`、`c(surface,bg) ≥ 1.08`、`c(subtle,alt) ≥ 1.10`、
> `subtle < border < strong`（深色下“更淡 = 更暗”）、`faint ≥ 4.0` 对三档同时成立。
> 靠“看着调”必然撞墙，我是把候选值丢进脚本算对比度、二分出可行区再去改代码的。
>
> 回归：`theme_content_surface_is_distinct_from_shell_and_panel`
> + `theme_content_surface_resolves_differently_per_mode`（已反向验证：
> 把 `Content` 退化为单一 `sunken` 后亮色断言立即变红）。


> 📋 2026-10-18 性能专项（用户追问“类似的性能问题排查了吗”）——**补做完的**：
>
> | 项 | 结论 |
> |---|---|
> | 查找命中高亮每行扫全部命中 | ✅ 已修：100 万命中时编辑帧 **74.2ms → 28.6ms**（分桶后只遍历本行） |
> | `Tree::measure` 稳态仍逐行求最宽 | ✅ 已修：**只缓存逐行宽不够**，求最大值那步仍 O(总行数)。现在连最宽值一起缓存，稳态 O(1)（回归：15 倍行数差下耗时比 < 4×，已做反向验证） |
> | 编辑器高亮/几何的规模敏感度 | ✅ 健康：1k → 8k 行（C++ 高亮）编辑帧 9.7 → 14.7ms（**1.5×**），纯文本 8.9 → 10.8ms（1.2×） |
> | Markdown 预览的规模敏感度 | ✅ 健康：打开 101 → 158ms（1.57×，块数 360 → 2880）、滚轮 6.5 → 7.0ms（1.08×） |
> | `Tree` 标签宽缓存失效判据 | ✅ 已验无缺陷（`sync_nodes` 全量重建）；补了一条用例锁住“行数不变但标签变”这条路径 |
>
> **方法沉淀**：这类问题靠读代码只能看出“是 O(n)”，必须用**两规模对照测量**
>（同内容 1×/8×）把“成本随规模增长”变成可见数字。三个静态扫描口径：
> ① 缓存淘汰是否“全清”；② 每帧/每次编辑路径上的全量循环；
> ③ **缓存命中后是不是还有一趟全行扫描**（`Tree` 就是栽在③，前面 `CodeEditor` 也是）。

> 📋 2026-10-18 六续：**分割线改成真正的“一条线”**（用户：“分割线显示一条线就行，不要有填充宽度和边框” + “拖动区域太小了，不好点”）。
>
> ## 实测的病
> 侧栏右边界逐像素（dpi 1.5）：
> ```
> 侧栏 (30,30,36) │ 4px (17,17,20) │ 1px (76,76,84) │ 4px (17,17,20) │ 编辑区 (13,13,16)
> ```
> 约 13px 的“条块”夹着一条线 = 用户说的“有填充宽度和边框”。
>
> 根因：`SplitView` 的手柄占 **8px 布局空间**，而它自身 `background` 全透明——
> 那 8px 露出的是**页面底色**，与两侧面板色都不同。
>
> ## 修法（用户选的方案 A）
> 手柄**不占布局空间**：两侧面板直接相邻，手柄矩形以分隔线为中轴向两侧各张一半，
> 纯命中区。于是视觉上只剩那条 1px 线，而命中区可以给到 **12px**
>（比旧 8px 更宽、更好点）——**两者不再互相牵制**。
>
> ## 改这里时踩到的两个真问题
> **① 手柄重叠后拖拽完全失效。** 事件派发是“子元素优先”，手柄与面板几何重叠时
> 永远被面板抢走。这不是测试写法问题——真机侧栏彻底拖不动。
> 修法：给 `Element` 加钩子 `claims_hit_before_children(point)`（默认 false，
> 行为与既有一致），`SplitView` 覆写为“在手柄内则自己先拿命中”，
> 并让 `hit_test` **只在手柄矩形上**返回真（否则会把整个分栏里的按钮全吃掉）。
>
> **② 8 个既有用例锁住了旧几何。** 它们的断言是 `a+b+handle = total`、
> `usable = total - handle`——几何语义变了就必须同步改，而不能放宽断言蒙过去。
> 同时删了 e2e 里写死的坐标公式 `(height - 8) * ratio + 4`：改为从
> 组件的**只读属性 `handle_size`** 取真值（默认值一改就静默抓不到，实测即如此）。
>
> 回归：`split_view_divider_is_a_line_and_handle_is_wide_enough`
> + `split_view_handle_wins_hit_over_overlapping_children`。
> **已反向验证**：把布局改回“手柄占空间”，前者立即红 5 条（差值精确指出 12px 的缝）。
> 实测结果：过渡带由 8~13px 降到 **1px**；拖拽 ratio 0.2 → 0.297 正常。

> 📋 2026-10-18 七续：**Windows release 不要控制台 + exe 图标**（用户：“要 WinMain 链接，不要控制台窗口，exe 文件图标也要有”）。
>
> ## 实测确认
> ```
> build\dev\bin\gbcode.exe
> Subsystem = 3 (CONSOLE)   ← 双击必弹黑框
> 节: [.text .rdata .buildid .data .pdata .tls .reloc /4]   ← 没有 .rsrc，图标缺失
> ```
>
> ## 关键发现：**不需要写 WinMain**
> `startup_arguments()` 走的是 `GetCommandLineW` + `CommandLineToArgvW`，
> **不依赖控制台的 `argc/argv` ——所以只要链接加 `-mwindows`，现有 `main()` 照常工作。
> 很多项目为转 GUI 手写一层 `WinMain` 转发，多一套入口就多一处不一致的可能。
>
> ## 做了什么
> 1. `TargetSpec` 新增两个字段（**通用能力，不只 gbcode**）：
>    - `system: "console" | "gui"`（缺省 console，**不能默认 gui**——工具链 st 与探测程序
>      要在终端里跑并读输出）
>    - `icon: "path.ico"`
> 2. `build.cpp`：gui 时加 `-mwindows`；有 icon 时生成 `.rc` → 编成 `.o` → 链进产物。
> 3. `tools/make_ico.py`：PNG → 多尺寸 ICO（16/24/32/48/64/128 DIB + 256 PNG 内嵌）。
>    ≤64 用 DIB 而非 PNG 内嵌：Windows 在小组尺寸上对 PNG 内嵌有历史坑。
> 4. `entry.cpp`：GUI 构建下 `AttachConsole(ATTACH_PARENT_PROCESS)` 把输出接回父控制台，
>    使 `gbcode --help` 在 cmd 里仍可见。
>
> 结果：`Subsystem = 2 (WINDOWS_GUI)`、`.rsrc` 含 `RT_ICON` + `RT_GROUP_ICON`。
>
> ## 踩到的三个坑（都是“静默失败”，值得写下来）
> **① `full_args` 是 `args` 的快照，拼好后再 `args.push_back` 等于丢弃。**
> 我把 `-mwindows` 追加到 `args`，而 `full_args` **早在上面就拼好了**——旗标静默丢失，
> 不报错也不警告。顺带发现：**Windows 静态链 C++ 运行库（`-static-libgcc`）
> 长期也有同样的问题**（它在快照之后）。一并修了。
>
> **② `g++ x.rc -o x.o` 默认按链接处理，必须加 `-c`。**
> 不加就是 `collect2.exe: error: ld returned 1 exit status`——报错指向链接器，
> 而真因是“编译器把 `.rc` 当输入文件去链”。
>
> **③ 一直在用旧的引导 `st`。**
> `build\bin\st.exe`（10-17）与 `build\dev\bin\st.exe`（新）是两个东西。
> 改构建系统后必须用**后者**才能生效，否则你会对着一堆“改了却没反应”白查。
>
> ## 一处我自己的缺陷（写完才发现）
> 第一版无条件 `freopen("CONOUT$")` 接管控制台——那会**弄坏重定向**：
> `gbcode --help > out.txt` 失效（输出跑控制台去了）。
> 改为**重定向优先**：`stdout` 句柄已经有效（文件/管道）就不插手，只在本无控制台时借父控制台。
> 实测：重定向 1838 字节正常落盘。
>
> 回归：`manifest_target_system_and_icon_are_parsed`（9 条，含“拼错的 system 必须报错
> 而不是静默回退到 console”）。**已反向验证**：改成静默回退后该用例立即变红。

> 📋 2026-10-18 八续：**鼠标指针随内容变化**（用户：“鼠标指针要随内容变化”）。
>
> ## 改之前
> 框架**完全没有**光标机制：`platform_win32.cpp` 把窗口类光标设成固定箭头
>（`IDC_ARROW`）后全程不再改动。编辑器正文、分隔线、按钮全是一个箭头，
> 用户无从判断“这块能不能点/能不能拖/能不能打字”。
>
> ## 链路（六层）
> 1. `ui::MouseCursor` 语义枚举（**语义名，不是平台常量**）+ `mouse_cursor_name`
> 2. `Element::set_cursor` / `cursor_at(point)` —— 后者允许**按位置细分**
> 3. 各组件声明：Button/ListItem/Tree 行/Tabs 标签 → Hand；Input/TextArea/
>    CodeEditor 正文 → Text；SplitView 分隔线 → ResizeH/V
> 4. `UiRoot::refresh_cursor` 逐级向上回退解析
> 5. `Backend::set_cursor`（默认空实现，headless 天然忽略）
> 6. Win32：`WM_SETCURSOR` + `SetCursor`
>
> ## 三个设计要点
> **① 逐级向上回退是核心。** `cursor_at` 返回 `Arrow` 有两种含义：
> “我这里就要箭头”与“我没意见”。不回退的话，一个没声明光标的容器会把它
> 里面按钮的 Hand 盖成箭头。回退让**只有最具体的那层**说了算。
>
> **② 必须处理 `WM_SETCURSOR`，只在 `WM_MOUSEMOVE` 里设是没用的。**
> Windows 每次鼠标移动后都用窗口类默认光标覆盖 `SetCursor` 的效果。
> 症状正是“我设了光标，动一下就变回箭头了”。
>
> **③ 拖拽期间光标要锁住。** 拉分隔线时指针很容易跑出那 12px 手柄；
> 若每帧重解析，光标会在 Resize 与 Arrow 之间抖动——用户正在拖的东西“看起来丢了”。
>
> ## 一个刻意的取舍
> `Text` **默认不改光标**：只有输入区（Input/编辑器/终端）用 I-beam，
> 静态正文保持箭头。理由：把整页文字都变成 I-beam 是浏览器的行为，IDE 不这么做。
>
> ## 光标怎么验收（它截图看不见）
> `metrics` 方法多报一个 `cursor` 字段（语义名）。实测结果：
> ```
> 编辑器正文    -> text
> 编辑器行号栏  -> arrow（不可编辑，显 I-beam 会误导）
> 分隔线        -> resize-h
> 树行 / 按钮   -> hand
> 空白处        -> arrow
> ```
> 6/6 全对。回归：`cursor_resolves_per_element_and_falls_back_upward`
> + `cursor_names_are_stable` + `split_view_cursor_is_resize_only_on_the_handle`。
> **已反向验证**：去掉逐级回退，回退断言立即变红。

> 📋 2026-10-13 **gbcode 实测缺陷清账轮（P1 九项全清 + 五项计划外）**：
> 上一轮（f6857e7）实测出的 9 条「确认未修」全部修完（每条见下方 P1 章内的「修复」注记）：
> 问题计数刷新（指纹收敛 + problems_revision_ 渲染信号）、glob 提示文案、菜单键位/hover
> 死代码、scm-refresh 撞 id（工具条改名 + **框架层 UiRoot::find 可见优先**）、invoke 补
> 顶层 error 字段（DESIGN 协议表同步）、run_shards 崩溃分片显式报错、--scale/--frames
> 非法值 exit 2（不再 abort）、clangd 不就绪（真根因：`--workspace` 从未调
> set_workspace——与条目推测的 ensure_started/which 无关，实测钉死）。
>
> **计划外修出五个真缺陷**（都在「Linux 首次实编/实跑」路径上现形——本仓此前只在
> Windows 上验证，这正是 BACKLOG 自举轮「能在另一平台实跑才算验过」的再次应验）：
> ① `platform_tls_openssl.cpp` 宏续行符后跟注释（stray '\' 编不过）+ Error 构造
> 形状错（10 处）——**该文件自 2a64fd9 加入起从未在任何平台被编过**；
> ② `resolve_system_libraries` 把跨平台并集原样交链接器（Linux 上链 -ldbghelp 失败），
> 改为按目标平台过滤（link-libs.txt 两节为准，工程自声明库保留）；
> ③ mingw 交叉档 NTDDI_VERSION 低于 ConPTY 门控值（HPCON 未声明）→ 0x0A000006；
> nanosrv 只在本机 MSYS2 需要，交叉档去掉（cannot find -lnanosrv）；
> ④ **e2e 会被会话恢复污染**：gbcode_e2e.py 裸启动恢复了上一轮手工验证的
> /tmp 工作区（第 3 步落空）——start() 注入 ST_HOME（session e2e 早有先例）；
> ⑤ 9 个 gbcode e2e 的二进制路径写死 `.exe` / PowerShell 语法 / 退役的
> #terminal-input 钩子——全部跨平台化对齐（Linux 首跑即现形）。
>
> **验证**：`st test` 单进程 324+ 与 16 分片全绿（无「未能启动」）；gbcode 11 个 e2e +
> events/input_action/visual_assert/partial_patch/gallery 全过；mingw 交叉 gbcode 通过；
> `--scale abc` exit 2；真 clangd 全链路（打开→诊断→计数）探针通过；lint 无新增。
> 另：本轮曾因容器 OOM 重跑 bootstrap.sh（ST_JOBS=8）重建了 build/bin/st。

## 待做（2026-10-09 起）

### `present` 时出现「画布有未提交的绘制命令」（2026-10-26 发现，方向已修正）

**现象**：真窗口跑 gallery 时，日志里出现过：

```
error GPU 呈现：画布有未提交的绘制命令（漏了 submit_deferred_locked）
warn  swapchain 呈现失败（present 被跳过了）→ 回退 GDI blit
```

**触发条件（实测）**：不是稳定复现。同一个二进制：

| 场景 | 错误次数 |
|---|---|
| 默认参数启动，观察 5 s | 0 |
| `--width 1100 --height 720`，重复 3 次 | 0 |
| `--scale 1.5` | 0 |
| 长观察 15 s + 点击 3 个导航页 | 0 |
| **首次观测到的那一次** | 数十次（连续） |

那次的环境差异：前面有一个 PowerShell 控制台窗口抢焦点，
并且**同时有其他后台构建任务在跑**（多进程争 GPU）。

### ⚠ 排查方向已修正（2026-10-27）

本文最初写的是“抓是哪条绘制路径忘了 `draw_scope()`”——**这个方向是错的**。
`71398b29` 已经把 `active_context()` / `note_deferred_draw()` **移到 private**，
`draw_scope()` 是唯一口子，**漏标记在类型系统上不可能发生**。
再去抓“哪条路径忘记账”只会白跑。

**真正未排除的可能**（按可能性排序）：

1. **提交失败但仍清零**。`submit_deferred_locked()` 里：
   ```cpp
   if (SUCCEEDED(deferred_->FinishCommandList(FALSE, &list)) && list != nullptr) {
     context_->ExecuteCommandList(list, FALSE);
     ...
   }
   deferred_pending_ = 0;   // ← 无论 Finish 成功与否都清零
   ```
   `FinishCommandList` 失败（设备丢失、驱动重置）时 `list` 为 null，
   命令**没有执行**却把 `pending` 清了——画面停在上一次内容，而日志无报错。
   这与观察到的“降级到 GDI blit + 画面仍正常”吻合（blit 拿的是旧 backbuffer）。
2. **多画布共用 / 锁竞争**下的交错。`g_context_mutex()` 是全局串行点，
   若某条路径在未持锁时调 `submit_deferred_locked`（它要求调用方持锁），
   `pending` 的读/写可能与其他线程交错。
3. 设备丢失/重置路径未处理（D3D11 的 `DXGI_ERROR_DEVICE_REMOVED`）。

**下一步**（换方向）：
· 在 `submit_deferred_locked` 里给 `FinishCommandList` **失败分支加日志**
  （当前是静默跳过）——这本身就是一个应该补的缺陷
· 确认 `present()` 调 `submit_deferred_locked` 时是否**确实持锁**（注释要求持锁）
· 用 `--backend win32` 多进程并行压测复现（首次观测时正是多进程环境）

### `st buildd` 守护进程：已评估，**不做**（2026-10-26）

**需求**：构建系统支持后台持续运行，减少大量修改后的编译时间。

**做到什么程度**：`st watch`（已交付）把「保存 → 构建」变成自动，
用户感知的等待已接近零。剩下的提升只有「进程启动 + 清单解析」。

**实测那个剩下的量**：`st version` 5 轮取最小 **0.122 s**。
而 `st watch --once` 总耗时 **0.34 s**（构建 0.13 + 扫描 0.2 + 启动 0.12，有重叠）。

**为何不做**：要付的复杂度是
守护进程生命周期（启动/健康/退出/残留）、控制通道协议（版本/鉴权/并发）、
多客户端协调、崩溃恢复、端口/socket 冲突处理——一整套**新的失效模式**。
而其中最难查的一类是「守护进程挂了但客户端以为它在跑」（与增量判定静默失效同型）。
0.12 s 的收益不值这个风险。

**何时重新考虑**：若将来出现“一次构建几分钟”的大工程，或需要跨机器共享构建缓存，
那时守护进程的收益模型完全不同（省的不再是启动开销，而是编译本身）。

### PCH 再瘦身：已做一轮，剩余项收益小（2026-10-26 已完成并发组）

已从 PCH 移出并发组（`<thread>`/`<mutex>`/`<chrono>`/`<atomic>`/`<future>`/`<stop_token>`）：
体积 **143.9 → 119.1 MB（−17%）**，全量构建 **31.39 → 28.67 s（−8.7%）**。
前置的自包含缺陷（13 个文件）已一并修完。

**剩余可选项与为何不动**：
| 组 | 体积增量 | 不动的原因 |
|---|---|---|
| `<format>` | +15.9 MB | **89/147 个单元直接用**，移出后每个都得重新解析 |
| `<filesystem>` | +11.6 MB | 路径密集代码里到处是；只 3 个 `.cpp` 直接 include，但经 `st/core/fs.hpp` 间接覆盖全面 |
| `<map>` 系容器 | +27.6 MB | 被大量使用 |

**现象**：`run_shards` 里子进程用 `capture_output = false`（直通终端），
于是**测试的全部输出不进控制台镜像**——一次 16 片全量测试后，
`.st/logs/test-*.log` 只有 **132 字节**（三行头部），失败用例的断言信息一条也没有。
而“看日志定位失败”正是日志落盘的主要用途。

**为何没当场改**：原注释的理由成立——“捕获到内存再回显会让长跑期间看不到任何进展”。
正确解法是**拉一条 tee**（`process::StreamHandle` 逐行读，同时写终端与镜像），
不是改成 `capture_output = true`。那是新增一段并发读逻辑（多片并行时各有两条流），
属独立小课题，不塞进本轮。

**方向**：`run_shards` 的单片与多片分支都用 `StreamHandle`：
起进程 → 循环 `read_line`（每次既 `st::print` 又落到镜像）→ `finish()` 拿退出码。
注意 `merge_stderr = true`（终端要时间顺序）。

### PowerShell 的 `Get-Content` 默认编码（已在歌白侧修，霜天侧供参考）

`sh` 工具的 UTF-8 引导段已补上 `$PSDefaultParameterValues['*:Encoding']`（见
`packages/server/src/core/security/sandbox.ts`）。背景：PS 5.1 的 `Get-Content`
默认按 ANSI 代码页，读 UTF-8 日志得到 `鏋勫缓瀹屾垚`；而 pwsh 7 默认 UTF-8，
所以只在没装 pwsh 的机器上现形。

**霜天侧注意**：`.st/logs/*.log` 写的是**正确 UTF-8**，读乱码是读取侧问题，
不要把日志改成 UTF-16 或 BOM 来“适配”。

### `st test` 的子进程输出不进日志（2026-10-26 发现）

**现象**：`run_shards` 里子进程用 `capture_output = false`（直通终端），
于是**测试的全部输出不进控制台镜像**——一次 16 片全量测试后，
`.st/logs/test-*.log` 只有 **132 字节**（三行头部），失败用例的断言信息一条也没有。
而“看日志定位失败”正是日志落盘的主要用途。

**为何没当场改**：原注释的理由成立——“捕获到内存再回显会让长跑期间看不到任何进展”。
正确解法是**拉一条 tee**（`process::StreamHandle` 逐行读，同时写终端与镜像），
不是改成 `capture_output = true`。那是新增一段并发读逻辑（多片并行时各有两条流），
属独立小课题，不塞进本轮。

**方向**：`run_shards` 的单片与多片分支都用 `StreamHandle`：
起进程 → 循环 `read_line`（每次既 `st::print` 又落到镜像）→ `finish()` 拿退出码。
注意 `merge_stderr = true`（终端要时间顺序）。

### PCH 移出「少数单元才用」的重量级头（2026-10-26 **已完成并发组**）

> **✅ 已完成（2026-10-26）**：并发组已移出，自包含缺陷已修。
> · PCH 体积 **143.9 → 119.1 MB（−17%）**
> · 全量构建 **31.39 → 28.67 s（−8.7%）**（三轮取最小）
> · 前置：13 个「用并发类型却不 include」的文件已补齐（逐个验证“不带 PCH 也能编译”）
>
> 剩余项（`<format>` / `<filesystem>` / 容器）为何不动，见上方
> 「PCH 再瘦身：已做一轮，剩余项收益小」那条。原文保留下方供回溯。

**现象**：`include/st/pch.hpp` 的 `.gch` 达 **143.9 MB**，构建 **4.26 s**（纯串行前置），
且 28 路并行时每个 `cc1plus` 各自 mmap + 反序列化它——既是固定前置成本，
也是实测 **1.41×** 并发膨胀的主要来源（标定实验：同一文件单线程 15.34 s，
**2 路并发就 1.34×**，28 路 1.41×）。

**实测收益（两个正交量同向下降，故为真收益）**：

| 变体 | PCH 体积 | PCH 构建 | 3 个代表单元合计（各 2 轮取最小） |
|---|---|---|---|
| 现状 | 143.9 MB | 4.97 s | 18.27 s |
| 移出 G5 并发组 | **119.1 MB（−17%）** | **4.79 s** | **16.61 s（−9.1%）** |

分组实测（PCH 构建增量体积）：G1 核心 +57.1 MB（无可避免）· G2 容器+算法 +27.6 MB ·
G3 format +15.9 MB（**89/147 个单元直接用，不能移**）· G4 filesystem +11.6 MB ·
**G5 并发（chrono/mutex/thread/future/atomic）+24.6 MB** · G6 C++23 设施 +4.0 MB。

**为什么没当场改（前置条件，必读）**：有 **7 个 `.cpp` 与 1 个头文件依赖 PCH 提供并发头**
——它们用 `std::thread`/`std::mutex`/`std::chrono` 却没有任何对应 `#include`
（实测：`src/core/thread_pool.cpp` 用 `std::thread`+`std::stop_token`，一个 include 都没有）。
这违反 CONVENTIONS §6「每个 `.cpp` 自包含、不得依赖 PCH」。
直接移出会在编译期报大量 `'thread' is not a member of 'std'`。

**方向**：
① 先逐个修自包含（补 `#include`）——这是「翻译单元自包含」约定的补课，独立可做；
② 再移出 G5，跑全量 test/lint；
③ 注意 `tools/check_selfcontained.py` 当前**静默 `sys.exit(count)`、不打印任何失败细节**
（实测 `python tools/check_selfcontained.py --tu-only -j 20` 只回一个退出码 49）——
修好它之前，自包含体检没有可用输出。那条单独记在下面的 P2。

### `tools/check_selfcontained.py` 不输出失败细节（2026-10-26）

**现象**：`python tools/check_selfcontained.py --tu-only -j 20` 只返回退出码（实测 49），
**stdout/stderr 全空**。它的用途是「找哪些翻译单元不自包含」，而它不说是哪几个。

**方向**：失败时逐条打印 `源文件 → 缺的头`（与 `st lint` 的 `文件:行 [规则] 说明` 同口径）。
可作为 §6 自包含约定的可执行门禁——目前它名义上存在、实际无法使用。

### `st lint` 单线程 22.8 s —— **已完成并行化**（2026-10-27）

> **✅ 已做**：串行 → 并行，**22.8 s → 1.30~1.37 s（约 17×）**，并行度 0.99 → **19.2/28**。
> 下面保留原文（含当时对“跨文件规则”的顾虑），并注明**那个顾虑实测不成立**。

**当时的判断**：759 文件、CPU 22.7 s / 墙钟 22.80 s ⇒ 并行度 **0.99/28（3.5%）**。

**原文方向与结果对比**：

| 当时的顾虑 | 实测 |
|---|---|
| “L16 层方向需看路径” ⇒ 要全局视图 | ❌ 不成立：`check_layer_includes()` **只看本文件自己的路径** |
| “清单豁免需看 `st.pkg`” ⇒ 要全局视图 | ❌ 不成立：`exemptions` 在外层解析好、**只读传入** |
| “跨文件状态少” | ✅ 成立：`suppressed` / `manifest_suppressed` 是**每文件局部计数器** |

⇒ **所有规则都是单文件规则**，并行只需把“读 + 扫”分到多线程。

**实现要点**（两处值得记）：
1. **先读进内存、再并行扫**：在并行任务里读盘会让几千个文件同时压在磁盘队列上，
   带宽固定而寻道竞争反而变慢。串行读（OS 页缓存友好）+ 并行 CPU 密集扫描。
2. **失败语义改了**：旧实现遇读失败立刻 return，并行下无法中途放弃，
   改成“全部跑完后报下标最小的那个”——副产品是用户能一次看到所有读失败的文件。

**一个自己写出的 bug（逆向验证抓到）**：初版写 `report.files_scanned = jobs.size()`，
即**无条件赋成总数**——分片漏掉文件时报告仍显示“扫了 N 个”。
改成统计 `scanned.scanned == true` 的条目才正确。

### `sqlite3.c` / `quickjs.c` 占串行总量 17.6%（2026-10-26 实测）

`sqlite3.c` 20.1 s、`quickjs.c` 14.2 s（单线程串行），而全量 54 单元串行共 194.8 s。
它们已是 `jobs_large` 闸门的受益者（本轮把闸门从 2 放宽到 8 后能与其它大单元并行），
但仍是**单个单元不可再分**的长尾。

**方向**（都未做，仅记录）：拆成多个 TU / 降优化档 / 用 `-O1` 单独覆盖。
属第三方源码，**改动风险高于收益**，不优先。

## P0

### 三维建模：图元库 / glTF / 建模运算（2026-10-30，已完成）

**做了什么**：新增 `src/geom/`（`primitives` / `mesh_ops` / `gltf`）与 `include/st/raster/` 下三个头：

- **22 个参数化图元**（plane / cube / uv_sphere / ico_sphere / cylinder / cone / capsule /
  torus / disc / circle / grid / tube / prism / gear / torus_knot / arrow / wedge /
  四个正多面体 / helix），契约是"水密 + 绕序与法线同向 + 单位法线 + UV 0..1 + 确定性"。
- **建模运算**：变换（逆转置法线 + 镜像翻绕序）、合并、焊接、法线重算、尺寸归一、
  落地、AABB、JSON 自检摘要、OBJ 导出。
- **glTF 2.0 读写**：`.gltf`(外部/内嵌 bin) 与 `.glb`(单文件)，含节点层级变换、PBR 材质、
  相机、错误带 JSON 路径。**格式选型理由**：Blender/Unity/Unreal/Godot/three.js 全线原生支持，
  且是模型最眼熟的三维格式。
- 接入两处调用面：`scene3d_script` 的 `shape`（22 个名字 + 别名）与新的 `gltf` 字段、
  裁云 `model3d` 的 `model_shape`（未知名字现在会 WARN 并给可用清单，不再静默落盒子）。

**为什么值得做**：在此之前三维只有 5 个内置工厂 + OBJ 文件，"想造一个胶囊/齿轮/棱柱"
只能手写 OBJ。而"能做 Blender 和游戏引擎"的前提是**能造形状 + 能用通用格式交换**——
这两件事这次都通了。

**同一轮补齐：组合模型的空间体系**（`src/geom/scene_graph.cpp` + `include/st/raster/scene_graph.hpp`）。
图元库解决"造形状"，"把零件组合成模型"需要节点树：父子层级（父动子动）、每节点局部坐标系、
以及**枢轴**（`local = T · T(pivot) · R · S · T(-pivot)`，`pivot_at` 支持 origin/center/bottom/top）。
扁平写法（`parent` 引用，父可后置）与嵌套写法等价（逐位比世界矩阵）。重名/环/父不存在/深度超限
全部在解析阶段报错并带路径。

**同一轮第三块：动画**（`src/geom/quaternion.cpp` + `src/geom/animation.cpp`）。
四元数（slerp / 欧拉互转）+ 关键帧轨道（translate/rotate/scale/visible）+ 四种缓动
+ 稠密化。场景脚本加 `animation` 字段与 `render_scene_at`；glTF 读写动画（含
CUBICSPLINE 取控制点、ease-* 导出前稠密化的取舍）。

**同一轮第四块：蒙皮**（`include/st/raster/skin.hpp` + `src/geom/skin.cpp`）。
骨架（关节名 + 逆绑定矩阵）、蒙皮矩阵（世界 × 逆绑定）、逐顶点 LBS、权重限 4 并归一化、
朴素自动权重。`Mesh::skins` 存成**平行边表**（每顶点 8 float）而不是塞进顶点——
`kVertexFloats = 9` 是生成器与三处后端的契约，改动面远大于收益。glTF 侧读写
`skins` / `JOINTS_0` / `WEIGHTS_0`（写出时补出缺失的关节节点）。

**同一轮第五、六块：碰撞拾取 + 材质贴图**。
碰撞：`math/collision.hpp`（射线×AABB slab / 射线×三角形 Möller–Trumbore / 球 / 盒距）
+ `raster/pick.hpp`（场景级 raycast/raycast_all/overlaps/bounds_gap，8 角世界 AABB）。
贴图：`texture` 字段 + UV 变换（scale/offset/repeat/clamp）+ 双后端一致，
采样契约"纹理 × 顶点色"。

**验收**：`tests/geom_primitives_test.cpp`（6 条，逐图元断言）、`tests/geom_gltf_test.cpp`
（6 条，含 `.glb` 头/chunk 长度、第三方文件、错误路径、落盘往返）、
`tests/geom_scene_graph_test.cpp`（7 条，组合模型：乘序/枢轴/扁平=嵌套/可见性继承/端到端/错误路径）、
`tests/geom_scene_integration_test.cpp`（8 条，含 22 图元**真的画出像素**、导出读回、尺寸字段优先级）、
`tools/primitives_gallery_probe.cpp`（22 格 PNG + 22 个 `.glb`，实测 22/22 通过）、
`tools/composite_pivot_probe.cpp`（四格对照图，父转/枢轴/可见性三条判据均 ok）、
`tests/geom_animation_test.cpp`（11 条，含 slerp 最短路径、缓动曲线、枢轴×动画组合）、
`tools/animation_probe.cpp`（8 帧分镜 + 带动画 glb，四条判据均 ok）、
`tests/geom_skin_test.cpp`（10 条，含绑定姿态恒等、未归一化权重不缩小、glTF 权重往返）、
`tools/skin_probe.cpp`（蒙皮 vs 刚性对照图：关节连续 vs 断裂）、
`tests/geom_collision_test.cpp`（10 条）与 `tools/pick_probe.cpp`（21 射线重投影核验）、
`tests/geom_texture_test.cpp`（6 条）与 `tools/texture_probe.cpp`（四格贴图对照）。
文档：`docs/geometry.md`。

**这一轮踩到的缺陷形态（值得记住）**：22 个生成器第一版有 **6 个绕序错**
（tube 内壁全反、gear 侧壁 32 个反、torus_knot/helix 整体内外翻转、wedge 少两个面、
capsule 10 个零面积三角形、dodecahedron 面表 33 条边界边）。
根因全都一样：**绕序靠人脑推**。修法不是逐个改，而是把定向从生成器里拿掉——
`Builder::face()` 用叉积算几何法线、与解析法线反向就自动翻序，于是"绕序与法线同向"
从"每个生成器各自保证的不变式"变成结构上不可能违反。
同理，`wedge` 改写成"顶点表 + 逐面自动定向"、`dodecahedron` 改用**对偶构造**
（二十面体的面心 = 十二面体顶点），不再手拼面表。

### 把构建工具从框架里剥离成独立项目（2026-10-26，结构性）

**这是根因，下面那条 `force=true` 只是它的症状。**

`st` 在 `st.pkg` 里就是一个普通 target：

```json
"st": { "kind": "executable",
        "sources": ["tools/stpm/*.cpp", "src/ui/window_control.cpp"],
        "exclude_sources": ["src/ui/**", "src/app/**", "src/control/**", "src/edit/**", "src/lsp/**"] }
```

它不是「构建工具依赖框架」，而是**它就是框架的一个应用**：用 `exclude_sources` 裁掉应用层，
剩下 core/math/codec/raster/text/md/xml/pkg/ext 全部**链进同一个可执行文件**（152 单元）。

**代价**：构建逻辑（`src/pkg/*.cpp`）住在框架的 `src/` 下，于是改一行构建逻辑 →
整个 `st` 目标处于「可能过期」状态 → 自举。而自举只能 `force=true` 全量重编，
因为没法可靠区分「哪些单元真的受这次构建逻辑改动影响」。

**为何值得剥离**：构建工具的理想形态是**只依赖 core/fs/process/hash/json**（读清单、
跑编译器、判增量），不需要 raster（画图元）、text（量字宽）、md、xml——那些是**应用**
才需要的。剥离后 `st` 的目标规模会从 152 单元降到几十个，自举成本随之下降一个量级，
`force=true` 那条也可以去掉（单元少到全量重编也只要几秒）。

**方向**（未做，需单独立项）：
① 查清 `src/pkg/*` 到底引用了哪些框架子系统（预期：core + fs + process + hash + json + string 就够；
   若发现间接引入 raster/text，那就是需要先拆的那一块）；
② 把 `tools/stpm` + `src/pkg` + 必要底层拆成独立 `st.pkg`，与框架**互不引用**；
③ 目录结构、target 划分、框架引用关系都变 ⇒ 属设计层变更，**需先同步 DESIGN.md 并单独评审**。

**这一条不要顺手做**（2026-10-26 明确决定不做）：它不是一个下午能收尾的改动，
顺手做会演变成一次没有评审的结构重构。本次已记录并停止。

### 自举（`st rebuild`）每次都全量重编 152 单元：`force = true` 是错的（2026-10-26）

> **✅ 已完成（2026-10-26 晚）**。根因确认与处置见下（保留原文以便回溯）：
> · `command_rebuild` **与** `rebuild_self_and_reexec` 两处都已是 `force = false`；
> · **真正的 55.5 s 大头不是 `force`**，而是 bootstrap 与 `st build` 的**缓存互不命中**：
>   对象目录不同（`build/obj/bootstrap` vs `build/dev/obj`）、对象命名不同（相对路径 vs 绝对路径归一化）、
>   且 bootstrap **完全不用 PCH**（每单元重解析 143.9 MB 头）。三处对齐后：
>   改一行 `src/pkg/*` → `st` 可用 **55.5 s → 6.84 s（8.1×）**；
>   bootstrap 后第一次 `st build` **重编 45 → 0**。
> · 可信性靠两件事：`.meta` 第三列内容哈希 + **逆向验证**（在源文件末尾追加
>   `extern "C" const char …[]` 外部链接符号，重建后到 `.o`/`.exe` 里搜——
>   **新改动与还原都真编进去了**）。另提供 `ST_VERIFY_SELF_REBUILD=1` + `ST_VERIFY_TOKEN` 开关。
> · 完整数据与口径：`docs/ITERATION_SPEED.md` §3.2。

**现象**：改 `src/pkg/build.cpp`（构建逻辑）后，下一次任何 `st build/test` 会先自举重建，
实测 **83.7s**（`st rebuild` 直接计时）。而真正改动的通常只有 1~2 个单元。

**根因**：`tools/stpm/main.cpp::command_rebuild` 里：

```cpp
options.force = true;   // 自举重来：产物路径变了，增量缓存也对不上
```

**这句话是错的**（已查证）：`output_override` 只影响**最终链接产物的路径**
（`build.cpp:2404`：`options.output_override.empty() ? default_output : options.output_override`），
而对象目录 `object_dir` 完全不变。`force` 只应影响“要不要重编”，与产物叫什么都无关。

**方向**：改为 `force = false`，让它走正常增量（只重编改动的单元 + 链接到 `staging`）。
预期待遇：个位数秒。

**为何没当场改（必读，否则会重踩）**：风险在“自举后新 `st` 必须真是新逻辑”，
而这正是这条 `force` 当初想防的事（见 `check_self_staleness` 注释里五次踩坑史：
`st` 是自举产物，改了构建逻辑后 `st build` 只编目标、永远不重编 `st`）。
验证方法：改一处**可观测行为**（例如 `--list` 的快速路径），自举后跑一次确认生效。

**注意别被假象骗（本轮实测）**：
· kill 构建会留下**孤儿 `cc1plus`/`g++` 进程** + 残留 `*.tmp`（占着文件删不掉），
  下一次构建因此报 `rename ... No such file or directory` 且多花几十秒。
  清理：`Get-Process | ?{ $_.ProcessName -match 'cc1plus|g\+\+|gcc' } | Stop-Process -Force`。
· 自举**会重跑本次命令**，所以 `st build` 的 WALL 里包含了自举时间，而它自己报的
  “编译 X 秒”不含。两者差一个数量级时，先查是不是自举，再怀疑测量方法。
· 用相对路径改文件时若 `workdir` 参数没生效，会改到错误位置（本轮把 `// touch-probe`
  写进了真源码，靠 `git status` 才发现）。**改源码一律用绝对路径**。

### `st test` 固定开销（2026-10-26，**已修**，留剩余项）

原问题：`--list` 不跑任何用例也要 14.4s，而直跑二进制单条用例 0.44s。

**已修三处**（提交 `fbc20b2`，13.3s → 5.5s）：
① 增量判定并行化（`compile_units` 的 `needs_rebuild` 循环）；
② `--list` 不再走构建（只枚举用例名，二进制新旧无关紧要）；
③ 带过滤词时不分片（27 条用例分 16 片，进程启动成本 2.1s ≫ 并行收益 0.44s）。

**剩余**：`st test <filter>` 仍有约 **5.5s**（其中约 1.3s 是构建增量判定，必须保留，
否则测的可能不是最新代码）。日常验证自己的改动时仍推荐直跑
`build/debug/bin/st_tests.exe <完整用例名>`（0.44s）。

### `scale_gate_select_panel_frame_is_independent_of_option_count` 并行全跑偶发失败（2026-10-23）

**现象**：`ST_SCENE3D_BACKEND=software st test` 全量并行跑时偶发失败；
单跑三次全绿。与 `scene3d_transparent_shows_the_far_surface_through_the_near_one`
是同一类（分片并行下的状态串扰）。

**与三维改动无关**：该用例不碰 `Scene3D` / `Material` / `platform_d3d11`。

**方向**：同上面那条——查并行分片之间是否有进程级共享状态（环境变量
`ST_SCENE3D_BACKEND`、GPU 设备单例）没有隔离。

### 构建：改了被 `#include` 的 `.cpp`，测试单元不重编（2026-10-23，**本轮返工六次的主因**）

**现象**：本仓库大量用 `#include "examples/caiyun/xxx.cpp"` 让测试对着真实实现断言。
但改了 `fx.cpp` 之后跑 `st test`，**测试单元不重编**，跑的是旧二进制：

- 注入回退（文件变短 11 字节）→ 仍命中缓存；
- mtime 推到未来 → 仍命中；
- 删测试单元 `.o` → 报「重编 0 / 命中缓存 147」；
- **连 `st_tests.exe` 一起删** → 才真的重建。

`.o.meta` 里**确实记了** `fx.cpp` 的 (mtime, size)，按理该抓到，但实测构不成触发。

**代价**：逆向验证连跑五轮全部假绿（测试没变红），每轮 1~2 分钟；
最后靠“手改 + 删二进制 + 直接跑”才看到真相。

**临时规避**（已写进 `scripts/verify_rotate_reverse.py`）：
每轮删 `build/dev/obj/*<测试名>*.o*` 与 `build/dev/bin/st_tests.exe`。

**方向**：查 `needs_rebuild` 在“测试单元的依赖是被 include 的 .cpp”这条路径上为何不触发；
或让 `st test --force` 真的强编（现在只强制跑、不重编）。

### `st test` 的判定不适合脚本化（2026-10-23）

两件事让自动化验证很难写：
1. **returncode 不可用**（无论结果如何都返回 1，包装层日志落 stderr）；
2. **只跑单个用例时没有汇总行**，只有一行 `FAIL <name>`（带 ANSI 颜色码）——
   用 `(\d+) failed` 判定会永远得到“0 失败”。

**方向**：退出码按真实结果返回；单用例模式也给汇总行；或提供 `--json` 输出。

### 验收尺子必须自检完整性：「没真正判」不能算 PASS（2026-10-23，已修+回归测试）

**已修**（`scripts/ot_bench.py` 的 `judge()` + `scripts/ot_bench_ref.py`），保留此条作为**纪律记录**：

同一形态的缺陷在一轮内出现三次，三次都是「尺子失效时静默通过」：

| # | 形态 | 后果 |
|---|---|---|
| 1 | 并行渲染静止占比分母恒为 0 | 报「静止 0.0%（0 对）」而摘要无异常 |
| 2 | 用变化**峰值**当强度 | motion-web 峰值 234 来自一帧硬切黑场，同片 p95 只有 2.01 |
| 3 | 相对判据缺参考值时**静默跳过** | `violations` 为空 ⇒ 退出码 0 ⇒ 看上去 PASS，实际一条判据没跑 |

第 3 类最危险：它不会让结果变难看，只会让结果**看起来正常**。实测代价：
`ref_baseline.json` 里从未有过 `bright_ratio` 字段 ⇒「过亮」对 13 条片子**一次都没判过**；
而没传 `--ref-baseline` 时所有相对判据都不执行、`violations` 为空 ⇒ 直接 PASS。

**现在的行为**：`REQUIRED_REF_KEYS` 缺失 → 报 `incomplete`；一条相对判据都没执行 → 报
`undecided` 且**退出码 4（未判定）**，而非 0。
护栏：`scripts/test_ot_bench_judge.py`（14 条）+ `scripts/verify_judge_reverse.py`
（逐条回退→确认变红→恢复，4 条全拦住）。

**同类风险还没查的**：框架里其他「解析失败就跳过」的校验路径——凡是“静默空集合=通过”的地方都要重看一眼。

### 裁云：`ScriptShape.rotate` 字段存在但绘制不使用（2026-10-23，**已修**）

`rotate` 有字段、`layout.cpp` 也解析了，但 `paint_script_shapes` 里没有 rotate 分支。
**这比缺字段更贵**：schema 自省能看到 `rotate`、脚本里写了不报错，而画面就是正的——
两个子会话都以为是自己的写法问题。

**已修**：矩形/文字/多边形三类图元绕**包围盒中心**旋转（几何提到 `src/raster/texture.cpp`
的 `rotate_point_deg` / `rotate_points_about_bounds`，那里才写得了单测）。
回归测试 6 条在 `tests/raster_texture_test.cpp`，逆向验证 4 条全拦住
（`scripts/verify_rotate_reverse.py`）。

### 裁云：缺多边形/四边形填充图元（2026-10-23，**已修**）

`shapes` 以前只有 `kind=0 圆 / 1 矩形 / 2 线 / 3 文字`。凡需透视/倾斜的图元
（三维近似、斜放的卡片、投影阴影）都只能用多个轴对齐矩形拼——实测（clawd）读起来像梯子。

**已修**：新增 `kind=4 多边形`，`points` 接受扁平数组 `[x0,y0,…]` 或 `[[x,y],…]` 两种写法；
少于 3 个顶点不画（而不是退化成点/线）。旋转叠加在顶点上。

> 注：旋转多边形/旋转矩形的绘制**没有圆角**（走 `fill_path`，不经过 `fill_rect` 的 radius）。
> 需要圆角又需要倾斜的场合，目前只能二选一。

### 裁云：`layout` 运行期错误静默降级，渲染日志里无任何痕迹（2026-10-23，**未修**）

`layout.js` 抛 `ReferenceError` 时，渲染器按设计降级为纯 JSON（只画图层）——画面只剩背景，
**而日志一个字都不打**。两个子会话各自撞到过一次（overlap 删了一行 `const push`、
clawd 同类），都是靠“抽帧发现画面空了”才定位。
**方向**：降级时在渲染日志打一行 `WARN layout 抛错，已降级：<消息>`；并与脚本语法错同等待遇（加载时就报）。

### 裁云：`shapes` 没有局部坐标系，钻穿尺度类片子做不了（2026-10-23）

**现象**：想在“一个格子里”画东西（clearing 钻进士一天里的半小时），
必须手写 `pageOrigin(dayScale, ax, ay)` 把日页坐标换算到屏幕。
一旦写错，`dayH=2900` 时画面里只剩左下角一块会议条（实测抽帧）。

**更硬的限制**：内容必须**装得进画面**。clearing 想钻 5200×，但日页宽 900px
⇒ 相机上限 1920/900 ≈ 2.13×。这就是 clearing 的 p95 只有 6.5（参考 16.5）的直接原因。

**方向**：给 `custom` 场景加 `pivot` + 局部 `scale`（子图元按局部坐标画），
而不是让脚本自己乘。这样 ① 钻任意倍率不需要重算原点；② 脚本可读性也回到“在层内画”。

**实测数据**：`docs/onetake_13case_benchmark.md` 第三节 G1/G2。

### 裁云：`LayerDelta.scale` 不重算 `align`，文字缩放会错位（2026-10-23）

**现象**：unbroken 的字标分两半写（左 "unbr" 右 "ken"，中间留给那个圆当 o）。
`scale 4.2 → 1` 之后，左半横到画面中偏，右半还在原位——两半脱开。

**原因**：`scale` 按图层中心缩放，而 `align` 决定的是文字在框内的对齐；
缩放后对齐基点没重算。

**方向**：要么让 scale 以对齐点为原点，要么一并提供 `pivot`（与上一条同一个修法）。

### `caiyun_verify` 工具调用即报错（2026-10-23）

`registry?.has is not a function`（In 'registry?.has("shuangtian_get")', 'registry?.has' is undefined）。
本轮的替代：判据已搬进 `scripts/ot_bench.py` 的 `judge()`（渲染日志的 `画面质量` 行 + motion_qa + carry_qa），
不依赖该工具。但它本身还会被别处使用，需要修。

## P1

- [ ] **GPU `present()` 的“调用方先提交”契约无法被满足（已改为自提交，但缺回归测试）**

  **现象**：`Presenter::present()` 原本要求“调用方已 `submit_deferred_locked()`”，
  否则报 `画布有未提交的绘制命令（漏了 submit_deferred_locked）` 并**跳过上屏**。
  但 `Surface` 公开接口里**没有提交入口**——调用方在结构上无法满足它。

  **实测代价**（相期启动，2026-10-27）：`platform_win32` 分配画布时调
  `surface_->clear(...)`（记一笔未提交绘制），紧接着建 presenter、
  `show_when_ready()` 调 `present()` → 断言为真 → 跳过上屏、退回 GDI blit。
  8 秒内重复 ** 5 次**：GPU 路径**从未生效**，每帧走 GPU→CPU 回读，
  首帧多花 **~440 ms**（窗口已显示却不响应 → 鼠标转圈）。

  **已改**：`present()` 自己 `submit_deferred_locked()`，之后才断言
  （那时仍有剩余才是真异常：`FinishCommandList` 失败 / 设备丢失）。
  实测修复后：未提交错误 0 次、回退 GDI 0 次，“未响应”窗口期 2056 ms → **4~6 ms**。

  **待做**：`present()` 需要真实窗口（swapchain），无头下无法建 presenter，
  所以当前**没有回归测试盖这条**。可考虑的方向：把“提交职责”下沉到
  `Surface`/记录层（在那里保证每次回读/呈现前已提交），使它可被无头测试铉住；
  或者给 `GpuCanvas` 加一个可注入的假 swapchain。

### 三维成员：`layout` 增量曾对组内成员静默失效（2026-10-26，已修，留回归口）

**现象**：同一脚本给 `model3d` 成员的 `dx` 从 0 改到 300，成片里物体**一动不动**
（实测屏幕中心两档都是 `(783, 627)`）；而同一个 `dx` 给普通图层完全正常。

**根因**：`paint_model3d_group`（`examples/caiyun/model3d.cpp`）对每个成员**重新**调
`evaluate_layer`，而那条路只算基础值与关键帧，**不包含脚本编排层增量**（`dx/dy/dw/dh`
由渲染器的 `states[]` 在 `apply_delta` 里加好）。组渲染器看不到那份 `states`。

**已修**：`paint_model3d_group` 新增 `const std::vector<LayerState>* member_states` 参数
（与 `members` 同序），`motion.cpp` 传 `&member_states`；同时把成员的最终状态存进
`Placement::state`，让**接触阴影读同一份**（否则物体动了、影留在原地）。
逆向验证：回退为 `nullptr` → 位移 0px；恢复 → 329px。

**还值得做**：这条目前**只有手工探针**（当时在临时会话目录里，未入仓），
没有进 `tests/`。它属于“代码读起来合理、跑起来静默错”的类型，
建议补一条单测：同一图层列表跑两帧（`dx=0` / `dx=300`），断言
`paint_model3d_group` 内部算出的 `placement.offset_x` 不同。

### `layout` 的 `alpha` 是**乘数**，`opacity:0` 的图层永远无法被脚本点亮（2026-10-26）

**现象**：脚本里写 `opacity: 0`（打算“由 JS 淡入”），`layout` 返回 `{alpha: 1.0}` ——
图层**始终不可见**。实测：`opacity=0 + alpha=1.0` → 0 像素；`opacity=1 + dh=200` → 正常。

**根因**：`apply_delta` 用 `state.alpha = clamp(state.alpha * delta.alpha)`。
这是**乘法语义**（合理：脚本可以“再压暗一点”），但它让 `opacity:0` 成为
不可恢复的死值 —— 而“初始不可见、由动画淡入”是写动画最自然的写法。

**方向**（二选一，需设计判断）：
① 保持乘法，但在 `parse_layers` 里对 `opacity==0` + 有 `layout` 的场景发
diagnostics 警告（“该图层无法被脚本点亮”）；
② 增一个显式的**绝对值**通道（如 `alpha_abs`），让脚本能覆盖而非相乘。
本片选了规避（脚本里 `opacity` 一律写 1.0，靠 `alpha` 淡入），但下一个作者还会撞。

### `line` 图层的 `h` 是**终点增量**，用 `dh` 画曲线时容易被误读（2026-10-26）

`motion.cpp` 的 `case LayerKind::Line` 写明 `w/h` 是终点增量（不是宽高），这是图层
语义里唯一的特例。实测后果：12 段 `line` 想画一条 `loss` 曲线，只给 `dy` 不给 `dh`
→ 12 段全部水平、叠成一条看不到的线（成片里“曲线消失”）。
给 `dh` 后正常（`dh` 确实会累加到 `state.h`，进而变成终点 y）。

**建议**：`docs/script_design.md` 的图层字段表里，给 `line` 的 `w/h` 加一句显式警告 +
一个“用 `line` 画折线”的最小例子（现在只有字段说明，没有例子，所以容易踩）。

### `model3d` 成员的 `y` 方向与源码注释**相反**（2026-10-26，以实测为准）

`model3d.cpp` 里 `placement.offset_z` 那段注释写“成员 `y` 越大越**远**”。
遮挡实验（大球在 `y=565`，黄块分别放不同 y，量黄色像素可见量）：

| 黄块 y | 可见像素 | 结论 |
|---|---|---|
| 415 | 16 | 被球遮住（在后） |
| 565 | 0 | 完全重合（被吞） |
| 685 | 2187 | 遮住球（**在前**） |
| 715 | 2488 | 更前 |

即 **`y` 增加 = 靠近相机**，与注释相反（那里带一个 `z_sign` 翻转）。
写脚本时以实测为准；注释与 `docs/script_design.md` 附录 B 都应改正。
另外实测到一个连带事实：不同深度的「脚本 x → 屏幕 x」**比例不同**，
所以“前景轨道上的物体停在背景工位前”必须按该深度单独标定，
不能复用另一深度的比例（本片两个深度分别是 1.33× 与 0.96×）。

### 转场交叉淡入会中段发黑（2026-10-26，已修，建议补回归）

**现象**：场景边界后 0.12s 的一帧几乎全黑（均值亮度从 31 跌到 10）。

**根因**（`examples/caiyun/project.cpp`）：旧实现先把**新一幕**不透明画进目标，
再把**旧一幕**以 `opacity = 1 - eased` 叠上去。两侧同时半透明 → 中段整屏变暗。

**已修**：改为「旧幕作底（不透明）+ 新幕以 `opacity = eased` 淡上来」，
并去掉了原先叠在转场上的全幅“闪白”（曾使全局亮度单帧 +71%）与随后的
“压暗台阶”（在修正顺序后会连新幕一起压暗）。

**还值得做**：没有回归测试。可加一条“转场中点不得比两侧暗 30% 以上”的像素断言
（现有 `tools/*_e2e.py` 已能从 mp4 抽帧，改造成本低）。

### 热重载：`Watch` 只盯主脚本与显式 `pages`，不跟 `import`/目录树（2026-10-25）

`ui::HotReload` 现在盯 `main.xml` + `app.js` + `Options::pages`。**多文件 JS 工程**
（`app.js` 里 `__load('a.js')` 之类）改了子页不会被发现——得手动加进 `pages`。
同样是“一个便利的半成品”，与 `docs/hot-reload.md` 的“不做依赖图”一致；
真要模块化时再做（那时需要的不是跳目录树，而是脚本侧的模块表）。

**为什么记下来**：不写的后果是有人拿它开发多文件应用，然后“改了 a.js 没反应”——
而那正是这条通路唯一一个静默失效面。

### `st check --toolchain=clang` 在 `src/raster/software_scene.cpp` 报两条错（2026-10-25，非本轮引入）

`-Wsign-conversion`（1275 行 `int` → `size_t`）与 `-Wunused-lambda-capture`（1287 行
`[samples]` 不必捕获）。两条都不是本轮改动磨出来的（本轮一行未碰该文件），
是跨编译器门禁本来就红。修法直白：1275 显式 `static_cast`，1287 去掉捕获。

### `tts_speak` 的神经引擎在本机无法通过工具调用（2026-10-25）

> **部分已修（同一轮内）**：失败路径原本会**丢掉真因**——现在会把 `error` 码与
> `detail.{stdout,stderr}` 一起带上（旧代码 `run.note || JSON.stringify(run.report)` 只要 note 有值就把
> report 丢掉；`neuralFailureNote` 又只 `String()` 一个对象 → `[object Object]`，
> 连 `no-report` 都匹配不上，落到兵底文案）。回归：`neural.test.ts` 两条，回退后 6 条变红。
> **剩下的是真因**：为什么工具调起的 python 拿不到 `sherpa_onnx`（手动 shell 能）。

**现象**：`tts_speak(engine="neural", ...)` 恒返「神经语音合成失败。原文：python 未返回报告」，
而**同一个 venv、同一套 sherpa-onnx 调用手搓脚本能正常合成**（同一模型 `vits-zh-theresa`，
`cfg.validate()` 为 true、804 个音色、3.18s 音频）。

**已确认的事实**（缩小范围用）：
- `keqing/python/venv/Scripts/python.exe` 里 `import sherpa_onnx` 正常（1.13.8）；
- 手搓脚本（同样用 `OfflineTtsConfig` + `date/number/phone/new_heteronym.fst`）出音频正常；
- 工具报的是 `neural.ts::neuralFailureNote` 的 `no-report` 分支——即 **python 进程没吐出报告行**，
  而不是模型/配置错误。

**方向**：stderr 已补上收集（见上）。剩下要查的是**工具执行环境**与手动 shell 的差异：
`ctx.runCommand` 的 env / cwd / 沙箱策略是否让子进程看不到 venv 的 DLL 目录
（`ImportError: DLL load failed` 是最常见的形态，而它在 stderr 里）。
下一步：把临时脚本与 stderr 落盘保留（现在是随时删的），再用同一份 venv 对比两边 env。

**本轮绕过**：当时用了一个临时脚本（同一条 sherpa 调用，直接落 WAV），未入仓。

### 裁云：承接机制做了 3/6 层（2026-10-23，对标 13 条片后的能力矩阵）

> 13 条的完整对标结果与逐条归因：`docs/onetake_13case_benchmark.md`。
> 结论：**6 条完全通过、7 条有差距**；差距 5/7 卡在「p95」（大面积同时变化），
> 而 p95 只有三种手段能撑：全屏像素阵列、大幅拉远、密集小图元——三种都用上了。

对照 `D:\code\onetake-main\cases` 的 13 条参考片自述，它们反复用的是
**六个不同层次的承接**。我们现在只做了前两层：

| 层次 | 参考片 | 裁云现状 |
|---|---|---|
| 身份承接（同一元素穿过边界，身份不变） | one-dot, unbroken | ✅ `carry_out` |
| 容器承接（外框变大把内容包进去） | skill-demo | △ 只有矩形 morph |
| 尺度承接（同一个世界放大 300×） | clearing, pith | ✅ 2026-10-23（`cam_anchor_x/y` + `cam_zoom_curve`） |
| **包含承接**（每层都在上一层内部，永不替换） | clearing | ❌ 未做 |
| **因果承接**（上一个动作的物理结果触发下一个） | knockon | ❌ 未做 |
| **配准承接**（两半印刷对位，只有对齐才出现第三色） | overlap | ❌ 未做 |

#### 包含承接：`zoom_into`（值得做）

**现象**：现在钻穿靠 `cam_zoom` 从 `from` 一刀切到 `to`，终点倍率与
“目标图层恰好铺满画面”**靠算术手算**（如第 47 格宽 9px ⇒ 需 1920/9≈213×）。
写错一个数就是“钻到一半停住”或“钻过去了”，而**画面不报错**。

**方向**：`carry_out` 增加 `kind: "zoom_into"`——承接元素的终点直接
写成目标图层的 id，由渲染器算出所需倍率与销点。这样“钻到那一格”
是声明式的，而不是手算常量。

#### 因果承接（**建议不做**，已想清楚的否证）

knockon 的“连锁反应”在参考片里是**刚体物理**（一个弹倒下一个）。
裁云的硬约束是“同一脚本两次出片必须逐帧同像素”，而物理引擎的
浮点累积误差在不同档位/并行度下不保证一致。要做得先解决
“确定性的刚体积分”，代价远大于这一条片的价值。

**替代**：脚本层用 `layout` JS 写确定性的“因果链”（上一元素的某个
数值驱动下一元素的入场时机），能拿到 90% 的观感而零风险。

#### 配准承接（**建议不做**）

overlap 的“双色叠印”是**印刷工艺专用**技法（两半CMYK 网屏对位），
通用性低；而“遮罩相乘产生第三色”需引入混合模式，那是一个独立的
渲染管线特性（影响全部图层），不该为一个技法而加。

### 裁云：矢量相机只盖住 `Painter` 类图元（粒子/三维/辉光不跟着缩放）

**现象**：`cam_vector` 的缩放在 `Painter` 的 `rect()`/`point()`/`design()` 里，
而这几个函数**只有矢量图元走**。粒子（`paint_particles`）、三维（`paint_model3d`）、
辉光（`paint_glow`）直接画到 canvas 上，**不跟着缩放**。

**后果**：矢量模式下开粒子/三维的镜头，会出现“背景在放大、粒子不动”的
观感分裂。目前靠脚本自己避免（矢量镜头不开这些效果）。

**方向**（三选一，需先量实际影响再定）：
1. 给这三类也接相机变换（粒子最简单——它与屏幕空间均匀分布，
   缩放后只需把数量密度按 `zoom²` 补回来）；
2. 矢量模式下自动禁用它们并在 `diagnostics` 里提示；
3. 不管（当前做法）——但这需要文档里说清楚，而不是让人撞上。

**另**：矢量模式不支持旋转/震动（`cam_rotate`/`cam_shake` 会被忽略，
只有 `cam_zoom` 生效）。这是当前实现的明确限制，不是遗漏。

### `caiyun_verify` 不可用：它按不存在的 `ctx.registry.has()` 查工具（**已修**）

> **已修（2026-10-25）**：`readProps` 改用契约里真实存在的
> `ctx.registry.resolve(name)` → `entry.tool.execute(args, ctx)`。
> 回归：`verify.test.ts` 三条（走哪个入口 / 依赖未装载的可操作提示 / 无 props 时不编数字），
> 回退后 3 条全红。**不再依赖另建一套判据绕开它。**
>
> 教训：把“契约里不存在的接口”写成可选链（`registry?.has`），
> 报错变成 `xxx is not a function`，而**调用点看着完全正常**——比直接 TS 报错难查得多。
> 这类地方应用包层的真实类型，不用 `as unknown as { ... }` 自己堆一个。

**现象**：调用 `caiyun_verify` 直接报
`读渲染状态失败：registry?.has is not a function. (In 'registry?.has("shuangtian_get")', 'registry?.has' is undefined)`。

**根因**（已定位到行）：`packages/agents/src/agents/caiyun/verify.ts:310-316` 把 `ctx.registry`
断言成 `{ has(n), call(n, a) }`，而**真实契约**（`packages/sdk/src/agent-contract.ts:174`）是
`{ schemas(), resolve(name), getAgentNames() }`——两方法都不存在。
`registry` 对象本身存在（所以 `?.` 不短路），但它没有 `has` 属性 ⇒ 调用时抛型。

**后果**：验片的主力工具失效，只能用 ffmpeg 量帧差 / ffprobe 看规格。后者能得出等价结论，
但少了它附带的“该往哪查”提示，而且每次都要现写脚本。

**方向**：改用真实契约——`resolve("shuangtian_get")` 有值即已装载，
调用走 `resolve(...)?.tool`。同时补一条单测：用一个**只实现真实契约三个方法**的桩
调 `readProps`，能直接拓住这个不匹配（现有桩如果带了 `has`，那就是“桩把差异抹平了”——
参见 CONVENTIONS §7.2）。

> ↑ 上述方向已于 2026-10-25 落地（见本节顶部）。以下原文保留供回溯。

### 裁云深钻示例片：数据只有三层嵌套，不够“每层都是新世界”

**现象**：一条 1×→28× 钻穿的 loss 曲线片能看，但钻到 28× 时
看到的是**同一条曲线的不同清晰度**，不是“进了另一层”。

**实测对照**：参考片 clearing 钻到 300×，每一层都是**真实重绘的其他内容**
（年→月→日→时段→事件），所以每一停都有新东西读。我们只画了三层
（1200 采样 × 8 微步 × 6 子步）且后两层幅度必须小于线宽才不会在远景糊，
于是它们在深钻时也只能是“主线上的小毛刺”。

**方向**（不是加倍率能解决的）：换一个**天生分层**的题材——
地图（国→省→市→街道）、芯片（板→封装→裸晶→单元）、
代码（文件→函数→表达式）。每层有不同的图元与标注，
关键是：**下一层的内容必须上一层的图元里真的装得下**。

## P1

### 三维：ACES 默认档对中等亮度抬得过多（2026-10-23，**主因已解决**）

**主因**：不是 ACES，而是**入口没解码**——`Mesh::to_unit` 只做 `/255`
（注释却写着“线性色”），于是 sRGB 0.55 被当线性 0.55 用（正确应 0.26，偏 2.1 倍），
再叠 ACES 的抬亮就接白了。

已在三处补齐 sRGB→线性（`to_unit` / 纹理采样 / HLSL），实测中灰球
屏幕亮度 0.508 → 0.451，哑光质感恢复。详见 `docs/scene3d_capability_plan.md`。

**残留待调**：ACES 在 0.2~0.6 区间导数 >1，仍会抬亮。若要更中性的默认值，
可选 `ToneMap::None` 或配成对默认值（ACES + 曝光 0.8）。这是**美术选择**，
需配 `tools/scene3d_color_probe.cpp` 量后再定，不凭观感调。

### 三维：色彩空间口径已实现，但未写进**接口头文件**（2026-10-23）

当前实现口径（正确，且已在 `mesh.cpp` / `software_scene.cpp` 里注明）：

| 位置 | 口径 |
|---|---|
| `math::Color` 入参（`colored_sphere` / `plane` 等生成器） | **sRGB 字节**，内部解码 |
| `Mesh::vertices` 直接写 | **线性值**，不做解码 |
| 纹理 RGBA8（`set_texture`） | **sRGB 字节**，渲染时解码 |
| `begin_frame(clear_color)` / `Lighting` 颜色向量 | **线性值** |

**问题**：这些约定只存在于 `.cpp` 的实现注释里，`include/st/raster/scene3d.hpp`
与 `mesh.hpp` 的公开接口**一个字没提**。调用方（裁云 / 界面）很容易传错，
画面会亮一档且看不出原因。

**方向**：把这些口径写进 `mesh.hpp` / `scene3d.hpp` 的接口注释；
或给 `clear_color` 这类参数改名带上 `_linear` 后缀（更硬的手段）。

### 三维：透明混合是**整网格**级排序，不是逐三角形（2026-10-23）

当前 `Material::opacity < 1` 的网格进透明队列，`end_frame` 前按模型平移 z 排序后重绘。
对“玻璃罩 / 半透卡片”这类**整体透明**的场景足够，但两种场合会错：
- 一个网格自身前后重叠（如半透的环面/螺旋）；
- 两个透明网格交叠而不平分前后。

**方向**：逐三角形排序（收集三角形 → 按质心相机距离排序 → 单独 draw call），
或上 OIT（加权混合）。两者都要改索引缓冲策略，属于中等改动。

### 三维：两腿的透明混合不在同一空间（2026-10-23）

软件腿在**线性空间**混合（先解码已画像素 → 混 → 再编码），GPU 腿走 D3D 固定功能
混合（发生在**已编码 sRGB 值**上）。实测同一半透场景中心像素：软件 (178,184,62)、
GPU (118,121,53)。两者都对（前者物理更准，后者是标准做法），但**不是同一个结果**。

**方向**：把三维渲染目标改成 sRGB 格式（`DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`），
硬件就会在线性空间混合；或让软件腿改成在编码值上混以对齐 GPU。
选哪个取决于“以哪条腿为准”，需先量观感差异。

### 三维：阴影贴图**过度遮蔽**（2026-10-24，**已实现但不启用**）

**现象**：`ShadowMap` 能建贴图、能读出遮挡，但地面**整片被压成 0**
（无阴影 84.4 → 有阴影 0.0），而不是只在球下方变暗。

**已定位的部分**（都有实测）：

| 项 | 状态 |
|---|---|
| 投影相机位置/朝向 | 正确：`eye=(-1.92,7.68,-1.15)` 朝 `dir=(0.24,-0.96,0.14)` |
| 几何收集 | 正确：282~513 个三角形 |
| 贴图覆盖 | 2009/262144 ≈ **0.77%**——斜视地面被压扁，属正常 |
| 深度范围 | `z ∈ [0.505, 0.563]`，范围窄但合理 |

**未定位**：`shadow_factor_at` 的深度比较在某些采样上恒判"在阴影里"。
重点怀疑：PCF 采样窗口（`soft_radius=1`，3×3）跨到了**未被写入的贴图像素**
（存 1.0），而 `nz - bias <= stored` 在这种组合下的边界行为还没查清。

**为何先不启用**：它现在会让画面**变错**（整片发黑），比"没有阴影"更糟。
API（`set_shadow_map`）保留、代码保留，默认 `strength = 0` 不启用。
`tools/scene3d_blender_probe.cpp` 的第 7 节会把这条作为「已知缺陷」打印出来，
**不计入通过**——不假装它能用。

**已有的替代**：`PlanarShadow`（平面投影）能覆盖"产品台面上的接触阴影"这个
主用法，实测有效（强度四档：137.9→77.8→41.7→12.2）。

**方向**：先单独写一个只测 `build_shadow_map` ↔ `shadow_factor_at` 的单元级探针
（给定已知几何，手算期望遮挡），把"贴图对不对"与"采样对不对"分开——
本轮卡住的原因正是两者混在一起调，改一处看不出是哪边的问题。

### 三维：GPU 腿不渲染投影阴影（2026-10-23，**设计决策，已显式声明**）

**实测**（`tools/scene3d_capability_matrix.cpp`，同一份场景两条腿各渲一遍）：

| 能力 | 软件腿差值 | GPU 腿差值 |
|---|---|---|
| 投影阴影（开关前后亮度差） | **4.37** | **0.00** |
| 透明混合（同上） | 0.98 | 3.10 |
| 金属高光（塑料 vs 金属） | 15.56 | 15.59 ✅ |

**为何暂不实现**：投影几何每帧随相机变化，走不了 `IMMUTABLE` 顶点缓冲的
内容哈希缓存，需要额外一套动态缓冲 + 一遍绘制；而裁云导出**强制走软件腿**
（`kTargetAcceptsTexture` 恒为 false，因为结果要回读到软件画布），
主用法上碰不到这条路径。

**已做的**：`scene3d_capability_matrix` 探针会把这条差异**显式打印**出来，
避免"一边有一边没有、而测试跑在默认后端所以看不出来"（那正是这条线上
反复出问题的形态）。要补时按软件腿 `PlanarShadow` 的四边形遮罩算法翻译。

### 三维：`scene3d_transparent_...` 在 GPU 腿上**没有后端守卫**（2026-10-23，未修）

**实测（2026-10-23 复核，比原来那条更准确）**：

```
单跑（不设后端）           → PASS
ST_SCENE3D_BACKEND=gpu 单跑 → FAIL
ST_SCENE3D_BACKEND=software → PASS
```

所以这不是「并行偶发」，而是**它在 GPU 腿上本来就不成立**——
GPU 只有一套固定写深度的状态（透明物体会被自己挡住，见本段上一条），
而这条用例测的正是「透过近处半透明看到远处」。**它缺一个
`backend_lacks_transparency()` 守卫**，与 `scene3d_shadow_takes_effect`
那条已有守卫的用例对照即可确认。

**影响**：`ST_SCENE3D_BACKEND=gpu st test` 全量跑会稳定红一条。
默认（不设该变量）与软件腿全绿。

**另**：`scale_gate_select_panel_frame_is_independent_of_option_count`
在并行全跑时偶发失败，与三维无关，已单独记录。

### 三维：GPU 腿的透明测试在**并行全跑时偶发失败**（2026-10-23，**上一条已修正此判断**）

**现象**：`scene3d_transparent_shows_the_far_surface_through_the_near_one`
在 `ST_SCENE3D_BACKEND=gpu` 下：
- **单跑连续 3 次全过**（275/282/292ms）
- **全跑（分片并行）偶发失败**，红通道不足 60（预期红+绿混合）

**已在 HEAD 上复现**（stash 掉本轮全部改动后跑 3 次：run3 出现同一失败），
所以不是本轮引入的回归。

**方向**：GPU 腿的 `draw_mesh` 虽然每次调用都拿 `g_context_mutex`，但**帧级资源
（rtv_/dsv_/深度缓冲）是实例自己的**。并行时 A 实例 `begin_frame` → B 实例
`begin_frame` → A `draw_mesh`，A 的渲染目标已经被切走。
正确修法是**帧级持有锁**，或每个实例用 `CreateDeferredContext` 独立上下文
（后者才是 D3D11 的标准做法，但改动面大）。

### 三维：默认光照偏亮，实体容易接近白色（2026-10-23，**待用户定档**）

`Lighting` 默认 `ambient 0.35 + diffuse 0.75`（相加 1.10）。实测球体
**暗/亮比 0.700**，画面上读作“纯色圆盘”，面间差异被色调映射压到 3 字节内。

已做对照实验（`ambient`/`diffuse` 五档，固定布光，400×400 球体）：

| ambient / diffuse | 暗/亮比 | 暗面绝对值 |
|---|---|---|
| **0.35 / 0.75（现值）** | 0.700 | 129.0 |
| 0.25 / 0.65 | 0.640 | 109.7 |
| 0.20 / 0.60 | 0.602 | 98.3 |
| 0.15 / 0.55 | 0.555 | 85.3 |
| 0.10 / 0.50 | 0.496 | 70.6 |

判据：暗/亮比越小越立体，但暗面绝对值太低会丢细节。舒适区约 0.35~0.60。

**为何没改**：这是**产品默认值**，改它会让所有已出片子的三维层变暗一档
（裁云 `model3d` 也读这个默认：`layer.hpp:326`、`model3d.hpp:69`）。
已询问用户但未得到选择，按“不擅自修改产品默认值”保留现状。
选定后应把依据数字写进 `Lighting` 的注释。

### 三维：阶段 1 已完成（2026-10-23）

背面剔除（`CullMode`，默认 Back）与透明混合（`Material::opacity` + 透明队列）已完成，
两腿均通过（各 1849 条）。遗留项见上方三条（逐三角形排序 / 混合空间 / 默认光照）。

下一步：**阶段 2 · 表现力**（Principled 材质 / GGX 高光 / 多光源 / 阴影）。

## P1

### 布局取整还剩 17.4%：主要来自文字宽度与滚动内容（2026-10-27）

**已完成的两步**（都在本轮落地）：文本行高取整（40.2%→22.9%）、
`grow` 均分取整（22.9%→**17.4%**）。

**还剩什么**：73/420 个非整坐标，分布已换过两轮来源。当前主要是：

| 来源 | 说明 |
|---|---|
| 文字宽度 | `measure_width` 是亚像素量（字形 advance 累加），文字块右边界天然非整 |
| 图标居中 | `Icon` 在偶数宽容器里居中会落在 .5 上（18/84 非整） |
| 滚动内容 | 内容高度由子项累加，非整是**正确**的（滚动偏移本来就是连续量） |

**为何不再继续追**：这三类的共性与前两轮不同——
- 文字宽度**不能取整**：那会让每行文字的实际占宽与排版计算不符，长文本累积误差；
- 滚动偏移**必须**连续，否则滚动会一格一格跳；
- 图标居中取整会让"居中"变成"偏 1px"，反而更明显。

也就是说剩下的 17.4% 里**大部分是正确行为**，不是缺陷。判据从"整像素比例"
换成"边界落在整像素上的可绘制元素"更准——但那需要区分"内容偏移"与"框边界"，
已超出当前收益，留作后续。

### 面内微渐变在 8 位色深下必然成台阶，需要抖动（2026-10-27）

**现象**：为了打破「72.5%（暗）/ 71.0%（亮）的 8×8 像素块完全纯色」这个塑料感，
给 `Element::paint_box` 的背景填充加了 1/255 的纵向微渐变。结果是**一档跳变**而不是坡。

**实测数据**（侧栏 x=100，暗色，666px 高）：

```
y=45  ~ y=381   rgb(25,31,43)     ← 336 行完全同值
y=381 ~  end    rgb(24,30,42)     ← 一行之内跳变
```

逐行模拟确认这是量化必然：1/255 的幅度在 666px 上只能跨 1 档，
分成两个各占 333 行的平台。幅度加到 4/255 会变成 5 个各占 133 行的平台——
人眼对**阶跃边缘**比对绝对差值敏感得多，所以"几个宽台阶"仍然是缺陷，
只是从一处变成五处。

**已回退**（该渐变已从 `paint_box` 移除），因为一档台阶比纯色更糟。

**正确方向**：有序抖动（ordered dithering）——量化前给每像素加 −amp/2..+amp/2
的按位置偏移，把"会被舍入成同一个值"的整片区域打散成交织的两三个值，
平均值仍精确而宏观成坡；副产品就是均匀亚像素噪点（正好补上"表面无纹理"）。

**但落地有三个必须一起处理的点**（这是它没当场做的原因）：

1. **竖直渐变的整行快路径**（`canvas.cpp:1090` 起）：`t` 只随 y 变化时整行
   共用一个 `row_color`。在这条路径上做逐像素抖动会产生**横向条纹**，
   比不抖更糟——必须绕开它。
2. **渐变 LUT 缓存**（`gradient_cache_*`）与横向渐变的 SIMD 递推路径
   （`gradient_row_` + `simd::over_row`）：缓存键不含抖动参数，且行内源色是
   递推出来的，抖动要么进键要么在递推时叠。
3. **性能回归**：这一带是 `PAINT_DIAGNOSIS.md` 里几十次优化的所在
   （竖直渐变 0.12ms/横向 4.22ms 的 35 倍差就是在这里拉平的），
   改完必须重跑诊断而不能只看"画面对了"。

### 径向/扫掠渐变在 `Style::background_gradient` 里没有几何映射（2026-10-29）

**现象**：`Element::paint_box` 的渐变分支最初只映射了 `Kind::Linear`。径向/扫掠的圆心
停在画布原点附近而半径按 0~1 算，于是整个元素被推出渐变范围、clamp 到**端点色**——
看上去像“整块纯色”。

**实测**：80×80 面板，径向渐变（中心 0.5,0.5 半径 0.5，红→蓝），
修复前中心与角落都是 `(0,0,255)`（纯端点色）；修复后中心 `(250,0,5)`、角落 `(0,0,255)`。

**已修**（本次）：三种 kind 都做映射；半径按**较短边**缩放（0~1 半径的语义是“相对元素
尺寸”，用较长边会把圆拉成椭圆，与 CSS `farthest-side` 同口径）。
回归测试：`tests/raster_gradient_kinds_test.cpp`（逆向验证过：回退后立即变红）。

**仍存的性能缺口**：软件端的径向/扫掠走**逐像素** `paint.sample()`
（`canvas.cpp` 的“径向/扫掠（或异常几何）：逐像素采样”），没有线性那样的整行/整列快路径。
线性渐变有竖向（0.12ms）与横向 SIMD（0.19ms）两条快路，径向/扫掠没有。
做氛围图（径向光晕）时若面积大，这一带会成为新的热点；
需要时按 `radial` 的对称性做“按行预计算半径”的快路径，但**先测再改**
（`PAINT_DIAGNOSIS.md` 的教训）。

### 色深可调（8 位 / 16F）探针结论：不解决渐变台阶（2026-10-29）

**背景**：用户问“能不能支持色深可调，两种按性能选用”，目标是想解决 BACKLOG 里那条
「面内微渐变在 8 位下必然成台阶」。本轮只做探针与结论，**未实现**。

**探针（纯数值，666px 高、幅度 1/255 的微渐变——与上面那条实测同档）**：

| 方案 | 最长平台 | 平台数 | 全平窗口 | RMS 误差 |
|---|---|---|---|---|
| A 8 位直接 | 333px | 2 | 40/41 | 0.29/255 |
| B 16F 内部 → 8 位输出 | **333px** | **2** | **40/41** | **0.29/255** |
| C 8 位 + Bayer4 抖动 | 333px | 126 | 25/41 | 0.37/255 |

**结论一：提高内部色深不消除台阶。** B 与 A **逐位相同**——台阶产生在最后一次
8 位量化上，中途用多少位都改变不了终点。高位深只能减少「多次混合/模糊的累积误差」
（毛玻璃叠多层、大尺寸渐变），不能改变单次量化的台阶。

**结论二：抖动也救不了“幅度不足”。** 1/255 幅度跨 666px 时，整段变化量只等于
**一个**量化级——抖动只能让临界行附近交织（全平窗口 40→25），最长平台仍 333px。
那里本来就没有信息，抖动无处发力。

**结论三：真正的判据是“跨级数”，不是“位深”。**

| 渐变跨度 | 目标平台 | 需级数 | 8 位够吗 |
|---|---|---|---|
| 666px | 4px | 167 | 可以（7.4 位） |
| 666px | 16px | 42 | 可以（5.4 位） |
| 2160px | 8px | 270 | **不够**（8.1 位） |

8 位对绝大多数界面渐变**够用**。缺的不是色深，是**幅度**。
幅度 32/255 跨 666px（约 20px 一级）就肉眼不可辨了。

**如果将来真要做“双色深”，成本与收益**：

| | 内部 8 位 | 内部 16F |
|---|---|---|
| 单次量化台阶 | 有 | **有**（终点仍 8 位） |
| 多次模糊/混合累积误差 | 会累积 | **显著改善** |
| 大尺寸（>1000px）渐变 | 可能不够 | **够** |
| CPU 成本 | 基准 | +30~60%（内存翻倍） |
| GPU 成本 | 基准 | **近于零**（FP16 算力等同或更高） |

假设做，合理的默认是：**软件端 8 位、GPU 端 16F**，均可用选项覆盖。
但注意：三个硬编码点会撑开改动面——`Color` 是 `uint8_t[4]`、画布像素是
`vector<uint32_t>`、`kFormat` 是 `R8G8B8A8_UNORM` 常量（`platform_d3d11.cpp:921`），
8 位假设散落在 22 个文件、约 490 处。

**建议**：真要提高渐变质量，优先做「幅度自适应 + 抖动」而不是色深开关；
后者留给真有场景时（HDR 输出、导出高色深图）。

### `still_ratio` 会把「元素一直在动」的镜头判成静止（2026-10-27）

**现象**：一条三镜片子（`styles/06-spine/film-multishot.json`）的第 2 镜，
元素轨迹显示 **0 帧对静止 / 102 帧对全在动**，而像素口径报 **静止 96.1%**。
两者相差 96 个百分点。

**实测数据**：

| 口径 | 幕 1（3.20–6.60s）静止占比 |
|---|---|
| 元素轨迹（`--track-elements`，真值） | **0.0%**（102/102 帧对都在变） |
| 像素 `still_ratio`（64 宽面积加权，帧差 <1.2） | **96.1%** |
| 全片 `moving_pair_ratio`（192 宽，Δ>6） | 18.2%（与真静止版 17.8% 几乎无差） |

**根因（两条，都是量尺而非片子的问题）**：

1. **面积加权稀释**：那条光带占全屏 **0.090%**。即使亮度从 0 变到 200，
   全屏平均差也只有 **0.18**，远低于判据 1.2。
   实测代价：三条不同动效改动（缩放呼吸 / alpha 脉冲 / 连续流光带）
   把 `格均差峰值` 从 12 抬到 **50（4 倍）**，而 `still_ratio` 纹丝不动停在 96.1%——
   凭它调参就是“怎么改都无效”。
2. **`moving_pair_ratio` 不区分「本镜在动」与「入场在动」**：
   把光带与脉冲全关成真静止后，全片口径只从 18.2% 掉到 17.8%（入场动画占了大部分）。

**方向**：判“某镜是不是卡住”应以**元素轨迹**为准（`--track-elements` 已有真值，
`scripts/carry_qa.py` 已能读），像素口径只作“整体观感是否闪动”的粗筛；
或在 `ShotStats` 里同时出轨迹口径的逐镜在动比例。当前 `still_ratio` 的措辞
（“静止占比”）容易让人把它当成“没东西在动”，建议在 summary 里注明
它只反映**大面积**变化。

### 暗色 `text_faint` 在 `surface_raised` 上只有 3.60:1（2026-10-27）

**现象**：`theme.cpp` 自己的注释要求 `text_faint` 对四个底面均 ≥4.5:1，实测暗色下：

| 底面 | 对比度 |
|---|---|
| `bg` | 5.53 ✓ |
| `surface` | 4.86 ✓ |
| `surface_alt` | 4.17 ✗ |
| `surface_sunken` | 5.69 ✓ |
| **`surface_raised`** | **3.60 ✗** |

亮色四个底均 ≥4.54，无问题。

**为何没列为阻断项**：`surface_raised` 是弹层底（菜单/命令面板/模态），而这些组件
当前在那里用的是 `text_muted`（暗色对 raised 为 5.98:1，达标），没有组件在该组合上
真用 `text_faint` —— 是**潜在**缺陷，不是当前渲染错误。

**为何仍是 P1**：弹层下一旦用小字（快捷键提示、分组标题）就会撞上；而 `font_2xs`
本轮落地后，小字使用只会更普遍。

**方向**：抬暗色 `text_faint` 会影响四个底的综合值（要重跑那张可行性不等式）；
或在 raised 面上另给一档——后者更可能，因为 raised 在暗色里比 alt 还亮一档，
而 `text_faint` 的职责是“能读但不抢戏”，再抬就抢戏了。

### 字号阶梯的后续观察（2026-10-27）

本轮已补 `font_2xs=11` 并把 `font_xs` 从 13 降到 12（阶梯 11/12/14/15/17/20/26/34），
gallery 的阶梯外字号从 49 处降到 0。两点值得继续看：

- **12→14 跳 2px**：阶梯里唯一的大跳。若日后出现“12 太小、14 太大”的实测场景，
  说明还需要一档（例如 13）；当前没有实测需求，不预先加。
- **框架组件的小字整体缩了 1px**（表头/面包屑/字幕/徽标/时间轴刻度都用 `font_xs`）。
  这是选“补一档并下调 xs”时的已知代价。若某处观感变挤，先看那里是否该改用 `font_sm`，
  而不是把 `font_xs` 调回去。

### 内阴影的后续扩展（2026-10-27）

`Shadow::inset` 本轮已落地（四条渐变带，见 `DESIGN.md` §4.2.5），目前只在
`Input`/`TextArea` 上启用。未做但值得做：

- **列表选中行 / 按下态**：`Button` 按下已有 `press_sink`（几何位移），
  再叠一层内阴影会更“按实”。收益中等，需先确认不会与 hover 提亮打架。
- **代码块 / 引用块**（`MarkdownView` 内）：这些是静态面，内阴影能让“引用区”
  读起来像被框进去的，而不是仅换了个底色。

### Windows 原生链路对标挖出的字体渲染缺口（2026-10-21，像素级证据）
### Windows 原生链路对标挖出的字体渲染缺口（2026-10-21，像素级证据）

> 完整报告：`docs/DWRITE_PIXEL_BENCHMARK.md`。与 `TEXT_AB_REPORT.md` 互补——
> 那份参照是 Linux+FreeType/Chromium，本份是 **Windows 原生 DirectWrite + Chrome 真窗口**。

**三点先记住**（都是能推翻旧结论的口径变化）：
1. 本机系统 DPI 是 **96 = 100% 缩放**（旧报告全是 1.5×）；
2. Windows 首选字体是 **微软雅黑（TrueType）** ⇒ **CFF stem darkening 在此完全空转**，
   旧报告「汉字已对齐（CFF 加墨功劳）」不能平移；
3. **浏览器 > DWrite 直调**（实心 50.0% vs 44.9%）——Skia 在 DWrite 之上还有对比度增强，
   所以 **用户所见的参照是浏览器，不是 DWrite**。

**实测缺口**（霜天应用默认档 vs 浏览器真窗口，18 行覆盖汉字/拉丁/数字/等宽/9~24px）：

| 量 | 值 | 含义 |
|---|---|---|
| 总墨量 | **1.037** | ≈持平（不是缺墨） |
| 实心像素 | **0.801** | 该实心的没实心 |
| 过渡像素 | **1.181** | 该空的没空 |
| 墨像素数 | **1.09** | 墨摊到了更多像素上 |

根因是**对比度压缩**：逐覆盖率分桶的差值曲线单调（+0.107 → 0.000 → −0.143，交叉在 0.65）。
归因已分离：**几何无差**（逐行墨迹宽 ≤1px）、γ 只平移不改斜率、
**主因 = 亚像素滤波把子像素覆盖率摊平**（满黑像素占比：霜天 12.2% vs DWrite 21.0%）、
次因 = 缺 Skia 的对比度增强环节。

**按字号分层（最值得动手的地方）**——实心比：

| 物理px | 13.5 | 15 | 16.5 | 18 | 19.5 | 21 | **24** | 27 | 36 |
|---|---|---|---|---|---|---|---|---|
| 实心比 | 0.620 | 0.648 | 0.687 | 0.776 | 0.700 | 0.759 | **0.912** | 0.917 | 0.946 |

**24 物理 px 是分水岭**；17~21 px 差距最大（正文 13~14px@100% 正落在这里）。
⚠ 小字号是**偏淡**（不是偏重）——墨摊到 17~30% 更多像素上。

**候选（按性价比，均为产品决策，未擅自改默认值）**：

- [ ] **P1-A LCD taps 收窄到 `{0,51,205,51,0}`**：对 DWrite 参照，实心 0.910→**0.992**、
      过渡 1.075→**1.005**、每墨像素墨量 0.688→0.721（参照 0.722）、彩边 **反而更接近**参照（64.3 vs 63.6）。
      **零代码改动**（`ST_TEXT_LCD_TAPS` 已存在），但改默认值需真机观感验收。
      逆向验证：同构建只改该变量，实心 +4.2pp、3.73% 像素改变（非缓存假象）。
- [ ] **P1-B 把覆盖率校正移到绘制阶段（带文字色）**：这是唯一能追上「浏览器」这一档的路
      （最优 taps 也只能到实心 0.958/过渡 1.040；差额就是 Skia 那层逐像素 LUT）。
      架构级——`SKIA_TEXT_RENDERING_STUDY.md §三` 记的缺口在此得到定量印证。
- [ ] **P1-C 按字号分档**（≤24 物理 px 单独一档）：`set_fitted_gamma` 接口已有、默认关。
      ⚠ 旧报告「测不出字号依赖」是 **Linux** 结论；Windows 上 0~24px 的落差是 0.62→0.91，**不是噪声**。
- [ ] P1-D 拉丁/等宽单独分档：最优 taps 下 latin 实心过冲 1.187、mono 仍偏淡 0.927。
- [ ] P1-E **在 100% 与 150% 两种系统缩放上各标一次**（本报告只测了 100%）。

**设施**（全部可复跑）：`tools/gen_dw_ab.py`（单源行表）、`tools/dwrite_ref.cpp`（DWrite 直调三模式）、
`tools/dw_ab_probe.cpp`（霜天应用默认档）、`tools/dw_ab_shot.mjs`（headed+CDP 截图+字体归属核对）、
`tools/dw_ab_diff.py`（量尺）、`tools/dw_ab_sweep.py`（旋钮扫描）。

**量尺坑**（这轮踩到，别再犯）：屏幕 `CopyFromScreen` 会被全屏窗口遮挡（截到的是 IDE）→
改用 CDP `page.screenshot`；青色标记框必须裁掉否则被算成墨；两侧基线口径不同源必须各自按行投影配对。

### P2：2026-10-20 全方向探索的否证合集（**以下都别重测**）

这一轮把“通用优化方向”逐个量了一遍，**绝大多数被数据否定**。列在这里是为了
让下一个人不用重走：

| 方向 | 实测 | 结论 | 工具 |
|---|---|---|---|
| 未归因的 0.42 ms 绘制开销 | 6 次重测**恒为 0.00 ms** | **是测量假象**（最后一帧 vs 全部分位相减） | `examples/gallery` 的分解输出 |
| 整树遍历（`paint`/`layout`） | 1200 节点 42 µs | 远小于帧时间，非主因 | `partial_frame_overhead_probe` |
| 帧末整树收尾（`collect_animation_requests` 等） | 300 节点 9.3 µs | 同上 | `frame_epilogue_cost_probe` |
| 裁剪剔除是否生效 | 小脏区/全屏 = **2.41×**，屏外/全屏 = 4.56× | **生效**，无缺陷 | `cull_effectiveness_probe` |
| 布局重复计算（measure vs arrange） | 96 段文本全量重排 **23.4 µs**（0.23 µs/段） | 文本宽度缓存有效 | `layout_duplicate_work_probe` |
| 绘制路径堆分配抖动 | 前/后 100 帧 **1.02×** | 无碎片累积 | `frame_allocation_probe` |
| 阴影缓存命中率 | 1 张 5.70 µs → 96 张 6.14 µs（**+7.7%**） | 命中良好，无容量问题 | `shadow_amortized_probe` |
| `covered_region` 是否生效 | 后端确实跳（**1.10×**） | 生效但**收益小**（阴影像素大多在卡片外侧） | `covered_region_verify_probe` |
| 文本宽度缓存容量 | 16 → 4096 条，单次 measure 恒定 **0.07~0.08 µs**（1.10×） | 容量充足，无拐点 | `text_cache_limit_probe` |
| GPU 字形缓存线性查找 | 512 项时 **27 µs**（占总 `text` 的 **4%**） | 不是瓶颈，**不必改哈希表** | `gpu_mask_cache_probe` |

### P1：**GPU 路径的真实画像**（与软件完全不同，别再拿软件结论套）

实测（gallery 1600×1000，release，`--bench 60`，5 次取最小）：

| 渲染器 | 总帧 p50 | 绘制 p50 |
|---|---|---|
| `software` | 3.72 ~ 4.77 ms | 4.54 ms |
| **`gpu`** | **1.53 ms** | **1.40 ms** |

**GPU 快 2.4~3.2 倍**。而逐原语的对比则完全反过来了：

| 原语 | GPU | 软件 | 倍数 |
|---|---|---|---|
| `round_rect` | **0.02 ms** | 0.88 ms | **44×** |
| `shadow` | **0.01 ms** | 0.63 ms | **63×** |
| `fill_path` | 0.03 ms | 0.40 ms | 13× |
| `clear` | 0.00 ms | 0.25 ms | — |
| **`text`** | **0.67 ms** | 0.58 ms | **1.15×（GPU 反而慢）** |

**结论**：

1. 本轮之前所有渲染优化（几何贴图缓存、阴影游程分组、文字查表、LCD 减除法）
   **全部只改了软件光栅器**——对 GPU 路径**没有影响**。
2. GPU 上 **`text` 占 84%**（0.67 / 0.80 ms），因为每字形 **2 次 draw call**
   （LCD 的 `lcd_attenuate` + `lcd_add`）；305 字形 → **610 次 draw/帧**。
3. **不是缓存问题**：`mask_cache_` 的线性查找只占 27 µs（4%）。
   改哈希表**不值得**。
4. GPU 侧也有 **37% 未归因**（0.47 ms）——待查，可能是一次性的
   常量缓冲更新与状态切换。

**GPU 文字的真优化方向**（未做，架构级）：**字形纹理图集**——把所有字形
打包进一张大纹理，整段文字一次 draw。当前每字形一次纹理绑定 + 两个 draw，
是 GPU 侧最大的单项。收益上限约 **0.6 ms/帧**（目前 GPU 整帧 1.4 ms）。

#### 尝试进展（2026-10-21）：底层已完成，**接线遇到 LCD 阻碍，未上线**

已实现且测试全绿（9 项、1536 断言）——这两份**保留在仓库中**，将来接着做可直接用：

| 文件 | 职责 |
|---|---|
| `include/st/raster/glyph_atlas.hpp` + `src/raster/glyph_atlas.cpp` | 货架式**位置**分配（`ShelfPacker`）：无重叠、不越界、放不下时不改状态 |
| `include/st/raster/glyph_atlas_content.hpp` + `src/raster/glyph_atlas_content.cpp` | **内容**管理（`GlyphAtlas`）：按 key 命中、扩容时**保留旧像素重新装箱**（120 条逐条校验通过） |
| `tests/raster_glyph_atlas_test.cpp` / `..._content_test.cpp` | 9 项不变量测试 |

**接线的硬阻碍（实测，别再重试同样思路）**：

- 灰度（`CoverageFormat::Grayscale`）合批**已验证可行**：
  亚像素测试里把灰度走合批、LCD 仍逐字，结果 **PASS**（结构性 0.003%）。
- 但 **LCD（亚像素）合批会丢彩边**：结构性差异 **0.814%**、彩边像素 **289→0**。
  已排除的可能：写入端（探针确认图集里 R/G/B 三通道 99% 不同）、
  UV 半像素偏移、灰度/LCD 共用纹理视图、图集上传时机（新增条目后已全量重传）。
- **根因未定位**。已确认的隔离现象：
  ① 关掉合批（`ST_GPU_GLYPH_BATCH=0`）就恢复正常；
  ② 只让灰度合批、LCD 逐字 → 正常；
  ③ 让 LCD 也合批 → 0.814% 差异。
- 而 **gallery 与多数真实界面默认走 LCD**（`TextRenderer::subpixel_`），
  所以“只合批灰度”**实际收益为零**（实测 GPU 帧 1.53 → 1.72 ms，反而慢 0.19 ms）。

**下次接着做的切入点**：

1. 先查 `vs_glyph` 里 `instance.uv` 与 `g_rect.zw` 的组合是否让 PS 的
   `input.local / g_rect.zw` 在**扩 1 像素的边界处**算错（主路径的 `local` 相对字形，
   合批路径的 `local` 是图集绝对坐标，两者边界行为可能不同）；
2. 或改用**图集 + 每字形一个顶点缓冲**（不搞实例化），先验证“图集本身”是否
   在 LCD 下正确——把“图集”与“合批”两个变量拆开单独验证。

> 教训：这次把“图集”与“合批”两个改动**同时**引入，导致差异归因耗费很多轮。
> 下次应先做“只换图集、不合批”（每字形仍单独 draw，但采样自图集）——
> 那一步能直接把问题分成“图集采样错”还是“合批顺序错”。

#### 2026-10-21 第二轮尝试：拆分成功一半，但**图集本身有数据完整性缺陷**

**已取得的确切结论（下次直接复用）**：

| 步骤 | 结果 |
|---|---|
| ① **只换图集、不合批** | ✅ **完全正确**：亚像素结构差 **0.000%**、彩边像素 **289 vs 289**（完全一致）；而且比原路径**更准**（最大 Δ 从 7/72 降到 **2/16**），小界面 0.477% → **0.057%** |
| ①的性能 | GPU 1600×1000：**1.83 → 1.53 ms（−16%）** |
| ② 再加**合批**（`DrawInstanced`） | ❌ 结构差 0.814%、彩边 **289 → 0** |

**合批的具体阻碍**：把 `ps_batch` 改成返回纯色，**画面完全不变**——说明那批
`DrawInstanced` 的产出根本没进最终画面（不是“算错了”）。已排除：输入布局槽号
（`INSTANCE0`）、`SV_VertexID` 每实例重置、`local` 归一化时机、实例缓冲槽位。

**而图集①本身有一个数据完整性缺陷（这是最终回退的原因）**：

- `text_glyphs_stay_correct_across_glyph_cache_trim_on_gpu` 报 **7018 像素错**
  （这条正是历史上“`folder` 显示成 `folBer`”的回归护栏）；
- 关掉图集（`ST_GPU_GLYPH_ATLAS=0`）就通过，确认是图集引入；
- 已排除：键复用（已加内容指纹，仍 7018）、图集扩容（测试内 4530 次 place
  均在 512×512 内、未扩容）、纹理上传时机；
- **未定位到真因**，而它是像素正确性缺陷，不能上线。

**下次的三个切入点**（按性价比排序）：

1. **先查“图集容量溢出”路径**：4530 次 place 在 512×512 内不可能全放下，
   後续字形会 `place` 失败 → 退回独立纹理 → **同一帧里两套纹理混用**。
   先确认这条混用是否自洽（尤其 `g_uv` 的缺省值、以及已入队字形的坐标）。
2. 把图集初妨尺寸调大（如 2048×2048）复测那个用例——若变绿，就是容量溢出。
3. 若仍红，改用**每字形一个独立“小图集”**（如 256×256 只放 16 个字形）
   逐步缩小范围，而不是直接上全局单图集。

#### 2026-10-21 第三轮：**7018 像素错的真因找到并修复，图集已上线**

**真因**：`atlas_sample` 只在 `fresh` 时设“脏标记”，而**只有批次路径消费那个标记**；
逐字形路径（不合批）不入队、不 flush，于是**图集内容变了但纹理从未重传**
——画出来的是旧内容/空。这和“容量溢出”无关，与键复用也无关。

**修法**：新增条目后**立即重传**（`fresh && !upload_atlas(...) → 失败退独立纹理`）。
两条约束必须同时满足：

| 约束 | 不满足的后果（实测） |
|---|---|
| 新增条目必须**立即**重传 | `text_glyphs_stay_correct_...` 报 **7018 像素错** |
| 命中时必须**不碰纹理** | 每字形传 1 MB → **92 ms/帧**（比不用图集慢 60 倍） |

**上线后的实测**：

- 正确性：亚像素结构差 **0.000%**、彩边 **289 vs 289**；文字最大 Δ **0**；
  小界面 0.477% → **0.057%**；且最大 Δ 从 7/72 降到 **2/16**（**比原路径更精确**，
  因为图集统一走 RGBA8，消掉了 R8 路径的量化误差）。
- 性能：**无可测收益**（同机交替测 1.68~1.87 ms 来回跳，在噪声内）。
  原因：gallery 的瓶颈是 **draw call 数**，不是纹理对象创建——而减少 draw call
  需要合批，合批又卡在 `DrawInstanced` 那条路上（见下）。
- 真实收益是**资源**：字形纹理对象 **512 → 2**（灰度 + 亚像素各一张）。

**仍然是未完成的那一半**：合批（`DrawInstanced`）无法定位——把 `ps_batch` 改成
返回纯色、画面完全不变，说明那批 draw 的产出没进最终画面。图集就位后，
下次可以从“顶点缓冲（不做实例化）”重试，或先查 `submit_glyph_batch` 里
渲染目标/视口与 `submit` 的差异。

#### 2026-10-21 第四轮：合批**实现并正确**，但粒度太细→零收益，默认关闭

**`DrawInstanced` 的谜团解开了**：换成**顶点缓冲**（不用实例化）后一次就对。
`vs_batch` / `ps_batch` 改用 `POSITION` + `TEXCOORD0..3` 语义，输入布局
`CreateInputLayout` 成功、draw 正常出图。**结论：那条 `DrawInstanced` 路径
的失败很可能就在输入布局静默失败**（当时用 `INSTANCE0` 作语义名、且返回值被忽略）。

**合批后逐像素正确**：亚像素结构差 **0.000%**、彩边 **289 vs 289 / 1091 vs 1091**。

**但无收益，且 p95 变差**：

| | p50 | p95 | 最大 |
|---|---|---|---|
| 合批开 | 1.47 ms | **5.31 ms** | **10.20 ms** |
| 合批关 | 1.56 ms | 2.31 ms | 2.78 ms |

**根因（实测数据）**：每帧 **244 次 flush**、平均每批仅 **4.7 个字形**（28 顶点/6）。
原路径 305 字形 = 610 次 draw；合批后 244×2 = **488 次**——只减 20%，
却多了顶点缓冲上传与常量重设。

**为何粒度这么细**：`submit()` 里必须 flush（保证绘制顺序），而 UI 树是
「边框 → 背景 → 文字 → 下个元素」顺序绘制的，字形天然被其它原语打散。
**这是“顺序绘制”这个前提决定的，不是实现问题。**

**要拿到收益必须先改渲染流程**：把整帧的字形先收集起来、最后统一画一次
（类似“延迟绘制”层）。那需要改动 `UiRoot::paint_frame` 与 `Element::paint`
的契约（元素不再直接画字形，而是登记到帧级列表）——属于架构级改动，
而当前 GPU 帧 1.5 ms、60 Hz 预算 16.7 ms，余量充足，**不建议现在做**。

**当前状态**：合批代码保留且已验证正确，**默认关闭**
（`ST_GPU_GLYPH_BATCH=1` 可开启）；默认走图集 + 逐字形（已验证）。

### P1：整帧成本的**真实**结构（已归因，附 19 倍差异的真相）

**为何要记**：不同测法给出的帧耗时差 **19 倍**，曾让我以为是性能缺陷。

| 场景（96 卡片 + 13px 文字，1280×800） | release 档 | 相对空面板 |
|---|---|---|
| A 空 `Panel` + `paint()` | **123.9 µs** | 1.00× |
| **B 换成 `Card`（带两层阴影）** | **1569.7 µs** | **13.07×** |
| C 再改走 `paint_frame()` | 1716.4 µs | 14.29× |

**结论**：

1. **`Card` 的两层阴影就是 13 倍差距的全部来源**（1446 µs / 96 张 ≈ 15 µs/张）。
2. `paint_frame` 比 `paint` 多出的工作 **≈ 1.2 倍**（损坏区收集 + 动画汇总）。
3. 剩下那截是 **release ↔ debug 档差**（约 3 倍）——`st test` 跑 debug（-O0）。
4. 所以“`st test` 报局部帧 2.46 ms”与“探针测整帧 130 µs”**两个数都对**，
   差在**档位 + 是否用 Card**。引用任一数字必须四元组同行：
   **档位 / 控件类型 / 测法（paint vs paint_frame）/ 树规模**。

### P1：**渲染的下一个真方向是“减内存流量”**（架构级，未开工）

> 这是 2026-10-20 一整轮遍历后的总定位：**渲染侧所有原语都已摸到带宽/物理下界**，
> CPU 层面再没有低垂果实。下面的逐项定案都在同一结论上：
>
> | 原语 | 现状 | 为何到头 |
> |---|---|---|
> | `clear` | 0.21 ms | 已达单层整屏写理论下界（`frame_floor_probe` 0.145 ns/px） |
> | `fill_path` | 0.39 ms | **0.290 ns/px** vs `simd::over_row` 物理下界 **0.262** |
> | `round_rect` | 0.86 ms | 大面板近下界；小控件已走几何贴图缓存（9.2×） |
> | `text` | 0.55 ms | LCD 单价 5.92 ns/px，落在短行合理区间；再快只能放弃 LCD |
> | `shadow` | 0.58 ms | 多层合并贴图已落地（`draw_shadow_layered`） |

#### 为什么“减流量”是真方向

实测（`tools/memory_bandwidth_probe.cpp`，见下方 P1 条目的完整数据）：

- 纯写型工作（`clear` / 整行填充）单线程已达 **11.47 GB/s**，
  而那台机器的单通道内存上限就在这个量级——**已经饱和**；
- 多线程只会变慢（2/8/16 线程 0.72× / 0.62× / 0.38×）；
- `simd::over_row` 是**读 + 写**，带宽需求翻倍。

所以继续优化 CPU 指令数没有意义，**唯一的真方向是减少要搬运的字节数**。

#### 两个候选（都是架构级，均未评估可行性）

1. **16 位像素格式**（预乘 RGBA4444 / RGB565 + 单独 alpha）
   - 收益：带宽减半；`over_row` 读+写都减半 → 理论上该段快近 2×。
   - 风险：**量化精度下降**。本项目对像素是零容差（见 `CONVENTIONS` §7.1），
     现有回归测试大量用逐像素相等断言——换格式后整批会变红，
     而那些测试抓的正是真缺陷（如本轮 `alpha_table` 的 1 LSB 问题）。
   - 必须先回答：**哪些测试是可以放宽到容忍的，哪些必须保持零容差**。
     在没想清这一点之前不能动。
2. **分块渲染 / 保留模式**（tile-based，只重算脏 tile）
   - 收益：静置帧已靠脏区拿到 22×（0.69 ms vs 15.0 ms），但**整帧路径仍在**；
     分块能把“整帧必须扫全屏”变成“只扫脏 tile”。
   - 风险：与现有 `Surface` 抽象、GPU 后端（swapchain 整帧送显）、
     局部上屏（`set_present_region`）三处都要重新对齐。工作量大得多。

#### 开始之前必须先回答的问题

1. **目标场景是什么**？当前 gallery 1600×1000 整帧 **3.52 ms**（60 Hz 预算 16.7 ms，
   余量 4.7×）。在没有“实际掉帧”的场景下做架构级重构，风险大于收益。
2. **GPU 路径是否同样受益**？GPU 后端整帧仅 ~1 ms（走 swapchain），
   而 16 位格式首先要改的是**软件光栅器**——可能只是优化了一个非主路径。
3. **验收口径怎么定**？现在有 `visual_diff` / `capture_hash` 做像素回归；
   换格式后需要一套新的“量化误差分布”验收，否则无法判断“变差了多少”。

#### 重启这个方向的条件

出现下面任一条再开工，而不是“因为它是剩下最大的优化”：

- 有用户反馈的具体掉帧场景（哪个操作、什么分辨率、多少 Hz）；
- 或 `gallery --bench` 的 p95 超过 16.7 ms（当前 4.5 ms）；
- 或不加 16 位格式就追不上目标帧率。

### P2：`fill_path` 已到物理下界——**不建议再优化**

**结论（2026-10-20 实测）**：这条路径没有余量了。

| 项 | 实测 | 说明 |
|---|---|---|
| 几何贴图命中单价 | **0.290 ns/px** | 63832 px 的圆角矩形 |
| `simd::over_row` 物理下界 | **0.262 ns/px** | 长 2048 行（`tools/simd_row_cost_probe.cpp`） |
| `simd::blend_row` 物理下界 | **0.184 ns/px** | over_row 本就是更贵的那个（解包逐像素源色） |
| 缓存未命中（建图） | 0.702 ns/px | 命中快 **2.42×** |

命中路径 0.290 vs 下界 0.262——**只剩 11%**，且那 11% 是画布/裁剪的合法开销。

**命中率健康**（`tools/geometry_hit_rate_probe.cpp`）：完全相同 98.3%（首帧必 miss）、
浮点噪声 `+1e-7` **100.0%**（键按 0.25px 量化，噪声被吸收）、
量化内 `+0.1px` 1.7%、量化外 `+0.3px` 33.3%。

**已排除的假设**（别再试）：

1. 「缓存没帮上忙」——开/关 = 0.23，命中快 **4.3×**，缓存有效。
2. 「`over_row` 比 `blend_row` 慢很多」——长行只差 **1.42×**，不是几十倍。
3. 「`fill_rect` 的 0.028 ns/px 是可追目标」——它的 1.42 M 像素里大部分
   被“跳过同色整屏底”优化跳过了（**根本没画**），不能当基准。

**唯一可能的剩余空间**（未做，价值低）：把几何键的量化粒度从 0.25px 放宽到 0.5px，
可提高滚动/缩放场景的复用率——但会降低贴图精度，需先量实际收益。

### P1：文字混合已改查表，且 LCD 路径已减除法——剩下的是“LCD 固有的 1.5× 代价”

**已修（2026-10-20）**：

1. `blend_coverage_row`（灰度）：逐像素 `scale_premul` → **按颜色缓存的 256 项表**。
   gallery `text` **0.67 → 0.59 ms（−12%）**。
2. `over_premul_lcd`（亚像素）：**源色不透明时的快路径**——
   `attenuation = fast_div255(255×α+127)` 恒等于 `α`，每个通道省一次乘+移位。
3. `blend_coverage_row_subpixel`：`masked` 判断提到循环外，无遮罩走无分支版本。

**LCD 位图单价：7.69 → 5.92 ns/px（−23%）**（`tools/lcd_text_cost_probe.cpp`）。

### 本轮量出的关键事实（别再重测）

| 问题 | 实测结论 | 出处 |
|---|---|---|
| 文字成本在排版/字形查找？ | **不是**：`measure` 只占 `draw` 的 **1%** | `text_subtract_probe` |
| 每行裁剪查询是主因？ | **不是**：`clip_rect` 0.8 ns + `has_mask_clip` 1.3 ns，每行共 **2.1 ns**，13 行合计只占一次调用 **3%** | `text_row_overhead_probe` |
| 字形位图有空行白走？ | **不是**：空行占比 **0.0%** | `glyph_row_waste_probe` |
| 文字 11 ns/px 是不是异常？ | **不是**：LCD 路径单价就是 **5.9~7.7 ns/px**（灰度 4.7），落在短行合理区间（3.6~6.1） | `lcd_text_cost_probe` |
| LCD vs 灰度贵多少？ | **1.43~1.66×**（每像素 3 通道 + 8 次 `fast_div255`） | `lcd_text_cost_probe` |
| 跨场景可比口径 | **1.56 µs/字符**（LCD，含混合，13~16px） | `text_subtract_probe` |

**“61 ns/px”那个数字是错的**：它用“与底色不同的像素”当分母，而小字号覆盖率小于
1/255 的像素被量化成 0（根本没画）——**分母漏掉了大量真实参与的像素**。
跨探针比较必须用**同一位图配置**（灰度 vs LCD）+ **同一分母定义**。

**未做项**：

1. **LCD 的 1.5× 是否值得**——这是**产品决策**（视觉锐利度 vs 帧时），不是缺陷。
   若某天要回退，`TextRenderer::set_subpixel(false)` 即可（又能拿回 ~33% 的 text 时间）。
2. **减少每行调用次数**（`blend_coverage_bitmap` 逐行调 `blend_coverage_row`）：
   实测每行固定开销只有 2.1 ns（占 3%），**收益上限很低**，不建议做。

### P1：阴影混合已改“按同 α 游程分组”——剩下的是多层合并与建图

**已修（2026-10-20）**：`draw_shadow` 的 `SrcOver` 快路径把行内**连续同遮罩值**的像素
当作一段：长段（≥ 8）走 `simd::blend_row`，短段逐像素；`mask_byte == 0` 的整段
直接跳过（比逐像素判断省）。

实测（`tools/shadow_blend_route_probe.cpp`）：

| 路线 | 耗时 | 相对 |
|---|---|---|
| A 逐像素 | 3.915 ms | 1.00 |
| B 按段分组 + `blend_row` | **2.181 ms** | **0.56（1.79×）** |

且 **A vs B 逐像素 0 差异**。gallery 上 `shadow` 单项 **0.84 → 0.62 ms（−26%）**。

**为何这条路成立**（两个先验，都是量的不是猜的）：

- `tools/shadow_mask_distribution_probe.cpp`：α=0 只占 **16~27%**，所以
  “再跳透明像素”最多就这么多，优化空间不在那里；
- `tools/shadow_run_length_probe.cpp`：同 α 游程**中位长度 1**，但
  **≥8px 的游程覆盖 80~85% 的像素**——典型的“少数长游程 + 大量孤点”，
  正是分组策略的适用形态。

**回归护栏**：`tests/paint_optimization_test.cpp` 的
`shadow_run_grouping_matches_per_pixel_blend`——同一进程、同一块画布，
只切 `set_shadow_run_grouping_enabled()`，5 个档位（含半透明 opacity 0.35、
彩色阴影、直角 blur8）必须 **Δ=0**。

⚠ **为何不复用 `layered_shadow_*` 那几条**：它们验的是 `draw_shadow_layered`
（走 `covered_region` 的另一条分支）。实测：把游程起点错写成 `run_end = x`
（长度归零、一声不响地不画）时，那 6 条用例**全部仍然绿**。

**未做项**（按价值降序）：

1. **多层阴影合并成一张贴图**（`docs/PAINT_DIAGNOSIS.md` §3.1 已证 1.8×）：
   两层遮罩非零区域高度重叠（并集只有 15744 px，而两者之和是 27808）。
   `src-over` 结合律保证“先合并再贴”与“两次依次贴”逐像素等价。
   需改调用方（把两层交给一次调用），改动面比本轮大。
2. **`shadow_mask` 建图本身**：现在只在首次建（已缓存），但缓存键是几何参数
   量化到 0.25 px——滚动/缩放时容易全部未命中。未量过。

### P0：圆角矩形每帧真光栅化 2 次 —— **已修**（附未做项与实测数据）

**原现象**：`fill_rect(radius > 0)` 的三带拆分里，上带与下带**各调一次**
`rasterize_path_runs(path)`（同一条完整圆角路径），中带另走一次。
即同一个形状的路径光栅化每帧跑 **2 次**（不是 1 次），且**完全不走几何 sprite 缓存**。

**已修（2026-10-20）**：

1. **圆角矩形先试几何贴图缓存（整块）**。抽出了 `fill_physical_path_with_cache`，
   让 `fill_path` 与 `fill_rect` 的圆角分支共用同一套缓存逻辑（后者建路径时已是
   物理坐标，直接交给 `fill_path` 会被双重缩放）。
2. **删掉三带拆分**，缓存不适用时退整块光栅化。上带/下带重复光栅化因此消失。

**实测收益**（`tools/round_rect_route_probe.cpp`，1600×1000，每项 50 轮取最小）：

| 形状 | 改前 | 改后 | 倍数 |
|---|---|---|---|
| 按钮 96×32 r6 | 5.371 ns/px | **0.586 ns/px** | 9.2× |
| 卡片 320×160 r12 | 0.850 ns/px | **0.303 ns/px** | 2.8× |
| 面板 720×480 r16 | 0.374 ns/px | **0.354 ns/px** | 1.06× |

整帧（gallery 1600×1000，10 次取最小）：
**4.28 → 3.55 ms（−17%）**；中位 4.53 → 3.74 ms。
`round_rect` 单项 1.38~1.93 → **1.05 ms**。

**等价性**（`tools/round_rect_diff_locate_probe.cpp`，8 个档位逐像素对比
非整坐标/非整尺寸/半透明/极扁/胶囊/超画布）：
**7 档零差**，1 档在圆角弧最外缘差 **1 LSB**（`120×44 r12` 的 `(133,21)`，
仅 R 通道 123 vs 122）。成因：贴图把预乘色量化成 8 位再 `over`，
与直接路径的一步量化在半格像素上差 1。这是**贴图机制的固有代价**。

**关键教训（写进 PITFALLS）**：定位差异时如果“预热”和“正式”两次绘制留在
同一张画布上，AA 边缘会**叠加变深**——实测那会把真实差异虚报成
**80~5188 像素 / 通道差 50**，而正确量法（预热后 `clear` 回干净底再画一次）是 **0~1 像素**。

**未做项**：

1. **贴图只存覆盖率、颜色在贴出时乘**（本次尝试后**主动放弃**）：
   8 位覆盖率会把 `opacity × 色α` 的量化误差放大，实测 `opacity=0.35` 下
   留下 **20 像素 / 通道差 1**；要零容差必须存 float 覆盖率并让建图绕过画布
   （见下一条），成本远大于收益。
2. **若将来要做第 1 条**：需要给 `detail::fill_path_aa` 加一个“只输出 float 覆盖率”
   的变体（现在它只往画布混），否则精度在源头就丢了——8 位还是 16 位都救不了。
3. **`show_when_ready` 91~150 ms 的构成未拆**（与渲染无关，属启动路径）。

### P2：启动成本已量化——**两段大头均不可动**（2026-10-20 实测，附口径陷阱）

#### 先把口径说清（否则数字不可比）

`tools/startup_latency_probe.py` 曾报 **660 ms**，真实值是：

| 场景 | 控制通道就绪 |
|---|---|
| **headless** | **271 ms** |
| **带真实窗口** | **644 ms** |

差异 373 ms = **窗口创建**（其中 D3D11 建设备 357.8 ms）。
告警：旧探针**没传 `--headless`**，所以量的本来就是真窗口——它没测错，是**读的人拿它当 headless 数**。

另外两个曾被我误判为原因的因素，实测都**不成立**：

| 怀疑项 | 实测 |
|---|---|
| 工作区大小（485 MB 的 bin 目录） | **无影响**（266~294 ms）。目录树惰性扫描 + 按层缓存，启动期只列第一层 |
| 会话标签数（1/4/8/16） | **无影响**（297~313 ms）。`restore_session` 只打开标签头 |

#### headless 五段分解（`tools/gbcode_startup_split.py`）

| 段 | 耗时 | 可否优化 |
|---|---|---|
| **A 进程启动 → 日志落盘** | **164 ms** | ❌ Windows 加载 8.5 MB 静态映像（`.text` 6.1 MB）+ 全局构造 |
| B → 会话恢复完成 | 4 ms | 已很快 |
| **C → DSL 首帧构建** | **55 ms** | 一次性（建窗口 + 构建组件树），与启动强绑 |
| D → 字体加载 | 15 ms | ❌ 读 6 face + 3 等宽 face（TTC/TTF 解析） |
| E → 控制通道就绪 | 3 ms | 已很快 |

#### 真窗口多出来的 373 ms

| 项 | 耗时 | 说明 |
|---|---|---|
| **D3D11 建设备**（`gpu::available()`） | **357.8 ms** | GPU 固有成本，已由 P0 条目分析定案 |
| 窗口创建其余 | ~15 ms | |

> **“选型结论跨进程缓存”曾实现过，后按“保持简单”整体删除**（见上方 P0 条目）。
> 若将来又要省这 358 ms，方向只有：① 先软件渲染上屏、后台异步建 D3D11 设备就绪后切换；
> ② `--renderer software` 显式指定（人的一次性判断）。二者都未做。

**结论：启动路径已无“低垂果实”**。剩余项要么是 Windows/GPU 固有成本，
要么需重构“先上屏再切后端”（架构级，风险高于收益）。

### P0：gbcode 启动慢 / 启动后卡住 —— **两处均已修**（附剩余项）

**用户报告**：“gbcode 启动为什么会卡顿 2 秒”、“首帧渲染快，但是页面还是会卡住 2 秒不能操作”。

先量账本（新增 `tools/startup_latency_probe.py` + 分段插桩，win32 真窗口 1884×1023 @1.25）。

#### ① 启动前段：渲染器 `auto` 选型（~410~518 ms）

| `start()` 内部阶段 | 耗时 |
|---|---|
| **`create_surface_for`（渲染器选型）** | **~410~518 ms** |
| — （其中 `gpu::available()` 建 D3D11 设备） | ~317~330 ms |
| — （其中 `gpu::create_canvas()` 编译着色器管线） | ~55~61 ms |
| **`show_when_ready`（首次上屏）** | 91~150 ms |
| `render_frame` ×2（首帧 + 上屏后补帧） | 40~60 ms |
| `fit_window` | 23~59 ms |
| `clipboard+script` / 控制通道 / 其余 | ~15 ms |
| **`app.start()` 合计** | **~910~1020 ms** |

窗口在 `show_when_ready` 之后才露面（后端故意建窗不显，避免白底），
所以“双击到看见界面”≈ `app.start()` ≈ 1 秒。

**修法**：`auto` 的语义**简化为“有 GPU 就用 GPU”**，否则软件。

为何不做“实测哪条更快”：那个判据要建 D3D11 设备 + 编着色器 + 缩尺基准，
全在窗口可见之前；而且两条路径性能接近时**结论会抖动**（实测同机两次启动
分别选到 gpu/software）——每次启动重新测就是在抽签。

现在只保留一条规则。实测：

```
渲染器选型：gpu      · 380.1 ms · D3D11 · NVIDIA L20-6Q · 11_1
渲染器选型：software ·   2.9 ms · 显式指定软件光栅器
```

那 380 ms 是 GPU 的**固有成本**（要建设备），去不掉。
极端环境若“有 GPU 反而更慢”，用 `--renderer software` 显式指定——
那是人的一次性判断，不该让每次启动都去测。

> 本项曾试过“选型结论跨进程缓存”（写 `renderer-choice.txt`），后来按
> “简单点”的决策**整体删除**。若将来又要省这 380 ms，方向见下方遗留项 2。

#### ② 启动后段：**界面出来了但卡住 3 秒点不动**（比上面严重得多）

只看 `app.start()` 会漏掉它——那 3 秒发生在**首帧之后的第一帧**：

```
[frame] #1 hosts=0ms term=0ms lang=3250ms tick=8ms
[frame] #2 hosts=3ms term=0ms lang=0ms    tick=4ms
```

细分到 `pump_language_service()` 内部：**`ensure_git_log()` 3301 ms**。

它一口气跑 **9 条 git 子进程**（log/branches/tags/remotes/stashes/refs/
upstream/operation/audit），全在主线程同步阻塞。大仓库（如
`D:/coding/modeltrain-be`）上就是 3 秒硬卡——用户的感受是
“界面出来了，但点不动、拖不动，两秒后才活”。

**修法**：读操作移到后台线程（`Tasks::run_async`），数据经 `post_ui` 回主线程落地。

- `git_service` 新增 `LogSnapshot`（纯数据）与 `read_log_snapshot()`；
- `ensure_git_log()` → `run_async` 跑读，完成后 `post_ui(adopt_git_log)`；
- **写成员与 `State` 必须留主线程**（工作线程写 `State` 是数据竞争，
  见 `st/core/async.hpp` 的线程模型）；
- 新增 `git_log_loading_` 防止重复提交任务：没有它，后台跑着的那几帧里
  `git_log_wanted_` 会被渲染期反复置真，于是**每帧提交一个新任务**（从卡一次变成持续卡）；
- 无调度器时（单测/嵌入式）退回 `load_git_log_sync()`，行为与旧实现逐行一致。

**实测效果**：

| | 改前 | 改后 |
|---|---|---|
| 第 1 帧 `pump_language_service` | **3250 ms** | **0 ms** |
| 首帧完成（探针口径） | 3916 ms | **881 ms** |

数据完整性已验：状态栏仍为 `Git：200 条提交 · 105 个分支 · 9 个标签`
（控制通道读回）。

#### 遗留（本轮未做，下次可接）

1. **`show_when_ready` 91~150 ms 的构成未拆**（首次 `ShowWindow` + 合成器映射，
   可能大部分是系统侧不可压缩的）。要查就先在那里插桩，别再猜。
2. **GPU 路径 380 ms 的设备创建无法避开**（选 `gpu` 就必须有设备）。
   若将来要做，方向是**在后台线程预建**（窗口先露骨架、设备好了再切），
   但那会把“首帧用哪个渲染器”变成时序问题——风险大于收益，**不建议轻动**。
3. **headless 默认不探测 GPU**（`renderer_note`：“无头缺省软件光栅器（不探测 GPU；
   显式 --renderer gpu/auto 才探测）”），所以本项**不影响测试**。
4. **git 异步后的可见延迟**：状态栏 Git 数字会**晚一两秒**才出现（后台在读）。
   这是正常的、也是该有的（界面不再等它）。若将来要做更好的反馈，
   可以在数字空着时先显示“读取中…”，而不是留白。

### P1：圆角矩形的**解析扫描线**——已试一轮，**净收益为负，已回退**

**试过什么**（2026-10-19，同一会话）：`Canvas::fill_rect` 的 `radius > 0` 分支直接算
每行的 x 跨度（解 `add_rounded_rect` 同一条三次贝塞尔），4 个垂直子行各发一条
`weight = 1/4` 运行段让 `blend_coverage_runs` 做垂直积分。

**为何失败**（两个独立原因，分别实测）：

1. **它必须排在几何 sprite 缓存之后才有意义，而“首次建图”那一遍本来就不是瓶颈。**
   实测把解析放在缓存**之前**时，它每帧都真跑一遍：`round_rect` **1.05 → 15.0 ms**、
   整帧 **4.45 → 20.0 ms**（比通用路径还慢一个数量级）——因为提前返回，缓存永远不命中。
2. **扫描线本身并不比贴图快。** 即使在缓存命中之后，慢的那部分已经变成“贴图混合”，
   而扫描线要逐像素算覆盖率（120 万像素 × 浮点），量级与贴图混合相当。

**再试时的入手点**：
- 先量“**缓存未命中占比**”。gallery 1600×1000 是 32 次调用 / 1.58 ms，若绝大多数命中，
  剩下的未命中只有个位数次/帧——绝对收益上限很小，不值得为它加一条并行实现。
- 真正的大头应是**中间整行段**（占面积 90%+）：可以完全不走 `blend_coverage_runs`，
  而是“中间带整行 `blend_span`（SIMD）+ 只有四个角各 `2r` 行逐像素”。这样 90% 的像素
  走 `0.025 ns/px` 那条路。
- 验收：`tools/paint_equiv_probe.cpp` 的形态（超差像素 < 0.2%、最大通道差 ≤ 25），
  **并且** `gpu_parity_test` 的 `gpu_software_diff_is_small_and_structural` 必须继续绿
  ——实测用 SDF 逐个像素算覆盖率（与 GPU 同式）反而会把该用例弄红（结构性 0.32%），
  因为通用路径是**内切折线**、比解析解系统性偏小约 4%。

### P1：`clear` 与首层不透明填充合并——**已完成**（2026-10-19）

已落地：`Canvas::clear` 记住「刚清成什么颜色、且未被任何绘制碰过」，`fill_rect`
遇到「同色不透明整屏底」直接跳过。A/B 对照：1440p **−8.8%**、1280×800 −5.0%、
1600×1000 −2.5%（见 `docs/PERF_2026-10-19.md` §3.3）。

**遗留可做**：`clear` 本体仍是纯内存写（1440p 写 14.7 MB / 1.0 ms ≈ 13.5 GB/s，
已接近单通道带宽）。若要再进一步，得让**首帧**直接用底色 `memset` 后不再清——
但那要求画布层知道“这一帧谁负责铺底”，属于接口变更，暂不做。

### P2：`subtree_may_paint` 的剪枝缓存——**已测：真实应用树太小，收益可忽略**

**现象**：`Element::paint` 每次对每个节点递归扫整棵子树判相交（O(n·d)），而不是
“自己有交集就直接画，无交集只探子”。

**为何看似值得**：它确实是纯矩形递归，按“每帧 × 每节点 × 子树大小”算，理论上
深树应用会有可观开销。

**实测（2026-10-19）**：两个真实应用的节点数——`gallery` **105**、`gbcode` **57**
（`metrics.nodes`）。合计每帧矩形相交次数在千级以下（~0.01 ms 量级），而整帧是 4~5 ms。
改成“向下传递”的写法预计省 **< 0.5%**，而剪枝写错的风险（漏画整块内容）远大于收益。

**所以不做**。若将来出现节点上千的界面（大型表格、画布编辑器）再回来复测：
判据是 `metrics.nodes` 过千、且能观测到纯矩形运算在 `frame_p50_ms` 里的占比。

### P2：行带并行（按水平带切分到线程池）——**已测：单线程已打满带宽，收益上限 ≈1.0×**

**2026-10-19 实测**（`tools/memory_bandwidth_probe.cpp`，1600×1000 = 6.1 MB 缓冲，
60 轮取最小，与渲染器同一访问模式）：

| 线程数 | 时间 | 带宽 | 相对单线程 |
|---|---|---|---|
| 1（整块写） | 0.558 ms | **11.47 GB/s** | 1.00× |
| 2（行带） | 0.772 ms | 8.29 GB/s | **0.72×**（变慢） |
| 4（行带） | 0.544 ms | 11.76 GB/s | 1.03× |
| 8（行带） | 0.899 ms | 7.12 GB/s | **0.62×**（变慢） |
| 16（行带） | 1.481 ms | 4.32 GB/s | **0.38×**（大幅变慢） |
| 1（`src-over` 读+写） | 0.956 ms | 6.69 GB/s | — |

**结论：纯写型工作（`clear` / 整行填充）单线程已经打满内存带宽**（11.47 GB/s，
对这台机器的单通道上限已接近饱和），再多线程只会加调度/同步开销。
4 线程刚好打平是可预期的（差不多填满了同一个带宽墙），8/16 线程明显变差。

**所以这条不该做**，除非先降低内存流量本身（那才是真方向）：
- `clear` + 首层底已合并（见 P1），剩下的 `clear` 无法再省；
- `over_row` 是读+写，带宽需求翻倍——要再快只能减流量。

> 减流量的两个候选（**16 位像素格式** / **分块保留模式**）及其开工前提，
> 已单独整理为本文首的 P1 条「**渲染的下一个真方向是“减内存流量”**」。
> 两处不重复描述细节，以那条为准。

**什么时候重启这个方向**：如果将来光栅化的耗时结构变成**计算型**占比为主
（如加入大量渐变/混合模式/滤镜），那时逐像素算术会跑赢带宽，并行才有意义。
判据：重跑 `memory_bandwidth_probe`，且 `over_row` 的 ns/px 明显高于 2× 带宽倒数。

### P1：大 Markdown 预览「首次打开」那一帧仍有 ~58ms（2026-10-19 遗留）

> 已修掉的部分见 `DESIGN.md` §4.5.2.1「第二轮」：548ms 的解冻重排、文本宽度 LRU
> 被后台刷爆（2.2~2.9×）、交付缺唤醒、`refresh_params` 判据错导致死循环。
> 现在**日常交互已达标**（预览滚动中位 7.6ms / 最差 12.9ms，编辑器打字 17.0ms，
> 均在 60fps 预算内）；只剩「切到大文档预览」那次的**一次性**首帧。

**现状量化**（gbcode + 447KB DESIGN.md，headless）：

| 场景 | 帧 | layout | paint |
|---|---|---|---|
| 切预览首帧 | 66ms | 58.8ms | 19.8ms |
| 预览稳态/滚动 | 7.6ms（中位） | 0.0ms | 7.4ms |

**已逐个排除（全是实测，不是推测——别重复这些弯路）**：

- `SlowContent::measure` 自身：`measure_ms` 属性实测 **0.0ms**；
- `MarkdownView::measure`：不慢（冻结生效，不再重排）；
- 终端 / Git 面板：隐藏时 `Element::measure` 已按 `visible_` 跳过，不在测量内；
- `WindowFrame::measure` / `TitleBar::measure` / `apply_theme`：各 0.0ms；
- 树规模：**298 个节点**，无重建（指针与节点数前后一致）；
- `TextRenderer` / `OffscreenTextPort` 构造：**0.00ms**；
- 文本宽度缓存：隔离端口后与主线程互不影响（`tools/port_isolation_probe.cpp` PASS）。

**决定性线索**：同一帧内对同一子树连测两次——**首次 45.3ms、再测 0.2ms（差 226×）**。
即这不是任何一处算法/重复计算，而是**首次遍历的冷内存（缺页/缓存未命中）或后台
排版线程抢 CPU** 的效应。

**下一步该量的（别先改代码）**：

> ✅ **2026-10-20 已量：缺页假设被否定**。工具：`tools/cold_memory_probe.cpp`
> 与 `tools/cold_memory_scale_probe.cpp`（用 `make_unique_for_overwrite` 绕过
> `vector` 的清零，否则冷内存效应被掩盖；裸 `new` 会被 lint L1 拦）。结果：
>
> | 缓冲 | 首次触碰 | 再碰一次 | 倍数 | 缺页 |
> |---|---|---|---|---|
> | **600 KB（= 447 KB Markdown 展开后的量级）** | **0.373 ms** | 0.012 ms | 31.9× | 1066 |
> | 2 MB | 0.544 ms | 0.091 ms | 5.9× | 3077 |
> | 8 MB | 2.241 ms | 0.687 ms | 3.3× | 12306 |
> | 32 MB | 9.163 ms | 3.280 ms | 2.8× | 49242 |
>
> **缺页机制确实存在**（64 MB 那次 131503 次缺页，理论值 131072，几乎完全吻合），
> 但**绝对量太小**：600 KB 只要 **0.373 ms**，与 58 ms 差 **155 倍**。
>
> 所以下面第 1 条（预碰）**不成立**，不要再投入。剩下只有第 2/3 条（分核 /
> 后台线程优先级），即“抢 CPU”假设。

1. ~~缺页数对比~~ → **已否定（见上表格）**。
2. 把主线程与排版线程**分核**再测——若分开后首帧掉到 10 ms 内，
   则根治方向是「后台线程降优先级/限核」。
3. 量后台线程的优先级是否真的生效。

> **本机限制**：`perf` / `taskset` 仅 Linux 可用，当前开发机是 Windows。
> 第 2/3 条需在 Linux 上验证；Windows 侧对应手段是 `SetThreadPriority` +
> `SetThreadAffinityMask`，但要先在能复现的应用里插桩。

**别做的事**：不要为这 58ms 再加缓存/懒加载层——数据表明首次与第二次差 226×，
任何“算得更快”的优化都动不了它。


> 📋 2026-10-17 **全面升到 C++23（含公共 API 扩展）**：清单 `cxx_standard` 成为
> 语言标准的**单一真源**（构建驱动、自举脚本、探针脚本同源派生），
> `Result<T>` 内芯换 `std::expected`（外形不变、新增 monadic 接口），
> `Surface::pixel_view()` 新增 `std::mdspan` 二维像素视图。
> 硬约束与“能用/不能用”的实测边界见 `docs/PITFALLS.md` 的「C++23」行。
> 本轮**刻意没做**的两项（各有实测依据，见下方 P1/P2）。
>
> ### P1：`std::print` —— 符号在 `libstdc++exp`，**加 `-lstdc++exp` 就能用**（未接）
>
> ⚠ **先前的结论错过一次，这里是修正后的版本**。当时看到链接失败
> （`undefined reference to std::__open_terminal(_iobuf*)` / `std::__write_to_terminal`）
> 就写成“MinGW 没实现这两个符号”——**错**。它们实现在 **`libstdc++exp.a`** 里
> （`ar t libstdc++exp.a` 可见 `print.o`），只是**不会自动链接**。
> `nm -A libstdc++exp.a | grep __open_terminal` 一行可证。
>
> 实测（g++ 16.2 / MinGW-w64，2026-10-17）：
>
> | 组合 | 结果 |
> |---|---|
> | 不加 `-lstdc++exp` | ❌ `undefined reference to std::__open_terminal` |
> | 加 `-lstdc++exp`（g++） | ✅ 正常 |
> | 加 `-lstdc++exp`（clang + `--target=x86_64-w64-windows-gnu`） | ✅ 正常 |
> | 中文 / emoji（非 BMP）/ stdout 为管道 | ✅ 全部正确（终端分支走 `WriteConsoleW`，非终端降级到 UTF-8 `fwrite`） |
>
> 所以：**`std::print` 能力上完全可用，甚至比 `st::print` 强一点**（终端上输出原生
> Unicode，不依赖控制台代码页）。目前不接的理由是**代价**，不是不可行：
> 1. `st.pkg` 的 `system_libs` 要**按平台**加 `stdc++exp`（Linux 上没有这个名字），
>    而这正是 `CONVENTIONS.md` §10 在防的“只在部分目标生效的分支”；
> 2. `exp` 库的定位是**未定 ABI 的预发布区**（见下一条 P2），将来可能改名或并入主库，
>    链接随之要改；
> 3. 收益小：`st::print` 已是 `std::format` + 直写 fd，功能等价；霜天的输出主要走
>    控制通道/日志/测试捕获，不靠 Windows 控制台画字。
>
> 要做就做成**能力探测**（像 sanitizer 那样探库是否存在），可用则切、不可用退回，
> 而不是硬编码平台分支。
>
> ### P2：`libstdc++exp` 是什么（别被名字骗了）
> 它不是“实验性质的玩具”，而是 **GCC 给“尚未进入稳定 ABI 承诺的设施”单开的库**：
> `ar t libstdc++exp.a` 只有 23 个目标文件——`print.o`、`stacktrace.o`（+ 15 个
> `std_stacktrace-*.o`，内嵌 libbacktrace 的 dwarf/pecoff 解析与 demangle）、
> `text_encoding.o`（C++26）、`contract26.o`（C++26 契约），以及 `<filesystem>` 的旧版残留。
> 体积 991 KB（主库 `libstdc++.a` 是 7.8 MB）。
>
> 关键事实：**同一设施在两个库之间不存在副本**——`std::print` / `std::stacktrace` 的
> 实现**只在** `exp` 里。碰见这两个设施的 `undefined reference` 时，
> **先查库再下结论**：`nm -A <lib>.a | grep <symbol>`，不要直接归因成“平台没实现”。
>
> ### P2：`std::stacktrace` 能链接但无法定位（符号化失效）
> 与 `std::print` **同处 `libstdc++exp.a`**（`stacktrace.o` + `std_stacktrace-*.o`，
> 其中含 `std_stacktrace-pecoff.o`——PE/COFF 的符号化路径）。
> `-lstdc++exp` 能链接成功、`current()` 也返回了 5 帧，
> 但 **5 帧全部解析成同一个无意义名字**（`register_frame_ctor`）——即符号化实际没工作。
> 注意它与 `std::print` 的区别：**同一个库里，一个能用、一个不能用**，
> 所以“加 `-lstdc++exp` 就完事”是错的推广——要逐设施验。
> clang 目标下则是链接直接失败（`_S_current` 未定义）。
> 结论：崩溃报告仍走自研路径（DbgHelp / addr2line），
> 判据与证据在 `tools/stacktrace_probe.cpp`（跑一次即可复核，退出码区分
> “无帧 / 全同名 / 不可用”）。重试前先跑它，别再从头摸。
>
> ### P2：`std::flat_map` 已量，**不换**（无查询收益）
> 按本仓真实访问形态实测（构建一次 + 大量 string_view 查询）：
> N=200 / 2000 条时 flat_map 相对 map 的查询是 **+2.7% / −0.2%**（无收益，
> 二分与红黑树都是 O(log N) 次字符串比较），相对 unordered_map 慢 **3~5 倍**。
> 小表（N ≤ 128）时 flat_map 的真收益在**构建**（比 map 快 30~60%，比 unordered_map
> 还快），查询仍与 map 持平、比 unordered_map 慢。
> 结论：不为了用新设施而换容器；将来遇到“高频建小表 + 数据量极小”的真热点再考虑，
> 基准可重跑（`tmp/flat_map_bench.cpp`、`tmp/flat_map_small_bench.cpp` 的形态）。
> 顺带：`lint` 每次 `manifest_exempt` 都构造临时 `std::string`（实测 32000 次查询
> 0.785 → 0.665 ms，改异质查找可省 ~15%）——量太小（整趟 lint 14 s），不值得单独做。
>
> ### P1：`--extra-flags` 未接线（命令行参数被静默丢弃）
> `BuildOptions::extra_flags`（`include/st/pkg/build.hpp`）存在且会被拼进编译标志，
> 但 **`tools/stpm/main.cpp` 没有任何代码把 `--extra-flags` 传给它**——
> 传了等于没传，构建照常成功、无任何提示（实测：`--extra-flags=-Os` 改成 `--force`
> 重建，编译命令行里仍是 `-O2`）。
> 危害不是“少个开关”，而是**实验档位时会把结论测反**：以为在测 `-Os`，实际在测 `-O2`，
> 而两者产物体积相差 2 MB。
> 方向：解析进 `Arguments.options` 并以空白切分追加到 `BuildOptions::extra_flags`；
> 同时**入缓存键**（否则“换 flags 但命中旧对象”会再次制造假读数）。
>
> ### P2：档位标志在清单 `flags` 之后，清单里写 `-O` 无效
> `build_language_flags` 的顺序是「清单 `flags` → 档位 → 工具链收敛」，而 GCC 取**最后**
> 出现的 `-O`。所以往 `st.pkg` 的 `flags` 里加 `-Os` 会被档位的 `-O2` 覆盖——
> 实测写进去后产物体积一字不差（5,575,682 字节完全相同）。
> 当前要改档位只能改 `profile_flags()`。这不算错误（顺序有明确理由：分档要做可追溯的例外），
> 但**清单里写 `-O` 是一个沉默的坑**，要么在解析时告警，要么在文档里写清。
>
> ### P2：体积参考值（2026-10-17 实测，供将来定档位用）
> `st.exe` **本来就是动态链接**（依赖 `libstdc++-6.dll` + `libgcc_s_seh-1.dll`；
> `st.pkg` 的 `-static-libgcc -static-libstdc++` 只在 mingw 交叉工具链下生效）。
> 均已剥调试信息：`dev -O1` **4,502 KB** / `release -O2` **5,445 KB** /
> `release -Os` **3,397 KB** / `release -Os` + `strip --strip-all` **3,036 KB**。
> 两个反直觉点：**`-O2` 反而比 `-O1` 大 21%**（内联展开使 `.text` 从 3.3 MB 涨到 4.3 MB）；
> `.text` 2.28 MB 是体积下限（真实代码），可剥的非必要段合计只 1 KB。
> 改 `release` 用 `-Os` 前须先过性能门禁（`frame_cost_stays_within_budget` 类用例）。

> 📋 2026-10-17 **X11 双缓冲上屏（P2 落地）**：两块共享段交替 + `XShmCompletionEvent`
> 判段可用，不再每帧 `XSync`。实现与理由见 `DESIGN.md` §4.6.2；状态机
> `include/st/shell/present_slots.hpp`；真窗口验证 `tools/x11_double_buffer_check.py`。
>
> ### 干净对照测量（权威；`tools/x11_present_bench.py`，2026-10-17 10:02）
>
> 同机同参：backend x11、`--scale 1`、`--bench 200`、同 DISPLAY；A/B/A/B 交替各两轮取中位；
> **测量前先各跑 60 帧热身并丢弃**（不热身时首次跑 总帧 p95 会到 55 ms——那是字体/页缓存
> 首次触碰的冷代价，不是稳态）。基线 = 真基线树 `/home/x/code/gebai-baseline-x11`
> （HEAD `5826d1b1`，无 `present_slots.hpp`、`XShmCompletionEvent` 计数 1）。
>
> **负载条件**：测量前 `/proc/loadavg` = `0.66 4.08 6.66`，测量后 `0.93 3.77 6.46`；
> 八个数据点逐点采样 1 min load ∈ [0.66, 0.93]；脚本前后两次进程扫描均为**零**
> 编译器/构建/测试进程（自检结论：`✓ 测量期间没有并行构建/测试在跑`）。
>
> | 场景 | 总帧 p50 / p95 | 绘制 p50 / p95 | 送显 p50 / p95（单段 + 每帧 XSync） | 送显 p50 / p95（双段 + 完成事件） | 送显降幅 |
> |---|---|---|---|---|---|
> | x11 1280×720 | 15.00 → **11.61** / 11.88 ms | 11.05 / 11.26 | 3.83 / 6.16 ms | **0.38 / 0.42 ms** | **10.1×** |
> | x11 1920×1080 | 25.00 → **18.50** / 18.89 ms | 17.44 / 17.79 | 7.14 / 9.28 ms | **0.86 / 0.90 ms** | **8.3×** |
>
> （总帧列是“基线 → 本树”的 p50；p95 列是本树的值。）
>
> ### ⚠ 早一版数字作废（负载不可比）
>
> 实现刚落地时在 load average **7~12**（另一条线在主树并行构建）的窗口里来过一组：
> 1280×720 送显 3.64 → 0.43 ms、1920×1080 7.14 → 0.95 ms。**这组作废**，原因是负载，
> 不是结论变化（方向与量级与干净组一致，但绝对值不可引用）。干净组的可核对值是上表。
>
> ### 独立探针（`tools/x11_present_probe.cpp --double`）
>
> 1080p 单段每帧 `XSync` 提交 4.8 ms → 双段 **0.96 ms**（`--pace 12` 模拟真帧间隔）。
> **满速**压测（帧间隔≈0，服务端跟不上）下两块段都会在途，双缓冲退化为“每两帧排空一次”，
> 提交 4.1 ms——这是本改造的上限风险：它不会错，只是收益缩到 ~1.2×。
> gallery 的 `--bench` 是“每帧全量重绘”，帧间隔 ≈ 绘制耗时（11~18 ms），所以它看到的是
> **真帧间隔工况**（上表）。
>
> `metrics.present_note` 实物（本树 900×640，`--scale 1`）：
> `MIT-SHM · 2/2 块（MIT-SHM 已启用（0.0）；上帧送显 900x640；完成事件 9 个）`
> ——路径/块数/后端版本/上帧送显面积/完成事件数五项都能直接查到。
>
> **本轮未做 / 后续可选**：
> - **P2：双缓冲的“可用块数”可以抬到 3+**。两块在“送显比绘制快”的满速压测下会两块
>   同时在途，双缓冲退化为每两帧排空一次。三块能拉长允许在途的帧数，代价是每块一整屏
>   内存（1080p 是 8 MB/块）。先要一个“真实应用里到底会不会两块同时压住”的量。
> - **P2：Windows 侧同口径对照**。Win32 现在走 `BitBlt`（DIB），本机无法量它占多少；
>   若它与 X11 同量级（~25%），值得同样做“局部上屏”（`Present` 的 dirty rect /
>   `UpdateLayeredWindow`）。需 Windows 机器实测才能定优先级。
> - **P1：绘制（paint）仍是整帧大头（1080p 17.6 ms）**。本轮未碰，
>   已有诊断见 `PAINT_DIAGNOSIS.md`（阴影“将被覆盖区域”提示接口那条已在 P1 列着）。

> 📋 2026-10-16 **Linux 上屏路径轮（X11 MIT-SHM + 局部上屏）**：已落地。
>
> 实测（Linux + GNOME/X11，gallery 全组件页，`--bench` 连续强制全量重绘，150 帧 p50）：
>
> | 场景 | 总帧 | 绘制 | 送显 |
> |---|---|---|---|
> | x11 1280×720 | 14.27 → **12.40** ms | 11.20 / 11.23 | 2.88 → **0.96** |
> | x11 1920×1080 | 24.82 → **20.28** ms | 17.87 / 17.94 | 6.71 → **2.14** |
>
> 局部上屏（只送损坏区）另使得悬浮/局部改动的 present 降到 **0.19~0.22 ms**
> （送显 196×101，对照整屏 1.07 ms）——这是交互场景的真实收益。
> 实现与理由见 `DESIGN.md` §4.6.2；独立量尺 `tools/x11_present_probe.cpp`。
>
> **待跟进的项（本轮未做）**：
>
> - **P2：X11 双缓冲上屏（省掉每帧 XSync）** —— **已于 2026-10-17 落地**（见上方该轮说明）。
>   当初的顾虑（事件基址推进、段回收、失败回退、完成事件可用性）逐项处置：基址运行期取、
>   段随槽建/拆、不回报完成事件时如实降级、Xwayland 实测可用。
> - **P2：Windows 侧同口径对照**。Win32 现在走 `BitBlt`（DIB），本机无法量它占多少；
> 若它与 X11 同量级（~25%），值得同样做“局部上屏”（`Present` 的 dirty rect /
> `UpdateLayeredWindow`）。需 Windows 机器实测才能定优先级。
> - **P1：绘制（paint）仍是整帧大头（1080p 17.9 ms）**。本轮未碰，
> 已有诊断见 `PAINT_DIAGNOSIS.md`（阴影“将被覆盖区域”提示接口那条已在 P1 列着）。
> - **P3：`wayland_win_check` 的缩放截图尺寸**：本机 GNOME 为 2×，脚本报帧缓冲 5120×2816，
>   而 `DESIGN.md` §4.6.1 里还写着旧环境的帧缓冲尺寸数字（旧环境值）。已在本轮发现但不改数字——
>   那属于“环境相关值不该写进文档”，应从文档里换成“与 `device_scale` 一致”这种可核对的判据。

> 📋 2026-10-14 **基础指标整改轮（性能 / 产物大小 / 内存）**：按当日的框架层评估报告
> 落地 7 项。**实测（Linux 容器，8 核，`NotoSansCJK-Regular.ttc` 19.5MB）**：
>
> | 项 | 改动前 | 改动后 |
> |---|---|---|
> | 无头软件路径内存（gallery） | **RSS 137.6 MiB** | **RSS 31.7 MiB（-77%）** |
> | `st`（dev 档）产物 | **70.1 MB** | **4.59 MB**（+ 分离符号 66 MB）|
> | `gallery`（dev 档）产物 | 29.7 MB | **6.87 MB**（+ 29.0 MB）|
> | 字体内存形态 | 15 个 face **整读驻留** | **15/15 走内存映射**，0 回退 |
> | 无头渲染器缺省 | `auto`（探测 GPU） | **software**（不探测）|
> | 缓存预算 | 三处魔数 | `st::memory` 单点 + `ST_MEM_BUDGET=low` |
>
> 落地要点（逐项取证见下文 P1/已完成条目）：
> ① **字体按需驻留**：新增 `st::fs::MappedFile`（`platform_fs_map.cpp` 单点封装，
>    POSIX `mmap` / Windows `CreateFileMapping`）；`FontFace::load` **映射优先、整读回退**
>    （`FontData::span()` 视图不变 → 解析层零改动）；`font_face_count` 改只读 512 字节头
>    （原先为问 face 数把 19.5MB 整个读一遍）。控制通道新增
>    `font_faces_loaded / font_mapped_loads / font_read_fallbacks`，
>    **回退可观测**（否则“这台机器为什么多占 80MB”会变成谜案）。
> ② **字体缓存去 thread_local**：`load_face_cached` 改进程级 + 锁外加载 + 双检；
>    计数器改进程级 `atomic`（线程局部时**测不出“别的线程又整读了一遍”**）。
> ③ **无头缺省 software**：`HeadlessBackend` 把空串/`auto` 解析为 `software`
>    （窗口后端不动——那里“谁画得更快”确实得测）；理由如实写进 `renderer_note`。
> ④ **产物分离调试信息**：`separate_debug_info`（`--only-keep-debug` → `--strip-debug` →
>    `--add-gnu-debuglink`），构建器与两个自举脚本都接上，`--no-strip` / `ST_NO_STRIP=1` 可关；
>    崩溃器（Win 侧 `addr2line` 提示、POSIX 侧新增一行）都改为**指向 `.debug`**。
> ⑤ **帧循环去刺**：`wall_times` / `frame_times` / `log_lines` 改环形缓冲（旧实现每帧
>    `erase(begin())` = O(n) 搬动）；`UiRoot::tick_declarative_hosts` 去每帧整表拷贝。
>
> **验证**：`st test` 全绿（338/340/339/340 四片，含慢用例）；`st lint` 0 违规；
> mingw 交叉语法档；**两条逆向验证**（去映射 → 字体用例红；
> 缓存回 `thread_local` → 跨线程用例红）。
>
> ⚠ **`--slow` 下的 4 条性能门禁红均非本轮引入**（已用基线坐实）：在**干净工作树**
> （`git worktree add 876039c`，本轮改动一行未碰）+ 自举重建的二进制上，
> `bench_rounded_card_fill` 同样 FAIL（**基线 28.31 ms / 24.88×** vs 本轮 28.26 ms / 14.22×
> ——绝对值同量级）。根因是**本容器环境**：`baseline_ms_per_pixel()` 的清屏是纯
> `memset` 且有 SIMD 快路径，把比值分母做小、所有比值偏大（本机 loadavg 常 >2）。
> 这与 BACKLOG §3141 已登记的“bench 余量卡在上限、重编即误报”是同一根（那边还指出
> `kMaxHeadroom` 实际余量在 3.46~3.51 之间踩线）。**不需在本轮修**，但两点值得记：
> ① 本容器不能作为性能门禁的判据环境（应以 Windows 宿主/发布档为准）；
> ② 这类“环境型红灯”至少要让后人能区分——下文已记下基线数字，下次可一条命令对账。
>
> 另：`independent_project_builds_links_and_runs` 在本容器**启动任何子进程后立即挂住**
> （与本次改动无关的容器限制，需真编译真跑）。
> ✅ Windows 宿主实测（2026-10-15）：**PASS**（9.27s / 10 断言，产物输出 integration-ok）
> ——容器限制不复存在，该项在 Windows 可作为常规验证路径。
>
> **本轮由自己的用例抓出的两处判据错**（记一笔，因为都是“看起来显然”的写法）：
> 分离调试信息的幂等判据先用 mtime 比（`--strip-debug` 会刷新产物 mtime ⇒ 恒假，每次白跑），
> 后改成“`.debug` 存在即跳过”（重链后**新产物的调试信息会永久丢失**）——最终用
> `objdump -h` 探“产物里还有没有 `.debug_info`”，探不到时**按需要重跑处理**（安全方向）。
>
> **待复测（需 Windows 机器）——✅ 已复测（2026-10-15 第 5 轮，Windows 本机）**：
> `mem_sample.py` 补 Windows 分支（ctypes：`GetProcessMemoryInfo` + Toolhelp32 线程计数，
> 不起 PowerShell 子进程、不引 psutil），修复前在 Windows 上「采样失败」（逆向验证已回放）。
> 实测（同报告口径，无头 software）：
> | 路径 | RSS 峰值 | 私有内存 | 线程 |
> |---|---|---|---|
> | gallery dev+software | **30.5 MiB** | 20.9 MiB | 18 |
> | gallery release+software | **35.2 MiB** | 20.5 MiB | 18 |
> | gallery release+auto（GPU） | 48.9 MiB | 87.6 MiB | 73 |
> 对照报告原始数字（176MB / 53MB GPU / 55 线程）：**整改收益在 Windows 上坐实**
> （RSS 降为 1/5；GPU 路径线程 73 vs 报告 55——驱动栈差异，同量级）。
> 复测口径：`python3 tools/mem_sample.py --exe <exe> -- --headless --control-port 0 --frames 100000 --renderer software`。
>
> 📋 2026-10-11 **自举修复轮**（Windows 首次自举链接失败）：现象是一串
> `undefined reference to st::codec::png_read_file / st::raster::Canvas::*`，**报错指向源码**，
> 而真因是 `bootstrap.ps1` 的单元集按目录写死（`src/core src/ext src/pkg tools/stpm`），
> 与 `st.pkg` 脱节——C6 新增的 `src/ext/video.cpp` 引 `codec/raster`，而这两块不在目录里。
>
> **修法（已落地）**：把"工具链需要哪些单元"变成清单的权威声明
> （`targets.st.sources` / `targets.st.exclude_sources`），两个自举脚本都改为
> `sources − exclude + target.sources` 推导。实测：64 单元 / 41.9s（Win）/ 48.6s（WSL），
> `st build st|gallery`、`st check`、`st lint`、测试全绿、交叉编译双目标通过。
>
> 📋 **同轮拓出的两个真缺陷**（都已修）：
> ① **工具链被迫吞下整条渲染链**：`st` 一行渲染不跑，却因两条**头文件级**反向依赖
>    （`core/platform_crash.cpp → ui/state_trace.hpp`、`shell → ui/window_control.hpp`）
>    拖进了 codec/raster/text/md/xml/shell。下面 P1 是治它的路。
> ② **`%lld` 配 `int64_t*` 在 LP64 上是构建失败**（`src/ext/video.cpp`）：
>    Linux 的 `int64_t` 是 `long`、Windows 是 `long long`——Windows 只警告，
>    WSL 上同一份代码 `-Wformat` 刷屏（配 `-Werror` 即失败）。**本机看不出跨平台问题**
>    这条再次应验：能在 WSL 实跑一次自举，才算真的验过。

> ✅ 2026-10-11 **第二次清账（C 轮）**：上一轮留下的 4 条「数据层已完成、视图层待做」
> 已全部补完（C1 冲突三份取数 / C2 合并视图 / C3 逐行暂存界面 / C4 TimelineView），
> 另完成 C5（画笔输入：量化后定案为"帧内事件合并"）与 C6（视频解码：评估后定案为
> 子进程 CLI）。**本章现在没有未完成的旧条目**，只剩下面 3 条新回流。
>
> 两次清账共同的一条经验：**"数据层已完成"的切分是对的，但它有个副作用**——
> 视图层没接上时，用户侧完全看不出进展（数据算得再对也看不见）。所以本轮把
> 「视图层」当成独立可交付物来做（各自有像素级断言），而不是当成数据层的尾巴。
>
> 📋 **C 轮另修出 3 个非清单内的真缺陷**（都值得单独记，因为它们此前被别的东西掩盖）：
> ① **用例里的未捕获异常会带走整个测试进程**（nlohmann `get<int>()` 对 null 抛，
>    逃到线程入口 = terminate）：整套 1268 条用例在第 800 条处整进程消失，
>    前面七百多条 PASS 全部作废——而 `--test-jobs` 分片**碰巧**能避开，于是
>    "分片绿"被当成了"测试全绿"。已在框架层加异常边界（记 FAIL 并继续）。
> ② **`ScriptBridge::expose` 挂到了裸全局**（而契约是 `app.…`）：
>    `expose("counter.bump")` → `globalThis.counter.bump`，
>    脚本里写 `app.counter.bump` 报 `TypeError: not a function`。
>    这个缺陷**被 ① 掩盖**：它导致后续断言拿到 null 再抛异常，看起来只是"跑到某处崩了"。
> ③ **播放头抢走片段左缘命中**（`Timeline`）：播放头默认停在 0ms 而命中带 3px，
>    于是"抓不住第一个片段的左边"（修剪左边缘完全没反应）。
>
> 🔢 **修完 ①② 之后：单进程全跑首次全绿（1257 passed / 0 failed / 24810 断言）**。
>    这条从此成为一条**可用的验证路径**——以前只能靠分片，而分片掩盖整类问题。


> 📋 2026-10-11 **Git 能力第二轮已落地**（见 `DESIGN.md` §4.2.12 / §4.2.13）：
> 写操作层（25 类命令构造 + 危险度三档 + 备份分支 + 多步控制 + 审计）、
> 读面补齐（标签/远程/暂存/引用/单文件历史/blame/多步状态/上游）、
> 界面（变更工具条与整组动作 + 提交面板 + 五视图工具窗 + 多步横幅）。
> **实测拓出 9 个真缺陷**，其中 5 个是底层机制问题（已全修）：
> `git blame` 无 `--no-color`（整条命令失败、输出全空）、审计凭空造 `.git/`、
> 写命令在“从未开过终端”时静默失败、切标签页**按位置回收**子元素致属性残留、
> `Repo` 清理抛异常中止整个测试二进制；另有 4 个观感类（输入框被拉高、
> 计数被压成“已…”、确认框按钮无稳定 id、标签名里的点在 CSS 选择器里被当类分隔符）。
> 下面三条是接下去的路。

### P1：测试产物缓存不重扫新增源文件 + 陈旧链接防护（2026-10-16 第 40 轮破案衍生）

**现象 A（缓存不重扫）**：`build/release` 目录是 10-15 12:45 旧产物，今日新增
`tests/gallery_component_parity_test.cpp` 后跑 `st build --profile release`，报
「重编 0 / 命中缓存 139」并直接重链旧 obj——**新 TU 从未编译**，产出的
`st_tests.exe` 不含新用例（过滤运行 0 匹配），且「全绿」极具迷惑性（1447 passed
是旧测试集的绿）。根因疑在构建 manifest 缓存对 `tests/*.cpp` 通配的展开结果不失效。

**现象 B（陈旧链接静默崩溃）**：改共享头（`gallery_exhibits.hpp` 的 `inline constexpr
std::array` 尺寸 30→34）后只跑 `st build gallery`（指定目标不链测试），旧
`st_tests.exe` 里按旧尺寸编译的符号照常链接——运行期 `std::bad_alloc` /
`0xC0000005`，编译期零告警；六跳排查才定位（详见 `.evolve/troubleshooting.md`）。

**修法方向**：①构建系统级——头文件依赖变更时传递失效到所有引用目标（含
`st_tests`）；或至少 `st build` 后提示「测试二进制未重链」。②缓存命中日志区分
「未变更」与「未扫描」。验收：改共享头尺寸后仅跑 `st build <target>`，`st_tests` 能自动重编或明确报出。

### P1：git_service 家族分片并行随机误伤（2026-10-16 第 40 轮观察）

**现象**：`st test` 全量（16 分片并行）时 git_service 家族随机失败——不同轮次
2→5→17 例不等，失败名单不固定；17 例逐个单跑全绿（`st test <name>` 三轮验证）。
历史上 29 轮已记录过同族并行误伤，今晨规模扩大。

**推测根因**：该家族用临时目录+git 子进程（磁盘/句柄竞争），16 进程并行时
子进程创建或临时目录操作随机超时。

**修法方向**：①该族用例打「环境敏感」标记进慢/隔离集（`--slow` 跳过）；
②或分片调度同族用例集中到单片；③或用例内部重试一次。验收：连续 10 轮
`st test` 全量不再出现该族随机红。

### P1：打破 `core → ui` 的传递依赖（工具链别再吞渲染链）

**现象**：`st`（构建/测试/lint/依赖管理，**一行渲染都不跑**）的单元集里有一半是渲染层
（`codec`/`raster`/`text`/`md`/`xml`/`shell`）。当前靠 `targets.st.exclude_sources`
把 `ui/*` 排掉、再单独把 `ui/window_control.cpp` 加回来——**能编过，但依赖方向是错的**。

**两条根因（都是头文件级的，所以 lint 看不见）**：

1. `src/core/platform_crash.cpp`（core）`include "st/ui/state_trace.hpp"`：
   `state_trace.hpp` 自己干净，但同模块的 `ui_root.hpp` → `raster/canvas.hpp` 把渲染链拖进来；
2. `src/shell/{backend,platform_win32}.cpp` `include "st/ui/window_control.hpp"`：
   「窗口边缘命中契约」定义在 `ui`（而 shell 必须用它），又把 `ui` 拉回工具链。

**根因①已修（2026-10-15，第 3 轮）**：取证发现它是**死 include**（零符号使用，
崩溃钩子早已走 `set_crash_extra_provider` 函数指针）——删掉即根治，无需设计变更。
同时新增 **lint L16 层方向规则**（core 文件 include `st/ui/` 头 → 违规）作为防护栏，
防复发（详见 CONVENTIONS §8 规则表）。

**根因②分析（只分析，未落地）**：`window_control` 契约（`WindowEdge` 枚举 +
`kWindowResizeBorder` + `resize_edge_at` 纯函数 + `WindowControl` 抽象端口）
上移 `core` 的影响面：

- **方案**：新建 include/st/core/window_control.hpp（与现 `st/ui/window_control.hpp`
  同内容，头文件注释同步改写「为什么放 core」）；`src/ui/window_control.cpp` →
  `src/core/`（新建 `window_control.cpp`）；旧 `st/ui/window_control.hpp` 改为转发头
  （`#include "st/core/window_control.hpp"`，过渡期兼容）或直接改全部 7 处引用点
  （`shell.hpp`、`platform_{win32,x11,wayland}.cpp`、`title_bar.hpp`、`window_frame.hpp`、
  `app.hpp`、`gallery/pages.hpp`）。
- **收益**：`targets.st` 清单可删 `window_control.cpp` 补丁项，`st` 不再把窗框
  契约编译进工具链；shell 不再 include ui。
- **风险/代价**：① 层语义变化——「窗口」概念进入 core，需在 DESIGN.md 分层表
  登记（core 新增职责：「与窗口系统无关的窗口几何契约」）；② `WindowControl`
  端口的方法签名引用 `shell` 的能力语义，但端口本身纯虚、无系统头，无污染；
  ③ 属**设计层面变更**，按协议要先定 DESIGN.md 分层表再动。

**方向**：见上分析。做完则 `targets.st` 不再需要 `window_control` 补丁项。
**属设计层面变更**（层间契约），要先定 `DESIGN.md` 的分层表再动。

### P2：构建期嵌入（`battery/embed.hpp`）与层闭包解耦

`src/ui/script_host.cpp` / `declarative_host.cpp` 直接 `include "battery/embed.hpp"`
（**构建期生成**），所以任何"还没有嵌入头"的阶段（自举、`check_selfcontained` 的 TU 组）
都不能编它们。当前靠"它们属于被排除的 ui 层"绕开——**耦合是隐性的**：
将来某个**工具自己**需要的单元一旦用上 `b::embed`，自举会以
`battery/embed.hpp: No such file or directory` 失败，而那看上去像路径问题。
**方向**：嵌入资源改走「运行期读文件 + 可执行文件旁查找」或「显式的生成步骤前置」，
让"能不能编"不再取决于"构建轮到哪一步"。

**分析（2026-10-15 第 4 轮，只分析未落地）**：

**现状机理**（取证）：`b::embed` 生成机制有三层作用域——工程库级（`st.pkg` 顶层
`embed`，如 `src/ui/script_api.js`）、框架级（引用框架的独立工程用）、目标级
（`targets.X.embed`，如 gallery 的 assets）。包含目录**按单元精确下发**
（`attach_embeds`，build.cpp:317-343）——同一命令行出现两个 `battery/embed.hpp`
时先命中的赢，另一方的资源 `static_assert` 失败（ODR 陷阱，DESIGN.md 缺陷表 #29）。
消费端**只有 2 个框架源**：`src/ui/script_host.cpp:14`、`src/ui/declarative_host.cpp:10`
（嵌 `script_api.js`/`declarative.js`——为可编辑器高亮 + 免转义，见 script_host.cpp:24-27
的注释）。

**候选方案对比**（代价递增，A/B/C/D）：

- **A. 运行期读文件**：脚本前置改为启动时从可执行文件旁/包内读 `.js`。代价：
  ① 丢「路径写错编译期报错」；② 分发形态变化（需随包带文件，单文件自包含性丢失）。
  **不推荐**——嵌入的编译期检查与自包含性正是它的价值。
- **B. 生成步骤前置**：把 `b::embed` 生成从「目标构建时」提到「自举之前」。
  代价：自举脚本要复刻 stpm 的生成器逻辑（或先编一个最小生成器）——维护两份
  生成逻辑，漂移风险高。**不推荐**。
- **C. 消费端下沉到非生成依赖**：保持 `b::embed` 不变，但把 `script_host.cpp` 的
  `b::embed` 调用改成**注入**——库层提供 `set_prelude_source(string_view)`
  （或构造参数），由 app 层（在嵌入头可得的环境）注入。代价：改动小（两个文件
  + 启动接线），但**只是把问题从 ui 移到 app**——app 层单元若被工具链需要，
  同样的坑还在。
- **D. 延迟 include**（最小侵入）：`script_host.cpp` 用 `#if ST_HAS_EMBED`
  门控——自举/工具链编译时退化为运行期读文件（保留嵌入为默认）。代价：
  两套加载路径并存，行为差异需测试覆盖。

**推荐**：维持现状 + 把隐性问题显性化——机械护栏（启发式与 L16 同型）：
`check_layer_includes` 再加一条「框架源 include `battery/embed.hpp` 时，
若该单元属于工具链闭包（st.pkg targets.st 未排除）则报」是过度耦合的机械检查，
但「谁属于工具链闭包」只有清单知道，lint 拿不到清单——退而求其次：
**在 `bootstrap.sh` 排除段加一行断言**：排除后的单元集若仍含
`script_host.cpp`/`declarative_host.cpp`（即消费 `battery/embed.hpp` 的两个文件），
直接报「层闭包问题，不是路径问题」并 exit 1。这把 BACKLOG 原文担心的
「将来某个工具单元用上 b::embed」变成**即时可诊断**的失败。
设计层变更（生成时序/作用域规则 A-D）仍留人工决策。

**已完成**：解码侧（`st::ext::VideoDecoder`，经 `ffmpeg` 子进程：抽帧 + 元信息 +
探测不到如实不支持，见 `docs/../include/st/ext/video.hpp`）。

**待做**：导出走 PNG 帧序列 + manifest（无损、可复现、不引入容器封装复杂度），
要改成容器（mp4/mkv）的话同一条子进程路径可用（`-i` 帧序列 → 编码）。
**这是导出管线的设计决定，不是能力缺失**——先问清"要不要可复现的无损中间产物"。

### P2：`ffmpeg` 常驻进程（若真要做逐帧播放）

当前每次抽帧起一个进程（~30ms）。素材查看（导入读元信息、缩略图、对齐）够用，
**实时播放不够**。两条路：常驻进程 + 管道（要处理进程死了/管道堵了/卡帧），
或链接 libavcodec（要解决依赖与交叉编译，见 `video.hpp` 的对比表）。

### P2：示例：caiyun 真实视频素材导入

现状：`st::ext::VideoDecoder` 已落地（v0.1.7）但**全库零使用**，caiyun 的素材
仍全部是程序化生成片段（诚实边界注释已同步为这个事实）。导入真实视频要的
不是一行解码调用：导入时读元信息（时长/分辨率/音轨）→ 缩略图与时间轴片段
卡的帧预览（抽帧 ~30ms/帧，必须缓存）→ 播放预览的降级策略（合成素材的
确定性 e2e 判据不能丢）。涉及 Project 模型扩一类「真实视频片段」（不可用
Generator 渲染）。为什么值得做：它是解码器能力的唯一应用示范位，也是
「能力存在但外部用不到」这条反推链的闭环；但要先定缩略图缓存策略再动工。

### P2：POSIX 侧 `process::run` 的 env 覆盖语义与 Windows 不一致（已修，2026-10-15 P1 落地单）

**现象**（分片口径统一轮实测发现）：`src/core/platform_process.cpp` POSIX 分支把
`options.env` **追加**在 `environ` 之后（同名键出现两次，`getenv` 取前者⇒覆盖静默失效）；
Windows 分支 `build_environment` 正确去重覆盖。影响面：覆盖**已存在**键时 POSIX 静默失效
（新增键两平台都对）；`run_shards` 下发 `ST_JUNIT_XML` 是新键故未爆。
**修复**：env 拼接逻辑提炼为平台无关纯函数（`src/core/process_env.hpp` 与
`src/core/process_env.cpp`）
（`merge_environment`：先剔除 base 中被 overrides 命中的键再拼接；键匹配大小写
敏感性参数化——Windows 不敏感/POSIX 敏感，两平台语义差异显式钉住而非磁巧一致）；
POSIX 分支 `run()` 改调它（`case_insensitive=false`），Windows `build_environment`
语义不动、注释指向同一事实源。回归 `tests/core_process_env_test.cpp` 六用例
（覆盖生效/新增键/两平台大小写语义/畸形条目与空 overrides 透传/键提取）——
**两平台本机都能跑**（提炼即为了这个：Windows 全量 st test 验语义，Linux/WSL
轮验同用例同语义）；逆向验证口径：注释掉 `merge_environment` 的剔除行即回追加式，
「覆盖生效/不敏感剔除」两组必红。
**附带项（已修，2026-10-15 同日）**：空分片「JUnit 报告写入失败」噪声——`test_main.cpp`
把 `write_junit` 返回 0 的两种含义（0 用例 vs 写失败）混为一谈；修复后只报后者，
回归用例 `empty_shard_writes_no_junit_and_reports_no_failure`（逆向验证变红确认），
`--test-jobs 4` 空片输出噪声实测消失。

### P2：帧门禁 `frame_cost_stays_within_budget` 在 Windows 上双档恒红（比值口径跨平台不稳）

**现象**（2026-10-15 Windows 宿主实测，正是整改轮注明「应以 Windows 宿主/发布档为准」的环境）：
`st test --slow` 里 `frame_cost_stays_within_budget` 恒红——
- dev 档（-O1）：best 53.86× 清屏 vs 阈值 50×（余量 0.92）；
- release 档（-O2）：best **56.06×** vs 50×——帧绝对耗时 9.06ms（比 Linux 容器快 40%），
  但清屏分母 0.162ms（快 2.3 倍），**比值反而超标**。

**根因**：比值（整帧耗时 / 清屏耗时）的两个成分对机器/编译器的敏感度不同——
分母是可向量化的 `memset`（对 CPU 存储带宽/自动向量化极敏感），分子是标量绘制循环。
阈值 50× 在 Linux 容器（release 38×~45×）标定，到 Windows 上分母缩小更快 → 比值系统性
偏大 → 护栏失明（恒红后，真回归也会被当「环境噪声」忽略）。与测试自己注释里的
档位问题（release 24.5× / dev 60.7×）同根：**比值不是机器无关量**。

**方向（设计决策，待人工定夺）**：
- A. 阈值按「标定环境」自适应（构建时探测 OS/档位选阈值表）——把隐式假设显式化；
- B. 分母换成更稳定的参照（如固定字节数的 memset 基准而非清屏，或比值上下界同档自适应）；
- C. 主门禁只在标定档（release + Linux CI）跑，其它档降为提示级——与
  `frame_budget_threshold_has_bounded_headroom` 的 `calibration_profile()` 跳过策略同型。

**另注**：p95 门（100×）与故障检测门在 Windows 双档均绿——问题只出在 50× 主门。

**补充观察（2026-10-15 全量串行实测）**：同一台 Windows、同一天，`--slow` 单跑
双档恒红（53.86×/56.06×）而全量串行里** PASS**（1.95s）——比值不仅跨平台不稳，
还**跨机器状态不稳**（单跑 vs 混在 1464 例里的缓存/频率状态不同）。这坐实方案
A/B 的必要性：阈值只要还挂在「单点标定」上，无论标在哪个平台都会在另一个
时间维度上重现这个问题。同日第二次全量串行又红（复跑间隔约 20 分钟）——
三次观察（单跑红/全量绿/全量红）确认其**同机同时段内随机翻转**，这不是
「换平台重标一次」能解决的，必须换口径（方案 B）或按环境自适应（方案 A）。

**收益量化（同轮落地，224e625）**：script_write_partial_vs_full_frame_cost 在
96 卡片基准场景量出 JS 写单卡片文本的**局部帧中位 0.69ms** vs 强制整帧
**15.0ms**（约 22×，dirty 区 185×97）——这是 mark_dirty_all 收敛
（8f3fd00/3636500）在真实写路径上的收益数字，几何签名注释已引用。

**方案 C 的补强论证（读 app_frame_budget_test.cpp 拓出）**：主门禁已对**插桩档**
做了同型松弛（`kFrameLimitInstrumented=90×`，268-274 行：「本门禁断言的是整帧
没失控，松弛后仍是有效安全网；而 50× 是为发布档标定的」）——但 **dev 档没有**，
尽管文件自己实测记录了 dev/release 比值差 2.5 倍（60.7×/24.5×）且
`calibration_profile()` 已承认非标定档比值不可比。dev 档主门禁沿同一论证松弛
（如 90×，或与插桩档共用常量）是最小改动；Windows dev 实测 53.86×~56.06×
在 50× 上下随机翻转正是这个不对称的直接后果。仍属阈值语义决策→待人工定夺。

### P2：把「窗口真的上屏」纳入质量门禁（现在只靠一个独立探针）

**现状**：2026-10-20 的软件路径空白窗口（`present()` 早退与惰性 DIB 互锁）已修，
并留下 `tools/present_software_verify.py` 作回归探针（带三态退出码：0 过 / 1 红 / 2 环境不具备）。
但它**不在任何自动门禁里**，需人工跑。

**为何现在没直接接进 `st test`**：它要**真实窗口**（`GetDC(hwnd)` + `GetPixel`），
而单元测试跑在无头/CI 上；Windows 专用，Linux CI 只能 SKIP。

**方向（待定）**：二选一——
① 接进 `examples_e2e_all.py`（它已经在真实窗口上跑，平台天然对齐）；
② 给 `st::shell` 抽一个平台无关的「上屏字节回流」接口（无头也能验），
   代价是后端多一层接口。倾向①：不为了可测性改造产品接口。

**旁证**：本次逆向验证显示**三处守卫必须同时回退才能复现全白**——
只回退 `present()` 时 `WM_PAINT` 会把它救回来，像素断言仍绿（只有计数红）。
所以这个缺陷的判据必须**同时**包含「像素非空」与「`present_skipped == 0`」，
二者缺一都会漏。

### P2：阴影 `covered_region` 已实现且已量化 —— **当前调用点接入无净收益，不接**（2026-10-20 实测）

> 原先的 P1（“预计再省 ≈2.5 ms/帧”）**结论不成立**：那个数字是
> `PAINT_DIAGNOSIS §4.1` 对“纯阴影合成”的孤立估算，未计入跳过本身的代价。
> 接口已落地且正确（`DrawOptions::covered_region` + `covered_radius`，按行解析圆角跨度），
> 但**在两处 UI 调用点上实测无收益**，故**不接入**。下面是完整取证，别再重复。

**测了什么**（两套工具，都是本轮新增、已入仓）：

| 工具 | 作用 |
|---|---|
| `tools/shadow_covered_probe.cpp` | 传/不传 `covered_region` 的**等价性 + 判别力 + 收益**（双层/单层分开报） |
| `tools/shadow_inside_ratio.cpp` | 卡片内部到底有多少阴影像素（能省的上限） |
| `tools/simd_short_row_probe.cpp` | `over_row` 在短行下的 ns/px（解释分段为何亏） |
| （诊断探针，未入仓） | 分段阈值的行命中率（结论已并入本条，脚本用完即删） |
| `tools/paint_ab_compare.py` | 真实界面 `paint_ms` 的交替 A/B（3 轮 × 10 样本取中位） |

**关键实测数据**：

1. **能省的量是真实的**：卡片内部 **100%** 都有阴影像素，占全部阴影像素的
   **33.7%（48px 高）/ 55.0%（200px 高）**——`shadow_inside_ratio`。
   所以“理论上该有收益”这个直觉没错。
2. **分段代价也是真实的**（`simd_short_row_probe`，140px 宽、两侧均分剩余、多轮取最小）：

   | 跳过 | 两侧各 | 收益 |
   |---|---|---|
   | 40px | 50px | **+16~18%** |
   | 80px | 30px | **−6~−10%** |
   | 120px | 10px | **+55~62%** |

   **非单调**：两侧 30px 最差（落在 `over_row` 向量主循环的部分迭代区间，尾巴走标量），
   而两侧 10px 很快（数据量本身就小）。单价：长 1~6 → 3.6~6.1 ns/px（是 512 长的
   12~21 倍）、长 8 → 0.85、长 16 → 0.47、长 ≥32 → 0.28~0.33。
3. **但端到端仍是负收益**（`shadow_covered_probe`，多轮）：
   双层 **−0.8%~−9.3%**（偏慢）、单层 **±1.5%（噪声内）**。
   **为何与第 2 条的“纯 over_row +60%”不一致**：纯探针里中间段是**完全不执行**的；
   而真实阴影路径里，贴图行迭代、`covered_span_at` 的 `sqrt`、`plain` 分支判断
   这些开销一分不少。省下的混合量在那个总开销里占比太小。
4. **真实界面（gbcode，`paint_ab_compare.py`）**：基线中位数 **9.10 ms** /
   接入 **9.15 ms** —— 差 **+0.5%**，在噪声内。

**已做的改进（保留，这是修复而非优化）**：`canvas.cpp` 的分段判据原来没有下限，
“跳过 1 像素也算一笔” → 每行都切成两段，稳慢 13%。现在要求
**跳过量 >= 40px** （依据上表：<40px 时代价大于收益）。改完等价性仍是硬保证，
且耗时从“稳慢 13%”变为“噪声级”。

**重新考虑的条件**（何时值得再试）：
- 阴影贴图的单次面积大幅上升（如整窗大卡片、全屏遮罩）——那时中间段足够长，
  左右两段也能过 32px 阈值；
- 或 `over_row` 增加**短行快速路径**（如长度 < 8 时直接走 2 像素一组的
  朴素循环而不构造 SIMD 常量）——那会让分段重新变得划算。
- 判据：重跑 `shadow_covered_probe`，要求「双层 传 < 不传」稳定出现（多轮同向）。

**不要做的事**：不要因为“接口已经写好了、不用可惜”就去接调用点——
本轮实测就是这条路径：接上去只得到 +0.5%（噪声内）和掉进短段陷阱的风险。

## 已完成（2026-10-19：大 Markdown 预览不再冻结主窗 —— 排版提速 + 框架层慢内容异步化）

> 用户报告「打开 shuangtian/DESIGN.md 预览整窗变卡」并问「框架层能不能让慢组件
> 不影响主窗口」。两层都交了：
>
> ① **MarkdownView 排版提速**：懒 `flat_prefix`（逐码点前缀宽首次选择才算，
> 重排省 27%）、`tokenize` 零拷贝、表格 `word_floor` 延迟到真要压缩列、
> 折行词宽复用、`rows_.reserve`。实测 DESIGN.md（447KB/3553 行）：热态重排
> 97→51ms、首帧 paint 60→16ms、滚动帧 10.7→7ms。
>
> ② **框架层 `ui::SlowContent`**（通用慢内容壳）：排版投 `Tasks::run_async`
> 后台，骨架占位 + 估算高度不阻塞布局，`future` 非阻塞探测采纳，世代号丢
> 过期结果；小文档同步兑底。配套：字体栈并发安全压测探针
> （`tools/font_thread_safety_probe.cpp`，零畸变）、`MarkdownView` 的
> `snapshot_layout_input`/`layout_offscreen`/`adopt_layout`/`freeze_offscreen`
> 四接口。实测：同步 156.8ms 的排版移出后主线程最差单帧 0.40ms；
> gbcode 打开 DESIGN.md 预览命令往返 1.3ms、排版完成 3509 行。
>
> ③ 踩坑实录（详见 DESIGN.md §4.5.2.1）：壳路径下组件的 `measure` 会用兑底宽
> 触发同步重排，把后台成果冲掉——需 `freeze_offscreen` 冻结；`adopt_layout`
> 必须同步源进 `stream_`/`blocks_`，否则后续任何自身重排都用空源把行模型冲成 1 行。
>
> 回归：`tests/ui_slow_content_test.cpp` 四用例 + `tools/md_async_bench.cpp` +
> `tools/gbcode_md_async_e2e.py` 五断言；gallery 对账补 `SlowContent` 豁免（包装组件）。
> 环境注：本轮在 Linux 侧需 g++-16（libstdc++ 的 `<mdspan>`），已自举重编全仓。

## 已完成（2026-10-16：UiRoot 快捷键枚举面 + gbcode 帮助卡单源化，原 P2 落地）

> 原待做条（本文件 4009 行附近的「`UiRoot` 快捷键注册表无可枚举面」）已落地。
> 处置（框架面）：`UiRoot::shortcuts()` 只读**值快照**（`ShortcutInfo{name, mods}`，
> 登记名小写归一、注册序、不暴露 handler）——枚举面只读，写入仍只有
> register/unregister 两口；实现见 `src/ui/ui_root.cpp`（快照而非引用：枚举面
> 不得交出可变视图，且注册表随时可能变动，引用即悬垂窗口）。
> 处置（应用面，gbcode）：新增单源表 `examples/gbcode/shortcut_help.hpp`
>（41 条 spec：键位/修饰键/守卫形态/文案/分组一体登记），main.cpp 注册块改
> bind-by-id（键位零手抄，行为/顺序/守卫忠实移植），帮助卡正文改
> `shortcuts_card_body(root.shortcuts())` 生成（手写体废除），启动时
> `shortcut_drift` 对账点名表外/漏注册/抢注（实测零漂移）。
> 对账钉进测试：`tests/gbcode_shortcut_card_test.cpp` 五用例
>（round-trip 双向差集/文案⇄修饰键/卡片⇄注册表/守卫形态/基线数量）；
> 框架面补 `tests/ui_shortcut_test.cpp` 两用例（快照字段与独立性/枚举不触发
> handler）。
> 逆向验证：回退 shortcuts() 为返回空表（可编译形态）→ 3+3 用例红（不是假绿）；
> 顺带抓到并修掉测试自身的失败路径 bug（ST_CHECK 失败不中断用例，带下标断言
> 未守长度会越界崩进程）。恢复后全绿（9 用例 154 断言）。
> e2e：headless 真 gbcode 按下 Ctrl+K——卡片分组/关键行/36 行/零「其他」节，
> Esc 关卡、Ctrl+G/Ctrl+J 抽查键位真生效。
> 已知历史形态如实迁移：Alt+↑/↓ 根级登记名（"alt-up"/"alt-down"）与后端键名
>（ArrowUp/ArrowDown）不匹配，真移行是 CodeEditor 自己的分支——原样保留，
> 键位归一另单处理；表内已用注释点名，不在本单扩散。

## 已完成（2026-10-16：ContextMenu 声明式收尾，注册表 + 包装声明）

> 原待做条（本文件待做区「`ContextMenu` 未进声明式注册表」）已落地。
> 真值校准：注册表条目（`ST_COMPONENT_LIST` 的 `X(ContextMenu, ...)`）与
> `dsl::context_menu` 包装实现（dsl.cpp）**早已存在**，缺的是公共头声明与
> 库内测试（全仓无声明无调用的孤儿函数）——本单只补齐这两样，未重写任何实现。
> 处置：① `dsl.hpp` 补 `context_menu` 声明（menu_bar 同区），依赖面用最小代价——
> `st::ui` 命名空间下前向声明 `ContextMenu`/`MenuItem`（与既有 `MenuBar` 前向声明
> 同款先例），`menu.hpp` 的包含仍留在 .cpp；② 新增
> `tests/ui_dsl_test.cpp` 的
> `dsl_context_menu_creates_from_registry_and_writes_anchor_items`
> （作曲后从注册名造元素 + anchor/items 写入断言 + overlay 形态下 Esc→on_close→sweep
> 全链，18 断言）。
> 逆向验证：注释 dsl.hpp 声明 → 测试 TU 编译失败
>（`'context_menu' was not declared in this scope`，无声明则其它 TU 调不到）；
> 临时禁用 dsl.cpp 写入段（可编译形态）→ 测试红
>（`期望 menu->anchor_.x == 120.0f，实际 0 != 120`），恢复后回绿。
> 同步：DESIGN.md 声明式章节（浮层生命周期节）补 context_menu 条目。

## 已完成（2026-10-16：删除已被取代的 IWYU 差集体检脚本，P2 落地）

> 原 P2 条目（`## P2` 章末的废弃工具清理条）已落地：`tools/` 下只做「直接用到的
> std 实体」与「显式 include 的标准库头」差集的 IWYU 体检脚本已删除——它只查
> `.cpp`、包含根写死为无、实际从未被跑（528 处全假阳，输出在 GBK 控制台还是
> 乱码），且已被 `tools/check_selfcontained.py` 取代（后者连头文件一起真编一遍、
> 按组补齐包含根，是真验证；取代史已并入 `tools/README.md` 该行）。
> 处置：删文件 + `docs/README.md` 工具分类清单、`tools/README.md` 工具表同步摘除；
> 全仓字面名清零（git grep 兜底扫描，仅 `.evolve/` 调度器托管件命中）。
> 验证：check_docs.py 全绿（§ 引用/路径实存/BACKLOG 结构/PITFALLS 格式均合规）；
> 构建面零依赖（st.pkg 不含它、src/ 无引用、无脚本调用），按清理单零构建收单。

## 已完成（2026-10-15：裸文件名控制文件的落盘白名单修复，P2 落地）

> 原 P2 条目（本文件 3757 行附近）已落地：`default_capture_dirs` 对无分隔符的
> control_file 不再放行字面 `"."`（客户端请求的是绝对路径，前缀比较永远不匹配，
> 该目录下全部拒绝），改为展开为**进程 CWD 的绝对路径**（`fs::current_dir()`，
> `generic_u8string()` 正斜杠口径，与 `capture_path_allowed` 归一化比较同口径）。
> 顺带：拒绝报错文案改为附**实际白名单内容**（旧文案不指向真因；新文案列出
> 具体目录，裸文件名场景能直接看到 CWD 绝对路径）。
> 安全语义不变：带目录的 control_file 行为照旧；白名单外目录照拒。
> 证据：`tests/core_basics_test.cpp`（`has_cwd` / `!has_literal_dot` 断言）+
> `tests/control_protocol_test.cpp` 的 `capture_bare_control_file_allows_cwd_and_rejects_outside`
> （三分支：①裸文件名 CWD 下放行 ②白名单外照拒 ③带目录形态行为不变）；
> 逆向验证：回退展开逻辑后①变红（拒）、恢复后回绿，实测 2026-10-15。

## 已完成（2026-10-15：find 的 `#` 选择器含点 id 容错，P2 落地）

> 原 P2 条目（本文件 3698 行附近）已落地：`find` 对 `#xxx` 形态的纯 id 选择器
> 先按整串试精确 id 匹配（`ui::Selector::exact_id`，不 CSS 拆分），命中即用；
> 不命中退回 CSS 解析；两者都不中且 `#` 后含 `.`/`:` 时报错文案附
> `[id=...]` 属性写法指引（不再只有一句「找不到」）。
> 不含点 `#id` 两条路径本就等价（精确 id 匹配），行为零变化。
> 证据：`tests/control_protocol_test.cpp` 的 `find_hash_selector_matches_id_containing_dots`
> （48 断言，逆向验证回退容错后用例①③变红）；文档警示见 `DESIGN.md` §6.3 与
> `docs/PITFALLS.md` 场景 5。

## 已完成（2026-10-15：分片与全跑的口径统一，P2 落地）

> 原 P2 条目「分片与全跑的**口径统一**」已落地为回归钉子：`tests/shard_parity_test.cpp`。
> 12 个哨兵用例（env 门控）+ 主用例自启动本二进制，分三路口径钉住：
> ① 选择口径：`--list` 不分片 vs `--shard i/4`，逐片位置强断言（第 i 片第 k 名 == 全集下标 k*4+(i-1)，
>   同时钉住「并集为全集」「互不重叠」）；
> ② 执行口径：过滤执行退出码 0，执行名单 == 片 --list 名单，四片执行并集（集合相等）== 全跑名单；
> ③ 失败口径：哨兵 07 故意 FAIL，落第 4 片（7%4=3）退出码 1、四片和 == 全跑 == 1——失败不被分片吞掉也不被放大。
> **验证**：filter 直跑 PASS（99 断言）；单进程全跑 1443 passed / 0 failed / 26788 assertions；
> `--test-jobs 4` 分片驱动 exit 0；**逆向验证**：注释 run_all 的 in_shard 过滤行后用例三路口径全红
> （片名单 12≠3、并集 48≠12、failed_shard_sum 4≠1），恢复后回绿。lint 0 违规
> （L3 X 宏走清单豁免，已登记 CONVENTIONS §8）。
> **顺带记录**（未修，见下方 P2 新条目）：POSIX 侧 `process::run` 的 env 是追加式、
> 同名键不覆盖（Windows 侧正确去重）；空分片的「JUnit 报告写入失败」噪声。

## 已完成（2026-10-11 C 轮：「数据层已完成」的 4 项补完 + 2 项评估定案）

> 上一轮留了 4 条"数据层已完成、视图层待做"。这一轮把它们**全部补成可交付物**，
> 另做两项"评估后定案"（C5/C6）。逐条见下，每条都有各自的可验证产物。

- [x] **C1 冲突三份取数**（`read_conflict_sides`）：`git show :1:/:2:/:3:` 逐个取，
  每份各自报 `ok`/`missing`。**不做"要么三份都有、要么失败"**——非冲突文件只有 `:0:`，
  而"删除/新增冲突"会缺某一号，当整体失败会把整类情况误报成取数失败。
  测试**真造一个冲突**（两分支改同一行），断言三份内容各自正确（序号不能反）
  且能直接喂给三方合并。
- [x] **C2 `st::ui::MergeView`**（三栏 + 逐块取舍 + 冲突跳转）：独立组件而非
  `DiffView` 的第三种 layout——合并需要一个 `DiffView` 没有的东西：**每段的选择状态**
  （`disabled_lines`/`read_only` 的含义在两种语义下会分叉）。
  修出真缺陷：`next`/`previous` 跳转不对称（"只有一处未决定冲突时 Shift+F7 没反应"，
  而屏幕上还挂着"还有 1 处未决定"）。gbcode 侧冲突文件点开**自动进合并视图**；
  三份内容在帧首泵取（子进程不能在渲染期起）；选择变化**不在回调里写盘**（置标记、帧首落盘）。
- [x] **C3 逐行暂存界面**（差异视图勾选列 + 工具条）：勾选框做成
  `CodeEditor::LineDecoration` 的一部分（逐行对齐只有编辑器知道；宿主在外面画必然与
  真实行高/滚动分叉）。**挖出 4 个真缺陷**：`checks[side.lines]` 是 off-by-one
  （`side.lines` 在 `begin_row` 内部才自增 ⇒ 永远取到 `None`，**整个勾选列不显示**）；
  勾选框嵌在 `if (use_ink)` 里而 `LineMark::None` 的行不画；`set_staging_mode` 只标脏
  不重建装饰；`set_diff` 不清选择（下标当身份用）。
- [x] **C4 `st::ui::TimelineView`**（标尺/轨道/片段/播放头 + 拖拽修剪）：
  数据层已就绪，组件只做"调换算 + 画 + 翻译事件"。修出真缺陷：
  **播放头抢走片段左缘命中**（播放头默认停在 0ms、命中带 3px，而第一个片段也从 0 开始）
  ——修法是按区域分化优先级（标尺带里播放头优先；轨道区里修剪边缘优先）。
  另把输入合并**从 `dispatch` 挪到宿主帧循环**（见下）。
- [x] **C5 画笔输入**：BACKLOG 要求"先量再定"，量完的结论是
  **该升框架的是"高频事件合并"，不是"指针捕获"**（后者框架早已做）。
  `WM_MOUSEMOVE` 拖动时可达 125~1000 次/秒而后端不合并——三重代价里最严重的是
  **撤销粒度**：命令式撤销下"一次移动 = 一条命令" ⇒ 画一笔几百条历史，
  用户按一次撤销只退 1px。新增 `st::ui::InputCoalescer`（保留首尾、拖拽期间不合并），
  louyue 侧 `PixelPatchCommand::try_merge` 做到**一笔一条历史**。
- [x] **C6 视频解码**：评估后定案**走 `ffmpeg` 子进程**（不链接 libavcodec）——
  依据与代价（每帧一进程 ~30ms，所以定位是素材查看而非播放）写进了头文件对比表。
  三条契约都围绕"别让缺能力看起来像文件坏了"（探测不到如实 `Unsupported`、
  探测判据是"文件真的在"、不是视频报 `Invalid` 而不返回全零元信息）。

### 两个被"整进程崩溃"掩盖了很久的真缺陷（见上方 C 轮说明）

- [x] 用例未捕获异常 → 框架层加异常边界（记 FAIL 并继续）。
- [x] `ScriptBridge::expose` 的命名空间契约（挂到 `app.` 之下）。

### 顺带

- [x] `st lint` 存量 2 项违反清零（`ui_root.cpp` 的诊断计数器加 `lint-allow: L8`）。
- [x] `video.cpp` 的探测缓存改用**函数内静态**（原文件作用域全局触发 L8）——
  初始化顺序明确，也不会被别的翻译单元的静态初始化意外读到。

## 已完成（2026-10-11 「待做」全章清账：13 项）

> 本轮把 `## 待做` 章的 13 条**全部处理完**（逐条见下），并顺带根治 3 个不在清单里、
> 但被这轮工作逼出来的**构建期真缺陷**。工作区干净，每条独立提交。
>
> 反复出现的两类形态（值得当成方法论）：
> 1. **「数据算对了、界面没接上」**——B7（行内区间）与 B6（历史副行）各撞一次。
>    两次都是"数据层单测全绿、用户看不见"，而判据分别是"区间真的到了编辑器"
>    与"列表真的画了副行"。**跨层的功能必须有一层断言它在下一层真的出现**。
> 2. **「静默失败」**——B10 的两个缺陷（点了输入框敲字没反应、输新颜色没反应）
>    都不报错。它们的共性是"我做了个 shadow 状态，而框架有权威状态"
>    与"缓冲区没清导致追加超长"。**凡是有权威来源的状态，绝不在本地再存一份**。

- [x] **B1 命中判定命名约定**：`Element::hit_test` 上写明「自绘子元素做细分命中时请另起
  名字」（caiyun 的时间轴只好叫 `grab_at`，而每个新自绘组件都会撞一次）。
- [x] **B2 诊断路径口径回归**：新增用例钉住「URI 形态推入诊断、正/反斜杠两种活动路径
  都读得到」。存量缺陷（曾让整个测试分片进程被带走），此前修了但**没有回归**。
- [x] **B3 墙钟帧间隔**：`Metrics::wall_p50_ms`/`wall_p95_ms` + `metrics` 协议上报；
  caiyun 压测改用统一字段（原先用「总秒数/总帧数」自算——那是**平均**而旁边是**分位**，
  两种统计量摆一行会被当成同类）。实测：墙钟 15.96ms vs 帧内 6.87ms，「被限帧」一眼可辨。
- [x] **B4 `TitleBar` 品牌区**：`set_brand()` + `brand_rect()`，gallery 改用（原现场是
  把品牌名塞进 `leading` 槽）。修出真缺陷：品牌名宽度**不能依赖 `TextPort`**
  （无头时量什么都得 0），改用保守估宽。
- [x] **B5 blame 行尾注解**：`LineDecoration::end_text`（框架能力）+ 状态栏开关 +
  按视口取数。修出 3 个真缺陷：`open_path` 写悬垂 `editor` 指针（**直接崩**）、
  构造期改 State 触发递归重组（标签对了内容不对）、`load_active_text` 在编辑器未创建时
  仍标记已装载。顺带补 `--open <path>`（编辑器历来该有）。
- [x] **B6 单文件历史弹窗**：`read_file_history` 的界面入口（编辑器右键菜单 + 命令面板）。
  修出真缺陷：`List::Entry` **没有 `subtitle` 字段**，于是 `dsl::ListItemData.subtitle`
  静默丢弃（"数据模型有、转接不搬"）。
- [x] **B7 行内字符级差异**：`diff_inline` + `refine_inline_diffs` + 编辑器行内区间底色。
  按码位而非字节切（字节级 LCS 会把汉字切成 2+1，高亮错位）。
  测试纠正了我的一个错误预期：`count`→`total` 是**两段**而非一段（有共同字母 t/o）。
- [x] **B8 `st::edit::UndoStack`**：撤销栈原语（5 例 78 断言）。
  三条语义各对应一个静默错：中途落新命令丢弃未来、空命令不入栈、游标与条目一起可查。
- [x] **B9 `st::ui::Viewport`**：二维缩放/平移 + 棋盘格透明底（7 例）。
  三条关键设计：缩放以锚点为心（正确性判据）、到边界后不再推 pan（防漂移）、
  棋盘格锚在内容坐标系。
- [x] **B10 `st::ui::ColorPicker`**：HSV 面板 + 十六进制 + 吸管（8 例 127 断言）。
  灰阶时保住色相（否则"我刚选的红"会悄悄变基）；解析失败如实拒绝（不静默变黑）。
- [x] **B11 逐行暂存**：数据层 `build_partial_patch` + gbcode 的 `StagePatch`/`UnstagePatch`。
  真 git e2e 验证「暂存区只有选中的改动」。踩到 `core.autocrlf` 的坑（LF 上下文 vs CRLF 工作区）。
- [x] **B12 三方合并数据层**：`merge_three_way`/`merge_apply`（9 例 55 断言）。
  五态区段（`BothSame` 不算冲突——把"两边加同一行"当冲突是纯噪音）。
  修出两个真缺陷：尾部纯插入丢失、相邻不重叠被误判成冲突。
- [x] **B13 Timeline 数据层**：多轨道 + 换算 + 命中优先级 + 像素阈值吸附 + 标尺步长（7 例 79 断言）。
  修出真缺陷：极窄片段点正中被判成左缘（"想移动却变成修剪"）。

### 顺带根治的 3 个构建期真缺陷（不在 BACKLOG 里，被这轮工作逼出来）

- [x] **测试链接命令行超限**（Windows `CreateProcessW` 32767 字符）：244 个测试单元时
  估算已达 32974。报错是 `无法启动 'g++.exe': 文件名或扩展名太长`——**完全指不到命令行
  长度**，而且时好时坏（取决于多少单元需重编），我先前几次撞上都当成环境抖动。
  修：主动估算 + 响应文件（`@file`）退路。**它解锁了"新增测试文件"**（此前不可能）。
  自己写错过一次：响应文件里用"长得像路径"挑参数，把 `-o` 的产物路径也挑走了
  → 报 `could not open ...st_tests.exe`（看着像产物不存在）。改成调用方显式分三段。
- [x] **`dbghelp`/`nanosrv` 链接库口径不一**：`bootstrap.ps1`（自举）、`st.pkg`（框架）、
  `default_system_libs`（**独立工程**走这条）三处各不相同，导致
  `independent_project_builds_links_and_runs` **长期失败**（BACKLOG 曾把它记为
  "既有问题、与本轮无关"）。MinGW 的 `libdbghelp.a` 只是壳，真正的 API 存根在 `libnanosrv.a`
  ——GNU `ld` 有时替你拉上而 `ld.lld` 会如实报未定义。三处补齐后该用例转 PASS。
- [x] **`List::Entry` 缺 `subtitle`**（见 B6）。

### 图标尺寸的相关量（后续如再调，从这里起）

本轮把图标盒径改成独立尺度令牌（`Metrics::icon_size_sm/icon_size/icon_size_lg`
= 20/22/26），活动栏图标墨迹从 16px 涨到 20px（物理）、五个图标的最长边离散度
从 1.25 收到 1.10。**若要再调制档位**：改令牌即可（不必改组件），
但记得同步 `DESIGN.md` §5.0.4 ④与本节的数字（那是决策依据）。


### P2：`st test --shard N/4` 在 Windows 上的“分片未能启动”已定位并修（备查）

**现象**：分片报“未能启动”而**一条失败记录都没有**，前面那一千多条用例明明全 PASS。

**根因两层**：
1. 假 LSP server 是 POSIX `sh` 脚本，Windows 不认 shebang → 握手必失败；
   而用例用 `ST_CHECK(fake_ready)`（不中断）后紧跟 `completion_trigger_characters[0]`，
   空 vector 取下标 → `_GLIBCXX_ASSERTIONS` `abort()`，**整个分片进程被带走**。
2. `gbcode` 桥的 `diagnostics` map key 两侧不同源：写入走 `uri_to_path`（Windows 下 `/`→`\`），
   查询走 `active_path`（`/` 形态）→ 查不到就返回空表（**不报错**），
   测试里 `.front()` 空 vector 又一次 abort。

**已修**：① 前置条件改 `ST_REQUIRE` 且诊断打印移到断言**之前**；
② 四条跑脚本的用例标 `ST_TEST_SLOW` + 平台跳过（环境不适配≠代码缺陷，
与 `lsp_language_smoke_test` 同一姿态）；③ 桥内新增 `normalize_path` 收束读写 key 口径。

**待办**：这两处是**存量**缺陷（`git stash` 验证过改动前同样崩），但它们只是把
“崩”变成“可见”——真正的缺口是**没有再回归测试**钉住“诊断在 Windows 上不会全丢”。
补一条：`service.sync_document_if_ready_for_test("C:\\x\\b.cpp", ...)` + `set_active_path("/x/b.cpp")`
后 `problems()` 必须非空（逆向验证：拆掉 `normalize_path` 应立刻变红）。



四项待做全部落地并推送（`b0cca9ea` / `681cd728` / `373ce10f` / `3c066250`）：

**① 图标表防重名单测**（`ui_icon_names_have_no_duplicates`）：断言条目数 == 唯一名数。
逆向验证过（植入 `triangle` 副本 → 变红并报出该名 → 清除恢复绿）。写这条时踩了
两个 `std::unique` 的坑（它把重复项**移走**、尾部内容**未指定**——把返回值当"重复
个数"会报成"74 条 74 个重复"，读尾部列举重复名会把 `triangle` 报成 `warning`），
最终改为直接扫相邻相等对。

**② lint L14 补「POSIX 函数直调」**：判据加 `::getpid|fork|execv|waitpid|kill|usleep|
nanosleep|clock_gettime|gettimeofday|opendir|readdir|closedir` 等**限定调用**形式。
缺口确证：`roots` 其实**已经**含 `tests/`，没抓住三处 `::getpid()` 是因为 L14 只盖
"系统头 include"与 `dlopen/dlsym`。植入验证过（植入 `::getpid()` → lint 精确报出该行）。

**③ LSP 相关测试跨平台**（三处，性质不同——分开处置而不是一律跳过）：
`lsp_client_uri_round_trip` 是**真缺陷**（测试硬编码 POSIX 路径期望，而
`path_to_uri`/`uri_to_path` 本就平台相关）→ 按平台给期望值；三个 `stream_handle_*`
夹具写死 `/bin/sh` → 新增 `tests/shell_fixture.hpp`（POSIX `/bin/sh`，Windows
PowerShell；口径与 `core_pty_test.cpp` 一致）；两个假 server 用例依赖可执行脚本 →
改 `ST_TEST_SLOW`；四个 `lsp_language_*_smoke` 的「`which` 找到 ≠ 能起来」→ 未就绪时
如实打印原因后跳过。**清完后全量测试首次 0 失败**。

**④ 镂月裁云两示例**：见 `DESIGN.md` §8.1 与两条提交说明。实现中当场修了两处框架
缺陷（`Canvas::draw_canvas` 忽略 `DrawOptions::blend`；`custom<T>` 无法接示例级组件），
两者都配了回归测试与逆向验证；反推出的新待做见本文件「待做」段。

## 已完成（2026-10-08 开发/测试过程沉淀：真窗口验证工具链）

### 动机

本轮修终端时，「真窗口实测」环节反复返工（临时脚本一个按键起一个 PowerShell），
时间花在**工具而不是问题上**：(1) PowerShell 启动抢走前台焦点 ⇒ 按键送不到窗口；
(2) `PostMessage` 不更新按键状态 ⇒ `Ctrl+←` 永远不生效；
(3) 拿「屏幕文本」判读被 PSReadLine 预测文本与 IME 组合窗污染。

### 产物

| 产物 | 内容 |
|---|---|
| `tools/st_win_input.py`（新） | 真窗口按键注入库：三条通道（`PostMessage` / `keybd_event` / `SendInput`）按"要不要修饰键、要不要前台"选型；`ctypes` 进程内调 user32（**不启子进程**，避抢焦点）；带 `ensure_foreground`（失败如实报，不无限重试） |
| `CONVENTIONS.md` §7.7.1（新） | 三条注入通道的选择表 + 三个已踩的坑（含“验一次按键只需发一条 `WM_KEYDOWN`”那条——补 `WM_CHAR` 会把缺陷原形写进夹具） |
| `CONVENTIONS.md` §7.7.2（新） | 量尺选择：别用屏幕文本判读；要选“与被测机制同构且不受重绘影响”的量（光标列/属性面/像素哈希） |
| `CONVENTIONS.md` §7.8 | 判据自己也会错（同一轮两处错都在判据）——先在已知合格/不合格样本上各跑一次 |
| `CONVENTIONS.md` §9.0（新） | 迭代循环两个卡点：应用在跑 ⇒ 链接 `permission denied`（**不是代码错**）；要真窗口就别用无头默认值起动 |
| `docs/README.md` | 按意图索引补两行（真按键验证、判据可信性） |

### 验证

- `tools/st_win_input.py` 本体自测：真窗口上 `type_text('abc')` ⇒ 终端光标列 **+3**；
  `press_key('left', ctrl=True)` ⇒ 位移 **−3**（按词跳）——两条通道均通；
- CLI `--help` / 探针可跑；`py_compile` 通过；`check_docs.py` 引用全部解析。

> 本轮沉淀的是"怎么验"；「第一次输入要回车两次」那个缺陷本身仍未定位（另起一轮）。

## 已完成（2026-10-08 终端亮暗主题配色 + 真彩声明）

### 现象（用户报）

「亮色主题高亮颜色看不清，终端要适配下亮暗主题的显示，支持真彩颜色」+「终端背景色也得调下」。

### 根因（已量化）

ANSI 前 16 色在 `terminal.cpp` 里**写死一套深底调色板**（xterm 默认值）。
放到亮色终端底 `#DDDDE4` 上：

| 颜色 | 浅底对比度 | |
|---|---|---|
| 亮黄 `#E5E510` | **1.00** | 与底色毫无差别 |
| 亮白 `#E5E5E5` | **1.07** | 同上 |
| 亮绿 `#23D18B` | **1.47** | 同上 |
| 青 `#11A8CD` | 2.07 | 偏弱 |

**16 色里 12 色低于 3:1**——这就是“高亮看不清”（`git diff` 的加粗高亮正是亮黄）。

另一半：终端底色直接借 `surface_sunken`（亮 `#DDDDE4` / 暗 `#050507`），
那是**通用凹槽语义**（输入框/代码底共用），不是终端该有的“一块屏幕”。

真彩部分：解码侧本来就对（`ansi_screen.cpp` 已处理 `38;2;r;g;b`），
但**对外没声明**——子进程里 `TERM`/`COLORTERM` **实测全空**，
程序（`git`/`ls`/`bat`）据此判定“不支持颜色”而**不上色**。

### 修法

- 新增 `TerminalPalette`（`Theme` 的第三块色板，与 `SyntaxPalette` 同一先例）：
  `bg`/`fg`/`cursor`/`selection` + 16 个 ANSI 色，亮暗各一套；
  亮底 `#FFFFFF`（16 色 6.2~11.2:1）、暗底 `#0C0C0C`（5.2~19.6:1）。
- `Terminal` 改从主题取色（删掉写死的 `kBase`；底色/前景/光标均不再借 `Palette`）。
- PTY 子进程注入 `TERM=xterm-256color` / `COLORTERM=truecolor`：
  Windows 侧手拼完整环境块（`lpEnvironment=nullptr` 是继承，要注入就得交出全量）；
  POSIX 侧用 `setenv(..., 0)`（**不覆盖**用户已有的 `TERM`）。

### 验证

| 项 | 结果 |
|---|---|
| `tests/ui_terminal_palette_test.cpp`（6 用例） | 全绿；对比度走 `st::math::contrast_ratio`（与主题自检同一份实现） |
| `tools/terminal_palette_reverse_verify.py` | 3 条回退**全部确实变红**（非恒绿测试） |
| 真窗口·亮色 | 底 `#FFFFFF`；绿/红/黄/青/**粗体黄**逐色可读（截图核验） |
| 真窗口·暗色 | 底 `#0C0C0C`；子进程读到 `TERM=[xterm-256color] COLORTERM=[truecolor]` |
| 全量测试 debug | **0 failed** |
| `st lint` | 0 违规 |
| mingw 交叉编译 | 通过（改了 Win32 平台分支） |

### 顺带记下的踩坑（写测试时自己先错了一次）

- **色相判据不能写成“红蓝都占主”**：合法的品红会 R≈B（`#861f9b` 蓝略高、`#ce63ce` 红蓝相等），
  判据应是“**绿最低**”；青同理是“**红最低**”。第一版写成“两个主通道都领先”把合法色判成了不合格。
- **亮色 brCyan 一开始与常规 cyan 取成同一个值**——“加粗提亮”那条断言当场拓出（这正是它存在的意义）。

## 已完成（2026-10-08 终端输入双回显：`KeyDown` 与 `TextInput` 双来源 + 修饰方向键死代码）

### 现象（用户报）

在 gbcode 的真窗口终端里**敲一个字符回显两个**（如按 `a` 出现 `aa`）。

### 根因（实测确认，两层）

1. **文本双来源**（用户可见症状的直接根因）：Win32 后端 `pump_messages` 调了
   `TranslateMessage`，于是一次物理按键产生 **`WM_KEYDOWN` + `WM_CHAR` 两个消息**，
   分别转成 `KeyDown(key="a")` 与 `TextInput(text="a")`。而 `Terminal::on_event`
   **两条都当文本来源**：`KeyDown` 落进裸字符回落（旧 `press_plain_bytes`）送一遍，
   `TextInput` 再送一遍 ⇒ PTY 收到两个字节、shell 回显两次。
   **框架契约是「`TextInput` 是文本的唯一来源」**（`Input`/`CodeEditor`/`TextArea`
   一致如此，`KeyDown` 里的裸字符一律不进字），终端违反了它。
2. **修饰方向键映射是死代码**（顺带实测发现）：`Ctrl/Shift/Alt + 方向/Home/End` 的
   `else if` 分支排在裸键分支（`key == "ArrowUp"`）**之后**，而裸键按字符串比较、无条件
   抢先匹配 ⇒ 修饰组合永远走不到。实测：按一次 `Ctrl+←` 只左移 1 格（应送出 `ESC[1;5D`），
   而 raw `ESC[1;5D` 在同一个 shell 里确实能按词跳 3 格。**即上一轮提交声称
   「Alt组合、修饰方向键全失灵 → 已补齐」的能力从未生效。**

### 实测数据

| 输入 | 修前 | 修后 |
|---|---|---|
| 只发 `KeyDown('q')` | 屏上 `q` | 无（文本归 `TextInput`） |
| 只发 `TextInput('w')` | 屏上 `w` | `w` |
| **一次按键** = `KeyDown(ch)`+`TextInput(ch)` 成对 | `aabbcc`（每字两遍） | `abc`（每字一遍） |
| 真窗口 PostMessage 3 次 `WM_KEYDOWN`（经 TranslateMessage 派生 WM_CHAR） | — | 光标列 **+3**（每按一次恰好 1 字符） |
| 真窗口 按住 Ctrl + `←` | 位移 −1（一格） | 位移 −3（按词跳） |

### 为什么既有测试全绿（可复用的教训）

控制通道的 `input.key` **只发 `KeyDown`**、`input.text` **只发 `TextInput`**——
**单发各自都正常**，只有真窗口才会成对出现。于是所有走控制通道的用例都看不见这个问题。
回归用例必须**显式构造「成对」这一输入形态**（`terminal_keydown_then_textinput_sends_one_character`），
而不能依赖某个驱动的既有调用序列。

### 修法

- 把输入翻译提为**纯函数 `Terminal::key_bytes(const Event&)`**（头文件声明）：
  可打印字符只认 `TextInput`；控制键/`Alt+字母`/`Ctrl+字母` 仍由 `KeyDown` 翻译；
  修饰 + 方向/Home/End **先于**裸键分支判定，按 xterm 参数编码（`1+Shift+2*Alt+4*Ctrl`）。
  删掉因此退役的 `press_plain_bytes` 与其 `<unordered_map>` 依赖。
- 回归：`tests/ui_terminal_input_test.cpp`（7 个用例，覆盖成对形态/裸键不发/修饰参数/
  组合修饰/控制键与 Alt·Ctrl 不倒退）。
- 逆向验证：`tools/terminal_input_reverse_verify.py`（3 条回退 → 期望红 → 恢复），
  实测**全部确实变红**（不是恒绿测试）。
- 契约写进 `DESIGN.md` §4「文本来源契约（`KeyDown` vs `TextInput`）」。

### 顺带发现（未修，待后续）

`tools/gbcode_e2e.py` **第一条断言就死**：它等 `#terminal-input`（旧「输出区 + 输入框」形态的
钩子），而 gbcode 的终端已迁到真终端组件（PTY，**没有输入框**）——该 id 早已不存在。
实测：**回退本轮改动（`git stash`）后失败完全相同** ⇒ 与本轮无关的存量漂移。
即这份 e2e 脚本自终端换真终端起就整体跑不起来了（29 组全不可达）。
方向：按真终端形态重写它（输入走 `terminal` 的 `send`/`send_line` 动作与
`input.key`，断言读 `screen`/`cursor` 属性）。

## 已完成（2026-10-07 分层重构：行排版抽为框架低级组件 `LineLayout`）

用户对前两轮返工的判语：「有这么难吗，是不是框架或组件设计不合理」+
「组件的设计原则是复用和定制，高级组件尽量使用低级组件组合」+
铁律「**框架层的问题不要应用层修**」。三条都成立——本轮按此重构。

### 诊断（已核实）

| 事实 | 证据 |
|---|---|
| **13 个组件各自调 `port.draw` 并各自算垂直定位** | `basic` / `input` / `list` / `table` / `tabs` / `select` / `slider` / `toggle` / `overlay` / `title_bar` / `markdown_view` / `element` / `code_editor` |
| 同一件事有**三条实现** | `Element::paint_text`（墨迹居中）/ 组件直接调 `centered_line_top` / 编辑器自己一套 |
| 前两轮返工就是在这三条路上的第三条打补丁 | 组件里长出 `line_block_offset` / `caret_top_cache_` / `text_offset_cache_` / `band_top` ——**全是框架该给的几何** |
| 又造出 **6 个散落 helper** | `line_block_offset` / `line_leading_split` / `line_ink_top` / `line_ink_metrics` / `line_ink_height` / `centered_line_top` |

### 改了什么

1. **新增 `include/st/ui/line_layout.hpp`**（低级组件）：
   `LineGeometry{origin_y, ink_top, ink_height, baseline}` + `layout_line()` /
   `layout_text_line()`。几何量**全部相对盒子顶**（多行控件可缓存复用）。
2. **删掉全部 6 个 helper**（连同 `code_editor` 的私有 `line_block_offset`）——
   它们的推导并入 `layout_line`；`centered_line_top` 保留为薄封装（迁完 13 个组件后可删）。
3. **`CodeEditor` 改为纯复用**：组件内**不再有任何手调偏移**，三个量全从
   `LineGeometry` 取（文字 `origin_y` / 带与光标 `ink_top`+`ink_height`）。
4. 属性面报 `line_height` / `text_offset` / `caret_top` / `ink_height` / `baseline`
   ——**自动化能直接核行盒几何**，不必自己再推一遍。

### 顺带堵住一个真回归（框架层）

空端口（无字体）下 `ink_metrics` 报不出墨迹，我第一版让 `ink_height = 0`
⇒ **光标条高度塌成 0、直接不可见**，`ui_focus_semantics_test` 的像素断言当场变红。
修在框架层：`layout_line` 给兜底几何（行盒居中 + `line_height × 0.8` 的墨迹高），
并写明这是**本层要守的不变量：调用方拿到的几何永远可用**——
不该让每个组件各判一次“报不出墨迹怎么办”。

### 验证

| 项 | 结果 |
|---|---|
| 新增 `tests/line_layout_test.cpp` | 4 条（墨迹居中 / 不多减 below / 两侧分摊 / 几何自洽）——回退均变红 |
| **像素等价**（重构前后同一场景） | **编辑器代码区逐像素零差异**（0 / 911200 px） |
| 全量 `st test --test-jobs 4` | **exit=0**（862 用例 / 21014 断言） |
| `st lint` / mingw 交叉编译 | 0 违规 / 通过 |

### 还没做（第5步，可分批）

13 个组件里还有 5 处在直接调 `centered_line_top`（`basic.cpp` ×2、
`components_internal.hpp`、`element.cpp`、`markdown_view.cpp`）。它们**行为已经是正确的**
（走的就是同一口径），迁移是**结构收窄**而非修缺陷：把最终那层薄封装也去掉，
让所有组件都直接读 `LineGeometry`。按“一次一件事 + 每步验证像素”的原则分批做。

## 已完成（2026-10-07 行盒垂直定位收尾：高亮带贴墨迹 + 基线不随内容漂移）

用户第二轮反馈：「背景和光标还是偏下」。上一轮的“增量两侧分摊 + 墨迹在行盒里居中”
只解决了一部分——本轮把它拆到底。

### 步骤 1：先承认“墨迹居中”的思路是错的（质心实测）

| 行类型 | 修复前质心（行盒 22.77，中心 11.39） | 改后质心 |
|---|---|---|
| 拉丁代码行 | 8.41（在行盒 **28%** 处） | 6.41 |
| 中文注释行 | 11.90 / 11.10（**46~52%**） | 10.56 / 9.77 |

拉丁行与中文行在行盒里的位置相差 **4.2px**——**一个位移不可能同时让两侧居中**。
根因：参考墨迹带被**括号**（细高笔画）撑高，居中它的**包围盒** ≠ 居中**视觉重量**。

### 步骤 2：真正治病的改法——高亮带贴合墨迹

不再调“文字在带里的位置”，而是让**带贴合文字**（`band_top` / `band_height` 取参考墨迹带）。
好处：带高从 22.77 降到 **15.07**（与一行代码的墨迹同高），“带里的空当朝哪边偏”
这个问题直接不存在；光标与带同源（上下各多伸 1px）。
实测：高亮带从 `y 375..408`（34px）变为 `y 381..402`（22px），底纹上下沿紧贴文字。

### 步骤 3：順手掘出的真缺陷——基线随内容漂移

`TextRenderer::shaped_ascent` 原先扫每个码点取 `max(face->metrics().ascender)`：
中文字体的 ascender 比拉丁大得多，于是“这行有没有中文”改变了基线。

实测（字号 15、Monospace）：拉丁 `11.14` / 中文 `15.87`，差 **4.7px**——屏上表现为
**中文注释行比代码行低 3.3px**，而且几乎贴到行盒底（下留白 0.0~0.4px）。

为何是缺陷：① 行高/命中/内容高度都用**主字体量尺**，只有基线扫内容；
② 绘制里“行盒顶”由同一段文字的 `shaped_ascent` 反推，漂移会反馈进行盒几何；
③ 字体回退的通行语义是“**字形**换字体、**基线不动**”。
改为直接返回 `ascent(size)`（主字体量尺）后，中/拉丁行质心差从 3.5px 降到 1.2~1.5px
（剩下的 1.2px 是 CJK 方块字与拉丁 x-height 的**字形分布**差异，不是定位错位）。

### 验证

| 项 | 结果 |
|---|---|
| 新增 `tests/text_baseline_test.cpp` | 1 条（基线不随内容变）——回退即变红 |
| `tests/code_editor_line_spacing_test.cpp` | 3 条（居中/不溢出/线性）——回退即两条变红 |
| 属性面 | 新增 `line_height` / `text_offset` / `caret_top`，让自动化能直接核行盒几何 |
| `st test --test-jobs 4` | **exit=0**（861 用例） |
| `st lint` / mingw 交叉编译 | 0 违规 / 通过 |

### 第三次迭代（用户：「上部的背景和光标怎么没了」）—— 带 = 行盒，文字居中

我上一版把**带缩短成墨迹带**去治「偏下」，制造了第三报。实测（编辑器代码区，物理 px）：

| 对象 | 上一版 | 本版 |
|---|---|---|
| 底纹带/行盒 | **381..402**（15.07 逻辑） | **375..408**（22.77 逻辑） |
| 文字墨迹 | 375..395 | 375..395（未变） |
| 光标 | 略 | 375..408 |

带短了 7.7px ⇒ **行距变成带与带之间的空白缝隙**，“背景被切短了”。

**两报的唯一合理解**：

| 现象 | 真因 | 修法 |
|---|---|---|
| 「背景和光标偏下」 | **文字没在盒里居中**（`centered_line_top` 多减一个 `below`） | 修 `origin_y` |
| 「上部的背景没了」 | 带被缩成墨迹带 | 带**回到行盒** |

两报**不矛盾**：该修的是**文字居中**，不是带的长短。我当时把“带太长 + 文字偏下”
误读成了“带太长”，于是缩带——正是这个误读造成了第三报。

新增回归 `line_layout_band_spans_the_whole_box` 与 `line_layout_band_never_cuts_off_the_text`
（后者用**真字体量真实文本的墨迹**，把带的尺寸与“字体实际字形”绑在一起，
不再依赖固定样本的巧合）。

### 工艺记录（本轮又踩的两类假信号）

1. **量尺把光标的半透明边缘当成了墨迹**：光标边缘是淡紫（`(212,196,224)`），
   “饱和蓝”判据排不掉；靠窄排除窗也排不到（边缘在光标列外 10px）。
   改为“向两侧扩到离开蓝色边界再留12px”，并新增 `tools/line_geometry_shot.py` 固化（该脚本后来已删，结论由逆行验证脚本接管）。
2. **探针与组件可能不共享同一份 inline 实现**：`line_block_offset` 是头文件 inline，
   而 `st build` 的增量判定会因 mtime/缓存把旧版本留在某个 `.o` 里——
   `line_box_probe`（单独重建）拿到 `4.13`、而 `gbcode`（复用缓存）拿到旧值 `-0.03`。
   **屏幕上看到的是应用的值**，所以核验必须读**组件属性面**（故新增那三个属性）。
   清除 `build/dev` 全量重建后两边一致。

## 已完成（2026-10-07 代码编辑器“行间距全在行下方”：行距增量两侧分摊）

用户反馈：「代码编辑器的行间距怎么全在行下方」。**观感类改动**，按
`docs/perceptual_changes.md` 的八步循环做（先量化、再改、再逆向验证）。

### 现象→正交量

先建**两个正交量**（手册 §2：“至少两个同向才可宣布更好”）：

- ① 上留白 = 行盒顶 → 墨迹顶；下留白 = 墨迹底 → 行盒底；取**下−上**
- ② 墨迹中心 − 行盒中心（字形在行内是否居中）

量尺：`tools/line_box_probe.cpp`（与绘制同一套 `RendererTextPort`，不靠截图目测）。

### 实测数据（字号 15 / Monospace / 浅色）

| spacing | 上留白 | 下留白 | 下−上 | 墨迹中心−行盒中心 |
|---|---|---|---|---|
| 1.00 | 3.34 | 2.01 | −1.33 | +0.665 |
| 1.15 | 3.34 | 4.98 | **+1.64** | +0.34 |
| 1.30 | 3.34 | 7.95 | **+4.61** | −0.51 |
| 1.50 | 3.34 | 11.91 | **+8.57** | −1.66 |

**上留白恒 3.34（一个像素不动），增量 100% 堆在下方**；拉丁体同样（上恒 1.57，
下 5.58→15.48）。“下−上”极差 **9.90px**。

### 根因

`TextPort::draw` 的契约是「`origin.y` = 行盒顶，`baseline = origin.y + shaped_ascent`」——
字形**死贴在行盒顶**；而 `CodeEditor::line_height()` 直接把
`port.line_height(size) × line_spacing_` 当行高用，多出来的空白便全落在字形下方
（与 CSS `line-height` 同病）。

### 修法

新增 `st::ui::line_block_offset(port, size, spacing)`：把
`(自然行高×倍数 − 自然行高)` 平分为上下两半，返回**上侧那半**；绘制侧把它加到
行墨迹的 `origin.y`（行号与光标同样跟随）。**行盒仍是原点**——底纹/选择/命中/滚动条
几何全部不动，因此只是局部改动。

为何抽成公共函数而不写在组件里：**量尺必须与实现走同一个入口**。本轮实测踩到了
这个坑——探针里自写一份公式副本时，回退实现后探针**依旧报绿**（典型的假护栏）；
抽成公共函数后逆向验证才真正变红。

### 修后数据

| spacing | 上留白 | 下留白 | 下−上 |
|---|---|---|---|
| 1.00 | 3.34 | 2.01 | −1.33 |
| 1.15 | 4.82 | 3.50 | −1.32 |
| 1.30 | 6.31 | 4.98 | −1.33 |
| 1.50 | 8.29 | 6.96 | −1.33 |

「下−上」极差 **0.01px**、墨迹中心极差 **0.005px**（纯浮点舍入）——
行距只改变**行间**空隙，字形在自己行盒里没挪位。对照表：
`tools/line_box_compare.py tmp_before.txt tmp_after.txt`。

### 验证

| 项 | 结果 |
|---|---|
| 新增 `tests/code_editor_line_spacing_test.cpp` | 2 条（线性/半量；跨档字形位置恒定） |
| 逆向验证 | 回退分摊 → 两条**都变红**；恢复 → 全绿 |
| `st test --test-jobs 4` | **exit=0**（859 用例 / 20995 断言） |
| `st lint` | 0 违规 |
| 真窗口截图 | 逐行上下留白目视均有气（不再贴着行顶） |

### 工艺记录（两处差点交付假护栏）

1. **探针自带公式副本** ⇒ 回退实现后量尺仍报绿。修法：公式抽成 `line_block_offset`，
   探针与组件同调它。
2. **回退代码编译不过**（`-Werror=unused-variable`）⇒ 跑的还是上一份二进制。
   修法：回退写法保留 `(void)var;`（手册 §7.3）。

## 已完成（2026-10-07 测试套随机 SIGSEGV：日志监听器悬垂）

**现象**：`st test --test-jobs N` 每次都会有一个分片以 `-1073741819`（0xC0000005）
异常退出，**崩的是哪片会漂**；`--shard 1/4` 单独跑更是 **100% 复现**（6/6）。

**定位**：mingw 自带 gdb（`gdb -batch -x <脚本>`；注意 `-ex "run --shard 1/4"` 会把参数
当成 gdb 自己的选项，`--shard` 要经 `set args` 传）拿到栈：

```
#5 st::log::write                      src/core/log.cpp:61
#6 st::log::info<...>                  include/st/core/log.hpp:51
#7 compile_units                       src/pkg/build.cpp:900      ← 编译 worker 线程
#8 st::pkg::build                      src/pkg/build.cpp:1389
#9 check_only_writes_no_artifacts      tests/pkg_check_test.cpp:104
#0 Application 的 lambda → impl_->log_lines.push_back   src/app/app.cpp:228
```

**根因**：`log::add_listener` **没有注销入口**，而监听器列表是**进程级全局**的。
`Application` 构造时注册了一个抓 `this` 的回调，**析构却从不摘**。于是：
任何临时 `Application` 都留下一个悬垂监听器；某条用例在测试进程内真调
`st::pkg::build`（`check_only_writes_no_artifacts`）→ 编译 worker 打一条 info →
踩到已经析构的 `Application`。**“崩的是哪片会漂”由此得到解释**：
哪一片先构造过 `Application`，哪一片的后续日志就会踩到野指针。

**为何之前查不出来**：① 崩点看起来在 `check_only_writes_no_artifacts`，单跑却永远绿
（那条用例本身没问题，是**进程里累积的脏状态**）；② 测试跑手只认**第一个位置参数**作为
过滤器，早期“成对/分组跑全过”的结论**全部无效**（实际只跑了组里第一条）。

**修法**（两层）：
- `log`：`add_listener` 返回**订阅 id**（`[[nodiscard]]`）、新增 `remove_listener(id)`；
  `write` 遍历前对订阅表**做快照**（回调里自行退订是常见用法，边遍历边 `erase` 就是 UB）。
- `Application`：把 id 存在 `Impl` 里，**析构首步注销**。

**验证**：

| 项 | 结果 |
|---|---|
| 新增回归 `tests/core_log_test.cpp` | **3 条**（注销后不再收、**另一线程**发日志也收不到、重复注销/回调内自退订不崩） |
| 逆向验证 | 回退 `Application` 的注销 → `--shard 1/4` **3/3 全崩**；恢复后 **4/4 全过** |
| 四个分片 | **首次全部 exit=0**（213/215/215/214 条用例，0 失败） |
| `st test --test-jobs 4` | **exit=0**（此前必有一片丢） |
| `st lint` / mingw 交叉编译 | 0 违规 / 通过 |

**顺带确认**：同类同题只剩 `Application` 一处（`ScriptHost` 的事件观察者已在析构里摘除）；
`dsl::State` 的订阅由 `Composer` 按帧回收，不走这条全局列表。

## 已完成（2026-10-07 菜单“秒退”与点击透传：后端双击多发 Click + 菜单项按下即激活）

用户反馈两句：「菜单按钮点击秒退」「感觉是点击事件透到下方了」。两句都是**真实现象**，
且各自对应一个独立缺陷——在真窗口（`--control-port`，backend=win32）上逐事件复现后定位。

### ① 一次物理双击产生 3 个 `Click`（后端账算错）

**现象**：连点两下菜单标题（或双击），刚打开的面板一闪就没；
单击打开后再点同一个标题也会关掉（toggle 手感本身是对的，错的是“一下”被算成了“两下”）。

**根因**：`platform_win32.cpp` 在 `WM_LBUTTONDBLCLK` 与随后的 `WM_LBUTTONUP` 两处
**各补了一个 `Click`**。而真实的 Win32 双击消息序列是
`LBUTTONDOWN, LBUTTONUP, LBUTTONDBLCLK, LBUTTONUP`——系统用 `DBLCLK` **代替**了
第二次的 `DOWN`（不是额外多一条）。于是：

| 消息 | 旧实现发出 | 应有的 |
|---|---|---|
| 第一个 `UP` | MouseUp + **Click** | 同 |
| `DBLCLK` | DoubleClick + **Click**（多余） | 只 `DoubleClick` |
| 第二个 `UP` | MouseUp + **Click** | 同 |

→ 一次双击 **3 个 `Click`**。对 `Button` 无妨（重复激活只是多跑一次），对 **toggle 语义**
的控件是致命的：菜单栏“点一下开、再点一下关”，连点两下＝开了又关。

**实测数据（真窗口，逐事件采样）**：
`down1=0  up1=0  click1=1  down2=1  dblclk=1  click2=0`——
面板确实是被**第二个 `Click`** 切掉的，而 `DBLCLK` 本身没让它消失。

**修法**：`DBLCLK` 只发 `DoubleClick`（`click_count=2`），**不再补发 `Click`**。
这类“一次手势几次计数”的账必须在后端算谁，不能让每个组件各自用标志位去猜。

### ② 菜单项在 `MouseDown` 就激活（一次点击执行三次、且透到下层）

**根因**：`MenuPanel::on_event` 把 `Click` 与 `MouseDown` 合在同一个 `case` 里。

**实测数据**（新增回归用例先跑，未修时）：一次物理点击（`Down→Up→Click`）
激活 **3 次**（`期望 activated == 1，实际 3`）、关闭通知 **3 次**。

**为何会“透到下方”**：在 `MouseDown` 那一刻就执行了动作**并把面板关掉**，而鼠标还没松开——
随后的 `MouseUp`/`Click` 已经落在**下面那个元素**上，于是“点菜单”会顺手触发下层控件。
用户感受正是“点菜单秒退，还透着点到了下方”。

**修法**：`MouseDown` 只更新高亮/按下态（与 `Button`/`MenuBar` 同一约定），`Click` 才激活。
同一缺陷在 `ContextMenu` 的 dismiss barrier 上也存在，一並修正（面板外也改成“松手后的
`Click` 才关闭”，避免“按住拖一下再松开”被当成关闭）。

### ③ 顺带补齐的两条菜单行为

- **再点已打开的那个标题 = 关闭**（浏览器/VSCode 菜单栏手感）；旧实现在同一个标题上
  重复点击只会一次次 `set_open_index`，“再点一下关掉”做不到。
- **`DoubleClick` 由菜单栏收下、不再冒泡到标题栏**：菜单栏长在标题栏的附属槽里，
  漏出去就是“双击菜单把窗口最大化”（与上一轮修的那条相偿相成）。

### 验证

| 项 | 结果 |
|---|---|
| `tests/ui_menu_test.cpp` | **13 条全绿**（新增 2 条；其中 `menu_panel_item_activates_once_per_physical_click` 做过逆向验证：回退即 `activated 3 != 1`） |
| `tools/gbcode_e2e.py` | **29 组全过**（新增 [23b] 再点标题关闭、[23c] 双击不得关面板） |
| 真窗口（backend=win32） | 真实鼠标：单击开面板 ✓、双击面板仍在 ✓、`titlebar.maximized=false` ✓、点「新建文件」→ 面板关 + 文件对话框开 ✓ |
| `st lint` | 0 违规（扫 352 文件） |
| `st test --profile dev menu --test-jobs 1` | **13 passed / 0 failed / 74 assertions** |
| `st test --profile dev --test-jobs 2`（全量四片） | 分片 2/4 **214 passed / 0 failed / 10340 assertions**；分片 1/4 在 `check_only_writes_no_artifacts` 处 **0xC0000005** 退出（库存问题，见 P1） |
| 四片单跑 | 2/4 = 214 passed / 10340 assertions、3/4 = 214 / 3073、4/4 = 213 / 3427（**全绿**）；1/4 仍崩（**217 条用例逐条累加 = 4137 断言**） |
| mingw 交叉编译 | 通过（本轮改了 `platform_win32.cpp`，87 单元重编 4：`platform_win32.cpp` / `menu.cpp` / `dsl.cpp` / 示例；18.70s） |

**还剩什么**：① `check_only_writes_no_artifacts` 的 0xC0000005 崩溃仍在（P1，改动前就存在，
与本轮无关；**崩的是哪片会漂**——本轮多次观测到分片 1/4，也见过分片 3/4）；
② 后端“`DBLCLK` 不再多补 `Click`”这条**没有自动化替身**：协议 `input.mouse{kind:"dblclick"}`
走的是 `EventKind::DoubleClick` 合成路径（`src/control/server.cpp`），不经过 `WM_LBUTTONDBLCLK`——
该改动靠真窗口实测 + mingw 交叉编译过关。

## 已完成（2026-10-07 gbcode 真可用化：菜单栏不再挂钩窗口 + 五项宣称能力落实）

用户要求：「完善 gbcode 的能力，要真实可用；**菜单栏按钮不要挂钩控制窗口事件
（如双击切换最大化）**」。两句话各自指向一类问题：前者是**功能声称与实际不符**，
后者是**框架的命中契约与文档自相矛盾**。

### ① 菜单栏挂进标题栏＝挂在窗口拖动区上（框架缺陷）

**现象**：双击「文件」菜单，窗口被最大化/还原。

**根因两层，都在框架侧**：
- `TitleBar::hits_caption()` 只排除了**尾部槽**（`trailing`），而菜单栏挂在
  `leading` 槽里——正好落在“从窗口左缘起”的拖动带内。头文件却写着
  “附属槽**不参与**拖动（它们是控件，不是拖动区）”：**注释与实现自相矛盾**。
- 菜单自己消费了按下与单击，唯独**双击**无人认领，于是冒泡到标题栏的
  `DoubleClick` 分支 → `window_toggle_maximize()`。

**修法**：`hits_caption()` = `drag_rect()` **减去** `leading`/`trailing` 各槽的矩形
（逐块挖掉，而不是“只排除尾部”）。

**回归**：`tests/ui_title_bar_test.cpp` 两条（前置/尾部槽各一，均逆向验证：
去掉挖除即 `maximize_calls 1 != 0`）；`tools/gbcode_e2e.py` 第 23 组用**真进程**
钉住“双击菜单栏不改窗口状态、单击仍能开面板”。

### ② 五项「帮助卡写着、代码里没有」的能力

| 声称 | 实际（改前） | 现在 |
|---|---|---|
| Ctrl+D 选中下一处同词 | 按下去**毫无反应** | `CodeEditor::select_next_occurrence()`（单光标模型，环绕；动作面同名） |
| Alt+↑/↓ 上/下移当前行 | 文本一字未动（落进普通方向键分支） | `CodeEditor::move_lines(delta)`（整行搬家，光标随行） |
| Ctrl+G 转到行… | 菜单项其实跳“第一个问题行”；Ctrl+G 无绑定 | 真浮层（行号 / `行:列`）+ 越界**如实报错**（`#goto-input` / `#goto-error` / `#goto-go`） |
| 查找条「全词 / 正则」 | 把词改写成 `\b词\b` 传给**不做正则**的组件 → **一处也搜不到，且不报错** | 全词成为组件一等选项（`FindOptions{.whole_word}`，命中那一刻判边界）；正则**如实置灰**并说明不支持 |
| 帮助卡 F8 / Ctrl+K 等条目 | 与实现逐条核对 | 帮助卡按实际键位重写（含 Ctrl+G） |

**教训（值得单独记）**：“帮助里列着、按下去没反应”属于**可信度**问题——
用户试一次就不再相信界面上任何一条说明。声称的能力要么真做，要么从文案里删掉。

### ③ 顺带挖出并修掉的三个框架缺陷（都是“同一件事两个口径”）

- **浮层的按键先给谁**（`UiRoot::dispatch_key_into`）：原实现**逆序问所有子元素**，
  于是“谁最后声明谁抢键”——浮层形如 `[文本][输入框][跳转][×]` 时，尾部 `Button`
  把本该属于输入框的 Enter 吃掉（`Button` 对 Enter 是 `activate()`）。实测症状：
  在“转到行”输入框里按回车，**浮层被关掉、`on_submit` 根本没跑**；查找条（尾部也有「×」）同理。
  修法：顺序改为 **容器自身 → 焦点所在链 → 其余子元素**。
  回归：`tests/ui_overlay_key_test.cpp`（回退焦点链即红）。
- **裁剪器件的命中没裁**（`UiRoot::hit_test_subtree`）：`clip_children` 只作用在绘制上，
  于是滚动后的长内容“看不见却抢点击”。实测：终端滚回 `Text` 的 bounds 为
  `y=-119 / 高 851`，**点标题栏/菜单栏命中的是它**。修法：命中侧照同一个开关裁，
  `ScrollView` 在构造里声明 `clip_children`（与它 `paint` 里压的裁剪同源）。
  回归：`tests/ui_scroll_clip_hit_test.cpp`（**夹具必须让内容横跨视口边界**——
  初版把它排到完全在视口之外，回退修复后依然绿，是一次假护栏）。
- **浮层内输入框的指针跨帧悬垂**：页面在 `build()` 开头把 `editor` 置空，
  却没置空浮层里的 `Input*`；而“聚焦输入框”的待办排在浮层构建**之前**，
  于是拿上一帧已销毁的指针调 `host()->set_keyboard_focus(...)` ——
  无头下随机 SIGSEGV（0xC0000005）。修法：两个 `Input*` 一并置空 + 待办移到构建之后。

### ④ 示例侧的两处跨平台缺陷（Windows 上原本“跑不通”）

- `--tool-root` 回退只找 `build/bin/st`，而 Windows 上是 `st.exe` → 终端里的 `st test`
  永远报“找不到工具链”。改为两个名字都试。
- `tools/gbcode_e2e.py` 把 `pathlib` 的反斜杠路径与应用的**正斜杠**输出直接 `==`，
  且 `cd /tmp` 写死 POSIX 路径 → 在 Windows 上必红。改为归一化比较 + `tempfile.gettempdir()`。

### 验证

- `tests/ui_title_bar_test.cpp` 25 条、`tests/ui_code_editor_test.cpp` 84 条、
  `tests/ui_overlay_key_test.cpp` 2 条、`tests/ui_scroll_clip_hit_test.cpp` 1 条 —— 全绿；
  新增的 6 条**逐条做过逆向验证**（回退实现即变红，含一次假护栏的修正）；
- `tools/gbcode_e2e.py` **27 组全过**（新增 23~27b 六组，跑真进程）；
- `st lint` 0 违规；`st build --toolchain mingw` 交叉编译通过；
- 截图核验：转到行浮层（含越界报错态）像素级确认。

## 已完成（2026-10-07 标题栏窗口按钮：悬停反馈修复 + 形态对齐系统）

用户要求「标题栏窗口操作按钮再全面优化、美化下」——**观感类任务**，按
`docs/perceptual_changes.md` 走：先把感受拆成正交量、再量、再给候选让用户拍板。

### 量出来的当前态（全部正交量）

| 量 | 实测 | 判定 |
|---|---|---|
| 按钮几何 | 46×40（纵横比 1.15） | 与系统规格一致（按钮 46 宽） |
| 字形墨迹 | 11×11 逻辑，居中偏移 **+0.00/+0.00** | 好，与系统同量级（10px） |
| 笔画粗细 | 2 物理 px | 好 |
| **普通按钮悬停对比度** | **1.0055**（只差 1/255） | **真缺陷**：悬停完全没有反馈 |
| 关闭按钮悬停对比度 | 1.0564 | 弱（“一抹淡红”） |

### ① 普通按钮的悬停反馈结构上不可能达标

根因：旧实现是 `surface_pressed.with_alpha_f(0.10)` **半透明罩层**，而标题栏底色
`surface_alt` 本身就是**不透明**的——alpha 在已不透明的底上不产生任何差异。
关闭按钮之所以看得见，纯粹是因为它用的是 danger 色相（色相变化不吃明度）。

顺带量清了上限：栏底 `#EDEDF2` 已在灰阶浅端，中性色叠上去**最多只能到 1.067**
（主题自己对“看得见”的门槛是 1.35）。即：普通按钮不可能靠中性色达到高对比度——
系统自己也靠**大块面积**而不是高对比度传达 hover。

### ② 按住标题栏会把**整条栏**压暗（顺带挖出来的缺陷）

量按钮按下态时发现 x=400/1200（远离任何按钮处）也从 `#EDEDF2` 变 `#DADADF`——
即按住标题栏拖窗口时整条栏会变暗，看着像窗框闪了一下。两层根因：

1. `TitleBar` 自己没关基类的悬浮/按下回放，而 `pressed_` 会在鼠标按在栏上
   **任何位置**时置位（拖动窗口的第一步就是按在栏上）；
2. 更隐蔽的是——`Element::paint_box` 里那条“按下了但 `hover_t` 尚未起来”的
   回退分支**只查了 `hover_effect_.background`、没查 `enabled`**，
   于是即使组件显式 `set_hover_effect({.enabled=false})`，它**依旧**会把底色压暗。
   逆向验证：只做第 1 条时该用例仍然变红（48542 px 差异），补上第 2 条才真生效。

### 候选与用户决策

出了 9 档各只改一个正交量的对照图（`tools/title_bar_buttons_probe.cpp`），
用户选 **F（纵向内缩块）+ G（关闭实心红）**。

### 落地

| 项 | 改前 | 改后 |
|---|---|---|
| 普通按钮悬停对比度 | 1.0055 | **1.0672**（复现路径：像素 1.1583） |
| 普通按钮按下对比度 | — | 1.3831 |
| 关闭按钮悬停对比度 | 1.0564 | **2.1066**（像素 4.0803） |
| 悬停块形状 | 全出血（与栏同高） | **92×64 物理 = 46×32 逻辑**（纵向缩 4） |
| 按住标题栏 | 整条栏变暗 | **0 px 差异** |

### 验证汇总

- `st test` **788 全绿**（+3 新用例）/ `st lint` 0 违规 / mingw 交叉编译通过；
- 三条新回归均做过**逆向验证**（回退修复必须变红）；
- 新增工具 `tools/title_bar_buttons_probe.cpp`（候选对照图 + 逐步度量），
  以后改窗口按钮只需重跑它，不必再手搓对照。

## 已完成（2026-10-07 焦点环 / 运行期 DPI / 粗体整形缓存 / SemiBold 真粗体面）

用户报三件事：「按钮聚焦不要再嵌套一层边框」、「画廊示例的 DPI 缩放有问题」、
「画廊首页的大黑字体边缘不平滑」。量下来是**四个独立缺陷 + 一个遗留**。

### ① 焦点环与控件描边是两道并排的带子（用户看到的那件事）

`paint_focus_ring` 从控件外沿 `R` **往外**画环，而控件自己的 border 占 `[R-bw, R]`——
两条相邻但不同色的带子：一道灰框 + 外一圈蓝框。
而 `Button` 是**第三种**写法（环在内部缩进 2px）：不透明底上看着像“按钮里又有个小框”，
`Secondary` 则同时看到外框 / 2px 空底 / 内环**三层**。

修法：环的起点从 `R` 移到 `R-bw`（环完整盖住描边），环宽取
`max(focus_width, border_width)`；按钮改走同一个助手。带描边的控件看到**一道加粗的环**，
无描边的看到**一道完整外环**（位置与以前一致）——两种形态都只有一道的宽度。

回归：`tests/ui_button_center_test.cpp` 的 `button_focus_ring_covers_its_own_border`，
判据是**连通段宽度**（沿水平中线扫出相对未聚焦帧变化的像素段）：
环带不得跨越 `R` 往外多长、不得宽过 `ceil(max(focus,border))+1`、且必须盖到 `R`。
逆向验证：把助手回退成“从 `R` 外扩” ⇒ 环带变成 `[R+1, R+2]`，三条断言全红。

### ② 运行期切 DPI 不同步字形超采样（同一 DPI 两种渲染）

`Application::set_device_scale` 只改了缓冲尺寸，而字形位图密度由 `TextRenderer` 的
`supersample`（构造时取解析出的 DPI）决定。于是“启动即 2.0”（超采样 2）与
“启动 1.0 再切到 2.0”（停在 1）**渲染结果不同**——后者大号字号的斜向/弧形边缘
出现可见阶梯（实测同区域 1524 个像素差 >32）。

修：`set_device_scale` 一并 `set_supersample(scale)`（后者同时清字形缓存）。

回归：`tests/text_glyph_integrity_test.cpp` 的 `app_runtime_dpi_switch_matches_startup_dpi`
走真实 `Application` 链路，要求两条到达路径**逐像素相同**。
逆向验证：去掉那行 ⇒ 6472 像素不同，断言变红。

> 配套：`text_supersample_change_rebuilds_glyph_bitmaps` 钉住“切超采样必须改变像素”
> 这一层（单测粒度）。它**单独不足以**防止这个缺陷——接线在 `Application` 里，
> 所以上面那条端到端用例才是真护栏。

### ③ 整形缓存键遗漏 `bold`：真粗体字体面**从未生效**（大黑字不平滑的真因）

用户报「画廊首页的大黑字体边缘不平滑」。量下来（灰度口径与 Chrome 雅黑 700 同字号对齐，
物理 39px「概览」）：

| 口径 | 霜天（修前） | Chrome 700 | Chrome 400 | 霜天（修后） |
|---|---|---|---|---|
| Σ墨（覆盖率之和） | 1089 | 1654 | 1049 | **1684** |
| 实心像素/墨 | 0.558 | 0.698 | 0.521 | **0.654** |
| 过渡像素/墨 | 0.337 | 0.254 | 0.329 | **0.302** |

修前的数**几乎等于 Chrome 400（常规）**——因为 `Heading` 的 Bold 档拿到的是常规面的字形。
真因是 `shape_cached` 的键：`fnv1a64(utf8) ^ size_key<<32 ^ role<<56 ^ fingerprint`——
**没有 `bold`**。而 `bold` 决定的是用哪张字体面，`ShapedText::runs` 里存的正是
`(face, glyph id)`。布局先跑的 `measure_width()`（无 `bold` 参数）把**常规面**的整形
结果写进缓存，绘制再用 `bold=true` 取就命中同一条。

这不只是“不变粗”：`(face, glyph)` 跨面混搭时取到的是另一张表的位图，
即**字被换成另一个**（宽度多半相同，是那种最难查的“合法但错误的字”）。

修后上表的三个正交量同时朝参照靠（墨量、实心率、过渡带），且有跨字体的直接证据：
把 `ST_FONT_CJK_BOLD` 指向 `simhei.ttf`，修前**逐像素零差异**（说明该变量根本没被走到），
修后标题与统计卡两处都变。

回归：`text_shape_cache_distinguishes_bold`（先 `measure_width` 再 `shape(bold=true)`，
要求 runs 的 `(face, glyph)` 与常规档至少有一处不同）。
逆向验证：把 `bold` 从键里去掉 ⇒ 断言 `differs` 变红。

### ④ SemiBold 走合成加粗，与浏览器不一致（用户第二轮反馈「还是不行」）

第一轮修完 ③ 后用户仍说不行、并给了浏览器截图。以浏览器为基准逐像素对照，
找到第二层差距：**浏览器的 `font-weight:600` 用的是真粗体面，而霜天 SemiBold 走合成加粗**。

浏览器侧实测（26px，物理 39px，同一字体）：

| 字重 | 400 | 500 | 600 | 700 | 800 |
|---|---|---|---|---|---|
| 字面宽 | 155 | 155 | **168** | **168** | 168 |
| Σ墨 | 1169 | 1169 | 1879 | 1879 | 1879 |

即浏览器把 600/700/800 **吸到同一张 Bold 面**（逐像素相同），与 400/500 明显不同。
而霜天：`prefers_real_bold` 只认 `Bold`，SemiBold 走合成加粗——合成加粗只加墨、
**不改字面宽**（247 vs 参照 258），所以字看着“胖而糊”（过渡带宽、实心率仍低）。

三路对照（26px「GPU (D3D11)」）：

| 口径 | 字面宽 | Σ墨 | 实心/墨 | 过渡/墨 |
|---|---|---|---|---|
| 浏览器 600（参照） | **258** | 3222 | 0.731 | 0.228 |
| 霜天 SemiBold·合成加粗（修前） | 247 | 2800 | 0.688 | 0.250 |
| 霜天 SemiBold·真粗体面（修后） | **258** | 3282 | 0.696 | 0.254 |

修：`prefers_real_bold` 把 `SemiBold` 也纳入（与 `Bold` 同一条路）。
卡片标题（15px）同步对齐：Σ墨 1347 vs 参照 1349（修前 1162）。

回归：`ui_semibold_uses_the_real_bold_face`（契约层断言 `prefers_real_bold` 三档的归属
+ 像素层断言 SemiBold 确实比 Regular 重）。
逆向验证：把 `SemiBold` 从 `prefers_real_bold` 里去掉 ⇒ 断言变红、且墨量从 2983 降到 2411。

> **这一轮的方法教训**：第一轮只看“墨量/实心率/过渡带”三个聚合量，
> 它们确实朝参照靠了，但**漏了“字面宽”这个正交量**——而那正是用户一眼看出
> “不对”的那个量。聚合量对不上≠对上了；下次拿参照时，先把参照侧的**字面宽/字间距**
> 也量一遍。

### 遗留（已记下方 P1）：粗体文本按**常规面**量宽

修 ③ 之后拓出来的一条：`TextPort::measure_width` 的接口面**没有 `bold`**，
所以布局按常规面量宽、绘制按粗体面画。实测 15px 中英混排「概览 Bold混合ABC」：
`measure(bold=false)=126.973` vs `shape(bold=true).width=130.620`，**差 3.65px**。
影响：粗体文本的居中/换行/省略号位置有系统性偏差（小，但系统性）。
修法要动 `TextPort` 接口面（新增带字重的度量入口 + 各实现跟进），
归为下一步而不是本轮顺手改。

### 验证汇总

- `st test` **785 全绿**（+4 新用例）/ `st lint` 0 违规 / mingw 交叉编译通过 /
  `check_docs.py` OK（含用例数/断言数同步）；
- 四条新回归均做过**逆向验证**（回退修复必须变红）——`copy2` 恢复会搬回旧 mtime，
  回退后必须 `st build --force` 重编，否则跑的是上一份二进制（本轮重现过一次）。

## 已完成（2026-10-06 内置图标：圆头描边 + 光学归一化 + `restore` 几何）

用户报「这个切换窗口大小的按钮不对吧」（标题栏的“还原”图标），并要求「丰富和美化内置的图标」。
前者是**真缺陷**，后者拆成两个正交量。

### ① `restore` 的后窗伸进了前窗内部（用户直接看到的）

设计数据写成 `M4 4 L16 4 L16 16`——后窗右边缘一路画到 `y=16`，
而那已经在前窗（`8,8`–`20,20`）**内部**，于是多出一截“插进窗里”的竖线。
正确收尾点是后窗右边缘与前窗上缘的交点 `y=8`：
`M16 8 L16 4 L4 4 L4 16 L8 16 M8 8 L20 8 L20 20 L8 20 Z`。

> **连带拓出一条“因缺陷而绿”的护栏**：`restore_icon_differs_from_maximize_icon`
> 比的是两个路径的 `flattened_bounds` 尺寸不等——而它过去能通过，
> **纯粹是因为那个伸出去的 bug 把包围盒撞大了**。修好几何后两者尺寸一致，它立刻变红。
> 已改成量**结构差**（子路径数 2 vs 1）+ 新增一条量**像素差**（185/1024 个像素不同），
> 两条都盯“看得不一样”这件事本身。

### ② 圆头端帽：零高度圆点从“画不出来”到真的可见

`Icon::draw` 原来用 raster 缺省的 `butt/miter`。`more-horizontal`/`more-vertical`/`list`
用 `M6 12 L6.02 12` 这类 **0.02 长**的线段当圆点，butt 下带外那半个圆被切掉，
残留覆盖率只有 **0.086**——接触印相实测这两格**墨迹完全为空**（“三点菜单”图标什么都没有）。
改 `LineCap::Round` + `LineJoin::Round` 后圆点显现，**73 格空格清零**，
转角也从尖角变圆润。

### ③ 光学归一化尺寸：一排图标“看着一样大”

`Icon::path` 改为按每个图标**自己的墨迹范围**等比缩放并居中到统一尺寸
（`kOpticalTargetPx = 17`，夹取 `[0.7, 1.9]`）。

| 口径 | 前 | 后 |
|---|---|---|
| 尺寸极差 | 0.354 | **0.208** |
| 尺寸 CV | 0.125 | **0.055** |
| 最大/最小 | 2.2× | **1.04×** |

（口径：每个图标在 96px 格里的墨迹包围盒最长边 ÷ 格宽；73 个图标全量）

### 量法与判断上的四个坑（都实际踩了）

1. **手抄图标名清单归因**——把 `restore` 记成了 `square`、凭空多出一个 `key2`，
   **归因直接错了**。改为让 `tools/icon_sheet_probe.cpp` 同时输出格位索引 JSON，
   名字与格位只有一个来源。
2. **A/B 对比用了同一个二进制**（两边都含归一化）——量出来“前=后”，对比完全无效。
3. **只放大不缩小**——大字形没被归一，尺寸比只从 1.9 降到 1.35（只修一半）。
4. **“有非底色像素”≠“看得见是点”**——第一版圆点用例用 `pixel != background` 计数，
   而 butt 下那 0.086 覆盖率也满足，**回退圆头依旧全绿**。改成“覆盖率 > 25% 才算一笔”后，
   回退即报**横向 0 段**。

### 新增工具与回归

- `tools/icon_sheet_probe.cpp`：图标接触印相（全部图标一张大图 + 格位索引 JSON），
  用于“把图形与名字并排看”——这类问题（方向、共形、缺墨）只有并排才看得出。
- `tests/ui_icon_path_test.cpp`：原 8 条改为**相对关系**判据（方向/次序/包含/跨度比），
  新增 `ui_icon_optical_size_is_consistent`、`ui_icon_restore_back_square_stays_outside_the_front`、
  `ui_icon_dot_style_glyphs_actually_render`（共 **11 条**）。
- `tests/ui_title_bar_test.cpp`：改正“因缺陷而绿”的那条 + 新增像素差用例。
- 全套件 **818 passed / 0 failed**；`st lint` 0 违规；mingw 交叉编译通过。
- 逆向验证：回退 `restore` 几何 → 红；回退归一化 → 红；回退圆头 → 红（三条均已验）。

### 遗留：`restore` 与 `square` 在 14px 下仍可能看不够清

归一化后两者笔画粗细一致、只差一个叠框。实测 32px 下有 185/1024 像素不同
（差距存在），但标题栏的图标只有 14px。若用户仍觉得分不消，
可把 `restore` 的描边压到 1.3（视觉重量 +23%）——**那是产品决策，待定**。

## 已完成（2026-10-06 按钮居中 / 悬浮上移 / 画廊图标按钮）

用户报两件事：「按钮文本没有居中」、「按钮悬浮时不要上移」，另加一条需求：
「画廊示例增加图标按钮、图标文字按钮，内容都要居中」。量下来是**三个独立缺陷**。

### ① 墨迹中心公式多减了一个 `below`（真因就是“没居中”）

`centered_line_top` 写的是 `shaped_ascent - (above + below)/2`，而 `InkMetrics` 的语义是
「基线上方 `above`、下方 `below`」——墨迹跨 `[基线-above, 基线+below]`，
**中心在基线下方 `(below-above)/2`**（不是上方）。两者差一个 `below`，
于是文字被**系统性压下 `below`**。

实测（真字体，`tools/button_center_probe.cpp`，字号 14；口径 = 墨迹包围盒中心 − 框中心）：

| 用例 | `below` | 修正前 | 修正后 |
|---|---|---|---|
| 汉字 | 1.91 | **+2.000** | **+0.000** |
| 混排 | 1.84 | **+2.000** | **+0.000** |
| 拉丁 | 0.18 | +0.500 | +0.500 |

真机（画廊 28 个按钮，暗色 @2x）：纵向偏移中位 **+1.23 → +0.02**，极差 2.02 → 1.02。

> **旧测试为何一直钉不住它**：旧的墨迹桩把 `below` 恒报 **0**，而 `below == 0` 时
> 两种写法**恰好相等**——桩把差异抹平了（`CONVENTIONS` §7.2 的同类坑）。
> 本次是**逆向验证**当场把它拓出来的：回退修复后用例仍然是绿的。
> 把桩的 `below` 改成真实量级（0.136em）后，回退即三条用例全红（dy=+2.000）。

### ② 按钮悬浮上浮 1.5px（用户直接看到的那件事）

`Button::apply_theme` 开的是 `HoverEffect{.lift = true}`，而 `paint_box` 在 `lift` 时
把**整块**（含文字与图标）上移 `metrics.hover_lift`。实测（画布墨迹包围盒，
Secondary 变体、让动画走完）：静止与悬浮的整块中心差 **−1.500px**，恰好等于	
该令牌默认值。

修：按钮改 `.lift = false`（悬浮反馈仍由 `background` + `border` 承担）；
`lift` 留给卡片这类“浮起来”的容器。

同时补了一条**对偶用例**：只断言“不动”很容易用“什么都不做”蒙混过关，
所以另有一条断言悬浮底色**确实变了**（实测静止 `#FAFAFC` → 悬浮 `#E3E3E5`）。

### ③ 图标名画不出来却预留了空间（→ 文字右偏 11.9px）

画廊把 `btn-refresh` 的图标写成 `activity`——**内置 73 个图标里没有这个名字**
（正确的是 `refresh`）。`Icon::draw` 静默不画，而 `measure`/`paint_content` 仍按
`icon_size + gap` 预留空位，于是文字被推到一侧：
真机实测**右偏 11.88px**（单元口径 +12.000）。

这不只是画廊写错名字——**框架层面**也该拦住：判据用 `Icon::has`（与 `Icon::draw`
内部同一查找），`measure` 与 `paint_content` 共用；图标名无效时不预留、也不计入宽度。
（顺带修了 `measure` 的一个相关漏接：纯图标按钮的宽度旧写法多算了一个 `space_sm`。）

### 新增：画廊的图标按钮 / 图标+文字按钮

- 纯图标按钮 6 个（`refresh/settings/search/edit/trash/send`，三种变体）；
- 图标+文字按钮 5 个（`plus 新建` / `upload 上传` / `download 导出` /
  `refresh 刷新` / `trash 删除`）。

真机终验：新增后横向极差 **12.38 → 2.00**、纵向中位 **+1.23 → +0.02**。

### 量法上的三个坑（都实际踩了）

1. **区域 `capture` 与全帧裁剪量同一按钮差了 10px**（+0.75 vs +10.50）。
   那是**量尺自身的缺陷**，不是被测对象——差点去修一个不存在的问题。
   最终改为不经过截图（桩端口 / 真字体直接渲染到画布量像素）。
2. **桩的 `draw` 收到的是行盒顶、不是基线**。当成基线用会让墨迹整体上移一个
   `shaped_ascent`——差值 +0.5 被量成 −14，误导到错误方向。
3. **底稿与主题不符时整个视口都会被当成墨**（浅色主题下拿黑色当底）。
   而且 `UiRoot` 拓的是 `surface` 而不是 `colors().bg`——
   最终改为**就地取角点像素**自校准，不从主题变量推。

### 回归与验证

- 新增 `tests/ui_button_center_test.cpp` 7 条（无图标居中 / 图标+文字居中 /
  未知图标仍居中 / 悬浮不上移 / 悬浮仍有反馈 / …）；
- 新增 `tools/button_center_probe.cpp`（真字体口径，桩量不到的维度）；
- 三条修复均已纳入 `tools/theme_reverse_verify.py`（共 21 条，全通过）；
- 全套件 **815 passed / 0 failed**（净 +5）；`st lint` 0 违规；mingw 交叉编译通过。

## 已完成（2026-10-06 三个既有红项：全量崩溃 / 文字补偿闸门 / GPU 边界丢行）

上轮收尾时如实记下三个「与本次改动无关」的红/崩，本轮逐个修完。**全量 `st test`
从 807 passed / 2 failed + 中途 abort 变为 809 passed / 0 failed**。

### ① 全量 `st test` 跑到 capture 用例崩在 nlohmann 断言（P0）

`tests/control_protocol_test.cpp:595` 的 `capture_pixel_size_comes_from_the_export_…`
单跑即崩。两个缺陷叠在一起：

- **用例硬编码 `/tmp/gebai-capture-size-probe.png`**。捕获落盘有白名单（默认 = 临时目录
  + 可执行文件目录 + 控制文件目录），`/tmp/...` 在 Linux 能过、**在 Windows 不是白名单**，
  `capture` 如实例返回错误帧。修：改用 `st::fs::make_temp_dir()` + `st::fs::join`
  （这正是仓库里其他用例已有的写法，我没照抄）。
- **`reply["result"]` 在 const Json 上取缺键是 nlohmann 的 UB**——它会直接 abort 进程，
  而不是报“一条用例失败”。于是错误帧一出现，整个测试进程就没了：
  后面所有用例不再执行，且崩溃点看起来与真因毫无关系（这也是
  `tools/theme_reverse_verify.py` 把它误判为“仍绿”的原因，见下）。
  修：取值一律走 `find` + 缺键时报断言失败。同一教训在提交 `92abd9c` 里已写过（§7.1），
  这个文件当时没改到。

**逆向验证**：把实现改回「`region × scale`」→ 用例变红（不再是崩进程）。

### ② `text_ink_compensation_evens_out_glyph_weight` 恒红：闸门钉错了档位

零构退化——**用例的档位与产品默认脱节**：

| | 拟合档位 | 逐字墨量极差（无补尝→有补尝） | 结果 |
|---|---|---|---|
| 用例写死的（`Normal`） | `normal` | 0.265 → 0.224（收窄 16%） | 卡在 ≥20% 阈值上恒红 |
| **应用默认（`Off`）** | `off` | 1.000 → 1.000 | 拟合不扰乱，契约无适用场景 |
| 应用曾经的默认（`light`） | `light` | **0.169 → 0.115（收窄 32%）** | 稳过阈值 |

即：补尝本身好着，而用例拿一个**已在产品里被放弃的激进档位**去卡它。
`app.cpp:56` 写得很清楚：默认已改为 `off`（以真窗口浏览器为基准逐轴标定，
拟合偏锐、三个正交轴都指向 off）。

修（按仓库既有纪律“默认档要抽成可测常量、实现与测试同一入口”）：

- 新增 `st::app::resolve_ink_compensation(fit)`——**拟合关则不补尝**（拟合不扰乱墨量时无事可做）；
  `app.cpp` 由无条件 `set_ink_compensation(true)` 改为走它；
- 用例改为 `resolve_text_fit("auto")` + `resolve_ink_compensation(...)` 取默认档，
  默认档为 `off` 时**如实跳过**（并断言“拟合关 ⇒ 补尝也关”的成对约定）；
- 新增 `ST_TEST_FIT` 环境变量**覆盖档位**——否则默认档关掉后，这条闸门永远跑不到断言，
  就变成一条无人验证的摆设。`ST_TEST_FIT=light st test text_ink_compensation` 走完整断言。

**逆向验证**（`ST_TEST_FIT=light`）：把补尝计算短路 → 极差 0.169 → 0.169，用例变红。
`app.hpp` 里那句错误的注释（“默认 `Normal`”）已一并改正——实现改了、注释不改
正是默认档静默漂移的温床。

### ③ `gpu_matches_software_for_layout_of_real_widgets` 结构性差异超阈

现象：结构性 0.307%（阈值 0.1%）、最大 Δ95。`tools/gpu_structural_probe.cpp`（新）
按行统计把它定位到**两整行**：y=124 共 350 像素、y=75 共 18 像素——
分别落在 `Input`（底边 y=124.3）与 `Switch`（底边 y=78.3）的**边界那一行**上。

**根因**：GPU 顶点着色器画的四边形**刚好贴住矩形边界**，而光栅化**按像素中心**
判图元内外——底边在 124.3 时，第 124 行的中心 124.5 落在四边形外，
**这一整行不产生片元**，SDF 再准也来不及（它本应给出 ~70% 覆盖率）。
软件光栅器对像素做面积积分，不会丢这一行。

修：顶点四边形**向外扩 1 像素**（`vs_main` 的 `pad`）。`local`/`pixel` 都是 `uv` 的
仿射函数，扩张只加余量、同片元处取值不变（梯度与位图采样不受影响）。

效果：**结构性 0.307% → 0.000%**，最大 Δ 95 → 30（纯边缘抗锯齿级）。

**逆向验证**：`pad` 改回 0 → 结构性回到 0.307%、用例变红。

### 同时修的两个基础设施缺陷（都是“让下次不用再猜”）

- `tools/theme_reverse_verify.py`：① 把「进程撞断言崩溃」误判成「仍绿」
  （旧口径只看有没有 `FAIL`）——实测就因此把上述 ① 误指为嫌疑人；
  ② Windows 上 `text=True` 的 GBK 解码崩（后台读线程吞异常 ⇒ `stdout` 变 `None`）；
  ③ 恢复阶段的 `os.remove` 失败会抩出 `finally`，**把源码永久留在回退状态**
  （实测把 `server.cpp`/`rasterizer.cpp` 各弄挂一次）——改为容错 + 恢复后校验原文。
- `tools/weight_spread_probe.cpp`：从未注册进 `st.pkg`，因此**一直没编译过**
  （两个 `-Werror=conversion` 错误）；已注册并修好。没有它就无法快速区分
  “拟合哪个档位扰得少”，上面 ② 的定档依据就无处复核。

### 新增工具

- `tools/ink_spread_probe.cpp`：逐字 dump（基准 / 拟合 / 拟合+补尝 / 补尝率），
  用来判“残差是补尝的固有边界还是实现退化”；
- `tools/gpu_structural_probe.cpp`：GPU/软件结构性差异的**空间分布**（按行统计 +
  最差像素坐标与两侧颜色 + 元素包围盒），用来把“哪一类内容对不上”变成可指认的对象。

### 逆向验证又拓出一个错归因（已修）

加上上面三条后跑 `theme_reverse_verify.py`，报了“回退后仍然是绿的”：
**`border_ring_total_ink_matches_perimeter_times_width` 其实量不到 `rasterize_mask`**。
它走的是 `Canvas::fill_path` → `detail::fill_path_aa` → `blend_coverage_runs`（缺陷①），
而 `rasterize_mask`（缺陷②）是**另一条通路**——我当初把二者当成同一条的两种口径，归错了。
真正走遮罩的入口是**路径裁剪**（`push_clip_path`）：

- 新增 `clip_mask_coverage_accumulates_instead_of_maxing`：用**圆角环**作裁剪路径，
  在全屏填色后量几何面积。为何是环而不是实心块：实心块只有边缘一圈受影响，
  取最大值/累加的差只有 0.18%（实测 0.9982 vs 0.9997）——**太小，抓不住**；
  而环让每个像素都是边界像素（实测 **0.9178 vs 1.0009**）。
  这恰好就是用户看到的那个形状。
- 这是“**回退验证必须跑一遍**”的又一例证：不跑这一步，这个错归因会静静躺在台账里，
  而证据链指向一个根本不测该缺陷的用例。
- 注释里还留下一处踩过的坑：拿“同外框的直角矩形”当圆角那份的参照会把真实面积比
  读成 0.974（那 2.6% 全是四角被切的几何，与覆盖率无关）；现在改为对**解析面积**，
  直角框只充当量尺自检。

## 已完成（2026-10-06 圆角边框：两处覆盖率缺陷 → “圆角比直边细”）

用户现象：“圆角是不是要稍微加厚点，看着比直线细一点”。**量化成立**，且不是“该加粗”，
而是光栅化缺陷。按观感手册先建尺（沿法向积分墨量 = 几何厚度），再逐项二分。

### 两处独立成因（各自都可单独造成该现象）

**① `Canvas::blend_coverage_runs` 逐 run 混合覆盖率**（软件路径 + GPU 合成）

覆盖率是**带符号权重之和**，而像素的最终不透明度是 `1 - Π(1-本像素各段覆盖率)`——
不是“逐段各混一次”的线性叠加。逐段混合时每段做一次 `alpha → u8 → 回浮点`，
在**斜边**上同一像素被多段各碰一点，量化零头每段都差一点，累加后形成系统性欠墨；
而水平/垂直边上每像素只被一段完整覆盖，**恰好不差**——这正是“直边对、圆弧细”的指纹。

实测（`tools/ring_raster_probe.cpp`，1 逻辑px 宽、60 逻辑px 长的带，scale=2）：

| 角度 | 0°/90° | 15° | 30° | **45°** | 60° | 75° |
|---|---|---|---|---|---|---|
| 面积比 | 1.0000 | 0.8817 | 0.8713 | **0.8568** | 0.9060 | 0.9462 |

改为“先逐像素累加覆盖率、同一像素只混一次”后，七档全部回到 **1.0000**。

**② `detail::rasterize_mask` 逐段取最大值**（GPU 路径遮罩）

同一个量在掩码光栅化里又错了一遍：那里用 `if (value > row[x]) row[x] = value;`
（逐段取最大）而不是累加。单轮廓的覆盖率本就该是各段之和，取最大在轴对齐边上
恰好正确、在弧/斜边上系统性偏低。

修①后端到端仍是 0.86，software 渲染器却回到 1.0025——**“两套后端数字不同”这一步
才把②定位出来**（只量一种后端会直接误判为“已修好”）。两处都修后：

| 后端 | scale 1.0 | 1.5 | 2.0 |
|---|---|---|---|
| GPU | 0.9937 | 1.0063 | 1.0009 |
| 软件 | 1.0013 | 1.0054 | 1.0023 |

（口径：圆弧法向墨量 / 直壁法向墨量；修复前 0.728 / 0.828 / 0.862）

### 量法上的三个坑（都实际踩了）

1. **在像素边界上做双线性采样**会把直壁读数从 1.000 量成 0.822（采样点落在两像素交界）；
   改用“沿法向积分墨量 / 满覆盖差”，直壁自检立刻回到精确的 1.000。
2. **覆盖率的底色歧义**：圆弧跨过卡片边界，一半覆盖率落在卡外（底色更深）——
   用“卡内底色”当基线会把那半边裁成 0，凭空造出“弧偏淡”。
   解法：**开/关边框两次截图取差**（`(on−off)/(border−off)`），逐像素精确、无底色假设。
3. **归一化口径**：四角 `r×r` 方框里大半是空的，拿“框内墨量/框面积”除出来天生偏低
   （实测这个错误量法给出 0.9602，而逐像素真值 1.00）。最终用不依赖实现的口径：
   **环总墨量 = 中心线周长 × 宽度**（注意用**中心线**而非外圈周长：外角弧更长，会系统性高估）。

### 回归

- `diagonal_band_ink_is_angle_independent`（斜带面积守恒，七档 ±1%）
- `border_ring_total_ink_matches_perimeter_times_width`（环总墨量 = 中心线周长×宽）
- 两条都已纳入 `tools/theme_reverse_verify.py`；**逆向验证：回退任一修复即变红**

### 同机发现的既有红项（已在本轮后续会话逐个修完）

`text_ink_compensation_evens_out_glyph_weight` 与
`gpu_matches_software_for_layout_of_real_widgets`（结构性 0.307% vs 阈值 0.1%）——
两条在基线工作树上同样红。**两者已于下一个会话修完，见本文件顶部「三个既有红项」那条**。

## 已完成（2026-10-06 P0：全量 `st test` 在 capture 类用例后崩在 nlohmann 断言）

### 现象

`st test`（无 filter，dev / debug 两档）跑到 **capture 协议用例之后**进程 abort：

```
Assertion failed: it != m_data.m_value.object->end(),
  file third_party/nlohmann/json.hpp, line 22188
```

### 已排除

- **与当时的圆角环修复无关**：把 `src/raster/path.cpp` 回退后重编再跑，同样崩在同一断言；
- 崩点是**制品侧**（`third_party/nlohmann/json.hpp`）而不是被测代码：断言的含义是
  「用 `at()`/`operator[]` 撞了一个不存在的 object 键」。

### 为什么它一直没被挡住

崩在**用例中途**（不是跑完报红），于是：

- `st test <单个过滤器>` 单独跑那几条 capture 用例**全绿**；
- 默认路径只看到「前面的用例都 PASS，然后进程没了」，
  退出码不落进任何一条汇总结论里；
- `tools/theme_reverse_verify.py` 也不受影响（它每次只跑一个指名用例）。

### 实测触发条件

| 顺序 | 结果 |
|---|---|
| 单跑 `st test capture` | 全绿 |
| 单跑 `st test control_protocol` | 未观察到崩 |
| 单跑 `st test capture_pixel_size_comes_from_the_export_not_the_request` | **崩** |
| `st test`（全量、无 filter） | 必崩（重复 3 次一致） |

**已定位并修好**：两个缺陷叠在一起（硬编码 `/tmp/` 路径 + `const Json` 取缺键是 UB），
详见本文件顶部「三个既有红项」那条。全量现已 809 passed / 0 failed。

### 一处配套坑（已修）

`tools/theme_reverse_verify.py` 原本只看输出里有没有 `FAIL`；进程**撞断言退出**时
既无 PASS 也无 FAIL，于是被报成「回退后仍绿」——实测就因此把这条**原版也崩**的
用例误指为嫌疑人（白查一轮）。

## 已完成（2026-10-06 P0：`st test` 偶发段错误 + DSL 单根契约静默破坏）

按“继续优化框架”推进清单时，在第一条上就撞到一个比清单里所有项都严重的**既有 P0**。

### 现象

干净工作树上 `st test` **偶发必然崩**（退出码 `0xC0000005`）：

- 每次“强制重编后紧接跑”的第 1 次崩，第 2 次通过（dev / debug 两档都是）；
- 崩点在 `dsl_box_props_axis_padding_reaches_layout`；
- gdb 现场：`bar` 非空但调用虚函数跳到**地址 0**——典型 vptr 已清 = **悬垂指针**。

### 真因：DSL 单根契约被静默破坏

那个测试的 `build()` 里写了**两个顶层元素**：

```cpp
axis_bar  = &row(c, {.id = "axis"},  [...]);
mixed_bar = &row(c, {.id = "mixed"}, [...]);   // ← 第二个会 set_content() 顶掉并析构第一个
```

而 DSL 是**单根**契约：第二个顶层声明会把第一个 `set_content()` 替掉（**连同析构**）——
测试保存的 `axis_bar` 当场悬垂，下一行访问就是地址 0。

**为何偶发**：只在“全量重编”后触发，增量不触发。第一与第二层的完整边界未完全定位
（现象可稳定复现，但归因需另一次专项排查），**已记入 BACKLOG 待查**——
不是“修好了就不用管”的那种。

### 修法（两处）

1. **契约破坏必须报错**（框架）：根层第二个声明写 `ReconcileStats::error` +
   stderr 诊断（“一个 build() 里声明了多个顶层元素…”），而不是静默销毁。
   区分“非法多根”与“合法换页（类型变了重建根）”只看一件事：本帧是否已经落过根元素。
2. **测试改成单根**（`column` 包住两根行）。

### ⚠ 一次走错方向的自查（最重要的教训）

我一度定位到“腾位分支把**无 key** 元素当残留释放”，写了修法 + 回归用例。
但**逆向验证把它掉了**：回退那条修法后，全量 **755 仍全绿、也不再崩**——
因为兄弟元素是**同类型**时走的是“复用”分支，压根不进腾位分支（那条用例对旧实现也绿）。
最后把那处修法**回退了**，只保留被证实的单根诊断。

这在提示词里早就写着（“新写的回归测试必须验证它真能抓住那个缺陷”），而本次差点就交付了一个
改错方向的“修复”——**逆向验证那一步不能省（也不能省真）。**

### 验证

- 修复后按触发条件**连跑 5 次**（每次先 `--force` 全量重编）：**5/5 全绿**（修前 2/2 首跑崩）；
- 新增两条回归：`dsl_multi_root_declaration_is_diagnosed`（**逆向验证：回退诊断后当场变红**）、
  `dsl_keyless_siblings_are_not_recycled_as_stale`（已观测行为护栏，注释里说明它**没**钉住缺陷）；
- `st test` **755 全绿** / `st lint` 0 违规 / `check_docs.py` OK / mingw 交叉编译通过 /
  gbcode E2E 与 gallery E2E 全通过。

### 待查（已记 BACKLOG）

崩溃的**触发条件**（为何只在全量重编后的首跑）：真因已消除，但那个条件里可能还埋着
别的东西（如首次全量构建后的某个缓存/顺序差异）。下次碰性能或测试基建时优先看。

## 已完成（2026-10-06 测试默认路径：慢/环境敏感用例门控）

接上一条（`expand_glob` 前缀下钻）。修完构建前置成本后继续量“AI 一轮迭代”
真实花在哪，结论出人意料：**编译不是大头，测试执行是**。

### 量到的事实

| 量 | 值 |
|---|---|
| `st test`（无改动） | **37.9s** |
| 其中直接跑已编好的 `st_tests.exe` | **31.8s** |
| 改一个 UI 头 → `st test` | **80.1s** |

测试套件 765 例共 34.6s，其中最大两块是 **性能门禁**（`frame_cost_*`/`frame_budget_*`
共 9.4s、`bench_*`）——它们量的是**机器性能**（帧耗时相对清屏的比值、吞吐阈值），
耗时与成败都由环境决定，不是代码正确性。

### 它真的是“偶发”的（同一二进制、同一天）

- 第一轮：`frame_cost_stays_within_budget` **FAIL**（best=54.82× vs 阈值 50×）
- 第二轮：它 **PASS**（19.40×），换成 `bench_thresholds_have_bounded_headroom` **FAIL**
  （余量超上限）

**两个方向都会失败**——机器慢就超上限，机器快就余量超界。这些用例进默认路径，
对 AI 迭代就是随机红灯：花时间判真假，而结论几乎总是“环境”。

### 修法：`ST_TEST_SLOW` + 默认跳过

- 新增宏 `ST_TEST_SLOW(名)`（标记慢/环境敏感）、`Registry::add_slow`、
  `Case::slow`；
- `run_all` 默认跳过；`--slow` 或 `st test --slow` 开启；
- **显式指名仍然跑**（`st test frame_cost` 意图明确，不该被门挡掉）——
  只在**未给 filter** 时跳过；
- 汇总行会明说：“另 12 个慢/环境敏感用例默认跳过”。

已标记：`frame_cost_*`/`frame_budget_*`（5）、`bench_*`（6）、
`independent_project_builds_links_and_runs`（真编译真跑，1）。

### 顺带修的**真实解析器缺陷**

`--slow` 一加上就暴露了：解析器把 `--name <非减号开头>` 一律当“带值选项”，
于是 `st test --slow frame_cost` 把 `frame_cost` 当成 `--slow` 的值 → **filter 变空、
全部用例都跑**。同样的坑对 `--list`/`--san` **一直存在**（如 `st test --list pkg`
的 filter 也被吞）。修法：一个显式的布尔选项名单（`is_boolean_option`），
布尔选项**绝不吃下一个参数**。

### 效果（确定性）

| 场景 | 修前 | 修后 |
|---|---|---|
| 改 UI 头 → `st test` | 80.1s | **42.3s** |
| `st test`（无改动） | 37.9s | **24.5s** |
| 慢用例 | 每次跑 | 需 `--slow` |

默认路径 753 例全绿（跳过 12），把“代码对不对”与“这台机器此刻快不快”分开了。

### 同时确认：PCH 与告警开关**都不是**收益点（不要再试）

- **PCH 精简无效**：交替三轮全量构建实测 A（现状）32.8s / B（去低覆盖头的
  filesystem+thread+future）32.7s / C（再减 chrono）32.7s——**无可测差异**。
  `.gch` 是页缓存映射，第二次构建起就常驻内存，缩小它并不省时间。
  （另一方面也解释了上一轮为何失败：那次把 **74% 覆盖率**的 `<format>`
  一起移出了 PCH。）
- **告警开关零成本**：`-Wall -Wextra -Wconversion …` 全开 3.28s vs 全关 3.20s。
- **并行扩展正常**：`-j14` 时 9.1/14 核。
- **方法**：墙钟对比必须**交替多轮取最小值**（本机有背景负载，单次测量会被污染），
  一次性 A/B 脚本用完即弃（不进交付）；判据只能是**全量构建端到端**，
  单 TU 微基准会骗人（已验证两次）。

### 一个方法论教训（写下来免得重蹈）

直接运行 `build/dev/bin/st_tests.exe` 是**过期的**——`st build` 不重建测试二进制
（`st test` 才建，且默认走 **debug 档**）。我曾据此得出“默认运行里有 1 个偶发
失败”的错误结论，白查一阵。**验证测试行为必须走 `st test`**，不要直接调产物。

## 已完成（2026-10-06 构建前置成本：`expand_glob` 前缀下钻）

用户要求：全面优化构建/运行速度，提升 AI 迭代效率。

**量到的第一个瓶颈不是编译，而是“每次构建都付的固定前置成本”**：
空构建（无事可做）要 **5.6s**，`st tree`（纯清单解析）**3.9s**。

### 根因：每个 glob 模式都从工程根 walk 一遍

`fs::expand_glob` 的实现是 `walk(root)` 后逐个 `match_glob`，而 `st.pkg` 有
**14 个模式** → 整个仓库被遍历 14 遍。实测：

| 量 | 值 |
|---|---|
| 一次全树 walk（`.`） | 0.27s / 4644 个条目 |
| 14 个模式各自 walk | **3.75s** |
| 其中花在 `build/` 子树 | **2.90s（78%）** |

`build/` 下 3372 个产物（3.9 GB）**对匹配零贡献**，却每次都被扫一遍。
而 AI 迭代一轮要调用很多次构建——这笔钱每轮都在付。

### 修法

1. **前缀下钻**：`src/core/*.cpp` 只遍历 `src/core`。语义上这是纯优化
   ——`match_glob` 要求模式段与路径段**逐段匹配**，不以字面前缀开头的路径
   不可能命中。
2. **全字面模式不遍历**：`src/ui/script_api.js` 这种（清单的 `embed` 就是）
   只需一次存在性判定。

顺带修的坑：改造时把全字面模式也当子树去 walk，于是清单报
「嵌入模式未命中任何文件: src/ui/script_api.js」——**构建直接失败**。已加回归护栏。

### 效果（确定性指标）

| 指标 | 修复前 | 修复后 |
|---|---|---|
| 空构建 | 5.6s | **0.23s**（24×） |
| `st tree` | 3.9s | **0.13s**（30×） |

新增 `tests/core_fs_test.cpp` 三条：字面前缀切分、全字面模式命中、
下钻不得越界/漏文件。

### 已排除（不要再试）与未做

- **改小 PCH 不可行**：单 TU 微基准看着像“精简 PCH 能省 40%”（114MB→66MB、
  1.88s→1.13s），但**全量构建实测反而从 32.9s 涨到 69.5s**——
  `<filesystem>/<format>/<thread>/<future>` 被从 PCH 移出后，用到它们的 TU
  各自全量解析模板，代价远超收益。已回退并在 `pch.hpp` 注释里留下实测表。
- **本会话的墙钟耗时对比全部被背景负载污染**（同时段 `bun` 进程占约一个核），
  上述 0.23s/0.13s 是负载无关的确定性量（文件系统遍历），可信。
- **PCH 仍待优化**：单 TU 里约 88% 是加载 `.gch` 本身的成本。方向应是
  **按模块分组 PCH**（ui/text/raster 各一份），而不是把标准库搬出 PCH。

## 已完成（2026-10-05 `CodeEditor` 默认字号调大一号 + 字号跟随主题）

用户要求：「代码编辑组件默认字体调大一号」。调常量时查出底下有个**结构性缺陷**。

### 缺陷：编辑器字号与主题脱钩（框架层）

`CodeEditor::apply_theme` 把字号**硬写成绝对像素**（`style_.font_size = 13.0f`），
而主题才是缩放的真值源（`--ui-font-scale` → `Metrics::scale_fonts`）。实测：
`--ui-font-scale 1.5` 时 UI 文字 15→22.5，**编辑器恒为 13.5**（普通 `Text` 走
`theme.metrics().font_base`，所以只有它不动）。

改法（对齐 `Text` 的既有模式，而不是另立一套）：

- `apply_theme` 记下 `theme.metrics().font_base`，实际字号**惰性算出**
  （`font_size()` = 显式像素 或 `base × font_scale()`）；
- 不缓存快照：`apply_theme` 只在 `UiRoot::layout()` 里跑，而本组件**故意**让布局脏
  不冒泡（否则每次编辑拖出整帧重绘）——缓存快照会停在旧值上（实测：
  档位 1.15→1.0 后仍报 17.25）；
- 新增 `font_scale`（倍数，与主题一起缩放）/`font_size_override`（显式像素）
  两个入口 + 协议属性 `font_scale`。前者让“设置面板列几档字号”成立，
  后者给“我就要 20px”留口。

### 默认值

`font_scale` 默认 **1.0**（与正文同级）；gbcode 示例用 **1.15**（比正文大一号）。

### 顺带修的两个同类问题

1. **示例每帧重设档位**：`custom<CodeEditor>` 里无条件 `set_font_scale(1.15f)`，
   把控制通道/设置项的 `set font_scale=…` 当场抹掉（与之前 `read_only` 同一个坑）。
   改为只在首次装上时写一次。
2. **回归测试里硬编码字号**：上轮新写的两条 Tab 用例用字面量 `13.0f` 量宽，
   字号一改就红——改成读 `editor.font_size()`（本就该与绘制同源）。

### 验证

- 新增 `tests/code_editor_font_scale_test.cpp`（5 条）：默认档 == 主题 base、
  主题缩放后**同比例**跟上、档位确实是倍数、显式值优先且可复位、档位夹取到
  可读区间、改字号标布局脏；
- **逆向验证**：把 `theme_base_font_` 改回硬编码 13.0 → **4 条变红**，
  失败信息直指“未跟随主题缩放”；
- `st test` **762 全绿**（+6）/ `st lint` 0 违规 / mingw 交叉编译通过 /
  gbcode E2E 18 组与 gallery E2E、editor smoke 全通过；
- 实测：`--ui-font-scale 1.5` 时编辑器 13.5→**25.875**（与 UI 文字同步）；
  控制通道改 `font_scale=1.0` 后实际字号 17.25→15，**重组后仍保持**；
  截图核验字号已明显变大。

## 已完成（2026-10-05 gbcode 标题栏与菜单栏合并成一行）

用户要求：**标题栏和菜单栏合并**；选定形态为**菜单在左、标题跟在其后**
（与 VS Code / Win11 记事本同构）。

### 实现：菜单栏挂进标题栏的 `leading` 附属槽

`TitleBar` 早就为此留了接口（`add_leading`/`add_trailing`，头注释里写着
“真实应用的外壳里，菜单入口、主题/DPI 等控件本来就长在标题栏这一行”）。
挂到 `leading` 后 `TitleBar::arrange` 自动做了三件事，无需额外代码：
标题**让到菜单右侧**、图标让位给槽（两者都画会叠在一起）、
`drag_rect` 不再把这一行算成拖动区。

但标题栏是**外壳**（`WindowFrame` 自持，在声明树之外），本页 `build()` 只管内容槽——
两者不在同一棵树上。解法：菜单自己成为 `CodeEditorPage::MenuHeader` 组件，
由 `dsl::mount_into(title_bar, menu_header)` 挂进那一行。
于是整个 IDE 仍是一个 `Component`（只是占两处树位），
菜单数据/下拉面板 overlay 全部归它自己。

### 两个必须同时做的配套（漏一个就“看着好好的、实际不动”）

1. **两棵声明树都要推进**：主循环原来只 `host->tick()`（内容槽）。
   漏掉 `menu_host_->tick()` 时，**点击仍命中 `menubar`、菜单状态也变了，
   但面板永远不会出现**（实测现象）。
2. **标题文案仍要推**：删 `build_title_bar` 时我漏了 `set_title`，
   标题栏停在启动时的占位文案上（E2E 当场抓到）。现改为本页在 `build()` 里
   调一次 `push_title()`——标题属于**窗框**的状态，菜单只是恰好与它同行，
   合并布局不等于合并所有权。

### 验证

- `st test` 756 全绿 / `st lint` 0 违规 / mingw 交叉编译通过 /
  gallery E2E 与 editor smoke 全通过；
- `gbcode_e2e.py` **18 组**全通过：新增 **[2b] 合并形态断言**（菜单栏完整落在
  标题栏内 + 在左部 + 占满行高 + 内容区紧跟这一行，即**没有留下空心**）；
- 菜单交互全部仍正常：点击开面板（面板锤在标题栏正下方）、悬停切菜单、
  Esc 关闭、点面板外关闭（坐标由 y=52 更新为 y=20）；
- 截图核验：**省下一整行**（内容区 y 从 72 → 40，即 32px 回到编辑器），
  一行内 `菜单 36→360 ｜ 标题 ｜ 窗口按钮` 层次清楚。

## 已完成（2026-10-05 gbcode 亮暗主题不可用）

用户报告「亮暗切换不可用」。**根因在示例层，框架无问题**。

### 1. 影子状态：按钮文案会翻转，主题从未落地

`toggle_theme()` 只写了页面自己的 `State<bool> dark_`，**从未调用**
`Application::set_theme_mode`。而 `dark_` 只被按钮自己的文案读——于是：

- 按钮标签在「亮色 ⇄ 暗色」间翻转（看起来在切）；
- 状态栏还报「主题 dark · 视觉令牌已切换」；
- 而 `theme.mode` 实测全程 `dark` 不变，像素也不变（**画面一点没动**）。

正确做法早就在同一仓库里：`examples/gallery` 从真值源
`app.root().theme().mode()` 读、调 `app.set_theme_mode()` 写。gbcode 改为同构：
新增 `theme_mode`（读）/`theme_setter`（写）两个注入点，**删掉影子状态 `dark_`**。

### 2. 状态栏子元素被内边距压扁（顺带查出）

状态栏写了 `height = 26, padding = 10` —— 四边统一内边距把 26px 行压成
**子元素只剩 6px 高**（26 − 10×2），图标与文字全被压扁成一条。
隐蔽之处：**容器自身尺寸完全正确**，只看容器 `bounds` 或截图粗看都正常。

修法：给 `BoxProps` 补 `padding_x`/`padding_y`（名字与协议属性面的同名属性
**对齐**——同一概念两套名字就是下一个静默分裂），整体 `padding` 之后应用
（于是可只覆盖一侧）；状态栏改用 `padding_x = 10`。

### 为何这两条能同时漏过全绿的 E2E

旧断言只看按钮文案 `label in ("亮色","暗色")`——而缺陷恰恰是「文案变、画面不变」，
于是这条断言**恒为真**，把一个完全不能用的功能报成通过。改为断言：
`theme.mode` 真值翻转 + `capture.hash` 像素变化 + **来回切**（只能单向切也是坏的）
+ 状态栏子元素占满行高且按钮真能被命中。

### 验证

逆向验证（回退修复必须让断言变红）：回退主题写入 → E2E 精确报出
`主题按钮未切换主题真值: light → light`；回退轴向内边距 → 单元用例变红。
`st test` 756 用例全绿（+1）/ `st lint` 0 违规 / mingw 交叉编译通过 /
gbcode E2E **17 组**全通过（+2）/ gallery E2E 与 editor smoke 全通过 /
截图核验亮暗两态均已真正换肤。

### 附带发现：`st test <filter>` 不重编

`st test axis_padding` 在源码已改的情况下报「测试构建：179 单元（**重编 0**）」
并跑出旧结果——带过滤器的 `test` 似乎不重新编译。实测中它让一次逆向验证
假绿了一次（差点误判“测试抓不住缺陷”）。绕法：先 `st build --profile=dev --force`
再 `st test`。待确认是否为工具链缺陷。

## 已完成（2026-10-05 平台默认字体取向）

用户要求：**界面默认中英文、黑体都用微软雅黑，等宽都用 Consolas**。

- 字体候选原先把 Windows / Linux / macOS 路径混在一张表里（`text.cpp`），
  且雅黑排在拉丁层**之后** → 英文被 Segoe UI 接管，界面是“中文雅黑、英文 Segoe”的两族混排。
- 新增平台层 `st/core/font_platform.{hpp,cpp}`：路径与首选族按平台集中
  （CONVENTIONS §10 第 1 条），`text.cpp` 只负责按返回顺序探测。
  链形：**平台首选 → 通用拉丁 → 通用 CJK → 符号回退**；环境变量覆盖仍最高优先。
- 粗体链与常规链**逐位同序**（按下标配同族粗体面）。

**客观验证**（不靠观感）：新增 `tools/font_stack_probe`（已入 `st.pkg`）打印
每个码点实际由哪个 face 提供——实测：`A`/`M`/`7`/`霜`/`。` 全部命中 `msyh.ttc`；
`i`/`M`/`W` 命中 `consola.ttf`（汉字 `霜` 回退 `msyh.ttc`）；粗体 `A`/`霜`
均命中 `msyhbd.ttc`；`✓` 仍由 `seguisym.ttf` 回退（换首选没断掉符号回退）。

**回归**：新增 `tests/text_font_default_test.cpp`（6 条）+ 修正探针的
`-Wconversion` 报错。逆向验证：把雅黑挪回拉丁之后 ✗3 条（中英文同族/
首选归属/粗体归属），把 Consola 挪到通用等宽之后 ✗1 条（等宽首选）——均真能抓住。
`st test` 755 用例全绿 / `st lint` 0 违规 / mingw 交叉编译通过 /
gbcode + gallery E2E 全通过 / 截图核验已换体（暗色与亮色两侧）。

## 已完成（2026-10-05 gbcode 可用性打磨轮）

背景：报告「很多特性实际都不能用」——改为**控制通道实测驱动**：逐条跑真实交互、
截图/像素测量取证，再按根因深浅分流到框架层与示例层。以下条目全部带回归测试，
并对关键修复做了**逆向验证**（临时回退修复 → 测试变红 → 恢复）。

### 框架层（`ui` 通用，不只 gbcode 受益）

- [x] **声明式 overlay 宿主吞掉全屏输入**：宿主是铺满视口的 `Panel`，而 `Element::hit_test`
  按整块矩形算 → 浮层一挂上，编辑器/侧栏/状态栏**全部点不动**（实测：菜单打开后
  连“点面板外关掉它”都做不到）。改为 `OverlayHost`：命中默认收窄到子元素（非模态浮层穿透），
  可按需切**屏障形态**（瞬态浮层：面板外点击=关闭且不穿透）。回归：`gbcode_e2e.py [10]/[11b]`。
- [x] **浮层键盘分派只问最外层宿主**：`UiRoot::dispatch` 的模态分支对 overlay 只调一次
  `on_event`；宿主自己不认 Esc → 真正认键的 `MenuPanel`/`CommandPalette` 在子树里收不到，
  “Esc 关面板”永远失效。新增 `dispatch_key_into`（深度优先下钻，容器自身优先），
  并修掉“先子后父”会让输入框先吞掉方向键的问题。回归：同上。
- [x] **`dsl::menu_bar` 未接 `on_menu_close`**：面板关闭请求无处可去（`MenuBar::open_index_`
  关了，overlay 还在）；且缺一个“外部请求关闭”的**公开入口**。补 `MenuBar::close_panel()`
  并在声明式封装里接线。
- [x] **`CommandPalette` 没有 `close` 动作**：`invoke(close)` 恒 `handled=false`——
  面板关不掉且遮罩仍在，把后续所有点击都挡住（实测：E2E“关了面板再点菜单”必失败）。
- [x] **`Element` 新增元素拿不到宿主契约**：`host_` 只在 `UiRoot::set_content/add_overlay`
  递归写一次，而声明式每帧新建元素是直接 `add_child`——于是 `CommandPalette::grab_focus()`
  只能走兜底路径，“面板打开了、键盘焦点还在编辑器里”（敲字跑进代码）。
  把继承点收敛到 `Element::add_child/insert_child`（`adopt_host()`）。
- [x] **`CodeEditor`：PageUp/PageDown 无效且把视口弹回顶部**：旧实现只改 `scroll_y_`，
  而其后 `ensure_cursor_visible` 按“光标恒可见”把它拉回光标行——光标在第 1 行时永远原地不动
  （实测：手工设 `scroll=0,400`，一按 PageDown 立刻回到 0）。改为翻页 = 光标按可见行数移动 + 视口跟随。
- [x] **`CodeEditor`：制表符量尺与绘制不同源**：`\t` 被当普通字形量宽/绘制，
  于是光标、选择、查找高亮、缩进参考线、鼠标命中**全部错位且随列号累积**
  （实测：同一段代码 Tab 缩进与 4 空格缩进渲染宽度不一致）。新增 `expand_tabs`
  （展开到制表位 + 偏移映射），绘制/量宽/命中统一走它。像素证据：修复后两种缩进截图**逐段一致**。
- [x] **`CodeEditor`：拖垂直滚动条会改光标**：命中落在“移光标”分支（实测光标从第 1 行跳到第 30 行）；
  且命中判据（`content_width > bounds_`）与实际 `max_scroll_x()` 不同口径。改为两条滚动条
  有独立拖拽态、几何收口到 `v_scroll_bar_rect/v_scroll_thumb_rect`（与绘制同一份）。
- [x] **`CodeEditor`：只读保护只靠各入口自觉**：`delete_selection` 未查 `read_only_`，
  配合属性面被重组重置就会“只读下清空全文”。保护下沉到 `delete_selection`（所有删除路径的必经点）。
- [x] **示例每帧重设编辑器属性**：`build_editor_area` 每次重组都执行 `set_language/set_read_only`，
  控制通道 `set read_only=true` / `set language=python` **活不过一次重组**（实测：
  设完只读后一按 `select_all`——它写状态触发重组——立刻被改回 `false`）。改为只写“持久配置”，
  语言/只读态归“当前标签”与属性面所有。

### 示例层（`examples/gbcode`）

- [x] 菜单「编辑/选择」里的 `copy`/`paste`/`goto-line` 是**假的**（copy 只读不写剪贴板、
  paste 插死字符串「（剪贴板内容）」、goto-line 恒跳第 1 行）→ 接真实 API。
- [x] 查找条打开后未聚焦（敲字跑进编辑器）→ 帧首 `apply_pending_focus` 经宿主契约聚焦。

### 验证

- `st test` **748 用例全绿**（+9 新用例，`tests/ui_code_editor_paging_test.cpp`）/ `st lint` 0 违规 /
  `gbcode_e2e.py` **15 组全通过**（+6 组）/ mingw 交叉编译通过。
- **逆向验证**（回退修复必须让测试变红）：PageDown、垂直滚动条拖拽、Tab 命中、
  浮层键盘下钻、overlay 命中收窄、示例属性面——共 6 处逐条验证。

## 已完成（2026-10 代码重构轮）

- [x] **组件绘制层重复助手收敛**（`components_internal.hpp`）：`oriented`(4 逐字副本，已变死代码)、
  `fill_round_rect`(5)、`paint_focus_ring`(5)、`paint_outline`(3)、`text_port_of`(11)、
  `tone_from_name`(2)、`draw_line`/`draw_triangle`——共 18 文件 +236/-398。
- [x] **空模块 `src/gpu` 归置**（该目录已删）：目录空置但被跟踪，`st.pkg` 模块名 12→11。
- [x] **`st test --san` 全量档恢复零报告**（预先存在的 heap-use-after-free）：
  `ui_select_opens_via_overlay_host`（`tests/ui_tabs_test.cpp`）在 `click_at` 后解引用
  `overlay = root.overlay_at(0)`——该指针已在 `dispatch → layout → Select::arrange →
  flush_dismiss → remove_overlay → reap_overlays` 链里被释放。框架的延迟析构**是对的**
  （作者为此用例旧修过 SIGSEGV），是**测试跨派发持有裸指针**违规：改为用 `overlay_count()==0`
  断言摘除结果，不再解引用失效指针。修后 `st test --san` = 697 passed/0 failed/0 ASan 报告。
  （已先实验确认该 UAF **先于重构存在**：组件目录整目录回退到 2089c30 后仍 5/5 失败。）
- [x] **两份 `utf8_prev` 语义分歧（含一个真缺陷）**：收敛为骨架层 `st::utf8_prev/utf8_next`
  （`st/core/string.hpp`，与既有 `decode_utf8`/`utf8_offset` 同族），`Input`/`TextArea`/
  `CodeEditor` 共用一份。同时修掉旧 `input.cpp` 副本的缺陷：旧版 `utf8_prev(text, size) == size`，
  而打字后光标恰在文末（`insert_text` 后 `cursor_ += size`）——`erase(size, 0)` 什么都不删，
  表现为「打完字退格键没反应」（中文/英文均中招）。`code_editor` 版本已正确，其语义即正式定义。
  回归：`tests/ui_input_test.cpp: ui_text_area_backspace_at_end_deletes_last_codepoint`
  （先在旧代码上跑红、修复后转绿）+ `tests/core_basics_test.cpp` 两组边界单测。

## 已完成（2026-10 结构优化轮 · 分支 optimize/structure-2026）

详细的改动清单与实测数据见 `docs/OPTIMIZATION_REPORT.md`；基线与度量探针见
`docs/OPTIMIZATION_BASELINE.md`。要点：

- [x] **修 4 处真实缺陷**：`active_composers` 无锁遍历（并发 UB）、语言注册表的跨 TU
  静态析构顺序、`dsl.cpp` 20 处解引用空指针、`owner_` 的 `void*` 类型擦除。
- [x] **补 2 处工具缺口**：L8 原先只判花括号深度 0（**匿名命名空间里的可变全局全部漏网**，
  规则近乎失效）；L14 原先只在文档里声称存在（禁用 include 检查）。
  收紧 L8 后**同一次扫描**从 0 违反变成 6 条真问题——「文档写了、工具没做」比不做更危险。
- [x] **消重复真源**：组件类型身份原先有**两套表**（`type_name` 特化 + `make_element`
  if 链，互不相识；漏改一处即静默失效）→ 收敛为一份清单，新增组件从改 3 处变改 1 处。
  连带修 L13 的 `kElementStateMembers`（与 `element.hpp` 手工同步、本轮已漏改过一次）。
- [x] **拆巨型函数**：`control/server.cpp` 的 `handle()` 840 → 29 行（+ 22 个方法函数）；
  `highlight_builtin.cpp` 511 行单函数 → 30 个具名函数。
- [x] **Element 瘦身**：`sizeof` 432 → 376 字节（渲染附属状态惰性分配）。
- [x] **json 前向头**：`app.hpp` / `control.hpp` 的预处理展开各降 23%；
  `manifest.hpp` 因 `Json` 成员是**语言限制**做不了，已在头里写明。
- [x] **新增 `st stats`**：源码结构度量工具（单遍扫描、剥离注释与字符串），
  把"最大函数/热点头/复杂度"从一次性脚本变成可复现的一条命令。

**如实记录的两处判断修正**（都写进了对应代码注释与报告）：
1. 立项时写的 `sizeof(Element)` 目标「~120B」**不可达**（核心字段就占 350，可压缩只约 80）；
2. 评审时写的 `Surface`「34 个纯虚拆成 4 路」是**过度设计**——真正有问题、也确实能修的
   只有 `blend_coverage_bitmap` 一处（它违反 `surface.hpp` 自己第 15 行的规则）。

## 已完成：整帧 paint 成本偏高（2026-10 性能轮）

**起点**：帧耗时门禁（`tests/app_frame_budget_test.cpp`）在评估手段轮测出来的——
这是"把评估手段做对"的直接回报。完整的诊断、探针与结论见
**`docs/PAINT_DIAGNOSIS.md`**；这里只记结果与**如实记录的未达标项**。

**优化前**（96 卡片 + 96 段文字，1280×800，release -O2）：
帧 p50 24.85 ms、paint 28.62 ms（`layout 0.03ms`，成本几乎全在 paint），
拆开是 **shadow 14.0 / stroke 6.9 / round_rect 2.1 / clear+text 1.6 ms**。

**做了什么**（两条，都是"等价但更省"，不是近似）：
1. **两层投影合一成一张缓存贴图**（`Surface::draw_shadow_layered`，见 `DESIGN.md §4.2.9.1`）：
   shadow 14.0 → **6.9 ms**。正确性依据是 **src-over 结合律**——贴图存"阴影自身的
   预乘色"，因此对任意底色都与"依次贴两次"等价（`tests/paint_optimization_test.cpp`
   用非纯色底与饱和色底守着）。
2. **圆角边框改走环形填充**（`raster::make_rounded_border_ring`）：stroke 6.9 → fill_path
   **3.9 ms**。差别不是 API 语义，而是 `stroke_path` 把 1 条路径展开成 **152 条边 + 顶点补圆**，
   而环形只有 76 条边。

**优化后**：判据帧（最快帧）**14.4 ms = 35×~38× 清屏** —— 40 FPS → **~68 FPS**，
**已进入 60 FPS 预算**。帧门禁随之改口径并收紧：
判据从 **p50 改为"最快帧"**（共享机器上 p50 的比值在 30×~47× 乱摆，而最快帧稳定在
35×~38×；外面还套多轮取最快，挡掉持续整轮的系统尖峰），阈值 **75× → 50×**、
余量上限 **25× → 2.5×**。
新增 `tests/paint_optimization_test.cpp`（17 用例：边框容差等价 / 环形空心与退化几何 /
两层贴图与两次绘制的 Δ≤1..2 等价 / 贴图不烘底色 / DPI / 缓存淘汰 / 两条"不许更慢"护栏）。

**同轮修掉的一个测量口径缺陷**（比优化本身更值得记住）：帧门禁与分阶段诊断
**从来没有挂文本端口**——`UiRoot::set_text_port()` 从未被调用，于是
`Element::paint_text` 走 `NullTextPort`（`measure_width()` 恒 0、`draw()` 空实现），
场景里 96 个 `Text` 元素**一个字形都没画**。"96 段文字"与实测不符，
"瓶颈是不是文字"这条方向因此永远测不出来（实测文字只占 3%）。现已挂真实
`RendererTextPort`（`RootFixture`）。

**未达标项（如实记录）**：目标写的是"paint 25.9 → ≈8 ms"，**现状 15.3 ms，未达标**。
剩下的差距有明确着落点，按可抠的量排序：
1. **阴影合成的"被盖区"**（≈2.5 ms 可省）：贴图非零 15744 px/张，而卡片自身只有 6720 px——
   每次投影有一半以上的混合做在"随后会被卡片自己**不透明底色**完全盖住"的区域上。
   需要给接口加"将被覆盖的区域"这一参数（并在每个调用点保证那个不变量：随后真的铺不透明底色）。
2. **圆角填充**（2.0 ms）：`fill_rect(radius)` 走通用路径光栅化，可做"圆角矩形"的专用扫描线。

   ⚠ **2026-10-14 尝试过，未采用（带数据记录，避免后人重走）**。先修回了一个真回归
   （`b420909` 那版逐像素累加遮蔽了 `simd::blend_row` 快路径，圆角 1.32× → 26.7× →
   回到 1.70~2.14×，详见文末性能门禁条），然后试了**解析扫描线**：

   | 版本 | 圆角比值 | 与 `Path` 版的像素差（gallery 全屏 102.4 万像素）|
   |---|---|---|
   | 基线（`Path` + 修好的快路径）| 1.70~2.14× | —（基准）|
   | 解析·行内恒定内缩 | 0.39× | **0.73%**（其中 0.63% 通道差 > 8）|
   | 解析·逐行 4 子带（复刻采样模型）| 2.41× | **0.70%**（其中 0.56% 通道差 > 8）|

   **两版都否决**，理由不同：
   * 行内恒定内缩虽然快到 **0.39×**（比整数对齐直角还快、比基线快 4~5 倍），
     但 0.63% 像素肉眼可见地不同（板边形的阶梯形状被抹平）；
   * 逐行 4 子带（复刻 `fill_path_aa` 的采样模型）降到了 2.41×——**比基线还慢**，
     因为多了“每行 4 条 run + 每像素累加”的中间物，而那正是 `merge_runs` 要避免的。

   结论：**两项指标不可兼得**，除非把 `fill_path_aa` 的采样模型本身换掉
   （例如改成“轴对齐段 + 弧段在 y 上解析积分”的混合模型）——那是另一档工作，
   而当前 1.7~2.2× 已在阈值（3×）内且相对度不高。有意保留：**不拿 0.56% 像素差
   换速度**（本仓已因这类“看着差不多”的改动返工过：`perceptual_changes.md`）。
   工具已留下：`tools/rounded_fill_probe.cpp` 与 `tools/raster_pixel_diff.py`
   ——下次真要碰这条路，先跑这两条得到“比值 + 像素差”两个数，再决定。
3. **清屏**（0.8 ms）：整屏 `clear` 与随后的绘制可合并（首行按"清色"写而非"混合"）。

**顺带发现、尚未处理**（与本轮无关，但已知）：
- ~~`raster_bench` 的余量自检在共享环境下会红~~ → **本轮顺手修好了**。根因不是阈值太紧，
  而是**单次测量把调度噪声当成了成本**：同一份代码连跑 4 次，渐变填充的比值在
  **1.10×~1.68×** 之间跳（+53%），于是"余量 = 阈值/实测"这个量在噪声上做文章。
  修法：把测量改成 `best_of`（重复 3 遍取最小值——噪声只会**抬高**耗时，最小值最接近真值），
  **阈值不动**。测量变准之后反而发现原来的阈值**普遍偏松**（文本真值 23× 而阈值 90×），
  于是顺势收紧到 3/24/50/4（余量 ~2.2~2.5，且 3 倍退化都能抓住）。
  同时把 sanitizer 档与发布档分开：ASan 的插桩开销 ∝ 内存访问量，而作为分母的"清屏"
  是纯内存写——比值在插桩档下**不再机器无关**（实测 59×~142× 乱跳），
  所以比值型断言在插桩档如实跳过（并打印跳过了哪一条），而不是把阈值放宽。
- ✅ **`script_memory_limit_stops_runaway_allocation` 已在基线树上就红 —— 2026-10-14 修好**。
  根因**不是**“QuickJS 报错文案与断言不一致”（当初的推测），而是**两条 OOM 分支没盖全**：

  | 分支 | 触发条件（同引擎、不同构建档） | 修复前文案 | 修复后 |
  |---|---|---|---|
  | 异常对象不可用 | 内存耗尽时连异常都构造不出（**dev 档实测走这条**）| 已含“内存超限：上限 N MiB” | 同（保持）|
  | 有异常但英文 | quickjs-ng 产出 `InternalError: out of memory`（**release 档实测走这条**）| **只有一句英文，上限无处可查** | 保留原文 + 追加“（通常是内存超限：上限 N MiB）”|

  修在 `describe_script_failure`（从 `describe_exception` 里抽出的**纯函数**）。

  ⚠ **抽出来本身就是这次最值得留的经验**：原先整形逻辑埋在要 `JSContext*` 的函数里，
  于是一条“文案能不能读懂”的断言只能靠真脚本撞 OOM——而 OOM 表现**随构建档不同**，
  于是缺陷**一档红一档绿**，而且**无法定向验证**：实测把识别代码改成 `if (false)` 后用例
  仍然 PASS（被另一条分支满足了）——那就是一条**不 load-bearing 的判据**。
  抽成纯函数后两条分支都能直接断言（新增 `script_failure_text_is_actionable_on_both_oom_paths`，
  含“与内存无关的异常不得被误加内存提示”的反向断言），
  逆向验证：删掉任一条分支 → 对应用例当场变红。

**这一轮最值得留下的一条**：`raster_bench` 那 4 个阈值此前"看起来标定得很讲究"
（余量 2.1~3.0，还有自检盯着），但它们的**基准值本身是噪声污染的**——
自检只在自我一致的口径里打转，发现不了"尺子本身在抖"。**先让测量稳，再谈阈值。**

## 2026-10-05 实战反馈轮：新发现（P1）

来自一个 7 页真实应用（`dev-tools`，霜天原生重写）的反馈。前四项（`for_each` 索引、
排版三件套、`input` 密码模式、`TextArea` 只读）与对齐复杂度、lint 豁免、集成回归
本轮已落地（见 git 提交），下列是**本轮没做完 / 新看出**的：

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

> 本章剩余的未完成项已归位到上方 P1/P2 待做区（2026-10-09 整理）。


## P1（2026-10-06 浏览器全字符集/全字号对照后）

### ⚠ 先纠正一条**已写错过**的结论：不存在"拉丁/等宽偏重 1.10~1.16"

**曾经写下的错误结论**：按"字类分组"报出汉字 0.99 / 等宽 1.16 / 拉丁 1.10（实心比），
并据此开了"真型字体偏重、需要按字体分档"的方向。

**错在哪（量尺）**：分类是按**行的标签文本**做的，而那段页里许多行的标签里混着两种字
（「常规 霜天组件画廊 Regular」「text #0F172A 正文」…）——于是**以拉丁/等宽为主**的那些行
被算进了汉字类，把汉字类拉高、把拉丁类拉低，得出一个**方向相反**的结论。

**改正后的干净测量**（新增 `tools/text_ab_families_page.html`：同一字族单独成行、只变字号；
分类由生成器写进 JSON 元数据的 `class` 字段，不再让下游猜字符串）：

| 档位 | 汉字 墨量/实心/过渡 | 拉丁 | 等宽 |
|---|---|---|---|
| **应用默认（拟合 light + 加墨 + γ1.10）** | 0.956 / **0.946** / 0.998 | 0.951 / **0.953** / 0.937 | 0.948 / **0.964** / 0.970 |
| 关拟合（加墨 + γ1.10） | 0.928 / 0.903 / 0.993 | 0.940 / 0.882 / 1.035 | 0.944 / 0.944 / 1.016 |
| 关拟合 + γ1.00 | 0.964 / 0.991 / 0.951 | 0.963 / 0.932 / 1.008 | 0.967 / 1.010 / 0.953 |

⇒ **三类字在应用默认档下都落在 0.95±0.02**，彼此没有系统差异；
此前的"汉字已经很准、拉丁偏重"是**量尺造出来的**。
（分段实测也确认：霜天在"纯覆盖率 + 亚像素滤波"下三类字其实略**轻** 0.95~0.96，
是拟合 light + γ1.10 把它们推回 0.95 附近。）

### ⚠ 参照敏感性：那"5%"在参照定义的噪声里，**不要当成缺陷去追**

上一条"过渡像素偏高约 5%"是用**浏览器原生 1.5× 渲染**当参照量的。为了排除
"1.5× 光栅化有谐波走样（定量化陷阱）"，做了对照实验：同一页让浏览器在
`deviceScaleFactor=3` 下渲染、再降采样到 1.5×（LANCZOS 2:1），同一套量尺重测：

| 参照 | 拉丁 实心/过渡 | 汉字 实心/过渡 | 等宽 实心/过渡 |
|---|---|---|---|
| 浏览器原生 1.5×（**用户实际看到的**） | 0.953 / 0.937 | 0.946 / 0.998 | 0.964 / 0.970 |
| 浏览器 3× → 降采样 1.5× | **1.038 / 0.904** | **1.029 / 0.988** | **1.101 / 0.943** |

**同一个霜天二进制、同一套量尺，只换参照的定义，实心比就从 0.95 变成 1.03~1.10。**
⇒ 这个量级（±5~10%）**落在"参照怎么定义"的噪声里**，不构成可追的缺陷。
（两列都有理由：原生 1.5× 是**用户实际那条链路**；3× 降采样理论上覆盖率更准，
但引入了降采样滤波这个新变量。）

**结论**：按约定"参照取用户实际那条链路"，**以原生 1.5× 为准**（三类字 0.95 附近）；
在换参照之前**不要**再改滤波/gamma 去追那几个百分点。
若将来要定"哪种参照才对"，那是一个独立的、需要先立判据的课题。

### 曾经的候选缺口：**过渡像素占比偏高**（三类字一致，受上一条限定）

**现象**：默认档下"过渡像素"比：汉字 0.998、拉丁 0.937、等宽 0.970 ——
即霜天在覆盖率投影口径下**边缘过渡带比浏览器宽约 5%**，而墨量与实心像素都对齐了。
观感上对应"边缘略软"，是最接近"够不够锐"的量。

**已知的强杠杆**：亚像素低通滤波强度。实测（应用默认档之上只改滤波）：
`{8,77,86,77,8}`（现状）→ 过渡比 0.937；`{2,20,212,20,2}`（1/4 强度）→ 数字未定档
（见 §4.3.7.12：1/4 强度在等宽串上比"全关"更锐且彩边更少，是 Pareto 点）。

**方向**：把滤波强度按**字族**分档（等宽代码行细笔画最需要锐度、正文档位取现状），
但要先解决"彩边 vs 锐度"的产品取舍——**属产品决策，需用户定档**。

### ✅ 已做：字形类**分档**（数字 0.92 / 字母 0.98 / 汉字 0.93）

**用户要求**：「已经对好了的不要动，细化分档」。据此**不改全局 γ、不动字母与汉字**，
只给**数字类**加一层覆盖率分档。

**字母类同批补一档**（用户同批要求）：基准档下字母的**逐字形总墨量比**在四个字号上是
0.975 / 0.950 / 0.943 / 0.952（大写）、0.989 / 0.974 / 0.941 / 0.943（小写）——
12/13px 明显偏轻。扫描后取 **γ=0.98**：

| 字母类 γ | 10px 大写/小写 | 11px | 12px | 13px |
|---|---|---|---|---|
| 不覆盖（基准） | 0.975 / 0.989 | 0.950 / 0.974 | 0.943 / 0.941 | 0.952 / 0.943 |
| **0.98（采用）** | **1.004 / 1.021** | **0.977 / 1.001** | **0.969 / 0.974** | **0.975 / 0.972** |
| 0.95 | 1.012 / 1.029（10px 过冲） | 0.984 / 1.008 | 0.976 / 0.986 | 0.984 / 0.979 |

⇒ 四个字号全部落进 **0.97~1.02**。`ST_TEXT_LETTER_GAMMA` 可覆盖。
（**汉字类（`Default`）不设档**——它此前已对齐（0.95~0.96），按"已对好的不要动"保持原样；
回归用例里直接断言"汉字位图与不设档的渲染器逐位相同"，并做过逆向验证。）

**汉字类单列并定档 γ=0.93**（同一批要求，第三档）：建**一行一字**的汉字专项页
（`tools/text_ab_han_page.html`，25 字 × 7 字号，含简单/中等/复杂笔形）测得汉字在
**10~20px 上稳定偏轻 5~7%**（总墨量比 0.929 / 0.933 / 0.944 / 0.940 / 0.949 / 0.938），
且**复杂字比简单字更轻**（复杂 0.90~0.93、简单 0.93~1.02）。扫描取 **0.93**：

| 汉字 γ | 10px | 11px | 12px | 13px | 15px | 20px |
|---|---|---|---|---|---|---|
| 不覆盖 | 0.929 | 0.933 | 0.944 | 0.940 | 0.949 | 0.938 |
| 0.94 | 0.987 | 0.991 | 0.998 | 0.993 | 0.993 | 0.972 |
| **0.93（采用）** | **0.992** | **0.995** | **1.002** | **0.997** | **0.997** | 0.974 |
| 0.92 | 0.996 | 0.999 | 1.006 | 1.001 | 1.000 | 0.976 |

20px 仍略低（0.974）是"字号越大越接近"的自然趋势，不做过补偿。
汉字此前并进 `Default`（连同标点/符号），**现已拆开**：`Default` 不设档（标点/符号保持原样）。

**新设施**：`TextRenderer::GlyphClass`（`Default` / `Digit` / `Letter` / `Han`）+ `set_class_gamma`，
`TextRun` 携带类、**绘制路径照它取位图**（这条接线最容易漏——见下"漏一次"），
缓存键含"类覆盖是否生效 + 其取值"。

**档位由扫描定**（`tools/text_ab_allglyphs_page.html`，逐字形墨量比，四个字号）：

| 数字类 γ | 数字 10/11/12/13px | 字母（大写/小写） |
|---|---|---|
| 不覆盖（基准） | 0.880 / 0.917 / 0.908 / 0.921 | 0.94~0.96 |
| **0.92（采用）** | 0.957 / 0.969 / 0.974 / 0.973（**按类单独扫描时的口径**） | 逐位不变 |
| 0.90 | 0.963 / 0.975 / 0.979 / 0.988 | 逐位不变 |
| 0.85 | 0.978 / 0.989 / 0.992 / **1.017（13px 过冲）** | 逐位不变 |

取 0.92：四个字号都落进 0.95~0.98，而 0.85 在 13px 会过冲。
可用 `ST_TEXT_DIGIT_GAMMA` 覆盖（诊断/复标用）。

**最终三比值**（数字类 γ=0.92，逐字形、按类）：

| 字号 | 数字 墨量/实心/过渡 | 字母（大写） | 字母（小写） |
|---|---|---|---|
| 10px | 0.957 / 0.962 / 0.988 | 0.961 | 0.943 |
| 11px | 0.969 / 0.944 / 0.993 | 0.939 | 0.947 |
| 12px | 0.974 / 0.981 / 0.941 | 0.943 | 0.938 |
| 13px | 0.973 / 1.000 / 0.990 | 0.952 | 0.939 |

**接线漏一次（值得记）**：`glyph_bitmap_of`（诊断/探针）走类很容易，而 `draw` 是**另一条路径**
——第一次实现时 `draw` 里仍调不带类的重载，于是"改类 γ → PNG 逐像素不变、
而接口层墨量却变了"。回归用例的第③段专门钉这一条（**逆向验证：去掉接线后当场变红**）。

**仍未做**：数字之外的细微类间差异（如 10px 竖笔类偏重 +17%、斜笔类偏轻 −13%），
以及"归因未定"那条下面的 TT 提示指令议题——都留待有**尺度无关判据**之后再谈。

### 更正：汉字那批数里有两个量尺假象（2026-10-06 二次复核）

复核"10px/20px 是不是真的偏低 5~7%"时发现，**部分偏差是我的行窗口造成的**：

**坑 1：用"紧凑 DOM 行框"切窗口会裁掉最宽那笔。**
汉字 `三`（三横、最下横最宽）在 32px 下被测成 38px 宽、浏览器 42px——
实为窗口把最宽横裁掉了；`二` 同理（-8px）。改用**相邻行框中点**（大窗口）后，
同一批数据从"10px 0.929 / 20px 0.938"变成 **0.992 / 0.974**。

**坑 2：大字号下"相邻行框中点"会吃到邻行的墨。**
32px 档窗口高 ~70px 而字形墨高 ~45px，实测霜天侧紧包围盒高度从 45 变成 70
（混进了上下邻行）；该档的数**整批不可用**（曾算出 0.52 这种明显错误的聚合值）。
⇒ **大字号的逐字形量测需要先做"墨迹连通域隔离"**，本仓库当前工具没做，故 32px 档不报数。

**结论**：复核后的干净读数是（大窗口、10~20px）：
**0.992 / 0.995 / 1.002 / 0.997 / 0.997 / 0.974** ——
10~15px 已对齐，**只有 20px 仍低 2.6%**（不做补偿：类 γ 与字号无关，
补 20px 会把 12px 顶到 1.028）。

### 更正：0.92 与 0.96 的差**不是"排布"造成的，而是统计量**（2026-10-06）

曾把"全量页（一行一字）测出字母 0.92、字族页（一行一串）测出 0.96"解释为
**"孤立单字形 vs 文本串"**。做直接实验后**该解释被推翻**：

新增 `tools/text_ab_string_page.html`（同一批 62 个字符**拼成一串**、四个字号各一行），
与"一行一字"页的同字号总墨量对比：

| 字号 | 一行一字 总墨比 | 拼串 总墨比 |
|---|---|---|
| 10px | 0.973 | 0.980 |
| 11px | 0.947 | 0.950 |
| 12px | 0.933 | 0.939 |
| 13px | 0.941 | 0.947 |

⇒ **两种排布差 ≤0.7%**，排布（孤立 / 串）不是变量。

顺带排除了**起点相位**这个机制（`tools/ink_phase_probe.cpp`）：同一字形把起点从
`.0` 扫到 `.9`，墨量和**恒为 11.15（极差 0.0%）**——因为笔位被取整到整数像素、
位图不带子像素偏移，所以"串内字形相位不同"在这条路径上不产生影响。

**真正的原因是统计量**：
- `0.92` 是**逐字形比值的中位数**（62 个比值的中间值，对少数明显偏轻的字形敏感）；
- `0.96` 是**总墨量之比**（按墨量加权，笔画粗的字形权重大）。

⇒ 以后报数必须**写明是哪个统计量**，两者不可互换。以"总墨量之比"为主口径
（它对应观感上的整体浓淡），"逐字形中位"作为离散度指标。

### （历史）数字/字母在小字号偏轻 8~10%：归因未定的部分

**现象（稳定）**：全量逐字形实测（62 字符 × 10~13px，`tools/text_ab_allglyphs_page.html`）
与真窗口浏览器比，**数字类稳定偏轻 8~10%**、字母 4~6%，且**字形盒宽/高比全部 1.000**
⇒ 差距在"墨"，不在几何。轮廓几何也逐点一致（`glyph_outline_compare --cp`，面积比 1.0000）。

**已排除**（都能二分，且结论是"无关"）：
- 宽度量化闸门、笔画加墨、几何拟合、**墨量补偿**：关掉后数字墨量比**逐位不变**
  （0.880 / 0.915 → 关补偿仍 0.880 / 0.915）。

**gamma 是关键旋钮（实测，可行动）**：同一批数字，只改覆盖率 gamma：

| γ | 10px 数字 | 10px 大写 | 10px 小写 | 13px 数字 |
|---|---|---|---|---|
| 1.10（现状） | 0.880 | 0.961 | 0.943 | 0.915 |
| 1.00 | 0.922 | 0.988 | 0.970 | 0.939 |
| 0.95 | 0.948 | 1.002 | 0.986 | 0.952 |
| **0.90** | **0.963** | 1.016 | 1.001 | **0.978** |
| 0.85 | 0.978 | 1.031 | 1.016 | 1.005 |

⇒ γ≈0.90 能让**三类字同时落在 0.96~1.02**（现状 1.10 时数字只有 0.880）。
但 γ 是**全局**的：它同时也是现有标定值（真窗口 1.10），改它等于改所有字形的观感——
**属产品决策**（与 §4.3.7.17 的标定结论冲突，需要重标而非拍脑袋）。

**为什么"做 TT 指令解释器"现在**不能**立项**（三条实测）：
1. 用 `tools/freetype_glyphs_render.cpp` + `freetype_hint_ceiling.py` 量"提示能补回多少"，
   结果**符号随字号翻**（10px 补 −0.04~−0.09、13px 补 +0.10~+0.13）——
   提示不是"单调加墨"，它按字形改轮廓，方向取决于相位；
2. 逐字形尺度的"霜天 / FreeType(默认档)"在同一字号内从 **0.86 涨到 1.08**（相位/反走样噪声），
   **符号会翻** ⇒ 它**不是稳定的逐字形归因量**，任何按它的结论都不可靠；
3. 我自己的量尺在这条调查上栽过三次（灰度 vs LCD 模式不一致、滤波 vs 未滤波口径不可比、
   探针把字号参数当码点）——每次都会给出一个"看起来合理"的错方向。

**正确顺序**：
1. 先做**尺度无关的逐字形判据**（跨字号求期望、或按类看整页），确认偏差有**稳定符号**；
2. 若稳定，再决定是"做提示指令"（贵）还是"按轮廓类型/字形类给覆盖率映射分档"（便宜）；
3. "先吸提示、再让轮廓跟随"（下面那条）在这条路上是**前置**工作，不是并行工作。

**判据**：数字类墨量比 ≥0.95 且**跨 10~13px 符号一致**；字形盒宽/高仍为 1.000。

### 拟采用但**未采用**的候选：字体自带 hints 的"先吸提示、再动轮廓"

`tools/stems_probe.cpp` 实测（Noto Sans CJK SC @逻辑 15px）：102 个字形里只有 12 个有
hint 参与拟合，多数只有 1 条。排查确认**不是实现问题**：
- 「中」的 5 条提示中有 3 条竖提示算下来是整个字身的边框宽（0.9~1.0 em > `max_stem_width`），
  按笔画定义就该拒绝；
- 复杂字（霜/魏）**只有横提示**（19 条），竖提示为 0；
- 把护栏放到几何法的预算（0.5px）后**一条也用不上**——提示边缘常在轮廓 0.5~1.6 物理像素之外。

而放宽护栏会**破坏位图 padding 不变式**（网格是拟合**之前**定的），
实测被 `grid_fit_combines_with_subpixel_without_distortion` 当场抓住（墨迹贴上/下边 + 墨量变化 >6%）。
⇒ 采用已**默认关闭**（`ST_TEXT_USE_FONT_HINTS=1` 开）。要做对，得按 FreeType Adobe 引擎那样
**先把提示吸附到网格、再让轮廓去跟随**（需要"提示 → 轮廓边"的归属，并在提示后重算包围盒）。

**判据**：`fit_hint_stems` 在复杂 CJK 字形上 ≥2（现在多为 0~1），且位图 padding 不变式仍成立。

### ✅ 已修：宽度量化把"设计宽度的小差异"放大成大差异（用户反馈 H/1/B 粗、a/n/l 细）

**用户反馈**（2026-10-06）：「最小字号里 H / 1 / B 的竖线比浏览器粗，a / n / l 的竖线比浏览器细」。

**量化定位**（`tools/stroke_width_stats.py` + `tools/text_ab_glyphs_page.html`，**一行一个字符**，
11px、逐字形量竖笔横截面、取四个高度比例的中位数）：

| | 大写竖笔 | 小写竖笔 | 大写/小写 |
|---|---|---|---|
| 真窗口浏览器（参照） | 1.69 | 1.54 | **1.10** |
| 霜天（量化开） | **2.00** | **1.28** | **1.56** |
| 霜天（量化关 / 拟合关） | 1.42~1.58 | 1.30~1.45 | **1.09** |
| 霜天（修复后 = 加闸门） | 1.42 | 1.30 | **1.09** |

字体给大写 ≈1.7px、小写 ≈1.5px（相差 10%），而 `round()` 把大写推到 **2**、小写推到 **1**
（相差 56%）——**把小差异放大成大差异**，正是用户看到的那一对现象。

**修法**：宽度量化加一道**适用闸** `kMaxQuantizeErrorPx = 0.2`（物理像素）——
取整误差超过它的笔画不量化，只做边缘吸附。
**实测还有一个更强的事实**：关掉量化与开着的输出**逐位相同**（`tools/dbg_guard` 探针已删，
结论记此）——因为"两条边都吸到整数网格"本身就让宽度成了整数差，
量化分支在多数笔画上与纯吸附等价，**它唯一可观察的效果就是那个放大**。

**量尺要求（也是本轮踩到的坑，写下来免得重犯）**：
- 必须**逐字形**：整行平均会把大写与小写混在一起，两者方向相反时正好抵消；
- 必须**一行一个字符**：小字号下相邻字母粘连，按列投影切字形会切错（实测 23 字符只切出 7~9 段）；
- 横截面**不能取竖直中线**：H/E/F 的横杠正好在中线，会把"横杠 + 两根竖"量成一条 5.4px 宽的笔画；
- 判据用**比值**（大写/小写）而不是绝对宽度——绝对值随字体/DPI 变，比值才在两个渲染器之间可比。

**回归**：`quantize_does_not_amplify_design_width_differences`（11px、7 个大写 + 5 个小写的
中位数比 < 1.25）、以及更新后的 `grid_fit_guard_refuses_excessive_shift`（钉住闸门两态）。

## P1（2026-10-06 第二轮：拟合的取舍）

### ✅ 已定：默认改为**不启用拟合**（γ 0.84/0.84/0.88）——以真窗口浏览器为基准逐轴标定

**触发**：用户看过"拟合开 / 拟合关 / 浏览器"三方对比图后判断**「拟合关更均匀」**。

**两个正交指标都支持这个判断**（逐字形、按字类，10~20px）：

| 指标（越低越接近参照） | 拟合 light | **拟合关** | 真窗口浏览器 |
|---|---|---|---|
| 覆盖离散 · 汉字 11/12/13/15px | 0.281/0.282/0.289/0.304 | 0.262/0.257/0.275/0.289 | **0.263/0.261/0.277/0.290** |
| 覆盖离散 · 字母 10/11/12/13px | 0.298/0.309/0.301/0.303 | 0.257/0.292/0.285/0.293 | **0.274/0.292/0.281/0.293** |
| 覆盖离散 · 数字 10/11/12/13px | 0.297/0.304/0.306/0.301 | 0.286/0.298/0.289/0.289 | **0.296/0.298/0.283/0.293** |
| 中间调占比（越低越"实"）· 汉字 11~15px | 0.619/0.600/0.575/0.505 | 0.634/0.644/0.585/0.545 | **0.685/0.670/0.620/0.577** |

⇒ **拟合关与参照几乎逐位重合**；拟合开使覆盖率分布**更离散**（这正是"不均匀"）。

**候选配置**（拟合关 + 三档 γ 重标，两个指标同时对上）：

| 配置 | 墨量比（汉字 11/12/13px） | 覆盖离散 |
|---|---|---|
| 当前默认（拟合 light + γ 0.92/0.98/0.93） | 0.977/0.974/0.977 | 0.264/0.256/0.274 |
| **候选（拟合关 + γ 0.85/0.85/0.88）** | **1.005/0.995/0.995** | **0.265/0.257/0.274** |
| 浏览器 | 1.000 | 0.263/0.261/0.277 |

字母（γ0.85）墨量 0.993~1.007、数字（γ0.85）0.997~1.008，离散也逐档对上。

**✅ 冲突已解开：不是"某把尺子错了"，而是两把尺子量两个正交轴，而浏览器两个轴都在"拟合关"侧**

复跑旧量尺（`tools/text_fit_by_size.cpp`，现行代码、12px 中文）：

| 12px 中文 | 中间调/实心（越低越锐） | 覆盖离散（越低越均匀） |
|---|---|---|
| 拟合开 | 3.131 | 0.287 |
| **拟合关** | **4.241** | **0.260** |
| **浏览器** | **4.099** | **0.264** |

- 旧尺子说"拟合更锐"**是对的**（3.131 < 4.241），但它**没有参照**——
  **"更锐"被当成了"更好"**，而浏览器的实际锐度就在 4.099（拟合关那一档）；
- 两个轴在浏览器上是**同一侧**（都是"拟合关"的值附近），
  所以这两条线**不互相矛盾，而是共同指向"拟合开在这两个轴上都偏离参照"**。

**✅ "拟合对谁真有正收益"已查清——按字号分，不是按字类**（覆盖墨量比，霜天/浏览器）：

| 字号 | 拟合开 | 拟合关 | 更接近浏览器 |
|---|---|---|---|
| 10 / 11 / 12 / 13 / 15px | 1.002 / 1.001 / 1.013 / 1.000 / 0.998 | 0.981 / 0.986 / 0.973 / 0.976 / 0.976 | **拟合开** |
| 20px | 0.977 | 0.985 | 拟合关 |
| 32px | 0.987 | 0.988 | 拟合关（几乎无差） |

⇒ **拟合在 UI 字号段（10~15px）对覆盖墨量是正收益**（把 0.98 拉到 1.00），
**大字号上转为轻微负收益**（≤1% 量级）。这与旧记录"UI 字号是负收益 → 反了"并不冲突：
旧的"负收益"结论量的是锐度轴（见上表），覆盖墨量轴上它一直是正的。

**因此候选方案的准确表述**：不是"关掉拟合"，而是**用类别 γ 补回拟合关掉后的墨量**，
以**牺牲覆盖墨量轴上那 1~2% 的对齐**换取**均匀度轴上的对齐**（0.287→0.260，
浏览器 0.264）。这是一个**轴之间的取舍**，不是"哪个更快"。

**✅ 分段方案已实测量化**（机制已实现：`TextRenderer::set_grid_fit_max_size`，默认 0=不限，
即**当前默认行为不变**）。汉字专项 25 字/档，离散差 = 霜天 − 浏览器（越接近 0 越均匀）：

| 字号 | 拟合不限（现状）<br>墨量比 / 离散差 | 拟合上限 22 物理（=15 逻辑） | 拟合全关 |
|---|---|---|---|
| 11px | 0.995 / **+0.0200** | 0.995 / +0.0200 | 0.983 / **+0.0020** |
| 12px | 1.002 / **+0.0220** | 1.002 / +0.0220 | 0.974 / **−0.0030** |
| 13px | 0.997 / **+0.0122** | 0.997 / +0.0122 | 0.977 / **−0.0020** |
| 15px | 0.997 / +0.0128 | 0.977 / **−0.0006** | 0.977 / −0.0006 |
| 20px | 0.974 / +0.0173 | 0.982 / **+0.0057** | 0.982 / +0.0057 |

**三条可执行结论**：
1. **20px 以上：拟合两个轴都无收益**（墨量 0.974 反而比关掉的 0.982 更差、离散代价 +0.017）
   ⇒ 这一段**该关**，没有争议；
2. **11~15px：拟合确实有墨量收益**（+1.2~2.8%），但**离散代价 +0.012~0.022** —— 真取舍；
3. **上限 22 物理与"全关"在 ≥15px 上数值完全相同** ⇒ 分段点取 **14~15 逻辑 px**
   （物理 21~22，正好落在已有的 `kQuantizeBelowPx = 21` 同一区间，两处口径一致）。

**用户决定（2026-10-06）**：「以浏览器效果为基准调」→ 采用**拟合关 + γ 重标**，
三档 γ 按**三个轴同时对齐浏览器**重扫定为 **0.84 / 0.84 / 0.88**：

| 轴 | 现行默认 | 浏览器 |
|---|---|---|
| 覆盖墨量比（字母 10~13px） | 0.997~1.010 | 1.000 |
| 覆盖墨量比（数字 10~13px） | 0.976~1.001 | 1.000 |
| 覆盖墨量比（汉字 11~15px） | 0.995~1.005 | 1.000 |
| 覆盖离散（汉字） | 差 −0.003~+0.002 | — |
| 边缘柔度（越低越锐） | 3.27~6.66 | 3.22~8.74 |

**"拟合是不是走了错路"——不是错路，是量尺缺了参照**：
4.3.2 当初在**单指标**（"边缘落在整数网格的比例"，中文 90.9%→21.8%）下验收，
**没有外部参照**——"比之前锐"被当成了"够锐"。补上"真窗口浏览器"这个参照后才发现：
**浏览器本身就在"不拟合"那一档**（边缘柔度 3.22~8.74 vs 拟合开的 2.81~4.86——
拟合开其实**过锐**了）。当时的量尺抓到了真现象（笔画边缘确实 100% 落分数相位），
但**缺了"参照在哪"这一半**。

**尾巴（已查清、不追）**：10/11px 一档"边缘柔度"仍偏离参照（霜天 17.99/5.94 vs 浏览器
12.44/8.74）。逐字形看**参照侧本身不稳定**——该区间每个字形只有 **15~26 个实心像素**
（≥15px 有 72~535 个），边缘柔度的**逐字形离散 std 达 14.1 / 11.1**（≥15px 仅 0.2~1.0）。
⇒ 这条偏差**落在参照自身的噪声里**，不是可追的缺陷（追它等于拟合噪声）。

**留下的选项**（未采纳，备查）：
- **A 保持现状**：锐度轴偏锐、均匀度轴偏离散（两个轴都偏离浏览器 ~2~9%）；
- **B 换候选**：均匀度轴对齐（0.260≈0.264）、墨量靠 γ 补回 1.00，
  代价是**锐度轴会偏软**（拟合关的 4.241 略高于浏览器 4.099，差 3.5%）；
- **按字号分段**（小字号拟合、大字号关）：**已量化**（见上表）：
  小字号保住墨量收益、大字号不再付离散代价；代价是 11~13px 的离散仍偏高 +0.012~0.022。
  **机制已实现、默认不启用**（`--text-fit-max-size`，缺省 0 = 不限）。

**状态**：未改任何默认值；候选配置可由探针参数复现
（`--nofit --digit-gamma=0.85 --letter-gamma=0.85 --han-gamma=0.88`）。
建议下一步先把候选接到应用的 `ST_TEXT_*` 环境变量，让用户在**真实应用**里对比，再定默认。

## P1（2026-10-06 第三轮：暗色主题 + SVG 参照链路）

### ✅ 已修：暗色主题下霜天系统性偏轻 5~6%（类 γ 只在亮底标定过）

**现象**（暗底、真窗口浏览器为基准，逐字形墨量比）：

| | 亮底 | 暗底（修前） |
|---|---|---|
| 朴素亮度墨量比（全量字母数字） | 0.996 | **0.942** |
| 同上（汉字专项） | 0.996 | **0.955** |

**两个独立口径一致确认**（覆盖率投影 + 朴素亮度），排除了量尺误差。

**根因**：**覆盖率 → 码值的映射在亮/暗底下不对称**。亮底是"白底黑字"（覆盖率把码值
**压暗**），暗底是"黑底白字"（覆盖率把码值**提亮**），同一个 γ 指数在两个方向上产生的
对比度变化不同。全局 γ **早已**按主题分档（`kDefaultCoverageGamma` 亮 1.10 / 暗 0.60，
注释写明"相差近 2 倍，单一默认值必然错一个主题"），但本轮的**类 γ 分档
（数字/字母/汉字）只在亮底标定过** —— 这一层没跟上。

**修法**：暗底独立标定（`st::app::DarkClassGammas`，按主题自动切换）：

| 类 | 亮底 | **暗底** |
|---|---|---|
| 数字 | 0.84 | **0.64** |
| 字母 | 0.84 | **0.62** |
| 汉字 | 0.88 | **0.75** |

修正后：汉字 0.992~1.005、字母 0.972~1.006、数字 0.988~1.006。

**自检入口**：`ST_TEXT_DEBUG=1` 打印主题 γ、拟合档与三个类 γ 的**实际生效值**
（亮 `theme_gamma=1.10 gamma=0.84/0.84/0.88`、暗 `theme_gamma=0.60 gamma=0.64/0.62/0.75`）。

### 📌 参照链路必须指名道姓：HTML 与 SVG 是不同的链路

**背景**：用户要求"SVG 也对比"。实测**霜天没有 SVG 文本渲染路径**——
`src/ui/svg.cpp` 是**图标子集**（path/基本形状/`<g>`/`<symbol>`/`<use>`/transform），
`<text>`/`<tspan>`/`font-family` **零处理**。所以"霜天渲染 SVG 文本"这条对照不存在。

**能比、且有价值的是**：浏览器自己在这两条链路上差多少（新增 SVG 文本页，与 HTML 页
**同一批行、同一位置、同一字体栈**）：

| 指标 | 浏览器 HTML | 浏览器 SVG |
|---|---|---|
| 全页墨量 | 31,498,501 | 31,498,501（**一字不差**） |
| 彩边强度（\|R−B\|，亚像素特征） | 3.830 | 3.830 |
| 逐像素差异 | — | **9.2% 像素不同**（差异中位 131/255） |

**两条结论**：
1. **"浓淡/覆盖率"轴上两条链路等价**（同字体栈下浏览器对两者执行同一套字形栅格化 +
   LCD 亚像素）⇒ **本轮的 γ 标定不需要按链路重做**；
2. **但"字形落点"不同**（9.2% 像素级偏移）：HTML 用 `top` 定位、SVG 用 `y=基线` 定位，
   **基线取整规则不同** ⇒ 若将来要校准"行内垂直落点"，**必须先说清以哪条链路为准**。

⇒ 这正是 `CONVENTIONS.md §9.2`「参照必须指名道姓」的又一实例：**"浏览器"不够，
要写清"浏览器渲染 HTML 文本"还是"浏览器渲染 SVG 文本"**。

**本轮顺带踩到并修掉的两个"生成物不同源"坑**（都不会让测试变红，只会让量到的差异
变成假差异，故必须靠断言钉住）：
1. 暗色行表**覆盖了亮色行表**（`emit` 给两版写同名 `_rows.inc`）⇒ 行表加主题后缀；
2. SVG 页字体栈**只取第一个族** ⇒ 汉字 fallback 到 `CJK JP` 而 HTML 是 `SC`
   ⇒ 必须用**完整字体栈**。

## P1（2026-10-06 第四轮：SVG 矢量图元对照 → 描边连续性）

### ✅ 已修：描边路径不可跨平台（四平台共用 `stroke_to_path`）→ 一个模块两处硬伤

**触发**：用户要求"对比实际 SVG 渲染"，并直接指出「**圆弧和折线不连续**」。

**现象**（192px、真窗口浏览器为基准，逐例墨量比）：17 例中 16 例在 ±10% 内，
唯一偏差是 `path-arc` 0.896；而按用户指出的两处细看——
单段直线 0.9965、半圆弧 0.936、4 段折线 0.909。

**两个真缺陷**：
1. **绕向抵消**：带四边形是**顺时针**、`Path::add_circle` 的圆是**逆时针**，
   同一路径内非零环绕下**重叠区互相抵消**。判别指纹：强制补圆后墨量**从 0.936 降到 0.640**
   （圆补得越多、墨越少）。它还**掩盖**了缺陷 2 数年——圆头端帽一直被抵消掉，看不出。
2. **线帽/连接语义缺失**：`stroke-linecap`/`stroke-linejoin` **文档写着支持、解析器里零处理**；
   `stroke_to_path` 一律补圆头（违反 SVG/CSS 缺省的 butt+miter）。补角判据是固定
   `cos(25°)` 阈值，而曲线展平后相邻弦夹角只有几度 → **一路不补** → 每顶点外侧
   留 `half×tan(θ/2)` 楔形缺口（几十段累加）。

**改法**（设计层面，已同步 `DESIGN.md §4.3.1.0`）：新增 `raster::LineCap`/`LineJoin`/
`StrokeStyle`，`Surface::stroke_path` 增 `style` 参数（缺省 butt/miter）；
`svg::Style` 增 `line_cap`/`line_join` 且按规范可继承；补角判据由固定角度改为
**缺口深度** `half×tan(θ/2) > 0.05px`（同样 5°，1px 线缺 0.04px 不可见、8px 线缺 0.35px 可见——
角度阈值表达不了）。

**实测**：单段直线 0.9965→**1.0000**、半圆弧 0.936→**0.981**、直角折线 0.909→**0.996**、
SVG 图元 17 例 16/17→**17/17**。

**新增回归**（`tests/raster_stroke_join_test.cpp`，4 条）：直线面积基线 / 弧解析面积 /
直角析线解析面积 / 弧顶列高剖面。**逆向验证**：回退绕向 ⇒ 弧 0.605、剖面凹陷 61.8%，
两条当场变红。
**判据自身也修过一次**：直角折线的解析预期写成"两带 − 重叠 + 半圆补角"（1040），
而 miter 外角是**方形**（960）——判据错会把正确实现判成"缺墨"，这正是"先建对尺子"的实例。

### 📌 待办：`stroke-linecap="square"` 的端面外扩未实现

`Square` 帽按规范应在端点外扩半个线宽；当前只解析了取值，几何未外扩（退化为 butt）。
影响面小（图标库极少用 square），但**语义已声明**，不补就是静默不一致。

## P0（当前：增量重绘恒退化为整帧）

### P0：增量重绘（脏区）**恒退化为整帧** —— 每次交互都重画整屏，22× 的差距没拿到

**现象**：任何交互（改文字 / 悬停 / 勾选框 / 输入）都走整帧重绘。实测 gallery
3533×1921 软件渲染，`metrics.partial_frame` 恒为 `false`、`dirty_rect` 恒为全视口：

| 操作 | partial_frame | 脏区 | paint |
|---|---|---|---|
| 改按钮文字（同宽） | false | 2355×1280 | 19.89 ms |
| 改按钮文字（更长） | false | 2355×1280 | 15.86 ms |
| 按钮 hover | false | 2355×1280 | 15.31 ms |
| 改文本内容 | false | 2355×1280 | 15.50 ms |
| 输入框写值 | false | 2355×1280 | 15.27 ms |
| 勾选复选框 | false | 2355×1280 | 14.58 ms |

**连几何完全不变的 hover / 勾选也整帧**。而 `src/ui/ui_root.cpp:162` 自己记着对照数据：
局部帧中位 **0.69 ms** vs 整帧 **15.0 ms**，单次写 **~22×**。
即“局部没生效”，不是“本来就该整帧”。

**根因链（已逐环打点确认）**：

1. `UiRoot::paint_frame` 的 `use_full` 判定为真，且 `full_flag=1`、`damage=0x0`
   —— 不是“脏区太大”，而是**根本没收到脏区**。
2. `full_flag` 来自 `Element::take_damage().needs_full`。
3. `Element::record_damage()`（`src/ui/element.cpp:405`）在
   `extras_if_any() == nullptr || extras().paint_margin_hint < 0.0f` 时
   将顶层 `damage_needs_full` 置位。
4. 打点看到的实际情况（改 hover 触发的那一帧）：

   ```
   WHOFULL type=CodeEditor id=demo-code-editor margin=-1.00 extras=1 bounds=0x0
   WHOFULL type=DiffView    id=demo-diff         margin=-2.00 extras=0 bounds=0x0
   ```

   **这些元素 `bounds = 0×0`**：它们从未被布局/绘制过（隐藏页、未滚到、刚创建），
   于是 `paint_margin_hint` 保持初始负值。只要任何变更碰到它们，就整帧。
5. 预热期就发生了 **285 次** `needs_full`（`Text 138 / CodeEditor 46 / DiffView 37 /
   Icon 26 / ScrollBar 16`…）—— 大量来自 `bounds=0x0` 的元素。

**方向**（三条互补，需先定设计取舍）：

- **① 零几何元素的“未绘制”不应等于“要求整帧”**。`bounds` 为空/`0×0` 的元素本来就不占像素，
  它的变更不会造成任何可见损坏。最小修法：`bounds_.is_empty()` 时直接 `return`（不置 needs_full）。
  这条收益最大且改动最小——预计能消掉那 285 次里的绝大部分。
- **② 元素首次绘制后的 margin 回填**：把“未绘制 ⇒ 整帧”限定在**首次绘制之前**，
  而不是“脏了就算”。需确认 `paint_margin_hint` 的初始化与回填时序。
- **③ GPU 画布接局部重绘**：`include/st/raster/canvas.hpp:85` 明写“GPU 画布暂不接
  （整帧仅 ~1ms，局部路径要另维护一套状态，收益不划算）”。实测 gbcode(GPU) 整帧
  `paint=2.66 ms`，若接局部可降到 ~0.2 ms——但要实现 `set_present_region` 的 GPU 侧支持。

**⚠ 尝试①时挖到的更深层缺陷（必读，否则会重走一遍）**：

改了 `record_damage()` 的 `bounds_.is_empty() → return` 后，局部路径**第一次真的跑起来了**
（输入框写值：`partial=True`、脏区 2151×156 = 4.9%、paint 15.2→**7.9 ms**），
但**像素校验不通过**：局部重绘后与强制整帧后逐像素比，**41,908 个像素差、最大通道差 132**。

关键定位（这个判据分清了责任）：

| 配置 | 脏区 | 差异像素 |
|---|---|---|
| 原始代码（强制 `use_full=false`） | `0×0`（无脏区） | **0**（因为我根本走不进去） |
| 加上①的修改后 | 2151×156 | **41,908**（**全在脏区内，区外 0**） |

**差异全在脏区之内** ⇒ 脏区算得对，是**局部重绘自身的“清底 + 重画整树”不完整**。
`ui_root.cpp:1219-1235` 的注释声称“`Element::paint` 自带按裁剪域剔除，整树遍历里
留下的正好是与损坏区相交的全部元素”——实测至少有一类绘制没被这个机制盖住。

**下次接手时建议的下一步**：在局部路径里把“哪些元素实际画了”与“哪些元素与脏区相交”
两份清单对下来，找出被漏掉的那类（优先查：阴影/发光这类外扩绘制、
`ScrollView` 的滚动内容、`Overlay`）。在比对通过之前，**不要把①合入**。

**已排除的假设（别重试）**：

- **不是 margin 不够大**：把 `paint_margin` 临时加到 +200px 重测，差异从 41,908
  反升到 **95,366**（脏区本身变大，漏画跟着变大）——说明漏的不是“画到盒子外的内容”。
- **不是坐标/DPI 换算错**：`element.cpp:903-906` 把物理裁剪换算回逻辑坐标的逻辑正确；
  差异全在脏区**内**（区外 0）也印证了脏区本身算得对。
- **不是 `bounds=0×0` 元素被误剔**：`subtree_may_paint` 用 `bounds.inset(-margin)`，
  空 bounds 会变成 margin 大小的矩形，反而**不会**被剔除。

**已测准的特征**（可用来反向定位）：

- 局部重绘只画了 **35 个元素**，整帧画 **102 个**（`metrics.painted_elements`）。
- 差异集中在 **4 个离散行块**：`y=[817,839] / [870,893] / [950,973] / [1003,1016]`，
  每块高 20~24px，x 从 598 开始——**文字行的典型形态**。
- 样本像素：局部后 `(179,179,186)` vs 整帧后 `(192,192,197)`——局部偏深，
  是**旧文字没被清掉盖上了新内容**。

**另一个副作用发现**：`include/st/ui/element.hpp:955` 的 `paint_margin_hint{-1.0f}`
默认值语义是“尚未绘制过”。而 `subtree_may_paint` 在**绘制前**就跑裁剪剔除，
被剔除的元素永远不会回填这个值 → 形成一个环：元素被剔除 ⇒ margin 保持 -1 ⇒
它一标脏就 `needs_full` ⇒ 整帧绘制才回填。这可能是“局部重绘很少能连续生效”的深层原因。

**为什么不只是微优化**：改动不增内存、不改视觉，收益量级（~22×）比光栅化侧所有
剩余优化加起来还大一个数量级。但必须先修好局部路径的完整性。

---

### 2026-10-20 复核：已落地两条，**仍不能走局部**（新取证，下次接着看这里）

已改并保留（都已跑全量测试，0 failed）：

1. **隐藏元素的外扩量回填**（`Element::paint` 入口，在 `if (!visible_) return;` **之前**）。
   旧位置在 visible 之后 → 隐藏元素永远回填不了 `paint_margin_hint` →
   它一标脏就 `needs_full`。
2. **零几何元素不再要求整帧**（`Element::record_damage`：`bounds_.is_empty()` 直接 return）。
   理由硬：`paint_box` 第一行就是 `if (bounds_.is_empty()) return;`，它画不出任何东西。

**修复前/后的量化对照**（`tools/partial_whofull_probe.py`，gbcode 稳态一次改文字）：

| | `WHOFULL`（要求整帧的元素次数） |
|---|---|
| 修复前 | **226** |
| 修复后 | **0** |

所以“285 次 needs_full”那条链已经断了。**但 `partial_frame` 仍然是 false**，故 P0 未完成。

**新查清的事实（本轮最大的收获，别重查）**：`WHOFULL` 归零后，`full=1` 的来源只剩两条：

1. **`pending_full_` 在 `mark_dirty_all()` 里置位**（全树重排/主题切换/overlay 增删）。
   打点确认：gbcode 启动期 **12 次**，**稳态 0 次**——所以它不是稳态的元凶。
2. **`damage_empty=1`（脏区为空 ⇒ 整帧）**——稳态那三帧全部是它。

关于第 2 条，**本轮试错得到一条硬约束，别再试**：

> 给 `mark_layout_dirty()` 加 `record_damage()`（上报本元素 bounds 当作脏区）
> **是错的**，会让既有回归 `damage_layout_change_falls_back_to_full_frame` 变红。
> 那条用例写明了设计意图：**重排会挑动兄弟，脏区必须按整帧算**。
> 窄脏区盖不住被挑动的邻元。

所以“`damage_empty` ⇒ 整帧”对**布局变更**是正确的，不该动。真正该问的是：
**为什么改一个按钮的文字会触发重排**（`set_label` → `mark_layout_dirty`）。
本轮已看到 `layout()` 里的**几何签名**在 gbcode 上是 `changed=0`（重排后矩形完全没变），
即布局跑了但结果不变——那种情况理论上不该要求任何损坏区。

**下次的入手点（本轮已试一条，已否）**：

- ❌ **在 `layout()` 的几何签名不变时清 `pending_full_`** —— 本轮试了，**无效果**：
  `pending_full_` 不是那三帧的成因，`damage_empty` 是独立条件。已回退。
- ✅ **真正该问的**：为什么 `set_label` 会触发重排？`Button::set_label` 无条件调
  `mark_layout_dirty()`，即使新文字与旧文字**宽度相同**（图标按钮的文字不参与布局时更甚）。
  可行方向：文字变更时先 compare 旧/新 measured size，相同则不置 `layout_dirty`。
  这比在 `UiRoot` 侧做任何让步都安全（不碰脏区语义，只少跑一次不必要的重排）。
  判据：`tools/partial_repaint_probe.py` 的 `partial` 列变 True；
  并跑 `tools/partial_repaint_verify.py` 确认像素差异仍为 **0**。

**本轮新增的量具（均已入仓）**：

| 工具 | 作用 |
|---|---|
| `tools/partial_repaint_probe.py` | 逐个操作报 `partial_frame` / 脏区 / `paint_ms` / `painted` |
| `tools/partial_whofull_probe.py` | 区分**启动期 vs 稳态**，分别统计 `WHOFULL` 与 `USEFULL` |
| `tools/partial_repaint_verify.py` | 局部帧 vs 强制整帧的**逐像素**比对（退出码 0/1/2） |

**已排除的假设（本轮实测）**：

- **不是几何签名在变**：`SIGCHANGE` 打点全部 `changed=0`。
- **不是 `mark_dirty_all` 在稳态被调**：稳态 0 次。
- **不是零几何元素了**：已降到 0。
- **不是 `layout_dirty` 元素数多**：`activity-explorer`（44×36，margin=60.5 已知）
  一个就够触发——它 `mark_layout_dirty` 但不上报脏区，于是脏区空。

### P0：高 DPI 下「内容只占左上角」——**已修三条路径，但用户现场仍未消除**

**现象**：用户在高 DPI 屏幕运行 gbcode，界面左侧正常、**右侧与下方大片空白**。
用户两次反馈“还是不行”，附截图两张（逐像素相同）。

**已知的事实（实测，不是推测）**：

| 项 | 结论 |
|---|---|
| DPI **渲染** | 正确。同一逻辑区域 200×24 在三档 scale 下截图 200×24 / 300×36 / 400×48，墨迹 780 / 1593 / 2556（≈面积比例），边缘梯度 221/222/222 |
| 布局逻辑边界 | 三档 scale 下**完全相同**（侧栏恒占视口 19.31%） |
| 用户截图的比值 | `内容 2160 / 画布 2880 = 0.75 = 1.5 / 2.0` ⇒ **画布按 2.0 分配、实际空间只有 1.5** |

**已修的三条路径**（`e691ca21` / `6768cb2e` / `ab91b7a2`）：

1. 初始窗口尺寸写死 1280×800，在高 DPI 小屏（13寸 1080p@200% ⇒ 仅 960×540 逻辑）
   放不下 → 新增 `Backend::work_area_logical()` / `fit_window_to_work_area()`，
   并按新 scale 把逻辑尺寸夹到工作区；侧栏补最小绝对宽 220（纯比例在窄窗口下会把
   标题截成“资…”）。
2. `set_device_scale`（= 应用内「页面缩放」）改窗口前未夹屏幕 → 先夹再改。
3. 改完窗口后**读回客户区校验**，按 `客户区 / 逻辑尺寸` 反算真实 scale（不再把请求值
   当实际值）；`framebuffer()` 每帧守卫「画布 == 客户区」，不一致就重建。

**验收（本机 Win32 真窗口 + 外部 Win32 API 测量，非应用自报）**：

| 步骤 | 画布 | 客户区 |
|---|---|---|
| 启动 1.5x | 2160×1350 | 2160×1350 |
| 切 2.0 | 2880×1800 | 2880×1800 |
| **外部 `SetWindowPos` 强改 1600×900** | **1600×900** | **1600×900** |

三档均 MATCH（兜底生效）。

**为何仍未关：本机复现不出来**。上述三档全部 MATCH，说明用户的触发路径与这台机器
不同（屏幕尺寸/DPI 组合/多显示器/窗口状态）。**下一步必须先拿到**：

- 用户侧的真实输出：`gbcode.exe --log-level debug` 的日志（尤其
  `win32 窗口已创建（逻辑 … 缩放 … 物理 …）` 与 `画布与客户区不一致` 两条）——
  它们会直接告出哪两个尺寸在打架；
- 或者一张**未压缩**截图（当前附件被压缩到 1280×800，边界测量精度不够）。

**别再做的事**：不要凭“应该是某条路径”继续改。已改三条都基于合理推断，而用户
两次反馈无效，说明推断链上还有一环是错的。**先取证再改**。

### 跨编译器严格模式的真实缺口（2026-10-06 编译器对比轮）

**现象**：同一份源码在 **GCC 16.2 下全绿**，而在 **Clang 23 下 92 个源文件里有 19 个编不过**
（`-Werror` 全开）。这不是"clang 太严"，而是**两族对同一处未定义/未指定行为的判断不同**——
这些差异在 GCC 上完全看不到，属于本框架一直在防的那类问题。

**本轮已修完（全部是自家代码的真缺陷，逐个改代码而非加抑制）**：

| 位置 | 问题 | 为何 GCC 不报 |
|---|---|---|
| `src/core/string.cpp` | `is_ascii_digit` 是**死函数**（同文件另有活的那个） | GCC 的未用静态函数告警仅在部分场景触发 |
| `src/text/text.cpp` | lambda `[this, ...]` 捕了 `this` **却从未用** | clang 有 `-Wunused-lambda-capture`，GCC 无对应检查 |
| `src/ui/ui_root.cpp` | `overlays_.begin() + index`（`size_t` → `difference_type` 隐式变号） | GCC 的 `-Wconversion` 不覆盖 `vector::iterator + n` 这条路径 |
| `src/raster/platform_d3d11.cpp` | `point_sample_` 是**从未使用的私有字段** | clang 有 `-Wunused-private-field`，GCC 无 |
| `include/st/ui/components/file_dialog.hpp` + `.cpp` | 双击判定三件套（`kDoubleClickMs`/`last_click_ms_`/`last_click_index_`）**从未接线** | 同上 |
| `src/shell/platform_win32.cpp` | `kResizeBorder`/`kMinClientWidth`/`kMinClientHeight` 是**后来那组同名常量的死副本**（值与用途都重复） | GCC 无 `-Wunused-const-variable` 的等价覆盖 |
| `src/ui/components/tree.cpp` | 文件作用域 `kNoSelection` 被**类内同名成员遮蔽**、从未被使用 | 同上 |
| `include/st/ui/dsl.hpp` | `DeclarativeHost::root_` 存了却从不读（composer 自己持有引用） | clang 有 `-Wunused-private-field` |
| `src/core/platform_net.cpp` / `src/pkg/platform_http.cpp` | `ioctlsocket(handle, FIONBIO, ...)`：`FIONBIO` 展开为 `_IOW(...)`，后者在**系统头内部**把 `sizeof(t)`（size_t）转成 `long` | 转换发生在系统头里，GCC 不报（已改为先取到局部 `long`，让转换有据可查） |

**已落地的机制**：清单 `toolchains.<名>.suppressions`——目标专属**告警收敛**，
追加在工程严格集与档位标志之后，**只用于 vendored 第三方头**（`sqlite3.h` 的 `__int64`、
`quickjs.h` 的旧式转换与 `NAN`）。工程自己的代码与 GCC 同等严格。

**未做（下一步）**：

### 子像素定位（字形落点相位）——**已量化待实施 **

> 本章剩余的未完成项已归位到上方 P1/P2 待做区（2026-10-09 整理）。

## P1

### 文本选中：剩余只读组件的逐个接入（2026-10-14）

**机制已就绪，缺的是逐个接**：公共层已立（`include/st/ui/text_selection.hpp` 的
`TextSelectionModel` + 公共纯函数层），`MarkdownView` / `Text` 已迁移；
`CodeEditor` / `Terminal` / `Input` 自带的实现语义正确、暂不动。

剩这些**只读文本**组件仍是“不可选中”（它们的文字用 `port.draw` 自绘，
接入选区只需填 `TextSelectionHost` 的六个回调 + 转发事件）：

`Table`（单元格文本；表格一行是整体语义，格内字符级可先不做）、`List`（行副标题）、
`Tree`（节点名）、`DiffView` / `MergeView`（代码行）、`TimelineView`（事件标题）、
`FileDialog`（路径）。

**接入前判据**：① 这份文本用户会不会想复制？（会才接——静态装饰文字接了反成干扰）
② 它的行模型能不能给出稳定的 `{行, 行内字节偏移}`（三个坐标系都能映射，但行序要稳）。

**接入后必做的三件事**：① 用**差分判据**量“高亮带 == 字迹带”（溢出 0）；
② 逆向验证（把 `bands()` 改回行盒高，测试必红）；③ 默认**关**选区。

### 文本选中复制能力：剩余文本组件（2026-10-14）

背景：用户要求「框架所有的文本查看和编辑组件都要有很好的文本选中复制能力」。
统一模型与各组件现状已写入 `DESIGN.md` §4.2.17g（必读）。
**全部完成**：`CodeEditor`（早已具备）、`MarkdownView`（字符级）、`Terminal`
（等宽网格 + 回看行号空间）、`Input`/`TextArea`（完整编辑体验）、
`Text`/`Heading`（只读，默认关）。

- [x] **`Terminal`：字符级选区**（等宽网格 `{绝对行号, 列}`；回看缓冲与当前屏
  统一行号空间；宽字符占 2 列；`Ctrl+Shift+C/A`（Ctrl+C/A 留给 shell）、
  双击选词、拖出边界自动回看滚动）。回归 `ui_terminal_selection_test.cpp` +
  真机实测（每 8px 精确少一字符；中文 16px/字）。
- [x] **`Input` / `TextArea`：完整编辑选区**（Shift+方向键/Home/End 扩选、
  双击选词、三击整行/当前行、`Ctrl+A/C/X`、密码框禁用、输入法替换选区、
  只读仍可选可拷）。回归 `ui_input_selection_test.cpp` 12 用例 + 真机实测。
- [x] **`Text` / `Heading`：只读选区**（默认关；`{行, 行内偏移}` + 显示行
  缓存；多行折行每视觉行可选）。回归 `ui_text_selection_readonly_test.cpp`。

**不做**（已评估）：`List` / `Table` / `Tree` / `Menu` / `Tabs` —— 它们的语义是
“选中一项”而非“选中文字”，加文本选区反而干扰交互（行选择已由元素 `selected` 态表达）。
**遗留边界**：`MarkdownView` 的表格单元格仍是**整行选择**（单元格各自折行、
无 `flat` 统一坐标系）；格内字符级选择待后续需要时再评估。

### gbcode 功能实测（2026-10-12，全部有可复现探针）

背景：用真控制通道（tree/get/set/invoke + 真鼠标键盘）＋真 git 仓库工作区＋
clangd 已装环境，对 gbcode 逐功能实测。下面每条都附「现象 → 判据 → 方向」，
数据来自会话内 `gbx/` 探针（客户端 `gbx/client.py`、造场 `gbx/make_ws.py`、
逐条复现 `gbx/repro_all.py` / `gbx/repro_more.py`）。**现有 11 个 e2e 全绿**，
下面 12 条都是它们**没覆盖到的入口**——这正是「e2e 全绿 ≠ 功能可用」的样本。

- [x] **Ctrl+P 快速打开：选中文件后不打开，状态栏却报「已打开」**（最严重）（f6857e7 已修）
  **现象**：Ctrl+P → 过滤 `main.cpp` → 点条目（或 Enter、或 `invoke activate`）→
  面板关闭、状态栏写「已打开 main.cpp」，而**编辑器内容/标签栏/标题栏三者全未变**。
  对照：用资源管理器树点同一文件 → 三者全部正常变化（链路本身可用）。
  **根因**：`run_command()`（page_actions.cpp:2652）里
  `id.rfind("file.open.", 0) == 0` 走 `open_by_name()`，而该函数只在**内置样例表
  `files_`** 里按名字查；Ctrl+P 文件表的 id 是两个都不是的**完整路径**
  （`file_commands()` 里 `.id = full`）→ 两个分支都不命中 → 落到 `run_command`
  末尾的兑底 `status_.set("已打开 " + leaf)`。**失败以「谎报成功」的形式出现**。
  **方向**：文件表条目给独立的 id 前缀（如 `path:`），或 `run_command` 里
  「路径存在 → `open_path`」优先于名字查表；并**禁止兑底分支写「已打开」**
  （未识别 id 应如实报「未处理」）。回归：钉住「Ctrl+P 选中后标签栏新增该项」。

- [x] **逐行暂存没有入口（`diff_staging_` 从未被置真）**（f6857e7 已修）
  **现象**：差异视图打开后，找不到任何「逐行暂存」开关；`#diff-staging-bar` /
  `#diff-stage-selected` / `#diff-staging-close` 永不出现。
  **实测**：`diff-view` 在树上、`hunks=1`，但把差异视图内所有 Button 的 id 列出来，
  无任何 staging/stage-selected 相关项。
  **根因**：`State<bool> diff_staging_` 全仓只有三处引用——`page_build.cpp:628`
  读它、`:655` `set(false)`、`:663` `set_staging_mode`。**从来没有
  `diff_staging_.set(true)`**，也没有菜单/命令面板/快捷键把它置真。
  **代价**：C3 轮已完成的 `build_partial_patch` 数据层与整套 UI（含单测）
  **在实际应用里不可达**，而 README/DESIGN 都写着「逐行暂存界面」已交付。
  **方向**：差异视图工具条加一个开关按钮（或命令面板条目），并配一条 e2e
  「点开关 → 勾选两行 → 暂存 → `git diff --cached` 只含那两行」。

- [x] **SCM 面板刷新不生效：git 数据更新了，但没触发重组**（f6857e7 已修）
  **现象**：点行内「暂存」→ 外部 `git status` 立刻显示已暂存成功，但侧栏「变更」
  面板的**分组/计数/文件行全部不变**（等 6s 也不变）；手工点工具条「刷新状态」
  **也无效**。此时做一次**无关**状态变化（切主题）→ 面板立刻变成正确内容。
  **实测（三步对照）**：基线 `已暂存2/7`；点暂存 + 手动刷新后**仍** `已暂存2/7`；
  切主题后变成 `已暂存2/7 · 未暂存组消失`（正确）。
  **根因**：`refresh_git()` 只写普通成员 `git_` 与 `status_.set(...)`；
  当**变更条数/分支/标签数都没变**（只改了分区）时，`State::set` 的相等早退
  让整个写入无失效 → 没有任何重组 → 面板继续显示上一帧快照。
  （`git_refresh_pending_` 那条自动刷新路径还叠加了下面「终端 busy 恒真」的楼。）
  **方向**：给 git 快照加一个单调递增的版本 `State<std::size_t>`（或直接在
  刷新时写一个必然变化的 key，如 `git_stamp_`），让「数据换了」与「重组发生」
  脱钩；不要在 `refresh_git` 里依赖状态栏文案是否变化。
  回归：钉住「暂存后不切主题，面板也必须变」。

- [x] **终端 `busy` 恒为真：运行面板永远显示「终端忙」+ 自动刷新永不落地**（f6857e7 已修）
  **现象**：终端已回到提示符（连跑两条 `echo` 已结束），`#terminal` 的 `busy`
  仍是 `"true"`；侧栏「运行」面板的忙闲指示恒为 `终端忙` + 持续转圈。
  **根因**：PTY 会话在 `start_pty()` 里 `target.running = true`，此后只在
  **进程退出**时清；shell 活着期间 `running` 恒真 → `any_busy()` 恒真。
  **连带**：`page_build.cpp:64` 的
  `if (git_refresh_pending_ && !terminal_ptr->any_busy())` 因此**永不成立**，
  git 写操作后的自动刷新一条都走不到（与上一条叠加时很难区分）。
  **方向**：分开「shell 进程活着」与「shell 正在执行命令」两个语义
  （如维护一个 `running_command` 计数，只在按下回车到提示符回归之间为真），
  `busy()` 返回后者；或给组件加 `idle_hint()`（屏幕尾行是不是提示符）。

- [x] **`#fs-action` 浮层既关不掉、也拦不住**（f6857e7 已修）
  **现象**：点资源管理器「新建文件/新建文件夹」→ `overlay-fs-action` 浮层打开后：
  Esc（含连按）**关不掉**；鼠标交互**穿透到下层**（点编辑器 `hit=editor`、
  点树 `hit=workspace-tree`）；同时全局快捷键被 `overlay_open()` 守卫挡掉
  （Ctrl+Shift+P 打不开命令面板）→ 界面看上去「僵住」，只能点它自己的 ×。
  **对照**：同工程的 `#find-bar` / `#goto-input` 都被 `main.cpp` 的 `escape`
  全局处理器显式关闭——这条链路漏了 fs-action。
  **方向**：Esc 分支补上 `fs_action_open_`；并决定它该是**模态**（遮罩屏障）
  还是**非模态**（跟查找条一样允许穿透）——现在是两种语义各占一半
  （遮罩画了但不拦、Esc 又不接）。

- [x] **资源管理器「新建文件」会静默清空同名既有文件**（f6857e7 已修）
  **现象**：先用既有内容写好 `d6-victim.txt`（71 字节）→ 「新建文件」输入**同名**
  → 确认后文件被截断为 **0 字节**，界面只报「已打开 …」，**无任何确认**。
  实测（复现脚本 `repro_more.py` 的 D6 段）：71 字节 → 0 字节。
  **根因**：`page_dialogs.cpp` 的新建分支无条件 `st::fs::write_text(path, "")`。
  **方向**：目标已存在时报错或先确认（与关脏标签同一套确认浮层）。
  另：该输入框把 `a/b`、`..`、`CON` 当合法路径/文件名**原样接受**
  （实测会在工作区里造出 `a/` 目录与 `CON` 文件）——需要一份名字/路径校验。

- [x] **clangd 已装但语言服务从未启动（LSP 全链路不可用）**（2026-10-13 修，真根因与条目推测不同）
  **真根因**：不在 ensure_started/which（那条链路是对的）——`--workspace` 命令行启动
  **从未调 `lsp_.set_workspace`**（只有 UI「打开文件夹」路径调），clangd 拿空 rootPath
  起，诊断永不回（实测：clangd 进程活着、25s 零诊断；手动对 clangd 发同样消息 0.0s 回诊断）。
  **修复**：main.cpp 构造 page 后 `set_language_workspace(options.workspace)`。
  **验证**：真仓库 + clangd 18，打开含未定义符号的 .cpp → 编辑器 diagnostics=2、
  状态栏错误计数 2（不需要点击）。

  **环境**：`where clangd` → `…\clang+llvm-23.1.2\bin\clangd.exe`（在 PATH 上）。
  **现象**：打开工作区里的一个 `.cpp`（语言 cpp）、写入未定义符号、等 15s——
  错误计数恒 0、`editor.diagnostics` 恒 `0`、**没有 clangd 进程**；
  `F12` 报「语言服务未就绪」；补全弹层不出现。
  **定位分工**：`st::process::which()` 在 Windows 上会补 `.exe` 并支持 PATH，
  单测可证其正确 → 怀疑在应用侧接线：`sync_language_document()` 只在
  `read_into_buffers` **新建标签的那一帧**被调（`page_build.cpp:232` 的
  `buffers[active].key != loaded_key_` 分支）；而 LSP 泵路径上的
  `sync_document` 还有 `lsp_.active_path() == buffer.path` 一道门。
  **方向**：先在 `ensure_started` 里加临时诊断输出（命中哪个 recipe、which 结果）
  把断点先钉在「哪一步返回 false」；然后补一条**不依赖外部 server** 的回归——
  用仓库已有的 fake LSP server（`tests/lsp_client_test.cpp` 的那套，注意
  坑位表里的 shebang/`ST_REQUIRE` 两条）验证「which 命中 ⇒ `ensure_started` 成功」。

- [x] **问题计数不在打开文件时刷新（内置轻量检查）**（2026-10-13 修）
  **修复**：`refresh_problems()` 收敛为指纹早退（来源标记+缓冲 key+文本/诊断哈希），
  三个时机（装载新文件后 / 编辑回调 / 帧首泵）全部接上且无条件调用；pump 里的
  `if (!diagnostics_changed) return;` 挡板移除。**渲染侧同修**：`problems_` 是普通成员
  不进依赖表，新增 `State problems_revision_`（与 git_revision_ 同款），状态栏计数
  scope 读它——否则「诊断已到编辑器（diagnostics=2）、计数恒 0、点一下才对」（实测复现）。

  **现象**：打开含 3 处真问题（超长行/Tab 缩进/行尾空白，已逐行核对过源文件）的
  `docs/README.md` → 状态栏「错误/警告」计数**连续 5 次采样恒为 `0/0`**、
  `editor.diagnostics` 恒 `0`；**点一下计数按钮才跳到 3**。
  之后切到无问题的 `main.cpp` → 计数从 3 掉到 1（也不对）。
  **根因**：`refresh_problems()` 只在「点计数 / F8」路径调用；
  LSP 泵（`pump_language_service`）只在 `lsp_.pump()` 返回 true 时刷新，
  而**内置轻量检查**（无 LSP 语言的回退）不在那条路径上。
  **方向**：把「文本变化/LSP 诊断变化/切文件」三个时机收敛到一个
  `refresh_problems_if_needed()`（带指纹早退），并在装载新文件后立即跑一次。

- [x] **「清空结果」后摘要仍显示旧命中数**（2026-10-13 修）
  **现象**：搜 `demo` → 摘要 `7 处命中`、10 行结果；点「清空结果」→ 结果行 0、
  输入框仍有词、**摘要仍是 `7 处命中`**（界面自相矛盾）。
  **根因**：`search-clear` 只清 `search_hits_`/`search_hit_paths_`/`last_query_`，
  没重置 `search_summary_`。**方向**：一并清（或改为从 `search_hits_.size()` 派生）。

- [x] **搜索 glob 提示与实际匹配口径不符**（2026-10-13 修）
  **修复**：placeholder 改「文件过滤（如 src/*.cpp、**.cpp、docs/**）」并就地注释
  说明 `*` 不跨 `/`——旧文案举的 `*.cpp` 恰是语义陷阱。

  **现象**：`#search-glob` 的占位提示写「文件过滤（如 *.cpp, src/**）」，但实测：
  `glob=*.cpp` → **0 处命中**；`glob=src/**` → 7 处；`glob=` 空 → 7 处。
  **根因**：`st::fs::match_glob` 的 `*` **不跨 `/`**（坑位表场景 6 有记），
  而提示让人以为 `*.cpp` 能匹配任意深度的 `.cpp`。
  **方向**：二选一——提示改成与语义一致的写法（`**.cpp` 或 `src/*.cpp`），
  或在 glob 里补「无 `/` 的模式按文件名匹配任意层级」这条常用语义。

- [x] **两处「界面在说谎」的键位/入口文案**（2026-10-13 修）
  **修复**：菜单 label/注释改 Ctrl+Alt+S；hover_request 从死代码变 live——命令面板
  新增 nav.hover 条目（鼠标悬停本就有真链路）；page.hpp 注释如实化。

  - 「文件」菜单写 `全部保存（Ctrl+K S）`、`page_actions.cpp:291` 注释同；
    而 `main.cpp:479` 实际注册的是 **Ctrl+Alt+S**（本框架无和弦键位）。
  - `page.hpp:1170` 注释写「悬停信息（入口快捷键 Ctrl+K 用）」，但 `main.cpp:499`
    把 Ctrl+K 绑给了「键盘快捷键」帮助浮层；`hover_request()` **全仓无调用方**
    （死代码），鼠标悬停那条路在无头下无法验证。
  **教训**：同一轮修过「帮助卡写着、代码里没有」那批（见下方「帮助卡 F8 / Ctrl+K」），
  **同一类问题又在菜单文案与头文件注释里长回来了一次**。方向：把这些文案做成
  可测的（菜单 label 里的键位从快捷键表生成），否则它们必然再漂。

- [x] **`invoke` 对未定义动作返回 `ok=true`（只靠 `handled=false` 区分）**（2026-10-13 修）
  **修复**：按本条「方向②」落地——ok 语义不变（兼容），`handled=false` 时响应顶层带
  `error` 说明；DESIGN.md 协议表 invoke 行更新为 `{ok, handled, state?, error?}` 并写明
  「调用方必须读 handled」。

  **现象**：`invoke terminal expand`（Terminal **没有** `expand` 动作）→
  `ok=true`、`handled=false`；`invoke editor no_such_action_zzz` 同。
  本会话实测因此被骗了一次：以为 `terminal expand` 生效，实际面板仍收着
  （`bottom-split.second_hidden` 仍 true）。
  **方向**：要么未定义动作返回 `ok=false` + 错误文案（与「元素不存在」同待遇），
  要么在控制通道响应里把 `handled` 提升为顶层字段并在文档里写明
  「调用方必须读它」。倾向后者（保兼容）＋ 文档显著化。

- [x] **`#explorer-*` / `#scm-refresh` 等 id 在「常驻 + 隐藏」后重复出现**（2026-10-13 修）
  **修复**：① 工具条刷新钮改 id `scm-refresh-toolbar`（同屏同 id=地址歧义）；
  ② 框架层根治：`UiRoot::find` 改**可见优先**——同 id 多实例先取可见者，全不可见
  才盲取第一个（保留「读隐藏元素属性」的兼容）。实测：访问过 SCM 再切回资源管理器后
  `get #explorer-new-file` 返回 visible=true，按 id 点击真实打开浮层。

  **现象**：`find "#explorer-new-file"` 返回 **2** 个（一个 bounds 正常、
  一个 `0x0`）——侧栏视图常驻后，`build_sidebar_actions` 在同一帧里
  按 `activity_` 只走一个分支，但**上一帧的分支子树仍在树上**（按 key 复用），
  于是同一 id 有多份。`#scm-refresh` 更是同时存在「侧栏头 24x30」与
  「工具条 26x28」两份**同 id 的可见按钮**（`find` 实测返回 **3** 份：
  `(196,50) 24x30`、`(280,50) 24x30`、`(54,112) 26x28`；
  另：按 y 坐标点 `(280,50)` 那份时 `hit` 报的是 `sidebar-title`——两者矩形重叠）。
  三份的点击行为都接同一个 `refresh_git()`，但自动化命中哪一份不确定。
  **代价**：e2e / 控制通道用 id 驱动时命中不确定的一份（本会话第一次点
  `#explorer-new-file` 就点到了 0x0 的那份，表现为「点了没反应」）。
  **方向**：给按视图切换的按钮 id 加视图前缀（`explorer-new-file` 已是），
  真正的问题是**同一视图的两处刷新钮同 id**——把工具条那个改名为
  `scm-refresh-toolbar`；另外把「骨架里 0x0 的隐藏残留」查清（是否该裁掉）。

- [x] **测试分片崩溃 = 静默丢测试通道（“分片未能启动 + 空原因”）**（2026-10-13 修）
  **修复**：`run_shards` 两处——启动失败带片号/总数与非空原因兜底；「非零退出 +
  未写出 JUnit 报告」（= 进程 abort/崩溃，片内后续用例未跑）显式报错并附单跑命令。

  **现象**：`st test --profile debug` 跑 16 个分片，每片都 `0 failed`，
  但整体**退出码 1**，末尾只有一行 `测试分片 2 未能启动: `（原因字符串为空）。
  逐条缩小后定位到 `tests/text_diff_test.cpp` 的 `text[range.second]`：
  `diff_inline` 返回的是**半开区间** `[6,12)`，而 `"你好世界"` 正好 12 字节
  ⇒ 下标 == `size()` ⇒ `operator[]` 越界；`_GLIBCXX_ASSERTIONS` 下直接 `abort`。
  **代价（这条比缺陷本身重要）**：一个用例越界就让**整个分片进程消失**，
  该分片后面 100+ 条用例一条未跑；而 16 个分片的汇总行统统是 `0 failed`——
  只看汇总就会当它全绿（本会话第一次跑就被这么骗过去了）。
  用例本身已修（右端到末尾即算合法边界）；全量重跑 1301 条 / 0 失败 / 无“未能启动”。
  **方向（框架侧未做）**：`run_shards` 在 `exit_codes[index] < 0` 时把子进程的
  **退出码**也打出来（现在只打了一个常为空的 `error().message`），
  并把「分片启动失败/异常退出」计入总失败数（现在只打印，不影响退出码的语义说明）。

- [x] **`--scale` / `--frames` 传非数字时进程直接崩（未捕获异常）**（2026-10-13 修）
  **修复**：stod/stoi 全部换带校验解析（含尾部垃圾如 `1.5x`），非法值与未知参数同路径
  （报错+exit 2）；`--scale` 夹 [0.1,10]；`--control-port` 范围校验；run_app 里两处
  裸 stod（--editor-font-scale/--editor-line-spacing）同修。实测：`--scale abc` → exit 2。

  **现象**：`gbcode --headless --scale abc --frames 3` →
  `terminate called after throwing an instance of 'std::invalid_argument' what(): stod`，
  退出码 `0xC0000409`（`--frames abc` 同理，`what(): stoi`）。
  而 `--bogus-flag` 走的是**正常路径**（退出 2 + 提示「未知参数…（用 --help 看可用项）」）。
  **方向**：common options 解析里把 `stod`/`stoi` 包上校验，非法值走与未知参数
  同一条路径（报错 + 退出 2）；顺带把 `--scale 0` / `--scale -2` 夹取到一个合法区间。

- [ ] **粗体文本按常规面量宽（修 `bold` 缓存键后拓出来的）**
  **现象**：`TextPort::measure_width` 的接口面**没有 `bold`**，所以布局（`Text::measure`）
  按常规面量宽、绘制（`paint_text` 的 `port.draw(..., bold=true)`）按粗体面画。
  两面字宽不等时，粗体文本的**居中/换行/省略号位置**有系统性偏差。
  **实测数据**：15px 中英混排「概览 Bold混合ABC」——`measure(bold=false)=126.973`
  vs `shape(bold=true).width=130.620`，**差 3.647px**（同一渲染器、同一串）。
  端到端证据：修 `bold` 缓存键后，页标题、统计卡大数字、按钮行、
  状态栏都出现像素变化（整帧 46475 个像素）——那是字面宽度变了的必然结果。
  **方向**：给 `TextPort` 加带字重的度量入口（`measure_width(..., FontWeight)` 或
  `measure(..., bool bold)`），`NullTextPort` / `RendererTextPort` / 测试桩三处跟进，
  `Element::paint_text` 与 `Text::measure` 走同一个字重。
  注意桩面（`tests/support/text_port_fixtures.hpp`）也要同步，否则量尺与实现分家。

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

- [x] **gbcode「平替 VSCode」第三批：编辑手感 + 视口可驱动 + 三个真缺陷** —— 2026-10-03

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

  **示例侧（gbcode）UX**：私有 `CommandPalette` 退役改用框架组件（−130 行）；
  **Ctrl+P 真实快速打开**（把面板表换成文件表，88 个文件里打字过滤）；
  搜索面板改**真实递归搜工作区**（跳隐藏目录/二进制/大文件，命中给「文件:行 + 片段」，
  点一下**跳到该行并选中命中**）；问题面板接**真实轻量检查**（行尾空白 / Tab 缩进 /
  TODO·FIXME / 超长行，点问题跳行），状态栏错误·警告计数与面板**同源**；
  状态栏补选区计数；标题栏加脏点（`window_title()` 单一来源，7 处赋值收敛）；
  终端补 `ls`/`find <词>`/`goto <行>`/`stats` 真实命令；查找条 Esc 可关（此前关不掉）。

  **验证**：`st test` **583 用例全绿**（+26）/ `st lint` 0 违规 /
  `tools/st_visual_check.py` **dev+san × gallery+gbcode × 亮/暗 × DPI 2.0 全部 0 失败步、
  无 sanitizer 报告** / mingw 交叉编译通过 / `tools/caret_ink_probe.cpp` 作为量尺入库。

- [x] **gbcode「平替 VSCode」第二批：查找替换 + 真实文件工作区 + 命令面板内置** —— 2026-10-03
  - `CodeEditor` find/replace 组件能力：`set_find`（全部命中高亮，主题新增 find_highlight/
    find_active 两枚 token）/`find_next`（环绕、就近起步）/`replace_current`（替换后跳下一
    命中）/`replace_all`（走撤销栈可回滚）；编辑后命中表保持重建；动作面
    find/find_next/find_prev/replace/replace_all/replace 进协议白名单；属性面
    find_needle/find_matches/find_active。7 用例。
  - `CommandPalette` 内置为框架组件（此前 backlog 挂账）：数据驱动（Command 表）+
    大小写不敏感过滤（title/detail）+ 键盘环绕导航 + Esc/遮罩关闭 + 属性面/动作面
    （select 支持序号与命令 id）；7 用例。gbcode 示例的私有实现退役（下批迁移调用点）。
  - gbcode `--workspace <dir>`：真实文件模式——资源树 `fs::list_dir` 扫描（目录增量
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
  （17 图标 ×16/32/64px 三档对照）；gbcode 活动栏/标题栏/状态栏图标切 SVG。
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
  端到端（真实 gbcode 截图逐区域量）：侧栏 12px 半覆盖 **−9%**、实心 **+10%**、墨量 +1.7%。
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
     （见已删的 `tools/text_phase_sensitivity.cpp` 的结论，后续由 `tools/glyph_phase_probe.cpp` 接手），所以收益主要在**字距均匀度**
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
 - [x] **script_host 提交链仍调 `mark_dirty_all`**（控制通道路径已改损坏区驱动）——
   2026-10-15 已收敛（提交 8f3fd00）：`commit_changes` 删尾部整帧标脏，逐元素路径
   （apply_properties/invoke_element/apply_layout_property）已自带精确标脏；
   回归 `script_write_takes_partial_damage_path`（JS 写单元素 → 局部损坏区重绘）。
   同轮拓出并修复 shard_parity_test 硬编码全局注册序的脆弱性（早链接 TU 正常
   新增用例即误击，改观察推导）。
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
  - [x] **IDE 形态界面声明式重写**（2026-10-03 示例整合）：`examples/gbcode` 整份界面
    改为声明式组装（五层布局 + 多标签 + 真实文件工作区 + 查找替换 + 菜单栏下拉 +
    命令面板 + 全局快捷键 + 终端 + 主题），与原本的 `examples/gbcode-dsl` 分身**合并为一份**
    （两份合计 2758 行 → 1605 行，−42%）；端到端 `tools/gbcode_e2e.py` 九项。
    同一批整合把 `counter` / `counter-js` / `todo-js` 三个小示例删除（语义已由单测完整覆盖）、
    gallery 新增「声明式」页（`mount_into` 子树挂载）；示例收敛为 gallery + gbcode 两个。
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
  - [x] 旧版 `examples/gbcode` 编译修复（三处 `-Werror=shadow`）。
  - [x] **`SplitView` 分隔线偏移修正**（用户报修）：`paint_content` 里 `handle_rect()`
    返回的已是**绝对坐标**，旧代码又加了一次 `bounds_.x`——线整整偏出一个 `bounds_.x`，
    在 gbcode 里表现为“标签下方一条穿过代码行号栏的竖线”（分栏 x=44 时线落到
    361.66，而两面板交界只在 314..322）。纵向分支、`grip_rect` 同错。
    回归测试 `ui_split_view_divider_paints_between_panes`：**宿主必须把分栏推到
    非原点**——`bounds_.x == 0` 时“多加一次”与不加等价，缺陷不会显形
    （第一版测试就落在原点，回退修复后仍然绿，是个假绿）。
  - [x] **gbcode 标题栏的窗口控制未右对齐**：重写时把原实现的
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
      （gbcode 整页声明式每帧在付这笔钱）。根因：复用分支无条件
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

### 从其它章归位（本次整理；原章声明已失效，条目内容未改）

- [ ] **字形笔位被 `lround` 取整 → 字距随相位跳**（指导书 §1.1）。
  > ⚠ 回滚记（2026-10-16）：曾落地「相位档」修复（eb86f72）并在此标 x，但真窗口
  > 观感**变差**（用户实测）——量化档位在小字上产生字形形状随档位微变的抖动感，
  > 墨心 σ 改善未能兑换成人眼收益。已整体回滚（代码/测试/量尺/文档），恢复待做。
  > 重做前必读：DESIGN §4.3.3 曾记录的实现铁律（平移在 grid_fit 后/锢定不变式/
  > phase 进缓存键）与 `.evolve/backup-v22-full-workspace.patch` 里的完整实现——
  > 下一次尝试要么拿到可信参照后再改，要么改用分档位图缓存以外的路径。
  `TextRenderer::draw` 的落点是 `x_begin = lround((origin.x + run.x) * device_scale) + offset_x`：
  整形保留的浮点笔位在**绘制时被抹掉**。实测（`tools/glyph_phase_probe.cpp`，1.5× DPI / 13.5px /
  4 段真实 UI 串 / 200 字形）：

  | 判据 | 实测 |
  |---|---|
  | 笔位相位分布（8 桶） | 8.5%~18%（接近均匀）→ **笔位本身是浮点，是绘制把它取整了** |
  | 字形被取整挪动量（均值） | **0.235px**（均匀相位理论值 0.250px） |
  | **相邻字距误差标准差** | **0.406px**（最坏 **0.905px**） |
  | 保留 1/4 像素相位后 | ≤ 0.125px； 1/64 相位 ≤ 0.008px |

- [ ] **下一步（方向已探明）**：正确方向是**加墨 + 拉陡过渡带**（γ<1 一侧，即 ClearType/Skia
  `SkMaskGamma` 干的事）。实测 γ=0.7：实心占比 10.8% → **11.8%**、过渡带 13.1% → **12.4%**
  （同时变实且变陡）。**改默认值前必须先拿可信参照**（真机非无头浏览器 / 系统 ClearType 输出），
  验收用**朴素像素口径 + 过渡带宽度双判据**。

  - [ ] **虚拟化长列表**：终端/输出面板的 ScrollView+Text 累积全文，日志长了退化；

- [ ] **`bench_thresholds_have_bounded_headroom` 余量卡在上限上，重编即误报**（待修）：
  `kMaxHeadroom = 3.5`（自检「阈值/实测 ≤ 3.5」），而实际余量在 **3.46~3.51** 之间——
  即**踩在上限上**。实测：在同一工作区**加一行纯注释**（零语义变化）触发重编，
  bench 就从 6 passed 变成 4 passed/2 failed（连跑 3 次稳定复现；去掉注释又全绿）。
  这不是测试写得假，而是它想卡的两头（“太松抓不住退化”与“太紧噪声即误报”）本来就窄，
  而“重编改变代码布局→实测比值漂移”未被纳入余量预算。
  影响：**任何不相关改动重编后都可能看到 1~2 条 bench 红灯**，容易被当成回归误判（本轮就撞到了）。
  方向：或把上限放宽到 4.0 并同时收紧阈值（保持“抓得住 3 倍退化”），
  或对同一度量取多次最小值（减少布局敏感），或把该自检标记为“需人工复核”而非硬断言。

  **2026-10-14 已修（本条的前半“余量踩线”仍然存在，下面记的是新增的两条事实）**：

  ### ① 真正被这条门禁抓住的真回归（已修）

  上面的“加一行注释重编就红”是**环境噪声**；但继续查下去发现了一件严重的事：
  这条门禁（连同 `bench_rounded_card_fill`）**一直在正确地报一个真实的性能回归，只是没人看**。

  | 圆角卡片填充（同几何，release，`tools/rounded_fill_probe.cpp`）| 比值 |
  |---|---|
  | `b420909` 之前（逐段混合，b9166d5 的“运行段”版）| **1.32×**（即注释里写的“实测 ≈1.35×”） |
  | `b420909` 之后（整行逐像素累加）| **26.7×**（**20 倍回归**）|

  根因：`b9166d5`（渲染 5× 轮）用“**运行段 + `merge_runs` 合并**”替掉了逐像素累加
  （圆角 10.12→0.61ms），而 `b420909` 修“圆角发虚”时**又在 `blend_coverage_runs` 里加回了
  整行逐像素累加**：它与 `merge_runs` **功能重复**，并且**遮蔽了 `simd::blend_row` 的
  full-span 快路径**，把 O(段数) 退回 O(像素数)。

  **修法（保留正确的语义，恢复快路径）**：仍然是“先按像素累加覆盖率、同一像素只混一次”
  （那是 `b420909` 修对的），但累加后**按量化后的 alpha 字节分组**，同值的极大段走 `simd::blend_row`——
  逐像素结果与旧版相同（量化口径逐字一致），而段数 ≤ 256（典型 2~4 段）。
  另加 `runs_share_pixels` 路由：**只有相邻 run 可能共享像素时才走累加**（互不相交的 run——
  如矩形的两条竖边——根本不存在“同像素被碰两次”，白走一趟反而更慢）。

  验证（三条独立判据，缺一不可）：
  1. **正确性不变**：像素级 A/B（`tools/raster_pixel_diff.py`）——HEAD 版 vs 修复版
     同一画廊场景截图 **248 像素差**，而同一二进制自身两次截图的噪声就是 **232 像素差**
     → 差异全在噪声量级；`diagonal_band_ink_is_angle_independent` 45° 比 1.0017（修复前 0.8568）；
     环总墨量 0.9988/1.0000 保持。
  2. **性能恢复**：release 档 `bench_rounded_card_fill` **26.7× → 1.70~2.11×**（阈值 3×）。
  3. **逆向验证（双向）**：禁用累加路径 → 斜带墨量掉到 0.83~0.92（欠墨，即“圆角发虚”的原形）
     → 用例变红；禁用整个快路径 → 回到整行累加语义、性能掉到 30ms。**两条路径各自都 load-bearing。**

  ### ② 比值不跨优化档稳定（已收口）

  上述修复后仍有一半的档位红：同一份代码 release 圆角 **2.03×** / dev **3.23×**（文本反向：release 26.96× / dev 18.74×），
  因为分母（清屏）是可向量化的 `memset`、而被测负载是标量循环，两者对 `-O` 的响应不同——
  而**本文件的阈值与“实测”注释全是 release 档标定的**。在 dev 档下拿 release 的阈值卡，
  **两个方向都会错**（圆角偏大 → 误报；投影/渐变偏小 → 自检“余量超上限”红灯）。

  收口口径与已有的 **san 档**完全一致（不把阈值按档放宽——那会把真 release 门禁变松）：
  新增 `calibration_profile()`（`NDEBUG && __OPTIMIZE__`，即本仓的 `release` 档，见 `profile_flags`），
  非标定档下**如实跳过比值型断言并在报告里说明**。`app_frame_budget_test.cpp` 同改。
  跑门禁：**`st test --profile release --slow`**。

  ⚠ 遗留：`calibration_profile()` 依赖 `profile_flags` 只给 `release` 发 `-DNDEBUG`——
  那是**对构建系统档位定义的耦合**，改档位时要搜 `NDEBUG` 复核。

  ⚠ 本条**前半**（`kMaxHeadroom` 余量与实测贴边、重编即漂移）仍未解决：现在四个负载在
  release 下的余量是 1.42~2.84（上限 3.5），比原前的 3.46~3.51 宽松了些（因为圆角那条从
  “踩线”回到正常），但“重编改变代码布局→比值漂移”仍可能把一条推到边界之外。
  真正的解法是把基准单位从“清屏”换成一个**有代表性成本**的量（如一次 `fill_rect` + 混合）。

  📊 **Windows 宿主基线（2026-10-15 实测，dev 档，与帧门禁问题同族）**：4 条 bench
  门禁在 Windows **全绿**（Linux 容器当时全红——再次坐实“容器不能作判据环境”）：
  圆角 3.39×（阈 3）/ 投影 7.33×（阈 24）/ 文本 32.08×（阈 50）/ 渐变 1.20×（阈 4）。
  但**投影的故障检测贴边**：3 倍退化 23.48× vs 阈值 24× → **抓不住**（自检如实打印但
  未断言；与帧门禁同族：比值口径跨平台/跨档不稳，阈值标定只覆盖了单点）。
  同族问题的完整分析与方案 A/B/C 见「帧门禁在 Windows 双档恒红」条目（2026-10-15）。

> 审视报告「第一梯队」与「第二梯队」（含渲染侧：增量重绘/整形缓存/LRU/渐变快路径）
> 均已在上述条目落地；无头环境无法验证的窗口模式 vsync 如实留在 P1。


## P1（2026-10-06 gbcode 全面优化轮：框架层缺口）

背景：把 `examples/gbcode` 做到“真正可用”（对标歌白文件工作台）时挖出的框架问题。
**已当场修的三项**（见各自段落），**仍未做的在下面**。

### ✅ 已修：`set_event_handler` 对覆写了 `on_event` 的组件静默失效

- **现象**：`set_event_handler` 的文档写着“在组件自身实现**之后**追加一次回调”，
  但**没有任何组件覆写真的调它**，分发层也没调。于是覆写了 `on_event` 的组件
  （`Input`/`Button`/`CodeEditor`… 二十多个）上装的处理器**永远不被调用**：
  调用方看到的是“设了处理器、无报错、无效果”。
- **实测**：终端输入框装 ↑↓ 翻历史 → 按 ↑ 毫无反应；而同一份处理器装在自定义元素上
  正常（所以旧的单测全绿）。
- **修法**：分发层（`UiRoot::dispatch_to`）在组件未消费之后统一调
  `invoke_event_handler`。回归：`ui_shortcut_test.cpp:
  event_handler_reaches_components_that_override_on_event`（回退修复后变红）。

### ✅ 已修：`ScrollView` 无“跟随底部”语义（日志/终端类场景永远差几行）

- **现象**：想让滚回窗自动跟在底部，只能调 `scroll_to(1e9)`。而 `max_scroll()` 取的是
  **上一次布局**的 `content_height_`——刚追加内容就调，会被**旧上限夹住**；
  等布局把内容量成新值并重新夹取时，它已是一个合法偏移，没人知道它本该在底部。
- **实测**：终端滚回内容 331.8px、旧上限 175.6 → 停在 175.6，**尾部两行永远看不到**；
  无论怎么调都差“新追加的那几行”（试过拿上一帧 max 比、置标志下帧滚，都不对）。
- **修法**：`ScrollView::set_follow_end(bool)` —— 在 `arrange` 里量完新几何、
  归一化偏移后立即回到底（“在底部”是持续意图，不是一次性动作）；配 `at_end()` 判据。
  回归：`ui_scroll_test.cpp: scroll_view_follow_end_pins_after_content_grows`
  （回退修复后变红：期望 1100、实际 0）。

### ✅ 已修：`Text` 默认单行——多行日志/输出需要显式 `set_multiline`

- **现象**：`Text` 的缺省是“单行省略”，用它显示滚回文本时整份日志被折成**一行**，
  看着就像“输出只有一行”（不加任何报错，内容也没丢——只是全在一行里）。
- **判定**：不是缺陷（单行是 `Text` 的合理默认），但**缺一条“何时必须开多行”的说明**。
  已在 `DESIGN.md` 的 `ScrollView` 段落里补上“容器内容用 Text 时必须 `set_multiline`”。

### ✅ 已修：`Button::measure` 忽略宽度约束（窄容器里横向溢出）

- **现象**：侧栏（270px）里的 Git 变更项是 `Button`，长路径**画到编辑区上面**，
  看起来像绘制错乱。
- **实测**：`#scm-host` 内条目的 `bounds` 右缘超出侧栏右缘 100+ px；
  `Button::measure` 首行是 `(void)constraints;`——完全无视可用宽度。
- **修法**：`measure` 夹到 `constraints.max_width`；`paint_content` 先 `ellipsize` 再量宽。
  与 `Text`/`Tabs` 等同口径（“显式宽度优先、否则听约束”）。

### ✅ 已修：`Spacer` 在矮容器里给出**负高度**矩形

- **现象**：`grow` 的 `Spacer` 在 30px 高的行里 `bounds.height == −18`。
- **修法**：`Spacer::arrange` 把副轴尺寸夹到非负（占位块没有内容，“多大”不该是负数）。
- **为何值得修**：负尺寸会被协议/测试当成异常值读走，也让任何“区域面积”类的断言失效。

### ✅ 已修：`CodeEditor` 吞掉右键（宿主无法做右键菜单）

- **现象**：`on_event` 对**任何按钮**的 `MouseDown` 都进“把光标搬到点的位置”
  分支并返回 `true`——宿主既收不到右键，也阻止不了“右键改了光标”。
- **修法**：新增 `CodeEditor::on_context_menu`（一等回调）；右键分支先于滚动条判定，
  **不改光标**（右键不改选择，与主流编辑器一致）。
- 回归：右键不动光标（实测 cursor 108 → 108）。



## 已完成（2026-10-06 主题：亚克力黑白质感 + 自定义主题）

用户要求：① 全面提升默认主题（亮/暗）的质感；② 支持主题自定义；
③ 方向明确为「**默认亚克力黑白质感，参考歌白**」。

### 做了什么

- **调色板换成亚克力黑白**（参考 `packages/web/src/themes/acrylic.css`）：
  亮 = 白色亚克力（`#E8E8EE` 底 + `#FAFAFC` 面板），暗 = 黑色亚克力
  （`#08080A` 底 → `#15151A` → `#1F1F26` → `#25252D` 逐级抬升）。
  第一版把中性色也放进 220° 蓝相（饱和度 47~63%），整个界面发蓝——**亚克力的高级感
  来自近中性的低饱和**，已改为饱和度 <25% 的近黑/近灰。
- **自绘管线没有 `backdrop-filter`**：把歌白的半透明面板**预合成成实色**
  （`rgba(255,255,255,0.8)` 叠在 `#E8E8EE` = `#FAFAFC`）；玻璃感改由新增的
  `highlight` 令牌（顶部内高光）+ 阴影重建。
- **三处新增令牌**：`surface_raised`（弹层底色）、`border_subtle`（第三档分隔线）、
  `highlight`（玻璃边缘）；两枚尺度旋钮 `shadow_strength`/`shadow_spread`。
- **`Style::top_highlight`**：由 `Element::paint_box` 统一绘制（横向渐变、两端 alpha 归零，
  再裁到圆角内），组件只需填字段。
- **弹层抬升三件套** `paint_raised_surface`（阴影 → `surface_raised` 底 → 玻璃边缘），
  菜单/对话框改走它（此前五处各拄一份「画阴影 + 填底」）。
- **主题自定义**：新 `st/ui/theme_io.hpp`（稀疏覆盖 JSON）+ 令牌名表
  （`palette_token_names` / `metric_token_names`，成员指针表保证名与取址不漂移）
  + `--theme-file` / `ST_THEME_FILE` + 控制通道 `theme` 的 `set` / `tokens` 分支。

### 三层来源与两个**实测踩到的**合成缺陷

优先级：内置基准 → 主题文件 → 运行期 `theme.set`。两处都在真进程里才发现：

1. **`base` 是基准选择器，不是覆盖项**。第一版把它留在 JSON 里交给覆盖层处理，
   而覆盖层会被「base 必须与当前模式一致」拒绝——于是文件里写着 `base:"dark"` 时，
   一旦切到亮色就**整份文件覆盖都被丢掉**（用户看到「我的配色全没了」）。
   修法：装载时一次消费掉 `base`，合成时跳过它。
2. **运行期改色必须累加**。第一版 `theme.set` 直接替换整份运行期配置，
   于是分两次发 `colors` 与 `metrics` 时后一笔抹掉前一笔；
   而切模式时又直接重读文件，把运行期改色也抹掉。
   修法：新增 `merge_overrides` + `compose_theme_with`，一切改变生效主题的动作走同一条合成。

### 验证

- `tests/ui_theme_io_test.cpp`（14 条：令牌名表往返、颜色语法、失败安全、累加、
  base 一致/不一致）+ `ui_theme_test.cpp` 新增 5 条（表面阶梯、抬升面、分隔线、
  玻璃边缘、阴影旋钮、主题名）；
- **`tools/theme_reverse_verify.py`：逐条回退改动、断言必须变红**。
  它抓出了**两条恒绿的假护栏**：
  ① `surface_sunken` 那条断言的阀值（1.05）太松——旧值也有 1.077，拦不住回退；
  ② alpha 小数那一条的回退文本**编译不过**（注释掉了唯一使用 `is_fraction` 的语句
  → `-Werror` 报未使用），于是测试跑的是旧二进制、假绿。
  两条都已修（阀值提到 1.08；回退文本改成 `*= 1.0` 仍能编过），现 7/7 全红。
- `tools/theme_file_e2e.py`：真进程验 `--theme-file` 与 `ST_THEME_FILE` 两条装载路径；
- `st test` 794 继 1 继（唯一红的是**既有环境问题**：`font_default_text_uses_one_family_for_latin_and_han`，
  fontconfig 下拉丁/汉字落到不同 face，与本轮零关联，改动前就红）；
- `st lint` 0 违反 / `check_docs.py` OK / mingw 交叉编译通过。

### 同时记下的两个**框架级**问题（本轮未修）

#### ① 增量构建会漏掉改动（P1，影响所有开发）

本会话至少撞到四次：改了 `.cpp` 后 `st test` / `st build` 报「重编 0 / 命中缓存 88」，
实际跑的是**旧二进制**（`build/debug/obj/*.o` 的时间戳比源码新但内容旧）。
实证：`src/ui/theme.cpp` 在 17:10 改成 `highlight = 0xFFFFFF00`，而
`build/debug/obj/..._src_ui_theme.cpp.o` 停在 17:11 的旧内容，于是断言
`light_theme.colors().highlight.a == 0U` 假红；`rm -f` 掉 `.o` 后立刻转绿。
**危害**：这会制造两类错结论——“改完还是红的”（其实没编）与反过来的假绿，
而本轮逆向验证脚本自己就踩了第二种。
**方向**：缓存命中判定除 mtime 外应纳入**源码内容哈希**（当前共享对象缓存已有 `fingerprint`，
但 profile 内的 `.o` 偷懒只看时间）；至少要在“源码比 `.o` 新”时强制重编。

#### ② `st test` 的失败输出会盖掉断言位置（P2）

过滤跑单个用例时失败只报 `0 passed, 1 failed, N assertions`，不带断言文件:行号；
而全量跑时反而有。定位一个循环里的多个子用例（如颜色语法的 7 个写法）
靠现有输出无法区分是哪一个（本轮临时在测试里 `st::print` 才定位到）。
**方向**：失败时无论过滤与否都打印文档与行号；循环内断言建议带参数化消息。

## 已完成（2026-10-06 交互态：悬浮/按下的语义修正 + 一处 use-after-free）

在做主题截图验收时，把每个交互态都量了一遍像素——**量出三个真缺陷**。
三个都不是本轮主题改动引入的，但都被这次量测暴露。

### ① 悬浮把按钮的语气抹平（`Element::paint_box`）

**现象**：暗色下主按钮 `#8b9aff` 悬停后变成 `#2b2b30`——纯中性灰，看起来像"被禁用"。

**根因**：`background.mix(colors.surface_hover, hover_t)`，而 `hover_t` 会收敛到 1，
`mix(..., 1.0)` 就是**完全替换**。代码紧邻的注释写着"不能把它们（Ghost/Soft）的语气抹平"，
而实现恰好就在抹平——注释与实现自相矛盾。

**修法**：改为**同向提亮/压暗**，幅度取 `surface_hover` 相对 `surface` 的**实际明度偏移**
（两侧在亚克力调色板里都是实色）——命中 `surface` 时正好等于主题定的悬浮档，
而主按钮只差一档明度、色相不变。

⚠ 踩过的坑：第一版用 `surface_hover.a / 255` 当幅度，但 alpha 现在恒为 1，
于是幅度是 0.45——实测把 `#15151a` 直接抬到 `#7e7e81`（中性灰）。
**幅度必须从实色的明度差算，不能从 alpha 算**。

### ② 按下态完全不生效（`Button::apply_theme` + `UiRoot::layout`）

**现象**：按下与悬停的像素**完全相同**，没有任何"按下去了"的反馈。

**根因**：`Button::apply_theme` 里的 `pressed_ ? primary_active : hovered_ ? primary_hover : primary`
分支是死代码——`apply_theme` **只在 layout 时跑**（`UiRoot::layout` 早退条件 + `DESIGN.md` §4.2.6），
而悬停/按下只标重绘、不触发布局，于是那些分支算出的色**永远停在旧状态**。

**修法**：三处收敛——
- `Button::apply_theme` 只管各变体的**常态语气**（删掉悬停/按下分支）；
- `Element::paint_box` 统一做"同向提亮/压暗 + **按下再深一档**"（系数 1.6，
  取自 `surface_pressed` 相对 `surface_hover` 的额外偏移量级）；
- 按钮恢复 `background = true`（否则去掉组件分支后**反馈尽失**：实测 Δ=0）。

### ③ 叠加层在 `paint` 里撞除自己 → use-after-free（ASan 实测）

**现象**：`Toast` 自动消失时**偶发 SIGSEGV**（依赖编译器把窄回调内联还是真调用，因而是脆弱的）。

**根因**（ASan 现场）：`Toast` 到期 → 它在自己的 `paint` 里调 `on_dismiss` → 宿主
`remove_overlay` → 而那时 `dispatching_` 为**假**（当前是绘制而非事件派发），
于是走**立即 delete** 分支——而 `Toast::paint` 的栈帧还在用 `this`。

**修法**：新增 `painting_overlays_` 标记（与 `dispatching_` 并列），绘制期间摘除的一律进墓场；
叠加层遍历搬到 `paint_overlays()`，在两对象之间回收——指针在整个绘制过程里始终有效。

**为什么旧用例拦不住**：`ui_feedback_test` 的自动消失用例是**直接调 `toast->paint(...)`**，
宿主是那个 lambda、根本不碰 `UiRoot`。只有把 Toast 挂进 `UiRoot` 的叠加层、
且 `on_dismiss` 真的调 `remove_overlay`，这条路径才成立——新增用例走的必须是 UiRoot。

### 验证

- `st test` **796 通过 / 1 失败**（唯一红的是既有环境问题，见上）；
- **ASan 构建下 4/4 无报告**（修复前 1 次即报）；
- `tools/theme_reverse_verify.py` 7/7 回退即红（含新增的悬浮那条）；
- 交互态矩阵逐按钮量色：色相全部保住、悬停有可见位移、按下比悬停更深；
- `st lint` 0 违反 / mingw 交叉编译通过。

### 教训（写下来免得重蹈）

**"组件里按住状态算色"这种写法，在"样式只在布局时刷新"的架构里天然是死代码。**
本轮两个缺陷（悬浮抹平、按下无效）都是同一个模式：
**一份状态要在两个地方各自算一遍色，而两处的刷新时机不同**。
收敛到单一机制（基类通用层）之后，两个问题一起消失——
而"按下与悬停像素完全相同"这个现象，光看代码是看不出来的。

## 已完成（2026-10-06 `capture` 区域参数：两条**静默失效**的缺陷）

起因：用 `capture` 的 region 截图时截到了不对的区域。查下去发现两个缺陷，
**共同点是都不报错**——调用方拿到 `ok:true`，而结果是错的。

### ① 扁平参数被静默忽略 → 截了全屏

**现象**：发 `{x, y, width, height}`（没用 `region` 包）时，返回 `ok:true`，
而实际导出的是一张**全屏图**。

**根因**：`capture_region_of` 只认 `region` 对象；扁平字段根本无人读，
于是落到 "空区域 = 全屏" 分支。

**修法**：给了扁平字段却没有 `region` ⇒ 报 `invalid`，错误信息里直接写出正确形式。
同理 `region` 宽/高 ≤ 0 也报错（`IntRect{}` 在全屏分支里兼做哨兵，
静默当全屏会让"忘了传宽高"看起来像成功）。

### ② `pixel_size` 报的是**请求值**而不是实际导出尺寸

**现象**（更严重）：请求 `region={x:1200,y:750,width:200,height:200}`（超出 1280×800 视口），
落盘 PNG 实际是 **80×50**，而响应写的是 **200×200**。

**根因**（两处相乘）：
1. `capture_pixels` 会用 `intersect(physical_bounds)` **夹取**区域（这是对的，防越界读）；
2. 但 `handle_capture` 的 `pixel_size` 是拿 `region × device_scale` **算**出来的，
   从来没看过导出结果。

**为何危险**：调用方正是靠 `pixel_size` 确认"拿到的是哪一块"。它一旦不可信，
"按响应校验尺寸"、"同区域再比哈希"这类断言全部失效——而且不会报错，只会静默失真。

**修法（让错报在结构上不可能）**：尺寸必须**只有一个来源**——导出结果。
接口从"只回路径"改成原子返回：
- `capture_to_file → Result<SavedShot{path,width,height}>`
- `capture_png → Result<PngView{png,width,height}>`

于是 `handle_capture` 除了抄两个字段别无选择（拿 `region` 重算会当场编译不出来）。
响应的 `region` 仍是**请求值**——调用方靠两者之差识别"被夹取了"。

### 验证

两个新用例均做了逆向验证（回退即变红）：
- `capture_region_rejects_half_formed_params`（回退后错误信息匹配不上）
- `capture_pixel_size_comes_from_the_export_not_the_request`

**过程中修了两处假护栏**（都是自己写的）：
1. 尺寸那条初版**回退后仍然全绿**——因为测试桩的 `capture_*` 不夹取，
   请求值恒等于实际值，区分不出"算的"与"拿的"。桩已补上与真实画布同语义的
   夹取（帧 64×48），并加了"请求 30×30 落在右下角 ⇒ 实际 14×8"这类关键用例。
2. 区域报错那条的断言顺序不稳：先读 `reply["error"]` 再查 `ok`，
   而 nlohmann 的 `operator[] const` 缺键是 **UB（断言崩溃）**——
   回退实现后响应里没 `error` 字段，测试直接崩而不是优雅变红。已改为先 `ST_CHECK(!ok)`。

## P2

- [ ] **三维：点光 `radius` 是硬截断，物体背面出现一圈硬边**（2026-10-27 实测）：
  `sample_light` 在距离超过 `radius` 时直接返回 0，衰减曲线没有尾巴。实测：
  中间球 + 两盏 `radius=9.0` 的点光（1000x620），沿垂直线扫描球背面，
  **6 像素内亮度从 104 掉到 34**。观感上是一圈「光被刀切掉」的硬边，
  在曲面体上尤其明显（直方体侧面碰上时几乎看不出来）。
  **已量到的规避**：把 `radius` 给大到截断落在画面外（实测 9.0 时不在
  1000x620 里露出来）——但这是“藏起来”而不是修好，相机拉远就重现。
  方向：给衰减加平滑尾巴，如 `smoothstep(radius*0.7, radius, dist)` 乘进去；
  需同步 GPU 腿（`platform_d3d11.cpp` 同一处公式）并补一条“背面扫描无骤变”
  的回归用例（判据：相邻像素亮度差 < 20）。
  为何值得做：这是**两盏灯就能触发**的默认缺陷，而三维图层的常见配置
  就是“主光 + 两盏冷暖补光”。

- [ ] **裁云：AI 重发脚本会抹掉人工的「片段级」编辑**（2026-10-17 人机共用实测）：
  `apply_script` 的语义是**清空片段、按脚本重建**，所以 `trim`/`move`/`set_opacity`/
  `set_speed` 这些改在 `Clip` 上的结果，在 AI 下一次重发脚本时全丢。实测：人把片段 3
  出点从 6.700 修到 6.000，AI 只改了一个标题就重发整份脚本 → 出点回到 6.700。
  （`set_scene_text` 改的是 `script` 本身，反过来能保住——所以丢的只是 `Clip` 上的量。）
  **影响面**：这正是「AI 为主、人机共用」最核心的一条——两人轮流干活时，AI 的每次
  重写都会**静默**回退人的微调，而界面看不出任何异常。
  方向：二选一——
  ① `apply_script` 重建时**按场景身份保留**片段级覆盖（`scene_index` 是稳定键，
     可带一份 `overrides[scene_index] = {trim, opacity, speed}` 跨重建存活）；
  ② 给 AI 加**细粒度写入动作**（`set_scene_text` / `set_scene_duration` /
     `set_scene_accent`），让它不必为了改一个字段重发整份脚本。
  两者不互斥，① 更根本（重写就该不丢已有编辑），②减少不必要的重写。
  为何值得做：不修的话，人机共用只能“各做一段、交接时以一方为准”，无法真正协同。

- [ ] **线性渐变（带 alpha）在带 alpha 的逐像素路径上比纯色慢 157 倍**（2026-10-17，
  caiyun「AI 视频导演」导出优化时用 `tools/gradient_fill_probe.cpp` 量出来）：1080p 整幅
  纯色 0.42ms、**竖向**线性渐变 0.60ms、**倾斜/横向带 alpha** 线性渐变 **66ms**、
  径向渐变 98ms。竖向之所以快，是因为 `blend_coverage_runs` 里有“整行同色 → SIMD
  `blend_row`”的快路径；一旦渐变方向不竖直（或色标 alpha 让逐行颜色不再是常量），
  就落到逐像素递推 + 标量混合。
  **影响面**：任何大面积铺光（背景光晕、氛围渐变、图表底色）只要写成斜向就会
  单帧多花几十毫秒；而“竖向渐变”这个限制**只能靠读光栅化器源码知道**——文档与
  接口都没有提示。
  方向：把快路径从“竖直”放宽到“任意方向但每行颜色恒定”（即沿 `y` 单调的渐变），
  或对倾斜线性渐变做“每行一次 sample + 整行 SIMD”的**近似**（用中点色的误差在柔和
  铺光里不可见，且可用误差上限把关）。判据：`gradient_fill_probe` 里斜向/横向
  带 alpha 线性渐变 ≤ 5 ms/帧（现在 66ms）。
  为啥值得做：这是“背景贵”的真因，而应用侧只能靠“别用斜向/径向”绕过——把这条
  限制留在光栅化器里，下一个示例会再撞一次。

- [x] **控制通道 find 的 `#` 选择器对含点 id 失配（文档/容错双缺口）**（2026-10-15 Windows
  全面测试轮发现）：`ui::Selector` 按 CSS 语义把 `.` 当类分隔符，而业务 id 常含点
  （`git-file-<path>`、`tasks/ListItem@task-42`）——`find("#git-file-alpha.txt")`
  被拆成 `Id: git-file-alpha + Type: txt`，count 恒 0，报错却是「找不到元素」
  （离真因很远）。实测 `Button[id=git-file-alpha.txt]` 属性写法可绕过。
  方向：文档明确警示；或 find 时对 `#` 前缀的整体做「完整 id 匹配优先」的容错
  （先试整串，再退 CSS 解析）。为什么值得做：智能体/脚本最容易写 `#id`，
  而含点 id 在 git 面板里是常态，这个坑会反复撞。
  → **已落地（2026-10-15 演进轮）**：整串优先容错 + 报错指引 + 文档警示，
  详见上方「已完成」段与回归用例 `find_hash_selector_matches_id_containing_dots`。

- [x] **declarative_host 其余 `__d_*` 桥同族 `mark_dirty_all` 冗余核实（P1 只分析轮）**
  （2026-10-15 第 13 轮，第 9 轮 3636500 的顺藤摸瓜收尾）：对
  `__d_set_root` / `__d_mount` / `__d_move` / `__d_clear_slot` 逐桥取证，
  **结论：无冗余——四个桥尾部均无 `mark_dirty_all`，无需落地改动**。
  逐桥证据（安全链与第 9 轮判据同构：逐属性标脏 / 结构操作自带 layout 脏 /
  重排几何签名兑底 / set_content 自带整帧）：
  - `__d_set_root`（declarative_host.cpp:104-122）：单根分支尾部
    `UiRoot::set_content`（ui_root.cpp:44-55）**自带 mark_dirty_all**（这处整帧
    属实，且是首帧语义所需）；子树分支 `insert_child` → Element::insert_child
    （element.cpp:234 标脏）。注意与 `__d_unmount`/`__d_apply_batch` 相反：
    此处整帧锤子**长在下游 `set_content` 里而非桥内**，下游自身服务全框架语义
    （UIA 挂树即换根），桥内本就没调，无冗余可删。
  - `__d_mount`（declarative_host.cpp:128-156）：尾部仅 `insert_child` →
    Element::insert_child 尾部 `mark_layout_dirty`（element.cpp:234）→
    `mark_layout_dirty` 沿 parent 链冒泡到根（element.cpp:431-438）→
    `UiRoot::layout` 尾部几何签名比对（ui_root.cpp:150-175）决定是否整帧——
    首挂/增项几何必变⇒自动整帧，与第 9 轮 `__d_unmount` 的安全链完全同构。
    组件覆写（ScrollView:170 / TitleBar::insert_child→add_trailing:241-247 /
    WindowFrame:125）全部转调 `Element::insert_child` 兜底，无旁路。
  - `__d_move`（declarative_host.cpp:261-296）：幂等护栏（element.cpp 同源逻辑
    返回 false）之后**尾部显式 `parent->mark_layout_dirty()`**（declarative_host.cpp:294
    ——拆下待插期间 parent 已达整帧脏，此为局部收紧非冗余）；下游
    remove_child + insert_child 各自标脏双保险，几何签名兑底。
  - `__d_clear_slot`（declarative_host.cpp:246-257）：尾部仅
    `host_element->remove_child(...)` → Element::remove_child 自带
    `mark_layout_dirty`（element.cpp:244），与第 9 轮删除 `__d_unmount` 尾部
    锤子的那条安全链逐字同构。
  为什么值得核实：第 9 轮 grep 只见 `__d_apply_batch`/`__d_unmount` 两处命中，
  但 `__d_set_root`/`__d_clear_slot` 走 `set_content`/`remove_child` 之外的
  结构路径（子树形态 insert/remove + 单根 set_content），需逐一确认下游标脏；
  现已逐桥确认闭环。附带核对：其余 `__d_` 桥中 `__d_create`（建而不挂，无树变）、
  `__d_slot_root`（纯读）、`__d_set_direction`（尾部自带 `mark_layout_dirty`）
  均无整帧锤子；`src/ui/declarative_host.cpp` 全文件 `mark_dirty_all` **零命中**
  （仅存两条 2026-10-15 历史注释），全仓存活点（set_theme/set_text_port/
  set_viewport/overlay 增删/DPI 切换）均为真全局变更的正当语义。只分析轮，
  无代码改动（免回归），证据链与结论见 `.evolve/ledger.md` 第 13 轮。

- [x] **控制文件为裸文件名（无目录）时落盘白名单的 `.` 条目形同虚设**（2026-10-15 同轮）：
  `default_capture_dirs` 对无分隔符的 control_file 放行字面 `"."`，
  而客户端请求的是绝对路径——前缀比较永远不匹配（该目录下全部拒绝）。
  方向：无分隔符时用进程 CWD 的绝对路径展开（`fs::current_dir()` + "/"）。
  为什么值得做：智能体会话里 ctl.json 常直接写在 CWD，视觉断言基线写入
  会莫名被拒，而报错文案不会指向这个原因。
  → **已落地（2026-10-15 演进轮）**：CWD 绝对路径展开 + 报错文案附白名单内容，
  详见上方「已完成」段与回归用例 `capture_bare_control_file_allows_cwd_and_rejects_outside`。

- [x] **多行 `Text` 折行缓存的键不含 `RenderContext::text`（端口）**（2026-10-15 销账：
  已实现于主线——basic.cpp measure/paint_content 判据已是四元组含
  `layout_cache_port_ != context.text`；护栏测试 ui_text_wrap_cache_test.cpp
  2026-10-14 已在线（text_wrap_cache_follows_text_port 双桩端口断言折行跟随，
  含换回还原分支）。条目是合并前复核记录，修复落地后未勾账。v2.2 台账 24 轮。
  原复核细节与修法草稿已随实现落地，不再保留。）

- [x] **`Tree` 缺程序化 select（写侧 `selected_key`）**（2026-10-12，gbcode 会话 e2e 亲历；
  2026-10-15 落地）：已加 `set_property("selected_key", key)`（notify=true 与键盘/
  鼠标同链，无效 key 清空选择不派发）；回归
  `tree_selected_key_is_writable_via_property_and_notifies`（含读回对称与无效 key
  分支，逆向验证：回退 notify=true 后断言变红）。e2e 从此可按键精确驱动，
  不再 `bounds.y + row_h*1.5` 坐标猜行序。
- [x] **`MenuPanel` 缺按 id 激活的动作面**（同上亲历；2026-10-15 落地）：已加
  `invoke_action("activate_item", item_id)`——与鼠标 Click 同一条链
  （`set_highlighted` → `on_activate` → `on_close`；“激活后关闭”不能省：`on_close`
  是声明式 overlay 的“不认领”信号，缺了它面板就留在屏上挡后续交互）；无效 id
  如实 false（不抛错不静默），分隔线（无 id）不参与；与既有下标激活（Click/Enter）
  共存、互不干扰。回归：`menu_panel_activate_item_by_id_drives_same_chain`
  （含高亮跟选的读回对称、无效 id 分支、未命中后面板不动），
  `menu_panel_id_and_index_activation_coexist`（Click/Enter/id 三路径同一面板，
  各自回调到位、不多不少），以及协议层端到端
  `invoke_activate_item_reaches_menu_panel_by_id`（`tests/control_protocol_test.cpp`，
  含无效 id `handled=false` 与未命中后面板仍可 find）。e2e 从此可按 id 精确驱动，
  不再 `bounds.y + 16 + i*32` 坐标猜行序。（原条目：菜单动态化——最近打开——
  后行序会变，坐标法脆。）
- [x] **`ST_REQUIRE` 在非 void 函数里编译失败**（2026-10-12，写单测亲历；2026-10-16 落地）：
  已取**方案②+③混合**：新增 `ST_REQUIRE_OR(expr, fallback)`（值返回 helper/lambda 里用：
  失败时记录失败 + `return (fallback)`，回什么由调用方给——语义是"早退当前函数"，
  不冒充弃跑用例）；同时 `ST_REQUIRE` 注释文档化"仅限 void 函数体，非 void 里
  编译失败是刻意护栏"。弃方案①（`return static_cast<void>()`：void 表达式
  return 到非 void 仍是硬错，条目原文已自我否定）。主流宏零改动（200+ 处用法
  不受影响）；豁免零新增（test.hpp 文件级 L3 已覆盖，与 ST_SKIP 同先例）。
  护栏互为镜像：ST_REQUIRE 在非 void 里硬错、ST_REQUIRE_OR 在 void 里硬错
  （`return (fallback)` 非法）——误用形态直接编译失败，不产生静默错语义。
  回归 `tests/require_or_test.cpp` 2 用例（失败路径记失败+回 fallback+计数；
  通过路径零干扰）+ 文件头说明与 expect() 型 void 化 helper 惯例的分工。
- [x] **`ST_CHECK_EQ` 不支持 `std::vector<std::string>`**（同上亲历；2026-10-15 落地）：
  已取 `ST_CHECK_LIST(actual, expected)` 宏方案（弃 formatter 特化——std 模板
  显式特化须依赖程序定义类型，是 UB）；失败消息含双方完整列表+首个不符下标，
  消息件 format_list/list_diff 提成可单测函数；gbcode_session_store_test 手写
  check_list 32 行上提删除。回归 test_framework_list_test.cpp 6 用例
  （含消息内容断言与临时值按值捕获），见 v2.2 台账第 21 轮（81729f6）。

- [ ] **`check_selfcontained.py` 纳入发布前流程**（当前是手工跑；无 CI，见下文 CI 已删）。
  本轮已发现它**首次运行就有真产出**（`channel.hpp` + `core_fs_test.cpp` 两处），
  说明"约定 + 手工习惯"这条路已经证明不够——但先观察几轮再定接入方式。
- [x] **对象缓存对“头文件变化”的失效范围要查**（2026-10-08 调试时踩到；
2026-10-15 v2.2 第 30 轮控制面探针已量清机制；**第 47 轮已修 restore 路径**，1032e80）：
改完 `tests/ui_terminal_pty_test.cpp` 后反复跑 `st test`，输出一直显示**旧的断言数**（6 而非 7），
且 `build/dev/obj` 里根本没有该单元的对象文件 —— 说明对象是从
`~/.shuangtian/cache/objects` 恢复的。补 `ST_NO_CACHE=1` 后仍一次没生效，
删掉旧对象再跑就对了。
**探针结论（第 30 轮，代码级量清）**：缓存键 `cache_key_for`（build.cpp:871-896）
含编译器路径/族/三元组/deps 格式版本/标志/档位标志/源路径/三方与框架标记/PCH
token/include 目录——**不含源内容与 mtime**；版本判定全在 `object_cache_restore`
（build.cpp:226-246）的**依赖新鲜度校验**：源文件与 `.d` 里每个依赖的 mtime
都不得比缓存对象新。所以“工程内对象被删”路径上，共享缓存靠 **mtime 比对**
知道条目版本（亲历当时是踩中了 mtime 未变化的窗口）。
**修复（第 47 轮，1032e80）**：restore 路径 mtime+size 双校验——缓存条目新增
`unit.meta`（逐行 `path TAB mtime TAB size`）；旧条目无 meta 走原 mtime-only
（老缓存不失效零行为变化）；mtime 严格不等兼兜时钟回拨；元数据先行落盘防错配。
实测：新条目 meta 落盘格式正确；探针（改 size 钉 mtime）确认**工程内快路径
不走 restore**——**残余窗口是 profile 内 .o 命中路径仍是 mtime-only**（同秒改+建
/旧 mtime 拷贝在快路径下仍可骗过，即下方 3620 条目——待后续单）；
以及等长改写（要哈希才能根治）。⚠️ 亲历「ST_NO_CACHE=1 补了没生效」未复现——
下次踩到先记录现场再排查。
  实测过程注意：st build 不含 tests 单元（删 tests 对象须用 st test 验证）；
  集成用例 `independent_project_*` 真建真编独立工程 ~5 分钟，探针时勿带。
- [x] **`st test` 的“链接失败被当成测试失败”很难看出**（2026-10-15 落地）：链接类错误
  现在分节「===== 链接/构建失败 =====」+「本轮没有任何用例结果——不是测试失败」
  + 固定退 125（不与失败用例数碰撞）；失败用例另分节「===== 失败用例（N）=====」
  退码仍=失败数。判据 is_link_failure 纯函数（消息前缀，非 ErrorCode——无 Link 位）。
  实测双分支（产物占用真链接挂→125；负载误伤→3+分节）。见 pkg_test_link_split_test.cpp。

- [x] **`font_stack_load_is_cached_across_calls` 常态红灯 —— 已修**（2026-10-08）：
  原断言 `hot_ms < 1.0`，实测 172 / 173 / 181 ms（单跑 3 次全红）。
  **量出来的真因**（`tools/font_stack_cost_probe.cpp` + `font_face_index_cost_probe.cpp`）：
  热调用 44 ms 里，绝大部分是「**失败不进缓存 × 盲试 face 下标**」——`load_face` 原本写
  `index = 0..11` 试到失败为止，而 `FontFace::load` **先把整个文件读进内存**才开始校验，
  越过界那一次一样要读完 `msyh.ttc`（20 MB，单次 9–12 ms）才报 `NotFound`；
  那条失败不进缓存 ⇒ **每次调用重付**（每个 CJK 候选各一次）。
  **产品侧修**：新增 `st::text::font_face_count()`（先问 face 数再按下标试，并加
  `face_count_cached` 因为 `st::fs` 无局部读接口、问一次就要读全文件）。
  实测 **44 ms → 0.87 ms**。
  **测试侧修**：改数确定量——新增 `st::text::font_cache_stats()`（`face_loads` /
  `face_hits` / `count_reads`，**thread_local**，与缓存本身同作用域），
  断言“热调用零新增 load/count”+“命中数确实在涨（防假绿）”。与机器快慢无关。
  逆向验证：把 `load_face` 改回猜下标 → 计数器每轮 `load` 增 5、探针退出码 1（抓到）。
- [x] **`terminal_pty_stop_interrupts_without_killing_the_shell` 偶发红 —— 已修**（2026-10-08）：
  实测 12 轮红 6 轮，且红的那几轮屏幕**一个字节都不再增长**。
  **量出来的真因**（`tools/pty_stop_probe.cpp`）：只等“命令行回显到屏幕上”就发 `Ctrl+C`，
  而**回显 ≠ 开始执行**——shell 收到整行就回显，真正 spawn 命令还要一小会儿；
  在那个窗口里，`0x03` 取决于行规程把它发给谁，前台进程组还没换成 `Start-Sleep` 时
  这一次被**吞掉**。关键判据：失败现场**再发一次 `Ctrl+C` 15 ms 就恢复**
  ⇒ 不是会话僵住、更不是产品缺陷（向未运行的命令发 `Ctrl+C` 本就该无效）。
  **修**：长命令改成先打一个“已开始执行”标记再睡，等标记出现才发 `Ctrl+C`（条件等待，非盲等）。
  ⚠ 标记**必须在命令文本里拼不出来**：第一版直接写 `running-9271`，而它就是命令行的子串
  ⇒ “等标记”退化成“等回显”，前置条件形同虚设（仍 25% 红）。现两平台都拼起来：
  PowerShell `('ru'+'nning-9271')`、bash `'ru''nning-9271'`。
  逆向验证：取 HEAD 原版 → **25 轮红 9 轮**；修复版 → **25 轮红 0 轮**。
  - **2026-10-17 复测：上面这次修法的前置条件选错了**（“等标记”方向对，标记的来源错）。
    标记由**前一条** bash 内建 `echo` 打出，而要被打断的是**后一条** `sleep`——两者分属两个前台作业，
    交接（fork→exec→tcsetpgrp）晚于标记出现。实测窗口宽度 **+0.6 ms（p50）/ +2.3 ms（max）**
    （`tools/pty_timeline.py`：标记字节到达 − 前台进程组易主），所以那次只把窗口从“整行解析之前”
    **缩小**到 1–2 ms，没有关掉；低负载下几乎打不中，这就是它仍然偶发的原因。
    **改为同进程组**：`/bin/sh -c "echo …; exec sleep 60"`——bash 眼里是一条命令（一个前台作业），
    `sh` 打出标记后 `exec` 成 `sleep`，标记之后无任何前台交接（实测该量 **−1.1 ms p50**）。
    `exec` 必要：少写它（`sh -c '…; sleep 60'`）就多一层等待中的 `sh`，负载 6.8→9.9 下红 **4/30**；
    带 `exec` 红 **0/30**。
    实测复测（低负载 1.69 → 1.51）：新写法 **24 轮红 0**（另两批 20 轮、20 轮均红 0）。
    判别力（`tools/pty_form_reverse_verify.py`，旧形态把窗口固定放大到 300 ms）：
    旧 **12/12 红**（零加压）/ 11/12 红（加压 8 路）、新 **0/12 红**；固定放大版另测 **8/8 红**
    且红轮目标 `sleep 60` 确实还在跑（正是被测语义要排除的状态）。
    ⚠ 已排除的放大手法（别重试）：① `echo …; sleep 0.05; sleep 60`——SIGINT 打死中间那条后 bash **中止整条命令列表**，
    prompt 照常回来（反而掩盖缺陷）；② `read -t 0.5`——SIGINT 同样中止它，同上；
    ③ 在标记后连发多次 Ctrl+C——一次命中就回提示符，测不出“某一次被吞”。
    Windows 分支（`Write-Output …; Start-Sleep`）**保持未改**：两者都是 PowerShell 进程内 cmdlet，
    不发生前台作业交接，只能靠 Windows 实测判定（见用例内注释）。
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

### 从其它章归位（本次整理；原章声明已失效，条目内容未改）

- [ ] **`for_each` 的 item 级失效粒度**（本轮只解了它的**复杂度**，没解**粒度**）。
  现状：`for_each` 按 key 复用元素（位置/身份不丢），但**不为每个 item 建独立作用域**——
  任一 item 依赖的状态变化仍重跑整个列表的 `build()`。实测 5000 项改一条数据 ≈ 1.4 ms
  （可接受），但那是**纯对齐 + diff**的代价；`item_fn` 里的业务计算（格式化、过滤、
  子组件构造）会按项数重复。
  方向（已探明）：item 级不能照搬 `sub_component`（后者需要“每个 item 一个 Component”，
  而 `for_each` 的形态是就地声明），需要“作用域挂在 item 上而不是子组件上”的机制——
  即 `begin_keyed_item` 同时开一个作用域、`end_keyed_item` 收尾，依赖登记到该作用域。
  预估工作量：中等（作用域生命周期已就位，需处理“位次与 key 解耦后 scope 如何复用”）。

- [ ] **内联 `Deps{{…}}` 与内联 lambda 实参触 GCC 13 的 `-Wdangling-reference` 误报**：
  `memo`/`effect` 返回引用，GCC 会把“实参里的临时”（临时 `Deps`、或直接写在实参位的
  lambda 闭包对象）当成被引用对象而报警。现状是调用方用**具名局部变量**回避
  （`examples/gallery/pages.cpp`）。更好的做法是从源头改：让 `memo` 返回**轻量句柄**
  （而非裸引用）或收 `Deps` 为值参——但那会动 API 形态，需权衡。

- [ ] **示例级组件回流决策：caiyun TimelineView / louyue ColorPicker+Viewport 双份维护**
  （v2.2 第 32 轮 rescan 发现，第 36 轮控制面量清，2026-10-16）：三个内置组件都从
  这两例泛化而来（v0.1.6/v0.1.7），原示例仍维护手写版。**量清事实**：内置版
  护栏齐全（timeline model 7 例+view 8 例/color_picker 8 例/viewport 族 35+ 例），
  且三组件均已 DSL X 宏注册（`dsl::TimelineView/ColorPicker/Viewport` 直接可用）；
  示例手写版 caiyun views.cpp 485 行（与 Project/snap/trim 模型深度耦合——监视器
  同步/检查器联动在类里）、louyue HSV 三滑块+棋盘格缩放规模小但同样无共享护栏
  （只靠 e2e 截图断言），行为漂移不可见。**两方向**：①示例回流内置版
  （删手写，示例变内置组件应用场景示范——caiyun 是接口重构非删代码，
  louyue 是小改）；②文档化「示例自绘验证场」定位（保留手写版作为泛化前
  试验田，但双份漂移风险永存）。**控制面推荐①**（护栏差距悬殊：50+ vs 0；
  DSL 已注册回流成本低；「能力存在但外部不用」正是 rescan 的核心病灶），
  但 caiyun 侧需排中单（耦合面大），louyue 侧小单可做。定案前不排工。
  **→ caiyun 半边已收账（2026-10-16，拆解步骤 6）**：经 S1-S5 框架增强
  （EditDelegate 委托通道/set_playhead_external/投影刷新防线/头部栏开关/tint）
  后换件落地——手写 450 行删除，`views.cpp` 瘦身为薄适配器（投影+编辑转发+控制
  协议薄壳），e2e 五段零修改全过（逆向验证：编辑绕过 Project 时第 2d 段检查器
  断言必红）。G25「外部真值源 + 委托编辑」示范位闭合，选中态真值归 Project
  （L3）。**louyue 半边仍开放**（ColorPicker+Viewport 回流，小单）——决策档
  保留至 louyue 侧落地后一并勾销。

- [ ] **状态类组件缺 `Change` 事件（ScriptBridge 事件表）**（v2.2 第 32 轮 rescan
  反推，xmlapp 实证）：勾选框只能挂 `click`（写 `change` 不报错但永不触发——
  app.js:59 注释踩过）。Checkbox/Switch/Radio/Select 的语义是「状态变了」，宿主 JS
  拿不到与点击解耦的状态变化通知（程序化 set 后 JS 无感知）。建议：事件表补
  `Change`（set_checked/set_selected_index 等状态写入路径统一派发），与 Click
  并存不互斥。JS 逻辑层路线里最高频的事件类型，缺它所有表单类 JS 应用都带着
  语义错位写。

- [ ] **ScriptBridge `ui.build` 无 key 对齐**（同上反推）：xmlapp render() 每次
  全量 clear+rebuild，行内输入态/滚动位置在数据刷新时全丢。dsl::list 已有
  ListItemData.key 机制，JS 侧三原语（build/clear/set）无对应物——跨语言能力面
  不对齐，「两种开发形态」的承诺打折。建议：`ui.build` 条目支持可选 `key`，
  宿主按 key 对齐复用（与声明式同一套对齐语义）。

- [ ] **gallery 组件页对账机制**（同上反推）：gallery 自称「控件全集」但缺 8
  个展示卡（TimelineView/ColorPicker/Viewport 全库零展示位），新增组件不会自动
  暴露缺失。建议：最低成本单测断言「ST_COMPONENT_LIST（dsl.cpp 注册表）-
  已知豁免清单 == gallery 展示集」——组件表加项不补卡即红。v0.1.5 专项后新增
  组件零跟进，缺机制的模式正在复发。

- [x] **基础组件未接排版覆盖**（2026-10-15 落地）：13 组件全接
  （Badge/Table/List 项/Tree/Select/Tabs/Checkbox/Tooltip/Toast/Avatar/
  KeyValueRow/Toggle/MarkdownView——四先例模式跟随）；口径：未接组件不再
  静默，DSL BoxProps 传到 C++ 侧可读回（typo_overrides_reach_cpp_sided_components
  直证）。回归 2 用例，全量 1471/0/27787。见 v2.2 台账第 23 轮（9ffffda）。

- [x] **FileDialog 的程序化选路只覆盖“单个路径”**（2026-10-16 落地）：多选已加
  `set_pending_paths` / 属性面 `pending_paths`（`|` 分隔，前置 `multi_select=true` +
  Open 模式；任一路径无效/跨目录/混目录 → 整单 false 零状态改动，不静默截断）+
  `on_confirm_multiple` 整份交付 + Ctrl+Click 同状态链 + 换目录/换模式自动清集合；
  选目录模式 `pending_path` 指向目录 → 进入并可确认（交出目录本身），指向文件
  如实 false。回归 4 用例（multi 完整链路 / 无效值七分支 / 选目录进+确认+
  拒文件 / 单路径不回归含 multi 回调零牵连）。

- [x] **`st test` 的集成用例在无框架根的目录下静默跳过**：
  `tests/pkg_integration_test.cpp` 拿不到 `ST_TEST_FRAMEWORK_ROOT` 时跳过（不假绿，
  但也不提醒）。若将来把它当 CI 门禁，需要让“跳过”在汇总里也可见（如计数行报“跳过 N”）。

  **已修（2026-10-16，控制面 bee6586b 单）**：摸底确认环境跳过与慢用例跳过是**两条路**
  （慢用例走注册期 `Case::slow` + `skipped_slow` 计数、汇总行「另 N 个」；环境跳过
  在用例体内 print+return，计成 PASS——st-tests-slow.log:1207 实证）。
  落码统一：新增 `ST_SKIP(reason)`（`Registry::record_skip` 进程级标志）——结果行
  报黄色 `SKIP`、汇总行报「`, N skipped（环境前置不满足，ST_SKIP）`」、退出码仍 0
  （不算通过也不算失败）；JUnit 报告跳过用例输出 `<skipped/>`。
  `pkg_integration_test.cpp` 三处环境跳过（ST_INTEGRATION_BUILD=0 / 框架根缺失 /
  st.pkg 缺失）全部改接 ST_SKIP。
  回归：`tests/skip_visibility_test.cpp`（哨兵自启动范式，钉结果行/汇总行/退出码
  三处可见性）；DESIGN §8.5 已同步。待实跑刷新断言数。

- [ ] **控制协议 `set` 对被拒属性的反馈只靠 changed 缺名，拼写错误易静默化**
  （v2.2 演进第 26 轮 w-fd 发现，控制面评估 2026-10-15）：`set` 应答 ok=true +
  changed 清单（真的改到的才进）；被拒属性（未知名/无效值/未开能力）仅表现为
  **changed 里没有它**——与 invoke 的 handled=false 红线不同。**评估结论：不是协议
  缺陷而是批量语义的如实设计**（invoke 是单动作二元结果，set 是批量属性表，
  逐属性结果在 changed）；但调用方不 diff changed 就误以为成功的风险真实存在
  （e2e 脚本拼写错误会静默跳过断言点）。候选方向（需权衡，不动协议主干）：
  ① set 响应加可选 `rejected` 清单（与 changed 对偶，零破坏增量）；
  ② 文档化「调用方必须 diff changed」约定 + e2e 辅助函数。定案前不排工。

- [ ] **第三方头应走 `-isystem` 而不是逐类抑制**。现状是 `-Wno-old-style-cast` 等会**全局**生效，
      连我们自己代码里的同类问题也一并放过（本轮之所以还能抓到 9 处自家缺陷，是因为那些
      `-Wno-*` 恰好没盖住它们——这不牢靠）。正道：`third_party` 那条 `-I` 改成 `-isystem`
      （GCC/clang 都支持，屏蔽来自该目录的告警而保留自家代码的检查）。
      这不是"顺手改一行"：`gather_include_dirs` 要区分"自家"与"第三方"两类包含目录，
      且缓存键与 PCH 消费端都要跟着变——需要单独立项并量一次收益。

- [ ] **把"双编译器构建"变成常规门禁**：本轮已证明它能拓出 GCC 看不到的真缺陷（上表九处）。
      但 **clang 工具链是可选的**（指向一份本地 LLVM 安装），所以门禁只能做成
      "有 clang 时跑"，而 GCC 扫过的仍是默认路径。

- [ ] **`build/dev` 的 PCH 是 114 MB / 磁盘 298 MB**（clang 侧同为 PCH 只有 30 MB，而全量构建
      两族持平——见 `docs/BUILD_TEST_PERF.md` §5）。压缩方向不在 PCH 体积而在**模块分层**（待验证）。

  - [ ] **编辑器分组**：VSCode 式左右分屏各持独立标签组需容器级支持。

- [ ] **`Panel` 不对子元素做裁剪 / 副轴尺寸传播**


  - **现象**：`Button` 溢出时直接画到相邻面板之上（无裁剪）；窄侧栏里 `Text` 长内容
  也会超宽（靠调用方自己 `ellipsize`）。
  - **修法方向**：要么 `Style` 上加 `clip: bool`（默认关，开时 `paint` 推裁剪矩形），
  要么明确“面板不裁剪”是设计取向并写进 `DESIGN.md §4.5`。
  - 本轮在示例层用 `ScrollView` 包一层规避（它会给子元素夹宽的约束）。



## 已完成（合并后 A 轮：继续排查同类形态）—— 2026-10-14

合入 master 后接的三条线索（A-3 / A-1 / A-2）。**结论：无新的活缺陷，但有 1 项潜伏脆弱点被坐实并补上。**

### A-3 缓存键是否对齐 `CodeEditor` —— **修了一处**

全库 `mutable` 缓存字段逐个核对"缓存键是否覆盖所有会让结果变化的输入"：

| 组件 | 缓存 | 键 | 判定 |
|---|---|---|---|
| `Text` 折行 | `display_lines_` | 内容 + 宽度 + 字号 | ⚠️ **缺端口** → 已补 |
| `Tree` 行宽 | `row_label_widths_` | 行数 + 字号 | ✅（`sync_nodes` 显式失效）|
| `ListItem` 项宽 | `label_width_` | 项数 + 字号 | ✅ |
| `Tabs` 项宽/前缀和 | `widths_`/`offsets_` | `labels_.size()` | ✅ |
| `CodeEditor` 几何 | `line_geometry_cache_` | `layout_port_`（**含端口**）| ✅ 参照组 |

**修法**（`basic.cpp` + `basic.hpp`）：缓存键加 `layout_cache_port_`，与 `CodeEditor`
的 `layout_port_` 口径一致。

**它为什么长期没暴露**（已查实，不是推测）：
`set_text_port` 在**生产代码里只有启动时一次**（`app.cpp:932`）——
DPI 切换、主题切换都不换端口。所以是**潜伏**脆弱点。

**为何还是该修**：
① 接口本身**允许**换端口（公开 API），任何宿主/测试都可能换；
② 同仓 `CodeEditor` 早就把端口纳入键了——**不同组件对同一件事两种做法**本身就是隐患；
③ 代价一行。

**护栏** `tests/ui_text_wrap_cache_test.cpp`（2 条），核心用**可配字宽的端口桩**
（标准夹具的两个桩字宽固定，表达不出"另一种字体"）。
**已反证**：把端口维从判据去掉，第一条立刻红（`wide_lines > narrow_lines` 失败）。

> 过程中桩本身出过一次错：第一版 `measure_width` 忽略 `size`，
> 于是"字号变大→行数变多"这条量不出变化（用例正确地把它变成了失败）。
> 已修（字宽按 `size/16` 缩放）。**桩不真实 = 量出来的结论不可信**。

### A-1 `std::string{}` 被当"未设置"用 —— 逐处看后**不是缺陷**

判据与 B 类一致：**该缺省是否有一个"写出来也达不到"的含义**。

| 字段 | 缺省语义 | 判定 |
|---|---|---|
| `process::Options::cwd{""}` | 空 = 继承当前进程 | ✅ 不能 chdir 到空路径，"空 cwd"不可达 |
| `ChannelSpec::cwd{""}` | 同上 | ✅ 同上 |
| `ChannelSpec::program{""}` | 空 = 未指定 → 后续 `open("")` 失败 | ✅ 与本仓"错误必须可见"的哲学一致（不静默回退）|
| `ChannelSpec::kind{"local"}` | 枚举缺省 | ✅ 不存在"未指定"态 |
| `log_file::path{""}` | 空 = 不落盘 | ✅ 空路径不是合法文件 |
| `gpu::Info::backend{""}` | 空 = 不可用 | ✅ 同 |
| `control_file{""}` | 空 = 不写控制文件 | ✅ 空路径不是合法文件 |

### A-2 `optional` 与空串混用 —— **不是缺陷**

最高频的形态是 `has_value() && !empty()`，但逐个看后它们**语义本就该两段**：

| 位置 | 两段各自的含义 | 判定 |
|---|---|---|
| `control/server.cpp` 的 `token` | `nullopt`=自动生成 / `""`=关闭鉴权 / 非空=指定 | ✅ **三态有意设计**（注释写明了）|
| `fs::read_env` 调用点 | 变量未设 vs 设为空串 → 两者都"无值" | ✅ 语义上等价，合并合理 |
| `fs::home_dir` 回退链 | `HOME` 有值才用，否则试 `USERPROFILE` | ✅ 空值不该参与回退 |

结论：`optional<string>` + 空串**不是"同一个语义两种表达"**——
当三段态都真实存在时（`token`），那是**正解而非凑合**。

### 本轮的元结论

A-1/A-2 两条线索**扫完无缺陷**。有价值的部分不是"又找到几个 bug"，而是：
**把"不是缺陷"的判定与理由逐条记下来**——否则下一个人会重走这一遗。
同时 A-3 证明这条线索确实产出了一个真问题（尽管是潜态）。

## 已完成（本轮「全部优化」落地，备查）

### 两类「结构性缺陷」的主动排查：输入方向 + 缺省值即歧义值 —— 2026-10-14

上一轮修的两处（滚轮方向、`--renderer auto`）都是**测试永远抓不到、只能靠人发现**的类型，
所以这一轮按同一思路**主动把两类扫完**。

#### A 类：输入方向/符号语义

**排查方式**：枚举全库 `wheel_delta` 使用点 + 拖拽/缩放方向，逐个判定是否符合约定。

结论：**方向语义目前全部一致**（下表）。三处非“内容偏移”类的也符合标准约定：

| 类型 | 组件 | 约定 | 判定 |
|---|---|---|---|
| 纵向滚动 | ScrollView / MarkdownView / MergeView / FileDialog / CompletionPopup / Input / Tabs / Table(Shift) / CodeEditor / Terminal | 向下滚（delta<0）→ offset 变大 | ✅ |
| 缩放 | Viewport / TimelineView（Ctrl+轮） | 上滚（delta>0）→ 放大 | ✅ 行业标准 |
| 拖拽平移 | Viewport（中键） | “抓住内容拖”（drag_origin − 位移） | ✅ 行业标准 |

**但发现两个真问题（都不是方向）**：

**A-1：约定是全局的、测试是局部的。**
方向约定只在 `scroll.cpp` 注释 + `ui_scroll_test.cpp` 的
`scroll_view_wheel_direction_is_natural`（**只覆盖 `ScrollView`**）里存在。
后果就是上一轮那两处 `+=` 各自测试全绿。

→ 新增 `tests/ui_wheel_direction_contract_test.cpp`：**表驱动**，6 个纵向可滚动组件逐条验
“向下滚必须让偏移变大”。表驱动的价值是**新增可滚动组件时漏测会被看见**。
（不覆盖横向/缩放/终端——语义不同，在各自测试里断言；文件头写明了理由。）

**A-2：滚动属性命名不统一。**
`markdown_view`/`input`/`table` 用 `scroll_offset`，而多数组件用 `scroll`——
同一件事两个名字，外部（控制通道/自动化/智能体）无法用同一句话问“你滚到哪了”。
→ 统一为 `scroll`（`actions.cpp` 的通用可设集、`ui_table_test.cpp` 一并改），
并在契约测试里加一条**命名统一性断言**（`property_names()` 必须含 `scroll`、不含 `scroll_offset`）。

> 另：`Tree` 不进契约表——它**自身不处理滚轮**（`onEvent` 无 `Wheel`），由宿主 `ScrollView` 驱动；
> 它的 `scroll` 属性是从绘制反推的（`first_visible × row_height`）。

#### B 类：缺省值即歧义值

**本仓自己已有权威判据**——`control.hpp` 里写着：

> 为什么用 `optional` 而不是哨兵字符串：曾用 `std::string token{"\0"}` 当「自动生成」哨兵，
> 但 `std::string` 从 `"\0"` 构造会**坍缩成空串**（遇 NUL 即止），于是「显式空串关闭鉴权」
> 与「未设置」无法区分——鉴权测试当场抓住（无法关闭）。

**同一文件里，只改了一半。** 紧邻的 `capture_dirs` 是：

```cpp
// 注释承诺：“空向量 = 禁止一切落盘（仍可 base64 回传）”
std::vector<std::string> capture_dirs{};

// 实现（server.cpp）：空 = 用默认白名单
impl_->capture_dirs = options.capture_dirs.empty() ? options.default_capture_dirs()
                                                  : options.capture_dirs;
```

⇒ **“想收紧到禁止落盘”会被静默扩成默认白名单（temp + exe 目录 + 控制文件目录）**——
**安全语义的反向**（想变严却变松）。

修法：改 `std::optional<std::vector<std::string>>`（与 `token` 同一形态）——
`nullopt` = 用默认；**显式空向量 = 真禁止**。

护栏 `capture_explicit_empty_whitelist_forbids_all_writes`，已反证（判据改回 `empty() ? 默认` 即红）。

> ⚠ 已如实区分：当前内部调用方（`app.cpp`）并未传空向量，所以它是**潜伏**缺陷
>（有注释承诺、测试也测不出）而非正在发作——但任何外部宿主/未来的开关都会踩到。

**扫描过的、判定“不是缺陷”的**（口径：`auto`/`0` 是否有一个“写出来也达不到”的含义）：

| 字段 | 缺省 | 判定 |
|---|---|---|
| `text_lcd` / `text_fit` / `text_gamma` / `ui_font_scale` … | `"auto"` | ✅ 未指定与显式 `auto` **结果相同**（都是“查 env 否则缺省”），无可达性差异 |
| `jobs` / `jobs_large` | `0` | ✅ 文档化哨兵（`0` = 按内存预算推导，不是可用的并行度）；`--jobs` 无外部解析入口 |
| `port` | `0` | ✅ 文档化（`0` = 内核分配，不是可绑定的端口） |
| `log_level` / `profile` | `"info"` / `"debug"` | ✅ 枚举缺省，不存在“未指定”态 |

### 两个用户报的缺陷：滚轮方向反了 + `--renderer auto` 被静默吞掉 —— 2026-10-14

#### ① 文件选择器滚轮方向反了（**同类共三处**）

全库约定在 `scroll.cpp` 里写成了明文：`Event::wheel_delta` 与系统同号，
**向上为正 / 向下为负**；而滚动偏移是“内容向上推了多少像素”，
offset 变大 = 看到更靠下的内容 ⇒ `offset -= delta × step`。

FileDialog 写的是 `+=`，等于把方向整个反过来。**排查后发现不是一处**：

| 文件 | 修正前 | 修正后 |
|---|---|---|
| `file_dialog.cpp` | `scroll_y_ += delta × step` | `-=` |
| `completion_popup.cpp` | `scroll_y_ += delta × step` | `-=` |
| `table.cpp`（Shift+水平轮） | `scroll_offset_ += delta × kScrollStep` | `-=` |

而全库其余滚动点（`scroll.cpp` / `CodeEditor` / `MarkdownView` / `MergeView` / `Tabs` / `Input`）
都是 `-`。

**为何长期测不出来**：`FileDialog::scroll_y_` 与 `CompletionPopup::scroll_y_`
**没有任何读取口**——于是“方向反了”无法用断言钉住。已补属性 `scroll`（与
`Tree`/`Table`/`MarkdownView` 的视口属性同一姿态：不可断言的效果 = 不可复用）。

**两处测试“固化”了 bug**（反向的实测教训）：

- `ui_table_test.cpp` 的 `ui_table_horizontal_scroll`：用 `wheel_delta = +1`（向上滚）
  却断言 `offset == 48`（offset 增大）——与约定相反，把 `table.cpp` 的反向 bug 写成了期望值。
  已改为 `-1`（向下滚 → 看到更靠右的列），并补一条反向断言（向上滚必须回到左端）。
- `ui_tabs_test.cpp:751` 是**对的**（“滚轮向下（delta 负）= 向右滚动”），
  `ui_scroll_test.cpp:57` 也对（“滚轮向下 = 负 delta……往下滚必须真的动”）——
  两份权威约定在仓里本来就在，反向的那两处是孤例。

护栏：`file_dialog_wheel_scrolls_downward_on_negative_delta`、
`completion_popup_wheel_scrolls_downward_on_negative_delta`。
已反证：把方向改回 `+`，两条都立刻红。

#### ② 显式 `--renderer auto` 在 headless 下被静默吞掉

**三方矛盾**：`backend.cpp` 的注释（204 行）与 note 文案（214 行）都承诺
“显式 `auto` 照旧探测”，而代码（206 行）把空串与 `auto` **一起**改写为 `software`。

根因：`AppOptions::renderer` 的声明缺省**本就是 `auto`**（`app.hpp:116`），
CLI 只在传了 `--renderer` 时赋值——于是「没传」与「显式传 `auto`」
**在字符串上完全不可区分**，那个判据根本无法兑现承诺。

后果不是“少了点功能”，而是**量错了机器**：`--renderer auto` 的无头 GPU 基准
静默跑在软件路径上（实测 1.44 ms → 11.49 ms），差点被当成 8.8 倍性能回归。

修法：`AppOptions::renderer` 的缺省改为**空串**（= 未指定），于是三态可区分：
空 = 无头缺省软件；`auto` = 照旧探测；`gpu`/`software` = 按名。
（`WindowOptions::renderer` 不动：窗口后端缺省 `auto` 是对的，且 win32 早已把空串按 `auto` 处理。）

**既有测试固化了 bug**：`shell_backend_test.cpp` 的
`headless_defaults_to_software_renderer` 写成 `for (const char* requested : {"", "auto"})`
——把“显式传 auto”也当“缺省”对待。已拆开：
空串 → software（且 note 必须说明是“**缺省**”）；显式 `auto` → 走探测分支
（不断言探测结果，只断言 note **不是**“缺省”——那是机器无关的，也是旧 bug 的直接体征）。
已反证：把判据改回 `empty() || == "auto"`，该断言立刻红。

### 组件层视口剔除 + 全类型契约护栏 —— 2026-10-14

**① `Tree` 逐行视口剔除（1000 行每帧 5.36 → 0.24 ms）。**
根因不是“缺虚拟化”，而是**窗口没对准**：`Element::paint` 已有视口剔除（`element.cpp:887`，
它自己的注释里就记着 SceneView 13.9 ms 的账），但**自绘组件不经过它**——`Tree` 的行不是
子 Element，而是扁平 `std::vector<Row>`，`paint_content` 无条件遍历全部行。

量尺 `tools/ui_tree_rows_bench.cpp`（新增，配 `tools/build_ui_probe.sh`）实测 400px 视口
（只容约 15 行）：1000 行 **5.36 ms** / 2000 行 **10.74** / 5000 行 **41.25**——**完美线性**，
即“视口里只显示十几行，却为全部行付账”。这也正是 `DESIGN §11` 那条
「`tree`（1000 节点）< 5 ms」**未达标的真实原因**（此前标 `*未测*`）。

修法：新增 `visible_row_range()`，口径与 `Element::paint` **逐字一致**（同一份
`canvas.clip_rect()` + `device_scale` 折算）——不另立一套。改后每帧 **0.24 ms 且与总行数无关**
（2000/5000 行同为 0.24）；全量绘制路径未变（16.27 vs 16.49 ms），证明没碰坏原路径。
护栏 `tests/ui_tree_viewport_test.cpp`（6 条，已验证“关掉剔除即变红”）。

**契约通电**：`first_visible` / `scroll` 过去**恒返回 "0"**（`tree.cpp` 老注释写着“它在契约里
仍然必须存在”——存在但没通电）。改为回读**上一帧绘制记录**（`visible_row_range_now()`，
与 `CodeEditor` 同一手法）。

**② 全类型契约面护栏（并揪出 `ColorPicker` 重复声明 `color`）。**
属性/动作面的守卫此前是**逐组件手工 fixture**（`ui_actions_test.cpp` 写死四个组件），
于是“碰巧没人写用例”的组件就是缺口。新增 `tests/ui_component_contract_test.cpp`：
遍历 `registered_element_types()`（**48 个**），对每个断言 6 类**如实上报**不变量
（名字非空/不重复、只读不写、未知名 `get` 返 `nullopt`/`set` 返 false/`invoke` 返 false）。
首跑即抓出 `ColorPicker::property_names()` 从基类继承 `color` 又 `push_back` 一次
（控制通道快照里重复）——已修。

**③ 口径校正：三处「32 个内置组件」→ 实测 48。**
`ST_COMPONENT_LIST` 已 48 条，而 `dsl.hpp` ×2 与 `DESIGN.md` ×1 仍写 32。
`tools/check_docs.py` 补上 **④b「组件计数核对」**——它此前只核对“清单里的名字**存在**”，
管不到正文里的**计数**（工具自称核对组件清单，实际没核）。反证：临时改回 32 → 立刻报漂移。

**④ `Tree::measure` 断崖——已修（同一轮后续）**：原留账项，根因与控制实验见下方「`Tree` 行宽缓存」条。

### `Tree` 行宽缓存（measure 断崖 11.85 → 0.004 ms）—— 2026-10-14
**上文 ④ 那条已做。** 根因经**控制变量实验**锁定：不是整形缓存（`shaped` LRU 4096）——
**只**放宽宽度缓存（`widths` 的 `> 4096 就 clear()`）就能让断崖完全消失
（4200 行 9.885 → 0.146 ms），单独放宽 `shaped` 阈值不起作用。

但**调大阈值只是把崖推远**：`measure_width` 在缓存失效后为每行做顺序扫描，
工作集（N 条）> 容量（4096）时命中率≈0——经典的「顺序扫描击穿 LRU」，
无论 LRU 还是整表清空都一样。正解是**让 `Tree` 自己按行缓存标签宽度**
（组件知道自己的行何时变、字号何时变；文本层只能猜）。

| 行数 | 改前 measure | 改后 | 倍数 |
|---|---|---|---|
| 1000 | 0.032 ms | 0.001 ms | 32× |
| 4000 | 0.134 ms | 0.003 ms | 45× |
| **4200** | **9.885 ms** | **0.003 ms** | **3295×** |
| 5000 | 11.853 ms | 0.004 ms | 2963× |
| 20000 | ≈53 ms | 0.016 ms | ≈3300× |

**失效契约**（三条，缺一条就是“改了标签/字号却读回旧宽度”——那是**正确性**问题）：
`sync_nodes` / `clear_nodes` / 字号变化。其中 `sync_nodes` 那条最隐蔽：
它先 `rows_.clear()` 再 push，**行数常与上次相同**，只靠“尺寸相同就不重算”会读到陈旧值。

护栏 `tests/ui_tree_label_width_cache_test.cpp`（6 条）：用**数 `measure_width` 调用次数**的
端口同时证明“缓存生效”与“失效正确”（不用耗时——机器相关）。
已验证能抓住缺陷：注释掉 `sync_nodes` 的失效调用后 `follows_sync_nodes` 立即变红。

> 教训：本项起因是上一轮“量到了但没动”——**留账比强行改值得**：
> 这一轮才能把根因量到单点（只放宽一个阈值即可复现/消除），而上一轮只有一条曲线。

### `ListItem` 项宽缓存（List measure 断崖 25.93 → 0.081 ms）—— 2026-10-14

**与上一条同源，但成因不同。** `List` 与 `Tree` 相反：每项是**真 `ListItem` 子元素**，
所以它**不覆写** `measure`/`arrange`/`paint`，全走 `Element` 基类逐子元素递归。
于是：

- `paint` **有**元素级剔除兜底（屏幕外的项不画）——实测 500→5000 项只涨 0.30→0.48 ms（亚线性）；
- `measure` **没有**任何剔除——N 项器要调 `measure_width` **2N 次**（label + subtitle）。

而文本层宽度缓存只有 4096 条且满了**整表清空**：2048 项 × 2 = 4096，
正好“自己把自己的缓存冲掉”→ 命中率≈0 → **断崖**。

控制变量实验（与 Tree 同法，只改一个变量）：只放宽 `widths` 阈值 → 断崖消失
（2100 项 10.92 → 0.20 ms）。定位点 2048 项 = 4096 次调用，与阈值精确吻合。

改法：把文本宽度缓存在**项自己身上**（`ListItem::label_width_`/`subtitle_width_`）。
缓存的是**纯文本度量**那一层（只取决于 `(文案, 字号)`），不缓存含内边距与夹取的最终宽度
（那些依赖 `constraints`/`style_`，逐轮都可能变）。

| 项数 | 改前 measure | 改后 | 倍数 |
|---|---|---|---|
| 2000 | 0.192 ms | 0.028 ms | 7× |
| **2100** | **10.923 ms** | **0.029 ms** | **377×** |
| 5000 | 25.927 ms | 0.081 ms | **320×** |

**失效契约**：`set_label` / `set_subtitle` / 字号变化。
护栏 `tests/ui_list_item_width_cache_test.cpp`（7 条，数 `measure_width` 调用次数）。
已验证能抓住缺陷：注释掉 `set_label` 的失效后 **3 条**测试变红。

> 两处缓存（Tree/List）**同根不同形**：同一个文本层缓存缺陷，在自绘组件上表现为
> “measure 里的手写循环”，在元素化组件上表现为“基类递归 × N 项”。这提醒了一件事：
> 这类缺陷的**载体**随组件实现形态变，而**根因**（全局缓存被顺序扫描击穿）是同一个。

### `Table` 行/列视口剔除（绘制 113.27 → 0.50 ms）—— 2026-10-14

**第三种形态，也是三种里最严重的。** `Table` 与 `Tree` 一样是自绘（无子节点），
且它已经写了一个提前退出：

```cpp
for (std::size_t index = 0; index < rows_.size(); ++index) {
  const math::Rect row = row_rect(index);
  if (row.y >= bounds_.bottom()) break;   // ← 看着对，实际在滚动中永不触发
```

**为何它不生效**：宿主把 Table 放进 `ScrollView` 时，`bounds_` 拿到的是**内容全高**
（`ScrollView::arrange` 把偏移烘进子元素的 `y`，高度给内容全高）——与 `Tree` 完全同样的情形。
于是 `bounds_.bottom()` 永远在所有行之下，`break` 一次不触发，仍迭代全部行×全部列，
每格调 `ellipsize` + `measure_width`。

**这个结论不是抽出来的，是量出来的**——量尺里做了 A/B 对照（同一个 break、只改 bounds 高度）：

| 行数 | bounds=内容全高（真实） | bounds=视口高（对照） |
|---|---|---|
| 1000 | 8.769 ms | 0.500 ms |
| 5000 | **113.274 ms** | 0.496 ms |

同一段代码、178× 差距——差别全在那把尺子**看不看得到视口**。

改法：按裁剪区剔行（`visible_row_range`，口径与 `Tree` 完全一致）+ 剔列
（`column_x` 已是滚动后坐标，与裁剪区无交叠的列直接 `continue`，不付 `ellipsize`）。

| 行数 | 改前 | 改后 | 倍数 |
|---|---|---|---|
| 500 | 4.590 ms | 0.503 ms | 9× |
| 1000 | 8.769 ms | 0.505 ms | 17× |
| 2000 | 45.980 ms | 0.504 ms | 91× |
| 5000 | 113.274 ms | 0.502 ms | **226×** |

改后**恒定**（等于对照值）——即“与总行数无关”，这才是剔除生效的判据。

**顺带补上缺失的视口契约**：`Table` 之前只有 `rows`/`scroll_offset`，**没有**
`first_visible`/`visible_rows`——而“用户现在看得到哪一块”是 AI 驱动界面时最常问的判断。
补上时发现一个设计要点：`visible_rows` **不能用 `bounds_.height / row_height` 算**
（那是内容全高，会报“200 行全看得见”）；必须与 `first_visible` 同源，用上一帧的绘制区间
（实测护栏当场抳住：期望 11、实际 200）。

护栏 `tests/ui_table_viewport_test.cpp`（6 条），已验证注释掉行剔除后 3 条变红。
既有 Table 用例（斑马纹/省略/横向滚动/hover）零回归。

### 多行 `Text` 排版缓存 + 逐行剔除（Terminal 行模式 3298 → 0.438 ms）—— 2026-10-14

**这一条挖到了基础组件。** 量 `CodeEditor`/`Terminal` 时两者的表现天差地别：

| 组件 | 20000 行稳态帧 | 结论 |
|---|---|---|
| `CodeEditor` | **3.83 ms**（500→20000 恒定） | 视口机制做对了（`visible_line_range()`） |
| `Terminal`（行模式） | **3298 ms** | 每帧成本 = 内容长度 |

连 500 行都是 **47 ms**（超过 16.7 ms 帧预算），不是大文件才出问题。

根因不在 Terminal，而在**多行 `Text`**（行模式就是把全文交给一个 `Text` 子件）：
`paint_content` 每帧 `port.wrap(content_)` **重折行全部内容** → 逐行 `measure_width`
→ 逐行 `draw`，**完全不看视口**。

修法两条：① 折行结果 + 行宽按 `(内容, 可用宽度, 字号)` 缓存；
② `paint_content` 按裁剪区只画可见那几行（口径与 `Element::paint`/`Tree` 一致）。

| 行数 | 改前稳态帧 | 改后 | 倍数 |
|---|---|---|---|
| 500 | 47.488 ms | 0.332 ms | 143× |
| 2000 | 705.416 ms | 0.334 ms | 2112× |
| 8000 | 2989.891 ms | 0.373 ms | 8015× |
| 20000 | 3298.367 ms | **0.438 ms** | **7530×** |

改后接近恒定（内容量 40× 而帧耗只涨 32%）——这才是剔除生效的判据。

护栏 `tests/ui_text_viewport_test.cpp`（6 条，已验证注释掉剔行后 2 条变红）。
**`Text` 是基础组件，影响面广**：全量测试 1394 条零回归（含 100 条 `text_*` 既有用例），
ASan 档亦无报告。

> 量到的教训（与前面几条合起来看）：这轮 5 个组件里，**凡是有自绘行循环的地方都漏了
> 视口**（Tree 的 paint、Table 的 paint、Text 的多行 paint），而**凡是有视口机制的都很健壮**
> （CodeEditor、Tree 的 paint——后者后来补上了）。判断一个新组件是否踩坑，最快的方式是问：
> “它内部有没有自己写的行循环？”有，就大概率没看视口。

### `MarkdownView` 组首行剔除 + 二分定位（4000 段 13.46 → 0.36 ms）—— 2026-10-14

继续按上一条的判据扫。`MarkdownView` 的 `paint_content` **有**逐行边界检查，看着没问
题——但有一条豁免把它的效果几乎全抵消了：

```cpp
if (outside && !row.group_first) continue;   // 组首行永不剔除
```

于是**每个屏幕外组的首行都会调 `paint_group`**——而它建 `Path` + `stroke_path`（圆角描边），
是每帧成本的主体。量尺实测（`tools/ui_rich_views_bench.cpp`，稳态帧，400px 视口）：

| 段数 | 改前 | 改为组矩形相交 | 再加二分 |
|---|---|---|---|
| 500 | 1.894 ms | 0.365 ms | **0.363 ms** |
| 1500 | 4.949 ms | 0.374 ms | **0.359 ms** |
| 4000 | **13.463 ms** | 3.670 ms | **0.360 ms** |

为什么两个都要做：组矩形相交解决了“屏幕外组的首行还画”（大头），
但**遍历全部行本身**仍是线性项（3.670 ms）；`rows_` 按文档序（`box.y` 单调递增），
所以可见区间可以**二分**出来（与 `Tree`/`Table` 同一手法）。

> ⚠ **二分的风险不是慢，是“漏画”**：它依赖“`rows_` 按 `box.y` 单调”这个前提，
> 一旦不成立就会静默少画几行。所以护栏的重点放在“**该画的都画了**”：
> 用记录型文本端口数“画了哪些行”，覆盖**视口在顶/中/底**三种情形，
> 并逐行断言“视口中心那几行必在、首末行必不在”。

护栏 `tests/ui_markdown_viewport_test.cpp`（5 条），已验证把二分位置改错后
**2 条**滚动用例立刻变红。同批补上 `MarkdownView` 缺失的视口契约
（`visible_row_range_now()` / `row_count()`）。

> 另记一条**使用陷阱**（非缺陷，但让我的测试先错了一轮）：`MarkdownView::measure`
> 在 `max_height` **无界**时把 `view_height_` 设成内容全高（自身撑高、由外部 ScrollView
> 滚动）——那种模式下 `max_scroll()==0`，内部 `scroll_to()` 完全无效。
> 要测“内部滚动”必须给有界高度。这是两种滚动模式的分工，不是缺陷。

### 规模不变性门禁 + 二分引入的一处漏画（含一次**自己的回归**）—— 2026-10-14

**先说教训：这轮差点把一处自己引入的漏画当成“改好了”。**

把本轮的六个量尺做成 CI 门禁时，我没照搬既有 bench 的「绝对耗时 / 清屏基准」（那套**只认
release 档**，文档还记着它在本容器余量踩线 0.66 倍 < 下限 1.3），而改用**尺度不变性**：

```
同一组件、同一视口，只改内容总量：t(N×) / t(1×)
  剔除做对了（O(可见行)） → ≈ 1      （实测 0.96~1.07）
  退化成 O(N)             → ≈ N      （实测 12~40）
判据 kMaxScaleRatio = 5   ← 两侧余量都 > 4 倍
```

它不依赖机器速度也不依赖档位（比值里没有“清屏 vs AA”那种跳档差异），
所以**在默认 dev 档下也硬断言**——而既有的比值型门禁在 dev 档是全部跳过的。

**然后反证时撞出了真事。** 把 `MarkdownView` 的剔除判据退回旧写法，门禁**没红**
（比值仍 0.99）。查下去才明白：二分已经让循环只覆盖可见行，所以“组首行豁免”根本不生效了；
**但它同时意味着组首行不再被访问**——而代码块/引用块的背景**只画在组首行那一次**。
于是：**我加二分那一步，引入了一处“滚动的代码块丢背景”**。

实测取证（临时诊断，已删）：40 行代码块、视口 300px、滚到中段：

| 位置 | `code_bg` 像素 | 非白像素 |
|---|---|---|
| 顶部（组首行可见） | 142592 | 144246 |
| 中段（组首行已滚过） | **0** | **0** | ← 整个视口全空

**为什么我原来的护栏没拦住它**：那几个用例断言的是“**画了哪些行**”
（用记录型文本端口数 `draw`），而组背景是 `paint_group` 里一次 `fill_rect_shape`，
**不经过 `TextPort::draw`**——所以它对“哪些行被画”完全不可见。
→ 补了一条**像素级**护栏 `md_code_block_background_survives_scrolling`，
已反证：去掉补画后它立刻红（`at_middle > 0` 失败）。

**修法**：`Row` 增一个 `group_head_row`（本组首行下标，排版期填）。`paint_content` 里：

| 情形 | 本组首行位置 | 处理 |
|---|---|---|
| ① | 在区间内 | 它自己走到时画（本帧只一次） |
| ② | 在区间**前**（已滚过） | 本行可见 → **由本行补画**（`rows_[group_head_row]`） |
| ③ | 在区间后 | 不可能（本行可见 ⇐ 首行在它之前） |

两侧都要防：只做①会漏画（上面那个表）；去掉判据退回“永不剔除”会让远处 4000 个屏幕外组
都建 `Path`+`stroke_path`——那正是 4000 段 13.463 ms 的来源。

### 门禁本体

`tests/ui_viewport_scale_gate_test.cpp`（5 条），覆盖本轮修过的每一处缺口：
`Terminal` / `Table` / `MarkdownView` / `Tree` / 多行 `Text`。实测比值均 **0.96~1.07**。
反证：注入 `Tree` 的全量遍历，比值 **1.00 → 18.18**，立刻红。

> 为何 `Tree`/`List` 的 **measure** 修复不进这个门禁：那两处是“断崖 → 便宜的线性扫描”，
> **不是 O(1)**，规模不变性对它们不成立；它们的正确护栏是结构性的
>（数 `measure_width` 调用次数），已在各自的 `*_width_cache_test.cpp` 里。

### 门禁扩到**全组件覆盖** + 又抓出两处（含一个 O(N²)）—— 2026-10-14

把规模门禁从 5 个组件扩到 **10 个**（新增 `List` / `SelectPanel` / `MenuPanel` / `Tabs` / `MergeView`）。
理由：扫描只能证明“**此刻**健康”，而门禁能证明“**将来**仍然健康”——
扫描的结论（“靠光栅层裁剪兜底”）是**可被静默破坏的假设**。

**扩完立刻又抓出两条**：

#### ① `Tabs`：绘制是 **O(N²)**（真缺陷）

`tab_rect(index)` 用 `for (cursor < index)` 累加前面所有标签的宽，而 `paint_content`
对**每个** index 都调它——1000 个标签就是 50 万次迭代。

修法（两步，各自都被门禁量出来）：

| 步骤 | 50 标签 | 1000 标签 | 比值 |
|---|---|---|---|
| 原始 | 0.212 ms | 2.437 ms | **11.48** |
| ① 前缀和（`offsets_`）→ `tab_rect` O(1) | 0.229 | 1.862 | 8.12 |
| ② 标签级二分剔除（前缀和单调 ⇒ 可二分） | 0.147 | **0.143** | **0.98** |

注意 ① 只消掉了**约 1/4**：剩下的大头是“对全部标签逐个 `ellipsize`+`measure_width`”，
而与 `Tree`/`Table`/`MarkdownView` 完全同形——**又一次“凡有自绘项循环就漏视口”**。

#### ② `MenuPanel`：**不是缺陷**（我的规模不现实）

1000 项时报 2.45 ms（比值 5.13）。但 `item_rect` 是 O(1)（`y = index * item_height_`），
没有嵌套循环——那多出来的时间是**阴影遮罩按矩形面积建**（1000 项面板高 28000px，
比任何屏幕都高，且组件无滚动支持）。

改量**现实范围**（菜单 5~30 项，多的“最近文件”也就几十）：**5 → 100 项比值 2.07**（过线）。

> 教训（第二次同类）：**门禁的规模必须落在现实范围**。
> 上次是 `SelectPanel` 的画布尺寸，这次是 `MenuPanel` 的项数——
> 都是“量了一个不存在的 UI”。判据要卡在“真实会发生的量级”上，否则量的是噪声。

#### 门禁的自我限定两条

- **规模取现实范围**：`MenuPanel` 5→100（不是 50→1000）；其余组件按“内容能到多大”定。
- **不覆盖 `Switch`/`Slider`/`Checkbox`**：它们的“N”不增长（单个控件），量了也没意义。

### 组件全面扫描收尾：剩余的都不是问题（附一次差点报错的量）—— 2026-10-14

把 29 个组件的 `paint_content` 全部扫了一遗（有无未剔除的项/行循环），逐个判定：

| 组件 | 项循环 | 剔除 | 判定 |
|---|---|---|---|
| `Tree` / `Table` / `Text` / `MarkdownView` / `List` / `Terminal` / `CodeEditor` | 有 | **已修** | 见上文各自条目 |
| `FileDialog` | 有 | **有**（`row.bottom() < list_.y …continue`，`file_dialog.cpp:674`） | 健康 |
| `MergeView` | 有 | 有 | 实测 200→4000 段恒 **3.25 ms** |
| `SelectPanel` | 有 | **无** | **不是问题**（见下） |
| `MenuPanel`（5~30 项）/ `Tabs`（几个）/ `TitleBar`（3 个钮）/ `TimelineView` / `ColorPicker` | 有 | 无 | 数据量小，不构成风险 |

**`SelectPanel`：一次差点报错的量（值得记）。**
初测（画布尺寸 = **面板高**）看着像严重缺陷：

| 选项数 | 初测（错口径） | **真实画布（800×600）** |
|---|---|---|
| 100 | 3.681 ms | **0.875 ms** |
| 500 | 19.262 ms | **1.644 ms** |
| 2000 | 84.847 ms | **3.676 ms** |
| 10000 | 461.076 ms | **32.954 ms** |

把画布开成“面板那么高” = **全屏都在画布内**，等于人为取消了裁剪；而真实屏幕是
固定尺寸，面板超出屏幕的部分**根本不会被画到**（光栅环节自己会裁）。换成真实画布后
每顶成本降到 1/N 量级，且在真实选项数（几十~几百）下是**亚毫秒~毫秒级**。

> 教训：这是「尺子口径必须与应用实际一致」的又一实例——**量尺的画布尺寸也是口径的一部分**。
> 前面的探针（Tree/Table/Text/Markdown）都是拿**固定视口画布**量的，所以口径没错；
> 这一个初版随手把画布设成了内容高，于是量了一个“不存在的世界”。**结论：不改。**

### 文字渲染：覆盖率 gamma 预校正 —— 2026-10-04（设施保留、**默认已关**，含一次被驳回的过度优化）

- [x] **设施（映射/开关/缓存键/可观测/回归）已建**：`α' = 1 − (1−α)^(1/γ)`、
  `--text-gamma auto|off|<数>`、`ST_TEXT_GAMMA`、协议 `metrics.text_gamma`、启动日志；
  亚像素下三通道乘**同一标量**（逐 channel 会破坏“三通道是同一几何覆盖率的三种采样”）；
  先 gamma 后滤波（滤波是“把墨在相邻子像素摊开”的物理过程）。
  `tests/text_coverage_gamma_test.cpp` **5 条**，含**默认值必须为 1.0** 的护栏。
- [x] **曾把默认值设为 2.2（提亮 / 完整线性空间合成）——用户实测驳回**：
  「优化后代码编辑器反而不如优化前」。朴素像素口径复核：实心像素占比 10.8% → **8.8%（−19%）**、
  过渡带 13.1% → **13.8%（更宽）**——“发灰、发虚”。**已回退为 1.0**（像素统计与修复前逐位一致）。
- [x] **两条口径教训已记档**（DESIGN §4.3.6）：① 拿**带假设**的量（覆盖率投影隐含“屏幕线性”）
  当基准，与“在 sRGB 空间混合”这个前提自相矛盾，结论必然偏；两个口径冲突时先信**假设少**的
  （朴素亮度）。② 参照不能用**无头**浏览器截图——它不带桌面 GPU 合成与字体渲染链路，
  渲染出的字比用户实际看到的更浅。
### 工具链：自举脚本与 `st.pkg` 脱节（2026-10-04，被上述任务随手踩到）

- [x] **`bootstrap.ps1`/`bootstrap.sh` 在 SQLite 内置后必崩**：两者各自硬编码
  `-Iinclude -Ithird_party` 并全量编译 `third_party/**/*.c`，而清单里多出了
  `include_dirs: [third_party/sqlite]` 与 `c_flags: [-include st_sqlite3_config.h]`
  ——`sqlite3.c` 找不到 `sqlite3.h` 直接 fatal error。**克隆下来根本编不出 `st`**。
  修法：版本号/宏/包含路径/第三方 C 源/C 源标志全部改为**读 `st.pkg`**（唯一权威），
  `third_party_sources` 按 glob 只编清单点名的（不再是全量扫描——那会替未来放进
  third_party 的参考实现/上游自测挑错）。

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
- [x] **示例精简与 VSCode 式重写**（2026-10）：删除 mdeditor 示例；gbcode 按 VSCode 信息架构
  重写（标题栏/菜单栏/活动栏+侧栏/多标签编辑区/底部面板/状态栏，全内置组件零自绘）。
  重写中反推出的框架缺口（详见 DESIGN.md §8.1.1）：
  - [x] **SplitView 内置化**（2026-10-02 落地）：框架组件 `SplitView`（拖拽分栏，
  见 DESIGN §4.5 v0.1.5 组件能力）；gbcode 侧栏/编辑区已迁移；测试 7 用例
  （`ui_split_view_test.cpp`）+ `tools/split_view_e2e.py`（拖拽/夹取/动作面/截图）。
  迁移中发现并修复框架交互缺口：`MouseUp` 未按拖拽归属投递（拖出手柄后释放丢失）。
- [x] **命令面板通用组件**（2026-10-03 落地）：`ui::CommandPalette`（FillViewport 遮罩 +
  顶部居中卡片 + 过滤列表 + 键盘环绕导航 + Esc/点遮罩关闭 + `query`/`command_count`/
  `match_count`/`active` 属性面与 `activate`/`select` 动作面）。gbcode 示例已迁移
  （退役 130 行私有实现），并新增 `grab_focus()`——**打开面板后必须调它**，否则全局
  快捷键只把面板显示出来、焦点仍在底层编辑器上（敲的字跑进代码里）。
  迁移中把鼠标链路也提上来了：点条目走数据层 `List::Entry::on_activate`
  （与 Enter 共用 `run_command`），不再与 `List` 的“点击即选中”争同一个槽位。
- [x] **单行 Input 动作面**（2026-10-02 落地）：`invoke submit/activate/clear` 与 TextArea 对齐；
  测试 `ui_input_invoke_*` 3 用例 + `tools/input_action_e2e.py` 真实应用端到端。
  - [x] **拖动缩放：逐帧真重绘（去掉“拉伸贴满、松手才重建”）**（**2026-10-04 落地**）：
  用户现象“拖动边框缩放时内置高度或宽度被拉伸扭曲，只在释放才重新渲染”。
  ① **量化**（`tools/live_resize_probe.py`，真鼠标 `SendInput` 拖边 + 屏幕像素抓图）：
  拖动 30 步 `metrics.frames` 只涨 **0~2**；“拖动中的画面” vs “同尺寸正确渲染”
  `mean|Δ|=19.3`、**21.8% 像素明显不同**（内容被横向拉了 ~1.16×）；
  一次尺寸变化的重建开销（`last_frame − layout − paint − present`）**~28.9 ms**；
  ② **根因与三笔成本**（顺序即依赖）：`renderer_ == "auto"` 在 `allocate_buffers` 里
  **每次尺寸变化都重跑选型基准**（建两块探测画布 + 各跑 3 轮）→ 选型只做一次
  （`resolved_renderer_`，理由也留住）；画布走新增的 `Surface::resize` 原地换缓冲
  （保留设备/上下文/字形与路径缓存与 swapchain）；交换链走 `ResizeBuffers`、DIB 惰性重建；
  ③ **旧实现的最后一块拼图**：`framebuffer()` 里 `size_dirty_ && !in_size_loop_` 把重建
  **在拖动期间整段禁掉**了（所以“重绘 32 帧”但每帧都画进旧尺寸画布）。
  ④ **两个护栏**（实测踩出来）：重入保护（没有就是拖动时栈溢出）；重绘期间
  **禁止 `pump_messages`**（`PeekMessage(PM_REMOVE)` 会把系统缩放手势的 `WM_MOUSEMOVE`
  抽走——症状极隐蔽：“帧在渲染、窗口却不动”）；
  ⑤ **视口对齐**：`render_frame()` 入口同步视口（拖动中 `tick()` 不转，布局会按旧视口排）；
  ⑥ **兜底**：`present` 在画布与后备缓冲不一致时**如实报错**（回退 GDI blit），
  不再让 DXGI 默默拉伸（“拉伸一帧”比“黑一帧”更难排查）；
  结果：拖动中画面与同尺寸正确渲染**逐像素相同**（GPU 与 software 两条路径都实测），
  渲染帧数 30/30、重建开销 **28.9 ms → 1.3~2.4 ms**；回归防线
  `tools/resize_regression_check.py`（空闲 3 秒 0 帧、最大化/还原、程序化改尺寸、正常关窗）。
  新增 2 用例（`dpi_canvas_resize_in_place_*` / `gpu_canvas_resize_in_place`）；
  全量 **676 用例 / 17197 断言全绿**、lint 0 违规。

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
    （`Application` 实现端口并转发后端）；**两个示例都已换壳**：gbcode 的 `#titlebar`
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
- [x] ~~**CI 三平台矩阵**~~（.github/workflows/shuangtian-ci.yml）——**已删除**（2026-10）：
  该工作流自建立起从未实际启用过，不产生任何门禁作用，却持续制造「有护栏」的错觉
  （真实代价见下条：`stats.cpp` 缺 `<map>` 这类缺陷本应由它拦住，实际一路进到自举才爆）。
  三平台编译验证改由发布前手工执行 `bootstrap.sh` / `bootstrap.ps1` + `st test` 承担。
- [x] **文档漂移全修** + check_docs.py v2（§ 引用/路径实存/组件清单/用例数四类自动核对）
- [x] **修首次自举失败（`st stats` 缺 `<map>`）+ 补「翻译单元自包含」约定**：
  `src/pkg/stats.cpp` 用了 4 处 `std::map` 却从未 `#include <map>`，而 `st build` 自动 `-include`
  `st/pch.hpp`（里面含 `<map>`）——于是**日常构建全绿、只有全新环境自举（无 PCH）才失败**
  （`error: 'map' is not a member of 'std'`，行号指向使用处 372 行）。已补 `<map>`（并归位 `<set>` 字母序），
  并在 CONVENTIONS §6 新增第 6 条「每个 `.cpp` 自包含、不得依赖 PCH/传递包含」、§10.3 扩为三个静默失效点。
- [x] **同期同类第二例：`channel.hpp` 缺 `<functional>`/`<cstdint>`**（2026-10-08，`bootstrap.ps1` 报错）：
  `ChannelSpec::port` 用 `std::uint16_t`、`ChannelFactory` 用 `std::function`，而头里只含
  `<memory>/<string>/<vector>`。`st build`、`st check`、`st test` **三个命令全绿**，
  只有首次自举报 `'uint16_t' in namespace 'std' does not name a type`，
  且行号指向 `channel.cpp:67` 而非缺头的那一行。
  **与 `<map>` 那条是同一个根因，而"新增约定"没拦住它**——因为约定只是一句话，
  日常命令里没有任何一条会去跑它（当时的 IWYU 差集体检脚本只查 `.cpp`、包含根写死，且从未被跑；
  该脚本已于 2026-10-16 删除，见上方「已完成」段）。
  因此这轮把门禁做成可执行工具：**`tools/check_selfcontained.py`**
  （头文件探针 + 按组补齐包含根的翻译单元编译；实测扫 302 个文件，失败 0）。
  逆向验证：回退 `channel.hpp` 两行 include → 头文件组与框架单元组各报红，
  共 2 处失败（退出码 1）；恢复 → 0 失败。CONVENTIONS §6.1 / §10.3 已加指向它的自查命令。
  副产物：已删掉手工草稿 `tools/tu_selfcontained.ps1`（同一逻辑的 PowerShell 版，已被取代）。
- [x] **顺带修 `tests/core_fs_test.cpp` 缺 `<algorithm>`**（同根因）：用了 `std::ranges::is_sorted`
  却没包含 `<algorithm>`；测试组在无 PCH 口径下报 `'is_sorted' is not a member of 'std::ranges'`。
  它一直没暴露，是因为 `st test` 走 PCH 口径——**测试自己也该被这条门禁覆盖**。
- [x] **修 `build gbcode` 编不过（Json 前向头漏补 `examples/`）**：
  `88ba665`（Json 前向头解耦）把 `dsl.hpp` 的 `#include "st/core/json.hpp"` 换成前向声明，
  给 `src/*`+`tests/*` 的 18 个 .cpp 补了 `json.hpp`——**却漏了整个 `examples/`**。
  而 `dsl::custom<T>` 是**模板**，实例化时需要 `Json` 完整类型（模板体调
  `create_element(…, empty_json_object(), …)`），于是 `examples/gbcode/main.cpp` 报
  「invalid use of incomplete type … basic_json」——报错落在标准库 `variant`/`type_traits`
  内部（300 行模板栈），完全看不出与 `dsl.hpp` 的关系。

  **为什么一路没被发现**：该提交自己的验证声明只跑 `st test` + `lint`，而 `st test` 只编
  `tests/*_test.cpp`、**不编 `examples/`** 也不编 `tools/`——两个环节都覆盖不到 gbcode。
  它自己的提交信息写着「需要完整类型的 .cpp（**含测试/示例**）自行包含 json.hpp」：
  **说明了但没做到**，且无任何机械检查会因“examples 编不过”而变红。

    修复（**最终形态**）：① `examples/gbcode/main.cpp` 补显式 `st/core/json.hpp`；
  ② 把 `dsl.hpp` 那句**错误的**注释「`Json` 只出现在签名里」改成事实；
  ③ **根治：把 `empty_json_object()` 从「按值返回 `Json`」改为「返回 `const Json&`」**——
  这是需求本身的来源：`custom<T>` 只因要按值传 `empty_json_object()` 的返回值才需要完整类型，
  改为引用后需求**从根上消失**（`create_element` 本来就收 `const Json&`，全链引用）。
  实施于 `json_fwd.hpp`（签名）+ `json.cpp`（函数内 `static const`，与同文件 `null_node()` 同模式），
  并在两处注释里写明「**不要改回按值**」及理由。

  **踩过的坑（值得记）**：第一版修法是加 `concept CompleteType = requires { sizeof(T); }`
  做完整性守卫（报错可读），结果在 GCC 16.2 上炸出 `-Werror=sfinae-incomplete`：
  `requires { sizeof(T) }` 是 **SFINAE 上下文**，探测不完整类型会被记录，当该类型随后被定义时，
  GCC 15+ 判定为缺陷。**触发者是 `server.cpp`——它根本没实例化 `custom<T>`**：
  因为 static_assert 条件不依赖 `T`，解析期就求值了（且 `server.cpp` 先含 `dsl.hpp`、后含
  `json.hpp`，正好构成「先失败、后定义」）。对照实测：`concept` 版本报错，直接写
  `static_assert(sizeof(Json) > 0, …)` 则不会（非 SFINAE 上下文）——但两者都只是绕过，
  返回引用才是消因。容器只有 GCC 13/14（无此警告），MinGW 16.2 才现形。

  教训一：**「改公共头 + 手动给 .cpp 补 include」这个模式必然漏**，尤其当目标目录不在
  `st test` 的编译范围内（examples/tools）。这类改动要枚举**所有**包含该头的 TU
  （可用 `grep -rl` 列全），而不是只改眼前的 src/tests。
  教训二：**能用数据结构/签名消除的需求，不要用断言去探测**——守卫依赖编译器行为
  （SFINAE 上下文是否被记录），而返回引用把需求彻底去掉，不依赖任何编译器。

> 本章剩余的未完成项已归位到上方 P1/P2 待做区（2026-10-09 整理）。

## 已完成（2026-10-18 gbcode 可用性实测轮：9 项缺陷 + 3 项能力缺口）

> 驱动方式：“实际测试 gbcode 的所有细节，要达到真正可用的水平”——用控制通道
> 驱动真窗口/真 shell/真 git 仓逐项走查，**不读代码猜**。
> 新增回归：`tools/gbcode_usability_e2e.py`（≈60 条断言，已入
> `tools/examples_e2e_all.py` 与 `docs/EXAMPLES_E2E.md` 入口表）。

### 缺陷（每一项都有“不修会怎样”的实测证据）

| # | 问题 | 根因 | 修法 |
|---|---|---|---|
| 1 | **终端标签显示完整路径**（`C:\Program Files\WindowsApps\…\pwsh.exe`），标签栏被撞碎 | PowerShell 7 启动即发 `OSC 0;<完整 exe 路径>`，落地时原样当标签 | `shell_label()`（去目录/去 `.exe`）+ OSC 标题统一经 `tab_label()` 归一（路径形态取尾名、限长 48 码点） |
| 2 | **终端 busy 恒真**：侧栏「运行」面板永远转圈显示「终端忙」，宿主所有 `!any_busy()` 的延迟动作永不生效 | `Terminal::busy()` 返回 `session.running`，而 PTY 下它只在**进程退出**时才清 | 语义分开：`busy()` = “命令在跑”（提示符回显 + 180ms 安静期双重判定）；新增属性 `running`（shell 活着）/`running_name`/`idle_ms`/`command_issued`；`any_busy()` 同步 |
| 3 | **无选区 Shift+Tab 反而增加缩进**（“键坏了”级别的手感缺陷） | `code_editor.cpp` 只看 `has_selection()`，否则走 `insert_text(indent_unit())`——反而插缩进 | 无选区时把选区取为当前行整行再 `indent_selection(true)`，光标回原位 |
| 4 | **UTF-8 BOM 未剥离**：`\ufeffhello` 原样进编辑器，行首多一个不可见字符、删不掉 | `fs::read_text` 直接返回字节 | 在 `read_text`（文本读取公共入口）剥 `EF BB BF`，所有调用方口径一致 |
| 5 | **状态栏「编码」单元格点了没反应**（两边邻格都可点，只有它不动） | 它是纯 `text`，不是 `status_item` | 改成可点状态项，点击给出“为什么改不了”的说明 |
| 6 | **树行不可寻址且读不到内容**：语义文本只有「N 行」，Agent 只能靠 `bounds + 行高` 猜坐标（行号随展开漂移） | `Tree` 是自绘组件，行不是 Element，也没有读面 | 加属性 `labels`/`keys`/`rows_detail`（每行 label/key/is_dir/depth/expanded）与写面 `activate_key`（与点行同链，不存在的 key 如实拒绝） |
| 7 | **二进制文件可编辑且 Ctrl+S 会写坏原文件**（实测：输入 3 个字节保存后文件变乱码，不可恢复） | 打开时按文本读，没人判定“这不是文本” | `looks_binary()`（头 8KiB：NUL 或控制字符密度 >5% 且 ≥8 个）+ `OpenBuffer::binary`；打开即只读，`save()`/`save_all()` 双重拒绕并在状态栏如实说明 |
| 8 | **逐行暂存“只删不加”**：勾中 `line 2` 那行后，暂存区里是“删掉 line 2”而不是“改成 CHANGED_A” | 勾选列只画在左栏（旧侧），用户能勾的只有删除行；`build_partial_patch` 对未选中的新增行直接丢掉 | 选择单位抬成**改动区段**（勾一行 = 整对改动，遇上下文断开），且勾选列**两栏都画**（右栏补身份表 + 点击回调） |
| 9 | **勾选后「暂存选中行」按钮恒灰**（`enabled` 停在构建期的 false） | 勾选发生在组件内部（自绘命中），不经过声明式状态 → 不触发重组 → 按钮永不重建 | 应用层新增 `State<std::size_t> diff_selected_count_`，由 `on_selection_changed` 抬成响应式信号；按钮读 State |

### 能力缺口（Agent/自动化根本够不着）

| # | 缺口 | 修法 |
|---|---|---|
| 10 | `DiffView` 的逐行暂存**无属性面/动作面**：能开模式、能看到按钮，却无法勾中任何一行 | 新增读面 `staging`/`selected_count`/`selected_lines`，动作 `toggle_line`/`select_line`/`deselect_line`/`select_all`/`clear_selection` |
| 11 | `DiffView::set_diff` 无条件清空选择，导致“勾选 → 触发重组 → 自己把勾选抹掉” | 相等早退（`diff_ == diff` 直接返回），只有内容真变才清选择 |
| 12 | 菜单项只能靠坐标点（面板高/条目数一改就点错） | `MenuPanel` 已有 `activate_item <id>`——本轮补上 e2e 覆盖（`save`/`save-as`/`close-tab`/`open` 四个全链路验证） |

### 额外发现（回归用例当场抓住的）

- **CRLF 文件行尾敲 `(` 不配对**：行尾候选集只有 `\n`，而 Windows 文件一行真正结尾是 `\r`。
  同一段代码改成 LF 就正常——这类“只在另一种行尾下才坏”的缺陷最难靠直觉发现。
- **短文件的 ANSI 日志被误判成二进制**：只比控制字符密度时，几十字节的文件里 2 个 ESC 就超 5%。
  加一条“控制字符个数 ≥ 8”才稳（回归 `gbcode_looks_binary_detects_…` 当场抓住）。

### 后续轮（2026-10-18 用户报“打开 DESIGN.md 整页变卡”）

| # | 问题 | 根因 | 修法 |
|---|---|---|---|
| 13 | **大文档编辑帧 90ms（打字 86.7ms/字符、整页卡）** | `TextRenderer::measure_width` 的宽度缓存在条目数超 **4096** 时 `clear()` 全清；而 4096 与“正常文件行数”同量级，5 千行文档持续落在“全清 → 全部重新整形”抖动区。放大因素：`max_line_width` 当时在 `rebuild_line_geometry`（每次文本变更都跑）里**逐行**测宽 | ① 宽度缓存改**真 LRU + 反向索引**（O(1) 命中更新），上限抬到与整形/字形缓存同口径；② `max_line_width` 改成**惰性 + 按文本世代缓存**，核心绘制路径不再隐式调它；③ 整形缓存上限 4096 → 同口径 |
| 14 | 无法用稳定断言钉住“缓存被全清” | 只能靠耗时判断，而耗时在负载波动下不可靠（试过：同一实现时红时绿） | `TextRenderer::width_cache_stats()` 暴露**命中/未命计数**，回归用命中率断言（已做反向验证） |
| 15 | **预览滚一点就大片内容没渲染**（用户截图） | 组背景与文字在**同一个逐行循环**里画：代码块首行滚出视口后，对**每一行**都重画整块背景 → 后画的背景把前面行的字盖掉，只剩最后一行可见 | 组背景改为**遍历前统一预画**（背景是比行更底的一层），只对**可见组**跑 |
| 16 | 排查“大片空白”时无法区分“区间错”与“绘制错” | `MarkdownView` 只有 C++ 接口 `visible_row_range_now()`，属性面拿不到；两者在截图上都是“空” | 属性面新增 `first_visible`/`last_visible`/`draw_stats`（行到了/字画了）/`painted_groups` |

**缺陷 15 的排查代价（值得记）**：初看像“滚动位置错”，实测却证明可见区间完全正常
（每次 15~22 行、`draw_spans` 调了 21 次）。真正的问题在**绘制顺序**——
这类“调用都对但结果被覆盖”的缺陷，只数调用次数或统计背景像素都测不出
（背景盖字时**背景像素反而更完整**，旧用例 `md_code_block_background_survives_scrolling`
因此一直绿着）。最终判据用**结构性约束**：`painted_groups ≤ 本帧涉及的组数`。

**量化（5 千行真文档，同机对比）**：

| 指标 | 修复前 | 修复后 |
|---|---|---|
| 编辑一帧 paint（5465 行） | 89.8 ms | **12.7 ms** |
| 连续输入（逐字符 RTT） | 86.7 ms | **15.5 ms** |
| 546 行 / 1366 行 / 5465 行 | 11.5 / 14.3 / 89.8 ms（线性恶化） | 11.3 / 12.3 / 12.7 ms（**基本持平**） |

**教训**：这类“随文件变大而变卡”的问题，靠读代码很难看出（`if (n > 4096) clear()` 看上去很无害），
必须用**多规模对照测量**（同内容 1×/¼/⅒）把“成本 ∝ 规模”变成可见曲线。

## Linux 窗口后端：如实未做的能力（2026-10-15）

X11 与 Wayland 两个后端已落地（去装饰建窗、送帧、输入、窗口控制、DPI）。以下是
**如实未实现**的部分——都返回 `Unsupported` 而不是假装成功，也都不影响主线
（界面能起、能被控制通道驱动、能截图验证）：

| 能力 | 现状 | 为什么没做 / 怎么做 |
|---|---|---|
| X11 窗口图标 | 未实现（`supports_window_icon()=false`） | 需要 `_NET_WM_ICON`（ARGB 数组属性）与 `WM_HINTS` 的 `icon_pixmap` 回退。Win32 侧已有 PNG→HICON 的实现（`platform_win32.cpp`），X11 侧要另写一份"PNG 解码 → ARGB 打包 → XChangeProperty"，工作量中等。优先级低：任务栏图标只在有 WM 的桌面会话里可见，而开发/CI 主要走无头与私有 X 显示 |
| X11/Wayland 跨进程剪贴板 | 未实现（进程内闭环可用） | X11：完整 ICCCM——`XSetSelectionOwner` + 处理 `SelectionRequest`（按 `TARGETS`/`UTF8_STRING` 应答）+ 读取时发 `SelectionRequest` 等 `SelectionNotify`（**异步**，而 `clipboard_text()` 是同步接口，需要一次有限等待）。Wayland：`wl_data_device_manager` 的 `data_source`/`data_offer` + MIME 协商。两者都不小，且服务器上没有别的桌面应用来互操作——先记在这里 |
| Wayland 窗口图标 | 未实现 | `xdg-shell` **没有** `set_icon`；标准做法是 `xdg-toplevel-icon-v3`（较新，GNOME 46+ 才支持）或退回 `_NET_WM_ICON`（仅 XWayland 路径）。需要把该协议也纳入 `tools/wayland_bindgen.py` 的输入 |
| Wayland 分数缩放 | 未实现（用整数倍 `wl_output.scale`） | `wp_fractional_scale_v1`：合成器发 `preferred_scale`（如 150→1.5×），客户端按该值渲染。真做起来还要处理 `wp_viewporter` 的 `set_destination`（缓冲逻辑尺寸 ≠ 窗口尺寸时必须用 viewport 收缩，否则画面被拉伸）。GNOME 默认整数缩放，因此当前实现覆盖了绝大多数场景 |
| Wayland 输入法（IME） | 未实现 | 需要 `text-input-v3` + `input-method-v2` 协议；当前键盘走 `libxkbcommon`，覆盖直接键入与非拉丁键盘布局，但**中文输入法的候选词窗口**不会出现 |
| X11 取窗口位置（用于 `begin_move`） | 用"最近一次输入事件的根坐标"代替现场 `XQueryPointer` | 语义等价（WM 校验收到的正是同一事件的时间戳与坐标），且省一次往返。要严格现场取值就把 `XQueryPointer` 加进符号表即可 |
