# 跨平台强制约束：详细版与自查清单

> 本文是 `CONVENTIONS.md` **§10 的展开**（那边是必须遵守的红线摘要，这边是逐条的成因、
> 反例与提交前自查清单）。**两边的编号不一一对应**——CONVENTIONS §10 按"约束类型"分
> （MSVC 口径 / 语言行为差异 / 构建静默失效），本文按"出错面"分（平台层 / 路径 / 入口 /
> 类型 / 构建 / 自查）。因此本文用独立编号，避免读者误以为能按号对照。

框架的目标平台是 **Linux / Windows / macOS**，且必须支持**交叉编译**（如 Linux 上产出 Windows 程序）。
跨平台不是"以后再说"的收尾工作：**一处平台假设会让整个目标平台编不过或运行期出错**，
而它在本机（Linux）往往完全看不出来。因此这些是**强制约束**，与 §2 的禁令同等地位。

## 1. 平台差异只能出现在 `platform_*` 里

| 允许 | 位置 |
|---|---|
| 系统头（`<windows.h>`、`<unistd.h>`、`<sys/socket.h>`、`<dlfcn.h>`…） | `src/**/platform_*.cpp` |
| 平台宏（`_WIN32`/`__linux__`/`__APPLE__`） | 同上；**极少数**确需在公共代码分叉时，必须两侧都能编译并有注释说明 |
| 平台的 `char*` API 边界（`execvp`、`CreateProcess`、宽字符转换） | 同上（§2 R6 的受控例外） |

**跨平台代码不得直接**：`#include <unistd.h>`、调 `getpid()`/`fork()`/`dlopen()`、用 `windows.h` 类型。
需要这类能力时**先在后端加一个平台无关的封装**（如 `process::current_id()`），再在平台文件里实现两侧。

> 反例（真实缺陷）：控制通道 `hello` 直接调 `::getpid()` → Linux 编得过，**Windows 交叉编译直接报
> `'::getpid' has not been declared`**；而且那里的写法 `getpid() == 0 ? 0 : 0` 恒为 0，pid 从来没上报对过。
> 正解：`process::current_id()`，Windows 侧走 `GetCurrentProcessId()`。

> 反例（真实缺陷）：`st::time::format_now` 直接用 POSIX `localtime_r` → Linux/macOS 编得过，
> **Windows（MSVC 与 MinGW）都没有这个函数**，交叉编译直接报未声明。
> 正解：平台无关封装 `st::time::local_fields()`，实现在 `src/core/platform_time.cpp`
> （POSIX `localtime_r`；Windows `localtime_s`，参数顺序相反）。回归 `tests/core_time_test.cpp`。
>
> **为什么公共代码里也该警惕**：`<ctime>` 是标准头、函数看着"通用"，很容易当成平台无关——
> 而 C 运行时里的 **`*_s`/`localtime_r`/`gmtime_r` 这类可重入变体恰恰是平台分裂最严重的一类**。
> 判断标准不是"头文件是否标准"，而是"两侧是否同名同参"。

## 2. 路径：一律 UTF-8 文本，进出都经 `st::fs`

