#pragma once

/// `st perf` 的请求参数（`perf_command.cpp` **唯一**的输入类型）。
///
/// 为何不复用 `main.cpp` 里的 `Arguments`：它住在匿名命名空间里（那是命令分发器
/// 的私有类型），跨 TU 引用不了。把需要的字段收成一个显式结构反而更好——
/// 这样 `perf_command.cpp` 的依赖面是**看得见的三行**，而不是一个万能 map。

#include <cstdint>
#include <string>
#include <vector>

namespace stpkg_cli {

/// 一次 `st perf` 请求。
struct PerfRequest {
  std::string target{};            ///< 目标名（空 = 只看 --compare）
  std::string profile{"release"};  ///< 构建/运行档
  std::uint32_t frames{200};       ///< 剖析采样帧数
  std::uint32_t bench{0};          ///< >0 时给应用加 `--bench N`（强制全量重绘）；
                                   ///< 未给时默认用 `frames`（静止界面拿不到样本）
  bool bench_given{false};         ///< 调用方显式给了 `--bench`（含 `--bench 0` = 量真实交互）
  std::string out{};               ///< 报告落盘路径（空 = 默认 tmp/perf-<target>.json）
  std::string baseline{};          ///< 与这份旧报告对比
  std::string compare{};           ///< `旧.json,新.json`：只比两份已存报告
  double threshold{0.05};          ///< 对比阈值（5% 以下视为噪声）
  bool as_json{false};             ///< 报告以 JSON 打到 stdout
};

/// 执行一次性能诊断（定义在 `perf_command.cpp`）。
[[nodiscard]] auto command_perf(const PerfRequest& request) -> int;

}  // namespace stpkg_cli
