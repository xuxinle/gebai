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

## 索引 A：「绿而无效」的五种形态（查之前先分清你在查什么）

> 下面五种**长相完全一样**（检查全绿），却各自坏在不同环节——按「该往哪儿修」分裂成五类。
> 本仓实测：五种散在五处，撞上时只会想起其中一种（通常是最不相关的那种）。**先认形态，再修。**

| # | 一句话 | 根因 | 处置 |
|---|---|---|---|
| ① | **零红没报分辨力**：15 轮/形态都 15/15 绿 ⇒ 判「`exec` 非必要」；加压 24 路后少 `exec` 红 **4/30**、带 `exec` 红 0/30，结论被推翻 | 分辨力不足（窗口太小，不是结论错） | 加压或结构性放大窗口；报**轮数 + 对照条件 + 检出下限**（N 轮全绿只说明 p ≲ 3/N） |
| ② | **回退是空操作**：`if (pattern.length < 0) return false` ——形参长度永不 `<0`，行为一点没变而全套仍绿 | 回退没改到东西（不是测试不行） | 改回退写法让它**真的改变行为**（`pattern.length >= 0` 恒真 ⇒ 永远走 `return false`） |
| ③ | **回退版编译不过**：字面 `false`/`const` 收窄触发 unreachable-code ⇒ 编译失败，跑的是**上一份二进制** | 根本没跑到新代码 | 回退文本必须能编译过（保留 `(void)var;`）+ 防 `copy2` 把旧 mtime 搬回来 |
| ④ | **锚点会过期**：旧锚点的特征串**正是**新修复要消灭的形状 ⇒ 留着它等于逼后来人把 bug 写回去，且每息误报 | 锚点描述的是**已死的缺陷** | 消灭缺陷时**同步改锚点**；收单前跑 `--anchors` |
| ⑤ | **锚点只在注释里**：特征串仍在源码 ⇒ `present: true` 全绿，但它只落在注释行，实现早被拿掉/已改写 | 锚点只证明「字符串还在」，不证明「那行代码还在」 | 分行统计（代码行 vs 注释行），代码行为 0 而总命中 >0 ⇒ **提示**（不是判退化） |

**五种的详述与实测数据**：① → 场景 2「没报分辨力的『零红』不是判据」；②③ → 场景 15「『回退后 0 条红』有三种成因」+ `reverse_verify.py` 头部；④ → 场景 15「锚点会过期」；⑤ → 场景 15「锚点只在注释里」。**这里只留一句话 + 处置，根因与数据以彼处为准**（不在两处各存一份真相）。

> **总判据：「绿」只证明「我查的那个东西当时在」，不证明「它还在起作用」。**
> 查之前先问：**我查的是行为、是代码、还是只是一个字符串？**

## 场景 1：刚改完代码，要跑测试/验证结论

| 坑 | 一句话 | 出处 |
|---|---|---|
| **`--force` 才重编** | `st build` 增量按 **mtime** 判定；改了源码但结论没变时**先怀疑二进制是旧的**。强制重编用 `--force`，或在改动**新增文件**后删 `st_tests` 产物 | `CONVENTIONS.md` §7.4 |
| **`st test <filter>` 不重编** | 带过滤器的 `test` 会重跑但**不重编**——"单跑绿、全量红"的经典成因。**已核实并修正（2026-10-13）：现行 `run_tests` 无条件 `compile_units`+`link`（mtime 增量），实测改源后 `st test <filter>` 正确重编重链（exe mtime > src mtime）。本条保留作为历史记录与复核方法：结论与源码矛盾时先比对 exe/src mtime** | `CONVENTIONS.md` §7.4；实测 2026-10-13（build.cpp 的 `run_tests`） |
| **产物路径有两套** | `build/bin/st`（自举产物）与 `build/dev/bin/st`（真产物）**不是同一个文件**；改了 linter 却看到旧行为时先确认跑的是哪个 | 本文件 · 实测 2026-10-09 |
| **字符串搜到 ≠ 新二进制** | 验证工具链是否含新逻辑，别在 exe 里搜字符串（新加的规则描述文本可能来自其它段/旧对象）；**决定性判据是行为**——如 lint 用 `--rules` 列表能不能列出新增规则（实测 2026-10-15：`Select-String st.exe "L16"` 命中但 `--rules` 没有——跑的是旧二进制） | `tools/stpm/main.cpp` 的 `--rules` 分支 |
| **`st build st` 不能自举** | 用 `st.exe` 构建它自己时，链接器要覆写正在运行的 exe → `permission denied`。**用另一份构建器**（如 `build/bin/st.exe` 构建 dev 档，或反之）——两套产物路径在这里恰好成为优势 | `st.pkg` targets.st；实测 2026-10-15 |
| **逆向验证用 `copy2` 恢复 = 假绿** | `copy2` 把**旧 mtime** 一起搬回来（比回退版编出的 `.o` 还旧）→ 跑的是上一份二进制。恢复要**删旧文件再写** | `CONVENTIONS.md` §7.3 表格 |
| **测试结论与代码矛盾时** | 先查二进制是不是新的，**不要**先怀疑代码 | `CONVENTIONS.md` §7.4 |
| **`st build <target>` 不链测试二进制** | 只建指定目标（示例/库），**不重链 `st_tests.exe`**——改共享头（尤其 `inline constexpr` 变量改尺寸）后只跑 `st build xxx`，旧测试二进制里按旧尺寸编译的符号照常链接，**运行期 bad_alloc/0xC0000005、编译期零告警**（实测六跳：disjoint 稳定崩+片 9 收尾崩，`st test` 重链后同时自愈）。改共享头后用 `st test`（无条件重链）验证 | 实测 2026-10-16；`.evolve/troubleshooting.md` |

## 场景 2：写回归测试 / 验证它真能抓住缺陷

