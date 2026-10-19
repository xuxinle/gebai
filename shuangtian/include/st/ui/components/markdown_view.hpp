#pragma once

/// Markdown 视图组件（面向大模型应用，`DESIGN.md` §4.5「约定二：原生 Markdown 渲染」）：
/// 把 `st::md` 的块模型渲染成自绘 UI 组件，支持**流式增量**（边生成边渲染）与滚动。
///
/// 设计要点
/// - **两趟渲染**：`measure` 把块序列排版成扁平行模型（`rows_`，内容坐标：x 相对内容左、
///   y 相对内容顶），`paint_content` 只按 `scroll_offset_` 平移绘制——画不改布局、滚不重排版；
/// - **流式**：内部用 `md::MdStream` 增量解析，块序列只增不改（前缀稳定 ⇒ 已渲染内容不跳变）；
///   `append_chunk` 可切在任意字节处（含 UTF-8 中间）；追加前若已贴底则追加后继续贴底；
/// - **配色取自主题**：标题/正文/行内码/代码块/列表/引用/表格的颜色与圆角全部来自 `Theme` token；
///   代码高亮的关键字/字符串/注释/数字由主题色 `mix` 派生（不硬编码色值）；
/// - **无字体亦可布局**：文本一律经 `TextPort`（`RenderContext::text`），为空时退化为
///   `NullTextPort`（宽度 0、绘制 no-op），排版流程照常不崩。
///
/// 已知受限（如实说明，`TextPort` 能力面所限）
/// - 端口只有「文本 + 字号 + 颜色」三个通道：**无字重/斜体**——Bold/SemiBold 以亚像素偏移
///   二次绘制近似（`draw_text_weighted`），斜体不倾斜（强调只体现在语义上）；
/// - **代码用等宽字体**（`FontStack` 的等宽库，探测不到时回退正文字体）：
///   代码块与行内码都走等宽——比例字体下 `i` 与 `M` 宽不同，缩进与列对齐会失真；
/// - `set_selectable` 本期只记录状态（选择/复制未实现，控制通道可读写该属性）；
/// - `HtmlBlock` 按 `md` 层约定原样保留、不渲染（也不进入语义文本）。

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/md/highlight.hpp"
#include "st/md/markdown.hpp"
#include "st/md/stream.hpp"
#include "st/ui/element.hpp"
#include "st/ui/components/slow_content.hpp"  // SlowLayoutSnapshot（异步排版快照）
#include "st/ui/line_layout.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// Markdown 渲染视图：`set_markdown` 全量替换 / `append_chunk` 流式追加，可滚动。
class MarkdownView : public Element {
 public:
  explicit MarkdownView(std::string markdown = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "MarkdownView"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Markdown; }

  /// 全量替换内容并**重置流式状态**（`MdStream::reset` 语义）与滚动位置。
  void set_markdown(std::string markdown);
  /// 流式追加一片原文（可切在任意字节处）：内部增量重建块序列并重排版。
  /// 追加前若已贴底（`stick_to_bottom`），追加后保持贴底。
  void append_chunk(std::string_view chunk);
  /// 当前累计原文（等于逐次 `append_chunk` 的拼接结果，可用于导出/复制）。
  [[nodiscard]] auto markdown() const -> std::string;
  /// 当前块数（流式过程中**单调不减**）。
  [[nodiscard]] auto block_count() const noexcept -> std::size_t;

  /// 是否可选中/复制文本（默认 **true**，2026-10-13 起选择真实生效：拖选行级
  /// 高亮 + Ctrl+C 复制选中文本；无选择时 Ctrl+C 复制全文）。
  void set_selectable(bool value);
  /// 当前选中的纯文本（无选择返回空；控制通道属性 `selection` 可读）。
  [[nodiscard]] auto selected_text() const -> std::string;

