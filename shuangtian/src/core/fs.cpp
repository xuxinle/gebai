#include "st/core/fs.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <array>
#include <fstream>
#include <system_error>
#include <thread>
#include <vector>

#include "st/core/string.hpp"

namespace st::fs {
namespace {

/// UTF-8 文本 → 文件系统路径。
///
/// **这是全框架路径处理的唯一入口**，跨平台语义就钉在这里：
/// 本框架对外一律用 **UTF-8 `std::string`** 表示路径；但 Windows 上
/// `std::filesystem::path(std::string)` 会按**本地 ANSI 代码页**解释（如 GBK），
/// 于是含中文的路径会直接找不到文件（Linux 上却一切正常——典型的“写的人测不出来”的缺陷）。
/// Windows 必须显式从 UTF-8 转为宽字符（宽字符才是它的真 Unicode 接口）。
[[nodiscard]] auto to_path(std::string_view utf8) -> std::filesystem::path {
#if defined(_WIN32)
  // **绝不允许抛异常**：路径字符串的来源极杂（PATH 环境变量、外部清单、命令行），
  // 只要其中任何一条含非法 UTF-8 字节（例如安装器写下的 GBK 目录名），
  // MSVC 的 `path(u8string)` 就会抛 `filesystem_error`——而本函数在"判断文件存不存在"
  // 这类纯查询路径上被调用，于是**整个进程直接 terminate**。
  // 实测：`st build --toolchain=mingw`（编译器不在 PATH 里，要逐条扫 PATH）因此 abort，
  // 连错误信息都来不及输出。排错成本极高，故此处宁可退化为“按字节放宽”也不抛：
  // 最差结果是“找不到那个文件”（正常的 NotFound），而不是“工具无声崩掉”。
  try {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
  } catch (const std::exception&) {
    // 非法 UTF-8：逐字节放宽（字节值保留，不解释为多字节序列）
  }
  std::wstring widened;
  widened.reserve(utf8.size());
  for (const char byte : utf8) {
    widened.push_back(static_cast<wchar_t>(static_cast<unsigned char>(byte)));
  }
  return std::filesystem::path(std::move(widened));
#else
  return std::filesystem::path(utf8);
#endif
}

/// 文件系统路径 → UTF-8 文本（返回的永远是 UTF-8，不会因为平台而变编码）。
///
/// **同样不得抛异常**：Windows 上宽→窄（`generic_string()`）会按本地代码页转换，
/// 遇到代码页表达不了的字符就抛 `filesystem_error`。实测：`st test` 在扫描源码目录时
/// 因某个文件名转不过去而直接 abort（连测试都没开始跑）；同时它也是**编码错误**——
/// 中文路径会变成 GBK 字节流，与“路径一律 UTF-8”的契约相抵。
[[nodiscard]] auto to_utf8(const std::filesystem::path& path) -> std::string {
  try {
    const std::u8string encoded = path.generic_u8string();
    return std::string(encoded.begin(), encoded.end());
  } catch (const std::exception&) {
#if defined(_WIN32)
    // 宽串含非法 UTF-16（语言对代理项）：退化为“字节保留”，路径会被当不存在处理
    const std::wstring& wide = path.native();
    std::string narrow;
    narrow.reserve(wide.size());
    for (const wchar_t unit : wide) {
      narrow.push_back(static_cast<char>(static_cast<unsigned char>(unit & 0xFFU)));
    }
    return narrow;
#else
    // POSIX：native 本身就是字节串（char），“字节保留”直接取原值
    return path.native();
#endif
  }
}

namespace sys = std::filesystem;

/// 内部沿用原名（实现委托给上面的 `to_path`/`to_utf8`，避免大范围重命名）。
[[nodiscard]] auto to_std_path(std::string_view path) -> sys::path { return to_path(path); }

[[nodiscard]] auto from_std_path(const sys::path& path) -> std::string { return to_utf8(path); }

[[nodiscard]] auto io_error(std::string_view what, std::string_view path, const std::error_code& ec) -> Error {
  return make_error(ErrorCode::Io,
                    std::string(what).append(" '").append(path).append("': ").append(ec.message()));
}

/// stdio 句柄 RAII（字节精确读写用；文本读写走 ifstream）。
class FileHandle {
 public:
  static auto open(std::string_view path, const char* mode) -> Result<FileHandle> {
    const std::string name(path);
    std::FILE* handle = std::fopen(name.c_str(), mode);
    if (handle == nullptr) {
      return unexpected(ErrorCode::NotFound, std::string("open failed '").append(path).append("'"));
    }
    return FileHandle(handle);
  }

