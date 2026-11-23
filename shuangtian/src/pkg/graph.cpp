#include "st/pkg/graph.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace st::pkg {
namespace {

/// `st/a/b/name.hpp` → `st/a/b`。空路径/单层返回空串（调用方跳过）。
auto directory_of(std::string_view path) -> std::string {
  const auto slash = path.rfind('/');
  if (slash == std::string_view::npos) return {};
  return std::string{path.substr(0, slash)};
}

/// 去掉 `.hpp` / `.cpp` 之类的扩展名，留基名。
auto stem_of(std::string_view path) -> std::string {
  const auto slash = path.rfind('/');
  const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const auto dot = name.rfind('.');
  return std::string{dot == std::string_view::npos ? name : name.substr(0, dot)};
}

/// Tarjan 强连通分量（迭代版）。
///
/// 为何不用递归：包数虽小（~25），但**将来会涨**，而深度递归在 Windows 默认栈上
/// 到几千层就有风险。迭代版代价只是代码长一点，换掉一整类"栈溢出"隐患。
///
/// 为何需要 SCC 而不是简单拓扑：实测存在 `st/ui` ↔ `st/ui/components` 的真实循环
/// （见 `graph.hpp` 的长注释）。要求零环等于要求重排数十个文件，代价与收益不成正比；
/// 收缩成一点同样能得到 DAG。
auto strongly_connected(const std::vector<std::string>& nodes,
                        const std::map<std::string, std::set<std::string>>& adjacency)
    -> std::vector<std::vector<std::string>> {
  struct Frame {
    const std::string* node;
    std::vector<const std::string*> successors;
    std::size_t next{0};
  };
  std::unordered_map<std::string, int> index;
  std::unordered_map<std::string, int> lowlink;
  std::unordered_set<std::string> on_stack;
  std::vector<std::string> stack;
  std::vector<std::vector<std::string>> result;
  int counter = 0;

  for (const auto& root : nodes) {
    if (index.contains(root)) continue;
    std::vector<Frame> frames;
    auto push = [&](const std::string& node) {
      index[node] = counter;
      lowlink[node] = counter;
      ++counter;
      stack.push_back(node);
      on_stack.insert(node);
      Frame frame{&node, {}};
      if (const auto found = adjacency.find(node); found != adjacency.end()) {
        for (const auto& successor : found->second) frame.successors.push_back(&successor);
      }
      frames.push_back(std::move(frame));
    };
    push(root);
    while (!frames.empty()) {
      auto& top = frames.back();
      if (top.next < top.successors.size()) {
        const std::string& successor = *top.successors[top.next];
        ++top.next;
        if (!index.contains(successor)) {
          push(successor);
        } else if (on_stack.contains(successor)) {
          lowlink[*top.node] = std::min(lowlink[*top.node], index[successor]);
        }
        continue;
      }
      const std::string finished = *top.node;
      frames.pop_back();
      if (!frames.empty()) {
        lowlink[*frames.back().node] = std::min(lowlink[*frames.back().node], lowlink[finished]);
      }
      if (lowlink[finished] == index[finished]) {
        std::vector<std::string> component;
        while (true) {
          std::string node = stack.back();
          stack.pop_back();
          on_stack.erase(node);
          component.push_back(std::move(node));
          if (component.back() == finished) break;
        }
        std::ranges::sort(component);
        result.push_back(std::move(component));
      }
    }
  }
  // 分量顺序稳定化：按最小成员名排序，便于测试与日志对照。
  std::ranges::sort(result, [](const auto& left, const auto& right) {
    return left.front() < right.front();
  });
  return result;
}

}  // namespace

auto package_of_header(std::string_view relative_to_include) -> std::string {
  const auto slash = relative_to_include.rfind('/');
  if (slash == std::string_view::npos) return {};
  return std::string{relative_to_include.substr(0, slash)};
}

auto package_of_unit(std::string_view relative_to_repo, const HeaderLookup& lookup) -> std::string {
  return package_of_unit(relative_to_repo, lookup, nullptr);
}

