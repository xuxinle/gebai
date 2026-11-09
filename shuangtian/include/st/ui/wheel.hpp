#pragma once

/// @file wheel.hpp
/// 滚轮事件的**公共约定**（一份实现）。
///
/// ## 为什么值得收
///
/// 滚轮方向在本仓踩过两次**静默**的坑，都是“符号写反”：
///
/// - `table.cpp` 的横向滚动曾用 `+`，与 `scroll.cpp` 的明文约定相反
///   （2026-10-14 修）；
/// - `file_dialog.cpp` 同一天修过同一类。
///
/// 症状一模一样且极难察觉：事件**被消费了**（`handled = true`）、界面**有反应**，
/// 只是偏移一直被夹在 0——页面下半截永远看不到。任何“像素正确”的测试都发现不了。
///
/// 步长来源也有过漂移：`scroll.cpp` 读主题 `Metrics::wheel_step`（可换主题调手感），
/// 而 `table.cpp` / `tabs.cpp` 用各自的 `constexpr 48.0f`——换主题时一部分组件跟得上、
/// 另一部分不跟。这里统一成「主题优先、无主题用同一个兑底值」。
///
/// ## 约定
///
/// `Event::wheel_delta` 与系统一致：**向上/前推为正，向下/后推为负**
/// （Win32 `WM_MOUSEWHEEL` 向上 +120；X11 button 4 也是上）。
///
/// 而“内容偏移”是**向下增大**的（滚到底 = offset 最大）。两者方向相反，
/// 所以纵向滚动一律 `offset -= delta * step`。
///
/// ## 用法
///
/// ```cpp
/// case EventKind::Wheel: {
///   // 自身不可滚时返回 false，把滚轮让给外层容器（嵌套滚动必须如此）。
///   const float next = st::ui::wheel_target(scroll_offset_, event, max_scroll(), context.theme);
///   if (next < 0.0f) return false;   // 无内容可滚
///   scroll_to(next);
///   return true;
/// }
/// ```

namespace st::ui {

/// 滚轮一格在**无主题**时的兑底步长（px）。与历史值一致（48）。
inline constexpr float kWheelFallbackStep = 48.0f;

/// 从主题取滚轮步长（不可用/非正时回到兑底值）。
template <typename ThemeT>
[[nodiscard]] auto wheel_step(const ThemeT& theme) noexcept -> float {
  const float step = theme.metrics().wheel_step;
  return step > 0.0f ? step : kWheelFallbackStep;
}

/// 把一次滚轮事件换算成**目标滚动偏移**。
///
/// @param offset      当前偏移（向下增大）
/// @param wheel_delta 事件里的原始增量（向上为正）
/// @param max_offset  可滚上限（`<= 0` = 内容没超高，本不该滚）
/// @param theme       取步长的主题
/// @return 夹取后的目标偏移；`max_offset <= 0` 时返回 `-1`（**调用方据此 return false
///         让事件冒泡给外层滚动容器**——这是嵌套滚动的正解，不要静默消费）
/// @param lines       事件自带的行数（Windows 一格 = 120 ⇒ 1 行；多数后端给 ±1）。
///                    传 0 表示忽略，按 1 行/格处理。
template <typename EventT, typename ThemeT>
[[nodiscard]] auto wheel_target(float offset, const EventT& event, float max_offset,
                                const ThemeT& theme, float lines = 0.0f) noexcept -> float {
  if (max_offset <= 0.0f) return -1.0f;
  const float step = wheel_step(theme);
  // `wheel_delta` 向上为正、偏移向下增大 ⇒ 取反。用 `lines` 缩放（0 = 按 1 行）。
  const float amount = (lines > 0.0f ? lines : 1.0f) * event.wheel_delta * step;
  const float target = offset - amount;
  if (target < 0.0f) return 0.0f;
  return target > max_offset ? max_offset : target;
}

}  // namespace st::ui
