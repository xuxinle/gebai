#include "st/pkg/build.hpp"
#include "st/pkg/compiler.hpp"
#include "st/pkg/embed.hpp"
#include "st/pkg/memory.hpp"
#include "st/pkg/framework.hpp"
#include "st/pkg/junit.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
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
  // 两个前端**同一条口径**（GCC 风格标志 + libstdc++ 运行库），所以优先序只影响"用哪个前端"，
  // 不影响构建语义。g++ 在前：它的 C++20 支持随发行版走，且 Windows 上自带 libstdc++。
  for (const auto name : {"g++", "clang++", "c++"}) {
    if (auto found = process::which(name); found.has_value()) return *found;
  }
  return unexpected(ErrorCode::Unsupported,
                    "未找到 C++ 编译器：安装 MinGW-w64 的 g++，或 LLVM 的 clang++"
                    "（把所在目录加入 PATH，或用 ST_CXX / CXX 显式指定）");
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

/// 有效编译标志的指纹：**含档位标志与 PCH 身份**。
///
/// 档位标志原先既不在时间戳判据里、也不在工程单元的缓存键里 —— 改档位等于没改。
/// 这里把它显式纳入，供"增量判新"与"共享缓存键"两处共同使用。
///
/// `pch_token`：该单元消费的 PCH 的身份（不消费时为）。PCH 不在 `.d` 依赖里，
/// 但它决定对象的语义（旧头内联定义会不会随 PCH 变化）——必须显式带进判定。
[[nodiscard]] auto compile_fingerprint(CompilerKind kind,
                                       const std::vector<std::string>& unit_flags,
                                       const std::vector<std::string>& profile_flags,
                                       bool third_party, const std::string& pch_token) -> std::string {
  std::string material(compiler_kind_name(kind));
  for (const auto& flag : unit_flags) material.append("\n").append(flag);
  for (const auto& flag : profile_flags) material.append("\n@").append(flag);
  if (third_party) material.append("\n-w");
  if (!pch_token.empty()) material.append("\n").append(pch_token);
  return std::string(st::hash::fnv1a64_hex(material));
}

/// 编译标志指纹文件：与对象同目录的 `<对象>.flags`，内容是**有效编译标志**的哈希。
///
/// 为什么必须有它（实测踩到的静默降级）：
/// `needs_rebuild` 原先只看**时间戳**（源文件/依赖头 vs 对象），而**编译标志变了
/// 并不会让任何源文件变新**——于是"改了档位或 `-D` 定义，产物根本不重建"，
/// 构建还高高兴兴报"重编 0 / 命中缓存 N"，人以为改动生效了。
/// 这与本文档 §8.2 第 30 条（改头文件不重编 → ABI 不匹配 → 随机器崩溃）是**同一类缺陷**，
/// 只是触发键从"头文件"换成了"编译标志"，后果一样：产物与预期不符却一声不响。
///
/// 实测：把 `dev` 档的 `-g` 去掉后重建，报"重编 0"，exe 大小一字节没变。
[[nodiscard]] auto flags_stamp_path(const std::string& object) -> std::string {
  return object + ".flags";
}

/// 目标是否需要重建：源文件、**任一被包含的头文件**、**编译标志**或 **PCH 身份**变化时重建。
[[nodiscard]] auto needs_rebuild(const CompileUnit& unit,
                                 const std::vector<std::string>& unit_flags,
                                 const std::vector<std::string>& profile_flags,
                                 CompilerKind kind, const std::string& pch_token) -> bool {
  if (!fs::is_regular_file(unit.object)) return true;
  const auto object_time = fs::modified_ns(unit.object);
  if (!object_time) return true;
  // 编译标志变了 → 必须重编（时间戳看不出这件事）
  const auto stamp = fs::read_text(flags_stamp_path(unit.object));
  if (!stamp.has_value() ||
      *stamp != compile_fingerprint(kind, unit_flags, profile_flags, unit.third_party, pch_token)) {
    return true;
  }
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
  std::string directory{};         ///< 含 PCH 产物的目录（`-I` 传入，供 `.gch` 同名命中）
  std::string header{};            ///< `-include` 的头名（如 `prefix.hpp`）
  /// 本次 PCH 的**身份令牌**（时间戳 + 大小）：进入编译指纹与缓存键。
  ///
  /// 为什么必须有：对象会把 PCH 里的定义内联进来（旧头内联体），而 **PCH 不在 `.d`
  /// 依赖清单里**——PCH 重建后，旧对象仍然"看起来是最新的"，链接时是拿旧头定义跑出的
  /// 不一致产物。宁可多编一次，不可静默错。
  std::string token{};
};

/// 构建（或复用）预编译头；失败时返回空（不阻断构建，只是慢一点）。
///
/// 关键细节（都踩过坑）：
/// 1. **代理头**：不直接编译 `pch.hpp`——头文件被当作"主文件"编译时 `#pragma once` 必然告警，
///    而为了压掉它需要 `-Wno-error`，这又会让 PCH 与消费者**标志不一致 → 编译器判 PCH 无效**。
///    改为生成 `prefix.hpp`（仅一行 `#include "st/pch.hpp"`）作为编译入口，问题消失且标志可完全一致。
/// 2. **标志一致**：PCH 创建与消费必须使用同一套标志（std/优化/告警/宏/包含路径）；
///    消费端只多 `-include` 与依赖输出选项（与 PCH 有效性无关）。
/// 3. **`target_triple` 必须一致**：`--target` 决定用哪套系统头/运行库，创建与消费两侧
///    不一致时编译器会**静默忽略** PCH（不报错，只是变慢）——所以它由调用方拼进同一份 flags。
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
      ST_LOG_WARN("预编译头构建失败（忽略，按常规编译进行）: {}", detail);
      (void)fs::remove_file(gch);
      return std::nullopt;
    }
    ST_LOG_INFO("预编译头已生成: {}", gch);
  }
  const auto gch_time = fs::modified_ns(gch).value_or(0);
  const auto gch_size = fs::file_size(gch).value_or(0);
  return PchContext{directory, "prefix.hpp",
                    std::format("<pch:{}:{}>", gch_time, gch_size)};
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
  /// 编译/链接**子进程**需要的环境变量。GCC 与 Clang 都不需要额外注入
  /// （系统头/库的位置由编译器自己的 driver 与 `--target` 决定），
  /// 保留这个字段是为了"子进程环境"这件事有一个明确的落点：
  /// 将来若有工具链需要它（如某些交叉工具链要 `SYSROOT`），加在这里而不是散落各处。
  /// 显式传递而不是改自身进程环境——"构建过程中修改自己的环境"会让（并行）编译、
  /// 链接、测试跑在不同环境里。
  std::map<std::string, std::string> env{};
  std::string platform{};           ///< 目标平台
  std::string executable_suffix{};  ///< 产物后缀（Windows 为 `.exe`）
  std::vector<std::string> system_libs{};
  std::vector<std::string> defines{};
  std::vector<std::string> extra_flags{};
  /// 目标专属**告警收敛**（只影响编译，不参与链接；见 `ToolchainSpec::suppressions`）。
  std::vector<std::string> suppressions{};
  /// 目标三元组（如 `x86_64-w64-windows-gnu`；空 = 用编译器自己的默认目标）。
  ///
  /// 为何需要它：`clang++` 的默认目标跟着**它自己的构建方式**走——LLVM 官方 Windows 包
  /// 编译成 MSVC 目标（吃 VS 的头与库），而我们要的是“GCC 风格驱动 + libstdc++ 运行库”
  /// 那条口径（`--target=x86_64-w64-windows-gnu`）。不显式指定时，`clang++` 在第一
  /// 个平台分支就会失败（MSVC STL 要求 Clang ≥ 20），而且报错位置离真正原因很远。
  std::string triple{};
  std::string directory_tag{};      ///< 目录隔离标记（空=本机档）
  /// 是否具备 sanitizer 运行库（仅 san 档用）。Windows 的 MinGW 发行版默认**不带**
  /// `libasan`/`libubsan`（实测 16.2.0），拿 `-fsanitize=` 直接链会得到 “cannot find -lasan”——
  /// 在 san 档构建前先探到并给出可行动的报错，而不是让它消失在几十条链接错里。
  bool sanitizers_available{true};
  [[nodiscard]] auto cross() const noexcept -> bool { return !directory_tag.empty(); }
  /// 编译与链接**共用**的目标参数（空三元组时为空）。
  ///
  /// 抽成一处而不是两侧各写一遍：编译用 A 目标、链接用 B 目标是最难查的一类错
  /// （报错是“找不到符号”或“无效的对象文件”，完全不提目标不一致）。
  [[nodiscard]] auto target_args() const -> std::vector<std::string> {
    if (triple.empty()) return {};
    return {std::format("--target={}", triple)};
  }
  /// 交叉产物能否在宿主直接执行：**目标平台 == 宿主平台**即放行（Windows 宿主上的
  /// mingw 交叉档产出的是本机可执行的 PE——`st test --toolchain=mingw` 应照常跑）；
  /// 目标与宿主不同（Linux 宿主编 mingw）仍拒绝，比 "Exec format error" 可读得多。
  [[nodiscard]] auto runs_on_host() const noexcept -> bool {
    return !cross() || platform == host_platform();
  }
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
    resolved.suppressions = spec->suppressions;
    resolved.triple = spec->target_triple;
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
  auto kind = compiler_kind_of(resolved.compiler_path);
  if (!kind) return forward_error(kind.error());
  resolved.kind = *kind;
