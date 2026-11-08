#include "st/ui/components/diff_view.hpp"
#include "st/ui/component_registry.hpp"  // 登记与导入需要注册表接口

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
/// 缩略条的上下内边距（轨道不贴到 body 边缘）。
constexpr float kOverviewInset{4.0f};
/// 缩略条里改动短横的高度。比行高细得多：它是“分布示意”，不是正文。
/// 取 2px：20 处改动在 600px 轨道里各占 3px 时就“连成一条虚线”，
/// 看着像滚动条而不是分布图（实测截图就是这个观感）。
constexpr float kOverviewMarkHeight{2.0f};
/// 缩略条最小可拖高度：长文件里可见比很小，再小就抓不住了。
constexpr float kOverviewMinBox{12.0f};
/// 缩略条与右栏竖向滚动条之间的间隙（避让热区冲突）。
constexpr float kOverviewScrollbarGap{14.0f};

}  // namespace

DiffView::DiffView() = default;
DiffView::~DiffView() = default;

void DiffView::set_diff(const text::FileDiff& diff) {
  // **内容真的变了才处理**：声明式宿主每帧都调 `set_diff`，而无条件重建会把
  // 两栏的滚动位置与光标当场抹掉（用户在阅读，画面却自己跳回开头）。
  // 比较成本可忽（差异结构 + 行文本），而“以为没变”会让视图停在旧内容上。
  if (diff_ == diff) return;
  diff_ = diff;
  current_hunk_ = diff_.hunks.empty() ? kNoHunk : 0;
  // ⚠ **不在这里置“首次定位”**：`set_diff` 也会被“右栏编辑后自动重算”调到，
  // 而那时把视口拽回第一处等于：用户正在打第 200 行，画面每 180ms 跳回第 1 行。
  // “打开就跳第一处”由宿主在**真的打开**时显式调 `request_focus_first_hunk()`。
  rebuild();
  push_to_editors();
  mark_layout_dirty();
}

void DiffView::set_texts(std::string_view old_text, std::string_view new_text) {
  set_diff(text::make_file_diff(old_text, new_text));
}

void DiffView::set_full_texts(std::string old_full, std::string new_full, std::string path,
                              bool keep_numbers) {
  // 两侧全文（含路径）一致 ⇒ 无变化（声明式每帧都会调，不能无条件重建）。
  if (full_file_ && full_old_ == old_full && full_new_ == new_full && full_path_ == path &&
      keep_numbers_ == keep_numbers) {
    return;
  }
  full_file_ = true;
  keep_numbers_ = keep_numbers;
  full_old_ = std::move(old_full);
  full_new_ = std::move(new_full);
  full_path_ = std::move(path);
  // 差异结构从**这两段全文**现算：整文件模式下的“新旧”只能是它们自己，
  // 不能吃宿主的 `diff_`（那个可能来自另一对文本，例如暂存区↔工作区）。
  diff_ = text::make_file_diff(full_old_, full_new_);
  // 路径只用于摘要展示（`summary()` 优先读 `diff_` 的路径，这里补上）。
  diff_.old_path = full_path_;
  diff_.new_path = full_path_;
  current_hunk_ = diff_.hunks.empty() ? kNoHunk : 0;
  rebuild();
  push_to_editors();
  // **重算后重新落实“该停在哪一块”**：`push_to_editors` 会把新文本推给编辑器，
  // 而两栏文本变了就意味着滚动位置得重新算（`replace_text_keep_state` 保留的是
  // 旧文本的滚动——切文件时那个值属于另一个文件，必须丢掉）。
  //
  // 为何不能只靠 `arrange` 里的消费：`set_full_texts` 之后宿主会调
  // `request_focus_first_hunk()`，那时 `bounds_` 可能已是旧值 → 立即滚会算出 0。
  // 统一在这里把待办与当前块重新对齐（`arrange` 里只负责真正滚）。
  if (!diff_.hunks.empty()) pending_scroll_hunk_ = current_hunk_;
  mark_layout_dirty();
}

void DiffView::commit_side_edit(bool new_side, std::string_view logical) {
  // **编辑闭环的同帧入口**：把用户刚敲的文本写进快照并立即重算差异。
  //
  // 为何需要它（2026-11-06）：编辑回调（`on_text_edit`）交给宿主的是“当前逻辑文本”，
  // 宿主写进自己的成员后靠 `State` 触发**下一次重组**才 `set_full_texts`。那段窗口里
  // 快照比编辑器旧一步——界面于是会有一帧“差异没跟着变”。把重算收在这里，
  // “改了就重算”是**同一次调用内**完成的。
  //
  // ⚠ **不重推文本**：调用方（编辑器自己）刚改完，此时再 `push_to_editors()` 会
  // 把重建出的显示行（带对齐填充）写回去，光标会被夹到另一行（正是那条老缺陷的形态）。
  // 所以这里只做：快照 → 差异 → 重建 → 装饰/配置，**跳过文本推送**。
  if (!full_file_) return;   // 差异块模式没有“两端全文”可改（右栏也编辑不了）
  std::string updated(logical);
  if (new_side ? full_new_ == updated : full_old_ == updated) return;
  if (new_side) {
    full_new_ = std::move(updated);
  } else {
    full_old_ = std::move(updated);
  }
  diff_ = text::make_file_diff(full_old_, full_new_);
  diff_.old_path = full_path_;
  diff_.new_path = full_path_;
  if (diff_.hunks.empty()) {
    current_hunk_ = kNoHunk;
  } else if (current_hunk_ == kNoHunk || current_hunk_ >= diff_.hunks.size()) {
    current_hunk_ = 0;
  }
  rebuild();
  // 只刷新装饰与配置面（不推文本，见上面的警告）。
  if (left_ != nullptr) left_->set_line_decorations(old_side_.marks);
  if (right_ != nullptr) {
    right_->set_line_decorations(layout_ == DiffLayout::SideBySide
                                     ? new_side_.marks
                                     : std::vector<CodeEditor::LineDecoration>{});
  }
  apply_editor_config();
  mark_layout_dirty();
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
  // 内联模式下右栏隐藏 → 它不该可写（否则用键盘 Tab 能聚焦到一个看不见的编辑器）。
  if (right_ != nullptr) {
    right_->set_read_only(read_only_ || layout_ != DiffLayout::SideBySide);
  }
  mark_layout_dirty();
}

