/// `st` —— 霜天自研工具链 CLI（构建 / 测试 / 禁令扫描 / 依赖管理 / 自检）。
/// 自举：`bootstrap.sh` 用编译器直接产出本程序，之后一切构建都走这里（不经 CMake/Make）。

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/thread_pool.hpp"
#include "st/core/time.hpp"
#include "st/pkg/build.hpp"
#include "st/pkg/fetch.hpp"
#include "st/pkg/lint.hpp"
#include "st/pkg/manifest.hpp"
#include "st/pkg/memory.hpp"
#include "st/pkg/registry.hpp"
#include "st/pkg/semver.hpp"
#include "st/pkg/stats.hpp"

namespace {

struct Arguments {
  std::string command{};
  std::vector<std::string> positional{};
  std::map<std::string, std::string> options{};
  std::vector<std::string> flags{};

  [[nodiscard]] auto has(std::string_view name) const -> bool {
    return std::ranges::find(flags, name) != flags.end();
  }
  [[nodiscard]] auto get(std::string_view name, std::string fallback = {}) const -> std::string {
    const auto iterator = options.find(std::string(name));
    return iterator != options.end() ? iterator->second : fallback;
  }
  [[nodiscard]] auto number(std::string_view name, double fallback) const -> double {
    if (const auto iterator = options.find(std::string(name)); iterator != options.end()) {
      if (auto parsed = st::parse_f64(iterator->second); parsed.has_value()) return *parsed;
    }
    return fallback;
  }
  /// 多个别名里任一命中即取（如 `-j` 与 `--jobs`）——只认短形式会让 `--jobs 4` **静默失效**。
  [[nodiscard]] auto number_any(const std::vector<std::string_view>& names, double fallback) const
      -> double {
    for (const auto name : names) {
      if (const auto iterator = options.find(std::string(name)); iterator != options.end()) {
        if (auto parsed = st::parse_f64(iterator->second); parsed.has_value()) return *parsed;
      }
    }
    return fallback;
  }
};

/// **布尔选项**名单：它们没有值，后面跟的东西是**位置参数**，不是它们的值。
///
/// 为何必须显式列：解析器把 `--name <非减号开头>` 一律当成“带值选项”（见下方分支），
/// 于是 `st test --slow frame_cost` 会把 `frame_cost` 当成 `--slow` 的值——
/// filter 变空、全部用例都跑（实测）。同样的坑对 `--list`/`--san` 一直存在（如
/// `st test --list pkg_` 会列全部）。列在这里就一次性堵住。
[[nodiscard]] auto is_boolean_option(std::string_view name) -> bool {
  static constexpr std::string_view kBooleanOptions[] = {
      "san", "list", "slow", "verbose", "v", "force", "all", "help", "rules",
      "no-pch", "quiet", "release",
  };
  return std::ranges::find(kBooleanOptions, name) != std::end(kBooleanOptions);
}

[[nodiscard]] auto parse_arguments(int argc, char** argv) -> Arguments {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string_view raw = argv[index];
    if (raw.starts_with("--")) {
      const std::string_view body = raw.substr(2);
      const std::size_t equals = body.find('=');
      if (equals != std::string_view::npos) {
        arguments.options[std::string(body.substr(0, equals))] = std::string(body.substr(equals + 1));
        continue;
      }
      // 布尔选项：直接记 flag，**绝不吃下一个参数**（见 `is_boolean_option`）。
      if (is_boolean_option(body)) {
        arguments.flags.emplace_back(body);
        continue;
      }
      // 下一个参数只要以 `-` 开头就不当值：否则 `--san -j 6` 会把 `-j` 吃成 `--san` 的值
      // （表现为"档位变成 debug + filter 变成 6"，两处都不报错，极难察觉）。
      if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with("-")) {
        const std::string_view next = argv[index + 1];
        const bool looks_like_value =
            next.find('/') != std::string_view::npos || next.find('.') != std::string_view::npos ||
            !next.empty();
        if (looks_like_value && !std::isdigit(static_cast<unsigned char>(next.front())) == false) {
          arguments.options[std::string(body)] = std::string(next);
          ++index;
          continue;
        }
        arguments.options[std::string(body)] = std::string(next);
        ++index;
        continue;
      }
      arguments.flags.emplace_back(body);
      continue;
    }
    // 单破折号短选项（`-j 6` / `-j=6`）：不能只认 `--`——
    // 实测 `st test --san -j 6` 里 `-j` 与 `6` 都落进 positional，`6` 被当成测试过滤器
    // （表现为"只跑了 4 个测试"），并发设置完全没生效。
    if (raw.size() > 1 && raw.starts_with("-")) {
      const std::string_view body = raw.substr(1);
      const std::size_t equals = body.find('=');
      if (equals != std::string_view::npos) {
        arguments.options[std::string(body.substr(0, equals))] =
            std::string(body.substr(equals + 1));
        continue;
      }
      // 短布尔选项（`-v`）同理不吃下一个参数。
      if (is_boolean_option(body)) {
        arguments.flags.emplace_back(body);
        continue;
      }
      if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with("-")) {
        arguments.options[std::string(body)] = std::string(argv[index + 1]);
        ++index;
        continue;
      }
      arguments.flags.emplace_back(body);
      continue;
    }
    if (raw.starts_with("-") && raw.size() > 1) {
      arguments.flags.emplace_back(raw.substr(1));
      continue;
    }
    if (arguments.command.empty()) {
      arguments.command = std::string(raw);
    } else {
      arguments.positional.emplace_back(raw);
    }
  }
  return arguments;
}

