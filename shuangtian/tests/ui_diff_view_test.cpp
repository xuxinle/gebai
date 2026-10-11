/// `ui::DiffView` 差异视图组件测试：两栏对齐、行号映射、hunk 导航、内联、禁用行、滚动同步。
///
/// 这个组件最容易错的地方都不会"报错"：两栏行数不等（后面全错位）、
/// 行号映射差一、分隔条与它标注的那一块错开。因此用例全部断言**结构量**
/// （显示行数、行号映射表、导航落点），而不只是"画出来了"。

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/text/diff.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/diff_view.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::DiffLayout;
using st::ui::DiffView;
using st::ui::RenderContext;
using st::ui::Theme;

struct Fixture {
  Theme theme{Theme::light()};
  std::shared_ptr<st::text::FontStack> stack{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<st::ui::TextPort> port{};
  DiffView view{};
  RenderContext context{theme, nullptr, 0.0};

  Fixture() {
    auto loaded = st::text::FontStack::system_default();
    if (loaded.has_value()) {
      stack = std::make_shared<st::text::FontStack>(std::move(*loaded));
      renderer = std::make_unique<st::text::TextRenderer>(*stack);
      port = st::app::make_text_port(*renderer);
      context.text = port.get();
    }
    view.set_id("diff");
  }

  [[nodiscard]] auto has_font() const noexcept -> bool { return port != nullptr; }

  void layout(float width = 900.0f, float height = 400.0f) {
    view.apply_theme(theme);
    view.measure(context, st::ui::Constraints{width, height});
    view.arrange(context, st::math::Rect{0.0f, 0.0f, width, height});
  }

  [[nodiscard]] auto property(std::string_view name) const -> std::string {
    return view.get_property(name).value_or(std::string{});
  }

  /// 行号映射表 → 数字数组（`numbers_old` / `numbers_new`）。
  [[nodiscard]] auto numbers(bool new_side) const -> std::vector<std::size_t> {
    const std::string raw = property(new_side ? "numbers_new" : "numbers_old");
    std::vector<std::size_t> out;
    std::size_t begin = 0;
    while (begin < raw.size()) {
      const std::size_t comma = raw.find(',', begin);
      const std::string piece = raw.substr(begin, comma == std::string::npos ? std::string::npos
                                                                            : comma - begin);
      if (!piece.empty()) out.push_back(static_cast<std::size_t>(std::stoull(piece)));
      if (comma == std::string::npos) break;
      begin = comma + 1;
    }
    return out;
  }
};

/// 一段「改中间一行 + 末尾加一行」的两版本文本。
constexpr std::string_view kOldText = "one\ntwo\nthree\nfour\nfive\n";
constexpr std::string_view kNewText = "one\nTWO\nthree\nfour\nfive\nsix\n";

}  // namespace

ST_TEST(diff_view_side_by_side_lines_are_aligned) {
  Fixture fx;
  fx.view.set_texts(kOldText, kNewText);
  fx.layout();
  // 两栏显示行数**必须相等**（占位行正是为此存在）；不等则从那一行起全部错位。
  ST_CHECK_EQ(fx.property("old_lines"), fx.property("new_lines"));
  ST_CHECK(!fx.property("old_lines").empty());
  const auto old_numbers = fx.numbers(false);
  const auto new_numbers = fx.numbers(true);
  ST_CHECK_EQ(old_numbers.size(), new_numbers.size());
}

ST_TEST(diff_view_changes_are_paired_on_the_same_row) {
  // 一段「删 2 行、加 1 行」：两栏占的行数必须由**配对**决定（不是各占各的），
  // 否则从这处改动起两栏会整体错位——而错位的界面“看着仍然很正常”。
  Fixture fx;
  fx.view.set_texts("a\nb\nc\nd\n", "a\nX\nY\nd\n");
  fx.layout(900.0f, 300.0f);
  const auto rows = fx.numbers(false).size();
  ST_CHECK(rows > 0U);
  // 显示行 = 分隔行(1) + 上下文 a(1) + 改动段 2 行 + 上下文 d(1) = 5。
  ST_CHECK_EQ(fx.property("old_lines"), std::string("5"));
  ST_CHECK_EQ(fx.property("new_lines"), std::string("5"));
  // 改动段里两行都是“左旧右新”同时出现（第一行都有内容，第二行右侧是填充）。
  const auto left = fx.property("left_text");
  const auto right = fx.property("right_text");
  ST_CHECK(left.find("b\nc") != std::string::npos);
  ST_CHECK(right.find("X\nY") != std::string::npos);
}

