#include "st/pkg/fetch.hpp"

#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "pkg_internal.hpp"
#include "st/core/error.hpp"
#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/ext/json.hpp"
#include "st/core/string.hpp"
#include "st/pkg/manifest.hpp"
#include "st/pkg/registry.hpp"

namespace st::pkg::detail {

auto resolve_location(std::string_view location, std::string_view base) -> std::string {
  if (location.empty()) return {};
  if (st::fs::is_absolute(location)) return st::fs::normalize(location);
  std::string root(base);
  if (root.empty()) {
    const auto current = st::fs::current_dir();
    root = current ? *current : std::string(".");
  }
  return st::fs::normalize(st::fs::join(root, location));
}

auto copy_tree_stable(std::string_view from, std::string_view to) -> Status {
  if (!st::fs::is_directory(from)) {
    return unexpected(ErrorCode::NotFound,
                      std::string("复制源不是目录: ").append(from));
  }
  if (auto created = st::fs::create_directories(to); !created) return created;
  const auto entries = st::fs::walk(from);
  if (!entries) return forward_error(entries.error());
  for (const auto& entry : *entries) {
    const std::string source = st::fs::join(from, entry.path);
    const std::string target = st::fs::join(to, entry.path);
    if (entry.is_dir) {
      if (auto created = st::fs::create_directories(target); !created) return created;
      continue;
    }
    if (auto copied = st::fs::copy_file(source, target); !copied) return copied;
    // 保留 mtime：tree_fingerprint（路径+大小+mtime）跨副本稳定，缓存/vendor 固化后仍可校验同一 sha256。
    std::error_code error;
    const std::filesystem::file_time_type stamp =
        std::filesystem::last_write_time(std::filesystem::path(source), error);
    if (error) {
      return unexpected(ErrorCode::Io, std::string("读取 mtime 失败: ").append(source));
    }
    std::filesystem::last_write_time(std::filesystem::path(target), stamp, error);
    if (error) {
      return unexpected(ErrorCode::Io, std::string("保留 mtime 失败: ").append(target));
    }
  }
  return ok();
}

}  // namespace st::pkg::detail