- 框架对内对外**统一 UTF-8** `std::string` 表示路径（与源码、JSON、脚本一致）。
- 路径拼接/比较/绝对路径判定**必须用 `st::fs`**（`join`/`relative_to`/`is_absolute`），
  不要手写 `base + "/" + leaf`（Windows 分隔符是 `\`，且存在盘符与 UNC 路径）。
- **文件流不要用 `std::string` 直接构造**：Windows 上 `std::ifstream(std::string)`
  按**本地 ANSI 代码页**解释，中文路径必然打不开。
  `fs` 层已用宽字符路径（`std::filesystem::path` 在 Windows 内部是 `wstring`）——
  新代码走 `fs::read_text/read_bytes/write_text`，或 `fs::to_path()`/`fs::to_utf8()`。
- 大小写：Windows/macOS 文件系统**不区分大小写**，别用大小写来区分文件。

```cpp
// 反例：中文路径在 Windows 上必然失败
std::ifstream stream(std::string(path));            // ✗
// 正例：走 fs 层（内部已是宽字符路径）
auto text = st::fs::read_text(path);                // ✓
// 正例：需要自己拿 path 时
auto stream = std::ifstream(st::fs::to_path(path)); // ✓
```

## 3. 进程入口：`argv` 在 Windows 是 ANSI

命令行参数在 Windows 是**本地 ANSI 编码**（中文机器是 GBK），Linux/macOS 才是 UTF-8。
示例与工具程序统一用 `ST_MAIN(fn)`（`st/core/entry.hpp`）：它在入口处把参数正规化成 UTF-8，
顺带把 Windows 控制台代码页设为 UTF-8（否则中文日志乱码）。

```cpp
#include "st/core/entry.hpp"
auto run_app(int argc, char** argv) -> int { /* 这里一定是 UTF-8 */ }
ST_MAIN(run_app)
```

## 4. 类型与格式化

| 陷阱 | 规则 |
|---|---|
| `long` 在 Windows 是 32 位、Linux 64 位 | 需要定宽就用 `std::int32_t/int64_t/std::size_t`，**不要用 `long` 表示字节数/偏移** |
| 打印 | 一律 `std::format`/`st::print`（避免 `%ld`/`%lld` 的平台差异；L7 已全局禁 printf） |
| 字符类型 | 文本一律 `char` + UTF-8；Windows 宽字符只在平台文件的 API 边界出现 |
| 结构体布局/打包 | 不假设 ABI；需要固定布局用显式类型与 `static_assert` |
| `size_t` 与整数混算 | 保持 `-Wconversion` 干净（本项目已开 `-Werror`，跨平台后更值得保持） |

## 5. 系统库、工具链与构建

- **系统库按目标平台解析**（`default_system_libs(platform)`），**不能**用宿主宏判断：

  | 平台 | 系统库 |
  |---|---|
  | linux | `pthread` `dl` `m` |
  | windows | `ws2_32`（交叉工具链可声明 `winpthread` 等） |
  | darwin | （默认无；按需声明） |

- 交叉编译在 `st.pkg` 的 `toolchains` 段声明（编译器、目标平台、系统库、宏、产物后缀），
  用 `st build <target> --toolchain=<名>` 启用；**产物与中间目录隔离**为 `build/<档位>-<工具链>/`。
- 交叉编译时**不套用宿主的可选特性**（如 `-fuse-ld=lld`/`mold`：宿主装的不一定支持目标格式）。
- 交叉产物的**运行时行为**无法在本机验证，因此新增平台分支必须至少做到"交叉编译通过"。
- **交叉 ≠ 一定不可在本机执行**：目标平台与宿主相同时（Windows 宿主上编 mingw 档）产物就是本机程序，
  `st test`/`st run --toolchain=<名>` 照常执行；仅当目标≠宿主才拒绝并提示在目标平台运行。

## 6. 自查清单（提交前逐条过）

1. 有没有新增系统头 / 平台宏 / `char*` API？→ 必须在 `platform_*` 里。
2. 有没有直接构造文件流或用字符串拼路径？→ 换成 `st::fs`。
3. 有没有假设 `long` 是 64 位、或依赖整数宽度？（本机跑不出问题）
4. 有没有在非平台文件里用 POSIX 函数（`getpid`/`usleep`/…）？
5. Windows 分支改了？→ 跑 `st build <target> --toolchain=mingw` 确认**编得过**
   （本机编不到 Windows 分支，这是唯一能发现问题的途径）。
6. 新增系统库依赖？→ 在 `toolchains` 里为目标平台声明。
7. 有没有让窗口系统画标题栏/边框？→ **不允许**：窗口装饰一律 UI 层自绘（`CONVENTIONS.md` §10 第 7 条）。
   需要平台能力（去边框建窗、拖拽与八向缩放接管、最小化/最大化/关闭）时，先在 `st::shell`
   契约里加接口，再在 `platform_*` 里两侧实现——**不要在应用层各写一套 `#if`**。

## 7. 内置通道（无头）与桌面的**一致性**约束（验收口径）

「无头截图看着好、实际运行发糊/错位」这类问题全部来自两侧**默认口径不同源**，
因此两侧的默认必须逐项对齐（已全部落实）：

