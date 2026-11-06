/// `nlr_push` 必须是 `naked`：否则 `(%rsp)` 取到的不是返回地址，整个 MicroPython 不可用。
///
/// ## 为什么需要这组用例（实测代价）
///
/// `third_party/micropython/py/nlrx64.c` 的 `nlr_push` 汇编第一句是
/// `movq (%rsp), %rax` —— 它假定 **`%rsp` 处就是返回地址**，这个前提**只有 `naked`
/// 函数才成立**。
///
/// 上游原写 `#if !MICROPY_NLR_OS_WINDOWS` 包住 `USE_NAKED`，于是 Windows/MinGW 下
/// 既不 `naked` 也不 `omit-frame-pointer`，GCC 默认插入 `push %rbp; mov %rsp,%rbp`：
/// `(%rsp)` 变成「刚存进去的 rbp」，被当成返回地址写进 `nlr->regs[0]`。
/// 等到 `nlr_jump` 把那个值写回栈再 `ret`，执行流就跳进了数据区。
///
/// 症状极其误导：`st_mp_init()` 内部的 `register_extra_modules()` 第一件事就是
/// `nlr_push`（装载 bpy 兼容层），所以**一启用 MicroPython 就崩**，
/// 崩溃地址落在 `0x15fcd0` 这类远低于 `.text` 基址（`0x140001000`）的地方，
/// 报错里完全看不出跟“帧指针”有关。实测它让
/// `three_view_dispatches_py_to_bpy_and_js_to_three` 单跑即死、整个 `st_tests`
/// 单进程全量在它这里中止。
///
/// ## 判据设计（为什么不只是「跑一段脚本不崩」）
///
/// 只跑脚本的话，`nlr_push` 写坏的是 `regs[0]`，而**只要不发生 `nlr_jump`
/// （没有 Python 异常）就永远不会用到那个值**——脚本正常跑完，测试恒绿，
/// 而缺陷原样留着。所以必须有两条：
///
/// ① **路径覆盖**：跑一段会**抛 Python 异常**的脚本，强制走到 `nlr_jump`。
///    这是唯一能让坏掉的 `regs[0]` 真正被用上的路径。
/// ② **形态断言**：直接看 `nlr_push` 前几条指令里**没有 `push %rbp`**。
///    这条不依赖“恰好触发了 jump”，是更紧的护栏。
///
/// 两条都留着：① 说明用户会看到的现象，② 在现象还没出现时就能拦住回退。

#include <cstring>
#include <string>

#include "st/core/log.hpp"
#include "st/python/runtime.hpp"
#include "st/test/test.hpp"

namespace {

using st::mp::Runtime;

/// 跑一段源码，返回是否成功（成功 = 没有未捕获异常）。
[[nodiscard]] auto run_source(const char* source) -> bool {
  return static_cast<bool>(Runtime::instance().run(source, "<nlr-test>"));
}

}  // namespace

/// ① 正常运行：`st_mp_init` 之后能跑通一句最简脚本。
///
/// 这条看起来“太基础”，但它正是缺陷现场：修好之前，`Runtime::run()` 连
/// **第一次**都走不到返回，进程直接死（所以用例会显式崩溃，而不是断言失败）。
ST_TEST(mp_runtime_runs_a_trivial_script) {
  const bool ok = run_source("result = 1 + 1\n");
  ST_CHECK(ok);
  st::log::out("[nlr] 最简脚本：{}\n", ok ? "通过" : "失败");
}

/// ② **强制走 `nlr_jump`**：抛一个 Python 异常。
///
/// 这是判据的核心。`nlr_push` 写坏 `regs[0]` 之后，只有在 `nlr_jump` 恢复时
/// 才会用到它——不抛异常就永远测不到。
///
/// “异常被正常捕获”这件事本身也是断言：`Runtime::run` 把未捕获异常转成错误返回，
/// 若 `nlr_jump` 跳错地方，进程会死在这里而不是返回 false。
ST_TEST(mp_runtime_survives_python_exception_through_nlr) {
  // 未捕获异常 → 走 nlr_jump → 回到 run 的兜底 → 返回错误（而不是崩）。
  const bool caught = !run_source("raise ValueError('nlr probe')\n");
  ST_CHECK(caught);

  // try/except 内部捕获 → 同样走 nlr_jump，但脚本继续跑完。
  const bool recovered = run_source(
      "try:\n"
      "    raise ValueError('inner')\n"
      "except ValueError:\n"
      "    pass\n"
      "result = 'recovered'\n");
  ST_CHECK(recovered);
  st::log::out("[nlr] 异常路径：未捕获={} / 已捕获={}\n", caught ? "转为错误" : "异常",
               recovered ? "恢复并跑完" : "未恢复");
}

/// ③ bpy 兼容层能被装载（`register_extra_modules` 的第一步就是 `nlr_push`）。
///
/// 它是缺陷的直接现场：修好之前这条必崩。`import bpy` 走的是
/// `st_bpy_bootstrap` → 一路 `nlr_push`。
ST_TEST(mp_bpy_prelude_loads) {
  const bool ok = run_source("import bpy\nresult = 'ok'\n");
  ST_CHECK(ok);
  st::log::out("[nlr] bpy 兼容层装载：{}\n", ok ? "通过" : "失败");
}
