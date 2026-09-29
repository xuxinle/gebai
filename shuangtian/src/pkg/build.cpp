#include "st/pkg/build.hpp"
#include "st/pkg/compiler.hpp"
#include "st/pkg/embed.hpp"
#include "st/pkg/memory.hpp"
#include "st/pkg/framework.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <semaphore>
#include <set>
#include <format>
#include <mutex>
#include <unordered_set>

#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/thread_pool.hpp"
#include "st/core/time.hpp"

namespace st::pkg {
auto detect_compiler(std::string_view override_compiler) -> Result<std::string> {
  if (!override_compiler.empty()) return std::string(override_compiler);
  for (const auto name : {"ST_CXX", "CXX"}) {
    if (const auto value = fs::read_env(name); value.has_value() && !value->empty()) return *value;
  }
#if defined(_WIN32)
  // Windows 上**首选 MSVC**（本框架在 Windows 的默认工具链）：
  // 目标机器上通常只装了 VS 的 C++ 工具集（没有 g++/clang++），而 `cl.exe` **不在 PATH 里**
  // ——必须先跑 vcvars 才可见，故走 VS 官方查询而不是 `which`（见 `pkg/compiler.hpp`）。
  if (auto msvc = find_msvc_toolchain(); msvc.has_value()) return msvc->cl;
#endif
  for (const auto name : {"g++", "clang++", "c++"}) {
    if (auto found = process::which(name); found.has_value()) return *found;
  }
#if defined(_WIN32)
  return unexpected(ErrorCode::Unsupported,
                    "未找到 C++ 编译器：安装 Visual Studio 的「使用 C++ 的桌面开发」工作负载，"
                    "或用 ST_VCVARS/ST_CL 指定已有的 MSVC 工具集");
#else
  return unexpected(ErrorCode::Unsupported, "未找到 C++ 编译器（g++/clang++/c++）");
#endif
}

namespace {

/// 源文件语言（决定编译器标志：`.c` 必须按 C 编译，不能用 `-std=c++20`）。
enum class SourceLang : std::uint8_t { Cxx, C };

struct CompileUnit {
  std::string source{};   ///< 绝对路径
  std::string object{};   ///< 绝对路径
  std::string depfile{};  ///< `-MMD` 产出的头依赖清单（`.d`）
  SourceLang lang{SourceLang::Cxx};
  bool third_party{false};     ///< 第三方源码（放宽告警、不进 PCH）
  /// 框架单元（来自被引用的框架）。这类单元的编译命令**必须与引用方工程无关**：
  /// 不含工程的 `-I`/`-D`，也不用工程的 PCH——否则共享对象缓存的键会随工程变化，缓存永不命中。
  /// 语义上也是对的：框架源只应看到框架自己的头，不该被引用方的同名头影响。
  bool framework_unit{false};
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

/// 共享对象缓存。
///
/// **为什么需要**：独立工程以"源码级依赖"引用框架（无需安装、始终同版本、交叉编译天然生效），
/// 代价是每个工程首次要把框架的 63 个源文件编一遍。缓存把这份代价摊掉：
/// 第二个工程起，框架部分直接从缓存取对象，只剩工程自己的源要编。
///
/// **键怎么取**：编译命令里除 `-o`/`-MF` 之外的全部内容（编译器、标志、宏、包含目录、源路径）。
/// 因此"改变产物的任何因素"变了键就变；而 `-o`/`-MF` 是**工程相关**的输出路径，必须排除，
/// 否则键随工程变、缓存永不命中。框架源的编译命令被刻意保持与工程无关
/// （不套工程的 `-I`，也不用工程的 PCH），这样键才跨工程稳定。
///
/// **有效性判定**：复用增量构建那一套——缓存对象的 `.d` 里记着全部依赖，
/// 只要没有依赖比缓存对象新，缓存对象就是最新的。**不引入第二套判定逻辑**，
/// 否则"增量构建"与"缓存"迟早会在边界条件上给出不同答案。
struct ObjectCache {
  std::string root{};     ///< 缓存根（空 = 未启用）
  std::size_t hits{0};
  std::size_t stores{0};
  [[nodiscard]] auto enabled() const noexcept -> bool { return !root.empty(); }
};

/// 解析缓存根：`ST_HOME` 或 `~/.shuangtian` 下的 `cache/objects`；`ST_NO_CACHE=1` 关闭。
[[nodiscard]] auto resolve_object_cache() -> ObjectCache {
  ObjectCache cache;
  if (const auto disabled = fs::read_env("ST_NO_CACHE"); disabled.has_value() && *disabled == "1") {
    return cache;
  }
  std::string home;
  if (const auto env = fs::read_env("ST_HOME"); env.has_value() && !env->empty()) {
    home = *env;
  } else {
    home = fs::join(fs::home_dir(), ".shuangtian");
  }
  if (home.empty()) return cache;
  cache.root = fs::join(home, "cache/objects");
  return cache;
}

/// 尝试从缓存恢复对象（命中即把 `.o` 与 `.d` 复制到位）。`-MMD` 的 `.d` 一并带回来，
/// 后续增量判定才有依赖清单可用。
[[nodiscard]] auto object_cache_restore(const ObjectCache& cache, std::string_view key,
                                        const CompileUnit& unit) -> bool {
  if (!cache.enabled()) return false;
  const std::string directory = fs::join(cache.root, key);
  const std::string cached_object = fs::join(directory, "unit.o");
  const std::string cached_depfile = fs::join(directory, "unit.d");
  if (!fs::is_regular_file(cached_object) || !fs::is_regular_file(cached_depfile)) return false;
  const auto cached_time = fs::modified_ns(cached_object);
  if (!cached_time) return false;
  // 依赖新鲜度：源文件与 `.d` 里的每个依赖都不得比缓存对象新
  const auto source_time = fs::modified_ns(unit.source);
  if (!source_time || *source_time > *cached_time) return false;
  for (const auto& dep : parse_depfile(cached_depfile)) {
    const auto dep_time = fs::modified_ns(dep);
    if (dep_time && *dep_time > *cached_time) return false;
    if (!dep_time && !fs::exists(dep)) return false;
  }
  if (auto status = fs::copy_file(cached_object, unit.object); !status) return false;
  if (auto status = fs::copy_file(cached_depfile, unit.depfile); !status) return false;
  return true;
}

/// 编译成功后写入缓存（失败不影响构建：缓存是加速手段，不是正确性前提）。
auto object_cache_store(const ObjectCache& cache, std::string_view key, const CompileUnit& unit,
                        std::atomic<std::size_t>& stores) -> void {
  if (!cache.enabled()) return;
  if (!fs::is_regular_file(unit.object) || !fs::is_regular_file(unit.depfile)) return;
  const std::string directory = fs::join(cache.root, key);
  if (auto status = fs::create_directories(directory); !status) return;
  if (auto status = fs::copy_file(unit.object, fs::join(directory, "unit.o")); !status) return;
  if (auto status = fs::copy_file(unit.depfile, fs::join(directory, "unit.d")); !status) return;
  stores.fetch_add(1);
}

/// 单元 → 专属包含目录（编译期嵌入用；未命中即无额外目录）。
using UnitIncludeMap = std::map<std::string, std::vector<std::string>>;

[[nodiscard]] auto make_units(const Manifest& manifest, const std::vector<std::string>& sources,
                              const std::string& object_dir,
                              const std::set<std::string>& third_party_paths,
                              const UnitIncludeMap& unit_includes = {},
                              const std::set<std::string>& framework_paths = {})
    -> std::vector<CompileUnit> {
  std::vector<CompileUnit> units;
  units.reserve(sources.size());
  for (const auto& relative : sources) {
    CompileUnit unit;
    unit.source = fs::is_absolute(relative) ? relative : fs::join(manifest.directory, relative);
    unit.object = fs::join(object_dir, sanitize_name(relative).append(".o"));
    unit.depfile = unit.object + ".d";
    unit.lang = language_of(unit.source);
    unit.third_party = third_party_paths.contains(unit.source);
    // 框架单元：跳过 PCH，让它的编译命令**与引用方工程无关**。
    // 这样对象缓存键才跨工程稳定（否则 PCH 路径里带着工程目录 → 每个工程都算新键）。
    unit.framework_unit = framework_paths.contains(unit.source);
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
/// 把一次嵌入生成的结果接进构建输入：生成物加入编译、包含目录**只**下发到指定单元。
///
/// `include_for` 是"哪些单元需要这个嵌入头"：**必须精确**——一个编译单元的命令行上
/// 同时出现两个 `battery/embed.hpp` 时，先命中的赢，另一方的资源会 `static_assert` 失败。
/// 因此框架的嵌入只给框架单元，工程的嵌入只给工程单元（详见 `embed.hpp` 的说明）。
[[nodiscard]] auto attach_embeds(const EmbedRequest& request,
                                 const std::vector<std::string>& include_for,
                                 std::vector<std::string>& built_sources,
                                 std::set<std::string>& third_party_paths,
                                 UnitIncludeMap& unit_includes) -> Status {
  if (request.contributions.empty()) return ok();
  bool has_patterns = false;
  for (const auto& contribution : request.contributions) {
    if (!contribution.patterns.empty()) has_patterns = true;
  }
  if (!has_patterns) return ok();

  auto embeds = generate_embeds(request);
  if (!embeds) return forward_error(embeds.error());
  if (embeds->include_dir.empty()) return ok();

  for (const auto& source : include_for) {
    unit_includes[fs::is_absolute(source) ? source : fs::join(request.work_directory, source)]
        .push_back(embeds->include_dir);
  }
  for (const auto& generated : embeds->sources) {
    built_sources.push_back(generated);
    third_party_paths.insert(generated);
    unit_includes[generated].push_back(embeds->include_dir);
  }
  return ok();
}

/// 构造一次库级嵌入请求（单个贡献方）。
[[nodiscard]] auto library_embed_request(std::string_view owner_name, std::string_view owner_directory,
                                        const std::vector<std::string>& patterns,
                                        std::string_view build_subdir,
                                        std::string_view template_directory,
                                        std::string_view runtime_source, bool with_runtime)
    -> EmbedRequest {
  EmbedRequest request;
  request.work_directory = std::string(owner_directory);
  request.template_directory = std::string(template_directory);
  request.runtime_source = std::string(runtime_source);
  request.build_subdir = std::string(build_subdir);
  request.scope = "lib";
  request.with_runtime = with_runtime;
  if (!patterns.empty()) {
    EmbedContribution contribution;
    contribution.base_directory = std::string(owner_directory);
    contribution.patterns = patterns;
    contribution.identifier_prefix = std::string(owner_name);
    contribution.origin = std::format("{} {}", owner_name, owner_directory);
    request.contributions.push_back(std::move(contribution));
  }
  return request;
}

/// 展开第三方源码清单（失败按空集处理：清单没声明第三方源码也能构建）。
[[nodiscard]] auto collect_third_party(const Manifest& manifest) -> std::set<std::string> {
  auto files = manifest.third_party_files();
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
/// 目标平台的默认系统库。
///
/// **参数是目标平台名，不是宿主的 `_WIN32`**：交叉编译时宿主与目标不同，
/// 用 `#if defined(_WIN32)` 判的是宿主（Linux 上恒为假），必然给 Windows 目标链上
/// `-lpthread -ldl -lm`——mingw 根本没有这些库，链接必然失败。
[[nodiscard]] auto default_system_libs(std::string_view platform) -> std::vector<std::string> {
  // Windows 目标的默认库：`ws2_32`（winsock）+ `user32`/`gdi32`（Win32 窗口后端）
  // + `shell32`（`CommandLineToArgvW`：`ST_MAIN` 拿命令行参数用它）。
  // 少一个就是链接期"无法解析的外部符号"，而报错位置离真正原因（少了哪个库）很远。
  if (platform == "windows") return {"ws2_32", "user32", "gdi32", "shell32"};
  return {"pthread", "dl", "m"};
}

/// 宿主机平台名（编译期常量；仅在未指定工具链时使用）。
[[nodiscard]] auto host_platform() -> std::string_view {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "darwin";
#else
  return "linux";
#endif
}

/// 解析后的工具链：清单描述 → 构建时可直接使用的一组值。
struct ResolvedToolchain {
  std::string compiler{};           ///< C++ 编译器（清单里的写法；空=自动探测本机）
  std::string c_compiler{};         ///< C 编译器（空=用 compiler + `-x c`）
  std::string compiler_path{};      ///< 解析后的编译器路径
  CompilerKind kind{CompilerKind::Gcc};  ///< 编译器族（标志拼法/依赖产出/链接方式由它决定）
  /// 编译/链接**子进程**需要的环境变量（MSVC 的 `INCLUDE`/`LIB`/`PATH`；GCC 系为空）。
  /// 显式传递而不是改自身进程环境：同一台机器上可能同时存在多套工具集，
  /// 而"构建过程中修改自己的环境"会让（并行）编译、链接、测试跑在不同环境里。
  std::map<std::string, std::string> env{};
  std::string platform{};           ///< 目标平台
  std::string executable_suffix{};  ///< 产物后缀（Windows 为 `.exe`）
  std::vector<std::string> system_libs{};
  std::vector<std::string> defines{};
  std::vector<std::string> extra_flags{};
  std::string directory_tag{};      ///< 目录隔离标记（空=本机档）
  [[nodiscard]] auto cross() const noexcept -> bool { return !directory_tag.empty(); }
};

/// 档位目录名：`dev` 或 `dev-mingw`。
///
/// **交叉编译必须与本地档隔离目录**：对象文件、PCH、嵌入生成物都不能与本地档混用
/// ——PCH 是按"编译器 + 目标"生成的，混用会得到难以理解的编译错误。
[[nodiscard]] auto profile_directory(std::string_view profile, const ResolvedToolchain& toolchain)
    -> std::string {
  if (!toolchain.cross()) return std::string(profile);
  return std::format("{}-{}", profile, toolchain.directory_tag);
}

/// 解析工具链（空名 = 本机默认，行为与"没有工具链概念"时完全一致）。
[[nodiscard]] auto resolve_toolchain(const Manifest& manifest, std::string_view name)
    -> Result<ResolvedToolchain> {
  ResolvedToolchain resolved;
  if (name.empty()) {
    resolved.platform = std::string(host_platform());
#if defined(_WIN32)
    resolved.executable_suffix = ".exe";
#endif
  } else {
    const ToolchainSpec* spec = manifest.find_toolchain(name);
    if (spec == nullptr) {
      return unexpected(ErrorCode::Invalid,
                        std::format("清单未定义工具链 '{}'（在 st.pkg 的 toolchains 段声明）", name));
    }
    resolved.compiler = spec->compiler;
    resolved.c_compiler = spec->c_compiler;
    resolved.platform = spec->platform.empty() ? std::string("none") : spec->platform;
    resolved.executable_suffix = spec->executable_suffix;
    resolved.system_libs = spec->system_libs;
    resolved.defines = spec->defines;
    resolved.extra_flags = spec->extra_flags;
    resolved.directory_tag = std::string(name);
    // 工具链必须真的存在：错误要早于"编了一半才发现找不到编译器"
    if (!process::which(resolved.compiler).has_value() && !fs::is_absolute(resolved.compiler)) {
      return unexpected(ErrorCode::NotFound,
                        std::format("工具链 '{}' 的编译器不可用: {}（请先安装）", name, resolved.compiler));
    }
  }
  // 编译器解析只做一次：编译与链接共用同一个路径与族（重复探测既慢、又可能给出不同答案）
  auto compiler = detect_compiler(resolved.compiler);
  if (!compiler) return forward_error(compiler.error());
  resolved.compiler_path = *compiler;
  resolved.kind = compiler_kind_of(resolved.compiler_path);
  // MSVC 的头/库/工具路径全部来自环境变量：不注入就是"找不到任何头文件"。
  // 探测失败要在**构建开始前**报错，而不是让几十个编译进程各自失败一遍。
  auto environment = compiler_environment(resolved.kind);
  if (!environment) return forward_error(environment.error());
  resolved.env = std::move(*environment);
  return resolved;
}

/// 框架自己的编译标志集（框架单元专用：与引用方工程无关）。
struct FrameworkFlags {
  std::vector<std::string> flags{};    ///< C++：框架的 flags + defines + include_dirs
  std::vector<std::string> c_flags{};  ///< C：同上但用 c_flags
};

/// 由框架清单构造框架标志集（框架单元专用：与引用方工程无关）。
///
/// MSVC 的处理是**先按 GCC 风格拼好、再整体翻译**：清单（含框架自己的 `st.pkg`）只写一份
/// 跨平台标志，翻译规则集中在 `pkg/compiler.hpp`——否则"哪个平台漏了哪个开关"会永久发散。
[[nodiscard]] auto make_framework_flags(const Manifest& framework_manifest,
                                        const std::vector<std::string>& framework_include_dirs,
                                        const std::vector<std::string>& profile_flags_list,
                                        const ResolvedToolchain& toolchain) -> FrameworkFlags {
  FrameworkFlags out;
  out.flags.push_back("-std=c++20");
  for (const auto& item : framework_manifest.flags) out.flags.push_back(item);
  for (const auto& item : framework_manifest.defines) {
    out.flags.push_back(std::format("-D{}", item));
  }
  for (const auto& dir : framework_include_dirs) out.flags.push_back(std::format("-I{}", dir));
  for (const auto& item : toolchain.defines) out.flags.push_back(std::format("-D{}", item));
  for (const auto& item : profile_flags_list) out.flags.push_back(item);

  out.c_flags = framework_manifest.c_flags;
  for (const auto& item : framework_manifest.defines) {
    out.c_flags.push_back(std::format("-D{}", item));
  }
  for (const auto& dir : framework_include_dirs) out.c_flags.push_back(std::format("-I{}", dir));
  for (const auto& item : toolchain.defines) out.c_flags.push_back(std::format("-D{}", item));
  for (const auto& item : profile_flags_list) out.c_flags.push_back(item);
  out.c_flags.push_back("-x");
  out.c_flags.push_back("c");

  if (toolchain.kind == CompilerKind::Msvc) {
    out.flags = translate_flags(toolchain.kind, out.flags, nullptr);
    out.c_flags = translate_flags(toolchain.kind, out.c_flags, nullptr);
  }
  for (const auto language : {SourceLanguage::Cxx, SourceLanguage::C}) {
    const auto pins = dialect_flags(toolchain.kind, language);
    auto& target = language == SourceLanguage::Cxx ? out.flags : out.c_flags;
    target.insert(target.end(), pins.begin(), pins.end());
  }
  return out;
}

[[nodiscard]] auto compile_units(const Manifest& manifest, const BuildOptions& options,
                                 const std::vector<CompileUnit>& units,
                                 const std::vector<std::string>& profile_flags_list,
                                 const ResolvedToolchain& toolchain,
                                 const FrameworkFlags* framework_flags, std::size_t& rebuilt)
    -> Result<std::size_t> {
  // 编译器在工具链解析阶段已经定下（路径 + 族 + 环境），这里只做非空校验：
  // 重复探测会多花时间，还可能因为环境变化给出与链接阶段不同的答案。
  if (toolchain.compiler_path.empty()) {
    return unexpected(ErrorCode::Unsupported, "未解析出 C++ 编译器（见 resolve_toolchain）");
  }
  const std::string& compiler_path = toolchain.compiler_path;
  const std::vector<std::string> include_dirs = gather_include_dirs(manifest, options);
  // 目标专属宏（如 `_WIN32_WINNT=0x0601`）：只对目标生效，不污染本机档
  const std::vector<std::string>& target_defines = toolchain.defines;

  // 顺序语义：工程全局严格集（清单 `flags`）在前，**分档标志在后**——
  // 分档可以针对优化等级做有据可查的例外（如优化档关闭 GCC 误报的 -Wnull-dereference）。
  // **C 与 C++ 各自一套语言标志**：`manifest.flags` 是本工程的 *C++* 严格集
  // （含 `-Wnon-virtual-dtor`/`-Woverloaded-virtual` 这类 C++ 专属告警），给 C 源会直接报
  // 「option is valid for C++ but not for C」。因此 C 源只取 `c_flags` + 档位 + 宏 + 包含路径；
  // 想让自家 C 源也严格，就在 `c_flags` 里显式写 `-Wall -Werror`（第三方源则另有 `-w`）。
  std::vector<std::string> flags;  // C++ 标志（PCH 建立与 C++ 单元共用，顺序必须一致）
  flags.push_back("-std=c++20");
  for (const auto& item : manifest.flags) flags.push_back(item);
  for (const auto& item : options.extra_flags) flags.push_back(item);
  for (const auto& item : manifest.defines) flags.push_back(std::format("-D{}", item));
  for (const auto& item : target_defines) flags.push_back(std::format("-D{}", item));
  for (const auto& dir : include_dirs) flags.push_back(std::format("-I{}", dir));
  for (const auto& item : profile_flags_list) flags.push_back(item);

  std::vector<std::string> c_flags;
  for (const auto& item : manifest.c_flags) c_flags.push_back(item);
  for (const auto& item : options.extra_flags) c_flags.push_back(item);
  for (const auto& item : manifest.defines) c_flags.push_back(std::format("-D{}", item));
  for (const auto& item : target_defines) c_flags.push_back(std::format("-D{}", item));
  for (const auto& dir : include_dirs) c_flags.push_back(std::format("-I{}", dir));
  for (const auto& item : profile_flags_list) c_flags.push_back(item);
  // `-x c` 强制按 C 编译：同一个编译器二进制即可，无需第二套工具链
  // （MSVC 由扩展名定语言，翻译层会把这两个参数丢掉，见 `translate_flags`）
  c_flags.push_back("-x");
  c_flags.push_back("c");

  // 编译器族翻译：清单恒为跨平台写法，差异在这里落到具体编译器。
  // 必须在**缓存键与命令行之前**完成：两者必须来自同一份标志。
  if (toolchain.kind == CompilerKind::Msvc) {
    std::vector<std::string> dropped;
    flags = translate_flags(toolchain.kind, flags, &dropped);
    c_flags = translate_flags(toolchain.kind, c_flags, nullptr);
    if (!dropped.empty()) {
      // 不静默：这些开关在 MSVC 上没有等价物，"以为还在检查"比"没检查"更危险
      std::string listed;
      for (const auto& item : dropped) listed.append(" ").append(item);
      log::info("MSVC 下无等价标志（已丢弃 {} 项）:{}", dropped.size(), listed);
    }
  }
  const auto add_dialect_flags = [&toolchain](std::vector<std::string>& list, SourceLanguage language) {
    const auto pins = dialect_flags(toolchain.kind, language);
    list.insert(list.end(), pins.begin(), pins.end());
  };
  add_dialect_flags(flags, SourceLanguage::Cxx);
  add_dialect_flags(c_flags, SourceLanguage::C);

  const std::string build_dir = fs::join(
      manifest.directory,
      std::format("build/{}", profile_directory(options.profile, toolchain)));
  // 预编译头是**框架的**（`include/st/pch.hpp`）：独立工程自己没有这个头，
  // 因此这里按"框架目录优先、其次工程目录"解析；都找不到就跳过 PCH（不是错误）。
  std::optional<PchContext> pch;
  if (options.use_pch) {
    const std::string pch_header = [&]() {
      const std::vector<std::string> candidates = {
          fs::join(manifest.directory, "include/st/pch.hpp"),
          options.extra_include_dirs.empty() ? std::string{}
                                             : fs::join(options.extra_include_dirs.front(),
                                                        "st/pch.hpp"),
      };
      for (const auto& candidate : candidates) {
        if (!candidate.empty() && fs::is_regular_file(candidate)) return candidate;
      }
      return std::string{};
    }();
    if (!pch_header.empty() && toolchain.kind != CompilerKind::Msvc) {
      pch = ensure_pch(compiler_path, flags, include_dirs, build_dir, pch_header);
    }
  }
  const PchContext* pch_ptr = pch.has_value() ? &*pch : nullptr;

  const ObjectCache object_cache = resolve_object_cache();

  // 每个单元的**最终编译标志**（第三方放宽 / 框架单元用框架标志集 / C 与 C++ 分派）。
  // 先统一算好：缓存键与真正的编译命令必须来自同一份标志，否则"键一致但命令不同"必然出问题。
  const auto unit_flags_for = [&](const CompileUnit& unit) -> std::vector<std::string> {
    const bool is_c = unit.lang == SourceLang::C;
    std::vector<std::string> chosen;
    if (unit.framework_unit && framework_flags != nullptr) {
      chosen = is_c ? framework_flags->c_flags : framework_flags->flags;
    } else {
      chosen = is_c ? c_flags : flags;
    }
    if (unit.third_party) chosen = strip_sanitizers(chosen);
    return chosen;
  };
  const auto cache_key_for = [&](const CompileUnit& unit) -> std::string {
    std::string material(compiler_path);
    // 编译器族入键：同一路径下的不同族（如 `clang-cl` 与 `clang++`）产物不兼容
    material.append("\n<").append(compiler_kind_name(toolchain.kind)).append(">");
    // **依赖清单格式版本**入键：缓存项的有效性靠它自己的 `.d` 判定，一旦依赖产出方式变了
    // （如 MSVC 从 `/showIncludes` 文本改为 `/sourceDependencies` JSON），旧条目的 `.d`
    // 可能不完整——“头文件改了不重编”这种静默降级会把错对象缓存永久钉住。
    // 改动依赖产出方式时**必须**升这个版本号。
    material.append("\n<deps:2>");
    for (const auto& flag : unit_flags_for(unit)) material.append("\n").append(flag);
    material.append("\n").append(unit.source);
    if (unit.third_party) material.append("\n-w");
    if (unit.framework_unit) material.append("\n<framework>");
    for (const auto& dir : unit.extra_include_dirs) material.append("\n-I").append(dir);
    return st::hash::fnv1a64_hex(material);
  };

  std::size_t cache_hits = 0;
  std::atomic<std::size_t> cache_stores{0};
  std::vector<const CompileUnit*> pending;
  pending.reserve(units.size());
  for (const auto& unit : units) {
    if (!options.force && !needs_rebuild(unit)) continue;
    // 工程内已过期 → 先问共享缓存：命中就不必真编译（跨工程复用框架对象的主要路径）
    if (!options.force && object_cache_restore(object_cache, cache_key_for(unit), unit)) {
      ++cache_hits;
      continue;
    }
    pending.push_back(&unit);
  }
  rebuilt = pending.size();
  if (pending.empty()) return std::size_t{0};

  // 并发方案：默认**按内存预算**推导（而不是按核数）——编译是内存密集型的，
  // `nproc` 为 28 的机器满并发就是 10–20 GB 瞬时占用，在 cgroup 限 8 GiB 的容器里必被 OOM 杀掉。
  const ConcurrencyPlan plan = plan_concurrency(options.jobs, options.jobs_large,
                                                options.max_memory_mb, options.profile,
                                                hardware_concurrency());
  const std::size_t workers = plan.jobs;
  log::info("并行编译 {} 路（{}），超大单元上限 {}", plan.jobs, plan.reason, plan.jobs_large);
  // 超大单元闸门：超阈值源文件同时最多 `jobs_large` 个在编（默认 1），
  // 避免"几个大块头叠在一起"这种最坏情形（即便它们的峰值与体积不成正比，串行化也钉住上界）
  const auto large_gate_limit = static_cast<std::ptrdiff_t>(plan.jobs_large);
  std::counting_semaphore<1024> large_gate(large_gate_limit);
  ThreadPool pool(workers);
  std::mutex error_mutex;
  std::string first_error;
  std::atomic<std::size_t> done{0};
  std::atomic<bool> dep_warning{false};  ///< 依赖清单降级的告警只打一次（不是每个单元各打一次）

  // 超大单元排到最后提交：小单元先跑满并发，大块头收尾时独占闸门
  std::stable_partition(pending.begin(), pending.end(),
                        [](const CompileUnit* unit) { return !is_large_unit(unit->source); });

  for (const auto* unit : pending) {
    pool.submit([&, unit, pch_ptr]() {
      // 超大单元过闸门（RAII：任何退出路径都释放，否则后续大单元会永久排队）
      const bool large = is_large_unit(unit->source);
      struct GateGuard {
        std::counting_semaphore<1024>* gate{nullptr};
        explicit GateGuard(std::counting_semaphore<1024>* target) : gate(target) {
          if (gate != nullptr) gate->acquire();
        }
        GateGuard(const GateGuard&) = delete;
        auto operator=(const GateGuard&) -> GateGuard& = delete;
        ~GateGuard() {
          if (gate != nullptr) gate->release();
        }
      };
      const GateGuard gate_guard(large ? &large_gate : nullptr);
      if (auto status = fs::ensure_parent(unit->object); !status) {
        const std::scoped_lock lock(error_mutex);
        if (first_error.empty()) first_error = status.error().to_string();
        return;
      }
      std::vector<std::string> args;
      args.push_back(compiler_path);
      const bool is_c = unit->lang == SourceLang::C;
      // 第三方源码不套我们的告警集，也不做 sanitizer 插桩（理由见 `strip_sanitizers`）；
      // 框架单元用**框架自己的**标志集（与引用方工程无关，缓存才能跨工程命中）。
      const std::vector<std::string> unit_flags = unit_flags_for(*unit);
      for (const auto& flag : unit_flags) args.push_back(flag);
      for (const auto& dir : unit->extra_include_dirs) {
        args.push_back(toolchain.kind == CompilerKind::Msvc ? std::format("/I{}", dir)
                                                              : std::format("-I{}", dir));
      }
      if (unit->third_party) args.push_back("-w");  // MSVC 的"关全部告警"恰好也是同一拼法
      // PCH 只服务 C++ 单元；C 源是另一套语言标准，且与 PCH 创建端标志不同，不能吃
      if (pch_ptr != nullptr && !is_c && !unit->framework_unit) {
        args.push_back(std::format("-I{}", pch_ptr->directory));
        args.push_back("-include");
        args.push_back(pch_ptr->header);
      }
      // **先写临时文件，成功再改名到位**：编译被中断（OOM 杀掉编译器、磁盘写满）时
      // 产物位置不会留下半截 `.o`——它比源文件新，增量判新会当成最新，
      // 于是下一次构建报出一堆莫名其妙的链接错误（实测碰到两次）。
      const std::string temporary_object = unit->object + ".tmp";
      const std::string temporary_depfile = unit->depfile + ".tmp";
      const std::string temporary_dependencies = unit->depfile + ".deps.tmp.json";
      if (toolchain.kind == CompilerKind::Msvc) {
        // MSVC 的依赖产出：`/sourceDependencies` 吐 JSON。
        // **文件名必须以 `.json` 结尾**：否则 cl 按 `/showIncludes` 的文本格式写
        // （中文 VS 上还是本地化文本），解析必然失败——而失败的表现是“头文件改了不重编”，
        // 这种静默降级比编不过危险得多（实测：结构体加一个成员后其他 .o 未重编 → ABI 不匹配崩溃）。
        args.push_back(std::format("/sourceDependencies{}", temporary_dependencies));
      } else {
        args.push_back("-pipe");
        args.push_back("-MMD");
        args.push_back("-MF");
        args.push_back(temporary_depfile);
      }
      args.push_back("-c");
      args.push_back(unit->source);
      if (toolchain.kind == CompilerKind::Msvc) {
        args.push_back(std::format("/Fo{}", temporary_object));
      } else {
        args.push_back("-o");
        args.push_back(temporary_object);
      }
      if (options.verbose) {
        log::info("compile: {} -> {}", fs::file_name(unit->source), fs::file_name(unit->object));
      }
      // 编译子进程要带上工具链环境（MSVC 的 INCLUDE/LIB/PATH 不注入就是找不到头与库）；
      // GCC 系该表为空，行为与以前完全一致。
      const process::Options process_options{.env = toolchain.env};
      auto result = process::run(args.front(), std::vector<std::string>(args.begin() + 1, args.end()),
                                 process_options);
      if (!result) {
        const std::scoped_lock lock(error_mutex);
        if (first_error.empty()) first_error = result.error().to_string();
        return;
      }
      if (result->exit_code != 0) {
        // 失败即丢掉半截产物（否则会骗过下一次增量判新）
        (void)fs::remove_file(temporary_object);
        (void)fs::remove_file(temporary_depfile);
        (void)fs::remove_file(temporary_dependencies);
        const std::scoped_lock lock(error_mutex);
        if (first_error.empty()) {
          first_error = std::format("编译失败: {}\n{}{}", unit->source, result->stdout_text,
                                    result->stderr_text);
        }
        return;
      }
      if (toolchain.kind == CompilerKind::Msvc) {
        // 把 JSON 依赖转成与 GCC 系**同一种** `.d`：
        // 下游的增量判新与共享对象缓存因此只有一套逻辑（转格式比再加一条分支便宜得多）。
        auto dependencies = fs::read_text(temporary_dependencies);
        (void)fs::remove_file(temporary_dependencies);
        std::string depfile_text = std::format("{}: {}\n", unit->object, unit->source);
        if (dependencies.has_value()) {
          auto converted = depfile_from_source_dependencies(*dependencies, unit->object);
          if (converted.has_value()) {
            depfile_text = std::move(*converted);
          } else if (!dep_warning.exchange(true)) {
            log::warn("MSVC 依赖清单解析失败（本次构建退化为只跟踪源文件）: {}",
                      converted.error().message);
          }
        } else if (!dep_warning.exchange(true)) {
          log::warn("MSVC 未产出依赖清单（{}）：头文件改动将不再触发重编",
                    fs::file_name(temporary_dependencies));
        }
        if (auto status = fs::write_text(temporary_depfile, depfile_text); !status) {
          const std::scoped_lock lock(error_mutex);
          if (first_error.empty()) first_error = status.error().to_string();
          return;
        }
      }
      // 成功：临时文件改名到位（产物位置要么是完整的，要么不存在）
      if (auto status = fs::rename(temporary_object, unit->object); !status) {
        const std::scoped_lock lock(error_mutex);
        if (first_error.empty()) first_error = status.error().to_string();
        return;
      }
      if (auto status = fs::rename(temporary_depfile, unit->depfile); !status) {
        const std::scoped_lock lock(error_mutex);
        if (first_error.empty()) first_error = status.error().to_string();
        return;
      }
      // 编译成功：写入共享缓存（失败不影响构建——缓存是加速手段，不是正确性前提）
      object_cache_store(object_cache, cache_key_for(*unit), *unit, cache_stores);
      ++done;
    });
  }
  pool.wait_idle();

  if (!first_error.empty()) return unexpected(ErrorCode::Invalid, first_error);
  if (cache_hits > 0 || cache_stores.load() > 0) {
    log::info("对象缓存：命中 {} · 新写入 {}", cache_hits, cache_stores.load());
  }
  return done.load();
}

[[nodiscard]] auto link(const Manifest& manifest, const BuildOptions& options,
                        const std::vector<CompileUnit>& units, const std::string& output,
                        const std::vector<std::string>& profile_flags_list,
                        const ResolvedToolchain& toolchain,
                        const std::vector<std::string>& extra_sources) -> Result<std::string> {
  if (toolchain.compiler_path.empty()) {
    return unexpected(ErrorCode::Unsupported, "未解析出链接器（见 resolve_toolchain）");
  }
  if (auto status = fs::ensure_parent(output); !status) return forward_error(status.error());

  // 系统库：**交叉工具链声明了 system_libs 就整体接管**——
  // 同一份清单要同时服务多平台，而"本机需要哪些系统库"（Linux 的 pthread/dl/m）
  // 对目标可能是错的甚至不存在（mingw 没有 dl/m，链接直接失败）。
  // 接管后不再追加本机默认，避免把宿主的东西塞进目标产物。
  std::vector<std::string> libraries;
  const bool toolchain_takes_over = toolchain.cross() && !toolchain.system_libs.empty();
  if (!toolchain_takes_over) {
    libraries.insert(libraries.end(), manifest.system_libs.begin(), manifest.system_libs.end());
    libraries.insert(libraries.end(), manifest.dependency_system.begin(),
                     manifest.dependency_system.end());
    const auto defaults = default_system_libs(toolchain.platform);
    libraries.insert(libraries.end(), defaults.begin(), defaults.end());
  }
  libraries.insert(libraries.end(), toolchain.system_libs.begin(), toolchain.system_libs.end());

  std::vector<std::string> args;
  if (toolchain.kind == CompilerKind::Msvc) {
    // MSVC：对象 + 输出 + 调试信息，库一律在 `/link` 之后。
    // 不用 link.exe 直接调：cl 会把同一个工具集环境与参数翻译先做一遍（路径、CRT 版本），
    // 自己拼反而容易把"用了哪个 CRT"弄成两套。
    for (const auto& unit : units) args.push_back(unit.object);
    for (const auto& extra : extra_sources) args.push_back(extra);
    args.push_back(std::format("/Fe:{}", output));
    const bool debug_info = std::ranges::any_of(profile_flags_list, [](const std::string& flag) {
      return flag.starts_with("-g");
    });
    for (const auto& flag : toolchain.extra_flags) args.push_back(flag);
    args.push_back("/link");
    // `/DEBUG` 是**链接器**选项，必须在 `/link` 之后：放在前面会被 cl 当成编译选项忽略
    // （报 D9002 而已），结果是“带 -g 构建却没有 PDB”——崩溃时连栈都符号不出来。
    if (debug_info) args.push_back("/DEBUG");
    const auto link_libraries =
        link_library_arguments(toolchain.kind, toolchain.platform, libraries);
    args.insert(args.end(), link_libraries.begin(), link_libraries.end());
  } else {
    for (const auto& flag : profile_flags_list) args.push_back(flag);
    // 链接器：存在 mold/lld 时优先（链接是纯 I/O + 符号解析，lld 通常快 2~4 倍）。
    // **交叉编译时跳过**：宿主装的 lld/mold 未必支持目标格式，交给交叉工具链自带的链接器最稳。
    if (!toolchain.cross()) {
      if (const auto lld = process::which("ld.lld"); lld.has_value()) {
        args.push_back("-fuse-ld=lld");
      } else if (const auto mold = process::which("mold"); mold.has_value()) {
        args.push_back("-fuse-ld=mold");
      }
    }
    for (const auto& unit : units) args.push_back(unit.object);
    for (const auto& extra : extra_sources) args.push_back(extra);
    args.push_back("-o");
    args.push_back(output);
    const auto link_arguments =
        link_library_arguments(toolchain.kind, toolchain.platform, libraries);
    for (const auto& item : link_arguments) args.push_back(item);
    for (const auto& flag : toolchain.extra_flags) args.push_back(flag);
  }

  auto result = process::run(toolchain.compiler_path, args,
                             process::Options{.cwd = manifest.directory, .env = toolchain.env});
  if (!result) return forward_error(result.error());
  if (result->exit_code != 0) {
    const std::string diagnostics = result->stdout_text + result->stderr_text;
    // 产物被正在运行的实例占着（Windows 上必现：exe 被占用时链接器写不进去）。
    // 原始报错只有一句 `LNK1168 无法打开 …exe 进行写入`，排错的人第一反总不是“那个窗口还开着”——
    // 直接给出下一步。
    if (diagnostics.find("LNK1168") != std::string::npos ||
        diagnostics.find("LNK1104") != std::string::npos ||
        diagnostics.find("Text file busy") != std::string::npos) {
      return unexpected(
          ErrorCode::Io,
          std::format("链接失败：产物被占用（{}）\n"
                      "请先结束正在运行的实例（例如 `st stop {}` 或结束进程），再重新构建。\n{}",
                      output, manifest.name, diagnostics));
    }
    return unexpected(ErrorCode::Invalid,
                      std::format("链接失败: {}\n{}", output, diagnostics));
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



auto build(const Manifest& manifest, const BuildOptions& options) -> Result<BuildStats> {
  const std::int64_t start_ns = time::now_ns();
  const std::string root = options.root.empty() ? manifest.directory : options.root;
  auto flags = profile_flags(options.profile);
  if (!flags) return forward_error(flags.error());

  auto sources = manifest.source_files();
  if (!sources) return forward_error(sources.error());

  // 框架引用（源码级依赖）：解析出框架贡献，并把它的编译选项合入**局部副本**。
  // 用副本而非改原件：清单是调用方的只读输入，构建过程不该污染它。
  std::optional<Framework> framework;
  if (manifest.framework.has_value()) {
    auto loaded = load_framework(*manifest.framework);
    if (!loaded) return forward_error(loaded.error());
    framework = std::move(*loaded);
  }
  Manifest effective = manifest;
  if (framework.has_value()) {
    const auto append = [](std::vector<std::string>& target, const std::vector<std::string>& extra) {
      target.insert(target.end(), extra.begin(), extra.end());
    };
    append(effective.include_dirs, framework->include_dirs);
    append(effective.flags, framework->flags);
    append(effective.c_flags, framework->c_flags);
    append(effective.defines, framework->defines);
    append(effective.system_libs, framework->system_libs);
    // 工具链：框架的并入，**引用方同名优先**（引用方知道自己环境的编译器叫什么）
    for (const auto& toolchain : framework->toolchains) {
      if (effective.find_toolchain(toolchain.name) == nullptr) {
        effective.toolchains.push_back(toolchain);
      }
    }
  }

  auto toolchain = resolve_toolchain(effective, options.toolchain);
  if (!toolchain) return forward_error(toolchain.error());
  const std::string subdir = profile_directory(options.profile, *toolchain);
  const std::string object_dir = fs::join(root, std::format("build/{}/obj", subdir));
  const std::string bin_dir = fs::join(
      root, std::format("build/{}/bin", profile_directory(options.profile, *toolchain)));
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

  // 第三方源码（third_party/）与库源一起编译：它们不是"可选附加"，而是库的组成部分。
  // 同时把路径集合交给 make_units——这些单元编译时会放宽告警并跳过 PCH。
  std::set<std::string> third_party_paths = collect_third_party(manifest);
  UnitIncludeMap library_includes;

  // 嵌入的"模板与运行时"来源：有框架就用框架的（工程不必自己内联一份 battery）；
  // 没有框架时用工程自己的 third_party/battery（自包含工程）。
  const std::string embed_support =
      framework.has_value() ? framework->embed_support_directory
                            : fs::join(manifest.directory, "third_party/battery");
  const std::string embed_runtime = fs::join(embed_support, "embed_impl.cpp");
  // 运行时实现（含进程级全局表）**恰好一处**提供：框架有嵌入就归框架，否则归工程库级。
  const bool framework_owns_runtime = framework.has_value() && !framework->embed.empty();

  // ① 工程自己的库级嵌入：只下发给**当前的工程单元**（此刻框架源还没进来，顺序很关键）
  {
    auto request = library_embed_request(manifest.name, manifest.directory, manifest.embed, subdir,
                                         embed_support, embed_runtime,
                                         /*with_runtime=*/!framework_owns_runtime);
    if (auto status = attach_embeds(request, library_sources, library_sources, third_party_paths,
                                    library_includes);
        !status) {
      return forward_error(status.error());
    }
  }

  // ② 框架源（绝对路径）并入；框架的第三方 C 源按第三方对待
  std::set<std::string> framework_sources{};
  if (framework.has_value()) {
    for (const auto& source : framework->sources) {
      library_sources.push_back(source);
      framework_sources.insert(source);
    }
    for (const auto& source : framework->third_party_sources) {
      library_sources.push_back(source);
      third_party_paths.insert(source);
      framework_sources.insert(source);
    }
    // ③ 框架自己的嵌入：只下发给**框架单元**（否则与工程的同名头在命令行上撞车）
    auto request = library_embed_request(framework->name, framework->directory, framework->embed,
                                         subdir, embed_support, embed_runtime,
                                         /*with_runtime=*/framework_owns_runtime);
    if (auto status = attach_embeds(request, framework->sources, library_sources, third_party_paths,
                                    library_includes);
        !status) {
      return forward_error(status.error());
    }
  }

  std::vector<std::string> all_sources = library_sources;
  for (const auto& path : third_party_paths) all_sources.push_back(path);
  std::ranges::sort(all_sources);
  all_sources.erase(std::unique(all_sources.begin(), all_sources.end()), all_sources.end());

  std::vector<CompileUnit> units =
      make_units(effective, all_sources, object_dir, third_party_paths, library_includes,
                 framework_sources);
  // 框架单元专用的标志集（框架自己的头/宏/严格集）——它决定框架对象能否跨工程命中缓存
  std::optional<FrameworkFlags> framework_flags;
  if (framework.has_value()) {
    framework_flags = make_framework_flags(manifest, framework->include_dirs, *flags, *toolchain);
  }
  std::size_t rebuilt = 0;
  const std::int64_t compile_start = time::now_ns();
  auto compiled = compile_units(effective, options, units, *flags, *toolchain,
                                framework_flags.has_value() ? &*framework_flags : nullptr, rebuilt);
  if (!compiled) return forward_error(compiled.error());
  const std::int64_t compile_ms = (time::now_ns() - compile_start) / 1'000'000;

  BuildStats stats;
  stats.units_total = units.size();
  stats.units_rebuilt = rebuilt;
  stats.units_cached = units.size() - rebuilt;
  stats.compile_ms = compile_ms;
  // 并发方案（与 `compile_units` 里用的是同一个推导）：摘要里如实展示，
  // 让用户看到"为什么是这个并发数"，而不是只能猜。
  {
    const ConcurrencyPlan plan = plan_concurrency(options.jobs, options.jobs_large,
                                                  options.max_memory_mb, options.profile,
                                                  hardware_concurrency());
    stats.workers = plan.jobs;
    stats.workers_large = plan.jobs_large;
    stats.memory_budget_mb = plan.budget_mb;
    stats.concurrency_reason = plan.reason;
  }
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
    // 生成的源与上游运行时都按"第三方"对待（`-w`、不插桩）——它们不是本工程的手写代码。
    UnitIncludeMap unit_includes;
    std::set<std::string> target_third_party = third_party_paths;  // 本目标的第三方集合（含生成的嵌入源码）
    if (!target->embed.empty()) {
      // 目标级嵌入（作用域 = 目标名，只下发给本目标的单元）。
      // 运行时实现由"库级嵌入"提供（框架有嵌入则归框架、否则归工程库级、都没有则归本目标）
      // ——同链接里出现两份全局态会重复定义符号（踩过）。
      EmbedRequest request;
      request.work_directory = manifest.directory;
      request.template_directory = embed_support;
      request.runtime_source = embed_runtime;
      request.build_subdir = subdir;
      request.scope = options.target;
      request.with_runtime = !framework_owns_runtime && manifest.embed.empty();
      EmbedContribution contribution;
      contribution.base_directory = manifest.directory;
      contribution.patterns = target->embed;
      contribution.identifier_prefix = options.target;
      contribution.origin = std::format("目标 {}", options.target);
      request.contributions.push_back(std::move(contribution));
      // `attach_embeds` 会把包含目录下发到 `target_sources` 里的每个单元
      // （含目标自身的源——只给生成源加会报 "battery/embed.hpp: No such file or directory"），
      // 键的绝对化也在里面统一处理（曾因相对/绝对不匹配静默查不到）。
      if (auto status = attach_embeds(request, target_sources, target_sources, target_third_party,
                                      unit_includes);
          !status) {
        return forward_error(status.error());
      }
    }
    std::ranges::sort(target_sources);
    target_sources.erase(std::unique(target_sources.begin(), target_sources.end()),
                         target_sources.end());
    target_units =
        make_units(effective, target_sources, object_dir, target_third_party, unit_includes);
    std::size_t target_rebuilt = 0;
    auto target_compiled =
        compile_units(effective, options, target_units, *flags, *toolchain,
                      framework_flags.has_value() ? &*framework_flags : nullptr, target_rebuilt);
    if (!target_compiled) return forward_error(target_compiled.error());
    stats.units_total += target_units.size();
    stats.units_rebuilt += target_rebuilt;
    stats.units_cached = stats.units_total - stats.units_rebuilt;
  }

  // 产物后缀按目标平台：Windows 必须是 `.exe`（否则系统不认为它是可执行程序）
  const std::string output = fs::join(bin_dir, target->name + toolchain->executable_suffix);
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

  auto linked = link(manifest, options, link_units, output, *flags, *toolchain, {});
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

  // 框架引用同样生效：独立工程的测试要能链接框架代码，且测试框架（`src/test/*.cpp`）来自框架
  std::optional<Framework> framework;
  if (manifest.framework.has_value()) {
    auto loaded = load_framework(*manifest.framework);
    if (!loaded) return forward_error(loaded.error());
    framework = std::move(*loaded);
  }
  Manifest effective = manifest;
  if (framework.has_value()) {
    const auto append = [](std::vector<std::string>& target, const std::vector<std::string>& extra) {
      target.insert(target.end(), extra.begin(), extra.end());
    };
    append(effective.include_dirs, framework->include_dirs);
    append(effective.flags, framework->flags);
    append(effective.c_flags, framework->c_flags);
    append(effective.defines, framework->defines);
    append(effective.system_libs, framework->system_libs);
    // 工具链：框架的并入，**引用方同名优先**（引用方知道自己环境的编译器叫什么）
    for (const auto& toolchain : framework->toolchains) {
      if (effective.find_toolchain(toolchain.name) == nullptr) {
        effective.toolchains.push_back(toolchain);
      }
    }
  }

  auto toolchain = resolve_toolchain(effective, options.toolchain);
  if (!toolchain) return forward_error(toolchain.error());
  const std::string subdir = profile_directory(options.profile, *toolchain);
  const std::string object_dir = fs::join(root, std::format("build/{}/obj", subdir));
  const std::string bin_dir = fs::join(
      root, std::format("build/{}/bin", profile_directory(options.profile, *toolchain)));
  if (auto status = fs::create_directories(object_dir); !status) return forward_error(status.error());
  if (auto status = fs::create_directories(bin_dir); !status) return forward_error(status.error());

  // 测试可执行 = 库源（排除框架入口）+ 框架源 + 测试框架 + 用例
  std::vector<std::string> library_sources;
  for (const auto& source : *sources) {
    if (source.starts_with("src/test/") || source.starts_with("tools/")) continue;
    library_sources.push_back(source);
  }
  // 测试框架（`src/test/*.cpp`，含 main）来自框架；无框架时是工程自己的
  const std::string runner_root = framework.has_value() ? framework->directory : manifest.directory;
  std::vector<std::string> runner_sources;
  auto runner = fs::expand_glob(runner_root, "src/test/*.cpp");
  if (runner) {
    for (const auto& item : *runner) {
      runner_sources.push_back(fs::is_absolute(item) ? item : fs::join(runner_root, item));
    }
  }
  std::set<std::string> third_party_paths = collect_third_party(manifest);
  UnitIncludeMap test_includes;

  const std::string embed_support =
      framework.has_value() ? framework->embed_support_directory
                            : fs::join(manifest.directory, "third_party/battery");
  const std::string embed_runtime = fs::join(embed_support, "embed_impl.cpp");
  const bool framework_owns_runtime = framework.has_value() && !framework->embed.empty();

  // ① 工程自己的库级嵌入（只下发当前工程单元；框架源此刻还没进来）
  {
    auto request = library_embed_request(manifest.name, manifest.directory, manifest.embed, subdir,
                                         embed_support, embed_runtime,
                                         /*with_runtime=*/!framework_owns_runtime);
    if (auto status = attach_embeds(request, library_sources, library_sources, third_party_paths,
                                    test_includes);
        !status) {
      return forward_error(status.error());
    }
  }
  // ② 框架源 + ③ 框架嵌入（只下发框架单元）
  std::set<std::string> framework_sources{};
  if (framework.has_value()) {
    for (const auto& source : framework->sources) {
      library_sources.push_back(source);
      framework_sources.insert(source);
    }
    for (const auto& source : framework->third_party_sources) {
      library_sources.push_back(source);
      third_party_paths.insert(source);
      framework_sources.insert(source);
    }
    auto request = library_embed_request(framework->name, framework->directory, framework->embed,
                                         subdir, embed_support, embed_runtime,
                                         /*with_runtime=*/framework_owns_runtime);
    if (auto status = attach_embeds(request, framework->sources, library_sources, third_party_paths,
                                    test_includes);
        !status) {
      return forward_error(status.error());
    }
  }

  std::vector<std::string> all = library_sources;
  for (const auto& item : *tests) all.push_back(item);
  for (const auto& item : runner_sources) all.push_back(item);
  for (const auto& path : third_party_paths) all.push_back(path);
  std::ranges::sort(all);
  all.erase(std::unique(all.begin(), all.end()), all.end());
  std::vector<CompileUnit> units =
      make_units(effective, all, object_dir, third_party_paths, test_includes, framework_sources);
  std::optional<FrameworkFlags> framework_flags;
  if (framework.has_value()) {
    framework_flags = make_framework_flags(manifest, framework->include_dirs, *flags, *toolchain);
  }
  std::size_t rebuilt = 0;
  auto compiled = compile_units(effective, options, units, *flags, *toolchain,
                                framework_flags.has_value() ? &*framework_flags : nullptr, rebuilt);
  if (!compiled) return forward_error(compiled.error());
  log::info("测试构建：{} 单元（重编 {}）", units.size(), rebuilt);

  const std::string output = fs::join(bin_dir, "st_tests" + toolchain->executable_suffix);
  auto linked = link(manifest, options, units, output, *flags, *toolchain, {});
  if (!linked) return forward_error(linked.error());

  std::vector<std::string> args;
  if (!filter.empty()) args.push_back(std::string(filter));
  // 交叉编译产物不能在本机执行：明确告知（比 "Exec format error" 可读得多）
  if (toolchain->cross()) {
    return unexpected(ErrorCode::Unsupported,
                      std::format("交叉编译产物无法在宿主执行: {}（请在目标平台运行）", output));
  }
  auto result = process::run(output, args, process::Options{.capture_output = false});
  if (!result) return forward_error(result.error());
  return result->exit_code;
}

}  // namespace st::pkg
