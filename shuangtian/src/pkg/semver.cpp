#include "st/pkg/semver.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/string.hpp"

namespace st::pkg {
namespace {

inline constexpr std::uint64_t max_u32_value = 0xFFFFFFFFULL;

[[nodiscard]] auto is_ascii_digit(char value) noexcept -> bool {
  return value >= '0' && value <= '9';
}

[[nodiscard]] auto is_ident_char(char value) noexcept -> bool {
  return is_ascii_digit(value) || (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         value == '-';
}

/// `.` 分隔的标识符串是否合法（非空段、段内字符限于 `[0-9A-Za-z-]`）。
[[nodiscard]] auto valid_dot_identifiers(std::string_view text) -> bool {
  if (text.empty()) return false;
  for (const auto part : st::split(text, '.')) {
    if (part.empty()) return false;
    for (const char character : part) {
      if (!is_ident_char(character)) return false;
    }
  }
  return true;
}

/// 纯数字段是否合法（非空且全数字）；`parse_core_number` 的前置条件由此保证。
[[nodiscard]] auto valid_number(std::string_view text) -> bool {
  if (text.empty()) return false;
  for (const char character : text) {
    if (!is_ascii_digit(character)) return false;
  }
  return true;
}

[[nodiscard]] auto parse_core_number(std::string_view digits) -> Result<std::uint32_t> {
  std::uint64_t value = 0;
  for (const char character : digits) {
    value = value * 10ULL + static_cast<std::uint64_t>(character - '0');
    if (value > max_u32_value) {
      return unexpected(ErrorCode::Overflow, std::format("版本数值段超出 uint32 范围: {}", digits));
    }
  }
  return static_cast<std::uint32_t>(value);
}

[[nodiscard]] auto starts_with(std::string_view text, std::string_view prefix) -> bool {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

/// 预发布标识是否为数字段（无前导零语义，按数值比较）。
[[nodiscard]] auto numeric_identifier(std::string_view text) -> bool { return valid_number(text); }

/// 数字标识符按数值比较（去前导零后先比长度、再比字典序），避免大数溢出。
[[nodiscard]] auto compare_numeric_identifier(std::string_view a, std::string_view b) -> int {
  std::size_t a_begin = 0;
  while (a_begin + 1 < a.size() && a[a_begin] == '0') ++a_begin;
  std::size_t b_begin = 0;
  while (b_begin + 1 < b.size() && b[b_begin] == '0') ++b_begin;
  const std::string_view left = a.substr(a_begin);
  const std::string_view right = b.substr(b_begin);
  if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
  if (left == right) return 0;
  return left < right ? -1 : 1;
}

[[nodiscard]] auto compare_prerelease(std::string_view a, std::string_view b) -> int {
  const std::vector<std::string_view> left = st::split(a, '.');
  const std::vector<std::string_view> right = st::split(b, '.');
  const std::size_t shared = left.size() < right.size() ? left.size() : right.size();
  for (std::size_t index = 0; index < shared; ++index) {
    const std::string_view lhs = left[index];
    const std::string_view rhs = right[index];
    const bool lhs_numeric = numeric_identifier(lhs);
    const bool rhs_numeric = numeric_identifier(rhs);
    if (lhs_numeric && rhs_numeric) {
      const int order = compare_numeric_identifier(lhs, rhs);
      if (order != 0) return order;
      continue;
    }
    if (lhs_numeric != rhs_numeric) return lhs_numeric ? -1 : 1;  // 数字段 < 字母数字段
    if (lhs != rhs) return lhs < rhs ? -1 : 1;
  }
  if (left.size() == right.size()) return 0;
  return left.size() < right.size() ? -1 : 1;  // 段数少者小
}

/// `^1.2.3` 的上界：首个非零段进位，其余归零。
[[nodiscard]] auto caret_upper(const Version& base) -> Version {
  Version upper{};
  if (base.major > 0U) {
    upper.major = base.major + 1U;
  } else if (base.minor > 0U) {
    upper.minor = base.minor + 1U;
  } else {
    upper.patch = base.patch + 1U;
  }
  return upper;
}

/// `~1.2.3` 的上界：同 major 的下一个 minor。
[[nodiscard]] auto tilde_upper(const Version& base) -> Version {
  Version upper{};
  upper.major = base.major;
  upper.minor = base.minor + 1U;
  return upper;
}

[[nodiscard]] auto comparator_matches(const Comparator& comparator, const Version& candidate)
    -> bool {
  if (comparator.op == Comparator::Op::Any) return true;
  // npm 口径：约束未点名预发布时，预发布候选不匹配（正式版约束不应选到 alpha/beta）。
  if (candidate.is_prerelease() && comparator.version.pre.empty()) return false;
  switch (comparator.op) {
    case Comparator::Op::Any:
      return true;
    case Comparator::Op::Exact:
      return compare(candidate, comparator.version) == 0;
    case Comparator::Op::Greater:
      return compare(candidate, comparator.version) > 0;
    case Comparator::Op::GreaterEqual:
      return compare(candidate, comparator.version) >= 0;
    case Comparator::Op::Less:
      return compare(candidate, comparator.version) < 0;
    case Comparator::Op::LessEqual:
      return compare(candidate, comparator.version) <= 0;
    case Comparator::Op::Caret:
      return compare(candidate, comparator.version) >= 0 &&
             compare(candidate, caret_upper(comparator.version)) < 0;
    case Comparator::Op::Tilde:
      return compare(candidate, comparator.version) >= 0 &&
             compare(candidate, tilde_upper(comparator.version)) < 0;
  }
  return false;
}

}  // namespace

auto Version::parse(std::string_view text) -> Result<Version> {
  const std::string_view trimmed = st::trim(text);
  if (trimmed.empty()) return unexpected(ErrorCode::Parse, "版本号为空");

  std::string_view core = trimmed;
  std::string_view prerelease{};
  std::string_view build{};

  if (const std::size_t plus = core.find('+'); plus != std::string_view::npos) {
    build = core.substr(plus + 1);
    core = core.substr(0, plus);
    if (!valid_dot_identifiers(build)) {
      return unexpected(ErrorCode::Parse,
                        std::format("构建元数据非法: {}", std::string(text)));
    }
  }
  if (const std::size_t dash = core.find('-'); dash != std::string_view::npos) {
    prerelease = core.substr(dash + 1);
    core = core.substr(0, dash);
    if (!valid_dot_identifiers(prerelease)) {
      return unexpected(ErrorCode::Parse, std::format("预发布标识非法: {}", std::string(text)));
    }
  }

  const std::vector<std::string_view> parts = st::split(core, '.');
  if (parts.empty() || parts.size() > 3) {
    return unexpected(ErrorCode::Parse,
                      std::format("版本号应为 1~3 段数字: {}", std::string(text)));
  }
  std::array<std::uint32_t, 3> numbers{0U, 0U, 0U};
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (!valid_number(parts[index])) {
      return unexpected(ErrorCode::Parse,
                        std::format("版本数字段非法: {}", std::string(parts[index])));
    }
    const Result<std::uint32_t> parsed = parse_core_number(parts[index]);
    if (!parsed) return forward_error(parsed.error());
    numbers[index] = *parsed;
  }

  Version version;
  version.major = numbers[0];
  version.minor = numbers[1];
  version.patch = numbers[2];
  version.pre = std::string(prerelease);
  version.build = std::string(build);
  return version;
}

auto Version::to_string() const -> std::string {
  std::string text = std::format("{}.{}.{}", major, minor, patch);
  if (!pre.empty()) {
    text.push_back('-');
    text.append(pre);
  }
  if (!build.empty()) {
    text.push_back('+');
    text.append(build);
  }
  return text;
}

auto Version::without_pre() const -> Version {
  Version copy = *this;
  copy.pre.clear();
  return copy;
}

auto compare(const Version& a, const Version& b) -> int {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  const bool a_pre = a.is_prerelease();
  const bool b_pre = b.is_prerelease();
  if (!a_pre && !b_pre) return 0;
  if (!a_pre) return 1;   // 正式版 > 预发布
  if (!b_pre) return -1;
  return compare_prerelease(a.pre, b.pre);
}

auto op_name(Comparator::Op op) -> std::string_view {
  switch (op) {
    case Comparator::Op::Any: return "*";
    case Comparator::Op::Exact: return "=";
    case Comparator::Op::Caret: return "^";
    case Comparator::Op::Tilde: return "~";
    case Comparator::Op::Greater: return ">";
    case Comparator::Op::GreaterEqual: return ">=";
    case Comparator::Op::Less: return "<";
    case Comparator::Op::LessEqual: return "<=";
  }
  return "?";
}

auto VersionReq::parse(std::string_view text) -> Result<VersionReq> {
  const std::string normalized = st::replace_all(text, ",", " ");
  const std::vector<std::string_view> tokens = st::split_whitespace(normalized);
  if (tokens.empty()) return unexpected(ErrorCode::Parse, "版本约束为空");

  VersionReq req;
  for (const std::string_view token : tokens) {
    Comparator comparator;
    std::string_view rest = token;
    if (token == "*" || token == "x" || token == "X") {
      comparator.op = Comparator::Op::Any;
      req.comparators.push_back(comparator);
      continue;
    }
    if (starts_with(rest, ">=")) {
      comparator.op = Comparator::Op::GreaterEqual;
      rest = rest.substr(2);
    } else if (starts_with(rest, "<=")) {
      comparator.op = Comparator::Op::LessEqual;
      rest = rest.substr(2);
    } else if (starts_with(rest, "==")) {
      comparator.op = Comparator::Op::Exact;
      rest = rest.substr(2);
    } else if (starts_with(rest, "^")) {
      comparator.op = Comparator::Op::Caret;
      rest = rest.substr(1);
    } else if (starts_with(rest, "~")) {
      comparator.op = Comparator::Op::Tilde;
      rest = rest.substr(1);
    } else if (starts_with(rest, ">")) {
      comparator.op = Comparator::Op::Greater;
      rest = rest.substr(1);
    } else if (starts_with(rest, "<")) {
      comparator.op = Comparator::Op::Less;
      rest = rest.substr(1);
    } else if (starts_with(rest, "=")) {
      comparator.op = Comparator::Op::Exact;
      rest = rest.substr(1);
    } else {
      comparator.op = Comparator::Op::Exact;  // 裸版本号 = 精确匹配
    }
    if (st::trim(rest).empty()) {
      return unexpected(ErrorCode::Parse, std::format("比较器缺少版本号: {}", std::string(token)));
    }
    const Result<Version> parsed = Version::parse(rest);
    if (!parsed) return forward_error(parsed.error());
    comparator.version = *parsed;
    req.comparators.push_back(comparator);
  }
  return req;
}

auto VersionReq::matches(const Version& version) const -> bool {
  for (const auto& comparator : comparators) {
    if (!comparator_matches(comparator, version)) return false;
  }
  return true;
}

auto VersionReq::to_string() const -> std::string {
  if (comparators.empty()) return "*";
  std::string text;
  for (const auto& comparator : comparators) {
    if (!text.empty()) text.push_back(' ');
    if (comparator.op == Comparator::Op::Any) {
      text.push_back('*');
      continue;
    }
    text.append(op_name(comparator.op));
    text.append(comparator.version.to_string());
  }
  return text;
}

auto VersionReq::any() -> VersionReq {
  VersionReq req;
  req.comparators.push_back(Comparator{});
  return req;
}

}  // namespace st::pkg
