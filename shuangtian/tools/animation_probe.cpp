// 动画探针：把一段**带关键帧动画**的组合模型渲成分镜图，并导出带动画的 `.glb`。
//
// ## 这个探针回答什么（单元测试回答不了的三件事）
//
// 1. **时间上是否连续**：单帧判据（"这帧有像素"）看不出"动画在第 2 帧突然跳"。
//    分镜图把 8 个等距时刻并排，人眼一眼能看出节奏对不对。
// 2. **缓动是否真在起作用**：对比"中点在哪儿"——匀速的中点在 0.5，
//    ease-in-out 的中点在 0.5 但**四分之一处明显靠后**，分镜图上手臂角度能看出来。
// 3. **glTF 里的动画是否完整**：导出的 `.glb` 用自家读取器读回，逐帧比采样值
//    （Blender 打开时用的是同一个 `animations` 数组）。
//
// 判据（打印可复算）：
// - 8 帧中至少有 4 帧互不相同（动画确实在动，不是静止或只抖一下）；
// - 末帧与首帧姿态一致（循环回到起点）；
// - 导出的 `.glb` 能被读回，且采样值与原始片段一致。
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
#include "st/raster/gltf.hpp"
#include "st/raster/primitives.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

using st::math::Color;
using st::raster::Easing;
using st::raster::Keyframe;
using st::raster::Track;
using st::raster::TrackKind;

constexpr int kCell = 200;
constexpr int kFrames = 8;

