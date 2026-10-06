#pragma once

/// 文字渲染 A/B 探针的共用骨架（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 存在的理由：字符集页与字号页**必须与应用程序同一档渲染**（LCD 亚像素 + 低通滤波 +
/// light 拟合 + 墨量补偿 + 默认 gamma），否则对照出来的不是用户看到的那台机器；
/// 而档位初始化代码如果两份各抄一遍，迟早漂移。这里只留一份。

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

namespace ab {

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontRole;
using st::text::FontStack;
using st::text::GlyphClass;
using st::text::GridFitMode;
using st::text::TextRenderer;

constexpr Color kBackground{0xFF, 0xFF, 0xFF, 0xFF};

/// 与应用程序同一档的渲染器（参数可由命令行覆盖，用于关掉某一项做对照）。
struct Options {
  bool subpixel{true};
  bool filter{true};
  bool compensate{true};
  bool fit{true};
  bool fit_normal{false};   ///< true = Normal 档（两轴都拟合）；false = Light（只竖笔画）
  bool darken{false};       ///< 笔画加墨（stem darkening，FreeType CFF 口径）
  float digit_gamma{0.0f};  ///< **只对数字类**覆盖 gamma（0 = 不覆盖）
  float letter_gamma{0.0f}; ///< **只对拉丁字母类**覆盖 gamma（0 = 不覆盖）
  float han_gamma{0.0f};    ///< **只对汉字类**覆盖 gamma（0 = 不覆盖）
  bool strict_hints{false}; ///< hints 用几何法同款护栏（对照用）
  float gamma{0.0f};        ///< 0 = 用出厂默认（按主题）
};

inline void apply(TextRenderer& renderer, const Options& options, float scale) {
  (void)scale;
  renderer.set_subpixel(options.subpixel);
  renderer.set_subpixel_filter(options.filter);
  renderer.set_grid_fit(!options.fit ? GridFitMode::Off
                                     : (options.fit_normal ? GridFitMode::Normal
                                                           : GridFitMode::Light));
  renderer.set_ink_compensation(options.compensate);
  renderer.set_stem_darkening(options.darken);
  if (options.digit_gamma > 0.0f) {
    renderer.set_class_gamma(GlyphClass::Digit, options.digit_gamma);
  }
  if (options.letter_gamma > 0.0f) {
    renderer.set_class_gamma(GlyphClass::Letter, options.letter_gamma);
  }
  if (options.han_gamma > 0.0f) {
    renderer.set_class_gamma(GlyphClass::Han, options.han_gamma);
  }
  if (options.strict_hints) {
    // 通过环境变量切换（进程内 setenv：探针是独立进程，不影响别处）。
    static const int kIgnore = setenv("ST_TEXT_HINT_STRICT", "1", 1);
    (void)kIgnore;
  }
  if (options.gamma > 0.0f) renderer.set_coverage_gamma(options.gamma);
}

inline void save(const std::vector<std::uint8_t>& rgba, int width, int height,
                 const std::string& path) {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 255U);
  // 尺寸一致性必须先查：不匹配时 `std::copy` 会直接写越界（实测过堆损坏）。
  if (rgba.size() != image.rgba.size()) {
    st::print("  口径不一致：像素 {} vs 目标 {}，跳过 {}\n", rgba.size(), image.rgba.size(), path);
    return;
  }
  std::copy(rgba.begin(), rgba.end(), image.rgba.begin());
  if (const auto status = st::codec::png_write_file(path, image); !status.has_value()) {
    st::print("  写 {} 失败：{}\n", path, status.error().message);
  }
}

/// 打印实际生效的字体链（量尺必须能自证"两侧同一套字体"）。
inline void print_font_stack(const FontStack& stack) {
  st::print("字体链：");
  for (std::size_t i = 0; i < stack.faces().size(); ++i) {
    const auto& face = stack.faces()[i];
    st::print(" [{}]={}#{}", i, face.path(), face.face_index());
  }
  st::print("\n");
}

}  // namespace ab
