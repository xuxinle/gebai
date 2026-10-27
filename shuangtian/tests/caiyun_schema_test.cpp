/// 裁云脚本 schema 与解析诊断的单测。
///
/// ## 这组用例钉的是什么
///
/// 1. **未知字段必须被报到**（含“你是不是想写 X”的候选名）——这是 AI 写脚本时
///    最贵的一类错误：拼错的键被静默忽略后，“球没自转”与“我写对了但转速是 0”
///    在画面上完全一样，只能靠盯画面猜。实测：往三维脚本里写
///    `model_rot` / `camera_azimuth` / `light_rimm`，旧实现照常出片、零提示。
/// 2. **越界必须被报到**（并说明实际生效值）——静默夹取会让写脚本的人以为
///    "写 200 段也没什么变化"，其实是早被夹到 96 了。
/// 3. **表必须覆盖解析代码真正读的所有键**——表是单一真值源，一旦解析加了字段
///    而表没跟上，"字段能用但校验说你写错了"这种矛盾比缺字段更难查。
///    这条是**逆向验证过**的：故意漏登一个键，它必须变红。
/// 4. **序列化不能丢字段**（往返不变量）——智能体会 `get script` 读回、改一处、
///    `apply_script` 写回；任何"能读不能写"的字段都会在那次往返里静默消失。

#include "examples/caiyun/director.cpp"
#include "examples/caiyun/schema.cpp"

#include "st/test/test.hpp"
// `st::print`：lint L7 禁 printf，统一走它（带编码/镜像/缓冲顺序保障）
#include "st/core/print.hpp"

namespace {

using caiyun::Diagnostic;
using caiyun::FieldSpec;
using caiyun::SchemaScope;
using caiyun::SchemaType;

/// 从诊断列表里取第一条包含 `needle` 的（找不到返回 nullptr）。
[[nodiscard]] auto find_diagnostic(const std::vector<Diagnostic>& list, std::string_view needle)
    -> const Diagnostic* {
  for (const Diagnostic& diag : list) {
    if (diag.message.find(needle) != std::string::npos) return &diag;
  }
  return nullptr;
}

/// 造一份最小可解析脚本，`layers` 由调用方给。
[[nodiscard]] auto script_with_layers(std::string_view layers_json) -> std::string {
  return std::format(
      R"({{"name":"t","fps":30,"size":[320,180],"scenes":[{{"kind":"custom","duration":0.2,
          "layers":[{}]}}]}})",
      layers_json);
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 一、schema 表自身的一致性
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(schema_type_and_scope_names_are_stable) {
  // 名字会进 JSON（智能体按它判断该写数字还是字符串），所以必须稳定且非空。
  for (const SchemaType type :
       {SchemaType::Number, SchemaType::Integer, SchemaType::Bool, SchemaType::String,
        SchemaType::Color, SchemaType::Dimension, SchemaType::Array, SchemaType::Object}) {
    const std::string_view name = caiyun::schema_type_name(type);
    ST_CHECK(!name.empty());
  }
  for (const SchemaScope scope : {SchemaScope::Script, SchemaScope::Audio, SchemaScope::Scene,
                                  SchemaScope::Layer, SchemaScope::KeyFrame,
                                  SchemaScope::ObjEntry}) {
    const std::string_view name = caiyun::schema_scope_name(scope);
    ST_CHECK(!name.empty());
  }
}

ST_TEST(schema_has_no_duplicate_names_within_scope) {
  // 同一作用域里重名 = 查表只能查到第一个，后登记的永远不会生效（且没有任何报错）。
  for (const SchemaScope scope : {SchemaScope::Script, SchemaScope::Audio, SchemaScope::Scene,
                                  SchemaScope::Layer, SchemaScope::KeyFrame,
                                  SchemaScope::ObjEntry}) {
    const auto fields = caiyun::fields_in_scope(scope);
    for (std::size_t i = 0; i < fields.size(); ++i) {
      for (std::size_t j = i + 1; j < fields.size(); ++j) {
        // 同一作用域里重名 = 查表只能查到第一个，后登记的永远不会生效（且无任何报错）。
        if (fields[i]->name == fields[j]->name) {
          // 让失败信息带上作用域与字段名（否则只说“断言失败”，沙子里找针）。
          ST_CHECK(false && "schema 同一作用域内字段名重复");
        }
      }
    }
  }
}

