/// 场景脚本 × 图元库 × glTF 的**端到端**回归。
///
/// ## 为什么不能只测"生成器产出了网格"
///
/// 图元的单元测试证明"几何是对的"，但它不证明"这份几何能被画出来"。
/// 这两件事之间有几个真实的失配点（都是本仓踩过的）：
///
/// - 脚本层只认五个形状名 → 新图元写进 JSON 直接报"不支持的形状"；
/// - 材质 `base_color` 与顶点色的口径不同（一个线性一个 sRGB）→ 图元被画成
///   "过曝的塑料"；
/// - 单面形状（平面/盘/网格）从背面看是**同一个像素都不画**——那是正确行为，
///   但如果相机放反了，测试会以为"形状没生成"。
///
/// 所以这里一路走到像素：解析 JSON → 渲染 → 读画布，再导出 glTF → 读回 → 比对。
#include <cmath>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gltf.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/primitives.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::raster::Canvas;
using st::raster::SceneSpec;
namespace prim = st::raster::primitives;

/// 数一数"非背景"的像素（背景取左上角实际值）。
auto inked_pixels(const Canvas& canvas) -> int {
  const Color background = canvas.pixel_at(0, 0);
  int inked = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      const int dr = static_cast<int>(pixel.r) - static_cast<int>(background.r);
      const int dg = static_cast<int>(pixel.g) - static_cast<int>(background.g);
      const int db = static_cast<int>(pixel.b) - static_cast<int>(background.b);
      if (dr * dr + dg * dg + db * db > 400) ++inked;
    }
  }
  return inked;
}

/// 渲染一个"只用一种形状"的场景，返回覆盖像素数。
auto render_shape_ink(const std::string& shape, int width, int height) -> int {
  const std::string json = R"({
    "camera": { "eye": [0.0, 0.6, 3.4], "target": [0.0, 0.0, 0.0], "fov": 45 },
    "lighting": { "ambient": 0.35, "diffuse": 0.75, "rim": 0.25 },
    "objects": [
      { "shape": ")" + shape + R"(", "radius": 0.7, "depth": 1.0, "height": 1.0,
        "segments": 16, "rings": 8, "major_radius": 0.6, "minor_radius": 0.2,
        "columns": 4, "rows": 4, "teeth": 10, "sides": 5, "turns": 2.0,
        "color": "#c8a07a", "metallic": 0.0, "roughness": 0.4 }
    ]
  })";
  auto spec = st::raster::parse_scene_spec_text(json);
  if (!spec) return -1;
  Canvas canvas(width, height);
  const auto rendered = st::raster::render_scene(*spec, canvas, Color{18, 20, 26, 255});
  if (!rendered) return -1;
  return inked_pixels(canvas);
}

}  // namespace

ST_TEST(scene_script_renders_every_primitive) {
  // 每个形状都必须"真的画出东西"。判据是覆盖像素数 > 0（而不是"没报错"）——
  // 生成器返回空网格时脚本照样走到 end_frame，画面上什么都没有。
  const char* shapes[] = {"plane",    "cube",     "uv_sphere",    "ico_sphere", "cylinder",
                          "cone",     "capsule",  "torus",        "disc",       "circle",
                          "grid",     "tube",     "prism",        "gear",       "torus_knot",
                          "arrow",    "wedge",    "tetrahedron",  "octahedron", "dodecahedron",
                          "icosahedron", "helix"};
  for (const char* shape : shapes) {
    const int ink = render_shape_ink(shape, 240, 240);
    st::log::out("[scene] {:<14} ink={}\n", shape, ink);
    ST_CHECK(ink > 0);
  }
}

