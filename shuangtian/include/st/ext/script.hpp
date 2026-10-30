#pragma once

/// 脚本引擎：**基于 QuickJS**（`third_party/quickjs/`，quickjs-ng 0.17.0，MIT）。
///
/// 定位：给应用与智能体一个**受控的表达式/逻辑层**——主题计算、界面联动、批量属性变换
/// 这类"用代码描述比用配置描述更短"的场景。它是 ui/core 层的可选依赖，静态库默认链接。
///
/// ## 安全姿态（这是本模块最需要读的一段）
///
/// 脚本 = 可执行代码，能力边界必须显式设计，不能"因为方便就默认打开"：
///
/// | 面 | 本封装的处置 |
/// |---|---|
/// | 系统访问（文件/进程/socket） | **不提供**。上游的 `quickjs-libc.c`（`std`/`os` 模块）已被刻意剔除（见 `third_party/SOURCES.md`），脚本里 `require('os')` 之类无从谈起 |
/// | 内存 | 运行时级硬上限（`JS_SetMemoryLimit`），超限即抛 JS 异常而非拖垮进程 |
/// | 栈 | 运行时级上限（`JS_SetMaxStackSize`），防深递归爆栈 |
/// | 执行时长 | 中断回调按截止时间打断（`JS_SetInterruptHandler`），死循环不会挂死调用方 |
/// | 宿主能力 | 只暴露**显式注册**的函数（`register_function`），默认全局是干净 JS 环境 |
/// | 与界面/协议的关系 | 控制通道的 `script` 方法**默认禁用**，需显式开启（见 `AppOptions::enable_script`） |
///
/// 换句话说：**引擎本身不主动给任何宿主能力**，能做什么完全取决于调用方注册了什么。
///
/// ## 错误与数值语义
///
/// - 一切失败都经 `Result` 返回（不抛异常）；JS 异常带消息与行号；
/// - 数值：JS 只有 IEEE double（`BigInt` 除外），因此 `Json` ↔ JS 转换时——
///   能被 `double` **精确表示**的整数转成 JSON 整数，超出范围的按浮点处理（并在文档标注，
///   避免"大整数在 JS 里悄悄失真"。需要 64 位整数请传字符串或 `BigInt`）。
///
/// 线程模型：一个 `ScriptEngine` **绑定一个线程**（QuickJS 运行时非线程安全），不要跨线程共享。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
// `Json` 只出现在签名里 → 用**前向头**（不拉入 nlohmann 的 25,526 行）。
// 需要构造/访问 Json 的 .cpp 自行包含 `st/core/json.hpp`。见 `st/core/json_fwd.hpp`。
#include "st/core/json_fwd.hpp"

namespace st::ext {

/// 引擎状态（定义在 `src/ext/script.cpp`）。
///
/// 之所以放在命名空间层而不是类的私有嵌套类型：实现文件里的 QuickJS 回调必须是**自由函数**
/// （C 函数指针），它们需要能命名这个类型；私有嵌套类型只有成员与友元能命名。
namespace detail {
struct ScriptState;
}  // namespace detail

/// 资源配额（构造时确定，生命周期内不变——配额是"这个脚本能干什么"的一部分，不该被临时放大）。
struct ScriptLimits {
  /// 运行时内存上限（字节）。默认 16 MiB：够写几千行逻辑，不够把进程吃光。
  std::size_t memory_bytes{16U * 1024U * 1024U};
  /// 栈上限（字节）。默认 512 KiB：够常规递归，深递归会被拦下。
  std::size_t stack_bytes{512U * 1024U};
  /// 单次 `eval`/`call` 的执行时限。默认 1000 ms。
  std::chrono::milliseconds timeout{1000};
  /// 单次执行允许的最长输出嵌套深度（`Json` ↔ JS 转换用，防循环引用无限递归）。
  std::uint32_t max_convert_depth{32};
  /// 单次执行允许的**中断轮询次数**上限（0 = 不限）。
  ///
  /// 名字如实反映它数的是什么：QuickJS 在解释循环里周期性调用中断回调，这里计的是**回调次数**，
  /// 不是 VM 指令数。实测约 2千次/秒（随版本与代码形态浮动），因此它是**粗粒度兜底**——
  /// 用于"同样的脚本在任何机器上都以同样的轮询次数为界"这种可复现场景；
  /// **执行时长的主力控制是 `timeout`**，别拿本项当精细配额。
  std::uint64_t max_interrupts{0};
};

/// 把 JS 异常描述**整形**成可行动的文案（纯函数，可直接单测）。
///
/// ## 为何单独抽出来（2026-10-14）
///
/// 这条整形逻辑原先埋在 `describe_exception` 里（要 `JSContext*`），于是一条“文案能不能读懂”
/// 的断言只能通过**真跑脚本撞 OOM** 来触发——而**同引擎的 OOM 表现随构建档不同**：
/// release 档产出真异常、文案是英文 `InternalError: out of memory`；dev 档下异常对象
/// 不可用（各走一条分支）。于是同一个缺陷在一档红、一档绿，而且它让测试**无法定向验证**
/// ——实测：把识别代码改成 `if (false)` 后用例仍然 PASS（因为被另一条分支满足了）。
///
/// 抽成纯函数后：两条路径的文案都能**直接断言**，与构建档无关。
///
/// @param raw    引擎给的原始描述（空 = 异常对象不可用）
/// @param limits 内存上限（用于把上限值写进提示）
[[nodiscard]] auto describe_script_failure(std::string_view raw, const ScriptLimits& limits)
    -> std::string;

/// 宿主函数：接收 JS 传入的参数（已转成 `Json`），返回结果（转回 JS）。
///
/// 返回 `Result<Json>`：宿主侧的失败（如"没有这个文件"）应作为**错误**回给脚本，
/// 而不是伪装成一个值——脚本可以用 try/catch 接住并决定怎么办。
using ScriptHostFunction = std::function<Result<Json>(const std::vector<Json>&)>;

/// **二进制宿主函数**：接收 `Float32Array` 直传的数据。
///
/// ## 为什么需要这条旁路（实测数据说话）
///
/// 走 `Json` 那条路时，脚本传 1 个顶点 = 3 个 `Json` 数字对象 + 递归转换。
/// 实测 **3.5 µs/顶点**：10 万顶点要 350 ms。对"一次性生成"够用，
/// 但对**每帧重算**（过程化动画）就是 3 fps——不可用。
///
/// 二进制旁路：JS 侧填好 `Float32Array`，宿主直接把那块内存按 `span<const float>` 读走，
/// 成本从"每元素一次对象分配"降到"每缓冲一次取指针"。
///
/// ## 为什么是"若干个 Float32Array"而不是通用 ArrayBuffer
///
/// 过程化几何需要的是**几组平行的 float**（位置/法线/颜色），不是任意字节流。
/// 接口按用途收窄，调用方的错误面就小：传错类型立即报错，
/// 而不是拿到一堆乱字节再在深处崩。
using ScriptBinaryHostFunction =
    std::function<Result<Json>(const std::vector<std::span<const float>>& arrays)>;

/// 二进制宿主函数可接收的数组数量上限。
inline constexpr std::size_t kMaxBinaryArrays = 8;

/// 一次执行的统计（供日志/测试观测资源使用）。
struct ScriptStats {
  std::size_t memory_used{0};
  std::uint64_t ops{0};
  std::chrono::microseconds elapsed{0};
  bool timed_out{false};
};

/// 嵌入式脚本引擎（每实例一个 QuickJS 运行时 + 上下文）。
class ScriptEngine {
 public:
  ScriptEngine();
  explicit ScriptEngine(ScriptLimits limits);
  ~ScriptEngine();
  ScriptEngine(const ScriptEngine&) = delete;
  auto operator=(const ScriptEngine&) -> ScriptEngine& = delete;
  ScriptEngine(ScriptEngine&&) noexcept;
  auto operator=(ScriptEngine&&) noexcept -> ScriptEngine&;

