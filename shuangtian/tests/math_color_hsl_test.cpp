/// `Color::to_hsl()`：sRGB8 → HSL 的反向转换。
///
/// ## 为什么需要这条
///
/// `to_hsl` 是给“按主题基色换色相、保留明暗语气”的场合用的（泳道自动取色）。
/// 它是 `from_hsl` 的**逆**，所以判据天然是**往返一致**：
/// `from_hsl(*to_hsl(c))` 必须回到 `c`（允许 1 个量化误差）。
///
/// 光有往返还不够——往返在**错误的实现**下也可能“自洽”（两个函数一起错）。
/// 所以另加三条**绝对锚点**：纯红/纯绿/纯蓝/灰阶的 HSL 必须是教科书的那些值。
/// 锚点错了，往返测试会跟着一起错，而锚点不会。
///
/// ## 实测代价（为什么灰阶要单列）
///
/// `to_hsl` 里 `delta <= 0` 那一支（灰阶）极易被写成“也走公式”——
/// 那样会得到 `saturation = 0/0 = NaN`。NaN 参与 `from_hsl` 后颜色变成黑色，
/// 而**画面上“某个颜色画不出来”不会报错**，只表现为“这条线不见了”。

#include "st/test/test.hpp"

#include <cmath>
#include <cstdint>

#include "st/core/log.hpp"
#include "st/math/color.hpp"

namespace {

using st::math::Color;

[[nodiscard]] auto hue_of(Color color) -> float { return std::get<0>(color.to_hsl()); }
[[nodiscard]] auto sat_of(Color color) -> float { return std::get<1>(color.to_hsl()); }
[[nodiscard]] auto light_of(Color color) -> float { return std::get<2>(color.to_hsl()); }

/// 两个色在 8bit 三通道上的最大差异（往返判据用它，而不是逐位相等）。
[[nodiscard]] auto max_channel_delta(Color a, Color b) -> int {
  const int dr = std::abs(static_cast<int>(a.r) - static_cast<int>(b.r));
  const int dg = std::abs(static_cast<int>(a.g) - static_cast<int>(b.g));
  const int db = std::abs(static_cast<int>(a.b) - static_cast<int>(b.b));
  return std::max({dr, dg, db});
}

}  // namespace

/// 四个**绝对锚点**：原色与灰阶的 HSL 必须是教科书值。
ST_TEST(color_to_hsl_matches_textbook_anchors) {
  // 纯红：h=0, s=1, l=0.5
  ST_CHECK_NEAR(hue_of(Color::rgb(255, 0, 0)), 0.0f, 0.5f);
  ST_CHECK_NEAR(sat_of(Color::rgb(255, 0, 0)), 1.0f, 0.01f);
  ST_CHECK_NEAR(light_of(Color::rgb(255, 0, 0)), 0.5f, 0.01f);

  // 纯绿：h=120
  ST_CHECK_NEAR(hue_of(Color::rgb(0, 255, 0)), 120.0f, 0.5f);
  // 纯蓝：h=240
  ST_CHECK_NEAR(hue_of(Color::rgb(0, 0, 255)), 240.0f, 0.5f);
  // 青：h=180
  ST_CHECK_NEAR(hue_of(Color::rgb(0, 255, 255)), 180.0f, 0.5f);

  st::log::out("[color] red h={} s={} l={}\n", static_cast<double>(hue_of(Color::rgb(255, 0, 0))),
               static_cast<double>(sat_of(Color::rgb(255, 0, 0))),
               static_cast<double>(light_of(Color::rgb(255, 0, 0))));
}