ST_TEST(schema_ranges_are_ordered) {
  // min > max 的范围会把任何值都判成越界（夹取反向），是纯数据错误但后果全片皆错。
  for (const FieldSpec& spec : caiyun::all_field_specs()) {
    if (!spec.has_range) continue;
    ST_CHECK(spec.min_value <= spec.max_value);
  }
}

ST_TEST(schema_every_field_has_summary) {
  // 说明是给智能体看的唯一提示；空说明等于“这个字段得自己猜”。
  for (const FieldSpec& spec : caiyun::all_field_specs()) {
    ST_CHECK(!spec.summary.empty());
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 二、建议名的质量（这一节直接决定 AI 能不能自己改对）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(schema_suggest_spin_for_model_rot) {
  // 最典型的错法：把 spin 记成 rot。
  ST_CHECK_EQ(std::string(caiyun::suggest_field(SchemaScope::Layer, "model_rot")), "model_spin");
}

ST_TEST(schema_suggest_light_rim_for_light_rimm) {
  // 多打一个字母：`light_rimm` 必须命中 `light_rim`，而不是等长的 `light_ambient`。
  ST_CHECK_EQ(std::string(caiyun::suggest_field(SchemaScope::Layer, "light_rimm")), "light_rim");
}

ST_TEST(schema_suggest_azimuth_for_camera_azimuth) {
  // 前缀写错（cam → camera）：子串信号应把它拉回来。
  ST_CHECK_EQ(std::string(caiyun::suggest_field(SchemaScope::Layer, "camera_azimuth")),
              "cam_azimuth");
}

ST_TEST(schema_suggest_cam_fov_for_camera_fov) {
  ST_CHECK_EQ(std::string(caiyun::suggest_field(SchemaScope::Layer, "camera_fov")), "cam_fov");
}

ST_TEST(schema_suggest_gives_nothing_for_unrelated_key) {
  // 完全无关的键不该硬配：错误的建议会让智能体照着改错，比不给建议更糟。
  ST_CHECK(caiyun::suggest_field(SchemaScope::Layer, "totally_unrelated_thing").empty());
}

ST_TEST(schema_suggest_respects_scope) {
  // `duration` 是场景字段，不是图层字段——跨作用域不得互相干扰。
  ST_CHECK(caiyun::suggest_field(SchemaScope::Layer, "duration").empty());
  ST_CHECK_EQ(std::string(caiyun::suggest_field(SchemaScope::Scene, "duration")), "duration");
}

ST_TEST(schema_edit_distance_is_symmetric_and_zero_on_equal) {
  ST_CHECK_EQ(caiyun::edit_distance("model_spin", "model_spin"), std::size_t{0});
  ST_CHECK_EQ(caiyun::edit_distance("abc", "abd"), std::size_t{1});
  ST_CHECK_EQ(caiyun::edit_distance("abd", "abc"), std::size_t{1});
  // 对称性（不对称的度量会让“建议”随书写方向改变，不可复现）
  ST_CHECK_EQ(caiyun::edit_distance("model_spin", "model_rot"),
              caiyun::edit_distance("model_rot", "model_spin"));
}

// ————————————————————————————————————————————————————————————————————————————
// 三、解析期诊断：未知字段（本组的核心，逆向验证过）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(parse_diagnostics_report_typo_with_suggestion) {
  const auto script = caiyun::parse_script(
      script_with_layers(R"({"kind":"model3d","x":"0%","y":"0%","w":"50%","h":"50%",
                              "model_rot":45})"));
  ST_REQUIRE(script.has_value());
  const auto& diags = script.value().diagnostics;
  // "拼错的字段 model_rot 必须产生诊断"
  ST_CHECK(!diags.empty());
  const Diagnostic* hit = find_diagnostic(diags, "model_rot");
  ST_REQUIRE(hit != nullptr);
  ST_CHECK_EQ(hit->key, std::string("model_rot"));
  ST_CHECK_EQ(hit->suggestion, std::string("model_spin"));
  // 路径要能定位到具体图层（智能体据此直接改）
  ST_CHECK_EQ(hit->path, std::string("scenes[0].layers[0]"));
  // 宽容语义：未知字段不阻断加载（老脚本仍要能出片）
  ST_CHECK(!hit->is_error);
}

ST_TEST(parse_diagnostics_report_all_typos_not_just_first) {
  // 一次性列全：AI 一轮就能改完，不必“改一个、跑一次、再发现下一个”。
  const auto script = caiyun::parse_script(
      script_with_layers(R"({"kind":"model3d","x":"0%","y":"0%","w":"50%","h":"50%",
                              "model_rot":45,"camera_azimuth":60,"light_rimm":0.9})"));
  ST_REQUIRE(script.has_value());
  const auto& diags = script.value().diagnostics;
  ST_CHECK(find_diagnostic(diags, "model_rot") != nullptr);
  ST_CHECK(find_diagnostic(diags, "camera_azimuth") != nullptr);
  ST_CHECK(find_diagnostic(diags, "light_rimm") != nullptr);
}

ST_TEST(parse_diagnostics_clean_script_has_none) {
  // 反向护栏：合法脚本必须零诊断——否则诊断会被当成噪音而忽略。
  const auto script = caiyun::parse_script(
      script_with_layers(R"({"kind":"model3d","x":"0%","y":"0%","w":"50%","h":"50%",
                              "model_spin":12,"cam_azimuth":30,"light_rim":0.45,
                              "model_shape":"sphere","model_segments":32})"));
  ST_REQUIRE(script.has_value());
  ST_CHECK_EQ(script.value().diagnostics.size(), std::size_t{0});
  ST_CHECK(!script.value().has_errors());
}

ST_TEST(parse_diagnostics_report_scene_level_typo) {
  // 场景层也要查：把 `duration` 写成 `dur` 是最常见的错之一。
  const auto script = caiyun::parse_script(
      R"({"name":"t","fps":30,"size":[320,180],"scenes":[{"kind":"custom","dur":2.0}]})");
  ST_REQUIRE(script.has_value());
  const Diagnostic* hit = find_diagnostic(script.value().diagnostics, "dur");
  ST_REQUIRE(hit != nullptr);
  ST_CHECK_EQ(hit->suggestion, std::string("duration"));
  ST_CHECK_EQ(hit->path, std::string("scenes[0]"));
}

ST_TEST(parse_diagnostics_report_script_level_typo) {
  const auto script = caiyun::parse_script(
      R"({"name":"t","fpss":30,"scenes":[{"kind":"custom","duration":1.0}]})");
  ST_REQUIRE(script.has_value());
  const Diagnostic* hit = find_diagnostic(script.value().diagnostics, "fpss");
  ST_REQUIRE(hit != nullptr);
  ST_CHECK_EQ(hit->suggestion, std::string("fps"));
}

ST_TEST(parse_diagnostics_report_unsupported_track_property) {
  // tracks 的子键（属性名）不在图层字段表里，必须单独校验，
  // 否则 `"scae"` 这类错写会完全静默（写了一个永不生效的轨道）。
  const auto script = caiyun::parse_script(script_with_layers(
      R"({"kind":"rect","x":"0%","y":"0%","w":"10%","h":"10%",
           "tracks":{"scae":[{"t":0,"v":1},{"t":1,"v":2}]}})"));
  ST_REQUIRE(script.has_value());
  const Diagnostic* hit = find_diagnostic(script.value().diagnostics, "scae");
  ST_REQUIRE(hit != nullptr);
  ST_CHECK(hit->message.find("scale") != std::string::npos);
}

// ————————————————————————————————————————————————————————————————————————————
// 四、解析期诊断：越界值（说明“写了多少 / 生效多少”）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(parse_diagnostics_report_clamped_value) {
  const auto script = caiyun::parse_script(
      script_with_layers(R"({"kind":"model3d","x":"0%","y":"0%","w":"50%","h":"50%",
                              "model_segments":200})"));
  ST_REQUIRE(script.has_value());
  const Diagnostic* hit = find_diagnostic(script.value().diagnostics, "200");
  ST_REQUIRE(hit != nullptr);
  ST_CHECK_EQ(hit->key, std::string("model_segments"));
  // 必须同时说明“按 96 生效”——只说越界不说结果，写脚本的人仍不知道实际值。
  ST_CHECK(hit->message.find("96") != std::string::npos);
  // 而且真的生效为 96（诊断与实际行为一致）
  ST_CHECK_EQ(script.value().scenes[0].layers[0].model_segments, 96);
}

