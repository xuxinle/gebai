// 整帧绘制分解探针（性能轮）。
//
// 与 `tests/app_frame_budget_test.cpp` 的 `frame_cost_breakdown_for_diagnostics` 同一场景，
// 但：
//   ① 挂 `PaintProfiler`，按**逐帧增量**取分解；
//   ② 报**最快那一帧**——本机是共享/受限环境（cgroup 4 核、loadavg 常 >2），噪声只会抬高
//      耗时，最小值最接近真实成本；
//   ③ **可挂真实文本端口**（默认挂；`ST_PROBE_TEXTPORT=0` 关）——门禁与旧探针都没挂端口，
//      于是 96 个 `Text` 元素实际**一个字形都没画**（`NullTextPort::measure_width()` 恒 0、
//      `draw()` 是空实现）。“96 段文字”这个说法与实测不符，这里给出有/无文字两组数。
//
// 用法： frame_profile_probe [frames] [scale]
//   ST_PROBE_TEXT / SHADOW / BORDER / BG = 0|1
//   ST_PROBE_TEXTPORT = 0|1（默认 1，挂真实字体引擎）
//   ST_PROBE_ROWS / ST_PROBE_COLS（默认 12×8）
//   ST_PROBE_VERBOSE=1 打印逐帧明细

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr int kWidth = 1280;
inline constexpr int kHeight = 800;
constexpr std::size_t kOps = static_cast<std::size_t>(st::raster::PaintOp::Count);

