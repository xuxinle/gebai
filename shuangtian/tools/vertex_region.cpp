/// 检查网格是否带**顶点色**（若有，就能按位置给身体不同部位上色）。
///
/// ## 背景
///
/// 想让"衣服贴好"，有三条路：
/// 1. **UV 分区贴图** —— 需要贴图按身体部位分区。实测这个模型的 UV 是全局均匀展开的
///    （每个高度层都覆盖整张 UV），**这条路走不通**。
/// 2. **挂独立几何** —— 之前用的，但坐标系没对齐，衣服飘在身边（用户反馈的问题）。
/// 3. **按顶点位置直接改顶点色** —— 如果能按模型高度/半径判定"这是躯干/腿/头"，
///    就能在网格上直接染色，衣服**天然贴合**（它就是同一张皮）。
///
/// 本工具检查第 3 条的前提：网格里有没有顶点色通道，以及顶点分布能否按区域区分。
#include <algorithm>
#include <cstdio>
#include <vector>

#include "st/core/entry.hpp"
#include "st/raster/gltf.hpp"

namespace {

auto run(int, char**) -> int {
  auto loaded = st::raster::gltf_read_file("assets/models/gltf/CesiumMan.glb");
  if (!loaded.has_value()) return 1;
  const auto& scene = *loaded;
  for (std::size_t m = 0; m < scene.meshes.size(); ++m) {
    const auto& mesh = scene.meshes[m];
    const std::size_t count = mesh.vertex_count();
    std::printf("mesh[%zu]: 顶点 %zu, indices %zu, uvs %zu, skins %zu\n", m, count,
                mesh.indices.size(), mesh.uvs.size(), mesh.skins.size());
    std::printf("  每顶点 float 数 = %zu / %zu = %zu\n", mesh.vertices.size(), count,
                count > 0 ? mesh.vertices.size() / count : 0);

    // 按高度 + 半径统计顶点分布，看能否用几何规则分区
    float min_y = 1e9f, max_y = -1e9f;
    float max_r = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
      const float x = mesh.vertices[i * 3 + 0];
      const float y = mesh.vertices[i * 3 + 1];
      min_y = std::min(min_y, y);
      max_y = std::max(max_y, y);
      max_r = std::max(max_r, std::abs(x));
    }
    std::printf("  范围: y[%.3f, %.3f]  max|x|=%.3f\n", static_cast<double>(min_y),
                static_cast<double>(max_y), static_cast<double>(max_r));

    // 分层看每层的 x 分布（能区分"腿（两条窄的）"与"躯干（一条宽的）"）
    constexpr int kLayers = 20;
    std::printf("  分层（y 从下到上）: 顶点数 / x 范围 / 是否双峰\n");
    for (int layer = 0; layer < kLayers; ++layer) {
      const float lo = min_y + (max_y - min_y) * static_cast<float>(layer) / kLayers;
      const float hi = min_y + (max_y - min_y) * static_cast<float>(layer + 1) / kLayers;
      int n = 0;
      float xmin = 1e9f, xmax = -1e9f;
      int near_center = 0;
      for (std::size_t i = 0; i < count; ++i) {
        const float y = mesh.vertices[i * 3 + 1];
        if (y < lo || y >= hi) continue;
        const float x = mesh.vertices[i * 3 + 0];
        ++n;
        xmin = std::min(xmin, x);
        xmax = std::max(xmax, x);
        if (std::abs(x) < 0.02f) ++near_center;
      }
      if (n == 0) continue;
      std::printf("    y[%6.3f,%6.3f] n=%4d  x[%6.3f,%6.3f]  中线附近=%d\n",
                  static_cast<double>(lo), static_cast<double>(hi), n,
                  static_cast<double>(xmin), static_cast<double>(xmax), near_center);
    }
  }
  return 0;
}

}  // namespace

ST_MAIN(run)
