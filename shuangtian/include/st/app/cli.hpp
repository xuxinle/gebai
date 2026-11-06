#pragma once

/// 应用的通用命令行解析：**一次写好，所有霜天应用共用**。
///
/// 为什么放进框架而不是每个应用自己写：控制通道是"应用可被智能体驱动"的入口，
/// 而它的端口与控制文件是**命令行给的**。应用若忘了解析这两个参数，
/// 自动化流程就只能去猜端口——生成的工程模板就踩过这个坑（模板没解析参数，
/// 于是 `--control-port 0 --control-file X` 被静默忽略，外部工具等不到控制通道就绪）。
///
/// 解析的选项（全部可选，未给出时保持 `AppOptions` 默认）：
///
/// | 选项 | 含义 |
/// |---|---|
/// | `--headless` | 不创建窗口（无显示环境的默认姿态） |
/// | `--backend NAME` | 显式指定后端：`headless`/`win32`/`x11`/`wayland` |
/// | `--width N` / `--height N` | 逻辑尺寸 |
/// | `--scale F` | DPI 缩放（物理像素 = 逻辑 × scale） |
/// | `--title TEXT` | 窗口标题 |
/// | `--theme MODE` | `light`/`dark`/`system` |
/// | `--theme-file PATH` | 自定义主题 JSON（稀疏覆盖，见 `st/ui/theme_io.hpp`） |
/// | `--control-port N` | 控制通道端口（`0` = 自动选空闲端口） |
/// | `--control-file PATH` | 把端口等写入该文件（自动化流程据此连接） |
/// | `--shots DIR` | 无头截图的落盘目录（控制通道 `encode=file` 且未给 `path` 时用） |
/// | `--enable-script` | 开启进程内脚本能力（默认关闭） |
/// | `--frames N` / `--ms N` | 跑够帧数/毫秒后退出（无头演示与回归用） |
///
/// 用法：
/// ```cpp
/// auto run_app(int argc, char** argv) -> int {
///   st::app::CommonOptions common;
///   if (auto parsed = st::app::parse_common_options(argc, argv, common); !parsed) {
///     std::fprintf(stderr, "%s\n", parsed.error().to_string().c_str());
///     return 1;
///   }
///   ...  // common.app 可直接交给 Application
/// }
/// ```

#include <string>

#include "st/app/app.hpp"
#include "st/core/error.hpp"

namespace st::app {

/// 通用选项解析结果。
struct CommonOptions {
  AppOptions app{};            ///< 可直接交给 `Application`（端口/控制文件/主题/脚本都在其中）
  std::uint32_t max_frames{0}; ///< 0 = 不按帧数退出
  std::uint32_t max_ms{0};     ///< 0 = 不按时间退出
  bool show_help{false};       ///< 出现 `--help`/`-h`

  /// **性能报告落盘路径**（空 = 不写）。
  ///
  /// 为何放在通用选项里：剖析的价值在于**产物**——一份能被 `st perf --compare`
  /// 对比、能被智能体直接读的 JSON。让每个应用自己接一次这件事，
  /// 结果就是“只有一个应用能产出报告”。
  std::string perf_report{};
  /// 退出时自动写报告（`--perf-report` 给出时隐含为真；`--perf-exit` 可单独要）。
  bool perf_on_exit{false};
};

/// 解析命令行。错误：`Invalid`（参数缺值/数值非法）。`--help` 不算错误（置 `show_help`）。
[[nodiscard]] auto parse_common_options(int argc, char** argv, CommonOptions& options) -> Status;

/// 把通用选项里的剖析/报告设置应用到已启动的 `Application`，并在退出时写报告。
///
/// 用法（每个应用的入口尾部照抄一行）：
/// ```cpp
/// const int code = app.run();
/// st::app::finish_with_report(app, common);   // 退出前写 --perf-report
/// return code;
/// ```
/// **不写在 `run()` 里**：报告是“这次运行的外交产物”，而 `run` 可能被自检/测试调用
/// 多次；把落盘放在入口的收尾处，语义才是“进程退出时留下证据”。
///
/// 返回是否写了文件（没配 `--perf-report` 时为 `false`）。
auto finish_with_report(Application& app, const CommonOptions& options) -> bool;

/// 打印通用选项的用法（供 `--help` 与错误提示复用）。
[[nodiscard]] auto common_options_usage(std::string_view program) -> std::string;

}  // namespace st::app
