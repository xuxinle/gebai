// 绘制原语消融 / 候选方案对照探针（性能轮）。
//
// 目的：在**改框架代码之前**把候选优化路线量出来——
// 「卡片 = 两层投影 + 圆角底 + 描边 + 文字」逐项量，并对照替代实现：
//   · 描边（stroke_to_path：逐段四边形 + 顶点补圆）vs **环形填充**（外/内两条圆角矩形反向绕）
//   · 投影合成在「遮罩非零范围」内 vs 整个 padding 区域
//   · 圆角填充 vs 直角填充（量出圆角路径光栅化的额外开销）
//
// 用法： paint_ablation_probe [repeats]
//   ST_ABLATION_CANVAS=1  每次绘制前清屏（模拟整屏重绘）
//   ST_ABLATION_MOVE=1    每遍错开位置（使阴影遮罩缓存无法复用）

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/text/text.hpp"
#include "st/ui/theme.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr int kWidth = 1280;
inline constexpr int kHeight = 800;
inline constexpr float kRadius = 12.0f;
inline constexpr float kCardW = 140.0f;
inline constexpr float kCardH = 48.0f;

auto elapsed_ms(const Clock::time_point& start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// 探针配置（显式注入，不用可变全局——`CONVENTIONS.md` §8 L8）。
struct Config {
  bool clear_each{false};
  bool move{false};
};

[[nodiscard]] auto env_flag(std::string_view name, bool fallback) -> bool {
  const char* value = std::getenv(std::string(name).c_str());
  if (value == nullptr) return fallback;
  return std::string_view{value} != "0";
}

template <class Body>
[[nodiscard]] auto measure(int repeat, Body&& body) -> double {
  double best = 1e9;
  for (int index = 0; index < repeat; ++index) {
    const auto start = Clock::now();
    body(index);
    best = std::min(best, elapsed_ms(start));
  }
  return best;
}

auto clear_canvas(st::raster::Canvas& canvas) -> void {
  canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
}

/// 环形边框路径：外圈圆角矩形（正向）+ 内圈（反向）——同一路径里两条子路径，
/// 非零环绕规则自然挖出内洞。替代「描边」的候选实现。
[[nodiscard]] auto make_border_ring(st::math::Rect outer, float radius, float width)
    -> st::raster::Path {
  st::raster::Path path;
  path.add_rounded_rect(outer, radius);
  st::raster::Path inner;
  const st::math::Rect box = outer.inset(st::math::Insets::all(width));
  inner.add_rounded_rect(box, radius > width ? radius - width : 0.0f);
  inner.reverse();
  path.add_path(inner);
  return path;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const int repeat = argc > 1 ? std::atoi(argv[1]) : 40;
  Config config;
  config.clear_each = env_flag("ST_ABLATION_CANVAS", false);
  config.move = env_flag("ST_ABLATION_MOVE", false);

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  clear_canvas(canvas);

  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::Shadow md = st::ui::shadow_md(theme);
  const st::math::Rect box{100.0f, 100.0f, kCardW, kCardH};
  const st::raster::Paint white = st::raster::Paint::solid(st::math::Color::rgb(0xFF, 0xFF, 0xFF));
  const st::raster::Paint border = st::raster::Paint::solid(theme.colors().border);

  auto place = [&](int index) -> st::math::Rect {
    if (!config.move) return box;
    return st::math::Rect{40.0f + static_cast<float>(index % 8) * 150.0f,
                          40.0f + static_cast<float>((index / 8) % 4) * 120.0f, kCardW, kCardH};
  };

  double clear_ms = 1e9;
  for (int index = 0; index < 7; ++index) {
    const auto start = Clock::now();
    clear_canvas(canvas);
    clear_ms = std::min(clear_ms, elapsed_ms(start));
  }

  struct Row {
    std::string name;
    double ms{0.0};
  };
  std::vector<Row> rows;
  auto add = [&](std::string name, double ms) { rows.push_back({std::move(name), ms}); };

  // —— 底：圆角 vs 直角 ——
  add("圆角填充 r=12（140×48）", measure(repeat, [&](int index) {
        if (config.clear_each) clear_canvas(canvas);
        canvas.fill_rect(place(index), white, kRadius);
      }));
  add("直角填充（同尺寸）", measure(repeat, [&](int index) {
        if (config.clear_each) clear_canvas(canvas);
        canvas.fill_rect(place(index), white, 0.0f);
      }));

  // —— 投影：关键层 / 环境层 / 两层 ——
  add(std::format("投影 关键层 blur={:.0f}", static_cast<double>(md.blur)),
      measure(repeat, [&](int index) {
        if (config.clear_each) clear_canvas(canvas);
        canvas.draw_shadow(box, kRadius, md.blur, md.color,
                           st::math::Point{md.offset_x, md.offset_y});
      }));
  add(std::format("投影 环境层 blur={:.0f}", static_cast<double>(md.blur2)),
      measure(repeat, [&](int index) {
        if (config.clear_each) clear_canvas(canvas);
        canvas.draw_shadow(box, kRadius, md.blur2, md.color2,
                           st::math::Point{md.offset2_x, md.offset2_y});
      }));
  add("投影 两层（卡片真实形态）", measure(repeat, [&](int index) {
        if (config.clear_each) clear_canvas(canvas);
        canvas.draw_shadow(box, kRadius, md.blur2, md.color2,
                           st::math::Point{md.offset2_x, md.offset2_y});
        canvas.draw_shadow(box, kRadius, md.blur, md.color,
                           st::math::Point{md.offset_x, md.offset_y});
      }));

  // —— 边框：描边（现状） vs 环形填充（候选） ——
  add("边框 描边 1px r=12（现状）", measure(repeat, [&](int index) {
        if (config.clear_each) clear_canvas(canvas);
        st::raster::Path outline;
        outline.add_rounded_rect(box.inset(st::math::Insets::all(0.5f)), kRadius - 0.5f);
        canvas.stroke_path(outline, border, 1.0f);
      }));
  add("边框 环形填充 1px r=12（候选）", measure(repeat, [&](int index) {
        if (config.clear_each) clear_canvas(canvas);
        const st::raster::Path ring = make_border_ring(box, kRadius, 1.0f);
        canvas.fill_path(ring, border);
      }));
  add("边框 描边 1px 直角（对照）", measure(repeat, [&](int index) {
        if (config.clear_each) clear_canvas(canvas);
        st::raster::Path outline;
        outline.add_rect(box.inset(st::math::Insets::all(0.5f)));
        canvas.stroke_path(outline, border, 1.0f);
      }));
  add("边框 环形填充 1px 直角（对照）", measure(repeat, [&](int index) {
        if (config.clear_each) clear_canvas(canvas);
        st::raster::Path ring;
        ring.add_rect(box);
        st::raster::Path inner;
        inner.add_rect(box.inset(st::math::Insets::all(1.0f)));
        inner.reverse();
        ring.add_path(inner);
        canvas.fill_path(ring, border);
      }));

  // —— 文字 ——
  auto fonts = st::text::FontStack::system_default();
  std::optional<st::text::TextRenderer> renderer;
  if (fonts) renderer.emplace(*fonts);
  const std::string sample = "卡片 0·0";
  if (renderer.has_value()) {
    add("文本 13px ×1（4 汉字）", measure(repeat, [&](int index) {
          if (config.clear_each) clear_canvas(canvas);
          (void)renderer->draw(canvas, sample, st::math::Point{20.0f, 20.0f}, 13.0f,
                               theme.colors().text);
        }));
  } else {
    add("文本：无系统字体（跳过）", 0.0);
  }

  // —— 整张卡片：现状 vs 候选（边框换环形填充） ——
  add("整卡片 现状（两层影+底+描边+字）", measure(repeat, [&](int index) {
        if (config.clear_each) clear_canvas(canvas);
        const st::math::Rect target = place(index);
        canvas.draw_shadow(target, kRadius, md.blur2, md.color2,
                           st::math::Point{md.offset2_x, md.offset2_y});
        canvas.draw_shadow(target, kRadius, md.blur, md.color,
                           st::math::Point{md.offset_x, md.offset_y});
        canvas.fill_rect(target, white, kRadius);
        st::raster::Path outline;
        outline.add_rounded_rect(target.inset(st::math::Insets::all(0.5f)), kRadius - 0.5f);
        canvas.stroke_path(outline, border, 1.0f);
        if (renderer.has_value()) {
          (void)renderer->draw(canvas, sample, st::math::Point{target.x + 16.0f, target.y + 14.0f},
                               13.0f, theme.colors().text);
        }
      }));
  add("整卡片 候选（边框换环形填充）", measure(repeat, [&](int index) {
        if (config.clear_each) clear_canvas(canvas);
        const st::math::Rect target = place(index);
        canvas.draw_shadow(target, kRadius, md.blur2, md.color2,
                           st::math::Point{md.offset2_x, md.offset2_y});
        canvas.draw_shadow(target, kRadius, md.blur, md.color,
                           st::math::Point{md.offset_x, md.offset_y});
        canvas.fill_rect(target, white, kRadius);
        canvas.fill_path(make_border_ring(target, kRadius, 1.0f), border);
        if (renderer.has_value()) {
          (void)renderer->draw(canvas, sample, st::math::Point{target.x + 16.0f, target.y + 14.0f},
                               13.0f, theme.colors().text);
        }
      }));

  st::print("ablation · repeat={} · clear_each={} · move={} · 清屏 {:.3f}ms\n", repeat,
            config.clear_each ? 1 : 0, config.move ? 1 : 0, clear_ms);
  for (const Row& row : rows) {
    st::print("  {:<40} {:>9.3} ms · {:>8.1} µs\n", row.name, row.ms, row.ms * 1000.0);
  }
  return 0;
}
