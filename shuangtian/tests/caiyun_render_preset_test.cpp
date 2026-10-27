/// 渲染预设（相机/光照的「预设 + 局部覆盖」）。
///
/// ## 这组用例守的是什么
///
/// 预设的语义只有一条：**本镜最终值 = 预设值 叠加 本镜覆盖值**。
/// 它有三个容易写错又不容易被发现的地方：
///
/// 1. **`null` = 删除**。不写这条规则，"本镜不要补光"就表达不出来
///    （留空 = 用预设的；空数组 = 一个空的补光列表，语义又不同）。
/// 2. **数组整体替换**，不是逐项合并。逐项合并会让"预设两盏灯、我只换第一盏"
///    必须先知道预设里有几盏——那比直接写两盏更麻烦。
/// 3. **预设残留必须是零**。展开后的脚本里不能再有 `preset` 键，
///    否则包换台机器打开时预设找不到，会**静默退回默认值**。
#include <cstdio>
#include <string>
#include <vector>

#include "examples/caiyun/director.hpp"
#include "examples/caiyun/render_preset.hpp"
// 测试目标不链接 examples/caiyun，所以直接包含实现（仓库惯例）。
#include "examples/caiyun/render_preset.cpp"
#include "examples/caiyun/render_preset_table.cpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/test/test.hpp"

namespace {

[[nodiscard]] auto parse(std::string_view text) -> st::Json {
  const auto json = st::json_parse(text);
  if (!json) {
    st::print("测试夹具 JSON 坏了：{}\n", text);
    return st::Json::object();
  }
  return *json;
}

}  // namespace

ST_TEST(caiyun_render_preset_merges_deeply) {
  const st::Json base = parse(R"({
    "key_direction": [-0.4, 0.66, 0.64],
    "ambient": 0.10, "diffuse": 0.52, "rim": 0.26,
    "extra_lights": [{"kind": "point"}, {"kind": "point"}]
  })");
  const st::Json overrides = parse(R"({"ambient": 0.30})");
  const st::Json merged = caiyun::merge_preset(base, overrides);

  // 覆盖的键生效
  ST_CHECK(merged["ambient"].get<double>() > 0.29);
  // 未提及的键**保留预设值**——这是"复用"的全部意义
  ST_CHECK(merged["diffuse"].get<double>() > 0.51);
  ST_CHECK(merged["rim"].get<double>() > 0.25);
  ST_CHECK(merged["key_direction"].is_array());
  // 数组没动过就原样保留
  ST_CHECK(merged["extra_lights"].size() == 2);
}

ST_TEST(caiyun_render_preset_null_deletes) {
  const st::Json base = parse(R"({
    "ambient": 0.10,
    "extra_lights": [{"kind": "point"}, {"kind": "point"}]
  })");
  // `null` = 删除这个键（JSON Merge Patch 语义）
  const st::Json overrides = parse(R"({"extra_lights": null})");
  const st::Json merged = caiyun::merge_preset(base, overrides);

  ST_CHECK(merged.find("extra_lights") == merged.end());
  // 其他键不受影响
  ST_CHECK(merged["ambient"].get<double>() > 0.09);

  // 对照组：空数组 ≠ null。空数组是"一个空的补光列表"，键要留下。
  const st::Json empty_list = caiyun::merge_preset(base, parse(R"({"extra_lights": []})"));
  ST_CHECK(empty_list.find("extra_lights") != empty_list.end());
  ST_CHECK(empty_list["extra_lights"].size() == 0);
}

ST_TEST(caiyun_render_preset_array_replaces_wholesale) {
  const st::Json base = parse(R"({"l": [{"v": 1}, {"v": 2}, {"v": 3}]})");
  const st::Json overrides = parse(R"({"l": [{"v": 9, "n": "x"}]})");
  const st::Json merged = caiyun::merge_preset(base, overrides);

  // 整体替换：长度变成 1，且项内容就是覆盖方给的
  ST_CHECK(merged["l"].size() == 1);
  ST_CHECK(merged["l"][0]["v"].get<double>() > 8.9);
  ST_CHECK(merged["l"][0].find("n") != merged["l"][0].end());
}

