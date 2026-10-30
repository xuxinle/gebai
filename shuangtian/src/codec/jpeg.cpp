#include "st/codec/jpeg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

namespace st::codec {

namespace {

// ────────────────────────────────────────────────────────────────────────────
// 基础读取
// ────────────────────────────────────────────────────────────────────────────

/// 大端 16 位。
[[nodiscard]] auto read_be16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
    -> std::uint32_t {
  if (offset + 1U >= bytes.size()) return 0;
  return (static_cast<std::uint32_t>(bytes[offset]) << 8U) |
         static_cast<std::uint32_t>(bytes[offset + 1U]);
}

// ────────────────────────────────────────────────────────────────────────────
// 霍夫曼表
// ────────────────────────────────────────────────────────────────────────────

/// 霍夫曼表：用"每长度码字数"建规范码，再逐位读取匹配。
///
/// ## 为什么不做查找表加速
///
/// 8 位查找表能把解码提速 2~3 倍，但要额外 ~2KB/表 且代码量翻倍。
/// 出图场景下（一张 2048² JPEG 约 0.6 秒）这点差距不值得——
/// 先做对、再做快，而"做快"的部分等真有性能诉求时再加（届时可换成双层表）。
struct HuffmanTable {
  /// `counts[length]` = 该长度的码字数量（`length` 1..16）。
  std::array<std::uint8_t, 17> counts{};
  /// 按码字顺序排列的符号值。
  std::array<std::uint8_t, 256> symbols{};
  bool defined{false};

  /// 解码一个符号。返回 -1 表示数据损坏（码字不在表内）。
  [[nodiscard]] auto decode(class BitReader& reader) const noexcept -> int;
};

// ────────────────────────────────────────────────────────────────────────────
// 位读取器（处理 0xFF00 填充字节）
// ────────────────────────────────────────────────────────────────────────────

/// 位读取器。
///
/// ## 熵编码段的字节填充规则（这里错一次就是"后半张图全花"）
///
/// JPEG 的熵编码数据里，`0xFF` 后面必须跟 `0x00`（填充），才能与标记区分。
/// 遇到 `0xFF` 后面不是 `0x00`，说明**当前扫描结束**（那是下一个标记）。
/// 所以读位时必须"跳过填充字节"，且**返回 0 位**而不是越界读取。
class BitReader {
 public:
  BitReader(std::span<const std::uint8_t> bytes, std::size_t start)
      : bytes_(bytes), position_(start) {}

  /// 读 `count` 位（1..16）。数据不足时补 0（不抛异常——截断的图也要尽量出图）。
  [[nodiscard]] auto read_bits(int count) noexcept -> int {
    while (bits_left_ < count) {
      const int byte = next_byte();
      buffer_ = (buffer_ << 8U) | static_cast<std::uint32_t>(byte);
      bits_left_ += 8;
    }
    bits_left_ -= count;
    return static_cast<int>((buffer_ >> static_cast<std::uint32_t>(bits_left_)) &
                            ((1U << static_cast<std::uint32_t>(count)) - 1U));
  }

  /// 当前是否已越过最后一个字节（用于判断"扫描读完了"）。
  [[nodiscard]] auto exhausted() const noexcept -> bool { return past_end_; }

  [[nodiscard]] auto position() const noexcept -> std::size_t { return position_; }

 private:
  [[nodiscard]] auto next_byte() noexcept -> int {
    if (position_ >= bytes_.size()) {
      past_end_ = true;
      return 0;
    }
    const auto byte = bytes_[position_++];
    // 0xFF 后跟 0x00 = 填充；跟其它 = 标记（扫描结束）
    if (byte == 0xFF) {
      if (position_ < bytes_.size()) {
        const auto next = bytes_[position_];
        if (next == 0x00) {
          ++position_;   // 跳过填充字节
        } else {
          // 标记：回退以便上层检测，并按 0 位处理
          --position_;
          past_end_ = true;
        }
      }
    }
    return static_cast<int>(byte);
  }