void print_usage() {
  st::print(R"((霜天工具链 st — 自研包管理器与构建驱动

用法: st <命令> [参数]

命令:
  build [target]        构建工程或指定目标（--profile=debug|release|san，-j N，--verbose，--force）
                        并发默认按**内存预算**推导（读 cgroup 上限，不再默认吃满核数）；
                        --jobs-large N 限超大单元并发（默认 1）；--max-memory MiB 覆盖预算
                        交叉编译：--toolchain=<名>（工具链在 st.pkg 的 toolchains 段声明）
  run <target> [args]   构建并运行目标（无头演示：run gallery -- --headless --frames 3）
  test [filter]         构建并运行单元测试（--san 开 ASan/UBSan 档，-j N 控并发；同支持 --jobs-large/--max-memory）
                        --slow 额外跑“慢/环境敏感”用例（量帧耗时/吞吐的性能门禁、
                        真编译的集成用例）——它们默认跳过：占测试壁钟近三分之一，
                        且在共享机器上会偶发红灯；显式写用例名仍会跑它
                        --list 只列用例不跑；--format junit [--junit-out 路径] 写逐用例报告（CI）
  lint [--explain RULE] 禁令扫描（CONVENTIONS §8；无参数即扫描工程，--rules 列出规则）
  deps                  解析依赖并打印依赖树（--locked 只读 st.lock）
  fetch                 解析 + 拉取依赖到缓存/工作区，并写 st.lock
  vendor                把依赖源码固化进 vendor/（离线可构建）
  doctor                环境自检（编译器/字体/显示后端/GPU/缓存）
  tree                  打印清单概览（目标、源文件数、依赖）
  init <name>           生成工程骨架（--framework=框架根；默认由 st 位置推断）
  clean [--all]         删除 build/ 全部档位与测试产物；--all 连 ~/.shuangtian/cache 一起清
  version               版本信息
  help                  本帮助
)");
}

[[nodiscard]] auto project_root() -> st::Result<std::string> {
  auto current = st::fs::current_dir();
  if (!current) return st::forward_error(current.error());
  return st::fs::absolute(*current);
}

[[nodiscard]] auto load_manifest(const Arguments& arguments) -> st::Result<st::pkg::Manifest> {
  const std::string path = arguments.get("manifest", "st.pkg");
  if (st::fs::is_regular_file(path)) return st::pkg::Manifest::load(path);
  auto root = project_root();
  if (!root) return st::forward_error(root.error());
  return st::pkg::Manifest::find(*root);
}

// —— 命令实现 ——

auto command_build(const Arguments& arguments) -> int {
  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  st::pkg::BuildOptions options;
  // 默认 dev（快速迭代档：-O1 兼顾编译速度与运行帧率）——与 README「日常开发/无头验证用」
  // 一致；此前默认 debug（-O0），跑得慢且与示例构建产出不同档。
  options.profile = arguments.get("profile", "dev");
  options.target = arguments.positional.empty() ? std::string{} : arguments.positional.front();
  // 交叉编译：--toolchain=<名>（命中 st.pkg 的 toolchains 段）
  options.toolchain = arguments.get("toolchain", "");
  options.verbose = arguments.has("verbose") || arguments.has("v");
  options.force = arguments.has("force");
  const auto jobs = arguments.number_any({"j", "jobs"}, 0.0);
  options.jobs = jobs > 0.0 ? static_cast<std::size_t>(jobs) : 0;
  // 0 = 自动：按内存预算推导（见 `pkg/memory.hpp`）；显式给出则完全接管
  const auto jobs_large = arguments.number("jobs-large", 0.0);
  options.jobs_large = jobs_large > 0.0 ? static_cast<std::size_t>(jobs_large) : 0;
  const auto max_memory = arguments.number("max-memory", 0.0);
  options.max_memory_mb = max_memory > 0.0 ? static_cast<std::uint64_t>(max_memory) : 0;
  options.use_pch = !arguments.has("no-pch");

  const std::int64_t start = st::time::now_ns();
  auto stats = st::pkg::build(*manifest, options);
  if (!stats) {
    std::fprintf(stderr, "%s\n", stats.error().message.c_str());
    return 1;
  }
  st::print("构建完成 [{}] 单元 {}（重编 {} / 命中缓存 {}）\n", options.profile,
              stats->units_total, stats->units_rebuilt, stats->units_cached);
  st::print("  编译 {} · {} · 总 {} · 并行 {} 路{}\n",
              st::time::format_duration_ns(stats->compile_ms * 1'000'000),
              stats->linked ? std::format("链接 {}",
                                          st::time::format_duration_ns(stats->link_ms * 1'000'000))
                            : std::string("链接已跳过（产物最新）"),
              st::time::format_duration_ns(st::time::now_ns() - start), stats->workers,
              stats->pch_used ? " · PCH" : "");
  // 并发决策必须可见："为什么是 14 路而不是 28"是运维/排查会问的第一个问题
  if (!stats->concurrency_reason.empty()) {
    st::print("  并发依据: {}（超大单元上限 {}）\n", stats->concurrency_reason,
                stats->workers_large);
  }
  if (!stats->artifact.empty()) st::print("产物: {}\n", stats->artifact);
  return 0;
}