#if defined(_WIN32)
  // 本机 Windows 档补系统版本宏：MinGW 头文件的默认 WINVER 偏旧，
  // 不补会让 `GetDpiForWindow`/`GetDpiForSystem` 等目标 API 在编译期不可见。
  // 交叉工具链在清单里自己声明同一组宏（同一份源码两种构建得到同一可见 API 面）。
  if (resolved.directory_tag.empty()) {
    for (const auto* define : {"_WIN32_WINNT=0x0A00", "WINVER=0x0A00",
                              "NTDDI_VERSION=0x0A000000"}) {
      if (std::ranges::find(resolved.defines, define) == resolved.defines.end()) {
        resolved.defines.emplace_back(define);
      }
    }
  }
#endif
  // sanitizer 运行库探测：只用"编译器能否找到自己的 sanitizer 库"判定，
  // 不依赖发行版名称（MinGW 多数发行版不带，Linux 发行版一般自带）。
  {
    const auto probe_one = [&](std::string_view library) -> bool {
      auto probe = process::run(resolved.compiler_path,
                                {std::format("-print-file-name={}", library)});
      if (!probe || probe->exit_code != 0) return false;
      // "找不到"时 GCC 原样回显库名（没有路径分隔符）——有路径才算真的存在。
      const std::string_view answer = st::trim(probe->stdout_text);
      return !answer.empty() && answer != library &&
             (answer.find('/') != std::string_view::npos ||
              answer.find('\\') != std::string_view::npos);
    };
    resolved.sanitizers_available = probe_one("libasan.a") || probe_one("libubsan.a");
  }
  return resolved;
}

/// 框架自己的编译标志集（框架单元专用：与引用方工程无关）。
struct FrameworkFlags {
  std::vector<std::string> flags{};    ///< C++：框架的 flags + defines + include_dirs
  std::vector<std::string> c_flags{};  ///< C：同上但用 c_flags
};

/// 由框架清单构造框架标志集（框架单元专用：与引用方工程无关）。
///
/// 清单（含框架自己的 `st.pkg`）恒写一份 **GCC 风格**跨平台标志，GCC 与 Clang 都直接吃。
///
/// ⚠ **`framework_c_flags` 必须来自框架清单**（不是调用方工程的 `c_flags`）。
/// 这里曾直接用调用方 `manifest.c_flags`：框架自己写的是
/// `"c_flags": ["-std=gnu11", "-include", "st_sqlite3_config.h"]`
/// （为 sqlite3.c 前置那一组 `SQLITE_*` 开关），而引用方工程的 `c_flags` 一般是**空**的——
/// 于是框架单元里唯一那个 C 源（sqlite3.c）丢了 `-include`，
/// `SQLITE_ENABLE_COLUMN_METADATA` 没定义 ⇒ `sqlite3_column_table_name/origin_name`
/// 在**链接期**才报 undefined reference（编译期毫无症状，因为头文件里根本不声明它们）。
///
/// 为什么"编译期没症状"必须写下来：框架自身构建时 `manifest` 就是框架清单，
/// 两者恰好相等，所以**框架自己构建一百次都不会复现**——只有"独立工程引用框架"才暴露。
[[nodiscard]] auto make_framework_flags(const Manifest& framework_manifest,
                                        const std::vector<std::string>& framework_include_dirs,
                                        const std::vector<std::string>& framework_c_flags,
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

  // C 源专用标志：框架的 `c_flags`（含 `-include st_sqlite3_config.h` 这类前置头）
  out.c_flags = framework_c_flags;
  for (const auto& item : framework_manifest.defines) {
    out.c_flags.push_back(std::format("-D{}", item));
  }
  for (const auto& dir : framework_include_dirs) out.c_flags.push_back(std::format("-I{}", dir));
  for (const auto& item : toolchain.defines) out.c_flags.push_back(std::format("-D{}", item));
  for (const auto& item : profile_flags_list) out.c_flags.push_back(item);
  out.c_flags.push_back("-x");
  out.c_flags.push_back("c");
  // 工具链专属告警收敛：框架单元也必须带上（框架与调用方工程共用同一份源码口径）
  out.flags.insert(out.flags.end(), toolchain.suppressions.begin(), toolchain.suppressions.end());
  out.c_flags.insert(out.c_flags.end(), toolchain.suppressions.begin(),
                     toolchain.suppressions.end());
  // 目标三元组（`--target`）：框架单元与引用方工程用**同一个**目标，
  // 否则共享对象缓存的键会随工程变化、缓存永不命中。
  for (const auto& item : toolchain.target_args()) {
    out.flags.push_back(item);
    out.c_flags.push_back(item);
  }

  for (const auto language : {SourceLanguage::Cxx, SourceLanguage::C}) {
    const auto pins = dialect_flags(toolchain.kind, language);
    auto& target = language == SourceLanguage::Cxx ? out.flags : out.c_flags;
    target.insert(target.end(), pins.begin(), pins.end());
  }
  return out;
}

/// 组装 C++ 与 C **两套**编译标志（顺序即语义，见下方注释）。
///
/// 抽成函数的理由（结构评审 P2）：`compile_units` 原先在一个函数里同时承担
/// 「标志组装」「增量判定」「缓存查询」「并发调度」「错误汇总」五件事（349 行）。
/// 这是其中最独立的一段——纯计算、不碰文件系统、可单独推敲。
/// 顺序语义仍然是这里的核心不变量，故注释随代码一起搬过来。
struct LanguageFlags {
  std::vector<std::string> cxx{};  ///< C++ 标志（PCH 建立与 C++ 单元共用，顺序必须一致）
  std::vector<std::string> c{};    ///< C 标志（`c_flags` + 档位 + 宏 + 包含路径）
};