| 维度 | 口径 | 保证方式 |
|---|---|---|
| 文本形态 | 亚像素 LCD + 网格拟合 normal（两通道同一默认） | `resolve_text_lcd` / `resolve_text_fit` 不读“有没有窗口”（`src/app/app.cpp`） |
| 像素密度 | 未显式指定时：窗口取窗口实际 DPI，无头取**系统显示缩放**（Windows） | `shell::system_display_scale()`；显式 `--scale`/`ST_SCALE` 两侧接管 |
| 呈现路径 | 窗口侧帧缓冲 →（DXGI swapchain / GDI blit）→ 客户区必须逐像素一致 | `tools/st_consistency_check.py`：`PrintWindow(PW_CLIENTONLY\|PW_RENDERFULLCONTENT)` 与帧缓冲逐像素比对 |
| 窗口装饰 | **无系统标题栏/边框**，窗框内每个像素都来自帧缓冲 | 平台后端去装饰建窗（`CONVENTIONS.md` §10 第 7 条）；✅ **三平台都已落地**：Win32 `WS_POPUP`+`WM_NCCALCSIZE`（`tools/title_bar_win_check.py`：客户区 == 窗口矩形）、X11 `_MOTIF_WM_HINTS`（`tools/x11_win_check.py`：无 `_NET_FRAME_EXTENTS` + 客户区 == 画布）、Wayland `xdg-decoration client_side`（`tools/wayland_win_check.py`） |
| 窗口尺寸真源 | 客户区 == 帧缓冲物理尺寸（不符即"被系统拉伸"） | Win32：客户区随时可问；X11：`XGetWindowAttributes`；Wayland：**只有 configure 事件**。三者的后端都以"实际客户区/configure 尺寸"为准重建缓冲，且**改 DPI 时窗口尺寸与缓冲一起改**（只改一半就是老病根的复发） |

**改了默认口径就要同步这个表**，并用一致性脚本重测（不是靠“看起来一样”）。

## 8. Linux 桌面会话上的验证（2026-11-24 新增）

前面的“一致性约束”说的都是**无头 vs 桌面**的像素口径。但 **Linux（X11/Wayland）**上还有一条
差异是 Windows 不会遇到的：`XMapWindow` 是**异步**的，映射前的像素会被 WM 背景盖掉；
而 Win32 的 `ShowWindow` 之后 GDI 直写立即生效。这一条曾让“首帧只在 Windows 上真正上了屏”。

### 8.1 真桌面验证的三个工具

| 工具 | 验什么 | 为什么不能用无头代替 |
|---|---|---|
| `tools/linux_desktop_shoot.py` | 应用能起、**像素真上屏** | `capture` 读的是进程内帧缓冲；它证明“画对了”，不证明“送到了屏幕” |
| `tools/linux_window_semantics_check.py` | 图标/去装饰/工作区/置底/最大化/光标 | 这些都是**写给 WM 的状态**，进程内属性面看不到 |
| `tools/linux_clipboard_check.py` | 跨进程剪贴板双向 | 进程内缓存“写进去读回来”全绿，但与别的应用不通时界面上一样 |

### 8.2 环境准备（两个坑）

```bash
export XAUTHORITY=/run/user/1002/.mutter-Xwaylandauth.0Q5NU3   # ① ：0 的授权不在此环境变量里
export DISPLAY=:0
loginctl unlock-session                                       # ② 锁屏下截图只能拍到锁屏
```

① 不设 `XAUTHORITY` 会报 `Authorization required`；它要**显式**指向 mutter 的 Xwayland
   auth 文件（路径里的随机串每次登录可能不同，用 `ls /run/user/*/.mutter-Xwaylandauth.*` 找）。

② 锁屏状态下所有应用截图**长得一模一样**（都是同张锁屏壁纸）——那会把“没测”
   伪装成“测过了”。这是自动化验收里代价最高的假阳性。

### 8.3 已发现的平台行为差异（必须在后端处理，不能在应用层 `#if`）

| 行为 | Win32 | Linux（X11/GNOME）| 后端怎么处理 |
|---|---|---|---|
| 显窗前的上屏 | `ShowWindow` 后 GDI 直写立即生效 | 映射前的 `XShmPutImage` 被 WM 背景盖掉 | `show_when_ready` 里先 `sync_buffers_to_client` 再 `blit`，且应用层之后还要再画一帧 |
| 尺寸接近工作区的窗口 | 按请求值 | **映射后 10ms 内** resize 到工作区 92% 会被自动最大化 | 铺开推迟到映射后 + 等一个 WM 回合（15ms）|
| 置底 | `HWND_BOTTOM` 只管当下，需持续监督 | `_NET_WM_STATE_BELOW` 是**持续状态**，发一次即可 | 各自实现，不强行统一 |
| 双缓冲后备内容 | 同一块后备缓冲（天然保留上帧）| 每帧换共享段（**不**保留上帧）| X11 侧为每块段记 `slot_primed_`，首次整屏拷一次 |