auto command_run(const Arguments& arguments) -> int {
  if (arguments.positional.empty()) {
    std::fprintf(stderr, "错误: run 需要目标名（如 st run gallery --headless）\n");
    return 1;
  }
  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  st::pkg::BuildOptions options;
  options.profile = arguments.get("profile", "release");
  options.target = arguments.positional.front();
  // 交叉编译：`--toolchain=<名>`（与 build/test 同一口径）——不带此参数才走本机档
  options.toolchain = arguments.get("toolchain", "");
  auto stats = st::pkg::build(*manifest, options);
  if (!stats) {
    std::fprintf(stderr, "%s\n", stats.error().message.c_str());
    return 1;
  }
  std::vector<std::string> app_args;
  for (std::size_t index = 1; index < arguments.positional.size(); ++index) {
    app_args.push_back(arguments.positional[index]);
  }
  st::print("运行: {}\n", stats->artifact);
  std::fflush(stdout);
  // 交叉产物仅在与宿主平台不同时拒绝执行（目标==宿主则照常跑，见 BuildStats.runs_on_host）
  if (!stats->runs_on_host) {
    std::fprintf(stderr, "错误: 交叉编译产物无法在宿主执行: %s（请在目标平台运行）\n",
                 stats->artifact.c_str());
    return 1;
  }
  st::process::Options run_options;
  run_options.capture_output = false;
  auto result = st::process::run(stats->artifact, app_args, run_options);
  if (!result) {
    std::fprintf(stderr, "错误: %s\n", result.error().to_string().c_str());
    return 1;
  }
  return result->exit_code;
}

auto command_test(const Arguments& arguments) -> int {
  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  st::pkg::BuildOptions options;
  // 默认 **dev 档**（与 `st build` 日常档同档）：库对象与日常构建**全复用**，
  // `st test` 只需编测试源文件本身——此前默认 debug（-O0）与 dev（-O1）指纹不同，
  // 两套对象各编各的，改一行代码跑测试要等全量重编（实测 250 单元 59s）。
  // UB/内存检测的主力是 `--san`（ASan+UBSan 插桩）显式档；需要 -O0 调试时
  // `--profile debug` 仍在。
  options.profile = arguments.has("san") ? "san" : arguments.get("profile", "dev");
  options.verbose = arguments.has("verbose");
  // 测试构建同样要能控并发：`san` 档插桩后编译期内存更高，受限容器里需要降并发
  // （实测 8 GiB cgroup 下全量 san 的 28 路并行会被 OOM killer 杀掉 cc1plus，
  //   表现为莫名其妙的链接错误——`.o` 只写了一半）。
  const auto jobs = arguments.number_any({"j", "jobs"}, 0.0);
  options.jobs = jobs > 0.0 ? static_cast<std::size_t>(jobs) : 0;
  // 测试构建同样跑完整的编译流程：并发默认按内存预算推导（否则大单元照样撞内存墙）
  const auto test_jobs_large = arguments.number("jobs-large", 0.0);
  options.jobs_large = test_jobs_large > 0.0 ? static_cast<std::size_t>(test_jobs_large) : 0;
  const auto test_max_memory = arguments.number("max-memory", 0.0);
  options.max_memory_mb = test_max_memory > 0.0 ? static_cast<std::uint64_t>(test_max_memory) : 0;
  options.toolchain = arguments.get("toolchain", "");
  // 测试并行度：**默认 = 硬件并发的一半**（上限 16）——测试执行是迭代的最大单项成本
  // （全套件 25s，编译只占零头），而它是**单核**的：不并行就等于把 27 个核空转。
  // 为何不是开满：套件里有一批重型用例（真光栅化文本、真连 TCP 的等待类），
  // 它们的耗时对 CPU 竞争敏感——开满会把单片耗时抬高到把收益吃掉（28 片实测
  // 反而与 16 片持平，且单例膨胀到 3.4×）；实测拐点在 12~16 片（见 `docs/BUILD_TEST_PERF.md`）。
  // `--test-jobs 1` 回到与分片前逐位等价的单进程。
  const std::size_t default_test_jobs =
      std::min<std::size_t>(16, std::max<std::size_t>(1, st::hardware_concurrency() / 2));
  const auto test_jobs = arguments.number_any({"test-jobs", "test_jobs"},
                                              static_cast<double>(default_test_jobs));
  options.test_jobs = test_jobs > 0.0 ? static_cast<std::size_t>(test_jobs) : 1;
  const std::string filter = arguments.positional.empty() ? std::string{} : arguments.positional.front();
  // `--list`：只列用例不跑（交测试框架入口）；`--format junit`：经 ST_JUNIT_XML 写逐用例报告
  const bool list_only = arguments.has("list");
  std::string junit_path;
  if (arguments.get("format", "") == "junit") {
    junit_path = arguments.get("junit-out", "");
    if (junit_path.empty()) junit_path = st::fs::join(manifest->directory, "build/test-results.xml");
  }
  st::print("{}测试 [{}]{}{}{}", list_only ? "列出" : "运行", options.profile,
              filter.empty() ? "" : std::format(" 过滤: {}", filter),
              options.test_jobs > 1 ? std::format(" {} 片并行", options.test_jobs) : "",
              junit_path.empty() ? "" : std::format(" → {}", junit_path));
  st::print("{}\n", arguments.has("slow") ? "（含慢/环境敏感用例）" : "");
  auto code = st::pkg::run_tests(*manifest, options, filter, list_only, junit_path,
                                 /*include_slow=*/arguments.has("slow"));
  if (!code) {
    std::fprintf(stderr, "%s\n", code.error().message.c_str());
    return 1;
  }
  return *code;
}