  FileHandle(const FileHandle&) = delete;
  auto operator=(const FileHandle&) -> FileHandle& = delete;
  FileHandle(FileHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  auto operator=(FileHandle&& other) noexcept -> FileHandle& {
    if (this != &other) {
      close();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  ~FileHandle() { close(); }

  [[nodiscard]] auto raw() noexcept -> std::FILE* { return handle_; }

 private:
  explicit FileHandle(std::FILE* handle) noexcept : handle_(handle) {}
  void close() noexcept {
    if (handle_ != nullptr) {
      std::fclose(handle_);
      handle_ = nullptr;
    }
  }
  std::FILE* handle_{nullptr};
};

/// 段内通配（`*` / `?`）匹配：两指针 + 回溯，无递归深度风险。
[[nodiscard]] auto match_segment(std::string_view pattern, std::string_view text) noexcept -> bool {
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t star = std::string_view::npos;
  std::size_t backtrack = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p;
      backtrack = t;
      ++p;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      ++backtrack;
      t = backtrack;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

[[nodiscard]] auto match_segments(const std::vector<std::string_view>& pattern, std::size_t pi,
                                  const std::vector<std::string_view>& parts, std::size_t si) -> bool {
  if (pi == pattern.size()) return si == parts.size();
  if (pattern[pi] == "**") {
    for (std::size_t skip = si; skip <= parts.size(); ++skip) {
      if (match_segments(pattern, pi + 1, parts, skip)) return true;
    }
    return false;
  }
  if (si == parts.size()) return false;
  if (!match_segment(pattern[pi], parts[si])) return false;
  return match_segments(pattern, pi + 1, parts, si + 1);
}

}  // namespace

auto read_text(std::string_view path) -> Result<std::string> {
  std::ifstream stream(to_std_path(path), std::ios::binary);
  if (!stream) return unexpected(ErrorCode::NotFound, std::string("read failed '").append(path).append("'"));
  std::string content;
  // **读到 EOF，不用 `seekg/tellg` 定长读**：`/proc`、`/sys`、cgroup 的虚拟文件报出的 size 是 0
  // （内容却非空），按 size 读会静默得到空字符串——而"读系统状态"恰好是这些文件的唯一用途。
  // 实测后果：cgroup 内存上限探测永远拿到 0，使"按内存推导编译并发"失效。
  std::array<char, 16 * 1024> buffer{};
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize taken = stream.gcount();
    if (taken > 0) content.append(buffer.data(), static_cast<std::size_t>(taken));
  }
  return content;
}

auto write_text(std::string_view path, std::string_view content) -> Status {
  if (auto status = ensure_parent(path); !status) return status;
  std::ofstream stream(to_std_path(path), std::ios::binary | std::ios::trunc);
  if (!stream) return unexpected(ErrorCode::Io, std::string("write failed '").append(path).append("'"));
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!stream) return unexpected(ErrorCode::Io, std::string("write incomplete '").append(path).append("'"));
  return ok();
}

auto append_text(std::string_view path, std::string_view content) -> Status {
  if (auto status = ensure_parent(path); !status) return status;
  std::ofstream stream(to_std_path(path), std::ios::binary | std::ios::app);
  if (!stream) return unexpected(ErrorCode::Io, std::string("append failed '").append(path).append("'"));
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!stream) return unexpected(ErrorCode::Io, std::string("append incomplete '").append(path).append("'"));
  return ok();
}

auto read_bytes(std::string_view path) -> Result<Bytes> {
  auto file = FileHandle::open(path, "rb");
  if (!file) return forward_error(file.error());
  if (std::fseek(file->raw(), 0, SEEK_END) != 0) {
    return unexpected(ErrorCode::Io, std::string("seek failed '").append(path).append("'"));
  }
  const long size = std::ftell(file->raw());
  if (size < 0) return unexpected(ErrorCode::Io, std::string("size failed '").append(path).append("'"));
  if (std::fseek(file->raw(), 0, SEEK_SET) != 0) {
    return unexpected(ErrorCode::Io, std::string("seek failed '").append(path).append("'"));
  }
  Bytes bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file->raw());
    if (read != bytes.size()) {
      return unexpected(ErrorCode::Io, std::string("read incomplete '").append(path).append("'"));
    }
  }
  return bytes;
}

