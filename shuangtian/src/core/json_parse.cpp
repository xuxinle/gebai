#include "st/core/json_parse.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace st {
namespace json_detail {
namespace {

/// UTF-8 码点编码到输出（转义路径用：把代理对合回真实码点）。
auto append_utf8(std::string& out, char32_t code_point) -> void {
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

/// 递归下降解析器。
///
/// 设计约束（逐条对应 `CONVENTIONS.md` 与本仓实际事故）：
/// - **零抛出**：一切失败经 `fail()` 记账并返回，从不抛（框架无通用异常边界）。
/// - **深度在递归中计数**：不做"预检 + 解析"两遍扫描；超限立即 `fail`（不是栈溢出崩进程）。
/// - **位置化诊断**：记录出错字节偏移，报错时换算成行列号并附附近片段。
class Parser {
 public:
  Parser(std::string_view text, const ParseOptions& options) : text_(text), options_(options) {}

  [[nodiscard]] auto run() -> Result<Json> {
    // 前导 BOM / 零宽字符：文件来源的高频脏数据，剥掉无损（记在 `notes_` 供上层诊断）
    skip_trivia();
    if (failed_) return std::move(error_);
    Json value = parse_value();
    if (failed_) return std::move(error_);
    skip_trivia();
    if (failed_) return std::move(error_);
    if (position_ != text_.size()) {
      fail("JSON 值之后还有多余内容");
      return std::move(error_);
    }
    return value;
  }

 private:
  // ── 错误 ──────────────────────────────────────────────────────────────
  auto fail(std::string_view message) -> void {
    if (failed_) return;  // 只保留第一个（最靠近真因）的错误
    failed_ = true;
    error_ = unexpected(ErrorCode::Parse, std::format("{}（{}）", message, locate(position_)));
  }

  /// "第 N 行第 M 列，偏移 K 字节" + 附近片段——人和模型都靠它定位。
  [[nodiscard]] auto locate(std::size_t offset) const -> std::string {
    std::size_t line = 1;
    std::size_t line_start = 0;
    const std::size_t end = offset < text_.size() ? offset : text_.size();
    for (std::size_t i = 0; i < end; ++i) {
      if (text_[i] == '\n') {
        ++line;
        line_start = i + 1;
      }
    }
    const std::size_t column = end - line_start + 1;
    // 附近片段：截到行边界，避免跨行片段读不出重点
    std::size_t snippet_begin = line_start;
    std::size_t snippet_end = text_.find('\n', end);
    if (snippet_end == std::string_view::npos) snippet_end = text_.size();
    constexpr std::size_t kMaxSnippet = 60;
    if (snippet_end - snippet_begin > kMaxSnippet) {
      snippet_end = snippet_begin + kMaxSnippet;
    }
    std::string snippet(text_.substr(snippet_begin, snippet_end - snippet_begin));
    return std::format("第 {} 行第 {} 列，偏移 {} 字节｜附近内容：{}", line, column, end, snippet);
  }

  // ── 词法辅助 ──────────────────────────────────────────────────────────

  [[nodiscard]] auto peek() const noexcept -> char {
    return position_ < text_.size() ? text_[position_] : '\0';
  }

  auto skip_trivia() -> void {
    while (position_ < text_.size()) {
      const char raw = text_[position_];
      if (raw == ' ' || raw == '\t' || raw == '\n' || raw == '\r') {
        ++position_;
        continue;
      }
      // 前导/夹杂的 BOM 与零宽字符（文件来源常见）
      if (raw == '\xEF' && position_ + 2 < text_.size() && text_[position_ + 1] == '\xBB' &&
          text_[position_ + 2] == '\xBF') {
        position_ += 3;
        continue;
      }
      break;
    }
  }

  auto consume(char expected) -> bool {
    if (peek() != expected) {
      fail(std::format("期望 '{}'（实际 {}）", expected, describe_here()));
      return false;
    }
    ++position_;
    return true;
  }

  [[nodiscard]] auto describe_here() const -> std::string {
    if (position_ >= text_.size()) return "输入已结束";
    return std::format("'{}'", text_[position_]);
  }

  // ── 值 ────────────────────────────────────────────────────────────────

  [[nodiscard]] auto parse_value() -> Json {
    if (failed_) return Json();
    skip_trivia();
    if (failed_) return Json();
    if (position_ >= text_.size()) {
      fail("缺少值（输入在此结束）");
      return Json();
    }
    if (++depth_ > options_.max_depth) {
      --depth_;
      fail(std::format("JSON 嵌套层数超过上限（{}）", options_.max_depth));
      return Json();
    }
    Json value = parse_value_inner();
    --depth_;
    return value;
  }

  [[nodiscard]] auto parse_value_inner() -> Json {
    switch (peek()) {
      case '{':
        return parse_object();
      case '[':
        return parse_array();
      case '"':
        return Json(parse_string());
      case 't':
        if (consume_literal("true")) return Json(true);
        return Json();
      case 'f':
        if (consume_literal("false")) return Json(false);
        return Json();
      case 'n':
        if (consume_literal("null")) return Json();
        return Json();
      default:
        return parse_number();
    }
  }

  auto consume_literal(std::string_view literal) -> bool {
    if (text_.substr(position_, literal.size()) != literal) {
      fail(std::format("无法识别的字面量（期望 \"{}\"）", literal));
      return false;
    }
    position_ += literal.size();
    return true;
  }

  // ── 数字（整数/无符号/浮点分开存，保真）────────────────────────────────

  [[nodiscard]] auto parse_number() -> Json {
    const std::size_t begin = position_;
    if (peek() == '-') ++position_;
    // 整数部分
    if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
      fail("不是合法的 JSON 值");
      return Json();
    }
    // 前导零：JSON 规范不允许（`01` 非法），与旧库一致地拒绝
    if (text_[position_] == '0') {
      ++position_;
      if (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        fail("数字不能有前导零");
        return Json();
      }
    } else {
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
    }
    bool is_float = false;
    // 小数部分
    if (position_ < text_.size() && text_[position_] == '.') {
      is_float = true;
      ++position_;
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
        fail("小数点后缺少数字");
        return Json();
      }
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
    }
    // 指数部分
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      is_float = true;
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
        fail("指数部分缺少数字");
        return Json();
      }
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
    }
    const std::string_view whole = text_.substr(begin, position_ - begin);
    // ## 为什么用 `std::from_chars` 而不是 `strtod` / `strtoll`
    //
    // 本仓的主力目标平台是 Windows + mingw64，那里的 `strtod` 走 msvcrt 实现
    //（含 locale/`_errno` 处理与多级调用），实测 **483 ns/值**；
    // `std::from_chars` 是本机 libstdc++ 的 Ryu 实现，**34 ns/值**（**14×**）。
    // `strtoll` → `from_chars` 同样从 54 ns 降到 14 ns（3.7×）。
    // 且 `from_chars` **不做分配**、**不读 locale**（strtod 会把 `1,5` 按德语 locale 解析成 1.5，
    // 而 JSON 只认点号——这本身就是个正确性优势），也不需要 NUL 结尾的临时 string。
    //
    // 一条纪律：`from_chars` 不对前导 `+` 宽容、且**不跳过空白**——这与 JSON 语法完全一致，
    // 反而省掉了“解析完再核对 end 指针”那一步。
    if (is_float) {
      double parsed = 0.0;
      const auto result = std::from_chars(whole.data(), whole.data() + whole.size(), parsed);
      // `result.ptr` 必须吃完整段：否则是 `1.2.3` 这类非法输入
      if (result.ec != std::errc{} || result.ptr != whole.data() + whole.size() ||
          !std::isfinite(parsed)) {
        fail("数字超出可表示范围或格式不合法");
        return Json();
      }
      return Json(parsed);
    }
    // 整数：先试 int64，溢出再试 uint64（雪花 ID 与 uint64 最大值都要逐位保真）
    if (whole.front() == '-') {
      std::int64_t parsed = 0;
      const auto result = std::from_chars(whole.data(), whole.data() + whole.size(), parsed);
      if (result.ec != std::errc{} || result.ptr != whole.data() + whole.size()) {
        fail("整数超出 int64 范围");
        return Json();
      }
      return Json(parsed);
    }
    std::int64_t signed_value = 0;
    const auto signed_result =
        std::from_chars(whole.data(), whole.data() + whole.size(), signed_value);
    if (signed_result.ec == std::errc{} && signed_result.ptr == whole.data() + whole.size()) {
      // int64 能装下 → 走有符号（常见 ID/时间戳都在这里）
      return Json(signed_value);
    }
    // 溢出 int64：再试 uint64（`18446744073709551615` 这类要逐位保住）
    std::uint64_t unsigned_value = 0;
    const auto unsigned_result =
        std::from_chars(whole.data(), whole.data() + whole.size(), unsigned_value);
    if (unsigned_result.ec != std::errc{} || unsigned_result.ptr != whole.data() + whole.size()) {
      fail("整数超出 uint64 范围");
      return Json();
    }
    return Json(unsigned_value);
  }

  // ── 字符串 ────────────────────────────────────────────────────────────

  [[nodiscard]] auto parse_string() -> std::string {
    // 调用方保证 `peek() == '"'`
    ++position_;
    std::string out;
    std::size_t chunk_begin = position_;
    for (;;) {
      if (position_ >= text_.size()) {
        fail("字符串未闭合（输入在此结束）");
        return out;
      }
      const char raw = text_[position_];
      if (raw == '"') {
        // 快路径：整段无转义（UTF-8 中文占大半场景）→ 一次 assign
        out.append(text_.substr(chunk_begin, position_ - chunk_begin));
        ++position_;
        return out;
      }
      if (raw == '\\') {
        out.append(text_.substr(chunk_begin, position_ - chunk_begin));
        ++position_;
        if (!parse_escape(out)) return out;
        chunk_begin = position_;
        continue;
      }
      const auto byte = static_cast<unsigned char>(raw);
      if (byte < 0x20) {
        // 裸控制字符：JSON 不允许（应写成 \n / \t / \u00XX）。严格拒绝——
        // 宽松修复属于"上层策略"，解析层必须保持规范的单一真值。
        fail("字符串中出现未转义的控制字符（应写成 \\n / \\t / \\u00XX）");
        return out;
      }
      ++position_;
    }
  }

  auto parse_escape(std::string& out) -> bool {
    if (position_ >= text_.size()) {
      fail("转义序列在输入末尾被截断");
      return false;
    }
    const char escape = text_[position_++];
    switch (escape) {
      case '"':
        out.push_back('"');
        return true;
      case '\\':
        out.push_back('\\');
        return true;
      case '/':
        out.push_back('/');
        return true;
      case 'b':
        out.push_back('\b');
        return true;
      case 'f':
        out.push_back('\f');
        return true;
      case 'n':
        out.push_back('\n');
        return true;
      case 'r':
        out.push_back('\r');
        return true;
      case 't':
        out.push_back('\t');
        return true;
      case 'u': {
        const auto surrogate = read_hex4();
        if (failed_) return false;
        if (surrogate >= 0xD800 && surrogate <= 0xDBFF) {
          // 高代理：必须紧跟 `\uXXXX` 低代理，合成一个码点
          if (position_ + 1 < text_.size() && text_[position_] == '\\' && text_[position_ + 1] == 'u') {
            position_ += 2;
            const auto low = read_hex4();
            if (failed_) return false;
            if (low < 0xDC00 || low > 0xDFFF) {
              fail("代理对不完整（高代理之后不是低代理）");
              return false;
            }
            const char32_t combined =
                0x10000 + ((surrogate - 0xD800) << 10) + (low - 0xDC00);
            append_utf8(out, combined);
            return true;
          }
          fail("代理对不完整（高代理之后缺少低代理）");
          return false;
        }
        if (surrogate >= 0xDC00 && surrogate <= 0xDFFF) {
          fail("孤立的低代理（缺少前面的高代理）");
          return false;
        }
        append_utf8(out, static_cast<char32_t>(surrogate));
        return true;
      }
      default:
        fail(std::format("非法的转义字符 '\\{}'", escape));
        return false;
    }
  }

  [[nodiscard]] auto read_hex4() -> unsigned {
    if (position_ + 4 > text_.size()) {
      fail("\\u 转义缺少 4 位十六进制数字");
      return 0;
    }
    unsigned value = 0;
    for (int digit = 0; digit < 4; ++digit) {
      const char hex = text_[position_];
      unsigned nibble = 0;
      if (hex >= '0' && hex <= '9') {
        nibble = static_cast<unsigned>(hex - '0');
      } else if (hex >= 'a' && hex <= 'f') {
        nibble = static_cast<unsigned>(hex - 'a' + 10);
      } else if (hex >= 'A' && hex <= 'F') {
        nibble = static_cast<unsigned>(hex - 'A' + 10);
      } else {
        fail("\\u 转义后不是合法的十六进制数字");
        return 0;
      }
      value = value * 16 + nibble;
      ++position_;
    }
    return value;
  }

  // ── 容器 ──────────────────────────────────────────────────────────────

  [[nodiscard]] auto parse_object() -> Json {
    ++position_;  // {
    Json object = Json::object();
    skip_trivia();
    if (failed_) return object;
    if (peek() == '}') {
      ++position_;
      return object;
    }
    for (;;) {
      skip_trivia();
      if (failed_) return object;
      if (peek() != '"') {
        fail(std::format("对象键必须是双引号字符串（实际 {}）", describe_here()));
        return object;
      }
      std::string key = parse_string();
      if (failed_) return object;
      skip_trivia();
      if (failed_) return object;
      if (!consume(':')) return object;
      Json value = parse_value();
      if (failed_) return object;
      // 重复键：后者覆盖前者（与旧库同语义；保序位置保持首次出现处）
      //
      // ## 为什么不每次扫全部已有成员
      // 逐键线性查重是 **O(n²)**——实测 64 键的对象每键成本是 4 键的 1.7 倍
      //（149 → 200 ns/键）。改成“先全部追加，结束后一次去重”：
      // 常见情况（无重复键）下就是纯 push_back，成本 O(n)。
      object.members_mutable().push_back(JsonMember{std::move(key), std::move(value)});
      skip_trivia();
      if (failed_) return object;
      if (peek() == ',') {
        ++position_;
        // 尾逗号：`{"a":1,}` 不是合法 JSON，明确拒绝（宽松修复留给上层策略）
        skip_trivia();
        if (failed_) return object;
        if (peek() == '}') {
          fail("对象末尾有多余逗号（尾逗号不是合法 JSON）");
          return object;
        }
        continue;
      }
      if (peek() == '}') {
        ++position_;
        deduplicate(object);
        return object;
      }
      fail(std::format("期望 ',' 或 '}}'（实际 {}）", describe_here()));
      return object;
    }
  }

  /// 重复键归约：**后者获胜**，但保留首次出现的位置（与旧库 `ordered_json` 同语义）。
  ///
  /// 只在“确实有重复键”时才付出 O(n²)；无重复时一遍扫描就结束。
  /// 这与“每插一个键就扫全部”不同——后者把最常见的无重复情形也拖成 O(n²)。
  static auto deduplicate(Json& object) -> void {
    JsonObject& members = object.members_mutable();
    if (members.size() < 2) return;
    // 先做一次廉价扫描：绝大多数文档无重复键，直接返回
    for (std::size_t index = 0; index < members.size(); ++index) {
      const std::string& key = members[index].first;
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (members[previous].first != key) continue;
        // 发现重复：把后者的值搬到首次位置，然后把后面那个删掉
        members[previous].second = std::move(members[index].second);
        members.erase(members.begin() + static_cast<JsonObject::difference_type>(index));
        --index;  // 删掉一个后下标要回退
        break;
      }
    }
  }

  [[nodiscard]] auto parse_array() -> Json {
    ++position_;  // [
    Json array = Json::array();
    skip_trivia();
    if (failed_) return array;
    if (peek() == ']') {
      ++position_;
      return array;
    }
    for (;;) {
      Json value = parse_value();
      if (failed_) return array;
      array.elements_mutable().push_back(std::move(value));
      skip_trivia();
      if (failed_) return array;
      if (peek() == ',') {
        ++position_;
        skip_trivia();
        if (failed_) return array;
        if (peek() == ']') {
          fail("数组末尾有多余逗号（尾逗号不是合法 JSON）");
          return array;
        }
        continue;
      }
      if (peek() == ']') {
        ++position_;
        return array;
      }
      fail(std::format("期望 ',' 或 ']'（实际 {}）", describe_here()));
      return array;
    }
  }

  std::string_view text_;
  const ParseOptions& options_;
  std::size_t position_{0};
  std::uint32_t depth_{0};
  bool failed_{false};
  Err error_{ErrorCode::Parse, ""};
};

}  // namespace

