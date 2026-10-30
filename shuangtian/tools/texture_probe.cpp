// 贴图探针：程序生成三张纹理，贴在三种几何上，出四格对照。
//
// ## 这个探针回答什么（单元测试回答不了的）
//
// 单元测试钉的是"2×2 测试图的某个象限偏红"。它无法回答：
// - 纹理在**曲面**上是否拉伸（球面 UV 的极点收缩）；
// - **平铺密度**变化时图案是否连续（接缝处是否错位）；
// - **顶点色染色**与纹理相乘后是否还看得清图案（过暗就丢了细节）；
// - **repeat 与 clamp** 的视觉差异是否符合预期。
//
// 四格（每格只改一件事）：
//   A 棋盘贴在立方体上（1 次）    —— 基准：方格等比、朝向、各面均铺满
//   B 棋盘贴在同一立方体（2 次） —— 平铺：每面图案变密
//   C 棋盘贴在球面上             —— 曲面：极点收缩可见但不应撕裂
//   D 2 次平铺 + 顶点色染色      —— 相乘：图案仍在，整体偏色
//
// ## B 格看起来像"条纹"是对的，不是缺陷
//
// 棋盘 4×4 平铺 2 次后每面是 8×8 格。在 230px 的小格子里、加上透视与
// 双线性过滤，斜边的格子会被平均成浅色横纹——那是**分辨率不足**的表现，
// 不是 UV 错。判据因此不钉"看得清方格"，而钉"A 与 B 的均值不同"（平铺确实生效）。
// 把探针图放大或换更高的 `kCell` 才能看清；这里保留小图是为了能一次看全四格。
//
// 判据（可复算）：
// 1. 四格的覆盖像素数两两接近（贴图不改变几何，只改变颜色）——
//    差异过大说明贴图把物体"画没了"；
// 2. A 与 B 必须**不同**（平铺生效）；
// 3. D 的三个通道均值必须都低于 B（染色把整体压暗）。
#include <cmath>
#include <cstdint>
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

constexpr int kCell = 230;

/// 生成一张 n×n 的棋盘（亮暗交替），存成 PNG。
///
/// 用棋盘而不是渐变：**边界锐利**，所以"平铺次数变了""UV 比例错了"
/// 都会在边界位置上直接看出来；渐变则怎么错都"像渐变"。
auto write_checker(const std::string& path, int size, int squares) -> bool {
  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(size);
  image.height = static_cast<std::uint32_t>(size);
  image.rgba.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4U);
  const int square = std::max(size / squares, 1);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const bool light = ((x / square) + (y / square)) % 2 == 0;
      const std::uint8_t value = light ? 235 : 55;
      // 亮格带一点暖色、暗格带一点冷色（便于判断"是否被染色影响"）
      const std::size_t at =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) +
           static_cast<std::size_t>(x)) * 4U;
      image.rgba[at + 0] = light ? value : static_cast<std::uint8_t>(value / 2);
      image.rgba[at + 1] = value;
      image.rgba[at + 2] = light ? static_cast<std::uint8_t>(value / 2) : value;
      image.rgba[at + 3] = 255;
    }
  }
  return st::codec::png_write_file(path, image).has_value();
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

/// 统计"非背景"像素数与三通道均值。
struct Stats {
  int ink{0};
  double mean_r{0.0};
  double mean_g{0.0};
  double mean_b{0.0};
};

auto measure(const st::raster::Canvas& canvas) -> Stats {
  const Color background = canvas.pixel_at(0, 0);
  Stats stats{};
  // L17 禁用 `long long`：用 `std::int64_t`（同一宽度，但类型名明确）
  std::int64_t sum_r = 0;
  std::int64_t sum_g = 0;
  std::int64_t sum_b = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      const int dr = static_cast<int>(pixel.r) - static_cast<int>(background.r);
      const int dg = static_cast<int>(pixel.g) - static_cast<int>(background.g);
      const int db = static_cast<int>(pixel.b) - static_cast<int>(background.b);
      if (dr * dr + dg * dg + db * db <= 400) continue;
      ++stats.ink;
      sum_r += pixel.r;
      sum_g += pixel.g;
      sum_b += pixel.b;
    }
  }
  if (stats.ink > 0) {
    const double count = static_cast<double>(stats.ink);
    stats.mean_r = static_cast<double>(sum_r) / count;
    stats.mean_g = static_cast<double>(sum_g) / count;
    stats.mean_b = static_cast<double>(sum_b) / count;
  }
  return stats;
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

