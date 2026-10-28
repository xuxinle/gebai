// 场景描述（JSON）探针：验证"一份 JSON 能完整描述一个三维场景"。
//
// ## 判据
//
// 1. **能解析**：合法 JSON → `SceneSpec`，字段与给定值一致（不是"默认值凑巧相同"）。
// 2. **错误能指路**：字段拼错/类型不对/形状未知 → 报错文本包含**字段路径**。
// 3. **能渲染**：渲染结果里有实体像素，且材质/光照参数确实生效（改一个参数画面会变）。
// 4. **确定性**：同一份 JSON 渲两次，逐像素相同。
#include <cmath>
#include <string>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {
using namespace st::raster;
using st::math::Color;

constexpr int kW = 400;
constexpr int kH = 400;

auto save_png(const Canvas& canvas, const std::string& path) -> bool {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = c.a;
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

/// 实体像素占比（与左上角背景比较）。
auto inked_ratio(const Canvas& canvas) -> double {
  const Color bg = canvas.pixel_at(0, 0);
  int n = 0;
  const int total = canvas.physical_width() * canvas.physical_height();
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color c = canvas.pixel_at(x, y);
      const int d = (static_cast<int>(c.r) - bg.r) * (static_cast<int>(c.r) - bg.r) +
                    (static_cast<int>(c.g) - bg.g) * (static_cast<int>(c.g) - bg.g) +
                    (static_cast<int>(c.b) - bg.b) * (static_cast<int>(c.b) - bg.b);
      if (d > 100) ++n;
    }
  }
  return total > 0 ? static_cast<double>(n) / total : 0.0;
}

auto mean_luma(const Canvas& canvas) -> double {
  double sum = 0.0;
  int n = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color c = canvas.pixel_at(x, y);
      sum += 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
      ++n;
    }
  }
  return n > 0 ? sum / n : 0.0;
}

