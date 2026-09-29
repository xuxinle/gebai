#pragma once

/// 依赖索引与回溯求解（`DESIGN.md` §7.3）：索引加载 → 约束收集 → 回溯选版 → 锁图输出。
/// 约定：
/// - 索引是纯数据（`{"packages":[{name,version,source,sha256}]}`），来自 `file://` 本地或 `http://`；
/// - 求解器本身**不做 I/O**：传递依赖的清单由 `DependencyProvider` 注入（生产侧读依赖目录，测试可打桩）；
/// - 选版口径：为每个依赖选择「满足全部调用者约束的最高版本」；无解时返回 `Invalid`（缺包时为 `NotFound`）
///   并在 message 里给出冲突链（谁要求了什么区间、索引中有哪些版本、冲突点的已选版本）；
/// - 可选依赖：`DependencySpec::version_req` 以 `?` 开头（如 `?^1.2.0`）表示「能解则纳入，无解则跳过」。

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/ext/json.hpp"
#include "st/pkg/manifest.hpp"
#include "st/pkg/semver.hpp"

namespace st::pkg {

/// 索引中的一次发布。
struct PackageRelease {
  std::string name{};
  Version version{};
  SourceSpec source{};
  std::string sha256{};
};

/// 依赖索引（`index.json`）：按名字提供可用版本与来源。
class Registry {
 public:
  /// 从 `file:///path/index.json`、`http://host/index.json` 或本地路径加载索引。
  /// 错误：`Invalid`（地址为空）、`Unsupported`（`https://` 缺 TLS）、`NotFound`/`Io`、`Parse`（JSON 非法）。
  static auto load(std::string_view location) -> Result<Registry>;
  /// 解析索引 JSON（`location` 仅作为 `location()` 的返回值）。错误：`Parse`。
  static auto parse_json(const st::Json& json, std::string_view location) -> Result<Registry>;

  /// 某包的全部可用版本（降序；无该包返回空）。
  [[nodiscard]] auto versions(std::string_view name) const -> std::vector<Version>;
  /// 精确取一次发布（不存在返回 nullptr；返回指针非拥有，生命周期同本索引）。
  [[nodiscard]] auto release(std::string_view name, const Version& version) const
      -> const PackageRelease*;
  [[nodiscard]] auto has(std::string_view name) const -> bool;
  [[nodiscard]] auto location() const -> const std::string& { return location_; }
  /// 全部发布项（诊断/遍历用）。
  [[nodiscard]] auto releases() const -> const std::vector<PackageRelease>& { return releases_; }

 private:
  std::string location_{};
  std::vector<PackageRelease> releases_{};
};

/// 求解结果中的一个包（已定版 + 具体来源）。
struct ResolvedPackage {
  std::string name{};
  Version version{};
  SourceSpec source{};
  std::string sha256{};
  std::vector<std::string> dependencies{};  ///< 直接依赖名（已选定者）
};

/// 求解结果（拓扑序：依赖在前）。
struct ResolvedGraph {
  std::vector<ResolvedPackage> packages{};

  /// 按名查找（不存在返回 nullptr；返回指针非拥有，生命周期同本图）。
  [[nodiscard]] auto find(std::string_view name) const -> const ResolvedPackage*;
};

/// 传递依赖的清单来源（解耦：求解器不做 I/O，生产侧读依赖目录/缓存，测试注入桩）。
using DependencyProvider = std::function<Result<std::vector<DependencySpec>>(const ResolvedPackage&)>;

/// 回溯求解：为每个依赖选择满足全部约束的最高版本，返回拓扑序的锁定图。
/// 前置：`provider` 可调用；`registry` 覆盖远端包（无索引条目但给出显式来源的依赖可解出合成版本）。
/// 错误：`Invalid`（版本冲突/依赖环约束不自洽）、`NotFound`（索引无此包且无显式来源）、
///       或 `provider` 的原样错误（所有候选都读不出依赖清单时）。
auto resolve(const std::vector<DependencySpec>& roots, const Registry& registry,
             const DependencyProvider& provider) -> Result<ResolvedGraph>;

/// 锁定图 → JSON（`st.lock`，含 `lock_version` 与 `build_fingerprint`）。
[[nodiscard]] auto graph_to_json(const ResolvedGraph& graph) -> st::Json;
/// JSON → 锁定图（`build_fingerprint` 不参与读入，由 `graph_to_json` 按内容重算）。错误：`Parse`。
[[nodiscard]] auto graph_from_json(const st::Json& json) -> Result<ResolvedGraph>;

}  // namespace st::pkg