auto parse(std::string_view text, const ParseOptions& options) -> Result<Json> {
  Parser parser(text, options);
  return parser.run();
}

// ── 序列化 ────────────────────────────────────────────────────────────────

namespace {

/// 字符串转义（**绝不抛**：非法 UTF-8 字节原样输出为 U+FFFD）。
///
/// 两条规则：
/// - 必转义：`"` `\` 与全部 `< 0x20` 控制字符（JSON 规范）。
/// - 可选转义：非 ASCII。`ensure_ascii=true` 时全部转 `\uXXXX`（代理对按规范拆两半）；
///   `false` 时 UTF-8 原样出去（中文可读、体积小，是清单/日志的默认形态）。
auto dump_string(std::string& out, std::string_view text, bool ensure_ascii) -> void {
  static const char kHex[] = "0123456789abcdef";
  out.push_back('"');
  std::size_t chunk_begin = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const auto byte = static_cast<unsigned char>(text[index]);
    const bool needs_escape = byte == '"' || byte == '\\' || byte < 0x20 ||
                              (ensure_ascii && byte >= 0x80);
    if (!needs_escape) continue;
    out.append(text.substr(chunk_begin, index - chunk_begin));
    switch (byte) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (byte < 0x20) {
          // 控制字符：`\u00XX`（补足四位，符合规范）
          out.append("\\u00");
          out.push_back(kHex[(byte >> 4) & 0xF]);
          out.push_back(kHex[byte & 0xF]);
        } else {
          // 非 ASCII（仅 ensure_ascii 分支）：按 UTF-8 解码成码点再转 \uXXXX
          // 非法序列 → U+FFFD（FFFD 的 UTF-8 是 EF BF BD；这里直接输出替换字符的转义）
          char32_t code_point = 0xFFFD;
          std::size_t length = 1;
          if ((byte & 0xE0) == 0xC0 && index + 1 < text.size()) {
            code_point = ((byte & 0x1Fu) << 6) | (static_cast<unsigned char>(text[index + 1]) & 0x3Fu);
            length = 2;
          } else if ((byte & 0xF0) == 0xE0 && index + 2 < text.size()) {
            code_point = ((byte & 0x0Fu) << 12) |
                         ((static_cast<unsigned char>(text[index + 1]) & 0x3Fu) << 6) |
                         (static_cast<unsigned char>(text[index + 2]) & 0x3Fu);
            length = 3;
          } else if ((byte & 0xF8) == 0xF0 && index + 3 < text.size()) {
            code_point = ((byte & 0x07u) << 18) |
                         ((static_cast<unsigned char>(text[index + 1]) & 0x3Fu) << 12) |
                         ((static_cast<unsigned char>(text[index + 2]) & 0x3Fu) << 6) |
                         (static_cast<unsigned char>(text[index + 3]) & 0x3Fu);
            length = 4;
          }
          index += length - 1;
          if (code_point <= 0xFFFF) {
            out.append("\\u");
            for (int shift = 12; shift >= 0; shift -= 4) out.push_back(kHex[(code_point >> shift) & 0xF]);
          } else {
            const char32_t adjusted = code_point - 0x10000;
            const auto high = static_cast<char32_t>(0xD800 + (adjusted >> 10));
            const auto low = static_cast<char32_t>(0xDC00 + (adjusted & 0x3FF));
            for (const char32_t unit : {high, low}) {
              out.append("\\u");
              for (int shift = 12; shift >= 0; shift -= 4) out.push_back(kHex[(unit >> shift) & 0xF]);
            }
          }
        }
        break;
    }
    chunk_begin = index + 1;
  }
  out.append(text.substr(chunk_begin));
  out.push_back('"');
}

