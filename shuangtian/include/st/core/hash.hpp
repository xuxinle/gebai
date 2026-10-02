#pragma once

/// 哈希与校验：SHA-256（内容寻址/依赖校验）、FNV-1a（键哈希）、CRC32/Adler32（PNG/zip/gzip）。

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::hash {

/// 增量 SHA-256（`digest()` 可重复调用，不消耗内部状态）。
class Sha256 {
 public:
  Sha256() noexcept;
  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view text) noexcept;
  [[nodiscard]] auto digest() const noexcept -> std::array<std::uint8_t, 32>;
  [[nodiscard]] auto hex() const -> std::string;

 private:
  void compress(const std::uint8_t* block) noexcept;  // 受控边界：仅接受 64 字节块

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_{0};
  std::size_t buffered_{0};
};

[[nodiscard]] auto sha256_hex(std::span<const std::uint8_t> data) -> std::string;
[[nodiscard]] auto sha256_hex(std::string_view text) -> std::string;
/// 流式读取文件计算 SHA-256（不整文件入内存）。
[[nodiscard]] auto sha256_file(std::string_view path) -> Result<std::string>;
/// 目录内容指纹：路径 + 大小 + mtime 的稳定哈希（构建缓存判据）。
[[nodiscard]] auto tree_fingerprint(std::string_view root) -> Result<std::string>;

[[nodiscard]] auto fnv1a64(std::string_view text) noexcept -> std::uint64_t;
[[nodiscard]] auto fnv1a64_hex(std::string_view text) -> std::string;
[[nodiscard]] auto fnv1a32(std::string_view text) noexcept -> std::uint32_t;

/// 增量 FNV-1a 64（流式字节指纹）：像素缓冲/大文件等不能一次给全的数据用它累积。
/// `value()` 可重复读取（不消耗内部状态）。
class Fnv1a64 {
 public:
  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view text) noexcept;
  [[nodiscard]] auto value() const noexcept -> std::uint64_t { return state_; }

 private:
  std::uint64_t state_{0xcbf29ce484222325ULL};
};

/// 一次性计算字节缓冲的 FNV-1a 64。
[[nodiscard]] auto fnv1a64_bytes(std::span<const std::uint8_t> data) noexcept -> std::uint64_t;

[[nodiscard]] auto crc32(std::span<const std::uint8_t> data) noexcept -> std::uint32_t;
[[nodiscard]] auto crc32(std::string_view text) noexcept -> std::uint32_t;
[[nodiscard]] auto adler32(std::span<const std::uint8_t> data) noexcept -> std::uint32_t;

[[nodiscard]] auto to_hex(std::span<const std::uint8_t> data) -> std::string;
[[nodiscard]] auto from_hex(std::string_view text) -> Result<std::vector<std::uint8_t>>;

}  // namespace st::hash
