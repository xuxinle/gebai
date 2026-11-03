#include "st/raster/scene3d_script.hpp"

#include "st/raster/gltf.hpp"
#include "st/codec/png.hpp"
#include "st/core/log.hpp"
#include "st/raster/animation.hpp"
#include "st/raster/scene_schema.hpp"
#include "st/raster/scene_graph.hpp"
#include "st/raster/scene_constraints.hpp"
#include "st/raster/mesh_ops.hpp"

#include <set>
#include "st/raster/primitives.hpp"

#include <cstdlib>
#include <cmath>
#include <string>

#include "st/core/fs.hpp"
#include "st/raster/mesh.hpp"

namespace st::raster {

namespace {

/// 度 → 弧度。
///
/// 脚本层用**度**（与人的直觉一致：写 90 就是转 90 度），
/// 内部数学层用弧度。转换点集中在这里，避免每处各写一遍常量。
[[nodiscard]] constexpr auto to_radians(float degrees) noexcept -> float {
  return degrees * 3.14159265358979323846f / 180.0f;
}

/// sRGB 字节 → **线性**浮点颜色（顶点色/材质色在下游全是线性）。
///
/// 直接用 `/255` 是错的：那是把 sRGB 值当线性值用，所有颜色会被洗亮。
[[nodiscard]] auto srgb_to_linear(st::math::Color color) noexcept -> math::Vec3 {
  const auto channel = [](std::uint8_t value) {
    const float srgb = static_cast<float>(value) / 255.0f;
    return srgb <= 0.04045f ? srgb / 12.92f
                            : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
  };
  return math::Vec3{channel(color.r), channel(color.g), channel(color.b)};
}

/// 弧度 → 度（序列化反向路径用；脚本 → JSON 的序列化尚未接线，函数语义已定型）。
[[nodiscard]] [[maybe_unused]] constexpr auto to_degrees(float radians) noexcept -> float {
  return radians * 180.0f / 3.14159265358979323846f;
}

// 消除 clang 的 unused-function（gcc 不告警）：`[[maybe_unused]]` 直接标在函数上，
// 序列化接线前没有人调它也不告警。
//（下一行的模板常量方案保留在历史里没有意义，已删。）

}  // namespace
namespace {

/// 从 JSON 里读一个三元组（支持 `[x,y,z]` 与 `{"x":…,"y":…,"z":…}` 两种写法）。
///
/// ## 为何两种都收
///
/// 数组更紧凑（手写方便），对象更自解释（模型生成时不易搞错顺序）。
/// 两种都在实际使用中出现过，强制一种只会让调用方多写一层转换。
auto read_vec3(const Json& parent, std::string_view key, math::Vec3 fallback,
               std::string_view path) -> Result<math::Vec3> {
  const Json* node = json_find(parent, key);
  if (node == nullptr) return fallback;
  if (node->is_array()) {
    const auto& arr = node->get_ref<const Json::array_t&>();
    if (arr.size() < 3) {
      return st::unexpected(ErrorCode::Parse,
                            std::string(path) + "." + std::string(key) + "：需要 3 个数字");
    }
    return math::Vec3{static_cast<float>(json_as_double(arr[0], 0.0)),
                      static_cast<float>(json_as_double(arr[1], 0.0)),
                      static_cast<float>(json_as_double(arr[2], 0.0))};
  }
  if (node->is_object()) {
    return math::Vec3{static_cast<float>(json_get_double(*node, "x", fallback.x)),
                      static_cast<float>(json_get_double(*node, "y", fallback.y)),
                      static_cast<float>(json_get_double(*node, "z", fallback.z))};
  }
  return st::unexpected(ErrorCode::Parse,
                        std::string(path) + "." + std::string(key) +
                            "：需要数组 [x,y,z] 或对象 {x,y,z}");
}

/// 解析颜色。支持 `"#rrggbb"` / `"#rrggbbaa"` / `[r,g,b]` / `[r,g,b,a]`。
///
/// ## 为何十六进制是首选
///
/// 设计稿给的是十六进制，而 `[200,180,160]` 这种写法**无法快速看出是什么颜色**。
/// 两者都支持，但文档里只推十六进制。
auto read_color(const Json& parent, std::string_view key, math::Color fallback,
                std::string_view path) -> Result<math::Color> {
  const Json* node = json_find(parent, key);
  if (node == nullptr) return fallback;
  if (node->is_string()) {
    const std::string text = node->get<std::string>();
    if (text.size() != 7 && text.size() != 9) {
      return st::unexpected(ErrorCode::Parse, std::string(path) + "." + std::string(key) +
                                                  "：颜色需为 #rrggbb 或 #rrggbbaa");
    }
    if (text[0] != '#') {
      return st::unexpected(ErrorCode::Parse, std::string(path) + "." + std::string(key) +
                                                  "：颜色需以 # 开头");
    }
    const auto hex = [&](std::size_t at) -> Result<int> {
      int value = 0;
      for (std::size_t i = at; i < at + 2; ++i) {
        const char ch = text[i];
        int digit = 0;
        if (ch >= '0' && ch <= '9') {
          digit = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
          digit = ch - 'a' + 10;
        } else if (ch >= 'A' && ch <= 'F') {
          digit = ch - 'A' + 10;
        } else {
          return st::unexpected(ErrorCode::Parse, std::string(path) + "." + std::string(key) +
                                                      "：颜色含非十六进制字符");
        }
        value = value * 16 + digit;
      }
      return value;
    };
    const auto r = hex(1);
    const auto g = hex(3);
    const auto b = hex(5);
    if (!r || !g || !b) return st::unexpected(ErrorCode::Parse, "颜色格式错误");
    int a = 255;
    if (text.size() == 9) {
      const auto av = hex(7);
      if (!av) return st::unexpected(ErrorCode::Parse, "颜色 alpha 格式错误");
      a = *av;
    }
    return math::Color{static_cast<std::uint8_t>(*r), static_cast<std::uint8_t>(*g),
                       static_cast<std::uint8_t>(*b), static_cast<std::uint8_t>(a)};
  }
  if (node->is_array()) {
    const auto& arr = node->get_ref<const Json::array_t&>();
    if (arr.size() < 3) {
      return st::unexpected(ErrorCode::Parse, std::string(path) + "." + std::string(key) +
                                                  "：需要 [r,g,b] 或 [r,g,b,a]");
    }
    const auto clamp255 = [](double v) {
      return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
    };
    const int a = arr.size() >= 4 ? clamp255(json_as_double(arr[3], 255.0)) : 255;
    return math::Color{clamp255(json_as_double(arr[0], 0.0)), clamp255(json_as_double(arr[1], 0.0)),
                       clamp255(json_as_double(arr[2], 0.0)), static_cast<std::uint8_t>(a)};
  }
  return st::unexpected(ErrorCode::Parse, std::string(path) + "." + std::string(key) +
                                              "：颜色需为 #rrggbb 字符串或 [r,g,b] 数组");
}

/// 读一个 0~1 的浮点（越界夹取，而不是报错）。
///
/// 为何夹取：这些是**比例参数**（metallic/roughness/opacity），传 1.2 的人
/// 想要的是"1"，报错只会让调用方多写一层 clamp。
auto read_unit(const Json& parent, std::string_view key, float fallback) -> float {
  return static_cast<float>(std::clamp(json_get_double(parent, key, fallback), 0.0, 1.0));
}

auto read_lighting(const Json& json) -> Result<Lighting> {
  Lighting lit{};
  const Json* node = json_find(json, "lighting");
  if (node == nullptr) return lit;
  if (!node->is_object()) {
    return st::unexpected(ErrorCode::Parse, "lighting：需要对象");
  }
  if (const auto* table = st::raster::find_field_table("lighting"); table != nullptr) {
    if (const auto status = st::raster::check_unknown_fields(*node, *table, "lighting"); !status) {
      return st::forward_error(status.error());
    }
  }
  const auto key_dir = read_vec3(*node, "light_from", lit.light_from, "lighting");
  if (!key_dir) return st::forward_error(key_dir.error());
  lit.light_from = *key_dir;
  lit.ambient = static_cast<float>(json_get_double(*node, "ambient", lit.ambient));
  lit.diffuse = static_cast<float>(json_get_double(*node, "diffuse", lit.diffuse));
  lit.rim = static_cast<float>(json_get_double(*node, "rim", lit.rim));
  lit.exposure = static_cast<float>(json_get_double(*node, "exposure", lit.exposure));
  // ## 环境反射（IBL）：金属"不发黑"的原因
  //
  // 这四个字段此前只存在于 C++ 的 `Lighting` 结构里，**没接到场景脚本**——
  // 于是剧本文件根本调不到，金属只能呈现"深色塑料"。
  // 实测症状：做相机时机身用深色 + metallic 0.85，渲出来是一块黑，
  // 因为金属没有漫反射、又没有环境可反射。
  if (const Json* sky = json_find(*node, "env_sky"); sky != nullptr) {
    const auto c = read_color(*node, "env_sky", math::Color{107, 133, 196, 255}, "lighting");
    if (!c) return st::forward_error(c.error());
    lit.env_sky = math::Vec3{static_cast<float>(c->r) / 255.0f,
                             static_cast<float>(c->g) / 255.0f,
                             static_cast<float>(c->b) / 255.0f};
  }
  if (const Json* ground = json_find(*node, "env_ground"); ground != nullptr) {
    const auto c = read_color(*node, "env_ground", math::Color{26, 26, 33, 255}, "lighting");
    if (!c) return st::forward_error(c.error());
    lit.env_ground = math::Vec3{static_cast<float>(c->r) / 255.0f,
                                static_cast<float>(c->g) / 255.0f,
                                static_cast<float>(c->b) / 255.0f};
  }
  lit.env_strength = static_cast<float>(json_get_double(*node, "env_strength", lit.env_strength));
  lit.env_rough_falloff =
      static_cast<float>(json_get_double(*node, "env_rough_falloff", lit.env_rough_falloff));

  const std::string tone = json_get_string(*node, "tone_map", "aces");
  if (tone == "none") {
    lit.tone_map = ToneMap::None;
  } else if (tone == "reinhard") {
    lit.tone_map = ToneMap::Reinhard;
  } else if (tone == "aces") {
    lit.tone_map = ToneMap::Aces;
  } else if (tone == "filmic") {
    lit.tone_map = ToneMap::Filmic;
  } else if (tone == "agx") {
    // Blender 4.x 的默认 view transform。高光不发彩、中间调略去饱和。
    lit.tone_map = ToneMap::Agx;
  } else {
    return st::unexpected(
        ErrorCode::Parse,
        "lighting.tone_map：只接受 none/reinhard/aces/filmic/agx，收到 " + tone);
  }

  const Json* extras = json_find(*node, "extra_lights");
  if (extras != nullptr) {
    if (!extras->is_array()) {
      return st::unexpected(ErrorCode::Parse, "lighting.extra_lights：需要数组");
    }
    const auto& arr = extras->get_ref<const Json::array_t&>();
    if (arr.size() > static_cast<std::size_t>(kMaxExtraLights)) {
      return st::unexpected(ErrorCode::Parse,
                            "lighting.extra_lights：最多 " + std::to_string(kMaxExtraLights) +
                                " 盏（常量缓冲固定大小），收到 " + std::to_string(arr.size()));
    }
    for (std::size_t i = 0; i < arr.size(); ++i) {
      const std::string path = "lighting.extra_lights[" + std::to_string(i) + "]";
      const auto dir = read_vec3(arr[i], "direction", math::Vec3{0.0f, 1.0f, 0.0f}, path);
      if (!dir) return st::forward_error(dir.error());
      lit.extra_lights[i].direction = *dir;
      lit.extra_lights[i].intensity =
          static_cast<float>(json_get_double(arr[i], "intensity", 0.0));
      const auto color = read_color(arr[i], "color", math::Color{255, 255, 255, 255}, path);
      if (!color) return st::forward_error(color.error());
      // 光源颜色在着色里是**线性**乘子：把 0~255 归一到 0~1
      lit.extra_lights[i].color = math::Vec3{static_cast<float>(color->r) / 255.0f,
                                       static_cast<float>(color->g) / 255.0f,
                                       static_cast<float>(color->b) / 255.0f};

      // ── 光源类型与位置（2026-10-24）──
      const std::string kind = json_get_string(arr[i], "type", "directional");
      if (kind == "directional" || kind == "sun") {
        lit.extra_lights[i].kind = LightKind::Directional;
      } else if (kind == "point") {
        lit.extra_lights[i].kind = LightKind::Point;
      } else if (kind == "spot") {
        lit.extra_lights[i].kind = LightKind::Spot;
      } else {
        return st::unexpected(ErrorCode::Parse,
                              path + ".type：只接受 directional/point/spot，收到 " + kind);
      }
      if (lit.extra_lights[i].kind != LightKind::Directional) {
        const auto pos = read_vec3(arr[i], "position", math::Vec3{0.0f, 2.0f, 2.0f}, path);
        if (!pos) return st::forward_error(pos.error());
        lit.extra_lights[i].position = *pos;
        lit.extra_lights[i].radius =
            static_cast<float>(json_get_double(arr[i], "radius", 0.0));
        lit.extra_lights[i].spot_inner_degrees =
            static_cast<float>(json_get_double(arr[i], "spot_inner", 20.0));
        lit.extra_lights[i].spot_outer_degrees =
            static_cast<float>(json_get_double(arr[i], "spot_outer", 30.0));
        if (lit.extra_lights[i].spot_outer_degrees <= lit.extra_lights[i].spot_inner_degrees) {
          return st::unexpected(ErrorCode::Parse,
                                path + ".spot_outer：必须大于 spot_inner（否则锥形无过渡区）");
        }
      }
    }
  }
  return lit;
}

auto read_shadow(const Json& json) -> Result<PlanarShadow> {
  PlanarShadow shadow{};
  const Json* node = json_find(json, "shadow");
  if (node == nullptr) return shadow;
  if (!node->is_object()) {
    return st::unexpected(ErrorCode::Parse, "shadow：需要对象");
  }
  if (const auto* table = st::raster::find_field_table("shadow"); table != nullptr) {
    if (const auto status = st::raster::check_unknown_fields(*node, *table, "shadow"); !status) {
      return st::forward_error(status.error());
    }
  }
  shadow.strength = read_unit(*node, "strength", 0.0f);
  shadow.ground_y = static_cast<float>(json_get_double(*node, "ground_y", 0.0));
  shadow.lift = static_cast<float>(json_get_double(*node, "lift", shadow.lift));
  const auto color = read_color(*node, "color", math::Color{0, 0, 0, 255}, "shadow");
  if (!color) return st::forward_error(color.error());
  shadow.color = math::Vec3{static_cast<float>(color->r) / 255.0f,
                      static_cast<float>(color->g) / 255.0f,
                      static_cast<float>(color->b) / 255.0f};
  return shadow;
}

/// JSON 值的类型名（用于报错时说明"收到的是什么"）。
///
/// 为什么要专门给中文类型名而不是 `json::value_t` 枚举：报错是给人/模型看的，
/// `number_integer` 这种名字对"我该写什么"没有帮助。
[[nodiscard]] auto json_type_name(const Json& value) -> std::string {
  if (value.is_null()) return "null";
  if (value.is_boolean()) return "布尔";
  if (value.is_number()) return "数字";
  if (value.is_string()) return "字符串";
  if (value.is_array()) return std::format("数组（{} 项）", value.size());
  if (value.is_object()) return "对象";
  return "未知";
}

/// 按字段表校验**已存在**字段的类型与范围。
///
/// ## 为什么必须校验类型（而不只是"给个默认值"）
///
/// `json_get_double(node, "radius", fallback)` 遇到 `"radius": "0.5"`（字符串）
/// 会**静默返回 fallback**。调用方看到"解析成功"，但值没生效——
/// 与大模型最常犯的"拼错字段"是同一类问题：**没有报错，所以无从修正**。
///
/// 所以这里显式检查：字段存在但类型不对 → 报错并指出期望类型。
///
/// 范围检查只覆盖"越界会静默变形"的几项（`segments >= 3` 之类）——
/// 完整范围校验留给 `describe_scene` 的"配置问题"提示，
/// 因为有些范围与形状相关（`segments` 对 cube 无意义），不该在这里硬卡。
[[nodiscard]] auto validate_field_types(const Json& node, const std::string& path) -> Status {
  const auto* table = st::raster::find_field_table("objects[]");
  if (table == nullptr) return st::ok();
  for (const st::raster::FieldSpec& field : table->fields) {
    const Json* value = json_find(node, field.name);
    if (value == nullptr) continue;
    const std::string& type = field.type;
    const std::string where = path + "." + field.name;
    if (type == "number") {
      // 允许整数与浮点；拒绝字符串与布尔（`true` 转成 1 是更常见的坑）
      if (!value->is_number()) {
        return st::unexpected(ErrorCode::Parse,
                              std::format("{}：期望数字（number），收到 {}；"
                                          "不要加引号，也不要用 true/false",
                                          where, json_type_name(*value)));
      }
    } else if (type == "bool") {
      if (!value->is_boolean()) {
        return st::unexpected(ErrorCode::Parse,
                              std::format("{}：期望布尔（true/false），收到 {}", where,
                                          json_type_name(*value)));
      }
    } else if (type == "string") {
      if (!value->is_string()) {
        return st::unexpected(ErrorCode::Parse,
                              std::format("{}：期望字符串（string），收到 {}", where,
                                          json_type_name(*value)));
      }
    } else if (type == "vec3") {
      if (!value->is_array() || value->size() != 3U) {
        return st::unexpected(ErrorCode::Parse,
                              std::format("{}：期望 3 个数字的数组 [x, y, z]，收到 {}", where,
                                          json_type_name(*value)));
      }
      for (std::size_t i = 0; i < 3U; ++i) {
        if (!(*value)[i].is_number()) {
          return st::unexpected(ErrorCode::Parse,
                                std::format("{}[{}]：期望数字，收到 {}", where, i,
                                            json_type_name((*value)[i])));
        }
      }
    } else if (type == "color") {
      // 颜色可以是 "#rrggbb" 字符串，也可以是 [r,g,b] 数组
      const bool as_string = value->is_string();
      const bool as_array = value->is_array() && value->size() >= 3U;
      if (!as_string && !as_array) {
        return st::unexpected(
            ErrorCode::Parse,
            std::format("{}：期望颜色（\"#rrggbb\" 或 [r,g,b]），收到 {}", where,
                        json_type_name(*value)));
      }
    }
  }
  // ## 范围检查：只覆盖"越界会静默变形"的整数分段
  //
  // 不做完整范围校验的理由：有些范围与形状相关（`segments` 对 cube 无意义），
  // 在解析期硬卡会误伤合法组合。这里只拦"无论什么形状都不合理"的几项。
  struct RangeCheck {
    const char* name;
    std::int64_t minimum;
  };
  const RangeCheck ranges[] = {{"segments", 3},   {"rings", 2},  {"minor_segments", 3},
                               {"subdivisions", 0}, {"columns", 1}, {"rows", 1},
                               {"sides", 3},        {"teeth", 3},  {"knot_p", 1},
                               {"knot_q", 1}};
  for (const RangeCheck& check : ranges) {
    const Json* value = json_find(node, check.name);
    if (value == nullptr || !value->is_number()) continue;
    const auto actual = static_cast<std::int64_t>(value->get<double>());
    if (actual < check.minimum) {
      return st::unexpected(
          ErrorCode::Parse,
          std::format("{}.{}：至少 {}, 收到 {}（负数或过小的分段会让几何退化成碎片）",
                      path, check.name, check.minimum, actual));
    }
  }
  return st::ok();
}

/// `SceneObjectSpec` → `SceneNode`（几何/材质留在 spec，空间信息进节点）。
///
/// 为什么要有这层桥：场景脚本的 `SceneObjectSpec` 同时承载"是什么"（形状/材质）
/// 与"在哪"（变换），而 `scene_graph` 只管"在哪"。两者职责不同但字段重叠，
/// 所以解析与渲染之间需要一个**显式**的转换点——而不是让两套结构互相 include。
[[nodiscard]] auto to_node(const SceneObjectSpec& object) -> st::raster::SceneNode {
  st::raster::SceneNode node{};
  node.name = object.name;
  // 布尔/阵列对象虽然 shape=group，但**几何由 children 运算产出**——
  // 它们是实体，不是层级容器（否则渲染会跳过，只看到空组）。
  node.has_geometry = !object.group || !object.op.empty() || !object.pattern.empty();
  node.local.translation = object.translate;
  node.local.rotation = object.rotation;
  node.local.scale = object.scale;
  node.pivot = object.pivot;
  node.visible = object.visible;
  node.children.reserve(object.children.size());
  for (const SceneObjectSpec& child : object.children) node.children.push_back(to_node(child));
  return node;
}

/// `SceneNode` → `SceneObjectSpec`（保留空间信息；几何/材质取默认，由渲染侧按名字回填）。
[[nodiscard]] auto from_node(const st::raster::SceneNode& node) -> SceneObjectSpec {
  SceneObjectSpec object{};
  object.name = node.name;
  object.group = !node.has_geometry;
  object.translate = node.local.translation;
  object.rotation = node.local.rotation;
  object.scale = node.local.scale;
  object.pivot = node.pivot;
  object.visible = node.visible;
  // ## `skin` / 形状 / 材质字段在树化时**只能从原始 spec 补回**
  //
  // `SceneNode` 只管空间（名字/变换/枢轴/可见性），不管"这个节点画什么、
  // 是否蒙皮"。所以 `from_node` 造出来的 spec 只有空间字段，几何字段是默认值。
  // 建树前必须把非空间字段搬过去——否则症状是"树化之后对象的形状/材质/蒙皮全丢"，
  // 而画面里只剩默认球体（或者干脆什么都不画）。
  // 这里留一个标记位，由调用方在建树后回填（见 `parse_scene_spec` 里那段）。
  object.children.reserve(node.children.size());
  for (const st::raster::SceneNode& child : node.children) object.children.push_back(from_node(child));
  return object;
}

auto read_object(const Json& node, std::size_t index) -> Result<SceneObjectSpec> {
  if (!node.is_object()) {
    return st::unexpected(ErrorCode::Parse,
                          "objects[" + std::to_string(index) + "]：需要对象");
  }
  const std::string path = "objects[" + std::to_string(index) + "]";
  // 未知字段检查（拼错 `raduis` 必须报错并给候选，不能静默用默认值）
  //
  // 注意 `children` 是嵌套的对象数组，它们各自在递归调用里检查——
  // 所以这里只查本层，不深入。
  if (const auto* table = st::raster::find_field_table("objects[]"); table != nullptr) {
    if (const auto status = st::raster::check_unknown_fields(node, *table, path); !status) {
      return st::forward_error(status.error());
    }
  }
  // 类型校验（`"radius": "0.5"` 必须报错，不能静默退回默认值）
  if (const auto status = validate_field_types(node, path); !status) {
    return st::forward_error(status.error());
  }
  SceneObjectSpec obj{};
  obj.shape = json_get_string(node, "shape", "sphere");

  const auto color = read_color(node, "color", obj.color, path);
  if (!color) return st::forward_error(color.error());
  obj.color = *color;

  const auto translate = read_vec3(node, "translate", obj.translate, path);
  if (!translate) return st::forward_error(translate.error());
  obj.translate = *translate;

  // `rotation` 是**度**（对调用方），内部统一换弧度。
  const auto rotation = read_vec3(node, "rotation", math::Vec3{}, path);
  if (!rotation) return st::forward_error(rotation.error());
  obj.rotation = math::Vec3{to_radians(rotation->x), to_radians(rotation->y),
                            to_radians(rotation->z)};

  const auto scale = read_vec3(node, "scale", obj.scale, path);
  if (!scale) return st::forward_error(scale.error());
  obj.scale = *scale;

  // ## 尺寸：`size` 是**唯一**写法（全长）
  //
  // 旧字段 radius/width/height/depth 已彻底删除——它们在同一形状下
  // 有时是半长有时是全长、有时完全无效（实测 `box` 的 height 无效、
  // `plane` 的 radius 是半长而 width 是全长），是"名字对但含义错"的根源。
  //
  // `size` 接受标量（球类是直径）或数组（按形状定顺序）。
  if (const Json* size_value = json_find(node, "size"); size_value != nullptr) {
    // `"auto"`（字符串或数组元素）：connect 的自动长度标记——语义在下面
    // 约束段校验（必须与 connect 同用）；这里放行，解算时填真值。
    const bool is_auto_string =
        size_value->is_string() && size_value->get<std::string>() == "auto";
    std::size_t auto_index = std::size_t(-1);  // 数组里 auto 的位置（本版仅 Y 项=1）
    if (size_value->is_array()) {
      for (std::size_t i = 0; i < size_value->size(); ++i) {
        if ((*size_value)[i].is_string() && (*size_value)[i].get<std::string>() == "auto") {
          auto_index = i;
        }
      }
    }
    if (size_value->is_number()) {
      obj.size.x = static_cast<float>(size_value->get<double>());
      obj.size_count = 1;
    } else if (size_value->is_array()) {
      if (size_value->empty() || size_value->size() > 3U) {
        return st::unexpected(
            ErrorCode::Parse,
            std::format("{}:size：数组需要 1~3 个数（按形状解释），收到 {} 项", path,
                        size_value->size()));
      }
      for (std::size_t i = 0; i < size_value->size(); ++i) {
        if (!(*size_value)[i].is_number() && i != auto_index) {
          return st::unexpected(ErrorCode::Parse,
                                std::format("{}:size[{}]：期望数字，收到 {}", path, i,
                                            json_type_name((*size_value)[i])));
        }
      }
      obj.size.x = static_cast<float>((*size_value)[0].get<double>());
      if (size_value->size() >= 2U) {
        obj.size.y = auto_index == 1U
                         ? 0.0f
                         : static_cast<float>((*size_value)[1].get<double>());
      }
      if (size_value->size() >= 3U) {
        obj.size.z = auto_index == 2U
                         ? 0.0f
                         : static_cast<float>((*size_value)[2].get<double>());
      }
      obj.size_count = static_cast<std::uint8_t>(size_value->size());
    } else if (!is_auto_string) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("{}:size：期望数字或数组，收到 {}", path,
                                        json_type_name(*size_value)));
    }
    // 尺寸必须为正（0 或负会让几何退化或翻面，且不会报错）；
    // auto 占位跳过（解算时填真值）。
    const bool has_auto = is_auto_string || auto_index != std::size_t(-1);
    if (!has_auto) {
      for (std::size_t i = 0; i < 3U; ++i) {
        const float value = (i == 0) ? obj.size.x : ((i == 1) ? obj.size.y : obj.size.z);
        if (i < obj.size_count && value <= 0.0f) {
          return st::unexpected(ErrorCode::Parse,
                                std::format("{}:size[{}]：必须 > 0（收到 {}）；"
                                            "size 是**全长**，不是半径",
                                            path, i, value));
        }
      }
    }
  }

