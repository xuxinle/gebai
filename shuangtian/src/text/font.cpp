#include "st/text/font.hpp"

#include <format>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"

// 字体表读取全部走「大端字节序 + 显式边界检查」的受检读取器（见 `read_u16`/`read_u32`）：
// 越界返回 0 并在结构层显式判错，因此不存在裸指针算术与类型双关。

namespace st::text {
namespace {

// —— 解析上限（防御恶意/损坏字体）——
inline constexpr std::size_t k_max_font_bytes = 256U * 1024U * 1024U;
inline constexpr std::uint32_t k_max_face_count = 1024U;
inline constexpr int k_max_composite_depth = 8;
inline constexpr std::size_t k_max_component_count = 64;
inline constexpr std::size_t k_max_type2_stack = 48;
inline constexpr int k_max_subr_depth = 10;
inline constexpr int k_max_dict_stack = 48;

/// 四字符表标签 → 大端整数（`'g' 'l' 'y' 'f'` → `0x676C7966`）。
[[nodiscard]] constexpr auto tag_of(char a, char b, char c, char d) noexcept -> std::uint32_t {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8U) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

[[nodiscard]] constexpr auto in_range(std::span<const std::uint8_t> data, std::size_t offset,
                                      std::size_t length) noexcept -> bool {
  return offset <= data.size() && length <= data.size() - offset;
}

[[nodiscard]] auto read_u8(std::span<const std::uint8_t> data, std::size_t offset) noexcept
    -> std::uint8_t {
  return offset < data.size() ? data[offset] : std::uint8_t{0};
}

[[nodiscard]] auto read_u16(std::span<const std::uint8_t> data, std::size_t offset) noexcept
    -> std::uint16_t {
  if (!in_range(data, offset, 2)) return 0;
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8U) |
                                    static_cast<std::uint16_t>(data[offset + 1]));
}

[[nodiscard]] auto read_i16(std::span<const std::uint8_t> data, std::size_t offset) noexcept
    -> std::int16_t {
  return std::bit_cast<std::int16_t>(read_u16(data, offset));
}

[[nodiscard]] auto read_i8(std::span<const std::uint8_t> data, std::size_t offset) noexcept
    -> std::int8_t {
  return std::bit_cast<std::int8_t>(read_u8(data, offset));
}

[[nodiscard]] auto read_u32(std::span<const std::uint8_t> data, std::size_t offset) noexcept
    -> std::uint32_t {
  if (!in_range(data, offset, 4)) return 0;
  return (static_cast<std::uint32_t>(data[offset]) << 24U) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
         static_cast<std::uint32_t>(data[offset + 3]);
}

[[nodiscard]] auto read_i32(std::span<const std::uint8_t> data, std::size_t offset) noexcept
    -> std::int32_t {
  return std::bit_cast<std::int32_t>(read_u32(data, offset));
}

/// F2Dot14 定点（TrueType 复合字形变换矩阵）。
[[nodiscard]] auto read_f2dot14(std::span<const std::uint8_t> data, std::size_t offset) noexcept
    -> float {
  return static_cast<float>(read_i16(data, offset)) / 16384.0f;
}

/// 读 `width`（1..4）字节大端无符号整数（CFF INDEX 偏移数组）。
[[nodiscard]] auto read_uint_be(std::span<const std::uint8_t> data, std::size_t offset,
                                std::size_t width) noexcept -> std::uint32_t {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < width; ++i) {
    value = (value << 8U) | static_cast<std::uint32_t>(read_u8(data, offset + i));
  }
  return value;
}

/// 浮点 → 整数（越界夹取，避免 C++ 未定义的浮点转整数溢出）。
[[nodiscard]] auto to_int_clamped(float value) noexcept -> int {
  if (value <= -1.0e7f) return -10000000;
  if (value >= 1.0e7f) return 10000000;
  return static_cast<int>(value);
}

// —— sfnt 表目录 ——

struct TableRecord {
  std::uint32_t tag{0};
  std::size_t offset{0};
  std::size_t length{0};
};

// —— cmap（码点 → 字形索引）——

struct CmapGroup {
  std::uint32_t first{0};
  std::uint32_t last{0};
  std::uint32_t start_glyph{0};
};

struct CmapTable {
  enum class Kind : std::uint8_t { None, Byte, Segment, Trimmed, Groups };

  Kind kind{Kind::None};
  // 格式 4
  std::vector<std::uint16_t> end_code{};
  std::vector<std::uint16_t> start_code{};
  std::vector<std::uint16_t> id_delta{};
  std::vector<std::uint16_t> id_range_offset{};
  std::size_t range_base{0};
  // 格式 12
  std::vector<CmapGroup> groups{};
  // 格式 6
  std::uint16_t first_code{0};
  std::vector<std::uint16_t> codes{};
  // 格式 0
  std::array<std::uint8_t, 256> byte_map{};
};

// —— glyf/loca ——

struct GlyfTable {
  std::size_t glyf_offset{0};
  std::size_t glyf_length{0};
  std::size_t loca_offset{0};
  bool long_loca{false};
  bool present{false};
};

// —— CFF ——

/// CFF INDEX：`offsets` 为 `count + 1` 项（末项即 INDEX 结束偏移），偏移相对 CFF 表起点。
struct CffIndex {
  std::vector<std::size_t> offsets{};

  [[nodiscard]] auto count() const noexcept -> std::size_t {
    return offsets.empty() ? 0 : offsets.size() - 1;
  }
  [[nodiscard]] auto item(std::span<const std::uint8_t> cff, std::size_t index) const noexcept
      -> std::span<const std::uint8_t> {
    if (index + 1 >= offsets.size()) return {};
    return cff.subspan(offsets[index], offsets[index + 1] - offsets[index]);
  }
};

struct CffPrivate {
  std::int32_t default_width_x{0};
  std::int32_t nominal_width_x{0};
  CffIndex subrs{};
};

struct CffTable {
  bool parsed{false};
  bool cid_keyed{false};
  std::size_t base{0};
  std::size_t length{0};
  CffIndex charstrings{};
  CffIndex global_subrs{};
  std::vector<CffPrivate> privates{};
  struct FdRange {
    std::uint32_t first{0};
    std::uint8_t fd{0};
  };
  std::vector<FdRange> fd_ranges{};
  std::vector<std::uint8_t> fd_by_glyph{};

  [[nodiscard]] auto fd_for(GlyphId id) const noexcept -> std::size_t {
    if (!fd_by_glyph.empty()) {
      return static_cast<std::size_t>(id) < fd_by_glyph.size()
                 ? static_cast<std::size_t>(fd_by_glyph[static_cast<std::size_t>(id)])
                 : 0;
    }
    if (fd_ranges.empty()) return 0;
    auto it = std::upper_bound(
        fd_ranges.begin(), fd_ranges.end(), id,
        [](GlyphId value, const FdRange& range) noexcept { return value < range.first; });
    if (it == fd_ranges.begin()) return 0;
    --it;
    return static_cast<std::size_t>(it->fd);
  }
};

/// CFF DICT 条目（操作符 + 操作数）。
struct DictOp {
  int op{0};
  int ext{0};
  std::vector<double> args{};
};

/// 字体解析结果与缓存（`FontFace::Data` 的实体；仅本 TU 可见）。
struct FontData {
  std::string path{};
  std::string name{};
  int face_index{0};
  std::vector<std::uint8_t> bytes{};
  std::vector<TableRecord> tables{};
  FontMetrics metrics{};
  std::size_t hmtx_offset{0};
  std::size_t hmtx_length{0};
  std::uint32_t num_h_metrics{0};
  std::uint32_t num_glyphs{0};
  CmapTable cmap{};
  GlyfTable glyf{};
  CffTable cff{};
  std::unordered_map<std::uint32_t, std::int16_t> kern{};
  std::mutex cff_mutex{};
  std::mutex outline_mutex{};
  std::unordered_map<GlyphId, std::shared_ptr<const raster::Path>> outlines{};
  std::mutex hints_mutex{};
  std::unordered_map<GlyphId, std::vector<StemHint>> hints{};

  [[nodiscard]] auto span() const noexcept -> std::span<const std::uint8_t> { return bytes; }

  [[nodiscard]] auto table(std::uint32_t want) const noexcept -> const TableRecord* {
    const auto it = std::lower_bound(
        tables.begin(), tables.end(), want,
        [](const TableRecord& record, std::uint32_t value) noexcept { return record.tag < value; });
    if (it == tables.end() || it->tag != want) return nullptr;
    return &(*it);
  }
};

}  // namespace

/// `FontFace` 内部数据：不可变解析结果 + 缓存（拷贝 `FontFace` 时共享）。
struct FontFace::Data {
  FontData value{};
};

namespace {

// —— sfnt：表目录与核心表 ——

[[nodiscard]] auto parse_sfnt_directory(FontData& data, std::size_t face_offset) -> Status {
  const auto bytes = data.span();
  if (!in_range(bytes, face_offset, 12)) {
    return unexpected(ErrorCode::Parse, "sfnt 头截断");
  }
  const std::uint32_t version = read_u32(bytes, face_offset);
  const bool known = version == 0x00010000U || version == tag_of('O', 'T', 'T', 'O') ||
                     version == tag_of('t', 'r', 'u', 'e') || version == tag_of('t', 'y', 'p', '1');
  if (!known) {
    return unexpected(ErrorCode::Parse, "未知 sfnt 版本");
  }
  const std::uint16_t count = read_u16(bytes, face_offset + 4);
  if (count == 0) return unexpected(ErrorCode::Parse, "sfnt 表数量为 0");
  const std::size_t directory = face_offset + 12;
  const std::size_t directory_bytes = static_cast<std::size_t>(count) * 16;
  if (!in_range(bytes, directory, directory_bytes)) {
    return unexpected(ErrorCode::Parse, "sfnt 表目录截断");
  }
  data.tables.clear();
  data.tables.reserve(count);
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::size_t record = directory + static_cast<std::size_t>(i) * 16;
    TableRecord entry{};
    entry.tag = read_u32(bytes, record);
    entry.offset = read_u32(bytes, record + 8);
    entry.length = read_u32(bytes, record + 12);
    if (entry.offset > bytes.size()) {
      return unexpected(ErrorCode::Parse, "sfnt 表偏移越界");
    }
    // 部分字体 length 字段不严谨：夹取到文件末尾，永不越过文件边界。
    if (entry.length > bytes.size() - entry.offset) entry.length = bytes.size() - entry.offset;
    data.tables.push_back(entry);
  }
  std::ranges::sort(data.tables, {}, &TableRecord::tag);
  return ok();
}

