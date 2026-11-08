/// 一次物理手势算**几次点击**——三个窗口后端必须是同一笔账。
///
/// ## 缺陷来源（用户 2026-11-08 报「鼠标连续点击有事件重复触发」）
///
/// 真窗口 + 真鼠标（`SendInput`）+ 应用自己的激活计数，实测：**一次双击激活 3 次**，
/// 事件账为 `click×2 + dblclick×1 + mousedown×1 + mouseup×2`。
///
/// `mousedown=1 / mouseup=2` 是关键线索：两次按下只产生一条 `MouseDown`。
/// 根因在 Win32 后端对系统消息的翻译：
///
/// | 消息 | 旧实现发出 |
/// |---|---|
/// | `WM_LBUTTONDOWN` | `MouseDown` |
/// | `WM_LBUTTONUP`   | `MouseUp` + `Click` |
/// | `WM_LBUTTONDBLCLK`（系统用它**代替**第二次的 `DOWN`） | 只 `DoubleClick` |
/// | `WM_LBUTTONUP`   | `MouseUp` + `Click` |
///
/// 于是第二次按下的 `MouseDown` 在框架里根本不存在，而点击族事件一次双击有三个
/// （`Click`/`DoubleClick`/`Click`）。`Button` 在 `Click` 与 `DoubleClick` 上都
/// `activate()`，用户点一次被触发 3 次。
///
/// ## 本文件的判据
///
/// 「一次按放 = 一条 `MouseDown` + 一条 `MouseUp` + **恰好一条**点击族事件」，
/// 且点击族按连击数升级。三个后端共用 `st::shell::detail::ClickStreak`，
/// 这里钉住它的语义（纯逻辑，不依赖窗口系统）。
///
/// ⚠ 这些断言做过**逆向验证**：把 `click_kind()` 改回恒返回 `Click`、
/// 把 `confirmed_repeat` 那一行删掉，对应用例都会变红（见 `tools/click_gesture_reverse_verify.py`）。

#include "st/test/test.hpp"

#include "../src/shell/click_gesture.hpp"

namespace {

using st::math::Point;
using st::shell::detail::ClickConfig;
using st::shell::detail::ClickStreak;
using st::ui::EventKind;

constexpr ClickConfig kConfig{500.0, 4.0};
constexpr Point kAt{100.0f, 200.0f};

/// 点击族断言包装：`EventKind` 没有 `std::formatter`（不为测试给枚举加一个），
/// 直接 `ST_CHECK_EQ` 编译不过。失败时 `ST_CHECK` 会打印源码行，
/// 而源码里已经把期望的枚举名写着了。
[[nodiscard]] auto kind_is(const ClickStreak& streak, EventKind expected) -> bool {
  return streak.click_kind() == expected;
}

}  // namespace

ST_TEST(click_streak_single_press_is_one_click) {
  ClickStreak streak;
  ST_CHECK_EQ(streak.press(1000.0, kAt, kConfig), 1);
  ST_CHECK(kind_is(streak, EventKind::Click));
}

ST_TEST(click_streak_quick_second_press_upgrades_to_double) {
  ClickStreak streak;
  ST_CHECK_EQ(streak.press(1000.0, kAt, kConfig), 1);
  // 时间窗内、位置相同 ⇒ 第二击。它照样要有自己的 MouseDown（后端据此重置
  // 「按下归属」），所以计数器是 2，而不是"跳过不看"。
  ST_CHECK_EQ(streak.press(1080.0, kAt, kConfig), 2);
  ST_CHECK(kind_is(streak, EventKind::DoubleClick));
}

ST_TEST(click_streak_third_press_caps_at_triple) {
  ClickStreak streak;
  (void)streak.press(1000.0, kAt, kConfig);
  (void)streak.press(1050.0, kAt, kConfig);
  ST_CHECK_EQ(streak.press(1100.0, kAt, kConfig), 3);
  ST_CHECK(kind_is(streak, EventKind::TripleClick));
  // 第四击没有更多语义：`EventKind` 只到三击，组件按三击处理。
  ST_CHECK_EQ(streak.press(1150.0, kAt, kConfig), 4);
  ST_CHECK(kind_is(streak, EventKind::TripleClick));
}