  obj.segments = static_cast<int>(json_get_i64(node, "segments", obj.segments));
  // 图元库的新参数（语义与 `primitives::PrimitiveParams` 逐项一致）
  obj.rings = static_cast<int>(json_get_i64(node, "rings", obj.rings));
  obj.subdivisions = static_cast<int>(json_get_i64(node, "subdivisions", obj.subdivisions));
  obj.major_radius = static_cast<float>(json_get_double(node, "major_radius", obj.major_radius));
  obj.minor_radius = static_cast<float>(json_get_double(node, "minor_radius", obj.minor_radius));
  obj.minor_segments = static_cast<int>(json_get_i64(node, "minor_segments", obj.minor_segments));
  obj.top_radius = static_cast<float>(json_get_double(node, "top_radius", obj.top_radius));
  obj.columns = static_cast<int>(json_get_i64(node, "columns", obj.columns));
  obj.rows = static_cast<int>(json_get_i64(node, "rows", obj.rows));
  obj.teeth = static_cast<int>(json_get_i64(node, "teeth", obj.teeth));
  obj.tooth_height = static_cast<float>(json_get_double(node, "tooth_height", obj.tooth_height));
  obj.tooth_width = static_cast<float>(json_get_double(node, "tooth_width", obj.tooth_width));
  obj.twist = static_cast<float>(json_get_double(node, "twist", obj.twist));
  obj.turns = static_cast<float>(json_get_double(node, "turns", obj.turns));
  obj.taper = static_cast<float>(json_get_double(node, "taper", obj.taper));
  obj.thickness = static_cast<float>(json_get_double(node, "thickness", obj.thickness));
  obj.section = json_get_string(node, "section", obj.section);
  obj.knot_p = static_cast<int>(json_get_i64(node, "knot_p", obj.knot_p));
  obj.knot_q = static_cast<int>(json_get_i64(node, "knot_q", obj.knot_q));
  obj.face_camera = json_get_bool(node, "face_camera", obj.face_camera);
  obj.gltf = json_get_string(node, "gltf", obj.gltf);
  obj.gltf_use_material = json_get_bool(node, "gltf_use_material", obj.gltf_use_material);

