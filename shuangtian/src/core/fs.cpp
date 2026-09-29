#include "st/core/fs.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

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
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
  return std::filesystem::path(utf8);
#endif
}

/// 文件系统路径 → UTF-8 文本（返回的永远是 UTF-8，不会因为平台而变编码）。
[[nodiscard]] auto to_utf8(const std::filesystem::path& path) -> std::string {
  const std::u8string encoded = path.generic_u8string();
  return std::string(encoded.begin(), encoded.end());
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
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < 0) return unexpected(ErrorCode::Io, std::string("size failed '").append(path).append("'"));
  content.resize(static_cast<std::size_t>(size));
  stream.seekg(0, std::ios::beg);
  stream.read(content.data(), size);
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
    entry.name = item.path().filename().generic_string();
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
    entry.name = item.path().filename().generic_string();
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
  sys::rename(to_std_path(from), to_std_path(to), ec);
  if (ec) return unexpected(ErrorCode::Io, io_error("rename", from, ec).message);
  return ok();
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

auto expand_glob(std::string_view root, std::string_view pattern) -> Result<std::vector<std::string>> {
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
