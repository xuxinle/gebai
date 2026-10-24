// 背面剔除方向验证：**单独**回答"剔的是背面还是正面"。
//
// ## 为什么必须单独验（不能靠现有测试）
//
// 对**封闭实体**，剔除正面和剔除背面在深度测试下都能得到"看起来正常的实体"：
// 剔背面 → 看到外表面（正确）；剔正面 → 看到内表面（形状相同、光照略异）。
// 所以"立方体能画出来"这类断言完全分辨不出方向对错。
//
// 真正的判据是**平面**（单面物体）：
// - 平面法线朝 +Z，相机在 +Z 看它 → 应可见（它朝向相机）；
// - 相机转到 -Z 看它 → 默认剔背面时应**完全不可见**（面积 0）。
// 方向反了的话这两条会互换。
//
// 用 `Mesh::plane`（法线 +Z）+ 两个相机位置，量可见像素数。
#include <cstdint>

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/scene3d.hpp"

namespace {
using namespace st::raster;
using st::math::Color;
using st::math::Vec3;
using st::math::Mat4;

constexpr int kW = 320;
constexpr int kH = 320;

/// 统计非背景像素数。
/// 统计**实体**像素数。
///
/// ## 为何不能拿“传入的清屏色”判背景（实测踩到）
///
/// `begin_frame(clear_color)` 现在会经色调映射 + sRGB 编码，所以画布里的
/// 背景色 != 传进去的那个值。拿传入值当背景 → **所有像素都被判为实体**，
/// 量出 102400（= 320×320 全屏），三种剔除模式看起来一模一样。
///
/// 正确做法：**先量一处必然是背景的位置**（左上角），用它当基准。
/// 这不依赖任何颜色管线细节。
auto lit_pixels(const Canvas& canvas) -> int {
  const st::math::Color bg = canvas.pixel_at(0, 0);   // 左上角一定是背景
  int n = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const auto c = canvas.pixel_at(x, y);
      const int d = (static_cast<int>(c.r) - bg.r) * (static_cast<int>(c.r) - bg.r) +
                    (static_cast<int>(c.g) - bg.g) * (static_cast<int>(c.g) - bg.g) +
                    (static_cast<int>(c.b) - bg.b) * (static_cast<int>(c.b) - bg.b);
      if (d > 100) ++n;
    }
  }
  return n;
}

auto render(const Mesh& mesh, Vec3 eye, CullMode cull) -> int {
  auto scene = Scene3D::create(kW, kH);
  if (!scene.has_value()) return -1;
  Canvas canvas{kW, kH, 1.0f};
  const Color bg{10, 12, 18, 255};
  canvas.clear(bg);
  Camera cam;
  cam.eye = eye;
  cam.target = Vec3{0.0f, 0.0f, 0.0f};
  (*scene)->begin_frame(bg);
  (*scene)->set_camera(cam);
  (*scene)->set_cull_mode(cull);
  (*scene)->draw_mesh(mesh, Mat4::identity());
  const auto calls = (*scene)->draw_calls();
  (*scene)->end_frame(canvas, st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                             static_cast<float>(kH)});
  (*scene)->flush();
  const int n = lit_pixels(canvas);
  st::print("    backend={} draw_calls={} lit={}\n", (*scene)->backend(), calls, n);
  return n;
}
}  // namespace

int main() {
  const Mesh plane = Mesh::plane(2.0f, 2.0f, Color{200, 200, 200, 255});  // 法线 +Z
  // 相机距离 4、fov 45°：视口高 ≈ 2*4*tan(22.5°) ≈ 3.3，平面边长 2 只占中间一部分。
  // （一开始用了 1x1 的 plane，结果它盖满全屏，三种模式都读到 102400 = 320×320，
  //  完全看不出差异——量尺不行。）
  const Vec3 front{0.0f, 0.0f, 4.0f};   // 从 +Z 看：正面朝相机
  const Vec3 back{0.0f, 0.0f, -4.0f};   // 从 -Z 看：背面朝相机

  struct Case { const char* name; CullMode mode; };
  const Case cases[] = {{"None（双面）", CullMode::None},
                        {"Back（剔背面）", CullMode::Back},
                        {"Front（剔正面）", CullMode::Front}};

  st::print("{:<18} {:>12} {:>12}\n", "剔除模式", "正面(cam+Z)", "背面(cam-Z)");
  int bad = 0;
  for (const Case& c : cases) {
    const int f = render(plane, front, c.mode);
    const int b = render(plane, back, c.mode);
    st::print("{:<18} {:>12} {:>12}\n", c.name, f, b);
    if (c.mode == CullMode::None && (f <= 0 || b <= 0)) {
      st::print("  ✗ 双面模式应两个方向都可见\n");
      ++bad;
    }
    if (c.mode == CullMode::Back && (f <= 0 || b != 0)) {
      st::print("  ✗ 剔背面：正面应可见、背面应为 0\n");
      ++bad;
    }
    if (c.mode == CullMode::Front && (f != 0 || b <= 0)) {
      st::print("  ✗ 剔正面：正面应为 0、背面应可见\n");
      ++bad;
    }
  }
  st::print("\n{}\n", bad == 0 ? "方向正确 ✓" : "方向错误 ✗");
  return bad == 0 ? 0 : 1;
}
