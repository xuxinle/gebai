# 霜天（Shuangtian）— 歌白内置原生桌面应用框架

> 霜天曉角：清冽、开阔、万物自明。
>
> **C++20 · 自研实现为体（仅四个外部依赖：一必需三可选）· 全自绘 · 软硬件渲染兼容 · 支持无头模式 · TCP 控制通道**

霜天是歌白的**原生躯体**：不依赖系统控件、不依赖桌面环境，把"窗口 + 组件 + 绘制 + 字体 + 文本 + 输入 + 远控"这一整套从零做起，并对外提供一条 TCP 控制通道，让智能体可以像操作浏览器一样操作原生应用。

外部依赖四个（一个必需、三个可选），全部内联在 `third_party/`（版本/来源/许可/SHA-256 全程台账，`sha256sum -c` 可校验，见 `third_party/SOURCES.md`）：

| 依赖 | 用途 | 性质 |
|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) 3.12.0 | JSON 解析/序列化（清单、锁文件、控制通道协议） | 必需（单头） |
| [quickjs-ng](https://github.com/quickjs-ng/quickjs) 0.17.0 | 应用内脚本层：**用 JS 写组件控制逻辑**，AI 经控制通道 `script` 读写界面（可选能力，**默认关闭**） | 可选 |
| [batterycenter/embed](https://github.com/batterycenter/embed) 1.2.19 | 编译期资源嵌入（`b::embed<"x.png">()`）；**生成期由 stpm 原生实现**，不引入 CMake | 可选 |
| [SQLite](https://sqlite.org/) 3.50.2 | 嵌入式数据库（`st::ext::Database`）——**amalgamation 源码内置**（上游官方推荐的用法），跨三平台同版本、零安装 | 可选（源码内置） |

渲染、字体（TTF/OTF/CID 解析与整形）、文本布局、Markdown、组件库、包管理器与构建驱动**全部自研**——
引入的只是四块"不值得自己写"的基础设施。详见 `CONVENTIONS.md` §3.8 与 `third_party/SOURCES.md`。

## 它解决什么问题

| 场景 | 霜天给的答案 |
|---|---|
| **Linux 服务器没有桌面，怎么开发原生界面？** | `--headless` 离屏渲染（GPU 走 D3D11/WARP，不可用则软件光栅器），像素结果与有窗口时逐像素一致；截图经控制通道取回 |
| **智能体怎么"看见"并"操作"原生界面？** | TCP 控制通道（`st-control/1`）：组件树/选择器查询、属性读写、动作触发、鼠标键盘注入、视觉树、条件等待、运行指标 |
| **一块代码要在 Windows/Linux/macOS 外观一致** | 全自绘：没有系统控件，所有像素由自己的光栅器产生；平台差异只集中在窗口后端（运行时 `dlopen` 探测，缺失自动回退 headless） |
| **没有 GPU / 驱动不全** | 软件光栅器（扫描线覆盖率抗锯齿 + SIMD 快路径）完整可用、且是**语义真相源**；GPU（D3D11）是首选路径，不可用时自动回退，不阻断任何功能 |
| **HiDPI 屏上字发虚、发丝线糊** | 逻辑像素 / 物理像素分离：`Canvas` 绘制 API 收逻辑坐标，内部按 `device_scale` 在**物理分辨率**上光栅化（字形亦按物理尺寸重栅格化）；运行时 `app.set_scale` 即时切换 |
| **想让界面逻辑少写 C++** | 内置脚本层（QuickJS，默认关闭）：`$('#status').set({text:'…'})`、`on('#save','click',…)`、`every(1000,…)`——读写与协议 `get`/`set`/`invoke` **同一份实现**；跨语言边界用「快照批量 + 变更集提交」，跨界次数与改了多少属性无关 |
| **要真的弹出窗口** | Windows 上开箱即用（直接双击 exe）：Win32 窗口后端已实现——DPI 感知、鼠标/键盘/滚轮、剪贴板、窗口缩放跟随；Linux 侧 `x11`/`wayland` 仍是探测 + 明确 `Unsupported` |
| **要在别处写一个霜天应用** | 独立工程：清单写 `"framework": {"path": "…"}` 即可（源/头/标志/嵌入/交叉编译工具链自动并入，无需安装、无需 CMake）；`st init` 生成工程骨架，首个工程冷构建 ≈30s、**第二个 ≈4s**（共享对象缓存）。详见 `docs/independent_project.md` |
| **要在 Linux 上产出 Windows 程序** | 交叉编译：清单声明工具链，`st build gallery --toolchain=mingw` → `build/dev-mingw/bin/gallery.exe`（实测 PE32+，仅依赖 Windows 自带 DLL） |
| **要保证代码在三个平台都对** | `CONVENTIONS` §10 跨平台强制约束（平台差异只能进 `platform_*`；路径统一 UTF-8 经 `st::fs`；`argv` 经 `ST_MAIN` 正规化；系统库按目标平台解析） |
| **要把资源文件编进程序** | 编译期嵌入：清单里写 `"embed": ["assets/*"]`，代码里 `b::embed<"assets/logo.png">()`——路径写错**编译期**就报错，开发期文件变了还能热重载 |
| **要写代码编辑器（高亮/行号/编辑）** | 内置 30 种主流语言的语法高亮（规则驱动、**可自定义语言**：注册一份规则即可，与内置语言同一台扫描器）；`ui::CodeEditor` 提供编辑/选择/撤销/缩进/注释切换/只读查看器与完整控制通道属性面 |
| **要写大模型应用（流式 Markdown）** | `st::md`（解析 + 流式增量 + 零依赖代码高亮）+ `ui::MarkdownView` 组件：`append_chunk` 边生成边渲染，前缀稳定不跳变 |
| **要把数据存下来（本地库/索引/缓存）** | `st::ext::Database`：**SQLite 源码内置**（amalgamation 随仓库分发，3.50.2，跨三平台同版本、零安装、无 ABI 版本问题），封装成 `Result` 风格的 C++20 面（预处理语句、事务与嵌套（SAVEPOINT）、五种值类型与 BLOB）；编译期开关集中在 `third_party/sqlite/st_sqlite3_config.h`（上游文件一字未改）。见 `docs/sqlite_integration.md` |
| **不想引入第三方依赖（含包管理）** | 本体零依赖；`stpm`（`st` CLI）自管构建与**第三方源码依赖**（语义版本回溯求解 + SHA-256 校验 + 缓存 + vendor 固化 + 直接驱动编译器，不经 CMake/Make） |
| **编译太慢** | 预编译头 + **轻量调试信息**（迭代档 `-g1`：重单元 -25%~-33%、对象 -73%）+ 头依赖增量（GCC `-MMD` / MSVC `/sourceDependencies`）+ 并行编译（**并发按内存与 CPU 配额两个上界推导**）+ 共享对象缓存：全量数十秒、改一文件秒级、无改动亚秒级 |
| **软件渲染会不会很慢？** | 覆盖率以**运行段**逐行交付（非逐像素数组）+ 逐项 SIMD + 阴影遮罩缓存：画廊全量重绘 1280×800 **≈12 ms**（原 63.2 ms）。性能手法的收益与代价（含一次失败的 SIMD 尝试）全部记在 `DESIGN.md` §4.2.3；`ST_PAINT_PROFILE=1` 可看逐原语分解（§4.2.8） |
| **界面好不好看、会不会越改越糊？** | 设计令牌带**可断言契约**：正文/辅助小字/语义色/按钮面/焦点环的对比度下限、文字三级的单调层次、阴影三档的两层结构与单调性、尺度阶梯——全部写成测试（`tests/ui_theme_test.cpp`）。改调色板越线立刻红灯，而不是等有人肉眼发现（§4.2.4） |
| **组件齐不齐、能不能一眼核验？** | 组件画廊（`examples/gallery`）分 **5 页**：概览 / 组件（选择控件·滑块·标签页·下拉·徽标·**72 个图标全集**·文字样式）/ 数据（表格·列表·键值）/ 控制通道（可被远程驱动的靶区）/ 关于（真实 Markdown 渲染）。图标全集把**图形与名字并排**——“图标转了个方向”这类错误在单个图标上看不出来，并排才看得出来 |

## 目录与分层

```
shuangtian/
├── CONVENTIONS.md   # 编码契约：全现代 C++20 + 禁用易错特性（编译器强制 + st lint 扫描）
├── DESIGN.md        # 权威设计（分层、接口、协议、DPI、包管理、里程碑）
├── README.md        # 本文件
├── st.pkg           # 工程清单（由 stpm 读取；目标：gallery / codeeditor / st 自身）
├── bootstrap.sh     # 自举（Linux/macOS）：用编译器直接编出 st（唯一非 st 构建入口，8 路并行）
├── bootstrap.ps1    # 自举（Windows）：同上，g++（MinGW-w64）优先、MSVC 回退（vswhere + vcvars64）
├── include/st/{core,math,codec,raster,text,md,ui,shell,gpu,control,app,pkg,ext}/
├── third_party/        # 第三方源码内联（nlohmann/json + quickjs-ng + battery/embed + sqlite，见 third_party/SOURCES.md 与 CHECKSUMS.sha256）
├── src/<层>/…       # 实现（与头同名；platform_*.cpp 为系统 API 单点封装）
├── examples/gallery/    # 示例一：组件集 / 设计系统巡检（含「声明式」页）
├── examples/codeeditor/ # 示例二：代码编辑器（声明式组装的 VSCode 式布局）
├── tests/               # 自研测试框架（ST_TEST/ST_CHECK…），st test 运行
└── tools/               # stpm 源码 + 联调脚本（Python）
```

依赖方向单向：`core ← math/codec ← raster ← text ← md ← ui ← app`；`shell` 提供窗口后端，`control` 提供远控，`pkg` 提供工具链。

## 快速开始（Linux 服务器，无桌面）

```bash
cd shuangtian

# ① 自举工具链（唯一一次手动编译；之后一切走 st）
./bootstrap.sh

# ② 构建示例（首次全量；增量通常秒级）
./build/bin/st build gallery --profile dev

# ③ 无头启动（自动分配控制端口并写控制文件）
./build/dev/bin/gallery --headless --control-port 0 --control-file /tmp/st-ctl.json &

# ④ 用控制通道看与操作（tools/st_probe.py 是最小示例客户端）
python3 tools/st_probe.py            # 或以控制文件为参数
python3 tools/st_visual_check.py     # 完整视觉验证：dev + san 两档、DPI/主题切换、截图落盘

# ⑤ 测试与禁令扫描
./build/bin/st test                  # 全部单元测试
./build/bin/st test --san            # ASan + UBSan 档
./build/bin/st lint                  # 禁用特性静态扫描（CONVENTIONS §8）
```

在歌白智能体里，这一切由 **`shuangtian` 子代理**封装为工具：`shuangtian_run`（构建/测试/lint/启动/停止）、`shuangtian_tree/find/get/set/invoke`、`shuangtian_click/type/key`、`shuangtian_capture`（截图直接可见）、`shuangtian_visual/wait/metrics/call`。

## 快速开始（Windows）

```powershell
cd shuangtian

# ① 自举工具链（g++ 优先、MSVC 回退；本机实测 ~27s）
pwsh -NoProfile -File .\bootstrap.ps1

# ② 构建示例（首次全量 ~31s；之后增量秒级、无改动毫秒级）
.\build\bin\st.exe build gallery --profile dev

# ③ 无头启动 + 用控制通道看与操作
.\build\dev\bin\gallery.exe --headless --control-port 0 --control-file $env:TEMP\st-ctl.json

# ④ 测试与禁令扫描
.\build\bin\st.exe test
.\build\bin\st.exe lint

# ⑤ 外挂性能基准（可选：强制全量重绘 N 帧，打印阶段耗时）
$env:ST_PAINT_PROFILE=1; .\build\dev\bin\gallery.exe --headless --bench 60
```

Windows 上的工具链口径（详见 `CONVENTIONS.md` §10.1）：**默认 g++（MinGW-w64）**——同一套 GCC
口径横跨三平台，跟进最新 C++ 标准不受 VS 版本牵制；g++ 缺席或主版本 < 13 时自动回退 MSVC
（无 MSVC 才到这一步）；GCC 风格标志由 `stpm` 统一翻译（MSVC 无等价物的会列出丢弃清单）；
Windows 目标的 GCC 链接默认静态 libgcc/libstdc++（产物双击即跑，不依赖 PATH 上的 dll）。

## 三维渲染：两条腿走路

架构原则（**软件保底、系统高阶锦上添花**）：

| 腿 | 角色 | 平台 | 现状 |
|---|---|---|---|
| **软件光栅器**（自带，含 z-buffer 与逐像素光照） | **保证跨平台可用** · 确定性 · 零依赖 | 所有平台（含无头/CI） | ✅ 已实现 |
| **系统高阶 API** | 锦上添花：有就用、更快 | Windows：D3D11 | ⏳ 2D 已用；**3D 的实现按测量触发**（`DESIGN.md` §8.4.2 给了阈值） |

入口是**平台中立**的 `raster::Scene3D`（`include/st/raster/scene3d.hpp`），
调用方（`ui::SceneView`）不感知用的是哪条腿。

**为什么不统一到某一个图形 API**：统一到 OpenGL 曾是个选项，但它两头都不占——
在自己的主场（Windows）输给 D3D11，在别的平台又不存在（GL 实现是 Windows 专用，
macOS 上 GL 已废弃）。它既不是"保证腿"也不是"加分腿"，因此**已移除**。
三维的保证腿是**软件实现**：非 Windows 上三维不会消失，只是慢一些。

资源去处口径不变：**代码进仓库，二进制与大源码库走资源管理**
（`third_party/` 的 quickjs / nlohmann 内置；模型权重走主仓库 `../../resources/`）。

## 控制通道速览（`st-control/1`）

帧 = `uint32` 大端长度 + UTF-8 JSON；请求 `{id, method, params}`，响应 `{id, ok, result|error}`，事件 `{event, seq, data}`。

```bash
# 组件树 / 选择器查询 / 读属性
{"method":"tree","params":{"depth":3}}
{"method":"find","params":{"selector":"Button[text~=保存]"}}
{"method":"get","params":{"id":"tool-bold"}}

# 操作（id 兼容 `#id` 写法；焦点经 UiRoot 路由，输入类事件按焦点派发）
{"method":"invoke","params":{"id":"#tool-bold","action":"click"}}
{"method":"set","params":{"id":"editor","props":{"text":"新内容"}}}
{"method":"input.mouse","params":{"kind":"click","x":420,"y":720}}
{"method":"input.text","params":{"text":"霜天","id":"editor"}}
{"method":"input.key","params":{"key":"Tab","ctrl":true}}

# 观察
{"method":"capture","params":{"encode":"file","path":"/tmp/a.png"}}   # 物理像素 PNG
{"method":"visual","params":{}}                                       # 绘制层+命中区
{"method":"wait","params":{"for":"text","text":"完成","timeout_ms":3000}}
{"method":"metrics","params":{}}                                      # 后端/无头/DPI/帧耗时
```

**坐标语义**：协议内一切坐标都是**逻辑像素**（与截图所见一致）；物理像素 = 逻辑 × `metrics.device_scale`。

## 编译效率（实测）

规模：**83 个翻译单元**（库 81 + 应用目标 2），4 核配额容器（宿主 10 核）。

| 场景 | 命令 | 耗时 |
|---|---|---|
| 全量（dev，PCH + 按配额 4 路并行 + lld） | `st build gallery --profile dev` | **≈ 75 s**（编译 67 s + 链接 0.1 s） |
| 全量（quick，`-O0 -g1`，最快迭代） | `st build gallery --profile quick` | **≈ 53 s** |
| 冷全量（禁共享缓存：清 `build/` + `ST_NO_CACHE=1`） | `st build gallery --profile dev` | **≈ 75 s**（八十一单元全部重编） |
| 改 1 个 `.cpp` | 同上 | **≈ 2.9 s**（只重编该单元 + 重链） |
| 改 1 个公共头（`element.hpp`，32 单元受影响） | 同上 | **≈ 38 s** |
| 无改动 | 同上 | **≈ 0.6 s**（跳编译与链接） |

五项机制（详见 `DESIGN.md` §7.5.1）：预编译头（**仅标准库**——把项目头也塞进去反而更慢，实测 3.5s→5.0s）、
**轻量调试信息**（迭代档 `-g1` 而非 `-g`，重单元编译 -25%~-33%、对象 -73%）、`-MMD` 头依赖增量、
并行编译（**并发按内存与 CPU 配额两个上界推导**——4 核配额容器里按硬件 10 路跑反而慢 12%）、lld 链接。

## 示例

**两个示例、两个职责面**（示例只留「一看就懂 + 能当靶场」的那两个；
其余能力由单元测试钉住，不另起示例重复验证）：

### `gallery` — 组件集 / 设计系统巡检（能力面）
导航栏、统计卡、按钮矩阵（5 变体 × 3 尺寸）、图标墙、表单、列表、进度条、主题切换、DPI 切换、截图按钮；覆盖组件库与设计令牌的一致性检查（DPI 2x 截图）。

**六页**：概览 / 组件 / 数据 / **声明式** / 控制通道 / 关于。其中「声明式」页演示
`st::ui::dsl` 的四种典型形态（**页壳手搭 + 内容区声明式子树**，`dsl::mount_into`）：
① 状态驱动表单（改输入 → 另一处文本自己变）；② key 对齐列表（增删不悳动已有项身份）；
③ 异步 `resource`（工作线程算，结果回主线程写状态）；④ 条件内容（关掉即从树中裁剪，不是 hidden）；
⑤ **状态系统高层原语**（`memo` 依赖未变不重算 / `effect` 依赖变化跑一次 / `ref` 跨重组稳定且不触发重组）。

### `codeeditor` — 代码编辑器（应用面；**整个界面由声明式描述**）

按 VSCode 的信息架构组装：标题栏（含**脏点**）/ 菜单栏 / 活动栏 + 侧栏（资源管理器·搜索·源代码管理·运行·扩展）/ 标签页编辑区（修改点、可关闭）/ 底部面板（问题·输出·终端）/ 状态栏（分支、错误警告计数、光标位置、选区计数、语言、主题）。

这份界面是一份 `Component::build()` 描述出来的（对标 Compose / ArkTS）：改状态 → 下一帧重组 →
真值树按 diff 更新，「改完要点哪里」的手工同步全部消失（对比同功能的命令式写法：同目录
曾经的 `codeeditor-dsl` 分身已在示例整合中并入本文档——两份合计 2758 行 → 1605 行）。

**内容都是真的，不是占位数据**：

| 能力 | 真实行为 |
|---|---|
| 真实文件工作区 | `--workspace <dir>` 下资源树 `fs::list_dir` 扫描（目录增量展开）、点文件 `fs::read_text` 打开、Ctrl+S `fs::write_text` 真实写盘（脏标记/标题栏/状态栏全联动） |
| **Ctrl+P 快速打开** | 把面板表换成**文件表**（递归扫描工作区，限量 300），打字即过滤（title/detail/id 三路，大小写不敏感），Enter 或点击即打开 |
| 搜索面板 | **递归搜工作区**（跳隐藏目录 / 二进制 / >1MiB 文件），命中给「文件:行 + 片段」，点一下**跳到该行并选中命中** |
| 问题面板 | **真实轻量检查**：行尾空白 / Tab 缩进 / TODO·FIXME / 超长行（>120 列）；点问题跳行；状态栏错误·警告计数与面板**同源** |
| 终端 | `help` / `langs` / `ls`（真实列目录）/ `find <词>`（真的搜并切到搜索面板）/ `goto <行>` / `stats` / `save` / `theme` / `open <文件>` / `clear` |
| 查找替换 | Ctrl+F / Ctrl+H 浮条：输入即查、计数 n/m、↑↓ 环绕、替换/全部替换、Esc 可关 |
| 编辑器 | 自动配对、智能 Home、Ctrl+Backspace/Delete 按词删除、缩进参考线、悬停行、可拖拽双滚动条、Ctrl+/ 注释、Ctrl+Tab 切标签（细节见 `DESIGN.md` §4.2.10） |

菜单栏、全局快捷键（Ctrl+S/W/Tab/B/P/Shift+P）、8 种内置语言样例 + 运行时注册的自定义语言 `stlog`；全部用内置组件组装，零自绘定制控件。

## 与歌白协同（智能体开发闭环）

```
shuangtian_run(action=build) → action=start（无头，返回端口/PID）
   → shuangtian_find/tree（定位）→ shuangtian_capture（看图核验）
   → shuangtian_set/invoke/type/click（操作）→ shuangtian_wait（等条件）
   → 改代码 → action=build（秒级增量）→ 复验 → action=stop（收尾）
```

子代理定义在 `packages/agents/src/agents/shuangtian/`（TS 定义 + 工具集 + 提示词），工具只读类免审批，`set`/`invoke`/`click`/`type`/`key`/`run` 需审批。

## 平台与现状

| 能力 | 状态 |
|---|---|
| 无头后端（headless）+ 软件光栅器 | ✅ 完整（本仓库全部示例与验证都在无头下完成） |
| 窗口后端 | ✅ **win32 完整**（真窗口 + 鼠标键盘 + 剪贴板 + DXGI swapchain 呈现 + DPI 感知与切换 + **首帧画完才显窗**，无启动黑框/白底）；⏳ x11 / wayland 未做（运行时探测缺失即回退 headless，不阻断流程） |
| 硬件合成 | ✅ **D3D11 全链路**（设备层→着色器原语→路径→DXGI 呈现，`--renderer=auto\|gpu\|software`）；三维为软件腿保底（系统高阶腿按测量触发，见 `DESIGN.md` §8.4）；⏳ Vulkan / Metal 待做 |
| DPI（含非整数 1.5x、运行时切换） | ✅ |
| 字体（TTF/OTF/OTC-CFF/CID、CJK 回退、SC face 优选） | ✅ |
| Markdown（解析 / 流式 / 高亮 / 渲染组件） | ✅ |
| TCP 控制通道（tree/find/get/set/invoke/input.*/capture/visual/wait/metrics/events/theme/app） | ✅ |
| 自研包管理器 `stpm`（求解/lock/获取/校验/vendor/构建/lint） | ✅ 构建与 lint 完整；第三方源码获取限制见 `DESIGN.md` §7.4 |
| **Windows 宿主 + g++（MinGW-w64）默认编译器** | ✅ g++ 优先（版本护栏 ≥ 13）、MSVC 可回退（`vswhere`+`vcvars64` 自动定位、标志翻译、`/sourceDependencies` 依赖追踪、`bootstrap.ps1`）；Windows 目标默认静态 libgcc/libstdc++（产物不要求 mingw dll）；实测 g++ 自举 27s / 全量构建 ~31s / 测试全绿 |
| **GPU 渲染（D3D11：硬件 → WARP）** | ✅ **M1–M6 全部落地**：设备层 / 着色器原语（文字与渐变与软件 **Δ0**）/ 投影（**Δ≤1**）/ 路径填充描边 / **DXGI swapchain 呈现** / `auto` 按实测选优。实测总帧 24.66→**1.53 ms**、送显 6.55→**0.03 ms**（详见 `DESIGN.md` §8.3） |
| 动画与过渡 | ✅ 悬浮事件与特效（背景/描边/上浮/发光，`HoverEffect` 声明式）、帧驱动过渡（`UiRoot` 时间轴 + 续帧协议）、3D 旋转 |
| **声明式 UI（`st::ui::dsl` + `ui::DeclarativeHost`）** | ✅ **双宿主**：C++ struct 组件 + `State<T>` + `build()`，与 JS `compose()`/`useState`（复用 ScriptHost）——≈ Jetpack Compose / 鸿蒙 ArkTS；**状态系统高层原语 `memo`/`effect`/`ref`/`persisted`（JS 侧同名的 `useMemo`/`useEffect`/`useRef`/`usePersisted`）**、ArkTS 链式修饰、**`resource`/`useResource` 异步**（线程池 + 取消牌 + 代次丢弃）、**按 key 对齐复用**（`for_each`/`ForEach`：增删重排后同一 key 仍是同一个元素，id `Type@key` 稳定）、**嵌套作用域树**（每层独立失效）、`custom<T>` 逃生舱、`overlay` 生命周期、`menu_bar`、`select`/`table`/`tree` 数据驱动、全局快捷键、条件裁剪、**异常冻结 + 递归 build 深度拦截 + 单帧预算顺延**、**子树挂载 `mount_into`**（宿主界面里的一页用声明式）、**场景卸载**（`compose` 换页不叠树 + `unmount_declarative()` 发 effect 清理）；JS 事件绑定按元素身份持有（复用帧零重绑成本）；双宿主一致性 fixture 四组；示例见上方两个（codeeditor 整页 + gallery 声明式页）；协议 `ui.create`/`ui.remove` 在线建删 |

## 相关文档

**完整地图见 `docs/README.md`**（哪份文档答什么问题）。最常用的三份：

- `DESIGN.md` — 权威设计（架构、接口、协议规范、DPI 契约、设计令牌、包管理）。
  两张表最值得先看：**§8.2 的 50 条实战缺陷**（每条带根因与修复）、**§11 性能目标与实测**
- `CONVENTIONS.md` — 编码契约（禁用特性清单 + 编译强制集 + `st lint` 规则）
- `docs/cross_platform.md` — 跨平台强制约束（写 C++ 前必读）；`docs/independent_project.md` — 用本框架建独立工程
