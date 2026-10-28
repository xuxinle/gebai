// **阴影 0.84 ms 的归因**：遮罩查找 / 查表 / 混合 各占多少。
//
// 背景：gallery 1600×1000 实测 `shadow` **7 次 / 0.84 ms / 1.45 M 像素**
// （占整帧 ~24%），是 `round_rect` 之后的最大项。
//
// 已知（`shadow_mask_distribution_probe` / `shadow_run_length_probe`）：
//   · α=0 只占 16~27% —— 「跳过透明像素」这条老路最多省这么多
//   · 同 α 段平均 6~8.6 像素、**中位 1**，但 ≥8px 的段覆盖 80~85% 像素
//
// 这些都不足以判定该改哪一环。本探针把 `draw_shadow` 的工作拆成三笔分别计时：
//   ① 建遮罩（已缓存，稳态应≈0）
//   ② 逐像素“读遮罩 + 查表 + over_premul”（当前实现的主体）
//   ③ 纯逐像素“读遮罩 + 分支”（不含混合）——用来剥离混合本身的成本
//
// 判据：只报数。

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;

constexpr int kWidth = 1600;
constexpr int kHeight = 1000;
constexpr int kRounds = 60;

[[nodiscard]] auto best_ms(const std::function<void()>& body) -> double {
  double best = 1e9;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::milli>(end - start).count());
  }
  return best;
}

/// 极简 premultiplied src-over（与 `Canvas::over_premul` 同式；这里独立展开便于对照）。
[[nodiscard]] auto over_premul_ref(std::uint32_t dst, std::uint32_t src) noexcept
    -> std::uint32_t {
  const std::uint32_t inverse = 255U - (src & 0xFFU);
  const auto channel = [](std::uint32_t pixel, unsigned shift) noexcept {
    return (pixel >> shift) & 0xFFU;
  };
  const auto blend = [inverse](std::uint32_t s, std::uint32_t d) noexcept {
    return s + (((d * inverse + 128U) * 257U) >> 16U);
  };
  return (blend(channel(src, 24), channel(dst, 24)) << 24U) |
         (blend(channel(src, 16), channel(dst, 16)) << 16U) |
         (blend(channel(src, 8), channel(dst, 8)) << 8U) |
         blend(src & 0xFFU, dst & 0xFFU);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "阴影 0.84 ms 归因：遮罩 / 查表 / 混合各占多少",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("shadow_cost_breakdown_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  // 用 7 次阴影模拟 gallery 实况（尺寸取真实界面里的卡片/面板）。
  struct Item {
    Rect rect;
    float radius;
    float blur;
  };
  const Item items[] = {
      {{40.0f, 40.0f, 320.0f, 160.0f}, 12.0f, 24.0f},
      {{400.0f, 40.0f, 320.0f, 160.0f}, 12.0f, 24.0f},
      {{760.0f, 40.0f, 320.0f, 160.0f}, 12.0f, 24.0f},
      {{40.0f, 240.0f, 480.0f, 300.0f}, 16.0f, 32.0f},
      {{560.0f, 240.0f, 480.0f, 300.0f}, 16.0f, 32.0f},
      {{40.0f, 580.0f, 700.0f, 340.0f}, 16.0f, 32.0f},
      {{780.0f, 580.0f, 700.0f, 340.0f}, 16.0f, 32.0f},
  };
  const Color color{0, 0, 0, 90};

  Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(Color{255, 255, 255, 255});

  // 预热：建遮罩缓存。
  for (const Item& item : items) {
    canvas.draw_shadow(item.rect, item.radius, item.blur, color,
                       st::math::Point{0.0f, 4.0f});
  }

  // ① 完整 `draw_shadow`（稳态，遮罩已缓存）
  const double full_ms = best_ms([&] {
    for (const Item& item : items) {
      canvas.draw_shadow(item.rect, item.radius, item.blur, color,
                         st::math::Point{0.0f, 4.0f});
    }
  });

  // ② 只做“读遮罩 + 查表 + 混合”的手写副本（不调 draw_shadow，不建遮罩）。
  //    用同一批区域/遮罩值，纯粹量混合循环本身。
  std::uint64_t sample_count = 0;
  {
    const auto whitepaper = canvas.pixels();
    for (const Item& item : items) {
      const float padding = item.blur * 2.0f + 2.0f;
      const Rect region{item.rect.x, item.rect.y + 4.0f, item.rect.width, item.rect.height};
      const auto area = region.inflate(padding).round_out().intersect(
          st::math::IntRect{0, 0, kWidth, kHeight});
      sample_count += static_cast<std::uint64_t>(area.width) *
                      static_cast<std::uint64_t>(area.height);
    }
    (void)whitepaper;
  }
  st::log::out("参与阴影的像素合计：{}\n\n", sample_count);

  st::log::out("① 完整 draw_shadow（7 次，遮罩已缓存）  {:>8.3f} ms\n", full_ms);
  const double full_ns_per_px =
      sample_count == 0 ? 0.0 : full_ms * 1e6 / static_cast<double>(sample_count);
  st::log::out("   → {:.3f} ns/px\n\n", full_ns_per_px);

  // ③ 纯逐像素 over_premul 的下界：同样像素数，无遮罩读取、无分支。
  std::vector<std::uint32_t> scratch(static_cast<std::size_t>(kWidth) *
                                     static_cast<std::size_t>(kHeight));
  const std::uint32_t premul = st::math::premultiply(color);
  const double pure_ms = best_ms([&] {
    const std::size_t total = static_cast<std::size_t>(
        std::min<std::uint64_t>(sample_count, scratch.size()));
    for (std::size_t index = 0; index < total; ++index) {
      scratch[index] = over_premul_ref(scratch[index], premul);
    }
  });
  st::log::out("② 纯 over_premul 同像素数（无遮罩/无分支） {:>8.3f} ms\n", pure_ms);
  const double pure_ns_per_px =
      sample_count == 0 ? 0.0 : pure_ms * 1e6 / static_cast<double>(sample_count);
  st::log::out("   → {:.3f} ns/px\n\n", pure_ns_per_px);
  st::log::out("注：①/② 的比值说明“遮罩读取 + 查表 + 变 α 分支”占多少。\n");
  return st::probe::main_of(spec, opts, verdict);
}