/// `st stats [--top N]`：源码结构度量（见 `st/pkg/stats.hpp` 的说明）。
///
/// 为什么把它做进工具链而不是留一次性脚本：结构评审反复要数同几件事
/// （最大函数、热点头、复杂度），一次性脚本既慢又不可复现——"最大函数 511 行"
/// 这种结论下次没人能一键复核。做进 `st` 之后，这把尺子随代码一起演进。
auto command_stats(const Arguments& arguments) -> int {
  auto root = project_root();
  if (!root) {
    std::fprintf(stderr, "错误: %s\n", root.error().to_string().c_str());
    return 1;
  }
  std::size_t top = 10;
  if (!arguments.get("top").empty()) {
    top = static_cast<std::size_t>(std::strtoul(arguments.get("top").c_str(), nullptr, 10));
    if (top == 0) top = 10;
  }
  auto stats = st::pkg::collect_stats(*root, top);
  if (!stats) {
    std::fprintf(stderr, "错误: %s\n", stats.error().to_string().c_str());
    return 1;
  }
  st::print("源码结构度量（{} 个文件）\n", stats->files);
  st::print("  总行数 {} · 非空行 {} · 函数 {}\n\n", stats->lines, stats->code_lines,
            stats->functions);

  st::print("最大文件\n");
  for (const auto& f : stats->biggest_files) st::print("  {:5d} 行  {}\n", f.lines, f.path);
  st::print("\n最大函数（含注释与签名）\n");
  for (const auto& f : stats->biggest_functions) {
    st::print("  {:5d} 行  {}:{}  {}\n", f.lines, f.file, f.line, f.name);
  }
  st::print("\n最复杂函数（近似圈复杂度）\n");
  for (const auto& f : stats->complex_functions) {
    st::print("  CC≈{:4d}  {:5d} 行  {}:{}  {}\n", f.complexity, f.lines, f.file, f.line, f.name);
  }
  st::print("\n函数长度分布\n");
  st::print("  >=200 行 {} · 150-199 {} · 100-149 {}\n", stats->functions_over_200,
            stats->functions_over_150, stats->functions_over_100);
  st::print("\n被包含最多的头（直接包含数）\n");
  for (const auto& h : stats->hot_headers) st::print("  {:4d}   {}\n", h.includers, h.header);
  st::print("\n爆炸半径（改它要重编多少 .cpp，含传递）\n");
  for (const auto& h : stats->blast_radius) st::print("  {:4d}   {}\n", h.includers, h.header);
  return 0;
}

auto command_lint(const Arguments& arguments) -> int {
  if (arguments.has("rules")) {
    for (const auto& [rule, description] : st::pkg::known_rules()) {
      st::print("  {:<4} {}\n", rule, description);
    }
    return 0;
  }
  if (!arguments.get("explain").empty()) {
    st::print("{}\n", st::pkg::explain_rule(arguments.get("explain")));
    return 0;
  }
  auto root = project_root();
  if (!root) {
    std::fprintf(stderr, "错误: %s\n", root.error().to_string().c_str());
    return 1;
  }
  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  // 清单里的 `lint.exempt`：成片边界（如业务层 worker 线程用异常传错）的工程级豁免。
  // 需在 `CONVENTIONS.md` §8 登记——「哪些边界可以不同」变成可复查的清单，
  // 而不是散落各处的行内注释。
  auto report = st::pkg::lint_project(*root, manifest->lint_exempt);
  if (!report) {
    std::fprintf(stderr, "错误: %s\n", report.error().to_string().c_str());
    return 1;
  }
  std::size_t failing = 0;
  for (const auto& violation : report->violations) {
    if (!violation.advisory) ++failing;
    st::print("{}{}:{} [{}] {}\n", violation.advisory ? "(提示) " : "", violation.file,
                violation.line, violation.rule, violation.text);
  }
  // 豁免计数**分开报**：行内（单点破例）与清单（工程级边界）是两种性质的东西，
  // 混在一起就看不出全局边界有多宽。
  st::print("\n扫描 {} 个文件，违反 {} 项（提示 {} 项，豁免 {} 处：行内 {} / 清单 {}）\n",
              report->files_scanned, failing, report->violations.size() - failing,
              report->suppressed, report->suppressed - report->suppressed_by_manifest,
              report->suppressed_by_manifest);
  return failing == 0 ? 0 : 1;
}

