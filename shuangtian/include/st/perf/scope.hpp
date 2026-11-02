#pragma once

/// @file scope.hpp
/// 阶段计时（嵌套安全）与**调用点桶**（回答"同一原语里哪一处调用贵"）。
///
/// ## 两个问题，两套记录
///
/// 只知道"shadow 合计 0.61 ms"不够用：一个界面里阴影可能有几十处调用，
/// 改哪一处取决于**哪一处最贵**。所以每次计时同时记两份：
/// 1. **按名字**（`"paint.shadow"`）——回答"这个环节一共多贵"；
/// 2. **按调用点**（`src/ui/components/card.cpp:812`）——回答"最贵的那一处在哪"。
///
/// 调用点由 `std::source_location::current()` 在**调用点**捕获（它是默认实参，
/// 展开发生在调用方，因此拿到的是埋点那一行）——不需要任何宏。
///
/// ## 嵌套
///
/// 阶段是嵌套的（`frame` ⊃ `paint` ⊃ `paint.shadow`）。朴素的"进入记开始、
/// 离开记差值"会让父阶段**包含**子阶段，于是各项相加远超总量（那正是
/// "0.42 ms 未归因"这类困惑的来源）。这里用**线程内阶段栈**：
/// - `self` = 从上次进入/子阶段离开到这次离开，**只算自己**；
/// - `total` = 含子阶段；
/// - 报数时两者都给，读者不必自己减。
///
/// ## 关闭时零成本
///
/// `Scope::Scope` 的第一件事是 `if (!enabled()) return;`——关掉时不读时钟、
/// 不动任何缓冲。运行期开关（`set_enabled`）因此也能立刻生效。
///
/// ## 用法
///
/// ```cpp
/// {
///   st::perf::Scope scope{"paint.shadow"};   // 作用域结束自动记账
///   ...
/// }
/// ```
///
/// **不要**把 `Scope` 存进成员或返回出去：它靠析构记账，生命周期必须是作用域。

#include <cstdint>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/perf/clock.hpp"
#include "st/perf/histogram.hpp"

namespace st::perf {

/// 一个**具名阶段**的累计统计（按名字聚合，跨所有调用点）。
struct PhaseStat {
  std::string name{};
  Histogram self{};   ///< 不含子阶段
  Histogram total{};  ///< 含子阶段
  std::uint64_t calls{0};
};

/// 一个**调用点**的累计统计（按 `file:line` 聚合）。
struct SiteStat {
  std::string site{};  ///< `src/ui/components/card.cpp:812`（相对仓库根的最短后缀）
  std::string phase{}; ///< 所属阶段名
  Histogram self{};
  Histogram total{};
  std::uint64_t calls{0};
};

/// 剖析会话：持有所有阶段与调用点。
///
/// **不是进程全局**：由创建者持有（应用一个、测试一个），便于隔离与并发测试。
/// 记录缓冲按线程分开，`phases()`/`sites()` 在快照时汇总——热路径不加锁。
class Recorder {
 public:
  Recorder();
  Recorder(const Recorder&) = delete;
  auto operator=(const Recorder&) -> Recorder& = delete;
  ~Recorder();

  /// 开始一段阶段（成对调用 `end()`；用 `Scope` 更省事）。
  /// `site` 为 `nullptr` 时不建调用点桶（只按名字聚合）。
  auto begin(std::string_view name, const std::source_location* site = nullptr) -> void;
  /// 同上，但直接收 `source_location` 的值（`Scope` 走这条）。
  auto begin_with_site(std::string_view name, const std::source_location& site) -> void {
    begin(name, &site);
  }
  /// 结束最近开始的阶段。
  auto end() -> void;

  /// 清空全部样本（保留配置）。
  void reset();

  /// 是否记录过任何样本。
  [[nodiscard]] auto empty() const -> bool;

  /// 按名字聚合的阶段（按 total 均值降序）。
  [[nodiscard]] auto phases() const -> std::vector<PhaseStat>;
  /// 按调用点聚合（按 total 均值降序）。
  [[nodiscard]] auto sites() const -> std::vector<SiteStat>;
  /// 顶层阶段（最外层）的总耗时直方图——"一次完整迭代多贵"。
  [[nodiscard]] auto iteration_total() const -> Histogram;
  /// 埋点异常数（栈溢出 / 未配对 `end`；>0 = 埋点写错了，报告必须报出来）。
  [[nodiscard]] auto anomalies() const -> std::uint64_t;

  /// 私有实现（线程缓冲表）。**定义在 .cpp**：此类型不参与内联。
  struct Impl;

 private:
  /// 独占持有实现（`Impl` 在此是不完整类型，故用 `unique_ptr` + 在 .cpp 析构；
  /// 拷贝已删，唯一所有权由它表达）。
  std::unique_ptr<Impl> impl_{};
};

/// 进程默认记录器（应用/测试各一个时用 `Recorder` 自建；需要"随手埋点"时用这个）。
///
/// 它**关掉时零成本**：`Scope` 在 `enabled()` 为假时根本不取它。
[[nodiscard]] auto recorder() noexcept -> Recorder&;

/// 作用域计时守卫：构造时 `begin`，析构时 `end`。
///
/// 用类而不是宏：本仓无异常边界，RAII 是唯一能保证"提前 return 也收尾"的形态
/// （`return`/`break`/`continue` 全覆盖）。本仓另禁函数式宏（用 `std::source_location`
/// 做默认实参就是为它准备的）。
class Scope {
 public:
  /// `site` 的默认实参在**调用点**求值（`source_location::current()` 的语义），
  /// 因此记下的永远是埋点那一行——不需要任何宏来注入它。
  explicit Scope(Recorder& recorder, std::string_view name,
                 const std::source_location site = std::source_location::current())
      : recorder_(&recorder) {
    if (!enabled()) {
      recorder_ = nullptr;
      return;
    }
    // `begin` 内部立刻把 `file_name()/line()` 抽成值，不保存这里的指针。
    recorder_->begin_with_site(name, site);
  }
  /// 记进**进程默认记录器**（`recorder()`）——最常见的用法。
  explicit Scope(std::string_view name,
                 const std::source_location site = std::source_location::current())
      : recorder_(&::st::perf::recorder()) {
    if (!enabled()) {
      recorder_ = nullptr;
      return;
    }
    recorder_->begin_with_site(name, site);
  }
  Scope(const Scope&) = delete;
  auto operator=(const Scope&) -> Scope& = delete;
  ~Scope() {
    if (recorder_ != nullptr) recorder_->end();
  }

 private:
  Recorder* recorder_{nullptr};
};

/// 进程默认记录器（应用/测试各一个时用 `Recorder` 自建；需要"随手埋点"时用这个）。
///
/// 它**关掉时零成本**：`Scope` 在 `enabled()` 为假时根本不取它。
[[nodiscard]] auto recorder() noexcept -> Recorder&;

/// 把 `__FILE__` 形态的路径缩短为"相对已知根的最短后缀"。
///
/// 为什么必须缩短：MinGW 上 `__FILE__` 是**绝对路径**，直接进报告会把整条构建路径
/// 泄漏出去，且同一文件的几十个调用点前缀完全相同、真正有信息量的只有尾巴。
/// 规则：去掉到 `src/`、`include/`、`tests/`、`tools/`、`examples/` 之一为止的前缀；
/// 找不到这些锚点时只保留最后两段。
[[nodiscard]] auto shorten_site(std::string_view site) -> std::string;

/// 把 `source_location` 转成展示用的 `file:line`。
[[nodiscard]] auto site_of(const std::source_location& location) -> std::string;

}  // namespace st::perf
