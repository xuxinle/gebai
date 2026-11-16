/// `Tabs::on_context_menu`：右键标签报“第几个”，且**不改选中**。
///
/// ## 两件事必须同时成立
///
/// 1. **右键要报**：右键是否产生 `Click` 取决于后端，所以组件在 `MouseDown`
///    (button==2) 就上报（与 `Tree::on_context_menu` 同一形态与同一理由）。
/// 2. **右键不改选中**：它只是“我想对这个标签做点什么”，不是“切到它”。
///
/// 第 2 条容易被“顺手也切一下”的写法破坏，而用户感受是
/// **右键看菜单的功夫，编辑器已经换页了**（长文件要重新加载、滚动位置丢失）。
/// 这条测试就钉它。

#include "st/test/test.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/theme.hpp"

namespace {

[[nodiscard]] auto make_context() -> st::ui::RenderContext {
  static const st::ui::Theme theme = st::ui::Theme::light();
  return st::ui::RenderContext{theme, nullptr, 0.0};
}

/// 造一个 4 标签的标签条，已布局到给定宽度（标签位置才能算）。
[[nodiscard]] auto make_tabs() -> std::unique_ptr<st::ui::Tabs> {
  auto tabs = std::make_unique<st::ui::Tabs>();
  tabs->set_tabs({"one.cpp", "two.cpp", "three.cpp", "four.cpp"});
  tabs->set_active(0);
  const auto context = make_context();
  st::ui::Constraints constraints;
  constraints.max_width = 800.0f;
  constraints.max_height = 40.0f;
  constraints.available_width = 800.0f;
  constraints.available_height = 40.0f;
  tabs->measure(context, constraints);
  tabs->arrange(context, st::math::Rect{0.0f, 0.0f, 800.0f, 34.0f});
  return tabs;
}

/// 第 `index` 个标签的横向中心（用 `tab_rect` 若有；否则按标签条内偏移估）。
[[nodiscard]] auto tab_center_x(const st::ui::Tabs& tabs, std::size_t index) -> float {
  const st::math::Rect tab = tabs.tab_rect(index);
  return tab.center().x;
}

}  // namespace

/// 右键某个标签：回调收到**那个下标**，且 `active` 不变。
ST_TEST(tabs_right_click_reports_index_without_changing_active) {
  auto tabs_ptr = make_tabs();
  st::ui::Tabs& tabs = *tabs_ptr;
  const auto context = make_context();

  std::vector<std::size_t> reported;
  tabs.on_context_menu = [&reported](std::size_t index) { reported.push_back(index); };

  // 右键第 2 个标签（下标 1）。
  st::ui::Event down{};
  down.kind = st::ui::EventKind::MouseDown;
  down.button = 2;
  down.position = st::math::Point{tab_center_x(tabs, 1), 17.0f};
  const bool handled = tabs.on_event(context, down);

  st::log::out("[tabs] 右键下标1 → handled={} reported={} active={}\n", handled, reported.size(),
               tabs.active_index());
  ST_CHECK(handled);
  ST_REQUIRE(reported.size() == 1);
  ST_CHECK_EQ(reported[0], std::size_t{1});
  // **不许改选中**：右键是“对它做点什么”，不是“切到它”。
  ST_CHECK_EQ(tabs.active_index(), std::size_t{0});
}

/// 右键每个标签都报到正确的下标（防“永远报 0”或“报了相邻项”）。
ST_TEST(tabs_right_click_maps_each_label_to_its_own_index) {
  auto tabs_ptr = make_tabs();
  st::ui::Tabs& tabs = *tabs_ptr;
  const auto context = make_context();

  std::vector<std::size_t> reported;
  tabs.on_context_menu = [&reported](std::size_t index) { reported.push_back(index); };

  for (std::size_t index = 0; index < 4; ++index) {
    st::ui::Event down{};
    down.kind = st::ui::EventKind::MouseDown;
    down.button = 2;
    down.position = st::math::Point{tab_center_x(tabs, index), 17.0f};
    (void)tabs.on_event(context, down);
  }

  st::log::out("[tabs] 四次右键报出：");
  for (const std::size_t index : reported) st::log::out("{} ", index);
  st::log::out("\n");
  ST_REQUIRE(reported.size() == 4);
  for (std::size_t index = 0; index < 4; ++index) ST_CHECK_EQ(reported[index], index);
  ST_CHECK_EQ(tabs.active_index(), std::size_t{0});
}

/// **左键仍然切标签**（别为了右键而把左键也拦掉）。
///
/// 这条是防火墙：`MouseDown` 分支里加右键判断时用 `[[fallthrough]]` 落回原逻辑，
/// 一旦写成 `return`，左键就废了——而“点标签没反应”很难归因到菜单代码。
ST_TEST(tabs_left_click_still_activates) {
  auto tabs_ptr = make_tabs();
  st::ui::Tabs& tabs = *tabs_ptr;
  const auto context = make_context();
  tabs.on_context_menu = [](std::size_t) {};

  st::ui::Event click{};
  click.kind = st::ui::EventKind::Click;
  click.button = 1;
  click.position = st::math::Point{tab_center_x(tabs, 2), 17.0f};
  (void)tabs.on_event(context, click);

  st::log::out("[tabs] 左键点下标2 → active={}\n", tabs.active_index());
  ST_CHECK_EQ(tabs.active_index(), std::size_t{2});
}
