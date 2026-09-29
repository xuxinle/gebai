#include "st/pkg/build.hpp"
#include "st/pkg/embed.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <format>
#include <mutex>
#include <unordered_set>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/thread_pool.hpp"
#include "st/core/time.hpp"

namespace st::pkg {
namespace {

/// 源文件语言（决定编译器标志：`.c` 必须按 C 编译，不能用 `-std=c++20`）。
enum class SourceLang : std::uint8_t { Cxx, C };

struct CompileUnit {
  std::string source{};   ///< 绝对路径
  std::string object{};   ///< 绝对路径
  std::string depfile{};  ///< `-MMD` 产出的头依赖清单（`.d`）
  SourceLang lang{SourceLang::Cxx};
  bool vendor{false};     ///< 第三方源码（放宽告警、不进 PCH）
  /// 该单元专属的包含目录。
  ///
  /// 为什么需要"按单元"而不是全局 `-I`：编译期嵌入生成的 `battery/embed.hpp` 是**按目标隔离**的
  /// （不同目标嵌入不同资源，声明集合不同），无法放进工程的公共包含路径。
  std::vector<std::string> extra_include_dirs{};
};

/// 按扩展名判定语言；非 `.c` 一律按 C++ 处理（`.cpp/.cc/.cxx`）。
[[nodiscard]] auto language_of(std::string_view path) noexcept -> SourceLang {
  const std::size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos) return SourceLang::Cxx;
  const std::string_view extension = path.substr(dot);
  return extension == ".c" ? SourceLang::C : SourceLang::Cxx;
}

/// 解析 GCC/Clang 的 `.d` 文件（`目标: 依赖…`，反斜杠续行；支持转义空格）。
[[nodiscard]] auto parse_depfile(std::string_view path) -> std::vector<std::string> {
  auto text = fs::read_text(path);
  if (!text) return {};
  std::vector<std::string> deps;
  std::string token;
  const auto flush = [&deps, &token]() {
    if (token.empty()) return;
    if (token.back() == ':') token.pop_back();
    if (!token.empty()) deps.push_back(token);
    token.clear();
  };
  bool seen_colon = false;
  for (std::size_t index = 0; index < text->size(); ++index) {
    const char raw = (*text)[index];
    if (raw == '\\' && index + 1 < text->size()) {
      const char next = (*text)[index + 1];
      if (next == ' ') {
        token.push_back(' ');
        ++index;
        continue;
      }
      if (next == '\n' || next == '\r') {
        flush();
        seen_colon = true;
        ++index;
        continue;
      }
    }
    if (raw == '\n' || raw == '\r') {
      flush();
      continue;
    }
    if (raw == ':') {
      token.clear();
      seen_colon = true;
      continue;
    }
    if (raw == ' ' || raw == '\t') {
      if (seen_colon) flush();
      continue;
    }
    token.push_back(raw);
  }
  flush();
  return deps;
}

/// 目标是否需要重建：源文件或**任一被包含的头文件**比目标新时重建（增量正确性的关键）。
[[nodiscard]] auto needs_rebuild(const CompileUnit& unit) -> bool {
  if (!fs::is_regular_file(unit.object)) return true;
  const auto object_time = fs::modified_ns(unit.object);
  if (!object_time) return true;
  const auto source_time = fs::modified_ns(unit.source);
  if (!source_time) return true;
  if (*source_time > *object_time) return true;
  for (const auto& dep : parse_depfile(unit.depfile)) {
    const auto dep_time = fs::modified_ns(dep);
    if (dep_time && *dep_time > *object_time) return true;
    if (!dep_time && !fs::exists(dep)) return true;  // 头被删除 → 重编
  }
  return false;
}

[[nodiscard]] auto sanitize_name(std::string_view relative) -> std::string {
  std::string name(relative);
  std::ranges::replace(name, '/', '_');
  std::ranges::replace(name, '\\', '_');
  std::ranges::replace(name, ':', '_');
  return name;
}

/// 单元 → 专属包含目录（编译期嵌入用；未命中即无额外目录）。
using UnitIncludeMap = std::map<std::string, std::vector<std::string>>;

[[nodiscard]] auto make_units(const Manifest& manifest, const std::vector<std::string>& sources,
                              const std::string& object_dir,
                              const std::set<std::string>& vendor_paths,
                              const UnitIncludeMap& unit_includes = {})
    -> std::vector<CompileUnit> {
  std::vector<CompileUnit> units;
  units.reserve(sources.size());
  for (const auto& relative : sources) {
    CompileUnit unit;
    unit.source = fs::is_absolute(relative) ? relative : fs::join(manifest.directory, relative);
    unit.object = fs::join(object_dir, sanitize_name(relative).append(".o"));
    unit.depfile = unit.object + ".d";
    unit.lang = language_of(unit.source);
    unit.vendor = vendor_paths.contains(unit.source);
    if (const auto found = unit_includes.find(unit.source); found != unit_includes.end()) {
      unit.extra_include_dirs = found->second;
    }
    units.push_back(std::move(unit));
  }
  return units;
}

/// 过滤掉 sanitizer 相关标志。
///
/// 第三方源码**不做插桩**：我们负责自家代码的内存/未定义行为，不负责上游的
/// （真发现了也是"改上游"而不是"改我们的构建"）。实践上还有两个硬理由：
/// ① 65k 行的 `quickjs.c` 在 `-O1` + ASan/UBSan 下单文件就要 GB 级内存，
///    并行构建会被 OOM killer 杀掉 cc1plus（实测）；
/// ② 混编不影响检测能力——ASan 的分配器是全局的，我们代码里的越界/释放后使用照抓不误。
[[nodiscard]] auto strip_sanitizers(const std::vector<std::string>& flags)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(flags.size());
  for (const auto& flag : flags) {
    if (flag.starts_with("-fsanitize=") || flag.starts_with("-fno-sanitize=")) continue;
    out.push_back(flag);
  }
  return out;
}


