// mp_reinit_probe —— 只需一次 `run()` 就能复现的崩溃：解释器的**进程级静态量**与
// `Runtime` 的**每线程实例**口径不一致。
//
// ## 已确认的事实链
//
// | 位置 | 是什么 | 口径 |
// |---|---|---|
// | `src/python/mp_bridge.c` 的 `s_initialized` / `s_heap` / `s_globals` | MicroPython 解释器状态 | **进程级静态量** |
// | `src/python/runtime.cpp` 的 `thread_local Runtime runtime` | `Runtime` 实例 | **每线程一份** |
// | `Runtime::Impl::initialized` | 该实例是否初始化过 | 每线程 |
//
// 于是：**第二个碰 MicroPython 的线程**看到自己那份 `initialized == false`，
// 就再调一次 `st_mp_init`——而 `s_initialized` 是 1，函数**立刻返回 0**（看起来成功），
// 但那个线程的 `Impl::initialized` 被置真。之后该线程 `run()` 时用的是**别的线程
// 建的解释器状态**（它是栈/堆上的 `mp_state_ctx`），互踩堆。
//
// 本探针把这条链变成可复现的三步：
//   ① 主线程 `run()` 一次 → 解释器建好（`s_initialized = 1`）；
//   ② 另一个线程 `run()` 一次 → 看见 `initialized == false` → 重复"初始化"；
//   ③ 该线程结束，`Runtime` 的 `thread_local` 析构 → `st_mp_deinit()`
//      → **把主线程还在用的堆 free 掉**。
//
// ③ 是最后一击，而且它解释了为什么崩溃表现是"跳进数据区"（`0x15f8e0` 之类）：
// 堆已被释放/重写，`.text` 之外随便什么内容都可能被当成返回地址。
//
// ## 为什么不用标准 gtest 风格的断言
//
// 本探针**要**允许自己崩：它量的是"崩在不在、崩在哪一步"。所以顺序打印每一步，
// 并把 `std::fflush` 放在每次打印之后——否则崩溃时 stdio 缓冲全丢，什么都看不到。

#include <cstdio>
#include <string>
#include <thread>

#include "st/python/runtime.hpp"

namespace {

constexpr const char* kScript = "result = 1 + 1\n";

void say(const char* text) {
  std::printf("%s\n", text);
  std::fflush(stdout);
}

}  // namespace

int main() {
  say("==== MicroPython 重复初始化 / 跨线程收尾 探测 ====");
  std::printf("available=%d\n", st::mp::Runtime::available() ? 1 : 0);
  std::fflush(stdout);

  say("[①] 主线程 run 一次（建解释器）");
  {
    auto result = st::mp::Runtime::instance().run(kScript, "<main>");
    std::printf("    主线程 run: %s\n", result ? "成功" : "失败");
    std::fflush(stdout);
  }

  say("[②] 另一个线程 run 一次（会看到自己的 initialized==false → 再调 st_mp_init）");
  {
    std::thread worker([] {
      auto result = st::mp::Runtime::instance().run(kScript, "<worker>");
      std::printf("    worker run: %s\n", result ? "成功" : "失败");
      std::fflush(stdout);
    });
    worker.join();
    say("    worker 已 join（它的 thread_local Runtime 已析构 → 调过 st_mp_deinit）");
  }

  say("[③] 主线程再用一次：若堆已被 worker 的析构 free 掉，这里就是崩溃点");
  {
    auto result = st::mp::Runtime::instance().run(kScript, "<main-again>");
    std::printf("    主线程再次 run: %s\n", result ? "成功" : "失败");
    std::fflush(stdout);
  }

  say("==== 全程未崩 ====");
  return 0;
}
