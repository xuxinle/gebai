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
#include "st/core/string.hpp"   // parse_u64（逐行暂存的动作参数解析）
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
  // **内容真的变了才处理**（实测缺陷）：声明式宿主每帧都调 `set_diff`，
  // 而旧实现无条件清空 `selected_`——于是勾选行一旦触发重组（例如按钮
  // enabled 跟着选中数变）就被自己抹掉了，用户看到“勾了又没勾”。
  // 比较成本可忽（差异结构 + 行文本），而“以为没变”会让视图停在旧内容上，
  // 所以只在相等时早退，不省略比较。
  if (diff_ == diff) return;
  diff_ = diff;
  current_hunk_ = diff_.hunks.empty() ? kNoHunk : 0;
  // ⚠ **换了 diff 必须清空选择**：`selected_` 存的是 `(hunk, line)` **下标**，
  // 而新 diff 的同一下标指向的是**另一行**。不清的话用户会看到"我明明选的是
  // 第 3 行的新增，怎么补丁里删的是别处"——这是下标当身份用的经典陷阱。
  if (!selected_.empty()) {
    selected_.clear();
    if (on_selection_changed) on_selection_changed();
  }
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
                         std::string_view text, bool gutter_bar,
                         std::vector<std::pair<std::size_t, std::size_t>> inline_ranges) {
  CodeEditor::LineDecoration decoration;
  decoration.line = side.lines + 1;   // 装饰按**1 起**行号指定
  decoration.mark = mark;
  if (side.staging && !side.checks.empty()) {
    // ⚠ 取的是**最后一项**（调用方在调 `begin_row` 之前刚 push 的、属于这一行的那个），
    // 而不是 `checks[side.lines]`。
    //
    // 第一版就是 `check_for(side.lines)`——那是 **off-by-one**：`side.lines` 在
    // `begin_row` 里才自增，所以读取时下标指向"下一行"、而表里还没那一项，
    // 于是**永远返回 None**（现象：开了逐行暂存模式却一个勾选框都看不到，
    // 而 `staging` 状态、选择状态、补丁构造全都正常——最难查的那种）。
    decoration.check = side.checks.back();
  }
  decoration.number = number;
  decoration.gutter_bar = gutter_bar;
  // 行内改动区间（“这个字改了”）：数据来自 `DiffLine::inline_old`/`inline_new`。
  decoration.inline_ranges = std::move(inline_ranges);
  side.marks.push_back(std::move(decoration));
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
  // 勾选列**两栏都开**（2026-10-18 改：只开左栏时“新增行”根本无勾选框）。
  old_side_.staging = staging_mode_;
  old_side_.checks.clear();
  old_side_.checkbox_targets.clear();
  // 右栏也参与逐行暂存（新增行只在右侧有勾选框，见下方右侧分支的解释）。
  new_side_.staging = staging_mode_;
  new_side_.checks.clear();
  new_side_.checkbox_targets.clear();
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
        // 勾选态：**在推行时同步建立"显示行 → 勾选态"这一一对应表**。
        // 放在这里而不是另起一遍遍历：显示行数由 `begin_row` 决定（含填充行），
        // 另起一遍必然要对齐两套循环（而它们只要差一行，勾选框就会整体错位一格）。
        if (old_side_.staging) {
          CodeEditor::LineDecoration::Check check =
              CodeEditor::LineDecoration::Check::None;
          const auto line_index = static_cast<std::size_t>(row.left);
          if (show) {
            if (line->kind == text::DiffLineKind::Context) {
              // 上下文行**不可选**：选了不产生任何改动（`build_partial_patch` 会丢掉），
              // 画成灰框让"为什么点不动"一眼可见——比"点了没反应"强得多。
              check = CodeEditor::LineDecoration::Check::Disabled;
            } else {
              check = is_line_selected(h, line_index)
                          ? CodeEditor::LineDecoration::Check::Checked
                          : CodeEditor::LineDecoration::Check::Unchecked;
            }
          }
          old_side_.checks.push_back(check);
          // 身份表与勾选态表**同批追加**（见字段说明：分家就会错位）。
          old_side_.checkbox_targets.push_back(
              show && line->kind != text::DiffLineKind::Context
                  ? std::make_pair(h, line_index)
                  : std::make_pair(kNoHunk, std::size_t{0}));
        }
        begin_row(old_side_, show ? line->old_line : 0, mark,
                  show ? line->text : std::string_view{}, hidden,
                  show ? line->inline_old : std::vector<std::pair<std::size_t, std::size_t>>{});
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
        // 勾选列**两栏都画**（2026-10-18 修）：
        // 只画在左栏时，用户能勾的只有“被删除的行”——
        // “新增一行”在右侧，根本没有勾选框（实测：新增类型的改动无法逐行暂存）。
        // 两栏共用同一份 `selected_`（身份是 `(hunk, line)`），所以勾任一侧
        // 都会点亮整对改动，语义上就是“这一处改动”。
        //
        // ⚠ **无条件 push**（不管是填充行还是上下文行）：`begin_row` 取的是
        // `checks.back()`，而 `checks` 的长度必须**等于显示行数**——少 push 一行，
        // 从那里开始就会整体错位一格（画面上完全看不出）。
        const bool checkable = show && line->kind != text::DiffLineKind::Context;
        const std::size_t right_index = checkable ? static_cast<std::size_t>(row.right) : 0U;
        CodeEditor::LineDecoration::Check right_check = CodeEditor::LineDecoration::Check::None;
        if (checkable) {
          right_check = is_line_selected(h, right_index)
                            ? CodeEditor::LineDecoration::Check::Checked
                            : CodeEditor::LineDecoration::Check::Unchecked;
        }
        new_side_.checks.push_back(right_check);
        new_side_.checkbox_targets.push_back(checkable
                                                ? std::make_pair(h, right_index)
                                                : std::make_pair(kNoHunk, std::size_t{0}));
        begin_row(new_side_, show ? line->new_line : 0, mark,
                  show ? line->text : std::string_view{}, hidden,
                  show ? line->inline_new : std::vector<std::pair<std::size_t, std::size_t>>{});
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
      begin_row(old_side_, number, mark, line.text, mark != CodeEditor::LineMark::None,
                is_added ? line.inline_new : line.inline_old);
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
    // 勾选框被点 → 反查 `(hunk, line)` 并切换。
    //
    // 反查靠 `old_side_.checks`：它的第 i 项对应第 i 个显示行，而"第 i 个显示行属于
    // 哪个 hunk 的哪一行"需要**另建一张表**——`checks` 只记了勾选态，没记身份。
    // 这张 `old_side_.checkbox_targets` 与 `checks` 在同一次遍历里同步建立（见
    // `rebuild_side_by_side`），所以两者的下标空间天然一致——不会出现
    // "勾选框与身份错位一格"。
    if (staging_mode_) {
      left_->on_checkbox_toggle = [this](std::size_t row) {
        if (row >= old_side_.checkbox_targets.size()) return;
        const auto [hunk, line] = old_side_.checkbox_targets[row];
        if (hunk == kNoHunk) return;   // 上下文行/填充行：不可选（画的就是灰框）
        (void)toggle_line(hunk, line);
      };
    } else {
      left_->on_checkbox_toggle = nullptr;
    }
  }
  if (right_ != nullptr) {
    // 内联模式右栏不参与：给它空文本（它此时也 `set_visible(false)`）。
    right_->set_language("diff");
    right_->set_show_line_numbers(show_line_numbers_);
    right_->set_read_only(true);
    right_->set_text(side_by_side ? new_side_.text : std::string{});
    right_->set_line_decorations(side_by_side ? new_side_.marks
                                              : std::vector<CodeEditor::LineDecoration>{});
    // 右栏的勾选回调（2026-10-18 加）：新增行只在右侧有勾选框，
    // 不装回调的话“点它没反应”（而左栏的勾选框是好的——最难查的那种半损坏）。
    if (staging_mode_ && side_by_side) {
      right_->on_checkbox_toggle = [this](std::size_t row) {
        if (row >= new_side_.checkbox_targets.size()) return;
        const auto [hunk, line] = new_side_.checkbox_targets[row];
        if (hunk == kNoHunk) return;   // 上下文行/填充行：不可选
        (void)toggle_line(hunk, line);
      };
    } else {
      right_->on_checkbox_toggle = nullptr;
    }
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
  // —— 逐行暂存的读面 ——
  if (name == "staging") return staging_mode_ ? "true" : "false";
  if (name == "selected_count") return std::to_string(selected_.size());
  if (name == "selected_lines") {
    // `hunk,line;hunk,line`（与 `input` 参数同口径）——补丁里的行序即此顺序。
    std::string out;
    for (const auto& [hunk, line] : selected_) {
      if (!out.empty()) out.push_back(';');
      out += std::format("{},{}", hunk, line);
    }
    return out;
  }
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
          "disabled_new_lines", "current_hunk_line",
          // 逐行暂存的读面（2026-10-18 补）：不开这个，调用方拿不到“勾了哪几行”，
          // 也就无法断言“暂存的是不是真的那几行”。
          "staging", "selected_count", "selected_lines"};
}

