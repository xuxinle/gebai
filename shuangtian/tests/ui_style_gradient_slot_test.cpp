/// **渐变的体积契约**：`Style::background_gradient` 必须**间接持有**。
///
/// ## 这一组测试守的是什么
///
/// `raster::Gradient` 内含 `std::array<math::Color, 256> lut_` = **1024 字节**查表。
/// 在 2026-11-23 之前 `Style` 用 `std::optional<raster::Gradient>` **内联**持有它，
/// 于是**每个** UI 元素都在身上背着一张 256 项的颜色查找表，哪怕它从不用渐变：
///
/// ```
/// sizeof(Style)    = 1304   （其中 1080 是这一个字段）
/// sizeof(Element)  = 1632   （67% 是这一个字段）
/// ```
///
/// 实测场景的代价：表格 5000 行 = **5.15 MiB** 纯浪费。改为
/// `std::shared_ptr<const raster::Gradient>` 后 `Style` 降到 240、`Element` 降到 560。
///
/// ## 为什么必须有这一条（而不是靠 code review 记住）
///
/// 这是**一个字段的持有方式**，写错时功能完全正常——渐变照画、测试照绿，
/// 只是每个元素多背 1 KB。没有任何行为断言能发现它，只有尺寸断言能。
/// 而尺寸一旦回退（有人为了省一次 `make_shared` 改回内联），本文件立刻变红。
///
/// ## 为什么 `shared_ptr<const>` 而不是 `unique_ptr`
///
/// `Style` 必须**可拷贝**：`Element::RenderExtras::animated_style` 是一份 `Style`，
/// `style() = style` / `paint_style()` 的路径也拷贝它。`unique_ptr` 会让 `Style`
/// 变成只可移动，波及全仓。`const` 则把「渐变是不可变值」变成编译期保证。
/// 共享语义下面是直接测的（拷贝后指向同一份，且互不干扰地清空）。

#include <memory>
#include <utility>

#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/test/test.hpp"
#include "st/ui/element.hpp"
#include "st/ui/style.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::raster::Gradient;

[[nodiscard]] auto red_to_blue() -> Gradient {
  return Gradient::linear(Point{0.0f, 0.0f}, Point{1.0f, 5.0f},
                          {{0.0f, Color{0xFF, 0x00, 0x00, 0xFF}},
                           {1.0f, Color{0x00, 0x00, 0xFF, 0xFF}}});
}

}  // namespace

/// 体积契约：两个 `sizeof` 的上界（本次重构的核心数字，回退即红）。
///
/// 上界取的是 64 位目标（Windows/Linux/macOS 三平台在本仓都是 LP64/LLP64，
/// 同一套布局）：
///   · `Style` ≤ 240：内联渐变时是 1304，这里直接排除那 1080 字节；
///   · `Element` ≤ 560：含 `Style` + 树/交互/语义字段。
ST_TEST(style_gradient_slot_is_indirect) {
  st::log::out("[size] sizeof(Style) = {} / sizeof(Element) = {}\n", sizeof(st::ui::Style),
               sizeof(st::ui::Element));
  ST_CHECK(sizeof(st::ui::Style) <= 240U);
  ST_CHECK(sizeof(st::ui::Element) <= 560U);
  // 反向佐证：被省掉的正是那张查表（若哪天 `Gradient` 自己瘦身了，这条会提示
  // 本文件的注释该更新）。
  ST_CHECK(sizeof(Gradient) >= 1024U);
}

/// 写侧入口必须成对：设纯色要清渐变（否则「渐变优先于纯色」会把纯色盖成残影）。
///
/// 这条与上面的体积改动同一处代码（`set_solid_background` 里 `= nullptr`），
/// 但它守的是**行为**：持有方式换掉后最容易漏改的就是这个清空动作。
ST_TEST(style_solid_background_clears_shared_gradient) {
  st::ui::Style style;
  style.set_background_gradient(red_to_blue());
  ST_REQUIRE(style.background_gradient != nullptr);

  style.set_solid_background(Color{0x33, 0x66, 0x99, 0xFF});
  ST_CHECK(style.background_gradient == nullptr);
  ST_CHECK_EQ(style.background.r, 0x33U);
}

/// `set_gradient_background` 的回退色语义：`background` 是「渐变画不出来时」的兜底。
///
/// 实测踩过（2026-11-10）：不写回退色时 `background` 会停在元素**上一次**的值上，
/// 于是 `gradient_opacity = 0` 的分支把按钮画成**透明底**——而只查 `background`
/// 的调用方看到的是错的东西。改成指针后这条语义必须原样成立。
///
/// 里面还有一条**回归陷阱**（2026-11-23 实测）：本函数（把渐变底整体立起来）
/// 会把 `gradient_opacity` 重置为 1，而 `set_background_gradient`（只换渐变本体）
/// **不能**重置它——否则“先设 `gradient_opacity = 0` 再给渐变”的调用方会静默丢掉那个 0。
/// 两者不可互换，这里把边界钉住。
ST_TEST(style_gradient_setter_keeps_fallback_color) {
  st::ui::Style style;
  style.set_gradient_background(red_to_blue(), Color{0x11, 0x22, 0x33, 0xFF});
  ST_REQUIRE(style.background_gradient != nullptr);
  ST_CHECK_EQ(style.background.r, 0x11U);
  ST_CHECK_EQ(style.background.g, 0x22U);
  ST_CHECK_EQ(style.background.a, 0xFFU);
  ST_CHECK_EQ(style.gradient_opacity, 1.0F);
}