/// 把**工程级嵌入**（`manifest.embed`）接进库源集合：生成字节数组 + 声明头，
/// 并把包含目录登记到所有库单元（否则库自身的源码 `#include "battery/embed.hpp"` 找不到）。
///
/// 目标级嵌入不在这里处理：它按目标隔离，见 `build()` 的目标分支。
[[nodiscard]] auto attach_library_embeds(const Manifest& manifest, std::string_view profile,
                                        std::vector<std::string>& library_sources,
                                        std::set<std::string>& vendor_paths,
                                        UnitIncludeMap& unit_includes) -> Status {
  if (manifest.embed.empty()) return ok();
  auto embeds = generate_embeds(manifest, /*target=*/{}, profile, /*with_runtime=*/true);
  if (!embeds) return forward_error(embeds.error());
  for (const auto& source : library_sources) {
    const std::string key =
        fs::is_absolute(source) ? source : fs::join(manifest.directory, source);
    unit_includes[key].push_back(embeds->include_dir);
  }
  for (const auto& generated : embeds->sources) {
    library_sources.push_back(generated);
    vendor_paths.insert(generated);
    unit_includes[generated].push_back(embeds->include_dir);
  }
  return ok();
}

/// 展开第三方源码清单（失败按空集处理：清单没声明 vendor 也能构建）。
[[nodiscard]] auto collect_vendor(const Manifest& manifest) -> std::set<std::string> {
  auto files = manifest.vendor_files();
  if (!files) return {};
  return std::set<std::string>(files->begin(), files->end());
}

