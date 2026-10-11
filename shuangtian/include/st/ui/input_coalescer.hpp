#pragma once

/// **高频输入的帧内合并**（`InputCoalescer`）：把一帧里到达的多个指针移动并成一次派发。
///
/// ## 为什么这值得在框架层
///
/// Windows 的 `WM_MOUSEMOVE` 在拖动时可以达到 **125~1000 次/秒**（普通鼠标约 125，
/// 高刷游戏鼠标 500~1000），而后端**不做任何合并**——每一个消息都变成一个 `MouseMove`
/// 事件、走完整条命中与派发路径。绘制类应用（画笔、框选、拖拽调参）因此在一次
/// 长笔画里要处理几百次事件，而每次都要：算坐标 → 改文档 → 重绘。
///
/// 三重代价（都不是"慢一点"那么轻）：
///
/// 1. **重绘**：一帧内推进几百次"重绘"请求，实际只画得出一帧——其余是白算；
/// 2. **笔迹质量**：相邻两次移动常常只差 1px，靠笔刷插值补出来的线**不如**
///    一次跨 8px 的长线段准（插值是近似，长线段是精确的几何）；
/// 3. **撤销粒度**（最严重）：命令式撤销里"一次移动 = 一条命令"，画一笔会产生
///    **几百条历史**——用户按撤销只退 1px，会判定"撤销坏了"。而这**不该**由每个
///    应用各自用 `MergePolicy` 去补救：那是事后合并历史，而根因是事件太多。
///
/// ## 语义（用之前必须知道的两条）
///
/// - **保留首尾，不只保留最新**：合并后的事件带 `from` 字段（本帧内最早的位置），
///   派发方据此知道"这一帧指针从哪走到哪"。只给最新位置会**丢掉本帧走过的整段路**——
///   快速划动时表现为笔迹断成一段一段。
/// - **派发时机在帧末**：这样一帧内到达的都被合并。代价是"位置比事件落后不到一帧"
///   （≤16ms），而这对指针交互不可感知（鼠标本身也有 ~8ms 的采样周期）。
///
/// ## 与"指针捕获"的关系
///
/// 拖拽归属（按下后移出元素仍收 move/up）**框架已经做了**（`UiRoot::pressed_`），
/// 不需要另一个原语。本组件只解决"太多"，不解决"归属"。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 一帧内合并后的指针移动。
struct CoalescedMove {
  /// 本帧内**最早**的位置（一次都没合并时等于 `latest`）。
  ///
  /// 绘制类应用要靠它画"这一帧走过的线段"——只给最新位置会丢掉整段路。
  math::Point earliest{};
  /// 本帧内**最新**的位置（真正决定指针在哪）。
  math::Point latest{};
  /// 本帧内被合并掉的移动次数（`0` = 只有一次移动，不算合并）。
  std::size_t merged{0};
  /// 合并期间指针是否**离开过**组件（有过一次带 `left` 标记的记录）。
  bool left{false};
  /// 最后一次移动携带的修饰键状态（合并只保留最新的：按住 Ctrl 是"当前状态"，
  /// 而不是"某个历史瞬间的状态"）。
  bool ctrl{false};
  bool shift{false};
  bool alt{false};
  bool meta{false};

  [[nodiscard]] auto moved() const noexcept -> bool {
    return earliest.x != latest.x || earliest.y != latest.y;
  }
};

/// 合并器：喂进每一次移动，帧末取一次结果。
///
/// 它**不拥有**派发逻辑（那是 `UiRoot` 的事）：这样它能被单独测试，
/// 而 `UiRoot` 只多两行（帧内 `feed`、帧末 `take`）。
class InputCoalescer {
 public:
  /// 记录一次指针移动（帧内可以调很多次）。
  void feed(math::Point position, bool ctrl = false, bool shift = false, bool alt = false,
            bool meta = false);
  /// 记录一次"指针离开"（`MouseLeave`）：合并结果要反映它。
  void feed_leave();

  /// 取走本帧的合并结果并清空（帧末调一次）。
  ///
  /// 返回 `nullopt` = 本帧没有移动（不派发——**不要**拿旧位置凑一次事件，
  /// 那会让"指针没动"与"指针动了一点"变成同一件事）。
  [[nodiscard]] auto take() -> std::optional<CoalescedMove>;

  /// 本帧是否收到了移动（不取走）。
  [[nodiscard]] auto pending() const noexcept -> bool { return has_any_; }
  /// 本帧被合并掉的移动次数（诊断/测试用）。
  [[nodiscard]] auto merged_count() const noexcept -> std::size_t { return merged_; }

  /// 丢弃未取走的内容（如焦点丢失、窗口失活：那一帧的移动不该再派发）。
  void reset() noexcept;

 private:
  bool has_any_{false};
  bool left_{false};
  std::size_t merged_{0};
  math::Point earliest_{};
  math::Point latest_{};
  bool ctrl_{false};
  bool shift_{false};
  bool alt_{false};
  bool meta_{false};
};

}  // namespace st::ui