auto DiffView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "next_hunk") return next_hunk() != kNoHunk;
  if (action == "previous_hunk") return previous_hunk() != kNoHunk;
  // —— 逐行暂存的驱动面（2026-10-18 补）——
  //
  // 为何必须有（实测）：勾选态原本**只能靠鼠标点勾选框**——`selected_lines()`
  // 只有 C++ 访问器，属性面与动作面都没有入口。于是“逐行暂存”对自动化
  // （e2e / Agent）完全不可达：能看到按钮、能开模式，却无法勾中任何一行。
  //
  // 参数统一用 `hunk,line`（逗号分隔，与补丁里的坐标同口径）：
  // `toggle_line "0,1"`。`select_all` / `clear_selection` 不带参数。
  if (action == "toggle_line" || action == "select_line" || action == "deselect_line") {
    const std::size_t comma = argument.find(',');
    if (comma == std::string_view::npos) return false;
    const auto parsed_hunk = st::parse_u64(argument.substr(0, comma));
    const auto parsed_line = st::parse_u64(argument.substr(comma + 1));
    if (!parsed_hunk.has_value() || !parsed_line.has_value()) return false;
    const auto hunk = static_cast<std::size_t>(*parsed_hunk);
    const auto line = static_cast<std::size_t>(*parsed_line);
    const bool was_selected = is_line_selected(hunk, line);
    if (action == "select_line" && was_selected) return true;
    if (action == "deselect_line" && !was_selected) return true;
    (void)toggle_line(hunk, line);
    return true;
  }
  if (action == "select_all") {
    set_all_selected(true);
    return true;
  }
  if (action == "clear_selection") {
    set_all_selected(false);
    return true;
  }
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