[[nodiscard]] auto parse_head_hhea(FontData& data) -> Status {
  const auto bytes = data.span();
  const TableRecord* head = data.table(tag_of('h', 'e', 'a', 'd'));
  if (head == nullptr) return unexpected(ErrorCode::Parse, "缺少 head 表");
  if (head->length < 54) return unexpected(ErrorCode::Parse, "head 表过短");
  const std::uint16_t upem = read_u16(bytes, head->offset + 18);
  if (upem == 0) return unexpected(ErrorCode::Parse, "unitsPerEm 为 0");
  data.metrics.units_per_em = static_cast<float>(upem);
  data.glyf.long_loca = read_i16(bytes, head->offset + 50) != 0;

  const TableRecord* hhea = data.table(tag_of('h', 'h', 'e', 'a'));
  if (hhea == nullptr || hhea->length < 36) return unexpected(ErrorCode::Parse, "缺少 hhea 表");
  data.metrics.ascender = static_cast<float>(read_i16(bytes, hhea->offset + 4));
  data.metrics.descender = static_cast<float>(read_i16(bytes, hhea->offset + 6));
  data.metrics.line_gap = static_cast<float>(read_i16(bytes, hhea->offset + 8));
  data.num_h_metrics = read_u16(bytes, hhea->offset + 34);
  if (data.num_h_metrics == 0) return unexpected(ErrorCode::Parse, "hhea numberOfHMetrics 为 0");

  const TableRecord* maxp = data.table(tag_of('m', 'a', 'x', 'p'));
  data.num_glyphs = maxp != nullptr && maxp->length >= 6 ? read_u16(bytes, maxp->offset + 4) : 0;

  const TableRecord* hmtx = data.table(tag_of('h', 'm', 't', 'x'));
  if (hmtx == nullptr) return unexpected(ErrorCode::Parse, "缺少 hmtx 表");
  data.hmtx_offset = hmtx->offset;
  data.hmtx_length = hmtx->length;
  const std::size_t needed =
      static_cast<std::size_t>(data.num_h_metrics) * 4;
  if (hmtx->length < needed) return unexpected(ErrorCode::Parse, "hmtx 表过短");

  if (data.metrics.ascender == 0.0f && data.metrics.descender == 0.0f) {
    // 退化：用 OS/2 typo 度量，再退化为 upem 比例（0.75 / 0.25）。
    const TableRecord* os2 = data.table(tag_of('O', 'S', '/', '2'));
    if (os2 != nullptr && os2->length >= 72) {
      data.metrics.ascender = static_cast<float>(read_i16(bytes, os2->offset + 68));
      data.metrics.descender = static_cast<float>(read_i16(bytes, os2->offset + 70));
    }
    if (data.metrics.ascender == 0.0f && data.metrics.descender == 0.0f) {
      data.metrics.ascender = data.metrics.units_per_em * 0.75f;
      data.metrics.descender = -data.metrics.units_per_em * 0.25f;
    }
  }
  data.metrics.glyph_count = data.num_glyphs;
  return ok();
}

[[nodiscard]] auto hmtx_advance(const FontData& data, GlyphId id) noexcept -> std::uint16_t {
  const std::size_t count = data.num_h_metrics;
  const std::size_t index = static_cast<std::size_t>(id) < count ? static_cast<std::size_t>(id)
                                                                : (count > 0 ? count - 1 : 0);
  return read_u16(data.span(), data.hmtx_offset + index * 4);
}

[[nodiscard]] auto hmtx_bearing(const FontData& data, GlyphId id) noexcept -> std::int16_t {
  const std::size_t count = data.num_h_metrics;
  if (static_cast<std::size_t>(id) < count) {
    return read_i16(data.span(), data.hmtx_offset + static_cast<std::size_t>(id) * 4 + 2);
  }
  const std::size_t extra = static_cast<std::size_t>(id) - count;
  return read_i16(data.span(), data.hmtx_offset + count * 4 + extra * 2);
}

// —— cmap ——

[[nodiscard]] auto cmap_subtable_score(std::uint16_t platform, std::uint16_t encoding,
                                       std::uint16_t format) noexcept -> int {
  int score = 0;
  switch (format) {
    case 12: score = 40; break;
    case 4: score = 30; break;
    case 6: score = 20; break;
    case 0: score = 10; break;
    default: return -1;
  }
  if (platform == 3 && (encoding == 10 || encoding == 1)) score += 4;
  else if (platform == 0) score += 3;
  else if (platform == 1 && encoding == 0) score += 1;
  else score -= 5;
  return score;
}

[[nodiscard]] auto parse_cmap_format4(FontData& data, std::size_t sub) -> Status {
  const auto bytes = data.span();
  if (!in_range(bytes, sub, 16)) return unexpected(ErrorCode::Parse, "cmap 格式 4 截断");
  const std::uint16_t seg_x2 = read_u16(bytes, sub + 6);
  if (seg_x2 == 0 || seg_x2 % 2 != 0) return unexpected(ErrorCode::Parse, "cmap 格式 4 段数非法");
  const std::size_t seg = seg_x2 / 2;
  const std::size_t total = 16 + seg * 8;
  if (!in_range(bytes, sub, total)) return unexpected(ErrorCode::Parse, "cmap 格式 4 数组截断");
  CmapTable& cmap = data.cmap;
  cmap.kind = CmapTable::Kind::Segment;
  cmap.end_code.resize(seg);
  cmap.start_code.resize(seg);
  cmap.id_delta.resize(seg);
  cmap.id_range_offset.resize(seg);
  for (std::size_t i = 0; i < seg; ++i) {
    cmap.end_code[i] = read_u16(bytes, sub + 14 + i * 2);
    cmap.start_code[i] = read_u16(bytes, sub + 16 + seg * 2 + i * 2);
    cmap.id_delta[i] = read_u16(bytes, sub + 16 + seg * 4 + i * 2);
    cmap.id_range_offset[i] = read_u16(bytes, sub + 16 + seg * 6 + i * 2);
  }
  cmap.range_base = sub + 16 + seg * 6;
  return ok();
}

[[nodiscard]] auto parse_cmap_format12(FontData& data, std::size_t sub) -> Status {
  const auto bytes = data.span();
  if (!in_range(bytes, sub, 16)) return unexpected(ErrorCode::Parse, "cmap 格式 12 截断");
  const std::uint32_t group_count = read_u32(bytes, sub + 12);
  if (group_count > (bytes.size() / 12)) {
    return unexpected(ErrorCode::Parse, "cmap 格式 12 组数非法");
  }
  const std::size_t total = 16 + static_cast<std::size_t>(group_count) * 12;
  if (!in_range(bytes, sub, total)) return unexpected(ErrorCode::Parse, "cmap 格式 12 数组截断");
  CmapTable& cmap = data.cmap;
  cmap.kind = CmapTable::Kind::Groups;
  cmap.groups.clear();
  cmap.groups.reserve(group_count);
  for (std::uint32_t i = 0; i < group_count; ++i) {
    const std::size_t at = sub + 16 + static_cast<std::size_t>(i) * 12;
    CmapGroup group{};
    group.first = read_u32(bytes, at);
    group.last = read_u32(bytes, at + 4);
    group.start_glyph = read_u32(bytes, at + 8);
    if (group.last < group.first) return unexpected(ErrorCode::Parse, "cmap 格式 12 组区间非法");
    cmap.groups.push_back(group);
  }
  return ok();
}

[[nodiscard]] auto parse_cmap_format6(FontData& data, std::size_t sub) -> Status {
  const auto bytes = data.span();
  if (!in_range(bytes, sub, 10)) return unexpected(ErrorCode::Parse, "cmap 格式 6 截断");
  const std::uint16_t count = read_u16(bytes, sub + 8);
  if (!in_range(bytes, sub + 10, static_cast<std::size_t>(count) * 2)) {
    return unexpected(ErrorCode::Parse, "cmap 格式 6 数组截断");
  }
  CmapTable& cmap = data.cmap;
  cmap.kind = CmapTable::Kind::Trimmed;
  cmap.first_code = read_u16(bytes, sub + 6);
  cmap.codes.resize(count);
  for (std::uint16_t i = 0; i < count; ++i) {
    cmap.codes[i] = read_u16(bytes, sub + 10 + static_cast<std::size_t>(i) * 2);
  }
  return ok();
}

[[nodiscard]] auto parse_cmap_format0(FontData& data, std::size_t sub) -> Status {
  const auto bytes = data.span();
  if (!in_range(bytes, sub + 6, 256)) return unexpected(ErrorCode::Parse, "cmap 格式 0 截断");
  CmapTable& cmap = data.cmap;
  cmap.kind = CmapTable::Kind::Byte;
  for (std::size_t i = 0; i < cmap.byte_map.size(); ++i) {
    cmap.byte_map[i] = read_u8(bytes, sub + 6 + i);
  }
  return ok();
}

[[nodiscard]] auto parse_cmap(FontData& data) -> Status {
  const auto bytes = data.span();
  const TableRecord* record = data.table(tag_of('c', 'm', 'a', 'p'));
  if (record == nullptr || record->length < 4) {
    return unexpected(ErrorCode::Parse, "缺少 cmap 表");
  }
  const std::size_t base = record->offset;
  const std::uint16_t table_count = read_u16(bytes, base + 2);
  if (!in_range(bytes, base + 4, static_cast<std::size_t>(table_count) * 8)) {
    return unexpected(ErrorCode::Parse, "cmap 子表目录截断");
  }
  std::size_t best_offset = 0;
  std::uint16_t best_format = 0;
  int best_score = -1;
  for (std::uint16_t i = 0; i < table_count; ++i) {
    const std::size_t at = base + 4 + static_cast<std::size_t>(i) * 8;
    const std::uint16_t platform = read_u16(bytes, at);
    const std::uint16_t encoding = read_u16(bytes, at + 2);
    const std::size_t sub = base + read_u32(bytes, at + 4);
    if (!in_range(bytes, sub, 2) || sub > bytes.size()) continue;
    const std::uint16_t format = read_u16(bytes, sub);
    const int score = cmap_subtable_score(platform, encoding, format);
    if (score > best_score) {
      best_score = score;
      best_offset = sub;
      best_format = format;
    }
  }
  if (best_score < 0) return unexpected(ErrorCode::Unsupported, "cmap 无受支持子表（4/6/12/0）");
  switch (best_format) {
    case 4: return parse_cmap_format4(data, best_offset);
    case 12: return parse_cmap_format12(data, best_offset);
    case 6: return parse_cmap_format6(data, best_offset);
    case 0: return parse_cmap_format0(data, best_offset);
    default: break;
  }
  return unexpected(ErrorCode::Unsupported, "cmap 子表格式不支持");
}

[[nodiscard]] auto cmap_lookup(const FontData& data, char32_t codepoint) noexcept -> GlyphId {
  const auto bytes = data.span();
  const CmapTable& cmap = data.cmap;
  switch (cmap.kind) {
    case CmapTable::Kind::None: return 0;
    case CmapTable::Kind::Byte: {
      if (codepoint >= 256) return 0;
      return cmap.byte_map[static_cast<std::size_t>(codepoint)];
    }
    case CmapTable::Kind::Trimmed: {
      if (codepoint < cmap.first_code) return 0;
      const std::size_t index = static_cast<std::size_t>(codepoint - cmap.first_code);
      return index < cmap.codes.size() ? cmap.codes[index] : 0;
    }
    case CmapTable::Kind::Groups: {
      const auto it = std::upper_bound(
          cmap.groups.begin(), cmap.groups.end(), static_cast<std::uint32_t>(codepoint),
          [](std::uint32_t value, const CmapGroup& group) noexcept { return value < group.first; });
      if (it == cmap.groups.begin()) return 0;
      const CmapGroup& group = *(it - 1);
      if (static_cast<std::uint32_t>(codepoint) > group.last) return 0;
      return group.start_glyph + (static_cast<std::uint32_t>(codepoint) - group.first);
    }
    case CmapTable::Kind::Segment: {
      if (codepoint > 0xFFFF) return 0;
      const auto code = static_cast<std::uint16_t>(codepoint);
      const auto it = std::lower_bound(cmap.end_code.begin(), cmap.end_code.end(), code);
      if (it == cmap.end_code.end()) return 0;
      const std::size_t seg = static_cast<std::size_t>(it - cmap.end_code.begin());
      if (seg >= cmap.start_code.size() || code < cmap.start_code[seg]) return 0;
      const std::uint16_t delta = cmap.id_delta[seg];
      const std::uint16_t range = cmap.id_range_offset[seg];
      if (range == 0) {
        return static_cast<GlyphId>((static_cast<std::uint32_t>(code) + delta) & 0xFFFFU);
      }
      const std::size_t address = cmap.range_base + seg * 2 + static_cast<std::size_t>(range) +
                                  static_cast<std::size_t>(code - cmap.start_code[seg]) * 2;
      const std::uint16_t glyph = read_u16(bytes, address);
      if (glyph == 0) return 0;
      return static_cast<GlyphId>((static_cast<std::uint32_t>(glyph) + delta) & 0xFFFFU);
    }
  }
  return 0;
}

