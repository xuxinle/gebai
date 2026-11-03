#include "st/raster/scene_xml.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "st/core/json.hpp"
#include "st/raster/scene_schema.hpp"
#include "st/raster/scene_serialize.hpp"

namespace st::raster {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// 标签字典：唯一的「标签 → 语义」表（双向序列化共用）
// ─────────────────────────────────────────────────────────────────────────────

/// XML 标签 → JSON 的 `shape` 值（小写 snake_case）。
///
/// ## 为什么单独一张表而不是导出 `scene3d_script.cpp` 的 `kShapes`
///
/// `kShapes` 的语义是「shape 字段接受什么」（JSON 层校验），这里的语义是
/// 「XML 标签怎么写」（形态映射）。两张表**服务的层面不同**，但必须保持
/// 一致——维护约定与字段表相同：那边加图元，这边同步加标签，
/// `scene_xml_test.cpp` 里有一条逐项比对测试盯着（漏加会红）。
///
/// 大小写宽松：`<sphere>` `<SPHERE>` 都收（解析处首字母归一）。
/// 末尾是别名标签（与 JSON 别名同名映射；序列化一律输出规范标签）。
const std::map<std::string, std::string, std::less<>>& tag_to_shape_table() {
  static const std::map<std::string, std::string, std::less<>> table = {
      {"Sphere", "sphere"},         {"Box", "box"},
      {"Plane", "plane"},           {"Cylinder", "cylinder"},
      {"Torus", "torus"},           {"UvSphere", "uv_sphere"},
      {"Cube", "cube"},             {"IcoSphere", "ico_sphere"},
      {"Cone", "cone"},             {"Capsule", "capsule"},
      {"Disc", "disc"},             {"Circle", "circle"},
      {"Grid", "grid"},             {"Tube", "tube"},
      {"Prism", "prism"},           {"Gear", "gear"},
      {"TorusKnot", "torus_knot"},  {"Arrow", "arrow"},
      {"Wedge", "wedge"},           {"Tetrahedron", "tetrahedron"},
      {"Octahedron", "octahedron"}, {"Dodecahedron", "dodecahedron"},
      {"Icosahedron", "icosahedron"}, {"Helix", "helix"},
      // 别名标签
      {"Ball", "sphere"},           {"Donut", "torus"},
      {"Spring", "helix"},          {"Cog", "gear"},
      {"Knot", "torus_knot"},
  };
  return table;
}

/// 内置图元名 → 序列化用的规范标签。
const std::map<std::string, std::string_view>& shape_to_tag_table() {
  static const std::map<std::string, std::string_view> table = {
      {"sphere", "Sphere"},         {"box", "Box"},
      {"plane", "Plane"},           {"cylinder", "Cylinder"},
      {"torus", "Torus"},           {"uv_sphere", "UvSphere"},
      {"cube", "Cube"},             {"ico_sphere", "IcoSphere"},
      {"cone", "Cone"},             {"capsule", "Capsule"},
      {"disc", "Disc"},             {"circle", "Circle"},
      {"grid", "Grid"},             {"tube", "Tube"},
      {"prism", "Prism"},           {"gear", "Gear"},
      {"torus_knot", "TorusKnot"},  {"arrow", "Arrow"},
      {"wedge", "Wedge"},           {"tetrahedron", "Tetrahedron"},
      {"octahedron", "Octahedron"}, {"dodecahedron", "Dodecahedron"},
      {"icosahedron", "Icosahedron"}, {"helix", "Helix"},
  };
  return table;
}

auto shape_for_tag(std::string_view canonical) -> std::optional<std::string> {
  const auto& table = tag_to_shape_table();
  const auto it = table.find(canonical);
  if (it == table.end()) return std::nullopt;
  return it->second;
}

/// 标签 → 规范形（大小写完全宽松：`<sphere>` `<SPHERE>` `<Sphere>` 等价）。
///
/// 与 xml-ui「标签 = 组件注册名，另有短名别名」同一宽松度；
/// 返回的是字典里的规范拼法（用于查 shape 与报错文案）。
auto canonical_tag(std::string_view raw) -> std::optional<std::string> {
  for (const auto& [name, shape] : tag_to_shape_table()) {
    if (name.size() != raw.size()) continue;
    bool equal = true;
    for (std::size_t i = 0; i < raw.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(name[i])) !=
          std::tolower(static_cast<unsigned char>(raw[i]))) {
        equal = false;
        break;
      }
    }
    if (equal) return name;
  }
  return std::nullopt;
}

