// 标题栏**窗口控制按钮**的候选对照图（观感类改动的"先给候选"环节）。
//
// 为什么需要它：用户的要求是「再全面优化、美化下」——这是**感受**，不是规格。
// 而感受必须落到可量、可比的具体形态上，再由用户拍板；否则我改完只能说
// "我觉得更好看了"，用户下一次仍然会说"还是不行"（本轮前两回就是这个教训）。
//
// 本探针把每个正交量**单独**出一格，格与格之间只差那一个量，
// 于是"哪一格更好"能被直接指认，而不是靠想象。
//
// 每档两行：上行三按钮静止、上行右侧"关闭悬停"；下行同左侧、
// 下行右侧"最大化悬停" —— 悬停反馈看不看得见，两边一对比就知道。
//
// 用法： title_bar_buttons_probe [输出目录]

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;

/// 一格 = 一套参数下画出的三个按钮（最小化 / 最大化 / 关闭）。
struct Spec {
  const char* name;         ///< 档名（产物文件名与表头都用它）
  float bar_h;              ///< 标题栏高
  float btn_w;              ///< 按钮宽
  float glyph_px;           ///< 图标盒边长（`Icon::draw` 的光学归一化把它映射成固定墨迹尺寸）
  float hover_strength;     ///< 悬停底色的强度 0..1（1 = 用满主题给的那一档）
  float radius_em;          ///< 圆角（逻辑 px）
  bool close_danger;        ///< 关闭按钮是否用 danger 色相
  /// 悬停底色是**不透明铺**（true）还是**半透明罩层**（false）。
  /// 这是"几乎看不见"的根因：罩层叠在已经不透明的底色上时，0.10 的 alpha 只剩 2/255。
  bool opaque_hover_fill;
  /// 悬停块的**内缩**（逻辑 px）：横向与纵向分开。
  /// 0 = 全出血（与栏同高，系统标题栏的做法，命中区与可见块重合）；
  /// 纵向内缩 >0 时块高 = 栏高 - 2×inset_y（在 40 栏里给 32 块 = Windows 11 的按钮高度）。
  float hover_inset_x;
  float hover_inset_y;
  /// 关闭按钮悬停时是否用**实心 danger 底 + on_primary 字形**（Windows 11 的形态）。
  /// 它是“不可逆动作”的最强提示：色相变化 + 明度反转，两个正交量同时动。
  bool close_solid;
  /// 普通按钮悬停的底色（若为空则用 kNeutralHover）。
  /// 标题栏底色本身很淡（#EDEDF2），中性色在上面最多只能拉开 ~1.03——
  /// 要“看得见”就得换更重的中性档（`surface_sunken` / `border`）。
  Color neutral_tint;
  bool use_custom_tint;
};

/// 亮色主题里与标题栏相关的几个色（就地取值，不从主题变量推——避免口径漂移）。
constexpr Color kBarBg = Color::rgb(0xED, 0xED, 0xF2);        ///< surface_alt
constexpr Color kGlyph = Color::rgb(0x14, 0x14, 0x18);        ///< text
constexpr Color kDanger = Color::rgb(0xD1, 0x3B, 0x40);       ///< danger
constexpr Color kNeutralHover = Color::rgb(0xE3, 0xE3, 0xE5); ///< surface_pressed
constexpr Color kSunken = Color::rgb(0xDD, 0xDD, 0xE4);       ///< surface_sunken（比 pressed 再重一档）

/// 逐格几何：三个按钮等宽排在右端。
[[nodiscard]] auto button_rect(const Spec& spec, float bar_w, std::size_t index) -> Rect {
  const float right = bar_w - static_cast<float>(2 - index) * spec.btn_w;
  return Rect{right - spec.btn_w, 0.0f, spec.btn_w, spec.bar_h};
}

