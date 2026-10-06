/// **图标集总览**探针：把 sprite 里的每个图标按多个尺寸渲染成一张网格图（仅验证用）。
///
/// 用途：一眼看全框架自带的图标集，以及"同一图标在不同尺寸下是否都清晰"
/// （矢量重栅的收益只有在多尺寸下才看得出来；位图放大是模糊的）。
///
/// 每个图标渲染两列：**霜天 svg::draw** 与（可选）浏览器对照由外部脚本拼。
/// 尺寸阶梯取图标常见的 12/16/20/24/32/48 物理像素。
///
/// 用法：icon_gallery_probe <icons.svg> <outdir> [scale]
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/svg.hpp"
#include "text_ab_common.hpp"

namespace {

constexpr int kSizes[] = {12, 16, 20, 24, 32, 48};
constexpr int kColPitch = 56;   ///< 每列的横向间距（最大尺寸 48 + 留白）
constexpr int kRowPitch = 40;   ///< 每行的纵向间距（在逻辑像素上按最大尺寸排）

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string icons_path = argc > 1 ? argv[1] : "examples/gallery/assets/icons.svg";
  const std::string out_dir = argc > 2 ? argv[2] : ".";
  const float scale = argc > 3 ? std::stof(argv[3]) : 2.0f;

  std::ifstream input(icons_path, std::ios::binary);
  if (!input) {
    st::eprint("读不到 {}\n", icons_path);
    return 1;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string sprite = buffer.str();

  st::ui::svg::IconSet set;
  if (!set.load(sprite)) {
    st::eprint("图标集解析失败（无 <symbol>）\n");
    return 1;
  }
  const auto ids = set.ids();
  st::print("图标 {} 个：\n", ids.size());

  const int width_logical = static_cast<int>(ids.size()) * kColPitch + 8;
  const int height_logical = static_cast<int>(std::size(kSizes)) * kRowPitch + 8;
  st::raster::Canvas canvas{static_cast<int>(width_logical * scale),
                            static_cast<int>(height_logical * scale), scale};
  canvas.clear(st::math::Color{0x1E, 0x22, 0x2A, 0xFF});

  const st::math::Color ink{0xE6, 0xEA, 0xF2, 0xFF};
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto doc = set.get(ids[i]);
    if (!doc) {
      st::print("  [{}] 取不到\n", ids[i]);
      continue;
    }
    for (std::size_t row = 0; row < std::size(kSizes); ++row) {
      const float size = static_cast<float>(kSizes[row]);
      // 逻辑坐标：每列一个图标、每行一个尺寸，居中放。
      const float left = static_cast<float>(i) * kColPitch + (kColPitch - size) * 0.5f;
      const float top = static_cast<float>(row) * kRowPitch + (kRowPitch - size) * 0.5f;
      st::ui::svg::draw(canvas, *doc,
                        st::math::Rect{left, top, size, size}, ink);
    }
  }
  const std::string path = out_dir + "/icon-gallery-st.png";
  ab::save(canvas.to_rgba8(), canvas.physical_width(), canvas.physical_height(), path);
  st::print("已写 {}（{} 图标 × {} 尺寸，画布 {}x{} 物理像素）\n", path, ids.size(),
            std::size(kSizes), canvas.physical_width(), canvas.physical_height());
  for (const auto& id : ids) st::print("  {}", id);
  st::print("\n");
  return 0;
}
