# 坑位速查（按**发生场景**索引）

> **这份文件是什么**：一张**索引**——按你「此刻在做什么」查"这类事上以前踩过什么"。
> 每条只给一句话 + **详情出处**（点进去看完整根因与代价）。
>
> **为什么需要它**：本仓的知识**不缺**，缺的是**在合适时机被想起来**。
> 实测证据（2026-10-09，见 `DOC_USABILITY_FINDINGS.md`）：一次真实实现轮里撞到
> 5 个**文档里早有记载**的坑，用到时**一个都想不起来**——因为它们按**时间**堆在
> 230 KB 的 `BACKLOG.md` 已完成段里，而人检索时是按**场景**想的。
>
> **维护约定**：只放"**会反复撞到、且撞到时想不起来**"的坑。
> 加条目用 `-` 列表，行内必须带 **`〔场景〕` 标签**与 **`→ 出处`**
> （`tools/check_docs.py` 会校验这两项，防它退化成又一个流水账）。
> 一次性的事故经过不要写这里——写 `BACKLOG.md` 已完成段，并在下面的表格里登记。

## 场景 1：刚改完代码，要跑测试/验证结论

| 坑 | 一句话 | 出处 |
|---|---|---|
| **`--force` 才重编** | `st build` 增量按 **mtime** 判定；改了源码但结论没变时**先怀疑二进制是旧的**。强制重编用 `--force`，或在改动**新增文件**后删 `st_tests` 产物 | `CONVENTIONS.md` §7.4 |
| **`st test <filter>` 不重编** | 带过滤器的 `test` 会重跑但**不重编**——"单跑绿、全量红"的经典成因。**已核实并修正（2026-10-13）：现行 `run_tests` 无条件 `compile_units`+`link`（mtime 增量），实测改源后 `st test <filter>` 正确重编重链（exe mtime > src mtime）。本条保留作为历史记录与复核方法：结论与源码矛盾时先比对 exe/src mtime** | `CONVENTIONS.md` §7.4；实测 2026-10-13（build.cpp 的 `run_tests`） |
| **产物路径有两套** | `build/bin/st`（自举产物）与 `build/dev/bin/st`（真产物）**不是同一个文件**；改了 linter 却看到旧行为时先确认跑的是哪个 | 本文件 · 实测 2026-10-09 |
| **逆向验证用 `copy2` 恢复 = 假绿** | `copy2` 把**旧 mtime** 一起搬回来（比回退版编出的 `.o` 还旧）→ 跑的是上一份二进制。恢复要**删旧文件再写** | `CONVENTIONS.md` §7.3 表格 |
| **测试结论与代码矛盾时** | 先查二进制是不是新的，**不要**先怀疑代码 | `CONVENTIONS.md` §7.4 |

## 场景 2：写回归测试 / 验证它真能抓住缺陷

| 坑 | 一句话 | 出处 |
|---|---|---|
| **假绿：桩把差异抹平** | 被测的是"谁把值传给谁"，而桩直接回了个恒等值 ⇒ 错误实现也判绿。**桩必须复现被测的那一段真实行为** | `CONVENTIONS.md` §7.2 |
| **假 ctx 的字段名与真 ctx 不同** | 测试夹具自己编了 `{name}`，而运行时 `listDir` 回的是 `{path}` ⇒ 读 `name` 恒为空却「测试全绿」，缺陷（`apps` 永远报“没有运行中的应用”）直到真实会话才暴露。夹具要用**真实现返回的形状**，不是看上去合理的形状 | `packages/agents/src/agents/shuangtian/shuangtian_tools.ts` 的 `apps` 注释 |
| **假绿：回退文本编译不过** | `-Werror` 打断编译 ⇒ 测试跑的是上一份二进制 ⇒ 假绿。回退写法要能编译过（保留 `(void)var;`） | `CONVENTIONS.md` §7.3 |
| **判据自己也会错** | 先把"什么算合格"在**一个已知样本**上验一遍，再拿去判被测对象 | `CONVENTIONS.md` §7.8 |
| **像素断言的三个坑** | 聚簇 / 步长 / 判据——不做逆向验证很容易交付"恒绿的护栏" | `CONVENTIONS.md` §7.1 |
| **`ST_CHECK` 之后跟着索引 = 分片静默失联** | 前置条件没满足时，后面那句 `v[0]` 会让 `_GLIBCXX_ASSERTIONS` 直接 `abort()`，**整个分片连同其余上千条用例一起消失**，只报“分片未能启动”而**一条失败记录都没有**。前置条件用 `ST_REQUIRE`，诊断打印要在断言**之前** | `tests/lsp_client_test.cpp` 的 `fake_ready` 处注释 |
| **假 server 用 `sh` 脚本 = 只在 POSIX 成立** | Windows 的 `CreateProcessW` **不认 shebang**（报“%1 不是有效的 Win32 应用程序”），而用例里还跟着索引空 vector——两者叠加就是上一条。跑脚本的用例要么标 `ST_TEST_SLOW` + 平台跳过，要么换成真程序 | `tests/lsp_client_test.cpp` 的 `FakeServer` 注释 |
| **“0 命中”与“读不到”必须是两个数** | “替换了 0 处”至少三种成因：目标不是磁盘文件 / 文件读失败 / 真没命中。只报一个总数时，排障要重跑一遍搜索才能区分。把**次因**（目标数/读失败数/无命中数）一并报出 | `examples/gbcode/main.cpp` 的 `replace_in_workspace` 注释 |
| **回归测试要调生产函数** | 测试里“自己做一遍正确逻辑再断言”= 测的是测试自己；回退生产实现后依然全绿（实测：崩溃报告文件名那条，回退后仍 PASS；改成调 `crash_report_file_name` 后回退立刻变红） | `tests/core_log_file_test.cpp` 的 `log_crash_report_filename_has_no_colon` 注释 |

