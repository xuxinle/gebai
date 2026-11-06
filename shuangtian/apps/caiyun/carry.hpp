#pragma once

/// 裁云 · **跨场承接层**（carry）：让两个分镜之间有一个东西“活下来并变成下一个”。
///
/// ## 为什么要单开一层（而不在场景里画）
///
/// 裁云的分镜是**独立渲染单位**：`render_frame` 逐幕渲染，再在边界做交叉淡入。
/// 于是两个画面之间没有任何东西连续——这正是成熟动效片的验收器判“幻灯片”的
/// 形态（后一画面把前一画面**替掉**了）。
///
/// 承接不能住在任何一幕里：它的生命周期**穿透边界**。所以它是合成之上的一层：
///
/// ```
/// 逐幕渲染 + 交叉淡入        ← 原样不动
///      ↓
/// 承接层（本文件）           ← 新增，只在声明了 carry 的边界工作
///      ↓
/// 全片淡入淡出 / 包装
/// ```
///
/// 不声明 `carry` 时本层**一笔不画**，成片与旧版逐像素一致。
///
/// ## 时间的定义
///
/// `CarryOut::at/dur` 是**成片时间**（不是幕内时间）。原因：承接跨越两幕，
/// 而两幕的幕内时间在边界处不连续，"幕内第 X 秒"无法同时指到边界两侧。
///
/// ## 与验收器的关系
///
/// 承接图元在每一帧都以**同一个名字**进元素轨迹（`CarryTracker`）。
/// 验收器据此认定“这个身份活过了边界”，并按它在这段里的位移/缩放判定
/// `carried`。这与成熟动效片的口径对齐：**有东西活下来，并且动了**。

#include <string>
#include <vector>

#include "director.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/surface.hpp"
#include "st/ui/text_port.hpp"

namespace caiyun {

/// 承接图元在某一时刻的几何（**设计坐标**，与图层的 x/y/w/h 同口径）。
struct CarryPose {
  /// 是否在活动区间内（`false` 时调用方不应画、也不应进轨迹）
  bool active{false};
  float x{0.0f};
  float y{0.0f};
  float w{0.0f};
  float h{0.0f};
  float radius{0.0f};
  /// 变形进度 0~1（已缓动）
  float k{0.0f};
  /// **不透明度**（`fade_out` > 0 时，变形末段从 1 降到 0）。
  ///
  /// 为何由求值层给而不是让调用方算：它是承接语义的一部分（“交待完就让位”），
  /// 而调用方（合成层）只该知道“画不画”，不必再知道“怎么算淡出”。
  float alpha{1.0f};
  /// 参与验收的身份名（`CarryOut::name`）
  const char* name{nullptr};
};

/// 求 `carry` 在成片时间 `time` 的形状（纯函数，可单测）。
///
/// `design_w/design_h` 用于解析 `"50%"` 这类画面比例；纯数字按设计像素。
/// 不在活动区间时返回 `active=false`（其余字段无意义）。
[[nodiscard]] auto evaluate_carry(const CarryOut& carry, double time, float design_w,
                                  float design_h) -> CarryPose;

/// 把「百分比或像素」解析成设计像素（与 `layer.cpp` 同口径）。
///
/// 单独暴露是为了让测试能钉住解析：`"50%"` 与 `960` 在 1920 宽下必须等价。
[[nodiscard]] auto resolve_carry_dim(std::string_view raw, float base, float fallback) -> float;

/// 缓动名 → 函数。未知名返回 `nullptr`（调用方用默认 `expo_out`）。
using CarryCurve = float (*)(float);
[[nodiscard]] auto carry_curve_by_name(std::string_view name) -> CarryCurve;

/// 把承接图元画进画布。
///
/// **实现放在 `motion.cpp`**（它拥有 `Painter` 与文字度量），因为承接的绘制必须与
/// 幕内图层**同一口径**（设计坐标入、内部乘 `scale`）。若在 `carry.cpp` 里另起
/// 一套坐标换算，两处必然漂移——而漂移在 1080p 导出（scale=1）下看不出来，
/// 只在预览与帧序列里现形。
void paint_carry(st::raster::Surface& target, const CarryOut& carry, const CarryPose& pose,
                 const st::ui::TextPort* text_port, float alpha);

/// 一帧里所有承接图元的轨迹记录（供 `--track-elements` 导出）。
///
/// 为何承接要单独收：它不属于任何一幕，`render_scene` 的图层轨迹收集器
/// 根本看不到它。若不给它一条轨迹，验收器就不知道“这个身份活过边界”——
/// 那么我们辛苦做的承接在自动化验收里等于不存在（这是个很容易漏的接线）。
struct CarryTrack {
  std::string name{};
  float x{0.0f};
  float y{0.0f};
  float w{0.0f};
  float h{0.0f};
};

/// 收集一帧内**所有**活动中的承接（按场景顺序）。
///
/// 返回空 = 本帧没有承接在跑（绝大多数帧如此）。
[[nodiscard]] auto collect_active_carries(const Script& script, double time, float design_w,
                                          float design_h) -> std::vector<CarryTrack>;

}  // namespace caiyun