auto write_bytes(std::string_view path, std::span<const std::uint8_t> data) -> Status {
  if (auto status = ensure_parent(path); !status) return status;
  auto file = FileHandle::open(path, "wb");
  if (!file) return forward_error(file.error());
  if (!data.empty()) {
    const std::size_t written = std::fwrite(data.data(), 1, data.size(), file->raw());
    if (written != data.size()) {
      return unexpected(ErrorCode::Io, std::string("write incomplete '").append(path).append("'"));
    }
  }
  return ok();
}

auto exists(std::string_view path) noexcept -> bool {
  std::error_code ec;
  return sys::exists(to_std_path(path), ec);
}

auto is_directory(std::string_view path) noexcept -> bool {
  std::error_code ec;
  return sys::is_directory(to_std_path(path), ec);
}

auto is_regular_file(std::string_view path) noexcept -> bool {
  std::error_code ec;
  return sys::is_regular_file(to_std_path(path), ec);
}

auto file_size(std::string_view path) -> Result<std::uint64_t> {
  std::error_code ec;
  const auto size = sys::file_size(to_std_path(path), ec);
  if (ec) return unexpected(ErrorCode::NotFound, io_error("size", path, ec).message);
  return static_cast<std::uint64_t>(size);
}

auto modified_ns(std::string_view path) -> Result<std::int64_t> {
  std::error_code ec;
  const auto stamp = sys::last_write_time(to_std_path(path), ec);
  if (ec) return unexpected(ErrorCode::NotFound, io_error("mtime", path, ec).message);
  const auto since = stamp.time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(since).count();
}

auto list_dir(std::string_view path) -> Result<std::vector<DirEntry>> {
  std::error_code ec;
  sys::directory_iterator iterator(to_std_path(path), ec);
  if (ec) return unexpected(ErrorCode::NotFound, io_error("list", path, ec).message);
  std::vector<DirEntry> entries;
  for (const auto& item : iterator) {
    DirEntry entry;
    entry.name = to_utf8(item.path().filename());
    entry.path = from_std_path(item.path());
    entry.is_dir = item.is_directory(ec);
    if (!entry.is_dir && item.is_regular_file(ec)) {
      const auto size = item.file_size(ec);
      entry.size = ec ? 0U : static_cast<std::uint64_t>(size);
    }
    entries.push_back(std::move(entry));
  }
  std::ranges::sort(entries, {}, &DirEntry::name);
  return entries;
}

auto walk(std::string_view root, std::uint32_t max_depth) -> Result<std::vector<DirEntry>> {
  if (!is_directory(root)) {
    return unexpected(ErrorCode::NotFound, std::string("not a directory: ").append(root));
  }
  std::vector<DirEntry> entries;
  const auto base = to_std_path(root);
  std::error_code ec;
  for (auto iterator = sys::recursive_directory_iterator(
           base, sys::directory_options::skip_permission_denied, ec);
       iterator != sys::recursive_directory_iterator(); iterator.increment(ec)) {
    if (ec) return unexpected(ErrorCode::Io, io_error("walk", root, ec).message);
    const auto depth = static_cast<std::uint32_t>(iterator.depth()) + 1U;
    if (depth > max_depth) {
      iterator.disable_recursion_pending();
      continue;
    }
    const auto& item = *iterator;
    DirEntry entry;
    entry.name = to_utf8(item.path().filename());
    entry.path = from_std_path(item.path().lexically_relative(base));
    entry.is_dir = item.is_directory(ec);
    if (!entry.is_dir && item.is_regular_file(ec)) {
      const auto size = item.file_size(ec);
      entry.size = ec ? 0U : static_cast<std::uint64_t>(size);
    }
    entries.push_back(std::move(entry));
  }
  std::ranges::sort(entries, {}, &DirEntry::path);
  return entries;
}

