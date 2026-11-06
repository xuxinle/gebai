// 把 bpy 脚本产出的几何导出成 JSON 资产（供 three.js 脚本组装时读入）。
//
// ## 这是什么
//
// 「bpy 做资产、js 组装」的**资产端**。跑一份 bpy 建模脚本，把每个物体的
// 几何 + 材质抽出来写成 JSON。three.js 脚本读它，再把同一份几何摆到不同
// 位置 / 角度 / 材质上，组成完整场景。
//
// ## 为什么需要独立工具
//
// 抽取器（`src/python/extract.py`）已经把场景转成了完整的 `SceneSpec`
// （含相机、灯光、物体）。但资产导出只要**物体几何**那部分。
//
// 而 `SceneSpec` 的 `objects` 是 `Scene3dObject`（带 mesh），格式正好够用——
// 所以本工具直接复用 `BpySceneExtractor`，再把结果筛成资产 JSON，不重写
// 一套提取逻辑（那会与抽取器漂移）。
//
// ## 输出格式（与 `tools/bpy_export_asset.py` 的文档一致）
//
// ```json
// {
//   "format": "st-asset/1",
//   "source": "apps/.../gears_asset.py",
//   "assets": {
//     "gear_42t": {
//       "positions": [...], "normals": [...], "indices": [...],
//       "material": { "color": [r,g,b], "metalness": 0.95, "roughness": 0.22 }
//     }
//   }
// }
// ```
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "st/core/json.hpp"
#include "st/raster/bpy_runtime.hpp"

namespace {

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

/// 顶点是交错的 9 个 float（pos xyz + normal xyz + rgb）；拆成两个扁平数组。
auto split_vertices(const std::vector<float>& vertices) -> std::pair<std::vector<float>,
                                                                    std::vector<float>> {
  std::vector<float> positions;
  std::vector<float> normals;
  const std::size_t stride = 9;
  positions.reserve(vertices.size() / stride * 3);
  normals.reserve(vertices.size() / stride * 3);
  for (std::size_t i = 0; i + stride <= vertices.size(); i += stride) {
    positions.push_back(vertices[i + 0]);
    positions.push_back(vertices[i + 1]);
    positions.push_back(vertices[i + 2]);
    normals.push_back(vertices[i + 3]);
    normals.push_back(vertices[i + 4]);
    normals.push_back(vertices[i + 5]);
  }
  return {positions, normals};
}

auto to_json(const std::vector<float>& values, int precision) -> std::string {
  std::string out = "[";
  char buffer[32];
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += ',';
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, static_cast<double>(values[i]));
    out += buffer;
  }
  out += ']';
  return out;
}

auto to_json_indices(const std::vector<std::uint32_t>& values) -> std::string {
  std::string out = "[";
  char buffer[16];
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += ',';
    std::snprintf(buffer, sizeof(buffer), "%u", values[i]);
    out += buffer;
  }
  out += ']';
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("用法：bpy_export_asset <bpy脚本.py> <输出.json>\n");
    return 2;
  }
  const std::string script_path = argv[1];
  const std::string out_path = argv[2];

  if (!st::raster::BpySceneExtractor::available()) {
    std::printf("bpy 不可用：%s\n",
                st::raster::BpySceneExtractor::unavailable_reason().c_str());
    return 1;
  }

  const auto source = read_file(script_path);
  if (source.empty()) {
    std::printf("读不到脚本：%s\n", script_path.c_str());
    return 1;
  }

  const auto spec = st::raster::BpySceneExtractor::scene_from_script(source);
  if (!spec.has_value()) {
    std::printf("抽取失败：%s\n", spec.error().message.c_str());
    return 1;
  }

  // 拼资产 JSON
  std::string out = "{\"format\":\"st-asset/1\",\"source\":\"";
  out += script_path;
  out += "\",\"assets\":{";

  std::size_t written = 0;
  std::size_t total_vertices = 0;
  std::size_t total_triangles = 0;
  for (const auto& object : spec->objects) {
    const auto& mesh = object.mesh;
    if (mesh.vertices.empty()) continue;
    const auto [positions, normals] = split_vertices(mesh.vertices);
    if (positions.empty()) continue;

    if (written > 0) out += ',';
    const char* name = object.name.empty() ? "object" : object.name.c_str();
    out += '"';
    out += name;
    out += "\":{";

    out += "\"positions\":";
    out += to_json(positions, 5);
    out += ",\"normals\":";
    out += to_json(normals, 5);
    out += ",\"indices\":";
    out += to_json_indices(mesh.indices);

    const auto& material = object.material;
    char color[96];
    std::snprintf(color, sizeof(color), "[%.4f,%.4f,%.4f]",
                  static_cast<double>(material.base_color.x),
                  static_cast<double>(material.base_color.y),
                  static_cast<double>(material.base_color.z));
    char mat[256];
    std::snprintf(mat, sizeof(mat),
                  ",\"material\":{\"color\":%s,\"metalness\":%.4f,\"roughness\":%.4f}",
                  color, static_cast<double>(material.metallic),
                  static_cast<double>(material.roughness));
    out += mat;
    out += '}';

    ++written;
    total_vertices += positions.size() / 3;
    total_triangles += mesh.indices.size() / 3;
  }
  out += "}}";

  if (written == 0) {
    std::printf("脚本没有产出任何带几何的物体\n");
    return 1;
  }

  std::ofstream stream(out_path, std::ios::binary);
  if (!stream) {
    std::printf("写不了 %s\n", out_path.c_str());
    return 1;
  }
  stream << out;
  stream.close();

  std::printf("已导出 %s\n", out_path.c_str());
  std::printf("  资产 %zu 个，顶点 %zu，三角形 %zu，%zu 字节\n", written, total_vertices,
              total_triangles, out.size());
  return 0;
}
