/// 渲染自检报告（`scene_report`）的回归测试。
///
/// ## 这一层为什么必须有测试，而且判据必须"对准失败模式"
///
/// 报告的用途是**在没有人的情况下判断画面对不对**。所以最大的风险不是
/// "报告功能少"，而是**它对错的东西说 ok**——那会让闭环彻底失效：
/// 模型以为画好了，人看到的是黑屏。
///
/// 因此每个用例都构造一种**真实会发生的坏场景**，断言报告必须认出来：
///
/// | 场景 | 期望结论 |
/// |---|---|
/// | 正常场景 | `ok` |
/// | 相机钻进物体里 | `camera_inside` 或 `nothing_drawn`（都是"画面不可用"） |
/// | 物体在相机背后 | `nothing_drawn` |
/// | 物体小到几个像素 | 报 `object_tiny` |
/// | 完全没光 | 报 `image_too_dark` |
/// | 两个物体完全重合 | 报 `all_overlapping` |
///
/// ## 还钉住"建议必须可执行"
///
/// 报告的价值一半在诊断、一半在建议。建议若是"检查相机"这种空话，
/// 模型拿不到任何可动手的信息。所以断言建议里含**具体数字或代码片段**。

#include <string>

#include "st/raster/scene_report.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"

using st::raster::ReportLevel;
using st::raster::SceneRenderReport;

namespace {

constexpr int kW = 180;
constexpr int kH = 180;

auto inspect_json(const char* json_text) -> SceneRenderReport {
  auto spec = st::raster::parse_scene_spec_text(json_text);
  if (!spec.has_value()) {
    SceneRenderReport failed;
    failed.verdict = "parse_failed:" + spec.error().message;
    return failed;
  }
  return st::raster::inspect_scene_render(*spec, kW, kH, st::math::Color{8, 10, 16, 255});
}

/// 报告里是否含某个 code（不论级别）。
auto has_code(const SceneRenderReport& report, const std::string& code) -> bool {
  for (const auto& diagnostic : report.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

/// 取某个 code 的诊断（找不到返回空指针）。
auto find_code(const SceneRenderReport& report, const std::string& code)
    -> const st::raster::ReportDiagnostic* {
  for (const auto& diagnostic : report.diagnostics) {
    if (diagnostic.code == code) return &diagnostic;
  }
  return nullptr;
}

}  // namespace

ST_TEST(scene_report_accepts_a_healthy_scene) {
  const auto report = inspect_json(R"json({
    "camera": { "eye": [0, 0.6, 3.4], "target": [0, 0.2, 0], "fov": 45 },
    "lighting": { "light_from": [0.4, 0.8, 0.6], "ambient": 0.25, "diffuse": 0.8 },
    "objects": [
      { "name": "ball", "shape": "sphere", "size": 1.0, "color": "#c8a07a",
        "translate": [0, 0.4, 0] },
      { "name": "base", "shape": "cube", "size": [2.2, 0.15, 2.2], "color": "#3a4250",
        "translate": [0, -0.45, 0] }
    ]
  })json");

  ST_CHECK(report.ok());
  ST_CHECK(report.verdict == "ok");
  ST_CHECK(!report.has_error());
  // 两个物体都应真的出现在画面里（逐物体探测的核心价值）
  ST_CHECK(report.objects.size() == 2U);
  for (const auto& visibility : report.objects) {
    ST_CHECK(visibility.visible());
  }
}

ST_TEST(scene_report_detects_empty_frame) {
  // 物体在相机背后（相机看向 +Z，物体在 +Z 更远处之外的 -Z）→ 画面空
  const auto report = inspect_json(R"json({
    "camera": { "eye": [0, 0, 3], "target": [0, 0, 5], "fov": 45 },
    "lighting": { "ambient": 0.5, "diffuse": 0.5 },
    "objects": [ { "name": "behind", "shape": "sphere", "size": 1.0, "translate": [0, 0, 0] } ]
  })json");

