/// 字形位图**相位**量尺（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 要回答的问题：字形位图到底有没有锚定在**物理像素网格**上？
///
/// `TextRenderer` 在**超采样空间**里栅格化（本机 1.5× DPI → `supersample = 2`），
/// 位图原点由 `floor(bounds) - padding` 给出（采样单位，1 单位 = 1/2 物理像素）。
/// 若位图在物理像素空间里的落点是 `min / supersample` 的**整数截断**，
/// 那么 `min` 为奇数时位图会整体偏移 **半个物理像素**——所有笔画都落在像素缝里，
/// 这正是「小字发糊」的典型成因，且**与抗锯齿模式、网格拟合都无关**。
///
/// 判据：同一组字形、连续字号档扫描。若位图已锚定网格，笔画的
/// 「峰值覆盖率 / 中间调占比」随字号应当**平滑变化**；
/// 若存在半像素相位，则会出现明显的**奇偶交替**（相邻字号一锐一糊）。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/text/text.hpp"

using st::text::FontRole;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

namespace {

struct Row {
  float size{0.0f};
  float peak{0.0f};
  int solid{0};
  int mid{0};
  double mid_ratio{0.0};
  double ink{0.0};
  int offset_x{0};
  int offset_y{0};
};

[[nodiscard]] auto scan(const TextRenderer& renderer, char32_t codepoint, float size) -> Row {
  Row row;
  row.size = size;
  const auto bitmap = renderer.glyph_bitmap_of(codepoint, size, FontRole::Proportional);
  if (bitmap == nullptr) return row;
  const int channels = bitmap->format == st::raster::CoverageFormat::Lcd ? 3 : 1;
  const std::size_t pixels =
      static_cast<std::size_t>(bitmap->width) * static_cast<std::size_t>(bitmap->height);
  for (std::size_t index = 0; index < pixels; ++index) {
    double value = 0.0;
    for (int channel = 0; channel < channels; ++channel) {
      value += static_cast<double>(
          bitmap->coverage[index * static_cast<std::size_t>(channels) +
                           static_cast<std::size_t>(channel)]);
    }
    value /= static_cast<double>(channels);
    row.peak = std::max(row.peak, static_cast<float>(value));
    row.ink += value;
    if (value > 0.85) ++row.solid;
    else if (value > 0.15) ++row.mid;
  }
  row.mid_ratio =
      row.solid > 0 ? static_cast<double>(row.mid) / static_cast<double>(row.solid) : -1.0;
  row.offset_x = bitmap->offset_x;
  row.offset_y = bitmap->offset_y;
  return row;
}

/// 相邻字号之间的「相位抖动」：越接近 0 说明随字号平滑，越大说明奇偶交替。
[[nodiscard]] auto roughness(const std::vector<Row>& rows) -> double {
  double total = 0.0;
  int count = 0;
  for (std::size_t index = 1; index < rows.size(); ++index) {
    if (rows[index - 1].solid == 0 || rows[index].solid == 0) continue;
    total += std::abs(rows[index].mid_ratio - rows[index - 1].mid_ratio);
    ++count;
  }
  return count > 0 ? total / static_cast<double>(count) : 0.0;
}

}  // namespace

auto main() -> int {
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  const FontStack& fonts = *stack;
  for (const float device_scale : {1.0f, 1.25f, 1.5f}) {
    TextRenderer renderer(fonts, device_scale);
    renderer.set_subpixel(false);
    renderer.set_grid_fit(GridFitMode::Normal);
    st::print("\n=== device_scale={} supersample={} ===\n", device_scale,
              static_cast<int>(std::lround(device_scale)));
    // 连续字号档：物理尺寸每次 +0.5（= 1/2 物理像素级的相位变化）
    for (const char32_t codepoint : {U'霜', U'H', U'三'}) {
      std::vector<Row> rows;
      for (int step = 0; step <= 14; ++step) {
        const float physical = 16.0f + static_cast<float>(step) * 0.5f;
        rows.push_back(scan(renderer, codepoint, physical));
      }
      st::print("  U+{:04X}  相位抖动={:.4f}\n", static_cast<unsigned>(codepoint),
                roughness(rows));
      st::print("    px      solid   mid   mid/solid   ink   off(x,y)\n");
      for (const Row& row : rows) {
        st::print("    {:5.1f}   {:6d} {:5d}   {:8.3f} {:7.1f}   ({:3d},{:3d})\n", row.size,
                  row.solid, row.mid, row.mid_ratio, row.ink, row.offset_x, row.offset_y);
      }
    }
  }
  return 0;
}
