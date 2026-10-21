// **`covered_region` 有没有真的生效**——直接问后端。
//
// ## 背景
//
// `Element::paint_box` 里阴影之后紧跟着用 `background` 铺满 `box`（不透明时
// 阴影落在 `box` 内的部分必然被完全覆盖）。为此给 `draw_shadow_layered` 传了
// `DrawOptions::covered_region`。
//
// **但改完实测耗时没变**（96 卡片 1609 → 1594 µs，噪声内）。只有三种可能：
//   ① 条件没满足（`background.a != 255` 或 `opacity < 0.999`）；
//   ② 条件满足了，但跳过量达不到 `kMinSkipPixels`（40 px），后端主动不跳；
//   ③ 条件满足且跳了，但省下的不值得（该场景本来就是贴图为主）。
//
// ## 这个探针做什么
//
// 不走 UI 层——**直接调 `draw_shadow_layered`**，分别传 / 不传 `covered_region`，
// 量差值。这样把“UI 层条件没满足”与“后端跳了但没用”分开：
//
//   · 直接传 covered_region 也没变快 → 后端侧的问题（②/③）
//   · 直接传变快了 → UI 层的条件没满足（①），该查 `background.a`
//
// 判据：报两者差值。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"

namespace {

using st::math::Color;

constexpr int kRounds = 200;
constexpr int kColumns = 8;
constexpr float kCardWidth = 140.0f;
constexpr float kCardHeight = 48.0f;
constexpr float kGapX = 8.0f;
constexpr float kGapY = 12.0f;
constexpr float kRadius = 8.0f;

[[nodiscard]] auto best_ns(const std::function<void()>& body) -> double {
  double best = 1e18;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(end - start).count();
    if (ns < best) best = ns;
  }
  return best;
}

[[nodiscard]] auto card_at(int index) -> st::math::Rect {
  const int row = index / kColumns;
  const int column = index % kColumns;
  return st::math::Rect{40.0f + static_cast<float>(column) * (kCardWidth + kGapX),
                        40.0f + static_cast<float>(row) * (kCardHeight + kGapY), kCardWidth,
                        kCardHeight};
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "covered_region 是否真的生效（绕过 UI 层直接测后端）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("covered_region_verify_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::raster::Canvas canvas(1280, 800, 1.0f);
  canvas.clear(Color{0xF7, 0xF8, 0xFA, 255});

  const Color key{0, 0, 0, 40};
  const Color ambient{0, 0, 0, 20};

  const auto draw_all = [&](bool with_covered) {
    for (int index = 0; index < 96; ++index) {
      const st::math::Rect card = card_at(index);
      auto options = st::raster::DrawOptions{.opacity = 1.0f};
      if (with_covered) {
        options.covered_region = card;
        options.covered_radius = kRadius;
      }
      canvas.draw_shadow_layered(card, kRadius, key, 12.0f, st::math::Point{0.0f, 2.0f}, ambient,
                                 24.0f, st::math::Point{0.0f, 6.0f}, options);
    }
  };

  draw_all(false);
  draw_all(true);   // 预热

  const double without_ns = best_ns([&] { draw_all(false); });
  const double with_ns = best_ns([&] { draw_all(true); });

  st::print("96 张卡片阴影（140×48 圆角 8，两层）\n\n");
  st::print("不传 covered_region   {:>9.1f} µs\n", without_ns / 1000.0);
  st::print("传 covered_region     {:>9.1f} µs   → {:.2f}×\n", with_ns / 1000.0,
            with_ns > 0.0 ? without_ns / with_ns : 0.0);
  st::print("\n判读：\n");
  const double gain = without_ns / std::max(with_ns, 1.0);
  if (gain < 1.05) {
    st::print("  传了几乎没变（{:.2f}×）→ **后端没真正跳**。\n", gain);
    st::print("  可能：跳过量未达 `kMinSkipPixels`(40px)，或贴图本身已按 spans 只覆盖非零区\n");
    st::print("  （阴影像素大多本来就在卡片外侧，可跳的部分很小）。\n");
  } else {
    st::print("  传了快 {:.2f}× → 后端确实跳了；那 UI 层就该生效，\n", gain);
    st::print("  若实测没变则是 UI 层条件没满足（查 `background.a`）。\n");
  }
  return st::probe::main_of(spec, opts, verdict);
}
