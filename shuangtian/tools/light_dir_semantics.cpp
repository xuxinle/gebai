// 判定 `Light::direction` 到底是「指向光源」还是「照射方向」。
//
// ## 为什么要单独做一个判定工具
//
// 头文件注释说是「指向光源」，而 `software_scene.cpp` 的注释说是「照射方向」。
// 两处相反，而后果很具体：阴影会投到物体的**反侧**，而画面里"有影子"、
// 只是位置不对——这种错误极难从画面上看出来，只能靠**几何推断**。
//
// ## 判据怎么来的
//
// 放一个球在地面上方，光从**右上前方**照来（指向光源的方向 x>0、y>0、z>0）。
// 那么影子必须落在球的**左后方**（x 更小）。
//
// 这个结论与坐标系手性无关、与相机无关：影子永远在光源的**背侧**。
//
// ## 用法
//
//   light_dir_semantics <direction_x> <direction_y> <direction_z> [backend]
//
// 打印：阴影贴图里"球心正上方那一点"的遮挡情况，以及投影到地面的位置。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

auto script(double dx, double dy, double dz) -> std::string {
  // ## 相机放在**侧面**看球与地面
  //
  // 第一版相机在 (0,10,10) 俯视原点，球刚好挡住它自己的影子，
  // 且地面占比很大——判据不可靠。改成侧面视角（看 -Y 向侧移），
  // 影子会清楚地落在地面上、不被球遮挡。
  char buffer[2048];
  std::snprintf(buffer, sizeof(buffer), R"J({
  "objects": [
    {"name":"floor","shape":"plane","size":30,"translate":[0,0,0],
     "color":[210,210,210],"roughness":0.9},
    {"name":"ball","shape":"sphere","size":2.0,"translate":[0,2.0,0],
     "color":[210,210,210],"roughness":0.9}
  ],
  "camera": {"eye":[0,9,16],"target":[0,0.5,0],"fov":45},
  "lighting": {"ambient":0.05,"diffuse":1.0,"tone_map":"none",
               "light_from":[%g,%g,%g]},
  "shadow_map": {"strength":1.0,"light_index":-1,"resolution":1024,
                 "extent":12.0,"soft_radius":1.0}
})J", dx, dy, dz);
  return std::string{buffer};
}

}  // namespace

int main(int argc, char** argv) {
  const double dx = argc > 1 ? std::atof(argv[1]) : 0.5;
  const double dy = argc > 2 ? std::atof(argv[2]) : 1.0;
  const double dz = argc > 3 ? std::atof(argv[3]) : 0.5;

  const auto spec = st::raster::parse_scene_spec_text(script(dx, dy, dz));
  if (!spec.has_value()) {
    std::printf("解析失败：%s\n", spec.error().message.c_str());
    return 1;
  }
  constexpr int kSize = 256;
  st::raster::Canvas canvas{kSize, kSize, 1.0f};
  const auto status =
      st::raster::render_scene(*spec, canvas, st::math::Color::rgba(0, 0, 0, 255));
  if (!status.has_value()) {
    std::printf("渲染失败：%s\n", status.error().message.c_str());
    return 1;
  }

  // 找"最暗像素"的重心 —— 那就是影子的位置（地面上最黑的区域）
  //
  // ## 阈值必须**动态**定，不能写死 60
  //
  // 第一版写死 60：`ambient=0` 时地面本身就很暗（实测 89% 像素 < 60），
  // 结果是"影子质心 = 画面中心"——完全无用。
  //
  // 正确做法：先算全图直方图，取 `min + (max-min)*0.25` 作为"明显暗部"阈值。
  const auto pixels = canvas.pixels();
  const auto luma_at = [&pixels](int x, int y) -> double {
    const auto c = st::math::unpremultiply(
        pixels[static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)]);
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
  };
  double min_luma = 255.0;
  double max_luma = 0.0;
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      const double v = luma_at(x, y);
      if (v < min_luma) min_luma = v;
      if (v > max_luma) max_luma = v;
    }
  }
  const double threshold = min_luma + (max_luma - min_luma) * 0.25;

  double sum_x = 0.0;
  double sum_y = 0.0;
  double count = 0.0;
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      if (luma_at(x, y) <= threshold) {
        sum_x += x;
        sum_y += y;
        count += 1.0;
      }
    }
  }
  std::printf("光方向(指向光源) = (%g, %g, %g)\n", dx, dy, dz);
  std::printf("亮度范围 = [%.1f, %.1f]，暗部阈值 = %.1f\n", min_luma, max_luma, threshold);
  if (count < 10.0) {
    std::printf("影子像素太少（%.0f）——判定不可靠\n", count);
    return 2;
  }
  // 屏幕坐标：相机在 (0,10,10) 看原点，所以屏幕 x 与世界 +X 同向
  std::printf("影子质心(屏幕) = (%.1f, %.1f)，像素数 %.0f\n", sum_x / count, sum_y / count,
              count);
  std::printf("期望：影子在光源的**背侧** → 世界 X 应为负 → 屏幕 x < %d\n", kSize / 2);
  std::printf("实测屏幕 x = %.1f → %s\n", sum_x / count,
              sum_x / count < kSize / 2 ? "影子在左侧（正确）" : "影子在右侧（方向反了）");
  return 0;
}
