/// 图标位图重采样（面积平均）。见 `include/st/shell/icon_scale.hpp` 的口径说明与实测数据。

#include "st/shell/icon_scale.hpp"

#include <cmath>
#include <cstddef>

namespace st::shell {

auto resample_rgba(std::span<const std::uint8_t> rgba, int src_width, int src_height, int dst_width,
                   int dst_height, int samples) -> std::vector<std::uint8_t> {
  if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) return {};
  const auto expected = static_cast<std::size_t>(src_width) * static_cast<std::size_t>(src_height) * 4U;
  if (rgba.size() < expected) return {};
  const int kSub = samples < 1 ? 1 : samples;
  std::vector<std::uint8_t> out(static_cast<std::size_t>(dst_width) * static_cast<std::size_t>(dst_height) * 4U, 0U);

  const auto channel_at = [&rgba, src_width, src_height](double sx, double sy, int channel) -> double {
    int ix = static_cast<int>(sx);
    int iy = static_cast<int>(sy);
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    if (ix >= src_width) ix = src_width - 1;
    if (iy >= src_height) iy = src_height - 1;
    const std::size_t idx = (static_cast<std::size_t>(iy) * static_cast<std::size_t>(src_width) +
                             static_cast<std::size_t>(ix)) *
                                4U +
                            static_cast<std::size_t>(channel);
    return static_cast<double>(rgba[idx]);
  };
  const auto to_byte = [](double v) -> std::uint8_t {
    const double c = v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v);
    return static_cast<std::uint8_t>(c + 0.5);
  };

  const double scale_x = static_cast<double>(src_width) / static_cast<double>(dst_width);
  const double scale_y = static_cast<double>(src_height) / static_cast<double>(dst_height);
  const double norm = static_cast<double>(kSub) * static_cast<double>(kSub);

  for (int y = 0; y < dst_height; ++y) {
    for (int x = 0; x < dst_width; ++x) {
      double a_sum = 0.0;
      double r_sum = 0.0;
      double g_sum = 0.0;
      double b_sum = 0.0;
      for (int sy = 0; sy < kSub; ++sy) {
        for (int sx = 0; sx < kSub; ++sx) {
          const double u = (static_cast<double>(x) + (static_cast<double>(sx) + 0.5) / kSub) * scale_x;
          const double v = (static_cast<double>(y) + (static_cast<double>(sy) + 0.5) / kSub) * scale_y;
          const double a = channel_at(u, v, 3) / 255.0;
          a_sum += a;
          // 预乘累加：半透明像素的配色权重应与其覆盖率成正比（否则边缘发灰）。
          r_sum += channel_at(u, v, 0) * a;
          g_sum += channel_at(u, v, 1) * a;
          b_sum += channel_at(u, v, 2) * a;
        }
      }
      const double a_avg = a_sum / norm;
      // 反预乘回直通 alpha。
      const double inv = a_sum > 1e-9 ? 1.0 / a_sum : 0.0;
      const std::size_t idx =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_width) + static_cast<std::size_t>(x)) * 4U;
      out[idx + 0U] = to_byte(r_sum * inv);
      out[idx + 1U] = to_byte(g_sum * inv);
      out[idx + 2U] = to_byte(b_sum * inv);
      out[idx + 3U] = to_byte(a_avg * 255.0);
    }
  }
  return out;
}

}  // namespace st::shell
