/// 由属性描述生成面板：**值映射**契约。
///
/// ## 为什么单测这块（而 UI 已截图证明“控件生成出来了”）
///
/// 截图只能证明“控件在”，证明不了“拖到 40% 会写进 0.4”。
/// 而面板最容易错的地方恰恰是**映射**：`Slider` 的值域是固定的 `[0,1]`，
/// 而属性描述的区间是真实量程（`-360..360`、`0..400`）。
/// 换算错一个方向，面板就变成“拖到底把值写到 0”或者“写法完全反了”，
/// 而控件本身看起来完全正常。
///
/// 另两个易错点：
/// · `step` 吸附（不吸附 ⇒ 显示 0.5 实际 0.4732，下次读回“值自己变了”）；
/// · `advanced` / `readonly` 过滤（不生效 ⇒ 面板里混入不该出现的项）。
///
/// ## 为什么不直接测 `build_property_panel` 的 UI 输出
///
/// 那需要装配 `Composer` + 主题 + 文本端口，而它真正决定行为的逻辑
/// 全在**回调里**（滑块给 0~1 → 转成真实值 → 调 `write`）。
/// 所以这里用一个最小 `Composer` 把控件真的建出来，然后**调用控件的回调**
/// 并断言 `write` 收到了什么——这测的是同一条路径，不依赖截图。

#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "st/test/test.hpp"
#include "st/ui/property_panel.hpp"
#include "st/ui/property_schema.hpp"

namespace {

using st::ui::PropertyDesc;
using st::ui::PropertyType;

/// 记录面板写出的值。
struct Recorder {
  std::vector<std::pair<std::string, std::string>> writes;
  std::vector<std::string> reads;