  // ── 组合模型的空间体系 ──
  obj.name = json_get_string(node, "name", obj.name);
  obj.parent = json_get_string(node, "parent", obj.parent);
  obj.group = json_get_bool(node, "group", json_get_bool(node, "kind_is_group", obj.group));
  obj.visible = json_get_bool(node, "visible", obj.visible);
  obj.skin = json_get_bool(node, "skin", obj.skin);

  // ── 材质贴图 ──
  obj.texture = json_get_string(node, "texture", obj.texture);
  obj.texture_scale_u = static_cast<float>(json_get_double(node, "texture_scale_u", obj.texture_scale_u));
  obj.texture_scale_v = static_cast<float>(json_get_double(node, "texture_scale_v", obj.texture_scale_v));
  obj.texture_offset_u = static_cast<float>(json_get_double(node, "texture_offset_u", obj.texture_offset_u));
  obj.texture_offset_v = static_cast<float>(json_get_double(node, "texture_offset_v", obj.texture_offset_v));
  obj.texture_clamp = json_get_bool(node, "texture_clamp", obj.texture_clamp);
  obj.texture_alpha = json_get_bool(node, "texture_alpha", obj.texture_alpha);
  // `tile` 是便利字段：一个数同时设两个方向的平铺次数（最常见用法）
  if (const Json* tile = json_find(node, "texture_tile"); tile != nullptr && tile->is_number()) {
    const auto value = static_cast<float>(tile->get<double>());
    obj.texture_scale_u = value;
    obj.texture_scale_v = value;
  }
  obj.pivot_at = json_get_string(node, "pivot_at", obj.pivot_at);
  const auto pivot = read_vec3(node, "pivot", obj.pivot, path);
  if (!pivot) return st::forward_error(pivot.error());
  obj.pivot = *pivot;
  if (!obj.pivot_at.empty() && !st::raster::parse_pivot_preset(obj.pivot_at).has_value()) {
    return st::unexpected(
        ErrorCode::Parse,
        std::format("{}.pivot_at：只接受 {}，收到 \"{}\"", path,
                    st::raster::pivot_preset_names(), obj.pivot_at));
  }
  // ── 装配约束（attach/connect/align 互斥；解算在建树前，见 parse_scene_spec）──
  obj.attach = json_get_string(node, "attach", obj.attach);
  obj.connect = json_get_string(node, "connect", obj.connect);
  obj.align = json_get_string(node, "align", obj.align);
  obj.op = json_get_string(node, "op", obj.op);
  obj.profile = json_get_string(node, "profile", obj.profile);
  obj.pattern = json_get_string(node, "pattern", obj.pattern);
  obj.pattern_count = static_cast<int>(json_get_double(node, "pattern_count",
                                                       static_cast<double>(obj.pattern_count)));
  obj.pattern_radius = static_cast<float>(
      json_get_double(node, "pattern_radius", static_cast<double>(obj.pattern_radius)));
  obj.pattern_step =
      read_vec3(node, "pattern_step", obj.pattern_step, path).value_or(obj.pattern_step);
  if (!obj.pattern.empty()) {
    if (obj.pattern != "ring" && obj.pattern != "row") {
      return st::unexpected(ErrorCode::Parse,
                            std::format("{}.pattern：只接受 ring/row，收到 \"{}\"", path,
                                        obj.pattern));
    }
    if (obj.pattern_count < 1) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("{}.pattern_count：至少 1（收到 {}）", path,
                                        obj.pattern_count));
    }
    obj.shape = "group";
    obj.group = true;
  }
  if (!obj.op.empty()) {
    if (obj.op != "union" && obj.op != "subtract" && obj.op != "intersect") {
      return st::unexpected(ErrorCode::Parse,
                            std::format("{}.op：只接受 union/subtract/intersect，收到 \"{}\"",
                                        path, obj.op));
    }
    if (obj.shape != "group") {
      // op 对象自身不直接画几何——shape 强制 group（避免两个几何来源打架）。
      obj.shape = "group";
      obj.group = true;
    }
  }
  {
    int constraint_count = 0;
    if (!obj.attach.empty()) ++constraint_count;
    if (!obj.connect.empty()) ++constraint_count;
    if (!obj.align.empty()) ++constraint_count;
    if (constraint_count > 1) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("{}：attach/connect/align 三选一（同时写多个是建模错误；"
                                        "复杂关系用分组节点拆分）",
                                        path));
    }
  }
  // `size` 的 Y 项写 "auto"（字符串或数组元素）：connect 时长度自动 = 两端距离。
  // 在这里把标志位记下来、占位清零；解算时填真值（serialize 只写真值）。
  {
    const Json* size_node = json_find(node, "size");
    const bool whole_auto =
        size_node != nullptr && size_node->is_string() &&
        size_node->get<std::string>() == "auto";
    bool elem_auto = false;
    if (size_node != nullptr && size_node->is_array()) {
      for (const Json& element : *size_node) {
        if (element.is_string() && element.get<std::string>() == "auto") elem_auto = true;
      }
    }
    if (whole_auto || elem_auto) {
      obj.connect_auto_length = true;
      obj.size.y = 0.0f;
      obj.size_count = std::max<std::uint8_t>(obj.size_count, 2U);
    }
    // connect_auto_length 只在 connect 时有意义（别处写 auto 是拼错，报错）
    if (obj.connect_auto_length && obj.connect.empty()) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("{}.size：\"auto\" 只能与 connect 一起用（长度=两端距离）",
                                        path));
    }
  }
  if (const Json* child_list = json_find(node, "children"); child_list != nullptr) {
    if (!child_list->is_array()) {
      return st::unexpected(ErrorCode::Parse, path + ".children：需要数组");
    }
    std::size_t child_index = 0;
    for (const Json& child : *child_list) {
      const auto parsed = read_object(child, child_index);
      if (!parsed) return st::forward_error(parsed.error());
      if (!parsed->parent.empty()) {
        return st::unexpected(ErrorCode::Parse,
                              std::format("{}.children[{}].parent：嵌套写法里不能再写 parent"
                                          "（两种写法选一种）",
                                          path, child_index));
      }
      obj.children.push_back(*parsed);
      ++child_index;
    }
  }
  // ## 分组节点不必是合法图元名
  //
  // `group: true` 的节点自己不画几何，`shape` 只作为"若被当叶子看待时"的兜底。
  // 所以名字校验要对它放行——否则写 `{"name":"robot","group":true,"children":[...]}`
  // 会因为默认 `shape` 之外的值被拒，而那个值根本用不到。
  if (obj.group) return obj;
  obj.sides = static_cast<int>(json_get_i64(node, "sides", obj.sides));

  obj.ground = json_get_bool(node, "ground", false);
  // ## 地面去倾斜（兼容旧脚本的必经一笔）
  //
  // 旧语义里 `plane` 朝 +Z（立面），所以所有旧脚本都写 `"rotation": [-π/2,0,0]`
  // 把它扳成水平。现在 `plane` 图元**本就朝 +Y**，那 -90° 会把它立成墙——
  // 地面与投影阴影一起失效（实测：开关阴影的画面均值 99.063 → 99.063，完全不变）。
  //
  // 所以 `ground: true` 只保留**绕 Y 的朝向**：水平面绕竖直轴转向有意义，
  // 绕 X/Z 倾斜与"地面"语义冲突。旧脚本的意图（要一个水平地面）因此被保住。
  if (obj.ground) obj.rotation = math::Vec3{0.0f, obj.rotation.y, 0.0f};
  // ## 地面的朝向（兼容旧脚本的必经一笔）
  //
  // `ground: true` 的平面在旧语义里是"朝 +Z 的立面"，脚本普遍用
  // `"rotation": [-π/2, 0, 0]` 把它扳成水平。而现在的 `plane` 图元**本就朝 +Y**，
  // 再转 -90° 会变成竖直墙——地面与投影阴影一起失效。
  //
  // 所以地面节点在这里只保留绕 Y 的朝向（水平面绕竖直轴转向是有意义的，
  // 绕 X/Z 倾斜则与"地面"语义冲突）。旧脚本因此逐像素不变。
  obj.metallic = read_unit(node, "metallic", 0.0f);
  obj.roughness = read_unit(node, "roughness", 1.0f);
  obj.specular = read_unit(node, "specular", 0.0f);
  obj.emission = static_cast<float>(json_get_double(node, "emission", 0.0));
  obj.opacity = read_unit(node, "opacity", 1.0f);
  // 自发光色与“不受光”开关（对齐 Blender 的 Emission Color / Strength 分离）
  //
  // ## 字段名以 `emissive_color` 为准，旧名 `emission_color` 保留兼容
  //
  // 实测踩过（2026-10-31）：schema 校验器声明的名字是 **`emissive_color`**，
  // 而这里原本读的是 **`emission_color`** —— 两者不一致的后果很阴：
  // 写 `emissive_color` **能过校验**，但自发光完全不生效
  //（`emissive_color_set` 恒为 false → 退回白色发光 → 屏幕一片灰白）。
  if (const Json* emissive = json_find(node, "emissive_color"); emissive != nullptr) {
    const auto c = read_color(node, "emissive_color", math::Color{255, 255, 255, 255}, path);
    if (!c) return st::forward_error(c.error());
    obj.emissive_color =
        math::Vec3{static_cast<float>(c->r) / 255.0f, static_cast<float>(c->g) / 255.0f,
                   static_cast<float>(c->b) / 255.0f};
    obj.emissive_color_set = true;
  } else if (const Json* legacy = json_find(node, "emission_color"); legacy != nullptr) {
    // 旧名兼容（上面的注释解释了为什么两个名字都存在）
    const auto c = read_color(node, "emission_color", math::Color{255, 255, 255, 255}, path);
    if (!c) return st::forward_error(c.error());
    obj.emissive_color =
        math::Vec3{static_cast<float>(c->r) / 255.0f, static_cast<float>(c->g) / 255.0f,
                   static_cast<float>(c->b) / 255.0f};
    obj.emissive_color_set = true;
  }
  obj.emission_unlit = json_get_bool(node, "emission_unlit", false);

  // ## 形状名要在这里校验，而不是等到 build_mesh
  //
  // 因为 build_mesh 的报错里没有对象索引，调用方（尤其生成代码的模型）
  // 拿不到"是第几个对象写错了"。在解析阶段就拦住，信息最全。
  //
  // `obj:` 前缀不在这张表里：它后面跟的是**任意路径**，不是枚举名。
  // 所以单独按前缀判定（见下），而不是往表里塞一个永远匹配不上的字串。
  // 形状白名单：与 `primitives` 的规范名一致（外加历史别名 sphere/box）。
  // ## 为什么允许别名
  //
  // `sphere` / `box` 是既有脚本里已经在用的名字（裁云图层、旧探针），改掉会让
  // **已经能出片的 JSON 突然报错**。别名映射到同一个生成器，行为逐像素不变。
  static constexpr const char* kShapes[] = {
      "sphere",   "box",         "plane",   "cylinder", "torus",     "uv_sphere",
      "cube",     "ico_sphere",  "cone",    "capsule",   "disc",     "circle",
      "grid",     "tube",        "prism",   "gear",      "torus_knot", "arrow",
      "wedge",    "tetrahedron", "octahedron", "dodecahedron", "icosahedron", "helix",
      "extrude",  "revolve",     "beam",     "staircase",
      "ornament", "fastener"};
  const bool is_obj_shape = obj.shape.rfind("obj:", 0) == 0;
  // 给了 `gltf` 时 `shape` 不参与几何选择（它只作为兜底默认值存在），
  // 所以不必强制它是内置图元名——否则写 `{"gltf": "a.glb"}` 会因为默认 `sphere`
  // 之外的值被拒绝，而那个值根本没用到。
  bool known = is_obj_shape || obj.gltf.rfind("gltf:", 0) == 0 || obj.group;
  for (const char* name : kShapes) {
    if (obj.shape == name) {
      known = true;
      break;
    }
  }
  if (is_obj_shape && obj.shape.size() <= 4) {
    return st::unexpected(ErrorCode::Parse, path + ".shape：`obj:` 后面需要文件路径");
  }
  if (!known) {
    std::string allowed;
    for (const char* name : kShapes) {
      if (!allowed.empty()) allowed += "/";
      allowed += name;
    }
    return st::unexpected(ErrorCode::Parse,
                          path + ".shape：只接受 " + allowed + " 或 obj:<路径>，收到 " +
                              obj.shape);
  }
  return obj;
}
}  // namespace