void DiffView::set_show_line_numbers(bool value) {
  if (show_line_numbers_ == value) return;
  show_line_numbers_ = value;
  apply_editor_config();
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
  if (read_only_ == value) return;
  read_only_ = value;
  // 具体到每一栏的只读位由 `apply_editor_config` 统一算（左栏看 `allow_left_edit_`，
  // 右栏看并列/内联）——这里不另写一套，以免两处口径分叉。
  //
  // **不碰文本**：本函数是声明式宿主每帧都会调的“配置面”接口，
  // 而重建快照可能落后于用户刚打的字（见 `apply_editor_config` 的说明）。
  apply_editor_config();
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
  } else if (full_file_) {
    rebuild_side_by_side_full();
  } else {
    rebuild_side_by_side();
  }
}

void DiffView::rebuild_side_by_side_full() {
  // 整文件并列：**先按行切成两份表**，再逐行推进。
  //
  // 算法与差异块模式同构（块内仍用 `build_rows` 配对），差别只在“块与块之间”：
  // 差异块模式直接跳到下一块（中间的行不显示），而整文件模式要**把中间的行补上**——
  // 那正是“编辑器与对比一体”的意思：看到的是整份文件，改动只是其中的标记。
  const auto split_lines = [](const std::string& text) {
    std::vector<std::string_view> out;
    std::size_t begin = 0;
    const auto push_trimmed = [&](std::size_t from, std::size_t to) {
      // **去掉行尾 `\r`**：CRLF 文件里每一行都带它，而 `text::diff_hunks` 比的是
      // 去掉后的语义行——两边口径不一致时，块内的行显示干净、块间的上下文行带着
      // 不可见的 `\r`，自动对比就会把它们判成改动（实测：整篇一片红）。
      while (to > from && (text[to - 1] == '\r' || text[to - 1] == '\n')) --to;
      out.push_back(std::string_view(text).substr(from, to - from));
    };
    for (std::size_t index = 0; index < text.size(); ++index) {
      if (text[index] == '\n') {
        push_trimmed(begin, index);
        begin = index + 1;
      }
    }
    // 末尾没有换行的最后一段也是内容；而**空文本**不该多出一行空行。
    if (begin < text.size() || text.empty()) push_trimmed(begin, text.size());
    return out;
  };
  const std::vector<std::string_view> old_lines = split_lines(full_old_);
  const std::vector<std::string_view> new_lines = split_lines(full_new_);

  std::size_t old_cursor = 0;   // 下一个未消费的旧侧行（0 起）
  std::size_t new_cursor = 0;   // 下一个未消费的新侧行

  const auto append_context = [&](std::size_t old_upto, std::size_t new_upto) {
    // 块前纯上下文：两侧行号同步递进（差异算法保证这两段行数一致）。
    while (old_cursor < old_upto && new_cursor < new_upto) {
      begin_row(old_side_, old_cursor + 1, CodeEditor::LineMark::None, old_lines[old_cursor],
                /*gutter_bar=*/false, {}, /*real=*/true);
      begin_row(new_side_, new_cursor + 1, CodeEditor::LineMark::None, new_lines[new_cursor],
                /*gutter_bar=*/false, {}, /*real=*/true);
      ++old_cursor;
      ++new_cursor;
    }
  };

  for (std::size_t h = 0; h < diff_.hunks.size(); ++h) {
    const text::DiffHunk& hunk = diff_.hunks[h];
    // `old_start/new_start` 是 1 起；块内部的上下文行也算在块里，
    // 所以块**实际占用**的起点要按 `context` 回退——不过 `diff_hunks` 的
    // `old_start` 已经是“含上下文的第一行”（统一 diff 口径），直接用即可。
    const std::size_t old_begin = hunk.old_count == 0 ? hunk.old_start : hunk.old_start - 1;
    const std::size_t new_begin = hunk.new_count == 0 ? hunk.new_start : hunk.new_start - 1;
    append_context(std::min(old_begin, old_lines.size()), std::min(new_begin, new_lines.size()));

    // `hunk_rows` **总是**记（它是导航与 `current_hunk_line` 的依据，与画不画分隔条无关）。
    old_side_.hunk_rows.push_back(old_side_.lines + 1);
    new_side_.hunk_rows.push_back(new_side_.lines + 1);
    if (show_separators_) {
      separator_rows_.push_back(old_side_.lines + 1);
      const std::string header =
          std::format("@@ -{},{} +{},{} @@{}", hunk.old_start, hunk.old_count, hunk.new_start,
                      hunk.new_count, hunk.section.empty() ? std::string{} : " " + hunk.section);
      begin_row(old_side_, CodeEditor::kNoNumber, CodeEditor::LineMark::None, header,
                  /*gutter_bar=*/false, {}, /*real=*/false);
      begin_row(new_side_, CodeEditor::kNoNumber, CodeEditor::LineMark::None, {},
                  /*gutter_bar=*/false, {}, /*real=*/false);
    }

    const std::vector<text::DiffRow> rows = text::build_rows(hunk, h);
    for (const text::DiffRow& row : rows) {
      {
        const bool present = row.left >= 0;
        const text::DiffLine* line =
            present ? &hunk.lines[static_cast<std::size_t>(row.left)] : nullptr;
        const bool show = line != nullptr;
        CodeEditor::LineMark mark = CodeEditor::LineMark::Filler;
        if (show) {
          mark = line->kind == text::DiffLineKind::Removed ? CodeEditor::LineMark::Removed
                                                          : CodeEditor::LineMark::None;
        }
        begin_row(old_side_, show ? line->old_line : (keep_numbers_ ? old_cursor + 1 : 0), mark,
                  show ? line->text : std::string_view{}, false,
                  show ? line->inline_old : std::vector<std::pair<std::size_t, std::size_t>>{},
                  /*real=*/show);
        if (show) old_cursor = std::max(old_cursor, static_cast<std::size_t>(line->old_line));
      }
      {
        const bool present = row.right >= 0;
        const text::DiffLine* line =
            present ? &hunk.lines[static_cast<std::size_t>(row.right)] : nullptr;
        const bool show = line != nullptr;
        CodeEditor::LineMark mark = CodeEditor::LineMark::Filler;
        if (show) {
          mark = line->kind == text::DiffLineKind::Added ? CodeEditor::LineMark::Added
                                                        : CodeEditor::LineMark::None;
        }
        begin_row(new_side_, show ? line->new_line : (keep_numbers_ ? new_cursor + 1 : 0), mark,
                  show ? line->text : std::string_view{}, false,
                  show ? line->inline_new : std::vector<std::pair<std::size_t, std::size_t>>{},
                  /*real=*/show);
        if (show) new_cursor = std::max(new_cursor, static_cast<std::size_t>(line->new_line));
      }
    }
  }
  // 末尾剩余（最后一个块之后的行）。
  append_context(old_lines.size(), new_lines.size());
}