[[nodiscard]] auto gather_include_dirs(const Manifest& manifest, const BuildOptions& options)
    -> std::vector<std::string> {
  std::vector<std::string> dirs;
  const auto push = [&dirs](const std::string& dir) {
    if (dir.empty()) return;
    if (std::ranges::find(dirs, dir) == dirs.end()) dirs.push_back(dir);
  };
  push(fs::join(manifest.directory, "include"));
  push(manifest.directory);
  for (const auto& extra : manifest.include_dirs) {
    push(fs::is_absolute(extra) ? extra : fs::join(manifest.directory, extra));
  }
  for (const auto& extra : options.extra_include_dirs) push(extra);
  return dirs;
}

struct PchContext {
  std::string directory{};  ///< 含 `st.hpp.gch` 的目录（作为 `-I` 传入）
  std::string header{};     ///< `-include` 的头名（如 `st.hpp`）
};

/// 构建（或复用）预编译头；失败时返回空（不阻断构建，只是慢一点）。
///
/// 关键细节（都踩过坑）：
/// 1. **代理头**：不直接编译 `pch.hpp`——头文件被当作"主文件"编译时 `#pragma once` 必然告警，
///    而为了压掉它需要 `-Wno-error`，这又会让 PCH 与消费者**标志不一致 → GCC 判 PCH 无效**。
///    改为生成 `prefix.hpp`（仅一行 `#include "st/pch.hpp"`）作为编译入口，问题消失且标志可完全一致。
/// 2. **标志一致**：PCH 创建与消费必须使用同一套标志（std/优化/告警/宏/包含路径）；
///    消费端只多 `-include prefix.hpp` 与依赖输出选项（`-MMD -MF`，与 PCH 有效性无关）。
[[nodiscard]] auto ensure_pch(const std::string& compiler, const std::vector<std::string>& flags,
                              const std::vector<std::string>& include_dirs,
                              const std::string& build_dir, const std::string& header_path)
    -> std::optional<PchContext> {
  if (!fs::is_regular_file(header_path)) return std::nullopt;
  const std::string directory = fs::join(build_dir, "pch");
  if (auto status = fs::create_directories(directory); !status) return std::nullopt;

  const std::string wrapper = fs::join(directory, "prefix.hpp");
  const std::string wrapper_body = std::format("#include \"{}\"\n", header_path);
  bool wrapper_changed = true;
  if (auto existing = fs::read_text(wrapper); existing.has_value()) {
    wrapper_changed = *existing != wrapper_body;
  }
  if (wrapper_changed) {
    if (auto status = fs::write_text(wrapper, wrapper_body); !status) return std::nullopt;
  }

  const std::string gch = wrapper + ".gch";
  bool stale = wrapper_changed;
  if (!stale) {
    if (const auto gch_time = fs::modified_ns(gch); gch_time.has_value()) {
      const auto header_time = fs::modified_ns(header_path);
      stale = !header_time.has_value() || *header_time > *gch_time;
    } else {
      stale = true;
    }
  }
  if (stale) {
    std::vector<std::string> args;
    for (const auto& flag : flags) args.push_back(flag);
    for (const auto& dir : include_dirs) args.push_back(std::format("-I{}", dir));
    args.push_back(std::format("-I{}", directory));
    args.push_back("-x");
    args.push_back("c++-header");
    args.push_back(wrapper);
    args.push_back("-o");
    args.push_back(gch);
    auto result = process::run(compiler, args);
    if (!result || result->exit_code != 0) {
      const std::string detail =
          result ? std::string(result->stderr_text).substr(0, 400) : result.error().message;
      log::warn("预编译头构建失败（忽略，按常规编译进行）: {}", detail);
      (void)fs::remove_file(gch);
      return std::nullopt;
    }
    log::info("预编译头已生成: {}", gch);
  }
  return PchContext{directory, "prefix.hpp"};
}

