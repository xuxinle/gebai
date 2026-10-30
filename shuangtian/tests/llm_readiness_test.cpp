/// 大模型可用性四项的回归测试。
///
/// 这四条钉的都是**"模型能不能自己发现自己写错了"**：
/// 1. 未知字段检查（拼错必须报错 + 给候选）
/// 2. 机读能力清单（清单与真实字段表一致，不漂移）
/// 3. 反向序列化（读→改→写闭环，语义保真）
/// 4. 场景自检（能指出"画面上会有什么问题"）
#include <algorithm>
#include <string>
#include <vector>

#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/raster/api_manifest.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_describe.hpp"
#include "st/raster/scene_schema.hpp"
#include "st/raster/scene_serialize.hpp"
#include "st/test/test.hpp"

namespace {

auto parse(std::string_view text) -> st::Result<st::raster::SceneSpec> {
  return st::raster::parse_scene_spec_text(text);
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// ① 未知字段检查
// ────────────────────────────────────────────────────────────────────────────

ST_TEST(schema_unknown_field_is_rejected_with_suggestion) {
  // ## 拼错字段必须报错并给候选（本轮最核心的一条）
  //
  // 旧行为：`raduis` 被静默忽略，用默认值 0.8 —— 解析"成功"但画面不变，
  // 调用方只能反复试错。这正是上一轮探针实测到的 5 个静默错误之一。
  const auto spec = parse(R"j({"objects":[{"shape":"cube","raduis":0.5}]})j");
  ST_REQUIRE(!spec.has_value());
  ST_CHECK(spec.error().code == st::ErrorCode::Parse);
  const std::string message = spec.error().message;
  // 必须指出：哪一层、哪个字段
  ST_CHECK(message.find("objects[0]") != std::string::npos);   // 路径
  ST_CHECK(message.find("raduis") != std::string::npos);       // 拼错的名字
  // 必须给出候选（模型才能一次改对）
  ST_CHECK(message.find("radius") != std::string::npos);
}

ST_TEST(schema_unknown_field_covers_all_layers) {
  // 每一层都要查：顶层、camera、lighting、objects、animation、tracks、keys、skeleton
  struct Case {
    const char* label;
    const char* json;
    const char* expect_path;
  };
  const Case cases[] = {
      {"顶层", R"j({"camra":{},"objects":[]})j", "scene.camra"},
      {"camera", R"j({"camera":{"fy":42},"objects":[]})j", "camera.fy"},
      {"lighting", R"j({"lighting":{"ambientt":0.3},"objects":[]})j", "lighting.ambientt"},
      {"shadow", R"j({"shadow":{"strenght":0.3},"objects":[]})j", "shadow.strenght"},
      {"objects", R"j({"objects":[{"shape":"cube","translaet":[0,1,0]}]})j",
       "objects[0].translaet"},
      {"animation", R"j({"objects":[],"animation":{"traks":[]}})j", "animation.traks"},
      {"tracks", R"j({"objects":[],"animation":{"tracks":[{"node":"a","kind":"rotate","keys":[],"easng":"linear"}]}})j",
       "animation.tracks[0].easng"},
      {"keys", R"j({"objects":[],"animation":{"tracks":[{"node":"a","kind":"rotate","keys":[{"t":0,"value":[0,0,0],"easing2":"x"}]}]}})j",
       "animation.tracks[0].keys[0].easing2"},
      {"skeleton", R"j({"objects":[],"skeleton":{"joint":["a"]}})j", "skeleton.joint"},
  };
  for (const Case& item : cases) {
    const auto spec = parse(item.json);
    ST_REQUIRE(!spec.has_value());
    const std::string message = spec.error().message;
    st::log::out("[schema] {} → {}\n", item.label, message);
    if (message.find(item.expect_path) == std::string::npos) {
      ST_CHECK(false);   // 报错里必须带该层路径
    }
  }
}

ST_TEST(schema_edit_distance_and_suggestions) {
  // 底层工具：编辑距离与候选选择
  ST_CHECK_EQ(st::raster::edit_distance("radius", "radius", 10), 0);
  ST_CHECK_EQ(st::raster::edit_distance("raduis", "radius", 10), 2);   // 相邻换位
  ST_CHECK_EQ(st::raster::edit_distance("xyz", "abc", 10), 3);

  const std::vector<std::string> candidates{"radius", "rings", "width", "depth"};
  const auto suggestions = st::raster::suggest_fields("raduis", candidates, 2, 3);
  ST_REQUIRE(!suggestions.empty());
  ST_CHECK_EQ(suggestions.front(), std::string("radius"));

  // 大小写差异应当算"距离 0 的候选"（`Radius` 明显是想写 `radius`）
  const auto cased = st::raster::suggest_fields("Radius", candidates, 2, 3);
  ST_REQUIRE(!cased.empty());
  ST_CHECK_EQ(cased.front(), std::string("radius"));

  // 完全无关时不给候选（避免胡乱指路）
  ST_CHECK(st::raster::suggest_fields("zzzzzz", candidates, 2, 3).empty());
}

ST_TEST(schema_field_type_errors_are_reported) {
  // 类型错必须报错（`json_get_double` 会静默退回默认值，不能靠它）
  const auto string_for_number = parse(R"j({"objects":[{"shape":"cube","radius":"0.5"}]})j");
  ST_REQUIRE(!string_for_number.has_value());
  ST_CHECK(string_for_number.error().message.find("期望数字") != std::string::npos);

  const auto bool_for_number = parse(R"j({"objects":[{"shape":"cube","segments":true}]})j");
  ST_REQUIRE(!bool_for_number.has_value());

  const auto bad_vec3 = parse(R"j({"objects":[{"shape":"cube","translate":[0,1]}]})j");
  ST_REQUIRE(!bad_vec3.has_value());
  ST_CHECK(bad_vec3.error().message.find("3 个数字") != std::string::npos);

  const auto bad_range = parse(R"j({"objects":[{"shape":"uv_sphere","segments":-5}]})j");
  ST_REQUIRE(!bad_range.has_value());
  ST_CHECK(bad_range.error().message.find("至少") != std::string::npos);
}

ST_TEST(schema_valid_script_still_parses) {
  // 新检查不能误伤合法脚本（这是最需要防的回归）
  const auto spec = parse(R"({
    "camera": {"eye":[2,1.6,2.8],"target":[0,0.25,0],"fov":42},
    "lighting": {"key_direction":[0.4,0.8,0.6],"ambient":0.3},
    "shadow": {"strength":0.35,"ground_y":0},
    "objects": [
      {"name":"ground","shape":"plane","radius":2.0,"depth":4.0,"ground":true},
      {"name":"ball","shape":"uv_sphere","radius":0.4,"translate":[0,0.4,0],
       "pivot_at":"bottom","metallic":0.2,"roughness":0.6}
    ]
  })");
  ST_REQUIRE(spec.has_value());
  ST_CHECK_EQ(spec->objects.size(), 2U);

  // 最小示例（`st_api --example` 输出的那段）也必须合法
  const auto example = parse(st::raster::minimal_example_script());
  ST_REQUIRE(example.has_value());
  ST_CHECK_EQ(example->objects.size(), 2U);
}

// ────────────────────────────────────────────────────────────────────────────
// ② 机读能力清单
// ────────────────────────────────────────────────────────────────────────────

ST_TEST(manifest_is_valid_json_and_covers_all_scopes) {
  const std::string text = st::raster::api_manifest_text(false);
  const auto parsed = st::json_parse(text);
  ST_REQUIRE(parsed.has_value());

  // 每个字段表都必须出现在清单里（**防漂移**：加了表忘了进清单会在这里炸）
  const auto* scopes = st::json_find(*parsed, "scopes");
  ST_REQUIRE(scopes != nullptr);
  for (const auto& table : st::raster::scene_field_tables()) {
    if (st::json_find(*scopes, table.scope) == nullptr) {
      st::log::err("[manifest] 缺少 scope：{}\n", table.scope);
      ST_CHECK(false);
    }
  }

  // 图元、轨道、缓动都要有
  ST_REQUIRE(st::json_find(*parsed, "primitives") != nullptr);
  const auto* tracks = st::json_find(*parsed, "tracks");
  ST_REQUIRE(tracks != nullptr);
  ST_CHECK_EQ(tracks->size(), 4U);
  const auto* easings = st::json_find(*parsed, "easings");
  ST_REQUIRE(easings != nullptr);
  ST_CHECK_EQ(easings->size(), 5U);
}

ST_TEST(manifest_field_entries_match_schema) {
  // 清单里的每个字段都必须与字段表**逐项一致**（名称/类型/必填）
  const auto manifest = st::raster::build_api_manifest();
  const auto* scopes = st::json_find(manifest, "scopes");
  ST_REQUIRE(scopes != nullptr);
  for (const auto& table : st::raster::scene_field_tables()) {
    const auto* scope = st::json_find(*scopes, table.scope);
    ST_REQUIRE(scope != nullptr);
    const auto* fields = st::json_find(*scope, "fields");
    ST_REQUIRE(fields != nullptr);
    ST_CHECK_EQ(fields->size(), table.fields.size());
    for (std::size_t i = 0; i < table.fields.size() && i < fields->size(); ++i) {
      const auto& actual = (*fields)[i];
      const auto& expected = table.fields[i];
      ST_CHECK_EQ(st::json_get_string(actual, "name", ""), expected.name);
      ST_CHECK_EQ(st::json_get_string(actual, "type", ""), expected.type);
      ST_CHECK_EQ(st::json_get_bool(actual, "required", false), expected.required);
    }
  }
}

// ────────────────────────────────────────────────────────────────────────────
// ③ 反向序列化（往返一致性）
// ────────────────────────────────────────────────────────────────────────────

namespace {

/// 比较两个 spec 的关键语义（不是逐字段——有些字段是归一化产物）。
void check_round_trip(std::string_view original, const char* label) {
  const auto first = parse(original);
  ST_REQUIRE(first.has_value());
  const auto text = st::raster::serialize_scene_text(*first, false);
  const auto second = parse(text);
  if (!second.has_value()) {
    st::log::err("[roundtrip:{}] 再解析失败：{}\n序列化结果：{}\n", label,
                 second.error().message, text);
    ST_REQUIRE(second.has_value());
  }

  // 物体数量与名字必须一致
  ST_CHECK_EQ(second->objects.size(), first->objects.size());
  for (std::size_t i = 0; i < first->objects.size() && i < second->objects.size(); ++i) {
    const auto& a = first->objects[i];
    const auto& b = second->objects[i];
    ST_CHECK_EQ(b.name, a.name);
    ST_CHECK_EQ(b.shape, a.shape);
    // 尺寸：往返后必须一致（这是"语义保真"的核心）
    ST_CHECK_NEAR(b.radius, a.radius, 1.0e-5f);
    ST_CHECK_NEAR(b.depth, a.depth, 1.0e-5f);
    ST_CHECK_NEAR(b.height, a.height, 1.0e-5f);
    ST_CHECK_NEAR(b.width, a.width, 1.0e-5f);
    // 变换
    ST_CHECK_NEAR(b.translate.x, a.translate.x, 1.0e-5f);
    ST_CHECK_NEAR(b.translate.y, a.translate.y, 1.0e-5f);
    ST_CHECK_NEAR(b.translate.z, a.translate.z, 1.0e-5f);
    ST_CHECK_NEAR(b.rotate.x, a.rotate.x, 1.0e-5f);
    ST_CHECK_NEAR(b.scale.x, a.scale.x, 1.0e-5f);
    // 材质
    ST_CHECK_NEAR(b.metallic, a.metallic, 1.0e-5f);
    ST_CHECK_NEAR(b.roughness, a.roughness, 1.0e-5f);
    ST_CHECK_NEAR(b.opacity, a.opacity, 1.0e-5f);
    ST_CHECK_EQ(b.ground, a.ground);
    ST_CHECK_EQ(b.group, a.group);
    // `specified` 掩码也要保真：否则读回时另一条分支会给出不同尺寸
    ST_CHECK_EQ(b.specified, a.specified);
  }

  // 相机
  ST_CHECK_NEAR(second->camera.eye.x, first->camera.eye.x, 1.0e-5f);
  ST_CHECK_NEAR(second->camera.fov_y_degrees, first->camera.fov_y_degrees, 1.0e-5f);
  ST_CHECK_NEAR(second->camera.near_z, first->camera.near_z, 1.0e-6f);
}

}  // namespace

ST_TEST(serialize_round_trip_minimal) {
  check_round_trip(st::raster::minimal_example_script(), "minimal");
}

ST_TEST(serialize_round_trip_rich_scene) {
  // 覆盖：多种图元参数、嵌套 children、材质、贴图变换、动画、骨架、分组
  check_round_trip(R"({
    "camera": {"eye":[5,3,5],"target":[0,1,0],"up":[0,1,0],"fov":50,"near":0.01,"far":200},
    "lighting": {"key_direction":[0.5,0.7,0.5],"ambient":0.25,"diffuse":0.9,"rim":0.15},
    "shadow": {"strength":0.4,"ground_y":0,"lift":0.02},
    "cull": "none",
    "asset_basis": "assets",
    "objects": [
      {"name":"root","group":true,"translate":[0,0,0],"children":[
        {"name":"arm","shape":"capsule","radius":0.08,"depth":0.7,"rotate":[0,0,0.4],
         "metallic":0.7,"roughness":0.3,"pivot_at":"top"},
        {"name":"hand","shape":"ico_sphere","radius":0.09,"subdivisions":2,
         "translate":[0,-0.7,0]}
      ]},
      {"name":"ground","shape":"plane","radius":3.0,"depth":6.0,"ground":true,
       "color":"#3a3f48","texture":"brick.png","texture_scale_u":4.0,"texture_scale_v":4.0,
       "texture_clamp":true},
      {"name":"ring","shape":"torus","major_radius":0.8,"minor_radius":0.12,
       "minor_segments":16,"segments":32,"color":"#ffcc44","emission":0.5}
    ],
    "animation": {"name":"wave","duration":2.0,"tracks":[
      {"node":"arm","kind":"rotate","keys":[
        {"t":0,"value":[0,0,0],"easing":"ease-in-out"},
        {"t":1,"value":[0,0,0.6]},
        {"t":2,"value":[0,0,0]}
      ]},
      {"node":"hand","kind":"scale","keys":[{"t":0,"value":[1,1,1]},{"t":1,"value":[1.2,1.2,1.2]}]}
    ]},
    "skeleton": {"joints":["root","arm","hand"]}
  })",
                   "rich");
}