ST_TEST(click_streak_slow_second_press_restarts) {
  ClickStreak streak;
  (void)streak.press(1000.0, kAt, kConfig);
  // 超过时间窗 ⇒ 这是一次新的单击（不是第二击）。
  ST_CHECK_EQ(streak.press(1700.0, kAt, kConfig), 1);
  ST_CHECK(kind_is(streak, EventKind::Click));
}

ST_TEST(click_streak_far_second_press_restarts) {
  ClickStreak streak;
  (void)streak.press(1000.0, kAt, kConfig);
  // 时间窗内但指针挪得远 ⇒ 也不是同一手势（用户点的是两处）。
  ST_CHECK_EQ(streak.press(1080.0, Point{kAt.x + 40.0f, kAt.y}, kConfig), 1);
  ST_CHECK(kind_is(streak, EventKind::Click));
}

ST_TEST(click_streak_honours_window_system_confirmation) {
  // Win32 的 `WM_LBUTTONDBLCLK` 是**系统已经判定**的又一击。它优先于我们的
  // 时间/位移启发式：系统考虑的是用户自己设置的双击速度（可能被调到 900ms），
  // 而我们的窗口是常量。
  ClickStreak streak;
  (void)streak.press(1000.0, kAt, kConfig);
  ST_CHECK_EQ(streak.press(2000.0, kAt, kConfig, /*confirmed_repeat=*/true), 2);
  ST_CHECK(kind_is(streak, EventKind::DoubleClick));
}

ST_TEST(click_streak_confirmation_alone_is_not_triple) {
  // 边界：系统说是连击、但我们一条按下都没记过（例如窗口刚失活清过账）。
  // 不能凭空算成第三击——至少是双击。
  ClickStreak streak;
  ST_CHECK_EQ(streak.press(1000.0, kAt, kConfig, /*confirmed_repeat=*/true), 2);
  ST_CHECK(kind_is(streak, EventKind::DoubleClick));
}

ST_TEST(click_streak_reset_starts_a_new_gesture) {
  ClickStreak streak;
  (void)streak.press(1000.0, kAt, kConfig);
  (void)streak.press(1050.0, kAt, kConfig);
  ST_CHECK_EQ(streak.count(), 2);
  // 窗口失活/其它键打断：不能把中断前后的两次按下连成一击。
  streak.reset();
  ST_CHECK_EQ(streak.count(), 0);
  ST_CHECK_EQ(streak.press(1100.0, kAt, kConfig), 1);
  ST_CHECK(kind_is(streak, EventKind::Click));
}

ST_TEST(click_streak_tolerates_sub_pixel_jitter) {
  // 容差是 4 **逻辑**px：手在按键时总会抖半像素，把抖动算成「点了两处」
  // 会让真实用户永远点不出双击（1.5x 屏上 1 物理像素 = 0.67 逻辑）。
  ClickStreak streak;
  (void)streak.press(1000.0, kAt, kConfig);
  ST_CHECK_EQ(streak.press(1050.0, Point{kAt.x + 0.6f, kAt.y - 0.6f}, kConfig), 2);
  ST_CHECK(kind_is(streak, EventKind::DoubleClick));
}

ST_TEST(click_streak_independent_presses_never_upgrade) {
  // 反向钉子：慢速多击必须全是单击，否则「一秒钟点三下」会被当成三击——
  // 那是"事件重复触发"的另一副面孔。
  ClickStreak streak;
  ST_CHECK_EQ(streak.press(1000.0, kAt, kConfig), 1);
  ST_CHECK_EQ(streak.press(1800.0, kAt, kConfig), 1);
  ST_CHECK_EQ(streak.press(2600.0, kAt, kConfig), 1);
  ST_CHECK(kind_is(streak, EventKind::Click));
}
