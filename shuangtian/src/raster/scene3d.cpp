/// `Lighting` 的非内联成员：目前只有 IBL 探针的归一化。
///
/// ## 为什么这一段要单独一个文件
///
/// 它是**两条腿共用的语义**（软件光栅 / D3D11），而不是某一个后端的实现细节。
/// 放进任何一个后端文件里，另一个人就会在另一个后端里"顺手再写一遍"——
/// 本仓已经因为这种"各写一遍"出过四次两条腿不一致的缺陷。
///
/// 放在 `scene3d.cpp`（`Scene3D` 这一层的公共实现）里，语义归属是清楚的：
/// **它定义的是"环境是什么"，不是"怎么画"。**

#include "st/raster/scene3d.hpp"

namespace st::raster {

auto Lighting::make_ibl_probes(math::Vec3* out_dirs, math::Vec3* out_colors,
                               std::size_t capacity) const -> std::size_t {
  if (ibl_strength <= 0.0f || out_dirs == nullptr || out_colors == nullptr) return 0;

  // ① 有显式探针：原样给出（这是 three 的 `scene.environment` 的等价物，
  //    也是 Blender `world` 有方向结构时的表达方式）。
  if (env_probe_count > 0) {
    const std::size_t count = std::min(env_probe_count, capacity);
    for (std::size_t i = 0; i < count; ++i) {
      out_dirs[i] = env_probes[i].direction;
      out_colors[i] = env_probes[i].color;
    }
    return count;
  }

  // ② 没有显式探针：用天/地两色当两个方向上/下的探针。
  //
  // 这不是"旧模型那个假反射"。区别是**它走的是同一个 IBL 公式**：
  // 漫反射按余弦加权、高光按反射向量加点、强度乘 `ibl_strength`。
  // 旧模型是把背景色乘一个系数直接加到结果上，连 BRDF 都不经过。
  //
  // 这样调用方只设 `ibl_sky`/`ibl_ground` 也能得到合理的环境光照，
  // 而不必理解"探针"这个概念——这是 bpy 的 `world`（均匀发光）与
  // 剧本里"上亮下暗"两种常见需求的短路径。
  if (capacity < 2) return 0;
  out_dirs[0] = math::Vec3{0.0f, 1.0f, 0.0f};
  out_colors[0] = ibl_sky;
  out_dirs[1] = math::Vec3{0.0f, -1.0f, 0.0f};
  out_colors[1] = ibl_ground;
  return 2;
}

}  // namespace st::raster