ST_TEST(serialize_only_writes_non_defaults) {
  // 只写非默认值是刻意的（输出更短、改动更醒目）——但要保证"必须显式"的项在
  const auto spec = parse(R"j({"objects":[{"shape":"cube","radius":0.5}]})j");
  ST_REQUIRE(spec.has_value());
  const std::string text = st::raster::serialize_scene_text(*spec, false);
  // `objects` 必须写（必填字段）
  ST_CHECK(text.find("\"objects\"") != std::string::npos);
  // `shape` 与 `radius` 必须写（核心信息）
  ST_CHECK(text.find("\"shape\"") != std::string::npos);
  ST_CHECK(text.find("\"radius\"") != std::string::npos);
  // 大量默认值不应出现（否则输出膨胀、改动看不清）
  ST_CHECK(text.find("\"metallic\"") == std::string::npos);
  ST_CHECK(text.find("\"opacity\"") == std::string::npos);
  st::log::out("[serialize] 最小场景输出 {} 字节\n", text.size());
}

ST_TEST(serialize_read_modify_write_loop) {
  // ## 这是反向序列化的**真实用途**：读 → 改一处 → 写回
  //
  // 模型最常见的工作方式就是"在已有场景上改一个数值"，
  // 而不是每次重写整份脚本（重写常常顺手改坏没打算碰的地方）。
  const auto original = parse(R"({
    "camera": {"eye":[3,2,3],"target":[0,0.5,0]},
    "objects": [
      {"name":"ball","shape":"uv_sphere","radius":0.5,"translate":[0,0.5,0]},
      {"name":"box","shape":"cube","radius":0.4,"translate":[1.2,0.4,0]}
    ]
  })");
  ST_REQUIRE(original.has_value());

  // 改一处：把 ball 的半径从 0.5 改成 0.8
  auto modified = *original;
  modified.objects[0].radius = 0.8f;
  modified.objects[0].specified |= st::raster::SceneObjectSpec::kSpecifiedRadius;

  const auto text = st::raster::serialize_scene_text(modified, false);
  const auto reloaded = parse(text);
  ST_REQUIRE(reloaded.has_value());
  ST_CHECK_EQ(reloaded->objects.size(), 2U);
  ST_CHECK_NEAR(reloaded->objects[0].radius, 0.8f, 1.0e-5f);   // 改的生效了
  ST_CHECK_NEAR(reloaded->objects[1].radius, 0.4f, 1.0e-5f);   // 没改的没被动
  ST_CHECK_NEAR(reloaded->objects[1].translate.x, 1.2f, 1.0e-5f);
}

