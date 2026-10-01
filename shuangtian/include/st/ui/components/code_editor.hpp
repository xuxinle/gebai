#pragma once

/// 代码编辑器组件：**可编辑 + 语法高亮 + 行号 + 选择/撤销**，面向"文件编辑器"场景。
///
/// 能力
/// - **语法高亮**：内置 30 种主流语言（C/C++/Rust/Go/Python/JS/TS/Java/C#/Kotlin/Swift/Ruby/
///   PHP/Lua/SQL/HTML/XML/CSS/SCSS/JSON/YAML/TOML/INI/Shell/Dockerfile/Makefile/CMake/
///   Markdown/Diff/Protobuf…，按语言名/别名/扩展名识别）；
/// - **自定义语言**：`set_language_spec()` 直接绑定一份自定义规则，或向进程注册表
///   （`text::global_languages().register_language()`）注册后按名字使用——两条路径走同一台扫描器；
/// - **编辑**：插入/删除、UTF-8 光标移动（含 Ctrl+词移动）、Home/End、跨行选择（鼠标拖选 /
///   双击选词 / 三击选行 / Shift+方向键）、全选、复制剪切粘贴、撤销重做、Tab 缩进/
///   Shift+Tab 反缩进、Ctrl+/ 注释切换、回车自动缩进、括号配对高亮；
/// - **只读模式**：`set_read_only(true)` 即"带高亮的代码查看器"（选择与复制仍可用）；
/// - **滚动**：垂直 + 水平（代码不折行），行号槽固定；
/// - **控制通道**：属性面（text/language/cursor/selection/read_only/…）与动作
///   （focus/select_all/undo/redo/goto_line/insert/copy/cut/paste/comment）齐全，
///   智能体可据此读写编辑器内容。
///
/// 坐标系：全部为**逻辑像素**（与协议一致）；文本索引为 **UTF-8 字节偏移**且落在码点边界。

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/text/highlight.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class CodeEditor : public Element {
 public:
  CodeEditor();
  ~CodeEditor() override;
  CodeEditor(const CodeEditor&) = delete;
  auto operator=(const CodeEditor&) -> CodeEditor& = delete;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "CodeEditor"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Code; }

  // —— 内容与外观 ——

  /// 设定全文；光标与选择会夹取到合法位置，撤销栈清空。
  void set_text(std::string text);
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  /// 按语言名（规范名/别名/扩展名）设定高亮语言；空串 = 关闭高亮。
  void set_language(std::string language);
  [[nodiscard]] auto language() const noexcept -> const std::string& { return language_; }
  /// 直接绑定一份自定义语言规则（优先于 `language_`；传空规则 = 回到按名字查表）。
  void set_language_spec(text::LanguageSpec spec);
  /// 从文件路径推断语言（等价于 `set_language(*language_from_path(path))`）。
  void set_language_from_path(std::string_view path);
  /// 只读：仍可选中/复制，但拒绝一切修改（含粘贴与动作）。
  void set_read_only(bool value) {
    read_only_ = value;
    mark_dirty();
  }
  [[nodiscard]] auto read_only() const noexcept -> bool { return read_only_; }
  void set_show_line_numbers(bool value) {
    show_line_numbers_ = value;
    mark_layout_dirty();
  }
  [[nodiscard]] auto show_line_numbers() const noexcept -> bool { return show_line_numbers_; }
  void set_highlight_enabled(bool value) {
    highlight_enabled_ = value;
    mark_highlight_dirty();
  }
  [[nodiscard]] auto highlight_enabled() const noexcept -> bool { return highlight_enabled_; }
  void set_tab_width(int width);
  [[nodiscard]] auto tab_width() const noexcept -> int { return tab_width_; }
  /// 插入空格而非制表符（默认 true）。
  void set_insert_spaces(bool value) noexcept { insert_spaces_ = value; }
  [[nodiscard]] auto insert_spaces() const noexcept -> bool { return insert_spaces_; }
  void set_font_size(float size);
  [[nodiscard]] auto font_size() const noexcept -> float { return font_size_; }

  // —— 光标与选择 ——

  [[nodiscard]] auto cursor_index() const noexcept -> std::size_t { return cursor_; }
  void set_cursor_index(std::size_t index);
  /// 选择区间 `[begin, end)`（无选择时 `begin == end == cursor`）。
  [[nodiscard]] auto selection() const -> std::pair<std::size_t, std::size_t>;
  [[nodiscard]] auto has_selection() const noexcept -> bool { return anchor_ != cursor_; }
  [[nodiscard]] auto selected_text() const -> std::string;
  void select_all();
  void clear_selection() { anchor_ = cursor_; }
  /// 选中给定区间（自动按码点边界夹取）。
  void set_selection(std::size_t begin, std::size_t end);
  [[nodiscard]] auto cursor_line() const -> std::size_t;
  [[nodiscard]] auto cursor_column() const -> std::size_t;
  [[nodiscard]] auto line_count() const -> std::size_t;
  /// 跳到第 `line` 行（从 1 起）并把该行滚入视野。
  void goto_line(std::size_t line);

  // —— 编辑 ——

  /// 在光标处插入文本（有选择时先替换）。
  void insert_text(std::string_view inserted);
  /// 用给定文本替换当前选择（无选择时等价于插入）。
  void replace_selection(std::string_view replacement);
  auto undo() -> bool;
  auto redo() -> bool;
  [[nodiscard]] auto can_undo() const noexcept -> bool { return !undo_stack_.empty(); }
  [[nodiscard]] auto can_redo() const noexcept -> bool { return !redo_stack_.empty(); }
  /// 选中行的缩进（Tab）、反缩进（Shift+Tab）。
  void indent_selection(bool reverse);
  /// 注释/取消注释选中行（用语言的第一个行注释标记；无行注释的语言返回 false）。
  auto toggle_comment() -> bool;

  // —— 滚动 ——

  void set_scroll_offset(float x, float y);
  [[nodiscard]] auto scroll_offset() const noexcept -> math::Point { return {scroll_x_, scroll_y_}; }
  void scroll_to_line(std::size_t line);

  // —— 回调 ——

  std::function<void(std::string_view)> on_change{};       ///< 内容变化（传最新全文）
  std::function<void()> on_cursor_change{};                ///< 光标/选择变化
  std::function<void(std::string_view)> on_submit{};       ///< Ctrl+Enter 提交

  // —— Element 覆写 ——

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  auto invoke_action(std::string_view action, std::string_view argument) -> bool override;

  // —— 静态信息（UI 语言选择器/控制通道枚举用）——

  /// 当前已注册的语言名（含内置与运行时注册的自定义语言，升序）。
  [[nodiscard]] static auto available_languages() -> std::vector<std::string>;

 private:
  /// 一行的 token 切片（偏移相对行起点）。
  using LineTokens = std::vector<text::Token>;

  /// 编辑快照（撤销/重做）。
  struct Snapshot {
    std::string text{};
    std::size_t cursor{0};
    std::size_t anchor{0};
  };

  [[nodiscard]] auto current_spec() const -> std::shared_ptr<const text::LanguageSpec>;
  void mark_highlight_dirty();
  /// 重建**纯文本行索引**（一趟扫描，便宜）。
  void rebuild_spans() const;
  /// 重建**高亮 token 缓存**（依赖文本与语言规则，较贵；只在绘制/需要时做）。
  void rebuild_tokens() const;
  /// 逐行变换 `[first_line, last_line]` 行区间并写回文本（缩进/注释等批量操作共用）。
  void transform_lines(std::size_t first_line, std::size_t last_line,
                       const std::function<std::string(std::string_view)>& transform);
  auto rebuild_line_geometry(const RenderContext& context) const -> void;
  [[nodiscard]] auto gutter_width(const RenderContext& context) const -> float;
  [[nodiscard]] auto line_height(const RenderContext& context) const -> float;
  [[nodiscard]] auto text_origin(const RenderContext& context) const -> math::Point;
  [[nodiscard]] auto content_height() const noexcept -> float;
  [[nodiscard]] auto content_width(const RenderContext& context) const -> float;
  [[nodiscard]] auto visible_line_range(const RenderContext& context) const
      -> std::pair<std::size_t, std::size_t>;
  [[nodiscard]] auto index_at_point(const RenderContext& context, math::Point point) const
      -> std::size_t;
  [[nodiscard]] auto x_for_index(const RenderContext& context, std::size_t index) const -> float;
  [[nodiscard]] auto line_of_index(std::size_t index) const -> std::size_t;
  [[nodiscard]] auto line_start(std::size_t line) const -> std::size_t;
  [[nodiscard]] auto line_end(std::size_t line) const -> std::size_t;
  [[nodiscard]] auto line_range(std::size_t line) const -> std::pair<std::size_t, std::size_t>;
  [[nodiscard]] auto column_of(std::size_t index) const -> std::size_t;
  [[nodiscard]] auto index_at_column(std::size_t line, std::size_t column) const -> std::size_t;
  [[nodiscard]] auto word_bounds(std::size_t index) const -> std::pair<std::size_t, std::size_t>;
  [[nodiscard]] auto matching_bracket() const -> std::optional<std::pair<std::size_t, std::size_t>>;
  [[nodiscard]] auto line_indent(std::size_t line) const -> std::string;

  void push_undo(bool coalesce);
  void notify_change();
  void notify_cursor();
  void ensure_cursor_visible(const RenderContext& context);
  /// 处理按键：返回是否为本编辑器认识的键（false = 未识别，UiRoot 侧继续冒泡/下沉，
  /// 全局快捷键由此获得落点）。
  [[nodiscard]] auto handle_key(const RenderContext& context, const Event& event) -> bool;
  void insert_newline();
  void erase_backward();
  void erase_forward();
  void delete_selection();
  void move_horizontal(int direction, bool extend);
  void move_vertical(const RenderContext& context, int direction, bool extend);
  void move_word(int direction, bool extend);
  void move_to_edge(bool to_end, bool extend);
  void move_document_edge(bool to_end, bool extend);
  void clamp_cursor();
  [[nodiscard]] auto indent_unit() const -> std::string;
  [[nodiscard]] auto indent_columns(const std::string& line) const -> std::size_t;

  std::string text_{};
  std::string language_{};
  std::shared_ptr<const text::LanguageSpec> custom_spec_{};
  std::size_t cursor_{0};
  std::size_t anchor_{0};
  float scroll_x_{0.0f};
  float scroll_y_{0.0f};
  float font_size_{13.0f};
  int tab_width_{4};
  bool insert_spaces_{true};
  bool read_only_{false};
  bool show_line_numbers_{true};
  bool highlight_enabled_{true};

  // 渲染缓存（`mutable`：`paint_content` 为 const，但需要惰性重建）。
  //
  // **分两层**：行索引（`line_spans_`）只随文本变、一趟扫描即可；高亮 token 还要看语言规则，
  // 贵一个数量级。分开后"取行号/算列"这类高频操作不会触发高亮重算（否则批量编辑退化为 O(n²)）。
  mutable bool spans_dirty_{true};
  mutable bool tokens_dirty_{true};
  mutable bool geometry_dirty_{true};
  mutable std::vector<std::pair<std::size_t, std::size_t>> line_spans_{};
  mutable std::vector<LineTokens> line_tokens_{};
  mutable float line_height_cache_{0.0f};
  mutable float gutter_cache_{0.0f};
  mutable float max_line_width_cache_{0.0f};
  mutable std::shared_ptr<const text::LanguageSpec> spec_cache_{};
  mutable bool spec_resolved_{false};

  std::vector<Snapshot> undo_stack_{};
  std::vector<Snapshot> redo_stack_{};
  std::int64_t last_edit_ms_{0};
};

}  // namespace st::ui
