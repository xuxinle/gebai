// 霜天 codec —— archive.cpp：tar / tar.gz / zip 读取，ustar 打包（自研，仅用本层 deflate 与 st::hash）。
//
// 算法：
//   • tar：按 512 字节块迭代。每块先校验 header 校验和（校验字段以空格参与求和，POSIX 无符号求和），
//     再解析 name/prefix/size/typeflag；数据块数 = ceil(size/512)。支持 ustar 的 prefix+name 拼接、
//     GNU 'L'（长名）/'K'（长链接名）扩展头、pax 'x' 扩展头的 `path=` 覆盖；连续两块全零即归档结束。
//     条目过滤：只保留常规文件（'0'/NUL）与目录（'5'），符号链接/硬链接/设备/FIFO/未知类型一律跳过
//     （不落盘、不产生逃逸面）。
//   • zip：从尾部反向搜索 EOCD（0x06054b50，≤64KB 注释）→ 遍历中央目录（0x02014b50，取方法/CRC/
//     压缩与原始长度/本地头偏移/外部属性）→ 每个条目按本地头（0x04034b50）定位数据起点 →
//     方法 0 直接拷贝、方法 8 走 inflate_raw → 校验原始长度与 CRC-32。
//   • 自动识别：`PK\x03\x04` → zip；`\x1f\x8b` → gzip 解压后按 tar；否则按 tar。
//   • 解包：条目路径逐段净化（拒绝绝对路径、盘符与 `..`），再拼到目标目录下写入并核对前缀。
//
// 限制（有意为之，非缺陷）：
//   • zip 只支持单盘、非 zip64、方法 0/8；不还原权限位/时间戳/注释；
//   • tar 不解 pax 'g' 全局头的 records（仅 'x' 单文件的 path=），不还原权限位与时间戳；
//   • archive_extract_to 不创建符号链接（含链接的归档其链接条目被跳过）；
//   • 全部输入经 std::span 访问，越界即返回 Error，无指针算术、无异常、无第三方依赖。

#include "st/codec/archive.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/core/string.hpp"

