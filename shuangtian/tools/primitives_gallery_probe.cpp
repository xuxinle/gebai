// 图元库总览探针：把 22 个图元各渲一格，拼成一张 PNG，并把每个图元导出成 `.glb`。
//
// ## 为什么要有这个探针（而不只是单元测试）
//
// 单元测试证明的是"几何契约成立"（水密/绕序/法线/UV）。它证明不了**观感**：
// 一个法线全都朝外、水密、UV 正常的模型，仍然可能"看着是扁的"——
// 比如齿轮的齿高太小、螺旋的管径太细、正十二面体的面被算错但仍在合法范围内。
//
// 所以这里一并出去两件东西：
// 1. `out/primitives/gallery.png`：22 格 + 名字的对照图，一眼能看出谁不对；
// 2. `out/primitives/<name>.glb`：每格同时导出成**能被 Blender/引擎打开**的文件，
//    这是"通用格式"这条要求的可验证产物（拖进 Blender 就能看到同一个形状）。
//
// 判据（打印在输出里，可复算）：
// - 每格的覆盖像素数 > 0（画出来了）且 < 画布面积（不是一整块纯色）；
// - 每格的 `.glb` 字节数 > 0 且能被自家读取器读回、顶点数一致；
// - 汇总行给出"通过格数 / 总格数"，便于 CI 或人工一眼判断。
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gltf.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/primitives.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using namespace st::raster;
using st::math::Color;
namespace prim = st::raster::primitives;
using prim::PrimitiveKind;

constexpr int kCell = 220;        // 每格边长（物理像素）
constexpr int kColumns = 6;       // 每行格数
constexpr int kLabelHeight = 20;  // 名字条高度

/// 把画布像素（0xAARRGGBB、已预乘）转成 PNG 需要的 RGBA8（非预乘）。
auto canvas_to_rgba(const Canvas& canvas) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  out.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      const std::size_t at =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) * 4U;
      // 画布内部是预乘 alpha；这里把颜色反预乘（否则半透明边缘会偏暗）
      const float alpha = static_cast<float>(pixel.a) / 255.0f;
      const auto unpremultiply = [alpha](std::uint8_t value) {
        if (alpha <= 0.0f) return static_cast<std::uint8_t>(0);
        const float scaled = static_cast<float>(value) / alpha;
        return static_cast<std::uint8_t>(std::lround(std::min(scaled, 255.0f)));
      };
      out[at + 0] = unpremultiply(pixel.r);
      out[at + 1] = unpremultiply(pixel.g);
      out[at + 2] = unpremultiply(pixel.b);
      out[at + 3] = pixel.a;
    }
  }
  return out;
}

/// 统计非背景像素（判"画出来了"）。
auto inked(const Canvas& canvas) -> int {
  const Color background = canvas.pixel_at(0, 0);
  int count = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      const int dr = static_cast<int>(pixel.r) - static_cast<int>(background.r);
      const int dg = static_cast<int>(pixel.g) - static_cast<int>(background.g);
      const int db = static_cast<int>(pixel.b) - static_cast<int>(background.b);
      if (dr * dr + dg * dg + db * db > 400) ++count;
    }
  }
  return count;
}

/// 把一个 RGBA8 图块贴到总图上的指定格子（含 1px 分隔线）。
void blit(std::vector<std::uint8_t>& target, int target_width, int cell_x, int cell_y,
          const std::vector<std::uint8_t>& source, int source_width, int source_height) {
  for (int y = 0; y < source_height; ++y) {
    for (int x = 0; x < source_width; ++x) {
      const int tx = cell_x + x;
      const int ty = cell_y + y;
      const std::size_t src =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(source_width) +
           static_cast<std::size_t>(x)) * 4U;
      const std::size_t dst = (static_cast<std::size_t>(ty) * static_cast<std::size_t>(target_width) +
                               static_cast<std::size_t>(tx)) * 4U;
      if (dst + 3U >= target.size()) continue;
      // 直接覆盖（每格都有自己的不透明背景）
      target[dst + 0] = source[src + 0];
      target[dst + 1] = source[src + 1];
      target[dst + 2] = source[src + 2];
      target[dst + 3] = source[src + 3];
    }
  }
}