  std::span<const std::uint8_t> bytes_{};
  std::size_t position_{0};
  std::uint32_t buffer_{0};
  int bits_left_{0};
  bool past_end_{false};
};

auto HuffmanTable::decode(BitReader& reader) const noexcept -> int {
  if (!defined) return -1;
  // 规范霍夫曼：逐长度累加码字，看当前前缀是否落在该长度区间内
  int code = 0;
  int first = 0;
  int index = 0;
  for (int length = 1; length <= 16; ++length) {
    code |= reader.read_bits(1);
    const int count = counts[static_cast<std::size_t>(length)];
    if (code - first < count) {
      return symbols[static_cast<std::size_t>(index + (code - first))];
    }
    index += count;
    first = (first + count) << 1U;
    code <<= 1U;
  }
  return -1;   // 码字不在表内：数据损坏
}

// ────────────────────────────────────────────────────────────────────────────
// 量化表 / 分量
// ────────────────────────────────────────────────────────────────────────────

struct Component {
  int id{0};
  int horizontal_sampling{1};   ///< 水平采样因子（H）
  int vertical_sampling{1};     ///< 垂直采样因子（V）
  int quant_table{0};           ///< 用哪张量化表
  int dc_table{0};
  int ac_table{0};
  /// 系数平面（按 8×8 块铺开，宽高 = 块数 × 8）。
  ///
  /// 存**完整的系数平面**（而不是解完一块就丢）是为了支持渐进式：
  /// 渐进 JPEG 会在多个扫描里逐步填充同一份系数，全部扫描结束后才 IDCT。
  /// 基线 JPEG 也可以走同一路径（一次扫描就填满），于是两种模式共用一套代码。
  std::vector<int> coefficients{};
  int blocks_wide{0};
  int blocks_high{0};
  /// DC 预测值（每个扫描重置）。
  int dc_predictor{0};
};

struct Frame {
  int width{0};
  int height{0};
  int precision{8};
  int components{0};
  bool progressive{false};
  std::vector<Component> parts{};
  int max_h{1};
  int max_v{1};
  /// 每 MCU 的像素尺寸（H×8, V×8）。
  int mcu_width{0};
  int mcu_height{0};
  int mcus_wide{0};
  int mcus_high{0};
};

// ────────────────────────────────────────────────────────────────────────────
// 之字形顺序
// ────────────────────────────────────────────────────────────────────────────

constexpr std::array<int, 64> kZigZag{
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// ────────────────────────────────────────────────────────────────────────────
// 解码扩展位（霍夫曼解码出的附加位，转成有符号值）
// ────────────────────────────────────────────────────────────────────────────

/// JPEG 的"接受扩展"规则：读 `size` 位，若最高位为 0 则是负值，要减去 `2^size - 1`。
///
/// ## 为什么不能直接当无符号读
///
/// 这套编码（"amplitude category" + 补码偏移）是 JPEG 特有的，不是普通补码。
/// 按普通补码读会让所有负系数差 1，症状是**整张图偏色**、暗部发绿——
/// 而看起来"图还在"，很难想到是这里。
[[nodiscard]] auto receive_extend(BitReader& reader, int size) noexcept -> int {
  if (size <= 0) return 0;
  if (size > 16) return 0;
  const int value = reader.read_bits(size);
  // 若最高位为 0，说明是负数
  if (value < (1 << (size - 1))) {
    return value - (1 << size) + 1;
  }
  return value;
}

// ────────────────────────────────────────────────────────────────────────────
// 反量化 + IDCT
// ────────────────────────────────────────────────────────────────────────────

/// 反量化后的 8×8 块（浮点，便于做 IDCT）。
using Block = std::array<float, 64>;

/// 反量化：系数 × 量化表。
[[nodiscard]] auto dequantize(const int* coefficients, const std::array<int, 64>& quant_table)
    -> Block {
  Block out{};
  for (int i = 0; i < 64; ++i) {
    // 量化表按**自然顺序**存（上游读表时已经转过之字形），系数也是自然顺序
    out[static_cast<std::size_t>(i)] =
        static_cast<float>(coefficients[i]) * static_cast<float>(quant_table[static_cast<std::size_t>(i)]);
  }
  return out;
}

/// 二维 8×8 IDCT（**分离式**：先对列做一维、再对行做一维）。
///
/// ## 为什么不用浮点 AAN 等快速算法
///
/// 分离式朴素实现是 8×8×8×2 = 1024 次乘加，比 AAN 慢约 3 倍，但**只有 20 行**。
/// 本仓的取舍一贯是"先对再做快"：正确性容易验证（可对照公式），
/// 而快速算法的定点数误差与系数缩放很难 debug（症状是"图有轻微块状偏差"）。
///
/// 预计算余弦表（`kCos`）避免每块重复算 `cos`——那是这类实现唯一必做的优化，
/// 因为它把 1024 次 `cos` 调用降到 0。
[[nodiscard]] auto idct_8x8(const Block& input) -> Block {
  // 预计算的余弦表：`kCos[u * 8 + x] = C(u)/2 * cos((2x+1) * u * pi / 16)`
  static const auto cosine = [] {
    std::array<float, 64> table{};
    for (int u = 0; u < 8; ++u) {
      const double scale = (u == 0 ? 1.0 / std::sqrt(2.0) : 1.0) * 0.5;
      for (int x = 0; x < 8; ++x) {
        table[static_cast<std::size_t>(u * 8 + x)] = static_cast<float>(
            scale * std::cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0));
      }
    }
    return table;
  }();

  Block temp{};
  // 第一遍：对每一列做一维 IDCT（u 方向 → x 方向）
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      float sum = 0.0f;
      for (int u = 0; u < 8; ++u) {
        sum += cosine[static_cast<std::size_t>(u * 8 + x)] *
               input[static_cast<std::size_t>(y * 8 + u)];
      }
      temp[static_cast<std::size_t>(y * 8 + x)] = sum;
    }
  }
  // 第二遍：对每一行做一维 IDCT（v 方向 → y 方向）
  Block out{};
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      float sum = 0.0f;
      for (int v = 0; v < 8; ++v) {
        sum += cosine[static_cast<std::size_t>(v * 8 + y)] *
               temp[static_cast<std::size_t>(v * 8 + x)];
      }
      out[static_cast<std::size_t>(y * 8 + x)] = sum;
    }
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 色彩转换
// ────────────────────────────────────────────────────────────────────────────