namespace st::codec {
namespace {

inline constexpr std::size_t kTarBlock = 512;

// —— 端序读取（调用方保证长度） ——

[[nodiscard]] auto read_le16(std::span<const std::uint8_t> data) noexcept -> std::uint16_t {
  return static_cast<std::uint16_t>(static_cast<std::uint32_t>(data[0]) |
                                    (static_cast<std::uint32_t>(data[1]) << 8U));
}

[[nodiscard]] auto read_le32(std::span<const std::uint8_t> data) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

// —— 文本字段 ——

/// 字节序列 → 文本（遇 NUL 截断；不校验编码，路径按 UTF-8 原样保留）。
[[nodiscard]] auto to_text(std::span<const std::uint8_t> data) -> std::string {
  std::string text;
  text.reserve(data.size());
  for (const std::uint8_t byte : data) {
    if (byte == 0U) break;
    text.push_back(static_cast<char>(byte));
  }
  return text;
}

/// tar 定长字段 → 文本（截断于 NUL，去掉尾部空格）。
[[nodiscard]] auto field_text(std::span<const std::uint8_t> field) -> std::string {
  std::string text = to_text(field);
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}

/// tar 八进制数字字段 → 数值（前导空格/NUL 跳过，遇首个非八进制字符结束）。
[[nodiscard]] auto parse_octal(std::span<const std::uint8_t> field) -> Result<std::uint64_t> {
  std::uint64_t value = 0;
  bool started = false;
  for (const std::uint8_t byte : field) {
    if (byte == 0U || byte == 0x20U) {
      if (started) break;
      continue;
    }
    if (byte < static_cast<std::uint8_t>('0') || byte > static_cast<std::uint8_t>('7')) {
      return st::unexpected(
          ErrorCode::Parse, std::format("tar 数字字段含非八进制字节: 0x{:02x}", byte));
    }
    if (value > (std::numeric_limits<std::uint64_t>::max() >> 3U)) {
      return st::unexpected(ErrorCode::Overflow, "tar 八进制字段溢出");
    }
    value = value * 8U + static_cast<std::uint64_t>(byte - static_cast<std::uint8_t>('0'));
    started = true;
  }
  return value;
}

/// header 校验和判定：`stored` 与「校验字段视作空格后的字节和」一致（POSIX unsigned 求和）。
[[nodiscard]] auto tar_checksum_ok(std::span<const std::uint8_t> block, std::uint64_t stored)
    -> bool {
  std::uint64_t sum = 0;
  for (std::size_t at = 0; at < kTarBlock; ++at) {
    const bool in_checksum_field = at >= 148 && at < 156;
    sum += in_checksum_field ? 0x20U : static_cast<std::uint64_t>(block[at]);
  }
  return sum == stored;
}

/// 解析 pax 扩展头记录，取 `path=` 覆盖（`<len> <key>=<value>\n` 序列）。
[[nodiscard]] auto pax_path_override(std::span<const std::uint8_t> payload)
    -> std::optional<std::string> {
  std::size_t at = 0;
  while (at < payload.size()) {
    std::size_t space = at;
    while (space < payload.size() && payload[space] != 0x20U) ++space;
    if (space >= payload.size()) return std::nullopt;
    std::uint64_t record_length = 0;
    for (std::size_t digit = at; digit < space; ++digit) {
      if (payload[digit] < static_cast<std::uint8_t>('0') ||
          payload[digit] > static_cast<std::uint8_t>('9')) {
        return std::nullopt;
      }
      record_length = record_length * 10U +
                      static_cast<std::uint64_t>(payload[digit] - static_cast<std::uint8_t>('0'));
    }
    if (record_length == 0 || at + record_length > payload.size()) return std::nullopt;
    const std::size_t record_end = at + static_cast<std::size_t>(record_length);
    std::size_t cursor = space + 1;
    const std::size_t key_begin = cursor;
    while (cursor < record_end && payload[cursor] != static_cast<std::uint8_t>('=') &&
           payload[cursor] != 0x0AU) {
      ++cursor;
    }
    if (cursor < record_end && payload[cursor] == static_cast<std::uint8_t>('=')) {
      std::size_t value_end = record_end;
      if (value_end > cursor + 1 && payload[value_end - 1] == 0x0AU) --value_end;
      const std::string key =
          to_text(payload.subspan(key_begin, cursor - key_begin));
      if (key == "path") {
        return to_text(payload.subspan(cursor + 1, value_end - cursor - 1));
      }
    }
    at = record_end;
  }
  return std::nullopt;
}

}  // namespace

auto tar_read(std::span<const std::uint8_t> input) -> Result<std::vector<ArchiveEntry>> {
  std::vector<ArchiveEntry> entries;
  std::size_t pos = 0;
  std::size_t zero_blocks = 0;
  bool end_marker = false;
  std::optional<std::string> pending_path;
  while (pos + kTarBlock <= input.size()) {
    const std::span<const std::uint8_t> block = input.subspan(pos, kTarBlock);
    pos += kTarBlock;

    const bool all_zero = std::all_of(block.begin(), block.end(),
                                      [](std::uint8_t byte) { return byte == 0U; });
    if (all_zero) {
      ++zero_blocks;
      if (zero_blocks >= 2) {  // 归档结束标记（两块全零）
        end_marker = true;
        break;
      }
      continue;
    }
    zero_blocks = 0;

    const auto stored = parse_octal(block.subspan(148, 8));
    if (!stored) return st::forward_error(stored.error());
    if (!tar_checksum_ok(block, *stored)) {
      return st::unexpected(ErrorCode::Parse, "tar header 校验和不符（归档损坏）");
    }
    const auto size_field = parse_octal(block.subspan(124, 12));
    if (!size_field) return st::forward_error(size_field.error());
    const std::uint64_t size = *size_field;
    const std::size_t data_blocks = static_cast<std::size_t>((size + kTarBlock - 1U) / kTarBlock);
    if (data_blocks > (input.size() - pos) / kTarBlock) {
      return st::unexpected(ErrorCode::Parse, "tar 条目数据块截断");
    }
    const std::span<const std::uint8_t> payload =
        input.subspan(pos, static_cast<std::size_t>(size));
    pos += data_blocks * kTarBlock;

    const std::uint8_t type = block[156];
    if (type == static_cast<std::uint8_t>('L')) {  // GNU 长名
      pending_path = field_text(payload);
      continue;
    }
    if (type == static_cast<std::uint8_t>('K')) continue;  // GNU 长链接名：链接条目本就跳过
    if (type == static_cast<std::uint8_t>('x')) {          // pax 单文件扩展头
      if (auto override_path = pax_path_override(payload)) pending_path = std::move(*override_path);
      continue;
    }
    if (type == static_cast<std::uint8_t>('g')) continue;  // pax 全局头：本层不保留

    std::string name = field_text(block.first(100));
    const std::string prefix = field_text(block.subspan(345, 155));
    if (!prefix.empty() && field_text(block.subspan(257, 6)) == "ustar") {
      name = prefix + "/" + name;
    }
    if (pending_path) {
      name = *pending_path;
      pending_path.reset();
    }

    const bool is_dir = type == static_cast<std::uint8_t>('5');
    const bool is_regular = type == static_cast<std::uint8_t>('0') || type == 0U;
    if (!is_dir && !is_regular) continue;  // 链接/设备/FIFO/未知类型：跳过

    ArchiveEntry entry;
    entry.path = name;
    entry.is_dir = is_dir;
    if (is_regular) {
      entry.size = size;
      entry.data.assign(payload.begin(), payload.end());
    }
    entries.push_back(std::move(entry));
  }
  if (!end_marker) {
    // 缺少结束标记：截断文件或根本不是 tar（避免把任意短文件当成空归档）
    return st::unexpected(ErrorCode::Parse, "tar 缺少归档结束标记（数据截断或非 tar 数据）");
  }
  return entries;
}

auto zip_read(std::span<const std::uint8_t> input) -> Result<std::vector<ArchiveEntry>> {
  constexpr std::uint32_t kLocalSignature = 0x04034B50U;
  constexpr std::uint32_t kCentralSignature = 0x02014B50U;
  constexpr std::uint32_t kEndSignature = 0x06054B50U;
  constexpr std::size_t kEndRecordSize = 22;
  constexpr std::size_t kCentralFixedSize = 46;
  constexpr std::size_t kLocalFixedSize = 30;

  if (input.size() < kEndRecordSize) {
    return st::unexpected(ErrorCode::Parse, "zip 文件过短（缺少 EOCD）");
  }
  const std::size_t search_floor =
      input.size() > kEndRecordSize + 0xFFFFU ? input.size() - kEndRecordSize - 0xFFFFU : 0;
  std::size_t eocd = 0;
  bool found = false;
  for (std::size_t at = input.size() - kEndRecordSize;; --at) {
    if (read_le32(input.subspan(at, 4)) == kEndSignature) {
      eocd = at;
      found = true;
      break;
    }
    if (at == search_floor) break;
  }
  if (!found) return st::unexpected(ErrorCode::Parse, "zip 缺少 EOCD 记录");

  if (read_le16(input.subspan(eocd + 4, 2)) != 0U || read_le16(input.subspan(eocd + 6, 2)) != 0U) {
    return st::unexpected(ErrorCode::Unsupported, "zip 分卷（多盘）归档不支持");
  }
  const std::size_t entry_count = read_le16(input.subspan(eocd + 10, 2));
  const std::size_t central_size = read_le32(input.subspan(eocd + 12, 4));
  const std::size_t central_offset = read_le32(input.subspan(eocd + 16, 4));
  if (entry_count == 0xFFFFU || central_size == 0xFFFFFFFFU || central_offset == 0xFFFFFFFFU) {
    return st::unexpected(ErrorCode::Unsupported, "zip64 归档不支持");
  }
  if (central_offset > input.size() || central_size > input.size() - central_offset) {
    return st::unexpected(ErrorCode::Parse, "zip 中央目录越界");
  }

  std::vector<ArchiveEntry> entries;
  std::size_t pos = central_offset;
  for (std::size_t index = 0; index < entry_count; ++index) {
    if (pos + kCentralFixedSize > input.size()) {
      return st::unexpected(ErrorCode::Parse, "zip 中央目录记录截断");
    }
    if (read_le32(input.subspan(pos, 4)) != kCentralSignature) {
      return st::unexpected(ErrorCode::Parse, "zip 中央目录签名错误");
    }
    const std::uint16_t method = read_le16(input.subspan(pos + 10, 2));
    const std::uint32_t expected_crc = read_le32(input.subspan(pos + 16, 4));
    const std::uint32_t compressed_size = read_le32(input.subspan(pos + 20, 4));
    const std::uint32_t uncompressed_size = read_le32(input.subspan(pos + 24, 4));
    const std::size_t name_length = read_le16(input.subspan(pos + 28, 2));
    const std::size_t extra_length = read_le16(input.subspan(pos + 30, 2));
    const std::size_t comment_length = read_le16(input.subspan(pos + 32, 2));
    const std::uint32_t external_attributes = read_le32(input.subspan(pos + 38, 4));
    const std::uint32_t local_offset = read_le32(input.subspan(pos + 42, 4));
    const std::size_t record_size =
        kCentralFixedSize + name_length + extra_length + comment_length;
    if (record_size > input.size() - pos) {
      return st::unexpected(ErrorCode::Parse, "zip 中央目录记录越界");
    }
    if (compressed_size == 0xFFFFFFFFU || uncompressed_size == 0xFFFFFFFFU ||
        local_offset == 0xFFFFFFFFU) {
      return st::unexpected(ErrorCode::Unsupported, "zip64 条目不支持");
    }
    const std::string name = to_text(input.subspan(pos + kCentralFixedSize, name_length));
    pos += record_size;

    if (local_offset > input.size() || kLocalFixedSize > input.size() - local_offset) {
      return st::unexpected(ErrorCode::Parse, "zip 本地头越界");
    }
    if (read_le32(input.subspan(local_offset, 4)) != kLocalSignature) {
      return st::unexpected(ErrorCode::Parse, std::format("zip 本地头签名错误（条目 {}）", name));
    }
    const std::size_t local_name_length = read_le16(input.subspan(local_offset + 26, 2));
    const std::size_t local_extra_length = read_le16(input.subspan(local_offset + 28, 2));
    const std::size_t data_at =
        static_cast<std::size_t>(local_offset) + kLocalFixedSize + local_name_length +
        local_extra_length;
    if (data_at > input.size() || compressed_size > input.size() - data_at) {
      return st::unexpected(ErrorCode::Parse, std::format("zip 条目数据越界（条目 {}）", name));
    }

    ArchiveEntry entry;
    entry.path = name;
    entry.is_dir = (!name.empty() && name.back() == '/') ||
                   ((external_attributes & 0x10U) != 0U && compressed_size == 0U);
    if (entry.is_dir) {
      entries.push_back(std::move(entry));
      continue;
    }

    const std::span<const std::uint8_t> payload = input.subspan(data_at, compressed_size);
    Bytes data;
    if (method == 0U) {
      data.assign(payload.begin(), payload.end());
    } else if (method == 8U) {
      auto inflated = inflate_raw(payload);
      if (!inflated) return st::forward_error(inflated.error());
      data = std::move(*inflated);
    } else {
      return st::unexpected(ErrorCode::Unsupported,
                            std::format("zip 压缩方法 {} 不支持（仅 store/deflate）", method));
    }
    if (data.size() != uncompressed_size) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("zip 条目解压长度不符（条目 {}）", name));
    }
    if (st::hash::crc32(data) != expected_crc) {
      return st::unexpected(ErrorCode::Parse, std::format("zip 条目 CRC-32 校验失败（条目 {}）", name));
    }
    entry.size = data.size();
    entry.data = std::move(data);
    entries.push_back(std::move(entry));
  }
  return entries;
}

