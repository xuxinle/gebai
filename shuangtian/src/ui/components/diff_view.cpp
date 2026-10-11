#include "st/ui/components/diff_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/text/text.hpp"
#include "st/ui/element.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

namespace {

/// 分隔条高度（`@@ … @@` 那一行）。比代码行略高：它是一条带注释的带子，
/// 与行号/代码同高会显得「又多了一行代码」。
constexpr float kSeparatorHeight{22.0f};
/// 头部高度（路径 + 增删统计）。
constexpr float kHeaderHeight{26.0f};
/// 分隔条左右的视觉边距（圆角底的边距；命中区仍是整行宽）。
constexpr float kSeparatorInset{6.0f};
/// 与 `CodeEditor` 的 `kTopPadding` 同值：算分隔条 y 时要和编辑器内容原点对齐。
/// 两处若漂移，分隔条会与它标注的那一块**错开若干像素**（而它看着仍然"有条"）。
constexpr float kEditorTopPadding{4.0f};

}  // namespace

DiffView::DiffView() = default;
DiffView::~DiffView() = default;

void DiffView::set_diff(const text::FileDiff& diff) {
  diff_ = diff;
  current_hunk_ = diff_.hunks.empty() ? kNoHunk : 0;
  rebuild();
  push_to_editors();
  mark_layout_dirty();
}

void DiffView::set_texts(std::string_view old_text, std::string_view new_text) {
  set_diff(text::make_file_diff(old_text, new_text));
}

void DiffView::clear() {
  diff_ = text::FileDiff{};
  current_hunk_ = kNoHunk;
  rebuild();
  push_to_editors();
  mark_layout_dirty();
}

void DiffView::set_layout(DiffLayout layout) {
  if (layout_ == layout) return;
  layout_ = layout;
  rebuild();
  push_to_editors();
  mark_layout_dirty();
}

void DiffView::set_show_line_numbers(bool value) {
  if (show_line_numbers_ == value) return;
  show_line_numbers_ = value;
  push_to_editors();
  mark_layout_dirty();
}

void DiffView::set_disabled_lines(std::vector<std::size_t> old_lines,
                                  std::vector<std::size_t> new_lines) {
  const auto normalize = [](std::vector<std::size_t>& lines) {
    std::ranges::sort(lines);
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
  };
  normalize(old_lines);
  normalize(new_lines);
  if (old_lines == disabled_old_ && new_lines == disabled_new_) return;
  disabled_old_ = std::move(old_lines);
  disabled_new_ = std::move(new_lines);
  rebuild();
  push_to_editors();
  mark_layout_dirty();
}

void DiffView::set_read_only(bool value) {
  read_only_ = value;
  if (left_ != nullptr) left_->set_read_only(value);
  if (right_ != nullptr) right_->set_read_only(true);
}

void DiffView::set_font_size(float size) {
  font_size_ = size;
  const float applied = size > 0.0f ? size : -1.0f;
  if (left_ != nullptr) left_->set_font_size(applied);
  if (right_ != nullptr) right_->set_font_size(applied);
  mark_layout_dirty();
}

auto DiffView::line_count(bool new_side) const -> std::size_t {
  return new_side ? new_side_.lines : old_side_.lines;
}

auto DiffView::summary() const -> std::string {
  std::string path = diff_.new_path.empty() ? diff_.old_path : diff_.new_path;
  if (path.empty()) path = diff_.old_path;
  if (path.empty() && diff_.hunks.empty()) return "无差异";
  std::string out = path;
  if (diff_.binary) return out + "  （二进制文件，无逐行差异）";
  if (diff_.added) {
    out += "  （新增文件）";
  } else if (diff_.deleted) {
    out += "  （删除文件）";
  } else if (diff_.renamed) {
    out += std::format("  （重命名自 {}）", diff_.old_path);
  }
  out += std::format("  +{} −{}", diff_.additions, diff_.deletions);
  if (!diff_.message.empty()) out += "  · " + diff_.message;
  return out;
}

auto DiffView::is_disabled(const std::vector<std::size_t>& list, std::size_t number) -> bool {
  return number != 0 && std::ranges::binary_search(list, number);
}