/// ── 装配约束解算（解析后、建树前；详见 scene_constraints.hpp）──
///
/// 遍历顺序：先序（父与兄先于子）。每解算一个对象，把它的**世界包围盒**
/// 登记进缓存——后续约束引用它时拿到的已是解算后的位形。
/// 前向引用在查缓存时自然报错（目标不在缓存里 = 尚未解算）。
///
/// ## 为什么世界包围盒增量登记能支撑「父动子动」
///
/// 子对象的约束引用父对象时，父的包围盒**含其局部 TRS**；解算发生在建树前，
/// 所以「世界」在这里等价于「顶层 spec 的展平坐标系」。若约束对象自身有父
/// （嵌套写法），约定：**约束一律在展平坐标系解算，然后写回顶层 translate**
/// ——即约束对象的局部 translate 被覆盖为世界值。带变换父的子对象要约束时，
/// 请用扁平写法（parent 留空、位置全由约束接管）——这条限制刻意保留，
/// 避免「父变换与约束叠加」的歧义。
auto solve_constraints(std::vector<SceneObjectSpec>& objects,
                       const std::string& asset_basis) -> Status {
  // 名字 → 世界包围盒（已解算部分）。
  std::map<std::string, st::raster::Bounds> solved_boxes;

  // 单对象的局部包围盒（几何或分组并集；不含自身 TRS——加 TRS 得世界盒）。
  const auto local_bounds_of = [&asset_basis](auto&& self, const SceneObjectSpec& object)
      -> st::raster::Bounds {
    st::raster::Bounds box = st::raster::Bounds::empty();
    if (!object.group || !object.op.empty() || !object.pattern.empty()) {
      SceneObjectSpec probe = object;
      if (object.shape.rfind("obj:", 0) == 0) {
        probe.shape = "obj:" + resolve_shape_path(object.shape.substr(4), asset_basis);
      }
      if (!object.gltf.empty()) {
        const std::string reference =
            object.gltf.rfind("gltf:", 0) == 0 ? object.gltf.substr(5) : object.gltf;
        probe.shape = "gltf:" + resolve_shape_path(reference, asset_basis);
      }
      if (probe.shape.rfind("gltf:", 0) != 0) {
        if (const auto mesh = build_mesh(probe); mesh.has_value()) {
          box = st::raster::bounds(*mesh);
        }
      }
    }
    for (const SceneObjectSpec& child : object.children) {
      const st::raster::Bounds child_box = self(self, child);
      if (child_box.is_empty()) continue;
      const st::raster::SceneNode child_node = to_node(child);
      const auto matrix = st::raster::local_matrix(child_node);
      for (int mask = 0; mask < 8; ++mask) {
        const math::Vec3 corner{(mask & 1) != 0 ? child_box.max.x : child_box.min.x,
                                (mask & 2) != 0 ? child_box.max.y : child_box.min.y,
                                (mask & 4) != 0 ? child_box.max.z : child_box.min.z};
        box = box.union_with(math::Vec3{
            matrix.value(0, 0) * corner.x + matrix.value(0, 1) * corner.y +
                matrix.value(0, 2) * corner.z + matrix.value(0, 3),
            matrix.value(1, 0) * corner.x + matrix.value(1, 1) * corner.y +
                matrix.value(1, 2) * corner.z + matrix.value(1, 3),
            matrix.value(2, 0) * corner.x + matrix.value(2, 1) * corner.y +
                matrix.value(2, 2) * corner.z + matrix.value(2, 3)});
      }
    }
    return box;
  };

  // 约束解算上下文：查已解算对象的世界锚点/包围盒。
  const st::raster::ConstraintContext context{
      .world_bounds_of =
          [&](const std::string& name) -> std::optional<st::raster::Bounds> {
            const auto found = solved_boxes.find(name);
            if (found == solved_boxes.end()) return std::nullopt;
            return found->second;
          },
      .world_anchor_of =
          [&](const std::string& name,
              st::raster::AnchorKind anchor) -> std::optional<math::Vec3> {
            const auto found = solved_boxes.find(name);
            if (found == solved_boxes.end()) return std::nullopt;
            return st::raster::anchor_of_bounds(found->second, anchor);
          },
  };

  // 先序遍历解算（递归 lambda；每对象：解算自身约束 → 登记世界盒 → 递归子）。
  const auto walk = [&](auto&& self, SceneObjectSpec& object,
                        const std::string& path) -> Status {
    const bool has_constraint =
        !object.attach.empty() || !object.connect.empty() || !object.align.empty();
    if (has_constraint) {
      // 我的局部几何包围盒（锚点差量计算用；未旋转时局部锚点=世界锚点-平移）。
      const st::raster::Bounds my_box = local_bounds_of(local_bounds_of, object);
      if (!object.attach.empty()) {
        // 语法：`[anchor_of:]target[+off]`——锚点词描述【目标】的哪个面
        //（读作「贴到桌子的顶面」）。我的锚点固定用 bottom（坐上去）
        // 或 center（用 center_of: 前缀显式写）。
        std::string target_text = object.attach;
        st::raster::AnchorKind my_anchor{st::raster::AnchorKind::Bottom};
        // 前缀 `center_of:`/`bottom_of:`/`top_of:` 出现在**开头**时是我的锚点
        //（读作「我的中心对准…」）；否则整个串是目标描述（锚点属于目标）。
        for (const std::string_view word : {"bottom", "center", "top", "left",
                                            "right", "front", "back"}) {
          const std::string prefix = std::string(word) + "_to:";
          if (target_text.rfind(prefix, 0) == 0) {
            const auto parsed = st::raster::AnchorKind::parse(word);
            if (parsed) my_anchor = *parsed;
            target_text = target_text.substr(prefix.size());
            break;
          }
        }
        const auto target = st::raster::parse_anchor_ref(target_text);
        if (!target) return st::forward_error(target.error());
        const st::raster::AnchorRef mine_ref{.anchor = my_anchor, .target = object.name};
        const auto world_point = st::raster::solve_attach(mine_ref, *target, context);
        if (!world_point) return st::forward_error(world_point.error());
        // 差量 = 目标世界点 - 我的锚点局部偏移（约束对象在展平系；translate 直接覆盖）。
        const math::Vec3 mine_local =
            my_box.is_empty() ? math::Vec3{} : st::raster::anchor_of_bounds(my_box, my_anchor);
        object.translate = *world_point - mine_local;
      } else if (!object.connect.empty()) {
        // 语法：`A -> B`（箭头分隔两端；每端是 `[anchor_of:]name[+off]`，
        // 省略 anchor 默认 A=bottom、B=top）。用箭头而不是冒号：
        // 端内的锚点已经用了冒号（`top_of:lamp`），再用冒号分隔端会歧义。
        constexpr std::string_view kArrow = " -> ";
        const auto sep = object.connect.find("->");
        if (sep == std::string::npos) {
          return st::unexpected(ErrorCode::Parse,
                                path + ".connect：要写 \"A -> B\"（箭头分隔两个锚点，"
                                        "如 \"col_cap -> reactor_base\"）");
        }
        const std::string from_text = object.connect.substr(0, sep);
        const std::string to_text = object.connect.substr(sep + kArrow.size() - 2);
        const auto trim = [](std::string s) {
          const auto first = s.find_first_not_of(" \t");
          const auto last = s.find_last_not_of(" \t");
          return first == std::string::npos ? std::string{} : s.substr(first, last - first + 1);
        };
        const auto from = st::raster::parse_anchor_ref(trim(from_text));
        if (!from) return st::forward_error(from.error());
        const auto to = st::raster::parse_anchor_ref(trim(to_text));
        if (!to) return st::forward_error(to.error());
        const auto solution = st::raster::solve_connect(*from, *to, context);
        if (!solution) return st::forward_error(solution.error());
        object.translate = solution->midpoint;
        // rotation_deg 是度；spec 内部存弧度（序列化层才转回度——
        // 与手写 JSON rotation 的解析同一条路）。
        constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
        object.rotation = st::math::Vec3{
            solution->rotation_deg.x * kDeg2Rad,
            solution->rotation_deg.y * kDeg2Rad,
            solution->rotation_deg.z * kDeg2Rad};
        if (object.connect_auto_length) {
          object.size.y = solution->length;
          object.size_count = std::max<std::uint8_t>(object.size_count, 2U);
          object.connect_auto_length = false;  // 已填真值；serialize 写数值
        }
      } else if (!object.align.empty()) {
        // 语法：`axis:target[+off]`（axis 关键字固定）。
        std::string target_text = object.align;
        if (target_text.rfind("axis:", 0) == 0) target_text = target_text.substr(5);
        const auto target = st::raster::parse_anchor_ref(target_text);
        if (!target) return st::forward_error(target.error());
        const auto solved = st::raster::solve_align(*target, object.translate.y, context);
        if (!solved) return st::forward_error(solved.error());
        object.translate = *solved;
      }
    }
    // 登记我的世界包围盒（含子树、含刚解算的 translate）。
    {
      st::raster::Bounds world = st::raster::Bounds::empty();
      const st::raster::Bounds box = local_bounds_of(local_bounds_of, object);
      if (!box.is_empty()) {
        // 旋转/缩放对包围盒的影响：取八角保守包围（约束对象通常轴对齐，
        // 但 connect 会写旋转——保守包围足够后续 attach 引用）。
        const st::raster::SceneNode node = to_node(object);
        const auto matrix = st::raster::local_matrix(node);
        for (int mask = 0; mask < 8; ++mask) {
          const math::Vec3 corner{(mask & 1) != 0 ? box.max.x : box.min.x,
                                  (mask & 2) != 0 ? box.max.y : box.min.y,
                                  (mask & 4) != 0 ? box.max.z : box.min.z};
          world = world.union_with(math::Vec3{
              matrix.value(0, 0) * corner.x + matrix.value(0, 1) * corner.y +
                  matrix.value(0, 2) * corner.z + matrix.value(0, 3),
              matrix.value(1, 0) * corner.x + matrix.value(1, 1) * corner.y +
                  matrix.value(1, 2) * corner.z + matrix.value(1, 3),
              matrix.value(2, 0) * corner.x + matrix.value(2, 1) * corner.y +
                  matrix.value(2, 2) * corner.z + matrix.value(2, 3)});
        }
      }
      if (!object.name.empty()) solved_boxes.emplace(object.name, world);
    }
    std::size_t child_index = 0;
    for (SceneObjectSpec& child : object.children) {
      const auto status = self(self, child,
                               std::format("{}/{}[{}]", path, child.name, child_index));
      if (!status) return st::forward_error(status.error());
      ++child_index;
    }
    return st::ok();
  };

  std::size_t index = 0;
  for (SceneObjectSpec& object : objects) {
    const auto status =
        walk(walk, object, std::format("objects[{}]", index));
    if (!status) return st::forward_error(status.error());
    ++index;
  }
  return st::ok();
}

