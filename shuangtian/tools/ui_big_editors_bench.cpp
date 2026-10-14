/// `CodeEditor` / `Terminal` 大内容量尺 —— 与 `ui_tree_rows_bench` 同一套口径。
///
/// ## 为什么这两个要一起量（它们是"机制齐全"的一对）
///
/// 与 `Tree`/`List`/`Table` 不同，这两个大组件**都已经有视口机制**：
///
/// - `CodeEditor`：`visible_line_range()` + `last_visible_first_/last_`（绘制只遍历可见行）；
/// - `Terminal`：屏幕模型自带 `scrollback_count()`/`rows()`（回看区 + 视口行）。
///
/// 所以这里要回答的不是"有没有剔除"，而是**剔除之外还剩多少随内容线性增长的成本**——
/// 那才是这两个组件在大文件/长日志下的真实上限。量尺把两项分开：
///
/// | 尺子 | 它回答的问题 |
/// |---|---|
/// | `set_text`/`append`（吞吐） | 喂内容本身的成本（一次性） |
/// | `paint`（稳态帧） | 每帧成本——**是否与内容总量无关** |
///
/// ## 用法
///
/// ```bash
/// tools/build_ui_probe.sh ui_big_editors_bench
/// build/probe/ui_big_editors_bench [行数]
/// ```

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/terminal.hpp"
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

/// 造 `lines` 行"像代码"的文本（高亮规则真的会扫到 token，不是纯注释）。
[[nodiscard]] auto make_source(std::size_t lines) -> std::string {
  std::string text;
  text.reserve(lines * 64U);
  for (std::size_t line = 0; line < lines; ++line) {
    text += "int value_";
    text += std::to_string(line);
    text += " = compute(";
    text += std::to_string(line % 97U);
    text += ");  // 第 ";
    text += std::to_string(line);
    text += " 行\n";
  }
  return text;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::size_t lines = argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1])) : 2000U;

  auto fonts = st::text::FontStack::system_default();
  if (!fonts.has_value()) {
    st::print("SKIP: 无系统字体，量尺不成立（measure_width 恒 0 会量出假结果）\n");
    return 0;
  }
  st::text::TextRenderer renderer{*fonts};
  auto port = st::app::make_text_port(renderer);

  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::RenderContext context{theme, port.get(), 0.0};

  constexpr int kWidth = 900;
  constexpr int kViewportHeight = 400;
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kViewportHeight, 1.0f);
  // 编辑器/终端在自己的 bounds 内滚动：把它们排成"内容全高"（与 ScrollView 子元素同姿态）。
  const float tall = static_cast<float>(kViewportHeight);  // 它们靠内部 scroll_y_ 滚动

  st::print("lines={}\n", lines);

  // ────────── CodeEditor ──────────
  {
    st::ui::CodeEditor editor;
    editor.set_language("cpp");
    const std::string source = make_source(lines);

    const auto set_start = Clock::now();
    editor.set_text(source);
    const double set_ms = elapsed_ms(set_start);

    editor.measure(context, st::ui::Constraints{});
    editor.arrange(context, st::math::Rect{0.0F, 0.0F, static_cast<float>(kWidth), tall});
    // 首次绘制（含一次性高亮重建）与稳态帧分开量——两者不是同一件事。
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    const auto first_start = Clock::now();
    editor.paint(context, canvas);
    const double first_paint_ms = elapsed_ms(first_start);

    const double steady_ms = best_ms(10, [&] {
      canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
      editor.paint(context, canvas);
    });
    st::print("  CodeEditor  set_text      {:8.3f} ms   <- 喂内容（含首次高亮）\n", set_ms);
    st::print("  CodeEditor  paint(首次)   {:8.3f} ms   <- 含一次性高亮重建\n", first_paint_ms);
    st::print("  CodeEditor  paint(稳态)   {:8.3f} ms   <- 每帧成本\n", steady_ms);
  }

  // ────────── Terminal（行模式 append） ──────────
  {
    st::ui::Terminal terminal;
    terminal.measure(context, st::ui::Constraints{});
    terminal.arrange(context, st::math::Rect{0.0F, 0.0F, static_cast<float>(kWidth), tall});

    const auto feed_start = Clock::now();
    for (std::size_t line = 0; line < lines; ++line) {
      std::string text = "$ build target_" + std::to_string(line) + " ok";
      terminal.append(std::move(text));
    }
    const double feed_ms = elapsed_ms(feed_start);

    terminal.measure(context, st::ui::Constraints{});
    terminal.arrange(context, st::math::Rect{0.0F, 0.0F, static_cast<float>(kWidth), tall});
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    const auto first_start = Clock::now();
    terminal.paint(context, canvas);
    const double first_paint_ms = elapsed_ms(first_start);

    const double steady_ms = best_ms(10, [&] {
      canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
      terminal.paint(context, canvas);
    });
    st::print("  Terminal    append×{}   {:8.3f} ms   <- 喂内容\n", lines, feed_ms);
    st::print("  Terminal    paint(首次)   {:8.3f} ms\n", first_paint_ms);
    st::print("  Terminal    paint(稳态)   {:8.3f} ms   <- 每帧成本\n", steady_ms);
  }

  return 0;
}