/// 悬停底色的**合成结果**（`strength` = 用满主题档的比例，0 = 不悬停）。
[[nodiscard]] auto hover_color(Color background, Color tint, float strength) -> Color {
  const float t = std::clamp(strength, 0.0f, 1.0f);
  if (t <= 0.0f) return background;
  const auto mix_chan = [t](std::uint8_t top, std::uint8_t bottom) -> std::uint8_t {
    const double dt = static_cast<double>(t);
    return static_cast<std::uint8_t>(std::lround(static_cast<double>(top) * dt +
                                                 static_cast<double>(bottom) * (1.0 - dt)));
  };
  return Color{mix_chan(tint.r, background.r), mix_chan(tint.g, background.g),
               mix_chan(tint.b, background.b), 255U};
}

void fill_rounded(Canvas& canvas, Rect rect, float radius, Color color) {
  if (rect.is_empty() || color.a == 0U) return;
  st::raster::Path path;
  path.add_rounded_rect(rect, std::min(radius, std::min(rect.width, rect.height) * 0.5f));
  canvas.fill_path(path, st::raster::Paint::solid(color));
}

/// 画一行三按钮；`hovered` 指定哪一格悬停（-1 = 都不悬停）。
void draw_row(Canvas& canvas, const Spec& spec, float x0, float y0, int hovered) {
  const float bar_w = spec.btn_w * 3.0f;
  canvas.fill_rect(Rect{x0, y0, bar_w, spec.bar_h}, st::raster::Paint::solid(kBarBg));
  for (std::size_t index = 0; index < 3; ++index) {
    const Rect rect = button_rect(spec, bar_w, index);
    const bool is_close = index == 2;
    const bool hot = hovered >= 0 && static_cast<std::size_t>(hovered) == index;
    Color tone = kGlyph;
    if (hot) {
      const bool danger_tone = is_close && spec.close_danger;
      const Color tint = spec.use_custom_tint
                             ? spec.neutral_tint
                             : (danger_tone ? kDanger : kNeutralHover);
      const Rect block = st::math::Rect{
          rect.x + spec.hover_inset_x, rect.y + spec.hover_inset_y,
          rect.width - spec.hover_inset_x * 2.0f, rect.height - spec.hover_inset_y * 2.0f}
                                 .offset(x0, y0);
      if (danger_tone && spec.close_solid) {
        // 系统形态：实心 danger 底 + 白字（明度与色相同时反转）
        fill_rounded(canvas, block, spec.radius_em, kDanger);
        tone = Color::rgb(0xFF, 0xFF, 0xFF);
      } else if (spec.opaque_hover_fill) {
        fill_rounded(canvas, block, spec.radius_em,
                     hover_color(kBarBg, tint, spec.hover_strength));
        if (danger_tone) tone = kDanger;
      } else {
        // 旧形态：半透明罩层
        fill_rounded(canvas, block, spec.radius_em,
                     tint.with_alpha_f(spec.hover_strength));
        if (danger_tone) tone = kDanger;
      }
    }
    const std::string_view name = index == 0 ? "minus" : (index == 1 ? "square" : "close");
    const float g = spec.glyph_px;
    const Rect box{x0 + rect.x + (rect.width - g) * 0.5f,
                   y0 + rect.y + (rect.height - g) * 0.5f, g, g};
    st::ui::Icon::draw(canvas, name, box, tone, 0.0f);
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string dir = argc > 1 ? argv[1] : ".";

  // —— 候选档：每档相对「当前」只改一个正交量 ——
  // 「当前」= 46×40 / 字形 14 / 悬停 0.10 半透明 / 圆角 6 / 关闭用 danger / 全出血
  const std::vector<Spec> specs = {
      {"A-current",       40.0f, 46.0f, 14.0f, 0.10f, 6.0f, true,  false, 0.0f, 0.0f, false, kNeutralHover, false},
      {"B-hover-opaque",  40.0f, 46.0f, 14.0f, 0.30f, 6.0f, true,  true,  0.0f, 0.0f, false, kNeutralHover, false},
      {"C-strong-neutral",40.0f, 46.0f, 14.0f, 1.00f, 4.0f, true,  true,  0.0f, 4.0f, false, kSunken,       true},
      {"D-system-full",   40.0f, 46.0f, 14.0f, 1.00f, 0.0f, true,  true,  0.0f, 0.0f, false, kSunken,       true},
      {"E-inset-all",     40.0f, 46.0f, 14.0f, 1.00f, 6.0f, true,  true,  4.0f, 4.0f, false, kSunken,       true},
      {"F-inset-y-only",  40.0f, 46.0f, 14.0f, 1.00f, 4.0f, true,  true,  0.0f, 4.0f, false, kSunken,       true},
      {"G-close-solid",   40.0f, 46.0f, 14.0f, 1.00f, 4.0f, true,  true,  0.0f, 4.0f, true,  kSunken,       true},
      {"H-glyph-16",      40.0f, 46.0f, 16.0f, 1.00f, 4.0f, true,  true,  0.0f, 4.0f, true,  kSunken,       true},
      {"I-neutral-close", 40.0f, 46.0f, 14.0f, 1.00f, 4.0f, false, true,  0.0f, 4.0f, false, kSunken,       true},
  };

  const float gap = 10.0f;
  const float pad = 12.0f;
  float max_bar_h = 0.0f;
  float max_group_w = 0.0f;
  for (const auto& spec : specs) {
    max_bar_h = std::max(max_bar_h, spec.bar_h);
    max_group_w = std::max(max_group_w, spec.btn_w * 3.0f);
  }
  const float row_h = max_bar_h;
  const float cell_h = row_h * 2.0f + 6.0f;
  const float content_w = max_group_w * 2.0f + 10.0f;
  const int width = static_cast<int>(std::ceil(content_w + pad * 2.0f));
  const int height = static_cast<int>(std::ceil(
      pad * 2.0f + cell_h * static_cast<float>(specs.size()) + gap * static_cast<float>(specs.size())));

  Canvas sheet{width, height, 1.0f};
  sheet.clear(Color::rgb(0xFF, 0xFF, 0xFF));

  std::string report;
  float y = pad;
  for (const auto& spec : specs) {
    const float right_x = pad + max_group_w + 10.0f;
    // 上行：静止 / 关闭悬停
    draw_row(sheet, spec, pad, y, -1);
    draw_row(sheet, spec, right_x, y, 2);
    // 下行：静止 / 最大化悬停
    draw_row(sheet, spec, pad, y + row_h + 6.0f, -1);
    draw_row(sheet, spec, right_x, y + row_h + 6.0f, 1);
    y += cell_h + gap;

    report += std::format(
        "{}: 栏高 {:.0f}  按钮 {:.0f}×{:.0f}（纵横比 {:.2f}）  字形盒 {:.0f}  "
        "悬停强度 {:.2f} {}  内缩 x{:.0f}/y{:.0f}  圆角 {:.1f}  关闭用 danger={}\n",
        spec.name, static_cast<double>(spec.bar_h), static_cast<double>(spec.btn_w),
        static_cast<double>(spec.bar_h), static_cast<double>(spec.btn_w / spec.bar_h),
        static_cast<double>(spec.glyph_px), static_cast<double>(spec.hover_strength),
        spec.opaque_hover_fill ? "不透明铺" : "半透明罩层（旧）",
        static_cast<double>(spec.hover_inset_x), static_cast<double>(spec.hover_inset_y),
        static_cast<double>(spec.radius_em), spec.close_danger ? "是" : "否");
  }

  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba = sheet.to_rgba8();
  const std::string sheet_path = dir + "/wbtn-grid.png";
  if (auto written = st::codec::png_write_file(sheet_path, image); !written) {
    st::print("写盘失败：{}\n", written.error().message);
    return 1;
  }
  st::print("网格图: {}\n", sheet_path);

  const std::string text_path = dir + "/wbtn-metrics.txt";
  if (auto status = st::fs::write_text(text_path, report); !status) {
    st::print("写度量失败：{}\n", status.error().to_string());
    return 1;
  }
  st::print("{}", report);
  st::print("度量: {}\n", text_path);
  return 0;
}
