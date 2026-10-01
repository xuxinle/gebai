/// hinting 收益**测量台**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 目的：在做任何实现之前先**量出每条路径的收益**，而不是凭空选“最大”的那个。
/// 实测结论（2026-10-01）：`TARGET_LIGHT/NORMAL`（= 读字体自带 hinting 指令）与无 hinting
/// 几乎一致（拉丁 3.2%→3.2%、中文 4.3%），**中文 CFF 上 TARGET_MONO 也无效**；
/// 唯一真正有效的是 **auto-hinter 式的几何网格拟合**（拉丁 49.0%、中文 20.6%）。
/// 因此“给中文实现 TrueType 指令解释器”是白工，方向是几何拟合（详见 `docs/BACKLOG.md` P1）。
///
/// 判据（精确、无歧义）：**竖笔画边缘是否落在整数像素网格上**。
///   FreeType 的 hinting 把轮廓点吸附到 26.6 定点网格（1/64 像素）；
///   “边缘在整数上” ⇔ `x` 离最近整数 < 1/32 px。一个不在整数上的边缘，
///   两侧必然各产生一个部分覆盖像素 —— 那就是“缓坡”。
///   所以只需看**轮廓点**，不必渲染位图，也就不受抗锯齿模式干扰。
///
/// 手工编译（FreeType 是**分析用**依赖，框架本体仍零第三方依赖）：
///
/// ```bash
/// cd shuangtian && ./build/bin/st build st --profile dev
/// OBJS=$(ls build/dev/obj/*.o | grep -v -E "(main\.cpp\.o|_test\.cpp\.o|test_runner\.cpp\.o|_vendor_|examples_)")
/// g++ -std=c++20 -O1 -Iinclude -Ithird_party -I/usr/include/freetype2 \
///     tools/hinting_gain_probe.cpp $OBJS -o /tmp/hgain -lfreetype -lpthread -ldl -lm
/// /tmp/hgain /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc 2
/// ```
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include "st/core/print.hpp"
#include "st/core/string.hpp"

namespace {

constexpr double kPixelSize = 16.88;  // 13.5 逻辑 px @1.25 DPI（与 stem_phase_probe 同口径）

const char* const kCjk =
    "霜天自绘概览组件数据控制通道关于按钮表单密码提交重置进度状态窗口字体渲染"
    "一二三四五六七八九十日月田目国回";
const char* const kLatin =
    "Handgloves Illegible abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789";

struct Options {
  bool hinting{true};
  FT_Int32 target{FT_LOAD_TARGET_NORMAL};
  /// 强制走 FreeType 的 **auto-hinter**（跳过字体自带指令）——
  /// 这是「自己实现 snapping」时要对齐的参考基准：它专为无 hinting 字体设计。
  bool force_autohint{false};
};

struct Stats {
  int vertical_edges{0};
  int on_grid{0};        // 边缘落在整数像素网格（误差 < 1/32 px）
  double phase_sum{0};   // 到最近整数网格的距离（px）之和
  int near_half{0};      // 落在"最糊相位" |frac-0.5| < 0.3
};

/// 轮廓点是否近垂直边（相邻两点的 x 变化远小于 y 变化）。
[[nodiscard]] auto measure_face(FT_Face face, const std::u32string& samples, const Options& options)
    -> Stats {
  Stats stats;
  FT_Int32 flags = FT_LOAD_NO_BITMAP;  // 只要轮廓
  if (!options.hinting) flags |= FT_LOAD_NO_HINTING;
  if (options.force_autohint) flags |= FT_LOAD_FORCE_AUTOHINT;
  flags |= options.target;

  for (const char32_t codepoint : samples) {
    const FT_UInt index = FT_Get_Char_Index(face, static_cast<FT_ULong>(codepoint));
    if (index == 0) continue;
    if (FT_Load_Glyph(face, index, flags) != 0) continue;
    if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) continue;
    const FT_Outline& outline = face->glyph->outline;
    if (outline.n_points < 2) continue;

    // 按轮廓（contour）遍历，取相邻点对
    for (int contour = 0; contour < outline.n_contours; ++contour) {
      const int begin = contour == 0 ? 0 : outline.contours[contour - 1] + 1;
      const int end = outline.contours[contour];  // 含
      const int count = end - begin + 1;
      if (count < 4) continue;
      for (int step = 0; step < count; ++step) {
        const FT_Vector& a = outline.points[begin + step];
        const FT_Vector& b = outline.points[begin + (step + 1) % count];
        const double dx = static_cast<double>(b.x - a.x) / 64.0;
        const double dy = static_cast<double>(b.y - a.y) / 64.0;
        // 近垂直：|dx| 很小、|dy| 够长（真正的竖笔画边）
        if (std::abs(dx) > 0.20 || std::abs(dy) < 1.0) continue;
        // **只取真曲线点**：竖笔画的直边两端都是 On-curve 点（控制点不构成几何边）
        const auto tag_a = FT_CURVE_TAG(outline.tags[begin + step]);
        const auto tag_b = FT_CURVE_TAG(outline.tags[begin + (step + 1) % count]);
        if (tag_a != FT_CURVE_TAG_ON && tag_b != FT_CURVE_TAG_ON) continue;

        const double x = static_cast<double>(a.x) / 64.0;
        ++stats.vertical_edges;
        const double nearest = std::round(x);
        const double distance = std::abs(x - nearest);
        stats.phase_sum += distance;
        if (distance < 1.0 / 32.0) ++stats.on_grid;
        const double fraction = x - std::floor(x);
        if (fraction > 0.2 && fraction < 0.8) ++stats.near_half;
      }
    }
  }
  return stats;
}

