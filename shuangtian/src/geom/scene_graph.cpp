#include "st/raster/scene_graph.hpp"

#include <cmath>
#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "st/raster/mesh_ops.hpp"

namespace st::raster {

// ────────────────────────────────────────────────────────────────────────────
// TransformTRS
// ────────────────────────────────────────────────────────────────────────────

auto TransformTRS::is_identity() const noexcept -> bool {
  return translation.x == 0.0f && translation.y == 0.0f && translation.z == 0.0f &&
         rotation.x == 0.0f && rotation.y == 0.0f && rotation.z == 0.0f && scale.x == 1.0f &&
         scale.y == 1.0f && scale.z == 1.0f;
}

// ────────────────────────────────────────────────────────────────────────────
// 枢轴预设
// ────────────────────────────────────────────────────────────────────────────

auto pivot_preset_name(PivotPreset preset) noexcept -> std::string_view {
  switch (preset) {
    case PivotPreset::Origin: return "origin";
    case PivotPreset::Center: return "center";
    case PivotPreset::Bottom: return "bottom";
    case PivotPreset::Top: return "top";
  }
  return "origin";
}

auto parse_pivot_preset(std::string_view name) -> std::optional<PivotPreset> {
  // 大小写与分隔符不敏感（`Bottom` / `bottom` / `BOTTOM` 都收）
  std::string key;
  key.reserve(name.size());
  for (const char ch : name) {
    if (ch == '-' || ch == ' ' || ch == '.') {
      key.push_back('_');
    } else {
      key.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
    }
  }
  if (key == "origin" || key == "root" || key == "zero") return PivotPreset::Origin;
  if (key == "center" || key == "centre" || key == "middle") return PivotPreset::Center;
  if (key == "bottom" || key == "base" || key == "feet" || key == "floor") {
    return PivotPreset::Bottom;
  }
  if (key == "top" || key == "head") return PivotPreset::Top;
  return std::nullopt;
}

auto pivot_preset_names() -> std::string {
  return "origin, center, bottom, top";
}

auto pivot_from_bounds(const Bounds& box, PivotPreset preset) noexcept -> math::Vec3 {
  if (box.is_empty() || preset == PivotPreset::Origin) return math::Vec3{};
  const math::Vec3 center = box.center();
  switch (preset) {
    case PivotPreset::Origin: return math::Vec3{};
    case PivotPreset::Center: return center;
    // 底面中心：x/z 取中心、y 取 min。这是"站在地上"的物体的自然旋转中心
    case PivotPreset::Bottom: return math::Vec3{center.x, box.min.y, center.z};
    case PivotPreset::Top: return math::Vec3{center.x, box.max.y, center.z};
  }
  return math::Vec3{};
}

// ────────────────────────────────────────────────────────────────────────────
// 局部 / 世界矩阵
// ────────────────────────────────────────────────────────────────────────────

auto local_matrix(const SceneNode& node) noexcept -> math::Mat4 {
  const TransformTRS& trs = node.local;
  // 顺序：T(平移) · T(枢轴) · Rz · Ry · Rx · S · T(-枢轴)
  //
  // ## 旋转合成顺序为什么是 X → Y → Z 而不是反过来
  //
  // 矩阵从右往左作用：`Rz * Ry * Rx * v` 表示"先绕 X、再绕 Y、最后绕 Z"——
  // 这正是 Blender `rotation_euler` 的 XYZ 模式（也是本仓 `mesh_ops::transform`
  // 的重载所用的顺序）。两处若不一致，"同一组欧拉角"在脚本与代码里会得到
  // 不同的姿态，而那种差在画面上只表现为"角度差一点"。
  math::Mat4 rotation = math::Mat4::identity();
  if (trs.rotation.x != 0.0f) {
    rotation = math::Mat4::rotation(math::Vec3{1.0f, 0.0f, 0.0f}, trs.rotation.x) * rotation;
  }
  if (trs.rotation.y != 0.0f) {
    rotation = math::Mat4::rotation(math::Vec3{0.0f, 1.0f, 0.0f}, trs.rotation.y) * rotation;
  }
  if (trs.rotation.z != 0.0f) {
    rotation = math::Mat4::rotation(math::Vec3{0.0f, 0.0f, 1.0f}, trs.rotation.z) * rotation;
  }

  math::Mat4 model = math::Mat4::translation(trs.translation);
  const bool pivot_used =
      node.pivot.x != 0.0f || node.pivot.y != 0.0f || node.pivot.z != 0.0f;
  if (pivot_used) model = model * math::Mat4::translation(node.pivot);
  model = model * rotation * math::Mat4::scaling(trs.scale);
  if (pivot_used) model = model * math::Mat4::translation(node.pivot * -1.0f);
  return model;
}

// ────────────────────────────────────────────────────────────────────────────
// 求解
// ────────────────────────────────────────────────────────────────────────────

namespace {

/// 展平用的递归上下文（深度与路径拼装）。
struct WalkState {
  ResolvedTree tree{};
  std::map<std::string, std::size_t> by_name{};
  std::set<std::string> duplicated{};
};

/// 递归展开一个节点。
///
/// 为什么用递归而不是显式栈：树的深度上限只有 32（`kMaxSceneDepth`），
/// 而递归版本能自然地带"父路径""父矩阵"两个上下文；显式栈要手写等价结构，
/// 那反而更容易在某处漏传一个上下文。
auto walk(const SceneNode& node, std::string_view parent_path, const math::Mat4& parent_world,
          bool parent_visible, std::size_t depth, int parent_index, WalkState& state) -> void {
  ResolvedNode resolved{};
  resolved.name = node.name;
  resolved.path = parent_path.empty()
                      ? (node.name.empty() ? std::format("#{}", state.tree.nodes.size())
                                           : node.name)
                      : (node.name.empty()
                             ? std::format("{}/#{}", parent_path, state.tree.nodes.size())
                             : std::format("{}/{}", parent_path, node.name));
  resolved.depth = depth;
  resolved.parent = parent_index;
  resolved.has_geometry = node.has_geometry;
  resolved.visible = parent_visible && node.visible;
  resolved.world = parent_world * local_matrix(node);

  const int index = static_cast<int>(state.tree.nodes.size());
  state.tree.nodes.push_back(resolved);
  if (!node.name.empty()) {
    if (state.by_name.contains(node.name)) {
      state.duplicated.insert(node.name);
    } else {
      state.by_name.emplace(node.name, static_cast<std::size_t>(index));
    }
  }
  for (const SceneNode& child : node.children) {
    walk(child, state.tree.nodes[static_cast<std::size_t>(index)].path, resolved.world,
         resolved.visible, depth + 1U, index, state);
  }
}

/// 校验深度：递归前先量一次，避免病态输入（几万层）把栈打爆。
auto depth_of(const SceneNode& node, std::size_t limit) -> std::size_t {
  if (limit == 0U) return 0U;
  std::size_t deepest = 0U;
  for (const SceneNode& child : node.children) {
    deepest = std::max(deepest, depth_of(child, limit - 1U));
  }
  return deepest + 1U;
}

}  // namespace

auto resolve(const std::vector<SceneNode>& roots) -> Result<ResolvedTree> {
  // 深度先查（用递归查深度本身也是递归，但 limit 递减保证它有界）
  for (const SceneNode& root : roots) {
    if (depth_of(root, kMaxSceneDepth + 1U) > kMaxSceneDepth) {
      return st::unexpected(
          ErrorCode::Invalid,
          std::format("场景树超过 {} 层（节点 \"{}\"）；请把层级压平或拆成多个场景",
                      kMaxSceneDepth, root.name.empty() ? std::string("<未命名>") : root.name));
    }
  }

  WalkState state{};
  for (const SceneNode& root : roots) {
    walk(root, {}, math::Mat4::identity(), true, 0U, -1, state);
  }

  // ## 重名必须拒绝（而不是"后者覆盖前者"）
  //
  // 组合模型里"按名字找零件"是主要操作方式（动画面板、导出后的引擎对象、
  // 报错时的指路）。重名时按名字寻址会静默指向其中一个，而调用方以为
  // 自己拿到的是另一个——这类错误的表现是"改 A 却动了 B"，极难定位。
  if (!state.duplicated.empty()) {
    std::string names;
    for (const std::string& name : state.duplicated) {
      if (!names.empty()) names += ", ";
      names += name;
    }
    return st::unexpected(ErrorCode::Invalid,
                          std::format("场景树里有重名节点：{}（节点名在同一棵树里必须唯一；"
                                      "不同分支下请用不同名字）",
                                      names));
  }
  // 把名字索引写进结果（`find_by_name` 用）
  state.tree.nodes.shrink_to_fit();
  ResolvedTree tree = std::move(state.tree);
  tree.name_index = std::move(state.by_name);
  return tree;
}

auto ResolvedTree::find_by_name(std::string_view name) const -> const ResolvedNode* {
  const auto found = name_index.find(std::string(name));
  if (found == name_index.end()) return nullptr;
  return found->second < nodes.size() ? &nodes[found->second] : nullptr;
}

auto ResolvedTree::find_by_path(std::string_view path) const -> const ResolvedNode* {
  for (const ResolvedNode& node : nodes) {
    if (node.path == path) return &node;
  }
  return nullptr;
}

// ────────────────────────────────────────────────────────────────────────────
// 扁平写法 → 树
// ────────────────────────────────────────────────────────────────────────────

auto attach_by_parent(std::vector<SceneNode>& nodes, const std::vector<std::string>& parent_names)
    -> Status {
  if (nodes.size() != parent_names.size()) {
    return st::unexpected(ErrorCode::Internal,
                          std::format("attach_by_parent：节点数 {} 与父名数 {} 不一致",
                                      nodes.size(), parent_names.size()));
  }
  // 先把名字索引建好（于是父可以出现在子之后——编辑顺序通常是先写零件、后加父）
  std::map<std::string, std::size_t> index_of;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const std::string& name = nodes[i].name;
    if (name.empty()) continue;
    if (index_of.contains(name)) {
      return st::unexpected(ErrorCode::Invalid,
                            std::format("扁平节点列表里有重名：{}（节点名必须唯一）", name));
    }
    index_of.emplace(name, i);
  }

