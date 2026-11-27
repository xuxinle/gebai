#pragma once

/// 单实例：**命名互斥锁 + 激活信号**。
///
/// ## 它解决的是什么
///
/// 桌面应用的"第二次启动"会开第二个窗口与第二个进程，而它们共享同一份数据
/// （dev-tools 的 `~/.dev-tools/state.json`、任何应用的配置文件）与同一块屏幕。
/// 用户的心智是"点图标 = 把那个窗口叫出来"，而不是"再开一个一模一样的"。
///
/// 正确形态：第二个进程**不进入主循环**，而是让已在运行的那个窗口出来（未持有焦点
/// 时抢回前台），然后自己以 0 退出。
///
/// ## 为什么是"命名锁 + 消息"而不是"探测已有窗口"
///
/// 探测窗口要枚举顶层窗口并匹配标题/类名——那是**启发式**：标题会被改、可能同时有
/// 多个同名应用（不同用户会话），而匹配错了就会去激活别人的窗口。
/// 命名互斥锁是**内核对象**：同名即同一份，跨会话也可由前缀隔离；拿到锁意味着
/// "我是第一个"，拿不到意味着"确实已经有一个"，没有猜测空间。
///
/// ## 激活信号为什么单独一条通道
///
/// 已经有锁的进程需要**被叫醒**。用命名事件（auto-reset）而不是共享内存/临时文件：
/// 事件是内核对象，等待方可阻塞等（不轮询、不耗 CPU），且天然一对一。
///
/// ## 平台姿态
///
/// | 平台 | 锁 | 激活信号 |
/// |---|---|---|
/// | Windows | `CreateMutexW`（同名 = 同一把） | `CreateEventW` + 等待线程 |
/// | POSIX | 抽象命名信号量/文件锁 | 命名 FIFO 或 POSIX 信号 |
///
/// **不可用时如实返回**（`Unsupported`）：无头/CI 里不该因为拿不到锁就跑不起来，
/// 也不该假装成功让第二个实例静默进入主循环。

#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include "st/core/error.hpp"

namespace st::platform {

/// 单实例守门。成功构造 = **本进程是第一个**；拿到 `AlreadyRunning` = 已有实例。
class SingleInstance {
 public:
  /// 尝试成为唯一实例。
  ///
  /// `key` 应该是"应用名 + 作用域"（如 `"devtools"`）——它会成为内核对象名的一部分，
  /// 所以**不要**包含路径/用户名这些会变的东西（那会让同一应用在不同目录下被当成
  /// 两个实例）。需要区分用户会话时由调用方把会话标识拼进 `key`。
  [[nodiscard]] static auto acquire(std::string_view key) -> Result<SingleInstance>;
  /// 已有实例时，**通知它把窗口调出来**（本进程即将退出）。
  ///
  /// 返回值语义：`true` = 信号已投递（不等于对方一定响应）；`false` = 通道没建立。
  [[nodiscard]] static auto notify_existing(std::string_view key) -> bool;

  SingleInstance() = default;
  ~SingleInstance();
  SingleInstance(const SingleInstance&) = delete;
  auto operator=(const SingleInstance&) -> SingleInstance& = delete;
  SingleInstance(SingleInstance&& other) noexcept;
  auto operator=(SingleInstance&& other) noexcept -> SingleInstance&;

  /// 取走"被要求激活"的次数（取后归零）。主循环每帧调用；>0 就该 `window_restore()`。
  ///
  /// 为什么用"计数"而不是回调：主循环在任何线程都可能收到信号，而恢复窗口
  /// **必须在主线程**（Win32 的 `SetForegroundWindow` 有线程亲和性）。
  [[nodiscard]] auto take_activate_requests() -> std::uint32_t;

  [[nodiscard]] auto valid() const noexcept -> bool { return handle_ != nullptr; }

 private:
  /// 收尾：先关“叫醒通道”再 join 等待线程，最后释放唯一性句柄。
  ///
  /// 析构与移动赋值共用它——两处都要求“先按确定顺序收尾，再换手”。
  /// 抽一处的理由：顺序错一步就是**退出时挂死**（线程还在 INFINITE 等待而 join 在等它），
  /// 而那个错不会在任何单测里现形，只会在关了窗口的那一刻。
  void release() noexcept;

  /// 平台句柄：Windows = 命名互斥锁（唯一性凭据）；POSIX = 监听套接字 fd。
  void* handle_{nullptr};
  /// 激活信号的句柄（Windows = auto-reset 事件；POSIX 共用 `handle_` 的监听 fd）。
  ///
  /// 与 `handle_` 分两个字段：**职责不同**——`handle_` 回答“我是不是唯一”，
  /// `signal_` 回答“怎么被叫醒”。合成一个字段就得靠平台分支去猜它到底是什么。
  void* signal_{nullptr};
  /// “请求线程退出”的通道（Windows = auto-reset 事件；POSIX = 管道读端）。
  ///
  /// 为何必须有它：等待线程阻塞在 INFINITE（Win32）/ accept（POSIX），**没有可轮询的
  /// 检查点**。而“关闭它等待的句柄”在 Win32 上是未定义行为（实测 join 永不返回）。
  /// 所以只能让它等的东西本身也变成信号——这是停一个阻塞线程的唯一可靠手段。
  void* stop_{nullptr};
  /// 激活信号的等待线程。
  ///
  /// **存放在实例里而不是 detach**：detach 的线程在本对象析构后仍可能跑着，
  /// 而它读的事件/fd 已随析构关闭——靠“关句柄会让等待失败”侥幸不崩不算设计。
  /// 持有后可在析构里**先关句柄（叫醒等待）、再 join**，退出路径就完全确定。
  std::thread wait_thread_{};
  std::string key_{};
};

}  // namespace st::platform