auto DiffView::split_lines_of(std::string_view text) -> std::vector<std::string_view> {
  // 切行的一个口径：去掉行尾 `\r`。
  //
  // CRLF 文件里每一行都带它，而 `diff_hunks` 比的是去掉后的语义行——
  // 两边口径不一致时，块内的行看着干净、块间的上下文行带着不可见的 `\r`，
  // 自动对比就会把它们判成改动（实测：整篇一片红）。
  std::vector<std::string_view> out;
  std::size_t begin = 0;
  const auto push = [&](std::size_t from, std::size_t to) {
    while (to > from && (text[to - 1] == '\r' || text[to - 1] == '\n')) --to;
    out.push_back(text.substr(from, to - from));
  };
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\n') {
      push(begin, index);
      begin = index + 1;
    }
  }
  // 末尾没有换行的最后一段也是内容；而**空文本**不该多出一行空行。
  if (begin < text.size() || text.empty()) push(begin, text.size());
  return out;
}

auto DiffView::logical_text(bool new_side) const -> std::string {
  const Side& side = new_side ? new_side_ : old_side_;
  // 从**当前编辑器里的文本**取行：它才是用户真的在编辑的那份（含新键入的行）。
  //
  // 为何不用 `side.text`：那是**上一次重建**的快照——用户刚打的字不在里面，
  // 自动对比会永远比到旧内容（实测：“改了但加法数不变”）。
  const CodeEditor* editor = new_side ? right_ : left_;
  const std::string& current = editor != nullptr ? editor->text() : side.text;

  // —— 没动过：快照与当前文本逐字节相同 ⇒ `real` 表直接逐行对应 ——
  //
  // 这是**唯一精确**的路径：填充行与真实行一一对上是重建时就知道的事实。
  if (current == side.text) {
    std::string out;
    std::size_t begin = 0;
    std::size_t row = 0;
    const auto append = [&](std::size_t from, std::size_t to) {
      if (row < side.real.size() && side.real[row] == 0) {
        ++row;
        return;
      }
      if (!out.empty()) out.push_back('\n');
      while (to > from && (current[to - 1] == '\r' || current[to - 1] == '\n')) --to;
      out.append(current, from, to - from);
      ++row;
    };
    for (std::size_t index = 0; index < current.size(); ++index) {
      if (current[index] == '\n') {
        append(begin, index);
        begin = index + 1;
      }
    }
    append(begin, current.size());
    return out;
  }

  // —— 动过了：按行级差异把当前行**映射回快照行** ——
  //
  // 为何不能继续按下标筛：`real` 表的行下标属于**快照**，而用户一按回车、
  // 或用控制通道整体重设文本，行数就变了——从那一行起全部错位：
  // 实测右栏只剩最后一行（前四行被当成填充行丢掉了），左栏则“四行全删”。
  //
  // 映射规则（与统一 diff 同构）：
  //  · 两侧保留的行（Context）→ 与快照一一对，沿用它的 `real` 位；
  //  · 只在新侧出现的行（Added）→ **用户新写的**，一律算真实内容；
  //  · 只在快照侧出现的行（Removed）→ 快照行没了，与当前行无关，跳过。
  const std::vector<std::string_view> snapshot = split_lines_of(side.text);
  const std::vector<std::string_view> lines = split_lines_of(current);
  const std::vector<text::DiffHunk> hunks = text::diff_hunks(side.text, current, 0);
  std::vector<char> keep(lines.size(), 1);   // 默认保留（未被任何块覆盖的行）
  std::size_t snapshot_at = 0;               // 下一个未消费的快照行（0 起）
  std::size_t line_at = 0;                   // 下一个未消费的当前行（0 起）
  const auto map_context = [&](std::size_t snapshot_upto, std::size_t line_upto) {
    while (snapshot_at < snapshot_upto && line_at < line_upto) {
      // 填充行随之被筛掉；真实行保留（默认就是 1，这里只负责与快照同步前进）。
      if (snapshot_at < side.real.size() && side.real[snapshot_at] == 0) keep[line_at] = 0;
      ++snapshot_at;
      ++line_at;
    }
  };
  for (const text::DiffHunk& hunk : hunks) {
    // `old_start/new_start` 是 1 起；0 行数的块（纯新增/纯删除）起点已在插入点，不再减 1。
    const std::size_t snapshot_begin =
        hunk.old_count == 0 ? hunk.old_start : hunk.old_start - 1;
    const std::size_t line_begin = hunk.new_count == 0 ? hunk.new_start : hunk.new_start - 1;
    map_context(std::min(snapshot_begin, snapshot.size()), std::min(line_begin, lines.size()));
    for (const text::DiffLine& line : hunk.lines) {
      if (line.kind == text::DiffLineKind::Removed) {
        if (snapshot_at < snapshot.size()) ++snapshot_at;
        continue;
      }
      // Context 行跟着快照走（快照耗完则无参照，保守地保留）；Added 行必然是真实内容。
      if (line.kind == text::DiffLineKind::Context && snapshot_at < side.real.size() &&
          side.real[snapshot_at] == 0) {
        keep[line_at] = 0;
      }
      if (snapshot_at < snapshot.size() && line.kind == text::DiffLineKind::Context) ++snapshot_at;
      if (line_at < lines.size()) ++line_at;
    }
  }
  std::string out;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (keep[index] == 0) continue;
    if (!out.empty()) out.push_back('\n');
    out.append(lines[index]);
  }
  return out;
}