  /// **链接点击回调**（链接渲染为一等交互）。
  ///
  /// 框架只上报「用户点了哪个 URL」——打开浏览器是**宿主**的事
  ///（平台能力：Linux xdg-open / Windows ShellExecute / macOS open）。
  /// 这与 `HostFocus` 的剪贴板同一取向：组件不该知道平台后端长什么样。
  /// 未设回调时点击链接仍会高亮（可用性可见）但不做任何事。
  std::function<void(std::string_view url)> on_link_click{};
  /// 链接悬停态回调（进入/离开链接时各一次；参数为空 = 离开）。
  std::function<void(std::string_view url)> on_link_hover{};
  /// 当前悬停的链接 URL（空 = 无）。悬停时链接会加下划线强调。
  [[nodiscard]] auto hovered_link() const -> const std::string& { return hovered_link_; }
  /// 排版宽度上限（`<= 0` 表示不限制，取容器给的最大宽度）。
  void set_max_width(float width);
  /// 正文基准字号（`<= 0` 表示用主题 `font_base`）。
  void set_base_font_size(float size);
  /// 代码字号（`<= 0` 表示用主题 `font_sm`；端口无等宽能力，仅字号不同）。
  void set_code_font_size(float size);

  // —— 语义 / 控制通道 ——

  /// 纯文本（去 Markdown 标记，按块顺序、行序拼接）：便于 `find` / `wait` 定位内容。
  [[nodiscard]] auto semantics_text() const -> std::string override { return plain_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument) -> bool override;

  // —— 元素接口 ——

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  /// 滚轮滚动、PageUp/PageDown/Home/End/方向键（返回 true 表示已处理）。
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  /// 当前滚动偏移（px，已夹取到 `[0, max_scroll]`）。
  [[nodiscard]] auto scroll_offset() const noexcept -> float;
  /// 滚动到指定偏移（px，自动夹取）。
  void scroll_to(float offset);
  /// **全量**内容高度（px，含内边距；与视口无关，流式过程中单调增长）。
  [[nodiscard]] auto content_height() const noexcept -> float;

  // —— 命中测试（公开面：宿主可以做悬停光标/右键菜单/挂复制按钮）——

  /// 内容坐标处的**链接 URL**（无则空）。
  ///
  /// 与 `paint` 共用同一份排版结果（`rows_` 与 `Span::link`）——
  /// “画在哪里”与“点中哪里”不会脉滑。
  [[nodiscard]] auto link_at(math::Point content_point) const -> std::string;
  /// 内容坐标处的**代码块首行下标**（-1 = 不在代码块上）。
  /// 公开面：宿主可用它把“复制代码”按钮挂到对应块（组件不自己做工具栏）。
  [[nodiscard]] auto code_block_at(math::Point content_point) const -> int;
  /// 内容坐标处的**行盒**（空 = 越界）。
  [[nodiscard]] auto row_box_at(float content_y) const -> math::Rect;

  // —— 异步排版（SlowContent 壳的对接面，2026-10-19 性能轮）——

  /// 拍下当前排版的全部输入（源文本 + 字号 + 宽度）：后台排版的唯一事实来源。
  /// 主线程调用；返回值自包含，可跨线程。
  [[nodiscard]] auto snapshot_layout_input(float width) const -> st::ui::SlowLayoutSnapshot;
  /// 后台排版：在调用线程上用快照排出一份行模型（不碰本组件的任何状态）。
  /// 返回的快照携带行模型（不透明负载）与内容高；线程安全依赖 `TextPort`
  /// 的带锁缓存（并发压测见 tools/font_thread_safety_probe.cpp）。
  [[nodiscard]] static auto layout_offscreen(const SlowLayoutSnapshot& input,
                                             const std::stop_token& token)
      -> SlowLayoutSnapshot;
  /// 采纳后台结果（主线程）：换入行模型并标脏。宽度/字号与快照不符时拒绝（返回 false）。
  auto adopt_layout(const SlowLayoutSnapshot& snapshot) -> bool;

  /// 离屏排版冻结（壳路径用）：true 期间 `set_layout_width` 跳过重排。
  ///
  /// 为什么需要：壳路径下排版权在后台，但组件的 `measure`/`arrange` 仍会因
  /// 宽度变化调 `set_layout_width`——若不拦，它会用当前源**同步重排**一次
  ///（大文档几十毫秒，异步就白做了），随后还可能被 adopt 覆盖（宽度口径
  /// 不同时来回跳）。冻结期间宽度变化只记录，解冻或 adopt 后按最终宽度走。
  void freeze_offscreen(bool frozen) { offscreen_frozen_ = frozen; }

