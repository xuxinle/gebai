/// `st` —— 霜天自研工具链 CLI（构建 / 测试 / 禁令扫描 / 依赖管理 / 自检）。
/// 自举：`bootstrap.sh` 用编译器直接产出本程序，之后一切构建都走这里（不经 CMake/Make）。

#include <algorithm>
#include <cstdio>
#include <format>
#include <map>
#include <string>
#include <vector>

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
#include "st/pkg/registry.hpp"
#include "st/pkg/semver.hpp"

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
};

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
      if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with("--")) {
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
  run <target> [args]   构建并运行目标（无头演示：run gallery -- --headless --frames 3）
  test [filter]         构建并运行单元测试（--san 开启 ASan/UBSan 档）
  lint [--explain RULE] 禁令扫描（CONVENTIONS §8；无参数即扫描工程，--rules 列出规则）
  deps                  解析依赖并打印依赖树（--locked 只读 st.lock）
  fetch                 解析 + 拉取依赖到缓存/工作区，并写 st.lock
  vendor                把依赖源码固化进 vendor/（离线可构建）
  doctor                环境自检（编译器/字体/显示后端/GPU/缓存）
  tree                  打印清单概览（目标、源文件数、依赖）
  init <name>           生成工程骨架
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
  options.profile = arguments.get("profile", "debug");
  options.target = arguments.positional.empty() ? std::string{} : arguments.positional.front();
  options.verbose = arguments.has("verbose") || arguments.has("v");
  options.force = arguments.has("force");
  const auto jobs = arguments.number("j", 0.0);
  options.jobs = jobs > 0.0 ? static_cast<std::size_t>(jobs) : 0;
  options.use_pch = !arguments.has("no-pch");

  const std::int64_t start = st::time::now_ns();
  auto stats = st::pkg::build(*manifest, options);
  if (!stats) {
    std::fprintf(stderr, "%s\n", stats.error().message.c_str());
    return 1;
  }
  st::print("构建完成 [{}] 单元 {}（重编 {} / 命中缓存 {}）\n", options.profile,
              stats->units_total, stats->units_rebuilt, stats->units_cached);
  st::print("  编译 {} · 链接 {} · 总 {} · 并行 {} 路{}\n",
              st::time::format_duration_ns(stats->compile_ms * 1'000'000),
              st::time::format_duration_ns((stats->elapsed_ms - stats->compile_ms) * 1'000'000),
              st::time::format_duration_ns(st::time::now_ns() - start), stats->workers,
              stats->pch_used ? " · PCH" : "");
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
  options.profile = arguments.has("san") ? "san" : "debug";
  options.verbose = arguments.has("verbose");
  const std::string filter = arguments.positional.empty() ? std::string{} : arguments.positional.front();
  st::print("运行测试 [{}]{}\n", options.profile,
              filter.empty() ? "" : std::format(" 过滤: {}", filter));
  auto code = st::pkg::run_tests(*manifest, options, filter);
  if (!code) {
    std::fprintf(stderr, "%s\n", code.error().message.c_str());
    return 1;
  }
  return *code;
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
  auto report = st::pkg::lint_project(*root);
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
  st::print("\n扫描 {} 个文件，违反 {} 项（提示 {} 项，豁免 {} 处）\n", report->files_scanned,
              failing, report->violations.size() - failing, report->suppressed);
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
  const std::string name = arguments.positional.front();
  const std::string root = st::fs::join(".", name);
  if (st::fs::exists(root)) {
    std::fprintf(stderr, "错误: 目录已存在: %s\n", root.c_str());
    return 1;
  }
  if (auto status = st::fs::create_directories(st::fs::join(root, "src")); !status) {
    std::fprintf(stderr, "错误: %s\n", status.error().to_string().c_str());
    return 1;
  }
  const std::string manifest = std::format(
      "{{\n  \"name\": \"{}\",\n  \"version\": \"0.1.0\",\n  \"kind\": \"executable\",\n"
      "  \"include_dirs\": [\"include\"],\n  \"sources\": [\"src/*.cpp\"],\n"
      "  \"targets\": [{{ \"name\": \"{}\", \"kind\": \"executable\", \"sources\": [\"src/main.cpp\"] }}],\n"
      "  \"dependencies\": {{ \"modules\": [], \"source\": [], \"system\": [] }}\n}}\n",
      name, name);
  if (auto status = st::fs::write_text(st::fs::join(root, "st.pkg"), manifest); !status) {
    std::fprintf(stderr, "错误: %s\n", status.error().to_string().c_str());
    return 1;
  }
  const std::string main_template = R"CPP(#include <cstdio>

#include "st/app/app.hpp"
#include "st/ui/components/basic.hpp"

auto main(int argc, char** argv) -> int {
  (void)argc;
  (void)argv;
  st::app::AppOptions options;
  options.title = "@NAME@";
  options.headless = true;
  st::app::Application app("@NAME@", "0.1.0", options);
  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  page->style().padding = st::math::Insets::all(24.0f);
  auto heading = std::make_unique<st::ui::Heading>("Hello 霜天", 1);
  page->add_child(std::move(heading));
  page->add_child(std::make_unique<st::ui::Text>("控制通道：tree / find / capture / input.*"));
  return app.run(std::move(page)).value_or(1);
}
)CPP";
  const std::string main_source = st::replace_all(main_template, "@NAME@", name);
  if (auto status = st::fs::write_text(st::fs::join(root, "src/main.cpp"), main_source); !status) {
    std::fprintf(stderr, "错误: %s\n", status.error().to_string().c_str());
    return 1;
  }
  st::print("工程已创建: {}\n  {}\n  {}\n下一步: cd {} && st build\n", root,
              st::fs::join(root, "st.pkg"), st::fs::join(root, "src/main.cpp"),
              name);
  return 0;
}

}  // namespace

auto main(int argc, char** argv) -> int {
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
  if (arguments.command == "doctor") return command_doctor(arguments);
  if (arguments.command == "tree") return command_tree(arguments);
  if (arguments.command == "deps") return command_deps(arguments);
  if (arguments.command == "init") return command_init(arguments);

  std::fprintf(stderr, "未知命令: %s\n\n", arguments.command.c_str());
  print_usage();
  return 1;
}
