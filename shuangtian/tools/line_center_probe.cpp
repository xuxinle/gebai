// ⚠ 已失效（2026-11-22 盘点）
//
// 失效原因：`centered_line_top` 签名已变
//
// 这个探针**当前编译不过**——它依赖的 API 已经删改。保留它是因为：
// 它记录过的实测结论可能仍被文档引用，删掉文件会让那些数字永久无法复核。
//
// 请把它当**历史档案**读，不要当活工具用。
// 真要复活：先按上面的原因把 API 改回当前形态，再验它测的东西是否还成立
//（很多测量前提已经变了，结论不保证可移植）。
//
// 盘点清单与分类判据见 tools/README.md「探针的分类与为何不全删」。

/// `centered_line_top` 算出的位置 vs 实际画出的墨迹位置——**逐 role 对照**。
///
/// ## 为什么需要它
///
/// 用户报「标题栏的图标、名称、按钮不在一条水平线」。实测三条中心：
/// 图标 19.5 / 品牌名 21.0 / 按钮文字 21.0（物理像素；栏物理中心 21.25）。
///
/// 而 `centered_line_top` 的契约是「让墨迹居中」，三条都走它却对不齐——
/// 那么要么是**算的位置不对**，要么是**画的位置与算的不同**。
///
/// `layout_line` 的 `role` 缺省 `Monospace`，而 `TextPort::draw` 缺省
/// `Proportional`——本探针把两个 role 各画一遍，量真实墨迹中心，
/// 直接回答“差多少、差在谁身上”。
#include <cstdio>
#include <string>

#include "st/app/text_port.hpp"
#include "st/text/text.hpp"
#include "st/codec/png.hpp"
#include "st/core/entry.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::raster::Canvas;

constexpr int kW = 400;
constexpr int kH = 120;
constexpr float kBoxY = 40.0f;
constexpr float kBoxH = 34.0f;
constexpr float kSize = 14.0f;
constexpr float kScale = 1.25f;   // 与小白实机一致

/// 在画布上量某一行的墨迹纵向范围（只看 x ∈ [x0,x1) 内）。
auto ink_rows(const Canvas& c, int x0, int x1) {
  int top = -1, bot = -1;
  for (int y = 0; y < c.height(); ++y) {
    bool any = false;
    for (int x = x0; x < x1; ++x) {
      const Color p = c.pixel_at(x, y);
      if ((p.r + p.g + p.b) / 3 < 200) { any = true; break; }
    }
    if (any) {
      if (top < 0) top = y;
      bot = y;
    }
  }
  return std::pair{top, bot};
}

auto run(int, char**) -> int {
  auto stack = st::text::FontStack::system_default();
  if (!stack) {
    std::fprintf(stderr, "拿不到系统字体：%s\n", stack.error().message.c_str());
    return 1;
  }
  st::text::TextRenderer renderer(*stack, kScale);
  const st::app::RendererTextPort port(renderer);

  std::printf("字号 %.0f，盒 y=%.0f h=%.0f，scale=%.2f\n", static_cast<double>(kSize),
              static_cast<double>(kBoxY), static_cast<double>(kBoxH),
              static_cast<double>(kScale));
  std::printf("盒中心（物理像素）= %.2f\n\n",
              static_cast<double>((kBoxY + kBoxH * 0.5f) * kScale));

  struct Case {
    const char* name;
    st::text::FontRole role;
  };
  // `draw` 的缺省是 Proportional；`layout_line` 的缺省是 Monospace
  const Case cases[] = {
      {"Proportional", st::text::FontRole::Proportional},
      {"Monospace", st::text::FontRole::Monospace},
  };

  std::printf("%-14s %-12s %-10s %-10s %s\n", "role", "绘制文本", "算出的y", "墨迹中心", "偏差");
  std::printf("%s\n", std::string(74, '-').c_str());

  // 关键对照：分别用**实际文本**与**参考样本本身**去画。
  // `layout_line` 用 `kInkReferenceSample`（"Ag(|") 算位置，而调用方画的是
  // 自己的文本。两者 `shaped_ascent` 不同就会错位——若“画参考样本”能居中
  // 而“画实际文本”偏上，就证实是**样本与实际文本的度量差**。
  const char* texts[] = {"AI", "Ag(|"};

  for (const Case& cs : cases) {
    for (const char* text : texts) {
      Canvas canvas{kW, kH, kScale};
      canvas.clear(Color{255, 255, 255, 255});

      // ① 按该 role 算位置（与 `centered_line_top` 内部同一调用）
      const float y = st::ui::centered_line_top(port, st::ui::kInkReferenceSample, kSize, kBoxY,
                                                kBoxH, cs.role);
      // ② 按该 role 画
      (void)port.draw(canvas, text, st::math::Point{40.0f, y}, kSize, Color{0, 0, 0, 255}, cs.role);

      const auto [top, bot] = ink_rows(canvas, 40, 220);
      const double center = (top + bot) / 2.0;
      const double expect = static_cast<double>(kBoxY + kBoxH * 0.5f) * static_cast<double>(kScale);
      std::printf("%-14s %-12s %-10.2f %-10.2f %+-10.2f\n", cs.name, text,
                  static_cast<double>(y) * static_cast<double>(kScale), center, center - expect);
    }
  }

  std::printf("\n口径：墨迹中心与盒中心的差值为 0 = 真正居中。\n");
  std::printf("若仅“画实际文本”偏、而“画参考样本”居中，就是样本与内容的度量差。\n");
  return 0;
}

}  // namespace

ST_MAIN(run)