/// 在格子上画一条"名字条"（一个色块 + 名称的摩斯式编码：这里只画纯色条，
/// 名称同时写进 PNG 旁边的 `README.txt`——探针不做字体渲染，那是 text 层的事）。
void fill_rect(std::vector<std::uint8_t>& target, int target_width, int x0, int y0, int w, int h,
               std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  for (int y = y0; y < y0 + h; ++y) {
    for (int x = x0; x < x0 + w; ++x) {
      const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(target_width) +
                              static_cast<std::size_t>(x)) * 4U;
      if (at + 3U >= target.size()) continue;
      target[at + 0] = r;
      target[at + 1] = g;
      target[at + 2] = b;
      target[at + 3] = 255;
    }
  }
}

auto params_for(PrimitiveKind kind) -> prim::PrimitiveParams {
  prim::PrimitiveParams params{};
  // 统一"单位尺度"：所有图元的包围盒都在 1.2 左右，镜头才不用逐个调
  params.radius = 0.55f;
  params.depth = 1.05f;
  params.height = 1.05f;
  params.segments = 40;
  params.rings = 20;
  params.subdivisions = 2;
  params.major_segments = 48;
  params.minor_segments = 20;
  params.major_radius = 0.42f;
  params.minor_radius = 0.14f;
  params.top_radius = kind == PrimitiveKind::Cone ? 0.28f : 0.0f;
  params.columns = 6;
  params.rows = 6;
  // ⚠ `sides` 非 0 时会**覆盖** `segments`（棱柱专用），所以只给棱柱设
  params.sides = kind == PrimitiveKind::Prism ? 6 : 0;
  params.teeth = 14;
  params.tooth_height = 0.12f;
  params.tooth_width = 0.45f;
  params.turns = 2.5f;
  params.knot_p = 2;
  params.knot_q = 3;
  params.color = Color{212, 176, 122, 255};   // 金属感的暖色，能看清高光与明暗
  return params;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "out/primitives";
  (void)st::fs::create_directories(out_dir);

  const std::vector<PrimitiveKind> kinds = {
      PrimitiveKind::Plane,        PrimitiveKind::Cube,         PrimitiveKind::UvSphere,
      PrimitiveKind::IcoSphere,    PrimitiveKind::Cylinder,     PrimitiveKind::Cone,
      PrimitiveKind::Capsule,      PrimitiveKind::Torus,        PrimitiveKind::Disc,
      PrimitiveKind::Circle,       PrimitiveKind::Grid,         PrimitiveKind::Tube,
      PrimitiveKind::Prism,        PrimitiveKind::Gear,         PrimitiveKind::TorusKnot,
      PrimitiveKind::Arrow,        PrimitiveKind::Wedge,        PrimitiveKind::Tetrahedron,
      PrimitiveKind::Octahedron,   PrimitiveKind::Dodecahedron, PrimitiveKind::Icosahedron,
      PrimitiveKind::Helix};

  const int rows = (static_cast<int>(kinds.size()) + kColumns - 1) / kColumns;
  const int cell_total = kCell + kLabelHeight;
  const int sheet_width = kColumns * cell_total;
  const int sheet_height = rows * cell_total;
  std::vector<std::uint8_t> sheet(
      static_cast<std::size_t>(sheet_width) * static_cast<std::size_t>(sheet_height) * 4U, 0U);
  fill_rect(sheet, sheet_width, 0, 0, sheet_width, sheet_height, 16, 17, 21);

  auto scene = Scene3D::create(kCell, kCell);
  if (!scene.has_value()) {
    st::log::err("无法创建三维场景：{}\n", scene.error().message);
    return 1;
  }
  st::log::out("三维修道：{}（{} × {}）\n", (*scene)->backend(), (*scene)->width(), (*scene)->height());

  int passed = 0;
  std::string manifest;
  int index = 0;
  for (const PrimitiveKind kind : kinds) {
    const std::string name{prim::kind_name(kind)};
    const prim::PrimitiveParams params = params_for(kind);
    const Mesh mesh = prim::make(kind, params);
    const auto box = bounds(mesh);

    // ── 渲染一格 ──
    Canvas canvas(kCell, kCell);
    canvas.clear(Color{24, 26, 32, 255});
    Camera camera{};
    camera.eye = st::math::Vec3{1.55f, 1.15f, 2.35f};
    camera.target = st::math::Vec3{0.0f, 0.0f, 0.0f};
    camera.fov_y_degrees = 38.0f;
    camera.near_z = 0.05f;
    camera.far_z = 40.0f;
    Lighting lighting{};
    lighting.exposure = 1.0f;
    lighting.tone_map = ToneMap::Aces;
    (*scene)->begin_frame(Color{2, 2, 3, 255});
    (*scene)->set_camera(camera);
    (*scene)->set_lighting(lighting);
    Material material{};
    material.base_color = st::math::Vec3{0.0f, 0.0f, 0.0f};   // 用顶点色（图元自带）
    material.base_color_set = false;
    material.metallic = 0.55f;
    material.roughness = 0.32f;
    material.specular = 1.0f;
    (*scene)->set_material(material);
    (*scene)->draw_mesh(mesh, st::math::Mat4::identity());
    (*scene)->end_frame(canvas, st::math::Rect{0.0f, 0.0f, static_cast<float>(kCell),
                                               static_cast<float>(kCell)});
    (*scene)->flush();

    const int ink = inked(canvas);
    const bool drawn = ink > 0 && ink < kCell * kCell;
    if (drawn) ++passed;

    // ── 贴进总图 ──
    const int cell_x = (index % kColumns) * cell_total;
    const int cell_y = (index / kColumns) * cell_total;
    const auto rgba = canvas_to_rgba(canvas);
    blit(sheet, sheet_width, cell_x, cell_y, rgba, kCell, kCell);
    // 名字条：色相随序号变化（人眼能对上 manifest 的顺序）
    const Color stripe = st::math::Color::from_hsl(static_cast<float>(index) * 37.0f, 0.55f, 0.55f);
    fill_rect(sheet, sheet_width, cell_x, cell_y + kCell, kCell, kLabelHeight, stripe.r, stripe.g,
              stripe.b);

    // ── 导出 glTF（能被 Blender/引擎打开）──
    GltfObject object{};
    object.mesh = mesh;
    object.name = name;
    GltfWriteOptions options{};
    options.binary = true;
    options.root_name = name;
    const std::string glb_path = st::fs::join(out_dir, name + ".glb");
    bool exported = false;
    if (const auto written = gltf_write_file(glb_path, {object}, options); written.has_value()) {
      if (const auto read_back = gltf_read_file(glb_path); read_back.has_value()) {
        exported = !read_back->meshes.empty() &&
                   read_back->meshes.front().vertex_count() == mesh.vertex_count();
      }
    }

    st::log::out("{:<14} ink={:6} verts={:5} tris={:5} size=({:.2f},{:.2f},{:.2f}) glb={}\n",
                 name, ink, mesh.vertex_count(), mesh.indices.size() / 3U, box.size().x,
                 box.size().y, box.size().z, exported ? "ok" : "FAIL");
    manifest += std::format("{:<14} ink={:6} verts={:5} tris={:5} glb={}\n", name, ink,
                            mesh.vertex_count(), mesh.indices.size() / 3U,
                            exported ? "ok" : "FAIL");
    ++index;
  }

  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(sheet_width);
  image.height = static_cast<std::uint32_t>(sheet_height);
  image.rgba = std::move(sheet);
  const std::string png_path = st::fs::join(out_dir, "gallery.png");
  if (const auto written = st::codec::png_write_file(png_path, image); !written.has_value()) {
    st::log::err("写 PNG 失败：{}\n", written.error().message);
    return 1;
  }
  (void)st::fs::write_text(st::fs::join(out_dir, "README.txt"),
                           "图元总览（顺序 = gallery.png 的格子顺序，从左到右、从上到下）\n\n" +
                               manifest + std::format("\n通过 {}/{} 格\n", passed,
                                                      static_cast<int>(kinds.size())));
  st::log::out("\n产物：\n  {}\n  {}\n  共 {}/{} 格通过\n", png_path,
               st::fs::join(out_dir, "README.txt"), passed, static_cast<int>(kinds.size()));
  return passed == static_cast<int>(kinds.size()) ? 0 : 2;
}
