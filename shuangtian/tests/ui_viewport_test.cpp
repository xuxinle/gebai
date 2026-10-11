/// `st::ui::Viewport` 二维缩放/平移视口测试。
///
/// 核心判据是**锚点不变量**：以某点缩放后，该点下的内容坐标不变。
/// 这条不成立的话「滚轮缩放」就是不好用的（整幅图以左上角为心缩放，
/// 用户得一边滚一边拖）——所以它是本文件的第一条用例，而不是观感项。
///
/// ⚠ 视口的 `bounds` 刻意**不在原点**（见 `Fixture`）：坐标换算的 bug 在原点处会被
/// 0 掩盖（`x - 0 == x`），这是这类组件最常见的假绿来源。

#include <cmath>
#include <memory>

#include "st/app/text_port.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/viewport.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Point;
using st::math::Rect;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::RenderContext;
using st::ui::Theme;
using st::ui::Viewport;

/// 内容占位（一个固定尺寸的 Panel：视口只关心它的 `measured_size`）。
constexpr float kContentW = 400.0f;
constexpr float kContentH = 300.0f;
/// 视口在"屏幕"上的位置：**非原点**（见文件头）。
constexpr float kOriginX = 37.0f;
constexpr float kOriginY = 11.0f;
constexpr float kViewW = 200.0f;
constexpr float kViewH = 150.0f;

struct Fixture {
  Theme theme{Theme::light()};
  std::shared_ptr<st::text::FontStack> stack{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<st::ui::TextPort> port{};
  Viewport viewport{};
  RenderContext context{theme, nullptr, 0.0};

  Fixture() {
    auto loaded = st::text::FontStack::system_default();
    if (loaded.has_value()) {
      stack = std::make_shared<st::text::FontStack>(std::move(*loaded));
      renderer = std::make_unique<st::text::TextRenderer>(*stack);
      port = st::app::make_text_port(*renderer);
      context.text = port.get();
    }
    auto content = std::make_unique<st::ui::Panel>();
    content->set_id("content");
    content->style().width = kContentW;
    content->style().height = kContentH;
    viewport.set_id("viewport");
    viewport.set_content(std::move(content));
    viewport.apply_theme(theme);
  }

  void layout() {
    viewport.measure(context, st::ui::Constraints{kViewW, kViewH});
    viewport.arrange(context, Rect{kOriginX, kOriginY, kViewW, kViewH});
  }

  /// 视口坐标（相对视口左上角）→ 屏幕坐标。
  [[nodiscard]] static auto screen(float local_x, float local_y) -> Point {
    return Point{kOriginX + local_x, kOriginY + local_y};
  }
};

[[nodiscard]] auto near(float a, float b, float tolerance = 1.0e-3f) -> bool {
  return std::fabs(a - b) <= tolerance;
}

}  // namespace

ST_TEST(viewport_coordinate_round_trip) {
  Fixture fx;
  fx.layout();
  // 视口坐标 → 内容坐标 → 视口坐标：必须原样回来。
  for (const auto& point : {Point{0.0f, 0.0f}, Point{10.0f, 20.0f}, Point{199.0f, 149.0f}}) {
    const Point content = fx.viewport.to_content(point);
    const Point back = fx.viewport.to_view(content);
    ST_CHECK(near(back.x, point.x));
    ST_CHECK(near(back.y, point.y));
  }
  // 缩放 2 倍后同样成立（换算里最容易漏的就是 zoom 的两次除法/乘法）。
  fx.viewport.set_zoom(2.0f);
  fx.layout();
  const Point content = fx.viewport.to_content(Point{40.0f, 30.0f});
  const Point back = fx.viewport.to_view(content);
  ST_CHECK(near(back.x, 40.0f));
  ST_CHECK(near(back.y, 30.0f));
  // 平移也参与换算。
  fx.viewport.set_pan(Point{15.0f, -7.0f});
  fx.layout();
  const Point content2 = fx.viewport.to_content(Point{12.0f, 9.0f});
  const Point back2 = fx.viewport.to_view(content2);
  ST_CHECK(near(back2.x, 12.0f));
  ST_CHECK(near(back2.y, 9.0f));
}

