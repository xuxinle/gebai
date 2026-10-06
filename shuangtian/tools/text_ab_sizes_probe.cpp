/// 文字渲染 A/B：**字号阶梯页**霜天侧（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 与 `tools/text_ab_sizes_page.html` 同源同布局：同一串文字逐档字号，
/// 用来回答「霜天与浏览器的差异是否随字号变化」（小字号与大字号本来该用不同修法）。
///
/// 说明：本文件取代旧的 `text_ab_sizes_probe.cpp`（那份依赖
/// `text_ab_sizes_rows.inc` 的单一 `kSample` + 独立生成的 HTML，两侧字号表已经漂移；
/// 现在行表与 HTML 由同一个生成器产出，探针只按行表渲染）。
///
/// 用法：text_ab_sizes_probe <outdir> [scale] [gamma] [--gray] [--nofit] [--nofilter] [--nocomp]
#include "text_ab_common.hpp"

#include <string>

#include "text_ab_sizes_rows.inc"

using ab::Point;
using ab::TextRenderer;

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const float scale = argc > 2 ? std::stof(argv[2]) : 1.5f;
  ab::Options options;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    // 第 3 个位置参数可以是 gamma（纯数字），也可以是开关——**必须先判开关**，
    // 否则 `std::stof("--nofit")` 直接抛异常（实测：探针 SIGABRT，看起来像渲染崩溃）。
    if (a == "--gray") options.subpixel = false;
    else if (a.rfind("--digit-gamma=", 0) == 0) options.digit_gamma = std::stof(a.substr(std::string("--digit-gamma=").size()));
    else if (a.rfind("--letter-gamma=", 0) == 0) options.letter_gamma = std::stof(a.substr(std::string("--letter-gamma=").size()));
    else if (a.rfind("--han-gamma=", 0) == 0) options.han_gamma = std::stof(a.substr(std::string("--han-gamma=").size()));
    else if (a == "--darken") options.darken = true;
    else if (a == "--fit-normal") options.fit_normal = true;
    else if (a == "--darken") options.darken = true;
    else if (a == "--nofit") options.fit = false;
    else if (a == "--nofilter") options.filter = false;
    else if (a == "--nocomp") options.compensate = false;
    else options.gamma = std::stof(a);
  }
  auto stack = st::text::FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  ab::print_font_stack(*stack);
  st::print("device_scale={}  画布={}x{} 逻辑  {} 行\n", scale, kWidth, kHeight, kLines.size());

  const int width = static_cast<int>(kWidth * scale);
  const int height = static_cast<int>(kHeight * scale);
  st::raster::Canvas canvas{width, height, scale};
  canvas.clear(ab::kBackground);
  TextRenderer renderer{*stack, scale};
  ab::apply(renderer, options, scale);
  for (const Line& line : kLines) {
    if (line.text.empty()) continue;
    (void)renderer.draw(canvas, line.text, Point{4.0f, line.top}, line.size, line.color, line.role,
                        0.0f, line.bold);
  }
  const std::string path = out_dir + "/ab-sizes-st.png";
  ab::save(canvas.to_rgba8(), width, height, path);
  st::print("  已写 {}（{}x{} 物理像素）\n", path, width, height);
  return 0;
}
