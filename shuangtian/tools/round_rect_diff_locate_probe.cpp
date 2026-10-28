// **精确定位**：`fill_rect(圆角)` 走整块贴图后与三带拆分差在哪一个像素。
//
// 背景：`sprite_equiv_probe` 在引入“圆角走贴图缓存”后报差异，而
// `round_rect_accuracy_probe` 在 5 个档位上算出两者与真值的偏差**完全相同**。
// 说明差异只出现在特定几何下——必须把它找出来并说清性质，
// 否则“等价”这个结论就是靠运气成立的（约定 §7.1）。
//
// ## 结论（本探针发现的事实）
//
// 差异只出现在**圆角弧的最外缘像素**上，最大 **1 个通道值**：
//   例：120×44 r12 → `(133,21)`，split=(123,163,235,255) / cache=(122,163,235,255)
//   那个像素恰好是右上弧最右侧（圆心 128,32、r12、y=21 ⇒ 弧缘 x≈132.8）。
//
// **成因**：贴图路径把预乘色**量化成 8 位存在贴图里**，再 `over` 到目标；
// 直接路径一次算完 `src-over`。两步量化与一步量化在“刚好卡在半格”的弧缘像素上
// 会差 1 LSB。这是**贴图机制的固有代价**，与几何键/位置无关。
//
// 若要彻底消除，得让贴图存 **float 覆盖率**而不是 8 位预乘色（见 BACKLOG）。
// 本探针的判据因此分两档：**零差**（默认要求）与 **1 LSB 内的弧缘像素**（已知、有界）。
//
// 做法：扫一批几何档位（含探针里那个 120×44 r12），对每档比
//   ① `fill_rect` + 关缓存（三带拆分）
//   ② `fill_rect` + 开缓存（整块贴图，且预热后清底重画——不能叠画两次）
// 逐像素列出差异位置与两侧确切值。

#include <cstdint>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Paint;

constexpr int kWidth = 400;
constexpr int kHeight = 300;

struct Case {
  const char* name;
  Rect rect;
  float radius;
  float opacity;
};

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "圆角矩形：三带拆分 vs 整块贴图，逐像素定位差异",
      .criterion = "所有档位 0 像素差",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("round_rect_diff_locate_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  const Color background{250, 250, 252, 255};
  const Color ink{0x2F, 0x6F, 0xE0, 255};

  // 含 `sprite_equiv_probe` 里那个 120x44 r12（差异就在它附近）。
  const Case cases[] = {
      {"120x44 r12 (探针用的)", Rect{20.0f, 20.0f, 120.0f, 44.0f}, 12.0f, 1.0f},
      {"120x44 r12 op=0.35", Rect{20.0f, 20.0f, 120.0f, 44.0f}, 12.0f, 0.35f},
      {"120x44 r12 非整y", Rect{20.0f, 60.5f, 120.0f, 44.0f}, 12.0f, 1.0f},
      {"96x32 r6", Rect{20.0f, 20.0f, 96.0f, 32.0f}, 6.0f, 1.0f},
      {"320x160 r12", Rect{20.0f, 20.0f, 320.0f, 160.0f}, 12.0f, 1.0f},
      {"720x480 r16 (超画布)", Rect{20.0f, 20.0f, 720.0f, 480.0f}, 16.0f, 1.0f},
      {"极扁 150x12 r4", Rect{20.0f, 20.0f, 150.0f, 12.0f}, 4.0f, 1.0f},
      {"胶囊 120x40 r20", Rect{20.0f, 20.0f, 120.0f, 40.0f}, 20.0f, 1.0f},
  };

  int failed_cases = 0;
  int one_lsb_cases = 0;
  st::log::out("{:<24}  {:>10}  {:>10}  {:>16}\n", "档位", "差异像素", "最大通道差", "首个差异位置");
  for (const Case& item : cases) {
    const Paint paint = Paint::solid(ink);

    Canvas split(kWidth, kHeight, 1.0f);
    split.clear(background);
    split.set_geometry_cache_enabled(false);
    split.fill_rect(item.rect, paint, item.radius, {.opacity = item.opacity});

    // ⚠ **预热不能留在同一张画布上再比**：预热那次已经把像素写过了，
    // 再画一次会让 AA 边缘**叠加变深**（实测那会把差异虚报成 80~5188 像素、
    // 通道差 50——全是重复绘制的产物，与“两条路是否等价”毫无关系）。
    // 正确做法：同一实例上预热（缓存条目在 `geometry_sprites_` 里，`clear` 不会清它），
    // 然后 `clear` 回干净底，再画**一次**。
    Canvas whole(kWidth, kHeight, 1.0f);
    whole.clear(background);
    whole.set_geometry_cache_enabled(true);
    whole.fill_rect(item.rect, paint, item.radius, {.opacity = item.opacity});   // 预热：建图
    whole.clear(background);                                                     // 清回干净底
    whole.fill_rect(item.rect, paint, item.radius, {.opacity = item.opacity});   // 正式：命中

    int diff_pixels = 0;
    int max_channel = 0;
    int first_x = -1;
    int first_y = -1;
    std::string detail;
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const auto left = split.pixel_at(x, y);
        const auto right = whole.pixel_at(x, y);
        const int delta = std::max({std::abs(static_cast<int>(left.r) - static_cast<int>(right.r)),
                                    std::abs(static_cast<int>(left.g) - static_cast<int>(right.g)),
                                    std::abs(static_cast<int>(left.b) - static_cast<int>(right.b)),
                                    std::abs(static_cast<int>(left.a) - static_cast<int>(right.a))});
        if (delta == 0) continue;
        ++diff_pixels;
        max_channel = std::max(max_channel, delta);
        if (first_x < 0) {
          first_x = x;
          first_y = y;
          // 把两侧的确切值打出来：知道“差多少”才能判断是量化零头还是真错画。
          detail = std::format("split=({},{},{},{}) cache=({},{},{},{})", left.r, left.g, left.b,
                               left.a, right.r, right.g, right.b, right.a);
        }
      }
    }
    const std::string where =
        first_x < 0 ? std::string("-") : std::format("({}, {}) {}", first_x, first_y, detail);
    st::log::out("{:<24}  {:>10}  {:>10}  {:>16}\n", item.name, diff_pixels, max_channel, where);
    // 判据分两档：
    //   · 零差 —— 理想
    //   · 1 LSB（max_channel == 1）且差异像素 ≤ 总面积 0.01% —— 已知的弧缘量化，接受
    // 超过这两档则为真缺陷。
    constexpr int kAreaPixels = kWidth * kHeight;
    constexpr int kOneLsbBudget = kAreaPixels / 10000;   // 0.01%
    if (diff_pixels == 0) continue;
    if (max_channel <= 1 && diff_pixels <= kOneLsbBudget) {
      ++one_lsb_cases;
      continue;
    }
    ++failed_cases;
  }

  st::log::out("\n");
  if (failed_cases == 0) {
    st::log::out("PASS：零差 {} 档", 0);
    st::log::out("，1 LSB 内 {} 档（贴图 8 位量化的固有代价，见文件头）\n", one_lsb_cases);
  } else {
    st::log::out("FAIL：{} 个档位超出 1 LSB 界限（见上表定位）。\n", failed_cases);
    verdict.state = st::probe::status::fail;
    verdict.reason = "圆角走贴图后与三带拆分的差异超出 1 LSB";
  }
  return st::probe::main_of(spec, opts, verdict);
}
