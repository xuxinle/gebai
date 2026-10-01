#pragma once

/// 矢量路径：子路径（move/line/quad/cubic/close）+ 常用形状构造 + 扁平化（贝塞尔 → 折线）。
/// 路径本身只是几何描述，不含样式；填充/描边由 `Canvas` 完成。

#include <cstdint>
#include <span>
#include <vector>

#include "st/math/geometry.hpp"

namespace st::raster {

struct PathCommand {
  enum class Kind : std::uint8_t { MoveTo, LineTo, QuadTo, CubicTo, Close };

  Kind kind{Kind::MoveTo};
  math::Point p1{};
  math::Point p2{};
  math::Point p3{};
};

/// 扁平化后的折线（`closed` 表示首尾相连）。
struct Polyline {
  std::vector<math::Point> points{};
  bool closed{false};
};

class Path {
 public:
  Path() = default;

  auto move_to(math::Point point) -> Path&;
  auto line_to(math::Point point) -> Path&;
  /// 二次贝塞尔（控制点、终点）。
  auto quad_to(math::Point control, math::Point end) -> Path&;
  /// 三次贝塞尔（两个控制点、终点）。
  auto cubic_to(math::Point control1, math::Point control2, math::Point end) -> Path&;
  auto close() -> Path&;

  /// 圆角矩形（四角同半径；半径自动夹取到短边一半）。
  auto add_rounded_rect(math::Rect rect, float radius) -> Path&;
  /// 四角独立半径（顺时针：左上/右上/右下/左下）。
  auto add_rounded_rect(math::Rect rect, float top_left, float top_right, float bottom_right,
                        float bottom_left) -> Path&;
  auto add_rect(math::Rect rect) -> Path&;
  auto add_circle(math::Point center, float radius) -> Path&;
  auto add_ellipse(math::Rect bounds) -> Path&;
  /// 圆弧（角度制，0° 指向 +x 轴、顺时针为正）。
  auto add_arc(math::Point center, float radius, float start_degrees, float end_degrees) -> Path&;
  /// 整段追加另一条路径（平移可选）。
  auto add_path(const Path& other, float dx = 0.0f, float dy = 0.0f) -> Path&;

  void clear();
  [[nodiscard]] auto is_empty() const noexcept -> bool { return commands_.empty(); }
  [[nodiscard]] auto commands() const noexcept -> std::span<const PathCommand> { return commands_; }
  /// 粗略包围盒（由控制点包围盒得出，含贝塞尔凸包）。
  [[nodiscard]] auto bounds() const noexcept -> math::Rect;
  /// 缩放（DPI 缩放/图标缩放；对全部控制点等比缩放）。
  [[nodiscard]] auto scaled(float factor) const -> Path;
  [[nodiscard]] auto translated(float dx, float dy) const -> Path;
  /// 逆时针（负向）绘制为正向——用于修正图标数据的绕向。
  void reverse();

  /// **逐控制点只读访问**：顺序为「按命令流展开」——
  /// `MoveTo`→1 点、`LineTo`→1 点、`QuadTo`→2 点（控制点、终点）、`CubicTo`→3 点、
  /// `Close`→0 点。调用方因此可以沿 `commands()` 同步推导每个点的**角色**
  /// （直线端点还是贝塞尔控制点），比额外维护一张角色表可靠。
  ///
  /// 用途：字形网格拟合要在像素空间微调**直线端点**的位置（把笔画边缘吸附到
  /// 像素网格），而曲线控制点只能跟着平移——那就必须先能区分两者，并能按同一顺序写回。
  [[nodiscard]] auto raw_points() const -> std::vector<math::Point>;
  /// 按 `raw_points()` 的同一顺序写回（数量不一致时不修改并返回 false）。
  auto set_raw_points(std::span<const math::Point> points) -> bool;

  /// 扁平化为折线（`tolerance` 为最大弦高误差，像素；0.1~0.3 通常足够）。
  [[nodiscard]] auto flatten(float tolerance) const -> std::vector<Polyline>;
  /// 扁平化并计算精确包围盒（基于折线）。
  [[nodiscard]] auto flattened_bounds(float tolerance) const -> math::Rect;

 private:
  std::vector<PathCommand> commands_{};
};

/// 常用形状工厂（图标与组件高频使用）。
[[nodiscard]] auto make_rounded_rect(math::Rect rect, float radius) -> Path;
[[nodiscard]] auto make_circle(math::Point center, float radius) -> Path;
[[nodiscard]] auto make_line(math::Point from, math::Point to) -> Path;

}  // namespace st::raster
