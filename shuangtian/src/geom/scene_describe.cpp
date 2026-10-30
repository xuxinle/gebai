#include "st/raster/scene_describe.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "st/core/json.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_graph.hpp"

namespace st::raster {

namespace {

/// 遍历 spec 里所有物体（含嵌套 children）。
template <typename Function>
void for_each_object(const std::vector<SceneObjectSpec>& objects, Function&& function) {
  for (const SceneObjectSpec& object : objects) {
    function(object);
    for_each_object(object.children, function);
  }
}

/// 8 个角变换后取并（与 `make_pick_object` 同一口径：旋转后不能只变换 min/max）。
[[nodiscard]] auto transformed_bounds(const Bounds& local, const math::Mat4& world) -> Bounds {
  if (local.is_empty()) return Bounds::empty();
  Bounds out = Bounds::empty();
  for (int mask = 0; mask < 8; ++mask) {
    const math::Vec3 corner{(mask & 1) != 0 ? local.max.x : local.min.x,
                            (mask & 2) != 0 ? local.max.y : local.min.y,
                            (mask & 4) != 0 ? local.max.z : local.min.z};
    const math::Vec3 moved{
        world.value(0, 0) * corner.x + world.value(0, 1) * corner.y +
            world.value(0, 2) * corner.z + world.value(0, 3),
        world.value(1, 0) * corner.x + world.value(1, 1) * corner.y +
            world.value(1, 2) * corner.z + world.value(1, 3),
        world.value(2, 0) * corner.x + world.value(2, 1) * corner.y +
            world.value(2, 2) * corner.z + world.value(2, 3)};
    out = out.union_with(moved);
  }
  return out;
}

}  // namespace

auto diagnostic_level_name(DiagnosticLevel level) noexcept -> std::string_view {
  switch (level) {
    case DiagnosticLevel::Info: return "info";
    case DiagnosticLevel::Warning: return "warning";
    case DiagnosticLevel::Error: return "error";
  }
  return "info";
}

auto SceneSummary::has_error() const noexcept -> bool {
  for (const Diagnostic& entry : diagnostics) {
    if (entry.level == DiagnosticLevel::Error) return true;
  }
  return false;
}

auto SceneSummary::has_warning() const noexcept -> bool {
  for (const Diagnostic& entry : diagnostics) {
    if (entry.level == DiagnosticLevel::Warning) return true;
  }
  return false;
}

auto describe_scene(const SceneSpec& spec) -> SceneSummary {
  SceneSummary summary{};

  // ── 造树：需要世界矩阵才能算包围盒 ──
  std::vector<SceneNode> roots;
  const auto to_node = [](const SceneObjectSpec& object, const auto& self) -> SceneNode {
    SceneNode node{};
    node.name = object.name;
    node.has_geometry = !object.group;
    node.local.translation = object.translate;
    node.local.rotation = object.rotate;
    node.local.scale = object.scale;
    node.pivot = object.pivot;
    node.visible = object.visible;
    for (const SceneObjectSpec& child : object.children) {
      node.children.push_back(self(child, self));
    }
    return node;
  };
  for (const SceneObjectSpec& object : spec.objects) roots.push_back(to_node(object, to_node));

  // 名字可能在 spec 里为空（未命名）——`resolve` 对空名生成 `#序号`，
  // 但这里只关心几何，所以空名不影响。
  const auto tree = resolve(roots);

  // ── 逐物体统计 ──
  std::vector<const SceneObjectSpec*> drawables;   // 可画几何的物体（按遍历顺序）
  for_each_object(spec.objects, [&](const SceneObjectSpec& object) {
    if (object.group) {
      ++summary.group_count;
    } else {
      ++summary.object_count;
      drawables.push_back(&object);
    }
    if (!object.texture.empty()) ++summary.textured_objects;
    if (object.ground) ++summary.ground_objects;
  });

  // 世界矩阵：按遍历顺序对齐（`resolve` 是深度优先先序，与 for_each_object 同序）
  // 这里用名字查更稳（空名会有 `#序号`，但那种情况下也只需近似统计）
  Bounds total = Bounds::empty();
  std::size_t drawable_index = 0;
  if (tree.has_value()) {
    for (const ResolvedNode& node : tree->nodes) {
      if (!node.has_geometry) continue;
      if (drawable_index >= drawables.size()) break;
      const SceneObjectSpec& object = *drawables[drawable_index];
      ++drawable_index;
      if (object.group) continue;

      const auto mesh = build_mesh(object);
      if (!mesh.has_value()) continue;
      summary.total_vertices += mesh->vertex_count();
      summary.total_triangles += mesh->indices.size() / 3U;
      // 地面节点在渲染时会去掉绕 X/Z 的倾斜（见 scene3d_script 的说明），
      // 摘要也按同一规则算，否则"报告说包围盒有这个高度、实际渲染没有"。
      math::Vec3 rotation = object.rotate;
      if (object.ground) rotation = math::Vec3{0.0f, rotation.y, 0.0f};
      const math::Mat4 local =
          math::Mat4::translation(object.translate) *
          math::Mat4::rotation(math::Vec3{1.0f, 0.0f, 0.0f}, rotation.x) *
          math::Mat4::rotation(math::Vec3{0.0f, 1.0f, 0.0f}, rotation.y) *
          math::Mat4::rotation(math::Vec3{0.0f, 0.0f, 1.0f}, rotation.z) *
          math::Mat4::scaling(object.scale);
      const math::Mat4 world = node.parent >= 0
                                   ? tree->nodes[static_cast<std::size_t>(node.parent)].world * local
                                   : local;
      total = total.union_with(transformed_bounds(bounds(*mesh), world));
    }
  }
  if (!total.is_empty()) {
    summary.bounds_min = total.min;
    summary.bounds_max = total.max;
    summary.bounds_valid = true;
    const math::Vec3 size = total.size();
    const float radius = std::max({size.x, size.y, size.z}) * 0.5f;
    // 由 fov 反解"让包围球占画面 62%"的距离（与 real_asset_probe 同一公式）
    const float half_fov = spec.camera.fov_y_degrees * 0.5f * 3.14159265f / 180.0f;
    const float tangent = std::tan(half_fov);
    summary.suggested_camera_distance =
        tangent > 1.0e-6f ? radius / (0.62f * tangent) : radius * 3.0f;
  }

  if (spec.animation.has_value()) {
    summary.animation_duration = spec.animation->duration();
    summary.animation_tracks = spec.animation->tracks.size();
  }

  // ── 诊断 ──
  const auto add = [&](DiagnosticLevel level, std::string code, std::string message,
                       std::string suggestion) {
    summary.diagnostics.push_back(
        Diagnostic{level, std::move(code), std::move(message), std::move(suggestion)});
  };

  // ① 完全空场景
  if (summary.object_count == 0U) {
    add(DiagnosticLevel::Error, "empty_scene",
        "场景里没有任何可画的物体（只有分组或 objects 为空）",
        "在 objects 里至少加一个 shape 不是 group 的物体");
  }

  // ② 相机在几何内部
  if (summary.bounds_valid) {
    const math::Vec3 eye = spec.camera.eye;
    const bool inside = eye.x > summary.bounds_min.x && eye.x < summary.bounds_max.x &&
                        eye.y > summary.bounds_min.y && eye.y < summary.bounds_max.y &&
                        eye.z > summary.bounds_min.z && eye.z < summary.bounds_max.z;
    if (inside) {
      add(DiagnosticLevel::Error, "camera_inside_geometry",
          std::format("相机 eye ({:.2f},{:.2f},{:.2f}) 落在场景包围盒内部",
                      static_cast<double>(eye.x), static_cast<double>(eye.y),
                      static_cast<double>(eye.z)),
          std::format("把 eye 往外挪到距中心约 {:.1f} 处（建议距离）",
                      static_cast<double>(summary.suggested_camera_distance)));
    }
    // ③ 相机太近 / 太远
    const math::Vec3 center{(summary.bounds_min.x + summary.bounds_max.x) * 0.5f,
                            (summary.bounds_min.y + summary.bounds_max.y) * 0.5f,
                            (summary.bounds_min.z + summary.bounds_max.z) * 0.5f};
    const math::Vec3 delta = eye - center;
    const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (!inside && summary.suggested_camera_distance > 0.0f) {
      const float ratio = distance / summary.suggested_camera_distance;
      if (ratio < 0.45f) {
        add(DiagnosticLevel::Warning, "camera_too_close",
            std::format("相机距场景中心 {:.2f}，比建议距离 {:.2f} 近很多（可能只看到局部）",
                        static_cast<double>(distance),
                        static_cast<double>(summary.suggested_camera_distance)),
            "把 eye 往外挪，或减小 fov");
      } else if (ratio > 3.0f) {
        add(DiagnosticLevel::Warning, "camera_too_far",
            std::format("相机距场景中心 {:.2f}，比建议距离 {:.2f} 远很多（物体可能很小）",
                        static_cast<double>(distance),
                        static_cast<double>(summary.suggested_camera_distance)),
            "把 eye 往内挪，或增大 fov");
      }
    }
    // ④ 近裁剪面会裁掉模型（**实测踩过的坑**：3cm 道具渲染出 0 像素）
    const math::Vec3 size = total.size();
    const float radius = std::max({size.x, size.y, size.z}) * 0.5f;
    if (spec.camera.near_z > radius * 0.5f && radius > 0.0f) {
      add(DiagnosticLevel::Warning, "near_plane_too_far",
          std::format("camera.near = {:.3f} 相对模型半径 {:.3f} 太大，模型可能被近裁剪面裁掉"
                      "（实测 3cm 道具会渲染出 0 像素）",
                      static_cast<double>(spec.camera.near_z), static_cast<double>(radius)),
          std::format("把 camera.near 调到 {:.4f} 或更小",
                      static_cast<double>(radius * 0.01f)));
    }
  }

  // ⑤ 有阴影但没有地面
  if (spec.shadow.strength > 0.0f && summary.ground_objects == 0U) {
    add(DiagnosticLevel::Info, "shadow_without_ground",
        "开了投影阴影，但没有标注 ground 的物体（阴影没有落点）",
        "加一个 plane 并设 \"ground\": true，或把 shadow.strength 设为 0");
  }

  // ⑥ 地面朝向被自动修正过
  for_each_object(spec.objects, [&](const SceneObjectSpec& object) {
    if (!object.ground) return;
    if (std::abs(object.rotate.x) > 1.0e-4f || std::abs(object.rotate.z) > 1.0e-4f) {
      add(DiagnosticLevel::Info, "ground_tilt_ignored",
          "ground 物体的绕 X/Z 旋转会被忽略（地面强制水平）",
          "如果确实要倾斜的面，不要标 ground");
    }
  });

  // ⑦ 贴图路径为空但写了贴图相关字段
  for_each_object(spec.objects, [&](const SceneObjectSpec& object) {
    if (object.texture.empty() &&
        (object.texture_scale_u != 1.0f || object.texture_scale_v != 1.0f ||
         object.texture_clamp)) {
      add(DiagnosticLevel::Warning, "texture_transform_without_texture",
          std::format("物体 {} 设了 UV 变换但没有 texture（变换不会生效）",
                      object.name.empty() ? "<未命名>" : object.name),
          "补上 texture 字段，或删掉那些 UV 变换字段");
    }
  });

  // ⑧ 动画轨道引用的节点不存在（渲染时才会报错，这里提前告知）
  if (spec.animation.has_value() && tree.has_value()) {
    for (const Track& track : spec.animation->tracks) {
      if (tree->find_by_name(track.node) == nullptr) {
        add(DiagnosticLevel::Error, "animation_missing_node",
            std::format("动画轨道引用了不存在的节点 \"{}\"", track.node),
            "检查 node 名与 objects[].name 是否一致（名字必须完全相同）");
      }
    }
  }

  // ⑨ 未命名物体（重名检查已在解析期做，这里只提示可维护性）
  std::size_t unnamed = 0;
  for_each_object(spec.objects, [&](const SceneObjectSpec& object) {
    if (object.name.empty()) ++unnamed;
  });
  if (unnamed > 0U && summary.object_count > 1U) {
    add(DiagnosticLevel::Info, "unnamed_objects",
        std::format("有 {} 个物体没写 name（动画/骨架/拾取都无法按名字引用它们）", unnamed),
        "给需要被引用的物体补上 name");
  }

  return summary;
}

auto summary_json(const SceneSummary& summary) -> Json {
  Json document = Json::object();
  Json facts = Json::object();
  facts["object_count"] = static_cast<std::int64_t>(summary.object_count);
  facts["group_count"] = static_cast<std::int64_t>(summary.group_count);
  facts["total_vertices"] = static_cast<std::int64_t>(summary.total_vertices);
  facts["total_triangles"] = static_cast<std::int64_t>(summary.total_triangles);
  facts["textured_objects"] = static_cast<std::int64_t>(summary.textured_objects);
  facts["ground_objects"] = static_cast<std::int64_t>(summary.ground_objects);
  facts["animation_duration"] = static_cast<double>(summary.animation_duration);
  facts["animation_tracks"] = static_cast<std::int64_t>(summary.animation_tracks);
  if (summary.bounds_valid) {
    Json bounds = Json::object();
    Json minimum = Json::array();
    minimum.push_back(static_cast<double>(summary.bounds_min.x));
    minimum.push_back(static_cast<double>(summary.bounds_min.y));
    minimum.push_back(static_cast<double>(summary.bounds_min.z));
    Json maximum = Json::array();
    maximum.push_back(static_cast<double>(summary.bounds_max.x));
    maximum.push_back(static_cast<double>(summary.bounds_max.y));
    maximum.push_back(static_cast<double>(summary.bounds_max.z));
    bounds["min"] = minimum;
    bounds["max"] = maximum;
    facts["bounds"] = bounds;
    facts["suggested_camera_distance"] = static_cast<double>(summary.suggested_camera_distance);
  }
  document["facts"] = facts;

  Json diagnostics = Json::array();
  for (const Diagnostic& entry : summary.diagnostics) {
    Json item = Json::object();
    item["level"] = std::string(diagnostic_level_name(entry.level));
    item["code"] = entry.code;
    item["message"] = entry.message;
    item["suggestion"] = entry.suggestion;
    diagnostics.push_back(item);
  }
  document["diagnostics"] = diagnostics;
  return document;
}

auto summary_text(const SceneSummary& summary) -> std::string {
  std::string text;
  text += std::format("物体 {} 个（分组 {} 个）· 顶点 {} · 三角形 {}\n", summary.object_count,
                      summary.group_count, summary.total_vertices, summary.total_triangles);
  if (summary.bounds_valid) {
    const math::Vec3 size{summary.bounds_max.x - summary.bounds_min.x,
                          summary.bounds_max.y - summary.bounds_min.y,
                          summary.bounds_max.z - summary.bounds_min.z};
    text += std::format("包围盒 ({:.2f},{:.2f},{:.2f}) – ({:.2f},{:.2f},{:.2f}) · 尺寸 "
                        "{:.2f}×{:.2f}×{:.2f}\n",
                        static_cast<double>(summary.bounds_min.x),
                        static_cast<double>(summary.bounds_min.y),
                        static_cast<double>(summary.bounds_min.z),
                        static_cast<double>(summary.bounds_max.x),
                        static_cast<double>(summary.bounds_max.y),
                        static_cast<double>(summary.bounds_max.z), static_cast<double>(size.x),
                        static_cast<double>(size.y), static_cast<double>(size.z));
    text += std::format("建议相机距离 {:.2f}\n",
                        static_cast<double>(summary.suggested_camera_distance));
  }
  if (summary.textured_objects > 0U) {
    text += std::format("带贴图物体 {} 个\n", summary.textured_objects);
  }
  if (summary.animation_duration > 0.0f) {
    text += std::format("动画 {} 段 · 时长 {:.2f}s\n", summary.animation_tracks,
                        static_cast<double>(summary.animation_duration));
  }
  if (summary.diagnostics.empty()) {
    text += "诊断：无问题\n";
    return text;
  }
  text += std::format("诊断 {} 条：\n", summary.diagnostics.size());
  for (const Diagnostic& entry : summary.diagnostics) {
    text += std::format("  [{}] {}：{}\n      建议：{}\n",
                        diagnostic_level_name(entry.level), entry.code, entry.message,
                        entry.suggestion);
  }
  return text;
}

}  // namespace st::raster
