// 霜天 codec —— deflate.cpp：RFC 1951（原始流）/ RFC 1950（zlib）/ RFC 1952（gzip）自研实现。
//
// 算法：
//   • inflate：LSB 优先位流读取器 + 规范 Huffman 解码；三类块全支持——
//     BTYPE=00 存储块（LEN/NLEN 互校验）、01 固定 Huffman、10 动态 Huffman
//     （码长码表展开 16/17/18 重复码，Kraft 溢出即报错）。LZ77 反向复制逐字节进行，天然支持重叠匹配。
//   • deflate：LZ77（3 字节乘法哈希 → 哈希链 → 贪心最长匹配，链长由 level 决定）+ 固定 Huffman 熵编码。
//     符号先统一生成为 Token 列表，再按 level 决定的块大小切块输出——因为 deflate 的 FINAL 位位于
//     块首、无法回填，只有先知道切块结果才能正确标注最后一块。不实现动态 Huffman 与存储块。
//   • zlib 包装：CMF/FLG（CM=8、CINFO≤7、FCHECK 整除 31、FDICT 拒绝）+ deflate 流 + Adler-32 大端尾。
//   • gzip 包装：10 字节固定头 + FEXTRA/FNAME/FCOMMENT/FHCRC 可选字段 + deflate 流 + CRC-32/ISIZE 小端尾。
//
// 限制（有意为之，非缺陷）：
//   • 一次性整体解压，无流式/字典复用——内存与输出同阶；
//   • deflate 只产固定 Huffman 块，压缩率低于 zlib 默认档（自洽可解，且系统 zlib 可解）；
//   • gzip 只解第一个成员，尾随成员视为不支持；
//   • 全部输入经 std::span 访问，越界即返回 Error，无指针算术、无异常、无第三方依赖。

#include "st/codec/deflate.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "st/core/hash.hpp"

