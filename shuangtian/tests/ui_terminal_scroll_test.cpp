/// 终端**滚动方向合约**测试（`Terminal::wheel_scroll_lines`）。
///
/// ### 本轮钉住的缺陷（2026-10-08，用户报「可以往上滚不能往下滚」）
///
/// 旧实现是：
///
/// ```cpp
/// const int per = std::max(1, static_cast<int>(-event.wheel_delta * 3.0f));
/// int next = target.scrollback_offset + per;   // 永远是 offset + 正数
/// ```
///
/// 两个错叠在一起：
///
/// 1. **`std::max(1, …)` 把符号抹掉了**——`per` 恒为正，于是向上滚与向下滚
///    都变成"偏移增大"，表现就是**只能往上滚**；
/// 2. 外层还有个取反，方向本就与系统口径相反。
///
/// 为什么之前没被发现：**单向断言看不出来**——"向上滚能看到更早的历史"在
/// 缺陷版下**也是成立的**（offset 确实在变大）。只有把**两个方向一起断言**
/// 才能抓住它。这是本文件与 `ui_terminal_input_test.cpp` 分开、且强调
/// "双向"的原因。
///
/// ### 契约（与系统 / `ScrollView` 同一口径）
///
/// `wheel_delta` **向上为正、向下为负**（Win32 的 `WM_MOUSEWHEEL` 向上给 +120，
/// X11 按钮 4 也是上）。`scrollback_offset` 是"从底部往上翻了多少行"，
/// 所以偏移变化与 `delta` **同号**。

#include "st/test/test.hpp"

#include <string>

#include "st/ui/components/terminal.hpp"

namespace {

using st::ui::Terminal;

/// 带数值的失败信息（`ST_CHECK` 的宏消息只有源码文本）。
void expect_eq(int actual, int wanted, std::string what) {
  ::st::test::Registry::instance().count_check();
  if (actual != wanted) {
    ::st::test::Registry::instance().record_failure(
        __FILE__, __LINE__,
        std::format("{}: 期望 {}，实际 {}", what, wanted, actual));
  }
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// ① 核心：两个方向都必须动，且方向相反
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(terminal_wheel_scrolls_up_for_positive_delta) {
  // 向上滚（`delta > 0`）⇒ 偏移变化为正（往上翻更多历史）。
  expect_eq(Terminal::wheel_scroll_lines(1.0f), 3, "向上滚一格");
  expect_eq(Terminal::wheel_scroll_lines(2.0f), 6, "向上滚两格");
}

ST_TEST(terminal_wheel_scrolls_down_for_negative_delta) {
  // **缺陷的直接判据**：向下滚必须得到负值。
  //
  // 逆向验证：把实现改回 `std::max(1, static_cast<int>(-delta * 3.0f))`，
  // 本用例立刻变红（期望 -3，实际 1）。
  expect_eq(Terminal::wheel_scroll_lines(-1.0f), -3, "向下滚一格");
  expect_eq(Terminal::wheel_scroll_lines(-2.0f), -6, "向下滚两格");
}

ST_TEST(terminal_wheel_directions_are_opposite) {
  // **双向判据**：同一档位下两个方向必须**符号相反、大小相等**。
  // 这一条专门用来抓"符号被抹掉"那类错——单向断言对它无效。
  for (const float step : {1.0f, 2.0f, 3.0f, 5.0f}) {
    const int up = Terminal::wheel_scroll_lines(step);
    const int down = Terminal::wheel_scroll_lines(-step);
    expect_eq(up + down, 0, std::format("{} 格的上下位移应互为相反数", step));
    ::st::test::Registry::instance().count_check();
    if (up <= 0 || down >= 0) {
      ::st::test::Registry::instance().record_failure(
          __FILE__, __LINE__,
          std::format("方向反了：{} 格时 up={} down={}（up 应 >0、down 应 <0）", step, up, down));
    }
  }
}

// ————————————————————————————————————————————————————————————————————————————
// ② 边界：不足一格不动、零不动
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(terminal_wheel_below_one_line_does_not_move) {
  // 半格（高精度滚轮/触摸板会给小数）**不该**被舍成 1 行——
  // 旧实现的 `std::max(1, …)` 恰好会把 0 也变成 1（"轻轻一滚就跳"）。
  expect_eq(Terminal::wheel_scroll_lines(0.0f), 0, "零增量");
  expect_eq(Terminal::wheel_scroll_lines(0.3f), 0, "不足三分之一格（上）");
  expect_eq(Terminal::wheel_scroll_lines(-0.3f), 0, "不足三分之一格（下）");
}

ST_TEST(terminal_wheel_accepts_fractional_precision_devices) {
  // 触摸板/高精度滚轮给的是小数：1/3 格应给 1 行（trunc 到整数）。
  expect_eq(Terminal::wheel_scroll_lines(1.0f / 3.0f), 1, "三分之一格（上）");
  expect_eq(Terminal::wheel_scroll_lines(-1.0f / 3.0f), -1, "三分之一格（下）");
}

// ————————————————————————————————————————————————————————————————————————————
// ③ 与 ScrollView 的口径一致（同一份语义，不能两套）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(terminal_wheel_matches_scrollview_direction_convention) {
  // `ScrollView::on_event` 写的是 `scroll_by(-event.wheel_delta * step_)`，
  // 其中 `scroll_by` 增大偏移 = 向下滚 ⇒ `delta > 0`（向上滚）应让偏移**减小**。
  // 终端的偏移语义相反（"往上翻了多少行"），因此这里**同号**。
  //
  // 这条用例的作用是**钉住"两边是有意不同、且都跟系统同向"**，
  // 免得日后有人看到两处符号不同而"顺手统一"成同一个符号。
  const int up = Terminal::wheel_scroll_lines(1.0f);
  const int down = Terminal::wheel_scroll_lines(-1.0f);
  ::st::test::Registry::instance().count_check();
  if (!(up > 0 && down < 0)) {
    ::st::test::Registry::instance().record_failure(
        __FILE__, __LINE__,
        std::format("终端偏移语义应与 wheel_delta 同号（上正下负），实际 up={} down={}", up,
                    down));
  }
}