| 坑 | 一句话 | 出处 |
|---|---|---|
| **没报分辨力的“零红”不是判据** | 「N 轮全绿 ⇒ X 不必要 / 没问题」是**分辨力的误读**（复现不出 ≠ 不存在）。实测同一功能、同一台机：**15 轮/形态、load 3.8 → 少 `exec` 的写法 15/15 绿、带 `exec` 也 15/15 绿**，据此得出「`exec` 非必要」；**加压 24 路、load 9.92、30 轮后 → 少 `exec` 红 4/30、带 `exec` 红 0/30**，前一结论被推翻。N 轮全绿只说明失败率 p ≲ 3/N，所以报告零红**必须同时给轮数 + 对照条件（负载/并发/形态）+ 该条件下的检出下限**；三样不全的“我测了 N 轮都绿”不得写进代码注释或文档（可作观察记录）。另一个形态：裸旧写法在低负载下 20 轮零红，拿它当逆向验证的判据等于没有判别力——要么加压，要么把窗口**结构性放大**后再回退 | `CONVENTIONS.md` §7.1；`tools/pty_form_reverse_verify.py` 的模块 docstring；实测 2026-10-17 |
| **假绿：桩把差异抹平** | 被测的是"谁把值传给谁"，而桩直接回了个恒等值 ⇒ 错误实现也判绿。**桩必须复现被测的那一段真实行为** | `CONVENTIONS.md` §7.2 |
| **假 ctx 的字段名与真 ctx 不同** | 测试夹具自己编了 `{name}`，而运行时 `listDir` 回的是 `{path}` ⇒ 读 `name` 恒为空却「测试全绿」，缺陷（`apps` 永远报“没有运行中的应用”）直到真实会话才暴露。夹具要用**真实现返回的形状**，不是看上去合理的形状 | `packages/agents/src/agents/shuangtian/shuangtian_tools.ts` 的 `apps` 注释 |
| **假绿：回退文本编译不过** | `-Werror` 打断编译 ⇒ 测试跑的是上一份二进制 ⇒ 假绿。回退写法要能编译过（保留 `(void)var;`） | `CONVENTIONS.md` §7.3 |
| **判据自己也会错** | 先把"什么算合格"在**一个已知样本**上验一遍，再拿去判被测对象 | `CONVENTIONS.md` §7.8 |
| **像素断言的三个坑** | 聚簇 / 步长 / 判据——不做逆向验证很容易交付"恒绿的护栏" | `CONVENTIONS.md` §7.1 |
| **像素采样点踩在文字笔画上 = 量的是字宽不是几何** | 在“某块区域应是某个底色”的位置取一个像素时，那里往往同时画着文字（轨道名在块左内 8px、片段标签在块左内 4px）。两边都取到的是**字形抗锯齿混色**，与两态底色都对不上——断言恒红，改采样 y 或调色值都无效。判据：同一像素换掉内容（清空 label）后颜色变不变；会变就说明它量的是文字。修法是让采样区**只由被测几何决定**（清空无关文字，或移到文字行下方） | `tests/ui_timeline_view_test.cpp` 的 `timeline_view_track_headers_toggle_collapses_prefix_column` 注释；实测 2026-10-17 |
| **在 `/tmp` 下的 worktree 跑套件 = 控制通道用例成片假红** | `default_capture_dirs()` 恒放行 `fs::temp_dir()`，于是“白名单外必须拒”的负向用例（`capture_bare_control_file_*`）在 `cwd` 落在 `/tmp` 时**永远拦不住**——看上去是被测代码坏了，实际是夹具位置把白名单扩大了。隔离 worktree 要建在 `/tmp` 之外 | 本文件 · 实测 2026-10-17 |
| **`ST_CHECK` 之后跟着索引 = 分片静默失联** | 前置条件没满足时，后面那句 `v[0]` 会让 `_GLIBCXX_ASSERTIONS` 直接 `abort()`，**整个分片连同其余上千条用例一起消失**，只报“分片未能启动”而**一条失败记录都没有**。前置条件用 `ST_REQUIRE`，诊断打印要在断言**之前** | `tests/lsp_client_test.cpp` 的 `fake_ready` 处注释 |
| **假 server 用 `sh` 脚本 = 只在 POSIX 成立** | Windows 的 `CreateProcessW` **不认 shebang**（报“%1 不是有效的 Win32 应用程序”），而用例里还跟着索引空 vector——两者叠加就是上一条。跑脚本的用例要么标 `ST_TEST_SLOW` + 平台跳过，要么换成真程序 | `tests/lsp_client_test.cpp` 的 `FakeServer` 注释 |
| **“0 命中”与“读不到”必须是两个数** | “替换了 0 处”至少三种成因：目标不是磁盘文件 / 文件读失败 / 真没命中。只报一个总数时，排障要重跑一遍搜索才能区分。把**次因**（目标数/读失败数/无命中数）一并报出 | `examples/gbcode/main.cpp` 的 `replace_in_workspace` 注释 |
| **回归测试要调生产函数** | 测试里“自己做一遍正确逻辑再断言”= 测的是测试自己；回退生产实现后依然全绿（实测：崩溃报告文件名那条，回退后仍 PASS；改成调 `crash_report_file_name` 后回退立刻变红） | `tests/core_log_file_test.cpp` 的 `log_crash_report_filename_has_no_colon` 注释 |
| **按注册序取模的测试期望必须观察推导** | 分片（`--shard`）按**全局注册序**取模，而全局序由其它 TU 的用例数决定——早链接 TU 正常新增一例就把哨兵块整体平移，三处硬编码期望（块起点≡0 mod 4 / 落片号 / 空片选片）**全部误击**，现象像 flaky（同二进制忽绿忽红——其实是中途重建）。期望值只能**运行时观察**（无过滤 `--list` 行号即全局注册序），不能算术硬编码 | `tests/shard_parity_test.cpp` 的 `full_listing_lines` 注释；实测 2026-10-15 |
| **多维下标放进断言宏 = 逗号被当成参数分隔符** | `ST_CHECK(view[y, x] == v)` 报的是 `macro 'ST_CHECK' passed 2 arguments, but takes just 1`（`ST_CHECK_EQ(view[y, x], v)` 则是 3 个）。**整体加一层括号**即可：`ST_CHECK((view[y, x] == v))` / `ST_CHECK_EQ((view[y, x]), v)`。同样适用于 `arr[i, j]` 这类自定义下标；一维下标不受影响，所以此坑只在第一次用 `mdspan` 时现形 | `tests/raster_pixel_view_test.cpp` 头部注释；实测 2026-10-17 |
| **像素比对时“预乘打包值”不是颜色通道** | `pixels()` / `pixel_view()` 存的是 `math::premultiply` 的产物（预乘 + 打包，**alpha 在最低字节**），而 `pixel_at()` 返回非预乘色。拿 `pixels()[i]` 当红色通道去比，得到的是 570425599 这种数字，看上去像“实现错了”（实测先错了一轮）。要比就经 `unpremultiply` 走一圈，或直接与 `premultiply(...)` 的期望值比 | `tests/raster_pixel_view_test.cpp`；`include/st/math/color.hpp` 的 `premultiply` / `unpremultiply` |

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
| **含点 id 直接写 `#id` 会被 CSS 拆分** | 选择器把 `.` 当类型分隔符、`:` 当状态分隔符，而业务 id 常含这些字符（`git-file-<path>` 等）；`find` 对 `#xxx` 形态有整串优先容错，但脚本 `ui.query`/`$()` 没有——含特殊字符的 id 用属性写法 `[id=...]` 最稳（实测 2026-10-15：`#git-file-alpha.txt` 拆成 `Id: git-file-alpha + Type: txt`，count 恒 0） | `DESIGN.md` §6.3 含点 id 警示段 |
| **“报成功”不等于文件在** | 自己写探针时，正则对中文尾注（如 `（区域 {…}`）会贪婪地吞进路径，于是“文件不存在”是探针的错、不是工具的错（已实测踩到）。判落盘用 `ls`/`find` 复核 | 实测 2026-10-13 |
| **验证 UI 读组件自报的值** | 不要信旁路探针（截图/像素统计）——两者口径不同 | `CONVENTIONS.md` §7.5 |
| **合成输入不一定触发交互态** | 下结论前先确认**前提状态成立**（悬浮/按下/焦点） | `CONVENTIONS.md` §7.6 |
| **真窗口实测用 `tools/st_win_input.py`** | 别自己拼 PowerShell（三条注入通道各有适用范围） | `CONVENTIONS.md` §7.7.1 |
| **先清掉自己留下的前台程序** | 否则被控应用的前台态判断会被上一个实例污染 | `CONVENTIONS.md` §7.7.3 |
| **`pgrep -f <pattern>` 会匹配到发起它的那条 shell** | 等待循环里写 `pgrep -f "x.py"` 时，`sh -c '… pgrep -f "x.py" …'` 的**命令行自身就含着这串** ⇒ 每次命中自己 ⇒ 条件恒真 ⇒ 等一个早已结束的东西，**而且看起来完全像在守规矩**。本仓实测两次：worker `ctx-units-1` 因此空转 **11:34:51→11:46:19（12 分钟）/ 159 条消息 / 106K ctx / 零实现**（7 次等待全被判「还在跑」，而当时真实验证进程为 0）；生生自己的 `pgrep -c -f 'python3.*reverse_verify'` 同样自匹配，改成 `'python3.*reverse_verify[.]py'` 后正确返回 0。**破字串**能自救的直接原因：命令行上是字面 `[.]`，而正则要的是 `.`。三条正确写法：① 有 pid 就 `while kill -0 <pid> 2>/dev/null; do sleep 1; done`；② `pgrep -x <进程名>`（不扫命令行）；③ 仍要 `-f` 时 `pgrep -f 'x[.]py' \| grep -vx -e $$ -e $PPID`（`grep -v $$` 是子串匹配，会滤错且排不掉父 shell）。等待循环还必须有上限，否则判据错一次就是无限期空转 | `packages/agents/src/agents/shengsheng/pgrep-lock.ts` 头注释（含两组实测数据）；`packages/agents/scripts/wait_probe.sh`（六项对照验收脚本：正向 exit 0，`--naive` 把破字串改回原样串必报红）；`shengsheng.md` §六「等待纪律」 |

