/// 性能分析体系的**度量正确性**护栏（`include/st/perf/*`）。
///
/// ## 它守的四件事
///
/// 这套体系的产物是"给模型/自动化读的诊断报告"。报告里任何一处读数是错的，
/// 都会导致**照着改错地方**——而比"没有诊断"更贵。所以这里把四个底座量规
/// 钉成断言（都是纯计算/纯记录，不需要真跑界面，因此可以进默认测试路径）：
///
/// | 分组 | 守什么 |
/// |---|---|
/// | 直方图 | 分位不偏乐观（桶上界 ≥ 真值）、样本量可信度、合并可加、溢出如实记录 |
/// | 阶段计时 | 嵌套时 parent.self 不含 child（"未归因"不能凭空出现）、同名跨次累计 |
/// | 帧剖析 | 稳态/冷启动分离（**首帧 200ms 不能把 p95 抬起来**）、逐帧环正确绕回 |
/// | 启动分解 | 同名步骤合并、里程碑顺序、总时长 = 末步累计 |
///
/// ## 逆向验证（这条最重要）
///
/// 每条断言都必须在"实现被改坏"时变红。已实测回退过的关键项：
/// ① 直方图返回桶**下界**而不是上界 → `quantile_is_not_optimistic` 红；
/// ② 冷启动帧混进分位 → `cold_frames_do_not_pollute_percentiles` 红；
/// ③ 父阶段的 self 不再减 child → `nested_scope_self_excludes_children` 红。
///
/// 不做这一步就会交付"恒绿的测试"——它看起来像护栏，实际什么都拦不住。

#include "st/test/test.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "st/perf/clock.hpp"
#include "st/perf/frame.hpp"
#include "st/perf/histogram.hpp"
#include "st/perf/report.hpp"
#include "st/perf/scope.hpp"
#include "st/perf/startup.hpp"

namespace {

using st::perf::FrameProfiler;
using st::perf::Histogram;
using st::perf::Recorder;

/// 剖析开关的用例内管理：**开启且不恢复**。
///
/// 为何不恢复：`previous_` 在用例开始时可能就是 `false`（若前一个用例关掉了），
/// 恢复成 `false` 会让本用例的 `record_*` 全变成空操作——而**测试会“通过”**
/// （空数据比断言失败更阴，这正是本仓反复出现的"绿而无效"）。
/// 直接保留“开”更安全：生产里剖析由 `ST_PERF` 控制，测试进程里开着无妨。
class ScopedPerf {
 public:
  ScopedPerf() { st::perf::set_enabled(true); }
  ~ScopedPerf() { st::perf::set_enabled(true); }
  ScopedPerf(const ScopedPerf&) = delete;
  auto operator=(const ScopedPerf&) -> ScopedPerf& = delete;
};

/// 让一段代码真的花掉至少 `target_ns`（忙等：`sleep` 的粒度太粗，测不出差值）。
void burn_ns(std::int64_t target_ns) {
  const std::int64_t start = st::perf::now_ns();
  while (st::perf::now_ns() - start < target_ns) {
    // 故意空转（不让编译器优化掉：看 `now_ns` 的返回值）。
  }
}

}  // namespace

// ——————————————————————————————————————————————————————————————
// 直方图
// ——————————————————————————————————————————————————————————————