ST_TEST(viewport_zoom_at_keeps_anchor_content_fixed) {
  // **本组件的核心判据**：以某点为锚缩放的目的是「光标下的那块内容不动」。
  Fixture fx;
  fx.layout();
  const Point anchor{kViewW * 0.5f, kViewH * 0.5f};
  const Point before = fx.viewport.to_content(anchor);

  fx.viewport.zoom_at(anchor, 4.0f);
  fx.layout();
  const Point after = fx.viewport.to_content(anchor);
  ST_CHECK(near(after.x, before.x, 1.0e-2f));
  ST_CHECK(near(after.y, before.y, 1.0e-2f));
  ST_CHECK(near(fx.viewport.zoom(), 4.0f));

  // 锚点不在中心时同样成立（中心点会让"以左上角为心"的错实现也碰巧通过一半）。
  const Point corner{8.0f, 5.0f};
  const Point corner_before = fx.viewport.to_content(corner);
  fx.viewport.zoom_at(corner, 1.0f / 4.0f);
  fx.layout();
  const Point corner_after = fx.viewport.to_content(corner);
  ST_CHECK(near(corner_after.x, corner_before.x, 1.0e-2f));
  ST_CHECK(near(corner_after.y, corner_before.y, 1.0e-2f));
}

ST_TEST(viewport_zoom_is_clamped_and_does_not_drift_at_bounds) {
  // 到边界后**不得再推 pan**：夹取之后仍按未夹取的倍率推 pan，
  // 表现是「缩放到头了，图还在慢慢跑」——这是最容易漏掉的一条。
  Fixture fx;
  fx.layout();
  fx.viewport.set_zoom(1.0e6f);
  ST_CHECK(near(fx.viewport.zoom(), Viewport::kMaxZoom));
  fx.viewport.set_zoom(0.0f);
  ST_CHECK(near(fx.viewport.zoom(), Viewport::kMinZoom));
  fx.viewport.set_zoom(-5.0f);
  ST_CHECK(near(fx.viewport.zoom(), Viewport::kMinZoom));

  // 顶到上限后连续 zoom_at：pan 必须纹丝不动。
  fx.viewport.set_zoom(Viewport::kMaxZoom);
  fx.layout();
  const Point pan_at_limit = fx.viewport.pan();
  for (int i = 0; i < 5; ++i) {
    fx.viewport.zoom_at(Point{30.0f, 20.0f}, 2.0f);
  }
  ST_CHECK(near(fx.viewport.zoom(), Viewport::kMaxZoom));
  ST_CHECK(near(fx.viewport.pan().x, pan_at_limit.x));
  ST_CHECK(near(fx.viewport.pan().y, pan_at_limit.y));

  // 下限同理。
  fx.viewport.set_zoom(Viewport::kMinZoom);
  fx.layout();
  const Point pan_at_min = fx.viewport.pan();
  for (int i = 0; i < 5; ++i) {
    fx.viewport.zoom_at(Point{30.0f, 20.0f}, 0.5f);
  }
  ST_CHECK(near(fx.viewport.zoom(), Viewport::kMinZoom));
  ST_CHECK(near(fx.viewport.pan().x, pan_at_min.x));
  ST_CHECK(near(fx.viewport.pan().y, pan_at_min.y));
}