[[nodiscard]] auto clamp_byte(int value) noexcept -> std::uint8_t {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return static_cast<std::uint8_t>(value);
}

/// YCbCr → RGB（ITU-R BT.601，与所有 JPEG 实现一致）。
[[nodiscard]] auto ycbcr_to_rgb(float y, float cb, float cr) noexcept
    -> std::array<std::uint8_t, 3> {
  // 定点实现（乘 4096 后取整）避免逐像素浮点差异；结果与浮点版在 8 位下一致
  const int iy = static_cast<int>(std::lround(y));
  const int icb = static_cast<int>(std::lround(cb)) - 128;
  const int icr = static_cast<int>(std::lround(cr)) - 128;
  const int r = iy + ((91881 * icr) >> 16);
  const int g = iy - ((22554 * icb + 46802 * icr) >> 16);
  const int b = iy + ((116130 * icb) >> 16);
  return {clamp_byte(r), clamp_byte(g), clamp_byte(b)};
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// 主解码流程
// ────────────────────────────────────────────────────────────────────────────

namespace {

/// 从量化表段（DQT）读表，**转成自然顺序**。
auto read_quant_table(std::span<const std::uint8_t> bytes, std::size_t& offset,
                      std::array<std::array<int, 64>, 4>& tables) -> Status {
  while (offset < bytes.size()) {
    const auto descriptor = bytes[offset++];
    const int precision = descriptor >> 4;      // 0 = 8 位, 1 = 16 位
    const int index = descriptor & 0x0F;
    if (index >= 4) {
      return st::unexpected(ErrorCode::Parse, std::format("DQT：量化表下标 {} 越界", index));
    }
    if (precision != 0 && precision != 1) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("DQT：量化精度 {} 不支持（0=8位, 1=16位）", precision));
    }
    const std::size_t needed = precision == 0 ? 64U : 128U;
    if (offset + needed > bytes.size()) {
      return st::unexpected(ErrorCode::Parse, "DQT：表数据被截断");
    }
    for (int i = 0; i < 64; ++i) {
      int value = 0;
      if (precision == 0) {
        value = bytes[offset + static_cast<std::size_t>(i)];
      } else {
        value = static_cast<int>(read_be16(bytes, offset + static_cast<std::size_t>(i) * 2U));
      }
      // 存成自然顺序（文件里是之字形）
      tables[static_cast<std::size_t>(index)][static_cast<std::size_t>(kZigZag[static_cast<std::size_t>(i)])] =
          value;
    }
    offset += needed;
  }
  return st::ok();
}