namespace {

/// 归档条目路径净化：拒绝空路径、绝对路径（`/`、`\`、Windows 盘符）与任何 `..` 分量。
[[nodiscard]] auto is_safe_relative_path(std::string_view path) -> bool {
  if (path.empty()) return false;
  if (path.front() == '/' || path.front() == '\\') return false;
  if (path.size() >= 2 && path[1] == ':') return false;
  for (const std::string_view part : st::split(path, '/')) {
    if (part == "..") return false;
  }
  for (const std::string_view part : st::split(path, '\\')) {
    if (part == "..") return false;
  }
  return true;
}

/// 定长字段写入（不足部分补 0，超长由调用方保证）。
void copy_field(std::span<std::uint8_t> field, std::string_view text) {
  const std::size_t count = std::min(field.size(), text.size());
  for (std::size_t at = 0; at < count; ++at) {
    field[at] = static_cast<std::uint8_t>(text[at]);
  }
}

/// tar 八进制字段写入（末字节留 NUL；字段容量 11 位八进制）。
void write_octal_field(std::span<std::uint8_t> field, std::uint64_t value) {
  const std::size_t digits = field.size() - 1;
  for (std::size_t index = 0; index < digits; ++index) {
    const std::size_t slot = digits - 1 - index;
    const unsigned shift = static_cast<unsigned>(index * 3U);
    field[slot] = static_cast<std::uint8_t>(static_cast<unsigned>('0') +
                                            static_cast<unsigned>((value >> shift) & 7U));
  }
  field.back() = 0U;
}

/// ustar 名称字段：`name`(100) + `prefix`(155)。
struct UstarName {
  std::string name{};
  std::string prefix{};
};

/// ustar 名称拆分：总长 ≤ 100 直接进 `name`；否则在某个 '/' 处拆成 `prefix`/`name` 两段。
[[nodiscard]] auto split_ustar_name(std::string_view path) -> std::optional<UstarName> {
  if (path.size() <= 100) return UstarName{std::string(path), std::string{}};
  std::optional<UstarName> best;
  for (std::size_t at = 0; at < path.size(); ++at) {
    if (path[at] != '/') continue;
    const std::size_t prefix_length = at;
    const std::size_t name_length = path.size() - at - 1;
    if (name_length == 0 || name_length > 100 || prefix_length > 155) continue;
    best = UstarName{std::string(path.substr(at + 1)), std::string(path.substr(0, at))};
  }
  return best;
}

/// 构造一个 ustar header 块（含校验和；时间戳/uid/gid 置 0 以保证产物可复现）。
[[nodiscard]] auto make_tar_header(std::string_view name, std::string_view prefix, std::uint64_t size,
                                   bool is_dir) -> std::array<std::uint8_t, kTarBlock> {
  std::array<std::uint8_t, kTarBlock> header{};
  const std::span<std::uint8_t> block(header);
  copy_field(block.subspan(0, 100), name);
  write_octal_field(block.subspan(100, 8), is_dir ? 0755U : 0644U);
  write_octal_field(block.subspan(108, 8), 0U);   // uid
  write_octal_field(block.subspan(116, 8), 0U);   // gid
  write_octal_field(block.subspan(124, 12), is_dir ? 0U : size);
  write_octal_field(block.subspan(136, 12), 0U);  // mtime
  for (std::size_t at = 148; at < 156; ++at) header[at] = 0x20U;  // 校验字段先填空格
  header[156] = static_cast<std::uint8_t>(is_dir ? '5' : '0');    // typeflag
  copy_field(block.subspan(257, 6), "ustar");
  header[263] = static_cast<std::uint8_t>('0');  // version "00"
  header[264] = static_cast<std::uint8_t>('0');
  copy_field(block.subspan(345, 155), prefix);

  std::uint64_t sum = 0;
  for (const std::uint8_t byte : header) sum += static_cast<std::uint64_t>(byte);
  const std::uint64_t checksum = sum & 0x3FFFFU;  // 6 位八进制可表示范围
  for (std::size_t index = 0; index < 6; ++index) {
    const std::size_t slot = 5 - index;
    const unsigned shift = static_cast<unsigned>(index * 3U);
    header[148 + slot] = static_cast<std::uint8_t>(static_cast<unsigned>('0') +
                                                   static_cast<unsigned>((checksum >> shift) & 7U));
  }
  header[154] = 0U;
  header[155] = 0x20U;
  return header;
}

}  // namespace