void report(const char* label, const Stats& stats) {
  const double mean = stats.vertical_edges > 0 ? stats.phase_sum / stats.vertical_edges : 0.0;
  const double on_grid =
      stats.vertical_edges > 0 ? 100.0 * stats.on_grid / stats.vertical_edges : 0.0;
  const double half = stats.vertical_edges > 0 ? 100.0 * stats.near_half / stats.vertical_edges : 0.0;
  st::print("  {:<38} 竖边 {:5}  在整数网格上 {:5.1f}%  平均离网格 {:.3f} px  最糊相位 {:5.1f}%\n",
            label, stats.vertical_edges, on_grid, mean, half);
}

}  // namespace

int main(int argc, char** argv) {
  FT_Library library = nullptr;
  if (FT_Init_FreeType(&library) != 0) {
    st::print("FreeType 初始化失败\n");
    return 1;
  }
  const std::string path = argc > 1 ? argv[1] : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
  const int face_index = argc > 2 ? std::stoi(argv[2]) : 0;
  FT_Face face = nullptr;
  if (FT_New_Face(library, path.c_str(), face_index, &face) != 0) {
    st::print("打开字体失败：{}\n", path);
    return 1;
  }
  FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(std::lround(kPixelSize)));

  // 轮廓类型：直接看 sfnt 表 tag（比宏更稳）
  bool has_cff = false;
  bool has_glyf = false;
  {
    const FT_ULong cff_tag = FT_MAKE_TAG('C', 'F', 'F', ' ');
    const FT_ULong glyf_tag = FT_MAKE_TAG('g', 'l', 'y', 'f');
    FT_ULong length = 0;
    if (FT_Load_Sfnt_Table(face, cff_tag, 0, nullptr, &length) == 0 && length > 0) has_cff = true;
    if (FT_Load_Sfnt_Table(face, glyf_tag, 0, nullptr, &length) == 0 && length > 0) has_glyf = true;
  }
  const std::string kind = has_cff && !has_glyf
                               ? "CFF 轮廓（只有 stem hint）"
                               : (has_glyf ? "TrueType 轮廓（有完整指令）" : "其他");
  st::print("\n=== {} (face {})  {} · pixel size {:.2f} ===\n",
            path.substr(path.find_last_of('/') + 1), face_index, kind, kPixelSize);

  const bool cjk_font = has_cff;
  const std::u32string samples = st::utf8_decode(argc > 3 ? argv[3] : (cjk_font ? kCjk : kLatin));
  st::print("  样本：{} 个字形\n", samples.size());

  Options none;
  none.hinting = false;
  Options light;
  light.hinting = true;
  light.target = FT_LOAD_TARGET_LIGHT;
  Options full;
  full.hinting = true;
  full.target = FT_LOAD_TARGET_NORMAL;
  Options mono;
  mono.hinting = true;
  mono.target = FT_LOAD_TARGET_MONO;
  // auto-hinter：专为无 hinting 字体设计的自动网格拟合（自研 snapping 的参考基准）
  Options auto_normal;
  auto_normal.force_autohint = true;
  auto_normal.target = FT_LOAD_TARGET_NORMAL;
  Options auto_light;
  auto_light.force_autohint = true;
  auto_light.target = FT_LOAD_TARGET_LIGHT;

  report("① 无 hinting（现框架等价物）", measure_face(face, samples, none));
  report("② TARGET_LIGHT（字体自带轻 hint）", measure_face(face, samples, light));
  report("③ 完整 TT 指令 hinting", measure_face(face, samples, full));
  report("④ TARGET_MONO（强制整数网格）", measure_face(face, samples, mono));
  report("⑤ auto-hinter（normal）", measure_face(face, samples, auto_normal));
  report("⑥ auto-hinter（light）", measure_face(face, samples, auto_light));

  FT_Done_Face(face);
  FT_Done_FreeType(library);
  return 0;
}
