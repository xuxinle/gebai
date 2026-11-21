// 声明式 UI 的**便利构造函数**（`dsl::text` / `button` / `markdown` / `table` / …）。
//
// ## 为什么这些函数单独一个翻译单元
//
// 每个便利函数都要对**具体组件类型**做 `dynamic_cast`（`Button*` / `MarkdownView*` /
// `Table*` …）才能调它的专属 setter。于是它对本 TU 产生该组件的**未定义引用**，
// 链接器必须把那个组件的 `.o` 拉进产物。
//
// 放在 `dsl.cpp`（声明式核心：Composer / reconcile / State 依赖追踪）里时，后果是
// **按需链接被一击穿透**：任何用了 `dsl` 的应用都会连带进 CodeEditor / Terminal /
// MarkdownView / DiffView / HtmlView / Table / Tree / FileDialog 八个组件——
// 实测 gedou 为此白背 **656 KB**（它一个都不用）。
//
// 拆开之后：`dsl.cpp` 只剩核心（不认识任何具体组件），本文件才认识组件。
// 谁真的调了 `dsl::table(...)`，谁才把 `table.o` 拉进来——这才是按需。
//
// ## 代价（必须知道）
//
// 用到便利函数的应用要多编一个 TU（约 18 KB 源码）。这是无条件的净收益：
// 不用组件的应用省下的是数百 KB，用得到的应用多花的是毫秒级编译时间。

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/ui/dsl.hpp"

#include "st/core/json.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/components/all.hpp"
#include "st/ui/element.hpp"
#include "st/ui/svg.hpp"

namespace st::ui::dsl {

// 便利函数体从 `dsl.cpp` 原样搬来（见该文件的同名函数历史）。
// `apply_box` / `fail_missing_element_factory` 由 `dsl.cpp` 提供**外部链接**定义
// （两者都在 `dsl.hpp` 里声明，可直接调用）。

auto text(Composer& c, std::function<std::string()> content, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Text", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Text");
  }
  apply_box(*element, props);
  if (auto* text_element = dynamic_cast<Text*>(element); text_element != nullptr) {
    text_element->set_content(content ? content() : std::string());
  }
  // 注：闭包被丢弃——v1 每次重组整个 build 重跑，content 在 build 期间求值即可。
  // （惰性求值 + 依赖收集的细粒度路径在 M2 后续接入；语义见 docs/declarative.md §4.1）
  return *element;
}

auto button(Composer& c, std::string label, std::function<void()> on_click,
            const BoxProps& props) -> Element& {
  Element* element = c.create_element("Button", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Button");
  }
  apply_box(*element, props);
  if (auto* button_element = dynamic_cast<Button*>(element); button_element != nullptr) {
    button_element->set_label(std::move(label));
  }
  if (on_click) c.connect(*element, "click", std::move(on_click));
  return *element;
}

auto checkbox(Composer& c, std::string label, bool checked,
              std::function<void(bool)> on_change, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Checkbox", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Checkbox");
  }
  apply_box(*element, props);
  if (auto* box = dynamic_cast<Checkbox*>(element); box != nullptr) {
    box->set_label(std::move(label));
    box->set_checked(checked);
    if (on_change) box->on_change = std::move(on_change);
  }
  return *element;
}

auto switch_(Composer& c, bool checked, std::function<void(bool)> on_change,
             const BoxProps& props) -> Element& {
  Element* element = c.create_element("Switch", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Switch");
  }
  apply_box(*element, props);
  if (auto* sw = dynamic_cast<Switch*>(element); sw != nullptr) {
    sw->set_checked(checked);
    if (on_change) sw->on_change = std::move(on_change);
  }
  return *element;
}

auto slider(Composer& c, float value, std::function<void(float)> on_change,
            const BoxProps& props) -> Element& {
  Element* element = c.create_element("Slider", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Slider");
  }
  apply_box(*element, props);
  if (auto* slider_element = dynamic_cast<Slider*>(element); slider_element != nullptr) {
    slider_element->set_value(value);
    if (on_change) slider_element->on_change = std::move(on_change);
  }
  return *element;
}

