#include "st/ui/components/code_editor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {
namespace {

/// UTF-8 前一个码点起点。
[[nodiscard]] auto utf8_prev(std::string_view utf8, std::size_t index) noexcept -> std::size_t {
  if (index == 0) return 0;
  std::size_t probe = index - 1;
  while (probe > 0 && (static_cast<unsigned char>(utf8[probe]) & 0xC0U) == 0x80U) --probe;
  return probe;
}

/// UTF-8 后一个码点起点。
[[nodiscard]] auto utf8_next(std::string_view utf8, std::size_t index) noexcept -> std::size_t {
  if (index >= utf8.size()) return utf8.size();
  std::size_t probe = index + 1;
  while (probe < utf8.size() && (static_cast<unsigned char>(utf8[probe]) & 0xC0U) == 0x80U) ++probe;
  return probe;
}

[[nodiscard]] auto is_word_char(char raw) noexcept -> bool {
  return (raw >= 'a' && raw <= 'z') || (raw >= 'A' && raw <= 'Z') || (raw >= '0' && raw <= '9') ||
         raw == '_' || static_cast<unsigned char>(raw) >= 0x80U;
}

/// 编辑器内部剪贴板。
///
/// 为什么不是系统剪贴板：无头模式没有系统剪贴板（服务器无 X/Wayland），而编辑器必须具备可用的
/// 复制/粘贴语义。进程内剪贴板在无头与有窗口下行为**完全一致**，也能被控制通道
/// 的 `invoke(copy/cut/paste)` 驱动（智能体可据此搬运文本）。
std::string& editor_clipboard() {
  static std::string clipboard;
  return clipboard;
}

inline constexpr float kGutterPadding{12.0f};
inline constexpr float kTopPadding{6.0f};
inline constexpr float kBottomPadding{6.0f};
inline constexpr float kCursorWidth{1.6f};
inline constexpr float kScrollBarWidth{9.0f};
inline constexpr std::int64_t kCoalesceWindowMs{600};
inline constexpr std::size_t kMaxUndoDepth{256};

[[nodiscard]] auto text_port_of(const RenderContext& context) -> const TextPort& {
  return context.text != nullptr ? *context.text : NullTextPort::instance();
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
  // 文本编辑类默认可聚焦（与 Input/TextArea 同口径）：“先有焦点才能键盘激活、
  // 键盘激活又要求先有焦点”是死循环——鼠标点击的聚焦路径只认 focusable()，
  // 默认 false 会让点击永远聚焦不了编辑器。
  set_focusable(true);
  style_.direction = FlexDirection::Column;
  style_.clip_children = true;
  style_.background = math::Color{0, 0, 0, 0};
}

CodeEditor::~CodeEditor() = default;

void CodeEditor::set_font_size(float size) {
  font_size_ = size;
  mark_layout_dirty();
}

void CodeEditor::set_tab_width(int width) {
  tab_width_ = std::clamp(width, 1, 16);
  mark_layout_dirty();
}

auto CodeEditor::available_languages() -> std::vector<std::string> {
  return text::global_languages().names();
}

void CodeEditor::apply_theme(const Theme& theme) {
  (void)theme;  // 语法配色在绘制时按 `theme.syntax()` 取（无需缓存到样式）
  style_.font_size = font_size_;
  style_.background = math::Color{0, 0, 0, 0};
  mark_dirty();
}

void CodeEditor::mark_highlight_dirty() {
  spans_dirty_ = true;
  tokens_dirty_ = true;
  geometry_dirty_ = true;
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
  mark_highlight_dirty();
  if (on_change) on_change(text_);
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
  mark_highlight_dirty();
  clamp_cursor();
  if (on_change) on_change(text_);
  if (on_cursor_change) on_cursor_change();
}

void CodeEditor::delete_selection() {
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
  push_undo(inserted.size() == 1);
  delete_selection();
  text_.insert(cursor_, inserted);
  cursor_ += inserted.size();
  anchor_ = cursor_;
  notify_change();
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
  const std::size_t begin = utf8_prev(text_, cursor_);
  push_undo(true);
  text_.erase(begin, cursor_ - begin);
  cursor_ = begin;
  anchor_ = begin;
  notify_change();
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

// ————————————————————————————————————————————————————————————————————————————
// 几何
// ————————————————————————————————————————————————————————————————————————————

auto CodeEditor::line_height(const RenderContext& context) const -> float {
  return text_port_of(context).line_height(font_size_);
}

auto CodeEditor::gutter_width(const RenderContext& context) const -> float {
  if (!show_line_numbers_) return 0.0f;
  const TextPort& port = text_port_of(context);
  const std::size_t digits = std::to_string(std::max<std::size_t>(line_count(), 1)).size();
  return port.measure_width(std::string(digits, '0'), font_size_, text::FontRole::Monospace) + kGutterPadding * 2.0f;
}

auto CodeEditor::rebuild_line_geometry(const RenderContext& context) const -> void {
  if (!geometry_dirty_) return;
  rebuild_tokens();
  line_height_cache_ = text_port_of(context).line_height(font_size_);
  gutter_cache_ = gutter_width(context);
  max_line_width_cache_ = 0.0f;
  const TextPort& port = text_port_of(context);
  for (const auto& [begin, end] : line_spans_) {
    const float width = port.measure_width(std::string_view(text_).substr(begin, end - begin), font_size_, text::FontRole::Monospace);
    max_line_width_cache_ = std::max(max_line_width_cache_, width);
  }
  geometry_dirty_ = false;
}

auto CodeEditor::text_origin(const RenderContext& context) const -> math::Point {
  rebuild_line_geometry(context);
  return math::Point{bounds_.x + gutter_cache_ + kGutterPadding, bounds_.y + kTopPadding - scroll_y_};
}

auto CodeEditor::content_height() const noexcept -> float {
  rebuild_spans();
  return static_cast<float>(line_spans_.size()) * line_height_cache_ + kTopPadding + kBottomPadding;
}

auto CodeEditor::content_width(const RenderContext& context) const -> float {
  rebuild_line_geometry(context);
  return gutter_cache_ + max_line_width_cache_ + kGutterPadding * 2.0f;
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

auto CodeEditor::x_for_index(const RenderContext& context, std::size_t index) const -> float {
  rebuild_line_geometry(context);
  const std::size_t line = line_of_index(index);
  const std::size_t start = line_start(line);
  const std::size_t stop = std::min(index, text_.size());
  const std::string_view prefix = std::string_view(text_).substr(start, stop - start);
  return text_origin(context).x + text_port_of(context).measure_width(prefix, font_size_);
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
  float width = 0.0f;
  std::size_t index = begin;
  while (index < end) {
    const std::size_t next = utf8_next(text_, index);
    const float advance = port.measure_width(std::string_view(text_).substr(index, next - index), font_size_, text::FontRole::Monospace);
    if (width + advance * 0.5f > target) return index;
    width += advance;
    index = next;
  }
  return end;
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
  };
  // 光标左右两侧的括号都算（编辑器惯例）
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
  const float cursor_x = x_for_index(context, cursor_) - (bounds_.x + gutter_cache_ + kGutterPadding);
  const float view_width = std::max(0.0f, bounds_.width - gutter_cache_ - kGutterPadding * 2.0f);
  if (cursor_x - scroll_x_ < 0.0f) {
    scroll_x_ = std::max(0.0f, cursor_x - 8.0f);
  } else if (cursor_x - scroll_x_ > view_width) {
    scroll_x_ = cursor_x - view_width + 8.0f;
  }
}

void CodeEditor::set_scroll_offset(float x, float y) {
  scroll_x_ = std::max(0.0f, x);
  scroll_y_ = std::max(0.0f, y);
  mark_dirty();
}

void CodeEditor::scroll_to_line(std::size_t line) {
  const std::size_t clamped = std::min(line == 0 ? 0 : line - 1, line_count() - 1);
  scroll_y_ = static_cast<float>(clamped) * line_height_cache_;
  mark_dirty();
}

// ————————————————————————————————————————————————————————————————————————————
// 布局与绘制
// ————————————————————————————————————————————————————————————————————————————

void CodeEditor::measure(const RenderContext& context, const Constraints& constraints) {
  const float height = text_port_of(context).line_height(font_size_);
  const float rows = std::min(static_cast<float>(line_count()), 24.0f);
  const float natural_height = rows * height + kTopPadding + kBottomPadding;
  measured_ = math::Size{constraints.max_width, std::min(constraints.max_height, std::max(80.0f, natural_height))};
  (void)context;
}

void CodeEditor::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  geometry_dirty_ = true;
  (void)context;
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
  const float origin_x = bounds_.x + gutter_cache_ + kGutterPadding - scroll_x_;
  const float origin_y = bounds_.y + kTopPadding - scroll_y_;

  // 行号槽底
  if (show_line_numbers_ && gutter_cache_ > 0.0f) {
    canvas.fill_rect(math::Rect{bounds_.x, bounds_.y, gutter_cache_, bounds_.height},
                     raster::Paint::solid(colors.surface_alt), 0.0f);
  }

  const auto [first_line, last_line] = visible_line_range(context);
  const std::size_t current = line_of_index(cursor_);
  const auto [sel_begin, sel_end] = selection();
  const auto brackets = focused_ ? matching_bracket() : std::nullopt;

  for (std::size_t line = first_line; line < last_line && line < line_spans_.size(); ++line) {
    const float row_top = origin_y + static_cast<float>(line) * height;
    const auto [begin, end] = line_spans_[line];
    const std::string_view row = std::string_view(text_).substr(begin, end - begin);

    // 当前行底色
    if (line == current) {
      canvas.fill_rect(math::Rect{bounds_.x, row_top, bounds_.width, height},
                       raster::Paint::solid(syntax.current_line), 0.0f);
    }

    // 查找命中高亮（先于选择：选中态压在命中态上）
    if (find_enabled_ && !find_matches_.empty()) {
      for (std::size_t m = 0; m < find_matches_.size(); ++m) {
        const auto& [hit_begin, hit_end] = find_matches_[m];
        // 与该行有交集才画
        if (hit_end <= begin || hit_begin >= end) continue;
        const std::size_t from = std::max(hit_begin, begin);
        const std::size_t to = std::min(hit_end, end);
        const float x0 = origin_x + port.measure_width(
                                         std::string_view(text_).substr(begin, from - begin),
                                         font_size_, text::FontRole::Monospace);
        const float x1 = origin_x + port.measure_width(
                                         std::string_view(text_).substr(begin, to - begin),
                                         font_size_, text::FontRole::Monospace);
        canvas.fill_rect(math::Rect{x0, row_top, std::max(x1 - x0, 2.0f), height},
                         raster::Paint::solid(m == find_active_ ? syntax.find_active
                                                                : syntax.find_highlight),
                         2.0f);
      }
    }

    // 选择底色（该行与选择区间的交集）
    if (sel_begin != sel_end) {
      const std::size_t slice_begin = std::max(sel_begin, begin);
      const std::size_t slice_end = std::min(sel_end, end + (sel_end > end ? 1U : 0U));
      if (slice_end > slice_begin || (sel_begin <= begin && sel_end >= end + 1)) {
        const std::size_t from = std::max(slice_begin, begin);
        const std::size_t to = std::min(std::max(slice_end, from), end);
        const float x0 = origin_x + port.measure_width(
                                       std::string_view(text_).substr(begin, from - begin), font_size_, text::FontRole::Monospace);
        const float x1 = origin_x + port.measure_width(
                                       std::string_view(text_).substr(begin, to - begin), font_size_, text::FontRole::Monospace);
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
        const float x0 = origin_x + port.measure_width(
                                       std::string_view(text_).substr(begin, position - begin), font_size_, text::FontRole::Monospace);
        const std::size_t next = utf8_next(text_, position);
        const float x1 = origin_x + port.measure_width(
                                       std::string_view(text_).substr(begin, next - begin), font_size_, text::FontRole::Monospace);
        canvas.fill_rect(math::Rect{x0 - 1.0f, row_top, x1 - x0 + 2.0f, height},
                         raster::Paint::solid(syntax.matching_bracket), 2.0f);
      }
    }

    // token 着色绘制（无 token 时整行按 plain 画）
    float pen = origin_x;
    const auto& tokens = line < line_tokens_.size() ? line_tokens_[line] : LineTokens{};
    if (tokens.empty()) {
      port.draw(canvas, row, math::Point{pen, row_top}, font_size_, syntax.plain, text::FontRole::Monospace);
    } else {
      std::size_t consumed = 0;
      for (const auto& token : tokens) {
        if (token.begin > consumed) {
          const std::string_view gap = row.substr(consumed, token.begin - consumed);
          port.draw(canvas, gap, math::Point{pen, row_top}, font_size_, syntax.plain, text::FontRole::Monospace);
          pen += port.measure_width(gap, font_size_, text::FontRole::Monospace);
        }
        const std::size_t length = std::min(token.end, row.size()) - std::min(token.begin, row.size());
        if (length == 0 || token.begin >= row.size()) continue;
        const std::string_view slice = row.substr(token.begin, length);
        port.draw(canvas, slice, math::Point{pen, row_top}, font_size_, token_color(syntax, token.kind),
                   text::FontRole::Monospace);
        pen += port.measure_width(slice, font_size_, text::FontRole::Monospace);
        consumed = token.begin + length;
      }
      if (consumed < row.size()) {
        port.draw(canvas, row.substr(consumed), math::Point{pen, row_top}, font_size_, syntax.plain, text::FontRole::Monospace);
      }
    }

    // 行号
    if (show_line_numbers_ && gutter_cache_ > 0.0f) {
      const std::string number = std::to_string(line + 1);
      const float number_width = port.measure_width(number, font_size_, text::FontRole::Monospace);
      port.draw(canvas, number,
                math::Point{bounds_.x + gutter_cache_ - kGutterPadding - number_width, row_top},
                font_size_, line == current ? syntax.plain : syntax.line_number,
                text::FontRole::Monospace);
    }
  }

  // 光标（聚焦且可见时）
  if (focused_ && !read_only_) {
    const std::size_t line = line_of_index(cursor_);
    if (line >= first_line && line < last_line) {
      const float row_top = origin_y + static_cast<float>(line) * height;
      const float x = x_for_index(context, cursor_);
      canvas.fill_rect(math::Rect{x, row_top + 1.0f, kCursorWidth, height - 2.0f},
                       raster::Paint::solid(syntax.cursor), 0.0f);
    }
  }

  // 垂直滚动条（内容超出时）
  const float total_height = content_height();
  if (total_height > bounds_.height && bounds_.height > 0.0f) {
    const float ratio = bounds_.height / total_height;
    const float thumb = std::max(24.0f, bounds_.height * ratio);
    const float travel = bounds_.height - thumb;
    const float max_scroll = std::max(1.0f, total_height - bounds_.height);
    const float offset = std::clamp(scroll_y_ / max_scroll, 0.0f, 1.0f) * travel;
    canvas.fill_rect(math::Rect{bounds_.right() - kScrollBarWidth - 2.0f, bounds_.y + offset,
                                kScrollBarWidth, thumb},
                     raster::Paint::solid(colors.border_strong), kScrollBarWidth * 0.5f);
  }
  canvas.pop_clip();
}

// ————————————————————————————————————————————————————————————————————————————
// 事件
// ————————————————————————————————————————————————————————————————————————————

void CodeEditor::activate() { set_focusable(true); }

auto CodeEditor::on_event(const RenderContext& context, Event& event) -> bool {
  if (!enabled()) return false;
  switch (event.kind) {
    case EventKind::MouseDown: {
      rebuild_line_geometry(context);
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
      return false;
    }
    case EventKind::MouseUp:
    case EventKind::Click:
      selecting_ = false;
      return false;  // 不吞：Click 的激活语义照常走
    case EventKind::Wheel: {
      scroll_y_ = std::max(0.0f, scroll_y_ - event.wheel_delta * 48.0f);
      const float max_scroll = std::max(0.0f, content_height() - bounds_.height);
      scroll_y_ = std::min(scroll_y_, max_scroll);
      mark_dirty();
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

  if (event.ctrl || event.meta) {
    if (key == "a" || key == "A") {
      select_all();
      return true;
    }
    if (key == "c" || key == "C") {
      const std::string picked = selected_text();
      if (!picked.empty()) editor_clipboard() = picked;
      return true;
    }
    if (key == "x" || key == "X") {
      if (read_only_) return true;
      const std::string picked = selected_text();
      if (picked.empty()) return true;
      editor_clipboard() = picked;
      push_undo(false);
      delete_selection();
      notify_change();
      return true;
    }
    if (key == "v" || key == "V") {
      if (read_only_) return true;
      insert_text(editor_clipboard());
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
    if (has_selection()) {
      indent_selection(extend);
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
    move_to_edge(false, extend);
  } else if (key == "End") {
    move_to_edge(true, extend);
  } else if (key == "PageUp") {
    scroll_y_ = std::max(0.0f, scroll_y_ - std::max(1.0f, bounds_.height - 40.0f));
    mark_dirty();
  } else if (key == "PageDown") {
    scroll_y_ = std::min(std::max(0.0f, content_height() - bounds_.height),
                         scroll_y_ + std::max(1.0f, bounds_.height - 40.0f));
    mark_dirty();
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
  if (name == "cursor") return std::to_string(cursor_);
  if (name == "line") return std::to_string(cursor_line() + 1);
  if (name == "column") return std::to_string(cursor_column() + 1);
  if (name == "lines") return std::to_string(line_count());
  if (name == "selection") {
    const auto [begin, end] = selection();
    return std::to_string(begin) + ":" + std::to_string(end);
  }
  if (name == "selected_text") return selected_text();
  if (name == "read_only") return read_only_ ? "true" : "false";
  if (name == "highlight") return highlight_enabled_ ? "true" : "false";
  if (name == "show_line_numbers") return show_line_numbers_ ? "true" : "false";
  if (name == "tab_width") return std::to_string(tab_width_);
  if (name == "font_size") return std::format("{}", font_size_);
  if (name == "find_needle") return find_needle_;
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
  if (name == "text") {
    set_text(std::string(value));
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
          "tab_width", "font_size", "goto_line", "find_needle", "find_matches", "find_active"};
}

// —— 查找与替换 ——

auto CodeEditor::set_find(std::string needle, bool case_sensitive) -> std::size_t {
  find_needle_ = std::move(needle);
  find_case_ = case_sensitive;
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
    find_matches_.emplace_back(hit, hit + needle.size());
    at = hit + needle.size();  // 不重叠
  }
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
  // 记住旧选中的文本位置附近：编辑后命中表重建，当前命中就近重定位
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
    const std::string picked = selected_text();
    if (!picked.empty()) editor_clipboard() = picked;
    return true;
  }
  if (action == "cut") {
    const std::string picked = selected_text();
    if (picked.empty() || read_only_) return false;
    editor_clipboard() = picked;
    push_undo(false);
    delete_selection();
    notify_change();
    return true;
  }
  if (action == "paste") {
    insert_text(editor_clipboard());
    return true;
  }
  if (action == "comment") return toggle_comment();
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
  if (action == "set_text") {
    set_text(std::string(argument));
    return true;
  }
  if (action == "find") {
    // argument: "needle" 或 "needle|case"（case: 0/1）
    const std::size_t bar = argument.find('|');
    if (bar == std::string_view::npos) {
      set_find(std::string(argument), false);
    } else {
      set_find(std::string(argument.substr(0, bar)), argument.substr(bar + 1) == "1");
    }
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