  /// 冻结期间收到的待排宽（内容宽；0 = 无待排）。壳每帧据此判要不要重投。
  [[nodiscard]] auto pending_layout_width() const noexcept -> float { return pending_width_; }

  /// 确保语义块就绪（懒解析；异步采纳后首次读语义时调一次）。
  /// 返回是否真的做了解析（诊断/量尺用）。
  auto ensure_semantic_blocks() const -> bool;

  /// 安装**排版取消信号**（后台排版用）：非空时 `append_blocks` 在块边界检查并早退。
  /// 只在离屏 worker 值对象上装（树上实例不装，它本身不在后台跑）。
  void set_layout_cancel(const std::stop_token* token) noexcept { layout_cancel_ = token; }

  /// 上一帧实际绘制的行区间（半开；从未绘制过时为空区间）。
  ///
  /// 为何补（与 `Tree`/`Table`/`Text` 同一姿态）：`MarkdownView` 有 `content_height()`
  /// 但**没有**“现在看得到哪几行”的读取口——而那是 AI 驱动界面时最常问的判断。
  /// 绘制期二分出的区间正好是答案，缓存上一帧的值就够（与 `CodeEditor` 同一手法）。
  [[nodiscard]] auto visible_row_range_now() const noexcept
      -> std::pair<std::size_t, std::size_t> {
    return {last_visible_first_, last_visible_last_};
  }
  /// 总行数（供视口断言与宿主估算用）。
  [[nodiscard]] auto row_count() const noexcept -> std::size_t { return rows_.size(); }

 private:
  /// 绘制颜色角色（布局期只记语义角色，`paint` 时按主题解析为具体色值）。
  enum class ColorRole : std::uint8_t {
    Text,
    Muted,
    Faint,
    Primary,
    Success,
    Accent,
    Warning,
    CodePlain,
    CodeKeyword,
    CodeString,
    CodeNumber,
    CodeComment,
    CodeType,
    CodeFunction,
    CodeBuiltin,
    CodeOperator,
    CodePreprocessor,
  };

  /// 行内片段（同一样式的连续文本）。
  struct Span {
    std::string text{};
    ColorRole color{ColorRole::Text};
    float size{14.0f};
    FontWeight weight{FontWeight::Regular};
    bool underline{false};
    bool strike{false};
    bool inline_code{false};
    /// 代码文本（代码块行与行内码）：用**等宽字体**渲染。
    ///
    /// 与 `inline_code` 分开：后者带“芯片底色”，而代码块行只要求字体。
    /// 等宽是代码可读性的前提——比例字体下 `i` 与 `M` 宽不同，缩进与列对齐全失真。
    bool code{false};
    /// 链接目标（非空 = 本片段是链接；点击回调与悬停下划线靠它）。
    ///
    /// 为何把它留在 Span 上：命中测试（“鼠标下的那个链接是哪个 URL”）
    /// 必须与绘制同源——另建一份“链接区间表”就是两套事实，迟早脱钩。
    std::string link{};
    /// 输入点在这个片段内的字节区间（相对于本 Span 的 `text`）。
    ///
    /// 为何要它：折行会把一个 Span 切成多份，而宿主需要知道“点击落在链接的
    /// 哪一段”（做 hover 高亮/同一行内多链接区分）；区间由排版期写入，
    /// 绘制期不用重算。
    std::size_t link_begin{0};
    std::size_t link_end{0};
    /// 等宽角色（由 `code` 推导的便捷取值）。
    [[nodiscard]] auto font_role() const noexcept -> st::text::FontRole {
      return code ? st::text::FontRole::Monospace : st::text::FontRole::Proportional;
    }
  };

