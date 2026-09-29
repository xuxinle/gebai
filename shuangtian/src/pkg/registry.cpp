#include "st/pkg/registry.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pkg_internal.hpp"
#include "st/core/error.hpp"
#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/core/json.hpp"
#include "st/core/string.hpp"
#include "st/pkg/manifest.hpp"
#include "st/pkg/semver.hpp"

namespace st::pkg {
namespace {

template <class T>
[[nodiscard]] auto contains(const std::vector<T>& items, const T& value) -> bool {
  return std::ranges::find(items, value) != items.end();
}

/// 版本按语义序找同值项（`Version` 不定义 `operator==`，避免语义型运算符重载滥用）。
[[nodiscard]] auto contains_version(const std::vector<Version>& items, const Version& version)
    -> bool {
  return std::ranges::find_if(items, [&version](const Version& item) {
           return compare(item, version) == 0;
         }) != items.end();
}

[[nodiscard]] auto body_to_text(const std::vector<std::uint8_t>& body) -> std::string {
  std::string text;
  text.reserve(body.size());
  for (const std::uint8_t byte : body) text.push_back(static_cast<char>(byte));
  return text;
}

// —— 求解器内部状态 ——

/// 一条需求：谁（`from`）在什么版本上要求了哪个区间。
struct Requirement {
  VersionReq req{};
  bool optional{false};
  bool active{true};
  bool has_source{false};
  SourceSpec source{};
  std::string from{};
};

/// 一个可选候选（版本 + 具体来源）。
struct Candidate {
  Version version{};
  SourceSpec source{};
  std::string sha256{};
};

/// 一次选定的完整记录（含撤销所需的全部分支副作用）。
struct Choice {
  Version version{};
  SourceSpec source{};
  std::string sha256{};
  std::vector<std::string> dependencies{};  ///< 直接依赖名（去重，按清单顺序）
  std::vector<std::string> reactivated{};   ///< 被本选择重新纳入求解的（原 skipped）包
  std::vector<std::pair<std::string, std::size_t>> added{};       ///< 本选择新增的需求位置
  std::vector<std::pair<std::string, std::size_t>> suppressed{};  ///< 本选择抑制的可选需求位置
};

/// 依赖文本 → 需求（剥离可选前缀 `?`）。错误：`Parse`（区间非法）。
[[nodiscard]] auto make_requirement(const DependencySpec& spec, std::string_view from)
    -> Result<Requirement> {
  Requirement requirement;
  std::string_view body = st::trim(spec.version_req);
  if (!body.empty() && body.front() == '?') {
    requirement.optional = true;
    body = st::trim(body.substr(1));
  }
  if (body.empty()) body = "*";
  const auto parsed = VersionReq::parse(body);
  if (!parsed) {
    return unexpected(parsed.error().code,
                      std::format("依赖 {} 的版本约束非法（{}）：{}", spec.name, body,
                                  parsed.error().message));
  }
  requirement.req = *parsed;
  requirement.source = spec.source;
  requirement.has_source = !spec.source.location.empty();
  requirement.from = std::string(from);
  return requirement;
}

/// 回溯求解器：约束收集 → 选最高可用版 → 传递依赖展开 → 冲突回溯。
class Solver {
 public:
  Solver(const Registry& registry, const DependencyProvider& provider)
      : registry_(registry), provider_(provider) {}

  auto run(const std::vector<DependencySpec>& roots) -> Result<ResolvedGraph> {
    for (const auto& spec : roots) {
      const auto requirement = make_requirement(spec, "根依赖");
      if (!requirement) return forward_error(requirement.error());
      root_names_.push_back(spec.name);
      if (!add_requirement(spec.name, *requirement, 0)) break;
    }
    if (!search(0)) return unexpected(error_code(), failure_message());
    return build();
  }

 private:
  // —— 约束管理 ——

