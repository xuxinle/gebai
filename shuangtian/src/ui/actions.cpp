#include "st/ui/actions.hpp"

#include <algorithm>
#include <array>
// `<format>`：`std::format`（`element_snapshot` 里格式化悬浮进度）。
// 本工程在 GCC 上不靠其它头间接带入它——Linux 档编译不过就是这么来的。
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/selector.hpp"

#include "st/ext/json.hpp"

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
  // `visible` / `enabled` 必须在**查询结果里**就带上：`find` 会连不可见元素一起返回
  // （选择器语义如此），而调用方（尤其是 AI）拿到一条记录就想去点/改——
  // 不先看看可不可见，就会点到不在屏上的东西（本会话实测因它踩过“点了没反应”）。
  const ui::SemanticsFlags flags = element.semantics_flags();
  value["visible"] = flags.visible;
  value["enabled"] = flags.enabled;
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
  // 核心交互态由**框架统一**提供（不依赖各组件的属性表）：
  // 悬浮是"只能从像素看出来"的状态——若某个组件忘了导出自己的 `hovered`，
  // 自动化验证就只能靠截图猜。这类"看不见"的缺口正是最难查的。
  properties["hovered"] = element.hovered();
  properties["hover_progress"] =
      std::format("{:.3f}", static_cast<double>(element.hover_progress()));
  properties["hover_effect"] = element.hover_effect().enabled;
  // `focused` 与 `enabled`/`visible` 同为**框架级**属性（`set` 也支持它）：
  // 只写不读会让自动化无法验证焦点落在哪——“点一下再看焦点”是最常见的验证动作，
  // 而焦点决定键盘输入的去向（读不到就只剩截图猜）。
  properties["focused"] = element.focused();
  properties["pressed"] = element.pressed();
  snapshot["props"] = std::move(properties);
  return snapshot;
}

/// 把一批属性应用到元素上，返回实际生效的属性名。
///
/// 控制通道的 `set` 方法与脚本宿主的 `ui.set` **共用这一份实现**——
/// 否则"两条路径能改的属性不一致"会变成难以察觉的语义分裂。
///
/// 属性名由**元素自己声明**（`property_names()`）为准，而不是在这里维护一份全局白名单：
/// 两处一旦不一致就会出现"读得到、改不了"的静默分裂——
/// 实测踩到：`Input` 的 `placeholder`（`get` 能读出占位文字，`set` 却返回 `changed: []`
/// 什么也不改，而调用方看到"成功"的返回就以为改好了）。
///
/// `enabled` / `visible` / `focused` 三个是框架级属性（不经 `set_property`，
/// 因为它们需要 `UiRoot` 参与焦点管理），先单独处理。
auto apply_properties(ui::UiRoot& root, ui::Element& element, const Json& props) -> Json {
  Json changed = Json::array();
  // 少数名字在部分元素上未被声明、但历史上一直可写：并集是"既消除分裂又不倒退"的做法。
  constexpr std::array<std::string_view, 9> kGenericSettable{
      "checked", "selected", "value", "text", "label", "icon", "options", "active",
      "scroll_offset"};
  // 布局属性（容器通用，任何元素可设；落到 `Style` 并标脏布局）：声明式 UI 的
  // Row/Column 与手搭代码/协议 `set` 共用同一份布局语义——不另立旁路（不变式 2）。
  const auto apply_layout_property = [&element](const std::string& name,
                                                const st::Json& value) -> bool {
    Style& style = element.style();
    const auto as_float = [&value]() -> float {
      return value.is_number() ? static_cast<float>(value.get<double>())
                               : std::strtof(st::json_as_string(value, "").c_str(), nullptr);
    };
    if (name == "gap") {
      style.gap = as_float();
    } else if (name == "padding") {
      const float inset = as_float();
      style.padding = math::Insets::all(inset);
    } else if (name == "padding_x") {
      const float inset = as_float();
      style.padding.left = inset;
      style.padding.right = inset;
    } else if (name == "padding_y") {
      const float inset = as_float();
      style.padding.top = inset;
      style.padding.bottom = inset;
    } else if (name == "margin") {
      const float inset = as_float();
      style.margin = math::Insets::all(inset);
    } else if (name == "width") {
      style.width = as_float();
    } else if (name == "height") {
      style.height = as_float();
    } else if (name == "grow") {
      style.grow = st::json_as_bool(value, style.grow);
    } else if (name == "radius") {
      style.radius = as_float();
    } else if (name == "direction") {
      style.direction =
          st::json_as_string(value, "") == "row" ? FlexDirection::Row : FlexDirection::Column;
    } else {
      return false;
    }
    element.mark_layout_dirty();
    return true;
  };
  for (const auto& [name, value] : props.items()) {
    bool applied = false;
    if (name == "enabled") {
      element.set_enabled(st::json_as_bool(value, true));
      applied = true;
    } else if (name == "visible") {
      element.set_visible(st::json_as_bool(value, true));
      applied = true;
    } else if (name == "focused") {
      // 接受与否如实回传：不可聚焦元素被 set_focus 拒绝时不能报「已应用」
      if (st::json_as_bool(value)) {
        applied = root.set_focus(&element);
      } else {
        applied = root.focused() != &element || root.set_focus(nullptr);
      }
    } else if (apply_layout_property(name, value)) {
      applied = true;
    } else {
      // 元素自己声明的属性面优先；未声明时回退到一份通用名（见函数注释）。
      const auto declared = element.property_names();
      const bool known =
          std::ranges::find(declared, name) != declared.end() ||
          std::ranges::find(kGenericSettable, std::string_view(name)) != kGenericSettable.end();
      if (known) {
        const std::string text =
            value.is_string() ? st::json_as_string(value) : st::json_dump(value);
        applied = element.set_property(name, text);
      }
    }
    if (applied) {
      changed.push_back(name);
      element.mark_dirty();
    }
  }
  // 变更清单登记（`ui.changed` 事件携带）：只在**真的改到了**属性时登记，
  // 各路径（协议 set / 脚本写入 / 声明式 diff）共用同一入口。
  if (!changed.empty()) root.note_changed(element);
  return changed;
}

/// 触发元素动作（控制通道 `invoke` 与脚本宿主 `ui.invoke` 共用）。
[[nodiscard]] auto invoke_element(ui::UiRoot& root, ui::Element& element, std::string_view action,
                                  std::string_view argument) -> bool {
  if (action == "focus" || action == "blur") {
    // 焦点必须经 UiRoot 设置：键盘事件按 root 的焦点元素派发，
    // 只改元素自身的 focused 标志会导致后续 input.text/input.key 无处可送。
    // 返回值 = 是否真的应用（不可聚焦元素返回 false，调用方据此报未生效）。
    return action == "blur" ? root.set_focus(nullptr) : root.set_focus(&element);
  }
  // 先让元素处理动作，**再**通知观察者（与 `UiRoot::dispatch_to` 同序）：
  // 否则脚本写入会被 C++ 处理器随即覆盖，表现为"JS 改了没生效"。
  const bool handled = element.invoke_action(action, argument);
  if (handled) {
    // 动作是状态变更源（toggle/open/close/select…）：登记进变更清单。
    // 无条件登记（不检查"确实改了"）——元素自己知道得比这里多，误报的代价
    // 只是一条冗余 id（去重过的集合），漏报的代价是通知链断掉。
    root.note_changed(element);
  }
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