namespace st::codec {
namespace {

inline constexpr std::size_t kMaxCodeBits = 15;
inline constexpr std::size_t kFixedLiteralCount = 288;
inline constexpr std::size_t kLengthCodeCount = 29;
inline constexpr std::size_t kDistanceCodeCount = 30;
inline constexpr std::size_t kWindowSize = 32768;
inline constexpr std::size_t kMinMatch = 3;
inline constexpr std::size_t kMaxMatch = 258;
inline constexpr unsigned kHashBits = 15;
inline constexpr std::size_t kHashSize = std::size_t{1} << kHashBits;
inline constexpr std::uint32_t kNoPosition = 0xFFFFFFFFU;

// —— 小工具：端序读取（调用方保证长度） ——

[[nodiscard]] auto read_be32(std::span<const std::uint8_t> data) noexcept -> std::uint32_t {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

[[nodiscard]] auto read_le32(std::span<const std::uint8_t> data) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

[[nodiscard]] auto read_le16(std::span<const std::uint8_t> data) noexcept -> std::uint16_t {
  return static_cast<std::uint16_t>(static_cast<std::uint32_t>(data[0]) |
                                    (static_cast<std::uint32_t>(data[1]) << 8U));
}

// —— 位流读取（deflate 为 LSB 优先） ——

/// deflate 位流读取器：不足即返回 false，绝不越界访问。
class BitReader {
 public:
  explicit BitReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  /// 读 `count`（0..24）位到 `out`（低位在前）；数据不足返回 false 且不改动 `out`。
  [[nodiscard]] auto read(std::uint32_t& out, unsigned count) noexcept -> bool {
    while (bit_count_ < count) {
      if (pos_ >= data_.size()) return false;
      bits_ |= static_cast<std::uint32_t>(data_[pos_]) << bit_count_;
      ++pos_;
      bit_count_ += 8;
    }
    const std::uint32_t mask = count == 0 ? 0U : ((std::uint32_t{1} << count) - 1U);
    out = bits_ & mask;
    bits_ >>= count;
    bit_count_ -= count;
    return true;
  }

  /// 对齐到字节边界（存储块的 LEN/NLEN 与字节数据要求字节对齐）。
  void align_to_byte() noexcept {
    const unsigned drop = bit_count_ % 8U;
    bits_ >>= drop;
    bit_count_ -= drop;
  }

  /// 读一个字节（仅用于存储块数据与对齐后的场景）。
  [[nodiscard]] auto read_byte(std::uint8_t& out) noexcept -> bool {
    std::uint32_t value = 0;
    if (!read(value, 8)) return false;
    out = static_cast<std::uint8_t>(value);
    return true;
  }

  /// 已消费到的字节位置（位缓冲恒小于 1 字节，即尾校验的起始偏移）。
  [[nodiscard]] auto byte_position() const noexcept -> std::size_t { return pos_; }

 private:
  std::span<const std::uint8_t> data_{};
  std::size_t pos_{0};
  std::uint32_t bits_{0};
  unsigned bit_count_{0};
};

// —— 规范 Huffman 表 ——

/// `counts[len]` = 码长为 `len` 的码字数；`symbols` 按（码长，符号）升序排列。
struct HuffmanTable {
  std::array<std::uint16_t, kMaxCodeBits + 1> counts{};
  std::vector<std::uint16_t> symbols{};
};

/// 由码长序列构造规范 Huffman 表。
/// 失败：码长 > 15、或码长树过完备（Kraft 和溢出）→ `ErrorCode::Parse`。
/// 注：允许**未尽**（incomplete）表——zlib 对距离表/码长表允许，遇到无解码字时解码阶段报错。
[[nodiscard]] auto build_table(std::span<const std::uint8_t> lengths, HuffmanTable& table)
    -> Status {
  table.counts.fill(0);
  for (const std::uint8_t length : lengths) {
    if (static_cast<std::size_t>(length) > kMaxCodeBits) {
      return st::unexpected(ErrorCode::Parse, std::format("Huffman 码长非法: {}", length));
    }
    ++table.counts[static_cast<std::size_t>(length)];
  }
  std::int32_t left = 1;
  for (std::size_t bits = 1; bits <= kMaxCodeBits; ++bits) {
    left <<= 1;
    left -= static_cast<std::int32_t>(table.counts[bits]);
    if (left < 0) {
      return st::unexpected(ErrorCode::Parse, "Huffman 码长树过完备（Kraft 和溢出）");
    }
  }
  std::array<std::size_t, kMaxCodeBits + 1> offsets{};
  std::size_t total = 0;
  for (std::size_t bits = 1; bits <= kMaxCodeBits; ++bits) {
    offsets[bits] = total;
    total += static_cast<std::size_t>(table.counts[bits]);
  }
  table.symbols.assign(lengths.size(), 0);
  for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
    const std::size_t length = static_cast<std::size_t>(lengths[symbol]);
    if (length != 0) {
      table.symbols[offsets[length]] = static_cast<std::uint16_t>(symbol);
      ++offsets[length];
    }
  }
  table.symbols.resize(total);
  return st::ok();
}

/// 按规范 Huffman 逐位解码一个符号；数据耗尽或无解返回 false。
[[nodiscard]] auto decode_symbol(BitReader& reader, const HuffmanTable& table,
                                 std::uint16_t& symbol) noexcept -> bool {
  std::int32_t code = 0;
  std::int32_t first = 0;
  std::int32_t index = 0;
  for (std::size_t bits = 1; bits <= kMaxCodeBits; ++bits) {
    std::uint32_t bit = 0;
    if (!reader.read(bit, 1)) return false;
    code |= static_cast<std::int32_t>(bit);
    const std::int32_t count = static_cast<std::int32_t>(table.counts[bits]);
    const std::int32_t offset = code - first;
    if (offset < count) {
      const std::size_t at =
          static_cast<std::size_t>(index) + static_cast<std::size_t>(offset);
      if (at >= table.symbols.size()) return false;
      symbol = table.symbols[at];
      return true;
    }
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return false;
}

// —— 固定 Huffman 码长（RFC 1951 §3.2.6） ——

[[nodiscard]] consteval auto make_fixed_literal_lengths() noexcept
    -> std::array<std::uint8_t, kFixedLiteralCount> {
  std::array<std::uint8_t, kFixedLiteralCount> lengths{};
  for (std::size_t symbol = 0; symbol < 144; ++symbol) lengths[symbol] = 8;
  for (std::size_t symbol = 144; symbol < 256; ++symbol) lengths[symbol] = 9;
  for (std::size_t symbol = 256; symbol < 280; ++symbol) lengths[symbol] = 7;
  for (std::size_t symbol = 280; symbol < kFixedLiteralCount; ++symbol) lengths[symbol] = 8;
  return lengths;
}

[[nodiscard]] consteval auto make_fixed_distance_lengths() noexcept
    -> std::array<std::uint8_t, kDistanceCodeCount + 2> {
  std::array<std::uint8_t, kDistanceCodeCount + 2> lengths{};
  for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) lengths[symbol] = 5;
  return lengths;
}

inline constexpr auto kFixedLiteralLengths = make_fixed_literal_lengths();
inline constexpr auto kFixedDistanceLengths = make_fixed_distance_lengths();

// —— 长度/距离码基值与附加位（RFC 1951 §3.2.5） ——

inline constexpr std::array<std::uint16_t, kLengthCodeCount> kLengthBase{
    3,   4,   5,   6,   7,   8,   9,   10,  11,  13,  15,  17,  19,  23,  27,
    31,  35,  43,  51,  59,  67,  83,  99,  115, 131, 163, 195, 227, 258};
inline constexpr std::array<std::uint8_t, kLengthCodeCount> kLengthExtra{
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
inline constexpr std::array<std::uint16_t, kDistanceCodeCount> kDistanceBase{
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,  513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
inline constexpr std::array<std::uint8_t, kDistanceCodeCount> kDistanceExtra{
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/// 动态块码长表的传输顺序（RFC 1951 §3.2.7）。
inline constexpr std::array<std::uint8_t, 19> kCodeLengthOrder{
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/// 解码一段 Huffman 编码的符号序列（字面量/长度/结束码），直到遇到结束码 256。
[[nodiscard]] auto inflate_codes(BitReader& reader, const HuffmanTable& literals,
                                 const HuffmanTable& distances, std::size_t max_output,
                                 Bytes& out) -> Status {
  while (true) {
    std::uint16_t symbol = 0;
    if (!decode_symbol(reader, literals, symbol)) {
      return st::unexpected(ErrorCode::Parse, "deflate 字面量/长度码无解或数据截断");
    }
    if (symbol < 256) {
      if (out.size() >= max_output) {
        return st::unexpected(ErrorCode::Overflow, "inflate 输出超过 max_output");
      }
      out.push_back(static_cast<std::uint8_t>(symbol));
      continue;
    }
    if (symbol == 256) return st::ok();

    const std::size_t length_index = static_cast<std::size_t>(symbol) - 257;
    if (length_index >= kLengthCodeCount) {
      return st::unexpected(ErrorCode::Parse, std::format("deflate 长度码越界: {}", symbol));
    }
    std::uint32_t extra = 0;
    if (!reader.read(extra, static_cast<unsigned>(kLengthExtra[length_index]))) {
      return st::unexpected(ErrorCode::Parse, "deflate 长度附加位截断");
    }
    const std::size_t length =
        static_cast<std::size_t>(kLengthBase[length_index]) + static_cast<std::size_t>(extra);

    std::uint16_t distance_symbol = 0;
    if (!decode_symbol(reader, distances, distance_symbol)) {
      return st::unexpected(ErrorCode::Parse, "deflate 距离码无解或数据截断");
    }
    if (static_cast<std::size_t>(distance_symbol) >= kDistanceCodeCount) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("deflate 距离码越界: {}", distance_symbol));
    }
    extra = 0;
    if (!reader.read(extra, static_cast<unsigned>(kDistanceExtra[distance_symbol]))) {
      return st::unexpected(ErrorCode::Parse, "deflate 距离附加位截断");
    }
    const std::size_t distance =
        static_cast<std::size_t>(kDistanceBase[distance_symbol]) + static_cast<std::size_t>(extra);
    if (distance > out.size()) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("deflate LZ77 回溯越界: 距离 {} > 已输出 {}",
                                        distance, out.size()));
    }
    if (out.size() + length > max_output) {
      return st::unexpected(ErrorCode::Overflow, "inflate 输出超过 max_output");
    }
    std::size_t from = out.size() - distance;
    for (std::size_t copied = 0; copied < length; ++copied) {
      const std::uint8_t byte = out[from];  // 重叠匹配：out 增长后 from 仍在已输出区间内
      out.push_back(byte);
      ++from;
    }
  }
}

/// 解析动态 Huffman 块头并构造字面量/距离表。
[[nodiscard]] auto read_dynamic_tables(BitReader& reader, HuffmanTable& literals,
                                       HuffmanTable& distances) -> Status {
  std::uint32_t literal_bits = 0;
  std::uint32_t distance_bits = 0;
  std::uint32_t code_length_bits = 0;
  if (!reader.read(literal_bits, 5) || !reader.read(distance_bits, 5) ||
      !reader.read(code_length_bits, 4)) {
    return st::unexpected(ErrorCode::Parse, "deflate 动态块头截断");
  }
  const std::size_t literal_count = static_cast<std::size_t>(literal_bits) + 257;
  const std::size_t distance_count = static_cast<std::size_t>(distance_bits) + 1;
  const std::size_t code_length_count = static_cast<std::size_t>(code_length_bits) + 4;
  if (literal_count > 286 || distance_count > kDistanceCodeCount) {
    return st::unexpected(ErrorCode::Parse, "deflate 动态块字面量/距离符号数越界");
  }

  std::array<std::uint8_t, kCodeLengthOrder.size()> code_lengths{};
  for (std::size_t slot = 0; slot < code_length_count; ++slot) {
    std::uint32_t value = 0;
    if (!reader.read(value, 3)) {
      return st::unexpected(ErrorCode::Parse, "deflate 码长码表截断");
    }
    code_lengths[kCodeLengthOrder[slot]] = static_cast<std::uint8_t>(value);
  }
  HuffmanTable code_length_table;
  Status status = build_table(std::span<const std::uint8_t>(code_lengths), code_length_table);
  if (!status) return st::forward_error(status.error());

  std::array<std::uint8_t, 286 + kDistanceCodeCount> lengths{};
  const std::span<std::uint8_t> all_lengths(lengths);
  const std::size_t total = literal_count + distance_count;
  std::size_t index = 0;
  while (index < total) {
    std::uint16_t symbol = 0;
    if (!decode_symbol(reader, code_length_table, symbol)) {
      return st::unexpected(ErrorCode::Parse, "deflate 码长解码失败");
    }
    if (symbol < 16) {
      all_lengths[index] = static_cast<std::uint8_t>(symbol);
      ++index;
      continue;
    }
    std::size_t repeat = 0;
    std::uint8_t value = 0;
    std::uint32_t extra = 0;
    if (symbol == 16) {
      if (index == 0) {
        return st::unexpected(ErrorCode::Parse, "deflate 码长重复码 16 缺少前值");
      }
      if (!reader.read(extra, 2)) {
        return st::unexpected(ErrorCode::Parse, "deflate 码长重复位截断");
      }
      repeat = 3 + static_cast<std::size_t>(extra);
      value = all_lengths[index - 1];
    } else if (symbol == 17) {
      if (!reader.read(extra, 3)) {
        return st::unexpected(ErrorCode::Parse, "deflate 码长重复位截断");
      }
      repeat = 3 + static_cast<std::size_t>(extra);
    } else if (symbol == 18) {
      if (!reader.read(extra, 7)) {
        return st::unexpected(ErrorCode::Parse, "deflate 码长重复位截断");
      }
      repeat = 11 + static_cast<std::size_t>(extra);
    } else {
      return st::unexpected(ErrorCode::Parse, "deflate 码长码非法");
    }
    if (index + repeat > total) {
      return st::unexpected(ErrorCode::Parse, "deflate 码长重复越界");
    }
    for (std::size_t filled = 0; filled < repeat; ++filled) {
      all_lengths[index] = value;
      ++index;
    }
  }
  if (all_lengths[256] == 0) {
    return st::unexpected(ErrorCode::Parse, "deflate 动态块缺少结束码 256");
  }
  status = build_table(all_lengths.first(literal_count), literals);
  if (!status) return st::forward_error(status.error());
  return build_table(all_lengths.subspan(literal_count, distance_count), distances);
}

/// 解一条 deflate 流到 `out`（多块循环），`consumed` 回传已消费的输入字节数。
[[nodiscard]] auto inflate_stream(std::span<const std::uint8_t> input, std::size_t max_output,
                                  Bytes& out, std::size_t& consumed) -> Status {
  BitReader reader(input);
  bool last_block = false;
  while (!last_block) {
    std::uint32_t final_bit = 0;
    std::uint32_t block_type = 0;
    if (!reader.read(final_bit, 1)) {
      return st::unexpected(ErrorCode::Parse, "deflate 流提前结束（块头缺失）");
    }
    if (!reader.read(block_type, 2)) {
      return st::unexpected(ErrorCode::Parse, "deflate 流提前结束（块类型缺失）");
    }
    last_block = final_bit != 0;

    if (block_type == 0) {
      reader.align_to_byte();
      std::uint32_t length = 0;
      std::uint32_t complement = 0;
      if (!reader.read(length, 16) || !reader.read(complement, 16)) {
        return st::unexpected(ErrorCode::Parse, "deflate 存储块长度字段截断");
      }
      if ((length ^ 0xFFFFU) != complement) {
        return st::unexpected(ErrorCode::Parse, "deflate 存储块 LEN/NLEN 校验失败");
      }
      const std::size_t bytes = static_cast<std::size_t>(length);
      if (out.size() + bytes > max_output) {
        return st::unexpected(ErrorCode::Overflow, "inflate 输出超过 max_output");
      }
      for (std::size_t copied = 0; copied < bytes; ++copied) {
        std::uint8_t byte = 0;
        if (!reader.read_byte(byte)) {
          return st::unexpected(ErrorCode::Parse, "deflate 存储块数据截断");
        }
        out.push_back(byte);
      }
      continue;
    }
    if (block_type == 3) {
      return st::unexpected(ErrorCode::Parse, "deflate 块类型 3 非法");
    }

    HuffmanTable literals;
    HuffmanTable distances;
    Status status = st::ok();
    if (block_type == 1) {
      status = build_table(std::span<const std::uint8_t>(kFixedLiteralLengths), literals);
      if (status) status = build_table(kFixedDistanceLengths, distances);
    } else {
      status = read_dynamic_tables(reader, literals, distances);
    }
    if (!status) return st::forward_error(status.error());
    status = inflate_codes(reader, literals, distances, max_output, out);
    if (!status) return st::forward_error(status.error());
  }
  consumed = reader.byte_position();
  return st::ok();
}

// —— 位流写出（LSB 优先） ——

/// deflate 位流写入器：低位先入，`flush()` 补齐到字节边界。
class BitWriter {
 public:
  explicit BitWriter(Bytes& out) noexcept : out_(out) {}