ST_TEST(diff_view_line_numbers_map_to_both_sides) {
  Fixture fx;
  fx.view.set_texts(kOldText, kNewText);
  fx.layout();
  const auto old_numbers = fx.numbers(false);
  const auto new_numbers = fx.numbers(true);
  constexpr std::size_t kNoNumber = static_cast<std::size_t>(-1);
  ST_REQUIRE(!old_numbers.empty());
  // 第一行是分隔条：**两侧都不给行号**（它不属于任何一侧）。
  ST_CHECK_EQ(old_numbers[0], kNoNumber);
  ST_CHECK_EQ(new_numbers[0], kNoNumber);
  // 其下第一行是上下文（两侧都是 1）。
  ST_CHECK_EQ(old_numbers[1], std::size_t{1});
  ST_CHECK_EQ(new_numbers[1], std::size_t{1});
  // 「two → TWO」是**同一显示行上的两个格**（成对交错）。
  // 这一条是并列视图的命门：不做配对会让两栏从第一处改动起整体错位，
  // 而错位的界面“看起来仍然很正常”（实测第一次落地就是这样）。
  bool saw_pair = false;
  for (std::size_t i = 0; i < old_numbers.size(); ++i) {
    if (old_numbers[i] == 2 && new_numbers[i] == 2) saw_pair = true;
  }
  ST_CHECK(saw_pair);
  // 两侧的号在上下文行上**一致**（这是「同一段文本的两次呈现」的判据）。
  std::size_t context_rows = 0;
  for (std::size_t i = 0; i < old_numbers.size(); ++i) {
    if (old_numbers[i] != 0 && old_numbers[i] != kNoNumber &&
        old_numbers[i] == new_numbers[i]) {
      ++context_rows;
    }
  }
  ST_CHECK(context_rows >= 4U);   // one/three/four/five
}

ST_TEST(diff_view_hunk_navigation_wraps_and_reports_position) {
  Fixture fx;
  // 两处相隔很远的改动 → 两个 hunk。
  std::string old_text;
  std::string new_text;
  for (int i = 0; i < 40; ++i) {
    const std::string line = "line" + std::to_string(i) + "\n";
    old_text += line;
    new_text += (i == 2 || i == 30) ? "line" + std::to_string(i) + "x\n" : line;
  }
  fx.view.set_texts(old_text, new_text);
  fx.layout(900.0f, 300.0f);
  ST_CHECK_EQ(fx.property("hunks"), std::string("2"));
  ST_CHECK_EQ(fx.property("current_hunk"), std::string("0"));
  const std::size_t first_line = static_cast<std::size_t>(std::stoull(fx.property("current_hunk_line")));
  ST_CHECK(first_line > 1U);
  // 下一个 → 1；再下一个 → 环绕回 0。
  ST_CHECK(fx.view.invoke_action("next_hunk", {}));
  ST_CHECK_EQ(fx.property("current_hunk"), std::string("1"));
  const std::size_t second_line =
      static_cast<std::size_t>(std::stoull(fx.property("current_hunk_line")));
  ST_CHECK(second_line > first_line);
  ST_CHECK(fx.view.invoke_action("next_hunk", {}));
  ST_CHECK_EQ(fx.property("current_hunk"), std::string("0"));
  // 上一个 → 环绕到最后一个。
  ST_CHECK(fx.view.invoke_action("previous_hunk", {}));
  ST_CHECK_EQ(fx.property("current_hunk"), std::string("1"));
  // 越界的 goto 如实失败（不假装成功）。
  ST_CHECK(!fx.view.invoke_action("goto_hunk", "9"));
  ST_CHECK(fx.view.invoke_action("goto_hunk", "1"));
}

