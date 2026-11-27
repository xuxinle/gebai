/// 主题切换后，**标题栏里的主题选择器必须跟上**（应用级端到端）。
///
/// ## 它钉的是什么（2026-11-27，dev-tools 实测）
///
/// 用户报告：应用默认暗色，点标题栏主题按钮打开选择器，**勾选落在「亮色」上**。
///
/// 根因是两段都没接：
/// 1. `ThemePicker` 构造时 `populate_builtins("light")` —— 它只看得到一个写死的默认值；
/// 2. `TitleBar::apply_theme`（主题变更**唯一**会到达本组件的路径）从不把它交给选择器。
///
/// 于是选择器永远停在 `light`。危险之处在于**属性面全自洽**：
/// `get value` 返回 `light`、面板也勾 `light`，两者一致——只有和真实主题比才露馅。
///
/// ## 为什么这条只能应用级测（这是我第一版的错误）
///
/// 我先写的断言是「`builtin_theme("dark")->name() == "dark"`」，回退修复后**依然全绿**：
/// `Theme::light()` / `dark()` 自己就写了名字，而 `builtin_theme()` 直接返回它们，
/// **绕过了应用合成路径**（`Application::compose_theme_with`）——
/// 真正需要写名字的是那一条。教训：测「谁把值传给谁」时，不能让被测路径被短路掉。
#include <memory>
#include <string>

#include "st/app/app.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/theme_picker.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"

namespace {

using st::app::AppOptions;
using st::app::Application;

/// 无头 + 只跑有限帧（与 `app_font_stack_lifetime_test` 同一套选项）。
[[nodiscard]] auto headless_options() -> AppOptions {
  AppOptions options;
  options.headless = true;
  options.max_frames = 1;
  options.defer_gpu = false;
  options.renderer = "software";
  options.theme = st::ui::ThemeMode::Dark;
  return options;
}

}  // namespace

/// 切主题后：`theme_snapshot().name`、标题栏选择器的 `value`、三角形三者必须一致。
ST_TEST(app_theme_name_propagates_to_title_bar_picker) {
  Application app("theme-propagation-probe", "0.1.0", headless_options());
  auto frame = std::make_unique<st::ui::WindowFrame>("探针");
  auto* title_bar = frame->title_bar();
  ST_REQUIRE(title_bar != nullptr);
  auto* picker = title_bar->theme_picker();
  ST_REQUIRE(picker != nullptr);
  app.set_content(std::move(frame));

  const auto started = app.start();
  ST_REQUIRE(started.has_value());
  app.root().layout(true);

  // 启动时（主题为 dark）：三者就要一致。
  const std::string before_shown = std::string(picker->current());
  const std::string before_reported =
      app.theme_snapshot().contains("name") ? app.theme_snapshot()["name"].as_string()
                                             : std::string();
  st::log::out("[theme-propagation] 启动时 上报={} 选择器={}\n", before_reported, before_shown);
  ST_CHECK_EQ(before_shown, before_reported);
  ST_CHECK_EQ(before_shown, std::string("dark"));

  // 切到一个**不同**的主题：两者要同时跟过去。
  const std::string target = before_shown == "light" ? "dark" : "light";
  const auto switched = app.set_theme_name(target);
  ST_REQUIRE(switched.has_value());
  // **推一帧**：`set_theme_name` 只把主题发给 `UiRoot`，而 `TitleBar::apply_theme`
  // 要等下一轮布局/绘制才跑（那里才是它拿到 `Theme` 的地方）。
  // 不推这一帧，读到的是**上一帧**的选择器值——那正是本用例第一版假红的原因。
  app.root().layout(true);

  // ① 应用侧上报的主题名
  const auto snapshot = app.theme_snapshot();
  const std::string reported = snapshot.contains("name") ? snapshot["name"].as_string()
                                                         : std::string();
  // ② 标题栏选择器的当前值（它驱动色卡墙上那个勾）
  const std::string shown = std::string(picker->current());

  st::log::out("[theme-propagation] 切换后 上报={} 选择器={}\n", reported, shown);
  ST_CHECK_EQ(shown, reported);
  ST_CHECK_EQ(shown, target);
}
