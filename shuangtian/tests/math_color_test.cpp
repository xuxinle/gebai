/// `math::Color` 的**字面量解析契约**测试。
///
/// 这里的每一条都对应一个"静默误读"的坑：颜色解析错了不会崩、不会报错，
/// 只会让某个界面元素显示成另一种颜色，而人眼很难反推是哪一行写错了。
/// 实测踩过：想把深色主题的阴影写成"黑色 55%"（`0x0000008C`），
/// 结果被 `from_hex` 按 6 位形式解析成 `0x00008C`——**纯蓝、完全不透明**，
/// 深色主题的卡片于是一圈蓝光。

#include "st/test/test.hpp"

#include "st/math/color.hpp"

namespace {

using st::math::Color;

/// 逐字段比较（`Color` 是聚合体，没有相等运算符时的兜底断言方式）。
[[nodiscard]] auto same(const Color& left, const Color& right) -> bool {
  return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
}

}  // namespace

ST_TEST(color_from_rgba_hex_always_reads_eight_digits) {
  // 显式 8 位：高位字节为 0 也必须正确解析出 alpha
  const Color near_black = Color::from_rgba_hex(0x0000008CU);
  ST_CHECK(same(near_black, Color{0, 0, 0, 0x8C}));

  const Color opaque_dark = Color::from_rgba_hex(0x0F172AFFU);
  ST_CHECK(same(opaque_dark, Color{0x0F, 0x17, 0x2A, 0xFF}));

  const Color translucent_navy = Color::from_rgba_hex(0x0F172A26U);
  ST_CHECK(same(translucent_navy, Color{0x0F, 0x17, 0x2A, 0x26}));
}

ST_TEST(color_from_hex_ambiguity_is_documented_behavior) {
  // `from_hex` 按**数值大小**区分 `0xRRGGBB` 与 `0xRRGGBBAA`：
  // `0x0000008C`（140）落在 6 位区间 → 被读成蓝色 `0x00008C` + 不透明。
  // 断言这条行为，是为了让"以后有人想改成按位数判断"时会看到这里的来龙去脉：
  // 设计令牌请一律用 `from_rgba_hex`（见 `src/ui/theme.cpp` 的 `hex()`）。
  const Color trap = Color::from_hex(0x0000008CU);
  ST_CHECK(same(trap, Color{0x00, 0x00, 0x8C, 0xFF}));
  ST_CHECK(!same(trap, Color{0, 0, 0, 0x8C}));

  // 高位字节非 0 时两种函数一致（此时数值自然 > 0xFFFFFF）
  ST_CHECK(same(Color::from_hex(0x0F172A26U), Color::from_rgba_hex(0x0F172A26U)));
}

ST_TEST(color_rgb_and_rgba_helpers_are_consistent) {
  ST_CHECK(same(Color::rgb(0x11, 0x22, 0x33), Color{0x11, 0x22, 0x33, 255}));
  ST_CHECK(same(Color::rgba(0x11, 0x22, 0x33, 0x44), Color{0x11, 0x22, 0x33, 0x44}));
  ST_CHECK(same(Color::rgb(0x11, 0x22, 0x33), Color::from_hex(0x112233U)));
}

ST_TEST(color_contrast_ratio_matches_wcag_reference_points) {
  // WCAG 的参考值：白/黑 = 21:1，同色 = 1:1
  ST_CHECK_NEAR(st::math::contrast_ratio(Color{255, 255, 255, 255}, Color{0, 0, 0, 255}), 21.0f,
                0.1f);
  ST_CHECK_NEAR(st::math::contrast_ratio(Color{255, 255, 255, 255}, Color{255, 255, 255, 255}),
                1.0f, 0.001f);
  // 对称性：交换前景/背景结果不变
  const Color a{0x25, 0x63, 0xEB, 255};
  const Color b{0xFF, 0xFF, 0xFF, 255};
  ST_CHECK_NEAR(st::math::contrast_ratio(a, b), st::math::contrast_ratio(b, a), 0.001f);
}

ST_TEST(color_ensure_contrast_reaches_target) {
  // 浅灰在白色上对比度不足：`ensure_contrast` 应把它压深到目标
  const Color background{255, 255, 255, 255};
  const Color weak{0xCC, 0xCC, 0xCC, 255};
  ST_CHECK(st::math::contrast_ratio(weak, background) < 4.5f);
  const Color fixed = st::math::ensure_contrast(weak, background, 4.5f);
  ST_CHECK(st::math::contrast_ratio(fixed, background) >= 4.5f);
  // 已经达标的颜色不该被改动
  const Color strong{0x0F, 0x17, 0x2A, 255};
  ST_CHECK(same(st::math::ensure_contrast(strong, background, 4.5f), strong));
}

ST_TEST(color_relative_luminance_is_ordered) {
  const float black = st::math::relative_luminance(Color{0, 0, 0, 255});
  const float mid = st::math::relative_luminance(Color{0x80, 0x80, 0x80, 255});
  const float white = st::math::relative_luminance(Color{255, 255, 255, 255});
  ST_CHECK_NEAR(black, 0.0f, 0.0001f);
  ST_CHECK_NEAR(white, 1.0f, 0.0001f);
  ST_CHECK(mid > black);
  ST_CHECK(mid < white);
}
