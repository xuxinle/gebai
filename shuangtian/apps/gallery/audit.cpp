/// 画廊自检引擎的实现（判据与"为什么这么判"见 `audit.hpp` 与每条检查的注释）。

#include "audit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// 展示集登记真源（tests 与 gallery 共用；不手抄）。
#include "../../tests/support/gallery_exhibits.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/components/all.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/property_schema.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace gallery {
namespace {

using st::ui::Element;

// ————————————————————————————————————————————————————————————————————————————
// 遍历
// ————————————————————————————————————————————————————————————————————————————

/// 深度优先收集全树的元素指针（含根）。
[[nodiscard]] auto collect_all(Element& root) -> std::vector<Element*> {
  std::vector<Element*> out;
  const auto walk = [&out](Element& node, auto&& self) -> void {
    out.push_back(&node);
    for (const auto& child : node.children()) {
      if (child != nullptr) self(*child, self);
    }
  };
  walk(root, walk);
  return out;
}

/// 从 UiRoot 的内容根开始收集（统一入口：自检只关心根内容里的元素）。
[[nodiscard]] auto collect_from_root(st::ui::UiRoot& root) -> std::vector<Element*> {
  Element* content = root.content();
  if (content == nullptr) return {};
  return collect_all(*content);
}

/// 类型名去重排序（对账用）。
[[nodiscard]] auto sorted_unique(std::vector<std::string> items) -> std::vector<std::string> {
  std::ranges::sort(items);
  items.erase(std::ranges::unique(items).begin(), items.end());
  return items;
}

[[nodiscard]] auto has_name(const std::vector<std::string>& sorted, std::string_view name) -> bool {
  return std::ranges::binary_search(sorted, name);
}

/// 明细行末尾的"还有 N 条"（页面上不能铺 200 行）。
void append_overflow(AuditCheck& check, std::size_t total, std::size_t shown) {
  if (total > shown) {
    check.details.push_back(std::format("…还有 {} 条（明细上限 {}）", total - shown, shown));
  }
}

/// 计时器（自检每项各记一笔：自检本身不该成为负担，得能看见）。
class Stopwatch {
 public:
  Stopwatch() : start_(std::chrono::steady_clock::now()) {}
  [[nodiscard]] auto ms() const -> double {
    const auto delta = std::chrono::steady_clock::now() - start_;
    return std::chrono::duration<double, std::milli>(delta).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

// ————————————————————————————————————————————————————————————————————————————
// ① 注册表对账（注册表 − 豁免 == 展示集）
// ————————————————————————————————————————————————————————————————————————————

/// 与 `tests/gallery_component_parity_test.cpp` 同一条判据，但在**运行时**跑：
/// 组件表加了项、既没补卡也没写豁免时，这里当场红，而不用等 `st test`。
[[nodiscard]] auto check_registry() -> AuditCheck {
  AuditCheck check{.id = "registry", .title = "注册表对账"};
  const Stopwatch watch;
  st::ui::import_all_components();
  const std::vector<std::string> registered = sorted_unique(st::ui::dsl::registered_element_types());

  std::vector<std::string> exempt;
  for (const auto& [name, reason] : kGalleryExempt) {
    (void)reason;
    exempt.emplace_back(name);
  }
  const std::vector<std::string> exempt_sorted = sorted_unique(exempt);
  std::vector<std::string> exhibits{kGalleryExhibits.begin(), kGalleryExhibits.end()};
  const std::vector<std::string> exhibits_sorted = sorted_unique(exhibits);

  std::vector<std::string> missing;   // 注册表里有、既没卡也没豁免
  std::vector<std::string> stale;     // 展示集登记了注册表外的名字
  for (const std::string& name : registered) {
    if (!has_name(exempt_sorted, name) && !has_name(exhibits_sorted, name)) missing.push_back(name);
  }
  for (const std::string& name : exhibits_sorted) {
    if (!has_name(registered, name)) stale.push_back(name);
  }
  std::vector<std::string> both;   // 同时在展示集与豁免集
  for (const std::string& name : exempt_sorted) {
    if (has_name(exhibits_sorted, name)) both.push_back(name);
  }

  const std::size_t expected = registered.size() - exempt_sorted.size();
  check.summary = std::format("注册表 {} 项 − 豁免 {} 项 = {} 项，展示集登记 {} 项", registered.size(),
                              exempt_sorted.size(), expected, exhibits_sorted.size());
  const std::size_t kShown = 12;
  for (const std::string& name : missing) {
    check.details.push_back(std::format("缺卡：{}（注册了但既没展示位也没豁免理由）", name));
  }
  for (const std::string& name : stale) {
    check.details.push_back(std::format("多登：{}（展示集里有、注册表里没有——改名或拼错）", name));
  }
  for (const std::string& name : both) {
    check.details.push_back(std::format("重复：{}（补卡后应从豁免清单移除）", name));
  }
  check.status = (missing.empty() && stale.empty() && both.empty()) ? AuditStatus::Pass
                                                                   : AuditStatus::Fail;
  append_overflow(check, missing.size() + stale.size() + both.size(), kShown);
  check.elapsed_ms = watch.ms();
  return check;
}

// ————————————————————————————————————————————————————————————————————————————
// ② 真值树自检
// ————————————————————————————————————————————————————————————————————————————

/// 展示集登记了却不在树上 = 缺卡；树上出现了登记外类型 = 登记集漂移。
///
/// 与 `main.cpp` 启动日志那份**同一组判据**（那边只在启动时打一行），
/// 这里返回结构化结果供界面展示，也让自检台能随页面变化重新跑。
[[nodiscard]] auto check_tree(st::ui::UiRoot& root) -> AuditCheck {
  AuditCheck check{.id = "tree", .title = "真值树"};
  const Stopwatch watch;
  std::vector<std::string> seen;
  if (Element* content = root.content(); content != nullptr) {
    for (Element* node : collect_all(*content)) seen.push_back(std::string(node->type()));
  }
  const std::vector<std::string> tree_types = sorted_unique(seen);

  std::vector<std::string> lazy{kGalleryLazy.begin(), kGalleryLazy.end()};
  const std::vector<std::string> lazy_sorted = sorted_unique(lazy);
  std::vector<std::string> exhibits{kGalleryExhibits.begin(), kGalleryExhibits.end()};
  const std::vector<std::string> exhibits_sorted = sorted_unique(exhibits);

  // 放行清单（与 `gallery_exhibits.hpp` 头部口径逐条对应）：
  // 注入型 / 非注册的框架组件 / 画廊自绘类 / 框架懒注入。
  std::vector<std::string> allowed{
      "ListItem",   "ScrollBar", "MenuPanel", "TitleBar",  "Element",
      "ProgressRow", "IconCell", "SvgIconCell", "SelectPanel", "SwatchCell",
      // 三维族：不进声明式注册表（无头按名建三维没有意义），由 C++ 直接 new 挂树；
      // 展示位在「能力矩阵」页。它们不参与 gallery 的注册表对账。
      "ThreeView",  "GraphView", "LabelOverlay", "SceneView",
      // 自检台自己的自绘类（暂无；保留命名空间以免以后新增时忘了登记）
  };
  for (const auto& [name, reason] : kGalleryExempt) {
    (void)reason;
    allowed.emplace_back(name);
  }
  const std::vector<std::string> allowed_sorted = sorted_unique(allowed);

  std::vector<std::string> missing;
  for (const std::string& name : exhibits_sorted) {
    if (has_name(lazy_sorted, name)) continue;   // 懒创建：启动树不要求在场
    if (!has_name(tree_types, name)) missing.push_back(name);
  }
  std::vector<std::string> unexpected;
  for (const std::string& name : tree_types) {
    if (has_name(exhibits_sorted, name) || has_name(lazy_sorted, name) ||
        has_name(allowed_sorted, name)) {
      continue;
    }
    unexpected.push_back(name);
  }

  check.summary = std::format("树上 {} 种 type；登记 {}（其中懒创建 {}），放行 {} 种注入/自绘类",
                              tree_types.size(), exhibits_sorted.size(), lazy_sorted.size(),
                              allowed_sorted.size());
  const std::size_t kShown = 12;
  for (const std::string& name : missing) {
    check.details.push_back(std::format("缺卡：{}（登记了却不在树上——展示位被删或建卡失败）", name));
  }
  for (const std::string& name : unexpected) {
    check.details.push_back(
        std::format("登记外：{}（新组件？该登记进 kGalleryExhibits，或写进放行清单）", name));
  }
  check.status = (missing.empty() && unexpected.empty()) ? AuditStatus::Pass : AuditStatus::Fail;
  append_overflow(check, missing.size() + unexpected.size(), kShown);
  check.elapsed_ms = watch.ms();
  return check;
}

// ————————————————————————————————————————————————————————————————————————————
// ③ 属性面（名字表 vs 描述表）
// ————————————————————————————————————————————————————————————————————————————

/// 三条判据：
/// 1. `property_names()`（可写）与 `readonly_property_names()`（只读）**不得重叠**——
///    重叠意味着"声明可写但设置不生效"，而那正是最难看出的那类不一致；
/// 2. `property_schema()` 里的名字必须出现在两张名字表之一（否则面板生成了控件，
///    控制通道却读不到它）；
/// 3. 描述自洽（`validate_property_schema`：重名/范围反了/step 过大）。
[[nodiscard]] auto check_property_surface(st::ui::UiRoot& root) -> AuditCheck {
  AuditCheck check{.id = "properties", .title = "属性面"};
  const Stopwatch watch;
  std::vector<Element*> all = collect_from_root(root);
  std::vector<std::string> overlaps;
  std::vector<std::string> schema_orphans;
  std::vector<std::string> schema_invalid;
  std::size_t with_names = 0;
  std::size_t with_schema = 0;
  std::size_t schema_props = 0;

  for (Element* element : all) {
    const std::string id(element->id());
    const std::vector<std::string_view> writable = element->property_names();
    const std::vector<std::string_view> readonly = element->readonly_property_names();
    if (!writable.empty()) ++with_names;
    for (const std::string_view name : writable) {
      if (std::ranges::find(readonly, name) != readonly.end()) {
        overlaps.push_back(std::format("{}.{}（同时在可写表与只读表）", id, name));
      }
    }
    const std::span<const st::ui::PropertyDesc> descs = element->property_schema();
    if (!descs.empty()) {
      ++with_schema;
      schema_props += descs.size();
      if (const auto problem = st::ui::validate_property_schema(descs); problem.has_value()) {
        schema_invalid.push_back(std::format("{}: {}", id, *problem));
      }
      for (const st::ui::PropertyDesc& desc : descs) {
        const bool listed = std::ranges::find(writable, desc.name) != writable.end() ||
                            std::ranges::find(readonly, desc.name) != readonly.end();
        if (!listed) {
          schema_orphans.push_back(
              std::format("{}.{}（描述里有、名字表里没有）", id, desc.name));
        }
      }
    }
  }

  check.summary = std::format("{} 个元素声明了属性名，{} 个声明了描述（共 {} 项描述）",
                              with_names, with_schema, schema_props);
  const std::size_t kShown = 12;
  for (const std::string& line : overlaps) check.details.push_back("重叠：" + line);
  for (const std::string& line : schema_orphans) check.details.push_back("未入表：" + line);
  for (const std::string& line : schema_invalid) check.details.push_back("描述不合法：" + line);
  if (!overlaps.empty()) {
    check.status = AuditStatus::Fail;
  } else if (!schema_orphans.empty() || !schema_invalid.empty()) {
    check.status = AuditStatus::Warn;
  } else {
    check.status = AuditStatus::Pass;
  }
  append_overflow(check, overlaps.size() + schema_orphans.size() + schema_invalid.size(), kShown);
  check.elapsed_ms = watch.ms();
  return check;
}

// ————————————————————————————————————————————————————————————————————————————
// ④ 主题对比度
// ————————————————————————————————————————————————————————————————————————————

/// WCAG 对比度门槛：正文 ≥ 4.5、次要 ≥ 3.0、极弱 ≥ 2.0。
///
/// 为什么这条必须在界面上：主题是**数据**（10 套内置主题 + 用户 JSON 覆盖），
/// 加一套新配色时"字看不清"不会有任何编译期/运行时信号——只有人眼。把门槛写成
/// 可复算的判据，换主题后看一眼表就知道这套配色能不能用。
[[nodiscard]] auto check_theme(const st::ui::Theme& theme) -> AuditCheck {
  AuditCheck check{.id = "theme", .title = "主题对比度"};
  const Stopwatch watch;
  const st::ui::Palette& colors = theme.colors();
  struct Pair {
    const char* name;
    st::math::Color fg;
    st::math::Color bg;
    float minimum;
  };
  const Pair pairs[] = {
      {"text / bg", colors.text, colors.bg, 4.5f},
      {"text / surface", colors.text, colors.surface, 4.5f},
      {"text / surface_alt", colors.text, colors.surface_alt, 4.5f},
      {"text_muted / bg", colors.text_muted, colors.bg, 3.0f},
      {"text_faint / bg", colors.text_faint, colors.bg, 2.0f},
      {"on_primary / primary", colors.on_primary, colors.primary, 3.0f},
      {"danger / bg", colors.danger, colors.bg, 2.5f},
      {"success / bg", colors.success, colors.bg, 2.5f},
      {"warning / bg", colors.warning, colors.bg, 2.5f},
      {"border / bg", colors.border, colors.bg, 1.05f},
      {"surface / bg", colors.surface, colors.bg, 1.02f},
  };
  std::vector<std::string> failures;
  float worst_ratio = 1.0e9F;
  std::string worst_name;
  for (const Pair& pair : pairs) {
    const float ratio = st::math::contrast_ratio(pair.fg, pair.bg);
    const float normalized = ratio / pair.minimum;   // < 1 即不达标
    if (normalized < worst_ratio) {
      worst_ratio = normalized;
      worst_name = pair.name;
    }
    const std::string line = std::format("{:<22} {:>5.2f}:1（门槛 {:.2f}）", pair.name,
                                         static_cast<double>(ratio),
                                         static_cast<double>(pair.minimum));
    check.details.push_back(line);
    if (ratio < pair.minimum) failures.push_back(line);
  }
  check.summary = std::format("{} 组令牌对，最低是 {}（达门槛的 {:.0f}%）", std::size(pairs), worst_name,
                              static_cast<double>(worst_ratio) * 100.0);
  if (!failures.empty()) {
    check.status = AuditStatus::Fail;
    for (const std::string& line : failures) check.details.push_back("不达标：" + line);
  } else if (worst_ratio < 1.08f) {
    // 贴着门槛过：不算缺陷，但换字色/改令牌时最先崩的就是这一对。
    check.status = AuditStatus::Warn;
  } else {
    check.status = AuditStatus::Pass;
  }
  check.elapsed_ms = watch.ms();
  return check;
}

// ————————————————————————————————————————————————————————————————————————————
// ⑤ 布局整像素
// ————————————————————————————————————————————————————————————————————————————

/// 半像素的边界会让 1px 描边被劈成两半（各 50% 覆盖），表现为"只在一个方向发虚"。
/// `grow` 均分是它的主要来源（858 可用宽 ÷ 4 = 208.5，见 `ui_grow_pixel_test`）。
///
/// ## 为何把尺寸与位置分开统计（实测教训）
///
/// 第一版把 x/y/w/h 全算进去，119 个可见元素里 53 个"不达标"——而其中绝大多数是
/// **居中造成的合法小数**（父宽 901、子宽 200 → x = 350.5）。那是布局的正确结果，
/// 不是缺陷；把所有居中元素都报成 FAIL 的尺子只会被忽略。
///
/// 真正的缺陷形态是**尺寸**落在半像素：那会让描边剥成两半、让相邻块相互渗色。
/// 所以两类分开：尺寸 → FAIL 线索；位置 → 只作 WARN 提示（它确实会让 1px 描边发虚，
/// 但那是居中的固有代价，而不是某处算错了）。
/// ⑥ 布局必须落**整像素**。
///
/// ## 为何要区分“布局决定的尺寸”与“文字量测定的尺寸”
///
/// 第一版拿全树的 `bounds` 判整数，结果恒定 FAIL：明细里全是
/// `Button（dpi-toggle）: w=100.54`、`Text（status-right）: w=213.59`
/// ——而这两个数**本来就该带小数**：它们由 `TextPort::measure_width()`（字体
/// advance 的浮点和）决定，而不是布局分配的结果。要求它们整像素等于要求
/// 字体度量变成整数，那是错的。
///
/// 一个恒定喊 FAIL 的检查比没有检查更糟：它训练人忽略这一行。
/// 所以判据改成：**只看尺寸由布局分配的那类元素**——
/// 有显式 `style.width/height`（如 `width = 120`）的，或子树里没有文字
/// 而尺寸来自 `grow`/固定值的容器。文字量测定的（叶子 Text/Button/
/// wrap-content 容器）跳过。
///
/// `x/y` 仍全量检查（位置是布局算的，与字体无关），半像素只算 Warn——
/// 1px 描边在 .5 上会略虚，但不影响可读性。
[[nodiscard]] auto check_integer_pixels(st::ui::UiRoot& root) -> AuditCheck {
  AuditCheck check{.id = "pixels", .title = "布局整像素"};
  const Stopwatch watch;
  std::vector<std::string> size_offenders;
  std::vector<std::string> pos_offenders;
  std::size_t checked = 0;
  std::size_t skipped_measured = 0;
  const auto off_grid = [](float value) {
    return std::fabs(value - std::round(value)) > 0.01f;
  };
  // 子树里是否有**文字**（决定这个元素的宽高是否被字体度量“污染”）
  //
  // 两条判定，**先看属性面再看结构**：
  // ① `semantics_text()` 非空：这个元素自带文本（`Text`/`Button`/`Chip`/`Input`…——
  //    它们要么自绘文字，要么按 `TextPort::measure_width()` 算宽）。
  // ② 子树里有 `Text`：容器包着文字按内容撑开。
  //
  // `Icon` **不算**：它是矢量路径，尺寸来自 `style().size`（定值），
  // 不是 advance 求和的浮点。把它算进来会让图标行整批豁免，“布局真算错了”也跟着漏。
  //
  // 为何不能只靠 ②：按钮是**自绘**的（没有 `Text` 子元素），
  // 而它的宽度正是 `measure_width(label) + padding`——不含 ① 时会报一批
  // `Button: w=100.54` 的假 FAIL（实测 9 条）。
  const auto has_text_descendant = [](const Element* element) {
    if (!element->semantics_text().empty()) return true;
    const std::function<bool(const Element*)> probe = [&](const Element* node) -> bool {
      if (node->type() == "Text") return true;
      for (std::size_t index = 0; index < node->child_count(); ++index) {
        if (probe(node->child_at(index))) return true;
      }
      return false;
    };
    return probe(element);
  };
  for (Element* element : collect_from_root(root)) {
    if (!element->effective_visible()) continue;   // 隐藏子树不参与布局，不必要求
    const st::math::Rect box = element->bounds();
    if (box.width <= 0.0f || box.height <= 0.0f) continue;   // 空盒（折叠/占位）不算
    ++checked;
    // 尺寸豁免的两类“本来就不该整像素”的量：
    //
    // ① **字体度量**：宽/高由 `TextPort::measure_width()`（advance 浮点和）决定，
    //    要求它整像素等于要求字体度量变成整数——那是错的。
    // ② **显式写下的亚像素值**：`style().width/height` 明写着小数（如 `Divider`
    //    的 `kLineThickness = 0.5`——头注释里说明了这是**故意的**：0.5 在 2×DPI 下
    //    恰好一个物理像素，1× 下被覆盖率画成淡线，两档都比 1px 实线更轻）。
    //    用户（或组件作者）写了小数就是不要求整像素，判据不能跟他拧着来。
    const bool explicit_fraction =
        (element->style().has_explicit_width() && off_grid(element->style().width)) ||
        (element->style().has_explicit_height() && off_grid(element->style().height));
    if (off_grid(box.width) || off_grid(box.height)) {
      if (has_text_descendant(element) || explicit_fraction) {
        ++skipped_measured;
      } else {
        size_offenders.push_back(
            std::format("{}（{}）: w={:.2f} h={:.2f}", element->type(), std::string(element->id()),
                        static_cast<double>(box.width), static_cast<double>(box.height)));
        continue;
      }
    }
    if (off_grid(box.x) || off_grid(box.y)) {
      pos_offenders.push_back(
          std::format("{}（{}）: x={:.2f} y={:.2f}", element->type(), std::string(element->id()),
                      static_cast<double>(box.x), static_cast<double>(box.y)));
    }
  }
  check.summary = std::format(
      "检查 {} 个可见元素：{} 个布局尺寸落半像素，{} 个位置落半像素（另 {} 个尺寸属“字体度量/显式亚像素”，不要求整像素）",
      checked, size_offenders.size(), pos_offenders.size(), skipped_measured);
  const std::size_t kShown = 8;
  for (std::size_t index = 0; index < std::min(size_offenders.size(), kShown); ++index) {
    check.details.push_back("尺寸：" + size_offenders[index]);
  }
  append_overflow(check, size_offenders.size(), kShown);
  for (std::size_t index = 0; index < std::min(pos_offenders.size(), kShown); ++index) {
    check.details.push_back("位置（居中的自然结果，仅 1px 描边会略虚）：" + pos_offenders[index]);
  }
  append_overflow(check, pos_offenders.size(), kShown);
  if (!size_offenders.empty()) {
    check.status = AuditStatus::Fail;   // 尺寸落半像素 = 布局算错（或 grow 没落整）
  } else if (!pos_offenders.empty()) {
    check.status = AuditStatus::Warn;
  } else {
    check.status = AuditStatus::Pass;
  }
  check.elapsed_ms = watch.ms();
  return check;
}

// ————————————————————————————————————————————————————————————————————————————
// ⑥ 图标覆盖
// ————————————————————————————————————————————————————————————————————————————

/// 图标名写错**不报错也不占位**：`IconView` 找不到名字就什么也不画，于是内容被
/// 空位推偏——`btn-refresh` 曾把名字写成 `activity`，而内置表里没有它。
/// 这条检查把"全树的图标名都在内置表里"变成可断言的事实。
[[nodiscard]] auto check_icons(st::ui::UiRoot& root) -> AuditCheck {
  AuditCheck check{.id = "icons", .title = "图标覆盖"};
  const Stopwatch watch;
  const std::vector<std::string_view> names = st::ui::Icon::names();
  std::size_t icon_views = 0;
  std::vector<std::string> unknown;
  for (Element* element : collect_from_root(root)) {
    if (element->type() != "Icon") continue;
    ++icon_views;
    const std::string_view name = element->get_property("name").value_or("");
    if (name.empty()) continue;
    // SVG sprite 的名字走另一张表（`svg:` 前缀强制源），这里只判内置表能认的那部分。
    if (name.starts_with("svg:")) continue;
    if (!st::ui::Icon::has(name)) {
      unknown.push_back(std::format("{}（id={}）", name, std::string(element->id())));
    }
  }
  check.summary =
      std::format("内置图标 {} 个；全树 {} 个图标视图，{} 个用了表外名字", names.size(), icon_views,
                  unknown.size());
  const std::size_t kShown = 10;
  for (std::size_t index = 0; index < std::min(unknown.size(), kShown); ++index) {
    check.details.push_back("表外名字：" + unknown[index]);
  }
  append_overflow(check, unknown.size(), kShown);
  check.status = unknown.empty() ? AuditStatus::Pass : AuditStatus::Fail;
  check.elapsed_ms = watch.ms();
  return check;
}

// ————————————————————————————————————————————————————————————————————————————
// ⑦ DPI 与尺寸
// ————————————————————————————————————————————————————————————————————————————

/// 物理分辨率必须等于逻辑 × DPI（四舍五入到物理像素）。两者不同步的典型症状是
/// "内容只占一部分、周边空白/黑框"——而它的比值恰好等于 `device_scale`，
/// 这是启动类问题最快的定位依据（见 `pitfalls.md` 的量测条目）。
[[nodiscard]] auto check_dpi(const AuditEnv& env) -> AuditCheck {
  AuditCheck check{.id = "dpi", .title = "DPI 与尺寸"};
  const Stopwatch watch;
  const auto expected_width = static_cast<int>(
      std::lround(static_cast<double>(env.logical_width) * static_cast<double>(env.device_scale)));
  const auto expected_height = static_cast<int>(
      std::lround(static_cast<double>(env.logical_height) * static_cast<double>(env.device_scale)));
  const int delta_w = std::abs(env.physical_width - expected_width);
  const int delta_h = std::abs(env.physical_height - expected_height);
  check.summary = std::format("{}×{} × {:.2f} → 期望 {}×{}，实际 {}×{}（Δ {}×{}）",
                              env.logical_width, env.logical_height,
                              static_cast<double>(env.device_scale), expected_width,
                              expected_height, env.physical_width, env.physical_height, delta_w,
                              delta_h);
  check.details.push_back(std::format("后端 {} · 渲染器 {} · 文字 {}", env.backend, env.renderer,
                                      env.text_renderer));
  check.details.push_back(std::format("节点 {} · 帧 {} · 帧耗时 p50 {:.2f}ms / p95 {:.2f}ms",
                                      env.nodes, env.frames, env.frame_p50_ms, env.frame_p95_ms));
  // 允许 1px 的取整差（不同后端各自 round/floor 一次）。
  check.status = (delta_w <= 1 && delta_h <= 1) ? AuditStatus::Pass : AuditStatus::Fail;
  if (check.status == AuditStatus::Fail) {
    check.details.push_back(std::format("比值 {:.3f} × {:.3f}（约等于 device_scale 就是尺寸/DPI 不同步）",
                                        static_cast<double>(env.physical_width) / std::max(1, env.logical_width),
                                        static_cast<double>(env.physical_height) / std::max(1, env.logical_height)));
  }
  check.elapsed_ms = watch.ms();
  return check;
}

// ————————————————————————————————————————————————————————————————————————————
// ⑧ 元素身份（id / key）
// ————————————————————————————————————————————————————————————————————————————

/// 自动化定位的判据：**自动化要操作的界面必须显式给 id**，否则退化成按序号猜。
/// 这里统计"可交互但没有显式 id（自动生成的 `Type[i]`）"的元素——它们是
/// e2e 最容易写出"点了别的元素"的地方。
[[nodiscard]] auto check_identity(st::ui::UiRoot& root) -> AuditCheck {
  AuditCheck check{.id = "identity", .title = "元素身份"};
  const Stopwatch watch;
  std::vector<std::string> auto_ids;
  std::size_t interactive = 0;
  for (Element* element : collect_from_root(root)) {
    if (!element->focusable() && element->role() == st::ui::Role::None) continue;
    ++interactive;
    const std::string id(element->id());
    // 自动生成的 id 形如 `Type[i]` 或 `Type@key`——前者说明调用方没给 id。
    if (id.find('[') != std::string::npos) {
      auto_ids.push_back(std::format("{}（自动 id：{}）", element->type(), id));
    }
  }
  check.summary = std::format("{} 个可交互/有角色的元素，{} 个没给显式 id", interactive,
                              auto_ids.size());
  const std::size_t kShown = 10;
  for (std::size_t index = 0; index < std::min(auto_ids.size(), kShown); ++index) {
    check.details.push_back(auto_ids[index]);
  }
  append_overflow(check, auto_ids.size(), kShown);
  // 自动 id 不违法（框架会给），但**数量很大**说明这页自动化不好写。
  check.status = auto_ids.empty() ? AuditStatus::Pass : AuditStatus::Warn;
  check.elapsed_ms = watch.ms();
  return check;
}

}  // namespace

auto audit_status_label(AuditStatus status) noexcept -> const char* {
  switch (status) {
    case AuditStatus::Pass: return "PASS";
    case AuditStatus::Warn: return "WARN";
    case AuditStatus::Fail: return "FAIL";
    case AuditStatus::Info: return "INFO";
  }
  return "INFO";
}

auto audit_status_tone(AuditStatus status) noexcept -> const char* {
  switch (status) {
    case AuditStatus::Pass: return "Success";
    case AuditStatus::Warn: return "Warning";
    case AuditStatus::Fail: return "Danger";
    case AuditStatus::Info: return "Muted";
  }
  return "Muted";
}

auto run_audit(st::ui::UiRoot& root, const AuditEnv& env) -> AuditReport {
  AuditReport report{};
  const Stopwatch total;
  report.frames = env.frames;

  const auto push = [&report](AuditCheck check) {
    switch (check.status) {
      case AuditStatus::Pass: ++report.pass_count; break;
      case AuditStatus::Warn: ++report.warn_count; break;
      case AuditStatus::Fail: ++report.fail_count; break;
      case AuditStatus::Info: break;
    }
    report.checks.push_back(std::move(check));
  };

  push(check_registry());
  push(check_tree(root));
  push(check_property_surface(root));
  push(check_theme(root.theme()));
  push(check_integer_pixels(root));
  push(check_icons(root));
  push(check_dpi(env));
  push(check_identity(root));

  report.total_ms = total.ms();
  return report;
}

}  // namespace gallery
