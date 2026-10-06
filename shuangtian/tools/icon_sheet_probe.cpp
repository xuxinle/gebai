// 图标接触印相（contact sheet）：把全部内置图标渲成一张大图，供肉眼评估“美不美”。
//
// ## 为什么必须是图而不是数字
// 图标美化属观感类改动，验收标准长在用户身上（`docs/perceptual_changes.md` 第一原则）。
// 数字只能答“变没变/差多少”，答不了“好不好看”——所以先把全部图标摆出来看。
//
// ## 两个正交量（同一张图上同时可读）
// ① **视觉尺寸一致性**：各图标墨迹占视图框的比例——忽大忽小是“不精致”的首要来源；
// ② **笔画粗细一致性**：标题/路径同档的图标应当看起来一样重。
// 两者都在图旁边给出数字（口径 = 墨迹包围盒相对 24×24 视图框）。
//
// 用法： icon_sheet_probe [out.png] [cell_px] [theme]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Rect;

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out = argc > 1 ? argv[1] : "icon-sheet.png";
  const int cell = argc > 2 ? std::atoi(argv[2]) : 72;
  const std::string theme_name = argc > 3 ? argv[3] : "dark";

  const auto names = st::ui::Icon::names();
  if (names.empty()) {
    st::print("图标表为空\n");
    return 1;
  }
  const auto theme = theme_name == "light" ? st::ui::Theme::light() : st::ui::Theme::dark();
  const Color background = theme.colors().surface;
  const Color ink = theme.colors().text;

  constexpr int kColumns = 10;
  const int rows = static_cast<int>((names.size() + kColumns - 1) / kColumns);
  constexpr int kLabel = 14;   // 每格底部的名字区（本探针只画图标，名字另打）
  const int width = kColumns * cell;
  const int height = rows * (cell + kLabel);

  st::raster::Canvas canvas{width, height, 1.0f};
  canvas.clear(background);

  st::print("图标数 {} → {}×{} 格（{:.0f}px/格）主题 {}\n", names.size(), kColumns, rows,
            static_cast<double>(cell), theme_name);
  st::print("\n{:<22} {:>8} {:>8} {:>8} {:>8}\n", "name", "w/24", "h/24", "cx%", "cy%");
  for (std::size_t index = 0; index < names.size(); ++index) {
    const auto column = static_cast<int>(index % kColumns);
    const auto row = static_cast<int>(index / kColumns);
    const float x = static_cast<float>(column * cell);
    const float y = static_cast<float>(row * (cell + kLabel));
    const float inset = static_cast<float>(cell) * 0.10f;
    const Rect box{x + inset, y + inset, static_cast<float>(cell) - inset * 2.0f,
                   static_cast<float>(cell) - inset * 2.0f};
    st::ui::Icon::draw(canvas, names[index], box, ink, 2.0f);

    // 量该图标在 24×24 视图框内的墨迹口——用 `view_bounds`（布局对齐用的同一份几何）。
    const Rect view = st::ui::Icon::view_bounds(names[index]);
    st::print("{:<22} {:>8.2f} {:>8.2f} {:>8.2f} {:>8.2f}\n", names[index],
              static_cast<double>(view.width / 24.0f), static_cast<double>(view.height / 24.0f),
              static_cast<double>(view.center().x / 24.0f * 100.0f),
              static_cast<double>(view.center().y / 24.0f * 100.0f));
  }

  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba = canvas.to_rgba8();
  const auto written = st::codec::png_write_file(out, image);
  if (!written) {
    st::print("写盘失败：{}\n", written.error().message);
    return 1;
  }

  // 同时写一份**格子索引**（`名字` → 格位与格宽）。
  //
  // 为何必要：评估时要在 Python 里按网格裁格量每格墨迹——而名字顺序一旦靠人手抄，
  // 就会错位（实测踩到：手抄的清单与真实顺序错位，`sparkles`/`cpu` 被量成“空的”，
  // 于是归因指向了别的图标）。这份索引让名字与格位**只有一个来源**。
  {
    const std::string index_path = out + ".json";
    std::string json = std::format("{{\"cell\":{},\"label\":{},\"columns\":{},\"cells\":[", cell,
                                   kLabel, kColumns);
    for (std::size_t index = 0; index < names.size(); ++index) {
      if (index > 0) json += ',';
      json += std::format("{{\"name\":\"{}\",\"col\":{},\"row\":{}}}", names[index],
                          index % static_cast<std::size_t>(kColumns),
                          index / static_cast<std::size_t>(kColumns));
    }
    json += "]}";
    if (auto file = std::fopen(index_path.c_str(), "wb"); file != nullptr) {
      (void)std::fwrite(json.data(), 1, json.size(), file);
      (void)std::fclose(file);
      st::print("已写出 {}（格位索引）\n", index_path);
    } else {
      st::print("写索引失败：{}\n", index_path);
    }
  }
  st::print("已写出 {}\n", out);
  return 0;
}
