/// **hint 路径验收入口**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 回答两个问题（方案 B：执行字体自带的 CFF stem hints）：
///   ① 字体里到底有多少 stem hints（`vstem`/`hstem`），各字形的分布如何；
///   ② 这些 hints 是否真的被用上了——逐字形打印 `GlyphBitmap` 的
///      `fit_stems`（参与拟合的笔画数）与 `fit_hint_stems`（其中来自 hints 的）。
///
/// 用法：stems_probe [逻辑字号] [--face 路径] [--index N]
#include <cmath>
#include <string>
#include <vector>

#include <cstdlib>

#include "st/core/print.hpp"
#include "st/text/text.hpp"

using st::text::FontRole;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

namespace {

/// 与 A/B 对照同一批样本（简单→复杂汉字 + 拉丁对照）。
const char* kSamples[] = {
    "一二十丁厂七卜人入八九几儿了力乃刀又", "三上下个大天大地小口山日月水火木金土",
    "中文测试字体渲染清晰组件画廊",         "霜天字体渲染粗细均匀锐利清晰",
    "魏蜀黍龘龗爨蠹灩鬱麤齉齾",             "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
};

/// UTF-8 → 码点（返回下一个字节位置）。
/// ⚠ 第一版漏了续字节的移位（`cp |= b[k] & 0x3F` 而没 `<< 6`），
/// 于是查的都是错码点——"字体里没有 hints"是**量尺错**而不是事实。
[[nodiscard]] auto next_codepoint(const char* p, char32_t& out) -> const char* {
  unsigned char b0 = static_cast<unsigned char>(*p);
  if (b0 < 0x80U) {
    out = b0;
    return p + 1;
  }
  if ((b0 & 0xE0U) == 0xC0U) {
    out = static_cast<char32_t>((b0 & 0x1FU) << 6U) |
          static_cast<char32_t>(static_cast<unsigned char>(p[1]) & 0x3FU);
    return p + 2;
  }
  if ((b0 & 0xF0U) == 0xE0U) {
    out = static_cast<char32_t>((b0 & 0x0FU) << 12U) |
          static_cast<char32_t>((static_cast<unsigned char>(p[1]) & 0x3FU) << 6U) |
          static_cast<char32_t>(static_cast<unsigned char>(p[2]) & 0x3FU);
    return p + 3;
  }
  out = static_cast<char32_t>((b0 & 0x07U) << 18U) |
        static_cast<char32_t>((static_cast<unsigned char>(p[1]) & 0x3FU) << 12U) |
        static_cast<char32_t>((static_cast<unsigned char>(p[2]) & 0x3FU) << 6U) |
        static_cast<char32_t>(static_cast<unsigned char>(p[3]) & 0x3FU);
  return p + 4;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  float logical = argc > 1 && argv[1][0] != '-' ? std::stof(argv[1]) : 15.0f;
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  const FontStack& fonts = *stack;
  const float scale = 1.5f;
  const float pixel_size = logical * scale;

  TextRenderer renderer{fonts, scale};
  renderer.set_grid_fit(GridFitMode::Normal);   // 两轴都拟合，便于看横画 hints
  renderer.set_subpixel(true);                  // 与应用程序同档（hint 只在亚像素分支注入）
  renderer.set_subpixel_filter(true);
  renderer.set_coverage_gamma(1.0f);

  // ① 字体里有多少 hints
  st::print("字体链：{} 个面；首位 {}#{}  逻辑 {:.1f}px → 物理 {:.1f}px\n", fonts.faces().size(),
            fonts.primary().path(), fonts.primary().face_index(), logical, pixel_size);
  st::print("汉字归属：{}#{}（hints 在它上面）\n",
            [&] { const auto* f = fonts.find_face(U'霜'); return f ? f->path() : std::string("—"); }(),
            [&] { const auto* f = fonts.find_face(U'霜'); return f ? f->face_index() : -1; }());
  st::print("\n① 字形自带的 stem hints（字体单位）\n");
  st::print("{:<8}{:>8}{:>8}{:>10}{:>10}\n", "码点", "竖提示", "横提示", "最窄竖宽", "最宽竖宽");
  for (const char* sample : kSamples) {
    for (const char* p = sample; *p != '\0';) {
      // UTF-8 → 码点（简化：本批全是 BMP）
      char32_t cp = 0;
      p = next_codepoint(p, cp);
      const auto* face = fonts.find_face(cp, FontRole::Proportional);
      if (face == nullptr) continue;
      const auto gid = face->glyph_index(cp);
      if (!gid.has_value()) continue;
      const auto& hints = face->stem_hints(*gid);
      int vertical = 0, horizontal = 0;
      float narrow = 1e9f, wide = 0.0f;
      for (const auto& hint : hints) {
        if (hint.vertical) {
          ++vertical;
          narrow = std::min(narrow, hint.hi - hint.lo);
          wide = std::max(wide, hint.hi - hint.lo);
        } else {
          ++horizontal;
        }
      }
      st::print("U+{:04X}  {:>8}{:>8}{:>10.1f}{:>10.1f}\n", static_cast<unsigned>(cp), vertical,
                horizontal, vertical ? narrow : 0.0f, vertical ? wide : 0.0f);
      break;   // 每串只看第一个字，够说明问题
    }
  }

  // ② hints 是否真被用上
  st::print("\n② 逐字形拟合诊断（fit_stems = 参与拟合的笔画数，hint = 其中来自字体的）\n");
  st::print("{:<10}{:>12}{:>10}{:>12}{:>12}\n", "字形", "字体hint数", "接上点", "参与拟合", "被拒");
  int with_hint = 0, without_hint = 0, total_stems = 0, total_hint = 0;
  for (const char* sample : kSamples) {
    for (const char* p = sample; *p != '\0';) {
      char32_t cp = 0;
      p = next_codepoint(p, cp);
      const auto bitmap = renderer.glyph_bitmap_of(cp, pixel_size);
      if (bitmap == nullptr) continue;
      total_stems += bitmap->fit_stems;
      total_hint += bitmap->fit_hint_stems;
      if (bitmap->fit_hint_stems > 0) ++with_hint;
      else ++without_hint;
      if (std::getenv("ST_TEXT_HINT_DEBUG") != nullptr) {
        st::print("HINTDBG cp=U+{:04X} seen={} bound={} stems={} fit={}\n",
                  static_cast<unsigned>(cp), bitmap->fit_hint_seen, bitmap->fit_hint_bound,
                  bitmap->fit_hint_stems, bitmap->fit_stems);
      }
      if (bitmap->fit_hint_seen > 0) {
        st::print("U+{:04X}  {:>12}{:>10}{:>12}{:>12}\n", static_cast<unsigned>(cp),
                  bitmap->fit_hint_seen, bitmap->fit_hint_bound, bitmap->fit_hint_stems,
                  bitmap->fit_rejected_stems);
      }
    }
  }
  st::print("合计：{} 个字形中 {} 个用上了 hints，{} 个没有；笔画总数 {}（其中 hint {}）\n", with_hint + without_hint,
            with_hint, without_hint, total_stems, total_hint);
  return 0;
}
