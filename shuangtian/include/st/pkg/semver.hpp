#pragma once

/// 语义版本与版本约束（`DESIGN.md` §7.3）：SemVer 2.0 核心三元组 + 预发布 / 构建元数据。
/// 约束支持 `*`、`=1.2.3`、`^1.2.3`、`~1.2.3`、`>=`/`>`/`<`/`<=`，多段以空白或逗号分隔（AND 语义）。
/// 约定：
/// - 预发布版本排序低于同号正式版（`1.0.0-alpha < 1.0.0`）；构建元数据不参与比较；
/// - `matches` 取 npm 口径：约束自身不含预发布标识时，预发布候选一律不匹配（避免误选 alpha 版），
///   唯一例外是 `*`（Any）——它匹配包括预发布在内的任意版本；
/// - 版本文本的缺省段补 0（`1` → `1.0.0`、`1.2` → `1.2.0`），裸版本号等价于 `=`（精确匹配）。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::pkg {

/// 语义版本（`major.minor.patch[-pre][+build]`）。
struct Version {
  std::uint32_t major{0};
  std::uint32_t minor{0};
  std::uint32_t patch{0};
  std::string pre{};    ///< 预发布标识（如 `beta.1`），空表示正式版
  std::string build{};  ///< 构建元数据（如 `sha.abc`），不参与比较

  /// 解析版本文本（允许缺省段：`1` / `1.2` / `1.2.3`，可选 `-pre`、`+build`）。
  /// 错误：`Parse` 格式非法；`Overflow` 数值段超出 `uint32_t`。
  static auto parse(std::string_view text) -> Result<Version>;

  /// 归一化文本（不保留原始前导零；含 pre/build）。
  [[nodiscard]] auto to_string() const -> std::string;
  /// 去掉预发布标识（保留构建元数据）的副本；用于区间上界比较。
  [[nodiscard]] auto without_pre() const -> Version;
  [[nodiscard]] auto is_prerelease() const noexcept -> bool { return !pre.empty(); }
};

/// 三方比较：`a < b` → -1，`a == b` → 0，`a > b` → 1（忽略 build；预发布 < 正式）。
[[nodiscard]] auto compare(const Version& a, const Version& b) -> int;

/// 单个约束段（如 `^1.2.3`）。
struct Comparator {
  enum class Op : std::uint8_t {
    Any,           ///< `*`
    Exact,         ///< `=1.2.3`
    Caret,         ///< `^1.2.3`（兼容到下一个非零段）
    Tilde,         ///< `~1.2.3`（同 minor 内）
    Greater,       ///< `>1.2.3`
    GreaterEqual,  ///< `>=1.2.3`
    Less,          ///< `<1.2.3`
    LessEqual,     ///< `<=1.2.3`
  };
  Op op{Op::Any};
  Version version{};
};

/// 版本约束（多段 = AND）。
struct VersionReq {
  std::vector<Comparator> comparators{};

  /// 解析约束文本（`*` / `=1.2.3` / `^1.2.3` / `~1.2.3` / `>=1.0.0 <2.0.0`，空白或逗号分隔多段）。
  /// 错误：`Parse` 空文本、比较器缺版本或版本非法；`Overflow` 版本数值段溢出。
  static auto parse(std::string_view text) -> Result<VersionReq>;
  /// 判定候选版本是否满足全部约束段；空 `comparators` 视为任意版本。
  [[nodiscard]] auto matches(const Version& version) const -> bool;
  /// 归一化文本（多段以空格连接）。
  [[nodiscard]] auto to_string() const -> std::string;
  /// 任意版本约束（`*`）。
  [[nodiscard]] static auto any() -> VersionReq;
};

/// 比较器运算符的文本短名（`*` / `=` / `^` / `~` / `>` / `>=` / `<` / `<=`），冲突链与日志使用。
[[nodiscard]] auto op_name(Comparator::Op op) -> std::string_view;

}  // namespace st::pkg