auto archive_read(std::span<const std::uint8_t> input) -> Result<std::vector<ArchiveEntry>> {
  if (input.size() >= 4 && input[0] == 0x50U && input[1] == 0x4BU &&
      (input[2] == 0x03U || input[2] == 0x05U || input[2] == 0x07U) &&
      (input[3] == 0x04U || input[3] == 0x06U || input[3] == 0x08U)) {
    return zip_read(input);
  }
  if (input.size() >= 2 && input[0] == 0x1FU && input[1] == 0x8BU) {
    auto raw = gzip_inflate(input);
    if (!raw) return st::forward_error(raw.error());
    return tar_read(*raw);
  }
  return tar_read(input);
}

auto archive_extract_to(std::span<const std::uint8_t> input, std::string_view dest_dir) -> Status {
  if (dest_dir.empty()) {
    return st::unexpected(ErrorCode::Invalid, "解包目标目录为空");
  }
  auto entries = archive_read(input);
  if (!entries) return st::forward_error(entries.error());
  const std::string root = st::fs::normalize(dest_dir);
  for (const ArchiveEntry& entry : *entries) {
    if (!is_safe_relative_path(entry.path)) {
      return st::unexpected(ErrorCode::Invalid,
                            std::format("归档条目路径不安全（绝对路径或 .. 逃逸）: {}", entry.path));
    }
    const std::string target = st::fs::join(root, entry.path);
    const std::string normalized = st::fs::normalize(target);
    if (normalized.size() < root.size() || normalized.compare(0, root.size(), root) != 0) {
      return st::unexpected(ErrorCode::Invalid,
                            std::format("归档条目逃逸目标目录: {}", entry.path));
    }
    if (entry.is_dir) {
      const Status created = st::fs::create_directories(target);
      if (!created) return st::forward_error(created.error());
      continue;
    }
    const Status written = st::fs::write_bytes(target, entry.data);
    if (!written) return st::forward_error(written.error());
  }
  return st::ok();
}

