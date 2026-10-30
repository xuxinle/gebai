#include "st/raster/scene_serialize.hpp"

#include <cmath>
#include <format>
#include <string>

#include "st/core/json.hpp"
#include "st/raster/animation.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"

namespace st::raster {

namespace {

/// 浮点输出：**去掉多余的尾零**（`1.0` 写成 `1`）。
///
/// 为什么值得专门处理：`1.0` 与 `1` 语义相同，但输出里满屏的 `.0` 会让
/// 模型读到的文本更长、更难扫。这不是微优化——往返文本是给模型读的。
[[nodiscard]] auto number_json(double value) -> Json {
  // ## 必须用 Json(...) 而不是 Json{...}
  //
  // nlohmann 的 `Json{x}` 会走**初始化列表**重载，把标量包成单元素数组
  //（`Json{42}` → `[42]`）。往返测试抓到了这个：序列化出的 `"eye":[[2.2],[1.6],[2.8]]`
  // 导致再解析时 "eye 与 target 重合"。
  //
  // 这是那种"看着能编译、输出也不报错，但语义完全错"的缺陷——
  // 只有往返测试（写出去再读回来）能发现它。
  const double rounded = std::round(value * 1.0e6) / 1.0e6;
  if (std::abs(rounded - std::round(rounded)) < 1.0e-9 && std::abs(rounded) < 9.0e15) {
    return Json(static_cast<std::int64_t>(std::llround(rounded)));
  }
  return Json(rounded);
}

[[nodiscard]] auto vec3_json(math::Vec3 value) -> Json {
  Json array = Json::array();
  array.push_back(number_json(value.x));
  array.push_back(number_json(value.y));
  array.push_back(number_json(value.z));
  return array;
}

[[nodiscard]] auto color_json(math::Color color) -> Json {
  return Json(std::format("#{:02x}{:02x}{:02x}", color.r, color.g, color.b));
}

[[nodiscard]] auto vec3_equals(math::Vec3 a, math::Vec3 b) noexcept -> bool {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

[[nodiscard]] auto color_equals(math::Color a, math::Color b) noexcept -> bool {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

/// 把"非默认值"才写入的辅助。
/// 返回值：是否需要这个字段。
template <typename T>
[[nodiscard]] auto differs(const T& value, const T& fallback) -> bool {
  return !(value == fallback);
}

[[nodiscard]] auto animation_json(const Clip& clip) -> Json {
  Json animation = Json::object();
  animation["name"] = clip.name;
  if (clip.duration() > 0.0f) animation["duration"] = number_json(clip.duration());
  Json tracks = Json::array();
  for (const Track& track : clip.tracks) {
    Json entry = Json::object();
    entry["node"] = track.node;
    entry["kind"] = std::string(track_kind_name(track.kind));
    Json keys = Json::array();
    for (const Keyframe& key : track.keys) {
      Json item = Json::object();
      item["t"] = number_json(key.time);
      item["value"] = vec3_json(key.value);
      if (key.easing != Easing::Linear) {
        item["easing"] = std::string(easing_name(key.easing));
      }
      keys.push_back(item);
    }
    entry["keys"] = keys;
    tracks.push_back(entry);
  }
  animation["tracks"] = tracks;
  return animation;
}

}  // namespace

auto serialize_object(const SceneObjectSpec& object) -> Json {
  // ## 只写非默认值 + 必须显式的项
  //
  // 默认值参照 `SceneObjectSpec` 的成员初值（那份初值是唯一真源，
  // 所以这里逐项对照而不是硬编码一份"默认表"——两份必然漂移）。
  const SceneObjectSpec defaults{};
  Json item = Json::object();

  if (!object.name.empty()) item["name"] = object.name;
  if (!object.parent.empty()) item["parent"] = object.parent;
  // `shape` 总要写：它是这个物体画什么的核心信息
  item["shape"] = object.shape;
  if (object.group) item["group"] = true;

  if (!vec3_equals(object.translate, defaults.translate)) {
    item["translate"] = vec3_json(object.translate);
  }
  // 写出时换回**度**（对调用方的单位）
  if (!vec3_equals(object.rotation, defaults.rotation)) {
    // 写出时换回**度**（脚本层单位）；内部是弧度
    constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;
    item["rotation"] = vec3_json(math::Vec3{object.rotation.x * kRadToDeg,
                                            object.rotation.y * kRadToDeg,
                                            object.rotation.z * kRadToDeg});
  }
  if (!vec3_equals(object.scale, defaults.scale)) item["scale"] = vec3_json(object.scale);
  if (!color_equals(object.color, defaults.color)) item["color"] = color_json(object.color);

  // 尺寸：只有 `size` 一条路（旧字段已删除，不必再考虑位掩码）
  if (object.size_count > 0U) {
    if (object.size_count == 1U) {
      item["size"] = number_json(object.size.x);
    } else {
      Json size_array = Json::array();
      size_array.push_back(number_json(object.size.x));
      size_array.push_back(number_json(object.size.y));
      if (object.size_count >= 3U) size_array.push_back(number_json(object.size.z));
      item["size"] = size_array;
    }
  }

  if (object.segments != defaults.segments) item["segments"] = object.segments;
  if (object.rings != defaults.rings) item["rings"] = object.rings;
  if (object.subdivisions != defaults.subdivisions) item["subdivisions"] = object.subdivisions;
  if (object.major_radius != defaults.major_radius) {
    item["major_radius"] = number_json(object.major_radius);
  }
  if (object.minor_radius != defaults.minor_radius) {
    item["minor_radius"] = number_json(object.minor_radius);
  }
  if (object.minor_segments != defaults.minor_segments) {
    item["minor_segments"] = object.minor_segments;
  }
  if (object.top_radius != defaults.top_radius) item["top_radius"] = number_json(object.top_radius);
  if (object.columns != defaults.columns) item["columns"] = object.columns;
  if (object.rows != defaults.rows) item["rows"] = object.rows;
  if (object.sides != defaults.sides) item["sides"] = object.sides;
  if (object.teeth != defaults.teeth) item["teeth"] = object.teeth;
  if (object.tooth_height != defaults.tooth_height) {
    item["tooth_height"] = number_json(object.tooth_height);
  }
  if (object.tooth_width != defaults.tooth_width) {
    item["tooth_width"] = number_json(object.tooth_width);
  }
  if (object.twist != defaults.twist) item["twist"] = number_json(object.twist);
  if (object.turns != defaults.turns) item["turns"] = number_json(object.turns);
  if (object.taper != defaults.taper) item["taper"] = number_json(object.taper);
  if (object.knot_p != defaults.knot_p) item["knot_p"] = object.knot_p;
  if (object.knot_q != defaults.knot_q) item["knot_q"] = object.knot_q;
  if (object.face_camera) item["face_camera"] = true;

  if (object.metallic != defaults.metallic) item["metallic"] = number_json(object.metallic);
  if (object.roughness != defaults.roughness) item["roughness"] = number_json(object.roughness);
  if (object.specular != defaults.specular) item["specular"] = number_json(object.specular);
  if (object.emission != defaults.emission) item["emission"] = number_json(object.emission);
  if (object.opacity != defaults.opacity) item["opacity"] = number_json(object.opacity);
  if (object.emissive_color_set) item["emissive_color"] = vec3_json(object.emissive_color);
  if (object.emission_unlit) item["emission_unlit"] = true;

  if (!object.gltf.empty()) item["gltf"] = object.gltf;
  if (object.gltf_use_material != defaults.gltf_use_material) {
    item["gltf_use_material"] = object.gltf_use_material;
  }
  if (object.ground) item["ground"] = true;
  if (!object.visible) item["visible"] = false;
  if (object.skin) item["skin"] = true;

  if (!vec3_equals(object.pivot, defaults.pivot)) item["pivot"] = vec3_json(object.pivot);
  if (!object.pivot_at.empty()) item["pivot_at"] = object.pivot_at;

  if (!object.texture.empty()) item["texture"] = object.texture;
  if (object.texture_scale_u != defaults.texture_scale_u) {
    item["texture_scale_u"] = number_json(object.texture_scale_u);
  }
  if (object.texture_scale_v != defaults.texture_scale_v) {
    item["texture_scale_v"] = number_json(object.texture_scale_v);
  }
  if (object.texture_offset_u != defaults.texture_offset_u) {
    item["texture_offset_u"] = number_json(object.texture_offset_u);
  }
  if (object.texture_offset_v != defaults.texture_offset_v) {
    item["texture_offset_v"] = number_json(object.texture_offset_v);
  }
  if (object.texture_clamp) item["texture_clamp"] = true;
  if (object.texture_alpha) item["texture_alpha"] = true;

  if (!object.children.empty()) {
    Json children = Json::array();
    for (const SceneObjectSpec& child : object.children) children.push_back(serialize_object(child));
    item["children"] = children;
  }
  return item;
}

auto serialize_scene(const SceneSpec& spec) -> Json {
  Json document = Json::object();
  const SceneSpec defaults{};

  // ── camera ──
  {
    Json camera = Json::object();
    camera["eye"] = vec3_json(spec.camera.eye);
    camera["target"] = vec3_json(spec.camera.target);
    if (!vec3_equals(spec.camera.up, defaults.camera.up)) {
      camera["up"] = vec3_json(spec.camera.up);
    }
    camera["fov"] = number_json(spec.camera.fov_y_degrees);
    if (spec.camera.near_z != defaults.camera.near_z) {
      camera["near"] = number_json(spec.camera.near_z);
    }
    if (spec.camera.far_z != defaults.camera.far_z) {
      camera["far"] = number_json(spec.camera.far_z);
    }
    if (spec.camera.projection != defaults.camera.projection) {
      camera["projection"] = spec.camera.projection == ProjectionKind::Orthographic
                                 ? "orthographic"
                                 : "perspective";
      if (spec.camera.projection == ProjectionKind::Orthographic) {
        camera["ortho_scale"] = number_json(spec.camera.ortho_scale);
      }
    }
    document["camera"] = camera;
  }

  // ── lighting ──
  {
    Json lighting = Json::object();
    lighting["light_from"] = vec3_json(spec.lighting.light_from);
    if (spec.lighting.ambient != defaults.lighting.ambient) {
      lighting["ambient"] = number_json(spec.lighting.ambient);
    }
    if (spec.lighting.diffuse != defaults.lighting.diffuse) {
      lighting["diffuse"] = number_json(spec.lighting.diffuse);
    }
    if (spec.lighting.rim != defaults.lighting.rim) {
      lighting["rim"] = number_json(spec.lighting.rim);
    }
    if (spec.lighting.exposure != defaults.lighting.exposure) {
      lighting["exposure"] = number_json(spec.lighting.exposure);
    }
    document["lighting"] = lighting;
  }

  // ── shadow ──
  if (spec.shadow.strength > 0.0f) {
    Json shadow = Json::object();
    shadow["strength"] = number_json(spec.shadow.strength);
    shadow["ground_y"] = number_json(spec.shadow.ground_y);
    if (spec.shadow.lift != defaults.shadow.lift) {
      shadow["lift"] = number_json(spec.shadow.lift);
    }
    document["shadow"] = shadow;
  }

  if (spec.cull_mode != defaults.cull_mode) {
    document["cull"] =
        spec.cull_mode == CullMode::None ? "none"
                                         : (spec.cull_mode == CullMode::Front ? "front" : "back");
  }
  if (!spec.asset_basis.empty()) document["asset_basis"] = spec.asset_basis;

  // ── skeleton ──
  if (spec.skeleton.has_value()) {
    Json skeleton = Json::object();
    Json joints = Json::array();
    for (const std::string& joint : spec.skeleton->joints) joints.push_back(joint);
    skeleton["joints"] = joints;
    if (!spec.skeleton->inverse_bind.empty()) {
      Json bindings = Json::array();
      for (const math::Mat4& matrix : spec.skeleton->inverse_bind) {
        Json values = Json::array();
        // 列主序（与解析口径一致）
        for (int column = 0; column < 4; ++column) {
          for (int row = 0; row < 4; ++row) values.push_back(number_json(matrix.value(row, column)));
        }
        bindings.push_back(values);
      }
      skeleton["inverse_bind"] = bindings;
    }
    document["skeleton"] = skeleton;
  }

  // ── animation ──
  if (spec.animation.has_value()) document["animation"] = animation_json(*spec.animation);

  // ── objects（必填，哪怕空数组）──
  Json objects = Json::array();
  for (const SceneObjectSpec& object : spec.objects) objects.push_back(serialize_object(object));
  document["objects"] = objects;

  return document;
}

auto serialize_scene_text(const SceneSpec& spec, bool pretty) -> std::string {
  return json_dump(serialize_scene(spec), pretty);
}

}  // namespace st::raster
