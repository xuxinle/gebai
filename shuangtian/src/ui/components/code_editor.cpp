#include "st/ui/components/code_editor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::text_port_of;

namespace {

// `utf8_prev` / `utf8_next` 现由骨架层提供（`st/core/string.hpp`）：与本文件原副本逐值等价，
// 收敛后与 `Input`/`TextArea` 共用同一份语义（旧 `Input` 副本在文末光标下有缺陷）。

[[nodiscard]] auto is_word_char(char raw) noexcept -> bool {
  return (raw >= 'a' && raw <= 'z') || (raw >= 'A' && raw <= 'Z') || (raw >= '0' && raw <= '9') ||
         raw == '_' || static_cast<unsigned char>(raw) >= 0x80U;
}

/// 自动配对：左/右半边对应表（`\0` = 不参与配对）。
/// 括号与三种引号共用一处，`insert_with_pairs` 与「空对一起删」两个调用点靠它保持一致。
[[nodiscard]] auto mate_of(char raw) noexcept -> char {
  switch (raw) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '"': return '"';
    case '\'': return '\'';
    case '`': return '`';
    default: return '\0';
  }
}

[[nodiscard]] auto is_pair_open(char raw) noexcept -> bool { return mate_of(raw) != '\0'; }

[[nodiscard]] auto is_pair_close(char raw) noexcept -> bool {
  return raw == ')' || raw == ']' || raw == '}' || raw == '"' || raw == '\'' || raw == '`';
}

/// 编辑器内部剪贴板（**进程级单例**）。
///
/// 为什么不是系统剪贴板：无头模式没有系统剪贴板（服务器无 X/Wayland），而编辑器必须具备可用的
/// 复制/粘贴语义。进程内剪贴板在无头与有窗口下行为**完全一致**，也能被控制通道
/// 的 `invoke(copy/cut/paste)` 驱动（智能体可据此搬运文本）。
///
/// 为什么集中在这一个函数里：**多个编辑器实例（多个窗口/多个 `CodeEditor`）共享同一份剪贴板**
/// ——这是刻意的（同一应用内“复制到另一处粘贴”应当可用），但它意味着状态是进程级的。
/// 集中成单一访问点后，将来要改成“每窗口一份”只需改这里一处，而不必去追 20 多个调用点。
///
/// 注：仅保留“最近一次的复制文本”，本身不涉及跨线程并发（UI 操作都在主线程上）。
/// lint-allow: L8 进程级剪贴板是刻意的跨实例共享语义，访问点收敛在此函数
std::string& editor_clipboard() {
  static std::string clipboard;
  return clipboard;
}

// 剪贴板双轨的成员辅助（copy_selection_to_clipboard / paste_text_from_clipboard）
// 声明在 hpp（它们要用 selected_text()/host()，必须是成员）。

inline constexpr float kGutterPadding{12.0f};
/// 勾选列宽（逐行暂存）。独立成常量是因为 `gutter_width` 与点击命中
/// **两处都要用它**——各写一个数早晚会分叉（那表现为"点框没反应"）。
inline constexpr float kCheckboxColumnWidth{18.0f};
/// 行尾注解与正文之间的间隔（见 `LineDecoration::end_text`）。
inline constexpr float kEndAnnotationGap{16.0f};
/// 行尾注解与视口右缘的保留（它不该贴死在边上）。
inline constexpr float kEndAnnotationRightPad{12.0f};
inline constexpr float kTopPadding{6.0f};
inline constexpr float kBottomPadding{6.0f};
inline constexpr float kCursorWidth{1.6f};
inline constexpr float kScrollBarWidth{9.0f};
inline constexpr std::int64_t kCoalesceWindowMs{600};
inline constexpr std::size_t kMaxUndoDepth{256};

/// 诊断严重级别 → 颜色。
///
/// 取主题的 `danger` / `warning` / `primary` / `text_muted`：
/// 诊断色必须与主题同源（写死红黄蓝在浅色主题上会刺眼、在暗色上会发灰）。
[[nodiscard]] auto diagnostic_color(const Theme& theme, CodeEditor::DiagnosticSeverity severity,
                                    const Palette& colors) noexcept -> math::Color {
  switch (severity) {
    case CodeEditor::DiagnosticSeverity::Error: return theme.colors().danger;
    case CodeEditor::DiagnosticSeverity::Warning: return theme.colors().warning;
    case CodeEditor::DiagnosticSeverity::Information: return colors.primary;
    case CodeEditor::DiagnosticSeverity::Hint: return colors.text_muted;
  }
  return theme.colors().danger;
}

