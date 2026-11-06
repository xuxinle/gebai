#include "st/shell/window_chrome.hpp"

#include <algorithm>
#include <cmath>

namespace st::shell {
namespace {

[[nodiscard]] auto to_physical(int logical, float scale, int minimum = 1) -> int {
  if (logical <= 0) return 0;
  const float safe = scale > 0.0f ? scale : 1.0f;
  return std::max(minimum, static_cast<int>(std::lround(static_cast<double>(logical) *
                                                        static_cast<double>(safe))));
}

/// 把一个字节量度的 alpha 乘到基色上（预乘）。
[[nodiscard]] auto premultiply(std::uint32_t base_r, std::uint32_t base_g, std::uint32_t base_b,
                               std::uint32_t alpha) noexcept -> std::uint32_t {
  return (alpha << 24) | ((base_r * alpha / 255U) << 16) | ((base_g * alpha / 255U) << 8) |
         (base_b * alpha / 255U);
}

}  // namespace

auto round_rect_distance(float px, float py, float centre_x, float centre_y, float half_width,
                         float half_height, float radius) noexcept -> float {
  // 标准圆角矩形 SDF：先把点折到第一象限，再做"内缩矩形 + 圆"的组合。
  const float safe_radius = std::clamp(radius, 0.0f, std::min(half_width, half_height));
  const float qx = std::abs(px - centre_x) - (half_width - safe_radius);
  const float qy = std::abs(py - centre_y) - (half_height - safe_radius);
  const float ax = std::max(qx, 0.0f);
  const float ay = std::max(qy, 0.0f);
  return std::sqrt(ax * ax + ay * ay) + std::min(std::max(qx, qy), 0.0f) - safe_radius;
}

auto ring_layout(const WindowChromeGeometry& geometry, const WindowChromeStyle& style) noexcept
    -> WindowRingLayout {
  WindowRingLayout layout;
  if (geometry.client_width <= 0 || geometry.client_height <= 0) return layout;
  if (geometry.maximized) return layout;   // 最大化不画（见头文件）
  if (!style.shadow || style.shadow_margin <= 0) return layout;
  layout.scale = geometry.scale > 0.0f ? geometry.scale : 1.0f;
  layout.margin = to_physical(style.shadow_margin, geometry.scale);
  layout.radius = to_physical(style.corner_radius, geometry.scale, /*minimum=*/0);
  // 圆角半径不能超过短边一半：否则圆角矩形退化成胶囊，肉眼上就是"窗口变成药丸"。
  layout.radius =
      std::min(layout.radius, std::min(geometry.client_width, geometry.client_height) / 2);
  layout.client_width = geometry.client_width;
  layout.client_height = geometry.client_height;
  layout.width = geometry.client_width + 2 * layout.margin;
  layout.height = geometry.client_height + 2 * layout.margin;
  layout.visible = true;
  return layout;
}

auto build_ring_pixels(const WindowRingLayout& layout, const WindowChromeStyle& style,
                       std::uint32_t shadow_rgb, std::span<std::uint32_t> out) -> bool {
  if (!layout.visible || layout.width <= 0 || layout.height <= 0 || layout.margin <= 0) return false;
  if (out.size() < static_cast<std::size_t>(layout.width) * static_cast<std::size_t>(layout.height)) {
    return false;
  }

  const float margin = static_cast<float>(layout.margin);
  const float centre_x = static_cast<float>(layout.width) / 2.0f;
  const float centre_y = static_cast<float>(layout.height) / 2.0f;
  // 窗口（客户区）在带坐标系里的半尺寸：整张图去掉两侧带厚就是它。
  const float window_half_w = static_cast<float>(layout.width) / 2.0f - margin;
  const float window_half_h = static_cast<float>(layout.height) / 2.0f - margin;
  const float radius = static_cast<float>(layout.radius);
  const float fade = std::max(1.0f, static_cast<float>(style.shadow_blur) * layout.scale * 1.6f);
  const float alpha_scale = std::clamp(style.shadow_alpha, 0.0f, 1.0f) * 255.0f;
  const auto base_r = static_cast<std::uint32_t>((shadow_rgb >> 16) & 0xFFU);
  const auto base_g = static_cast<std::uint32_t>((shadow_rgb >> 8) & 0xFFU);
  const auto base_b = static_cast<std::uint32_t>(shadow_rgb & 0xFFU);

  // SDF 可分离：`qx` 只依赖 x、`qy` 只依赖 y。先各算一遍一维数组，逐像素只剩 3 次浮点。
  std::vector<float> qx(static_cast<std::size_t>(layout.width));
  std::vector<float> qy(static_cast<std::size_t>(layout.height));
  const float safe_radius = std::clamp(radius, 0.0f, std::min(window_half_w, window_half_h));
  const float inset_x = window_half_w - safe_radius;
  const float inset_y = window_half_h - safe_radius;
  for (int x = 0; x < layout.width; ++x) {
    qx[static_cast<std::size_t>(x)] = std::abs(static_cast<float>(x) + 0.5f - centre_x) - inset_x;
  }
  for (int y = 0; y < layout.height; ++y) {
    qy[static_cast<std::size_t>(y)] = std::abs(static_cast<float>(y) + 0.5f - centre_y) - inset_y;
  }

  // **只写环**：某一行的 `|qy|` 已经进到窗口内部（> safe_radius）时，只有左右两侧
  // （共 `margin + radius + 1` 列）可能是阴影，中间整段跳过。这是 4K 下 20 ms → <1 ms 的关键。
  const auto write_pixel = [&](std::uint32_t& cell, float px, float point_y, float ay) {
    const float ax = std::max(px, 0.0f);
    const float d = std::sqrt(ax * ax + ay * ay) + std::min(std::max(px, point_y), 0.0f) -
                    safe_radius;
    if (d <= 0.0f) return;
    const float t = std::clamp(d / fade, 0.0f, 1.0f);
    const auto alpha =
        static_cast<std::uint32_t>(std::clamp(alpha_scale * (1.0f - t) * (1.0f - t), 0.0f, 255.0f));
    if (alpha == 0U) return;
    cell = premultiply(base_r, base_g, base_b, alpha);
  };
  const int side_span = layout.margin + layout.radius + 1;
  for (int y = 0; y < layout.height; ++y) {
    const float point_y = qy[static_cast<std::size_t>(y)];
    const float ay = std::max(point_y, 0.0f);
    std::uint32_t* row = out.data() + static_cast<std::size_t>(y) * layout.width;
    // 该行**竖直方向在窗口内**（point_y ≤ 0）⇒ 它只可能在左右两侧出阴影，中间整段跳过。
    // 反之（在上带/下带里）需要整行扫，因为那里左右都有阴影，还有圆角。
    const bool row_inside_vertically = point_y <= 0.0f;
    if (row_inside_vertically) {
      for (int x = 0; x < side_span && x < layout.width; ++x) {
        write_pixel(row[x], qx[static_cast<std::size_t>(x)], point_y, ay);
      }
      for (int x = std::max(side_span, layout.width - side_span); x < layout.width; ++x) {
        write_pixel(row[x], qx[static_cast<std::size_t>(x)], point_y, ay);
      }
      continue;
    }
    for (int x = 0; x < layout.width; ++x) {
      write_pixel(row[x], qx[static_cast<std::size_t>(x)], point_y, ay);
    }
  }
  return true;
}

auto corner_radius_physical(const WindowChromeGeometry& geometry,
                            const WindowChromeStyle& style) noexcept -> int {
  if (geometry.client_width <= 0 || geometry.client_height <= 0) return 0;
  if (geometry.maximized) return 0;   // 最大化：直角贴边
  if (style.corner_radius <= 0) return 0;
  const int wanted = to_physical(style.corner_radius, geometry.scale, /*minimum=*/0);
  return std::min(wanted, std::min(geometry.client_width, geometry.client_height) / 2);
}

}  // namespace st::shell