  /// 表格单元格：**支持多行**（折行后的行文本列表）。
  ///
  /// 为何是多行而不是单行：表格单元格的自然宽差异极大（实测本仓 README：
  /// 一列 68 字符、另一列 8 字符），而可用宽有限——单行单元格只能二选一：
  /// 要么缩列把内容砍成 `Wind...`（旧行为，用户：“截断是灾难级的”），
  /// 要么横向溢出。浏器/主流 Markdown 渲染器的做法都是**单元格内折行**：
  /// 行高随最高单元格长，内容一字不丢。
  struct Cell {
    math::Rect box{};
    std::vector<std::string> lines{};   ///< 折行后的行文本（至少一行）
  };

  enum class RowKind : std::uint8_t { Text, Heading, ListRow, CodeLine, Divider, TableRow };
  enum class GroupKind : std::uint8_t { None, Code, Quote };

  /// 排版行：绘制所需的一切（内容坐标），不再依赖排版期的临时状态。
  struct Row {
    RowKind kind{RowKind::Text};
    math::Rect box{};          ///< 行区域（内容坐标）
    float line_height{0.0f};   ///< 行高（文本垂直居中基准）
    float text_x{0.0f};        ///< 文本起点 x（项目符号列之后）
    std::vector<Span> spans{};
    /// 本行**扁平纯文本 + 每码点前缀宽**，供**字符级选择**用。
    ///
    /// 为何要这份冗余：选择需要一个**单一坐标系**（字节偏移 ↔ x 可互推）。
    /// 若在 `spans` 上直接算，每个 Span 各自计时会有“同一行两种口径”的风险
    ///（折行/换行位置会静默错位），且**代码行以前根本不填 `span_x`**。
    /// 分工：选择与命中用 `flat`/`flat_prefix`，绘制仍用 `spans`（保留着色）。
    std::string flat{};
    /// `flat` 里每个**码点起始处**的 x（相对 `text_x`；长度 = 码点数 + 1）。
    ///
    /// **懒计算**（2026-10-19 性能轮）：`flat_ready` 为假时本向量尚未算出，
    /// 首次 `offset_to_x`/`caret_at` 时经 `ensure_flat_prefix` 补算并置真。
    /// 懒的理由：逐码点 `measure_width` 在大文档重排里占 ~27%（DESIGN.md 447KB
    /// 实测 97ms 里的 26ms），而选择/命中只有用户拖选时才发生——
    /// 排版是每帧路径、选择是罕见路径，重活该往罕见路径让。
    /// `mutable`：补算发生在 const 的命中/绘制路径上（与 `last_visible_*` 同一手法）。
    mutable std::vector<float> flat_prefix{};
    mutable bool flat_ready{false};
  /// 每个 Span 的 x 区间（与 `spans` 一一对应，仅链接片段有意义）。
  ///
  /// 为何在**排版期**写：命中测试（`link_at`）需要“鼠标 x 落在哪个 Span 里”，
  /// 而 `paint` 是 const、不能回写；且绘制期重算量宽会与排版期的折行结果脱钩
  ///（字体测量有累加误差）。排版期本来就逐词累加 x，顺手记下零成本。
  std::vector<std::pair<float, float>> span_x{};
    math::Rect clip{};         ///< 行级裁剪区（代码行/表格行；空表示不额外裁剪）
    GroupKind group{GroupKind::None};
    math::Rect group_rect{};   ///< 组矩形（代码块/引用块）
    bool group_first{false};   ///< 组内首行：背景/边框/语言标签在此行绘制一次
    /// 本组**首行的下标**（`group_first` 行的下标；非组行等于自身）。
    ///
    /// 为何要它（2026-10-14，实测修的一个**漏画**）：`paint_content` 现在用二分只遍历
    /// 「与视口相交的行」。而组背景画在**组首行**那次绘制里——当组首行已滚到视口上方时，
    /// 它不在区间内，背景就**整块不画**（实测：40 行代码块滚到中段后 code_bg 像素 = 0，
    /// 而非白像素也是 0，即整屏全空）。
    ///
    /// 有了这个下标，区间内的任一行都能拿到“本组首行”，从而在它落在区间外时补画。
    std::size_t group_head_row{0};
    std::string language{};    ///< 代码块语言标签
    math::Rect marker{};       ///< 项目符号区域（无序圆点/有序序号/任务框）
    bool marker_dot{false};
    bool task{false};
    bool checked{false};
    std::string marker_text{};  ///< 有序列表序号文本
    std::vector<Cell> cells{};
    bool table_header{false};
    bool table_top{false};
    bool table_bottom{false};
  };

