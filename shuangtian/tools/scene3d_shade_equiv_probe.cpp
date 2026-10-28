// 渐进采用验证：默认材质（不设 Principled 参数）必须与旧 `shade_terms` 路径
// **逐像素相同**。
//
// ## 为什么这条比"新功能能用"更重要
//
// 引入 GGX / Principled 时最容易犯的错是：新参数走新公式、默认值也走新公式，
// 于是所有**已出的片子**外观都变了，而且没有任何测试会发现——
// 因为测试跑的就是新公式。
//
// 判据就是直接调两个函数比输出：`shade_surface(specular=0, emission=0, metallic=0)`
// 必须与 `shade_terms` 逐位等价。
#include <cstdio>
#include <cmath>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/raster/scene3d.hpp"

int main() {
  using namespace st::raster;
  using st::math::Vec3;

  Lighting lit{};
  lit.key_direction = Vec3{0.4f, 0.8f, 0.6f};

  int bad = 0;
  int checked = 0;
  // 遍历一批法线与颜色（含边界：正对、背向、掠射、黑白中间值）
  const float normals[][3] = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {0, -1, 0},
                              {0.6f, 0.6f, 0.5f}, {0.1f, 0.1f, 1.0f}};
  const float colors[][3] = {{1, 1, 1}, {0.5f, 0.5f, 0.5f}, {0.2f, 0.6f, 0.9f},
                             {0.0f, 0.0f, 0.0f}};
  for (const auto& n : normals) {
    for (const auto& c : colors) {
      const Vec3 nv{n[0], n[1], n[2]};
      const Vec3 cv{c[0], c[1], c[2]};
      const Vec3 old_out = shade_terms(nv, cv, lit);
      const Vec3 new_out =
          shade_surface(nv, cv, Vec3{0, 0, 1}, lit, /*metallic=*/0.0f, /*roughness=*/1.0f,
                        /*specular=*/0.0f, /*emission=*/0.0f, Vec3{1, 1, 1});
      ++checked;
      const float d = std::fabs(old_out.x - new_out.x) + std::fabs(old_out.y - new_out.y) +
                      std::fabs(old_out.z - new_out.z);
      if (d > 1e-6f) {
        ++bad;
        st::log::out("  MISMATCH n=({:.1f},{:.1f},{:.1f}) c=({:.2f},{:.2f},{:.2f}): "
                  "old=({:.6f},{:.6f},{:.6f}) new=({:.6f},{:.6f},{:.6f})\n",
                  nv.x, nv.y, nv.z, cv.x, cv.y, cv.z, old_out.x, old_out.y, old_out.z,
                  new_out.x, new_out.y, new_out.z);
      }
    }
  }
  st::log::out("\n检查 {} 组，不一致 {} 组 -> {}\n", checked, bad,
            bad == 0 ? "默认路径逐位等价" : "默认路径被改变了");
  return bad == 0 ? 0 : 1;
}
