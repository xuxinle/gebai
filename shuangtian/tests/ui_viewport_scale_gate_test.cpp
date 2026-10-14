/// 大内容组件的**规模不变性**门禁 —— 让本轮的视口剔除成果不再悄悄退化。
///
/// ## 为什么不用"绝对耗时"或"比值 vs 清屏"
///
/// 本仓既有的性能门禁（`raster_bench_test.cpp`）用「被测负载 / 清屏每像素成本」做归一化，
/// 并**只在 release 档**断言（`calibration_profile()`）——因为清屏是纯 `memset`（可向量化）、
/// 而 AA 光栅化是标量循环，两者对 `-O` 的响应不同；文档还记着这条门禁在本容器里
/// 余量踩线（实测 0.66 倍 < 下限 1.3）。照搬那套会**继承同样的脆弱**。
///
/// ## 本文件的口径：**尺度不变性**（不依赖机器速度，也不依赖档位）
///
/// 同一组件、同一视口，只改**内容总量**：`t(N×) / t(1×)`。
///
/// | 视口剔除 | 期望比值 | 实测 |
/// |---|---|---|
/// | 做对了（O(可见行)） | ≈ 1 | 1.0 ~ 1.3 |
/// | 退化成 O(N) | ≈ N | 12 ~ 40 |
///
/// 判据 `kMaxScaleRatio = 5` 落在两者之间，**两侧各留 4 倍以上余量**：
/// 噪声（实测抖动 < 15%）根本够不着它，而退化（≥12×）一定越线。
/// 这也是它不必限定 release 档的原因——比值里没有"清屏 vs AA"那种跨档差异。
///
/// ## 覆盖的组件（每个都对应本轮修过的一处缺口）
///
/// | 组件 | 缺口形态 | 修前比值 |
/// |---|---|---|
/// | `Terminal`（行模式） | 多行 `Text` 每帧重折行 + 逐行画（无视口） | ≈ 40× |
/// | `Table` | 提前退出 `row.y >= bounds_.bottom()` 在 ScrollView 下永不触发 | ≈ 40× |
/// | `MarkdownView` | 组首行被剔除规则豁免 → 屏幕外每个组都建 `Path`+`stroke_path` | ≈ 20× |
/// | `Tree` | `paint_content` 遍历全部行 | ≈ 12× |
/// | 多行 `Text`（直接） | 每帧 `wrap` 全部内容 + 逐行量宽 | ≈ 20× |
///
/// ## 为什么断言"绘制"而不是"measure"
///
/// `measure` 侧的两处修复（`Tree`/`ListItem` 的行宽缓存）**不是** O(1)——
/// 它们把"断崖"换成了"便宜的线性扫描"，所以**规模不变性对它不成立**。
/// 那两处的正确护栏是**结构性的**（数 `measure_width` 调用次数），
/// 已经分别在 `ui_tree_label_width_cache_test.cpp` / `ui_list_item_width_cache_test.cpp` 里。

#include "st/test/test.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/components/tree.hpp"
#include "st/ui/theme.hpp"

