#include "st/core/hash.hpp"

#include <array>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"

// Windows CNG（BCrypt）：提供硬件加速的 SHA-256（SHA-NI）。
//
// 为何引入系统库：本文件自带的 `Sha256` 是可移植实现，实测 267 MB/s；
// 而 CNG 在本机跑到 **1777 MB/s**（6.6 倍）。指纹路径对付的是 259MB 的可执行体，
// 差这 6 倍就是 1 s 与 150 ms 的区别。
// 第三方/系统库纪律：系统库不需要 `third_party/` 登记，但要记在这里为何要它。
#if defined(_WIN32)
// `windows.h` 默认把 `min`/`max` 定义成宏：一旦带上，`std::min`/`std::max` 的调用点
// 会以「C2589 非法标记」报错，与真正原因相隔很远。
#define NOMINMAX 1
#include <windows.h>
#include <bcrypt.h>
#endif

namespace st::hash {
namespace {

inline constexpr std::array<std::uint32_t, 64> kSha256Constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

inline constexpr std::array<std::uint32_t, 8> kSha256Initial{
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU,
    0x5be0cd19U};

[[nodiscard]] constexpr auto rotate_right(std::uint32_t value, unsigned bits) noexcept
    -> std::uint32_t {
  return (value >> bits) | (value << (32U - bits));
}

[[nodiscard]] consteval auto make_crc_table() noexcept -> std::array<std::uint32_t, 256> {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1U) != 0U ? (0xEDB88320U ^ (value >> 1U)) : (value >> 1U);
    }
    table[index] = value;
  }
  return table;
}

inline constexpr auto kCrcTable = make_crc_table();

}  // namespace