// ────────────────────────────────────────────────────────────────────────────
// ④ 场景自检摘要
// ────────────────────────────────────────────────────────────────────────────

ST_TEST(describe_reports_empty_scene) {
  const auto spec = parse(R"j({"objects":[]})j");
  ST_REQUIRE(spec.has_value());
  const auto summary = st::raster::describe_scene(*spec);
  ST_CHECK_EQ(summary.object_count, 0U);
  ST_CHECK(summary.has_error());   // 空场景必须报错
  bool found = false;
  for (const auto& entry : summary.diagnostics) {
    if (entry.code == "empty_scene") found = true;
  }
  ST_CHECK(found);
}

ST_TEST(describe_reports_healthy_scene) {
  // 正常场景不该有 error（可以有 info）
  const auto spec = parse(st::raster::minimal_example_script());
  ST_REQUIRE(spec.has_value());
  const auto summary = st::raster::describe_scene(*spec);
  ST_CHECK_EQ(summary.object_count, 2U);
  ST_CHECK(summary.total_triangles > 0U);
  ST_CHECK(summary.bounds_valid);
  ST_CHECK(!summary.has_error());
  ST_CHECK(summary.suggested_camera_distance > 0.0f);
  st::log::out("[describe] 三角形={} 建议距离={:.2f}\n", summary.total_triangles,
               static_cast<double>(summary.suggested_camera_distance));
}

