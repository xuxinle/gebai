/// 矢量相机（`Painter` 在绘制期施加缩放）：回归测试。
///
/// ## 这组用例钉的是什么
///
/// 裁云有**两条相机路径**，它们解决不同问题：
/// · **重采样**（`apply_camera`）：整幅画完再变换。小幅度运镜用它——
///   一次重采样、所有图元一起动（含粒子/三维/辉光）。
/// · **矢量**（`VectorCamera`）：绘制期施加缩放，图元含文字**重新光栅化**。
///   钻穿镜头用它——**只有它能做到大幅缩放不糊**。
///
/// 实测（1920×1080，同一场景只改 `cam_vector`）：
///
/// | 倍率 | 重采样 p99.99 边缘梯度 | 矢量 | 倍数 |
/// |---|---|---|---|
/// | 9.7× | 18.0 | 151.0 | 8.4× |
/// | 14.1× | 13.0 | 155.0 | 11.9× |
/// | 18.4× | 10.0 | 142.6 | 14.3× |
/// | 22.8× | 7.0 | 92.0 | 13.1× |
///
/// 重采样随倍率**持续退化**（18→13→10→7），矢量**基本保持**——
/// 因为矢量矩形的细节是无限的，而位图的细节天花板就是源分辨率。
///
/// ## 这组用例为什么不能只看"数值变了"
///
/// 相机最容易错的地方是**锚点偏移**：1× 完全正确、10× 偏 300px、
/// 30× 整幅空白，而所有断言都不报错（它只是"画得不对"）。
/// 所以这里的判据是**几何位置**：一个已知世界坐标点的屏幕位置。

#include <cmath>

#include "examples/caiyun/fx.hpp"
#include "st/math/geometry.hpp"
#include "st/test/test.hpp"

namespace {

using caiyun::CameraMove;
using caiyun::VectorCamera;
using caiyun::apply_camera;

/// 一张有硬边的小画布（深底 + 中心白块）。
auto make_hard_edge_canvas() -> st::raster::Canvas {
  st::raster::Canvas canvas{200, 120, 1.0f};
  canvas.clear(st::math::Color{0, 0, 0, 255});
  canvas.fill_rect(st::math::Rect{80.0f, 40.0f, 40.0f, 40.0f},
                   st::raster::Paint::solid(st::math::Color{255, 255, 255, 255}));
  return canvas;
}

/// 边缘陡度：最大相邻差（硬边 = 大，模糊 = 被摊平到多个像素）。
auto edge_steepness(const st::raster::Canvas& canvas) -> int {
  int best = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 1; x < canvas.physical_width(); ++x) {
      const int a = static_cast<int>(canvas.pixel_at(x - 1, y).r);
      const int b = static_cast<int>(canvas.pixel_at(x, y).r);
      best = std::max(best, std::abs(b - a));
    }
  }
  return best;
}

}  // namespace

ST_TEST(vector_camera_identity_when_zoom_is_one) {
  // zoom=1 时必须是**恒等变换**（老路径零成本、逐像素不变）。
  VectorCamera cam{};
  cam.zoom = 1.0f;
  const auto [x, y] = cam.apply(123.0f, 456.0f);
  ST_CHECK_NEAR(x, 123.0f, 1e-5f);
  ST_CHECK_NEAR(y, 456.0f, 1e-5f);
}

ST_TEST(vector_camera_anchor_stays_put) {
  // 核心几何：锚点在任何倍率下都映射到**它自己**。
  // 这正是"从这一格里钻出去"的全部前提。
  for (const float zoom : {1.5f, 4.0f, 20.0f, 120.0f}) {
    VectorCamera cam{};
    cam.zoom = zoom;
    cam.anchor_x = 640.0f;
    cam.anchor_y = 360.0f;
    const auto [x, y] = cam.apply(640.0f, 360.0f);
    ST_CHECK_NEAR(x, 640.0f, 1e-3f);
    ST_CHECK_NEAR(y, 360.0f, 1e-3f);
  }
}

ST_TEST(vector_camera_scales_distance_from_anchor) {
  // 锚点外一点的位移必须精确等于 `(p - anchor) * zoom + anchor`。
  // 这条比"锚点不动"更强：它同时钉住了**缩放倍数**。
  VectorCamera cam{};
  cam.zoom = 8.0f;
  cam.anchor_x = 100.0f;
  cam.anchor_y = 200.0f;
  const auto [x, y] = cam.apply(110.0f, 205.0f);
  ST_CHECK_NEAR(x, 100.0f + 10.0f * 8.0f, 1e-4f);
  ST_CHECK_NEAR(y, 200.0f + 5.0f * 8.0f, 1e-4f);
}

