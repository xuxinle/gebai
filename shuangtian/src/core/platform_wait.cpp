// 平台边界：本文件封装高精度睡眠与句柄等待（系统 API 细节集中在此，CONVENTIONS §10.1）。
#include "st/core/wait.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(_WIN32)
// winsock2.h 必须在 windows.h **之前**（否则 winsock2.h 自己发 `#warning`，
// 而本仓库把警告当错误 —— 交叉编译当场失败）。反过来 windows.h 会拽进旧的
// winsock.h，两个头同时在场时 `SOCKET`/`WSAPoll` 等声明会互相打架。
#include <winsock2.h>
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
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

// —— 可被打断的句柄等待 ——
//
// 主循环空闲时把控制通道的监听句柄交给内核：新请求一到就返回，而不是盲睡固定拍。
// 为什么值得做：盲睡把命令落地时刻**量化到节拍边界**，实测延迟 = 帧节拍 − 距上次命令
// 的间隔（间隔 0ms → 15.7ms；8ms → 8.5ms；16ms → 3.8ms）——连续操作几乎总是白等一拍。
//
// 平台差异只在“句柄类型”上：POSIX 是 `fd`、Windows 是 `SOCKET`；两边的等待 API
// （`poll` / `WSAPoll`）语义一致，都支持毫秒超时。`EINTR` 重试是 POSIX 的必要细节——
// 信号打断会使 `poll` 返回 -1，不重试就会把“被信号吵醒”误报成“不可等”。
auto wait_handle(std::intptr_t handle, bool readable, int timeout_ms) -> bool {
  // 单句柄版保留给“等可写”这类用途；`wait_any_readable` 只等可读。
  (void)readable;
  return wait_any_readable(std::span<const std::intptr_t>(&handle, 1), timeout_ms);
}

auto wait_any_readable(std::span<const std::intptr_t> handles, int timeout_ms) -> bool {
  if (handles.empty()) return false;
#if defined(_WIN32)
  std::vector<WSAPOLLFD> entries(handles.size());
  for (std::size_t index = 0; index < handles.size(); ++index) {
    entries[index].fd = static_cast<SOCKET>(handles[index]);
    entries[index].events = POLLRDNORM;
  }
  const int ready = ::WSAPoll(entries.data(), static_cast<ULONG>(entries.size()), timeout_ms);
  return ready > 0;
#else
  std::vector<pollfd> entries(handles.size());
  for (std::size_t index = 0; index < handles.size(); ++index) {
    entries[index].fd = static_cast<int>(handles[index]);
    entries[index].events = POLLIN;
  }
  int ready = 0;
  do {
    ready = ::poll(entries.data(), static_cast<nfds_t>(entries.size()), timeout_ms);
  } while (ready < 0 && errno == EINTR);
  return ready > 0;
#endif
}

}  // namespace st::platform
