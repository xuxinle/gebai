// tmp_dwab_probe.cpp —— 霜天侧 DirectWrite 对标渲染（仅验证用，不进框架构建）。
//
// 与 tools/dwab/dwrite_ref.cpp 同一份行表（tools/tmp_gen_dwab.py 单源生成 .inc/.tsv/.json），
// 产出物理像素 1:1 的 PNG，与 dw-natural-light.png 等逐带比较。
//
// 档位：**应用程序默认档**（LCD 亚像素 + 5-tap 滤波 + fit=off + 主题 gamma + 类 gamma），
// 由 ab::Options 的默认值 + 类 gamma 三行组成——与 src/app/app.cpp 的接线逐项一致。
//   浅底：coverage_gamma=1.10（kDefaultCoverageGamma）
//   深底：coverage_gamma=0.60（kDefaultCoverageGammaOnDark）
//   类 gamma：亮 0.84/0.84/0.88（digit/letter/han）；暗 0.64/0.62/0.75
//
// 用法：tmp_dwab_probe <outdir> [--scale 1.5] [--dark] [--gamma X] [--nogray]
//                       [--fit light|normal|off] [--no-darken] [--no-class-gamma]
//                       [--gray]
#include "text_ab_common.hpp"

#include "st/app/app.hpp"   // ClassGammas / DarkClassGammas：探针必须用应用同一份常量
#include "st/core/fs.hpp"     // read_env：复刻 resolve_text_lcd 的“环境变量覆盖”口径

#include "dwab/rows-light.inc"
#include "dwab/rows-dark.inc"

#include <cstdio>
#include <string>

using ab::Color;
using ab::Point;
using ab::TextRenderer;

namespace {

template <typename DwColorT>
inline Color to_color(const DwColorT& c) {
  return Color{c.r, c.g, c.b, 255};
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  float scale = 1.5f;
  bool dark = false;
  float gamma_override = 0.0f;
  float supersample_override = 0.0f;
  float strength_override = -1.0f;   // <0 = 不覆盖（用应用默认）
  float contrast_override = -1.0f;
  bool use_curve = true;
  std::string curve_path;
  bool skia_mode = false;
  float skia_contrast = 1.0f;
  float digit_g = 0.0f, letter_g = 0.0f, han_g = 0.0f;   // 0 = 用内置默认档
  bool class_gamma = true;
  ab::Options options;   // 亚像素/滤波：用**应用层解析结果**（见下），不在探针里写死
  options.fit = false;     // src/app/app.cpp：resolve_text_fit 默认 Off（浏览器基准）
  options.compensate = false;  // resolve_ink_compensation(Off) = false
  options.darken = false;  // app 默认开但仅作用于 CFF；msyh 是 TrueType → 无效果（如实接线为关）
  // **抗锯齿形态与应用默认一致**（`resolve_text_lcd("auto")` 的规则）。
  // 探针只链 core/math/codec/raster/text（无 app 单元），所以在这里复刻同一规则。
  // ⚠ **默认值必须与 app.cpp 同步改**：本轮就踩过——app 改灰度后又改回亚像素，
  // 而这里漏改，量出的彩边 79.9 与真实默认完全不符（假数据）。
  options.subpixel = [] {
    const auto v = st::fs::read_env("ST_TEXT_LCD");
    if (v.has_value() && !v->empty()) {
      if (*v == "1" || *v == "on" || *v == "true") return true;
      if (*v == "0" || *v == "off" || *v == "false") return false;
    }
    return true;   // 与应用默认一致：亚像素（实测 VS Code 走 ClearType）
  }();
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dark") dark = true;
    else if (a == "--gray") options.subpixel = false;
    else if (a == "--lcd") options.subpixel = true;
    else if (a == "--no-class-gamma") class_gamma = false;
    else if (a == "--no-darken") options.darken = false;
    else if (a == "--fit-light") { options.fit = true; options.fit_normal = false; }
    else if (a == "--fit-normal") { options.fit = true; options.fit_normal = true; }
    else if (a.rfind("--gamma=", 0) == 0) gamma_override = std::stof(a.substr(8));
    else if (a.rfind("--supersample=", 0) == 0) supersample_override = std::stof(a.substr(14));
    else if (a.rfind("--strength=", 0) == 0) strength_override = std::stof(a.substr(11));
    else if (a.rfind("--cstrength=", 0) == 0) contrast_override = std::stof(a.substr(12));
    else if (a == "--no-curve") use_curve = false;
    else if (a.rfind("--curve=", 0) == 0) curve_path = a.substr(8);
    else if (a == "--skia") skia_mode = true;
    else if (a.rfind("--skia-contrast=", 0) == 0) {
      skia_mode = true;
      skia_contrast = std::stof(a.substr(16));
    }
    else if (a.rfind("--digit-gamma=", 0) == 0) digit_g = std::stof(a.substr(14));
    else if (a.rfind("--letter-gamma=", 0) == 0) letter_g = std::stof(a.substr(15));
    else if (a.rfind("--han-gamma=", 0) == 0) han_g = std::stof(a.substr(12));
    else if (a.rfind("--scale=", 0) == 0) scale = std::stof(a.substr(8));
  }

  auto stack = st::text::FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  ab::print_font_stack(*stack);

  // 两主题行表同文本同布局（仅色不同），统一用 light 行表，色按主题取。
  const auto& lines = dwab_light::kDwLines;
  const int n_lines = (int)(sizeof(dwab_light::kDwLines) / sizeof(dwab_light::kDwLines[0]));
  const float logical_w = dwab_light::kDwWidth;
  const float logical_h = dwab_light::kDwHeight;
  const int width = (int)(logical_w * scale + 0.5f);
  const int height = (int)(logical_h * scale + 0.5f);
  ab::g_background = dark ? to_color(dwab_dark::kDwBg) : to_color(dwab_light::kDwBg);