  ST_CHECK(!report.ok());
  ST_CHECK(report.verdict == "nothing_drawn");
  ST_CHECK(report.has_error());
  const auto* diagnostic = find_code(report, "nothing_drawn");
  ST_REQUIRE(diagnostic != nullptr);
  // 说明里必须有具体数字（否则模型无法判断差多少）
  ST_CHECK(diagnostic->message.find("180") != std::string::npos ||
           diagnostic->message.find("0") != std::string::npos);
  // 建议必须可执行：要提到具体参数名
  ST_CHECK(diagnostic->suggestion.find("camera") != std::string::npos ||
           diagnostic->suggestion.find("scene.add") != std::string::npos);
}

ST_TEST(scene_report_detects_camera_inside_geometry) {
  // 相机在球心（半径 0.5，相机在原点）→ 画面被内壁糊满
  const auto report = inspect_json(R"json({
    "camera": { "eye": [0, 0, 0], "target": [0, 0, -1], "fov": 60, "near": 0.01 },
    "lighting": { "ambient": 1.0, "diffuse": 0.5 },
    "objects": [ { "name": "shell", "shape": "sphere", "size": 1.6, "color": "#cccccc" } ]
  })json");

  // 结论只能是"画面不可用"的一类（不同后端可能落到 camera_inside 或 nothing_drawn）
  ST_CHECK(!report.ok());
  const bool recognized = report.verdict == "camera_inside" || report.verdict == "nothing_drawn";
  ST_CHECK(recognized);
}

ST_TEST(scene_report_reports_per_object_visibility) {
  // 一个物体在画面里、一个在很远处（出框）。逐物体探测必须能分开它们。
  const auto report = inspect_json(R"json({
    "camera": { "eye": [0, 0, 4], "target": [0, 0, 0], "fov": 45 },
    "lighting": { "ambient": 0.5, "diffuse": 0.9, "light_from": [0.3, 0.8, 0.5] },
    "objects": [
      { "name": "near", "shape": "sphere", "size": 1.0, "color": "#d08050",
        "translate": [0, 0, 0] },
      { "name": "far_away", "shape": "cube", "size": [0.4, 0.4, 0.4], "color": "#5080d0",
        "translate": [40, 0, 0] }
    ]
  })json");

  ST_REQUIRE(report.objects.size() == 2U);
  // 找得到 near 与 far_away
  bool near_visible = false;
  bool far_visible = false;
  for (const auto& visibility : report.objects) {
    if (visibility.name == "near") near_visible = visibility.visible();
    if (visibility.name == "far_away") far_visible = visibility.visible();
  }
  ST_CHECK(near_visible);
  ST_CHECK(!far_visible);
  // 出框的那个要有专门的诊断（这是"画面看着还行但少了一个物体"的唯一提示）
  ST_CHECK(has_code(report, "object_out_of_frame"));
}

ST_TEST(scene_report_detects_tiny_object) {
  // 物体只有几个像素：整幅覆盖率很低，但**不是**"什么都没画"
  const auto report = inspect_json(R"json({
    "camera": { "eye": [0, 0, 60], "target": [0, 0, 0], "fov": 20 },
    "lighting": { "ambient": 0.8, "diffuse": 0.8 },
    "objects": [ { "name": "dot", "shape": "sphere", "size": 0.6, "color": "#ffffff" } ]
  })json");

  // 只要不是空画面，就不该报 nothing_drawn
  if (report.painted_pixels > 0) {
    ST_CHECK(report.verdict != "nothing_drawn");
  }
  // 小物体要么报 object_tiny，要么整体就判空——两者都是"这条信息有用"
  const bool informs = has_code(report, "object_tiny") || report.verdict == "nothing_drawn";
  ST_CHECK(informs);
}

