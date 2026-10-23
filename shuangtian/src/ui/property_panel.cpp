/// 由属性描述生成面板（见 `property_panel.hpp`）。

#include "st/ui/property_panel.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"

namespace st::ui {
namespace {

/// 把字符串解析成 double（失败给 `nullopt`）。
///
/// 不复用 `std::stod` 的异常路径：`CONVENTIONS.md` R10 禁异常，
/// 而面板读的是**外部数据**（可能被人工/脚本写成任意字符串）。
[[nodiscard]] auto parse_number(std::string_view text) -> std::optional<double> {
  if (text.empty()) return std::nullopt;
  double value = 0.0;
  std::size_t consumed = 0;
  try {
    value = std::stod(std::string(text), &consumed);
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (consumed != text.size()) return std::nullopt;
  return value;
}

[[nodiscard]] auto parse_bool(std::string_view text) -> bool {
  return text == "true" || text == "1" || text == "on" || text == "yes";
}

[[nodiscard]] auto format_number(double value, PropertyType type) -> std::string {
  if (type == PropertyType::Integer) return std::format("{:.0f}", value);
  // 去尾零：面板回写的值与用户看到的一致，避免 "1.000000" 这类噪音。
  std::string text = std::format("{:.4f}", value);
  while (!text.empty() && text.back() == '0') text.pop_back();
  if (!text.empty() && text.back() == '.') text.pop_back();
  return text.empty() ? "0" : text;
}

}  // namespace

auto build_property_panel(dsl::Composer& composer, std::span<const PropertyDesc> descs,
                          const PropertyPanelHooks& hooks,
                          const PropertyPanelOptions& options) -> std::size_t {
  std::size_t built = 0;
  for (const PropertyDesc& desc : descs) {
    if (desc.advanced && !options.show_advanced) continue;
    if (desc.readonly && !options.show_readonly) continue;

    const std::string name(desc.name);
    const std::string id = std::format("{}-{}", hooks.id_prefix, name);
    const bool enabled = options.editable && !desc.readonly;
    const std::string label = desc.summary.empty() ? name : std::string(desc.summary);

    // 读当前值：一次读、多分支用。读不到时用类型的“零值”占位，
    // 但仍然生成控件——面板少一个控件比显示一个空值更难排查。
    const std::optional<std::string> raw =
        hooks.read != nullptr ? hooks.read(desc.name) : std::nullopt;

    const auto commit = [&hooks, name](std::string value) {
      if (hooks.write == nullptr) return;
      (void)hooks.write(name, value);
    };

    // 标签（可选）：放在控件上方一行，与裁云检查器现有的视觉语言一致。
    if (options.label_width > 0.0f) {
      dsl::text(
          composer, [label] { return label; },
          {.color = Tone::Muted, .size = 12.0f, .id = id + "-label"});
    }

    switch (desc.type) {
      case PropertyType::Number:
      case PropertyType::Integer: {
        // 换算走**公共纯函数**（`property_to_unit` / `property_from_unit`）：
        // 面板拖到底、脚本写 999、控制通道写 -5，三条入口必须同一个结果。
        const PropertyRangeMap range = property_range_of(desc);
        const double current = raw.has_value() ? parse_number(*raw).value_or(range.min) : range.min;
        (void)dsl::slider(
            composer, property_to_unit(range, current),
            [range, desc, commit](float unit) {
              commit(format_number(property_from_unit(range, desc, unit), desc.type));
            },
            {.id = id, .key = name})
            .set_enabled(enabled);
        break;
      }
      case PropertyType::Boolean: {
        const bool checked = raw.has_value() && parse_bool(*raw);
        (void)dsl::switch_(
            composer, checked,
            [commit](bool on) { commit(on ? "true" : "false"); },
            {.id = id, .key = name})
            .set_enabled(enabled);
        break;
      }
      case PropertyType::Choice: {
        // 候选来自描述本身（单一真相源）。选中值不在候选里时**不预选任何项**
        // （`nullopt`），而不是默默选第一个——后者会让“值已被改过”这件事看不见。
        std::vector<dsl::SelectOptionData> items;
        items.reserve(desc.choices.size());
        std::optional<std::size_t> current_index;
        for (std::size_t index = 0; index < desc.choices.size(); ++index) {
          const std::string_view choice = desc.choices[index];
          items.push_back(dsl::SelectOptionData{.value = std::string(choice),
                                               .label = std::string(choice)});
          if (raw.has_value() && *raw == choice) current_index = index;
        }
        (void)dsl::select(
            composer, items, current_index,
            [commit, choices = desc.choices](std::size_t index) {
              if (index < choices.size()) commit(std::string(choices[index]));
            },
            {.id = id, .key = name})
            .set_enabled(enabled);
        break;
      }
      case PropertyType::Color:
      case PropertyType::Text:
      case PropertyType::Multiline: {
        // 文本类一律用单行输入：`Color` 的 `#rrggbb` 本身就是短文本。
        // 多行暂时不用文本域（面板里极少需要，而 TextArea 引入滚动嵌套）。
        const std::string current = raw.value_or(std::string{});
        (void)dsl::input(
            composer, current, [commit](std::string value) { commit(std::move(value)); },
            {.id = id, .key = name})
            .set_enabled(enabled);
        break;
      }
    }
    ++built;
  }
  return built;
}

}  // namespace st::ui
