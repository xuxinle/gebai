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

/// **大块内容指纹**（SHA-256 前 8 字节，小端读作 u64）。与 `fnv1a64_bytes` 的分工：
///
/// | | `fnv1a64_bytes` | `content_hash64` |
/// |---|---|---|
/// | 实测（259MB，-O2）| 278 ms | 130 ms |
/// | 适用 | 小数据（源码/文本/配置）| **大文件/大缓冲区** |
/// | 跨语言一致 | 需手写对齐 | **天然**（任何语言都有 SHA-256）|
///
/// 最大的价值不在那 2 倍，而在**跨语言一致**：打包脚本（TypeScript）与宿主（C++）
/// 必须算出同一个值，手写算法在 JS 里只能用 BigInt——那是 **14.8 秒/259MB** 的灾难。
/// 改用 SHA-256 后两侧都走各自内置的优化实现（JS 侧 150ms，走硬件指令集）。
///
/// 注：只取 32 字节摘要的前 8 字节，对该用途（同一份文件的自比对）碰撞概率天文学级足够；
/// 但**不要把它当安全哈希用**（截断后的摘要不适合任何认证场景）。
[[nodiscard]] auto content_hash64(std::span<const std::uint8_t> data) noexcept -> std::uint64_t;

[[nodiscard]] auto crc32(std::span<const std::uint8_t> data) noexcept -> std::uint32_t;
[[nodiscard]] auto crc32(std::string_view text) noexcept -> std::uint32_t;
[[nodiscard]] auto adler32(std::span<const std::uint8_t> data) noexcept -> std::uint32_t;

[[nodiscard]] auto to_hex(std::span<const std::uint8_t> data) -> std::string;
[[nodiscard]] auto from_hex(std::string_view text) -> Result<std::vector<std::uint8_t>>;

}  // namespace st::hash