## 场景 3：改观感（字体/配色/动画/手感）

| 坑 | 一句话 | 出处 |
|---|---|---|
| **验收标准长在用户身上** | 这类改动的判据不是测试红绿——**先读手册再动手** | `perceptual_changes.md`（全篇） |
| **量尺三坑** | 判据要自适应底色 / 取样区避开自己造的边界 / 多实例改善量应一致（各不一致 = 量法失效） | `CONVENTIONS.md` §9.1.1 |
| **至少两个正交量同向才算更好** | "整体更锐"与"彼此更均匀"是**两个问题**，只看前者会把反向改动包装成进步 | `perceptual_changes.md` |
| **结论与观感冲突时先怀疑尺子** | 本线的每一次"用户说更差"都对应一次**口径缺陷** | `perceptual_changes.md` |

## 场景 4：碰系统 API / 路径 / 入口（跨平台）

| 坑 | 一句话 | 出处 |
|---|---|---|
| **系统头只能在 `platform_*`** | 业务代码里不准出现平台宏/平台 API；lint L14 会拦（含 `::getpid` 等 POSIX 直调） | `CONVENTIONS.md` §10 第 1 条 |
| **路径一律 UTF-8 且走 `st::fs`** | 不要手写 `base + "/" + leaf`、不要 `ifstream(std::string)`（Windows 按 ANSI 解释，中文必坏） | `CONVENTIONS.md` §10 第 2 条 |
| **入口用 `ST_MAIN(fn)`** | Windows 的 `argv` 是 ANSI，中文参数会乱 | `CONVENTIONS.md` §10 |
| **改了平台分支就要交叉编译** | 本机（Linux）看不出 Windows 分支的问题，`--toolchain=mingw` 是唯一发现途径 | `CONVENTIONS.md` §10 |
| **同一份路径两种分隔符 = 静默丢数据** | `LspClient::uri_to_path` 在 `_WIN32` 下把 `/` 换 `\`，而应用侧 `active_path` 是 `st::fs` 的 `/` 形态——`map` 查不到 key 就返回空表，**不报错**。两侧的 key 必须过同一个归一化函数 | `examples/gbcode/lsp_bridge.cpp` 的 `normalize_path` 注释 |
| **Win32 的鼠标消息不带修饰键** | `WM_LBUTTONDOWN` 的 `wParam` 只有 `MK_SHIFT`/`MK_CONTROL`（无 Alt），只读它会得到“Ctrl 有、Alt 恒无”的半吊子事实。鼠标事件一律走 `GetKeyState`（与键盘同一口径） | `src/shell/platform_win32.cpp` 的 `push_mouse` 注释 |
| **`CreateIconFromResourceEx` 的错误码会骗人** | 手拼 `RT_ICON` 字节失败时它只回 `ERROR_FILE_NOT_FOUND(2)`——与“文件”毫无关系。改用 `CreateDIBSection` + `CreateIconIndirect`：输入是两个真位图句柄，不存在“字节布局对不对”这类无法定位的问题 | `src/shell/platform_win32.cpp` 的 `make_icon` 注释 |
| **文件名不能直接用 ISO 8601 时间戳** | `2026-10-10T14:18:33.362Z` 含 `:`，而 `:` 在 Windows 文件名里非法 ⇒ `open` 失败 ⇒ **崩溃报告永不生成且无提示**（失败发生在启动时，用户看不到）。文件名里把 `:` 换成 `-` | `include/st/core/entry.hpp` 的 `crash_report_file_name` 注释 |
| **`SYMBOL_INFO` 不能当普通数组用** | 它的 `Name` 是**柔性数组**：必须整块分配并设 `SizeOfStruct`/`MaxNameLen`。直接 `reinterpret_cast` 一个 `char[N]` 会让 `SymFromAddr` 恒失败，看起来像“二进制没符号” | `src/core/platform_crash.cpp` 的 `walk_stack` 注释 |
| **`addr2line` 要 link-time 地址** | 传模块内偏移恒得 `??:0`；要加上 PE 首选基址 `0x140000000`。且 DbgHelp 只读 PDB（读不了 MinGW 的 DWARF）——MinGW 构建下自己的帧必然要它兜底 | `src/core/platform_crash.cpp` 的 `walk_stack` 注释 |
| **本机链接的库看顶层 `system_libs`** | `toolchains.<名>.system_libs` 只在**交叉编译**时**整体接管**；本机构建根本不看它。加 Windows 系统库（`dbghelp`/`psapi`）时两处都要写 | `st.pkg` 的 `//system_libs` 注释 |
| **改绘制原语前先确认有几份实现** | `Surface` 的绘图原语在 **`Canvas`（软件）与 `platform_d3d11.cpp`（GPU）各有一份**。只修一份时：headless 回归**全绿**，真窗口**一模一样**——看上去“修好了”。实测：`draw_canvas_at` 只修 Canvas 那侧，截图哈希**与修复前逐位相同**。判据：真窗口验证不要用 headless 截图，**比像素哈希** | `src/raster/canvas.cpp` / `platform_d3d11.cpp` 的 `draw_canvas_at` |
| **`draw_canvas_at` 的 x/y 是物理像素，`draw_canvas` 的 rect 是逻辑** | 两者混用会**多乘一次 `device_scale`**，贴图向右下溢出（DPI 1.5 下 18→偏 9px）。症状看上去是“图标没居中”，去改布局算式会白跑 | `Canvas::draw_canvas_at` 注释 + `tests/raster_canvas_at_scale_test.cpp` |
| **“看着修好了”必须用哈希对** | 两张人眼看着不同的截图可能**逐位相同**（我报过一次“修好了”，MD5 一模一样）——两次截的是同一个未重启的进程，或修复根本没进渲染路径 | `shuangtian_capture_hash` / `Get-FileHash` |
| **改构建系统的开关必须看真实命令行** | `st.pkg` 的开关要先变成编译器**看得见的东西**才算数。实测三个静默失效：加进 `flags` 而非 `defines`（不进命令行）/ 往还不存在的 `effective` 里 push（被后面整体覆盖）/ `defines` 要**裸名**却写了 `-D` 前缀（变成 `-D-DST_…`）。三者都不报错，只能靠 `--verbose` 逐字读命令行发现 | `src/pkg/build.cpp` 的 `ST_LOG_DISABLED` 段注释 |
| **“代码明明改了却没生效”先查工具自身二进制** | `st.exe` 自己也是被构建出来的。改了 `src/pkg/*` 后 `st build X` **不会**重建 `st.exe`——它拿旧版本跑，于是开关全部静默失效。实测：`st.exe` 是 10/9 的、`build.cpp` 是 10/10 的，白查三轮。先 `st build st --profile dev`（且 `st.exe` 在运行时无法覆盖，要用 `build/bin/st.exe` 那个自举副本） | `CONVENTIONS.md` §7.4 的延伸 |
| **`log::info(` 批量替换会咬到 `st::log::info(`** | 正则 `\blog::info\(` 在 `st::log::info(` 上也匹配，替换后得到 `st::ST_LOG_INFO(`（不存在的宏）。替换后必须全局复查 `::ST_LOG_` 这类残留 | `src/core/entry.cpp` 等 |
| **宏名与函数名同名会自引用** | `#define ST_LOG_INFO(...) st::log::info(__VA_ARGS__)` 里 `info` 再次被同名宏替换，展开成 `st::log::ST_LOG_INFO(...)`——报 `expected unqualified-id`。要加一层 `ST_LOG_DETAIL_*` 间接 | `include/st/core/log.hpp` 的宏段注释 |