auto input(Composer& c, std::string value, std::function<void(std::string)> on_input,
           const BoxProps& props, bool password) -> Element& {
  Element* element = c.create_element("Input", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Input");
  }
  apply_box(*element, props);
  if (auto* input_element = dynamic_cast<Input*>(element); input_element != nullptr) {
    // 幂等写入：内容相同就不写——`Input::set_text` 会把光标推到末尾，
    // 而声明式下 build 随时可能重跑（任一状态变更）；无条件写会让
    // 「打字中光标乱跳」（输入框回归到上一次 set 时的末尾）——实测踩到过。
    if (input_element->value() != value) input_element->set_text(value);
    // 密码态也幂等写（`set_password` 自身有相等早退，这里写直白些与 set_text 对齐）
    if (input_element->password() != password) input_element->set_password(password);
    if (on_input) {
      input_element->on_change = [on_input](std::string_view v) { on_input(std::string(v)); };
    }
  }
  return *element;
}

auto progress(Composer& c, float value, const BoxProps& props) -> Element& {
  Element* element = c.create_element("ProgressBar", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("ProgressBar");
  }
  apply_box(*element, props);
  if (auto* bar = dynamic_cast<ProgressBar*>(element); bar != nullptr) {
    bar->set_value(value);
  }
  return *element;
}

auto badge(Composer& c, std::string text_value, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Badge", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Badge");
  }
  apply_box(*element, props);
  if (auto* badge_element = dynamic_cast<Badge*>(element); badge_element != nullptr) {
    badge_element->set_text(std::move(text_value));
  }
  return *element;
}

auto heading(Composer& c, std::string content, std::uint32_t level, const BoxProps& props)
    -> Element& {
  Element* element = c.create_element("Heading", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Heading");
  }
  apply_box(*element, props);
  if (auto* head = dynamic_cast<Heading*>(element); head != nullptr) {
    head->set_level(level);
    head->set_content(std::move(content));
  }
  return *element;
}

auto divider(Composer& c, bool vertical) -> Element& {
  Element* element = c.create_element("Divider", st::Json::object());
  if (element == nullptr) {
    fail_missing_element_factory("Divider");
  }
  if (vertical) element->style().width = 1.0f;  // 竖线近似：v1 水平线为主
  return *element;
}

auto card(Composer& c, const BoxProps& props, std::function<void()> children) -> Element& {
  Element* element = c.create_element("Card", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Card");
  }
  apply_box(*element, props);
  BuildScope scope(c, element);
  if (children) children();
  return *element;
}

auto spacer(Composer& c, float size) -> Element& {
  Element* element = c.create_element("Spacer", st::Json::object());
  if (element == nullptr) {
    fail_missing_element_factory("Spacer");
  }
  // `size <= 0` = **弹性空隙**（不是“0 宽固定块”）：
  // `spacer()` 是右对齐的惯用写法，而固定宽度为 0 的块在布局里等同于“不存在”
  // ——调用方要的是「把其余内容推开」，写成固定 0 宽只会静默失效
  // （实测：标题栏的 — □ × 因此挤在标题文字后面）。
  if (size > 0.0f) {
    element->style().width = size;
    element->style().height = size;
  } else {
    element->style().grow = true;
  }
  return *element;
}

auto icon(Composer& c, std::string name, float size, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Icon", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Icon");
  }
  apply_box(*element, props);
  if (auto* view = dynamic_cast<IconView*>(element); view != nullptr) {
    view->set_icon(std::move(name));
    view->set_size(size);
  }
  return *element;
}

auto markdown(Composer& c, std::function<std::string()> source, const BoxProps& props)
    -> Element& {
  Element* element = c.create_element("MarkdownView", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("MarkdownView");
  }
  apply_box(*element, props);
  if (auto* view = dynamic_cast<MarkdownView*>(element); view != nullptr) {
    // `source` 是惰性闭包（与 `text` 同口径）：重组时重新求值，闭包内读 State 即自动订阅。
    // `set_markdown` 自带相等早退，所以这里不必像 `input` 那样手写幂等判定。
    view->set_markdown(source ? source() : std::string());
    // 排版三件套的下放（dsl.hpp 的 markdown 声明注释早已承诺，此前从未接上）
    //（见 `MarkdownView::apply_theme` 里的同一段说明）：
    //   `size`  → 正文字号（`set_base_font_size`，盖过主题的 font_base；
    //             代码块字号与标题阶梯仍按主题算）；
    //   `color`/`hex_color` → 文字色显式覆盖（每帧回放，盖过主题的 text 色）。
    // 不接的话，DSL 侧写了 `.size = 16` 首帧看似生效、下一帧被 apply_theme 盖回
    //（文档承诺与实现脱节的静默失效）。
    if (props.size >= 0.0f) view->set_base_font_size(props.size);
    if (!props.hex_color.empty()) {
      if (const auto parsed = svg::parse_color(props.hex_color); parsed.has_value()) {
        view->set_text_color(*parsed);
      }
    } else if (props.color.has_value()) {
      view->set_text_tone(*props.color);
    }
  }
  return *element;
}

