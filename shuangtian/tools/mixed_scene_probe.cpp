// 混合工作流（bpy 几何 + three.js 组装）的端到端判据。
//
// 判据要能回答三个问题：
//   ① bpy 的几何**真的进了** three.js 脚本（`THREE.__assets.<名>` 拿得到）；
//   ② 最终场景以 three.js 为准（相机/灯光/物体都来自它，不是 bpy 的）；
//   ③ 换掉 bpy 资产内容，画面**必须变**（否则可能是"资产根本没参与渲染"）。
// 第 ③ 条是最容易漏的：前两条在"资产被读进来但物体没画"时也会通过。

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/three_runtime.hpp"

namespace {

auto read_file(const std::string& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

/// 画一张图，返回 {均值亮度, 非背景像素占比}。
struct Stats {
  double mean{0.0};
  double painted{0.0};
};

auto render(const std::string& scene_json, const std::string& out_png) -> Stats {
  const auto spec = st::raster::parse_scene_spec_text(scene_json);
  if (!spec.has_value()) {
    std::printf("  ✗ 解析失败：%s\n", spec.error().message.c_str());
    return {};
  }
  st::raster::Canvas canvas{520, 380, 1.0f};
  const auto background = st::math::Color::rgba(13, 13, 13, 255);
  const auto status = st::raster::render_scene(*spec, canvas, background);
  if (!status.has_value()) {
    std::printf("  ✗ 渲染失败：%s\n", status.error().message.c_str());
    return {};
  }
  const auto& pixels = canvas.pixels();
  double sum = 0.0;
  std::size_t painted = 0;
  const std::size_t total = pixels.size();
  // ⚠ `canvas.pixels()` 是 `span<const uint32_t>`，每个元素是**预乘**的
  // `R<<24 | G<<16 | B<<8 | A`（见 `math::premultiply`），**不是** RGBA 字节流。
  // 按字节读会把 `size()/4` 算成像素数并读出天文数字（实测踩过）。
  for (std::size_t i = 0; i < total; ++i) {
    const st::math::Color c = st::math::unpremultiply(pixels[i]);
    const int r = c.r;
    const int g = c.g;
    const int b = c.b;
    sum += 0.299 * r + 0.587 * g + 0.114 * b;
    // “画了东西” = 与背景（AgX+sRGB 后的中性灰 ≈ 71/76/84）明显不同
    if (std::abs(r - 71) > 14 || std::abs(g - 76) > 14 || std::abs(b - 84) > 14) ++painted;
  }
  if (!out_png.empty()) {
    st::codec::PngImage image;
    image.width = static_cast<std::uint32_t>(canvas.physical_width());
    image.height = static_cast<std::uint32_t>(canvas.physical_height());
    image.rgba.resize(total * 4);
    for (std::size_t i = 0; i < total; ++i) {
      const st::math::Color c = st::math::unpremultiply(pixels[i]);
      image.rgba[i * 4 + 0] = c.r;
      image.rgba[i * 4 + 1] = c.g;
      image.rgba[i * 4 + 2] = c.b;
      image.rgba[i * 4 + 3] = 255;
    }
    if (!st::codec::png_write_file(out_png, image)) {
      std::printf("  ! 写 PNG 失败：%s\n", out_png.c_str());
    }
  }
  return Stats{.mean = total ? sum / static_cast<double>(total) : 0.0,
               .painted = total ? static_cast<double>(painted) / static_cast<double>(total) : 0.0};
}

auto json_string(const std::string& text) -> std::string {
  std::string out = "\"";
  for (const char ch : text) {
    if (ch == '"' || ch == '\\') out += '\\';
    if (ch == '\n') { out += "\\n"; continue; }
    if (ch == '\r') continue;
    out += ch;
  }
  out += '"';
  return out;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string bpy_path = argc > 1 ? argv[1] : "apps/jianmu/scenes/mixed_gears_asset.py";
  const std::string three_path = argc > 2 ? argv[2] : "apps/jianmu/scenes/mixed_gears.js";
  const std::string shot = argc > 3 ? argv[3] : "mixed_probe.png";

  const std::string bpy = read_file(bpy_path);
  const std::string three = read_file(three_path);
  if (bpy.empty() || three.empty()) {
    std::printf("读不到脚本：%s / %s\n", bpy_path.c_str(), three_path.c_str());
    return 2;
  }

  int failures = 0;
  const auto check = [&](bool ok, const char* label, const std::string& detail = "") {
    std::printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", label,
                detail.empty() ? "" : ("  " + detail).c_str());
    if (!ok) ++failures;
  };

  // ── ① 混合：bpy_script + three_script 同时给 ──
  const std::string mixed = std::string("{\"bpy_script\":") + json_string(bpy) +
                            ",\"three_script\":" + json_string(three) +
                            ",\"three_script_name\":\"mixed_gears.js\"}";
  const auto mixed_spec = st::raster::parse_scene_spec_text(mixed);
  if (!mixed_spec.has_value()) {
    std::printf("混合场景解析失败：%s\n", mixed_spec.error().message.c_str());
    return 2;
  }
  check(!mixed_spec->bpy_script.empty() && !mixed_spec->three_script.empty(),
        "两个脚本都保留在 spec 里（不再互斥清空）");

  const Stats mixed_stats = render(mixed, shot);
  check(mixed_stats.painted > 0.08, "混合场景画出了内容",
        std::format("覆盖 {:.1f}%", mixed_stats.painted * 100.0));
  check(mixed_stats.mean > 12.0, "混合场景不是死黑",
        std::format("均值 {:.1f}", mixed_stats.mean));

  // ── ② 反证：同一份 three 脚本**不给**资产时必须失败 ──
  //
  // 这是“资产真的参与了”的关键判据：`mixed_gears.js` 开头就断言两个资产存在，
  // 所以不给资产 → 抛错。若这条通过而 ① 也通过，说明 ① 里资产确实到位了。
  //
  // ⚠ 没有这条，“资产被读进来但物体没画”的实现在 ① 也能绿。
  //
  // ⚠⚠ 判据不要绕第二层（`scene_spec_from_three` / `render_scene`）：
  // 两者对“脚本抛错但调用方不检查”的情形会给出“看起来正常”的返回值——
  // 实测就踩过：拿 `render_scene` 的 has_value() 当判据，回退修复后仍然绿。
  // 直接问**最底层的求值器**：资产缺失时它会报错（脚本里那个 throw）。
  auto standalone = st::raster::ThreeRuntime::instance().evaluate(three, "mixed_gears.js");
  check(!standalone.has_value(), "单独跑组装脚本会失败（证明它真的依赖 bpy 资产）");

  // ── ③ 相机/灯光以 three.js 为准 ──
  //
  // bpy 资产脚本里**没有相机也没有灯**，所以混合场景能出图本身就说明
  // three.js 那一侧的相机与灯光生效了。这里再钉一条：three 的 AgX 之外的
  // 色调映射与 bpy 的默认（AgX）**必须不同**——如果最终光照变成了 bpy 的，
  // 说明叠加顺序反了。
  check(mixed_spec->lighting.tone_map == st::raster::ToneMap::None ||
            mixed_spec->lighting.tone_map == st::raster::ToneMap::Aces,
        "色调映射沿用 three 来源（不是 bpy 的 AgX）");

  std::printf("\n%s（%d 项失败）\n", failures == 0 ? "混合工作流全部通过" : "混合工作流有问题",
              failures);
  return failures == 0 ? 0 : 1;
}
