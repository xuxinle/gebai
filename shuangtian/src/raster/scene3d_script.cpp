#include "st/raster/scene3d_script.hpp"

#include "st/raster/gltf.hpp"
#include "st/raster/scene_graph.hpp"

#include <set>
#include "st/raster/primitives.hpp"

#include <cmath>
#include <string>

#include "st/core/fs.hpp"
#include "st/raster/mesh.hpp"

namespace st::raster {
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
  const auto key_dir = read_vec3(*node, "key_direction", lit.key_direction, "lighting");
  if (!key_dir) return st::forward_error(key_dir.error());
  lit.key_direction = *key_dir;
  lit.ambient = static_cast<float>(json_get_double(*node, "ambient", lit.ambient));
  lit.diffuse = static_cast<float>(json_get_double(*node, "diffuse", lit.diffuse));
  lit.rim = static_cast<float>(json_get_double(*node, "rim", lit.rim));
  lit.exposure = static_cast<float>(json_get_double(*node, "exposure", lit.exposure));

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

/// `SceneObjectSpec` → `SceneNode`（几何/材质留在 spec，空间信息进节点）。
///
/// 为什么要有这层桥：场景脚本的 `SceneObjectSpec` 同时承载"是什么"（形状/材质）
/// 与"在哪"（变换），而 `scene_graph` 只管"在哪"。两者职责不同但字段重叠，
/// 所以解析与渲染之间需要一个**显式**的转换点——而不是让两套结构互相 include。
[[nodiscard]] auto to_node(const SceneObjectSpec& object) -> st::raster::SceneNode {
  st::raster::SceneNode node{};
  node.name = object.name;
  node.has_geometry = !object.group;
  node.local.translation = object.translate;
  node.local.rotation = object.rotate;
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
  object.rotate = node.local.rotation;
  object.scale = node.local.scale;
  object.pivot = node.pivot;
  object.visible = node.visible;
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
  SceneObjectSpec obj{};
  obj.shape = json_get_string(node, "shape", "sphere");

  const auto color = read_color(node, "color", obj.color, path);
  if (!color) return st::forward_error(color.error());
  obj.color = *color;

  const auto translate = read_vec3(node, "translate", obj.translate, path);
  if (!translate) return st::forward_error(translate.error());
  obj.translate = *translate;

  const auto rotate = read_vec3(node, "rotate", obj.rotate, path);
  if (!rotate) return st::forward_error(rotate.error());
  obj.rotate = *rotate;

  const auto scale = read_vec3(node, "scale", obj.scale, path);
  if (!scale) return st::forward_error(scale.error());
  obj.scale = *scale;

  // 记下"调用方显式写了哪些尺寸字段"（见 `SceneObjectSpec::specified` 的说明）。
  // 这一位是"静默用错默认值"的解药：缺字段就落到该形状的历史默认，
  // 写了就按写的来——两种脚本都能得到符合预期的形状。
  const auto note_field = [&](const char* key, std::uint8_t flag) {
    if (json_find(node, key) != nullptr) obj.specified |= flag;
  };
  note_field("radius", SceneObjectSpec::kSpecifiedRadius);
  note_field("depth", SceneObjectSpec::kSpecifiedDepth);
  note_field("height", SceneObjectSpec::kSpecifiedHeight);
  note_field("width", SceneObjectSpec::kSpecifiedWidth);
  obj.radius = static_cast<float>(json_get_double(node, "radius", obj.radius));
  obj.width = static_cast<float>(json_get_double(node, "width", obj.width));
  obj.height = static_cast<float>(json_get_double(node, "height", obj.height));
  obj.depth = static_cast<float>(json_get_double(node, "depth", obj.depth));
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
  // 旧语义里 `plane` 朝 +Z（立面），所以所有旧脚本都写 `"rotate": [-π/2,0,0]`
  // 把它扳成水平。现在 `plane` 图元**本就朝 +Y**，那 -90° 会把它立成墙——
  // 地面与投影阴影一起失效（实测：开关阴影的画面均值 99.063 → 99.063，完全不变）。
  //
  // 所以 `ground: true` 只保留**绕 Y 的朝向**：水平面绕竖直轴转向有意义，
  // 绕 X/Z 倾斜与"地面"语义冲突。旧脚本的意图（要一个水平地面）因此被保住。
  if (obj.ground) obj.rotate = math::Vec3{0.0f, obj.rotate.y, 0.0f};
  // ## 地面的朝向（兼容旧脚本的必经一笔）
  //
  // `ground: true` 的平面在旧语义里是"朝 +Z 的立面"，脚本普遍用
  // `"rotate": [-π/2, 0, 0]` 把它扳成水平。而现在的 `plane` 图元**本就朝 +Y**，
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
  if (const Json* emissive = json_find(node, "emission_color"); emissive != nullptr) {
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
      "wedge",    "tetrahedron", "octahedron", "dodecahedron", "icosahedron", "helix"};
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

auto parse_scene_spec(const Json& json) -> Result<SceneSpec> {
  if (!json.is_object()) {
    return st::unexpected(ErrorCode::Parse, "场景描述：顶层需要对象");
  }
  SceneSpec spec{};

  const Json* camera = json_find(json, "camera");
  if (camera != nullptr) {
    if (!camera->is_object()) {
      return st::unexpected(ErrorCode::Parse, "camera：需要对象");
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
    const auto attached = st::raster::attach_by_parent(flat, parents);
    if (!attached) return st::forward_error(attached.error());
    spec.objects.clear();
    spec.objects.reserve(flat.size());
    for (const st::raster::SceneNode& root : flat) spec.objects.push_back(from_node(root));
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

  // ## 尺寸取值：显式写的 > 该形状的历史字段 > 图元库默认值
  //
  // 历史字段（旧脚本在用，不能改语义）：
  //   sphere → radius；box → width/height/depth；plane → width/height；
  //   cylinder → radius/height；torus → radius。
  // 通用字段（新脚本自然写法）：radius（半尺寸）、depth（Y 向长度）。
  const auto radius_of = [&](float fallback) {
    if (object.has(SceneObjectSpec::kSpecifiedRadius)) return object.radius;
    return fallback;
  };
  // Y 向长度：`depth` 与 `height` 都常被用来表达"高"，谁写了用谁（depth 优先）
  const auto length_y = [&](float fallback) {
    if (object.has(SceneObjectSpec::kSpecifiedDepth)) return object.depth;
    if (object.has(SceneObjectSpec::kSpecifiedHeight)) return object.height;
    return fallback;
  };

  if (shape == "sphere") {
    params.radius = radius_of(object.radius);
    return primitives::make_uv_sphere(params);
  }
  if (shape == "box") {
    // 旧语义：`width`(X) × `height`(Y) × `depth`(Z)；通用写法用 `radius` 表达半边长。
    params.radius = object.has(SceneObjectSpec::kSpecifiedWidth) ? object.width * 0.5f
                                                                 : radius_of(0.5f);
    params.depth = length_y(1.0f);
    params.height = object.has(SceneObjectSpec::kSpecifiedDepth) ? object.depth : 0.0f;
    return primitives::make_cube(params);
  }
  if (shape == "plane") {
    // 旧语义：`width` × `height`，朝 +Z（立面）。新图元朝 +Y；要立面写 `face_camera`。
    params.radius = object.has(SceneObjectSpec::kSpecifiedWidth) ? object.width * 0.5f
                                                                 : radius_of(0.8f);
    params.depth = object.has(SceneObjectSpec::kSpecifiedHeight)
                       ? object.height
                       : (object.has(SceneObjectSpec::kSpecifiedDepth) ? object.depth : 1.6f);
    return primitives::make_plane(params);
  }
  if (shape == "cylinder") {
    // 旧语义：`radius` + `height`。通用写法用 `depth` 表达高。
    params.radius = radius_of(0.5f);
    params.depth = length_y(1.0f);
    return primitives::make_cylinder(params);
  }
  if (shape == "torus" || shape == "torus_knot") {
    // 旧 `torus` 语义：`radius` 是环外半径、管径固定为 0.35×；新参数可显式覆盖
    params.major_radius = object.major_radius > 0.0f ? object.major_radius : radius_of(0.6f);
    params.minor_radius = object.minor_radius > 0.0f ? object.minor_radius
                                                     : params.major_radius * 0.35f;
    params.minor_segments = object.minor_segments;
    params.major_segments = object.segments;
    params.twist = object.twist;
    if (shape == "torus") return primitives::make_torus(params);
    return primitives::make_torus_knot(params);
  }

  // 其余形状直接走图元库：`radius` + `depth` 是通用的一套，
  // 旧字段 `height` 在 `depth` 有意义处优先（与 `PrimitiveParams` 的约定一致）。
  params.radius = object.radius;
  params.depth = object.depth;
  params.height = object.height;
  params.top_radius = object.top_radius;
  params.major_radius = object.major_radius > 0.0f ? object.major_radius : object.radius;
  params.minor_radius = object.minor_radius > 0.0f ? object.minor_radius
                                                   : object.major_radius * 0.35f;
  params.minor_segments = object.minor_segments;
  params.major_segments = object.segments;
  params.twist = object.twist;
  params.sides = object.sides;
  return primitives::make_from_name(shape, params);
}

auto render_scene(const SceneSpec& spec, Canvas& canvas, math::Color background)
    -> Result<void> {
  auto scene = Scene3D::create(canvas.physical_width(), canvas.physical_height());
  if (!scene.has_value()) {
    return st::unexpected(ErrorCode::Unsupported, "无法创建三维场景（后端不可用）");
  }
  return render_scene_into(spec, **scene, canvas, background);
}

auto render_scene_into(const SceneSpec& spec, Scene3D& scene, Canvas& canvas,
                       math::Color background) -> Result<void> {
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
          } else {
            material.metallic = obj.metallic;
            material.roughness = obj.roughness;
            material.specular = obj.specular;
            material.opacity = obj.opacity;
          }
          scene.set_material(material);
          scene.draw_mesh(gltf_scene->meshes[mesh_index], world * node.transform);
        }
      }
      return {};
    }

    const auto mesh = build_mesh(resolved_object);
    if (!mesh) return st::forward_error(mesh.error());

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
  const auto resolved = st::raster::resolve(roots);
  if (!resolved) return st::forward_error(resolved.error());

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
    if (!object.group) {
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
    // 枢轴 = 预设位置 − 节点原点（在**父坐标系**里表达枢轴，因为 local_matrix
    // 把它加在局部坐标系中，而局部原点在父坐标系里的位置正是 translation）
    const st::raster::Bounds box = bounds_of(bounds_of, *found->second, node.name);
    if (box.is_empty()) return node.world;
    const math::Vec3 pivot_local = st::raster::pivot_from_bounds(box, *preset);
    st::raster::SceneNode patched = to_node(*found->second);
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
    if (!obj.group) {
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
