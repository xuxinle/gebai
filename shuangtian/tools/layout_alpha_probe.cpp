// 量「layout 画出的图元要让 32×18 网格逐帧差 ≥1.2，需要多大 alpha / 多大尺寸」
//
// ## 为什么要专门量
//
// 我连续几轮都在调 alpha（0.05 → 0.10 → 0.30 → 0.55），静止占比从 88.4% 到 87.0%，
// 每次改动都"几乎没变"。缺的是一个**定量关系**：给定底色与点色，
// 点要多大多亮，才能让 60×60 的一格均值变化 1.2？
//
// 本探针用框的实际公式算（不是估）：
//   格均值变化 = (点面积 / 格面积) × |点亮度 - 底亮度| × alpha
// 反解出「达到 1.2 所需的最小 (面积 × alpha)」，再对照实际渲染验证。
#include <cstdio>   // lint-allow: L7 探针输出（量测工具，不产出交付物）
#include <cmath>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/surface.hpp"

namespace {

constexpr int W = 1920, H = 1080;
constexpr int COLS = 32, ROWS = 18;
constexpr int CELL_W = 64, CELL_H = 60;   // 实际是 1920/32=60, 1080/18=60

// 亮度（与 ot_bench 同口径：0.299R + 0.587G + 0.114B）
double luma(double r, double g, double b) { return 0.299 * r + 0.587 * g + 0.114 * b; }

// 把画布降采样成 COLS×ROWS 的灰度网格（与量尺同口径）
std::vector<double> grid_of(const st::raster::Canvas& c) {
  const auto rgba = c.to_rgba8();
  const int w = c.physical_width(), h = c.physical_height();
  std::vector<double> out(static_cast<std::size_t>(COLS) * ROWS, 0.0);
  for (int gy = 0; gy < ROWS; ++gy) {
    for (int gx = 0; gx < COLS; ++gx) {
      const int x0 = gx * w / COLS, x1 = std::max(x0 + 1, (gx + 1) * w / COLS);
      const int y0 = gy * h / ROWS, y1 = std::max(y0 + 1, (gy + 1) * h / ROWS);
      double sum = 0.0;
      std::int64_t count = 0;
      for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
          const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4U;
          // to_rgba8 是 R,G,B,A 字节序（探针实证：清 0x123456 得 (18,52,86,255)）
          sum += luma(rgba[i], rgba[i + 1U], rgba[i + 2U]);
          ++count;
        }
      }
      out[static_cast<std::size_t>(gy) * COLS + gx] = sum / static_cast<double>(count);
    }
  }
  return out;
}

}  // namespace

int main() {
  // 底色 #0B0E14（歌白片的深底）
  const st::math::Color BG{0x0B, 0x0E, 0x14, 255};
  // 点色 #8B9AFF（片子的 accent）
  const st::math::Color DOT{0x8B, 0x9A, 0xFF, 255};

  const double bg_l = luma(BG.r, BG.g, BG.b);
  const double dot_l = luma(DOT.r, DOT.g, DOT.b);
  std::printf("底色 luma=%.1f  点色 luma=%.1f  差=%.1f\n\n", bg_l, dot_l, dot_l - bg_l);
  std::printf("网格格尺寸 %dx%d = %d px²\n\n", W / COLS, H / ROWS, (W / COLS) * (H / ROWS));

  std::printf("%-10s %-10s %-12s %-12s %s\n", "点数", "点直径", "alpha", "理论Δ", "实测Δ");
  std::printf("%s\n", "------------------------------------------------------------------");

  // 每格放 1 个点（密度与真片相同：间距 52~64px）
  const int NX = COLS, NY = ROWS;

  for (const int dia : {8, 13, 20, 34}) {
    for (const float alpha : {0.10f, 0.25f, 0.45f, 0.70f}) {
      st::raster::Canvas canvas{W, H, 1.0f};
      canvas.clear(BG);

      // 帧 A：无点（纯底）
      const auto grid_a = grid_of(canvas);

      // 帧 B：每格中心画一个点
      for (int gy = 0; gy < NY; ++gy) {
        for (int gx = 0; gx < NX; ++gx) {
          const float cx = (static_cast<float>(gx) + 0.5f) *
                           (static_cast<float>(W) / static_cast<float>(COLS));
          const float cy = (static_cast<float>(gy) + 0.5f) *
                           (static_cast<float>(H) / static_cast<float>(ROWS));
          st::math::Color c = DOT;
          c.a = static_cast<std::uint8_t>(alpha * 255.0f + 0.5f);
          canvas.fill_circle(st::math::Point{cx, cy}, static_cast<float>(dia) * 0.5f,
                             st::raster::Paint::solid(c));
        }
      }
      const auto grid_b = grid_of(canvas);

      // 逐格差的均值（与 ot_bench 的 change 同口径）
      double total = 0.0;
      for (std::size_t i = 0; i < grid_a.size(); ++i) {
        total += std::abs(grid_b[i] - grid_a[i]);
      }
      const double measured = total / static_cast<double>(grid_a.size());

      // 理论：覆盖率 × 亮度差 × alpha
      const double cell_area = (W / COLS) * (H / ROWS);
      const double dot_area = 3.14159265 * (dia * 0.5) * (dia * 0.5);
      const double cover = dot_area / cell_area;
      const double theory = cover * (dot_l - bg_l) * static_cast<double>(alpha);

      std::printf("%-10d %-10d %-12.2f %-12.2f %.2f%s\n", NX * NY, dia,
                  static_cast<double>(alpha), theory, measured,
                  measured >= 1.2 ? "  ← 够" : "");
    }
  }

  std::printf("\n结论：需要 **实测Δ ≥ 1.2** 才能让「静止占比」真正下降。\n");
  return 0;
}
