#include "st/raster/scene3d_script.hpp"

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

  obj.radius = static_cast<float>(json_get_double(node, "radius", obj.radius));
  obj.width = static_cast<float>(json_get_double(node, "width", obj.width));
  obj.height = static_cast<float>(json_get_double(node, "height", obj.height));
  obj.depth = static_cast<float>(json_get_double(node, "depth", obj.depth));
  obj.segments = static_cast<int>(json_get_i64(node, "segments", obj.segments));

  obj.ground = json_get_bool(node, "ground", false);
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
  static constexpr const char* kShapes[] = {"sphere", "box", "plane", "cylinder", "torus"};
  const bool is_obj_shape = obj.shape.rfind("obj:", 0) == 0;
  bool known = is_obj_shape;
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
  if (object.shape == "sphere") {
    return Mesh::colored_sphere(object.radius, std::max(object.segments, 4), object.color);
  }
  if (object.shape == "box") {
    return Mesh::colored_box(object.width, object.height, object.depth, object.color);
  }
  if (object.shape == "plane") {
    return Mesh::plane(object.width, object.height, object.color);
  }
  if (object.shape == "cylinder") {
    return Mesh::cylinder(object.radius, object.height, std::max(object.segments, 3),
                          object.color);
  }
  if (object.shape == "torus") {
    return Mesh::torus(object.radius, object.radius * 0.35f, std::max(object.segments, 3),
                       std::max(object.segments / 2, 3), object.color);
  }
  if (object.shape.rfind("obj:", 0) == 0) {
    // 不带 `asset_basis` 的重载：`build_mesh` 只拿到对象、拿不到场景。
    // 带基准目录的那条在 `draw_one` 里单独处理（场景级信息在那才可见）。
    return Mesh::load_obj_file(object.shape.substr(4), object.color);
  }
  return st::unexpected(ErrorCode::Unsupported, "不支持的形状：" + object.shape);
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
  const auto draw_one = [&](const SceneObjectSpec& obj) -> Result<void> {
    // `obj:` 的路径基准目录是**场景级**信息，`build_mesh` 只拿到对象，
    // 所以在这里先重写成已解析路径再交给它。
    SceneObjectSpec resolved = obj;
    if (obj.shape.rfind("obj:", 0) == 0) {
      resolved.shape = "obj:" + resolve_shape_path(obj.shape.substr(4), spec.asset_basis);
    }
    const auto mesh = build_mesh(resolved);
    if (!mesh) return st::forward_error(mesh.error());

    Material material{};
    material.metallic = obj.metallic;
    material.roughness = obj.roughness;
    material.specular = obj.specular;
    material.emission = obj.emission;
    material.opacity = obj.opacity;
    // 自发光色与“纯发光体”开关。
    //
    // 为何 `emission_unlit` 要把漫反射项**清零**：Blender 的 Emission 不受光照影响，
    // 而本管线的 `emission` 是“加在受光结果上”的。两者差异在暗场景里很明显：
    // 霓虹灯管应当自己亮着，而不是“被环境照亮后再亮一点”。
    material.emissive_color = obj.emissive_color;
    material.emissive_color_set = obj.emissive_color_set;
    material.emission_unlit = obj.emission_unlit;
    scene.set_material(material);

    // 变换顺序：缩放 → 绕 X → 绕 Y → 绕 Z → 平移。
    //
    // 为何是"缩放先"：先平移再缩放会把位移也放大（那不是使用者的预期）。
    math::Mat4 model = math::Mat4::translation(obj.translate);
    if (std::abs(obj.rotate.z) > 1e-6f) {
      model = model * math::Mat4::rotation(math::Vec3{0.0f, 0.0f, 1.0f}, obj.rotate.z);
    }
    if (std::abs(obj.rotate.y) > 1e-6f) {
      model = model * math::Mat4::rotation(math::Vec3{0.0f, 1.0f, 0.0f}, obj.rotate.y);
    }
    if (std::abs(obj.rotate.x) > 1e-6f) {
      model = model * math::Mat4::rotation(math::Vec3{1.0f, 0.0f, 0.0f}, obj.rotate.x);
    }
    model = model * math::Mat4::scaling(obj.scale);
    scene.draw_mesh(*mesh, model);
    return {};
  };

  for (const SceneObjectSpec& obj : spec.objects) {
    if (!obj.ground) continue;
    const auto status = draw_one(obj);
    if (!status) return st::forward_error(status.error());
  }
  scene.set_planar_shadow(spec.shadow);
  for (const SceneObjectSpec& obj : spec.objects) {
    if (obj.ground) continue;
    const auto status = draw_one(obj);
    if (!status) return st::forward_error(status.error());
  }

  scene.end_frame(canvas,
                      math::Rect{0.0f, 0.0f, static_cast<float>(canvas.physical_width()),
                                 static_cast<float>(canvas.physical_height())});
  scene.flush();
  return {};
}

}  // namespace st::raster
