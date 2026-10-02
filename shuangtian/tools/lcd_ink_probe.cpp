/// ink 偏差分解探针（仅验证用，不进框架构建）：逐字形打印「亚像素 vs 灰度」的
/// 逐像素平均偏差，分别测「滤波开 / 滤波关」，用于分解 ink 口径的偏差构成。
///
/// 2026-10-02 用它定位了 text_subpixel_ink_matches_grayscale 长期红的根因：
/// 超阈（0.1083）来自 LCD 5-tap 滤波的横向摊墨（'I'@11px），与 hinting 无关；
/// 未滤波的逐像素形状偏差仅 0.0163。口径据此拆分（见该测试的注释）。
///
/// 编译（同 tools/README.md 口径；Windows 下无需 -ldl）：
/// ```bash
/// cd shuangtian && ./build/bin/st build st --profile dev
/// OBJS=$(ls build/dev/obj/*.o | grep -v -E "(main\.cpp\.o|_test\.cpp\.o|test_runner\.cpp\.o|_vendor_|examples_)")
/// g++ -std=c++20 -O1 -Iinclude -Ithird_party tools/lcd_ink_probe.cpp $OBJS -o /tmp/lcd_ink_probe -lpthread -lm
/// ```
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/text/text.hpp"

using st::text::FontStack;
using st::text::TextRenderer;

namespace {

constexpr std::string_view kSamples = "霜天自绘概览组件数据控制通道 win32 DPI";

[[nodiscard]] auto mean_abs_of(const TextRenderer::GlyphBitmap& gray,
                               const TextRenderer::GlyphBitmap& lcd) -> double {
  const std::size_t pixels =
      static_cast<std::size_t>(gray.width) * static_cast<std::size_t>(gray.height);
  if (pixels == 0 || lcd.coverage.size() < pixels * 3U || gray.coverage.size() < pixels) {
    return 0.0;
  }
  double total = 0.0;
  for (std::size_t index = 0; index < pixels; ++index) {
    const double mean = (static_cast<double>(lcd.coverage[index * 3U + 0U]) +
                         static_cast<double>(lcd.coverage[index * 3U + 1U]) +
                         static_cast<double>(lcd.coverage[index * 3U + 2U])) /
                        3.0;
    total += std::abs(mean - static_cast<double>(gray.coverage[index]));
  }
  return total / static_cast<double>(pixels);
}

}  // namespace

int main() {
  auto loaded = FontStack::system_default();
  if (!loaded.has_value()) {
    st::print("无字体\n");
    return 0;
  }
  FontStack stack(std::move(*loaded));
  if (stack.empty()) {
    st::print("字体栈为空\n");
    return 0;
  }

  TextRenderer gray(stack, 1.0f);
  TextRenderer lcd_f(stack, 1.0f);
  lcd_f.set_subpixel(true);
  TextRenderer lcd_raw(stack, 1.0f);
  lcd_raw.set_subpixel(true);
  lcd_raw.set_subpixel_filter(false);

  st::print("{:<8} {:>5} {:>10} {:>10}\n", "字形", "px", "滤波开", "滤波关");
  double worst_f = 0.0;
  double worst_r = 0.0;
  char32_t worst_f_cp = 0;
  char32_t worst_r_cp = 0;
  float worst_f_size = 0.0f;
  float worst_r_size = 0.0f;
  for (const float size : {11.0f, 13.5f, 15.0f, 22.0f, 40.0f}) {
    for (const char32_t codepoint : st::utf8_decode(kSamples)) {
      const auto a = gray.glyph_bitmap_of(codepoint, size);
      const auto bf = lcd_f.glyph_bitmap_of(codepoint, size);
      const auto br = lcd_raw.glyph_bitmap_of(codepoint, size);
      if (a == nullptr || bf == nullptr || br == nullptr || a->coverage.empty()) continue;
      const double mf = mean_abs_of(*a, *bf);
      const double mr = mean_abs_of(*a, *br);
      if (mf > worst_f) {
        worst_f = mf;
        worst_f_cp = codepoint;
        worst_f_size = size;
      }
      if (mr > worst_r) {
        worst_r = mr;
        worst_r_cp = codepoint;
        worst_r_size = size;
      }
      // 只打印偏差较大的（前几档）
      if (mf > 0.04 || mr > 0.04) {
        st::print("{:>6X} {:>5.1f} {:>10.4f} {:>10.4f}\n", static_cast<unsigned>(codepoint),
                  static_cast<double>(size), mf, mr);
      }
    }
  }
  st::print("\n滤波开：worst={:.4f} @U+{:04X} {}px\n", worst_f, static_cast<unsigned>(worst_f_cp),
            static_cast<double>(worst_f_size));
  st::print("滤波关：worst={:.4f} @U+{:04X} {}px\n", worst_r, static_cast<unsigned>(worst_r_cp),
            static_cast<double>(worst_r_size));
  return 0;
}