/// **灰阶不能出 NaN**（`delta == 0` 分支）：饱和度为 0、色相无所谓。
ST_TEST(color_to_hsl_gray_has_zero_saturation_and_is_finite) {
  for (const std::uint8_t level : {std::uint8_t{0}, std::uint8_t{64}, std::uint8_t{128},
                                    std::uint8_t{255}}) {
    const Color gray = Color::rgb(level, level, level);
    const float saturation = sat_of(gray);
    ST_CHECK(std::isfinite(saturation));
    ST_CHECK_NEAR(saturation, 0.0f, 0.001f);
    ST_CHECK_NEAR(light_of(gray), static_cast<float>(level) / 255.0f, 0.01f);
    // 往返回灰阶（NaN 会让它变成别的颜色）。
    const auto [h, s, l] = gray.to_hsl();
    const Color back = Color::from_hsl(h, s, l);
    ST_CHECK_EQ(max_channel_delta(gray, back), 0);
  }
  st::log::out("[color] 灰阶四档均无 NaN\n");
}

/// **往返一致**：`from_hsl(to_hsl(c)) ≈ c`（8bit 量化允许 1）。
ST_TEST(color_from_hsl_round_trips_through_to_hsl) {
  // 覆盖各象限 + 低饱和 + 高饱和 + 暗/亮端（色相公式有 6 个分支，要都走到）。
  const Color samples[] = {
      Color::rgb(255, 0, 0),     Color::rgb(0, 255, 0),     Color::rgb(0, 0, 255),
      Color::rgb(255, 255, 0),   Color::rgb(0, 255, 255),   Color::rgb(255, 0, 255),
      Color::rgb(200, 120, 40),  Color::rgb(40, 120, 200),  Color::rgb(120, 200, 40),
      Color::rgb(30, 30, 60),    Color::rgb(220, 220, 250), Color::rgb(128, 128, 130),
      Color::rgb(17, 99, 200),   Color::rgb(250, 240, 235),
  };
  int worst = 0;
  for (const Color sample : samples) {
    const auto [h, s, l] = sample.to_hsl();
    const Color back = Color::from_hsl(h, s, l);
    const int delta = max_channel_delta(sample, back);
    worst = std::max(worst, delta);
    if (delta > 1) {
      st::log::out("[color] 往返偏差 {} at rgb({},{},{}) → h={} s={} l={}\n", delta,
                   static_cast<int>(sample.r), static_cast<int>(sample.g),
                   static_cast<int>(sample.b), static_cast<double>(h),
                   static_cast<double>(s), static_cast<double>(l));
    }
    ST_CHECK(delta <= 1);
  }
  st::log::out("[color] {} 个样本往返最大偏差 = {}\n", std::size(samples), worst);
}

/// 两者**互逆**：`to_hsl(from_hsl(h,s,l))` 回到同一组 HSL 参数。
///
/// 与上一条互补：上一条从颜色出发，这条从 HSL 出发（会覆盖 `from_hsl` 的量化）。
ST_TEST(color_hsl_round_trips_from_hsl_side) {
  for (const float hue : {0.0f, 45.0f, 90.0f, 180.0f, 270.0f, 359.0f}) {
    for (const float saturation : {0.2f, 0.6f, 1.0f}) {
      const float lightness = 0.5f;
      const Color color = Color::from_hsl(hue, saturation, lightness);
      const auto [h, s, l] = color.to_hsl();
      // 色相在 8bit 下损失较大（尤其低饱和），容差放到 3°。
      const float hue_delta = std::abs(h - hue);
      const float hue_error = std::min(hue_delta, 360.0f - hue_delta);   // 环形距离
      if (hue_error > 3.0f || std::abs(s - saturation) > 0.05f || std::abs(l - lightness) > 0.05f) {
        st::log::out("[color] from_hsl({},{},{}) → to_hsl h={} s={} l={}\n",
                     static_cast<double>(hue), static_cast<double>(saturation),
                     static_cast<double>(lightness), static_cast<double>(h),
                     static_cast<double>(s), static_cast<double>(l));
      }
      ST_CHECK(hue_error <= 3.0f);
      ST_CHECK_NEAR(s, saturation, 0.05f);
      ST_CHECK_NEAR(l, lightness, 0.05f);
    }
  }
}