/// 令牌类别 → 语法色板取色。
[[nodiscard]] auto token_color(const SyntaxPalette& palette, text::TokenKind kind) noexcept
    -> math::Color {
  switch (kind) {
    case text::TokenKind::Plain: return palette.plain;
    case text::TokenKind::Keyword: return palette.keyword;
    case text::TokenKind::Type: return palette.type;
    case text::TokenKind::String: return palette.string;
    case text::TokenKind::Number: return palette.number;
    case text::TokenKind::Comment: return palette.comment;
    case text::TokenKind::Function: return palette.function;
    case text::TokenKind::Operator: return palette.operator_;
    case text::TokenKind::Punctuation: return palette.punctuation;
    case text::TokenKind::Preprocessor: return palette.preprocessor;
    case text::TokenKind::Builtin: return palette.builtin;
    case text::TokenKind::Attribute: return palette.attribute;
    case text::TokenKind::Key: return palette.key;
    case text::TokenKind::Tag: return palette.tag;
    case text::TokenKind::Inserted: return palette.inserted;
    case text::TokenKind::Deleted: return palette.deleted;
  }
  return palette.plain;
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 构造与外观
// ————————————————————————————————————————————————————————————————————————————

CodeEditor::CodeEditor() {
  set_id("code-editor");
  // 编辑器正文 = 可编辑文字 → I-beam。这与 Input 同口径，
  // 但**行号栏（gutter）例外**：那里不可编辑、点了是选中整行，
  // 显示 I-beam 会误导（用户会以为能在行号上打字）。见 `cursor_at`。
  set_cursor(MouseCursor::Text);
  // 文本编辑类默认可聚焦（与 Input/TextArea 同口径）：“先有焦点才能键盘激活、
  // 键盘激活又要求先有焦点”是死循环——鼠标点击的聚焦路径只认 focusable()，
  // 默认 false 会让点击永远聚焦不了编辑器。
  set_focusable(true);
  style_.direction = FlexDirection::Column;
  style_.clip_children = true;
  style_.background = math::Color{0, 0, 0, 0};
}

CodeEditor::~CodeEditor() = default;

void CodeEditor::set_font_scale(float scale) {
  // 夹取到可读区间：0 会让整屏文字消失（且崩溃式难查），过大则一行放不下几个字。
  font_scale_ = std::clamp(scale, 0.5f, 4.0f);
  font_size_px_ = -1.0f;   // 档位与绝对字号互斥
  mark_layout_dirty();
}

void CodeEditor::set_font_size(float size) {
  font_size_px_ = size;
  mark_layout_dirty();
}

void CodeEditor::set_tab_width(int width) {
  tab_width_ = std::clamp(width, 1, 16);
  mark_layout_dirty();
}

void CodeEditor::set_indent_guides(bool value) {
  if (indent_guides_ == value) return;
  indent_guides_ = value;
  mark_dirty();
}

auto CodeEditor::available_languages() -> std::vector<std::string> {
  return text::global_languages().names();
}

void CodeEditor::set_line_decorations(std::vector<LineDecoration> marks) {
  // 勾选列是否启用：决定 gutter 宽度（见 `gutter_width`）与点击命中。
  // 在这里算一次而不是每帧扫装饰表：装饰表在滚动/打字时不变。
  const bool had_checkbox = any_checkbox_;
  any_checkbox_ = false;
  for (const auto& mark : marks) {
    if (mark.check != LineDecoration::Check::None) {
      any_checkbox_ = true;
      break;
    }
  }
  if (had_checkbox != any_checkbox_) geometry_dirty_ = true;   // gutter 宽度变了要重算几何
  line_marks_ = std::move(marks);
  rebuild_line_mark_index();
  mark_dirty();
}

void CodeEditor::rebuild_line_mark_index() {
  // 按行号建索引（绘制每帧都要查「这一行是什么装饰」，线性查找会在长文档上放大）。
  // 槽里存「装饰下标 + 1」（0 = 无）；同一行多条时**后者胜**（与整体替换的顺序语义一致）。
  const std::size_t rows = line_count() + 2;
  line_mark_index_.assign(rows, 0);
  for (std::size_t index = 0; index < line_marks_.size(); ++index) {
    const std::size_t line = line_marks_[index].line;
    if (line > 0 && line < rows) line_mark_index_[line] = index + 1;
  }
}

void CodeEditor::clear_line_decorations() {
  if (line_marks_.empty() && line_mark_index_.empty()) return;
  line_marks_.clear();
  line_mark_index_.clear();
  mark_dirty();
}

namespace {

/// 滚动位置是否变化（浮点比较按**像素千分之一**夹取：绘制是亚像素的，
/// 严格相等会让「完全相同的两次滚」也触发回调，破坏并列两栏的同步循环）。
[[nodiscard]] auto scroll_changed(float before, float after) noexcept -> bool {
  return std::abs(before - after) > 0.001f;
}

}  // namespace

auto CodeEditor::notify_scroll(float before_x, float before_y) -> void {
  if (!scroll_changed(before_x, scroll_x_) && !scroll_changed(before_y, scroll_y_)) return;
  if (on_scroll) on_scroll(scroll_x_, scroll_y_);
}

auto ThemeSnapshot::of(const Theme& theme) noexcept -> ThemeSnapshot {
  const Palette& colors = theme.colors();
  const SyntaxPalette& syntax = theme.syntax();
  ThemeSnapshot snapshot;
  snapshot.mode = theme.mode();
  snapshot.name = theme.name();
  snapshot.font_base = theme.metrics().font_base;
  snapshot.text = colors.text;
  snapshot.surface_sunken = colors.surface_sunken;
  snapshot.surface_raised = colors.surface_raised;
  snapshot.surface_alt = colors.surface_alt;
  snapshot.syntax_plain = syntax.plain;
  snapshot.syntax_keyword = syntax.keyword;
  snapshot.syntax_string = syntax.string;
  snapshot.syntax_comment = syntax.comment;
  snapshot.syntax_number = syntax.number;
  snapshot.syntax_current_line = syntax.current_line;
  snapshot.syntax_cursor = syntax.cursor;
  return snapshot;
}

void CodeEditor::apply_theme(const Theme& theme) {
  // **主题真的变了才标脏**：声明式宿主每次 `layout()` 都会重走一遍
  // `apply_theme_tree`，无条件 `mark_dirty()` 会让编辑器（占视口 45%+）每帧
  // 报一整块损坏区，与终端的同类标脏合并后恒超 55% 阈值 → 每帧整帧 + 整窗
  // 上屏（WARP 下实测 382% 单核）。
  //
  // ⚠ 判据必须是**内容**而不是指针：`UiRoot::set_theme` 是 `theme_ = std::move(theme)`
  //——同一地址、内容变了。用指针比较会让切主题彻底不生效（实测：字号与
  // 亮/暗切换对编辑器全部无效，因为 `theme_base_font_` 永远停在第一次的值）。
  const ThemeSnapshot snapshot = ThemeSnapshot::of(theme);
  if (applied_theme_.has_value() && *applied_theme_ == snapshot) return;
  applied_theme_ = snapshot;
  // 语法配色在绘制时按 `theme.syntax()` 取（无需缓存到样式），但**基准字号必须在这里落**：
  // 主题是字号缩放（`--ui-font-scale` → `Metrics::scale_fonts`）的唯一真值源，
  // 不读它就会与普通文字脱钩（edit 前：本组件硬写绝对像素，缩放全局生效惟它不动）。
  theme_base_font_ = theme.metrics().font_base;
  const float size = font_size();
  if (std::abs(style_.font_size - size) > 0.001f) {
    style_.font_size = size;
    mark_layout_dirty();   // 行高/列宽/滚动极限全部依赖字号
  }
  style_.background = math::Color{0, 0, 0, 0};
  mark_dirty();
}

void CodeEditor::mark_highlight_dirty() {
  spans_dirty_ = true;
  tokens_dirty_ = true;
  geometry_dirty_ = true;
  // 文本变了 ⇒ 最宽行可能也变了（`max_line_width` 的缓存失效）。
  // 用世代号而不是布尔：`rebuild_spans` 本就要重建索引，那时递增即可。
  ++span_generation_;
  mark_dirty();
}

// ————————————————————————————————————————————————————————————————————————————
// 内容与语言
// ————————————————————————————————————————————————————————————————————————————

void CodeEditor::set_text(std::string text) {
  if (text_ == text) return;
  text_ = std::move(text);
  cursor_ = 0;  // 载入文档后从头展示（与 TextArea 一致）
  anchor_ = 0;
  scroll_x_ = 0.0f;
  scroll_y_ = 0.0f;
  undo_stack_.clear();
  redo_stack_.clear();
  if (find_enabled_) {
    rebuild_find_matches();
    find_active_set_ = false;
    find_active_ = kNoFindMatch;
  }
  hover_line_ = -1;
  h_dragging_ = false;
  mark_highlight_dirty();
  // **不触发 `on_change`**（2026-10-12，来自 gbcode「打开文件没改却提示已修改」）。
  //
  // `set_text` 的调用方是宿主的**程序化装载**（打开/切换/重载文件），
  // 语义是「换了一份文档」而不是「用户编辑了」——而 `on_change` 在宿主侧
  // 几乎总是接标脏逻辑，冒充用户编辑会把刚打开的文件立刻标成未保存。
  // 这与 Input/TextArea 的既定决议一致：C++ API 静默，属性面写入才通知
  // （见 `set_property("text")` 与 DESIGN.md「程序化写入不冒充用户编辑」）。
  // 用户编辑走 `notify_change()`（insert/delete/undo/replace_all 都经它）。
}

void CodeEditor::set_language(std::string language) {
  if (language_ == language && !custom_spec_) return;
  language_ = std::move(language);
  custom_spec_.reset();
  spec_resolved_ = false;
  spec_cache_.reset();
  mark_highlight_dirty();
}

void CodeEditor::set_language_spec(text::LanguageSpec spec) {
  if (spec.name.empty()) spec.name = "custom";
  language_ = spec.name;
  // 借注册表做规范化（去重/排序/大小写折叠），拿回规范化后的不可变副本
  text::LanguageRegistry scratch;
  (void)scratch.register_language(std::move(spec));
  custom_spec_ = scratch.find(language_);
  spec_resolved_ = true;
  spec_cache_ = custom_spec_;
  mark_highlight_dirty();
}

void CodeEditor::set_language_from_path(std::string_view path) {
  if (const auto language = text::language_from_path(path); language.has_value()) {
    set_language(*language);
  }
}

auto CodeEditor::current_spec() const -> std::shared_ptr<const text::LanguageSpec> {
  if (custom_spec_) return custom_spec_;
  if (!spec_resolved_) {
    spec_cache_ = language_.empty() ? std::shared_ptr<const text::LanguageSpec>{}
                                    : text::global_languages().find(language_);
    spec_resolved_ = true;
  }
  return spec_cache_;
}

// ————————————————————————————————————————————————————————————————————————————
// 行索引与高亮缓存
// ————————————————————————————————————————————————————————————————————————————

void CodeEditor::rebuild_spans() const {
  if (!spans_dirty_) return;
  line_spans_.clear();
  std::size_t begin = 0;
  while (true) {
    const std::size_t stop = text_.find('\n', begin);
    if (stop == std::string::npos) {
      line_spans_.emplace_back(begin, text_.size());
      break;
    }
    line_spans_.emplace_back(begin, stop);
    begin = stop + 1;
  }
  spans_dirty_ = false;
}

void CodeEditor::rebuild_tokens() const {
  rebuild_spans();
  if (!tokens_dirty_) return;
  line_tokens_.assign(line_spans_.size(), LineTokens{});
  const auto spec = highlight_enabled_ ? current_spec() : nullptr;
  tokens_dirty_ = false;
  if (!spec || text_.empty()) return;  // 高亮关闭/未知语言：token 全空，绘制按 plain 处理
  // 全量词法扫描后按行切分。
  //
  // 为什么可以全量：实测 1000 行 C++ ≈ 1.7 ms、8000 行 ≈ 13 ms；编辑器每次键入至多触发一次，
  // 常规文件（数百行）在亚毫秒级。跨行状态（块注释/多行串）因此天然正确——
  // 不需要"行状态缓存"那套复杂度。若将来要支持数万行文件，再引入按行状态增量即可。
  const std::vector<text::Token> tokens = text::highlight_with(text_, *spec);
  std::size_t token_index = 0;
  for (std::size_t line = 0; line < line_spans_.size(); ++line) {
    const auto& [begin, end] = line_spans_[line];
    while (token_index < tokens.size() && tokens[token_index].end <= begin) ++token_index;
    std::size_t probe = token_index;
    while (probe < tokens.size() && tokens[probe].begin < end) {
      const text::Token& token = tokens[probe];
      const std::size_t slice_begin = std::max(token.begin, begin);
      const std::size_t slice_end = std::min(token.end, end);
      if (slice_end > slice_begin) {
        line_tokens_[line].push_back(
            text::Token{token.kind, slice_begin - begin, slice_end - begin});
      }
      ++probe;
    }
  }
}

auto CodeEditor::line_count() const -> std::size_t {
  rebuild_spans();
  return line_spans_.empty() ? 1U : line_spans_.size();
}

auto CodeEditor::line_range(std::size_t line) const -> std::pair<std::size_t, std::size_t> {
  rebuild_spans();
  if (line_spans_.empty()) return {0, 0};
  return line_spans_[std::min(line, line_spans_.size() - 1)];
}

auto CodeEditor::line_start(std::size_t line) const -> std::size_t { return line_range(line).first; }

auto CodeEditor::line_end(std::size_t line) const -> std::size_t { return line_range(line).second; }

auto CodeEditor::line_of_index(std::size_t index) const -> std::size_t {
  rebuild_spans();
  if (line_spans_.empty()) return 0;
  const std::size_t clamped = std::min(index, text_.size());
  std::size_t low = 0;
  std::size_t high = line_spans_.size() - 1;
  while (low < high) {
    const std::size_t mid = low + (high - low + 1) / 2;
    if (line_spans_[mid].first <= clamped) {
      low = mid;
    } else {
      high = mid - 1;
    }
  }
  return low;
}

auto CodeEditor::column_of(std::size_t index) const -> std::size_t {
  const std::size_t line = line_of_index(index);
  const std::size_t start = line_start(line);
  if (index <= start) return 0;
  return st::utf8_length(text_.substr(start, std::min(index, text_.size()) - start));
}

auto CodeEditor::index_at_column(std::size_t line, std::size_t column) const -> std::size_t {
  const auto [begin, end] = line_range(line);
  const std::string_view row = std::string_view(text_).substr(begin, end - begin);
  if (column == 0) return begin;
  const std::size_t bytes = st::utf8_offset(row, column);
  return begin + std::min(bytes, row.size());
}

auto CodeEditor::cursor_line() const -> std::size_t { return line_of_index(cursor_); }

auto CodeEditor::cursor_column() const -> std::size_t { return column_of(cursor_); }

auto CodeEditor::word_bounds(std::size_t index) const -> std::pair<std::size_t, std::size_t> {
  const std::size_t size = text_.size();
  std::size_t begin = std::min(index, size);
  std::size_t end = begin;
  while (begin > 0) {
    const std::size_t previous = utf8_prev(text_, begin);
    if (!is_word_char(text_[previous])) break;
    begin = previous;
  }
  while (end < size) {
    if (!is_word_char(text_[end])) break;
    end = utf8_next(text_, end);
  }
  return {begin, end};
}

auto CodeEditor::line_indent(std::size_t line) const -> std::string {
  const auto [begin, end] = line_range(line);
  std::size_t index = begin;
  while (index < end && (text_[index] == ' ' || text_[index] == '\t')) ++index;
  return text_.substr(begin, index - begin);
}

auto CodeEditor::indent_unit() const -> std::string {
  if (!insert_spaces_) return "\t";
  return std::string(static_cast<std::size_t>(tab_width_), ' ');
}

// ————————————————————————————————————————————————————————————————————————————
// 光标与选择
// ————————————————————————————————————————————————————————————————————————————

void CodeEditor::clamp_cursor() {
  cursor_ = std::min(cursor_, text_.size());
  anchor_ = std::min(anchor_, text_.size());
  while (cursor_ > 0 && cursor_ < text_.size() &&
         (static_cast<unsigned char>(text_[cursor_]) & 0xC0U) == 0x80U) {
    --cursor_;
  }
  while (anchor_ > 0 && anchor_ < text_.size() &&
         (static_cast<unsigned char>(text_[anchor_]) & 0xC0U) == 0x80U) {
    --anchor_;
  }
}

void CodeEditor::set_cursor_index(std::size_t index) {
  cursor_ = std::min(index, text_.size());
  clamp_cursor();
  anchor_ = cursor_;
  mark_dirty();
  if (on_cursor_change) on_cursor_change();
}

auto CodeEditor::selection() const -> std::pair<std::size_t, std::size_t> {
  return cursor_ < anchor_ ? std::pair{cursor_, anchor_} : std::pair{anchor_, cursor_};
}

auto CodeEditor::selected_text() const -> std::string {
  const auto [begin, end] = selection();
  return text_.substr(begin, end - begin);
}

void CodeEditor::copy_selection_to_clipboard() {
  const std::string picked = selected_text();
  if (picked.empty()) return;
  editor_clipboard() = picked;
  if (host() != nullptr) (void)host()->host_set_clipboard_text(picked);
}

std::string CodeEditor::paste_text_from_clipboard() const {
  if (host() != nullptr) {
    const std::string from_platform = host()->host_clipboard_text();
    if (!from_platform.empty()) return from_platform;
  }
  return editor_clipboard();
}

void CodeEditor::select_all() {
  anchor_ = 0;
  cursor_ = text_.size();
  mark_dirty();
  if (on_cursor_change) on_cursor_change();
}

void CodeEditor::set_selection(std::size_t begin, std::size_t end) {
  anchor_ = std::min(begin, text_.size());
  cursor_ = std::min(end, text_.size());
  clamp_cursor();
  mark_dirty();
  if (on_cursor_change) on_cursor_change();
}

void CodeEditor::goto_line(std::size_t line) {
  const std::size_t clamped = std::min(line == 0 ? 0 : line - 1, line_count() - 1);
  cursor_ = std::min(line_start(clamped), text_.size());
  anchor_ = cursor_;
  mark_dirty();
  if (on_cursor_change) on_cursor_change();
}

// ————————————————————————————————————————————————————————————————————————————
// 编辑
// ————————————————————————————————————————————————————————————————————————————

void CodeEditor::push_undo(bool coalesce) {
  const std::int64_t now = st::time::now_ms();
  const bool merge = coalesce && !undo_stack_.empty() && now - last_edit_ms_ < kCoalesceWindowMs;
  if (!merge) {
    undo_stack_.push_back(Snapshot{text_, cursor_, anchor_});
    if (undo_stack_.size() > kMaxUndoDepth) undo_stack_.erase(undo_stack_.begin());
  }
  last_edit_ms_ = now;
  redo_stack_.clear();
}

void CodeEditor::notify_change() {
  if (find_enabled_) refresh_find_after_edit();
  // 文本变了 → 行数变了 → 行装饰的按行索引必须重建。
  //
  // 不重建的后果是**静默错行**：索引表还按旧行数算，插/删一行之后
  // 所有装饰都偏了一行（差异视图里就是「颜色标在错的行上」，而它看起来仍然"有颜色"）。
  if (!line_marks_.empty()) rebuild_line_mark_index();
  mark_highlight_dirty();
  clamp_cursor();
  if (on_change) on_change(text_);
  if (on_cursor_change) on_cursor_change();
}

void CodeEditor::delete_selection() {
  // 只读保护放在**这一层**：所有删除路径（Backspace/Delete/Ctrl+X/替换/
  // `replace_selection`）都经它下来，只靠各入口自己判会漏（实测：只读下
  // `select_all` + Delete 能把全文清空）。
  if (read_only_) return;
  const auto [begin, end] = selection();
  if (begin == end) return;
  text_.erase(begin, end - begin);
  cursor_ = begin;
  anchor_ = begin;
}

void CodeEditor::replace_selection(std::string_view replacement) {
  if (read_only_) return;
  push_undo(false);
  delete_selection();
  text_.insert(cursor_, replacement);
  cursor_ += replacement.size();
  anchor_ = cursor_;
  notify_change();
}

void CodeEditor::insert_text(std::string_view inserted) {
  if (read_only_ || inserted.empty()) return;
  // 自动配对：先给配对/包裹/跳闭符三条语义一个接管机会（未接管则照原样落盘）。
  if (auto_pairs_ && inserted.size() == 1 && insert_with_pairs(inserted)) return;
  push_undo(inserted.size() == 1);
  delete_selection();
  text_.insert(cursor_, inserted);
  cursor_ += inserted.size();
  anchor_ = cursor_;
  notify_change();
}

/// 自动配对（编辑体验的核心一条）。
///
/// 三条语义，与主流编辑器一致：
/// - **跳过闭符**：光标右边已是同一闭符（且无选择）→ 只前移一格，不插入（避免 `()）`）；
/// - **包裹**：有选择 + 输入左符/引号 → 用一对把选中文本包起来，选择保留在内部；
/// - **配对插入**：输入左符 → 补右符，光标留在中间；`"`/`'`/`` ` `` 只在「不在词中」时配对
///   （否则 `isn't` 这类文本会被越修改越乱——宁可少帮一手）。
///
/// 引号/闭符在 `read_only_` 下不生效（由 `insert_text` 入口拦下）。
///
/// 返回 true = 已接管（调用方不再原样插入）。
///
/// 注：不自动配对引号内的引号（那要靠词法状态，此处只用「前一个字符是不是词字符」这条便宜的启发式）。
auto CodeEditor::insert_with_pairs(std::string_view inserted) -> bool {
  if (inserted.empty() || read_only_) return false;
  const char typed = inserted[0];

  // ① **先判跳过闭符**——闭符本身不在 `mate_of` 表里，放到下面会被误当成
  // “不参与配对”直接返回。光标右侧就是同一个闭合符 → 只前移（仅限无选择）：
  // 这是“敲 `(` 自动补了 `)` 后，再敲 `)` 不会变成 `())`”的关键一步。
  if (is_pair_close(typed) && !has_selection() && cursor_ < text_.size() &&
      text_[cursor_] == typed) {
    cursor_ += 1;
    anchor_ = cursor_;
    mark_dirty();
    if (on_cursor_change) on_cursor_change();
    return true;
  }

  const char mate = mate_of(typed);
  if (mate == '\0') return false;

  // ② 包裹选择：`(`/`[`/`{`/引号 均适用（引号包裹是选词加引号的高频动作）
  if (has_selection()) {
    const auto [begin, end] = selection();
    const std::string picked = text_.substr(begin, end - begin);
    push_undo(false);
    std::string wrapped;
    wrapped.reserve(picked.size() + 2);
    wrapped.push_back(typed);
    wrapped.append(picked);
    wrapped.push_back(mate);
    text_.replace(begin, end - begin, wrapped);
    anchor_ = begin + 1;
    cursor_ = anchor_ + picked.size();
    notify_change();
    return true;
  }

  // ③ 引号：只在「不在词中」时配对（`it's` / `a"b` 这类文本不被破坏）
  if (typed == '"' || typed == '\'' || typed == '`') {
    const bool after_word = cursor_ > 0 && is_word_char(text_[cursor_ - 1]);
    if (after_word) return false;
  }

  // ④ 配对插入（括号：只在行尾/空白/闭符前补右半；`f(x)` 里手动敲 `)` 不该被改成 `f(x))`）
  if (typed == '(' || typed == '[' || typed == '{') {
    // ⚠ 行尾集合必须含 `\r`：Windows 上绝大多数文件是 CRLF，一行真正结尾的
    // 下一个字符是 `\r`（不是 `\n`）——旧集合只有 `\n`，于是**CRLF 文件里在
    // 行尾敲 `(` 不会配对**，而同一个文件改成 LF 就正常（实测：`x = 1(\r\n`）。
    // 这类“只在另一种行尾下才坏”的缺陷最难靠直觉发现，必须由回归钉住。
    const bool tail_ok = cursor_ >= text_.size() || text_[cursor_] == ' ' || text_[cursor_] == '\t' ||
                         text_[cursor_] == '\n' || text_[cursor_] == '\r' ||
                         is_pair_close(text_[cursor_]);
    if (!tail_ok) return false;
  }
  push_undo(true);
  text_.insert(cursor_, 1, typed);
  text_.insert(cursor_ + 1, 1, mate);
  cursor_ += 1;
  anchor_ = cursor_;
  notify_change();
  return true;
}

void CodeEditor::insert_newline() {
  if (read_only_) return;
  const std::size_t line = line_of_index(cursor_);
  const std::size_t start = line_start(line);
  const std::string indent = line_indent(line);
  const std::string_view before = std::string_view(text_).substr(start, cursor_ - start);
  const std::string_view trimmed = st::trim_end(before);
  const bool opens_block =
      !trimmed.empty() && (trimmed.back() == '{' || trimmed.back() == '(' ||
                           trimmed.back() == '[' || trimmed.back() == ':');
  const std::string unit = indent_unit();
  if (!opens_block) {
    replace_selection(std::string("\n").append(indent));
    return;
  }
  // `{|}` 处回车 → 展开为三行（`{` / 缩进行 / `}`），符合编辑器惯例
  const bool closes_next =
      cursor_ < text_.size() && (text_[cursor_] == '}' || text_[cursor_] == ')' || text_[cursor_] == ']');
  if (!closes_next) {
    replace_selection(std::string("\n").append(indent).append(unit));
    return;
  }
  std::string inserted = "\n";
  inserted.append(indent).append(unit).append("\n").append(indent);
  push_undo(false);
  delete_selection();
  text_.insert(cursor_, inserted);
  cursor_ += 1 + indent.size() + unit.size();
  anchor_ = cursor_;
  notify_change();
}

void CodeEditor::erase_backward() {
  if (read_only_) return;
  if (has_selection()) {
    push_undo(false);
    delete_selection();
    notify_change();
    return;
  }
  if (cursor_ == 0) return;
  // 空对一起删：光标夹在 `()` / `""` 中间时，一次退格把两边一起吃掉
  // （否则用户得按两下，且第二下看着像“什么都没删”）
  if (auto_pairs_ && cursor_ < text_.size() && is_pair_open(text_[cursor_ - 1]) &&
      mate_of(text_[cursor_ - 1]) == text_[cursor_]) {
    push_undo(false);
    text_.erase(cursor_ - 1, 2);
    cursor_ -= 1;
    anchor_ = cursor_;
    notify_change();
    return;
  }
  const std::size_t begin = utf8_prev(text_, cursor_);
  push_undo(true);
  text_.erase(begin, cursor_ - begin);
  cursor_ = begin;
  anchor_ = begin;
  notify_change();
}

/// 按词删除（Ctrl+Backspace / Ctrl+Delete）。
///
/// 语义与 `move_word` 对齐：向左删到上一词首之前（跳过的空白也一并删掉），
/// 向右删到下一词尾（含尾随空白）。行首/文首/文末自然停止。
void CodeEditor::erase_word(bool backward) {
  if (read_only_) return;
  if (has_selection()) {
    push_undo(false);
    delete_selection();
    notify_change();
    return;
  }
  if (backward) {
    if (cursor_ == 0) return;
    std::size_t begin = cursor_;
    while (begin > 0 && !is_word_char(text_[utf8_prev(text_, begin)])) begin = utf8_prev(text_, begin);
    while (begin > 0 && is_word_char(text_[utf8_prev(text_, begin)])) begin = utf8_prev(text_, begin);
    if (begin == cursor_) return;
    push_undo(false);
    text_.erase(begin, cursor_ - begin);
    cursor_ = begin;
    anchor_ = begin;
  } else {
    if (cursor_ >= text_.size()) return;
    std::size_t end = cursor_;
    // 已在词首：直接吃掉该词；否则先跨过空白再吃（与 `move_word` 向右同族）。
    if (!is_word_char(text_[end])) {
      std::size_t probe = end;
      while (probe < text_.size() && !is_word_char(text_[probe])) probe = utf8_next(text_, probe);
      // 后面已经没有词了（尾随空白）：把剩下的空白一并清掉，而不是“什么也不删”
      if (probe >= text_.size()) {
        push_undo(false);
        text_.erase(cursor_);
        notify_change();
        return;
      }
      end = probe;
    }
    while (end < text_.size() && is_word_char(text_[end])) end = utf8_next(text_, end);
    if (end == cursor_) return;
    push_undo(false);
          text_.erase(cursor_, end - cursor_);
  }
  notify_change();
}

/// 智能 Home（VSCode 同款两段式）：光标不在行首非空列 → 先跳到该列（对齐缩进后
/// 开始敲字的落点）；已在 → 跳列 0（真行首）。
/// 注：这是高频手感键——一次 Home 落在“代码开始处”而不被缩进空白卡住。
void CodeEditor::smart_home(bool extend) {
  const std::size_t line = line_of_index(cursor_);
  const auto [begin, end] = line_range(line);
  std::size_t head = begin;
  while (head < end && (text_[head] == ' ' || text_[head] == '\t')) ++head;
  const std::size_t target = cursor_ == head ? begin : head;
  cursor_ = target;
  if (!extend) anchor_ = cursor_;
}

void CodeEditor::erase_forward() {
  if (read_only_) return;
  if (has_selection()) {
    push_undo(false);
    delete_selection();
    notify_change();
    return;
  }
  if (cursor_ >= text_.size()) return;
  const std::size_t end = utf8_next(text_, cursor_);
  push_undo(true);
  text_.erase(cursor_, end - cursor_);
  notify_change();
}

auto CodeEditor::undo() -> bool {
  if (undo_stack_.empty()) return false;
  Snapshot snapshot = std::move(undo_stack_.back());
  undo_stack_.pop_back();
  redo_stack_.push_back(Snapshot{text_, cursor_, anchor_});
  text_ = std::move(snapshot.text);
  cursor_ = snapshot.cursor;
  anchor_ = snapshot.anchor;
  notify_change();
  return true;
}

auto CodeEditor::redo() -> bool {
  if (redo_stack_.empty()) return false;
  Snapshot snapshot = std::move(redo_stack_.back());
  redo_stack_.pop_back();
  undo_stack_.push_back(Snapshot{text_, cursor_, anchor_});
  text_ = std::move(snapshot.text);
  cursor_ = snapshot.cursor;
  anchor_ = snapshot.anchor;
  notify_change();
  return true;
}

void CodeEditor::transform_lines(std::size_t first_line, std::size_t last_line,
                                 const std::function<std::string(std::string_view)>& transform) {
  rebuild_spans();
  if (line_spans_.empty()) return;
  const std::size_t total = line_spans_.size();
  const std::size_t first = std::min(first_line, total - 1);
  const std::size_t last = std::min(last_line, total - 1);
  const std::size_t first_begin = line_spans_[first].first;
  const std::size_t last_end = line_spans_[last].second;

  std::string rebuilt;
  rebuilt.reserve(text_.size() + 64);
  rebuilt.append(text_, 0, first_begin);
  for (std::size_t line = first; line <= last; ++line) {
    const auto [begin, end] = line_spans_[line];
    rebuilt.append(transform(std::string_view(text_).substr(begin, end - begin)));
    if (line < last) rebuilt.push_back('\n');
  }
  rebuilt.append(text_, last_end, text_.size() - last_end);
  text_ = std::move(rebuilt);
  cursor_ = std::min(cursor_, text_.size());
  anchor_ = std::min(anchor_, text_.size());
  notify_change();
}

void CodeEditor::indent_selection(bool reverse) {
  if (read_only_) return;
  const auto [begin, end] = selection();
  const std::size_t first_line = line_of_index(begin);
  const std::size_t last_line = line_of_index(end);
  const std::string unit = indent_unit();
  push_undo(false);
  transform_lines(first_line, last_line, [&unit, reverse, this](std::string_view row) {
    if (reverse) {
      std::size_t remove = 0;
      while (remove < row.size() && remove < unit.size() && row[remove] == unit[remove]) ++remove;
      if (remove == 0 && !row.empty() && row.front() == '\t') remove = 1;
      return std::string(row.substr(remove));
    }
    std::string out;
    out.reserve(row.size() + unit.size());
    out.append(unit).append(row);
    (void)this;
    return out;
  });
}

auto CodeEditor::toggle_comment() -> bool {
  if (read_only_) return false;
  const auto spec = current_spec();
  if (!spec || spec->line_comment.empty()) return false;
  const std::string marker = spec->line_comment;
  const auto [begin, end] = selection();
  const std::size_t first_line = line_of_index(begin);
  const std::size_t last_line = line_of_index(end);

  rebuild_spans();
  bool all_commented = true;
  for (std::size_t line = first_line; line <= last_line && line < line_spans_.size(); ++line) {
    const auto [line_begin, line_end] = line_spans_[line];
    std::size_t index = line_begin;
    while (index < line_end && (text_[index] == ' ' || text_[index] == '\t')) ++index;
    if (text_.compare(index, marker.size(), marker) != 0) {
      all_commented = false;
      break;
    }
  }

  push_undo(false);
  transform_lines(first_line, last_line, [&marker, all_commented](std::string_view row) {
    std::size_t indent = 0;
    while (indent < row.size() && (row[indent] == ' ' || row[indent] == '\t')) ++indent;
    if (!all_commented) {
      if (row.empty()) return std::string{};
      std::string out;
      out.reserve(row.size() + marker.size() + 1);
      out.append(row.substr(0, indent)).append(marker).push_back(' ');
      out.append(row.substr(indent));
      return out;
    }
    std::size_t skip = indent + marker.size();
    while (skip < row.size() && row[skip] == ' ') ++skip;
    return std::string(row.substr(0, indent)) + std::string(row.substr(skip));
  });
  return true;
}

auto CodeEditor::select_next_occurrence() -> bool {
  rebuild_spans();
  if (text_.empty()) return false;

  // ① 无选中：先把光标处的词选中（这是 Ctrl+D 的第一次按下）。
  if (!has_selection()) {
    const auto [begin, end] = word_bounds(cursor_);
    if (begin == end) return false;  // 光标不在词上（空白/标点）——不冒充已处理
    // 光标回退一格时也能取到左边的词（与双击选词的观感一致）。
    const auto [left_begin, left_end] = word_bounds(cursor_ > 0 ? cursor_ - 1 : 0);
    if (left_begin < left_end && (begin == end || cursor_ == begin)) {
      anchor_ = left_begin;
      cursor_ = left_end;
    } else {
      anchor_ = begin;
      cursor_ = end;
    }
    mark_dirty();
    if (on_cursor_change) on_cursor_change();
    return true;
  }

  // ② 已有选中：从**选区末端**起向后找同一段文本，找到就挪过去；到文末则环绕。
  //
  // 字面比较（区分大小写）：Ctrl+D 的语义是“下一处**一模一样**的那段字”，
  // 大小写不同是另一处词，不该被吞进来。
  //
  // 比较起点用 `cursor_`（选区末端）而不是 `max(anchor,cursor)`：向后选时二者相同，
  // 向前选时也符合“继续往下找”的直觉（与主流编辑器一致）。
  const auto [sel_begin, sel_end] = selection();
  const std::string_view needle(text_.data() + sel_begin, sel_end - sel_begin);
  if (needle.empty()) return false;
  const std::size_t from = std::min(sel_end, text_.size());
  std::size_t hit = text_.find(needle, from);
  if (hit == std::string::npos) hit = text_.find(needle, 0);
  if (hit == std::string::npos) return false;
  // 环绕后可能又回到“就是当前这一处”（全文只此一处）：维持原选区返回 false，
  // 让调用方（状态栏/宿主）能如实报告“没有下一处”，而不是看着像执行成功。
  if (hit == sel_begin) return false;
  anchor_ = hit;
  cursor_ = hit + needle.size();
  mark_dirty();
  if (on_cursor_change) on_cursor_change();
  return true;
}

auto CodeEditor::move_lines(int delta) -> bool {
  if (read_only_ || delta == 0) return false;
  rebuild_spans();
  if (line_spans_.empty()) return false;
  const auto [sel_begin, sel_end] = selection();
  const std::size_t first = line_of_index(sel_begin);
  const std::size_t last = line_of_index(sel_end);
  const std::size_t total = line_spans_.size();
  // 越界不搬：到文档首还往上、到文档末还往下都如实返回 false
  // （静默“看似成功但没动”会让宿主的状态栏报出与实际相反的结论）。
  if (delta < 0 && first == 0) return false;
  if (delta > 0 && last + 1 >= total) return false;
  const std::size_t target = delta < 0 ? first - 1 : last + 1;

  // 搬的是**整行**（含行尾换行），这样中间那一段可以原样平移；
  // 两端的孤儿描述必须一起算清楚，否则会多/少一个空行（最容易错的一步）。
  const std::size_t block_begin = line_spans_[first].first;
  const std::size_t block_end = line_spans_[last].second;
  const std::size_t target_begin = line_spans_[target].first;
  const std::size_t target_end = line_spans_[target].second;

  std::string moved(text_, block_begin, block_end - block_begin);
  std::string neighbour(text_, target_begin, target_end - target_begin);
  // 光标/选区的新行号**在改动文本之前算定**：`rebuilt` 一赋回 `text_`，
  // 原来的行/列查询就落到新文本上了（最容易错的一步——会在改完后再"定位"到错的列）。
  const std::size_t cursor_line = line_of_index(cursor_);
  const std::size_t cursor_column = column_of(cursor_);
  const std::size_t anchor_line = line_of_index(anchor_);
  const std::size_t anchor_column = column_of(anchor_);

  std::string rebuilt;
  rebuilt.reserve(text_.size() + 1);
  if (delta < 0) {
    // [target][moved] 互换：把 moved 放到 target 前面
    rebuilt.append(text_, 0, target_begin);
    rebuilt.append(moved).push_back('\n');
    rebuilt.append(neighbour);
    rebuilt.append(text_, block_end, text_.size() - block_end);
  } else {
    rebuilt.append(text_, 0, block_begin);
    rebuilt.append(neighbour).push_back('\n');
    rebuilt.append(moved);
    rebuilt.append(text_, target_end, text_.size() - target_end);
  }
  push_undo(false);
  text_ = std::move(rebuilt);

  // 光标/选区跟着行走：**行号 ±1、列不变**（选中多行时整个块一起走）。
  const auto shift = [&](std::size_t line, std::size_t column) -> std::size_t {
    if (line < first || line > last) return index_at_column(std::min(line, line_count() - 1), column);
    return index_at_column(delta < 0 ? line - 1 : line + 1, column);
  };
  cursor_ = shift(cursor_line, cursor_column);
  anchor_ = shift(anchor_line, anchor_column);
  clamp_cursor();
  notify_change();
  return true;
}

// ————————————————————————————————————————————————————————————————————————————
// 几何
// ————————————————————————————————————————————————————————————————————————————

auto CodeEditor::line_height(const RenderContext& context) const -> float {
  // **行距倍数的唯一入口**：绘制、滚动、命中测试、内容高度全走这里，
  // 因此乘系数只在这一处——分散到各处必然会漏（实测同类缺陷：字号只接了一半路径）。
  return text_port_of(context).line_height(font_size()) * line_spacing_;
}

void CodeEditor::set_line_spacing(float spacing) {
  // 非法值忽略（与 `set_font_scale` 同姿态：静默接受 NaN/0 会让整个编辑器排版崩掉，
  // 而调用方从返回值看不到任何线索）。
  if (!std::isfinite(spacing) || spacing <= 0.0f) return;
  if (std::abs(line_spacing_ - spacing) < 0.001f) return;   // 相等早退（每帧调用不重建几何）
  line_spacing_ = spacing;
  mark_layout_dirty();   // 行高/滚动极限/内容高度全部依赖它
}

auto CodeEditor::gutter_width(const RenderContext& context) const -> float {
  if (!show_line_numbers_) return 0.0f;
  const TextPort& port = text_port_of(context);
  const std::size_t digits = std::to_string(std::max<std::size_t>(line_count(), 1)).size();
  float width = port.measure_width(std::string(digits, '0'), font_size(),
                                   text::FontRole::Monospace) + kGutterPadding * 2.0f;
  // 勾选列（逐行暂存）：**加宽 gutter** 而不是把框叠在行号上——
  // 叠上去会让行号在勾选时变得不可读（而"看着几号行"与"选哪一行"要同时可用）。
  if (any_checkbox_) width += kCheckboxColumnWidth;
  return width;
}

/// 指针形态**按位置细分**：正文给 I-beam，行号栏（gutter）给箭头。
///
/// 为何不能整个编辑器一律 Text：行号栏的作用是“选中整行/点断点”，
/// 它**不可编辑**。原生编辑器（VSCode/IDEA/Sublime）在这里都是箭头——
/// 显示 I-beam 会让人以为能在行号上打字。
///
/// ⚠ 用 `gutter_cache_` 而不是调 `gutter_width(context)`：`cursor_at` 的签名
/// 不带 `RenderContext`（指针查询发生在鼠标移动时，那时没有绘制上下文）。
/// 缓存由 `rebuild_line_geometry` 每帧写好，鼠标事件一定发生在某帧之后，
/// 所以读到的总上一帧的值——而 gutter 宽度只在行数位数变化时改变，
/// 这个滞后不可感知。
auto CodeEditor::cursor_at(math::Point point) const noexcept -> MouseCursor {
  if (show_line_numbers_ && point.x < bounds_.x + gutter_cache_) return MouseCursor::Arrow;
  return MouseCursor::Text;
}

auto CodeEditor::rebuild_line_geometry(const RenderContext& context) const -> void {
  if (!geometry_dirty_) return;
  rebuild_tokens();
  line_height_cache_ = text_port_of(context).line_height(font_size()) * line_spacing_;
  // **一行的几何从框架拿**（`ui::layout_text_line`）：墨迹落位、墨迹带范围
  // 全部由它给出，组件不再自己算偏移。
  //
  // 这里曾有一份组件自己的行盒推导（`line_block_offset` / `caret_top_cache_`）——
  // 那是**把框架该给的东西写在应用层**：同一件事在框架里被违反了三条路
  // （`Element::paint_text` 一条、按钮直接调 `centered_line_top` 一条、
  // 编辑器自己一条），结果修一处不动另一处。现统一到 `line_layout.hpp`。
  line_geometry_cache_ = ui::layout_text_line(text_port_of(context), font_size(), line_spacing_);
  gutter_cache_ = gutter_width(context);
  // ⚠ **不再在这里逐行量宽度**（2026-10-18，实测驱动）。
  //
  // 旧写法：把全部 `line_spans_` 走一遍、逐行 `measure_width`，求
  // `max_line_width_cache_`（只为横向滚动范围服务）。代价是
  // **O(总行数 × 行宽)**——而它在**每次文本变更**后都要重跑：
  // 打开 5465 行的 DESIGN.md 后每敲一个字符，单帧 paint 从 4.5ms 涨到 90ms
  //（实测：546 行 11.5ms / 1366 行 14.3ms / 5465 行 97.5ms，线性增长）。
  // 而这份值的用途只有横向滚动条与 max_scroll_x，根本不需要在“画这一帧”之前知道。
  // 所以改为**惰性**（`max_line_width(context)`），只在真要算横向范围时才走全表；
  // 它自身再带一层按文本版本号的缓存，编辑后第一次查询付一次、后续白拿。
  geometry_dirty_ = false;
}

auto CodeEditor::max_line_width(const RenderContext& context) const -> float {
  // 横向滚动范围专用（
  // `content_width` / `max_scroll_x` 两个调用点）。
  //
  // 为什么惰性：它要量**所有行**（长行文本不量就不知道最宽是多少），
  // 而绝大多数帧（打字、光标移动、滚动）根本不需要这个数——只有横向
  // 滚动条与“内容宽度”需要。放进 `rebuild_line_geometry` 等于让每次编辑
  // 都按文件大小付费（实测大文件单帧 90ms）。
  //
  // 缓存键 = `line_spans_` 的世代号（`spans_dirty_` 复位时递增）：
  // 文本变 → 世代变 → 重算一次；文本没变 → 白拿。
  if (max_width_generation_ == span_generation_ && !geometry_dirty_) {
    return max_line_width_cache_;
  }
  rebuild_line_geometry(context);   // 保证 `gutter_cache_` 等已就绪
  const TextPort& port = text_port_of(context);
  float widest = 0.0f;
  for (const auto& [begin, end] : line_spans_) {
    const float width = port.measure_width(std::string_view(text_).substr(begin, end - begin),
                                         font_size(), text::FontRole::Monospace);
    widest = std::max(widest, width);
  }
  max_line_width_cache_ = widest;
  max_width_generation_ = span_generation_;
  return max_line_width_cache_;
}

auto CodeEditor::text_origin(const RenderContext& context) const -> math::Point {
  rebuild_line_geometry(context);
  // ⚠ **必须含 `- scroll_x_`**：这是全部 x 坐标的量尺（`x_for_index` → 光标 /
  // 选区 / 查找高亮 / 缩进线 / 命中测试）。漏掉它的话正文会滚、而上述元素不滚
  //——用户报的“横向滚动光标偏移了”就是这个（文字滚了 384px，光标还停在原位）。
  return math::Point{bounds_.x + gutter_cache_ + kGutterPadding - scroll_x_,
                     bounds_.y + kTopPadding - scroll_y_};
}

auto CodeEditor::content_height() const noexcept -> float {
  rebuild_spans();
  return static_cast<float>(line_spans_.size()) * line_height_cache_ + kTopPadding + kBottomPadding;
}

auto CodeEditor::content_width(const RenderContext& context) const -> float {
  return gutter_cache_ + max_line_width(context) + kGutterPadding * 2.0f;
}

auto CodeEditor::visible_line_range(const RenderContext& context) const
    -> std::pair<std::size_t, std::size_t> {
  const float height = line_height(context);
  rebuild_spans();
  const std::size_t total = line_spans_.size();
  if (height <= 0.0f || total == 0) return {0, total};
  const float top = scroll_y_ - kTopPadding;
  const float bottom = scroll_y_ + bounds_.height - kTopPadding;
  const auto first = static_cast<std::size_t>(std::max(0.0f, std::floor(top / height)));
  const auto last = static_cast<std::size_t>(std::max(0.0f, std::ceil(bottom / height)));
  return {std::min(first, total > 0 ? total - 1 : 0), std::min(last + 1, total)};
}

auto CodeEditor::expand_tabs(std::string_view row) const -> TabExpansion {
  TabExpansion expansion;
  expansion.map.resize(row.size() + 1);
  expansion.text.reserve(row.size() + 8);
  std::size_t column = 0;   // 逻辑列（码点）
  const std::size_t width = static_cast<std::size_t>(std::max(1, tab_width_));
  for (std::size_t index = 0; index < row.size();) {
    expansion.map[index] = expansion.text.size();
    const char raw = row[index];
    if (raw == '\t') {
      // 补到下一个制表位（至少一列：列 0 的 Tab 展开为 width 个空格）
      const std::size_t filler = width - (column % width);
      expansion.text.append(filler, ' ');
      column += filler;
      ++index;
      continue;
    }
    // 非 ASCII：整码点一起走（避免把一个多字节字符截成一个字节）
    const std::size_t next = (static_cast<unsigned char>(raw) < 0x80U) ? index + 1 : utf8_next(row, index);
    expansion.text.append(row.substr(index, next - index));
    expansion.map[next] = expansion.text.size();
    column += 1;
    index = next;
  }
  expansion.map[row.size()] = expansion.text.size();
  return expansion;
}

auto CodeEditor::v_scroll_bar_rect(const RenderContext& context) const -> math::Rect {
  if (bounds_.is_empty()) return {};
  // 内容未溢出时不画也不命中（否则空文档上下也会有一条可拖的条纹）
  if (content_height() <= bounds_.height || bounds_.height <= 0.0f) return {};
  const float reserve = max_scroll_x(context) > 0.5f ? kScrollBarWidth : 0.0f;
  return math::Rect{bounds_.right() - kScrollBarWidth - 2.0f, bounds_.y,
                    kScrollBarWidth, std::max(1.0f, bounds_.height - reserve)};
}

auto CodeEditor::v_scroll_thumb_rect(const RenderContext& context) const -> math::Rect {
  const math::Rect track = v_scroll_bar_rect(context);
  if (track.width <= 0.0f) return {};
  const float total = content_height();
  if (total <= 0.0f) return {};
  const float thumb = std::max(24.0f, track.height * std::min(1.0f, track.height / total));
  const float travel = std::max(1.0f, track.height - thumb);
  const float max_scroll = std::max(1.0f, total - bounds_.height);
  const float offset = std::clamp(scroll_y_ / max_scroll, 0.0f, 1.0f) * travel;
  return math::Rect{track.x, track.y + offset, track.width, thumb};
}

void CodeEditor::apply_v_scroll_drag(const RenderContext& context, float pointer_y) {
  const math::Rect track = v_scroll_bar_rect(context);
  if (track.width <= 0.0f) return;
  const float total = content_height();
  const float thumb = std::max(24.0f, track.height * std::min(1.0f, track.height / total));
  const float travel = std::max(1.0f, track.height - thumb);
  const float local = std::clamp((pointer_y - track.y - v_drag_offset_) / travel, 0.0f, 1.0f);
  const float max_scroll = std::max(0.0f, total - bounds_.height);
  // **不碰光标**：滚动条的语义是“看另一个地方”，不是“把光标挪过去”。
  const float before_y = scroll_y_;
  scroll_y_ = local * max_scroll;
  mark_dirty();
  notify_scroll(scroll_x_, before_y);
}

auto CodeEditor::x_for_index(const RenderContext& context, std::size_t index) const -> float {
  rebuild_line_geometry(context);
  const std::size_t line = line_of_index(index);
  const std::size_t start = line_start(line);
  const std::size_t stop = std::min(index, text_.size());
  const std::string_view raw = std::string_view(text_).substr(start, stop - start);
  // **制表符按制表位展开后再量宽**：展开后与绘制同源（见 `expand_tabs`）。
  const TabExpansion expanded = expand_tabs(raw);
  const std::string_view prefix = expanded.text;
  // **字体角色必须与绘制一致**（Monospace）：漏传则落到接口默认的 Proportional，
  // 于是“量宽用比例字体、绘字用等宽字体”，光标/选择/查找高亮/缩进线全部对不上字，
  // 且偏移随列号线性累积（实测 45 列处偏 7.7px——用户报的“光标漂移”就是这个）。
  // 这条路径是全部 x 坐标的量尺：`paint_content` 的 pen 递推跟它同源，两者不能分家。
  return text_origin(context).x +
         text_port_of(context).measure_width(prefix, font_size(), text::FontRole::Monospace);
}

auto CodeEditor::index_at_point(const RenderContext& context, math::Point point) const -> std::size_t {
  rebuild_line_geometry(context);
  const float height = line_height(context);
  if (height <= 0.0f) return 0;
  const std::size_t line = line_of_index(0) + static_cast<std::size_t>(
                                                  std::max(0.0f, std::floor((point.y - bounds_.y - kTopPadding + scroll_y_) / height)));
  const auto [begin, end] = line_range(line);
  const TextPort& port = text_port_of(context);
  const float target = point.x - (bounds_.x + gutter_cache_ + kGutterPadding - scroll_x_);
  if (target <= 0.0f) return begin;
  // **先展开制表符再逐字量宽**（与绘制/x_for_index 同一量尺）：
  // 直接把 `\t` 当字形去量会让命中在含 Tab 的行上整体错位。
  const TabExpansion expanded = expand_tabs(std::string_view(text_).substr(begin, end - begin));
  float width = 0.0f;
  std::size_t index = begin;
  while (index < end) {
    const std::size_t next = utf8_next(text_, index);
    const std::size_t from = expanded.map[index - begin];
    const std::size_t to = expanded.map[std::min(next, end) - begin];
    const float advance = port.measure_width(expanded.text.substr(from, to - from), font_size(),
                                             text::FontRole::Monospace);
    if (width + advance * 0.5f > target) return index;
    width += advance;
    index = next;
  }
  return end;
}

/// 鼠标位置落在第几行（不在文本区则 -1）。悬停底纹用（不进入内容缓存）。
auto CodeEditor::hover_line_at(const RenderContext& context, math::Point point) const -> int {
  rebuild_line_geometry(context);
  const float height = line_height_cache_;
  if (height <= 0.0f) return -1;
  if (point.y < bounds_.y + kTopPadding - scroll_y_) return -1;
  if (point.y > bounds_.bottom() - kBottomPadding) return -1;
  const auto line = static_cast<std::ptrdiff_t>(
      std::floor((point.y - bounds_.y - kTopPadding + scroll_y_) / height));
  if (line < 0) return -1;
  const auto total = static_cast<std::ptrdiff_t>(line_count());
  if (line >= total) return -1;
  return static_cast<int>(line);
}

auto CodeEditor::checkbox_line_at(const RenderContext& context, math::Point point) const -> int {
  if (!any_checkbox_ || gutter_cache_ <= 0.0f) return -1;
  // 勾选列占 gutter 的**最左** `kCheckboxColumnWidth`（见绘制处）。
  if (point.x < bounds_.x || point.x > bounds_.x + kCheckboxColumnWidth) return -1;
  // 纵向复用 `hover_line_at`：**行高的权威来源只有一处**，
  // 自己再算一遍必然与绘制分叉（"点框没反应"最常见的成因）。
  return hover_line_at(context, point);
}

/// 水平滚动条矩形（内容溢出时贴在文本区底部；否则为空）。
auto CodeEditor::h_scroll_bar_rect() const -> math::Rect {
  if (bounds_.is_empty()) return {};
  const float track_x = bounds_.x + gutter_cache_;
  const float track_w = std::max(0.0f, bounds_.width - gutter_cache_ - kScrollBarWidth - 4.0f);
  if (track_w <= 0.0f) return {};
  return math::Rect{track_x, bounds_.bottom() - kScrollBarWidth - 2.0f, track_w, kScrollBarWidth};
}

auto CodeEditor::max_scroll_x(const RenderContext& context) const -> float {
  const float widest = max_line_width(context);
  const float view_width = std::max(0.0f, bounds_.width - gutter_cache_ - kGutterPadding);
  return std::max(0.0f, widest + kGutterPadding - view_width + kGutterPadding +
                   kScrollBarWidth);
}

/// 水平条拖拽：指针 x 映到偏移（保留抓取点相对滑块左边的距离）。
/// 注：按「滑块行程比例」映射，而非把指针位置直接当偏移——否则滑块一到右端就自相矛盾。
void CodeEditor::apply_h_scroll_drag(const RenderContext& context, float pointer_x) {
  rebuild_line_geometry(context);
  const math::Rect track = h_scroll_bar_rect();
  const float max_x = max_scroll_x(context);
  if (track.width <= 0.0f || max_x <= 0.0f) return;
  const float view = std::max(1.0f, bounds_.width - gutter_cache_ - kGutterPadding);
  const float content = max_x + view;
  const float thumb_w = std::max(24.0f, track.width * std::min(1.0f, view / content));
  const float travel = std::max(1.0f, track.width - thumb_w);
  const float local = std::clamp((pointer_x - track.x - h_drag_offset_) / travel, 0.0f, 1.0f);
  scroll_x_ = local * max_x;
  mark_dirty();
}

auto CodeEditor::matching_bracket() const -> std::optional<std::pair<std::size_t, std::size_t>> {
  const auto bracket_at = [this](std::size_t index) -> char {
    if (index >= text_.size()) return '\0';
    const char raw = text_[index];
    return (raw == '(' || raw == ')' || raw == '[' || raw == ']' || raw == '{' || raw == '}') ? raw
                                                                                              : '\0';
  };
  const auto mate = [](char raw) -> char {
    switch (raw) {
      case '(': return ')';
      case ')': return '(';
      case '[': return ']';
      case ']': return '[';
      case '{': return '}';
      case '}': return '{';
      default: return '\0';
    }
  };  // 光标左右两侧的括号都算（编辑器惯例）
  for (const std::size_t probe : {cursor_, cursor_ > 0 ? utf8_prev(text_, cursor_) : 0}) {
    const char raw = bracket_at(probe);
    if (raw == '\0') continue;
    const char closing = mate(raw);
    const bool forward = (raw == '(' || raw == '[' || raw == '{');
    std::int64_t depth = 0;
    if (forward) {
      for (std::size_t index = probe; index < text_.size(); ++index) {
        const char current = text_[index];
        if (current == raw) {
          ++depth;
        } else if (current == closing) {
          --depth;
          if (depth == 0) return std::pair{probe, index};
        }
      }
    } else {
      std::size_t index = probe == 0 ? 0 : probe;
      while (true) {
        const char current = text_[index];
        if (current == raw) {
          ++depth;
        } else if (current == closing) {
          --depth;
          if (depth == 0) return std::pair{probe, index};
        }
        if (index == 0) break;
        index = utf8_prev(text_, index);
      }
    }
    return std::nullopt;
  }
  return std::nullopt;
}

void CodeEditor::ensure_cursor_visible(const RenderContext& context) {
  rebuild_line_geometry(context);
  const float height = line_height(context);
  const float cursor_y = static_cast<float>(line_of_index(cursor_)) * height;
  const float view_height = std::max(0.0f, bounds_.height - kTopPadding - kBottomPadding);
  if (cursor_y - kTopPadding < scroll_y_) {
    scroll_y_ = std::max(0.0f, cursor_y - kTopPadding);
  } else if (cursor_y + height > scroll_y_ + view_height) {
    scroll_y_ = cursor_y + height - view_height + kTopPadding;
  }
  // 水平方向：保证光标可见
  //
  // ⚠ `x_for_index` 已经含 `- scroll_x_`（走 `text_origin`），所以这里要把它
  // **加回去**得到“无滚动时的列位置”。旧写法直接减基准点，而 `x_for_index`
  // 那时不含滚动——两边改一处就会变成 `width - 2*scroll_x` 的错值，
  // 表现为“横向滚完后光标突然跑掉/消失”。
  const float gutter_right_now = show_line_numbers_ ? gutter_cache_ : 0.0f;
  const float caret_view_x = x_for_index(context, cursor_) - (bounds_.x + gutter_right_now + kGutterPadding);
  const float view_width = std::max(0.0f, bounds_.width - gutter_cache_ - kGutterPadding * 2.0f);
  if (caret_view_x < 0.0f) {
    scroll_x_ = std::max(0.0f, scroll_x_ + caret_view_x - 8.0f);
  } else if (caret_view_x > view_width) {
    scroll_x_ = scroll_x_ + caret_view_x - view_width + 8.0f;
  }
  // 统一夹取：光标在文末时 `scroll_y_` 不得把视口推过内容底（否则最后几行全白）
  clamp_scroll(context);
}

void CodeEditor::set_scroll_offset(float x, float y) {
  scroll_x_ = std::max(0.0f, x);
  scroll_y_ = std::max(0.0f, y);
  mark_dirty();
}

/// 上下标量滚动夹取（垂直/水平共用一处）——`scroll_to_line`/滚轮/动作面全部经此。
///
/// 之前只有滚轮路径做了夹取，`scroll_to_line` 直接赋值 `line * line_height`：
/// 超过内容高度时画出**空白屏**（视口越过最后一行、什么都不显示）——
/// 这是控制通道 `scroll_to_line` 与示例“跳转到行”都踩得到的真缺陷。
void CodeEditor::clamp_scroll(const RenderContext& context) {
  const float max_y = std::max(0.0f, content_height() - bounds_.height);
  scroll_y_ = std::clamp(scroll_y_, 0.0f, max_y);
  const float view_width = std::max(0.0f, bounds_.width - gutter_cache_ - kGutterPadding);
  const float max_x = std::max(0.0f, content_width(context) - gutter_cache_ - kGutterPadding * 2.0f -
                                         view_width + kScrollBarWidth);
  scroll_x_ = std::clamp(scroll_x_, 0.0f, max_x);
}

void CodeEditor::scroll_by(float dx, float dy) {
  const float before_x = scroll_x_;
  const float before_y = scroll_y_;
  scroll_x_ = std::max(0.0f, scroll_x_ + dx);
  scroll_y_ = std::max(0.0f, scroll_y_ + dy);
  mark_dirty();
  notify_scroll(before_x, before_y);
}

void CodeEditor::scroll_to_line(std::size_t line) {
  const std::size_t clamped = std::min(line == 0 ? 0 : line - 1, line_count() - 1);
  const float before_y = scroll_y_;
  scroll_y_ = static_cast<float>(clamped) * line_height_cache_;
  mark_dirty();
  notify_scroll(scroll_x_, before_y);
}

auto CodeEditor::first_visible_line(const RenderContext& context) const -> std::size_t {
  rebuild_line_geometry(context);
  const float height = line_height_cache_;
  if (height <= 0.0f) return 1;
  // 取 ceil：问的是“用户看到的第一行”，而不是“完全未被上边缘切到的第一行”。
  // `scroll_to_line(n)` 后本值应正好是 n（测试钉死这条口径）。
  const float top = std::max(0.0f, scroll_y_ - kTopPadding);
  const auto first = static_cast<std::size_t>(std::ceil(static_cast<double>(top / height)));
  return std::min(first, line_count() - 1) + 1;
}

auto CodeEditor::visible_line_count(const RenderContext& context) const -> std::size_t {
  rebuild_line_geometry(context);
  const float height = line_height_cache_;
  if (height <= 0.0f) return line_count();
  const auto count = static_cast<std::size_t>(std::max(1.0f, bounds_.height / height));
  return std::max<std::size_t>(1, count);
}

auto CodeEditor::caret_offset_x(const RenderContext& context) const -> float {
  return x_for_index(context, cursor_) - bounds_.x;
}

auto CodeEditor::caret_screen_rect() const noexcept -> math::Rect {
  if (bounds_.is_empty() || line_height_cache_ <= 0.0f) return math::Rect{};
  const std::size_t line = line_of_index(cursor_);
  const float row_top = bounds_.y + kTopPadding - scroll_y_ + static_cast<float>(line) * line_height_cache_;
  const std::size_t line_begin = line < line_spans_.size() ? line_spans_[line].first : 0;
  const std::size_t column = cursor_ >= line_begin ? cursor_ - line_begin : 0;
  // 列 → 像素：按**平均字宽**估（`max_line_width_cache_` 是整篇最长行的宽度，
  // 拿它算单列会偏差很大）。等宽字体下"一个字符约 0.6 个字号"是稳定近似——
  // 弹层定位差几像素无妨（它本来就贴在光标下方，不是精确锚定到字符）。
  const float char_width = std::max(4.0f, font_size() * 0.6f);
  // 本函数无 `RenderContext`（`noexcept`、供宿主在任意时点调），
  // 不能走 `text_origin(context)`，只能就地算——
  // 公式必须与 `text_origin` 一致（含 `- scroll_x_`），否则横向滚动后会偏。
  const float x = bounds_.x + gutter_cache_ + kGutterPadding - scroll_x_ +
                  static_cast<float>(column) * char_width;
  return math::Rect{x, row_top, std::max(1.0f, char_width), line_height_cache_};
}

auto CodeEditor::last_visible_line(const RenderContext& context) const -> std::size_t {
  const std::size_t first = first_visible_line(context);
  const std::size_t count = visible_line_count(context);
  return std::min(first + count - 1, line_count());
}

// ————————————————————————————————————————————————————————————————————————————
// 布局与绘制
// ————————————————————————————————————————————————————————————————————————————

void CodeEditor::measure(const RenderContext& context, const Constraints& constraints) {
  const float height = text_port_of(context).line_height(font_size());
  const float rows = std::min(static_cast<float>(line_count()), 24.0f);
  const float natural_height = rows * height + kTopPadding + kBottomPadding;
  measured_ = math::Size{constraints.max_width, std::min(constraints.max_height, std::max(80.0f, natural_height))};
  (void)context;
}

void CodeEditor::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  geometry_dirty_ = true;
  // 行高缓存可能在本次 arrange 之前已由绘制填过；若还没有，这儿补上
  // （仅当上下文有可用文本端口时——属性面的视口推算依赖它，见 get_property）。
  if (line_height_cache_ <= 0.0f && context.text != nullptr) {
    line_height_cache_ = context.text->line_height(font_size()) * line_spacing_;
  }
  (void)context;
}