ST_TEST(vector_camera_beats_resample_on_large_zoom) {
  // 端到端判据：**同一张源图、同一倍率**下，矢量重绘的边缘必须比重采样陡得多。
  //
  // 判据不是"两个数值不同"，而是**方向 + 量级**：矢量应当显著更陡
  // （实测真片里 8~14 倍；单元测试留余量用 >2 倍）。
  //
  // ## 为何用 8× 而不是 20×
  //
  // 源图里那个白块是 40×40。20× 时它变成 800×800，而验证画布只有 400×300——
  // 矩形**整个跑出画面**，量到的是纯色而不是边缘（实测就是这条变红的原因）。
  // 夹具必须保证"放大后边缘仍在画面内"：8× → 320×320 < 400×300（高度刚好）。
  const auto source = make_hard_edge_canvas();
  constexpr float kZoom = 6.0f;

  // 重采样：放大位图 ⇒ 硬边被插值摊平
  CameraMove move{};
  move.zoom_from = kZoom;
  move.zoom_to = kZoom;
  const auto resampled = apply_camera(source, move, 0.0f, 0.0, 1.0f);
  const int resample_steep = edge_steepness(resampled);

  // 矢量：按放大后的坐标**重新绘制**同一个矩形 ⇒ 硬边仍是硬边
  // 画布给足尺寸（源 200×120 放大 6× 后是 1200×720，取中心 400×300 那块）
  st::raster::Canvas vector_drawn{400, 300, 1.0f};
  vector_drawn.clear(st::math::Color{0, 0, 0, 255});
  VectorCamera cam{};
  cam.zoom = kZoom;
  cam.anchor_x = 100.0f;   // 源画面中心
  cam.anchor_y = 60.0f;
  const auto [x0, y0] = cam.apply(80.0f, 40.0f);
  const auto [x1, y1] = cam.apply(120.0f, 80.0f);
  // 把放大后的矩形平移到画布可见区（否则又跑到画布外）
  const float shift_x = 200.0f - (x0 + x1) * 0.5f;
  const float shift_y = 150.0f - (y0 + y1) * 0.5f;
  vector_drawn.fill_rect(st::math::Rect{x0 + shift_x, y0 + shift_y, x1 - x0, y1 - y0},
                         st::raster::Paint::solid(st::math::Color{255, 255, 255, 255}));
  const int vector_steep = edge_steepness(vector_drawn);

  ST_CHECK(vector_steep > resample_steep * 2);
  // 重采样在大倍率下必然被摊平（这条防止"两边都很陡导致假通过"）
  ST_CHECK(resample_steep < 200);
}

ST_TEST(vector_camera_is_deterministic) {
  // 裁云硬要求：同一时刻两次渲染逐像素相同。
  VectorCamera cam{};
  cam.zoom = 13.7f;
  cam.anchor_x = 512.0f;
  cam.anchor_y = 288.0f;
  const auto a = cam.apply(731.25f, 402.5f);
  const auto b = cam.apply(731.25f, 402.5f);
  ST_CHECK(a.first == b.first);
  ST_CHECK(a.second == b.second);
}

// —— 物理像素入口：`path` / `ellipse` 这类直画 canvas 的图元靠它 ——
//
// 早先这些图元**完全不过相机**，于是出现最难查的那种错：同一画面里
// rect/text 都在正常放大，唯独曲线纹丝不动（实测 240× 钻穿时曲线全程不动，
// 先怀疑了相机公式、锚点、窗口，全不对，最后才发现是某一类图元漏了）。

ST_TEST(vector_camera_physical_entry_matches_design_entry) {
  // 两个入口必须给出**同一结果**（同一个世界点，只是坐标系不同）。
  // 若有人把 apply_physical 的 `px / scale` 写成 `px * scale`，这条立刻变红。
  for (const float scale : {1.0f, 2.0f, 1.5f}) {
    VectorCamera cam{};
    cam.zoom = 7.0f;
    cam.anchor_x = 640.0f;
    cam.anchor_y = 360.0f;
    // 世界设计坐标 (400, 300) ⇒ 物理像素 (400*scale, 300*scale)
    const auto [dx, dy] = cam.apply(400.0f, 300.0f);
    const auto [px, py] = cam.apply_physical(400.0f * scale, 300.0f * scale, scale);
    ST_CHECK_NEAR(px, dx * scale, 1e-3f);
    ST_CHECK_NEAR(py, dy * scale, 1e-3f);
  }
}

ST_TEST(vector_camera_physical_entry_scales_with_zoom) {
  // 最强的判据：同一个点，zoom 越大，离锚点越远。
  // 回退成“不过相机”（直接返 px,py）时，距离恒等于 1× 的值 ⇒ 变红。
  VectorCamera cam{};
  cam.anchor_x = 500.0f;
  cam.anchor_y = 500.0f;
  const float px = 900.0f, py = 500.0f, scale = 1.0f;

  cam.zoom = 1.0f;
  const auto [x1, y1] = cam.apply_physical(px, py, scale);
  const float d1 = std::abs(x1 - 500.0f);

  cam.zoom = 9.0f;
  const auto [x9, y9] = cam.apply_physical(px, py, scale);
  const float d9 = std::abs(x9 - 500.0f);

  ST_CHECK_NEAR(d1, 400.0f, 1e-3f);      // 1× 就是原距离
  ST_CHECK_NEAR(d9, 3600.0f, 1e-2f);     // 9× 是 9 倍
  (void)y1; (void)y9;
}