// —— name ——

[[nodiscard]] auto decode_utf16be(std::span<const std::uint8_t> raw) -> std::string {
  std::string out{};
  std::size_t i = 0;
  while (i + 1 < raw.size()) {
    const auto unit = static_cast<std::uint32_t>((static_cast<std::uint32_t>(raw[i]) << 8U) |
                                                 static_cast<std::uint32_t>(raw[i + 1]));
    i += 2;
    char32_t codepoint = static_cast<char32_t>(unit);
    if (unit >= 0xD800U && unit <= 0xDBFFU && i + 1 < raw.size()) {
      const auto low = static_cast<std::uint32_t>((static_cast<std::uint32_t>(raw[i]) << 8U) |
                                                  static_cast<std::uint32_t>(raw[i + 1]));
      if (low >= 0xDC00U && low <= 0xDFFFU) {
        i += 2;
        codepoint = static_cast<char32_t>(0x10000U + ((unit - 0xD800U) << 10U) + (low - 0xDC00U));
      }
    }
    if (codepoint == 0) continue;
    if (codepoint < 0x20) continue;
    st::encode_utf8(codepoint, out);
  }
  return out;
}

[[nodiscard]] auto decode_latin1(std::span<const std::uint8_t> raw) -> std::string {
  std::string out{};
  for (const std::uint8_t byte : raw) {
    if (byte == 0) continue;
    if (byte < 0x20U) continue;
    st::encode_utf8(static_cast<char32_t>(byte), out);
  }
  return out;
}

[[nodiscard]] auto parse_name(FontData& data) -> Status {
  const auto bytes = data.span();
  const TableRecord* record = data.table(tag_of('n', 'a', 'm', 'e'));
  if (record == nullptr || record->length < 6) return ok();  // 缺 name 表不算加载失败
  const std::size_t base = record->offset;
  const std::uint16_t count = read_u16(bytes, base + 2);
  const std::size_t string_base = base + read_u16(bytes, base + 4);
  if (!in_range(bytes, base + 6, static_cast<std::size_t>(count) * 12)) {
    return ok();
  }
  std::string full_name{};
  std::string family_name{};
  int full_score = 0;
  int family_score = 0;
  for (std::uint16_t i = 0; i < count; ++i) {
    const std::size_t at = base + 6 + static_cast<std::size_t>(i) * 12;
    const std::uint16_t platform = read_u16(bytes, at);
    const std::uint16_t encoding = read_u16(bytes, at + 2);
    const std::uint16_t name_id = read_u16(bytes, at + 6);
    const std::size_t length = read_u16(bytes, at + 8);
    const std::size_t offset = string_base + read_u16(bytes, at + 10);
    if (name_id != 1 && name_id != 4) continue;
    if (!in_range(bytes, offset, length)) continue;
    int score = 0;
    std::string text{};
    if (platform == 3) {
      score = 4;
      text = decode_utf16be(bytes.subspan(offset, length));
    } else if (platform == 0) {
      score = 3;
      text = decode_utf16be(bytes.subspan(offset, length));
    } else if (platform == 1 && encoding == 0) {
      score = 1;
      text = decode_latin1(bytes.subspan(offset, length));
    } else {
      continue;
    }
    if (text.empty()) continue;
    if (name_id == 4 && score >= full_score) {
      full_score = score;
      full_name = text;
    } else if (name_id == 1 && score >= family_score) {
      family_score = score;
      family_name = text;
    }
  }
  data.name = !full_name.empty() ? full_name : family_name;
  return ok();
}

}  // namespace
}  // namespace st::text

// ============================ TrueType 轮廓（glyf/loca）============================

