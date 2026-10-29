/// glTF 2.0 读写回归测试。
///
/// ## 判据全都钉"下游能看见的事实"
///
/// glTF 是**交换格式**：写得语法正确但语义错了，文件照样能解析，只是进了 Blender
/// 之后"看着怪"。所以这里的判据不查 JSON 字段存在与否，而是：
///
/// 1. **往返保真**：写 → 读 → 逐顶点/逐索引比对（几何、法线、UV、变换）；
/// 2. **容器合规**：`.glb` 的魔数/版本/长度/chunk 类型与对齐（Blender 与
///    Khronos 验证器逐条检查的就是这些）；
/// 3. **第三方文件能读**：手写一份"别人导出的"最小 glTF（字段顺序、出现方式不同），
///    验证解析器不依赖自己写出的那一种形态；
/// 4. **错误带路径**：坏文件必须给出 `accessors[0]` / `meshes[0].primitives[0]` 这类
///    可定位的消息（生成代码的模型据此自我修正）。
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/raster/gltf.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/primitives.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::raster::GltfObject;
using st::raster::GltfScene;
using st::raster::GltfWriteOptions;
using st::raster::Mesh;
namespace prim = st::raster::primitives;

auto make_cube_mesh() -> Mesh {
  prim::PrimitiveParams params{};
  params.radius = 0.5f;
  params.depth = 1.0f;
  params.height = 1.0f;
  params.color = Color{200, 120, 60, 255};
  return prim::make_cube(params);
}

auto make_sphere_mesh() -> Mesh {
  prim::PrimitiveParams params{};
  params.radius = 0.7f;
  params.segments = 12;
  params.rings = 8;
  return prim::make_uv_sphere(params);
}