auto create_directories(std::string_view path) -> Status {
  std::error_code ec;
  sys::create_directories(to_std_path(path), ec);
  if (ec && ec != std::errc::file_exists) {
    return unexpected(ErrorCode::Io, io_error("mkdir", path, ec).message);
  }
  return ok();
}

auto ensure_parent(std::string_view path) -> Status {
  const std::string dir = parent(path);
  if (dir.empty()) return ok();
  if (is_directory(dir)) return ok();
  return create_directories(dir);
}

auto remove_all(std::string_view path) -> Status {
  std::error_code ec;
  sys::remove_all(to_std_path(path), ec);
  if (ec) return unexpected(ErrorCode::Io, io_error("remove", path, ec).message);
  return ok();
}

auto remove_file(std::string_view path) -> Status {
  std::error_code ec;
  sys::remove(to_std_path(path), ec);
  if (ec) return unexpected(ErrorCode::Io, io_error("unlink", path, ec).message);
  return ok();
}

auto rename(std::string_view from, std::string_view to) -> Status {
  if (auto status = ensure_parent(to); !status) return status;
  std::error_code ec;
  // Windows：刚写完的文件可能短暂地被索引/杀毒进程占用，`rename` 会以"另一个进程正在
  // 使用此文件"失败（实测：并行编译时偶发地把整次构建弄挂）。这是可恢复的瞬态错误，
  // 短暂重试即可——不重试的代价是"构建偶尔神秘失败一次"，最难排查的那类问题。
  constexpr int kAttempts = 12;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    ec.clear();
    sys::rename(to_std_path(from), to_std_path(to), ec);
    if (!ec) return ok();
#if defined(_WIN32)
    // 32 = ERROR_SHARING_VIOLATION，5 = ERROR_ACCESS_DENIED（被占用时的典型错误码）
    if (ec.value() == 32 || ec.value() == 5) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5 * (attempt + 1)));
      continue;
    }
#endif
    break;
  }
  return unexpected(ErrorCode::Io, io_error("rename", from, ec).message);
}

auto copy_file(std::string_view from, std::string_view to) -> Status {
  if (auto status = ensure_parent(to); !status) return status;
  std::error_code ec;
  sys::copy_file(to_std_path(from), to_std_path(to), sys::copy_options::overwrite_existing, ec);
  if (ec) return unexpected(ErrorCode::Io, io_error("copy", from, ec).message);
  return ok();
}

auto copy_tree(std::string_view from, std::string_view to) -> Status {
  if (!is_directory(from)) {
    return unexpected(ErrorCode::NotFound, std::string("not a directory: ").append(from));
  }
  std::error_code ec;
  sys::copy(to_std_path(from), to_std_path(to),
            sys::copy_options::recursive | sys::copy_options::overwrite_existing, ec);
  if (ec) return unexpected(ErrorCode::Io, io_error("copy tree", from, ec).message);
  return ok();
}

auto current_dir() -> Result<std::string> {
  std::error_code ec;
  const auto path = sys::current_path(ec);
  if (ec) return unexpected(ErrorCode::Io, io_error("cwd", ".", ec).message);
  return from_std_path(path);
}

auto temp_dir() -> std::string {
  std::error_code ec;
  const auto path = sys::temp_directory_path(ec);
  if (ec) return "/tmp";
  return from_std_path(path);
}

auto home_dir() -> std::string {
  if (const auto home = read_env("HOME"); home.has_value() && !home->empty()) return *home;
  if (const auto profile = read_env("USERPROFILE"); profile.has_value() && !profile->empty()) return *profile;
  std::error_code ec;
  return from_std_path(sys::current_path(ec));
}