auto overlay(Composer& c, std::string_view key, const BoxProps& props,
             std::function<void()> children) -> Element& {
  // 槽位（同 key 复用宿主）；`key` 即身份，本次不声明 → sweep 移除
  Element* host = c.overlay_slot(key);
  apply_box(*host, props);
  BuildScope scope(c, host);
  if (children) children();
  return *host;
}

auto menu_bar(Composer& c, const std::vector<MenuData>& menus,
              std::function<void(const std::string&, const std::string&)> on_action,
              std::function<void(std::size_t)> on_open_menu, const BoxProps& props) -> MenuBar* {
  Element* element = c.create_element("MenuBar", st::Json::object(), props.key);
  if (element == nullptr) return nullptr;
  apply_box(*element, props);
  auto* bar = dynamic_cast<MenuBar*>(element);
  if (bar == nullptr) return nullptr;
  std::vector<Menu> list;
  list.reserve(menus.size());
  for (const auto& menu : menus) {
    Menu entry{};
    entry.id = menu.id;
    entry.label = menu.label;
    for (const auto& item : menu.items) {
      MenuItem item_entry{};
      item_entry.id = item.id;
      item_entry.label = item.label;
      item_entry.separator = item.separator;
      entry.items.push_back(std::move(item_entry));
    }
    list.push_back(std::move(entry));
  }
  bar->set_menus(list);
  if (on_action) {
    bar->on_action = [on_action](const std::string& menu, const std::string& item) {
      on_action(menu, item);
    };
  }
  if (on_open_menu) bar->on_open_menu = on_open_menu;
  // 下拉面板的关闭请求（Esc / 点面板外 / 激活条目）必须回落给调用方——
  // 面板的 `on_close` 已在 `MenuBar::make_panel` 里发，但**谁来摘这个 overlay**
  // 只有调用方知道。不接它的话：“Esc 关了”的只是 `MenuBar::open_index_`，
  // 面板本身永远留在屏上（实测：菜单打开后关不掉）。
  //
  // 这里接的是**同一个 `on_open_menu` 回调的逆语义**：`kNoIndex` 表示“不打开任何菜单”，
  // 即声明层面不认领那个 overlay，下一帧框架 sweep 自动摘除。
  bar->on_menu_close = [bar, on_open_menu]() {
    bar->set_open_index(MenuBar::kNoIndex);
    if (on_open_menu) on_open_menu(MenuBar::kNoIndex);
  };
  return bar;
}

auto context_menu(Composer& c, math::Point anchor, std::vector<MenuItem> items,
                  const BoxProps& props) -> ContextMenu& {
  Element* element = c.create_element("ContextMenu", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("ContextMenu");
  }
  apply_box(*element, props);
  // 锚点与条目是构造后写入的一等接口（与 `ContextMenu::make` 同一套：
  // 工厂里也走 `anchor_` + `MenuPanel` 两段，语义一致）。
  auto* menu = dynamic_cast<ContextMenu*>(element);
  if (menu != nullptr) {
    menu->anchor_ = anchor;
    if (menu->panel() != nullptr) {
      menu->panel()->set_items(std::move(items));
    }
  }
  return *menu;
}


auto list(Composer& c, const std::vector<ListItemData>& items,
          std::function<void(std::size_t)> on_click, const BoxProps& props) -> Element& {
  Element* element = c.create_element("List", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("List");
  }
  apply_box(*element, props);
  if (auto* list_element = dynamic_cast<List*>(element); list_element != nullptr) {
    std::vector<List::Entry> entries;
    entries.reserve(items.size());
    for (const auto& item : items) {
      List::Entry entry{};
      entry.key = item.key;
      entry.label = item.label;
      // ⚠ 副行必须一并搬（否则 DSL 侧给了值也画不出来——`Entry` 曾经没这个字段，
      // 那是「数据模型有、转接不搬」的典型缺陷）。
      entry.subtitle = item.subtitle;
      entry.icon = item.icon;
      entries.push_back(std::move(entry));
    }
    list_element->sync_items(entries);
    if (on_click) {
      // List 的统一回调口是 on_select（点击/键盘/协议 invoke 都汇聚到 select→on_select；
      // Entry::on_activate 是「项级回调」，会覆盖 bind_item 的选中链路，不用它）
      list_element->set_on_select([on_click](std::size_t index) { on_click(index); });
    }
  }
  return *element;
}