void DiffView::begin_row(Side& side, std::size_t number, CodeEditor::LineMark mark,
                           std::string_view text, bool gutter_bar,
  std::vector<std::pair<std::size_t, std::size_t>> inline_ranges, bool real) {
  CodeEditor::LineDecoration decoration;
  decoration.line = side.lines + 1;   // 装饰按**1 起**行号指定
  decoration.mark = mark;
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
  side.real.push_back(real ? 1 : 0);
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
    begin_row(old_side_, CodeEditor::kNoNumber, CodeEditor::LineMark::None, header,
                  /*gutter_bar=*/false, {}, /*real=*/false);
    // 右栏只占一行高度、**不重复显示 `@@`**（两栏同行本已是同一块的分隔线：
    // 右栏再画一遍会让人以为“右侧也有一段这样的内容”）。
    begin_row(new_side_, CodeEditor::kNoNumber, CodeEditor::LineMark::None, {},
                  /*gutter_bar=*/false, {}, /*real=*/false);
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
                  show ? line->text : std::string_view{}, hidden,
                  show ? line->inline_old : std::vector<std::pair<std::size_t, std::size_t>>{},
                  /*real=*/show);
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
                  show ? line->text : std::string_view{}, hidden,
                  show ? line->inline_new : std::vector<std::pair<std::size_t, std::size_t>>{},
                  /*real=*/show);
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
    begin_row(old_side_, CodeEditor::kNoNumber, CodeEditor::LineMark::None, header,
                /*gutter_bar=*/false, {}, /*real=*/false);
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
                is_added ? line.inline_new : line.inline_old, /*real=*/true);
    }
  }
}