## 场景 5：用脚本驱动控制通道 / 写 e2e

| 坑 | 一句话 | 出处 |
|---|---|---|
| **先 `hello` 携带 token** | 控制通道拒绝任何未带 token 的请求；漏了这一步第一条 `find` 就挂 | `tools/gbcode_e2e.py` 的 `client.ok("hello")` |
| **驱动控制通道：别自己写 TCP 客户端** | 现成的是 `tools/st_client_lib.py`（`load_control` + `call_with_token`，已处理握手/帧累积/分片）。手搓会重新踩 token、四字节长度前缀、`header += chunk`（写成 `header += header` 会卡在 recv）三件事 | `tools/st_client_lib.py` 头注释 |
| **`host:port` / 纯端口两种 target 不带 token** | 只能传**控制文件路径**（或省略 target）；带鉴权的实例上按 `host:port` 传必然在握手阶段被拒，而报错看着像“token 不对”、不像“寻址方式不对” | `packages/agents/src/agents/shuangtian/shuangtian_client.ts` 的 `resolve_target` |
| **智能体工具寻址与 e2e 脚本是两套入口** | 工具侧现在会从同端口的控制文件自动补齐 token、并扫描 `.shuangtian/*-control.json` 发现实例；写 e2e 脚本时这些**不会**自动发生 | 同上 |
| **Windows `subprocess` 默认 GBK** | 仓库输出是 UTF-8；不显式 `encoding="utf-8"` 会抛解码错，异常被吞后一律报 unknown（**差点把工具问题当成代码问题**） | 本文件 · 实测 2026-10-09 |
| **截图落盘白名单不递归** | `capture` 只允许写到 temp / 可执行文件目录 / **控制文件所在目录**（= `<会话>/.shuangtian/`，前缀比较、不下钻）。传 `<会话>/qa/x.png` 会被服务端拒；智能体工具会自动改落到 `.shuangtian/shots/` 并在输出里注明“已改落” | `src/control/server.cpp` 的 `default_capture_dirs` |
| **`count` 是“回了几个”不是“命中几个”** | `find` 的 `count` = 返回条数（受 `limit` 截断），`total` = 命中总数。只看 `count` 会把“命中 20+”读成“命中 20”（实测：小模型据此误解返回结构） | `DESIGN.md` §6.2 `find` 行 |
| **“报成功”不等于文件在** | 自己写探针时，正则对中文尾注（如 `（区域 {…}`）会贪婪地吞进路径，于是“文件不存在”是探针的错、不是工具的错（已实测踩到）。判落盘用 `ls`/`find` 复核 | 实测 2026-10-13 |
| **验证 UI 读组件自报的值** | 不要信旁路探针（截图/像素统计）——两者口径不同 | `CONVENTIONS.md` §7.5 |
| **合成输入不一定触发交互态** | 下结论前先确认**前提状态成立**（悬浮/按下/焦点） | `CONVENTIONS.md` §7.6 |
| **真窗口实测用 `tools/st_win_input.py`** | 别自己拼 PowerShell（三条注入通道各有适用范围） | `CONVENTIONS.md` §7.7.1 |
| **先清掉自己留下的前台程序** | 否则被控应用的前台态判断会被上一个实例污染 | `CONVENTIONS.md` §7.7.3 |

