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
  return {"script", "last_action", "last_error", "render_stage", "render_progress",
          "render_output", "render_error", "render_frames_done", "render_frames_total",
          "render_log", "scene_count", "duration", "fps", "size", "name"};
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
  } else {
    last_error_ = std::format("未知动作：{}", action);
    return false;
  }
  mark_dirty();
  return last_error_.empty();
}

}  // namespace caiyun