void DiffView::apply_editor_config() {
  const bool side_by_side = layout_ == DiffLayout::SideBySide;
  // 左栏可写的判据只算一次（两处都用）：内联模式左栏是**唯一**那一栏，它必须可写。
  const bool left_writable = allow_left_edit_;
  if (left_ != nullptr) {
    left_->set_language("diff");
    left_->set_show_line_numbers(show_line_numbers_);
    left_->set_read_only(read_only_ || !left_writable);
    if (font_size_ > 0.0f) left_->set_font_size(font_size_);
    // 滚动同步：两栏必须一起滚，且不能互相唤醒（见 `sync_scroll_from`）。
    // 接在这里而不是 `arrange`：后者是布局，重复接线在切布局时会把回调覆盖成同一个。
    left_->on_scroll = [this](float, float) { sync_scroll_from(*left_); };
    // 左栏也可写时（空白文本对比：两侧都是用户自己的内容）同样把“变了”交给宿主。
    // 传**逻辑文本**：对齐填充行不是内容，带出去会把整篇当成改动。
    //
    // 两件事同时做，顺序不能反：
    //   ① `commit_side_edit` 就地写快照 + 重算（同帧闭环，差异不再“慢一帧”）；
    //   ② 再通知宿主（宿主往往还要做别的事：标脏、同步 LSP…）。
    if (left_writable) {
      left_->on_change = [this](std::string_view /*text*/) {
        const std::string logical = logical_text(false);
        commit_side_edit(false, logical);
        if (on_left_text_edit) on_left_text_edit(logical);
      };
    } else {
      left_->on_change = nullptr;
    }
  }
  if (right_ != nullptr) {
    // 内联模式右栏不参与：给它空文本（它此时也 `set_visible(false)`）。
    right_->set_language("diff");
    right_->set_show_line_numbers(show_line_numbers_);
    // 可编辑的判据：**并列模式**且非只读。内联模式只有左栏在画，右栏不可见也不该可写。
    //
    // ⚠ 两侧都可写场景（空白文本对比）不能靠 `!allow_left_edit_` 反推：
    // 那个位只回答“左栏能不能写”，与右栏无关（当初就是在这里把右栏写死的）。
    right_->set_read_only(read_only_ || !side_by_side);
    if (font_size_ > 0.0f) right_->set_font_size(font_size_);
    right_->on_scroll = [this](float, float) { sync_scroll_from(*right_); };
    // **右栏可编辑**（只读关闭时）：它是「当前内容」那一侧。编辑后交给宿主重算差异
    //（宿主才知道“跟什么比”）。
    //
    // ⚠ 传的是**本组件自己算的逻辑文本**（剔掉了对齐填充行与 `@@` 分隔行），
    // 而不是 `right_->text()`：后者带着那些造出来的行，拿它去比会把整篇算成改动
    //（实测：自动对比一次之后 +39 −283，画面上一片红——那正是这个坑）。
    if (!read_only_ && side_by_side) {
      right_->on_change = [this](std::string_view /*text*/) {
        const std::string logical = logical_text(true);
        commit_side_edit(true, logical);
        if (on_text_edit) on_text_edit(logical);
      };
    } else {
      right_->on_change = nullptr;
    }
  }
}

void DiffView::push_to_editors() {
  const bool side_by_side = layout_ == DiffLayout::SideBySide;
  if (left_ != nullptr) {
    // **保留光标与滚动**：重算会把整栏文本重新合成（带对齐填充行），
    // 而用户正在改的地方必须待在眼皮底下。
    //
    // ⚠ 这里用 `replace_text_keep_state` 而不是 `set_text`：后者语义是“换了一份文档”
    // （光标归 0、滚动回顶、撤销栈清空）——逐键重算时就是“每敲一个字光标跳回行首”。
    //
    // ⚠ 必须把**两份 `real` 表**一起交上去：填充行的数量随重算变化，
    // 只看显示行号会把光标定位到别的行上（实测：跑到当前行倒数第二个字符之后）。
    left_->replace_text_keep_state(old_side_.text, pushed_real_left_, old_side_.real);
    pushed_real_left_ = old_side_.real;
    left_->set_line_decorations(old_side_.marks);
  }
  if (right_ != nullptr) {
    // 同左栏：保留光标与滚动（两份 `real` 表都交，见左栏那段说明）。
    // 内联模式下右栏被清空，它不可见也不可写，位置无所谓。
    right_->replace_text_keep_state(side_by_side ? new_side_.text : std::string{},
                                    pushed_real_right_,
                                    side_by_side ? new_side_.real : std::vector<char>{});
    pushed_real_right_ = side_by_side ? new_side_.real : std::vector<char>{};
    right_->set_line_decorations(side_by_side ? new_side_.marks
                                              : std::vector<CodeEditor::LineDecoration>{});
  }
  // 配置面（语言/行号/只读/字号/回调）每次推完都重算一次：
  // 文本变了可能改变“右栏是否参与”（切布局走的不是本函数）。
  apply_editor_config();
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
  // 导航要**先能算出非零偏移**。
  //
  // ⚠ `scroll_to_line` 内部会 `min(…, max_scroll_y())`，而 `max_scroll_y()` 依赖
  // **本组件当前排布**（`bounds_.height`）——构建期调本函数时它还是 0，
  // 于是 `scroll_y_` 被夹回 0：**静默不滚**（用户：“没有自动跳到第一处差异”）。
  // 行高就绪也不够：两个条件都要（行高决定「滚到哪里」，视口高决定「最多滚多少」）。
  if (!line_height_ready() || bounds_.height <= 0.0f) {
    pending_scroll_hunk_ = index;
    mark_dirty();
    return true;
  }
  const auto scroll_to = [this, index](CodeEditor& editor, bool new_side) {
    const std::size_t row = hunk_first_line(index, new_side);
    if (row != 0) editor.scroll_to_line(row);
  };
  if (left_ != nullptr) scroll_to(*left_, false);
  if (layout_ == DiffLayout::SideBySide && right_ != nullptr) scroll_to(*right_, true);
  if (on_hunk_change) on_hunk_change(index);
  mark_dirty();
  return true;
}