ST_TEST(viewport_wheel_zooms_around_pointer_and_middle_drag_pans) {
  Fixture fx;
  fx.layout();
  // 滚轮在视口内的某点向上：以**该点**为锚放大（不是视口中心）。
  const Point pointer = Fixture::screen(120.0f, 90.0f);
  const Point anchor_before = fx.viewport.to_content(Point{120.0f, 90.0f});
  Event wheel;
  wheel.kind = EventKind::Wheel;
  wheel.position = pointer;
  wheel.wheel_delta = 1.0f;
  ST_CHECK(fx.viewport.on_event(fx.context, wheel));
  ST_CHECK(fx.viewport.zoom() > 1.0f);
  const Point anchor_after = fx.viewport.to_content(Point{120.0f, 90.0f});
  ST_CHECK(near(anchor_after.x, anchor_before.x, 1.0e-2f));
  ST_CHECK(near(anchor_after.y, anchor_before.y, 1.0e-2f));

  // 左键按下不归视口（留给内容自己用）；中键才起平移。
  Event left;
  left.kind = EventKind::MouseDown;
  left.button = 1;
  left.position = pointer;
  ST_CHECK(!fx.viewport.on_event(fx.context, left));

  Event middle;
  middle.kind = EventKind::MouseDown;
  middle.button = 3;
  middle.position = pointer;
  ST_CHECK(fx.viewport.on_event(fx.context, middle));
  const Point pan_before = fx.viewport.pan();
  const float zoom = fx.viewport.zoom();
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = Point{pointer.x + 20.0f, pointer.y + 10.0f};
  ST_CHECK(fx.viewport.on_event(fx.context, move));
  // 跟手：拖动 (20,10) 屏幕像素，内容坐标里的 pan 变化 = 位移 / zoom（方向相反）。
  ST_CHECK(near(fx.viewport.pan().x, pan_before.x - 20.0f / zoom, 1.0e-2f));
  ST_CHECK(near(fx.viewport.pan().y, pan_before.y - 10.0f / zoom, 1.0e-2f));

  Event up;
  up.kind = EventKind::MouseUp;
  up.button = 3;
  up.position = move.position;
  ST_CHECK(fx.viewport.on_event(fx.context, up));
  // 松开之后再移：不再改 pan。
  const Point pan_after_up = fx.viewport.pan();
  Event move2;
  move2.kind = EventKind::MouseMove;
  move2.position = Point{pointer.x + 60.0f, pointer.y + 60.0f};
  ST_CHECK(!fx.viewport.on_event(fx.context, move2));
  ST_CHECK(near(fx.viewport.pan().x, pan_after_up.x));
  ST_CHECK(near(fx.viewport.pan().y, pan_after_up.y));
}

ST_TEST(viewport_fit_centers_content_without_upscaling) {
  Fixture fx;
  fx.layout();
  // 内容 400x300 放进 200x150 视口：适配倍率 = min(0.5, 0.5) = 0.5。
  fx.viewport.fit_to_view();
  ST_CHECK(near(fx.viewport.zoom(), 0.5f, 1.0e-3f));
  // 居中：视口中心对应的内容点 = 内容中心。
  const Point center = fx.viewport.to_content(Point{kViewW * 0.5f, kViewH * 0.5f});
  ST_CHECK(near(center.x, kContentW * 0.5f, 0.5f));
  ST_CHECK(near(center.y, kContentH * 0.5f, 0.5f));

  // 小内容不被放大（`fit` 是"刚好看全"，不是"铺满"）。
  auto small = std::make_unique<st::ui::Panel>();
  small->set_id("small");
  small->style().width = 40.0f;
  small->style().height = 30.0f;
  fx.viewport.set_content(std::move(small));
  fx.layout();
  fx.viewport.fit_to_view();
  ST_CHECK(near(fx.viewport.zoom(), 1.0f));
}

