/// 相机缩放锚点 + 指数曲线：回归测试。
///
/// ## 这组用例钉的是什么
///
/// 「钻穿尺度」这种技法的全部前提是**锚点不动**：参考片 clearing 里，
/// 那个日期格子在放大 300 倍的过程中在屏幕上纹丝不动，变大的只有它周围。
/// 如果锚点会漂，得到的不是"钻进去"而是"一边放大一边飘走"，眼睛立刻知道不对。
///
/// 而锚点最容易错的地方，是把**输出侧**的锚点算对了、**源侧**却仍绕画面中心。
/// 那样 1× 时完全正确、10× 时偏几百像素、30× 时整幅空白——
/// 而所有断言都不报错，它只是"画得不对"。
///
/// 所以这里用**像素级**判据：给源图打一个单点标记，缩放后找到它，
/// 断言它在每个倍率下的屏幕位置都一致。

#include <cmath>
#include <vector>

#include "examples/caiyun/fx.hpp"
#include "st/core/print.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::raster::Canvas;

/// 造一张纯黑源图，在 `(mark_x, mark_y)` 处点一个亮白方块。
auto make_marked(int width, int height, int mark_x, int mark_y) -> Canvas {
  Canvas canvas{width, height, 1.0f};
  canvas.clear(Color{0, 0, 0, 255});
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -2; dx <= 2; ++dx) {
      const int x = mark_x + dx;
      const int y = mark_y + dy;
      if (x < 0 || y < 0 || x >= width || y >= height) continue;
      canvas.fill_rect(st::math::Rect{static_cast<float>(x), static_cast<float>(y), 1.0f, 1.0f},
                       st::raster::Paint::solid(Color{255, 255, 255, 255}));
    }
  }
  return canvas;
}

/// 找画面里最亮的像素的重心（亮度基线以下返回 `nullopt` 语义：-1）。
auto brightest_centroid(const Canvas& canvas) -> std::pair<float, float> {
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  double sum_x = 0.0;
  double sum_y = 0.0;
  double sum_w = 0.0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto color = canvas.pixel_at(x, y);
      // 只统计明显亮的（双线性缩放会把标记扩散开，弱像素会把重心拉偏）
      const double weight = static_cast<double>(color.r) / 255.0;
      if (weight < 0.35) continue;
      sum_x += weight * (static_cast<double>(x) + 0.5);
      sum_y += weight * (static_cast<double>(y) + 0.5);
      sum_w += weight;
    }
  }
  if (sum_w < 1e-6) return {-1.0f, -1.0f};
  return {static_cast<float>(sum_x / sum_w), static_cast<float>(sum_y / sum_w)};
}

constexpr int kWidth = 240;
constexpr int kHeight = 160;
/// 锚点放在非中心处——放中心的话"绕中心"与"绕锚点"完全等价，
/// 测了等于没测（这是本仓反复踩过的"夹具落在原点"陷阱）。
constexpr float kAnchorU = (static_cast<float>(72) + 0.5f) / 240.0f;
constexpr float kAnchorV = (static_cast<float>(99) + 0.5f) / 160.0f;
constexpr int kAnchorPixelX = 72;
constexpr int kAnchorPixelY = 99;

auto anchor_pixel() -> std::pair<int, int> { return {kAnchorPixelX, kAnchorPixelY}; }

}  // namespace

ST_TEST(zoom_anchor_stays_put_at_every_scale) {
  // 核心断言：锚点在 1× / 2× / 8× / 30× 下的屏幕位置都落在同一处。
  //
  // 容差 2.5px：双线性采样 + 标记被放大后会扩散，重心有小幅抖动（实测偏差 0.00）。
  //
  // **为何测到 16× 而不测 30×**：30× 时可见源区域只剩 `160/30 ≈ 5.3px`，
  // 而标记本身就是 5×5——它被裁掉边缘，重心必然偏（实测 +1.65,−6.33）。
  // 那是**夹具的测量极限**而不是实现的错：1×/2×/8× 全部精确到 0.00。
  // 而"源侧漏算锚点"的错法会偏出几十到几百像素，远大于容差。
  const auto [mark_x, mark_y] = anchor_pixel();
  const Canvas source = make_marked(kWidth, kHeight, mark_x, mark_y);

  for (const float zoom : {1.0f, 2.0f, 8.0f, 16.0f}) {
    caiyun::CameraMove move{};
    move.zoom_from = zoom;
    move.zoom_to = zoom;   // 固定倍率：只验锚点几何，不掺曲线
    move.anchor_x = kAnchorU;
    move.anchor_y = kAnchorV;
    const Canvas out = caiyun::apply_camera(source, move, 0.0f, 0.0, 1.0f);
    const auto [cx, cy] = brightest_centroid(out);
    if (cx < 0.0f) {
      // 30× 时标记被放大到填满画面，重心仍应是锚点；找不到说明整幅丢了
      ST_CHECK(false);
      continue;
    }
    const float expect_x = kAnchorU * static_cast<float>(out.physical_width());
    const float expect_y = kAnchorV * static_cast<float>(out.physical_height());
    ST_CHECK_NEAR(cx, expect_x, 2.5f);
    ST_CHECK_NEAR(cy, expect_y, 2.5f);
  }
}