[[nodiscard]] auto compile_units(const Manifest& manifest, const BuildOptions& options,
                                 const std::vector<CompileUnit>& units,
                                 const std::vector<std::string>& profile_flags_list,
                                 std::size_t& rebuilt) -> Result<std::size_t> {
  auto compiler = detect_compiler();
  if (!compiler) return forward_error(compiler.error());
  const std::vector<std::string> include_dirs = gather_include_dirs(manifest, options);

  // 顺序语义：工程全局严格集（清单 `flags`）在前，**分档标志在后**——
  // 分档可以针对优化等级做有据可查的例外（如优化档关闭 GCC 误报的 -Wnull-dereference）。
  // **C 与 C++ 各自一套语言标志**：`manifest.flags` 是本工程的 *C++* 严格集
  // （含 `-Wnon-virtual-dtor`/`-Woverloaded-virtual` 这类 C++ 专属告警），给 C 源会直接报
  // 「option is valid for C++ but not for C」。因此 C 源只取 `c_flags` + 档位 + 宏 + 包含路径；
  // 想让自家 C 源也严格，就在 `c_flags` 里显式写 `-Wall -Werror`（vendor 源则另有 `-w`）。
  std::vector<std::string> flags;  // C++ 标志（PCH 建立与 C++ 单元共用，顺序必须一致）
  flags.push_back("-std=c++20");
  for (const auto& item : manifest.flags) flags.push_back(item);
  for (const auto& item : options.extra_flags) flags.push_back(item);
  for (const auto& item : manifest.defines) flags.push_back(std::format("-D{}", item));
  for (const auto& dir : include_dirs) flags.push_back(std::format("-I{}", dir));
  for (const auto& item : profile_flags_list) flags.push_back(item);

  std::vector<std::string> c_flags;
  for (const auto& item : manifest.c_flags) c_flags.push_back(item);
  for (const auto& item : options.extra_flags) c_flags.push_back(item);
  for (const auto& item : manifest.defines) c_flags.push_back(std::format("-D{}", item));
  for (const auto& dir : include_dirs) c_flags.push_back(std::format("-I{}", dir));
  for (const auto& item : profile_flags_list) c_flags.push_back(item);
  // `-x c` 强制按 C 编译：同一个编译器二进制即可，无需第二套工具链
  c_flags.push_back("-x");
  c_flags.push_back("c");

  const std::string build_dir =
      fs::join(manifest.directory, std::format("build/{}", options.profile));
  std::optional<PchContext> pch;
  if (options.use_pch) {
    pch = ensure_pch(*compiler, flags, include_dirs, build_dir,
                     fs::join(manifest.directory, "include/st/pch.hpp"));
  }
  const PchContext* pch_ptr = pch.has_value() ? &*pch : nullptr;

  std::vector<const CompileUnit*> pending;
  pending.reserve(units.size());
  for (const auto& unit : units) {
    if (options.force || needs_rebuild(unit)) pending.push_back(&unit);
  }
  rebuilt = pending.size();
  if (pending.empty()) return std::size_t{0};

  const std::size_t workers = options.jobs != 0 ? options.jobs : hardware_concurrency();
  ThreadPool pool(workers);
  std::mutex error_mutex;
  std::string first_error;
  std::atomic<std::size_t> done{0};

  for (const auto* unit : pending) {
    pool.submit([&, unit, pch_ptr]() {
      if (auto status = fs::ensure_parent(unit->object); !status) {
        const std::scoped_lock lock(error_mutex);
        if (first_error.empty()) first_error = status.error().to_string();
        return;
      }
      std::vector<std::string> args;
      args.push_back(*compiler);
      const bool is_c = unit->lang == SourceLang::C;
      // 第三方源码不套我们的告警集，也不做 sanitizer 插桩（理由见 `strip_sanitizers`）；
      // 因此编译标志按"是否 vendor"分两条路，而不是在原标志上做加法。
      const std::vector<std::string> unit_flags =
          unit->vendor ? strip_sanitizers(is_c ? c_flags : flags) : (is_c ? c_flags : flags);
      for (const auto& flag : unit_flags) args.push_back(flag);
      for (const auto& dir : unit->extra_include_dirs) args.push_back(std::format("-I{}", dir));
      if (unit->vendor) args.push_back("-w");
      // PCH 只服务 C++ 单元；C 源是另一套语言标准，且与 PCH 创建端标志不同，不能吃
      if (pch_ptr != nullptr && !is_c) {
        args.push_back(std::format("-I{}", pch_ptr->directory));
        args.push_back("-include");
        args.push_back(pch_ptr->header);
      }
      args.push_back("-pipe");
      args.push_back("-MMD");
      args.push_back("-MF");
      args.push_back(unit->depfile);
      args.push_back("-c");
      args.push_back(unit->source);
      args.push_back("-o");
      args.push_back(unit->object);
      if (options.verbose) {
        log::info("compile: {} -> {}", fs::file_name(unit->source), fs::file_name(unit->object));
      }
      auto result = process::run(args.front(), std::vector<std::string>(args.begin() + 1, args.end()));
      if (!result) {
        const std::scoped_lock lock(error_mutex);
        if (first_error.empty()) first_error = result.error().to_string();
        return;
      }
      if (result->exit_code != 0) {
        const std::scoped_lock lock(error_mutex);
        if (first_error.empty()) {
          first_error = std::format("编译失败: {}\n{}{}", unit->source, result->stdout_text,
                                    result->stderr_text);
        }
        return;
      }
      ++done;
    });
  }
  pool.wait_idle();

  if (!first_error.empty()) return unexpected(ErrorCode::Invalid, first_error);
  return done.load();
}