ST_TEST(describe_catches_camera_inside_geometry) {
  // ## 相机在物体内部 → 画面全黑，但解析完全成功
  //
  // 这是"能解析但画面上是错的"的典型：所有字段都合法，
  // 只有看图才知道没东西。自检要在渲染前就指出来。
  const auto spec = parse(R"({
    "camera": {"eye":[0,0,0],"target":[1,0,0]},
    "objects": [{"shape":"uv_sphere","radius":1.0}]
  })");
  ST_REQUIRE(spec.has_value());
  const auto summary = st::raster::describe_scene(*spec);
  ST_CHECK(summary.has_error());
  bool found = false;
  for (const auto& entry : summary.diagnostics) {
    if (entry.code == "camera_inside_geometry") {
      found = true;
      ST_CHECK(!entry.suggestion.empty());   // 建议必须可执行
    }
  }
  ST_CHECK(found);
}

ST_TEST(describe_catches_near_plane_clipping) {
  // ## 这条钉住真实素材实测踩到的坑：3cm 道具 + near=0.1 → 渲染 0 像素
  const auto spec = parse(R"({
    "camera": {"eye":[0.1,0.1,0.2],"target":[0,0,0],"near":0.1,"fov":40},
    "objects": [{"shape":"uv_sphere","radius":0.03}]
  })");
  ST_REQUIRE(spec.has_value());
  const auto summary = st::raster::describe_scene(*spec);
  bool found = false;
  for (const auto& entry : summary.diagnostics) {
    if (entry.code == "near_plane_too_far") found = true;
  }
  ST_CHECK(found);
}

