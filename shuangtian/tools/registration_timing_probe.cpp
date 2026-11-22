// ⚠ 已失效（2026-11-22 盘点）
//
// 失效原因：依赖构建宏 PROBE_AUTO/PROBE_ON_DEMAND
//
// 这个探针**当前编译不过**——它依赖的 API 已经删改。保留它是因为：
// 它记录过的实测结论可能仍被文档引用，删掉文件会让那些数字永久无法复核。
//
// 请把它当**历史档案**读，不要当活工具用。
// 真要复活：先按上面的原因把 API 改回当前形态，再验它测的东西是否还成立
//（很多测量前提已经变了，结论不保证可移植）。
//
// 盘点清单与分类判据见 tools/README.md「探针的分类与为何不全删」。

/// 注册时机的**自动/按需**两条断言（各自独立产物，不能用同一可执行验证）。
///
/// ## 为什么必须是独立产物
///
/// 这两条说的都是「某个组件的 `.o` 有没有进产物」，而在同一个测试二进制里
/// **所有组件都被链进来了**——区分不出来。所以它们各自的 main 必须是独立链接的。
///
/// 配套脚本：`tools/check_registration_timing.sh`。
///
/// ## 断的是什么
///
/// **AUTO**：真的 `new` 了 `Terminal`、一行 `import` 都不写 → 它必须已注册。
///   这证明"用到即自动注册"成立（靠组件 `.cpp` 里的静态登记块）。
///
/// **ON_DEMAND**：include 了 `terminal.hpp` 但只用 `Text` → `Terminal` 必须查不到，
///   且产物里不能有它的成员符号。这证明"按需"在产物层面成立。
///
/// 两条一起守的意义：**只 include 不引用 → 不进产物**（按需），
/// **引用 → 进产物 → 静态登记自动生效**（自动）。任一被破坏，
/// `st/ui/components/README.md` 第二节的说法就成了假话。
#include <cstdio>
#include <memory>
#include <string>

#include "st/ui/component_registry.hpp"

// 由编译命令选择：-DPROBE_AUTO 或 -DPROBE_ON_DEMAND（不定义则报错，避免误用）
#if !defined(PROBE_AUTO) && !defined(PROBE_ON_DEMAND)
#error "必须定义 PROBE_AUTO 或 PROBE_ON_DEMAND（见 tools/check_registration_timing.sh）"
#endif

#if defined(PROBE_AUTO)
#include "st/ui/components/terminal.hpp"

int main() {
  // 真的用到 Terminal：这一步让 terminal.o 进产物，静态登记随之在 main 之前发生。
  auto terminal = std::make_unique<st::ui::Terminal>();
  std::printf("造出 type=%s\n", std::string(terminal->type()).c_str());

  // 关键：**没有**任何 import_components 调用。
  const bool registered = st::ui::component_registry::contains("Terminal");
  std::printf("未写 import，Terminal 已注册=%d\n", registered ? 1 : 0);
  std::printf("RESULT=%s\n", registered ? "AUTO_OK" : "AUTO_FAIL");
  return registered ? 0 : 1;
}

#else  // PROBE_ON_DEMAND
#include "st/ui/components/text.hpp"

int main() {
  // 只用到 Text —— Terminal 的 .o 不该被拉进来。
  auto text = std::make_unique<st::ui::Text>("hi");
  std::printf("造出 type=%s\n", std::string(text->type()).c_str());

  const bool leaked = st::ui::component_registry::contains("Terminal");
  std::printf("没用 Terminal，它已注册=%d\n", leaked ? 1 : 0);
  std::printf("RESULT=%s\n", leaked ? "LEAKED" : "ON_DEMAND_OK");
  return leaked ? 1 : 0;
}
#endif