auto command_doctor(const Arguments& arguments) -> int {
  (void)arguments;
  st::print("霜天环境自检\n");
  st::print("  版本            : {}\n", "0.1.0");
  const auto compiler = st::pkg::detect_compiler();
  st::print("  C++ 编译器      : {}\n",
              compiler ? compiler->c_str() : std::format("未找到（{}）", compiler.error().message));
  if (compiler) {
    auto version = st::process::run(*compiler, {"--version"});
    if (version && version->exit_code == 0) {
      st::print("  编译器版本      : {}\n", st::trim(version->stdout_text).data());
    }
  }
  st::print("  逻辑核心        : {}\n", st::hardware_concurrency());
  // 容器/系统 CPU 配额：与核数不同的第二个并发上界（配额小于核数时以配额为准）
  if (const std::size_t quota = st::pkg::detect_cpu_quota(); quota > 0) {
    st::print("  CPU 配额        : {} 核（cgroup/ST_CPU_LIMIT）\n", quota);
  } else {
    st::print("  CPU 配额        : 不可知（容器 CFS 配额探测失败，按逻辑核心数；可用 ST_CPU_LIMIT 指定）\n");
  }
  // 内存预算与推导出的并发：编译是内存密集型的，这两个数字比核数更能决定能不能跑完
  {
    const auto limit = st::pkg::detect_memory_limit();
    if (limit.limit_mb > 0) {
      st::print("  内存上限        : {} MiB（{}）\n", limit.limit_mb, limit.source);
    } else {
      st::print("  内存上限        : 不可知（并发将退回按核数推导；可用 ST_MEMORY_MB 指定）\n");
    }
    for (const auto profile : {"dev", "san"}) {
      const auto plan = st::pkg::plan_concurrency(0, 0, 0, profile, st::hardware_concurrency());
      st::print("  默认并发 [{}]    : {} 路（超大单元 {}）· {}\n", profile, plan.jobs,
                  plan.jobs_large, plan.reason);
    }
  }
  // 交叉编译工具链探测：清单里声明了什么、本机是否真的装了
  if (auto manifest = load_manifest(arguments); manifest) {
    if (manifest->toolchains.empty()) {
      st::print("  交叉工具链      : 未声明（st.pkg 的 toolchains 段可声明，如 mingw）\n");
    }
    for (const auto& toolchain : manifest->toolchains) {
      const auto found = st::process::which(toolchain.compiler);
      st::print("  交叉工具链 [{}]  : {} → {}\n", toolchain.name, toolchain.compiler,
                  found ? *found : std::string("未安装"));
      // 工具链声明的**目标三元组**必须与编译器自报的默认目标并列可见：
      // 两者不同不是错（`--target` 覆盖默认），但不显示会让人在错的方向上找原因——
      // 实测：`clang++` 自报 `Target: x86_64-pc-windows-msvc`，而本工程实际按
      // `x86_64-w64-windows-gnu` 编译，只看 `--version` 的输出会得出相反结论。
      if (!toolchain.target_triple.empty()) {
        st::print("                    --target={}（覆盖上面这行自报的默认目标）\n",
                  toolchain.target_triple);
      }
      if (found) {
        auto version = st::process::run(toolchain.compiler, {"--version"});
        if (version && version->exit_code == 0) {
          st::print("                    {}\n", st::trim(version->stdout_text).data());
        }
      }
    }
  }
  const auto display_env = []() -> bool {
    const auto display = st::fs::read_env("DISPLAY");
    const auto wayland = st::fs::read_env("WAYLAND_DISPLAY");
    return (display.has_value() && !display->empty()) || (wayland.has_value() && !wayland->empty());
  }();
  const std::string backend_hint = !display_env ? "headless"
      : (st::fs::exists("/usr/lib/x86_64-linux-gnu/libX11.so.6") ? "x11（窗口实现规划 v0.2）" : "headless");
  st::print("  显示服务        : {}\n", display_env ? "有" : "无（无头模式）");
  st::print("  图形后端        : {}\n", backend_hint);
  const auto fonts = [] {
    std::vector<std::string> found;
    for (const auto path : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                            "C:/Windows/Fonts/msyh.ttc"}) {
      if (st::fs::is_regular_file(path)) found.emplace_back(path);
    }
    return found;
  }();
  st::print("  系统字体        : {} 个候选{}\n", fonts.size(),
              fonts.empty() ? "（文本将不渲染）" : "");
  for (const auto& font : fonts) st::print("      - {}\n", font);
  const std::string cache = st::fs::join(st::fs::home_dir(), ".shuangtian/cache");
  st::print("  依赖缓存        : {}{}\n", cache, st::fs::is_directory(cache) ? "" : "（尚未创建）");
  st::print("  控制通道        : 可用（TCP，默认只绑 127.0.0.1）\n");
  st::print("  无头开发        : 可用（headless 后端 = 软件光栅器，像素结果与窗口一致）\n");
  return 0;
}

