/// `st::ui::ColorPicker` 取色器测试。
///
/// 重点是**两个最容易错的地方**：
/// ① HSV↔RGB 往返（尤其灰阶——色相在数学上无定义，实现很容易在这里"悄悄换色"）；
/// ② 十六进制解析（非法输入必须**如实拒绝并保留原值**——静默兜底成黑色是这类控件
///    最糟的失败方式：用户以为设上了，画面却是黑的，而两者之间没有任何提示）。

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#include "st/app/text_port.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/color_picker.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::ui::ColorPicker;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::RenderContext;
using st::ui::Theme;

constexpr float kOriginX = 23.0f;
constexpr float kOriginY = 17.0f;

struct Fixture {
  Theme theme{Theme::light()};
  std::shared_ptr<st::text::FontStack> stack{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<st::ui::TextPort> port{};
  ColorPicker picker{};
  RenderContext context{theme, nullptr, 0.0};

  Fixture() {
    auto loaded = st::text::FontStack::system_default();
    if (loaded.has_value()) {
      stack = std::make_shared<st::text::FontStack>(std::move(*loaded));
      renderer = std::make_unique<st::text::TextRenderer>(*stack);
      port = st::app::make_text_port(*renderer);
      context.text = port.get();
    }
    picker.set_id("picker");
    picker.apply_theme(theme);
  }

  void layout() {
    picker.measure(context, st::ui::Constraints{ColorPicker::kDefaultWidth,
                                               ColorPicker::kDefaultHeight});
    picker.arrange(context, st::math::Rect{kOriginX, kOriginY, ColorPicker::kDefaultWidth,
                                           ColorPicker::kDefaultHeight});
  }
};

[[nodiscard]] auto near(float a, float b, float tolerance) -> bool {
  return std::fabs(a - b) <= tolerance;
}

/// 逐通道比较（容差 2：四舍五入的往复误差应远小于此）。
[[nodiscard]] auto color_close(Color lhs, Color rhs, int tolerance = 2) -> bool {
  const auto diff = [](std::uint8_t a, std::uint8_t b) {
    return std::abs(static_cast<int>(a) - static_cast<int>(b));
  };
  return diff(lhs.r, rhs.r) <= tolerance && diff(lhs.g, rhs.g) <= tolerance &&
         diff(lhs.b, rhs.b) <= tolerance;
}

}  // namespace

ST_TEST(color_picker_hsv_round_trip_covers_primaries_and_gray) {
  Fixture fx;
  fx.layout();
  // 六个色相节点 + 三档明度：往返后 RGB 必须不变（这是 HSV 交互能不能用的地基）。
  for (const auto& original : {Color::rgb(255, 0, 0),     // 0°
                               Color::rgb(255, 255, 0),   // 60°
                               Color::rgb(0, 255, 0),     // 120°
                               Color::rgb(0, 255, 255),   // 180°
                               Color::rgb(0, 0, 255),     // 240°
                               Color::rgb(255, 0, 255),   // 300°
                               Color::rgb(128, 64, 32),   // 任意中间色
                               Color::rgb(1, 2, 3)}) {    // 接近黑
    fx.picker.set_color(original);
    ST_CHECK(color_close(fx.picker.color(), original));
    // 用 HSV 再设回去（模拟"用户拖动一下又拖回原处"）也应当一致。
    const float h = fx.picker.hue();
    const float s = fx.picker.saturation();
    const float v = fx.picker.value();
    fx.picker.set_hsv(h, s, v);
    ST_CHECK(color_close(fx.picker.color(), original));
    // 色相在 [0,360)
    ST_CHECK(fx.picker.hue() >= 0.0f);
    ST_CHECK(fx.picker.hue() < 360.0f);
  }

  // 灰阶：色相在数学上无定义。要求**往返后仍是灰**（R==G==B），
  // 且色相不被冲成 0——那样用户把 V 拉回来会得到红色而不是他原来的颜色。
  fx.picker.set_color(Color::rgb(120, 30, 200));   // 先给一个明确的色相
  const float hue_before = fx.picker.hue();
  fx.picker.set_hsv(fx.picker.hue(), 0.0f, 0.5f);  // 拖到饱和度 0（灰）
  const Color gray = fx.picker.color();
  ST_CHECK(gray.r == gray.g);
  ST_CHECK(gray.g == gray.b);
  ST_CHECK(near(fx.picker.hue(), hue_before, 1.0f));   // 色相保住了
  // 再把饱和度拉回来：应当回到**同一个色相族**——具体 RGB 不必等于原色，
  // 因为 `set_hsv(h, 1, 1)` 表达的是「该色相的纯色」，而原色是 `hsv(h, s0, v0)`
  // 的一个中间态（S/V 都被显式改过）。判据用色相而非 RGB。
  const float hue_restored_before = fx.picker.hue();
  fx.picker.set_hsv(hue_restored_before, 1.0f, 1.0f);
  // 纯色相族：该色相的 S=1、V=1，其最大通道必须是 255。
  const Color pure = fx.picker.color();
  ST_CHECK(std::max({pure.r, pure.g, pure.b}) == 255);
  ST_CHECK(near(fx.picker.hue(), hue_before, 1.0f));
}

