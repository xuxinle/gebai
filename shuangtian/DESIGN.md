# 霜天（Shuangtian）— 歌白内置原生桌面应用框架 · 设计

> 霜天：取自「霜天曉角」——清冽、开阔、万物自明。定位是**歌白的原生躯体**：跨平台、全自绘、软硬件渲染兼容、可在无桌面环境完整运行（无头），并对外暴露一条 **TCP 控制通道**，让智能体可以像操作浏览器一样操作原生应用（组件、视觉元素、键盘鼠标、截图）。
>
> 编写约定见 `CONVENTIONS.md`（**全现代 C++20，禁用易错特性，三重强制**）。

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
            │ 绘制显示列表（DisplayList：fill/round_rect/gradient/text/shadow/image）      │ 事件
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

**分层依赖单向**：`core ← codec/math ← raster ← text ← md ← ui ← app`；`shell` 依赖 `core`+`raster`（把输入事件喂给 app）；`gpu` 依赖 `raster`（消费 Framebuffer）；`control` 依赖 `ui`+`app`（只读语义树 + 派发事件）；`pkg` 只依赖 `core`+`codec`（清单/求解/归档）。

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
├── examples/{gallery,mdeditor,codeeditor}/
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
5. **非整数 DPI 支持**：缓冲尺寸 `round(logical × scale)`（如 1.5x 下 101 逻辑宽 → 152 物理宽），逻辑尺寸由物理尺寸反算，不做累积取整。
6. **运行时切换**：`app.set_scale`（控制通道）/`Application::set_device_scale` 重建帧缓冲 + 清空字形缓存 + 全量标脏；无需重启。
7. **像素访问语义**：`pixel_at/set_pixel/pixels()/content_bounds()` 是**物理**口径（缓冲/编码视角）；逻辑口径用 `pixel_at_point()/content_bounds_logical()`。
8. **截图**：`capture` 输出的是物理分辨率 PNG（`scale=2` 的 1280×800 视口导出 2560×1600），协议回包带 `region` 与 `screen.scale` 便于反查。

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

### 4.2.3 语法高亮与代码编辑器（`st/text/highlight` + `ui::CodeEditor`）

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
- 缓存：字形轮廓、字形位图、整形结果（按 文本+字号+字体栈 键）。
- 回退链默认：`ST_FONT_LATIN` / `ST_FONT_CJK` / 系统探测（`/usr/share/fonts`、`C:\Windows\Fonts`、`/System/Library/Fonts`）。

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
  virtual void paint(DisplayList&) const = 0;
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
组件库（`include/st/ui/components/*.hpp`）：`Text` `Icon` `Button` `IconButton` `Link` `Input` `TextArea` `Checkbox` `Radio` `Switch` `Slider` `Select` `Dropdown` `Menu` `Tabs` `SegmentedControl` `Table` `List` `ScrollView` `ScrollBar` `ProgressBar` `Spinner` `Badge` `Avatar` `Chip` `Card` `Panel` `Divider` `Dialog` `Toast` `Tooltip` `TreeView` `Sparkline` `BarChart` `MarkdownView`。
- 布局：自研 flex 子集（`direction`/`gap`/`padding`/`margin`/`grow`/`shrink`/`align`/`justify`/`wrap`/百分比/固定尺寸/自适应内容）。
- 样式：`Style` 结构体 + `Theme`（token 表）；状态 `:hover`/`:active`/`:focus`/`:disabled`/`:selected` 由组件按 token 插值。
- 图标：自绘矢量路径集（`IconName` + 路径数据），零位图资源、任意缩放清晰。

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
- `x11`/`win32`/`wayland`：`dlopen`/`GetProcAddress` 运行时绑定，不产生链接期依赖。

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
| `tree` | `{root?, depth?, include_hidden?, max_nodes?}` | `{nodes:[...], truncated}` | 组件树快照（id/type/role/bounds/text/value/state/flags/children） |
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
| `metrics` | — | `{backend, headless, uptime_ms, frames, fps, frame_ms:{p50,p95}, dirty_ratio, nodes, allocations}` | 运行时指标 |
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

## 6.6 扩展层（`ext`）：JSON 与脚本

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

## 6.7 脚本控制组件（`ui::ScriptHost`）

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
    "mdeditor": { "kind": "executable", "sources": ["examples/mdeditor/*.cpp"] },
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
| **预编译头（PCH）** | `include/st/pch.hpp` 汇总高频标准库与核心头 → 一次编译为 `build/<profile>/pch/st.hpp.gch`，其后每个 TU 以 `-include st.hpp` 复用（GCC 按 `-I` 命中 `.gch`） | 消除重复头部解析（首选加速项） |
| **并行编译** | 自研 `ThreadPool` 按 `nproc` 并行调度翻译单元（`--jobs N` 可覆盖） | 8 核约 6~8× |
| **头依赖增量** | 每个 TU 产出 `-MMD -MF <obj>.d`，重建判定取「源文件 + **全部被包含头文件**」的 mtime 最大值——改一个头文件只重编真正受影响的单元 | 增量构建从"全量重编"变为"精确重编" |
| **分档构建** | `dev`（-O1 -g，日常迭代）/ `debug`（-O0 -g）/ `release`（-O2 -DNDEBUG）/ `san`（ASan+UBSan） | 开发档编译时间约减半 |