auto to_bytes(const std::string& text) -> std::vector<std::uint8_t> {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

auto read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) -> std::uint32_t {
  std::uint32_t value = 0;
  if (offset + 4U <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

}  // namespace

ST_TEST(gltf_glb_round_trip_preserves_geometry) {
  GltfObject object{};
  object.mesh = make_sphere_mesh();
  object.name = "sphere";
  object.transform = st::math::Mat4::translation(st::math::Vec3{1.0f, 2.0f, 3.0f});
  object.material.base_color = st::math::Vec3{0.5f, 0.25f, 0.125f};
  object.material.base_color_set = true;
  object.material.metallic = 0.0f;
  object.material.roughness = 0.35f;

  GltfWriteOptions options{};
  options.binary = true;
  auto bytes = st::raster::gltf_write_binary({object}, options);
  ST_REQUIRE(bytes.has_value());
  ST_CHECK(bytes->size() > 100U);

  // 容器头：魔数 / 版本 / 总长 / 第一个 chunk 是 JSON
  ST_CHECK_EQ(read_u32(*bytes, 0), 0x46546C67U);           // "glTF"
  ST_CHECK_EQ(read_u32(*bytes, 4), 2U);                    // version
  ST_CHECK_EQ(read_u32(*bytes, 8), bytes->size());         // 声明长度 == 实际长度
  ST_CHECK_EQ(read_u32(*bytes, 16), 0x4E4F534AU);          // "JSON"

  auto scene = st::raster::gltf_read(*bytes);
  ST_REQUIRE(scene.has_value());
  ST_REQUIRE(!scene->meshes.empty());
  const Mesh& loaded = scene->meshes.front();
  ST_CHECK_EQ(loaded.vertex_count(), object.mesh.vertex_count());
  ST_CHECK_EQ(loaded.indices.size(), object.mesh.indices.size());
  // 顶点逐个比（位置 + 法线；浮点在二进制里原样往返，应逐位相同）
  int mismatched = 0;
  for (std::size_t i = 0; i < loaded.vertex_count(); ++i) {
    for (std::size_t c = 0; c < 6U; ++c) {
      const float expected = object.mesh.vertices[i * Mesh::kVertexFloats + c];
      const float actual = loaded.vertices[i * Mesh::kVertexFloats + c];
      if (std::abs(expected - actual) > 1.0e-6f) ++mismatched;
    }
  }
  ST_CHECK_EQ(mismatched, 0);
  ST_CHECK(loaded.indices == object.mesh.indices);
  // 变换（节点 matrix）也要落在读取结果的节点上
  ST_REQUIRE(!scene->nodes.empty());
  const st::math::Mat4& transform = scene->nodes.front().transform;
  ST_CHECK_NEAR(transform.value(0, 3), 1.0f, 1.0e-5f);
  ST_CHECK_NEAR(transform.value(1, 3), 2.0f, 1.0e-5f);
  ST_CHECK_NEAR(transform.value(2, 3), 3.0f, 1.0e-5f);
  // 材质：metallic 0 必须**显式写出**（glTF 缺省是 1，不写就会变成金属）
  ST_REQUIRE(!scene->materials.empty());
  ST_CHECK_NEAR(scene->materials.front().metallic, 0.0f, 1.0e-5f);
  ST_CHECK_NEAR(scene->materials.front().roughness, 0.35f, 1.0e-5f);
}

ST_TEST(gltf_glb_length_fields_are_consistent) {
  // 长度字段错对齐/错 4 字节时，Blender 会直接报 "Invalid GLB"，
  // 而本地解析器只要能容忍就"自己读得通"——所以必须逐字段核。
  GltfObject object{};
  object.mesh = make_cube_mesh();
  GltfWriteOptions options{};
  options.binary = true;
  auto bytes = st::raster::gltf_write_binary({object}, options);
  ST_REQUIRE(bytes.has_value());
  const std::uint32_t total = read_u32(*bytes, 8);
  ST_CHECK_EQ(total, static_cast<std::uint32_t>(bytes->size()));
  // JSON chunk
  const std::uint32_t json_length = read_u32(*bytes, 12);
  ST_CHECK_EQ(json_length % 4U, 0U);   // 规范要求 4 字节对齐
  // chunk 布局：[length][type][data]（type 在 length **之后**）
  const std::size_t bin_offset = 20U + json_length;
  ST_REQUIRE(bin_offset + 8U <= bytes->size());
  const std::uint32_t bin_length = read_u32(*bytes, bin_offset);
  ST_CHECK_EQ(read_u32(*bytes, bin_offset + 4U), 0x004E4942U);   // "BIN\0"
  ST_CHECK_EQ(bin_length % 4U, 0U);
  ST_CHECK_EQ(bin_offset + 8U + bin_length, bytes->size());
}

ST_TEST(gltf_gltf_text_can_be_parsed_by_others) {
  // 判据：写出的是**标准 glTF**，而不是"我们自己能读的 JSON"。
  // 这里用规范要求的必需字段逐条核对（asset/scenes/nodes/meshes/accessors/
  // bufferViews/buffers，以及 POSITION 必须带 min/max）。
  GltfObject object{};
  object.mesh = make_cube_mesh();
  object.name = "box";
  GltfWriteOptions options{};
  options.binary = false;
  options.embed_binary = true;   // 内嵌 base64：单文件，便于分发/贴进对话
  auto result = st::raster::gltf_write_json({object}, options);
  ST_REQUIRE(result.has_value());
  auto document = st::json_parse(result->json);
  ST_REQUIRE(document.has_value());

  ST_CHECK_EQ(st::json_path(*document, "asset.version").get<std::string>(), "2.0");
  ST_CHECK(!st::json_path(*document, "asset.generator").get<std::string>().empty());
  ST_CHECK_EQ(st::json_path(*document, "scenes").size(), 1U);
  ST_CHECK_EQ(st::json_path(*document, "nodes").size(), 2U);   // 根 + 一个对象
  ST_CHECK_EQ(st::json_path(*document, "meshes").size(), 1U);
  const st::Json& primitive = st::json_path(
      st::json_at(st::json_at(st::json_at(*document, "meshes")[0], "primitives")[0], "attributes"),
      "POSITION");
  ST_CHECK(primitive.is_number());
  ST_CHECK_EQ(primitive.get<int>(), 0);
  ST_CHECK_EQ(st::json_at(st::json_at(st::json_at(*document, "meshes")[0], "primitives")[0], "mode")
                  .get<int>(),
              4);   // TRIANGLES
  // POSITION 的 min/max 是**规范要求**的（下游用它做包围盒/剔除，缺了会报警告）
  ST_CHECK_EQ(st::json_at(st::json_at(*document, "accessors")[0], "min").size(), 3U);
  ST_CHECK_EQ(st::json_at(st::json_at(*document, "accessors")[0], "max").size(), 3U);
  // 内嵌缓冲：data URI
  const std::string uri = st::json_at(st::json_at(*document, "buffers")[0], "uri").get<std::string>();
  ST_CHECK(uri.rfind("data:", 0) == 0);

  // 读回来自家文本也必须还原几何
  auto scene = st::raster::gltf_read(to_bytes(result->json));
  ST_REQUIRE(scene.has_value());
  ST_REQUIRE(!scene->meshes.empty());
  ST_CHECK_EQ(scene->meshes.front().vertex_count(), object.mesh.vertex_count());
}

ST_TEST(gltf_reads_third_party_minimal_file) {
  // 手写一份"别人导出的"最小 glTF：字段顺序不同、用 data URI、无 indices
  // （非索引三角形）、无 NORMAL（必须自动算法线，否则整块是黑的）。
  const std::string json_text = R"json({
    "asset": { "version": "2.0", "generator": "test-fixture" },
    "scene": 0,
    "scenes": [ { "nodes": [0] } ],
    "nodes": [ { "name": "tri" } ],
    "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 } } ] } ],
    "accessors": [ { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                     "min": [0,0,0], "max": [1,1,0] } ],
    "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 36 } ],
    "buffers": [ { "byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
  })json";
  auto scene = st::raster::gltf_read(to_bytes(json_text));
  ST_REQUIRE(scene.has_value());
  ST_REQUIRE(!scene->meshes.empty());
  const Mesh& mesh = scene->meshes.front();
  ST_CHECK_EQ(mesh.vertex_count(), 3U);
  ST_CHECK_EQ(mesh.indices.size(), 3U);   // 无 indices → 顺序三个一组
  ST_CHECK_EQ(scene->generator, "test-fixture");
  // 法线必须被算出来（缺 NORMAL 时保留 (0,0,0) 会让光照恒为 0 → 全黑）
  int zero_normals = 0;
  for (std::size_t i = 0; i < mesh.vertex_count(); ++i) {
    const auto v = mesh[i];
    const float length = std::sqrt(v[3] * v[3] + v[4] * v[4] + v[5] * v[5]);
    if (length < 0.5f) ++zero_normals;
  }
  ST_CHECK_EQ(zero_normals, 0);
}

ST_TEST(gltf_errors_carry_json_paths) {
  // 坏文件的错误必须能指路：模型拿到 "accessors[0]" 才能改，拿到 "解析失败" 只能重写一遍。
  auto bad = st::raster::gltf_read(to_bytes(R"({"asset":{"version":"2.0"}})"));
  // 没有 meshes 不算错（空场景合法）；但 version 必须被检查
  auto wrong_version =
      st::raster::gltf_read(to_bytes(R"({"asset":{"version":"1.0"},"meshes":[]})"));
  ST_REQUIRE(!wrong_version.has_value());
  ST_CHECK(wrong_version.error().message.find("2.") != std::string::npos);

  auto missing_asset = st::raster::gltf_read(to_bytes(R"({"meshes":[]})"));
  ST_REQUIRE(!missing_asset.has_value());
  ST_CHECK(missing_asset.error().message.find("asset") != std::string::npos);

  // accessor 越界：错误里要有 accessors[0] / POSITION 路径
  const std::string out_of_range = R"json({
    "asset": { "version": "2.0" },
    "meshes": [ { "primitives": [ { "attributes": { "POSITION": 7 } } ] } ],
    "accessors": []
  })json";
  auto broken = st::raster::gltf_read(to_bytes(out_of_range));
  ST_REQUIRE(!broken.has_value());
  ST_CHECK(broken.error().message.find("meshes[0].primitives[0]") != std::string::npos);
  ST_CHECK(broken.error().message.find("POSITION") != std::string::npos);

  // 非三角形图元（mode=1 线）必须**明确报不支持**，而不是静默跳过整个文件
  const std::string lines = R"json({
    "asset": { "version": "2.0" },
    "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 }, "mode": 1 } ] } ],
    "accessors": [ { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" } ],
    "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 36 } ],
    "buffers": [ { "byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA" } ]
  })json";
  auto unsupported = st::raster::gltf_read(to_bytes(lines));
  ST_REQUIRE(!unsupported.has_value());
  ST_CHECK(unsupported.error().code == st::ErrorCode::Unsupported);
  ST_CHECK(unsupported.error().message.find("mode=4") != std::string::npos);
}