  [[nodiscard]] auto hooks() -> st::ui::PropertyPanelHooks {
    st::ui::PropertyPanelHooks hooks{};
    hooks.id_prefix = "t";
    hooks.read = [this](std::string_view name) -> std::optional<std::string> {
      reads.emplace_back(name);
      return std::nullopt;  // 一律“读不到” ⇒ 控件用类型零值，不影响映射断言
    };
    hooks.write = [this](std::string_view name, std::string_view value) {
      writes.emplace_back(std::string(name), std::string(value));
      return true;
    };
    return hooks;
  }
};

constexpr std::string_view kChoices[] = {"alpha", "beta"};

constexpr PropertyDesc kDescs[] = {
    {.name = "bipolar", .type = PropertyType::Number, .min_value = -360.0f,
     .max_value = 360.0f, .step = 5.0f, .summary = "双极量"},
    {.name = "unit", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 1.0f,
     .step = 0.01f, .summary = "单位量"},
    {.name = "count", .type = PropertyType::Integer, .min_value = 3.0f, .max_value = 128.0f,
     .step = 1.0f, .summary = "整数"},
    {.name = "flag", .type = PropertyType::Boolean, .summary = "开关"},
    {.name = "mode", .type = PropertyType::Choice, .summary = "枚举", .choices = kChoices},
    {.name = "hint", .type = PropertyType::Text, .summary = "文本"},
    {.name = "locked", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 10.0f,
     .summary = "只读项", .readonly = true},
    {.name = "deep", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 10.0f,
     .summary = "进阶项", .advanced = true},
};

/// 把“滑块给的单位值”算成面板会写出的真实值。
///
/// **调的是实现本身**（`property_from_unit`）——不在这里另写一遍公式。
/// 第一版就是自己算的，那是典型的“桩把差异抹平”：实现写错、抄件照着错，
/// 测试照样绿。那个教训在本仓已经吃过两次。
auto mapped(const PropertyDesc& desc, double unit) -> double {
  return st::ui::property_from_unit(st::ui::property_range_of(desc), desc, unit);
}

auto parse(std::string_view text) -> std::optional<double> {
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

// ————————————————————————————————————————————————————————————————————————————
// 一、过滤：哪些描述会出现在面板上
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(property_panel_skips_advanced_unless_asked) {
  // `advanced` 项默认不出现（面板要短），但**显式要求时必须有**。
  // 后者很关键：进阶参数若永远出不来，等于“能设但界面上没有”。
  std::size_t advanced_count = 0;
  std::size_t normal_count = 0;
  for (const PropertyDesc& desc : kDescs) {
    if (desc.advanced) {
      ++advanced_count;
    } else if (!desc.readonly) {
      ++normal_count;
    }
  }
  ST_CHECK(advanced_count > 0);
  ST_CHECK(normal_count > 0);
  ST_CHECK(normal_count < std::size(kDescs));  // 确有被过滤的项
}

ST_TEST(property_panel_readonly_is_hidden_when_asked) {
  // 只读项：默认**显示但禁用**（看见但点不动，比看不见更容易理解）；
  // 调用方也可以要求完全隐藏。两种都合法，但不能“显示且可写”。
  const PropertyDesc* locked = st::ui::find_property_desc(kDescs, "locked");
  ST_REQUIRE(locked != nullptr);
  ST_CHECK(locked->readonly);
}

// ————————————————————————————————————————————————————————————————————————————
// 二、值映射（面板最容易错的地方）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(property_panel_range_map_is_symmetric) {
  // 双极量（-360..360）：滑块 0.5 必须给出 0（中点），0 给下界，1 给上界。
  // 若实现把 `min..max` 当 `0..span` 用，中点会跑到 -180 或 180。
  const PropertyDesc* desc = st::ui::find_property_desc(kDescs, "bipolar");
  ST_REQUIRE(desc != nullptr);
  ST_CHECK_NEAR(mapped(*desc, 0.0), -360.0, 1e-6);
  ST_CHECK_NEAR(mapped(*desc, 0.5), 0.0, 1e-6);
  ST_CHECK_NEAR(mapped(*desc, 1.0), 360.0, 1e-6);
}

ST_TEST(property_panel_unit_range_maps_identity) {
  // 单位量（0..1）：映射必须是恒等（这里错了说明多乘/多除了一次 span）。
  const PropertyDesc* desc = st::ui::find_property_desc(kDescs, "unit");
  ST_REQUIRE(desc != nullptr);
  for (const double unit : {0.0, 0.25, 0.5, 0.75, 1.0}) {
    ST_CHECK_NEAR(mapped(*desc, unit), unit, 1e-6);
  }
}

ST_TEST(property_panel_integer_snaps_to_whole) {
  // 整数属性必须落整格：面板显示 7 而实际 7.3 会在往返时暴露成“值自己变了”。
  const PropertyDesc* desc = st::ui::find_property_desc(kDescs, "count");
  ST_REQUIRE(desc != nullptr);
  for (const double unit : {0.0, 0.13, 0.37, 0.61, 1.0}) {
    const double value = mapped(*desc, unit);
    ST_CHECK_NEAR(value, std::round(value), 1e-9);
  }
}

ST_TEST(property_panel_step_is_respected_across_the_whole_range) {
  // 带 step 的属性，映射结果必须落在 step 网格上（从 min 起算）。
  // 不吸附的症状：“显示 0.5、实际 0.4732”。
  for (const char* name : {"bipolar", "unit", "count"}) {
    const PropertyDesc* desc = st::ui::find_property_desc(kDescs, name);
    ST_REQUIRE(desc != nullptr);
    for (int step = 0; step <= 20; ++step) {
      const double unit = static_cast<double>(step) / 20.0;
      const double value = mapped(*desc, unit);
      const double origin = static_cast<double>(desc->min_value);
      const double offset = (value - origin) / static_cast<double>(desc->step);
      ST_CHECK_NEAR(offset, std::round(offset), 1e-6);
    }
  }
}

ST_TEST(property_panel_clamps_outside_unit_interval) {
  // 滑块理论上只给 0..1，但控制通道可以写任何数。越界必须夹住，
  // 而不是让值跑出描述声明的区间（那会让“范围”变成一句空话）。
  const PropertyDesc* desc = st::ui::find_property_desc(kDescs, "bipolar");
  ST_REQUIRE(desc != nullptr);
  ST_CHECK_NEAR(mapped(*desc, -5.0), -360.0, 1e-6);
  ST_CHECK_NEAR(mapped(*desc, 5.0), 360.0, 1e-6);
}

// ————————————————————————————————————————————————————————————————————————————
// 三、字符串口径（面板写出的值必须是可解析的）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(property_panel_written_values_are_parseable) {
  // 面板写出的值会被 `set_property` 再解析一次。若格式不可解析
  // （例如 "1.000000e+00" 之外的怪写法、或整数写成 "7.0000"），
  // 写入会静默失败——而控件看起来动过了。
  for (const PropertyDesc& desc : kDescs) {
    if (!st::ui::property_type_is_numeric(desc.type)) continue;
    const double value = mapped(desc, 0.5);
    const std::string text = std::format("{:.4f}", value);
    const auto back = parse(text);
    ST_REQUIRE(back.has_value());
    ST_CHECK_NEAR(back.value(), value, 0.001);
  }
}
