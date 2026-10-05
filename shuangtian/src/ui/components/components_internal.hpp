#pragma once

/// 组件库内部共享助手（**不对外**）——组件实现文件公用的绘制 / 文本小工具。
///
/// 为什么要有这个头：这些助手此前在 `feedback` / `overlay` / `table` / `file_dialog` /
/// `menu` 等实现文件里**各抄一份**（逐字相同，或仅差参数），改一处要同步改五处。
/// `oriented()` 就是典型结局——它随 raster 层改为「覆盖率取绝对值」语义后已退化为恒等
/// 变换，五份副本却仍在**每次填充**里白跑一遍路径扁平化 + 反转重建。
///
/// 收敛到一处后，这类扫描与单点修复才成立（`scripts` 里的重复检测、后续的批量改动都受益）。
///
/// 风格与 `src/raster/rasterize_internal.hpp`、`src/pkg/pkg_internal.hpp` 一致：只被
/// `src/ui/components/*.cpp` 包含的私有头，不进 `include/st`（不属于对外 API）。

#include <string>
#include <string_view>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/element.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui::components_internal {

/// 文本端口取用（`RenderContext::text` 可为空 → 退化为 no-op 端口，布局仍可运行）。
[[nodiscard]] inline auto text_port_of(const RenderContext& context) -> const TextPort& {
  return context.text != nullptr ? *context.text : NullTextPort::instance();
}

/// 圆角矩形填充（**四角独立半径**；四角同半径时的简写在下方；`radius == 0` 即普通矩形）。
///
/// 历史（勿重蹈）：本函数曾与一份 `oriented(path)` 成对出现在各组件里——当时 raster 层
/// `Canvas::blend_coverage_row` 只接受**正绕向**覆盖率，而 `Path` 工厂
/// （`add_rect`/`add_rounded_rect`/`add_circle`）产出反向绕向，于是每个实现文件都抄一份
/// 「按扁平化折线反转重建」的归一层。raster 层现已统一为取绝对值语义
/// （`Canvas::blend_coverage_runs`、`rasterizer.cpp` 均 `std::abs(coverage)`），
/// 该层退化为恒等变换，已随本次收敛删除。
inline void fill_round_rect(raster::Surface& canvas, math::Rect rect, float top_left,
                            float top_right, float bottom_right, float bottom_left,
                            const raster::Paint& paint) {
  if (rect.is_empty()) return;
  raster::Path path;
  path.add_rounded_rect(rect, top_left, top_right, bottom_right, bottom_left);
  canvas.fill_path(path, paint);
}

/// 四角同半径的画笔填充（渐变压笔用）。
inline void fill_round_rect(raster::Surface& canvas, math::Rect rect, float radius,
                            const raster::Paint& paint) {
  fill_round_rect(canvas, rect, radius, radius, radius, radius, paint);
}

/// 四角同半径的纯色填充（`radius == 0` 即普通矩形）。
inline void fill_round_rect(raster::Surface& canvas, math::Rect rect, float radius,
                            math::Color color) {
  fill_round_rect(canvas, rect, radius, radius, radius, radius, raster::Paint::solid(color));
}

/// 单行文本（省略号截断 + 垂直居中，左对齐；标题 / 单元格 / 按钮标签通用）。
inline void draw_line(const RenderContext& context, raster::Surface& canvas, std::string_view text,
                      math::Rect box, float size, math::Color color) {
  if (text.empty() || box.width <= 0.0f || box.height <= 0.0f) return;
  const TextPort& port = text_port_of(context);
  const std::string clipped = port.ellipsize(text, size, box.width);
  if (clipped.empty()) return;
  const float line = port.line_height(size);
  const float y = box.y + (box.height - line) * 0.5f;
  port.draw(canvas, clipped, math::Point{box.x, y}, size, color);
}

/// 展开三角（目录行前导；`expanded` 时旋转 90°）。
inline void draw_triangle(raster::Surface& canvas, math::Point center, float radius,
                          math::Color color, bool expanded) {
  raster::Path path;
  const float r = radius;
  if (expanded) {
    // 向下：顶点在下
    path.move_to(math::Point{center.x - r, center.y - r * 0.6f});
    path.line_to(math::Point{center.x + r, center.y - r * 0.6f});
    path.line_to(math::Point{center.x, center.y + r * 0.6f});
  } else {
    // 向右：顶点在右
    path.move_to(math::Point{center.x - r * 0.6f, center.y - r});
    path.line_to(math::Point{center.x - r * 0.6f, center.y + r});
    path.line_to(math::Point{center.x + r * 0.6f, center.y});
  }
  path.close();
  canvas.fill_path(path, raster::Paint::solid(color));
}

/// 焦点环：控件矩形外扩 `metrics.focus_width / 2`，圆角跟随控件。
///
/// 圆角夹取到「短边一半 - 2」：半径等于半边的圆角路径描边会退化（无极值直线段时描边
/// 塌成发丝线，圆形指示器 / 圆形滑块尤其明显），故圆形控件用近似圆的圆角矩形画环。
/// 此前 input / select / slider / toggle / split_view 各抄一份（其中三者逐字相同、
/// input 与 split_view 是缺夹取的早期版本）——统一到夹取版，两版在既有调用点**结果逐值相同**。
inline void paint_focus_ring(const RenderContext& context, raster::Surface& canvas, math::Rect rect,
                             float radius) {
  const float width = context.theme.metrics().focus_width;
  if (width <= 0.0f || rect.is_empty()) return;
  const float offset = width * 0.5f;
  const math::Rect outer = rect.inflate(offset);
  const float limit = std::max(std::min(outer.width, outer.height) * 0.5f - 2.0f, 0.0f);
  raster::Path ring;
  ring.add_rounded_rect(outer, std::min(radius + offset, limit));
  canvas.stroke_path(ring, raster::Paint::solid(context.theme.colors().focus_ring), width);
}

/// 圆角描边：线宽完全落在矩形内侧。
inline void paint_outline(raster::Surface& canvas, math::Rect rect, float radius, math::Color color,
                          float width) {
  if (width <= 0.0f || color.a == 0U || rect.is_empty()) return;
  const float half = width * 0.5f;
  raster::Path outline;
  outline.add_rounded_rect(rect.inset(math::Insets::all(half)), radius > half ? radius - half : 0.0f);
  canvas.stroke_path(outline, raster::Paint::solid(color), width);
}

}  // namespace st::ui::components_internal
