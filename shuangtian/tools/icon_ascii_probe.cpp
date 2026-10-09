// 把图标渲染成**逐像素 ASCII**，核验形态（这个渲染器下的既定排查手段）。
//
// 用法： icon_ascii_probe <name> [box_px] [mode: stroke|filled|auto]
//
// 为何需要：几何量（ink_w/ink_h）只说"多大"，说不出"线—圆—线"的节奏对不对。
// 小尺寸下抗锯齿会把细节抹平，必须看像素。

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/path.hpp"
#include "st/ui/icon.hpp"

namespace {

using st::math::Color;
using st::math::Rect;

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string name = argc > 1 ? argv[1] : "diff";
  const int size = argc > 2 ? std::atoi(argv[2]) : 16;
  const std::string mode = argc > 3 ? argv[3] : "auto";

  const Color background = Color::rgb(0xFF, 0xFF, 0xFF);
  const Color ink = Color::rgb(0x00, 0x00, 0x00);
  const Rect box{0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size)};

  if (mode == "auto") {
    st::raster::Canvas canvas{size, size, 1.0f};
    canvas.clear(background);
    st::ui::Icon::draw(canvas, name, box, ink, 0.0f);
    st::print("Icon::draw（自动选路径类型）· {} · {}px\n", name, size);
    for (int y = 0; y < size; ++y) {
      std::string line;
      for (int x = 0; x < size; ++x) {
        const int r = static_cast<int>(canvas.pixel_at(x, y).r);
        // 覆盖率 = 1 - r/255
        const int cov = 255 - r;
        line += cov > 200 ? '#' : (cov > 120 ? '+' : (cov > 40 ? '.' : ' '));
      }
      st::print("|{}|\n", line);
    }
  } else {
    const st::raster::Path path = st::ui::Icon::path(name, box, 0.0f);
    st::raster::Canvas canvas{size, size, 1.0f};
    canvas.clear(background);
    if (mode == "filled") {
      st::ui::Icon::draw_filled(canvas, name, box, ink);
      st::print("draw_filled · {} · {}px\n", name, size);
    } else {
      st::print("stroke_path（未走 Icon::draw）\n");
    }
    for (int y = 0; y < size; ++y) {
      std::string line;
      for (int x = 0; x < size; ++x) {
        const int cov = 255 - static_cast<int>(canvas.pixel_at(x, y).r);
        line += cov > 200 ? '#' : (cov > 120 ? '+' : (cov > 40 ? '.' : ' '));
      }
      st::print("|{}|\n", line);
    }
    (void)path;
  }
  return 0;
}
