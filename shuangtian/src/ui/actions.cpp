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
#include "st/ui/svg.hpp"   // parse_color（色值解析：与 DSL 的 hex_color 同一份）

#include "st/core/json.hpp"

namespace st::ui {
namespace {

/// 对齐枚举的名字 → 值（与 XML 属性面**同一份口径**：都用名字，不靠数字）。
[[nodiscard]] auto parse_align(const std::string& text, Align& out) -> bool {
  if (text == "start") {
    out = Align::Start;
  } else if (text == "center") {
    out = Align::Center;
  } else if (text == "end") {
    out = Align::End;
  } else if (text == "stretch") {
    out = Align::Stretch;
  } else {
    return false;
  }
  return true;
}

/// 判断属性名是否属于布局/样式面（`apply_layout_property` 的分支清单）。
///
/// 用途：`set` 被拒项的拒因分类——布局名字若被拒，只能是**值不合法**
/// （枚举拼错/颜色解析失败），不能归为「未知名」。与 `apply_layout_property`
/// 的分支清单**同步维护**（新增布局属性时两处都要加）。
[[nodiscard]] auto is_layout_property_name(std::string_view name) -> bool {
  constexpr std::array kLayoutNames{
      std::string_view{"gap"},          std::string_view{"padding"},
      std::string_view{"padding_x"},    std::string_view{"padding_y"},
      std::string_view{"margin"},       std::string_view{"width"},
      std::string_view{"height"},       std::string_view{"grow"},
      std::string_view{"radius"},       std::string_view{"direction"},
      std::string_view{"min_width"},    std::string_view{"min_height"},
      std::string_view{"max_width"},    std::string_view{"max_height"},
      std::string_view{"shrink"},       std::string_view{"wrap"},
      std::string_view{"align_items"},  std::string_view{"align_self"},
      std::string_view{"justify"},      std::string_view{"surface"},
      std::string_view{"background"},   std::string_view{"text_color"},
      std::string_view{"text_size"},    std::string_view{"text_align"},
      std::string_view{"font_size"},    std::string_view{"font_weight"},
      std::string_view{"monospace"},    std::string_view{"border_width"},
      std::string_view{"border_color"}, std::string_view{"opacity"},
      std::string_view{"clip_children"}};
  return std::ranges::find(kLayoutNames, name) != kLayoutNames.end();
}

}  // namespace

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
  // 可读写属性 + 只读属性都进快照（`get` 应能读到只读值，如 `selection`）。
  for (const auto name : element.property_names()) {
    if (auto value = element.get_property(name); value.has_value()) {
      // 属性值统一按字符串承载（组件属性面本就是文本协议），但布尔语义要保真
      properties[std::string(name)] = *value;
    }
  }
  for (const auto name : element.readonly_property_names()) {
    if (auto value = element.get_property(name); value.has_value()) {
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

/// 把一批属性应用到元素上，返回实际生效的属性名；
/// `rejected` 非空时同步收集**被拒项**（名字+拒因，与 changed 对偶）。
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
auto apply_properties(ui::UiRoot& root, ui::Element& element, const Json& props,
                      std::vector<RejectedProperty>* rejected) -> Json {
  // 被拒项登记（带拒因分类，BACKLOG 4022 方向①）：
  //  `rejected` 为 nullptr（旧调用方）时静默不收，行为与旧版完全一致。
  const auto note_rejected = [rejected](std::string_view name, RejectReason reason) {
    if (rejected != nullptr) rejected->push_back(RejectedProperty{std::string(name), reason});
  };
  Json changed = Json::array();
  // 少数名字在部分元素上未被声明、但历史上一直可写：并集是"既消除分裂又不倒退"的做法。
  constexpr std::array<std::string_view, 9> kGenericSettable{
      "checked", "selected", "value", "text", "label", "icon", "options", "active",
      "scroll"};
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
    } else if (name == "min_width") {
      style.min_width = as_float();
    } else if (name == "min_height") {
      style.min_height = as_float();
    } else if (name == "max_width") {
      style.max_width = as_float();
    } else if (name == "max_height") {
      style.max_height = as_float();
    } else if (name == "shrink") {
      style.shrink = st::json_as_bool(value, style.shrink);
    } else if (name == "wrap") {
      style.wrap = st::json_as_bool(value, style.wrap);
    } else if (name == "align_items") {
      // 枚举统一走**名字**（与 XML 属性面同一口径，不靠数字）。
      if (!parse_align(st::json_as_string(value, ""), style.align_items)) return false;
    } else if (name == "align_self") {
      if (!parse_align(st::json_as_string(value, ""), style.align_self)) return false;
    } else if (name == "justify") {
      const std::string text = st::json_as_string(value, "");
      if (text == "start") {
        style.justify = Justify::Start;
      } else if (text == "center") {
        style.justify = Justify::Center;
      } else if (text == "end") {
        style.justify = Justify::End;
      } else if (text == "space_between") {
        style.justify = Justify::SpaceBetween;
      } else if (text == "space_around") {
        style.justify = Justify::SpaceAround;
      } else {
        return false;
      }
    } else if (name == "surface") {
      // 语义表面档位在 **Element** 上（不在 Style），且是“下一帧 apply_theme 生效”。
      const std::string text = st::json_as_string(value, "");
      if (text == "none") {
        element.set_surface(Element::Surface::None);
      } else if (text == "base") {
        element.set_surface(Element::Surface::Base);
      } else if (text == "alt") {
        element.set_surface(Element::Surface::Alt);
      } else if (text == "sunken") {
        element.set_surface(Element::Surface::Sunken);
      } else {
        return false;
      }
    } else if (name == "background") {
      // 允许 `#rgb`/`#rrggbb`/`#rrggbbaa` 与 CSS 基本色名（与 DSL 的 `hex_color` 同一解析）。
      const auto parsed = svg::parse_color(st::json_as_string(value, ""));
      if (!parsed.has_value()) return false;
      style.background = *parsed;
    } else if (name == "text_color") {
      // 文字色走**显式覆盖**（`set_text_color`）：直接写 `style.color` 会被
      // 下一帧的主题刷新盖回（与 DSL 里 `hex_color` 同一机制）。
      const auto parsed = svg::parse_color(st::json_as_string(value, ""));
      if (!parsed.has_value()) return false;
      element.set_text_color(*parsed);
    } else if (name == "text_size") {
      element.set_text_size(as_float());
    } else if (name == "text_align") {
      switch (static_cast<int>(as_float())) {
        case 0: style.text_align = TextAlign::Start; break;
        case 1: style.text_align = TextAlign::Center; break;
        case 2: style.text_align = TextAlign::End; break;
        default: return false;
      }
    } else if (name == "font_size") {
      style.font_size = as_float();
    } else if (name == "font_weight") {
      const std::string text = st::json_as_string(value, "");
      if (text == "regular") {
        style.font_weight = FontWeight::Regular;
      } else if (text == "medium") {
        style.font_weight = FontWeight::Medium;
      } else if (text == "semibold" || text == "semi_bold") {
        style.font_weight = FontWeight::SemiBold;
      } else if (text == "bold") {
        style.font_weight = FontWeight::Bold;
      } else {
        return false;
      }
    } else if (name == "monospace") {
      style.monospace = st::json_as_bool(value, style.monospace);
    } else if (name == "border_width") {
      style.border_width = as_float();
    } else if (name == "border_color") {
      const auto parsed = svg::parse_color(st::json_as_string(value, ""));
      if (!parsed.has_value()) return false;
      style.border_color = *parsed;
    } else if (name == "opacity") {
      style.opacity = as_float();
    } else if (name == "clip_children") {
      style.clip_children = st::json_as_bool(value, style.clip_children);
    } else {
      return false;
    }
    element.mark_layout_dirty();
    return true;
  };
  for (const auto& [name, value] : props.items()) {
    bool applied = false;
    // 未进入任何可写面时才登记未知名；一旦进入任一可写面（名字是认得的），
    // 后续若被拒只能是「值不合法」或「能力未开」——两种拒因下面分别登记。
    bool known_name = false;
    if (name == "enabled") {
      element.set_enabled(st::json_as_bool(value, true));
      applied = true;
    } else if (name == "id") {
      // `id` / `key` 是**元素元数据**（不在 `property_names()` 里，因为它们是身份
      // 而非内容），但“外部按 id 寻址”正是协议/脚本/XML 共用的前提——
      // 不能设 id 就意味着“建完就找不回来”。所以它们属框架级可写属性。
      element.set_id(st::json_as_string(value));
      applied = true;
    } else if (name == "key") {
      element.set_key(st::json_as_string(value));
      applied = true;
    } else if (name == "visible") {
      element.set_visible(st::json_as_bool(value, true));
      applied = true;
    } else if (name == "focused") {
      known_name = true;   // 框架级属性：名字一定认得，拒绝只能是能力未开
      // 接受与否如实回传：不可聚焦元素被 set_focus 拒绝时不能报「已应用」
      if (st::json_as_bool(value)) {
        applied = root.set_focus(&element);
      } else {
        applied = root.focused() != &element || root.set_focus(nullptr);
      }
    } else {
      // 布局/样式属性先试（名字在 apply_layout_property 的分支里）。
      if (apply_layout_property(name, value)) {
        applied = true;
      } else {
        // 元素自己声明的属性面优先；未声明时回退到一份通用名（见函数注释）。
        const auto declared = element.property_names();
        const bool known =
            std::ranges::find(declared, name) != declared.end() ||
            std::ranges::find(kGenericSettable, std::string_view(name)) != kGenericSettable.end();
        if (known) {
          known_name = true;   // 名字认得：若拒绝则是值不合法（元素/枚举/颜色解析拒了）
          const std::string text =
              value.is_string() ? st::json_as_string(value) : st::json_dump(value);
          applied = element.set_property(name, text);
        }
      }
    }
    if (applied) {
      changed.push_back(name);
      element.mark_dirty();
    } else {
      // 与 changed 对偶的拒绝面（BACKLOG 4022 方向①）：被拒项按拒因分类回传，
      // 调用方不必 diff changed 才能发现「拼错名字/值」——那曾是 e2e 静默跳断言点的
      // 成因。拒因分三类：未知名（名字不认得）/ 无效值（名字认得但值被拒）/
      // 未开能力（focused=true 但元素不可聚焦）。
      const bool layout_name = is_layout_property_name(name);
      note_rejected(name,
                    name == "focused"           ? RejectReason::Unfocusable
                    : known_name || layout_name ? RejectReason::InvalidValue
                                                : RejectReason::UnknownName);
    }
  }
  // 变更清单登记（`ui.changed` 事件携带）：只在**真的改到了**属性时登记，
  // 各路径（协议 set / 脚本写入 / 声明式 diff）共用同一入口。
  if (!changed.empty()) root.note_changed(element);
  return changed;
}

auto apply_properties(ui::UiRoot& root, ui::Element& element, const Json& props) -> Json {
  // 旧签名（无拒绝收集）：既有调用方（脚本桥/XML/声明式/单元测试）零改动，
  // 行为与收集版完全一致（不收被拒项，只回 changed）。
  return apply_properties(root, element, props, nullptr);
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
