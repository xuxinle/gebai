/// 桌面壳窗口能力的契约测试（`fill_monitor` / `stay_at_bottom` / 系统任务栏）。
///
/// 这三项是"替代原生桌面"所需的窗口形态能力，全部由平台后端实现——
/// 而平台差异在无头环境测不到。所以这里钉**能在任何环境断言的两层**：
///
/// 1. **纯函数** `monitor_fill_rect`：铺满语义 = 原样返回显示器矩形（不做边距收缩）。
///    它的价值在于"用错那块屏"这类错误（多显示器下拿主屏算会把窗口摆到另一块屏上），
///    而那是纯几何，可以精确断言。
/// 2. **无头降级如实**：`set_stay_at_bottom` / `set_system_taskbar_visible` 在无头
///    后端必须返回 `Unsupported`（而不是假装成功）。这条防的是"能力位撒谎"——
///    应用据此判断该不该提供切换按钮，撒谎会让用户点到一个永远不生效的开关。
#include "st/test/test.hpp"

#include "st/app/app.hpp"
#include "st/math/geometry.hpp"
#include "st/shell/shell.hpp"

ST_TEST(monitor_fill_rect_returns_monitor_unchanged) {
  // 主屏 3840×2160 在原点；副屏在右侧偏移 (3840, 0)。铺满必须是"那块屏的矩形"，
  // 不能有边距收缩、也不能被归零到原点（副屏归零就会跳到主屏去）。
  const st::math::IntRect primary{0, 0, 3840, 2160};
  const st::math::IntRect filled_primary = st::shell::monitor_fill_rect(primary);
  ST_CHECK(filled_primary.x == 0);
  ST_CHECK(filled_primary.y == 0);
  ST_CHECK(filled_primary.width == 3840);
  ST_CHECK(filled_primary.height == 2160);

  const st::math::IntRect secondary{3840, 0, 1920, 1080};
  const st::math::IntRect filled_secondary = st::shell::monitor_fill_rect(secondary);
  ST_CHECK(filled_secondary.x == 3840);   // 位置保留（副屏原点不在 0,0）
  ST_CHECK(filled_secondary.width == 1920);
  ST_CHECK(filled_secondary.height == 1080);
}

ST_TEST(monitor_fill_rect_keeps_taskbar_area) {
  // 工作区（rcWork）与显示器（rcMonitor）的差别就是任务栏条带：
  // 铺满**不能**退化成工作区。这里用 2160 vs 2120 表达"底部 40px 任务栏"。
  const st::math::IntRect monitor{0, 0, 1920, 1080};
  const st::math::IntRect work{0, 0, 1920, 1040};
  const st::math::IntRect filled = st::shell::monitor_fill_rect(monitor);
  ST_CHECK(filled.height == 1080);          // 含任务栏条带
  ST_CHECK(filled.height != work.height);   // 明确不是工作区口径
}

ST_TEST(headless_reports_desktop_shell_capabilities_truthfully) {
  // 无头后端没有显示器、没有 Z 序、没有系统任务栏——三条都必须如实拒绝。
  // 这条测试的价值在**防撒谎**：`Application::desktop_shell_supported()` 与
  // UI 上那个切换按钮都靠这些返回值决定行为。
  st::app::AppOptions options;
  options.headless = true;
  options.backend = "headless";
  options.width = 320;
  options.height = 200;
  st::app::Application app("st_desktop_caps_probe", "0.0.0", options);
  const auto started = app.start();
  ST_CHECK(started.has_value());
  if (!started.has_value()) return;

  ST_CHECK(!app.desktop_shell_supported());
  const auto bottom = app.set_stay_at_bottom(true);
  ST_CHECK(!bottom.has_value());
  const auto taskbar = app.set_system_taskbar_visible(false);
  ST_CHECK(!taskbar.has_value());
  // `fill_monitor` 在无头下也如实报"做不到铺满"（没有显示器可铺）。
  const auto monitor = app.fill_monitor();
  ST_CHECK(!monitor.has_value());
  // 切模式也必须失败并**保持不变**（不能出现"模式位翻成 true 但窗口没变"）。
  const auto toggled = app.set_desktop_shell_mode(true);
  ST_CHECK(!toggled.has_value());
  ST_CHECK(!app.desktop_shell_mode());
}
