// 按钮居中的**真机口径**探针：用真实字体端口渲染按钮内容，量墨迹包围盒。
//
// 桩（`tests/ui_button_center_test.cpp`）能钉住几何推导，但量不到「字体度量与实际墨迹
// 不一致」这类问题——那正是真机上纵向偏 1~2px 的嫌疑来源。本探针用真字体，
// 把「按 ink_metrics 推出来的墨迹中心」与「图上实际的墨迹中心」并排打出来。
//
// 用法： button_center_probe [font_size]

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::Button;
using st::ui::RenderContext;
using st::ui::Theme;
using st::ui::UiRoot;

constexpr int kWidth = 320;
constexpr int kHeight = 120;

struct InkBox {
  bool any{false};
  float x0{0}, x1{0}, y0{0}, y1{0};
  [[nodiscard]] auto cx() const -> float { return (x0 + x1) * 0.5f; }
  [[nodiscard]] auto cy() const -> float { return (y0 + y1) * 0.5f; }
};

[[nodiscard]] auto ink_box(const Canvas& canvas, Color background) -> InkBox {
  InkBox box;
  int min_x = canvas.physical_width(), min_y = canvas.physical_height(), max_x = -1, max_y = -1;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      if (canvas.pixel_at(x, y) == background) continue;
      min_x = std::min(min_x, x);
      max_x = std::max(max_x, x);
      min_y = std::min(min_y, y);
      max_y = std::max(max_y, y);
    }
  }
  if (max_x < 0) return box;
  box.any = true;
  box.x0 = static_cast<float>(min_x);
  box.x1 = static_cast<float>(max_x + 1);
  box.y0 = static_cast<float>(min_y);
  box.y1 = static_cast<float>(max_y + 1);
  return box;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const float size = argc > 1 ? std::stof(argv[1]) : 14.0f;
  auto loaded = st::text::FontStack::system_default();
  if (!loaded || loaded->empty()) {
    st::print("字体栈不可用\n");
    return 0;
  }
  auto stack = std::make_shared<st::text::FontStack>(std::move(*loaded));
  auto renderer = std::make_unique<st::text::TextRenderer>(*stack, 1.0f);
  auto port = st::app::make_text_port(*renderer);
  Theme theme{Theme::dark()};
  UiRoot root;
  root.set_theme(theme);
  root.set_viewport(st::math::Size{static_cast<float>(kWidth), static_cast<float>(kHeight)});

  const struct {
    const char* label;
    const char* tag;
  } cases[] = {{"主要操作", "汉字"}, {"Abc", "拉丁"}, {"Abc 汉字", "混排"}};

  const Rect box{20.0f, 30.0f, 160.0f, 40.0f};
  for (const auto& item : cases) {
    auto owned = std::make_unique<Button>(item.label);
    Button* button = owned.get();
    root.set_content(std::move(owned));
    const RenderContext context{theme, port.get(), 0.0};
    button->set_text_size(size);
    button->measure(context, st::ui::Constraints{});
    button->arrange(context, box);

    Canvas canvas{kWidth, kHeight, 1.0f};
    const Color background{0, 0, 0, 0};
    canvas.clear(background);
    button->paint_content(context, canvas);
    const InkBox ink = ink_box(canvas, background);

    const auto metrics = port->ink_metrics(item.label, size);
    st::print("[{}] 字号 {:.1f}\n", item.tag, static_cast<double>(size));
    st::print("   行盒 {:.2f} · shaped_ascent {:.2f} · ink above/below {:.2f}/{:.2f}\n",
              static_cast<double>(port->line_height(size)),
              static_cast<double>(port->shaped_ascent(item.label, size)),
              metrics ? static_cast<double>(metrics->above) : -1.0,
              metrics ? static_cast<double>(metrics->below) : -1.0);
    st::print("   框中心 y={:.2f} · 图上墨迹 y=[{:.2f},{:.2f}] 中心 {:.2f} → 偏移 {:+.3f}\n",
              static_cast<double>(box.center().y), static_cast<double>(ink.y0),
              static_cast<double>(ink.y1), static_cast<double>(ink.cy()),
              static_cast<double>(ink.cy() - box.center().y));
    st::print("   横向：框中心 x={:.2f} · 墨迹 x=[{:.2f},{:.2f}] 中心 {:.2f} → 偏移 {:+.3f}\n",
              static_cast<double>(box.center().x), static_cast<double>(ink.x0),
              static_cast<double>(ink.x1), static_cast<double>(ink.cx()),
              static_cast<double>(ink.cx() - box.center().x));
  }
  return 0;
}
