/// `Tree` 视口剔除与可见行区间的回归护栏。
///
/// ## 这组用例守的是什么
///
/// `Tree` 是**自绘**组件：行不是子 `Element`，而是 `std::vector<Row>` 扁平数组。它因此
/// **不经过** `Element::paint` 的视口剔除（那只覆盖子元素树）——`paint_content` 过去无条件
/// 遍历全部行。实测（`tools/ui_tree_rows_bench.cpp`，400px 视口只容纳约 15 行）：
///
/// | 行数 | 改前每帧 | 改后每帧 |
/// |---|---|---|
/// | 1000 | 5.36 ms | 0.24 ms |
/// | 2000 | 10.74 ms | 0.24 ms |
/// | 5000 | 41.25 ms | 0.24 ms |
///
/// 改前**完美线性**（视口里只显示十几行，却为全部行付账）；`DESIGN.md §11` 那条
/// 「`tree`（1000 节点）< 5 ms」的目标缺口正在这里。
///
/// ## 为什么断言"哪些行被绘制"而不是耗时
///
/// 耗时是**机器相关的**（同一台机器重编都会漂移，见 `docs/BACKLOG.md` 里 bench 余量踩线
/// 那条），而"某行有没有被画"是结构性的、可复现的。且剔除错了的表现是"某行不见了"
/// ——这类问题在截图里很难发现（本项目跨运行像素不确定，逐像素 A/B 不可用，理由见
/// `tests/ui_culling_test.cpp` 开头）。所以这里用 `FixedAdvanceTextPort::drawn()` 直接数
/// 画过的标签。

#include "st/test/test.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/ui/components/tree.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::math::Rect;
using st::raster::Canvas;
using st::test::FixedAdvanceTextPort;
using st::ui::Constraints;
using st::ui::Event;
using st::ui::RenderContext;
using st::ui::Theme;
using st::ui::TreeNode;
using st::ui::Tree;

inline constexpr int kViewportWidth = 260;
inline constexpr int kViewportHeight = 400;
inline constexpr float kRowHeight = 26.0F;

/// 造 `count` 行（标签可定位到行号，便于断言"画了哪几行"）。
[[nodiscard]] auto rows_of(std::size_t count) -> std::vector<TreeNode> {
  std::vector<TreeNode> rows;
  rows.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    rows.push_back(TreeNode{.key = "k" + std::to_string(index),
                            .label = "row" + std::to_string(index),
                            .is_dir = false,
                            .depth = 0});
  }
  return rows;
}

/// 绘制夹具：真实 `Canvas` + 定宽文本桩（记录画过的标签）。
///
/// `drawn()` 只给 const 引用（共享 fixture 不提供 clear），所以取差集而不是清空。
struct PaintFixture {
  Theme theme{Theme::light()};
  FixedAdvanceTextPort port{};
  Canvas canvas{kViewportWidth, kViewportHeight};

  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }

  /// 布局到 `bounds`，绘制一次，返回**本次新画的标签**。
  [[nodiscard]] auto paint_at(Tree& tree, Rect bounds) -> std::vector<std::string> {
    tree.measure(context(), Constraints{});
    tree.arrange(context(), bounds);
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    const std::size_t before = port.drawn().size();
    tree.paint(context(), canvas);
    const auto& all = port.drawn();
    return {all.begin() + static_cast<std::ptrdiff_t>(before), all.end()};
  }

  /// 常规情形：视口矩形、从内容顶部开始。
  [[nodiscard]] auto paint_at_origin(Tree& tree) -> std::vector<std::string> {
    return paint_at(tree, Rect{0.0F, 0.0F, static_cast<float>(kViewportWidth),
                               static_cast<float>(kViewportHeight)});
  }
};

}  // namespace

/// 视口只容十几行时，**只画视口内的行**（不是全部 1000 行）。
ST_TEST(tree_paint_draws_only_rows_inside_viewport) {
  PaintFixture fixture;
  Tree tree;
  tree.sync_nodes(rows_of(1000));

  const std::vector<std::string> drawn = fixture.paint_at_origin(tree);

  ST_CHECK(!drawn.empty());
  ST_CHECK(drawn.size() <= 20U);  // 关键：**不是** 1000
  ST_CHECK(drawn.size() >= 14U);  // 且不能少画到"屏幕中间有洞"
  ST_CHECK_EQ(drawn.front(), std::string("row0"));
  ST_CHECK(drawn.back() != std::string("row999"));
}

