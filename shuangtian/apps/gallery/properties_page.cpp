/// 「属性面」检视器页：由 `PropertyDesc` 自动生成面板，改动落到真实元素上。
///
/// ## 这一页在质检什么
///
/// 框架有三张表在描述"这个组件有哪些可调参数"：
///
/// | 表 | 回答什么 | 消费者 |
/// |---|---|---|
/// | `property_names()` | 哪些名字**可写** | 控制通道 `set` |
/// | `readonly_property_names()` | 哪些名字**只读** | 控制通道 `get` |
/// | `property_schema()` | 类型/范围/步进/候选/说明 | **自动生成面板**（`build_property_panel`） |
///
/// 三张表**同时存在**就必然有漂移：描述了却没进名字表（面板有控件、`set` 不认）、
/// 名字表里有却没描述（能设但面板上没有）、两张名字表重叠（声明可写但设了不生效）。
/// 这类缺陷在界面上只表现为"这个控件好像没什么用"——最难查的一类。
///
/// 所以这一页的价值不在"有个面板"，而在于**它把三张表摆在一起**：
/// 左边是自动生成的可调控件（描述驱动），右边是两张名字表的原文。
/// 任何一处对不上，人一眼就能看出来，`audit.cpp` 的 `check_property_surface` 也会 FAIL。
///
/// ## 为什么用"真元素"而不是造一个假对象
///
/// 面板的读写都打到**树上的真实组件**（`target` 是 `UiRoot::find` 找到的那个），
/// 所以"拖滑块 → 元素真的变了 → 组件重绘"是一条完整闭环。造个假对象只能验面板本身。

#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pages.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/button.hpp"
#include "st/ui/components/card.hpp"
#include "st/ui/components/heading.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/components/progress_bar.hpp"
#include "st/ui/components/segmented_control.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/components/switch.hpp"
#include "st/ui/components/text.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"
#include "st/ui/property_panel.hpp"
#include "st/ui/property_schema.hpp"

namespace gallery {
namespace {

using st::math::Insets;
using st::ui::Button;
using st::ui::Card;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::Heading;
using st::ui::Panel;
using st::ui::Text;
using st::ui::Tone;

/// 可检视的靶子：一组"属性面完整度不同"的真实组件。
///
/// 为什么每个靶子都要**单独建实例**而不是直接看页面里已有的控件：
/// 检视器会真的改它的属性（滑块拖动、开关切换），拿页面既有控件当靶子会把
/// 那一页的演示状态改乱。这些靶子只属于本页。
struct ProbeSpec {
  const char* id;
  const char* label;
  const char* note;
};

inline constexpr ProbeSpec kProbes[] = {
    {"probe-slider", "Slider（滑块）", "value / label / step"},
    {"probe-switch", "Switch（开关）", "checked / label"},
    {"probe-progress", "ProgressBar（进度条）", "value / tone"},
    {"probe-segmented", "SegmentedControl（分段）", "index / options"},
    {"probe-button", "Button（按钮）", "label / variant / icon"},
};

/// 建一个靶子组件（按 id 造对应类型）。
[[nodiscard]] auto make_probe(std::string_view id) -> std::unique_ptr<Element> {
  if (id == "probe-slider") {
    auto slider = std::make_unique<st::ui::Slider>(0.62f);
    slider->set_label("检视器滑块");
    slider->style().width = 260.0f;
    slider->set_id(std::string(id));
    return slider;
  }
  if (id == "probe-switch") {
    auto toggle = std::make_unique<st::ui::Switch>("检视器开关");
    toggle->set_checked(true);
    toggle->set_id(std::string(id));
    return toggle;
  }
  if (id == "probe-progress") {
    auto progress = std::make_unique<st::ui::ProgressBar>(0.45f);
    progress->set_id(std::string(id));
    progress->style().width = 260.0f;
    return progress;
  }
  if (id == "probe-segmented") {
    auto segmented = std::make_unique<st::ui::SegmentedControl>();
    segmented->set_options({"紧凑", "标准", "宽松"});
    segmented->set_index(1);
    segmented->set_id(std::string(id));
    return segmented;
  }
  auto button = std::make_unique<Button>("检视器按钮", Button::Variant::Primary,
                                        Button::Size::Small);
  button->set_id(std::string(id));
  return button;
}


/// 属性检视器的声明式实现（`mount_into` 子树；宿主见本页的 `property-host`）。
///
/// 声明式在这里不是为了好看：面板要**随选中项变化重建**（换靶子 → 换描述表 → 换控件），
/// 手搭就得自己 remove/clear + 重建整棵子树；声明式按 key 对齐，换靶子只重建差异部分。
struct PropertyInspector : st::ui::dsl::Component {
  using Component::Component;

