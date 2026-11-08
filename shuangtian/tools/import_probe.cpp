/// 按需导入的**机制验证探针**（不属于应用，只在 `tools/` 里跑）。
///
/// ## 它证明什么
///
/// 「按需导入」要成立，必须同时是两件事：
///
/// 1. **只 include 的几个组件可用**——`make_element("Panel"/"Text"/"Button")` 能造出来；
/// 2. **没 include 的组件整块不进产物**——链接后的二进制里查不到 `Terminal` 的符号，
///    而且 `make_element("Terminal")` 返回空。
///
/// 第 2 条是**这条机制真正难的地方**：改造前（集中清单 + `dsl.cpp` 全量 include），
/// 任何应用都会把全部 50 个组件链进产物（实测 `nm -C` 册府产物里
/// `Terminal`/`CodeEditor`/`DiffView` 全在）。
///
/// ## 怎么跑
///
/// ```bash
/// # 构建（st 会把它当独立探针目标；也可以手工 g++ 一行）
/// st build import_probe
/// # 判据：输出里 components=3，且 terminal_present=0
/// ```
///
/// 手工编（更能说明问题——完全不经构建系统）：
///
/// ```bash
/// g++ -std=c++23 -Iinclude -Ithird_party -Ithird_party/sqlite \
///     tools/import_probe.cpp build/dev/obj/_library_units_cefu.a -o /tmp/import_probe
/// ```
#include <cstdio>
#include <string>

#include "st/ui/component_registry.hpp"
#include "st/ui/components/button.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/components/text.hpp"

int main() {
  // ★ **这一步就是"我要这三个组件"的全部声明**（见 component_registry.hpp 文件头）。
  // 它引用 `component_importer<T>::run`——那定义在组件自己的 `.cpp` 里，
  // 所以链接器必须把那些目标文件拉进产物；没列在这里的组件就不会进来。
  st::ui::import_components<st::ui::Panel, st::ui::Text, st::ui::Button>();

  const auto names = st::ui::component_registry::names();
  std::printf("components=%zu\n", names.size());
  for (const auto& name : names) std::printf("  - %s\n", name.c_str());

  // ① 导入过的组件必须能造
  const bool panel_ok = st::ui::component_registry::create("Panel") != nullptr;
  const bool text_ok = st::ui::component_registry::create("Text") != nullptr;
  const bool button_ok = st::ui::component_registry::create("Button") != nullptr;
  std::printf("panel=%d text=%d button=%d\n", panel_ok ? 1 : 0, text_ok ? 1 : 0,
              button_ok ? 1 : 0);

  // ② 没 include 的组件必须查不到（这是"按需"的行为面）
  const bool terminal_present = st::ui::component_registry::create("Terminal") != nullptr;
  std::printf("terminal_present=%d\n", terminal_present ? 1 : 0);

  const bool ok = panel_ok && text_ok && button_ok && !terminal_present;
  std::printf("RESULT=%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