auto parse_scene_spec(const Json& json) -> Result<SceneSpec> {
  if (!json.is_object()) {
    return st::unexpected(ErrorCode::Parse, "场景描述：顶层需要对象");
  }
  // ## 先查未知字段（在解析任何内容之前）
  //
  // 顺序很重要：若先解析再检查，拼错的字段会先让某处退回默认值、
  // 然后才报"未知字段"——两条错误一起出现，调用方反而不知道哪个是真的。
  // 放在最前面，报错就只有一个来源。
  if (const auto* table = st::raster::find_field_table("scene");
      table != nullptr) {
    if (const auto status = st::raster::check_unknown_fields(json, *table, "scene"); !status) {
      return st::forward_error(status.error());
    }
  }
  SceneSpec spec{};

  const Json* camera = json_find(json, "camera");
  if (camera != nullptr) {
    if (!camera->is_object()) {
      return st::unexpected(ErrorCode::Parse, "camera：需要对象");
    }
    if (const auto* table = st::raster::find_field_table("camera"); table != nullptr) {
      if (const auto status = st::raster::check_unknown_fields(*camera, *table, "camera"); !status) {
        return st::forward_error(status.error());
      }
    }
    const auto eye = read_vec3(*camera, "eye", spec.camera.eye, "camera");
    if (!eye) return st::forward_error(eye.error());
    spec.camera.eye = *eye;
    const auto target = read_vec3(*camera, "target", spec.camera.target, "camera");
    if (!target) return st::forward_error(target.error());
    spec.camera.target = *target;
    const auto up = read_vec3(*camera, "up", spec.camera.up, "camera");
    if (!up) return st::forward_error(up.error());
    spec.camera.up = *up;
    spec.camera.fov_y_degrees =
        static_cast<float>(json_get_double(*camera, "fov", spec.camera.fov_y_degrees));
    spec.camera.near_z =
        static_cast<float>(json_get_double(*camera, "near", spec.camera.near_z));
    spec.camera.far_z =
        static_cast<float>(json_get_double(*camera, "far", spec.camera.far_z));
    // 投影类型：perspective（默认）/ orthographic。
    //
    // 正交下 `fov` 失效，由 `ortho_scale`（垂直可视高度，世界单位）决定。
    const std::string proj = json_get_string(*camera, "projection", "perspective");
    if (proj == "perspective") {
      spec.camera.projection = ProjectionKind::Perspective;
    } else if (proj == "orthographic" || proj == "ortho") {
      spec.camera.projection = ProjectionKind::Orthographic;
    } else {
      return st::unexpected(ErrorCode::Parse,
                            "camera.projection：只接受 perspective/orthographic，收到 " + proj);
    }
    spec.camera.ortho_scale =
        static_cast<float>(json_get_double(*camera, "ortho_scale", spec.camera.ortho_scale));
    if (spec.camera.ortho_scale <= 0.0f) {
      return st::unexpected(ErrorCode::Parse, "camera.ortho_scale：必须为正（垂直可视高度）");
    }
    // 相机退化检查：eye == target 时 look_at 会产生 NaN 基向量，
  // 整幅画面变黑且没有任何报错（实测过这类问题）。
    const math::Vec3 eye_delta = spec.camera.eye - spec.camera.target;
    if (eye_delta.dot(eye_delta) < 1e-12f) {
      return st::unexpected(ErrorCode::Invalid,
                            "camera：eye 与 target 重合，无法确定朝向");
    }
  }

  const auto lighting = read_lighting(json);
  if (!lighting) return st::forward_error(lighting.error());
  spec.lighting = *lighting;

  const auto shadow = read_shadow(json);
  if (!shadow) return st::forward_error(shadow.error());
  spec.shadow = *shadow;

  const std::string cull = json_get_string(json, "cull", "back");
  if (cull == "none") {
    spec.cull_mode = CullMode::None;
  } else if (cull == "back") {
    spec.cull_mode = CullMode::Back;
  } else if (cull == "front") {
    spec.cull_mode = CullMode::Front;
  } else {
    return st::unexpected(ErrorCode::Parse,
                          "cull：只接受 none/back/front，收到 " + cull);
  }

  // ── 骨架（可选）──
  if (const Json* skeleton = json_find(json, "skeleton"); skeleton != nullptr) {
    if (!skeleton->is_object()) {
      return st::unexpected(ErrorCode::Parse, "skeleton：需要对象");
    }
    if (const auto* table = st::raster::find_field_table("skeleton"); table != nullptr) {
      if (const auto status = st::raster::check_unknown_fields(*skeleton, *table, "skeleton");
          !status) {
        return st::forward_error(status.error());
      }
    }
    st::raster::Skeleton parsed{};
    const Json* joints = json_find(*skeleton, "joints");
    if (joints == nullptr || !joints->is_array()) {
      return st::unexpected(ErrorCode::Parse, "skeleton.joints：需要数组（关节名，与节点名一致）");
    }
    std::size_t index = 0;
    for (const Json& joint : *joints) {
      if (!joint.is_string()) {
        return st::unexpected(ErrorCode::Parse,
                              std::format("skeleton.joints[{}]：需要字符串（关节名）", index));
      }
      parsed.joints.push_back(joint.get<std::string>());
      ++index;
    }
    if (parsed.joints.empty()) {
      return st::unexpected(ErrorCode::Parse, "skeleton.joints：至少要一个关节");
    }
    // 逆绑定矩阵（可选）：每个关节 16 个数，**列主序**（与 Mat4 相同）
    if (const Json* bind = json_find(*skeleton, "inverse_bind"); bind != nullptr) {
      if (!bind->is_array() || bind->size() != parsed.joints.size()) {
        return st::unexpected(
            ErrorCode::Parse,
            std::format("skeleton.inverse_bind：需要与 joints 等长的数组（{} 个矩阵），收到 {} 个",
                        parsed.joints.size(), bind->is_array() ? bind->size() : 0U));
      }
      std::size_t matrix_index = 0;
      for (const Json& matrix : *bind) {
        if (!matrix.is_array() || matrix.size() != 16U) {
          return st::unexpected(
              ErrorCode::Parse,
              std::format("skeleton.inverse_bind[{}]：需要 16 个数（列主序 4×4）", matrix_index));
        }
        math::Mat4 value = math::Mat4::identity();
        for (int column = 0; column < 4; ++column) {
          for (int row = 0; row < 4; ++row) {
            const Json& element = matrix[static_cast<std::size_t>(column * 4 + row)];
            if (!element.is_number()) {
              return st::unexpected(
                  ErrorCode::Parse,
                  std::format("skeleton.inverse_bind[{}][{}]：需要数字", matrix_index,
                              column * 4 + row));
            }
            value.set(row, column, static_cast<float>(element.get<double>()));
          }
        }
        parsed.inverse_bind.push_back(value);
        ++matrix_index;
      }
    }
    spec.skeleton = std::move(parsed);
  }

  // ── 动画（可选；一段）──  // ── 动画（可选；一段）──
  if (const Json* animation = json_find(json, "animation"); animation != nullptr) {
    if (!animation->is_object()) {
      return st::unexpected(ErrorCode::Parse, "animation：需要对象");
    }
    if (const auto* table = st::raster::find_field_table("animation"); table != nullptr) {
      if (const auto status = st::raster::check_unknown_fields(*animation, *table, "animation");
          !status) {
        return st::forward_error(status.error());
      }
    }
    st::raster::Clip clip{};
    clip.name = json_get_string(*animation, "name", "animation");
    if (const Json* tracks = json_find(*animation, "tracks"); tracks != nullptr) {
      if (!tracks->is_array()) {
        return st::unexpected(ErrorCode::Parse, "animation.tracks：需要数组");
      }
      std::size_t track_index = 0;
      for (const Json& track_node : *tracks) {
        const std::string path = std::format("animation.tracks[{}]", track_index);
        ++track_index;
        if (const auto* table = st::raster::find_field_table("animation.tracks[]");
            table != nullptr) {
          if (const auto status = st::raster::check_unknown_fields(track_node, *table, path);
              !status) {
            return st::forward_error(status.error());
          }
        }
        st::raster::Track track{};
        track.node = json_get_string(track_node, "node", "");
        if (track.node.empty()) {
          return st::unexpected(ErrorCode::Parse, path + ".node：不能为空");
        }
        const std::string kind_name = json_get_string(track_node, "kind", "translate");
        const auto kind = st::raster::parse_track_kind(kind_name);
        if (!kind.has_value()) {
          return st::unexpected(ErrorCode::Parse,
                                std::format("{}.kind：只接受 {}，收到 \"{}\"", path,
                                            st::raster::track_kind_names(), kind_name));
        }
        track.kind = *kind;
        const Json* keys = json_find(track_node, "keys");
        if (keys == nullptr || !keys->is_array()) {
          return st::unexpected(ErrorCode::Parse, path + ".keys：需要数组");
        }
        std::size_t key_index = 0;
        for (const Json& key_node : *keys) {
          const std::string key_path = std::format("{}.keys[{}]", path, key_index);
          ++key_index;
          if (const auto* table = st::raster::find_field_table("animation.tracks[].keys[]");
              table != nullptr) {
            if (const auto status = st::raster::check_unknown_fields(key_node, *table, key_path);
                !status) {
              return st::forward_error(status.error());
            }
          }
          st::raster::Keyframe key{};
          // `t` 与 `time` 都收（前者是推荐写法，后者不让人因拼写被拒）
          const Json* time_node = json_find(key_node, "t");
          if (time_node == nullptr) time_node = json_find(key_node, "time");
          if (time_node == nullptr || !time_node->is_number()) {
            return st::unexpected(ErrorCode::Parse,
                                  key_path + ".t：缺少时间（秒；`t` 或 `time`）");
          }
          key.time = static_cast<float>(time_node->get<double>());
          if (!std::isfinite(key.time)) {
            return st::unexpected(ErrorCode::Parse, key_path + ".t：必须是有限数（NaN 会污染插值）");
          }
          const auto value = read_vec3(key_node, "value", math::Vec3{}, key_path);
          if (!value) return st::forward_error(value.error());
          // rotation 轨道的值也是**度**（与对象 `rotation` 字段同一套对调用方
          // 约定），内部存弧度——漏这一步的症状是“动画跑了但物体不转”
          //（实测 480° 被当 480 弧度，浮点上几乎看不出）。
          key.value = track.kind == st::raster::TrackKind::Rotation
                          ? math::Vec3{to_radians(value->x), to_radians(value->y),
                                       to_radians(value->z)}
                          : *value;
          const std::string easing = json_get_string(key_node, "easing", "linear");
          const auto parsed_easing = st::raster::parse_easing(easing);
          if (!parsed_easing.has_value()) {
            return st::unexpected(ErrorCode::Parse,
                                  std::format("{}.easing：只接受 {}，收到 \"{}\"", key_path,
                                              st::raster::easing_names(), easing));
          }
          key.easing = *parsed_easing;
          track.keys.push_back(key);
        }
        // 关键帧排序：手写 JSON 常按叙事顺序写，而采样要求时间有序。
        // 同一时刻的两个关键帧保持**后写的赢**（`stable_sort` 保住输入顺序，
        // 然后采样取"最后一个 ≤ t 的"）。
        std::stable_sort(track.keys.begin(), track.keys.end(),
                         [](const st::raster::Keyframe& lhs, const st::raster::Keyframe& rhs) {
                           return lhs.time < rhs.time;
                         });
        clip.tracks.push_back(std::move(track));
      }
    }
    spec.animation = std::move(clip);
  }

  // ── 素材基准目录（`asset_basis`）──
  //
  // ## 踩到过：字段被读取但从没被解析
  //
  // `resolve_shape_path` 一直在用 `spec.asset_basis`，但解析器里没有读这个字段，
  // 于是它恒为空——`obj:` / `gltf:` / `texture` 的相对路径全部退回"按工作目录找"。
  // 现有用例能过是因为**它们是从 C++ 直接给 `spec.asset_basis` 赋值**的，
  // 走 JSON 的路径从未被覆盖。写贴图用例时才发现（画面是顶点色，没有贴图）。
  //
  // 教训：字段"被用到"不等于"被解析到"，两头都要有用例。
  spec.asset_basis = json_get_string(json, "asset_basis", spec.asset_basis);
  spec.snap = static_cast<float>(json_get_double(json, "snap", spec.snap));
  if (spec.snap < 0.0f) {
    return st::unexpected(ErrorCode::Parse, "snap：步长必须 >= 0（0 = 关闭）");
  }

  const Json* objects = json_find(json, "objects");
  if (objects == nullptr) {
    return st::unexpected(ErrorCode::Parse, "objects：缺少（哪怕是空数组也要写）");
  }
  if (!objects->is_array()) {
    return st::unexpected(ErrorCode::Parse, "objects：需要数组");
  }
  const auto& arr = objects->get_ref<const Json::array_t&>();
  spec.objects.reserve(arr.size());
  for (std::size_t i = 0; i < arr.size(); ++i) {
    const auto obj = read_object(arr[i], i);
    if (!obj) return st::forward_error(obj.error());
    spec.objects.push_back(*obj);
  }
  // 扁平写法（`objects` 里平铺 + 各自 `parent`）→ 树。
  //
  // ## 为什么在解析阶段就建树（而不是渲染时才算）
  //
  // 建树要校验三件事：父存在、无环、无重名。这三件事都是**输入错误**，
  // 应当在"解析"这一步就报出来——否则渲染到一半才发现"某个节点找不到父"，
  // 调用方拿到的是一帧半成品。而且建树之后 `spec.objects` 就是根节点列表，
  // 渲染与导出都只需一次前序遍历。
  // ## 重名检查在解析阶段就做（不只在扁平写法里）
  //
  // 嵌套写法不走 `attach_by_parent`，但重名同样是**输入错误**：
  // 按名字寻址会静默指向其中一个，而调用方以为拿到的是另一个。
  // 所以这里对整棵树（含嵌套 children）统一查一遍。
  {
    std::set<std::string> seen;
    std::string duplicated;
    const auto scan = [&](auto&& self, const SceneObjectSpec& object) -> void {
      if (!object.name.empty() && !seen.insert(object.name).second) {
        if (!duplicated.empty()) duplicated += ", ";
        duplicated += object.name;
      }
      for (const SceneObjectSpec& child : object.children) self(self, child);
    };
    for (const SceneObjectSpec& object : spec.objects) scan(scan, object);
    if (!duplicated.empty()) {
      return st::unexpected(ErrorCode::Invalid,
                            std::format("场景里有重名节点：{}（节点名在同一场景里必须唯一；"
                                        "不同分支下请用不同名字）",
                                        duplicated));
    }
  }

  if (std::any_of(spec.objects.begin(), spec.objects.end(),
                  [](const SceneObjectSpec& object) { return !object.parent.empty(); })) {
    std::vector<st::raster::SceneNode> flat;
    std::vector<std::string> parents;
    flat.reserve(spec.objects.size());
    parents.reserve(spec.objects.size());
    for (const SceneObjectSpec& object : spec.objects) {
      flat.push_back(to_node(object));
      parents.push_back(object.parent);
    }
    // 记下"名字 → 原始 spec"，建树后把几何/材质/skin 回填到树上
    std::map<std::string, const SceneObjectSpec*> original;
    const auto index_original = [&](auto&& self, const SceneObjectSpec& object) -> void {
      original.emplace(object.name, &object);
      for (const SceneObjectSpec& child : object.children) self(self, child);
    };
    for (const SceneObjectSpec& object : spec.objects) index_original(index_original, object);
    const auto attached = st::raster::attach_by_parent(flat, parents);
    if (!attached) return st::forward_error(attached.error());
    spec.objects.clear();
    spec.objects.reserve(flat.size());
    for (const st::raster::SceneNode& root : flat) {
      // 递归地把空间字段换上树里的值、其余字段保留原始 spec 的
      const auto graft = [&](auto&& self, const st::raster::SceneNode& node) -> SceneObjectSpec {
        SceneObjectSpec out{};
        if (const auto found = original.find(node.name); found != original.end()) {
          out = *found->second;
          out.children.clear();
        }
        out.name = node.name;
        out.group = !node.has_geometry;
        out.translate = node.local.translation;
        out.rotation = node.local.rotation;
        out.scale = node.local.scale;
        out.pivot = node.pivot;
        out.visible = node.visible;
        out.children.reserve(node.children.size());
        for (const st::raster::SceneNode& child : node.children) {
          out.children.push_back(self(self, child));
        }
        return out;
      };
      spec.objects.push_back(graft(graft, root));
    }
  }

  // ── 网格吸附（在约束解算前：只吸手写摆放，解算出的装配值不受影响）──
  //
  // 吸附服务「手写 translate」（把 -1.05 对到 -1.0）；约束解算产出精确的
  // 装配值（台面顶 + 半高），再吸附只会破坏咬合。只吸 translate——
  // rotation/scale 吸附会把 45° 吸到网格上，不是用户意图。递归含子级。
  if (spec.snap > 0.0f) {
    const auto snap_value = [](float value, float step) {
      return std::round(value / step) * step;
    };
    const auto snap_tree = [&](auto&& self, SceneObjectSpec& object) -> void {
      object.translate.x = snap_value(object.translate.x, spec.snap);
      object.translate.y = snap_value(object.translate.y, spec.snap);
      object.translate.z = snap_value(object.translate.z, spec.snap);
      for (SceneObjectSpec& child : object.children) self(self, child);
    };
    for (SceneObjectSpec& object : spec.objects) snap_tree(snap_tree, object);
  }

  // ── 装配约束解算（解析后、建树前；写回 translate/rotation/size）──
  //
  // 顺序：先序遍历（父与兄先于子），每解算一个对象就把它的**世界包围盒**
  // 登记进缓存——后续约束引用它时拿到的已是解算后的位形。这保证一遍拓扑序
  // 完成，无迭代；前向引用（引用后面才出现的名字）在查缓存时自然报错。
  if (std::any_of(spec.objects.begin(), spec.objects.end(), [](const SceneObjectSpec& object) {
        const auto has = [](auto&& self, const SceneObjectSpec& o) -> bool {
          return !o.attach.empty() || !o.connect.empty() || !o.align.empty() ||
                 std::any_of(o.children.begin(), o.children.end(),
                             [&](const SceneObjectSpec& c) { return self(self, c); });
        };
        return has(has, object);
      })) {
    const auto status = solve_constraints(spec.objects, spec.asset_basis);
    if (!status) return st::forward_error(status.error());
  }
  return spec;
}

