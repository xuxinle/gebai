// **阴影的遮罩值分布**：那 1.45 M 像素里到底有多少是真需要画的。
//
// 背景（gallery 1600×1000 实测）：`shadow` 7 次调用 / **0.84 ms** / **1.45 M 像素**
// （占整帧 ~24%），而 `draw_shadow` 已有一条 `if (mask_byte == 0) continue;` 的跳过。
//
// 所以关键问题不是“有没有跳过”，而是**跳过能省多少**：
//   · 若大量像素是 0（全透明） → 现在的逐像素分支已能跳过，优化空间在“整行/整块跳过”
//   · 若绝大多数是 1~30（模糊出的极淡值） → 跳不过，真正的成本是**逐像素混合本身**
//
// 这决定下一步该做什么，不该靠猜。
//
// 判据：只报数（分布直方图 + 各类占比）。

#include <array>
#include <cstdint>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;

/// 一个典型的卡片阴影参数（blur 24 / 偏移 (0,4) 之类）。
struct Params {
  const char* name;
  float width;
  float height;
  float radius;
  float blur;
  float offset_y;
};

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "阴影遮罩值分布（决定“跳过透明像素”还能省多少）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("shadow_mask_distribution_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  constexpr int kWidth = 1600;
  constexpr int kHeight = 1000;
  const Params params[] = {
      {"卡片 320x160 r12 blur24", 320.0f, 160.0f, 12.0f, 24.0f, 4.0f},
      {"小卡 200x120 r8 blur16", 200.0f, 120.0f, 8.0f, 16.0f, 2.0f},
      {"大面板 720x480 r16 blur32", 720.0f, 480.0f, 16.0f, 32.0f, 6.0f},
  };

  st::log::out("{:<26}  {:>12}  {:>10}  {:>10}  {:>10}  {:>10}\n", "参数", "区域像素",
            "α=0 占比", "α≤8 占比", "α≤32 占比", "α>32 占比");
  for (const Params& item : params) {
    Canvas canvas(kWidth, kHeight, 1.0f);
    canvas.clear(Color{0, 0, 0, 0});
    // 画一次阴影（遮罩会进缓存，这里只关心一次性的遮罩内容）。
    const Rect rect{100.0f, 100.0f, item.width, item.height};
    canvas.draw_shadow(rect, item.radius, item.blur, Color{0, 0, 0, 90},
                       st::math::Point{0.0f, item.offset_y});

    // 遮罩区域 = 矩形外扩 blur*2+2 并偏移（与 `draw_shadow` 一致）。
    const float padding = item.blur * 2.0f + 2.0f;
    const Rect region{
        rect.x,
        rect.y + item.offset_y,
        rect.width,
        item.height,
    };
    const Rect inflated = region.inflate(padding);
    const auto area = inflated.round_out().intersect(
        st::math::IntRect{0, 0, kWidth, kHeight});

    std::uint64_t total = 0;
    std::uint64_t zero = 0;
    std::uint64_t le8 = 0;
    std::uint64_t le32 = 0;
    std::uint64_t gt32 = 0;
    for (int y = area.y; y < area.bottom(); ++y) {
      for (int x = area.x; x < area.right(); ++x) {
        ++total;
        const auto pixel = canvas.pixel_at(x, y);
        const int alpha = pixel.a;
        if (alpha == 0) {
          ++zero;
        } else if (alpha <= 8) {
          ++le8;
        } else if (alpha <= 32) {
          ++le32;
        } else {
          ++gt32;
        }
      }
    }
    const auto pct = [total](std::uint64_t value) {
      return total == 0 ? 0.0 : static_cast<double>(value) * 100.0 / static_cast<double>(total);
    };
    st::log::out("{:<26}  {:>12}  {:>9.1f}%  {:>9.1f}%  {:>9.1f}%  {:>9.1f}%\n", item.name, total,
              pct(zero), pct(le8), pct(le32), pct(gt32));
  }
  st::log::out("\n注：“α=0 占比”那一列是现有 `if (mask_byte == 0) continue;` 已经能省的；\n");
  st::log::out("    剩下那些极淡值（α≤32）**跳不过**，它们的成本是逐像素混合本身。\n");
  return st::probe::main_of(spec, opts, verdict);
}