  /// 执行脚本并取回返回值（`undefined` → `null`）。
  /// 错误：`Parse`（语法/运行时异常，消息含行号）、`Timeout`（超时）、`Unsupported`（返回值不可转换）。
  [[nodiscard]] auto eval(std::string_view source, std::string_view filename = "<script>")
      -> Result<Json>;

  /// 调用脚本中已定义的函数（`eval` 里 `function f(){}` 或 `globalThis.f = ...` 之后）。
  [[nodiscard]] auto call(std::string_view function, const std::vector<Json>& arguments)
      -> Result<Json>;

  /// 注册宿主函数（脚本里以 `name(...)` 调用）。返回 `Invalid` 表示名字非法或已被占用
  /// （不覆盖既有绑定：静默覆盖会让"谁改了这个函数"变得无法追查）。
  auto register_function(std::string name, ScriptHostFunction function) -> Status;

  /// 注册二进制宿主函数：脚本以 `Float32Array` 传参，宿主直接读内存。
  ///
  /// ```js
  /// const positions = new Float32Array(n * 3);
  /// addGeometry(positions);            // 单个数组
  /// addGeometry(positions, normals);   // 多个平行数组
  /// ```
  ///
  /// ## 类型不符时明确报错（不隐式转换）
  ///
  /// 传普通数组会立即报错并指出是第几个参数、期望什么类型。
  /// **不隐式转换**——那会悄悄退回慢路径，于是"我明明用了二进制通道怎么还是慢"
  /// 变成一个查不出的问题。
  auto register_binary_function(std::string name, ScriptBinaryHostFunction function) -> Status;

  /// 设置/读取脚本全局值（走 JSON 转换）。
  auto set_global(std::string_view name, const Json& value) -> Status;
  [[nodiscard]] auto global(std::string_view name) -> Result<Json>;

  /// 校验脚本语法而不执行（用于配置加载时提前报错）。
  /// 错误：`Parse`（语法错误，消息含行号）。
  [[nodiscard]] auto check_syntax(std::string_view source) -> Status;

  /// 泵一轮 Promise 微任务（`JS_ExecutePendingJob`），返回执行的 job 数。
  ///
  /// 为什么需要它：异步回调（`Promise.then`）不会自己跑——宿主必须显式驱动。
  /// 声明式层的 `useResource` 与 AI 写的异步逻辑都依赖这条泵；
  /// 宿主在主循环每帧调用一次即可（无挂起 job 时零开销）。
  auto pump_jobs() -> std::size_t;

  /// 上次执行的统计。
  [[nodiscard]] auto last_stats() const noexcept -> const ScriptStats& { return stats_; }

  /// 生效的配额。
  [[nodiscard]] auto limits() const noexcept -> const ScriptLimits& { return limits_; }

  /// 是否真的可用了（引擎构造失败时为否——例如平台不支持的极端情况）。
  [[nodiscard]] auto valid() const noexcept -> bool;

  /// 引擎版本（如 `"0.17.0"`）。
  [[nodiscard]] static auto version() -> std::string_view;

 private:
  std::unique_ptr<detail::ScriptState> impl_;
  ScriptLimits limits_{};
  ScriptStats stats_{};
};

/// 便捷入口：新建引擎执行一次脚本（一次性场景；多次执行请持有实例复用运行时）。
[[nodiscard]] auto run_script(std::string_view source, const ScriptLimits& limits = {})
    -> Result<Json>;

}  // namespace st::ext