namespace st::text {
namespace {

struct GlyphPoint {
  float x{0.0f};
  float y{0.0f};
  bool on_curve{false};
};

[[nodiscard]] auto point_of(const GlyphPoint& point) noexcept -> math::Point {
  return math::Point{point.x, point.y};
}

[[nodiscard]] auto midpoint(const GlyphPoint& first, const GlyphPoint& second) noexcept
    -> math::Point {
  return math::Point{(first.x + second.x) * 0.5f, (first.y + second.y) * 0.5f};
}

/// 把 TrueType 闭合轮廓（on/off 点混合、隐含中点）转成二次贝塞尔路径。
auto emit_contour(raster::Path& path, std::span<const GlyphPoint> contour) -> void {
  const std::size_t count = contour.size();
  if (count == 0) return;
  std::size_t index = 0;
  if (contour[0].on_curve) {
    path.move_to(point_of(contour[0]));
    index = 1;
  } else if (contour[count - 1].on_curve) {
    path.move_to(point_of(contour[count - 1]));
  } else {
    path.move_to(midpoint(contour[count - 1], contour[0]));
  }
  while (index < count) {
    const GlyphPoint& current = contour[index];
    if (current.on_curve) {
      path.line_to(point_of(current));
      ++index;
      continue;
    }
    const GlyphPoint& next = contour[(index + 1) % count];
    if (next.on_curve) {
      path.quad_to(point_of(current), point_of(next));
      index += 2;
    } else {
      path.quad_to(point_of(current), midpoint(current, next));
      ++index;
    }
  }
  path.close();
}

/// 按仿射矩阵（a b c d / dx dy）变换并追加路径。
auto append_transformed(raster::Path& out, const raster::Path& source, float a, float b, float c,
                        float d, float dx, float dy) -> void {
  const auto map = [a, b, c, d, dx, dy](math::Point point) noexcept -> math::Point {
    return math::Point{a * point.x + c * point.y + dx, b * point.x + d * point.y + dy};
  };
  for (const raster::PathCommand& command : source.commands()) {
    switch (command.kind) {
      case raster::PathCommand::Kind::MoveTo: out.move_to(map(command.p1)); break;
      case raster::PathCommand::Kind::LineTo: out.line_to(map(command.p1)); break;
      case raster::PathCommand::Kind::QuadTo: out.quad_to(map(command.p1), map(command.p2)); break;
      case raster::PathCommand::Kind::CubicTo:
        out.cubic_to(map(command.p1), map(command.p2), map(command.p3));
        break;
      case raster::PathCommand::Kind::Close: out.close(); break;
    }
  }
}

[[nodiscard]] auto glyf_range(const FontData& data, GlyphId id, std::size_t& start,
                              std::size_t& end) -> Status {
  const auto bytes = data.span();
  const GlyfTable& glyf = data.glyf;
  if (!glyf.present) return unexpected(ErrorCode::Unsupported, "字体不含 glyf 轮廓");
  if (data.num_glyphs != 0 && id >= data.num_glyphs) {
    return unexpected(ErrorCode::NotFound, "字形索引越界");
  }
  if (glyf.long_loca) {
    const std::size_t at = glyf.loca_offset + static_cast<std::size_t>(id) * 4;
    if (!in_range(bytes, at, 8)) return unexpected(ErrorCode::Parse, "loca 表越界");
    start = read_u32(bytes, at);
    end = read_u32(bytes, at + 4);
  } else {
    const std::size_t at = glyf.loca_offset + static_cast<std::size_t>(id) * 2;
    if (!in_range(bytes, at, 4)) return unexpected(ErrorCode::Parse, "loca 表越界");
    start = static_cast<std::size_t>(read_u16(bytes, at)) * 2;
    end = static_cast<std::size_t>(read_u16(bytes, at + 2)) * 2;
  }
  if (end < start) return unexpected(ErrorCode::Parse, "loca 偏移非单调");
  if (end > glyf.glyf_length) return unexpected(ErrorCode::Parse, "glyf 记录越界");
  return ok();
}

[[nodiscard]] auto outline_glyf(const FontData& data, GlyphId id, raster::Path& out,
                                int depth) -> Status;

/// TrueType 简单字形（直线 + 二次曲线）。
[[nodiscard]] auto outline_glyf_simple(std::size_t base, std::size_t limit,
                                       std::span<const std::uint8_t> bytes,
                                       std::int16_t contour_count, raster::Path& out) -> Status {
  const std::size_t contours = static_cast<std::size_t>(contour_count);
  if (!in_range(bytes, base + 10, contours * 2 + 2)) {
    return unexpected(ErrorCode::Parse, "glyf 轮廓端点数组截断");
  }
  std::vector<std::uint16_t> ends(contours);
  for (std::size_t i = 0; i < contours; ++i) {
    ends[i] = read_u16(bytes, base + 10 + i * 2);
    if (i > 0 && ends[i] < ends[i - 1]) return unexpected(ErrorCode::Parse, "glyf 轮廓端点非单调");
  }
  if (contours == 0) return ok();
  const std::size_t point_count = static_cast<std::size_t>(ends[contours - 1]) + 1;
  if (point_count == 0 || point_count > 100000) {
    return unexpected(ErrorCode::Parse, "glyf 点数非法");
  }
  std::size_t cursor = base + 10 + contours * 2;
  const std::size_t instructions = read_u16(bytes, cursor);
  cursor += 2;
  if (cursor > limit || instructions > limit - cursor) {
    return unexpected(ErrorCode::Parse, "glyf 指令区越界");
  }
  cursor += instructions;

  std::vector<std::uint8_t> flags(point_count);
  std::size_t filled = 0;
  while (filled < point_count) {
    if (cursor >= limit) return unexpected(ErrorCode::Parse, "glyf 标志数组截断");
    const std::uint8_t flag = bytes[cursor];
    ++cursor;
    flags[filled] = flag;
    ++filled;
    if ((flag & 0x08U) != 0) {  // REPEAT
      if (cursor >= limit) return unexpected(ErrorCode::Parse, "glyf 标志重复计数截断");
      std::uint8_t repeat = bytes[cursor];
      ++cursor;
      while (repeat > 0 && filled < point_count) {
        flags[filled] = flag;
        ++filled;
        --repeat;
      }
    }
  }

  std::vector<GlyphPoint> points(point_count);
  std::int32_t coordinate = 0;
  for (std::size_t i = 0; i < point_count; ++i) {
    const std::uint8_t flag = flags[i];
    if ((flag & 0x02U) != 0) {  // X_SHORT_VECTOR
      if (cursor >= limit) return unexpected(ErrorCode::Parse, "glyf x 坐标截断");
      const auto delta = static_cast<std::int32_t>(bytes[cursor]);
      ++cursor;
      coordinate += (flag & 0x10U) != 0 ? delta : -delta;
    } else if ((flag & 0x10U) == 0) {  // X 同前
      if (cursor + 2 > limit) return unexpected(ErrorCode::Parse, "glyf x 坐标截断");
      coordinate += static_cast<std::int32_t>(read_i16(bytes, cursor));
      cursor += 2;
    }
    points[i].x = static_cast<float>(coordinate);
  }
  coordinate = 0;
  for (std::size_t i = 0; i < point_count; ++i) {
    const std::uint8_t flag = flags[i];
    if ((flag & 0x04U) != 0) {  // Y_SHORT_VECTOR
      if (cursor >= limit) return unexpected(ErrorCode::Parse, "glyf y 坐标截断");
      const auto delta = static_cast<std::int32_t>(bytes[cursor]);
      ++cursor;
      coordinate += (flag & 0x20U) != 0 ? delta : -delta;
    } else if ((flag & 0x20U) == 0) {
      if (cursor + 2 > limit) return unexpected(ErrorCode::Parse, "glyf y 坐标截断");
      coordinate += static_cast<std::int32_t>(read_i16(bytes, cursor));
      cursor += 2;
    }
    points[i].y = static_cast<float>(coordinate);
    points[i].on_curve = (flag & 0x01U) != 0;
  }

  std::size_t first = 0;
  for (std::size_t c = 0; c < contours; ++c) {
    const std::size_t last = ends[c];
    if (last < first || last >= point_count) return unexpected(ErrorCode::Parse, "glyf 轮廓点越界");
    emit_contour(out, std::span<const GlyphPoint>(points).subspan(first, last - first + 1));
    first = last + 1;
  }
  return ok();
}

/// TrueType 复合字形（组件 + 2x2 变换）。
[[nodiscard]] auto outline_glyf_composite(const FontData& data, std::size_t base,
                                          std::size_t limit, std::span<const std::uint8_t> bytes,
                                          raster::Path& out, int depth) -> Status {
  std::size_t cursor = base + 10;
  std::size_t components = 0;
  for (;;) {
    if (++components > k_max_component_count) {
      return unexpected(ErrorCode::Parse, "glyf 复合组件过多");
    }
    if (cursor + 4 > limit) return unexpected(ErrorCode::Parse, "glyf 复合组件头截断");
    const std::uint16_t flags = read_u16(bytes, cursor);
    const GlyphId component_glyph = read_u16(bytes, cursor + 2);
    cursor += 4;
    const bool words = (flags & 0x0001U) != 0;
    const bool xy_values = (flags & 0x0002U) != 0;
    float dx = 0.0f;
    float dy = 0.0f;
    if (words) {
      if (cursor + 4 > limit) return unexpected(ErrorCode::Parse, "glyf 组件参数截断");
      if (xy_values) {
        dx = static_cast<float>(read_i16(bytes, cursor));
        dy = static_cast<float>(read_i16(bytes, cursor + 2));
      }
      cursor += 4;
    } else {
      if (cursor + 2 > limit) return unexpected(ErrorCode::Parse, "glyf 组件参数截断");
      if (xy_values) {
        dx = static_cast<float>(read_i8(bytes, cursor));
        dy = static_cast<float>(read_i8(bytes, cursor + 1));
      }
      cursor += 2;
    }
    float a = 1.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 1.0f;
    if ((flags & 0x0008U) != 0) {
      if (cursor + 2 > limit) return unexpected(ErrorCode::Parse, "glyf 组件缩放截断");
      a = read_f2dot14(bytes, cursor);
      d = a;
      cursor += 2;
    } else if ((flags & 0x0040U) != 0) {
      if (cursor + 4 > limit) return unexpected(ErrorCode::Parse, "glyf 组件缩放截断");
      a = read_f2dot14(bytes, cursor);
      d = read_f2dot14(bytes, cursor + 2);
      cursor += 4;
    } else if ((flags & 0x0080U) != 0) {
      if (cursor + 8 > limit) return unexpected(ErrorCode::Parse, "glyf 组件矩阵截断");
      a = read_f2dot14(bytes, cursor);
      b = read_f2dot14(bytes, cursor + 2);
      c = read_f2dot14(bytes, cursor + 4);
      d = read_f2dot14(bytes, cursor + 6);
      cursor += 8;
    }
    if (xy_values) {
      raster::Path component{};
      const Status status = outline_glyf(data, component_glyph, component, depth + 1);
      if (!status) return status;
      append_transformed(out, component, a, b, c, d, dx, dy);
    }
    // 点位对齐参数（ARGS_ARE_XY_VALUES 未置位）极罕见：跳过该组件（见文档「受限之处」）。
    if ((flags & 0x0020U) == 0) break;
  }
  return ok();
}

[[nodiscard]] auto outline_glyf(const FontData& data, GlyphId id, raster::Path& out,
                                int depth) -> Status {
  if (depth > k_max_composite_depth) {
    return unexpected(ErrorCode::Parse, "glyf 复合字形嵌套过深");
  }
  std::size_t start = 0;
  std::size_t end = 0;
  const Status range = glyf_range(data, id, start, end);
  if (!range) return range;
  if (end == start) return ok();  // 空字形（空格等）
  const auto bytes = data.span();
  const std::size_t base = data.glyf.glyf_offset + start;
  const std::size_t limit = data.glyf.glyf_offset + end;
  if (!in_range(bytes, base, 10) || limit > bytes.size() || base > limit) {
    return unexpected(ErrorCode::Parse, "glyf 记录过短");
  }
  const std::int16_t contour_count = read_i16(bytes, base);
  if (contour_count >= 0) {
    return outline_glyf_simple(base, limit, bytes, contour_count, out);
  }
  return outline_glyf_composite(data, base, limit, bytes, out, depth);
}

[[nodiscard]] auto parse_glyf(FontData& data) -> Status {
  const TableRecord* glyf = data.table(tag_of('g', 'l', 'y', 'f'));
  const TableRecord* loca = data.table(tag_of('l', 'o', 'c', 'a'));
  if (glyf == nullptr || loca == nullptr) {
    if (data.table(tag_of('C', 'F', 'F', ' ')) != nullptr) return ok();  // CFF 字体无 glyf/loca
    return unexpected(ErrorCode::Unsupported, "字体既无 glyf/loca 也无 CFF 轮廓表");
  }
  const std::size_t entry_size = data.glyf.long_loca ? 4 : 2;
  const std::size_t needed =
      (static_cast<std::size_t>(data.num_glyphs) + 1) * entry_size;
  if (loca->length < needed) return unexpected(ErrorCode::Parse, "loca 表过短");
  data.glyf.glyf_offset = glyf->offset;
  data.glyf.glyf_length = glyf->length;
  data.glyf.loca_offset = loca->offset;
  data.glyf.present = true;
  return ok();
}

// —— kern（格式 0 水平字距）——

[[nodiscard]] auto parse_kern(FontData& data) -> Status {
  const auto bytes = data.span();
  const TableRecord* record = data.table(tag_of('k', 'e', 'r', 'n'));
  if (record == nullptr || record->length < 4) return ok();
  const std::size_t base = record->offset;
  const std::size_t limit = base + record->length;
  if (read_u16(bytes, base) != 0) return ok();  // 仅支持 OpenType 版本 0（Apple 版本跳过）
  const std::uint16_t table_count = read_u16(bytes, base + 2);
  std::size_t cursor = base + 4;
  for (std::uint16_t i = 0; i < table_count; ++i) {
    if (cursor + 6 > limit) break;
    const std::uint16_t sub_version = read_u16(bytes, cursor);
    const std::uint16_t length = read_u16(bytes, cursor + 2);
    const std::uint16_t coverage = read_u16(bytes, cursor + 4);
    if (length < 6 || cursor + length > limit) break;
    const bool horizontal = (coverage & 0x0001U) != 0;
    const bool minimum = (coverage & 0x0002U) != 0;
    const bool cross_stream = (coverage & 0x0004U) != 0;
    const auto format = static_cast<std::uint8_t>((coverage >> 8U) & 0x00FFU);
    if (format == 0 && horizontal && !minimum && !cross_stream && sub_version == 0) {
      const std::size_t pairs_offset = cursor + 6;
      if (pairs_offset + 8 <= limit) {
        const std::size_t pair_count = read_u16(bytes, pairs_offset);
        const std::size_t pairs_base = pairs_offset + 8;
        if (pairs_base + pair_count * 6 <= limit) {
          data.kern.reserve(data.kern.size() + pair_count);
          for (std::size_t p = 0; p < pair_count; ++p) {
            const std::size_t at = pairs_base + p * 6;
            const auto left = static_cast<std::uint32_t>(read_u16(bytes, at));
            const auto right = static_cast<std::uint32_t>(read_u16(bytes, at + 2));
            const std::int16_t value = read_i16(bytes, at + 4);
            const std::uint32_t key = (left << 16U) | right;
            if (data.kern.find(key) == data.kern.end()) data.kern.emplace(key, value);
          }
        }
      }
    }
    cursor += length;
  }
  return ok();
}

}  // namespace
}  // namespace st::text

// ============================ CFF / Type2 charstring ============================

