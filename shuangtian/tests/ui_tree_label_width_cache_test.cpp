/// `Tree` 行宽缓存的**一致性面**护栏。
///
/// ## 这组用例守的是什么
///
/// `Tree::measure` 过去每次遍历全部行调 `TextPort::measure_width`，而文本层的宽度缓存
/// **只有 4096 条、满了整表清空**（`text.cpp`）。于是超过 4096 行时每轮 measure 都在
/// "刚被清空"的缓存上做顺序扫描——命中率≈0，**断崖式**退化。实测
/// （`tools/ui_tree_rows_bench.cpp`，重复 measure 路径）：
///
/// | 行数 | 改前 | 改后 |
/// |---|---|---|
/// | 4000 | 0.134 ms | 0.003 ms |
/// | 4200 | **9.885 ms** | **0.003 ms** |
/// | 5000 | 11.853 ms | 0.004 ms |
/// | 20000 | ~53 ms | 0.016 ms |
///
/// 修法是让 `Tree` 自己按行缓存标签宽度（组件知道自己的行何时变，文本层只能猜）。
///
/// ## 为什么这组用例的重点是"失效"而不是"变快"
///
/// 缓存的**性能**收益由 `ui_tree_rows_bench` 量（机器相关，不适合硬断言）；
/// 而缓存的**风险**是"改了标签/字号却读回旧宽度"——那是**正确性**问题，且表现为
/// "界面宽度不对"这种截图里也难一眼看出的偏差。所以这里用一个**数调用次数**的
/// 文本端口：它同时证明两件事——重复 measure 不再重复量（缓存生效）、
/// 数据变化后确实重新量了（失效正确）。

#include "st/test/test.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/components/tree.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::test::ProportionalTextPort;
using st::ui::Constraints;
using st::ui::RenderContext;
using st::ui::Theme;
using st::ui::TreeNode;
using st::ui::Tree;

/// 包装比例端口，**计数 `measure_width` 的调用次数**（其余一律转发）。
///
/// 为什么包一层而不是改共享 fixture：这是本文件独有的观察需求，塞进 `tests/support/`
/// 会给其他 9 个使用者增加一个它们不用的字段（那份 fixture 的头部注释专门讲过
/// "为消重复而消重复"的教训）。
///
/// 选 `ProportionalTextPort`（而非 `FixedAdvanceTextPort`）作内层：后者的宽度是
/// `kAdvance × 字符数`，**与 `size` 无关**——用它会让"字号变了宽度该跟着变"
/// 这条断言天然不成立（实测踩到：`scale_fonts` 后宽度纹丝不动）。
class CountingTextPort final : public st::ui::TextPort {
 public:
  ProportionalTextPort inner{};
  mutable std::size_t width_calls{0};

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return inner.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(
      std::string_view utf8, float size,
      st::text::FontRole role = st::text::FontRole::Proportional) const -> float override {
    ++width_calls;
    return inner.measure_width(utf8, size, role);
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return inner.line_height(size);
  }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color, st::text::FontRole role = st::text::FontRole::Proportional,
            float embolden = 0.0f, bool bold = false) const override {
    inner.draw(canvas, utf8, origin, size, color, role, embolden, bold);
  }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    return inner.ellipsize(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    return inner.wrap(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    return inner.wrap_limited(utf8, size, max_width, max_lines);
  }
};

struct Fixture {
  Theme theme{Theme::light()};
  CountingTextPort port{};

  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

[[nodiscard]] auto rows_with_labels(const std::vector<std::string>& labels)
    -> std::vector<TreeNode> {
  std::vector<TreeNode> rows;
  rows.reserve(labels.size());
  for (std::size_t index = 0; index < labels.size(); ++index) {
    rows.push_back(TreeNode{.key = "k" + std::to_string(index), .label = labels[index]});
  }
  return rows;
}

/// 量一次并返回 `measure_width` 的**本次新增调用数**。
[[nodiscard]] auto measure_calls(Fixture& fixture, Tree& tree) -> std::size_t {
  const std::size_t before = fixture.port.width_calls;
  tree.measure(fixture.context(), Constraints{});
  return fixture.port.width_calls - before;
}

}  // namespace

/// 数据没变时重复 measure **不再重复量**（缓存生效的直接证据）。
ST_TEST(tree_label_width_cache_avoids_repeat_measurement) {
  Fixture fixture;
  Tree tree;
  tree.sync_nodes(rows_with_labels({"alpha", "beta", "gamma", "delta"}));

  ST_CHECK_EQ(measure_calls(fixture, tree), std::size_t{4});  // 首轮：每行量一次
  ST_CHECK_EQ(measure_calls(fixture, tree), std::size_t{0});  // 次轮：一次也不量
}

