/// AI 控制通道桥的实现（见 `director_bridge.hpp`）。

#include "director_bridge.hpp"

#include <format>
#include <string>
#include <string_view>

#include "st/math/geometry.hpp"

namespace caiyun {

void DirectorBridge::measure(const st::ui::RenderContext& context,
                             const st::ui::Constraints& constraints) {
  (void)context;
  (void)constraints;
  // 控制面不占布局：0×0（仍参与命中与寻址——控制通道按 id 找元素，不看尺寸）。
  measured_ = st::math::Size{0.0f, 0.0f};
}

void DirectorBridge::paint_content(const st::ui::RenderContext& context,
                                   st::raster::Surface& canvas) const {
  (void)context;
  (void)canvas;
}

auto DirectorBridge::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "script") return hooks_.script_json ? hooks_.script_json() : std::string{};
  // ——— 自省面（智能体不读源码也能知道全部字段）———
  //
  // `schema_json` 返回字段总表（名/类型/范围/说明）；`schema_diagnostics` 返回
  // 上一次脚本解析产生的诊断（未知字段/越界）；`diagnostic_count` 是它的条数
  // （供“一秒判断这次改动有没有写错字段”的快速断言）。
  if (name == "schema_json") return schema_to_json();
  if (name == "json_schema") return schema_to_json_schema();
  if (name == "schema_diagnostics") {
    return hooks_.schema_diagnostics ? hooks_.schema_diagnostics() : std::string{};
  }
  if (name == "diagnostic_count") {
    return hooks_.diagnostic_count ? hooks_.diagnostic_count() : std::string{};
  }
  if (name == "edit_locked") {
    return hooks_.edit_locked && hooks_.edit_locked() ? "true" : "false";
  }
  if (name == "last_action") return last_action_;
  if (name == "last_error") return last_error_;
  if (name.starts_with("render_")) {
    const std::string key(name.substr(7));
    if (hooks_.render_value) return hooks_.render_value(key);
    return std::string{};
  }
  if (hooks_.project_value) {
    std::string value = hooks_.project_value(std::string(name));
    if (!value.empty()) return value;
  }
  return std::nullopt;
}

auto DirectorBridge::set_property(std::string_view name, std::string_view value) -> bool {
  // 唯一的可写属性是 `script`（等价 `invoke apply_script <json>`）——控制通道的
  // `set` 是"写属性"，而换脚本正是写这份数据，两条入口给同一个行为。
  if (name == "script") {
    last_action_ = "set_script";
    last_error_ = hooks_.apply_script ? hooks_.apply_script(std::string(value)) : "未接线";
    mark_dirty();
    return last_error_.empty();
  }
  return false;
}

auto DirectorBridge::property_names() const -> std::vector<std::string_view> {
  return {"script", "last_action", "last_error", "edit_locked", "render_stage", "render_progress",
          "render_output", "render_error", "render_frames_done", "render_frames_total",
          "render_log", "render_elapsed",
          // 画面质量（渲完帧后可用）：summary 给人看，quality_json 给程序取，
          // 其余是拆开的单值——判据要能直接进断言，不必解析 JSON。
          "render_quality", "render_quality_json", "render_quality_still_ratio",
          "render_quality_bright_ratio", "render_quality_mean_luma",
          "render_quality_change_density", "render_quality_worst_still_clip",
          "render_quality_static_threshold", "render_quality_partial",
          // 字段自省与诊断（AI 原生的核心面，见 `schema.hpp`）
          "schema_json", "schema_diagnostics", "diagnostic_count",
          // **标准 JSON Schema**（draft-2020-12）：喂给约束解码，让模型在生成时
          // 就写不出错误字段名——比事后校验更早一层。
          "json_schema",
          "scene_count", "duration", "fps", "size", "name"};
}

auto DirectorBridge::invoke_action(std::string_view action, std::string_view argument) -> bool {
  last_action_ = std::string(action);
  last_error_.clear();
  if (action == "apply_script") {
    last_error_ = hooks_.apply_script ? hooks_.apply_script(std::string(argument)) : "未接线";
  } else if (action == "set_script") {
    last_error_ = hooks_.apply_script ? hooks_.apply_script(std::string(argument)) : "未接线";
  } else if (action == "load_script") {
    last_error_ = hooks_.load_script ? hooks_.load_script(std::string(argument)) : "未接线";
  } else if (action == "save_script") {
    last_error_ = hooks_.save_script ? hooks_.save_script(std::string(argument)) : "未接线";
  } else if (action == "render") {
    last_error_ = hooks_.start_render ? hooks_.start_render(std::string(argument)) : "未接线";
  } else if (action == "render_stop") {
    if (hooks_.stop_render) hooks_.stop_render();
  } else if (action == "edit_lock" || action == "unlock_edit") {
    // AI 接管/交还编辑权。`edit_lock` 可带参数（`on`/`off`/`false`/`0`），
    // 不带则视为开启——“交给 AI”本来就是默认意图。
    bool on = action == "edit_lock";
    if (action == "edit_lock" && !argument.empty()) {
      on = !(argument == "off" || argument == "false" || argument == "0");
    }
    last_error_ = hooks_.set_edit_lock ? hooks_.set_edit_lock(on) : "未接线";
  } else {
    last_error_ = std::format("未知动作：{}", action);
    return false;
  }
  mark_dirty();
  return last_error_.empty();
}

}  // namespace caiyun
