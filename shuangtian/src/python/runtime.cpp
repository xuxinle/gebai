// 霜天 · MicroPython 嵌入宿主（C++ 侧）
//
// ## 这一层与 C 胶水层的分工
//
// | 层 | 职责 |
// |---|---|
// | `src/python/mp_bridge.c`（C） | 唯一接触 MicroPython 头的地方；做 init/run/deinit |
// | 本文件（C++） | 线程内单例、错误语义包装、给调用方一个干净的 `Result<RunResult>` |
//
// 分开的理由见 `include/st/python/mp_bridge.h`：MicroPython 的头是 C99 的
// （弹性数组/零长数组），C++ 直接 include 会触发 `-Werror=pedantic` 硬错误。
#include "st/python/runtime.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "st/python/mp_bridge.h"

namespace st::mp {
namespace {

/// 结果 / 错误缓冲的初值。
///
/// ## 为什么用固定缓冲而不是动态增长
///
/// `st_mp_run` 的签名是"C 缓冲 + 容量"，它要能在**异常路径**里也写错误信息，
/// 而异常路径上做动态分配是不安全的（可能正好是内存耗尽触发的异常）。
///
/// 8 KB 足够放一个场景值的 `str()`（bpy 抽取器不会把整个网格塞进 result）；
/// 超长会截断，而截断点之后的内容对诊断没有价值。
constexpr std::size_t kBufferBytes = 8192;

/// 线程内单例的"是不是我"检查。
///
/// MicroPython 的解释器状态是进程全局的（`mp_state_ctx`），同一时刻只能有一个
/// 活着的解释器。若两个线程各建一个，会互相踩堆——而症状是随机的内存破坏，
/// 比崩溃难查得多。所以用原子变量记录持有者。
std::atomic<std::thread::id> g_owner{};

}  // namespace

struct Runtime::Impl {
  bool initialized{false};
};

Runtime::Runtime() {
  impl_ = new Impl();
}

Runtime::~Runtime() {
  if (impl_ != nullptr && impl_->initialized) {
    st_mp_deinit();
    impl_->initialized = false;
    g_owner.store(std::thread::id());
  }
  delete impl_;
}

auto Runtime::available() -> bool {
  return unavailable_reason().empty();
}

auto Runtime::unavailable_reason() -> std::string {
  const std::thread::id owner = g_owner.load();
  if (owner != std::thread::id() && owner != std::this_thread::get_id()) {
    return "MicroPython 解释器已被另一个线程占用（它是进程级单例，不能多线程共享）";
  }
  return {};
}

auto Runtime::instance() -> Runtime& {
  thread_local Runtime runtime;
  return runtime;
}

auto Runtime::run(std::string_view source, std::string_view filename) -> Result<RunResult> {
  if (!impl_->initialized) {
    const std::thread::id self = std::this_thread::get_id();
    std::thread::id expected{};
    if (!g_owner.compare_exchange_strong(expected, self) && expected != self) {
      return st::unexpected(ErrorCode::Unsupported, unavailable_reason());
    }
    std::vector<char> error(kBufferBytes, '\0');
    if (st_mp_init(error.data(), error.size()) != 0) {
      g_owner.store(std::thread::id());
      return st::unexpected(ErrorCode::Unsupported, std::string(error.data()));
    }
    impl_->initialized = true;
  }

  std::vector<char> value(kBufferBytes, '\0');
  std::vector<char> error(kBufferBytes, '\0');
  const int status = st_mp_run(source.data(), source.size(), filename.data(), filename.size(),
                              value.data(), value.size(), error.data(), error.size());
  if (status == 0) {
    return RunResult{std::string(value.data()), {}};
  }
  if (status == 1) {
    return st::unexpected(ErrorCode::Invalid, std::string(error.data()));
  }
  return st::unexpected(ErrorCode::Unsupported, "Python 解释器未初始化");
}

}  // namespace st::mp