/// 从霍夫曼表段（DHT）读表。
auto read_huffman_tables(std::span<const std::uint8_t> bytes, std::size_t& offset,
                         std::array<HuffmanTable, 4>& dc_tables,
                         std::array<HuffmanTable, 4>& ac_tables) -> Status {
  while (offset < bytes.size()) {
    if (offset + 17 > bytes.size()) {
      return st::unexpected(ErrorCode::Parse, "DHT：表头被截断");
    }
    const auto descriptor = bytes[offset++];
    const int table_class = descriptor >> 4;    // 0 = DC, 1 = AC
    const int index = descriptor & 0x0F;
    if (index >= 4 || table_class > 1) {
      return st::unexpected(ErrorCode::Parse,
                           std::format("DHT：表描述符 {:#x} 无效（class={} index={}）", descriptor,
                                       table_class, index));
    }
    HuffmanTable table{};
    int total = 0;
    for (int length = 1; length <= 16; ++length) {
      table.counts[static_cast<std::size_t>(length)] = bytes[offset++];
      total += table.counts[static_cast<std::size_t>(length)];
    }
    if (total > 256 || offset + static_cast<std::size_t>(total) > bytes.size()) {
      return st::unexpected(ErrorCode::Parse, std::format("DHT：符号数 {} 越界或被截断", total));
    }
    for (int i = 0; i < total; ++i) {
      table.symbols[static_cast<std::size_t>(i)] = bytes[offset++];
    }
    table.defined = true;
    if (table_class == 0) {
      dc_tables[static_cast<std::size_t>(index)] = table;
    } else {
      ac_tables[static_cast<std::size_t>(index)] = table;
    }
  }
  return st::ok();
}

/// 解码一个 8×8 块的系数（基线模式：DC 差分 + AC 游程）。
auto decode_block(BitReader& reader, const HuffmanTable& dc_table, const HuffmanTable& ac_table,
                  const std::array<int, 64>& quant_table, int& dc_predictor,
                  std::array<int, 64>& out) -> bool {
  (void)quant_table;
  out.fill(0);
  // DC：霍夫曼解出"位数"，再读该位数的差分
  const int dc_size = dc_table.decode(reader);
  if (dc_size < 0) return false;
  dc_predictor += receive_extend(reader, dc_size);
  out[0] = dc_predictor;
  // AC：游程编码
  int k = 1;
  while (k < 64) {
    const int rs = ac_table.decode(reader);
    if (rs < 0) return false;
    const int run = rs >> 4;         // 前面有多少个 0
    const int size = rs & 0x0F;      // 后面值的位数
    if (size == 0) {
      if (run == 15) {
        k += 16;                     // ZRL：16 个 0
        continue;
      }
      break;                         // EOB：块结束
    }
    k += run;
    if (k >= 64) break;
    out[static_cast<std::size_t>(k)] = receive_extend(reader, size);
    ++k;
  }
  return true;
}

/// 渐进式：解码 DC 首次扫描。
auto decode_dc_first(BitReader& reader, const HuffmanTable& dc_table, int& dc_predictor,
                     int successive_low, int* block, int blocks_wide) -> bool {
  (void)blocks_wide;
  const int size = dc_table.decode(reader);
  if (size < 0) return false;
  dc_predictor += receive_extend(reader, size);
  // 渐进式 DC：值左移（后续扫描会补低位）
  block[0] = dc_predictor << successive_low;
  return true;
}

/// 渐进式：解码 DC 细化扫描（补一位）。
auto decode_dc_refine(BitReader& reader, int* block) -> bool {
  if (reader.read_bits(1) != 0) block[0] |= 1;
  return true;
}

/// 渐进式：解码 AC 首次扫描。
auto decode_ac_first(BitReader& reader, const HuffmanTable& ac_table, int spectral_start,
                     int spectral_end, int successive_low, int* block) -> bool {
  if (spectral_start == 0) return true;   // DC 由 DC 扫描负责
  int k = spectral_start;
  while (k <= spectral_end && k < 64) {
    const int rs = ac_table.decode(reader);
    if (rs < 0) return false;
    const int run = rs >> 4;
    const int size = rs & 0x0F;
    if (size == 0) {
      if (run != 15) break;   // EOB
      k += 16;
      continue;
    }
    k += run;
    if (k > spectral_end || k >= 64) break;
    block[kZigZag[static_cast<std::size_t>(k)]] = receive_extend(reader, size) << successive_low;
    ++k;
  }
  return true;
}