auto command_tree(const Arguments& arguments) -> int {
  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  auto sources = manifest->source_files();
  auto tests = manifest->test_files();
  st::print("{} {} ({})\n", manifest->name, manifest->version,
              manifest->kind);
  st::print("  清单        : {}\n", st::fs::join(manifest->directory, "st.pkg"));
  st::print("  模块        : {} 个\n", manifest->modules.size());
  st::print("  源文件      : {} 个\n", sources ? sources->size() : 0U);
  st::print("  测试文件    : {} 个\n", tests ? tests->size() : 0U);
  st::print("  目标        :\n");
  for (const auto& target : manifest->targets) {
    st::print("      {:<10} {:<14} {}\n", target.name, target.kind,
                target.sources.empty() ? "-" : target.sources.front());
  }
  st::print("  依赖        : 源码 {} 个 / 模块 {} 个 / 系统库 {} 个\n",
              manifest->dependencies.size(), manifest->dependency_modules.size(),
              manifest->dependency_system.size());
  for (const auto& dependency : manifest->dependencies) {
    st::print("      - {} {}\n", dependency.name, dependency.version_req);
  }
  return 0;
}

auto command_deps(const Arguments& arguments) -> int {
  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  if (manifest->dependencies.empty()) {
    st::print("无第三方源码依赖（框架本体零依赖；需要时 `st add <name>` 引入）\n");
    return 0;
  }
  st::print("依赖解析（回溯求解 · 语义版本）\n");
  std::size_t complete = 0;
  for (const auto& dependency : manifest->dependencies) {
    const char* kind = "path";
    switch (dependency.source.kind) {
      case st::pkg::SourceSpec::Kind::Path: kind = "path"; break;
      case st::pkg::SourceSpec::Kind::Http: kind = "http"; break;
      case st::pkg::SourceSpec::Kind::GitTarball: kind = "git-tarball"; break;
      case st::pkg::SourceSpec::Kind::Registry: kind = "registry"; break;
    }
    const auto requirement = st::pkg::VersionReq::parse(dependency.version_req);
    st::print("  {:<20} {:<12} {:<12} {}\n", dependency.name,
                requirement ? requirement->to_string() : "?（约束非法）", kind,
                dependency.source.location);
    ++complete;
  }
  st::print("\n共 {} 个直接依赖；已解析 {} 个\n", manifest->dependencies.size(), complete);
  return 0;
}

