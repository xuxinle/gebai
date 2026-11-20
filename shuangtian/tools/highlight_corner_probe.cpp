/// 边缘高光四形态 + 左上圆角放大图：用来肉眼核对“圆角处是否连续渐变”。
///
/// 为何要出图：属性面（`highlight_edges` 枚举值）读回来全对，而“圆角上有道
/// 亮度阶跃”是**纯形态**缺陷——只有像素能看出来。本探针把左上角裁出来放大，
/// 让阶跃（如果有）一眼可见。
///
/// 用法：
///     st build highlight_corner_probe --profile dev
///     .\build\dev\bin\highlight_corner_probe.exe --out qa/
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::HighlightEdges;
using st::ui::Panel;
using st::ui::UiRoot;

constexpr int kW = 360;
constexpr int kH = 200;
constexpr float kX = 40.0f;
constexpr float kY = 40.0f;
constexpr float kBoxW = 280.0f;
constexpr float kBoxH = 120.0f;
constexpr float kRadius = 18.0f;
constexpr float kRing = 2.0f;

auto render(HighlightEdges edges) -> std::unique_ptr<Canvas> {
  UiRoot root;
  root.set_viewport({static_cast<float>(kW), static_cast<float>(kH)});
  root.set_theme(st::ui::Theme::light());

  auto card = std::make_unique<Panel>();
  auto* raw = card.get();
  raw->set_id("probe-card");
  root.set_content(std::move(card));

  raw->style().set_solid_background(Color{120, 120, 120, 255});
  raw->style().radius = kRadius;
  raw->style().shadow = {};
  raw->style().border_width = 0.0f;
  raw->style().top_highlight = Color{255, 255, 255, 235};
  raw->style().highlight_width = kRing;
  raw->style().highlight_edges = edges;

  st::ui::RenderContext context{root.theme(), nullptr, 1.0f};
  const Rect box{kX, kY, kBoxW, kBoxH};
  raw->measure(context, st::ui::Constraints{});
  raw->arrange(context, box);

  auto canvas = std::make_unique<Canvas>(kW, kH, 1.0f);
  canvas->clear(Color{120, 120, 120, 255});
  raw->paint(context, *canvas);
  return canvas;
}

/// 把左上角裁出并放大 `zoom` 倍（最近邻，保留像素级细节）。
auto zoom_corner(const Canvas& src, int zoom) -> std::unique_ptr<Canvas> {
  const int x0 = static_cast<int>(kX) - 2;
  const int y0 = static_cast<int>(kY) - 2;
  const int cw = static_cast<int>(kRadius) + 10;
  const int ch = static_cast<int>(kRadius) + 10;

  auto out = std::make_unique<Canvas>(cw * zoom, ch * zoom, 1.0f);
  out->clear(Color{0, 0, 0, 255});
  for (int y = 0; y < ch * zoom; ++y) {
    for (int x = 0; x < cw * zoom; ++x) {
      out->set_pixel(x, y, src.pixel_at(x0 + x / zoom, y0 + y / zoom));
    }
  }
  return out;
}

auto write_png(const Canvas& canvas, const std::string& path) -> bool {
  const int w = canvas.width();
  const int h = canvas.height();
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(w);
  image.height = static_cast<std::uint32_t>(h);
  image.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const Color c = canvas.pixel_at(x, y);
      const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                             static_cast<std::size_t>(x)) * 4U;
      image.rgba[i + 0] = c.r;
      image.rgba[i + 1] = c.g;
      image.rgba[i + 2] = c.b;
      image.rgba[i + 3] = c.a;
    }
  }
  const auto written = st::codec::png_write_file(path, image);
  if (!written) {
    std::fprintf(stderr, "写入失败 %s: %s\n", path.c_str(), written.error().message.c_str());
    return false;
  }
  std::printf("  -> %s\n", path.c_str());
  return true;
}

auto run(int argc, char** argv) -> int {
  std::string dir = "qa";
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--out" && i + 1 < argc) dir = argv[++i];
  }
  (void)st::fs::create_directories(dir);

  struct Case {
    const char* name;
    HighlightEdges edges;
  };
  const std::vector<Case> cases = {
      {"all", HighlightEdges::All},
      {"tb", HighlightEdges::TopBottom},
      {"tbs", HighlightEdges::TopBottomShaded},
      {"top", HighlightEdges::Top},
  };

  for (const Case& c : cases) {
    auto full = render(c.edges);
    (void)write_png(*full, st::fs::join(dir, std::string("hl-") + c.name + ".png"));
    auto zoom = zoom_corner(*full, 8);
    (void)write_png(*zoom, st::fs::join(dir, std::string("hl-") + c.name + "-corner8x.png"));
  }
  std::printf("完成：四形态全图 + 左上角 8x 放大\n");
  return 0;
}

}  // namespace

ST_MAIN(run)