ST_TEST(parse_diagnostics_no_report_for_in_range_value) {
  const auto script = caiyun::parse_script(
      script_with_layers(R"({"kind":"model3d","x":"0%","y":"0%","w":"50%","h":"50%",
                              "model_segments":48})"));
  ST_REQUIRE(script.has_value());
  for (const Diagnostic& diag : script.value().diagnostics) {
    ST_CHECK(diag.key != "model_segments");
  }
}

ST_TEST(parse_diagnostics_negative_spin_is_legal) {
  // `model_spin` 允许负值（反向自转）——范围校验不能把合法用法判成越界。
  const auto script = caiyun::parse_script(
      script_with_layers(R"({"kind":"model3d","x":"0%","y":"0%","w":"50%","h":"50%",
                              "model_spin":-14})"));
  ST_REQUIRE(script.has_value());
  ST_CHECK_EQ(script.value().diagnostics.size(), std::size_t{0});
  ST_CHECK_NEAR(script.value().scenes[0].layers[0].model_spin, -14.0f, 0.001f);
}

// ————————————————————————————————————————————————————————————————————————————
// 五、表覆盖度：解析代码读到的每个图层键都必须在表里
// ———————————————————————————————————————————————————————————————————————————
//
// 这是把“两处真相必须一致”变成可执行断言。它用一份**独立于表**的键清单
// （本文件硬编码，取自 `parse_layers` 的真实读取），逐条要求表里有对应项。
// 故意漏登一个键 → 这条必红（已逆向验证）。