auto package_of_unit(std::string_view relative_to_repo, const HeaderLookup& lookup,
                     const IncludeReader* self_includes) -> std::string {
  const auto directory = directory_of(relative_to_repo);
  const auto stem = stem_of(relative_to_repo);
  // `src/a/b` → `st/a/b`
  std::string fallback = directory;
  if (fallback.starts_with("src/")) {
    fallback = std::format("st{}", fallback.substr(std::string_view{"src"}.size()));
  } else if (fallback == "src") {
    fallback = "st";
  }
  const auto candidates = lookup(stem);
  if (!candidates.has_value()) return fallback;
  const std::string same_stem = "/" + stem + ".hpp";

  // ① **单元自己 include 了哪个同名头**——最权威的证据，优先于一切路径推断。
  //
  // 为何必须是第一判据（实测反例）：`src/geom/animation.cpp` 实现的是
  // `st/raster/animation.hpp`，但仓里还有另一个同名头 `st/ui/animation.hpp`，
  // 于是“唯一同名头”和“同目录同名头”两条规则双双失效 → 回退成 `st/geom`，
  // 而 `st/geom` 被判成“不需要”⇒ 整个 `animation.cpp` 被裁 ⇒
  // 链接报 `undefined symbol: st::raster::sample_clip`（jianmu 实测）。
  // 而那个 cpp 的**第一行**就是 `#include "st/raster/animation.hpp"`——
  // 证据一直都在，只是之前的规则没去看它。
  if (self_includes != nullptr) {
    bool matched = false;
    std::string matched_package;
    for (const auto& included : (*self_includes)(std::string{relative_to_repo})) {
      if (!included.ends_with(same_stem)) continue;
      if (std::ranges::find(*candidates, included) == candidates->end()) continue;
      // 多个同名头都被 include 时收敛到同一包才算数（否则拿第一个，
      // 它们本来就在不同包里，任取一个都是猜——但这种情况实测不存在）。
      const std::string package = package_of_header(included);
      if (matched && package != matched_package) return fallback;
      matched = true;
      matched_package = package;
    }
    if (matched) return matched_package;
  }

  // ② 同目录同名头在 → 就是这个包（常规情况，`src/ui/button.cpp` → `st/ui`）。
  const std::string same_directory = fallback + same_stem;
  if (std::ranges::find(*candidates, same_directory) != candidates->end()) {
    return package_of_header(same_directory);
  }
  // ③ 全局唯一同名头在 → 用它的目录（`src/geom/gltf.cpp` → `st/raster/gltf.hpp`）。
  if (candidates->size() == 1) return package_of_header(candidates->front());
  // ④ 都没有 / 有多个分居各处且自己没 include → 回退目录同名。
  return fallback;
}