  auto add_requirement(const std::string& name, Requirement requirement, std::size_t depth) -> bool {
    if (const auto found = chosen_.find(name); found != chosen_.end()) {
      if (requirement.req.matches(found->second.version)) return true;
      if (requirement.optional) return true;  // 可选依赖不满足即忽略
      record_version_conflict(
          depth, std::format("包 {} 已被选定为 {}，但 {} 要求 {}", name,
                             found->second.version.to_string(), requirement.from,
                             requirement.req.to_string()));
      return false;
    }
    if (skipped_.count(name) != 0) {
      if (requirement.optional) return true;
      skipped_.erase(name);  // 出现必需需求 → 重新纳入求解
    }
    constraints_[name].push_back(std::move(requirement));
    return true;
  }

  /// 抑制与本次选择不匹配的可选需求（可选依赖「能解则纳入，无解则跳过」）。
  void suppress_optional(const std::string& name) {
    const auto found = chosen_.find(name);
    if (found == chosen_.end()) return;
    std::vector<Requirement>& requirements = constraints_[name];
    for (std::size_t index = 0; index < requirements.size(); ++index) {
      Requirement& requirement = requirements[index];
      if (!requirement.active || !requirement.optional) continue;
      if (requirement.req.matches(found->second.version)) continue;
      requirement.active = false;
      chosen_[name].suppressed.emplace_back(name, index);
    }
  }

  void undo_choice(const std::string& name) {
    const auto found = chosen_.find(name);
    if (found == chosen_.end()) return;
    for (const auto& [dependency, index] : found->second.added) {
      const auto entry = constraints_.find(dependency);
      if (entry == constraints_.end() || index >= entry->second.size()) continue;
      entry->second[index].active = false;
    }
    for (const auto& reactivated : found->second.reactivated) skipped_.insert(reactivated);
    for (const auto& [target, index] : found->second.suppressed) {
      const auto entry = constraints_.find(target);
      if (entry == constraints_.end() || index >= entry->second.size()) continue;
      entry->second[index].active = true;
    }
    chosen_.erase(name);
  }

  // —— 搜索 ——

  auto search(std::size_t depth) -> bool {
    const std::string name = next_unresolved();
    if (name.empty()) return true;  // 全部解析完毕

    const std::vector<Candidate> candidates = candidate_list(name, depth);
    if (candidates.empty()) {
      if (!all_optional(name)) return false;  // 冲突已记录
      skipped_.insert(name);                  // 全可选且无可用版本 → 跳过该依赖
      if (search(depth)) return true;
      skipped_.erase(name);
      return false;
    }
    for (const Candidate& candidate : candidates) {
      if (try_candidate(name, candidate, depth)) return true;
    }
    return false;
  }

  auto try_candidate(const std::string& name, const Candidate& candidate, std::size_t depth) -> bool {
    Choice choice;
    choice.version = candidate.version;
    choice.source = candidate.source;
    choice.sha256 = candidate.sha256;
    chosen_.insert_or_assign(name, choice);
    suppress_optional(name);

    ResolvedPackage package;
    package.name = name;
    package.version = candidate.version;
    package.source = candidate.source;
    package.sha256 = candidate.sha256;

    bool usable = true;
    const auto dependencies = provider_(package);
    if (!dependencies) {
      failure_ = dependencies.error();  // 该候选不可用 → 试下一个
      usable = false;
    } else {
      const std::string from = std::format("{} {}", name, candidate.version.to_string());
      for (const auto& spec : *dependencies) {
        const auto requirement = make_requirement(spec, from);
        if (!requirement) {
          failure_ = requirement.error();
          usable = false;
          break;
        }
        const bool was_skipped = skipped_.count(spec.name) != 0;
        const std::size_t before = constraints_[spec.name].size();
        if (!add_requirement(spec.name, *requirement, depth + 1)) {
          usable = false;
          break;
        }
        if (constraints_[spec.name].size() > before) {
          chosen_[name].added.emplace_back(spec.name, before);
        }
        if (was_skipped && skipped_.count(spec.name) == 0) {
          chosen_[name].reactivated.push_back(spec.name);
        }
        if (!contains(chosen_[name].dependencies, spec.name)) {
          chosen_[name].dependencies.push_back(spec.name);
        }
      }
    }
    if (usable && search(depth + 1)) return true;
    undo_choice(name);
    return false;
  }

