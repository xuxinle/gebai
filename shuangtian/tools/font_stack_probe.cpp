/// 字体栈诊断（仅验证用）：打印**每个码点实际由哪个 face 提供**，并报告该字形的度量。

///
/// 为什么需要它：与浏览器逐像素对照时发现「宽度一致、高度差 19%」——
/// 那意味着**两侧用的不是同一个 CJK 字体**（汉字在不同 CJK 字体里占的 em 比例不同）。
/// 只看渲染结果无法定位这件事，必须把"谁提供了这个字形"打出来。
///
/// 用法：font_stack_probe.exe [码点十六进制...]
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/text/text.hpp"

using st::text::FontRole;
using st::text::FontStack;

auto main(int argc, char** argv) -> int {
  // `--faces <path>`：遍历集合字体的各个 face，打印 name 与汉字墨迹高度。
  // 用途：判断"某个 face 虽然覆盖汉字、但字形是别的语种变体"（实测踩到：
  // `msyh.ttc` face 0 有汉字码点却比浏览器用的中文面**矮 19%**）。
  if (argc > 2 && std::string(argv[1]) == "--faces") {
    constexpr char32_t kProbe = U'画';
    for (int index = 0; index < 12; ++index) {
      auto face = st::text::FontFace::load(argv[2], index);
      if (!face) break;
      const std::string name = face->name();
      const bool covers = face->has_glyph(kProbe);
      double ink_h = -1.0;
      if (covers) {
        if (const auto g = face->glyph_for(kProbe); g.has_value()) {
          const auto& m = face->metrics();
          const float units = m.units_per_em > 0.0f ? m.units_per_em : 1000.0f;
          // 用 bearing_y 估墨迹高度不精确；这里只报 advance 与 ascender，够判断"是不是同一面"。
          ink_h = static_cast<double>(g->advance) / static_cast<double>(units);
        }
      }
      const auto& m = face->metrics();
      st::print("  face[{:>2}] name={}  covers_U+753B={}  advance={:.3f}em  asc/em={:.3f}\n",
                index, name, covers ? 1 : 0, ink_h,
                m.ascender / (m.units_per_em > 0.0f ? m.units_per_em : 1000.0f));
    }
    return 0;
  }

  std::vector<char32_t> probes;
  for (int i = 1; i < argc; ++i) probes.push_back(static_cast<char32_t>(std::stoul(argv[i], nullptr, 16)));
  if (probes.empty()) probes = {U'画', U'霜', U'A', U'2', U'\u2713'};

  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  const FontStack& fonts = *stack;
  st::print("=== FontStack 实际装配 ===\n");
  std::size_t index = 0;
  for (const auto& face : fonts.faces()) {
    const auto& m = face.metrics();
    st::print("  [{:>2}] {}  face={}  units/em={:.0f} asc={:.0f} desc={:.0f} linegap={:.0f}\n",
              index++, face.path(), face.face_index(), m.units_per_em, m.ascender, m.descender,
              m.line_gap);
  }
  st::print("  等宽 {} 个 / 粗体 {} 个\n", fonts.monospace_faces().size(), fonts.bold_faces().size());

  // 等宽链与粗体链的**逐档路径**：这两条链的“首位是谁”同样是产品约定
  //（等宽要 Consolas；粗体必须与常规链**逐位同族**，否则拉丁粗体会接管中文字）。
  st::print("  等宽链：");
  for (const auto& face : fonts.monospace_faces()) st::print(" {};", face.path());
  st::print("\n  粗体链：");
  for (const auto& face : fonts.bold_faces()) st::print(" {};", face.path());
  st::print("\n");

  st::print("\n=== 码点归属 ===\n");
  for (const char32_t cp : probes) {
    const auto* face = fonts.find_face(cp, FontRole::Proportional);
    if (face == nullptr) {
      st::print("  U+{:04X}  <无字体覆盖>\n", static_cast<unsigned>(cp));
      continue;
    }
    const auto glyph = face->glyph_for(cp);
    const auto& m = face->metrics();
    const float units = m.units_per_em > 0.0f ? m.units_per_em : 1000.0f;
    if (glyph.has_value()) {
      st::print("  U+{:04X}  ← {}\n          face_index={}  advance={:.3f}em  bearing=({:.3f},{:.3f})em\n",
                static_cast<unsigned>(cp), face->path(), face->face_index(),
                glyph->advance / units, glyph->bearing_x / units, glyph->bearing_y / units);
    } else {
      st::print("  U+{:04X}  ← {}（glyph_for 失败）\n", static_cast<unsigned>(cp), face->path());
    }
  }

  // —— 等宽与粗体的归属：两条独立回退链，同样只看"谁接管"是客观事实 ——
  // 等宽链：ASCII 应是 Consolas；汉字不在 Consolas 里 → 回退正文档（雅黑）。
  // 粗体链：中英文都应在**雅黑粗体**（msyhbd）——若英文落到 segoeuib、中文落到
  // 别的面，说明两条链的下标没对齐。
  st::print("\n=== 等宽链归属 ===\n");
  for (const char32_t cp : {U'i', U'M', U'W', U'霜'}) {
    const auto* mono_face = fonts.find_face(cp, FontRole::Monospace);
    st::print("  U+{:04X}  ← {}\n", static_cast<unsigned>(cp),
              mono_face != nullptr ? mono_face->path() : std::string("<无覆盖>"));
  }
  st::print("\n=== 粗体归属 ===\n");
  for (const char32_t cp : {U'A', U'霜'}) {
    const auto* bold_face = fonts.find_face(cp, FontRole::Proportional, /*bold=*/true);
    st::print("  U+{:04X}  ← {}\n", static_cast<unsigned>(cp),
              bold_face != nullptr ? bold_face->path() : std::string("<无覆盖>"));
  }
  return 0;
}