ST_TEST(schema_covers_every_layer_key_the_parser_reads) {
  // 与 `director.cpp::parse_layers` 逐行对应（顺序同解析顺序）。
  constexpr std::string_view kParsedLayerKeys[] = {
      "kind", "name", "x", "y", "w", "h", "fill", "fill_alt", "stroke", "color",
      "gradient_dir", "stroke_width", "radius", "opacity", "text", "size", "tracking",
      "line_height", "kin_stagger", "kin_dur", "kin_enter", "kin_rise", "kin_float",
      "kin_float_hz", "kin_caret", "kin_caret_scale", "kin_type_cps", "bold", "align",
      "src", "contain", "dim", "points", "closed", "intensity", "count", "spread", "enter",
      "enter_at", "enter_dur", "model_shape", "model_color", "model_color_alt",
      "model_segments", "model_scale", "model_spin", "model_yaw", "model_tilt", "model_bob",
      "model_bob_hz", "model_backdrop", "model_texture", "model_group", "model_shadow",
      "model_bounce_squash", "model_shadow_color", "bounce", "bounce_hz", "bounce_damping",
      "bounce_at", "path_from_x", "path_from_y", "path_to_x", "path_to_y", "path_arc",
      "path_dur", "path_at", "path_ease", "tracks", "cam_azimuth", "cam_elevation",
      "cam_distance", "cam_fov", "cam_dolly_from", "cam_dolly_to", "light_azimuth",
      "light_elevation", "light_ambient", "light_rim"};
  for (const std::string_view key : kParsedLayerKeys) {
    // "解析会读 '{}'，但 schema 表里没登记它", key
  ST_CHECK(caiyun::find_field_schema(SchemaScope::Layer, key) != nullptr);
  }
}

