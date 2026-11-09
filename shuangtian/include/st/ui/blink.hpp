#pragma once

#include <cmath>

/// @file blink.hpp
/// 光标/指示灯的**周期闪烁**协议（一份实现）。
///
/// ## 为什么必须收成一份
///
/// 这个协议由四步组成，缺一步就坏，而坏法各不相同：
///
/// 1. 用 `fmod(time_seconds, period)` 算相位 → 得可见性；
/// 2. **登记下一次翻转时刻**（`request_wake_at`）——漏了它，静止时帧循环已收敛，
///    光标就冻在恰好可见的那一相位（看上去“一直亮着但不闪”）；
/// 3. 只在**相位真翻转**的那帧上报损坏区——每帧报会与帧间损坏判据互相触发成
///    每帧续帧循环（实测 headless 35fps 空转）；
/// 4. 损坏区要**小**（光标那一小条）——报整个 bounds 会越过合并阈值退化成整帧。
///
/// 实测（2026-11）收拢前全仓有 **7 份**各自手写的实现，其中：
/// - `input.cpp` 内部逐字重复了两遍；
/// - `code_editor` 用 duty 0.5、`input` 用 **0.55**、`terminal` 用字面量 `0.5`
///   ——**同一个概念三种取值**，而 code_editor 的注释还写着“与那两处刻意一致，
///   前半可见”，注释本身就是错的；
/// - `number_input` / `search_box` / `tag_input` 三处漏了第 2 步，改用
///   `request_animation()`（**连续续帧**）——光标会闪，但帧循环永不停歇
///   （正是“gbcode CPU 70%”那一类形态）。
///
/// ## 用法
///
/// ```cpp
/// if (focused_) {
///   const BlinkState blink = blink_.tick(context.time_seconds, *this);
///   if (blink.needs_damage()) record_damage_rect(caret_band());
///   if (blink.visible) draw_caret();
/// }
/// ```
///
/// `tick` 内部已经做了第 2 步（登记唤醒），调用方**无法遗漏**。
/// 损坏区由调用方按自己的几何上报（第 4 步）——`BlinkCursor` 不知道光标在哪。

namespace st::ui {

/// 闪烁参数。默认与既有 4 处实现的历史口径一致（1.0s / 前半可见）。
struct BlinkSpec {
  double period_seconds{1.0};
  /// 一个周期内**可见**所占比例（0~1）。0.5 = 前半可见。
  double duty{0.5};

  /// 光标口径（1.0s / 前半可见）。全仓光标默认用它，保证同屏多个光标节奏一致。
  [[nodiscard]] static constexpr auto caret() noexcept -> BlinkSpec { return BlinkSpec{}; }
};

/// 一次 `tick` 的结果。
struct BlinkState {
  /// 本帧是否应把光标画出来。
  bool visible{true};
  /// 本帧是否是**相位翻转帧**（只有这种帧才该上报损坏区）。
  bool phase_changed{false};

  /// 本帧需要上报损坏区吗（等价于 `phase_changed`，语义化命名）。
  [[nodiscard]] auto needs_damage() const noexcept -> bool { return phase_changed; }
};

/// 闪烁状态机：持有“上次相位”与周期参数。
///
/// **每个闪烁源一个实例**（每个光标一个）——它无锁、无全局状态，可安全放进元素成员。
class BlinkCursor {
 public:
  explicit BlinkCursor(BlinkSpec spec = BlinkSpec::caret()) noexcept : spec_(spec) {}

  /// 推进一帧。
  ///
  /// @param time_seconds 应用帧时间轴（`RenderContext::time_seconds`，单调秒）
  /// @param owner        闪烁的宿主元素——`request_wake_at` 登记在它身上
  ///
  /// **登记下一次唤醒是本函数的职责**（见文件头第 2 步）：只要调用 `tick`，
  /// 该元素就会在下一个翻转点被准时唤醒，不会出现“光标冻住”。
  template <typename ElementT>
  [[nodiscard]] auto tick(double time_seconds, const ElementT& owner) -> BlinkState {
    const double period = spec_.period_seconds > 0.0 ? spec_.period_seconds : 1.0;
    const double phase = std::fmod(time_seconds, period);
    const double visible_span = period * spec_.duty;
    const bool visible = phase < visible_span;

    // 下一次翻转点：可见期内 → 到本相位结束；否则 → 到下个周期开始。
    const double next_flip = time_seconds + (visible ? visible_span - phase : period - phase);
    owner.request_wake_at(next_flip);

    const int now = visible ? 1 : 0;
    const bool changed = last_visible_ != now;
    last_visible_ = now;
    last_flip_at_ = next_flip;
    return BlinkState{.visible = visible, .phase_changed = changed};
  }

  /// 应用失去焦点/进入只读时调用：把状态复位成“未知”，下次 `tick` 必然算作翻转
  /// （于是会立刻上报一次损坏区，光标重新出现得以及时画出）。
  void reset() noexcept { last_visible_ = -1; }

  [[nodiscard]] auto spec() const noexcept -> const BlinkSpec& { return spec_; }
  void set_spec(BlinkSpec spec) noexcept { spec_ = spec; }
  /// 上次算出的下一翻转时刻（诊断/测试用；未 tick 过为 -1）。
  [[nodiscard]] auto next_flip_at() const noexcept -> double { return last_flip_at_; }
  /// 上一帧算出的可见性（0/1；-1 = 未知/未 tick）。
  ///
  /// 供**属性面**播报用：“它在不在闪”是行为契约，而光标每秒只翻两次的像素变化
  /// 很容易在截图/哈希里跨过翻转点而看不见（实测连拍 10 张全一样）。
  [[nodiscard]] auto last_visible() const noexcept -> int { return last_visible_; }

 private:
  BlinkSpec spec_{};
  /// -1 = 未知（下一帧必判为翻转）。
  int last_visible_{-1};
  double last_flip_at_{-1.0};
};

}  // namespace st::ui