  [[nodiscard]] auto next_unresolved() const -> std::string {
    for (const auto& [name, requirements] : constraints_) {
      if (chosen_.count(name) != 0 || skipped_.count(name) != 0) continue;
      if (std::ranges::none_of(requirements, [](const Requirement& item) { return item.active; })) {
        continue;
      }
      return name;
    }
    return {};
  }

  [[nodiscard]] static auto satisfies(const std::vector<Requirement>& requirements,
                                      const Version& version, bool include_optional) -> bool {
    for (const Requirement& requirement : requirements) {
      if (!requirement.active) continue;
      if (!include_optional && requirement.optional) continue;
      if (!requirement.req.matches(version)) return false;
    }
    return true;
  }

  [[nodiscard]] auto all_optional(const std::string& name) const -> bool {
    const auto found = constraints_.find(name);
    if (found == constraints_.end()) return false;
    bool any = false;
    for (const Requirement& requirement : found->second) {
      if (!requirement.active) continue;
      any = true;
      if (!requirement.optional) return false;
    }
    return any;
  }

  /// 候选版本列表（降序；显式来源优先于索引来源）。冲突时返回空并已记录。
  auto candidate_list(const std::string& name, std::size_t depth) -> std::vector<Candidate> {
    const auto found = constraints_.find(name);
    if (found == constraints_.end()) return {};
    const std::vector<Requirement>& requirements = found->second;

    const SourceSpec* pinned = nullptr;
    for (const Requirement& requirement : requirements) {
      if (!requirement.active) continue;
      if (requirement.has_source && requirement.source.kind != SourceSpec::Kind::Registry &&
          !requirement.source.location.empty()) {
        pinned = &requirement.source;
        break;
      }
    }

    std::vector<Candidate> all;
    if (registry_.has(name)) {
      for (const Version& version : registry_.versions(name)) {
        const PackageRelease* release = registry_.release(name, version);
        Candidate candidate;
        candidate.version = version;
        if (pinned != nullptr) {
          candidate.source = *pinned;
          candidate.sha256 = pinned->sha256;
        } else if (release != nullptr) {
          candidate.source = release->source;
          candidate.sha256 = release->sha256;
        }
        all.push_back(std::move(candidate));
      }
    } else if (pinned != nullptr) {
      // 索引无此包但依赖给出显式来源（本地目录/归档/索引地址）→ 合成唯一候选，
      // 版本取约束下界（`^1.2.0` → 1.2.0；`*` → 0.0.0）。
      Version derived{};
      bool has_base = false;
      for (const Requirement& requirement : requirements) {
        if (!requirement.active) continue;
        for (const auto& comparator : requirement.req.comparators) {
          if (comparator.op == Comparator::Op::Any) continue;
          derived = comparator.version;
          has_base = true;
          break;
        }
        if (has_base) break;
      }
      Candidate candidate;
      candidate.version = derived;
      candidate.source = *pinned;
      candidate.sha256 = pinned->sha256;
      all.push_back(std::move(candidate));
    } else {
      record_conflict(depth, describe_missing(name, requirements));
      return {};
    }

    std::vector<Candidate> strict;
    std::vector<Candidate> relaxed;
    bool has_required = false;
    for (const Candidate& candidate : all) {
      if (satisfies(requirements, candidate.version, true)) {
        strict.push_back(candidate);
      } else if (satisfies(requirements, candidate.version, false)) {
        relaxed.push_back(candidate);
      }
    }
    for (const Requirement& requirement : requirements) {
      if (requirement.active && !requirement.optional) has_required = true;
    }
    if (!strict.empty()) return strict;
    if (has_required && !relaxed.empty()) {
      return relaxed;  // 必需需求可满足、仅可选需求不满足：选定后抑制该可选需求
    }
    if (!has_required) {
      return {};  // 全可选且无满足者：交给 search 走「跳过可选依赖」分支（不记冲突）
    }
    record_version_conflict(depth, conflict_message(name, requirements));
    return {};
  }

