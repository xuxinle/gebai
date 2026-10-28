/// 内阴影（`Shadow::inset`）必须真的把**上内缘**压暗，而不是静默不可见。
///
/// ## 守的是什么（三处都会“不崩、不报错、就是不生效”）
///
/// 内阴影的实现是「外投影的偏移取反 + 裁剪到框内」。这条路径上有三个坑：
/// ① 若把 `covered_region` 传给 `draw_shadow`——那是“随后会被不透明底盖掉”的优化，
///    而内阴影正是要在底**之上**可见的东西，传了就被盖掉，像素上什么都看不到；
/// ② 若把内阴影画在 `fill_rect(background)` **之前**——同样被盖；
/// ③ 若忘了 `push_clip_rounded_rect`——阴影会糊到框外，变成一圈外发光。
/// 三者代码读起来都“像对的”，只能靠量像素发现。
///
/// ## 判据为何是三条一起
///
/// 只断言“上内缘变暗”的话，一个把整块涂暗的实现（等价于换了个更深的底色）也能通过。
/// 所以同时要求：上内缘变暗、深处几乎不变、且框外不受影响。
///
/// ⚠ 必须走真实的 `Element::paint_box`（经 `Panel::paint`），不是自己画一次阴影——
/// 那样测的是“手写的内阴影 vs 不做”，根本没经过被测代码（`CONVENTIONS` §7.2）。

#include "st/test/test.hpp"

#include <cmath>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::raster::Canvas;
using st::ui::Theme;

constexpr int kCanvasW = 320;
constexpr int kCanvasH = 240;

[[nodiscard]] auto make_inset_canvas() -> Canvas {
  Canvas canvas = Canvas::for_logical_size(kCanvasW, kCanvasH, 1.0f);
  canvas.clear(Color::rgb(0xF7, 0xF8, 0xFA));
  return canvas;
}

[[nodiscard]] auto luminance(const Color& c) -> double {
  return 0.2126 * static_cast<double>(c.r) + 0.7152 * static_cast<double>(c.g) +
         0.0722 * static_cast<double>(c.b);
}

/// 画一个 `surface_sunken` 的圆角块，可选是否带内阴影。
[[nodiscard]] auto render_block(const Theme& theme, bool with_inset) -> Canvas {
  const st::math::Rect box{40.0f, 40.0f, 200.0f, 100.0f};
  st::ui::Panel panel;
  panel.style().background = theme.colors().surface_sunken;
  panel.style().radius = 8.0f;
  if (with_inset) {
    panel.style().shadow.color = theme.metrics().sunken_shadow;
    panel.style().shadow.blur = theme.metrics().sunken_shadow_blur;
    panel.style().shadow.offset_y = 1.0f;
    panel.style().shadow.inset = true;
  }
  const st::ui::RenderContext context{theme, nullptr, 0.0};
  panel.arrange(context, box);
  Canvas canvas = make_inset_canvas();
  panel.paint(context, canvas);
  return canvas;
}

}  // namespace

ST_TEST(element_inset_shadow_darkens_only_the_inner_edge) {
  const Theme theme = Theme::dark();
  const Canvas plain = render_block(theme, /*with_inset=*/false);
  const Canvas inset = render_block(theme, /*with_inset=*/true);

  // 取样列取框中，避开圆角；行分别取上内缘、框内深处、以及框外上方。
  constexpr int kCol = 140;
  constexpr int kTopInner = 41;   // 框 y=40，上一行 40 是边本身，取 41
  constexpr int kDeep = 90;       // 框中心
  constexpr int kAbove = 36;      // 框外

  const double plain_top = luminance(plain.pixel_at(kCol, kTopInner));
  const double inset_top = luminance(inset.pixel_at(kCol, kTopInner));
  const double plain_deep = luminance(plain.pixel_at(kCol, kDeep));
  const double inset_deep = luminance(inset.pixel_at(kCol, kDeep));
  const double plain_above = luminance(plain.pixel_at(kCol, kAbove));
  const double inset_above = luminance(inset.pixel_at(kCol, kAbove));
  st::log::out("[inset] 上内缘 {:.1f}→{:.1f}  深处 {:.1f}→{:.1f}  框外 {:.1f}→{:.1f}\n", plain_top,
            inset_top, plain_deep, inset_deep, plain_above, inset_above);

  // ① 上内缘必须变暗——“压进去”的读法就来自这里
  ST_CHECK(inset_top < plain_top);
  // ② 暗化量要够看得出（<1/255 等于没画）
  ST_CHECK(plain_top - inset_top > 1.0);
  // ③ 深处几乎不变：内阴影是光影，不是“换个更深的底色”
  ST_CHECK(std::abs(inset_deep - plain_deep) < 2.0);
  // ④ 框外不受影响：忘了裁剪时这里会变暗（糊成一圈外发光）
  ST_CHECK(std::abs(inset_above - plain_above) < 2.0);
}

ST_TEST(element_inset_shadow_is_off_by_default) {
  // `Shadow::inset` 的缺省值必须是 false——否则所有既有阴影（卡片/弹层）
  // 会突然全部翻到内侧，是**全局观感事故**且没有任何编译期提示。
  const st::ui::Shadow shadow{};
  ST_CHECK(!shadow.inset);
  // `second_visible()` 对 inset 恒为假：内阴影只有一层才有“压下去”的读法。
  st::ui::Shadow two_layer{};
  two_layer.color2 = Color::rgb(0, 0, 0);
  two_layer.color2.a = 40;
  two_layer.blur2 = 8.0f;
  ST_CHECK(two_layer.second_visible());
  two_layer.inset = true;
  ST_CHECK(!two_layer.second_visible());
}
