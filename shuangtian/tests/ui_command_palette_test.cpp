/// 命令面板组件测试：过滤/键盘导航/执行/属性面与动作面。

#include <memory>
#include <string>

#include "st/test/test.hpp"
#include "st/ui/components/command_palette.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::CommandPalette;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::Theme;
using st::ui::UiRoot;
using st::ui::List;

struct PaletteFixture {
  Theme theme{Theme::light()};
  UiRoot root{};
  st::ui::RenderContext context{theme, nullptr, 0.0};
  CommandPalette* palette{nullptr};
  std::string executed;
  std::string closed;

  PaletteFixture() {
    root.set_theme(theme);
    auto palette_owned = std::make_unique<CommandPalette>();
    palette = palette_owned.get();
    root.add_overlay(std::move(palette_owned), st::ui::UiRoot::OverlayLayout::FillViewport);
    palette->arrange(context, st::math::Rect{0.0f, 0.0f, 1280.0f, 800.0f});
    palette->on_command = [this](std::string_view id) { executed = std::string(id); };
    palette->on_close = [this]() { closed = "yes"; };
  }

  auto command(std::string id, std::string title, std::string detail = {})
      -> CommandPalette::Command {
    CommandPalette::Command item;
    item.id = std::move(id);
    item.title = std::move(title);
    item.detail = std::move(detail);
    return item;
  }
};

auto press_key(CommandPalette& palette, const st::ui::RenderContext& context, std::string key) -> bool {
  st::ui::Event event;
  event.kind = st::ui::EventKind::KeyDown;
  event.key = std::move(key);
  return palette.on_event(context, event);
}

}  // namespace

ST_TEST(palette_lists_all_commands_by_default) {
  PaletteFixture fx;
  fx.palette->set_commands({fx.command("save", "保存", "Ctrl+S"),
                            fx.command("close", "关闭编辑器", "Ctrl+W"),
                            fx.command("theme", "切换主题", "")});
  ST_CHECK_EQ(fx.palette->command_count(), static_cast<std::size_t>(3));
  ST_CHECK_EQ(fx.palette->match_count(), static_cast<std::size_t>(3));
}

ST_TEST(palette_filters_case_insensitive_over_title_and_detail) {
  PaletteFixture fx;
  fx.palette->set_commands({fx.command("save", "Save File", "Ctrl+S"),
                            fx.command("close", "关闭编辑器", "Ctrl+W"),
                            fx.command("theme", "切换主题", "Toggle Theme")});
  fx.palette->set_query("CTRL");
  ST_CHECK_EQ(fx.palette->match_count(), static_cast<std::size_t>(2));  // 两者的 detail 命中
  fx.palette->set_query("主题");
  ST_CHECK_EQ(fx.palette->match_count(), static_cast<std::size_t>(1));
  ST_CHECK_EQ(fx.palette->active_id(), "theme");
  fx.palette->set_query("zzz不存在的命令");
  ST_CHECK_EQ(fx.palette->match_count(), static_cast<std::size_t>(0));
  ST_CHECK(fx.palette->active_id().empty());
}

ST_TEST(palette_keyboard_navigation_wraps) {
  PaletteFixture fx;
  fx.palette->set_commands({fx.command("a", "A"), fx.command("b", "B"), fx.command("c", "C")});
  ST_CHECK_EQ(fx.palette->move_highlight(1), static_cast<std::size_t>(1));
  ST_CHECK_EQ(fx.palette->move_highlight(1), static_cast<std::size_t>(2));
  ST_CHECK_EQ(fx.palette->move_highlight(1), static_cast<std::size_t>(0));  // 环绕
  ST_CHECK_EQ(fx.palette->move_highlight(-1), static_cast<std::size_t>(2));
  // 键盘事件路径
  ST_CHECK(press_key(*fx.palette, fx.context, "ArrowDown"));
  ST_CHECK_EQ(fx.palette->active_id(), "a");
  ST_CHECK(press_key(*fx.palette, fx.context, "ArrowUp"));
  ST_CHECK_EQ(fx.palette->active_id(), "c");
}