/// 两个写入口的语义边界：`set_background_gradient` **不得**动 `gradient_opacity`。
///
/// 为何要单独一条（实测代价）：有了“只设渐变”的辅助函数后，最自然的写法就是把它
/// 做成“设渐变 + 重置 opacity = 1”（看起来更干净）——而那会让
/// `paint_box_gradient_opacity_is_respected` 从绿变红：该用例先设 `gradient_opacity = 0`
/// 再给渐变，重置后就画出了满强度渐变。环境里没别的东西能拦住这种“顺手加一行”。
ST_TEST(style_gradient_only_setter_leaves_opacity_alone) {
  st::ui::Style style;
  style.gradient_opacity = 0.0F;
  style.set_background_gradient(red_to_blue());
  ST_REQUIRE(style.background_gradient != nullptr);
  ST_CHECK_EQ(style.gradient_opacity, 0.0F);   // 不得被重置成 1
}

/// **拷贝共享同一份渐变**（不是各拷一张 1 KB 查表），且两边各自清空互不影响。
///
/// 为什么这条必须测：`Style` 可拷贝是本方案选 `shared_ptr` 的理由，而“共享”
/// 与“独立”两种实现**行为上都对**——只有量指针才能区分。它同时钉住另一件事：
/// 拷贝方清空渐变不得影响原对象（`shared_ptr` 的 reset 只丢自己那一份引用）。
ST_TEST(style_copies_share_one_gradient_instance) {
  st::ui::Style original;
  original.set_background_gradient(red_to_blue());
  ST_REQUIRE(original.background_gradient != nullptr);
  const Gradient* shared = original.background_gradient.get();

  st::ui::Style copy = original;                       // 拷贝（`animated_style` 的路径）
  ST_REQUIRE(copy.background_gradient != nullptr);
  ST_CHECK(copy.background_gradient.get() == shared);   // 同一份，没有复制查表

  copy.set_solid_background(Color{0x00, 0x00, 0x00, 0xFF});
  ST_CHECK(copy.background_gradient == nullptr);
  ST_REQUIRE(original.background_gradient != nullptr);  // 原对象不受影响
  ST_CHECK(original.background_gradient.get() == shared);
}

/// 拷贝存活于源对象销毁之后（`shared_ptr` 的生命周期保证）。
///
/// 用 `unique_ptr` 也能编译（前提是 `Style` 可移动），但若哪天有人在绘制路径上
/// 存一份 `const Gradient*` 而忘了持有，`Style` 拷贝就会变成悬垂——这条钉住
/// 「拷贝出来的是**自有**的一份所有权」。
ST_TEST(style_gradient_copy_outlives_source) {
  st::ui::Style copy;
  {
    st::ui::Style source;
    source.set_background_gradient(red_to_blue());
    ST_REQUIRE(source.background_gradient != nullptr);
    copy = source;
  }   // source 析构
  ST_REQUIRE(copy.background_gradient != nullptr);
  ST_REQUIRE(copy.background_gradient->stops().size() >= 2);
  ST_CHECK_EQ(copy.background_gradient->stops().front().color.r, 0xFFU);
}

/// 端到端：间接持有的渐变**真的画得出来**（并且盖过纯色底）。
///
/// 只量尺寸不够——`shared_ptr` 若在 `paint_box` 里被误判成空（例如判据漏改一处
/// `has_value()`），尺寸全对、画面全错。这条用像素钉住“画出来了”。
ST_TEST(paint_box_gradient_from_shared_slot_renders) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  st::ui::Style style;
  style.radius = 0.0f;
  style.set_gradient_background(
      Gradient::linear(Point{0.0f, 0.0f}, Point{1.0f, 0.0f},
                       {{0.0f, Color{0xFF, 0x00, 0x00, 0xFF}},
                        {1.0f, Color{0x00, 0x00, 0xFF, 0xFF}}}),
      Color{0x00, 0xFF, 0x00, 0xFF});   // 回退纯色故意用亮绿：被画出来就说明渐变没生效

  st::ui::UiRoot root;
  root.set_theme(theme);
  root.set_viewport({80.0f, 40.0f});
  root.set_text_port(nullptr);
  auto panel = std::make_unique<st::ui::Panel>();
  panel->style() = style;
  root.set_content(std::move(panel));
  root.layout(true);
  Canvas canvas{80, 40};
  canvas.clear(theme.colors().bg);
  root.paint(canvas);

  const Color left = canvas.pixel_at_point(Point{4.0f, 20.0f});
  const Color right = canvas.pixel_at_point(Point{76.0f, 20.0f});
  st::log::out("[gradient] left=({},{},{}) right=({},{},{})\n", left.r, left.g, left.b, right.r,
               right.g, right.b);
  ST_CHECK(left.r > 150 && left.b < 110);
  ST_CHECK(right.b > 150 && right.r < 110);
  // 中点必须是插值色（红蓝都在）——说明画的是渐变而不是两端纯色拼接。
  const Color mid = canvas.pixel_at_point(Point{40.0f, 20.0f});
  ST_CHECK(mid.r > 60 && mid.b > 60);
  // 回退纯色（亮绿）一个像素都不该露出来。
  ST_CHECK(left.g < 80 && mid.g < 120 && right.g < 80);
}