[[nodiscard]] auto build_language_flags(const Manifest& manifest, const BuildOptions& options,
                                        const std::vector<std::string>& include_dirs,
                                        const std::vector<std::string>& target_defines,
                                        const std::vector<std::string>& profile_flags_list,
                                        const ResolvedToolchain& toolchain) -> LanguageFlags {
  // 顺序语义：工程全局严格集（清单 `flags`）在前，**分档标志在后**——
  // 分档可以针对优化等级做有据可查的例外（如优化档关闭 GCC 误报的 -Wnull-dereference）。
  // **C 与 C++ 各自一套语言标志**：`manifest.flags` 是本工程的 *C++* 严格集
  // （含 `-Wnon-virtual-dtor`/`-Woverloaded-virtual` 这类 C++ 专属告警），给 C 源会直接报
  // 「option is valid for C++ but not for C」。因此 C 源只取 `c_flags` + 档位 + 宏 + 包含路径；
  // 想让自家 C 源也严格，就在 `c_flags` 里显式写 `-Wall -Werror`（第三方源则另有 `-w`）。
  LanguageFlags out;
  out.cxx.push_back("-std=c++20");
  for (const auto& item : manifest.flags) out.cxx.push_back(item);
  for (const auto& item : options.extra_flags) out.cxx.push_back(item);
  for (const auto& item : manifest.defines) out.cxx.push_back(std::format("-D{}", item));
  for (const auto& item : target_defines) out.cxx.push_back(std::format("-D{}", item));
  for (const auto& dir : include_dirs) out.cxx.push_back(std::format("-I{}", dir));
  for (const auto& item : profile_flags_list) out.cxx.push_back(item);
  // 工具链专属告警收敛**必须在档位标志之后**：它是"最后一道收敛"，语义上覆盖前面所有来源
  // （工程严格集、档位例外、框架并入）。放在这里也让缓存键自然包含它。
  for (const auto& item : toolchain.suppressions) out.cxx.push_back(item);

  for (const auto& item : manifest.c_flags) out.c.push_back(item);
  for (const auto& item : options.extra_flags) out.c.push_back(item);
  for (const auto& item : manifest.defines) out.c.push_back(std::format("-D{}", item));
  for (const auto& item : target_defines) out.c.push_back(std::format("-D{}", item));
  for (const auto& dir : include_dirs) out.c.push_back(std::format("-I{}", dir));
  for (const auto& item : profile_flags_list) out.c.push_back(item);
  for (const auto& item : toolchain.suppressions) out.c.push_back(item);
  // `-x c` 强制按 C 编译：同一个编译器二进制即可，无需第二套工具链
  out.c.push_back("-x");
  out.c.push_back("c");

  // 目标三元组：**必须与缓存键、PCH 创建侧使用同一份**（`clang --target=…`），
  // 否则"缓存键说一个目标、命令行却编另一个目标"，产物与记录不符。
  for (const auto& item : toolchain.target_args()) {
    out.cxx.push_back(item);
    out.c.push_back(item);
  }

  const auto add_dialect_flags = [&toolchain](std::vector<std::string>& list,
                                              SourceLanguage language) {
    const auto pins = dialect_flags(toolchain.kind, language);
    list.insert(list.end(), pins.begin(), pins.end());
  };
  add_dialect_flags(out.cxx, SourceLanguage::Cxx);
  add_dialect_flags(out.c, SourceLanguage::C);

  // 重名与包含路径校验放在这里（原先紧跟在组装之后），保持"组装即校验"的一处性。
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
  const LanguageFlags language_flags = build_language_flags(manifest, options, include_dirs,
                                                            target_defines, profile_flags_list,
                                                            toolchain);
  std::vector<std::string> flags = language_flags.cxx;
  std::vector<std::string> c_flags = language_flags.c;

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
    // 检查模式不建 PCH：**另一个编译器吃不了这个编译器的 .gch**
    // （`prefix.hpp.gch` 是 gcc 的格式，clang 会直接报 “is not a valid precompiled header”），
    // 而“用 clang 检查”正是 `st check` 的主要用法。代价是每单元重解析标准库头——
    // 实测 77 单元 16.5 s（见 `docs/BUILD_CHECK.md`），可接受。
    if (!pch_header.empty() && !options.check_only) {
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
    // 文件级加速档：SIMD 内核单独启用 AVX2（其余单元不动）。
    // 为什么不给全工程开：`-mavx2` 会改变全局代码生成与 ABI 假设，且 CPUID 门控在运行时
    // 才发生——编译期全局开关会牺牲老 CPU 兼容性；只在运行时按能力分派的文件里启用，
    // 其他单元保持默认（SSE2 基线）。
    if (!is_c && unit.source.find("simd.cpp") != std::string::npos) {
      chosen.emplace_back("-mavx2");
    }
    return chosen;
  };
  // 该单元消费的 PCH 身份（不消费者为空）：只有“真正会吃 PCH 的单元”才带 token，
  // 与编译分支的判定条件保持一致（否则无关单元会因 PCH 重建而无谓重编）。
  const auto pch_token_for = [&](const CompileUnit& unit) -> std::string {
    if (pch_ptr == nullptr || unit.lang == SourceLang::C || unit.framework_unit || unit.third_party) {
      return {};
    }
    return pch_ptr->token;
  };
  const auto cache_key_for = [&](const CompileUnit& unit) -> std::string {
    std::string material(compiler_path);
    // 编译器族入键：同一路径下的不同族产物不兼容（且**目标三元组**也入键——
    material.append("\n<").append(compiler_kind_name(toolchain.kind)).append(">");
    // 目标三元组入键：同一份源码编给不同目标（gnu vs msvc）产物不兼容，
    // 而**命令行的 `--target` 不在 flags 里**（它由 `target_args()` 追加）——
    // 不入键会让"换目标后仍命中旧对象"，链接期报无效对象文件。
    material.append("\n<t:").append(toolchain.triple).append(">");
    // **依赖清单格式版本**入键：缓存项的有效性靠它自己的 `.d` 判定，一旦依赖产出方式变了
    // （转格式规则、编译器输出格式等），旧条目的 `.d`
    // 可能不完整——“头文件改了不重编”这种静默降级会把错对象缓存永久钉住。
    // 改动依赖产出方式时**必须**升这个版本号。
    material.append("\n<deps:2>");
    for (const auto& flag : unit_flags_for(unit)) material.append("\n").append(flag);
    // **档位标志也必须入键**：工程单元（如示例应用自己的 .cpp）的 `unit_flags_for`
    // 只返回 st.pkg 里的 flags，不含 `-O1/-g/-DNDEBUG/-fsanitize`。
    // 不入键的话，共享缓存会把**另一个档位**编出来的对象交给本次构建
    // （实测：去掉 dev 的 `-g` 后重建，报"命中缓存 67"、产物一字节没变）。
    for (const auto& flag : profile_flags_list) material.append("\n@").append(flag);
    material.append("\n").append(unit.source);
    if (unit.third_party) material.append("\n-w");
    if (unit.framework_unit) material.append("\n<framework>");
    if (const auto token = pch_token_for(unit); !token.empty()) material.append("\n").append(token);
    for (const auto& dir : unit.extra_include_dirs) material.append("\n-I").append(dir);
    return st::hash::fnv1a64_hex(material);
  };

  std::size_t cache_hits = 0;
  std::atomic<std::size_t> cache_stores{0};
  std::vector<const CompileUnit*> pending;
  pending.reserve(units.size());
  for (const auto& unit : units) {
    // 检查模式：**全部单元都查**——不做增量判定，也不查共享对象缓存。
    // 理由：这里的两个跳过机制判的都是“`.o` 是不是还要重编”，而检查模式根本不产出 `.o`。
    // 若沿用它们，一个“已构建过、源码未变”的仓库会**一个单元都不查**，
    // 于是 `st check` 直接报成功——一个恒绿的检查比没有检查更糟。
    const bool must_check = options.check_only;
    if (!must_check && !options.force &&
        !needs_rebuild(unit, unit_flags_for(unit), profile_flags_list, toolchain.kind,
                       pch_token_for(unit))) {
      continue;
    }
    // 工程内已过期 → 先问共享缓存：命中就不必真编译（跨工程复用框架对象的主要路径）
    if (!must_check && !options.force &&
        object_cache_restore(object_cache, cache_key_for(unit), unit)) {
      ++cache_hits;
      // 从共享缓存恢复的对象也要补写标志指纹：否则下次构建因"缺指纹"再重编一遍，
      // 缓存就白命中了（指纹与是否重新编译无关，它是"这个对象是用什么标志编的"的说明）。
      (void)fs::write_text(
          flags_stamp_path(unit.object),
          compile_fingerprint(toolchain.kind, unit_flags_for(unit), profile_flags_list,
                              unit.third_party, pch_token_for(unit)));
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
  ST_LOG_INFO("并行编译 {} 路（{}），超大单元上限 {}", plan.jobs, plan.reason, plan.jobs_large);
  // 超大单元闸门：超阈值源文件同时最多 `jobs_large` 个在编（默认 1），
  // 避免"几个大块头叠在一起"这种最坏情形（即便它们的峰值与体积不成正比，串行化也钉住上界）
  const auto large_gate_limit = static_cast<std::ptrdiff_t>(plan.jobs_large);
  std::counting_semaphore<1024> large_gate(large_gate_limit);
  ThreadPool pool(workers);
  std::mutex error_mutex;
  std::string first_error;
  /// 本轮失败的全部源文件（去重后进失败汇总）。
  std::vector<std::string> failed_sources;
  std::atomic<std::size_t> done{0};

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
        args.push_back(std::format("-I{}", dir));
      }
      if (unit->third_party) args.push_back("-w");
      // PCH 只服务本工程/框架自己的 C++ 单元：
      // - C 源是另一套语言标准，不能吃；
      // - 框架单元跳过，保证其编译命令与引用方工程无关（对象缓存才能跨工程命中）；
      // - 第三方源码不吃：它们并不包含框架头，强制包含 PCH（`-include`）会改变其编译语义，
      //   且上游源码里"弃用但合法"的用法会被 `-Werror` 当成构建错误——那不是我们要修的东西。
      if (pch_ptr != nullptr && !is_c && !unit->framework_unit && !unit->third_party) {
        const std::vector<std::string> pch_args =
            pch_consume_args(pch_ptr->directory, pch_ptr->header);
        args.insert(args.end(), pch_args.begin(), pch_args.end());
      }
      // **先写临时文件，成功再改名到位**：编译被中断（OOM 杀掉编译器、磁盘写满）时
      // 产物位置不会留下半截 `.o`——它比源文件新，增量判新会当成最新，
      // 于是下一次构建报出一堆莫名其妙的链接错误（实测碰到两次）。
      //
      // 检查模式：只做到**语义分析**为止——不生成代码、不写对象、不写依赖清单。
      // 实测这是全量构建耗时的 44%（77 单元 16.5 s vs 全量 37 s），且不产生任何副作用。
      const std::string temporary_object = unit->object + ".tmp";
      const std::string temporary_depfile = unit->depfile + ".tmp";
      if (options.check_only) {
        args.push_back("-fsyntax-only");
        args.push_back(unit->source);
      } else {
        // 依赖产出：`-MMD -MF` 直接写 GCC 风格 `.d`——增量判新与共享对象缓存因此只有一套逻辑。
        args.push_back("-pipe");
        args.push_back("-MMD");
        args.push_back("-MF");
        args.push_back(temporary_depfile);
        args.push_back("-c");
        args.push_back(unit->source);
        args.push_back("-o");
        args.push_back(temporary_object);
      }
      if (options.verbose) {
        // dump **完整参数**（而不仅是标志集）：命令行是构建问题的第一现场，
        // 排查“为什么这个单元的行为与另一个不同”时，需要的正是实际传了什么。
        std::string flag_dump;
        for (std::size_t i = 1; i < args.size(); ++i) flag_dump.append(" ").append(args[i]);
        ST_LOG_INFO("compile: {}{} -> {}", fs::file_name(unit->source), flag_dump,
                  fs::file_name(unit->object));
      }
      // 编译子进程要带上工具链环境（当前两族都为空；保留这个字段是为了将来加族时
      // 有一个明确的落点，而不是到处补 env）。
      const process::Options process_options{.env = toolchain.env};
      auto result = process::run(args.front(), std::vector<std::string>(args.begin() + 1, args.end()),
                                 process_options);
      if (!result) {
        const std::scoped_lock lock(error_mutex);
        failed_sources.push_back(unit->source);
        if (first_error.empty()) first_error = result.error().to_string();
        return;
      }
      if (result->exit_code != 0) {
        // 失败即丢掉半截产物（否则会骗过下一次增量判新）
        (void)fs::remove_file(temporary_object);
        (void)fs::remove_file(temporary_depfile);
        const std::scoped_lock lock(error_mutex);
        // **全部失败都收**（不只首个）：多单元并发编译时，一次错误常连带
        // 污染若干文件（同一个头文件出错 → 所有引用者都失败）。只报首个会让
        // 人以为"就一个文件有问题"，改完再跑一轮才发现还有别的——AI 迭代里
        // 这一轮就是几分钟。这里收全，失败汇总时一次说清。
        failed_sources.push_back(unit->source);
        if (first_error.empty()) {
          first_error = std::format("编译失败: {}\n{}{}", unit->source, result->stdout_text,
                                    result->stderr_text);
        }
        return;
      }
      // 检查模式到此结束：没有产物要改名，也不写指纹/缓存（什么都没产出）。
      if (options.check_only) {
        ++done;
        return;
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
      // 写下本次编译用的**标志指纹**：下一次增量判新据此发现"标志变了"。
      // 写失败不影响本次构建（指纹缺失只会导致下次多编一次，不会出错）。
      (void)fs::write_text(
          flags_stamp_path(unit->object),
          compile_fingerprint(toolchain.kind, unit_flags, profile_flags_list,
                              unit->third_party, pch_token_for(*unit)));
      // 编译成功：写入共享缓存（失败不影响构建——缓存是加速手段，不是正确性前提）
      object_cache_store(object_cache, cache_key_for(*unit), *unit, cache_stores);
      ++done;
    });
  }
  pool.wait_idle();

  if (!first_error.empty()) {
    // —— 失败报告：首错（截断） + 失败清单 + 复现命令 ——
    //
    // 三段各自的理由：
    // ① **首错截断**：并行编译时首个错误后面常跟几十行级联错误（同一个头文件
    //    出错 → 所有引用者连锁），刷屏后真正要看的那几行被顶出视野。
    //    取前 40 行足够看清"错在哪"，完整内容仍可由编辑器打开文件看。
    // ② **失败清单**：只报首个会让人以为"就一个文件有问题"——改完再跑一轮
    //    才发现还有别的。一次说清，省一轮往返（AI 迭代里一轮就是几分钟）。
    // ③ **复现命令**：把"我该跑什么"直接给出，不用回忆参数（`--profile dev`
    //    这类细节靠记忆最容易错）。
    constexpr std::size_t kErrorHeadLines = 40;
    std::string report = first_error;
    std::size_t newline_count = 0;
    for (std::size_t index = 0; index < report.size(); ++index) {
      if (report[index] == '\n' && ++newline_count > kErrorHeadLines) {
        report.resize(index);
        report += std::format("\n  …（首错已截断至 {} 行；完整内容请看上面的文件）\n",
                              kErrorHeadLines);
        break;
      }
    }
    std::vector<std::string> unique_failed = failed_sources;
    std::ranges::sort(unique_failed);
    unique_failed.erase(std::unique(unique_failed.begin(), unique_failed.end()),
                        unique_failed.end());
    if (unique_failed.size() > 1) {
      report += std::format("\n本轮共 {} 个单元编译失败：\n", unique_failed.size());
      for (const auto& source : unique_failed) {
        report += std::format("  - {}\n", source);
      }
    }
    // `manifest.name` 就是目标名（`st.pkg` 的 name；一份清单一个目标）。
    report += std::format("\n复现：st build {} --profile {}\n", manifest.name, options.profile);
    return unexpected(ErrorCode::Invalid, report);
  }
  if (cache_hits > 0 || cache_stores.load() > 0) {
    ST_LOG_INFO("对象缓存：命中 {} · 新写入 {}", cache_hits, cache_stores.load());
  }
  return done.load();
}