ST_TEST(scene_script_keeps_legacy_shape_semantics) {
  // 既有脚本里的 `sphere` / `box` / `plane` / `cylinder` / `torus` 必须**照旧能画**，
  // 且尺寸语义不变（`box` 用 width/height/depth、`plane` 朝 +Z）。
  // 这条是"渐进采用"的底线：新图元接进来不能改老脚本的出片结果。
  const std::string json = R"({
    "camera": { "eye": [0.0, 0.0, 3.2], "target": [0.0, 0.0, 0.0], "fov": 45 },
    "objects": [ { "shape": "plane", "width": 1.6, "height": 1.6, "color": "#88aaff" } ]
  })";
  auto spec = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE(spec.has_value());
  Canvas canvas(200, 200);
  ST_CHECK(st::raster::render_scene(*spec, canvas, Color{10, 10, 10, 255}).has_value());
  // 平面朝 +Z、相机在 +Z 侧 → 必须铺满可见区域（> 半个画布）
  const int ink = inked_pixels(canvas);
  st::log::out("[scene] legacy plane ink={}\n", ink);
  ST_CHECK(ink > 200 * 200 / 4);
}

ST_TEST(scene_script_rejects_unknown_shape_with_hint) {
  // 形状写错时必须给可用清单（模型据此自我修正）+ 带对象索引
  const std::string json = R"({
    "objects": [ { "shape": "dodecahedronx" } ]
  })";
  auto spec = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE(!spec.has_value());
  ST_CHECK(spec.error().message.find("objects[0].shape") != std::string::npos);
  ST_CHECK(spec.error().message.find("dodecahedron") != std::string::npos);
  ST_CHECK(spec.error().message.find("dodecahedronx") != std::string::npos);
}

ST_TEST(scene_script_primitive_params_take_effect) {
  // 参数必须真的进到几何里：同一形状改 `radius`，包围盒必须跟着变大。
  // 判据用 `build_mesh`（不渲染），因为"大了 10%"在 240px 画面上只有几个像素差。
  st::raster::SceneObjectSpec object{};
  object.shape = "uv_sphere";
  object.radius = 0.5f;
  auto small = st::raster::build_mesh(object);
  ST_REQUIRE(small.has_value());
  object.radius = 1.0f;
  auto large = st::raster::build_mesh(object);
  ST_REQUIRE(large.has_value());
  const auto small_box = st::raster::bounds(*small);
  const auto large_box = st::raster::bounds(*large);
  ST_CHECK_NEAR(small_box.size().x, 1.0f, 0.05f);
  ST_CHECK_NEAR(large_box.size().x, 2.0f, 0.05f);

  // 齿轮的齿数：齿数翻倍 → 侧壁三角形数翻倍
  object.shape = "gear";
  object.radius = 0.6f;
  object.teeth = 8;
  auto gear8 = st::raster::build_mesh(object);
  ST_REQUIRE(gear8.has_value());
  object.teeth = 16;
  auto gear16 = st::raster::build_mesh(object);
  ST_REQUIRE(gear16.has_value());
  ST_CHECK(gear16->indices.size() > gear8->indices.size());

  // 细分次数：ico_sphere 每 +1 三角形数 ×4
  object.shape = "ico_sphere";
  object.radius = 0.5f;
  object.subdivisions = 0;
  auto ico0 = st::raster::build_mesh(object);
  ST_REQUIRE(ico0.has_value());
  object.subdivisions = 1;
  auto ico1 = st::raster::build_mesh(object);
  ST_REQUIRE(ico1.has_value());
  ST_CHECK_EQ(ico1->indices.size(), ico0->indices.size() * 4U);
}