// ————————————————————————————————————————————————————————————————————————————
// 诊断装饰（LSP 阶段 3）
// ————————————————————————————————————————————————————————————————————————————

void CodeEditor::set_diagnostics(std::vector<DiagnosticMark> marks) {
  // 排序（按起点）：绘制与命中都依赖有序——乱序输入不该让下面每条查询自己排。
  std::sort(marks.begin(), marks.end(),
            [](const DiagnosticMark& a, const DiagnosticMark& b) {
              if (a.begin != b.begin) return a.begin < b.begin;
              return a.end < b.end;
            });
  diagnostics_ = std::move(marks);
  hover_diagnostic_index_.reset();   // 内容换了：旧的悬浮指向可能已越界
  mark_layout_dirty();
  mark_dirty();
}

void CodeEditor::clear_diagnostics() {
  diagnostics_.clear();
  hover_diagnostic_index_.reset();
  mark_layout_dirty();
  mark_dirty();
}

auto CodeEditor::severity_on_line(std::size_t line) const -> std::optional<DiagnosticSeverity> {
  if (diagnostics_.empty() || line >= line_spans_.size()) return std::nullopt;
  const auto [line_begin, line_end] = line_spans_[line];
  std::optional<DiagnosticSeverity> worst{};
  for (const auto& mark : diagnostics_) {
    if (mark.begin >= line_end && line_end > line_begin) break;   // 已过本行（有序）
    // 与本行区间相交（空范围诊断按它所在行算）。
    const bool inside = mark.begin >= line_begin && mark.begin < line_end;
    const bool covers = mark.begin <= line_begin && mark.end >= line_end;
    if (!inside && !covers) continue;
    if (!worst.has_value() || static_cast<int>(mark.severity) < static_cast<int>(*worst)) {
      worst = mark.severity;
    }
  }
  return worst;
}