/// 行高是否真的量出来了（`scroll_to_line` 能不能算出非零偏移）。
///
/// 为何不能只看 `bounds().height > 0`：那是**本组件**的矩形，它非零不代表内部编辑器
/// 已经把行高算出来了（还没 `measure`/`arrange` 过时行高缓存是 0）。
auto DiffView::line_height_ready() const -> bool {
  if (left_ == nullptr) return false;
  const auto value = left_->get_property("line_height");
  if (!value.has_value()) return false;
  try {
    return std::stof(*value) > 0.0f;
  } catch (...) {
    return false;
  }
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
  const float header = show_header_ ? kHeaderHeight : 0.0f;
  const float height = constraints.max_height < kUnbounded ? constraints.max_height : header;
  measured_ = math::Size{constraints.max_width, height};
  (void)context;
}

void DiffView::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  const bool side_by_side = layout_ == DiffLayout::SideBySide;
  const float header_height = show_header_ ? kHeaderHeight : 0.0f;
  const float body_y = rect.y + header_height;
  const float body_height = std::max(0.0f, rect.height - header_height);
  const float half = side_by_side ? std::floor(rect.width * 0.5f) : rect.width;

  bool left_created = false;
  bool right_created = false;

  if (left_ == nullptr) {
    auto owned = std::make_unique<CodeEditor>();
    left_ = owned.get();
    left_->set_id("diff-left");
    add_child(std::move(owned));
    left_created = true;
  }
  if (side_by_side && right_ == nullptr) {
    auto owned = std::make_unique<CodeEditor>();
    right_ = owned.get();
    right_->set_id("diff-right");
    add_child(std::move(owned));
    right_created = true;
  }
  // 编辑器新建：统一走 `push_to_editors` 把文本/装饰/只读位/回调**一次配齐**。
  //
  // ⚠ 以前在 `arrange` 里自己写了一份（`set_read_only(true)` 写死等），
  // 与 `push_to_editors` 的口径分叉——后果是右栏永远是只读的：
  // 组件自己的 `editable` 属性报“可编辑”，实际那个编辑器却拒绕所有输入。
  // 两处都设“同一个量”就是这类缺陷的温床，所以只留一处。
  //
  // ⚠ **只在刚造出编辑器时推**：`arrange` 每帧都跑，而两栏文本的真值源是
  // `set_diff`/`set_full_texts` 重建出的快照。宿主“用户编辑 → 重算 → 下一帧
  // 重建”之间存在一个窗口，此时快照还是**上一帧**的；在 arrange 里无条件推
  // 就会把用户刚打的字**回退**掉，且光标被夹回上一版行尾——现象是“在行尾
  // 每输一个字光标就退回一个字符”（实测：编辑器 `abc1`、光标 4 → 快照 `abc`
  // 覆盖后光标夹到 3）。
  const bool created = left_created || right_created;
  if (created) push_to_editors();
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

  // —— 右缘缩略条（覆盖层，**不挤窄编辑器**）——
  //
  // 位置：**右栏内缘**（滚动条左侧一点点）。两个约束：
  //  ① 不能与右栏滚动条重叠——两者都是竖向拖动，抢同一个热区就是灾难；
  //  ② 不能被裁到编辑器之外。
  // 所以从右缘向左量“滚动条宽 + 间隙”。
  const float overview_w = show_overview_ ? std::min(overview_width_, rect.width * 0.08f) : 0.0f;
  const float track_h = std::max(0.0f, body_height - kOverviewInset * 2.0f);
  overview_track_ = math::Rect{rect.x + rect.width - overview_w - kOverviewScrollbarGap,
                               body_y + kOverviewInset, overview_w, track_h};
  overview_box_valid_ = overview_w > 0.0f && track_h > 0.0f;
  // 可见区框：由滚动偏移与内容总高反算。**行数 × 行高 = 内容高**
  //（左栏是整份文件，行高已就绪后才能算；否则框不成立）。
  const std::size_t total_lines = left_ != nullptr ? left_->line_count() : 0U;

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

  // —— 缩略条的**可见区框**（需要行高，所以排到这里）——
  //
  // 三个量各自的意义：
  //  内容总高 = 总行数 × 行高（整份文件）；
  //  可见高   = 视口高；
  //  框高     = 轨道高 × 可见高 / 内容总高（下限 12px，否则长文件下框小到无法拖）；
  //  框顶     = 轨道高 × 滚动偏移 / 内容总高。
  //
  // ⚠ 本组件**不知道**视口高，它得从子件的 `line_height` + 自身 body 高反推：
  // 视口高就是 body_height（编辑器排满整个 body）。
  if (overview_box_valid_ && line_height > 0.0f) {
    const float content_h = static_cast<float>(total_lines) * line_height;
    const float viewport_h = std::max(1.0f, body_height);
    if (content_h <= viewport_h + 1.0f) {
      // 内容装得下：整条都是可见区，缩略条没有信息量——照样画（见 paint），
      // 但框就是整条（不画“部分”）——调用方靠 `overview_box_` 判断可见比。
      overview_box_ = overview_track_;
    } else {
      const float ratio = viewport_h / content_h;
      const float box_h = std::max(kOverviewMinBox, overview_track_.height * ratio);
      const float max_scroll = std::max(1.0f, content_h - viewport_h);
      const float scrolled = left_ != nullptr ? std::min(left_->scroll_offset().y, max_scroll) : 0.0f;
      const float travel = overview_track_.height - box_h;
      const float box_y = overview_track_.y + (max_scroll > 0.0f ? travel * scrolled / max_scroll : 0.0f);
      overview_box_ = math::Rect{overview_track_.x, box_y, overview_track_.width, box_h};
    }
  } else {
    overview_box_valid_ = false;
  }

  // —— 首次定位到第一个差异（待办，见 `set_auto_focus_first_hunk`）——
  //
  // 必须在这里（行高已经量出来之后）：`scroll_to_line` 乘的就是它。
  // 一次性：消费后清标记，否则用户手动滚开之后每次重排都会被拉回去。
  //
  // ⚠ 不能 `return` 早退：下面要按**新的** `scroll_y` 重算分隔条矩形，
  // 早退会让分隔条停在旧位置的矩形上（看着像“分隔条与内容错开一屏”）。
  if (pending_scroll_hunk_ != kNoHunk && line_height > 0.0f &&
      pending_scroll_hunk_ < diff_.hunks.size()) {
    const std::size_t target = pending_scroll_hunk_;
    pending_scroll_hunk_ = kNoHunk;
    current_hunk_ = target;
    if (left_ != nullptr) {
      const std::size_t row = hunk_first_line(target, false);
      if (row != 0) left_->scroll_to_line(row);
    }
    if (side_by_side && right_ != nullptr) {
      const std::size_t row = hunk_first_line(target, true);
      if (row != 0) right_->scroll_to_line(row);
    }
    if (on_hunk_change) on_hunk_change(target);
  }

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
  const TextPort* port = context.text;
  if (show_header_) {
    canvas.fill_rect(math::Rect{bounds_.x, bounds_.y, bounds_.width, kHeaderHeight},
                     raster::Paint::solid(colors.surface_alt), 0.0f);
    if (port != nullptr) {
      const float size = context.theme.metrics().font_sm;
      const std::string label = summary();
      const float text_y = bounds_.y + layout_line(*port, label, size, kHeaderHeight).origin_y;
      port->draw(canvas, label, math::Point{bounds_.x + 8.0f, text_y}, size,
                 diff_.binary ? colors.text_muted : colors.text, text::FontRole::Proportional);
    }
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

  // —— 右缘缩略条（IDEA 同款）——
  //
  // 三层信息叠在一条 12px 宽的竖条上：
  //   底：轨道（极淡，让“整体范围”可见）；
  //   中：每个差异一行彩色短横（位置 = 比例映射，颜色区分增/删）；
  //   顶：可见区框（半透明主色底 + 描边，可拖）。
  //
  // 为何要画差异短横而不只是“位置框”：
  // 几千行的文件里，用户真正想知道的是“改动集中在哪一段”——
  // 这是**分布**信息，不是**位置**信息。两者正交，缺一个就不成立。
  if (show_overview_ && overview_box_valid_) {
    canvas.fill_rect(overview_track_, raster::Paint::solid(colors.surface_alt.with_alpha_f(0.55F)),
                     2.0f);
    const std::size_t total = left_ != nullptr ? left_->line_count() : 0U;
    if (total > 0U && !diff_.hunks.empty()) {
      const float span = static_cast<float>(total);
      // 短横**不占满轨道宽**（缩进 3px）：满宽会读成“分段式滚动条”，
      // 而缩进后的短横更像“标注”（IDEA 同）。
      const float inner_w = std::max(2.0f, overview_track_.width - 6.0f);
      const float inner_x = overview_track_.x + 3.0f;
      for (const text::DiffHunk& hunk : diff_.hunks) {
        // 块的纵向范围：用它的**新侧起止行**（并列视图的对齐行号）。
        // 起止相同时（单行改动）给一个最小高度，否则它会被压成 0 像素而看不见。
        const float begin = static_cast<float>(hunk.new_start == 0U ? 1U : hunk.new_start);
        const float end = begin + static_cast<float>(std::max<std::size_t>(hunk.new_count, 1U));
        const float y0 = overview_track_.y +
                         overview_track_.height * std::min(1.0f, (begin - 1.0f) / span);
        const float y1 = overview_track_.y + overview_track_.height * std::min(1.0f, end / span);
        const float mark_h = std::max(kOverviewMarkHeight, y1 - y0);
        // 颜色：本块含新增 → 主色（绿系语义）；含删除 → 危险色；两者都含 → 主色。
        bool has_add = false;
        bool has_del = false;
        for (const text::DiffLine& line : hunk.lines) {
          if (line.kind == text::DiffLineKind::Added) has_add = true;
          if (line.kind == text::DiffLineKind::Removed) has_del = true;
        }
        const math::Color mark_color =
            has_add ? colors.success : (has_del ? colors.danger : colors.text_muted);
        canvas.fill_rect(math::Rect{inner_x, y0, inner_w, mark_h},
                         raster::Paint::solid(mark_color.with_alpha_f(0.85F)), 1.0f);
      }
    }
    // 可见区框：半透明底 + 描边（拖动时它跟着动，是唯一的反馈）。
    canvas.fill_rect(overview_box_, raster::Paint::solid(colors.primary.with_alpha_f(0.16F)),
                     2.0f);
    const float bw = 1.0f;
    canvas.fill_rect(math::Rect{overview_box_.x, overview_box_.y, overview_box_.width, bw},
                     raster::Paint::solid(colors.primary.with_alpha_f(0.55F)), 0.0f);
    canvas.fill_rect(
        math::Rect{overview_box_.x, overview_box_.y + overview_box_.height - bw,
                   overview_box_.width, bw},
        raster::Paint::solid(colors.primary.with_alpha_f(0.55F)), 0.0f);
  }
}

