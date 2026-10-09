/// 崩溃栈输出的验证程序（无头）：装框架的崩溃处理器，然后真的崩溃。
///
/// 不写成单测的原因：崩溃会终止进程，测试跑在同一进程里会带走整个测试运行器。
/// 这里做成一个**小可执行**（`crash_probe`，见 st.pkg 的 targets），由 e2e 或人工
/// 运行、检查 stderr。它是"崩溃栈能不能显示函数名"的**可重复验证入口**——
/// `-rdynamic` 一丢，这里立刻退化成裸地址。

#include <cstdio>

#include "st/core/entry.hpp"
#include "st/core/print.hpp"
#include "st/core/string.hpp"

/// 崩溃链（`noinline` 保证栈上真有这几帧；`volatile` 防优化器删掉整条链）。
///
/// ⚠ **不放匿名命名空间**：内部链接的符号不进动态符号表（`dladdr` 拿不到名），
/// 崩溃栈里会显示成 `?`。这是"验证程序"自身的坑，也提示真实项目里
/// **崩溃链上的关键函数别放匿名命名空间**（或者依赖 `.symtab` 兜底那条路）。
__attribute__((noinline)) void probe_depth_three(volatile int* pointer) { *pointer = 42; }
__attribute__((noinline)) void probe_depth_two(volatile int* pointer) {
  probe_depth_three(pointer);
}
__attribute__((noinline)) void probe_depth_one() { probe_depth_two(nullptr); }

auto run_app(int argc, char** argv) -> int {
  (void)argc;
  (void)argv;
  st::print("crash_probe: 即将触发空指针解引用（验证崩溃栈输出）\n");
  std::fflush(stdout);
  probe_depth_one();
  return 0;   // 不可达（上面必然崩）；写在这里是为了明确意图
}

ST_MAIN(run_app)
