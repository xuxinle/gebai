/// 帧剖析的实现（见 `include/st/perf/frame.hpp`）。
///
/// 本文件只依赖 `perf` 底座 + 标准库：报告里的动态部分（findings 的文字推导）
/// 也在这里，因为它需要"知道各段的量级关系"，那正是剖析器自己的知识。

#include "st/perf/frame.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <limits>
#include <unordered_map>

#include "st/core/json.hpp"
#include "st/perf/clock.hpp"
#include "st/perf/scope.hpp"

namespace st::perf {
namespace {

/// 把阶段耗时写进当前帧的三段（`layout`/`paint`/`present`），供逐帧样本用。
/// 自定义阶段名不影响（它们只进直方图）。
void set_frame_phase(std::string_view name, double ms, double& layout, double& paint,
                     double& present) {
  if (name == "layout") layout = ms;
  else if (name == "paint") paint = ms;
  else if (name == "present") present = ms;
}

/// 预算判据：60 FPS（16.67 ms）。**这是产品口径**，不是"越省越好"的口径——
/// 报告里超过它就报 `critical`，接近（>80%）报 `warn`。
inline constexpr double kFrameBudgetMs = 16.67;
inline constexpr double kWarnRatio = 0.8;

/// 各段的主导判据：某段占比超过这个值就说"它主导了这一帧"。
inline constexpr double kDominantShare = 0.6;

[[nodiscard]] auto percentile(const std::vector<double>& sorted, double ratio) -> double {
  if (sorted.empty()) return 0.0;
  const auto index = static_cast<std::size_t>(std::min(
      static_cast<double>(sorted.size() - 1), ratio * static_cast<double>(sorted.size() - 1)));
  return sorted[index];
}

[[nodiscard]] auto sorted_copy(const std::vector<double>& values) -> std::vector<double> {
  std::vector<double> out = values;
  std::ranges::sort(out);
  return out;
}

/// 直方图 → JSON（分位、误差上界、样本量可信度一起给，**不留裸数字**）。
[[nodiscard]] auto json_of_histogram(const Histogram& histogram) -> Json {
  Json out = Json::object();
  out["n"] = histogram.count;
  out["mean_ms"] = histogram.mean_ms();
  out["p50_ms"] = histogram.quantile_ms(0.5);
  out["p90_ms"] = histogram.quantile_ms(0.9);
  out["p95_ms"] = histogram.quantile_ms(0.95);
  out["p99_ms"] = histogram.quantile_ms(0.99);
  out["min_ms"] = histogram.min_ms();
  out["max_ms"] = histogram.max_ms();
  out["total_ms"] = histogram.total_ms();
  out["relative_error"] = histogram.max_relative_error();
  out["reliable"] = histogram.reliable();
  return out;
}

}  // namespace

namespace {

/// 当前生效的帧剖析器（`nullptr` = 未剖析）。
///
/// 用 `atomic` 而不是普通指针：控制通道从服务线程开始/停止采样，而读取方是
/// 帧线程（`Element::paint`）——两边必须无数据竞争地看到同一事实。
[[nodiscard]] auto active_slot() noexcept -> std::atomic<FrameProfiler*>& {
  // lint-allow: L8 进程级剖析开关（跨线程可见，用 atomic 承载；语义就是"一个开关"）
  static std::atomic<FrameProfiler*> slot{nullptr};
  return slot;
}

}  // namespace

auto active_frame_profiler() noexcept -> FrameProfiler* {
  return active_slot().load(std::memory_order_relaxed);
}

void set_active_frame_profiler(FrameProfiler* profiler) noexcept {
  active_slot().store(profiler, std::memory_order_relaxed);
}

FrameProfiler::FrameProfiler(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity), frames_(capacity == 0 ? 1 : capacity) {}

void FrameProfiler::begin_frame(double now_ms) {
  if (!enabled()) return;
  in_frame_ = true;
  // 墙钟帧间隔 = 相邻两次**进入**本函数的时刻之差（与 `Metrics::wall_p50_ms` 同口径）。
  // 为何在 entry 记而不是 exit：被限帧时帧尾到下一帧头那段睡眠才是主要项。
  if (last_begin_ms_ > 0.0 && now_ms > last_begin_ms_) {
    frame_wall_ms_ = now_ms - last_begin_ms_;
  } else {
    frame_wall_ms_ = 0.0;
  }
  last_begin_ms_ = now_ms;
  frame_layout_ms_ = 0.0;
  frame_paint_ms_ = 0.0;
  frame_present_ms_ = 0.0;
}

void FrameProfiler::end_frame(std::uint64_t elements, bool partial, bool presentable) {
  if (!enabled() || !in_frame_) return;
  in_frame_ = false;
  const double total = frame_layout_ms_ + frame_paint_ms_ + frame_present_ms_;

  FrameSample sample{};
  sample.index = completed_;
  sample.total_ms = total;
  sample.layout_ms = frame_layout_ms_;
  sample.paint_ms = frame_paint_ms_;
  sample.present_ms = frame_present_ms_;
  sample.wall_ms = frame_wall_ms_;
  sample.elements = elements;
  sample.partial = partial;
  sample.presentable = presentable;
  sample.cold = completed_ < kColdFrameCount;

  // 元素级成本的分母：只数**稳态且真画了东西**的帧。
  // 空帧与冷启动帧里根元素照样会记一笔，用全部帧做分母会把每帧成本系统性压低。
  // （元素样本本身已在 `record_element` 里滤掉冷启动帧，这里的计数只用于类聚合。）
  if (!sample.cold && presentable) ++painted_frames_;

  // 环形写：槽位预填，`write_` 指向下一个待写槽；绕回一圈后置 `wrapped_`。
  frames_[write_] = sample;
  write_ = (write_ + 1) % frames_.size();
  if (write_ == 0) {
    wrapped_ = true;
    truncated_ = true;
  }
  ++completed_;
}

void FrameProfiler::record_phase(std::string_view name, double ms) {
  if (!enabled()) return;
  const bool cold = completed_ < kColdFrameCount;
  const auto ns = static_cast<std::int64_t>(ms * 1'000'000.0);
  // 阶段表很小（十来个名字），线性查找比哈希更快也无分配。
  for (auto& phase : phases_) {
    if (phase.name == name) {
      if (cold) {
        phase.cold_ns.add(ns);
      } else {
        phase.ns.add(ns);
        ++phase.count;
      }
      set_frame_phase(name, ms, frame_layout_ms_, frame_paint_ms_, frame_present_ms_);
      return;
    }
  }
  FramePhase phase{};
  phase.name = std::string(name);
  if (cold) {
    phase.cold_ns.add(ns);
  } else {
    phase.ns.add(ns);
    phase.count = 1;
  }
  phases_.push_back(std::move(phase));
  set_frame_phase(name, ms, frame_layout_ms_, frame_paint_ms_, frame_present_ms_);
}

void FrameProfiler::record_element(std::string_view type, std::string_view id, std::int64_t self_ns,
                                   std::int64_t total_ns) {
  if (!enabled() || total_ns <= 0) return;
  // 冷启动帧不进元素样本：首帧含字体首次光栅化与图元缓存建表（实测 203ms），
  // 一个 `Card` 在那帧里就能吃几十毫秒——混进来会让"热点"变成已知的假象。
  if (completed_ < kColdFrameCount) return;
  for (auto& accum : element_accum_) {
    if (accum.id == id && accum.type == type) {
      accum.self_ns += self_ns;
      accum.total_ns += total_ns;
      ++accum.frames;
      return;
    }
  }
  if (element_accum_.size() >= kMaxElementEntries) return;  // 保护：异常界面不撑爆内存
  Accum accum{};
  accum.type = std::string(type);
  accum.id = std::string(id);
  accum.self_ns = self_ns;
  accum.total_ns = total_ns;
  accum.frames = 1;
  element_accum_.push_back(std::move(accum));
}

void FrameProfiler::begin_element_batch() {
  if (!enabled()) return;
  batch_open_ = true;
  batch_ns_ = 0;
}

void FrameProfiler::end_element_batch() {
  if (!enabled()) return;
  batch_open_ = false;
  (void)batch_ns_;
}

auto FrameProfiler::phases() const -> std::vector<FramePhase> { return phases_; }

auto FrameProfiler::elements() const -> std::vector<ElementClassCost> {
  // 按类型聚合**每帧自持成本**（不含子孙）：这才是"这类组件的实现自己花了多少"，
  // 只有它才指向可改的地方。
  //
  // 口径推导：某类型在第 t 帧的贡献 = Σ_{该类型被画过的元素} 自持耗时。
  // 累计值 `self_ns` 是跨帧总和，除以**它出现的帧数**得到"每出现一次花多少"，
  // 再除以**稳态帧数**还原成"按帧平均的贡献"。两步都不能省：
  //   - 只除稳态帧数 → 只在少数帧出现的元素看起来白拿时间（被低估）；
  //   - 只除自身帧数 → 多个元素的合计会超过帧总时长（实测出现过"占帧 137%"）。
  const auto steady_frames = static_cast<std::int64_t>(std::max<std::uint64_t>(1, painted_frames_));
  std::vector<ElementClassCost> result{};
  for (const auto& accum : element_accum_) {
    const auto own_frames = static_cast<std::int64_t>(std::max<std::uint64_t>(1, accum.frames));
    const std::int64_t per_frame = accum.self_ns / own_frames;
    auto found = std::ranges::find_if(result, [&](const ElementClassCost& cost) {
      return cost.type == accum.type;
    });
    if (found == result.end()) {
      ElementClassCost cost{};
      cost.type = accum.type;
      cost.elements = 1;
      cost.ns.add(per_frame);
      result.push_back(std::move(cost));
    } else {
      found->elements += 1;
      found->ns.add(per_frame);
    }
  }
  std::ranges::sort(result, [](const ElementClassCost& a, const ElementClassCost& b) {
    return a.ns.mean_ms() > b.ns.mean_ms();
  });
  (void)steady_frames;
  return result;
}

auto FrameProfiler::hot_elements(std::size_t limit) const -> std::vector<ElementCost> {
  std::vector<ElementCost> result{};
  result.reserve(element_accum_.size());
  for (const auto& accum : element_accum_) {
    ElementCost cost{};
    cost.id = accum.id;
    cost.type = accum.type;
    // **用该元素自己出现的帧数**做分母："它一被画就要多少"。
    // 用全局帧数会把每帧成本切碎（只在大促页出现的卡片会看起来"很便宜"）。
    const auto own_frames = static_cast<std::int64_t>(std::max<std::uint64_t>(1, accum.frames));
    cost.ns.add(accum.self_ns / own_frames);
    cost.total_ns = accum.total_ns / own_frames;
    result.push_back(std::move(cost));
  }
  std::ranges::sort(result, [](const ElementCost& a, const ElementCost& b) {
    return a.ns.mean_ms() > b.ns.mean_ms();
  });
  if (result.size() > limit) result.resize(limit);
  return result;
}

void FrameProfiler::reset() {
  frames_.clear();
  frames_.resize(capacity_);
  write_ = 0;
  wrapped_ = false;
  truncated_ = false;
  completed_ = 0;
  painted_frames_ = 0;
  phases_.clear();
  element_accum_.clear();
  in_frame_ = false;
  last_begin_ms_ = 0.0;
  frame_layout_ms_ = frame_paint_ms_ = frame_present_ms_ = 0.0;
  frame_wall_ms_ = 0.0;
}

auto FrameProfiler::frames_csv() const -> std::string {
  std::string out{
      "frame,layout_ms,paint_ms,present_ms,total_ms,wall_ms,elements,partial,presentable,cold\n"};
  const std::size_t size = wrapped_ ? frames_.size() : write_;
  const std::size_t start = wrapped_ ? write_ : 0;
  for (std::size_t index = 0; index < size; ++index) {
    const FrameSample& sample = frames_[(start + index) % frames_.size()];
    out += std::format("{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{}\n", sample.index,
                       sample.layout_ms, sample.paint_ms, sample.present_ms, sample.total_ms,
                       sample.wall_ms, sample.elements, sample.partial ? 1 : 0,
                       sample.presentable ? 1 : 0, sample.cold ? 1 : 0);
  }
  return out;
}

auto FrameProfiler::report(const MeasurementContext& context) const -> Report {
  Report report{};
  report.kind = "frame";
  report.context = context;
  const std::size_t written = wrapped_ ? frames_.size() : write_;
  report.context.frames = static_cast<std::uint32_t>(written);

  // —— 先算统计（口径自检也要用）——
  std::vector<double> totals{};
  std::vector<double> walls{};
  totals.reserve(written);
  walls.reserve(written);
  std::uint64_t empty_frames = 0;
  std::uint64_t cold_frames = 0;
  double cold_worst_ms = 0.0;
  std::vector<const FrameSample*> in_order{};
  in_order.reserve(written);
  const std::size_t start = wrapped_ ? write_ : 0;
  for (std::size_t index = 0; index < written; ++index) {
    const FrameSample& sample = frames_[(start + index) % frames_.size()];
    in_order.push_back(&sample);
    if (!sample.presentable) ++empty_frames;
    if (sample.cold) {
      // **冷启动帧不进分位**：它们含字体首次光栅化/缓存建表（实测 200ms 对 3.9ms），
      // 混进来会把 p95 抬到 19ms 并报"超出预算"——那是假警报（另有专门条目说启动慢）。
      ++cold_frames;
      cold_worst_ms = std::max(cold_worst_ms, sample.total_ms);
      continue;
    }
    totals.push_back(sample.total_ms);
    if (sample.wall_ms > 0.0) walls.push_back(sample.wall_ms);
  }
  const auto sorted_totals = sorted_copy(totals);
  const auto sorted_walls = sorted_copy(walls);
  const double p50 = percentile(sorted_totals, 0.5);
  const double p95 = percentile(sorted_totals, 0.95);
  const double worst = sorted_totals.empty() ? 0.0 : sorted_totals.back();
  const double wall_p50 = percentile(sorted_walls, 0.5);
  const double wall_p95 = percentile(sorted_walls, 0.95);

  // —— 口径自检（先于结论：数据不可信时结论没有意义）——
  {
    Caveat caveat{};
    caveat.kind = "profile";
    caveat.ok = !(context.profile == "debug");
    caveat.detail =
        std::format("构建档 = {}（{}）", context.profile.empty() ? "未知" : context.profile,
                    context.profile == "debug" ? "最慢档，耗时约为 release 的 2~4 倍"
                                               : "可用于性能比较");
    caveat.fix = caveat.ok ? "" : "用 release 档重跑：st build <target> --profile release";
    report.caveats.push_back(std::move(caveat));
  }
  {
    Caveat caveat{};
    caveat.kind = "samples";
    caveat.ok = totals.size() >= 30;
    caveat.detail = totals.empty()
                        ? std::format("没有稳态帧样本（共 {} 帧，其中冷启动 {} 帧）", written,
                                      cold_frames)
                        : std::format("稳态样本 {} 帧（共 {} 帧，已排除冷启动 {} 帧）",
                                      totals.size(), written, cold_frames);
    caveat.fix = caveat.ok ? "" : "延长采样：--perf-frames 200 或让界面持续动起来";
    report.caveats.push_back(std::move(caveat));
  }
  if (cold_frames > 0) {
    Caveat caveat{};
    caveat.kind = "cold_start";
    caveat.ok = true;
    caveat.detail =
        std::format("首 {} 帧为冷启动（最差 {:.1f}ms），已排除出分位与元素统计", cold_frames,
                    cold_worst_ms);
    caveat.fix = "";
    report.caveats.push_back(std::move(caveat));
  }
  {
    Caveat caveat{};
    caveat.kind = "clock";
    caveat.ok = high_resolution_clock();
    caveat.detail = high_resolution_clock()
                        ? std::format("元素级计时用 CPU 刻度（标定误差 ≤ {:.2f}%）",
                                      clock_uncertainty() * 100.0)
                        : "本平台无高精度刻度，元素级读数含时钟读表开销（约 20ns/次）";
    caveat.fix = caveat.ok ? "" : "元素级排序仍可用（相对关系保留），但绝对值偏高";
    report.caveats.push_back(std::move(caveat));
  }
  {
    Caveat caveat{};
    caveat.kind = "coverage";
    caveat.ok = !truncated_;
    caveat.detail = truncated_
                        ? std::format("逐帧序列已截断（只保留最后 {} 帧）", frames_.size())
                        : std::format("逐帧序列完整（{} 帧）", written);
    caveat.fix = "";
    report.caveats.push_back(std::move(caveat));
  }
  {
    Caveat caveat{};
    const std::uint64_t anomalies = ::st::perf::recorder().anomalies();
    caveat.kind = "instrumentation";
    caveat.ok = anomalies == 0;
    caveat.detail = std::format("埋点异常 {} 次（栈溢出/未配对 end）", anomalies);
    caveat.fix = caveat.ok ? "" : "检查作用域守卫是否漏了作用域（异常会使读数偏小）";
    report.caveats.push_back(std::move(caveat));
  }

  // —— findings ——
  // ① 预算。**必须区分"帧内小、墙钟大"（被限帧）与"两个都大"（真的重）**。
  if (totals.empty()) {
    // 没有稳态样本时**不能给结论**：给一个"看起来正常"的 0ms 就是把"没测"冒充"测过了"。
    Finding finding{};
    finding.kind = "frame_budget";
    finding.severity = "warn";
    finding.title = "没有稳态帧样本，无法判断帧预算";
    finding.evidence = std::format("共 {} 帧，其中冷启动 {} 帧", written, cold_frames);
    finding.action = "延长采样（--perf-frames 200）或让界面持续动起来再接报告";
    report.findings.push_back(std::move(finding));
  } else {
    Finding finding{};
    finding.kind = "frame_budget";
    finding.evidence =
        std::format("n={} p50={:.2f}ms p95={:.2f}ms max={:.2f}ms（稳态）", totals.size(), p50, p95,
                    worst);
    if (p95 >= kFrameBudgetMs) {
      finding.severity = "critical";
      finding.title =
          std::format("帧内耗时超出 60FPS 预算（p95 {:.2f}ms > {:.2f}ms）", p95, kFrameBudgetMs);
      finding.action = "先看下面的 paint/layout/present 主导项与元素热点，再决定改哪一层";
    } else if (p95 >= kFrameBudgetMs * kWarnRatio) {
      finding.severity = "warn";
      finding.title = std::format("帧内耗时已接近 60FPS 预算（p95 {:.2f}ms，余量 {:.1f}%）", p95,
                                  (1.0 - p95 / kFrameBudgetMs) * 100.0);
      finding.action = "提前处理：留出的余量已不足 20%";
    } else {
      finding.severity = "info";
      finding.title =
          std::format("帧内耗时在 60FPS 预算内（p95 {:.2f}ms / {:.2f}ms）", p95, kFrameBudgetMs);
    }
    report.findings.push_back(std::move(finding));
  }
  // ①b 冷启动：单独说（它不进分位，但"启动第一帧 200ms"是真问题）。
  if (cold_frames > 0 && cold_worst_ms >= kFrameBudgetMs) {
    Finding finding{};
    finding.kind = "cold_start";
    finding.severity = cold_worst_ms >= 100.0 ? "warn" : "info";
    finding.title = std::format("首帧（冷启动）耗时 {:.1f}ms", cold_worst_ms);
    finding.evidence =
        std::format("前 {} 帧含字体首次光栅化/图元缓存建表，已排除出分位与元素统计", cold_frames);
    finding.action = "看 ST_PERF_STARTUP=1 的启动分解；若首帧建表占大头，属预期代价";
    report.findings.push_back(std::move(finding));
  }
  // ② 墙钟 vs 帧内：把"被限帧"这件事显式说出来，否则读者会把 16.6ms 当成"渲染很慢"。
  if (wall_p50 > 0.0 && p50 > 0.0) {
    Finding finding{};
    finding.kind = "pacing";
    const bool paced = wall_p50 > p50 * 1.5 && p50 < kFrameBudgetMs;
    finding.severity = "info";
    if (paced) {
      finding.title =
          std::format("墙钟间隔 {:.2f}ms 远大于帧内耗时 {:.2f}ms：**被帧预算限帧**，不是渲染慢",
                      wall_p50, p50);
      finding.evidence =
          std::format("wall p50={:.2f}ms p95={:.2f}ms / 帧内 p50={:.2f}ms", wall_p50, wall_p95,
                      p50);
      finding.action = "这不是缺陷；要量真实上限请用 --bench（每帧强制全量重绘、不睡帧预算）";
    } else {
      finding.title =
          std::format("墙钟与帧内耗时一致（wall p50 {:.2f}ms / 帧内 p50 {:.2f}ms）", wall_p50, p50);
      finding.evidence = std::format("wall p95={:.2f}ms", wall_p95);
    }
    report.findings.push_back(std::move(finding));
  }
  // ③ 抖动：p95 与 p50 的比值。毛刺是"用户能感觉到卡"的主因，而均值看不见它。
  if (p50 > 0.01 && p95 > p50 * 2.0) {
    Finding finding{};
    finding.kind = "jitter";
    finding.severity = p95 > kFrameBudgetMs ? "critical" : "warn";
    finding.title = std::format("帧耗时抖动大（p95/p50 = {:.1f}×）", p95 / p50);
    finding.evidence = std::format("p50={:.2f}ms p95={:.2f}ms max={:.2f}ms", p50, p95, worst);
    // 找出最贵的那几帧（逐帧序列的唯一不可替代价值）。只从**稳态帧**里找：
    // 冷启动帧永远最大，列出来只会让读者去查一个既知事实。
    std::vector<const FrameSample*> sorted{};
    sorted.reserve(totals.size());
    for (const FrameSample* sample : in_order) {
      if (!sample->cold) sorted.push_back(sample);
    }
    std::ranges::sort(sorted, [](const FrameSample* a, const FrameSample* b) {
      return a->total_ms > b->total_ms;
    });
    std::string top{};
    for (std::size_t index = 0; index < std::min<std::size_t>(5, sorted.size()); ++index) {
      if (index > 0) top += " ";
      top += std::format("#{}({:.1f}ms)", sorted[index]->index, sorted[index]->total_ms);
    }
    finding.where = "最贵帧: " + top;
    finding.action = "看这些帧的 partial/presentable 标记：空帧多则是无效重绘，否则是偶发重活";
    report.findings.push_back(std::move(finding));
  }
  // ④ 空转烧帧：presentable=false 的帧说明"什么都没变还走了一遍帧路径"。
  if (!totals.empty() && empty_frames * 10 >= totals.size() * 3) {
    Finding finding{};
    finding.kind = "idle_churn";
    finding.severity = "warn";
    finding.title = std::format("空帧占比 {:.0f}%（{} / {} 稳态帧没有任何像素变化）",
                                static_cast<double>(empty_frames) * 100.0 /
                                    static_cast<double>(totals.size()),
                                empty_frames, totals.size());
    finding.evidence = "空帧仍会走 collect→layout 判脏→paint_frame 早退";
    finding.action = "查是谁在每帧请求重绘（动画续帧 / 定时唤醒 / 状态写入）";
    report.findings.push_back(std::move(finding));
  }
  // ⑤ 阶段主导项（哪一段该改）。
  {
    double layout_sum = 0.0;
    double paint_sum = 0.0;
    double present_sum = 0.0;
    std::uint64_t steady = 0;
    for (const FrameSample* sample : in_order) {
      if (sample->cold) continue;  // 冷启动帧的大头是建表，不代表稳态构成
      layout_sum += sample->layout_ms;
      paint_sum += sample->paint_ms;
      present_sum += sample->present_ms;
      ++steady;
    }
    const double sum = layout_sum + paint_sum + present_sum;
    if (sum > 0.0 && steady > 0) {
      struct Entry {
        const char* name;
        double value;
      };
      const std::array<Entry, 3> entries{{
          {"layout", layout_sum},
          {"paint", paint_sum},
          {"present", present_sum},
      }};
      const Entry* dominant = &entries[0];
      for (const Entry& entry : entries) {
        if (entry.value > dominant->value) dominant = &entry;
      }
      if (dominant->value / sum >= kDominantShare) {
        Finding finding{};
        finding.kind = std::string("dominant_") + dominant->name;
        finding.severity = "info";
        finding.title = std::format("{} 段主导（占 {:.0f}%）", dominant->name,
                                    dominant->value / sum * 100.0);
        finding.evidence =
            std::format("每帧均值 layout={:.2f} paint={:.2f} present={:.2f} ms",
                        layout_sum / static_cast<double>(steady), paint_sum / static_cast<double>(steady),
                        present_sum / static_cast<double>(steady));
        if (std::string_view{dominant->name} == "paint") {
          finding.action = "下钻元素热点（sections.hot_elements）与绘制原语（ST_PAINT_PROFILE=1）";
        } else if (std::string_view{dominant->name} == "layout") {
          finding.action = "查测量/排版：宽的父容器是否每帧重排、文本宽度缓存是否失效";
        } else {
          finding.action = "查上屏路径（metrics.present_note）与是否送整屏而非损坏区";
        }
        report.findings.push_back(std::move(finding));
      }
    }
  }
  // ⑥ 元素热点（最贵的一类 / 最贵的单个）。
  {
    const auto classes = elements();
    if (!classes.empty() && p50 > 0.0 && classes.front().ns.mean_ms() > 0.0) {
      const ElementClassCost& top = classes.front();
      const double share = top.ns.mean_ms() / p50 * 100.0;
      if (share >= 8.0) {
        Finding finding{};
        finding.kind = "element_class_hotspot";
        finding.severity = share >= 30.0 ? "warn" : "info";
        finding.title = std::format("{} 类元素每帧自持 {:.2f}ms（占帧 {:.0f}%）", top.type,
                                    top.ns.mean_ms(), share);
        finding.evidence = std::format("{} 个元素 / 每帧自持均值 {:.2f}ms（n={}）", top.elements,
                                       top.ns.mean_ms(), top.ns.count);
        finding.action = "看 sections.hot_elements 里该类型最贵的具体元素，再查其绘制实现";
        report.findings.push_back(std::move(finding));
      }
    }
    const auto hot = hot_elements(3);
    if (!hot.empty() && hot.front().ns.mean_ms() > p50 * 0.1 && p50 > 0.0) {
      Finding finding{};
      finding.kind = "element_hotspot";
      finding.severity = "info";
      finding.title = std::format("最贵的单个元素：{}（{}）每帧自持 {:.2f}ms", hot.front().id,
                                  hot.front().type, hot.front().ns.mean_ms());
      std::string list{};
      for (const auto& cost : hot) {
        if (!list.empty()) list += "; ";
        list += std::format("{} {:.2f}ms", cost.id, cost.ns.mean_ms());
      }
      finding.evidence = "top3: " + list;
      finding.where = hot.front().id;
      finding.action = "该元素的绘制里是否每帧做重活（缓存失效、重复文本测量、逐帧重建）";
      report.findings.push_back(std::move(finding));
    }
  }

  // —— 证据层（sections）——
  {
    Json sections = Json::object();
    Json metrics = Json::object();
    // 可跨运行比较的标量放这里（`--compare` 按 key 成对比较）。
    metrics["frame_p50_ms"] = p50;
    metrics["frame_p95_ms"] = p95;
    metrics["frame_max_ms"] = worst;
    metrics["wall_p50_ms"] = wall_p50;
    metrics["wall_p95_ms"] = wall_p95;
    metrics["frames"] = static_cast<std::int64_t>(written);
    metrics["steady_frames"] = static_cast<std::int64_t>(totals.size());
    sections["metrics"] = std::move(metrics);

    Json phase_list = Json::array();
    for (const FramePhase& phase : phases_) {
      Json item = json_of_histogram(phase.ns);
      item["name"] = phase.name;
      // 冷启动帧的阶段耗时单独列出（"启动那一下 200ms"是真问题，不能被剔除掉）。
      item["cold_total_ms"] = phase.cold_ns.total_ms();
      phase_list.push_back(std::move(item));
    }
    sections["phases"] = std::move(phase_list);

    Json class_list = Json::array();
    for (const ElementClassCost& cost : elements()) {
      Json item = json_of_histogram(cost.ns);
      item["type"] = cost.type;
      item["elements"] = cost.elements;
      class_list.push_back(std::move(item));
    }
    sections["elements"] = std::move(class_list);

    Json hot_list = Json::array();
    for (const ElementCost& cost : hot_elements(20)) {
      Json item = json_of_histogram(cost.ns);
      item["id"] = cost.id;
      item["type"] = cost.type;
      // 含子孙的每帧均值：与 `ns`（自持）并列，读者不必自己去减。
      item["total_ms"] = static_cast<double>(cost.total_ns) / 1'000'000.0;
      hot_list.push_back(std::move(item));
    }
    sections["hot_elements"] = std::move(hot_list);

    Json frame_list = Json::array();
    const std::size_t frame_slot_count = frames_.size();
    const std::size_t frame_start = wrapped_ ? write_ : 0;
    for (std::size_t index = 0; index < written; ++index) {
      const FrameSample& sample = frames_[(frame_start + index) % frame_slot_count];
      Json item = Json::object();
      item["i"] = sample.index;
      item["total_ms"] = sample.total_ms;
      item["layout_ms"] = sample.layout_ms;
      item["paint_ms"] = sample.paint_ms;
      item["present_ms"] = sample.present_ms;
      item["wall_ms"] = sample.wall_ms;
      item["elements"] = sample.elements;
      item["partial"] = sample.partial;
      item["presentable"] = sample.presentable;
      item["cold"] = sample.cold;
      frame_list.push_back(std::move(item));
    }
    sections["frames"] = std::move(frame_list);

    sections["frame_budget_ms"] = kFrameBudgetMs;
    sections["truncated"] = truncated_;
    report.sections_json = json_dump(sections, false);
  }
  return report;
}

}  // namespace st::perf
