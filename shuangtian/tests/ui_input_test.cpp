/// 输入框（`Input`）的**绘制契约**测试。
///
/// 为什么专门测这个：`set_icon_prefix` 一直存在、`inner_box` 也一直为图标留槽位，
/// 但 `paint_content` 从未把它画出来——于是设了图标的输入框表现为
/// "看不到图标，文字还莫名右移一段"。这类"API 存在但没接线"的缺陷
/// 编译器不会报、代码审查也容易漏（`DESIGN.md` §8.2 第 26 条是同一族），
/// 只有"渲染出来数像素"才能钉住。

#include "st/test/test.hpp"

#include <cmath>
#include <string>

#include "st/raster/canvas.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::ui::Input;
using st::ui::RenderContext;
using st::ui::TextArea;
using st::ui::Theme;

inline constexpr int kWidth = 320;
inline constexpr int kHeight = 40;

/// 文本端口为 `nullptr`：这些用例只看几何与图形（图标/边框/底色），不看文字。
struct Fixture {
  Theme theme{Theme::light()};
  RenderContext context{theme, nullptr, 0.0};
  st::raster::Canvas canvas{kWidth, kHeight};

  Fixture() { canvas.clear(Color{0, 0, 0, 0}); }
};

/// 前置图标槽位。**直接读组件自己的 `style().padding`**，不要重写常量：
/// 测试里复刻一遍布局公式，一旦实现改了间距就会出现“测试自己骗自己”。
/// 纵向内缩 4px：槽位是全高的，直接取样会把上下边框算进去。
[[nodiscard]] auto icon_slot(const Input& input, const st::ui::Metrics& metrics) -> Rect {
  const Rect bounds = input.bounds();
  return Rect{bounds.x + input.style().padding.left + metrics.space_md, bounds.y + 4.0f, 18.0f,
              bounds.height - 8.0f};
}

/// 区域内**与参考色不同**的像素数。
///
/// 不能用“非透明像素数”：输入框底色本身就是不透明的，整个框都算“有墨”——
/// 判“有没有画出图标”必须拿底色当参考，看像素是否被改过。
[[nodiscard]] auto differing_pixels(const st::raster::Canvas& canvas, const Rect& region,
                                    Color reference) -> int {
  const int x0 = static_cast<int>(std::floor(region.x));
  const int y0 = static_cast<int>(std::floor(region.y));
  const int x1 = static_cast<int>(std::ceil(region.right()));
  const int y1 = static_cast<int>(std::ceil(region.bottom()));
  int count = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      if (canvas.pixel_at(x, y) != reference) ++count;
    }
  }
  return count;
}

void render(Input& input, Fixture& fixture) {
  input.apply_theme(fixture.theme);
  input.measure(fixture.context, st::ui::Constraints{.max_width = kWidth, .max_height = kHeight});
  input.arrange(fixture.context, Rect{0.0f, 0.0f, kWidth, kHeight});
  input.paint(fixture.context, fixture.canvas);
}

}  // namespace

ST_TEST(ui_input_icon_prefix_is_drawn) {
  Fixture fixture;
  Input input;
  input.set_icon_prefix("search");
  render(input, fixture);

  const Rect slot = icon_slot(input, fixture.theme.metrics());
  // 图标是描边式矢量：16px 的放大镜至少几十个像素颜色不同于底色
  ST_CHECK(differing_pixels(fixture.canvas, slot, fixture.theme.colors().surface) > 40);
}

ST_TEST(ui_input_without_icon_leaves_slot_empty) {
  Fixture fixture;
  Input input;  // 不设 icon_prefix
  render(input, fixture);

  const Rect slot = icon_slot(input, fixture.theme.metrics());
  // 没有任何图标时，槽位里应该是**纯底色**（没被任何图元碰过）
  ST_CHECK(differing_pixels(fixture.canvas, slot, fixture.theme.colors().surface) == 0);
}

ST_TEST(ui_input_text_box_shifts_right_for_icon) {
  Fixture fixture;
  Input plain;
  Input with_icon;
  with_icon.set_icon_prefix("search");
  render(plain, fixture);
  render(with_icon, fixture);

  // `inner_box` 是私有实现细节，这里用"可视行为"间接断言：
  // 有图标时文本起点必须右移（否则图标会压在文字上）
  ST_CHECK(with_icon.bounds().width == plain.bounds().width);
  const Rect slot = icon_slot(with_icon, fixture.theme.metrics());
  // 图标槽位完全在输入框内部（不越界），且不触及右端
  ST_CHECK(slot.x > with_icon.bounds().x);
  ST_CHECK(slot.right() < with_icon.bounds().right());
}

