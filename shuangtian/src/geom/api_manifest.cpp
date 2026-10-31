#include "st/raster/api_manifest.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "st/core/json.hpp"
#include "st/raster/animation.hpp"
#include "st/raster/primitives.hpp"
#include "st/raster/scene_schema.hpp"

namespace st::raster {

namespace {

/// 图元名与别名（从图元库直接取，不手抄——否则新增图元时清单会漏）。
auto primitive_names() -> std::vector<std::string> {
  std::vector<std::string> names;
  const std::string all = primitives::all_kind_names();
  std::string current;
  for (const char ch : all) {
    if (ch == ',' || ch == ' ') {
      if (!current.empty()) {
        names.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) names.push_back(current);
  return names;
}

/// 一个形状的 `size` 说明（**唯一真源**，`st_api --shape` 与文档都从这推）。
struct ShapeSizeInfo {
  const char* shape;
  /// `size` 各项的含义（按顺序）。1 项表示标量。
  const char* items;
  /// 一句示例。
  const char* example;
  /// 旧写法换算（迁移与告警用）。
  const char* legacy;
};

/// 形状 → `size` 含义表。
///
/// ## 为什么集中在这张表里
///
/// "这个形状的 size 怎么写"是模型最高频的问题（实测：旧设计下必须
/// 去读 2.6 万字的 geometry.md 或者赌一把）。答案集中一处，
/// `st_api --shape` 直接输出，文档也从这里生成。
///
/// 这张表与 `build_mesh` 的换算逻辑必须同步——测试
/// `size_is_full_length_for_every_shape` 逐个量了包围盒，对不上会红。
constexpr ShapeSizeInfo kShapeSizes[] = {
    {"cube", "[X, Y, Z] 三边全长", R"j({"shape":"cube","size":[2,1,3]})j",
     "width→size[0], depth→size[1] 与 size[2], height 在旧语义里**无效**"},
    {"box", "[X, Y, Z] 三边全长", R"j({"shape":"box","size":[2,1,3]})j",
     "width→size[0], depth→size[1] 与 size[2], height 在旧语义里**无效**"},
    {"plane", "[X, Z] 两向全长（平面在 XZ 面）", R"j({"shape":"plane","size":[2,4]})j",
     "width→size[0], height→size[1]"},
    {"uv_sphere", "直径（标量）", R"j({"shape":"uv_sphere","size":2})j", "radius×2 → size"},
    {"sphere", "直径（标量）", R"j({"shape":"sphere","size":2})j", "radius×2 → size"},
    {"ico_sphere", "直径（标量）", R"j({"shape":"ico_sphere","size":2})j", "radius×2 → size"},
    {"cylinder", "[直径, 高]", R"j({"shape":"cylinder","size":[2,4]})j",
     "radius×2 → size[0], height 或 depth → size[1]"},
    {"cone", "[底面直径, 高]", R"j({"shape":"cone","size":[2,4]})j",
     "radius×2 → size[0], height 或 depth → size[1]"},
    {"capsule", "[直径, 全长（含两端半球）]", R"j({"shape":"capsule","size":[1,3]})j",
     "radius×2 → size[0], height 或 depth → size[1]"},
    {"torus", "[外径, 管径]", R"j({"shape":"torus","size":[2,0.4]})j",
     "radius 是**中心线**半径：外径 = 2×(radius + 0.35×radius), 管径 = 2×0.35×radius"},
    {"torus_knot", "[外径, 管径]", R"j({"shape":"torus_knot","size":[2,0.4]})j",
     "同 torus"},
    {"prism", "[外接圆直径, 高]", R"j({"shape":"prism","size":[2,4],"sides":6})j",
     "radius×2 → size[0], depth → size[1]"},
    {"gear", "[外径, 厚度]", R"j({"shape":"gear","size":[2,0.3],"teeth":12})j",
     "radius×2 → size[0], depth → size[1]"},
    {"disc", "直径（标量）", R"j({"shape":"disc","size":2})j", "radius×2 → size"},
    {"circle", "直径（标量）", R"j({"shape":"circle","size":2})j", "radius×2 → size"},
    {"grid", "[X, Z] 两向全长", R"j({"shape":"grid","size":[4,4],"columns":8})j",
     "radius×2 → size[0], depth → size[1]"},
    {"tube", "[直径, 高]", R"j({"shape":"tube","size":[2,4]})j", "radius×2 → size[0], depth → size[1]"},
    {"helix", "[直径, 高]", R"j({"shape":"helix","size":[2,4],"turns":3})j",
     "radius×2 → size[0], depth → size[1]"},
    {"wedge", "[X, Y, Z] 三边全长", R"j({"shape":"wedge","size":[2,1,3]})j",
     "radius×2 → size[0], depth → size[1], height → size[2]"},
    {"arrow", "[直径, 高]", R"j({"shape":"arrow","size":[0.4,2]})j",
     "radius×2 → size[0], depth → size[1]"},
    {"tetrahedron", "边长（标量）或围球直径", R"j({"shape":"tetrahedron","size":2})j",
     "radius×2 → size"},
    {"octahedron", "围球直径（标量）", R"j({"shape":"octahedron","size":2})j", "radius×2 → size"},
    {"dodecahedron", "围球直径（标量）", R"j({"shape":"dodecahedron","size":2})j", "radius×2 → size"},
    {"icosahedron", "围球直径（标量）", R"j({"shape":"icosahedron","size":2})j", "radius×2 → size"},
};

}  // namespace

auto build_api_manifest() -> Json {
  Json manifest = Json::object();
  manifest["version"] = "2";
  manifest["generated_for"] = "shuangtian/raster/scene3d_script";
  manifest["_usage"] =
      "这是场景脚本的字段契约。写脚本前先查对应的 scope；"
      "尺寸一律用 size（全长）。所有字段名必须精确匹配"
      "（拼错会报错并给出候选，不会静默忽略）。";
  manifest["_breaking_change"] =
      "v2 起尺寸语义统一：用 size（全长）。旧字段 radius/width/height/depth "
      "仍可用但会告警，且它们在不同形状下含义不同（这正是要淘汰它们的原因）。"
      "查某个形状的 size 怎么写：st_api --shape <名字>";

  // ── 图元 ──
  Json primitives = Json::object();
  primitives["names"] = primitive_names();
  primitives["_note"] = "shape 字段填其中之一，或 obj:<路径> / gltf:<路径>";
  // 每个形状的 size 含义（模型最需要的一列）
  Json sizes = Json::object();
  for (const ShapeSizeInfo& info : kShapeSizes) {
    Json entry = Json::object();
    entry["size_means"] = info.items;
    if (const auto parsed = json_parse(info.example); parsed.has_value()) {
      entry["example"] = *parsed;
    } else {
      entry["example"] = info.example;
    }
    entry["legacy"] = info.legacy;
    sizes[info.shape] = entry;
  }
  primitives["size_by_shape"] = sizes;
  manifest["primitives"] = primitives;

  // ── 动画枚举 ──
  Json tracks = Json::array();
  for (const TrackKind kind :
       {TrackKind::Translation, TrackKind::Rotation, TrackKind::Scale, TrackKind::Visible}) {
    tracks.push_back(std::string(track_kind_name(kind)));
  }
  manifest["tracks"] = tracks;

  Json easings = Json::array();
  for (const Easing easing : {Easing::Linear, Easing::Constant, Easing::EaseIn, Easing::EaseOut,
                              Easing::EaseInOut}) {
    easings.push_back(std::string(easing_name(easing)));
  }
  manifest["easings"] = easings;

  // ── 各层字段（从字段表推，唯一真源）──
  Json scopes = Json::object();
  for (const FieldTable& table : scene_field_tables()) {
    Json fields = Json::array();
    for (const FieldSpec& field : table.fields) {
      Json entry = Json::object();
      entry["name"] = field.name;
      entry["type"] = field.type;
      if (field.required) entry["required"] = true;
      if (!field.range.empty()) entry["range"] = field.range;
      if (!field.summary.empty()) entry["summary"] = field.summary;
      if (!field.example.empty()) {
        if (const auto parsed = json_parse(field.example); parsed.has_value()) {
          entry["example"] = *parsed;
        } else {
          entry["example"] = field.example;
        }
      }
      if (!field.pitfalls.empty()) {
        Json pitfalls = Json::array();
        for (const std::string& pitfall : field.pitfalls) pitfalls.push_back(pitfall);
        entry["pitfalls"] = pitfalls;
      }
      fields.push_back(entry);
    }
    Json scope = Json::object();
    scope["fields"] = fields;
    scopes[table.scope] = scope;
  }
  manifest["scopes"] = scopes;

  // ── 起步脚本 ──
  Json quickstart = Json::object();
  quickstart["_note"] = "最小可运行脚本（直接可用；改数值即可看到效果）";
  if (const auto parsed = json_parse(minimal_example_script()); parsed.has_value()) {
    quickstart["script"] = *parsed;
  }
  manifest["quickstart"] = quickstart;

  // ── 常见错误（把踩过的坑集中一处）──
  Json mistakes = Json::array();
  mistakes.push_back("**尺寸用 size（全长）**：写 size:[2,4] 就是 2 宽 4 高，没有半长");
  mistakes.push_back("字段名拼错（raduis）→ 报错并给候选；不要猜，看报错里的候选");
  mistakes.push_back("`rotation` 与动画里 rotate 轨道的 value 都用**度**：写 90 就是转 90 度");
  mistakes.push_back("尺寸只有 `size`（全长）；旧字段 radius/width/height/depth 已删除，写了会报错");
  mistakes.push_back("模型半径 < 0.05 时必须把 camera.near 调小，否则渲染出空白");
  mistakes.push_back("light_from 是**指向光源**的方向，写反会让阴影朝反方向");
  mistakes.push_back("父节点不可见时整棵子树都不可见（visible 会继承）");
  mistakes.push_back("同一场景内节点名必须唯一（动画与骨架按名字引用）");
  mistakes.push_back("写完用 st_api --check 核尺寸：包围盒会打印出来，能立刻发现写错");
  manifest["common_mistakes"] = mistakes;

  return manifest;
}

auto minimal_example_script() -> std::string {
  // 刻意保持极短（十行以内）：模型要先能跑通再谈复杂。
  // 每一行都只用到**最基础**的字段，避免"示例里已经在用高级特性"。
  //
  // 尺寸一律用 `size`（全长）—— 这是首选写法，示例必须示范它。
  return R"json({
  "camera": { "eye": [2.2, 1.6, 2.8], "target": [0, 0.25, 0], "fov": 42 },
  "lighting": { "light_from": [0.4, 0.8, 0.6], "ambient": 0.3, "diffuse": 0.8 },
  "shadow": { "strength": 0.35, "ground_y": 0 },
  "objects": [
    { "name": "ground", "shape": "plane", "size": [4, 4],
      "color": "#3a3f48", "ground": true },
    { "name": "ball", "shape": "uv_sphere", "size": 0.8,
      "color": "#c8a07a", "translate": [0, 0.4, 0],
      "pivot_at": "bottom" }
  ]
})json";
}

auto api_manifest_text(bool pretty) -> std::string {
  return json_dump(build_api_manifest(), pretty);
}

auto shape_size_doc(std::string_view shape) -> std::string {
  for (const ShapeSizeInfo& info : kShapeSizes) {
    if (shape == info.shape) {
      return std::format(
          "形状 {}：\n"
          "  size 含义：{}\n"
          "  示例：{}\n"
          "  旧写法换算：{}\n"
          "  注意：size 一律是**全长**，没有任何半长写法。",
          info.shape, info.items, info.example, info.legacy);
    }
  }
  // 不认识时列出可用名字（而不是只说"没有"）
  std::string all;
  for (const ShapeSizeInfo& info : kShapeSizes) {
    if (!all.empty()) all += ", ";
    all += info.shape;
  }
  return std::format("没有形状 \"{}\" 的尺寸说明。已知形状：{}", shape, all);
}

}  // namespace st::raster
