#include "st/pkg/watch.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/pkg/manifest.hpp"

namespace st::pkg {
namespace {

/// 永远不监视的目录（相对仓库根）：产物、日志、VCS、依赖缓存。
///
/// 为何必须排除 `build/`：它是**输出**目录。若监视它，一次构建写入产物 → 侦测到变化
/// → 再构建 → 再写产物……形成自激循环（而且不会有任何报错，只是永远转）。
[[nodiscard]] auto is_excluded_directory(std::string_view relative) -> bool {
  static constexpr std::string_view kExcluded[] = {
      "build", ".git", ".st", "tmp", "node_modules", ".cache"};
  const std::size_t slash = relative.find('/');
  const std::string_view head = slash == std::string_view::npos ? relative : relative.substr(0, slash);
  for (const auto prefix : kExcluded) {
    if (head == prefix) return true;
  }
  return false;
}

/// 文件是否属于"值得监视"的类别（按扩展名）。
///
/// 收 `.hpp`/`.cpp`/`.c`/`.h` 与清单/配置：这些是**构建输入**。
/// 不收 `.js`/`.png` 之类资源：它们要么被嵌入（嵌入产物在 `build/` 下，已排除），
/// 要么在运行期读（改了不需要重编，`st run` 自己会重新读）。
[[nodiscard]] auto has_watchable_extension(std::string_view path) -> bool {
  static constexpr std::string_view kExts[] = {".hpp", ".cpp",  ".c",  ".h",
                                               ".pkg", ".json",  ".toml"};
  for (const auto ext : kExts) {
    if (path.size() >= ext.size() &&
        path.substr(path.size() - ext.size()) == ext) {
      return true;
    }
  }
  return false;
}

/// 相对路径（统一正斜杠，便于与排除规则比对）。
///
/// ⚠ 两种输入形态都要能处理：
/// · **绝对路径**（构造自 `root + rel`）：做 `relative_to`；
/// · **已经是相对路径**（`fs::walk` 的返回值就是这种）：原样归一化。
/// 第一版只看后者当绝对路径处理，`relative_to(相对路径, 绝对 root)` 返回空，
/// 于是所有文件都被当成“不在 root 下”而丢弃——监视列表恒为空。
[[nodiscard]] auto relative_slash(std::string_view root, std::string_view path) -> std::string {
  if (fs::is_absolute(path)) {
    return fs::normalize(fs::relative_to(std::string(path), std::string(root)));
  }
  return fs::normalize(path);
}

}  // namespace

auto collect_watched_files(const std::string& root, const std::vector<std::string>& include,
                           const std::vector<std::string>& exclude)
    -> Result<std::vector<WatchedFile>> {
  // ## 为何“一次遍历 + 内存匹配”而不是逐 pattern 调 `expand_glob`
  //
  // 实测（2026-10-26）：`watch_plan_from_manifest` 会产生十几个 pattern
  //（每个 target 的 sources/include_sources + include_dirs 下的 `**/*.hpp`）。
  // 逐个调 `expand_glob` = **逐个遍历目录树**——650 个文件测出 **1.45 s**，
  // 而构建本身只要 **124 ms**。扫描占了 92%，watch 的“秒出”全被它吃掉了。
  //
  // 改法：一次 `fs::walk` 把整棵树列出来（它内部就是一次 opendir 递归），
  // 再用**已编译的 glob 规则**在内存里判定。开销从“O(patterns × 树)”降到 O(树)。
  //
  // 为何不用 `fs::walk` 的 max_depth：头文件可能有任意深度（`include/st/a/b/c.hpp`），
  // 而目录排除（build/.git/tmp）已经在 `is_excluded_directory` 里做了，所以全深度也可控。
  // 为何不用 `fs::walk` 全树：实测（2026-10-26）全树遍历（含 `third_party` 与 `build`）
  // 反而**更慢**（1.45 s → 3.68 s）——它要进 `build/dev/obj`（369 个 .o）与
  // `third_party/quickjs`、`third_party/sqlite` 等大子树，而这些**没有一个会匹配**。
  //
  // 正确做法：只遍历“模式字面前缀”指向的子树（`src`、`include`、`tests` 等），
  // 而这些前缀本身很少（~5 个）。这与 `expand_glob` 的策略一致，但**每棵子树只走一次**
  // （原实现是“每个 pattern 走一次”，十几个 pattern 就把 `src/` 走了十几遍）。
  std::set<std::string> subtrees;
  for (const auto& pattern : include) {
    const std::string normalized = fs::normalize(pattern);
    const std::size_t wildcard = normalized.find_first_of("*[?");
    // 取到最后一个 `/` 之前作为子树（`src/**/*.cpp` → `src`；`src/pkg/*.cpp` → `src/pkg`）
    std::string prefix = wildcard == std::string::npos ? normalized : normalized.substr(0, wildcard);
    const std::size_t slash = prefix.find_last_of('/');
    // 没通配符的全字面 pattern（如 `st.pkg`）单独处理，不进子树
    if (wildcard == std::string::npos) continue;
    prefix = slash == std::string::npos ? std::string{} : prefix.substr(0, slash);
    // 空前缀（如 `**/*.cpp`）：通配符在段首 → 从根开始扫全部。
    // 不能直接 `continue`（那会真的一个文件都不监视），也不能传空串——
    // 用 `"."` 显式表达“根”。
    // 剪到**最浅的已知根**（`src/pkg` 与 `src/core` 只留 `src`）在下面的合并里做。
    subtrees.insert(prefix.empty() ? std::string(".") : prefix);
  }

  // 把子树合并：若 A 是 B 的前缀，只保留 A
  std::vector<std::string> roots;
  for (const auto& dir : subtrees) {
    bool covered = false;
    for (const auto& kept : roots) {
      if (dir == kept || (dir.size() > kept.size() && dir.starts_with(kept) &&
                          dir[kept.size()] == '/')) {
        covered = true;
        break;
      }
    }
    if (covered) continue;
    // 新区可能覆盖旧项（如先收了 `src/pkg` 再收 `src`）
    std::erase_if(roots, [&](const std::string& kept) {
      return kept.size() > dir.size() && kept.starts_with(dir) && kept[dir.size()] == '/';
    });
    roots.push_back(dir);
  }

  std::vector<fs::DirEntry> entries;
  for (const auto& dir : roots) {
    auto walked = fs::walk(fs::join(root, dir), 64);
    if (!walked) {
      ST_LOG_INFO("watch：子树遍历失败 {}：{}", dir, walked.error().message);
      continue;
    }
    for (auto& entry : *walked) {
      // ⚠ `walk` 返回的 `path` 已经是**相对传入目录**的完整相对路径
      //（实测：`walk(root/src/pkg)` 返回 `src/pkg/b.cpp`，**不是** `b.cpp`）——
      // 所以这里**不能再拼 `dir`**。第一版拼了，得到 `src/src/a.cpp`，
      // 于是 glob 一个都不匹配、监视列表恒为空（而代码不报错，看起来只是“没变化”）。
      if (!fs::is_absolute(entry.path) && !entry.path.starts_with(dir)) {
        entry.path = fs::join(dir, entry.path);
      }
      entries.push_back(std::move(entry));
    }
  }

  // glob 匹配直接用框架已有的 `fs::match_glob`（`*` 段内任意、`?` 单字符、`**` 跨目录）。
  //
  // ## 为何不自己写（实测代价：写了一版全错）
  //
  // 第一版用「两指针 + 回溯」的经典 wildcard 算法，把「`**` 跨目录 / `*` 不跨目录」
  // 的状态藏在回溯变量里——一旦回溯到更早的星就丢了。结果 `src/**/*.cpp` 对
  // `src/math/color.cpp` 返回 **false**，监视列表从 650 掉到 397（漏了 110 个文件），
  // 而 watch 看起来一切正常：只是“改了文件不触发构建”。
  //
  // 这是「先查有没有现成实现」那条纪律的实例：`fs::match_glob` 早就在
  // `include/st/core/fs.hpp` 里、有文档、有测试，我却重写了一遍。


  std::vector<std::string> include_rules;
  include_rules.reserve(include.size());
  for (const auto& pattern : include) include_rules.push_back(fs::normalize(pattern));
  std::vector<std::string> exclude_rules;
  exclude_rules.reserve(exclude.size());
  for (const auto& pattern : exclude) exclude_rules.push_back(fs::normalize(pattern));

  std::vector<WatchedFile> out;
  out.reserve(entries.size() / 4);
  for (const auto& entry : entries) {
    if (entry.is_dir) continue;
    const std::string rel = relative_slash(root, entry.path);
    if (is_excluded_directory(rel)) continue;
    if (!has_watchable_extension(rel)) continue;

    bool wanted = false;
    for (const auto& rule : include_rules) {
      const bool hit = fs::match_glob(rule, rel);
      if (hit) {
        wanted = true;
        break;
      }
    }
    if (!wanted) continue;
    for (const auto& rule : exclude_rules) {
      if (fs::match_glob(rule, rel)) {
        wanted = false;
        break;
      }
    }
    if (!wanted) continue;

    // ⚠ 必须用**完整路径** stat：`entry.path` 是相对 root 的，而本函数不保证
    // 调用时 cwd == root（实测：测试里 root 是临时目录，cwd 是工程根——
    // `entry.path` 指向的文件根本不在 cwd 下，`modified_ns` 恒返回 nullopt，
    // 于是所有文件都被“读不到”静默跳过，结果是**空的监视列表、不报错**）。
    const std::string full_path = fs::join(root, rel);
    const auto mtime = fs::modified_ns(full_path);
    const auto size = fs::file_size(full_path);
    if (!mtime.has_value() || !size.has_value()) {
      continue;
    }
    out.push_back(WatchedFile{fs::normalize(full_path), *mtime, *size});
  }
  std::ranges::sort(out, {}, &WatchedFile::path);
  return out;
}

auto diff_watched(const std::vector<WatchedFile>& before, const std::vector<WatchedFile>& after)
    -> std::vector<std::string> {
  // 两个快照都按 path 有序（`collect_watched_files` 保证），用归并比较——O(n) 而非 map。
  std::vector<std::string> changed;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < before.size() || j < after.size()) {
    if (j >= after.size() || (i < before.size() && before[i].path < after[j].path)) {
      changed.push_back(before[i].path);   // 被删
      ++i;
    } else if (i >= before.size() || after[j].path < before[i].path) {
      changed.push_back(after[j].path);    // 新增
      ++j;
    } else {
      if (before[i].mtime_ns != after[j].mtime_ns || before[i].size != after[j].size) {
        changed.push_back(after[j].path);  // 改动
      }
      ++i;
      ++j;
    }
  }
  return changed;
}

