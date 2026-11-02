/// 报告序列化（JSON / 文本）与摘要解析（见 `include/st/perf/report.hpp`）。
///
/// 单独一个 TU 的理由：它需要完整 JSON 实现（`nlohmann` 约 2.5 万行），而
/// `perf` 的其余部分（直方图/时钟/作用域）刻意不依赖它——那些是热路径的底座，
/// 不该为了"写一份报告"把 2.5 万行头文件拉进每一个包含者。

#include "st/perf/report.hpp"

#include <algorithm>
#include <format>
#include <string_view>

#include "st/core/json.hpp"
#include "st/perf/histogram.hpp"

namespace st::perf {
namespace {

[[nodiscard]] auto caveat_json(const Caveat& caveat) -> Json {
  Json out = Json::object();
  out["kind"] = caveat.kind;
  out["ok"] = caveat.ok;
  out["detail"] = caveat.detail;
  out["fix"] = caveat.fix;
  return out;
}

[[nodiscard]] auto finding_json(const Finding& finding) -> Json {
  Json out = Json::object();
  out["severity"] = finding.severity;
  out["kind"] = finding.kind;
  out["title"] = finding.title;
  out["evidence"] = finding.evidence;
  out["where"] = finding.where;
  out["action"] = finding.action;
  return out;
}

[[nodiscard]] auto context_json(const MeasurementContext& context) -> Json {
  Json out = Json::object();
  out["app"] = context.app;
  out["app_version"] = context.app_version;
  out["backend"] = context.backend;
  out["renderer"] = context.renderer;
  out["headless"] = context.headless;
  out["width"] = context.width;
  out["height"] = context.height;
  out["device_scale"] = static_cast<double>(context.device_scale);
  out["profile"] = context.profile;
  out["os"] = context.os;
  out["cpu"] = context.cpu;
  out["frames"] = context.frames;
  out["wall_ms"] = context.wall_ms;
  return out;
}

/// 严重度排序（critical 在前），同级按 kind 字典序——输出稳定，便于 diff。
[[nodiscard]] auto severity_rank(std::string_view severity) -> int {
  if (severity == "critical") return 0;
  if (severity == "warn") return 1;
  return 2;
}

}  // namespace

auto to_json(const Report& report) -> std::string {
  Json root = Json::object();
  root["schema"] = report.schema;
  root["kind"] = report.kind;
  root["context"] = context_json(report.context);
  Json caveats = Json::array();
  for (const Caveat& caveat : report.caveats) caveats.push_back(caveat_json(caveat));
  root["caveats"] = std::move(caveats);
  Json findings = Json::array();
  for (const Finding& finding : report.findings) findings.push_back(finding_json(finding));
  root["findings"] = std::move(findings);
  // `sections_json` 是本模块**不解释**的内容：生产者已经给出 JSON 文本，
  // 这里原样嵌回（解析失败则退化为字符串字段，保证报告永远可读）。
  if (auto parsed = json_parse(report.sections_json); parsed.has_value()) {
    root["sections"] = std::move(*parsed);
  } else {
    root["sections"] = Json::object();
    root["sections_error"] = "生产者给出的 sections_json 不是合法 JSON";
  }
  return json_dump(root, true, 2);
}

auto to_text(const Report& report) -> std::string {
  std::string out{};
  out += std::format("== 性能诊断（{}）==\n", report.kind);
  // —— 结论 ——
  out += "\n结论\n";
  if (report.findings.empty()) {
    out += "  （没有发现异常项）\n";
  } else {
    std::vector<Finding> ordered = report.findings;
    std::ranges::stable_sort(ordered, [](const Finding& a, const Finding& b) {
      return severity_rank(a.severity) < severity_rank(b.severity);
    });
    for (const Finding& finding : ordered) {
      out += std::format("  [{}] {}\n", finding.severity, finding.title);
      if (!finding.evidence.empty()) out += std::format("      证据: {}\n", finding.evidence);
      if (!finding.where.empty()) out += std::format("      位置: {}\n", finding.where);
      if (!finding.action.empty()) out += std::format("      建议: {}\n", finding.action);
    }
  }
  // —— 口径 ——（放在结论之后、分解之前：先让读者知道这份数据可不可信）
  out += "\n口径\n";
  if (report.caveats.empty()) {
    out += "  （未声明口径）\n";
  } else {
    for (const Caveat& caveat : report.caveats) {
      out += std::format("  [{}] {}: {}\n", caveat.ok ? "ok" : "!!", caveat.kind, caveat.detail);
      if (!caveat.ok && !caveat.fix.empty()) out += std::format("      重跑: {}\n", caveat.fix);
    }
  }
  // —— 环境 ——
  out += std::format(
      "\n测量环境\n  {} {} / backend={} renderer={} headless={} / {}x{} scale={:.2f} / "
      "profile={} / {} 帧 / {} ms\n",
      report.context.app, report.context.app_version, report.context.backend,
      report.context.renderer, report.context.headless ? "是" : "否", report.context.width,
      report.context.height, static_cast<double>(report.context.device_scale),
      report.context.profile, report.context.frames, report.context.wall_ms);
  return out;
}

auto summarize_json(std::string_view text) -> ReportSummary {
  ReportSummary summary{};
  auto parsed = json_parse(text);
  if (!parsed.has_value()) return summary;
  const Json& root = *parsed;
  summary.schema = json_get_string(root, "schema");
  summary.kind = json_get_string(root, "kind");
  const Json& context = json_at(root, "context");
  summary.context.app = json_get_string(context, "app");
  summary.context.backend = json_get_string(context, "backend");
  summary.context.renderer = json_get_string(context, "renderer");
  summary.context.profile = json_get_string(context, "profile");
  summary.context.width = static_cast<int>(json_get_i64(context, "width"));
  summary.context.height = static_cast<int>(json_get_i64(context, "height"));
  summary.context.device_scale = static_cast<float>(json_get_double(context, "device_scale", 1.0));
  summary.context.frames = static_cast<std::uint32_t>(json_get_i64(context, "frames"));
  // 关键标量：从 `sections.metrics` 收（生产者按"可跨运行比较"的项放进来）。
  const Json& metrics = json_at(json_at(root, "sections"), "metrics");
  if (metrics.is_object()) {
    for (auto it = metrics.begin(); it != metrics.end(); ++it) {
      if (it.value().is_number()) summary.metrics.emplace_back(it.key(), it.value().get<double>());
    }
  }
  return summary;
}

}  // namespace st::perf