  /// 写 `count`（0..24）位，取 `value` 的低 `count` 位。
  void write(std::uint32_t value, unsigned count) noexcept {
    if (count == 0) return;
    const std::uint64_t mask = (std::uint64_t{1} << count) - 1U;
    bits_ |= (static_cast<std::uint64_t>(value) & mask) << bit_count_;
    bit_count_ += count;
    while (bit_count_ >= 8) {
      out_.push_back(static_cast<std::uint8_t>(bits_ & 0xFFU));
      bits_ >>= 8;
      bit_count_ -= 8;
    }
  }

  /// 写 Huffman 码字：位流中码字 MSB 在前，故先做位反序再按 LSB 入流。
  void write_code(std::uint32_t code, unsigned count) noexcept {
    write(reverse_bits(code, count), count);
  }

  /// 补齐到字节边界（整条流的收尾）。
  void flush() noexcept {
    if (bit_count_ > 0) {
      out_.push_back(static_cast<std::uint8_t>(bits_ & 0xFFU));
      bits_ = 0;
      bit_count_ = 0;
    }
  }

 private:
  [[nodiscard]] static constexpr auto reverse_bits(std::uint32_t value,
                                                   unsigned count) noexcept -> std::uint32_t {
    std::uint32_t result = 0;
    for (unsigned bit = 0; bit < count; ++bit) {
      result = (result << 1U) | (value & 1U);
      value >>= 1U;
    }
    return result;
  }

