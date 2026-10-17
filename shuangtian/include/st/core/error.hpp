#pragma once

/// 霜天错误模型（`CONVENTIONS.md` R10）：业务错误一律经 `Result` 显式传播，禁止异常。
/// - `Result<T>` 是 `std::expected<T, Error>` 的**薄门面**（C++23 起该设施可用）：
///   保留本仓既有调用面（`has_value`/`value`/`error`/`operator*`/`value_or`），
///   存储、生命周期与移动语义交给标准库——这正是升级到 C++23 的意义：
///   当年手写的 `variant<T, Error>` 只是它的临时替代品；
/// - 失败用 `st::unexpected(code, "原因")` 构造，上抛用 `st::forward_error(下层错误)`；
///   （`st::unexpected` 返回 `Err`，它是**唯一**能隐式转成失败态的入口，
///   以免 `Result<std::string>` 被一个字符串误当成成功值）；
/// - `value()`/`error()` 有前置条件（调用方须先判 `has_value()`）；
/// - C++23 的 monadic 接口（`and_then`/`transform`/`or_else`/`transform_error`）
///   直接转发给 `std::expected`，调用方可按需使用。

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

// 语言标准守卫与可选设施探测（低于 C++23 直接 #error，见该头注释）：
// 放在错误模型头里是因为 `Result` 几乎被每个翻译单元使用，守卫因此无需各自重复。
#include "st/core/cxx.hpp"

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
///
/// 内芯是 `std::expected`：本类只保留「本仓调用面 + `Err` 构造通道」，
/// 其余（拷贝/移动/生命周期/`value_or` 之后的 monadic 操作）全部转发。
/// **刻意不继承** `std::expected`：构造函数需逐条转发、而且继承会让 `Result` 与
/// `std::expected` 在重载决议里"长得一样"（如 `if (!r)` 的两套 `operator!` 相互影响）。
///
/// 不可隐式从 `Error` 构造（只能从 `Err`）：否则 `Result<std::string>` 里
/// 一个 `std::string` 与一个 `Error` 都能隐式进——后者合法但不直观，且与
/// “把一个值当成结果”混淆。
  template <class T = void>
class Result {
 public:
  using value_type = T;
  using storage_type = std::expected<T, Error>;

  Result(T value) : storage_(std::move(value)) {}              // lint-allow: L10 隐式值→成功是 Result 的刻意设计（与 std::expected 一致）
  Result(Err failure) : storage_(std::unexpect, std::move(failure.error)) {}  // lint-allow: L10 隐式错误→失败同上

  [[nodiscard]] auto has_value() const noexcept -> bool { return storage_.has_value(); }
  explicit operator bool() const noexcept { return storage_.has_value(); }

  /// 前置条件：`has_value()`。
  [[nodiscard]] auto value() const& -> const T& { return storage_.value(); }
  [[nodiscard]] auto value() & -> T& { return storage_.value(); }
  [[nodiscard]] auto value() && -> T&& { return std::move(storage_).value(); }
  [[nodiscard]] auto operator*() const& -> const T& { return *storage_; }
  [[nodiscard]] auto operator*() & -> T& { return *storage_; }
  [[nodiscard]] auto operator*() && -> T&& { return std::move(*storage_); }
  [[nodiscard]] auto operator->() const -> const T* { return storage_.operator->(); }
  [[nodiscard]] auto operator->() -> T* { return storage_.operator->(); }

  /// 前置条件：`!has_value()`。
  [[nodiscard]] auto error() const noexcept -> const Error& { return storage_.error(); }

  /// 有值取值，失败取给定的替代（免 `has_value()` 三行样板）。
  [[nodiscard]] auto value_or(const T& fallback) const& -> T { return storage_.value_or(fallback); }
  [[nodiscard]] auto value_or(T&& fallback) && -> T {
    return std::move(storage_).value_or(std::move(fallback));
  }

  /// monadic 接口：直接转发给 `std::expected`（C++23）。
  /// 返回类型由标准库推导——它仍是 `std::expected`，不是 `Result`：用空型链式
  /// 调用时不需要 `st::Err` 包装，语义更直接。
  template <class Fn>
  auto and_then(Fn&& fn) const& { return storage_.and_then(std::forward<Fn>(fn)); }
  template <class Fn>
  auto and_then(Fn&& fn) && { return std::move(storage_).and_then(std::forward<Fn>(fn)); }
  template <class Fn>
  auto transform(Fn&& fn) const& { return storage_.transform(std::forward<Fn>(fn)); }
  template <class Fn>
  auto transform(Fn&& fn) && { return std::move(storage_).transform(std::forward<Fn>(fn)); }
  template <class Fn>
  auto or_else(Fn&& fn) const& { return storage_.or_else(std::forward<Fn>(fn)); }
  template <class Fn>
  auto or_else(Fn&& fn) && { return std::move(storage_).or_else(std::forward<Fn>(fn)); }
  template <class Fn>
  auto transform_error(Fn&& fn) const& { return storage_.transform_error(std::forward<Fn>(fn)); }

  /// 底层 `std::expected`（需要完整标准接口时用，如 `emplace`/`swap`）。
  [[nodiscard]] auto expected() const& noexcept -> const storage_type& { return storage_; }
  [[nodiscard]] auto expected() & noexcept -> storage_type& { return storage_; }
  [[nodiscard]] auto expected() && noexcept -> storage_type&& { return std::move(storage_); }

 private:
  storage_type storage_;
};

/// 无值结果（`std::expected<void, Error>` 的薄门面，与主模板同口径）。
///
/// 为何不用 `std::expected<void,Error>` 直接当作 `Status`：
/// `Status` 在仓内有 **3000+** 处使用，“成功”是一等公民（`return ok()`），
/// 而 `std::expected<void,E>` 的成功态是 `std::in_place`——这层别名会渗到每个调用点。
/// 本类保持“缺省即成功”的语义，同时提供 `and_then`/`transform` 与 `expected()` 出口。
template <>
class Result<void> {
 public:
  using value_type = void;
  using storage_type = std::expected<void, Error>;

  Result() noexcept = default;
  Result(Err failure) : storage_(std::unexpect, std::move(failure.error)) {}  // lint-allow: L10 隐式构造

  [[nodiscard]] auto has_value() const noexcept -> bool { return storage_.has_value(); }
  explicit operator bool() const noexcept { return storage_.has_value(); }
  /// 前置条件：`!has_value()`。
  [[nodiscard]] auto error() const noexcept -> const Error& { return storage_.error(); }

  /// monadic 接口（与主模板同口径，直接转发）。
  template <class Fn>
  auto and_then(Fn&& fn) const& { return storage_.and_then(std::forward<Fn>(fn)); }
  template <class Fn>
  auto transform(Fn&& fn) const& { return storage_.transform(std::forward<Fn>(fn)); }
  template <class Fn>
  auto or_else(Fn&& fn) const& { return storage_.or_else(std::forward<Fn>(fn)); }
  template <class Fn>
  auto transform_error(Fn&& fn) const& { return storage_.transform_error(std::forward<Fn>(fn)); }

  /// 底层 `std::expected<void, Error>`。
  [[nodiscard]] auto expected() const& noexcept -> const storage_type& { return storage_; }
  [[nodiscard]] auto expected() & noexcept -> storage_type& { return storage_; }

 private:
  storage_type storage_{};
};

using Status = Result<void>;

/// 成功（无值）。
[[nodiscard]] inline auto ok() noexcept -> Status { return Status{}; }

}  // namespace st