**共同教训**：这些都没法靠读代码发现——它们在两边**都编得过、都能跑**，
只是 Linux 上的表现是“屏幕黑 / 窗口被最大化 / 拷不出去”。只有真机截图才能把它们逐出来。

> 完整缺陷清单与实测数据见 `docs/LINUX_DESKTOP_ADAPTATION_20261124.md`。

## 9. 测试用例的跨平台口径（2026-11-24 新增）

Linux 适配收尾阶段发现：**12 个失败用例全是“在 Windows 上绿、到 Linux 上红”**，
而且它们已经在仓里摆了几个月。根因可以归四类，每一类的修法都不同——
**一律加 `ST_SKIP` 是错的**（那会把真缺陷盖掉）。

### 9.1 四类根因与正确修法

| 类型 | 实例 | 正确修法 |
|---|---|---|
| **平台假设写死** | `pkg_library_test` 硬编码 `bin/a.exe` | **修测试**：按 `_WIN32` 推后缀（`host_executable_suffix()`）。这验的“合成目标与失败隔离”在 Linux 上**一样有语义**，跳过等于让功能裸奔 |
| **实现里藏了平台专属判据** | `separate_debug_info` 早退只认 PE 的 `symbols stripped` 文案 | **修实现**：改成跨格式成立的“段存在性”（`.symtab` 在不在）。否则 Linux 上每次构建都白跑三遍 objcopy |
| **接口在两个分支上语义不一致** | `read_content_at(Worktree)` 缺文件时设 `error`，而 git 分支清 `error` | **修实现**：统一成 `missing=true && error.empty()`（新增/删除不是错误） |
| **测试耦合到环境取值** | 主题令牌“头部栏色 == 轨道空白色”（实测差 3 个 RGB）、量化对拉丁的收益（-10%，因字体而异）、emoji 字体能否加载 | **改判据**：改成量**真语义**（“落在头部栏区域且是浅色底”、“不变差即可”、“真的找到了字形”），**不是**放宽阈值到能过为止 |

### 9.2 `ST_SKIP` 的三个硬规矩（踩过）

1. **`ST_SKIP` 只标记、不转移控制流**——后面必须自己 `return`。本次有 12 处
   `if (!x) ST_SKIP(...)` 漏了 `return`，结果打印了 SKIP 还继续往下断言，反而报了
   FAIL+SKIP 两条（汇总行看着像“又跳过又失败”）。
2. **静默 `return` 不是跳道**——`if (!env) return;` 在那个用例上计成 **PASS**，
   把“没验”伪装成绿灯（仓里 `tests/skip_visibility_test.cpp` 就是为这件事立的）。
   所有“环境前置不满足”必须走 `ST_SKIP(reason)`。
3. **守卫要问“能力是否存在”，不是“宽度是否非零”**——emoji 用例旧守卫用
   `plain > 0`，而缺字时渲染器回退 **tofu 宽度**（实测 16.0）！于是守卫放行、
   断言全红。正确守卫是 `find_face(...) != nullptr`（**真的解析到了 face**）。

### 9.3 耗时由外部工具决定的用例要显式超时

`lsp_language_*_smoke` 的耗时取决于**外部语言服务器首轮索引**（pyright 要扫完整
内建 stub 包），不是被测代码的快慢。默认 10s 软超时会把“这台机器慢”报成
“用例超时失败”，且**软超时一旦触发就记 FAIL**（即使用例自己后来走 `ST_SKIP`）。
修法：`ST_TEST_WITH_TIMEOUT(name, 40'000)`，与 `pkg_integration_test` 同一口径。

### 9.4 一个被带出来的**真缺陷**：字体 face 加载失败不缓存

起因是 `font_face_cache_is_process_wide_not_thread_local` 红（`19 != 17`）。
查下去发现不是环境：`load_face_cached` 里写的是 `if (!face) return face;`
——**失败不进缓存**（原注释的理由是“字体可能被装上/卸下”）。

后果在“系统链里有能探到但加载不了的字体”时爆发：本机
`NotoColorEmoji.ttf` 是位图字体（只有 CBDT/CBLC，无 glyf/CFF），引擎不支持 →
**每次重建字体栈都重读 10MB 再解析一遍再失败**。Tasks 池 8 线程就是八倍。
修法：加**负缓存**（`failures` 表，与 `entries` 同一作用域同寿命）。

**教训**：这条用例本来是“护缓存共享”的，却因为一个**未覆盖的路径**（失败加载）
在另一平台上变成了“护一个不存在的不变量”。测试红了先问“它到底在护什么”。
