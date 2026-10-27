#include "render_preset.hpp"

#include <algorithm>
#include <format>

#include "st/core/fs.hpp"

namespace caiyun {
namespace {

using st::ErrorCode;

/// `null` 是"删除这个键"的标记（与 JSON Merge Patch 的 RFC 7386 同语义）。
///
/// 采用这个约定而不是自造一套：`null = 删除` 已是广泛使用的做法，
/// 而"留空 = 用预设"与"写空数组 = 空列表"之间的差别必须能表达出来。
[[nodiscard]] auto is_deletion(const st::Json& node) -> bool { return node.is_null(); }

}  // namespace

auto merge_preset(const st::Json& base, const st::Json& override_values) -> st::Json {
  if (!base.is_object() || !override_values.is_object()) {
    // 形状不同：覆盖方胜出。这条让"预设是对象、本镜写标量"这种错配
    // 不至于静默保留预设——宁可给出本镜写的东西（后续 schema 校验会报类型）。
    return override_values;
  }
  st::Json out = base;
  for (auto it = override_values.begin(); it != override_values.end(); ++it) {
    const std::string& key = it.key();
    if (is_deletion(it.value())) {
      out.erase(key);  // null = 删掉预设里的这一项
      continue;
    }
    const auto existing = out.find(key);
    if (existing != out.end() && existing->is_object() && it.value().is_object()) {
      out[key] = merge_preset(*existing, it.value());  // 逐键深合并
    } else {
      out[key] = it.value();  // 数组 / 标量：整体替换
    }
  }
  return out;
}

auto builtin_lighting_presets() -> std::vector<RenderPreset> {
  std::vector<RenderPreset> out;

  // studio：实测过的棚拍打光。总光量 0.88 是过曝分界线
  //（给到 1.25 时环面结 30.6% 像素过曝，0.88 时 0%）。
  out.push_back(RenderPreset{
      .name = "studio",
      .body = st::json_parse(R"({
        "key_direction": [-0.40, 0.66, 0.64],
        "ambient": 0.10, "diffuse": 0.52, "rim": 0.26,
        "exposure": 0.92, "tone_map": "agx",
        "extra_lights": [
          {"kind": "point", "position": [-3.4, 2.0, -1.0],
           "color": [1.0, 0.74, 0.46], "intensity": 1.5, "radius": 9.0},
          {"kind": "point", "position": [3.6, 1.2, 1.6],
           "color": [0.44, 0.68, 1.0], "intensity": 1.3, "radius": 9.0}
        ]
      })")
                   .value_or(st::Json::object()),
      .origin = "<builtin>",
  });

  // moody：低环境 + 强边缘光。适合"暗场里浮出一个物体"的镜头。
  out.push_back(RenderPreset{
      .name = "moody",
      .body = st::json_parse(R"({
        "key_direction": [0.5, 0.35, 0.6],
        "ambient": 0.04, "diffuse": 0.40, "rim": 0.52,
        "exposure": 0.85, "tone_map": "aces",
        "extra_lights": [
          {"kind": "spot", "position": [2.4, 3.0, 1.2],
           "color": [0.55, 0.70, 1.0], "intensity": 1.2, "radius": 8.0,
           "spot_inner_degrees": 22, "spot_outer_degrees": 40}
        ]
      })")
                   .value_or(st::Json::object()),
      .origin = "<builtin>",
  });

  // flat：无方向感平光。界面截图式镜头用它，避免立体感抢戏。
  out.push_back(RenderPreset{
      .name = "flat",
      .body = st::json_parse(R"({
        "key_direction": [0.0, 1.0, 0.15],
        "ambient": 0.55, "diffuse": 0.45, "rim": 0.0,
        "exposure": 1.0, "tone_map": "none",
        "extra_lights": []
      })")
                   .value_or(st::Json::object()),
      .origin = "<builtin>",
  });

  return out;
}

