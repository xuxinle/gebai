// 多材质槽：一个网格按面分两种材质，验证两组颜色都出现且面积合理。
//
// ## 为什么必须用像素判据
//
// 多材质的实现有三个容易错的地方，其中两个**不报错**：
//
// | 错法 | 症状 |
// |---|---|
// | 只画最后一组 | 物体整块一个颜色 |
// | 每组都整网格重画 | 也是整块一个颜色（最后一组覆盖前面） |
// | 槽位下标越界不说 | 静默用默认材质 |
//
// 三者都能"跑通"，只是画面错。所以判据必须看**两种颜色在画面上都存在**，
// 而且**面积比例接近**（否则可能是一组压了另一组）。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/raster/bpy_runtime.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

using namespace st::raster;

int failures = 0;

auto expect(bool ok, const char* label, const std::string& detail) -> void {
  std::printf("  [%s] %-34s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
  if (!ok) ++failures;
}

auto save_png(const Canvas& canvas, const std::string& path) -> bool {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const st::math::Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = c.a;
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

/// 数"偏红"与"偏蓝"的像素。
///
/// 判据用**色相倾向**而不是精确 RGB：渲染有光照与色调映射，颜色值会变，
/// 但红蓝的**相对关系**是稳定的。
struct Counts {
  std::size_t reddish{0};
  std::size_t bluish{0};
  std::size_t total{0};
};

auto count_colors(const Canvas& canvas) -> Counts {
  Counts out{};
  for (const auto& premul : canvas.pixels()) {
    const auto c = st::math::unpremultiply(premul);
    ++out.total;
    const int r = c.r;
    const int g = c.g;
    const int b = c.b;
    if (r > g + 25 && r > b + 25) {
      ++out.reddish;
    } else if (b > r + 25 && b > g + 15) {
      ++out.bluish;
    }
  }
  return out;
}

constexpr int kW = 640;
constexpr int kH = 480;

/// 立方体上下两半用不同材质：上半红、下半蓝。
///
/// 建法：bmesh 逐个建 6 个面，前 3 个（上、前、右）用槽 0，后 3 个用槽 1。
/// 这样两个材质在屏幕上各占约一半。
constexpr const char* kScript = R"PY(
import bpy
import bmesh
import math
from mathutils import Vector, Euler

mat_a = bpy.data.materials.new('upper')
mat_a.diffuse_color = (0.90, 0.12, 0.10, 1.0)   # 红
mat_a.metallic = 0.0
mat_a.roughness = 0.45

mat_b = bpy.data.materials.new('lower')
mat_b.diffuse_color = (0.10, 0.25, 0.95, 1.0)   # 蓝
mat_b.metallic = 0.0
mat_b.roughness = 0.45

mesh = bpy.data.meshes.new('two_tone')
bm = bmesh.new()
h = 1.0
pts = [(-h, -h, -h), (h, -h, -h), (h, h, -h), (-h, h, -h),
       (-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h)]
v = [bm.verts.new(p) for p in pts]
# 6 个面：前 3 个给槽 0，后 3 个给槽 1
bm.faces.new((v[4], v[5], v[6], v[7]))   # 顶
bm.faces.new((v[0], v[1], v[5], v[4]))   # 前
bm.faces.new((v[1], v[2], v[6], v[5]))   # 右
bm.faces.new((v[3], v[2], v[1], v[0]))   # 底
bm.faces.new((v[2], v[3], v[7], v[6]))   # 后
bm.faces.new((v[3], v[0], v[4], v[7]))   # 左
bm.verts.index_update()
# 按 bpy 的法：faces 顺序与 polygons 一致，逐个设 material_index
idx = 0
for face in bm.faces:
    face_index = idx
    idx += 1
bm.to_mesh(mesh)
bm.free()

mesh.polygons[0].material_index = 0
mesh.polygons[1].material_index = 0
mesh.polygons[2].material_index = 0
mesh.polygons[3].material_index = 1
mesh.polygons[4].material_index = 1
mesh.polygons[5].material_index = 1

obj = bpy.data.objects.new('two_tone', mesh)
obj.material_slots.append(mat_a)
obj.material_slots.append(mat_b)
obj.location = Vector((0.0, 0.0, 0.0))
obj.rotation_euler = Euler(0.55, 0.0, 0.65)

cam_data = bpy.data.cameras.new('cam')
cam_data.lens = 55.0
cam_obj = bpy.data.objects.new('cam', cam_data)
cam_obj.location = Vector((0.0, -6.5, 0.0))
cam_obj.rotation_euler = Euler(1.5708, 0.0, 0.0)

lamp = bpy.data.lights.new('key')
lamp.type = 'SUN'
lamp.energy = 2200.0
lamp_obj = bpy.data.objects.new('key', lamp)
lamp_obj.location = Vector((3.0, -4.0, 5.0))
lamp_obj.rotation_euler = Euler(0.7, 0.2, -0.5)

result = obj
)PY";

}  // namespace