ST_TEST(gltf_file_round_trip_through_disk) {
  // 落盘往返：`.glb` 单文件、`.gltf` + 外部 `.bin` 两条路径都要能读回来。
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st_gltf_roundtrip");
  (void)st::fs::create_directories(dir);

  GltfObject object{};
  object.mesh = make_cube_mesh();
  object.name = "box";

  GltfWriteOptions binary{};
  binary.binary = true;
  const std::string glb_path = st::fs::join(dir, "cube.glb");
  ST_CHECK(st::raster::gltf_write_file(glb_path, {object}, binary).has_value());
  ST_CHECK(st::fs::exists(glb_path));
  auto from_glb = st::raster::gltf_read_file(glb_path);
  ST_REQUIRE(from_glb.has_value());
  ST_REQUIRE(!from_glb->meshes.empty());
  ST_CHECK_EQ(from_glb->meshes.front().vertex_count(), object.mesh.vertex_count());

  GltfWriteOptions separate{};
  separate.binary = false;
  separate.embed_binary = false;
  separate.binary_name = "cube.bin";
  const std::string gltf_path = st::fs::join(dir, "cube.gltf");
  ST_CHECK(st::raster::gltf_write_file(gltf_path, {object}, separate).has_value());
  ST_CHECK(st::fs::exists(gltf_path));
  // 外部 .bin 必须真的写出来——否则 .gltf 单独能解析、几何却是空的
  ST_CHECK(st::fs::exists(st::fs::join(dir, "cube.bin")));
  auto from_gltf = st::raster::gltf_read_file(gltf_path);
  ST_REQUIRE(from_gltf.has_value());
  ST_REQUIRE(!from_gltf->meshes.empty());
  ST_CHECK_EQ(from_gltf->meshes.front().vertex_count(), object.mesh.vertex_count());
  ST_CHECK_EQ(from_gltf->meshes.front().indices.size(), object.mesh.indices.size());
}
