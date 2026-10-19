#pragma once

/// 画笔：纯色 / 线性 / 径向 / 扫掠渐变 + 混合模式 + 绘制选项。
/// 渐变内部维护 256 级 LUT（构造时预计算），逐像素采样为常数时间。

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"

namespace st::raster {

enum class BlendMode : std::uint8_t {
  SrcOver,  ///< 常规叠加（默认）
  Src,      ///< 覆盖（含 alpha 替换）
  DstOver,
  Multiply,
  Screen,
  Overlay,
  Darken,
  Lighten,
  Add,      ///< 加性（光晕/高亮）
};

[[nodiscard]] constexpr auto to_string(BlendMode mode) noexcept -> std::string_view {
  switch (mode) {
    case BlendMode::SrcOver: return "src_over";
    case BlendMode::Src: return "src";
    case BlendMode::DstOver: return "dst_over";
    case BlendMode::Multiply: return "multiply";
    case BlendMode::Screen: return "screen";
    case BlendMode::Overlay: return "overlay";
    case BlendMode::Darken: return "darken";
    case BlendMode::Lighten: return "lighten";
    case BlendMode::Add: return "add";
  }
  return "src_over";
}

struct GradientStop {
  float offset{0.0f};  ///< [0,1]
  math::Color color{};
};

class Gradient {
 public:
  enum class Kind : std::uint8_t { Linear, Radial, Sweep };

  [[nodiscard]] static auto linear(math::Point from, math::Point to,
                                   std::vector<GradientStop> stops) -> Gradient;
  [[nodiscard]] static auto radial(math::Point center, float radius,
                                   std::vector<GradientStop> stops) -> Gradient;
  [[nodiscard]] static auto sweep(math::Point center, std::vector<GradientStop> stops) -> Gradient;

  [[nodiscard]] auto kind() const noexcept -> Kind { return kind_; }
  [[nodiscard]] auto stops() const noexcept -> std::span<const GradientStop> { return stops_; }
  /// 几何：线性 = 起点/终点；径向与扫掠 = 圆心。
  ///
  /// 为什么需要暴露它：GPU 光栅器要**在着色器里**按像素算 `t`
  /// （线性投影 / 径向距离 / 扫掠角度）。没有几何就只能回到 CPU 逐像素采样，
  /// 而那正是 GPU 路径要摆脱的开销。
  [[nodiscard]] auto start() const noexcept -> math::Point { return start_; }
  [[nodiscard]] auto end() const noexcept -> math::Point { return end_; }
  [[nodiscard]] auto radius() const noexcept -> float { return radius_; }

  /// 采样（`point` 为画布坐标）。
  [[nodiscard]] auto sample(math::Point point) const noexcept -> math::Color;

  /// 按**已算好的位置参数**采样（0..1，内部 clamp 后查 LUT）。
  ///
  /// 为什么单独暴露：批量光栅化里位置可沿扫描线**递推**——线性渐变的 t 沿 x
  /// 每次递增一个常量（dx/len²），比逐像素算点积便宜得多；竖直渐变则整行同 t。
  /// 径向/扫掠的几何是非线性的，仍逐像素算位置后调它。
  [[nodiscard]] auto sample_position(float position) const noexcept -> math::Color;

  /// 返回按 LUT 采样的颜色（索引 0..255，供批量光栅化复用）。
  [[nodiscard]] auto lut() const noexcept -> std::span<const math::Color> { return lut_; }

  void rebuild();

 private:
  Kind kind_{Kind::Linear};
  math::Point start_{};
  math::Point end_{};
  float radius_{0.0f};
  std::vector<GradientStop> stops_{};
  std::array<math::Color, 256> lut_{};
};

/// 画笔：纯色或渐变。`sample()` 为逐像素求值入口。
class Paint {
 public:
  [[nodiscard]] static auto solid(math::Color color) -> Paint;
  [[nodiscard]] static auto with_gradient(Gradient gradient) -> Paint;

  [[nodiscard]] auto is_solid() const noexcept -> bool { return gradient_ == nullptr; }
  [[nodiscard]] auto color() const noexcept -> math::Color { return color_; }
  [[nodiscard]] auto gradient() const noexcept -> const Gradient* { return gradient_.get(); }

  [[nodiscard]] auto sample(math::Point point) const noexcept -> math::Color;

  /// 整笔不透明且为纯色时的快速路径（可直接按行填充）。
  [[nodiscard]] auto is_opaque_solid() const noexcept -> bool {
    return is_solid() && color_.a == 255U;
  }

 private:
  math::Color color_{0, 0, 0, 255};
  std::shared_ptr<const Gradient> gradient_{};
};

struct DrawOptions {
  BlendMode blend{BlendMode::SrcOver};
  float opacity{1.0f};
  bool antialias{true};
  /// **随后会被不透明底完全盖住的区域**（**逻辑坐标**，与 `draw_shadow_layered` 的
  /// `rect` 同一坐标系；空 = 无提示）。
  ///
  /// 用途：投影这类“先画、随即被自己的不透明底色盖掉”的原语，落在该区域内的像素
  /// 合成了也看不见。后端可以据此跳过那段合成（实测贴图的非零区是卡片自身面积的
  /// 2.3 倍，其中一半以上落在卡片底下）。
  ///
  /// ## 调用方契约（不满足就不能传）
  ///
  /// 1. 该区域在本原语之后**真的**会被一个 `alpha == 255` 且 `opacity == 1` 的
  ///    `SrcOver/Src` 填充完全覆盖；
  /// 2. 这中间不得有任何依赖“阴影已合成”的绘制（如对底色做 `Multiply`）。
  ///
  /// 满足这两条时“跳过”与“先画再盖”**逐像素相同**（`src-over` 结合律 +
  /// 不透明源即完全替换）；不满足时跳过会露底——因此后端只在
  /// `blend == SrcOver && opacity >= 0.999` 时才真的跳过（其余情形当作无提示）。
  ///
  /// `covered_radius` 是该区域的圆角半径：**圆角外的四个小缺口不在覆盖区内**，
  /// 后端按行解析算出精确行内跨度，而不是把整块矩形当覆盖——否则卡片四角会丢阴影。
  math::Rect covered_region{};
  float covered_radius{0.0f};
};

/// 线帽（线段端点形态）。缺省 `Butt`——与 SVG/CSS 的默认一致
/// （`stroke-linecap` 缺省 butt；圆头必须显式声明）。
enum class LineCap { Butt, Round, Square };

/// 线段连接（折线转角形态）。缺省 `Miter`——与 SVG/CSS 的默认一致。
///
/// ⚠ 与"圆头补角"的区别：补角是**几何必需**（否则转角外侧留缺口），任何连接形态都要补；
/// `Round` 只是把补的形状定为圆弧。旧实现把它当成"要不要补"，于是无法表达 butt/miter。
enum class LineJoin { Miter, Round, Bevel };

/// 描边样式（`Canvas::stroke_path` 的可选参数）。
struct StrokeStyle {
  LineCap cap{LineCap::Butt};
  LineJoin join{LineJoin::Miter};
};

}  // namespace st::raster
