// 组合模型空间体系的视觉探针：**同一份模型，只改枢轴/父子关系**，逐格出图对照。
//
// ## 为什么必须看图（单元测试不够）
//
// `tests/geom_scene_graph_test.cpp` 钉的是世界矩阵的数值（父动子动、枢轴符号、
// 扁平=嵌套）。它无法回答"这个组合体看起来是不是一个整体"——
// 零件之间是否对齐、枢轴选得是否符合直觉、转动之后是否还"站在那里"。
//
// 所以这里做四格对照，每格都改**一个变量**：
//
// | 格 | 变量 | 期望看到的差异 |
// |---|---|---|
// | 1 | 机械臂竖直（无旋转） | 基准姿态 |
// | 2 | 肩关节转 50° | 整条手臂绕**肩**摆开，肘/手跟着走 |
// | 3 | 同上但枢轴在原点（错误用法） | 手臂整体**平移**着甩出去（而不是绕肩） |
// | 4 | 同样姿态，但父节点被隐藏 | 整棵子树消失（不是只少一个零件） |
//
// 判据（打印出来可复算）：四格的覆盖像素数必须两两不同；2、3 两格的差异
// 正是"枢轴生效"的直接证据。
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

using st::math::Color;

constexpr int kCell = 300;

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
      // 画布内部预乘 → PNG 要非预乘
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

/// 机械臂模型：底座 + 肩（分组，枢轴在肩关节点）+ 上臂 + 肘 + 前臂 + 手。
///
/// 参数只改**一个**：肩关节的旋转角、以及肩的枢轴是否在关节点上。
auto robot_json(float shoulder_angle, bool pivot_at_joint, bool hide_shoulder) -> std::string {
  // 肩的局部原点就在关节点上（底座顶端），所以"绕关节点转" = 枢轴为 0。
  // 第三格的错误用法是**把枢轴放在底座中心**（世界 (0,0,0)），于是转动会带着手臂平移。
  // ## 两格的差别只有枢轴
  //
  // 肩的局部原点**就是肩关节点**（父底座的顶端）。所以：
  // - 正确用法：枢轴 = (0,0,0)（绕关节点转，手臂以肩为圆心摆开）；
  // - 错误用法：枢轴 = (0,-0.12,0)（在底座中心），于是整条手臂被**甩出去**
  //   而不是绕着肩转——这正是"没有枢轴概念时只能靠父节点加偏移"那种土办法
  //   最容易出的错。
  const std::string pivot =
      pivot_at_joint ? R"pv("pivot": [0.0, 0.0, 0.0])pv"
                     : R"pv("pivot": [0.0, -0.1, 0.0])pv";
  return std::format(R"json({{
    "camera": {{ "eye": [1.3, 1.05, 2.3], "target": [0.05, 0.55, 0.0], "fov": 40 }},
    "lighting": {{ "light_from": [0.4, 0.8, 0.6], "ambient": 0.35, "diffuse": 0.7, "rim": 0.2 }},
    "shadow": {{ "strength": 0.35, "ground_y": 0.0 }},
    "objects": [
      {{"name": "ground", "shape": "plane", "color": "#3c414b", "ground": true, "size": [2.8, 1.4]}},
      {{"name": "base", "shape": "cylinder", "color": "#8a8f99", "translate": [0.0, 0.05, 0.0], "metallic": 0.6, "roughness": 0.35, "size": [0.44, 0.1]}},
      {{ "name": "shoulder", "group": true, "translate": [0.0, 0.1, 0.0],
         "rotation": [0.0, 0.0, {}], "visible": {}, {},
         "children": [
           {{"name": "upper", "shape": "capsule", "color": "#d4b07a", "translate": [0.0, 0.28, 0.0], "metallic": 0.3, "roughness": 0.4, "size": [0.15, 0.62]}},
           {{"name": "elbow", "shape": "uv_sphere", "color": "#6f7681", "translate": [0.0, 0.58, 0.0], "metallic": 0.8, "roughness": 0.25, "size": 0.18}},
           {{ "name": "forearm", "group": true, "translate": [0.0, 0.58, 0.0],
              "rotation": [0.0, 0.0, -0.9],
              "children": [
                {{"name": "lower", "shape": "capsule", "color": "#c8a07a", "translate": [0.0, 0.22, 0.0], "metallic": 0.3, "roughness": 0.4, "size": [0.12, 0.5]}},
                {{"name": "hand", "shape": "box", "color": "#6f7681", "translate": [0.0, 0.5, 0.0], "metallic": 0.85, "roughness": 0.2, "size": [0.15, 0.1, 0.1]}}
              ] }}
         ] }}
    ]
  }})json",
                    shoulder_angle, hide_shoulder ? "false" : "true", pivot);
}