void DiffView::rebuild() {
  old_side_ = Side{};
  new_side_ = Side{};
  separator_rows_.clear();
  if (layout_ == DiffLayout::Unified) {
    rebuild_unified();
  } else {
    rebuild_side_by_side();
  }
}

void DiffView::begin_row(Side& side, std::size_t number, CodeEditor::LineMark mark,
                         std::string_view text, bool gutter_bar) {
  CodeEditor::LineDecoration decoration;
  decoration.line = side.lines + 1;   // 装饰按**1 起**行号指定
  decoration.mark = mark;
  decoration.number = number;
  decoration.gutter_bar = gutter_bar;
  side.marks.push_back(decoration);
  // 行分隔符**按“已写过几行”补**，不是看 `text` 是否为空：
  // 第一行就是空行（右栏的分隔行就是这种）时，看 `text.empty()` 会把这一行整个吃掉
  //——右栏从此比左栏少一行，整体上移一行（实测截图）。
  if (side.lines != 0) side.text += '\n';
  side.text += text;
  ++side.lines;
  // 行号表必须与显示行**逐一对应**（它是两栏对齐的权威数据，也是自动化的取样口）。
  // 把它放在这里而不是各调用点：三处调用点各推一次，早晚会漏（实测漏的正是分隔条那一行）。
  side.numbers.push_back(number);
}

void DiffView::rebuild_side_by_side() {
  for (std::size_t h = 0; h < diff_.hunks.size(); ++h) {
    const text::DiffHunk& hunk = diff_.hunks[h];
    const std::string header =
        std::format("@@ -{},{} +{},{} @@{}", hunk.old_start, hunk.old_count, hunk.new_start,
                    hunk.new_count, hunk.section.empty() ? std::string{} : " " + hunk.section);
    // 分隔条在两侧**都占一行**（两栏显示行必须一一对应；只占一侧会让后面全错位）。
    separator_rows_.push_back(old_side_.lines + 1);
    // 分隔条**不给行号**（`kNoNumber`）：它不属于任何一侧，不是代码行。
    // 旧写法给 0，而 0 的语义是“用本编辑器自己的行号”——截图里它就显示成了 1。
    begin_row(old_side_, CodeEditor::kNoNumber, CodeEditor::LineMark::None, header);
    // 右栏只占一行高度、**不重复显示 `@@`**（两栏同行本已是同一块的分隔线：
    // 右栏再画一遍会让人以为“右侧也有一段这样的内容”）。
    begin_row(new_side_, CodeEditor::kNoNumber, CodeEditor::LineMark::None, {});
    // `hunk_rows` 存「该块第一行」的 1 起行号（分隔条自己占它上面那一行）。
    old_side_.hunk_rows.push_back(old_side_.lines + 1);
    new_side_.hunk_rows.push_back(new_side_.lines + 1);

    // **成对交错**（`build_rows`）：一次「改动」里的删除行与新增行要占**同一批显示行**
    // （删 3 行、加 1 行 ⇒ 这一段 3 行高，右侧补两行填充）。
    //
    // 这一步做错是**最贵的一类错**：把每个 `DiffLine` 各生成一行（不做配对）会让
    // 两栏从第一处改动起整体错位，于是每个被修改的行都显示成“左侧删 A、右侧加别处的 B”
    // ——而界面上完全看不出是哪一步错了（实测第一次落地的截图就是这样）。
    const std::vector<text::DiffRow> rows = text::build_rows(hunk, h);
    for (const text::DiffRow& row : rows) {
      // —— 左栏（旧侧）——
      {
        const bool present = row.left >= 0;
        const text::DiffLine* line = present ? &hunk.lines[static_cast<std::size_t>(row.left)]
                                             : nullptr;
        // 「禁用」（撤销/压缩提交里的已应用改动）：该侧这一行**不出现**，
        // 而它照样占一行——否则两栏会从这一行起整体错位。
        const bool hidden = line != nullptr && is_disabled(disabled_old_, line->old_line);
        const bool show = line != nullptr && !hidden;
        CodeEditor::LineMark mark = CodeEditor::LineMark::Filler;
        if (show) {
          mark = line->kind == text::DiffLineKind::Removed ? CodeEditor::LineMark::Removed
                                                           : CodeEditor::LineMark::None;
        } else if (hidden) {
          mark = CodeEditor::LineMark::None;   // 藏起来的行：不留填充底（它是“被去掉的”）
        }
        begin_row(old_side_, show ? line->old_line : 0, mark,
                  show ? line->text : std::string_view{}, hidden);
      }
      // —— 右栏（新侧）——
      {
        const bool present = row.right >= 0;
        const text::DiffLine* line = present ? &hunk.lines[static_cast<std::size_t>(row.right)]
                                             : nullptr;
        const bool hidden = line != nullptr && is_disabled(disabled_new_, line->new_line);
        const bool show = line != nullptr && !hidden;
        CodeEditor::LineMark mark = CodeEditor::LineMark::Filler;
        if (show) {
          mark = line->kind == text::DiffLineKind::Added ? CodeEditor::LineMark::Added
                                                         : CodeEditor::LineMark::None;
        } else if (hidden) {
          mark = CodeEditor::LineMark::None;
        }
        begin_row(new_side_, show ? line->new_line : 0, mark,
                  show ? line->text : std::string_view{}, hidden);
      }
    }
  }
}

