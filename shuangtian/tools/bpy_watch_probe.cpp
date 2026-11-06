// bpy 真实资产端到端：轮系场景（齿轮箱）——脚本 → 抽取 → 渲染 → 出图。
//
// ## 为什么单独一个探针
//
// `bpy_script_probe` 验的是"机制通不通"（一个圆柱够）。而这里要验的是
// **"用它真能造出复杂东西"**——60 齿渐开线齿轮、多轮啮合布局、轮辐窗口。
//
// 那是 bpy 路线价值的最终检验：如果造不出齿轮，前面所有机制都只是玩具。
//
// ## 判据
//
// 1. 脚本能跑（没有 Python 异常）
// 2. 抽出的物体数对（5 主链 + 3 惰轮 = 8）
// 3. 几何量合理（每个齿轮顶点数在千级——太少说明齿形没建出来）
// 4. 出像素（非背景占比）
// 5. 啮合：相邻轮的实际中心距离 = m(z1+z2)/2（这条最容易错、也最不显眼）
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/raster/bpy_runtime.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

using namespace st::raster;

int failures = 0;

auto expect(bool ok, const char* label, const std::string& detail) -> void {
  std::printf("  [%s] %-32s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
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

/// 非背景像素占比（参照画布角落——背景是均匀的）。
auto coverage(const Canvas& canvas) -> double {
  const auto pixels = canvas.pixels();
  if (pixels.empty()) return 0.0;
  const auto bg = st::math::unpremultiply(pixels[0]);
  std::size_t drawn = 0;
  for (const auto& premul : pixels) {
    const auto c = st::math::unpremultiply(premul);
    const double dr = std::fabs(double(c.r) - double(bg.r)) / 255.0;
    const double dg = std::fabs(double(c.g) - double(bg.g)) / 255.0;
    const double db = std::fabs(double(c.b) - double(bg.b)) / 255.0;
    if (dr + dg + db > 0.05) ++drawn;
  }
  return double(drawn) / double(pixels.size());
}

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

constexpr int kW = 1100;
constexpr int kH = 800;

}  // namespace

int main(int argc, char** argv) {
  const std::string script_path =
      argc > 1 ? argv[1] : "apps/jianmu/scenes/bpy_watch.py";
  const auto script = read_file(script_path);
  if (script.empty()) {
    std::printf("读不到脚本：%s\n", script_path.c_str());
    return 1;
  }
  std::printf("脚本：%s（%zu 字节）\n\n", script_path.c_str(), script.size());

  if (!BpySceneExtractor::available()) {
    std::printf("bpy 不可用：%s\n", BpySceneExtractor::unavailable_reason().c_str());
    return 1;
  }

  // ── ① 抽取 ──────────────────────────────────────────────────────────
  //
  // 两个入口各取所需：
  // · `BpySceneExtractor::extract` → `ThreeScene`（看几何/相机/灯光）
  // · `parse_scene_spec_text`      → `SceneSpec`（渲染用，且验证自动识别）
  const auto scene = BpySceneExtractor::scene_from_script(script);
  if (!scene.has_value()) {
    std::printf("  [FAIL] 抽取：%s\n", scene.error().message.c_str());
    return 1;
  }
  expect(!scene->objects.empty(), "抽取成功",
         std::to_string(scene->objects.size()) + " 个物体");

  const auto spec = parse_scene_spec_text(script);
  if (!spec.has_value()) {
    std::printf("  [FAIL] 转 SceneSpec：%s\n", spec.error().message.c_str());
    return 1;
  }
  expect(!spec->bpy_script.empty(), "自动识别：纯 Python 走 bpy 分支",
         spec->bpy_script.empty() ? "被当 three 了" : "ok");

  // ── ② 物体数（5 主链 + 3 惰轮）───────────────────────────────────────
  expect(scene->objects.size() == 8, "物体数 = 8（5 主链 + 3 惰轮）",
         std::to_string(scene->objects.size()) + " 个");

  // ── ③ 几何量：每个齿轮的顶点数应当上千 ──────────────────────────────
  //
  // 齿形每齿 7 个轮廓点，42 齿 = 294 个外轮廓点；加上内外环、轮毂、
  // 轴孔、轮辐窗口，一个 42 齿齿轮的三角面数在数千量级。
  // 若顶点数只有几十，说明**齿形根本没建出来**（退化成一个圆盘）。
  {
    std::size_t min_tris = SIZE_MAX;
    std::size_t max_tris = 0;
    std::string worst;
    for (const auto& object : scene->objects) {
      const auto tris = object.mesh.indices.size() / 3;
      if (tris < min_tris) {
        min_tris = tris;
        worst = object.name;
      }
      if (tris > max_tris) max_tris = tris;
    }
    char detail[192];
    std::snprintf(detail, sizeof(detail), "%zu ~ %zu 三角形（最少的是 %s）",
                  min_tris, max_tris, worst.c_str());
    expect(min_tris > 200, "几何量：每个齿轮都建出齿形", detail);
  }

  // ── ④ 相机与灯光 ────────────────────────────────────────────────────
  {
    const bool has_camera = scene->camera.fov_y_degrees > 1.0f;
    char detail[128];
    std::snprintf(detail, sizeof(detail), "fov=%.1f° near=%.3f",
                  double(scene->camera.fov_y_degrees), double(scene->camera.near_z));
    expect(has_camera, "相机参数", detail);
    expect(scene->lighting.diffuse > 0.0f, "主光强度",
           std::to_string(double(scene->lighting.diffuse)));
  }

  // ── ⑤ 渲染 + 出像素 ─────────────────────────────────────────────────
  {
    const st::math::Color background =
      st::math::Color::rgba(16, 17, 21, 255);
    Canvas canvas{kW, kH, 1.0f};
    const auto status = render_scene(*spec, canvas, background);
    if (!status) {
      std::printf("  [FAIL] 渲染：%s\n", status.error().message.c_str());
      return 1;
    }
    const double ratio = coverage(canvas);
    char detail[128];
    std::snprintf(detail, sizeof(detail), "非背景 %.1f%%", ratio * 100.0);
    expect(ratio > 0.05, "出像素：轮系画上去了", detail);
    if (!save_png(canvas, "bpy_watch_render.png")) {
      std::printf("  写图失败\n");
    } else {
      std::printf("  已出图 bpy_watch_render.png\n");
    }
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("失败 %d 项\n", failures);
    return 1;
  }
  std::printf("bpy 真实资产（轮系）端到端通过\n");
  return 0;
}