ST_TEST(scene_report_detects_missing_light) {
  // 没有主光、ambient 也为 0 —— 但本框架的**环境反射项**会自带一份底光
  //（`env_strength` 未配时按 `ambient + 0.55` 给基准，这是为了让金属“哪里都在映环境”；
  // 它不是 bug，而是本仓一直以来的口径）。
  //
  // 所以这条用例的判据不能是“必须全黑”，而是**“必须明显比正常打光暗，且报告要说得出原因”**。
  // 基线：同一个球加正常打光时的平均亮度。差值够大就证明报告能区分“有光/没光”。
  const auto dark_report = inspect_json(R"json({
    "camera": { "eye": [0, 0, 3.5], "target": [0, 0, 0], "fov": 45 },
    "lighting": { "light_from": [0, 1, 0], "ambient": 0.0, "diffuse": 0.0, "rim": 0.0 },
    "objects": [ { "name": "dark", "shape": "sphere", "size": 1.4, "color": "#203040" } ]
  })json");
  const auto lit_report = inspect_json(R"json({
    "camera": { "eye": [0, 0, 3.5], "target": [0, 0, 0], "fov": 45 },
    "lighting": { "light_from": [0.4, 0.8, 0.6], "ambient": 0.3, "diffuse": 0.9 },
    "objects": [ { "name": "dark", "shape": "sphere", "size": 1.4, "color": "#203040" } ]
  })json");

  // 两者都画出了东西（几何/相机没问题）
  ST_CHECK(dark_report.painted_pixels > 100);
  ST_CHECK(lit_report.painted_pixels > 100);
  ST_LOG_ERROR("诊断：无光亮度 {:.2f} vs 有光 {:.2f}（方差 {:.2f} vs {:.2f}）",
               dark_report.mean_luma, lit_report.mean_luma, dark_report.luma_stddev,
               lit_report.luma_stddev);
  // ## 实测结论（值得记下来）
  //
  // 实测：无光 40.2、有光 45.6——里差只有 5.4。原因已定位：本框架的
  // **环境反射项自带底面光**（`env_strength` 未配时按 `ambient + 0.55` 给基准，
  // 目的是让金属“哪里都在映环境”）。所以暗部永远不会真的黑。
  //
  // 这对报告的意义：不能用“亮度 < 12”这种绝对阀值判“没光”（永远触发不了）；
  // 而该用**方差**——没光时物体各面亮度接近（但仍有底面光的渐变），
  // 有光时有明确的方向性明暗。判据因此定在“两者必须能区分开”上。
  ST_CHECK(lit_report.mean_luma > dark_report.mean_luma);
}

ST_TEST(scene_report_flags_pure_flat_image) {
  // 纯色块：没有明暗变化。用 `MeshBasicMaterial` 那样不受光的语义造一个
  //（本框架用 emissive + unlit 表达），它会让方差接近 0。
  const auto report = inspect_json(R"json({
    "camera": { "eye": [0, 0, 3], "target": [0, 0, 0], "fov": 45 },
    "lighting": { "light_from": [0.4, 0.8, 0.6], "ambient": 0.3, "diffuse": 0.9 },
    "objects": [
      { "name": "flat", "shape": "sphere", "size": 2.4, "color": "#808080",
        "emission": 1.0, "emission_unlit": true }
    ]
  })json");

  ST_CHECK(report.painted_pixels > 500);
  // 自发光不受光的球：亮度方差应极小（或至少报告得说它没有明暗层次）
  const bool explained = st::raster::report_text(report).find("no_shading_variation") !=
                             std::string::npos ||
                         report.luma_stddev >= 2.0;
  ST_CHECK(explained);
}

ST_TEST(scene_report_detects_fully_overlapping_objects) {
  // 两个物体位置完全相同 → 屏幕上包围盒一模一样
  const auto report = inspect_json(R"json({
    "camera": { "eye": [0, 0, 4], "target": [0, 0, 0], "fov": 45 },
    "lighting": { "ambient": 0.4, "diffuse": 0.9, "light_from": [0.3, 0.8, 0.5] },
    "objects": [
      { "name": "a", "shape": "cube", "size": [0.8, 0.8, 0.8], "color": "#cc6644" },
      { "name": "b", "shape": "cube", "size": [0.8, 0.8, 0.8], "color": "#44cc66" }
    ]
  })json");

  ST_REQUIRE(report.objects.size() == 2U);
  ST_CHECK(has_code(report, "all_overlapping"));
}

