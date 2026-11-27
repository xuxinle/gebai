/// 单实例：**唯一性判定 + 激活信号**。
///
/// ## 它解决的是什么
///
/// 桌面应用的"第二次启动"会开第二个窗口与第二个进程，而它们共享同一份数据
/// （`state.json`、配置文件）与同一块屏幕。用户的心智是"点图标 = 把那个窗口叫出来"，
/// 而不是"再开一个一模一样的"。
///
/// 正确形态：第二个进程**不进入主循环**，让已在运行的那个窗口出来（未持有焦点时
/// 抢回前台），然后自己以 0 退出。
///
/// ## 为什么是"内核对象"而不是"探测已有窗口"
///
/// 探测窗口要枚举顶层窗口并匹配标题/类名——那是**启发式**：标题会被改、可能同时有
/// 多个同名应用（不同用户会话），匹配错了就会去激活别人的窗口。
/// 内核对象（Win32 命名互斥锁 / POSIX 抽象命名套接字）是**确定的**：
/// 同名即同一份，没有猜测空间。
///
/// ## 为什么两个平台实现都写在这个文件里
///
/// 与本仓 `platform_pty.cpp` / `platform_time.cpp` 同构：**一个文件 + 条件编译**，
/// 不按平台拆成多个 `platform_*_xxx.cpp`。理由是可构建性——`sources` 的通配符
/// （`src/core/*.cpp`）会把同目录所有 `.cpp` 都编进来，拆文件就必须为每个平台
/// 加排除规则，而那类规则漏一条就是"重复符号"（报错指向源码，真因在清单）。
///
/// ## 激活信号为什么用独立通道
///
/// 需要**叫醒**已运行的那个进程。用可等待的内核对象（Win32 auto-reset 事件 /
/// POSIX 监听套接字）而不是共享内存或临时文件：等待方可阻塞等（不轮询、不耗 CPU）。
///
/// Windows 部分需要 `<windows.h>`；POSIX 部分需要 `<sys/socket.h>`——
/// 两者都只在对应分支里出现（`CONVENTIONS.md` §10：系统头只许出现在 `platform_*`）。
#include "st/platform/instance.hpp"

#include <atomic>
#include <cstring>
#include <format>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace st::platform {