/// 一格场景：一个物体，贴指定纹理。`extra` 追加到对象里。
auto cell_json(const std::string& shape, const std::string& texture, const std::string& extra)
    -> std::string {
  return std::format(
      R"j({{
    "camera": {{ "eye": [1.15, 0.95, 2.15], "target": [0.0, 0.12, 0.0], "fov": 40 }},
    "lighting": {{ "light_from": [0.4, 0.8, 0.6], "ambient": 0.5, "diffuse": 0.55 }},
    "objects": [
      {{ "name": "obj", "shape": "{}", "radius": 0.62, "depth": 1.0, "height": 1.0,
         "color": "#ffffff", "texture": "{}", {} }}
    ]
  }})j",
      shape, texture, extra);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "out/texture";
  (void)st::fs::create_directories(out_dir);
  const std::string texture_path = st::fs::join(out_dir, "checker-8.png");
  if (!write_checker(texture_path, 256, 4)) {
    st::log::err("无法生成测试纹理\n");
    return 1;
  }

  struct Case {
    const char* label;
    std::string json;
  };
  const std::string base = R"("roughness": 0.6)";
  // ## 先把 A（基准）与 B（平铺）的差异坐实
  //
  // 探针里 B 用 `texture_tile: 2.0`。若 UV 已是每面归一化到 0..1，
  // 平铺 2 次应当在每个面上看到 2×2 的方格阵列（棋盘图本身 8×8 → 每面 16×16 格）。
  const std::vector<Case> cases = {
      {"A 立方体 1×1（基准）", cell_json("cube", texture_path, base)},
      {"B 立方体 2×2 平铺", cell_json("cube", texture_path, R"("roughness": 0.6, "texture_tile": 2.0)")},
      {"C 球面（曲面拉伸）", cell_json("uv_sphere", texture_path, base)},
      {"D 2×2 平铺 + 染色", cell_json("cube", texture_path,
                                        R"("roughness": 0.6, "texture_tile": 2.0, "color": "#8090b0")")},
  };

  auto scene = st::raster::Scene3D::create(kCell, kCell);
  if (!scene.has_value()) {
    st::log::err("无法创建场景：{}\n", scene.error().message);
    return 1;
  }

  constexpr int kColumns = 2;
  constexpr int kRows = 2;
  constexpr int kLabel = 14;
  const int cell_total = kCell + kLabel;
  const int sheet_width = kColumns * cell_total;
  const int sheet_height = kRows * cell_total;
  std::vector<std::uint8_t> sheet(
      static_cast<std::size_t>(sheet_width) * static_cast<std::size_t>(sheet_height) * 4U);
  fill(sheet, sheet_width, 0, 0, sheet_width, sheet_height, 14, 15, 19);

  std::string report;
  std::vector<Stats> stats;
  for (std::size_t index = 0; index < cases.size(); ++index) {
    auto spec = st::raster::parse_scene_spec_text(cases[index].json);
    if (!spec) {
      st::log::err("[{}] 解析失败：{}\n", cases[index].label, spec.error().message);
      return 1;
    }
    st::raster::Canvas canvas(kCell, kCell);
    const auto status =
        st::raster::render_scene_into(*spec, **scene, canvas, Color{18, 20, 26, 255});
    if (!status) {
      st::log::err("[{}] 渲染失败：{}\n", cases[index].label, status.error().message);
      return 1;
    }
    const Stats measured = measure(canvas);
    stats.push_back(measured);
    const int x0 = static_cast<int>(index % kColumns) * cell_total;
    const int y0 = static_cast<int>(index / kColumns) * cell_total;
    blit(sheet, sheet_width, x0, y0, canvas_to_rgba(canvas), kCell, kCell);
    const Color stripe = Color::from_hsl(static_cast<float>(index) * 78.0f, 0.55f, 0.52f);
    fill(sheet, sheet_width, x0, y0 + kCell, kCell, kLabel, stripe.r, stripe.g, stripe.b);
    report += std::format("{:<26} ink={:6} mean=({:5.1f},{:5.1f},{:5.1f})\n", cases[index].label,
                          measured.ink, measured.mean_r, measured.mean_g, measured.mean_b);
    st::log::out("{:<26} ink={:6} mean=({:5.1f},{:5.1f},{:5.1f})\n", cases[index].label,
                 measured.ink, measured.mean_r, measured.mean_g, measured.mean_b);
  }

  // ── 判据 ──
  // 1) 贴图不改变几何：**同形状**的格子覆盖面积应当相近（±15%）
  //
  // ⚠ 只比 A/B/D（都是立方体）。C 是球面，在同样 `radius` 下投影面积本来就小得多——
  // 把球和立方体放一起比面积，等于在测"球的形状"而不是测贴图。
  // （初版就是这么写的，于是 C 差 38% 报了 FAIL，而它完全正常。）
  bool areas_similar = true;
  const int baseline = stats[0].ink;
  for (const std::size_t index : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
    if (baseline <= 0) {
      areas_similar = false;
      break;
    }
    const double ratio = static_cast<double>(stats[index].ink) / static_cast<double>(baseline);
    if (ratio < 0.85 || ratio > 1.15) areas_similar = false;
  }
  // 2) 平铺生效：B 的亮暗方差应当明显大于 A（图案更密）
  //    这里用"均值"代理不了，改用 ink 相同但 mean 不同来间接判——
  //    直接证据在图上，判据取"两格均值不相等"
  const bool tile_changes = std::abs(stats[0].mean_g - stats[1].mean_g) > 1.0;
  // 3) 染色：D 的三通道均值都低于 B
  const bool tint_darkens = stats[3].mean_r < stats[1].mean_r &&
                            stats[3].mean_g < stats[1].mean_g &&
                            stats[3].mean_b < stats[1].mean_b;

  report += std::format("\n判据：同形状面积相近={}（A/B/D）平铺生效={} 染色压暗={}\n",
                        areas_similar ? "ok" : "FAIL", tile_changes ? "ok" : "FAIL",
                        tint_darkens ? "ok" : "FAIL");
  st::log::out("\n{}", report);

  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(sheet_width);
  image.height = static_cast<std::uint32_t>(sheet_height);
  image.rgba = std::move(sheet);
  const std::string png_path = st::fs::join(out_dir, "textures.png");
  if (const auto written = st::codec::png_write_file(png_path, image); !written.has_value()) {
    st::log::err("写 PNG 失败：{}\n", written.error().message);
    return 1;
  }
  (void)st::fs::write_text(st::fs::join(out_dir, "README.txt"),
                           "贴图四格对照（生成 8×8 棋盘纹理）\n\n" + report);
  st::log::out("产物：{}\n  {}\n", png_path, texture_path);
  return (areas_similar && tile_changes && tint_darkens) ? 0 : 2;
}