  /// 当前选中的靶子 id（`State`：改它 → 下一帧重组）。
  st::ui::dsl::State<std::string> selected{std::string(kProbes[0].id)};
  /// 最近一次写入的结果（"value=0.62 已生效" / "被拒绝：unknown_name"）。
  st::ui::dsl::State<std::string> last_write{};
  /// 写到真实元素上的入口（由页面注入：`UiRoot::find` + `set_property`）。
  std::function<Element*(std::string_view)> find_target{};
  /// 读真实元素的属性（`get_property`）。
  std::function<std::optional<std::string>(std::string_view, std::string_view)> read_property{};
  /// 写真实元素的属性（返回是否生效）。
  std::function<bool(std::string_view, std::string_view, std::string_view)> write_property{};

  void build(st::ui::dsl::Composer& c) override {
    using namespace st::ui::dsl;

    // 靶子选择器（分段控件：一次只有 5 个，用不着下拉）
    (void)row(c, {.gap = 8.0f, .key = "probe-picker"}, [&] {
      (void)text(c, [] { return std::string("检视目标"); }, {.size = 12.0f, .key = "pick-label"});
      std::size_t active = 0;
      for (std::size_t index = 0; index < std::size(kProbes); ++index) {
        if (selected.value() == kProbes[index].id) active = index;
      }
      (void)custom<st::ui::SegmentedControl>(
          c,
          [&, active](st::ui::SegmentedControl& control) {
            control.set_id("probe-picker");
            control.set_options({"滑块", "开关", "进度", "分段", "按钮"});
            control.set_index(static_cast<int>(active), false);
            control.on_change = [this](int index) {
              if (index >= 0 && static_cast<std::size_t>(index) < std::size(kProbes)) {
                selected.set(std::string(kProbes[static_cast<std::size_t>(index)].id));
              }
            };
          },
          {.key = "picker"});
    });

    // 找到当前靶子的真实元素（不持有指针跨帧：重组后指针可能换人）
    const std::string target_id = selected.value();
    Element* target = find_target ? find_target(target_id) : nullptr;
    if (target == nullptr) {
      (void)text(c, [target_id] { return "找不到靶子元素：" + target_id; },
                 {.color = Tone::Danger, .key = "missing"});
      return;
    }

    // —— 左：描述驱动的自动面板 ——
    (void)text(c, [&] { return std::format("{} 的属性描述（{} 项）", target_id,
                                           target->property_schema().size()); },
               {.size = 13.0f, .weight = st::ui::FontWeight::SemiBold, .key = "schema-title"});

    const std::span<const st::ui::PropertyDesc> descs = target->property_schema();
    if (descs.empty()) {
      (void)card(c, {.gap = 6.0f, .padding = 12.0f, .key = "no-schema"}, [&] {
        (void)text(c, [] {
          return std::string(
              "这个组件没有声明 property_schema()，所以生成不了自动面板。\n"
              "这不是缺陷（描述是可选覆写），但意味着它无法被数据驱动地调参——"
              "右侧的两张名字表仍然可以读写。");
        }, {.color = Tone::Muted, .size = 12.0f, .key = "no-schema-text"});
      });
    } else {
      st::ui::PropertyPanelHooks hooks{};
      hooks.id_prefix = "inspect";
      hooks.read = [this, target_id](std::string_view name) -> std::optional<std::string> {
        return read_property ? read_property(target_id, name) : std::nullopt;
      };
      hooks.write = [this, target_id](std::string_view name, std::string_view value) -> bool {
        const bool ok = write_property ? write_property(target_id, name, value) : false;
        last_write.set(std::format("{} = {} → {}", name, value, ok ? "已生效" : "被拒绝"));
        return ok;
      };
      (void)card(c, {.gap = 10.0f, .padding = 14.0f, .key = "panel"}, [&] {
        (void)st::ui::build_property_panel(c, descs, hooks,
                                          {.show_advanced = true,
                                           .show_readonly = true,
                                           .editable = true,
                                           .label_width = 96.0f});
      });
    }

    if (!last_write.value().empty()) {
      (void)text(c, [&] { return "最近一次写入：" + last_write.value(); },
                 {.color = Tone::Muted, .size = 12.0f, .key = "last-write"});
    }

    // —— 右：两张名字表的原文（"能设但面板没有"与"面板有但设不动"的判据）——
    (void)text(c, [] { return std::string("属性名表（控制通道 get/set 的口径）"); },
               {.size = 13.0f, .weight = st::ui::FontWeight::SemiBold, .key = "names-title"});
    (void)card(c, {.gap = 6.0f, .padding = 12.0f, .key = "names"}, [&] {
      const std::vector<std::string_view> writable = target->property_names();
      const std::vector<std::string_view> readonly = target->readonly_property_names();
      std::string joined;
      for (const std::string_view name : writable) {
        if (!joined.empty()) joined += " · ";
        joined += name;
      }
      (void)text(c, [writable, joined] {
        return std::format("可写 {} 项：{}", writable.size(), joined.empty() ? "（无）" : joined);
      }, {.color = Tone::Muted, .size = 12.0f, .key = "writable"});
      std::string locked;
      for (const std::string_view name : readonly) {
        if (!locked.empty()) locked += " · ";
        locked += name;
      }
      (void)text(c, [readonly, locked] {
        return std::format("只读 {} 项：{}", readonly.size(), locked.empty() ? "（无）" : locked);
      }, {.color = Tone::Faint, .size = 12.0f, .key = "readonly"});
      // 两张表重叠 = 声明可写但设置不生效（最难查的一类不一致）
      std::string overlap;
      for (const std::string_view name : writable) {
        for (const std::string_view other : readonly) {
          if (name != other) continue;
          if (!overlap.empty()) overlap += " · ";
          overlap += name;
        }
      }
      if (!overlap.empty()) {
        (void)text(c, [overlap] { return "重叠（可写表与只读表重复）：" + overlap; },
                   {.color = Tone::Danger, .size = 12.0f, .key = "overlap"});
      }
    });
  }
};

/// 页面局部：靶子元素的查找/读/写（经真实元素树，不走应用）。
struct ProbeAccess {
  st::ui::UiRoot* root{nullptr};