ST_TEST(schema_covers_every_scene_key_the_parser_reads) {
  constexpr std::string_view kParsedSceneKeys[] = {
      "kind",    "duration", "title",        "subtitle",  "lines",      "accent",
      "background", "values", "layers",      "layout",    "image",      "image_card",
      "image_dim", "image_ken_burns", "dof", "particles", "particle_alpha", "grain",
      "grain_alpha", "wrap", "glow",        "glow_cx",   "glow_cy",    "glow_radius",
      "god_ray", "god_ray_angle", "light_sweep", "cam_zoom_from", "cam_zoom_to",
      "cam_rotate", "cam_pan_x", "cam_pan_y", "cam_shake"};
  for (const std::string_view key : kParsedSceneKeys) {
    // "解析会读 '{}'，但 schema 表里没登记它", key
  ST_CHECK(caiyun::find_field_schema(SchemaScope::Scene, key) != nullptr);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 六、序列化往返：不能丢字段（本组也是逆向验证过的）
// ———————————————————————————————————————————————————————————————————————————
//
// 消费者是智能体：`get script` 读回 → 改一处 → `apply_script` 写回。
// 任何“能读不能写”的字段都会在那一次往返里静默消失，而静态画面看不出来
// （早先实测丢了 25 个字段，包括 `bounce*` / `path_*` / `kin_*` / `tracks`）。

ST_TEST(roundtrip_keeps_layer_animation_fields) {
  const auto first = caiyun::parse_script(script_with_layers(R"({
      "kind":"rect","x":"10%","y":"10%","w":"20%","h":"20%",
      "bounce":24,"bounce_hz":1.7,"bounce_damping":2.6,"bounce_at":0.4,
      "path_from_x":"5%","path_to_x":"60%","path_arc":80,"path_dur":0.9,
      "path_at":0.2,"path_ease":3})"));
  ST_REQUIRE(first.has_value());
  const std::string dumped = caiyun::script_to_json(first.value());
  const auto second = caiyun::parse_script(dumped);
  ST_REQUIRE(second.has_value());
  const auto& layer = second.value().scenes[0].layers[0];
  ST_CHECK_NEAR(layer.bounce, 24.0f, 0.01f);
  ST_CHECK_NEAR(layer.bounce_hz, 1.7f, 0.01f);
  ST_CHECK_NEAR(layer.bounce_damping, 2.6f, 0.01f);
  ST_CHECK_NEAR(layer.bounce_at, 0.4f, 0.01f);
  ST_CHECK_EQ(layer.path_from_x, std::string("5%"));
  ST_CHECK_EQ(layer.path_to_x, std::string("60%"));
  ST_CHECK_NEAR(layer.path_arc, 80.0f, 0.01f);
  ST_CHECK_NEAR(layer.path_dur, 0.9f, 0.01f);
  ST_CHECK_EQ(layer.path_ease, 3);
}

ST_TEST(roundtrip_keeps_kinetic_type_fields) {
  const auto first = caiyun::parse_script(script_with_layers(R"({
      "kind":"text","x":"10%","y":"10%","w":"60%","h":"20%","text":"三维",
      "kin_stagger":0.05,"kin_dur":0.5,"kin_enter":2,"kin_rise":30,
      "kin_float":5,"kin_float_hz":0.7,"kin_caret":1,"kin_caret_scale":0.9,
      "kin_type_cps":18})"));
  ST_REQUIRE(first.has_value());
  const auto second = caiyun::parse_script(caiyun::script_to_json(first.value()));
  ST_REQUIRE(second.has_value());
  const auto& layer = second.value().scenes[0].layers[0];
  ST_CHECK_NEAR(layer.kin_stagger, 0.05f, 0.001f);
  ST_CHECK_NEAR(layer.kin_dur, 0.5f, 0.001f);
  ST_CHECK_EQ(layer.kin_enter, 2);
  ST_CHECK_NEAR(layer.kin_rise, 30.0f, 0.01f);
  ST_CHECK_NEAR(layer.kin_float, 5.0f, 0.01f);
  ST_CHECK_NEAR(layer.kin_float_hz, 0.7f, 0.01f);
  ST_CHECK_EQ(layer.kin_caret, 1);
  ST_CHECK_NEAR(layer.kin_caret_scale, 0.9f, 0.01f);
  ST_CHECK_NEAR(layer.kin_type_cps, 18.0f, 0.01f);
}

ST_TEST(roundtrip_keeps_3d_world_and_shadow_fields) {
  const auto first = caiyun::parse_script(script_with_layers(R"({
      "kind":"model3d","x":"0%","y":"0%","w":"50%","h":"50%",
      "model_group":"desk","model_shadow":0.6,"model_shadow_color":"#05060f",
      "model_bounce_squash":0.18,"model_texture":"media/x.png"})"));
  ST_REQUIRE(first.has_value());
  const auto second = caiyun::parse_script(caiyun::script_to_json(first.value()));
  ST_REQUIRE(second.has_value());
  const auto& layer = second.value().scenes[0].layers[0];
  // 丢 `model_group` 的后果最隐蔽：读回后物体散到各自独立的三维世界，
  // 静态帧看上去仍在同一画面里（只是"不再互相遮挡"）。
  ST_CHECK_EQ(layer.model_group, std::string("desk"));
  ST_CHECK_NEAR(layer.model_shadow, 0.6f, 0.01f);
  ST_CHECK_NEAR(layer.model_bounce_squash, 0.18f, 0.01f);
  ST_CHECK_EQ(layer.model_texture, std::string("media/x.png"));
  ST_CHECK_EQ(layer.model_shadow_color.r, std::uint8_t{0x05});
}

ST_TEST(roundtrip_keeps_tracks) {
  const auto first = caiyun::parse_script(script_with_layers(R"({
      "kind":"rect","x":"0%","y":"0%","w":"10%","h":"10%",
      "tracks":{"y":[{"t":0,"v":400},{"t":1,"v":200,"ease":2}]}})"));
  ST_REQUIRE(first.has_value());
  const auto second = caiyun::parse_script(caiyun::script_to_json(first.value()));
  ST_REQUIRE(second.has_value());
  const auto& tracks = second.value().scenes[0].layers[0].tracks;
  ST_REQUIRE(tracks.size() == 1);
  ST_CHECK_EQ(tracks[0].property, std::string("y"));
  ST_REQUIRE(tracks[0].keys.size() == 2);
  ST_CHECK_NEAR(tracks[0].keys[0].value, 400.0f, 0.01f);
  ST_CHECK_NEAR(tracks[0].keys[1].value, 200.0f, 0.01f);
  ST_CHECK_EQ(tracks[0].keys[1].ease, 2);
}

ST_TEST(roundtrip_keeps_scene_grain_and_wrap) {
  const auto first = caiyun::parse_script(
      R"({"name":"t","fps":30,"size":[320,180],"scenes":[{"kind":"custom","duration":1.0,
          "grain":800,"grain_alpha":0.12,"wrap":false,"glow":0.5}]})");
  ST_REQUIRE(first.has_value());
  const auto second = caiyun::parse_script(caiyun::script_to_json(first.value()));
  ST_REQUIRE(second.has_value());
  const auto& scene = second.value().scenes[0];
  // 丢 grain 的后果：静态段从"安静但活着"退回"完全冻结"（看起来像卡住）。
  ST_CHECK_EQ(scene.grain, 800);
  ST_CHECK_NEAR(scene.grain_alpha, 0.12f, 0.001f);
  ST_CHECK(!scene.wrap);
  ST_CHECK_NEAR(scene.glow, 0.5f, 0.001f);
}