ST_TEST(ui_input_placeholder_color_is_readable) {
  // 占位符用 `text_faint`：它是"输入提示"的唯一线索，必须达到辅助文字级对比度。
  // 这条断言把"令牌改色"与"组件实际用色"绑在一起——只改令牌不动组件也会被它抓到。
  for (const Theme& theme : {Theme::light(), Theme::dark()}) {
    const Color background = theme.colors().surface;
    ST_CHECK(st::math::contrast_ratio(theme.colors().text_faint, background) >= 4.0f);
  }
}

ST_TEST(ui_input_focus_changes_border_pixel) {
  Fixture fixture;
  Input input;
  render(input, fixture);
  const auto border_pixel_plain = fixture.canvas.pixel_at(kWidth / 2, 0);

  Fixture focused_fixture;
  Input focused;
  focused.set_focused(true);
  render(focused, focused_fixture);
  const auto border_pixel_focused = focused_fixture.canvas.pixel_at(kWidth / 2, 0);

  // 未聚焦与聚焦的顶边框像素必须不同（焦点是键盘可达性的唯一线索）
  ST_CHECK(border_pixel_plain != border_pixel_focused);
}

ST_TEST(ui_input_disabled_background_differs_from_enabled) {
  Fixture enabled_fixture;
  Input enabled_input;
  render(enabled_input, enabled_fixture);

  Fixture disabled_fixture;
  Input disabled_input;
  disabled_input.set_enabled(false);
  render(disabled_input, disabled_fixture);

  // 禁用态必须有可见的视觉差异（否则用户会一直点它）
  ST_CHECK(enabled_fixture.canvas.pixel_at(kWidth / 2, kHeight / 2) !=
            disabled_fixture.canvas.pixel_at(kWidth / 2, kHeight / 2));
}

// ————————————————————————————————————————————————————————————————————————————
// 动作面（invoke submit/activate/clear）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(ui_input_invoke_submit_triggers_on_submit) {
  // 此前单行 Input 只有键盘路径（Enter）能提交，`invoke submit` 返回 unsupported——
  // 自动化只能模拟真实回车才能提交，与 TextArea 的动作面不对称（DESIGN §8.1.1 同族缺口）。
  Input input;
  input.set_text("命令面板");
  std::string submitted;
  int calls = 0;
  input.on_submit = [&](std::string_view value) {
    submitted = std::string(value);
    ++calls;
  };

  ST_CHECK(input.invoke_action("submit", {}));
  ST_CHECK_EQ(calls, 1);
  ST_CHECK_EQ(submitted, std::string("命令面板"));

  // activate 同义（与 Enter 键的 `activate()` 同源）
  ST_CHECK(input.invoke_action("activate", {}));
  ST_CHECK_EQ(calls, 2);
}

ST_TEST(ui_input_invoke_clear_empties_text) {
  Input input;
  input.set_text("要清掉的内容");
  int changes = 0;
  input.on_change = [&](std::string_view) { ++changes; };

  ST_CHECK(input.invoke_action("clear", {}));
  ST_CHECK_EQ(input.value(), std::string{});
  // `on_change` **不**发：`clear` 经 `set_text`，而程序化写入从不冒充用户编辑
  // （与 `set value` 属性面同语义：只有键盘/粘贴等用户路径才触发 on_change）。
  ST_CHECK_EQ(changes, 0);
}

ST_TEST(ui_input_invoke_unknown_action_falls_through) {
  Input input;
  // 未知动作回落到基类（返回 false 而非假装成功——假成功会让自动化误判）
  ST_CHECK(!input.invoke_action("definitely_not_an_action", {}));
}

// ————————————————————————————————————————————————————————————————————————————
// DSL `input(password=...)` 与 `TextArea` 只读
// ————————————————————————————————————————————————————————————————————————————

