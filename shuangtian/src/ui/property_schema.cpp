/// 霜天属性描述契约的实现（见 `property_schema.hpp`）。

#include "st/ui/property_schema.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/core/json.hpp"

namespace st::ui {

auto validate_property_schema(std::span<const PropertyDesc> descs) -> std::optional<std::string> {
  for (std::size_t i = 0; i < descs.size(); ++i) {
    const PropertyDesc& desc = descs[i];
    if (desc.name.empty()) {
      return std::format("第 {} 条属性描述缺 name", i);
    }
    // 重名 = 查表只能查到第一个，后面那个永远不会被面板生成出来
    for (std::size_t j = i + 1; j < descs.size(); ++j) {
      if (descs[i].name == descs[j].name) {
        return std::format("属性 '{}' 被登记了两次（面板只会生成第一个）", desc.name);
      }
    }
    if (property_type_is_numeric(desc.type)) {
      // 反向区间会让夹取把任何值都推到 min——面板上表现为"拖到哪都一样"
      if (desc.min_value > desc.max_value) {
        return std::format("属性 '{}' 的范围反了：[{}, {}]", desc.name, desc.min_value,
                           desc.max_value);
      }
      // step 必须为正且小于区间（否则滑块只有一格）
      if (desc.step < 0.0f) {
        return std::format("属性 '{}' 的 step 为负（{}）", desc.name, desc.step);
      }
      if (desc.step > 0.0f && desc.max_value > desc.min_value &&
          desc.step > desc.max_value - desc.min_value) {
        return std::format("属性 '{}' 的 step（{}）大于整个区间 [{}, {}]，滑块只有一格",
                           desc.name, desc.step, desc.min_value, desc.max_value);
      }
    }
    if (desc.type == PropertyType::Choice && desc.choices.empty()) {
      // 空候选的下拉在面板上是个永远点不出东西的框
      return std::format("属性 '{}' 的类型是 choice 但没给 choices", desc.name);
    }
    if (desc.type != PropertyType::Choice && !desc.choices.empty()) {
      // 不是错，但几乎总是写错了（候选只对 choice 生效）——明确拦下比静默忽略好
      return std::format("属性 '{}' 给了 choices 但类型是 {}（候选只对 choice 生效）",
                         desc.name, property_type_name(desc.type));
    }
  }
  return std::nullopt;
}

auto property_schema_to_json(std::span<const PropertyDesc> descs) -> std::string {
  st::Json list = st::Json::array();
  for (const PropertyDesc& desc : descs) {
    st::Json item = st::Json::object();
    item["name"] = std::string(desc.name);
    item["type"] = std::string(property_type_name(desc.type));
    if (property_type_is_numeric(desc.type)) {
      item["min"] = desc.min_value;
      item["max"] = desc.max_value;
      if (desc.step > 0.0f) item["step"] = desc.step;
    }
    if (!desc.summary.empty()) item["summary"] = std::string(desc.summary);
    if (!desc.choices.empty()) {
      st::Json choices = st::Json::array();
      for (const std::string_view choice : desc.choices) choices.push_back(std::string(choice));
      item["choices"] = std::move(choices);
    }
    if (desc.readonly) item["readonly"] = true;
    if (desc.advanced) item["advanced"] = true;
    list.push_back(std::move(item));
  }
  return st::json_dump(list, false);
}

auto find_property_desc(std::span<const PropertyDesc> descs, std::string_view name)
    -> const PropertyDesc* {
  for (const PropertyDesc& desc : descs) {
    if (desc.name == name) return &desc;
  }
  return nullptr;
}

auto clamp_property_value(const PropertyDesc& desc, double raw) -> double {
  double value = raw;
  // 整数类型必须落到整格上：否则面板显示 3 而实际是 3.4，
  // 下次读回来"怎么变了"——这类不一致只在往返时才暴露。
  if (desc.type == PropertyType::Integer) value = std::round(value);
  if (property_type_is_numeric(desc.type)) {
    value = std::clamp(value, static_cast<double>(desc.min_value),
                       static_cast<double>(desc.max_value));
    // 吸附到 step 网格（从 min 起算，这样 min 本身永远是一个合法停点）
    if (desc.step > 0.0f) {
      const double origin = static_cast<double>(desc.min_value);
      const double offset = (value - origin) / static_cast<double>(desc.step);
      value = origin + std::round(offset) * static_cast<double>(desc.step);
      // 吸附可能把值推出上界（例如 max 不是 step 的整数倍）——再夹一次
      value = std::clamp(value, static_cast<double>(desc.min_value),
                         static_cast<double>(desc.max_value));
    }
  }
  // NaN 必须被拦下：它会静默传播进布局，表现为"元素消失"而不是报错
  if (!std::isfinite(value)) value = static_cast<double>(desc.min_value);
  return value;
}

auto property_range_of(const PropertyDesc& desc) -> PropertyRangeMap {
  return PropertyRangeMap{static_cast<double>(desc.min_value),
                          static_cast<double>(desc.max_value)};
}

auto property_to_unit(const PropertyRangeMap& range, double value) -> float {
  const double span = range.max - range.min;
  if (span <= 0.0) return 0.0f;
  return static_cast<float>(std::clamp((value - range.min) / span, 0.0, 1.0));
}

auto property_from_unit(const PropertyRangeMap& range, const PropertyDesc& desc, double unit)
    -> double {
  const double span = range.max - range.min;
  const double raw = range.min + span * std::clamp(unit, 0.0, 1.0);
  // **换算完再走同一份夹取**（含整数落格、step 吸附、NaN 拦截）。
  //
  // 为何不在这里重写一遍吸附：那就是两处真相。面板拖到底、脚本写 999、
  // 控制通道写 -5，三条入口必须得到同一个结果。
  return clamp_property_value(desc, raw);
}

}  // namespace st::ui