namespace st::text {
namespace {

[[nodiscard]] auto to_i32_clamped(double value) noexcept -> std::int32_t {
  if (value <= -2147483000.0) return -2147483000;
  if (value >= 2147483000.0) return 2147483000;
  return static_cast<std::int32_t>(value);
}

/// 解析 CFF INDEX（`offsets` 末项为 INDEX 结束偏移，全部相对 CFF 表起点）。
[[nodiscard]] auto parse_cff_index(std::span<const std::uint8_t> cff, std::size_t offset,
                                   std::size_t& next, CffIndex& out) -> Status {
  out.offsets.clear();
  if (!in_range(cff, offset, 2)) return unexpected(ErrorCode::Parse, "CFF INDEX 头越界");
  const std::size_t count = read_u16(cff, offset);
  if (count == 0) {
    next = offset + 2;
    return ok();
  }
  if (!in_range(cff, offset, 3)) return unexpected(ErrorCode::Parse, "CFF INDEX 头截断");
  const std::size_t off_size = read_u8(cff, offset + 2);
  if (off_size < 1 || off_size > 4) return unexpected(ErrorCode::Parse, "CFF INDEX 偏移宽度非法");
  const std::size_t widths = (count + 1) * off_size;
  if (!in_range(cff, offset + 3, widths)) {
    return unexpected(ErrorCode::Parse, "CFF INDEX 偏移数组截断");
  }
  const std::size_t data_start = offset + 3 + widths;
  if (data_start > cff.size()) return unexpected(ErrorCode::Parse, "CFF INDEX 数据越界");
  const std::size_t available = cff.size() - data_start;
  out.offsets.resize(count + 1);
  for (std::size_t i = 0; i <= count; ++i) {
    out.offsets[i] = read_uint_be(cff, offset + 3 + i * off_size, off_size);
    if (i > 0 && out.offsets[i] < out.offsets[i - 1]) {
      return unexpected(ErrorCode::Parse, "CFF INDEX 偏移非单调");
    }
  }
  if (out.offsets[0] != 1) return unexpected(ErrorCode::Parse, "CFF INDEX 首偏移非 1");
  const std::size_t last = out.offsets[count];
  if (last - 1 > available) return unexpected(ErrorCode::Parse, "CFF INDEX 数据越界");
  for (std::size_t i = 0; i <= count; ++i) {
    out.offsets[i] = data_start + out.offsets[i] - 1;
  }
  next = data_start + last - 1;
  return ok();
}

/// 解析 CFF DICT 操作符序列（Top DICT / Private DICT / Font DICT 通用）。
[[nodiscard]] auto parse_dict_operators(std::span<const std::uint8_t> dict,
                                        std::vector<DictOp>& out) -> Status {
  std::vector<double> operands{};
  std::size_t i = 0;
  while (i < dict.size()) {
    const std::uint8_t first = dict[i];
    if (first <= 21) {
      DictOp entry{};
      if (first == 12) {
        if (i + 1 >= dict.size()) return unexpected(ErrorCode::Parse, "CFF DICT 扩展操作符截断");
        entry.op = 12;
        entry.ext = dict[i + 1];
        i += 2;
      } else {
        entry.op = first;
        i += 1;
      }
      entry.args = operands;
      out.push_back(std::move(entry));
      operands.clear();
      continue;
    }
    if (first == 28) {
      if (i + 3 > dict.size()) return unexpected(ErrorCode::Parse, "CFF DICT 整数截断");
      operands.push_back(static_cast<double>(read_i16(dict, i + 1)));
      i += 3;
      continue;
    }
    if (first == 29) {
      if (i + 5 > dict.size()) return unexpected(ErrorCode::Parse, "CFF DICT 整数截断");
      operands.push_back(static_cast<double>(read_i32(dict, i + 1)));
      i += 5;
      continue;
    }
    if (first == 30) {
      std::string digits{};
      std::size_t cursor = i + 1;
      bool finished = false;
      while (!finished) {
        if (cursor >= dict.size()) return unexpected(ErrorCode::Parse, "CFF DICT 实数截断");
        const std::uint8_t byte = dict[cursor];
        ++cursor;
        for (int half = 0; half < 2; ++half) {
          const auto nibble = static_cast<std::uint8_t>(half == 0 ? (byte >> 4U) & 0x0FU : byte & 0x0FU);
          if (nibble <= 9U) {
            digits.push_back(static_cast<char>(static_cast<int>('0') + static_cast<int>(nibble)));
          } else if (nibble == 0x0AU) {
            digits.push_back('.');
          } else if (nibble == 0x0BU) {
            digits.push_back('E');
          } else if (nibble == 0x0CU) {
            digits.append("E-");
          } else if (nibble == 0x0EU) {
            digits.push_back('-');
          } else if (nibble == 0x0FU) {
            finished = true;
            break;
          }
        }
      }
      i = cursor;
      const std::optional<double> value = st::parse_f64(digits);
      operands.push_back(value.has_value() ? *value : 0.0);
      continue;
    }
    if (first <= 246) {
      operands.push_back(static_cast<double>(first) - 139.0);
      i += 1;
      continue;
    }
    if (first <= 250) {
      if (i + 2 > dict.size()) return unexpected(ErrorCode::Parse, "CFF DICT 正整数截断");
      operands.push_back((static_cast<double>(first) - 247.0) * 256.0 +
                         static_cast<double>(dict[i + 1]) + 108.0);
      i += 2;
      continue;
    }
    if (first <= 254) {
      if (i + 2 > dict.size()) return unexpected(ErrorCode::Parse, "CFF DICT 负整数截断");
      operands.push_back(-((static_cast<double>(first) - 251.0) * 256.0 +
                           static_cast<double>(dict[i + 1]) + 108.0));
      i += 2;
      continue;
    }
    // 255：16.16 定点（CFF2 DICT 用；CFF1 中不出现）
    if (i + 5 > dict.size()) return unexpected(ErrorCode::Parse, "CFF DICT 定点数截断");
    operands.push_back(static_cast<double>(read_i32(dict, i + 1)) / 65536.0);
    i += 5;
    if (operands.size() > static_cast<std::size_t>(k_max_dict_stack)) {
      return unexpected(ErrorCode::Parse, "CFF DICT 操作数过多");
    }
  }
  return ok();
}

[[nodiscard]] auto dict_number(const std::vector<DictOp>& ops, int op, int ext) noexcept
    -> std::optional<double> {
  for (const DictOp& entry : ops) {
    if (entry.op == op && entry.ext == ext && !entry.args.empty()) return entry.args[0];
  }
  return std::nullopt;
}

[[nodiscard]] auto dict_pair(const std::vector<DictOp>& ops, int op, int ext) noexcept
    -> std::optional<std::pair<double, double>> {
  for (const DictOp& entry : ops) {
    if (entry.op == op && entry.ext == ext && entry.args.size() >= 2) {
      return std::pair<double, double>{entry.args[0], entry.args[1]};
    }
  }
  return std::nullopt;
}

/// 解析 Private DICT（含局部 Subrs INDEX）。
[[nodiscard]] auto parse_cff_private(std::span<const std::uint8_t> cff, std::size_t size,
                                     std::size_t offset, CffPrivate& out) -> Status {
  if (size == 0) return ok();
  if (!in_range(cff, offset, size)) return unexpected(ErrorCode::Parse, "CFF Private DICT 越界");
  std::vector<DictOp> ops{};
  const Status status = parse_dict_operators(cff.subspan(offset, size), ops);
  if (!status) return status;
  for (const DictOp& entry : ops) {
    if (entry.op == 20 && !entry.args.empty()) {
      out.default_width_x = to_i32_clamped(entry.args[0]);
    } else if (entry.op == 21 && !entry.args.empty()) {
      out.nominal_width_x = to_i32_clamped(entry.args[0]);
    } else if (entry.op == 19 && !entry.args.empty()) {
      const double relative = entry.args[0];
      if (relative < 0.0) return unexpected(ErrorCode::Parse, "CFF Subrs 偏移为负");
      const auto subrs_at = offset + static_cast<std::size_t>(relative);
      std::size_t next = 0;
      const Status sub_status = parse_cff_index(cff, subrs_at, next, out.subrs);
      if (!sub_status) return sub_status;
    }
  }
  return ok();
}

/// FDSelect：字形 → Font DICT 序号（格式 0 / 3）。
[[nodiscard]] auto parse_fdselect(std::span<const std::uint8_t> cff, std::size_t offset,
                                  std::size_t glyph_count, CffTable& table) -> Status {
  if (offset >= cff.size()) return unexpected(ErrorCode::Parse, "CFF FDSelect 越界");
  const std::uint8_t format = read_u8(cff, offset);
  if (format == 0) {
    if (!in_range(cff, offset + 1, glyph_count)) {
      return unexpected(ErrorCode::Parse, "CFF FDSelect 逐字形数组截断");
    }
    table.fd_by_glyph.assign(glyph_count, std::uint8_t{0});
    for (std::size_t i = 0; i < glyph_count; ++i) {
      table.fd_by_glyph[i] = read_u8(cff, offset + 1 + i);
    }
    return ok();
  }
  if (format == 3) {
    if (!in_range(cff, offset + 3, 2)) return unexpected(ErrorCode::Parse, "CFF FDSelect 截断");
    const std::size_t range_count = read_u16(cff, offset + 1);
    if (!in_range(cff, offset + 3, (range_count + 1) * 3)) {
      return unexpected(ErrorCode::Parse, "CFF FDSelect 区间数组截断");
    }
    table.fd_ranges.clear();
    table.fd_ranges.reserve(range_count);
    for (std::size_t i = 0; i < range_count; ++i) {
      CffTable::FdRange range{};
      range.first = read_u16(cff, offset + 3 + i * 3);
      range.fd = read_u8(cff, offset + 3 + i * 3 + 2);
      table.fd_ranges.push_back(range);
    }
    return ok();
  }
  return unexpected(ErrorCode::Unsupported, "CFF FDSelect 格式不支持");
}

/// 惰性解析 CFF 表（首次取字形轮廓时触发；`FontFace` 拷贝共享结果）。
[[nodiscard]] auto parse_cff_table(FontData& data) -> Status {
  const auto bytes = data.span();
  const TableRecord* record = data.table(tag_of('C', 'F', 'F', ' '));
  if (record == nullptr) return unexpected(ErrorCode::Unsupported, "字体不含 CFF 表");
  if (record->length < 4) return unexpected(ErrorCode::Parse, "CFF 表过短");
  CffTable& table = data.cff;
  table.base = record->offset;
  table.length = record->length;
  const std::span<const std::uint8_t> cff = bytes.subspan(record->offset, record->length);
  const std::size_t header_size = read_u8(cff, 2);
  if (header_size < 4) return unexpected(ErrorCode::Parse, "CFF 头长度非法");
  std::size_t cursor = header_size;
  CffIndex name_index{};
  CffIndex top_dicts{};
  CffIndex string_index{};
  Status status = parse_cff_index(cff, cursor, cursor, name_index);
  if (!status) return status;
  status = parse_cff_index(cff, cursor, cursor, top_dicts);
  if (!status) return status;
  if (top_dicts.count() != 1) return unexpected(ErrorCode::Parse, "CFF TopDICT 数量非 1");
  status = parse_cff_index(cff, cursor, cursor, string_index);
  if (!status) return status;
  status = parse_cff_index(cff, cursor, cursor, table.global_subrs);
  if (!status) return status;

  std::vector<DictOp> ops{};
  status = parse_dict_operators(top_dicts.item(cff, 0), ops);
  if (!status) return status;
  const std::optional<double> charstrings_at = dict_number(ops, 17, 0);
  if (!charstrings_at.has_value() || *charstrings_at < 0.0) {
    return unexpected(ErrorCode::Parse, "CFF 缺 CharStrings");
  }
  std::size_t charstrings_end = 0;
  status = parse_cff_index(cff, static_cast<std::size_t>(*charstrings_at), charstrings_end,
                           table.charstrings);
  if (!status) return status;
  if (table.charstrings.count() == 0) return unexpected(ErrorCode::Parse, "CFF CharStrings 为空");
  table.cid_keyed = dict_number(ops, 12, 30).has_value();  // ROS

  const std::optional<double> fd_array = dict_number(ops, 12, 36);
  const std::optional<double> fd_select = dict_number(ops, 12, 37);
  const std::optional<std::pair<double, double>> private_dict = dict_pair(ops, 18, 0);
  if (fd_array.has_value() && *fd_array >= 0.0) {
    CffIndex font_dicts{};
    std::size_t fd_end = 0;
    status = parse_cff_index(cff, static_cast<std::size_t>(*fd_array), fd_end, font_dicts);
    if (!status) return status;
    if (font_dicts.count() == 0) return unexpected(ErrorCode::Parse, "CFF FDArray 为空");
    table.privates.clear();
    table.privates.reserve(font_dicts.count());
    for (std::size_t i = 0; i < font_dicts.count(); ++i) {
      std::vector<DictOp> font_ops{};
      status = parse_dict_operators(font_dicts.item(cff, i), font_ops);
      if (!status) return status;
      CffPrivate entry{};
      const std::optional<std::pair<double, double>> dict = dict_pair(font_ops, 18, 0);
      if (dict.has_value()) {
        status = parse_cff_private(cff, static_cast<std::size_t>(dict->first),
                                   static_cast<std::size_t>(dict->second), entry);
        if (!status) return status;
      }
      table.privates.push_back(std::move(entry));
    }
  } else {
    CffPrivate entry{};
    if (private_dict.has_value()) {
      status = parse_cff_private(cff, static_cast<std::size_t>(private_dict->first),
                                 static_cast<std::size_t>(private_dict->second), entry);
      if (!status) return status;
    }
    table.privates.push_back(std::move(entry));
  }
  if (fd_select.has_value() && *fd_select >= 0.0) {
    status = parse_fdselect(cff, static_cast<std::size_t>(*fd_select), table.charstrings.count(),
                            table);
    if (!status) return status;
  }
  if (data.num_glyphs == 0) {
    data.num_glyphs = static_cast<std::uint32_t>(table.charstrings.count());
    data.metrics.glyph_count = data.num_glyphs;
  }
  table.parsed = true;
  return ok();
}

[[nodiscard]] auto ensure_cff(FontData& data) -> Status {
  const std::scoped_lock lock(data.cff_mutex);
  if (data.cff.parsed) return ok();
  return parse_cff_table(data);
}

// —— Type2 charstring 解释器 ——

struct Type2State {
  std::span<const std::uint8_t> cff{};
  const CffTable* table{nullptr};
  const CffPrivate* priv{nullptr};
  std::size_t fd{0};
  std::size_t local_bias{0};
  std::size_t global_bias{0};
  std::array<float, 32> transient{};
  std::vector<float> stack{};
  raster::Path path{};
  float x{0.0f};
  float y{0.0f};
  float width{0.0f};
  int stems{0};
  bool width_seen{false};
  /// 笔画提示的收集口（nullptr = 不收集，仅出轮廓）。
  /// 与 `path` 在同一次 charstring 解释中产出——**不重复解释**，也就不存在两次结果不一致。
  std::vector<StemHint>* hints{nullptr};
};

[[nodiscard]] auto cff_bias(std::size_t count) noexcept -> std::size_t {
  if (count < 1240) return 107;
  if (count < 33900) return 1131;
  return 32768;
}

[[nodiscard]] auto type2_push(Type2State& state, float value) -> Status {
  if (state.stack.size() >= k_max_type2_stack) {
    return unexpected(ErrorCode::Parse, "CFF 数值栈溢出");
  }
  state.stack.push_back(value);
  return ok();
}

[[nodiscard]] auto type2_pop(Type2State& state, float& out) -> Status {
  if (state.stack.empty()) return unexpected(ErrorCode::Parse, "CFF 数值栈下溢");
  out = state.stack.back();
  state.stack.pop_back();
  return ok();
}

/// 首个清栈操作符可能携带额外宽度参数（Type2 宽度约定）。
auto type2_take_width(Type2State& state, std::size_t expected) -> void {
  if (state.width_seen) return;
  state.width_seen = true;
  if (state.stack.size() > expected && !state.stack.empty()) {
    state.width = static_cast<float>(state.priv->nominal_width_x) + state.stack.front();
    state.stack.erase(state.stack.begin());
  }
}

auto type2_take_stems(Type2State& state, bool vertical) -> void {
  if (!state.width_seen) {
    state.width_seen = true;
    if (state.stack.size() % 2 != 0 && !state.stack.empty()) {
      state.width = static_cast<float>(state.priv->nominal_width_x) + state.stack.front();
      state.stack.erase(state.stack.begin());
    }
  }
  // **记录笔画提示**（`vstem`/`hstem` 的操作数是"成对的位置与宽度"）。
  //
  // 位置相对**当前点 y**（Type2 规格：stem hints 的坐标基于 rmoveto 之前的临时原点），
  // 宽度可为负（表示 bottom/top 顺序相反），因此统一取 min/max 归一。
  //
  // 为什么要它们：CFF charstring **不存标准宽度**（与 Type1 不同），宽度信息只存在于
  // stem hints 里；而按轮廓几何反推笔画实测召回只有 ~19%，于是同一字里只有一部分笔画
  // 被吸附、另一部分留着分数相位——那正是"线条粗细不均匀"的来源。
  if (state.hints != nullptr) {
    const std::size_t count = state.stack.size() / 2;
    for (std::size_t k = 0; k < count; ++k) {
      const float a = state.stack[k * 2];
      const float b = state.stack[k * 2 + 1];
      state.hints->push_back(StemHint{state.y + std::min(a, a + b), state.y + std::max(a, a + b),
                                      vertical});
    }
  }
  state.stems += static_cast<int>(state.stack.size() / 2);
  state.stack.clear();
}

auto type2_line(Type2State& state, float dx, float dy) -> void {
  state.x += dx;
  state.y += dy;
  state.path.line_to(math::Point{state.x, state.y});
}

auto type2_curve(Type2State& state, float dx1, float dy1, float dx2, float dy2, float dx3,
                 float dy3) -> void {
  const math::Point control1{state.x + dx1, state.y + dy1};
  const math::Point control2{state.x + dx1 + dx2, state.y + dy1 + dy2};
  const math::Point end{state.x + dx1 + dx2 + dx3, state.y + dy1 + dy2 + dy3};
  state.path.cubic_to(control1, control2, end);
  state.x = end.x;
  state.y = end.y;
}

/// Type2 子程序定位：**操作数是"编号 − bias"，故可为负**（bias 由该 FD 的子程序数量决定：
/// <1240 → 107、<33900 → 1131、否则 32768）。这里必须先加 bias 再作边界检查——
/// 直接把负操作数判为越界会让所有"低编号子程序"字形（CJK 里占大多数）全部丢失。
[[nodiscard]] auto type2_subr_code(Type2State& state, int raw_index, bool global)
    -> std::optional<std::span<const std::uint8_t>> {
  const CffIndex& index = global ? state.table->global_subrs : state.priv->subrs;
  const auto bias = static_cast<std::ptrdiff_t>(global ? state.global_bias : state.local_bias);
  const std::ptrdiff_t position = static_cast<std::ptrdiff_t>(raw_index) + bias;
  if (position < 0) return std::nullopt;
  const auto slot = static_cast<std::size_t>(position);
  if (slot + 1 >= index.offsets.size()) return std::nullopt;
  return state.cff.subspan(index.offsets[slot], index.offsets[slot + 1] - index.offsets[slot]);
}

[[nodiscard]] auto type2_exec(Type2State& state, std::span<const std::uint8_t> code, int depth)
    -> Status;

/// Type2 扩展操作符（前缀 12）。
[[nodiscard]] auto type2_extended(Type2State& state, std::uint8_t ext) -> Status {
  const auto size = [&state]() noexcept { return state.stack.size(); };
  switch (ext) {
    case 0: case 1: case 2:  // dotsection / vstem3 / hstem3（已废弃，按提示算子处理）
      type2_take_stems(state, ext == 1);
      return ok();
    case 3: case 4: {  // and / or
      float b = 0.0f;
      float a = 0.0f;
      Status status = type2_pop(state, b);
      if (!status) return status;
      status = type2_pop(state, a);
      if (!status) return status;
      const bool left = a != 0.0f;
      const bool right = b != 0.0f;
      return type2_push(state, (ext == 3 ? (left && right) : (left || right)) ? 1.0f : 0.0f);
    }
    case 5: {  // not
      float a = 0.0f;
      const Status status = type2_pop(state, a);
      if (!status) return status;
      return type2_push(state, a == 0.0f ? 1.0f : 0.0f);
    }
    case 9: {  // abs
      float a = 0.0f;
      const Status status = type2_pop(state, a);
      if (!status) return status;
      return type2_push(state, a < 0.0f ? -a : a);
    }
    case 10: case 11: case 12: case 24: {  // add / sub / div / mul
      float b = 0.0f;
      float a = 0.0f;
      Status status = type2_pop(state, b);
      if (!status) return status;
      status = type2_pop(state, a);
      if (!status) return status;
      if (ext == 10) return type2_push(state, a + b);
      if (ext == 11) return type2_push(state, a - b);
      if (ext == 24) return type2_push(state, a * b);
      if (b == 0.0f) return unexpected(ErrorCode::Parse, "CFF div 除零");
      return type2_push(state, a / b);
    }
    case 14: {  // neg
      float a = 0.0f;
      const Status status = type2_pop(state, a);
      if (!status) return status;
      return type2_push(state, -a);
    }
    case 15: {  // eq
      float b = 0.0f;
      float a = 0.0f;
      Status status = type2_pop(state, b);
      if (!status) return status;
      status = type2_pop(state, a);
      if (!status) return status;
      return type2_push(state, a == b ? 1.0f : 0.0f);
    }
    case 18: {  // drop
      float value = 0.0f;
      return type2_pop(state, value);
    }
    case 20: {  // put
      float index_value = 0.0f;
      float value = 0.0f;
      Status status = type2_pop(state, index_value);
      if (!status) return status;
      status = type2_pop(state, value);
      if (!status) return status;
      const int slot = to_int_clamped(index_value);
      if (slot < 0 || static_cast<std::size_t>(slot) >= state.transient.size()) {
        return unexpected(ErrorCode::Parse, "CFF transient 数组越界");
      }
      state.transient[static_cast<std::size_t>(slot)] = value;
      return ok();
    }
    case 21: {  // get
      float index_value = 0.0f;
      const Status status = type2_pop(state, index_value);
      if (!status) return status;
      const int slot = to_int_clamped(index_value);
      if (slot < 0 || static_cast<std::size_t>(slot) >= state.transient.size()) {
        return unexpected(ErrorCode::Parse, "CFF transient 数组越界");
      }
      return type2_push(state, state.transient[static_cast<std::size_t>(slot)]);
    }
    case 22: {  // ifelse
      float v2 = 0.0f;
      float v1 = 0.0f;
      float s2 = 0.0f;
      float s1 = 0.0f;
      Status status = type2_pop(state, v2);
      if (!status) return status;
      status = type2_pop(state, v1);
      if (!status) return status;
      status = type2_pop(state, s2);
      if (!status) return status;
      status = type2_pop(state, s1);
      if (!status) return status;
      return type2_push(state, v1 <= v2 ? s1 : s2);
    }
    case 23:  // random（确定性实现：固定 0.5，保证同一字体渲染可复现）
      return type2_push(state, 0.5f);
    case 26: {  // sqrt
      float a = 0.0f;
      const Status status = type2_pop(state, a);
      if (!status) return status;
      return type2_push(state, a <= 0.0f ? 0.0f : static_cast<float>(std::sqrt(a)));
    }
    case 27: {  // dup
      if (state.stack.empty()) return unexpected(ErrorCode::Parse, "CFF dup 栈为空");
      return type2_push(state, state.stack.back());
    }
    case 28: {  // exch
      if (size() < 2) return unexpected(ErrorCode::Parse, "CFF exch 栈不足");
      const std::size_t top = state.stack.size() - 1;
      const float swap = state.stack[top];
      state.stack[top] = state.stack[top - 1];
      state.stack[top - 1] = swap;
      return ok();
    }
    case 29: {  // index
      float index_value = 0.0f;
      const Status status = type2_pop(state, index_value);
      if (!status) return status;
      const int offset = to_int_clamped(index_value);
      if (offset < 0 || static_cast<std::size_t>(offset) >= state.stack.size()) {
        return unexpected(ErrorCode::Parse, "CFF index 越界");
      }
      return type2_push(state, state.stack[state.stack.size() - 1 - static_cast<std::size_t>(offset)]);
    }
    case 30: {  // roll
      float shift_value = 0.0f;
      float count_value = 0.0f;
      Status status = type2_pop(state, shift_value);
      if (!status) return status;
      status = type2_pop(state, count_value);
      if (!status) return status;
      const int count = to_int_clamped(count_value);
      if (count <= 0 || static_cast<std::size_t>(count) > state.stack.size()) {
        return unexpected(ErrorCode::Parse, "CFF roll 参数非法");
      }
      const std::size_t total = static_cast<std::size_t>(count);
      int shift = to_int_clamped(shift_value) % count;
      if (shift < 0) shift += count;
      std::rotate(state.stack.end() - static_cast<std::ptrdiff_t>(total), state.stack.end() - shift,
                  state.stack.end());
      return ok();
    }
    case 34: {  // hflex
      if (size() < 7) return unexpected(ErrorCode::Parse, "CFF hflex 参数不足");
      const float dy2 = state.stack[2];
      type2_curve(state, state.stack[0], 0.0f, state.stack[1], state.stack[2], state.stack[3], 0.0f);
      type2_curve(state, state.stack[4], 0.0f, state.stack[5], -dy2, state.stack[6], 0.0f);
      state.stack.clear();
      return ok();
    }
    case 35: {  // flex
      if (size() < 13) return unexpected(ErrorCode::Parse, "CFF flex 参数不足");
      type2_curve(state, state.stack[0], state.stack[1], state.stack[2], state.stack[3],
                  state.stack[4], state.stack[5]);
      type2_curve(state, state.stack[6], state.stack[7], state.stack[8], state.stack[9],
                  state.stack[10], state.stack[11]);
      state.stack.clear();
      return ok();
    }
    case 36: {  // hflex1
      if (size() < 9) return unexpected(ErrorCode::Parse, "CFF hflex1 参数不足");
      const float dy6 = -(state.stack[1] + state.stack[3] + state.stack[7]);
      type2_curve(state, state.stack[0], state.stack[1], state.stack[2], state.stack[3],
                  state.stack[4], 0.0f);
      type2_curve(state, state.stack[5], 0.0f, state.stack[6], state.stack[7], state.stack[8], dy6);
      state.stack.clear();
      return ok();
    }
    case 37: {  // flex1
      if (size() < 11) return unexpected(ErrorCode::Parse, "CFF flex1 参数不足");
      const float dx = state.stack[0] + state.stack[2] + state.stack[4] + state.stack[6] + state.stack[8];
      const float dy = state.stack[1] + state.stack[3] + state.stack[5] + state.stack[7] + state.stack[9];
      const float dx6 = std::abs(dx) > std::abs(dy) ? state.stack[10] : 0.0f;
      const float dy6 = std::abs(dx) > std::abs(dy) ? 0.0f : state.stack[10];
      type2_curve(state, state.stack[0], state.stack[1], state.stack[2], state.stack[3],
                  state.stack[4], state.stack[5]);
      type2_curve(state, state.stack[6], state.stack[7], state.stack[8], state.stack[9], dx6, dy6);
      state.stack.clear();
      return ok();
    }
    default: break;
  }
  return unexpected(ErrorCode::Unsupported, "不支持的 CFF 扩展操作符");
}

[[nodiscard]] auto type2_exec(Type2State& state, std::span<const std::uint8_t> code, int depth)
    -> Status {
  if (depth > k_max_subr_depth) return unexpected(ErrorCode::Parse, "CFF 子程序嵌套过深");
  std::size_t i = 0;
  while (i < code.size()) {
    const std::uint8_t op = code[i];
    if (op == 28) {
      if (i + 3 > code.size()) return unexpected(ErrorCode::Parse, "CFF 数值截断");
      const Status status = type2_push(state, static_cast<float>(read_i16(code, i + 1)));
      if (!status) return status;
      i += 3;
      continue;
    }
    if (op >= 32) {
      Status status = ok();
      if (op <= 246) {
        status = type2_push(state, static_cast<float>(static_cast<int>(op) - 139));
        i += 1;
      } else if (op <= 250) {
        if (i + 2 > code.size()) return unexpected(ErrorCode::Parse, "CFF 数值截断");
        status = type2_push(state, static_cast<float>((static_cast<int>(op) - 247) * 256 +
                                                      static_cast<int>(code[i + 1]) + 108));
        i += 2;
      } else if (op <= 254) {
        if (i + 2 > code.size()) return unexpected(ErrorCode::Parse, "CFF 数值截断");
        status = type2_push(state, static_cast<float>(-(static_cast<int>(op) - 251) * 256 -
                                                      static_cast<int>(code[i + 1]) - 108));
        i += 2;
      } else {
        if (i + 5 > code.size()) return unexpected(ErrorCode::Parse, "CFF 定点数截断");
        status = type2_push(state, static_cast<float>(read_i32(code, i + 1)) / 65536.0f);
        i += 5;
      }
      if (!status) return status;
      continue;
    }
    ++i;  // 消费主操作符字节
    switch (op) {
      case 1:
      case 3:
      case 18:
      case 23:  // hstem / vstem / hstemhm / vstemhm
        // 方向：`vstem`(3) 与 `vstemhm`(23) 是竖笔画；`hstem`(1) 与 `hstemhm`(18) 是横笔画。
        type2_take_stems(state, op == 3 || op == 23);
        break;
      case 19:
      case 20: {  // hintmask / cntrmask
        type2_take_stems(state, false);
        const std::size_t mask_bytes = (static_cast<std::size_t>(state.stems) + 7) / 8;
        if (i + mask_bytes > code.size()) return unexpected(ErrorCode::Parse, "CFF hintmask 截断");
        i += mask_bytes;
        break;
      }
      case 21: {  // rmoveto
        type2_take_width(state, 2);
        if (state.stack.size() < 2) return unexpected(ErrorCode::Parse, "CFF rmoveto 参数不足");
        const float dx = state.stack[0];
        const float dy = state.stack[1];
        state.stack.clear();
        state.x += dx;
        state.y += dy;
        state.path.move_to(math::Point{state.x, state.y});
        break;
      }
      case 22: {  // hmoveto
        type2_take_width(state, 1);
        if (state.stack.empty()) return unexpected(ErrorCode::Parse, "CFF hmoveto 参数不足");
        state.x += state.stack[0];
        state.stack.clear();
        state.path.move_to(math::Point{state.x, state.y});
        break;
      }
      case 4: {  // vmoveto
        type2_take_width(state, 1);
        if (state.stack.empty()) return unexpected(ErrorCode::Parse, "CFF vmoveto 参数不足");
        state.y += state.stack[0];
        state.stack.clear();
        state.path.move_to(math::Point{state.x, state.y});
        break;
      }
      case 5: {  // rlineto
        if (state.stack.size() % 2 != 0) return unexpected(ErrorCode::Parse, "CFF rlineto 参数非法");
        for (std::size_t k = 0; k + 1 < state.stack.size(); k += 2) {
          type2_line(state, state.stack[k], state.stack[k + 1]);
        }
        state.stack.clear();
        break;
      }
      case 6:
      case 7: {  // hlineto / vlineto（交替方向）
        bool horizontal = op == 6;
        for (const float value : state.stack) {
          if (horizontal) {
            type2_line(state, value, 0.0f);
          } else {
            type2_line(state, 0.0f, value);
          }
          horizontal = !horizontal;
        }
        state.stack.clear();
        break;
      }
      case 8: {  // rrcurveto
        if (state.stack.size() % 6 != 0) return unexpected(ErrorCode::Parse, "CFF rrcurveto 参数非法");
        for (std::size_t k = 0; k + 5 < state.stack.size(); k += 6) {
          type2_curve(state, state.stack[k], state.stack[k + 1], state.stack[k + 2],
                      state.stack[k + 3], state.stack[k + 4], state.stack[k + 5]);
        }
        state.stack.clear();
        break;
      }
      case 24: {  // rcurveline
        const std::size_t total = state.stack.size();
        if (total < 8 || (total - 2) % 6 != 0) {
          return unexpected(ErrorCode::Parse, "CFF rcurveline 参数非法");
        }
        for (std::size_t k = 0; k + 5 < total - 2; k += 6) {
          type2_curve(state, state.stack[k], state.stack[k + 1], state.stack[k + 2],
                      state.stack[k + 3], state.stack[k + 4], state.stack[k + 5]);
        }
        type2_line(state, state.stack[total - 2], state.stack[total - 1]);
        state.stack.clear();
        break;
      }
      case 25: {  // rlinecurve
        const std::size_t total = state.stack.size();
        if (total < 8 || (total - 6) % 2 != 0) {
          return unexpected(ErrorCode::Parse, "CFF rlinecurve 参数非法");
        }
        for (std::size_t k = 0; k + 1 < total - 6; k += 2) {
          type2_line(state, state.stack[k], state.stack[k + 1]);
        }
        type2_curve(state, state.stack[total - 6], state.stack[total - 5], state.stack[total - 4],
                    state.stack[total - 3], state.stack[total - 2], state.stack[total - 1]);
        state.stack.clear();
        break;
      }
      case 26: {  // vvcurveto
        std::size_t k = 0;
        float dx1 = 0.0f;
        if (state.stack.size() % 4 == 1) {
          dx1 = state.stack[0];
          k = 1;
        }
        while (k + 3 < state.stack.size()) {
          type2_curve(state, dx1, state.stack[k], state.stack[k + 1], state.stack[k + 2], 0.0f,
                      state.stack[k + 3]);
          dx1 = 0.0f;
          k += 4;
        }
        state.stack.clear();
        break;
      }
      case 27: {  // hhcurveto
        std::size_t k = 0;
        float dy1 = 0.0f;
        if (state.stack.size() % 4 == 1) {
          dy1 = state.stack[0];
          k = 1;
        }
        while (k + 3 < state.stack.size()) {
          type2_curve(state, state.stack[k], dy1, state.stack[k + 1], state.stack[k + 2],
                      state.stack[k + 3], 0.0f);
          dy1 = 0.0f;
          k += 4;
        }
        state.stack.clear();
        break;
      }
      case 30:
      case 31: {  // vhcurveto / hvcurveto（交替起止方向，末段可带 1 个补充参数）
        bool horizontal = op == 31;
        std::size_t k = 0;
        while (state.stack.size() >= k + 4) {
          float dx1 = 0.0f;
          float dy1 = 0.0f;
          float dx2 = 0.0f;
          float dy2 = 0.0f;
          float dx3 = 0.0f;
          float dy3 = 0.0f;
          if (horizontal) {
            dx1 = state.stack[k];
            dx2 = state.stack[k + 1];
            dy2 = state.stack[k + 2];
            dy3 = state.stack[k + 3];
          } else {
            dy1 = state.stack[k];
            dx2 = state.stack[k + 1];
            dy2 = state.stack[k + 2];
            dx3 = state.stack[k + 3];
          }
          k += 4;
          if (state.stack.size() == k + 1) {
            if (horizontal) {
              dx3 = state.stack[k];
            } else {
              dy3 = state.stack[k];
            }
            ++k;
          }
          type2_curve(state, dx1, dy1, dx2, dy2, dx3, dy3);
          horizontal = !horizontal;
        }
        state.stack.clear();
        break;
      }
      case 10:
      case 29: {  // callsubr / callgsubr
        float raw = 0.0f;
        const Status status = type2_pop(state, raw);
        if (!status) return status;
        const bool global = op == 29;
        const std::optional<std::span<const std::uint8_t>> sub =
            type2_subr_code(state, to_int_clamped(raw), global);
        if (!sub.has_value()) {
          return unexpected(ErrorCode::Parse,
                            std::format("CFF 子程序索引越界（raw={} global={} fd={}）",
                                        to_int_clamped(raw), global, state.fd));
        }
        const Status call_status = type2_exec(state, *sub, depth + 1);
        if (!call_status) return call_status;
        break;
      }
      case 11:  // return
        return ok();
      case 14: {  // endchar
        type2_take_width(state, 0);
        if (state.stack.size() >= 4) {
          return unexpected(ErrorCode::Unsupported, "CFF seac 组合字形未支持");
        }
        state.stack.clear();
        return ok();
      }
      case 12: {  // 扩展操作符
        if (i >= code.size()) return unexpected(ErrorCode::Parse, "CFF 扩展操作符截断");
        const std::uint8_t ext = code[i];
        ++i;
        const Status status = type2_extended(state, ext);
        if (!status) return status;
        break;
      }
      default:
        return unexpected(ErrorCode::Unsupported, "不支持的 CFF 操作符");
    }
  }
  return ok();
}

[[nodiscard]] auto outline_cff(FontData& data, GlyphId id, raster::Path& out,
                               std::vector<StemHint>* hints = nullptr) -> Status {
  const Status ready = ensure_cff(data);
  if (!ready) return ready;
  const CffTable& table = data.cff;
  if (static_cast<std::size_t>(id) >= table.charstrings.count()) {
    return unexpected(ErrorCode::NotFound, "字形索引越界");
  }
  const std::span<const std::uint8_t> cff = data.span().subspan(table.base, table.length);
  const std::span<const std::uint8_t> code = table.charstrings.item(cff, id);
  if (code.empty()) return ok();
  const std::size_t fd = table.fd_for(id);
  const CffPrivate fallback{};
  const CffPrivate* priv = fd < table.privates.size() ? &table.privates[fd] : &fallback;
  Type2State state{};
  state.cff = cff;
  state.table = &table;
  state.priv = priv;
  state.fd = fd;
  state.local_bias = cff_bias(priv->subrs.count());
  state.global_bias = cff_bias(table.global_subrs.count());
  state.hints = hints;
  const Status status = type2_exec(state, code, 0);
  if (!status) return status;
  out = std::move(state.path);
  return ok();
}

}  // namespace
}  // namespace st::text