## 场景 6：改构建系统 / 依赖 / 缓存

| 坑 | 一句话 | 出处 |
|---|---|---|
| **构建系统的三个静默失效点** | 改头文件不重编 / 标志与缓存键不一致 / 平台漏开关——**什么也不报，只是不重编** | `CONVENTIONS.md` §10.3 |
| **两个构建不能并行** | 后者会以 `io: rename ... Resource device` 失败 | `CONVENTIONS.md` §6 附近 |
| **判据落在产物存在性上** | 不要用退出码/日志当代理信号（实测：PCH 创建"报成功"其实静默空转） | `DESIGN.md` §8.2 第 49 条 |
| **自举单元集必须从清单推导，不能按目录写死** | bootstrap 写死了 `src/core src/ext src/pkg`，与 `st.pkg` 脱节——新增 `src/ext/video.cpp` 引 `codec/raster` 后，报的是链接期一串 `undefined reference to st::...`（**报错指向源码，真因是"该编的单元没编"**）。现在按清单推：`sources − targets.st.exclude_sources + targets.st.sources` | `bootstrap.sh` / `bootstrap.ps1` 的相邻注释 |
| **排除模式 `src/ui/*` 不跨子目录** | `*` 不跨 `/`（与 `fs::match_glob` 同口径），要跨得写 `src/ui/**`。写错的症状是**排除静默不生效**（只警告"未命中"），于是 `EXCLUDE` 形同虚设 | `src/core/fs.cpp` `match_segments` |
| **工具链只该编它真需要的单元** | `st` 一行渲染都不跑，却被 `core/platform_crash.cpp → ui/state_trace.hpp` 与 `shell → ui/window_control.hpp` 两条**头文件级**反向依赖拖进整条渲染链（codec/raster/text/md/xml/shell）。见 `BACKLOG.md` P1 | `include/st/ui/window_control.hpp` 注释 |
| **自举把"应用向"单元也编进来会撞构建期生成头** | `src/ui/script_host.cpp` / `declarative_host.cpp` include **构建期生成**的 `battery/embed.hpp`（由 `st build` 产出）——自举阶段它还不存在，编它们必然 `fatal error`。这两条又正好是"工具不需要的应用层" | `src/pkg/embed.cpp` 的 `generate_embeds` |
| **`%lld` 与 `int64_t` 在 LP64 上不是同一类型** | Linux/macOS `int64_t` 是 `long`、Windows 是 `long long`。`sscanf("%lld", &int64_var)` 只在 Windows 合法，Linux 上 `-Wformat` + `-Werror` = **构建失败**（实测：WSL 跑 bootstrap 时刷屏）。用 `long long` 承接后显式转换 | `src/ext/video.cpp` 的 `parse_probe_output` 注释 |

## 场景 7：写声明式界面（`dsl`）