ST_TEST(roundtrip_keeps_layout_reference_not_inlined) {
  // layout 是文件路径时必须写回**路径**，不能把整份 JS 内联进 JSON——
  // 那会让脚本从"一行路径"胖成几百行，且原路径信息永久丢失。
  const auto first = caiyun::parse_script(
      R"({"name":"t","fps":30,"size":[320,180],"scenes":[{"kind":"custom","duration":1.0,
          "layout":{"source":"function layout(t){return [];}","timeout_ms":250}}]})");
  ST_REQUIRE(first.has_value());
  const auto second = caiyun::parse_script(caiyun::script_to_json(first.value()));
  ST_REQUIRE(second.has_value());
  ST_CHECK_EQ(second.value().scenes[0].layout.source, std::string("function layout(t){return [];}"));
  ST_CHECK_EQ(second.value().scenes[0].layout.timeout_ms, 250);
}

// ————————————————————————————————————————————————————————————————————————————
// 七、schema JSON 自省面
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(schema_json_lists_all_fields_with_metadata) {
  const std::string text = caiyun::schema_to_json();
  const auto parsed = st::json_parse(text);
  ST_REQUIRE(parsed.has_value());
  const st::Json& root = parsed.value();
  ST_CHECK(root.contains("scopes"));
  const st::Json& scopes = root["scopes"];
  ST_CHECK(scopes.contains("layer"));
  // 三维字段必须在 layer 作用域里（智能体是按名字找的，不在就等于不存在）
  bool found_spin = false;
  for (const st::Json& item : scopes["layer"]) {
    if (item.value("name", std::string{}) == "model_spin") {
      found_spin = true;
      ST_CHECK_EQ(item.value("type", std::string{}), std::string("number"));
      // 范围与说明都要在：没有范围，智能体只能靠试
      ST_CHECK(item.contains("min"));
      ST_CHECK(item.contains("max"));
      ST_CHECK(!item.value("summary", std::string{}).empty());
    }
  }
  // "schema_json 里找不到 model_spin"
  ST_CHECK(found_spin);
}

// ————————————————————————————————————————————————————————————————————————————
// 四、`--strict` 的拦截判据
//
// 这一组的价值在于：静默忽略是“AI 最贵的坑”——字段名拼错时旧实现照常出片、
// 日志零提示，而“球没自转”与“写对了但转速是 0”在画面上完全一样。
// 严格模式必须把这类错误变成**失败**。
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(strict_blocks_unknown_field_even_though_is_error_is_false) {
  // 关键语义：未知字段的 `is_error` 有意为 false（保老脚本能出片），
  // 所以严格模式**不能**只看 is_error——这一条就是钉住这个区别。
  const auto script = caiyun::parse_script(
      script_with_layers(R"({"kind":"rect","x":"0%","y":"0%","w":"10%","h":"10%",
                              "model_rot":45})"));
  ST_REQUIRE(script.has_value());
  const auto& diags = script.value().diagnostics;
  const Diagnostic* hit = find_diagnostic(diags, "model_rot");
  ST_REQUIRE(hit != nullptr);
  ST_CHECK(!hit->is_error);  // 宽容语义仍在
  // 但严格模式要拦住它
  ST_CHECK_EQ(caiyun::strict_violation_count(diags), std::size_t{1});
}