// ============================ 公开 API ============================

namespace st::text {
namespace {

/// 默认构造（未加载）face 的零值兜底（`const` 函数局部静态，非可变全局状态）。
[[nodiscard]] auto empty_metrics() noexcept -> const FontMetrics& {
  static const FontMetrics instance{};
  return instance;
}

[[nodiscard]] auto empty_text() noexcept -> const std::string& {
  static const std::string instance{};
  return instance;
}

}  // namespace

/// 字体文件包含的 face 数量（非集合字体为 1）。供 `FontStack` 探测 TTC/OTC 用。
[[nodiscard]] auto font_face_count(std::string_view path) -> Result<std::uint32_t> {
  auto content = st::fs::read_bytes(path);
  if (!content) return st::forward_error(content.error());
  if (content->size() < 12) return unexpected(ErrorCode::Parse, "字体文件过短");
  const std::span<const std::uint8_t> bytes(*content);
  if (read_u32(bytes, 0) == tag_of('t', 't', 'c', 'f')) {
    const std::uint32_t count = read_u32(bytes, 8);
    if (count == 0 || count > k_max_face_count) {
      return unexpected(ErrorCode::Parse, "集合字体 face 数非法");
    }
    if (!in_range(bytes, 12, static_cast<std::size_t>(count) * 4)) {
      return unexpected(ErrorCode::Parse, "集合字体 face 目录截断");
    }
    return count;
  }
  return std::uint32_t{1};
}

auto FontFace::load(std::string_view path, int face_index) -> Result<FontFace> {
  if (face_index < 0) return unexpected(ErrorCode::Invalid, "face_index 不能为负");
  auto content = st::fs::read_bytes(path);
  if (!content) return st::forward_error(content.error());
  if (content->empty()) return unexpected(ErrorCode::Parse, "字体文件为空");
  if (content->size() > k_max_font_bytes) {
    return unexpected(ErrorCode::Invalid, "字体文件过大（>256MB）");
  }
  auto holder = std::make_shared<Data>();
  FontData& data = holder->value;
  data.path = std::string(path);
  data.face_index = face_index;
  data.bytes = std::move(*content);

  const auto bytes = data.span();
  if (bytes.size() < 12) return unexpected(ErrorCode::Parse, "字体文件过短");
  std::size_t face_offset = 0;
  if (read_u32(bytes, 0) == tag_of('t', 't', 'c', 'f')) {
    const std::uint32_t count = read_u32(bytes, 8);
    if (count == 0 || count > k_max_face_count) {
      return unexpected(ErrorCode::Parse, "集合字体 face 数非法");
    }
    if (!in_range(bytes, 12, static_cast<std::size_t>(count) * 4)) {
      return unexpected(ErrorCode::Parse, "集合字体 face 目录截断");
    }
    if (static_cast<std::uint32_t>(face_index) >= count) {
      return unexpected(ErrorCode::NotFound, "face 序号越界");
    }
    face_offset = read_u32(bytes, 12 + static_cast<std::size_t>(face_index) * 4);
    if (face_offset + 12 > bytes.size()) return unexpected(ErrorCode::Parse, "face 偏移越界");
  } else if (face_index != 0) {
    return unexpected(ErrorCode::NotFound, "非集合字体只支持 face 0");
  }

  Status status = parse_sfnt_directory(data, face_offset);
  if (!status) return st::forward_error(status.error());
  status = parse_head_hhea(data);
  if (!status) return st::forward_error(status.error());
  status = parse_cmap(data);
  if (!status) return st::forward_error(status.error());
  status = parse_name(data);
  if (!status) return st::forward_error(status.error());
  status = parse_glyf(data);
  if (!status) return st::forward_error(status.error());
  status = parse_kern(data);
  if (!status) return st::forward_error(status.error());

  FontFace face{};
  face.data_ = std::move(holder);
  return face;
}

auto FontFace::glyph_index(char32_t codepoint) const -> Result<GlyphId> {
  if (data_ == nullptr) return unexpected(ErrorCode::Invalid, "FontFace 未加载");
  const GlyphId id = cmap_lookup(data_->value, codepoint);
  if (id == 0) return unexpected(ErrorCode::NotFound, "字体不包含该码点");
  return id;
}

auto FontFace::glyph(GlyphId id) const -> Result<Glyph> {
  if (data_ == nullptr) return unexpected(ErrorCode::Invalid, "FontFace 未加载");
  const FontData& data = data_->value;
  if (data.num_glyphs != 0 && id >= data.num_glyphs) {
    return unexpected(ErrorCode::NotFound, "字形索引越界");
  }
  Glyph result{};
  result.id = id;
  result.advance = static_cast<float>(hmtx_advance(data, id));
  result.bearing_x = static_cast<float>(hmtx_bearing(data, id));
  const auto outline = glyph_outline(id);
  if (!outline) return st::forward_error(outline.error());
  const math::Rect bounds = outline->bounds();
  result.bearing_y = bounds.y + bounds.height;
  result.empty = outline->is_empty();
  return result;
}

auto FontFace::glyph_for(char32_t codepoint) const -> Result<Glyph> {
  const auto id = glyph_index(codepoint);
  if (!id) return st::forward_error(id.error());
  return glyph(*id);
}

auto FontFace::has_glyph(char32_t codepoint) const -> bool {
  if (data_ == nullptr) return false;
  return cmap_lookup(data_->value, codepoint) != 0;
}

auto FontFace::metrics() const noexcept -> const FontMetrics& {
  if (data_ == nullptr) return empty_metrics();
  return data_->value.metrics;
}

auto FontFace::name() const -> const std::string& {
  if (data_ == nullptr) return empty_text();
  return data_->value.name;
}

auto FontFace::path() const -> const std::string& {
  if (data_ == nullptr) return empty_text();
  return data_->value.path;
}

auto FontFace::is_cff() const noexcept -> bool {
  if (data_ == nullptr) return false;
  const FontData& data = data_->value;
  return !data.glyf.present && data.table(tag_of('C', 'F', 'F', ' ')) != nullptr;
}

auto FontFace::face_index() const noexcept -> int {
  return data_ == nullptr ? 0 : data_->value.face_index;
}

auto FontFace::glyph_outline(GlyphId id) const -> Result<raster::Path> {
  if (data_ == nullptr) return unexpected(ErrorCode::Invalid, "FontFace 未加载");
  FontData& data = data_->value;
  {
    const std::scoped_lock lock(data.outline_mutex);
    const auto it = data.outlines.find(id);
    if (it != data.outlines.end()) return *it->second;
  }
  raster::Path path{};
  Status status = ok();
  if (data.glyf.present) {
    status = outline_glyf(data, id, path, 0);
  } else if (data.table(tag_of('C', 'F', 'F', ' ')) != nullptr) {
    status = outline_cff(data, id, path);
  } else {
    return unexpected(ErrorCode::Unsupported, "字体无可解析的轮廓表（glyf/CFF）");
  }
  if (!status) return st::forward_error(status.error());
  auto cached = std::make_shared<const raster::Path>(std::move(path));
  {
    const std::scoped_lock lock(data.outline_mutex);
    data.outlines.insert_or_assign(id, cached);
  }
  return *cached;
}

auto FontFace::kerning(GlyphId left, GlyphId right) const noexcept -> float {
  if (data_ == nullptr) return 0.0f;
  const std::uint32_t key = ((left & 0xFFFFU) << 16U) | (right & 0xFFFFU);
  const auto it = data_->value.kern.find(key);
  if (it == data_->value.kern.end()) return 0.0f;
  return static_cast<float>(it->second);
}

auto FontFace::stem_hints(GlyphId id) const -> const std::vector<StemHint>& {
  static const std::vector<StemHint> kEmpty{};
  if (data_ == nullptr) return kEmpty;
  FontData& data = data_->value;
  {
    const std::scoped_lock lock(data.hints_mutex);
    const auto it = data.hints.find(id);
    if (it != data.hints.end()) return it->second;
  }
  // 只在 CFF 字上有提示（`glyf` 真型字体走另一条 hinting 路线，本引擎不实现）。
  std::vector<StemHint> collected{};
  if (!data.glyf.present && data.table(tag_of('C', 'F', 'F', ' ')) != nullptr) {
    raster::Path ignored{};
    (void)outline_cff(data, id, ignored, &collected);
  }
  const std::scoped_lock lock(data.hints_mutex);
  return data.hints.emplace(id, std::move(collected)).first->second;
}

}  // namespace st::text