  st::raster::Canvas canvas{width, height, scale};
  canvas.clear(ab::g_background);
  TextRenderer renderer{*stack, scale};
  // —— 应用默认档接线（src/app/app.cpp 逐项一致）——
  renderer.set_subpixel(options.subpixel);            // resolve_text_lcd 默认 true
  renderer.set_subpixel_filter(options.filter);       // 默认 true
  renderer.set_grid_fit(options.fit ? (options.fit_normal ? st::text::GridFitMode::Normal
                                                          : st::text::GridFitMode::Light)
                                    : st::text::GridFitMode::Off);  // 默认 Off
  renderer.set_ink_compensation(false);               // resolve_ink_compensation(Off)=false
  const float theme_gamma = dark ? TextRenderer::kDefaultCoverageGammaOnDark   // 0.60
                                 : TextRenderer::kDefaultCoverageGamma;        // 1.10
  renderer.set_coverage_gamma(gamma_override > 0.0f ? gamma_override : theme_gamma);
  // 超采样：默认 1.0（应用默认）。像素级复刻探索用——提高覆盖率采样精度，
  // 看能否把「小字号覆盖率被摊平」的分布拉回与 DWrite 一致。
  if (supersample_override > 0.0f) renderer.set_supersample(supersample_override);
  if (strength_override >= 0.0f) renderer.set_subpixel_strength(strength_override);
  if (contrast_override >= 0.0f) renderer.set_coverage_contrast_strength(contrast_override);
  // 字重曲线：`--curve=<文件>` 或环境变量 `ST_TEXT_WEIGHT_CURVE`；
  // `--no-curve` 显式关闭（对照实验用）。默认尝试内置路径。
  if (use_curve) {
    st::text::WeightCurve curve;
    std::size_t loaded = 0;
    if (!curve_path.empty()) {
      loaded = curve.load_from_file(curve_path);
    } else if (const auto env = st::fs::read_env("ST_TEXT_WEIGHT_CURVE");
               env.has_value() && !env->empty()) {
      loaded = curve.load_from_file(*env);
    } else {
      loaded = curve.load_from_file("resources/text_weight_curve.txt");
    }
    if (loaded > 0) {
      st::print("  weight_curve: {} 个采样点  {}\n", loaded, curve.describe());
      renderer.set_weight_curve(std::move(curve));
    } else {
      st::print("  weight_curve: 未加载（{}）\n", curve_path.empty() ? "默认路径不存在" : curve_path);
    }
  } else {
    st::print("  weight_curve: 已禁用（--no-curve）\n");
  }
  // Skia 式覆盖率校正（Chromium 在 DWrite 之上那一层）：实测 VS Code 比霜天「更实且彩边更少」
  // 的差距就在这里。contrast 越大，中间调越向两端推（→ 更实、彩边更少）。
  if (skia_mode) {
    renderer.set_coverage_correct(TextRenderer::CoverageCorrect::Skia);
    renderer.set_coverage_contrast(skia_contrast);
    renderer.set_text_colors(dark ? 0xE8EEF9U : 0x0F172AU, dark ? 0x0F1115U : 0xFFFFFFFFU);
  }
  if (class_gamma) {
    using GC = st::text::GlyphClass;
    // **直接引用应用层常量**（`st::app::ClassGammas`）：探针写死一份旧值，
    // 就会出现“应用改了默认、探针还在测旧档”的静默错位（本文件就踩过：
    // app.hpp 已改 0.70/1.50，探针 fallback 还是 0.88/0.84，量出的汉字实心 0.753 而非 0.97）。
    const st::app::ClassGammas light{};
    const st::app::DarkClassGammas dark_g{};
    float d_flt = dark ? dark_g.digit : light.digit;
    float l_flt = dark ? dark_g.letter : light.letter;
    float h_flt = dark ? dark_g.han : light.han;
    if (digit_g > 0.0f) d_flt = digit_g;
    if (letter_g > 0.0f) l_flt = letter_g;
    if (han_g > 0.0f) h_flt = han_g;
    renderer.set_class_gamma(GC::Digit, d_flt);
    renderer.set_class_gamma(GC::Letter, l_flt);
    renderer.set_class_gamma(GC::Han, h_flt);
  }
  renderer.set_stem_darkening(true);                  // app 默认开（TrueType 上无效果，如实接线）

  st::print("scale={} dark={} gamma={} class_gamma={} (digit={} letter={} han={}) subpixel={} fit={} ss={}\n",
            scale, dark, renderer.coverage_gamma(), class_gamma,
            renderer.class_gamma(st::text::GlyphClass::Digit),
            renderer.class_gamma(st::text::GlyphClass::Letter),
            renderer.class_gamma(st::text::GlyphClass::Han),
            options.subpixel, options.fit, renderer.supersample());
  st::print("  subpixel_strength={}\n", renderer.subpixel_strength());

  const auto fg = dark ? to_color(dwab_dark::kDwFg) : to_color(dwab_light::kDwFg);
  for (int i = 0; i < n_lines; ++i) {
    const auto& line = lines[i];
    const st::text::FontRole role = line.mono ? st::text::FontRole::Monospace
                                              : st::text::FontRole::Proportional;
    (void)renderer.draw(canvas, line.text, Point{4.0f, line.top}, line.size,
                        fg, role, 0.0f, line.bold);
  }
  char path[512];
  sprintf_s(path, sizeof path, "%s/st-%s.png", out_dir.c_str(), dark ? "dark" : "light");
  ab::save(canvas.to_rgba8(), width, height, path);
  st::print("已写 {}（{}x{} 物理像素）\n", path, width, height);
  return 0;
}