auto parse_scene_spec_text(std::string_view text) -> Result<SceneSpec> {
  const auto json = json_parse(text);
  if (!json) return st::forward_error(json.error());
  return parse_scene_spec(*json);
}

auto resolve_shape_path(std::string_view reference, std::string_view asset_basis)
    -> std::string {
  if (reference.empty()) return {};
  std::string ref{reference};
  if (st::fs::is_absolute(ref)) return ref;
  if (!asset_basis.empty()) {
    const std::string joined = st::fs::join(std::string(asset_basis), ref);
    // 只有**文件真的存在**才采用拼接结果：否则报错里会出现一个
    // 调用方从未写过的路径，排查时先怀疑拼接规则而不是“我路径写错了”。
    if (st::fs::exists(joined)) return joined;
  }
  return ref;
}

auto build_mesh(const SceneObjectSpec& object) -> Result<Mesh> {
  // ── 阵列对象：first child = 原型，实例化 count 次后 merge ──
  if (!object.pattern.empty()) {
    if (object.children.empty()) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("{}：pattern 需要至少 1 个子元素作为原型", object.name));
    }
    const SceneObjectSpec& prototype = object.children.front();
    const auto base_mesh = build_mesh(prototype);
    if (!base_mesh) return st::forward_error(base_mesh.error());
    const st::raster::SceneNode node = to_node(prototype);
    const st::math::Mat4 base_matrix = st::raster::local_matrix(node);
    const int count = object.pattern_count;
    std::vector<Mesh> instances;
    instances.reserve(static_cast<std::size_t>(count));
    constexpr float kTwoPi = 6.28318530717958647692f;
    for (int i = 0; i < count; ++i) {
      st::math::Mat4 placement{};
      if (object.pattern == "ring") {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(count);
        placement = st::math::Mat4::translation(
                        st::math::Vec3{std::sin(angle) * object.pattern_radius, 0.0f,
                                       std::cos(angle) * object.pattern_radius}) *
                    st::math::Mat4::rotation(st::math::Vec3{0.0f, 1.0f, 0.0f}, angle);
      } else {
        placement = st::math::Mat4::translation(object.pattern_step * static_cast<float>(i));
      }
      instances.push_back(st::raster::transform(*base_mesh, placement * base_matrix));
    }
    return st::raster::merge(instances);
  }
  // ── 布尔运算对象：children = 操作体（首个为基体），递归求值后链式运算 ──
  //
  // `op` 非空时 children 的层级语义被【重定义为操作体列表】（不是父子层级）
  //——同一个 children 字段、两种语义，以 op 为开关。这么设计而不是另开
  // `operands` 数组的理由：XML 的嵌套写法天然就是「标签套标签」,
  // <Subtract><Box/><Cylinder/></Subtract> 里 Cylinder 就是 Box 的子元素；
  // 序列化/Part 展开/约束解算全部零改动复用。
  if (!object.op.empty()) {
    st::raster::BooleanOp boolean_op{};
    if (object.op == "subtract") {
      boolean_op = st::raster::BooleanOp::Subtract;
    } else if (object.op == "union") {
      boolean_op = st::raster::BooleanOp::Union;
    } else if (object.op == "intersect") {
      boolean_op = st::raster::BooleanOp::Intersect;
    } else {
      return st::unexpected(ErrorCode::Parse,
                            std::format("{}.op：只接受 union/subtract/intersect，收到 \"{}\"",
                                        object.name, object.op));
    }
    if (object.children.size() < 2U) {
      return st::unexpected(
          ErrorCode::Parse,
          std::format("{}：{} 至少需要 2 个子元素（首个是基体，其余是工具）",
                      object.name, object.op));
    }
    std::vector<Mesh> operands;
    operands.reserve(object.children.size());
    for (const SceneObjectSpec& child : object.children) {
      const auto mesh = build_mesh(child);
      if (!mesh) return st::forward_error(mesh.error());
      // 子元素的 TRS 在网格层烘培（操作体各自的 translate/rotation 生效）。
      const st::raster::SceneNode node = to_node(child);
      operands.push_back(st::raster::transform(*mesh, st::raster::local_matrix(node)));
    }
    const auto result = st::raster::boolean_chain(operands, boolean_op);
    if (!result) return st::forward_error(result.error());
    Mesh mesh = *result;
    // 尺寸缩放（布尔结果的 resize 语义与普通图元一致：按包围盒归一）。
    if (object.size_count > 0U) {
      const st::raster::Bounds box = st::raster::bounds(mesh);
      const math::Vec3 current = box.size();
      math::Vec3 target = current;
      if (object.size_count >= 1U) target.x = object.size.x;
      if (object.size_count >= 2U) target.y = object.size.y;
      if (object.size_count >= 3U) target.z = object.size.z;
      const math::Vec3 scale{current.x > 1e-6f ? target.x / current.x : 1.0f,
                             current.y > 1e-6f ? target.y / current.y : 1.0f,
                             current.z > 1e-6f ? target.z / current.z : 1.0f};
      mesh = st::raster::transform(
          mesh, st::math::Mat4::scaling(scale));
    }
    // 顶点色统一为对象色：布尔结果的顶点来自各操作体（各自的颜色），
    // 而场景层只认 `object.color`——不覆盖会出现“同一块板上两种颜色”。
    //
    // ⚠ 必须走 sRGB→线性转换：顶点色是**线性**的，而 `object.color` 是
    // sRGB 字节。直接 `/255` 会把颜色洗亮（#7a6a58 的 0.478 直通，
    // 正确线性值 0.195）——实测症状：布尔件在画面里是一块发白的浅灰，
    // 与周围件的包浆色判若两个场景。
    const math::Vec3 linear = srgb_to_linear(object.color);
    for (std::size_t i = 0; i + 8 < mesh.vertices.size(); i += 9U) {
      mesh.vertices[i + 6U] = linear.x;
      mesh.vertices[i + 7U] = linear.y;
      mesh.vertices[i + 8U] = linear.z;
    }
    return mesh;
  }
  if (object.shape.rfind("obj:", 0) == 0) {
    // 不带 `asset_basis` 的重载：`build_mesh` 只拿到对象、拿不到场景。
    // 带基准目录的那条在 `draw_one` 里单独处理（场景级信息在那才可见）。
    return Mesh::load_obj_file(object.shape.substr(4), object.color);
  }

  // 形状 → 图元库。`sphere` / `box` 是历史别名（既有脚本在用），
  // 映射到 `uv_sphere` / `cube` 后参数语义与原实现保持一致：
  //   - `sphere` 的 `radius` 就是半径（原 `colored_sphere` 同义）；
  //   - `box` 的三轴用 `width`/`height`/`depth`（原 `colored_box` 同义）。
  const std::string_view shape = object.shape;
  primitives::PrimitiveParams params{};
  params.segments = object.segments;
  params.rings = object.rings;
  params.subdivisions = object.subdivisions;
  params.color = object.color;
  params.face_camera = object.face_camera;
  params.columns = object.columns;
  params.rows = object.rows;
  params.teeth = object.teeth;
  params.tooth_height = object.tooth_height;
  params.tooth_width = object.tooth_width;
  params.knot_p = object.knot_p;
  params.knot_q = object.knot_q;
  params.turns = object.turns;
  params.taper = object.taper;
  params.thickness = object.thickness;
  params.section = object.section;
  // 草图轮廓（extrude/revolve）："x,y x,y …"（空格分点、逗号分坐标）。
  // 解析失败报错指路（点名与期望格式）；顺逆时针都收，生成器归一。
  if (!object.profile.empty()) {
    // ## 分隔符规约
    //
    // 点间分隔：空白或 `;`（推荐 `;`——坐标内可以写空格，可读性更好）；
    // 坐标内：逗号。
    // 实测坑：`"0, -0.14"` 里逗号后的空格既是坐标内空格又是点分隔，
    // 无法区分——所以空白分点时**坐标内不能有空格**，要可读就用 `;`。
    std::size_t cursor = 0;
    while (cursor < object.profile.size()) {
      while (cursor < object.profile.size() &&
             (object.profile[cursor] == ' ' || object.profile[cursor] == '\t' ||
              object.profile[cursor] == '\n' || object.profile[cursor] == '\r' ||
              object.profile[cursor] == ';')) {
        ++cursor;
      }
      if (cursor >= object.profile.size()) break;
      const auto space = object.profile.find_first_of(" \t\n\r;", cursor);
      const std::string point_text =
          object.profile.substr(cursor, space == std::string::npos
                                                ? std::string::npos
                                                : space - cursor);
      const auto comma = point_text.find(',');
      if (comma == std::string::npos) {
        return st::unexpected(
            ErrorCode::Parse,
            std::format("{}.profile：每个点是 \"x,y\"（逗号分隔），收到 \"{}\"；"
                        "整体格式：\"0,0 1,0 1,1\"（空格分点）"
                        "——注意坐标内不能有空格（\"0, 1\" 会被当成两个点）",
                        object.name, point_text));
      }
      try {
        const float x = std::stof(point_text.substr(0, comma));
        const float y = std::stof(point_text.substr(comma + 1));
        params.profile.emplace_back(x, y);
      } catch (const std::exception&) {
        return st::unexpected(
            ErrorCode::Parse,
            std::format("{}.profile：坐标不是数字（\"{}\"）", object.name, point_text));
      }
      if (space == std::string::npos) break;
      cursor = space + 1;
    }
    const std::size_t needed = object.shape == "revolve" ? 2U : 3U;
    if (params.profile.size() < needed) {
      return st::unexpected(
          ErrorCode::Parse,
          std::format("{}.profile：{} 至少需要 {} 个点，收到 {} 个",
                      object.name, object.shape, needed, params.profile.size()));
    }
  }

  // ## 尺寸取值：**只有 `size` 一条路**
  //
  // 旧字段（radius/width/height/depth）已彻底删除。它们在各形状下语义不同
  // （实测：`box` 的 height 完全无效、`plane` 的 radius 是半长而 width 是全长），
  // 是"写对了名字但含义不对"的根源——那类错误任何检查都抓不住。
  //
  // `size_of(i, fallback)`：第 i 项，没写就返回该形状的合理默认。
  const bool has_size = object.size_count > 0U;
  const auto size_of = [&](std::size_t index, float fallback) -> float {
    if (!has_size) return fallback;
    if (index == 0U) return object.size.x;
    if (index == 1U) return (object.size_count >= 2U) ? object.size.y : fallback;
    if (index == 2U) return (object.size_count >= 3U) ? object.size.z : fallback;
    return fallback;
  };

  // ## 形状 → 图元库（尺寸只有 `size` 一条路）
  //
  // `PrimitiveParams` 里仍是 `radius`/`depth`/`height` —— 那是**内部实现层**，
  // 它服务的是图元生成器（圆的半径、柱的高），语义在那一层是清晰的。
  // 脚本层只暴露 `size`（全长），在这里换一次。
  if (shape == "cube" || shape == "box") {
    // [X, Y, Z] 三边全长。图元入参：radius = 半个 X，depth = Y，height = Z（0 = 同 X）
    params.radius = size_of(0U, 1.0f) * 0.5f;
    params.depth = size_of(1U, 1.0f);
    params.height = size_of(2U, 0.0f);
    return primitives::make_cube(params);
  }
  if (shape == "plane") {
    // [X, Z]（平面在 XZ 面）。入参：radius = 半个 X，depth = Z
    params.radius = size_of(0U, 1.6f) * 0.5f;
    params.depth = size_of(1U, 1.6f);
    return primitives::make_plane(params);
  }
  if (shape == "torus" || shape == "torus_knot") {
    // [外径, 管径]。入参：major_radius 是**中心线**半径 = (外径 - 管径) / 2
    const float outer = size_of(0U, 1.6f);
    const float tube = size_of(1U, outer * 0.35f);
    params.major_radius = std::max((outer - tube) * 0.5f, tube * 0.05f);
    params.minor_radius = tube * 0.5f;
    params.minor_segments = object.minor_segments;
    params.major_segments = object.segments;
    params.twist = object.twist;
    if (shape == "torus") return primitives::make_torus(params);
    return primitives::make_torus_knot(params);
  }

  // 其余形状统一是"圆形截面 + Y 向长度"：
  //   球类 → 只用第 0 项（直径）
  //   柱/锥/胶囊/棱柱/齿轮/管/螺旋/箭头 → [直径, 高]
  params.radius = size_of(0U, 1.0f) * 0.5f;
  params.depth = size_of(1U, 1.0f);
  params.top_radius = object.top_radius;
  // ## 螺旋/环结的"螺旋半径"与"管半径"
  //
  // 这两个不是 `radius`（= size[0]/2，是**外廓**）。旧代码拿
  // `object.major_radius * 0.35f` 当管半径——而 `major_radius` 默认 0，
  // 于是管半径变成 0（被 sanitized 抬到 1e-4）→ 螺旋细成一条看不见的线。
  //
  // 正确映射（与 torus 同一口径）：
  //   螺旋中心线半径 = 外廓半径 - 管半径
  //   管半径 = 外廓半径 × 0.18（视觉上"细管绕大圈"）
  // 显式写 major_radius/minor_radius 时以调用方为准。
  const float outer_radius = params.radius;
  const float tube = object.minor_radius > 0.0f
                         ? object.minor_radius
                         : std::max(outer_radius * 0.18f, 0.005f);
  params.minor_radius = tube;
  params.major_radius = object.major_radius > 0.0f
                            ? object.major_radius
                            : std::max(outer_radius - tube, tube);
  params.minor_segments = object.minor_segments;
  params.major_segments = object.segments;
  params.twist = object.twist;
  params.sides = object.sides;
  // 草图形状：profile 必填（上面的解析已验证点数）。
  //
  // 尺寸语义：`size[1]` = 拉伸深度（已在上面经 `params.depth` 用）。
  // x/z 范围由 profile 决定——**不参与包围盒归一**（否则会把轮廓比例
  // 拉伸成 0.3 立方，L 形支架变薄片）。其余图元继续走归一。
  if (shape == "extrude" || shape == "revolve") {
    if (object.profile.empty()) {
      return st::unexpected(
          ErrorCode::Parse,
          std::format("{}：{} 需要 profile 属性（\"x,y x,y …\" 点列）", object.name, shape));
    }
    return primitives::make_from_name(shape, params);
  }
  return primitives::make_from_name(shape, params);
}