void DiffView::rebuild_unified() {
  for (std::size_t h = 0; h < diff_.hunks.size(); ++h) {
    const text::DiffHunk& hunk = diff_.hunks[h];
    const std::string header =
        std::format("@@ -{},{} +{},{} @@{}", hunk.old_start, hunk.old_count, hunk.new_start,
                    hunk.new_count, hunk.section.empty() ? std::string{} : " " + hunk.section);
    separator_rows_.push_back(old_side_.lines + 1);
    begin_row(old_side_, CodeEditor::kNoNumber, CodeEditor::LineMark::None, header);
    old_side_.hunk_rows.push_back(old_side_.lines + 1);
    for (const text::DiffLine& line : hunk.lines) {
      const bool hidden = line.kind == text::DiffLineKind::Removed
                              ? is_disabled(disabled_old_, line.old_line)
                              : is_disabled(disabled_new_, line.new_line);
      if (hidden) continue;
      // 内联：删除行与新增行都照原样出现（没有对齐用的占位行）。
      const bool is_added = line.kind == text::DiffLineKind::Added;
      const std::size_t number = is_added ? line.new_line : line.old_line;
      const CodeEditor::LineMark mark = is_added ? CodeEditor::LineMark::Added
                                                 : (line.kind == text::DiffLineKind::Removed
                                                        ? CodeEditor::LineMark::Removed
                                                        : CodeEditor::LineMark::None);
      begin_row(old_side_, number, mark, line.text, mark != CodeEditor::LineMark::None);
    }
  }
}

void DiffView::push_to_editors() {
  const bool side_by_side = layout_ == DiffLayout::SideBySide;
  if (left_ != nullptr) {
    left_->set_language("diff");
    left_->set_show_line_numbers(show_line_numbers_);
    left_->set_read_only(read_only_ || !allow_left_edit_);
    left_->set_text(old_side_.text);
    left_->set_line_decorations(old_side_.marks);
  }
  if (right_ != nullptr) {
    // 内联模式右栏不参与：给它空文本（它此时也 `set_visible(false)`）。
    right_->set_language("diff");
    right_->set_show_line_numbers(show_line_numbers_);
    right_->set_read_only(true);
    right_->set_text(side_by_side ? new_side_.text : std::string{});
    right_->set_line_decorations(side_by_side ? new_side_.marks
                                              : std::vector<CodeEditor::LineDecoration>{});
  }
}

auto DiffView::hunk_first_line(std::size_t index, bool new_side) const -> std::size_t {
  const Side& side = new_side ? new_side_ : old_side_;
  if (index >= side.hunk_rows.size()) return 0;
  return side.hunk_rows[index];   // 该块第一行的 1 起行号
}

auto DiffView::step_hunk(int delta) -> std::size_t {
  const std::size_t count = diff_.hunks.size();
  if (count == 0) return kNoHunk;
  std::size_t next = 0;
  if (current_hunk_ == kNoHunk) {
    next = delta >= 0 ? 0 : count - 1;
  } else if (delta >= 0) {
    next = (current_hunk_ + 1) % count;
  } else {
    next = (current_hunk_ + count - 1) % count;
  }
  return goto_hunk(next) ? next : kNoHunk;
}

