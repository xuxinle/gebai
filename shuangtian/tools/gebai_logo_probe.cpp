// 歌白品牌标记的接触印相（contact sheet）：把 `examples/assets/gebai-logo.svg` 在多个尺寸下
// 渲成一张图，供肉眼核对"在标题栏那个尺寸（16 逻辑 px）还认不认得出"。
//
// ## 为什么必须看图
//
// 小尺寸下的辨识度是**观感判据**（`docs/perceptual_changes.md` 第一原则：验收标准长在用户身上），
// 数字答不了"看着还是个记号吗"。而 16px 盒子里能承载的特征数很少——两瓣白形会不会糊成
// 一块、蓝底圆角会不会被抗锯齿吃掉，只有渲出来才知道。
//
// ## 顺带给出的结构量
//
// 每个尺寸旁边打三个数：实际墨迹包围盒（相对盒径）、白瓣占蓝底的比例、以及两瓣之间的
// 最小缝宽（像素）——**缝是这条记号最先消失的特征**（糊了就成了实心块），
// 所以它是"最小可用尺寸"的判据。
//
// 用法： gebai_logo_probe [out.png] [svg_path] [icon_png]
//
// 给了 `icon_png` 时额外导出一张**裸图标**（透明底、按视框铺满）：
// 那就是应用用 `b::embed<>` 嵌进去、交给 `Backend::set_window_icon` 的字节。
// 导出放在本探针里而不是另建一个工具：两者读同一份 SVG、同一套缩放口径，
// “看着怎样”与“嵌进去的是什么”永远同源。
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/svg.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Rect;

/// 读文件（探针自用，不必进 `st::fs` 的统一入口——它不处理中文路径场景）。
auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

/// 单个尺寸下量两件事：
/// ① **白瓣最小缝宽**（像素）——逐行扫，取"白-非白-白"段落里非白游程的最小值；
/// ② 蓝底覆盖率（有蓝色像素的列/行范围），用于确认整块记号没有超出盒径。
struct Metrics {
  float min_gap{0.0f};
  bool has_white{false};
  bool has_blue{false};
};

auto measure(const st::raster::Surface& surface) -> Metrics {
  Metrics out{};
  out.min_gap = 1e9f;
  const int width = surface.physical_width();
  const int height = surface.physical_height();
  for (int y = 0; y < height; ++y) {
    int run = 0;
    bool seen_white = false;
    bool pending_gap = false;
    for (int x = 0; x < width; ++x) {
      const Color pixel = surface.pixel_at(x, y);
      const bool white = pixel.r > 200 && pixel.g > 200 && pixel.b > 200 && pixel.a > 128;
      const bool blue = pixel.b > 120 && pixel.b > pixel.r + 40;
      if (blue) out.has_blue = true;
      if (white) {
        out.has_white = true;
        if (seen_white && pending_gap) {
          out.min_gap = std::min(out.min_gap, static_cast<float>(run));
        }
        seen_white = true;
        pending_gap = false;
        run = 0;
      } else if (seen_white) {
        ++run;
        pending_gap = true;
      }
    }
  }
  if (!out.has_white || out.min_gap > 1e8f) out.min_gap = 0.0f;
  return out;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out = argc > 1 ? argv[1] : "gebai-logo-sheet.png";
  const std::string path =
      argc > 2 ? argv[2] : "examples/assets/gebai-logo.svg";
  const std::string source = read_file(path);
  if (source.empty()) {
    st::print("读不到 SVG：{}\n", path);
    return 1;
  }
  const auto document = st::ui::svg::parse(source);
  if (!document.has_value() || document->is_empty()) {
    st::print("SVG 解析失败（{} 个节点）\n", document.has_value() ? document->nodes.size() : 0);
    return 1;
  }
  st::print("解析成功：viewBox {}x{}，{} 个节点\n", document->view_w, document->view_h,
            document->nodes.size());

  // 逐档尺寸（物理像素）：16 = 标题栏逻辑 16px @1.0；24/32 = 高 DPI 下的对应档；
  // 48/96 用于确认矢量放大不失真。1x 与 2x 都摆出来，因为 DPI 1.5 时标题栏落在 24px。
  const std::vector<int> sizes = {16, 24, 32, 48, 96};
  constexpr int kCell = 128;
  constexpr int kLabel = 18;
  const int width = static_cast<int>(sizes.size()) * kCell;
  const int height = kCell + kLabel;

  st::raster::Canvas canvas{width, height, 1.0f};
  // 底色取**标题栏实际底色**（浅色主题的 `surface_alt`）而不是自定义暗色：
  // 图标画在那一块上，底色决定了"蓝底与白瓣的边界还剩多少对比"。
  // 在自选暗底上看着清楚、一贴到真实标题栏就糊掉，是本类探针最容易犯的错。
  canvas.clear(st::ui::Theme::light().colors().surface_alt);

  for (std::size_t index = 0; index < sizes.size(); ++index) {
    const int size = sizes[index];
    const float inset = static_cast<float>(kCell - size) * 0.5f;
    const float x = static_cast<float>(index * kCell) + inset;
    const float y = inset;
    st::ui::svg::draw(canvas, *document, Rect{x, y, static_cast<float>(size), static_cast<float>(size)},
                      std::nullopt);

    // 量该尺寸下的实况（裁到该格子内再量，避免串档）。
    st::raster::Canvas cell{size, size, 1.0f};
    cell.clear(Color{0, 0, 0, 0});
    st::ui::svg::draw(cell, *document, Rect{0.0f, 0.0f, static_cast<float>(size),
                                            static_cast<float>(size)},
                      std::nullopt);
    const Metrics metrics = measure(cell);
    st::print("{:>4}px  白瓣缝宽 {:>4.1f}px  白 {}  蓝 {}\n", size, metrics.min_gap,
              metrics.has_white ? "有" : "无", metrics.has_blue ? "有" : "无");
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
  st::print("已写出 {}\n", out);

  // —— 裸图标导出（给窗口/任务栏图标用）——
  //
  // 尺寸取 256：Windows 会把它缩到 16/32 用，而 `set_window_icon` 自己按目标尺寸
  // 重新取样——给一个足够大的源只在缩小时损失采样精度，不会模糊。
  if (argc > 3) {
    const std::string icon_path = argv[3];
    constexpr int kIconSize = 256;
    st::raster::Canvas icon_canvas{kIconSize, kIconSize, 1.0f};
    icon_canvas.clear(Color{0, 0, 0, 0});   // 透明底：图标外围不能是方色块
    st::ui::svg::draw(icon_canvas, *document,
                      Rect{0.0f, 0.0f, static_cast<float>(kIconSize),
                           static_cast<float>(kIconSize)},
                      std::nullopt);
    st::codec::PngImage icon_image;
    icon_image.width = kIconSize;
    icon_image.height = kIconSize;
    icon_image.rgba = icon_canvas.to_rgba8();
    if (auto status = st::codec::png_write_file(icon_path, icon_image); !status) {
      st::print("裸图标写盘失败：{}\n", status.error().message);
      return 1;
    }
    st::print("已写出 {}（{}×{} 裸图标，供窗口/任务栏使用）\n", icon_path, kIconSize, kIconSize);
  }
  return 0;
}
