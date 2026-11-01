/// 分析 `CesiumMan.glb` 的 **UV 布局** —— 决定"衣服贴哪里"的依据。
///
/// ## 为什么需要它
///
/// 现在用的是"在骨骼上挂独立几何"的方式做衣服，结果衣服与身体错位
/// （截图里人物是光秃秃的肤色，衣服零件飘在旁边）。
///
/// 正确做法是**改贴图**：模型自带 UV，只要知道"躯干/腿/手臂分别对应 UV 的哪一块"，
/// 就能往那些区域填衣服颜色——衣服会**自然贴合身体**（因为它本来就是同一张网格）。
///
/// 本工具把 UV 空间的分布打出来（按高度分层统计），用来推断分区：
/// 顶点在模型空间越高的地方，它落在贴图上的哪个 UV 区域。
#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

#include "st/core/entry.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/gltf.hpp"

namespace {

auto run(int, char**) -> int {
  auto loaded = st::raster::gltf_read_file("assets/models/gltf/CesiumMan.glb");
  if (!loaded.has_value()) {
    std::fprintf(stderr, "读取失败\n");
    return 1;
  }
  const auto& scene = *loaded;
  for (std::size_t m = 0; m < scene.meshes.size(); ++m) {
    const auto& mesh = scene.meshes[m];
    const std::size_t count = mesh.vertex_count();
    std::printf("mesh[%zu] 顶点 %zu, uvs=%zu (每顶点 2 个 float ⇒ %zu 个)\n", m, count,
                mesh.uvs.size(), mesh.uvs.size() / 2);
    if (mesh.uvs.size() < count * 2) {
      std::printf("  没有 UV，跳过\n");
      continue;
    }
    // 按模型 Y 高度分层，统计每层顶点的 UV 中心
    struct Bucket {
      float sum_u{0.0f};
      float sum_v{0.0f};
      int count{0};
      float min_u{1e9f}, max_u{-1e9f}, min_v{1e9f}, max_v{-1e9f};
    };
    std::map<int, Bucket> buckets;
    float min_y = 1e9f, max_y = -1e9f;
    for (std::size_t i = 0; i < count; ++i) {
      const float y = mesh.vertices[i * 3 + 1];
      min_y = std::min(min_y, y);
      max_y = std::max(max_y, y);
    }
    const int kLayers = 12;
    for (std::size_t i = 0; i < count; ++i) {
      const float y = mesh.vertices[i * 3 + 1];
      const float t = (y - min_y) / std::max(1e-5f, max_y - min_y);
      const int layer = std::clamp(static_cast<int>(t * kLayers), 0, kLayers - 1);
      const float u = mesh.uvs[i * 2 + 0];
      const float v = mesh.uvs[i * 2 + 1];
      Bucket& b = buckets[layer];
      b.sum_u += u;
      b.sum_v += v;
      b.min_u = std::min(b.min_u, u);
      b.max_u = std::max(b.max_u, u);
      b.min_v = std::min(b.min_v, v);
      b.max_v = std::max(b.max_v, v);
      ++b.count;
    }
    std::printf("  高度分层（模型 y 区间 [%.3f, %.3f]）→ UV 范围\n",
                static_cast<double>(min_y), static_cast<double>(max_y));
    for (const auto& [layer, b] : buckets) {
      if (b.count == 0) continue;
      const float y_lo = min_y + (max_y - min_y) * static_cast<float>(layer) / kLayers;
      const float y_hi = min_y + (max_y - min_y) * static_cast<float>(layer + 1) / kLayers;
      std::printf("    y[%.2f,%.2f] n=%3d  U[%.2f,%.2f] V[%.2f,%.2f]  中心(%.2f,%.2f)\n",
                  static_cast<double>(y_lo), static_cast<double>(y_hi), b.count,
                  static_cast<double>(b.min_u), static_cast<double>(b.max_u),
                  static_cast<double>(b.min_v), static_cast<double>(b.max_v),
                  static_cast<double>(b.sum_u / static_cast<float>(b.count)),
                  static_cast<double>(b.sum_v / static_cast<float>(b.count)));
    }
  }
  return 0;
}

}  // namespace

ST_MAIN(run)
