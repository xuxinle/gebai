#include "st/ui/xml_ui.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <unordered_set>

#include "st/core/fs.hpp"
#include "st/ext/json.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace st::ui {
namespace {

/// XML 标签名 → 注册名 的**别名表**。
///
/// 为何需要别名而不是直接照搬注册名：
/// - 注册名是**对外契约**（属性面/协议/脚本按它寻址），不能为了"XML 好看"改它
///   （`IconView` 的注册名是 `Icon`、`TextArea`/`CommandPalette` 与类名同名）；
/// - 而 XML 标签是**写给读界面描述的人看的**，`<Row>`/`<Column>` 比 `<Panel>`
///   直接，`<Separator>` 比 `Divider` 自然。
///
/// 于是别名表只做"更好看的名字 → 注册名"的翻译，二者不互相牵制。
/// ⚠ 右边必须是**注册名**（`dsl::make_element` 认的那个），不是 C++ 类名：
/// `Icon` 的注册名就是 `Icon`（不是 `IconView`）——写成类名会静默查不到。
/// 这张表的每一项都有单测钉住（`tests/xml_ui_test.cpp`）。
struct Alias {
  const char* tag;
  const char* type;
};

constexpr Alias kAliases[] = {
    {"Row", "Panel"},     {"Column", "Panel"},    {"Separator", "Divider"},
    {"Progress", "ProgressBar"}, {"Toolbar", "Panel"}, {"Group", "Panel"},
    // `Scroll` 是 `ScrollView` 的常见叫法（XML 里写 `<Scroll>` 比 `<ScrollView>` 自然）。
    // 注册名是 `ScrollView`（见 dsl.cpp 的 X 宏表），不加这条别名就会
    // **静默"认不出标签"**——子树整块丢掉，现象是卡片的容器选择器都找不到。
    {"Scroll", "ScrollView"},
};

/// 需要构造参数的类型：XML 里用属性给（`<Table columns="a,b">`）。
/// 返回 false = 这个类型不能由本层直接造（需要 C++ 侧装配）。
auto make_element_for(const std::string& tag, const st::xml::Node& node, bool& need_row)
    -> std::unique_ptr<Element> {
  need_row = false;
  std::string type = tag;
  for (const auto& alias : kAliases) {
    if (type == alias.tag) {
      type = alias.type;
      break;
    }
  }
  if (tag == "Row") need_row = true;
  if (type == "Panel") return std::make_unique<Panel>(need_row ? FlexDirection::Row
                                                              : FlexDirection::Column);
  // 其余交给组件工厂（一份清单，两处派生；见 `dsl.cpp` 的 `ST_COMPONENT_LIST`）。
  (void)node;
  return dsl::make_element(type);
}

/// 属性是否属于 `BoxProps` 的字段名（这些由 `apply_box_props` 处理）。
///
/// 判据用**显式白名单**而不是"交给 apply_properties 试一下"：
/// 后者会把认不出的属性静默吞掉（`changed` 里没有 = 没生效，但调用方很难看出），
/// 而我们要把它们**报出来**。
const std::unordered_set<std::string>& box_prop_names() {
  static const std::unordered_set<std::string> names{
      "gap",     "padding",  "padding_x", "padding_y", "margin",  "width",      "height",
      "min_width", "min_height", "max_width", "max_height", "grow", "shrink", "wrap",
      "radius",  "direction", "align_items", "align_self", "justify", "surface",
      "background", "border_width", "border_color", "opacity", "clip_children",
      "text_align", "font_size", "font_weight", "monospace", "text_color", "text_size",
      "id",      "key",      "enabled",  "visible",    "label",    "text",       "icon",
      "value",   "placeholder", "checked", "selected", "active",  "options",
      "columns", "items",    "variant",  "title",    "body",       "actions",
      "content", "hint",     "level",    "page",       "read_only", "password",
  };
  return names;
}

/// 把一条 XML 属性转成 `apply_properties` 认的 JSON 值。
///
/// **不做类型推断**（`"8"` 不自动变成数字 8）：`apply_properties` 自己对数字/字符串
/// 都接受（布局属性走 `as_float`，它会 `strtof` 字符串），而推断会引入
/// "`"007"` 变成 7" 这类意外。XML 属性本来就是文本，交给消费者决定。
auto to_json(std::string_view text) -> st::Json {
  // 只有**明确的**真/假才转布尔：其余一律原样字符串。
  if (text == "true" || text == "false") return st::Json(text == "true");
  return st::Json(std::string(text));
}

/// 把一条属性写进元素：先 `BoxProps` 字段，再落到属性面（走 `apply_properties`）。
///
/// `source_name` 与 `name` **必须分开传**：`normalize_property` 会改名
///（`align` → `align_items`、`bg` → `background`），而**取值只能按 XML 里原来那个名字读**。
/// 曾经只传归一后的名字，于是 `attr_or("align_items")` 拿到空串（XML 里写的是 `align`），
/// `parse_align("")` 必失败——**每一个短名属性都被报成"值没被接受"**，
/// 而症状是"对齐/背景色写了不生效"（本轮热重载的建树报告里一次暴出 2 处）。
auto apply_one(UiRoot& root, Element& element, const std::string& name,
               const std::string& source_name, const st::xml::Node& node) -> bool {
  if (name == "id") {
    element.set_id(std::string(node.attr_or("id", "")));
    return true;
  }
  if (name == "key") {
    element.set_key(std::string(node.attr_or("key", "")));
    return true;
  }
  if (name == "hint") {
    element.set_hint(std::string(node.attr_or("hint", "")));
    return true;
  }
  st::Json props = st::Json::object();
  props[name] = to_json(node.attr_or(source_name, ""));
  const st::Json changed = ui::apply_properties(root, element, props);
  return !changed.empty();
}

void build_node(UiRoot& root, Element& parent, const st::xml::Node& node, XmlBuildReport& report,
                const XmlBuildOptions& options);

/// XML 属性名 → 框架属性名的**别名归一**（只是换名字，不猜语义）。
///
/// 为何需要：属性面的名字是**对外契约**（协议/脚本按它寻址），不能为了 XML 好看去改；
/// 而 XML 是写给人看的，短名更顺手。两者靠这张表分开：
///
/// | XML 写 | 实际属性 |
/// |---|---|
/// | `align` | `align_items`（与 `text_align` 区分开） |
/// | `valign` | `align_self` |
/// | `bg` / `fg` | `background` / `text_color` |
///
/// 注意这里**不做**"按元素猜语义"：`size` 在 `<Text>` 上是字号、在 `<Button>` 上是
/// 尺寸档，两者在各自属性面上**同名**——保留名字、由元素自己解释，比本层猜更准确。
[[nodiscard]] auto normalize_property(const std::string& name) -> std::string {
  if (name == "align") return "align_items";
  if (name == "valign") return "align_self";
  if (name == "bg") return "background";
  if (name == "fg") return "text_color";
  return name;
}

/// 递归建一个节点的子元素并挂到 `parent`。
void build_children(UiRoot& root, Element& parent, const st::xml::Node& node,
                    XmlBuildReport& report, const XmlBuildOptions& options) {
  for (const auto& child : node.children) {
    build_node(root, parent, child, report, options);
  }
}

void build_node(UiRoot& root, Element& parent, const st::xml::Node& node, XmlBuildReport& report,
                const XmlBuildOptions& options) {
  bool need_row = false;
  auto owned = make_element_for(node.name, node, need_row);
  if (owned == nullptr) {
    const std::string tag = node.name;
    if (std::ranges::find(report.unknown_tags, tag) == report.unknown_tags.end()) {
      report.unknown_tags.push_back(tag);
    }
    if (options.strict) return;   // 严格模式：整棵建树会在调用方被判定为失败
    return;                       // 容错模式：跳过这个节点（但已经记进报告）
  }
  ++report.elements;
  Element* element = owned.get();
  // 文本内容：`<Text>你好</Text>` / `<Button>确定</Button>` 这类。
  // 先于属性落：属性里的同名项（`text="…"`）应当覆盖它。
  if (!node.text.empty()) {
    const auto declared = element->property_names();
    const bool takes_text =
        std::ranges::find(declared, std::string_view("text")) != declared.end() ||
        std::ranges::find(declared, std::string_view("content")) != declared.end();
    if (takes_text) {
      st::Json props = st::Json::object();
      props["text"] = node.text;
      (void)ui::apply_properties(root, *element, props);
    }
  }
  // 属性：BoxProps 字段优先（它们是布局/外观的一等表达），其余走属性面。
  for (const auto& [name, unused] : node.attributes) {
    (void)unused;
    // 报告里写**原文属性名**（`align` 而非 `align_items`）——用户对着 XML 排查。
    const std::string target = normalize_property(name);
    const bool known = box_prop_names().contains(target);
    const bool applied = apply_one(root, *element, target, name, node);
    if (!applied && !known) {
      report.unknown_attributes.push_back(
          std::format("{}.{}（第 {} 行）", node.name, name, node.line));
    } else if (!applied) {
      // 名字认识但这一步没落上：可能是枚举值写错（`align_items="middle"`）。
      report.unknown_attributes.push_back(
          std::format("{}.{} 的值没被接受（第 {} 行）", node.name, name, node.line));
    }
  }
  build_children(root, *element, node, report, options);
  parent.add_child(std::move(owned));
}

}  // namespace

