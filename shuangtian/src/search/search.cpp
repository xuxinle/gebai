#include "st/search/search.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

#include "st/core/thread_pool.hpp"
#include "st/search/simd_scan.hpp"

namespace st::search {
namespace {

// ——— 默认忽略的目录名（产物 / 版本控制 / 依赖）———
//
// 这些目录要么是构建产物（搜它们纯浪费，还会搜出大量编译噪音），要么是版本控制
// 内部（`.git` 的对象是压缩二进制）。用 `unordered_set` 做 O(1) 判定：每进一层目录
// 一次 hash，比线性扫小 vector 更稳（目录数往往上万）。
const std::unordered_set<std::string>& default_ignore_dirs() {
  static const std::unordered_set<std::string> dirs{
      ".git",   ".svn",  ".hg",      "build",   "out",      "dist",     "target",
      ".cache", "node_modules", ".venv", "venv", "__pycache__", ".idea", ".vscode",
      ".turbo", ".next", ".nuxt",    "coverage", "bin",     "obj",      ".gradle",
      ".pytest_cache"};
  return dirs;
}

[[nodiscard]] auto ascii_lower(char ch) noexcept -> char {
  return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] auto to_lower_copy(std::string_view text) -> std::string {
  std::string out(text);
  for (char& ch : out) ch = ascii_lower(ch);
  return out;
}

[[nodiscard]] auto trim_view(std::string_view text) -> std::string_view {
  const auto is_space = [](char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
  };
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_space(text[begin])) ++begin;
  while (end > begin && is_space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

// ——— `.gitignore` 的简单行级规则 ———
//
// 只取最常用的一小撮：空行 / `#` 注释忽略；`/xxx` 锚定到根；其余按「任意层级的
// 名字」匹配；末尾 `/` 只匹配目录；段内支持 `*` / `?`（复用 `st::fs::match_glob`）。
//
// 明确**不做**：`!` 取反、`**` 交叉层、转义空白——这些在「枚举提速」场景用不到，
// 而「假装全支持」会给出与 git 不同的结果（比不做更糟）：取反行直接跳过。
struct IgnoreRule {
  std::string pattern{};
  bool anchored{false};
  bool dir_only{false};
};

[[nodiscard]] auto parse_gitignore(const std::string& text) -> std::vector<IgnoreRule> {
  std::vector<IgnoreRule> rules;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t eol = text.find('\n', begin);
    const std::size_t end = eol == std::string::npos ? text.size() : eol;
    std::string_view line(text.data() + begin, end - begin);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    begin = (eol == std::string::npos) ? text.size() + 1 : eol + 1;
    if (line.empty() || line.front() == '#' || line.front() == '!') continue;
    IgnoreRule rule;
    if (line.back() == '/') {
      rule.dir_only = true;
      line.remove_suffix(1);
    }
    if (!line.empty() && line.front() == '/') {
      rule.anchored = true;
      line.remove_prefix(1);
    }
    if (line.empty()) continue;
    rule.pattern = std::string(line);
    rules.push_back(std::move(rule));
  }
  return rules;
}

[[nodiscard]] auto ignored_by(const std::vector<IgnoreRule>& rules, const std::string& relative,
                              bool is_dir) -> bool {
  if (rules.empty()) return false;
  const std::size_t slash = relative.find_last_of('/');
  const std::string leaf = slash == std::string::npos ? relative : relative.substr(slash + 1);
  for (const IgnoreRule& rule : rules) {
    if (rule.dir_only && !is_dir) continue;
    if (rule.anchored) {
      if (st::fs::match_glob(rule.pattern, relative)) return true;
      continue;
    }
    if (st::fs::match_glob(rule.pattern, leaf)) return true;
    if (st::fs::match_glob(rule.pattern, relative)) return true;
    // 结尾匹配：gitignore 里 `build` 这一行对 `x/build` 也生效。
    const std::string suffix = "/" + rule.pattern;
    if (relative.size() >= suffix.size() &&
        relative.compare(relative.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return true;
    }
  }
  return false;
}

// ——— 迷你正则子集 ———
//
// 支持：字面量、`.`、`.*`、`^` / `$` 锚定、`\d` / `\w` / `\s`。
// **不做**集合 / 分组 / 量词：假装支持会静默给出错结果，不如明确不支持。
// 返回首个匹配的起始字节列；找不到返回 nullopt。
[[nodiscard]] auto match_regex_at(std::string_view pattern, std::string_view text)
    -> std::optional<std::size_t> {
  bool anchored_start = false;
  if (!pattern.empty() && pattern.front() == '^') {
    anchored_start = true;
    pattern.remove_prefix(1);
  }
  bool anchored_end = false;
  if (!pattern.empty() && pattern.back() == '$') {
    anchored_end = true;
    pattern.remove_suffix(1);
  }
  const std::size_t last_start = anchored_start ? 0 : text.size();
  for (std::size_t start = 0; start <= last_start; ++start) {
    std::size_t cursor = start;
    std::size_t pattern_at = 0;
    bool ok = true;
    while (pattern_at < pattern.size()) {
      const char token = pattern[pattern_at];
      if (token == '\\' && pattern_at + 1 < pattern.size()) {
        if (cursor >= text.size()) {
          ok = false;
          break;
        }
        const char klass = pattern[pattern_at + 1];
        const char ch = text[cursor];
        const bool hit = (klass == 'd' && ch >= '0' && ch <= '9') ||
                         (klass == 'w' && is_word_char(static_cast<unsigned char>(ch))) ||
                         (klass == 's' && (ch == ' ' || ch == '\t'));
        if (!hit) {
          ok = false;
          break;
        }
        ++cursor;
        pattern_at += 2;
        continue;
      }
      if (token == '.' && pattern_at + 1 < pattern.size() && pattern[pattern_at + 1] == '*') {
        // `.*` 贪婪回溯：从长到短试剩余模式能否在此处之后匹配。
        const std::string_view rest = pattern.substr(pattern_at + 2);
        bool matched = false;
        for (std::size_t probe = text.size() + 1; probe-- > cursor;) {
          if (match_regex_at(rest, text.substr(probe)).has_value()) {
            matched = true;
            break;
          }
        }
        if (!matched) {
          ok = false;
          break;
        }
        pattern_at = pattern.size();
        break;
      }
      if (token == '.') {
        if (cursor >= text.size()) {
          ok = false;
          break;
        }
        ++cursor;
        ++pattern_at;
        continue;
      }
      if (cursor >= text.size() || text[cursor] != token) {
        ok = false;
        break;
      }
      ++cursor;
      ++pattern_at;
    }
    if (ok && pattern_at >= pattern.size() && (!anchored_end || cursor == text.size())) {
      return start;
    }
  }
  return std::nullopt;
}

/// 在**单行**上定位首个命中，返回列（0 起字节）。
///
/// 字面量 + 区分大小写走 SIMD `find_substring`（最快路径）；不区分大小写走
/// SIMD `find_byte_case_insensitive_from` 取候选 + 逐字节折叠校验——比「整行转小写
/// 再搜」少一次全量写内存与一次额外读。
[[nodiscard]] auto locate_in_line(std::string_view line, std::string_view needle,
                                  const SearchOptions& options) -> std::optional<std::size_t> {
  if (needle.empty()) return std::nullopt;
  if (options.regex) {
    if (options.case_sensitive) return match_regex_at(needle, line);
    return match_regex_at(to_lower_copy(needle), to_lower_copy(line));
  }
  const auto data = simd::as_bytes(line);
  const std::size_t size = line.size();
  if (size < needle.size()) return std::nullopt;
  const auto word_boundary_ok = [&](std::size_t at) {
    const std::size_t after = at + needle.size();
    const bool left_ok = at == 0 || !is_word_char(data[at - 1]);
    const bool right_ok = after >= size || !is_word_char(data[after]);
    return left_ok && right_ok;
  };
  if (options.case_sensitive) {
    const auto needle_bytes = simd::as_bytes(needle);
    std::size_t from = 0;
    while (from + needle.size() <= size) {
      const std::size_t at_in_window =
          simd::find_substring(data.data() + from, size - from, needle_bytes.data(), needle.size());
      if (at_in_window == size - from) return std::nullopt;
      const std::size_t at = from + at_in_window;
      if (!options.whole_word || word_boundary_ok(at)) return at;
      from = at + 1;
    }
    return std::nullopt;
  }
  const auto first_byte = static_cast<std::uint8_t>(needle.front());
  std::size_t from = 0;
  while (from + needle.size() <= size) {
    const std::size_t at = simd::find_byte_case_insensitive_from(data.data(), size, from, first_byte);
    if (at + needle.size() > size) return std::nullopt;
    bool equal = true;
    for (std::size_t k = 0; k < needle.size(); ++k) {
      if (ascii_lower(static_cast<char>(data[at + k])) != ascii_lower(needle[k])) {
        equal = false;
        break;
      }
    }
    if (equal && (!options.whole_word || word_boundary_ok(at))) return at;
    from = at + 1;
  }
  return std::nullopt;
}

/// 命中前的**快速否定**：一行都不用切就能确定「这段缓冲里没有命中」。
///
/// 对字面量查询用 SIMD 扫整段缓冲；绝大多数文件因此不必进逐行切分。
/// 正则模式无法预先否定（`.*`/锚定使字面量前缀不可靠），直接返回 true（进逐行）。
[[nodiscard]] auto may_contain(std::string_view content, std::string_view needle,
                               const SearchOptions& options) -> bool {
    if (options.regex || needle.empty()) return true;
  const auto data = simd::as_bytes(content);
  const std::size_t size = content.size();
  if (size < needle.size()) return false;
  if (options.case_sensitive) {
    const auto needle_bytes = simd::as_bytes(needle);
    return simd::find_substring(data.data(), size, needle_bytes.data(), needle.size()) != size;
  }
  // 不区分大小写：只要整段里连「首字母（两种形态）」都找不到，就一定无命中。
  // 首字母存在时可能误报为「有」（只当快速否定用，宁多扫不放过）。
  return simd::find_byte_case_insensitive_from(data.data(), size, 0,
                                               static_cast<std::uint8_t>(needle.front())) != size;
}

/// 逐行扫描并把命中追加进 `hits`（`path` / `relative` 由调用方填）。
void scan_lines(std::string_view content, std::string_view needle, const SearchOptions& options,
                std::string_view path, std::string_view relative, std::vector<Hit>& hits,
                std::size_t max_hits_for_file) {
  const auto bytes = simd::as_bytes(content);
  const std::size_t size = content.size();
  std::size_t line_no = 0;
  std::size_t begin = 0;
  std::size_t produced = 0;
  while (begin < size) {
    if (max_hits_for_file != 0 && produced >= max_hits_for_file) return;
    // 行尾定位是内容扫描的主成本——走 SIMD，不是逐字节循环。
    const std::size_t eol = simd::find_byte_from(bytes.data(), size, begin, '\n');
    std::string_view line(content.data() + begin, eol - begin);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    ++line_no;
    if (const auto column = locate_in_line(line, needle, options); column.has_value()) {
      Hit hit;
      hit.path = std::string(path);
      hit.relative = std::string(relative);
      hit.line = line_no;
      hit.column = *column;
      if (options.capture_line_text) {
        std::string_view trimmed = trim_view(line);
        if (trimmed.size() > options.max_line_bytes) trimmed = trimmed.substr(0, options.max_line_bytes);
        hit.line_text = std::string(trimmed);
      }
      hits.push_back(std::move(hit));
      ++produced;
    }
    if (eol >= size) break;
    begin = eol + 1;
  }
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————
// 枚举
// ————————————————————————————————————————————————————————————————————————

auto enumerate(std::string_view root, const EnumerateOptions& options) -> Result<EnumerateResult> {
  if (root.empty() || !st::fs::exists(root) || !st::fs::is_directory(root)) {
    return unexpected(ErrorCode::NotFound, std::string("不是目录：").append(root));
  }
  EnumerateResult result;
  std::vector<IgnoreRule> gitignore_rules;
  if (options.respect_gitignore) {
    if (auto text = st::fs::read_text(st::fs::join(root, ".gitignore")); text.has_value()) {
      gitignore_rules = parse_gitignore(*text);
    }
  }
  const auto& default_dirs = default_ignore_dirs();
  const auto is_extra_ignored = [&](const std::string& name) {
    return std::find(options.extra_ignore_dirs.begin(), options.extra_ignore_dirs.end(), name) !=
           options.extra_ignore_dirs.end();
  };

  // 显式栈并行 bfs：每层目录一次 `list_dir`，目录间用 `ThreadPool` 并行（目录枚举是
  // gbcode 快速打开/搜索首屏的关键路径，串行扫大仓库会很慢）。文件判筛在生成时完成。
  struct Frame {
    std::string dir{};
    std::string relative{};
    std::uint32_t depth{0};
  };
  std::mutex results_mutex;
  std::mutex work_mutex;
  std::vector<Frame> work;
  work.push_back(Frame{std::string(root), std::string{}, 0});
  ThreadPool pool;
  while (!work.empty()) {
    // 本批目录一次性分发到线程池（批间串行：子目录要到本批完成后才拿到）。
    std::vector<Frame> batch;
    {
      const std::lock_guard<std::mutex> guard(work_mutex);
      batch.swap(work);
    }
    std::vector<std::future<void>> futures;
    futures.reserve(batch.size());
    for (const Frame& frame : batch) {
      futures.push_back(pool.submit([&, frame]() {
        auto listing = st::fs::list_dir(frame.dir);
        if (!listing.has_value()) return;
        std::vector<FileEntry> local_files;
        std::vector<Frame> local_dirs;
        std::uint64_t dirs_visited = 1;
        std::uint64_t files_seen = 0;
        std::uint64_t dirs_ignored = 0;
        std::uint64_t files_ignored = 0;
        std::uint64_t files_binary = 0;
        std::uint64_t files_large = 0;
        for (const auto& item : *listing) {
          if (item.name.empty() || item.name.front() == '.') continue;   // 隐藏项一律跳过
          const std::string relative =
              frame.relative.empty() ? item.name : frame.relative + "/" + item.name;
          const std::string child_path = st::fs::join(frame.dir, item.name);
          if (item.is_dir) {
            if (frame.depth + 1 > options.max_depth) continue;
            if (default_dirs.count(item.name) != 0 || is_extra_ignored(item.name) ||
                ignored_by(gitignore_rules, relative, true)) {
              ++dirs_ignored;
              continue;
            }
            local_dirs.push_back(Frame{child_path, relative, frame.depth + 1});
            continue;
          }
          ++files_seen;
          if (ignored_by(gitignore_rules, relative, false)) {
            ++files_ignored;
            continue;
          }
          if (item.size > options.max_file_bytes) {
            ++files_large;
            continue;
          }
          // 二进制筛选：按策略（默认按扩展名，零 IO；内容档才开一次头）。
          if (options.binary_filter == BinaryFilter::kByExtension &&
              looks_binary_by_extension(item.name)) {
            ++files_binary;
            continue;
          }
          if (options.binary_filter == BinaryFilter::kByContent) {
            auto head = st::fs::read_span(child_path, 0, 8192);
            if (head.has_value() && looks_binary(simd::as_text(*head))) {
              ++files_binary;
              continue;
            }
          }
          FileEntry entry;
          entry.path = child_path;
          entry.relative = relative;
          entry.name = item.name;
          entry.size = item.size;
          local_files.push_back(std::move(entry));
        }
        const std::lock_guard<std::mutex> guard(results_mutex);
        result.stats.dirs_visited += dirs_visited;
        result.stats.files_seen += files_seen;
        result.stats.dirs_ignored += dirs_ignored;
        result.stats.files_ignored += files_ignored;
        result.stats.files_binary += files_binary;
        result.stats.files_large += files_large;
        result.files.insert(result.files.end(), local_files.begin(), local_files.end());
        {
          const std::lock_guard<std::mutex> work_guard(work_mutex);
          work.insert(work.end(), local_dirs.begin(), local_dirs.end());
        }
      }));
    }
    for (auto& future : futures) future.get();
  }
  result.stats.files_kept = result.files.size();
  std::ranges::sort(result.files, {}, &FileEntry::relative);
  return result;
}

auto filter_paths(const std::vector<FileEntry>& files, std::string_view query, std::size_t limit)
    -> std::vector<std::size_t> {
  const std::size_t cap = limit == 0 ? files.size() : limit;
  if (query.empty()) {
    std::vector<std::size_t> all(cap);
    for (std::size_t i = 0; i < cap; ++i) all[i] = i;
    return all;
  }
  const std::string needle = to_lower_copy(query);
  struct Scored {
    std::size_t index{0};
    int longest_run{0};  ///< 最长连续命中段（越长越好）
    int last_pos{0};     ///< 末次命中位置（越小越好：优先文件名尾段）
    int path_len{0};     ///< 路径长度（越短越好）
  };
  std::vector<Scored> matched;
  matched.reserve(std::min<std::size_t>(files.size(), 4096));
  // 候选上限：子序列查询（如单字母）在十万文件仓库里能命中绝大多数，全排序不值当。
  constexpr std::size_t kMaxCandidates = 20000;
  for (std::size_t index = 0; index < files.size(); ++index) {
    const std::string& relative = files[index].relative;
    std::size_t query_at = 0;
    int run = 0;
    int longest_run = 0;
    int last_pos = -1;
    for (std::size_t i = 0; i < relative.size() && query_at < needle.size(); ++i) {
      if (ascii_lower(relative[i]) != needle[query_at]) continue;
      run = (last_pos >= 0 && static_cast<std::ptrdiff_t>(i) == last_pos + 1) ? run + 1 : 1;
      longest_run = std::max(longest_run, run);
      last_pos = static_cast<int>(i);
      ++query_at;
    }
    if (query_at < needle.size()) continue;
    matched.push_back(Scored{index, longest_run, last_pos, static_cast<int>(relative.size())});
    if (matched.size() >= kMaxCandidates) break;
  }
  std::ranges::sort(matched, [](const Scored& a, const Scored& b) {
    if (a.longest_run != b.longest_run) return a.longest_run > b.longest_run;
    if (a.last_pos != b.last_pos) return a.last_pos < b.last_pos;
    return a.path_len < b.path_len;
  });
  std::vector<std::size_t> out;
  out.reserve(std::min(matched.size(), cap));
  for (const Scored& item : matched) {
    out.push_back(item.index);
    if (out.size() >= cap) break;
  }
  return out;
}

// ————————————————————————————————————————————————————————————————————————
// 内容搜索
// ————————————————————————————————————————————————————————————————————————

auto looks_binary(std::string_view bytes) -> bool {
  const std::size_t limit = std::min<std::size_t>(bytes.size(), 8192);
  std::size_t control = 0;
  for (std::size_t i = 0; i < limit; ++i) {
    const auto ch = static_cast<unsigned char>(bytes[i]);
    if (ch == 0) return true;
    if (ch < 0x20 && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v') {
      ++control;
    }
  }
  return limit > 0 && control * 20 > limit;   // 控制字符 > 5%
}

auto is_word_char(unsigned char byte) noexcept -> bool {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '_';
}

auto looks_binary_by_extension(std::string_view name) -> bool {
  const std::size_t dot = name.find_last_of('.');
  if (dot == std::string_view::npos) return false;
  std::string ext;
  for (std::size_t i = dot + 1; i < name.size(); ++i) ext.push_back(ascii_lower(name[i]));
  // 已知二进制/媒体/归档/可执行/字体/编译产物扩展名。
  static const std::unordered_set<std::string> kBinaryExt{
      "png",   "jpg",  "jpeg", "gif",  "bmp",  "ico",  "webp", "tif",  "tiff", "psd",
      "zip",   "gz",   "tgz",  "bz2",  "xz",   "7z",   "rar",  "tar",  "zst",  "lz4",
      "exe",   "dll",  "so",   "dylib", "o",   "obj",  "a",    "lib",  "pdb",  "class",
      "pdf",   "doc",  "docx", "xls",  "xlsx", "ppt",  "pptx", "mp3",  "mp4",  "wav",
      "flac",  "ogg",  "avi",  "mkv",  "mov",  "webm", "ttf",  "otf",  "ttc",  "woff",
      "woff2", "eot",  "wasm", "bin",  "dat",  "db",   "sqlite", "sqlite3", "pyc",
      "pt",    "pth",  "onnx", "npy",  "npz",  "pkl",  "parquet", "ico2"};
  return kBinaryExt.count(ext) != 0;
}

auto search_buffer(std::string_view content, std::string_view query, const SearchOptions& options)
    -> std::vector<Hit> {
  std::vector<Hit> hits;
  if (query.empty() || !may_contain(content, query, options)) return hits;
  scan_lines(content, query, options, std::string_view{}, std::string_view{}, hits,
             options.max_hits_per_file);
  return hits;
}

auto search_file(std::string_view path, std::string_view query, const SearchOptions& options,
                 std::string_view relative) -> Result<std::vector<Hit>> {
  std::vector<Hit> hits;
  if (query.empty()) return hits;
  // 优先内存映射：省一次「内核→用户」的整文件拷贝，大文件尤其明显。
  if (auto mapped = st::fs::MappedFile::open(path); mapped.has_value()) {
    const auto span = mapped->span();
    const std::string_view content = simd::as_text(span);
    if (looks_binary(content) || !may_contain(content, query, options)) return hits;
    scan_lines(content, query, options, path, relative, hits, options.max_hits_per_file);
    return hits;
  }
  auto content = st::fs::read_text(path);
  if (!content.has_value()) return forward_error(content.error());
  if (looks_binary(*content) || !may_contain(*content, query, options)) return hits;
  scan_lines(*content, query, options, path, relative, hits, options.max_hits_per_file);
  return hits;
}

auto search_paths(const std::vector<FileEntry>& files, std::string_view query,
                  const SearchOptions& options,
                  const std::function<void(std::size_t, std::size_t)>& progress) -> SearchResult {
  SearchResult result;
  if (query.empty() || files.empty()) return result;
  const std::size_t total = files.size();
  result.files_scanned = total;

  // 每个文件一个独立命中桶：并行只读文件、只写各自的桶（无共享可变状态，免锁），
  // 最后按输入顺序拼接——**稳定顺序**是结果可复现的前提，不能因并行而抖。
  enum class FileState : std::uint8_t { kNoHit, kHit, kUnreadable, kBinary, kLarge };
  std::vector<std::vector<Hit>> per_file(total);
  std::vector<FileState> states(total, FileState::kNoHit);
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> done{0};

  ThreadPool pool;
  const std::size_t workers = std::max<std::size_t>(1, pool.thread_count());
  std::vector<std::future<void>> futures;
  futures.reserve(workers);
  for (std::size_t worker = 0; worker < workers; ++worker) {
    futures.push_back(pool.submit([&]() {
      for (;;) {
        const std::size_t index = next.fetch_add(1);
        if (index >= total) break;
        const FileEntry& entry = files[index];
        if (entry.size > options.max_file_bytes) {
          states[index] = FileState::kLarge;
          done.fetch_add(1, std::memory_order_relaxed);
          if (progress) progress(done.load(std::memory_order_relaxed), total);
          continue;
        }
        const auto scan = [&](std::string_view content) {
          if (looks_binary(content)) {
            states[index] = FileState::kBinary;
            return;
          }
          if (!may_contain(content, query, options)) return;
          scan_lines(content, query, options, entry.path, entry.relative, per_file[index],
                     options.max_hits_per_file);
          if (!per_file[index].empty()) states[index] = FileState::kHit;
        };
        if (auto mapped = st::fs::MappedFile::open(entry.path); mapped.has_value()) {
          const auto span = mapped->span();
          scan(simd::as_text(span));
        } else if (auto text = st::fs::read_text(entry.path); text.has_value()) {
          scan(*text);
        } else {
          states[index] = FileState::kUnreadable;
        }
        done.fetch_add(1, std::memory_order_relaxed);
        if (progress) progress(done.load(std::memory_order_relaxed), total);
      }
    }));
  }
  for (auto& future : futures) future.get();

  for (std::size_t index = 0; index < total; ++index) {
    switch (states[index]) {
      case FileState::kUnreadable: ++result.skipped_unreadable; continue;
      case FileState::kBinary: ++result.skipped_binary; continue;
      case FileState::kLarge: ++result.skipped_large; continue;
      case FileState::kHit: break;
      default: continue;
    }
    ++result.files_with_hits;
    for (auto& hit : per_file[index]) {
      if (result.hits.size() >= options.max_hits_total) {
        result.truncated = true;
        break;
      }
      result.hits.push_back(std::move(hit));
    }
    if (result.truncated) break;
  }
  return result;
}

}  // namespace st::search