  // 逐个查父；形成的"父指向"表用来做环检测
  std::vector<int> parent_of(nodes.size(), -1);
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const std::string& parent = parent_names[i];
    if (parent.empty()) continue;
    const auto found = index_of.find(parent);
    if (found == index_of.end()) {
      return st::unexpected(ErrorCode::NotFound,
                            std::format("节点 \"{}\" 的 parent 指向不存在的 \"{}\"",
                                        nodes[i].name.empty() ? std::string("<未命名>") : nodes[i].name,
                                        parent));
    }
    if (found->second == i) {
      return st::unexpected(ErrorCode::Invalid,
                            std::format("节点 \"{}\" 的 parent 指向自己", nodes[i].name));
    }
    parent_of[i] = static_cast<int>(found->second);
  }

  // 环检测：沿父链走，步数超过节点数就说明有环
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    std::size_t steps = 0;
    int current = static_cast<int>(i);
    while (current >= 0) {
      current = parent_of[static_cast<std::size_t>(current)];
      if (++steps > nodes.size()) {
        return st::unexpected(ErrorCode::Invalid,
                              std::format("parent 引用形成了环（经过节点 \"{}\"）",
                                          nodes[i].name.empty() ? std::string("<未命名>")
                                                                : nodes[i].name));
      }
    }
  }

  // 搬迁：**从后往前**把子节点摘下来挂到父上。
  // 为什么从后往前：`std::vector<SceneNode>` 的 `children` 是值成员，摘一次
  // 就要搬一次元素；从后往前搬保证"还没搬的索引"不受影响（搬动只发生在
  // 更靠后的位置）。搬完把根节点收集起来。
  std::vector<std::vector<std::size_t>> children_of(nodes.size());
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (parent_of[i] >= 0) children_of[static_cast<std::size_t>(parent_of[i])].push_back(i);
  }
  std::vector<SceneNode> tree_roots;
  // 递归搬（深度已被上限约束）
  const auto graft = [&](auto&& self, std::size_t index) -> SceneNode {
    SceneNode node = std::move(nodes[index]);
    node.children.clear();
    for (const std::size_t child : children_of[index]) {
      node.children.push_back(self(self, child));
    }
    return node;
  };
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (parent_of[i] < 0) tree_roots.push_back(graft(graft, i));
  }
  if (tree_roots.empty() && !nodes.empty()) {
    return st::unexpected(ErrorCode::Invalid, "扁平节点列表里没有根节点（parent 全部非空）");
  }
  nodes = std::move(tree_roots);
  return st::ok();
}

// ────────────────────────────────────────────────────────────────────────────
// 烘焙
// ────────────────────────────────────────────────────────────────────────────

auto bake_into(std::vector<Mesh>& accumulator, const Mesh& mesh, const math::Mat4& world) -> void {
  if (mesh.is_empty()) return;
  // 只做一次变换（`mesh_ops::transform` 已处理法线的逆转置与镜像翻绕序）
  accumulator.push_back(transform(mesh, world));
}

}  // namespace st::raster
