// **小形状每像素成本高**的归因：固定开销到底花在哪一步。
//
// 背景：`round_rect_cost_probe` 实测每像素成本随面积急剧变化：
//   按钮 96x32（3072 px）→ **4.686 ns/px**
//   卡片 320x160（51200 px）→ 0.837 ns/px
//   面板 720x480（345600 px）→ 0.359 ns/px
// 差 13 倍，说明有大笔**与面积无关的固定开销**被摊在小形状上。
//
// `fill_rect` 走 `fill_path`，其缓存路径每帧对**每次绘制**都要：
//   ① `scale_ != 1` 时 `path.scaled(scale_)`（建一份新路径：12 条命令 + 4 条贝塞尔）；
//   ② `path_control_origin()`（遍历全部命令）；
//   ③ `path_geometry_key()`（再遍历一遍，每控制点 2 次 lround）；
//   ④ 线性查找 `geometry_sprites_`（vector，最多 512 条）；
//   ⑤ `std::function<Path()>` 的构造（每次绘制一次，可能堆分配）。
//
// 本探针逐个量这些步骤，判定「该砍哪一笔」。
//
// 判据：只报数。

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/math/path.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Paint;
using st::math::Path;

constexpr int kRounds = 200;

[[nodiscard]] auto best_ns(const std::function<void()>& body) -> double {
  double best = 1e18;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::nano>(end - start).count());
  }
  return best;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "小形状的固定开销归因（建路径 / 键计算 / 缓存查找各占多少）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("round_rect_fixed_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  // 构造一个和 `fill_rect` 内部完全同形的圆角路径（4 段贝塞尔 + 4 条边）。
  const auto make_round_rect = [](Rect rect, float radius) {
    Path path;
    path.add_rounded_rect(rect, radius);
    return path;
  };

  const Rect button{100.0f, 100.0f, 96.0f, 32.0f};
  const Rect panel{100.0f, 100.0f, 720.0f, 480.0f};

  for (const auto& [name, box, radius] : {
           std::tuple{"按钮 96x32 r6", button, 6.0f},
           std::tuple{"面板 720x480 r16", panel, 16.0f},
       }) {
    const Path path = make_round_rect(box, radius);
    const double area = static_cast<double>(box.width) * static_cast<double>(box.height);
    st::log::out("{}（{:.0f} px，{} 条命令）\n", name, area, path.commands().size());

    // ① 仅构造路径（`add_rounded_rect` 本身）
    const double t_add = best_ns([&] {
      const Path p = make_round_rect(box, radius);
      (void)p.commands().size();
    });
    // ② 仅 origin 计算（走一遍命令流）——**复刻 `Canvas::path_control_origin` 的同式逻辑**
    //    （那两个方法是 private，不为探测去改可见性；这里是同一份算法）。
    const auto control_origin = [](const Path& p, float scale, float pad) {
      bool has = false;
      float min_x = 0.0f;
      float min_y = 0.0f;
      for (const auto& command : p.commands()) {
        const st::math::Point points[3] = {command.p1, command.p2, command.p3};
        const int count = command.kind == st::math::PathCommand::Kind::CubicTo   ? 3
                          : command.kind == st::math::PathCommand::Kind::QuadTo ? 2
                          : command.kind == st::math::PathCommand::Kind::Close  ? 0
                                                                                  : 1;
        for (int index = 0; index < count; ++index) {
          if (!has) {
            min_x = points[index].x;
            min_y = points[index].y;
            has = true;
            continue;
          }
          min_x = std::min(min_x, points[index].x);
          min_y = std::min(min_y, points[index].y);
        }
      }
      if (!has) return st::math::Point{0.0f, 0.0f};
      return st::math::Point{std::round(min_x * scale - pad), std::round(min_y * scale - pad)};
    };
    const double t_origin = best_ns([&] {
      const auto o = control_origin(path, 1.0f, 1.0f);
      (void)o;
    });
    // ③ origin + key（两遍命令流）
    const auto origin = control_origin(path, 1.0f, 1.0f);
    const double t_key = best_ns([&] {
      // 复刻 `Canvas::path_geometry_key` 的 FNV 混合（同式）。
      std::uint64_t hash = 1469598103934665603ULL;
      const auto mix64 = [&hash](std::uint64_t part) {
        hash ^= part;
        hash *= 1099511628211ULL;
      };
      const auto quantize = [&mix64, origin](float value, float offset) {
        mix64(static_cast<std::uint64_t>(
            static_cast<std::int64_t>(std::lround((value - offset) * 256.0f)) + 0x40000000));
      };
      mix64(25000ULL);
      mix64(100000ULL);
      const auto commands = path.commands();
      mix64(commands.size());
      for (const auto& command : commands) {
        mix64(static_cast<std::uint64_t>(static_cast<std::uint8_t>(command.kind)));
        quantize(command.p1.x, origin.x);
        quantize(command.p1.y, origin.y);
        const bool has_p2 = command.kind == st::math::PathCommand::Kind::QuadTo ||
                            command.kind == st::math::PathCommand::Kind::CubicTo;
        if (has_p2) {
          quantize(command.p2.x, origin.x);
          quantize(command.p2.y, origin.y);
        }
        if (command.kind == st::math::PathCommand::Kind::CubicTo) {
          quantize(command.p3.x, origin.x);
          quantize(command.p3.y, origin.y);
        }
      }
      (void)hash;
    });
    // ④ std::function 构造（每次绘制一次）
    const double t_fn = best_ns([&] {
      const auto f = [&path, origin]() -> Path { return path.translated(-origin.x, -origin.y); };
      const std::function<Path()> fn = f;
      (void)fn;
    });

    st::log::out("  ① add_rounded_rect        {:>9.0f} ns  （{:>5.2f} ns/px）\n", t_add,
              t_add / area);
    st::log::out("  ② path_control_origin     {:>9.0f} ns  （{:>5.2f} ns/px）\n", t_origin,
              t_origin / area);
    st::log::out("  ③ path_geometry_key       {:>9.0f} ns  （{:>5.2f} ns/px）\n", t_key,
              t_key / area);
    st::log::out("  ④ std::function 构造      {:>9.0f} ns  （{:>5.2f} ns/px）\n", t_fn,
              t_fn / area);
    const double sum = t_add + t_origin + t_key + t_fn;
    st::log::out("  合计（不含实际混合）      {:>9.0f} ns  （{:>5.2f} ns/px）\n\n", sum, sum / area);
  }
  st::log::out("注：这些是**每次绘制都要付**的成本（即使缓存命中）。\n");
  return st::probe::main_of(spec, opts, verdict);
}
