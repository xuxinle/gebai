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
| **`st test <filter>` 不重编** | 带过滤器的 `test` 会重跑但**不重编**——"单跑绿、全量红"的经典成因 | `BACKLOG.md`「`st test <filter>` 不重编」 |
| **产物路径有两套** | `build/bin/st`（自举产物）与 `build/dev/bin/st`（真产物）**不是同一个文件**；改了 linter 却看到旧行为时先确认跑的是哪个 | 本文件 · 实测 2026-10-09 |
| **逆向验证用 `copy2` 恢复 = 假绿** | `copy2` 把**旧 mtime** 一起搬回来（比回退版编出的 `.o` 还旧）→ 跑的是上一份二进制。恢复要**删旧文件再写** | `CONVENTIONS.md` §7.3 表格 |
| **测试结论与代码矛盾时** | 先查二进制是不是新的，**不要**先怀疑代码 | `CONVENTIONS.md` §7.4 |

## 场景 2：写回归测试 / 验证它真能抓住缺陷

| 坑 | 一句话 | 出处 |
|---|---|---|
| **假绿：桩把差异抹平** | 被测的是"谁把值传给谁"，而桩直接回了个恒等值 ⇒ 错误实现也判绿。**桩必须复现被测的那一段真实行为** | `CONVENTIONS.md` §7.2 |
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

## 场景 5：用脚本驱动控制通道 / 写 e2e

| 坑 | 一句话 | 出处 |
|---|---|---|
| **先 `hello` 携带 token** | 控制通道拒绝任何未带 token 的请求；漏了这一步第一条 `find` 就挂 | `tools/gbcode_e2e.py` 的 `client.ok("hello")` |
| **Windows `subprocess` 默认 GBK** | 仓库输出是 UTF-8；不显式 `encoding="utf-8"` 会抛解码错，异常被吞后一律报 unknown（**差点把工具问题当成代码问题**） | 本文件 · 实测 2026-10-09 |
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

## 场景 7：写声明式界面（`dsl`）

| 坑 | 一句话 | 出处 |
|---|---|---|
| **三件必知** | `mount` 是单根替换 / `pump_async()` 必须先于 `dirty()` 判断 / 构建期存的元素指针不可跨帧用 | `declarative.md` §5.5 |
| **每帧无条件 `set_*` 会抹掉控制通道设的值** | 只在"值真变了"时写；持久配置只在首次装上时设一次 | `examples/gbcode/main.cpp` 的 `editor_configured_` 注释 |
| **`apply_theme` 每次布局都跑** | 在 configure 里写 `style().background` 会**被主题覆盖**（实测：色块看着像没画） | `examples/louyue/canvas_view.hpp` 的 `Swatch` 注释 |
| **自绘组件不必与 `Element` 成员同名** | `hit_test`（返回 `bool`）已被基类占用；自己的命中判定要另起名字（`grab_at`） | `examples/caiyun/views.hpp` 注释 |

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

## 怎么用这份速查

**开工前**：扫一眼你**当前场景**那一节（30 秒）。
**卡住时**：先来这里查"这类事以前踩过什么"，再去出处看详情。

> 若某条让你省下了往返，**回来给它加出处链接或补一条**——
> 这份表的价值就是"下一个撞同一堵墙的人能查到"。