auto CodeEditor::diagnostic_at(std::size_t offset) const -> const DiagnosticMark* {
  // 取**最具体**的一条：诊断常嵌套（外层语法错、内层语义错），鼠标停在内层时
  // 用户想看到的是内层那条（与 VSCode 一致）。
  //
  // "更具体"的定义有坑：**空范围（整行）诊断不算具体**——它覆盖整行却没有明确的
  // 字符区间，若按"范围长度最小"排序，一个 0 长度的整行警告会**盖掉**同行的
  // 具体错误（实测：`missing` 上的错误被整行警告顶掉，因为 0 < 7）。
  // 规则：有区间的优先于空区间的；都有区间时取更短的。
  const DiagnosticMark* best = nullptr;
  const auto specificity = [](const DiagnosticMark& mark) -> std::size_t {
    if (mark.end == mark.begin) return std::numeric_limits<std::size_t>::max();   // 最不具体
    return mark.end - mark.begin;
  };
  for (const auto& mark : diagnostics_) {
    if (mark.begin > offset) break;   // 有序：后续都更靠后
    const bool hit = offset >= mark.begin && (offset < mark.end || mark.end == mark.begin);
    if (!hit) continue;
    // 注意：`specificity` 大 = 不具体，所以这里取小的。
    if (best == nullptr || specificity(mark) < specificity(*best)) best = &mark;
  }
  return best;
}

auto CodeEditor::diagnostic_at_point(const RenderContext& context, math::Point point) const
    -> const DiagnosticMark* {
  if (bounds_.is_empty() || !bounds_.contains(point)) return nullptr;
  return diagnostic_at(index_at_point(context, point));
}

void CodeEditor::set_hover_diagnostic(const DiagnosticMark* mark) {
  if (mark == nullptr) {
    if (hover_diagnostic_index_.has_value()) {
      hover_diagnostic_index_.reset();
      mark_dirty();
    }
    return;
  }
  for (std::size_t index = 0; index < diagnostics_.size(); ++index) {
    if (&diagnostics_[index] == mark) {
      if (!hover_diagnostic_index_.has_value() || *hover_diagnostic_index_ != index) {
        hover_diagnostic_index_ = index;
        mark_dirty();
      }
      return;
    }
  }
}