| 坑 | 一句话 | 出处 |
|---|---|---|
| **三件必知** | `mount` 是单根替换 / `pump_async()` 必须先于 `dirty()` 判断 / 构建期存的元素指针不可跨帧用 | `declarative.md` §5.5 |
| **每帧无条件 `set_*` 会抹掉控制通道设的值** | 只在"值真变了"时写；持久配置只在首次装上时设一次 | `examples/gbcode/main.cpp` 的 `editor_configured_` 注释 |
| **`add_trailing_button` 在 configure 里会每帧追加一个** | 可变追加型接口（push 进组件状态）碰上按 key 复用的声明式重组 = 状态堆和（实测：md 预览钮越点越多）。**声明式里必须用 `begin/sync/finish_trailing_sync`**（目标态对齐，框架已内化该惯例） | `DESIGN.md` §4.2.17b/§4.2.17d；`include/st/ui/components/tabs.hpp` 尾部区说明 |
| **滚轮步长从主题取，别自己写常量** | 各组件自定步长会漏乘（实测：MarkdownView 一格 1px，滚一屏百格）。`Metrics::wheel_step`（48px/格）是唯一口径；按行滚的组件（终端/合并视图）除外 | `include/st/ui/theme.hpp` 的 `wheel_step`；`DESIGN.md` §4.2.17d |
| **同位类型切换会静默丢元素级状态** | 位置对齐遇类型不符 = 销毁旧元素新建（换页合法），但文本/滚动/展开态全丢，且应用侧「已装载」类判据不会自动失效（实测：md 预览切回源码空白）。看 `ReconcileStats::type_switches` + 日志定位；状态判据要绑定实例而非 key | `DESIGN.md` §4.2.17c/§4.2.17d |
| **`apply_theme` 每次布局都跑** | 在 configure 里写 `style().background` 会**被主题覆盖**（实测：色块看着像没画） | `examples/louyue/canvas_view.hpp` 的 `Swatch` 注释 |
| **自绘组件不必与 `Element` 成员同名** | `hit_test`（返回 `bool`）已被基类占用；自己的命中判定要另起名字（`grab_at`） | `examples/caiyun/views.hpp` 注释 |
| **一个条件分支里声明多个视图 = 切走即销毁** | 切换会丢掉滚动位置/输入内容/展开态。要「切回来还是原样」就用**常驻声明 + `set_visible(false)`**（框架在绘制/命中/布局三处都跳过不可见元素），并配“首次访问才声明”避免启动首帧白建全部 | `examples/gbcode/main.cpp` 的 `build_sidebar` 注释 + `tools/gbcode_sidebar_keepalive_e2e.py` |
| **`RenderContext` 持 `const Theme&`——别绑临时对象** | `RenderContext{Theme::light(), …}` 绑的是**临时对象**，语句/函数结束后引用悬垂；读到的是被复用的栈内存（实测：接上 `Metrics::wheel_step` 后读到 **0**，滚动一次性跳到底）。主题必须比 context 活得长（`static` 或外层变量） | `include/st/ui/element.hpp` 的 `RenderContext`；`tests/ui_scroll_test.cpp` 的 `make_tall_scroll` |
| **量宽与折行必须同口径** | `measure_width(整串)` 含字距调整/连字，**比逐码点累加小**；用它定列宽而用逐字累加判溢出，临界处差一两个字符（实测 `batterycenter/embed` → `embe\|d`）。另：`wrap` 把**空格也计入 `pen`**，按词扫描会丢掉它——保底宽要留余量 | `src/text/text.cpp` 的 `TextRenderer::wrap`；`markdown_view.cpp` 的 `word_floor_of` |
| **图标描边要传 `0.0f`（随盒缩放）** | `Icon::draw` 的 `stroke_width` 默认 **2.0f 固定像素**；小盒（≤15px）里比声明值粗一大截、笔画糊成一团（实测 Tabs 尾部钮“图标太丑”）。`IconView`/`Button` 都传 `0.0f` = 按 `stroke/24 × 盒径` 缩放 | `include/st/ui/icon.hpp` 的 `draw`；`src/ui/components/tabs.cpp` 尾部钮 |
| **视图“常驻”后，渲染期里那些只在“切过去那一帧”跑的代码会变成每帧跑** | 典型是渲染期同步起子进程（`process::run`）——它会踩坏声明式树。改为在构建函数里**置一个待办标记**、帧首泵消费 | `examples/gbcode/main.cpp` 的 `git_probe_wanted_` 注释 |
| **容器 measure 给子元素的约束必须与 arrange 几何一致** | `arrange` 只给 rect、**不重 measure**；measure 阶段给错就永远错。实测：`SplitView` 在有界约束下仍用 unbounded 去测子面板 → 内容型组件（MarkdownView/CodeEditor）落回自己的兑底常量，表现为「面板拉伸/拖分割条时文字不重排」。写自定义容器时：按最终几何给**有限**约束 | `src/ui/components/split_view.cpp` 的 `measure`；`DESIGN.md` §4.2.17f |
| **调试布局“宽变了但内容不变”时先分开看两个量** | 控件宽（`bounds`）与**排版用的内容宽**是两回事。`MarkdownView` 有只读 `layout_width` 专供此用——实测就是靠它发现“排版宽恒为 608”（= 兑底常量），一步定位到是约束没传下来 | `DESIGN.md` §4.2.17f；`markdown_view.cpp` 的 `layout_width` 属性 |
| **同级分支声明子元素会按位置回收** | 切分支时，新分支里没显式设的属性（icon/width）会**沿用旧分支的值**（实测：远程页的按钮带着日志页刷新钮的 `icon=refresh`）。给每个分支包一层**带 key 的容器** | `DESIGN.md` §4.2.13 |
| **`Input` 在列里 `grow` 会吃掉剩余高度** | 提交框被拉成三行高的大框、旁边的计数被压成“已…”。输入框要包在**行**里再 `grow` | `DESIGN.md` §4.2.13 |
| **列表行用 `Button` 要显式左对齐** | 默认居中会让几十行左缘参差不齐（看着像一堆独立按钮），而列表是拿来“扫”左缘的 → `Button::Align::Start` | `include/st/ui/components/basic.hpp` 的 `Align` 注释 |
| **焦点环只在键盘导航时出现** | 鼠标按下也会给焦点，若只看 `focused_` 就画环，点一下按钮就留一圈边框。判据是**全局**状态（“上一次输入是键盘还是鼠标”）→ 放在宿主（`HostFocus::focus_ring_visible`），组件画环前问它；默认真（程序化聚焦仍画，同 CSS `:focus-visible`） | `DESIGN.md` §4.2.17 |
| **`MouseDown` 命中不到时不要清 `pressed_`** | 清了会让后续 `MouseMove` 失去拖拽归属（滑块/滚动条拖不动）。同一支里也不要漏 `target->set_pressed(true)`——组件的“按住拖动中”靠它判 | `src/ui/ui_root.cpp` 的 `MouseDown` 分支注释 |
| **计时类行为别在事件回调里拿时间戳** | 事件分发期读到的是**上一帧帧首**的 `time_seconds`；拿它当基准会算出“从上一个帧首到现在”这个巨大 delta，延时被整帧跳过（实测 816ms，气泡 2ms 就弹）。修法：在**帧**里立基准 | `DESIGN.md` §4.2.18 的两个计时陷阱 |
| **`hide_*()` 的早期返回会吞掉状态重置** | “本来就没显示就早返回”是合理优化，但调用方若依赖它顺带重置的字段（时间基准），就会静默失效——重置与早返回要分开写 | `src/ui/ui_root.cpp` 的 `set_hint_target` |
| **扫标签时 `>` 要先跳引号** | `<A label="a > b"/>` 里的 `>` 不是标签结束。直接 `find('>')` 会把标签截成 `<A label="a >`，属性值变成 `a >`、后续全乱（本仓 `st/xml` 第一版就错在这里） | `src/xml/xml.cpp` 的 `find_tag_end` |
| **容错解析器也要留诊断** | 自动收口未闭合标签是好事，但**默默**收口会让“少写一个 `</Row>`”永远看不到（树形看着正常，只是内容跑到了另一层）。容错 + 报行号，两者不矛盾 | `DESIGN.md` §4.2.19 |
| **`on(sel, 未知事件名, fn)` 会静默永不触发** | 绑定时不校验事件名，现象是“点了没反应”，而根因要一路查到事件表。**现在注册时直接报错**（可用名：click/dblclick/mousedown/…） | `src/ui/script_host.cpp` 的 `is_known_event` |
| **重建式渲染下事件会重复绑定** | `clear()` + 重建行时，新元素用**同一个 id**，而旧绑定还在宿主里——一次点击调两次处理器（`toggle` 两次 = 改回原值，看起来“没反应”）。句柄 `.on()` 现已内置“同 id 同事件先解旧” | `src/ui/script_api.js` 的 `boundKeys` |
| **`id` 不在属性面里** | `id`/`key` 是元素元数据（不在 `property_names()`），而 `apply_properties` 只认声明的名字——传给 `ui.build` 的 `id` 被静默忽略，然后被自动 id 覆盖（`auto-Panel`）。现在框架级可写（`id`/`key`） | `src/ui/actions.cpp` 的 `apply_properties` |
| **`WindowFrame::content()` 必须 grow** | 它是窗框（列布局）的第二个孩子，不 grow 就只占“内容高”——窗口底下一大片空白，而调用方看自己的子树是满的，极难定位 | `src/ui/components/window_frame.cpp` |
| **建树后立刻按 id 查会落空（快照是上一帧的）** | `handle.append` 返回 `$(新 id)`时，快照还没包含新元素 → 拿到“空句柄”（`exists:false`），调用方报 `not a function`。直接用桥返回的信息造句柄，不查快照 | `src/ui/script_api.js` 的 `handle.append` |
| **隐藏分支的“结构”必须与显示分支一致** | 显示时直接声明子元素、隐藏时又包一层带别的 key 的容器 = 结构随状态变 ⇒ 子树被销毁重建（终端一重建就丢会话）。两个面板都直接声明、用 `set_visible(false)` | `DESIGN.md` §4.2.15 |
| **自绘组件报尺寸前要防抖、退化尺寸不报** | 面板收起那一帧可用高度归零，若被 `max(N, …)` 夹一下真报给 PTY，屏幕就被截（实测 14 行 → 11 行）。退化尺寸跳过；**防抖的“确认”必须放在每帧都跑的地方**（`paint_content`），放 `arrange` 里会因布局不脏而永远不确认 | `DESIGN.md` §4.2.16 |
| **“点活动栏 = 打开”是错的** | 面板默认就是开的，点一下是**收起**。判断可见性要看状态（`#bottom-split` 的 `second_hidden`），否则探针会自己制造“功能坏了”的假象 | `tools/gbcode_terminal_scroll_e2e.py` 文件头 |