ST_TEST(viewport_properties_and_actions) {
  Fixture fx;
  fx.layout();
  // 属性面：控制通道与 XML 靠它设视图状态。
  ST_CHECK(fx.viewport.set_property("zoom", "2.5"));
  ST_CHECK(near(fx.viewport.zoom(), 2.5f, 1.0e-3f));
  ST_CHECK(fx.viewport.set_property("pan_x", "12"));
  ST_CHECK(near(fx.viewport.pan().x, 12.0f));
  ST_CHECK(fx.viewport.set_property("checkerboard", "false"));
  ST_CHECK(!fx.viewport.checkerboard());
  // 非法值必须**如实拒绝**（静默当成 0/false 会让"设错了"看起来像"功能坏了"）。
  ST_CHECK(!fx.viewport.set_property("zoom", "abc"));
  ST_CHECK(!fx.viewport.set_property("zoom", "2.5x"));
  ST_CHECK(!fx.viewport.set_property("checkerboard", "yes"));
  ST_CHECK(near(fx.viewport.zoom(), 2.5f, 1.0e-3f));   // 拒绝后原值不变

  const auto zoom = fx.viewport.get_property("zoom");
  ST_REQUIRE(zoom.has_value());
  ST_CHECK(zoom->starts_with("2.5"));
  ST_REQUIRE(fx.viewport.get_property("checkerboard").has_value());
  ST_CHECK_EQ(*fx.viewport.get_property("checkerboard"), std::string("false"));

  // 动作面：键盘/脚本路径（无鼠标位置 → 以视口中心为锚）。
  ST_CHECK(fx.viewport.invoke_action("zoom_in", {}));
  ST_CHECK(fx.viewport.zoom() > 2.5f);
  ST_CHECK(fx.viewport.invoke_action("reset", {}));
  ST_CHECK(near(fx.viewport.zoom(), 1.0f));
  ST_CHECK(near(fx.viewport.pan().x, 0.0f));
  ST_CHECK(fx.viewport.invoke_action("fit", {}));
  ST_CHECK(!fx.viewport.invoke_action("不存在的动作", {}));
}

ST_TEST(viewport_checkerboard_changes_pixels_and_follows_content) {
  // 棋盘格开关必须**真的影响画面**（只断言 setter 存下来是假绿）；
  // 且它锚在**内容坐标系**：平移时格子跟着内容走（否则看起来像"桌面在穿透图片"）。
  //
  // ⚠ 取样点必须落在**内容之外**：棋盘格是"内容下面那层透明底"，内容有自己的背景
  // （`Panel` 默认铺底），在内容区域内取样量到的是内容的颜色、与格子无关。
  // 实测踩到：第一版在 (5,5) 取样，开关格子画面完全不变——那不是组件坏了。
  Fixture fx;
  fx.layout();
  // 缩到 0.5：内容只有 200x150，视口右侧与下方留出空白（那里才是"没有内容"）。
  fx.viewport.set_zoom(0.5f);
  fx.layout();

  const int probe_x = static_cast<int>(kOriginX + kViewW - 12.0f);
  const int probe_y = static_cast<int>(kOriginY + kViewH - 12.0f);
  const auto sample = [&fx, probe_x, probe_y]() {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(400, 300, 1.0f);
    canvas.clear(st::math::Color::rgb(255, 255, 255));
    fx.viewport.paint(fx.context, canvas);
    return canvas.pixel_at(probe_x, probe_y);
  };

  const auto with_checker = sample();
  fx.viewport.set_checkerboard(false);
  const auto without_checker = sample();
  ST_CHECK(with_checker != without_checker);

  // 再开回来：与基准一致（防「只是画了别的东西」）。
  fx.viewport.set_checkerboard(true);
  ST_CHECK(sample() == with_checker);

  // 平移半个格子：格子的相位跟着内容变（锚在内容坐标系）。
  // 判据用**整片空白的指纹**（单点可能恰好落在同色格上）。
  const auto region_hash = [&fx]() {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(400, 300, 1.0f);
    canvas.clear(st::math::Color::rgb(255, 255, 255));
    fx.viewport.paint(fx.context, canvas);
    std::size_t hash = 1469598103934665603ULL;
    for (int y = static_cast<int>(kOriginY + kViewH) - 40; y < static_cast<int>(kOriginY + kViewH) - 2;
         ++y) {
      for (int x = static_cast<int>(kOriginX + kViewW) - 40;
           x < static_cast<int>(kOriginX + kViewW) - 2; ++x) {
        hash = (hash ^ canvas.pixel_at(x, y).to_hex()) * 1099511628211ULL;
      }
    }
    return hash;
  };
  const std::size_t before = region_hash();
  fx.viewport.set_pan(st::math::Point{fx.viewport.checker_size() * 0.5f, 0.0f});
  fx.layout();
  ST_CHECK(region_hash() != before);
}