/// 渐进式：解码 AC 细化扫描。
///
/// 这一段的规则最绕：已非零的系数**补一位**（而不是赋值），
/// 新出现的非零系数则按"当前位 + 符号"插入。写错的症状是
/// "渐进式图比基线模糊/有噪点"，而基线图完全正常——很容易误判为"素材质量差"。
auto decode_ac_refine(BitReader& reader, const HuffmanTable& ac_table, int spectral_start,
                      int spectral_end, int successive_low, int* block) -> bool {
  int k = spectral_start;
  const int positive_bit = 1 << successive_low;
  const int negative_bit = -1 << successive_low;
  while (k <= spectral_end && k < 64) {
    const int rs = ac_table.decode(reader);
    if (rs < 0) return false;
    int run = rs >> 4;
    int size = rs & 0x0F;
    int value = 0;
    if (size == 0) {
      if (run != 15) {
        // EOB：把剩余已非零系数的细化位读完
        if (reader.read_bits(1) != 0) return false;
        return true;
      }
      run = 16;
    } else {
      value = reader.read_bits(1) != 0 ? positive_bit : negative_bit;
    }
    // 跳过 `run` 个"尚未非零"的位置，同时对途中已非零的系数补细化位
    while (k <= spectral_end && k < 64) {
      const int index = kZigZag[static_cast<std::size_t>(k)];
      if (block[index] != 0) {
        if (reader.read_bits(1) != 0) {
          // 细化位为 1：按当前位宽向远离 0 的方向调整
          if ((block[index] & positive_bit) == 0) {
            block[index] += block[index] > 0 ? positive_bit : negative_bit;
          }
        }
      } else {
        if (run == 0) {
          if (size != 0) block[index] = value;
          ++k;
          break;
        }
        --run;
      }
      ++k;
    }
  }
  return true;
}

}  // namespace

