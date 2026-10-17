/// `MarkdownView` / `MergeView` 大内容量尺 —— 与 `ui_tree_rows_bench` 同一套口径。
///
/// ## 筛查结论（按"有没有自绘行循环"这条判据）
///
/// | 组件 | 行循环 | 视口处理 |
/// |---|---|---|
/// | `MarkdownView` | **有**（`for (const Row& row : rows_)`） | 逐行 cheap 边界检查 + `continue`（同 `List` 的形态） |
/// | `MergeView` | **有**（`for (index < plan_.segments.size())`） | 逐段 cheap 边界检查 + `continue` |
///
/// 两者都是"**遍历全部 + 逐项便宜检查**"——不是 `Tree`/`Table`/`Text` 那种"零剔除"，
/// 也不是 `List` 那种"元素级剔除兜底"。所以真正要量的是：
/// **每帧成本是否随内容总量线性增长**（若是，说明 N 次迭代本身在大 N 下就够贵）。
///
/// ## 用法
///
/// ```bash
/// tools/build_ui_probe.sh ui_rich_views_bench
/// build/probe/ui_rich_views_bench [规模]
/// ```

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/merge_view.hpp"
#include "st/ui/theme.hpp"

namespace {

using Clock = std::chrono::steady_clock;

auto elapsed_ms(Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template <class Fn>
[[nodiscard]] auto best_ms(int rounds, Fn&& fn) -> double {
  double best = 1e9;
  for (int round = 0; round < rounds; ++round) {
    const auto start = Clock::now();
    fn();
    best = std::min(best, elapsed_ms(start));
  }
  return best;
}

/// 造 `blocks` 段的 Markdown（段落 + 列表 + 代码块 + 表格各占一部分，让行种类混合）。
[[nodiscard]] auto make_markdown(std::size_t blocks) -> std::string {
  std::string text;
  text.reserve(blocks * 80U);
  for (std::size_t index = 0; index < blocks; ++index) {
    switch (index % 4U) {
      case 0:
        text += "## 小节 " + std::to_string(index) + "\n\n这是第 " + std::to_string(index) +
                " 段正文，用来把行数堆起来。\n\n";
        break;
      case 1:
        text += "- 列表项 " + std::to_string(index) + "\n- 列表项 " +
                std::to_string(index + 1) + "\n\n";
        break;
      case 2:
        text += "```cpp\nint value_" + std::to_string(index) + " = " +
                std::to_string(index) + ";\n```\n\n";
        break;
      default:
        text += "| 列 A | 列 B |\n| --- | --- |\n| " + std::to_string(index) +
                " | 数据 |\n\n";
        break;
    }
  }
  return text;
}

/// 造 `lines` 行的纯文本（两侧几乎相同 → 产物段数少；这是"大文件小差异"的常见形态）。
[[nodiscard]] auto make_side(std::size_t lines, bool theirs) -> std::string {
  std::string text;
  text.reserve(lines * 32U);
  for (std::size_t index = 0; index < lines; ++index) {
    text += "line ";
    text += std::to_string(index);
    // 每隔 50 行制造一处差异（冲突段），其余为共同段。
    if (theirs && index % 50U == 7U) text += "  <theirs>";
    text += '\n';
  }
  return text;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "`MarkdownView` / `MergeView` 大内容量尺 —— 与 `ui_tree_rows_bench` 同一套口径。",
      .criterion = "",  // 判据由人补（脚本不猜合格线）
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("ui_rich_views_bench", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  const std::size_t scale = argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1])) : 500U;

  auto fonts = st::text::FontStack::system_default();
  if (!fonts.has_value()) {
    st::print("SKIP: 无系统字体，量尺不成立（measure_width 恒 0 会量出假结果）\n");
    return 0;
  }
  st::text::TextRenderer renderer{*fonts};
  auto port = st::app::make_text_port(renderer);

  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::RenderContext context{theme, port.get(), 0.0};

  constexpr int kWidth = 800;
  constexpr int kHeight = 400;
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);

  st::print("scale={}\n", scale);

  // ────────── MarkdownView ──────────
  {
    st::ui::MarkdownView view;
    const std::string markdown = make_markdown(scale);

    const auto set_start = Clock::now();
    view.set_markdown(markdown);
    const double set_ms = elapsed_ms(set_start);

    view.measure(context, st::ui::Constraints{});
    view.arrange(context, st::math::Rect{0.0F, 0.0F, static_cast<float>(kWidth),
                                         static_cast<float>(kHeight)});
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    const auto first_start = Clock::now();
    view.paint(context, canvas);
    const double first_paint_ms = elapsed_ms(first_start);

    const double steady_ms = best_ms(10, [&] {
      canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
      view.paint(context, canvas);
    });
    st::print("  MarkdownView  set_markdown  {:8.3f} ms\n", set_ms);
    st::print("  MarkdownView  paint(首次)   {:8.3f} ms\n", first_paint_ms);
    st::print("  MarkdownView  paint(稳态)   {:8.3f} ms   <- 每帧成本\n", steady_ms);
  }

  // ────────── MergeView ──────────
  {
    st::ui::MergeView view;
    const std::string ours = make_side(scale * 4U, false);
    const std::string theirs = make_side(scale * 4U, true);

    const auto set_start = Clock::now();
    view.set_texts(ours, theirs);
    const double set_ms = elapsed_ms(set_start);

    view.measure(context, st::ui::Constraints{});
    view.arrange(context, st::math::Rect{0.0F, 0.0F, static_cast<float>(kWidth),
                                         static_cast<float>(kHeight)});
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    const auto first_start = Clock::now();
    view.paint(context, canvas);
    const double first_paint_ms = elapsed_ms(first_start);

    const double steady_ms = best_ms(10, [&] {
      canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
      view.paint(context, canvas);
    });
    st::print("  MergeView     set_texts     {:8.3f} ms   <- 含一次三方合并\n", set_ms);
    st::print("  MergeView     paint(首次)   {:8.3f} ms\n", first_paint_ms);
    st::print("  MergeView     paint(稳态)   {:8.3f} ms   <- 每帧成本\n", steady_ms);
  }

  // 结论退出码由外壳统一给出（0 合格 / 1 不合格 / 77 跳过）

  return st::probe::main_of(spec, opts, verdict);
}
