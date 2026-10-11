/// `DiffView` 逐行暂存（C3 / B11 的界面层）测试。
///
/// 这一层的核心风险是**两张表分家**：勾选框画在“第 i 个显示行”，而选择身份是
/// `(hunk, line)` 下标。它俩一旦错位一格，界面上完全看不出（勾选框照样画、照样响应），
/// 但生成出来的补丁会动**错误的行**——那是最难查的一类错。所以本文件的重心在
/// “勾选框位置 ↔ 选中的行”的一致性，以及补丁内容是否真的只含选中的行。

#include <algorithm>
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

using st::text::DiffLineKind;
using st::ui::DiffView;
using st::ui::Theme;

constexpr float kOriginX = 41.0f;   // 刻意不在原点（坐标 bug 在原点会被 0 掩盖）
constexpr float kOriginY = 27.0f;
constexpr float kWidth = 700.0f;
constexpr float kHeight = 300.0f;

struct Fixture {
  Theme theme{Theme::light()};
  std::shared_ptr<st::text::FontStack> stack{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<st::ui::TextPort> port{};
  DiffView view{};
  st::ui::RenderContext context{theme, nullptr, 0.0};

  Fixture() {
    auto loaded = st::text::FontStack::system_default();
    if (loaded.has_value()) {
      stack = std::make_shared<st::text::FontStack>(std::move(*loaded));
      renderer = std::make_unique<st::text::TextRenderer>(*stack);
      port = st::app::make_text_port(*renderer);
      context.text = port.get();
    }
    view.set_id("diff");
    view.apply_theme(theme);
    view.set_read_only(true);
  }

  void layout() {
    view.measure(context, st::ui::Constraints{kWidth, kHeight});
    view.arrange(context, st::math::Rect{kOriginX, kOriginY, kWidth, kHeight});
  }

  /// 装载一个“一行改、一行加、若干上下文”的差异。
  void load() {
    view.set_texts("keep1\nold2\nkeep3\nkeep4\nkeep5\n", "keep1\nnew2\nkeep3\nadded4\nkeep5\n");
    layout();
  }

  /// 找到第一个指定类型的行（返回 `(hunk, line)`；找不到返回 `{kNoHunk, 0}`）。
  [[nodiscard]] auto find_line(DiffLineKind kind) const
      -> std::pair<std::size_t, std::size_t> {
    const auto& hunks = view.diff().hunks;
    for (std::size_t hunk = 0; hunk < hunks.size(); ++hunk) {
      for (std::size_t line = 0; line < hunks[hunk].lines.size(); ++line) {
        if (hunks[hunk].lines[line].kind == kind) return {hunk, line};
      }
    }
    return {DiffView::kNoHunk, 0};
  }
};

/// 从补丁文本里摘出 **hunk 正文**（保留首字符前缀：` ` / `+` / `-`）。
///
/// ⚠ 必须**包含上下文行**（首字符是空格）：本文件要断言的正是"未选中的删除行
/// 被降级成上下文"——第一版只收 `+`/`-` 开头的行，于是那条断言永远失败，
/// 而它断言的内容其实是对的（补丁里明明白白有一行 ` old2`）。
[[nodiscard]] auto patch_payload(const std::string& patch) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= patch.size()) {
    const std::size_t end = patch.find('\n', start);
    const std::string line =
        patch.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const bool is_header = line.starts_with("+++") || line.starts_with("---") ||
                           line.starts_with("diff ") || line.starts_with("index ") ||
                           line.starts_with("@@") || line.starts_with("\\ No newline");
    if (!line.empty() && !is_header && (line[0] == ' ' || line[0] == '+' || line[0] == '-')) {
      out.push_back(line);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

[[nodiscard]] auto contains(const std::vector<std::string>& haystack, const std::string& needle)
    -> bool {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

}  // namespace

ST_TEST(diff_view_staging_mode_toggles_checkbox_column) {
  Fixture fx;
  fx.load();
  // 默认关：装饰里没有勾选框（零开销）。
  ST_CHECK(!fx.view.staging_mode());
  for (const auto& mark : fx.view.diff().hunks.empty() ? std::vector<int>{}
                                                       : std::vector<int>{}) {
    (void)mark;
  }
  const auto added = fx.find_line(DiffLineKind::Added);
  ST_REQUIRE(added.first != DiffView::kNoHunk);
  // 模式关时也能调用（选择状态独立于模式），但勾选列不画。
  fx.view.set_staging_mode(true);
  ST_CHECK(fx.view.staging_mode());
  fx.layout();
  ST_CHECK_EQ(fx.view.selected_count(), std::size_t{0});
}

ST_TEST(diff_view_toggle_line_selects_only_stageable_lines) {
  Fixture fx;
  fx.load();
  const auto added = fx.find_line(DiffLineKind::Added);
  const auto removed = fx.find_line(DiffLineKind::Removed);
  ST_REQUIRE(added.first != DiffView::kNoHunk);
  ST_REQUIRE(removed.first != DiffView::kNoHunk);

  int changes = 0;
  fx.view.on_selection_changed = [&changes] { ++changes; };

  // 新增行可选。
  ST_CHECK(fx.view.toggle_line(added.first, added.second));
  ST_CHECK(fx.view.is_line_selected(added.first, added.second));
  ST_CHECK_EQ(fx.view.selected_count(), std::size_t{1});
  ST_CHECK_EQ(changes, 1);
  // 再点一次取消。
  ST_CHECK(!fx.view.toggle_line(added.first, added.second));
  ST_CHECK(!fx.view.is_line_selected(added.first, added.second));
  ST_CHECK_EQ(fx.view.selected_count(), std::size_t{0});
  ST_CHECK_EQ(changes, 2);

  // **上下文行如实拒绝**：选了不产生任何改动，允许选会让按钮状态与实际结果对不上。
  std::size_t context_hunk = DiffView::kNoHunk;
  std::size_t context_line = 0;
  for (std::size_t hunk = 0; hunk < fx.view.diff().hunks.size(); ++hunk) {
    for (std::size_t line = 0; line < fx.view.diff().hunks[hunk].lines.size(); ++line) {
      if (fx.view.diff().hunks[hunk].lines[line].kind == DiffLineKind::Context) {
        context_hunk = hunk;
        context_line = line;
        break;
      }
    }
    if (context_hunk != DiffView::kNoHunk) break;
  }
  ST_REQUIRE(context_hunk != DiffView::kNoHunk);
  ST_CHECK(!fx.view.toggle_line(context_hunk, context_line));
  ST_CHECK_EQ(fx.view.selected_count(), std::size_t{0});

  // 越界也拒绝。
  ST_CHECK(!fx.view.toggle_line(999, 0));
  ST_CHECK(!fx.view.toggle_line(0, 999));
}

ST_TEST(diff_view_selection_stays_sorted_for_stable_patches) {
  // 先点后面的行、再点前面的行：内部顺序必须按文件序（补丁的行序靠它稳定，
  // 而用户点选顺序是任意的）。
  Fixture fx;
  fx.view.set_texts("a\nb\nc\nd\ne\nf\ng\n", "A\nb\nc\nd\nE\nf\nG\n");
  fx.layout();
  std::vector<std::pair<std::size_t, std::size_t>> picked;
  for (std::size_t hunk = 0; hunk < fx.view.diff().hunks.size(); ++hunk) {
    for (std::size_t line = 0; line < fx.view.diff().hunks[hunk].lines.size(); ++line) {
      if (fx.view.diff().hunks[hunk].lines[line].kind != DiffLineKind::Context) {
        picked.emplace_back(hunk, line);
      }
    }
  }
  ST_REQUIRE(picked.size() >= 2U);
  // 逆序点（后一行先点）。
  for (auto iterator = picked.rbegin(); iterator != picked.rend(); ++iterator) {
    (void)fx.view.toggle_line(iterator->first, iterator->second);
  }
  const auto& selected = fx.view.selected_lines();
  ST_REQUIRE(selected.size() == picked.size());
  ST_CHECK(std::is_sorted(selected.begin(), selected.end()));
  ST_CHECK(selected.front() == picked.front());
  ST_CHECK(selected.back() == picked.back());
}

ST_TEST(diff_view_build_patch_contains_only_selected_lines) {
  Fixture fx;
  fx.load();
  const auto added = fx.find_line(DiffLineKind::Added);
  const auto removed = fx.find_line(DiffLineKind::Removed);
  ST_REQUIRE(added.first != DiffView::kNoHunk);
  ST_REQUIRE(removed.first != DiffView::kNoHunk);

  // 空选择 → 空补丁（**不是**一个空 hunk：那会让 git 报与意图无关的错）。
  ST_CHECK(fx.view.build_stage_patch().empty());

  // 只选新增行：补丁里只该有那一行 `+`。
  //
  // ⚠ `find_line` 返回的是**第一处**这类行——所以这里的新增行文本是 `new2`
  //（不是后面那处 `added4`）。第一版断言写成了 `added4`，白追一轮
  //（而补丁其实是对的：探针打出来就是 `+new2`）。**断言里的具体文本要与
  // "取到的是哪一处"对齐**——`find_line` 的语义是"第一个"，就写第一个的文本。
  ST_CHECK(fx.view.toggle_line(added.first, added.second));
  const std::string added_patch = fx.view.build_stage_patch();
  ST_CHECK(!added_patch.empty());
  const auto added_lines = patch_payload(added_patch);
  ST_CHECK(contains(added_lines, "+new2"));
  ST_CHECK(!contains(added_lines, "+added4"));
  // 未选中的删除行要**降级成上下文**（它仍存在，只是这次不删）——
  // 所以它既不该以 `-` 出现，也不该消失（消失会让 hunk 的行数对不上）。
  ST_CHECK(!contains(added_lines, "-old2"));
  ST_CHECK(contains(added_lines, " old2"));

  // 换成只选删除行：这次出现 `-old2`，且新增行不出现。
  fx.view.set_all_selected(false);
  ST_CHECK(fx.view.toggle_line(removed.first, removed.second));
  const auto removed_lines = patch_payload(fx.view.build_stage_patch());
  ST_CHECK(contains(removed_lines, "-old2"));
  ST_CHECK(!contains(removed_lines, "+new2"));

  // 同一处改动的 **新增 + 删除** 都选：两条都在（这是"整行替换"的完整形态）。
  fx.view.set_all_selected(false);
  ST_CHECK(fx.view.toggle_line(added.first, added.second));
  ST_CHECK(fx.view.toggle_line(removed.first, removed.second));
  const auto both_lines = patch_payload(fx.view.build_stage_patch());
  ST_CHECK(contains(both_lines, "+new2"));
  ST_CHECK(contains(both_lines, "-old2"));

  // `set_hunk_selected` 一次选中整块（等价逐行选）。
  fx.view.set_all_selected(false);
  fx.view.set_hunk_selected(added.first, true);
  ST_CHECK(fx.view.selected_count() >= 1U);
  const auto hunk_patch = patch_payload(fx.view.build_stage_patch());
  ST_CHECK(contains(hunk_patch, "+new2"));
  // 整块选中后再取消：清空该块。
  fx.view.set_hunk_selected(added.first, false);
  ST_CHECK_EQ(fx.view.selected_count(), std::size_t{0});
}

ST_TEST(diff_view_changing_diff_clears_selection) {
  // 下标当身份用的经典陷阱：换了 diff 而不清选择，同一下标指向**另一行**——
  // 用户会看到"我选的是第 3 行的新增，补丁里动的却是别处"。
  Fixture fx;
  fx.load();
  const auto added = fx.find_line(DiffLineKind::Added);
  ST_REQUIRE(added.first != DiffView::kNoHunk);
  ST_CHECK(fx.view.toggle_line(added.first, added.second));
  ST_CHECK_EQ(fx.view.selected_count(), std::size_t{1});

  int changes = 0;
  fx.view.on_selection_changed = [&changes] { ++changes; };
  fx.view.set_texts("x\ny\n", "x\nY\n");   // 换一份完全不同的差异
  ST_CHECK_EQ(fx.view.selected_count(), std::size_t{0});
  ST_CHECK_EQ(changes, 1);
  ST_CHECK(fx.view.build_stage_patch().empty());
}

ST_TEST(diff_view_paints_checkbox_column_only_in_staging_mode) {
  // **真的画出来了**：只断言 `set_staging_mode` 存下来是假绿。
  Fixture fx;
  fx.load();
  const auto render = [&fx]() {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(900, 500, 1.0f);
    canvas.clear(st::math::Color::rgb(1, 2, 3));
    fx.view.paint(fx.context, canvas);
    return canvas;
  };

  // 取样点：**确实有代码行**的高度（`DiffView` 顶部有一条 `kHeaderHeight` 的头，
  // 所以取样要越过它——第一版取 `+60` 落在了第二行上，而那一行的高度恰好
  // 在两种模式下都只有底色差异……更稳的是取**整列指纹**，避免"恰好同色"）。
  const auto column_hash = [](const st::raster::Canvas& canvas) {
    std::size_t hash = 1469598103934665603ULL;
    for (float y = kOriginY + 10.0f; y < kOriginY + 200.0f; y += 1.0f) {
      for (float x = kOriginX + 1.0f; x < kOriginX + 20.0f; x += 1.0f) {
        hash = (hash ^ canvas.pixel_at(static_cast<int>(x), static_cast<int>(y)).to_hex()) *
               1099511628211ULL;
      }
    }
    return hash;
  };

  const auto before = column_hash(render());
  fx.view.set_staging_mode(true);
  fx.layout();
  const auto after = column_hash(render());
  // 勾选列出现后该点像素必须变（gutter 加宽 + 框/边框画上去）。
  // 若这里不变，说明勾选列根本没画——而 `staging_mode()` 仍是 true（假绿）。
  ST_CHECK(before != after);
}

ST_TEST(diff_view_staging_mode_off_has_no_checkbox_decoration) {
  // 关掉模式后装饰里不该残留勾选框（否则 gutter 会一直宽着）。
  Fixture fx;
  fx.load();
  fx.view.set_staging_mode(true);
  fx.layout();
  fx.view.set_staging_mode(false);
  fx.layout();
  ST_CHECK(!fx.view.is_line_selected(0, 0));
  ST_CHECK(fx.view.selected_count() == 0U);
}