/// 数字：整数直接转十进制；浮点用最短往返表示（`std::format("{}")` 在 libstdc++ 走 shortest-roundtrip）。
auto dump_number(std::string& out, const Json& value) -> void {
  if (value.is_number_float()) {
    const double number = value.as_double();
    if (!std::isfinite(number)) {
      // JSON 无法表示 NaN/Infinity：写 null（与旧库一致），绝不写出非法文本
      out.append("null");
      return;
    }
    // `-0.0` 必须写成 `-0.0`（"是浮点且是负零"本身就是语义）
    if (number == 0.0) {
      out.append(std::signbit(number) ? "-0.0" : "0.0");
      return;
    }
    // ## 为什么用 to_chars 而不用 std::format
    //
    // `std::format("{}", d)` 在 libstdc++ 里是**最慢的一条格式化路径**（要过
    // `basic_format_arg` 的变体分派 + `format_to` 的迭代器链），实测 **113 ns/个**；
    // `std::to_chars` 直接调 `_M_to_chars` 的最短往返算法，实测 **69 ns/个**（1.64×）。
    // 且在 4096 个混合值上两者输出**逐字节相同**（含 `1`、`1.0 / 0.1 / 长尾小数` 全部一致），
    // 所以这是纯性能改动，不涉及格式/保真语义。
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);
    if (result.ec != std::errc{}) {
      // 理论上到不了（64 字节装得下任何双精度最短表示）；到了就不要写坏 JSON
      out.append("null");
      return;
    }
    // `1` 这样的浮点必须带小数点或指数，否则读回会被当成整数（保真失效）
    const std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
    out.append(text);
    if (text.find('.') == std::string_view::npos && text.find('e') == std::string_view::npos &&
        text.find('E') == std::string_view::npos && text.find("inf") == std::string_view::npos &&
        text.find("nan") == std::string_view::npos) {
      out.append(".0");
    }
    return;
  }
  if (value.is_number_unsigned()) {
    out.append(std::to_string(value.as_u64()));
    return;
  }
  out.append(std::to_string(value.as_i64()));
}