void CodeEditor::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const SyntaxPalette& syntax = context.theme.syntax();
  const Palette& colors = context.theme.colors();
  rebuild_line_geometry(context);
  const TextPort& port = text_port_of(context);
  const float height = line_height_cache_;
  if (height <= 0.0f) return;

  canvas.push_clip_rect(bounds_);
  // 正文绘制的**左边界**（行号栏右缘）：横向滚动时正文不能滑到行号下面。
  //
  // `origin_x` 含 `- scroll_x_`，滚到后面时文字会跑到行号栏区域。
  // 按需求：“编辑器滚动横向时不要滚动到行号下”。
  // 实际裁剪在 `draw_span` 里（只包正文，不包行号）——见那里的说明。
  const float gutter_right = show_line_numbers_ ? bounds_.x + gutter_cache_ : bounds_.x;
  // **与 `x_for_index` 同一量尺**：绘制与光标/选区/命中都读 `text_origin`，
  // 两边各写一份公式就会漂（历史上“光标偏了 7.7px”就是这么来的）。
  const math::Point text_start = text_origin(context);
  const float origin_x = text_start.x;
  const float origin_y = text_start.y;

  // 行号槽底：**与编辑区（内容区）同色**。
  //
  // 不能写死单一 token：编辑区是 `Surface::Content`，而它按主题取不同层
  //（见 `Element::apply_surface`）：暗色 → `surface_sunken`（#0D0D10）、
  // 亮色 → `surface_raised`（纯白）。若写死 `surface_sunken`，
  // 暗色对了但亮色下行号栏会是一条深灰竖带（实测 #DCDCE5 vs 编辑区纯白）。
  //
  // 这里与 `apply_surface` 的 Content 分支**同一判定**，保证两主题下都与编辑区同底。
  const auto content_bg = context.theme.mode() == st::ui::ThemeMode::Dark
                              ? colors.surface_sunken
                              : colors.surface_raised;
  if (show_line_numbers_ && gutter_cache_ > 0.0f) {
    canvas.fill_rect(math::Rect{bounds_.x, bounds_.y, gutter_cache_, bounds_.height},
                     raster::Paint::solid(content_bg), 0.0f);
  }

  const auto [first_line, last_line] = visible_line_range(context);
  // 横向滚动的裁剪在**正文绘制处**（`draw_span`）做，不在这一层：
  // 行号 / 当前行底色 / gutter 竖条与正文在同一个 for 循环里交织，
  // 在这一层一刀切会把行号也裁掉（实测：行号整列消失）。
  // 记下本帧的可见行范围（1 起）供宿主在帧首取用（`visible_line_range_now()`）。
  last_visible_first_ = first_line + 1;
  last_visible_last_ = last_line + 1;
  // **一行的几何全部从框架拿**（`ui::LineGeometry`，见 `line_layout.hpp`）。
  //
  // 三个量都相对**行盒顶**，所以每行只需 `row_top + 它`：
  //   `origin_y`   → 文字绘制的落位（`TextPort::draw` 收的就是这个）
  //   `ink_top`    → 高亮带/光标/缩进参考线的顶
  //   `ink_height` → 上述元素的高度
  //
  // 本组件**不再自己算任何偏移**。这里曾有一组组件自己的推导，逐项被用户报出来的：
  // ① “行间距全在行下方”→ 行距增量只加在盒底；
  // ② “背景和光标还是偏下”→ 高亮带撑满行盒（22.77）而一行代码的墨迹只有 ~15px，
  //    文字上方必然空一截；
  // ③ 两者都是为了补框架缺位而在应用层打的补丁（违反“框架问题不在应用层修”）。
  // 现统一为：带**贴合墨迹**（`ink_top`/`ink_height`），文字按 `origin_y` 落位。
  const ui::LineGeometry line_geometry = line_geometry_cache_;
  const float text_offset = line_geometry.origin_y;   // 向后兼容的名字（本函数内部）
  const float band_top = line_geometry.ink_top;
  const float band_height = line_geometry.ink_height;
  const std::size_t current = line_of_index(cursor_);
  const auto [sel_begin, sel_end] = selection();
  const auto brackets = focused_ ? matching_bracket() : std::nullopt;

  for (std::size_t line = first_line; line < last_line && line < line_spans_.size(); ++line) {
    const float row_top = origin_y + static_cast<float>(line) * height;
    const auto [begin, end] = line_spans_[line];
    const std::string_view row = std::string_view(text_).substr(begin, end - begin);
    // **本行所有 x 坐标的唯一量尺**：制表符先展开、所有偏移经 `expanded.map` 换算。
    // 选择/命中高亮/缩进参考线/token 递推各自量宽都会因 `\t` 而错位，
    // 统一到这一处后它们天然同源。
    const TabExpansion expanded = expand_tabs(row);
    const auto x_at = [&](std::size_t offset) -> float {
      const std::size_t at = std::min(offset, row.size());
      const std::size_t mapped = expanded.map[at];
      return origin_x + port.measure_width(std::string_view(expanded.text).substr(0, mapped),
                                           font_size(), text::FontRole::Monospace);
    };

    // 悬停行底纹（最淡的一层；当前行与选择压在它上面）
    if (static_cast<int>(line) == hover_line_ && line != current) {
      canvas.fill_rect(math::Rect{bounds_.x, row_top + band_top, bounds_.width, band_height},
                       raster::Paint::solid(colors.surface_alt), 0.0f);
    }

    // —— 行装饰（差异视图 / Git gutter）——
    //
    // 顺序刻意如此：装饰带在**悬停底纹之上**、当前行底色**之下**——
    // 三层同时存在时，“光标在哪一行”仍应是一眼可见的那一层（当前行带是主题
    // 专门为此选的强色），而增/删色只作底；反过来会被行带盖成一片，
    // 用户在差异视图里就分不清“我正在看哪一行”。
    const LineDecoration* decoration = nullptr;
    // ⚠ 索引差一：循环里的 `line` 是 **0 基**，而装饰按**人读的 1 起行号**指定
    //（与 `goto_line`/`diagnostic` 的属性面同一套坐标系）。
    const std::size_t line_number = line + 1;
    if (line_number < line_mark_index_.size() && line_mark_index_[line_number] != 0) {
      decoration = &line_marks_[line_mark_index_[line_number] - 1];
    }
    if (decoration != nullptr && decoration->mark != LineMark::None) {
      math::Color ink{};
      bool use_ink = true;
      switch (decoration->mark) {
        case LineMark::Added:
        case LineMark::Modified: ink = syntax.inserted; break;
        case LineMark::Removed: ink = syntax.deleted; break;
        case LineMark::Filler: {
          // 对齐填充行：用**远弱一档**的底（它是“这里没有内容”，不是“这里有改动”）——
          // 与真实行同强度会让人以为对方多了一段内容。
          ink = colors.surface_alt.with_alpha_f(0.45F);
          break;
        }
        case LineMark::None: use_ink = false; break;
      }
      if (use_ink) {
        // 带**撑满行盒**（`0 .. height`），与选择底色同一口径：
        // 差异带是一条连续的条带，用行带（`ink_top/ink_height`）会在行间留缝，看着像“条纹”。
        canvas.fill_rect(math::Rect{bounds_.x, row_top, bounds_.width, height},
                         raster::Paint::solid(ink.with_alpha_f(0.16F)), 0.0f);
        if (show_line_numbers_ && gutter_cache_ > 0.0f) {
          canvas.fill_rect(math::Rect{bounds_.x, row_top, gutter_cache_, height},
                           raster::Paint::solid(ink.with_alpha_f(0.16F)), 0.0f);
          if (decoration->gutter_bar) {
            // 槽内竖条：贴 gutter 右缘的 3px 实心条（IDEA/VSCode 同款位置；左侧让给勾选框）。
            canvas.fill_rect(math::Rect{bounds_.x + gutter_cache_ - 3.0f, row_top, 3.0f, height},
                             raster::Paint::solid(ink), 0.0f);
          }
        }
      }
    }

      // 勾选框：**与时序（`LineMark`）无关地画**。
      //
      // 它原本嵌在 `if (use_ink)` 里，于是 `LineMark::None` 的行（正是「没改动的行」）
      // 永远不画框——而用户要选的恰好是改动行，看起来就像「勾选框根本不会画」
      //（实测踩到：像素断言显示开/关模式画面完全一样，探针也一条都没打）。
      //
      // 它在 gutter **最左**（槽内竖条已让到右缘，两者不叠）。
      if (decoration != nullptr && decoration->check != LineDecoration::Check::None) {
        const float box = std::max(9.0f, std::min(13.0f, height * 0.62f));
        const math::Rect box_rect{bounds_.x + kGutterPadding * 0.5f,
                                  row_top + (height - box) * 0.5f, box, box};
        const bool checked = decoration->check == LineDecoration::Check::Checked;
        const bool disabled = decoration->check == LineDecoration::Check::Disabled;
        const math::Color edge = disabled ? colors.text_muted.with_alpha_f(0.5F)
                                          : (checked ? colors.primary : colors.text_muted);
        canvas.fill_rect(box_rect,
                         raster::Paint::solid(checked && !disabled
                                                  ? colors.primary.with_alpha_f(0.9F)
                                                  : colors.surface),
                         2.0f);
        // 边框用四道 1px 实心条画（框架的描边只有 `stroke_path`，为一个方框引 Path
        // 不划算；而 1px 的四条与真描边在视觉上不可分辨）。
        const raster::Paint edge_paint = raster::Paint::solid(edge);
        canvas.fill_rect(math::Rect{box_rect.x, box_rect.y, box_rect.width, 1.0f}, edge_paint, 0.0f);
        canvas.fill_rect(
            math::Rect{box_rect.x, box_rect.bottom() - 1.0f, box_rect.width, 1.0f}, edge_paint,
            0.0f);
        canvas.fill_rect(math::Rect{box_rect.x, box_rect.y, 1.0f, box_rect.height}, edge_paint,
                         0.0f);
        canvas.fill_rect(
            math::Rect{box_rect.right() - 1.0f, box_rect.y, 1.0f, box_rect.height}, edge_paint,
            0.0f);
        if (checked && !disabled) {
          // 勾：用一串小方块沿两段斜线铺（不依赖图标资源——无图标后端也要能看）。
          const raster::Paint tick = raster::Paint::solid(colors.on_primary);
          const math::Point tick_start{box_rect.x + box * 0.24f, box_rect.y + box * 0.50f};
          const math::Point tick_corner{box_rect.x + box * 0.44f, box_rect.y + box * 0.72f};
          const math::Point tick_finish{box_rect.x + box * 0.78f, box_rect.y + box * 0.26f};
          const auto draw_tick = [&canvas, tick](math::Point from, math::Point to) {
            constexpr int kSteps = 8;
            for (int step = 0; step <= kSteps; ++step) {
              const float t = static_cast<float>(step) / static_cast<float>(kSteps);
              const float x = from.x + (to.x - from.x) * t;
              const float y = from.y + (to.y - from.y) * t;
              canvas.fill_rect(math::Rect{x - 0.9f, y - 0.9f, 1.8f, 1.8f}, tick, 0.0f);
            }
          };
          draw_tick(tick_start, tick_corner);
          draw_tick(tick_corner, tick_finish);
        }
      }

    // 当前行底色 + 行号槽内的当前行指示（行号槽高亮 = VSCode 的“我在哪一行”锚点）
    if (line == current) {
      // **贴合墨迹**（`band_top`/`band_height`）：撑满行盒会让文字上方空一截，
      // 看着就是“背景偏下”（见 `band_top` 处的说明）。
      canvas.fill_rect(math::Rect{bounds_.x, row_top + band_top, bounds_.width, band_height},
                       raster::Paint::solid(syntax.current_line), 0.0f);
      if (show_line_numbers_ && gutter_cache_ > 0.0f) {
        canvas.fill_rect(math::Rect{bounds_.x, row_top + band_top, gutter_cache_, band_height},
                         raster::Paint::solid(syntax.current_line), 0.0f);
      }
    }

    // 查找命中高亮（先于选择：选中态压在命中态上）
    if (find_enabled_ && !find_matches_.empty()) {
      // 只遍历**本行**的命中（见 `find_line_ranges_` 的说明：
      // 旧写法是每行扫全部命中，命中数 100 万时单帧 paint 涨 4.2 倍）。
      const auto [hit_first, hit_last] = find_range_of_line(line);
      for (std::size_t m = hit_first; m < hit_last; ++m) {
        const auto& [hit_begin, hit_end] = find_matches_[m];
        // 与该行有交集才画（跨行命中时这里仍会过滤，是安全网）。
        if (hit_end <= begin || hit_begin >= end) continue;
        const std::size_t from = std::max(hit_begin, begin);
        const std::size_t to = std::min(hit_end, end);
        const float x0 = x_at(from - begin);
        const float x1 = x_at(to - begin);
        canvas.fill_rect(math::Rect{x0, row_top, std::max(x1 - x0, 2.0f), height},
                         raster::Paint::solid(m == find_active_ ? syntax.find_active
                                                                : syntax.find_highlight),
                         2.0f);
      }
    }

    // **行内改动区间**（“这个字改了”）：画在字符**之下**、选择与查找之上。
    //
    // 为何在字符之下：底色要先铺好、再把字压上去（反过来会把字盖住）。
    // 为何在选择之下：选择是“用户当前选了什么”，优先级高于“这行哪里改了”——
    // 两层同时存在时用户要先看清自己的选区（与 VSCode 一致）。
    if (decoration != nullptr && !decoration->inline_ranges.empty()) {
      // 行内差异只存在于**改行**上：用该行的带色（增/删）而不是另开一个色，
      // 这样“行改”与“字改”是同一色的两层强度，观感是一个整体。
      math::Color ink{};
      if (decoration->mark == LineMark::Added || decoration->mark == LineMark::Modified) {
        ink = syntax.inserted;
      } else if (decoration->mark == LineMark::Removed) {
        ink = syntax.deleted;
      } else {
        ink = syntax.inserted;   // 无行级标记时（如宿主直接给行内区间）按“改动”色
      }
      for (const auto& [from, to] : decoration->inline_ranges) {
        if (to <= from || to > (end - begin)) continue;   // 越界区间直接跳过（不画半截）
        const float x0 = x_at(from);
        const float x1 = x_at(to);
        // 行盒高（不是墨迹高）：让底色连成一条，不然多段区间看着是“几个小方块”。
        canvas.fill_rect(math::Rect{x0, row_top, std::max(x1 - x0, 1.0f), height},
                         raster::Paint::solid(ink.with_alpha_f(0.32F)), 0.0f);
      }
    }

    // 选择底色（该行与选择区间的交集）
    if (sel_begin != sel_end) {
      const std::size_t slice_begin = std::max(sel_begin, begin);
      const std::size_t slice_end = std::min(sel_end, end + (sel_end > end ? 1U : 0U));
      if (slice_end > slice_begin || (sel_begin <= begin && sel_end >= end + 1)) {
        const std::size_t from = std::max(slice_begin, begin);
        const std::size_t to = std::min(std::max(slice_end, from), end);
        const float x0 = x_at(from < begin ? 0 : from - begin);
        const float x1 = x_at(to < begin ? 0 : to - begin);
        const float width = std::max(x1 - x0, (to == from && to < end) ? 2.0f : 0.0f);
        if (width > 0.0f) {
          canvas.fill_rect(math::Rect{x0, row_top, width, height},
                           raster::Paint::solid(syntax.selection), 0.0f);
        }
      }
    }

    // 括号配对底色
    if (brackets.has_value()) {
      for (const std::size_t position : {brackets->first, brackets->second}) {
        if (position < begin || position >= end) continue;
        const float x0 = x_at(position < begin ? 0 : position - begin);
        const std::size_t next = utf8_next(text_, position);
        const float x1 = x_at(next < begin ? 0 : std::min(next, end) - begin);
        canvas.fill_rect(math::Rect{x0 - 1.0f, row_top, x1 - x0 + 2.0f, height},
                         raster::Paint::solid(syntax.matching_bracket), 2.0f);
      }
    }

    // 缩进参考线：每级缩进一条竖线（视觉列对齐——Tab 按 `tab_width` 展开）。
    //
    // 颜色取 `border_strong`（而不是 `border`）：实测 `border` 在浅色主题下
    // 只带来 ~7% 的亮度变化（AA 后又只剩一半），13.5px 代码上基本看不见——
    // “缩进参考线”这类装饰“看不见就等于没有”。亮度差拉到 ~25% 既不抢正文，
    // 又能一眼看出缩进层次（与主流编辑器同量级）。
    if (indent_guides_ && !row.empty()) {
      const float space_w = text_port_of(context).measure_width(" ", font_size(),
                                                               text::FontRole::Monospace);
      const std::size_t unit = static_cast<std::size_t>(std::max(1, tab_width_));
      // 缩进宽度按**展开后**的视觉列算（与绘制/命中同一量尺）——原先 Tab 按
      // `tab_width`、空格按 1 各自累加，含 Tab 的行参考线会与字符错位。
      std::size_t visual = 0;
      for (std::size_t probe = begin; probe < end; ++probe) {
        const char raw = text_[probe];
        if (raw != ' ' && raw != '\t') break;
        const std::size_t previous_level = visual / unit;
        visual += raw == '\t' ? unit - (visual % unit) : 1U;
        const std::size_t level = visual / unit;
        for (std::size_t step = previous_level; step < level; ++step) {
          const float x = origin_x + static_cast<float>((step + 1) * unit) * space_w - 0.5f;
          canvas.fill_rect(math::Rect{x, row_top, 1.0f, height},
                           raster::Paint::solid(colors.border_strong), 0.0f);
        }
      }
    }

    // token 着色绘制：**统一走展开后的字形串**（`\t` 已变成空格）——
    // 直接 `draw(row)` 会把制表符交给字体自己处理，与上方量尺不同源。
    float pen = origin_x;
    const auto& tokens = line < line_tokens_.size() ? line_tokens_[line] : LineTokens{};
    const auto draw_span = [&](std::size_t from, std::size_t to, math::Color color) {
      const std::size_t mapped_from = expanded.map[std::min(from, row.size())];
      const std::size_t mapped_to = expanded.map[std::min(to, row.size())];
      if (mapped_to <= mapped_from) return;
      const std::string_view slice =
          std::string_view(expanded.text).substr(mapped_from, mapped_to - mapped_from);
      // **正文只画在行号栏右侧**：横向滚动时文字不能滑到行号下面。
      //
      // 为何在这层设子裁剪（而不是整个 `paint_content` 一层）：行号、当前行底色、
      // gutter 竖条与本循环里的装饰是**交织绘制**的（同一个 for），一刀切会把
      // 行号也裁掉（实测：裁剪放在外层后行号整列消失）。这里只包住正文，
      // 代价是每段一次 push/pop（每行 token 数级别，十几项）。
      const bool clip_text = gutter_right > bounds_.x;
      if (clip_text) {
        canvas.push_clip_rect(math::Rect{gutter_right, bounds_.y,
                                         bounds_.right() - gutter_right, bounds_.height});
      }
      port.draw(canvas, slice, math::Point{pen, row_top + text_offset}, font_size(), color,
                text::FontRole::Monospace);
      if (clip_text) canvas.pop_clip();
      pen += port.measure_width(slice, font_size(), text::FontRole::Monospace);
    };
    if (tokens.empty()) {
      draw_span(0, row.size(), syntax.plain);
    } else {
      std::size_t consumed = 0;
      for (const auto& token : tokens) {
        const std::size_t token_begin = std::min(token.begin, row.size());
        if (token_begin > consumed) draw_span(consumed, token_begin, syntax.plain);
        const std::size_t length = std::min(token.end, row.size()) - token_begin;
        if (length == 0) continue;
        draw_span(token_begin, token_begin + length, token_color(syntax, token.kind));
        consumed = token_begin + length;
      }
      if (consumed < row.size()) draw_span(consumed, row.size(), syntax.plain);
    }

    // —— 行尾注解（`LineDecoration::end_text`：blame 作者/日期这类灰字）——
    //
    // 位置 = **本行文本之后**（跟行一起滚动，才不会让人看错行）。
    //
    // ⚠ 旧实现"装不下就整条不画"（要求 `ellipsize` 后长度不变），在**真实代码行**上
    // 等于**永远不画**：一行的代码宽度通常已接近视口右边，`作者 日期` 那十几个字
    // 根本没有空位——用户看到的就是"开了 blame 什么也没变"（实测：`read_blame`
    // 返回 27 行、界面一条都没有）。短行的样例文件里它能画出来，所以既有 e2e
    // （临时仓库里是 `int main() {` 这种短行）一直是绿的。
    //
    // 现在的取法：仍优先贴在文本之后；**若那里装不下完整注解，就把它右对齐到视口
    // 右边缘**——只要那一带没有被本行文字占用，就能完整显示在"这一行"的右侧空白区。
    // 两个位置都试过还装不下才不画（那种情况是真的没地方放，比如行文本本身已到边）。
    if (decoration != nullptr && !decoration->end_text.empty()) {
      const float font_sm = context.theme.metrics().font_sm;
      const float text_right = pen;
      const float hard_right = bounds_.right() - kEndAnnotationRightPad;
      const float width = port.measure_width(decoration->end_text, font_sm);
      float annotation_left = pen + kEndAnnotationGap;
      if (annotation_left + width > hard_right) {
        // 退一步：右对齐到视口右边缘（不与本行文字重叠才算可用）。
        annotation_left = hard_right - width;
      }
      const bool fits = annotation_left >= text_right + kEndAnnotationGap * 0.5f &&
                        annotation_left + width <= hard_right && annotation_left >= bounds_.x;
      if (fits) {
        port.draw(canvas, decoration->end_text, math::Point{annotation_left, row_top + text_offset},
                  font_sm, colors.text_faint);
      }
    }

    // 诊断波浪线：画在**字形之上**（先画就被字符盖住了）。
    //
    // 形态：沿基线的锯齿（VSCode 同款）。用 1px 竖线逐列拼而不是画一条曲线——
    // 框架的路径描边在小尺寸上会有半像素模糊，而锯齿需要"锋利的方波"观感。
    // 行内只有 1px 高，宽度换得的是"远看是一条彩色下划线、近看是波浪"的效果。
    if (!diagnostics_.empty()) {
      for (const auto& mark : diagnostics_) {
        if (mark.begin > end) break;                       // 有序：后续都更靠后
        if (mark.end < begin && mark.end != mark.begin) continue;
        // 与本行的交集（空范围诊断 = 整行）。
        std::size_t from = std::max(mark.begin, begin);
        std::size_t to = mark.end == mark.begin ? end : std::min(mark.end, end);
        if (to < from) to = from;
        // 端点在本行之外时夹到行首/行尾，避免波浪线越出行宽。
        if (mark.begin < begin) from = begin;
        const float x0 = from == begin ? origin_x : x_at(from - begin);
        float x1 = to == end ? origin_x + static_cast<float>(end - begin) * 0.0f : x_at(to - begin);
        if (to == end) {
          // 行末：按整行文本宽度算（`x_at` 需要行内下标，用展开串长度）。
          x1 = origin_x + port.measure_width(std::string_view(expanded.text), font_size(),
                                             text::FontRole::Monospace);
        }
        const float width = std::max(2.0f, x1 - x0);
        const math::Color ink = diagnostic_color(context.theme, mark.severity, colors);
        const float wave_y = row_top + height - 2.0f;
        // 锯齿：2px 周期的方波（4px 一个完整波长，与 VSCode 一致）。
        constexpr float kPeriod = 4.0f;
        std::size_t step = 0;
        for (float pen_x = x0; pen_x < x0 + width; pen_x += 2.0f, ++step) {
          const bool up = (step % 2) == 1;
          const float y = up ? wave_y - 1.0f : wave_y;
          const float segment = std::min(2.0f, x0 + width - pen_x);
          canvas.fill_rect(math::Rect{pen_x, y, segment, 1.0f}, raster::Paint::solid(ink), 0.0f);
        }
        (void)kPeriod;
      }
    }

    // 行号（`decoration->number` 非 0 时用它——并列差异的两栏是同一份文本的两次呈现，
    // 右栏要显示新侧行号、左栏显示旧侧行号；那两串号只有宿主知道）
    if (show_line_numbers_ && gutter_cache_ > 0.0f) {
      // 行号三态：`kNoNumber` = 不显示（分隔条/对齐填充行）、`0` = 用真实行号、
      // 其余 = 宿主指定的号（并列差异的两侧行号）。
      const std::size_t declared = decoration != nullptr ? decoration->number : 0;
      const bool hide_number =
          declared == kNoNumber || (decoration != nullptr && decoration->mark == LineMark::Filler);
      const std::size_t shown = declared != 0 && declared != kNoNumber ? declared : line + 1;
      const std::string number = hide_number ? std::string{} : std::to_string(shown);
      const float number_width = port.measure_width(number, font_size(), text::FontRole::Monospace);
              const math::Color number_color =
          line == current ? syntax.plain : syntax.line_number;
      port.draw(canvas, number,
                math::Point{bounds_.x + gutter_cache_ - kGutterPadding - number_width,
                            row_top + text_offset},
                font_size(), number_color, text::FontRole::Monospace);
      // 行号槽诊断标记：该行有诊断时在槽**最左侧**画一个圆点（VSCode 同款位置）。
      // 为什么不用波浪线替代：波浪线在长行上只覆盖出错的那一段，而"文件里哪几行有问题"
      // 需要一眼看全——槽标记是给"扫一眼"的，波浪线是给"看具体哪里"的。
      if (const auto severity = severity_on_line(line); severity.has_value()) {
        const math::Color ink = diagnostic_color(context.theme, *severity, colors);
        const float dot_y = row_top + height * 0.5f;
        canvas.fill_circle(math::Point{bounds_.x + 6.0f, dot_y}, 3.0f,
                           raster::Paint::solid(ink));
      }
    }
  }

  // 光标（聚焦且可见时）
  if (focused_ && !read_only_) {
    const std::size_t line = line_of_index(cursor_);
    if (line >= first_line && line < last_line) {
      const float row_top = origin_y + static_cast<float>(line) * height;
      const float x = x_for_index(context, cursor_);
      // 光标**严格等于行带**（同顶同高）——它标记当前行，不该超出背景。
      // 原先写作 `band_top - 1` / `band_height + 2`（想让它“看起来到底”），
      // 那是**应用层乱调**：光标上下各伸出带外 1px，一眼就看出“超出背景”。
      //
      // ⚠ 横向滚动后光标 x 可能落到**行号栏区域内**（甚至负值）——
      // 它与正文用同一量尺（`x_for_index` → `text_origin` 含 `- scroll_x_`）。
      // 此时不该把光标画到行号栏上（行号栏的底色在它之后画，会把光标盖掉一半，
      // 看着像“光标不见了”）。判断：完全越过左缘就不画。
      const bool inside = x + kCursorWidth > gutter_right;
      if (inside) {
        canvas.fill_rect(math::Rect{x, row_top + band_top, kCursorWidth, band_height},
                         raster::Paint::solid(syntax.cursor), 0.0f);
      }
    }
  }

  // —— 诊断悬浮提示（在内容之上；不做独立 Element——它是纯提示，不参与布局）——
  //
  // 位置策略：贴着诊断所在行的**下方**（VSCode 同款）；下方空间不够就翻到上方。
  // 宽度按最长行排（上限视口 90%），高度按行数算。
  //
  // 这里原本还有**第二条来源**：LSP 符号悬停（`hover_info_`，鼠标停在符号上的
  // "这个符号是什么"卡片）。2026-10-19 按用户要求**整套删除**——那套卡片要
  // 防抖、要跟词边界、要管消散时机，体感始终不跟手（用户原话"太难用了"）。
  // 诊断提示是另一回事：它只在你指向波浪线时出现、内容就是那条错误，没有争议。
  const DiagnosticMark* hovered_diagnostic = hover_diagnostic();
  if (hovered_diagnostic != nullptr) {
    const Metrics& metrics = context.theme.metrics();
    const std::size_t line = line_of_index(hovered_diagnostic->begin);
    const float row_top = origin_y + static_cast<float>(line) * height;
    const float box_pad = 8.0f;
    const float line_h = port.line_height(metrics.font_sm);
    // 消息按 `\n` 拆行（server 常给多行），外加来源前缀行。
    std::vector<std::string> lines;
    std::string source;
    const std::string body = hovered_diagnostic->message;
    source = hovered_diagnostic->source;
    std::size_t at = 0;
    while (at <= body.size()) {
      const std::size_t next = body.find('\n', at);
      lines.push_back(
          body.substr(at, next == std::string::npos ? std::string::npos : next - at));
      if (next == std::string::npos) break;
      at = next + 1;
    }
    if (!source.empty()) lines.insert(lines.begin(), source);
    // ⚠ 量宽必须用**与绘制同一个 role**（下面 `port.draw` 传的是 `Monospace`）。
    //
    // 旧写法 `measure_width(text, font_sm)` 走默认 `Proportional`，而等宽字体比比例
    // 字体宽——背景框永远偏窄，文字溢出到框外（用户报的「内容溢出背景宽度」）。
    // DPI 只是把差异放大，不是原因。
    float box_width = 0.0f;
    for (const auto& text : lines) {
      box_width = std::max(
          box_width, port.measure_width(text, metrics.font_sm, text::FontRole::Monospace));
    }
    // 长行折行而不是横向截断：诊断消息里的路径/符号名横向截掉就看不懂了。
    // 上限不变（视口 90%），超宽的行按估算字符数硬折（等宽字体下估得准）。
    const float max_text_width = std::max(80.0f, bounds_.width * 0.9f) - box_pad * 2.0f;
    std::vector<std::string> wrapped;
    wrapped.reserve(lines.size());
    for (const auto& text : lines) {
      if (text.empty() ||
          port.measure_width(text, metrics.font_sm, text::FontRole::Monospace) <=
              max_text_width) {
        wrapped.push_back(text);
        continue;
      }
      // 硬折（按显示宽度累加）：用 `utf8_next` 取字符边界，不拆码点。
      std::string folded;
      float used = 0.0f;
      for (std::size_t at_char = 0; at_char < text.size();) {
        const std::size_t next_char = st::utf8_next(text, at_char);
        const std::string_view piece(text.data() + at_char, next_char - at_char);
        const float piece_width =
            port.measure_width(piece, metrics.font_sm, text::FontRole::Monospace);
        if (used + piece_width > max_text_width && !folded.empty()) {
          wrapped.push_back(folded);
          folded.clear();
          used = 0.0f;
        }
        folded.append(piece);
        used += piece_width;
        at_char = next_char;
      }
      if (!folded.empty()) wrapped.push_back(std::move(folded));
    }
    lines = std::move(wrapped);
    const float max_width = std::max(80.0f, bounds_.width * 0.9f);
    box_width = std::min(box_width + box_pad * 2.0f, max_width);
    const float box_height = static_cast<float>(lines.size()) * line_h + box_pad * 2.0f;
    float box_x = std::min(origin_x, bounds_.right() - box_width - 4.0f);
    box_x = std::max(box_x, bounds_.x + 4.0f);
    float box_y = row_top + height + 2.0f;
    if (box_y + box_height > bounds_.bottom() - 4.0f) {
      box_y = row_top - box_height - 2.0f;   // 下方不够：翻到上方
    }
    box_y = std::max(box_y, bounds_.y + 4.0f);
    // 左侧色条：按诊断严重级别取色。
    const math::Color ink =
        diagnostic_color(context.theme, hovered_diagnostic->severity, colors);
    canvas.fill_rect(math::Rect{box_x, box_y, box_width, box_height},
                     raster::Paint::solid(colors.surface_alt), metrics.radius_md);
    canvas.fill_rect(math::Rect{box_x, box_y, 3.0f, box_height}, raster::Paint::solid(ink),
                     metrics.radius_sm);
    float text_y = box_y + box_pad;
    for (const auto& text : lines) {
      port.draw(canvas, text, math::Point{box_x + box_pad, text_y}, metrics.font_sm,
                (!source.empty() && text == source) ? ink : colors.text,
                text::FontRole::Monospace);
      text_y += line_h;
    }
  }

  // —— 滚动条（两条都画；滑块长度与位置由内容/视口比例推出）——
  //
  // 两条滚动条的几何都由 `v_scroll_bar_rect`/`h_scroll_bar_rect` 给出（与命中同一份）。
  const float bar_x = bounds_.right() - kScrollBarWidth - 2.0f;
  const float bar_h = std::max(1.0f, bounds_.height - (max_scroll_x(context) > 0.5f ? kScrollBarWidth : 0.0f));
  const math::Rect v_track = v_scroll_bar_rect(context);
  const math::Rect v_thumb = v_scroll_thumb_rect(context);
  if (v_track.width > 0.0f) {
    canvas.fill_rect(math::Rect{bar_x, bounds_.y, kScrollBarWidth, bar_h},
                     raster::Paint::solid(colors.surface_alt), kScrollBarWidth * 0.5f);
    canvas.fill_rect(math::Rect{v_thumb.x + kScrollBarWidth * 0.5f - 1.5f, v_thumb.y, 3.0f,
                                v_thumb.height},
                     raster::Paint::solid(colors.border_strong), 1.5f);
  }

  const bool has_h = max_scroll_x(context) > 0.5f;
  if (has_h) {
    const math::Rect track = h_scroll_bar_rect();
    canvas.fill_rect(track, raster::Paint::solid(colors.surface_alt), kScrollBarWidth * 0.5f);
    const float max_x = max_scroll_x(context);
    const float view = std::max(1.0f, bounds_.width - gutter_cache_ - kGutterPadding);
    const float ratio = std::min(1.0f, view / (max_x + view));
    const float thumb = std::max(24.0f, track.width * ratio);
    const float travel = std::max(1.0f, track.width - thumb);
    const float offset = std::clamp(scroll_x_ / std::max(1.0f, max_x), 0.0f, 1.0f) * travel;
    canvas.fill_rect(math::Rect{track.x + offset, track.y + track.height * 0.25f, thumb,
                                track.height * 0.5f},
                     raster::Paint::solid(colors.border_strong), track.height * 0.25f);
  }

  canvas.pop_clip();
}

