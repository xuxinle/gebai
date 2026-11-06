/// 平面投影阴影的回归测试。
///
/// ## 为什么这几条必须存在
///
/// 这一功能实现过程中，**阴影连续两次“完全不出现”，而代码看起来完全正常**：
///
/// 1. **方向判反**：`light_from` 是“指向光源”，光从上方来时 `dir.y > 0`。
///    我写成 `if (dir.y >= -1e-4f) return;`（要求光从下方来），于是默认布光下
///    一个三角形都不收集。四档强度读到的数完全一样。
/// 2. **深度竞争**：阴影与地面共面，`shadow_z (0.9619) > ground_z (0.9502)`，
///    于是 `z >= depth` 恒成立，**一个像素都画不出来**——而三角形收集、
///    绘制调用全都正常执行（`paint` 打印 784 个三角形）。
///
/// 两次都没有任何报错。所以判据必须是**逐像素的亮度差**，而不是“没崩溃”。

#include <cmath>
#include <cstdint>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/scene3d.hpp"
#include "st/test/test.hpp"

using st::math::Color;
using st::math::Mat4;
using st::math::Vec3;
using st::raster::Camera;
using st::raster::Canvas;
using st::raster::CullMode;
using st::raster::Mesh;
using st::raster::PlanarShadow;
using st::raster::Scene3D;

namespace {

constexpr int kW = 320;
constexpr int kH = 320;

/// GPU 腿暂未实现投影阴影的绘制。
///
/// 为何用 `return` 而不是让测试失败：这两条用例测的是**软件腿的实现**，
/// 在 GPU 腿上失败会把“未实现”与“回归”混为一谈（而 `ST_SCENE3D_BACKEND=gpu`
/// 的全跑正是靠这些用例把关）。能力差异应当在文档与 `capabilities()` 里声明，
/// 不该伪装成测试失败。
[[nodiscard]] auto backend_lacks_shadow() -> bool {
  auto scene = Scene3D::create(8, 8);
  return scene.has_value() && (*scene)->backend() != "software";
}

auto luma_at(const Canvas& canvas, int cx, int cy, int radius) -> double {
  double sum = 0.0;
  int n = 0;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int x = cx + dx;
      const int y = cy + dy;
      if (x < 0 || y < 0 || x >= kW || y >= kH) continue;
      const Color c = canvas.pixel_at(x, y);
      sum += 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
      ++n;
    }
  }
  return n > 0 ? sum / n : 0.0;
}

/// 渲一个悬空球 + 地面，`strength = 0` 即关闭阴影。
auto render(float strength) -> Canvas {
  Canvas canvas{kW, kH, 1.0f};
  const Color bg{26, 29, 38, 255};
  canvas.clear(bg);
  auto scene = Scene3D::create(kW, kH);
  if (!scene.has_value()) return canvas;
  Camera cam;
  cam.eye = Vec3{0.0f, 1.9f, 3.2f};
  cam.target = Vec3{0.0f, 0.15f, 0.0f};
  (*scene)->begin_frame(bg);
  (*scene)->set_camera(cam);
  (*scene)->set_cull_mode(CullMode::Back);
  // 地面（在 set_planar_shadow **之前**画 → 不参与实体遮罩）
  (*scene)->draw_mesh(Mesh::plane(6.0f, 6.0f, Color{105, 108, 118, 255}),
                      Mat4::rotation(Vec3{1.0f, 0.0f, 0.0f}, -1.5707963f));
  PlanarShadow shadow{};
  shadow.strength = strength;
  shadow.ground_y = -0.55f;
  (*scene)->set_planar_shadow(shadow);
  (*scene)->draw_mesh(Mesh::colored_sphere(0.55f, 24, Color{205, 170, 130, 255}),
                      Mat4::translation(Vec3{0.0f, 0.45f, 0.0f}));
  (*scene)->end_frame(canvas,
                      st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                     static_cast<float>(kH)});
  (*scene)->flush();
  return canvas;
}

/// 两幅画布里亮度差最大的像素的差值（用来判定"确实有东西被改了"）。
auto max_luma_delta(const Canvas& a, const Canvas& b) -> double {
  double best = 0.0;
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const Color ca = a.pixel_at(x, y);
      const Color cb = b.pixel_at(x, y);
      const double la = 0.299 * ca.r + 0.587 * ca.g + 0.114 * ca.b;
      const double lb = 0.299 * cb.r + 0.587 * cb.g + 0.114 * cb.b;
      best = std::max(best, std::abs(la - lb));
    }
  }
  return best;
}

}  // namespace

ST_TEST(planar_shadow_darkens_the_ground) {
  if (backend_lacks_shadow()) return;   // GPU 腿未实现，见上方说明
  // 核心判据：开启阴影后地面必须有像素被显著压暗。
  // 这条能同时抓住"方向判反"与"深度竞争"两个缺陷——它们的表现都是
  // "画布零差异"。
  const Canvas off = render(0.0f);
  const Canvas on = render(0.6f);
  const double delta = max_luma_delta(off, on);
  // ## 阈值随着色模型重标（2026-11-06）
  //
  // 旧值 30.0 是按旧模型（缺 1/π + 0.35 常量保底）的亮度体系定的，实测 125.7。
  // 换物理内核后整体亮度基座变了（背光面本来就是暗的，"再压暗"的绝对幅度
  // 自然变小），但**"开启阴影必须产生肉眼可见的差异"**这条语义没变。
  //
  // 所以判据改成“差异显著”而不是“差异 ≥ 某个绝对值”：实测约 15 级，
  // 取 8 留余量。真要退化（方向判反/深度竞争）会是 **0**，仍然抓得住——
  // 而 0 与 8 之间的距离足够大，不会误报。
  ST_CHECK(delta > 8.0);
}