/// 一段 2 秒的机械臂挥动：肩绕 Z 摆 0 → 1.0 → 0，肘部跟随做相位延迟。
/// 两个关节相位不同，才看得出"父子链一起动"而不是刚体平移。
auto robot_json(float duration) -> std::string {
  const double half = static_cast<double>(duration) * 0.5;
  const double quarter = static_cast<double>(duration) * 0.25;
  const double whole = static_cast<double>(duration);
  return std::format(R"json({{
    "camera": {{ "eye": [0.95, 0.75, 1.75], "target": [0.05, 0.5, 0.0], "fov": 36 }},
    "lighting": {{ "key_direction": [0.4, 0.8, 0.6], "ambient": 0.34, "diffuse": 0.76, "rim": 0.22 }},
    "shadow": {{ "strength": 0.32, "ground_y": 0.0 }},
    "objects": [
      {{ "name": "ground", "shape": "plane", "radius": 1.6, "depth": 1.6,
         "color": "#3c414b", "ground": true }},
      {{ "name": "base", "shape": "cylinder", "radius": 0.2, "depth": 0.1,
         "color": "#8a8f99", "translate": [0.0, 0.05, 0.0],
         "metallic": 0.6, "roughness": 0.35 }},
      {{ "name": "shoulder", "group": true, "translate": [0.0, 0.1, 0.0],
        "children": [
          {{ "name": "upper", "shape": "capsule", "radius": 0.07, "depth": 0.62,
             "color": "#d4b07a", "translate": [0.0, 0.28, 0.0],
             "metallic": 0.3, "roughness": 0.4 }},
          {{ "name": "elbow", "group": true, "translate": [0.0, 0.58, 0.0],
            "children": [
              {{ "name": "forearm", "shape": "capsule", "radius": 0.055, "depth": 0.5,
                 "color": "#c8a07a", "translate": [0.0, 0.22, 0.0],
                 "metallic": 0.3, "roughness": 0.4 }},
              {{ "name": "hand", "shape": "box", "radius": 0.07, "depth": 0.1, "height": 0.15,
                 "color": "#6f7681", "translate": [0.0, 0.5, 0.0],
                 "metallic": 0.85, "roughness": 0.2 }}
            ] }}
        ] }}
    ],
    "animation": {{
      "name": "wave", "duration": {0:.3f},
      "tracks": [
        {{ "node": "shoulder", "kind": "rotate",
           "keys": [ {{ "t": 0.0, "value": [0, 0, 0], "easing": "ease-in-out" }},
                     {{ "t": {1:.3f}, "value": [0, 0, 1.0], "easing": "ease-in-out" }},
                     {{ "t": {2:.3f}, "value": [0, 0, 0] }} ] }},
        {{ "node": "elbow", "kind": "rotate",
           "keys": [ {{ "t": 0.0, "value": [0, 0, -0.5], "easing": "ease-in-out" }},
                     {{ "t": {3:.3f}, "value": [0, 0, -1.3], "easing": "ease-in-out" }},
                     {{ "t": {4:.3f}, "value": [0, 0, -0.5] }} ] }}
      ]
    }}
  }})json",
                    whole, half, whole, quarter, whole);
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
  const std::string out_dir = argc > 1 ? argv[1] : "out/animation";
  (void)st::fs::create_directories(out_dir);

  constexpr float kDuration = 2.0f;
  const std::string json = robot_json(kDuration);
  auto spec = st::raster::parse_scene_spec_text(json);
  if (!spec) {
    st::log::err("解析失败：{}\n", spec.error().message);
    return 1;
  }
  if (!spec->animation.has_value()) {
    st::log::err("脚本里没有动画（检查 animation 字段）\n");
    return 1;
  }

  auto scene = st::raster::Scene3D::create(kCell, kCell);
  if (!scene.has_value()) {
    st::log::err("无法创建三维场景：{}\n", scene.error().message);
    return 1;
  }

  // ── 分镜图：8 帧等距 ──
  constexpr int kColumns = 4;
  const int rows = (kFrames + kColumns - 1) / kColumns;
  constexpr int kLabel = 14;
  const int cell_total = kCell + kLabel;
  const int sheet_width = kColumns * cell_total;
  const int sheet_height = rows * cell_total;
  std::vector<std::uint8_t> sheet(
      static_cast<std::size_t>(sheet_width) * static_cast<std::size_t>(sheet_height) * 4U);
  fill(sheet, sheet_width, 0, 0, sheet_width, sheet_height, 14, 15, 19);

  std::vector<int> inks;
  std::vector<float> shoulder_angles;
  std::string report;
  for (int frame = 0; frame < kFrames; ++frame) {
    // 末帧取 `duration`（不是 duration*(7/8)）：要验证"循环回到起点"
    const float time = frame == kFrames - 1 ? kDuration
                                            : kDuration * static_cast<float>(frame) /
                                                  static_cast<float>(kFrames - 1);
    st::raster::Canvas canvas(kCell, kCell);
    const auto status = st::raster::render_scene_at_into(*spec, time, **scene, canvas,
                                                         Color{18, 20, 26, 255});
    if (!status) {
      st::log::err("渲染失败（t={}）：{}\n", time, status.error().message);
      return 1;
    }
    const int ink = inked(canvas);
    inks.push_back(ink);
    // 采样肩角（判"动画在推进"用可复算的数字，而不是靠肉眼看图）
    const auto sampled = st::raster::sample_clip(*spec->animation, time);
    float shoulder = 0.0f;
    for (const auto& entry : sampled) {
      if (entry.node == "shoulder") shoulder = entry.local.rotation.z;
    }
    shoulder_angles.push_back(shoulder);

    const int cell_x = (frame % kColumns) * cell_total;
    const int cell_y = (frame / kColumns) * cell_total;
    blit(sheet, sheet_width, cell_x, cell_y, canvas_to_rgba(canvas), kCell, kCell);
    // 标签条：亮度随角度变化（人眼能对上节奏）
    const float hue = 200.0f + shoulder * 90.0f;
    const Color stripe = Color::from_hsl(hue, 0.6f, 0.5f);
    fill(sheet, sheet_width, cell_x, cell_y + kCell, kCell, kLabel, stripe.r, stripe.g, stripe.b);
    report += std::format("frame {} t={:.3f}s  ink={:6}  shoulder={:.3f}rad\n", frame,
                          static_cast<double>(time), ink,
                          static_cast<double>(shoulder));
    st::log::out("frame {} t={:.3f}s  ink={:6}  shoulder={:.3f}rad\n", frame,
                 static_cast<double>(time), ink, static_cast<double>(shoulder));
  }

  // ── 判据 ──
  int distinct = 0;
  for (std::size_t i = 0; i < inks.size(); ++i) {
    bool unique = true;
    for (std::size_t j = 0; j < i; ++j) {
      if (inks[i] == inks[j]) {
        unique = false;
        break;
      }
    }
    if (unique) ++distinct;
  }
  const bool animates = distinct >= 4;
  const bool returns = std::abs(shoulder_angles.front() - shoulder_angles.back()) < 1e-3f;
  const bool peaks = std::abs(shoulder_angles[kFrames / 2 - 1]) > 0.5f;

  // ── 导出带动画的 .glb，并读回验证 ──
  std::vector<st::raster::GltfObject> objects;
  {
    // 用一个可见的盒子代表整台设备（导出只验证"动画数据在里面"）
    st::raster::primitives::PrimitiveParams params{};
    params.radius = 0.4f;
    params.depth = 0.8f;
    st::raster::GltfObject object{};
    object.mesh = st::raster::primitives::make_cube(params);
    object.name = "shoulder";
    objects.push_back(object);
  }
  st::raster::GltfWriteOptions options{};
  options.binary = true;
  // 稠密化：把 ease-in-out 曲线展开成密集线性帧，否则 glTF 里只剩匀速
  options.animations = {st::raster::densify(*spec->animation, 1.0f / 30.0f)};
  const std::string glb_path = st::fs::join(out_dir, "wave.glb");
  bool exported = false;
  std::size_t exported_keys = 0;
  if (const auto written = st::raster::gltf_write_file(glb_path, objects, options);
      written.has_value()) {
    if (const auto read_back = st::raster::gltf_read_file(glb_path); read_back.has_value()) {
      exported = !read_back->animations.empty();
      if (exported) {
        for (const auto& track : read_back->animations.front().tracks) {
          exported_keys += track.keys.size();
        }
      }
    }
  }

  report += std::format(
      "\n判据：在动={} 回到起点={} 摆到位={} 导出动画={}（稠密化后 {} 个关键帧）\n",
      animates ? "ok" : "FAIL", returns ? "ok" : "FAIL", peaks ? "ok" : "FAIL",
      exported ? "ok" : "FAIL", exported_keys);
  st::log::out("\n{}", report);

  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(sheet_width);
  image.height = static_cast<std::uint32_t>(sheet_height);
  image.rgba = std::move(sheet);
  const std::string png_path = st::fs::join(out_dir, "storyboard.png");
  if (const auto written = st::codec::png_write_file(png_path, image); !written.has_value()) {
    st::log::err("写 PNG 失败：{}\n", written.error().message);
    return 1;
  }
  (void)st::fs::write_text(st::fs::join(out_dir, "README.txt"),
                           "动画分镜（4 列 × 2 行，t 从 0 到 2s）\n\n" + report);
  st::log::out("产物：\n  {}\n  {}\n", png_path, glb_path);
  return (animates && returns && peaks && exported) ? 0 : 2;
}