## 场景 6：改构建系统 / 依赖 / 缓存

| 坑 | 一句话 | 出处 |
|---|---|---|
| **构建系统的三个静默失效点** | 改头文件不重编 / 标志与缓存键不一致 / 平台漏开关——**什么也不报，只是不重编** | `CONVENTIONS.md` §10.3 |
| **清单里写 `-O` 等于没写（档位标志在后面）** | `build_language_flags` 的顺序是「清单 `flags` → 档位 → 工具链收敛」，而 GCC 取**最后**出现的 `-O`。往 `st.pkg` 的 `flags` 加 `-Os`，会被档位的 `-O2` 覆盖——实测产物体积**逐字节相同**（5,575,682 字节），且无任何提示。改档位只能改 `profile_flags()` | `src/pkg/build.cpp` 的 `build_language_flags`；实测 2026-10-17 |
| **`--extra-flags` 传了等于没传** | `BuildOptions::extra_flags` 存在且会进编译标志，但 `tools/stpm/main.cpp` **从未把命令行参数接到它上面**。危害不是“少个开关”，而是**实验档位时会把结论测反**：以为在测 `-Os`、实际在测 `-O2`，而两者体积差 2 MB。判据：`--verbose` 看编译命令行里的 `-O` 到底是哪一个 | `docs/BACKLOG.md` 的 P1 条；实测 2026-10-17 |
| **优化档不等于变小** | `-O2` 产物体积**反而比 `-O1` 大 21%**（`.text` 从 3.3 → 4.3 MB，内联展开所致）；真要小用 `-Os`（`.text` → 2.28 MB，体积 −38%）。报体积时必同时给档位，否则“减小了”可能只是换了个优化档 | 实测 2026-10-17（`st.exe` 三档对照） |
| **C++23 设施的“能编 ≠ 能链接 ≠ 能工作”（实测，含一次错判）** | 四个层次判据各不同：① **宏在**（`__cpp_lib_*`）；② **头存在**（`__has_include`）；③ **能链接**——注意还要分**“不报错”与“默认就链上”**；④ **能工作**。逐设施实测：<br>• **`std::print`**：编译过、默认链接**挂**（`undefined reference to std::__open_terminal/_write_to_terminal`），但**加 `-lstdc++exp` 完全可用**（中文/emoji/管道全对）。**我当时把“符号在另一个库里”错判成了“平台没实现”——一条 `nm -A libstdc++exp.a \| grep __open_terminal` 就能避免**；<br>• **`std::stacktrace`**：同样在 `libstdc++exp` 里，链得上、`current()` 也回帧，但 **5 帧全同名**（`register_frame_ctor`）——符号化没工作。**同一个库里一个能用一个不能用，不能整库推广“加了就行”**；<br>• **`<move_only_function>`**：宏在但**没有独立头**（在 `<functional>` 里）⇒ `fatal error: No such file`；<br>• **`<generator>`**：GCC 有、clang 目标下宏不存在。<br>**遇 `undefined reference` 的纪律**：先 `nm -A <候选库>.a \| grep <符号>` 确认“是没实现还是没链接”，**再**下结论 | `include/st/core/cxx.hpp` 的探针函数；`tools/stacktrace_probe.cpp`；`docs/BACKLOG.md` 待做段的 P1/P2 |
| **`libstdc++exp` 不是“废弃/玩具”库** | 它是 GCC 给“**尚未进入稳定 ABI 承诺**的设施”单开的库：23 个目标文件（`print.o`、`stacktrace.o` + 15 个 `std_stacktrace-*.o`、`text_encoding.o`、`contract26.o` 与 `<filesystem>` 旧残留），991 KB。**这些设施的实现只在它里面，主库里没有副本**——所以面对 `std::print`/`<stacktrace>` 的未定义符号时，先想到它，而不是“平台不支持” | `ar t libstdc++exp.a`；`docs/BACKLOG.md` 的 P2 条 |
| **两个构建不能并行** | 后者会以 `io: rename ... Resource device`（Linux 上更常见 `No such file or directory`）失败——`.o.tmp` 是**固定名不含 PID**，两进程抢同一个 | `CONVENTIONS.md` §9.0③ |
| **并行构建的读数污染往往是静默的（不报错）** | 只记“rename 会失败”不够：四层里只有 ① 会报错，**其余三层一声不响**——② 同一 `build/<档>/obj` 里两个进程各写各的 `.o`，最终目录**混代**（实测 6 个同名单元归属 1/5 分裂，链接照常成功）；③ PCH/链接产物直接 `-o` 覆写不过 `.tmp`；④ 共享缓存 `unit.o` 是 `copy_file` 覆写而非原子 rename，读侧能拿到**半截对象**（实测 30% 读截断）且**跨构建存活**⇒“改回去了仍然红”。判据：`objdump -h ~/.shuangtian/cache/objects/<key>/unit.o` 读不出就是坏条目；**清 `build/` 修不好它**（不同根） | `CONVENTIONS.md` §9.0④；`docs/BUILD_ISOLATION_PLAN.md` §0 |
| **判据落在产物存在性上** | 不要用退出码/日志当代理信号（实测：PCH 创建"报成功"其实静默空转） | `DESIGN.md` §8.2 第 49 条 |
| **造高负载不能用真并发 `st build`** | 它是链路 ④（写坏**全机共享**对象缓存）最容易的触发方式：实测在主树启 10 个并发 `st build --force`（load 峰 55）→ 缓存里 **13 个 `unit.o` 变 0 字节**、主树 47 个 `.tmp` 碎屑，随后**别人**的构建报 `undefined reference`（从缓存“恢复”出空对象）。受害者不是造负载的人，而是之后**每一个** worker。要负载用纯 CPU 空转（`yes > /dev/null` × N），要编译压力就开在工作树里 | `CONVENTIONS.md` §9.0④ 判据 5；`docs/BUILD_ISOLATION_PLAN.md` |
| **自举单元集必须从清单推导，不能按目录写死** | bootstrap 写死了 `src/core src/ext src/pkg`，与 `st.pkg` 脱节——新增 `src/ext/video.cpp` 引 `codec/raster` 后，报的是链接期一串 `undefined reference to st::...`（**报错指向源码，真因是"该编的单元没编"**）。现在按清单推：`sources − targets.st.exclude_sources + targets.st.sources` | `bootstrap.sh` / `bootstrap.ps1` 的相邻注释 |
| **语言标准也必须从清单派生（三处硬编码）** | 升 C++23 时发现：`st.pkg` 与 `DESIGN.md` 早写着 `cxx_standard`，**而 `src/pkg/` 里没有一行解析它**——真正生效的是 `build.cpp` 里三处硬编码 `-std=c++20`（框架标志、工程标志）加两个自举脚本。表现：改清单无任何效果，且“st build 用新标准、自举出的 st 用旧标准”两边都能编过。现在清单是唯一真源，另加了 `st/core/cxx.hpp` 的 `#error` 守卫（探针/IDE/手编绕过构建驱动时能立刻停住，而不是以“标准库内部错”的形式报错） | `src/pkg/build.cpp` 的 `make_framework_flags` / `build_language_flags`；`include/st/core/cxx.hpp` |
| **排除模式 `src/ui/*` 不跨子目录** | `*` 不跨 `/`（与 `fs::match_glob` 同口径），要跨得写 `src/ui/**`。写错的症状是**排除静默不生效**（只警告"未命中"），于是 `EXCLUDE` 形同虚设 | `src/core/fs.cpp` `match_segments` |
| **工具链只该编它真需要的单元** | `st` 一行渲染都不跑，却被 `core/platform_crash.cpp → ui/state_trace.hpp`（死 include）与 `shell → ui/window_control.hpp` 两条**头文件级**反向依赖拖进整条渲染链。前者已删 + lint **L16** 机械拦截（core 文件 include `st/ui/` 头即违规）；后者见 `BACKLOG.md` P1 | `include/st/ui/window_control.hpp` 注释 |
| **幂等判据用 mtime 或「产物存在即跳过」都是错判据** | 实测两处：① mtime 比——`--strip-debug` 会刷新产物 mtime ⇒ 幂等判据恒假、每次白跑；② `.debug` 存在即跳过——重链后**新产物的调试信息永久丢失**。正解：探真实内容（`objdump -h` 查 `.debug_info` 是否还在），探不到时按需要重跑处理（安全方向） | `BACKLOG.md` 整改轮（2026-10-14）「本轮由自己的用例抓出的两处判据错」 |
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
| **行/列容器的 `max_width` 是 `kUnbounded`，真可用宽在 `available_width`** | “铺满父级”的容器/内容型组件直接取 `max_width` 当尺寸，就会在 Row 里拿到 1e9→落回兑底常量（或按偏大的值分栏），而 `arrange` 给的是真实 rect——**排版宽 ≠ 控件宽**。用 `fill_width/fill_height`；内容型组件另在 `arrange` 里用真实矩形重排一次（`ScrollView` 内已内化此惯例） | `include/st/ui/element.hpp` 的 `fill_width`；`ScrollView::arrange` / `MarkdownView::arrange` |
| **这类重排不能加“几何变了才做”的早退** | measure 阶段的粗口径宽**每帧**都会把子组件重排一次；只在 rect 变化时补救⇒第二帧起不再修正，最终停在错误宽度（实测：加了早退后 `layout_width` 仍为按 1280 算的 985.6，而控件宽是 982.4） | `src/ui/components/split_view.cpp` 的 `arrange` 注释 |
| **逆向验证时“回退半边修复”＝假绿** | 修复分在两层（measure 取宽 + arrange 重排）时，只回退一层，另一层照样把测试养绿。回退要**完整回到缺陷状态**，且回退文本要能编译过（保留 `(void)var;`） | `CONVENTIONS.md` §7.3；本行实测（`SplitView` 那条测试回退一层仍绿） |
| **做文本选择：先扁平化，且命中/绘制必须同一基准** | ① 选择状态用 `{行, 字节偏移}`，行内备一份**扁平文本 + 每码点前缀宽**——在分片（Span/单元格）上各自算偏移会静默错位；② 命中侧鼠标 x 相对**控件左缘**，而行内 `text_x` 是**内容坐标**（还要加 `padding.left`）——两边不一致会让高亮多/少一个 `padding.left`（表现为行首多一段空白） | `DESIGN.md` §4.2.17g；`markdown_view.cpp` 的 `caret_at` / `paint_selection` |
| **选区高亮的垂直范围取字形墨迹，不取行高** | 行高含 `line_gap` 与字体留白，拿它当高亮高度会让高亮比文字高出一截。**正解已收敛到框架原语**：`st::ui::layout_line(...)` 的 `glyph_top`/`glyph_height`（或 `TextSelectionModel::bands()`，它内部走同一个量）。别再用 `ascent..descent` 手算——见下一行的实代价 | `include/st/ui/line_layout.hpp`；`include/st/ui/text_selection.hpp` |
| **`ink_top/ink_height` 是“行带”，不是“墨迹带”** | 这两个名字极易误用：它们是**整个行盒**（供行背景/缩进参考线，故意撑满以免行间留缝），行距 1.6 倍时比字形高出近一整行。拿它画**选中高亮/光标**就会“高亮跑到字上方”。选区/光标一律用 `glyph_top`/`glyph_height` | `line_layout.hpp` 的 `LineGeometry` 字段注释（含两报的实测数据） |
| **同一件事存在多条实现 ⇒ 必然漂移（文本选区的实代价）** | `CodeEditor`/`MarkdownView`/`Terminal`/`Input`/`Text` 各写一套拖选+双击选词+`Ctrl+A`+高亮几何，而框架里渲染文字的组件有二十多个。`MarkdownView` 那套**连错两轮**（先用行高、后差一个 `padding.top`），而正确几何在 `line_layout.hpp` 里早有。**新加文本组件一律接 `TextSelectionModel`，不要另写一套** | `include/st/ui/text_selection.hpp`（行为层）；`DESIGN.md` §4.2.17i |
| **要复用的纯函数别放在 `src/**/*_internal.hpp`** | 那类头只在 `src` 内部可见，应用/子 Agent 拿不到，只能**各抄一份**（“同一件事多条实现”的入口）。判据：**会被两个以上模块用到**的功能就该进 `include/st/` | 实测：选区纯函数原在 `components_internal.hpp`，已提升到 `include/st/ui/text_selection.hpp` |*——像素量化：文字 y≈114~133、高亮 y≈136~144） | `DESIGN.md` §4.2.17g；`terminal.cpp` / `basic.cpp` 的选区绘制 |
| **`property_names()` 是“可读写”，只读属性要另开一份表** | `selection` 这类天然只读：混进 `property_names()` 会让「声明的名字必须可写」这条通用护欄必然失败（实测：`ui_actions_set_changes_all_declared_string_properties` 变红）；不入表又会让 `get` 读不到（快照只遍历 `property_names()`）。用 `Element::readonly_property_names()`（默认空）：快照遍历两张，`set` 只认可写表 | `include/st/ui/element.hpp` 的 `readonly_property_names`；`src/ui/actions.cpp` 的 `element_snapshot` |
| **测试桩必须复现被测段的真实行为** | 桩 `FixedAdvanceTextPort::wrap` 只按宽度硬切、**不识别 `\n`**，而真实 `TextRenderer::wrap` 把 `\n` 当硬换行——于是 `Text::set_multiline` 的多行用例里“显示行”只有 1 行，**跟行选择永远为空也判不出错**（实测踩到，白查了一轮） | `CONVENTIONS.md` §7.2；`tests/support/text_port_fixtures.hpp` |
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
| **“渐变慢”只对径向 / 斜向成立** | 1080p 整幅：纯色 0.42ms、竖向线性 0.60ms、径向 98ms、带 alpha 的斜向线性 66ms（竖向有逐行 SIMD 快路径）。所以“用渐变会变慢”这个直觉只在非竖向时成立——大面积铺光优先竖向 | `tools/gradient_fill_probe.cpp`；`tests` 无（探针自带判据） |
| **用格子/条带去近似光晕会留下可见接缝** | 实测：20×20 格子铺光是一层棋盘格、纯横条带是一条横带。低频内容的**高频近似**看不见这件事靠不住 | `examples/caiyun/motion.cpp` 的 `glow` 注释 |
| **同一件事写成两条路径，其中一条漏了一步** | 脚本装载有两条入口（启动构造 / UI 按钮）：只有后者做了相对路径重基，于是命令行出片“0/4 轨配音”、UI 出片正常。**相同输入两条路径两种行为**的经典形态 | `README.md` §caiyun；子Agent 提示词「同一件事只留一个入口」 |
| **“锁住编辑”不能在 UI 层逐个禁用** | 编辑入口有六七处，逐个置灰必漏一处；而 `控制通道 invoke` 本身就绕过 `enabled`。门禁必须长在数据层（`Project::can_edit()`），UI 只负责让人看见原因 | `README.md` §“AI 控制态”；`tools/caiyun_director_e2e.py` 第 6 步 |
| **验证“控件点不动”必须用真鼠标** | `invoke` 走的是“直接触发”通道（绕 enabled），拿它验 UI 层会假绿。真窗口 + `mouse_event` 才能验到 `set_enabled(false)`；无头模式下没有系统鼠标事件 | `tools/caiyun_lock_realwindow.py` |
| **`set` 属性不会触发 `on_change`** | `Switch::set_property("checked", …)` 只改 `checked_`，不调 `on_change`（`activate()` 才调）——拿它验「人拨开关」会得到恒绿的假测试。验“动作”一律走 `invoke activate`；也说明开关类控件必须用 `activate` 路径才等价于点击 | `tools/caiyun_director_e2e.py` 第 6c 步 |
| **算点击坐标不能用窗口矩形** | `GetWindowRect` 含标题栏/边框，而控制通道的逻辑坐标从**客户区**左上算。用窗口矩形会让点击整体下移一个标题栏高——实测开关在逻辑 y=402，却点到了 624，然后被误读成“框架的 on_change 没接上”（在框架里多查了一轮）。用 `ClientToScreen(0,0)` 拿客户区原点；DPI 倍率取 `metrics.device_scale`，**不要从窗口像素反推**（窗口逻辑宽随布局变） | `tools/control_switch_realwindow.py` |
| **模型画文字 = 废图** | 让 sd-turbo 出「带标题的背景图」，得到的是扭曲字母（它没有排版能力）。素材只出背景/纹理，文字一律由渲染器画；提示词必须带 `no text` | `tools/imagegen.py`；README §imagegen |
| **Windows PATH 的 python 是商店 stub** | 直接 `spawn("python")` 会打开微软商店、不跑脚本也不报错——于是工具"检查环境"得到空输出、误报不可用（比不报更费时间）。必须按已知安装位置显式解析真实解释器 | `packages/agents/src/agents/imagegen/imagegen.ts` |
| **PyTorch 官方源单连接 0.2MB/s，国内镜像无 CUDA wheel** | 2.4GB 要 3.4 小时；清华/阿里/北外/中科大的 pytorch-wheels 实测全 404。唯一出路是**并行分段下载**（自带 `tools/pfetch.py`，12 路实测 17MB/s，快 85 倍），且必须带断点续传（后台命令会被生命周期超时杀掉） | `tools/pfetch.py`、`tools/fetch_sd_turbo.py` |
| **反向验证要跑"回退后构建的那个档"** | 回退后构建 debug、却让 e2e 用它默认的 dev 二进制 → 测试仍在跑旧的 → **回退被测不出、得到假绿**（实测差点把有效断言当成无效）。反向验证必须把 binary 显式传给 e2e | `tools/caiyun_director_e2e.py` 头部说明 |
| **素材块插到文字块之后会盖住标题** | 块数组自底向上（后画的在上），`push_back` 一张图就把标题盖没了——看着像"渲染坏了"，实际是叠放次序 | `examples/louyue/main.cpp` 素材插入处 |
| **cover 算了裁切区却没用 = 拉伸变形** | `blit_cover` 算出了与目标同比例的源区域，却直接整幅 `draw_canvas` → 长宽比不一致时画面被拉变形（人脸/产品一眼假）。必须真的按 src 区域贴 | `examples/caiyun/assets_lib.cpp` |
| **`includes("/")` 判绝对路径在 Windows 上失效** | `C:\Users\x.png` 不含正斜杠 → 被当成相对路径原样返回，白名单重写整段被跳过。只在 Windows 红、CI 跑 Linux 时看不见。跨平台口径用 `isAbsolute()` | `packages/agents/src/agents/shuangtian/shuangtian_tools.ts` resolveShotPath |
| **SDXL 尺寸必须是 64 的倍数且短边 ≥ 512** | 给 500×500 会抛错或出扭曲构图（训练分辨率 1024²）。提示词救不了，是硬约束 | `tools/imagegen.py` generate() |
| **高频题材（星云/电路）不能做底板** | 细节密到压暗后叠字糊成一片；好看 ≠ 可用。底板只用渐变/雾光/单色纹理，并在提示词追加「低对比/大留白/柔和过渡」 | `tools/gen_gebai_assets.py` |
| **卡片标题固定字号会溢出** | 卡片宽固定而文案是外部输入（AI 写的）：固定 50 号字遇长句直接压到卡外。必须先 `text_width` 量宽再按比例缩（下限 30 号） | `examples/caiyun/motion.cpp` paint_triptych |
| **fp16 变体下载后加载器找不到** | 只下 `*.fp16.safetensors` 时 diffusers 按标准名 `diffusion_pytorch_model.safetensors` 找 → 报 no file named。用**硬链接**补标准名（同卷不占额外空间；Windows 非管理员可用 New-Item -ItemType HardLink） | `tools/fetch_diffusers_model.py` |
| **多层同心圆叠不出径向渐变** | 每层圆的边缘都会留下可见的环，比不加光晕还难看（实测 14 层，画面像水波纹）。径向光晕只能逐像素算衰减，且限制在包围盒内（全幅逐像素 1080p 约 50ms/帧） | `examples/caiyun/fx.cpp` paint_glow |
| **运镜不能作用于包装层** | 整幅一起变换会让识标被放大、页脚被推出画面、拉远时四周露黑边。包装层（台标/页脚/进度/角标）必须固定——顺序是：内容 → 运镜 → 贴回 → 包装 | `examples/caiyun/motion.cpp` render_scene |
| **推镜会裁掉贴边内容** | 版式安全边距 5%（1080p=96px），推到 1.06 倍只剩 1%——实测数据页的 100/10 几乎顶到画面边，读起来像排版坏了。推镜不超过 1.08，且内容必须落在 `Grid::margin` 内 | `examples/caiyun/fx.hpp` CameraMove |
| **单查单调性抓不住径向渐变里的环** | 半径量化造成的阶梯本身是单调不增的——回退成量化版后单调性判据仍全绿（假绿）。要查二阶差分：台阶处会出现 10+ 的尖峰，真渐变只差几个灰阶 | `tests/caiyun_fx_test.cpp` |
| **examples 下的实现不能只 include 头** | 测试构建只编译 `tests/*_test.cpp` + 框架库源码，不链接 examples 的目标文件 → 直接 include 头会报 undefined symbol。要么 include 实现，要么把源加入测试目标 | `tests/caiyun_fx_test.cpp` |
| **e2e 默认二进制指向 `build/dev`，重建 `debug` 后测试仍跑旧产物** | `DEFAULT_BIN` 写死 `build/dev/bin/*.exe`，而回归规范档是 `debug`——于是"改代码 → 建 debug → 跑 e2e"全程测的是上一份 dev 二进制，**断言恒绿**。实测代价：删掉 `points` 序列化后 e2e 仍通过，差点把假绿当反向验证成功。判据：反向验证没红就先怀疑二进制，`ST_PROFILE=debug` 或显式传 `build/debug/bin/x.exe` | `tools/*_e2e.py` DEFAULT_BIN |
| **`Viewport::fit_to_view` 换文档后无效** | 它读子元素的 `content_size()`，而换了文档后子元素还没重新 `measure`（即使先 `mark_layout_dirty()`）——实测 view=876x662 / doc=1920x1080，调用后 zoom 纹丝不动。要让大图装进小视口，**改渲染尺寸**而不是改缩放 | `examples/louyue/main.cpp`；README §×镂月海报面 |

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
| **「环境瞬态」标签会掩盖真 bug** | 100% 可复现的崩仍被当成环境瞬态——本案前期误判三次（代码 bisect/ASan/环境逐个排除，其实先查二进制新旧就能定位）。**复现率 100% 时先查二进制链路再怀疑环境**；陈旧链接+inline 变量尺寸错配的崩签名 = bad_alloc/0xC0000005 出现在小分配/收尾处 | 实测 2026-10-16；`.evolve/troubleshooting.md` |
| **`-Wshadow`/`-Wconversion` 是结构错误的探测器** | 手工搬移代码块时括号数错 ⇒ `paint_content` 的行循环被提前关闭 ⇒ 后面几十条 `-Wshadow` 报"变量遮蔽"。**编译器先于运行期发现了它** | `src/ui/components/code_editor.cpp` |

