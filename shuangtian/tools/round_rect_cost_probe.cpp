// **圆角矩形的时间去哪了**：把它拆成「贴图混合（带宽）」与「建图/光栅化（计算）」。
//
// 背景：gallery 1600×1000 实测 `round_rect` 33 次 / 1.46 ms / 1.11 M 像素
// = **1.32 ns/px**，而同一帧的 `fill_rect`（直角整行快路径）是 **0.035 ns/px**
// —— 差 37 倍。那个比值不能只用"要走路径光栅化"解释，所以必须分开量：
//
//   ① **命中缓存后**的纯贴图混合成本（与 `fill_rect` 同属带宽型）；
//   ② **未命中时的建图成本**（光栅化 + 建贴图，计算型）。
//
// 两个数决定完全不同的优化方向：
//   - 若 ① 已经不便宜 → 瓶颈在带宽，该减**写入量**（overdraw / 位宽）；
//   - 若 ① 很便宜而 ② 很贵 → 该**减少建图次数**（缓存键/生命周期/预热），
//     而不是再去优化光栅化算法（那条已在 BACKLOG 试过并回退）。
//
// 判据：只报数。

#include <chrono>
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

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "圆角矩形：贴图混合（带宽）vs 建图（计算）的成本拆分",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("round_rect_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  // 三种典型尺寸：小控件（按钮）、中卡片、大面板。
  struct Case {
    const char* name;
    float width;
    float height;
    float radius;
  };
  const Case cases[] = {
      {"按钮 96x32 r6", 96.0f, 32.0f, 6.0f},
      {"卡片 320x160 r12", 320.0f, 160.0f, 12.0f},
      {"面板 720x480 r16", 720.0f, 480.0f, 16.0f},
  };

  st::log::out("画布 {}x{}，每项 {} 轮取最小\n\n", kWidth, kHeight, kRounds);
  st::log::out("{:<22}  {:>10}  {:>12}  {:>12}  {:>12}\n", "形状", "面积(px)", "命中 ns/px",
            "未命中 ns/px", "未命中/命中");
  for (const Case& item : cases) {
    const Rect box{100.0f, 100.0f, item.width, item.height};
    const double area = static_cast<double>(item.width) * static_cast<double>(item.height);    const Paint paint = Paint::solid(Color::rgb(0xFF, 0xFF, 0xFF));

    // ① 命中：把同一形状画很多次，缓存必然命中（除第一次）。
    {
      Canvas canvas(kWidth, kHeight, 1.0f);
      canvas.clear(Color::rgb(0x20, 0x20, 0x28));
      // 预热：先画一次建图。
      canvas.fill_rect(box, paint, item.radius);
      const double ms = best_ms([&] {
        for (int index = 0; index < 40; ++index) {
          canvas.fill_rect(box, paint, item.radius);
        }
      });
      const double ns_per_px = ms * 1e6 / (area * 40.0);
      st::log::out("{:<22}  {:>10.0f}  {:>12.3f}", item.name, area, ns_per_px);

      // ② 未命中：每轮换一个半径，逼它建新图——但那会 Mix 进光栅化+建图之外的
      //    "新键计算"成本。更干净的做法是每轮清表重画同一形状。
      double miss_ns_per_px = 0.0;
      {
        double best = 1e9;
        for (int round = 0; round < kRounds; ++round) {
          Canvas fresh(kWidth, kHeight, 1.0f);
          fresh.clear(Color::rgb(0x20, 0x20, 0x28));
          const auto start = std::chrono::steady_clock::now();
          fresh.fill_rect(box, paint, item.radius);   // 缓存是空的 → 必未命中
          const auto end = std::chrono::steady_clock::now();
          best = std::min(best, std::chrono::duration<double, std::milli>(end - start).count());
        }
        miss_ns_per_px = best * 1e6 / area;
      }
      st::log::out("  {:>12.3f}  {:>12.2f}×\n", miss_ns_per_px,
                ns_per_px > 0.0 ? miss_ns_per_px / ns_per_px : 0.0);
    }
  }
  st::log::out("\n注：「命中」= 贴图复用，纯混合成本（带宽型）；"
            "「未命中」= 含路径光栅化 + 建贴图（计算型）。\n");
  st::log::out("    若未命中远贵于命中 → 减少建图次数比优化光栅化更值。\n");
  return st::probe::main_of(spec, opts, verdict);
}