auto read_env(std::string_view name) -> std::optional<std::string> {
  const std::string key(name);
  const char* const value = std::getenv(key.c_str());
  if (value == nullptr) return std::nullopt;
  return std::string(value);
}

auto join(std::string_view base, std::string_view leaf) -> std::string {
  if (base.empty()) return std::string(leaf);
  if (leaf.empty()) return std::string(base);
  std::string out(base);
  if (out.back() != '/') out.push_back('/');
  std::string_view rest = leaf;
  while (!rest.empty() && rest.front() == '/') rest.remove_prefix(1);
  out.append(rest);
  return normalize(out);
}

auto normalize(std::string_view path) -> std::string {
  std::string input(path);
  std::ranges::replace(input, '\\', '/');
  std::vector<std::string_view> stack;
  for (auto part : split(input, '/')) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (!stack.empty() && stack.back() != "..") {
        stack.pop_back();
        continue;
      }
      if (!input.empty() && input.front() == '/') continue;
    }
    stack.push_back(part);
  }
  std::string out;
  const bool rooted = !input.empty() && input.front() == '/';
  if (rooted) out.push_back('/');
  out.append(st::join(stack, "/"));
  if (out.empty()) out = rooted ? "/" : ".";
  return out;
}

auto absolute(std::string_view path) -> Result<std::string> {
  std::error_code ec;
  const auto resolved = sys::absolute(to_std_path(path), ec);
  if (ec) return unexpected(ErrorCode::Io, io_error("absolute", path, ec).message);
  return normalize(from_std_path(resolved));
}

auto parent(std::string_view path) -> std::string {
  std::string text = normalize(path);
  const std::size_t pos = text.find_last_of('/');
  if (pos == std::string::npos) return {};
  if (pos == 0) return "/";
  return text.substr(0, pos);
}

auto file_name(std::string_view path) -> std::string {
  const std::string text = normalize(path);
  const std::size_t pos = text.find_last_of('/');
  return pos == std::string::npos ? text : text.substr(pos + 1);
}

auto extension(std::string_view path) -> std::string {
  const std::string name = file_name(path);
  const std::size_t pos = name.find_last_of('.');
  if (pos == std::string::npos || pos == 0) return {};
  return name.substr(pos);
}

auto stem(std::string_view path) -> std::string {
  const std::string name = file_name(path);
  const std::size_t pos = name.find_last_of('.');
  if (pos == std::string::npos || pos == 0) return name;
  return name.substr(0, pos);
}

auto is_absolute(std::string_view path) noexcept -> bool {
  if (path.empty()) return false;
  if (path.front() == '/' || path.front() == '\\') return true;
  return path.size() > 1 && path[1] == ':';
}

auto relative_to(std::string_view path, std::string_view base) -> std::string {
  const std::string target = normalize(path);
  const std::string root = normalize(base);
  if (target == root) return ".";
  if (target.size() > root.size() && target.compare(0, root.size(), root) == 0 &&
      target[root.size()] == '/') {
    return target.substr(root.size() + 1);
  }
  return target;
}

auto match_glob(std::string_view pattern, std::string_view path) -> bool {
  const std::vector<std::string_view> pattern_parts = split(pattern, '/');
  const std::vector<std::string_view> path_parts = split(path, '/');
  std::vector<std::string_view> clean_pattern;
  for (const auto part : pattern_parts) {
    if (!part.empty()) clean_pattern.push_back(part);
  }
  std::vector<std::string_view> clean_path;
  for (const auto part : path_parts) {
    if (!part.empty() && part != ".") clean_path.push_back(part);
  }
  return match_segments(clean_pattern, 0, clean_path, 0);
}

/// 模式里的**字面前缀目录**（第一个含通配符的段之前）与「是否全为字面」。
///
/// 用途：`expand_glob` 只需在那棵子树里遍历；全字面模式甚至不需要 walk。
/// 为何必须做（实测）：本仓库 `st.pkg` 有 14 个模式（`src/core/*.cpp` 等），
/// 而旧实现**每个模式都从根 walk 一次** → 全仓（含 `build/` 数千个产物）被扫 14 遍。
/// 实测：14 次 walk 共 **3.75s**，其中 **2.9s（78%）** 花在 `build/` 上——
/// 对匹配零贡献，却是每次 `st build`/`st test`（含空构建）都付的固定成本。
///
/// 语义上这是纯优化：`match_glob` 要求模式段与路径段**逐段匹配**，因此
/// 不以字面前缀开头的路径不可能命中——排除它们不改变结果。
struct LiteralPrefix {
  std::string dir{};
  /// 模式里**没有任何通配符**（`*?[`）：它本身就是一个路径，无需遍历。
  bool exact{false};
};