  /// 递归排版时的当前帧（缩进与可用宽度随嵌套变化，垂直游标由调用方共享）。
  struct Frame {
    float indent{0.0f};
    float avail{0.0f};
    ColorRole role{ColorRole::Text};
    bool nested{false};
  };

  /// 已解析的排版参数（随主题刷新；仅当值变化才重排版——换主题配色不需要重排）。
  struct TextMetrics {
    std::array<float, 6> heading_sizes{};
    std::array<float, 6> heading_margins{};
    float base{14.0f};
    float code{13.0f};
    float table{12.0f};

    auto operator==(const TextMetrics& other) const noexcept -> bool = default;
  };

  // —— 内容与状态 ——
  st::md::MdStream stream_{};
  std::vector<st::md::Block> blocks_{};
  std::string plain_{};              ///< 去标记纯文本（语义通道）
  std::vector<Row> rows_{};          ///< 排版结果（`measure` 构建，`paint` 消费）
  TextMetrics metrics_{};
  const TextPort* layout_port_{nullptr};
  float layout_width_{0.0f};
  float content_height_{0.0f};
  /// 上一帧绘制的可见行区间（半开；`paint_content` 写，见 `visible_row_range_now()`）。
  /// `mutable`：`paint_content` 是 const（「绘制不改元素状态」是框架硬约束）。
   mutable std::size_t last_visible_first_{0};
 mutable std::size_t last_visible_last_{0};
 /// 诊断计数（`paint_content` 写，属性面 `draw_stats` 读）：本帧“遍历到的行数”
 /// 与“其中真的带了字的行数”。两者分家 = “行到了但字没画”；
 /// 是 2026-10-18 排查“滚动后大片空白”时加的唯一能区分两种成因的观测点。
 mutable std::size_t debug_rows_drawn_{0};
 mutable std::size_t debug_rows_with_text_{0};
 /// 本帧画了几次组背景（`paint_content` 写，属性面 `painted_groups` 读）。
 /// 必须 ≤ 本帧涉及的组数；缺陷实现下等于“组内行数”（每行重画整块背景）。
 mutable std::size_t debug_painted_groups_{0};
 float view_height_{0.0f};
  float scroll_offset_{0.0f};
  float max_width_{0.0f};
  float base_font_size_{0.0f};
  float code_font_size_{0.0f};
  bool needs_layout_{true};
  /// 离屏排版冻结（见 `freeze_offscreen`）：主线程壳持有排版权时置位。
  bool offscreen_frozen_{false};
  /// 后台排版取消信号（仅离屏 worker 实例装；见 `set_layout_cancel`）。
  const std::stop_token* layout_cancel_{nullptr};
  /// **懒解析的语义源**（2026-10-19）：异步采纳只拷源、不解析。
  ///
  /// 为何不直接喂 `blocks_`：`MdStream::feed` 对 447KB 是一次完整解析（主线程
  /// 实测 40~50ms），异步排版省下的时间会被它吃干净。而常见路径（纯看预览）
  /// 根本不需要 `blocks_`——只需要 `rows_`（后台已排好）。
  /// 真正要读语义（`markdown()`/选中文案/全文复制）时经 `ensure_semantic_blocks()`
  /// 按需解析一次并缓存。
  std::string semantic_source_{};
  /// 语义块是否已就绪（false 且 `semantic_source_` 非空 = 待懒解析）。
  mutable bool semantic_ready_{true};
  /// 冻结期间收到的**待排宽**（内容宽，0 = 无）。
  ///
  /// 壳每帧经 `pending_layout_width()` 读它判「宽度变了没有」，变了就带着新宽重投；
  /// adopt 后清。不混进 `layout_width_`：后者描述「当前 rows 是按多宽排的」
  /// （见 `set_layout_width` 的说明——两者混用会让坐标口径分裂）。
  float pending_width_{0.0f};
  bool want_bottom_{false};          ///< 追加时贴底，等排版完成后再落到实测高度
  bool stick_to_bottom_{true};
  bool selectable_{true};
  /// 当前悬停的链接（空 = 无）；悬停时链接额外加粗下划线强调。
  std::string hovered_link_{};

