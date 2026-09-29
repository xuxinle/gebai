# 霜天（Shuangtian）— 歌白内置原生桌面应用框架

> 霜天曉角：清冽、开阔、万物自明。
>
> **C++20 · 零第三方依赖 · 全自绘 · 软硬件渲染兼容 · 支持无头模式 · TCP 控制通道**

霜天是歌白的**原生躯体**：不依赖系统控件、不依赖桌面环境、不依赖第三方库，把"窗口 + 组件 + 绘制 + 字体 + 文本 + 输入 + 远控"这一整套从零做起，并对外提供一条 TCP 控制通道，让智能体可以像操作浏览器一样操作原生应用。

## 它解决什么问题

| 场景 | 霜天给的答案 |
|---|---|
| **Linux 服务器没有桌面，怎么开发原生界面？** | `--headless` 离屏渲染（软件光栅器是唯一真相源），像素结果与有窗口时逐像素一致；截图经控制通道取回 |
| **智能体怎么"看见"并"操作"原生界面？** | TCP 控制通道（`st-control/1`）：组件树/选择器查询、属性读写、动作触发、鼠标键盘注入、视觉树、条件等待、运行指标 |
| **一块代码要在 Windows/Linux/macOS 外观一致** | 全自绘：没有系统控件，所有像素由自己的光栅器产生；平台差异只集中在窗口后端（运行时 `dlopen` 探测，缺失自动回退 headless） |
| **没有 GPU / 驱动不全** | 软件光栅器（扫描线覆盖率抗锯齿 + SIMD 快路径）为默认且完整可用；硬件后端是**可选加速**，不作为前提 |
| **HiDPI 屏上字发虚、发丝线糊** | 逻辑像素 / 物理像素分离：`Canvas` 绘制 API 收逻辑坐标，内部按 `device_scale` 在**物理分辨率**上光栅化（字形亦按物理尺寸重栅格化）；运行时 `app.set_scale` 即时切换 |
| **要写代码编辑器（高亮/行号/编辑）** | 内置 30 种主流语言的语法高亮（规则驱动、**可自定义语言**：注册一份规则即可，与内置语言同一台扫描器）；`ui::CodeEditor` 提供编辑/选择/撤销/缩进/注释切换/只读查看器与完整控制通道属性面 |
| **要写大模型应用（流式 Markdown）** | `st::md`（解析 + 流式增量 + 零依赖代码高亮）+ `ui::MarkdownView` 组件：`append_chunk` 边生成边渲染，前缀稳定不跳变 |
| **不想引入第三方依赖（含包管理）** | 本体零依赖；`stpm`（`st` CLI）自管构建与**第三方源码依赖**（语义版本回溯求解 + SHA-256 校验 + 缓存 + vendor 固化 + 直接驱动编译器，不经 CMake/Make） |
| **编译太慢** | 预编译头 + 头依赖增量（`-MMD` 解析）+ 8 路并行 + lld 链接 + 目标级源排除：全量数十秒、改一文件秒级、无改动毫秒级 |

## 目录与分层

```
shuangtian/
├── CONVENTIONS.md   # 编码契约：全现代 C++20 + 禁用易错特性（编译器强制 + st lint 扫描）
├── DESIGN.md        # 权威设计（分层、接口、协议、DPI、包管理、里程碑）
├── README.md        # 本文件
├── st.pkg           # 工程清单（由 stpm 读取；目标：gallery / mdeditor / st 自身）
├── bootstrap.sh     # 自举：用编译器直接编出 st（唯一非 st 构建入口，8 路并行）
├── include/st/{core,math,codec,raster,text,md,ui,shell,gpu,control,app,pkg,gpu}/
├── src/<层>/…       # 实现（与头同名；platform_*.cpp 为系统 API 单点封装）
├── examples/gallery/    # 示例一：组件集 / 设计系统巡检
├── examples/mdeditor/   # 示例二：Markdown 编辑器（含四个自绘定制组件）
├── examples/codeeditor/ # 示例三：代码编辑器（语法高亮 / 自定义语言 / 只读查看器）
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
./build/bin/st build mdeditor --profile dev

# ③ 无头启动（自动分配控制端口并写控制文件）
./build/dev/bin/mdeditor --headless --control-port 0 --control-file /tmp/st-ctl.json &

# ④ 用控制通道看与操作（tools/st_probe.py 是最小示例客户端）
python3 tools/st_probe.py            # 或以控制文件为参数
python3 tools/st_visual_check.py     # 完整视觉验证：dev + san 两档、DPI/主题切换、截图落盘

# ⑤ 测试与禁令扫描
./build/bin/st test                  # 全部单元测试
./build/bin/st test --san            # ASan + UBSan 档
./build/bin/st lint                  # 禁用特性静态扫描（CONVENTIONS §8）
```