auto jpeg_decode(std::span<const std::uint8_t> bytes) -> Result<JpegImage> {
  if (bytes.size() < 4U || bytes[0] != 0xFF || bytes[1] != 0xD8) {
    return st::unexpected(ErrorCode::Parse, "JPEG 缺少 SOI 标记（FF D8）");
  }

  Frame frame{};
  std::array<std::array<int, 64>, 4> quant_tables{};
  std::array<bool, 4> quant_defined{};
  std::array<HuffmanTable, 4> dc_tables{};
  std::array<HuffmanTable, 4> ac_tables{};
  bool have_frame = false;

  std::size_t position = 2;
  while (position + 4U <= bytes.size()) {
    // 找下一个标记（跳过填充的 0xFF）
    if (bytes[position] != 0xFF) {
      ++position;
      continue;
    }
    while (position < bytes.size() && bytes[position] == 0xFF) ++position;
    if (position >= bytes.size()) break;
    const auto marker = bytes[position++];

    // 无长度的标记
    if (marker == 0xD9) break;                     // EOI
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;   // RSTn / TEM

    if (position + 2U > bytes.size()) break;
    const std::uint32_t length = read_be16(bytes, position);
    if (length < 2U || position + length > bytes.size()) break;
    const auto segment = bytes.subspan(position + 2U, length - 2U);
    position += length;

    switch (marker) {
      case 0xDB: {   // DQT
        std::size_t offset = 0;
        if (const auto status = read_quant_table(segment, offset, quant_tables); !status) {
          return st::forward_error(status.error());
        }
        for (std::size_t i = 0; i < 4; ++i) {
          bool non_zero = false;
          for (const int value : quant_tables[i]) {
            if (value != 0) non_zero = true;
          }
          if (non_zero) quant_defined[i] = true;
        }
        break;
      }
      case 0xC4: {   // DHT
        std::size_t offset = 0;
        if (const auto status = read_huffman_tables(segment, offset, dc_tables, ac_tables);
            !status) {
          return st::forward_error(status.error());
        }
        break;
      }
      case 0xC0:   // SOF0 基线
      case 0xC1:   // SOF1 扩展顺序
      case 0xC2: { // SOF2 渐进式
        if (have_frame) {
          return st::unexpected(ErrorCode::Parse, "JPEG 出现多个 SOF（不支持多帧）");
        }
        if (segment.size() < 6U) {
          return st::unexpected(ErrorCode::Parse, "SOF 段被截断");
        }
        frame.precision = segment[0];
        frame.height = static_cast<int>(read_be16(segment, 1));
        frame.width = static_cast<int>(read_be16(segment, 3));
        frame.components = segment[5];
        frame.progressive = marker == 0xC2;
        if (frame.precision != 8) {
          return st::unexpected(
              ErrorCode::Unsupported,
              std::format("JPEG 精度 {} 位不受支持（只支持 8 位；12 位 JPEG 极罕见）",
                          frame.precision));
        }
        if (frame.width <= 0 || frame.height <= 0) {
          return st::unexpected(ErrorCode::Parse,
                                std::format("JPEG 尺寸无效：{}×{}", frame.width, frame.height));
        }
        if (frame.components != 1 && frame.components != 3 && frame.components != 4) {
          return st::unexpected(ErrorCode::Unsupported,
                                std::format("JPEG 分量数 {} 不受支持（只支持 1/3/4）",
                                            frame.components));
        }
        if (segment.size() < 6U + static_cast<std::size_t>(frame.components) * 3U) {
          return st::unexpected(ErrorCode::Parse, "SOF 分量表被截断");
        }
        frame.parts.clear();
        for (int i = 0; i < frame.components; ++i) {
          Component part{};
          part.id = segment[6U + static_cast<std::size_t>(i) * 3U];
          const auto sampling = segment[7U + static_cast<std::size_t>(i) * 3U];
          part.horizontal_sampling = sampling >> 4;
          part.vertical_sampling = sampling & 0x0F;
          if (part.horizontal_sampling < 1 || part.horizontal_sampling > 4 ||
              part.vertical_sampling < 1 || part.vertical_sampling > 4) {
            return st::unexpected(ErrorCode::Parse,
                                  std::format("JPEG 分量 {} 的采样因子 {}×{} 无效", i,
                                              part.horizontal_sampling, part.vertical_sampling));
          }
          part.quant_table = segment[8U + static_cast<std::size_t>(i) * 3U];
          frame.max_h = std::max(frame.max_h, part.horizontal_sampling);
          frame.max_v = std::max(frame.max_v, part.vertical_sampling);
          frame.parts.push_back(part);
        }
        frame.mcu_width = frame.max_h * 8;
        frame.mcu_height = frame.max_v * 8;
        frame.mcus_wide = (frame.width + frame.mcu_width - 1) / frame.mcu_width;
        frame.mcus_high = (frame.height + frame.mcu_height - 1) / frame.mcu_height;
        // 为每个分量分配系数平面
        for (Component& part : frame.parts) {
          part.blocks_wide = frame.mcus_wide * part.horizontal_sampling;
          part.blocks_high = frame.mcus_high * part.vertical_sampling;
          part.coefficients.assign(
              static_cast<std::size_t>(part.blocks_wide) * static_cast<std::size_t>(part.blocks_high) *
                  64U,
              0);
        }
        have_frame = true;
        break;
      }
      case 0xC3:   // SOF3 无损
        return st::unexpected(ErrorCode::Unsupported,
                              "JPEG 无损模式（SOF3）不受支持；请用 baseline/progressive 重新导出");
      case 0xC9:   // SOF9 算术编码
      case 0xCA:
      case 0xCB:
        return st::unexpected(ErrorCode::Unsupported,
                              "JPEG 算术编码（SOF9/10/11）不受支持；请关掉算术编码重新导出");
      case 0xDA: {   // SOS 扫描
        if (!have_frame) {
          return st::unexpected(ErrorCode::Parse, "JPEG 在 SOF 之前遇到 SOS");
        }
        if (segment.empty()) {
          return st::unexpected(ErrorCode::Parse, "SOS 段为空");
        }
        const int component_count = segment[0];
        if (component_count < 1 || component_count > frame.components ||
            segment.size() < 1U + static_cast<std::size_t>(component_count) * 2U + 3U) {
          return st::unexpected(ErrorCode::Parse, "SOS 分量表无效");
        }
        // 扫描参数
        const auto params_at = 1U + static_cast<std::size_t>(component_count) * 2U;
        const int spectral_start = segment[params_at];
        const int spectral_end = segment[params_at + 1U];
        const int approx = segment[params_at + 2U];
        const int successive_high = approx >> 4;
        const int successive_low = approx & 0x0F;

        // 每个分量在本扫描里用的表
        struct ScanPart {
          Component* component{nullptr};
          int dc_table{0};
          int ac_table{0};
        };
        std::vector<ScanPart> scan_parts;
        for (int i = 0; i < component_count; ++i) {
          const int id = segment[1U + static_cast<std::size_t>(i) * 2U];
          const auto tables = segment[2U + static_cast<std::size_t>(i) * 2U];
          Component* found = nullptr;
          for (Component& part : frame.parts) {
            if (part.id == id) found = &part;
          }
          if (found == nullptr) {
            return st::unexpected(ErrorCode::Parse,
                                  std::format("SOS 引用了不存在的分量 id={}", id));
          }
          ScanPart scan{};
          scan.component = found;
          scan.dc_table = tables >> 4;
          scan.ac_table = tables & 0x0F;
          if (scan.dc_table >= 4 || scan.ac_table >= 4) {
            return st::unexpected(ErrorCode::Parse, "SOS 的表下标越界");
          }
          scan_parts.push_back(scan);
        }

        BitReader reader(bytes, position);
        for (Component& part : frame.parts) part.dc_predictor = 0;

        if (frame.progressive) {
          // ── 渐进式：按扫描类型处理 ──
          for (int mcu_y = 0; mcu_y < frame.mcus_high; ++mcu_y) {
            for (int mcu_x = 0; mcu_x < frame.mcus_wide; ++mcu_x) {
              for (const ScanPart& scan : scan_parts) {
                const int blocks = scan.component->horizontal_sampling *
                                   scan.component->vertical_sampling;
                for (int block = 0; block < blocks; ++block) {
                  const int bx = mcu_x * scan.component->horizontal_sampling +
                                 (block % scan.component->horizontal_sampling);
                  const int by = mcu_y * scan.component->vertical_sampling +
                                 (block / scan.component->horizontal_sampling);
                  if (bx >= scan.component->blocks_wide ||
                      by >= scan.component->blocks_high) {
                    continue;
                  }
                  int* base = scan.component->coefficients.data() +
                              (static_cast<std::size_t>(by) *
                                   static_cast<std::size_t>(scan.component->blocks_wide) +
                               static_cast<std::size_t>(bx)) * 64U;
                  if (spectral_start == 0 && successive_high == 0) {
                    if (!decode_dc_first(reader,
                                         dc_tables[static_cast<std::size_t>(scan.dc_table)],
                                         scan.component->dc_predictor, successive_low, base,
                                         scan.component->blocks_wide)) {
                      // 数据耗尽：停止解析（截断的图也要尽量出图）
                      break;
                    }
                  } else if (spectral_start == 0) {
                    if (!decode_dc_refine(reader, base)) break;
                  } else if (successive_high == 0) {
                    if (!decode_ac_first(reader,
                                         ac_tables[static_cast<std::size_t>(scan.ac_table)],
                                         spectral_start, spectral_end, successive_low, base)) {
                      break;
                    }
                  } else {
                    if (!decode_ac_refine(reader,
                                          ac_tables[static_cast<std::size_t>(scan.ac_table)],
                                          spectral_start, spectral_end, successive_high, base)) {
                      break;
                    }
                  }
                }
              }
            }
          }
        } else {
          // ── 基线：一趟解完所有块 ──
          for (int mcu_y = 0; mcu_y < frame.mcus_high; ++mcu_y) {
            for (int mcu_x = 0; mcu_x < frame.mcus_wide; ++mcu_x) {
              for (const ScanPart& scan : scan_parts) {
                const int blocks = scan.component->horizontal_sampling *
                                   scan.component->vertical_sampling;
                for (int block = 0; block < blocks; ++block) {
                  const int bx = mcu_x * scan.component->horizontal_sampling +
                                 (block % scan.component->horizontal_sampling);
                  const int by = mcu_y * scan.component->vertical_sampling +
                                 (block / scan.component->horizontal_sampling);
                  if (bx >= scan.component->blocks_wide ||
                      by >= scan.component->blocks_high) {
                    continue;
                  }
                  auto quant = quant_tables[static_cast<std::size_t>(
                      std::min(scan.component->quant_table, 3))];
                  if (!quant_defined[static_cast<std::size_t>(
                          std::min(scan.component->quant_table, 3))]) {
                    return st::unexpected(
                        ErrorCode::Parse,
                        std::format("JPEG 缺少量化表 {}", scan.component->quant_table));
                  }
                  int* base = scan.component->coefficients.data() +
                              (static_cast<std::size_t>(by) *
                                   static_cast<std::size_t>(scan.component->blocks_wide) +
                               static_cast<std::size_t>(bx)) * 64U;
                  std::array<int, 64> block_values{};
                  if (!decode_block(reader, dc_tables[static_cast<std::size_t>(scan.dc_table)],
                                    ac_tables[static_cast<std::size_t>(scan.ac_table)], quant,
                                    scan.component->dc_predictor, block_values)) {
                    break;
                  }
                  std::copy(block_values.begin(), block_values.end(), base);
                }
              }
            }
          }
        }
        // 扫描数据结束：跳到下一个标记（从读取器当前位置继续找 0xFF）
        position = reader.position();
        if (position < bytes.size() && bytes[position] == 0xFF) {
          // 已在标记处，交由外层循环处理
        }
        break;
      }
      default:
        break;   // 其它段（APPn / COM / DRI 等）跳过
    }
    if (marker == 0xDA) {
      // SOS 之后的数据长度未在段头里给出，需靠熵解码的位读取器定位结束点；
      // 定位已在上面做（`position = reader.position()`）。
    }
  }

  if (!have_frame) {
    return st::unexpected(ErrorCode::Parse, "JPEG 未找到 SOF（帧头）");
  }

  // ── 反量化 + IDCT + 上采样 + 色彩转换 ──
  //
  // 每个分量先解成"浮点采样平面"（含上采样到全尺寸），再合成 RGB。
  const auto plane_width = static_cast<std::size_t>(frame.width);
  const auto plane_height = static_cast<std::size_t>(frame.height);
  std::vector<std::vector<float>> planes(frame.parts.size());
  for (std::size_t part_index = 0; part_index < frame.parts.size(); ++part_index) {
    planes[part_index].assign(plane_width * plane_height, 0.0f);
  }

  for (std::size_t part_index = 0; part_index < frame.parts.size(); ++part_index) {
    const Component& part = frame.parts[part_index];
    const auto quant_index = static_cast<std::size_t>(std::min(part.quant_table, 3));
    if (!quant_defined[quant_index] && !frame.progressive) {
      // 已经在解码时校验过；这里兜底（渐进式可能在别的扫描里定义了表）
      continue;
    }
    const auto& quant = quant_tables[quant_index];
    std::array<int, 64> zero_block{};
    (void)zero_block;

    for (int by = 0; by < part.blocks_high; ++by) {
      for (int bx = 0; bx < part.blocks_wide; ++bx) {
        const int* coefficients =
            part.coefficients.data() +
            (static_cast<std::size_t>(by) * static_cast<std::size_t>(part.blocks_wide) +
             static_cast<std::size_t>(bx)) * 64U;
        // 渐进式：系数已在扫描里左移过，直接反量化即可；量化表可能未定义（全 0 块）
        Block block_values = dequantize(coefficients, quant);
        const Block spatial = idct_8x8(block_values);
        // 写到该分量的全尺寸平面（按采样因子放大）
        for (int y = 0; y < 8; ++y) {
          for (int x = 0; x < 8; ++x) {
            const int plane_x = (bx * 8 + x);
            const int plane_y = (by * 8 + y);
            // 采样因子决定该分量在输出平面上覆盖的像素范围
            const int step_x = frame.max_h / part.horizontal_sampling;
            const int step_y = frame.max_v / part.vertical_sampling;
            for (int dy = 0; dy < step_y; ++dy) {
              for (int dx = 0; dx < step_x; ++dx) {
                const int out_x = plane_x * step_x + dx;
                const int out_y = plane_y * step_y + dy;
                if (out_x < frame.width && out_y < frame.height) {
                  planes[part_index][static_cast<std::size_t>(out_y) * plane_width +
                                     static_cast<std::size_t>(out_x)] =
                      spatial[static_cast<std::size_t>(y * 8 + x)] + 128.0f;
                }
              }
            }
          }
        }
      }
    }
  }

  // 合成 RGBA
  JpegImage image{};
  image.width = frame.width;
  image.height = frame.height;
  image.rgba.assign(plane_width * plane_height * 4U, 255U);
  for (int y = 0; y < frame.height; ++y) {
    for (int x = 0; x < frame.width; ++x) {
      const auto index = static_cast<std::size_t>(y) * plane_width + static_cast<std::size_t>(x);
      const auto out_at = index * 4U;
      if (frame.parts.size() == 1) {
        const auto gray = clamp_byte(static_cast<int>(std::lround(planes[0][index])));
        image.rgba[out_at + 0] = gray;
        image.rgba[out_at + 1] = gray;
        image.rgba[out_at + 2] = gray;
      } else {
        const auto rgb = ycbcr_to_rgb(planes[0][index], planes[1][index], planes[2][index]);
        image.rgba[out_at + 0] = rgb[0];
        image.rgba[out_at + 1] = rgb[1];
        image.rgba[out_at + 2] = rgb[2];
      }
      image.rgba[out_at + 3] = 255U;
    }
  }
  return image;
}

}  // namespace st::codec
