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
├── examples/{gallery,mdview}/
├── vendor/<name>/        # stpm vendor 固化的第三方源码（随仓库分发，离线可构建）
└── docs/                 # 控制协议规范、设计 token 表等
```

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
- 无任意代码执行入口（**没有** `eval`；只有数据与动作）。
- 控制通道的每次调用在应用侧日志留痕（`log` 级别 `info`，含 method 与耗时）。

## 7. 包管理（stpm）

### 7.1 定位
自研包管理器：**框架本体零第三方依赖**，但框架之上/后续引入的第三方**源码**由 stpm 统一管理——不依赖系统包管理器、不用 CMake/Make、不下载二进制（源码级 vendor 优先，保证可审计与可离线）。

### 7.2 清单 `st.pkg`（JSON）
```json
{
  "name": "shuangtian",
  "version": "0.1.0",
  "kind": "static_library",
  "cxx_standard": 20,
  "modules": ["core", "math", "codec", "raster", "text", "md", "ui", "shell", "gpu", "control", "app"],
  "include_dirs": ["include"],
  "sources": ["src/**/*.cpp"],
  "tests": ["tests/*_test.cpp"],
  "flags": ["-fno-strict-aliasing"],
  "defines": ["ST_VERSION=\"0.1.0\""],
  "targets": {
    "st": { "kind": "executable", "sources": ["tools/stpm/*.cpp"] },
    "gallery": { "kind": "executable", "sources": ["examples/gallery/*.cpp"] },
    "mdview": { "kind": "executable", "sources": ["examples/mdview/*.cpp"] }
  },
  "dependencies": {
    "modules": [],
    "source": [],
    "system": ["dl", "pthread"]
  }
}
```
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

### 7.5 构建图与直驱编译器
- `st build [target]`：解析清单 → 拓扑排序（依赖先编）→ 生成编译命令 → **直接调用 `g++`/`clang++`**（`-MMD -MF` 依赖文件 + 增量判新旧）。
- 产物：`build/<profile>/<target>/…`；`profile` ∈ `debug` / `release` / `san`。
- 并行：按 `nproc` 并行编译单元（自研 job 池，`std::jthread`）。

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
`st` 自身是 C++ 程序：`bootstrap.sh` 用最朴素的编译器调用把 `tools/stpm/*.cpp` + 所需 `src/core/*` 编成 `build/bin/st`（唯一非 st 构建入口）；此后一切（含 `st` 自身重建）由 `st build` 完成。

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

**方法论**：这十条里没有一条能从"读代码"看出，全部来自「无头运行 + 控制通道观察 + 截图核验 + ASan 复跑」的闭环。
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