auto render_scene(const SceneSpec& spec, Surface& canvas, math::Color background)
    -> Result<void> {
  auto scene = Scene3D::create(canvas.physical_width(), canvas.physical_height());
  if (!scene.has_value()) {
    return st::unexpected(ErrorCode::Unsupported, "无法创建三维场景（后端不可用）");
  }
  return render_scene_into(spec, **scene, canvas, background);
}

/// 实现主体：`time` 为 `std::nullopt` 表示"不采样动画"（静态场景）。
///
/// 为什么用一个可选时间而不是两个函数体：动画与静态只有**一行**差别
/// （`roots` 是否被采样结果替换），拆成两份必然漂移。
[[nodiscard]] auto render_impl(const SceneSpec& spec, const std::optional<float>& time,
                               Scene3D& scene, Surface& canvas, math::Color background)
    -> Result<void>;

auto render_scene_into(const SceneSpec& spec, Scene3D& scene, Surface& canvas,
                       math::Color background) -> Result<void> {
  return render_impl(spec, std::nullopt, scene, canvas, background);
}

auto render_scene_at_into(const SceneSpec& spec, float time, Scene3D& scene, Surface& canvas,
                          math::Color background) -> Result<void> {
  return render_impl(spec, time, scene, canvas, background);
}

auto render_scene_at(const SceneSpec& spec, float time, Surface& canvas, math::Color background)
    -> Result<void> {
  auto scene = Scene3D::create(canvas.physical_width(), canvas.physical_height());
  if (!scene.has_value()) {
    return st::unexpected(ErrorCode::Unsupported, "无法创建三维场景（后端不可用）");
  }
  return render_impl(spec, time, **scene, canvas, background);
}

