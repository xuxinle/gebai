#include <cmath>
#include <format>
#include <iostream>

#include "st/test/test.hpp"
#include "st/ui/wheel.hpp"

namespace {

/// 最小主题替身：`wheel_step` 可指定，其余不用。
struct FakeTheme {
  struct Metrics {
    float wheel_step{48.0f};
  } m{};
  [[nodiscard]] auto metrics() const noexcept -> const Metrics& { return m; }
};

struct FakeEvent {
  float wheel_delta{0.0f};
};

}  // namespace

// 这一组钉的是**方向约定**：`wheel_delta` 向上/前推为正，而滚动偏移向下增大。
// 收拢前符号在各组件里各写一遍，`table.cpp` 与 `file_dialog.cpp` 都写反过，
// 症状是"事件被消费、界面有反应，但偏移永远夹在 0"——像素测试完全看不出来。

ST_TEST(wheel_up_increases_visible_content) {
  // 向上滚（正 delta）= 看上面的内容 ⇒ 偏移变小。
  FakeTheme theme;
  FakeEvent up{.wheel_delta = 1.0f};
  const float target = st::ui::wheel_target(100.0f, up, 500.0f, theme);
  std::cout << std::format("  [wheel] up: 100 -> {:.1f}\n", target) << std::flush;
  ST_CHECK(target < 100.0f);
  ST_CHECK(std::abs(target - 52.0f) < 0.01f);  // 100 - 48
}

ST_TEST(wheel_down_decreases_offset) {
  FakeTheme theme;
  FakeEvent down{.wheel_delta = -1.0f};
  const float target = st::ui::wheel_target(100.0f, down, 500.0f, theme);
  std::cout << std::format("  [wheel] down: 100 -> {:.1f}\n", target) << std::flush;
  ST_CHECK(target > 100.0f);
  ST_CHECK(std::abs(target - 148.0f) < 0.01f);
}

ST_TEST(wheel_clamps_at_both_ends) {
  FakeTheme theme;
  // 起点向上滚：夹到 0，不出现负偏移。
  ST_CHECK(std::abs(st::ui::wheel_target(0.0f, FakeEvent{.wheel_delta = 1.0f}, 500.0f, theme)) < 0.01f);
  // 终点向下滚：夹到上限，不越界。
  ST_CHECK(std::abs(st::ui::wheel_target(500.0f, FakeEvent{.wheel_delta = -1.0f}, 500.0f, theme) -
                    500.0f) < 0.01f);
}

ST_TEST(wheel_returns_negative_when_nothing_to_scroll) {
  // 内容没超高 ⇒ 返回负值，调用方据此 return false **把滚轮冒泡给外层**。
  // 静默消费是本仓踩过的坑（实测：相期卡片列表滚不动）。
  FakeTheme theme;
  ST_CHECK(st::ui::wheel_target(0.0f, FakeEvent{.wheel_delta = -1.0f}, 0.0f, theme) < 0.0f);
  ST_CHECK(st::ui::wheel_target(0.0f, FakeEvent{.wheel_delta = -1.0f}, -5.0f, theme) < 0.0f);
}

ST_TEST(wheel_step_follows_theme_then_fallback) {
  // 主题优先；非正值回到兑底值——收拢前 `table`/`tabs` 用各自的 constexpr，
  // 换主题时一部分组件跟得上、另一部分不跟。
  FakeTheme themed;
  themed.m.wheel_step = 20.0f;
  ST_CHECK(std::abs(st::ui::wheel_step(themed) - 20.0f) < 0.01f);

  FakeTheme unset;
  unset.m.wheel_step = 0.0f;
  ST_CHECK(std::abs(st::ui::wheel_step(unset) - st::ui::kWheelFallbackStep) < 0.01f);
  ST_CHECK(std::abs(st::ui::kWheelFallbackStep - 48.0f) < 0.01f);
}

ST_TEST(wheel_lines_scale_the_step) {
  // 后端给的行数（Windows 一格 = 120 原始值 ⇒ 换算成 1 行）可直接放大步长。
  FakeTheme theme;
  FakeEvent down{.wheel_delta = -1.0f};
  const float one = st::ui::wheel_target(0.0f, down, 1000.0f, theme, 1.0f);
  const float three = st::ui::wheel_target(0.0f, down, 1000.0f, theme, 3.0f);
  ST_CHECK(std::abs(one - 48.0f) < 0.01f);
  ST_CHECK(std::abs(three - 144.0f) < 0.01f);
}