## 场景 8：画自绘组件 / 写光栅管线

| 坑 | 一句话 | 出处 |
|---|---|---|
| **合成入口必须传 `blend`** | `Canvas::draw_canvas` 曾忽略 `DrawOptions::blend`，八种混合模式静默退化成普通叠加——**界面完全看不出** | `DESIGN.md` §5.0.5；`tests/raster_compose_blend_test.cpp` |
| **自绘几何从 `bounds_.x/y` 起算** | 画布是**视口绝对坐标**；按局部坐标画会整块位移 | `include/st/ui/element.hpp` `paint_content` 注释 |
| **小尺寸图标只能承载少量大特征** | 多段曲线相接会把墨堆成一团；直线优于曲线（20px 下尤其明显） | `src/ui/icon.cpp` 图标表注释 |
| **渲染要保证确定性** | 持续动画的验证靠**帧哈希**（同帧两次相同、异帧不同），不靠"看着在动" | `examples/caiyun/views.hpp` 的 `frame_hash` 注释 |

## 场景 9：下结论 / 归因（元层面，最贵的一类）

| 坑 | 一句话 | 出处 |
|---|---|---|
| **结论与实测矛盾时先怀疑尺子** | 本轮三次"指标说代码不对"实际都是量法错（手写小数、选色前提不成立、脚本编码） | `DOC_USABILITY_FINDINGS.md`；`DESIGN.md` §8.2 第 49 条 |
| **"整体更锐"≠"彼此更均匀"** | 网格拟合那次：旧指标一路涨而用户每次说更差——它把两个问题混成一个 | `perceptual_changes.md` |
| **别把框架缺陷当"用法不对"** | 判定方法：去读框架源码里那个函数——内部自相矛盾就是缺陷（实测：坐标算错被绕了三四种写法） | 子Agent 提示词「发现框架问题时」 |
| **先修测量口径，再谈归因** | `PAINT_DIAGNOSIS.md` §0 就是"先修一个测量口径缺陷，否则所有归因都是错的" | `PAINT_DIAGNOSIS.md` §0 |