## 场景 14：改内存 / 产物大小（资源类指标）
| 坑 | 一句话 | 出处 |
|---|---|---|
| **`thread_local` 缓存的真实代价是内存** | 字体的 `load_face_cached` 原先是 `thread_local`：Tasks 池 8 线程里任何非首线程首次碰字体都会**重新整读全部字体文件**（最坏 8×86MB）。且计数器也是线程局部时，“别的线程又读了一遍”在数字上根本看不见 | `src/text/text.cpp` 的 `load_face_cached` 注释 |
| **“整读进 vector”与“内存映射”的差别在内存归属** | 不是快慢：`read_bytes` 是私有堆副本（常驻到 free），`mmap`/`CreateFileMapping` 是文件页的共享副本（压力下系统可回收）。字体链 15 个 face 的 ~86MB 常驻就是整读造成的 | `include/st/core/fs.hpp` 的 `MappedFile` 注释 |
| **Windows 映射必须带 `FILE_SHARE_DELETE`** | 不加它，映射存活期间字体文件**无法删除/改名**（用户重装字体报“文件被占用”）；加上它映射仍读到旧内容，但不锁路径 | `src/core/platform_fs_map.cpp` 的 `open_file_mapping` 注释 |
| **POSIX 上 `open` 目录会成功，`mmap` 才报 `ENODEV`** | 报错文本看着像硬件问题。显式挡目录，给可行动的理由 | 同上 |
| **`-g1` 对 C++ 模板实例仍然很大** | 设计预期“只行号表所以小”，实测 `st.exe` dev 档 76.96MB **其中 89% 是 `.debug_*`**（模板/内联函数照样生成 DIE 骨架）。链接后 `objcopy` 三步（`--only-keep-debug` / `--strip-debug` / `--add-gnu-debuglink`）→ 4.59MB，调试能力不丢 | `src/pkg/build.cpp` 的 `separate_debug_info` 注释 |
| **`objcopy --strip-debug` 会刷新产物 mtime** | 拿“`.debug` 比产物新”做幂等判据**恒假**（每次重链都白跑三遍）。正确判据是 `objdump -h` 看产物里还有没有 `.debug_info` | 同上（本轮被自己的用例抓住） |
| **“`.debug` 已存在”不是“已分离”** | 重链后 `.debug` 是**上一次的**符号，产物却是新的新的——此时跳过 = **新产物的调试信息永久丢失**。宁多跑一遍不丢符号 | 同上 |
| **系统头只能在 `platform_*`（包括这次）** | 第一版把 `mmap` 实现直接写进 `fs.cpp`，`st lint` 当场报两条 L14。规则的价值正在这里：把“顺手在通用层塞平台代码”变成必须跨过的一道门 | `include/st/core/fs_map.hpp` 的说明 |
| **无头路径不该探测 GPU** | `auto` 会真的建 D3D11 设备：+53MB / +55 线程 / +4.6GB 虚拟地址（驱动进程内常驻），全付在“任何像素产生之前”。无头缺省应为软件光栅器 | `src/shell/backend.cpp` 的 `HeadlessBackend` 注释 |
| **“先量内存再动手”而不是先改上限** | 缓存几十 MiB，而字体整读 86MB + 驱动常驻 53MB——不分解就去改缓存上限（小项）是拿大项换小项 | `DESIGN.md` §11.1 的内存去向表 |
| **优化前先量“这段到底占几成”** | 本仓是“先量再改”的纪律最好的例子：上屏（present）在 X11 1080p 占整帧 **27%**，而它与绘制同量级——不量就会去优化已经很快的绘制。且**介质与范围是两个独立病因**（共享内存 vs 只送损坏区），分别值得做 | `tools/x11_present_probe.cpp`；`DESIGN.md` §4.6.2 |
| **优化窗口后端的“上屏”必须先问清缓冲是单块还是多块** | X11 可以只送损坏区（每次都把同一块画布推给服务端），而 Wayland 是**双缓冲**（交替 attach，每块都必须是完整帧）——同样改法会让另一块停在旧内容（交替提交时闪烁）。**同一个 `set_present_region` 契约在两个后端的合法解释不同**，这不是接口设计失误而是事实差异 | `src/shell/platform_wayland.cpp` 的 `commit_frame` 注释 |
| **“存住上层资源”的判据要覆盖所有路径** | X11 上屏缓冲两条路径（共享段 / 私有 vector），而就绪判据只看了 `image_`（私有路径的句柄）⇒ SHM 路径**每帧重走一遍 shmget+shmat+attach+XSync+拆段**（比上屏本身还贵：送显 6.2 vs 2.0 ms）。共用检查必须写成 `(A 非空 || B 非空) && 尺寸相符` | `src/shell/platform_x11.cpp` 的 `prepare_backbuffer` 注释；实测 2026-10-16 |
| **“存住上层资源”这类缺陷的症状是“慢而不错”** | 它不会崩、不会报错、截图也对——只表现为一个恒定的时间开销（每帧建段）。**逆向验证也拦不住它**（断言全绿）——只能靠日志刷屏（本次每帧一条 “上屏走 MIT-SHM”）与数字对不上（实测值远低于探针预期）发现 | 同上 |