auto pixels_equal(const Canvas& a, const Canvas& b) -> bool {
  const auto pa = a.pixels();
  const auto pb = b.pixels();
  if (pa.size() != pb.size()) return false;
  for (std::size_t i = 0; i < pa.size(); ++i) {
    if (pa[i] != pb[i]) return false;
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const Color background{22, 24, 32, 255};
  int failures = 0;

  // ── 1. 一份完整的场景 JSON ──
  const std::string scene_text = R"json({
    "camera": { "eye": [0, 0.7, 3.2], "target": [0, 0.1, 0], "fov": 42 },
    "lighting": {
      "key_direction": [0.4, 0.8, 0.6],
      "ambient": 0.18, "diffuse": 0.62, "rim": 0.2,
      "tone_map": "aces",
      "extra_lights": [
        { "direction": [-0.7, 0.2, 0.5], "color": "#ffd9b0", "intensity": 0.5 }
      ]
    },
    "shadow": { "strength": 0.4, "ground_y": -0.5 },
    "cull": "back",
    "objects": [
      { "shape": "plane", "width": 6, "height": 6, "color": "#6b6f7a",
        "rotate": [-1.5707963, 0, 0], "translate": [0, -0.5, 0], "ground": true },
      { "shape": "sphere", "radius": 0.6, "color": "#c8a07a", "segments": 32,
        "metallic": 0.0, "roughness": 0.18, "specular": 1.0,
        "translate": [0, 0.15, 0] },
      { "shape": "box", "width": 0.5, "height": 0.5, "depth": 0.5, "color": "#7a8fc8",
        "metallic": 1.0, "roughness": 0.25, "specular": 1.0,
        "translate": [0.95, -0.2, 0.1], "rotate": [0, 0.6, 0] }
    ]
  })json";

  st::log::out("── 1. 解析与字段核对 ──\n");
  const auto spec = parse_scene_spec_text(scene_text);
  if (!spec) {
    st::log::out("解析失败：{}\n", spec.error().message);
    return 1;
  }
  const auto check_near = [&](const char* name, double got, double want, double tol) {
    const bool ok = std::abs(got - want) <= tol;
    st::log::out("  {:<22} got={:<10.4f} want={:<10.4f} {}\n", name, got, want,
              ok ? "[OK]" : "[X]");
    if (!ok) ++failures;
  };
  check_near("camera.fov", spec->camera.fov_y_degrees, 42.0, 1e-6);
  check_near("camera.eye.y", spec->camera.eye.y, 0.7, 1e-6);
  check_near("lighting.ambient", spec->lighting.ambient, 0.18, 1e-6);
  check_near("lighting.diffuse", spec->lighting.diffuse, 0.62, 1e-6);
  check_near("extra_lights[0].i", spec->lighting.extra_lights[0].intensity, 0.5, 1e-6);
  check_near("shadow.strength", spec->shadow.strength, 0.4, 1e-6);
  check_near("shadow.ground_y", spec->shadow.ground_y, -0.5, 1e-6);
  check_near("objects 数量", static_cast<double>(spec->objects.size()), 3.0, 0.0);
  st::log::out("  {:<22} got={:<10} \n", "objects[1].shape", spec->objects[1].shape);
  st::log::out("  {:<22} got=#{:02x}{:02x}{:02x}\n", "objects[1].color",
            static_cast<int>(spec->objects[1].color.r), static_cast<int>(spec->objects[1].color.g),
            static_cast<int>(spec->objects[1].color.b));
  if (spec->objects[1].shape != "sphere") ++failures;
  if (spec->objects[1].color.r != 0xC8) ++failures;   // #c8a07a

  // ── 2. 错误能指路 ──
  st::log::out("\n── 2. 错误信息是否带字段路径 ──\n");
  struct BadCase {
    const char* name;
    const char* text;
    const char* must_contain;
  };
  const BadCase bad[] = {
      {"未知形状", R"j({"objects":[{"shape":"dodecahedron"}]})j", "objects[0].shape"},
      {"颜色格式错", R"j({"objects":[{"shape":"box","color":"#xyz"}]})j", "objects[0].color"},
      {"向量长度不足", R"j({"camera":{"eye":[0,1]}})j", "camera.eye"},
      {"缺 objects", R"j({"camera":{}})j", "objects"},
      {"tone_map 非法", R"j({"lighting":{"tone_map":"filmic2"},"objects":[]})j", "tone_map"},
      {"光源过多", R"j({"lighting":{"extra_lights":[{},{},{},{}]},"objects":[]})j", "extra_lights"},
  };
  for (const BadCase& c : bad) {
    const auto r = parse_scene_spec_text(c.text);
    const bool failed = !r.has_value();
    const std::string msg = r.has_value() ? std::string("(居然成功了)") : r.error().message;
    const bool located = msg.find(c.must_contain) != std::string::npos;
    st::log::out("  {:<16} 拒绝={} 含路径={} : {}\n", c.name, failed ? "是" : "否",
              located ? "是" : "否", msg);
    if (!failed || !located) ++failures;
  }

  {
  // ── 3. 能渲染 + 参数确实生效 ──
  st::log::out("\n── 3. 渲染与参数生效 ──\n");
  Canvas canvas{kW, kH, 1.0f};
  const auto status = render_scene(*spec, canvas, background);
  if (!status) {
    st::log::out("渲染失败：{}\n", status.error().message);
    return 1;
  }
  const double inked = inked_ratio(canvas);
  st::log::out("  实体像素占比 {:.3f}\n", inked);
  if (inked < 0.3) ++failures;   // 地面就占了大半屏
  save_png(canvas, out_dir + "/scene-json-full.png");

  // 只改 metallic（0 → 1），其余一字不动：画面必须变
  SceneSpec metal = *spec;
  metal.objects[1].metallic = 1.0f;
  Canvas metal_canvas{kW, kH, 1.0f};
  if (render_scene(metal, metal_canvas, background)) {
    const double a = mean_luma(canvas);
    const double b = mean_luma(metal_canvas);
    st::log::out("  改 metallic 前后均值：{:.2f} -> {:.2f} 差={:.2f}\n", a, b, std::abs(a - b));
    if (std::abs(a - b) < 2.0) ++failures;
    save_png(metal_canvas, out_dir + "/scene-json-metal.png");
  }

  // 关闭阴影：画面必须变亮（地面不被压暗）
  SceneSpec no_shadow = *spec;
  no_shadow.shadow.strength = 0.0f;
  Canvas ns_canvas{kW, kH, 1.0f};
  if (render_scene(no_shadow, ns_canvas, background)) {
    const double a = mean_luma(canvas);
    const double b = mean_luma(ns_canvas);
    st::log::out("  开关阴影前后均值：{:.2f} -> {:.2f}\n", a, b);
    if (b <= a) ++failures;   // 关掉阴影应当更亮
  }

  }

  // ── 4. 确定性 ──
  st::log::out("\n── 4. 确定性 ──\n");
  {
    Canvas first{kW, kH, 1.0f};
    Canvas again{kW, kH, 1.0f};
    const auto r1 = render_scene(*spec, first, background);
    const auto r2 = render_scene(*spec, again, background);
    if (r1 && r2) {
      const bool same = pixels_equal(first, again);
      st::log::out("  同一份 JSON 渲两次逐像素相同：{}\n", same ? "是" : "否");
      if (!same) ++failures;
    }
  }

  st::log::out("\n{}\n", failures == 0 ? "全部通过" : "有 " + std::to_string(failures) + " 项失败");
  return failures == 0 ? 0 : 1;
}
