#pragma once

/// 霜天错误模型（`CONVENTIONS.md` R10）：业务错误一律经 `Result` 显式传播，禁止异常。
/// - `Result<T>` 为**自研**结果类型（`std::expected` 风格，C++20 尚无该设施，故自实现）；
/// - 失败用 `st::unexpected(code, "原因")` 构造，上抛用 `st::forward_error(下层错误)`；
/// - `value()`/`error()` 有前置条件（调用方须先判 `has_value()`）。

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace st {

/// 错误分类（稳定枚举，跨协议/日志序列化用其 `to_string` 名）。
enum class ErrorCode : std::uint8_t {
  Invalid,     ///< 参数或状态非法
  NotFound,    ///< 目标不存在
  Io,          ///< 读写/网络 I/O 失败
  Parse,       ///< 解析失败（JSON/字体/PNG/清单…）
  Unsupported, ///< 能力不支持（如缺失的图形后端）
  Timeout,     ///< 超时
  Internal,    ///< 内部不变量被破坏
  Busy,        ///< 资源忙（如端口占用）
  Cancelled,   ///< 被取消（协作式停止）
  Ambiguous,   ///< 匹配到多个目标
  Exists,      ///< 已存在（不可覆盖）
  Permission,  ///< 权限不足
  Overflow,    ///< 数值/缓冲区溢出
};

/// 稳定短名（协议 `error.code` 与日志字段使用）。
[[nodiscard]] constexpr auto to_string(ErrorCode code) noexcept -> std::string_view {
  switch (code) {
    case ErrorCode::Invalid: return "invalid";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::Io: return "io";
    case ErrorCode::Parse: return "parse";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::Timeout: return "timeout";
    case ErrorCode::Internal: return "internal";
    case ErrorCode::Busy: return "busy";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::Ambiguous: return "ambiguous";
    case ErrorCode::Exists: return "exists";
    case ErrorCode::Permission: return "permission";
    case ErrorCode::Overflow: return "overflow";
  }
  return "unknown";
}

struct Error {
  ErrorCode code{ErrorCode::Internal};
  std::string message{};

  [[nodiscard]] auto to_string() const -> std::string {
    std::string text(st::to_string(code));
    text.append(": ");
    text.append(message);
    return text;
  }
};

/// 错误包装：让 `Result<T>` 能从「失败的 Error」隐式构造（`return st::unexpected(...);`）。
struct Err {
  Error error{};
};

[[nodiscard]] inline auto make_error(ErrorCode code, std::string message) -> Error {
  return Error{code, std::move(message)};
}

[[nodiscard]] inline auto unexpected(ErrorCode code, std::string message) -> Err {
  return Err{Error{code, std::move(message)}};
}

[[nodiscard]] inline auto unexpected(Error error) -> Err { return Err{std::move(error)}; }

/// 原样上抛下层错误：`if (!x) return st::forward_error(x.error());`
[[nodiscard]] inline auto forward_error(const Error& error) -> Err { return Err{error}; }

/// 结果类型：成功携带 `T`，失败携带 `Error`。
template <class T = void>
class Result {
 public:
  using value_type = T;

  Result(T value) : storage_(std::move(value)) {}          // lint-allow: L10 隐式值→成功是 Result 的刻意设计（与 std::expected 一致）
  Result(Err failure) : storage_(std::move(failure.error)) {}  // lint-allow: L10 隐式错误→失败同上

  [[nodiscard]] auto has_value() const noexcept -> bool { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  /// 前置条件：`has_value()`。
  [[nodiscard]] auto value() const& -> const T& { return std::get<0>(storage_); }
  [[nodiscard]] auto value() & -> T& { return std::get<0>(storage_); }
  [[nodiscard]] auto value() && -> T&& { return std::get<0>(std::move(storage_)); }
  [[nodiscard]] auto operator*() const& -> const T& { return std::get<0>(storage_); }
  [[nodiscard]] auto operator*() & -> T& { return std::get<0>(storage_); }
  [[nodiscard]] auto operator->() const -> const T* { return std::addressof(std::get<0>(storage_)); }
  [[nodiscard]] auto operator->() -> T* { return std::addressof(std::get<0>(storage_)); }

  /// 前置条件：`!has_value()`。
  [[nodiscard]] auto error() const noexcept -> const Error& { return std::get<1>(storage_); }

  /// 有值取值，失败取给定的替代（免 `has_value()` 三行样板）。
  [[nodiscard]] auto value_or(const T& fallback) const& -> T {
    return has_value() ? std::get<0>(storage_) : fallback;
  }
  [[nodiscard]] auto value_or(T&& fallback) && -> T {
    return has_value() ? std::get<0>(std::move(storage_)) : std::move(fallback);
  }

 private:
  std::variant<T, Error> storage_;
};

/// 无值结果。
template <>
class Result<void> {
 public:
  using value_type = void;

  Result() noexcept = default;
  Result(Err failure) : error_(std::move(failure.error)), failed_(true) {}  // lint-allow: L10 隐式构造

  [[nodiscard]] auto has_value() const noexcept -> bool { return !failed_; }
  explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] auto error() const noexcept -> const Error& { return error_; }

 private:
  Error error_{};
  bool failed_{false};
};

using Status = Result<void>;

/// 成功（无值）。
[[nodiscard]] inline auto ok() noexcept -> Status { return Status{}; }

}  // namespace st