auto elapsed_ms(const Clock::time_point& start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

auto env_flag(std::string_view name, bool fallback) -> bool {
  const char* value = std::getenv(std::string(name).c_str());
  if (value == nullptr) return fallback;
  return std::string_view{value} != "0";
}

auto env_int(std::string_view name, int fallback) -> int {
  const char* value = std::getenv(std::string(name).c_str());
  return value != nullptr ? std::atoi(value) : fallback;
}

/// 场景配置。**不用可变全局**（`CONVENTIONS.md` §8 L8：进程级可变状态一律显式注入）——
/// 探针虽小，但"配置从哪来"与框架里其他代码遵守同一条约定，免得后人照抄。
struct Config {
  bool text{true};
  bool shadow{true};
  bool border{true};
  bool background{true};
  bool textport{true};
  bool verbose{false};
  int rows{12};
  int columns{8};
};

auto build_ui(const Config& config) -> std::unique_ptr<st::ui::Element> {
  auto root = std::make_unique<st::ui::Panel>();
  root->set_id("probe-root");
  root->style().direction = st::ui::FlexDirection::Column;
  root->style().gap = 12.0f;
  for (int row = 0; row < config.rows; ++row) {
    auto row_panel = std::make_unique<st::ui::Panel>();
    row_panel->set_id(std::format("row-{}", row));
    row_panel->style().direction = st::ui::FlexDirection::Row;
    row_panel->style().gap = 8.0f;
    for (int column = 0; column < config.columns; ++column) {
      auto card = std::make_unique<st::ui::Card>();
      card->set_id(std::format("card-{}-{}", row, column));
      card->style().width = 140.0f;
      card->style().height = 48.0f;
      if (!config.shadow) card->set_shadow_level(0);
      if (!config.border) card->style().border_width = 0.0f;
      if (!config.background) card->style().background = st::math::Color{0, 0, 0, 0};
      if (config.text) {
        auto label = std::make_unique<st::ui::Text>(std::format("卡片 {}·{}", row, column));
        label->style().font_size = 13.0f;
        card->add_child(std::move(label));
      }
      row_panel->add_child(std::move(card));
    }
    root->add_child(std::move(row_panel));
  }
  return root;
}

struct Breakdown {
  double paint_ms{0.0};
  double op_ms[kOps]{};
  std::uint64_t op_calls[kOps]{};
};

struct Snapshot {
  double op_ms[kOps]{};
  std::uint64_t op_calls[kOps]{};
};

auto snapshot(const st::raster::PaintProfiler& profiler) -> Snapshot {
  Snapshot out;
  for (std::size_t index = 0; index < kOps; ++index) {
    const st::raster::PaintOpStat& stat = profiler.op(static_cast<st::raster::PaintOp>(index));
    out.op_ms[index] = stat.ms;
    out.op_calls[index] = stat.calls;
  }
  return out;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const int frames = argc > 1 ? std::atoi(argv[1]) : 24;
  const float scale = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 1.0f;
  Config config;
  config.text = env_flag("ST_PROBE_TEXT", true);
  config.shadow = env_flag("ST_PROBE_SHADOW", true);
  config.border = env_flag("ST_PROBE_BORDER", true);
  config.background = env_flag("ST_PROBE_BG", true);
  config.textport = env_flag("ST_PROBE_TEXTPORT", true);
  config.verbose = env_flag("ST_PROBE_VERBOSE", false);
  config.rows = env_int("ST_PROBE_ROWS", 12);
  config.columns = env_int("ST_PROBE_COLS", 8);

  std::optional<st::text::FontStack> fonts;
  std::optional<st::text::TextRenderer> renderer;
  std::unique_ptr<st::ui::TextPort> port;
  if (config.textport) {
    auto loaded = st::text::FontStack::system_default();
    if (!loaded) {
      st::print("probe: 无系统字体——文本端口不可用（本组数据不含文字绘制）\n");
      config.textport = false;
    } else {
      fonts.emplace(std::move(*loaded));
      renderer.emplace(*fonts);
      port = st::app::make_text_port(*renderer);
    }
  }

  st::ui::UiRoot root{};
  root.set_theme(st::ui::Theme::light());
  if (port != nullptr) root.set_text_port(port.get());
  root.set_content(build_ui(config));
  root.set_viewport({kWidth, kHeight});

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, scale);
  st::raster::PaintProfiler profiler;
  canvas.set_profiler(&profiler);

  double clear_unit = 1e9;
  for (int index = 0; index < 7; ++index) {
    const auto start = Clock::now();
    canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    clear_unit = std::min(clear_unit, elapsed_ms(start));
  }

  std::vector<Breakdown> samples;
  Snapshot previous = snapshot(profiler);
  for (int index = 0; index < frames; ++index) {
    root.mark_dirty_all();
    const auto frame_start = Clock::now();
    root.layout(true);
    const auto after_layout = Clock::now();
    canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    (void)root.paint_frame(canvas);
    const auto after_paint = Clock::now();
    const Snapshot now = snapshot(profiler);
    Breakdown entry;
    entry.paint_ms = std::chrono::duration<double, std::milli>(after_paint - after_layout).count();
    for (std::size_t op = 0; op < kOps; ++op) {
      entry.op_ms[op] = now.op_ms[op] - previous.op_ms[op];
      entry.op_calls[op] = now.op_calls[op] - previous.op_calls[op];
    }
    previous = now;
    if (config.verbose) {
      std::string detail;
      for (std::size_t op = 0; op < kOps; ++op) {
        if (entry.op_calls[op] == 0) continue;
        detail += std::format(" {}={:.2f}ms/{}次",
                              st::raster::paint_op_name(static_cast<st::raster::PaintOp>(op)),
                              entry.op_ms[op], static_cast<double>(entry.op_calls[op]));
      }
      st::print("  #{:<3} 帧={:7.2}ms{}\n", index, entry.paint_ms, detail);
    }
    if (index >= frames / 4) samples.push_back(entry);
  }
  if (samples.empty()) {
    st::print("probe: 采样为空\n");
    return 1;
  }
  const auto fastest = std::ranges::min_element(samples, {}, &Breakdown::paint_ms);
  const Breakdown& best = *fastest;
  std::vector<double> paints;
  for (const auto& sample : samples) paints.push_back(sample.paint_ms);
  std::ranges::sort(paints);

  st::print("probe · {}x{} · text={} shadow={} border={} bg={} port={} · scale={:.2f} · 清屏 "
            "{:.3f}ms · 稳态 {} 帧（噪声 p50 {:.2f}ms）\n",
            config.rows, config.columns, config.text ? 1 : 0, config.shadow ? 1 : 0,
            config.border ? 1 : 0, config.background ? 1 : 0, port != nullptr ? 1 : 0,
            static_cast<double>(scale), clear_unit, static_cast<int>(samples.size()),
            paints[paints.size() / 2]);
  st::print("  最优帧 paint {:.2f}ms（{:.1f}× 清屏）· painted={} full={}\n", best.paint_ms,
            best.paint_ms / clear_unit, root.painted_elements(),
            root.last_frame_partial() ? 0 : 1);
  double accounted = 0.0;
  for (std::size_t op = 0; op < kOps; ++op) {
    if (best.op_calls[op] == 0) continue;
    accounted += best.op_ms[op];
    st::print("  {:<12} {:>7.2} 次 · {:>7.2} ms · {:>9.1} µs/次\n",
              st::raster::paint_op_name(static_cast<st::raster::PaintOp>(op)),
              static_cast<double>(best.op_calls[op]), best.op_ms[op],
              best.op_ms[op] * 1000.0 / static_cast<double>(best.op_calls[op]));
  }
  st::print("  （分解 {:.2f}ms / 帧计 {:.2f}ms；差 {:.2f}ms = 树遍历+剔除+调度）\n", accounted,
            best.paint_ms, best.paint_ms - accounted);
  return 0;
}