ST_TEST(caiyun_render_preset_applies_and_leaves_no_trace) {
  // 端到端：一段带 preset 的 model_script 展开后
  // ① 值来自预设 + 覆盖；② **不残留 preset 键**。
  st::Json script = parse(R"({
    "camera": {"preset": "front"},
    "lighting": {"preset": "studio", "ambient": 0.30, "extra_lights": null},
    "objects": []
  })");
  const auto applied = caiyun::apply_render_presets(
      script, caiyun::builtin_camera_presets(), caiyun::builtin_lighting_presets(),
      "scenes[0].layers[0]");
  ST_CHECK(applied.has_value());
  if (!applied) {
    st::print("展开失败：{}\n", applied.error().message);
    return;
  }

  // 相机：front 预设的 eye 在，preset 键没了
  ST_CHECK(script["camera"].find("preset") == script["camera"].end());
  ST_CHECK(script["camera"]["eye"].is_array());
  ST_CHECK(script["camera"]["fov"].get<double>() > 43.0);

  // 光照：ambient 被覆盖成 0.30，extra_lights 被 null 删掉，preset 键没了
  ST_CHECK(script["lighting"].find("preset") == script["lighting"].end());
  ST_CHECK(script["lighting"]["ambient"].get<double>() > 0.29);
  ST_CHECK(script["lighting"].find("extra_lights") == script["lighting"].end());
  // 未被覆盖的项来自 studio 预设（diffuse 0.52）
  ST_CHECK(script["lighting"]["diffuse"].get<double>() > 0.51);
}

ST_TEST(caiyun_render_preset_unknown_name_is_an_error_with_choices) {
  st::Json script = parse(R"({"camera": {"preset": "no_such_preset"}})");
  const auto applied = caiyun::apply_render_presets(
      script, caiyun::builtin_camera_presets(), caiyun::builtin_lighting_presets(),
      "scenes[2].layers[1]");
  // 必须是错误，不能静默忽略：否则片子照常出，而作者以为用了预设。
  ST_CHECK(!applied.has_value());
  if (applied) return;
  const std::string message = applied.error().message;
  st::print("报错：{}\n", message);
  // 报错要能定位（哪一幕哪一图层）并列出可选项
  ST_CHECK(message.find("scenes[2].layers[1]") != std::string::npos);
  ST_CHECK(message.find("no_such_preset") != std::string::npos);
  ST_CHECK(message.find("front") != std::string::npos);
}

ST_TEST(caiyun_render_preset_package_dir_overrides_builtin) {
  // 包内 `<root>/lighting/x.json` 应当覆盖同名内置预设。
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st_cypkg_presets");
  (void)st::fs::remove_all(dir);
  (void)st::fs::create_directories(st::fs::join(dir, "lighting"));
  (void)st::fs::write_text(st::fs::join(st::fs::join(dir, "lighting"), "studio.json"),
                           R"({"ambient": 0.99, "diffuse": 0.01, "rim": 0.0})");

  caiyun::set_render_presets_from_package(dir);
  const auto& table = caiyun::render_presets();
  const auto* studio = caiyun::find_preset(table.lightings, "studio");
  ST_CHECK(studio != nullptr);
  if (studio != nullptr) {
    ST_CHECK(studio->body["ambient"].get<double>() > 0.98);
    ST_CHECK(studio->origin != "<builtin>");
  }
  // 内置的其他预设仍在（覆盖是**按名**的，不是整表替换）
  ST_CHECK(caiyun::find_preset(table.lightings, "moody") != nullptr);
  ST_CHECK(caiyun::find_preset(table.cameras, "hero") != nullptr);

  // 复原，避免污染同进程的其他用例
  caiyun::set_render_presets_from_package("");
  (void)st::fs::remove_all(dir);
}
