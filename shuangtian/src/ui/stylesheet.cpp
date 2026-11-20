// CSS 式子集解析器的实现。设计理由见头文件。
#include "st/ui/stylesheet.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"
#include "st/ui/svg.hpp"   // parse_color（色值解析的唯一实现，与 DSL 的 hex_color 共用）
#include "st/ui/ui_root.hpp"

namespace st::ui {
namespace {

/// 校验器用的**最小页面**：一个空容器，只要能承载样式即可。
///
/// 为何需要真元素：`validate_stylesheet` 必须真跑一遍解析才能发现
/// “键表里有、但 `apply_declaration` 没有分支”这种缺陷（只查表发现不了）。
struct ProbePage : dsl::Component {
  void build(dsl::Composer& c) override {
    (void)dsl::column(c, {.width = 80.0f, .height = 60.0f, .key = "probe"}, [] {});
  }
};

/// 键名归一：小写、`-` → `_`（CSS 习惯用 `-`，C++ 习惯用 `_`，两者都认）。
auto normalize(std::string_view raw) -> std::string {
  std::string out;
  out.reserve(raw.size());
  for (const char ch : raw) {
    if (ch == ' ' || ch == '\t') continue;
    out.push_back(ch == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

auto trim(std::string_view text) -> std::string_view {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                           text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                           text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

/// 一个声明（`key value` 或 `key: value`）。
struct Declaration {
  std::string key;     ///< 已归一
  std::string value;   ///< 原文（未归一，颜色/枚举要保留大小写语义）
};

/// 切分声明列表。
///
/// **分隔符宽容**：`,` `;` 换行都算——CSS 用 `;`，本框架历史语法用 `,`，
/// 大模型两种都会写。不支持嵌套括号（本子集的取值里没有函数式语法）。
auto split_declarations(std::string_view text) -> std::vector<Declaration> {
  std::vector<Declaration> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = start;
    while (end < text.size() && text[end] != ',' && text[end] != ';' && text[end] != '\n') ++end;
    std::string_view item = trim(text.substr(start, end - start));
    start = end + 1;
    if (item.empty()) continue;
    // `key: value` / `key value` 两种都认（CSS 是前者，历史 glass 语法也是前者，
    // 而大模型偶尔会写成后者）。
    std::size_t split = item.find(':');
    if (split == std::string_view::npos) {
      split = item.find(' ');
      if (split == std::string_view::npos) split = item.find('\t');
    }
    if (split == std::string_view::npos) continue;
    Declaration decl;
    decl.key = normalize(item.substr(0, split));
    decl.value = std::string(trim(item.substr(split + 1)));
    if (!decl.key.empty() && !decl.value.empty()) out.push_back(std::move(decl));
  }
  return out;
}

auto parse_float(std::string_view text) -> std::optional<float> {
  const std::string copy(text);
  char* end = nullptr;
  const float value = std::strtof(copy.c_str(), &end);
  if (end == copy.c_str()) return std::nullopt;
  if (!std::isfinite(value)) return std::nullopt;
  return value;
}

auto parse_color_impl(std::string_view text) -> std::optional<math::Color> {
  if (text == "transparent") return math::Color{0, 0, 0, 0};
  return svg::parse_color(text);   // 复用 `svg.hpp` 那一份——不另造第二套色值解析
}

auto parse_edges(std::string_view text) -> std::optional<HighlightEdges> {
  const std::string v = normalize(text);
  if (v == "all" || v == "ring" || v == "4") return HighlightEdges::All;
  if (v == "tb" || v == "topbottom" || v == "top_bottom" || v == "2") {
    return HighlightEdges::TopBottom;
  }
  if (v == "top" || v == "t" || v == "1") return HighlightEdges::Top;
  return std::nullopt;
}

auto parse_weight(std::string_view text) -> std::optional<FontWeight> {
  const std::string v = normalize(text);
  if (v == "regular" || v == "normal" || v == "400") return FontWeight::Regular;
  if (v == "medium" || v == "500") return FontWeight::Medium;
  if (v == "semibold" || v == "600") return FontWeight::SemiBold;
  if (v == "bold" || v == "700") return FontWeight::Bold;
  return std::nullopt;
}

auto parse_text_align(std::string_view text) -> std::optional<TextAlign> {
  const std::string v = normalize(text);
  if (v == "start" || v == "left") return TextAlign::Start;
  if (v == "center" || v == "middle") return TextAlign::Center;
  if (v == "end" || v == "right") return TextAlign::End;
  return std::nullopt;
}

auto parse_align(std::string_view text) -> std::optional<Align> {
  const std::string v = normalize(text);
  if (v == "start" || v == "flex_start") return Align::Start;
  if (v == "center") return Align::Center;
  if (v == "end" || v == "flex_end") return Align::End;
  if (v == "stretch") return Align::Stretch;
  return std::nullopt;
}

auto parse_justify(std::string_view text) -> std::optional<Justify> {
  const std::string v = normalize(text);
  if (v == "start" || v == "flex_start") return Justify::Start;
  if (v == "center") return Justify::Center;
  if (v == "end" || v == "flex_end") return Justify::End;
  if (v == "between" || v == "space_between") return Justify::SpaceBetween;
  if (v == "around" || v == "space_around") return Justify::SpaceAround;
  return std::nullopt;
}

auto parse_bool(std::string_view text) -> std::optional<bool> {
  const std::string v = normalize(text);
  if (v == "1" || v == "on" || v == "true" || v == "yes") return true;
  if (v == "0" || v == "off" || v == "false" || v == "no") return false;
  return std::nullopt;
}

/// 阴影：`<blur> <offset_y> <color>`（省略 offset 视为 0；`inset` 前缀表示内阴影）。
///
/// 为何不是 CSS 的完整 `x y blur spread color`：本框架的 `Shadow` 只有
/// 单层偏移 + 模糊（没有 spread），宣称支持 CSS 全集会让调用方写出无效值。
auto parse_shadow(std::string_view text, Shadow& out) -> bool {
  // `shadow 0` / `none` = **不要投影**（`Shadow{}` 的 `visible()` 为假）。
  // 这条是必需的：`Card` 默认带 md 投影，而玻璃卡片要把投影关掉
  //（投影画在玻璃底下，会被背板模糊平均成一圈暗晕）；没这一档就只能绕
  // `elevation = Flat`，而那又连锁影响高光（一段历史坑）。
  const std::string flat = normalize(text);
  if (flat == "0" || flat == "none" || flat == "off") {
    out = Shadow{};
    return true;
  }
  std::vector<std::string> parts;
  std::size_t start = 0;
  bool inset = false;
  while (start <= text.size()) {
    std::size_t end = start;
    while (end < text.size() && text[end] != ' ') ++end;
    std::string_view tok = trim(text.substr(start, end - start));
    start = end + 1;
    if (tok.empty()) continue;
    if (normalize(tok) == "inset") {
      inset = true;
      continue;
    }
    parts.emplace_back(tok);
  }
  std::optional<math::Color> color;
  std::vector<float> numbers;
  for (const std::string& part : parts) {
    if (part.front() == '#') {
      color = parse_color_impl(part);
      if (!color.has_value()) return false;
      continue;
    }
    const auto number = parse_float(part);
    if (!number.has_value()) return false;
    numbers.push_back(*number);
  }
  if (!color.has_value()) return false;
  out.color = *color;
  out.inset = inset;
  out.blur = numbers.empty() ? 0.0f : numbers[0];
  out.offset_y = numbers.size() > 1 ? numbers[1] : 0.0f;
  out.offset_x = numbers.size() > 2 ? numbers[2] : 0.0f;
  return true;
}

// ————————————————————————————————————————————————————————————————————————
// 键表：**每个键对应 `Style` 的唯一字段**
//
// ⚠ 新增键必须同时做三件事（缺一件就是“声明了却不生效”的老毛病）：
//   ① 在 `sheet_key_names()` 登记；② 在 `apply_declaration` 里落位；
//   ③ 若它需要“打过声明标记才能扛住 apply_theme”，在这一处打。
// ————————————————————————————————————————————————————————————————————————

auto apply_declaration(Element& element, Style& style, const Declaration& decl, StyleGroup group,
                       StyleGroup filter) -> bool {
  const std::string& key = decl.key;
  const std::string_view raw = decl.value;
  const auto num = [&raw] { return parse_float(raw); };

  // —— Layout ——
  if (group == StyleGroup::Layout) {
    if (key == "width") {
      if (auto v = num()) { style.width = *v; return true; }
    } else if (key == "height") {
      if (auto v = num()) { style.height = *v; return true; }
    } else if (key == "max_width") {
      if (auto v = num()) { style.max_width = *v; return true; }
    } else if (key == "max_height") {
      if (auto v = num()) { style.max_height = *v; return true; }
    } else if (key == "min_width") {
      if (auto v = num()) { style.min_width = *v; return true; }
    } else if (key == "min_height") {
      if (auto v = num()) { style.min_height = *v; return true; }
    } else if (key == "gap") {
      if (auto v = num()) { style.gap = *v; return true; }
    } else if (key == "padding") {
      if (auto v = num()) { style.padding = math::Insets::all(*v); return true; }
    } else if (key == "padding_x") {
      if (auto v = num()) {
        // ⚠ 用**指定初始化器**：`Insets` 的字段序是 `{left, top, right, bottom}`
        // （不是 CSS 的 `{top, right, bottom, left}`）——按位置写会侣错。
        style.padding = math::Insets{.left = *v,
                                     .top = style.padding.top,
                                     .right = *v,
                                     .bottom = style.padding.bottom};
        return true;
      }
    } else if (key == "padding_y") {
      if (auto v = num()) {
        style.padding = math::Insets{.left = style.padding.left,
                                     .top = *v,
                                     .right = style.padding.right,
                                     .bottom = *v};
        return true;
      }
    } else if (key == "margin") {
      if (auto v = num()) { style.margin = math::Insets::all(*v); return true; }
    } else if (key == "grow") {
      if (auto v = parse_bool(raw)) { style.grow = *v; return true; }
    } else if (key == "shrink") {
      if (auto v = parse_bool(raw)) { style.shrink = *v; return true; }
    } else if (key == "wrap") {
      if (auto v = parse_bool(raw)) { style.wrap = *v; return true; }
    } else if (key == "align_items") {
      if (auto v = parse_align(raw)) { style.align_items = *v; return true; }
    } else if (key == "align_self") {
      if (auto v = parse_align(raw)) { style.align_self = *v; return true; }
    } else if (key == "justify") {
      if (auto v = parse_justify(raw)) { style.justify = *v; return true; }
    } else if (key == "absolute") {
      if (auto v = parse_bool(raw)) { style.absolute = *v; return true; }
    } else if (key == "left") {
      if (auto v = num()) { style.left = *v; return true; }
    } else if (key == "top") {
      if (auto v = num()) { style.top = *v; return true; }
    } else if (key == "right") {
      if (auto v = num()) { style.right = *v; return true; }
    } else if (key == "bottom") {
      if (auto v = num()) { style.bottom = *v; return true; }
    }
    return false;
  }

  // —— Paint ——
  if (group == StyleGroup::Paint) {
    if (key == "fill" || key == "background") {
      if (auto v = parse_color_impl(raw)) {
        style.set_solid_background(*v);
        return true;
      }
    } else if (key == "radius") {
      if (auto v = num()) { style.radius = *v; return true; }
    } else if (key == "radius_max") {
      if (auto v = num()) { style.radius_max = *v; return true; }
    } else if (key == "opacity") {
      if (auto v = num()) { style.opacity = std::clamp(*v, 0.0f, 1.0f); return true; }
    } else if (key == "border") {
      // `border: 0` = 无描边；`border: 1 #00000022` = 宽 + 色。
      // **必须走存档入口**：`Card::apply_theme` 每帧把 `border_width` 重置成主题值，
      // 而描边画在高光之后——不存声明就会被盖成一道灰线。
      std::vector<std::string> parts;
      std::size_t start = 0;
      while (start <= raw.size()) {
        std::size_t end = start;
        while (end < raw.size() && raw[end] != ' ') ++end;
        std::string_view tok = trim(raw.substr(start, end - start));
        start = end + 1;
        if (!tok.empty()) parts.emplace_back(tok);
      }
      if (!parts.empty()) {
        const auto width = parse_float(parts[0]);
        if (!width.has_value()) return false;
        element.set_border_width_override(*width);
        style.border_width = *width;
        if (parts.size() > 1) {
          if (auto c = parse_color_impl(parts[1])) {
            style.border_color = *c;
            element.set_border_color_override(*c);
          } else {
            return false;
          }
        }
        return true;
      }
    } else if (key == "border_width") {
      if (auto v = num()) {
        element.set_border_width_override(*v);
        style.border_width = *v;
        return true;
      }
    } else if (key == "border_color") {
      if (auto v = parse_color_impl(raw)) {
        style.border_color = *v;
        element.set_border_color_override(*v);
        return true;
      }
    } else if (key == "shadow") {
      Shadow shadow{};
      if (parse_shadow(raw, shadow)) {
        style.shadow = shadow;
        element.set_shadow_override(shadow);
        return true;
      }
    } else if (key == "clip") {
      if (auto v = parse_bool(raw)) { style.clip_children = *v; return true; }
    } else if (key == "theme_backdrop") {
      if (auto v = parse_bool(raw)) { style.use_theme_backdrop = *v; return true; }
    }
    return false;
  }

  // —— Glass ——
  if (group == StyleGroup::Glass) {
    if (key == "blur") {
      if (auto v = num()) { style.backdrop_blur = *v; return true; }
    } else if (key == "tint") {
      if (auto v = num()) {
        style.backdrop_tint = *v;
        // 打标记：否则 `apply_theme_glass` 每帧写回主题值（实测改 0.22→0.9 画面不变）。
        element.mark_glass_tint_declared();
        return true;
      }
    } else if (key == "refraction" || key == "refract") {
      if (auto v = num()) { style.backdrop_refraction = *v; return true; }
    } else if (key == "dispersion") {
      if (auto v = num()) { style.backdrop_dispersion = *v; return true; }
    } else if (key == "theme_glass") {
      if (auto v = parse_bool(raw)) { style.theme_glass = *v; return true; }
    } else if (key == "highlight") {
      if (auto v = num()) {
        // 只定**强度**：颜色随主题（`Metrics::glass_highlight`），
        // 由 `apply_theme_glass` 合成。占位色留白，避免回放期出现黑边。
        style.top_highlight = math::Color{255, 255, 255,
                                         static_cast<std::uint8_t>(
                                             std::clamp(*v, 0.0f, 1.0f) * 255.0f)};
        element.set_top_highlight_override(style.top_highlight);
        return true;
      }
    } else if (key == "highlight_color") {
      if (auto v = parse_color_impl(raw)) {
        style.top_highlight = math::Color{v->r, v->g, v->b, style.top_highlight.a};
        element.set_top_highlight_override(style.top_highlight);
        // 显式给色后主题不再覆盖（与 tint 同一条契约）。
        element.mark_top_highlight_color_declared();
        return true;
      }
    } else if (key == "ring" || key == "highlight_width") {
      if (auto v = num()) { style.highlight_width = std::max(*v, 0.0f); return true; }
    } else if (key == "edges" || key == "highlight_edges") {
      if (auto v = parse_edges(raw)) { style.highlight_edges = *v; return true; }
    }
    return false;
  }

  // —— Text ——
  if (group == StyleGroup::Text) {
    if (key == "color") {
      if (auto v = parse_color_impl(raw)) { style.color = *v; return true; }
    } else if (key == "size" || key == "font_size") {
      if (auto v = num()) { style.font_size = *v; return true; }
    } else if (key == "weight" || key == "font_weight") {
      if (auto v = parse_weight(raw)) { style.font_weight = *v; return true; }
    } else if (key == "align" || key == "text_align") {
      if (auto v = parse_text_align(raw)) { style.text_align = *v; return true; }
    } else if (key == "monospace") {
      if (auto v = parse_bool(raw)) { style.monospace = *v; return true; }
    }
    return false;
  }
  (void)filter;
  return false;
}

auto group_of_key(std::string_view name) -> std::optional<StyleGroup> {
  for (const SheetKey& key : sheet_key_names()) {
    if (key.name == name) return key.group;
  }
  return std::nullopt;
}

}  // namespace

auto sheet_key_names() -> const std::vector<SheetKey>& {
  using G = StyleGroup;
  static const std::vector<SheetKey> keys = {
      // —— Layout ——
      {"width", G::Layout, "<像素>"},
      {"height", G::Layout, "<像素>"},
      {"max_width", G::Layout, "<像素>"},
      {"max_height", G::Layout, "<像素>"},
      {"min_width", G::Layout, "<像素>"},
      {"min_height", G::Layout, "<像素>"},
      {"gap", G::Layout, "<像素>"},
      {"padding", G::Layout, "<像素>"},
      {"padding_x", G::Layout, "<像素>"},
      {"padding_y", G::Layout, "<像素>"},
      {"margin", G::Layout, "<像素>"},
      {"grow", G::Layout, "0|1"},
      {"shrink", G::Layout, "0|1"},
      {"wrap", G::Layout, "0|1"},
      {"align_items", G::Layout, "start|center|end|stretch"},
      {"align_self", G::Layout, "start|center|end|stretch"},
      {"justify", G::Layout, "start|center|end|between|around"},
      {"absolute", G::Layout, "0|1"},
      {"left", G::Layout, "<像素>"},
      {"top", G::Layout, "<像素>"},
      {"right", G::Layout, "<像素>"},
      {"bottom", G::Layout, "<像素>"},
      // —— Paint ——
      {"fill", G::Paint, "#RRGGBB[AA]|transparent"},
      {"background", G::Paint, "同 fill"},
      {"radius", G::Paint, "<像素>"},
      {"radius_max", G::Paint, "<像素>"},
      {"opacity", G::Paint, "0~1"},
      {"border", G::Paint, "<宽> [#RRGGBB]"},
      {"border_width", G::Paint, "<像素>"},
      {"border_color", G::Paint, "#RRGGBB[AA]"},
      {"shadow", G::Paint, "[inset] <blur> <offset_y> [#RRGGBBAA]"},
      {"clip", G::Paint, "0|1"},
      {"theme_backdrop", G::Paint, "0|1"},
      // —— Glass ——
      {"blur", G::Glass, "<像素>"},
      {"tint", G::Glass, "0~1"},
      {"refraction", G::Glass, "<像素>"},
      {"refract", G::Glass, "同 refraction"},
      {"dispersion", G::Glass, "0~1"},
      {"theme_glass", G::Glass, "0|1"},
      {"highlight", G::Glass, "0~1（强度，颜色随主题）"},
      {"highlight_color", G::Glass, "#RRGGBB"},
      {"ring", G::Glass, "<像素>"},
      {"highlight_width", G::Glass, "同 ring"},
      {"edges", G::Glass, "all|tb|top"},
      {"highlight_edges", G::Glass, "同 edges"},
      // —— Text ——
      {"color", G::Text, "#RRGGBB[AA]"},
      {"size", G::Text, "<像素>"},
      {"font_size", G::Text, "同 size"},
      {"weight", G::Text, "regular|medium|semibold|bold"},
      {"font_weight", G::Text, "同 weight"},
      {"align", G::Text, "start|center|end"},
      {"text_align", G::Text, "同 align"},
      {"monospace", G::Text, "0|1"},
  };
  return keys;
}

auto apply_stylesheet(Element& element, std::string_view text, SheetReport* report)
    -> std::size_t {
  Style& style = element.style();
  SheetReport local;
  std::size_t applied = 0;
  for (const Declaration& decl : split_declarations(text)) {
    const auto group = group_of_key(decl.key);
    if (!group.has_value()) {
      local.unknown.push_back(decl.key);
      continue;
    }
    if (apply_declaration(element, style, decl, *group, *group)) {
      ++applied;
    } else {
      // 键认识但**值**不认识（`radius: abc`）：也算失败，且必须报出来——
      // 静默忽略会让“写了却没生效”变成谜。
      local.unknown.push_back(decl.key + "=" + decl.value);
    }
  }
  local.applied = applied;
  if (report != nullptr) *report = std::move(local);
  return applied;
}

auto validate_stylesheet(std::string_view text) -> SheetReport {
  // **真的解析一遍**（不是只查表）。
  //
  // 为何不能只查 `group_of_key`（实测 2026-11-19）：
  // 那只能验“名字在表里”，验不了“这个名字真的能落位”。
  // 而“登了名字但忘了写 `apply_declaration` 分支”正是这一层**唯一的新增缺陷形态**
  // （键表驱动的方式少写一条分支，编译器不会报任何东西）。
  // 实测：写入一个表里有、解析层不认的键，只查表版的校验器仍然全绿。
  //
  // 现在用一个**临时元素**真跑一次 `apply_stylesheet`，
  // 于是“表里有但落不了位”与“值不合法”都会被归进 `unknown`。
  UiRoot scratch;
  scratch.set_viewport({100.0f, 100.0f});
  auto page = std::make_shared<ProbePage>();
  if (dsl::mount(scratch, page) == nullptr) {
    // 建不出探针元素时退回只查表（不应发生，但不因此崩掉校验器）。
    SheetReport fallback;
    for (const Declaration& decl : split_declarations(text)) {
      if (!group_of_key(decl.key).has_value()) {
        fallback.unknown.push_back(decl.key);
      } else {
        ++fallback.applied;
      }
    }
    return fallback;
  }
  scratch.layout();
  Element* probe = scratch.content();
  if (probe == nullptr) return SheetReport{};
  SheetReport report;
  (void)apply_stylesheet(*probe, text, &report);
  return report;
}

}  // namespace st::ui