auto DiffView::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  // —— 缩略条拖拽（优先于分隔条点击：它叠在最右侧，且意图更明确）——
  //
  // 两种手势：
  //   按在**框内** → 拖动（记住按点与框顶的差，拖起来不跳）；
  //   按在**轨道其他位置** → 跳转（把框中心接到按点）——这是滚动条的惯例。
  if (overview_box_valid_ && (event.kind == EventKind::MouseDown ||
                              event.kind == EventKind::MouseMove ||
                              event.kind == EventKind::MouseUp)) {
    const bool inside_track = overview_track_.contains(event.position);
    const bool inside_box = overview_box_.contains(event.position);
    if (event.kind == EventKind::MouseDown && inside_track) {
      overview_dragging_ = true;
      overview_grab_dy_ = inside_box ? event.position.y - overview_box_.y : overview_box_.height * 0.5F;
      if (!inside_box) {
        // 点轨道空白：直接把视口搬过去（不用先点击再拖，那是两步）。
        scroll_from_overview(event.position.y);
      }
      return true;
    }
    if (event.kind == EventKind::MouseMove && overview_dragging_) {
      scroll_from_overview(event.position.y);
      return true;
    }
    if (event.kind == EventKind::MouseUp && overview_dragging_) {
      overview_dragging_ = false;
      return true;
    }
  }
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

