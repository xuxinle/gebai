#include <algorithm>
#include <format>

#include "director.hpp"
#include "render_preset.hpp"
#include "st/core/fs.hpp"

namespace caiyun {
namespace {

using st::ErrorCode;

/// 进程内预设表。见头文件“为何是 thread_local”的说明。
thread_local RenderPresetTable g_presets{};

/// 合并两组预设：后者覆盖前者同名项（包内预设可以盖掉内置的）。
[[nodiscard]] auto merge_preset_tables(const std::vector<RenderPreset>& base,
                                       std::vector<RenderPreset> extra)
    -> std::vector<RenderPreset> {
  std::vector<RenderPreset> out = base;
  for (RenderPreset& preset : extra) {
    const auto existing = std::find_if(out.begin(), out.end(), [&](const RenderPreset& other) {
      return other.name == preset.name;
    });
    if (existing != out.end()) {
      *existing = std::move(preset);  // 同名：包内的赢
    } else {
      out.push_back(std::move(preset));
    }
  }
  return out;
}

}  // namespace

auto render_presets() -> RenderPresetTable& {
  if (!g_presets.initialized) {
    // 未显式设置时也要能用：内置预设在任何情况下都可用，
    // 否则“我写了 preset: studio”会因为调用方漏调一次初始化而静默失效。
    g_presets.cameras = builtin_camera_presets();
    g_presets.lightings = builtin_lighting_presets();
    g_presets.initialized = true;
  }
  return g_presets;
}

void set_render_presets_from_package(std::string_view package_root) {
  RenderPresetTable table{};
  table.cameras = builtin_camera_presets();
  table.lightings = builtin_lighting_presets();

  if (!package_root.empty()) {
    // 约定目录：`<包根>/cameras/`、`<包根>/lighting/`。
    // 没写这两个目录不报错——一个包可以只用内置预设。
    table.cameras =
        merge_preset_tables(table.cameras,
                            load_presets_from_dir(st::fs::join(std::string(package_root),
                                                               "cameras")));
    table.lightings =
        merge_preset_tables(table.lightings,
                            load_presets_from_dir(st::fs::join(std::string(package_root),
                                                               "lighting")));
  }
  table.initialized = true;
  g_presets = std::move(table);
}

auto expand_script_presets(Script& script) -> Result<void> {
  const RenderPresetTable& table = render_presets();
  for (std::size_t i = 0; i < script.scenes.size(); ++i) {
    Scene& scene = script.scenes[i];
    for (std::size_t j = 0; j < scene.layers.size(); ++j) {
      Layer& layer = scene.layers[j];
      if (layer.model_script.empty()) continue;
      const auto parsed = st::json_parse(layer.model_script);
      if (!parsed || !parsed->is_object()) continue;
      st::Json expanded = *parsed;
      const std::string where = std::format("scenes[{}].layers[{}]", i, j);
      const auto applied =
          apply_render_presets(expanded, table.cameras, table.lightings, where);
      if (!applied) return st::forward_error(applied.error());
      layer.model_script = expanded.dump();
    }
  }
  return {};
}

}  // namespace caiyun
