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

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/math/path.hpp"
#include "st/ui/element.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/text_selection.hpp"
#include "st/ui/theme.hpp"

namespace st::ui::components_internal {

/// 文本端口取用（`RenderContext::text` 可为空 → 退化为 no-op 端口，布局仍可运行）。
[[nodiscard]] inline auto text_port_of(const RenderContext& context) -> const TextPort& {
  return context.text != nullptr ? *context.text : NullTextPort::instance();
}

/// 色调短名 → `Tone`（控制通道属性面解析；未知名返回 `nullopt`）。
/// 此前 feedback / overlay 各拄一份逐字相同的实现。
[[nodiscard]] inline auto tone_from_name(std::string_view name) -> std::optional<Tone> {
  if (name == "default") return Tone::Default;
  if (name == "muted") return Tone::Muted;
  if (name == "faint") return Tone::Faint;
  if (name == "primary") return Tone::Primary;
  if (name == "accent") return Tone::Accent;
  if (name == "success") return Tone::Success;
  if (name == "warning") return Tone::Warning;
  if (name == "danger") return Tone::Danger;
  if (name == "on_primary") return Tone::OnPrimary;
  return std::nullopt;
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
  math::Path path;
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

/// **顶部内高光**（亚克力的「玻璃边缘」）：在圆角内沿顶边画一条
/// `width` 高的横向渐变亮线（两端 alpha 归零）。
///
/// 为什么不是一条等宽实线：等宽线在两端会被读成「多了一道边框」，
/// 而玻璃边缘的受光是中间强、向两侧衰减的。渐变两端归零后自然消失在圆角处，
/// 也就不需要在圆角处另做处理（仍会裁剪到圆角内，免得直角探出）。
///
/// 与 `Element::paint_box` 里那份是同一套几何：组件自绘底时（弹层/菜单/对话框）
/// 没有 `style_.top_highlight` 可填，只能自己调本函数，两边必须看起来一样。
inline void draw_glass_edge(const RenderContext& context, raster::Surface& canvas, math::Rect rect,
                            float radius, float width = 1.0f) {
  const math::Color edge = context.theme.colors().highlight;
  if (edge.a == 0U || width <= 0.0f || rect.is_empty()) return;
  const math::Color fading = edge.with_alpha(0);
  const math::Rect band{rect.x, rect.y, rect.width, width};
  const raster::Gradient gradient = raster::Gradient::linear(
      math::Point{band.x, band.y}, math::Point{band.right(), band.y},
      std::vector<raster::GradientStop>{raster::GradientStop{0.0f, fading},
                                        raster::GradientStop{0.5f, edge},
                                        raster::GradientStop{1.0f, fading}});
  if (radius > 0.0f) canvas.push_clip_rounded_rect(rect, radius);
  canvas.fill_rect(band, raster::Paint::with_gradient(gradient));
  if (radius > 0.0f) canvas.pop_clip();
}

/// 弹层表面四件套：阴影（环境层 + 关键层）→ **毛玻璃**→ 底色 → 玻璃边缘。
///
/// 存在的理由：菜单 / 对话框 / 气泡 / 命令面板五处各自抄过一段几乎相同的
/// 「画阴影 + 填底」，而亚克力把「抬升感」托付给了这几步的组合——
/// 分散写的话，改一处漏一处的结果是“某个浮层没有边缘光”，肉眼看得出、测试难抓。
///
/// ## 毛玻璃为什么在这里做
///
/// 它模糊的是**这一步之前已经画在画布上的像素**（弹层背后的内容），
/// 所以必须在**画底色之前**执行——顺序反了就只能模糊到自己的底，白做。
/// 后端不支持时（`supports_blur` 为假、或半径令牌为 0）自动跳过，
/// 下面的实色底照常画，观感退回“不透明弹层”而不是报错或半透明裸底。
inline void paint_raised_surface(const RenderContext& context, raster::Surface& canvas,
                                 math::Rect rect, float radius, const Shadow& shadow) {
  if (rect.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  // **辉光在最外层**（在阴影之前画）：它是“氛围”，几何固定为
  // 阴影半径的 2 倍、无偏移，靠 alpha 调强度。
  //
  // 为何不是“作为阴影的第三层”：`draw_shadow_layered` 的几何参数量是
  // “关键层 + 环境层”两档，再塞一层会把那个语义弄乱（而且辉光的偏移与阴影不同）。
  // 强度为 0（缺省）时完全不画，绝大多数主题零开销。
  if (colors.shadow_glow.a != 0U && metrics.shadow_glow_strength > 0.0f) {
    const float strength = metrics.shadow_glow_strength;
    const float glow_blur = shadow.visible() ? std::max(shadow.blur2, shadow.blur) * 2.0f : 24.0f;
    const math::Color glow{colors.shadow_glow.r, colors.shadow_glow.g, colors.shadow_glow.b,
                           static_cast<std::uint8_t>(std::min(
                               255.0f, static_cast<float>(colors.shadow_glow.a) * strength))};
    canvas.draw_shadow(rect, radius, glow_blur, glow, math::Point{0.0f, 0.0f});
  }
  if (shadow.visible()) {
    const bool has_ambient = shadow.second_visible();
    canvas.draw_shadow_layered(
        rect, radius, shadow.color, shadow.blur,
        math::Point{shadow.offset_x, shadow.offset_y},
        has_ambient ? shadow.color2 : math::Color{0, 0, 0, 0},
        has_ambient ? shadow.blur2 : 0.0f,
        math::Point{shadow.offset2_x, shadow.offset2_y});
  }
  // 毛玻璃：先模糊背板，再叠半透明底色。
  //
  // 裁剪到圆角内是**必须的**：`blur_region` 只按矩形跑模糊，不认圆角，
  // 不裁的话四个角上的模糊会溢出到圆角之外，看起来像“毛边”。
  const bool frosted = metrics.overlay_blur_radius > 0.0f && canvas.supports_blur();
  if (frosted) {
    if (radius > 0.0f) canvas.push_clip_rounded_rect(rect, radius);
    (void)canvas.blur_region(rect, metrics.overlay_blur_radius);
    if (radius > 0.0f) canvas.pop_clip();
    // 着色层：让模糊后的背板沉下去，文字才读得出来。
    const math::Color tint =
        colors.surface_raised.with_alpha_f(math::clamp01(metrics.overlay_blur_tint));
    fill_round_rect(canvas, rect, radius, tint);
  } else {
    fill_round_rect(canvas, rect, radius, colors.surface_raised);
  }
  draw_glass_edge(context, canvas, rect, radius);
}

/// 单行文本（省略号截断 + **按墨迹区**垂直居中，左对齐；标题 / 单元格 / 按钮标签通用）。
///
/// 居中口径由 `st::ui::centered_line_top` 统一给（见 `text_port.hpp`）——那是全仓唯一的
/// "把一行字放进盒子"公式；按钮 / 菜单 / 表格各自算一份时，改一处漏一处的表现
/// 就是"某些组件的字偏一两像素"。
inline void draw_line(const RenderContext& context, raster::Surface& canvas, std::string_view text,
                      math::Rect box, float size, math::Color color) {
  if (text.empty() || box.width <= 0.0f || box.height <= 0.0f) return;
  const TextPort& port = text_port_of(context);
  const std::string clipped = port.ellipsize(text, size, box.width);
  if (clipped.empty()) return;
  const float y = centered_line_top(port, clipped, size, box.y, box.height);
  port.draw(canvas, clipped, math::Point{box.x, y}, size, color);
}

/// 展开三角（目录行前导；`expanded` 时旋转 90°）。
inline void draw_triangle(raster::Surface& canvas, math::Point center, float radius,
                          math::Color color, bool expanded) {
  math::Path path;
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

/// 焦点环：贴住控件外沿的**一道**环，并盖住控件自己的描边。
///
/// 用户报的“嵌套一层边框”是怎么来的：控件自己的 border 占 `[R-bw, R]`（环填充向内），
/// 而环从 `R` 往外画 —— 两条**相邻但不同色**的带子并排：一道灰框 + 外面再一圈蓝框，
/// 读起来就是“边框外面又套了一层”。
///
/// 修法不是把环收进去（那会让无描边的控件丢掉外环，`Checkbox`/`Radio`/`Switch`
/// 的环会缩到指示器内部），而是把环的**起点从 `R` 移到 `R-bw`**：环完整盖住
/// 控件自己的描边。于是：
/// - 有描边的控件（`Secondary` 按钮 / 输入框 / 复选框）：看到**一道加粗的环**；
/// - 无描边的控件：看到**一道完整的外环**（与以前外层位置一致）。
///
/// 环宽取 `max(focus_width, border_width)`：比 border 细的环会把 border 的外半个像素
/// 露在环外，又变回两道线。
///
/// 圆角夹取到「短边一半 - 2」：半径等于半边的圆角路径描边会退化（无极值直线段时描边
/// 塌成发丝线，圆形指示器 / 圆形滑块尤其明显），故圆形控件用近似圆的圆角矩形画环。
inline void paint_focus_ring(const RenderContext& context, raster::Surface& canvas, math::Rect rect,
                             float radius) {
  const Metrics& metrics = context.theme.metrics();
  const float width = std::max(metrics.focus_width, metrics.border_width);
  if (width <= 0.0f || rect.is_empty()) return;
  // 环中心线相对控件外沿的偏移：`width/2`（环自己撑到外沿）- `border_width`（内移以盖住描边）。
  const float offset = width * 0.5f - metrics.border_width;
  const math::Rect outer = rect.inflate(offset);
  if (outer.width <= 0.0f || outer.height <= 0.0f) return;
  const float limit = std::max(std::min(outer.width, outer.height) * 0.5f - 2.0f, 0.0f);
  math::Path ring;
  ring.add_rounded_rect(outer, std::min(std::max(radius + offset, 0.0f), limit));
  canvas.stroke_path(ring, raster::Paint::solid(context.theme.colors().focus_ring), width);
}

/// 圆角描边：线宽完全落在矩形内侧。
///
/// **走环形填充，不走描边**：描边（`stroke_path`）把路径每一段扩展成独立四边形 +
/// 顶点补圆，一个圆角矩形要 ~152 条边；环形只有 2 条子路径、76 条边。
/// 实测 0.051 ms → 0.018 ms（2.8×，见 `make_rounded_border_ring` 与
/// `docs/PAINT_DIAGNOSIS.md` §2.2）。视觉上是同一条边框的两种抗锯齿逼近
/// （像素容差对照见 `tools/paint_equiv_probe.cpp`）。
inline void paint_outline(raster::Surface& canvas, math::Rect rect, float radius, math::Color color,
                          float width) {
  if (width <= 0.0f || color.a == 0U || rect.is_empty()) return;
  canvas.fill_path(math::make_rounded_border_ring(rect, radius, width),
                   raster::Paint::solid(color));
}

// —————————————————————— 文本选择（2026-10-14）——————————————————————
//
// 多个文本组件（MarkdownView / Terminal / Input / TextArea / Text）共用同一套
// 「选择」语义。收敛在这里而不是各写一份：三处独立实现必然漂移，而「选了 A 拷出 B」
// 这类缺陷极难定位。约定见 `DESIGN.md` §4.2.17g。

/// 车符位 / 选择区间 / 词界 / 最近码点 / 切片——**已提升到公共头**
/// `st/ui/text_selection.hpp`（应用与子 Agent 也需要它们；此前只在组件私有头里，
/// 于是每个新组件各抄一份）。本头保留同名别名，既有组件无需改名。
using st::ui::Caret;
using st::ui::compare_caret;
using st::ui::kLineEndSentinel;
using st::ui::nearest_codepoint;
using st::ui::normalize_selection;
using st::ui::SelectionBand;
using st::ui::SelectionRange;
using st::ui::slice_line;
using st::ui::TextSelectionHost;
using st::ui::TextSelectionModel;
using st::ui::word_range_at;

}  // namespace st::ui::components_internal