  // —— 文本选择（2026-10-13）——
  //
  // **字符级**选择：锚点与焦点都是 `{line, offset}`（line = `rows_` 下标，
  // offset = 该行 `flat` 里的**字节偏移**）。拖选时高亮精确到字符，
  // 复制时分行拼接选中片段（跨行时行间补 `\n`）。
  //
  // 为何不再用行级：用户实测“不好用”——行级无法选中一行内的部分文字，
  // 也无法从行中开始跨行选。字符级需要「位置 → 字节」的逆映射，
  // 靠 `Row::flat` + `flat_prefix` 提供（排版期算好，命中期不重量宽）。
  struct Caret {
    int line{-1};
    std::size_t offset{0};
    [[nodiscard]] auto valid() const noexcept -> bool { return line >= 0; }
  };
  Caret select_anchor_{};        ///< 拖选起点（按下的那一端）
  Caret select_focus_{};         ///< 拖选终点（跟随鼠标的那一端）
  bool selecting_{false};        ///< 拖选中（MouseMove 继续更新 focus）
  bool has_selection_{false};    ///< 是否存在非空选择（两端相等 = 无）

  /// 选择区间的**有序**两端（begin ≤ end）。
  [[nodiscard]] auto selection_ends() const -> std::pair<Caret, Caret>;
  /// 命中：内容坐标 → 最近的车符位（行内按 x 找到最近的字节偏移）。
  [[nodiscard]] auto caret_at(math::Point point) const -> Caret;
  /// 行内字节偏移 → x（相对该行 `text_x`）；靠 `flat_prefix` 二分。
  [[nodiscard]] auto offset_to_x(const Row& row, std::size_t offset) const -> float;
  /// 懒补算一行的 `flat_prefix`（首次选择/命中时调用；幂等）。
  ///
  /// 口径与旧排版期实现**同一份**（逐码点 `measure_width` 累加、跨 Span 续传起点 x）
  /// ——两处口径分家会出现「高亮与选择错位」，那是比慢更难查的错。
  void ensure_flat_prefix(const Row& row, const TextPort& port) const;