ST_TEST(scene_primitives_export_to_gltf_and_come_back) {
  // 端到端：图元 → 场景脚本 → glTF 导出 → 读回 → 几何一致。
  // 这条把"能造、能画、能导出、能被别人打开"四件事串成一条链。
  prim::PrimitiveParams params{};
  params.radius = 0.5f;
  params.depth = 1.2f;
  params.segments = 12;
  params.minor_segments = 8;
  params.major_segments = 16;
  params.major_radius = 0.5f;
  params.minor_radius = 0.15f;
  params.teeth = 10;

  const struct {
    const char* name;
    prim::PrimitiveKind kind;
  } cases[] = {
      {"cube", prim::PrimitiveKind::Cube},
      {"uv_sphere", prim::PrimitiveKind::UvSphere},
      {"ico_sphere", prim::PrimitiveKind::IcoSphere},
      {"cylinder", prim::PrimitiveKind::Cylinder},
      {"cone", prim::PrimitiveKind::Cone},
      {"capsule", prim::PrimitiveKind::Capsule},
      {"torus", prim::PrimitiveKind::Torus},
      {"tube", prim::PrimitiveKind::Tube},
      {"gear", prim::PrimitiveKind::Gear},
      {"helix", prim::PrimitiveKind::Helix},
      {"dodecahedron", prim::PrimitiveKind::Dodecahedron},
      {"wedge", prim::PrimitiveKind::Wedge},
  };
  for (const auto& item : cases) {
    st::raster::GltfObject object{};
    object.mesh = prim::make(item.kind, params);
    object.name = item.name;
    ST_REQUIRE(!object.mesh.is_empty());
    st::raster::GltfWriteOptions options{};
    options.binary = true;
    auto bytes = st::raster::gltf_write_binary({object}, options);
    ST_REQUIRE(bytes.has_value());
    auto scene = st::raster::gltf_read(*bytes);
    ST_REQUIRE(scene.has_value());
    ST_REQUIRE(!scene->meshes.empty());
    const auto& loaded = scene->meshes.front();
    st::log::out("[gltf] {:<14} verts {} -> {} tris {} -> {}\n", item.name,
                 object.mesh.vertex_count(), loaded.vertex_count(),
                 object.mesh.indices.size() / 3U, loaded.indices.size() / 3U);
    ST_CHECK_EQ(loaded.vertex_count(), object.mesh.vertex_count());
    ST_CHECK_EQ(loaded.indices.size(), object.mesh.indices.size());
    ST_CHECK(loaded.indices == object.mesh.indices);
  }
}

ST_TEST(mesh_ops_weld_and_transform_keep_invariants) {
  // 焊接：立方体按位置+法线焊接后顶点数不变（硬边保留），
  // 而"只按位置"的重复顶点会被合掉。
  prim::PrimitiveParams params{};
  params.radius = 0.5f;
  params.depth = 1.0f;
  params.height = 1.0f;
  const auto cube = prim::make_cube(params);
  const auto welded = st::raster::weld(cube, 1.0e-5f);
  ST_CHECK_EQ(welded.vertex_count(), cube.vertex_count());   // 硬边不能被焊掉

  // 变换：非均匀缩放后法线必须仍是单位长（否则光照强度会跟着缩放漂）
  auto scaled = st::raster::transform(cube, st::math::Mat4::scaling({2.0f, 0.5f, 1.0f}));
  int bad_normals = 0;
  for (std::size_t i = 0; i < scaled.vertex_count(); ++i) {
    const auto v = scaled[i];
    const float length = std::sqrt(v[3] * v[3] + v[4] * v[4] + v[5] * v[5]);
    if (std::abs(length - 1.0f) > 0.02f) ++bad_normals;
  }
  ST_CHECK_EQ(bad_normals, 0);
  // 镜像（负缩放）会翻转绕序——变换后几何法线必须仍与顶点法线同向
  auto mirrored = st::raster::transform(cube, st::math::Mat4::scaling({-1.0f, 1.0f, 1.0f}));
  int opposite = 0;
  for (std::size_t t = 0; t + 2 < mirrored.indices.size(); t += 3) {
    const auto v0 = mirrored[mirrored.indices[t]];
    const auto v1 = mirrored[mirrored.indices[t + 1]];
    const auto v2 = mirrored[mirrored.indices[t + 2]];
    const st::math::Vec3 p0{v0[0], v0[1], v0[2]};
    const st::math::Vec3 p1{v1[0], v1[1], v1[2]};
    const st::math::Vec3 p2{v2[0], v2[1], v2[2]};
    const st::math::Vec3 geo = (p1 - p0).cross(p2 - p0);
    const st::math::Vec3 n{(v0[3] + v1[3] + v2[3]) / 3.0f, (v0[4] + v1[4] + v2[4]) / 3.0f,
                           (v0[5] + v1[5] + v2[5]) / 3.0f};
    if (geo.dot(n) < 0.0f) ++opposite;
  }
  ST_CHECK_EQ(opposite, 0);
}

