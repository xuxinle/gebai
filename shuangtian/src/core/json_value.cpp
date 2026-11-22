#include "st/core/json_value.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

// `Json::parse` 的兼容入口需要完整解析器（它只在本 TU 里用到，不进公开头——
// 否则 `json_value.hpp` 与 `json_parse.hpp` 会互相包含）。
#include "st/core/json_parse.hpp"

namespace st {
namespace {

/// 空成员表/元素表（非对象/非数组时返回它们，供范围 for 得到空区间）。
[[nodiscard]] auto empty_members() noexcept -> const JsonObject& {
  static const JsonObject instance;
  return instance;
}

[[nodiscard]] auto empty_elements() noexcept -> const JsonArray& {
  static const JsonArray instance;
  return instance;
}

}  // namespace

auto Json::parse(std::string_view text) -> Json {
  json_detail::ParseOptions options;
  auto parsed = json_detail::parse(text, options);
  if (!parsed) {
    // 哨兵值：`is_discarded()` 为真（调用方应当视为"没有值"）。
    // 需要错误消息时走 `st::json_parse`（Result + 行列号定位）。
    Json discarded;
    discarded.kind_ = json_detail::Kind::Discarded;
    return discarded;
  }
  return std::move(*parsed);
}

// ── 数组型构造（`node["names"] = some_string_vector;` 的便利通道）──────────

Json::Json(const std::vector<std::string>& items) : kind_(json_detail::Kind::Array) {
  array_.reserve(items.size());
  for (const std::string& item : items) array_.emplace_back(item);
}

Json::Json(std::vector<std::string>&& items) : kind_(json_detail::Kind::Array) {
  array_.reserve(items.size());
  for (std::string& item : items) array_.emplace_back(std::move(item));
}

Json::Json(const std::vector<std::int64_t>& items) : kind_(json_detail::Kind::Array) {
  array_.reserve(items.size());
  for (const std::int64_t item : items) array_.emplace_back(Json(static_cast<long long>(item)));
}

Json::Json(const std::vector<double>& items) : kind_(json_detail::Kind::Array) {
  array_.reserve(items.size());
  for (const double item : items) array_.emplace_back(Json(item));
}

Json::Json(const std::vector<float>& items) : kind_(json_detail::Kind::Array) {
  array_.reserve(items.size());
  for (const float item : items) array_.emplace_back(Json(static_cast<double>(item)));
}

Json::Json(const std::vector<Json>& items) : kind_(json_detail::Kind::Array) { array_ = items; }

Json::Json(std::vector<Json>&& items) : kind_(json_detail::Kind::Array) { array_ = std::move(items); }

auto Json::append(const std::vector<Json>& items) -> void {
  if (kind_ != json_detail::Kind::Array) *this = Json::array();
  array_.insert(array_.end(), items.begin(), items.end());
}

// ── 工厂 ──────────────────────────────────────────────────────────────────

auto Json::object() -> Json {
  Json value;
  value.kind_ = json_detail::Kind::Object;
  return value;
}

auto Json::object(std::initializer_list<std::pair<std::string_view, Json>> members) -> Json {
  Json value;
  value.kind_ = json_detail::Kind::Object;
  value.object_.reserve(members.size());
  for (const auto& member : members) {
    // 重复键：后者覆盖前者的值，保留首次出现位置（与解析器的重复键语义一致）
    Json* existing = value.find_mutable(member.first);
    if (existing != nullptr) {
      *existing = member.second;
      continue;
    }
    value.object_.push_back(JsonMember{std::string(member.first), member.second});
  }
  return value;
}

auto Json::array() -> Json {
  Json value;
  value.kind_ = json_detail::Kind::Array;
  return value;
}

auto Json::array(std::initializer_list<Json> items) -> Json {
  Json value;
  value.kind_ = json_detail::Kind::Array;
  value.array_.assign(items.begin(), items.end());
  return value;
}

// ── 尺寸 ──────────────────────────────────────────────────────────────────

auto Json::size() const noexcept -> std::size_t {
  switch (kind_) {
    case json_detail::Kind::Object:
      return object_.size();
    case json_detail::Kind::Array:
      return array_.size();
    case json_detail::Kind::String:
      return text_.size();
    case json_detail::Kind::Null:
    case json_detail::Kind::Discarded:
      return 0;
    default:
      return 1;
  }
}

auto Json::empty() const noexcept -> bool {
  switch (kind_) {
    case json_detail::Kind::Object:
      return object_.empty();
    case json_detail::Kind::Array:
      return array_.empty();
    case json_detail::Kind::String:
      return text_.empty();
    case json_detail::Kind::Null:
    case json_detail::Kind::Discarded:
      return true;
    default:
      return false;
  }
}

// ── 原始值访问 ────────────────────────────────────────────────────────────

auto Json::as_i64() const noexcept -> std::int64_t {
  switch (kind_) {
    case json_detail::Kind::Integer:
      return scalar_.integer_;
    case json_detail::Kind::Unsigned:
      return scalar_.unsigned_ <= static_cast<std::uint64_t>(INT64_MAX) ? static_cast<std::int64_t>(scalar_.unsigned_) : 0;
    case json_detail::Kind::Float:
      return static_cast<std::int64_t>(scalar_.number_);
    default:
      return 0;
  }
}

auto Json::as_u64() const noexcept -> std::uint64_t {
  switch (kind_) {
    case json_detail::Kind::Unsigned:
      return scalar_.unsigned_;
    case json_detail::Kind::Integer:
      return scalar_.integer_ < 0 ? 0U : static_cast<std::uint64_t>(scalar_.integer_);
    case json_detail::Kind::Float:
      return scalar_.number_ < 0 ? 0U : static_cast<std::uint64_t>(scalar_.number_);
    default:
      return 0;
  }
}

auto Json::as_double() const noexcept -> double {
  switch (kind_) {
    case json_detail::Kind::Float:
      return scalar_.number_;
    case json_detail::Kind::Integer:
      return static_cast<double>(scalar_.integer_);
    case json_detail::Kind::Unsigned:
      return static_cast<double>(scalar_.unsigned_);
    default:
      return 0.0;
  }
}

auto Json::as_string() const noexcept -> const std::string& {
  static const std::string empty;
  return kind_ == json_detail::Kind::String ? text_ : empty;
}

auto Json::members() const noexcept -> const JsonObject& {
  return kind_ == json_detail::Kind::Object ? object_ : empty_members();
}

auto Json::elements() const noexcept -> const JsonArray& {
  return kind_ == json_detail::Kind::Array ? array_ : empty_elements();
}

// ── 对象访问 ──────────────────────────────────────────────────────────────
// `find` / `find_mutable` 定义在头文件（**热路径，需要内联**）：见 json_value.hpp 中的注释。

auto Json::operator[](std::string_view key) const noexcept -> const Json& {
  const Json* found = find(key);
  return found != nullptr ? *found : Json::null_node();
}

auto Json::operator[](std::string_view key) noexcept -> Json& {
  // 写路径：非对象先"升级"成对象（`node["k"] = v` 能凭空长出对象）
  if (kind_ != json_detail::Kind::Object) *this = Json::object();
  Json* it = find_mutable(key);
  if (it != nullptr) return *it;
  // 插入到末尾：保序语义下"新键在后"（与写完再读回的顺序一致）
  object_.push_back(JsonMember{std::string(key), Json()});
  return object_.back().second;
}

// ── 数组访问 ──────────────────────────────────────────────────────────────

auto Json::operator[](std::size_t index) const noexcept -> const Json& {
  if (kind_ != json_detail::Kind::Array || index >= array_.size()) return Json::null_node();
  return array_[index];
}

auto Json::operator[](std::size_t index) noexcept -> Json& {
  if (kind_ != json_detail::Kind::Array) *this = Json::array();
  // 越界自动扩展（`arr[2] = x` 会把中间补成 null）
  if (index >= array_.size()) array_.resize(index + 1);
  return array_[index];
}

auto Json::push_back(Json value) -> void {
  if (kind_ != json_detail::Kind::Array) *this = Json::array();
  array_.push_back(std::move(value));
}

auto Json::emplace_back() -> Json& {
  if (kind_ != json_detail::Kind::Array) *this = Json::array();
  array_.emplace_back();
  return array_.back();
}

auto Json::back() const noexcept -> const Json& {
  if (kind_ != json_detail::Kind::Array || array_.empty()) return Json::null_node();
  return array_.back();
}

auto Json::back() noexcept -> Json& {
  if (kind_ != json_detail::Kind::Array) *this = Json::array();
  if (array_.empty()) array_.emplace_back();
  return array_.back();
}

auto Json::front() const noexcept -> const Json& {
  if (kind_ != json_detail::Kind::Array || array_.empty()) return Json::null_node();
  return array_.front();
}

auto Json::front() noexcept -> Json& {
  if (kind_ != json_detail::Kind::Array) *this = Json::array();
  if (array_.empty()) array_.emplace_back();
  return array_.front();
}

// ── 修改 ──────────────────────────────────────────────────────────────────

auto Json::clear() noexcept -> void {
  switch (kind_) {
    case json_detail::Kind::Object:
      object_.clear();
      break;
    case json_detail::Kind::Array:
      array_.clear();
      break;
    case json_detail::Kind::String:
      text_.clear();
      break;
    default:
      // 标量/null：清成 null（"清空一个数字"的唯一合理语义）
      kind_ = json_detail::Kind::Null;
      break;
  }
}

auto Json::erase(std::string_view key) -> bool {
  if (kind_ != json_detail::Kind::Object) return false;
  // `find_mutable` 返回的是值指针（不是迭代器）：这里要用下标才能 `erase`
  for (std::size_t index = 0; index < object_.size(); ++index) {
    if (object_[index].first == key) {
      object_.erase(object_.begin() + static_cast<JsonObject::difference_type>(index));
      return true;
    }
  }
  return false;
}

auto Json::erase_index(std::size_t index) -> bool {
  if (kind_ != json_detail::Kind::Array || index >= array_.size()) return false;
  array_.erase(array_.begin() + static_cast<JsonArray::difference_type>(index));
  return true;
}

// ── 内部 ──────────────────────────────────────────────────────────────────

auto Json::members_mutable() noexcept -> JsonObject& {
  if (kind_ != json_detail::Kind::Object) *this = Json::object();
  return object_;
}

auto Json::elements_mutable() noexcept -> JsonArray& {
  if (kind_ != json_detail::Kind::Array) *this = Json::array();
  return array_;
}

// ── 迭代器 ────────────────────────────────────────────────────────────────

auto Json::members_begin() const noexcept -> const_object_iterator { return object_.begin(); }

auto Json::members_end() const noexcept -> const_object_iterator { return object_.end(); }

auto Json::dump(std::uint32_t indent) const -> std::string {
  // 与 `st::json_dump(value, pretty, indent)` 同一条路径（本 TU 已包含 json_parse.hpp）。
  return json_detail::dump(*this, indent, /*ensure_ascii=*/false);
}

auto Json::structurally_equal(const Json& other) const noexcept -> bool {
  // 数字跨类别：整型/无符号/浮点按**数值**比（`1` 与 `1.0` 是同一个数）
  if (is_number() && other.is_number()) {
    if (is_number_float() || other.is_number_float()) return as_double() == other.as_double();
    if (is_number_unsigned() || other.is_number_unsigned()) return as_u64() == other.as_u64();
    return as_i64() == other.as_i64();
  }
  if (kind_ != other.kind_) return false;
  switch (kind_) {
    case json_detail::Kind::Null:
    case json_detail::Kind::Discarded:
      return true;
    case json_detail::Kind::Boolean:
      return boolean_ == other.boolean_;
    case json_detail::Kind::String:
      return text_ == other.text_;
    case json_detail::Kind::Array: {
      if (array_.size() != other.array_.size()) return false;
      for (std::size_t index = 0; index < array_.size(); ++index) {
        if (!array_[index].structurally_equal(other.array_[index])) return false;
      }
      return true;
    }
    case json_detail::Kind::Object: {
      if (object_.size() != other.object_.size()) return false;
      // **不看键序**：键序是存储细节，不是值语义。按"每对键值都能在对方找到相等值"比。
      for (const JsonMember& member : object_) {
        const Json* found = other.find(member.first);
        if (found == nullptr) return false;
        if (!member.second.structurally_equal(*found)) return false;
      }
      return true;
    }
    default:
      return false;  // 数字已在上方处理
  }
}

auto Json::begin() const -> const_iterator {
  if (kind_ == json_detail::Kind::Object) return const_iterator(object_.begin(), object_.end());
  if (kind_ == json_detail::Kind::Array) return const_iterator(array_.begin(), array_.end());
  const JsonArray& nothing = empty_elements();
  return const_iterator(nothing.begin(), nothing.end());
}

auto Json::end() const -> const_iterator {
  if (kind_ == json_detail::Kind::Object) return const_iterator(object_.end(), object_.end());
  if (kind_ == json_detail::Kind::Array) return const_iterator(array_.end(), array_.end());
  const JsonArray& nothing = empty_elements();
  return const_iterator(nothing.end(), nothing.end());
}

auto Json::begin() -> array_iterator { return elements_mutable().begin(); }

auto Json::end() -> array_iterator { return elements_mutable().end(); }

// ── const_iterator ────────────────────────────────────────────────────────

auto Json::const_iterator::operator*() const -> const Json& {
  if (as_object_) return object_->second;
  return *array_;
}

auto Json::const_iterator::key() const -> const std::string& {
  static const std::string empty;
  return as_object_ ? object_->first : empty;
}

auto Json::const_iterator::value() const -> const Json& {
  if (as_object_) return object_->second;
  return *array_;
}

auto Json::const_iterator::operator++() -> const_iterator& {
  if (as_object_) {
    ++object_;
  } else {
    ++array_;
  }
  return *this;
}

auto Json::const_iterator::operator++(int) -> const_iterator {
  const_iterator copy = *this;
  ++(*this);
  return copy;
}

auto Json::const_iterator::operator==(const const_iterator& other) const noexcept -> bool {
  if (as_object_ != other.as_object_) return false;
  if (as_object_) return object_ == other.object_;
  return array_ == other.array_;
}

}  // namespace st
