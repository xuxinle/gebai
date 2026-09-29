#pragma once

/// 依赖获取 / 校验 / 缓存 / vendor 固化 / lockfile（`DESIGN.md` §7.4）。
/// 约定与本期边界：
/// - **来源**：`path`（本地已解包目录）、`git-tarball`（本地归档或已解包目录）、`http`（明文下载）、
///   `registry`（索引地址，fetch 内部先行解析为具体来源）；
/// - **校验**：`sha256` 非空时一律比对——目录源按 `st::hash::tree_fingerprint`（路径+大小+mtime，
///   构建缓存判据语义），归档/http 按字节 SHA-256；不匹配返回 `Parse` 且**拒用**；
/// - **缓存**：`{cache_dir}/{sha256|指纹}/src/`（内容寻址，命中即复用；`origin.json` 记录来源）；
///   `cache_dir` 为空则不缓存、目录源直接引用原目录；源目录消失但缓存可用时按 `sha256` 离线复用；
/// - **vendor 固化**：`vendor/<name>/` + `vendor.lock`，复制保留 mtime，使目录指纹跨副本稳定，
///   固化后离线可校验、可构建；
/// - **归档解包未实现**（`.tar`/`.tar.gz`/`.zip` 由并行开发的 `st::codec` 提供）：相关来源返回
///   `Unsupported` 并说明替代路径——归档字节仍会被校验并落入缓存，待 codec 落地即可接入。

#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/pkg/registry.hpp"

namespace st::pkg {

/// 获取选项。
struct FetchOptions {
  std::string cache_dir{};       ///< 缓存根（空 = 不缓存，目录源直接引用原目录）
  std::string work_dir{};        ///< 相对来源地址的解析基准（空 = 当前目录）
  bool offline{false};           ///< 离线：禁止网络（仅用本地目录与缓存）
  bool allow_network{true};      ///< 网络开关（false 等价于离线）
};

/// 一次成功获取的结果。
struct FetchedPackage {
  ResolvedPackage package{};  ///< 已定版依赖（`source` 为解析后的具体来源）
  std::string directory{};    ///< 本地源码目录（绝对路径）
  std::string sha256{};       ///< 实际校验和/指纹（缓存键）
};

/// 获取单个依赖：解析来源 → 校验 → 落缓存/引用本地目录。
/// 错误：`Invalid`（名字/地址非法）、`NotFound`（源不存在且缓存不可用）、`Parse`（校验和不匹配）、
///       `Unsupported`（https/离线不可达/归档解包待 codec）、`Io`/`Timeout`/`Overflow`（下载）。
[[nodiscard]] auto fetch_package(const ResolvedPackage& package, const FetchOptions& options)
    -> Result<FetchedPackage>;

/// 按图序（依赖在前）获取全部依赖；任一失败即返回（message 标注失败的包名）。
[[nodiscard]] auto fetch_all(const ResolvedGraph& graph, const FetchOptions& options)
    -> Result<std::vector<FetchedPackage>>;

/// 把已获取的依赖源码树固化到 `vendor/<name>/`，并在 `vendor_root` 写 `vendor.lock`（含来源/校验和/依赖）。
/// 前置：`packages` 的 `directory` 均为有效目录；`vendor_root` 非空。错误：`Invalid`（名字不安全）、
/// `NotFound`（源码目录不可用）、`Io`（复制/写文件失败）。
[[nodiscard]] auto vendor_packages(const std::vector<FetchedPackage>& packages,
                                   std::string_view vendor_root) -> Status;

/// 写 `st.lock`（JSON：`lock_version`/`build_fingerprint`/`packages[]`）。错误：`Io`。
[[nodiscard]] auto lock_write(std::string_view path, const ResolvedGraph& graph) -> Status;
/// 读 `st.lock`。错误：`NotFound`/`Io`、`Parse`（JSON 或版本非法）。
[[nodiscard]] auto lock_read(std::string_view path) -> Result<ResolvedGraph>;

}  // namespace st::pkg