  // —— 冲突与错误 ——

  void record_conflict(std::size_t depth, std::string message) {
    if (depth > conflict_depth_) {
      conflict_depth_ = depth;
      conflicts_.clear();
    }
    if (depth < conflict_depth_) return;
    if (contains(conflicts_, message)) return;
    conflicts_.push_back(std::move(message));
  }

  void record_version_conflict(std::size_t depth, std::string message) {
    any_version_conflict_ = true;
    record_conflict(depth, std::move(message));
  }

  [[nodiscard]] auto describe_missing(const std::string& name,
                                      const std::vector<Requirement>& requirements) const
      -> std::string {
    std::string text = std::format("包 {} 未在索引中找到（{} 无该包，依赖也未给出显式来源）：", name,
                                   registry_.location().empty() ? "索引" : registry_.location());
    for (const Requirement& requirement : requirements) {
      if (!requirement.active) continue;
      text.append(std::format("\n    - {}（来自 {}）", requirement.req.to_string(), requirement.from));
    }
    return text;
  }

  [[nodiscard]] auto conflict_message(const std::string& name,
                                      const std::vector<Requirement>& requirements) const
      -> std::string {
    std::string text = std::format("包 {} 的版本约束无法同时满足：", name);
    for (const Requirement& requirement : requirements) {
      if (!requirement.active) continue;
      text.append(std::format("\n    - {}（来自 {}）", requirement.req.to_string(), requirement.from));
    }
    const std::vector<Version> available = registry_.versions(name);
    if (available.empty()) {
      text.append("\n  索引中该包没有可用版本");
    } else {
      text.append("\n  索引中可用版本：");
      for (std::size_t index = 0; index < available.size(); ++index) {
        if (index != 0) text.append(", ");
        text.append(available[index].to_string());
      }
    }
    if (!chosen_.empty()) {
      text.append("\n  冲突点的已选版本：");
      bool first = true;
      for (const auto& [key, choice] : chosen_) {
        if (!first) text.append(", ");
        text.append(std::format("{} {}", key, choice.version.to_string()));
        first = false;
      }
    }
    return text;
  }

  [[nodiscard]] auto failure_message() const -> std::string {
    if (!conflicts_.empty()) {
      std::string text = "无法求解依赖：存在版本冲突";
      for (const auto& conflict : conflicts_) {
        text.append("\n  * ");
        text.append(conflict);
      }
      return text;
    }
    if (failure_.has_value()) return std::format("无法求解依赖：{}", failure_->to_string());
    return "无法求解依赖：无解（未找到可用候选）";
  }

  [[nodiscard]] auto error_code() const -> ErrorCode {
    if (conflicts_.empty()) return failure_.has_value() ? failure_->code : ErrorCode::Invalid;
    return any_version_conflict_ ? ErrorCode::Invalid : ErrorCode::NotFound;
  }

  // —— 输出 ——

  void visit(const std::string& name, std::set<std::string>& emitted,
             std::set<std::string>& in_progress, std::vector<std::string>& order) const {
    if (emitted.count(name) != 0 || in_progress.count(name) != 0) return;
    in_progress.insert(name);
    if (const auto found = chosen_.find(name); found != chosen_.end()) {
      for (const auto& dependency : found->second.dependencies) {
        visit(dependency, emitted, in_progress, order);
      }
    }
    in_progress.erase(name);
    emitted.insert(name);
    order.push_back(name);
  }