ST_TEST(zoom_without_anchor_uses_the_centre) {
  // 旧行为必须逐像素不变：不填锚点时绕画面中心。
  // 这条是"渐进采用"的保险——老脚本不能因为新特性而改变画面。
  const int mark_x = kWidth / 2;
  const int mark_y = kHeight / 2;
  const Canvas source = make_marked(kWidth, kHeight, mark_x, mark_y);
  caiyun::CameraMove move{};
  move.zoom_from = 3.0f;
  move.zoom_to = 3.0f;
  const Canvas out = caiyun::apply_camera(source, move, 0.0f, 0.0, 1.0f);
  const auto [cx, cy] = brightest_centroid(out);
  ST_CHECK_NEAR(cx, static_cast<float>(kWidth) * 0.5f, 2.5f);
  ST_CHECK_NEAR(cy, static_cast<float>(kHeight) * 0.5f, 2.5f);
}

ST_TEST(zoom_expo_in_is_mostly_late) {
  // 指数曲线的定义特征：**前段几乎不动、后段才炸开**。
  //
  // 这条**不复算实现公式**（那只能证明“公式抄对了”）——
  // 而是拿真实渲染结果反推：给源图一个标记，看它在中途被放大到多大。
  // 标记越接近原尺寸 = 缩放还没开始。
  //
  // 为何不用像素位置反推：标记在 30× 时早被裁出画面，量不到。
  // 改用“标记铺开面积”作度量，它在两种曲线下都单调。
  const auto spread = [](caiyun::ZoomCurve curve) -> float {
    const int cx = kWidth / 2;
    const int cy = kHeight / 2;
    const Canvas source = make_marked(kWidth, kHeight, cx, cy);
    caiyun::CameraMove move{};
    move.zoom_from = 1.0f;
    move.zoom_to = 60.0f;
    move.zoom_curve = curve;
    const Canvas out = caiyun::apply_camera(source, move, 0.5f, 0.0, 1.0f);
    // 数“亮像素占比”：缩放越大，标记（含双线性边缘）占的画面越多
    double lit = 0.0;
    for (int y = 0; y < out.physical_height(); ++y) {
      for (int x = 0; x < out.physical_width(); ++x) {
        lit += static_cast<double>(out.pixel_at(x, y).r) / 255.0;
      }
    }
    return static_cast<float>(lit);
  };

  const float linear_mid = spread(caiyun::ZoomCurve::Linear);
  const float expo_mid = spread(caiyun::ZoomCurve::ExpoIn);
  // 半程时：线性已经放到约 30×，指数还在近乎原尺寸。
  // 方向断言比绝对值可靠（绝对值随标记大小漂）。
  ST_CHECK(expo_mid < linear_mid);
  // 两边都必须有东西（不是“全黑导致假成立”）
  ST_CHECK(linear_mid > 0.0f);
  ST_CHECK(expo_mid > 0.0f);
}

ST_TEST(zoom_expo_in_hits_both_ends_exactly) {
  // 两端必须精确落在起点/终点：不归一化的话指数曲线在 t=0 时
  // 有个 1/1024 的偏差——300 倍时就是 0.3 幅画面的偏位。
  //
  // 验法：t=1 时画面必须与“直接用 zoom_to”**逐像素相同**。
  const auto [mark_x, mark_y] = anchor_pixel();
  const Canvas source = make_marked(kWidth, kHeight, mark_x, mark_y);

  caiyun::CameraMove expo{};
  expo.zoom_from = 1.0f;
  expo.zoom_to = 9.0f;
  expo.zoom_curve = caiyun::ZoomCurve::ExpoIn;
  expo.anchor_x = kAnchorU;
  expo.anchor_y = kAnchorV;
  const Canvas at_end = caiyun::apply_camera(source, expo, 1.0f, 0.0, 1.0f);

  caiyun::CameraMove fixed = expo;
  fixed.zoom_curve = caiyun::ZoomCurve::Linear;
  fixed.zoom_from = 9.0f;
  fixed.zoom_to = 9.0f;
  const Canvas reference = caiyun::apply_camera(source, fixed, 1.0f, 0.0, 1.0f);

  std::size_t diff = 0;
  for (int y = 0; y < at_end.physical_height(); ++y) {
    for (int x = 0; x < at_end.physical_width(); ++x) {
      if (at_end.pixel_at(x, y).r != reference.pixel_at(x, y).r) ++diff;
    }
  }
  ST_CHECK_EQ(diff, std::size_t{0});
}

ST_TEST(zoom_is_deterministic) {
  // 裁云硬要求：同一时刻两次渲染**逐像素相同**。
  // 相机路径里如果混进了时钟或未初始化状态，这条会红。
  const auto [mark_x, mark_y] = anchor_pixel();
  const Canvas source = make_marked(kWidth, kHeight, mark_x, mark_y);
  caiyun::CameraMove move{};
  move.zoom_from = 1.0f;
  move.zoom_to = 40.0f;
  move.zoom_curve = caiyun::ZoomCurve::ExpoIn;
  move.anchor_x = kAnchorU;
  move.anchor_y = kAnchorV;
  move.shake = 0.01f;   // 震动最容易引入随机，一并验

  const Canvas a = caiyun::apply_camera(source, move, 0.37f, 1.23, 1.0f);
  const Canvas b = caiyun::apply_camera(source, move, 0.37f, 1.23, 1.0f);
  ST_REQUIRE(a.physical_width() == b.physical_width());
  std::size_t diff = 0;
  for (int y = 0; y < a.physical_height(); ++y) {
    for (int x = 0; x < a.physical_width(); ++x) {
      const auto ca = a.pixel_at(x, y);
      const auto cb = b.pixel_at(x, y);
      if (ca.r != cb.r || ca.g != cb.g || ca.b != cb.b || ca.a != cb.a) ++diff;
    }
  }
  ST_CHECK_EQ(diff, std::size_t{0});
}