auto tar_write(const std::vector<ArchiveEntry>& entries) -> Result<Bytes> {
  constexpr std::uint64_t kMaxTarSize = 8ULL * 1024ULL * 1024ULL * 1024ULL - 1ULL;
  Bytes out;
  for (const ArchiveEntry& entry : entries) {
    if (!is_safe_relative_path(entry.path)) {
      return st::unexpected(ErrorCode::Invalid,
                            std::format("tar 条目路径不安全（绝对路径或 .. 逃逸）: {}", entry.path));
    }
    const auto split = split_ustar_name(entry.path);
    if (!split) {
      return st::unexpected(ErrorCode::Unsupported,
                            std::format("tar 条目路径超出 ustar 字段容量: {}", entry.path));
    }
    const std::uint64_t size = entry.is_dir ? 0U : static_cast<std::uint64_t>(entry.data.size());
    if (size > kMaxTarSize) {
      return st::unexpected(ErrorCode::Overflow, std::format("tar 条目过大: {}", entry.path));
    }
    const auto header = make_tar_header(split->name, split->prefix, size, entry.is_dir);
    out.insert(out.end(), header.begin(), header.end());
    if (entry.is_dir) continue;
    out.insert(out.end(), entry.data.begin(), entry.data.end());
    const std::size_t remainder = entry.data.size() % kTarBlock;
    if (remainder != 0U) out.insert(out.end(), kTarBlock - remainder, 0U);
  }
  out.insert(out.end(), 2 * kTarBlock, 0U);  // 归档结束标记：两块全零
  return out;
}

}  // namespace st::codec