auto tag_for_shape(const std::string& shape) -> std::optional<std::string_view> {
  const auto& table = shape_to_tag_table();
  const auto it = table.find(shape);
  if (it == table.end()) return std::nullopt;
  return it->second;
}

/// 结构标签集合（非图元）：`<Scene>` 的直接子级。
constexpr std::string_view kStructTags[] = {
    "Camera", "Lighting", "Shadow", "Animation", "Skeleton", "Scene",
};

auto is_struct_tag(std::string_view tag) -> bool {
  // 大小写宽松（<camera> 也认）：结构标签只有 6 个，线性比对够用。
  for (const std::string_view t : kStructTags) {
    if (tag.size() != t.size()) continue;
    bool equal = true;
    for (std::size_t i = 0; i < tag.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(tag[i])) !=
          std::tolower(static_cast<unsigned char>(t[i]))) {
        equal = false;
        break;
      }
    }
    if (equal) return true;
  }
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// 报错：统一「第 N 行 <Tag>：消息」格式（行号是 XML 相对 JSON 的独有优势）
// ─────────────────────────────────────────────────────────────────────────────

auto at(const xml::Node& node, std::string_view message) -> std::string {
  return std::format("第 {} 行 <{}>：{}", node.line, node.name, message);
}

/// 「；你是不是想写 a / b」尾缀（候选列表的统一拼法）。
auto suggest_suffix(const std::vector<std::string>& suggestions) -> std::string {
  if (suggestions.empty()) return {};
  std::string suffix = "；你是不是想写 ";
  for (std::size_t i = 0; i < suggestions.size(); ++i) {
    if (i != 0) suffix += " / ";
    suffix += suggestions[i];
  }
  return suffix;
}

/// 未知属性报错（编辑距离候选来自字段表——**不重复维护**候选清单）。
auto unknown_attr_error(const xml::Node& node, std::string_view attr,
                        const FieldTable& table) -> std::string {
  return at(node, std::format("未知属性 \"{}\"{}", attr,
                              suggest_suffix(suggest_fields(attr, table.names()))));
}

// ─────────────────────────────────────────────────────────────────────────────
// 属性值 → Json 值（按字段表声明的类型转换；类型不符 = 报错，不静默）
// ─────────────────────────────────────────────────────────────────────────────