// ————————————————————————————————————————————————————————————————————————————
// 事件
// ————————————————————————————————————————————————————————————————————————————

void CodeEditor::activate() { set_focusable(true); }

void CodeEditor::mark_layout_dirty() {
  // **不向上冒泡**：本组件的几何只取决于自身 `bounds_` 与**解析后的字号**（行高/最大行宽
  // 都是惰性重算的），父容器不需要重新 measure/arrange 它。
  //
  // 为什么必须这么做：基类实现会把 `layout_dirty` 一路冒泡到根元素，而
  // `UiRoot::layout()` 的进入条件是 `dirty_ || tree_layout_dirty()`、尾部又无条件
  // `pending_full_ = true`——于是在编辑器里改一个字就会导致**整帧重绘**。
  // 实测代价（gbcode，1280×800）：整帧 paint **11.5 ms** vs 局部帧子毫秒；
  // 控制通道上表现为 `invoke` p50 **15.9 ms** 而读操作只有 4.1 ms——
  // 差值就是“写操作触发的那一帧”。增量重绘（损坏区）机制因此形同虚设。
  layout_dirty_ = true;
  dirty_ = true;
  mark_dirty();
}

auto CodeEditor::on_event(const RenderContext& context, Event& event) -> bool {
  if (!enabled()) return false;
  switch (event.kind) {
    case EventKind::MouseDown: {
      rebuild_line_geometry(context);
      // **右键不改光标**：它属于“上下文菜单”这条独立语义（主流编辑器同款）。
      // 先于滚动条判定——右键在滚动条上也不应触发拖拽滑块。
      if (event.button == 2) {
        if (on_context_menu) on_context_menu(event.position);
        return true;
      }
      // 勾选框优先于一切：它是**按钮式**语义（点一下切换，光标不动）。
      // 排在滚动条之前是因为它在 gutter 里、与滚动条不重叠；排在这里只是为了让
      // "点框"这件事在代码里一眼可见（否则它会被夹在滚动条与光标两段之间）。
      if (event.button == 1 && on_checkbox_toggle) {
        if (const int box_line = checkbox_line_at(context, event.position); box_line >= 0) {
          on_checkbox_toggle(static_cast<std::size_t>(box_line));
          event.handled = true;
          return true;   // 吞掉：不让它同时移光标
        }
      }
      // 滚动条优先：两条滚动条都**不能落到“移光标”分支**上——
      // 拖滚动条是“看另一个地方”，把光标一起挪走是错的（实测：拖垂直条后光标从第 1 行跳到第 30 行）。
      const math::Rect v_track = v_scroll_bar_rect(context);
      if (v_track.width > 0.0f && v_track.contains(event.position)) {
        const math::Rect thumb = v_scroll_thumb_rect(context);
        v_dragging_ = true;
        // 点在滑块上：保留相对抓取点（不跳变）；点在轨道上：直接跳到该位置
        v_drag_offset_ = thumb.contains(event.position)
                             ? event.position.y - thumb.y
                             : thumb.height * 0.5f;
        apply_v_scroll_drag(context, event.position.y);
        return true;
      }
      if (h_scroll_bar_rect().contains(event.position) && max_scroll_x(context) > 0.0f) {
        h_dragging_ = true;
        apply_h_scroll_drag(context, event.position.x);
        return true;
      }
      // **`Ctrl+点击`**：交给宿主做“跳到定义”这类跳转，**本组件不动光标/不开拖选**。
      //
      // 为何吞掉这次点击（返回 true）而不是“先移光标再回调”：移光标会写一次选择，
      // 而跳转成功后宿主换文件、编辑器重装文本——那一次选择既无意义，
      // 又会在“跳转失败”（无定义）时白留下一个光标位移。
      if (event.ctrl && on_ctrl_click && (event.button == 0 || event.button == 1)) {
        on_ctrl_click(index_at_point(context, event.position));
        return true;
      }
      cursor_ = index_at_point(context, event.position);
      anchor_ = cursor_;
      if (event.button == 0 || event.button == 1) selecting_ = true;  // 左键开拖
      mark_dirty();
      if (on_cursor_change) on_cursor_change();
      return true;
    }
    case EventKind::DoubleClick: {
      const std::size_t index = index_at_point(context, event.position);
      const auto [begin, end] = word_bounds(index);
      anchor_ = begin;
      cursor_ = end;
      mark_dirty();
      return true;
    }
    case EventKind::TripleClick: {
      const std::size_t line = line_of_index(index_at_point(context, event.position));
      const auto [begin, end] = line_range(line);
      anchor_ = begin;
      cursor_ = std::min(end + 1, text_.size());
      mark_dirty();
      return true;
    }
    case EventKind::MouseMove: {
      // 拖拽选择：用 selecting_ 状态而非 `event.button`——Win32 的 `WM_MOUSEMOVE`
      // 不携带按键状态（后端恒传 0），按 button 判定在真窗口永远不触发（实测踩过）。
      if (selecting_) {
        cursor_ = index_at_point(context, event.position);
        mark_dirty();
        if (on_cursor_change) on_cursor_change();
        return true;
      }
      if (h_dragging_) {
        apply_h_scroll_drag(context, event.position.x);
        return true;
      }
      if (v_dragging_) {
        apply_v_scroll_drag(context, event.position.y);
        return true;
      }
      // 悬停行底纹（阅读长文件时定位当前行；VSCode 同族反馈）。
      //
      // **只报行级 damage 而不是 `mark_dirty()` 整编辑器**（2026-10-29 修
      // “鼠标扫过文本区 CPU 拉满”）：底纹实际只改新旧两行各一条 ~20px 细条
      //（绘制处 `fill_rect(bounds_.x, row_top+band_top, bounds_.width, band_height)`），
      // 而 `mark_dirty` 报的是整个编辑器 bounds（占视口 80%+ → 超 55% 阈值
      // 退化整帧+整窗上屏，WARP 下每帧 ~7ms CPU 拷贝，实测 382% 单核）。
      // 这里按同样的几何只报两条行条；dirty 标志不置——底纹绘制不依赖
      // 其它状态重算，行几何已在本函数开头 `hover_line_at → rebuild_line_geometry`
      // 里重建。
      const int line = hover_line_at(context, event.position);
      if (line != hover_line_) {
        const auto line_band = [&](int index) -> math::Rect {
          if (index < 0) return {};
          const float height = line_height_cache_;
          if (height <= 0.0f) return {};
          const float top = bounds_.y + kTopPadding +
                            static_cast<float>(index) * height - scroll_y_;
          // 与绘制处同源：行高的一部分作为底纹（视觉对齐当前行底色），
          // 不确定具体占比时取整行——宽取编辑器全宽（底纹就是全宽的）。
          return math::Rect{bounds_.x, top, bounds_.width, height};
        };
        record_damage_rect(line_band(hover_line_));  // 旧行：底纹消失
        record_damage_rect(line_band(line));         // 新行：底纹出现
        hover_line_ = line;
      }
      // （这里原来还有"悬停词变化 → `on_hover_word`"回调，供宿主发起 LSP hover。
      //  LSP 符号悬停卡片已整套删除，取词与防抖一并下掉——鼠标移动不再产生
      //  任何"词区间"计算，少一条每像素都要走的分支。）
      return false;
    }
    case EventKind::MouseUp:
      selecting_ = false;
      h_dragging_ = false;
      v_dragging_ = false;
      return false;  // 不吞：Click 的激活语义照常走
    case EventKind::Click:
      selecting_ = false;
      h_dragging_ = false;
      v_dragging_ = false;
      return false;  // 不吞：Click 的激活语义照常走
    case EventKind::Wheel: {
      // Shift+滚轮 = 水平滚动（主流编辑器惯例；也是长行唯一不靠拖拽的入口）。
      // 步长取主题 `Metrics::wheel_step`（全局滚动手感一致，MarkdownView/ScrollView
      // 等同源；无主题时兑底 48）。
      const float wheel_step = context.theme.metrics().wheel_step > 0.0f
                                  ? context.theme.metrics().wheel_step
                                  : 48.0f;
      const float before_x = scroll_x_;
      const float before_y = scroll_y_;
      if (event.shift) {
        scroll_x_ = std::max(0.0f, scroll_x_ - event.wheel_delta * wheel_step);
      } else {
        scroll_y_ = std::max(0.0f, scroll_y_ - event.wheel_delta * wheel_step);
      }
      clamp_scroll(context);
      mark_dirty();
      notify_scroll(before_x, before_y);
      return true;
    }
    case EventKind::TextInput:
      if (event.text.empty()) return false;
      insert_text(event.text);
      return true;
    case EventKind::KeyDown:
      // 未识别的键返回 false 冒泡（全局快捷键/焦点环由此获得落点）；
      // 认识的键（含未产生变更的合法处理，如 read_only 下 Ctrl+C）返回 true。
      return handle_key(context, event);
    default:
      return false;
  }
}

