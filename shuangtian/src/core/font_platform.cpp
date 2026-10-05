/// 平台字体偏好实现：系统默认字体链的候选文件（按平台取首选）。
///
/// 平台差异（字体目录与首选族名）集中在此文件，字型引擎只按返回顺序探测——
/// 见 CONVENTIONS.md §10 第 1 条「平台差异只能出现在 platform_* 里」。
///
/// 为什么需要它：此前候选表把 Windows / Linux / macOS 的路径混在一张列表里，
/// 而「首选是哪一族」是产品取向（Windows 要中英文同体），不是路径知识——
/// 两者混在一处会让“改一个默认字体”变成跨平台事故。

#include "st/core/font_platform.hpp"

#include <initializer_list>

#include "st/core/fs.hpp"

namespace st::platform {
namespace {

/// 只保留真实存在的常规文件（探测不到即跳过，不报错）。
[[nodiscard]] auto existing(std::initializer_list<FontPreference> entries)
    -> std::vector<FontPreference> {
  std::vector<FontPreference> out;
  for (const FontPreference& entry : entries) {
    if (entry.path.empty()) continue;
    if (!fs::is_regular_file(entry.path)) continue;
    out.push_back(entry);
  }
  return out;
}

void append(std::vector<FontPreference>& out, std::vector<FontPreference> more) {
  out.insert(out.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
}

// —— 首选档（平台取向）——

/// Windows 用微软雅黑：它自带拉丁字形，因此**中英文同一族**——这正是它被排到
/// 拉丁回退层之前的原因（否则英文会被 Segoe UI 接管，界面出现"中文雅黑、
/// 英文 Segoe"的两族混排）。其余平台不指定特定取向，只用通用回退层。
[[nodiscard]] auto primary_text_faces() -> std::vector<FontPreference> {
#if defined(_WIN32)
  return existing({{"C:/Windows/Fonts/msyh.ttc", true}});
#else
  return {};
#endif
}

[[nodiscard]] auto primary_text_faces_bold() -> std::vector<FontPreference> {
#if defined(_WIN32)
  return existing({{"C:/Windows/Fonts/msyhbd.ttc", true}});
#else
  return {};
#endif
}

/// Windows 用 Consolas（`consola.ttf`）。汉字不在其中，由 `find_face(role=Monospace)`
/// 的“等宽库 → 正文档”回退补足（代码里的中文注释仍可读）。
[[nodiscard]] auto primary_mono_faces() -> std::vector<FontPreference> {
#if defined(_WIN32)
  return existing({{"C:/Windows/Fonts/consola.ttf", false}});
#else
  return {};
#endif
}

// —— 回退层：拉丁 → CJK → 符号 ——

[[nodiscard]] auto latin_faces() -> std::vector<FontPreference> {
#if defined(_WIN32)
  return existing({{"C:/Windows/Fonts/segoeui.ttf", false},
                   {"C:/Windows/Fonts/arial.ttf", false}});
#elif defined(__APPLE__)
  return existing({{"/System/Library/Fonts/SFNS.ttf", false},
                   {"/System/Library/Fonts/Helvetica.ttc", false}});
#else
  return existing({{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", false},
                   {"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", false},
                   {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", false}});
#endif
}

[[nodiscard]] auto latin_faces_bold() -> std::vector<FontPreference> {
#if defined(_WIN32)
  return existing({{"C:/Windows/Fonts/segoeuib.ttf", false},
                   {"C:/Windows/Fonts/arialbd.ttf", false}});
#elif defined(__APPLE__)
  return existing({{"/System/Library/Fonts/SFNS-Bold.ttf", false}});
#else
  return existing({{"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", false},
                   {"/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", false},
                   {"/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", false}});
#endif
}

[[nodiscard]] auto cjk_faces() -> std::vector<FontPreference> {
#if defined(_WIN32)
  // 雅黑缺席时的汉字回退（不能没有 CJK 面，否则中文整串空白）。
  return existing({{"C:/Windows/Fonts/simhei.ttf", true},
                   {"C:/Windows/Fonts/msjh.ttc", true}});
#elif defined(__APPLE__)
  return existing({{"/System/Library/Fonts/PingFang.ttc", true}});
#else
  return existing({{"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", true},
                   {"/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf", true},
                   {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", true},
                   {"/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", true}});
#endif
}

[[nodiscard]] auto cjk_faces_bold() -> std::vector<FontPreference> {
#if defined(_WIN32)
  return existing({{"C:/Windows/Fonts/msjhbd.ttc", true}});
#elif defined(__APPLE__)
  return existing({{"/System/Library/Fonts/PingFang.ttc", true}});
#else
  return existing({{"/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", true},
                   {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", true},
                   {"/usr/share/fonts/truetype/noto/NotoSansCJK-Bold.ttc", true}});
#endif
}

/// 符号回退：拉丁与 CJK 字体都缺的几何/箭头/勾叉符号（✓ U+2713、✗ U+2717 这类）。
///
/// 为什么必须显式给（2026-10-04 与浏览器逐像素对照时发现）：缺的回退表现为
/// **该字符整块空白**——界面里写 "✓ 已通过" 只见「已通过」。放**最后**：
/// 它是符号字体、字面风格与正文不同，只应在别的字体都没有时才接管。
[[nodiscard]] auto symbol_faces() -> std::vector<FontPreference> {
#if defined(_WIN32)
  return existing({{"C:/Windows/Fonts/seguisym.ttf", false}});
#elif defined(__APPLE__)
  return existing({{"/System/Library/Fonts/Apple Symbols.ttf", false}});
#else
  return existing({{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", false}});
#endif
}

[[nodiscard]] auto mono_faces() -> std::vector<FontPreference> {
#if defined(_WIN32)
  return existing({{"C:/Windows/Fonts/CascadiaMono.ttf", false},
                   {"C:/Windows/Fonts/cour.ttf", false}});
#elif defined(__APPLE__)
  return existing({{"/System/Library/Fonts/Menlo.ttc", false},
                   {"/System/Library/Fonts/SFNSMono.ttf", false}});
#else
  return existing({{"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", false},
                   {"/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", false},
                   {"/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf", false}});
#endif
}

}  // namespace

auto preferred_text_fonts() -> std::vector<FontPreference> {
  std::vector<FontPreference> out = primary_text_faces();
  append(out, latin_faces());
  append(out, cjk_faces());
  append(out, symbol_faces());
  return out;
}

auto preferred_text_fonts_bold() -> std::vector<FontPreference> {
  // **与常规链逐位同序**：`find_face(..., bold)` 按下标配档。
  std::vector<FontPreference> out = primary_text_faces_bold();
  append(out, latin_faces_bold());
  append(out, cjk_faces_bold());
  return out;
}

auto preferred_mono_fonts() -> std::vector<FontPreference> {
  std::vector<FontPreference> out = primary_mono_faces();
  append(out, mono_faces());
  return out;
}

}  // namespace st::platform