int main() {
  if (!BpySceneExtractor::available()) {
    std::printf("bpy 不可用：%s\n", BpySceneExtractor::unavailable_reason().c_str());
    return 1;
  }

  // ── ① 抽取：分组表必须有两组 ────────────────────────────────────────
  const auto scene = BpySceneExtractor::scene_from_script(kScript);
  if (!scene.has_value()) {
    std::printf("  [FAIL] 抽取：%s\n", scene.error().message.c_str());
    return 1;
  }
  if (scene->objects.empty()) {
    std::printf("  [FAIL] 场景里没有物体\n");
    return 1;
  }
  const auto& object = scene->objects[0];
  {
    const auto groups = object.mesh.material_group_count();
    char detail[160];
    std::snprintf(detail, sizeof(detail), "%zu 组 / %zu 个材质槽", groups,
                  object.materials.size());
    expect(groups == 2, "抽取：两组材质分组", detail);
    expect(object.materials.size() == 2, "抽取：两个材质槽",
           std::to_string(object.materials.size()) + " 个");
    // 直接验那两个查询谓词（渲染分支靠它们决定走不走多材质路径）
    expect(object.mesh.has_material_groups(), "分组谓词 has_material_groups",
           object.mesh.has_material_groups() ? "true" : "false（渲染会走单材质路径）");
  }

  // ── ② 渲染 + 两种颜色都要出现 ───────────────────────────────────────
  {
    const auto spec = parse_scene_spec_text(kScript);
    if (!spec.has_value()) {
      std::printf("  [FAIL] 转 SceneSpec：%s\n", spec.error().message.c_str());
      return 1;
    }
    const st::math::Color background =
      st::math::Color::rgba(16, 17, 21, 255);
    Canvas canvas{kW, kH, 1.0f};
    const auto status = render_scene(*spec, canvas, background);
    if (!status) {
      std::printf("  [FAIL] 渲染：%s\n", status.error().message.c_str());
      return 1;
    }
    const auto counts = count_colors(canvas);
    const double red_ratio = double(counts.reddish) / double(counts.total);
    const double blue_ratio = double(counts.bluish) / double(counts.total);

    // 诊断：把画面上的颜色分布打出来（全黑/全灰时分不清是光照还是材质问题）
    std::size_t dark = 0;
    std::size_t bright = 0;
    int sample_r = 0, sample_g = 0, sample_b = 0;
    int max_sum = 0;
    for (const auto& premul : canvas.pixels()) {
      const auto c = st::math::unpremultiply(premul);
      const int sum = int(c.r) + int(c.g) + int(c.b);
      if (sum < 30) ++dark;
      if (sum > max_sum) {
        max_sum = sum;
        sample_r = c.r;
        sample_g = c.g;
        sample_b = c.b;
      }
      if (sum > 200) ++bright;
    }
    std::printf("        诊断：最亮像素 rgb(%d,%d,%d) 和=%d；暗像素 %.1f%%；亮像素 %.1f%%\n",
                sample_r, sample_g, sample_b, max_sum,
                double(dark) / double(counts.total) * 100.0,
                double(bright) / double(counts.total) * 100.0);

    char detail[192];
    std::snprintf(detail, sizeof(detail), "红 %.1f%% / 蓝 %.1f%%（两组都非零且量级相当）",
                  red_ratio * 100.0, blue_ratio * 100.0);
    // 判据：两种颜色都存在（>1%），且比例接近（都不超过对方的 4 倍）
    // —— 只画一组时的症状是其中一个恰好 0%。
    const bool both_present = red_ratio > 0.01 && blue_ratio > 0.01;
    const double ratio = red_ratio > blue_ratio ? red_ratio / blue_ratio
                                                : blue_ratio / red_ratio;
    expect(both_present && ratio < 4.0, "渲染：两种材质颜色都出现", detail);
    (void)save_png(canvas, "material_groups_render.png");
    std::printf("        已出图 material_groups_render.png\n");
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("失败 %d 项\n", failures);
    return 1;
  }
  std::printf("多材质槽全部通过\n");
  return 0;
}
