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
#include <thread>
#include <unordered_set>

#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/thread_pool.hpp"
#include "st/core/time.hpp"

namespace st::pkg {

// 内容哈希（实现见文件后部的 `content_hash_of`）。
// 前置声明在**命名空间外**：`serialize_deps_meta`（匿名命名空间内）要用它写第 4 列，
// 而 `file_content_hash` / `deps_entry_matches` 是公开 API（要有回归测试直接调），
// 实现必须在匿名命名空间之外 —— 两处共用同一个符号，不能各声明一个。
[[nodiscard]] auto content_hash_of(const std::string& path) -> std::optional<std::uint64_t>;
auto detect_compiler(std::string_view override_compiler) -> Result<std::string> {
  if (!override_compiler.empty()) return std::string(override_compiler);
  for (const auto name : {"ST_CXX", "CXX"}) {
    if (const auto value = fs::read_env(name); value.has_value() && !value->empty()) return *value;
  }
  // 两个前端**同一条口径**（GCC 风格标志 + libstdc++ 运行库），所以优先序只影响"用哪个前端"，
  // 不影响构建语义。g++ 在前：它对标准的支持随发行版走，且 Windows 上自带 libstdc++。
  for (const auto name : {"g++", "clang++", "c++"}) {
    if (auto found = process::which(name); found.has_value()) return *found;
  }
  return unexpected(ErrorCode::Unsupported,
                    "未找到 C++ 编译器：安装 MinGW-w64 的 g++，或 LLVM 的 clang++"
                    "（把所在目录加入 PATH，或用 ST_CXX / CXX 显式指定）");
}

namespace {

/// 源文件语言（决定编译器标志：`.c` 必须按 C 编译，不能用 `-std=c++XX`）。
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
///
/// ## ⚠ 冒号的判定必须限在「首个目标行」（2026-10-26 修，Windows 上的静默失效）
///
/// 原实现里 `if (raw == ':') { token.clear(); seen_colon = true; }` **无条件触发**，
/// 而 Windows 路径 `C:/Users/...` 的**盘符冒号**会命中它 —— 于是 `C:` 被当成
/// “目标/依赖分隔符”被丢掉，路径变成 `/Users/...`。
///
/// 后果是**静默且严重**的（实测：改了 `tests/pkg_memory_test.cpp` 而 `.o` 不重编）：
/// `serialize_deps_meta` 把这个坏路径写进 `.meta`，`deps_entry_matches` 读不到它
/// ⇒ 每条依赖每次都判“变了”（白重编）或恒判“没变”（漏重编），且**头文件改动检测一并失效**。
///
/// 定义在匿名命名空间**之外**：它是头文件里的公开 API（回归测试要直接调——
/// “解析层”缺陷跑一遍完整构建看不出来，只是“不重编”、不报错）。
}

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
  bool seen_colon = false;   // 已越过“目标:”分隔符
  // 盘符冒号（`C:`）不能当分隔符。
  //
  // GCC 的 `-MMD` 输出形如（行尾反斜杠续行）：
  //     `<obj>.tmp:` 续行  `<源文件路径>` 续行  `<头文件路径>`
  // 首行里出现**两个**冒号：盘符 `C:` 与目标分隔符。
  // 判据：分隔符后面一定跟空白或行尾；盘符冒号后面紧跟路径内容。
  // （旧实现不分青红蟹一律把冒号当分隔符，靠“顺手把目标行也吞掉”蒙对了个数，
  //   而代价是**所有依赖路径丢盘符**——那才是真正的缺陷。）
  // 注释里不写反斜杠+换行：`-Wcomment` 会把行尾的反斜杠当成续行，
  // 把下一行也卷进注释（编译报 `multi-line comment [-Werror=comment]`）。
  const auto is_target_separator = [&text](std::size_t at) {
    const std::size_t next = at + 1;
    if (next >= text->size()) return true;
    const char ch = (*text)[next];
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\\';
  };
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
    // 只在**还没越过目标分隔符**、且该冒号后面是空白/行尾/续行时，才把它当分隔符。
    // 盘符里的 `C:` 后面是路径字符 → 不是分隔符，照常规进 token。
    if (raw == ':' && !seen_colon && is_target_separator(index)) {
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

namespace {

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

/// 工程内快路径的依赖快照侧边车：`<对象>.meta`（与 `.flags` 同目录同寿命）。
///
/// 记录「这份 .o 编出来时」源文件与全部依赖的 (mtime, size)——快路径的
/// `needs_rebuild` 据此做 mtime+size 双校验（共享缓存 `object_cache_restore`
/// 读的 `unit.meta` 是同一套格式，见 `serialize_deps_meta`）。两个出生路径都写：
/// 真编译后、从共享缓存恢复后——只要 .o 在，它的出生描述就在。
[[nodiscard]] auto deps_meta_path(const std::string& object) -> std::string {
  return object + ".meta";
}

/// 依赖快照的反序列化目标：restore 与快路径共用。
///
/// ## 为什么是 `(mtime, size, hash)` 三元组而不是二元组（2026-10-25）
///
/// 原来只记 `(mtime, size)`，于是**同长度改动 + 保留 mtime** 必然判"没变"：
/// 实测把 `render.hpp` 改成 `rendEr.hpp`（等长）+ `os.utime` 回原值，
/// `st build` 报「编译 111ms / 链接已跳过」，而源文件已经是改过的代码；
/// `git status` 同时显示无改动（它也用 stat 缓存）。三方一起被骗。
///
/// 加一列内容哈希后，这个窗口被关掉。代价实测（`tools/deps_hash_cost_probe`，
/// 151 文件 / 3.9MB）：只 stat 5.59ms、读全部+fnv1a64 **18.43ms**、
/// 读全部+sha256 30.51ms。而真实构建的依赖总量是 43.6MB（去重后），
/// 对一次 7.4s 的构建约 +0.2s（2.7%）——**换掉一次误判导致的整轮重编就回本**。
///
/// 选 fnv1a64 而不是 sha256：这里要的是"内容变了没有"，不是抗碰撞证明。
/// 攻击者不参与构建；而 sha256 慢 1.7 倍，没有对应收益。
struct DepsEntry {
  std::int64_t mtime{0};
  std::uint64_t size{0};
  /// 内容哈希；空 = 旧格式（该条退回 mtime+size 判定）。
  std::uint64_t hash{0};
  bool has_hash{false};
};

using DepsMeta = std::map<std::string, DepsEntry>;

/// 依赖快照序列化：`path<TAB>mtime<TAB>size` 逐行，覆盖源文件与 `.d` 里的全部依赖。
///
/// 含制表符的路径**不记录**（也不引入转义语法）：`\t` 在 Windows 路径里是
/// `D:\tools` 这类真实子串，转义/反转义会把无辜路径改坏；而真实路径几乎不含
/// 制表符，跳过即可——该依赖退回 mtime-only（保守方向，不交付旧内容）。
/// 读不到 mtime/size 的路径同样不记（两侧对称：不记 = 不校）。
[[nodiscard]] auto serialize_deps_meta(const std::string& source, const std::string& depfile) -> std::string {
  std::string meta;
  std::set<std::string> seen{};   // 同一文件被多个单元引用时只算一次哈希
  const auto append_entry = [&meta, &seen](const std::string& path) {
    if (path.find('\t') != std::string::npos) return;
    if (!seen.insert(path).second) return;
    const auto time = fs::modified_ns(path);
    const auto size = fs::file_size(path);
    if (!time.has_value() || !size.has_value()) return;
    meta += path;
    meta += '\t';
    meta += std::to_string(*time);
    meta += '\t';
    meta += std::to_string(*size);
    // 第 4 列：内容哈希。算不出来就不写（旧行为，判定退回 mtime+size）。
    if (const auto digest = content_hash_of(path); digest.has_value()) {
      meta += '\t';
      meta += std::to_string(*digest);
    }
    meta += '\n';
  };
  append_entry(source);
  for (const auto& dep : parse_depfile(depfile)) append_entry(dep);
  return meta;
}

/// 依赖快照反序列化（与 `serialize_deps_meta` 对称）。空文本/损坏行 → 空表或
/// 缺行，调用方按"无记录不校验"处理——旧条目/旧对象不因此失效。
[[nodiscard]] auto parse_deps_meta(std::string_view text) -> DepsMeta {
  DepsMeta snapshot;
  for (const std::string_view line : st::split(text, '\n')) {
    if (line.empty()) continue;
    const std::size_t first = line.find('\t');
    if (first == std::string_view::npos) continue;
    const std::size_t second = line.find('\t', first + 1);
    if (second == std::string_view::npos) continue;
    const auto mtime = st::parse_i64(line.substr(first + 1, second - first - 1));
    // 第 3 列之后可能还有第 4 列（哈希）；旧格式没有 → 本轮构建前产出的 .meta
    // 仍能解析，该条按 `has_hash = false` 退回 mtime+size 判定（不阻塞构建）。
    const std::size_t third = line.find('\t', second + 1);
    const auto size_text = (third == std::string_view::npos)
                               ? line.substr(second + 1)
                               : line.substr(second + 1, third - second - 1);
    const auto size = st::parse_u64(size_text);
    if (!mtime.has_value() || !size.has_value()) continue;
    DepsEntry entry{};
    entry.mtime = *mtime;
    entry.size = *size;
    if (third != std::string_view::npos) {
      if (const auto hash = st::parse_u64(line.substr(third + 1)); hash.has_value()) {
        entry.hash = *hash;
        entry.has_hash = true;
      }
    }
    snapshot.emplace(std::string(line.substr(0, first)), entry);
  }
  return snapshot;
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
  // ## 为何不在这里“mtime 比 .o 新 ⇒ 直接重建”
  //
  // 原实现是这么写的，代价实测（2026-10-26）：
  // 仅 `touch` 一个被 33 个单元依赖的头（内容一字未改）⇒ **重编 34 个单元、发 20.3 s**。
  // 而“元数据变了但内容没变”在真实工作流里很常见：
  // · `git checkout` / `stash pop` 切分支（mtime 常被整批刷新）
  // · `touch` / 构建脚本批量更新时间戳
  // · 编辑器原子保存（写临时文件再 rename）
  // 这 20 秒是**纯浪费**：编出来的对象与已有的逐字节相同。
  //
  // 所以 mtime 不再参与“是否重建”的判定——**内容哈希是唯一真相源**。
  // 唯一例外：`.meta` 侧边车缺失（本改动前构建的旧对象）时没有内容可依，
  // 那时退回 mtime-only（与改动前一致），否则那些对象的依赖会被全量重算一遍。
  if (!fs::is_regular_file(deps_meta_path(unit.object))) {
    if (*source_time > *object_time) return true;
    for (const auto& dep : parse_depfile(unit.depfile)) {
      const auto dep_time = fs::modified_ns(dep);
      if (dep_time && *dep_time > *object_time) return true;
      if (!dep_time && !fs::exists(dep)) return true;  // 头被删除 → 重编
    }
    return false;
  }
  // **mtime+size 双校验（快路径）**：上面纯 mtime 判定抓不住「内容变了但 mtime
  // 没变」的窗口（同秒改+建、拷贝保留旧 mtime、时钟回拨）——旧对象一声不响地
  // 继续链接。侧边车 `<obj>.meta` 记着这份 .o 出生时的源+依赖 (mtime, size)，
  // 严格不等比对（`!=` 而非 `>`：回拨后的 mtime 比 .o 还旧，`>` 抓不住）。
  // 与 restore 路径同一套序列化（`parse_deps_meta`），两路判定语义一致。
  // 缺侧边车（本改动前构建的旧对象）→ 退回 mtime-only，不阻塞构建。
  if (const auto meta_text = fs::read_text(deps_meta_path(unit.object)); meta_text.has_value()) {
    const DepsMeta snapshot = parse_deps_meta(*meta_text);
    // ## 判定：mtime/size 一致时**还要比内容哈希**（2026-10-25）
    //
    // 少了这一步就有那个窗口：同长度改动 + 保留 mtime（`copy2` / `cp -p` /
    // 编辑器"保留时间戳" / `os.utime` 回写）→ 判"没变" → 拿旧产物当新结果，
    // 而构建照样报"编译 111ms"。实测过，`git status` 也一起被骗。
    //
    // 哈希**只在这一步现算**：mtime 或 size 任一不同就已判"变了"，直接返回，
    // 不读内容。所以日常构建（改了文件）不付这笔成本；只有"看起来没变"的
    // 文件才读一次——而它们本来就是要被跳过的那批，读 43.6MB 约 0.2s。
    const auto changed = [&](const std::string& path) -> bool {
      const auto found = snapshot.find(path);
      if (found == snapshot.end()) return false;   // 无记录不校验（对称：写入侧不记）
      const auto& entry = found->second;
      const auto recorded = entry.has_hash ? std::optional<std::uint64_t>(entry.hash)
                                           : std::nullopt;
      return !deps_entry_matches(path, entry.mtime, entry.size, recorded);
    };
    if (changed(unit.source)) return true;
    for (const auto& dep : parse_depfile(unit.depfile)) {
      if (changed(dep)) return true;
    }
    // ## 依赖文件被删除 ⇒ 必须重编
    //
    // `deps_entry_matches` 对“读不到”会返回 false（判“变了”）——那条路径已经覆盖了。
    // 但**源文件被删**（而不是头）时 `changed()` 同样返回 true（读不到），也对。
    // 这里额外显式检查一次：`.meta` 里有记录、而文件已经不存在 —— 这是
    // “删了头但没重编”的典型场景（旧实现靠 `!dep_time && !fs::exists(dep)` 抓）。
    if (!fs::is_regular_file(unit.source)) return true;
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
/// 没有依赖比缓存对象新（mtime）且侧边车 `unit.meta` 记录的 (mtime, size) 都
/// 未变，缓存对象才是最新的（两路共用 `parse_deps_meta`，不引入第二套判定逻辑
/// ——否则"增量构建"与"缓存"迟早会在边界条件上给出不同答案）。
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
  // mtime+size 双校验（BACKLOG「对象缓存对头文件变化的失效范围」修复；旧条目无
  // .meta 时按 mtime-only 放行——见下方逐处注释）。
  const std::string cached_meta = fs::join(directory, "unit.meta");
  // 解析与序列化是共用的一对（`parse_deps_meta`/`serialize_deps_meta`——工程内
  // 快路径侧边车 `<obj>.meta` 与共享缓存 `unit.meta` 同一套格式）；解析失败/
  // 空文本 = 无记录不校验，旧条目不因此失效。
  const DepsMeta snapshot = parse_deps_meta(fs::read_text(cached_meta).value_or(""));
  const bool has_meta = !snapshot.empty();
  // 依赖新鲜度：源文件与 `.d` 里的每个依赖都不得比缓存对象新；有元数据时
  // **再加 mtime+size 严格比对**——mtime 窗口（同秒改+建、拷贝保留旧 mtime、
  // 时钟回拨）里内容变了 mtime 大概率也变，兜住绝大多数静默旧对象顶替。
  // 等长改写仍漏（那要内容哈希才能根治，见 BACKLOG 观察方向）。无记录（旧条目）不校验。
  const auto stale = [&](const std::string& path) -> bool {
    const auto dep_time = fs::modified_ns(path);
    if (!dep_time) return !fs::exists(path);   // 头被删除 → 重编
    if (*dep_time > *cached_time) return true;
    if (has_meta) {
      if (const auto found = snapshot.find(path); found != snapshot.end()) {
        // mtime **严格等于记录值**才放行：`>` 比对抓不住时钟回拨（回拨后改的文件
        // mtime 比缓存对象还旧，看起来"更不新"），`!=` 抓得住。误伤面仅 touch/
        // 跨文件系统拷贝带来的 mtime 抖动（FAT 2s 粒度）——多编一次后重写
        // .meta 即恢复，保守方向不交付旧内容。
        // 与增量快路径**共用同一份判定**（含内容哈希）：
        // 否则"增量构建"与"共享缓存"会在边界条件上给出不同答案，
        // 而同长度改动 + 保留 mtime 正是那种边界。
        const auto& entry = found->second;
        const auto recorded =
            entry.has_hash ? std::optional<std::uint64_t>(entry.hash) : std::nullopt;
        if (!deps_entry_matches(path, entry.mtime, entry.size, recorded)) return true;
      }
    }
    return false;
  };
  if (stale(unit.source)) return false;
  for (const auto& dep : parse_depfile(cached_depfile)) {
    if (stale(dep)) return false;
  }
  if (auto status = fs::copy_file(cached_object, unit.object); !status) return false;
  if (auto status = fs::copy_file(cached_depfile, unit.depfile); !status) return false;
  // **restore 后把 `.o` 的 mtime 置为现在**。
  //
  // 为何必需：Win32 `CopyFile`（`std::filesystem::copy_file` 在 Windows 上的实现）
  // **保留源文件时间戳**。于是缓存 restore 出来的对象“内容是新条目的，mtime 却停在
  // 缓存条目创建的那一刻”。下游任何按时间比较的判据都会被误导，典型是
  // 链接跳过里的“.o 比产物旧吗”——旧 mtime 让它放行，于是**产物停留在旧代码上**。
  //（链接指纹已同时加入 (mtime, size)，这里再 touch 一次，让两套判据一致、
  //  且“刚构建过”这个语义在时间戳上也成立。）
  (void)fs::touch_now(unit.object);
  // **同步落工程内侧边车**：restore 复制回来的对象进入快路径（下一轮
  // `needs_rebuild` 直接判它），侧边车让快路径读到同一套 (mtime, size) 快照——
  // 两条路径一个真值源。旧缓存条目（无 .meta，快照空）时这里不写：该对象走
  // 快路径 mtime-only（与历史行为一致，不阻塞构建）。
  if (has_meta) {
    if (const auto meta_text = fs::read_text(cached_meta)) {
      (void)fs::write_text(deps_meta_path(unit.object), *meta_text);
    }
  }
  return true;
}

/// 编译成功后写入缓存（失败不影响构建：缓存是加速手段，不是正确性前提）。
///
/// 元数据 `unit.meta`（BACKLOG 修复）：逐行 `path<TAB>mtime<TAB>size`（格式与
/// 含制表符路径的处理见 `serialize_deps_meta`），覆盖源文件与 `.d` 里的全部依赖
/// ——restore 时 mtime+size 双校验。
auto object_cache_store(const ObjectCache& cache, std::string_view key, const CompileUnit& unit,
                        std::atomic<std::size_t>& stores) -> void {
  if (!cache.enabled()) return;
  if (!fs::is_regular_file(unit.object) || !fs::is_regular_file(unit.depfile)) return;
  const std::string directory = fs::join(cache.root, key);
  if (auto status = fs::create_directories(directory); !status) return;
  // 元数据先行：对象落盘前的窗口里，restore 看到的是"新 .o + 无/旧 .meta"，
  // 后者只会退化到 mtime-only（保守方向——多编一次，不交付旧内容）。
  if (auto status = fs::write_text(fs::join(directory, "unit.meta"),
                                   serialize_deps_meta(unit.source, unit.depfile)); !status) {
    return;
  }
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
///
/// **单一数据源：仓库根的 `link-libs.txt`**（bootstrap.ps1/sh 与本函数共用同一份）。
/// 历史教训：清单曾在自举脚本×2 / 框架清单 / 交叉 toolchains / 本函数五处各写一份，
/// 漏过三次（dbghelp×2、secur32×1），每次都是「报错指向源码的 undefined reference」
/// ——排查方向被带偏。收敛后改一处即全局生效。
///
/// 读不到该文件时（独立工程把框架 vendor 出去，不带仓库根文件）退回内置同值
/// 兑底——口径仍一致（下方的 `kFallbackLibs*` 与 link-libs.txt 内容同步维护）。
[[nodiscard]] auto default_system_libs(std::string_view platform) -> std::vector<std::string> {
  // 查找 link-libs.txt：从当前目录向上最多三级（框架根 / 独立工程的 vendor 布局）。
  const std::string wanted = platform == "windows" ? "windows" : "posix";
  for (int depth = 0; depth < 3; ++depth) {
    std::string prefix;
    for (int up = 0; up < depth; ++up) prefix += "../";
    const auto content = fs::read_text(prefix + "link-libs.txt");
    if (!content.has_value()) continue;
    std::vector<std::string> libs;
    bool in_section = false;
    for (const std::string_view line : split(*content, '\n')) {
      std::string_view trimmed = line;
      while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.remove_prefix(1);
      }
      if (trimmed.empty() || trimmed.front() == '#') continue;
      if (trimmed == "windows:") { in_section = wanted == "windows"; continue; }
      if (trimmed == "posix:") { in_section = wanted == "posix"; continue; }
      if (in_section) {
        std::string lib(trimmed);
        if (!lib.empty() && lib.back() == '\r') lib.pop_back();
        libs.push_back(std::move(lib));
      }
    }
    if (!libs.empty()) return libs;
  }
  // 兑底（与 link-libs.txt 同步维护；独立工程场景）。
  // Windows：`nanosrv` 是 MinGW 里 dbghelp 的真身存根（壳库只转发）；
  // POSIX：dl 供 TLS 的运行时 dlopen OpenSSL。
  if (platform == "windows") {
    return {"ws2_32", "user32", "gdi32", "shell32", "dbghelp", "nanosrv", "psapi",
            "secur32", "winpthread"};
  }
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
  // 语言标准由**框架清单**的 `cxx_standard` 派生（不是调用方工程的）：框架源是按框架自己
  // 声明的标准写的，用调用方的标准去编它会造成"同一份源码按不同标准编出不同对象"，
  // 共享对象缓存的键也就随工程漂移。清单没写时 `parse_json` 已经填入缺省值，
  // 这里的空串分支只防手段性直构 `Manifest`（如测试/嵌入方）绕过了解析。
  const auto standard_flag = Manifest::cxx_standard_flag(framework_manifest.cxx_standard);
  out.flags.push_back(standard_flag.empty() ? std::format("-std=c++{}", Manifest::kDefaultCxxStandard)
                                            : standard_flag);
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
  // 语言标准：来自**本工程清单**的 `cxx_standard`（构建侧唯一真源）。
  // 它必须排在最前：后随的档位/工程 flags 可能再给 `-std=`，按编译器"后者覆盖前者"的
  // 惯例，那样才轮得到工程显式覆盖；但缺省情况下清单值就是最终生效值。
  const auto standard_flag = Manifest::cxx_standard_flag(manifest.cxx_standard);
  out.cxx.push_back(standard_flag.empty()
                        ? std::format("-std=c++{}", Manifest::kDefaultCxxStandard)
                        : standard_flag);
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
    if (!is_c && unit.source.contains("simd.cpp")) {
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

  // ## 增量判定并行化（2026-10-26）
  //
  // ## 为何必须并行
  //
  // `needs_rebuild` 是**纯读**的判定（看 `.flags` / `.meta` / 依赖头 mtime+size，
  // 必要时读内容算哈希），但原先是在一个串行 for 里逐个调。
  // 实测代价：`st build --no-tests` 2.0s，而 `st test <filter>` 10.2s——
  // 差的 8s 就在“多出 75 个测试单元的增量判定”上（每单元读 `.d` + 几个依赖头，
  // 看起来没变时还要读 42.6MB 内容算 FNV 哈希）。
  //
  // 判定之间**没有依赖**（每个单元只看自己的 `.o`/`.d`/`.meta`），
  // 所以可以并发。这不改任何语义，只是把串行的读盘扇开。
  //
  // `needs_rebuild` 内部无共享可写状态（`content_hash_cached` 自带 mutex），
  // 因此并行安全；并发度沿用本函数的 `workers` 口径。
  std::vector<std::uint8_t> needs(units.size(), 0);
  {
    std::atomic<std::size_t> next{0};
    const std::size_t lane_count = std::max<std::size_t>(1, units.size());
    const std::size_t lanes = std::min<std::size_t>({lane_count, 16, std::thread::hardware_concurrency() == 0
                                                                          ? std::size_t{4}
                                                                          : static_cast<std::size_t>(std::thread::hardware_concurrency())});
    std::vector<std::thread> workers{};
    workers.reserve(lanes);
    for (std::size_t lane = 0; lane < lanes; ++lane) {
      workers.emplace_back([&]() {
        while (true) {
          const std::size_t index = next.fetch_add(1);
          if (index >= units.size()) return;
          const CompileUnit& unit = units[index];
          if (options.check_only || options.force) {
            needs[index] = 1;   // 检查/强制模式：全部要做
            continue;
          }
          needs[index] = needs_rebuild(unit, unit_flags_for(unit), profile_flags_list,
                                       toolchain.kind, pch_token_for(unit))
                             ? 1
                             : 0;
        }
      });
    }
    for (std::thread& worker : workers) worker.join();
  }

  for (std::size_t unit_index = 0; unit_index < units.size(); ++unit_index) {
    const CompileUnit& unit = units[unit_index];
    // 检查模式：**全部单元都查**——不做增量判定，也不查共享对象缓存。
    // 理由：这里的两个跳过机制判的都是“`.o` 是不是还要重编”，而检查模式根本不产出 `.o`。
    // 若沿用它们，一个“已构建过、源码未变”的仓库会**一个单元都不查**，
    // 于是 `st check` 直接报成功——一个恒绿的检查比没有检查更糟。
    const bool must_check = options.check_only;
    if (!must_check && !options.force && needs[unit_index] == 0) {
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

  // ## 提交顺序：**大的先发**（LPT，Longest Processing Time first）
  //
  // 原先写的是 `stable_partition` 把小单元放前、大单元放后，理由写的是
  // “小单元先跑满并发，大块头收尾时独占闸门”。**那句话的后果正好相反**：
  // 大块头被挤到队尾，前面 50 个小单元 2s 内跑完，剩下 26 个核只能看
  // 一两个巨无霸（sqlite3.c 20.1s、quickjs.c 14.2s）单独吃一个核——
  // 实测平均并行度 5.59/28、全量墙钟 42.29s，而串行总和只 194.8s（÷28 = 6.96s）。
  //
  // 正解是经典多机调度结论（LPT）：**按预计耗时降序提交**。大块头一开头就开跑，
  // 它们的耗时长尾被后续小单元填满；末尾只剩小单元，尾巴短。
  //
  // 预计耗时用**源文件字节数**作代理（编译时间与字节数强相关：
  // 实测 20.1s 的 sqlite3.c 是 8MB 级、9.6s 的 build.cpp 是 108KB）。
  // 不用真实计时：首轮没有历史，先试编一遍得不偿失。
  if (pending.size() > 1) {
    // 预计算大小与「是否大单元」：排序比较器会被调 O(n log n) 次，
    // 在里现查 file_size / is_large_unit 等于把 syscall 放进排序内循环。
    struct Rank {
      const CompileUnit* unit{nullptr};
      bool large{false};
      std::uintmax_t bytes{0};
    };
    std::vector<Rank> ranked;
    ranked.reserve(pending.size());
    for (auto* unit : pending) {
      const auto bytes = fs::file_size(unit->source).value_or(0);
      ranked.push_back(Rank{unit, bytes >= kLargeUnitBytes, bytes});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Rank& left, const Rank& right) {
      // 超大单元**恒**排最前：它们既是耗时最长的一批，又受闸门限制
      // （晚提交 = 晚排队 = 尾巴更长）。同类按体积降序（LPT）。
      if (left.large != right.large) return left.large;
      return left.bytes > right.bytes;
    });
    for (std::size_t index = 0; index < ranked.size(); ++index) pending[index] = ranked[index].unit;
  }

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
      // 依赖快照侧边车（快路径 mtime+size 双校验用）：与共享缓存 `unit.meta` 同一套
      // 序列化。写失败同样不影响本次构建（缺侧边车 → 快路径退回 mtime-only）。
      (void)fs::write_text(deps_meta_path(unit->object),
                           serialize_deps_meta(unit->source, unit->depfile));
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
    // 清单顶层的 `system_libs` 可能是**跨平台并集**（本仓库的 st.pkg 就是：
    // Windows 宿主上维护，Windows 节 + POSIX 节写在一起）——不能原样交给链接器。
    // 规则：**按目标平台过滤**，而不是枚举例外（旧代码只对 windows 目标滤 dl/rt，
    // POSIX 目标上把 dbghelp/nanosrv/secur32 全部传出——Linux 宿主上
    // `st build gbcode` 链接时才现形：unable to find library -ldbghelp）。
    // 权威清单就是 link-libs.txt（default_system_libs 读它）：只放行**两节交集
    // 之外、清单里明确写了且当节认可**的库。实现：目标节的库集合 + 非节内库
    // （工程自加的依赖库名，如 sqlite3）照放——判断依据是“名字是否出现在
    // link-libs.txt 任一节”里：出现过的就是平台库，按目标节取舍；没出现过的
    // 是工程自己声明的依赖，与平台无关，保留。
    const auto defaults = default_system_libs(toolchain.platform);
    const auto other_platform_libs = default_system_libs(
        toolchain.platform == "windows" ? "posix" : "windows");
    const auto keep = [&defaults, &other_platform_libs](const std::string& lib) {
      // 工程自声明库：不在任何一个平台节里 → 与平台无关，保留。
      const bool platform_lib =
          std::ranges::find(defaults, lib) != defaults.end() ||
          std::ranges::find(other_platform_libs, lib) != other_platform_libs.end();
      if (!platform_lib) return true;
      // 平台库：只留目标平台的（Windows 目标滤 POSIX 库，POSIX 目标滤 Windows 库）。
      return std::ranges::find(defaults, lib) != defaults.end();
    };
    for (const auto& lib : manifest.system_libs) {
      if (keep(lib)) libraries.push_back(lib);
    }
    for (const auto& lib : manifest.dependency_system) {
      if (keep(lib)) libraries.push_back(lib);
    }
    libraries.insert(libraries.end(), defaults.begin(), defaults.end());
  }
  libraries.insert(libraries.end(), toolchain.system_libs.begin(), toolchain.system_libs.end());
  return libraries;
}

/// Windows `CreateProcessW` 的命令行上限（字符，含结尾 NUL）。
/// POSIX 的 `execve` 按**单个参数**计（`ARG_MAX`，通常 2MB），量级完全不同。
inline constexpr std::size_t kWindowsCommandLineLimit = 32767;
/// 保守余量：命令行里还要放程序路径、`-o` 目标、旗标与引号。
/// 估算超过「上限 - 余量」就用响应文件——宁早不晚（晚一次就是一次构建失败）。
inline constexpr std::size_t kCommandLineHeadroom = 2048;

/// 估算「把这些参数拼成命令行」要多少字符（逐个加引号 + 分隔空格，同 Windows 口径）。
///
/// 为何不真的拼出来再量：拼一次是 O(参数数) 的字符串拷贝，而链接参数上千个；
/// 而这里只需要量级判断（差几个字符不影响结论）。
[[nodiscard]] auto estimate_command_line(const std::string& program,
                                         const std::vector<std::string>& args) -> std::size_t {
  std::size_t total = program.size() + 2;   // 程序路径 + 空格 + 结尾 NUL
  for (const auto& arg : args) {
    total += arg.size() + 3;   // 引号 + 空格
  }
  return total;
}

/// 用**响应文件**重试一次链接（`g++ @file`——GCC/Clang 的通用机制）。
///
/// 写到哪里：产物同目录的同名 `.rsp`（放在 `build/` 下，与产物同生命周期）。
/// 不写临时目录：路径更短、且出问题时能直接打开看（临时目录里的会先被清）。
///
/// `long_args` 是要进文件的那些（**对象文件与额外源**这类“一参数一项”的路径列表），
/// `head`/`tail` 留在命令行（旗标、`-o 产物`、库）——这样即使响应文件的换行处理
/// 有差异，链接行为的语义也不会变（旗标顺序与位置敏感）。
///
/// ⚠ 这里**必须由调用方显式给出哪些参数进文件**，不能靠“长得像路径”去猜：
/// 第一版就是猜的（“含 `.o` 或 `build/` 就当对象文件”），而 `-o` 的产物路径
/// 恰好也含 `build/`——于是它被挪进了响应文件，命令行里 `-o` 没了目标，
/// 链接器报 `could not open .../st_tests.exe: no such file or directory`。
/// 那个报错看着像“产物不存在”，而真因是**搞错了哪个参数属于哪一边**。
[[nodiscard]] auto link_with_response_file(const ResolvedToolchain& toolchain,
                                           const std::vector<std::string>& head,
                                           const std::vector<std::string>& long_args,
                                           const std::vector<std::string>& tail,
                                           const std::string& output,
                                           const std::string& cwd)
    -> Result<process::RunResult> {
  const std::string response_path = output + ".rsp";
  // 响应文件的换行规则：GCC 把**空白**当分隔（不是 shell 的引号规则），
  // 所以一参数一行最稳（含空格的路径不需要引号，GCC 会原样吃整行）。
  std::string body;
  for (const auto& arg : long_args) {
    body += arg;
    body += '\n';
  }
  if (body.empty()) return unexpected(ErrorCode::Invalid, "响应文件无可写内容（调用方参数错）");
  if (auto written = fs::write_text(response_path, body); !written) {
    return forward_error(written.error());
  }
  std::vector<std::string> command_line_args = head;
  command_line_args.push_back("@" + response_path);
  command_line_args.insert(command_line_args.end(), tail.begin(), tail.end());
  return process::run(toolchain.compiler_path, command_line_args,
                      process::Options{.cwd = cwd, .env = toolchain.env});
}

// 匿名 namespace 内的 link() 用下方的占用判定（定义在 st::pkg 层供测试直连）。

/// 把 Windows 图标资源编成对象文件（`RT_GROUP_ICON` + `RT_ICON`）。
///
/// 为何走“生成 `.rc` 再编”而不是直接调 `windres`：
/// - `g++ x.rc` 会自己找 `windres`（MinGW 装里就带），交叉工具链前缀也自动对——
///   少一条“用户得先装 windres 并配 PATH”的隐形成本；
/// - 直接调 `windres` 要多维护一套工具链路径解析。
///
/// 产物落在产物同级的 `.res/` 下（而不是 build 根的固定位置）：图标属于**目标**，
/// 同名不同目标不能互相覆盖（否则 gallery 与 gbcode 会抢同一个 `icon.o`）。
///
/// 幂等：`.rc` 内容与时间戳都比对象新时才重编——图标很少变，没必要每轮链接都跑。
[[nodiscard]] auto compile_windows_resource(const TargetSpec& target, const BuildOptions& options,
                                           const ResolvedToolchain& toolchain,
                                           const std::string& output)
    -> Result<std::string> {
  const std::string root = options.root.empty() ? std::string{"."} : options.root;
  const std::string icon_path = root + "/" + target.icon;
  if (!fs::is_regular_file(icon_path)) {
    return unexpected(ErrorCode::NotFound,
                      std::format("目标 {} 声明的图标不存在：{}", target.name, target.icon));
  }
  // `.ico` 路径在 `.rc` 里要写成 Windows 路径（反斜杠 + 转义）。
  std::string rc_icon = icon_path;
  for (auto& character : rc_icon) {
    if (character == '/') character = '\\';
  }
  std::string escaped;
  for (const char character : rc_icon) {
    if (character == '\\') escaped += "\\\\";
    else escaped += character;
  }
  // `1 ICON` 里的 1 是资源 ID，要小于 0x8000 才允许由应用自己分配（编译器口径）。
  const std::string rc_source = std::format("1 ICON \"{}\"\n", escaped);

  const std::string resource_dir = output + ".res";
  if (auto status = fs::create_directories(resource_dir); !status) {
    return forward_error(status.error());
  }
  const std::string rc_path = resource_dir + "/icon.rc";
  const std::string object_path = resource_dir + "/icon.o";

  bool needs_rebuild = !fs::is_regular_file(object_path);
  if (!needs_rebuild) {
    // 判据只看 **`.rc` 内容是否变了**（图标路径或 ID 换了）。
    // 为何不靠时间戳：框架没有暴露 stat 的 mtime，而“内容相等就不重编”
    // 在语义上更准——它直接回答“这份 `.rc` 换了吗”。
    auto existing = fs::read_text(rc_path);
    needs_rebuild = !existing || *existing != rc_source;
  }
  if (needs_rebuild) {
    if (auto status = fs::write_text(rc_path, rc_source); !status) {
      return forward_error(status.error());
    }
    // `-c` 不可省：`g++ x.rc -o x.o` 默认按**链接**处理（不编译），
    // 于是它拿 `x.rc` 当输入文件去链接，报 `ld returned 1 exit status`。
    // 加上 `-c` 才是“只编译不链接”，产物才是真正的对象文件。
    std::vector<std::string> command{"-c", rc_path, "-o", object_path};
    auto built = process::run(toolchain.compiler_path, command,
                             process::Options{.cwd = root, .env = toolchain.env});
    if (!built) return forward_error(built.error());
    if (built->exit_code != 0) {
      std::string shown;
      for (const auto& part : command) shown += (shown.empty() ? "" : " ") + part;
      return unexpected(ErrorCode::Internal,
                        std::format("编译 Windows 图标资源失败\n{}{}\n{}",
                                    built->stdout_text, built->stderr_text, shown));
    }
  }
  return object_path;
}

/// 产物后缀：可执行文件按目标平台（Windows 必须 `.exe`，否则系统不认为它是
/// 可执行程序）；静态库一律 `.a`。
///
/// 为何库不用 `toolchain.executable_suffix`：那是 **exe** 后缀（Windows 为 `.exe`），
/// 而归档必须叫 `.a`——它是 GNU 格式的静态库，与 MSVC 的 `.lib` 不兼容。
/// 用 `.exe` 给库命名会让链接器把它当成可执行文件处理。
[[nodiscard]] auto artifact_suffix(const TargetSpec& target, const ResolvedToolchain& toolchain)
    -> std::string {
  if (target.kind == "library") return ".a";
  return toolchain.executable_suffix;
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

  // 目标声明（库分支与 Windows GUI/图标都要用，所以在函数入口就找到）
  const TargetSpec* link_target = nullptr;
  if (!options.target.empty()) {
    // `targets` 是 **vector**（保持清单声明顺序，便于确定性输出）——线性找。
    for (const auto& candidate : manifest.targets) {
      if (candidate.name == options.target) {
        link_target = &candidate;
        break;
      }
    }
  }

  // ## 静态库分支：不走编译器驱动，走 `ar`
  //
  // `kind = "library"` 的产物是静态库（`.a`），而 `g++ ... -o x.a` 会当成
  // “链接一个叫 x.a 的可执行文件”——产物是 PE/ELF 可执行，不是归档。
  // 静态库必须用 `ar rcs`。
  //
  // 为何 `ar` 也从编译器路径推：交叉工具链的 `ar` 与宿主的 `ar` 目标格式不同
  // （mingw 的 `ar` 能写 PE 归档，宿主 GNU ar 也行，但 MSVC 风格目标不行）。
  // 取编译器同目录下的 `ar`（`g++` 旁边那个），找不到再回退 PATH。
  if (link_target != nullptr && link_target->kind == "library") {
    std::string archiver;
    if (const auto sibling = fs::join(fs::parent(toolchain.compiler_path), "ar");
        fs::is_regular_file(sibling + toolchain.executable_suffix)) {
      archiver = sibling + toolchain.executable_suffix;
    } else if (const auto found = process::which("ar"); found.has_value()) {
      archiver = *found;
    } else {
      return unexpected(ErrorCode::NotFound,
                        "未找到静态库归档工具 `ar`（静态库目标需要它；"
                        "请安装 binutils，或确认它与 C++ 编译器在同一目录）");
    }
    if (units.empty()) {
      return unexpected(ErrorCode::Invalid,
                        std::format("静态库目标 {} 没有任何对象文件（检查 sources/白名单）",
                                    link_target->name));
    }
    // 先删旧库：`ar rcs` 是**追加/更新**语义，不先删会把已移除源文件的旧成员留在库里
    // ——与“链接跳过”那个缺陷同一类（删源后产物仍带旧代码）。
    (void)fs::remove_file(output);
    std::vector<std::string> ar_args{"rcs"};
    ar_args.push_back(output);
    for (const auto& unit : units) ar_args.push_back(unit.object);
    auto archived = process::run(archiver, ar_args,
                                 process::Options{.cwd = manifest.directory, .env = toolchain.env});
    if (!archived) return forward_error(archived.error());
    if (archived->exit_code != 0) {
      return unexpected(ErrorCode::Invalid,
                        std::format("静态库归档失败: {}\n{}{}", output, archived->stdout_text,
                                    archived->stderr_text));
    }
    return output;
  }

  const std::vector<std::string> libraries = resolve_system_libraries(manifest, toolchain);

  std::vector<std::string> args;
  // `args`（head）= 旗标与链接器开关；`long_args` = **对象文件与额外源**（"一参数一项"
  // 的长列表）；`tail` = `-o 产物` + 库 + 工具链附加旗标。
  //
  // 为何从一开始就分三段（而不是到响应文件那里再分）：响应文件只该装 `long_args`，
  // 而"哪些参数是长列表"**只有这里知道**——让下游去猜就是第一版那个缺陷
  //（按"长得像路径"挑，把 `-o` 的产物路径也挑进了响应文件）。
  std::vector<std::string> long_args;
  std::vector<std::string> tail;
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

  // ⚠ **以下所有 `args` 追加必须发生在 `full_args` 拼接之前**。
  //
  // 踩过的真坑（2026-10-18）：`full_args` 是 `args` 的**快照**（拷贝）。
  // 把新旗标推到 `args` 而快照已经拼好——那条旗标就**静默丢弃**了：
  // 链接器收到的是一个不含它的命令行，既不报错也不警告。
  // 这也是 Windows 静态链运行库长期没生效的原因（它在快照之后）。
  // 判据：命令行走 `full_args`，响应文件路径走 `args`——两路必须看到同一堆旗标。

  // Windows 目标的**静态链接 C++ 运行库**（`-static-libgcc -static-libstdc++`）。
  // 不静态时产物要求 `libgcc_s_seh-1.dll`/`libstdc++-6.dll` 在 PATH 上——用户双击 exe
  // 或从别的目录启动时就报“找不到 dll”（与 mingw 交叉档同口径，见 st.pkg 的 extra_flags）。
  if (toolchain.platform == "windows") {
    for (const auto* flag : {"-static-libgcc", "-static-libstdc++"}) {
      if (std::ranges::find(args, flag) == args.end() &&
          std::ranges::find(toolchain.extra_flags, flag) == toolchain.extra_flags.end()) {
        args.emplace_back(flag);
      }
    }
  }

  // —— Windows 图形子系统 + exe 图标（2026-10-18）——
  //
  // `-mwindows` 把 PE 头的 Subsystem 从 3(CONSOLE) 改成 2(WINDOWS_GUI)：
  // 双击 exe **不再弹控制台窗口**。这是交付级要求——带黑框的 GUI 程序
  // 在用户眼里就是“没做完”。
  //
  // **不需要另写 `WinMain`**：`startup_arguments()` 走的是
  // `GetCommandLineW` + `CommandLineToArgvW`，不依赖控制台的 `argc/argv`，
  // 所以现有 `main()` 在 GUI 子系统下照常工作。
  // （反例：很多项目为转 GUI 手写一层 `WinMain` 转发——多一套入口就多一处
  //   不一致的可能，而这里根本不需要。）
  if (toolchain.platform == "windows" && link_target != nullptr &&
      link_target->system == "gui") {
    args.emplace_back("-mwindows");
  }
  // 图标：`.ico` 必须先编成对象再链进去（GNU 驱动不认识 `.ico`）。
  // 编好的对象放进 `long_args`（与其它对象同一批），不额外占命令行长度预算。
  if (toolchain.platform == "windows" && link_target != nullptr && !link_target->icon.empty()) {
    if (auto resource = compile_windows_resource(*link_target, options, toolchain, output);
        resource) {
      long_args.push_back(*resource);
    } else {
      return forward_error(resource.error());
    }
  }

  // 对象与额外源："一参数一项"，进响应文件（`long_args`）。
  for (const auto& unit : units) long_args.push_back(unit.object);
  for (const auto& extra : extra_sources) long_args.push_back(extra);
  // `tail` = `-o 产物` + 库 + 工具链附加旗标（**库必须在对象之后**，链接器单趟解析）。
  tail.push_back("-o");
  tail.push_back(output);
  // ## 先链本工程内的库目标（`libraries` 字段）
  //
  // 必须排在系统库**之前**：静态库的链接顺序是单趟解析——若 `-lst` 排在
  // `-lstdc++` 后面，st 里用到的 libstdc++ 符号可能在那时还没被引入。
  // 路径用 `-L<目录> -l<名字>`（而不是直写 `.a` 路径）：
  // · 与系统库同一语义，链接器自己选静态/动态；
  // · 带 `-L` 后名字与产物文件名解耦（将来产物改版不用动命令）。
  if (link_target != nullptr && !link_target->libraries.empty()) {
    std::vector<std::string> lib_dirs;
    for (const auto& lib_name : link_target->libraries) {
      const TargetSpec* dep = nullptr;
      for (const auto& candidate : manifest.targets) {
        if (candidate.name == lib_name) {
          dep = &candidate;
          break;
        }
      }
      if (dep == nullptr) {
        return unexpected(ErrorCode::Invalid,
                          std::format("目标 {} 声明了 libraries 里的 '{}'，但清单里没有这个目标",
                                      link_target->name, lib_name));
      }
      if (dep->kind != "library") {
        // 只报不阻：允许引用可执行目标在语义上没意义（`.exe` 不能当库链），
        // 但用错的人应当看到一句明确的话，而不是链接器抛一堆 undefined reference。
        return unexpected(ErrorCode::Invalid,
                          std::format("目标 {} 的 libraries 里的 '{}' 是 {} 而不是 library",
                                      link_target->name, lib_name, dep->kind));
      }
      // 直接给**库文件完整路径**，不用 `-L` + `-l`。
      //
      // 为何不用 `-L`：实测 lld 在这个位置对 `-L` 的处理不可靠（`-L` 与 `-l`
      // 顺序变了就找不到，而手工命令行同样的顺序却能找到——差异在驱动拼参数的方式）。
      // 直写路径没有歧义：链接器拿到什么就链什么，不依赖搜索路径语义。
      // 代价是库改名/改名位置时要同步改命令——而这是构建器自己算的，不是人写的。
      const std::string lib_file = fs::join(fs::parent(output), lib_name + ".a");
      tail.push_back(lib_file);
      (void)dep;
    }
  }
  const auto link_arguments = link_library_arguments(toolchain.platform, libraries);
  for (const auto& item : link_arguments) tail.push_back(item);
  for (const auto& flag : toolchain.extra_flags) tail.push_back(flag);

  // 完整命令行（未启用响应文件时用它；长度估算也基于它）。
  // **必须在上面所有 `args` 追加完成之后才拼**（见上面的踩坑注释）。
  std::vector<std::string> full_args = args;
  full_args.insert(full_args.end(), long_args.begin(), long_args.end());
  full_args.insert(full_args.end(), tail.begin(), tail.end());

  // 主动判长度：超阈值就直接用响应文件，不等失败。
  //
  // 为何不依赖“失败后重试”那个退路：超长时 `CreateProcessW` 的报错是
  // “文件名或扩展名太长”——**不指向命令行长度**，而“先失败一次”本身也是
  // 一次白跑的链接（测试链接要几十秒）。所以两条都留：主动判 + 失败重试兜底。
  const std::size_t estimated = estimate_command_line(toolchain.compiler_path, full_args);
  const bool command_line_too_long =
      toolchain.platform == "windows" &&
      estimated > kWindowsCommandLineLimit - kCommandLineHeadroom;
  auto result =
      command_line_too_long
          ? link_with_response_file(toolchain, args, long_args, tail, output, manifest.directory)
          : process::run(toolchain.compiler_path, full_args,
                         process::Options{.cwd = manifest.directory, .env = toolchain.env});
  if (command_line_too_long && result.has_value()) {
    // 如实报一声：响应文件是**退路**，而"链接参数多到需要退路"本身是值得知道的事
    //（下一轮该考虑拆测试二进制了）。
    ST_LOG_INFO("链接参数超长，已改用响应文件：{}（预计命令行 {} 字符，上限 {}）", output,
                estimated, kWindowsCommandLineLimit);
  }
  if (!result) {
    // 命令行超长（Windows `CreateProcessW` 上限 32767 字符）——**用响应文件重试**。
    //
    // 为何必须有这条退路：测试二进制会把**所有测试单元**链在一起，单元数随仓成长，
    // 迟早撞上限。实测：244 个单元时命令行估算已 32974 字符（超 207）。
    // 而报错信息是 `无法启动 'g++.exe': 文件名或扩展名太长`——**完全指不到命令行长度**
    //（看着像文件名有问题），这是那种"查半天才发现是构建器的问题"的缺陷。
    if (!command_line_too_long) {
      if (auto retried = link_with_response_file(toolchain, args, long_args, tail, output,
                                                 manifest.directory);
          retried.has_value()) {
        ST_LOG_INFO("链接失败且疑似命令行过长，已用响应文件重试成功：{}", output);
        result = std::move(retried);
      }
    }
    if (!result) return forward_error(result.error());
  }
  if (result->exit_code != 0) {
    const std::string diagnostics = result->stdout_text + result->stderr_text;
    // 产物被正在运行的实例占着（Windows 上必现：exe 被占用时链接器写不进去）。
    // 原始报错只有一句 `LNK1168 无法打开 …exe 进行写入`，排错的人第一反总不是“那个窗口还开着”——
    // 直接给出下一步。
    if (artifact_locked(diagnostics)) {
      return unexpected(
          ErrorCode::Io,
          std::format("链接失败：产物被占用（{}）\n"
                      "请先结束正在运行的实例（如关掉那个窗口 / 任务管理器结束进程；"
                      "它占着旧产物，链接器写不进去），再重新构建。\n{}",
                      output, diagnostics));
    }
    return unexpected(ErrorCode::Invalid,
                      std::format("链接失败: {}\n{}", output, diagnostics));
  }
  (void)options;
  return output;
}

}  // namespace

/// 文件内容的 fnv1a64。读不到 → `nullopt`（该条不记哈希，退回 mtime+size 判定）。
///
/// ## 为何在这里算而不是"每次都算"
///
/// 只有**写入侧**（真的编出一个 `.o` 之后）才算一次，而且每个文件只算一次
/// （`append_entry` 里的路径已去重）。增量构建的**快路径不调它**——
/// 它只会在 `(mtime, size)` 都匹配、需要验证内容时才现算（见 `changed`）。
[[nodiscard]] auto content_hash_cached(const std::string& path) -> std::optional<std::uint64_t>;

[[nodiscard]] auto content_hash_of(const std::string& path) -> std::optional<std::uint64_t> {
  const auto bytes = fs::read_bytes(path);
  if (!bytes.has_value()) return std::nullopt;
  return st::hash::fnv1a64_bytes(std::span<const std::uint8_t>(bytes->data(), bytes->size()));
}

/// 单次构建内的内容哈希缓存（**进程级**，但每次 `st build` 都是新进程，所以等同于"每次构建一份"）。
///
/// ## 为什么必须有
///
/// 不缓存时：432 个单元 × 每单元约 18 个依赖 = 7643 次哈希计算，
/// 而**去重后只有 1066 个文件**——含重复的读取量 201.6 MB vs 去重 43.6 MB，
/// 差 **4.6 倍**。缓存把它拉回 43.6 MB（约 0.2s）。
///
/// 用 `mutex` 是因为增量判定跑在并行分片里（28 路）；命中缓存的路径只碰一次 map 查找，
/// 未命中的才真读文件——而同一文件在并发下重复"未命中"的窗口很小，
/// 代价可接受（最多多读一次），换来实现简单。
[[nodiscard]] auto content_hash_cached(const std::string& path) -> std::optional<std::uint64_t> {
  // 进程级缓存：一次构建内的内容哈希复用（见上方说明）。
  // 用 static 是**有意的**——它的生命周期就该是"这次构建"（每次 `st` 运行是一个进程）。
  // lint-allow: L8 进程级缓存（单次构建内复用，进程生命周期即天然作用域）
  static std::mutex mutex;
  // lint-allow: L8 同上（与 mutex 配套的哈希表）
  static std::map<std::string, std::optional<std::uint64_t>> cache;
  {
    const std::scoped_lock lock(mutex);
    if (const auto found = cache.find(path); found != cache.end()) return found->second;
  }
  const auto digest = content_hash_of(path);
  {
    const std::scoped_lock lock(mutex);
    cache.emplace(path, digest);
  }
  return digest;
}

auto file_content_hash(std::string_view path) -> std::optional<std::uint64_t> {
  return content_hash_of(std::string(path));
}

/// 判定一份依赖记录是否仍与文件一致。**增量快路径与缓存复用共用这一份**——
/// 两处各写一遍必然在边界条件上给出不同答案（这正是 `.meta` 当初设计成共用的理由）。
///
/// ## 口径：内容哈希是**唯一的真相源**（2026-10-26 定稿）
///
/// 两个历史版本都各自错一半：
///
/// | 实现 | 行为 | 错在哪 |
/// |---|---|---|
/// | v1（mtime 变即判变） | mtime 不等 → 直接判变 | `touch` / `checkout` 时**白编**（实测 touch 一个头 → 重编 34 单元、20.3 s） |
/// | v2（快路径 mtime+size 一致即判未变） | 一致 → 直接判未变 | **等长修改 + mtime 还原**时漏报（本文件那条用例抓到的） |
/// | v3（当前：**全比内容**） | 一律算哈希定论 | 代价：每次构建读一遍依赖（去重后 43.6 MB，~0.2 s） |
///
/// 选 v3 的理由：两个方向都错过的实现，无论哪个方向都不可接受——
/// 一个导致“白等 20 秒”，一个导致“拿旧产物当新结果”。
/// 而 0.2 s 的代价在 `st watch`（保存即构建）下几乎无感。
///
/// 对**读不到**的情形仍然保守：判“变了”（宁可多编一次，不交付旧代码）。
auto deps_entry_matches(std::string_view path, std::int64_t recorded_mtime,
                        std::uint64_t recorded_size,
                        std::optional<std::uint64_t> recorded_hash) -> bool {
  const std::string owned(path);
  (void)recorded_mtime;   // 保留参数：`.meta` 仍记录它们，便于人工排查与旧格式识别
  (void)recorded_size;
  if (!recorded_hash.has_value()) {
    // 旧格式记录（无哈希）：没有内容可依，只能退回元数据比对。
    // 两侧都一致才认为“未变”——保守方向。
    const auto time = fs::modified_ns(owned);
    const auto size_now = fs::file_size(owned);
    return time.has_value() && *time == recorded_mtime && size_now.has_value() &&
           *size_now == recorded_size;
  }
  const auto digest = content_hash_cached(owned);
  if (!digest.has_value()) return false;   // 读不到：保守判“变了”
  return *digest == *recorded_hash;
}

/// 在**编译器旁边**找 binutils 工具（`objcopy` / `objdump`），找不到再回退 PATH。
///
/// 为何先找编译器旁：交叉工具链的 `x86_64-w64-mingw32-objcopy` 与它的 gcc 同装，
/// 用宿主 objcopy 去处理交叉产物会因目标格式不同而报错（且报错文本不指向“工具选错”）。
///
/// ⚠ 交叉前缀不一定叫 `g++`（可能是 `gcc` / `clang++`）——所以剥离规则是
/// "从最后一个 `-` 之前的前缀拿下"，探到不在就依次回退：
/// 目标前缀 → 无前缀 → PATH。多试两个路径的成本远小于“工具找不到”。
[[nodiscard]] auto find_binutil(std::string_view tool, std::string_view compiler_path,
                               std::string_view executable_suffix) -> std::string {
  const std::string suffix(executable_suffix);
  const std::string compiler_dir = fs::parent(std::string(compiler_path));
  if (!compiler_dir.empty()) {
    const std::string stem = fs::file_name(compiler_path);
    // `x86_64-w64-mingw32-g++` → 前缀 `x86_64-w64-mingw32-` → `x86_64-w64-mingw32-objcopy`
    const std::size_t dash = stem.rfind('-');
    if (dash != std::string::npos) {
      const std::string candidate =
          fs::join(compiler_dir, stem.substr(0, dash + 1) + std::string(tool) + suffix);
      if (fs::is_regular_file(candidate)) return candidate;
    }
    // 无前缀（本机 g++ / clang++ → 同目录 objcopy）
    const std::string plain = fs::join(compiler_dir, std::string(tool) + suffix);
    if (fs::is_regular_file(plain)) return plain;
  }
  if (const auto found = process::which(tool); found.has_value()) return *found;
  return {};
}

/// 分离调试信息：把产物里的 DWARF 搬到隔壁的 `<产物>.debug`，产物本体瘦身。
///
/// ## 为何需要（2026-10-14 产物评估实测）
///
/// `st.exe`（dev 档 `-O1 -g1`）实测 **76.96MB，其中 68.4MB（89%）是 `.debug_*`**
/// （`.debug_info` 47.8MB + `.debug_loclists` 13.2MB），而 `.text` 仅 4.45MB。
/// `-g1` 并没有像设计预期那样小：它对 **C++ 模板实例**（libstdc++ 头里爆炸的实例化）
/// 仍要为每个内联/模板函数生成行号表与 DIE 骨架。而交付物根本不需要它。
///
/// ## 做法（与 gdb 的约定，而不是自定义格式）
///
/// ① `objcopy --only-keep-debug 产物 产物.debug`（把调试信息拷到分离文件）
/// ② `objcopy --strip-debug 产物`（从产物里去掉它）
/// ③ `objcopy --add-gnu-debuglink=产物.debug 产物`（记下"符号在隔壁"）
///
/// 第三步是关键：它让 **gdb 自动找到**分离符号（DebugLink 段里是文件名 + CRC），
/// 不需要用户记住 `file`/`symbol-file` 命令。崩溃器的 `addr2line -e 产物` 也一样——
/// 实测（本仓 st 自身）：**69.7MB → 4.57MB**，而 `nm`/`addr2line` 仍能解出函数名。
///
/// ## 边界
///
/// - **只对 ELF/PE 这类 objcopy 认的格式生效**；objcopy 不在（或报错）时**不算失败**
///   （产物已链接成功，只是胖了点）——返回 `Error` 由调用方记一条 warn。
/// - **不分离 `san`/`debug` 档的完整 `-g`**：那些档的调试体验是第一优先级，
///   而且它们的使用场景是"就地调试"（分离反而多一步）。
/// - `objcopy` 从**与编译器同目录**开始找（交叉工具链的 `*-objcopy` 与它的 gcc 同装），
///   再回退 PATH——交叉构建时用宿主 objcopy 会因目标格式不同而报错。
[[nodiscard]] auto separate_debug_info(const std::string& artifact,
                                       std::string_view compiler_path,
                                       std::string_view executable_suffix,
                                       bool verbose) -> Status {
  const std::string objcopy = find_binutil("objcopy", compiler_path, executable_suffix);
  if (objcopy.empty()) {
    return unexpected(ErrorCode::Unsupported, "未找到 objcopy（跳过调试信息分离）");
  }

  const std::string debug_file = artifact + ".debug";
  // 幂等判据：**产物里已经没有调试段，且 `.debug` 已存在**，才跳过。
  //
  // ⚠ 这里踩过两次，两次都是”看起来显然“的写法错了：
  //
  // ① **不能拿 mtime 比**（第一版）：`objcopy --strip-debug` **会刷新产物的 mtime**，
  //    于是 `.debug` 永远是“旧的”——判据恒假，每次重链都白跑三遍 objcopy。
  // ② **不能只看`.debug` 存在**：重链后 `.debug` 是**上一次的**符号文件，产物却是新的
  //    且未 strip。此时跳过 = **新产物的调试信息永久丢失**（比多跑一遍重得多）。
  //
  // 正确且可观测的事实：产物内不含调试段。用 `objdump -h` 看（与 objcopy 同属 binutils，
  // 交叉工具链里必同装）。**objdump 探不到时按"需要重跑"处理**——多花时间是可接受的，
  // 丢符号不可接受。
  if (fs::is_regular_file(debug_file)) {
    if (const std::string objdump = find_binutil("objdump", compiler_path, executable_suffix);
        !objdump.empty()) {
      if (auto headers = process::run(objdump, {"-h", artifact});
          headers.has_value() && headers->exit_code == 0) {
        // ELF 与 PE 的 DWARF 段名差一个下划线前缀（MinGW 实测为 `.debug_info`，
        // 但 GNU 对 PE 的习惯是加 `_`）——两边都查。
        const bool has_debug_sections =
            headers->stdout_text.contains(".debug_info") ||
            headers->stdout_text.contains("__debug_info");
        if (!has_debug_sections) return ok();
      }
    }
  }

  const auto run_objcopy = [&](const std::vector<std::string>& args) -> Status {
    if (verbose) {
      // 拼串而非 `st::join(args, ...)`：`st::join` 的参数是 `vector<string_view>`，
      // 此处是 `vector<string>`（为去重而保留的容器类型）——直接拼接更短且无歧义。
      std::string dump;
      for (const auto& item : args) dump.append(" ").append(item);
      ST_LOG_INFO("objcopy:{}", dump);
    }
    auto result = process::run(objcopy, args, process::Options{.cwd = fs::parent(artifact)});
    if (!result.has_value()) return forward_error(result.error());
    if (result->exit_code != 0) {
      return unexpected(ErrorCode::Invalid,
                        std::format("objcopy 失败（{}）：{}{}", args.empty() ? "" : args.front(),
                                    result->stdout_text, result->stderr_text));
    }
    return ok();
  };

  // ① 拷出调试信息（先做它：万一 ② 之后才发现拷不出来，DWARF 已经没了）
  if (auto status = run_objcopy({"--only-keep-debug", artifact, debug_file}); !status) {
    return status;
  }
  // ② 从产物里去掉
  if (auto status = run_objcopy({"--strip-debug", artifact}); !status) return status;
  // ③ 记下"符号在隔壁"（gdb 靠 DebugLink 自动认）
  if (auto status = run_objcopy({"--add-gnu-debuglink=" + debug_file, artifact}); !status) {
    // DebugLink 没写上不算致命（产物已瘦身、`.debug` 也在），但 gdb 就不能自动找到了——
    // 记一条 warn 而不是静默。
    ST_LOG_WARN("未能写入 DebugLink（gdb 需手工 `symbol-file`）：{}", status.error().message);
  }
  const auto stripped_size = fs::file_size(artifact);
  const auto debug_size = fs::file_size(debug_file);
  if (stripped_size.has_value() && debug_size.has_value()) {
    ST_LOG_INFO("已分离调试信息：{} → {:.2f} MB（分离符号 {:.2f} MB）", fs::file_name(artifact),
                static_cast<double>(*stripped_size) / (1024.0 * 1024.0),
                static_cast<double>(*debug_size) / (1024.0 * 1024.0));
  }
  return ok();
}

/// 链接诊断里「产物被占用」的判定（导出：`st::pkg::artifact_locked`，
/// 声明在 build.hpp——测试 `pkg_build_lock_test.cpp` 直接断言三套工具链文案）。
///
/// 三套工具链三套文案，漏一套就退回裸报「链接失败」——排错的人想不到是
/// 残留进程占着产物（本仓 Windows+MinGW 宿主上这是常态路径：无头实例忘停就重编）。
bool artifact_locked(std::string_view diagnostics) {
  // MSVC：LNK1168（无法写入）/ LNK1104（无法打开）
  if (diagnostics.find("LNK1168") != std::string_view::npos) return true;
  if (diagnostics.find("LNK1104") != std::string_view::npos) return true;
  // POSIX：Text file busy
  if (diagnostics.find("Text file busy") != std::string_view::npos) return true;
  // MinGW（ld/lld，实测）：`failed to write output '…exe': permission denied`。
  // 两个关键词同时出现才判占用：单独的 permission denied 可能是目录权限问题
  //（那的下一步不足「结束实例」，误导）。
  return diagnostics.find("failed to write output") != std::string_view::npos &&
         diagnostics.find("permission denied") != std::string_view::npos;
}

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



/// 构建测试可执行（只构建、不运行）。定义在 `run_tests` 附近，先声明供 `build()` 用。
[[nodiscard]] auto build_test_binary(const Manifest& manifest, const BuildOptions& options)
    -> Result<std::string>;

/// `st build` 末尾的收尾：**测试二进制过期就把它编出来**。
///
/// ## 为什么 `st build` 要管这件事
///
/// `st.pkg` 把 `tests/*_test.cpp` 声明成独立目标，`st build` 历来不碰它。
/// 于是「改了测试断言 → 跑 st build → 直接执行 st_tests」这条路上，
/// 验的是**上一版代码**——而 `st build` 会报「构建完成」，看不出任何异常。
///
/// 对 AI 尤其致命：它的整套迭代是「改 → 构建 → 跑 → 看结果」，
/// 而**拿旧结果做结论**是这条链上最贵的失误（会去查被测代码，白烧几轮）。
///
/// ## 为什么是"只构建不运行"
///
/// 「能不能构建」与「断言是否通过」是两个语义，混在一起会让 `st build`
/// 的成败同时表达两件事；而且很多人只想跑应用、不关心测试。
/// 所以这里**只保证磁盘上的二进制是最新的**，跑不跑由 `st test` 决定。
///
/// ## 何时跳过
///
/// - 没有 `tests` 声明；或
/// - 测试二进制**尚不存在**（从没跑过测试——这是正常状态，不该每次构建都白编一遍）；
/// - 或 `options.no_tests`（显式关掉）。
///
/// 返回值只用于日志，失败**不**让 `st build` 失败：测试编不过不该挡住产物
/// 已经成功这件事（那是 `st test` 的报错范围）。
/// @return 是否真的重建了（用于在汇总里告诉用户「本次构建包含测试」）
[[nodiscard]] auto refresh_stale_test_binary(const Manifest& manifest, const BuildOptions& options)
    -> bool {
  if (options.no_tests) return false;
  if (!options.target.empty()) return false;   // 只对库目标做（应用目标语义是"出那个产物"）
  auto tests = manifest.test_files();
  if (!tests || tests->empty()) return false;

  const auto toolchain = resolve_toolchain(manifest, options.toolchain);
  if (!toolchain) return false;
  const std::string bin_dir = fs::join(
      options.root.empty() ? manifest.directory : options.root,
      std::format("build/{}/bin", profile_directory(options.profile, *toolchain)));
  const std::string binary = fs::join(bin_dir, "st_tests" + toolchain->executable_suffix);
  const auto bin_time = fs::modified_ns(binary);
  if (!bin_time.has_value()) return false;   // 从没构建过测试 → 不是"过期"，是"没开始"

  if (!test_binary_is_stale(*tests, binary)) return false;

  ST_LOG_INFO("测试目标已过期 → 同步重建（不运行）");
  auto built = build_test_binary(manifest, options);
  if (!built) {
    // 不让构建失败：产物本身是好的。但必须让人看见，否则下次又会拿旧二进制。
    ST_LOG_WARN("测试目标重建失败（不影响本次产物）：{}", built.error().message);
    return false;
  }
  return true;
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
  // 嵌入产物目录（`<root>/build/<档位>/embed_lib`）：识别“哪些单元是嵌入生成的”。
  const std::string build_dir = fs::join(root, std::format("build/{}", subdir));
  if (auto status = fs::create_directories(object_dir); !status) return forward_error(status.error());
  if (auto status = fs::create_directories(bin_dir); !status) return forward_error(status.error());

  // 目标级源选择：两种语义二选一（`only_sources` 非空时走白名单）。
  //
  // ## 为何要有白名单
  //
  // `exclude_sources` 是黑名单：漏一条就多编几个无关单元，而构建照常成功，
  // **静默变胖**——只能靠人去把每条间接依赖都推一遍。实测：`st` 目标逐项排了 13 条，
  // 仍因 `src/ui/window_control.cpp` 间接依赖 `math/geometry.hpp` 把 math 拉了回来。
  //
  // 白名单相反：漏写会链接报错（可发现），不可能静默多编。
  std::vector<std::string> library_sources = *sources;
  if (!options.target.empty()) {
    if (const TargetSpec* spec = manifest.find_target(options.target); spec != nullptr) {
      // 目标级 `log` 覆盖工程级（`std::optional`：只有显式写了才覆盖）。
      if (spec->log_enabled.has_value() && *spec->log_enabled != manifest.log_enabled) {
        // 先摘掉工程级那条，避免同时出现相反的两个定义（裸名，见上方说明）。
        std::erase(effective.defines, std::string("ST_LOG_DISABLED=1"));
        if (!*spec->log_enabled) effective.defines.push_back("ST_LOG_DISABLED=1");
      }
      if (!spec->include_sources.empty()) {
        // 白名单生效时必须可见：否则“字段写了但没生效”与“生效了”在输出上一模一样。
        // 实测教训：这个分支曾因跑的是旧二进制而从未进入，而构建报告只说
        // “单元 N”，看不出白名单到底管没管用——多花了几个小时。
        const std::size_t before = library_sources.size();
        // 白名单：库源**整体丢弃**，只保留与白名单匹配的那部分。
        // 不用“先清空再按 glob 加”是因为 `*sources` 是已展开的绝对路径列表，
        // 按 glob 重新展开会远程依赖清单解析顺序（两处展开结果可能不一致）。
        std::erase_if(library_sources, [&spec, &manifest](const std::string& path) {
          const std::string relative = fs::relative_to(path, manifest.directory);
          return std::none_of(spec->include_sources.begin(), spec->include_sources.end(),
                              [&](const std::string& pattern) {
                                return fs::match_glob(pattern, relative);
                              });
        });
        if (library_sources.empty()) {
          ST_LOG_WARN("目标 {} 的 include_sources 未命中任何库级源文件", options.target);
        } else {
          ST_LOG_INFO("白名单：目标 {} 库源 {} → {} 个（include_sources {} 条）", options.target, before,
                      library_sources.size(), spec->include_sources.size());
        }
      } else {
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

  if (options.target.empty()) {
    stats.tests_rebuilt = refresh_stale_test_binary(manifest, options);
    return stats;
  }

  const TargetSpec* target = manifest.find_target(options.target);
if (target == nullptr) {
  return unexpected(ErrorCode::NotFound, std::format("目标不存在: {}", options.target));
}

// ## 先递归构建本目标依赖的库目标
//
// 为何必须在编译本目标**之前**：链接期需要 `.a` 存在，而 `-l` 只告诉链接器去哪找。
// 若等到链接才发现库没建，就得到一句 `cannot find -lst`——看不出“该先建库”。
//
// 递归而不是单层：库也可能依赖另一个库（核心库 / 扩展库）。`visited` 防环
// （A 依赖 B、B 依赖 A 是清单错误，但不应造成栈溢出）。
{
  std::set<std::string> visiting{};
  const auto build_libraries_of = [&](const TargetSpec& spec, auto&& self) -> Status {
    for (const auto& lib_name : spec.libraries) {
      const TargetSpec* dep = manifest.find_target(lib_name);
      if (dep == nullptr) {
        return unexpected(ErrorCode::Invalid,
                          std::format("目标 {} 的 libraries 里 '{}' 不存在", spec.name, lib_name));
      }
      if (dep->kind != "library") {
        return unexpected(ErrorCode::Invalid,
                          std::format("目标 {} 的 libraries 里 '{}' 是 {}，只能是 library",
                                      spec.name, lib_name, dep->kind));
      }
      if (!visiting.insert(lib_name).second) {
        return unexpected(ErrorCode::Invalid,
                          std::format("目标依赖成环：{} → {}", spec.name, lib_name));
      }
      if (auto nested = self(*dep, self); !nested) return nested;
      // 子目标构建（把 target/output_override 改到库上，其余选项不变）
      BuildOptions lib_options = options;
      lib_options.target = lib_name;
      lib_options.output_override.clear();   // 库产物路径由它自己算
      auto lib_stats = build(manifest, lib_options);
      if (!lib_stats) return forward_error(lib_stats.error());
      visiting.erase(lib_name);
    }
    return {};
  };
  if (auto ready = build_libraries_of(*target, build_libraries_of); !ready) {
    return forward_error(ready.error());
  }
}

  std::vector<CompileUnit> target_units;
  if (!target->sources.empty() || !target->embed.empty()) {
    std::vector<std::string> target_sources;
    for (const auto& pattern : target->sources) {
      auto matches = fs::expand_glob(manifest.directory, pattern);
      if (!matches) return forward_error(matches.error());
      for (const auto& item : *matches) target_sources.push_back(item);
    }
    // ## `inherit_project_embed = false`：把工程级嵌入从本目标里剔出去
    //
    // 工程级 `embed`（如本仓的 `src/ui/script_api.js`）原本会**无条件**并入每个目标；
    // 于是 `st_core`（通用库）里会多出 UI 的 JS 字节数组——声明是“不依赖 UI 的通用层”，
    // 实现里却带着 UI 的产物。
    //
    // ## ⚠ 判据必须是 `build/<档位>/embed/lib`，不是 `embed_lib`
    //
    // 上一版的判据是 `fs::join(build_dir, "embed_lib")`，**一直是错的**：
    // 实际路径是 `build/dev/embed/lib/src/…/x.cpp`（`embed` 目录下再一层 `lib`）。
    // 两者对不上 ⇒ 剔除永远命中 0 个，而**回归测试没能发现**：
    // 测试用的是临时目录里的小工程，它的嵌入路径恰好匹配了那个错判据。
    // 实测（2026-10-26）：`ar t st_core.a` 里看得到 `ui_declarative_js.cpp.o`。
    // 教训：这类“路径前缀”判据必须用**真实仓库**验证一次，不能只靠单测。
    if (target->inherit_project_embed.has_value() && !*target->inherit_project_embed) {
      const std::string embed_marker = fs::normalize(fs::join(build_dir, "embed"));
      const auto before = target_sources.size();
      std::erase_if(target_sources, [&](const std::string& path) {
        // 只剔 `build/<档位>/embed/` 下的（不是整个 build 目录）
        const std::string normalized = fs::normalize(path);
        return normalized.starts_with(embed_marker) &&
               normalized.find("/embed/") != std::string::npos;
      });
      if (target_sources.size() != before) {
        ST_LOG_INFO("目标 {}：不继承工程级嵌入，已剔除 {} 个生成的嵌入单元", target->name,
                    before - target_sources.size());
      }
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
    // ## 路径形态归一：同一文件可能以「相对清单目录」与「绝对路径」两种形态进来
    //
    // `expand_glob` 的返回口径是**相对 root**，但清单里也允许写绝对路径
    // （`src/ui/window_control.cpp` 这类）；框架源那条路则一律是绝对路径。
    // 两种形态在 `std::unique` 看来是两个不同串，于是**同一文件编两遍**，
    // 对象名也按不同形态生成（`src_ui_x.cpp.o` 与 `C__…_src_ui_x.cpp.o`），
    // 链接期报 `duplicate symbol`。
    //
    // 实测（2026-10-26）：`st.pkg` 的 `targets.st` 从 `exclude_sources` 换成
    // `include_sources` 白名单后，`window_control.cpp` 同时出现在库源（白名单命中）
    // 与目标源（`sources` 显式列出）→ 自举链接失败、且旧 `st` 无法重建自己＝死锁。
    //
    // 归一成「相对清单目录、正斜杠」再比——这也是 `include_sources` / `exclude_sources`
    // 的匹配口径，三者必须一致。
    // ## 去重：`sources` 与 `include_sources` 的交集只能编一次
    //
    // 两者是**两条独立的源收集路径**（`library_sources` 从顶层 sources 来，
    // `target_sources` 从 target 的 sources 来），各自的 glob 可能命中同一个文件。
    // 同一个 `.cpp` 编两遍 → 两份 `.o`（对象名由路径规则生成，两条路径命名不同）
    // → 链接期 `multiple definition`。
    //
    // 实测：`st` 的 `sources` 含 `src/ui/window_control.cpp`，而顶层 `sources` 的
    // `src/ui/*.cpp` 也含它，于是报 `window_control.cpp:7` 重复定义。
    //
    // 以 `library_sources` 为准剔除（它们已经代表“顶层口径”）；这样白名单里的文件
    // 若同时写了 `sources` 也不会重复。
    if (!target_sources.empty()) {
      // 两边都先归一到「相对清单目录 + 正斜杠」，再比集合成员。
      // 用 `std::set` 而不是 vector+find：单元数上千时这是 O(n²) → O(n log n)，
      // 而这段在每个 `st build` 都跑。
      std::set<std::string> library_relatives{};
      for (const auto& path : library_sources) {
        library_relatives.insert(fs::normalize(fs::relative_to(path, manifest.directory)));
      }
      const auto before_dedupe = target_sources.size();
      std::erase_if(target_sources, [&](const std::string& path) {
        return library_relatives.contains(fs::normalize(fs::relative_to(path, manifest.directory)));
      });
      if (target_sources.size() != before_dedupe) {
        ST_LOG_INFO("目标 {}：{} 个源同时出现在库源与 targets.sources，按库源去重", options.target,
                    before_dedupe - target_sources.size());
      }
    }
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

  // 产物后缀：Windows 可执行文件必须 `.exe`（否则系统不认为它是可执行程序）；
  // 静态库一律 `.a`（GCC 风格驱动 + `ar` 归档；与 MSVC 的 `.lib` 不兼容）。
  const std::string default_output =
      fs::join(bin_dir, target->name + artifact_suffix(*target, *toolchain));
  const std::string output =
      options.output_override.empty() ? default_output : options.output_override;
  std::vector<CompileUnit> link_units = units;
  link_units.insert(link_units.end(), target_units.begin(), target_units.end());
  // ## `inherit_project_embed = false`：把工程级嵌入的**对象**从链接集合里剔除
  //
  // 光在 `target_sources` 上剔除不够：工程级嵌入的生成源已经进了 `library_sources`
  // （→ `units`），那是**所有目标共享**的库级源。所以要在最终的 `link_units`
  // （库级 + 目标级）上再按 `library_includes` 剔除一次。
  //
  // 实测（2026-10-26）：只改 `target_sources` 时，`ar t st_core.a` 里仍有两个嵌入成员
  // （`ui_declarative_js.cpp.o` / `ui_script_api_js.cpp.o`）——库声明是“不依赖 UI 的通用层”，
  // 产物里却带着 UI 的 JS 字节数组。
  if (target->inherit_project_embed.has_value() && !*target->inherit_project_embed) {
    // 剔除判据：路径在**嵌入产物目录**下（`build/<档位>/embed/`）。
    //
    // ⚠ 不能用 `library_includes.contains(source)`：那个表登记的是**所有**库级单元的
    // 包含目录（包括普通源），用它剔除会把整个库剔空（实测：44 个成员→剩 5 个，且只剩第三方）。
    // 嵌入产物的路径特征是统一的：在 `build/<subdir>/embed/` 下（含再一层 `lib/`）。
    const std::string embed_marker = fs::normalize(fs::join(build_dir, "embed"));
    const auto before = link_units.size();
    std::erase_if(link_units, [&](const CompileUnit& unit) {
      return fs::normalize(unit.source).starts_with(embed_marker);
    });
    if (link_units.size() != before) {
      ST_LOG_INFO("目标 {}：不继承工程级嵌入，链接集合剔除 {} 个嵌入单元", target->name,
                  before - link_units.size());
    }
  }

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
  //
  // ⚠ **对象的 (mtime, size) 必须混进指纹**（2026-10-20 修）：
  // 共享对象缓存 restore 时用 `copy_file`，Windows 的 `CopyFile` **保留源文件时间戳**
  // ——于是"内容换成了新对象、但 .o 的 mtime 还是旧条目那时的时间"。此时：
  //   · `rebuilt == 0`（本轮没重编，是从缓存拿的）
  //   · `.o` 的 mtime 比产物旧 → 时间戳比较放行
  // 两条判据都不响，**产物一声不响地停留在旧代码上**。实测现象就是
  // "源码明明改了、测试却跑的是旧行为"，而且要 `--force` 才能醒。
  // 把 (mtime, size) 入指纹后，内容一变指纹就变，不再依赖时间先后。
  const auto link_fingerprint = [&]() -> std::string {
    std::string material(compiler_kind_name(toolchain->kind));
    for (const auto& unit : link_units) {
      material.append("\n").append(unit.object);
      // 对象身份：mtime + size。size 抓“内容长度变了”，mtime 抓“重建过”。
      // 两者都不看时，缓存 restore 换内容而不换时间戳的场景就会漏（见上）。
      const auto time = fs::modified_ns(unit.object).value_or(0);
      const auto bytes = fs::file_size(unit.object).value_or(0);
      material.append("\t").append(std::to_string(time));
      material.append("\t").append(std::to_string(bytes));
    }
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
    stats.tests_rebuilt = refresh_stale_test_binary(manifest, options);
    return stats;
  }

  const std::int64_t link_start = time::now_ns();
  auto linked = link(manifest, options, link_units, output, *flags, *toolchain, {});
  if (!linked) return forward_error(linked.error());
  if (options.strip_debug && target->kind != "library") {
    // 分离调试信息（默认；`--no-strip` 关）。失败**不算构建失败**——产物已经能跑，
    // 只是胖了点（分离是交付体验项，不是正确性项）。
    // 静态库不分离：`objcopy --only-keep-debug` 对归档是逐成员的，语义与“产出一份
    // 独立 .debug”不同（会把每个 .o 各搞一遍），而库的调试信息本来就由使用方
    // 链接后再分离。
    if (auto stripped = separate_debug_info(output, toolchain->compiler_path,
                                            toolchain->executable_suffix, options.verbose);
        !stripped) {
      ST_LOG_WARN("分离调试信息失败（产物仍可用，仅体积偏大）：{}", stripped.error().message);
    }
  }
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
      // 报错必须带可定位的信息（曾实测「测试分片 2 未能启动: 」——原因栏空白，
      // 而真相是分片进程越界 abort）：启动失败带上 errno 语义，已启动但异常
      // 退出（被信号杀/非零码退出且没写 JUnit 报告——分片正常时退出码就是失败
      // 用例数，但那时报告一定在；报告缺失的非零退出只可能是异常路径）另列一条。
      const std::string reason = first_errors[index].empty()
                                     ? std::string("未知原因（进程未返回结果）")
                                     : first_errors[index];
      return unexpected(
          ErrorCode::Internal,
          std::format("测试分片 {}/{} 未能启动: {}", index + 1, shards, reason));
    }
    // 异常退出 vs 正常失败：分片进程的退出码就是该片的失败用例数（0 = 全绿）。
    // 但「报告文件没写出来」说明进程根本没跑到收尾——最常见的是用例里
    // abort/SIGSEGV 把整片带走（曾实测：16 片汇总全是 0 failed、整体退出码 1，
    // 中间夹一行空原因的「未能启动」——只看汇总行会当成全绿）。
    // 这里把「非零退出 + 无报告」显式报出，并计入总失败数，不再只是静默打印。
    if (exit_codes[index] != 0 && !junit_path.empty() &&
        !fs::exists(shard_reports[index])) {
      (void)fs::remove_all(shard_dir);
      return unexpected(ErrorCode::Internal,
                        std::format("测试分片 {}/{} 异常退出（码 {}）且未写出报告："
                                    "片内用例可能直接 abort/崩溃，后续用例未运行——"
                                    "请单跑该分片复现：{} --shard {}/{}",
                                    index + 1, shards, exit_codes[index], output,
                                    index + 1, shards));
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

/// 构建测试可执行（**只构建，不运行**），返回产物路径。
///
/// ## 为什么单独抽出来
///
/// `st test` = 构建 + 运行；而 `st build` 在测试过期时也需要**只构建**这一段
/// （见 `build()` 末尾）。两处共用同一份构建逻辑，避免"改一处忘另一处"——
/// 那条分叉会让 `st test` 与 `st build` 对同一个工程得出不同的编译结果。
[[nodiscard]] auto build_test_binary(const Manifest& manifest, const BuildOptions& options)
    -> Result<std::string> {
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
  if (!compiled) {
    return unexpected(compiled.error().code,
                      std::format("测试构建失败（编译段）：{}", compiled.error().message));
  }
  ST_LOG_INFO("测试构建：{} 单元（重编 {}）", units.size(), rebuilt);

  const std::string output = fs::join(bin_dir, "st_tests" + toolchain->executable_suffix);
  auto linked = link(manifest, options, units, output, *flags, *toolchain, {});
  if (!linked) {
    // 链接失败单独报（BACKLOG 3837）：ld.lld「failed to write output … permission denied」
    // 曾被误判为「一堆用例没跑＝测试失败」——产物没出来，用例根本没跑。
    // 「测试构建失败」前缀是 is_link_failure 的判据，不能改。
    return unexpected(linked.error().code,
                      std::format("测试构建失败（链接段）：{}", linked.error().message));
  }
  // 测试二进制同样分离调试信息：它是**开发用**产物，但每次都链、而且比 gallery 大得多
  // （几百个单元全静态链入）——留着 70MB 的 `.debug_*` 只会白占磁盘与拷贝时间。
  // 崩了照样能回溯：`addr2line -e st_tests.debug` 或直接交给 gdb（它自己认分离符号）。
  if (options.strip_debug) {
    if (auto stripped = separate_debug_info(output, toolchain->compiler_path,
                                           toolchain->executable_suffix, options.verbose);
        !stripped) {
      ST_LOG_WARN("分离调试信息失败（测试产物仍可用）：{}", stripped.error().message);
    }
  }

  return output;
}

auto run_tests(const Manifest& manifest, const BuildOptions& options, std::string_view filter,
               bool list_only, std::string_view junit_path, bool include_slow) -> Result<int> {
  // ## `--list` 直接复用已有二进制，不走构建
  //
  // 实测（2026-10-26）：`st test --list` 壁钟 **7.6s**，而直接跑
  // `st_tests.exe --list` 只需 **0.5s**。差的 7s 全在 `build_test_binary` 的
  // 增量判定：`needs_rebuild` 对每个单元的每个依赖头调 `deps_entry_matches`，
  // 而后者在 mtime+size 未变时**会读内容算哈希**——75 个测试单元 × 数百个
  // 依赖头，就是那几秒。
  //
  // 而 `--list` 只枚举用例名、**不运行任何用例**，二进制是不是最新无关紧要
  // （最坏也只是列出旧用例名，下一次真跑自然重建）。所以二进制存在就直接用。
  // 产物不存在时仍然构建——否则“首次 --list”会失败。
  auto toolchain = resolve_toolchain(manifest, options.toolchain);
  if (!toolchain) return forward_error(toolchain.error());

  std::string output{};
  if (list_only) {
    const std::string root = options.root.empty() ? manifest.directory : options.root;
    const std::string candidate = fs::join(
        fs::join(fs::join(root, "build"), profile_directory(options.profile, *toolchain)),
        fs::join("bin", "st_tests" + toolchain->executable_suffix));
    if (fs::is_regular_file(candidate)) output = candidate;
  }

  if (output.empty()) {
    auto test_binary = build_test_binary(manifest, options);
    if (!test_binary) return forward_error(test_binary.error());
    output = *test_binary;
  }

  std::vector<std::string> args = test_args(filter, list_only, include_slow);
  // 交叉产物仅在与宿主平台不同时拒绝执行（目标==宿主则照常跑，见 runs_on_host）
  if (!toolchain->runs_on_host()) {
    return unexpected(ErrorCode::Unsupported,
                      std::format("交叉编译产物无法在宿主执行: {}（请在目标平台运行）", output));
  }
  return run_shards(output, manifest.directory, args, list_only, junit_path, options.test_jobs);
}

/// 错误分类：`run_tests` 链路上哪些失败是「链接/构建失败」而非「用例断言红」。
///
/// 判据（产品实测口径，2026-10 BACKLOG 3837）：
/// - 编译/链接段的失败（build.cpp:2228-2232 段）以「测试构建失败」开头；
/// - 分片进程异常退出/未启动以「测试分片 …/N」开头（含真 abort 与起不来两种）；
/// - 其余（清单加载、工具链解析、JUnit 合并失败……）不是用例失败，但也不属
///   「产物没出来」——归入非链接类，CLI 层按旧语义报（不影响退码判断）。
/// 退出码语义见 command_test：链接失败类固定退 125，与「恰好 N 个用例失败」
/// 区分开，脚本/演进循环不再需要从 stderr 文本里猜。
[[nodiscard]] bool is_link_failure(const st::Error& error) {
  const std::string_view message = error.message;
  return message.starts_with("测试构建失败") || message.starts_with("测试分片");
}

}  // namespace st::pkg