  // —— 排版 ——
  void rebuild_rows(const RenderContext& context, float width);
  /// 按**给定控件宽**重排内容（`measure` 与 `arrange` 共用唯一入口）。
  void set_layout_width(const RenderContext& context, float width);
  /// 本组件想要的排版宽（`max_width` → `available_width` → 兑底常量）。
  [[nodiscard]] auto desired_width(const Constraints& constraints) const noexcept -> float;
  /// 一行的垂直几何（**全部委托框架原语 `layout_line`**，见定义处的说明）。
///
/// ⚠ `role` 必须与**该行文字实际绘制时用的角色一致**：`layout_line` 拿它去量参考
/// 样本的 ascender 来定基线，而不同字体面的 ascender 差得多
///（实测同字号下拉丁 11.14 / 中文 15.87）——角色传错就是**整行文字偏移**
///（实测：正文传成 Monospace 后文字比高亮低约 12px）。
[[nodiscard]] auto line_geometry(const TextPort& port, float size, float line_height,
                               text::FontRole role) const -> LineGeometry;
  /// 控件坐标 → 内容坐标（绘制/命中/高亮三路**共用的唯一变换**）。
  [[nodiscard]] auto content_point_of(math::Point control_point) const -> math::Point;
  /// 行下标 ↔ 内容 y 的互查（选择/滚动用；越界/空白区返回最近行）。
  [[nodiscard]] auto row_at_y(float content_y) const -> int;
  /// 高亮选区（paint 里画在文本层之下，**逐行分段**：每行的选中区间独立算）。
  ///
  /// 需要 `port`（算字形**墨迹区**——高亮的垂直范围取墨迹不取行盒，见定义处的说明）。
  void paint_selection(const RenderContext& context, raster::Surface& canvas,
                       const struct Palette& colors) const;
  void append_blocks(const std::vector<st::md::Block>& blocks, const RenderContext& context,
                     const Frame& frame, float& y);
  void append_heading(const st::md::Block& block, const RenderContext& context, const Frame& frame,
                      float& y);
  void append_paragraph(const std::vector<st::md::Inline>& inlines, const RenderContext& context,
                        const Frame& frame, float& y, float size, float line_height,
                        FontWeight weight, RowKind kind);
  void append_code_block(const st::md::Block& block, const RenderContext& context,
                         const Frame& frame, float& y);
  void append_list(const st::md::Block& block, const RenderContext& context, const Frame& frame,
                   float& y);
  void append_quote(const st::md::Block& block, const RenderContext& context, const Frame& frame,
                    float& y);
  void append_table(const st::md::Block& block, const RenderContext& context, const Frame& frame,
                    float& y);
  void append_divider(const Frame& frame, float& y);
  /// 折行并压入行序列（按可用宽度贪婪折行；ASCII 词为断点、非 ASCII 逐码点可断；
  /// 单词超宽则按码点硬拆——超长 URL 不越界、不丢内容）。
  void push_spans(std::vector<Span> spans, const RenderContext& context, float x, float avail,
                  float line_height, RowKind kind, float& y);
  [[nodiscard]] auto inline_spans(const std::vector<st::md::Inline>& inlines, ColorRole role,
                                  float size, FontWeight weight) const -> std::vector<Span>;
  [[nodiscard]] auto code_line_spans(std::string_view code, std::size_t begin, std::size_t end,
                                     const std::vector<st::md::Token>& tokens,
                                     std::size_t& token_index, float size) const -> std::vector<Span>;

  // —— 语义文本 ——
  void rebuild_plain_text();
  void append_plain(const std::vector<st::md::Block>& blocks, std::uint32_t depth);

  // —— 滚动 ——
  [[nodiscard]] auto max_scroll() const noexcept -> float;
  [[nodiscard]] auto clamp_scroll(float offset) const noexcept -> float;
  [[nodiscard]] auto at_bottom() const noexcept -> bool;
  void settle_scroll();

  // —— 绘制（`paint_content` 的分工；颜色在此时按主题解析）——
  [[nodiscard]] static auto role_of_token(st::md::TokenKind kind) -> ColorRole;
  /// 取色。非 static：正文角色（`ColorRole::Text`）要读本元素的显式文字色覆盖
  ///（`text_color_override`/`text_tone_override`，DSL `BoxProps` 的落点）——
  /// 没有 this 拿不到；两个调用点都在 `paint_content` 体系的成员函数里。
  [[nodiscard]] auto color_of(const Theme& theme, ColorRole role) const -> math::Color;
  /// 绘制文本（端口无字重通道：Bold/SemiBold 以亚像素偏移二次绘制近似）。
  static void draw_text_weighted(const TextPort& port, raster::Surface& canvas,
                                 std::string_view text, math::Point origin, float size,
                                 math::Color color, FontWeight weight,
                                 text::FontRole role = text::FontRole::Proportional);
  /// 顺序绘制行内片段（超宽时以省略号收尾）。
  void draw_spans(const RenderContext& context, raster::Surface& canvas,
                  const std::vector<Span>& spans, math::Point start, float max_right,
                  float line_height) const;
  /// 组背景（代码块：底色 + 边框 + 语言标签；引用：底色 + 左侧竖条），组内首行绘制一次。
  void paint_group(const RenderContext& context, raster::Surface& canvas, const Row& row,
                   math::Point origin) const;
  /// 项目符号（无序圆点 / 有序序号 / 任务方框）。
  void paint_marker(const RenderContext& context, raster::Surface& canvas, const Row& row,
                    math::Point origin) const;
  /// 表格行（表头底色、网格线、单元格文本）。
  void paint_table_row(const RenderContext& context, raster::Surface& canvas, const Row& row,
                       math::Point origin) const;
};

}  // namespace st::ui
