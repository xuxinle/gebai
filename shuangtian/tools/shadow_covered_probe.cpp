// `DrawOptions::covered_region` 的**真实收益**量化（性能轮）。
//
// 为什么需要这个探针：框架层已实现「跳过随后会被不透明底盖住的阴影像素」
// （`paint.hpp` 的契约 + `canvas.cpp` 的按行解析跨度），`docs/PERF_2026-10-19.md`
// §3.2 报的是**理想值**（15744 → ≈6500 触达像素）。但 API 是**可选参数**，
// 谁都可以选择不传——而「只接进了渲染器、调用点没接」正是本仓记录过的缺陷形态
// （`docs/PITFALLS.md`）。这个探针把「传 / 不传」放进同一份二进制里 A/B 对照：
//
//   ① **等价性**（正确性，硬判据）：不透明底 + 传 covered_region 与「不传但先画后盖」
//      必须**逐像素相同**。这是 `over(B, over(D, U)) == B` 的直接验证。
//   ② **收益**（性能）：同一场景下两种写法的耗时差 + 实际触达像素数。
//
// 判据（探针自带）：
//   - `max_channel_diff == 0`（不透明底情形必须逐像素相等，否则**不是优化而是 bug**）；
//   - 半透明底 + 传 covered_region → **必须**出现可见差异（证明后端真的跳了，
//     而不是“传了也没用”从而让等价性判据恒绿——`CONVENTIONS.md` §7.2 的桩陷阱）。
//
// 用法：
//   py -3 tools/check_probes.py 会校验本文件接入了统一外壳；
//   直接跑：build/dev/bin/shadow_covered_probe（退出码 0 = 通过）。

#include <chrono>
#include <cstdint>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::DrawOptions;
using st::raster::Paint;

constexpr int kWidth = 800;
constexpr int kHeight = 600;
constexpr int kCards = 96;              // 与 PERF_2026-10-19 §3.2 的规模一致
constexpr float kCardWidth = 140.0f;
constexpr float kCardHeight = 48.0f;
constexpr float kRadius = 8.0f;
constexpr float kBlur = 16.0f;

[[nodiscard]] auto make_canvas() -> Canvas {
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  canvas.clear(Color::rgb(0xF0, 0xF0, 0xF4));
  return canvas;
}

/// 一张卡片：阴影 → 不透明底。`use_covered` 决定是否把「随后要铺的底」告诉阴影。
void paint_card(Canvas& canvas, Rect box, bool use_covered, bool opaque_face, bool two_layer) {
  const Color shadow_color{0, 0, 0, 60};
  // 环境层（更宽、更淡）——只有双层路径才会被用到。
  const Color ambient_color{0, 0, 0, 24};
  const Color face = opaque_face ? Color::rgb(0xFF, 0xFF, 0xFF) : Color{0xFF, 0xFF, 0xFF, 128};

  DrawOptions options{};
  if (use_covered) {
    // 契约三前提：SrcOver（默认）+ opacity ≥ 0.999（默认）+ 随后真的铺不透明底。
    // 第三个由本函数保证（下面紧跟 fill_rect），而 `opaque_face` 决定它成立与否。
    options.covered_region = box;
    options.covered_radius = kRadius;
  }
  canvas.draw_shadow_layered(box, kRadius, shadow_color, kBlur, Point{0.0f, 2.0f},
                             two_layer ? ambient_color : Color{0, 0, 0, 0},
                             two_layer ? kBlur * 2.0f : 0.0f, Point{0.0f, 0.0f}, options);
  canvas.fill_rect(box, Paint::solid(face), kRadius);
}

/// 渲染整屏卡片（按网格铺 `kCards` 张）。
[[nodiscard]] auto render_cards(bool use_covered, bool opaque_face, bool two_layer) -> Canvas {
  Canvas canvas = make_canvas();
  constexpr int kCols = 8;
  constexpr float kGapX = 20.0f;
  constexpr float kGapY = 20.0f;
  for (int index = 0; index < kCards; ++index) {
    const int col = index % kCols;
    const int row = index / kCols;
    const float x = 20.0f + static_cast<float>(col) * (kCardWidth + kGapX);
    const float y = 20.0f + static_cast<float>(row) * (kCardHeight + kGapY);
    paint_card(canvas, Rect{x, y, kCardWidth, kCardHeight}, use_covered, opaque_face, two_layer);
  }
  return canvas;
}