namespace {

using Clock = std::chrono::steady_clock;

/// 判据：`t(N×) / t(1×)` 上限。实测固定后 ≈1，退化后 ≥12，5 落在两侧余量都 >4 倍处。
inline constexpr double kMaxScaleRatio{5.0};
/// 取多次**最小值**（噪声只抬高耗时；最小值最接近真实成本，同 `raster_bench_test` 的理由）。
inline constexpr int kRepeats{5};
inline constexpr int kWidth{800};
inline constexpr int kHeight{400};

auto elapsed_ms(Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template <class Fn>
[[nodiscard]] auto best_ms(Fn&& fn) -> double {
  double best = 1e9;
  for (int round = 0; round < kRepeats; ++round) {
    const auto start = Clock::now();
    fn();
    best = std::min(best, elapsed_ms(start));
  }
  return best;
}

/// 字体只加载一次（每个用例都 `system_default()` 会重复读盘，把门禁本身变慢）。
///
/// `static` 有理由：它是**只读的字体重链缓存**（构造后不再改），与 lint L8 要拦的
/// 「业务逻辑里的可变共享状态」不是一回事（同 `dsl.cpp` 的 `external_elements()` 口径）。
// lint-allow: L8 只读字体重链缓存（构造后不再改）
[[nodiscard]] auto& shared_setup() {
  struct Setup {
    std::unique_ptr<st::text::FontStack> fonts{};
    std::unique_ptr<st::ui::TextPort> port{};
    st::ui::Theme theme{st::ui::Theme::light()};
    bool ready{false};

    Setup() {
      auto loaded = st::text::FontStack::system_default();
      if (!loaded.has_value()) return;
      fonts = std::make_unique<st::text::FontStack>(std::move(*loaded));
      renderer = std::make_unique<st::text::TextRenderer>(*fonts);
      port = st::app::make_text_port(*renderer);
      ready = port != nullptr;
    }
    std::unique_ptr<st::text::TextRenderer> renderer{};
  };
  static Setup setup;
  return setup;
}

[[nodiscard]] auto context_for(const decltype(shared_setup())& setup) -> st::ui::RenderContext {
  return st::ui::RenderContext{setup.theme, setup.port.get(), 0.0};
}

[[nodiscard]] auto rows_text(std::size_t count) -> std::string {
  std::string text;
  text.reserve(count * 32U);
  for (std::size_t index = 0; index < count; ++index) {
    text += "int value_" + std::to_string(index) + " = compute();\n";
  }
  return text;
}

[[nodiscard]] auto markdown_blocks(std::size_t count) -> std::string {
  std::string text;
  for (std::size_t index = 0; index < count; ++index) {
    if (index % 3U == 0) {
      text += "## 小节 " + std::to_string(index) + "\n\n段落正文 " + std::to_string(index) + "\n\n";
    } else if (index % 3U == 1) {
      text += "```cpp\nint v" + std::to_string(index) + " = 1;\n```\n\n";
    } else {
      text += "- 项 " + std::to_string(index) + "\n\n";
    }
  }
  return text;
}

/// 打印比值并返回它（断言放在各用例里）。
///
/// 不在本函数里 `ST_REQUIRE`：它是**返回 `double` 的**函数，而 `ST_REQUIRE` 展开成
/// `return;`（只在 `void` 函数里合法）。
[[nodiscard]] auto report_ratio(const char* name, double small_ms, double large_ms,
                                std::size_t factor) -> double {
  const double ratio = small_ms > 0.0 ? large_ms / small_ms : 1.0e9;
  st::print("[scale-gate] {}: t(1×)={:.3f}ms · t({}×)={:.3f}ms → 比值 {:.2f}（上限 {:.1f}）\n",
            name, small_ms, factor, large_ms, ratio, kMaxScaleRatio);
  return ratio;
}

}  // namespace

/// `Terminal`（行模式）：内容量 20× 而每帧成本不变。
///
/// 这条守的是本轮最大的一处缺口（3298 ms → 0.438 ms @20000 行）。
/// 它同时覆盖了多行 `Text`——终端行模式就是把全文交给一个 `Text` 子件。
ST_TEST(scale_gate_terminal_frame_is_independent_of_log_size) {
  auto& setup = shared_setup();
  if (!setup.ready) return;

  const auto measure_at = [&](std::size_t lines) {
    st::ui::Terminal terminal;
    st::ui::RenderContext context = context_for(setup);
    terminal.measure(context, st::ui::Constraints{});
    terminal.arrange(context,
                     st::math::Rect{0.0F, 0.0F, static_cast<float>(kWidth),
                                    static_cast<float>(kHeight)});
    for (std::size_t index = 0; index < lines; ++index) {
      terminal.append("$ build target_" + std::to_string(index) + " ok");
    }
    terminal.measure(context, st::ui::Constraints{});
    terminal.arrange(context,
                     st::math::Rect{0.0F, 0.0F, static_cast<float>(kWidth),
                                    static_cast<float>(kHeight)});
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    return best_ms([&] {
      canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
      terminal.paint(context, canvas);
    });
  };

  const double small = measure_at(50U);
  const double large = measure_at(1000U);
  ST_CHECK(report_ratio("Terminal 每帧", small, large, 20U) <= kMaxScaleRatio);
}

/// `Table`：行数 20× 而每帧成本不变（守 113.27 ms → 0.502 ms @5000 行）。
ST_TEST(scale_gate_table_frame_is_independent_of_row_count) {
  auto& setup = shared_setup();
  if (!setup.ready) return;

  const auto measure_at = [&](std::size_t rows) {
    std::vector<st::ui::TableColumn> columns{
        {.name = "a", .width = 140.0F}, {.name = "b", .width = 140.0F},
        {.name = "c", .width = 140.0F}, {.name = "d", .width = 140.0F}};
    st::ui::Table table{columns};
    for (std::size_t index = 0; index < rows; ++index) {
      table.add_row({"file_" + std::to_string(index) + ".cpp", "modified", "1.2 KB", "main"});
    }
    st::ui::RenderContext context = context_for(setup);
    table.measure(context, st::ui::Constraints{});
    // **内容全高**（ScrollView 子元素给的就是这个）——这正是旧代码提前退出失效的那种排布。
    const float full = static_cast<float>(rows) * table.row_height() + 40.0F;
    table.arrange(context, st::math::Rect{0.0F, 0.0F, static_cast<float>(kWidth), full});
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    return best_ms([&] {
      canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
      table.paint(context, canvas);
    });
  };

  const double small = measure_at(50U);
  const double large = measure_at(1000U);
  ST_CHECK(report_ratio("Table 每帧", small, large, 20U) <= kMaxScaleRatio);
}

/// `MarkdownView`：块数 20× 而每帧成本不变（守 13.463 ms → 0.360 ms @4000 段）。
///
/// 这条同时盯住**二分定位**——若有人把二分改回全量遍历，或把"组矩形相交"判据写错
/// （退回"组首行永不剔除"），比值都会立刻越线。
ST_TEST(scale_gate_markdown_frame_is_independent_of_document_size) {
  auto& setup = shared_setup();
  if (!setup.ready) return;

  const auto measure_at = [&](std::size_t blocks) {
    st::ui::MarkdownView view;
    view.set_markdown(markdown_blocks(blocks));
    st::ui::RenderContext context = context_for(setup);
    view.measure(context, st::ui::Constraints{});
    view.arrange(context,
                 st::math::Rect{0.0F, 0.0F, static_cast<float>(kWidth),
                                static_cast<float>(kHeight)});
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    return best_ms([&] {
      canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
      view.paint(context, canvas);
    });
  };

  const double small = measure_at(50U);
  const double large = measure_at(1000U);
  ST_CHECK(report_ratio("MarkdownView 每帧", small, large, 20U) <= kMaxScaleRatio);
}

/// `Tree`：行数 20× 而每帧成本不变（守 5.36 ms → 0.24 ms @1000 行）。
ST_TEST(scale_gate_tree_frame_is_independent_of_row_count) {
  auto& setup = shared_setup();
  if (!setup.ready) return;

  const auto measure_at = [&](std::size_t rows) {
    st::ui::Tree tree;
    std::vector<st::ui::TreeNode> nodes;
    nodes.reserve(rows);
    for (std::size_t index = 0; index < rows; ++index) {
      nodes.push_back(st::ui::TreeNode{.key = "k" + std::to_string(index),
                                       .label = "translation_unit_" + std::to_string(index) + ".cpp",
                                       .depth = static_cast<int>(index % 4U)});
    }
    tree.sync_nodes(nodes);
    st::ui::RenderContext context = context_for(setup);
    tree.measure(context, st::ui::Constraints{});
    // 内容全高（ScrollView 子元素的姿态）。
    const float full = static_cast<float>(rows) * tree.row_height() + 40.0F;
    tree.arrange(context, st::math::Rect{0.0F, 0.0F, static_cast<float>(kWidth), full});
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    return best_ms([&] {
      canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
      tree.paint(context, canvas);
    });
  };

  const double small = measure_at(50U);
  const double large = measure_at(1000U);
  ST_CHECK(report_ratio("Tree 每帧", small, large, 20U) <= kMaxScaleRatio);
}

/// 多行 `Text`（直接）：行数 20× 而每帧成本不变。
///
/// 单列一条（而不是只靠 Terminal 那条）：`Text` 是**基础组件**，
/// 将来若有别的宿主用它放大内容，这条门禁仍然盖得住。
ST_TEST(scale_gate_multiline_text_frame_is_independent_of_line_count) {
  auto& setup = shared_setup();
  if (!setup.ready) return;

  const auto measure_at = [&](std::size_t lines) {
    st::ui::Text text{rows_text(lines)};
    text.set_multiline(true);
    st::ui::RenderContext context = context_for(setup);
    // ⚠ **宽度必须与 arrange 的一致**：`Text::measure` 用 `constraints.max_width`、
    // `paint` 用 `bounds_.width`，两者不等就每帧重折行（本门禁初版因此跑了 12.6 s 并
    // 触发软超时——那正是 `Text` 排版缓存要解决的那个 cache miss）。
    st::ui::Constraints constraints;
    constraints.max_width = static_cast<float>(kWidth);
    text.measure(context, constraints);
    const float line_height = setup.port->line_height(setup.theme.metrics().font_base);
    text.arrange(context,
                 st::math::Rect{0.0F, 0.0F, static_cast<float>(kWidth),
                                static_cast<float>(lines) * line_height + 40.0F});
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
    return best_ms([&] {
      canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
      text.paint(context, canvas);
    });
  };

  const double small = measure_at(50U);
  const double large = measure_at(1000U);
  ST_CHECK(report_ratio("多行 Text 每帧", small, large, 20U) <= kMaxScaleRatio);
}