## 场景 10：写文档 / 提交

| 坑 | 一句话 | 出处 |
|---|---|---|
| **数字必须实测且注明口径** | 用例数有"源码声明 1062 / 实跑 994 / `--list` 1077"三个口径，混用会对不上 | `docs/README.md` 写作约定 |
| **状态以代码为准** | 写着"待做"却已完成会误导（实测：`LOUYUE_CAIYUN.md` 在实现后仍标"代码未动"） | `docs/README.md` 写作约定 |
| **章节号是接口** | 被源码注释引用；重编号会静默打断论证链（先 grep `§`） | `docs/README.md` 写作约定 |
| **待做项必须在 P0/P1/P2 章内** | 散在历史章里肉眼扫不到 = 不存在（实测 39 条散落）；`check_docs` 会拦 | `BACKLOG.md` 头部结构约定 |
| **新知识写哪份** | 框架缺陷→BACKLOG / 方法→专题手册 / 子Agent 纪律→提示词 / 改动缘由→git 提交 / 一次性过程→**不要单开文件** | `docs/README.md`「新知识该写哪份」 |

## 场景 11：构建期与跨层功能（2026-10-11 清账轮沉淀）

| 坑 | 一句话 | 出处 |
|---|---|---|
| **构建报错可能完全指不到真因** | 「无法启动 'g++.exe': 文件名或扩展名太长」看着像文件名问题，真因是**命令行总长**超 `CreateProcessW` 上限；查到它花了数轮 | `BACKLOG.md`「顺带根治的构建期真缺陷」 |
| **时好时坏的构建失败先怀疑"规模临界"** | 同一命令有时成功有时失败 → 不是环境抖动，而是"取决于多少单元需重编"的临界；当成抖动会白等很多轮 | 同上 |
| **「长得像 X 就当成 X」是启发式的经典错法** | 用"含 `.o` 或 `build/` 就当对象文件"挑参数，把 `-o` 的产物路径也挑走了 → 报 `could not open ...exe`（看着像产物不存在）。**该由调用方显式给出，不该猜** | 同上 |
| **同一件事在多个入口各写一份＝口径必然分家** | `dbghelp`/`nanosrv` 在自举脚本、框架清单、默认库表三处各不相同，导致独立工程链接**长期失败**。看到"三个入口同一份清单"就该收敛到一处。**已收敛（2026-10-12）**：五处清单（bootstrap.ps1/sh、st.pkg 两处 toolchains、build.cpp 默认表）统一到仓库根 `link-libs.txt`（纯文本按平台分节），自举脚本与 `default_system_libs` 都读它，读不到时用同值内置兑底（独立工程场景）。加新系统库只改 link-libs.txt 一处 | `link-libs.txt` 头注释；`build.cpp:default_system_libs` |
| **Windows 上 `core.autocrlf=true` 会让补丁对不上** | 工作区 CRLF、`git diff` 给 LF → 补丁报 `does not apply`；表现极具误导性（**手工验证 OK、脚本里失败**） | `tools/partial_patch_e2e.py` |
| **数据算对了、界面没接上** | B6 副行（`List::Entry` 缺字段→`dsl` 的值被静默丢弃）与 B7 行内区间各撞一次。**跨层功能必须有一层断言"它在下一层真的出现"** | `BACKLOG.md` 已完成段的方法论前言 |
| **有权威来源的状态，别在本地再存一份** | 组件自维护 `hex_focused_` 而 `TextInput` 只投给 `UiRoot::focused_` → 「点了输入框敲字没反应」。改用框架焦点 + 未上树时退化 | 同上（B10） |
| **无头/未就绪时"量尺"返回 0，别据此算布局** | `TextPort` 为空时 `measure()` 得 0，品牌区宽度退化成 0；改用**保守估宽**（不需要量尺） | 同上（B4） |
| **新增一个测试/源文件可能整批构建失败** | 单元数逼近命令行上限时，**先修构建器**（响应文件退路），否则每个新文件都在赌运气 | 同上 |
| **行号/偏移有三种坐标，混一次就全错** | unified diff 的 hunk 头行号（1 起）/ `hunk.lines` 下标（0 起）/ 编辑器 model 行（1 起）；霜天的 `DiagnosticMark` 与 `selection()` 是**字节**偏移 | `DESIGN.md` §4.2.13、`code_editor.hpp` |
| **极窄元素上"边缘带"会吃掉整个本体** | 2px 宽片段的左右边缘带各占 1px，中点两边都满足 → 「想移动却变成修剪」。宽度要**严格小于半宽**，判据用半开区间 | `src/ui/timeline.cpp` `hit_test` |
| **吸附不能吸到自己** | 不排除被拖的那个元素，它会永远吸在原位 → 「拖不动」。阈值要用**像素**（用毫秒会让"放大后吸附极难触发"） | 同上 `Model::snap` |
| **合并里"两侧改成同一内容"不是冲突** | 当冲突处理会让用户在两份一模一样的内容里选，纯属噪音 | `include/st/text/merge.hpp` |

