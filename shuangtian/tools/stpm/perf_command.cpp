/// `st perf`：**一条命令拿到可行动的诊断**。
///
/// ## 为什么是命令而不是"再写一个探针"
///
/// 本仓已有 ~40 个一次性探针（`tools/*_probe.cpp`），它们的共同问题是：
/// **用的人得先知道该用哪个、怎么编译、输出怎么读**。而性能排查的真实起点是
/// "这个应用现在慢 / 我想确认它没变慢"——那时需要的是一条命令 + 一份**结论**，
/// 不是一套工具。所以这里把"构建 → 启动（带剖析）→ 采样 → 汇总"收成一个动词。
///
/// ## 三种用法
///
/// | 用法 | 命令 | 场景 |
/// |---|---|---|
/// | 跑一次并给报告 | `st perf gallery` | "它到底慢在哪" |
/// | 与基线对比 | `st perf gallery --baseline old.json` | "这次改动有没有让它变慢" |
/// | 只看已存报告 | `st perf --compare old.json new.json` | "两份报告差多少" |
///
/// 前两种会**真构建真跑**（耗时几秒~几十秒），第三种纯读文件（毫秒级）。

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/core/net.hpp"
#include "st/core/print.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/perf/report.hpp"
#include "st/pkg/build.hpp"
#include "st/pkg/manifest.hpp"

#include "perf_request.hpp"