[[nodiscard]] auto literal_prefix(std::string_view pattern) -> LiteralPrefix {
  LiteralPrefix out{};
  bool saw_wildcard = false;
  std::size_t begin = 0;
  while (begin <= pattern.size()) {
    const std::size_t end = pattern.find('/', begin);
    const std::string_view segment =
        pattern.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    // 段内一旦出现通配符，之后的段都不可当字面前缀。
    if (segment.find_first_of("*?[") != std::string_view::npos) {
      saw_wildcard = true;
      break;
    }
    if (segment == "." || segment.empty()) {   // `.` 与空段（`a//b`）不影响语义
      if (end == std::string_view::npos) break;
      begin = end + 1;
      continue;
    }
    if (!out.dir.empty()) out.dir.push_back('/');
    out.dir.append(segment);
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  out.exact = !saw_wildcard;
  return out;
}

auto glob_literal_prefix(std::string_view pattern) -> std::string {
  return literal_prefix(pattern).dir;
}

auto expand_glob(std::string_view root, std::string_view pattern) -> Result<std::vector<std::string>> {
  // 只遍历模式字面前缀对应的子树（见 `literal_prefix` 的实测记录）。
  const LiteralPrefix info = literal_prefix(pattern);
  // ① 模式**全是字面**：就是一个路径。不去 walk——直接把该文件（若存在）归一化成
  //    相对 root 的路径。必要时回退到“模式相对 root”的解读（清单里两者都在用）。
  if (info.exact) {
    const std::string as_root_relative = join(root, pattern);
    if (is_regular_file(as_root_relative)) {
      return std::vector<std::string>{std::string(pattern)};
    }
    // 清单在 `expand_patterns` 里会 `join(directory, relative)`，所以这里也允许
    // “模式本身已是相对清单目录的完整路径”的解读。
    if (is_regular_file(pattern)) return std::vector<std::string>{std::string(pattern)};
    return std::vector<std::string>{};
  }
  const std::string& prefix = info.dir;
  if (prefix.empty()) {
    auto entries = walk(root);
    if (!entries) return forward_error(entries.error());
    std::vector<std::string> matches;
    for (const auto& entry : *entries) {
      if (entry.is_dir) continue;
      if (match_glob(pattern, entry.path)) matches.push_back(entry.path);
    }
    std::ranges::sort(matches);
    return matches;
  }
  const std::string base = join(root, prefix);
  if (!is_directory(base)) return std::vector<std::string>{};   // 前缀不存在 ⇒ 无匹配
  auto entries = walk(base);
  if (!entries) return forward_error(entries.error());
  std::vector<std::string> matches;
  for (const auto& entry : *entries) {
    if (entry.is_dir) continue;
    // `walk(base)` 给的是相对 base 的路径，拼回前缀后才是相对 root 的（返回值口径不变）。
    const std::string relative = join(prefix, entry.path);
    if (match_glob(pattern, relative)) matches.push_back(relative);
  }
  std::ranges::sort(matches);
  return matches;
}

auto make_temp_dir(std::string_view prefix) -> Result<std::string> {
  const std::string base = temp_dir();
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::string candidate =
        join(base, std::string(prefix).append("-").append(to_string(static_cast<std::uint64_t>(
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())) ^
            static_cast<std::uint64_t>(attempt) * std::uint64_t{2654435761ULL})));
    if (exists(candidate)) continue;
    if (auto status = create_directories(candidate); !status) return forward_error(status.error());
    return candidate;
  }
  return unexpected(ErrorCode::Busy, "无法创建唯一临时目录");
}

}  // namespace st::fs
