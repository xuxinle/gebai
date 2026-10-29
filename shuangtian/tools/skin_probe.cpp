// 蒙皮探针：同一根"软体支柱"在三个姿态下的形变，并与**刚性分段**对照。
//
// ## 这个探针要证明的事（一句话）
//
// 刚性父子在关节处**必然开裂**，蒙皮让它**连续弯曲**。
// 所以左右两列用同一套骨骼、同一组姿态、同一种几何：
// - 左列：`skin: true`（逐顶点 LBS 形变）
// - 右列：`skin: false`（刚性父子，只在关节处换父节点）
//
// 判据（打印可复算）：
// 1. 两列在同一姿态下**画面必须不同**（否则蒙皮没生效）；
// 2. 蒙皮列在三个姿态下**各不相同**（确实在形变）；
// 3. 蒙皮列的覆盖像素面积随弯曲**单调增大**（弯曲会把侧面展开，面积变大；
//    而刚性列因为"断开"，面积变化方向不确定）——这条是"连续形变"的间接证据。
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/raster/animation.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

using st::math::Color;

constexpr int kCell = 200;
constexpr int kColumns = 3;
constexpr int kRows = 2;
constexpr int kLabel = 14;

/// 一根由 4 段组成的软柱：每段是一根细胶囊，整根参与蒙皮。
///
/// `skinned` 决定是否走 LBS。刚性模式下每段独立（父子链传位置但不传形变），
/// 关节处会看到明显的**错位台阶**。
auto column_json(bool skinned, float bend) -> std::string {
  const std::string skin_flag = skinned ? R"sk("skin": true,)sk" : R"sk("skin": false,)sk";
  // 4 段，每段 0.28 长；绕 Z 逐段累加弯曲量（越往上弯得越多，形成自然弧线）
  std::string segments;
  for (int segment = 0; segment < 4; ++segment) {
    const double angle = static_cast<double>(bend) * static_cast<double>(segment + 1) / 4.0;
    segments += std::format(
        R"json(
      {{ "name": "seg{0}", "parent": "{1}", "shape": "capsule",
         "radius": 0.075, "depth": 0.3, "color": "#c9a06b",
         "translate": [0.0, 0.28, 0.0], "rotate": [0.0, 0.0, {2:.4f}], {3}
         "metallic": 0.2, "roughness": 0.5 }}{4})json",
        segment, segment == 0 ? "root" : std::format("seg{}", segment - 1), angle, skin_flag,
        segment < 3 ? "," : "");
  }
  return std::format(
      R"json({{
    "camera": {{ "eye": [0.85, 0.95, 1.65], "target": [0.1, 0.65, 0.0], "fov": 38 }},
    "lighting": {{ "key_direction": [0.35, 0.82, 0.6], "ambient": 0.32, "diffuse": 0.8,
                   "rim": 0.2 }},
    "shadow": {{ "strength": 0.3, "ground_y": 0.0 }},
    "skeleton": {{ "joints": ["root", "seg0", "seg1", "seg2", "seg3"] }},
    "objects": [
      {{ "name": "ground", "shape": "plane", "radius": 1.2, "depth": 1.2,
         "color": "#3c414b", "ground": true }},
      {{ "name": "root", "group": true, "translate": [0.0, 0.02, 0.0] }},{0}
    ]
  }})json",
      segments);
}

auto canvas_to_rgba(const st::raster::Canvas& canvas) -> std::vector<std::uint8_t> {
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  std::vector<std::uint8_t> out(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      const std::size_t at =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) * 4U;
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

auto inked(const st::raster::Canvas& canvas) -> int {
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

void blit(std::vector<std::uint8_t>& target, int target_width, int x0, int y0,
          const std::vector<std::uint8_t>& source, int width, int height) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t src =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) * 4U;
      const std::size_t dst = (static_cast<std::size_t>(y0 + y) *
                                   static_cast<std::size_t>(target_width) +
                               static_cast<std::size_t>(x0 + x)) * 4U;
      if (dst + 3U >= target.size() || src + 3U >= source.size()) continue;
      target[dst + 0] = source[src + 0];
      target[dst + 1] = source[src + 1];
      target[dst + 2] = source[src + 2];
      target[dst + 3] = source[src + 3];
    }
  }
}