## 场景 15：写逆向验证脚本（回退→变红→恢复）

> 这类脚本的每一行都是为了避开一个**已实测过的假绿**。开工前先扫这一节。

| 坑 | 一句话 | 出处 |
|---|---|---|
| **「回退后变红」红在构建阶段 = 假红（与假绿正好相反）** | `and_then` 的回退实现（误用 `transform`）让返回类型从 `expected<T,E>` 变成双层 `expected<expected<T,E>,E>`；而 `ST_CHECK_EQ` 会用 `std::format` 拼失败信息，**libstdc++ 没有 `expected` 的 `formatter` 特化** ⇒ 整个翻译单元编不过。看上去“回退后变红了”，其实**一条断言都没跑到**（报错是 `static assertion failed: std::formatter must be specialized`）。三条处置：① 断言**不格式化嵌套 `expected`**（先转成 `bool`/标量）；② 主要判据用**类型检查**（`std::same_as<decltype(...), expected<int,E>>` 转 `bool`），因为“平链 vs 双层”才是 `and_then`/`transform` 的真区别；③ 回退后**必须看失败输出是哪一类**——`FAIL … 断言失败` 才算抓到，`编译失败/产物未产出` 一律当作没验证 | `tests/core_cxx23_contract_test.cpp` 的 `result_monadic_short_circuits_on_error` 注释；实测 2026-10-17 |
| **只观察副作用对某些语义差异没有分辨力** | 同一条用例：`transform` 与 `and_then` 在成功态**都**调回调、失败态**都**短路，于是“回调被调了/没被调”两条断言在回退后**全绿**（实测就是这样假绿的）。分辨率必须落在**两者真正不同的地方**（这里是返回类型），不能落在“看上去也合理的区别”上 | 同上；`CONVENTIONS.md` §7.1 |
| **「回退后 0 条红」有三种成因，根因与处置完全不同** | 逆向验证报「回退后 0 条红」时**先分清是哪一种**——它可能是**测试没判别力**（补测试/加压）、也可能是**回退本身是空操作**（改回退写法）、还可能是**回退版压根没编译过**（保证能编译 + 防旧 mtime）。三者混同会让你朝**错的方向**修（最常见的错：一律当成“测试不行”去加断言，而回退根本没改到东西）。① **零红没报分辨力**——现象：少 `exec` 与带 `exec` 两形态各 **15 轮、load 3.8 都 15/15 绿**，据此判「`exec` 非必要」；真相：加压 24 路、load 9.92、30 轮后**少 `exec` 红 4/30、带 `exec` 红 0/30**，前一结论被推翻。根因=**分辨力不足**（4/30≈13% 的失败率，15 轮的检出概率只有 ~12%）。处置=**加压或结构性放大窗口，并报出轮数 + 对照条件（负载/并发/形态）+ 检出下限**（N 轮全绿只说明 p ≲ 3/N）。② **回退是空操作**——现象：回退写成 `if (pattern.length < 0) return false`，形参长度永不 `<0` ⇒ 行为一点没变、全套仍绿，逆向验证当场报「[假绿] 回退后 0 条红」。根因=**回退没改到东西**（不是测试不行）。处置=**改回退写法让它真的改变行为**（`pattern.length >= 0` 恒真 ⇒ 永远走到 `return false`；实测 tsc rc=0、3 条变红且命中期望用例）。③ **回退版编译不过**——现象：字面 `false` 或被 `const` 收窄的条件触发 unreachable-code 诊断 ⇒ 编译失败，**跑的是上一份二进制/旧模块**，于是“全绿”。根因=**根本没跑到新代码**。处置=**回退文本必须能编译过**（保留 `(void)var;`、用恒假但可编译的条件），并防 `copy2` 把旧 mtime 搬回来让增量构建不重编 | `CONVENTIONS.md` §7.1（分辨力）/§7.3（编译过 + mtime）；① 见本文件场景 2 的「没报分辨力的『零红』不是判据」与 `tools/pty_form_reverse_verify.py` 的模块 docstring（15/15 vs 4/30，实测 2026-10-17）；② 见 `packages/agents/src/agents/shengsheng/reverse_verify.py` 里 `isSelfMatch` 的 `5b494802` 回退注释（`pattern.length`）；③ 见同文件头部「② 回退文本编译不过」与本节「判『编译成功』不能用退出码」；本节「回退测例要真能区分修复」是 ① 的同族另一副面孔 |
| **判“编译成功”不能用退出码** | `st test` 有失败用例时也返回非 0——而那正是我们要的“变红”。用退出码判会把自己的“测试红了”误判成“编译失败”，于是**整条用例被当成无效**而跳过 | `tools/present_reverse_verify.py` 的 `build()` 注释 |
| **测试输出带 ANSI 颜色码** | `\x1b[32mPASS\x1b[0m …`——不剥掉的话 `startswith("PASS ")` **永远不成立**，于是每条都报“全绿”（恒假的假绿判据）。本轮四条全报 0 passed，而单跑二进制明明是红的 | 同上 `run()` 注释 |
| **档位必须编译与运行一致** | `st test` 默认 **debug**，而脚本读 `build/dev/bin/st_tests`——不显式 `--profile dev` 就是“编 debug × 跑 dev”，永远读旧二进制 | 同上 |
| **回退测例要真能区分修复** | “向外取整”那条最初写的用例（`{7,0,3,3}` ×1.5 → 10.0~15.0）下 ceil 与 floor **结果相同**，回退后测试仍是绿的——**逆向验证当场把它抓成“假绿”报了出来**。挑用例时要问：这个输入下，修复与缺陷的**行为真的不同吗** | 同上 `CASES` 的注释；`tests/shell_backend_test.cpp` 的 `★` 用例 | 修“圆角发虚”时在覆盖率混合里加回“整行逐像素累加”，它**遮蔽了 `simd::blend_row` 的 full-span 快路径**（而上游 `merge_runs` 早已合并了重叠段）——圆角填充 **1.32× → 26.7×（20 倍），而门禁一直在报，只是没人看**。修法：保留累加（正确性需要它）但按 alpha 字节分组走 SIMD | `src/raster/canvas.cpp` 的 `blend_coverage_runs` 注释 |
| **基准分母不能是“可向量化到不像话”的量** | 性能门禁的分母是“清屏”（纯 `memset`，SIMD 快路径），而被测是标量 AA 光栅化——两者对 `-O` 响应不同，于是**同一负载的比值在 release 2.03× / dev 3.23×**、文本甚至反向。用它做门禁只能锁定单一优化档 | `tests/raster_bench_test.cpp` 的 `calibration_profile()` 注释 |
| **“比值型”断言要认档（与 san 同一口径）** | 不把阈值按档放宽（那会真的把 release 门禁变松），而是非标定档**如实跳过并说明**；`__OPTIMIZE__` 在 `-O1` 就定义，区分不出档位——本仓用 `NDEBUG`（只有 release 带） | 同上 |
| **性能红要先分清“环境”与“真回归”** | 本次差点把它归为“容器环境慢”（因为其它三项都吻合），实际是**一个真回归**。分辨法：① 比值是相对量，机器慢会**同时**抬高分子分母；② 用**同几何**的对照（同尺寸非对齐直角）比绝对耗时——两者一致就说明是路径本身慢 | `tools/rounded_fill_probe.cpp` 与 `docs/BACKLOG.md` 的性能门禁条 |
| **“不可定向验证”的判据等于没判据** | 测试里写 `if (false)` 把被测代码禁用后**用例仍然 PASS** —— 因为它被同一函数的另一条分支满足了。修法：把整形/判定逻辑抽成**纯函数**再断言两条分支（`describe_script_failure`）；**新增判据一定要做一次禁用实验** | `src/ext/script.cpp` 与 `tests/ext_script_test.cpp` |
| **同一引擎的行为可能随构建档不同** | QuickJS 的 OOM 在 release 档产出真异常（英文 `out of memory`）、在 dev 档异常对象**不可用**（走空描述分支）——于是同一个缺陷一档红一档绿，只靠“真跑脚本”根本测不准 | 同上 |
| **优化要和画质一起量（两者不可兼职）** | 解析扫描线降了 4~5 倍耗时，却带 **0.56% 像素差**（板阶被抹平）。同一项改动要同时报“比值 **与** 像素差”两个数，差超过噪声就不算“等价优化” | `tools/raster_pixel_diff.py`、`docs/BACKLOG.md` 的圆角填充条 |
| **锚点会过期：旧修复的特征串可能正是新修复要消灭的缺陷形状** | 修复锚点是「某缺陷不会回来」的钉子，但缺陷被消灭时，描述它的特征串往往**正是**新修复要删掉的形状——留着旧锚点等于在源码里把缺陷钉住：要让它通过，就得把 bug 写回去。本仓真案例：锚点曾写 `("运行态 ctx 判据", "const ctxOver = … > WORKER_CTX_LIMIT")`，而「拿复用建议线当模型窗口上限」正是拆两量那一单消灭的缺陷——留着它 ① 锚点永久缺失 ⇒ 每息误报「修复被回退」；② 后来人为让锚点过而把 bug 写回去。**两条纪律**：消灭缺陷时**同步改掉描述它的锚点**（改成只有新修复存在才成立的特征串）；**收单前跑一次锚点检查**（`python3 tools/shengsheng_sentinel.py --anchors`，任一 `present: false` 即拒收，台账写清哪个锚点退化/对应哪个提交）。为何比测试多一层：测试可能覆盖不全，锚点检查是「我以为我写了，现在确认它还在文件里」。**同型陷阱**：锚点串若只落在**注释行**，它守的是文档不是行为（实现删了注释还在 ⇒ 恒绿；详见本节「锚点只在注释里」） | `tools/shengsheng_sentinel.py` 的 `ANCHORS` + `RETIRED_SHAPES`/`stale_anchor_hints`（真案例 `1939b64f`→`e4181ef1`）；`packages/agents/src/agents/shengsheng/shengsheng.md` §六「收单纪律」；`tools/sentinel_anchor_stale_drill.py`（退化→必红的逆向验证） |
| **锚点只在注释里 ⇒ 守的是文档不是行为** | **现象**：锚点检查报 `present: true`、全套全绿，但那个特征串只落在**注释行**——实现早被拿掉或已改写，注释还留着（**锚点恒绿而修复已经没了**）。**根因**：锚点只证明「某个字符串还在」，不证明「那行代码还在」——它与「行为被守住了」不是一回事。**处置**：**分行统计**而不是全文命中——只认**行首**的注释前缀（`//`、`#`、`*`、`--`、`<!--`），分开报「总命中」与「代码行命中」；**代码行为 0 而总命中 >0** 就报出来。**但必须是提示，不是判退化**：两个判据都可能误伤（锚点也可能故意钉一句注释里的常量说明），判错的代价是有人去「修」一个没坏的锚点、或为了让告警消失而改动好代码——**代价不对称**，故只报 `comment_only` 提示，由收单的人做一次判断 | `tools/shengsheng_sentinel.py` 的 `_anchor_hit_lines` / `stale_anchor_hints`（提交 `7050b7e1`）；本节上一条「锚点会过期」末尾的同型陷阱 |

