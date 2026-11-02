/// 冷启动分解的实现（见 `include/st/perf/startup.hpp`）。
///
/// ## 状态放在哪里
///
/// 启动这**只有一条时间线**（进程级），且埋点来自不同模块（字体/后端/UI/应用），
/// 传一个 Context 进去不现实。所以用一份**受锁保护的进程级记录**：
/// - 不是 `thread_local`：启动阶段可能有后台线程（字体预热、资源加载）参与；
/// - 锁的代价可接受：启动期只有几十次 mark，不在热路径上。
///
/// 这条是本模块与 `scope.cpp`（帧路径、无锁）的**刻意差别**——两处的约束不同。

#include "st/perf/startup.hpp"

#include <algorithm>
#include <format>
#include <mutex>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/perf/clock.hpp"

namespace st::perf {
namespace {

/// 一步启动开销。
struct Step {
  std::string name{};
  std::int64_t delta_ns{0};     ///< 相对上一步
  std::int64_t cumulative_ns{0};///< 相对进程入口
  std::uint64_t count{0};       ///< 同名合并次数
};

struct StartupState {
  bool enabled{false};
  std::int64_t origin_ns{0};
  std::int64_t last_ns{0};
  std::vector<Step> steps{};
  mutable std::mutex mutex{};
};

[[nodiscard]] auto state() -> StartupState& {
  // lint-allow: L8 启动时间线是**进程级单条**（见文件头“状态放在哪里”），受锁保护
  static StartupState shared{};
  return shared;
}

[[nodiscard]] auto env_flag() noexcept -> bool {
  // lint-allow: L8 环境快照（进程生命周期内不变）
  static const bool snapshot = [] {
    const auto value = fs::read_env("ST_PERF_STARTUP");
    if (!value.has_value() || value->empty()) return false;
    return *value != "0" && *value != "off";
  }();
  return snapshot;
}

}  // namespace

auto startup_enabled() noexcept -> bool {
  StartupState& shared = state();
  return shared.enabled || env_flag();
}

void set_startup_enabled(bool value) noexcept {
  StartupState& shared = state();
  const std::scoped_lock lock(shared.mutex);
  shared.enabled = value;
}

auto startup_origin_ns() noexcept -> std::int64_t {
  StartupState& shared = state();
  const std::scoped_lock lock(shared.mutex);
  return shared.origin_ns;
}

void startup_mark(std::string_view name) {
  if (!startup_enabled()) return;
  StartupState& shared = state();
  const std::int64_t now = perf::now_ns();
  const std::scoped_lock lock(shared.mutex);
  if (shared.origin_ns == 0) {
    // 第一个 mark 建立零点：名字就是它，耗时 0（“从进程开始到这里”本身不可测，
    // 因为入口之前还有 CRT/动态链接——那一段归操作系统，不归应用）。
    shared.origin_ns = now;
    shared.last_ns = now;
    Step step{};
    step.name = std::string(name);
    step.delta_ns = 0;
    step.cumulative_ns = 0;
    step.count = 1;
    shared.steps.push_back(std::move(step));
    return;
  }
  const std::int64_t delta = now > shared.last_ns ? now - shared.last_ns : 0;
  shared.last_ns = now;
  const std::int64_t cumulative = now - shared.origin_ns;
  for (auto& step : shared.steps) {
    if (step.name == name) {
      step.delta_ns += delta;
      step.cumulative_ns = cumulative;
      ++step.count;
      return;
    }
  }
  Step step{};
  step.name = std::string(name);
  step.delta_ns = delta;
  step.cumulative_ns = cumulative;
  step.count = 1;
  shared.steps.push_back(std::move(step));
}

auto startup_has_data() noexcept -> bool {
  StartupState& shared = state();
  const std::scoped_lock lock(shared.mutex);
  return shared.steps.size() >= 2;
}

auto startup_report(const MeasurementContext& context) -> Report {
  Report report{};
  report.kind = "startup";
  report.context = context;

  std::vector<Step> steps{};
  std::int64_t total_ns = 0;
  {
    StartupState& shared = state();
    const std::scoped_lock lock(shared.mutex);
    steps = shared.steps;
    total_ns = shared.steps.empty() ? 0 : shared.steps.back().cumulative_ns;
  }
  if (steps.size() < 2) {
    Caveat caveat{};
    caveat.kind = "startup";
    caveat.ok = false;
    caveat.detail = "没有启动里程碑（需要在关键位置调用 st::perf::startup_mark）";
    caveat.fix = "设 ST_PERF_STARTUP=1 并在启动路径上埋点";
    report.caveats.push_back(std::move(caveat));
    return report;
  }

  const double total_ms = static_cast<double>(total_ns) / 1'000'000.0;
  // 找最贵的一步（排除第一个"起点"里程碑，它的 delta 恒 0）。
  const Step* dominant = nullptr;
  for (const Step& step : steps) {
    if (dominant == nullptr || step.delta_ns > dominant->delta_ns) dominant = &step;
  }

  {
    Finding finding{};
    finding.kind = "startup_total";
    // 判据：1 秒是人类能明显感到"启动慢"的门槛；300ms 是"勉强可接受"。
    finding.severity = total_ms >= 1000.0 ? "critical" : (total_ms >= 300.0 ? "warn" : "info");
    finding.title = std::format("冷启动总耗时 {:.1f}ms（{} 个里程碑）", total_ms, steps.size());
    std::string list{};
    for (const Step& step : steps) {
      if (step.delta_ns == 0) continue;
      if (!list.empty()) list += " → ";
      list += std::format("{} {:.1f}ms", step.name, static_cast<double>(step.delta_ns) / 1e6);
    }
    finding.evidence = list.empty() ? "（各步骤均未超过 0.1ms）" : list;
    finding.action = total_ms >= 300.0 ? "看下面最贵的那一步：它是不是可以延后/移到后台" : "";
    report.findings.push_back(std::move(finding));
  }
  if (dominant != nullptr && dominant->delta_ns > 0) {
    const double share = total_ms > 0.0 ? static_cast<double>(dominant->delta_ns) / 1e6 / total_ms : 0.0;
    Finding finding{};
    finding.kind = "startup_dominant";
    finding.severity = share >= 0.5 ? "warn" : "info";
    finding.title = std::format("最贵的启动步骤：{} {:.1f}ms（占 {:.0f}%）", dominant->name,
                                static_cast<double>(dominant->delta_ns) / 1e6, share * 100.0);
    finding.evidence = std::format("累计到该步 {:.1f}ms（同名步骤共 {} 次）", 
                                  static_cast<double>(dominant->cumulative_ns) / 1e6,
                                  dominant->count);
    finding.where = dominant->name;
    finding.action = "看这一步内部：能否延后（首帧之后）/缓存/并行；延后是首选（首屏不受影响）";
    report.findings.push_back(std::move(finding));
  }

  // 证据层：逐步明细。`steps` 数组直接可读；`metrics` 里放可比较的标量。
  Json sections = Json::object();
  Json metrics = Json::object();
  metrics["startup_total_ms"] = total_ms;
  metrics["startup_steps"] = static_cast<std::int64_t>(steps.size());
  if (dominant != nullptr) {
    metrics["startup_dominant_ms"] = static_cast<double>(dominant->delta_ns) / 1e6;
  }
  sections["metrics"] = std::move(metrics);
  Json step_list = Json::array();
  for (const Step& step : steps) {
    Json item = Json::object();
    item["name"] = step.name;
    item["delta_ms"] = static_cast<double>(step.delta_ns) / 1e6;
    item["cumulative_ms"] = static_cast<double>(step.cumulative_ns) / 1e6;
    item["count"] = step.count;
    step_list.push_back(std::move(item));
  }
  sections["steps"] = std::move(step_list);
  report.sections_json = json_dump(sections, false);
  return report;
}

void log_startup_summary() {
  if (!startup_has_data()) return;
  std::vector<Step> steps{};
  std::int64_t total_ns = 0;
  {
    StartupState& shared = state();
    const std::scoped_lock lock(shared.mutex);
    steps = shared.steps;
    total_ns = shared.steps.back().cumulative_ns;
  }
  std::ranges::sort(steps, [](const Step& a, const Step& b) { return a.delta_ns > b.delta_ns; });
  ST_LOG_INFO("冷启动分解：总 {:.1f}ms（最贵前 5 步）",
              static_cast<double>(total_ns) / 1'000'000.0);
  for (std::size_t index = 0; index < std::min<std::size_t>(5, steps.size()); ++index) {
    if (steps[index].delta_ns == 0) continue;
    ST_LOG_INFO("  {:<24} {:>8.1f}ms  （累计 {:.1f}ms{}）", steps[index].name,
                static_cast<double>(steps[index].delta_ns) / 1'000'000.0,
                static_cast<double>(steps[index].cumulative_ns) / 1'000'000.0,
                steps[index].count > 1 ? std::format("，{} 次", steps[index].count) : "");
  }
}

void startup_reset() {
  StartupState& shared = state();
  const std::scoped_lock lock(shared.mutex);
  shared.origin_ns = 0;
  shared.last_ns = 0;
  shared.steps.clear();
}

}  // namespace st::perf