ST_TEST(color_picker_hex_parse_accepts_six_and_eight_digits) {
  Fixture fx;
  fx.layout();
  ST_CHECK(fx.picker.set_hex_text("#FF8800"));
  ST_CHECK(fx.picker.color() == Color::rgb(255, 136, 0));
  ST_CHECK_EQ(fx.picker.hex_text(), std::string("#FF8800"));   // 规范化回显

  // 小写与省略 `#` 都接受。
  ST_CHECK(fx.picker.set_hex_text("ff8800"));
  ST_CHECK(fx.picker.color() == Color::rgb(255, 136, 0));
  ST_CHECK(fx.picker.set_hex_text("00ff00"));
  ST_CHECK(fx.picker.color() == Color::rgb(0, 255, 0));

  // 八位 = RRGGBBAA，**不按数值大小猜**：
  // `#0000008C` 必须被读成"黑色 55% 透明"，而不是 `color.hpp` 里记的那个坑
  //（按数值大小猜会变成纯蓝不透明）。
  ST_CHECK(fx.picker.set_hex_text("#0000008C"));
  ST_CHECK(fx.picker.color() == Color::rgba(0, 0, 0, 0x8C));
  ST_CHECK_EQ(fx.picker.hex_text(), std::string("#0000008C"));
}

ST_TEST(color_picker_hex_rejects_invalid_and_keeps_previous) {
  Fixture fx;
  fx.layout();
  ST_CHECK(fx.picker.set_hex_text("#123456"));
  const Color previous = fx.picker.color();

  // 各种非法输入：全部必须**如实拒绝**，且颜色不变。
  for (const std::string_view bad : {"", "#", "12", "#12345", "#1234567", "#123456789", "#GGGGGG",
                                     "#12 456", "0x123456", "#abc"}) {
    ST_CHECK(!fx.picker.set_hex_text(bad));
    ST_CHECK(fx.picker.hex_invalid());
    ST_CHECK(fx.picker.color() == previous);   // 保留原值（不是静默变黑）
  }

  // 合法输入清掉错误标记。
  ST_CHECK(fx.picker.set_hex_text("#654321"));
  ST_CHECK(!fx.picker.hex_invalid());
  ST_CHECK(fx.picker.color() == Color::rgb(0x65, 0x43, 0x21));
}

ST_TEST(color_picker_on_change_fires_for_user_input_only) {
  Fixture fx;
  fx.layout();
  int calls = 0;
  Color last{};
  fx.picker.on_change = [&](Color value) {
    ++calls;
    last = value;
  };

  // 程序设色（`notify = false`）**不**触发回调：否则「把当前颜色同步进控件」
  // 会再触发一次应用逻辑，用户操作与程序同步就混在一起了。
  fx.picker.set_color(Color::rgb(10, 20, 30));
  ST_CHECK_EQ(calls, 0);
  // 显式要求通知时才触发。
  fx.picker.set_color(Color::rgb(10, 20, 31), true);
  ST_CHECK_EQ(calls, 1);
  ST_CHECK(last == Color::rgb(10, 20, 31));

  // 同一颜色再设一次：不该重复触发（相等早退）。
  fx.picker.set_color(Color::rgb(10, 20, 31), true);
  ST_CHECK_EQ(calls, 1);
}