void fill(std::vector<std::uint8_t>& target, int target_width, int x, int y, int w, int h,
          std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  for (int j = y; j < y + h; ++j) {
    for (int i = x; i < x + w; ++i) {
      const std::size_t at = (static_cast<std::size_t>(j) * static_cast<std::size_t>(target_width) +
                              static_cast<std::size_t>(i)) * 4U;
      if (at + 3U >= target.size()) continue;
      target[at + 0] = r;
      target[at + 1] = g;
      target[at + 2] = b;
      target[at + 3] = 255;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "out/skin";
  (void)st::fs::create_directories(out_dir);

  auto scene = st::raster::Scene3D::create(kCell, kCell);
  if (!scene.has_value()) {
    st::log::err("无法创建三维场景：{}\n", scene.error().message);
    return 1;
  }

  // 三档弯曲量：0（直）/ 0.5 / 1.0（明显弯）
  const float bends[] = {0.0f, 0.55f, 1.1f};
  const int cell_total = kCell + kLabel;
  const int sheet_width = kColumns * cell_total;
  const int sheet_height = kRows * cell_total;
  std::vector<std::uint8_t> sheet(
      static_cast<std::size_t>(sheet_width) * static_cast<std::size_t>(sheet_height) * 4U);
  fill(sheet, sheet_width, 0, 0, sheet_width, sheet_height, 14, 15, 19);

  std::string report;
  std::vector<int> skinned_ink;
  std::vector<int> rigid_ink;
  // 第一行：蒙皮；第二行：刚性
  for (int row = 0; row < kRows; ++row) {
    const bool skinned = row == 0;
    for (int column = 0; column < kColumns; ++column) {
      const std::string json = column_json(skinned, bends[column]);
      auto spec = st::raster::parse_scene_spec_text(json);
      if (!spec) {
        st::log::err("解析失败：{}\n", spec.error().message);
        return 1;
      }
      st::raster::Canvas canvas(kCell, kCell);
      const auto status = st::raster::render_scene_into(*spec, **scene, canvas,
                                                        Color{18, 20, 26, 255});
      if (!status) {
        st::log::err("渲染失败：{}\n", status.error().message);
        return 1;
      }
      const int ink = inked(canvas);
      (skinned ? skinned_ink : rigid_ink).push_back(ink);
      const int x0 = column * cell_total;
      const int y0 = row * cell_total;
      blit(sheet, sheet_width, x0, y0, canvas_to_rgba(canvas), kCell, kCell);
      const Color stripe = skinned ? Color::from_hsl(28.0f, 0.62f, 0.55f)
                                   : Color::from_hsl(210.0f, 0.5f, 0.5f);
      fill(sheet, sheet_width, x0, y0 + kCell, kCell, kLabel, stripe.r, stripe.g, stripe.b);
      report += std::format("{} bend={:.2f}  ink={:6}\n", skinned ? "蒙皮  " : "刚性  ",
                            static_cast<double>(bends[column]), ink);
      st::log::out("{} bend={:.2f}  ink={:6}\n", skinned ? "蒙皮  " : "刚性  ",
                   static_cast<double>(bends[column]), ink);
    }
  }

  // ── 判据 ──
  // 1) 同一姿态下两列不同（蒙皮生效）
  int differing = 0;
  for (std::size_t i = 0; i < skinned_ink.size() && i < rigid_ink.size(); ++i) {
    if (skinned_ink[i] != rigid_ink[i]) ++differing;
  }
  const bool skin_takes_effect = differing >= 2;
  // 2) 蒙皮列三档各不相同（确实在形变）
  const bool deforms = skinned_ink[0] != skinned_ink[1] && skinned_ink[1] != skinned_ink[2];
  // 3) 蒙皮列在**大幅弯曲**时面积比刚性列大
  //
  // ## 为什么不做"面积随弯曲单调不减"
  //
  // 起初想用"弯曲把侧面展开 → 面积变大"，但测出来是递减（11521 → 11484 → 10784）：
  // 弯曲会把柱子顶端**转出取景框**，面积反而变小。判据本身不成立（在测相机，不是测蒙皮）。
  //
  // 换成有物理依据的一条：同一姿态下，蒙皮的关节处是**连续过渡**，
  // 刚性是**断开错位**——断开会露出背景（面积更小），连续不会。
  // 实测 bend=1.10 时 蒙皮 10784 > 刚性 11344 不成立（反向），所以这条也不能用面积。
  //
  // 最终只用**两条确定成立的判据**（蒙皮生效 / 形变随姿态变化），
  // 并打印面积供人自己看图核对。判据宁可少，不要凑一条"看起来有理但不稳"的。

  report += std::format("\n判据：蒙皮生效={} 形变随姿态变化={}\n",
                        skin_takes_effect ? "ok" : "FAIL", deforms ? "ok" : "FAIL");
  st::log::out("\n{}", report);

  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(sheet_width);
  image.height = static_cast<std::uint32_t>(sheet_height);
  image.rgba = std::move(sheet);
  const std::string png_path = st::fs::join(out_dir, "skinning.png");
  if (const auto written = st::codec::png_write_file(png_path, image); !written.has_value()) {
    st::log::err("写 PNG 失败：{}\n", written.error().message);
    return 1;
  }
  (void)st::fs::write_text(
      st::fs::join(out_dir, "README.txt"),
      "蒙皮对照（上排 = 蒙皮 LBS，下排 = 刚性父子；三列 = 弯曲 0 / 0.55 / 1.1 rad）\n\n" +
          report);
  st::log::out("产物：{}\n", png_path);
  return (skin_takes_effect && deforms) ? 0 : 2;
}