在歌白智能体里，这一切由 **`shuangtian` 子代理**封装为工具：`shuangtian_run`（构建/测试/lint/启动/停止）、`shuangtian_tree/find/get/set/invoke`、`shuangtian_click/type/key`、`shuangtian_capture`（截图直接可见）、`shuangtian_visual/wait/metrics/call`。

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

规模：**46 个翻译单元**（应用目标已排除工具链模块 `src/pkg/*`），8 核。

| 场景 | 命令 | 耗时 |
|---|---|---|
| 全量（dev，PCH + 8 路并行 + lld） | `st build gallery --profile dev` | **≈ 56 s**（编译 51.4 s + 链接 4.5 s） |
| 全量（quick，`-O0`，最快迭代） | `st build gallery --profile quick` | **≈ 33 s**（实测 30 s 编译 + 3 s 链接） |
| 换一个应用目标（复用库对象） | `st build mdeditor --profile dev` | **5.0 s**（仅链接） |
| 改 1 个 `.cpp` | 同上 | **3.1 s**（只重编该单元 + 重链） |
| 无改动 | 同上 | **≈ 0.02 s**（跳编译与链接：实测 22 ms） |
| 改 1 个公共头 | 同上 | 只重编依赖该头的单元（依据 `.d` 依赖图） |

四项机制（详见 `DESIGN.md` §7.5.1）：预编译头（**仅标准库**——把项目头也塞进去反而更慢，实测 3.5s→5.0s）、`-MMD` 头依赖增量、并行编译、lld 链接。

## 示例

### `gallery` — 组件集 / 设计系统巡检
导航栏、统计卡、按钮矩阵（5 变体 × 3 尺寸）、图标墙（50+ 自绘矢量图标）、表单、列表、进度条、主题切换、DPI 切换、截图按钮；覆盖组件库与设计令牌的一致性检查（`png` 中 DPI 2x 截图）。

### `codeeditor` — 代码编辑器（语法高亮 + 自定义语言）
左侧可写编辑器、右侧只读查看器；顶部一键切换 8 种内置语言样例（C++/Python/Rust/JSON/YAML/HTML/SQL/Diff）
加一个**运行时注册的自定义语言** `stlog`（业务日志格式）；亮/暗主题、DPI 切换、Ctrl+/ 注释、拖拽选择等。
既是能力演示，也是"文件编辑器"这一形态的最小完整实现。

### `mdeditor` — Markdown 编辑器（完备性 / 易用性 / 高阶定制）
左侧大纲 + 中间编辑区 + 右侧实时预览 + 底部语法高亮源码视图 + 工具栏 + 状态栏；文件保存/打开对话框、主题与 DPI 切换、**流式生成演示**（按 token 节奏 `append_chunk` 进预览）。

其中四个**框架里没有的组件**全部只靠 `Element` 的四个扩展点（`measure`/`arrange`/`paint_content`/`on_event`）写成，并复用同一套主题令牌与文本端口：

| 定制组件 | 做什么 | 说明 |
|---|---|---|
| `SourceView` | 带行号 + Markdown 语法着色的源码视图 | 逐行分层着色（标题/引用/围栏/行内码/强调），自带滚动 |
| `OutlinePanel` | 可点击文档大纲 | 消费 `st::md` 解析树，点击跳转预览 |
| `SplitHandle` | 可拖拽左右分栏 | 拖拽中实时改比例 |
| `StatsBar` | 实时字数/词数/行数/块数/阅读时长 | 每次编辑即时重算 |

这正是"高阶定制能力"的判据：**不需改动框架代码，也不需新增内置控件**。

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
| 窗口后端（x11 / wayland / win32） | ⏳ 运行时探测已就绪，窗口实现在 v0.2（缺失自动回退 headless，不阻断流程） |
| 硬件合成（Vulkan/GL） | ⏳ 探测与回退已定形（`gpu` 层），实测需有 GPU 的机器 |
| DPI（含非整数 1.5x、运行时切换） | ✅ |
| 字体（TTF/OTF/OTC-CFF/CID、CJK 回退、SC face 优选） | ✅ |
| Markdown（解析 / 流式 / 高亮 / 渲染组件） | ✅ |
| TCP 控制通道（tree/find/get/set/invoke/input.*/capture/visual/wait/metrics/events/theme/app） | ✅ |
| 自研包管理器 `stpm`（求解/lock/获取/校验/vendor/构建/lint） | ✅ 构建与 lint 完整；第三方源码获取限制见 `DESIGN.md` §7.4 |
| 动画与过渡系统 | ⏳ v0.2 |

## 相关文档

- `DESIGN.md` — 权威设计（架构、接口、协议规范、DPI 契约、设计令牌、包管理、里程碑）
- `CONVENTIONS.md` — 编码契约（禁用特性清单 + 编译强制集 + `st lint` 规则）
