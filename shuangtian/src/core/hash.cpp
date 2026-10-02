#include "st/core/hash.hpp"

#include <cstdio>
#include <format>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"

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
  auto file = std::fopen(std::string(path).c_str(), "rb");
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