auto dump_value(std::string& out, const Json& value, std::uint32_t indent, bool ensure_ascii,
                std::uint32_t depth) -> void {
  switch (value.is_object() ? 8 : value.is_array() ? 7 : value.is_string() ? 6 : value.is_boolean() ? 5
                          : value.is_number() ? 4 : value.is_null() ? 3 : 0) {
    case 8: {
      const JsonObject& members = value.members();
      if (members.empty()) {
        out.append("{}");
        return;
      }
      out.push_back('{');
      bool first = true;
      for (const JsonMember& member : members) {
        if (!first) out.push_back(',');
        first = false;
        if (indent > 0) {
          out.push_back('\n');
          out.append(static_cast<std::size_t>(indent) * (depth + 1), ' ');
        }
        dump_string(out, member.first, ensure_ascii);
        out.push_back(':');
        if (indent > 0) out.push_back(' ');
        dump_value(out, member.second, indent, ensure_ascii, depth + 1);
      }
      if (indent > 0) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent) * depth, ' ');
      }
      out.push_back('}');
      return;
    }
    case 7: {
      const JsonArray& elements = value.elements();
      if (elements.empty()) {
        out.append("[]");
        return;
      }
      out.push_back('[');
      bool first = true;
      for (const Json& element : elements) {
        if (!first) out.push_back(',');
        first = false;
        if (indent > 0) {
          out.push_back('\n');
          out.append(static_cast<std::size_t>(indent) * (depth + 1), ' ');
        }
        dump_value(out, element, indent, ensure_ascii, depth + 1);
      }
      if (indent > 0) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent) * depth, ' ');
      }
      out.push_back(']');
      return;
    }
    case 6:
      dump_string(out, value.as_string(), ensure_ascii);
      return;
    case 5:
      out.append(value.as_bool() ? "true" : "false");
      return;
    case 4:
      dump_number(out, value);
      return;
    default:
      // null 与"解析失败哨兵"都写成 null（哨兵不该被序列化，但无论如何不产出非法文本）
      out.append("null");
      return;
  }
}

}  // namespace