[[nodiscard]] auto link(const Manifest& manifest, const BuildOptions& options,
                        const std::vector<CompileUnit>& units, const std::string& output,
                        const std::vector<std::string>& profile_flags_list,
                        const std::vector<std::string>& extra_sources) -> Result<std::string> {
  auto compiler = detect_compiler();
  if (!compiler) return forward_error(compiler.error());
  if (auto status = fs::ensure_parent(output); !status) return forward_error(status.error());

  std::vector<std::string> args;
  for (const auto& flag : profile_flags_list) args.push_back(flag);
  // 链接器：存在 mold/lld 时优先（链接是纯 I/O + 符号解析，lld 通常快 2~4 倍）
  if (const auto lld = process::which("ld.lld"); lld.has_value()) {
    args.push_back("-fuse-ld=lld");
  } else if (const auto mold = process::which("mold"); mold.has_value()) {
    args.push_back("-fuse-ld=mold");
  }
  for (const auto& unit : units) args.push_back(unit.object);
  for (const auto& extra : extra_sources) args.push_back(extra);
  args.push_back("-o");
  args.push_back(output);
  for (const auto& lib : manifest.system_libs) args.push_back(std::format("-l{}", lib));
  for (const auto& lib : manifest.dependency_system) args.push_back(std::format("-l{}", lib));
  for (const auto& lib : default_system_libs()) args.push_back(std::format("-l{}", lib));

  auto result = process::run(*compiler, args, process::Options{.cwd = manifest.directory});
  if (!result) return forward_error(result.error());
  if (result->exit_code != 0) {
    return unexpected(ErrorCode::Invalid,
                      std::format("链接失败: {}\n{}{}", output, result->stdout_text,
                                  result->stderr_text));
  }
  (void)options;
  return output;
}

}  // namespace