ST_TEST(mesh_ops_obj_export_round_trips) {
  // OBJ 导出必须能被自家加载器读回（索引 1 基、Y-up、`//vn` 写法）。
  prim::PrimitiveParams params{};
  params.radius = 0.5f;
  params.segments = 8;
  params.rings = 6;
  const auto sphere = prim::make_uv_sphere(params);
  const std::string text = st::raster::to_obj_text(sphere, "test_sphere");
  ST_CHECK(text.find("o test_sphere") != std::string::npos);
  ST_CHECK(text.find("\nvn ") != std::string::npos);
  auto loaded = st::raster::Mesh::load_obj(text, Color{255, 255, 255, 255});
  ST_REQUIRE(loaded.has_value());
  ST_CHECK_EQ(loaded->indices.size(), sphere.indices.size());
  // 顶点数会变（OBJ 的 `f` 写法让加载器按面复制顶点），但三角形数必须一致
  ST_CHECK_EQ(loaded->indices.size() / 3U, sphere.indices.size() / 3U);
  const auto original_box = st::raster::bounds(sphere);
  const auto loaded_box = st::raster::bounds(*loaded);
  ST_CHECK_NEAR(loaded_box.size().x, original_box.size().x, 0.02f);
  ST_CHECK_NEAR(loaded_box.size().y, original_box.size().y, 0.02f);
}

ST_TEST(scene_script_loads_gltf_file) {
  // 端到端：造一个齿轮 → 导出 .glb → 在场景脚本里用 `gltf` 字段引用 → 渲染出东西。
  // 这条串起"框架图元 → 通用格式 → 回到场景"的完整回路（大模型最常写的就是这个形态）。
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st_scene_gltf_test");
  (void)st::fs::create_directories(dir);

  prim::PrimitiveParams params{};
  params.radius = 0.6f;
  params.depth = 0.35f;
  params.teeth = 12;
  params.segments = 24;
  st::raster::GltfObject object{};
  object.mesh = prim::make_gear(params);
  ST_REQUIRE(!object.mesh.is_empty());
  object.name = "gear";
  st::raster::GltfWriteOptions options{};
  options.binary = true;
  const std::string path = st::fs::join(dir, "gear.glb");
  ST_REQUIRE(st::raster::gltf_write_file(path, {object}, options).has_value());

  const std::string json = R"({
    "camera": { "eye": [0.8, 0.9, 2.6], "target": [0.0, 0.0, 0.0], "fov": 40 },
    "objects": [ { "shape": "sphere", "gltf": "gear.glb", "rotate": [0.4, 0.0, 0.0] } ]
  })";
  auto spec = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE(spec.has_value());
  // `asset_basis` 指向临时目录：`gear.glb` 是相对引用
  spec->asset_basis = dir;
  Canvas canvas(200, 200);
  ST_CHECK(st::raster::render_scene(*spec, canvas, Color{12, 12, 14, 255}).has_value());
  const int ink = inked_pixels(canvas);
  st::log::out("[scene] gltf ink={}\n", ink);
  ST_CHECK(ink > 0);

  // 文件不存在时必须报错（而不是静默画空）
  auto missing = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE(missing.has_value());
  missing->asset_basis = st::fs::join(dir, "nope");
  Canvas canvas2(64, 64);
  const auto rendered = st::raster::render_scene(*missing, canvas2, Color{0, 0, 0, 255});
  ST_CHECK(!rendered.has_value());
}