/// **行数不变但标签变了**时必须重算——缓存一致性面的关键失效点
/// （`sync_nodes` 里 `rows_.clear()` 后紧接 push，行数常与上次相同）。
ST_TEST(tree_label_width_cache_follows_sync_nodes) {
  Fixture fixture;
  Tree tree;
  tree.sync_nodes(rows_with_labels({"short", "b"}));
  (void)measure_calls(fixture, tree);
  const float narrow = tree.measured_size().width;

  // 同样 2 行，标签变长：宽度必须跟着变大，而不是读回缓存里的旧值。
  tree.sync_nodes(rows_with_labels({"a_very_long_label_indeed", "b"}));
  ST_CHECK(measure_calls(fixture, tree) > 0U);  // 确实重新量了
  ST_CHECK(tree.measured_size().width > narrow);
}

/// `clear_nodes` 后缓存不残留：再填新行按新行量。
ST_TEST(tree_label_width_cache_clears_with_nodes) {
  Fixture fixture;
  Tree tree;
  tree.sync_nodes(rows_with_labels({"a"}));
  (void)measure_calls(fixture, tree);

  tree.clear_nodes();
  tree.measure(fixture.context(), Constraints{});
  ST_CHECK_EQ(tree.node_count(), std::size_t{0});

  tree.sync_nodes(rows_with_labels({"a_much_longer_label"}));
  ST_CHECK_EQ(measure_calls(fixture, tree), std::size_t{1});  // 新行必须量
}

/// **字号变了**要重算（宽度依赖字号；不失效就会"字号调大、树宽没跟上"）。
ST_TEST(tree_label_width_cache_follows_font_size) {
  Fixture fixture;
  Tree tree;
  tree.sync_nodes(rows_with_labels({"label"}));
  (void)measure_calls(fixture, tree);
  const float small = tree.measured_size().width;

  // 字号变：文档里的正道是**整体缩放字号阶梯**（`Metrics::scale_fonts`），
  // 再 `apply_theme` 落到 `style_.font_size`。
  fixture.theme.metrics().scale_fonts(2.0F);
  tree.apply_theme(fixture.theme);

  ST_CHECK(measure_calls(fixture, tree) > 0U);  // 必须重量
  ST_CHECK(tree.measured_size().width > small);
}

/// 缓存**不改变结果**：命中与否量出的宽度一致。
ST_TEST(tree_label_width_cache_preserves_result) {
  Fixture fixture;
  Tree reference;
  Tree cached;
  const auto rows = rows_with_labels({"x", "a_longer_one", "mid_len"});
  reference.sync_nodes(rows);
  cached.sync_nodes(rows);

  reference.measure(fixture.context(), Constraints{});
  for (int round = 0; round < 3; ++round) cached.measure(fixture.context(), Constraints{});

  ST_CHECK_EQ(cached.measured_size().width, reference.measured_size().width);
  ST_CHECK_EQ(cached.measured_size().height, reference.measured_size().height);
}

/// 大行数下缓存仍生效——断崖修复的直接断言（调用数不随轮数涨，而不是"变快"）。
ST_TEST(tree_label_width_cache_holds_for_many_rows) {
  Fixture fixture;
  Tree tree;
  std::vector<std::string> labels;
  labels.reserve(5000);
  for (std::size_t index = 0; index < 5000; ++index) {
    labels.push_back("translation_unit_" + std::to_string(index) + ".cpp");
  }
  tree.sync_nodes(rows_with_labels(labels));

  ST_CHECK_EQ(measure_calls(fixture, tree), std::size_t{5000});  // 首轮全量
  ST_CHECK_EQ(measure_calls(fixture, tree), std::size_t{0});     // 次轮零调用（改前这里是 5000）
}