namespace {

/// 激活请求计数（进程级）。等待线程是 **detached** 的，要活到进程结束；
/// 若把计数放进 `SingleInstance` 成员，`Application` 析构后线程就在读悬垂。
std::atomic<std::uint32_t> g_activate_requests{0};  // lint-allow: L8 进程级信号计数，等待线程 detached 且比 SingleInstance 活得久，无 Context 可注

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// Windows：命名互斥锁（判唯一）+ 命名事件（叫醒）
// ════════════════════════════════════════════════════════════════════════════
#if defined(_WIN32)

namespace {

constexpr wchar_t kMutexSuffix[] = L"-instance";
constexpr wchar_t kEventSuffix[] = L"-activate";

/// 内核对象名只允许保守字符集：把可能被解释成路径/命名空间前缀的字符换掉。
/// 这个名字只在本机内核对象命名空间里用，不需要保真。
[[nodiscard]] auto wide_object_name(const std::string& key, const wchar_t* suffix)
    -> std::wstring {
  std::wstring out;
  out.reserve(key.size() + 32);
  for (const char ch : key) {
    const bool legal = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
    out.push_back(legal ? static_cast<wchar_t>(ch) : L'_');
  }
  out += suffix;
  return out;
}

/// 等待线程：**多路等待**两个句柄——激活事件与停止事件。
///
/// 为何不是“靠关句柄叫醒”：Win32 上**关闭一个正在被等待的句柄**是未定义行为
/// （实测：join 永不返回，退出时挂死）。要停一个阻塞在 `INFINITE` 的线程，
/// 唯一可靠的办法是**让它等的东西也变成信号**，于是多等一个 stop 事件。
void wait_thread_main(HANDLE activate_event, HANDLE stop_event) {
  const HANDLE handles[2] = {stop_event, activate_event};
  while (true) {
    // `bWaitAll = FALSE`：任一信号即返回；返回 index 0 = stop。
    const DWORD waited = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (waited == WAIT_OBJECT_0) return;                          // 停止
    if (waited != WAIT_OBJECT_0 + 1) return;                      // 出错/被弃
    g_activate_requests.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace

auto SingleInstance::acquire(std::string_view key) -> Result<SingleInstance> {
  const std::string name(key);
  // ① 判唯一：命名互斥锁。创建后**一直持有**（"谁持有谁唯一"），进程退出由内核回收。
  const std::wstring mutex_name = wide_object_name(name, kMutexSuffix);
  HANDLE mutex = ::CreateMutexW(nullptr, FALSE, mutex_name.c_str());
  if (mutex == nullptr) {
    return unexpected(ErrorCode::Io,
                      std::format("单实例锁创建失败（GetLastError={}）", ::GetLastError()));
  }
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    ::CloseHandle(mutex);
    return unexpected(ErrorCode::Busy, std::format("已有实例在运行：{}", name));
  }

  // ② 建激活事件（第二个进程要用它，所以由**第一个进程**创建）。
  const std::wstring event_name = wide_object_name(name, kEventSuffix);
  HANDLE event = ::CreateEventW(nullptr, FALSE /*auto-reset*/, FALSE, event_name.c_str());
  if (event == nullptr) {
    ::CloseHandle(mutex);
    return unexpected(ErrorCode::Io,
                      std::format("激活事件创建失败（GetLastError={}）", ::GetLastError()));
  }
  // 停止事件：**不命名**（只有本进程的等待线程用，不该被别的进程看见）。
  HANDLE stop = ::CreateEventW(nullptr, FALSE /*auto-reset*/, FALSE, nullptr);
  if (stop == nullptr) {
    ::CloseHandle(event);
    ::CloseHandle(mutex);
    return unexpected(ErrorCode::Io,
                      std::format("停止事件创建失败（GetLastError={}）", ::GetLastError()));
  }
  SingleInstance instance;
  // 互斥锁句柄 = "我是唯一实例"的全部凭据；事件交给等待线程。
  instance.handle_ = mutex;
  instance.signal_ = event;
  instance.stop_ = stop;
  // **持有**线程（不 detach）：析构时用 stop 事件叫醒它、再 join，退出路径完全确定。
  instance.wait_thread_ = std::thread(wait_thread_main, event, stop);
  instance.key_ = name;
  return instance;
}

auto SingleInstance::notify_existing(std::string_view key) -> bool {
  const std::string name(key);
  const std::wstring event_name = wide_object_name(name, kEventSuffix);
  // `OpenEvent` 而不是 `CreateEvent`：后者会在"对方其实没建"时凭空造一个
  // 永不有人等的事件，把失败伪装成成功。
  HANDLE event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str());
  if (event == nullptr) return false;
  const bool ok = ::SetEvent(event) != 0;
  ::CloseHandle(event);
  return ok;
}

SingleInstance::~SingleInstance() { release(); }

// ════════════════════════════════════════════════════════════════════════════
// POSIX：抽象命名套接字（判唯一）+ 连接投递（叫醒）
// ════════════════════════════════════════════════════════════════════════════
#else

namespace {

/// 抽象命名套接字地址（Linux）/ 文件路径（macOS、BSD）。
///
/// Linux 的抽象命名空间由内核托管：进程一死名字自动消失，不落磁盘、不需清理，
/// 语义正好是"活着的实例才占着这个名字"。macOS 没有它，退回 `/tmp` 下的 socket 文件。
[[nodiscard]] auto socket_address(std::string_view key, std::size_t& length) -> sockaddr_un {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string name(key);
#if defined(__linux__)
  address.sun_path[0] = '\0';
  const std::size_t copied = std::min(name.size(), sizeof(address.sun_path) - 2);
  std::memcpy(address.sun_path + 1, name.data(), copied);
  length = offsetof(sockaddr_un, sun_path) + 1 + copied;
#else
  const std::string path = std::string("/tmp/st-instance-") + name + ".sock";
  const std::size_t copied = std::min(path.size(), sizeof(address.sun_path) - 1);
  std::memcpy(address.sun_path, path.data(), copied);
  address.sun_path[copied] = '\0';
  ::unlink(path.c_str());   // 陈旧文件先清掉（新实例要独占这个名字）
  length = sizeof(address);
#endif
  return address;
}

/// 等待连接的后台线程：用 `poll` **同时等监听 fd 与停止管道**。
///
/// 为何不直接阻塞在 `accept`：那无法被可靠中断（关闭 fd 能否唤醒阻塞中的 accept
/// 是未定义的，与 Win32 关句柄同类）。而 `poll` 能同时监控两个 fd，
/// stop 管道可写就可控退出了。
void accept_thread_main(int listen_fd, int stop_fd) {
  while (true) {
    pollfd fds[2] = {{stop_fd, POLLIN, 0}, {listen_fd, POLLIN, 0}};
    const int ready = ::poll(fds, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if ((fds[0].revents & POLLIN) != 0) return;          // 停止信号
    if ((fds[1].revents & POLLIN) == 0) continue;        // 只会是错误，回到下一轮
    const int client = ::accept(listen_fd, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      return;
    }
    char byte = 0;
    (void)::read(client, &byte, 1);
    ::close(client);
    g_activate_requests.fetch_add(1, std::memory_order_relaxed);
  }
}

/// `handle_` 的类型是 `void*`：把 fd 编码进去（+1 避开 nullptr 语义，
/// 因为 `valid()` 判的就是 `handle_ != nullptr`）。
constexpr auto encode_fd(int fd) -> void* {
  return reinterpret_cast<void*>(static_cast<std::intptr_t>(fd) + 1);
}
constexpr auto decode_fd(void* handle) -> int {
  return static_cast<int>(reinterpret_cast<std::intptr_t>(handle) - 1);
}

}  // namespace

auto SingleInstance::acquire(std::string_view key) -> Result<SingleInstance> {
  const std::string name(key);
  std::size_t address_length = 0;
  const sockaddr_un address = socket_address(name, address_length);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return unexpected(ErrorCode::Io, std::format("单实例套接字创建失败（errno={}）", errno));
  }
  // 绑定**成功** = 这个名字之前没人占 → 我是第一个。
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address),
             static_cast<socklen_t>(address_length)) != 0) {
    const int code = errno;
    ::close(fd);
    if (code == EADDRINUSE) {
      return unexpected(ErrorCode::Busy, std::format("已有实例在运行：{}", name));
    }
    return unexpected(ErrorCode::Io, std::format("单实例绑定失败（errno={}）", code));
  }
  if (::listen(fd, 4) != 0) {
    const int code = errno;
    ::close(fd);
    return unexpected(ErrorCode::Io, std::format("单实例监听失败（errno={}）", code));
  }
  // 停止管道：读端给线程，写端留在实例里（闭包时写一个字节即叫停）。
  int stop_pipe[2] = {-1, -1};
  if (::pipe(stop_pipe) != 0) {
    const int code = errno;
    ::close(fd);
    return unexpected(ErrorCode::Io, std::format("停止管道创建失败（errno={}）", code));
  }
  SingleInstance instance;
  instance.handle_ = encode_fd(fd);
  // POSIX 侧监听 fd 与停止管道读端都交给线程；`signal_` 不需要额外句柄
  // （叫醒走“连一下那个套接字”，见 `notify_existing`）。
  instance.stop_ = encode_fd(stop_pipe[1]);
  instance.wait_thread_ = std::thread(accept_thread_main, fd, stop_pipe[0]);
  // 读端只属于线程（它退出时自己不开），所以这里直接关掉本进程的读端引用：
  // 线程手里的 fd 编号已传入，close 只减引用计数——**不能**，fd 是进程级编号。
  // 所以读端由线程在使用，不在实例里再持一份；epilogue 只关写端。
  instance.key_ = name;
  return instance;
}

