/// **真实 SVG 矢量图形**的 A/B 探针：霜天侧（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 与 `text_ab_*`（文字页）的区别：本条线**没有字体参与**——量到的是**矢量光栅化**
/// （路径填充 / 圆角 / 变换 / 描边 / 填充规则）本身，差异能被干净归因。
///
/// 每个 case 与 `svg_ab_page.html` 里**同一个 `<svg>` 源码**逐字相同
/// （源码由 `tools/gen_svg_ab.py` 写进 `svg_ab_cases.json`，两侧同源）。
///
/// 用法：svg_ab_probe <outdir> [scale] [--dark]
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "text_ab_common.hpp"
#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/svg.hpp"

/// cases 目录由构建脚本注入（`-DSVG_AB_CASES_DIR="…/tools"`），避免探针里硬编码路径。
#ifndef SVG_AB_CASES_DIR
#define SVG_AB_CASES_DIR "."
#endif

namespace {

/// 极简 JSON 取值（只取本探针需要的字段，不引 json 依赖到 tools）。
auto json_strings(std::string_view text, std::string_view key) -> std::vector<std::string> {
  std::vector<std::string> out;
  const std::string needle = "\"" + std::string(key) + "\": \"";
  std::size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string_view::npos) {
    pos += needle.size();
    std::string value;
    while (pos < text.size() && text[pos] != '"') {
      if (text[pos] == '\\' && pos + 1 < text.size()) {
        const char esc = text[pos + 1];
        value += (esc == 'n') ? '\n' : ((esc == 't') ? '\t' : esc);
        pos += 2;
        continue;
      }
      value += text[pos++];
    }
    out.push_back(std::move(value));
  }
  return out;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const float scale = argc > 2 ? std::stof(argv[2]) : 1.5f;
  bool dark = false;
  for (int i = 3; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--dark") dark = true;
  }
  const std::string cases_path =
      std::string(SVG_AB_CASES_DIR) + (dark ? "/svg_ab_cases_dark.json" : "/svg_ab_cases.json");
  std::ifstream input(cases_path, std::ios::binary);
  if (!input) {
    st::eprint("读不到 {}（先跑 tools/gen_svg_ab.py）\n", cases_path);
    return 1;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string text = buffer.str();

  std::vector<std::string> names = json_strings(text, "name");
  std::vector<std::string> sources = json_strings(text, "source");
  if (names.empty() || names.size() != sources.size()) {
    st::eprint("cases 解析失败：{} 个 name，{} 个 source\n", names.size(), sources.size());
    return 1;
  }
  // 与生成器**同一套网格布局**（见 `tools/gen_svg_ab.py`）：每格 `kCell`×`kCell`，
  // 图形居中放置。两侧同格 ⇒ 逐格比较时坐标天然对齐，差异才可归因。
  constexpr int kCols = 6;
  constexpr int kCell = 96;
  constexpr int kGlyph = 64;
  // ⚠ **坐标一律逻辑像素**：`Canvas{width, height, scale}` 内部已带缩放，
  // 绘制坐标会被它乘 `scale`。这里若用物理像素定位，就会**再乘一次 1.5**——
  // 症状是每行只放得下 4 个（应 6 个）、图形大 1.5 倍（实测踩到，误以为是 SVG 渲染缺陷）。
  const int rows = (static_cast<int>(names.size()) + kCols - 1) / kCols;
  const int width = static_cast<int>(kCols * kCell * scale);
  const int height = static_cast<int>(rows * kCell * scale);
  st::raster::Canvas canvas{width, height, scale};
  canvas.clear(dark ? st::math::Color{0x0F, 0x11, 0x15, 0xFF}
                    : st::math::Color{0xFF, 0xFF, 0xFF, 0xFF});

  int drawn = 0;
  for (std::size_t i = 0; i < names.size(); ++i) {
    const auto doc = st::ui::svg::parse(sources[i]);
    if (!doc) {
      st::print("  [{}] 解析失败——跳过\n", names[i]);
      continue;
    }
    const int index = static_cast<int>(i);
    const float off = static_cast<float>((kCell - kGlyph) / 2);
    const float left = static_cast<float>(index % kCols) * kCell + off;
    const float top = static_cast<float>(index / kCols) * kCell + off;
    st::ui::svg::draw(canvas, *doc,
                      st::math::Rect{left, top, static_cast<float>(kGlyph),
                                     static_cast<float>(kGlyph)});
    ++drawn;
  }
  const std::string path = out_dir + (dark ? "/svg-ab-dark-st.png" : "/svg-ab-st.png");
  // 编码共用 `text_ab_common.hpp` 的 `save`（含尺寸一致性检查；PNG 编码不另写一份）。
  ab::save(canvas.to_rgba8(), width, height, path);
  st::print("已写 {}（{} case，{}x{} 物理像素，scale={}，dark={}）\n", path, drawn, width, height,
            scale, dark);
  return 0;
}