/// 行数从 100 涨到 4000，**每帧绘制量不跟着涨**——剔除的结构性证据。
///
/// 这条是本组用例的核心：它直接对应 `ui_tree_rows_bench` 量出的那条曲线
/// （改前 1000/2000/5000 行的 paint 是 5.36/10.74/41.25 ms，完美线性）。
ST_TEST(tree_paint_cost_is_independent_of_total_row_count) {
  const auto painted_for = [](std::size_t rows) {
    PaintFixture fixture;
    Tree tree;
    tree.sync_nodes(rows_of(rows));
    return fixture.paint_at_origin(tree).size();
  };
  const std::size_t small_count = painted_for(100U);
  const std::size_t large_count = painted_for(4000U);
  // 40 倍行数差，绘制量必须**完全相同**（视口能装多少就画多少）。
  ST_CHECK_EQ(small_count, large_count);
}

/// 裁剪区被父级收窄时，只画收窄后可见的那几行（剔除读的是 `canvas.clip_rect()`）。
ST_TEST(tree_paint_respects_narrowed_clip_rect) {
  PaintFixture fixture;
  Tree tree;
  tree.sync_nodes(rows_of(50));

  // 裁剪到最上面 3 行（78px）。注意 `clip` 是**画布坐标**，树从 y=0 排起。
  fixture.canvas.push_clip_rect(Rect{0.0F, 0.0F, static_cast<float>(kViewportWidth), 78.0F});
  const std::vector<std::string> drawn =
      fixture.paint_at(tree, Rect{0.0F, 0.0F, static_cast<float>(kViewportWidth),
                                  50.0F * kRowHeight});
  fixture.canvas.pop_clip();

  ST_CHECK(drawn.size() <= 5U);  // 3 行 + 区间宽出量
  ST_CHECK_EQ(drawn.front(), std::string("row0"));
}

/// `first_visible` 是**真值**（上一帧首可见行），不再恒 "0"。
ST_TEST(tree_first_visible_reflects_last_paint) {
  PaintFixture fixture;
  Tree tree;
  tree.sync_nodes(rows_of(100));

  // 从未绘制过 → 保守回退 0（不报荒谬数字）。
  ST_CHECK_EQ(tree.get_property("first_visible").value_or("?"), std::string("0"));
  ST_CHECK_EQ(tree.visible_row_range_now().first, std::size_t{0});

  const std::vector<std::string> drawn = fixture.paint_at_origin(tree);
  ST_CHECK(!drawn.empty());
  // 视口从内容顶开始 → 首可见行仍是 0，但**末可见行**已是非平凡值。
  const auto [first, last] = tree.visible_row_range_now();
  ST_CHECK_EQ(first, std::size_t{0});
  ST_CHECK(last >= 14U);
  ST_CHECK(last <= 20U);
}

/// 把树**往下挪**（模拟已经滚动）后，`first_visible` 要跟着走——这是"契约通电"的核心。
ST_TEST(tree_first_visible_follows_scrolled_bounds) {
  PaintFixture fixture;
  Tree tree;
  tree.sync_nodes(rows_of(200));
  tree.measure(fixture.context(), Constraints{});
  // 内容已滚动到第 50 行：`ScrollView` 就是把 `-offset` 烘进子元素 bounds.y 的。
  // 高度**必须是内容全高**（不是视口高）——这正是真实情形：树比视口高，
  // 元素级剔除（`Element::paint`）才不会把整棵树裁掉（实控：写视口高会什么都没画）。
  tree.arrange(fixture.context(), Rect{0.0F, -50.0F * kRowHeight,
                                       static_cast<float>(kViewportWidth),
                                       200.0F * kRowHeight});
  const std::size_t before = fixture.port.drawn().size();
  fixture.canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
  tree.paint(fixture.context(), fixture.canvas);
  ST_CHECK(fixture.port.drawn().size() > before);  // 确实画了东西

  const auto [first, last] = tree.visible_row_range_now();
  ST_CHECK_EQ(first, std::size_t{50});
  ST_CHECK(last > first);
  // 属性面与之一致（`scroll` 按首可见行 × 行高报，单位是内容像素）。
  ST_CHECK_EQ(tree.get_property("first_visible").value_or("?"), std::string("50"));
  ST_CHECK_EQ(tree.get_property("scroll").value_or("?"), std::string("1300.0"));
}

/// 空树 / 未布局不崩、不算出荒谬区间。
ST_TEST(tree_visible_range_is_safe_when_empty_or_unlaid) {
  PaintFixture fixture;
  Tree empty;
  const std::vector<std::string> drawn = fixture.paint_at_origin(empty);
  ST_CHECK(drawn.empty());
  const auto [first, last] = empty.visible_row_range_now();
  ST_CHECK_EQ(first, std::size_t{0});
  ST_CHECK_EQ(last, std::size_t{0});
  ST_CHECK_EQ(empty.get_property("first_visible").value_or("?"), std::string("0"));
  ST_CHECK_EQ(empty.get_property("scroll").value_or("?"), std::string("0.0"));
}
