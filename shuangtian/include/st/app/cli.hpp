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
#include <string_view>
#include <utility>
#include <vector>

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

/// 通用选项里的单个开关是否**吃一个值**（`--width 800` → true；`--headless` → false）。
///
/// 为何要在一个公开头里回答这个问题：`parse_options` 要先把参数流**切成令牌**
/// （应用参数与通用参数混排时，必须知道“下一个 argv 到底是值还是另一个开关”）。
/// 而“哪些开关带值”这个知识**只能有一份**——若在应用侧再写一份表，
/// 两份一旦分家，症状就是“某个开关把下一个参数当成了自己的值”（静默且难查）。
/// 所以这里把那个知识从实现里导出来，而不是让调用方猜。
[[nodiscard]] auto common_option_takes_value(std::string_view name) noexcept -> bool;

// ————————————————————————————————————————————————————————————————————————————
// 应用的**自有参数**：声明式登记，由框架统一解析
// ————————————————————————————————————————————————————————————————————————————
//
// ## 为何要有这一层
//
// 本仓曾出现两种相反的病，都是**“各写一套解析器”直接造成的**：
//
// 1. **重复实现导致行为分家**：框架给 `parse_common_options` 加了一个开关，
//    自己解析的应用不认识它 → 被静默吃掉。实测事故：`--renderer` 之外新加的
//    开关没生效，三档渲染出来的图几乎一样（只差 20 像素），差点被当成
//    “三档观感接近”的结论；
// 2. **反向缺口**：`--perf-report` / `--log-file` 这类诊断开关在只实现了一半的
//    应用里直接报“未知参数”，于是“只有一个应用能产出性能报告”。
//
// 做法：应用用下表**声明**自己的开关（名字 / 是否带值 / 允许的取值 / 说明），
// 框架统一步进、统一报错、统一 `--help`。应用不再手写 `argv` 循环——
// “哪些参数存在”只有一份事实（表），解析与文档不可能再分家。

/// 一条应用自有参数。
///
/// 只需声明**名字与是否带值**：取值怎么解释由应用在回调里做
/// （`--workspace .` 与 `--paused` 的语义差得很远，框架不猜）。
struct AppOption {
  /// 参数名，**必须带前导 `--`**（`-x` 短选项不走这条）。
  std::string_view name{};
  /// 吃掉后面几个值（`--name value` = 1；`--pack A B` = 2；`0` = 布尔开关）。
  ///
  /// 为何用计数而不是 `takes_value` 一个布尔：有的参数天然是**一对**
  /// （`--snapshot <帧目录> <输出目录>`、`--diff-snap <A> <B>`）。
  /// 把这种参数强行拆成两个开关会改变命令行语法（而脚本与文档早已在用双值形式），
  /// 让应用自己解析又会把“切令牌”的知识拆成两份——正是本设计要消灭的东西。
  std::size_t value_count{0};
  /// `--help` 里那一行说明（必须写，否则用户只能读源码）。
  std::string_view help{};
  /// 可选的取值白名单（对**第一个**值生效）。非空时**拼错就报错**。
  ///
  /// 为何默认不白名单：`--workspace DIR` / `--script PATH` 这种取值空间是“任意
  /// 路径”，列不出来；而 `--renderer` / `--text-lcd` 这类枚举必须列——静默回落会让
  /// “我明明指定了 GPU”变成谜案。两类的差别很大，所以由应用显式声明。
  std::vector<std::string_view> allowed{};

  /// 兼容旧写法：`takes_value = true` 等价于 `value_count = 1`。
  ///
  /// 保留它是因为“布尔还是带值”是绝大多数参数的形态，写 `value_count = 1` 噪音更大；
  /// 而双值参数只是少数（本仓目前只有裁云的三条）。
  struct bool_tag {};
};

/// 声明式地写一条“带 1 个值”的参数（比写 `value_count = 1` 更接近口语）。
[[nodiscard]] constexpr auto takes_value(std::string_view name, std::string_view help,
                                         std::vector<std::string_view> allowed = {})
    -> AppOption {
  return AppOption{.name = name, .value_count = 1, .help = help, .allowed = std::move(allowed)};
}

/// 声明式地写一条“多值”参数。
[[nodiscard]] constexpr auto takes_values(std::string_view name, std::size_t count,
                                          std::string_view help) -> AppOption {
  return AppOption{.name = name, .value_count = count, .help = help};
}

/// 声明式地写一条布尔开关。
[[nodiscard]] constexpr auto flag(std::string_view name, std::string_view help) -> AppOption {
  return AppOption{.name = name, .value_count = 0, .help = help};
}

/// 解析结果：应用自有参数的取值。
///
/// 用 `values` 而不是给每个参数生成一个字段：应用侧的字段名是它的领域词汇
/// （`workspace` / `scene` / `record_dir`），框架不该认识它们。
/// 读法：`values.value_at("--workspace")`（无值时为空串）。
struct AppOptionValues {
  /// 参数名 → 取值**列表**（布>/开兺为空列表，以“存在”表示开关被打开）。
  ///
  /// 多值参数（`--pack A B`）占两项；同一参数出现多次则追加——
  /// “后写胜”由调用方用 `last()` 表达（它有明确语义，而“取第一个”没有）。
  std::vector<std::pair<std::string, std::vector<std::string>>> values{};

  [[nodiscard]] auto has(std::string_view name) const -> bool;
  /// 取第一个匹配参数的**第一个**值；未给出时返回 `fallback`。
  [[nodiscard]] auto get(std::string_view name, std::string_view fallback = {}) const
      -> std::string;
  /// 取**最后一次**出现的那组值（“后写胜”）。
  [[nodiscard]] auto last(std::string_view name) const -> std::vector<std::string>;
  /// 位置取值：第 `index` 个值（越界返回空串）。
  [[nodiscard]] auto value_at(std::string_view name, std::size_t index) const
      -> std::string;
};

/// **统一入口**：一次解析「框架通用开关 + 应用自有开关」。
///
/// 语义：
/// - 框架认识的（见 `parse_common_options`）落到 `common`；
/// - 应用在 `app_options` 里声明的落到 `values`；
/// - **两边都不认识的 → 报错**，且错误信息里点名该参数。
///   这条不能放松：静默忽略未知参数正是“新开关被吃掉”那类事故的温床。
/// - `--help` 置 `common.show_help`，不影响解析成功（由调用方决定打印什么）。
///
/// 为何不把应用参数也收进 `CommonOptions`：那一来框架就认识 `--workspace` 了
/// ——而它只对 gbcode 有意义。参数的**归属**必须与“谁在用”一致，否则下一个应用
/// 要求再加一个字段时，开始那一句“通用”就不成立了。
[[nodiscard]] auto parse_options(int argc, char** argv, const std::vector<AppOption>& app_options,
                                 CommonOptions& common, AppOptionValues& values) -> Status;

/// 把「通用选项 + 应用选项」拼成完整用法文本。
///
/// 两段分开列：用户要能一眼看出“哪些是所有霜天应用都有的、哪些是这个应用特有的”。
[[nodiscard]] auto options_usage(std::string_view program,
                                 const std::vector<AppOption>& app_options) -> std::string;

}  // namespace st::app