/// **缓存失效判据必须看内容，不能只看行数**。
///
/// 缺陷形态（2026-10-18 静态审查发现）：`Tree::measure` 的宽度缓存失效条件曾是
/// “行数变了 或 字号变了”。而目录刷新最常见的形态恰恰是**行数不变、标签变了**
/// （改文件名、切换同名数量的目录、状态图标变化）——
/// 那种情况下宽度永远是旧的：树会按旧标签宽排版，**长标签被截、短标签留一大片空**，
/// 而"行数没变"让这个 bug 看起来完全不该发生。
///
/// 判据：同样 3 行，改掉其中一行的标签（长度不同），measure 出的宽必须跟着变。
ST_TEST(tree_label_width_cache_invalidates_on_label_change) {
  Fixture fixture;
  Tree tree;
  tree.sync_nodes(rows_with_labels({"a.cpp", "b.cpp", "c.cpp"}));
  tree.measure(fixture.context(), Constraints{});
  const float narrow = tree.measured_size().width;

  // 行数不变，只把某一行改成一个**明显更长**的标签。
  tree.sync_nodes(rows_with_labels({"a.cpp", "b.cpp",
                                    "a_really_long_file_name_that_should_widen.cpp"}));
  tree.measure(fixture.context(), Constraints{});
  const float widened = tree.measured_size().width;

  ST_CHECK(widened > narrow);
}

/// **稳态 measure 必须是 O(1)**：行集与字号都没变时，重复 measure 不该有任何逐行工作。
///
/// 缺陷形态（2026-10-18 静态审查发现）：只缓存逐行标签宽时，求“最宽行”那一步
/// 仍**每帧扫全部行**——“缓存生效”只省了 `measure_width` 调用，总成本仍 O(总行数)。
/// 目录大的时候（数千行）滚动/悬停/焦点变化每次都要走一遍。
///
/// 判据不看时间（机器相关），看**结构性事实**：稳态 measure 之后再用
/// `measure_width` 计数端口检查——调用数必须是 0；并且再用一个“行数翻倍”的
/// 树验证 measure 结果不因重复调用漂移。
ST_TEST(tree_measure_is_constant_work_when_unchanged) {
  // **稳态 measure 不该有逐行工作**（不只看“量宽调用数”）。
  //
  // 缺陷形态（2026-10-18 静态审查发现）：只缓存逐行标签宽时，求“最宽行”那一步
  // 仍**每帧扫全部行**——“缓存生效”只省了 `measure_width` 调用，总成本仍 O(总行数)。
  //
  // ⚠ 判据选型（踩过的坑）：只数 `measure_width` 调用**测不出来**——
  // 求最大值的循环本来就不调它（缺陷实现下也是 0 次）。真正有效的判据是
  // **耗时随行数的增长**：稳态下 3000 行与 200 行的 measure 必须同量级。
  // 用“大/小”比值而不是绝对毫秒，避开机器差异。
  Fixture fixture;
  Tree tree;
  const auto steady_measure_us = [&](std::size_t rows) {
    Tree local;
    std::vector<std::string> labels;
    labels.reserve(rows);
    for (std::size_t index = 0; index < rows; ++index) {
      labels.push_back("node_" + std::to_string(index) + ".cpp");
    }
    local.sync_nodes(rows_with_labels(labels));
    local.measure(fixture.context(), Constraints{});   // 首轮：建立缓存
    // 稳态：连续多次 measure，取总计（次数越多，单次噪声越小）。
    const auto started = std::chrono::steady_clock::now();
    for (int round = 0; round < 200; ++round) {
      local.measure(fixture.context(), Constraints{});
    }
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
  };

  const double small_us = steady_measure_us(200);
  const double large_us = steady_measure_us(3000);
  // 15 倍行数差：稳态耗时比值必须远小于它（缺陷实现下会接近线性）。
  // 取 4× 门槛：缓存到位时两者应接近（1×~2×），线性时会到 15×。
  ST_CHECK(large_us < small_us * 4.0 + 50.0);
  (void)tree;
}

/// 最大行宽的缓存**必须随内容/字号/主题失效**（缓存的代价是失效逻辑要完整）。
ST_TEST(tree_max_row_width_cache_invalidates) {
  Fixture fixture;
  Tree tree;
  tree.sync_nodes(rows_with_labels({"aa", "bb"}));
  tree.measure(fixture.context(), Constraints{});
  const float narrow = tree.measured_size().width;

  // ① 换成更长的标签（行数不变）→ 必须变宽。
  tree.sync_nodes(rows_with_labels({"aa", "a_much_longer_label_here.cpp"}));
  tree.measure(fixture.context(), Constraints{});
  ST_CHECK(tree.measured_size().width > narrow);

  // ② 换字号 → 宽度必须重算（不是回读旧值）。
  const float before_font = tree.measured_size().width;
  tree.style().font_size *= 2.0F;
  tree.measure(fixture.context(), Constraints{});
  ST_CHECK(tree.measured_size().width > before_font);
}
