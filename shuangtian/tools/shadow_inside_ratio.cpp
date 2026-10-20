// 阴影贴图在「卡片内部」到底有多少非零像素（决定 `covered_region` 跳过值不值）。
//
// 背景：`shadow_covered_probe` 实测双层路径「传 covered_region 慢 13.5%」，
// 真实界面 A/B 也测不出收益（±0.5%）。本探针回答**为什么**：
//
//   `covered_region` 省下的是「落在卡片 box 内部」的阴影合成。而阴影贴图是
//   绕在卡片外侧的（模糊扩散），卡片内部的贡献本就极少——若内部非零像素占比很低，
//   那"跳过"省下的混合量就很小，抵不上每行 `sqrt`+分段的开销。
//
// 判据：只报数（这是解释性探针，没有合格线）。

#include <cstdint>
#include <format>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;

/// 数「卡片 box 内部」与「阴影总区域」里的非零像素。
struct Coverage {
  std::size_t inside_nonzero{0};
  std::size_t inside_area{0};
  std::size_t outside_nonzero{0};
  std::size_t outside_area{0};
};

[[nodiscard]] auto measure(float card_h, float radius, float blur) -> Coverage {
  constexpr int kWidth = 400;
  constexpr int kHeight = 400;
  const Rect box{100.0f, 100.0f, 140.0f, card_h};

  Canvas base = Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  base.clear(Color::rgb(0xFF, 0xFF, 0xFF));
  Canvas drawn = Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  drawn.clear(Color::rgb(0xFF, 0xFF, 0xFF));
  // 双层阴影（与 `element.cpp` 的可见形态一致：关键层 + 环境层）。
  drawn.draw_shadow_layered(box, radius, Color{0, 0, 0, 60}, blur, Point{0.0f, 2.0f},
                            Color{0, 0, 0, 24}, blur * 2.0f, Point{0.0f, 0.0f});

  const float padding = blur * 2.0f * 2.0f + 4.0f;
  const st::math::IntRect region = box.inflate(padding).round_out();
  Coverage out{};
  for (int y = region.y; y < region.bottom(); ++y) {
    for (int x = region.x; x < region.right(); ++x) {
      if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) continue;
      const Color a = base.pixel_at(x, y);
      const Color b = drawn.pixel_at(x, y);
      const bool changed = a.r != b.r || a.g != b.g || a.b != b.b;
      // 「内部」按矩形算（保守：不含四角缺口），与 `element.cpp` 传 covered_region 的口径一致。
      const bool inside = static_cast<float>(x) >= box.x && static_cast<float>(x) < box.right() &&
                          static_cast<float>(y) >= box.y && static_cast<float>(y) < box.bottom();
      if (inside) {
        ++out.inside_area;
        if (changed) ++out.inside_nonzero;
      } else {
        ++out.outside_area;
        if (changed) ++out.outside_nonzero;
      }
    }
  }
  return out;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "阴影在「卡片内部」的非零像素占比（解释 covered_region 为何无收益）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("shadow_inside_ratio", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  for (const float card_h : {48.0f, 200.0f}) {
    const Coverage cov = measure(card_h, 8.0f, 16.0f);
    const auto pct = [](std::size_t part, std::size_t whole) {
      return whole == 0 ? 0.0 : static_cast<double>(part) / static_cast<double>(whole) * 100.0;
    };
    st::print("卡片高 {:5.0f}px\n", static_cast<double>(card_h));
    st::print("  box 内部：非零 {:6d} / 面积 {:6d}  = {:5.2f}%\n", cov.inside_nonzero,
              cov.inside_area, pct(cov.inside_nonzero, cov.inside_area));
    st::print("  box 外部：非零 {:6d} / 面积 {:6d}  = {:5.2f}%\n", cov.outside_nonzero,
              cov.outside_area, pct(cov.outside_nonzero, cov.outside_area));
    const std::size_t total_nonzero = cov.inside_nonzero + cov.outside_nonzero;
    st::print("  → 全部阴影像素中，落在内部的占 {:5.2f}%（这才是「跳过」能省的）\n\n",
              pct(cov.inside_nonzero, total_nonzero));
  }
  return st::probe::main_of(spec, opts, verdict);
}