  Bytes& out_;
  std::uint64_t bits_{0};
  unsigned bit_count_{0};
};

// —— 固定 Huffman 编码表（编码侧，与解码侧 kFixedLiteralLengths 同源） ——

[[nodiscard]] constexpr auto fixed_literal_code(std::size_t symbol) noexcept -> std::uint32_t {
  if (symbol < 144) return 0x30U + static_cast<std::uint32_t>(symbol);
  if (symbol < 256) return 0x190U + static_cast<std::uint32_t>(symbol - 144);
  if (symbol < 280) return 0x00U + static_cast<std::uint32_t>(symbol - 256);
  return 0xC0U + static_cast<std::uint32_t>(symbol - 280);
}

[[nodiscard]] constexpr auto fixed_literal_bits(std::size_t symbol) noexcept -> unsigned {
  if (symbol < 144) return 8;
  if (symbol < 256) return 9;
  if (symbol < 280) return 7;
  return 8;
}

// —— LZ77 符号（Token）与压缩参数 ——

/// 一个字面量或一次匹配：`distance == 0` 时 `value` 为字面量字节；否则 `value` 为匹配长度、`distance` 为距离。
struct Token {
  std::uint16_t value{0};
  std::uint16_t distance{0};
};

/// Token 覆盖的输入字节数（用于按块大小切块）。
[[nodiscard]] constexpr auto token_cost(const Token& token) noexcept -> std::size_t {
  return token.distance == 0 ? 1 : static_cast<std::size_t>(token.value);
}

/// `level` 对应的压缩参数：哈希链长上限与块大小（压缩率/速度折中）。
struct DeflateOptions {
  std::size_t max_chain{32};
  std::size_t block_size{48u * 1024u};
};

[[nodiscard]] auto options_for_level(int level) noexcept -> DeflateOptions {
  const int clamped = std::clamp(level, 0, 9);
  constexpr std::array<std::size_t, 10> chains{4, 4, 8, 16, 24, 32, 48, 64, 96, 128};
  constexpr std::array<std::size_t, 10> blocks{16u * 1024u,   16u * 1024u,  32u * 1024u,
                                               32u * 1024u,   48u * 1024u,  64u * 1024u,
                                               64u * 1024u,   128u * 1024u, 128u * 1024u,
                                               256u * 1024u};
  const std::size_t slot = static_cast<std::size_t>(clamped);
  return DeflateOptions{chains[slot], blocks[slot]};
}

/// 3 字节滚动哈希（乘法散列，取高 `kHashBits` 位）。前置条件：`pos + 2 < data.size()`。
[[nodiscard]] auto hash3(std::span<const std::uint8_t> data, std::size_t pos) noexcept
    -> std::size_t {
  const std::uint32_t value = (static_cast<std::uint32_t>(data[pos]) << 16U) |
                              (static_cast<std::uint32_t>(data[pos + 1]) << 8U) |
                              static_cast<std::uint32_t>(data[pos + 2]);
  return static_cast<std::size_t>((value * 2654435761U) >> (32U - kHashBits));
}

/// LZ77：贪心最长匹配（3 字节哈希 + 哈希链，链长受 `max_chain` 限制，窗口 32K、最长匹配 258）。
[[nodiscard]] auto lz77_tokens(std::span<const std::uint8_t> input, std::size_t max_chain)
    -> std::vector<Token> {
  const std::size_t size = input.size();
  std::vector<Token> tokens;
  tokens.reserve(size / 2 + 1);
  if (size < kMinMatch) {
    for (const std::uint8_t byte : input) tokens.push_back(Token{byte, 0});
    return tokens;
  }
  std::vector<std::uint32_t> head(kHashSize, kNoPosition);
  std::vector<std::uint32_t> chain(size, kNoPosition);
  std::size_t pos = 0;
  while (pos < size) {
    if (pos + kMinMatch > size) {
      tokens.push_back(Token{input[pos], 0});
      ++pos;
      continue;
    }
    const std::size_t slot = hash3(input, pos);
    const std::size_t max_length = std::min(kMaxMatch, size - pos);
    std::size_t best_length = 0;
    std::size_t best_distance = 0;
    std::uint32_t candidate = head[slot];
    std::size_t walked = 0;
    while (candidate != kNoPosition && walked < max_chain) {
      const std::size_t at = static_cast<std::size_t>(candidate);
      if (pos - at > kWindowSize) break;  // 链按位置递减：一旦超出窗口，后续只会更远
      ++walked;
      std::size_t length = 0;
      while (length < max_length && input[at + length] == input[pos + length]) ++length;
      if (length > best_length) {
        best_length = length;
        best_distance = pos - at;
        if (length >= max_length) break;
      }
      candidate = chain[at];
    }
    chain[pos] = head[slot];
    head[slot] = static_cast<std::uint32_t>(pos);
    if (best_length >= kMinMatch) {
      tokens.push_back(Token{static_cast<std::uint16_t>(best_length),
                             static_cast<std::uint16_t>(best_distance)});
      for (std::size_t skip = 1; skip < best_length; ++skip) {
        const std::size_t at = pos + skip;
        if (at + kMinMatch > size) break;
        const std::size_t skip_slot = hash3(input, at);
        chain[at] = head[skip_slot];
        head[skip_slot] = static_cast<std::uint32_t>(at);
      }
      pos += best_length;
    } else {
      tokens.push_back(Token{input[pos], 0});
      ++pos;
    }
  }
  return tokens;
}

// —— Token → 固定 Huffman 位流 ——

void write_length(BitWriter& writer, std::size_t length) {
  std::size_t slot = 0;
  while (slot + 1 < kLengthCodeCount && length >= kLengthBase[slot + 1]) ++slot;
  const std::size_t symbol = 257 + slot;
  writer.write_code(fixed_literal_code(symbol), fixed_literal_bits(symbol));
  writer.write(static_cast<std::uint32_t>(length - kLengthBase[slot]),
               static_cast<unsigned>(kLengthExtra[slot]));
}

void write_distance(BitWriter& writer, std::size_t distance) {
  std::size_t slot = 0;
  while (slot + 1 < kDistanceCodeCount && distance >= kDistanceBase[slot + 1]) ++slot;
  writer.write_code(static_cast<std::uint32_t>(slot), 5);  // 固定距离码：5 位，值即符号序号
  writer.write(static_cast<std::uint32_t>(distance - kDistanceBase[slot]),
               static_cast<unsigned>(kDistanceExtra[slot]));
}

void write_token(BitWriter& writer, const Token& token) {
  if (token.distance == 0) {
    const std::size_t symbol = static_cast<std::size_t>(token.value);
    writer.write_code(fixed_literal_code(symbol), fixed_literal_bits(symbol));
    return;
  }
  write_length(writer, static_cast<std::size_t>(token.value));
  write_distance(writer, static_cast<std::size_t>(token.distance));
}

/// 完整 deflate 压缩：先生成符号序列，再按块大小切块编码（FINAL 位在块首，必须切块已知后写入）。
[[nodiscard]] auto deflate_stream(std::span<const std::uint8_t> input, int level) -> Bytes {
  const DeflateOptions options = options_for_level(level);
  const std::vector<Token> tokens = lz77_tokens(input, options.max_chain);
  Bytes out;
  out.reserve(input.size() / 2 + 64);
  BitWriter writer(out);
  std::size_t index = 0;
  while (index < tokens.size()) {
    const std::size_t begin = index;
    std::size_t consumed = 0;
    while (index < tokens.size() && consumed < options.block_size) {
      consumed += token_cost(tokens[index]);
      ++index;
    }
    const bool last = index >= tokens.size();
    writer.write(last ? 1U : 0U, 1);
    writer.write(1U, 2);  // BTYPE=01：固定 Huffman
    for (std::size_t at = begin; at < index; ++at) write_token(writer, tokens[at]);
    writer.write_code(fixed_literal_code(256), fixed_literal_bits(256));  // 块结束码
  }
  if (tokens.empty()) {
    writer.write(1U, 1);  // 空输入：一个空的最终固定块
    writer.write(1U, 2);
    writer.write_code(fixed_literal_code(256), fixed_literal_bits(256));
  }
  writer.flush();
  return out;
}

}  // namespace

auto inflate_raw(std::span<const std::uint8_t> input, std::size_t max_output) -> Result<Bytes> {
  Bytes out;
  std::size_t consumed = 0;
  const Status status = inflate_stream(input, max_output, out, consumed);
  if (!status) return st::forward_error(status.error());
  return out;
}

auto deflate_raw(std::span<const std::uint8_t> input, int level) -> Result<Bytes> {
  return deflate_stream(input, level);
}

auto zlib_deflate(std::span<const std::uint8_t> input, int level) -> Result<Bytes> {
  const int clamped = std::clamp(level, 0, 9);
  const std::uint8_t cmf = 0x78;  // CM=8（deflate）、CINFO=7（32K 窗口）
  std::uint32_t flg = 0;
  if (clamped >= 9) {
    flg = 3U << 6U;
  } else if (clamped >= 6) {
    flg = 2U << 6U;
  } else if (clamped >= 2) {
    flg = 1U << 6U;
  }
  const std::uint32_t header = static_cast<std::uint32_t>(cmf) * 256U + flg;
  flg |= (31U - (header % 31U)) % 31U;  // FCHECK：使 (CMF*256+FLG) % 31 == 0

  Bytes out;
  out.reserve(input.size() / 2 + 64);
  out.push_back(cmf);
  out.push_back(static_cast<std::uint8_t>(flg));
  const Bytes body = deflate_stream(input, clamped);
  out.insert(out.end(), body.begin(), body.end());
  const std::uint32_t adler = st::hash::adler32(input);
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::uint8_t>((adler >> static_cast<unsigned>(shift)) & 0xFFU));
  }
  return out;
}