  [[nodiscard]] auto build() const -> ResolvedGraph {
    std::set<std::string> emitted;
    std::set<std::string> in_progress;
    std::vector<std::string> order;
    for (const auto& name : root_names_) visit(name, emitted, in_progress, order);
    for (const auto& [name, choice] : chosen_) {
      static_cast<void>(choice);
      visit(name, emitted, in_progress, order);
    }

    ResolvedGraph graph;
    for (const auto& name : order) {
      const auto found = chosen_.find(name);
      if (found == chosen_.end()) continue;
      ResolvedPackage package;
      package.name = name;
      package.version = found->second.version;
      package.source = found->second.source;
      package.sha256 = found->second.sha256;
      for (const auto& dependency : found->second.dependencies) {
        if (dependency == name) continue;
        if (chosen_.count(dependency) == 0) continue;
        if (!contains(package.dependencies, dependency)) {
          package.dependencies.push_back(dependency);
        }
      }
      graph.packages.push_back(std::move(package));
    }
    return graph;
  }

  const Registry& registry_;
  const DependencyProvider& provider_;
  std::vector<std::string> root_names_{};
  std::map<std::string, std::vector<Requirement>> constraints_{};
  std::map<std::string, Choice> chosen_{};
  std::set<std::string> skipped_{};
  std::vector<std::string> conflicts_{};
  std::size_t conflict_depth_{0};
  bool any_version_conflict_{false};
  std::optional<Error> failure_{};
};

}  // namespace

auto Registry::load(std::string_view location) -> Result<Registry> {
  if (location.empty()) return unexpected(ErrorCode::Invalid, "索引地址为空");
  const Status tls = detail::reject_tls(location);
  if (!tls) return forward_error(tls.error());

  std::string text;
  if (detail::is_http(location)) {
    const auto response = detail::http_get(location, detail::HttpOptions{});
    if (!response) return forward_error(response.error());
    if (response->status != 200) {
      return unexpected(ErrorCode::Io, std::format("索引下载失败（HTTP {}）：{}", response->status,
                                                   std::string(location)));
    }
    text = body_to_text(response->body);
  } else {
    const std::string path = detail::strip_file_scheme(location);
    const auto file = st::fs::read_text(path);
    if (!file) return forward_error(file.error());
    text = *file;
  }

  const auto json = st::json::parse(text);
  if (!json) {
    return unexpected(json.error().code, std::format("索引解析失败（{}）：{}", std::string(location),
                                                     json.error().message));
  }
  return parse_json(*json, location);
}

auto Registry::parse_json(const st::Value& json, std::string_view location) -> Result<Registry> {
  Registry registry;
  registry.location_ = std::string(location);

  const st::Value* packages = nullptr;
  if (json.is_array()) {
    packages = &json;
  } else if (json.is_object()) {
    packages = json.find("packages");
    if (packages == nullptr) packages = json.find("releases");
    if (packages == nullptr) return unexpected(ErrorCode::Parse, "索引缺少 packages 数组");
  } else {
    return unexpected(ErrorCode::Parse, "索引根节点必须是对象或数组");
  }
  if (!packages->is_array()) return unexpected(ErrorCode::Parse, "索引 packages 必须是数组");

  for (const auto& item : packages->items()) {
    if (!item.is_object()) return unexpected(ErrorCode::Parse, "索引项必须是 JSON 对象");
    const std::string name = item.get_string("name");
    if (name.empty()) return unexpected(ErrorCode::Parse, "索引项缺少 name");
    const auto version = Version::parse(item.get_string("version"));
    if (!version) {
      return unexpected(ErrorCode::Parse, std::format("索引项 {} 的版本非法（{}）：{}", name,
                                                      item.get_string("version"),
                                                      version.error().message));
    }
    const auto source = source_spec_from_object(item);
    if (!source) return forward_error(source.error());
    PackageRelease release;
    release.name = name;
    release.version = *version;
    release.source = *source;
    release.sha256 = source->sha256.empty() ? item.get_string("sha256") : source->sha256;
    registry.releases_.push_back(std::move(release));
  }
  return registry;
}

auto Registry::versions(std::string_view name) const -> std::vector<Version> {
  std::vector<Version> found;
  for (const auto& release : releases_) {
    if (release.name != name) continue;
    if (contains_version(found, release.version)) continue;
    found.push_back(release.version);
  }
  std::ranges::sort(found, [](const Version& a, const Version& b) { return compare(a, b) > 0; });
  return found;
}

auto Registry::release(std::string_view name, const Version& version) const -> const PackageRelease* {
  for (const auto& release : releases_) {
    if (release.name != name) continue;
    if (compare(release.version, version) == 0) return &release;
  }
  return nullptr;
}

auto Registry::has(std::string_view name) const -> bool {
  for (const auto& release : releases_) {
    if (release.name == name) return true;
  }
  return false;
}

auto ResolvedGraph::find(std::string_view name) const -> const ResolvedPackage* {
  for (const auto& package : packages) {
    if (package.name == name) return &package;
  }
  return nullptr;
}

auto resolve(const std::vector<DependencySpec>& roots, const Registry& registry,
             const DependencyProvider& provider) -> Result<ResolvedGraph> {
  if (!provider) {
    return unexpected(ErrorCode::Invalid,
                      "依赖提供者（DependencyProvider）为空：求解器不做 I/O，需要外部注入");
  }
  Solver solver(registry, provider);
  return solver.run(roots);
}

auto graph_to_json(const ResolvedGraph& graph) -> st::Value {
  st::Value json = st::Value::object();
  json.set("lock_version", st::Value(1));

  st::Value packages = st::Value::array();
  std::string fingerprint_input;
  for (const auto& package : graph.packages) {
    st::Value entry = st::Value::object();
    entry.set("name", st::Value(package.name));
    entry.set("version", st::Value(package.version.to_string()));
    entry.set("source", source_to_json(package.source));
    entry.set("sha256", st::Value(package.sha256));
    st::Value dependencies = st::Value::array();
    for (const auto& name : package.dependencies) dependencies.push(st::Value(name));
    entry.set("dependencies", dependencies);
    packages.push(entry);

    fingerprint_input.append(package.name);
    fingerprint_input.push_back('@');
    fingerprint_input.append(package.version.to_string());
    fingerprint_input.push_back('|');
    fingerprint_input.append(std::string(source_kind_name(package.source.kind)));
    fingerprint_input.push_back(':');
    fingerprint_input.append(package.source.location);
    fingerprint_input.push_back('|');
    fingerprint_input.append(package.sha256);
    fingerprint_input.append("|deps:");
    for (std::size_t index = 0; index < package.dependencies.size(); ++index) {
      if (index != 0) fingerprint_input.push_back(',');
      fingerprint_input.append(package.dependencies[index]);
    }
    fingerprint_input.push_back('\n');
  }
  json.set("packages", packages);
  if (!fingerprint_input.empty()) {
    json.set("build_fingerprint", st::Value(st::hash::sha256_hex(fingerprint_input)));
  }
  return json;
}

auto graph_from_json(const st::Value& json) -> Result<ResolvedGraph> {
  if (!json.is_object()) return unexpected(ErrorCode::Parse, "锁文件根节点必须是 JSON 对象");
  const st::Value& packages = json.at("packages");
  if (!packages.is_array()) return unexpected(ErrorCode::Parse, "锁文件缺少 packages 数组");

  ResolvedGraph graph;
  for (const auto& item : packages.items()) {
    if (!item.is_object()) return unexpected(ErrorCode::Parse, "锁文件 packages 元素必须是对象");
    ResolvedPackage package;
    package.name = item.get_string("name");
    if (package.name.empty()) return unexpected(ErrorCode::Parse, "锁文件条目缺少 name");
    const auto version = Version::parse(item.get_string("version"));
    if (!version) {
      return unexpected(ErrorCode::Parse,
                        std::format("锁文件条目 {} 的版本非法（{}）：{}", package.name,
                                    item.get_string("version"), version.error().message));
    }
    package.version = *version;
    const auto source = source_spec_from_object(item);
    if (!source) return forward_error(source.error());
    package.source = *source;
    package.sha256 = source->sha256.empty() ? item.get_string("sha256") : source->sha256;
    package.dependencies = item.get_string_array("dependencies");
    graph.packages.push_back(std::move(package));
  }
  return graph;
}

}  // namespace st::pkg
