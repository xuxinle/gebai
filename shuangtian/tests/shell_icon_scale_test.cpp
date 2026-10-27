/// 图标重采样（`st::shell::resample_rgba`）的行为契约。
///
/// ## 为什么这条必须有测试
///
/// 它决定「任务栏/通知区里的图标糊不糊」，而以前的实现是 `make_icon` 里的
/// **最近邻取整**（一行 `source_x = x * width / size`），注释还写着"缩幅很小，
/// 不值得引入重采样"——但应用给的源图常见 32→16、48→16，是 2~3 倍缩幅。
/// 症状是"图标看着糊"，既不崩也不报错，**没有任何自动化手段能发现**。
///
/// 下面每条判据都做过**逆向验证**（把实现退回最近邻，本文件必须变红）。

#include "st/test/test.hpp"

#include <cstdint>
#include <vector>

#include "st/shell/icon_scale.hpp"

namespace {

using st::shell::resample_rgba;

/// 造一张 RGBA8 位图。
/// `pixel(x, y)` 返回该点的 (r, g, b, a)。
template <typename Fn>
auto make_bitmap(int w, int h, Fn pixel) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0U);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const auto [r, g, b, a] = pixel(x, y);
      const std::size_t i =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4U;
      out[i + 0U] = r;
      out[i + 1U] = g;
      out[i + 2U] = b;
      out[i + 3U] = a;
    }
  }
  return out;
}

/// 目标像素的（平均）通道值。
auto at(const std::vector<std::uint8_t>& bmp, int w, int x, int y, int channel) -> int {
  return static_cast<int>(bmp[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                               static_cast<std::size_t>(x)) *
                                  4U +
                              static_cast<std::size_t>(channel)]);
}

/// 「过渡带」像素数（0 < a < 255）：越大 = 边缘越连续；最近邻几乎为 0。
auto transition_count(const std::vector<std::uint8_t>& bmp, int w, int h) -> int {
  int n = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int a = at(bmp, w, x, y, 3);
      if (a > 0 && a < 255) ++n;
    }
  }
  return n;
}

}  // namespace

ST_TEST(icon_resample_identity_when_size_matches) {
  // 源 = 目标时必须**逐字节恒等**：否则「我明明给的是 16px 精绘图」会被再糊一遍，
  // 而这恰恰是应用按尺寸绘制之后最常见的路径（16px 源 → 16px 托盘图标）。
  const auto src = make_bitmap(16, 16, [](int x, int y) {
    return std::tuple{std::uint8_t(x * 16), std::uint8_t(y * 16), std::uint8_t(7), std::uint8_t(255)};
  });
  const auto out = resample_rgba(src, 16, 16, 16, 16);
  ST_CHECK_EQ(out.size(), src.size());
  for (std::size_t i = 0; i < src.size(); ++i) ST_CHECK_EQ(static_cast<int>(out[i]), static_cast<int>(src[i]));
}

ST_TEST(icon_resample_halves_produce_intermediate_alpha) {
  // 2× 缩幅下，一个「左半不透明 / 右半透明」的边界必须产生**中间 alpha**：
  // 这正是最近邻做不到的（它只会二选一取某个源像素，边界永远是非 0 即 255）。
  // 相期图标的圆形边缘同理——最近邻下它是硬台阶，面积平均下才是连续过渡。
  const auto src = make_bitmap(32, 32, [](int x, int) {
    const auto a = x < 17 ? std::uint8_t(255) : std::uint8_t(0);   // 17/32，故意不落在 2 的整数倍
    return std::tuple{std::uint8_t(255), std::uint8_t(255), std::uint8_t(255), a};
  });
  const auto out = resample_rgba(src, 32, 32, 16, 16, 4);
  const int transition = transition_count(out, 16, 16);
  ST_CHECK(transition > 0);   // 面积平均：边界列上出现 0<a<255
  // 边界应落在中间几列（不是贴到最右/最左）
  bool mid = false;
  for (int x = 6; x <= 10; ++x) {
    const int a = at(out, 16, x, 8, 3);
    if (a > 0 && a < 255) mid = true;
  }
  ST_CHECK(mid);
}

ST_TEST(icon_resample_beats_nearest_neighbour_on_smooth_edge) {
  // 与「最近邻」对照：同一张 32px 圆盘缩到 16px，量**过渡带像素数**。
  // 面积平均必须明显更多（边缘连续），这是「图标糊」的可量化判据。
  const auto src = make_bitmap(32, 32, [](int x, int y) {
    const double dx = x - 15.5, dy = y - 15.5;
    const double d = dx * dx + dy * dy;
    const auto a = d <= 13.0 * 13.0 ? std::uint8_t(255) : std::uint8_t(0);
    return std::tuple{std::uint8_t(0xD9), std::uint8_t(0x77), std::uint8_t(0x06), a};
  });
  const auto area = resample_rgba(src, 32, 32, 16, 16, 4);

  // 最近邻（旧实现）：手工复算，作为对照基线
  std::vector<std::uint8_t> nearest(16U * 16U * 4U, 0U);
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      const int sx = (x * 32) / 16;
      const int sy = (y * 32) / 16;
      const std::size_t s = (static_cast<std::size_t>(sy) * 32U + static_cast<std::size_t>(sx)) * 4U;
      const std::size_t t = (static_cast<std::size_t>(y) * 16U + static_cast<std::size_t>(x)) * 4U;
      for (int c = 0; c < 4; ++c) nearest[t + static_cast<std::size_t>(c)] = src[s + static_cast<std::size_t>(c)];
    }
  }

  const int area_transition = transition_count(area, 16, 16);
  const int nearest_transition = transition_count(nearest, 16, 16);
  ST_CHECK(nearest_transition == 0);          // 旧实现：完全没有中间调（这就是「硬、跳、糊」）
  ST_CHECK(area_transition >= 16);            // 新实现：有连续过渡
  ST_CHECK(area_transition > nearest_transition);
}

ST_TEST(icon_resample_keeps_colour_of_fully_opaque_pixels) {
  // 全不透明区域的颜色必须原样保留（不得因预乘/反预乘往返而漂移）。
  const auto src = make_bitmap(8, 8, [](int, int) {
    return std::tuple{std::uint8_t(0xD9), std::uint8_t(0x77), std::uint8_t(0x06), std::uint8_t(255)};
  });
  const auto out = resample_rgba(src, 8, 8, 4, 4);
  ST_CHECK_EQ(at(out, 4, 1, 1, 0), 0xD9);
  ST_CHECK_EQ(at(out, 4, 1, 1, 1), 0x77);
  ST_CHECK_EQ(at(out, 4, 1, 1, 2), 0x06);
  ST_CHECK_EQ(at(out, 4, 1, 1, 3), 255);
}

ST_TEST(icon_resample_rejects_bad_input) {
  // 长度不符 / 尺寸非正：返回空（调用方如实报错），不猜、越界读。
  std::vector<std::uint8_t> too_short(3, 0U);
  ST_CHECK(resample_rgba(too_short, 4, 4, 2, 2).empty());
  const auto ok = make_bitmap(4, 4, [](int, int) {
    return std::tuple{std::uint8_t(1), std::uint8_t(2), std::uint8_t(3), std::uint8_t(4)};
  });
  ST_CHECK(resample_rgba(ok, 0, 4, 2, 2).empty());
  ST_CHECK(resample_rgba(ok, 4, 4, 0, 2).empty());
  ST_CHECK(!resample_rgba(ok, 4, 4, 2, 2).empty());
}