auto build_from_xml_node(const st::xml::Node& node, XmlBuildReport* report,
                         const XmlBuildOptions& options) -> std::unique_ptr<Element> {
  XmlBuildReport local;
  XmlBuildReport& out = report != nullptr ? *report : local;
  // 需要一个 `UiRoot` 才能走 `apply_properties`（`focused` 那条路要用它；
  // 其余属性不依赖它）。这里用一个**局部的空根**——建树只是把属性落进元素，
  // 与宿主窗口无关（元素随后被挂到哪个根上都能用）。
  UiRoot scratch;
  // ⚠ **匿名 root**（`root.name` 为空）= `st::xml` 把多个顶层元素收在了一个无名容器下：
  // 这里要建的是**容器本身**，而不是“拿空名字去查组件工厂”（那必然查不到，
  // 于是整棵建树返回 nullptr——实测：`<Text>一</Text><Text>二</Text>` 连第一个都建不出来）。
  const bool anonymous = node.name.empty();
  bool need_row = false;
  auto root_element = anonymous ? std::make_unique<Panel>(FlexDirection::Column)
                                : make_element_for(node.name, node, need_row);
  if (root_element == nullptr) {
    out.unknown_tags.push_back(node.name);
    return nullptr;
  }
  ++out.elements;
  if (!options.root_id.empty()) root_element->set_id(options.root_id);
  if (anonymous) {
    // 匿名容器只用于“兜住多个顶层元素”，建完就被拆开：把它自己当根返回会让
    // 调用方多一层无意义的包装（而且那层没有 id、选择器也选不中）。
    build_children(scratch, *root_element, node, out, options);
    if (options.strict && !out.clean()) return nullptr;
    return root_element;
  }
  Element* element = root_element.get();
  if (!node.text.empty()) {
    const auto declared = element->property_names();
    const bool takes_text =
        std::ranges::find(declared, std::string_view("text")) != declared.end() ||
        std::ranges::find(declared, std::string_view("content")) != declared.end();
    if (takes_text) {
      st::Json props = st::Json::object();
      props["text"] = node.text;
      (void)ui::apply_properties(scratch, *element, props);
    }
  }
  for (const auto& [name, value] : node.attributes) {
    (void)value;
    const std::string target = normalize_property(name);
    const bool known = box_prop_names().contains(target);
    const bool applied = apply_one(scratch, *element, target, name, node);
    if (!applied && !known) {
      out.unknown_attributes.push_back(
          std::format("{}.{}（第 {} 行）", node.name, name, node.line));
    } else if (!applied) {
      out.unknown_attributes.push_back(
          std::format("{}.{} 的值没被接受（第 {} 行）", node.name, name, node.line));
    }
  }
  build_children(scratch, *element, node, out, options);
  if (options.strict && !out.clean()) return nullptr;
  return root_element;
}