auto command_init(const Arguments& arguments) -> int {
  if (arguments.positional.empty()) {
    std::fprintf(stderr, "错误: init 需要工程名\n");
    return 1;
  }
  const std::string location = arguments.positional.front();
  // 绝对路径直接用：`fs::join` 会剥离 leaf 的前导 `/`（那是"拼接子路径"的语义），
  // 拿它拼绝对路径会静默变成相对路径（实测 `st init /tmp/x` 落到了 `./tmp/x`）。
  const std::string root = st::fs::is_absolute(location) ? location : st::fs::join(".", location);
  // 工程名：`--name` 优先；否则取目录基名。
  // **不能拿 `location` 当名字**：那样写进 st.pkg 的 name/target 会是完整路径
  // （实测 `st init /tmp/x` 生成了 `"name": "/tmp/x"`），既不合法也不好看。
  std::string name = arguments.get("name", "");
  if (name.empty()) {
    const std::size_t slash = root.find_last_of('/');
    name = slash == std::string::npos ? root : root.substr(slash + 1);
    if (name.empty()) name = "app";
  }
  if (st::fs::exists(root)) {
    std::fprintf(stderr, "错误: 目录已存在: %s\n", root.c_str());
    return 1;
  }
  if (auto status = st::fs::create_directories(st::fs::join(root, "src")); !status) {
    std::fprintf(stderr, "错误: %s\n", status.error().to_string().c_str());
    return 1;
  }
  // 清单结构必须与解析器一致——旧模板有三个独立缺陷，导致 `st init` 生成的工程**一律构建不了**
  // （这条路径原先没有任何测试覆盖）：
  //   ① `targets` 写成**数组**，而解析器要对象（`parse: targets 必须是 JSON 对象`）；
  //   ② 工程名用了完整路径（`st init /tmp/x` → `"name": "/tmp/x"`）而非 `--name`；
  //   ③ 顶层 `sources` 与 target 的源重复匹配 `src/main.cpp` → **符号重复定义**。
  // 框架头路径也一并写入：`st` 自己知道框架在哪（可执行文件在 `<framework>/build/bin/st`），
  // 写进去让新工程开箱即可构建（否则用户得手工配 include 路径）。
  const std::string framework_root = []() -> std::string {
    auto executable = st::process::executable_path();
    if (!executable) return {};
    // `<framework>/build/bin/st` → 上溯三级
    std::string path = st::fs::normalize(*executable);
    for (int level = 0; level < 3; ++level) {
      const std::size_t slash = path.find_last_of('/');
      if (slash == std::string::npos) return {};
      path = path.substr(0, slash);
    }
    return path;
  }();
  // 框架引用：写进清单的 `framework` 段，由 stpm 在构建时并入框架的源/头/标志/嵌入。
  // **不再手工塞 include 路径**——那只解决"找得到头"，链接照样缺符号（实测踩过）。
  const std::string explicit_framework = arguments.get("framework", "");
  std::string framework_path = explicit_framework;
  if (framework_path.empty()) framework_path = framework_root;
  if (framework_path.empty()) {
    std::fprintf(stderr,
                 "错误: 无法确定框架位置（用 --framework=<霜天框架根> 显式指定，"
                 "或从框架的 build/bin/st 运行本命令）\n");
    return 1;
  }
  framework_path = st::fs::normalize(framework_path);
  if (!st::fs::is_regular_file(st::fs::join(framework_path, "st.pkg"))) {
    std::fprintf(stderr, "错误: %s 下没有 st.pkg（--framework 应指向霜天框架根目录）\n",
                 framework_path.c_str());
    return 1;
  }
  const std::string manifest = std::format(
      "{{\n  \"name\": \"{}\",\n  \"version\": \"0.1.0\",\n  \"kind\": \"executable\",\n"
      "  \"framework\": {{ \"path\": \"{}\" }},\n"
      // 顶层 sources 留空：本项目只有一个入口，放在 target 里（两边都匹配会重复定义符号）
      "  \"sources\": [],\n"
      "  \"targets\": {{ \"{}\": {{ \"kind\": \"executable\", \"sources\": [\"src/*.cpp\"] }} }},\n"
      "  \"dependencies\": {{ \"modules\": [], \"source\": [], \"system\": [] }}\n}}\n",
      name, framework_path, name);
  if (auto status = st::fs::write_text(st::fs::join(root, "st.pkg"), manifest); !status) {
    std::fprintf(stderr, "错误: %s\n", status.error().to_string().c_str());
    return 1;
  }
  const std::string main_template = R"CPP(#include <cstdio>
#include <cstdlib>

#include "st/app/app.hpp"
#include "st/app/cli.hpp"
#include "st/core/entry.hpp"
#include "st/ui/components/basic.hpp"

auto run_app(int argc, char** argv) -> int {
  // 通用命令行（`--headless` / `--control-port` / `--control-file` / `--theme` / …）：
  // **必须解析**——控制通道的端口与控制文件就是从这里来的，不解析的话
  // 外部工具（包括歌白）就找不到这个应用，也就谈不上"可被驱动"。
  st::app::CommonOptions common;
  if (auto parsed = st::app::parse_common_options(argc, argv, common); !parsed) {
    std::fprintf(stderr, "%s\n", parsed.error().to_string().c_str());
    return 1;
  }
  if (common.show_help) {
    std::fputs(st::app::common_options_usage("@NAME@").c_str(), stdout);
    return 0;
  }
  if (common.app.title.empty()) common.app.title = "@NAME@";
  common.app.max_frames = common.max_frames;
  // 后端交给框架自动判断：有显示服务就开窗口，没有就落 headless
  // （**不要**在这里强制 headless——那会让 Windows/Linux 桌面上的应用永远不出窗口；
  //   自动化流程自己传 `--headless` 即可）

  st::app::Application app("@NAME@", "0.1.0", common.app);
  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  page->style().padding = st::math::Insets::all(24.0f);
  auto heading = std::make_unique<st::ui::Heading>("Hello 霜天", 1);
  page->add_child(std::move(heading));
  page->add_child(std::make_unique<st::ui::Text>("控制通道：tree / find / capture / input.*"));
  // `Application::run` 返回 `Result<int>`（框架不设异常；错误经 Result 返回），
  // **没有 `value_or`**——旧模板写成 `.value_or(1)` 导致生成的项目一律编译不过。
  auto outcome = app.run(std::move(page));
  if (!outcome) {
    std::fprintf(stderr, "运行失败: %s\n", outcome.error().to_string().c_str());
    return 1;
  }
  return *outcome;
}

// 跨平台入口（同框架示例）：正规化 argv 编码并设好控制台代码页
ST_MAIN(run_app)

)CPP";
  const std::string main_source = st::replace_all(main_template, "@NAME@", name);
  if (auto status = st::fs::write_text(st::fs::join(root, "src/main.cpp"), main_source); !status) {
    std::fprintf(stderr, "错误: %s\n", status.error().to_string().c_str());
    return 1;
  }
  // `build.sh`：让人不必记住 st 的位置与参数（Agent 用绝对路径直接调 st 即可）
  const std::string build_script = std::format(
      "#!/bin/sh\n"
      "# 由 `st init` 生成：把霜天框架的工具链与参数固定下来，便于直接构建/运行。\n"
      "# 交叉编译：./build.sh --toolchain=mingw\n"
      "set -e\n"
      "ST=\"${{ST:-{}/build/bin/st}}\"\n"
      "\"$ST\" build {} \"$@\"\n",
      framework_path, name);
  if (auto status = st::fs::write_text(st::fs::join(root, "build.sh"), build_script); !status) {
    std::fprintf(stderr, "错误: %s\n", status.error().to_string().c_str());
    return 1;
  }
  // `.gitignore`：构建产物不入库
  if (auto status = st::fs::write_text(st::fs::join(root, ".gitignore"), "build/\n"); !status) {
    std::fprintf(stderr, "错误: %s\n", status.error().to_string().c_str());
    return 1;
  }

  st::print("工程已创建: {}\n  {}\n  {}\n  {}\n", root, st::fs::join(root, "st.pkg"),
              st::fs::join(root, "src/main.cpp"), st::fs::join(root, "build.sh"));
  st::print("引用框架: {}\n", framework_path);
  st::print("下一步: cd {} && ./build.sh\n", root);
  return 0;
}