/// 按逗号切分（容忍空格与尾逗号）。
auto split_csv(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> parts;
  std::string current;
  for (const char ch : text) {
    if (ch == ',') {
      if (!current.empty()) parts.push_back(current);
      current.clear();
    } else if (ch != ' ' && ch != '\t') {
      current.push_back(ch);
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

/// 数字解析（严格：整串必须是有限数字，局部成功不算）。
auto parse_number_strict(std::string_view text) -> std::optional<double> {
  try {
    std::size_t used = 0;
    const double value = std::stod(std::string(text), &used);
    if (used != text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

/// 属性文本 → 按字段类型转换后的 Json 值。
///
/// ## 为什么在这里做类型转换而不是塞字符串让 JSON 层管
///
/// JSON 层看到的将是「字符串类型」（XML 属性都是字符串），它无法区分
/// `"0.5"` 是「写错的字符串」还是「XML 来的数字」。所以类型解释是
/// **XML 前端的本职**：按字段表声明的类型转，转不动就报错（带行号）。
auto attr_to_json(const xml::Node& node, const std::string& attr,
                  const std::string& text, const FieldSpec& field) -> Result<Json> {
  const std::string where =
      std::format("第 {} 行 <{} {}=\"…\">", node.line, node.name, attr);
  const auto fail = [&](std::string detail) -> Result<Json> {
    return st::unexpected(ErrorCode::Parse, where + "：" + detail);
  };

  if (field.type == "bool") {
    // 与 `xml::Node::attr_bool` 同一套词汇（true/1/yes/false/0/no）。
    if (text == "true" || text == "1" || text == "yes") return Json(true);
    if (text == "false" || text == "0" || text == "no") return Json(false);
    return fail("布尔要写 true/false（收到 \"" + text + "\"）");
  }
  if (field.type == "number" || field.type == "number/array") {
    // `number/array`：统一尺寸字段 `size`——标量或 1~3 个数（逗号串）；
    // 纯 `number` 走 parts.size()==1 的同一条路（split_csv 对无逗号串返回单元素）。
    //
    // `auto`（仅 size 的某项）：connect 的自动长度标记——该项作为字符串元素
    // 透传（JSON 层 connect 解析接住并填真值）。整串 auto 则透传整串。
    const auto parts = split_csv(text);
    const bool has_auto =
        std::find(parts.begin(), parts.end(), std::string("auto")) != parts.end();
    if (has_auto) {
      if (parts.size() == 1) return Json(text);
      Json array = Json::array();
      for (const auto& part : parts) {
        if (part == "auto") {
          array.push_back(part);
        } else {
          const auto value = parse_number_strict(part);
          if (!value) return fail("不是数字（\"" + part + "\"）");
          array.push_back(*value);
        }
      }
      return array;
    }
    if (field.type == "number" && parts.size() != 1) {
      return fail("要写单个数字（收到逗号串）");
    }
    if (parts.size() > 3) return fail("size 最多三个数（收到更多）");
    if (parts.size() == 1) {
      const auto value = parse_number_strict(parts[0]);
      if (!value) return fail("不是数字（\"" + parts[0] + "\"）");
      return Json(*value);
    }
    Json array = Json::array();
    for (const auto& part : parts) {
      const auto value = parse_number_strict(part);
      if (!value) return fail("不是数字（\"" + part + "\"）");
      // 整数值输出整数（size="24, 2" → [24, 2] 而非 [24.0, 2.0]）。
      if (*value == std::floor(*value) && std::abs(*value) < 1e15) {
        array.push_back(static_cast<std::int64_t>(*value));
      } else {
        array.push_back(*value);
      }
    }
    return array;
  }
  if (field.type == "vec3") {
    const auto parts = split_csv(text);
    if (parts.size() != 3) {
      return fail("vec3 要写三个数（x, y, z），收到 " + std::to_string(parts.size()) + " 个");
    }
    Json array = Json::array();
    for (const auto& part : parts) {
      const auto value = parse_number_strict(part);
      if (!value) return fail("不是数字（\"" + part + "\"）");
      array.push_back(*value);
    }
    return array;
  }
  if (field.type == "color") {
    // 颜色两形：`#rrggbb[aa]` 或 `r,g,b[,a]`（0-255，与 JSON 数组形一致）。
    if (text.rfind('#', 0) == 0) return Json(text);
    const auto parts = split_csv(text);
    if (parts.size() < 3 || parts.size() > 4) {
      return fail("颜色要写 #rrggbb 或 r,g,b[,a]");
    }
    Json array = Json::array();
    for (const auto& part : parts) {
      const auto value = parse_number_strict(part);
      if (!value) return fail("不是数字（\"" + part + "\"）");
      array.push_back(*value);
    }
    return array;
  }
  // string（pivot_at / cull / projection / gltf / texture …）：原样。
  return Json(text);
}

// ─────────────────────────────────────────────────────────────────────────────
// XML 树 → Json（核心转换；每层都做未知属性检查，报错带行号）
// ─────────────────────────────────────────────────────────────────────────────

/// 按字段表把节点的属性逐个写进 `out`（未知属性报错带候选）。
auto write_attrs(const xml::Node& node, const FieldTable& table, Json& out) -> Status {
  for (const auto& [attr, text] : node.attributes) {
    const FieldSpec* field = table.find(attr);
    if (field == nullptr) {
      return st::unexpected(ErrorCode::Parse, unknown_attr_error(node, attr, table));
    }
    auto value = attr_to_json(node, attr, text, *field);
    if (!value) return st::forward_error(value.error());
    out[attr] = std::move(*value);
  }
  return st::ok();
}

/// 图元节点 → 对象 Json（含递归 children）。
auto object_to_json(const xml::Node& node, Json& out) -> Status {
  // 标签 → 规范形（大小写完全宽松）→ shape。
  const std::string tag = canonical_tag(node.name).value_or(std::string(node.name));
  const bool is_group = tag == "Group";
  const bool is_object_ref = tag == "Object";
  const auto shape_of_tag = shape_for_tag(tag);
  if (!shape_of_tag && !is_group && !is_object_ref) {
    if (is_struct_tag(tag)) {
      return st::unexpected(
          ErrorCode::Parse,
          at(node, std::format("<{}> 是结构标签，不能出现在物体层", tag)));
    }
    // 未知图元：候选 = 全部图元标签（拼错如 <Sphrre> → Sphere）。
    std::vector<std::string> candidates;
    candidates.reserve(tag_to_shape_table().size());
    for (const auto& [name, shape] : tag_to_shape_table()) candidates.push_back(name);
    return st::unexpected(
        ErrorCode::Parse,
        at(node, "未知图元标签" + suggest_suffix(suggest_fields(tag, candidates))));
  }
  // 标签语义：图元 → shape；Group → group=true（无几何）；
  // Object → 外部引用容器，shape/gltf 走属性（`obj:`/`gltf:` 路径无法当标签名）。
  if (shape_of_tag) out["shape"] = *shape_of_tag;
  if (is_group) out["group"] = true;

  const FieldTable* table = find_field_table("objects[]");
  bool object_ref_has_shape = false;
  for (const auto& [attr, text] : node.attributes) {
    if (attr == "shape") {
      if (is_object_ref) {
        // <Object shape="obj:chair.obj">：唯一能写 shape 属性的场合。
        object_ref_has_shape = true;
        out["shape"] = text;
        continue;
      }
      return st::unexpected(
          ErrorCode::Parse,
          at(node, "shape 由标签名表达（<Sphere>），不要再写 shape 属性"));
    }
    if (attr == "group" && is_group) {
      // <Group> 自带语义，重复写 group 属性只会引歧义。
      return st::unexpected(
          ErrorCode::Parse,
          at(node, "<Group> 标签已表达 group，不必再写 group 属性"));
    }
    const FieldSpec* field = table->find(attr);
    if (field == nullptr) {
      return st::unexpected(ErrorCode::Parse, unknown_attr_error(node, attr, *table));
    }
    auto value = attr_to_json(node, attr, text, *field);
    if (!value) return st::forward_error(value.error());
    out[attr] = std::move(*value);
  }
  if (is_object_ref && !object_ref_has_shape && !out.contains("gltf")) {
    return st::unexpected(
        ErrorCode::Parse,
        at(node, "<Object> 需要 shape 属性（obj:<路径>）或 gltf 属性——它就是为外部引用准备的"));
  }

  // 子元素 = children（递归；`<Key>` 等结构子元素不会出现在物体层）。
  Json children = Json::array();
  for (const auto& child : node.children) {
    Json child_json = Json::object();
    if (const auto status = object_to_json(child, child_json); !status) {
      return st::forward_error(status.error());
    }
    children.push_back(std::move(child_json));
  }
  if (!children.empty()) out["children"] = std::move(children);
  return st::ok();
}

/// `<ExtraLight direction="…" intensity="…"/>`（lighting 的子元素）。
auto extra_light_to_json(const xml::Node& node, Json& out) -> Status {
  const FieldTable* table = find_field_table("lighting");
  // extra_lights 数组元素层没有独立 scope（字段表按 9 层登记）；direction /
  // color / intensity 三个名字都按 lighting 层的字段类型解释（direction 是
  // vec3、其余是 number），未知属性仍报错——不因为数组层就放任。
  static constexpr std::string_view kKnown[] = {"direction", "color", "intensity"};
  for (const auto& [attr, text] : node.attributes) {
    const FieldSpec* field = table->find(attr);
    const bool known_extra =
        std::find(kKnown, kKnown + 3, attr) != (kKnown + 3) || field != nullptr;
    if (!known_extra) {
      std::vector<std::string> candidates{"direction", "color", "intensity"};
      return st::unexpected(ErrorCode::Parse,
                            at(node, std::format("未知属性 \"{}\"{}", attr,
                                                 suggest_suffix(suggest_fields(attr, candidates)))));
    }
    // 类型：direction 按 vec3、color 按 color、intensity 按 number。
    if (field != nullptr) {
      auto value = attr_to_json(node, attr, text, *field);
      if (!value) return st::forward_error(value.error());
      out[attr] = std::move(*value);
      continue;
    }
    // lighting 表里没有（direction/color/intensity 不在 lighting 顶层字段里，
    // 只在 extra_lights 的 example 里出现）：按名字定类型。
    FieldSpec adhoc;
    adhoc.name = attr;
    adhoc.type = attr == "direction" ? "vec3" : (attr == "color" ? "color" : "number");
    auto value = attr_to_json(node, attr, text, adhoc);
    if (!value) return st::forward_error(value.error());
    out[attr] = std::move(*value);
  }
  return st::ok();
}

/// `<Track>`：属性 + `<Key>` 子元素数组。
auto track_to_json(const xml::Node& node, Json& out) -> Status {
  const FieldTable* table = find_field_table("animation.tracks[]");
  if (const auto status = write_attrs(node, *table, out); !status) {
    return st::forward_error(status.error());
  }
  const FieldTable* key_table = find_field_table("animation.tracks[].keys[]");
  Json keys = Json::array();
  for (const auto& child : node.children) {
    if (child.name != "Key") {
      return st::unexpected(
          ErrorCode::Parse,
          at(child, std::format("<Track> 里只能放 <Key>，收到 <{}>", child.name)));
    }
    Json key = Json::object();
    for (const auto& [attr, text] : child.attributes) {
      const FieldSpec* field = key_table->find(attr);
      if (field == nullptr) {
        return st::unexpected(ErrorCode::Parse, unknown_attr_error(child, attr, *key_table));
      }
      auto value = attr_to_json(child, attr, text, *field);
      if (!value) return st::forward_error(value.error());
      key[attr] = std::move(*value);
    }
    keys.push_back(std::move(key));
  }
  out["keys"] = std::move(keys);
  return st::ok();
}

/// `<Skeleton joints="root, arm" inverse_bind="…"/>`。
///
/// `inverse_bind`（16 数 × 关节数的矩阵数组）**不提供属性写法**——一串矩阵
/// 塞进逗号属性不可读也不可查错。骨架带逆绑定时请用 JSON 形态（或程序构造）。
auto skeleton_to_json(const xml::Node& node, Json& out) -> Status {
  const FieldTable* table = find_field_table("skeleton");
  for (const auto& [attr, text] : node.attributes) {
    if (attr == "joints") {
      Json joints = Json::array();
      for (const auto& part : split_csv(text)) joints.push_back(part);
      out["joints"] = std::move(joints);
      continue;
    }
    if (attr == "inverse_bind") {
      return st::unexpected(
          ErrorCode::Parse,
          at(node, "inverse_bind 不支持 XML 属性写法（矩阵数组不可读）；请用 JSON 形态"));
    }
    const FieldSpec* field = table->find(attr);
    if (field == nullptr) {
      return st::unexpected(ErrorCode::Parse, unknown_attr_error(node, attr, *table));
    }
    auto value = attr_to_json(node, attr, text, *field);
    if (!value) return st::forward_error(value.error());
    out[attr] = std::move(*value);
  }
  return st::ok();
}

/// `<Scene>` 的一个直接子级 → scene Json 的对应键。
auto scene_child_to_json(const xml::Node& child, Json& scene) -> Status {
  // 结构标签大小写宽松（canonical 后再分发）；非结构标签 = 图元
  // （object_to_json 自己也做 canonical，这里的回退只影响 Group/Object
  // 这两个不在图元字典里的名字——它们保持原样交给下层判定）。
  const std::string name = canonical_tag(child.name).value_or(std::string(child.name));
  if (name == "Camera" || name == "Lighting" || name == "Shadow") {
    const std::string scope =
        name == "Camera" ? "camera" : (name == "Lighting" ? "lighting" : "shadow");
    const FieldTable* table = find_field_table(scope);
    Json node_json = Json::object();
    if (const auto status = write_attrs(child, *table, node_json); !status) {
      return st::forward_error(status.error());
    }
    // Lighting 的 <ExtraLight> 子元素 → extra_lights 数组。
    if (name == "Lighting") {
      Json extra = Json::array();
      for (const auto& sub : child.children) {
        if (sub.name != "ExtraLight") {
          return st::unexpected(
              ErrorCode::Parse,
              at(sub, std::format("<Lighting> 里只能放 <ExtraLight>，收到 <{}>", sub.name)));
        }
        Json light = Json::object();
        if (const auto status = extra_light_to_json(sub, light); !status) {
          return st::forward_error(status.error());
        }
        extra.push_back(std::move(light));
      }
      if (!extra.empty()) node_json["extra_lights"] = std::move(extra);
    } else if (!child.children.empty()) {
      return st::unexpected(ErrorCode::Parse,
                            at(child, std::format("<{}> 不接受子元素", name)));
    }
    scene[scope] = std::move(node_json);
    return st::ok();
  }
  if (name == "Animation") {
    const FieldTable* table = find_field_table("animation");
    Json anim = Json::object();
    if (const auto status = write_attrs(child, *table, anim); !status) {
      return st::forward_error(status.error());
    }
    Json tracks = Json::array();
    for (const auto& sub : child.children) {
      if (sub.name != "Track") {
        return st::unexpected(
            ErrorCode::Parse,
            at(sub, std::format("<Animation> 里只能放 <Track>，收到 <{}>", sub.name)));
      }
      Json track = Json::object();
      if (const auto status = track_to_json(sub, track); !status) {
        return st::forward_error(status.error());
      }
      tracks.push_back(std::move(track));
    }
    if (!tracks.empty()) anim["tracks"] = std::move(tracks);
    scene["animation"] = std::move(anim);
    return st::ok();
  }
  if (name == "Skeleton") {
    Json skel = Json::object();
    if (const auto status = skeleton_to_json(child, skel); !status) {
      return st::forward_error(status.error());
    }
    scene["skeleton"] = std::move(skel);
    return st::ok();
  }
  // 其余 = 图元标签（objects）。
  Json obj = Json::object();
  if (const auto status = object_to_json(child, obj); !status) {
    return st::forward_error(status.error());
  }
  scene["objects"].push_back(std::move(obj));
  return st::ok();
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 对外入口：parse
// ─────────────────────────────────────────────────────────────────────────────

auto parse_scene_xml_node(const xml::Node& root) -> Result<SceneSpec> {
  Json scene = Json::object();
  scene["objects"] = Json::array();

  if (root.name == "Scene") {
    // <Scene> 自身的属性：scene 层字段表（cull / asset_basis）。
    const FieldTable* table = find_field_table("scene");
    if (const auto status = write_attrs(root, *table, scene); !status) {
      return st::forward_error(status.error());
    }
    for (const auto& child : root.children) {
      if (const auto status = scene_child_to_json(child, scene); !status) {
        return st::forward_error(status.error());
      }
    }
  } else {
    // 裸图元根：<Sphere>…</Sphere> 直接当 objects[0]（方便粘一段就预览）。
    Json obj = Json::object();
    if (const auto status = object_to_json(root, obj); !status) {
      return st::forward_error(status.error());
    }
    scene["objects"].push_back(std::move(obj));
  }

  return parse_scene_spec(scene);
}

auto parse_scene_xml(std::string_view source) -> Result<SceneSpec> {
  xml::Error error{};
  const auto document = xml::parse(source, &error);
  if (!document) {
    return st::unexpected(
        ErrorCode::Parse,
        std::format("XML 解析失败（第 {} 行）：{}", error.line, error.message));
  }
  return parse_scene_xml_node(document->root);
}

// ─────────────────────────────────────────────────────────────────────────────
// 反向：SceneSpec → XML（复用 serialize_scene 的 Json，确定性转换）
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// 数字 → 最短等价文本（`1` 不写 `1.0`）。
auto number_text(double value) -> std::string {
  if (value == std::floor(value) && std::abs(value) < 1e15) {
    return std::format("{}", static_cast<std::int64_t>(value));
  }
  return std::format("{}", value);
}

/// 标量 Json → 属性值文本。
auto scalar_to_attr(const Json& value) -> std::optional<std::string> {
  if (value.is_number_integer()) {
    return number_text(static_cast<double>(value.get<std::int64_t>()));
  }
  if (value.is_number_float()) return number_text(value.get<double>());
  if (value.is_boolean()) {
    return value.get<bool>() ? std::string("true") : std::string("false");
  }
  if (value.is_string()) return value.get<std::string>();
  return std::nullopt;
}

/// 数组（vec3/color/size）→ "a, b, c" 文本（全数字数组才转）。
auto array_to_attr(const Json& value) -> std::optional<std::string> {
  if (!value.is_array() || value.empty()) return std::nullopt;
  std::string text;
  for (const auto& item : value) {
    if (!item.is_number()) return std::nullopt;
    if (!text.empty()) text += ", ";
    text += number_text(item.get<double>());
  }
  return text;
}

/// Json 值 → 属性文本（转不动 = nullopt，调用方决定怎么写）。
auto value_to_attr(const Json& value) -> std::optional<std::string> {
  if (value.is_array()) return array_to_attr(value);
  return scalar_to_attr(value);
}

/// 把一个 Json 对象的「标量/数字数组」键值对写成属性串（追加到 out）。
auto write_json_attrs(const Json& object, std::string& out,
                      const std::vector<std::string>& skip = {}) -> void {
  for (const auto& [key, value] : object.items()) {
    if (std::find(skip.begin(), skip.end(), key) != skip.end()) continue;
    if (auto text = value_to_attr(value)) {
      out += " " + key + "=\"" + std::string(xml::escape(*text)) + "\"";
    }
  }
}

/// 物体 Json → XML 片段。
///
/// 标签选择（与解析侧的受理规则对称，往返测试盯着）：
/// - 内置图元 → 规范标签（Sphere/Box/…）；
/// - `group: true` → `<Group>`（不写 shape——分组无几何）；
/// - 其余（`obj:`/`gltf:` 外部引用）→ `<Object>`，shape/gltf 走属性
///   （路径里可以有 `:`/`/` 等字符，当不了标签名）。
auto object_to_xml(const Json& object, std::string& out, std::string_view indent) -> void {
  const std::string shape = object.value("shape", std::string("sphere"));
  const bool is_group = object.value("group", false);
  const bool has_gltf = object.contains("gltf") && object.at("gltf").is_string();
  std::string tag;
  if (!is_group) {
    if (const auto canonical = tag_for_shape(shape)) {
      tag = std::string(*canonical);
    } else {
      tag = "Object";
    }
  } else {
    tag = "Group";
  }

  out += indent;
  out += "<" + tag;
  if (tag == "Object") {
    // <Object>：shape（obj: 路径）与 gltf 都是属性，谁在写谁。
    out += " shape=\"" + std::string(xml::escape(shape)) + "\"";
  }
  write_json_attrs(object, out, is_group ? std::vector<std::string>{"shape", "children", "gltf", "group"}
                                         : std::vector<std::string>{"shape", "children", "gltf"});
  if (has_gltf) {
    out += " gltf=\"" + std::string(xml::escape(object.at("gltf").get<std::string>())) + "\"";
  }

  const auto children = object.find("children");
  if (children == object.end() || children->empty()) {
    out += "/>\n";
    return;
  }
  out += ">\n";
  for (const auto& child : *children) {
    object_to_xml(child, out, std::string(indent) + "  ");
  }
  out += indent;
  out += "</" + tag + ">\n";
}

}  // namespace

auto serialize_scene_xml(const SceneSpec& spec) -> std::string {
  const Json json = serialize_scene(spec);
  std::string out = "<Scene";
  // <Scene> 自身：cull / asset_basis（其余顶层键都是子元素）。
  write_json_attrs(json, out, {"camera", "lighting", "shadow", "objects", "animation",
                               "skeleton"});
  out += ">\n";

  const auto write_struct = [&](const char* key, const char* tag,
                                const std::vector<std::string>& skip = {}) {
    if (!json.contains(key)) return;
    const Json& node = json.at(key);
    out += "  <" + std::string(tag);
    write_json_attrs(node, out, skip);
    out += "/>\n";
  };

  write_struct("camera", "Camera");
  // Lighting：extra_lights → <ExtraLight> 子元素。
  if (json.contains("lighting")) {
    const Json& node = json.at("lighting");
    const auto extra = node.find("extra_lights");
    if (extra == node.end() || extra->empty()) {
      out += "  <Lighting";
      write_json_attrs(node, out);
      out += "/>\n";
    } else {
      out += "  <Lighting";
      write_json_attrs(node, out, {"extra_lights"});
      out += ">\n";
      for (const auto& light : *extra) {
        out += "    <ExtraLight";
        write_json_attrs(light, out);
        out += "/>\n";
      }
      out += "  </Lighting>\n";
    }
  }
  write_struct("shadow", "Shadow");

  if (json.contains("skeleton")) {
    const Json& skel = json.at("skeleton");
    out += "  <Skeleton";
    if (skel.contains("joints")) {
      std::string joints;
      for (const auto& joint : skel.at("joints")) {
        if (!joints.empty()) joints += ", ";
        joints += joint.get<std::string>();
      }
      out += " joints=\"" + std::string(xml::escape(joints)) + "\"";
    }
    // inverse_bind：XML 不支持（见 skeleton_to_json 的注释），省略——
    // 省略时 JSON 层按单位阵处理，语义仍可往返。
    out += "/>\n";
  }

  if (json.contains("animation")) {
    const Json& anim = json.at("animation");
    const auto tracks = anim.find("tracks");
    if (tracks == anim.end() || tracks->empty()) {
      out += "  <Animation";
      write_json_attrs(anim, out);
      out += "/>\n";
    } else {
      out += "  <Animation";
      write_json_attrs(anim, out, {"tracks"});
      out += ">\n";
      for (const auto& track : *tracks) {
        out += "    <Track";
        write_json_attrs(track, out, {"keys"});
        out += ">\n";
        for (const auto& key : track.value("keys", Json::array())) {
          out += "      <Key";
          write_json_attrs(key, out);
          out += "/>\n";
        }
        out += "    </Track>\n";
      }
      out += "  </Animation>\n";
    }
  }

  if (json.contains("objects")) {
    for (const auto& object : json.at("objects")) {
      object_to_xml(object, out, "  ");
    }
  }
  out += "</Scene>\n";
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// looks_like_scene_xml
// ─────────────────────────────────────────────────────────────────────────────

auto looks_like_scene_xml(std::string_view source) -> bool {
  std::size_t i = 0;
  if (source.substr(0, 3) == "\xEF\xBB\xBF") i = 3;  // BOM
  while (i < source.size() && (source[i] == ' ' || source[i] == '\t' ||
                               source[i] == '\r' || source[i] == '\n')) {
    ++i;
  }
  if (i >= source.size() || source[i] != '<') return false;
  ++i;
  // <?xml …?> 声明：跳过后再看真正的根标签。
  if (i < source.size() && source[i] == '?') {
    const auto end = source.find("?>", i);
    if (end == std::string_view::npos) return false;
    i = end + 2;
    while (i < source.size() && (source[i] == ' ' || source[i] == '\t' ||
                                 source[i] == '\r' || source[i] == '\n')) {
      ++i;
    }
    if (i >= source.size() || source[i] != '<') return false;
    ++i;
  } else if (i < source.size() && source[i] == '!') {
    return false;  // <!-- 注释或 <!DOCTYPE：按 JSON 路走（那边会给出清晰报错）
  }
  std::string tag;
  while (i < source.size() &&
         (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_')) {
    tag.push_back(source[i]);
    ++i;
  }
  if (tag.empty()) return false;
  // 大小写完全宽松（与解析同一套 canonical_tag）。
  if (canonical_tag(tag).has_value()) return true;
  // <Scene>/<scene>/<SCENE>：与结构标签同一宽松度。
  if (tag.size() == 5) {
    static constexpr std::string_view kScene = "Scene";
    for (std::size_t k = 0; k < tag.size(); ++k) {
      if (std::tolower(static_cast<unsigned char>(tag[k])) !=
          std::tolower(static_cast<unsigned char>(kScene[k]))) {
        return false;
      }
    }
    return true;
  }
  return false;
}

}  // namespace st::raster