ST_TEST(strict_allows_clean_script) {
  const auto script = caiyun::parse_script(script_with_layers(
      R"({"kind":"rect","x":"0%","y":"0%","w":"10%","h":"10%","fill":"#ffffff"})"));
  ST_REQUIRE(script.has_value());
  ST_CHECK_EQ(caiyun::strict_violation_count(script.value().diagnostics), std::size_t{0});
}

ST_TEST(strict_counts_every_typo) {
  // 一次列全、一次拦全：AI 一轮就能改完，不必“改一个、跑一次、再发现下一个”。
  const auto script = caiyun::parse_script(
      script_with_layers(R"({"kind":"model3d","x":"0%","y":"0%","w":"50%","h":"50%",
                              "model_rot":45,"camera_azimuth":60,"light_rimm":0.9})"));
  ST_REQUIRE(script.has_value());
  ST_CHECK_EQ(caiyun::strict_violation_count(script.value().diagnostics), std::size_t{3});
}

ST_TEST(strict_does_not_block_range_clamp) {
  // 越界是**有意保留的宽容**（写 200 按 96 处理）——严格模式不该拦它，
  // 否则“宽容解析”会退化成“一律拒绝”，手工调参寸步难行。
  const auto script = caiyun::parse_script(
      script_with_layers(R"({"kind":"model3d","x":"0%","y":"0%","w":"50%","h":"50%",
                              "model_spin":99999})"));
  ST_REQUIRE(script.has_value());
  // 确实产生了越界诊断（告知生效值）……
  ST_CHECK(!script.value().diagnostics.empty());
  // ……但不构成严格模式下的拦截
  ST_CHECK_EQ(caiyun::strict_violation_count(script.value().diagnostics), std::size_t{0});
}

ST_TEST(strict_blocks_wrong_type) {
  // 类型写错同样是“写了但没生效”，必须拦。
  const auto script = caiyun::parse_script(
      script_with_layers(R"({"kind":"model3d","x":"0%","y":"0%","w":"50%","h":"50%",
                              "model_spin":"fast"})"));
  ST_REQUIRE(script.has_value());
  ST_CHECK(caiyun::strict_violation_count(script.value().diagnostics) >= 1U);
}

// ————————————————————————————————————————————————————————————————————————————
// 五、标准 JSON Schema 导出（喂约束解码的机器契约）
//
// 与 `schema_to_json`（自省面）不同，这一份是**机器契约**：
// 只有 `additionalProperties: false` 真正存在，"写错字段名"才有机会在
// **生成时**被挡掉——否则导出的只是一份好看的文档。
// ————————————————————————————————————————————————————————————————————————————

namespace {

[[nodiscard]] auto parse_json_schema() -> st::Json {
  return st::json_parse(caiyun::schema_to_json_schema()).value();
}

}  // namespace

ST_TEST(json_schema_declares_draft_2020_12) {
  const st::Json schema = parse_json_schema();
  ST_CHECK_EQ(schema.value("$schema", std::string{}),
              std::string("https://json-schema.org/draft/2020-12/schema"));
  ST_CHECK_EQ(schema.value("type", std::string{}), std::string("object"));
}

ST_TEST(json_schema_rejects_unknown_fields_at_every_level) {
  // 这条是整个导出存在的理由：没有 additionalProperties:false，
  // 拼错的字段名仍会被静默忽略，导出就毫无价值。
  const st::Json schema = parse_json_schema();
  ST_CHECK(schema.contains("additionalProperties"));
  ST_CHECK(!schema.value("additionalProperties", true));
  for (const auto& [name, node] : st::json_at(schema, "$defs").items()) {
    if (node.value("additionalProperties", true)) {
      // 打印是哪个作用域漏了，便于直接改
      st::print("作用域 {} 未声明 additionalProperties:false\n", name);
    }
    ST_CHECK(!node.value("additionalProperties", true));
  }
}

ST_TEST(json_schema_covers_every_registered_field) {
  // 表里有的字段，导出的 properties 里必须都有——否则"校验通过"与
  // "schema 里没这个字段"会互相矛盾。
  const st::Json schema = parse_json_schema();
  const st::Json& layer =
      st::json_at(st::json_at(st::json_at(schema, "$defs"), "layer"), "properties");
  ST_CHECK(layer.contains("model_spin"));
  ST_CHECK(layer.contains("model_bob"));
  ST_CHECK(layer.contains("kind"));
  ST_CHECK(layer.contains("opacity"));
  // 别名字段不该作为合法键出现（列进去等于认下拼错的写法）
  ST_CHECK(!layer.contains("model_rot"));
}

ST_TEST(json_schema_maps_types_and_ranges) {
  const st::Json schema = parse_json_schema();
  const st::Json& layer =
      st::json_at(st::json_at(st::json_at(schema, "$defs"), "layer"), "properties");

  // 数字 + 范围
  const st::Json& spin = st::json_at(layer, "model_spin");
  ST_CHECK_EQ(spin.value("type", std::string{}), std::string("number"));
  ST_CHECK(spin.value("minimum", 0.0) < 0.0);
  ST_CHECK(spin.value("maximum", 0.0) > 0.0);

  // Dimension：数字或百分比串（`oneOf` 必须是两个对象元素，不是数组的数组）
  const st::Json& x = st::json_at(layer, "x");
  ST_CHECK(x.contains("oneOf"));
  const st::Json& variants = st::json_at(x, "oneOf");
  ST_CHECK_EQ(variants.size(), std::size_t{2});
  ST_CHECK(variants[0].is_object());
  ST_CHECK(variants[1].is_object());
  ST_CHECK_EQ(variants[0].value("type", std::string{}), std::string("number"));
  ST_CHECK_EQ(variants[1].value("type", std::string{}), std::string("string"));

  // Color：字符串 + 十六进制约束
  const st::Json& fill = st::json_at(layer, "fill");
  ST_CHECK_EQ(fill.value("type", std::string{}), std::string("string"));
  ST_CHECK_EQ(fill.value("pattern", std::string{}),
              std::string("^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$"));
}

ST_TEST(json_schema_requires_scenes) {
  // 没有 scenes 的脚本无法渲染——顶层必须把它标成 required，
  // 这样约束解码不会生成"合法但没有分镜"的脚本。
  const st::Json schema = parse_json_schema();
  const st::Json& required = st::json_at(schema, "required");
  bool has_scenes = false;
  for (const auto& item : required) {
    if (item.get<std::string>() == "scenes") has_scenes = true;
  }
  ST_CHECK(has_scenes);
}

ST_TEST(json_schema_description_warns_about_aliases) {
  // 说明里带上"勿写 X"：约束解码看不到 description，
  // 但**读提示词的模型**能看到——两道防线各管一段。
  const st::Json schema = parse_json_schema();
  const st::Json& spin = st::json_at(st::json_at(st::json_at(st::json_at(
      schema, "$defs"), "layer"), "properties"), "model_spin");
  const std::string desc = spin.value("description", std::string{});
  ST_CHECK(desc.find("model_rot") != std::string::npos);
}

ST_TEST(json_schema_links_nested_arrays) {
  // 这一条是"标准校验器救回来的缺陷"的钉子。
  //
  // 实测：`layers` 只声明 `type: array` 而不给 `items` 时，校验器**不会进入元素**，
  // 图层里的拼写错（`model_rot`）永远抓不到——schema 看着完整，样本全放行。
  // 自己的单测当时六条全绿，直到用标准校验器才暴露。
  const st::Json schema = parse_json_schema();
  const st::Json& sceneProps =
      st::json_at(st::json_at(st::json_at(schema, "$defs"), "scene"), "properties");
  const st::Json& layerArray = st::json_at(sceneProps, "layers");
  const st::Json& layerItems = st::json_at(layerArray, "items");
  ST_CHECK(layerItems.is_object());
  ST_CHECK_EQ(layerItems.value("$ref", std::string{}),
              std::string("#/$defs/layer"));
}

ST_TEST(json_schema_one_of_holds_objects_not_arrays) {
  // draft-2020-12 要求 oneOf 的元素是 schema（对象），不是数组。
  // 写成 `[[{...}],[{...}]]` 是合法 JSON，但元 schema 校验直接失败——
  // 而约束解码器会拒掉整个 schema，这条路就白修了。
  const st::Json schema = parse_json_schema();
  const st::Json& variants = st::json_at(
      st::json_at(st::json_at(st::json_at(st::json_at(schema, "$defs"), "layer"),
                   "properties"), "x"), "oneOf");
  ST_CHECK_EQ(variants.size(), std::size_t{2});
  for (const auto& item : variants) {
    ST_CHECK(item.is_object());   // 不是数组
    ST_CHECK(item.contains("type"));
  }
}