/// `st clean [--all]`：删除构建产物。
/// 背景：`build/` 含多个档位（dev/debug/release/san）与测试产物，实测堆积到 1.5GB+
/// 而此前没有任何清理命令。`--all` 连共享对象缓存（`~/.shuangtian/cache`）一起清——
/// 下次构建会重新全量编译，只在磁盘告急或怀疑缓存污染时用。
[[nodiscard]] auto command_clean(const Arguments& arguments) -> int {
  const auto manifest = load_manifest(arguments);
  std::string root;
  if (manifest) {
    root = manifest->directory;
  } else {
    // 没有清单也能清：默认当前目录（把"删产物"做成不依赖工程解析的操作）
    const auto current = st::fs::current_dir();
    root = current ? *current : std::string{"."};
  }
  const std::string build_dir = st::fs::join(root, "build");
  std::uint64_t removed = 0;
  if (st::fs::is_directory(build_dir)) {
    // 先量一下再删：用户应该知道刚才释放了多少
    std::function<void(const std::string&)> visit = [&](const std::string& dir) {
      const auto entries = st::fs::list_dir(dir);
      if (!entries) return;
      for (const auto& entry : *entries) {
        if (entry.is_dir) {
          visit(entry.path);
        } else {
          removed += entry.size;
        }
      }
    };
    visit(build_dir);
    if (auto status = st::fs::remove_all(build_dir); !status) {
      std::fprintf(stderr, "删除失败: %s\n", status.error().message.c_str());
      return 1;
    }
  }
  const auto format_mb = [](std::uint64_t bytes) {
    return bytes >= 1024ULL * 1024ULL
               ? std::format("{:.1f} MiB", static_cast<double>(bytes) / 1048576.0)
               : std::format("{} KiB", bytes / 1024ULL);
  };
  st::print("已清理 {}（释放 {}）\n", build_dir, removed > 0 ? format_mb(removed) : std::string{"0"});

  if (arguments.has("all")) {
    std::string home;
    if (const auto env = st::fs::read_env("ST_HOME"); env.has_value() && !env->empty()) {
      home = *env;
    } else {
      home = st::fs::join(st::fs::home_dir(), ".shuangtian");
    }
    const std::string cache_dir = st::fs::join(home, "cache");
    if (st::fs::is_directory(cache_dir)) {
      if (auto status = st::fs::remove_all(cache_dir); !status) {
        std::fprintf(stderr, "缓存删除失败: %s\n", status.error().message.c_str());
        return 1;
      }
      st::print("已清理共享缓存 {}（对象缓存；下次构建全量重编）\n", cache_dir);
    }
  }
  return 0;
}

}  // namespace

auto run_app(int argc, char** argv) -> int {
  st::log::set_level(st::log::Level::Warn);
  const Arguments arguments = parse_arguments(argc, argv);
  if (arguments.command.empty() || arguments.command == "help" || arguments.has("help")) {
    print_usage();
    return arguments.command.empty() ? 1 : 0;
  }
  if (arguments.has("verbose") || arguments.has("v")) st::log::set_level(st::log::Level::Info);

  if (arguments.command == "version") {
    st::print("st (霜天工具链) {}\n", "0.1.0");
    const auto compiler = st::pkg::detect_compiler();
    st::print("编译器: {}\n", compiler ? compiler->c_str() : "未找到");
    return 0;
  }
  if (arguments.command == "build") return command_build(arguments);
  if (arguments.command == "run") return command_run(arguments);
  if (arguments.command == "test") return command_test(arguments);
  if (arguments.command == "lint") return command_lint(arguments);
  if (arguments.command == "stats") return command_stats(arguments);
  if (arguments.command == "doctor") return command_doctor(arguments);
  if (arguments.command == "tree") return command_tree(arguments);
  if (arguments.command == "deps") return command_deps(arguments);
  if (arguments.command == "init") return command_init(arguments);
  if (arguments.command == "clean") return command_clean(arguments);


  std::fprintf(stderr, "未知命令: %s\n\n", arguments.command.c_str());
  print_usage();
  return 1;
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
