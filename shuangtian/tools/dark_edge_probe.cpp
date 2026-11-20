/// 暗色主题下「左右仍有高光」的成分拆解。
///
/// ## 现象
///
/// 用户在**暗色主题**下看到气泡四边都有浅色亮线；亮色主题下同一份配置
/// （`edges tbs`）是对的。实测（暗色，卡内 33）：
///   顶部 119, 97（亮）  底部 41, 79（亮）
///   左侧 26, 26（暗 7）  右侧 28, 27（暗 6）
/// 左右数值上**确实更暗**，但 10x 放大图能看出一条垂直浅线——
/// 说明那条线不是 `top_highlight` 画的，而是**别的成分**。
///
/// ## 本探针
///
/// 在暗色主题下把各成分逐个关掉，看左侧那条浅线在哪一组消失：
///   A. 完整样式
///   B. 关折射（refraction 0, dispersion 0）
///   C. 关高光（highlight 0）
///   D. 都关
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/entry.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::Panel;
using st::ui::UiRoot;

auto probe(const std::string& style, const char* out_png, bool save) {
  UiRoot root;
  root.set_viewport({400.0f, 200.0f});
  root.set_theme(st::ui::Theme::dark());

  // 用 **`Card`**（实机用的就是它），不是 `Panel`：
  // `Card::apply_theme` 会写 `surface` / `top_highlight` / `border` 等，
  // 与 `Panel` 的主题默认完全不同——这是探针与实机可能分叉的第一个点。
  auto card = std::make_unique<st::ui::Card>();
  auto* raw = card.get();
  raw->set_id("bubble");
  root.set_content(std::move(card));
  (void)st::ui::apply_glass_shorthand(*raw, style);

  st::ui::RenderContext context{root.theme(), nullptr, 1.0f};
  // 用**实机气泡的真实尺寸**（298×98，radius 14）——
  // 探针早些用 300×120，高度不同会让“圆角占高比例”不同，
  // 而 `TopBottomShaded` 的剔除区间是按 `cap_h = radius + ring` 算的。
  const Rect box{40.0f, 40.0f, 298.0f, 98.0f};
  raw->measure(context, st::ui::Constraints{});
  raw->arrange(context, box);

  Canvas canvas{400, 200, 1.0f};
  canvas.clear(Color{24, 24, 27, 255});   // 暗色页面底
  raw->paint(context, canvas);

  if (save) {
    st::codec::PngImage img;
    img.width = static_cast<std::uint32_t>(canvas.width());
    img.height = static_cast<std::uint32_t>(canvas.height());
    img.rgba.resize(static_cast<std::size_t>(img.width) * img.height * 4U);
    for (int y = 0; y < canvas.height(); ++y) {
      for (int x = 0; x < canvas.width(); ++x) {
        const Color c = canvas.pixel_at(x, y);
        const std::size_t i = (static_cast<std::size_t>(y) * img.width +
                               static_cast<std::size_t>(x)) * 4U;
        img.rgba[i] = c.r; img.rgba[i + 1] = c.g; img.rgba[i + 2] = c.b; img.rgba[i + 3] = c.a;
      }
    }
    (void)st::codec::png_write_file(out_png, img);
  }

  const int cy = 100;
  std::vector<int> left, right, top, bottom;
  for (int d = 0; d < 5; ++d) {
    const Color l = canvas.pixel_at(40 + d, cy);   left.push_back((l.r + l.g + l.b) / 3);
    const Color r = canvas.pixel_at(337 - d, cy);  right.push_back((r.r + r.g + r.b) / 3);
    const Color t = canvas.pixel_at(190, 40 + d);  top.push_back((t.r + t.g + t.b) / 3);
    const Color b = canvas.pixel_at(190, 137 - d); bottom.push_back((b.r + b.g + b.b) / 3);
  }
  return std::make_tuple(left, right, top, bottom);
}

auto run(int, char**) -> int {
  struct Case { const char* name; std::string style; bool save; };
  const std::vector<Case> cases = {
      {"A 完整", "radius 14, border 0, shadow 0, theme_glass 1, blur 16, refraction 6, "
                  "dispersion 0.3, highlight 0.85, ring 1.5, edges tbs", true},
      {"B 关折射", "radius 14, border 0, shadow 0, theme_glass 1, blur 16, refraction 0, "
                    "dispersion 0, highlight 0.85, ring 1.5, edges tbs", true},
      {"C 关高光", "radius 14, border 0, shadow 0, theme_glass 1, blur 16, refraction 6, "
                    "dispersion 0.3, highlight 0, ring 1.5, edges tbs", true},
      {"D 都关", "radius 14, border 0, shadow 0, theme_glass 1, blur 16, refraction 0, "
                  "dispersion 0, highlight 0, ring 1.5, edges tbs", false},
      {"E edges=all", "radius 14, border 0, shadow 0, theme_glass 1, blur 16, refraction 6, "
                       "dispersion 0.3, highlight 0.85, ring 1.5, edges all", true},
  };

  std::printf("暗色主题，卡底 33，页面底 24\n");
  std::printf("%-12s %-22s %-22s %-20s %s\n", "case", "左(外→内)", "右(外→内)", "上(外→内)", "下(外→内)");
  std::printf("%s\n", std::string(104, '-').c_str());
  for (const Case& c : cases) {
    const std::string png = std::string("qa/dark-") + c.name + ".png";
    auto [l, r, t, b] = probe(c.style, png.c_str(), c.save);
    auto fmt = [](const std::vector<int>& v) {
      std::string s; for (int x : v) s += std::to_string(x) + " "; return s;
    };
    std::printf("%-12s %-22s %-22s %-20s %s\n", c.name, fmt(l).c_str(), fmt(r).c_str(),
                fmt(t).c_str(), fmt(b).c_str());
  }
  std::printf("\n卡内参照 33。左右若出现明显 > 33 的值，就是那条浅线。\n");
  return 0;
}

}  // namespace

ST_MAIN(run)