auto zlib_inflate(std::span<const std::uint8_t> input, std::size_t max_output) -> Result<Bytes> {
  if (input.size() < 6) {
    return st::unexpected(ErrorCode::Parse, "zlib 流过短（不足 6 字节）");
  }
  const std::uint8_t cmf = input[0];
  const std::uint8_t flg = input[1];
  if ((cmf & 0x0FU) != 8U) {
    return st::unexpected(ErrorCode::Unsupported,
                          std::format("zlib 压缩方法非 deflate: {}", cmf & 0x0FU));
  }
  if ((cmf >> 4U) > 7U) {
    return st::unexpected(ErrorCode::Parse, "zlib 窗口大小字段（CINFO）非法");
  }
  if ((static_cast<std::uint32_t>(cmf) * 256U + static_cast<std::uint32_t>(flg)) % 31U != 0U) {
    return st::unexpected(ErrorCode::Parse, "zlib 头校验（FCHECK）失败");
  }
  if ((flg & 0x20U) != 0U) {
    return st::unexpected(ErrorCode::Unsupported, "zlib 预设字典（FDICT）不支持");
  }
  Bytes out;
  std::size_t consumed = 0;
  const Status status = inflate_stream(input.subspan(2), max_output, out, consumed);
  if (!status) return st::forward_error(status.error());
  const std::size_t trailer = 2 + consumed;
  if (input.size() < trailer + 4) {
    return st::unexpected(ErrorCode::Parse, "zlib 尾校验（Adler-32）缺失");
  }
  const std::uint32_t expected = read_be32(input.subspan(trailer, 4));
  const std::uint32_t actual = st::hash::adler32(out);
  if (expected != actual) {
    return st::unexpected(ErrorCode::Parse, std::format("zlib Adler-32 校验失败: 期望 {:08x} 实际 {:08x}",
                                                        expected, actual));
  }
  return out;
}