/// 解析要链接的系统库（结构评审 P2：从 `link` 里抽出的纯计算段）。
///
/// 两条规则，都很容易被"顺手改错"：
/// 1. **交叉工具链声明了 `system_libs` 就整体接管**——同一份清单要同时服务多平台，
///    而"本机需要哪些系统库"（Linux 的 pthread/dl/m）对目标可能是错的甚至不存在
///    （mingw 没有 dl/m，链接直接失败）。接管后不再追加本机默认。
/// 2. **POSIX 专属库在 Windows 目标上要过滤**（同上：`cannot find -ldl`）。
[[nodiscard]] auto resolve_system_libraries(const Manifest& manifest,
                                            const ResolvedToolchain& toolchain)
    -> std::vector<std::string> {
  std::vector<std::string> libraries;
  const bool toolchain_takes_over = toolchain.cross() && !toolchain.system_libs.empty();
  if (!toolchain_takes_over) {
    const auto keep = [&toolchain](const std::string& lib) {
      return !(toolchain.platform == "windows" && (lib == "dl" || lib == "rt"));
    };
    for (const auto& lib : manifest.system_libs) {
      if (keep(lib)) libraries.push_back(lib);
    }
    for (const auto& lib : manifest.dependency_system) {
      if (keep(lib)) libraries.push_back(lib);
    }
    const auto defaults = default_system_libs(toolchain.platform);
    libraries.insert(libraries.end(), defaults.begin(), defaults.end());
  }
  libraries.insert(libraries.end(), toolchain.system_libs.begin(), toolchain.system_libs.end());
  return libraries;
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

  const std::vector<std::string> libraries = resolve_system_libraries(manifest, toolchain);

  std::vector<std::string> args;
  for (const auto& flag : profile_flags_list) args.push_back(flag);
  // 链接器：存在 mold/lld 时优先（链接是纯 I/O + 符号解析，lld 通常快 2~4 倍）。
  //
  // **交叉编译时跳过**：宿主装的 lld/mold 未必支持目标格式，交给交叉工具链自带的链接器最稳。
  // **Clang 也跳过**：它默认就用 LLVM 链接器，显式 `-fuse-ld=lld` 会让它去 PATH 里找
  // GNU 味的 `ld.lld`，与它自己选定的链接器不是一回事。
  if (!toolchain.cross() && toolchain.kind == CompilerKind::Gcc) {
    if (const auto lld = process::which("ld.lld"); lld.has_value()) {
      args.push_back("-fuse-ld=lld");
    } else if (const auto mold = process::which("mold"); mold.has_value()) {
      args.push_back("-fuse-ld=mold");
    }
  }
  // `-rdynamic`：把可执行文件的符号表导出到动态符号表——**崩溃栈能否显示函数名
  // 全靠它**（`backtrace_symbols` 只认动态符号表里的名字，否则只有裸地址）。
  //
  // 为什么值得付这个代价（产物略大、链接略慢）：无头应用崩溃时"能看见栈"与
  // "只有地址"是"能自己定位"与"得手工包 gdb 再来一遍"的区别——AI 开发闭环里
  // 后者意味着多一轮往返。实测代价：gbcode 产物 2.99MB → 3.1MB（+3.7%）。
  // 交叉编译/Windows 跳过（GNU 扩展，且 Windows 栈走 CaptureStackBackTrace+PDB）。
  if (!toolchain.cross() && toolchain.platform != "windows") {
    args.emplace_back("-rdynamic");
  }
  // 目标三元组：链接侧必须与编译侧**同一个**（否则拿 A 目标的运行库链 B 目标的对象）
  for (const auto& item : toolchain.target_args()) args.push_back(item);
  for (const auto& unit : units) args.push_back(unit.object);
  for (const auto& extra : extra_sources) args.push_back(extra);
  args.push_back("-o");
  args.push_back(output);
  const auto link_arguments = link_library_arguments(toolchain.platform, libraries);
  for (const auto& item : link_arguments) args.push_back(item);
  for (const auto& flag : toolchain.extra_flags) args.push_back(flag);
  // Windows 目标的**静态链接 C++ 运行库**（`-static-libgcc -static-libstdc++`）。
  // 不静态时产物要求 `libgcc_s_seh-1.dll`/`libstdc++-6.dll` 在 PATH 上——用户双击 exe
  // 或从别的目录启动时就报"找不到 dll"（与 mingw 交叉档同口径，见 st.pkg 的 extra_flags）。
  if (toolchain.platform == "windows") {
    for (const auto* flag : {"-static-libgcc", "-static-libstdc++"}) {
      if (std::ranges::find(args, flag) == args.end() &&
          std::ranges::find(toolchain.extra_flags, flag) == toolchain.extra_flags.end()) {
        args.emplace_back(flag);
      }
    }
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

/// 按**编译器族**收敛档位标志：去掉目标编译器不认识的选项。
///
/// 为何必须做：清单与档位表只写**一份跨平台标志**（CONVENTIONS §10.1 的既定取向），
/// 而 `-Wno-array-bounds` / `-Wno-stringop-overflow` / `-Wno-null-dereference` 是
/// **GCC 专有**的档标（它们对应 GCC 在"优化 + 系统头内联"下的误报，见 `profile_flags`
/// 的长注释）。Clang 侧的处理完全不同：
///
/// - `-Wstringop-overflow` 在 clang 里**不存在**；
/// - Clang 对未知的 `-Wno-*` 不是立刻报错，而是**在产生任何别的诊断时才补报**
///   `-Wunknown-warning-option`——于是它看起来"平时没事"，一旦某个单元有真问题，
///   报错列表里会多出这条无关的错，真正的原因被埋在后面（实测踩到）。
///   本项目 `-Werror` 全开，所以它就是**构建失败**。
///
/// 处理原则：**只去掉确知不被支持的**，不"顺手"把两族都认识的警告也关掉
/// （那会把真检查一起丢掉，比构建失败更糟）。
void strip_foreign_flags(std::vector<std::string>& flags, CompilerKind kind) {
  if (kind != CompilerKind::Clang) return;
  static constexpr std::string_view kGccOnly[] = {
      "-Wno-array-bounds",          // GCC 用它静默 SB 分析误报；clang 无此警告名
      "-Wno-stringop-overflow",     // 同上，clang 完全没有
      "-Wno-null-dereference",      // clang 叫 -Wnull-dereference 但只在特定检查下触发
      "-Wno-free-nonheap-object",   // clang 无此警告名
  };
  std::erase_if(flags, [](const std::string& flag) {
    return std::ranges::find(kGccOnly, flag) != std::end(kGccOnly);
  });
}

auto profile_flags(std::string_view profile) -> Result<std::vector<std::string>> {
  // 说明：优化档（dev/release）关闭 `-Wnull-dereference`——GCC 13 在本项目的
  // 「优化 + 预编译头」组合下会对 libstdc++ 内联代码（vector::insert 等）产生**误报**，
  // 而该警告在 -O0（debug 档）与 sanitizer 档（san，ASan/UBSan 实际检测）下仍然全部保留。
  //
  // **调试信息用 `-g1`（仅行号表）而非 `-g`**：迭代档（dev/quick）的目标是「看一眼栈回溯」，
  // 只需文件:行号，不需要逐变量单步的完整 DWARF；完整调试信息（局部变量/类型/内联展开）
  // 占编译时间的 25–33%、占对象体积 73%（实测 build.cpp：9.1 MB → 2.4 MB，9.6 s → 6.4 s）。
  // 全量调试信息留给显式的 `debug`/`san` 档：把便宜的默认给日常，把贵的给真需要的场合。
  // `-Warray-bounds` 与 `-Wnull-dereference` 是**同一类**："优化档 + libstdc++ 内联"下
  // GCC 13 的误报。实测形态：`std::string + "/" + leaf` 这种短串拼接会被它判成
  // "memcpy offset [32, 42] out of bounds"（把 SSO 缓冲区当成了目标边界），
  // 而 `operator+` 明明会重新分配。
  //
  // 后果比一条警告严重得多：**`release` 档（`-O2`）原先根本编译不过**
  // （`tests/raster_software_scene_test.cpp` / `text_glyph_integrity_test.cpp` 两处），
  // 等于四个构建档里有一档长期无法评估——而"发布档能不能编"正是最该被保证的事。
  // 用户侧的评估手段问题，与这条是同一个根：**没有人在跑它**。
  //
  // 口径：`-O0` 的档（quick/debug）**保留**该警告（那里不会误报，是真检查）；
  // 优化档关掉。真实越界由 `san` 档的 ASan 在运行期抓（那才是权威判据）。
  // 这一族是「GCC 13 在优化档下对**被内联的 libstdc++ 代码**做静态分析」的误报，
  // 不是一条两条：实测在 `release`（-O2）下依次撞到
  // `-Warray-bounds`（把 SSO 缓冲区当 memcpy 边界）与 `-Wstringop-overflow`
  // （写 27 字节到 16 字节区域——同一条 `std::string + "/" + x` 的另一种报法）。
  // 逐条追是打地鼠，故按**族**处理。
  //
  // 口径与既有 `-Wno-null-dereference` 完全一致（同一段注释上方已说明其性质）：
  // **只有 -O0 的档（quick/debug）保留这些静态分析警告**（那里不误报，是真检查）；
  // 优化档关掉，真越界交给 `san` 档的 ASan 在运行期抓。
  constexpr const char* kOptimizerFalsePositives[] = {"-Wno-null-dereference",
                                                      "-Wno-array-bounds",
                                                      "-Wno-stringop-overflow",
                                                      "-Wno-free-nonheap-object"};
  // 上列四条全是**GCC 专有**的档标（按族参数化见 `strip_foreign_flags`）。
  // 共同性质：**只在优化档 + libstdc++ 内联时触发，`-O0` 下不触发**——
  // 也就是说它们描述的是“优化器的推理路径”，不是源码里的真实缺陷。
  //
  // 最新一条（`-Wno-free-nonheap-object`）的取证：
  // `examples/gbcode/git_service.cpp` 在 `-O2` 下报
  // “释放了偏移 32 的指针”（`offset 32` 恰好是 libstdc++ 下 `std::string` 的大小），
  // 而该处只有一个 `std::vector<std::string>` 临时量的析构，返回值是 `optional<string>`
  // 值拷贝——没有任何路径会释放偏移指针。**同一文件同一告警集，`-O1` 干净、`-O2` 报错**。
  // 真越界由 `san` 档的 ASan 在运行期抓（那是判据，不是这里的静态推理）。
  // 它只出现在 release：`dev`/`san` 用 `-O1`，`quick`/`debug` 用 `-O0`。
  if (profile == "dev") {
    // 快速迭代档（默认）：-O1 兼顾编译速度与运行帧率（日常开发/无头验证用）
    return std::vector<std::string>{"-O1", "-g1", "-fno-omit-frame-pointer",
                                    kOptimizerFalsePositives[0], kOptimizerFalsePositives[1],
                                    kOptimizerFalsePositives[2]};
  }
  if (profile == "quick") {
    // 最速迭代档：-O0（单文件编译最快，适合"改一行看一眼"的内循环）
    return std::vector<std::string>{"-O0", "-g1"};
  }
  if (profile == "debug") {
    // 全量调试档：-O0 + 完整 `-g`（逐变量单步、完整类型信息）——只在真的需要调试时用
    return std::vector<std::string>{"-O0", "-g", "-fno-omit-frame-pointer"};
  }
  if (profile == "release") {
    return std::vector<std::string>{"-O2", "-DNDEBUG", kOptimizerFalsePositives[0],
                                    kOptimizerFalsePositives[1], kOptimizerFalsePositives[2],
                                    kOptimizerFalsePositives[3]};
  }
  if (profile == "san") {
    // sanitizer 档同样关掉两个"优化 + 系统头"下的 GCC 误报：
    // `-Wmaybe-uninitialized` 会在 libstdc++ `<regex>`（lint 规则用到）里对 `std::function`
    // 的控制块报"可能未初始化"；真实未初始化读由 UBSan/ASan 在运行期抓，防护强度不受影响。
    // 保留完整 `-g`：崩溃报告里的变量值是可行动信息，不属于"迭代速度"可牺牲的部分。
    return std::vector<std::string>{"-O1", "-g", "-fno-omit-frame-pointer",
                                    "-fsanitize=address,undefined", "-Wno-maybe-uninitialized",
                                    "-Wno-null-dereference", "-Wno-array-bounds",
                                    "-Wno-stringop-overflow"};
  }
  return unexpected(ErrorCode::Invalid, std::format("未知构建档位: {}", profile));
}

/// san 档的工具链前置检查：没有 sanitizer 运行时就在构建开始前报错。
///
/// Windows 的 MinGW 发行版多数**不带** `libasan`/`libubsan`——让它走到链接期是一片
/// “cannot find -lasan”（数十个单元各报一遍），而根因只有一行；在选档时就给可行动的提示。
[[nodiscard]] auto sanitizer_requirement_check(std::string_view profile,
                                               const ResolvedToolchain& toolchain) -> Status {
  if (profile != "san" || toolchain.sanitizers_available) return ok();
  return unexpected(
      ErrorCode::Unsupported,
      std::format("san 档需要 sanitizer 运行库（ASan/UBSan），而当前编译器不带：{}\n"
                  "  在 Linux/macOS 上用 g++/clang++ 跑 `st test --san`；"
                  "Windows 上改用 dev/debug 档，或换带 sanitizer 运行库的工具链",
                  toolchain.compiler_path));
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
  // 日志开关 → **编译期**裁掉（`ST_LOG_DISABLED`）。
  //
  // ⚠ 三个坑都踩过（都表现为"报构建完成、开关静默失效"）：
  //
  // ① **必须写在 `effective` 构造之后**。原先写在前面（往还不存在的对象里 push），
  //    随后被 `Manifest effective = manifest;` **整体覆盖**。
  // ② **走 `defines` 而不是 `flags`**：`compile_units` 取的是传进去的 `flags`
  //    （只汇总 `st.pkg` 的 `flags` 字段），往 `flags` 里塞的 `-D` 不进编译命令。
  // ③ **`defines` 是裸名**（`build_language_flags` 统一加 `-D` 前缀），
  //    所以只能写 `ST_LOG_DISABLED=1`——写成 `-DST_LOG_DISABLED=1` 会变成
  //    `-D-DST_LOG_DISABLED=1`（非法宏名，编译器不报错）。
  //
  // 三个坑都是**静默**的（不报错、只少一个宏），用 `--verbose` 逐字读命令行才看得出来——
  // 那次排查正是在命令行里发现"一条 `-DST_LOG_DISABLED` 都没有"。
  if (!manifest.log_enabled) effective.defines.push_back("ST_LOG_DISABLED=1");
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
  // 档位标志按**编译器族**收敛（见 `strip_foreign_flags`）：GCC 专有档标给 clang
  // 会在 `-Werror` 下直接变构建失败，而报错位置毫无指向性。
  strip_foreign_flags(*flags, toolchain->kind);
  if (auto status = sanitizer_requirement_check(options.profile, *toolchain); !status) {
    return forward_error(status.error());
  }
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
      // 目标级 `log` 覆盖工程级（`std::optional`：只有显式写了才覆盖）。
      if (spec->log_enabled.has_value() && *spec->log_enabled != manifest.log_enabled) {
        // 先摘掉工程级那条，避免同时出现相反的两个定义（裸名，见上方说明）。
        std::erase(effective.defines, std::string("ST_LOG_DISABLED=1"));
        if (!*spec->log_enabled) effective.defines.push_back("ST_LOG_DISABLED=1");
      }
      for (const auto& pattern : spec->exclude_sources) {
        const auto before = library_sources.size();
        std::erase_if(library_sources, [&pattern, &manifest](const std::string& path) {
          // 源文件列表是绝对路径：按「相对清单目录」匹配，模式才符合直觉（如 src/pkg/*）
          return fs::match_glob(pattern, fs::relative_to(path, manifest.directory));
        });
        if (before == library_sources.size()) {
          ST_LOG_WARN("排除模式未命中任何源文件: {}", pattern);
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
  // 框架单元专用的标志集（框架自己的头/宏/严格集）——它决定框架对象能否跨工程命中缓存。
  // **C 标志必须取 `framework->c_flags`**（框架清单的）：取 `manifest.c_flags` 在框架自建时
  // 恰好相等（同一个清单），只有独立工程引用时才暴露——见 `make_framework_flags` 的注释。
  std::optional<FrameworkFlags> framework_flags;
  if (framework.has_value()) {
    framework_flags = make_framework_flags(manifest, framework->include_dirs, framework->c_flags,
                                           *flags, *toolchain);
  }
  std::size_t rebuilt = 0;
  const std::int64_t compile_start = time::now_ns();
  auto compiled = compile_units(effective, options, units, *flags, *toolchain,
                                framework_flags.has_value() ? &*framework_flags : nullptr, rebuilt);
  if (!compiled) return forward_error(compiled.error());
  const std::int64_t compile_ms = (time::now_ns() - compile_start) / 1'000'000;

  // `st check` 的出口：用到 `build()` 为止的全套决策（单元枚举 / 框架引用 / 嵌入生成物 /
  // 第三方放宽 / 按族收敛标志 / 目标三元组 / 并发推导），只把最后一步换成“不产出”。
  //
  // **原地返回而不是再写一个 check()**：上面那几件事里有几件很容易只改到一半
  // （嵌入生成物与框架单元的标记尤其隐形），两套枚举一旦分叉，症状是
  // “检查过了但编不过”——那时人会先去怀疑别的东西。共用同一段除了省代码，
  // 更重要的是让两者**不可能**分叉。
  if (options.check_only) {
    BuildStats check_stats;
    check_stats.units_total = units.size();
    check_stats.units_rebuilt = rebuilt;              // 检查模式：这个值就是“实际检查的单元数”
    check_stats.units_cached = 0;
    check_stats.compile_ms = compile_ms;
    check_stats.elapsed_ms = (time::now_ns() - start_ns) / 1'000'000;
    const ConcurrencyPlan plan = plan_concurrency(options.jobs, options.jobs_large,
                                                  options.max_memory_mb, options.profile,
                                                  hardware_concurrency());
    check_stats.workers = plan.jobs;
    check_stats.workers_large = plan.jobs_large;
    check_stats.memory_budget_mb = plan.budget_mb;
    check_stats.concurrency_reason = plan.reason;
    check_stats.pch_used = false;                     // 检查模式不建也不消费 PCH
    return check_stats;
  }

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
    const std::int64_t target_compile_start = time::now_ns();
    auto target_compiled =
        compile_units(effective, options, target_units, *flags, *toolchain,
                      framework_flags.has_value() ? &*framework_flags : nullptr, target_rebuilt);
    if (!target_compiled) return forward_error(target_compiled.error());
    // 目标单元也是编译：计入编译耗时（否则它会潜进"链接"里，把主时段的账全打乱）
    stats.compile_ms += (time::now_ns() - target_compile_start) / 1'000'000;
    stats.units_total += target_units.size();
    stats.units_rebuilt += target_rebuilt;
    stats.units_cached = stats.units_total - stats.units_rebuilt;
  }

  // 产物后缀按目标平台：Windows 必须是 `.exe`（否则系统不认为它是可执行程序）
  const std::string output = fs::join(bin_dir, target->name + toolchain->executable_suffix);
  std::vector<CompileUnit> link_units = units;
  link_units.insert(link_units.end(), target_units.begin(), target_units.end());

  // 链接跳过：0 个单元重编 + 产物已存在且比全部目标文件新 → 不重链（无改动构建近乎瞬时）
  //
  // ⚠ **但只有"参与链接的东西"没变时才能跳**。原先只看"产物的时间戳比现有对象新"，
  // 于是**删掉一个源文件后永不重链**：被删的对象已不在 `link_units` 里，剩下的对象
  // 又都比产物旧 → 判为最新 → 产物里那部分代码**一直留着**。
  // 实测就是用户问的"示例程序怎么有 6MB"：移除 OpenGL 之后每个构建都报"重编 0"、
  // 从不重链，产物仍带着 GL 后端与 glad 单头编出来的近 3MB 代码；
  // 强制全量重建后立刻降到 2.99MB（release 是 2.95MB）。
  //
  // 因此把"上次链接用了哪些对象 + 什么标志"记成指纹，由它决定能否跳过。
  const auto link_fingerprint = [&]() -> std::string {
    std::string material(compiler_kind_name(toolchain->kind));
    for (const auto& unit : link_units) material.append("\n").append(unit.object);
    for (const auto& flag : *flags) material.append("\n@").append(flag);
    return std::string(st::hash::fnv1a64_hex(material));
  }();
  const std::string link_stamp_path = output + ".link";
  const bool up_to_date = [&]() {
    if (options.force || rebuilt != 0 || stats.units_rebuilt != 0) return false;
    const auto output_time = fs::modified_ns(output);
    if (!output_time) return false;
    // 参与链接的集合/标志变了（含"少了某个源文件"）→ 必须重链
    const auto stamp = fs::read_text(link_stamp_path);
    if (!stamp.has_value() || *stamp != link_fingerprint) return false;
    for (const auto& unit : link_units) {
      const auto object_time = fs::modified_ns(unit.object);
      if (!object_time || *object_time > *output_time) return false;
    }
    return true;
  }();
  if (up_to_date) {
    stats.artifact = output;
    stats.runs_on_host = toolchain->runs_on_host();
    stats.elapsed_ms = (time::now_ns() - start_ns) / 1'000'000;
    return stats;
  }

  const std::int64_t link_start = time::now_ns();
  auto linked = link(manifest, options, link_units, output, *flags, *toolchain, {});
  if (!linked) return forward_error(linked.error());
  stats.linked = true;
  stats.link_ms = (time::now_ns() - link_start) / 1'000'000;
  // 记下本次链接的"成分"，下次据此判断能否跳过（写失败只是下次多链一次，不影响正确性）
  (void)fs::write_text(link_stamp_path, link_fingerprint);
  stats.artifact = output;
  stats.runs_on_host = toolchain->runs_on_host();
  stats.elapsed_ms = (time::now_ns() - start_ns) / 1'000'000;
  return stats;
}

namespace {

/// 测试进程的 argv（分片无关的部分）。
[[nodiscard]] auto test_args(std::string_view filter, bool list_only, bool include_slow)
    -> std::vector<std::string> {
  std::vector<std::string> args;
  // `--list` 交测试进程处理（列出用例名后即退，不跑测试；filter 仍生效）
  if (list_only) args.push_back("--list");
  // 慢/环境敏感用例默认跳过（量机器性能的帧/吞吐门禁、需真编译的集成用例）——
  // 它们耗时长且在共享机器上偶发红灯，不该阻塞迭代内循环；`st test --slow` 显式开启。
  if (include_slow) args.push_back("--slow");
  if (!filter.empty()) args.push_back(std::string(filter));
  return args;
}

/// 运行测试（单进程或按分片并行），返回失败用例数。
///
/// ## 为何值得并行
///
/// 全套件 819 例：**CPU 27.5 s / 墙钟 33.6 s**——它只吃一个核，其余 27 核空转。
/// 而测试执行是迭代的最大单项成本（编译只占零头），所以这里把用例按注册顺序
/// 分成 `test_jobs` 片，每片一个独立进程。
///
/// ## 片数从哪来
///
/// `st test --test-jobs N`（默认 = 硬件并发的一半，上限 16）。为何不默认开满：
/// 套件里有一批**重型用例**（真光栅化文本、真连 TCP 的等待类），它们的耗时对 CPU
/// 竞争敏感——开满 28 路时**机时涨到 3.28×**（`text_*` 用例中位膨胀 3.2×），
/// 而墙钟反而比 16 片慢 12%（实测见 `docs/BUILD_TEST_PERF.md`）。`--test-jobs 1` 回到与分片前逐位等价的单进程。
///
/// ## 汇总
///
/// 每片写各自的 JUnit XML，父进程把它们**合并成一份**写回 `junit_path`
/// （否则后跑的子进程会覆盖前一片的报告，而 CI 只看到最后一片）。合并由
/// `st::pkg::merge_junit_files` 做（`include/st/pkg/junit.hpp`）。
[[nodiscard]] auto run_shards(const std::string& output, const std::string& framework_root,
                              const std::vector<std::string>& base_args, bool list_only,
                              std::string_view junit_path, std::size_t test_jobs) -> Result<int> {
  // 框架根经环境变量下发：**集成级用例**（`tests/pkg_integration_test.cpp`）要真建一个
  // 引用 framework 的最小工程来构建——而"框架在哪"只有构建驱动知道（它是从清单目录/可执行位置
  // 推出来的）。让测试进程去猜仓库布局会随目录结构变化而碎，也容易猜错后构造出假失败。
  std::map<std::string, std::string> base_env;
  if (!framework_root.empty()) base_env["ST_TEST_FRAMEWORK_ROOT"] = framework_root;

  if (list_only) {
    // `--list` 一次性列完，不分片：它的输出是给人/给 CI 看的清单（顺序即注册顺序）。
    process::Options run_options{.env = base_env, .capture_output = false};
    auto result = process::run(output, base_args, run_options);
    if (!result) return forward_error(result.error());
    return result->exit_code;
  }

  const std::size_t shards = test_jobs > 1 ? test_jobs : 1;
  if (shards == 1) {
    process::Options run_options{.env = base_env, .capture_output = false};
    if (!junit_path.empty()) run_options.env["ST_JUNIT_XML"] = std::string(junit_path);
    auto result = process::run(output, base_args, run_options);
    if (!result) return forward_error(result.error());
    return result->exit_code;
  }

  // —— 多片：每片一个临时 JUnit 文件，父进程合并后写回 junit_path ——
  const std::string shard_dir =
      fs::join(fs::temp_dir(), std::format("st-shards-{}", process::current_id()));
  // 清掉可能残留的旧分片报告：否则某片没写出文件时会把上一次的结果合进去
  (void)fs::remove_all(shard_dir);
  if (auto status = fs::create_directories(shard_dir); !status) {
    return forward_error(status.error());
  }
  std::vector<std::string> shard_reports(shards);
  for (std::size_t index = 0; index < shards; ++index) {
    shard_reports[index] = fs::join(shard_dir, std::format("shard-{}.xml", index + 1));
  }

  ST_LOG_INFO("测试分片：{} 片并行（每片一个测试进程；片内顺序不变）", shards);
  // 用线程池而不是裸 std::thread：与编译侧同一套并发原语（RAII 回收、可诊断）。
  // 子进程输出**不捕获**（直接继承终端）：分片不是为了给人看整齐的报告，
  // 捕获到内存再回显会让长跑期间看不到任何进展。
  ThreadPool pool(shards);
  std::mutex result_mutex;
  std::vector<int> exit_codes(shards, -1);
  std::vector<std::string> first_errors(shards);
  for (std::size_t index = 0; index < shards; ++index) {
    pool.submit([&, index]() {
      std::vector<std::string> args = base_args;
      args.push_back("--shard");
      args.push_back(std::format("{}/{}", index + 1, shards));
      process::Options run_options{.env = base_env, .capture_output = false};
      run_options.env["ST_JUNIT_XML"] = shard_reports[index];
      auto result = process::run(output, args, run_options);
      const std::scoped_lock lock(result_mutex);
      if (!result) {
        first_errors[index] = result.error().message;
        return;
      }
      exit_codes[index] = result->exit_code;
    });
  }
  pool.wait_idle();

  int failed_total = 0;
  for (std::size_t index = 0; index < shards; ++index) {
    if (exit_codes[index] < 0) {
      (void)fs::remove_all(shard_dir);
      return unexpected(ErrorCode::Internal,
                        std::format("测试分片 {} 未能启动: {}", index + 1, first_errors[index]));
    }
    failed_total += exit_codes[index];
  }
  if (!junit_path.empty()) {
    // 各片报告里的**失败用例数**由合并函数回填；返回值只用于日志（不改变退出码语义）
    std::size_t merged_failed = 0;
    if (auto status = merge_junit_files(shard_reports, junit_path, merged_failed); !status) {
      (void)fs::remove_all(shard_dir);
      return forward_error(status.error());
    }
    ST_LOG_INFO("JUnit 报告已合并（{} 片，失败用例 {}）", shards, merged_failed);
  }
  (void)fs::remove_all(shard_dir);
  return failed_total;
}

}  // namespace

auto run_tests(const Manifest& manifest, const BuildOptions& options, std::string_view filter,
               bool list_only, std::string_view junit_path, bool include_slow) -> Result<int> {
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
  // 档位标志按**编译器族**收敛（见 `strip_foreign_flags`）：GCC 专有档标给 clang
  // 会在 `-Werror` 下直接变构建失败，而报错位置毫无指向性。
  strip_foreign_flags(*flags, toolchain->kind);
  if (auto status = sanitizer_requirement_check(options.profile, *toolchain); !status) {
    return forward_error(status.error());
  }
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
    framework_flags = make_framework_flags(manifest, framework->include_dirs, framework->c_flags,
                                           *flags, *toolchain);
  }
  std::size_t rebuilt = 0;
  auto compiled = compile_units(effective, options, units, *flags, *toolchain,
                                framework_flags.has_value() ? &*framework_flags : nullptr, rebuilt);
  if (!compiled) return forward_error(compiled.error());
  ST_LOG_INFO("测试构建：{} 单元（重编 {}）", units.size(), rebuilt);

  const std::string output = fs::join(bin_dir, "st_tests" + toolchain->executable_suffix);
  auto linked = link(manifest, options, units, output, *flags, *toolchain, {});
  if (!linked) return forward_error(linked.error());

  std::vector<std::string> args = test_args(filter, list_only, include_slow);
  // 交叉产物仅在与宿主平台不同时拒绝执行（目标==宿主则照常跑，见 runs_on_host）
  if (!toolchain->runs_on_host()) {
    return unexpected(ErrorCode::Unsupported,
                      std::format("交叉编译产物无法在宿主执行: {}（请在目标平台运行）", output));
  }
  return run_shards(output, manifest.directory, args, list_only, junit_path, options.test_jobs);
}

}  // namespace st::pkg
