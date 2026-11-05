// 诊断：立方体的法线朝向对不对（与外法线比）。
//
// ## 为什么需要
//
// 多材质立方体渲染成**全黑**，而材质/光照/分组数据全部正常（`bpy_lighting_probe`
// 打出来都对）。剩下的嫌疑就是**面法线朝内**——那样 `N·L < 0`，光照贡献为 0，
// 只剩 ambient，而 ambient 0.35 乘深红/深蓝后已经很暗。
//
// 判据：立方体是中心在原点的盒子，所以顶点 `p` 处的外法线近似 `normalize(p)`。
// 逐个三角形比较它算出来的法线与那个方向：点积为负 = 朝内（错）。
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "st/raster/bpy_runtime.hpp"

namespace {

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法：bpy_normal_probe <脚本.py>\n");
    return 2;
  }
  const auto script = read_file(argv[1]);
  if (script.empty() || !st::raster::BpySceneExtractor::available()) {
    std::printf("读不到脚本或 bpy 不可用\n");
    return 1;
  }
  const auto scene = st::raster::BpySceneExtractor::scene_from_script(script);
  if (!scene.has_value()) {
    std::printf("抽取失败：%s\n", scene.error().message.c_str());
    return 1;
  }

  int inward = 0;
  int outward = 0;
  for (const auto& object : scene->objects) {
    const auto& mesh = object.mesh;
    const std::size_t stride = st::raster::Mesh::kVertexFloats;
    const std::size_t tris = mesh.indices.size() / 3;
    for (std::size_t t = 0; t < tris; ++t) {
      const std::size_t i0 = mesh.indices[t * 3] * stride;
      if (i0 + 5 >= mesh.vertices.size()) continue;
      // 抽出的法线
      const double nx = double(mesh.vertices[i0 + 3]);
      const double ny = double(mesh.vertices[i0 + 4]);
      const double nz = double(mesh.vertices[i0 + 5]);
      // 顶点位置（局部空间），用来估外法线方向
      const double px = double(mesh.vertices[i0]);
      const double py = double(mesh.vertices[i0 + 1]);
      const double pz = double(mesh.vertices[i0 + 2]);
      const double plen = std::sqrt(px * px + py * py + pz * pz);
      if (plen < 1e-6) continue;
      const double dot = (nx * px + ny * py + nz * pz) / plen;
      if (dot < 0.0) {
        ++inward;
      } else {
        ++outward;
      }
      std::printf("三角形 %2zu: 法线(%+.2f %+.2f %+.2f) 位置(%+.2f %+.2f %+.2f) dot=%+.3f %s\n",
                  t, nx, ny, nz, px, py, pz, dot, dot < 0 ? "朝内(错)" : "朝外");
    }
  }
  std::printf("\n朝外 %d / 朝内 %d\n", outward, inward);
  if (inward > 0) {
    std::printf("=> 有法线朝内的三角形，光照会算成 0（只剩 ambient）\n");
  }
  return inward > 0 ? 1 : 0;
}