auto build_from_xml(std::string_view source, XmlBuildReport* report, const XmlBuildOptions& options)
    -> std::unique_ptr<Element> {
  XmlBuildReport local;
  XmlBuildReport& out = report != nullptr ? *report : local;
  st::xml::Error error;
  auto document = st::xml::parse(source, &error);
  if (!document.has_value()) {
    out.parse_warning = std::format("XML 解析失败：{}（第 {} 行）", error.message, error.line);
    return nullptr;
  }
  if (!error.message.empty()) {
    out.parse_warning = std::format("{}（第 {} 行）", error.message, error.line);
  }
  // 匿名 root（多个顶层元素）时：**第一个元素**当根，其余作为它的后续兄弟会被丢——
  // 所以那种形态下要求调用方用严格模式或自己包一层。这里建第一个并在报告里说明。
  if (document->root.name.empty() && !document->root.children.empty()) {
    if (!out.parse_warning.empty()) out.parse_warning += "；";
    out.parse_warning += std::format("有 {} 个顶层元素，只用了第一个（建议用单一根元素）",
                                     document->root.children.size());
  }
  auto element = build_from_xml_node(document->root, &out, options);
  if (element == nullptr) return nullptr;
  return element;
}

auto build_from_xml_file(const std::string& path, XmlBuildReport* report,
                         const XmlBuildOptions& options) -> std::unique_ptr<Element> {
  XmlBuildReport local;
  XmlBuildReport& out = report != nullptr ? *report : local;
  const auto text = st::fs::read_text(path);
  if (!text.has_value()) {
    out.parse_warning = std::format("读不到文件：{}（{}）", path, text.error().message);
    return nullptr;
  }
  return build_from_xml(*text, &out, options);
}

}  // namespace st::ui
