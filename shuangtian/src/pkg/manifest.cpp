#include "st/pkg/manifest.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/string.hpp"
#include "st/pkg/semver.hpp"

namespace st::pkg {
namespace {

/// 本层已识别的顶层字段（其余进 `extra_fields`，回写时保留）。
inline constexpr std::array<std::string_view, 15> known_keys{
    "name",       "version",     "kind",        "modules",        "include_dirs",
    "sources",    "tests",       "flags",       "defines",        "system_libs",
    "targets",    "dependencies", "dependency_modules", "dependency_system",
    "shuangtian_pkg_format"};

[[nodiscard]] auto known_key(std::string_view key) -> bool {
  for (const auto candidate : known_keys) {
    if (candidate == key) return true;
  }
  return false;
}

[[nodiscard]] auto to_array(const std::vector<std::string>& items) -> st::Value {
  st::Value array = st::Value::array();
  for (const auto& item : items) array.push(st::Value(item));
  return array;
}

[[nodiscard]] auto parse_target(const std::string& name, const st::Value& json) -> Result<TargetSpec> {
  if (!json.is_object()) {
    return unexpected(ErrorCode::Parse, std::format("目标 {} 必须是 JSON 对象", name));
  }
  TargetSpec target;
  target.name = name;
  target.kind = json.get_string("kind", "executable");
  target.sources = json.get_string_array("sources");
  target.flags = json.get_string_array("flags");
  target.exclude_sources = json.get_string_array("exclude_sources");
  return target;
}

/// 版本约束文本：前缀 `?` 表示可选依赖（求解器语义），校验时剥离。
[[nodiscard]] auto validate_version_req(std::string_view text, std::string_view name) -> Status {
  std::string_view body = st::trim(text);
  if (!body.empty() && body.front() == '?') body = st::trim(body.substr(1));
  if (body.empty()) return ok();
  const auto parsed = VersionReq::parse(body);
  if (!parsed) {
    return unexpected(parsed.error().code,
                      std::format("依赖 {} 的版本约束非法（{}）：{}", name, body,
                                  parsed.error().message));
  }
  return ok();
}

[[nodiscard]] auto parse_dependency(const st::Value& json) -> Result<DependencySpec> {
  DependencySpec spec;
  if (json.is_string()) {
    const std::string text = json.as_string();
    const std::size_t at = text.rfind('@');
    if (at == std::string::npos || at == 0) {
      spec.name = text;
    } else {
      spec.name = text.substr(0, at);
      spec.version_req = text.substr(at + 1);
    }
  } else if (json.is_object()) {
    spec.name = json.get_string("name");
    spec.version_req = json.get_string("version", "*");
    const auto source = source_spec_from_object(json);
    if (!source) return forward_error(source.error());
    spec.source = *source;
  } else {
    return unexpected(ErrorCode::Parse, "依赖项必须是对象或 \"name@req\" 字符串");
  }
  if (spec.name.empty()) return unexpected(ErrorCode::Parse, "依赖项缺少 name");
  if (spec.version_req.empty()) spec.version_req = "*";
  if (const Status valid = validate_version_req(spec.version_req, spec.name); !valid) {
    return forward_error(valid.error());
  }
  return spec;
}

[[nodiscard]] auto expand_patterns(const std::string& directory,
                                   const std::vector<std::string>& patterns,
                                   std::string_view field) -> Result<std::vector<std::string>> {
  if (patterns.empty()) return std::vector<std::string>{};
  if (directory.empty()) {
    return unexpected(ErrorCode::Invalid,
                      std::format("清单缺少 directory，无法展开 {} 的 glob", field));
  }
  std::vector<std::string> files;
  std::set<std::string> seen;
  for (const auto& pattern : patterns) {
    const auto matched = st::fs::expand_glob(directory, pattern);
    if (!matched) return forward_error(matched.error());
    for (const auto& relative : *matched) {
      if (!seen.insert(relative).second) continue;
      files.push_back(st::fs::join(directory, relative));
    }
  }
  return files;
}

}  // namespace

auto source_kind_name(SourceSpec::Kind kind) -> std::string_view {
  switch (kind) {
    case SourceSpec::Kind::Path: return "path";
    case SourceSpec::Kind::Http: return "http";
    case SourceSpec::Kind::GitTarball: return "git-tarball";
    case SourceSpec::Kind::Registry: return "registry";
  }
  return "path";
}

auto parse_source_kind(std::string_view text) -> Result<SourceSpec::Kind> {
  const std::string lowered = st::ascii_lower(st::trim(text));
  if (lowered == "path" || lowered == "local" || lowered == "dir" || lowered == "directory") {
    return SourceSpec::Kind::Path;
  }
  if (lowered == "http" || lowered == "https" || lowered == "url") return SourceSpec::Kind::Http;
  if (lowered == "git-tarball" || lowered == "git_tarball" || lowered == "tarball" ||
      lowered == "git" || lowered == "archive") {
    return SourceSpec::Kind::GitTarball;
  }
  if (lowered == "registry" || lowered == "index") return SourceSpec::Kind::Registry;
  return unexpected(ErrorCode::Parse, std::format("未知依赖来源类型: {}", std::string(text)));
}

auto source_to_json(const SourceSpec& spec) -> st::Value {
  st::Value json = st::Value::object();
  json.set("kind", st::Value(source_kind_name(spec.kind)));
  json.set("location", st::Value(spec.location));
  if (!spec.sha256.empty()) json.set("sha256", st::Value(spec.sha256));
  return json;
}

auto source_spec_from_object(const st::Value& json) -> Result<SourceSpec> {
  if (!json.is_object()) return unexpected(ErrorCode::Parse, "依赖来源必须是 JSON 对象");
  SourceSpec spec;
  const st::Value* nested = json.find("source");
  const st::Value& outer_sha256 = json.at("sha256");
  if (nested != nullptr && nested->is_object()) {
    const auto kind = parse_source_kind(nested->get_string("kind", "path"));
    if (!kind) return forward_error(kind.error());
    spec.kind = *kind;
    spec.location = nested->get_string("location", nested->get_string("url"));
    spec.sha256 = nested->get_string("sha256");
    if (spec.sha256.empty()) spec.sha256 = outer_sha256.as_string();
    return spec;
  }
  if (nested != nullptr && !nested->is_null()) {
    return unexpected(ErrorCode::Parse, "source 字段必须是 JSON 对象");
  }
  const auto kind = parse_source_kind(json.get_string("kind", "path"));
  if (!kind) return forward_error(kind.error());
  spec.kind = *kind;
  spec.location = json.get_string("location");
  if (spec.location.empty()) spec.location = json.get_string("url");
  spec.sha256 = json.get_string("sha256");
  return spec;
}

auto Manifest::find_target(std::string_view target_name) const -> const TargetSpec* {
  for (const auto& target : targets) {
    if (target.name == target_name) return &target;
  }
  return nullptr;
}

auto Manifest::source_files() const -> Result<std::vector<std::string>> {
  return expand_patterns(directory, sources, "sources");
}

auto Manifest::test_files() const -> Result<std::vector<std::string>> {
  return expand_patterns(directory, tests, "tests");
}

auto Manifest::parse_json(const st::Value& json, std::string_view directory) -> Result<Manifest> {
  if (!json.is_object()) return unexpected(ErrorCode::Parse, "清单根节点必须是 JSON 对象");

  Manifest manifest;
  manifest.name = json.get_string("name");
  if (manifest.name.empty()) return unexpected(ErrorCode::Parse, "清单缺少 name 字段");
  manifest.version = json.get_string("version", "0.1.0");
  if (manifest.version.empty()) manifest.version = "0.1.0";
  const auto version = Version::parse(manifest.version);
  if (!version) {
    return unexpected(ErrorCode::Parse, std::format("清单 version 非法（{}）：{}", manifest.version,
                                                    version.error().message));
  }
  manifest.kind = json.get_string("kind", "static_library");
  if (manifest.kind.empty()) manifest.kind = "static_library";
  manifest.directory = std::string(directory);
  manifest.modules = json.get_string_array("modules");
  manifest.include_dirs = json.get_string_array("include_dirs");
  manifest.sources = json.get_string_array("sources");
  manifest.tests = json.get_string_array("tests");
  manifest.flags = json.get_string_array("flags");
  manifest.defines = json.get_string_array("defines");
  manifest.system_libs = json.get_string_array("system_libs");

  const st::Value& targets = json.at("targets");
  if (!targets.is_null()) {
    if (!targets.is_object()) return unexpected(ErrorCode::Parse, "targets 必须是 JSON 对象");
    for (const auto& [name, value] : targets.fields()) {
      const auto target = parse_target(name, value);
      if (!target) return forward_error(target.error());
      manifest.targets.push_back(*target);
    }
  }

  const st::Value& dependencies = json.at("dependencies");
  const st::Value* source_deps = nullptr;
  if (dependencies.is_array()) {
    source_deps = &dependencies;
  } else if (dependencies.is_object()) {
    manifest.dependency_modules = dependencies.get_string_array("modules");
    manifest.dependency_system = dependencies.get_string_array("system");
    source_deps = dependencies.find("source");
    if (source_deps != nullptr && !source_deps->is_array()) {
      if (source_deps->is_object()) {
        const auto single = parse_dependency(*source_deps);
        if (!single) return forward_error(single.error());
        manifest.dependencies.push_back(*single);
        source_deps = nullptr;
      } else if (!source_deps->is_null()) {
        return unexpected(ErrorCode::Parse, "dependencies.source 必须是数组或对象");
      } else {
        source_deps = nullptr;
      }
    }
  } else if (!dependencies.is_null()) {
    return unexpected(ErrorCode::Parse, "dependencies 必须是对象或数组");
  }

  if (source_deps != nullptr) {
    for (const auto& item : source_deps->items()) {
      const auto dependency = parse_dependency(item);
      if (!dependency) return forward_error(dependency.error());
      manifest.dependencies.push_back(*dependency);
    }
  }
  // 顶层兼容写法
  if (manifest.dependency_modules.empty()) {
    manifest.dependency_modules = json.get_string_array("dependency_modules");
  }
  if (manifest.dependency_system.empty()) {
    manifest.dependency_system = json.get_string_array("dependency_system");
  }

  for (const auto& [key, value] : json.fields()) {
    if (known_key(key)) continue;
    manifest.extra_fields.emplace_back(key, value);
  }
  return manifest;
}

auto Manifest::load(std::string_view path) -> Result<Manifest> {
  const auto text = st::fs::read_text(path);
  if (!text) return forward_error(text.error());
  const auto json = st::json::parse(*text);
  if (!json) {
    return unexpected(json.error().code,
                      std::format("清单解析失败（{}）：{}", std::string(path),
                                  json.error().message));
  }
  std::string directory = st::fs::parent(path);
  if (directory.empty()) directory = ".";
  const auto absolute = st::fs::absolute(directory);
  if (!absolute) return forward_error(absolute.error());
  return parse_json(*json, *absolute);
}

auto Manifest::find(std::string_view directory) -> Result<Manifest> {
  const std::string candidate = st::fs::join(directory, "st.pkg");
  if (!st::fs::is_regular_file(candidate)) {
    return unexpected(ErrorCode::NotFound,
                      std::format("目录下未找到 st.pkg: {}", std::string(directory)));
  }
  return load(candidate);
}

auto Manifest::to_json() const -> st::Value {
  st::Value json = st::Value::object();
  json.set("name", st::Value(name));
  json.set("version", st::Value(version));
  json.set("kind", st::Value(kind));
  json.set("modules", to_array(modules));
  json.set("include_dirs", to_array(include_dirs));
  json.set("sources", to_array(sources));
  json.set("tests", to_array(tests));
  json.set("flags", to_array(flags));
  json.set("defines", to_array(defines));
  json.set("system_libs", to_array(system_libs));

  st::Value target_map = st::Value::object();
  for (const auto& target : targets) {
    st::Value entry = st::Value::object();
    entry.set("kind", st::Value(target.kind));
    entry.set("sources", to_array(target.sources));
    if (!target.flags.empty()) entry.set("flags", to_array(target.flags));
    target_map.set(target.name, entry);
  }
  json.set("targets", target_map);

  st::Value dependency_map = st::Value::object();
  dependency_map.set("modules", to_array(dependency_modules));
  st::Value source_deps = st::Value::array();
  for (const auto& dependency : dependencies) {
    st::Value entry = st::Value::object();
    entry.set("name", st::Value(dependency.name));
    entry.set("version", st::Value(dependency.version_req));
    entry.set("source", source_to_json(dependency.source));
    source_deps.push(entry);
  }
  dependency_map.set("source", source_deps);
  dependency_map.set("system", to_array(dependency_system));
  json.set("dependencies", dependency_map);

  for (const auto& [key, value] : extra_fields) {
    if (known_key(key)) continue;
    json.set(key, value);
  }
  return json;
}

auto Manifest::write(std::string_view path, const Manifest& manifest) -> Status {
  return st::json::write_file(path, manifest.to_json(), true);
}

}  // namespace st::pkg