auto render_impl(const SceneSpec& spec, const std::optional<float>& time, Scene3D& scene,
                 Surface& canvas, math::Color background) -> Result<void> {
  canvas.clear(background);

  // ## 顺序在这里被钉死
  //
  // 这正是本模块存在的理由：调用方无法把 `set_material` 排到 `begin_frame` 前。
  scene.begin_frame(background);
  scene.set_camera(spec.camera);
  scene.set_lighting(spec.lighting);
  scene.set_cull_mode(spec.cull_mode);

  // ## 顺序：地面 → 阴影开关 → 其余物体
  //
  // 平面投影阴影的契约是「`set_planar_shadow` 之前画的不算实体」。
  // 若先设阴影再画地面，地面会被当成实体、打上遮罩，阴影一个像素都画不出来
  //（实测：开关阴影画面均值 115.40 → 115.40，完全不变）。
  //
  // 分两遍的代价是遍历两次 `spec.objects`，收益是调用方不必知道这个顺序。

  // ── 蒙皮矩阵（前向声明，实际值在 `resolve` 之后算）──
  //
  // ## 为什么声明与填充分成两处
  //
  // `draw_world` 是 lambda，按引用捕获；而蒙皮矩阵必须等"动画采样 + resolve"
  // 之后才能算（它依赖当前姿态的世界矩阵）。所以先声明可变更的容器、
  // 让 lambda 捕获它，再在 resolve 之后填值——捕获是引用，填值对 lambda 可见。
  std::optional<st::raster::SkinMatrices> skin_matrices;
  /// 已 resolve 的树（蒙皮自动权重要用它的世界矩阵）；在 `resolve` 之后填。
  std::optional<st::raster::ResolvedTree> resolved_tree;

  // 用**已解算的世界矩阵**画一个对象（几何/材质来自它的 spec）。
  //
  // 与旧 `draw_one` 的差别只有一处：旧版在这里现算 `T·R·S`，新版直接收矩阵。
  // 为什么必须改：组合模型的世界矩阵是"父链 × 局部（含枢轴）"，
  // 在绘制函数里再算一遍等于把同一套语义实现两遍——而两处一旦漂移，
  // 表现为"深层节点的位置和预期差一点点"，极难定位。
  const auto draw_world = [&](const SceneObjectSpec& obj,
                              const math::Mat4& world) -> Result<void> {
    SceneObjectSpec resolved_object = obj;
    if (obj.shape.rfind("obj:", 0) == 0) {
      resolved_object.shape = "obj:" + resolve_shape_path(obj.shape.substr(4), spec.asset_basis);
    }
    if (!obj.gltf.empty()) {
      const std::string reference = obj.gltf.rfind("gltf:", 0) == 0 ? obj.gltf.substr(5) : obj.gltf;
      resolved_object.shape = "gltf:" + resolve_shape_path(reference, spec.asset_basis);
    }

    if (resolved_object.shape.rfind("gltf:", 0) == 0) {
      const std::string path = resolved_object.shape.substr(5);
      auto gltf_scene = gltf_read_file(path);
      if (!gltf_scene) return st::forward_error(gltf_scene.error());
      // 是否有 glTF 贴图被绑定（画完要清，见下）
      bool gltf_texture_bound = false;
      for (const GltfNode& node : gltf_scene->nodes) {
        for (const std::size_t mesh_index : node.meshes) {
          if (mesh_index >= gltf_scene->meshes.size()) continue;
          Material material{};
          if (obj.gltf_use_material && node.material >= 0 &&
              static_cast<std::size_t>(node.material) < gltf_scene->materials.size()) {
            const auto& slot = gltf_scene->materials[static_cast<std::size_t>(node.material)];
            // glTF 的 baseColorFactor 是**线性**，`Material::base_color` 也是线性
            material.base_color = math::Vec3{static_cast<float>(slot.base_color.r) / 255.0f,
                                             static_cast<float>(slot.base_color.g) / 255.0f,
                                             static_cast<float>(slot.base_color.b) / 255.0f};
            material.base_color_set = true;
            material.metallic = slot.metallic;
            material.roughness = slot.roughness;
            material.opacity = slot.opacity;
            material.specular = slot.metallic > 0.0f ? 1.0f : 0.0f;
            material.emission = slot.emissive;
            material.emissive_color = slot.emissive_color;
            material.emissive_color_set = slot.emissive > 0.0f;

            // ── 贴图：从 glTF 素材自动挂上（脚本零改动）──
            //
            // ## 这是"吸收生态素材"的最后一环
            //
            // 前面几步把图片解码进了 `GltfScene::textures`，这里把它交给渲染器。
            // 关键点：`set_texture` 是**场景状态**，必须在 `draw_mesh` 之前设；
            // 且每个 mesh 画完要清掉，否则上一张图会泄漏到下一个材质（症状是
            // "这个模型的下半身贴着上一个模型的图"）。
            const int texture_index = slot.base_color_texture;
            if (texture_index >= 0 &&
                static_cast<std::size_t>(texture_index) < gltf_scene->textures.size()) {
              const auto& texture = gltf_scene->textures[static_cast<std::size_t>(texture_index)];
              if (texture.is_valid()) {
                scene.set_texture(texture.rgba, texture.width, texture.height);
                // `KHR_texture_transform`（等价于场景脚本的 texture_scale/offset）
                scene.set_texture_transform(slot.uv_scale_u, slot.uv_scale_v, slot.uv_offset_u,
                                           slot.uv_offset_v, false);
                gltf_texture_bound = true;
              }
            }
          } else {
            // ## `gltf_use_material=false` 时**必须**把剧本的 color 铺成基色
            //
            // 原先这里只设了 metallic/roughness/specular/opacity，漏了 base_color。
            // 而 glTF 网格的顶点色恒为白（导出器不写颜色），于是：
            // 剧本里写 `"color": "#c08a5a"` 完全无效，模型永远是**一块白模**。
            //
            // 这正是“白模接入”要解决的最后一环——几何来自 glTF，
            // 材质/颜色由剧本控制，两者才能各自发挥长处。
            material.base_color = math::Vec3{static_cast<float>(obj.color.r) / 255.0f,
                                             static_cast<float>(obj.color.g) / 255.0f,
                                             static_cast<float>(obj.color.b) / 255.0f};
            material.base_color_set = true;
            material.metallic = obj.metallic;
            material.roughness = obj.roughness;
            material.specular = obj.specular;
            material.opacity = obj.opacity;
          }
          scene.set_material(material);
          scene.draw_mesh(gltf_scene->meshes[mesh_index], world * node.transform);
          // 画完清掉贴图：避免泄漏到下一个材质/下一个对象
          if (gltf_texture_bound) {
            scene.set_texture({}, 0, 0);
            scene.set_texture_transform(1.0f, 1.0f, 0.0f, 0.0f, false);
            gltf_texture_bound = false;
          }
        }
      }
      return {};
    }

    // ── 贴图与 UV 变换（**必须在 draw_mesh 之前设**：它们是场景状态）──
    //
    // ## 每帧每对象都重新读文件是浪费，但这里故意不缓存
    //
    // 缓存要处理"文件改了""同一路径不同对象"两件事，而 `render_scene` 的调用
    // 形态是"一次出图"（不是每帧几百次）。真正需要缓存的是交互式预览，
    // 那种场合应当由调用方持有 `Scene3D` 并自己管资源生命周期。
    // 先做对、再做快：读一次 PNG 在 260×200 的出图里完全不是瓶颈。
    bool texture_ready = false;
    if (!obj.texture.empty()) {
      const std::string path = resolve_shape_path(obj.texture, spec.asset_basis);
      if (const auto image = st::codec::png_read_file(path); image.has_value()) {
        scene.set_texture(image->rgba, static_cast<int>(image->width),
                          static_cast<int>(image->height));
        texture_ready = true;
      } else {
        // 贴图加载失败：**不中断渲染**（一张图丢了不该让整帧出不来），
        // 但要留下可查的痕迹——否则"怎么没贴上"要看代码才知道。
        ST_LOG_WARN("场景对象 \"{}\" 的贴图读取失败：{}（改用顶点色）", obj.name, path);
      }
    }
    if (!texture_ready) scene.set_texture({}, 0, 0);   // 显式清除：避免上一对象的贴图泄漏
    scene.set_texture_transform(obj.texture_scale_u, obj.texture_scale_v, obj.texture_offset_u,
                               obj.texture_offset_v, obj.texture_clamp);

    auto mesh = build_mesh(resolved_object);
    if (!mesh) return st::forward_error(mesh.error());

    // ── 蒙皮：按当前姿态形变（重网格缓存的是**绑定姿态**，形变结果每帧不同）──
    if (obj.skin && skin_matrices.has_value()) {
      if (!mesh->has_skins()) {
        // 没有权重：用朴素自动权重（离哪个关节近给谁）。
        // 真实角色应当在 Blender 里刷权重再用 glTF 导入；这里保"能跑起来"。
        *mesh = st::raster::auto_weights(*mesh, *spec.skeleton, *resolved_tree);
      }
      *mesh = st::raster::skin_mesh(*mesh, *skin_matrices);
    }

    Material material{};
    material.metallic = obj.metallic;
    material.roughness = obj.roughness;
    material.specular = obj.specular;
    material.emission = obj.emission;
    material.opacity = obj.opacity;
    // 自发光色与“纯发光体”开关。
    material.emissive_color = obj.emissive_color;
    material.emissive_color_set = obj.emissive_color_set;
    material.emission_unlit = obj.emission_unlit;
    scene.set_material(material);
    scene.draw_mesh(*mesh, world);
    return {};
  };

  // ── 组合模型：先解整棵树的世界矩阵，再按"父先子后"的顺序画 ──
  //
  // ## 为什么只解一次（而不是递归时逐层乘）
  //
  // 递归里逐层 `parent * local` 每层都要重算父矩阵，深度一深就成 O(n·depth)；
  // 而且**枢轴**与可见性继承都需要"完整父链"，递归版本得把上下文层层传下去。
  // 一次性展平（`resolve`）把这两件事都变成一次遍历，且求得的世界矩阵可以直接
  // 交给导出器/包围盒计算，渲染之外的用途也复用同一份。
  // ## 先给未命名节点补名（统一规则）
  //
  // 踩过：`spec` 侧用 `<unnamed>` 当键，而 `scene_graph::resolve` 对空名生成 `#序号`，
  // 两边规则不一致 → 渲染时"按路径找不到 spec" → **静默跳过**，
  // 画面全黑而解析完全成功（`st test scene3d_script_accepts_obj_shape` 的 0 像素）。
  //
  // 修法：建树之前一次性把空名字补成 `#先序号`，此后**整条链路只有一种命名规则**。
  // 用副本而不是改调用方的 spec：`render_scene` 的入参是 const，改它属于越权。
  std::vector<SceneObjectSpec> named = spec.objects;
  {
    std::size_t counter = 0;
    const auto assign = [&](auto&& self, SceneObjectSpec& object) -> void {
      if (object.name.empty()) object.name = std::format("#{}", counter++);
      for (SceneObjectSpec& child : object.children) self(self, child);
    };
    for (SceneObjectSpec& object : named) assign(assign, object);
  }

  std::vector<st::raster::SceneNode> roots;
  roots.reserve(named.size());
  for (const SceneObjectSpec& object : named) roots.push_back(to_node(object));
  // ── 动画采样（只有给了时间且场景带动画时才生效）──
  //
  // 顺序很关键：**先采样、再 resolve**。因为采样改的是 `local`（局部 TRS），
  // 世界矩阵必须从"动画后的 local"重新求；反过来做（先 resolve 再改 local）
  // 得到的是上一帧的世界矩阵，症状是"动画慢一帧"。
  if (time.has_value() && spec.animation.has_value()) {
    const st::raster::Clip& clip = *spec.animation;
    const auto status = st::raster::validate_clip(clip, roots);
    if (!status) return st::forward_error(status.error());
    const auto sampled = st::raster::sample_clip(clip, *time);
    if (std::getenv("ST_ANIM_DEBUG") != nullptr) {
      std::fprintf(stderr, "[anim] t=%.2f 采样 %zu 个节点：", double(*time), sampled.size());
      for (const auto& entry : sampled) {
        std::fprintf(stderr, "%s(rot %.1f°)", entry.node.c_str(),
                     double(entry.local.rotation.y * 57.29578f));
      }
      std::fprintf(stderr, "\n");
    }
    roots = st::raster::apply_sample(roots, sampled);
  }
  auto resolved = st::raster::resolve(roots);
  if (!resolved) return st::forward_error(resolved.error());
  resolved_tree = *resolved;
  if (spec.skeleton.has_value()) {
    // 蒙皮矩阵 = 世界矩阵 · 逆绑定矩阵；世界矩阵是"动画采样 + resolve"之后的那一份
    auto computed = st::raster::compute_skin_matrices(*spec.skeleton, *resolved);
    if (!computed) return st::forward_error(computed.error());
    skin_matrices = std::move(*computed);
  }

  // 名字 → 原始 spec（几何/材质字段在渲染时要取回来）。
  // 用**名字**而不是路径：上面已保证整棵树内名字唯一，而名字与
  // `ResolvedNode::name` 是同一个字段，不会像路径那样出现两套拼装规则。
  std::map<std::string, const SceneObjectSpec*> spec_of;
  const auto index_specs = [&](auto&& self, const SceneObjectSpec& object) -> void {
    spec_of.emplace(object.name, &object);
    for (const SceneObjectSpec& child : object.children) self(self, child);
  };
  for (const SceneObjectSpec& object : named) index_specs(index_specs, object);

  // 几何包围盒缓存（枢轴预设要按自身几何算；同名同形状只算一次）
  std::map<std::string, st::raster::Bounds> local_bounds;
  const auto bounds_of = [&](auto&& self, const SceneObjectSpec& object,
                             std::string_view path) -> st::raster::Bounds {
    if (const auto found = local_bounds.find(std::string(path)); found != local_bounds.end()) {
      return found->second;
    }
    st::raster::Bounds box = st::raster::Bounds::empty();
    if (!object.group || !object.op.empty() || !object.pattern.empty()) {
      SceneObjectSpec resolved_object = object;
      if (object.shape.rfind("obj:", 0) == 0) {
        resolved_object.shape = "obj:" + resolve_shape_path(object.shape.substr(4), spec.asset_basis);
      }
      if (!object.gltf.empty()) {
        const std::string reference =
            object.gltf.rfind("gltf:", 0) == 0 ? object.gltf.substr(5) : object.gltf;
        resolved_object.shape = "gltf:" + resolve_shape_path(reference, spec.asset_basis);
      }
      if (resolved_object.shape.rfind("gltf:", 0) != 0) {
        if (const auto mesh = build_mesh(resolved_object); mesh.has_value()) {
          box = st::raster::bounds(*mesh);
        }
      }
    }
    // 分组的包围盒 = 子节点（在自身局部坐标里）的并
    for (const SceneObjectSpec& child : object.children) {
      const std::string child_path = std::format("{}/{}", path,
                                                 child.name.empty() ? "<unnamed>" : child.name);
      const st::raster::Bounds child_box = self(self, child, child_path);
      if (child_box.is_empty()) continue;
      const auto child_node = to_node(child);
      const st::raster::Bounds transformed = [&] {
        const auto matrix = st::raster::local_matrix(child_node);
        st::raster::Bounds out = st::raster::Bounds::empty();
        for (const auto& corner : {std::pair{child_box.min, 0}, std::pair{child_box.max, 1}}) {
          const math::Vec3 point = corner.first;
          const math::Vec3 moved{
              matrix.value(0, 0) * point.x + matrix.value(0, 1) * point.y +
                  matrix.value(0, 2) * point.z + matrix.value(0, 3),
              matrix.value(1, 0) * point.x + matrix.value(1, 1) * point.y +
                  matrix.value(1, 2) * point.z + matrix.value(1, 3),
              matrix.value(2, 0) * point.x + matrix.value(2, 1) * point.y +
                  matrix.value(2, 2) * point.z + matrix.value(2, 3)};
          out = out.union_with(moved);
        }
        // 八个角都要并（只并两个角在旋转后会低估）
        for (int mask = 0; mask < 8; ++mask) {
          const math::Vec3 corner_point{(mask & 1) != 0 ? child_box.max.x : child_box.min.x,
                                        (mask & 2) != 0 ? child_box.max.y : child_box.min.y,
                                        (mask & 4) != 0 ? child_box.max.z : child_box.min.z};
          out = out.union_with(math::Vec3{
              matrix.value(0, 0) * corner_point.x + matrix.value(0, 1) * corner_point.y +
                  matrix.value(0, 2) * corner_point.z + matrix.value(0, 3),
              matrix.value(1, 0) * corner_point.x + matrix.value(1, 1) * corner_point.y +
                  matrix.value(1, 2) * corner_point.z + matrix.value(1, 3),
              matrix.value(2, 0) * corner_point.x + matrix.value(2, 1) * corner_point.y +
                  matrix.value(2, 2) * corner_point.z + matrix.value(2, 3)});
        }
        return out;
      }();
      box = box.union_with(transformed);
    }
    local_bounds.emplace(std::string(path), box);
    return box;
  };

  // 一个节点的**最终世界矩阵**（含枢轴）：枢轴预设按几何包围盒算
  const auto world_matrix_of = [&](const st::raster::ResolvedNode& node) {
    const auto found = spec_of.find(node.name);
    if (found == spec_of.end() || found->second->pivot_at.empty()) return node.world;
    const auto preset = st::raster::parse_pivot_preset(found->second->pivot_at);
    if (!preset.has_value()) return node.world;
    // 枢轴 = 预设位置（在节点局部坐标系里表达）。
    const st::raster::Bounds box = bounds_of(bounds_of, *found->second, node.name);
    if (box.is_empty()) return node.world;
    const math::Vec3 pivot_local = st::raster::pivot_from_bounds(box, *preset);
    st::raster::SceneNode patched = to_node(*found->second);
    // ## 必须用**已采样**的 local，而不是 spec 里的原始值
    //
    // 踩过：这里原先从 `*found->second` 重新 `to_node()`，于是动画刚采好的
    // `local`（旋转角）被场景里写死的初始值覆盖——症状是"角色在动，但凡是
    // 设了 pivot_at 的节点都纹丝不动"，而采样日志里角度明明在变。
    // 世界矩阵只能由 `node`（resolve 的输出，已含动画）推出来。
    patched.local = node.local;
    patched.pivot = pivot_local;
    return node.parent >= 0
               ? resolved->nodes[static_cast<std::size_t>(node.parent)].world *
                     st::raster::local_matrix(patched)
               : st::raster::local_matrix(patched);
  };


  // 画一棵子树（父先子后；不可见整棵跳过）
  const auto paint = [&](const st::raster::ResolvedNode& node) -> Result<void> {
    if (!node.visible) return {};
    const auto found = spec_of.find(node.name);
    if (found == spec_of.end()) return {};
    const SceneObjectSpec& obj = *found->second;
    const math::Mat4 world = world_matrix_of(node);
    // 布尔/阵列对象 shape=group 但**有实体几何**（由 children 运算产出）
    //——与 to_node 的 has_geometry 同一口径，两处必须一致（否则
    // “能建出来但画不出来”）。
    if (!obj.group || !obj.op.empty() || !obj.pattern.empty()) {
      const auto status = draw_world(obj, world);
      if (!status) return st::forward_error(status.error());
    }
    return {};
  };

  // 先画地面（阴影契约：`set_planar_shadow` 之前画的算"地面/非实体"）
  //
  // ⚠ 顺序被**世界矩阵**影响：`resolve` 的输出是"父先子后"的先序，所以这里
  // 必须保证地面是**根节点**（或至少它的祖先不依赖"阴影之前/之后"的划分）。
  // 旧脚本里地面就是一个顶层对象，天然满足。
  for (const st::raster::ResolvedNode& node : resolved->nodes) {
    const auto found = spec_of.find(node.name);
    if (found == spec_of.end() || !found->second->ground) continue;
    const auto status = paint(node);
    if (!status) return st::forward_error(status.error());
  }
  scene.set_planar_shadow(spec.shadow);
  for (const st::raster::ResolvedNode& node : resolved->nodes) {
    const auto found = spec_of.find(node.name);
    if (found != spec_of.end() && found->second->ground) continue;
    const auto status = paint(node);
    if (!status) return st::forward_error(status.error());
  }

  scene.end_frame(canvas,
                      math::Rect{0.0f, 0.0f, static_cast<float>(canvas.physical_width()),
                                 static_cast<float>(canvas.physical_height())});
  scene.flush();
  return {};
}

}  // namespace st::raster
