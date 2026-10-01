# 霜天（Shuangtian）— 歌白内置原生桌面应用框架 · 设计

> 霜天：取自「霜天曉角」——清冽、开阔、万物自明。定位是**歌白的原生躯体**：跨平台、全自绘、软硬件渲染兼容、可在无桌面环境完整运行（无头），并对外暴露一条 **TCP 控制通道**，让智能体可以像操作浏览器一样操作原生应用（组件、视觉元素、键盘鼠标、截图）。
>
> 编写约定见 `CONVENTIONS.md`（**全现代 C++20，禁用易错特性，三重强制**）。

## 目录

| 章 | 内容 |
|---|---|
| [1](#1-目标与非目标) | 目标与非目标 |
| [2](#2-总体架构) | 总体架构 |
| [3](#3-目录结构) | 目录结构 |
| [4](#4-关键接口) | 关键接口（core / raster / text / md / ui / shell / gpu / control / pkg，含 DPI 契约、填充语义、令牌契约、后端选择） |
| [5](#5-设计系统token) | 设计系统（token） |
| [6](#6-控制协议规范st-control1) | 控制协议规范 `st-control/1`（含扩展层、脚本宿主） |
| [7](#7-包管理stpm) | 包管理（stpm） |
| [8](#8-工程实践无头开发闭环与实现现状) | **工程实践**：无头闭环 · 示例检验 · 50 条实战缺陷 · 渲染两条腿（GPU/软件） · 验证体系 |
| [9](#9-与歌白主库的集成) | 与歌白主库的集成 |
| [10](#10-里程碑) | 里程碑与实现现状 |
| [11](#11-性能目标v01-基线) | 性能（目标与实测） |

> 想看「有哪些坑、别重蹈」直接跳 **§8.2**（50 条真实缺陷，每条都带根因与修复）；
> 想看「怎么上手 / 建独立工程」看 `README.md` 与 `docs/`（见 `docs/README.md` 的文档地图）。

## 1. 目标与非目标

**目标**
1. **跨平台**：Windows / Linux / macOS 单一代码库；平台差异集中在 `shell/platform_*` 与 `core/platform_*`。
2. **全自绘**：不依赖任何系统控件（无 GTK/Qt/Win32 控件），所有像素由霜天自己的光栅器产生——外观在任何平台完全一致。
3. **软硬件渲染兼容**：软件光栅器是**唯一真相源**（保证无 GPU、无桌面也 100% 可用），硬件后端（Vulkan/GL）在可用时作为**合成加速**接入；后端缺失自动降级，行为与像素结果一致。
4. **无头模式**：`--headless` 下不创建窗口、不依赖任何显示服务，离屏渲染 → 可截图、可控制、可测试。Linux 服务器无桌面即可开发与验证 UI。
5. **TCP 控制通道**：组件获取与控制（tree/find/get/set/invoke）、视觉元素获取（visual/capture）、键鼠事件触发（input.\*）、条件等待与事件订阅。
6. **美观**：内置一套现代设计系统（设计 token + 组件库 + 自绘矢量图标），默认主题可直接用于产品。
7. **原生 Markdown 渲染**：面向大模型应用（流式增量渲染、GFM、自研代码高亮）。
8. **自研包管理器**：框架本体零第三方依赖；后续引入的第三方源码也由 `stpm` 自己管理（获取/求解/锁定/校验/固化/构建）。

**非目标（本期）**
- 不做浏览器引擎（不解析 HTML/CSS；UI 用 C++ API 声明，样式用 token 系统）。
- 不做富文本编辑（本期是只读 Markdown 渲染 + 输入框）。
- 不做无障碍 API 桥接（自绘应用天然无法被系统 AT 读取；短期以 TCP 语义树替代，`tree`/`find` 即无障碍语义来源）。
- 不做移动端。

## 2. 总体架构

```
                         ┌──────────────────────────── 应用（examples/*）────────────────────────────┐
                         │  gallery（组件画廊）        mdview（大模型 Markdown 应用）                │
                         └───────────────────────────────────┬───────────────────────────────────────┘
                                                             │  st::app::App（窗口/主题/事件循环/页面栈）
┌────────────────────────────── ui 层 ───────────────────────┴───────────────────────────────────────┐
│  Element 树（组件）· FlexLayout（自研布局）· Style/Token/Theme · Component 库 · 矢量图标 · 命中测试   │
└───────────┬───────────────────────────────────────────────────────────────────────────┬──────────────┘
            │ 立即模式直绘：paint(RenderContext, Surface&) —— 原语 fill/圆角矩形/          │ 事件
            │ 渐变/文本/阴影/图像（Surface 为软件 Canvas 与 GpuCanvas 共同抽象）        │
┌───────────▼──────────────────────── raster 层 ────────────────────────────┐   ┌───────▼──────────┐
│ Canvas（像素缓冲）· 路径填充 + 抗锯齿 · 渐变/阴影/裁剪/合成 · 仿射 · SIMD   │   │ shell 层          │
└───────────┬───────────────────────────────┬──────────────────────────────┘   │ 事件循环/窗口后端 │
            │                               │                                  │ headless / x11 /  │
┌───────────▼──────────┐        ┌───────────▼──────────┐                       │ wayland / win32   │
│ text 字体引擎         │        │ codec（PNG/tar/zip/  │                       └───────┬──────────┘
│ TTF/OTF/OTC/CJK      │        │  deflate/inflate）    │                              │
└──────────────────────┘        └──────────────────────┘                       ┌──────▼───────────┐
                                                                                │ gpu 合成后端      │
   md 层（Markdown：解析 / MdStream 流式增量 / 代码高亮）                          │ vulkan/gl（可选）  │
                                                                                └──────────────────┘
┌──────────────────────────── core 层 ───────────────────────────────────────────────────────────────┐
│ JSON · 字符串 · Result/Error · 日志 · 时间 · SHA-256/CRC32 · 文件与路径 · 事件循环与线程 · RAII 封装 │
└────────────────────────────────────────────────────────────────────────────────────────────────────┘
                                   ▲
                    ┌──────────────┴───────────────┐
                    │ control 层：TCP 控制通道       │ ← 歌白 shuangtian 子代理（TS 客户端）
                    │ 协议 + 服务端 + 事件订阅       │
                    └──────────────────────────────┘
                    ┌──────────────────────────────┐
                    │ pkg 层：stpm 引擎（工具链）     │ ← st CLI（init/build/run/add/fetch/vendor/…）
                    └──────────────────────────────┘
```

**分层依赖单向**：`core ← codec/math ← raster ← text ← md ← ui ← app`；`shell` 依赖 `core`+`raster`+`ui`（窗口后端把输入事件以 `ui::Event` 喂给 app）；`gpu`（实现物理上在 raster 层，接口 `include/st/raster/gpu.hpp`）依赖 `raster`；`control` 依赖 `ui`+`ext`，与 `app` 通过**纯虚 `Host` 接口解耦**（依赖倒置：app 实现 Host 并拥有 Server，控制层不反向 include app）；`pkg` 只依赖 `core`+`codec`（清单/求解/归档）。

## 3. 目录结构

```
shuangtian/
├── CONVENTIONS.md        # 编码契约（禁令 + 强制）
├── DESIGN.md             # 本文件（权威设计）
├── README.md             # 使用与协同开发工作流
├── st.pkg                # 框架自身包清单（供 stpm 构建）
├── bootstrap.sh          # 引导编译 stpm 自身（唯一非 st 构建入口）
├── include/st/<层>/*.hpp # 公共头（自包含）
├── src/<层>/*.cpp        # 实现（与头同名优先）
│   └── **/platform_*.cpp # 平台/系统 API 单点封装（禁令受控例外）
├── tests/<层>_<模块>_test.cpp
 ├── examples/{gallery,codeeditor}/
├── third_party/<name>/   # 外部依赖源码（直接内联、随仓库分发）：nlohmann/json、quickjs-ng、
│                         # batterycenter/embed；来源/许可/校验和见 SOURCES.md 与 CHECKSUMS.sha256
└── docs/                 # 控制协议规范、设计 token 表等
```

> `ext/` 层：外部基础设施的适配（JSON 薄封装、脚本引擎）。它与 `core/raster/ui` 的分工是
> “不值得自己写、但必须控住边界” vs “自己写到底”——详 §6.6。

## 4. 关键接口

### 4.1 core
```cpp
namespace st {
template <class T> using Result = std::expected<T, Error>;
enum class ErrorCode { Invalid, NotFound, Io, Parse, Unsupported, Timeout, Internal, Busy, Cancelled };
struct Error { ErrorCode code; std::string message; };
}  // namespace st
```
- `st::json::Value`：自研 JSON（顺序保序对象、UTF-8、解析/序列化、`std::format` 集成）。用于**协议、清单、lock、事件**——一切结构化数据。
- `st::Log`：级别 + 分类 + 结构化字段，默认输出 stderr。
- `st::fs`：`read_file`/`write_file`/`list_dir`/`temp_dir`/`path_join` 等（`std::filesystem` 之上的薄封装）。
  **`read_text` 读到 EOF，不用 `seekg/tellg` 定长读**：`/proc`、`/sys`、cgroup 的虚拟文件报出的
  size 是 0（内容却非空），按大小读会静默得到空串——而"读系统状态"恰好是这些文件的唯一用途
  （实测：cgroup 内存上限探测永远拿到 0，使"按内存推导编译并发"失效）。
- `st::EventLoop`：定时器 + 投递任务 + `std::jthread` worker；`st::Signal`（简易信号槽，`std::function` + RAII 连接）。
- `st::hash`：SHA-256 / FNV-1a / CRC32 / Adler32。

### 4.2 raster
```cpp
namespace st::raster {
class Canvas {                      // 像素缓冲（RGBA8888 预乘 alpha）
 public:
  Canvas(int w, int h);
  int width() const noexcept; int height() const noexcept;
  Color pixel(int x, int y) const;                     // 受检
  void clear(Color c);
  void fill_path(const Path& p, const Paint& paint, const DrawOptions& = {});
  void fill_rect(Rect r, Paint paint, float radius = 0.0f);
  void stroke_path(const Path& p, Paint paint, float width, ...);
  void fill_gradient(Rect r, const Gradient&, float radius = 0.0f);
  void draw_image(const Image&, Rect dst, float opacity = 1.0f);
  void push_clip(Rect | Path); void pop_clip();
  void draw_shadow(...);                                // 盒式/高斯近似
  void blend_from(const Canvas& src, BlendMode, float opacity);
  std::span<const std::uint8_t> pixels() const;
};
}
```
- `Path`：`move_to/line_to/quad_to/cubic_to/close` + 扁平化（`flatten(tolerance)`）。
- `Paint`：纯色 / 线性渐变 / 径向渐变 / 扫掠渐变。
- 抗锯齿：扫描线 + 覆盖率（每像素面积采样，非 MSAA），保证 1px 边框在任意 DPI 下平滑。
覆盖率以**运行段**（`CoverageRun{x0,x1,weight}`）逐行交付，不是逐像素浮点数组（见 §4.2.3）。
- **覆盖率位图有两种通道布局**（`CoverageFormat`）：`Grayscale`（每像素 1 个覆盖率，
  路径遮罩/无头口径）与 `Lcd`（每像素 R/G/B 三个覆盖率，亚像素文字）。
  `Surface::blend_coverage_bitmap` 按它选混合公式——**灰度是单 α、亚像素是逐通道 α**
  （见 §4.3.1），把布局留成调用方的口头约定，后端就只能猜，猜错的表现是「字变糊」或「字变形」。
- `Image`：RGBA 位图 + 缩放（双线性）+ 九宫格绘制。
- SIMD：`simd_*.cpp` 提供 fill/composite/blend 的 SSE2/AVX2/NEON 快路径，标量路径为语义基准（两者须逐像素一致，单测断言）。

### 4.2.1 DPI 与像素密度（一等公民）

**坐标系约定（全框架唯一口径）**：
| 概念 | 单位 | 谁在用 |
|---|---|---|
| **逻辑像素**（logical px） | UI/布局/命中的唯一单位 | `Element` 全部几何、`UiRoot` 视口、控制协议**全部坐标** |
| **物理像素**（physical px） | 真实帧缓冲 | `Canvas` 缓冲、PNG 截图、字形栅格化 |
| `device_scale`（DPR） | 物理 / 逻辑 | `Canvas::device_scale()`、`shell::Backend`、协议 `screen.scale` |

**规则**
1. **`raster::Canvas` 的所有公开绘制 API 收逻辑坐标**（`fill_rect`/`fill_path`/`stroke_path`/`draw_shadow`/`push_clip_*`/`draw_canvas`…），内部按 `device_scale` 换算到物理像素后再光栅化 —— 组件与业务代码**永远不需要手写 `* dpr`**，也就不会漏乘或重乘。
2. `device_scale == 1.0` 时走零开销路径（不做任何换算），行为与无 DPI 完全一致。
3. **几何按物理分辨率光栅化**：路径在扁平化前先 `Path::scaled(scale)`，覆盖率在物理像素空间计算——2x 屏上得到的是**真实两倍采样**，而不是把 1x 位图插值放大（后者会糊）。描边宽度同比例放大，故 1px 发丝线在 2x 屏上占 2 物理像素，视觉等宽且更锐利。
4. **字形按物理尺寸栅格化**：`text::TextRenderer::draw` 取 `canvas.device_scale()`，以 `size × dpr` 生成字形覆盖率位图（可选 `supersample` 超采样后下采样），缓存键含物理尺寸 —— 排版在逻辑单位下稳定（换 DPI 不重排版），像素密度随 DPI 提升。
   桌面模式下再按 §4.3.1 走**亚像素（LCD）**：水平 3 倍子像素采样，缓存键另含渲染模式位。
5. **非整数 DPI 支持**：缓冲尺寸 `round(logical × scale)`（如 1.5x 下 101 逻辑宽 → 152 物理宽），逻辑尺寸由物理尺寸反算，不做累积取整。
6. **运行时切换**：`app.set_scale`（控制通道）/`Application::set_device_scale` 重建帧缓冲 + 清空字形缓存 + 全量标脏；无需重启。
7. **像素访问语义**：`pixel_at/set_pixel/pixels()/content_bounds()` 是**物理**口径（缓冲/编码视角）；逻辑口径用 `pixel_at_point()/content_bounds_logical()`。
8. **截图**：`capture` 输出的是物理分辨率 PNG（`scale=2` 的 1280×800 视口导出 2560×1600），协议回包带 `region` 与 `screen.scale` 便于反查。
9. **两通道同一默认密度**：未显式指定 `scale` 时，窗口后端取**窗口实际 DPI**（多显示器下即所在屏），
   无头后端取**系统显示缩放**（`shell::system_display_scale()`，Windows 查主显示器 DPI/96；其它平台 1.0）——
   内置通道的截图与桌面窗口默认同一像素密度，“截图里看着对不对”与“实机跑起来”不会因 DPI 分家。
   `--scale` / `ST_SCALE` 显式指定时两侧都完全接管（回归与复现）。

**验证**：`tests/raster_dpi_test.cpp` 覆盖逻辑/物理换算、整数与非整数 DPI、对齐边缘无灰边、发丝线物理宽度、字形墨水面积随 DPR² 增长、逻辑布局与 DPI 无关、运行时切换。

### 4.2.2 填充语义（血泪条款）

填充**必须隐式闭合每条子路径**（与 PostScript/SVG/TrueType 一致）。字体轮廓的 charstring 通常
**不含显式 `Close` 且首尾点不重合**，若只按"相邻点连边"构造扫描线边表，缺失的闭合边会让环绕数
永不归零 —— 填充一路向右溢出，字形糊成黑块（CJK 带「口」部首的字形最先暴露）。
`rasterize_polylines` 因此对每条折线补上 `last → first` 的闭合边（首尾重合时跳过）。

配套两条同样重要的语义：
- **覆盖率是带符号量**（非零环绕：顺 +、逆 −，孔洞处累加为 0）。画布侧以 **绝对值** 作为不透明度，
  否则逆时针轮廓整片丢失（圆角矩形、阴影、字形全部中招）。
- **孔洞靠轮廓方向相反实现**：外轮廓与内轮廓方向相反时中间镂空；同向则填实（符合规范）。

回归测试：`tests/raster_fill_test.cpp`（开口三角形补闭合边、嵌套轮廓留孔、逆时针轮廓可见、
CJK 多轮廓字形不糊块、Latin/CJK 带孔字形墨迹占比上限）。

### 4.2.3 覆盖率运行段（性能与正确性同源）

早期实现是“每行一块 `float coverage[宽]`，四个子采样各自把区间内**每个像素**加一遍”。
一个 460×150 的圆角卡片因此要做 ~55 万次浮点累加，而其中绝大多数是在重算“这里就是满覆盖”。
现在改成：

1. **逐行收集运行段**（每个子采样把交叉点成对转成带符号区间，权重 1/4）；
2. **扫描线合并**（事件 + 累加权重，O(n log n)）把重叠段并成互不重叠的段——正负相加即孔洞，
   与逐像素累加的语义**逐位等价**；
3. **按段混合**：满覆盖整段走 SIMD 行混合，只对端点像素按小数比例单独混合。

带来的不只是速度：**旧实现在描边（每个线段四边形 + 顶点圆，方向不一致）上会错误抵消**，
图标会出现“缺了半截弧线”的缺口；运行段合并后图形完整。实测同一帧：

| 绘制项 | 优化前 | 优化后（单层阴影） | 优化后（两层阴影 + 令牌打磨） |
|---|---|---|---|
| 一帧全量重绘（画廊 1280×800） | 63.2 ms | 8.5 ms | **12.0 ms** |
| 圆角卡片填充（460×150 ×27） | 10.12 ms | 0.60 ms | 0.61 ms |
| 描边 ×44（图标/边框） | 7.33 ms | 3.47 ms | 3.53 ms |
| 卡片投影 ×16（两层） | 10.05 ms（×8 单层） | 2.99 ms（×8 单层） | 6.43 ms（16 次调用） |
| 文本 ×62 | 1.05 ms | 0.98 ms | 1.06 ms |
| 清屏（1M 像素，作为机器基准） | 0.19 ms | 0.16 ms | 0.22 ms |

> 最后一列是**有意付出的代价**：多一层环境光让每张卡片多一次合成（一帧 +3.5 ms），
> 换来的是“卡片真的浮起来”而不是“白底 + 一道灰边”（见 §4.2.5）。
> 12 ms 仍然是全量重绘 1280×800 的成本；实际应用只重绘脏区。
> 这个代价试过用 SIMD 挣回来，**失败了**：变 alpha 的向量化合成比逐像素查表慢 1.76 倍
> （同会话交替测量 7.5 vs 13.1 ms）——“α 只有 256 种取值 + 表在 L1”这个先验比向量宽度值钱。

关键手法（按收益排序）：① 覆盖率用运行段而非逐像素数组；② 阴影遮罩按几何参数缓存
（形状+模糊与画布内容无关，同尺寸卡片每帧重算纯属浪费）；③ 逐像素查表代替算术
（遮罩→α→预乘色的 256 项表）；④ `/255` 换乘法-移位（x86 整数除法是几十个周期，而逐像素最多 4 处）；
⑤ 预乘颜色提到循环外；⑥ 描边只对**真的需要补角**的顶点补圆（平滑折线的相邻段夹角只有几度）。

护栏：`tests/raster_bench_test.cpp`（四类典型负载的 ms/项上界，并打印相对于“清屏”的倍数）；
回归：`tests/raster_fill_test.cpp` / `raster_dpi_test.cpp` / `ui_toggle_test.cpp`（单像素级焦点环比对）。

### 4.2.4 视觉令牌的可验证契约（对比度 / 层次 / 阴影分层）

颜色是唯一无法靠代码审查发现的领域：`#94A3B8` 与 `#64748B` 在 diff 里看不出差别，
而前者在白底上只有 2.6:1——11px 的导航标题与版本号直接“看不清”。因此把“好看”拆成
可断言的量（`tests/ui_theme_test.cpp` + `tests/math_color_test.cpp`）：

| 断言 | 口径 | 目的 |
|---|---|---|
| 正文三级文字在 bg/surface/surface_alt 上的对比度 | 正文 4.5:1、辅助小字 4.0:1 | 改调色板越线立刻红灯 |
| 语义色（primary/danger/success）在卡片面上 | ≥ 4.5:1（橙色按大字级 3:1） | “带色文字/图标”可读 |
| `on_primary` 在 `primary` 实底上、`primary` 在 `primary_soft` 上 | ≥ 4.5:1 | 按钮两种变体的文字可读 |
| 控件描边在 `surface` 上 | ≥ 1.35:1（浅色）/ 1.35:1（深色） | 输入框“看得出是个控件” |
| 文字三级的**单调性与间距** | 相邻两级对比度比 ≥ 1.08 | 层次不是名义上的（改色容易把三级挤成一坨） |
| 阴影三档**两层俱在**且单调递增 | 环境层 blur > 关键层、单层不透明度 ≤ 16% | 只有关键层 = 灰边，只有环境层 = 一坨雾 |
| 焦点环合成到各底色后 | ≥ 1.35:1 | 键盘可达性的唯一线索看得见 |
| 尺度令牌成阶梯 | 间距/圆角/字号/控件高度单调 | 否则布局出现“差 1px”的脏边 |

### 4.2.5 阴影：为什么要两层，以及一个颜色字面量的陷阱

单层高斯投影在浅底上会被读成一条“灰边”——真实光照是近处紧、远处散的**叠加**。
`Shadow` 因此提供第二层（`color2/blur2/offset2_y`），`shadow_sm/md/lg` 给出
“关键层 + 环境层”的参数（浅底上总不透明度 ≤ 20%）；`Element::paint_box` 先画环境层再画关键层
（顺序反了会被大层盖住）。遮罩按几何参数缓存，多一层只多一次查表合成（0.37 → 0.55 ms/张）。

同一节记一个**颜色字面量陷阱**（实测让深色主题卡片发光）：
`Color::from_hex` 靠“数值 > 0xFFFFFF”区分 `0xRRGGBB` 与 `0xRRGGBBAA`，
于是 `0x0000008C`（想写“黑 55%”）被读成 `0x00008C` = **纯蓝、不透明**。
对策是在 API 上消除歧义：新增 `Color::from_rgba_hex`（恒读 8 位），
设计令牌统一走它；并用测试把两种函数的差异钉在案（`color_from_hex_ambiguity_is_documented_behavior`）。

### 4.2.6 主题切换会重跑 `apply_theme`

`UiRoot::set_theme` 只标脏、不重跑样式；因此**组件必须在 `measure`/`paint_content` 里
用 `context.theme` 重新取色**（`Tabs`/`Button`/`Input` 均如此）。依赖 `apply_theme` 一次性写入的
字段（如 `Card` 的 `style_.shadow`）在切主题后会保留旧值——排查这种问题的唯一手段是
**把像素剖出来看**（本次就是靠“越靠近卡片蓝色通道越高”定位到颜色字面量误读）。

### 4.2.7 换行布局（`Style::wrap`）与“宽度何时确定”

行容器设 `wrap` 后按可用宽换行：

- **尺寸**：宽度取最宽的一行、高度取各行高之和 + 行间隙（而不是“全部子节点累加”与“最高一行”）；
- **排布**：先分行（每行能装下多少），再逐行放；**行高由行内最高的子元素决定**，
  行内 `Align::Center/End` 相对**行高**对齐（不是相对容器高）。
- **无宽度上界时退化为单行**：没有上界就永远装得下，强行分行会把内容排成每行一个。

这里有一个值得记住的**时序陷阱**：换行结果取决于可用宽，而可用宽在 `measure` 阶段未必已知——
父容器若是行方向，它给子节点的是**无宽度约束**（Flex 的 shrink-to-fit 语义）。
于是 `wrap` 容器会把“宽度未定”误判成“宽度无限”，算出单行高度。
因此 `ScrollView::arrange` 在宽度确定后用**实际内容宽重测一次**内容。
两个现象（只显示一行 / 只显示两行）根因不同，先后踩了两次（缺陷表 42、43）。

### 4.2.8 绘制剖析（默认关闭，不挂即零开销）

`raster::PaintProfiler` 按原语分类（清屏/矩形/圆角/路径/描边/阴影/位图/裁剪遮罩/文本）统计
调用次数、覆盖像素与累计耗时，由 `ST_PAINT_PROFILE=1` 挂到帧缓冲画布上，
经 `Application::paint_profile()` 取用（画廊 `--bench` 直接打印分解）。

存在的理由：**“一帧 30ms”本身不指向任何行动**——是阴影、文字还是渐变，只能靠分解看。
本次优化的每一步都先由它定位再动手（第一批数据直接推翻了我的直觉：以为卡在文字，实际 70% 在阴影与圆角）。

### 4.2.8.1 增量重绘（损坏区机制）

每帧不再无条件清屏 + 全树重绘：元素级变更（`mark_dirty`/动画续帧）把自己的
`bounds + 绘制外扩（paint_margin）`上报到所在树的根；`UiRoot::paint_frame` 帧首
收集后：

- **软件画布**（可直接寻址）：清损坏区 → 推裁剪 → **绘制整树**——`Element::paint`
  的按裁剪域剔除（与视口剔除同一机制）把越界分支 O(1) 跳过，留下的恰好是
  “与损坏区相交的全部元素”，z 序天然正确（单测：局部帧与全量帧**逐像素一致**）；
- **保守回落**：画布不支持局部（GPU，整帧仅 ~1ms）/ 布局重排（会挪动任意兄弟）/
  损坏区为空或超视口 55% → 整帧（语义与旧路径完全一致）。

实测（gallery 2×DPI 软件）：悬停过渡帧 paint **38 ms → 1.86 ms**。可观测：
`metrics.partial_frame` 与 `metrics.dirty_rect`；控制通道路径（`set`/`invoke`/输入）
已从“每次都整帧”收敛到损坏区驱动（元素未标脏时仍回落整帧兜底）。

### 4.2.9 渲染后端：GPU 首选、软件兜底

两条实现、**一个接口**。UI/文本/组件层只认 `raster::Surface`，不知道这一帧是谁画的：

| 实现 | 定位 | 状态 |
|---|---|---|
| `raster::Canvas` | 软件光栅器。**语义真相源**：无 GPU、无显示服务、非支持平台 100% 可用；回归测试以它为准 | 完整 |
| `raster::gpu::GpuCanvas` | D3D11（硬件 → WARP 回退）。默认首选 | 设备层 + 离屏渲染 + 回读已落地；绘制原语在后续里程碑 |

四条设计约束（都是有代价的选择，写下来免得以后被无意破坏）：

1. **接口只放“两边都能做”的原语**。`Surface` 上有 `fill_rect`/`fill_path`/`stroke_path`/`draw_shadow`/
   `blend_coverage_bitmap`/裁剪/像素读回；而“按覆盖率运行段混合一行”这类
   **软件内部机制留在 `Canvas` 上**——把它放到接口上会逼 GPU 实现去模拟 CPU 的数据结构。
2. **文字走接口原语**（`blend_coverage_bitmap`），不直调软件的按行混合 API。
   字形渲染产出的就是覆盖率位图，而这是 CPU 与 GPU 都能做的语义（软件逐行混合、
   GPU 上传成 A8 纹理贴四边形）。若让文字依赖软件内部 API，GPU 路径就没法画字——
   而文字是界面里最常见的原语。
3. **运行时探测，不产生链接期依赖**：GPU 库由平台层 `LoadLibrary` 动态加载
   （`d3d11.dll` / `d3dcompiler_47.dll`），与窗口后端同一口径。没显卡/没驱动的机器
   照常构建与运行（自动落软件），而不是链接期就挂。
4. **无显示服务也能用 GPU**：设备与窗口无关。硬件不行还有 **WARP**（微软的 GPU 指令集
   软件实现）——这不只是兼容手段，更是**唯一能在有显卡的机器上验证“无 GPU 路径”的办法**：
   设 `ST_GPU_FORCE_WARP=1` 重启进程，整条 GPU 代码路径就以“无显卡机器”的形态跑一遍
   （实测适配器会变成 `Microsoft Basic Render Driver`，特性级别 11_1）。

色彩约定是两边最容易错的地方，因此写成测试：画布内部是**预乘** `0xRRGGBBAA`，
而 D3D11 纹理内存序是 R,G,B,A。搞错就是“红蓝互换”或“半透明看起来偏深”，
而截图上只表现为“颜色不对”。`gpu_clear_matches_software_canvas_exactly` 用逐字节断言钉住它。

### 4.2.10 语法高亮与代码编辑器（`st/text/highlight` + `ui::CodeEditor`）

**高亮引擎（`text` 层）**——规则驱动、运行时可注册，零第三方依赖、零正则库：

| 维度 | 说明 |
|---|---|
| 规则即数据 | 一门语言 = 一个 `LanguageSpec`（关键字/类型/内建/定义关键字表 + 注释与字符串定界 + 若干特征开关如 `key_value_keys`/`tag_begin`/`markdown_like`/`diff_like`/`css_like`/`case_insensitive`）。**新增语言不需要写代码**，用户注册的规则与内置规则走同一台扫描器 |
| 内置主流语言 | 30 种规范语言（C/C++/Rust/Go/Python/JS/TS/Java/C#/Kotlin/Swift/Ruby/PHP/Lua/SQL/HTML/XML/CSS/SCSS/JSON/YAML/TOML/INI/Shell/Dockerfile/Makefile/CMake/Markdown/Diff/Protobuf），按**规范名 / 别名 / 文件扩展名 / 常见无扩展名文件名**（`Makefile`/`Dockerfile`/`CMakeLists.txt`）四种方式解析 |
| 注册表 | `LanguageRegistry`：`register_language(spec, replace)`（`replace=true` 可覆盖内置）、`unregister`、`find`（返回 `shared_ptr`，**取到手后即使被注销也依然有效**）。进程级实例 + 可自建实例（测试隔离 / 按工程定制语言集） |
| 扫描语义 | 单趟线性扫描，输出**升序、互不重叠**的 token 区间；跨行状态（块注释/三引号串/多行串）天然正确；未闭合构造吃到文末（编辑器"正在输入"的常态）；非法 UTF-8 只按普通字节处理，不截断区间 |
| 词法类别（16） | plain/keyword/type/string/number/comment/function/operator/punctuation/preprocessor/builtin/attribute/key/tag/inserted/deleted（后四类服务标记语言、配置格式与 Diff） |
| 性能 | 实测 1000 行 C++ ≈ 1.7 ms、8000 行 ≈ 13 ms（纯词法，单趟） |

**代码编辑器组件（`ui::CodeEditor`）**——面向"文件编辑器"场景，不折行 + 垂直/水平滚动 + 固定行号槽：

- **编辑**：插入/删除、UTF-8 光标移动（Ctrl 按词）、Home/End（Ctrl 到文首/文末）、
  跨行选择（鼠标拖选 / 双击选词 / 三击选行 / Shift+方向键）、全选、复制剪切粘贴、
  撤销重做（按键合并 600ms 窗口，深度上限 256）、Tab 缩进/Shift+Tab 反缩进、
  **Ctrl+/ 注释切换**（用该语言的行注释标记，无标记的语言返回 false）、
  **回车自动缩进**（`{`/`(`/`[`/`:` 后自动 +1 级；`{|}` 处回车展开为三行）、括号配对高亮
- **只读模式**：`set_read_only(true)` 即"带高亮的代码查看器"（选择与复制仍可用）——
  示例 `codeeditor` 左侧可写、右侧只读，同一组件两种形态
- **语言**：`set_language(name)`（名字/别名/扩展名）、`set_language_from_path(path)`、
  `set_language_spec(spec)`（直接绑定自定义规则）
- **控制通道**：属性面 `text/language/cursor/line/column/lines/selection/selected_text/read_only/highlight/show_line_numbers/tab_width/font_size/goto_line`；
  动作 `focus/select_all/undo/redo/clear_selection/insert/copy/cut/paste/comment/indent/dedent/goto_line/set_text`
- **语义值**：`semantics_value()` 给**当前行内容**（整篇代码塞进语义树既无意义也会撑爆控制通道响应）

**剪贴板**：使用进程内剪贴板而非系统剪贴板——无头模式没有系统剪贴板（服务器无 X/Wayland），
而编辑器必须具备可用的复制/粘贴语义。进程内剪贴板在无头与有窗口下行为**完全一致**，
也能被控制通道的 `invoke(copy/cut/paste)` 驱动（智能体可据此搬运文本）。

**渲染缓存分两层**：行索引（`line_spans_`，随文本变，一趟扫描）与高亮 token（`line_tokens_`，
还要看语言规则）。分开后"取行号/算列"这类高频操作不会触发高亮重算——否则批量编辑会退化成 O(n²)。

### 4.3 text
```cpp
namespace st::text {
class FontFace {                                  // 单个字体文件/face
  static Result<FontFace> load(std::string_view path, int face_index = 0);
  GlyphId glyph_index(char32_t cp) const;
  Result<Glyph> glyph(GlyphId) const;              // 轮廓 + 度量（缓存）
  Metrics metrics() const;  // units_per_em, ascender, descender, line_gap
};
class FontStack {                                  // 回退链（拉丁 → CJK → 符号）
  FontStack(std::vector<FontFace> faces);
  const Glyph* glyph_for(char32_t cp) const;       // 首个命中
  ShapedText shape(std::string_view utf8, float size, ...) const;
};
class TextRenderer {                               // 字形 → 位图缓存（LRU）
  TextRenderer(const FontStack&, int dpi_scale);
  TextMetrics measure(std::string_view text, float size) const;
  void draw(Canvas&, TextLayout, Point, Color) const;
};
}
```
- 支持 **TTF（glyf/loca/cmap/hmtx/kern/GPOS 子集）** 与 **OTF/OTC（CFF、CID-keyed、Type2 charstring、FDArray/FDSelect）**，即 Linux 服务器上的 `NotoSansCJK-Regular.ttc` 可直接用。
- 缓存：字形轮廓、字形位图、整形结果（按 文本+字号+role+字体栈 键）。
  字形与整形缓存均为**真 LRU + 内存预算**（字形 24 MiB、整形 16 MiB，超预算从
  尾部增量淘汰）——旧实现超 512 条目全清，CJK 大文档滚动时会有周期性重栅格化尖峰
  （由 2026-09-30 审视定位并修复）。
- 回退链默认：`ST_FONT_LATIN` / `ST_FONT_CJK` / 系统探测（`/usr/share/fonts`、`C:\Windows\Fonts`、`/System/Library/Fonts`）。

### 4.3.1 文字抗锯齿：灰度 vs 亚像素（LCD）

**起因**：屏幕上 125% DPI、13.5px 正文「看着就是糊的」，而 Chrome/VSCode 的字看着锐。
把两台机器的文字边缘逐像素取样后，差异很清楚：

```
Chrome 边缘：  背景(24,24,29) → 蓝(24,24,133) → 亮(172,205,211)   ← 1 像素陡变 + RGB 彩边
霜天 边缘：    背景(38,53,82) → (43,57,86) → (110,121,146) → 亮   ← 缓坡、纯灰
```

前者是 **ClearType 类亚像素渲染**（把显示器像素的 R/G/B 三层分别当采样点用，
水平有效分辨率 ×3），后者是灰度抗锯齿。人眼的参照系是前者，于是后者显糊。

**实现（`TextRenderer::set_subpixel`）**
1. 字形轮廓按 **x × 3** 光栅化到 `3·supersample 列 × supersample 行` 的 scratch
   （每个物理像素横跨三个子像素），逐子像素聚合成覆盖率 → 每像素 R/G/B 三个值
   （`CoverageFormat::Lcd`，等价于 FreeType 的 `FT_RENDER_MODE_LCD`）。
2. **位图网格与灰度模式逐像素重合**：LCD 的包围盒由灰度口径 **×3 推导**，
   于是 `width/height/offset_x/offset_y` 两种模式完全相同——开/关只改边缘合成方式，
   **不挪字**（`tests/text_subpixel_test.cpp` 钉住这一点）。
3. 可选 5-tap 低通滤波（FreeType `FT_LCD_FILTER_DEFAULT` 权重 `{8,77,86,77,8}/256`，
   在**子像素轴**上滤波、边界夹取），压彩边。默认开；`ST_TEXT_LCD_FILTER=0` 关闭。
4. 缓存键含**渲染模式位**（灰度与亚像素的排布不同：1 项/像素 vs 3 项/像素，
   混用等于按错误长度解读）。

**合成公式（软件与 GPU 必须同式）**：`out_c = S_c·α_c + D_c·(1 - a_s·α_c)`
（`S` 预乘源色、`a_s` 源 alpha、`α_c` 该通道覆盖率）。关键是**目标衰减也逐通道**：
黑字压白底时 `S_c = 0`，彩边全在 `D_c·(1-α_c)` 那一项上，用标量 α 就等于什么都没做。
- 软件（`Canvas::blend_coverage_row_subpixel`）：逐像素按三通道 α 混合；
  非 `SrcOver` 模式（Multiply/Overlay…）没有逐通道语义，**如实退化**为三通道均值。
- GPU（D3D11）：硬件混合的 α 是标量，做不到逐通道衰减，因此**分两遍**——
  ① `SrcBlend=ZERO / DestBlend=INV_SRC_COLOR`（逐通道衰减目标，α 通道不动），
  ② `SrcBlend=ONE / DestBlend=ONE`（加性加回 `S_c·α_c`）。覆盖率上传成 `R8G8B8A8` 纹理
  一次采样拿三通道；`blend_coverage_bitmap` 的纹理缓存另一并带上“是否三通道”标志。
  能力上报多一条 `gpu::Capabilities::lcd_text`（**不支持的后端必须如实报 false**，
  上层据此退回灰度，而不是悄悄画成灰度却声称支持）。

**开关与口径**

| 场景 | 形态 | 理由 |
|---|---|---|
| 所有场景（默认） | **LCD 亚像素 + 网格拟合 normal** | **内置通道（无头）与桌面窗口同源**：截图所见与实际运行一致是开发闭环的验收前提 |
| 可断言基准 | `--text-lcd=off --text-fit=off`（灰度 + 无拟合） | 像素对像素回归对比（跨版本基线）用显式关闭取得 |
| 显式指定 | `--text-lcd auto\|on\|off`、`ST_TEXT_LCD=1\|0`；`--text-fit auto\|off\|light\|normal`、`ST_TEXT_FIT` | 对照实验与降级路径 |

运行期可查：启动日志一行「文字渲染：…」、控制协议 `metrics.text_renderer`（`lcd`/`grayscale`）
与 `metrics.text_fit`。

**诚实的实测结论（本机 Linux + 软件光栅器，110 个字形的统计）**

| 指标 | 灰度 | LCD（滤波） | LCD（不滤波） |
|---|---|---|---|
| 墨量（三通道和 ÷ 3） | 基准 | **+1.4%**（逐字形最坏 +6.8%） | 同左 |
| 子像素亮度 profile 平均差 | — | 0.016 | 0.02 |
| 亮度最大斜率（相对灰度） | 1.00 | 0.87~1.00 | **1.11** |
| 边缘 10→90% 过渡宽度 | 基准 | 1.00×（不变） | 1.00× |
| 彩边能量 | 0 | 基准 | +9%（滤波把它压掉 8.3%） |

也就是说：**亚像素换来的是「1/3 像素的边缘定位精度 + 彩边」，不是「过渡带变窄」**
——逐样本统计（中间调占比、过渡带长度、亮度 profile）与灰度基本一致（±2% 以内），
那 +1.4% 的墨量差来自水平光栅化分辨率不同（曲线扁平化容差在 3 倍细网格上更精细）。
**13.5px 小字「发糊」的主因另有其人：笔画未对齐像素网格（无 hinting / 网格拟合）**
——这条有**可量化的证据**（`tools/stem_phase_probe.cpp`；13.5 逻辑 px @1.25 DPI
= 物理 16.88px；统计 184 条竖笔画的左右边缘）：

| 判据 | 现状 | 边缘吸附到整数网格（估计上限） |
|---|---|---|
| 竖笔画边缘**落在整数网格**（过渡带 0 个像素） | **0.0%** | 100% |
| 每个边缘留 1 个过渡像素（缓坡） | **91.8%** | 0% |
| 左边缘落在最糊相位（`|frac-0.5| < 0.3`） | **55.4%** | — |

即**当前没有一条竖笔画的边缘落在整数网格上**，一半以上正好落在像素正中间；
一个 1.2~1.7px 宽的笔画因此必然摊成两端各一个过渡像素的缓坡。这与抗锯齿模式**无关**
（灰度/亚像素都过不了这一关）——它由 **§4.3.2 网格拟合**解决，与本条互不替代。

### 4.3.2 字形网格拟合（grid fitting / hinting）

**先量后做**（`tools/hinting_gain_probe.cpp`，判据 = 竖笔画边缘落在整数网格的比例，

| 路径 | 拉丁（TrueType，DejaVu） | 中文（CFF，Noto CJK） |
|---|---|---|
| 现状（无 hinting） | 3.2% | 5.2% |
| `FT_LOAD_TARGET_LIGHT` / `NORMAL`（读字体自带指令） | 3.2% | 4.3% |
| **auto-hinter（normal）** | **49.0%** | **20.6%** |
| `TARGET_MONO`（强制整数网格） | 97.0% | 4.3% |

**三条决定性结论**（改变了实现方向，省下几千行无用功）：
1. **读字体自带 hinting 指令 ≈ 什么都没做**（拉丁 3.2%→3.2%）；
2. **中文界面字体是 CFF**（只有 stem hint，没有 TT 那套指令），`TARGET_MONO` 对它无效
   ——**给中文实现 TrueType 指令解释器（`fpgm`/`prep`/`glyf`）是白工**；
3. 唯一有效的是 **auto-hinter 式的几何网格拟合**：不看字体指令，从轮廓几何自推笔画位置。

**实现**（`include/st/text/grid_fit.hpp` + `src/text/grid_fit.cpp`）：
1. **抽笔画**：轮廓里近垂直/近水平的**直线边**两两配对成窄条（宽度 ≤ `max_stem_width`，
   span 重叠），按 span 重叠归组；
2. **两侧各自吸附**到最近的整数（不对称处理）：只把整条笔画平移的话，另一侧的边缘仍留在
   分数相位上——那只解决一半问题；
3. **曲线控制点按参数权重跟随两端**（二次 1:3 / 三次 1:4 的 Bernstein 系数）：
   只挪端点会让曲线走样，那比不拟合更糟；
4. **护栏**：单边位移 > `max_shift`（0.5px）拒绝该边；整字平均位移（**按全部点摊销**）
   > `max_mean_shift` 时整体放弃；一点都没动就如实报 `applied=false`。

**实测收益**（`tools/stem_phase_probe.cpp`，同一把尺子量拟合前后；13.5px @1.25 DPI）：

| 判据 | 现状 | 拟合后（Normal） |
|---|---|---|
| 中文：每边一个过渡像素（最糊） | **90.9%** | **21.8%** |
| 中文：锐笔画（0 个过渡像素） | 0% | **54.7%** |
| 拉丁：每边一个过渡像素 | 87.3% | 44.0% |
| 边缘落在整数网格（拉丁 / 中文） | 1.3% / 7.5% | **41~51% / 57.8%** |
| 中间调像素占比（13.5px 中文） | 71.1% | **49.5%** |
| 墨量变化（聚合） | — | **−0.3%~+2.5%**（字号越大越接近恒等） |

中文的收益（20.6% → 57.8%）是 **FreeType auto-hinter 基准的 2.8 倍**；
拉丁（50.9%）与基准（49.0%）持平。**字宽与位图网格逐字段不变**（拟合只动轮廓点、
不动 `hmtx`，且包围盒在拟合**之前**算好）——这条有测试钉死
（`tests/text_grid_fit_test.cpp::grid_fit_bitmap_grid_matches_unfitted`）。

**踩过的两个真回归**（都由测试/截图抓回，记在这里防复发）：
1. **亚像素路径不能用 `Path::scaled(3)` 做水平放大**——它会把 x 与 y **一起**乘 3，
   字形被纵向拉成 3 倍高、只有上半部分落在画布里，表现为「文字像被切成两半」。
   必须用 `build_path(horizontal=3)` 重新构一次（只乘 x），再把拟合位移的 x 分量 ×3 叠上去。
2. **包围盒必须在拟合之前算**：否则拟合把墨迹推到新的整数位置后 `floor`/`ceil` 差 1px，
   位图尺寸随开关跳变 ——「开一下同一个字就挪了半像素」。

**开关**：`--text-fit auto|off|light|normal`（三个示例 + 通用命令行）、`ST_TEXT_FIT`；
默认 **`normal`（所有场景，与亚像素同一口径：内置通道与桌面同源；需要不改字形边沿的
可断言基准时显式传 `off`）**。启动日志与协议 `metrics.text_fit` 可查。

### 4.4 md
```cpp
namespace st::md {
struct Block { BlockKind kind; std::vector<Inline> inlines; std::vector<Block> children; ... };
std::vector<Block> parse(std::string_view markdown);
class MdStream {                       // 流式增量（LLM 边生成边渲染）
 public:
  std::vector<Block> feed(std::string_view chunk);   // 返回当前完整块序列（最后一块可为未闭合）
  std::vector<Block> snapshot() const;
  void reset();
};
std::vector<HighlightSpan> highlight(std::string_view code, std::string_view lang);
}
```
- CommonMark 子集 + GFM：表格、任务列表、删除线、自动链接、围栏代码块（带语言）、嵌套列表、引用、分隔线。
- 高亮：零依赖词法着色（c/cpp/ts/js/python/json/bash/rust/go/yaml，关键词 + 字符串 + 数字 + 注释 + 函数名）。

### 4.4.1 app：通用命令行与启动契约

`st/app/app.hpp` 提供 `Application`，`st/app/cli.hpp` 提供**所有应用共用的命令行解析**
（`--headless` / `--backend` / `--width/--height` / `--scale` / `--title` / `--theme` /
`--control-port` / `--control-file` / `--enable-script` / `--frames` / `--ms`）。

放进框架而不是让每个应用自己写，是因为**控制通道是"应用可被驱动"的入口，而它的端口与控制文件
是命令行给的**：应用漏解析这两个参数，外部工具就只能去猜端口、等不到控制通道就绪
（生成的工程模板踩过这个坑，`--control-port 0 --control-file X` 被静默忽略）。

启动契约（推荐写法）：

```cpp
st::app::Application app("myapp", "0.1.0", common.app);
build_page(app.root());                 // ① 先建界面
app.on_ready([&] { load_scripts(app); });// ② 再登记"启动后"初始化（脚本宿主在 start() 里才创建）
return *app.run();                       // ③ 无参 run()：用已设的根组件
```

- `run()` 与 `run(content)` 的区别是**实质性的**：后者会先 `set_content(...)`，
  因此 `run(nullptr)` 会把先前设好的界面**清掉**（踩过：建完界面再 `run(nullptr)`，界面是空的）。
- `AppOptions::headless` **真的参与后端选择**（`headless=true` 等价于指定 headless 后端）；
  它原先只在"后端为空时报告用"，是个静默无效的开关。
- 后端自动选择失败时**回退 headless 而不是报错**：探测到 `libX11` 但没有可用显示服务的机器上，
  能无头跑起来（控制通道完成开发与验证）远比"启动失败"有用。

### 4.5 ui

> **脚本驱动的组件控制**见 §6.7（`ui::ScriptHost`）：JS 读写组件、事件桥、定时器，
> 与协议/C++ 共用同一套读写语义；跨语言边界采用「快照批量 + 变更集提交」。

```cpp
namespace st::ui {
class Element {                                  // 组件基类
 public:
  virtual ~Element() = default;
  ElementId id() const;                          // 稳定 id（未显式指定则按路径生成）
  virtual std::string_view type() const = 0;     // 组件类型名（tree 输出）
  virtual Size measure(const Constraints&) = 0;
  virtual void arrange(Rect final_rect) = 0;
  virtual void paint(const RenderContext&, raster::Surface&) const = 0;  // 立即模式直绘（Surface 为软件/GPU 共同抽象；保留模式 DisplayList 为 v0.3+ 演进项，见 docs/BACKLOG.md）
  virtual bool on_event(const Event&);           // 命中后的事件处理
  virtual void collect_semantics(SemanticsNode&) const;   // 语义树（tree/find/无障碍）
  ...
};
class UiRoot {                                   // 树根：布局 → 绘制 → 事件分发 → 脏区
 public:
  void set_child(std::unique_ptr<Element>);
  void layout(Size viewport);
  void paint(raster::Canvas&);
  bool dispatch(const input::Event&);            // 命中测试 + 捕获/冒泡
  Element* find(ElementId); std::vector<Element*> query(const Selector&);
  Theme& theme();                                 // 主题切换触发全树重绘
};
}
```
组件库（`include/st/ui/components/*.hpp`）——**已实现 32 个**：`Text` `Icon` `Button` `Input` `TextArea` `Checkbox` `Radio` `Switch` `Slider` `Select` `Tabs` `Table` `List` `ScrollView` `ScrollBar` `ProgressBar` `Spinner` `Badge` `Avatar` `Chip` `Card` `Panel` `Divider` `Dialog` `Toast` `Tooltip` `CodeEditor` `MarkdownView` `Tree` `MenuBar` `MenuPanel` `ContextMenu` `FileDialog`。
**规划中 5 个**（勿在文档外引用，待实现后移入上行）：`IconButton` `Link` `Dropdown` `SegmentedControl` `Sparkline`。
- 布局：自研 flex 子集（`direction`/`gap`/`padding`/`margin`/`grow`/`shrink`/`align`/`justify`/`wrap`/百分比/固定尺寸/自适应内容）。
- 样式：`Style` 结构体 + `Theme`（token 表）；状态 `:hover`/`:active`/`:focus`/`:disabled`/`:selected` 由组件按 token 插值。
- 图标：自绘矢量路径集（`IconName` + 路径数据），零位图资源、任意缩放清晰。

**v0.1.5 补全的组件能力**（画廊全场景覆盖反推）：
- `Toast::set_auto_dismiss_ms`：自动消失（帧时间轴驱动，`expired()` 可查、`on_dismiss` 回调、属性面
  `auto_dismiss_ms`/`expired` 可读写）；到期那帧**连阴影都不落盘**（推演前置在 `paint()`）。默认 `0`=常驻。
- `Table::set_selected_row/selected_row`：选中行（`primary_soft` 底 + 主色左缘条 + 主色文字，压过斑马纹/hover）；
  属性面 `selected_row`（`-1` 清除、越界拒绝）；语义值 `8x5 sel=2` 可断言；`clear_rows` 一并清选中。
- `TextArea` 属性面补齐（`value/text/placeholder/cursor_index/scroll_offset` + 动作 `clear/submit`）——
  此前未实现，`set value` 被静默忽略（与 `Input` 踩过的坑同类）。
- 叠加层 z 序修正：`paint`/`paint_frame` 中 overlay 改在内容**之后**绘制（浮层在景上）；
  `UiRoot::find/query` 同步覆盖叠加层（对话框/轻提示/下拉面板此前从选择器里消失）。

**vsedit 反馈驱动的编辑器形态能力**（类编辑器应用反推）：
- **事件派发契约**：`on_event` 返回 `true` = 消费（冒泡停止）、`false` = 放行（继续冒泡到
  父级；到达根后进入全局语义）。组件只对**认识的键**返回 true——未识别组合键必须放行
  （否则全局快捷键没有落点），**只读/无操作的分支同样必须放行**（返回 true 却什么都不做
  等于把键吃掉：只读编辑器曾因此把 Tab 吞下，焦点被永久扣住、Tab/Shift+Tab 均无响应）。
  「键自含」如编辑器的 Tab 缩进，也由这同一个返回值表达（可编辑 → true → 焦点不动）；
  不另立平行的声明式接口（那会与真值漂移，且无调用方）。
- **全局快捷键**：`UiRoot::register_shortcut(key, {ctrl,shift,alt,meta}, handler)`；
  KeyDown 派发顺序固定为**快捷键表 → 浮层 → 焦点元素 → Tab 焦点环**（后注册优先；
  handler 返回 false 放弃消费继续下沉）。
- **命中拦截**：`Element::intercepts_input()`（默认 true）——不可见/不拦截的浮层不再
  截住下层内容（`Dialog` 覆写为 `visible()`）。
- **行为注入**：`Element::set_event_handler(fn)`（组件实现之后、冒泡之前调用；免子类化
  的小交互，如拖拽把手改宽度）。
- **叠加层排布形态**：`add_overlay(el, OverlayLayout::Stack|FillViewport)`——
  FillViewport 时 overlay 分到全视口矩形自行定位卡片（模态遮罩/命令面板标准形态；
  `Dialog` 免 `set_viewport_rect`，`Toast` 底部居中且不遮内容；Stack 保持历史堆叠行为）。
- **Tabs 编辑器化**：`Tab{key,label,modified,closable}` + `sync_tabs`（key 复用、活动态跟
  key 走）+ `on_close`（点 × 触发，不删标签）+ 溢出滚动（箭头/滚轮/夹取，
  `scroll_offset()` 可读写，active 项自动滚回可见）。
- **文本编辑类默认可聚焦**：`Input`/`TextArea`/`CodeEditor` 构造即 `set_focusable(true)`——
  点击聚焦只认 `focusable()`，而键盘激活（`activate()`）又要求先有焦点，默认 `false` 是
  死循环（点击永远聚焦不了编辑器）；宿主不再需要 `set_focusable(true)` 的集成 workaround。
- **焦点语义严格化**：`UiRoot::set_focus` 按 `focusable()` 裁决并返回 `bool`（不可聚焦即拒绝），
  消除「焦点在它、Tab 环跳过它」的状态分裂；`input.text`/`app.focus` 遇到不可聚焦目标
  直接报错（不再默默把文本送给旧焦点元素）。
- **Tree**：扁平可见行数组 + `sync_nodes`（key 复用、选中态跟 key）+ `on_toggle(key,
  expanded)` 懒加载（目录展开时才 list_dir）+ ↑↓/Enter/←→ 键盘导航。
- **MenuBar / ContextMenu**：声明式 `Menu{id,label,items[]}`；`make_panel(i)` 锚定标题
  正下方（Stack overlay 不锚定，面板自己落到 anchor）；ContextMenu 挂 FillViewport、
  dismiss barrier（面板外点击关闭并消费，MouseMove 照常穿透）、越界翻转/夹入视口。
- **FileDialog**：打开/保存（目录导航 + 自绘文件列表 + 自绘文件名输入行 +
  `on_confirm(full_path)`/`on_cancel`；fs 失败呈现错误行不崩溃）。
- **工具链（stpm）**：**Windows 默认 g++（MinGW-w64，主版本 ≥ 13）**，MSVC 可回退——
  两族的 PCH 均由 `pch_consume_args` 按族分派（MSVC `/Yu+/FI+/Fp`；GCC `-I+-include`）；
  MSVC PCH 创建入口用 `.cpp` 包装 + **产物存在性核对**（`cl` 对 `.hpp` 主文件是静默跳过、
  退出码仍为 0，只看退出码会把“没生成”报成“已生成”）；`-Werror→/WX` 附带 `/w35105`
  （旧 Windows SDK 系统头 C5105 已知误报降级）。
- **控制通道**：`capture` 落盘白名单默认含**控制文件所在目录**（智能体会话目录直落
  截图，免二次搬运）。

### 4.5.1 交互元素与元素身份（每条都由实际缺陷换来）

| 约定 | 为什么 |
|---|---|
| **动作一律放在 `virtual void activate()`** | 它是所有激活路径的唯一汇聚点：鼠标点击（`on_event`）、协议 `invoke(click)`（`invoke_action`）、脚本 `ui_invoke`。写在 `on_event` 里 → 真实点击有效、`invoke(click)` 静默无效（列表项踩过），而自动化只能走 `invoke` |
| 容器要提供**成对的**增删接口 | 只能追加不能清空 → 过滤/按新数据刷新无法实现（`List::clear_items` 由此而来） |
| 自动生成的 id 必须**稳定、唯一、无控制字符** | 选择器、协议消费方、脚本层都拿它当键。生成时用**拥有型**容器拼装（`vector<string>` 而非 `vector<string_view>`——后者会指向临时字符串，拼出垃圾字节与 NUL） |
| 刷新数据用 `sync_items` 而非 `clear_items` + 逐个 `add_item` | 前者按 `key` 复用子元素，**id 与选中态都保持**；后者索引推倒重来，外部按 id 引用会错位、选中态静默丢失 |
| `UiRoot` 的 `focused_`/`hovered_`/`pressed_` 必须**用前校验** | 它们是裸指针，而子树会被重建/替换。元件从树上摘下时无法通知到 root（`Element` 没有 root 反指），所以每次使用前做"仍不在树上"的检查——**只比较指针、不解引用**（指向已销毁元素时解引用即 UB）。校验点：`dispatch()`、`update_hover()`、`set_focus()`、`focused()` |
| 组件不得声明与 `Element` 保护成员**同名**的成员 | 遮蔽（shadowing）**编译零警告**，且症状静默：焦点写基类、读遮蔽副本时功能失效而测试全绿（`CodeEditor` 自带 `bool focused_{false}` → 光标永不绘制、括号高亮失效；而直接调 `set_focused` 的组件级单测读写落在同一侧，全体通过）。有独立语义就**改名**（`KeyValueRow` 的显示标签 `key_` → `label_`：基类 `key_` 是稳定逻辑身份、参与自动 id，不是显示文案）。守规则：lint `L13` |
| `semantics_flags()` 覆写必须以 `Element::semantics_flags()` 起手 | 用 `SemanticsFlags flags{}` 重建会丢掉 visible/enabled/focused/hovered/pressed → 焦点经 `UiRoot::set_focus` 设置时，语义树与 `:focused` 选择器恒报 false（控制通道看到的元素状态与真实不符，`tree`/`:focused` 全不可信）。守规则：lint `L13` |
| 焦点只能经 `UiRoot::set_focus`，且它按 `focusable()` **严格裁决** | `focusable()` 是「能否持有焦点」的契约，Tab 焦点环按它筛选；无条件赋值 →「root 焦点指向它、键盘派发给它、Tab 环跳过它」的状态分裂。不可聚焦即拒绝并返回 `false`，调用方如实上报（`input.text`/`app.focus` 目标不可聚焦直接报错——此前会静默不聚焦、**把文本送给旧焦点元素**，写错元素比报错危险） |
| 文本编辑类构造即 `set_focusable(true)` | 点击聚焦路径只认 `focusable()`（`hit_test → focusable() && set_focus`），而 `activate()`（Enter/Space）又要求先有焦点——默认 `false` 是个死循环：**点击永远聚焦不了编辑器**（`Input`/`TextArea` 早已如此，`CodeEditor` 补上） |
| 自绘组件的 `paint_content` 画布是**视口绝对坐标** | 必须以 `bounds_.x/y` 为原点偏移；按局部坐标画会整块位移（实测：自绘树上移 70px 压住标题、标签画进顶栏）。基线接口（`paint_box`/`paint_text`/子节点）已按 `bounds_` 落位，只有自己的几何需手动偏移（长期选项：绘制前自动 translate+clip 改为局部坐标，需全组件迁移，单独立项） |

#### 4.5.1.1 元素身份：`id` 与 `key` 的分工

| | 显式 `set_id` | `set_key`（推荐） |
|---|---|---|
| 表达 | 这个元素**叫什么名字** | 这个元素在**兄弟之间是谁** |
| 层级 | 完整 id，自己拼路径 | 只给身份，路径/去重/选择器安全由框架管 |
| 重排后 | 只要没改动就稳定 | 稳定（与位置无关） |

自动 id 的片段规则：有 `key` → `Type@<sanitized-key>`（如 `tasks/ListItem@task-42`）；
无 `key` → `Type[index]`。`key` 会被转义成选择器安全字符（非 `[A-Za-z0-9_-]` → `-`），
因为 id 的本职就是**被外部引用**（`#tasks/ListItem@task-42`），而被选择器切词用的
`#`/`.`/`:`/`[`/空白 出现在 id 里就再也定位不到。

`key` 是**业务身份**（任务 id、行主键、配置项名），不是显示文案：文案变了 id 不变，
这正是"按 id 引用"能跨刷新存活的前提。兄弟间 `key` 重复会在插入时告警（否则自动 id 撞车，
按 id 查找可能命中另一个元素）。

```cpp
list.sync_items({{.key = task.id, .label = task.title}});   // 同 key 的项沿用同一元素与同一 id
```

### 4.6 shell
```cpp
namespace st::shell {
struct WindowOptions { int width, height; float scale; std::string title; bool headless; bool resizable; };
class Backend {                                   // 纯抽象
 public:
  virtual ~Backend() = default;
  virtual std::string_view name() const = 0;      // "headless"/"x11"/"wayland"/"win32"
  virtual Result<void> create_window(const WindowOptions&) = 0;
  virtual void present(const raster::Canvas&) = 0;      // 提交一帧
  virtual std::optional<input::Event> poll() = 0;       // 输入事件（无则空）
  virtual void set_title(std::string_view) = 0;
  virtual Result<void> set_clipboard_text(std::string_view);
  virtual Result<std::string> clipboard_text();
};
Result<std::unique_ptr<Backend>> create_backend(std::string_view name);   // 自动选择：非 headless 时逐个尝试
}
```
- `headless`：**默认在无 DISPLAY/WAYLAND_DISPLAY 时自动选择**；离屏 Framebuffer，输入事件由控制通道注入。
- 平台后端：运行时探测（`dlopen`/`LoadLibrary`），不产生链接期依赖；缺失或未实现时如实报 `Unsupported`。

**实现状态**：`headless` 与 **`win32` 已实现**；`x11`/`wayland` 目前只做探测与明确的 `Unsupported`
（UI 层与软件光栅器与平台无关，补后端是纯粹的窗口层工作量）。

Win32 后端的实现要点（**全部来自实际运行，不是读代码能看出来的**）：

| 点 | 教训 |
|---|---|
| 像素搬运要**重排通道** | 画布内部是 `0xRRGGBBAA`（预乘），GDI 的 32bpp DIB 是内存序 B,G,R,A——不重排就红蓝互换（表现为"截图色调不对"） |
| 事件序列要**发全** | 按钮激活在 `UiRoot::dispatch` 的 `Click` 分支；只发 Down/Up 会让按钮"有焦点、有按压效果，**点了没反应**"。对齐协议 `input.mouse{kind:"click"}` 的 Down→Up→**Click**；双击补计数为 2 的 Click |
| **DPI 感知必须声明** | 不声明 `SetProcessDpiAwarenessContext` 的话，系统会把我们的位图**再拉伸一次**（模糊 + 逻辑坐标与实际位图错位） |
| 缩放以**窗口实际 DPI** 为准 | 多显示器下 DPI 可能不同（`WM_DPICHANGED` 后重算） |
| 窗口尺寸归**应用**管 | `WM_SIZE` 只重建帧缓冲；应用在 `tick()` 里比对 `logical_size()` 同步视口——否则拖大窗口只看到左上角旧区域 |
| 关窗走**收尾路径** | `WM_CLOSE` 置 `close_requested()`，由应用主循环退出（进程内还有控制通道/脚本宿主要正常停止，不能直接 `exit`） |

验证（平台后端**必须实际跑窗口**才算验证）：`python3 tools/st_win_check.py` —— wine + Xvfb 下启动
真实 exe，断言后端/视口，**真实鼠标点击**、**真实键盘输入**、窗口缩放跟随、优雅退出，并留截图。

### 4.7 gpu
```cpp
namespace st::gpu {
struct Capabilities { std::string api; std::string device; bool available; };
Result<Capabilities> probe();                       // dlopen 探测（vulkan → gl → 无）
class Compositor {                                  // UI 图层 → GPU 合成
 public:
  static Result<std::unique_ptr<Compositor>> create();
  virtual void submit(const raster::Canvas&, ...) = 0;
};
}
```
本期定位：**可选合成加速**。软件路径永远是语义基准；`metrics.backend` 如实上报 `software` / `vulkan` / `gl`。

### 4.8 control（TCP 控制通道）
见 §6 协议规范。服务端在 `App` 启动时按 `--control-port`（0 = 自动分配并写入 `--control-file`）监听；只监听回环地址（`127.0.0.1`）由默认策略保证安全（`--control-bind` 可改，需显式）。

### 4.9 pkg（stpm）
见 §7。

## 5. 设计系统（token）

**品牌意象**：霜天——冷冽清晨、冰蓝与霜白。中性色偏冷，品牌色取**冰蓝**，辅以**青色**点缀；深色模式为「极夜霜原」。

| 类别 | token | 亮色 | 暗色 |
|---|---|---|---|
| 背景 | `bg` | `#F6F8FC` | `#0A0F1A` |
| 表面 | `surface` | `#FFFFFF` | `#121A2B` |
| 表面次 | `surface_alt` | `#EEF2F9` | `#1A2438` |
| 边框 | `border` | `#DDE4EF` | `#26324A` |
| 边框强 | `border_strong` | `#C3CEDF` | `#33425F` |
| 文本 | `text` | `#0F172A` | `#E8EEF9` |
| 次要文本 | `text_muted` | `#64748B` | `#94A3BD` |
| 弱文本 | `text_faint` | `#94A3B8` | `#64748B` |
| 主色 | `primary` | `#2563EB` | `#4C8DFF` |
| 主色 hover | `primary_hover` | `#1D4ED8` | `#6BA1FF` |
| 主色弱底 | `primary_soft` | `#E4ECFE` | `#16233D` |
| 强调 | `accent` | `#0891B2` | `#22D3EE` |
| 成功 | `success` | `#059669` | `#34D399` |
| 警告 | `warning` | `#D97706` | `#FBBF24` |
| 危险 | `danger` | `#DC2626` | `#F87171` |
| 焦点环 | `focus_ring` | `#2563EB66` | `#4C8DFF66` |

| 类别 | token | 值 |
|---|---|---|
| 间距 | `space.xs/sm/md/lg/xl/2xl` | 4 / 8 / 12 / 16 / 24 / 32 |
| 圆角 | `radius.sm/md/lg/xl/pill` | 6 / 10 / 14 / 20 / 999 |
| 字号 | `font.xs/sm/base/lg/xl/2xl/3xl` | 12 / 13 / 14 / 16 / 20 / 26 / 34 |
| 行高 | 倍数 | 1.45（正文）/ 1.25（标题）/ 1.6（Markdown 段落） |
| 字重 | `regular/medium/semibold/bold` | 400 / 500 / 600 / 700 |
| 阴影 | `shadow.sm/md/lg` | 2/8/24 模糊，`rgba(15,23,42,0.06/0.10/0.18)`，y 偏移 1/2/6 |
| 动效 | `motion.fast/normal/slow` | 120 / 180 / 260 ms |
| 缓动 | `ease.standard/entrance/exit` | `cubic-bezier(0.2,0,0,1)` / `(0,0,0.2,1)` / `(0.4,0,1,1)` |

排版原则：4px 栅格；文本对比度 ≥ 4.5:1（正文）/ 3:1（大字与图形）；1px 发丝边框统一 `border`；聚焦态一律焦点环；动效只用于状态过渡（不做装饰性抖动）。

## 6. 控制协议规范（`st-control/1`）

**传输**：TCP。帧 = `uint32` 大端长度 + UTF-8 JSON 体（单帧默认上限 64 MiB，截图可调）。
**连接**：`hello` 握手 → 请求/响应（`id` 回显）→ 服务端可随时推送事件帧。

### 6.1 消息形态
```json
// 请求
{"id": 7, "method": "find", "params": {"selector": "Button[text~=保存]", "limit": 10}}
// 响应
{"id": 7, "ok": true, "result": {...}}
{"id": 7, "ok": false, "error": {"code": "not_found", "message": "no element matches"}}
// 事件（无 id）
{"event": "ui.changed", "seq": 42, "data": {"changed": ["#btn-save"]}}
```
错误码：`bad_request` `not_found` `ambiguous` `unsupported` `timeout` `busy` `internal`。

### 6.1.1 坐标与 DPI 语义（协议级约定）

- **协议内一切坐标都是逻辑像素**（`input.mouse` 的 `x/y`、`capture.region`、`find` 返回的 `bounds`、`visual` 树、`tree` 节点 bounds）——与用户所见一致，与 DPI 无关。
- `hello` 的 `screen` 同时给出 `width/height`（逻辑）、`scale`（DPR）、`physical_width/physical_height`（物理），并显式标注 `coordinate_space: "logical"`。
- 需要物理像素时：`physical = logical × screen.scale`；`capture` 按物理分辨率出图。
- `metrics` 回包同样带 `device_scale` 与物理尺寸，便于断言"确实在高 DPI 下渲染"。
- 运行时改密度：`{"method":"app","params":{"action":"set_scale","scale":2.0}}` → 重建帧缓冲、字形缓存失效重栅格化、全量重绘（布局不变）。

### 6.2 方法表

| 方法 | 参数 | 结果 | 说明 |
|---|---|---|---|
| `hello` | `{protocol, client, subscribe?}` | `{protocol, app:{name,version}, pid, backend, headless, screen:{w,h,scale}, theme, capabilities}` | 握手；版本不符即拒 |
| `ping` | — | `{ts}` | 存活 |
| `tree` | `{depth?}` | `{tree:{id,type,role,bounds,state,text,children:[...]}, version}` | **嵌套**语义树快照（`depth` 可限深；`version` 用于判断是否需要重新拉取） |
| `find` | `{selector, limit?, visible_only?}` | `{matches:[{id,type,role,bounds,text,path}]}` | 选择器查询 |
| `get` | `{id, props?}` | `{id, type, props:{...}}` | 读取属性（缺省返回全部） |
| `set` | `{id, props}` | `{changed:[...]}` | 设置属性/文本/值（触发重绘与 `ui.changed`） |
| `invoke` | `{id, action, args?}` | `{ok, state?}` | 动作：`click` `dblclick` `focus` `blur` `toggle` `select` `scroll_to` `submit` `open` `close` |
| `input.mouse` | `{kind, x, y, button?, buttons?, modifiers?, delta?, to?}` | `{handled, hit}` | kind: `move` `down` `up` `click` `dblclick` `triple` `scroll` `drag` |
| `input.key` | `{kind, key?, code?, text?, modifiers?, repeat?}` | `{handled, focused}` | kind: `press` `down` `up` `text` |
| `input.text` | `{text, id?}` | `{handled, inserted}` | 便捷输入（聚焦或指定输入框追加文本） |
| `capture` | `{id?, region?, scale?, format?, encode?}` | `{width,height,format,base64? ,path?}` | 截图（元素区域或全屏；`encode=file` 直接落盘） |
| `visual` | `{id?, depth?, include_paint?}` | `{layers:[{id,type,bounds,z,visible,opacity,fill,radius,text,hit_region}], hits:[...]}` | **视觉元素树**：绘制层与实际命中区（区别于语义树） |
| `wait` | `{for, selector?, text?, timeout_ms?, stable_ms?}` | `{satisfied, elapsed_ms, detail}` | `for`: `element` `gone` `text` `text_gone` `stable` `frames` |
| `metrics` | — | `{backend, headless, renderer, text_renderer, uptime_ms, frames, fps, frame_ms:{p50,p95}, dirty_ratio, nodes, allocations}` | 运行时指标 |
| `events` | `{enable, kinds?}` | `{enabled, kinds}` | 订阅：`ui.changed` `frame` `input` `log` `theme` |
| `theme` | `{mode?}` | `{mode, tokens}` | 读/切主题（`light`/`dark`/`system`） |
| `app` | `{action, args?}` | `{ok}` | `resize` `quit` `reload` `screenshot_dir` `title` |
| `script` | `{code?, function?, args?, filename?}` | `{result, ops?, memory?}` | **默认禁用**（需 `--enable-script`）。`code` 直接执行；或 `function`+`args` 调用已定义函数；受内存/栈/时长/转换深度四重配额 |
| `shutdown` | `{graceful?}` | `{ok}` | 关闭服务（应用退出） |

### 6.3 选择器语法
```
selector   := compound ( '>' compound )*
compound   := simple* ( '[' attr op value ']' )*
simple     := '#' id | '.' type | type | ':' state
attr       := text | value | role | id | type | name | placeholder | checked | enabled …
op         := '=' (精确) | '~=' (包含) | '^=' (前缀) | '$=' (后缀) | '!=' (不等)
state      := visible | hidden | focused | enabled | disabled | checked | selected | hover
```
示例：`Button`、`#save-btn`、`Panel > Button[text~=保存]`、`Input:focus`、`ListItem:selected`。
匹配子串时对 UTF-8 安全；`find` 返回按树的先序顺序。

### 6.4 语义树 vs 视觉树
- `tree`/`find` 走 **语义树**（组件声明自己要暴露什么；`role` 面向无障碍与自动化，与 DOM/a11y 概念对齐）。
- `visual` 走 **绘制层树**（实际画了什么、z 序、命中区），用于「看起来对不对」的验证与像素级调试。
- 同一元素 id 在两侧一致，可交叉引用——`find` 定位 → `visual` 看渲染 → `capture` 看像素。

### 6.5 安全
- 默认只绑 `127.0.0.1`；`--control-bind` 显式才会监听其他地址。
- **默认无任意代码执行入口**：`script` 方法默认不存在，需宿主应用以 `--enable-script`
  （`AppOptions::enable_script`）显式开启；开启后仍受四重配额（内存/栈/时长/转换深度）约束，
  且脚本只能看到**显式注册**的宿主函数（本框架只注册界面操作与日志，无文件/网络/进程）。
  开启与未开启时 `hello` 的 `capabilities` 如实反映差异。
- 控制通道的每次调用在应用侧日志留痕（`log` 级别 `info`，含 method 与耗时）。

### 6.6 扩展层（`ext`）：JSON 与脚本

`ext` 是**外部基础设施的适配层**，与自研的 `core/raster/text/ui` 分层明确：这里放的是
“不值得自己写、但必须控住边界”的能力。两个模块，两种姿态：

**`st/ext/json.hpp`（必需）**——基于 nlohmann/json 的**薄封装**，不是重新实现 JSON。
封装存在的唯一理由是**把 nlohmann 的抛异常接口收敛成 `Result` 风格**
（本框架不设通用异常边界，抛出去就是 `std::terminate`）：

| 面 | 接口 | 语义 |
|---|---|---|
| 解析/写盘 | `json_parse` / `json_parse_file` / `json_write_file` | 失败即 `Result`；解析前先做**嵌套深度预检**（上限 128，防递归下降爆栈）、失败时用抛出式解析取**字节/行列位置**再翻译成错误 |
| 读取 | `json_get_*` / `json_find` / `json_at` / `json_path` | **永不抛**：类型不符或键缺失退化为默认值/null 节点（“配置写错不该让进程崩”） |
| 构造 | 直接用 nlohmann 原生（`Json::object()` / `obj["k"]=v`） | 不再包一层 |

类型用 `ordered_json`（**保留键插入顺序**）：清单与 lock 文件要被人读、被 git diff，
排序会让一次无关的读取-回写产生满屏 diff。

**为什么换掉自研实现**（实测，非偏好）：旧实现在 `src/core/json.cpp` 里把**所有数字统一存 `double`**，
于是 `9007199254740993`(2^53+1) 写成 `…992`、雪花 ID `1234567890123456789` 变 `1.2345678901234568e+18`
且 `as_i64()` 差 21；`1.0` 被吞成 `1`、`-0` 变 `0`（整型/浮点语义不保真）；而且它**没有任何单测**。
换成 nlohmann 后整数/浮点分型存储，三个失真点全部消失，并顺带获得成熟测试与 fuzz 覆盖。

**`st/ext/script.hpp`（可选）**——嵌入式 JS（QuickJS）。定位是“界面逻辑的表达层”：
主题计算、批量属性变换、联动规则这些“用代码写比用配置写短”的场景。

- **能力边界是设计出来的**：上游的 `quickjs-libc.c`（`std`/`os` 模块：文件/进程/socket）
  **已从 `third_party/` 剔除**，脚本里不存在 "require('os')" 这种东西；
- **四重配额在运行时层强制**（不是入口处检查参数）：内存上限、栈上限、
  中断回调按截止时间打断（死循环不会挂死调用方）、JSON↔JS 转换深度（挡循环引用）；
- **宿主函数只来自显式注册**，且失败对脚本是**可捕获异常**（脚本能区分“成功且返回空”与“失败了”）；
- 数值语义：能被 `double` 精确表示的整数保持整数（`{"line":3}` 不会变 `3.0`）；
  `NaN`/`Infinity` 与函数值**明确报错**而不是静默变 `null`；超出 `int64` 的 `BigInt` 退回字符串保真
  （不能用 `JS_ToBigInt64` 判溢出——它超范围时照样返回“成功”并给出截断值）。

接入界面：`codeeditor --enable-script`（默认关闭）。脚本 API 与性能模型见下节。

### 6.7 脚本控制组件（`ui::ScriptHost`）

### 目标

1. **组件控制逻辑用 JS 简化**——本可以不用 C++ 写的交互逻辑（联动、校验、状态回显、定时刷新）改用 JS；
2. **AI 通过 JS 读取与控制 UI**——`script` 从“一次性执行”升级为“可注册事件处理器的常驻逻辑层”。

### 性能模型：跨语言边界是唯一成本

| 反模式 | 本设计 |
|---|---|
| 每次读属性跨一次边界 | **快照批量**：首个查询时把整棵元素树（含属性面）一次送进 JS，之后 JS 内自由读写（100 次选择器查询 = 1 次跨界） |
| 每次改属性立即回写 | **变更集提交**：JS 侧 `set/click/focus` 记入队列，阶段末一次取回并应用（20 个属性 = 1 次提交；有单测锁住这条） |
| 所有事件都桥接 | 只桥接**被显式监听**的（选择器 × 事件类型）；鼠标移动类高频事件按帧合并 |

一次脚本入口的跨界次数是**常数级**（取变更集 1 次 + 事件派发 1 次），与“改了多少属性”无关。

### 三条路径一套语义

读取走 `ui::element_snapshot`、写入走 `ui::apply_properties`、动作走 `ui::invoke_element`——
与 C++ 应用代码、协议 `get`/`set`/`invoke` **完全同一份实现**（提到 `st/ui/actions.hpp`）。
这不是洁癖：实践里因“三份实现”踩过两次（脚本看不到属性面、`set` 白名单不一致）。

### JS API（`src/ui/script_api.js`，编译期嵌入）

```js
$('#save').text                         // 读属性（支持 #id / Type / Type[prop=v] / [prop^=v]…）
$$('Button')                            // 列表；count('Button') 只数数
tree()                                  // 整棵快照（调试用）
$('#status').set({ text: '已保存 ✓' })     // 排队变更（阶段末统一提交）
$('#save').click() / .focus() / .invoke(action, arg)
on('#save', 'click', e => log('clicked'))   // 事件绑定（返回绑定 id，可 off）
every(1000, () => ...) / after(500, () => ...)   // 定时器（主循环推进，不起线程）
state.count = (state.count ?? 0) + 1     // 脚本侧状态（跨执行保留，AI 可读回）
```

选择器未命中时：读属性得 `undefined`、写操作被**忽略但记日志**——
脚本不必到处判空，而拼错选择器（最常见的错误）也不会静默无效。

### 事件顺序（易错点）

观察者通知放在**元素自身处理之后**（`UiRoot::dispatch_to` 与 `ui::invoke_element` 同序）。
若放在之前，脚本写入会被随后的 C++ 处理器覆盖，表现为“用 JS 改了界面却没生效”（实测踩过）。

### 控制通道

`script` 方法一个入口覆盖四种用途（AI 用同一入口完成“写逻辑 → 触发 → 验证”）：

| 参数 | 语义 |
|---|---|
| `code` | 执行脚本片段（返回最后一条表达式的值） |
| `function` + `args` | 调用脚本中已定义的函数 |
| `selector` + `event` + `on` | 注册事件处理器（返回绑定 id），**常驻** |
| `off` / `bindings` | 注销 / 列出绑定 |
| `state` | 读回脚本侧状态（检查脚本内部逻辑走到哪一步） |

### 嵌入的 JS 资源

`src/ui/script_api.js`（运行时前置）与示例的 `assets/logic.js` 都是**编译期嵌入的真实文件**
（battery::embed，见 §7.8）：编辑器里能高亮、无需 C++ 原始字符串转义，改完重新构建即生效。

## 7. 包管理（stpm）

### 7.1 定位
自研包管理器：不依赖系统包管理器、不用 CMake/Make、不下载二进制（源码级内联优先，保证可审计与可离线）。
框架本体的外部依赖**直接内联在 `third_party/`**（nlohmann/json、quickjs-ng、batterycenter/embed），其余**全部自研**；
第三方**源码**直接内联在 `third_party/`（来源/许可/校验和见 `third_party/SOURCES.md`）；
stpm 另提供依赖获取能力（版本求解 + SHA-256 校验 + 缓存 + vendor 固化 + 直驱编译）。

### 7.2 清单 `st.pkg`（JSON）
```json
{
  "name": "shuangtian",
  "version": "0.1.0",
  "kind": "static_library",
  "cxx_standard": 20,
  "modules": ["core", "math", "codec", "raster", "text", "md", "ui", "shell", "gpu", "control", "app"],
  "include_dirs": ["include", "third_party"],
  "sources": ["src/**/*.cpp"],
  "third_party_sources": ["third_party/quickjs/*.c"],
  "c_flags": ["-std=gnu11"],
  "tests": ["tests/*_test.cpp"],
  "flags": ["-fno-strict-aliasing"],
  "defines": ["ST_VERSION=\"0.1.0\""],
  "targets": {
    "st": { "kind": "executable", "sources": ["tools/stpm/*.cpp"] },
    "gallery": { "kind": "executable", "sources": ["examples/gallery/*.cpp"] },
    "codeeditor": { "kind": "executable", "sources": ["examples/codeeditor/*.cpp"] }
  },
  "dependencies": {
    "modules": [],
    "source": [],
    "system": ["dl", "pthread"]
  }
}
```

两个字段专门服务第三方源码：

- **`third_party_sources`**：第三方翻译单元清单。这些单元**不套本工程的告警集（`-w`）、不进 PCH、
  不做 sanitizer 插桩**。理由：我们负责自家代码的质量，不负责上游的；
  不插桩还避免了“65k 行的 `quickjs.c` 在 `-O1`+ASan 下单文件就要 GB 级内存，并行构建被 OOM 杀掉”。
  混编不影响对我们的检测能力——ASan 的分配器是全局的。
- **`c_flags`**：C 源专用标志（默认 `-std=gnu11`）。**C 标志与 C++ 标志彻底分开**：
  `flags` 里的 `-Wnon-virtual-dtor`/`-Woverloaded-virtual` 是 C++ 专属，喂给 C 会直接报错。
  两者都得不到 `-std` 串味，`-x c` 保证同一个编译器二进制即可编 C，无需第二套工具链。
- `source` 依赖项：`{ "name": "...", "version": "^1.2", "source": {"kind":"path|http|git-tarball","url":"..."}, "sha256": "...", "build": {...} }`
- `system` 仅声明链接/加载项（`dl`/`pthread`/`m`），**不是第三方源码依赖**。

### 7.3 求解与锁定
- 语义版本（`^`/`~`/`=`/区间）+ **回溯求解器**：传递依赖、版本冲突、可选特性开关；无解时报冲突链。
- `st.lock`：`{name, version, source, sha256, deps[], build_fingerprint}`——构建可复现的依据。`--locked` 模式只按 lock 构建。

### 7.4 获取、校验、缓存、固化
- 源：`path`（本地目录）/ `http`（自研 HTTP 客户端）/ `git-tarball`（本地归档）；HTTPS 经**运行时 dlopen 系统 TLS**（缺失则明确报错并给替代）。
- 解包：自研 `tar`(ustar/pax) + `gzip`(inflate) + `zip`（复用 codec）。
- 缓存：`{ST_HOME:-~/.shuangtian}/cache/<sha256>/`；命中即跳过下载；**SHA-256 不匹配一律拒用**。
- 工作区：项目 `.st/work/`（依赖解包与中间产物）；`st vendor` 把依赖源码树固化进 `vendor/<name>/` + `vendor.lock`（**随仓库分发、离线可构建**）。
- 构建集成：依赖以 **声明式** `st.build` 规则（源文件/包含目录/宏/产出）纳入构建图——**不执行任意脚本**。
- 隔离：每个依赖独立 include 根与独立中间目录；多版本共存按目录隔离。
- **依赖直接内联（`third_party/`）**：三个外部依赖的源码**随仓库分发**，不是"需要下载的依赖"。
  `third_party/SOURCES.md` 记录来源 URL / 版本 / 许可 / **逐文件 SHA-256** / 剔除与修改清单——
  `cd third_party && sha256sum -c CHECKSUMS.sha256` 一命令回答“依赖了什么、哪些上游代码被动了”。
  许可合规（MIT 保留版权、Apache-2.0 保留 LICENSE 与修改声明）在同一文档中说明。

### 7.5 构建图与直驱编译器
- `st build [target]`：解析清单 → 拓扑排序（依赖先编）→ 生成编译命令 → **直接调用 `g++`/`clang++`**（`-MMD -MF` 依赖文件 + 增量判新旧）。
- 产物：`build/<profile>/<target>/…`；`profile` ∈ `debug` / `release` / `san`。
- 并行：按 `nproc` 并行编译单元（自研 job 池，`std::jthread`）。
- **按语言分派标志**：`.c` 走 `c_flags`+`-x c`（C 语言标准与 C++ 标志集互不串味），其余走 C++ 标志集；
  C 源不吃 PCH；第三方单元额外 `-w` 且剥掉 sanitizer 插桩（见 §7.2 两个字段的说明）。

### 7.5.1 编译效率（工程化硬指标）

霜天零第三方依赖，但**标准库头部解析**占单文件编译耗时的一半以上（`<format>`/`<filesystem>`/`<thread>` 尤重）。
构建驱动因此做了四件事，全部由 `st build` 自动完成，无需额外工具（不用 ccache / 不用 distcc）：

| 机制 | 做法 | 效果 |
|---|---|---|
| **预编译头（PCH）** | 框架头 `include/st/pch.hpp` 汇总高频标准库与核心头 → 一次编译为 `build/<profile>/pch/` 下的 PCH，其后每个 TU 复用（GCC 按 `-I` 命中 `.gch`；MSVC `/Yu+/FI+/Fp`） | 消除重复头部解析（首选加速项） |
| **并行编译** | 自研 `ThreadPool` 按 `nproc` 并行调度翻译单元（`--jobs N` 可覆盖） | 8 核约 6~8× |
| **头依赖增量** | 每个 TU 产出 `-MMD -MF <obj>.d`，重建判定取「源文件 + **全部被包含头文件**」的 mtime 最大值——改一个头文件只重编真正受影响的单元 | 增量构建从"全量重编"变为"精确重编" |
| **分档构建** | `dev`（-O1 -g，日常迭代）/ `debug`（-O0 -g）/ `release`（-O2 -DNDEBUG）/ `san`（ASan+UBSan） | 开发档编译时间约减半 |
| **产物原子写** | 编译先写 `<obj>.tmp` / `<dep>.tmp`，进程成功退出后才 `fs::rename` 到位 | 中断（OOM 杀编译器、磁盘满）不会在产物位置留下**半截 `.o`** |

产物原子写的理由：半截 `.o` 比源文件新，增量判新会把它当最新的，于是**下一次**构建报出
一堆莫名其妙的链接错误（`undefined symbol`），而真实原因（上一次编译被杀）早就过去了。
实测被这个现象骗过两次，才改成"要么完整、要么不存在"。

`st build` 结束会打印耗时构成（编译 / 链接 / 总时长 / 并行度 / 是否使用 PCH），便于回归对比。
`--no-pch` 可关闭预编译头用于排查（例如 PCH 与某编译选项冲突时）。

**并发由内存决定，不由核数决定**（实测数据驱动）：编译是内存密集型的，本框架单翻译单元实测峰值
`-O1 -g` 60–490 MB、`san` 档（ASan/UBSan 插桩）630–700 MB；`nproc` 为 28 的机器满并发就是
10–20 GB 瞬时占用。两个反直觉的事实：

1. **峰值与源文件大小不成正比**——`build.cpp`（55 KB）峰值 487 MB，而 `quickjs.c`（2.1 MB / 6.5 万行）
   只有 335 MB（C++ 单元的头部展开常比"行数多"更贵）。所以**不能只按体积分档**，
   主机制必须是"内存预算 ÷ 单单元估算"。
2. **容量与核数无关**——容器限 8 GiB 而宿主机 114 GiB 时 `free` 看起来毫无压力，
   只有读 cgroup 才知道真实上限。

机制（`pkg/memory.hpp` + `compile_units` 调度处）：

| 环节 | 做法 |
|---|---|
| 预算探测 | cgroup v2 `memory.max` → v1 `memory.limit_in_bytes` → `/proc/meminfo`；`ST_MEMORY_MB` 可覆盖 |
| 推导并发 | `jobs = clamp((预算 × 7/8) / 单单元估算, 1, nproc)`（预留 1/8 给链接与系统） |
| 单元估算 | `san` 768 MB；`debug`/`quick` 384 MB；`dev`/`release` 512 MB（取实测**上限**而非均值——估偏只是慢，估偏乐观就是 OOM） |
| 超大单元闸门 | 源文件 ≥ 512 KB 的单元走独立窄闸门（默认**同时 1 个**），且**排到最后提交**——小单元先跑满并发，大块头收尾时独占 |
| 显式接管 | `--jobs N` / `--jobs-large N` / `--max-memory MiB`（CI 可固定行为） |
| 可观测 | `st doctor` 报告内存上限与各档推导出的并发（含理由）；每次构建打印一行决策 |

> `st doctor` 曾报"内存上限不可知"——顺着查出一个真缺陷：`fs::read_text` 原先按 `seekg/tellg`
> 得到的大小定长读，而 `/proc`、`/sys`、cgroup 的虚拟文件 **size 报 0（内容却非空）**，于是读到空串，
> 内存探测永远失败。已改为读到 EOF（见 §4.1）。

**实测（本仓库当前规模：54 个翻译单元，8 核）**：
| 场景 | 时间 |
|---|---|
| 串行全量（无 PCH，基线） | ≈ 49 s |
| 并行全量（8 路 + PCH） | 见 README「实测数据」（由 `st build` 自报） |
| 改 1 个 .cpp 后增量 | 亚秒级（仅重编该单元 + 链接） |
| 改 1 个公共头后增量 | 只重编依赖该头的单元（依据 `.d` 依赖图） |

### 7.6 CLI
| 命令 | 作用 |
|---|---|
| `st init <name>` | 生成工程骨架 |
| `st build [target] [--profile debug\|release\|san] [--locked] [-j N]` | 构建 |
| `st run <target> [args…]` | 构建并运行 |
| `st test [filter] [--san]` | 构建并运行单测（含 sanitizer 档）；`--list` 只列用例不跑；`--format junit [--junit-out 路径]` 写逐用例 XML 报告（CI 消费） |
| `st lint [--explain <rule>]` | 禁令静态扫描（`CONVENTIONS.md` §8） |
| `st add <spec>` / `st remove <name>` | 依赖增删（改清单 + 重求解 + 写 lock；**规划中**，CLI 尚未接线） |
| `st fetch` / `st sync` | 获取依赖 / 同步 lock（**规划中**；当前 HTTP 仅明文 + 解包未实现，实际可用源为 path） |
| `st tree` / `st audit` / `st outdated` | 依赖树 / 校验和与许可证字段复核 / 版本检查（**规划中**，CLI 尚未接线，见 `docs/BACKLOG.md`） |
| `st clean [--all]` | 删除 `build/` 各档产物（`--all` 连共享对象缓存一起清） |
| `st doctor` | 环境自检（编译器、字体、显示后端、GPU、TLS、缓存；Windows 下报告内存上限与并发推导） |

### 7.7 引导（bootstrap）
`st` 自身是 C++ 程序：`bootstrap.sh` 用最朴素的编译器调用把 `tools/stpm/*.cpp` + 所需 `src/core/*` + `src/ext/*` 编成 `build/bin/st`（唯一非 st 构建入口）；此后一切（含 `st` 自身重建）由 `st build` 完成。

### 7.8 编译期资源嵌入（battery::embed）

上游 [`batterycenter/embed`](https://github.com/batterycenter/embed)（Apache-2.0，v1.2.19）
把资源文件在编译期变成字节数组，接口是 `b::embed<"path">()`——**路径写错在编译期就报错**，
开发期还能热重载（磁盘文件变了就回调）。

它与我们的构建哲学有一处正面冲突：上游是**一个 CMake 文件**，而本框架用 stpm **取代** CMake。
解法是按职责切开：

| 部分 | 处置 |
|---|---|
| 运行时（`embed_impl.cpp`，热重载） | **原样内联**（`third_party/battery/`），编译进目标 |
| 头模板 / 单文件模板 | 内联为模板（占位符语义与上游逐字一致） |
| 生成逻辑（上游的 CMake） | **由 `st build` 原生实现**（`src/pkg/embed.cpp`） |

于是拿到与上游**相同的 API** 且零 CMake 依赖：

```cpp
#include "battery/embed.hpp"                 // stpm 生成到 build/<profile>/embed/.../include/
b::embed<"examples/gallery/assets/about.txt">().str()   // 编译期路径检查 + 嵌好的字节
```

语义与上游逐项对齐（标识符规则 `tolower(target_path)`、查找键 = 使用者书写的相对路径、
编译期 `static_assert`、`B_PRODUCTION_MODE` 关闭热重载并去掉绝对路径）。清单声明：

- **目标级** `targets.<名>.embed`：应用自己的资源（标识符前缀 = 目标名）；
- **工程级** `embed`：框架自身要用的资源（如脚本运行时前置，前缀 = 工程名）。

两个实现要点（都有实际教训）：

- **字节数组用八进制转义**（`\NNN`，固定三位）：C++ 的 `\x` 会**贪婪**吃掉后续十六进制字符，
  八进制从语法上不可能歧义；
- **运行时实现只编译一次**（库有嵌入时归库，否则归目标）：两边都带会让同一个可执行文件
  出现两份进程级全局表（实测链接期 duplicate symbol），热重载表也会分裂。

### 7.9 交叉编译（Linux 上产出 Windows/macOS 程序）

**为什么要有**：框架的目标平台是三平台，而开发机通常只有 Linux。
"Windows 分支是否正确"这件事在 Linux 上**永远测不出来**——
本轮实测就抓到了一批只存在于 Windows 分支的问题：

| 类型 | 实例 |
|---|---|
| 平台 API 不存在 | `::getpid()`（控制通道 `hello`）→ 交叉编译直接报 `has not been declared` |
| 从未被编译的代码攒了质量问题 | Windows 分支里的 `nodiscard` 忽略返回值、`-Wunused-but-set-variable`（`-Werror` 下直接失败） |
| 宿主宏误判目标 | `default_system_libs()` 用 `#if defined(_WIN32)` 判的是**宿主**（Linux 恒为假），给 Windows 目标链上了 `-lpthread -ldl -lm` |

**怎么用**：工具链在 `st.pkg` 的 `toolchains` 段声明，`--toolchain=<名>` 启用。

```jsonc
"toolchains": {
  "mingw": {
    "compiler": "x86_64-w64-mingw32-g++",
    "c_compiler": "x86_64-w64-mingw32-gcc",
    "platform": "windows",
    "system_libs": ["ws2_32", "winpthread"],     // 接管（不再追加 linux 的 pthread/dl/m）
    "defines": ["_WIN32_WINNT=0x0601", "WINVER=0x0601"],
    "executable_suffix": ".exe",                  // 产物带后缀，Windows 才认它是程序
    "extra_flags": ["-static-libgcc", "-static-libstdc++"]
  }
}
```

```bash
st build gallery --toolchain=mingw        # → build/dev-mingw/bin/gallery.exe（PE32+）
st build st --toolchain=mingw             # 工具链自身也交叉编一遍（覆盖只被 stpm 用到的平台层）
st run gallery --toolchain=mingw          # 宿主==目标才执行；否则拒绝并提示在目标平台运行
st doctor                                 # 探测清单声明的工具链是否真的装了
```

CI（`.github/workflows/shuangtian-ci.yml`）在 Linux 作业里对 **gallery 与 `st` 自身**都做
mingw 交叉编译——这是 Windows 分支唯一的持续验证手段。

**关键设计点**：

| 点 | 理由 |
|---|---|
| 目录隔离 `build/<档位>-<工具链>/` | PCH 是按"编译器 + 目标"生成的；对象/嵌入生成物混用会得到难解的编译错误 |
| 系统库**整体接管**（工具链声明了就不追加本机默认） | 一份清单服务多平台，"本机需要哪些库"对目标可能是错的甚至不存在 |
| 交叉编译不套 `-fuse-ld=lld/mold` | 宿主装的链接器不一定支持目标格式 |
| `st test`/`st run --toolchain` 按**目标平台是否等于宿主**决定能否执行 | 目标≠宿主：拒绝并提示"请在目标平台运行"（比 `Exec format error` 可读）；**目标==宿主放行**——Windows 宿主上的 mingw 档产出本机可执行的 PE，`st test --toolchain=mingw` 应照常跑（此前按"交叉即不可执行"一刀切，误拒了 Windows→Windows 场景） |
| C 源沿用同一编译器 + `-x c` | 不引入第二套工具链（mingw 的 `g++ -x c` 即可编 QuickJS） |

> 顺带：交叉编译是**平台分支的强制验证手段**。`CONVENTIONS §10` 因此规定
> "改了平台分支就要交叉编译一次"——这是唯一能发现 Windows 分支问题的途径。

### 7.10 独立工程引用框架（源码级依赖）

**纳入方式**：清单里写 `"framework": { "path": "…" }`，构建时把框架的**库部分**并入本工程的构建图。
不做"预编译库 + 安装步骤"：安装位置、ABI/编译器版本、交叉编译两套产物都是持续的麻烦；
源码级引用**无需安装、始终同版本、离线可构建、交叉编译天然生效**。

| 并入 | 不并入 |
|---|---|
| 框架源（排除 `src/pkg/*`）、包含目录、严格标志与宏 | 框架的 `targets`（那是框架自己的示例/工具） |
| 第三方 C 源（QuickJS：按第三方放宽告警、不插桩） | 框架的 `tests`（引用方不跑框架单测） |
| 编译期嵌入（`script_api.js`）、系统库 | `src/pkg/*`（工具链实现，应用用不到） |
| 交叉编译工具链（引用方同名可覆盖） | —— |

**嵌入的按单元分发**（关键约束）：框架与工程各自生成 `battery/embed.hpp`，
且**只下发给各自的编译单元**。若两者的包含目录同时出现在一个编译单元的命令行上，
先命中的赢、另一方的资源会 `static_assert` 失败（"No such file"）。
因此框架目录里有框架的嵌入头，工程目录里有工程的，互不可见——跨边界引用对方资源不受支持。

**工具链继承**：目标平台要哪些系统库（`ws2_32`/`gdi32`/`user32`）、要什么宏（`_WIN32_WINNT`）
是"框架与平台如何配合"的知识，不该要求引用方知道。工程想覆盖就自己声明同名工具链。

### 7.11 共享对象缓存

**动机**：源码级依赖的代价是"每个工程首次要编框架源"。缓存把这份代价摊掉。

| 项 | 取值 |
|---|---|
| 位置 | `{ST_HOME:-~/.shuangtian}/cache/objects/<键>/unit.{o,d}`（`ST_HOME` 缺省 `~/.shuangtian`） |
| 键 | 编译命令（编译器 + 全部标志/宏/包含目录 + 源路径 + 语言 + 第三方/框架标记），**排除 `-o`/`-MF`** |
| 有效性 | 复用增量构建那一套：缓存对象带 `.d` 依赖清单，**没有任何依赖比它新**才算命中 |
| 关闭 | `ST_NO_CACHE=1`（测量干净耗时用） |

**为什么框架单元用"框架自己的标志集"编译**：缓存键里绝不能有工程相关的东西。
若框架单元照抄工程的 `-I<工程目录>`/`-D<工程宏>`，键会随工程变化 → 缓存永不命中
（实测：第二个工程 30 s 全量重编）。因此框架单元：

- 只带框架自己的包含目录（`include` / `third_party`）与框架的宏；
- **不吃工程的 PCH**（PCH 路径里带着工程目录）；
- 语义上也更对：框架源不该被引用方的同名头影响。

实测：新工程冷构建 ≈ 30 s，第二个工程 ≈ 4 s（59/60 单元命中）。

## 8. 工程实践：无头开发闭环与实现现状

> 本节记的是「怎么把它做出来的」：无头闭环（8）、示例检验（8.1）、
> 真实缺陷清单（8.2）、两条渲染路线的落地（8.3/8.4）、验证体系（8.5）。
> 接口与契约在 §4–§7，这里只放**实践与现状**。

```
① st build                          # 编译框架与应用
② st run gallery --headless --control-port 0 --control-file .st/ctl.json
                                    # 无窗口启动；端口自动分配并写入文件
③ 歌白 shuangtian 子代理（或任意 TCP 客户端）：
   tree / find / get / visual       # 看清结构
   capture --encode file           # 截图落盘 → 直接看图核验美观
   set / invoke / input.*           # 改状态、点击、输入
   wait / metrics                   # 等就绪、看性能
④ 改代码 → st build → 重启 → 复验（闭环）
```
关键点：**无头后端 + 软件光栅器**保证像素结果与有窗口时一致；`capture` 落盘的 PNG 即是最终外观。

### 8.1 示例项目（用真实应用检验框架）

框架自带两个**功能完整**的示例应用（`examples/`），分别对应"能力面"与"应用面"：

| 示例 | 定位 | 检验点 |
|---|---|---|
| `gallery` | **组件集 / 设计系统巡检**：导航、统计卡、按钮矩阵、图标墙、表单、列表、进度条、主题与 DPI 切换、截图 | 组件库完整度、设计令牌一致性、DPI 正确性、控制通道可达性 |
| `codeeditor` | **VSCode 式代码编辑器**：标题栏/菜单栏/活动栏+侧栏（资源管理器·搜索·源代码管理·运行·扩展）/多标签编辑区（修改点、可关闭）/底部面板（问题·输出·终端）/状态栏；命令面板（Ctrl+Shift+P）、全局快捷键、8 种内置语言 + 自定义语言 `stlog` | **易用性**（一个 IDE 形态界面 ≈ 数百行声明式组装，全部用内置组件，零自绘）、**组件库覆盖度**（MenuBar/Tabs/Tree/List/Overlay/快捷键在同一真实形态下的协同）、**控制通道全链路**（多编辑器实例的属性面与动作） |

`codeeditor` 的重写史本身是框架演进的一个注脚（详见 §8.1.1）：早期版本靠**自绘定制组件**（Markdown 编辑器 `mdeditor` 的 SourceView/OutlinePanel/SplitHandle/StatsBar）验证扩展点闭环；重写后同样的 IDE 形态**全部由内置组件组装**——证明组件库覆盖度已从"能扩展出来"进化到"开箱即有"。

### 8.1.1 演进反推：从两个示例看组件库的下一步

把示例更替当成一次**对框架的测量**：同一形态（桌面 IDE）在两个时代用两种写法实现，差值就是框架这两年的实际演进；而新写法里仍然要"绕一绕"的地方，就是下一步该长的能力。反推结论（按证据强度排序）：

**① 已经闭环的：扩展点 → 内置化**
- mdeditor 时代（已删除）：四个自绘组件（SourceView/OutlinePanel/SplitHandle/StatsBar）只用 `Element` 四扩展点写成——证明"框架没给的也能长出来"；
- VSCode 重写：同样的编辑器形态零自绘——Tabs（key 同步/修改点/可关闭/溢出滚动）、Tree、MenuBar+MenuPanel、List、Overlay、register_shortcut 全部内置；
- 演进方向被验证：**组件先在示例里自绘验证形态，形态稳定后内置化**。这条路径本身就是框架的扩展策略。

**② 被编辑器形态逼出来的能力（ui_root.hpp 注释自证）**
- `register_shortcut`（快捷键表先于焦点链）：注释明写"编辑器形态的 Ctrl+S/Ctrl+W/Ctrl+Tab 需要先于一切组件的落点"——没有这个，文本组件吞键后全局快捷键永远不可靠；
- `Tabs::sync_tabs`（按 key 对齐、保留修改点与活动态）：多标签编辑是它存在的直接理由；
- `OverlayLayout::FillViewport`：注释明写"每个应用重复造轮子的历史缺口"——命令面板/模态遮罩的标准形态。

**③ 重写中暴露、仍需演进的缺口**
- **桌面窗框/标题栏**：示例只能用装饰性图标模拟窗口控制（— □ ×）——平台 shell 层应提供系统标题栏融入或自绘窗框，目前应用层无从谈起；
- **SplitView 内置化**：SplitHandle 曾是 mdeditor 的自绘组件，重写后编辑器侧栏/主区/面板仍无拖拽分栏——它该从"示例级自绘"升为框架组件；
- **命令面板通用组件**：本次在示例里手写了 CommandPalette（FillViewport + 过滤列表 + 键盘导航）——与 MenuPanel/SelectPanel 同族，值得内置为 `CommandPalette`；
- **单行 Input 的动作面**：`TextArea` 支持 `invoke submit`，单行 `Input` 没有（本次交互验证发现）——DESIGN §8.2 第 26 条同族缺口（API 存在但动作面未实现）；
- **虚拟化长列表**：终端/输出面板用 ScrollView + Text 累积全文，日志长了会退化——需要虚拟化 List（按可见行复用元素）；
- **编辑器分组**：VSCode 的编辑器组（左右分屏各持独立标签组）当前无法用 Panel 组合自然表达，需要容器级支持。

**④ 文档口径**：本节描述的是"从代码反推的方向"，不是承诺——各项进入 BACKLOG.md 后按实际需要排期。

### 8.2 实战缺陷与修复（无头协同开发暴露的真实问题）

用智能体在无头服务器上"开发—运行—观察—操作"自己写的框架，第一轮就把几类只有真跑才暴露的问题逼了出来。
记录在此，作为同类问题的检查清单：

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | CJK 部分字形（带「口」部首）渲染成黑块 | 填充未隐式闭合子路径 → 环绕数不归零、填充右溢 | `rasterize_polylines` 补 `last→first` 闭合边（§4.2.2） |
| 2 | 圆角矩形、阴影、字形**全部**失效（只剩直角实心） | 覆盖率为带符号量，画布侧直接当透明度用了 → 逆时针轮廓被丢弃 | `blend_coverage_row` 取绝对值 |
| 3 | 大量汉字轮廓失败「CFF 子程序索引越界」 | Type2 `callsubr` 操作数是"编号 − bias"、**可为负**，实现却把负值判为越界 | 先加 bias 再判边界；FD 子程序 bias 按该 FD 计数取 |
| 4 | 界面偶发闪退（`TextRenderer::draw` 段错误/ASan heap-use-after-free） | 字形缓存"先插入后淘汰"，`clear()` 把刚插入的位图一起销毁并返回悬垂指针 | 缓存值改 `shared_ptr`，淘汰移到插入之前 |
| 5 | `st build` 报成功但没有任何产物 | `process::run` 二次 `waitpid` 失败（ECHILD）后把 `status` 留在 0 → **失败退出码被当成 0**，掩盖编译/链接失败 | 只在未回收时 wait；`waited < 0` 直接报错 |
| 6 | 空闲客户端被服务端误判断开（`wait` 挂起连接丢失、主循环每次空转 5s） | 非阻塞 socket 上直接 `read_some`，EAGAIN 走"等待可读"分支超时后被当成 EOF | 读前先 `wait_readable(0)`，无数据即返回，不算断开 |
| 7 | 编辑器打开文档后视图停在文末 | `TextArea::set_text` 把光标置于末尾，`sync_scroll` 随之滚到底 | 光标回文首（编辑中插入文本另有保持可见的路径） |
| 8 | `invoke focus` 后 `input.text` 无处可送 | 焦点只改元素自身标志，未经 `UiRoot` 路由 | `invoke` 的 `focus`/`blur` 走 `UiRoot::set_focus` |
| 9 | PCH 建了却从不生效（编译照旧慢） | PCH 与消费端标志不一致（消费端多 `-Wno-error`）→ GCC 判 PCH 无效 | 生成 `prefix.hpp` 代理头统一入口，两端标志完全一致 |
| 10 | 预编译头让编译**更慢**（3.5s → 5.0s/单元） | PCH 里塞了项目头，不需要它们的 TU 白白解析 | PCH 只放标准库（§7.5.1） |
| 11 | **运行时切 DPI 完全不生效**（内容缩在左上 1/4） | `Canvas` 的移动构造/移动赋值没搬运 `scale_`/`inverse_scale_`：`*canvas = Canvas::for_logical_size(w,h,2)` 之后新画布尺寸是 2x 而换算仍按 1x | 移动语义一并搬运缩放；加回归 `dpi_move_preserves_device_scale` |
| 12 | HiDPI 下 `capture` 只截到左上 1/4，且 region 与坐标对不上 | 截图默认区域用 `int_bounds()`（逻辑尺寸）取像素；`region` 是逻辑坐标却直接当物理像素用 | 默认区域改整个**物理**缓冲；`region` 逻辑→物理换算；回包新增 `pixel_size` 标注实际像素尺寸 |
| 13 | 2x 屏上 `Card` 内的子项**整片消失** | `push_clip_rounded_rect` 未做逻辑→物理换算：圆角路径按逻辑坐标光栅化，却与物理口径的 mask/clip 混用，裁剪区只覆盖左上 1/4 | 入参逻辑坐标统一换算后再构造路径；头文件写明 `push_clip_path` 按物理像素解释 |
| 14 | 根页面里的 `grow` 失效（整页缩在上半截） | `UiRoot::layout` 按内容**自然高度**排布根节点，根层的 `grow` 没有可分配空间 | 根内容按「至少铺满视口」排布（内容更高时保持自然高度） |
| 15 | 自定义语言的"定义关键字"（`fn`/`def`）着色静默失效 | 扫描器只在**关键字表**里查，未查 `definition_keywords` | 命中定义关键字同样按关键字着色（它同时是"其后标识符是函数名"的判定依据） |
| 16 | Ctrl+←/→ 按词移动会停在空白处（"回退到词首"落空一格） | 词移动实现只做单步跳过，未先跳过空白再找词边界 | 向左停在词首、向右停在词尾之后（与主流编辑器一致） |
| 17 | 控制通道 `input.mouse` 直接**终止进程**（nlohmann `at()` 抛异常，无异常边界 → terminate） | JSON 迁移时按“首参是字符串字面量”启发式改写 `at()`，`params.at(name)`（变量键）被漏掉；而修饰键本就是可选的 | 改用 `json_at`；新增 lint 规则 **L12** 机械拦住同类写法（见 §8） |
| 18 | 脚本里 `ui_get('editor').props` 为空 | `ui_get` 只返回元素基本信息，属性面只在控制通道 `get` 里拼装——同一元素两条路径两个样 | 抽出 `element_snapshot`，`get` 与 `ui_get` 共用 |
| 19 | 内存受限时的错误消息是空洞的 `null` | 内存耗尽时 QuickJS 连异常对象都建不出来（构造它也要分配内存） | 识别该情况，直接给出“通常是内存超限：上限 N MiB”与实测值 |
| 20 | 函数返回值静默变成 `{}` / 超大 `BigInt` 被静默截断 | 转换顺序上函数被当作“无键对象”；`JS_ToBigInt64` 超范围时仍返回成功并给出截断值 | 函数提前识别并报错；BigInt 改按十进制文本 + `from_chars` 精确判定，超范围退回字符串 |
| 21 | 名字骗人：`max_ops` 声称“指令数”，实际是中断回调次数（且默认量级下永远等不到） | QuickJS 在 VM 周期里回调中断钩子，频度实测约 2 千次/秒，与“CPU 指令数”不是一回事 | 改名 `max_interrupts` 并在文档中写明它是粗粒度兜底（时长控制的主力是 `timeout`） |
| 22 | 脚本绑定的事件**永远收不到 UI 事件** | 事件观察者要调用方手动 `set_event_observer` 接上；忘了不报错，只是静默失效（写测试时当场踩到） | 改为 `ScriptHost` 构造时**自接**、析构时摘除（消除易漏的人为接线） |
| 23 | 用 `invoke(click)` 触发按钮时，脚本**收不到**该点击 | `invoke` 直接调元素的 `invoke_action`，不经过事件分发管线；而真实鼠标点击会走 | 点击类动作合成事件并通知观察者（三条路径对监听者表现一致） |
| 24 | 脚本改了界面却“没生效” | 观察者通知在 C++ 处理**之前**，脚本写入随即被处理器覆盖 | 通知**后置**：脚本看到处理后的状态，其写入是最终态（`dispatch_to` 与 `invoke_element` 同序） |
| 25 | 脚本前置里 `host` 为 `undefined` | 前置按 `globalThis.__st_host` 取桥，而宿主注册的是**全局函数**（`__snapshot` 等） | 前置改为直接引用宿主全局函数（一处列出“宿主提供了什么”） |
| 26 | 对输入框 `set {value:...}` **静默无效** | `Input` 从未实现属性面（`get/set_property`），而 `apply_properties` 的文档却把 `value` 列为支持项——协议 `set` 一直受影响 | 补齐 `Input` 属性面（value/placeholder/password/enabled/visible）；教训：声称支持的名字必须有对应实现与测试 |
| 27 | 拼错选择器（如 `$('brand')` 少了 `#`）**静默丢弃** 整个写入 | 未命中句柄的写操作被无声忽略 | 未命中仍不抛异常（脚本保持简短），但**记日志**给出提示（含正确的 `#id` 写法） |
| 28 | 库级与目标级都编译 `embed_impl.cpp` → 链接期重复符号 | 运行时含进程级全局表；两边各带一份 | 运行时只编一次：库有嵌入时归库，否则归目标（§7.8） |
| 29 | 生成的头文件按作用域互相污染（ODR） | 每个作用域的声明/返回链不同，却共享同一个 `EmbeddedFile` 类 | 按范围隔离包含目录；目标自己的源也需这条 `-I`（曾漏加，表现为找不到 `battery/embed.hpp`） |
| 30 | **改了头文件不重编 → 运行期 access violation**（给 `Canvas` 加一个成员就崩） | MSVC 依赖清单静默降级：`/sourceDependencies` 只有**文件名以 `.json` 结尾**时才输出 JSON（用了 `.tmp` 后缀，拿到的是本地化文本，解不成 → 退化回“只跟踪源文件”）；且共享对象缓存把这份不完整的 `.d` 当作有效性依据，把错误钉住 | JSON 遍历补上“数组元素是字符串”一层（v1.2 格式）并加专项测试；缓存键加入依赖格式版本 `<deps:N>`；清缓存 + 清 `build/` 重建 |
| 31 | 往下滚**滚不动**（页面下半截永远看不到），但事件 `handled=true` 不报错 | `ScrollView` 把 `wheel_delta` 加反了：系统口径是“上为正”，而偏移随内容向下增大；取错符号后偏移一直被夹在 0 | `scroll_by(-event.wheel_delta * step_)`；新增 `tests/ui_scroll_test.cpp`（方向/夹取/键盘/通知） |
| 32 | 图标“缺了半截弧线”、描边形状残缺 | 描边路径由“每段四边形 + 顶点圆”拼成，两者绕向不一致，旧覆盖率逐像素累加时**错误抵消** | 覆盖率改运行段合并（§4.2.3），重叠区按符号相加后取绝对值 |
| 33 | 焦点环一侧明显偏深（单像素级发现） | 运行段混合时尾部端点像素在“已含在整段内”的情况下又被单独混合一次 → 同一像素叠了两次 | 尾端像素只在区间右边界超出时才单独处理 |
| 34 | HiDPI 下 `gallery.ilk/.pdb` 缺失（崩溃时连栈都符号不出来） | `/DEBUG` 是**链接器**选项，放在 `/link` 之前被 `cl` 当编译选项丢弃（只报 D9002） | `/DEBUG` 移到 `/link` 之后 |
| 35 | `st test` / `st build --toolchain=…` 在 Windows 上**无声 abort**（无输出、无日志） | `std::filesystem::path` 的两个编码方向都会抛：PATH 里一条非 UTF-8 目录名（UTF-8→宽）、某个文件名代码页表达不了（宽→窄，`generic_string()`）；而调用点都是纯查询，抛出去就是 terminate | `to_path`/`to_utf8` 改为不抛（分别回退到“逐字节放宽”与“字节保留”）；列目录改用 `generic_u8string()`；回归 `tests/core_fs_test.cpp` |
| 36 | 中文路径在 Windows 上变成 GBK 字节流 | `entry.name` 用 `generic_string()`（本地代码页）而非 `generic_u8string()` | 统一走 `to_utf8()`（§10 第 2 条） |
| 37 | **深色主题的卡片一圈蓝光**（越靠近卡片蓝色通道越高） | `Color::from_hex` 靠“数值 > 0xFFFFFF”区分 `0xRRGGBB` 与 `0xRRGGBBAA`；`0x0000008C`（想写“黑 55%”）落在 6 位区间 → 被读成 `0x00008C` = 纯蓝、不透明 | 新增 `Color::from_rgba_hex`（恒读 8 位）并让设计令牌统一走它；`tests/math_color_test.cpp` 把两者差异钉住 |
| 38 | 输入框没图标，文字还莫名右移一段 | `set_icon_prefix` 存在、`inner_box` 也一直留槽位，但 `paint_content` **从未把它画出来** | 按槽位绘制图标（聚焦时主色）；`tests/ui_input_test.cpp` 数槽位像素 |
| 39 | 11px 的导航标题/版本号“看不清” | `text_faint = #94A3B8` 在白底上只有 **2.6:1**（同层 `text_muted` 是 4.8:1）——层次号称三级，实际一级半 | 重排文字三级（faint 提至 4.1:1、muted 至 6.3:1）；把对比度写成测试（§4.2.4） |
| 40 | 卡片“浮不起来”，看着像白底 + 一道灰边 | 单层高斯投影无法同时表达“近紧远散”，且在浅底上会被读成灰边 | `Shadow` 增第二层；`shadow_sm/md/lg` 改“关键层 + 环境层”；卡片默认升到 md、描边降为半透明（§4.2.5） |
| 41 | **所有图标都“转了个方向”**：`chevron-down` 指向右、`home` 变成 `<E`、`folder` 歪成破四边形 | `Icon::path` 写成 `map(next_number(cursor), next_number(cursor))`——C++ **不规定函数实参的求值顺序**，MSVC 从右往左求值，于是 x/y 被交换。而 GCC/Clang 从左往右，**Linux 上完全正常** | 拆成独立语句（`read_point()`）；`tests/ui_icon_path_test.cpp` 断言**解析出的坐标本身**（像素断言挡不住：交换后依然有像素） |
| 42 | 72 个图标只显示 13 个，第二行起被卡片裁掉 | `Style::wrap` 字段一直存在，但 `measure` / `layout_children` **都没实现它**：行容器仍按“全部子节点累加”算尺寸，高度只等于最高的一行 | 补两处换行实现（分行 + 逐行排布，行高取行内最高）；`tests/ui_layout_test.cpp` 钉住语义（含“无宽度上界时退化为单行”） |
| 43 | 图标表能换行了，但仍只显示两行 | 滚动容器被放进 Row 容器时，Row 给 grow 子节点的是**无宽度约束**（Flex 的 shrink-to-fit 语义），`kUnbounded` 一路传到 `wrap` 容器 → 误判成单行 | `ScrollView::arrange` 用**已确定的内容宽重测一次**内容（宽度定下来后才测） |
| 44 | `Slider::set_label` 设了却看不见，轨道位置也差一截 | 与第 26/38 条同一族：API 存在，但 `paint_content` 从未把 `label_` 画出来 | 补绘制（标题在左、百分比在右），并在 `measure`/`track_rect` 里为标题让出一条高度（无标题时几何不变） |
| 45 | 占位文字“改不了”：`set` 返回成功的 `changed: []` 但值不变 | `apply_properties` 自己维护一份可写属性白名单，与各组件声明的 `property_names()` **分裂**——读得到、改不了，而且不报错 | 属性名以**元素自己的声明**为准（白名单只作并集兜底）；`tests/ui_actions_test.cpp` 用“凡声明的都必须可写”当护栏 |
| 46 | 侧栏 5 个导航项里 4 个是死的（点了只换高亮） | 示例应用只建了 1 页，其余 4 页从未存在 | 拆出 `pages.cpp`：5 页用 `set_visible` 切换（隐藏页零布局零绘制、组件状态不丢）；切页同时复位滚动、清焦点 |
| 47 | 界面里“一半组件没人看过” | 画廊只有 1 页，组件写好了但没有展示面——**缺陷 41~45 全部是靠“把它们摆出来”才发现的** | 5 页画廊 + 72 图标全集（图形与名字并排才能看出方向错） |
| 48 | **`st test` 在 g++ 构建下启动即段错误**（`--list` 也崩；MSVC 下从未出现） | 测试注册表的互斥锁是**命名空间作用域的 `std::mutex`**，而 `ST_TEST` 生成的 `Registrar` 是其它 TU 的静态对象，在各自静态初始化阶段就调 `Registry::add` → 锁它：跨 TU 静态初始化顺序**未定义**（g++ 按链接顺序踩中未构造的临界区，MSVC 侥幸顺序正确） | 锁改为**函数内静态**（C++ 保证首次使用时构造，与调用方初始化顺序无关）；适配 g++ 构建时发现 |
| 49 | **MSVC 的 PCH 创建是静默空转**（日志“已生成”、消费端一律 C1083） | `cl` 对“.hpp 作为主文件”不报错而是**静默跳过**（D9024/D9027 + “没有执行操作”），而退出码仍是 0——只看退出码就把“没生成”报成“已生成” | 创建入口改用 `.cpp` 包装 + **产物存在性核对**（退出码靠不住的教训：判定成立足于事实而非代理信号） |
| 50 | 无头截图与窗口实际运行**是两种字、两种密度**（开发闭环的视觉判断失效） | 两侧**默认口径不同源**：文本（无头灰度+无拟合 vs 窗口 LCD+拟合）与 DPI（无头 1x vs 窗口系统值）都按“有没有窗口”分叉 | 默认统一（文本两侧同源；无头未显式指定时取系统显示缩放）；新增 `tools/st_consistency_check.py` 把两条不改量钉成可重测的检查 |

### 8.2.1 这批缺陷说明了什么

**方法论**：这五十条里没有一条能从"读代码"看出，全部来自「无头运行 + 控制通道观察 + 截图核验 + ASan 复跑」的闭环。
其中第 37~40 条（颜色误读、图标未接线、对比度不足、阴影只有一层）说明一件事：
**“美观”不是一个可以靠审美讨论解决的问题，而是一组可以被断言的性质**——
只有把像素剖出来、把对比度算出来，才能把“看着不舒服”变成“哪一行写错了”。

第 41~46 条则说明另一件事：**把东西“摆在一起看”本身就是一种测试**。
图标“转了个方向”在单个图标上根本看不出来（对钩交换后仍像对钩、放大镜仍是放大镜），
而把 72 个图标的**图形与名字并排**摆出来，异常一眼就能看出；
同理，“有 API 却不生效”的三个缺陷（43~45）也都是把界面用真鼠标真键盘点一遍才暴露的——
它们都不会报错，只会静默给人一个错误的结果。

第 30 条是个分水岭：这里错的是**构建系统**，而它的失败方式是“什么也不报，只是不重编”，
后果是 ABI 不匹配的随机制崩溃。所以它现在的防线是三道：源码解析的专项测试、
缓存键里的格式版本号、以及在文档里把这条路径标为“不得静默失败”。
第 31 条则是另一类陷阱：**事件被处理了（`handled=true`）、只是什么也没发生**——
“静默无效”比报错难发现一个数量级，只有当有人真的去滚一下才能发现。
两类问题共同的教训是：**“看起来能用”不足以当作验收，必须看真实像素与真实行为。**

第 17 条尤其值得记下：它是**引入成熟第三方库时新增的风险面**——“上游抛异常”与本框架
“不设通用异常边界、错误经 Result 返回”的契约正面冲突。对策不是“小心点”，而是
① 用一层薄封装把抛异常接口收敛掉，② 加一条 lint 规则把漏网写法机械拦住。
这也是霜天把"可被智能体驱动"作为一等需求的原因——**能被自动观察和操作的界面，才有资格被自动开发**。

第 48~50 条来自“换一个编译器”与“两个通道对账”这两件看似与功能无关的事，但它们指向同一条原则：
**换编译器是对“未定义行为/未声明假设”的最廉价审计**（48 是静态初始化顺序、49 是把退出码当代理信号），
而**“两个通道默认同源”是把开发闭环本身当作被测对象**（50）——前者只有真的换一个编译器才能暴露，
后者只有把截图与实机放在一起量才能证实。

### 8.3 GPU 渲染（D3D11：已全部落地）

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M1 | `Surface` 抽象缝 + 软件实现（零行为变化） | 已落地：测试数与截图**逐像素**比对均证明零变化 |
| M2 | D3D11 设备层（硬件→WARP）、离屏 RT、回读、探测上报 | 已落地：硬件 `RTX 4080 SUPER` 与 WARP 双路径 |
| M3a | 着色器管线：圆角 SDF、渐变、遮罩文字、位图、矩形/圆角裁剪 | 已落地：文字/渐变与软件 **Δ0** |
| M3b | 投影（遮罩 RT + 可分离盒式模糊 ×3 + 缓存） | 已落地：与软件 **Δ≤1** |
| M4 | 任意路径填充/描边/路径裁剪（CPU 覆盖率 → GPU 合成 + 缓存） | 已落地（**混合路径**，见下） |
| M5 | win32 换 DXGI swapchain 呈现 | **已落地**：送显 6.55ms → 0.03ms |
| M6 | `--renderer=auto\|gpu\|software` + 基准对比 | **已落地**：`auto` 实测选优（软件 47.5ms vs GPU 0.77ms） |
| M7 | 亚像素文字合成（`CoverageMaskLcd`：`R8G8B8A8` 覆盖率纹理 + **两遍混合**） | 已落地：与软件**同式**（`out_c = S_c·α_c + D_c·(1-a_s·α_c)`）。本机为 Linux（无 D3D11）→ 只能交叉编译 + 由 `tests/gpu_parity_test.cpp::gpu_subpixel_text_matches_software` 在 Windows 侧真跑；为此把「管线建不起来」变成**显式失败**：`capabilities()`/`create_canvas()` 会挡住着色器编译失败，避免「画不出来却看起来很快」 |

### 8.3.1 路径为什么是“CPU 光栅化 + GPU 合成”

任意路径要与软件光栅器**一致**只有两条路：

1. **GPU 三角化 + 解析 AA**：要把曲线三角化与边缘 AA 权重在 GPU 侧重写一份。
   而软件侧已实现这套且被大量回归测试钉住——重写的结果是**两套不可能一致的抗锯齿**，
   而一致性正是本项目的验收口径。
2. **复用同一套覆盖率光栅化做遮罩，交给 GPU 合成**（已选）。

选 2 的代价与收益都很清楚：正确性同源（同一个 `rasterize_mask`），且
**静态路径可整块缓存**——图标/自绘形状在帧间不变，缓存后每帧只剩一次纹理贴图，
而软件侧每帧都要重新光栅化。**因此路径上的 GPU 收益来自缓存与合成，不来自光栅化本身**；
这里不假装它是全 GPU 光栅化。

### 8.3.2 GPU / 软件的验收口径（容差，不是 bit-exact）

两者不可能逐像素相同（SDF 解析覆盖率 vs 扫描线覆盖率）。要求 bit-exact 等于设一个
不可能达成的目标，而那样的测试最终只会被人删掉。因此断言三件事：
几何同位、颜色量级一致、覆盖区域接近；并且**区分两类差异**：

- `differing_ratio`（差 >12）：主要来自边缘抗锯齿权重不同；
- `structural_ratio`（差 >64）：**内容不同**（形状错了/位置错了/通道写反了）。

实测（硬件路径）：

| 场景 | 结构性差异 | 最大通道差 |
|---|---|---|
| 文字（中英数混排） | **0.000%** | **Δ0** |
| 渐变（256px 线性） | **0.000%** | **Δ0** |
| 投影（3 组半径/模糊） | **0.000%** | **Δ≤1** |
| 实心圆角卡片 | 0.000% | Δ≤6（仅边缘环） |
| 真实控件组合界面 | **0.000%** | Δ≤150（仅边缘像素） |

**两个“假绿”教训**（都写进了测试注释）：
① 第一版对比用例传了 `nullptr` 文本端口 → 两侧都没画字 → “完美 0 差异”，
   而“文字在 GPU 上能画”因此毫无依据；② `ink_ratio` 的底色常量字节序写错
   （画布是 `0xRRGGBBAA`）→ 指标恒为 1.0、看着永远“通过”。
   现在用例会先断言字体真的加载、且底色对比真的有意义。

### 8.4 三维渲染：软件保底 + 系统高阶（原 OpenGL 方案已移除）

需求来源（**已修订**）：最初是"引入 opengl 源码，实现复杂图形和三维渲染能力"；
现在按**"框架层面统一接口、系统层面用系统最佳方案"**重做，见下。

### 8.4.1 决策：为什么移除 OpenGL

曾有过三个选项，实测与代码事实把答案逼得很清楚：

| 选项 | 结论 |
|---|---|
| 统一到 OpenGL | **两头不占**：Windows 上不如 D3D11（同一个厂商驱动，DXGI flip 呈现 0.03ms），别的平台又不存在（GL 实现是 Windows 专用、macOS 上 GL 已废弃）。要在 GLSL 重写 1651 行已验过的 D3D11 语义与它的等价性口径 |
| 保留 OpenGL 作为第三条腿 | GL 的"复杂图形"（模板缓冲 `fill_path`）**没有调用者**——2D 的路径填充早由软件光栅器做好（§4.2.3，且是 GPU 的等价性基准）。留着的代价是**两套不保证一致的复杂图形实现** |
| **软件保底 + 系统高阶**（已选） | 三维走平台中立接口：**软件实现保证跨平台可用**（已实现），系统高阶实现按**测量触发**（见 §8.4.2） |

因此删除了：`include/st/raster/gl.hpp`、`src/raster/platform_gl.cpp`、
`tests/gl_test.cpp`、`third_party/opengl/gl.h`（33 万字节的 glad 单头）。

**删之前先补了它占着的东西**（这是关键，否则就是净损失）：

| 原 GL 侧能力 | 现覆盖位置 |
|---|---|
| 深度/光照/相机投影 | `tests/raster_software_scene_test.cpp`（软件 3D） |
| 网格与 OBJ 加载 | `tests/raster_mesh_test.cpp`（平台中立） |
| **路径填充的非零环绕 / 带洞 / 自交** | `tests/raster_path_winding_test.cpp`（**原先只在 GL 侧被测**；软件侧是保证腿却缺覆盖，已补） |
| 描边只画轮廓 | 同上 |

### 8.4.2 平台现状与两条腿

入口是平台中立的 `raster::Scene3D`（`include/st/raster/scene3d.hpp`），
`ui::SceneView` 用它、不感知是哪条腿。

| 腿 | 实现 | 现状 |
|---|---|---|
| 软件 | `src/raster/software_scene.cpp`：自带 z-buffer、逐像素光照、近面裁剪、透视校正插值 | ✅ **已实现，所有平台可用**（`Scene3D::available()` 恒 true）——非 Windows 上三维不会消失 |
| 系统高阶 | `gpu::create_scene3d()`（D3D11，Windows） | ⏳ **接口位已留、实现待做**。见下方"触发条件"——**不是遗漏，是等数据** |

**关于系统高阶腿的触发条件**（避免"以后补"变成永远不补，也避免现在写用不上的代码）：

2D 的 GPU 腿有明确收益（实测软件 47.5ms vs GPU 0.77ms），所以当时就该做。
但 3D 的现状不同：界面里的三维是**一块卡片**（gallery 实测 960×200），
软件腿在这个尺寸下的成本是个位数毫秒级，而两种成本都与视图面积成正比、
都不随场景复杂度爆炸（软件侧按三角形包围盒裁剪）。

所以这条腿的**触发条件是测量**，不是感觉：

- 出现**真实**的三维场景使软件腿单帧 > 8ms（约 2D 整帧的一半），或
- 视图面积显著增大（例如全屏 3D），或
- 网格规模达到 5 万三角形以上且需要 60fps

在此之前写 D3D11 的 3D 实现属于**投机**：它会引入深度缓冲、独立的 3D 着色器、
顶点/索引缓冲上传、以及一次回读合成（`end_frame` 的契约是"交给一张图"，
GPU 结果要落到 `Surface` 仍要经过回读）——**成本确定，收益未证实**。
接口（`Scene3D`）已经把位置留好：加实现不需要改调用方。

两条腿必须**可对照**：`software_scene.cpp` 的光照公式与 GPU 侧保持一致，
否则"软件保底"就只是"另一种长相"。

### 8.4.2.1 历史记录：原 OpenGL 三维方案（已移除）

> 本节保留的是**已移除方案**的设计记录（为什么当时可行、为什么被删），
> 供以后评估 Vulkan/Metal 3D 腿时参考。现行方案见上方 §8.4.2「平台现状与两条腿」。

原方案用 WGL 离屏 FBO 实现无头 3D：WGL 上下文需要窗口句柄（HDC），但那个窗口
**从不显示**（`WS_POPUP`、不 `ShowWindow`），渲染全在 FBO 上——无头环境同样能渲染 3D，
与 D3D11 后端同一口径。入口点全部**动态解析**（不链接 `opengl32.lib`）。

移除原因（实测，非偏好）：在自己的主场（Windows）输给 D3D11，在其他平台又不存在
（macOS 上 GL 已废弃）——它既不是"保证腿"也不是"加分腿"。评估记录见 §8.4.1 末尾。

网格（立方体/球/长方体）**每面不同色 + 纬度渐变**：单色立方体转过 90° 看不出来，
于是"模型矩阵对不对"这类问题会被漏掉。光照（Lambert + 边缘光）是"看起来像 3D"的最小代价。

### 8.4.3 复杂图形：模板缓冲而非三角化

要在 GPU 上正确填充**带洞与自交**的路径，有两条路：

| 路线 | 问题 |
|---|---|
| 耳切/单调三角化 | 只对简单多边形有效；洞与自交要额外处理，而自交正是复杂图形的常见形态 |
| **模板缓冲 + 环绕累加**（已选） | 轮廓只需扇形展开；`INCR_WRAP`/`DECR_WRAP` 恰好就是**非零环绕**的定义 |

选模板法的决定性理由：它与软件光栅器**同语义**（非零环绕）——两者对同一条自交路径的结果一致，
才谈得上互相替代。`stroke_path` 也复用软件侧的 `stroke_to_path` 转轮廓，**"描边"的几何定义两边共用**。

### 8.4.4 两个只表现为"卡住/崩溃"的坑

1. **逐像素写 GPU 画布 = 每像素一次全屏回读**（历史：时为 `GlScene::composite`，现为 D3D11 合成器同理）。
   GPU 画布的 `set_pixel` 每次写入都触发一次全屏回读（2560×1600 = 16 MB）→ 192k 像素 = 192k 次回读
   → **主线程再也回不来**。
   症状迷惑：帧数**不增长**（卡在 `++frames` 之前），但控制通道照常应答（它在自己线程上）。
   修法是先在软件暂存画布上做一次转换、再**一次 blit**。
   回归用例必须**拿 GPU 画布当目标**——软件画布的 `set_pixel` 是 O(1)，用软件画布测永远暴露不了。

2. **`UiRoot::time_seconds_` 从未被赋值**（恒为 0）→ 所有基于时间的动画在真实应用里都走
   "静态帧直接落位"分支，即**动画完全不播**。单元测试自己构造 `RenderContext` 给了时间，所以一直没暴露。
   现在由 `Application::render_frame` 用**启动以来的单调时间**推进。

**教训**：帧耗时基准**看不见忙循环**（每帧都很快，只是停不下来）——必须直接测空闲 CPU。
另见 §8.3.2 的两个"假绿"测试。

### 8.5 验证体系（一个可被智能体开发的框架，自己也要可被验证）

| 层次 | 手段 | 命令 | 现状 |
|---|---|---|---|
| 单元测试 | 自研测试框架（`ST_TEST`/`ST_CHECK*`；`--list` 列用例、`--format junit` 出 CI 报告、per-case 超时护栏） | `st test` | 全绿（**483 用例**；g++ 与回退 MSVC 两侧同批结果，唯 1 个已登记的字形墨量阈值存量项待校准） |
| sanitizer | ASan + UBSan 全量复跑（UB 即 bug，不是"测试问题"） | `st test --san` | 零报告（需带 sanitizer 运行库的编译器；MinGW 发行版不带时构建前明确报错） |
| 内置通道一致性 | `tools/st_consistency_check.py`：窗口帧缓冲 vs 客户区实际像素（逐像素） + 无头 vs 窗口同参数（scale/文本形态/拟合/截图接近度） | `python tools/st_consistency_check.py`（Windows 真机） | 5 项全过（呈现 0.000%、同源项全等） |
| sanitizer | ASan + UBSan 全量复跑（UB 即 bug，不是"测试问题"） | `st test --san` | 零报告 |
| 禁令扫描 | **13 条**禁用特性规则（L1–L13；L8/L13 为作用域感知的专用检查）+ 文件布局 + 禁用 include | `st lint` | 0 违规（230 文件、6 处登记豁免） |
| 无头视觉 | `tools/st_visual_check.py`：dev/san × gallery/codeeditor 全序列（查询/操作/输入/主题/DPI 2x）+ 截图 + sanitizer 日志检查 | `python3 tools/st_visual_check.py` | 0 失败步 |
| 字体对照 | `tools/ft_compare.cpp`：用 FreeType 对照自研 CFF 解释器的轮廓数/包围盒（**仅测试用，不进框架构建**） | 手工编译运行 | 一致 |
| 文字抗锯齿对照 | `tools/lcd_compare.cpp`：同一段文字按 灰度/亚像素(滤波)/亚像素(原始) 各渲一张 PNG，并打印某个扫描行的边缘剖面（**仅验证用，不进框架构建**） | 手工编译运行（命令见文件头） | 见 §4.3.1 的实测表 |
| 小字锐度量尺 | `tools/stem_phase_probe.cpp`（竖笔画边缘相位与过渡带，支持 `--fit=normal` 对比拟合前后）、`tools/hinting_gain_probe.cpp`（用 FreeType 量各 hinting 档的网格对齐率）、`tools/grid_fit_report.cpp`（中间调占比 + 墨量变化） | 手工编译运行 | 见 §4.3.2 |
| 控制通道联调 | `tools/st_probe.py`（顺序序列）、`tools/st_shot_region.py`（区域高清截图）、`tools/st_gdb_probe.py`（崩溃复现 + 回溯）、`tools/st_project_check.py`（独立工程闭环：init→写码→构建→驱动→交叉编译）、`tools/st_win_check.py`（win32 窗口路径：wine+Xvfb 下真实键鼠/缩放/退出断言） | 手工运行 | — |
| 编辑器形态冒烟 | `tools/st_editor_smoke.py`：点击即聚焦（焦点链路）+ `input.text` 送达焦点元素 + 退格复原 + `FillViewport` 浮层铺满视口 + Esc 关闭 | `python3 tools/st_editor_smoke.py all` | 全通过（9 项断言） |

**为什么把"验证脚本"当交付物**：无头框架的正确性证据只能来自"跑起来看"。这几个脚本把
"启动 → 操作 → 截图 → 断言 → 收尾"固化成可重复命令，任何人（或任何智能体）改完代码都能一键复现同一套证据链。

## 9. 与歌白主库的集成

- 框架源码：仓库根 `shuangtian/`（与 `keqing/` 平级）；随包分发时由构建脚本物化到 `{GEBAI_HOME}/vendor/shuangtian/`（源码形态，含 `bootstrap.sh`，用户机器上 `st build` 即可，无需联网）。
- 子代理：`packages/agents/src/agents/shuangtian/`（TS 定义 + TCP 客户端工具集），工具命名空间 `shuangtian_*`；只读类免审，`set`/`invoke`/`input.*` 需审批。
- 二者协同：子代理是**操作面**，框架是**被操作面**；「协同开发」= 智能体用子代理在无头模式下驱动框架应用的开发与验证闭环。

## 10. 里程碑

> 状态以**代码与实测为准**，不以计划为准：写着"待做"却已完成的条目会误导读者
> （本表此前就把动画系统、win32 后端列为未来项，而它们都已完成）。

| 版本 | 内容 | 状态 |
|---|---|---|
| **v0.1** | core / codec / raster（含 **DPI 缩放**）/ text / md / ui（组件库）/ shell(headless) / control(TCP) / stpm / gallery + codeeditor / 子代理 / 文档 | ✅ 已完成 |
| **v0.1+（本期追加）** | ① **Windows 宿主 + g++（MinGW-w64）默认编译器**（自举 / 标志翻译 / 依赖追踪；MSVC 可回退）② **GPU 渲染全链路**（D3D11 设备层 → 着色器原语 → 路径 → **DXGI swapchain 呈现**，见 §8.3）③ **三维与网格**（平台中立的 `Scene3D` + **软件实现**：z-buffer / 逐像素光照 / 近面裁剪；`Mesh` 生成与 OBJ 加载；原 OpenGL 方案经评估后移除，见 §8.4）④ **动画与过渡**（悬浮特效、时间轴推进、续帧协议）⑤ 性能优化（整帧重绘 63.2→≈12 ms）⑥ **视口剔除**（屏幕外不再绘制）⑦ **内置通道与桌面一致性**（文本默认与 DPI 默认两侧同源，见 §4.3/§4.2.1）| ✅ 已完成 |
| **v0.1.5（画廊全场景）** | ① 画廊补齐 Dialog/Toast/Tooltip/TextArea/禁用态/表格选中行/实时统计卡（28 组件全部有可视化场景）② `Toast` 自动消失（帧时间轴）③ `Table` 选中行（视觉+语义+属性）④ `TextArea` 属性面补齐 ⑤ **叠加层 z 序修正**（浮层在景上）+ `find/query` 覆盖叠加层 ⑥ `SceneView` 逐帧调试输出移除 | ✅ 已完成 |
| v0.2 | 文本选择与复制、更多组件（日期选择、图表）、X11 / Wayland 窗口后端 | 待做 |
| v0.3 | Vulkan 合成后端、**图层缓存与局部重绘**（当前 `dirty_rect_` 只有整视口口径）、多窗口 | 待做 |
| v0.4 | 原生源码依赖生态（`st-packages` 索引）、`st publish` | 待做 |

## 11. 性能目标（v0.1 基线）

> **目标**是设计时的意图，**实测**是本机（RTX 4080 SUPER / 28 核 / g++（MinGW-w64））的真实数字。
> 没有实测的项如实留空——填一个"看起来达标"的猜测值比空着更糟。

| 项 | 目标 | 实测 |
|---|---|---|
| 稳态整帧重绘（1280×800，含文本） | < 12 ms（CPU 软件路径） | **≈12 ms**（优化前 63.2 ms；逐原语分解见 §4.2.8） |
| GPU 总帧（同上场景） | — | **1.53 ms**（优化前 24.66 ms） |
| GPU 送显 | — | **0.03 ms**（DXGI swapchain 前 6.55 ms，即 17×） |
| 渲染器选择 | 按实测选优 | `auto` 实测：软件 47.5 ms vs GPU 0.77 ms（1280×800 基准场景） |
| 构建：自举 / 增量 / 无改动 | — | 自举 ≈50 s；增量（改 1 单元）秒级；**无改动 dev 档 ≈1.6 s**（链接指纹命中直接跳过；此前每次重链 3.3 s） |
| 首帧（无头，1280×800） | < 40 ms | **≈2.8 ms（GPU）/ ≈13.3 ms（软件）**（2026-09-30 复测） |
| 脏区增量重绘 | < 3 ms | **≈1.9 ms**（2026-09-30 落地：悬停过渡帧 @2×DPI 软件，paint 38 ms → 1.86 ms；像素级等价测试见 `tests/ui_damage_test.cpp`） |
| 渐变填充（1200×36，debug bench） | — | **0.122 ms/块**（优化前 6.67 ms；纵向整行同色 + SIMD 整行混合，54×） |
| 控制命令延迟（无头空闲，ping） | — | **≈4.5 ms**（优化前 31.2 ms：示例 16 ms 硬编码睡眠 + Windows 15.6 ms 定时器粒度；见 `st/core/wait.hpp`） |
| 空闲 CPU（无头，30 s） | — | **≈0.6%（单核）** |
| 事件 → 画面更新延迟 | < 16 ms | *未测*（增量重绘后：本地命令到帧 ≈4.5 ms 命令延迟 + 帧内开销） |
| 字体：CJK 字形光栅化（首次） | < 2 ms/字 | **≈6.6 µs/字**（208 个生僻字首栅格化实测；缓存命中路径见 §4.2.8 剖析） |
| `tree`（1000 节点） | < 5 ms | *未测* |
| 内存（空应用） | < 20 MiB | *未测* |
