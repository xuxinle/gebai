// 整帧热点探针（性能轮）：**只为采样剖析存在**——无限循环跑「标脏 → 布局 → 清屏 → 绘制整树」，
// 挂 `gdb` 采样即可拿到函数级归属（本机没有 perf，`gdb` 是唯一可用的采样器）。
//
// 关键：**不挂 `PaintProfiler`**（它每次原语两次 `clock_gettime`，会污染归因）。
//
// 用法： paint_hotspot_probe [max_frames]   （0 或缺省 = 无限循环）
//   ST_HOTSPOT_ROWS / COLS / TEXT / SHADOW / BORDER / BG 与 frame_profile_probe 同义

#include <chrono>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

inline constexpr int kWidth = 1280;
inline constexpr int kHeight = 800;

auto env_flag(std::string_view name, bool fallback) -> bool {
  const char* value = std::getenv(std::string(name).c_str());
  if (value == nullptr) return fallback;
  return std::string_view{value} != "0";
}

auto env_int(std::string_view name, int fallback) -> int {
  const char* value = std::getenv(std::string(name).c_str());
  return value != nullptr ? std::atoi(value) : fallback;
}

/// 场景配置（显式注入，不用可变全局——`CONVENTIONS.md` §8 L8）。
struct Config {
  bool text{true};
  bool shadow{true};
  bool border{true};
  bool background{true};
  int rows{12};
  int columns{8};
};

auto build_ui(const Config& config) -> std::unique_ptr<st::ui::Element> {
  auto root = std::make_unique<st::ui::Panel>();
  root->set_id("hotspot-root");
  root->style().direction = st::ui::FlexDirection::Column;
  root->style().gap = 12.0f;
  for (int row = 0; row < config.rows; ++row) {
    auto row_panel = std::make_unique<st::ui::Panel>();
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

}  // namespace

auto main(int argc, char** argv) -> int {
  const int max_frames = argc > 1 ? std::atoi(argv[1]) : 0;
  Config config;
  config.text = env_flag("ST_HOTSPOT_TEXT", true);
  config.shadow = env_flag("ST_HOTSPOT_SHADOW", true);
  config.border = env_flag("ST_HOTSPOT_BORDER", true);
  config.background = env_flag("ST_HOTSPOT_BG", true);
  config.rows = env_int("ST_HOTSPOT_ROWS", 12);
  config.columns = env_int("ST_HOTSPOT_COLS", 8);

  st::ui::UiRoot root{};
  root.set_theme(st::ui::Theme::light());
  root.set_content(build_ui(config));
  root.set_viewport({kWidth, kHeight});

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  const auto start = std::chrono::steady_clock::now();
  long long frames = 0;
  for (;;) {
    root.mark_dirty_all();
    root.layout(true);
    canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    (void)root.paint_frame(canvas);
    ++frames;
    if (max_frames > 0 && frames >= max_frames) break;
  }
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  st::print("hotspot probe: {} 帧 · {:.2f} ms/帧（{:.1f} FPS）\n", frames, ms / frames,
            1000.0 * static_cast<double>(frames) / ms);
  return 0;
}