  [[nodiscard]] auto find(std::string_view id) const -> Element* {
    if (root == nullptr) return nullptr;
    // 靶子挂在 `#probe-stage` 之下：用整 id 精确查（`UiRoot::find` 是全树 id 查找）
    return root->find(id);
  }
  [[nodiscard]] auto read(std::string_view id, std::string_view name) const
      -> std::optional<std::string> {
    Element* element = find(id);
    if (element == nullptr) return std::nullopt;
    return element->get_property(name);
  }
  auto write(std::string_view id, std::string_view name, std::string_view value) const -> bool {
    Element* element = find(id);
    if (element == nullptr) return false;
    const bool ok = element->set_property(name, value);
    if (ok) element->mark_layout_dirty();
    return ok;
  }
};

}  // namespace

/// 构建属性面页（含靶子区、检视器宿主、说明卡）。
///
/// 检视器本身是声明式子树（`mount_into` 到 `#property-host`），由 `main.cpp` 挂载——
/// 与「声明式」页同一条通路，正好也演示了**同一页混用两种写法**是可行的。
[[nodiscard]] auto build_properties_page(const PageHooks& hooks) -> std::unique_ptr<Panel> {
  auto page = std::make_unique<Panel>(FlexDirection::Column);
  page->set_id("page-properties");
  page->style().padding = Insets::all(24.0f);
  page->style().gap = 16.0f;
  page->add_child(std::make_unique<Heading>("属性面", 2));
  auto sub = std::make_unique<Text>(
      "组件声明 property_schema() → build_property_panel 自动生成控件 → 改动经 set_property "
      "落到**真实元素**上。三张表（可写名 / 只读名 / 描述）任何一处对不上，这一页就会看起来不对。");
  sub->set_tone(Tone::Muted);
  page->add_child(std::move(sub));

  // —— 靶子区 ——
  auto stage_card = std::make_unique<Card>(16.0f);
  stage_card->set_id("card-probe-stage");
  stage_card->style().direction = FlexDirection::Column;
  stage_card->style().gap = 10.0f;
  auto stage_title = std::make_unique<Text>("检视靶子（真实组件实例）");
  stage_title->set_weight(st::ui::FontWeight::SemiBold);
  stage_card->add_child(std::move(stage_title));
  auto stage = std::make_unique<Panel>(FlexDirection::Row);
  stage->set_id("probe-stage");
  stage->style().gap = 24.0f;
  stage->style().wrap = true;
  stage->style().align_items = st::ui::Align::Center;
  for (const ProbeSpec& spec : kProbes) {
    auto column = std::make_unique<Panel>(FlexDirection::Column);
    column->style().gap = 6.0f;
    auto label = std::make_unique<Text>(spec.label);
    label->set_scale(Text::Scale::Xs);
    label->set_tone(Tone::Faint);
    column->add_child(std::move(label));
    column->add_child(make_probe(spec.id));
    stage->add_child(std::move(column));
  }
  stage_card->add_child(std::move(stage));
  page->add_child(std::move(stage_card));

  // —— 检视器宿主（声明式子树挂到这里）——
  auto host = std::make_unique<Panel>(FlexDirection::Column);
  host->set_id("property-host");
  host->style().gap = 10.0f;
  page->add_child(std::move(host));

  // —— 说明卡（这一页在验什么）——
  auto about = std::make_unique<Card>(16.0f);
  about->set_id("card-properties-about");
  about->style().direction = FlexDirection::Column;
  about->style().gap = 6.0f;
  auto about_title = std::make_unique<Text>("三张表各自管什么");
  about_title->set_weight(st::ui::FontWeight::SemiBold);
  about->add_child(std::move(about_title));
  auto body = std::make_unique<Text>(
      "property_names() 决定 set 认哪些名字；readonly_property_names() 决定 get 能读到哪些"
      "只读项；property_schema() 决定自动面板生成什么控件（类型/范围/步进/候选）。\n"
      "三者任一漂移都只表现为“这个控件好像没什么用”——所以自检台有专门一条检查它们。");
  body->set_tone(Tone::Muted);
  body->set_scale(Text::Scale::Xs);
  about->add_child(std::move(body));
  page->add_child(std::move(about));

  (void)hooks;
  return page;
}

/// 造属性检视器组件（供 `main.cpp` 挂到 `#property-host`）。
///
/// 指针注入放在这里（而不是组件内部去 `UiRoot` 找）：组件不该知道自己在哪棵树上——
/// 它只声明"我要能按 id 找元素、能读、能写"，具体怎么找由宿主给。
[[nodiscard]] auto make_property_inspector_component(st::ui::UiRoot& root)
    -> std::shared_ptr<st::ui::dsl::Component> {
  auto inspector = std::make_shared<PropertyInspector>();
  auto access = std::make_shared<ProbeAccess>(ProbeAccess{&root});
  inspector->find_target = [access](std::string_view id) { return access->find(id); };
  inspector->read_property = [access](std::string_view id, std::string_view name) {
    return access->read(id, name);
  };
  inspector->write_property = [access](std::string_view id, std::string_view name,
                                       std::string_view value) {
    return access->write(id, name, value);
  };
  return inspector;
}

}  // namespace gallery