namespace st::pkg {
namespace {

[[nodiscard]] auto with_source(const ResolvedPackage& package, const SourceSpec& spec,
                               const std::string& sha256) -> ResolvedPackage {
  ResolvedPackage copy = package;
  copy.source = spec;
  if (!sha256.empty()) copy.sha256 = sha256;
  return copy;
}

[[nodiscard]] auto cache_entry(const FetchOptions& options, std::string_view key) -> std::string {
  return st::fs::join(options.cache_dir, key);
}

[[nodiscard]] auto cache_src(const FetchOptions& options, std::string_view key) -> std::string {
  return st::fs::join(cache_entry(options, key), "src");
}

[[nodiscard]] auto cache_ready(const std::string& entry) -> bool {
  return st::fs::is_directory(st::fs::join(entry, "src")) &&
         st::fs::is_regular_file(st::fs::join(entry, "origin.json"));
}

[[nodiscard]] auto write_origin(const std::string& entry, const SourceSpec& spec,
                                const std::string& key, const std::string& origin) -> Status {
  st::Json json = st::Json::object();
  json["key"] = key;
  json["kind"] = std::string(source_kind_name(spec.kind));
  json["location"] = spec.location;
  json["origin"] = origin;
  json["sha256"] = key;
  return json_write_file(st::fs::join(entry, "origin.json"), json, true);
}

/// 归档扩展名（用于给出精确的「待 codec」提示）。
[[nodiscard]] auto archive_kind(std::string_view path) -> std::string_view {
  const std::string lowered = st::ascii_lower(path);
  if (lowered.ends_with(".tar.gz") || lowered.ends_with(".tgz")) return "tar.gz";
  if (lowered.ends_with(".tar.xz")) return "tar.xz";
  if (lowered.ends_with(".zip")) return "zip";
  if (lowered.ends_with(".tar")) return "tar";
  return "归档";
}

/// `registry` 来源 → 具体来源（读索引取该包该版本的发布信息）。
[[nodiscard]] auto resolve_registry_source(const ResolvedPackage& package, const SourceSpec& index)
    -> Result<SourceSpec> {
  if (index.location.empty()) {
    return unexpected(ErrorCode::Invalid,
                      std::format("依赖 {} 的 registry 来源缺少索引地址", package.name));
  }
  const auto registry = Registry::load(index.location);
  if (!registry) return forward_error(registry.error());
  const PackageRelease* release = registry->release(package.name, package.version);
  if (release == nullptr) {
    return unexpected(ErrorCode::NotFound,
                      std::format("索引 {} 中没有 {} {}", index.location, package.name,
                                  package.version.to_string()));
  }
  SourceSpec spec = release->source;
  if (spec.sha256.empty()) {
    spec.sha256 = package.sha256.empty() ? release->sha256 : package.sha256;
  }
  return spec;
}

/// 归档文件（尚未解包）：校验字节 SHA-256、落缓存，然后明确报「待 codec」。
[[nodiscard]] auto archive_unpack_pending(const ResolvedPackage& package, const SourceSpec& spec,
                                          const std::string& archive_path,
                                          const FetchOptions& options) -> Result<FetchedPackage> {
  const auto digest = st::hash::sha256_file(archive_path);
  if (!digest) return forward_error(digest.error());
  if (!spec.sha256.empty() && spec.sha256 != *digest) {
    return unexpected(ErrorCode::Parse,
                      std::format("依赖 {} 归档校验和不匹配：期望 {}，实际 {}（{}）", package.name,
                                  spec.sha256, *digest, archive_path));
  }
  const std::string key = spec.sha256.empty() ? *digest : spec.sha256;
  std::string cached;
  if (!options.cache_dir.empty()) {
    const std::string entry = cache_entry(options, key);
    const std::string target = st::fs::join(entry, "archive.bin");
    if (!st::fs::is_regular_file(target)) {
      if (auto created = st::fs::create_directories(entry); !created) {
        return forward_error(created.error());
      }
      if (auto copied = st::fs::copy_file(archive_path, target); !copied) {
        return forward_error(copied.error());
      }
      if (auto origin = write_origin(entry, spec, key, archive_path); !origin) {
        return forward_error(origin.error());
      }
    }
    cached = std::format("，归档字节已缓存于 {}", target);
  }
  return unexpected(ErrorCode::Unsupported,
                    std::format("归档解包由 codec 层提供（并行开发中）：依赖 {} 的 {} 归档 {} 已校验 "
                                "sha256={}{}；请改用 path 源（已解包目录）",
                                package.name, std::string(archive_kind(archive_path)), archive_path,
                                key, cached));
}

/// 本地目录/归档来源（`path`、`git-tarball`）。
[[nodiscard]] auto fetch_local(const ResolvedPackage& package, const SourceSpec& spec,
                               const FetchOptions& options) -> Result<FetchedPackage> {
  const std::string source_path = detail::resolve_location(spec.location, options.work_dir);
  if (source_path.empty()) {
    return unexpected(ErrorCode::Invalid, std::format("依赖 {} 的路径来源为空", package.name));
  }
  if (!st::fs::exists(source_path)) {
    // 源不存在但给了 sha256 且缓存就绪 → 离线复用（vendor/缓存固化后的可复现构建）。
    if (!spec.sha256.empty() && !options.cache_dir.empty()) {
      const std::string entry = cache_entry(options, spec.sha256);
      if (cache_ready(entry)) {
        FetchedPackage fetched;
        fetched.package = with_source(package, spec, spec.sha256);
        fetched.directory = cache_src(options, spec.sha256);
        fetched.sha256 = spec.sha256;
        return fetched;
      }
    }
    return unexpected(ErrorCode::NotFound,
                      std::format("依赖源不存在: {}（依赖 {}）", source_path, package.name));
  }
  if (!st::fs::is_directory(source_path)) {
    return archive_unpack_pending(package, spec, source_path, options);
  }

  const auto fingerprint = st::hash::tree_fingerprint(source_path);
  if (!fingerprint) {
    return unexpected(fingerprint.error().code,
                      std::format("无法计算依赖 {} 的源码树指纹（{}）：{}", package.name, source_path,
                                  fingerprint.error().message));
  }
  if (!spec.sha256.empty() && spec.sha256 != *fingerprint) {
    return unexpected(ErrorCode::Parse,
                      std::format("依赖 {} 校验和不匹配（目录树指纹）：期望 {}，实际 {}（{}）",
                                  package.name, spec.sha256, *fingerprint, source_path));
  }
  const std::string key = spec.sha256.empty() ? *fingerprint : spec.sha256;

  FetchedPackage fetched;
  fetched.package = with_source(package, spec, key);
  fetched.sha256 = key;
  if (options.cache_dir.empty()) {
    fetched.directory = source_path;  // 无缓存：直接引用原目录
    return fetched;
  }

  const std::string entry = cache_entry(options, key);
  const std::string target = cache_src(options, key);
  if (cache_ready(entry)) {
    const auto cached_fingerprint = st::hash::tree_fingerprint(target);
    if (cached_fingerprint && *cached_fingerprint == key) {  // 命中：内容寻址，无需再读源
      fetched.directory = target;
      return fetched;
    }
    if (auto removed = st::fs::remove_all(entry); !removed) return forward_error(removed.error());
  } else if (st::fs::exists(entry)) {
    if (auto removed = st::fs::remove_all(entry); !removed) return forward_error(removed.error());
  }
  if (auto created = st::fs::create_directories(entry); !created) return forward_error(created.error());
  if (auto copied = detail::copy_tree_stable(source_path, target); !copied) {
    return forward_error(copied.error());
  }
  if (auto origin = write_origin(entry, spec, key, source_path); !origin) {
    return forward_error(origin.error());
  }
  fetched.directory = target;
  return fetched;
}

/// 明文 http 来源（下载 + 校验 + 缓存；解包待 codec）。
[[nodiscard]] auto fetch_http(const ResolvedPackage& package, const SourceSpec& spec,
                              const FetchOptions& options) -> Result<FetchedPackage> {
  const std::string key = spec.sha256;
  if (!key.empty() && !options.cache_dir.empty()) {
    const std::string cached = st::fs::join(cache_entry(options, key), "archive.bin");
    if (st::fs::is_regular_file(cached)) {  // 缓存命中：内容寻址，无需再下载
      return unexpected(ErrorCode::Unsupported,
                        std::format("归档解包由 codec 层提供（并行开发中）：依赖 {} 的归档已缓存（sha256={}，"
                                    "位于 {}），HTTP 源 {} 未再下载",
                                    package.name, key, cached, spec.location));
    }
  }
  if (options.offline || !options.allow_network) {
    return unexpected(ErrorCode::Unsupported,
                      std::format("离线模式（offline/allow_network=false）下无法下载依赖 {}（{}）："
                                  "请先在有网环境 fetch/vendor，或改用 path 源",
                                  package.name, spec.location));
  }
  const auto response = detail::http_get(spec.location, detail::HttpOptions{});
  if (!response) return forward_error(response.error());
  if (response->status != 200) {
    return unexpected(ErrorCode::Io, std::format("依赖 {} 下载失败（HTTP {}）：{}", package.name,
                                                 response->status, spec.location));
  }
  const std::string digest = st::hash::sha256_hex(response->body);
  if (!spec.sha256.empty() && spec.sha256 != digest) {
    return unexpected(ErrorCode::Parse,
                      std::format("依赖 {} 下载内容校验和不匹配：期望 {}，实际 {}（{}）", package.name,
                                  spec.sha256, digest, spec.location));
  }
  const std::string actual_key = spec.sha256.empty() ? digest : spec.sha256;
  std::string cached;
  if (!options.cache_dir.empty()) {
    const std::string entry = cache_entry(options, actual_key);
    const std::string target = st::fs::join(entry, "archive.bin");
    if (st::fs::exists(target)) {
      if (auto removed = st::fs::remove_all(entry); !removed) return forward_error(removed.error());
    }
    if (auto created = st::fs::create_directories(entry); !created) return forward_error(created.error());
    if (auto written = st::fs::write_bytes(target, response->body); !written) {
      return forward_error(written.error());
    }
    if (auto origin = write_origin(entry, spec, actual_key, spec.location); !origin) {
      return forward_error(origin.error());
    }
    cached = std::format("，归档字节已缓存于 {}", target);
  }
  return unexpected(ErrorCode::Unsupported,
                    std::format("归档解包由 codec 层提供（并行开发中）：依赖 {} 已从 {} 下载并校验 "
                                "sha256={}（{} 字节）{}；请改用 path 源（已解包目录）",
                                package.name, spec.location, actual_key, response->body.size(), cached));
}

}  // namespace

auto fetch_package(const ResolvedPackage& package, const FetchOptions& options)
    -> Result<FetchedPackage> {
  if (package.name.empty()) return unexpected(ErrorCode::Invalid, "依赖名为空");
  if (!detail::is_safe_package_name(package.name)) {
    return unexpected(ErrorCode::Invalid,
                      std::format("依赖名不能用作目录名（含路径分隔符或为相对段）: {}", package.name));
  }

  SourceSpec spec = package.source;
  if (spec.kind == SourceSpec::Kind::Registry) {
    const auto resolved = resolve_registry_source(package, spec);
    if (!resolved) return forward_error(resolved.error());
    spec = *resolved;
  }
  const Status tls = detail::reject_tls(spec.location);
  if (!tls) return forward_error(tls.error());

  switch (spec.kind) {
    case SourceSpec::Kind::Path:
    case SourceSpec::Kind::GitTarball:
      return fetch_local(package, spec, options);
    case SourceSpec::Kind::Http:
      return fetch_http(package, spec, options);
    case SourceSpec::Kind::Registry:
      break;  // 已在上方解析为具体来源
  }
  return unexpected(ErrorCode::Unsupported,
                    std::format("依赖 {} 的来源类型不受支持", package.name));
}

auto fetch_all(const ResolvedGraph& graph, const FetchOptions& options)
    -> Result<std::vector<FetchedPackage>> {
  std::vector<FetchedPackage> fetched;
  fetched.reserve(graph.packages.size());
  for (const auto& package : graph.packages) {
    const auto item = fetch_package(package, options);
    if (!item) {
      return unexpected(item.error().code,
                        std::format("获取依赖 {} 失败：{}", package.name, item.error().message));
    }
    fetched.push_back(*item);
  }
  return fetched;
}

auto vendor_packages(const std::vector<FetchedPackage>& packages, std::string_view vendor_root)
    -> Status {
  if (vendor_root.empty()) return unexpected(ErrorCode::Invalid, "vendor 根目录为空");
  if (auto created = st::fs::create_directories(vendor_root); !created) {
    return forward_error(created.error());
  }

  st::Json lock = st::Json::object();
  lock["format"] = 1;
  lock["vendor_root"] = std::string(vendor_root);
  st::Json items = st::Json::array();
  for (const auto& fetched : packages) {
    const std::string& name = fetched.package.name;
    if (!detail::is_safe_package_name(name)) {
      return unexpected(ErrorCode::Invalid, std::format("依赖名不能用作 vendor 目录名: {}", name));
    }
    if (fetched.directory.empty() || !st::fs::is_directory(fetched.directory)) {
      return unexpected(ErrorCode::NotFound,
                        std::format("依赖 {} 的源码目录不可用: {}", name, fetched.directory));
    }
    const std::string destination = st::fs::join(vendor_root, name);
    if (st::fs::exists(destination)) {
      if (auto removed = st::fs::remove_all(destination); !removed) {
        return forward_error(removed.error());
      }
    }
    if (auto copied = detail::copy_tree_stable(fetched.directory, destination); !copied) {
      return forward_error(copied.error());
    }

    st::Json item = st::Json::object();
    item["name"] = name;
    item["version"] = fetched.package.version.to_string();
    item["source"] = source_to_json(fetched.package.source);
    item["sha256"] = fetched.sha256.empty() ? fetched.package.sha256 : fetched.sha256;
    item["directory"] = name;
    st::Json dependencies = st::Json::array();
    for (const auto& dependency : fetched.package.dependencies) {
      dependencies.push_back(dependency);
    }
    item["dependencies"] = dependencies;
    items.push_back(item);
  }
  lock["packages"] = items;
  return json_write_file(st::fs::join(vendor_root, "vendor.lock"), lock, true);
}

auto lock_write(std::string_view path, const ResolvedGraph& graph) -> Status {
  return json_write_file(path, graph_to_json(graph), true);
}

auto lock_read(std::string_view path) -> Result<ResolvedGraph> {
  const auto text = st::fs::read_text(path);
  if (!text) return forward_error(text.error());
  const auto json = st::json_parse(*text);
  if (!json) {
    return unexpected(json.error().code, std::format("锁文件解析失败（{}）：{}", std::string(path),
                                                     json.error().message));
  }
  return graph_from_json(*json);
}

}  // namespace st::pkg
