// 霜天声明式 UI 示例：计数器（C++ struct 风格，≈ ArkTS @Component）。
//
// 用法：
//   counter --headless --frames 3 --control-port 0   （无头冒烟）
//   counter                                          （窗口模式）
//
// 本示例展示 docs/declarative.md 的 C++ DSL 全链路：
//   struct 组件 + State 成员 + build() + 状态驱动重组 + 元素复用。

#include <cstdio>
#include <format>
#include <memory>
#include <string>

#include "st/app/app.hpp"
#include "st/app/cli.hpp"
#include "st/core/entry.hpp"
#include "st/core/time.hpp"
#include "st/ui/dsl.hpp"

namespace {

using namespace st::ui;
using namespace st::ui::dsl;

/// 主页面：计数 + 条件内容 + 进度联动（三种状态形态各一）。
struct CounterPage : Component {
  State<int> count{0};
  State<bool> show_extra{false};

  void build(Composer& c) override {
    column(c, {.gap = 12.0f, .padding = 24.0f}, [&] {
      heading(c, "霜天声明式计数器", 2);
      text(c, [&] { return std::format("点击了 {} 次", count.value()); }, {.key = "count"});
      row(c, {.gap = 8.0f}, [&] {
        button(c, "+1", [this] { count.set(count.value() + 1); }, {.key = "inc"});
        button(c, "-1", [this] { count.set(count.value() - 1); }, {.key = "dec"});
        button(c, "重置", [this] { count.set(0); }, {.key = "reset"});
        button(c, show_extra.value() ? "收起" : "展开", [this] { show_extra.set(!show_extra.value()); },
               {.key = "toggle"});
      });
      // 派生状态：进度随计数前进（读 count → 自动依赖）
      progress(c, [&] {
        const int value = count.value();
        return value <= 0 ? 0.0f : (value >= 10 ? 1.0f : static_cast<float>(value) / 10.0f);
      }());
      if (show_extra.value()) {
        (void)card(c, {.padding = 12.0f, .key = "extra"}, [&] {
          (void)text(c, [&] { return std::format("计数是{}数", count.value() % 2 == 0 ? "偶" : "奇"); });
          (void)badge(c, [&] { return count.value() > 5 ? std::string("超过 5") : std::string("≤ 5"); }());
        });
      }
    });
  }
};

}  // namespace

auto run_app(int argc, char** argv) -> int {
  st::app::CommonOptions common;
  if (auto parsed = st::app::parse_common_options(argc, argv, common); !parsed) {
    std::fprintf(stderr, "%s\n", parsed.error().to_string().c_str());
    return 1;
  }
  common.app.title = "霜天声明式计数器";
  st::app::Application app("counter", "0.1.0", common.app);

  // 声明式挂载：组件 → 重组器 → 真值树（协议 tree/get/set/invoke 照常可用）。
  // 挂在 start() 之前（set_content 语义与手搭界面一致）。重组随主循环 tick 推进。
  auto host = dsl::mount(app.root(), std::make_shared<CounterPage>());
  if (host == nullptr) return 1;

  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败: %s\n", status.error().to_string().c_str());
    return 1;
  }

  std::uint32_t frames = 1;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    if (host->dirty()) {
      (void)host->tick();
      app.request_repaint();
    }
    app.tick();
    ++frames;
    if (common.max_frames > 0 && frames >= common.max_frames) break;
    if (common.max_ms > 0 && st::time::now_ms() - frame_start_ms >= static_cast<std::int64_t>(common.max_ms)) break;
    app.pace_loop(frame_start_ms);
  }
  return 0;
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