using st::Json;
using st::Status;
using st::ok;
using st::unexpected;
using st::ErrorCode;
using st::forward_error;
using st::json_parse;
using st::json_parse_file;
using st::json_dump;
using st::json_at;
using st::json_get_bool;
using st::json_get_i64;
using st::json_get_string;
namespace stpkg_cli {

namespace {

/// 控制通道的一次调用（单连接，超时友好）。
///
/// 这里的客户端是**最小**实现：控制协议只有"长度前缀 + JSON"两层，而
/// `st perf` 只需要一个方法。协议细节（勿省）：
/// ① 每连接首帧必须是 `hello`（带 token），否则被拒并断连；
/// ② 帧是 **4 字节大端长度 + JSON**；③ 响应按 `id` 匹配。
[[nodiscard]] auto control_call(std::uint16_t port, std::string_view token, std::string_view method,
                                const Json& params, int timeout_ms, Json& out) -> Status {
  auto stream = st::net::connect_tcp("127.0.0.1", port, timeout_ms);
  if (!stream) return forward_error(stream.error());
  stream->set_no_delay(true);
  const auto send = [&stream](const Json& message) -> Status {
    const std::string body = json_dump(message, false);
    const auto length = static_cast<std::uint32_t>(body.size());
    const std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>((length >> 24U) & 0xFFU),
        static_cast<std::uint8_t>((length >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((length >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(length & 0xFFU)};
    if (auto written = stream->write_all(header); !written) return written;
    return stream->write_text(body);
  };
  // 握手 + 目标请求**同批写出**（少一个往返）。
  Json hello = Json::object();
  hello["id"] = 0;
  hello["method"] = "hello";
  Json hello_params = Json::object();
  if (!token.empty()) hello_params["token"] = token;
  hello["params"] = std::move(hello_params);
  if (auto sent = send(hello); !sent) return sent;
  Json request = Json::object();
  request["id"] = 1;
  request["method"] = std::string(method);
  request["params"] = params;
  if (auto sent = send(request); !sent) return sent;

  // 读两帧（hello 的响应 + 目标响应）。
  // 读取形态与服务端（`server.cpp`）一致：`std::string` 缓冲、`read_some` 逐块拼——
  // 全程 `char`，不需要 `uint8_t*→string_view` 的指针转换（lint L6 禁令）。
  std::string pending{};
  std::array<std::uint8_t, 8192> chunk{};
  auto read_exact_bytes = [&stream, &pending, &chunk](std::size_t count) -> Status {
    pending.clear();
    pending.reserve(count);
    std::size_t done = 0;
    while (done < count) {
      const auto want = static_cast<std::size_t>(
          count - done < chunk.size() ? count - done : chunk.size());
      const auto got = stream->read_some(std::span<std::uint8_t>{chunk.data(), want});
      if (!got) return forward_error(got.error());
      if (*got == 0) return unexpected(ErrorCode::Io, "连接被对端关闭（进程可能已退出）");
      pending.append(chunk.data(), chunk.data() + *got);
      done += *got;
    }
    return ok();
  };
  for (int round = 0; round < 2; ++round) {
    if (auto ready = stream->wait_readable(timeout_ms); !ready.has_value() || !*ready) {
      return unexpected(ErrorCode::Timeout, "控制通道响应超时（应用可能在忙/已退出）");
    }
    std::array<std::uint8_t, 4> header{};
    if (auto read = stream->read_exact(header); !read) return read;
    const auto length = (static_cast<std::uint32_t>(header[0]) << 24U) |
                        (static_cast<std::uint32_t>(header[1]) << 16U) |
                        (static_cast<std::uint32_t>(header[2]) << 8U) |
                        static_cast<std::uint32_t>(header[3]);
    if (length == 0 || length > 64U * 1024U * 1024U) {
      return unexpected(ErrorCode::Parse, std::format("控制通道帧长度异常: {}", length));
    }
    if (auto read = read_exact_bytes(length); !read) return read;
    auto parsed = json_parse(pending);
    if (!parsed.has_value()) {
      return unexpected(ErrorCode::Parse, "控制通道返回的不是合法 JSON");
    }
    if (json_get_i64(*parsed, "id", -1) == 1) {
      out = std::move(*parsed);
      return ok();
    }
  }
  return unexpected(ErrorCode::Parse, "未收到目标响应（协议异常）");
}

/// 控制文件（`--control-file` 写出的握手信息）。
struct ControlInfo {
  std::uint16_t port{0};
  std::string token{};
  std::int64_t pid{0};
};

[[nodiscard]] auto read_control(std::string_view path) -> std::optional<ControlInfo> {
  auto parsed = json_parse_file(path);
  if (!parsed.has_value()) return std::nullopt;
  ControlInfo info{};
  info.port = static_cast<std::uint16_t>(json_get_i64(*parsed, "port", 0));
  info.token = json_get_string(*parsed, "token");
  info.pid = json_get_i64(*parsed, "pid", 0);
  if (info.port == 0) return std::nullopt;
  return info;
}

}  // namespace

/// 报告对比：**不让调用方自己算百分比**（那是模型最容易出错的一步）。
///
/// 规则：只比 `metrics` 里同名的标量；变化超过阈值才算差异，并按"变慢/变快"
/// 分列。阈值默认 5%（性能噪声在 3% 量级，5% 以下不提），可用 `--threshold` 改。
auto compare_reports(const st::perf::ReportSummary& baseline, const st::perf::ReportSummary& current,
                     double threshold) -> int {
  st::print("性能对比（基线 → 当前，change = 当前/基线 − 1）\n");
  // 口径先对齐：档位/后端/尺寸不同就**直接说不可比**，而不是给一堆百分比。
  bool comparable = true;
  if (baseline.context.profile != current.context.profile) {
    st::print("  ⚠ 构建档不同（{} → {}）：耗时不可比\n", baseline.context.profile,
              current.context.profile);
    comparable = false;
  }
  if (baseline.context.backend != current.context.backend) {
    st::print("  ⚠ 后端不同（{} → {}）：上屏成本不可比\n", baseline.context.backend,
              current.context.backend);
    comparable = false;
  }
  if (baseline.context.width != current.context.width ||
      baseline.context.height != current.context.height) {
    st::print("  ⚠ 尺寸不同（{}x{} → {}x{}）：像素量不同，不可比\n", baseline.context.width,
              baseline.context.height, current.context.width, current.context.height);
    comparable = false;
  }
  if (!comparable) {
    st::print("  → 结论：这两份报告**不能**直接比。先用相同档位/后端/尺寸各跑一次。\n");
    return 2;
  }

  std::size_t regressions = 0;
  std::size_t improvements = 0;
  // **只比“耗时类”标量**：`frames`/`steady_frames` 这种是样本量，不是性能指标；
  // 把它们列进对比只会得出“帧数变少 = 变快 96%”这种毫无意义的结果（实测踩过）。
  // 白名单而不是黑名单：新增的标量默认不进对比，需要时显式加上——
  // 这样“比了个不该比的东西”不会静默发生。
  static constexpr std::array<std::string_view, 5> kTimeMetrics{
      "frame_p50_ms", "frame_p95_ms", "frame_max_ms", "wall_p50_ms", "wall_p95_ms"};
  for (const auto& [name, base] : baseline.metrics) {
    const bool time_metric =
        std::ranges::find(kTimeMetrics, std::string_view{name}) != kTimeMetrics.end();
    if (!time_metric) continue;
    const auto found = std::ranges::find_if(current.metrics, [&](const auto& entry) {
      return entry.first == name;
    });
    if (found == current.metrics.end()) continue;
    const double now = found->second;
    if (base <= 0.0) continue;
    const double change = now / base - 1.0;
    const bool worse = change > threshold;
    const bool better = change < -threshold;
    if (worse) ++regressions;
    if (better) ++improvements;
    st::print("  {:<16} {:>9.3f} → {:>9.3f}  {}{:+.1f}%{}\n", name, base, now,
              worse ? "变慢 " : (better ? "变快 " : ""), change * 100.0,
              worse || better ? "" : "（在噪声内）");
  }
  st::print("\n结论：{} 项变慢 / {} 项变快（阈值 ±{:.1f}%）\n", regressions, improvements,
            threshold * 100.0);
  if (regressions > 0) {
    st::print("  → 变慢的项先用 `st perf <target>` 取完整报告，看 findings 指向哪一段/哪个元素。\n");
    return 1;
  }
  return 0;
}

/// `st perf` 的实现入口（声明在 `perf_request.hpp` 旁，`main.cpp` 不依赖 `st/perf`）。
///
/// 参数语义见 `PerfRequest` 的字段注释。
auto command_perf(const PerfRequest& request) -> int {
  // ① 纯对比模式：不构建、不启动——最快的反馈路径。
  const std::string compare_pair = request.compare;
  if (!compare_pair.empty()) {
    const auto parts = st::split(compare_pair, ',');
    if (parts.size() != 2) {
      st::eprint("--compare 需要两个路径（逗号分隔）：--compare old.json,new.json\n");
      return 1;
    }
    const auto baseline_text = st::fs::read_text(parts[0]);
    const auto current_text = st::fs::read_text(parts[1]);
    if (!baseline_text || !current_text) {
      st::eprint("读不到报告文件：{}\n", baseline_text ? parts[1] : parts[0]);
      return 1;
    }
    return compare_reports(st::perf::summarize_json(*baseline_text),
                           st::perf::summarize_json(*current_text),
                           request.threshold);
  }

  if (request.target.empty()) {
    st::eprint("用法: st perf <目标名> [--frames N] [--bench N] [--baseline PATH] [--json]\n");
    st::eprint("      st perf --compare 旧.json,新.json\n");
    return 1;
  }
  const std::string target = request.target;
  // 工程根：先看当前目录有没有 `st.pkg`，再往上找。
  const auto manifest = st::pkg::Manifest::find(".");
  if (!manifest) {
    st::eprint("错误: 本目录（或其祖先）里找不到 st.pkg\n");
    return 1;
  }
  const std::string root = manifest->directory;
  const std::string profile = request.profile;
  const std::uint32_t frames = request.frames;
  const std::uint32_t bench = request.bench;

  // ② 构建（增量：缓存命中时 < 1 秒）。
  st::pkg::BuildOptions build_options;
  build_options.profile = profile;
  build_options.target = target;
  auto stats = st::pkg::build(*manifest, build_options);
  if (!stats) {
    st::eprint("构建失败: {}\n", stats.error().message);
    return 1;
  }

  // ③ 启动一个**带剖析**的实例（无头 + 控制通道 + 采样帧数）。
  const std::string control_file = st::fs::join(root, std::format("tmp/perf-{}.ctl.json", target));
  (void)st::fs::create_directories(st::fs::join(root, "tmp"));
  // **先删旧控制文件**：上一轮异常退出会留下一个带旧端口/token 的文件，
  // 而下面的就绪轮询会立刻把它当成“已就绪”——然后拿旧 token 去握手，
  // 被服务端拒后报出“发送失败: winsock 10057”这种离真因很远的错误。
  (void)st::fs::remove_file(control_file);
  const std::string report_path =
      request.out.empty() ? st::fs::join(root, std::format("tmp/perf-{}.json", target)) : request.out;
  std::vector<std::string> app_args{"--headless", "--control-port", "0", "--control-file",
                                    control_file, "--perf-frames", std::to_string(frames)};
  // **默认用 `--bench`**（每帧强制全量重绘）：不传的话，静止界面从第二帧起
  // 就不脏、`paint_frame` 走空帧早退——实测只会拿到 3 个稳态样本，
  // 而报告会诚实地说“样本不足”（那份报告没用）。
  // 要量**真实交互**（含局部重绘）时传 `--bench 0` 显式关掉。
  const std::uint32_t effective_bench = request.bench_given ? bench : frames;
  if (effective_bench > 0) {
    app_args.push_back("--bench");
    app_args.push_back(std::to_string(effective_bench));
  }
  st::process::StreamHandle child;
  child.open(stats->artifact, app_args, root, true);
  if (!child.valid()) {
    st::eprint("启动失败: {}\n", std::string(child.error()));
    return 1;
  }
  // 逐步汇报（`--verbose` 才打）：这条链上每一步都可能卡（启动/就绪/采样/取样/收工），
  // 而“命令没反应”是最贵的故障形态——看不到进展就只能逐段加日志重试。
  const bool verbose = std::getenv("ST_PERF_VERBOSE") != nullptr;
  const auto trace = [verbose](std::string_view stage) {
    if (verbose) st::eprint("[st perf] {}\n", stage);
  };
  trace("应用已启动");
  // **必须把管道读空**：应用会持续写日志，管道缓冲满了它就阻塞在写、
  // 永远跑不到取报告那一步（实测：看上去像卡在“等控制通道/取报告超时”）。
  std::atomic<bool> keep_draining{true};
  std::mutex log_mutex{};
  std::string drained{};
  std::thread drainer{[&child, &keep_draining, &log_mutex, &drained] {
    std::string line{};
    // **不能用 `read_line` 的返回值作循环条件**：它会阻塞到“下一条输出或 EOF”，
    // 而我们在收尾时把 `keep_draining` 置假后 join——若此时应用已无输出但管道未关，
    // 这个线程就永远醒不来，整个 `st perf` 卡死在收工上（实测踩过）。
    // 做法：先等“可读”，再读一行；不可读就检查退出标志。
    while (keep_draining.load(std::memory_order_relaxed)) {
      if (!child.read_line(line)) break;  // EOF：子进程已关管道
      const std::scoped_lock lock(log_mutex);
      if (drained.size() < 256U * 1024U) {
        drained += line;
        drained += '\n';
      }
    }
  }};
  const auto finish_child = [&]() {
    // 先让子进程退出（关管道 → 读端看到 EOF → 排空线程自然结束），再 join。
    // 顺序反了就是死等：线程阻塞在 read，而 `keep_draining` 只能被 join 之后的人看到。
    (void)child.finish();
    if (drainer.joinable()) drainer.join();
  };

  // ④ 等控制通道就绪（最多 15 秒），然后取报告。
  std::optional<ControlInfo> info{};
  for (int attempt = 0; attempt < 150; ++attempt) {
    info = read_control(control_file);
    if (info.has_value() && info->port != 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  // 给应用的**采样时间**：`--perf-frames N` 要攒够 N 帧才算样本够——
  // 而 `st perf` 默认跑在静止界面上（无动画就不重绘），不等就是“回到 3 帧”。
  // 等待量按帧数估（60fps 下限最保守）：N/60 秒，封顶 6 秒。
  if (info.has_value()) {
    trace("控制通道就绪");
    const auto settle_ms = static_cast<int>(
        std::min(6000.0, static_cast<double>(frames) * 1000.0 / 60.0) + 500.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
    trace("采样窗口结束");
  }
  if (!info.has_value()) {
    st::eprint("应用没有在 15 秒内就绪（控制文件 {} 未出现）。应用输出：\n", control_file);
    child.terminate();
    finish_child();
    const std::scoped_lock lock(log_mutex);
    st::eprint("{}\n", drained);
    return 1;
  }

  Json params = Json::object();
  params["action"] = "snapshot";
  params["frames"] = static_cast<std::int64_t>(frames);
  trace("取报告");
  Json reply = Json::object();
  if (auto called = control_call(info->port, info->token, "perf", params, 30000, reply); !called) {
    st::eprint("取性能报告失败: {}\n", called.error().to_string());
    Json discard = Json::object();
    (void)control_call(info->port, info->token, "shutdown", Json::object(), 5000, discard);
    child.terminate();
    finish_child();
    return 1;
  }
  // ⑤ 收工：让实例退出（不留孤儿进程——下一次 `st perf` 会撞端口/文件）。
  //
  // ⚠ 用**独立的** `shutdown_reply` 接收：把收工的响应写回 `reply` 会把刚取到的
  // 报告覆盖成 `{"ok":true}`，然后报告变成 `null`、诊断输出一片空白（实测踩过，
  // 而“没报错但没数据”是最难查的形态）。
  trace("已取到报告，收工");
  Json shutdown_reply = Json::object();
  (void)control_call(info->port, info->token, "shutdown", Json::object(), 5000, shutdown_reply);
  finish_child();
  trace("子进程已回收");

  const Json& report = json_at(json_at(reply, "result"), "report").is_null()
                            ? json_at(reply, "report")
                            : json_at(json_at(reply, "result"), "report");
  const std::string report_text = json_dump(report, true);
  if (auto written = st::fs::write_text(report_path, report_text); !written) {
    st::eprint("报告写入失败（{}）: {}\n", report_path, written.error().to_string());
    return 1;
  }

  // ⑥ 输出：默认给**给人读的结论**（findings + 口径），`--json` 给完整报告。
  if (request.as_json) {
    st::print("{}\n", report_text);
  } else {
    const Json& context = json_at(report, "context");
    st::print("性能诊断：{}（{} / {}）\n", json_get_string(context, "app"),
              json_get_string(context, "backend"), json_get_string(context, "profile"));
    st::print("\n结论\n");
    const Json& findings = json_at(report, "findings");
    if (findings.empty()) st::print("  （没有发现异常项）\n");
    for (const auto& finding : findings) {
      st::print("  [{}] {}\n", json_get_string(finding, "severity"),
                json_get_string(finding, "title"));
      const std::string evidence = json_get_string(finding, "evidence");
      if (!evidence.empty()) st::print("      证据: {}\n", evidence);
      const std::string where = json_get_string(finding, "where");
      if (!where.empty()) st::print("      位置: {}\n", where);
      const std::string action = json_get_string(finding, "action");
      if (!action.empty()) st::print("      建议: {}\n", action);
    }
    st::print("\n口径\n");
    for (const auto& caveat : json_at(report, "caveats")) {
      const bool ok = json_get_bool(caveat, "ok", true);
      st::print("  [{}] {}: {}\n", ok ? "ok" : "!!", json_get_string(caveat, "kind"),
                json_get_string(caveat, "detail"));
    }
    st::print("\n完整报告: {}\n", report_path);
  }

  // ⑦ 与基线对比（给了才做）。
  const std::string baseline_path = request.baseline;
  if (!baseline_path.empty()) {
    const auto baseline_text = st::fs::read_text(baseline_path);
    if (!baseline_text) {
      st::eprint("读不到基线：{}\n", baseline_path);
      return 1;
    }
    return compare_reports(st::perf::summarize_json(*baseline_text),
                           st::perf::summarize_json(report_text),
                           request.threshold);
  }
  return 0;
}

}  // namespace stpkg_cli