ST_TEST(diff_view_unified_shows_both_sides_in_one_column) {
  Fixture fx;
  fx.view.set_texts(kOldText, kNewText);
  fx.view.set_layout(DiffLayout::Unified);
  fx.layout();
  ST_CHECK_EQ(fx.property("layout"), std::string("unified"));
  const std::string left = fx.property("left_text");
  // 内联：删除行与新增行都出现。
  ST_CHECK(left.find("-two") != std::string::npos || left.find("two") != std::string::npos);
  ST_CHECK(left.find("TWO") != std::string::npos);
  ST_CHECK(left.find("six") != std::string::npos);
  // 内联没有占位行：显示行数 < 并列两栏的行数之和。
  ST_CHECK_EQ(fx.property("new_lines"), std::string("0"));
  // 行号：内联按"这一行属于哪一侧"给号（新增行给新侧号、删除行给旧侧号）。
  const auto numbers = fx.numbers(false);
  constexpr std::size_t kNoNumber = static_cast<std::size_t>(-1);
  ST_REQUIRE(numbers.size() >= 3U);
  ST_CHECK_EQ(numbers[0], kNoNumber);   // 分隔条不给号
  // 切回并列：右栏文本恢复且两栏等长。
  fx.view.set_layout(DiffLayout::SideBySide);
  fx.layout();
  ST_CHECK_EQ(fx.property("old_lines"), fx.property("new_lines"));
  ST_CHECK(fx.property("right_text").find("six") != std::string::npos);
}

ST_TEST(diff_view_disabled_lines_keep_alignment) {
  Fixture fx;
  fx.view.set_texts(kOldText, kNewText);
  fx.layout();
  const std::string aligned = fx.property("old_lines");
  // 禁用被改的那一行（旧侧 2 / 新侧 2）：两侧都不再出现它，但**行数仍必须相等**。
  fx.view.set_disabled_lines({2}, {2});
  ST_CHECK_EQ(fx.property("old_lines"), fx.property("new_lines"));
  ST_CHECK_EQ(fx.property("disabled_old_lines"), std::string("2"));
  ST_CHECK_EQ(fx.property("disabled_new_lines"), std::string("2"));
  const auto old_numbers = fx.numbers(false);
  constexpr std::size_t kNoNumber = static_cast<std::size_t>(-1);
  bool has_two = false;
  for (const std::size_t number : old_numbers) {
    if (number == 2U) has_two = true;
  }
  ST_CHECK(!has_two);   // 被禁用的行不再占行号
  ST_CHECK(!old_numbers.empty());
  ST_CHECK_EQ(old_numbers.front(), kNoNumber);
  // 禁用"未改动"的行同样保持对齐（它两侧都消失，行号也不出现）。
  fx.view.set_disabled_lines({3}, {3});
  ST_CHECK_EQ(fx.property("old_lines"), fx.property("new_lines"));
  (void)aligned;
}

ST_TEST(diff_view_scroll_syncs_between_panes) {
  Fixture fx;
  if (!fx.has_font()) return;
  std::string old_text;
  std::string new_text;
  for (int i = 0; i < 200; ++i) {
    old_text += "line" + std::to_string(i) + "\n";
    new_text += i == 100 ? "line" + std::to_string(i) + "x\n" : "line" + std::to_string(i) + "\n";
  }
  fx.view.set_texts(old_text, new_text);
  fx.layout(900.0f, 200.0f);
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(900, 200, 1.0f);
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.view.paint(fx.context, canvas);

  st::ui::CodeEditor* left = fx.view.left_editor();
  st::ui::CodeEditor* right = fx.view.right_editor();
  ST_REQUIRE(left != nullptr);
  ST_REQUIRE(right != nullptr);
  // 滚左栏 → 右栏跟随（两栏内容等行，纵向必须一起走）。
  ST_CHECK(fx.view.invoke_action("goto_hunk", "0"));
  const float y = left->scroll_offset().y;
  ST_CHECK(y > 0.0f);
  ST_CHECK(std::abs(right->scroll_offset().y - y) < 1.0f);
  // 属性面写入也走同一条路（自动化用它驱动）。
  ST_CHECK(fx.view.set_property("scroll", "120"));
  ST_CHECK(std::abs(left->scroll_offset().y - 120.0f) < 1.0f);
  ST_CHECK(std::abs(right->scroll_offset().y - 120.0f) < 1.0f);
}