ST_TEST(color_picker_properties_expose_and_accept_color) {
  Fixture fx;
  fx.layout();
  // 属性面必须报 `color`（控制通道与 XML 都靠它设色）。
  const auto names = fx.picker.property_names();
  bool has_color = false;
  for (const auto name : names) {
    if (name == "color") has_color = true;
  }
  ST_CHECK(has_color);

  ST_CHECK(fx.picker.set_property("color", "#336699"));
  ST_CHECK(fx.picker.color() == Color::rgb(0x33, 0x66, 0x99));
  const auto reported = fx.picker.get_property("color");
  ST_REQUIRE(reported.has_value());
  ST_CHECK_EQ(*reported, std::string("#336699"));
  // 语义值同源（可访问性/自动化断言用）。
  ST_CHECK_EQ(fx.picker.semantics_value(), std::string("#336699"));

  // 非法值经属性面同样被拒绝（不静默生效）。
  ST_CHECK(!fx.picker.set_property("color", "not-a-color"));
  ST_CHECK(fx.picker.color() == Color::rgb(0x33, 0x66, 0x99));

  // HSV 属性可读写（供"色相环"这类外部控件联动）。
  ST_CHECK(fx.picker.set_property("saturation", "0.5"));
  ST_CHECK(near(fx.picker.saturation(), 0.5f, 1.0e-3f));
  ST_CHECK(!fx.picker.set_property("saturation", "abc"));
  // 动作面：`set_color` 与属性面同一条路。
  ST_CHECK(fx.picker.invoke_action("set_color", "#FF0000"));
  ST_CHECK(fx.picker.color() == Color::rgb(255, 0, 0));
  ST_CHECK(!fx.picker.invoke_action("set_color", "#XYZ"));
}

ST_TEST(color_picker_eyedropper_disabled_without_source) {
  Fixture fx;
  fx.layout();
  // 无源时**如实禁用**（不假装可用）。
  ST_CHECK(!fx.picker.has_eyedropper());
  ST_CHECK(!fx.picker.invoke_action("eyedropper", {}));
  const auto reported = fx.picker.get_property("eyedropper");
  ST_REQUIRE(reported.has_value());
  ST_CHECK_EQ(*reported, std::string("false"));
  // 点吸管区域也不该被吞掉（无源时它就是一个普通位置）。
  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = Point{fx.picker.eyedropper_rect().center().x,
                        fx.picker.eyedropper_rect().center().y};
  ST_CHECK(!fx.picker.on_event(fx.context, down));

  // 给了源就能取色，并且走 `on_change`（用户操作路径）。
  fx.picker.set_eyedropper_source([](Point) -> std::optional<Color> {
    return Color::rgb(7, 8, 9);
  });
  ST_CHECK(fx.picker.has_eyedropper());
  int calls = 0;
  fx.picker.on_change = [&](Color) { ++calls; };
  ST_CHECK(fx.picker.invoke_action("eyedropper", {}));
  ST_CHECK(fx.picker.color() == Color::rgb(7, 8, 9));
  ST_CHECK_EQ(calls, 1);
}