auto SingleInstance::notify_existing(std::string_view key) -> bool {
  const std::string name(key);
  std::size_t address_length = 0;
  const sockaddr_un address = socket_address(name, address_length);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  const bool connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                                   static_cast<socklen_t>(address_length)) == 0;
  if (connected) {
    const char ping = 'a';
    (void)::write(fd, &ping, 1);
  }
  ::close(fd);
  return connected;
}

SingleInstance::~SingleInstance() { release(); }

#endif  // _WIN32

// ════════════════════════════════════════════════════════════════════════════
// 平台无关部分
// ════════════════════════════════════════════════════════════════════════════

auto SingleInstance::take_activate_requests() -> std::uint32_t {
  // 只允许一个消费方（主循环），所以直接交换即可。
  return g_activate_requests.exchange(0, std::memory_order_relaxed);
}

void SingleInstance::release() noexcept {
  // **顺序固定**：先发停止信号（让线程离开等待）→ 再 join → 最后关各句柄。
  //
  // 为何不能“先关句柄叫醒它”：Win32 上关闭一个正在被等待的句柄是未定义行为
  // （实测：join 永不返回，退出时挂死）。POSIX 的阻塞 `accept` 同理不可靠。
  // 所以停止必须是一个**信号**，而不是“句柄消失”。
#if defined(_WIN32)
  if (stop_ != nullptr) ::SetEvent(static_cast<HANDLE>(stop_));
  if (wait_thread_.joinable()) wait_thread_.join();
  if (stop_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(stop_));
    stop_ = nullptr;
  }
  if (signal_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(signal_));
    signal_ = nullptr;
  }
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (stop_ != nullptr) {
    const char byte = 's';
    (void)::write(decode_fd(stop_), &byte, 1);
  }
  if (wait_thread_.joinable()) wait_thread_.join();
  if (stop_ != nullptr) {
    ::close(decode_fd(stop_));
    stop_ = nullptr;
  }
  signal_ = nullptr;
  if (handle_ != nullptr) {
    ::close(decode_fd(handle_));
    handle_ = nullptr;
  }
#endif
}

SingleInstance::SingleInstance(SingleInstance&& other) noexcept
    : handle_(other.handle_), signal_(other.signal_), stop_(other.stop_),
      wait_thread_(std::move(other.wait_thread_)), key_(std::move(other.key_)) {
  other.handle_ = nullptr;
  other.signal_ = nullptr;
  other.stop_ = nullptr;
}

auto SingleInstance::operator=(SingleInstance&& other) noexcept -> SingleInstance& {
  if (this == &other) return *this;
  // **先按析构的顺序收尾自己的**（关句柄 → join 线程），再接管对方的。
  // 直接覆盖会踩两个坑：目标已 joinable 的 `std::thread` 被移动赋值会 `std::terminate`；
  // 目标的旧句柄则直接泄漏。
  release();
  handle_ = other.handle_;
  signal_ = other.signal_;
  stop_ = other.stop_;
  wait_thread_ = std::move(other.wait_thread_);
  key_ = std::move(other.key_);
  other.handle_ = nullptr;
  other.signal_ = nullptr;
  other.stop_ = nullptr;
  return *this;
}

}  // namespace st::platform