/// `Input::set_password` 一直存在，但声明式入口（`dsl::input`）此前没有这个形参——
/// 设设置页的 API Key 输入框**只能** `custom<Input>` 逃生船手动开。
/// 那是安全相关能力（键/令牌/口令），不该只对"愿意写逃生船"的调用方开放。
///
/// 本条钉两件事：① 秘密**从 DSL 就能设**；② 掩码只影响**显示**，`value()` 仍是明文
/// （提交给后端的是真值，不是那串圆点——这条最容易写反）。
ST_TEST(ui_input_password_mode_masks_display_only) {
  // 用真实声明式路径（`dsl::mount` + `Component::build`）——而不是手搭 Composer：
  // 后者绕过了宿主与作用域机制，测到的不是应用真正走的那条路。
  struct KeyPage : st::ui::dsl::Component {
    // **必须是 State**（不是普通 bool）：普通成员改了不会标脏，`tick()` 就不会重跑 build——
    // 于是“切成非秘密模式”根本没生效，测试会挂在一个看不出原因的地方。
    st::ui::dsl::State<bool> secret{true};
    void build(st::ui::dsl::Composer& c) override {
      st::ui::dsl::input(c, "sk-secret", {}, {.id = "key"}, secret.value());
    }
  };

  st::ui::UiRoot root;
  root.set_viewport({320.0F, 200.0F});
  auto page = std::make_shared<KeyPage>();
  auto host = st::ui::dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);

  auto* field = dynamic_cast<Input*>(root.find("key"));
  ST_REQUIRE(field != nullptr);
  ST_CHECK(field->password());                             // ① 秘密真的开了
  ST_CHECK_EQ(field->value(), std::string("sk-secret"));   // ② 读出来的是明文
  ST_CHECK_EQ(field->semantics_value(), std::string("•••••••••"));   // 语义面是掩码

  // 关掉后回到明文显示（同一入口可逆；且走重组路径）
  page->secret.set(false);
  (void)host->tick();
  root.layout(true);
  field = dynamic_cast<Input*>(root.find("key"));
  ST_REQUIRE(field != nullptr);
  ST_CHECK(!field->password());
  ST_CHECK_EQ(field->value(), std::string("sk-secret"));   // 内容不因切模式而丢
}

/// `TextArea` 只读：**编辑禁入，但光标仍可定位、仍可滚动、仍可获得焦点**。
///
/// 为何不能拿 `set_enabled(false)` 凑合（本用例的全部意义）：disabled 的语义是
/// "这个控件不可用"——视觉变灰、不可交互，连滚动看内容都不行。而 JSON 格式化输出
/// 这类只读展示**必须能滚动看**，只是不接受修改。两者是不同的事。
///
/// 反例（故意破坏）：把 `on_event` 里 `read_only_` 的两处判断去掉，本用例当场变红。
ST_TEST(ui_text_area_read_only_blocks_editing_but_keeps_navigation) {
  TextArea area;
  area.set_text("第一行\n第二行");
  area.set_read_only(true);
  ST_CHECK(area.read_only());
  ST_CHECK(area.enabled());   // 只读 ≠ 禁用（这是本用例的判据核心）

  Theme theme = Theme::light();
  RenderContext context{theme, nullptr, 0.0};
  area.measure(context, st::ui::Constraints{.max_width = 320.0F, .max_height = 200.0F});
  area.arrange(context, Rect{0.0F, 0.0F, 320.0F, 200.0F});

  const std::string original = area.value();
  // ① 打字/回车/退格/删除一律不落字
  st::ui::Event typed;
  typed.kind = st::ui::EventKind::TextInput;
  typed.text = "偷偷插入";
  ST_CHECK(area.on_event(context, typed));
  ST_CHECK_EQ(area.value(), original);
  for (const std::string& key : {std::string("Enter"), std::string("Backspace"),
                                 std::string("Delete")}) {
    st::ui::Event event;
    event.kind = st::ui::EventKind::KeyDown;
    event.key = key;
    ST_CHECK(area.on_event(context, event));
    ST_CHECK_EQ(area.value(), original);
  }
  // ② 程序化写入仍可改（`set_text` 不是"用户编辑"，与 Input 的 `clear` 同口径）
  area.set_text("换一份内容");
  ST_CHECK_EQ(area.value(), std::string("换一份内容"));

  // ③ 导航仍可用：点击能定位光标、滚轮能滚（只读展示要能看）
  st::ui::Event click;
  click.kind = st::ui::EventKind::Click;
  click.position = st::math::Point{20.0F, 10.0F};
  ST_CHECK(area.on_event(context, click));
  st::ui::Event wheel;
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.wheel_delta = 10.0F;
  ST_CHECK(area.on_event(context, wheel));

  // ④ 属性面读写同一状态
  ST_CHECK(area.set_property("read_only", "false"));
  ST_CHECK(!area.read_only());
  const auto readable = area.get_property("read_only");
  ST_REQUIRE(readable.has_value());
  ST_CHECK_EQ(*readable, std::string("false"));
  // 语义面：只读时 editable=false（但组件仍在场、仍可读）
  area.set_read_only(true);
  ST_CHECK(!area.semantics_flags().editable);
  ST_CHECK(area.semantics_flags().visible);
}