ST_TEST(describe_catches_animation_missing_node) {
  const auto spec = parse(R"({
    "objects": [{"name":"ball","shape":"cube"}],
    "animation": {"tracks":[{"node":"hand","kind":"rotate","keys":[{"t":0,"value":[0,0,0]}]}]}
  })");
  ST_REQUIRE(spec.has_value());
  const auto summary = st::raster::describe_scene(*spec);
  ST_CHECK(summary.has_error());
  bool found = false;
  for (const auto& entry : summary.diagnostics) {
    if (entry.code == "animation_missing_node") found = true;
  }
  ST_CHECK(found);
}

ST_TEST(describe_catches_texture_transform_without_texture) {
  const auto spec = parse(R"({
    "objects": [{"shape":"cube","texture_scale_u":4.0}]
  })");
  ST_REQUIRE(spec.has_value());
  const auto summary = st::raster::describe_scene(*spec);
  bool found = false;
  for (const auto& entry : summary.diagnostics) {
    if (entry.code == "texture_transform_without_texture") found = true;
  }
  ST_CHECK(found);
}

ST_TEST(describe_json_is_valid_and_has_facts) {
  const auto spec = parse(st::raster::minimal_example_script());
  ST_REQUIRE(spec.has_value());
  const auto summary = st::raster::describe_scene(*spec);
  const auto document = st::raster::summary_json(summary);
  const std::string text = st::json_dump(document, false);
  const auto parsed = st::json_parse(text);
  ST_REQUIRE(parsed.has_value());
  ST_REQUIRE(st::json_find(*parsed, "facts") != nullptr);
  ST_REQUIRE(st::json_find(*parsed, "diagnostics") != nullptr);
  // facts 里必须有相机距离（模型据此直接算视角，不必自己推）
  ST_REQUIRE(st::json_find(*st::json_find(*parsed, "facts"), "suggested_camera_distance") !=
             nullptr);
  // 文本形式也要可读
  const std::string readable = st::raster::summary_text(summary);
  ST_CHECK(readable.find("物体") != std::string::npos);
}
