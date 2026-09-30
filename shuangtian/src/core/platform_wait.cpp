// 平台边界：本文件封装高精度睡眠（系统 API 细节集中在此，CONVENTIONS §10.1）。
#include "st/core/wait.hpp"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace st::platform {

#if defined(_WIN32)
namespace {

/// 当前线程的高精度等待定时器（首次调用创建，之后复用）。
///
/// **每线程一个**：`SetWaitableTimer` 会覆盖同一句柄的上一次设定——多线程共用
/// 一个定时器时，一个线程的睡眠会被另一个线程的设定提前/推后（竞态）。
/// `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` 需要 Win10 1803+；老系统返回 NULL，
/// 回退 `Sleep`（粒度仍是 15.6ms，但不比现状更差）。
[[nodiscard]] auto high_resolution_timer() noexcept -> HANDLE {
  static thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  return timer;
}

}  // namespace
#endif

void sleep_ms(double milliseconds) {
  if (milliseconds <= 0.0) return;
#if defined(_WIN32)
  if (const HANDLE timer = high_resolution_timer(); timer != nullptr) {
    // 相对时间：负值，100ns 单位（最小 1 个单位避免 0 变成"不等待"）
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(milliseconds * 10000.0);
    if (due.QuadPart == 0) due.QuadPart = -1;
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != 0) {
      (void)WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
  Sleep(static_cast<DWORD>(milliseconds < 1.0 ? 1.0 : milliseconds));
#else
  std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(milliseconds));
#endif
}

}  // namespace st::platform
