#pragma once

/// `st.pkg` 清单（JSON，`DESIGN.md` §7.2）：模块划分、源文件 glob、构建目标与三类依赖。
/// 约定：
/// - `directory` 是**运行时上下文**（清单所在目录的绝对路径），用于展开 glob 与解析相对源地址，
///   不作为清单字段写出；
/// - `sources` / `tests` 支持 `st::fs::expand_glob` 通配（`*`、`?`、`**`），展开结果为绝对路径；
/// - 依赖分三类：`dependencies`（第三方源码，多来源）、`dependency_modules`（同仓模块）、
///   `dependency_system`（系统链接项：`dl`/`pthread`/`m`，非第三方源码）；
/// - `extra_fields` 保存本层未识别的顶层字段，`to_json` 原样回写（与真实清单互操作时不丢字段）。

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/json.hpp"

namespace st::pkg {

/// 依赖来源规格（`Path` 本地目录 / `Http` 明文 tar.gz / `GitTarball` 本地归档或已解包目录 /
/// `Registry` 索引地址：`file://` 或 `http://`）。
struct SourceSpec {
  enum class Kind : std::uint8_t { Path, Http, GitTarball, Registry };

  Kind kind{Kind::Path};
  std::string location{};  ///< Path: 本地目录；Http: URL；GitTarball: 本地归档/目录；Registry: 索引地址
  std::string sha256{};    ///< 期望校验和（空 = 不校验；目录源按 `st::hash::tree_fingerprint` 语义）

  /// 是否为本地目录形态的来源（`Path`，或 `GitTarball` 指向已解包目录）。
  [[nodiscard]] auto is_directory_source() const noexcept -> bool {
    return kind == Kind::Path || kind == Kind::GitTarball;
  }
};

/// 来源类型的稳定短名（清单/锁文件/日志序列化用）。
[[nodiscard]] auto source_kind_name(SourceSpec::Kind kind) -> std::string_view;
/// 解析来源类型短名；未知名称返回 `Parse`。
[[nodiscard]] auto parse_source_kind(std::string_view text) -> Result<SourceSpec::Kind>;
/// 来源规格 → JSON 对象（`{"kind","location","sha256"}`）。
[[nodiscard]] auto source_to_json(const SourceSpec& spec) -> st::Value;
/// 从对象解析来源规格：支持 `{"kind","location"|"url","sha256"}` 平铺形式，
/// 以及 `{"source":{...},"sha256":"..."}` 嵌套形式（嵌套缺 `sha256` 时取外层）。
[[nodiscard]] auto source_spec_from_object(const st::Value& json) -> Result<SourceSpec>;

/// 第三方源码依赖项。
struct DependencySpec {
  std::string name{};
  std::string version_req{"*"};  ///< 版本约束文本（见 `st::pkg::VersionReq`；前缀 `?` 表示可选依赖）
  SourceSpec source{};
};

/// 构建目标（可执行/库）。
struct TargetSpec {
  std::string name{};
  std::string kind{"executable"};
  std::vector<std::string> sources{};
  std::vector<std::string> flags{};
  /// 从库源中排除的 glob（如工具链专属的 `src/pkg/*`——应用不需要，排除后少编译若干翻译单元）。
  std::vector<std::string> exclude_sources{};
};

/// `st.pkg` 清单。
struct Manifest {
  std::string name{};
  std::string version{"0.1.0"};
  std::string kind{"static_library"};
  std::string directory{};  ///< 清单所在目录（绝对路径，供源/包含路径解析）
  std::vector<std::string> modules{};
  std::vector<std::string> include_dirs{};
  std::vector<std::string> sources{};  ///< 支持 glob（`src/**/*.cpp`）
  std::vector<std::string> tests{};
  std::vector<std::string> flags{};
  std::vector<std::string> defines{};
  std::vector<std::string> system_libs{};
  std::vector<TargetSpec> targets{};
  std::vector<DependencySpec> dependencies{};     ///< 第三方源码依赖
  std::vector<std::string> dependency_modules{};  ///< dependencies.modules
  std::vector<std::string> dependency_system{};   ///< dependencies.system
  st::json::Object extra_fields{};                ///< 未识别顶层字段（回写保留，互操作用）

  /// 查目标（不存在返回 nullptr；返回指针非拥有，生命周期同本清单）。
  [[nodiscard]] auto find_target(std::string_view target_name) const -> const TargetSpec*;
  /// 展开 `sources` 的 glob，返回绝对路径列表（按模式顺序 + 模式内字典序，跨模式去重）。
  /// 错误：`Invalid`（缺 `directory`）、`NotFound`（目录不存在）、`Io`。
  [[nodiscard]] auto source_files() const -> Result<std::vector<std::string>>;
  /// 展开 `tests` 的 glob（语义同 `source_files`）。
  [[nodiscard]] auto test_files() const -> Result<std::vector<std::string>>;
  /// 解析清单 JSON；`directory` 为清单所在目录（绝对路径）。错误：`Parse`。
  static auto parse_json(const st::Value& json, std::string_view directory) -> Result<Manifest>;
  /// 读取 `st.pkg` 文件（`path` 为文件路径）。错误：`NotFound`/`Io`/`Parse`。
  static auto load(std::string_view path) -> Result<Manifest>;
  /// 在目录里查找 `st.pkg` 并读取。错误：`NotFound`（无清单）/`Io`/`Parse`。
  static auto find(std::string_view directory) -> Result<Manifest>;
  /// 序列化为清单 JSON（含 `extra_fields`；不写出 `directory`）。
  [[nodiscard]] auto to_json() const -> st::Value;
  /// 写出清单文件（pretty JSON）。错误：`Io`。
  static auto write(std::string_view path, const Manifest& manifest) -> Status;
};

}  // namespace st::pkg
