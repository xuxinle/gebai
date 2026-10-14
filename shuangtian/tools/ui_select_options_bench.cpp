/// `SelectPanel` 选项数量尺 —— 收尾扫描（前面几轮的判据：有没有未剔除的项循环）。
///
/// ## 为什么只量它
///
/// 全组件扫描（`paint_content` 里有无未剔除的循环）之后，剩下的候选都是**小数据量**
/// 组件：`MenuPanel`（菜单 5~30 项）、`Tabs`（标签几个~几十个）、`TitleBar`（按钮 3 个）。
/// 唯一可能长大的自绘列表是 `SelectPanel`：它**无滚动**（面板一次铺开全部选项），
/// 且 `paint_content` 遍历全部选项且**没有任何剔除**。
///
/// 所以这里量它，看它是否真的构成问题——**不预设结论**。
///
/// ## 用法
///
/// ```bash
/// tools/build_ui_probe.sh ui_select_options_bench
/// build/probe/ui_select_options_bench [选项数]
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
#include "st/ui/components/select.hpp"
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

[[nodiscard]] auto make_options(std::size_t count) -> std::vector<st::ui::SelectOption> {
  std::vector<st::ui::SelectOption> options;
  options.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    st::ui::SelectOption option;
    option.value = "value-" + std::to_string(index);
    option.label = "选项 label-" + std::to_string(index);
    options.push_back(std::move(option));
  }
  return options;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::size_t count = argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1])) : 200U;

  auto fonts = st::text::FontStack::system_default();
  if (!fonts.has_value()) {
    st::print("SKIP: 无系统字体\n");
    return 0;
  }
  st::text::TextRenderer renderer{*fonts};
  auto port = st::app::make_text_port(renderer);

  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::RenderContext context{theme, port.get(), 0.0};

  st::ui::SelectPanel panel{make_options(count), std::nullopt};

  panel.measure(context, st::ui::Constraints{});
  const float height = panel.measured_size().height;
  // **真实场景**：画布是屏幕（800×600），面板自己高到全部选项——于是绝大部分选项
  // 落在屏幕之外，但 `paint_content` 的循环仍会遍历它们（没有剔除）。
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(800, 600, 1.0f);
  panel.arrange(context, st::math::Rect{0.0f, 0.0f, 320.0f, height});

  const double measure_ms = best_ms(20, [&] { panel.measure(context, st::ui::Constraints{}); });
  const double paint_ms = best_ms(10, [&] {
    canvas.clear(st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});
    panel.paint(context, canvas);
  });

  st::print("options={}  面板高={:.0f}px  画布=800×600（屏幕尺寸）\n", count, height);
  st::print("  measure   {:8.3f} ms\n", measure_ms);
  st::print("  paint     {:8.3f} ms   <- 每帧成本\n", paint_ms);
  return 0;
}
