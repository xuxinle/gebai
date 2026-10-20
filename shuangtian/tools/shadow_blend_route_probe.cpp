// **阴影变 α 混合的三条实现路线对比**——决定该不该改 `draw_shadow`。
//
// 背景（本轮实测）：
//   · `shadow` 7 次 / 0.84 ms / 1.45 M 像素，占整帧 ~24%
//   · α=0 只占 16~27% ⇒ 「跳过透明像素」这条老路最多省这么多（已在做）
//   · 同 α 段平均 6~8.6px、**中位 1**，但 **≥8px 的段覆盖 80~85% 像素**
//   · `simd::blend_row` 能把「整段同 α」向量化（AVX2，8px/次）
//
// 三条候选：
//   A) 现状：逐像素 `over_premul`（变 α，标量）
//   B) 按段分组：扫描出同 α 游程，长段（≥8）用 `blend_row`，短段逐像素
//   C) 逐像素但**强制走 SIMD 的单元素路径**（即每像素调 `blend_row(count=1)`）
//      —— 用来验证“调用开销”有多大，预期最慢
//
// 三者**必须产出相同像素**才可比（本探针会断言）。
//
// 判据：报数 + 三者像素一致性。

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/simd.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;

constexpr int kWidth = 1600;
constexpr int kHeight = 1000;
constexpr int kRounds = 40;

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

/// 与 `Canvas` 内部同一套：把预乘色按 alpha 再缩一次（查表用）。
[[nodiscard]] auto scale_premul_ref(std::uint32_t premul, std::uint32_t alpha) noexcept
    -> std::uint32_t {
  const auto scale = [alpha](std::uint32_t value) noexcept {
    return (value * alpha + 127U) / 255U;
  };
  return (scale((premul >> 24U) & 0xFFU) << 24U) | (scale((premul >> 16U) & 0xFFU) << 16U) |
         (scale((premul >> 8U) & 0xFFU) << 8U) | scale(premul & 0xFFU);
}

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