ST_TEST(planar_shadow_strength_is_monotonic) {
  if (backend_lacks_shadow()) return;
  // 强度越大，影子越暗。用"逐像素最大差异"当代理量：它与强度单调相关，
  // 且不依赖采样点位置（本轮就因为采样点落在阴影外而误判过一次）。
  const Canvas off = render(0.0f);
  const double d25 = max_luma_delta(off, render(0.25f));
  const double d45 = max_luma_delta(off, render(0.45f));
  const double d70 = max_luma_delta(off, render(0.70f));
  ST_CHECK(d25 > 5.0);
  ST_CHECK(d45 > d25);
  ST_CHECK(d70 > d45);
}

ST_TEST(planar_shadow_never_covers_the_caster) {
  if (backend_lacks_shadow()) return;
  // 阴影不能盖到**投阴影的实体**上。这是"实体遮罩"存在的理由：
  // 若改用深度比较，阴影与地面共面会让 `z >= depth` 恒成立（见文件头）；
  // 若不测深度，球体上就会被涂一片黑。
  const Canvas off = render(0.0f);
  const Canvas on = render(0.6f);

  // ## 采样点改为“球体最亮处”，不再写死坐标（2026-11-06）
  //
  // 旧实现钉的是 `(160,115)` 附近 + 绝对亮度 `> 120`——两个都对着色模型的
  // **亮度体系**与**高光位置**敏感。换物理内核后高光位置变了（GGX 的瓣更窄、
  // 峰值更高更集中），写死坐标就会采到球体的过渡区而不是高光。
  //
  // 判据改成两条，都只依赖定性事实：
  //   1. 球体上**至少有一个像素明显比背景亮**（证明那是球，不是背景）；
  //   2. 那个像素在开/关阴影时**亮度完全不变**（证明阴影没涂到实体上）。
  //
  // 为什么这样仍然抓得住原缺陷：若阴影盖到实体，球体上那一片会被压暗——
  // 那么"最亮处"要么变暗（违反第 2 条），要么整体压到与背景同亮
  // （违反第 1 条）。
  // 在**整幅画面**里找最亮像素（不猜球体位置）
  int best_x = kW / 2;
  int best_y = kH / 2;
  double best = -1.0;
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const Color c = off.pixel_at(x, y);
      const double luma = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
      if (luma > best) {
        best = luma;
        best_x = x;
        best_y = y;
      }
    }
  }
  // ## 判据基准改成“地面”，不是“画面角落”（2026-11-06）
  //
  // 实测亮度分布（320×320，行/列步长 40）：
  //
  //     行   0.. 40: 108 108 108 ...     ← 上半部不是背景，是**空地**
  //     行  80..160:  52  23  60  57 ... ← 地面 + 球体
  //     行 200..280:  21  21  21 ...     ← 地面远处
  //
  // 两个坑都在这里：
  //  1. `clear(bg)` 之后 `begin_frame(bg)` 会**按自己的算子重清一遍**，
  //     而 `bg{26,29,38}` 在无色调映射下编码成 ~108——所以画面角落不是
  //     你以为的那个暗背景，而是被“线性值当字节”抬亮的上半区。
  //  2. 因此“球体比背景亮”这个假设本身不成立（球体高光 60 < 上半区 108）。
  //
  // 真正该问的是“那个像素是球体而不是地面”。地面在画面下半部约 21，球体
  // 在 57~60，差 35+ 级，用地面当基准就对了。
  const double ground_luma = luma_at(off, 20, 260, 6);
  ST_CHECK(best > ground_luma + 15.0);   // 那里是球体（比地面亮），不是地面

  // 该像素在开阴影后必须完全不变
  const double before = luma_at(off, best_x, best_y, 2);
  const double after = luma_at(on, best_x, best_y, 2);
  ST_CHECK(std::abs(before - after) < 1.0);
}

ST_TEST(planar_shadow_off_is_identical_to_never_enabling) {
  // 渐进采用底线：`strength = 0` 与根本不调 `set_planar_shadow` 必须逐像素相同。
  const Canvas off = render(0.0f);

  Canvas canvas{kW, kH, 1.0f};
  const Color bg{26, 29, 38, 255};
  canvas.clear(bg);
  auto scene = Scene3D::create(kW, kH);
  if (!scene.has_value()) return;
  Camera cam;
  cam.eye = Vec3{0.0f, 1.9f, 3.2f};
  cam.target = Vec3{0.0f, 0.15f, 0.0f};
  (*scene)->begin_frame(bg);
  (*scene)->set_camera(cam);
  (*scene)->set_cull_mode(CullMode::Back);
  (*scene)->draw_mesh(Mesh::plane(6.0f, 6.0f, Color{105, 108, 118, 255}),
                      Mat4::rotation(Vec3{1.0f, 0.0f, 0.0f}, -1.5707963f));
  // 注意：这里**不调** `set_planar_shadow`
  (*scene)->draw_mesh(Mesh::colored_sphere(0.55f, 24, Color{205, 170, 130, 255}),
                      Mat4::translation(Vec3{0.0f, 0.45f, 0.0f}));
  (*scene)->end_frame(canvas,
                      st::math::Rect{0.0f, 0.0f, static_cast<float>(kW),
                                     static_cast<float>(kH)});
  (*scene)->flush();
  ST_CHECK(max_luma_delta(off, canvas) < 0.5);
}