/// 渲染一张到画布。
auto render(const std::string& json) -> st::raster::Canvas {
  st::raster::Canvas canvas(kCell, kCell);
  auto spec = st::raster::parse_scene_spec_text(json);
  if (!spec) {
    st::log::err("解析失败：{}\n", spec.error().message);
    return canvas;
  }
  auto scene = st::raster::Scene3D::create(kCell, kCell);
  if (!scene) return canvas;
  const auto status =
      st::raster::render_scene_into(*spec, **scene, canvas, Color{18, 20, 26, 255});
  if (!status) st::log::err("渲染失败：{}\n", status.error().message);
  return canvas;
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
  const std::string out_dir = argc > 1 ? argv[1] : "out/scene_graph";
  (void)st::fs::create_directories(out_dir);

  struct Case {
    const char* label;
    std::string json;
  };
  const std::vector<Case> cases = {
      {"A 竖直基准", robot_json(0.0f, true, false)},
      {"B 绕肩转 50°", robot_json(0.87f, true, false)},
      {"C 枢轴错放（整体甩出）", robot_json(0.87f, false, false)},
      {"D 隐藏肩（整棵子树消失）", robot_json(0.0f, true, true)},
  };

  constexpr int kColumns = 2;
  constexpr int kLabel = 18;
  const int cell_total = kCell + kLabel;
  const int sheet_width = kColumns * cell_total;
  const int sheet_height = static_cast<int>(cases.size()) * cell_total / kColumns;
  std::vector<std::uint8_t> sheet(
      static_cast<std::size_t>(sheet_width) * static_cast<std::size_t>(sheet_height) * 4U);
  fill(sheet, sheet_width, 0, 0, sheet_width, sheet_height, 14, 15, 19);

  std::string report;
  std::vector<int> inks;
  int index = 0;
  for (const Case& item : cases) {
    const st::raster::Canvas canvas = render(item.json);
    const int ink = inked(canvas);
    inks.push_back(ink);
    const int cell_x = (index % kColumns) * cell_total;
    const int cell_y = (index / kColumns) * cell_total;
    blit(sheet, sheet_width, cell_x, cell_y, canvas_to_rgba(canvas), kCell, kCell);
    const Color stripe = Color::from_hsl(static_cast<float>(index) * 62.0f, 0.6f, 0.55f);
    fill(sheet, sheet_width, cell_x, cell_y + kCell, kCell, kLabel, stripe.r, stripe.g, stripe.b);
    report += std::format("{:<26} ink={:6}\n", item.label, ink);
    st::log::out("{:<26} ink={:6}\n", item.label, ink);
    ++index;
  }

  // 判据：A≠B（父旋转确实带动子树）、B≠C（枢轴确实生效）、A≠D（隐藏继承生效）
  const bool parent_rotates = inks[0] != inks[1];
  const bool pivot_matters = inks[1] != inks[2];
  const bool visibility_inherits = inks[0] != inks[3];
  report += std::format("\n判据：父转带动子树={} 枢轴生效={} 可见性继承={}\n",
                        parent_rotates ? "ok" : "FAIL", pivot_matters ? "ok" : "FAIL",
                        visibility_inherits ? "ok" : "FAIL");
  st::log::out("\n{}", report);

  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(sheet_width);
  image.height = static_cast<std::uint32_t>(sheet_height);
  image.rgba = std::move(sheet);
  const std::string png_path = st::fs::join(out_dir, "composite-pivots.png");
  if (const auto written = st::codec::png_write_file(png_path, image); !written.has_value()) {
    st::log::err("写 PNG 失败：{}\n", written.error().message);
    return 1;
  }
  (void)st::fs::write_text(st::fs::join(out_dir, "README.txt"),
                           "组合模型空间体系对照（左上→右下：A/B/C/D）\n\n" + report);
  st::log::out("产物：{}\n", png_path);
  return (parent_rotates && pivot_matters && visibility_inherits) ? 0 : 2;
}