## 场景 16：提交与交付（你要把这一单的改动落进仓）

> 同一棵工作树可能同时跑着别人的活。开工前先扫这一节——**这一节的三条都是用别人的劳动换的**。

| 坑 | 一句话 | 出处 |
|---|---|---|
| **宽范围暂存会带走别的 worker 的未跟踪文件** | `git add -A` / `.` / `-u` / `commit -a` 把工作树里**别人的成果**一起提交。**根因是守卫的缺口，不是粗心**：`edit`/`write` 的「防陈旧覆盖守卫」只管**已跟踪文件**的修改，管不了未跟踪的新文件（`??`）——**谁 add 谁带走**，而新增文件恰是 worker 最常产出的东西（新模块、新用例）。实测（2026-10-17，本仓）：worker `shared-lease` 的 `856ba9bb` 带走了另一个**仍在跑**的 worker（`retire-tree`，当时 384 消息）两份未完成交付物——`proc-tree.ts`（**+596**）与 `shengsheng-retire-tree.test.ts`（**+573**），而提交说明里只字未提。内容没丢，但：另一个 worker 按「我还没提交」继续干 → 重复劳动/旧基线；台账归属记错；有缺陷时责任人定错。**判据**：`git add -- <逐个路径>`；提交前 `git diff --cached --name-status` 逐行核对，出现不在工单清单里的文件就**停下**；工作树有 `??` 时先问「这是谁的新文件」 | `packages/agents/src/agents/shengsheng/commit-scope.ts` 头部；`shengsheng.md` 第六节；实测 2026-10-17 |
| **「核过没问题」与「读不到」必须分开报** | 提交范围核对（`collect action=review`）在读不到 git（不在仓内/没装 git/沙箱）时若只说「无异常」，等于**在最需要警报的环境里安静**。三态必须分开：读不到 ⇒ 明说「本项未核」；核过无异常 ⇒ 明说「已核 N/N 均在工单内」；有疑 ⇒ 逐行列出。与「0 命中与读不到必须是两个数」同型，只是换到 git 这一侧 | `commit-scope.ts` 的 `readLastCommit` 注释；`shengsheng.ts` 的 `auditCommitScope` |
| **`--name-status` 与 `--numstat` 同传时前者胜出** | `git log --name-status --numstat` 里 **numstat 行根本不出现**——于是「+596」这个**事故最重要的特征**被静默丢掉（实测：首版就这么写，用例里所有文件都显示成“（二进制）”）。要两个都要，用 `--raw --numstat`（raw 块 + numstat 块各自成块） | `commit-scope.ts` 的 `parseLastCommit` 注释；实测 2026-10-17 |