auto profile_flags(std::string_view profile) -> Result<std::vector<std::string>> {
  // 说明：优化档（dev/release）关闭 `-Wnull-dereference`——GCC 13 在本项目的
  // 「优化 + 预编译头」组合下会对 libstdc++ 内联代码（vector::insert 等）产生**误报**，
  // 而该警告在 -O0（debug 档）与 sanitizer 档（san，ASan/UBSan 实际检测）下仍然全部保留。
  if (profile == "dev") {
    // 快速迭代档：-O1 兼顾编译速度与运行帧率（日常开发/无头验证用）
    return std::vector<std::string>{"-O1", "-g", "-fno-omit-frame-pointer",
                                    "-Wno-null-dereference"};
  }
  if (profile == "quick") {
    // 最速迭代档：-O0（编译最快；帧率够用即可，适合"改一行看一眼"的循环）
    return std::vector<std::string>{"-O0", "-g"};
  }
  if (profile == "debug") {
    return std::vector<std::string>{"-O0", "-g", "-fno-omit-frame-pointer"};
  }
  if (profile == "release") {
    return std::vector<std::string>{"-O2", "-DNDEBUG", "-Wno-null-dereference"};
  }
  if (profile == "san") {
    // sanitizer 档同样关掉两个"优化 + 系统头"下的 GCC 误报：
    // `-Wmaybe-uninitialized` 会在 libstdc++ `<regex>`（lint 规则用到）里对 `std::function`
    // 的控制块报"可能未初始化"；真实未初始化读由 UBSan/ASan 在运行期抓，防护强度不受影响。
    return std::vector<std::string>{"-O1", "-g", "-fno-omit-frame-pointer",
                                    "-fsanitize=address,undefined", "-Wno-maybe-uninitialized",
                                    "-Wno-null-dereference"};
  }
  return unexpected(ErrorCode::Invalid, std::format("未知构建档位: {}", profile));
}

auto default_system_libs() -> std::vector<std::string> {
#if defined(_WIN32)
  return {"ws2_32"};
#else
  return {"pthread", "dl", "m"};
#endif
}

auto detect_compiler() -> Result<std::string> {
  for (const auto name : {"ST_CXX", "CXX"}) {
    if (const auto value = fs::read_env(name); value.has_value() && !value->empty()) return *value;
  }
  for (const auto name : {"g++", "clang++", "c++"}) {
    if (auto found = process::which(name); found.has_value()) return *found;
  }
  return unexpected(ErrorCode::Unsupported, "未找到 C++ 编译器（g++/clang++/c++）");
}