auto gzip_deflate(std::span<const std::uint8_t> input, int level) -> Result<Bytes> {
  const int clamped = std::clamp(level, 0, 9);
  const std::uint8_t extra_flags =
      static_cast<std::uint8_t>(clamped >= 9 ? 2 : (clamped <= 2 ? 4 : 0));  // XFL：仅提示用途
  const std::array<std::uint8_t, 10> header{0x1FU, 0x8BU, 0x08U, 0x00U, 0x00U,
                                            0x00U, 0x00U, 0x00U, extra_flags, 0x03U};
  Bytes out;
  out.reserve(input.size() / 2 + 64);
  out.insert(out.end(), header.begin(), header.end());
  const Bytes body = deflate_stream(input, clamped);
  out.insert(out.end(), body.begin(), body.end());
  const std::uint32_t crc = st::hash::crc32(input);
  const std::uint32_t isize = static_cast<std::uint32_t>(input.size() & 0xFFFFFFFFU);
  const std::array<std::uint32_t, 2> tail{crc, isize};
  for (const std::uint32_t word : tail) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
      out.push_back(static_cast<std::uint8_t>((word >> shift) & 0xFFU));
    }
  }
  return out;
}

auto gzip_inflate(std::span<const std::uint8_t> input, std::size_t max_output) -> Result<Bytes> {
  if (input.size() < 18) {
    return st::unexpected(ErrorCode::Parse, "gzip 流过短（不足 18 字节）");
  }
  if (input[0] != 0x1FU || input[1] != 0x8BU) {
    return st::unexpected(ErrorCode::Parse, "gzip 魔数不符");
  }
  if (input[2] != 8U) {
    return st::unexpected(ErrorCode::Unsupported, "gzip 压缩方法非 deflate");
  }
  const std::uint8_t flags = input[3];
  if ((flags & 0xE0U) != 0U) {
    return st::unexpected(ErrorCode::Parse, "gzip 保留标志位非零");
  }
  std::size_t pos = 10;
  if ((flags & 0x04U) != 0U) {  // FEXTRA
    if (pos + 2 > input.size()) {
      return st::unexpected(ErrorCode::Parse, "gzip FEXTRA 字段截断");
    }
    const std::size_t extra_length = read_le16(input.subspan(pos, 2));
    pos += 2;
    if (pos + extra_length > input.size()) {
      return st::unexpected(ErrorCode::Parse, "gzip FEXTRA 数据截断");
    }
    pos += extra_length;
  }
  if ((flags & 0x08U) != 0U) {  // FNAME
    while (pos < input.size() && input[pos] != 0U) ++pos;
    if (pos >= input.size()) {
      return st::unexpected(ErrorCode::Parse, "gzip FNAME 缺少终止符");
    }
    ++pos;
  }
  if ((flags & 0x10U) != 0U) {  // FCOMMENT
    while (pos < input.size() && input[pos] != 0U) ++pos;
    if (pos >= input.size()) {
      return st::unexpected(ErrorCode::Parse, "gzip FCOMMENT 缺少终止符");
    }
    ++pos;
  }
  if ((flags & 0x02U) != 0U) {  // FHCRC
    if (pos + 2 > input.size()) {
      return st::unexpected(ErrorCode::Parse, "gzip FHCRC 字段截断");
    }
    const std::uint16_t expected = read_le16(input.subspan(pos, 2));
    const std::uint16_t actual =
        static_cast<std::uint16_t>(st::hash::crc32(input.first(pos)) & 0xFFFFU);
    if (expected != actual) {
      return st::unexpected(ErrorCode::Parse, "gzip 头 CRC（FHCRC）校验失败");
    }
    pos += 2;
  }

  Bytes out;
  std::size_t consumed = 0;
  const Status status = inflate_stream(input.subspan(pos), max_output, out, consumed);
  if (!status) return st::forward_error(status.error());
  const std::size_t trailer = pos + consumed;
  if (input.size() < trailer + 8) {
    return st::unexpected(ErrorCode::Parse, "gzip 尾校验（CRC-32/ISIZE）缺失");
  }
  const std::uint32_t expected_crc = read_le32(input.subspan(trailer, 4));
  const std::uint32_t expected_size = read_le32(input.subspan(trailer + 4, 4));
  if (expected_crc != st::hash::crc32(out)) {
    return st::unexpected(ErrorCode::Parse, "gzip CRC-32 校验失败");
  }
  if (expected_size != static_cast<std::uint32_t>(out.size() & 0xFFFFFFFFU)) {
    return st::unexpected(ErrorCode::Parse, "gzip ISIZE（解压长度）校验失败");
  }
  return out;
}

}  // namespace st::codec