## 场景 17：验收一单「机制类」交付（拦截 / 闸门 / 清理 / 权限 / 范围限制）

> **总判据：正向通过 + 反向通过 + 会红的反证 = 三缺一不算交付。**
> 三者是同一件事的三个层次，各自证明的事完全不同（下面表格第一条把三者并列说清）。

| 坑 | 一句话 | 出处 |
|---|---|---|
| **只有正向断言 = 半个证人** | **正向通过**（「该做的做了」）只证明**「我要做的事对不对」**；**反向通过**（「不该动的没动」：不得 X / 其它 Y 必须不受影响 / 不属于自己的 Z 必须仍存在）才证明**「我不该碰的东西有没有被碰到」**；**会红的反证**（退回实现必须变红）才证明**判据本身有判别力**。⇒ **三缺一不算交付。** 缺正向是「没做」；缺反证是「不知道判据是不是恒绿的仪式」；**缺反向最危险——它是三者里唯一不会自己喊出来的**：正向缺失会红，反证缺失会在回退时暴露，而「碰了不该碰的」在正常路径上**毫无信号**。本仓实测代价（两起都是「只验了正向」）：① 1169 行被带走——`shared-lease` 的 `856ba9bb` 用宽范围暂存把另一条仍在跑的线两份未完成交付物（`proc-tree.ts` +596、`shengsheng-retire-tree.test.ts` +573）一起提交，当时没有「**不得包含别人的文件**」这条反向断言；② 并发 build 写坏缓存——同一窗口有人用 **10 路并发 `st build`** 写坏全机共享对象缓存，当时没有「**不得影响他人构建**」这条断言。两个正面标杆：`retire-tree`（`a6556005`）构造 **5 个进程覆盖 4 种归因**，扫 417 个只挑 3 个——应杀的 3 个（自己的标记 125820 / 自己标记+fork 125821 / 递归找到的孙进程 125824）**已不在 ✓**，不得杀的 3 个（无标记 125822、125825，以及**别人的标记 ffff…（125823）——绝不能杀**）**仍活着 ✓**；它的自述是**「丢掉『标记属别的会话就不碰』这一条，就等于『把全机同名的都杀了』——反向用例必须把它拦住（跨线祸及比不杀更坏）」**。`shared-lease`（`856ba9bb`）另一面：同仓 B 根被顺延（回显五要素 + 标「跨线」），**不同 git 仓的 C 根照派**（资源名是不同键 `repo:.../gebai:...` vs `repo:.../other-repo:...`）——那条反向用例才是「**不会把所有人互相锁死**」的证据 | `packages/agents/src/agents/shengsheng/shengsheng.md` 第六节；`packages/agents/scripts/reverse_verify_retire_tree.py` 的案例 B（退回「按命令行猜归因」必须变红）；`packages/agents/src/agents/shengsheng/shengsheng-retire-tree.test.ts` 的「真场景」用例；`CONVENTIONS.md` §7.3；实测 2026-10-17 |

**派单侧配套**：机制类工单的 `deliver` **必须含一条反向断言（不是可选项）**，句式可照抄——「反向验收：构造 <不属于本单的对象>，本单动作后它**必须不受影响**（否则会 <具体祸及>）」。三个占位（对象 / 不得发生的动作 / 具体祸及）缺任一处，这条断言就无法判真假，也就无法据此拒收。

**收单侧配套**：反向断言要能**真的抓住**它声称防的事——把实现退回去（如把「标记属别的会话就不碰」改成恒真）之后，那条反向用例**必须红**；不红就说明它写的是注释而不是判据（与场景 15「回退后 0 条红」三种成因同源）。

## 怎么用这份速查

**开工前**：扫一眼你**当前场景**那一节（30 秒）。
**卡住时**：先来这里查"这类事以前踩过什么"，再去出处看详情。

> 若某条让你省下了往返，**回来给它加出处链接或补一条**——
> 这份表的价值就是"下一个撞同一堵墙的人能查到"。
