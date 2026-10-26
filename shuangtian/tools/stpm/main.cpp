/// `st` —— 霜天自研工具链 CLI（构建 / 测试 / 禁令扫描 / 依赖管理 / 自检）。
/// 自举：`bootstrap.sh` 用编译器直接产出本程序，之后一切构建都走这里（不经 CMake/Make）。

#include <algorithm>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <optional>
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

#include "cli_log.hpp"

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

/// `st` 版本——**单一真源**（`st version` / `st doctor` 共用）。
/// 与 `st.pkg` 的 `version`、`ST_VERSION` 宏（清单 defines）同步升版：
/// 此前两处字面量各写各的，已经漂移过一轮。
constexpr const char* kStpmVersion = "0.1.0";

/// **布尔选项**名单：它们没有值，后面跟的东西是**位置参数**，不是它们的值。
///
/// 为何必须显式列：解析器把 `--name <非减号开头>` 一律当成“带值选项”（见下方分支），
/// 于是 `st test --slow frame_cost` 会把 `frame_cost` 当成 `--slow` 的值——
/// filter 变空、全部用例都跑（实测）。同样的坑对 `--list`/`--san` 一直存在（如
/// `st test --list pkg_` 会列全部）。列在这里就一次性堵住。
[[nodiscard]] auto is_boolean_option(std::string_view name) -> bool {
  static constexpr std::string_view kBooleanOptions[] = {
      "san", "list", "slow", "verbose", "v", "force", "all", "help", "rules",
      "no-pch", "no-strip", "offline", "locked", "quiet", "release",
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
        arguments.options[std::string(body)] = std::string(argv[index + 1]);
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
  build [target]        构建工程或指定目标（--profile=quick|dev|debug|release|san，默认 dev；
                        -j N，--verbose，--force）
                                                --no-strip 保留产物内调试信息（默认链接后分离到 <产物>.debug，
                        产物本体可小 ~94%；gdb 会自动找分离符号）
                        --no-tests 不刷新过期的测试二进制（默认会**只构建不运行**地
                        同步一次，避免「改了断言→st build→直跑 st_tests→验旧代码」）
                        并发默认按**内存预算**推导（读 cgroup 上限，不再默认吃满核数）；
                        --jobs-large N 限超大单元并发（默认 1）；--max-memory MiB 覆盖预算
                        交叉编译：--toolchain=<名>（工具链在 st.pkg 的 toolchains 段声明）
  run <target> [args]   构建并运行目标（无头演示：run gallery -- --headless --frames 3）
                        ⚠ 壳层源码未改的目标**跳过构建**（XML/JS 是运行期读的，改它们不用编）：
                        判据是目标第一条 sources 比产物新不新；要强制重建加 `--force`
                        XML/JS 应用的改文件即生效（热重载）见 docs/hot-reload.md
  test [filter]         构建并运行单元测试（--san 开 ASan/UBSan 档，-j N 控并发；同支持 --jobs-large/--max-memory）
                        --slow 额外跑“慢/环境敏感”用例（量帧耗时/吞吐的性能门禁、
                        真编译的集成用例）——它们默认跳过：占测试壁钟近三分之一，
                        且在共享机器上会偶发红灯；显式写用例名仍会跑它
                        --list 只列用例不跑；--format junit [--junit-out 路径] 写逐用例报告（CI）
  lint [--explain RULE] 禁令扫描（CONVENTIONS §8；无参数即扫描工程，--rules 列出规则）
  check                  **只检查不产出**（-fsyntax-only）：不写 .o/.d、不读 PCH、不链接
                        与 build 走**同一套**单元枚举与标志组装，所以"检查过了"就是"编得过"
                        主要是**跨编译器门禁**：`st check --toolchain=clang`
                        实测全量 77 单元 ~15s（gcc 全量构建 36s），且无副作用、可重复跑
                        可查：未使用私有字段/未使用 lambda 捕获/隐式变号/死代码……
                        **不查链接期问题**（ODR 违反、符号缺失、ABI 不匹配）——那些得真链接
  deps                  列出并**回溯求解**第三方源码依赖（语义版本；--locked 只读 st.lock，
                        锁不住即失败——CI 可复现构建的兜底；传递依赖读各依赖自己的 st.pkg）
  fetch                 求解 + 获取依赖到共享缓存并写 st.lock（path/已解包目录源可直接用；
                        sha256 缺失时如实计算；http/归档解包待 st::codec 接入，见 DESIGN §7.4）
  vendor                把已获取的依赖源码固化进 vendor/ + vendor.lock（随仓库分发、
                        离线可构建）；--offline 禁网络只走本地/缓存
  doctor                环境自检（编译器/字体/显示后端/GPU/缓存）
  log [--tail N] [--grep 文本] [--list] [--full] [--action X]
                        读最近一次构建/测试日志（默认尾部 40 行；路径规则 .st/logs/）
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

/// `st rebuild`：手工触发重建（与自动重建**同一份实现**）。
///
/// 与自动路径的差别只有一点：手工重建**不重跑命令**（人只是想更新工具），
/// 自动重建**必须重跑**（否则本次命令还在旧逻辑上跑，等于白重建）。
auto command_rebuild(const Arguments& arguments) -> int {
  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  const auto exe_result = st::process::executable_path();
  if (!exe_result.has_value()) return 1;
  const std::string self = st::fs::normalize(*exe_result);
  const std::string staging = self + std::format(".new-{}", st::process::current_id());

  st::pkg::BuildOptions options;
  options.profile = arguments.get("profile", "dev");   // 与 bootstrap 默认一致
  options.target = "st";
  // ## 为何**不**设 `force = true`（2026-10-26 实测）
  //
  // 原先这里写 `force = true`，理由是“产物路径变了，增量缓存也对不上”。
  // 那句话是错的：`output_override` 只影响**最终链接产物的路径**
  // （见 `build.cpp`：`options.output_override.empty() ? default_output : options.output_override`），
  // 而对象目录 `object_dir` 完全不变 —— 那些 `.o` 仍然可复用。
  //
  // 代价实测：`st rebuild` 热态（无任何源码改动）**48.2s**，而它实际只需链接。
  // 构建工具自己的构建时间应当是**秒级**：改一行 `src/pkg/*.cpp` 只该重编它一个。
  //
  // 正确性：交付物是否正确取决于 `.o` 是不是新代码，而那由 `needs_rebuild`（含内容哈希）
  // 保证，与最终产物叫什么无关。自举后**用新工具重跑本次命令**（`rebuild_self_and_reexec`）
  // 也保证“新逻辑真的生效”——若新代码没进去，重跑后行为不会变，那是可观测的。
  options.force = false;
  options.no_tests = true;     // 只重建工具本身
  options.strip_debug = !arguments.has("no-strip");
  options.verbose = arguments.has("verbose") || arguments.has("v");
  options.output_override = staging;

  st::print("重建构建工具（目标 st，档位 {}）…\n", options.profile);
  const std::int64_t start = st::time::now_ns();
  auto stats = st::pkg::build(*manifest, options);
  if (!stats) {
    std::fprintf(stderr, "%s\n", stats.error().message.c_str());
    (void)st::fs::remove_file(staging);
    return 1;
  }
  const std::string built = stats->artifact.empty() ? staging : stats->artifact;
  if (!st::fs::is_regular_file(built)) {
    std::fprintf(stderr, "重建失败：产物不存在（%s）\n", built.c_str());
    return 1;
  }
  const std::string elapsed = st::time::format_duration_ns(st::time::now_ns() - start);

  // 旧文件改名而不是删除：Windows 上删不掉正在运行的 exe，但改名可以；
  // 留着它还能在"新版起不来"时立刻换回去。
  const std::string backup = self + ".old";
  (void)st::fs::remove_file(backup);
  if (auto moved = st::fs::rename(self, backup); !moved) {
    std::fprintf(stderr,
                 "无法替换正在运行的 %s（改名失败：%s）\n"
                 "  新产物已保留：%s\n",
                 self.c_str(), moved.error().message.c_str(), built.c_str());
    return 1;
  }
  if (auto placed = st::fs::rename(built, self); !placed) {
    std::fprintf(stderr, "新产物就位失败：%s\n", placed.error().message.c_str());
    (void)st::fs::rename(backup, self);
    return 1;
  }

  st::print("已重建 → {}（{}）\n", self, elapsed);
  st::print("  旧版本备份在 {}\n", backup);
  st::print("  **当前进程仍跑旧逻辑；下次运行 st 即为新版。**\n");
  return 0;
}

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
  // 调试信息分离默认开（`--no-strip` 关）：不分离的代价是“每个产物白胖 ~70MB”
  //（实测 dev 档 st.exe 76.96MB，其中 89% 是 `.debug_*`），而分离的代价是零。
  options.strip_debug = !arguments.has("no-strip");
  // 默认会顺带刷新过期的测试二进制（只构建、不运行）；`--no-tests` 关掉它。
  // 理由见 `BuildOptions::no_tests`：拿旧测试二进制做结论是 AI 迭代链上最贵的失误。
  options.no_tests = arguments.has("no-tests");

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
  // 测试二进制被顺带重建时必须说一声——这个动作的全部价值就在于消除
  // 「以为验了新代码，其实跑的是旧的」。静默重建等于把功劳埋了。
  if (stats->tests_rebuilt) {
    st::print("  测试: 目标已过期 → 已同步重建（只构建，未运行；跑测试用 `st test`）\n");
  }

  return 0;
}

/// 产物后缀：**按宿主平台**取（本机档与宿主同平台）。
///
/// 为何不写成常量的 `.exe`：那会把"在 Linux 上干活"变成“永远找不到产物→永远重编”，
/// 即"不重编"这条优化在 Linux 上完全不生效（而 Linux 正是本框架的主战场之一）。
/// 带 `--toolchain` 时**不走**快路径（那条是交叉档，跑的可能是别的平台的产物），
/// 所以这里只需要宿主的口径。
[[nodiscard]] auto host_executable_suffix() -> std::string {
#if defined(_WIN32)
  return ".exe";
#else
  return {};
#endif
}

/// 「资源文件是运行期读的」目标：壳层源码没改就直接跑现有产物。
///
/// 返回 `nullopt` = 走正常构建路径（因为 `--force`/`--toolchain`/无清单/无目标/
/// 无产物/壳层源码更新）；返回带退出码的值 = 已经跑完。
///
/// 判据只有一条：**壳层源文件是否比产物新**（比 `st::pkg` 的增量判定更严：任何一壳层
/// 源比产物新就重建）。此外它**必须自己报告**“没重编”——那是让人以为构建过了的陷阱。
[[nodiscard]] auto run_without_build(const Arguments& arguments, const std::string& target,
                                     const std::vector<std::string>& app_args)
    -> std::optional<int> {
  if (arguments.has("force") || !arguments.get("toolchain", "").empty()) return std::nullopt;
  auto manifest = load_manifest(arguments);
  if (!manifest.has_value()) return std::nullopt;
  const st::pkg::TargetSpec* spec = manifest->find_target(target);
  if (spec == nullptr || spec->sources.empty()) return std::nullopt;
  auto shell_sources = st::fs::expand_glob(manifest->directory, spec->sources.front());
  if (!shell_sources.has_value()) return std::nullopt;
  const std::string profile = arguments.get("profile", "release");
  const std::string artifact = st::fs::join(
      manifest->directory,
      st::fs::join(std::format("build/{}/bin", profile), target + host_executable_suffix()));
  if (!st::fs::is_regular_file(artifact)) return std::nullopt;
  const auto artifact_time = st::fs::modified_ns(artifact);
  if (!artifact_time.has_value()) return std::nullopt;
  std::string newest_source{};
  std::int64_t newest_stamp = 0;
  for (const auto& source : *shell_sources) {
    const auto stamp = st::fs::modified_ns(source);
    if (stamp.has_value() && *stamp > newest_stamp) {
      newest_stamp = *stamp;
      newest_source = source;
    }
  }
  if (newest_stamp > *artifact_time) return std::nullopt;

  st::print("未重编：资源文件（XML/JS）是运行期读的——产物较壳层源码新，直接用。\n");
  st::print("  （改了 C++ 才需要构建；要强制重建加 build 或 --force）\n");
  st::print("运行: {}\n", artifact);
  std::fflush(stdout);
  st::process::Options run_options;
  run_options.capture_output = false;
  auto result = st::process::run(artifact, app_args, run_options);
  if (!result) {
    std::fprintf(stderr, "错误: %s\n", result.error().to_string().c_str());
    return 1;
  }
  return result->exit_code;
}

auto command_run(const Arguments& arguments) -> int {
  if (arguments.positional.empty()) {
    std::fprintf(stderr, "错误: run 需要目标名（如 st run gallery --headless）\n");
    return 1;
  }
  const std::string target = arguments.positional.front();
  std::vector<std::string> app_args;
  for (std::size_t index = 1; index < arguments.positional.size(); ++index) {
    app_args.push_back(arguments.positional[index]);
  }

  // ## 「XML + JS 应用」（xmlapp / xiangqi 这类）的**不重编**通路
  //
  // 这类应用的界面与逻辑都在**运行期读**的文件里（`ui/*.xml` / `ui/*.js`），
  // 改它们**不需要构建**：产物的源码边界只是 `examples/xmlapp/*.cpp` 那几百行壳层。
  // 但旧行为是无条件先 `build`（编辑一个 `gap` → 等 6~8 秒链接，而且产物被在跑的
  // 实例占用时直接报「结束实例再重编」）——把"调一个间距"变成一次编译。
  if (const auto reused = run_without_build(arguments, target, app_args);
      reused.has_value()) {
    return *reused;
  }

  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  st::pkg::BuildOptions options;
  options.profile = arguments.get("profile", "release");
  options.target = target;
  // 交叉编译：`--toolchain=<名>`（与 build/test 同一口径）——不带此参数才走本机档
  options.toolchain = arguments.get("toolchain", "");
  auto stats = st::pkg::build(*manifest, options);
  if (!stats) {
    std::fprintf(stderr, "%s\n", stats.error().message.c_str());
    return 1;
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
  // 测试二进制同样默认分离调试信息（它链入几百个单元，`.debug_*` 比 gallery 还大）
  options.strip_debug = !arguments.has("no-strip");
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
  // 测试并行度：**按机器实际资源推导**（与编译并发同一套探测：硬件核数 / CPU 配额 /
  // 内存上限），见 `plan_test_shards`。为何不能用"核数/2"：测试进程实测只占
  // 112 MB 工作集（内存很宽松）却占满一个核，所以它的上界是**核数**而非内存——
  // 与编译并发（内存敏感）不是同一个约束，不能共用一条公式。
  // **`--test-jobs 1` 回到与分片前逐位等价的单进程。
  //
  // `kTestShardsCap` 是策略上限而非资源上界：实测 18 片时并行度 11.0、19 片降到 9
  // （CPU 密集型用例超订后互相抢核），即超过某点墙钟不再改善；上限只挡这一头，
  // 真实的机器资源（少核/小配额容器）仍会先把片数压下来。
  constexpr std::size_t kTestShardsCap = 16;
  const st::pkg::ShardPlan shard_plan = st::pkg::plan_test_shards(0, st::hardware_concurrency(),
                                                                  0, 0, kTestShardsCap);
  // ## 带过滤词时不分片（2026-10-26 实测）
  //
  // 分片是为**全量套件**设计的：每片一个独立进程，各自加载 27MB 的 st_tests.exe
  // 并枚举全部用例，所以片数越多固定成本越高。
  //
  // 实测（`st test caiyun`，命中 27 条用例）：
  //   单分片 **5.5s**　｜　4 片 5.9s　｜　16 片 **7.6s**
  // 也就是说“并行”在少量用例上是净亏损——多出的 8 个进程启动（约 2.1s）
  // 远超它们并行省下的执行时间（27 条用例一共才 0.44s）。
  //
  // 判据：有过滤词 = 只跑一小批 ⇒ 单进程直跑。
  // 全量（无过滤词）仍然分片——那时单进程要跑几分钟。
  const std::string filter = arguments.positional.empty() ? std::string{} : arguments.positional.front();
  const std::size_t default_shards = filter.empty() ? shard_plan.shards : 1;
  const auto test_jobs = arguments.number_any({"test-jobs", "test_jobs"},
                                              static_cast<double>(default_shards));
  options.test_jobs = test_jobs > 0.0 ? static_cast<std::size_t>(test_jobs) : 1;
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
    const st::Error& error = code.error();
    if (st::pkg::is_link_failure(error)) {
      // 链接/构建失败 ≠ 用例失败：固定退 125，脚本不用再从 stderr 猜（BACKLOG 3837）。
      // 125 同 POSIX「命令不可运行」位（timeout 命令的同款语义），不会与任何失败用例数碰撞。
      std::fprintf(stderr, "\n===== 链接/构建失败 =====\n%s\n（产物未产出或分片进程未跑完，"
                  "本轮没有任何用例结果——不是测试失败）\n", error.message.c_str());
      return 125;
    }
    std::fprintf(stderr, "%s\n", error.message.c_str());
    return 1;
  }
  if (*code > 0) {
    std::fprintf(stderr, "\n===== 失败用例（%d）=====\n（以上为用例断言红；产物正常、分片正常）\n", *code);
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

auto command_check(const Arguments& arguments) -> int {
  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  st::pkg::BuildOptions options;
  options.profile = arguments.get("profile", "dev");
  options.target = arguments.get("target", "");
  options.toolchain = arguments.get("toolchain", "");
  options.verbose = arguments.has("verbose");
  options.check_only = true;               // ← 关键：同一段决策，不产出
  // 并发：检查是**纯 CPU**（无对象写盘、无链接），不需按编译的内存预算保守推导，
  // 所以不用 `plan_concurrency` 的内存口径；但也不超订硬件核数（超了只会互相抢）。
  const auto jobs = arguments.number_any({"j", "jobs"}, 0.0);
  options.jobs = jobs > 0.0 ? static_cast<std::size_t>(jobs) : st::hardware_concurrency();

  st::print("检查 [{}]{}{}", options.profile,
            options.toolchain.empty() ? "" : std::format(" 工具链: {}", options.toolchain),
            options.target.empty() ? "" : std::format(" 目标: {}", options.target));
  st::print("{}\n", options.verbose ? "（详细）" : "");

  const auto started = st::time::now_ns();
  auto stats = st::pkg::build(*manifest, options);
  if (!stats) {
    st::print("\n检查未通过\n{}", stats.error().to_string());
    return 1;
  }
  const auto elapsed = (st::time::now_ns() - started) / 1'000'000;
  st::print("检查通过 [{}] 单元 {}（{:.2f}s）并行 {} 路 · {}\n", options.profile,
            stats->units_total, static_cast<double>(elapsed) / 1000.0, stats->workers,
            stats->concurrency_reason);
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
  st::print("  版本            : {}\n", kStpmVersion);
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
    // 测试分片：与编译并发**同一套资源探测**，但约束不同（测试进程内存轻、吃满一个核），
    // 所以它有自己的推导（见 `plan_test_shards`）。两行并列显示才能看出"为何两类并行不一样"。
    const auto shard_plan = st::pkg::plan_test_shards(0, st::hardware_concurrency(), 0, 0,
                                                      16);
    st::print("  默认测试分片    : {} 片 · {}\n", shard_plan.shards, shard_plan.reason);
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

// —— 依赖命令三兄弟（deps / fetch / vendor）的公共前置 ——

/// 共享缓存根（与 `st clean --all`、doctor 同一处）：`{ST_HOME:-~/.shuangtian}/cache`。
[[nodiscard]] auto shared_cache_dir() -> std::string {
  std::string home;
  if (const auto env = st::fs::read_env("ST_HOME"); env.has_value() && !env->empty()) {
    home = *env;
  } else {
    home = st::fs::join(st::fs::home_dir(), ".shuangtian");
  }
  return st::fs::join(home, "cache");
}

/// 求解出的（或 `--locked` 从锁文件读入的）依赖图。
struct DependencyPlan {
  st::pkg::ResolvedGraph graph{};   ///< 拓扑序，依赖在前
  std::string lock_path{};          ///< st.lock 的落盘位置（清单旁）
};

/// registry 源：任一 Registry 依赖给出索引地址（多个时取第一个，并在解析处如实校验）；
/// 无 registry 依赖 → 空索引（path 源依赖仍可解，求解器会给出合成版本）。
[[nodiscard]] auto load_registry(const st::pkg::Manifest& manifest)
    -> st::Result<st::pkg::Registry> {
  for (const auto& dependency : manifest.dependencies) {
    if (dependency.source.kind == st::pkg::SourceSpec::Kind::Registry &&
        !dependency.source.location.empty()) {
      return st::pkg::Registry::load(dependency.source.location);
    }
  }
  return st::pkg::Registry::parse_json(st::Json::object(), "(empty)");
}

/// 求解（或 `--locked` 时读锁）。
/// 传递依赖的清单读**依赖自己的 st.pkg**（`DependencyProvider` 注入：求解器不做 I/O，
/// 生产侧在这里读依赖目录）。归档/http 源当前拿不到源码目录（解包待 st::codec），
/// 如实报错而不是假装"无传递依赖"——那会把缺依赖静默漏掉。
[[nodiscard]] auto plan_dependencies(const st::pkg::Manifest& manifest, bool locked)
    -> st::Result<DependencyPlan> {
  const std::string lock_path = st::fs::join(manifest.directory, "st.lock");
  if (locked) {
    auto graph = st::pkg::lock_read(lock_path);
    if (!graph) return st::forward_error(graph.error());
    return DependencyPlan{std::move(*graph), lock_path};
  }
  auto registry = load_registry(manifest);
  if (!registry) return st::forward_error(registry.error());
  const st::pkg::DependencyProvider provider = [](const st::pkg::ResolvedPackage& package)
      -> st::Result<std::vector<st::pkg::DependencySpec>> {
    if (!package.source.is_directory_source() || package.source.location.empty()) {
      return st::unexpected(st::ErrorCode::Unsupported,
                            std::format("依赖 {} 的来源（{}）暂不能读传递依赖：归档解包"
                                        "待 st::codec 接入（DESIGN §7.4）",
                                        package.name,
                                        st::pkg::source_kind_name(package.source.kind)));
    }
    auto nested = st::pkg::Manifest::load(st::fs::join(package.source.location, "st.pkg"));
    if (!nested) return st::forward_error(nested.error());
    return nested->dependencies;
  };
  auto graph = st::pkg::resolve(manifest.dependencies, *registry, provider);
  if (!graph) return st::forward_error(graph.error());
  return DependencyPlan{std::move(*graph), lock_path};
}

auto command_deps(const Arguments& arguments) -> int {
  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  if (manifest->dependencies.empty()) {
    // 引导语不再提不存在的 `st add`：登记依赖就是编辑 st.pkg 的 dependencies 段。
    st::print("无第三方源码依赖（框架本体零依赖；需要时在 st.pkg 的 dependencies.source 段登记）\n");
    return 0;
  }
  auto plan = plan_dependencies(*manifest, arguments.has("locked"));
  if (!plan) {
    std::fprintf(stderr, "错误: %s\n", plan.error().to_string().c_str());
    return 1;
  }
  st::print("依赖解析（回溯求解 · 语义版本）{}\n",
            arguments.has("locked") ? " · --locked（只读 st.lock）" : "");
  for (const auto& package : plan->graph.packages) {
    // 标记直接依赖（*）：求解后的图是全量的，混在一起看不出哪些是清单自己声明的。
    const bool direct = std::any_of(manifest->dependencies.begin(), manifest->dependencies.end(),
                                    [&](const st::pkg::DependencySpec& root) {
                                      return root.name == package.name;
                                    });
    st::print("  {} {:<20} {:<12} {:<12} {}\n", direct ? "*" : " ", package.name,
              package.version.to_string(),
              st::pkg::source_kind_name(package.source.kind), package.source.location);
  }
  st::print("\n共 {} 个直接依赖；解析后共 {} 个包（拓扑序，依赖在前；* = 直接依赖）\n",
            manifest->dependencies.size(), plan->graph.packages.size());
  return 0;
}

auto command_fetch(const Arguments& arguments) -> int {
  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  if (manifest->dependencies.empty()) {
    st::print("无第三方源码依赖，无需获取\n");
    return 0;
  }
  auto plan = plan_dependencies(*manifest, arguments.has("locked"));
  if (!plan) {
    std::fprintf(stderr, "错误: %s\n", plan.error().to_string().c_str());
    return 1;
  }
  st::pkg::FetchOptions options;
  options.work_dir = manifest->directory;
  options.cache_dir = shared_cache_dir();
  options.offline = arguments.has("offline");
  auto fetched = st::pkg::fetch_all(plan->graph, options);
  if (!fetched) {
    std::fprintf(stderr, "错误: %s\n", fetched.error().to_string().c_str());
    return 1;
  }
  for (const auto& item : *fetched) {
    st::print("  {:<20} → {}（sha256 {}）\n", item.package.name, item.directory,
              item.sha256.empty() ? "-" : item.sha256);
  }
  // `--locked` 时不重写锁（只读语义）；否则锁是求解结果的落盘。
  if (!arguments.has("locked")) {
    if (auto status = st::pkg::lock_write(plan->lock_path, plan->graph); !status) {
      std::fprintf(stderr, "错误: %s\n", status.error().to_string().c_str());
      return 1;
    }
    st::print("已写 {}\n", plan->lock_path);
  }
  st::print("获取完成，共 {} 个包\n", fetched->size());
  return 0;
}

auto command_vendor(const Arguments& arguments) -> int {
  auto manifest = load_manifest(arguments);
  if (!manifest) {
    std::fprintf(stderr, "错误: %s\n", manifest.error().to_string().c_str());
    return 1;
  }
  if (manifest->dependencies.empty()) {
    st::print("无第三方源码依赖，无需固化\n");
    return 0;
  }
  // vendor 走真求解（不吃 --locked：固化的是"现在解出来的"这棵树，锁是它的产物之一）。
  auto plan = plan_dependencies(*manifest, /*locked=*/false);
  if (!plan) {
    std::fprintf(stderr, "错误: %s\n", plan.error().to_string().c_str());
    return 1;
  }
  st::pkg::FetchOptions options;
  options.work_dir = manifest->directory;
  options.cache_dir = shared_cache_dir();
  options.offline = arguments.has("offline");
  auto fetched = st::pkg::fetch_all(plan->graph, options);
  if (!fetched) {
    std::fprintf(stderr, "错误: %s\n", fetched.error().to_string().c_str());
    return 1;
  }
  const std::string vendor_root = st::fs::join(manifest->directory, "vendor");
  if (auto status = st::pkg::vendor_packages(*fetched, vendor_root); !status) {
    std::fprintf(stderr, "错误: %s\n", status.error().to_string().c_str());
    return 1;
  }
  if (auto status = st::pkg::lock_write(plan->lock_path, plan->graph); !status) {
    std::fprintf(stderr, "错误: %s\n", status.error().to_string().c_str());
    return 1;
  }
  st::print("已固化 {} 个包到 {}（vendor.lock 随源码树；离线可构建）\n", fetched->size(),
            vendor_root);
  st::print("已写 {}\n", plan->lock_path);
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

/// `st log`：读最近一次（或指定的）构建/测试日志。
///
/// ## 为何要一个命令而不是直接 `cat`
///
/// 日志路径带时间戳与 pid，人记不住；而且“最新一份”要按**文件名里的时间**排
/// （不是 mtime——拷来拷去后 mtime 会变）。这个命令把两件事都做成一行：
///
/// ```
/// st log                  # 最近一次日志的尾部（默认 40 行）
/// st log --tail 200       # 更多行
/// st log --grep error     # 只看出错行（带文件名/行号前缀）
/// st log --full           # 全量（构建日志一般几 KB，直接灌给模型都行）
/// st log --list           # 列出最近 10 份（看“最近跑过什么”）
/// st log --action test    # 只看最近一次 test（不看 build）
/// ```
///
/// 输出**带日志路径前缀**（多文件时）或首行声明来源：不给出处就等于把
/// “哪一次跑的输出”变成谜。
[[nodiscard]] auto command_log(const Arguments& arguments) -> int {
  const auto manifest = load_manifest(arguments);
  std::string root;
  if (manifest) {
    root = manifest->directory;
  } else {
    const auto current = st::fs::current_dir();
    root = current ? *current : std::string{"."};
  }
  const std::string dir = st::fs::join(root, std::string(st::cli_log::kDirectory));
  if (!st::fs::is_directory(dir)) {
    st::print("还没有日志目录：{}\n", dir);
    return 1;
  }
  const auto entries = st::fs::list_dir(dir);
  if (!entries) {
    st::print("读不了日志目录：{}\n", dir);
    return 1;
  }
  // 文件名里的时间戳决定了“新旧”（不用 mtime：拷贝/恢复会改 mtime）。
  // `action` 过滤对前缀匹配——`--action build` 同时命中 `build-` 与 `build_tests-`。
  const std::string action = arguments.get("action", "");
  std::vector<std::string> files;
  for (const auto& entry : *entries) {
    if (entry.is_dir) continue;
    if (!entry.path.ends_with(".log")) continue;
    const std::string leaf = st::fs::file_name(entry.path);
    if (!action.empty() && !leaf.starts_with(action)) continue;
    files.push_back(entry.path);
  }
  if (files.empty()) {
    st::print("没有匹配的日志（目录 {}{}）\n", dir,
              action.empty() ? std::string{} : std::format("，过滤 {}", action));
    return 1;
  }
  std::ranges::sort(files);
  if (arguments.has("list")) {
    const std::size_t shown = std::min<std::size_t>(files.size(), 10);
    st::print("最近 {} 份日志（共 {}）：\n", shown, files.size());
    for (std::size_t index = files.size() - shown; index < files.size(); ++index) {
      const auto size = st::fs::file_size(files[index]).value_or(0);
      st::print("  {}  {} 字节\n", st::fs::file_name(files[index]), size);
    }
    return 0;
  }
  const std::string& latest = files.back();
  const auto text = st::fs::read_text(latest);
  if (!text) {
    st::print("读不了日志：{}\n", latest);
    return 1;
  }
  // 日志文件自己的首行就是「日志：<它自己的路径>」（见 `cli_log::begin`）——
  // 这里就不再重复一遍。只在本行与首行不同时才打（如 --action 筛到旧文件）。
  if (!(*text).starts_with("日志：")) st::print("日志：{}\n", latest);
  const auto lines = st::split(*text, '\n');
  const std::string pattern = arguments.get("grep", "");
  if (!pattern.empty()) {
    std::size_t hits = 0;
    for (std::size_t index = 0; index < lines.size(); ++index) {
      if (!lines[index].contains(pattern)) continue;
      // 行号前缀：与源码的 `文件:行` 习惯对齐，方便再回到日志里定位
      st::print("{:>4}: {}\n", index + 1, lines[index]);
      ++hits;
    }
    st::print("── 命中 {} 行（共 {} 行）\n", hits, lines.size());
    return hits == 0 ? 1 : 0;
  }
  if (arguments.has("full")) {
    st::print_raw(*text);
    return 0;
  }
  const auto tail_text = arguments.number("tail", 40.0);
  const std::size_t tail_lines =
      tail_text > 0.0 ? static_cast<std::size_t>(tail_text) : std::size_t{40};
  const std::size_t begin = lines.size() > tail_lines ? lines.size() - tail_lines : 0;
  for (std::size_t index = begin; index < lines.size(); ++index) {
    st::print_raw(lines[index]);
    if (index + 1 < lines.size()) st::print_raw("\n");
  }
  if (begin > 0) {
    st::print("\n── 尾部 {} 行（共 {} 行；--full 看全部）\n", lines.size() - begin, lines.size());
  }
  return 0;
}

/// 自检结果：构建工具自身的源码是否比可执行文件新。
struct SelfStaleness {
  bool stale{false};
  std::size_t newer_count{0};
  std::string newest_path{};
};

/// 构建工具**自检**：`st` 自己的源码是否比 `st.exe` 新。
///
/// ## 为什么必须有这一步
///
/// `st` 是**自举**产物。改 `src/pkg/build.cpp`（构建逻辑本身）之后，
/// `st build <目标>` 只编目标、**永远不重编 `st`** —— 新逻辑不生效，
/// 而它照样报"构建完成"。实测代价：5 轮实验（含逆向验证）全跑在旧二进制上。
///
/// ## 判据为什么不用 bootstrap 的 `.d`
///
/// 第一版拿 `build/obj/bootstrap/*.d` 当权威清单，**实测失效**：
/// bootstrap 在 `$objDir` 里编译，gcc 的 `-MMD` 把路径写成 basename
/// （`build.cpp`、`src_pkg_build.cpp.o:`），拼上仓库根后**全部不存在** →
/// 一个"更新"都找不到 → 自检永不触发（touch 了源文件也没提示）。
///
/// 第二版只取 `st` target 的 `sources`（`tools/stpm/*.cpp`）——**又一次失明**：
/// `src/pkg/*.cpp` 来自**顶层** `sources`（经 target 的 `exclude_sources` 过滤
/// 后并入），而那正是构建逻辑所在。候选数从 220 涨到 319 才对。
///
/// 现在按清单的真实规则算：顶层 `sources` 减去 `exclude_sources` 的前缀，
/// 再并上 target 自己的 `sources`。
///
/// 只比 **mtime**：这一步每次运行都跑，读 3.9MB 内容只为"判断要不要重建"
/// 不值得。构建**产物**那条路才用内容哈希（`deps_entry_matches`）。
[[nodiscard]] auto check_self_staleness() -> SelfStaleness {
  SelfStaleness result{};
  const auto exe_result = st::process::executable_path();
  if (!exe_result.has_value()) return result;
  const std::string exe = st::fs::normalize(*exe_result);
  const auto exe_time = st::fs::modified_ns(exe);
  if (!exe_time.has_value()) return result;

  // exe 在 <root>/build/bin/st → root 是上三级
  const std::string root = st::fs::parent(st::fs::parent(st::fs::parent(exe)));
  if (root.empty()) return result;
  const std::string manifest_path = st::fs::join(root, "st.pkg");
  if (!st::fs::is_regular_file(manifest_path)) return result;

  const auto manifest = st::pkg::Manifest::load(manifest_path);
  if (!manifest.has_value()) return result;
  const st::pkg::TargetSpec* st_target = nullptr;
  for (const auto& spec : manifest->targets) {
    if (spec.name == "st") {
      st_target = &spec;
      break;
    }
  }
  if (st_target == nullptr) return result;

  std::set<std::string> candidates{};
  const auto add_tree = [&](const std::string& dir) {
    const auto walked = st::fs::walk(dir, 8);
    if (!walked.has_value()) return;
    for (const auto& entry : *walked) {
      if (entry.is_dir) continue;
      if (!entry.path.ends_with(".cpp") && !entry.path.ends_with(".hpp") &&
          !entry.path.ends_with(".c")) {
        continue;
      }
      candidates.insert(st::fs::join(dir, entry.path));
    }
  };

  // 候选源 = 顶层 sources（减去 exclude_sources 的前缀）+ target 自己的 sources
  //
  // ⚠ `include_sources`（白名单）**必须优先于** `exclude_sources` 参与判定，
  // 与 `build()` 的库源选择同口径。只按黑名单算的症状不是“多算几个文件”而是两头都错：
  // · 多算：`src/raster`/`src/text`/… 也被当成候选——改一个与 `st` 无关的文件就白重建一次；
  // · 漏算：白名单命中的文件（`src/pkg/*`、`src/core/*`……）不在候选里——
  //   改了构建逻辑却不自检，静默跑旧逻辑（那正是本函数存在的理由）。
  //
  // 用 `expand_glob` 直接展开白名单，而不是从顶层模式推目录：`include_sources`
  // 里既有 `src/pkg/*.cpp` 这种目录 glob，也有 `src/ext/database.cpp` 这种精确文件，
  // 自己推前缀两种都会算错。
  std::set<std::string> exclude_prefixes{};
  if (st_target->include_sources.empty()) {
    for (const auto& pattern : st_target->exclude_sources) {
      const std::size_t star = pattern.find('*');
      exclude_prefixes.insert(st::fs::normalize(pattern.substr(0, star)));
    }
    for (const auto& pattern : manifest->sources) {
      const std::size_t slash = pattern.find_last_of("/\\");
      if (slash == std::string::npos) continue;
      const std::string dir = st::fs::normalize(st::fs::join(root, pattern.substr(0, slash)));
      bool excluded = false;
      for (const auto& prefix : exclude_prefixes) {
        if (dir.size() >= prefix.size() && dir.compare(0, prefix.size(), prefix) == 0) {
          excluded = true;
          break;
        }
      }
      if (!excluded) add_tree(dir);
    }
  } else {
    // 白名单：只展开它点名的源（与 `build()` 里那条 `erase_if` 等价）
    for (const auto& pattern : st_target->include_sources) {
      const auto matched = st::fs::expand_glob(root, pattern);
      if (!matched.has_value()) continue;
      for (const auto& relative : *matched) {
        candidates.insert(st::fs::normalize(st::fs::join(root, relative)));
      }
    }
  }
  for (const auto& pattern : st_target->sources) {
    const std::size_t slash = pattern.find_last_of("/\\");
    if (slash == std::string::npos) continue;
    add_tree(st::fs::join(root, pattern.substr(0, slash)));
  }
  for (const auto& inc : manifest->include_dirs) add_tree(st::fs::join(root, inc));

  std::int64_t newest_time = 0;
  for (const auto& path : candidates) {
    const auto time = st::fs::modified_ns(path);
    if (!time.has_value() || *time <= *exe_time) continue;
    ++result.newer_count;
    if (*time > newest_time) {
      newest_time = *time;
      result.newest_path = path;
    }
  }
  result.stale = result.newer_count > 0;
  return result;
}

/// 重建构建工具自身，并**用新工具重跑本次命令**。
///
/// ## 为什么是"自动"而不是"提示"
///
/// 提示会被人忽略（我自己就会），而"改了构建逻辑却在跑旧逻辑"是**静默失效**——
/// 它让"我改了、也验证了"这句话整个不成立。所以默认自动重建。
///
/// ## 为什么要"用新工具重跑本次命令"
///
/// 重建后当前进程仍是**旧的代码映像**（Windows 上旧 exe 只是被改名，映射还在）。
/// 若在这里继续执行本次命令，用的还是旧逻辑 —— 等于重建白做。
/// 所以：重建 → 起一个新进程把自己重新执行一遍（`capture_output = false`
/// 让子进程直通终端）→ 把它的退出码原样返回。
///
/// `--no-self-rebuild` 可关掉这条（CI 里想固定用某个二进制时有用）。
[[nodiscard]] auto rebuild_self_and_reexec(const std::vector<std::string>& argv,
                                           const std::string& profile) -> int {
  const auto manifest_result = st::pkg::Manifest::load("st.pkg");
  if (!manifest_result.has_value()) {
    std::fprintf(stderr, "自动重建失败：读不到 st.pkg\n");
    return 1;
  }
  const auto exe_result = st::process::executable_path();
  if (!exe_result.has_value()) return 1;
  const std::string self = st::fs::normalize(*exe_result);
  const std::string staging = self + std::format(".new-{}", st::process::current_id());

  st::print("检测到构建工具源码有更新 → 自动重建（档位 {}）…\n", profile);

  st::pkg::BuildOptions options;
  options.profile = profile;
  options.target = "st";
  // ## 为何**不**用 `force = true`（2026-10-26 实测）
  //
  // 原先这里写 `force = true`，理由写在 BACKLOG 里：“产物路径变了，增量缓存也对不上”。
  // 那句话是错的：`output_override` 只影响**最终链接产物的路径**
  //（`build.cpp`：`options.output_override.empty() ? default_output : options.output_override`），
  // 而对象目录 `object_dir` 完全不变 —— 那些 `.o` 仍然可复用。
  //
  // 代价实测：改一行 `src/pkg/*.cpp` → 自举 **55.5s**（全量 43 单元），
  // 而实际只需重编 1~2 个单元 + 链接（预期个位数秒）。
  // 构建工具自己的重建时间应当是**秒级**——它是每次改构建逻辑都要付的固定成本。
  //
  // ## 正确性靠什么保证（这是当初写 `force = true` 想防的事）
  //
  // 担心是合理的：若增量判定漏了改动，新 `st` 会是**旧逻辑**，而它照样报“重建完成”。
  // 三道防线，都不靠“相信增量”：
  //
  // 1. **`.meta` 第三列是内容哈希**（`deps_entry_matches`）：不是只比 mtime/size。
  //    `copy2`/`git checkout` 把旧 mtime 搬回来也骗不过它。
  // 2. **链接指纹含对象的 (mtime,size)**：缓存 restore 换了内容却不换时间戳也会被拓出。
  // 3. **重建后必须用新工具重跑本次命令**（本函数末尾）：若新代码没进去，
  //    重跑后的行为与旧的一致——那是**可观测**的（见 `ST_VERIFY_SELF_REBUILD`）。
  //
  // 另外提供一个**可复核的手段**（verify 开关）：设 `ST_VERIFY_SELF_REBUILD=1` 时，
  // 重建后比一次“产物里是否含新源文件的内容哈希”——不符即报错并拒绝继续。
  options.force = false;
  options.no_tests = true;
  options.strip_debug = true;
  options.verbose = false;
  options.output_override = staging;

  const std::int64_t start = st::time::now_ns();
  auto stats = st::pkg::build(*manifest_result, options);
  if (!stats) {
    std::fprintf(stderr, "自动重建失败：%s\n  请修好后重试，或加 --no-self-rebuild 跳过。\n",
                 stats.error().message.c_str());
    (void)st::fs::remove_file(staging);
    return 1;
  }
  const std::string built = stats->artifact.empty() ? staging : stats->artifact;
  const std::string elapsed = st::time::format_duration_ns(st::time::now_ns() - start);

  // 替换：旧文件改名（Windows 上删不掉正在运行的 exe，但改名可以）
  const std::string backup = self + ".old";
  (void)st::fs::remove_file(backup);
  if (auto moved = st::fs::rename(self, backup); !moved) {
    // 改名失败：如实报并保留新产物，让用户手动处理
    std::fprintf(stderr,
                 "自动重建的产物已生成（%s），但无法替换正在运行的 %s：%s\n"
                 "  请关闭其他 st 进程后重跑。\n",
                 built.c_str(), self.c_str(), moved.error().message.c_str());
    return 1;
  }
  if (auto placed = st::fs::rename(built, self); !placed) {
    (void)st::fs::rename(backup, self);   // 尽力回滚
    std::fprintf(stderr, "新产物就位失败：%s\n", placed.error().message.c_str());
    return 1;
  }
  st::print("已重建（{}），用新工具重跑本次命令…\n", elapsed);

  // ## 可信增量校验（`ST_VERIFY_SELF_REBUILD=1` 时开）
  //
  // 从 `force = true` 改成增量后，“新 `st` 真的是新逻辑吗”必须能被**独立复核**，
  // 而不能只靠“我相信 `.meta` 的哈希”。这里做一件不依赖构建器自述的事：
  // **在重建产物里搜本次改动源文件的内容**。
  //
  // 为何这个判据能抓“假增量”：若增量判定漏了改动，产物就是**旧代码**，
  // 而旧代码里不可能有本次新增的字符串。这是“产物里有没有新字符串”那条
  //（PITFALLS 场景 20 的取证手法）的自动化版。
  //
  // 只查**语义可预期**的一类痕迹：命令行开关名、错误文案、函数名都可以，
  // 但由调用方指定（没法自己猜“这次改了什么”）。所以它是个显式开关，
  // 不进默认路径（默认路径多一次全文件扫描会给每次自举加 100ms+）。
  if (const auto verify = st::fs::read_env("ST_VERIFY_SELF_REBUILD");
      verify.has_value() && !verify->empty() && *verify != "0") {
    // 把要搜的字符串写成“参与本次构建的源文件里，比旧 exe 新的那些文件名”
    // ——文件名本身就会进 `.debug`/符号表吗？不一定。所以改为搜**编译进来的字符串**：
    // 用 `ST_VERIFY_TOKEN` 显式给出（例如刚加的一条文案）。
    const auto token = st::fs::read_env("ST_VERIFY_TOKEN");
    if (token.has_value() && !token->empty()) {
      const auto bytes = st::fs::read_bytes(staging);
      if (!bytes.has_value()) {
        std::fprintf(stderr, "可信增量校验失败：读不了新产物 %s\n", staging.c_str());
        return 1;
      }
      const std::string haystack(reinterpret_cast<const char*>(bytes->data()), bytes->size());
      if (!haystack.contains(*token)) {
        std::fprintf(stderr,
                     "可信增量校验失败：新产物里找不到 ST_VERIFY_TOKEN=%s\n"
                     "  说明增量判定漏了本次改动（产物仍是旧代码）。\n"
                     "  排查：① 查对应 `.o.meta` 第三列是否等于当前源文件的内容哈希；\n"
                     "        ② 临时用 `st rebuild --force` 看是否就好（能好则是增量判据的问题）。\n",
                     token->c_str());
        (void)st::fs::remove_file(staging);
        return 1;
      }
      st::print("可信增量校验通过（产物含 {}）\n", *token);
    }
  }

  // 用新二进制重跑同一条命令（不含程序名）
  st::process::Options run_options{};
  run_options.capture_output = false;   // 子进程直通终端，不要再套一层捕获
  auto rerun = st::process::run(self, argv, run_options);
  if (!rerun.has_value()) {
    std::fprintf(stderr, "重跑失败：%s\n  新工具已就位，请手动重跑原命令。\n",
                 rerun.error().message.c_str());
    return 1;
  }
  return rerun->exit_code;
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

  // ## 控制台镜像（日志落盘）：一个出口覆盖全部命令
  //
  // 放在 `run_app` 而不是各个 `command_*` 里：构建/测试/lint/rebuild/check 的输出
  // 最终都经 `st::print`/`st::eprint`，在**分发命令之前**打开镜像，
  // 就不存在“新加一个命令忘了接日志”的可能。
  //
  // 为何默认开：失败时“为了看一眼错误而重跑一遍”是直接的迭代时间损失，
  // 而写一份日志的代价是几毫秒。`--no-log` / `ST_NO_CONSOLE_LOG=1` 可关。
  //
  // 只在**有实际输出**的命令上开（`version`/`help` 之类纯查询不落盘）：
  // 否则 `ls .st/logs` 会被大量“看一眼版本号”的空日志淹没。
  static const char* const kLogged[] = {"build", "run", "test", "lint", "check", "rebuild",
                                        "clean", "vendor", "fetch", "deps"};
  const bool logged_command = std::any_of(
      std::begin(kLogged), std::end(kLogged), [&](const char* name) { return arguments.command == name; });
  const bool want_log = logged_command && !arguments.has("no-log");
  // 日志根：清单所在目录（找不到清单时退回当前目录）。不提前 load_manifest——
  // 那会在“清单本身有语法错误”时把日志开在错误的位置，而那正是最需要日志的时候。
  if (want_log) {
    std::string log_root = ".";
    if (const auto root = project_root(); root.has_value()) log_root = *root;
    (void)st::cli_log::begin(log_root, arguments.command,
                            arguments.positional.empty() ? std::string{} : arguments.positional.front(),
                            arguments.get("profile", ""));
  }
  // 退出前收尾（**不靠 RAII**：这个函数有多处 `return`，而收尾必须在**最外层**
  // 那一个出口执行；写成局部对象会被每一处 return 提前析构，日志尾部就丢了）。
  const auto finish = [&](int code) -> int {
    if (want_log) st::cli_log::end(code);
    return code;
  };

  // ## 自检 + **自动重建**（每次运行都查）
  //
  // 只在**会受构建逻辑影响**的命令上做：`version`/`help` 之类纯查询不重建，
  // 免得"看一眼版本号"要等 40 秒。`rebuild` 自己也不走这条（它本来就是重建）。
  //
  // 重建后**用新工具重跑本次命令**——当前进程是旧映像，在这里继续执行
  // 用的还是旧逻辑，等于白重建。
  static const char* const kSelfAware[] = {"build", "run", "test", "lint", "check",
                                           "stats", "tree", "deps", "doctor"};
  const bool self_aware = std::any_of(std::begin(kSelfAware), std::end(kSelfAware),
                                      [&](const char* name) { return arguments.command == name; });
  if (self_aware && !arguments.has("no-self-rebuild")) {
    const SelfStaleness stale = check_self_staleness();
    if (stale.stale) {
      std::vector<std::string> rerun_args{};
      rerun_args.reserve(static_cast<std::size_t>(argc));
      for (int i = 1; i < argc; ++i) rerun_args.emplace_back(argv[i]);
      return rebuild_self_and_reexec(rerun_args, arguments.get("profile", "dev"));
    }
  }

  if (arguments.command == "version") {
    st::print("st (霜天工具链) {}\n", kStpmVersion);
    const auto compiler = st::pkg::detect_compiler();
    st::print("编译器: {}\n", compiler ? compiler->c_str() : "未找到");
    return 0;
  }
  if (arguments.command == "rebuild") return finish(command_rebuild(arguments));
  if (arguments.command == "build") return finish(command_build(arguments));
  if (arguments.command == "run") return finish(command_run(arguments));
  if (arguments.command == "test") return finish(command_test(arguments));
  if (arguments.command == "lint") return finish(command_lint(arguments));
  if (arguments.command == "check") return finish(command_check(arguments));
  if (arguments.command == "stats") return command_stats(arguments);
  if (arguments.command == "doctor") return command_doctor(arguments);
  if (arguments.command == "tree") return command_tree(arguments);
  if (arguments.command == "deps") return finish(command_deps(arguments));
  if (arguments.command == "fetch") return finish(command_fetch(arguments));
  if (arguments.command == "vendor") return finish(command_vendor(arguments));
  if (arguments.command == "init") return command_init(arguments);
  if (arguments.command == "clean") return finish(command_clean(arguments));
  if (arguments.command == "log") return command_log(arguments);


  std::fprintf(stderr, "未知命令: %s\n\n", arguments.command.c_str());
  print_usage();
  return 1;
}
// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