auto DiffView::goto_hunk(std::size_t index) -> bool {
  if (index >= diff_.hunks.size()) return false;
  current_hunk_ = index;
  // 导航要**先有行高**：`scroll_to_line` 依赖行高缓存（首次绘制/排布前为 0）。
  if (left_ != nullptr) {
    const std::size_t row = hunk_first_line(index, false);
    if (row != 0) left_->scroll_to_line(row);
  }
  if (layout_ == DiffLayout::SideBySide && right_ != nullptr) {
    const std::size_t row = hunk_first_line(index, true);
    if (row != 0) right_->scroll_to_line(row);
  }
  if (on_hunk_change) on_hunk_change(index);
  mark_dirty();
  return true;
}

void DiffView::sync_scroll_from(CodeEditor& source) {
  if (syncing_) return;
  syncing_ = true;
  const float y = source.scroll_offset().y;
  if (layout_ == DiffLayout::SideBySide) {
    CodeEditor* other = (&source == left_) ? right_ : left_;
    if (other != nullptr) {
      // 只同步纵向：两栏内容等行，横向各看各的（长行不该互相绑架）。
      other->set_scroll_offset(other->scroll_offset().x, y);
    }
  }
  syncing_ = false;
  if (on_scroll) on_scroll(y);
}

void DiffView::apply_theme(const Theme& theme) {
  (void)theme;
  // 两侧编辑器是子元素，主题由 `UiRoot` 分别下发；本组件没有自己的颜色状态。
  mark_dirty();
}

void DiffView::measure(const RenderContext& context, const Constraints& constraints) {
  // 差异视图**不收缩**：它要吃掉给它的全部高度（内容可滚动，自然高度没有意义）。
  const float height = constraints.max_height < kUnbounded ? constraints.max_height : kHeaderHeight;
  measured_ = math::Size{constraints.max_width, height};
  (void)context;
}

void DiffView::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  const bool side_by_side = layout_ == DiffLayout::SideBySide;
  const float body_y = rect.y + kHeaderHeight;
  const float body_height = std::max(0.0f, rect.height - kHeaderHeight);
  const float half = side_by_side ? std::floor(rect.width * 0.5f) : rect.width;

  if (left_ == nullptr) {
    auto owned = std::make_unique<CodeEditor>();
    left_ = owned.get();
    left_->set_id("diff-left");
    left_->set_show_line_numbers(show_line_numbers_);
    left_->set_read_only(read_only_ || !allow_left_edit_);
    left_->set_language("diff");
    if (font_size_ > 0.0f) left_->set_font_size(font_size_);
    left_->on_scroll = [this](float, float) { sync_scroll_from(*left_); };
    left_->set_text(old_side_.text);
    left_->set_line_decorations(old_side_.marks);
    add_child(std::move(owned));
  }
  if (side_by_side && right_ == nullptr) {
    auto owned = std::make_unique<CodeEditor>();
    right_ = owned.get();
    right_->set_id("diff-right");
    right_->set_show_line_numbers(show_line_numbers_);
    // 右栏**恒只读**：两侧都可写会让"同一份内容有两个真值源"（改哪边算改？）。
    // 将来要支持"接受左侧"这类操作，也应经命令回流到文档，而不是让两栏各写一份。
    right_->set_read_only(true);
    right_->set_language("diff");
    if (font_size_ > 0.0f) right_->set_font_size(font_size_);
    right_->on_scroll = [this](float, float) { sync_scroll_from(*right_); };
    right_->set_text(new_side_.text);
    right_->set_line_decorations(new_side_.marks);
    add_child(std::move(owned));
  }
  if (right_ != nullptr) {
    // 切模式只翻可见位（子元素与其状态保留，切回来时滚动位置还在）。
    right_->set_visible(side_by_side);
  }

  const auto layout_editor = [&](CodeEditor& editor, math::Rect area) {
    Constraints constraints;
    constraints.max_width = area.width;
    constraints.max_height = area.height;
    constraints.available_width = area.width;
    constraints.available_height = area.height;
    editor.measure(context, constraints);
    editor.arrange(context, area);
    editor.apply_theme(context.theme);
  };
  layout_editor(*left_, math::Rect{rect.x, body_y, side_by_side ? half : rect.width, body_height});
  if (side_by_side && right_ != nullptr) {
    layout_editor(*right_,
                  math::Rect{rect.x + half, body_y, rect.width - half, body_height});
  }

  // —— 分隔条位置：由**行高 × 行号**推出，与编辑器绘制同源 ——
  //
  // 不自己算行高（那是编辑器的事）：读它的 `line_height` 属性。两者若各算一遍，
  // 分隔条会与它标注的那一块错开（而它看着仍然"有条"）。
  const auto line_height_of = [](const CodeEditor& editor) -> float {
    const auto value = editor.get_property("line_height");
    if (!value.has_value()) return 0.0f;
    try {
      return std::stof(*value);
    } catch (...) {
      return 0.0f;
    }
  };
  const float line_height = line_height_of(*left_);
  separator_rects_.clear();
  separator_rects_.reserve(separator_rows_.size());
  if (line_height > 0.0f) {
    const float scroll_y = left_->scroll_offset().y;
    for (const std::size_t row : separator_rows_) {
      const float y = body_y + kEditorTopPadding - scroll_y +
                      static_cast<float>(row - 1) * line_height;
      separator_rects_.push_back(math::Rect{
          rect.x + kSeparatorInset, y,
          std::max(0.0f, rect.width - kSeparatorInset * 2.0f), kSeparatorHeight});
    }
  }
}