ST_TEST(palette_enter_executes_highlighted_and_esc_closes) {
  PaletteFixture fx;
  fx.palette->set_commands({fx.command("save", "保存"), fx.command("close", "关闭")});
  fx.palette->set_query("关");
  ST_CHECK_EQ(fx.palette->match_count(), static_cast<std::size_t>(1));
  ST_CHECK(press_key(*fx.palette, fx.context, "Enter"));
  ST_CHECK_EQ(fx.executed, "close");
  ST_CHECK(press_key(*fx.palette, fx.context, "Escape"));
  ST_CHECK_EQ(fx.closed, "yes");
}

ST_TEST(palette_properties_and_actions) {
  PaletteFixture fx;
  fx.palette->set_commands({fx.command("a", "Alpha"), fx.command("b", "Beta")});
  // 属性面
  ST_CHECK_EQ(fx.palette->get_property("command_count").value(), "2");
  ST_CHECK_EQ(fx.palette->get_property("match_count").value(), "2");
  fx.palette->set_property("query", "alp");
  ST_CHECK_EQ(fx.palette->get_property("query").value(), "alp");
  ST_CHECK_EQ(fx.palette->get_property("match_count").value(), "1");
  fx.palette->set_property("query", "");  // 清过滤：全部命令回到可选集
  ST_CHECK_EQ(fx.palette->get_property("match_count").value(), "2");
  // 动作面：select 按序号 / 按 id；activate 执行
  ST_CHECK(fx.palette->invoke_action("select", "0"));
  ST_CHECK(fx.palette->invoke_action("activate", ""));
  ST_CHECK_EQ(fx.executed, "a");
  ST_CHECK(fx.palette->invoke_action("select", "b"));
  ST_CHECK(fx.palette->invoke_action("activate", ""));
  ST_CHECK_EQ(fx.executed, "b");
}

ST_TEST(palette_mask_click_outside_card_closes) {
  PaletteFixture fx;
  fx.palette->set_commands({fx.command("a", "A")});
  st::ui::Event click;
  click.kind = st::ui::EventKind::MouseDown;
  click.position = st::math::Point{10.0f, 700.0f};  // 视口底部（卡片外）
  (void)fx.palette->on_event(fx.context, click);
  ST_CHECK_EQ(fx.closed, "yes");
}

ST_TEST(palette_set_query_rebuilds_highlight_reset) {
  PaletteFixture fx;
  fx.palette->set_commands({fx.command("a", "Alpha"), fx.command("b", "Beta")});
  fx.palette->set_query("a");         // 先设一个非空过滤词
  fx.palette->move_highlight(1);
  fx.palette->set_query("al");        // 变化 → rebuild → 高亮归零
  ST_CHECK_EQ(fx.palette->get_property("active").value(), "0");
}

// ————————————————————————————————————————————————————————————————————————————
// 交互链路（回归）：点击执行 / 键盘落点
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(palette_clicking_item_executes_the_command) {
  // 回归用例：执行链曾挂在元素层的 `ListItem::on_activate` 上（取出列表项裸指针再绑），
  // 而 `List` 用同一个槽位实现“点击即选中本容器”——两者争用 ⇒ 命令被静默覆盖，
  // 症状是**鼠标点条目毫无反应**（只有 Enter 能用）。鼠标是主要入口，这条必须先过。
  PaletteFixture fx;
  fx.palette->set_commands({fx.command("alpha-id", "Alpha"), fx.command("beta-id", "Beta")});
  fx.palette->arrange(fx.context, st::math::Rect{0.0f, 0.0f, 1280.0f, 800.0f});

  // 找到卡片里第 2 个列表项，点在它身上（从面板子树里按类型找，不依赖内部成员名）
  st::ui::Element* target = nullptr;
  for (std::size_t index = 0; index < fx.palette->child_count(); ++index) {
    auto* card = fx.palette->child_at(index);
    if (card == nullptr) continue;
    for (std::size_t inner = 0; inner < card->child_count(); ++inner) {
      auto* candidate = card->child_at(inner);
      if (candidate != nullptr && candidate->type() == "List") {
        if (candidate->child_count() >= 2) target = candidate->child_at(1);
      }
    }
  }
  ST_REQUIRE(target != nullptr);
  const st::math::Rect box = target->bounds();
  ST_CHECK(!box.is_empty());

  Event click;
  click.kind = EventKind::MouseDown;
  click.button = 1;
  click.position = st::math::Point{box.center().x, box.center().y};
  (void)fx.root.dispatch(click);
  click.kind = EventKind::MouseUp;
  (void)fx.root.dispatch(click);
  click.kind = EventKind::Click;
  (void)fx.root.dispatch(click);
  ST_CHECK_EQ(fx.executed, std::string("beta-id"));
}

