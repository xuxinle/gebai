#pragma once

/// @file probe.hpp
/// 诊断探针的统一外壳：**入口签名 / 参数 / 结论 / 退出码 / 帮助文本**。
///
/// ## 为什么需要它
///
/// `tools/*.cpp` 有 86 个探针，它们各自写自己的 `main`：入口签名出现 5 种形态，
/// 54 个手写「用法」段落，61 个直接读 `argv[1]`（越界即 UB）。更要紧的是
/// **32 个探针没有非零退出码**——其中 19 个在注释里写了明确判据，却只能靠人读输出，
/// 于是「这个量规今天还合格吗」无法机器回答，也就无法进 CI。
///
/// 本外壳把这五件事收成一处，**不动测量逻辑本身**：
///
/// | 维度 | 统一后 |
/// |---|---|
/// | 入口 | `run(spec, args)` 返回结论，由 {@ref st::probe::main_of} 转成退出码 |
/// | 参数 | {@ref st::probe::options}——具名取值 + 越界安全；自动支持 `--help` / `--json` |
/// | 结论 | {@ref st::probe::verdict}：`ok` / `fail` / `skip`（含原因） |
/// | 退出码 | `ok`=0 / `fail`=1 / `skip`=77 / 参数错=2（与 `sysexits` 惯例相容） |
/// | 输出 | 文本走 `st::print`；`--json` 时由外壳追加一行结构化结论 |
///
/// ## 为什么不用宏
///
/// CONVENTIONS §2 的 R7 把函数式宏列为硬红线。所以这里不提供 `ST_PROBE_MAIN(...)`：
/// 探针末尾写的是**一个普通函数定义**
///
/// ```cpp
/// auto main(int argc, char** argv) -> int {
///   return st::probe::main_of(spec(), st::probe::options::from(argc, argv));
/// }
/// ```
///
/// 这行样板可以照抄，也可以由改造脚本生成；它没有隐藏控制流，调试器能正常断进去。
///
/// ## 判定口径：`skip` 不是 `ok`
///
/// 探针经常「条件不具备」（缺字体、非真窗口、Windows 无 X11）。把它报成 `ok` 会让
/// 「没测」冒充「测过了」——这正是本仓反复出现的假绿形态。所以三态：
/// **`ok` 测过且合格 / `fail` 测过且不合格 / `skip` 没测成（带原因）**。
/// 退出码 `77`（`EX_NOPERM` 惯例位）让 CI 能把「跳过」与「通过」分开统计。

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/print.hpp"

namespace st::probe {

/// 探针结论。**三态而非布尔**：把「没测成」当通过是本仓反复出现的假绿来源。
enum class status {
  ok,    ///< 测过，且符合判据
  fail,  ///< 测过，但不符合判据
  skip,  ///< 没测成（条件不具备）；`reason` 必须说明缺什么
};

/// 一条具名检查的结果（一个探针可报多条，便于定位是哪一条不过）。
struct check {
  std::string name{};     ///< 检查项名（如 `横向偏移`、`缓存零新增`）
  bool passed{true};
  std::string detail{};   ///< 实测值/偏差的文字说明（人读，也是 `--json` 的载荷）
};

/// 探针自述：标题、判据一句话、可选参数说明。`--help` 直接由它生成。
struct spec {
  std::string_view title{};           ///< 一句话说明这是什么量规
  std::string_view criterion{};       ///< 「合格」的判据（一句话；没有确定判据的基准探针写空）
  std::string_view usage_args{};      ///< 额外位置参数说明，如 `"[font_size]"`
  std::span<const std::string_view> flags{};  ///< 探针自己认的选项（`--foo`），供 `--help` 列出
};

/// 解析后的命令行。
///
/// **不用 `argv[i]` 裸下标**（R17：越界未检查）；位置参数与选项分开取，
/// 取值缺失返回空 `optional` 而不是 UB。
class options {
 public:
  /// 从 `main` 的 `argc/argv` 构造。**不复用 `argv` 的裸指针算术**（R4）。
  [[nodiscard]] static auto from(int argc, char** argv) -> options {
    options out;
    for (int i = 1; i < argc; ++i) {
      std::string_view a{argv[i] == nullptr ? "" : argv[i]};
      if (a.empty()) continue;
      if (a.front() == '-') {
        out.flags_.emplace_back(a);
      } else {
        out.positional_.emplace_back(a);
      }
    }
    return out;
  }

  /// 是否给了某选项。`--json` / `--help` 由外壳消费，探针一般不必自己判。
  [[nodiscard]] auto has(std::string_view flag) const -> bool {
    for (const auto& f : flags_) {
      if (f == flag) return true;
    }
    return false;
  }

  /// 第 `index` 个位置参数（0 起）；缺省返回空。
  [[nodiscard]] auto positional(std::size_t index) const -> std::optional<std::string_view> {
    if (index >= positional_.size()) return std::nullopt;
    return positional_[index];
  }

  /// 第 `index` 个位置参数按整数解析；缺失或非法返回空（**不抛**：探针不该因参数崩溃）。
  [[nodiscard]] auto positional_int(std::size_t index) const -> std::optional<int> {
    const auto text = positional(index);
    if (!text) return std::nullopt;
    int value = 0;
    bool any = false;
    for (const char c : *text) {
      if (c < '0' || c > '9') {
        if (!(c == '-' && !any)) return std::nullopt;
        continue;
      }
      value = value * 10 + (c - '0');
      any = true;
    }
    return any ? std::optional<int>{value} : std::nullopt;
  }