/// 选择变化后的统一收尾：**重建装饰并推给编辑器**。
///
/// 为何必须重建（而不只是标脏）：勾选态是 `LineDecoration::check` 的一部分，
/// 而装饰由 `rebuild()` 生成——只标脏的话屏幕上什么都不会变（实测：点了勾选框，
/// 状态变了、画面没变）。
void DiffView::refresh_checkboxes() {

  if (!staging_mode_) return;
  rebuild();
  push_to_editors();
  mark_dirty();
}

void DiffView::set_staging_mode(bool value) {
  if (staging_mode_ == value) return;
  staging_mode_ = value;
  // ⚠ **必须重建**（不只是标脏）：勾选框是**装饰**的一部分，而装饰由 `rebuild()`
  // 生成 —— 只标脏的话 `checks` 表还是旧的（全是 `None`），于是"开了模式却看不到
  // 勾选框"。实测踩到：像素断言显示开/关模式的画面完全一样。
  rebuild();
  push_to_editors();
  // 关掉模式时**保留选择**：用户可能只是切回去看一眼差异，回来接着挑。
  // 清空选择是"取消操作"，那该由显式的动作承担（宿主可 `set_all_selected(false)`）。
  mark_layout_dirty();
}

auto DiffView::is_line_selected(std::size_t hunk, std::size_t line) const -> bool {
  for (const auto& item : selected_) {
    if (item.first == hunk && item.second == line) return true;
  }
  return false;
}