/// 两次运行的**最大通道差**（逐像素、含 alpha）。
[[nodiscard]] auto max_channel_diff(const Canvas& left, const Canvas& right) -> int {
  const auto a = left.pixels();
  const auto b = right.pixels();
  const std::size_t count = std::min(a.size(), b.size());
  int worst = 0;
  for (std::size_t index = 0; index < count; ++index) {
    for (int shift = 0; shift < 32; shift += 8) {
      const int ca = static_cast<int>((a[index] >> shift) & 0xFFU);
      const int cb = static_cast<int>((b[index] >> shift) & 0xFFU);
      worst = std::max(worst, std::abs(ca - cb));
    }
  }
  return worst;
}

/// 跑 `rounds` 轮取**最小**耗时（与本仓其它性能探针同一口径：排除调度噪声）。
[[nodiscard]] auto best_ms(bool use_covered, bool opaque_face, bool two_layer, int rounds) -> double {
  double best = 1e9;
  for (int round = 0; round < rounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    const Canvas canvas = render_cards(use_covered, opaque_face, two_layer);
    const auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    best = std::min(best, ms);
    // 防止编译器把整段消掉：结果参与一次可观测的读取。
    if (canvas.pixel_at(0, 0).a == 0U) return best;
  }
  return best;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "`DrawOptions::covered_region` 的等价性与真实收益（阴影跳过被不透明底盖住的像素）",
      .criterion = "不透明底逐像素相等（max_diff==0）**且**半透明底必须可观测差异（判别力）",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("shadow_covered_probe", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  // ── ① 等价性：不透明底 + 跳过 == 不跳过（硬保证）──
  // 用**双层**阴影：`covered_region` 的跳过只实现在合并贴图那条路上
  // （`draw_shadow_layered` 的双层分支）；单层走 `draw_shadow`，那里不跳。
  const Canvas covered_good = render_cards(true, true, true);
  const Canvas baseline_good = render_cards(false, true, true);
  const int diff_opaque = max_channel_diff(covered_good, baseline_good);
  st::print("不透明底 + 双层：传 covered_region vs 不传，最大通道差 = {}\n", diff_opaque);

  // ── ② 判别力：半透明底 + 跳过 **必须**有可见差异 ──
  //
  // 若这里也是 0，说明后端根本没跳（或者跳错了），那么 ① 的“相等”就是恒真的假护栏
  // （`CONVENTIONS.md` §7.2：桩把差异抹平）。这条是 ① 的**反向配套**。
  const Canvas covered_translucent = render_cards(true, false, true);
  const Canvas baseline_translucent = render_cards(false, false, true);
  const int diff_translucent = max_channel_diff(covered_translucent, baseline_translucent);
  st::print("半透明底 + 双层：传 covered_region vs 不传，最大通道差 = {}（应 > 0）\n",
            diff_translucent);

  // ── ③ 收益（按阴影层数分开报：单层目前没有跳过）──
  constexpr int kRounds = 12;
  const double covered_two = best_ms(true, true, true, kRounds);
  const double baseline_two = best_ms(false, true, true, kRounds);
  const double covered_one = best_ms(true, true, false, kRounds);
  const double baseline_one = best_ms(false, true, false, kRounds);
  const auto pct = [](double base, double now) -> std::string {
    if (base <= 0.0) return "-";
    return std::format("{:.1f}%", (base - now) / base * 100.0);
  };
  st::print("耗时（{} 张卡片，{} 轮取最小）：\n", kCards, kRounds);
  st::print("  双层 不传 : {:.3f} ms\n", baseline_two);
  st::print("  双层 传   : {:.3f} ms  （省 {}）\n", covered_two, pct(baseline_two, covered_two));
  st::print("  单层 不传 : {:.3f} ms\n", baseline_one);
  st::print("  单层 传   : {:.3f} ms  （省 {}）\n", covered_one, pct(baseline_one, covered_one));

  if (diff_opaque != 0) {
    verdict.state = st::probe::status::fail;
    verdict.reason = std::format("不透明底应逐像素相等，实测最大通道差 {}（跳过不是等价变换）",
                                 diff_opaque);
  } else if (diff_translucent == 0) {
    verdict.state = st::probe::status::fail;
    verdict.reason =
        "半透明底也无差异 —— covered_region 可能根本没生效（上一条判据于是失去判别力）";
  }
  return st::probe::main_of(spec, opts, verdict);
}