ST_TEST(color_picker_mouse_drag_updates_hsv) {
  Fixture fx;
  fx.layout();
  // 点 SV 块左上角 = 饱和度 0、明度 1（白）。
  const st::math::Rect sv = fx.picker.sv_square_rect();
  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = Point{sv.x + 1.0f, sv.y + 1.0f};
  ST_CHECK(fx.picker.on_event(fx.context, down));
  ST_CHECK(near(fx.picker.saturation(), 0.0f, 0.02f));
  ST_CHECK(near(fx.picker.value(), 1.0f, 0.02f));

  // 拖到右下角 = 饱和度 1、明度 0（黑）。
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = Point{sv.right() - 1.0f, sv.bottom() - 1.0f};
  ST_CHECK(fx.picker.on_event(fx.context, move));
  ST_CHECK(near(fx.picker.saturation(), 1.0f, 0.02f));
  ST_CHECK(near(fx.picker.value(), 0.0f, 0.02f));
  Event up;
  up.kind = EventKind::MouseUp;
  up.position = move.position;
  ST_CHECK(fx.picker.on_event(fx.context, up));
  // 松开后再移：不再改 HSV。
  const float s_after = fx.picker.saturation();
  Event move2;
  move2.kind = EventKind::MouseMove;
  move2.position = Point{sv.x + 1.0f, sv.y + 1.0f};
  ST_CHECK(!fx.picker.on_event(fx.context, move2));
  ST_CHECK(near(fx.picker.saturation(), s_after, 1.0e-4f));

  // 色相条：点到中间 → 约 180°。
  const st::math::Rect bar = fx.picker.hue_bar_rect();
  Event hue_down;
  hue_down.kind = EventKind::MouseDown;
  hue_down.button = 1;
  hue_down.position = Point{bar.x + bar.width * 0.5f, bar.y + bar.height * 0.5f};
  ST_CHECK(fx.picker.on_event(fx.context, hue_down));
  ST_CHECK(near(fx.picker.hue(), 180.0f, 3.0f));
  Event hue_up;
  hue_up.kind = EventKind::MouseUp;
  hue_up.position = hue_down.position;
  ST_CHECK(fx.picker.on_event(fx.context, hue_up));
}

ST_TEST(color_picker_hex_input_keeps_draft_until_valid) {
  Fixture fx;
  fx.layout();
  ST_CHECK(fx.picker.set_hex_text("#101010"));
  const Color base = fx.picker.color();

  // 聚焦输入框后逐字敲：**敲到一半不该改颜色**（否则用户每敲一个字符颜色就乱跳）。
  // ⚠ 组件用**框架焦点**判"键盘输入归不归我"（`TextInput` 只投给 `UiRoot::focused_`）。
  // 这里没有 `UiRoot`（单测直接调 `on_event`），所以显式把焦点设上——
  // 这正是 `MouseDown` 命中输入框时会做的事（它调 `focus()`）。
  fx.picker.set_focused(true);
  Event focus;
  focus.kind = EventKind::MouseDown;
  focus.button = 1;
  focus.position = fx.picker.hex_rect().center();
  ST_CHECK(fx.picker.on_event(fx.context, focus));
  for (const char* chunk : {"#", "F", "F"}) {
    Event text;
    text.kind = EventKind::TextInput;
    text.text = chunk;
    fx.picker.on_event(fx.context, text);
  }
  ST_CHECK(fx.picker.color() == base);   // 半截输入不改色

  // 补到 6 位：即时提交（用户不用按回车）。
  // 注意计数：此时缓冲区是 `#FF`（3 字符），还要 4 个十六进制位才够 6 位数字。
  for (const char* chunk : {"0", "0", "0", "0"}) {
    Event text;
    text.kind = EventKind::TextInput;
    text.text = chunk;
    fx.picker.on_event(fx.context, text);
  }
  ST_CHECK(fx.picker.color() == Color::rgb(255, 0, 0));

  // 退格删掉一位 → 回到"编辑中"，颜色保持（不回退到旧值，那会显得像抖动）。
  Event back;
  back.kind = EventKind::KeyDown;
  back.key = "Backspace";
  ST_CHECK(fx.picker.on_event(fx.context, back));
  ST_CHECK(fx.picker.color() == Color::rgb(255, 0, 0));

  // 非十六进制字符被拒（不吞进缓冲区）。
  Event bad;
  bad.kind = EventKind::TextInput;
  bad.text = "zz";
  ST_CHECK(!fx.picker.on_event(fx.context, bad));
}
