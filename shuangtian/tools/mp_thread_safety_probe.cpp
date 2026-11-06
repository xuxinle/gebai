// mp_thread_safety_probe —— MicroPython 运行时**跨线程共享解释器**会不会崩？
//
// ## 为什么要问（取证背景）
//
// `tests/ui_three_view_test.cpp` 的 `three_view_dispatches_py_to_bpy_and_js_to_three`
// 会单跑就崩（退出码 `0xC0000005`）。gdb 抓到的栈是：
//
// ```
// Thread 5 received signal SIGSEGV
// #0 0x000000000237ef60 in ?? ()      ← 落到匿名地址（数据区），不是代码
// #1 0x000000000237eef0 in ??
// ```
//
// 那种「跳进数据区」的形态是**栈/堆被写坏**的典型表现，而不是某个函数返回了空指针。
// 而 `src/python/runtime.cpp` 的注释自己写着：
//   「MicroPython 的解释器状态是进程全局的（`mp_state_ctx`），同一时刻只能有一个
//     活着的解释器。若两个线程各建一个，会互相踩堆——而症状是随机的内存破坏」
// ——本测试进程里恰好有工作线程（`Tasks` 池）。
//
// 于是本探针要回答三件事，每件都要有能复现的数字：
//   ① 解释器**只在主线程**跑（串行，`mp_init`/`mp_run`/`mp_deinit` 全在同一线程）→ 会崩吗？
//   ② 从**两个线程**并发跑 → 会崩吗？
//   ③ `Runtime::available()` 在「已被别的线程占用」时是否**如实**返回 false？
//      （若它返回 true，调用方就没有任何手段在崩之前退让——那本身就是一个缺陷）
//
// ## 编译与运行
//
//   st build mp_thread_safety_probe --profile debug
//   build/debug/bin/mp_thread_safety_probe.exe
//
// 判据：① 必须过；② 崩或不崩都要如实打印（不是断言它一定崩）；
//       ③ `available()` 必须与「解释器真被占用」一致——这条是**可修的缺陷**的判据。

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "st/python/runtime.hpp"

namespace {

/// 一段最小 bpy 风格脚本（与真实用例同类：import + 建对象 + 取名）。
constexpr const char* kScript = R"py(
import bpy
bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 0.0, 0.0))
obj = bpy.context.active_object
obj.name = 'probe_cube'
result = 'ok'
)py";

[[nodiscard]] auto run_once(const char* tag) -> bool {
  auto& runtime = st::mp::Runtime::instance();
  auto result = runtime.run(kScript, "<probe>");
  if (!result) {
    std::printf("    %s: run 失败: %s\n", tag, result.error().message.c_str());
    return false;
  }
  return true;
}

}  // namespace

int main() {
  std::printf("==== MicroPython 跨线程共享探测 ====\n");
  std::printf("available=%d  reason='%s'\n", st::mp::Runtime::available() ? 1 : 0,
              st::mp::Runtime::unavailable_reason().c_str());

  // ① 单线程串行：同一线程里多次 run（第一次会 mp_init）。
  std::printf("\n[①] 单线程连续 5 次\n");
  bool all_ok = true;
  for (int i = 0; i < 5; ++i) {
    const bool ok = run_once(("run#" + std::to_string(i)).c_str());
    all_ok = all_ok && ok;
  }
  std::printf("  单线程结果：%s\n", all_ok ? "全部成功" : "有失败");

  // ③ `available()` 的判据：解释器**在本线程**活着时，它应该仍然说「可用」
  //    （本线程就是持有者）；而**别的线程**问时必须说「不可用」。
  std::printf("\n[③] available() 与「持有者线程」的一致性\n");
  std::printf("  本线程（持有者）问：available=%d（期望 1）\n",
              st::mp::Runtime::available() ? 1 : 0);
  {
    // 另开一个线程问：解释器此时被主线程持有（刚 ① 里 init 过）。
    std::atomic<int> seen{-1};
    std::thread other([&] { seen.store(st::mp::Runtime::available() ? 1 : 0); });
    other.join();
    std::printf("  另一线程问：available=%d（期望 0——否则调用方无从退让）\n", seen.load());
    if (seen.load() != 0) {
      std::printf("  ⚠ 判据不符：`available()` 在解释器被别的线程持有时仍报可用。\n"
                  "    调用方据此认为可以跑，随后与持有者互踩堆——正是本用例崩溃的形态。\n");
    }
  }

  // ② 两个线程并发跑（预期：崩或报错；两者都如实打印）。
  std::printf("\n[②] 两线程并发 run（本进程会崩也属预期，如实记录）\n");
  std::fflush(stdout);
  std::atomic<int> ok_count{0};
  std::atomic<int> err_count{0};
  std::vector<std::thread> workers;
  for (int t = 0; t < 2; ++t) {
    workers.emplace_back([&, t] {
      for (int i = 0; i < 20; ++i) {
        auto& runtime = st::mp::Runtime::instance();
        auto result = runtime.run(kScript, "<probe>");
        if (result) {
          ok_count.fetch_add(1);
        } else {
          err_count.fetch_add(1);
        }
      }
      std::printf("    线程 %d 结束：ok=%d err=%d\n", t, ok_count.load(), err_count.load());
      std::fflush(stdout);
    });
  }
  for (auto& worker : workers) worker.join();
  std::printf("  并发结果：ok=%d err=%d（走到这里说明没崩）\n", ok_count.load(), err_count.load());
  return 0;
}
