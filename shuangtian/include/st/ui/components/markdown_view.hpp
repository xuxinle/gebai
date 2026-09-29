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
/// - **无等宽**能力：代码用同字体 + 稍小字号（默认 `font_sm`），非真等宽；
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

  /// 预留：是否可选中文本（本期仅记录状态；控制通道可读写）。
  void set_selectable(bool value);
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
  };

  /// 表格单元格（文本已在布局期按列宽省略）。
  struct Cell {
    math::Rect box{};
    std::string text{};
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
    math::Rect clip{};         ///< 行级裁剪区（代码行/表格行；空表示不额外裁剪）
    GroupKind group{GroupKind::None};
    math::Rect group_rect{};   ///< 组矩形（代码块/引用块）
    bool group_first{false};   ///< 组内首行：背景/边框/语言标签在此行绘制一次
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
  float view_height_{0.0f};
  float scroll_offset_{0.0f};
  float max_width_{0.0f};
  float base_font_size_{0.0f};
  float code_font_size_{0.0f};
  bool needs_layout_{true};
  bool want_bottom_{false};          ///< 追加时贴底，等排版完成后再落到实测高度
  bool stick_to_bottom_{true};
  bool selectable_{false};

  // —— 排版 ——
  void rebuild_rows(const RenderContext& context, float width);
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
  [[nodiscard]] static auto color_of(const Theme& theme, ColorRole role) -> math::Color;
  /// 绘制文本（端口无字重通道：Bold/SemiBold 以亚像素偏移二次绘制近似）。
  static void draw_text_weighted(const TextPort& port, raster::Surface& canvas,
                                 std::string_view text, math::Point origin, float size,
                                 math::Color color, FontWeight weight);
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