auto DiffView::toggle_line(std::size_t hunk, std::size_t line) -> bool {
  // 下面的状态变更都要**重建装饰**（勾选态画在装饰里，不重建就看不到变化）。
  if (hunk >= diff_.hunks.size()) return false;
  if (line >= diff_.hunks[hunk].lines.size()) return false;
  // **只允许选中"可暂存"的行**：上下文行与删除行在"暂存"这个动作里语义不同——
  // 上下文行没改（选了不产生任何改动，`build_partial_patch` 会丢掉它）；
  // 而删除行选中是"这次要删掉它"（合法）。所以上下文行**如实拒绝**，
  // 而不是"选上但不生效"（那会让按钮状态与实际结果对不上）。
  //
  // **勾"行"实际勾的是它所属的改动区段**（2026-10-18 修，实测驱动）：
  // 勾选列只能画在一侧（本组件画在左栏，所以能点到的是删除行），
  // 而用户看到的是“这一行变了”——只暂存删除部分等于把文件内容**删掉**
  // （实测：勾 `line 2` 得到的补丁是 `-line 2`，而不是改成 `line 2 CHANGED_A`）。
  // 真实用户对“一行改”的意图是**整对**（删旧 + 加新），所以选中单位取
  // “相邻连续改动组成的区段”（遇到上下文行断开）——与 git 的 hunk 直觉一致。
  if (diff_.hunks[hunk].lines[line].kind == text::DiffLineKind::Context) return false;
  // 找出该行所在区段（同 hunk 内、非 Context 的连续行）。
  const auto& all = diff_.hunks[hunk].lines;
  std::size_t first = line;
  while (first > 0 && all[first - 1].kind != text::DiffLineKind::Context) --first;
  std::size_t last = line;
  while (last + 1 < all.size() && all[last + 1].kind != text::DiffLineKind::Context) ++last;
  const bool already = is_line_selected(hunk, line);
  for (std::size_t at = first; at <= last; ++at) {
    const std::pair<std::size_t, std::size_t> key{hunk, at};
    const auto found = std::find(selected_.begin(), selected_.end(), key);
    if (already) {
      if (found != selected_.end()) selected_.erase(found);
    } else if (found == selected_.end()) {
      // 插入到**保持 (hunk, line) 升序**的位置：补丁里的行序必须按文件顺序，
      // 而用户点选的顺序是任意的（先点第 5 行再点第 2 行很常见）。
      auto position = selected_.begin();
      while (position != selected_.end() && *position < key) ++position;
      selected_.insert(position, key);
    }
  }
  refresh_checkboxes();
  if (on_selection_changed) on_selection_changed();
  return !already;
}

void DiffView::set_hunk_selected(std::size_t hunk, bool selected) {
  if (hunk >= diff_.hunks.size()) return;
  // 先从选择里摘掉该 hunk 的现有项（保持其它 hunk 的选择不动）。
  std::vector<std::pair<std::size_t, std::size_t>> kept;
  for (const auto& item : selected_) {
    if (item.first != hunk) kept.push_back(item);
  }
  selected_ = std::move(kept);
  if (selected) {
    for (std::size_t line = 0; line < diff_.hunks[hunk].lines.size(); ++line) {
      if (diff_.hunks[hunk].lines[line].kind == text::DiffLineKind::Context) continue;
      selected_.emplace_back(hunk, line);
    }
    // 整块选中后要重排（上面的追加是按行序的，但插入点在同一 hunk 尾部，
    // 而其它 hunk 的项可能排在它之后）。
    std::sort(selected_.begin(), selected_.end());
  }
  refresh_checkboxes();
  if (on_selection_changed) on_selection_changed();
}

void DiffView::set_all_selected(bool selected) {
  selected_.clear();
  if (selected) {
    for (std::size_t hunk = 0; hunk < diff_.hunks.size(); ++hunk) {
      for (std::size_t line = 0; line < diff_.hunks[hunk].lines.size(); ++line) {
        if (diff_.hunks[hunk].lines[line].kind == text::DiffLineKind::Context) continue;
        selected_.emplace_back(hunk, line);
      }
    }
  }
  refresh_checkboxes();
  if (on_selection_changed) on_selection_changed();
}

auto DiffView::build_stage_patch() const -> std::string {
  if (selected_.empty()) return {};
  // 把 `(hunk, line)` 对按 hunk 归组——`build_partial_patch` 收的正是那个形状。
  //
  // 选择单位已在 `toggle_line` 里抬成"改动区段"（勾一行 = 整对改动），
  // 所以这里直接按选中的行列出即可；无需再做扩展。
  std::vector<std::pair<std::size_t, std::vector<std::size_t>>> grouped;
  std::size_t cursor = 0;
  while (cursor < selected_.size()) {
    const std::size_t hunk = selected_[cursor].first;
    std::vector<std::size_t> lines;
    while (cursor < selected_.size() && selected_[cursor].first == hunk) {
      lines.push_back(selected_[cursor].second);
      ++cursor;
    }
    grouped.emplace_back(hunk, std::move(lines));
  }
  return text::build_partial_patch(diff_, grouped);
}

}  // namespace st::ui