## 场景 12：测试与验证路径（2026-10-11 C 轮沉淀）

| 坑 | 一句话 | 出处 |
|---|---|---|
| **用例里的未捕获异常会带走整个测试进程** | `Json::get<int>()` 对 null **抛**（不是断言失败）；逃到用例线程入口 = `terminate` ⇒ 整套 1268 条用例在第 800 条处消失，前面几百条 PASS 全部作废。框架层已加异常边界（记 FAIL 并继续） | `src/test/test_runner.cpp` |
| **"分片绿"不等于"测试全绿"** | `--test-jobs N` 分片**碰巧**避开某些用例；我这一轮先信了分片结果，后来才发现单进程全跑必崩。**判据要用最长的那条路径（全跑）** | `BACKLOG.md` C 轮说明 |
| **崩溃会掩盖真正的缺陷** | `expose` 挂错命名空间 → 后续断言拿到 null → 抛异常 → 整进程 terminate。于是报出来只是"跑到某处崩了"，而"`app.add` 不是函数"这个真因被藏了很久。**先修异常边界，缺陷自己会浮出来** | 同上 |
| **二进制比源码旧时，验证是假的** | `st build` **不包含**测试目标；`st test` 才会编它。删掉 `st_tests.exe` 后重建期间又跑了几轮"验证"，读到的全是旧行为（一度以为"改了没用"） | `src/pkg/build.cpp` |
| **文件被占用时 `ld` 报 permission denied** | 后台任务（gdb / 长跑的测试）握着 exe；`Get-Process` 未必显示得到。**先 `bg_task action=list` 看有没有在跑的东西** | 实测 |
| **整进程崩溃时 JUnit 报告也不会写** | 写报告在跑完之后；崩了就没有报告 ⇒ CI 上表现为"没有结果"而不是"有失败" | `src/test/test_main.cpp` |
| **异常边界只该包"用例体"** | 包住用例体（`item.body()`）而不包整个 runner：框架自己的簿记出问题应当照常崩（那是框架 bug，不该被当成用例失败） | `src/test/test_runner.cpp` |

## 场景 13：把"数据层已完成"当交付物

| 坑 | 一句话 | 出处 |
|---|---|---|
| **数据算对了，用户看不见** | 两次清账里反复出现：`Model`/`plan` 全绿而**界面上什么都没有**。跨层功能必须有一层断言"它在下一层真的出现"（像素级） | `BACKLOG.md` 两轮清账 |
| **界面层的假绿比数据层更隐蔽** | `set_x(true)` 返回 true ≠ 画对了。用 `Canvas` 取像素断言（开关两种状态画面**不同**），比断言 setter 值强得多 | `tests/ui_diff_staging_test.cpp` |
| **辅助函数的过滤条件也是假护栏** | `patch_payload` 第一版只收 `+`/`-` 开头的行，于是"未选中的删除行降级成上下文"这条断言**永远失败**——而补丁其实是对的（探针打出来明明白白有一行 ` old2`） | 同上 |
| **先后两条一起跑才崩 = 跨用例状态污染** | 单跑全过、连跑必崩。定位靠 `--shard i/N` 缩小 + gdb 抓抛出点，**不要**先怀疑"环境抖动" | `BACKLOG.md` C 轮 |
| **`-Wshadow`/`-Wconversion` 是结构错误的探测器** | 手工搬移代码块时括号数错 ⇒ `paint_content` 的行循环被提前关闭 ⇒ 后面几十条 `-Wshadow` 报"变量遮蔽"。**编译器先于运行期发现了它** | `src/ui/components/code_editor.cpp` |

## 怎么用这份速查

**开工前**：扫一眼你**当前场景**那一节（30 秒）。
**卡住时**：先来这里查"这类事以前踩过什么"，再去出处看详情。

> 若某条让你省下了往返，**回来给它加出处链接或补一条**——
> 这份表的价值就是"下一个撞同一堵墙的人能查到"。
