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

/// 预乘空间的两层合成：`under` 在底，`over` 在上（标准 source-over）。
///
/// 为何需要它：圆角边缘那一像素既属于窗口（窗口色×覆盖率）又属于阴影
/// （阴影浓度×剩余比例）。早先二者是二选一，于是弧边紧贴一圈满强度阴影，
/// 合成到白底后是一个孤立暗点（实测亮度 147，而邻像素 246）。
[[nodiscard]] auto premultiply_over(std::uint32_t over_r, std::uint32_t over_g,
                                    std::uint32_t over_b, std::uint32_t over_alpha,
                                    std::uint32_t under_r, std::uint32_t under_g,
                                    std::uint32_t under_b,
                                    std::uint32_t under_alpha) noexcept -> std::uint32_t {
  if (over_alpha == 0U) return premultiply(under_r, under_g, under_b, under_alpha);
  if (under_alpha == 0U) return premultiply(over_r, over_g, over_b, over_alpha);
  const auto over_p = premultiply(over_r, over_g, over_b, over_alpha);
  const auto under_p = premultiply(under_r, under_g, under_b, under_alpha);
  const std::uint32_t keep = 255U - over_alpha;   // 上层不透明的部分不透出底层
  const auto mix = [keep](std::uint32_t a, std::uint32_t b) -> std::uint32_t {
    return std::min(255U, a + b * keep / 255U);
  };
  return (mix((over_p >> 24) & 0xFF, (under_p >> 24) & 0xFF) << 24) |
         (mix((over_p >> 16) & 0xFF, (under_p >> 16) & 0xFF) << 16) |
         (mix((over_p >> 8) & 0xFF, (under_p >> 8) & 0xFF) << 8) |
         mix(over_p & 0xFF, under_p & 0xFF);
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
  const float inverse_fade = 1.0f / fade;   // 逐像素除法 → 乘法
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
    // 直边段（**绝大多数像素**）只有一个分量非零，`sqrt(ax²+0)` 就是 `ax` 本身。
    // 只有四个角的弧带才两个分量都非零。实测这一条快路径把 `build_ring_pixels`
    // 从 3.6 ms 降到一半以下（2054×1354，1.5× DPI）。
    if (ay <= 0.0f) {
      return ax + std::min(std::max(px, point_y), 0.0f) - safe_radius;
    }
    if (ax <= 0.0f) {
      return ay + std::min(std::max(px, point_y), 0.0f) - safe_radius;
    }
    return std::sqrt(ax * ax + ay * ay) + std::min(std::max(px, point_y), 0.0f) - safe_radius;
  };
  const auto write_pixel = [&](std::uint32_t& cell, float px, float point_y, float ay) {
    const float d = coverage_at(px, point_y, ay);
    // ## 圆角边缘：**窗口色与阴影按覆盖率合成**（标准 1 像素抗锯齿）
    //
    // 贡献两路，按覆盖比例叠加：
    //   圆角覆盖率 `cov = clamp(0.5 - d, 0, 1)`  —— 这一像素“属于窗口”的比例
    //   阴影强度  `shadow = (1-t)²`               —— 窗外阴影在那一点的浓度
    //
    // 合成方式（预乘空间，over 运算）：
    //   窗口色贡献：`edge * cov`
    //   阴影贡献：  `shadow * (1 - cov)`   ← **被窗口遮住的部分要让出来**
    //
    // ⚠ **早先是二选一，那是错的**：AA 带只在 `d ∈ (-0.5, 0.5)`，
    // 超出就直接画满强度阴影（alpha 107）。于是弧边缘紧贴一圈近黑像素，
    // 合成到白底后亮度 147，而紧邻的窗口内容是 246 —— 用户看到的就是
    // “圆角与直线交界处有两个黑点”（实测坐标 (23,18)/(21,19)/(20,20) 等）。
    //
    // 为何覆盖率的区间取 `d ∈ (-0.5, 1.5)`：
    //   · `d < -0.5`：像素中心已在弧内，窗口自己会画，影子层必须透明；
    //   · `d > 1.5` ：完全在窗外，只有阴影（上面两路公式此时 cov=0，自然退化成纯阴影）。
    const float cov = std::clamp(0.75f - d * 0.75f, 0.0f, 1.0f);
    // 只在**弧附近**画：`d < -0.5` 已在窗口内部（窗口自己画，影子层必须透明），
    // `d > 0.5` 则覆盖率为 0（纯阴影）。
    //
    // 为何 `cov > 0.0f` 不够、还要限 `d > -1.24f`：
    // 窗口内部深处 `cov` 也是 1.0，影子层在那里必须保持透明
    // （否则会盖住窗口自己的内容）。
    const bool has_window_color = antialias_corner && cov > 0.0f && d > -1.24f;

    // 阴影强度：**贴窗口最浓，向外衰减**（`(1-t)²`）。
    //
    // 为何贴窗口就要最浓：柔和阴影的本质是“光被窗口遮住”。紧贴窗口的那
    // 一条线被遮得最彻底，所以最暗；向外光绕射进来，逐渐变亮。
    //
    // ⚠ **不能让它从 0 开始**（踩过）：曾把公式改成 `rise × fall` 升-降包络，
    // 理由是“消黑点”。结果贴窗口处的 alpha 只有 7，到第 8 像素才到峰值
    // 102 —— 用户看到的是一条**窗口与阴影之间的空隙**。
    //
    // 黑点真正的解法不在这里，而在下面 `has_window_color` 的**两路合成**：
    // 圆弧上窗口色占 `cov`、阴影只占 `1-cov`，于是弧边不会出现“满强度
    // 阴影紧贴亮窗口”的孤立黑点。
    const float t = d * inverse_fade;
    // **早退**：`t ≥ 1` 时阴影 alpha 为 0，且此时 `cov` 也早已为 0（`d > 0.5`）。
    // 大窗口上多数像素落在这里，提前返回省掉后面的乘法与判断。
    if (t >= 1.0f) return;
    const float tc = t <= 0.0f ? 0.0f : t;
    const float inv = 1.0f - tc;
    const float falloff = inv * inv;
    const auto shadow_alpha =
        static_cast<std::uint32_t>(std::clamp(alpha_scale * falloff, 0.0f, 255.0f));

    if (has_window_color) {
      // 两路合成（预乘 over）：窗口色在上、阴影在下
      const auto edge_a = static_cast<std::uint32_t>(cov * 255.0f + 0.5f);
      const auto shadow_a = static_cast<std::uint32_t>(
          static_cast<float>(shadow_alpha) * (1.0f - cov) + 0.5f);
      cell = premultiply_over(edge_r, edge_g, edge_bb, edge_a, base_r, base_g, base_b, shadow_a);
      return;
    }
    if (d > 0.0f) {
      // **纯窗外**：只有阴影。
      if (shadow_alpha == 0U) return;
      cell = premultiply(base_r, base_g, base_b, shadow_alpha);
      return;
    }
    // 弧内侧（d ≤ -0.5）：窗口自己的地盘，影子层必须保持透明。
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