auto watch_plan_from_manifest(const Manifest& manifest) -> Result<std::vector<WatchedFile>> {
  std::vector<std::string> include;
  std::vector<std::string> exclude;

  // ① 清单自身
  include.push_back("st.pkg");

  // ② 工程级与各 target 的源（含测试）
  for (const auto& pattern : manifest.sources) include.push_back(pattern);
  for (const auto& pattern : manifest.tests) include.push_back(pattern);
  for (const auto& target : manifest.targets) {
    for (const auto& pattern : target.sources) include.push_back(pattern);
    // 白名单目标：它只编这些，所以也只盯这些（盯别的等于白白多扫文件）
    for (const auto& pattern : target.include_sources) include.push_back(pattern);
    // 测试目标（`<target>_tests` 是约定名）
  }
  // 库源（`sources` 之外的顶层 `src/**`）——实际构建会编它们，所以也得盯
  include.push_back("src/**/*.cpp");

  // ③ 头文件目录：全收（头是最大的一类重编来源，漏一个就白等）
  for (const auto& dir : manifest.include_dirs) {
    include.push_back(std::format("{}/**/*.hpp", dir));
  }

  // ④ 排除：产物/缓存/VCS/第三方（第三方源码数量大且几乎不变）
  exclude.push_back("build/**");
  exclude.push_back(".st/**");
  exclude.push_back("tmp/**");
  exclude.push_back(".git/**");
  exclude.push_back("third_party/**");

  return collect_watched_files(manifest.directory, include, exclude);
}

}  // namespace st::pkg