ST_TEST(diff_view_separator_follows_scroll) {
  Fixture fx;
  if (!fx.has_font()) return;
  std::string old_text;
  std::string new_text;
  for (int i = 0; i < 60; ++i) {
    old_text += "line" + std::to_string(i) + "\n";
    new_text += i == 5 ? "line" + std::to_string(i) + "x\n" : "line" + std::to_string(i) + "\n";
  }
  fx.view.set_texts(old_text, new_text);
  fx.layout(900.0f, 300.0f);
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(900, 300, 1.0f);
  canvas.clear(st::math::Color::rgb(255, 255, 255));
  fx.view.paint(fx.context, canvas);
  // 分隔条矩形是**排布时按行高算的**（首帧 paint 才填行高缓存，故重排一次）。
  fx.layout(900.0f, 300.0f);
  const st::math::Rect before = fx.view.separator_rect(0);
  ST_CHECK(!before.is_empty());
  // 滚下去之后分隔条跟着上移（它是内容的一部分，不是固定头部）。
  fx.view.set_property("scroll", "80");
  fx.layout(900.0f, 300.0f);
  const st::math::Rect after = fx.view.separator_rect(0);
  ST_CHECK(after.y < before.y);
}

ST_TEST(diff_view_binary_and_empty_states_are_explicit) {
  Fixture fx;
  st::text::FileDiff binary;
  binary.old_path = "logo.png";
  binary.new_path = "logo.png";
  binary.binary = true;
  binary.additions = 0;
  binary.deletions = 0;
  fx.view.set_diff(binary);
  fx.layout();
  ST_CHECK_EQ(fx.property("binary"), std::string("true"));
  // 「二进制」不等于「没有差异」：摘要必须如实说明，而不是留白。
  ST_CHECK(fx.property("summary").find("二进制") != std::string::npos);
  ST_CHECK_EQ(fx.property("hunks"), std::string("0"));
  // 无差异：导航不崩、不假报成功。
  fx.view.clear();
  ST_CHECK_EQ(fx.property("hunks"), std::string("0"));
  ST_CHECK_EQ(fx.property("current_hunk"), std::string("-1"));
  ST_CHECK(!fx.view.invoke_action("next_hunk", {}));
  ST_CHECK(fx.property("summary").find("无差异") != std::string::npos);
}

ST_TEST(diff_view_summary_covers_rename_and_added) {
  Fixture fx;
  st::text::FileDiff renamed;
  renamed.old_path = "old_name.cpp";
  renamed.new_path = "new_name.cpp";
  renamed.renamed = true;
  renamed.additions = 3;
  renamed.deletions = 1;
  fx.view.set_diff(renamed);
  const std::string summary = fx.property("summary");
  ST_CHECK(summary.find("new_name.cpp") != std::string::npos);
  ST_CHECK(summary.find("重命名自 old_name.cpp") != std::string::npos);
  ST_CHECK(summary.find("+3") != std::string::npos);
  ST_CHECK(summary.find("−1") != std::string::npos);
}

ST_TEST(diff_view_passes_inline_ranges_to_editors) {
  // 行内字符级差异要**从数据走到视图**：`make_file_diff` 算出的 `inline_old`/`inline_new`
  // 必须出现在两侧编辑器的行装饰上，否则「改一个词」在界面上仍是整行高亮
  //（数据层算对了、界面层没接上，是这类功能最常见的“做完了却看不见”）。
  Fixture fx;
  // 只改一个词：`two` → `TWO`。
  fx.view.set_texts(kOldText, kNewText);
  fx.layout();
  st::ui::CodeEditor* left = fx.view.left_editor();
  st::ui::CodeEditor* right = fx.view.right_editor();
  ST_REQUIRE(left != nullptr);
  ST_REQUIRE(right != nullptr);

  std::size_t old_inline = 0;
  for (const auto& mark : left->line_decorations()) {
    if (!mark.inline_ranges.empty()) ++old_inline;
  }
  std::size_t new_inline = 0;
  for (const auto& mark : right->line_decorations()) {
    if (!mark.inline_ranges.empty()) ++new_inline;
  }
  ST_CHECK(old_inline >= 1U);
  ST_CHECK(new_inline >= 1U);

  // 区间必须**在那一行的文本范围内**（越界说明坐标系错了——字节 vs 字符列混用
  // 会算出超过行长度的偏移，而绘制时会被静默跳过，看不见但也不报错）。
  const std::string left_text = left->text();
  for (const auto& mark : left->line_decorations()) {
    for (const auto& [from, to] : mark.inline_ranges) {
      ST_CHECK(from < to);
      ST_CHECK(to <= left_text.size() + 1U);
    }
  }
  // 上下文行不该带行内区间（`two`/`TWO` 那一行才是改行）。
  for (const auto& mark : left->line_decorations()) {
    if (mark.mark == st::ui::CodeEditor::LineMark::None && mark.number <= 5U) {
      ST_CHECK(mark.inline_ranges.empty());
    }
  }
}