ST_TEST(palette_grab_focus_routes_typing_into_the_filter_box) {
  // 回归用例：`grab_focus()` 之前不存在，宿主（示例）只能 set_visible(true)——
  // 于是焦点还留在底层编辑器上：**面板开着，敲的字却跑进了代码里**。
  // 本用例断言“调了 grab_focus 之后，键盘输入进的是过滤框”。
  PaletteFixture fx;
  fx.palette->set_commands({fx.command("alpha", "Alpha"), fx.command("beta", "Beta")});
  fx.palette->arrange(fx.context, st::math::Rect{0.0f, 0.0f, 1280.0f, 800.0f});
  fx.palette->grab_focus();
  // 焦点确实落在面板子树内
  ST_CHECK(fx.root.focused() != nullptr);
  bool inside = false;
  st::ui::Element* walker = fx.root.focused();
  while (walker != nullptr) {
    if (walker == fx.palette) {
      inside = true;
      break;
    }
    walker = walker->parent();
  }
  ST_CHECK(inside);

  // 真实键入：文字必须进过滤框（match_count 随之收敛），而不是消失在别处
  Event text;
  text.kind = EventKind::TextInput;
  text.text = "beta";
  (void)fx.root.dispatch(text);
  ST_CHECK_EQ(fx.palette->query(), std::string("beta"));
  ST_CHECK_EQ(fx.palette->match_count(), static_cast<std::size_t>(1));
}

ST_TEST(palette_click_and_enter_have_identical_effects) {
  // 回归用例：执行链曾**分成两份**——Enter 走 `activate_highlighted()`（调 handler），
  // 鼠标点走列表项回调（只发 `on_command`）。症状取决于宿主怎么用：
  // gbcode 把 `on_command` 当“已执行”去刷状态栏，于是**状态说执行了、文件没打开**。
  // 本用例逐字段比对两条路径的效果：命令 id、handler 调用次数、回调次数必须全等。
  PaletteFixture fx;
  int handler_calls = 0;
  auto command = fx.command("alpha", "Alpha");
  command.handler = [&handler_calls]() { ++handler_calls; };
  fx.palette->set_commands({command});
  fx.palette->arrange(fx.context, st::math::Rect{0.0f, 0.0f, 1280.0f, 800.0f});

  // ① Enter 路径
  ST_CHECK(press_key(*fx.palette, fx.context, "Enter"));
  const std::string after_enter = fx.executed;
  const int handlers_after_enter = handler_calls;

  // ② 鼠标点击路径（同一命令、同一状态）
  fx.executed.clear();
  st::ui::Element* target = nullptr;
  for (std::size_t index = 0; index < fx.palette->child_count(); ++index) {
    auto* card = fx.palette->child_at(index);
    if (card == nullptr) continue;
    for (std::size_t inner = 0; inner < card->child_count(); ++inner) {
      auto* candidate = card->child_at(inner);
      if (candidate != nullptr && candidate->type() == "List" && candidate->child_count() > 0) {
        target = candidate->child_at(0);
      }
    }
  }
  ST_REQUIRE(target != nullptr);
  const st::math::Rect box = target->bounds();
  Event click;
  click.kind = EventKind::MouseDown;
  click.button = 1;
  click.position = st::math::Point{box.center().x, box.center().y};
  (void)fx.root.dispatch(click);
  click.kind = EventKind::MouseUp;
  (void)fx.root.dispatch(click);
  click.kind = EventKind::Click;
  (void)fx.root.dispatch(click);

  ST_CHECK_EQ(after_enter, std::string("alpha"));
  ST_CHECK_EQ(fx.executed, after_enter);        // 外部通知一致
  ST_CHECK_EQ(handler_calls, handlers_after_enter + 1);  // handler 也真的跑了
}