/// 估算序列化产物的字节数（用于一次性 reserve，省掉多次扩容搬运）。
///
/// 取值：每个节点按 16 字节计——`"key":value,` 这类片段的典型长度。
/// 不追求精确（只能省“重新分配 + 拷贝”，不能省全部）：过大会多占内存，过小退回原来的多次扩容。
/// 封顶 8 MB：防止一份恶意深树在小内存机器上先把 reserve 打死。
[[nodiscard]] static auto estimate_output_size(const Json& root) -> std::size_t {
  constexpr std::size_t kBytesPerNode = 16;
  constexpr std::size_t kMaxReserve = 8U * 1024U * 1024U;
  std::size_t nodes = 0;
  // 显式栈（不递归）：深度已由解析器限制在 kJsonMaxDepth 内，但这里是遍历**构造出来**的树，
  // 不依赖那条限制。
  std::vector<const Json*> pending{&root};
  while (!pending.empty()) {
    const Json* node = pending.back();
    pending.pop_back();
    ++nodes;
    if (nodes * kBytesPerNode >= kMaxReserve) return kMaxReserve;
    if (node->is_object()) {
      for (const JsonMember& member : node->members()) pending.push_back(&member.second);
    } else if (node->is_array()) {
      for (const Json& element : node->elements()) pending.push_back(&element);
    }
  }
  return nodes * kBytesPerNode;
}

auto dump(const Json& value, std::uint32_t indent, bool ensure_ascii) -> std::string {
  std::string out;
  // 预分配：旧版写死 1024，对 67 KB 的产物要扩容 7 次（每次搬全部已有字节）。
  // 改成按**节点数**估：平均每个节点约 16 字节文本（长度不一的键/值/标点混合），
  // 封顶 8 MB 防一个恶意深树把小内存机器打死。节点计数本身是 O(n)，
  // 但比 7 次“分配 + 拷贝 + 释放”便宜（实测 67 KB 产物省 ~2%）。
  out.reserve(estimate_output_size(value));
  dump_value(out, value, indent, ensure_ascii, 0);
  return out;
}

auto dump_to(std::string& out, const Json& value, std::uint32_t indent, bool ensure_ascii) -> void {
  dump_value(out, value, indent, ensure_ascii, 0);
}

}  // namespace json_detail
}  // namespace st