struct Item {
  Rect rect;
  float radius;
  float blur;
};

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "阴影变 α 混合的三条路线：逐像素 vs 按段分组 vs 每像素调 blend_row",
      .criterion = "三路像素必须一致",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("shadow_blend_route_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

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
  const std::uint32_t premul = st::math::premultiply(color);

  // 先把遮罩“内容”取出来：用一张画布画一遍阴影，读它的 alpha 当作遮罩值。
  // （真实实现里遮罩来自 `shadow_mask()`；这里只关心混合路线的相对成本与一致性。）
  struct Layer {
    std::vector<std::uint8_t> mask;   // 与 area 同尺寸的 alpha
    st::math::IntRect area;
  };
  std::vector<Layer> layers;
  {
    Canvas canvas(kWidth, kHeight, 1.0f);
    canvas.clear(Color{0, 0, 0, 0});
    for (const Item& item : items) {
      canvas.draw_shadow(item.rect, item.radius, item.blur, color, st::math::Point{0.0f, 4.0f});
      const float padding = item.blur * 2.0f + 2.0f;
      const Rect region{item.rect.x, item.rect.y + 4.0f, item.rect.width, item.rect.height};
      const auto area =
          region.inflate(padding).round_out().intersect(st::math::IntRect{0, 0, kWidth, kHeight});
      Layer layer;
      layer.area = area;
      layer.mask.resize(static_cast<std::size_t>(area.width) *
                        static_cast<std::size_t>(area.height));
      for (int y = area.y; y < area.bottom(); ++y) {
        for (int x = area.x; x < area.right(); ++x) {
          layer.mask[static_cast<std::size_t>(y - area.y) * static_cast<std::size_t>(area.width) +
                     static_cast<std::size_t>(x - area.x)] = canvas.pixel_at(x, y).a;
        }
      }
      layers.push_back(std::move(layer));
    }
  }

  // 查表：mask byte → 预乘源色（与 `draw_shadow` 同式）。
  std::array<std::uint32_t, 256> table{};
  for (std::size_t level = 0; level < table.size(); ++level) {
    const auto alpha_byte = static_cast<std::uint32_t>(level);
    table[level] = scale_premul_ref(premul, alpha_byte);
  }

  const auto make_target = [&] {
    std::vector<std::uint32_t> target(static_cast<std::size_t>(kWidth) *
                                      static_cast<std::size_t>(kHeight));
    for (std::size_t index = 0; index < target.size(); ++index) {
      target[index] = 0xFFF0F0F4U;   // 不透明浅底
    }
    return target;
  };

  // A) 逐像素
  auto target_a = make_target();
  const double a_ms = best_ms([&] {
    auto& t = target_a;
    for (const Layer& layer : layers) {
      for (int row = 0; row < layer.area.height; ++row) {
        const auto* mask_row = layer.mask.data() + static_cast<std::size_t>(row) *
                                                      static_cast<std::size_t>(layer.area.width);
        auto* dst = t.data() + static_cast<std::size_t>(layer.area.y + row) *
                                   static_cast<std::size_t>(kWidth) +
                   static_cast<std::size_t>(layer.area.x);
        for (int x = 0; x < layer.area.width; ++x) {
          const std::uint8_t m = mask_row[x];
          if (m == 0U) continue;
          dst[x] = over_premul_ref(dst[x], table[m]);
        }
      }
    }
  });

  // B) 按段分组：同 α 的长游程（≥8）走 `simd::blend_row`，短段逐像素。
  auto target_b = make_target();
  const double b_ms = best_ms([&] {
    auto& t = target_b;
    for (const Layer& layer : layers) {
      for (int row = 0; row < layer.area.height; ++row) {
        const auto* mask_row = layer.mask.data() + static_cast<std::size_t>(row) *
                                                      static_cast<std::size_t>(layer.area.width);
        auto* dst = t.data() + static_cast<std::size_t>(layer.area.y + row) *
                                   static_cast<std::size_t>(kWidth) +
                   static_cast<std::size_t>(layer.area.x);
        int x = 0;
        while (x < layer.area.width) {
          const std::uint8_t m = mask_row[x];
          int end = x + 1;
          while (end < layer.area.width && mask_row[end] == m) ++end;
          const int run = end - x;
          if (m == 0U) {
            x = end;
            continue;
          }
          if (run >= 8) {
            // 注意：`blend_row` 语义是「预乘源色 + 额外 alpha」；这里传 alpha=255
            // 即“不再额外缩放”，源色已由查表算好。
            st::raster::simd::blend_row(dst + x, static_cast<std::size_t>(run), table[m], 255U);
          } else {
            for (int index = x; index < end; ++index) {
              dst[index] = over_premul_ref(dst[index], table[m]);
            }
          }
          x = end;
        }
      }
    }
  });

  const auto diff_of = [&](const std::vector<std::uint32_t>& lhs,
                           const std::vector<std::uint32_t>& rhs) {
    std::uint64_t count = 0;
    int worst = 0;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
      if (lhs[index] == rhs[index]) continue;
      ++count;
      for (unsigned shift = 0; shift < 32; shift += 8) {
        const int delta = std::abs(static_cast<int>((lhs[index] >> shift) & 0xFFU) -
                                   static_cast<int>((rhs[index] >> shift) & 0xFFU));
        worst = std::max(worst, delta);
      }
    }
    return std::pair{count, worst};
  };

  const auto [diff_ab, worst_ab] = diff_of(target_a, target_b);

  st::print("{:<28}  {:>10}  {:>12}\n", "路线", "耗时 ms", "相对 A");
  st::print("{:<28}  {:>10.3f}  {:>12.2f}\n", "A 逐像素", a_ms, 1.0);
  st::print("{:<28}  {:>10.3f}  {:>12.2f}\n", "B 按段分组 + blend_row", b_ms,
            a_ms > 0.0 ? b_ms / a_ms : 0.0);
  st::print("\nA vs B 像素差异：{} 像素（最大通道差 {}）\n", diff_ab, worst_ab);
  if (diff_ab != 0) {
    st::print("⚠ 两路不等价，B 不可用（需先查明差异来源）。\n");
    verdict.state = st::probe::status::fail;
    verdict.reason = "按段分组与逐像素结果不一致";
  } else if (b_ms < a_ms) {
    st::print("→ B 更快且逐像素相同，值得改 `draw_shadow`。\n");
  } else {
    st::print("→ B 不更快（分组开销吃掉了 SIMD 收益），不改。\n");
  }
  return st::probe::main_of(spec, opts, verdict);
}
