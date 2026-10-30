#include "st/raster/api_manifest.hpp"

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
  // `all_kind_names()` 返回逗号分隔串
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

}  // namespace

auto minimal_example_script() -> std::string {
  // 刻意保持极短（十行以内）：模型要先能跑通再谈复杂。
  // 每一行都只用到**最基础**的字段，避免"示例里已经在用高级特性"。
  return R"json({
  "camera": { "eye": [2.2, 1.6, 2.8], "target": [0, 0.25, 0], "fov": 42 },
  "lighting": { "key_direction": [0.4, 0.8, 0.6], "ambient": 0.3, "diffuse": 0.8 },
  "shadow": { "strength": 0.35, "ground_y": 0 },
  "objects": [
    { "name": "ground", "shape": "plane", "radius": 2.0, "depth": 4.0,
      "color": "#3a3f48", "ground": true },
    { "name": "ball", "shape": "uv_sphere", "radius": 0.4,
      "color": "#c8a07a", "translate": [0, 0.4, 0],
      "pivot_at": "bottom" }
  ]
})json";
}

auto build_api_manifest() -> Json {
  Json manifest = Json::object();
  manifest["version"] = "1";
  manifest["generated_for"] = "shuangtian/raster/scene3d_script";
  // 一句话说明这份清单怎么用（模型第一眼就该知道）
  manifest["_usage"] =
      "这是场景脚本的字段契约。写脚本前先查对应的 scope；"
      "所有字段名必须精确匹配（拼错会报错并给出候选，不会静默忽略）。";

  // ── 图元 ──
  Json primitives = Json::object();
  primitives["names"] = primitive_names();
  primitives["_note"] = "shape 字段填其中之一，或 obj:<路径> / gltf:<路径>";
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
        // 示例是 JSON 片段：能解析就嵌入成值（模型读起来更直观），
        // 解析不了就留成字符串（至少不丢信息）
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

  // ── 常见错误（把踩过的坑集中一处，模型最容易犯的）──
  Json mistakes = Json::array();
  mistakes.push_back("字段名拼错（raduis）→ 报错并给候选；不要猜，看报错里的候选");
  mistakes.push_back("rotate/value 用**弧度**，不是角度：90 度 = 1.5708");
  mistakes.push_back("plane 的 radius 是半边宽、depth 是全长（不是半长）");
  mistakes.push_back("cube 的 depth 是 Y 向长度、height 是 Z 向长度");
  mistakes.push_back("模型半径 < 0.1 时必须把 camera.near 调小，否则渲染出空白（实测 3cm 道具）");
  mistakes.push_back("key_direction 是**指向光源**的方向，写反会让阴影朝反方向");
  mistakes.push_back("父节点不可见时整棵子树都不可见（visible 会继承）");
  mistakes.push_back("同一场景内节点名必须唯一（动画与骨架按名字引用）");
  manifest["common_mistakes"] = mistakes;

  return manifest;
}

auto api_manifest_text(bool pretty) -> std::string {
  return json_dump(build_api_manifest(), pretty);
}

}  // namespace st::raster