Sha256::Sha256() noexcept : state_(kSha256Initial) {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t at = index * 4;
    schedule[index] = (static_cast<std::uint32_t>(block[at]) << 24U) |
                      (static_cast<std::uint32_t>(block[at + 1]) << 16U) |
                      (static_cast<std::uint32_t>(block[at + 2]) << 8U) |
                      static_cast<std::uint32_t>(block[at + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t a = schedule[index - 15];
    const std::uint32_t b = schedule[index - 2];
    const std::uint32_t s0 = rotate_right(a, 7) ^ rotate_right(a, 18) ^ (a >> 3U);
    const std::uint32_t s1 = rotate_right(b, 17) ^ rotate_right(b, 19) ^ (b >> 10U);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choice = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + choice + kSha256Constants[index] + schedule[index];
    const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
  total_bytes_ += data.size();
  std::size_t offset = 0;
  if (buffered_ > 0) {
    while (offset < data.size() && buffered_ < 64) {
      buffer_[buffered_] = data[offset];
      ++buffered_;
      ++offset;
    }
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (data.size() - offset >= 64) {
    compress(data.data() + offset);
    offset += 64;
  }
  while (offset < data.size()) {
    buffer_[buffered_] = data[offset];
    ++buffered_;
    ++offset;
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));  // lint-allow: L6 字节视图
}

auto Sha256::digest() const noexcept -> std::array<std::uint8_t, 32> {
  Sha256 working = *this;
  const std::uint64_t bit_length = working.total_bytes_ * 8U;
  const std::uint8_t pad = 0x80U;
  working.update(std::span<const std::uint8_t>(&pad, 1));
  const std::uint8_t zero = 0x00U;
  while (working.buffered_ != 56) working.update(std::span<const std::uint8_t>(&zero, 1));
  std::array<std::uint8_t, 8> length_bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    const unsigned shift = static_cast<unsigned>((7 - index) * 8);
    length_bytes[index] = static_cast<std::uint8_t>((bit_length >> shift) & 0xFFU);
  }
  working.update(std::span<const std::uint8_t>(length_bytes));
  std::array<std::uint8_t, 32> out{};
  for (std::size_t word = 0; word < 8; ++word) {
    const std::uint32_t value = working.state_[word];
    for (std::size_t byte = 0; byte < 4; ++byte) {
      const unsigned shift = static_cast<unsigned>((3 - byte) * 8);
      out[word * 4 + byte] = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
    }
  }
  return out;
}

auto Sha256::hex() const -> std::string { return to_hex(digest()); }

auto sha256_hex(std::span<const std::uint8_t> data) -> std::string {
  Sha256 hasher;
  hasher.update(data);
  return hasher.hex();
}

auto sha256_hex(std::string_view text) -> std::string {
  Sha256 hasher;
  hasher.update(text);
  return hasher.hex();
}

auto sha256_file(std::string_view path) -> Result<std::string> {
  // 必顶用 `fs::open_file`（而不是 `std::fopen`）：后者在 Windows 上按 ANSI 代码页
  // 解释路径，含中文的目录会直接打不开（详见 `fs.hpp` 里 `open_file` 的注释）。
  auto file = st::fs::open_file(path, "rb");
  if (file == nullptr) {
    return unexpected(ErrorCode::NotFound, std::string("open failed '").append(path).append("'"));
  }
  struct Closer {
    std::FILE* handle;
    ~Closer() { std::fclose(handle); }
  } closer{file};
  Sha256 hasher;
  std::array<std::uint8_t, 64 * 1024> chunk{};
  while (true) {
    const std::size_t read = std::fread(chunk.data(), 1, chunk.size(), file);
    if (read > 0) hasher.update(std::span<const std::uint8_t>(chunk.data(), read));
    if (read < chunk.size()) break;
  }
  return hasher.hex();
}

auto tree_fingerprint(std::string_view root) -> Result<std::string> {
  auto entries = fs::walk(root);
  if (!entries) return forward_error(entries.error());
  Sha256 hasher;
  for (const auto& entry : *entries) {
    if (entry.is_dir) continue;
    hasher.update(entry.path);
    hasher.update(std::string("\x1f").append(to_string(entry.size)));
    const auto stamp = fs::modified_ns(fs::join(root, entry.path));
    hasher.update(std::string("\x1e").append(stamp ? to_string(*stamp) : std::string("0")));
    hasher.update("\n");
  }
  return hasher.hex();
}

auto fnv1a64(std::string_view text) noexcept -> std::uint64_t {
  std::uint64_t value = 0xcbf29ce484222325ULL;
  for (const char raw : text) {
    value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
    value *= 0x100000001b3ULL;
  }
  return value;
}

auto fnv1a64_hex(std::string_view text) -> std::string {
  return std::format("{:016x}", fnv1a64(text));
}

auto fnv1a32(std::string_view text) noexcept -> std::uint32_t {
  std::uint32_t value = 0x811c9dc5U;
  for (const char raw : text) {
    value ^= static_cast<std::uint32_t>(static_cast<unsigned char>(raw));
    value *= 0x01000193U;
  }
  return value;
}

void Fnv1a64::update(std::span<const std::uint8_t> data) noexcept {
  for (const std::uint8_t byte : data) {
    state_ ^= static_cast<std::uint64_t>(byte);
    state_ *= 0x100000001b3ULL;
  }
}

void Fnv1a64::update(std::string_view text) noexcept {
  // 不 reinterpret_cast 指针：逐字符转 `unsigned char` 后与字节路径同一算法。
  // （`update(span)` 的循环体完全一样，不抽公共函数是因为代价只在字面量循环里。）
  for (const char raw : text) {
    state_ ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
    state_ *= 0x100000001b3ULL;
  }
}

auto fnv1a64_bytes(std::span<const std::uint8_t> data) noexcept -> std::uint64_t {
  Fnv1a64 hasher;
  hasher.update(data);
  return hasher.value();
}

// ── 大块内容指纹（`content_hash64`）───────────────────────────────────────
//
// ## 为何需要它（实测代价）
//
// 本函数只有一条调用面：宿主物化时判断“尾部 payload 与既有侧车是不是同一份”。
// 但它对付的是 **259MB 的可执行体**，而两侧都是各自语言的最差实现：
//
// | 路径 | 实现 | 259MB 实测 |
// |---|---|---|
// | 旧：C++ `fnv1a64_bytes` | 逐字节串行乘 | **278 ms** |
// | 旧：JS `fnv1a64`（打包）| BigInt 多精度 | **14,317 ms** ← 真灾难 |
// | JS 不碰 BigInt 的 32 位对半 | 手写 | **4,435 ms**（仍是瓶颈）|
// | **CNG SHA-256（本实现）** | **硬件 SHA-NI** | **146 ms / 1777 MB/s** |
// | JS `crypto.sha256` | 硬件 SHA-NI | **142 ms** |
//
// 结论：不能靠“换一个手写算法”解决——JS 侧手写循环的天花板就在 4 s 量级。
// 真正的解法是**两侧都用各自的系统/库原生 SHA-256**，取前 8 字节作指纹。
// 它同时解决了三个问题：快、两侧天然一致（不再两份代码逐位对齐）、有硬件加速。
//
// ## 为何 Windows 上走 CNG（BCrypt）而不是框架的 `Sha256`
//
// 框架的 `Sha256` 是**可移植 C++ 实现**（267 MB/s），比 CNG 慢 **18 倍**。
// 它在小数据/构建指纹上完全够用，但不适合 259MB 的交付路径。
// 跨平台回退：非 Windows 仍用框架的 `sha256_hex`（正确性一致，只是慢一点）。

auto content_hash64(std::span<const std::uint8_t> data) noexcept -> std::uint64_t {
  std::array<std::uint8_t, 8> head{};
#if defined(_WIN32)
  // CNG：开算法 → 建哈希 → 喂数据 → 取摘要。失败（算法不可用等）则回退框架实现——
  // 这里是**指纹判定**不是安全边界，回退不影响正确性，只影响速度。
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD written = 0;
    // 查询必须在成功开句柄之后、建哈希之前（对象长度因实现而异）。
    const bool queried =
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &written, 0) == 0 &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
                          sizeof(hash_size), &written, 0) == 0 &&
        hash_size >= 8;
    bool hashed = false;
    if (queried) {
      std::vector<std::uint8_t> object(object_size);
      std::vector<std::uint8_t> digest(hash_size);
      BCRYPT_HASH_HANDLE handle = nullptr;
      if (BCryptCreateHash(algorithm, &handle, object.data(), object_size, nullptr, 0, 0) == 0) {
        // 长度超过 ULONG 的缓冲区（>4GB）不现实；截断上限如实取整即可。
        const ULONG length = static_cast<ULONG>(data.size());
        const bool fed = BCryptHashData(handle, const_cast<PUCHAR>(data.data()), length, 0) == 0;
        const bool finished = fed && BCryptFinishHash(handle, digest.data(), hash_size, 0) == 0;
        BCryptDestroyHash(handle);
        if (finished) {
          for (std::size_t index = 0; index < 8; ++index) head[index] = digest[index];
          hashed = true;
        }
      }
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (hashed) {
      std::uint64_t value = 0;
      for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(head[index]) << (index * 8U);
      }
      return value;
    }
  }