auto PackageGraph::build(const std::vector<std::string>& headers,
                         const std::vector<std::string>& units,
                         const IncludeReader& read_includes) -> PackageGraph {
  PackageGraph graph;
  std::map<std::string, Package> by_name;

  // —— 归属：头文件按路径目录；单元按同名头优先 ——
  // 同名头索引：基名 → 头路径列表（供 `package_of_unit` 判“唯一同名头在别处”）
  std::unordered_map<std::string, std::vector<std::string>> headers_by_stem;
  for (const auto& header : headers) {
    // 头路径统一成"相对 include/"的形式（`st/…`）
    const std::string relative = header.starts_with("include/") ? header.substr(8) : header;
    headers_by_stem[stem_of(relative)].push_back(relative);
  }
  const HeaderLookup lookup = [&headers_by_stem](std::string_view stem)
      -> std::optional<std::vector<std::string>> {
    if (const auto found = headers_by_stem.find(std::string{stem}); found != headers_by_stem.end()) {
      return found->second;
    }
    return std::nullopt;
  };

  for (const auto& header : headers) {
    const std::string relative = header.starts_with("include/") ? header.substr(8) : header;
    const std::string name = package_of_header(relative);
    if (name.empty()) continue;
    by_name[name].name = name;
    by_name[name].headers.push_back(header);
  }

  for (const auto& unit : units) {
    // 归属规则只维护在 `package_of_unit` 一处，避免两边走歧（初版就是两边各写一份，
    // 结果测试捕获到 `src/geom/gltf.cpp` 被判成 `st/geom`）。
    // 传入 `read_includes` 启用最强判据：该单元自己 include 了哪个同名头。
    const std::string name = package_of_unit(unit, lookup, &read_includes);
    by_name[name].name = name;
    by_name[name].units.push_back(unit);
  }

  // —— 边：头 + 实现两侧的 include 都算（严口径） ——
  std::map<std::string, std::set<std::string>> adjacency;
  const auto add_edges = [&](const std::string& file, const std::string& owner) {
    for (const auto& included : read_includes(file)) {
      const std::string target = package_of_header(included);
      if (target.empty() || target == owner) continue;
      adjacency[owner].insert(target);
    }
  };
  for (const auto& [name, package] : by_name) {
    adjacency.try_emplace(name);
    for (const auto& header : package.headers) add_edges(header, name);
    for (const auto& unit : package.units) add_edges(unit, name);
  }

  std::vector<std::string> names;
  names.reserve(by_name.size());
  for (const auto& [name, unused] : by_name) names.push_back(name);

  graph.components_ = strongly_connected(names, adjacency);
  for (std::size_t position = 0; position < graph.components_.size(); ++position) {
    for (const auto& member : graph.components_[position]) {
      graph.component_index_[member] = position;
    }
  }
  // 依赖表保留**原始**边（跨分量 + 同分量自环一并去掉自环），
  // 闭包按分量算，因此这里跨分量的边才是关键。
  // **每个包都要在表里出现**（哪怕是空集）：否则调用方 `at()` 会抛异常。
  // 初版就漏了这个——测试 `pkg_graph_builds_basic_edges` 直接抓到 `map::at`。
  for (const auto& [name, unused] : by_name) graph.dependencies_.try_emplace(name);
  for (const auto& [from, targets] : adjacency) {
    for (const auto& to : targets) {
      if (graph.component_index_.at(from) == graph.component_index_.at(to)) continue;
      graph.dependencies_[from].insert(to);
    }
  }

  // 包列表按名字排序（稳定输出）
  for (auto& [name, package] : by_name) {
    std::ranges::sort(package.headers);
    std::ranges::sort(package.units);
    graph.packages_.push_back(std::move(package));
  }
  std::ranges::sort(graph.packages_, [](const Package& left, const Package& right) {
    return left.name < right.name;
  });
  return graph;
}

auto PackageGraph::component_of(const std::string& package) const -> std::optional<std::size_t> {
  if (const auto found = component_index_.find(package); found != component_index_.end()) {
    return found->second;
  }
  return std::nullopt;
}

auto PackageGraph::closure(const std::set<std::string>& roots) const -> std::set<std::string> {
  // 先转到分量空间：同分量内的包必须同生共死，收敛成一个节点再求可达。
  std::unordered_set<std::size_t> reached;
  std::vector<std::size_t> stack;
  for (const auto& root : roots) {
    if (const auto component = component_of(root); component.has_value()) {
      if (reached.insert(*component).second) stack.push_back(*component);
    }
  }
  // 分量级邻接（由包级依赖现算，避免再存一份可能不同步的表）
  std::map<std::size_t, std::set<std::size_t>> by_component;
  for (const auto& [from, targets] : dependencies_) {
    for (const auto& to : targets) {
      by_component[component_index_.at(from)].insert(component_index_.at(to));
    }
  }
  while (!stack.empty()) {
    const std::size_t current = stack.back();
    stack.pop_back();
    if (const auto found = by_component.find(current); found != by_component.end()) {
      for (const auto successor : found->second) {
        if (reached.insert(successor).second) stack.push_back(successor);
      }
    }
  }
  // 回到包空间
  std::set<std::string> result;
  for (std::size_t position = 0; position < components_.size(); ++position) {
    if (!reached.contains(position)) continue;
    for (const auto& member : components_[position]) result.insert(member);
  }
  return result;
}

auto PackageGraph::units_for(const std::set<std::string>& packages) const
    -> std::vector<std::string> {
  std::vector<std::string> result;
  for (const auto& package : packages_) {
    if (!packages.contains(package.name)) continue;
    result.insert(result.end(), package.units.begin(), package.units.end());
  }
  std::ranges::sort(result);
  return result;
}

auto PackageGraph::cycles() const -> std::vector<std::vector<std::string>> {
  std::vector<std::vector<std::string>> result;
  for (const auto& component : components_) {
    if (component.size() > 1) result.push_back(component);
  }
  return result;
}

auto PackageGraph::edge_count() const -> std::size_t {
  std::size_t total = 0;
  for (const auto& [from, targets] : dependencies_) total += targets.size();
  return total;
}

}  // namespace st::pkg