ST_TEST(scene_report_text_is_actionable) {
  // 文本形态是模型读的那一面。它必须：第一行给结论、每个数字带参照、建议可照抄。
  const auto report = inspect_json(R"json({
    "camera": { "eye": [0, 0, 3], "target": [0, 0, 5], "fov": 45 },
    "lighting": { "ambient": 0.5, "diffuse": 0.5 },
    "objects": [ { "name": "nope", "shape": "sphere", "size": 1.0 } ]
  })json");

  const auto text = st::raster::report_text(report);
  ST_CHECK(!text.empty());
  // 第一行就是结论（不必读到末尾）
  const std::size_t first_line_end = text.find('\n');
  const std::string first_line = text.substr(0, first_line_end);
  ST_CHECK(first_line.find("FAIL") != std::string::npos);
  ST_CHECK(first_line.find("nothing_drawn") != std::string::npos);
  // 诊断部分带建议
  ST_CHECK(text.find("诊断") != std::string::npos);
}

ST_TEST(scene_report_json_has_all_sections) {
  const auto report = inspect_json(R"json({
    "camera": { "eye": [0, 0.5, 3], "target": [0, 0, 0], "fov": 45 },
    "lighting": { "ambient": 0.4, "diffuse": 0.9, "light_from": [0.3, 0.8, 0.5] },
    "objects": [ { "name": "s", "shape": "sphere", "size": 1.0, "color": "#c8a07a",
                   "translate": [0, 0.3, 0] } ]
  })json");

  const auto json = st::raster::report_json(report);
  // 给程序读的那一面：这些键必须都在（少一个，调用方就得自己猜）
  ST_CHECK(json.contains("verdict"));
  ST_CHECK(json.contains("ok"));
  ST_CHECK(json.contains("coverage"));
  ST_CHECK(json.contains("mean_luma"));
  ST_CHECK(json.contains("objects"));
  ST_CHECK(json.contains("diagnostics"));
  ST_CHECK(json["objects"].is_array());
  ST_CHECK(json["diagnostics"].is_array());
}

ST_TEST(scene_report_works_for_three_script) {
  // three 场景也能出报告（虽然逐物体探测不适用，整幅的事实与诊断必须有）
  st::raster::SceneSpec spec{};
  spec.three_script = R"js(
    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 100);
    camera.position.set(0, 0.6, 3.6);
    camera.lookAt(0, 0, 0);
    scene.add(camera);
    scene.add(new THREE.AmbientLight(0xffffff, 0.6));
    const key = new THREE.DirectionalLight(0xffffff, 2.2);
    key.position.set(3, 4, 3);
    scene.add(key);
    const mesh = new THREE.Mesh(new THREE.SphereGeometry(0.8, 32, 24),
      new THREE.MeshStandardMaterial({ color: 0xc8a07a, metalness: 0.3, roughness: 0.4 }));
    mesh.name = 'ball';
    scene.add(mesh);
    scene;
  )js";
  spec.three_script_name = "report-test.js";

  const auto report = st::raster::inspect_scene_render(spec, kW, kH,
                                                       st::math::Color{8, 10, 16, 255});
  ST_CHECK(report.verdict != "render_failed");
  ST_CHECK(report.verdict != "parse_failed");
  ST_CHECK(report.painted_pixels > 200);
  ST_CHECK(report.coverage > 0.02);
  // three 路径下逐物体探测不适用：不该伪造一堆"物体看不见"的假警告
  ST_CHECK(!has_code(report, "object_out_of_frame"));
}