auto build(const Manifest& manifest, const BuildOptions& options) -> Result<BuildStats> {
  const std::int64_t start_ns = time::now_ns();
  const std::string root = options.root.empty() ? manifest.directory : options.root;
  auto flags = profile_flags(options.profile);
  if (!flags) return forward_error(flags.error());

  auto sources = manifest.source_files();
  if (!sources) return forward_error(sources.error());

  const std::string object_dir = fs::join(root, std::format("build/{}/obj", options.profile));
  const std::string bin_dir = fs::join(root, std::format("build/{}/bin", options.profile));
  if (auto status = fs::create_directories(object_dir); !status) return forward_error(status.error());
  if (auto status = fs::create_directories(bin_dir); !status) return forward_error(status.error());

  // 目标级源排除：应用不需要工具链模块（pkg 等）时跳过其编译（少若干翻译单元）
  std::vector<std::string> library_sources = *sources;
  if (!options.target.empty()) {
    if (const TargetSpec* spec = manifest.find_target(options.target); spec != nullptr) {
      for (const auto& pattern : spec->exclude_sources) {
        const auto before = library_sources.size();
        std::erase_if(library_sources, [&pattern, &manifest](const std::string& path) {
          // 源文件列表是绝对路径：按「相对清单目录」匹配，模式才符合直觉（如 src/pkg/*）
          return fs::match_glob(pattern, fs::relative_to(path, manifest.directory));
        });
        if (before == library_sources.size()) {
          log::warn("排除模式未命中任何源文件: {}", pattern);
        }
      }
    }
  }

  // 第三方源码（vendor/）与库源一起编译：它们不是"可选附加"，而是库的组成部分。
  // 同时把路径集合交给 make_units——这些单元编译时会放宽告警并跳过 PCH。
  std::set<std::string> vendor_paths = collect_vendor(manifest);
  UnitIncludeMap library_includes;
  if (auto status = attach_library_embeds(manifest, options.profile, library_sources, vendor_paths,
                                          library_includes);
      !status) {
    return forward_error(status.error());
  }
  std::vector<std::string> all_sources = library_sources;
  for (const auto& path : vendor_paths) all_sources.push_back(path);
  std::ranges::sort(all_sources);
  all_sources.erase(std::unique(all_sources.begin(), all_sources.end()), all_sources.end());

  std::vector<CompileUnit> units =
      make_units(manifest, all_sources, object_dir, vendor_paths, library_includes);
  std::size_t rebuilt = 0;
  const std::int64_t compile_start = time::now_ns();
  auto compiled = compile_units(manifest, options, units, *flags, rebuilt);
  if (!compiled) return forward_error(compiled.error());
  const std::int64_t compile_ms = (time::now_ns() - compile_start) / 1'000'000;

  BuildStats stats;
  stats.units_total = units.size();
  stats.units_rebuilt = rebuilt;
  stats.units_cached = units.size() - rebuilt;
  stats.compile_ms = compile_ms;
  stats.workers = options.jobs != 0 ? options.jobs : hardware_concurrency();
  stats.pch_used = options.use_pch;
  stats.elapsed_ms = (time::now_ns() - start_ns) / 1'000'000;

  if (options.target.empty()) return stats;

  const TargetSpec* target = manifest.find_target(options.target);
  if (target == nullptr) {
    return unexpected(ErrorCode::NotFound, std::format("目标不存在: {}", options.target));
  }
  std::vector<CompileUnit> target_units;
  if (!target->sources.empty() || !target->embed.empty()) {
    std::vector<std::string> target_sources;
    for (const auto& pattern : target->sources) {
      auto matches = fs::expand_glob(manifest.directory, pattern);
      if (!matches) return forward_error(matches.error());
      for (const auto& item : *matches) target_sources.push_back(item);
    }
    // 编译期嵌入：生成字节数组与声明头，并把它们纳入本目标的编译单元。
    // 生成的源与 vendor 运行时都按"第三方"对待（`-w`、不插桩）——它们不是本工程的手写代码。
    UnitIncludeMap unit_includes;
    std::set<std::string> target_vendor = vendor_paths;  // 本目标的第三方集合（含生成的嵌入源码）
    if (!target->embed.empty()) {
      // 运行时实现只由库级嵌入提供（若库没有嵌入，则由本目标提供）——避免同链接里出现两份全局态
      auto embeds = generate_embeds(manifest, options.target, options.profile,
                                    /*with_runtime=*/manifest.embed.empty());
      if (!embeds) return forward_error(embeds.error());
      // **目标自身的源也要能 `#include "battery/embed.hpp"`**——只给生成源加包含目录是不够的
      // （踩过：目标源报 "battery/embed.hpp: No such file or directory"）。
      //
      // 键必须与 `make_units` 里的 `unit.source` 同形：`expand_glob` 返回的是**相对路径**，
      // 而 `make_units` 会拼成绝对路径后再查表——不归一就会静默查不到（曾表现为
      // "包含目录没传"，实际是键不匹配）。
      for (const auto& existing : target_sources) {
        const std::string key =
            fs::is_absolute(existing) ? existing : fs::join(manifest.directory, existing);
        unit_includes[key].push_back(embeds->include_dir);
      }
      for (const auto& generated : embeds->sources) {
        target_sources.push_back(generated);
        target_vendor.insert(generated);
        unit_includes[generated].push_back(embeds->include_dir);
      }
    }
    std::ranges::sort(target_sources);
    target_sources.erase(std::unique(target_sources.begin(), target_sources.end()),
                         target_sources.end());
    target_units = make_units(manifest, target_sources, object_dir, target_vendor, unit_includes);
    std::size_t target_rebuilt = 0;
    auto target_compiled = compile_units(manifest, options, target_units, *flags, target_rebuilt);
    if (!target_compiled) return forward_error(target_compiled.error());
    stats.units_total += target_units.size();
    stats.units_rebuilt += target_rebuilt;
    stats.units_cached = stats.units_total - stats.units_rebuilt;
  }

  const std::string output = fs::join(bin_dir, target->name);
  std::vector<CompileUnit> link_units = units;
  link_units.insert(link_units.end(), target_units.begin(), target_units.end());

  // 链接跳过：0 个单元重编 + 产物已存在且比全部目标文件新 → 不重链（无改动构建近乎瞬时）
  const bool up_to_date = [&]() {
    if (options.force || rebuilt != 0 || stats.units_rebuilt != 0) return false;
    const auto output_time = fs::modified_ns(output);
    if (!output_time) return false;
    for (const auto& unit : link_units) {
      const auto object_time = fs::modified_ns(unit.object);
      if (!object_time || *object_time > *output_time) return false;
    }
    return true;
  }();
  if (up_to_date) {
    stats.artifact = output;
    stats.elapsed_ms = (time::now_ns() - start_ns) / 1'000'000;
    return stats;
  }

  auto linked = link(manifest, options, link_units, output, *flags, {});
  if (!linked) return forward_error(linked.error());
  stats.linked = true;
  stats.artifact = output;
  stats.elapsed_ms = (time::now_ns() - start_ns) / 1'000'000;
  return stats;
}

