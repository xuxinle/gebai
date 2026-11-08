#pragma once

/// 窗口后端之间共用的**一份**「连击计数」：把一串按放翻译成「这是第几击」。
///
/// ## 为什么必须只有一份（实测代价）
///
/// 三个后端的窗口系统能力不同：Win32 的窗口类带 `CS_DBLCLKS`，系统会把第二次按下
/// **替换**成 `WM_LBUTTONDBLCLK`（不是额外多一条）；X11/Wayland 没有这个能力，得自己
/// 按时间 + 位移算。而「一次物理手势算几次点击」必须是**同一笔账**——分头算就会
/// 一个平台双击 = 2、另一个 = 3。
///
/// 实测（2026-11-08，真窗口 + 真鼠标 `SendInput`，判据是应用自己的激活计数）：
/// Win32 侧一次双击发出 `Click` / `DoubleClick` / `Click` **三个**点击族事件，
/// `Button` 因此激活 **3** 次；同一手势在 X11 侧只有 **2** 次（`Click` + `DoubleClick`）。
///
/// ## 后端契约
///
/// 一次按放 = 一条 `MouseDown` + 一条 `MouseUp` + **恰好一条**点击族事件
/// （`Click` / `DoubleClick` / `TripleClick`，按 `count()` 升级）。
/// 点击族是这次按放的**收尾**，不是「又按了一次」——所以：
///
/// - 每次按放都要有 `MouseDown`（`WM_LBUTTONDBLCLK` 代替了按下消息，也要补一条）；
/// - 每次按放的 `MouseUp` 只配**一条**点击族事件（不要再额外补 `Click`）。
///
/// 谁在用：`platform_win32.cpp` / `platform_x11.cpp` / `platform_wayland.cpp`。
/// 语义与 `DESIGN.md`「输入」一章的「一次按放是一个手势」同一条口径。

#include <cmath>
#include <cstdint>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::shell::detail {

/// 连击判定的两个阈值。
struct ClickConfig {
  /// 连击时间窗（毫秒）。Win32 传系统 `GetDoubleClickTime()`（用户可调，默认 500），
  /// 其余平台用桌面通行值。
  double window_ms{400.0};
  /// 位置容差（**逻辑**像素）：两次按下之间指针挪动超过它就当新手势。
  /// 取桌面通行量级（GTK/X11 用 4px），太小会把手抖误判成两次独立点击。
  double slop{4.0};
};

/// 连击计数器。**无平台依赖、可单测**——这正是把它抽出来的理由。
class ClickStreak {
 public:
  /// 一次按压开始。
  ///
  /// @param at_ms 该按下的时间戳（毫秒，单调时钟；`steady_clock` 之类）
  /// @param at    按下位置的**逻辑**坐标
  /// @param config 时间窗与容差
  /// @param confirmed_repeat 窗口系统**已经判定**这是连击的又一击
  ///        （Win32 的 `WM_LBUTTONDBLCLK`——它代替了第二次的按下消息）。
  ///        传 `true` 时结果至少是 2，即便时间戳看起来不像连击：
  ///        系统的判定优先于我们的启发式（它考虑用户设置的鼠标速度）。
  /// @return 这是第几击（1 起）
  auto press(double at_ms, math::Point at, const ClickConfig& config,
             bool confirmed_repeat = false) -> int {
    const bool continued = has_last_ && (at_ms - last_ms_) <= config.window_ms &&
                           std::abs(static_cast<double>(at.x) - static_cast<double>(last_.x)) <=
                               config.slop &&
                           std::abs(static_cast<double>(at.y) - static_cast<double>(last_.y)) <=
                               config.slop;
    if (continued) {
      ++count_;
    } else {
      count_ = 1;
    }
    if (confirmed_repeat && count_ < 2) count_ = 2;
    last_ms_ = at_ms;
    last_ = at;
    has_last_ = true;
    return count_;
  }

  /// 当前连击数（1 起；未按下过为 0）。
  [[nodiscard]] auto count() const noexcept -> int { return count_; }

  /// 松手时要补发的那**一条**点击族事件。
  ///
  /// 三击以上一律 `TripleClick`：`EventKind` 只到三击，而"第 4 击"对组件没有
  /// 更多语义（与 X11/Wayland 的既有口径一致）。
  [[nodiscard]] auto click_kind() const noexcept -> ui::EventKind {
    if (count_ >= 3) return ui::EventKind::TripleClick;
    if (count_ == 2) return ui::EventKind::DoubleClick;
    return ui::EventKind::Click;
  }

  /// 清空（窗口失活、指针离窗、按放被系统取消时用——不能把中断前后的两次按下
  /// 连成一击）。
  void reset() noexcept {
    count_ = 0;
    last_ms_ = 0.0;
    last_ = math::Point{};
    has_last_ = false;
  }

 private:
  int count_{0};
  double last_ms_{0.0};
  math::Point last_{};
  bool has_last_{false};
};

}  // namespace st::shell::detail
