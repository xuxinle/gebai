// 世界背景 → 环境反射：判据（防回归）。
//
// ## 背景
//
// 环境反射是**金属出色的唯一来源**（纯 Cook-Torrance 下金属除高光外全黑）。
// 而 `Lighting::env_sky` 默认是 `(0.52, 0.58, 0.68)`——一个明亮的天空。
//
// 典型 bpy 脚本的世界背景只相当于线性 `0.006`——**差 87 倍**。
//
// 实测后果（只统计主体，`tools/subject_stats`）：
//
// | | 主体均值 | 暗部 |
// |---|---|---|
// | 修前（亮天空） | 139.6 | 0.0% |
// | **修后（跟场景）** | **99.9** | **9.0%** |
// | 真 Blender | 78.1 | 34.6% |
// | 真 three.js | 74.0 | 50.2% |
//
// ## 判据
//
// 1. 抽出的 `world` 必须存在且非零
// 2. 场景的 `env_sky` 必须等于世界背景色（不是硬编码的亮天空）
// 3. 把世界背景调亮 → 金属应当变亮（证明环境确实驱动金属）
// 4. 把世界背景调暗 → 金属应当出现更多暗部
#include <cmath>
#include <cstdio>
#include <string>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/raster/bpy_runtime.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

using namespace st::raster;

int failures = 0;

auto expect(bool ok, const char* label, const std::string& detail) -> void {
  std::printf("  [%s] %-42s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
  if (!ok) ++failures;
}

/// 一个金属球的场景（背景可调）。
auto make_script(float world_value) -> std::string {
  char buffer[1024];
  std::snprintf(buffer, sizeof(buffer), R"PY(
import bpy
from mathutils import Vector, Euler

bpy.context.scene.world.background_color = (%.4f, %.4f, %.4f)

mat = bpy.data.materials.new('metal')
mat.diffuse_color = (0.72, 0.55, 0.24, 1.0)
mat.metallic = 0.95
mat.roughness = 0.22

bpy.ops.mesh.primitive_cube_add(size=2.0)
obj = bpy.context.active_object
obj.material_slots.append(mat)
obj.rotation_euler = Euler(0.5, 0.4, 0.0)

cam_data = bpy.data.cameras.new('cam')
cam_data.lens = 55.0
cam_obj = bpy.data.objects.new('cam', cam_data)
cam_obj.location = Vector((0.0, -7.0, 0.0))
cam_obj.rotation_euler = Euler(1.5708, 0.0, 0.0)

lamp = bpy.data.lights.new('key')
lamp.type = 'SUN'
lamp.energy = 2600.0
lamp_obj = bpy.data.objects.new('key', lamp)
lamp_obj.location = Vector((3.0, -4.0, 5.0))
lamp_obj.rotation_euler = Euler(0.7, 0.2, -0.5)

result = obj
)PY",
                   double(world_value), double(world_value), double(world_value));
  return std::string(buffer);
}

struct Stats {
  double mean{0.0};
  double dark{0.0};
};

auto render_stats(const SceneSpec& spec) -> Stats {
  Stats out;
  Canvas canvas{480, 360, 1.0f};
  const auto status = render_scene(spec, canvas, st::math::Color::rgba(16, 17, 21, 255));
  if (!status) return out;
  const auto pixels = canvas.pixels();
  const std::size_t total = pixels.size();
  if (total == 0) return out;
  // 背景色（角落）
  const auto bg = st::math::unpremultiply(pixels[0]);
  double sum = 0.0;
  std::size_t subject = 0;
  std::size_t dark = 0;
  for (const auto& premul : pixels) {
    const auto c = st::math::unpremultiply(premul);
    const int diff = std::abs(int(c.r) - int(bg.r)) + std::abs(int(c.g) - int(bg.g)) +
                     std::abs(int(c.b) - int(bg.b));
    if (diff <= 12) continue;   // 背景
    ++subject;
    const double luma =
        0.2126 * double(c.r) + 0.7152 * double(c.g) + 0.0722 * double(c.b);
    sum += luma;
    if (luma < 64.0) ++dark;
  }
  if (subject > 0) {
    out.mean = sum / double(subject);
    out.dark = double(dark) / double(subject);
  }
  return out;
}

}  // namespace

int main() {
  if (!BpySceneExtractor::available()) {
    std::printf("bpy 不可用：%s\n", BpySceneExtractor::unavailable_reason().c_str());
    return 1;
  }

  // ── ① 抽取：world 必须被读出来 ─────────────────────────────────────
  const float kWorldValue = 0.05f;
  const auto script = make_script(kWorldValue);
  const auto scene = BpySceneExtractor::scene_from_script(script);
  if (!scene.has_value()) {
    std::printf("  [FAIL] 抽取：%s\n", scene.error().message.c_str());
    return 1;
  }
  {
    const auto& sky = scene->lighting.env_sky;
    char detail[192];
    std::snprintf(detail, sizeof(detail), "env_sky=(%.4f %.4f %.4f) 期望 ≈%.4f",
                  double(sky.x), double(sky.y), double(sky.z), double(kWorldValue));
    const bool close = std::fabs(double(sky.x) - double(kWorldValue)) < 0.02 &&
                       std::fabs(double(sky.y) - double(kWorldValue)) < 0.02;
    expect(close, "世界背景 → env_sky（不再用亮天空）", detail);
  }

  // ── ② 背景越亮，金属越亮 ───────────────────────────────────────────
  {
    const auto dim = render_stats(*parse_scene_spec_text(make_script(0.01f)));
    const auto bright = render_stats(*parse_scene_spec_text(make_script(0.5f)));
    char detail[192];
    std::snprintf(detail, sizeof(detail), "背景 0.01 → 主体 %.1f；背景 0.5 → 主体 %.1f",
                  dim.mean, bright.mean);
    expect(bright.mean > dim.mean + 10.0, "环境驱动金属亮度（背景越亮金属越亮）", detail);
  }

  // ── ③ 暗背景时金属必须有暗部 ───────────────────────────────────────
  //
  // 这是修前最严重的问题：亮天空让一切均匀发亮，暗部恒为 0。
  {
    const auto dim = render_stats(*parse_scene_spec_text(make_script(0.008f)));
    char detail[192];
    std::snprintf(detail, sizeof(detail), "暗背景(0.008) 主体均值 %.1f，暗部 %.1f%%",
                  dim.mean, dim.dark * 100.0);
    expect(dim.dark > 0.02, "暗背景：金属出现暗部（修前恒为 0%）", detail);
  }

  // ── ④ 亮背景的金属应当比暗背景更接近过曝 ───────────────────────────
  {
    const auto dim = render_stats(*parse_scene_spec_text(make_script(0.008f)));
    const auto bright = render_stats(*parse_scene_spec_text(make_script(0.8f)));
    char detail[192];
    std::snprintf(detail, sizeof(detail), "暗 %.1f vs 亮 %.1f", dim.mean, bright.mean);
    expect(bright.mean > dim.mean, "亮背景金属更亮", detail);
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("失败 %d 项\n", failures);
    return 1;
  }
  std::printf("世界背景 → 环境反射 判据全部通过\n");
  return 0;
}
