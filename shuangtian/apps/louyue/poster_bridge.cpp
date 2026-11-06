/// 海报控制面的实现（见 `poster_bridge.hpp`）。

#include "poster_bridge.hpp"

#include <format>
#include <string>

#include "st/math/geometry.hpp"

namespace louyue {

void PosterBridge::measure(const st::ui::RenderContext& context,
                           const st::ui::Constraints& constraints) {
  (void)context;
  (void)constraints;
  // 控制面不占布局：0×0（仍可被控制通道按 id 寻址）。
  measured_ = st::math::Size{0.0f, 0.0f};
}

void PosterBridge::paint_content(const st::ui::RenderContext& context,
                                 st::raster::Surface& canvas) const {
  (void)context;
  (void)canvas;
}

auto PosterBridge::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "poster") return hooks_.poster_json ? hooks_.poster_json() : std::string{};
  if (name == "edit_locked") return hooks_.edit_locked && hooks_.edit_locked() ? "true" : "false";
  if (name == "last_action") return last_action_;
  if (name == "last_error") return last_error_;
  if (name == "export_path") return hooks_.last_export ? hooks_.last_export() : std::string{};
  if (hooks_.poster_value) {
    std::string value = hooks_.poster_value(std::string(name));
    if (!value.empty()) return value;
  }
  return std::nullopt;
}

auto PosterBridge::set_property(std::string_view name, std::string_view value) -> bool {
  // 唯一的可写属性是 `poster`（等价 `invoke apply_poster`）——写属性即换作品，
  // 两条入口给同一个行为（控制通道的 set 是"写数据"，换脚本正是写这份数据）。
  if (name == "poster") {
    last_action_ = "set_poster";
    last_error_ = hooks_.apply_poster ? hooks_.apply_poster(std::string(value)) : "未接线";
    mark_dirty();
    return last_error_.empty();
  }
  return false;
}

auto PosterBridge::property_names() const -> std::vector<std::string_view> {
  return {"poster", "edit_locked", "last_action", "last_error", "export_path",
          "blocks", "layer_count", "size", "export_size", "name"};
}

auto PosterBridge::invoke_action(std::string_view action, std::string_view argument) -> bool {
  last_action_ = std::string(action);
  last_error_.clear();
  if (action == "apply_poster" || action == "set_poster") {
    last_error_ = hooks_.apply_poster ? hooks_.apply_poster(std::string(argument)) : "未接线";
  } else if (action == "load_poster") {
    last_error_ = hooks_.load_poster ? hooks_.load_poster(std::string(argument)) : "未接线";
  } else if (action == "save_poster") {
    last_error_ = hooks_.save_poster ? hooks_.save_poster(std::string(argument)) : "未接线";
  } else if (action == "export_png" || action == "export") {
    last_error_ = hooks_.export_png ? hooks_.export_png(std::string(argument)) : "未接线";
  } else if (action == "edit_lock" || action == "unlock_edit") {
    // 与裁云同口径：`edit_lock` 可带 `off`/`false`/`0` 表示交还。
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

}  // namespace louyue
