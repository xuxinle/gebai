// 霜天声明式 UI 示例：计数器（JS 宿主，compose 风格）。
//
// 与 examples/counter（C++ DSL）同一界面、同一语义——双宿主互为一致性验证。
// 用法：
//   counter-js --headless --frames 3 --control-port 0
//
// JS 声明式代码经 DeclarativeHost 注入运行；界面与 C++ 版一致：
// 标题 + 计数文本 + (+1/-1/重置/展开) 按钮 + 进度条 + 条件卡片。

#include <cstdio>
#include <memory>
#include <string>

#include "st/app/app.hpp"
#include "st/app/cli.hpp"
#include "st/core/entry.hpp"
#include "st/core/time.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/declarative_host.hpp"
#include "st/ui/script_host.hpp"

namespace {

/// 界面逻辑（JS，compose 风格）——运行时注入，改它不用重新编译。
/// 嵌入为原始字符串（示例规模小；框架侧的 declarative.js 才走 embed 机制）。
constexpr std::string_view kAppJs = R"JS(
let count = null;
let showExtra = null;
compose('CounterJs', () => {
  if (count === null) count = useState(0);
  if (showExtra === null) showExtra = useState(false);
  const kids = [
    heading('霜天声明式计数器（JS）', 2),
    text(() => '点击了 ' + count.value + ' 次'),
    row({gap: 8}, [
      button('+1', () => { count.value = count.value + 1; }),
      button('-1', () => { count.value = count.value - 1; }),
      button('重置', () => { count.value = 0; }),
      button(showExtra.value ? '收起' : '展开', () => { showExtra.value = !showExtra.value; }),
    ]),
    progress(count.value <= 0 ? 0 : (count.value >= 10 ? 1 : count.value / 10)),
  ];
  if (showExtra.value) {
    kids.push(card({padding: 12}, [
      text(() => '计数是' + (count.value % 2 === 0 ? '偶数' : '奇数')),
      badge(count.value > 5 ? '超过 5' : '≤ 5'),
    ]));
  }
  return column({gap: 12, padding: 24}, kids);
});
)JS";

}  // namespace

auto run_app(int argc, char** argv) -> int {
  st::app::CommonOptions common;
  if (auto parsed = st::app::parse_common_options(argc, argv, common); !parsed) {
    std::fprintf(stderr, "%s\n", parsed.error().to_string().c_str());
    return 1;
  }
  common.app.title = "霜天声明式计数器（JS）";
  common.app.enable_script = true;   // JS 宿主的前提：脚本引擎就绪
  st::app::Application app("counter-js", "0.1.0", common.app);

  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败: %s\n", status.error().to_string().c_str());
    return 1;
  }

  // 脚本宿主在 start() 里创建——之后才能接声明式层
  auto* script = app.script();
  if (script == nullptr || !script->valid()) {
    std::fprintf(stderr, "脚本宿主未就绪\n");
    return 1;
  }
  auto decl = st::ui::DeclarativeHost::attach(*script, app.root());
  if (decl == nullptr) {
    std::fprintf(stderr, "声明式宿主接入失败\n");
    return 1;
  }
  if (auto status = decl->run(std::string(kAppJs), "<counter-js>"); !status) {
    std::fprintf(stderr, "声明式代码执行失败: %s\n", status.error().message.c_str());
    return 1;
  }

  std::uint32_t frames = 1;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    (void)decl->tick();   // 帧首重组（dirty 才做事）
    app.tick();
    ++frames;
    if (common.max_frames > 0 && frames >= common.max_frames) break;
    if (common.max_ms > 0 &&
        st::time::now_ms() - frame_start_ms >= static_cast<std::int64_t>(common.max_ms)) {
      break;
    }
    app.pace_loop(frame_start_ms);
  }
  return 0;
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
