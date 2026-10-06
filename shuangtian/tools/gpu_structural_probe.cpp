// GPU/软件结构性差异的**空间分布**诊断：把同一个场景渲染两份，按行统计「差 >64」的
// 像素数，并列出最差像素的坐标与两侧颜色——用来判定结构性差异落在文字、控件还是几何上。
//
// 为什么需要它：`gpu_matches_software_for_layout_of_real_widgets` 只报一个总占比
// （0.307% vs 阈值 0.1%），而"哪一类内容对不上"决定了该修哪一边（GPU 光栅器还是场景）。
//
// 用法： gpu_structural_probe

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/surface.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/toggle.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Surface;
using st::ui::Button;
using st::ui::FlexDirection;
using st::ui::Input;
using st::ui::Panel;
using st::ui::Switch;
using st::ui::Text;
using st::ui::Theme;

inline constexpr int kWidth = 400;
inline constexpr int kHeight = 300;
inline constexpr std::uint32_t kBackground = 0x0A0F1AFFU;

struct Scene {
  Theme theme{Theme::light()};
  std::shared_ptr<st::text::FontStack> stack{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<st::ui::TextPort> port{};
  std::optional<st::ui::RenderContext> context_storage{};
  Canvas software{kWidth, kHeight};
  std::unique_ptr<Surface> gpu{};

  Scene() {
    context_storage.emplace(st::ui::RenderContext{theme, nullptr, 0.0});
    if (auto loaded = st::text::FontStack::system_default()) {
      stack = std::make_shared<st::text::FontStack>(std::move(*loaded));
      renderer = std::make_unique<st::text::TextRenderer>(*stack);
      port = st::app::make_text_port(*renderer);
      context_storage.emplace(st::ui::RenderContext{theme, port.get(), 0.0});
    }
    auto created = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
    if (created.has_value()) gpu = std::move(*created);
  }

  [[nodiscard]] auto context() -> st::ui::RenderContext& { return *context_storage; }

  void render(const st::ui::Element& root) {
    software.clear(Color{0x0A, 0x0F, 0x1A, 0xFF});
    root.paint(context(), software);
    if (gpu != nullptr) {
      gpu->clear(Color{0x0A, 0x0F, 0x1A, 0xFF});
      root.paint(context(), *gpu);
    }
  }
};

[[nodiscard]] auto channel_delta(std::uint32_t a, std::uint32_t b) -> int {
  int delta = 0;
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    const int ca = static_cast<int>((a >> shift) & 0xFFU);
    const int cb = static_cast<int>((b >> shift) & 0xFFU);
    delta = std::max(delta, std::abs(ca - cb));
  }
  return delta;
}

}  // namespace

auto main() -> int {
  Scene scene;
  if (scene.gpu == nullptr) {
    st::print("GPU 表面不可用\n");
    return 0;
  }
  Panel root(FlexDirection::Column);
  root.style().padding = st::math::Insets::all(20.0f);
  root.style().gap = 12.0f;
  auto heading = std::make_unique<Text>("GPU 对比");
  heading->set_font_size(20.0f);
  root.add_child(std::move(heading));
  auto toggle = std::make_unique<Switch>("启用");
  toggle->set_checked(true);
  root.add_child(std::move(toggle));
  auto input = std::make_unique<Input>();
  input->set_placeholder("输入…");
  root.add_child(std::move(input));
  auto button = std::make_unique<Button>("确定");
  root.add_child(std::move(button));

  root.measure(scene.context(), st::ui::Constraints{.max_width = kWidth, .max_height = kHeight});
  root.arrange(scene.context(), Rect{0.0f, 0.0f, kWidth, kHeight});
  scene.render(root);

  const auto left = scene.software.pixels();
  const auto right = scene.gpu->pixels();
  st::print("尺寸 {} vs {}\n", left.size(), right.size());
  if (left.size() != right.size() || left.empty()) return 0;

  std::vector<int> per_row(kHeight, 0);
  std::size_t structural = 0;
  std::array<std::pair<int, int>, 8> worst{};
  worst.fill({0, -1});
  for (std::size_t index = 0; index < left.size(); ++index) {
    const int delta = channel_delta(left[index], right[index]);
    if (delta > 64) {
      ++structural;
      const auto y = static_cast<int>(index / static_cast<std::size_t>(kWidth));
      if (y >= 0 && y < kHeight) ++per_row[static_cast<std::size_t>(y)];
    }
    for (auto& entry : worst) {
      if (delta > entry.first) {
        entry = {delta, static_cast<int>(index)};
        break;
      }
    }
  }
  st::print("结构性像素（Δ>64）{} / {} = {:.3f}%  阈值 0.1%\n", structural, left.size(),
            100.0 * static_cast<double>(structural) / static_cast<double>(left.size()));
  st::print("\n按行分布（只列非零行）：\n");
  for (int y = 0; y < kHeight; ++y) {
    const int count = per_row[static_cast<std::size_t>(y)];
    if (count == 0) continue;
    st::print("  y={:3}  {:3}  {}\n", y, count, std::string(static_cast<std::size_t>(count), '#'));
  }
  st::print("\n最差 8 处：\n");
  for (const auto& [delta, index] : worst) {
    if (index < 0) continue;
    st::print("  ({:3},{:3}) Δ{:<4} 软件 #{:08X}  GPU #{:08X}\n", index % kWidth, index / kWidth, delta,
              left[static_cast<std::size_t>(index)], right[static_cast<std::size_t>(index)]);
  }

  // 元素定位：把每个元素的包围盒列出来，与上面非零行对齐看是谁在 y=75 / y=124
  st::print("\n元素包围盒（按 y 排序）：\n");
  std::vector<std::pair<Rect, std::string>> boxes;
  const auto walk = [&](const st::ui::Element& element, auto&& self) -> void {
    boxes.emplace_back(element.bounds(), std::string(element.type()));
    for (const auto& child : element.children()) {
      if (child != nullptr) self(*child, self);
    }
  };
  walk(root, walk);
  std::ranges::sort(boxes, {}, [](const auto& entry) { return entry.first.y; });
  for (const auto& [bounds, type] : boxes) {
    st::print("  y={:6.1f} h={:5.1f}  x={:6.1f} w={:6.1f}  {}\n", static_cast<double>(bounds.y),
              static_cast<double>(bounds.height), static_cast<double>(bounds.x),
              static_cast<double>(bounds.width), type);
  }
  (void)kBackground;
  return 0;
}