auto CodeEditor::handle_key(const RenderContext& context, const Event& event) -> bool {
  const std::string& key = event.key;
  const bool extend = event.shift;

  // Alt+↑ / Alt+↓：上/下移当前行（或选中行块）。**必须排在 Ctrl 分支之前**：
  // 这组键带 `alt` 而不带 `ctrl`，落到下面的无修饰分支会被当成普通方向键——
  // “文本一字未动、光标移了一行”看起来像“移行没生效”，最难从现象反推原因。
  if (event.alt && !event.ctrl && (key == "ArrowUp" || key == "ArrowDown")) {
    if (read_only_) return false;
    (void)move_lines(key == "ArrowUp" ? -1 : 1);
    return true;
  }

  if (event.ctrl || event.meta) {
    if (key == "a" || key == "A") {
      select_all();
      return true;
    }
    if (key == "c" || key == "C") {
      copy_selection_to_clipboard();
      return true;
    }
    if (key == "x" || key == "X") {
      if (read_only_) return true;
      const std::string picked = selected_text();
      if (picked.empty()) return true;
      copy_selection_to_clipboard();
      push_undo(false);
      delete_selection();
      notify_change();
      return true;
    }
    if (key == "v" || key == "V") {
      if (read_only_) return true;
      insert_text(paste_text_from_clipboard());
      return true;
    }
    if (key == "z" || key == "Z") {
      (event.shift ? redo() : undo());
      return true;
    }
    if (key == "y" || key == "Y") {
      redo();
      return true;
    }
    // Ctrl+D：选中下一处同词（帮助卡列着它，而实现里此前一个字都没有）。
    // 不在这里写 Alt+↑/↓——它们不带 ctrl，已在上面的 Alt 分支拦下。
    if (key == "d" || key == "D") {
      (void)select_next_occurrence();
      return true;
    }
    if (key == "/") {
      toggle_comment();
      return true;
    }
    if (key == "s" || key == "S") {
      if (on_submit) on_submit(text_);
      return true;
    }
    if (key == "Enter") {
      if (on_submit) on_submit(text_);
      return true;
    }
    if (key == "Home") {
      cursor_ = 0;
      anchor_ = extend ? anchor_ : cursor_;
      ensure_cursor_visible(context);
      mark_dirty();
      return true;
    }
    if (key == "End") {
      cursor_ = text_.size();
      anchor_ = extend ? anchor_ : cursor_;
      ensure_cursor_visible(context);
      mark_dirty();
      return true;
    }
    if (key == "Backspace") {
      erase_word(true);
      return true;
    }
    if (key == "Delete") {
      erase_word(false);
      return true;
    }
    if (key == "ArrowLeft") {
      move_word(-1, extend);
      return true;
    }
    if (key == "ArrowRight") {
      move_word(1, extend);
      return true;
    }
    // 未识别的 Ctrl/Alt/Meta 组合（Ctrl+S 保存、Ctrl+W 关标签等全局语义）：放行冒泡
    return false;
  }

  if (key == "Enter") {
    insert_newline();
  } else if (key == "Tab") {
    // 只读：Tab 无编辑语义 → **放行冒泡**，由 UiRoot 交回焦点环。
    // 若仍报“已消费”，焦点会被永久扣在只读视图上（Tab/Shift+Tab 都无响应）——
    // 只读视图恰恰是焦点环里走得进的元素。
    if (read_only_) return false;
    // **Shift+Tab = 反缩进**（编辑器通行手感；缺了它用户会以为“键坏了”）。
    //
    // 无选区时也要能反缩进：把选区指向“本行行首到本行行首”仍会走
    // `indent_selection(reverse)`，它对“起始行 == 结束行”成立（按行取缩进）。
    // 旧实现只看 `has_selection()`，于是 Shift+Tab 在没有选区时落到
    // `insert_text(indent_unit())`——**按 Shift+Tab 反而增加缩进**（实测确定）。
    if (event.shift) {
      if (has_selection()) {
        indent_selection(true);
      } else {
        // 无选区：把整行交给反缩进（选区取行首..行尾，不改变用户选择）。
        const std::size_t line = line_of_index(cursor_);
        const auto [line_begin, line_end] = line_range(line);
        anchor_ = line_begin;
        cursor_ = line_end;
        indent_selection(true);
        // 光标回到原来的列（不把用户扔到行尾）
        cursor_ = std::min(line_begin, text_.size());
        anchor_ = cursor_;
        mark_dirty();
        if (on_cursor_change) on_cursor_change();
      }
    } else if (has_selection()) {
      indent_selection(false);
    } else {
      insert_text(indent_unit());
    }
  } else if (key == "Backspace") {
    erase_backward();
  } else if (key == "Delete") {
    erase_forward();
  } else if (key == "ArrowLeft") {
    move_horizontal(-1, extend);
  } else if (key == "ArrowRight") {
    move_horizontal(1, extend);
  } else if (key == "ArrowUp") {
    move_vertical(context, -1, extend);
  } else if (key == "ArrowDown") {
    move_vertical(context, 1, extend);
  } else if (key == "Home") {
    smart_home(extend);
  } else if (key == "End") {
    move_to_edge(true, extend);
  } else if (key == "PageUp" || key == "PageDown" || key == "Pageup" || key == "Pagedown") {
    // 翻页 = 滚一屏 **且把光标带进视口**（主流编辑器语义）。
    //
    // 原先只改 `scroll_y_`：它不移动光标，而其后 `ensure_cursor_visible()` 又按
    // “光标恒在视口内”把滚动偏移拉回光标那一行——**光标在第 1 行时翻页永远原地不动**
    // （实测：scroll 手工设到 400，一按 PageDown 就被拉回 0）。光标先按可见行数移动，
    // 滚动随之跟上，两者不再打架。
    const bool down = key == "PageDown" || key == "Pagedown";
    const std::size_t rows = std::max<std::size_t>(1, visible_line_count(context) - 1);
    const std::size_t line = line_of_index(cursor_);
    const std::size_t total = line_count();
    const std::size_t target = down ? std::min(line + rows, total == 0 ? 0 : total - 1)
                                    : (line > rows ? line - rows : 0);
    cursor_ = index_at_column(target, column_of(cursor_));
    if (!extend) anchor_ = cursor_;
    ensure_cursor_visible(context);
    // 滚到“新光标恰好处于原先的相对位置”：向下把目标行顶到视口末，向上顶到视口首。
    scroll_y_ = static_cast<float>(target) * line_height_cache_ - kTopPadding;
    clamp_scroll(context);
    mark_dirty();
    if (on_cursor_change) on_cursor_change();
    return true;
  } else if (key == "Escape") {
    clear_selection();
    mark_dirty();
  } else {
    return false;  // 未认识的裸键：放行冒泡（全局快捷键/焦点环）
  }
  ensure_cursor_visible(context);
  mark_dirty();
  if (on_cursor_change) on_cursor_change();
  return true;
}

void CodeEditor::move_horizontal(int direction, bool extend) {
  cursor_ = direction < 0 ? utf8_prev(text_, cursor_) : utf8_next(text_, cursor_);
  cursor_ = std::min(cursor_, text_.size());
  if (!extend) anchor_ = cursor_;
}

void CodeEditor::move_vertical(const RenderContext& context, int direction, bool extend) {
  (void)context;
  const std::size_t line = line_of_index(cursor_);
  const std::size_t column = column_of(cursor_);
  const std::size_t total = line_count();
  const std::size_t target = direction < 0 ? (line == 0 ? 0 : line - 1)
                                           : std::min(line + 1, total == 0 ? 0 : total - 1);
  cursor_ = index_at_column(target, column);
  if (!extend) anchor_ = cursor_;
}

void CodeEditor::move_word(int direction, bool extend) {
  // 词移动语义（与主流编辑器一致）：向左停在**词首**、向右停在**词尾之后**，
  // 中间的空白/标点被跳过——不能把光标停在空白处（否则"回退到词首"会落空一格）。
  if (direction < 0) {
    while (cursor_ > 0 && !is_word_char(text_[utf8_prev(text_, cursor_)])) {
      cursor_ = utf8_prev(text_, cursor_);
    }
    while (cursor_ > 0 && is_word_char(text_[utf8_prev(text_, cursor_)])) {
      cursor_ = utf8_prev(text_, cursor_);
    }
  } else {
    while (cursor_ < text_.size() && !is_word_char(text_[cursor_])) {
      cursor_ = utf8_next(text_, cursor_);
    }
    while (cursor_ < text_.size() && is_word_char(text_[cursor_])) {
      cursor_ = utf8_next(text_, cursor_);
    }
  }
  if (!extend) anchor_ = cursor_;
}

void CodeEditor::move_to_edge(bool to_end, bool extend) {
  const std::size_t line = line_of_index(cursor_);
  cursor_ = to_end ? line_end(line) : line_start(line);
  if (!extend) anchor_ = cursor_;
}

void CodeEditor::move_document_edge(bool to_end, bool extend) {
  cursor_ = to_end ? text_.size() : 0;
  if (!extend) anchor_ = cursor_;
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面 / 语义 / 动作
// ————————————————————————————————————————————————————————————————————————————

auto CodeEditor::semantics_text() const -> std::string { return language_; }

auto CodeEditor::semantics_value() const -> std::string {
  // 语义值给"当前行内容"：整篇代码塞进语义树既无意义也会撑爆控制通道响应
  const std::size_t line = line_of_index(cursor_);
  const auto [begin, end] = line_range(line);
  return text_.substr(begin, end - begin);
}

auto CodeEditor::semantics_flags() const -> SemanticsFlags {
  // 起手式必须是基类结果：`SemanticsFlags flags{}` 会**丢掉** visible/enabled/focused/
  // hovered/pressed —— 焦点经 UiRoot::set_focus 设置时，语义树与 `:focused` 选择器
  // 恒报 false（只直接调 set_focused 的单元测试看不出来）。
  SemanticsFlags flags = Element::semantics_flags();
  flags.editable = !read_only_;
  flags.scrollable = true;
  flags.selected = has_selection();
  return flags;
}

auto CodeEditor::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "text") return text_;
  if (name == "language") return language_;
  // 诊断计数（自动化/状态栏用）：总条数 + 错误条数——两者分开是因为
  // "有没有错"与"有多少提示"是两个问题（错误数决定能不能提交，提示数只影响观感）。
  if (name == "diagnostics") return std::to_string(diagnostics_.size());
  if (name == "diagnostic_errors") {
    std::size_t errors = 0;
    for (const auto& mark : diagnostics_) {
      if (mark.severity == DiagnosticSeverity::Error) ++errors;
    }
    return std::to_string(errors);
  }
  if (name == "hover_diagnostic") {
    const DiagnosticMark* hovered = hover_diagnostic();
    return hovered != nullptr ? hovered->message : std::string{};
  }
  if (name == "cursor") return std::to_string(cursor_);
  if (name == "line") return std::to_string(cursor_line() + 1);
  if (name == "column") return std::to_string(cursor_column() + 1);
  if (name == "lines") return std::to_string(line_count());
  if (name == "selection") {
    const auto [begin, end] = selection();
    return std::to_string(begin) + ":" + std::to_string(end);
  }
  if (name == "selected_text") return selected_text();
  // 行尾注解的**可观测面**（只读）：`行号:文本` 逗号分隔。
  //
  // 为何要暴露：注解是像素级产物，"有没有真进装饰表"在控制通道上否则只能靠截图
  // 指纹（对内容变化不敏感，且需要 Pillow）。这个属性让 e2e 能断言
  // 「哪一行有注解、文字是什么」——blame 只标光标行这条语义尤其需要它。
  if (name == "end_annotations") {
    std::string out;
    for (const auto& mark : line_marks_) {
      if (mark.end_text.empty()) continue;
      if (!out.empty()) out += ",";
      out += std::to_string(mark.line) + ":" + mark.end_text;
    }
    return out;
  }
  if (name == "read_only") return read_only_ ? "true" : "false";
  if (name == "highlight") return highlight_enabled_ ? "true" : "false";
  if (name == "show_line_numbers") return show_line_numbers_ ? "true" : "false";
  if (name == "tab_width") return std::to_string(tab_width_);
  if (name == "indent_guides") return indent_guides_ ? "true" : "false";
  if (name == "auto_pairs") return auto_pairs_ ? "true" : "false";
  if (name == "scroll") {
    return std::format("{:.1f},{:.1f}", static_cast<double>(scroll_x_),
                       static_cast<double>(scroll_y_));
  }
  if (name == "first_visible_line" || name == "visible_lines") {
    // **不做视口推理**：这两个量需要文本度量（行高/字体），而属性面没有
    // `RenderContext`。曾经想过“把 arrange 收到的上下文指针缓存起来”，
    // 那是错的——`UiRoot::render_context()` **按值返回**，缓存它的指针会悬垂
    // （实测：控制通道 `get` 当场 SIGSEGV，崩在字体度量里）。
    //
    // 现口径：从 `bounds_` 与行高缓存（绘制过一次就有值）推导，拿不到就如实退化为
    // “第一行 / 总行数”。调用方若需要精确视口，用带上下文的重载
    // `first_visible_line(context)` / `visible_line_count(context)`。
    const float height = line_height_cache_;
    if (height <= 0.0f || bounds_.height <= 0.0f) {
      return name == "first_visible_line" ? std::string("1") : std::to_string(line_count());
    }
    const auto visible =
        std::max<std::size_t>(1, static_cast<std::size_t>(bounds_.height / height));
    if (name == "visible_lines") return std::to_string(visible);
    const float top = std::max(0.0f, scroll_y_ - kTopPadding);
    const auto first =
        std::min(static_cast<std::size_t>(std::ceil(static_cast<double>(top / height))),
                 line_count() - 1);
    return std::to_string(first + 1);
  }
  if (name == "font_size") return std::format("{}", font_size());
  // 字体档位（倍数）：读回的是**输入値**而非解析后的像素——两个属性各自有存在的理由
  //（`font_size` 回答“多大”，`font_scale` 回答“相对正文多少倍”）。
  if (name == "font_scale") return std::format("{}", font_scale_);
  // 行盒几何（自动化核“文字/光标在行里坐得对不对”用）：直接从**框架的** LineGeometry 报出，
  // 与绘制同一份数据——量尺不必自己再算一遍。
  if (name == "line_height") return std::format("{:.2f}", line_height_cache_);
  if (name == "text_offset") return std::format("{:.2f}", line_geometry_cache_.origin_y);
  if (name == "caret_top") return std::format("{:.2f}", line_geometry_cache_.ink_top);
  if (name == "ink_height") return std::format("{:.2f}", line_geometry_cache_.ink_height);
  if (name == "baseline") return std::format("{:.2f}", line_geometry_cache_.baseline);
  if (name == "find_needle") return find_needle_;
  if (name == "find_word") return find_word_ ? "true" : "false";
  if (name == "find_case") return find_case_ ? "true" : "false";
  if (name == "find_matches") return std::to_string(find_matches_.size());
  if (name == "find_active") {
    return find_active_ == kNoFindMatch ? std::string("-1") : std::to_string(find_active_);
  }
  return std::nullopt;
}