  [[nodiscard]] auto positional_count() const -> std::size_t { return positional_.size(); }
  [[nodiscard]] auto wants_json() const -> bool { return has("--json"); }
  [[nodiscard]] auto wants_help() const -> bool { return has("--help") || has("-h"); }

 private:
  std::vector<std::string_view> positional_{};
  std::vector<std::string_view> flags_{};
};

/// 一次探针运行的完整结果。
struct result {
  status state{status::ok};
  /// `skip` 时必填（缺什么条件）；`fail` 时建议填。
  std::string reason{};
  std::vector<check> checks{};

  /// 记一条检查。`passed=false` 不会立刻改 `state`——由 {@ref conclude} 统一裁决，
  /// 免得「中途记了一条不过、后面又改回 ok」这种自相矛盾。
  void add(std::string name, bool passed, std::string detail) {
    checks.push_back(check{std::move(name), passed, std::move(detail)});
  }

  /// 收尾裁决：任一条 `passed=false` ⇒ `fail`。已显式标 `skip` 的不覆盖
  /// （跳过是因为条件不具备，此时残存的检查结果没有意义）。
  auto conclude() -> result& {
    if (state == status::skip) return *this;
    for (const auto& c : checks) {
      if (!c.passed) {
        state = status::fail;
        if (reason.empty()) reason = c.name + "：不符合判据";
        return *this;
      }
    }
    return *this;
  }
};

/// 构造一个「跳过」结论（缺字体、非真窗口等）。**必须给原因**——
/// 没有原因的跳过与静默通过等价。
[[nodiscard]] inline auto skip(std::string why) -> result {
  result r;
  r.state = status::skip;
  r.reason = std::move(why);
  return r;
}

/// 退出码映射。`skip` 用 77（`sysexits` 的 `EX_NOPERM` 惯例位），使 CI 能把它
/// 与「通过（0）」分开统计——把跳过算作通过是本仓反复出现的假绿。
inline constexpr int exit_ok = 0;
inline constexpr int exit_fail = 1;
inline constexpr int exit_usage = 2;
inline constexpr int exit_skip = 77;

[[nodiscard]] inline auto exit_code_of(status s) -> int {
  switch (s) {
    case status::ok: return exit_ok;
    case status::fail: return exit_fail;
    case status::skip: return exit_skip;
  }
  return exit_fail;
}

[[nodiscard]] inline auto name_of(status s) -> std::string_view {
  switch (s) {
    case status::ok: return "ok";
    case status::fail: return "fail";
    case status::skip: return "skip";
  }
  return "fail";
}

/// `--json` 的单行结论（由外壳在文本输出之后追加，便于机器解析而不干扰人读）。
/// 手写序列化而不用 `st::json`：本头被探针共用，**不引入额外依赖**（§6 头文件纪律）。
[[nodiscard]] inline auto to_json(const result& r) -> std::string {
  auto escape = [](std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
      switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\n': out.append("\\n"); break;
        case '\t': out.append("\\t"); break;
        default: out.push_back(c);
      }
    }
    return out;
  };
  std::string out{"{\"verdict\":\""};
  out.append(name_of(r.state));
  out.append("\",\"reason\":\"").append(escape(r.reason)).append("\",\"checks\":[");
  bool first = true;
  for (const auto& c : r.checks) {
    if (!first) out.push_back(',');
    first = false;
    out.append("{\"name\":\"").append(escape(c.name)).append("\",\"passed\":");
    out.append(c.passed ? "true" : "false");
    out.append(",\"detail\":\"").append(escape(c.detail)).append("\"}");
  }
  out.append("]}");
  return out;
}

/// 打印 `--help`（由 `spec` 生成，取代 54 份手写用法段落）。
inline void print_help(std::string_view program, const spec& s) {
  st::print("{}\n", s.title);
  if (!s.criterion.empty()) st::print("\n判据：{}\n", s.criterion);
  st::print("\n用法：{} {} [--json] [--help]\n", program, s.usage_args);
  if (!s.flags.empty()) {
    st::print("选项：\n");
    for (const auto f : s.flags) st::print("  {}\n", f);
  }
  st::print("  通用：--json 追加一行结构化结论；--help 显示本说明\n");
  st::print("\n退出码：0 合格 / 1 不合格 / 77 条件不具备（跳过，不冒充通过）/ 2 参数错\n");
}

/// 把探针结论转成进程退出码，并在 `--json` 时追加一行结构化结论。
///
/// `spec` 只用于将来可能的自动标注；当前不参与判码，故参数保持无名的 `const spec&`
/// （命名了却不用会撞 `-Werror=unused-parameter`——本仓的严格口径）。
///
/// 探针 `main` 的样板（照抄即可，无宏、无隐藏控制流）：
/// ```cpp
/// auto main(int argc, char** argv) -> int {
///   return st::probe::main_of(spec(), st::probe::options::from(argc, argv));
/// }
/// ```
[[nodiscard]] inline auto main_of([[maybe_unused]] const spec& s, const options& opts, const result& r) -> int {
  if (r.state == status::fail || r.state == status::skip) {
    st::eprint("\n[{}] {}\n", name_of(r.state), r.reason);
  }
  if (opts.wants_json()) st::print("{}\n", to_json(r));
  return exit_code_of(r.state);
}

/// 处理 `--help` 的便捷入口：给了 `--help` 就打印说明并返回 `true`（调用方随即返回 0）。
[[nodiscard]] inline auto help_requested(std::string_view program, const spec& s, const options& opts) -> bool {
  if (!opts.wants_help()) return false;
  print_help(program, s);
  return true;
}

}  // namespace st::probe
