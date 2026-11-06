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
                       std::uint32_t shadow_rgb, std::span<std::uint32_t> out,
                       std::uint32_t edge_rgb, bool antialias_corner) -> bool {
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
  // 圆角抗锯齿用的窗口底色（与上面的阴影色分开：一个是"光被遮挡"，一个是"窗口本体"）
  const auto edge_r = static_cast<std::uint32_t>((edge_rgb >> 16) & 0xFFU);
  const auto edge_g = static_cast<std::uint32_t>((edge_rgb >> 8) & 0xFFU);
  const auto edge_bb = static_cast<std::uint32_t>(edge_rgb & 0xFFU);

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

  // 环位图同时承担**两件事**：窗外的阴影 + 圆角边缘的**抗锯齿**。
  //
  // ## 为何圆角要靠位图而不是 `SetWindowRgn`
  //
  // `SetWindowRgn` 接受的是**一位区域**（在区内/在区外），系统无法用它做部分覆盖。
  // 实测证据（窗口放干净桌面上取像素，`tools/window_chrome_quality_probe.py`）：
  //   沿 45° 屏过圆角边缘，亮度 `189 → 247` **一步跳变**，中间过渡像素数 **0**。
  // 那就是用户说的"不光滑"（锯齿台阶）。
  //
  // 修法：区域仍用 `SetWindowRgn`（它负责**裁剪掉**窗口外面的像素），但把
  // **圆角内那一圈边缘像素**交给影子层去画——影子层是 per-pixel alpha 的分层窗口，
  // 可以表达"这个像素 60% 是窗口色"。于是窗口本体被裁成直角，而圆角处的
  // 平滑边由影子层补上，两者叠起来就是抗锯齿的圆角。
  const auto coverage_at = [&](float px, float point_y, float ay) -> float {
    const float ax = std::max(px, 0.0f);
    return std::sqrt(ax * ax + ay * ay) + std::min(std::max(px, point_y), 0.0f) - safe_radius;
  };
  const auto write_pixel = [&](std::uint32_t& cell, float px, float point_y, float ay) {
    const float d = coverage_at(px, point_y, ay);
    // ## 圆角边缘：**标准 1 像素抗锯齿**（与 DWM 同款口径）
    //
    // 覆盖率 = `clamp(0.5 - d, 0, 1)`：
    //   · `d = -0.5`（像素中心在弧内半像素）→ 覆盖 1.0
    //   · `d =  0`  （中心恰在弧上）→ 覆盖 **0.5**（半透，这才是真正的过渡）
    //   · `d = +0.5`（中心在弧外半像素）→ 覆盖 0
    //
    // ⚠ 之前写的是 `clamp(1.5 - d, 0, 1)`，那是错的：它在 `d=0` 时给覆盖 1.0，
    // 于是弧上**只有一档覆盖**（要么满要么零），弧变成硬边——实测正是用户报的台阶。
    //
    // 对照 DWM（`tools/window_corner_visual_diff.py` 量的真实像素，背景 11/窗口 247）：
    //   过渡值 34 / 80 / 127 / 174 / 221 —— 等差 ≈ 47，即**均匀的 1 像素抗锯齿**。
    // 本公式与它同口径。
    //
    // 为何只画外侧（`d ≥ 0`）：窗口本体用 `CreateRoundRectRgn` 裁成圆角，
    // **内侧**（d<0）属于窗口自己的像素区，影子层画什么都会被窗口盖住
    // （实测：那样做窗口左上角变纯直角）。外侧正是被裁掉的位置，能透出来。
    if (antialias_corner && d > -0.5f && d < 0.5f) {
      const float coverage = std::clamp(0.5f - d, 0.0f, 1.0f);
      const auto cover = static_cast<std::uint32_t>(coverage * 255.0f);
      if (cover > 0U) {
        cell = premultiply(edge_r, edge_g, edge_bb, cover);
        return;
      }
    }
    if (d > 0.0f) {
      // **窗外**：阴影。`(1-t)²` 衰减（平方而非线性：柔和阴影的强度随距离加速下降）。
      const float t = std::clamp((d - 1.0f) / fade, 0.0f, 1.0f);
      const float falloff = (1.0f - t) * (1.0f - t);
      const auto alpha =
          static_cast<std::uint32_t>(std::clamp(alpha_scale * falloff, 0.0f, 255.0f));
      if (alpha == 0U) return;
      cell = premultiply(base_r, base_g, base_b, alpha);
      return;
    }
    // 弧内侧（d ≤ 0）：窗口自己的地盘，影子层必须保持透明。
  };
  // 快速路径的横向扫描宽度：要同时覆盖"窗外阴影"与"圆角 AA 带"，
  // 所以比 margin 多出 `radius + 3`（圆角弧跨过 radius，AA 再向外延伸 1.5）。
  const int side_span = layout.margin + layout.radius + 3;
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
