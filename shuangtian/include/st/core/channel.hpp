/// 执行通道（Channel）：把「一条命令跑在哪、怎么读写」抽象出来。
///
/// ## 为什么要有这层
///
/// 「执行一条命令」的形态不止一种：本机子进程、SSH 远端 shell、容器 `exec`、
/// 测试用的假通道……如果调用方直接持有 `st::process::StreamHandle`，
/// **通道就被写死在调用方里**了——换一种通道要改的是每一个用到处，
/// 而不是加一个类。
///
/// 这一层把差异收成**一个接口 + 一个描述结构**：
///
/// ```
/// 调用方（终端组件 / 任务系统 / …）
///    │  只认 Channel
///    ▼
/// Channel ── LocalProcessChannel   （本机子进程，本文件提供）
///         ├─ 宿主自定义：SSH / 容器 / 假通道（测试回放）
/// ```
///
/// ## 与 `st::process::StreamHandle` 的关系
///
/// `StreamHandle` 是**本机子进程**这条通道的实现细节，不是接口本身
/// （它已经提供的流式语义是对的：`open` 不抛异常、`read_line` 阻塞、
/// `finish` 回收退出码——`Channel` 照搬这套约定，于是本地通道几乎是一层薄壳）。
///
/// ## 约定（三条，都有实际理由）
///
/// 1. **失败不抛异常**：`open` 之后用 `valid()` 判定，`error()` 取原因。
///    执行失败是**预期内的结果**（命令不存在、主机连不上），不是一个异常事件。
/// 2. **`read_line` 阻塞**：它是给**工作线程**用的。调用方（如终端组件）
///    负责起线程、把行搬回界面线程——通道不碰线程模型。
/// 3. **`terminate` 之后 `read_line` 要尽快返回**：否则"中止"这个动作在界面上
///    是假的（用户点了中止，线程还堵在读里）。

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace st::exec {

/// 一次连接的描述（宿主填、通道读）。
///
/// `kind` 是**给工厂看的**：工厂按它挑实现（`"local"` → 本机子进程）。
/// 其余字段是通用槽位——通道实现只取自己认识的那些，多出来的忽略。
struct ChannelSpec {
  /// 通道类型（`"local"` / `"ssh"` / 宿主自定义）。
  std::string kind{"local"};
  /// 要执行的东西：`local` 下是 shell 程序（`/bin/bash`、`cmd.exe`）。
  std::string program{};
  /// 程序参数。`{cmd}` 占位会被替换为命令行；无占位时按平台惯例追加（`-c` / `/c`）。
  std::vector<std::string> args{};
  /// 工作目录（空 = 继承当前进程）。
  std::string cwd{};
  /// 要执行的命令行（`local` 下由 shell 解释）。
  std::string command{};

  // —— 远端通道用的槽位（`local` 忽略）——
  /// 主机（`ssh` 的 `user@host` 形态）。
  std::string host{};
  std::uint16_t port{0};
  std::string user{};
  /// 额外选项（远端通道自定义：密钥路径、跳板机…）。
  std::vector<std::string> options{};
};

/// 执行通道。
///
/// 生命周期：`open` → （多次 `read_line`）→ `finish`（或 `terminate`）。
/// `open` 之前与 `finish` 之后的状态不保证（实现可以把句柄放掉）。
class Channel {
 public:
  Channel() = default;
  virtual ~Channel() = default;
  Channel(const Channel&) = delete;
  auto operator=(const Channel&) -> Channel& = delete;
  Channel(Channel&&) = delete;
  auto operator=(Channel&&) -> Channel& = delete;

  /// 建立连接并（可选地）执行 `spec.command`。
  ///
  /// 失败**不抛异常**：调完用 `valid()` 判定。这样"命令不存在""主机连不上"
  /// 这些**预期内**的结果和真正的程序错误区分得开。
  virtual void open(const ChannelSpec& spec) = 0;

  /// 读一行（不含行尾）。返回 false = 流已结束（EOF / 出错）。
  ///
  /// **会阻塞**——给工作线程用，不要在界面线程调。
  virtual auto read_line(std::string& out) -> bool = 0;

  /// 等待结束并回收资源；返回退出码（异常终止返回 128+signal）。
  virtual auto finish() -> int = 0;

  /// 请求终止。“中止”按钮的实现。
  ///
  /// 契约：**调用后 `read_line` 要尽快返回 false**——否则中止在界面上是假的。
  virtual void terminate() = 0;

  [[nodiscard]] virtual auto valid() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto error() const -> std::string = 0;

  /// 这条通道是否支持在**同一条连接**上再发一条命令（会话式）。
  ///
  /// 本机子进程通道：false（一条命令一个进程，跑完即止）。
  /// SSH 连接复用、PTY 会话：true。终端组件据此决定"新命令是复用还是重开"。
  [[nodiscard]] virtual auto is_session_reusable() const noexcept -> bool { return false; }
};

/// 本机子进程通道（缺省通道；`st::process::StreamHandle` 的薄适配）。
///
/// 单独成一个类而不是让 `StreamHandle` 继承 `Channel`：后者是 core 里被多处
/// 直接使用的具体类型，把接口塞进它会让**所有**用到处都跟着依赖这套虚函数；
/// 而适配层只有十几行。
[[nodiscard]] auto make_local_channel() -> std::unique_ptr<Channel>;

/// 通道工厂：按 `spec.kind` 造通道。宿主注入给组件（换通道 / 测试桩）。
using ChannelFactory = std::function<std::unique_ptr<Channel>(const ChannelSpec&)>;

/// 缺省工厂：认识 `"local"`，其余 `kind` 返回 nullptr（并让调用方如实报错，
/// 而不是悄悄回退到本机——"以为连了远端、其实跑在本机"是最危险的一类静默错误）。
[[nodiscard]] auto default_channel_factory() -> ChannelFactory;

}  // namespace st::exec
