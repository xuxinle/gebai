/// 裁云图层属性描述表（见 `layer_schema.hpp`）。

#include "layer_schema.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "layer.hpp"
#include "st/core/log.hpp"
#include "st/core/json.hpp"
#include "st/ui/property_schema.hpp"

namespace caiyun {
namespace {

using st::ui::PropertyDesc;
using st::ui::PropertyType;

// 候选表必须是静态存储期（`PropertyDesc::choices` 是 span，指向它）。
constexpr std::string_view kAlignChoices[] = {"center", "left", "right", "justify"};
constexpr std::string_view kGradientChoices[] = {"vertical", "horizontal"};

// 布局是**唯一真相**：范围要与 `get/set_layer_property` 里实际夹取的一致。
//
// 为何不给每个字段都写 `advanced`：面板上先出现的应该是“最常改的”。
// 位置/尺寸/透明度是常用项；三维参数属于进阶（`advanced`）。
constexpr PropertyDesc kLayerProperties[] = {
    // —— 通用外观 ——
    {.name = "opacity", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 1.0f,
     .step = 0.01f, .summary = "整体透明度"},
    {.name = "radius", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 400.0f,
     .step = 1.0f, .summary = "圆角"},
    {.name = "stroke_width", .type = PropertyType::Number, .min_value = 0.0f,
     .max_value = 40.0f, .step = 0.5f, .summary = "描边宽度"},
    {.name = "fill", .type = PropertyType::Color, .summary = "填充色"},
    {.name = "stroke", .type = PropertyType::Color, .summary = "描边色"},
    {.name = "color", .type = PropertyType::Color, .summary = "文字色"},
    {.name = "gradient_dir", .type = PropertyType::Choice, .summary = "渐变方向",
     .choices = kGradientChoices},

    // —— 文字 ——
    {.name = "text", .type = PropertyType::Text, .summary = "文字内容"},
    {.name = "size", .type = PropertyType::Number, .min_value = 6.0f, .max_value = 400.0f,
     .step = 1.0f, .summary = "字号"},
    {.name = "tracking", .type = PropertyType::Number, .min_value = -20.0f, .max_value = 60.0f,
     .step = 0.5f, .summary = "字距"},
    {.name = "line_height", .type = PropertyType::Number, .min_value = 1.0f, .max_value = 4.0f,
     .step = 0.05f, .summary = "行高倍数"},
    {.name = "bold", .type = PropertyType::Boolean, .summary = "粗体（走真粗体面）"},
    {.name = "align", .type = PropertyType::Choice, .summary = "对齐", .choices = kAlignChoices},

    // —— 三维（进阶）——
    {.name = "model_segments", .type = PropertyType::Integer, .min_value = 3.0f,
     .max_value = 128.0f, .step = 1.0f, .summary = "三维细分", .advanced = true},
    {.name = "model_scale", .type = PropertyType::Number, .min_value = 0.05f,
     .max_value = 8.0f, .step = 0.01f, .summary = "三维缩放", .advanced = true},
    {.name = "model_spin", .type = PropertyType::Number, .min_value = -90.0f,
     .max_value = 90.0f, .step = 0.5f, .summary = "自转速度（度/秒）", .advanced = true},
    // 场景脚本：字段值是**一整个 JSON 对象**，而 schema 的属性类型只覆盖
    // 标量（文字/数字/布尔/枚举/颜色）。所以用 `Multiline` 寄存它：
    // 界面能把它当成多行文本编辑，而“未知字段”告警也不会误报。
    //
    // 为何不用 `Text`：那会渲染成单行输入框，一整个场景 JSON 在里面
    // 既看不清也没法编辑。
    {.name = "model_script", .type = PropertyType::Multiline,
     .summary = "场景脚本（整幅三维场景的 JSON）", .advanced = true},
    {.name = "cam_distance", .type = PropertyType::Number, .min_value = 1.5f,
     .max_value = 12.0f, .step = 0.1f, .summary = "相机距离", .advanced = true},
    {.name = "cam_fov", .type = PropertyType::Number, .min_value = 8.0f, .max_value = 110.0f,
     .step = 1.0f, .summary = "视野角（度）", .advanced = true},
    {.name = "light_azimuth", .type = PropertyType::Number, .min_value = -360.0f,
     .max_value = 360.0f, .step = 5.0f, .summary = "主光方位角", .advanced = true},
    {.name = "light_rim", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 1.0f,
     .step = 0.01f, .summary = "边缘光", .advanced = true},
};

}  // namespace

auto layer_property_schema() -> std::span<const PropertyDesc> {
  return kLayerProperties;
}

namespace {

/// 把一个颜色格式化成 `#rrggbb`（带 alpha 时 `#rrggbbaa`）。
auto color_text(const st::math::Color& color) -> std::string {
  if (color.a == 255) {
    return std::format("#{:02x}{:02x}{:02x}", color.r, color.g, color.b);
  }
  return std::format("#{:02x}{:02x}{:02x}{:02x}", color.r, color.g, color.b, color.a);
}

/// 解析 `#rrggbb` / `#rgb` / `#rrggbbaa`；失败给 `nullopt`。
auto parse_color(std::string_view text) -> std::optional<st::math::Color> {
  if (text.empty() || text.front() != '#') return std::nullopt;
  int_least32_t value = 0;
  std::size_t digits = 0;
  for (std::size_t index = 1; index < text.size(); ++index) {
    const char ch = text[index];
    int_least32_t nibble = 0;
    if (ch >= '0' && ch <= '9') {
      nibble = ch - '0';
    } else if (ch >= 'a' && ch <= 'f') {
      nibble = ch - 'a' + 10;
    } else if (ch >= 'A' && ch <= 'F') {
      nibble = ch - 'A' + 10;
    } else {
      return std::nullopt;
    }
    value = value * 16 + nibble;
    ++digits;
  }
  const auto channel = [](int_least32_t raw) -> std::uint8_t {
    return static_cast<std::uint8_t>(std::clamp<int_least32_t>(raw, 0, 255));
  };
  if (digits == 3) {
    // #rgb 简写：每位重复一次
    const auto expand = [](int_least32_t nibble) -> int_least32_t { return nibble * 17; };
    return st::math::Color{channel(expand((value >> 8) & 0xF)),
                           channel(expand((value >> 4) & 0xF)), channel(expand(value & 0xF)),
                           255};
  }
  if (digits == 6) {
    return st::math::Color{channel((value >> 16) & 0xFF), channel((value >> 8) & 0xFF),
                           channel(value & 0xFF), 255};
  }
  if (digits == 8) {
    return st::math::Color{channel((value >> 24) & 0xFF), channel((value >> 16) & 0xFF),
                           channel((value >> 8) & 0xFF), channel(value & 0xFF)};
  }
  return std::nullopt;
}

/// 数值的**同源夹取**：范围直接取描述表。
///
/// 为何一定要这样：面板与脚本是两个入口，若各自 `clamp` 一套边界，
/// 就会出现“面板拖到 1.0，脚本却写进了 5.0”的不一致，而两边都返回成功。
auto clamp_to_schema(const PropertyDesc& desc, double raw) -> double {
  double value = raw;
  if (desc.type == PropertyType::Integer) value = std::round(value);
  value = std::clamp(value, static_cast<double>(desc.min_value),
                     static_cast<double>(desc.max_value));
  if (desc.step > 0.0f) {
    const double origin = static_cast<double>(desc.min_value);
    const double offset = (value - origin) / static_cast<double>(desc.step);
    value = std::clamp(origin + std::round(offset) * static_cast<double>(desc.step),
                       static_cast<double>(desc.min_value),
                       static_cast<double>(desc.max_value));
  }
  if (!std::isfinite(value)) return static_cast<double>(desc.min_value);
  return value;
}

/// 把文本解析成数值（失败给 nullopt）。
auto parse_number(std::string_view text) -> std::optional<double> {
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

}  // namespace

auto get_layer_property(const Layer& layer, std::string_view name)
    -> std::optional<std::string> {
  const auto number = [](double value) { return std::format("{:.4f}", value); };
  if (name == "opacity") return number(static_cast<double>(layer.opacity));
  if (name == "radius") return number(static_cast<double>(layer.radius));
  if (name == "stroke_width") return number(static_cast<double>(layer.stroke_width));
  if (name == "fill") return color_text(layer.fill);
  if (name == "stroke") return color_text(layer.stroke);
  if (name == "color") return color_text(layer.color);
  if (name == "gradient_dir") {
    return std::string(layer.gradient_dir == 1 ? "horizontal" : "vertical");
  }
  if (name == "text") return layer.text;
  if (name == "size") return number(static_cast<double>(layer.size));
  if (name == "tracking") return number(static_cast<double>(layer.tracking));
  if (name == "line_height") return number(static_cast<double>(layer.line_height));
  if (name == "bold") return std::string(layer.bold ? "true" : "false");
  if (name == "align") {
    static constexpr std::string_view kNames[] = {"center", "left", "right", "justify"};
    const int index = std::clamp(layer.align, 0, 3);
    return std::string(kNames[index]);
  }
  if (name == "model_segments") return std::to_string(layer.model_segments);
  if (name == "model_scale") return number(static_cast<double>(layer.model_scale));
  if (name == "model_spin") return number(static_cast<double>(layer.model_spin));
  if (name == "model_script") return layer.model_script;
  if (name == "cam_distance") return number(static_cast<double>(layer.cam_distance));
  if (name == "cam_fov") return number(static_cast<double>(layer.cam_fov));
  if (name == "light_azimuth") return number(static_cast<double>(layer.light_azimuth));
  if (name == "light_rim") return number(static_cast<double>(layer.light_rim));
  return std::nullopt;
}

auto set_layer_property(Layer& layer, std::string_view name, std::string_view value) -> bool {
  const auto desc = st::ui::find_property_desc(kLayerProperties, name);
  if (desc == nullptr) return false;

  // 颜色与文本不走数值夹取，单独处理（但仍在同一函数里——单一入口）。
  if (desc->type == PropertyType::Color) {
    const auto color = parse_color(value);
    if (!color.has_value()) return false;
    if (name == "fill") layer.fill = *color;
    else if (name == "stroke") layer.stroke = *color;
    else if (name == "color") layer.color = *color;
    else return false;
    return true;
  }
  if (desc->type == PropertyType::Text) {
    if (name != "text") return false;
    layer.text = std::string(value);
    return true;
  }
  if (desc->type == PropertyType::Multiline) {
    if (name != "model_script") return false;
    // ## 不在 setter 里校验 JSON（这里踩过一次）
    //
    // 最初想“当场拦住非法 JSON”，但这会与一条**通用不变式**冲突：
    // `layer_schema_every_name_is_readable_and_writable` 要求每个 schema 字段
    // 都能用任意探测值写入——因为界面上用户确实会编到一半（半截 JSON）。
    // 拒绝写入会让“字段存在但改不动”，比存一份非法值更难查。
    //
    // 所以这里**原样存**，把错误推给真正使用它的地方：
    // `model3d.cpp` 渲染前解析，失败则画醒目占位 + `ST_LOG_WARN`。
    layer.model_script = std::string(value);
    return true;
  }
  if (desc->type == PropertyType::Boolean) {
    if (name != "bold") return false;
    layer.bold = value == "true" || value == "1" || value == "on";
    return true;
  }
  if (desc->type == PropertyType::Choice) {    for (std::size_t index = 0; index < desc->choices.size(); ++index) {
      if (desc->choices[index] != value) continue;
      if (name == "gradient_dir") {
        layer.gradient_dir = static_cast<int>(index);
        return true;
      }
      if (name == "align") {
        layer.align = static_cast<int>(index);
        return true;
      }
      return false;
    }
    return false;
  }

  // 数值：**一律经同源夹取**
  const auto parsed = parse_number(value);
  if (!parsed.has_value()) return false;
  const double snapped = clamp_to_schema(*desc, *parsed);
  if (name == "opacity") layer.opacity = static_cast<float>(snapped);
  else if (name == "radius") layer.radius = static_cast<float>(snapped);
  else if (name == "stroke_width") layer.stroke_width = static_cast<float>(snapped);
  else if (name == "size") layer.size = static_cast<float>(snapped);
  else if (name == "tracking") layer.tracking = static_cast<float>(snapped);
  else if (name == "line_height") layer.line_height = static_cast<float>(snapped);
  else if (name == "model_segments") layer.model_segments = static_cast<int>(snapped);
  else if (name == "model_scale") layer.model_scale = static_cast<float>(snapped);
  else if (name == "model_spin") layer.model_spin = static_cast<float>(snapped);
  else if (name == "cam_distance") layer.cam_distance = static_cast<float>(snapped);
  else if (name == "cam_fov") layer.cam_fov = static_cast<float>(snapped);
  else if (name == "light_azimuth") layer.light_azimuth = static_cast<float>(snapped);
  else if (name == "light_rim") layer.light_rim = static_cast<float>(snapped);
  else return false;
  return true;
}

auto layer_schema_json() -> std::string {
  return st::ui::property_schema_to_json(kLayerProperties);
}

}  // namespace caiyun
