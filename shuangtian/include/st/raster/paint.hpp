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
};

}  // namespace st::raster
