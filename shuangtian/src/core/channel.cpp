/// 执行通道：本机子进程适配 + 缺省工厂（见 `channel.hpp` 的设计说明）。

#include "st/core/channel.hpp"

#include "st/core/process.hpp"

namespace st::exec {

namespace {

/// 本机子进程通道。
///
/// 这层适配刻意薄（十几行）：`StreamHandle` 的流式语义已经就是 `Channel` 要的
/// （失败不抛、`read_line` 阻塞、`finish` 回收、`terminate` 可中止），
/// 唯一要做的是**把 `ChannelSpec` 翻成 `StreamHandle::open` 的三个参数**。
class LocalProcessChannel final : public Channel {
 public:
  void open(const ChannelSpec& spec) override {
    std::vector<std::string> argv = build_argv(spec);
    stream_.open(spec.program, argv, spec.cwd);
  }

  auto read_line(std::string& out) -> bool override { return stream_.read_line(out); }
  auto finish() -> int override { return stream_.finish(); }
  void terminate() override { stream_.terminate(); }
  [[nodiscard]] auto valid() const noexcept -> bool override { return stream_.valid(); }
  [[nodiscard]] auto error() const -> std::string override { return std::string(stream_.error()); }

 private:
  /// `ChannelSpec` → argv：`{cmd}` 占位替换；无占位按平台惯例追加（`-c` / `/c`）。
  [[nodiscard]] static auto build_argv(const ChannelSpec& spec) -> std::vector<std::string> {
    std::vector<std::string> argv;
    argv.push_back(spec.program);
    bool substituted = false;
    for (const auto& arg : spec.args) {
      if (arg == "{cmd}") {
        argv.push_back(spec.command);
        substituted = true;
      } else {
        argv.push_back(arg);
      }
    }
    if (substituted) return argv;
    if (spec.command.empty()) return argv;   // 只起交互 shell（无命令）
#ifdef _WIN32
    argv.push_back("/c");
#else
    argv.push_back("-c");
#endif
    argv.push_back(spec.command);
    return argv;
  }

  st::process::StreamHandle stream_{};
};

}  // namespace

auto make_local_channel() -> std::unique_ptr<Channel> {
  return std::make_unique<LocalProcessChannel>();
}

auto default_channel_factory() -> ChannelFactory {
  return [](const ChannelSpec& spec) -> std::unique_ptr<Channel> {
    // **只认已知 kind**：不认识的返回 nullptr 让调用方如实报错。
    // 悄悄回退到本机是最危险的一类静默错误（"以为连了远端、其实跑在本机"）。
    if (spec.kind.empty() || spec.kind == "local") return make_local_channel();
    return nullptr;
  };
}

}  // namespace st::exec