auto run_tests(const Manifest& manifest, const BuildOptions& options, std::string_view filter)
    -> Result<int> {
  const std::string root = options.root.empty() ? manifest.directory : options.root;
  auto flags = profile_flags(options.profile);
  if (!flags) return forward_error(flags.error());

  auto sources = manifest.source_files();
  if (!sources) return forward_error(sources.error());
  auto tests = manifest.test_files();
  if (!tests) return forward_error(tests.error());

  const std::string object_dir = fs::join(root, std::format("build/{}/obj", options.profile));
  const std::string bin_dir = fs::join(root, std::format("build/{}/bin", options.profile));
  if (auto status = fs::create_directories(object_dir); !status) return forward_error(status.error());
  if (auto status = fs::create_directories(bin_dir); !status) return forward_error(status.error());

  // 测试可执行 = 库源（排除框架入口）+ 测试框架 + 用例
  std::vector<std::string> library_sources;
  for (const auto& source : *sources) {
    if (source.starts_with("src/test/") || source.starts_with("tools/")) continue;
    library_sources.push_back(source);
  }
  std::vector<std::string> runner_sources;
  auto runner = fs::expand_glob(manifest.directory, "src/test/*.cpp");
  if (runner) {
    for (const auto& item : *runner) runner_sources.push_back(item);
  }
  std::set<std::string> vendor_paths = collect_vendor(manifest);
  UnitIncludeMap test_includes;
  // 库级嵌入先接上（它会给 `library_sources` 追加生成源），再拼最终单元列表
  if (auto status = attach_library_embeds(manifest, options.profile, library_sources, vendor_paths,
                                          test_includes);
      !status) {
    return forward_error(status.error());
  }
  std::vector<std::string> all = library_sources;
  for (const auto& item : *tests) all.push_back(item);
  for (const auto& item : runner_sources) all.push_back(item);
  for (const auto& path : vendor_paths) all.push_back(path);
  std::ranges::sort(all);
  all.erase(std::unique(all.begin(), all.end()), all.end());
  std::vector<CompileUnit> units = make_units(manifest, all, object_dir, vendor_paths, test_includes);
  std::size_t rebuilt = 0;
  auto compiled = compile_units(manifest, options, units, *flags, rebuilt);
  if (!compiled) return forward_error(compiled.error());
  log::info("测试构建：{} 单元（重编 {}）", units.size(), rebuilt);

  const std::string output = fs::join(bin_dir, "st_tests");
  auto linked = link(manifest, options, units, output, *flags, {});
  if (!linked) return forward_error(linked.error());

  std::vector<std::string> args;
  if (!filter.empty()) args.push_back(std::string(filter));
  auto result = process::run(output, args, process::Options{.capture_output = false});
  if (!result) return forward_error(result.error());
  return result->exit_code;
}

}  // namespace st::pkg