ST_TEST(perf_histogram_quantile_is_not_optimistic) {
  // **不许比实测值更乐观**：读端拿这个数判断"超没超预算"，报小就是假绿。
  // 定桶直方图返回**桶上界**，所以 `quantile >= 真值` 必须恒成立。
  Histogram histogram{};
  for (int value = 1; value <= 1000; ++value) {
    histogram.add(static_cast<std::int64_t>(value) * 1000);  // 1µs..1ms
  }
  ST_CHECK_EQ(histogram.count, 1000U);
  const std::int64_t p50 = histogram.quantile_ns(0.5);
  const std::int64_t p95 = histogram.quantile_ns(0.95);
  // 真值：第 500 个样本 = 500µs；第 950 个 = 950µs。
  ST_CHECK(p50 >= 500'000);
  ST_CHECK(p95 >= 950'000);
  // 上界不至于离谱：误差上界 9.05%，故不应超过真值的 1.2 倍。
  ST_CHECK(static_cast<double>(p50) <= 500'000.0 * 1.2);
  ST_CHECK(static_cast<double>(p95) <= 950'000.0 * 1.2);
  ST_CHECK(histogram.reliable());
  // 误差上界如实上报（不到 10%）。
  ST_CHECK(histogram.max_relative_error() < 0.10);
}

ST_TEST(perf_histogram_small_sample_is_flagged_unreliable) {
  // 「零红没报分辨力」的同型：3 个样本算出的 p95 不是结论。
  // 报告必须能区分"测过了"与"测得太少"——`reliable()` 就是那个开关。
  Histogram histogram{};
  histogram.add(1'000'000);
  histogram.add(2'000'000);
  histogram.add(3'000'000);
  ST_CHECK_EQ(histogram.count, 3U);
  ST_CHECK(!histogram.reliable());  // < 30
  for (int index = 0; index < 30; ++index) histogram.add(1'000'000);
  ST_CHECK(histogram.reliable());
}

ST_TEST(perf_histogram_merge_is_additive) {
  // 多线程各记一份、末了汇总 —— 合并必须与"从头记一遍"等价，
  // 否则并发场景的读数会系统性偏小（只看到主线程那一份）。
  Histogram left{};
  Histogram right{};
  Histogram combined{};
  for (int index = 0; index < 50; ++index) {
    left.add(1'000'000);
    right.add(3'000'000);
    combined.add(1'000'000);
    combined.add(3'000'000);
  }
  Histogram merged = left;
  merged.merge(right);
  ST_CHECK_EQ(merged.count, combined.count);
  ST_CHECK_EQ(merged.quantile_ns(0.5), combined.quantile_ns(0.5));
  ST_CHECK_NEAR(merged.total_ms(), combined.total_ms(), 0.001);
  ST_CHECK_EQ(merged.min_ns, combined.min_ns);
  ST_CHECK_EQ(merged.max_ns, combined.max_ns);
}

ST_TEST(perf_histogram_overflow_is_reported_not_dropped) {
  // 超出上界的样本要**如实记录**、不能静默丢弃：否则"有一次 5 分钟的超时"
  // 会从报告里彻底消失（`count` 少了、`max` 也少了）。
  Histogram histogram{};
  histogram.add(1'000'000);
  histogram.add(st::perf::kHistogramCeilingNs * 4);  // 远超上界
  ST_CHECK_EQ(histogram.overflow, 1U);
  ST_CHECK_EQ(histogram.count, 1U);
  // 溢出样本仍参与 max/sum —— 它确实发生过。
  ST_CHECK(histogram.max_ns > st::perf::kHistogramCeilingNs);
  // 自述里要出现"溢出"字样（`describe()` 是给报告文本用的）。
  ST_CHECK(histogram.describe().find("溢出") != std::string::npos);
}

// ——————————————————————————————————————————————————————————————
// 阶段计时（嵌套）
// ——————————————————————————————————————————————————————————————

ST_TEST(perf_nested_scope_self_excludes_children) {
  // **嵌套阶段的 parent.self 必须不含 child**：否则"父阶段 10ms"里 9ms 是子阶段，
  // 读者按 parent.self 去优化会找错地方（本仓的"未归因 0.42ms"就是这个困惑）。
  ScopedPerf perf_guard{};
  Recorder& recorder = st::perf::recorder();
  recorder.reset();
  {
    st::perf::Scope outer{recorder, "outer"};
    burn_ns(1'000'000);
    {
      st::perf::Scope inner{recorder, "inner"};
      burn_ns(4'000'000);
    }
    burn_ns(1'000'000);
  }
  const auto phases = recorder.phases();
  ST_REQUIRE(phases.size() == 2U);
  const st::perf::PhaseStat* outer = nullptr;
  const st::perf::PhaseStat* inner = nullptr;
  for (const auto& phase : phases) {
    if (phase.name == "outer") outer = &phase;
    if (phase.name == "inner") inner = &phase;
  }
  ST_REQUIRE(outer != nullptr);
  ST_REQUIRE(inner != nullptr);
  // outer 的 total 含 inner，self 不含——**这是结构性质，与机器快慢无关**。
  // 断言用加和关系（self + child = total）而不是绝对 ms：
  // 机器忙时忙等循环会被刮让，而“self 把子节点算进去了”是个确定的算法错误
  //（实测：用 `self < 3.5ms` 这种绝对阈值在并行跑测试时会假红）。
  ST_CHECK(outer->total.mean_ms() > inner->total.mean_ms());
  ST_CHECK(outer->self.mean_ms() < outer->total.mean_ms());
  // 关键：outer.self + inner.self 应当≈ outer.total（差出的是“记录外的零碎”：
  // 两次 burn 之间的几条指令 + RAII 开销）。容差按比例给（20%）。
  const double sum = outer->self.mean_ms() + inner->self.mean_ms();
  ST_CHECK(sum <= outer->total.mean_ms() * 1.2);
  ST_CHECK(sum >= outer->total.mean_ms() * 0.8);
  ST_CHECK_EQ(recorder.anomalies(), 0U);
}

ST_TEST(perf_repeated_phase_name_accumulates) {
  // 同名阶段多次进入要**累计到一行**（`sites()` 才是逐调用点）。
  ScopedPerf perf_guard{};
  Recorder& recorder = st::perf::recorder();
  recorder.reset();
  for (int index = 0; index < 5; ++index) {
    st::perf::Scope scope{recorder, "tick"};
    burn_ns(200'000);
  }
  const auto phases = recorder.phases();
  ST_REQUIRE(phases.size() == 1U);
  ST_CHECK_EQ(phases[0].calls, 5U);
  ST_CHECK_EQ(phases[0].total.count, 5U);
  // 5 次 × 0.2ms ≈ 1ms（累加进 `total_ms`，不是平均值）。
  ST_CHECK(phases[0].total.total_ms() >= 0.8);
}


// ——————————————————————————————————————————————————————————————
// 帧剖析
// ——————————————————————————————————————————————————————————————

ST_TEST(perf_cold_frames_do_not_pollute_percentiles) {
  // **首帧 200ms 不能把 p95 抬到 19ms**：这是实测踩到过的假警报来源
  // （报告说"超出 60FPS 预算"，实际稳态 3.9ms）。冷启动帧必须被单独记账。
  ScopedPerf perf_guard{};
  FrameProfiler profiler{200};
  // 两帧冷启动（各 200ms）
  for (int index = 0; index < 2; ++index) {
    profiler.begin_frame(static_cast<double>(index) * 200.0);
    profiler.record_phase("paint", 200.0);
    profiler.end_frame(100, false, true);
  }
  // 40 帧稳态（各 4ms）
  for (int index = 0; index < 40; ++index) {
    profiler.begin_frame(400.0 + static_cast<double>(index) * 4.0);
    profiler.record_phase("paint", 4.0);
    profiler.end_frame(100, false, true);
  }
  st::perf::MeasurementContext context{};
  context.app = "test";
  context.profile = "release";  // 否则会加一条 profile 告诫
  const st::perf::Report report = profiler.report(context);
  const auto summary = st::perf::summarize_json(st::perf::to_json(report));
  double p95 = 0.0;
  double steady = 0.0;
  for (const auto& [name, value] : summary.metrics) {
    if (name == "frame_p95_ms") p95 = value;
    if (name == "steady_frames") steady = value;
  }
  // 稳态样本 = 40（两帧冷启动被排除）
  ST_CHECK_EQ(static_cast<int>(steady), 40);
  // **关键**：p95 是 4ms 量级，不是 200ms。
  ST_CHECK(p95 > 3.0);
  ST_CHECK(p95 < 6.0);
  // 冷启动必须被单独报出来（不能被"排除"就等于"消失"）。
  bool has_cold_caveat = false;
  bool cold_finding = false;
  for (const auto& caveat : report.caveats) {
    if (caveat.kind == "cold_start") has_cold_caveat = true;
  }
  for (const auto& finding : report.findings) {
    if (finding.kind == "cold_start") cold_finding = true;
  }
  ST_CHECK(has_cold_caveat);
  ST_CHECK(cold_finding);
}

ST_TEST(perf_frame_ring_wraps_without_mixing_frames) {
  // 环形缓冲绕回后，读端必须拿到**最后 N 帧**而不是"新帧 + 旧帧混在一起"。
  // 实现踩过的坑：预填零 + 用 `push_back` 增长 → 读到一堆 0ms。
  ScopedPerf perf_guard{};
  FrameProfiler profiler{10};
  for (int index = 0; index < 25; ++index) {
    profiler.begin_frame(static_cast<double>(index) * 5.0);
    profiler.record_phase("paint", static_cast<double>(index));  // 0..24 ms
    profiler.end_frame(1, false, true);
  }
  const auto& frames = profiler.frames();
  ST_CHECK_EQ(frames.size(), 10U);
  // 绕回后保留的是**最后 10 帧**（index 15..24）。
  std::uint64_t smallest = 1'000'000;
  std::uint64_t largest = 0;
  for (const auto& sample : frames) {
    smallest = std::min(smallest, sample.index);
    largest = std::max(largest, sample.index);
  }
  ST_CHECK_EQ(smallest, 15U);
  ST_CHECK_EQ(largest, 24U);
  ST_CHECK(profiler.truncated());
  // CSV 同样必须只含这 10 行（+ 表头）。
  const std::string csv = profiler.frames_csv();
  std::size_t lines = 0;
  for (const char character : csv) {
    if (character == '\n') ++lines;
  }
  ST_CHECK_EQ(lines, 11U);
}

ST_TEST(perf_frame_report_distinguishes_pacing_from_slowness) {
  // 「帧内小、墙钟大」= 被限帧（不是缺陷）；「两个都大」才是真的重。
  // 这条断言守的是报告的**判据**：它必须能用 `pacing` finding 说出这件事，
  // 否则读者会把 16ms 的墙钟间隔当成"渲染慢"。
  ScopedPerf perf_guard{};
  FrameProfiler profiler{100};
  // 帧内 2ms，但墙钟间隔 16.6ms（典型被帧预算限帧）
  for (int index = 0; index < 40; ++index) {
    const double now = static_cast<double>(index) * 16.6;
    profiler.begin_frame(now);
    profiler.record_phase("paint", 2.0);
    profiler.end_frame(50, false, true);
  }
  st::perf::MeasurementContext context{};
  context.profile = "release";
  const st::perf::Report report = profiler.report(context);
  bool paced = false;
  for (const auto& finding : report.findings) {
    if (finding.kind == "pacing" && finding.title.find("限帧") != std::string::npos) paced = true;
    // 反向护栏：不该同时报"超出预算"（帧内只有 2ms）。
    if (finding.kind == "frame_budget") ST_CHECK(finding.severity != "critical");
  }
  ST_CHECK(paced);
}

// ——————————————————————————————————————————————————————————————
// 启动分解
// ——————————————————————————————————————————————————————————————

ST_TEST(perf_startup_merges_same_name_steps) {
  // 「字体加载」会发生 6 次（6 个 face）：报告里要合成一行（6 次的总时间），
  // 而不是六行 0.3ms —— 读者要看的是"字体一共花了多少"。
  st::perf::set_startup_enabled(true);
  st::perf::startup_reset();
  st::perf::startup_mark("start");
  for (int index = 0; index < 6; ++index) {
    burn_ns(200'000);
    st::perf::startup_mark("font");
  }
  burn_ns(200'000);
  st::perf::startup_mark("first_frame");
  ST_CHECK(st::perf::startup_has_data());

  st::perf::MeasurementContext context{};
  const st::perf::Report report = st::perf::startup_report(context);
  ST_CHECK_EQ(report.kind, std::string{"startup"});
  const auto summary = st::perf::summarize_json(st::perf::to_json(report));
  double total = 0.0;
  double dominant = 0.0;
  for (const auto& [name, value] : summary.metrics) {
    if (name == "startup_total_ms") total = value;
    if (name == "startup_dominant_ms") dominant = value;
  }
  // 总时长 ≈ 7 × 0.2ms = 1.4ms（留调度余量）
  ST_CHECK(total > 1.0);
  // 最贵的一步是合并后的 "font"（6 × 0.2 = 1.2ms），不是单次 0.2ms。
  ST_CHECK(dominant > 0.9);
  // 报告里要能指出"最贵的一步是哪个"。
  bool named = false;
  for (const auto& finding : report.findings) {
    if (finding.kind == "startup_dominant" && finding.where == "font") named = true;
  }
  ST_CHECK(named);
  st::perf::set_startup_enabled(false);
  st::perf::startup_reset();
}

ST_TEST(perf_report_schema_is_stable_and_parseable) {
  // 报告是**协议**：读者（`st perf --compare`、控制通道、模型）按 `schema` 判字段语义。
  // 这条用例守两件事：① schema 字段在；② 输出的 JSON 能被自己的摘要器读回来。
  ScopedPerf perf_guard{};
  FrameProfiler profiler{50};
  for (int index = 0; index < 35; ++index) {
    profiler.begin_frame(static_cast<double>(index) * 5.0);
    profiler.record_phase("paint", 3.0);
    profiler.end_frame(10, false, true);
  }
  st::perf::MeasurementContext context{};
  context.app = "unit";
  context.profile = "release";
  const st::perf::Report report = profiler.report(context);
  const std::string json = st::perf::to_json(report);
  ST_CHECK(json.find("\"schema\": \"st.perf.report/1\"") != std::string::npos);
  const auto summary = st::perf::summarize_json(json);
  ST_CHECK_EQ(summary.schema, std::string{"st.perf.report/1"});
  ST_CHECK_EQ(summary.kind, std::string{"frame"});
  ST_CHECK_EQ(summary.context.app, std::string{"unit"});
  ST_CHECK(!summary.metrics.empty());
}

ST_TEST(perf_disabled_records_nothing) {
  // 关闭时**零记录**：这条是"剖析能常驻在生产代码里"的前提。
  // 若关闭时仍有记录，那么生产环境的帧耗时会被量尺本身污染，
  // 而用户看到的报告里不会有任何提示（最坏的形态：数据看起来正常）。
  st::perf::set_enabled(false);
  Recorder& recorder = st::perf::recorder();
  recorder.reset();
  {
    st::perf::Scope scope{recorder, "should_not_record"};
    burn_ns(100'000);
  }
  ST_CHECK(recorder.phases().empty());
  ST_CHECK(recorder.empty());

  FrameProfiler profiler{10};
  profiler.begin_frame(0.0);
  profiler.record_phase("paint", 5.0);
  profiler.end_frame(10, false, true);
  ST_CHECK_EQ(profiler.frame_count(), 0U);
  ST_CHECK(profiler.frames().empty() || profiler.frames().size() == 10U);
  // 帧样本不应被写入（`write_` 停在 0）
  for (const auto& sample : profiler.frames()) ST_CHECK(!sample.presentable || sample.total_ms == 0.0);
}