`st build` 结束会打印耗时构成（编译 / 链接 / 总时长 / 并行度 / 是否使用 PCH），便于回归对比。
`--no-pch` 可关闭预编译头用于排查（例如 PCH 与某编译选项冲突时）。

**并行度与内存峰值（踩过）**：翻译单元的内存占用差了两个数量级——
`third_party/quickjs/quickjs.c` 单文件 6.5 万行，在 `-O1 -g` 下编译峰值可达 GB 级，
而普通单元只有几十 MB。于是"28 路并行"在**受限容器**里会撞内存墙：
实测在 8 GiB cgroup 的容器中全量 `san` 档（ASan/UBSan 插桩，编译期内存更高）
被 OOM killer 杀掉 `cc1plus`，表现为莫名其妙的链接错误（`.Lubsan_data` 未定义——
其实是 `.o` 被写了一半）。对策：

- 受限环境用 `-j N` 控制并发（8 GiB 内存下全量 san 建议 `-j 6`）；宿主机 100 GB+ 内存时 28 路无压力；
- 真正的根治是"按单元大小限流"（大单元少并发），已在 self_optimize backlog 登记（#7）。

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
| `st test [filter] [--san]` | 构建并运行单测（含 sanitizer 档） |
| `st lint [--explain <rule>]` | 禁令静态扫描（`CONVENTIONS.md` §8） |
| `st add <spec>` / `st remove <name>` | 依赖增删（改清单 + 重求解 + 写 lock） |
| `st fetch` / `st sync` | 获取依赖 / 同步 lock |
| `st vendor` / `st vendor --offline` | 固化第三方源码到 `vendor/` / 校验离线可构建 |
| `st tree` / `st audit` / `st outdated` | 依赖树 / 校验和与许可证字段复核 / 版本检查 |
| `st doctor` | 环境自检（编译器、字体、显示后端、GPU、TLS、缓存） |

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
st doctor                                 # 探测清单声明的工具链是否真的装了
```

**关键设计点**：

| 点 | 理由 |
|---|---|
| 目录隔离 `build/<档位>-<工具链>/` | PCH 是按"编译器 + 目标"生成的；对象/嵌入生成物混用会得到难解的编译错误 |
| 系统库**整体接管**（工具链声明了就不追加本机默认） | 一份清单服务多平台，"本机需要哪些库"对目标可能是错的甚至不存在 |
| 交叉编译不套 `-fuse-ld=lld/mold` | 宿主装的链接器不一定支持目标格式 |
| `st test --toolchain` 明确拒绝执行 | 交叉产物无法在宿主运行；报"请在目标平台运行"比 `Exec format error` 可读 |
| C 源沿用同一编译器 + `-x c` | 不引入第二套工具链（mingw 的 `g++ -x c` 即可编 QuickJS） |

> 顺带：交叉编译是**平台分支的强制验证手段**。`CONVENTIONS §10` 因此规定
> "改了平台分支就要交叉编译一次"——这是唯一能发现 Windows 分支问题的途径。

## 8. 无头开发工作流（Linux 服务器，无桌面）

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

## 8.1 示例项目（用真实应用检验框架）

框架自带两个**功能完整**的示例应用（`examples/`），分别对应"能力面"与"应用面"：

| 示例 | 定位 | 检验点 |
|---|---|---|
| `gallery` | **组件集 / 设计系统巡检**：导航、统计卡、按钮矩阵、图标墙、表单、列表、进度条、主题与 DPI 切换、截图 | 组件库完整度、设计令牌一致性、DPI 正确性、控制通道可达性 |
| `mdeditor` | **Markdown 编辑器**：工具栏、大纲、编辑区、实时预览、语法高亮源码视图、可拖拽分栏、实时统计、文件保存/打开对话框、**流式生成演示**（模拟 LLM token 流逐块喂给预览） | **完备性**（编辑→解析→高亮→预览→读写→对话框→主题→DPI→控制通道全链路）、**易用性**（一个可用编辑器 ≈ 数百行组装代码）、**高阶定制**（四个自绘组件全部只依赖 `Element` 的四个扩展点） |

`mdeditor` 里的四个定制组件是"高阶定制能力"的实证：
`SourceView`（带行号与 Markdown 语法着色的源码视图）、`OutlinePanel`（解析 `st::md` 语法树得到可点击大纲）、
`SplitHandle`（可拖拽分栏）、`StatsBar`（实时字数/词数/行数/块数与阅读时长）。
它们**没有改动框架任何一行代码**——只用 `measure`/`arrange`/`paint_content`/`on_event` 四个扩展点，
复用同一套主题令牌与文本端口，即获得与内置组件一致的视觉语言。这正是"组合式扩展点 + 统一令牌"的设计目标。

## 8.2 实战缺陷与修复（无头协同开发暴露的真实问题）

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

**方法论**：这十六条里没有一条能从"读代码"看出，全部来自「无头运行 + 控制通道观察 + 截图核验 + ASan 复跑」的闭环。

第 17 条尤其值得记下：它是**引入成熟第三方库时新增的风险面**——“上游抛异常”与本框架
“不设通用异常边界、错误经 Result 返回”的契约正面冲突。对策不是“小心点”，而是
① 用一层薄封装把抛异常接口收敛掉，② 加一条 lint 规则把漏网写法机械拦住。
这也是霜天把"可被智能体驱动"作为一等需求的原因——**能被自动观察和操作的界面，才有资格被自动开发**。

## 8.3 验证体系（一个可被智能体开发的框架，自己也要可被验证）

| 层次 | 手段 | 命令 | 现状 |
|---|---|---|---|
| 单元测试 | 自研测试框架（`ST_TEST`/`ST_CHECK*`，102 用例 / 2018 断言） | `st test` | 全绿 |
| sanitizer | ASan + UBSan 全量复跑（UB 即 bug，不是"测试问题"） | `st test --san` | 零报告 |
| 禁令扫描 | 11 条禁用特性规则 + 文件布局 + 禁用 include | `st lint` | 0 违规（7 处登记豁免） |
| 无头视觉 | `tools/st_visual_check.py`：dev/san × gallery/mdeditor 全序列（查询/操作/输入/主题/DPI 2x）+ 截图 + sanitizer 日志检查 | `python3 tools/st_visual_check.py` | 0 失败步 |
| 字体对照 | `tools/ft_compare.cpp`：用 FreeType 对照自研 CFF 解释器的轮廓数/包围盒（**仅测试用，不进框架构建**） | 手工编译运行 | 一致 |
| 控制通道联调 | `tools/st_probe.py`（顺序序列）、`tools/st_shot_region.py`（区域高清截图）、`tools/st_gdb_probe.py`（崩溃复现 + 回溯） | 手工运行 | — |

**为什么把"验证脚本"当交付物**：无头框架的正确性证据只能来自"跑起来看"。这几个脚本把
"启动 → 操作 → 截图 → 断言 → 收尾"固化成可重复命令，任何人（或任何智能体）改完代码都能一键复现同一套证据链。

## 9. 与歌白主库的集成

- 框架源码：仓库根 `shuangtian/`（与 `keqing/` 平级）；随包分发时由构建脚本物化到 `{GEBAI_HOME}/vendor/shuangtian/`（源码形态，含 `bootstrap.sh`，用户机器上 `st build` 即可，无需联网）。
- 子代理：`packages/agents/src/agents/shuangtian/`（TS 定义 + TCP 客户端工具集），工具命名空间 `shuangtian_*`；只读类免审，`set`/`invoke`/`input.*` 需审批。
- 二者协同：子代理是**操作面**，框架是**被操作面**；「协同开发」= 智能体用子代理在无头模式下驱动框架应用的开发与验证闭环。

## 10. 里程碑

| 版本 | 内容 |
|---|---|
| **v0.1（本期）** | core / codec / raster（含 **DPI 缩放**）/ text / md / ui（组件库）/ shell(headless + 运行时 dlopen 桩) / gpu（探测 + 回退）/ control(TCP) / stpm / gallery + mdview / 子代理 / 文档 |
| v0.2 | 动画与过渡系统、文本选择与复制、更多组件（日期选择、图表）、X11/Win32 后端实测打磨 |
| v0.3 | Vulkan 合成后端实测、图层缓存与局部重绘优化、DPR/多窗口 |
| v0.4 | 原生源码依赖生态（`st-packages` 索引）、`st publish` |

## 11. 性能目标（v0.1 基线）

| 项 | 目标 |
|---|---|
| 首帧（无头，1080×720） | < 40 ms |
| 稳态整帧重绘（1080×720，含文本） | < 12 ms（CPU 软件路径） |
| 脏区增量重绘 | < 3 ms |
| 事件 → 画面更新延迟 | < 16 ms |
| 字体：CJK 字形光栅化（首次） | < 2 ms/字（之后缓存命中 < 50 µs） |
| `tree`（1000 节点） | < 5 ms |
| 内存（空应用） | < 20 MiB |