auto builtin_camera_presets() -> std::vector<RenderPreset> {
  std::vector<RenderPreset> out;

  out.push_back(RenderPreset{
      .name = "front",
      .body = st::json_parse(R"({"eye": [0, 0.35, 5.4], "target": [0, 0, 0], "fov": 44})")
                   .value_or(st::Json::object()),
      .origin = "<builtin>",
  });

  // hero：低角仰拍。眼点低于目标点，透视显得物体高大。
  out.push_back(RenderPreset{
      .name = "hero",
      .body = st::json_parse(R"({"eye": [0, -0.55, 4.2], "target": [0, 0.35, 0], "fov": 38})")
                   .value_or(st::Json::object()),
      .origin = "<builtin>",
  });

  // top：俯视。eye 在正上方，up 换 Z 轴（否则 look_at 退化）。
  out.push_back(RenderPreset{
      .name = "top",
      .body = st::json_parse(
                  R"({"eye": [0, 5.0, 0.01], "target": [0, 0, 0], "up": [0, 0, -1], "fov": 40})")
                   .value_or(st::Json::object()),
      .origin = "<builtin>",
  });

  return out;
}

auto find_preset(const std::vector<RenderPreset>& presets, std::string_view name)
    -> const RenderPreset* {
  for (const RenderPreset& preset : presets) {
    if (preset.name == name) return &preset;
  }
  return nullptr;
}

auto load_presets_from_dir(std::string_view dir) -> std::vector<RenderPreset> {
  std::vector<RenderPreset> out;
  if (dir.empty() || !st::fs::is_directory(dir)) return out;
  const auto entries = st::fs::list_dir(dir);
  if (!entries) return out;

  std::vector<std::string> files;
  for (const auto& entry : *entries) {
    if (entry.is_dir) continue;
    if (entry.name.size() > 5 && entry.name.substr(entry.name.size() - 5) == ".json") {
      files.push_back(entry.path);
    }
  }
  std::sort(files.begin(), files.end());

  for (const std::string& file : files) {
    const auto text = st::fs::read_text(file);
    if (!text) continue;
    const auto parsed = st::json_parse(*text);
    if (!parsed || !parsed->is_object()) continue;
    RenderPreset preset{};
    // 预设名 = 文件名去扩展名（`lighting/studio.json` → `studio`）。
    // 不从内容里读 `name`：两个真值源必然漂移，而文件名是人一眼能看到的那个。
    const std::string leaf = st::fs::file_name(file);
    preset.name = leaf.size() > 5 ? leaf.substr(0, leaf.size() - 5) : leaf;
    preset.body = *parsed;
    preset.origin = file;
    out.push_back(std::move(preset));
  }
  return out;
}

auto apply_render_presets(st::Json& model_script,
                          const std::vector<RenderPreset>& camera_presets,
                          const std::vector<RenderPreset>& lighting_presets,
                          std::string_view where) -> Result<void> {
  const auto merge_one = [&](std::string_view key, const std::vector<RenderPreset>& table,
                             std::string_view kind) -> Result<void> {
    const auto node = model_script.find(std::string{key});
    if (node == model_script.end()) return {};  // 没写这一层：不动

    // 两种写法都收：
    //   "camera": {"preset": "hero", "fov": 30}   ← 预设 + 覆盖
    //   "camera": {"eye": [...], ...}             ← 完整写法（当前行为）
    if (!node->is_object()) return {};
    const auto preset_name_node = node->find("preset");
    if (preset_name_node == node->end()) return {};  // 没用预设
    if (!preset_name_node->is_string()) {
      return st::unexpected(
          ErrorCode::Parse,
          std::format("{}.{}.preset：需要字符串（预设名）", where, key));
    }
    const std::string name = preset_name_node->get<std::string>();
    const RenderPreset* preset = find_preset(table, name);
    if (preset == nullptr) {
      std::string available;
      for (const RenderPreset& candidate : table) {
        if (!available.empty()) available += "/";
        available += candidate.name;
      }
      return st::unexpected(
          ErrorCode::Parse,
          std::format("{}.{}.preset：「{}」不存在。可用：{}", where, key, name,
                      available.empty() ? "（无）" : available));
    }

    // 覆盖值 = 本镜写的其他键（去掉 preset 本身）。
    st::Json overrides = st::Json::object();
    for (auto it = node->begin(); it != node->end(); ++it) {
      if (it.key() == "preset") continue;
      overrides[it.key()] = it.value();
    }
    model_script[std::string{key}] = merge_preset(preset->body, overrides);
    (void)kind;
    return {};
  };

  if (const auto cam = merge_one("camera", camera_presets, "相机"); !cam) {
    return st::forward_error(cam.error());
  }
  if (const auto lit = merge_one("lighting", lighting_presets, "光照"); !lit) {
    return st::forward_error(lit.error());
  }
  return {};
}

}  // namespace caiyun