auto CodeEditor::set_property(std::string_view name, std::string_view value) -> bool {
  const auto parse_size = [](std::string_view text) -> std::optional<std::size_t> {
    try {
      return static_cast<std::size_t>(std::stoull(std::string(text)));
    } catch (...) {
      return std::nullopt;
    }
  };
  const auto truthy = [](std::string_view text) {
    return text == "true" || text == "1" || text == "yes";
  };
  if (name == "font_scale") {
    try {
      set_font_scale(std::stof(std::string(value)));
      return true;
    } catch (...) {
      return false;
    }
  }
  if (name == "font_size") {
    // 负值/零 = 恢复“跟随主题”（与 `set_font_size` 的语义一致）。
    try {
      const float size = std::stof(std::string(value));
      font_size_px_ = size > 0.0f ? size : -1.0f;
      mark_layout_dirty();
      return true;
    } catch (...) {
      return false;
    }
  }
  if (name == "text") {
    // 属性面写入（多来自控制通道/自动化）**通知** `on_change`，与 Input 同一决议：
    // 宿主把 `on_change` 当“文本变了”的唯一钩子，静默改会出现
    // “`get` 读回来是新文本、宿主状态里还是旧的”。C++ API `set_text` 仍静默
    // （程序化装载不冒充用户编辑，见其实现处的注释）。
    const std::string before = text_;
    set_text(std::string(value));
    if (text_ != before && on_change) on_change(text_);
    return true;
  }
  if (name == "language") {
    set_language(std::string(value));
    return true;
  }
  if (name == "cursor") {
    if (const auto index = parse_size(value); index.has_value()) {
      set_cursor_index(*index);
      return true;
    }
    return false;
  }
  if (name == "selection") {
    const std::size_t separator = value.find(':');
    if (separator == std::string_view::npos) return false;
    const auto begin = parse_size(value.substr(0, separator));
    const auto end = parse_size(value.substr(separator + 1));
    if (!begin.has_value() || !end.has_value()) return false;
    set_selection(*begin, *end);
    return true;
  }
  if (name == "read_only") {
    set_read_only(truthy(value));
    return true;
  }
  // 两个查找选项改完都要**重建命中表**：不重建就会出现“勾了全词、命中数还是旧的”，
  // 而高亮画的是旧命中——选项与界面静默分岔。
  if (name == "find_word") {
    find_word_ = truthy(value);
    find_active_set_ = false;
    find_active_ = kNoFindMatch;
    rebuild_find_matches();
    mark_dirty();
    return true;
  }
  if (name == "find_case") {
    find_case_ = truthy(value);
    find_active_set_ = false;
    find_active_ = kNoFindMatch;
    rebuild_find_matches();
    mark_dirty();
    return true;
  }
  if (name == "highlight") {
    set_highlight_enabled(truthy(value));
    return true;
  }
  if (name == "show_line_numbers") {
    set_show_line_numbers(truthy(value));
    return true;
  }
  if (name == "tab_width") {
    if (const auto width = parse_size(value); width.has_value()) {
      set_tab_width(static_cast<int>(*width));
      return true;
    }
    return false;
  }
  if (name == "indent_guides") {
    set_indent_guides(truthy(value));
    return true;
  }
  if (name == "auto_pairs") {
    set_auto_pairs(truthy(value));
    return true;
  }
  if (name == "scroll") {
    const std::size_t comma = value.find(',');
    const float before_x = scroll_x_;
    const float before_y = scroll_y_;
    try {
      if (comma == std::string_view::npos) {
        set_scroll_offset(0.0f, static_cast<float>(std::stod(std::string(value))));
      } else {
        set_scroll_offset(static_cast<float>(std::stod(std::string(value.substr(0, comma)))),
                          static_cast<float>(std::stod(std::string(value.substr(comma + 1)))));
      }
      // 属性面写入也是一次滚动（并列视图的同步会互写属性）——
      // 不通知会让「拖左栅滚、右栅不动」。
      notify_scroll(before_x, before_y);
      return true;
    } catch (...) {
      return false;
    }
  }
  if (name == "goto_line" || name == "line") {
    if (const auto line = parse_size(value); line.has_value()) {
      goto_line(*line);
      return true;
    }
    return false;
  }
  return false;
}

auto CodeEditor::property_names() const -> std::vector<std::string_view> {
  return {"text",   "language",  "cursor",   "line",       "column",     "lines",
          "selection", "selected_text", "read_only", "highlight", "show_line_numbers",
          "tab_width", "indent_guides", "auto_pairs", "font_size", "font_scale", "scroll",
          "first_visible_line", "visible_lines", "goto_line", "find_needle", "find_matches",
          "find_active", "find_case", "find_word", "line_height", "text_offset", "caret_top",
          "ink_height", "baseline", "diagnostics", "diagnostic_errors", "hover_diagnostic",
          "end_annotations"};
}

// —— 查找与替换 ——

auto CodeEditor::set_find(std::string needle, FindOptions options) -> std::size_t {
  find_needle_ = std::move(needle);
  find_case_ = options.case_sensitive;
  find_word_ = options.whole_word;
  find_enabled_ = !find_needle_.empty();
  find_active_set_ = false;
  find_active_ = kNoFindMatch;
  rebuild_find_matches();
  mark_dirty();
  return find_matches_.size();
}

void CodeEditor::clear_find() {
  find_needle_.clear();
  find_matches_.clear();
  find_enabled_ = false;
  find_active_set_ = false;
  find_active_ = kNoFindMatch;
  mark_dirty();
}

void CodeEditor::rebuild_find_matches() {
  find_matches_.clear();
  find_line_ranges_.clear();
  if (find_needle_.empty()) return;
  // 大小写不敏感：ASCII 折叠后逐位置比较（图标/编辑器场景的务实口径；
  // Unicode case-fold 超出子集范围，CJK 不受影响）
  const auto fold = [](std::string& text) {
    for (char& ch : text) {
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
  };
  std::string haystack = text_;
  std::string needle = find_needle_;
  if (!find_case_) {
    fold(haystack);
    fold(needle);
  }
  std::size_t at = 0;
  while (at + needle.size() <= haystack.size()) {
    const std::size_t hit = haystack.find(needle, at);
    if (hit == std::string::npos) break;
    at = hit + needle.size();  // 不重叠
    // 全词：两侧都不得是词字符（与 `word_bounds` 同一套 `is_word_char` 判据——
    // 两处各写一份的话，“双击选中什么”与“全词匹配什么”会静默错位）。
    if (find_word_) {
      const bool left_ok = hit == 0 || !is_word_char(haystack[hit - 1]);
      const bool right_ok = at >= haystack.size() || !is_word_char(haystack[at]);
      if (!left_ok || !right_ok) continue;
    }
    find_matches_.emplace_back(hit, at);
  }
  // —— 按行分桶（2026-10-18，实测驱动）——
  //
  // 为何需要（实测踩到）：绘制时曾是“每一可见行 × 全部命中”两层循环——
  // 命中数 100 万时编辑帧 paint 从 17.4ms 涨到 **74.2ms**（4.2×），
  // 而绝大多数命中都落在屏幕之外，纯属白扫。
  // 现在建一张“行 → [首, 末) 命中下标”的表，绘制时只遍历本行那几条。
  // 建表是 O(命中数) 的一趟（本来就要扫一遍找命中），不新增量级。
  if (find_matches_.empty()) return;
  rebuild_spans();
  find_line_ranges_.assign(line_spans_.size(), {0, 0});
  std::size_t line = 0;
  for (std::size_t index = 0; index < find_matches_.size(); ++index) {
    const std::size_t hit = find_matches_[index].first;
    // 前进到包含 `hit` 的行（命中天然升序，所以行号单调不减）。
    while (line + 1 < line_spans_.size() && hit >= line_spans_[line].second + 1) ++line;
    auto& range = find_line_ranges_[line];
    if (range.second == 0) range.first = index;   // 本行第一条
    range.second = index + 1;                     // 半开区间的末
  }
}

/// 本行范围内的查找命中下标区间 `[first, last)`（无命中则两者相等）。
[[nodiscard]] auto CodeEditor::find_range_of_line(std::size_t line) const
    -> std::pair<std::size_t, std::size_t> {
  if (line >= find_line_ranges_.size()) return {0, 0};
  const auto& range = find_line_ranges_[line];
  if (range.second == 0) return {range.first, range.first};
  return range;
}

void CodeEditor::select_find_match(std::size_t index) {
  if (index >= find_matches_.size()) return;
  const auto [begin, end] = find_matches_[index];
  cursor_ = end;
  anchor_ = begin;
  find_active_ = index;
  find_active_set_ = true;
  clamp_cursor();
  mark_dirty();
}

auto CodeEditor::find_next(bool backward) -> std::size_t {
  if (find_matches_.empty() || !find_enabled_) return static_cast<std::size_t>(-1);
  if (!find_active_set_) {
    // 从光标位置就近起步（VSCode 同族：从光标向下找第一个）
    std::size_t best = 0;
    for (std::size_t i = 0; i < find_matches_.size(); ++i) {
      if (find_matches_[i].first >= cursor_) {
        best = i;
        break;
      }
      best = i;  // 一直到最后都没 >= cursor → 环绕到最后命中（backward 语义起点）
    }
    if (!backward) {
      // 光标之后第一个；若全在光标前 → 环绕到 0
      best = 0;
      bool found = false;
      for (std::size_t i = 0; i < find_matches_.size(); ++i) {
        if (find_matches_[i].first >= cursor_) {
          best = i;
          found = true;
          break;
        }
      }
      if (!found) best = 0;
    }
    select_find_match(best);
    return best;
  }
  std::size_t next = find_active_;
  if (backward) {
    next = next == 0 ? find_matches_.size() - 1 : next - 1;
  } else {
    next = next + 1 >= find_matches_.size() ? 0 : next + 1;
  }
  select_find_match(next);
  return next;
}

auto CodeEditor::replace_current(std::string_view replacement) -> bool {
  if (read_only_ || !find_active_set_ || find_active_ >= find_matches_.size()) {
    return false;
  }
  const auto [begin, end] = find_matches_[find_active_];
  push_undo(false);
  text_.replace(text_.begin() + static_cast<std::ptrdiff_t>(begin),
                text_.begin() + static_cast<std::ptrdiff_t>(end), replacement);
  cursor_ = begin + replacement.size();
  anchor_ = cursor_;
  // 重算命中，然后跳到「被替换处的下一个命中」（VSCode 语义：替换 → 前进）。
  // 注意不能先 notify_change（它也会刷新命中表）——统一走这里的语义。
  rebuild_find_matches();
  find_active_set_ = !find_matches_.empty();
  if (find_active_set_) {
    std::size_t next = kNoFindMatch;
    for (std::size_t i = 0; i < find_matches_.size(); ++i) {
      if (find_matches_[i].first >= cursor_) {
        next = i;
        break;
      }
    }
    if (next == kNoFindMatch) next = 0;  // 环绕
    select_find_match(next);
  } else {
    find_active_ = kNoFindMatch;
  }
  clamp_cursor();
  mark_layout_dirty();
  mark_highlight_dirty();
  mark_dirty();
  notify_change();
  return true;
}

auto CodeEditor::replace_all(std::string_view replacement) -> std::size_t {
  if (read_only_ || find_matches_.empty()) return 0;
  push_undo(false);
  std::string out;
  out.reserve(text_.size());
  std::size_t at = 0;
  for (const auto& [begin, end] : find_matches_) {
    out.append(text_, at, begin - at);
    out.append(replacement);
    at = end;
  }
  out.append(text_, at, text_.size() - at);
  const std::size_t count = find_matches_.size();
  text_ = std::move(out);
  cursor_ = std::min(cursor_, text_.size());
  anchor_ = cursor_;
  refresh_find_after_edit();
  notify_change();
  return count;
}

void CodeEditor::refresh_find_after_edit() {
  if (!find_enabled_) return;
  rebuild_find_matches();
  find_active_ = find_matches_.empty() ? kNoFindMatch : 0;
  find_active_set_ = !find_matches_.empty();
  clamp_cursor();
  mark_layout_dirty();
  mark_highlight_dirty();
  mark_dirty();
}

auto CodeEditor::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "focus" || action == "activate") return true;  // 焦点由 UiRoot 统一设置
  if (action == "select_all") {
    select_all();
    return true;
  }
  if (action == "undo") return undo();
  if (action == "redo") return redo();
  if (action == "clear_selection") {
    clear_selection();
    mark_dirty();
    return true;
  }
  if (action == "insert") {
    insert_text(argument);
    return true;
  }
  if (action == "copy") {
    copy_selection_to_clipboard();
    return true;
  }
  if (action == "cut") {
    const std::string picked = selected_text();
    if (picked.empty() || read_only_) return false;
    copy_selection_to_clipboard();
    push_undo(false);
    delete_selection();
    notify_change();
    return true;
  }
  if (action == "paste") {
    if (read_only_) return true;
    insert_text(paste_text_from_clipboard());
    return true;
  }
  if (action == "comment") return toggle_comment();
  // 控制通道 / 宿主可驱动的三项编辑动作（与键盘走**同一入口**，
  // 避免“快捷键能用、动作面不能用”这种两条腿不一样长的局面）。
  if (action == "select_next_occurrence") return select_next_occurrence();
  if (action == "move_line_up") return move_lines(-1);
  if (action == "move_line_down") return move_lines(1);
  if (action == "move_lines") {
    try {
      return move_lines(std::stoi(std::string(argument)));
    } catch (...) {
      return false;
    }
  }
  if (action == "indent") {
    indent_selection(false);
    return true;
  }
  if (action == "dedent") {
    indent_selection(true);
    return true;
  }
  if (action == "goto_line") {
    try {
      const auto line = static_cast<std::size_t>(std::stoull(std::string(argument)));
      goto_line(line);
      return true;
    } catch (...) {
      return false;
    }
  }
  if (action == "scroll_to_line") {
    try {
      scroll_to_line(static_cast<std::size_t>(std::stoull(std::string(argument))));
      return true;
    } catch (...) {
      return false;
    }
  }
  if (action == "scroll_by") {
    // 参数是 `dy`（正 = 向下），与 `scroll_by(dx, dy)` 的纵向分量同向。
    try {
      scroll_by(0.0f, static_cast<float>(std::stod(std::string(argument))));
      return true;
    } catch (...) {
      return false;
    }
  }
  if (action == "reveal_line") {
    // 跳转并**保证可见**（goto_line 不滚屏）——示例“搜索命中 → 跳到该行”的落点。
    try {
      const auto line = static_cast<std::size_t>(std::stoull(std::string(argument)));
      goto_line(line);
      scroll_to_line(line);
      return true;
    } catch (...) {
      return false;
    }
  }
  if (action == "set_text") {
    set_text(std::string(argument));
    return true;
  }
  if (action == "find") {
    // argument: "needle" / "needle|case" / "needle|case|word"（case/word：0/1）
    const std::size_t bar = argument.find('|');
    if (bar == std::string_view::npos) {
      set_find(std::string(argument), FindOptions{});
      return true;
    }
    const std::string_view needle = argument.substr(0, bar);
    const std::string_view rest = argument.substr(bar + 1);
    const std::size_t bar2 = rest.find('|');
    const FindOptions options{.case_sensitive = rest.substr(0, bar2) == "1",
                              .whole_word = bar2 != std::string_view::npos &&
                                            rest.substr(bar2 + 1) == "1"};
    set_find(std::string(needle), options);
    return true;
  }
  if (action == "clear_find") {
    clear_find();
    return true;
  }
  if (action == "find_next") return find_next(false) != static_cast<std::size_t>(-1);
  if (action == "find_prev") return find_next(true) != static_cast<std::size_t>(-1);
  if (action == "replace") return replace_current(argument);
  if (action == "replace_all") return replace_all(argument) > 0;
  return false;
}

}  // namespace st::ui