/// **卡片配色必须跟随主题**（回归：用户报「Ctrl+P 面板配色不对」）。
///
/// 缺陷形态（2026-10-18）：`CommandPalette::apply_theme` 只把主题色算进
/// `card_bg_` / `card_border_` 两个**成员字段**，而绘制读的是
/// `card->style().background`——字段与真值分家。于是切主题时
/// 卡片颜色纹丝不动，与内部 `Input`/`ListItem`（它们用主题色）对不上：
/// 暗色界面上出现一块浅色卡片，或反之。
///
/// 判据不是「像素好看」，而是**结构契约**：`apply_theme(theme)` 之后，
/// 卡片实际用于绘制的 `style().background` 必须等于该主题的
/// `surface_raised`（弹层底色的语义位）。这条把「字段算对了」与
/// 「字段真的落地了」区分开——后者才是缺陷所在。
ST_TEST(palette_card_background_follows_theme) {
  PaletteFixture fixture;
  const st::ui::Element* card = fixture.root.overlay_at(0);
  ST_CHECK(card != nullptr);
  // 找 `palette-card`（它挂在 overlay 根的子树里）。
  const st::ui::Element* found = nullptr;
  const auto search = [&](auto&& self, const st::ui::Element& element) -> void {
    if (element.id() == "palette-card") found = &element;
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      if (element.child_at(index) != nullptr) self(self, *element.child_at(index));
    }
  };
  search(search, *card);
  ST_CHECK(found != nullptr);

  const auto background_of = [&] { return found->style().background; };
  const auto same = [](st::math::Color left, st::math::Color right) {
    return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
  };

  // ① 暗色：卡片必须用暗色的 `surface_raised`（而不是亮色，也不是硬编码色）。
  // 模拟 `UiRoot::layout` 的真实递归（`apply_theme` 只作用于单元素，递归由树侧负责）。
  const auto apply_tree = [](auto&& self, st::ui::Element& element, const Theme& theme) -> void {
    element.apply_theme(theme);
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      if (element.child_at(index) != nullptr) self(self, *element.child_at(index), theme);
    }
  };
  const Theme dark = Theme::dark();
  apply_tree(apply_tree, *fixture.palette, dark);
  ST_CHECK(same(background_of(), dark.colors().surface_raised));

  // ② 浅色：必须跟着换（这就是「只改字段不写回 style」会失败的那一步）。
  const Theme light = Theme::light();
  apply_tree(apply_tree, *fixture.palette, light);
  ST_CHECK(same(background_of(), light.colors().surface_raised));

  // ③ 两个主题的 `surface_raised` 本身必须不同——否则上面两条断言是空转。
  ST_CHECK(!same(dark.colors().surface_raised, light.colors().surface_raised));
}

/// **切换主题后，面板里的子元素也必须换色**（输入框走 `theme.surface`）。
///
/// 与上一条互补：上一条守「卡片自己的色落地了」，这条守「子树收到了新主题」。
/// 两者缺一就会出现「卡片换了、里面还是旧色」的半吊子状态——而那在截图里
/// 看起来就像「配色不对」，很难一眼定位到是卡片的锅还是子元素的锅。
ST_TEST(palette_children_follow_theme_switch) {
  PaletteFixture fixture;
  const st::ui::Element* card = fixture.root.overlay_at(0);
  ST_CHECK(card != nullptr);
  const st::ui::Input* input = nullptr;
  const auto search = [&](auto&& self, const st::ui::Element& element) -> void {
    if (element.type() == "Input") input = dynamic_cast<const st::ui::Input*>(&element);
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      if (element.child_at(index) != nullptr) self(self, *element.child_at(index));
    }
  };
  search(search, *card);
  ST_CHECK(input != nullptr);

  // 输入框的底色在 `apply_theme` 里落到 `style().background`（见 `input.cpp` 的
  // `apply_*` 段）。暗色下它应当是暗色的 `surface`——**不是**亮色的。
  const auto apply_tree = [](auto&& self, st::ui::Element& element, const Theme& theme) -> void {
    element.apply_theme(theme);
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      if (element.child_at(index) != nullptr) self(self, *element.child_at(index), theme);
    }
  };
  apply_tree(apply_tree, *fixture.palette, Theme::dark());
  const st::math::Color dark_bg = input->style().background;
  apply_tree(apply_tree, *fixture.palette, Theme::light());
  const st::math::Color light_bg = input->style().background;
  ST_CHECK(dark_bg.r != light_bg.r || dark_bg.g != light_bg.g || dark_bg.b != light_bg.b);
  // 暗色主题下的输入底应当**比浅色主题明显更暗**（判据取亮度，不锁死色值）。
  const int dark_luma = static_cast<int>(dark_bg.r) + dark_bg.g + dark_bg.b;
  const int light_luma = static_cast<int>(light_bg.r) + light_bg.g + light_bg.b;
  ST_CHECK(dark_luma < light_luma);
}