void DiffView::set_show_overview(bool value) {
  if (show_overview_ == value) return;
  show_overview_ = value;
  mark_layout_dirty();
}

void DiffView::set_overview_width(float value) {
  // 夹到合理范围：太窄看不见（且命中区抓不住），太宽就是在抢内容区。
  const float clamped = std::clamp(value, 4.0f, 40.0f);
  if (overview_width_ == clamped) return;
  overview_width_ = clamped;
  mark_layout_dirty();
}

void DiffView::scroll_from_overview(float pointer_y) {
  if (left_ == nullptr || !overview_box_valid_) return;
  const float line_height = [&] {
    const auto value = left_->get_property("line_height");
    if (!value.has_value()) return 0.0f;
    try {
      return std::stof(*value);
    } catch (...) {
      return 0.0f;
    }
  }();
  if (line_height <= 0.0f) return;
  const float content_h = static_cast<float>(left_->line_count()) * line_height;
  const float travel = overview_track_.height - overview_box_.height;
  if (travel <= 0.0f) return;
  // 框顶 = 指针 − 抓取偏移 → 换成比例 → 乘可滚范围。
  const float box_top = std::clamp(pointer_y - overview_grab_dy_, overview_track_.y,
                                   overview_track_.y + travel);
  const float ratio = (box_top - overview_track_.y) / travel;
  const float viewport_h = std::max(1.0f, bounds_.height - (show_header_ ? kHeaderHeight : 0.0f));
  const float max_scroll = std::max(0.0f, content_h - viewport_h);
  left_->set_scroll_offset(left_->scroll_offset().x, ratio * max_scroll);
  // 两栏滚动同步走**既有链路**（`sync_scroll_from` 是子件的滚动回调入口，
  // 这里直接调它会跳过 `syncing_` 保护——所以要复用它的语义：先同步另一栏，
  // 再发 `on_scroll`）。
  sync_scroll_from(*left_);
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
  // 编辑开关的**可观测面**：e2e 靠它断言“打开对比视图时右栏可改”。
  if (name == "editable") return read_only_ ? "false" : "true";
  if (name == "binary") return diff_.binary ? "true" : "false";
  if (name == "summary") return summary();
  if (name == "old_lines") return std::to_string(old_side_.lines);
  if (name == "new_lines") return std::to_string(new_side_.lines);
  if (name == "left_text") return left_ != nullptr ? left_->text() : std::string{};
  if (name == "right_text") return right_ != nullptr ? right_->text() : std::string{};
  // **逻辑文本**（剔掉对齐填充行与 `@@` 分隔行后的真实内容）——
  // 自动对比的输入就是这个，所以它必须是可观测的：
  // “自动对比把整篇都算成改了”这种缺陷，不把输入暴露出来就只能靠猜。
  if (name == "logical_left") return logical_text(false);
  if (name == "logical_right") return logical_text(true);
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
          "editable",   "binary",             "summary",    "old_lines", "new_lines",
          "logical_left", "logical_right",
          "numbers_old",
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

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
//
// 两件事都在**这个 TU 里**完成，因为它们互相依赖：
//  ① 定义登记器（`ST_UI_COMPONENT_IMPL_*`）——完整特化只能出现在这里；
//  ② 调 `import_components<T>()` 把它登记进注册表。
//
// ② 必须在**函数体**里：命名空间作用域下以模板实参开头的表达式会被当成声明
//（实测：`expected unqualified-id before 'void'`）。
ST_UI_COMPONENT_IMPL(::st::ui::DiffView);

namespace {
[[maybe_unused]] const bool st_ui_imported_diff_view_cpp = []() {
  st::ui::import_components<::st::ui::DiffView>();
  return true;
}();
}  // namespace
