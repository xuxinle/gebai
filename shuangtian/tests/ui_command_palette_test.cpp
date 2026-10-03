/// 命令面板组件测试：过滤/键盘导航/执行/属性面与动作面。

#include <memory>
#include <string>

#include "st/test/test.hpp"
#include "st/ui/components/command_palette.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::CommandPalette;
using st::ui::Theme;
using st::ui::UiRoot;

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
