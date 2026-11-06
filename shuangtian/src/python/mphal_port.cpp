// 霜天 · MicroPython 移植层实现（对应 third_party/micropython/mphalport.h）
//
// 这里只实现四个真需要的东西：时钟、输出丢弃、输入不支持。
// 每一项都写清"为什么不给"——这些"不给"是设计决定，不是没做完。
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" {
#include "py/mpconfig.h"
}

// ── 时钟 ────────────────────────────────────────────────────────────────
//
// 用 `steady_clock` 而不是 `system_clock`：脚本里可能用 `time.ticks_ms()`
// 算时间差，而系统时钟会被 NTP 校准（可能往回跳），回跳会让差值变成负数。
namespace {

auto steady_us() -> std::uint64_t {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

extern "C" {

// ── 堆分配 ──────────────────────────────────────────────────────────────
//
// 16 MB 的 GC 堆：`new`/`malloc` 在 Windows 与 POSIX 上都是按需提交的，
// 所以“给 16 MB”不等于“占 16 MB 物理内存”。
//
// 对齐到 `alignof(std::uintptr_t)` 是 MicroPython 的要求（它把堆当指针数组用）。
//
// 这两个函数是 **MicroPython 定的 C ABI**（`mp_hal_alloc`/`mp_hal_free` 的签名
// 由解释器定，返回裸 `void*`、无权选择 RAII 类型）——它们就是“分配器实现方”本身，
// 与 `L1` 要防的“随手 new/delete 泄漏”是两回事。
auto mp_hal_alloc(size_t bytes) -> void* { return malloc(bytes); }  // lint-allow: L1 实现 mp_hal 分配器（C ABI，必须返回裸指针）
void mp_hal_free(void* ptr) { free(ptr); }  // lint-allow: L1 同上，与 mp_hal_alloc 配对

auto mp_hal_ticks_ms(void) -> std::uint64_t { return steady_us() / 1000U; }
auto mp_hal_ticks_us(void) -> std::uint64_t { return steady_us(); }
auto mp_hal_ticks_cpu(void) -> std::uint64_t { return steady_us(); }

// ── 输出：丢弃 ──────────────────────────────────────────────────────────
//
// ## 为什么丢弃而不是转给 stdout
//
// ① 宿主进程的输出流不能被污染——裁云的渲染日志与进度都走 stdout，
//    脚本一行 print 插进去会让日志解析失效。
// ② 更要紧的是**引导**：模型一旦发现 print 能用，就会开始用它做调试，
//    而真正的诊断应该走返回值或抛异常（两者都能被 C++ 侧结构化处理）。
//
// 需要看中间值时，写 `result = ...` 或 `raise ValueError(...)`。
auto mp_hal_stdout_tx_strn(const char* str, size_t len) -> std::uintptr_t {
  (void)str;
  (void)len;
  return len;
}

void mp_hal_stdout_tx_strn_cooked(const char* str, size_t len) {
  (void)str;
  (void)len;
}

// ── 输入：不支持 ────────────────────────────────────────────────────────
//
// 没有 REPL（`MICROPY_HELPER_REPL=0`），所以不会被调用。
// 返回 -1 = "没有输入"，是 MicroPython 约定的无输入信号。
auto mp_hal_stdin_rx_chr(void) -> int { return -1; }

}  // extern "C"