auto select(Composer& c, const std::vector<SelectOptionData>& options,
            std::optional<std::size_t> selected,
            std::function<void(std::size_t)> on_change, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Select", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Select");
  }
  apply_box(*element, props);
  if (auto* widget = dynamic_cast<Select*>(element); widget != nullptr) {
    std::vector<SelectOption> list;
    list.reserve(options.size());
    for (const auto& option : options) {
      list.push_back(SelectOption{option.value, option.label});
    }
    widget->set_options(std::move(list));
    widget->set_selected_index(selected);
    if (on_change) {
      // `Select::on_change` 传的是**选中项的 value 文本**（组件不知道调用方的索引语义）。
      // 声明式入口要的是「第几项」——按 value 反查（options 是本次声明的快照）。
      widget->on_change = [on_change, options](std::string_view value) {
        for (std::size_t index = 0; index < options.size(); ++index) {
          if (options[index].value == value) {
            on_change(index);
            return;
          }
        }
      };
    }
  }
  return *element;
}

auto table(Composer& c, const std::vector<TableColumnData>& columns,
           const std::vector<std::vector<std::string>>& rows,
           std::function<void(std::size_t)> on_row_click, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Table", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Table");
  }
  apply_box(*element, props);
  if (auto* widget = dynamic_cast<Table*>(element); widget != nullptr) {
    std::vector<TableColumn> list;
    list.reserve(columns.size());
    for (const auto& column : columns) {
      TableColumn entry{};
      entry.name = column.label;   // 组件字段叫 `name`（表头文本），声明式条目叫 `label`
      entry.width = column.width;
      list.push_back(std::move(entry));
    }
    widget->set_columns(std::move(list));
    widget->clear_rows();
    for (const auto& row : rows) widget->add_row(row);
    if (on_row_click) widget->set_on_row_click(std::move(on_row_click));
  }
  return *element;
}

auto tree(Composer& c, const std::vector<TreeNodeData>& nodes,
          std::function<void(const std::string&, bool)> on_toggle,
          std::function<void(const std::string&)> on_select, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Tree", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Tree");
  }
  apply_box(*element, props);
  if (auto* widget = dynamic_cast<Tree*>(element); widget != nullptr) {
    std::vector<TreeNode> list;
    list.reserve(nodes.size());
    for (const auto& node : nodes) {
      TreeNode entry{};
      entry.key = node.key;
      entry.label = node.label;
      entry.expanded = node.expanded;
      entry.is_dir = node.is_dir;
      entry.depth = node.depth;
      entry.icon = node.icon;
      list.push_back(std::move(entry));
    }
    widget->sync_nodes(list);   // 按 key 对齐（同 `Tree::sync_nodes` 语义）
    if (on_toggle) {
      widget->on_toggle = [on_toggle](std::string_view key, bool expanded) {
        on_toggle(std::string(key), expanded);
      };
    }
    if (on_select) {
      widget->on_select = [on_select](std::string_view key) { on_select(std::string(key)); };
    }
  }
  return *element;
}

auto tabs(Composer& c, const std::vector<TabData>& items, std::size_t active,
         std::function<void(std::size_t)> on_change,
         std::function<void(const std::string&)> on_close, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Tabs", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Tabs");
  }
  apply_box(*element, props);
  if (auto* tabs_element = dynamic_cast<Tabs*>(element); tabs_element != nullptr) {
    std::vector<Tabs::Tab> list;
    list.reserve(items.size());
    for (const auto& item : items) {
      list.push_back(Tabs::Tab{item.key, item.label, item.modified, item.closable});
    }
    tabs_element->sync_tabs(list);   // 按 key 复用（id/活动态保持）
    tabs_element->set_active(active);
    if (on_change) {
      tabs_element->on_change = [on_change](std::size_t index) { on_change(index); };
    }
        if (on_close) {
      tabs_element->on_close = [tabs_element, on_close](std::size_t index) {
        // 回调给「key」而不是索引：索引会随标签增删漂移，key 才是业务身份。
        // ⚠ 用 `tab_key` 而非 `tab_label`：「标签名 ≠ key」时两者分叉
        //（如 `untitled-1.txt` vs `untitled-1`）——旧写法拿 label 回去，
        // 调用方按 key 对账对不上，关闭静默落空。
        if (index < tabs_element->tab_count()) {
          on_close(std::string(tabs_element->tab_key(index)));
        }
      };
    }
  }
  return *element;
}


}  // namespace st::ui::dsl
