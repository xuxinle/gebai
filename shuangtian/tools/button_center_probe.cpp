// 按钮居中的**真机口径**探针：用真实字体端口渲染按钮内容，量墨迹包围盒。
//
// 桩（`tests/ui_button_center_test.cpp`）能钉住几何推导，但量不到「字体度量与实际墨迹
// 不一致」这类问题——那正是真机上纵向偏 1~2px 的嫌疑来源。本探针用真字体，
// 把「按 ink_metrics 推出来的墨迹中心」与「图上实际的墨迹中心」并排打出来。
//
// 判据：纵向偏移 |Δy| ≤ 1.0 px（超过即「字体度量与实际墨迹不一致」）。
// 横向不判——比例字体的末字右侧留白本就使墨迹中心偏离框中心，那是排版事实而非缺陷。

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
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
  const st::probe::spec spec{
      .title = "按钮居中：按 ink_metrics 推出的墨迹中心 vs 图上实际墨迹中心",
      .criterion = "纵向偏移 |Δy| ≤ 1.0 px",
      .usage_args = "[font_size]",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("button_center_probe", spec, opts)) return st::probe::exit_ok;
  // 参数非法不再抛（`std::stof` 越界/非数字是 UB 或异常）：缺失即取缺省值。
  const float size = opts.positional_int(0) ? static_cast<float>(*opts.positional_int(0)) : 14.0f;

  auto loaded = st::text::FontStack::system_default();
  if (!loaded || loaded->empty()) {
    // **不能返回 0**：字体栈不可用是「没测成」而非「测过且合格」。
    // 返回 0 会让 CI 把「本机没字体」当成「居中没问题」——正是本仓反复出现的假绿。
    return st::probe::main_of(spec, opts, st::probe::skip("字体栈不可用（本机无可用系统字体）"));
  }
  st::probe::result verdict{};
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
    st::log::out("[{}] 字号 {:.1f}\n", item.tag, static_cast<double>(size));
    st::log::out("   行盒 {:.2f} · shaped_ascent {:.2f} · ink above/below {:.2f}/{:.2f}\n",
              static_cast<double>(port->line_height(size)),
              static_cast<double>(port->shaped_ascent(item.label, size)),
              metrics ? static_cast<double>(metrics->above) : -1.0,
              metrics ? static_cast<double>(metrics->below) : -1.0);
    st::log::out("   框中心 y={:.2f} · 图上墨迹 y=[{:.2f},{:.2f}] 中心 {:.2f} → 偏移 {:+.3f}\n",
              static_cast<double>(box.center().y), static_cast<double>(ink.y0),
              static_cast<double>(ink.y1), static_cast<double>(ink.cy()),
              static_cast<double>(ink.cy() - box.center().y));
    st::log::out("   横向：框中心 x={:.2f} · 墨迹 x=[{:.2f},{:.2f}] 中心 {:.2f} → 偏移 {:+.3f}\n",
              static_cast<double>(box.center().x), static_cast<double>(ink.x0),
              static_cast<double>(ink.x1), static_cast<double>(ink.cx()),
              static_cast<double>(ink.cx() - box.center().x));
    // 只判纵向：横向的偏移来自比例字体末字右侧留白（排版事实，不是缺陷）。
    const double dy = static_cast<double>(ink.cy() - box.center().y);
    verdict.add(std::string{item.tag} + " 纵向偏移", std::abs(dy) <= 1.0,
                std::format("Δy={:+.3f}px", dy));
  }
  verdict.conclude();
  return st::probe::main_of(spec, opts, verdict);
}
