/// 背景“浅/深”判据：回归测试。
///
/// ## 为什么这条判据值得单测
///
/// 它决定铺光与暗角要不要画。判错的两种后果都**看不出来是哪一层画坏的**：
///
/// · 浅底误判为深 → 米白画面上刷出四个大黑角 + 中间一片脏灰
///   （实测：整条日历片的“脏”就是这个，而所有指标都毫无异常）；
/// · 深底误判为浅 → 深色片默默失去体积感，只是“看起来比较平”。
///
/// 所以这里钉的是**判据在真实配色上的结论**，不是某个阈值数字。

#include "examples/caiyun/fx.hpp"

#include "st/test/test.hpp"

namespace {

using st::math::Color;

}  // namespace

ST_TEST(backdrop_light_detects_cream_and_white) {
  // 实测用过的米白配色（对标片的底色）——必须判成浅色，否则暗角毁画面。
  ST_CHECK(caiyun::backdrop_is_light(Color{0xF0, 0xEA, 0xDD, 255}, Color{0xE8, 0xE0, 0xCE, 255}));
  ST_CHECK(caiyun::backdrop_is_light(Color{255, 255, 255, 255}, Color{255, 255, 255, 255}));
  ST_CHECK(caiyun::backdrop_is_light(Color{0xFA, 0xF7, 0xEF, 255}, Color{0xF2, 0xEC, 0xE0, 255}));
}

ST_TEST(backdrop_light_preserves_dark_themes) {
  // 深色主题（本项目主力配色）必须判成深色：铺光/暗角是它们要的。
  ST_CHECK(!caiyun::backdrop_is_light(Color{0x05, 0x07, 0x0F, 255}, Color{0x0B, 0x12, 0x20, 255}));
  ST_CHECK(!caiyun::backdrop_is_light(Color{0x0A, 0x0F, 0x1A, 255}, Color{0x11, 0x18, 0x2A, 255}));
  // 深底带一点亮色（常见：底部微亮）**不应**被误判成浅色——
  // 阈值取 0.5 就会误判，而那会让这类片默默丢掉体积感。
  ST_CHECK(!caiyun::backdrop_is_light(Color{0x0B, 0x12, 0x20, 255}, Color{0x2A, 0x35, 0x50, 255}));
}

ST_TEST(backdrop_light_uses_the_brighter_side) {
  // 渐变可能一亮一暗：**任一为浅色就会露馅**（暗角在亮的那一端最刺眼），
  // 所以判据必须取两色中较亮者，而不是只看 bg_top。
  const Color light_mid{0xC8, 0xC0, 0xB0, 255};   // 中浅
  const Color dark{0x10, 0x14, 0x1C, 255};
  ST_CHECK(caiyun::backdrop_is_light(dark, light_mid));
  ST_CHECK(caiyun::backdrop_is_light(light_mid, dark));
}

ST_TEST(backdrop_light_is_monotone_in_brightness) {
  // 亮度越高越可能是浅色——灰阶扫描不应出现“中间是浅、更亮反而深”这种翻转。
  // 出现翻转就意味着判据依赖了某个奇怪的分支（如只看某一通道）。
  bool previous = false;
  for (int level = 0; level <= 255; level += 5) {
    const Color shade{static_cast<std::uint8_t>(level), static_cast<std::uint8_t>(level),
                      static_cast<std::uint8_t>(level), 255};
    const bool light = caiyun::backdrop_is_light(shade, shade);
    if (previous) ST_CHECK(light);   // 一旦变浅，更亮的不许变回深
    previous = light;
  }
  ST_CHECK(previous);   // 纯白必须是浅色
}
