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
#include "st/ui/styling.hpp"   // Appearance / apply_appearance（样式系统落位）

namespace st::ui {
namespace {

/// —— 样式系统（`Appearance`）的属性名与落位 ——
///
/// 这五个是**档位**（`raised`/`compact`/`pill`…）而不是数值。落位必须过
/// `Tokens` 解析：抬升要同时给出表面阶/阴影档/顶部高光，尺寸要三联动（高+字+图标），
/// 密度要乘到 padding/gap。自己拼数值就是这层样式系统要消灭的“40 处各自为政”。
[[nodiscard]] auto is_appearance_name(std::string_view name) -> bool {
  return name == "variant" || name == "elevation" || name == "radius_mode" ||
         name == "control_size" || name == "density";
}

/// 把一个属性写进 `Appearance`（只改它自己那个字段，其余保持）。
///
/// 名字与 `Appearance` 字段名一致，**只多了 `radius_mode`/`control_size` 两个**：
/// 那两个字段在结构体里叫 `radius`/`size`（短名在 DSL 里更顺手），而属性面用全名
/// 避免与 `Style::radius`（数值）、`Button::Size`（组件自己的尺寸档）混淆——
/// 两个同名不同义的东西同时出现在 XML/协议里是排查灾难。
[[nodiscard]] auto apply_appearance_field(Appearance& appearance, std::string_view name,
                                          const st::Json& value) -> bool {
  const std::string text = st::json_as_string(value, "");
  if (name == "variant") {
    // **遍历 `variant_name` 反查，不手写值表**（2026-11-10，词表统一）。
    //
    // 为何：手写的值表漏了 `soft`——而 `Soft` 正是按钮的**选中态**（工具栏开关
    // 全用它）。漏掉的后果不是“报错”，而是 `XML 写 variant="soft"` 报“值没被接受”
    // 然后**静默不落位**：相期的「知道了」按钮因此长得像普通按钮。
    // 读写两侧接同一份词表（写在这里、读在 `variant_name`）就不存在漏项。
    constexpr std::array kAll{Variant::Plain,  Variant::Neutral, Variant::Primary,
                              Variant::Accent, Variant::Success, Variant::Warning,
                              Variant::Danger, Variant::Ghost,   Variant::Soft};
    for (const Variant candidate : kAll) {
      if (text == variant_name(candidate)) {
        appearance.variant = candidate;
        break;
      }
    }
    if (!appearance.variant.has_value()) return false;
  } else if (name == "elevation") {
    if (text == "flat") appearance.elevation = Elevation::Flat;
    else if (text == "raised") appearance.elevation = Elevation::Raised;
    else if (text == "overlay") appearance.elevation = Elevation::Overlay;
    else if (text == "modal") appearance.elevation = Elevation::Modal;
    else return false;
  } else if (name == "radius_mode") {
    // 档名用 `sm`/`md`/`lg`/`xl`/`pill`/`none`；**另外接受 `s`/`m`/`l` 短写**，
    // 因为 `radius_mode="md"` 与 `size="md"` 在 XML 里只差前缀，手写时很易把
    // `md` 敲成 `m`——而“值被拒”的报错已经足够把定错方向，不必再添一份碎碎的事
    // 让它静默通过。
    if (text == "none") appearance.radius = RadiusMode::None;
    else if (text == "sm" || text == "s") appearance.radius = RadiusMode::SM;
    else if (text == "md" || text == "m") appearance.radius = RadiusMode::MD;
    else if (text == "lg" || text == "l") appearance.radius = RadiusMode::LG;
    else if (text == "xl") appearance.radius = RadiusMode::XL;
    else if (text == "pill") appearance.radius = RadiusMode::Pill;
    else return false;
  } else if (name == "control_size") {
    if (text == "sm" || text == "small") appearance.size = ControlSize::Small;
    else if (text == "md" || text == "medium") appearance.size = ControlSize::Medium;
    else if (text == "lg" || text == "large") appearance.size = ControlSize::Large;
    else return false;
  } else if (name == "density") {
    if (text == "compact") appearance.density = Density::Compact;
    else if (text == "regular") appearance.density = Density::Regular;
    else if (text == "comfortable") appearance.density = Density::Comfortable;
    else return false;
  } else {
    return false;
  }
  return true;
}

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
      std::string_view{"clip_children"},
      // 叠放（绝对定位）：五个量一起在这张表里——它是“名字认得”的唯一真源
      // （`apply_properties` 用它区分“未知名”与“值不合法”，而落点在上面的
      // `apply_layout_property`）。两处必须同步增删，否则又回到
      // “白名单说支持、实现没做”或反之。
      std::string_view{"absolute"},        std::string_view{"left"},
      std::string_view{"top"},             std::string_view{"right"},
      std::string_view{"bottom"}};
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
  // `effective_visible`：**含祖先链**的“真正看不看得见”。
  //
  // 为何需要两个字段而不是把 `visible` 改成含祖先链：`visible` 是**自身状态**，
  // 它必须能被写回验证（`set visible=false; get visible` → false）——
  // 变成含祖先链后，一个藏在隐藏页里的复选框会读回 false，而它自己并没被设过。
  // 两个词分工明确：`visible` = “我让没让自己显示”，`effective_visible` = “观众看不看得见”。
  //
  // 为何必须有后者（实测踩过）：`find` 会把隐藏页里的元素一并返回，
  // 它们 `visible: true`——调用方（尤其自动化）以为可以点，而实际在屏外/被掩。
  value["effective_visible"] = element.effective_visible();
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
  const auto writable = element.property_names();
  const auto readonly = element.readonly_property_names();
  for (const auto name : writable) {
    if (auto value = element.get_property(name); value.has_value()) {
      // 属性值统一按字符串承载（组件属性面本就是文本协议），但布尔语义要保真
      properties[std::string(name)] = *value;
    }
  }
  for (const auto name : readonly) {
    if (auto value = element.get_property(name); value.has_value()) {
      properties[std::string(name)] = *value;
    }
  }
  // **基类可写但没有名字表声明的项**（`color`/`font_size`/`weight`）。
  //
  // 为何必须在快照里：快照是调用方真正看到的东西（`get` 单发一个名当然能读到，
  // 但快照只遍历名字表）。不补这一轮就是“能读但看不见”——实测确认过：
  // `set color` 成功、`get color` 拿到新值，而快照里完全没有 `color` 这个键。
  // 调用方（尤其自动化）靠快照判断“改成了什么”，缺一个名就是一次静默失明。
  for (const std::string_view name : {"color", "font_size", "weight"}) {
    if (std::ranges::find(writable, name) != writable.end() ||
        std::ranges::find(readonly, name) != readonly.end()) {
      continue;   // 已在名字表里：上面两轮已收，不要重复
    }
    if (auto value = element.get_property(name); value.has_value()) {
      properties[std::string(name)] = *value;
    }
  }
  properties["enabled"] = element.enabled();
  properties["visible"] = element.visible();
  // 含祖先链的“真正看不看得见”（与元素 JSON 里的同名字段一致；见那边注释）。
  properties["effective_visible"] = element.effective_visible();
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
      } else if (text == "content") {
        // 「我在这里读东西」的阅读面（编辑器/终端/预览同语义）：在亮色下归白、
        // 暗色下更沉，由主题决定方向（见 element.hpp 的 Surface::Content 说明）。
        // XML/JS 侧此前写不了这一档，只能拿 base/alt 凑——语义面该能在 XML 里声明。
        element.set_surface(Element::Surface::Content);
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
      // **不能直接写 `style.font_weight`**。它是**派生值**：`Text::apply_theme` /
      // `Button::apply_theme` 每次都用组件自己的 `weight_` 重写它，
      // 于是直接写 `style_` 只活到下一次主题回放（实测：本行写完、下一帧就回 regular，
      // `set` 返回 changed、读回却是 regular、像素一点没变——“报告成功但无效”）。
      //
      // 正确路径是 `Element::set_property`（它走 `set_text_weight`，把值存进
      // `text_weight_override`，由 `apply_text_overrides()` 在主题回放**末尾**重新落位）。
      // 把它从这张布局表里摘掉，名字仍由 `is_base_writable_property` 认得。
      const std::string text = st::json_as_string(value, "");
      if (text == "regular") {
        element.set_text_weight(FontWeight::Regular);
      } else if (text == "medium") {
        element.set_text_weight(FontWeight::Medium);
      } else if (text == "semibold" || text == "semi_bold") {
        element.set_text_weight(FontWeight::SemiBold);
      } else if (text == "bold") {
        element.set_text_weight(FontWeight::Bold);
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
    } else if (name == "absolute") {
      // 叠放（绝对定位）。
      //
      // 为何四个偏移量要**一并**处理：`Style::absolute` 为真后子元素就改按
      // 「父内容盒左上角 + left/top」定位（`right/bottom` 非负时改按右/底缘锤定）。
      // 只落 `absolute` 不落偏移的后果是它叠在左上角——看着像“属性没生效”。
      //
      // 这个缺口当年已经踩过一次：`xml_ui.cpp` 的白名单收录了它（还带着一段
      // “上次漏了它导致图谱覆盖层消失”的注释），但**这张实现表里没有分支**——
      // 于是 XML 写 `absolute="true"` 得到的是“的值没被接受”，把人往“值写错了”
      // 的方向引。白名单收录 ≠ 实现，两者必须同一批改。
      style.absolute = st::json_as_bool(value, style.absolute);
    } else if (name == "left") {
      style.left = as_float();
    } else if (name == "top") {
      style.top = as_float();
    } else if (name == "right") {
      style.right = as_float();
    } else if (name == "bottom") {
      style.bottom = as_float();
    } else if (name == "intercepts") {
      // 输入穿透：`false` = 这一层不接收鼠标，点击落到下层。
      //
      // 为何不能只靠叠放：叠在内容上的覆盖层（文字层、缩略条、拖拽角）
      // 默认会吃掉整块区域的命中，于是“看得见但下面的东西点不动”。
      // 这是 `Element` 级状态（不是 `Style`），因为命中判定在 `UiRoot` 分配事件的路径上。
      element.set_intercepts_input(st::json_as_bool(value, true));
      return true;   // 不改布局，不走下面的 mark_layout_dirty
    } else if (is_appearance_name(name)) {
      // 样式系统（`Appearance`）：五个字段都是**档位语义**，不是具体数值——
      // 落位要过 `Tokens` 解析（抬升→表面+阴影+高光、尺寸→高+字+图标三联动、
      // 密度→padding/gap 倍率），所以不能在这里直接写 `style_`。
      //
      // 走 `apply_appearance` 而不自己拼数值的理由：跨 token 的联动规则只能有
      // 一处（否则同一档位在不同组件上长得不一样——那正是这层存在的原因）。
      Appearance appearance{};
      // 已有声明要**先读回来**：`apply_appearance` 是整体存档，只传一个新字段
      // 会把之前的声明抹掉（“先写 elevation 再写 density，抬升就没了”）。
      if (auto declared = element.appearance(); declared.has_value()) appearance = *declared;
      if (!apply_appearance_field(appearance, name, value)) return false;
      // `apply_appearance` 的返回值是“**样式值变了没**”，不是“**属性收下了没**”。
      // 两者在“声明了一个与当前值相同的档位”时不一致——实测 `radius_mode="none"`
      // 在默认 radius=0 的元素上落位成功但 `changed=false`，拿它当成功判据就会
      // 把这一个合法声明报成“值没被接受”。声明是**状态**（存档供主题重放），
      // 它总是成功的；只有档位名认不出才是失败（上一行已判）。
      (void)element.apply_appearance(appearance);
      return true;
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
        // **也要认基类无条件支持的写入口**（`color`/`font_size`/`weight`）。
        //
        // 为何不能只看 `property_names()`（2026-11-10 实测）：这三项刻意**不随
        // `with_universal` 下发**（否则违反“声明必须可写且填任意值都生效”的契约），
        // 但它们在任何会画字的元素上都应该能 `set`。只看名字表就会变成：
        //   `get color` 能读（回退基类）、快照里没有（名字表不含）、`set` 不生效
        // ——— “能读不能写也不能发现”的第三类分裂（实测确认过）。
        //
        // `is_base_writable` 与 `Element::set_property` 的 `base_accepts`
        // 是**同一份名单**（两处必须同步，否则又回到“认了但落不上”）。
        const bool known =
            std::ranges::find(declared, name) != declared.end() ||
            std::ranges::find(kGenericSettable, std::string_view(name)) != kGenericSettable.end() ||
            Element::is_base_writable_property(name);        if (known) {
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
///
/// ## `force`：对“看不见的元素”装不装
///
/// 默认（`false`）**拒绝对 `effective_visible() == false` 的元素触发点击类动作**。
/// 为何：`find`/`query` 会把隐藏页里的元素一并返回，而它们自己 `visible=true`。
/// 调用方（尤其自动化）拿到 id 就去 invike，而实际东西在屏外/被掩——
/// 结果是“回调执行了、界面没变”，比“找不到元素”难查得多。
///
/// 为何只卡**点击类**：它们语义是“用户在这里点了一下”，而看不见的东西点不到。
/// 其余动作（`animate` / `select` / `toggle` / `stop-animation`…）是**程序化状态变更**，
/// 恰恰有用例需要作用于隐藏元素（先藏后播）。
///
/// 确实要强行点隐藏元素（自动化补测/故意模拟）时传 `force=true`——
/// 它存在，但不该是默认。
[[nodiscard]] auto invoke_element(ui::UiRoot& root, ui::Element& element, std::string_view action,
                                  std::string_view argument, bool force) -> bool {
  if (action == "focus" || action == "blur") {
    // 焦点必须经 UiRoot 设置：键盘事件按 root 的焦点元素派发，
    // 只改元素自身的 focused 标志会导致后续 input.text/input.key 无处可送。
    // 返回值 = 是否真的应用（不可聚焦元素返回 false，调用方据此报未生效）。
    return action == "blur" ? root.set_focus(nullptr) : root.set_focus(&element);
  }
  const bool click_like =
      action == "click" || action == "dblclick" || action == "tripleclick";
  if (click_like && !force && !element.effective_visible()) {
    return false;   // 看不见的东西点不到（调用方应报“未生效”，不要假装成功）
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