auto DiffView::separator_rect(std::size_t index) const -> math::Rect {
  return index < separator_rects_.size() ? separator_rects_[index] : math::Rect{};
}

void DiffView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  const Palette& colors = context.theme.colors();
  canvas.fill_rect(math::Rect{bounds_.x, bounds_.y, bounds_.width, kHeaderHeight},
                   raster::Paint::solid(colors.surface_alt), 0.0f);
  const TextPort* port = context.text;
  if (port != nullptr) {
    const float size = context.theme.metrics().font_sm;
    const std::string label = summary();
    const float text_y = bounds_.y + layout_line(*port, label, size, kHeaderHeight).origin_y;
    port->draw(canvas, label, math::Point{bounds_.x + 8.0f, text_y}, size,
               diff_.binary ? colors.text_muted : colors.text, text::FontRole::Proportional);
  }

  // 分隔条：`@@ … @@` 那条带子；当前所在的差异块加一道主色竖条。
  for (std::size_t index = 0; index < separator_rects_.size(); ++index) {
    const math::Rect& bar = separator_rects_[index];
    if (bar.y + bar.height < bounds_.y || bar.y > bounds_.y + bounds_.height) continue;
    const bool active = index == current_hunk_ && current_hunk_ != kNoHunk;
    canvas.fill_rect(bar,
                     raster::Paint::solid(colors.surface_hover.with_alpha_f(active ? 0.9F : 0.5F)),
                     3.0f);
    if (active) {
      canvas.fill_rect(math::Rect{bar.x, bar.y, 3.0f, bar.height},
                       raster::Paint::solid(colors.primary), 1.5f);
    }
  }
}

auto DiffView::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  // 点击分隔条 → 跳到该差异块（「点一下就去那里」是差异视图的基本手势）。
  if (event.kind == EventKind::Click) {
    for (std::size_t index = 0; index < separator_rects_.size(); ++index) {
      if (separator_rects_[index].contains(event.position)) {
        (void)goto_hunk(index);
        return true;
      }
    }
  }
  return false;
}

auto DiffView::semantics_text() const -> std::string { return summary(); }

