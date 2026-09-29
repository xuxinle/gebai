#include "st/ui/actions.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/selector.hpp"

namespace st::ui {

auto rect_to_json(math::Rect rect) -> st::Json {
  st::Json value = st::Json::object();
  value["x"] = static_cast<double>(rect.x);
  value["y"] = static_cast<double>(rect.y);
  value["width"] = static_cast<double>(rect.width);
  value["height"] = static_cast<double>(rect.height);
  return value;
}

[[nodiscard]] auto element_to_json(ui::Element& element) -> Json {
  Json value = Json::object();
  value["id"] = element.derived_id();
  value["type"] = std::string(element.type());
  value["role"] = std::string(ui::to_string(element.role()));
  value["bounds"] = rect_to_json(element.bounds());
  const std::string text = element.semantics_text();
  if (!text.empty()) value["text"] = text;
  const std::string item_value = element.semantics_value();
  if (!item_value.empty()) value["value"] = item_value;
  return value;
}

/// 元素快照：基本信息（id/type/role/bounds/text/value）+ **属性面**（property_names 逐个读）。
///
/// 控制通道的 `get` 与脚本宿主的 `ui_get` **共用这一份实现**——
/// 否则脚本侧会看不到语言/光标/行数等属性，形成"同一个元素两个样"的认知陷阱
/// （实践中就踩过：脚本里 `ui_get('editor').language` 是 undefined）。
[[nodiscard]] auto element_snapshot(ui::Element& element) -> Json {
  Json snapshot = element_to_json(element);
  Json properties = Json::object();
  for (const auto name : element.property_names()) {
    if (auto value = element.get_property(name); value.has_value()) {
      // 属性值统一按字符串承载（组件属性面本就是文本协议），但布尔语义要保真
      properties[std::string(name)] = *value;
    }
  }
  properties["enabled"] = element.enabled();
  properties["visible"] = element.visible();
  snapshot["props"] = std::move(properties);
  return snapshot;
}

/// 把一批属性应用到元素上，返回实际生效的属性名。
///
/// 控制通道的 `set` 方法与脚本宿主的 `ui.set` **共用这一份实现**——
/// 否则"两条路径能改的属性不一致"会变成难以察觉的语义分裂。
auto apply_properties(ui::UiRoot& root, ui::Element& element, const Json& props) -> Json {
  Json changed = Json::array();
  for (const auto& [name, value] : props.items()) {
    bool applied = false;
    if (name == "enabled") {
      element.set_enabled(st::json_as_bool(value, true));
      applied = true;
    } else if (name == "visible") {
      element.set_visible(st::json_as_bool(value, true));
      applied = true;
    } else if (name == "focused") {
      if (st::json_as_bool(value)) {
        root.set_focus(&element);
      } else if (root.focused() == &element) {
        root.set_focus(nullptr);
      }
      applied = true;
    } else if (name == "checked" || name == "selected" || name == "value" || name == "text" ||
               name == "label" || name == "icon" || name == "options" || name == "active" ||
               name == "scroll_offset") {
      const std::string text = value.is_string() ? st::json_as_string(value) : st::json_dump(value);
      applied = element.set_property(name, text);
    }
    if (applied) {
      changed.push_back(name);
      element.mark_dirty();
    }
  }
  return changed;
}

/// 触发元素动作（控制通道 `invoke` 与脚本宿主 `ui.invoke` 共用）。
[[nodiscard]] auto invoke_element(ui::UiRoot& root, ui::Element& element, std::string_view action,
                                  std::string_view argument) -> bool {
  if (action == "focus" || action == "blur") {
    // 焦点必须经 UiRoot 设置：键盘事件按 root 的焦点元素派发，
    // 只改元素自身的 focused 标志会导致后续 input.text/input.key 无处可送。
    root.set_focus(action == "focus" ? &element : nullptr);
    return true;
  }
  // 先让元素处理动作，**再**通知观察者（与 `UiRoot::dispatch_to` 同序）：
  // 否则脚本写入会被 C++ 处理器随即覆盖，表现为"JS 改了没生效"。
  const bool handled = element.invoke_action(action, argument);
  // 点击类动作：语义就是一次点击——合成事件通知观察者（脚本桥），
  // 让"协议 invoke / 脚本 ui_invoke / 真实鼠标点击"三条路径对事件监听者表现一致。
  if (action == "click" || action == "dblclick" || action == "tripleclick") {
    Event synthetic;
    synthetic.kind = action == "click" ? EventKind::Click
                     : action == "dblclick" ? EventKind::DoubleClick
                                            : EventKind::TripleClick;
    synthetic.position = math::Point{element.bounds().x + element.bounds().width * 0.5f,
                                     element.bounds().y + element.bounds().height * 0.5f};
    synthetic.button = 1;
    synthetic.click_count = action == "click" ? 1 : (action == "dblclick" ? 2 : 3);
    root.notify_event_observer(synthetic, element);
  }
  return handled;
}

auto query_elements(UiRoot& root, std::string_view selector, std::size_t limit)
    -> Result<std::vector<Element*>> {
  auto parsed = Selector::parse(selector);
  if (!parsed) return forward_error(parsed.error());
  return root.query(*parsed, limit);
}

}  // namespace st::ui
