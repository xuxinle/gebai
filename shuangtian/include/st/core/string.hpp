#pragma once

/// 字符串与 UTF-8 工具（`std::string` / `std::string_view` 语义，禁止 `char*` 运算）。
/// UTF-8 一律按码点处理，切片以码点为单位（UI 文本布局依赖该语义）。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st {

/// 单个 UTF-8 码点及其字节长度。
struct Codepoint {
  char32_t value{0};
  std::uint8_t bytes{0};  ///< 1..4；非法序列返回 1（按替换字符处理）
};

[[nodiscard]] auto is_ascii(std::string_view text) noexcept -> bool;
[[nodiscard]] auto ascii_lower(std::string_view text) -> std::string;
[[nodiscard]] auto ascii_upper(std::string_view text) -> std::string;
[[nodiscard]] auto ascii_iequals(std::string_view a, std::string_view b) noexcept -> bool;
[[nodiscard]] auto ascii_starts_with_ci(std::string_view text, std::string_view prefix) noexcept -> bool;

/// 大小写归一（ASCII 快路径）：纯 ASCII 输入等价 `ascii_lower/upper`；含非 ASCII 时
/// 其余字节原样保留（UI 场景的 key/扩展名归一用，不做 Unicode 语义折叠）。
[[nodiscard]] auto lower(std::string_view text) -> std::string;
[[nodiscard]] auto upper(std::string_view text) -> std::string;

[[nodiscard]] auto trim(std::string_view text) -> std::string_view;
[[nodiscard]] auto trim_start(std::string_view text) -> std::string_view;
[[nodiscard]] auto trim_end(std::string_view text) -> std::string_view;

/// 按分隔符切分（保留空段；不分配字符串，返回视图）。
[[nodiscard]] auto split(std::string_view text, char sep) -> std::vector<std::string_view>;
[[nodiscard]] auto split_str(std::string_view text, std::string_view sep) -> std::vector<std::string_view>;
/// 按任意空白切分并丢弃空段（命令行/清单解析用）。
[[nodiscard]] auto split_whitespace(std::string_view text) -> std::vector<std::string_view>;
[[nodiscard]] auto join(const std::vector<std::string_view>& parts, std::string_view sep) -> std::string;
[[nodiscard]] auto replace_all(std::string_view text, std::string_view from, std::string_view to) -> std::string;

[[nodiscard]] auto parse_i64(std::string_view text) -> std::optional<std::int64_t>;
[[nodiscard]] auto parse_u64(std::string_view text) -> std::optional<std::uint64_t>;
[[nodiscard]] auto parse_f64(std::string_view text) -> std::optional<double>;
[[nodiscard]] auto parse_bool(std::string_view text) -> std::optional<bool>;

/// 数字 → 文本（避免 std::format 的本地化差异，固定 C locale 语义）。
[[nodiscard]] auto to_string(std::int64_t value) -> std::string;
[[nodiscard]] auto to_string(std::uint64_t value) -> std::string;
[[nodiscard]] auto to_string(double value) -> std::string;

// —— UTF-8 ——

/// 从 `index` 处解码一个码点并前进（非法字节按单字节替换处理，不抛错）。
[[nodiscard]] auto decode_utf8(std::string_view text, std::size_t& index) -> Codepoint;
auto encode_utf8(char32_t codepoint, std::string& out) -> void;
[[nodiscard]] auto utf8_encode(std::u32string_view codepoints) -> std::string;
[[nodiscard]] auto utf8_decode(std::string_view text) -> std::u32string;
/// 码点数量（越界字节计入）。
[[nodiscard]] auto utf8_length(std::string_view text) noexcept -> std::size_t;
/// 按码点切片：[begin, begin+count) 的字节视图（越界自动夹取）。
[[nodiscard]] auto utf8_slice(std::string_view text, std::size_t begin, std::size_t count) -> std::string_view;
/// 第 n 个码点的字节偏移（n == 长度时返回字节长度）。
[[nodiscard]] auto utf8_offset(std::string_view text, std::size_t index) noexcept -> std::size_t;
[[nodiscard]] auto utf8_is_valid(std::string_view text) noexcept -> bool;

[[nodiscard]] constexpr auto is_space_codepoint(char32_t value) noexcept -> bool {
  return value == U' ' || value == U'\t' || value == U'\n' || value == U'\r' || value == U'\f' || value == U'\v';
}

/// 把任意文本转义为可安全单行打印的形式（控制字符转 `\xNN`）。
[[nodiscard]] auto escape_control(std::string_view text) -> std::string;

}  // namespace st