auto DiffView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "hunks") return std::to_string(diff_.hunks.size());
  if (name == "current_hunk") {
    return current_hunk_ == kNoHunk ? std::string("-1") : std::to_string(current_hunk_);
  }
  if (name == "additions") return std::to_string(diff_.additions);
  if (name == "deletions") return std::to_string(diff_.deletions);
  if (name == "layout") return layout_ == DiffLayout::SideBySide ? "side_by_side" : "unified";
  if (name == "binary") return diff_.binary ? "true" : "false";
  if (name == "summary") return summary();
  if (name == "old_lines") return std::to_string(old_side_.lines);
  if (name == "new_lines") return std::to_string(new_side_.lines);
  if (name == "left_text") return left_ != nullptr ? left_->text() : std::string{};
  if (name == "right_text") return right_ != nullptr ? right_->text() : std::string{};
  if (name == "scroll") {
    const float y = left_ != nullptr ? left_->scroll_offset().y : 0.0f;
    return std::format("{:.1f}", static_cast<double>(y));
  }
  if (name == "disabled_old_lines") return join_numbers(disabled_old_);
  if (name == "disabled_new_lines") return join_numbers(disabled_new_);
  if (name == "current_hunk_line") {
    return current_hunk_ == kNoHunk ? std::string("0")
                                    : std::to_string(hunk_first_line(current_hunk_, false));
  }
  // 行号映射（自动化核「两栏行号对不对」用）：第 `row`（1 起）个**显示行**在该侧的真实行号。
  if (name == "number_old" || name == "number_new") {
    return std::nullopt;   // 需要参数，见 `number_at`（属性面只给整表）
  }
  if (name == "numbers_old") return join_numbers(old_side_.numbers);
  if (name == "numbers_new") return join_numbers(new_side_.numbers);
  return std::nullopt;
}

auto DiffView::join_numbers(const std::vector<std::size_t>& values) -> std::string {
  std::string out;
  out.reserve(values.size() * 3);
  for (const std::size_t value : values) {
    if (!out.empty()) out += ',';
    out += std::to_string(value);
  }
  return out;
}

auto DiffView::parse_numbers(std::string_view text) -> std::optional<std::vector<std::size_t>> {
  std::vector<std::size_t> parsed;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t comma = text.find(',', begin);
    const std::string_view piece =
        comma == std::string_view::npos ? text.substr(begin) : text.substr(begin, comma - begin);
    if (!piece.empty()) {
      try {
        parsed.push_back(static_cast<std::size_t>(std::stoull(std::string(piece))));
      } catch (...) {
        return std::nullopt;
      }
    }
    if (comma == std::string_view::npos) break;
    begin = comma + 1;
  }
  return parsed;
}

auto DiffView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "layout") {
    if (value == "side_by_side" || value == "sidebyside") {
      set_layout(DiffLayout::SideBySide);
      return true;
    }
    if (value == "unified") {
      set_layout(DiffLayout::Unified);
      return true;
    }
    return false;
  }
  if (name == "show_line_numbers") {
    set_show_line_numbers(value == "true" || value == "1");
    return true;
  }
  if (name == "read_only") {
    set_read_only(value == "true" || value == "1");
    return true;
  }
  if (name == "font_size") {
    try {
      set_font_size(std::stof(std::string(value)));
      return true;
    } catch (...) {
      return false;
    }
  }
  if (name == "disabled_old_lines" || name == "disabled_new_lines") {
    const auto parsed = parse_numbers(value);
    if (!parsed.has_value()) return false;
    if (name == "disabled_old_lines") {
      set_disabled_lines(*parsed, disabled_new_);
    } else {
      set_disabled_lines(disabled_old_, *parsed);
    }
    return true;
  }
  if (name == "scroll") {
    if (left_ == nullptr) return false;
    try {
      const float y = static_cast<float>(std::stod(std::string(value)));
      left_->set_scroll_offset(left_->scroll_offset().x, y);
      sync_scroll_from(*left_);
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
}

auto DiffView::property_names() const -> std::vector<std::string_view> {
  return {"hunks",      "current_hunk",       "additions",  "deletions", "layout",
          "binary",     "summary",            "old_lines",  "new_lines", "numbers_old",
          "numbers_new", "left_text",         "right_text", "scroll",    "disabled_old_lines",
          "disabled_new_lines", "current_hunk_line"};
}

auto DiffView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "next_hunk") return next_hunk() != kNoHunk;
  if (action == "previous_hunk") return previous_hunk() != kNoHunk;
  if (action == "goto_hunk") {
    try {
      return goto_hunk(static_cast<std::size_t>(std::stoull(std::string(argument))));
    } catch (...) {
      return false;
    }
  }
  if (action == "set_layout") return set_property("layout", argument);
  if (action == "clear") {
    clear();
    return true;
  }
  return false;
}

}  // namespace st::ui