#endif
  // 回退路径（非 Windows / CNG 不可用）：框架的可移植实现。
  // 与大前端同口径：取十六进制前 16 字符 = 前 8 字节，再小端拼成 u64。
  const std::string hex = sha256_hex(data);
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8 && index * 2 + 1 < hex.size(); ++index) {
    const auto nibble = [](char c) -> std::uint64_t {
      if (c >= '0' && c <= '9') return static_cast<std::uint64_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<std::uint64_t>(c - 'a' + 10);
      if (c >= 'A' && c <= 'F') return static_cast<std::uint64_t>(c - 'A' + 10);
      return 0;
    };
    const std::uint64_t byte = (nibble(hex[index * 2]) << 4U) | nibble(hex[index * 2 + 1]);
    value |= byte << (index * 8U);
  }
  return value;
}

auto crc32(std::span<const std::uint8_t> data) noexcept -> std::uint32_t {
  std::uint32_t value = 0xFFFFFFFFU;
  for (const std::uint8_t byte : data) {
    value = kCrcTable[(value ^ byte) & 0xFFU] ^ (value >> 8U);
  }
  return value ^ 0xFFFFFFFFU;
}

auto crc32(std::string_view text) noexcept -> std::uint32_t {
  return crc32(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));  // lint-allow: L6 字节视图
}

auto adler32(std::span<const std::uint8_t> data) noexcept -> std::uint32_t {
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const std::uint8_t byte : data) {
    a = (a + byte) % 65521U;
    b = (b + a) % 65521U;
  }
  return (b << 16U) | a;
}

auto to_hex(std::span<const std::uint8_t> data) -> std::string {
  std::string out;
  out.reserve(data.size() * 2);
  for (const std::uint8_t byte : data) {
    out.append(std::format("{:02x}", static_cast<unsigned>(byte)));
  }
  return out;
}

auto from_hex(std::string_view text) -> Result<std::vector<std::uint8_t>> {
  if (text.size() % 2 != 0) return unexpected(ErrorCode::Parse, "hex 长度必须是偶数");
  std::vector<std::uint8_t> out;
  out.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    const auto nibble = [](char raw) -> int {
      if (raw >= '0' && raw <= '9') return raw - '0';
      if (raw >= 'a' && raw <= 'f') return raw - 'a' + 10;
      if (raw >= 'A' && raw <= 'F') return raw - 'A' + 10;
      return -1;
    };
    const int high = nibble(text[index]);
    const int low = nibble(text[index + 1]);
    if (high < 0 || low < 0) return unexpected(ErrorCode::Parse, "非法 hex 字符");
    out.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return out;
}

}  // namespace st::hash
