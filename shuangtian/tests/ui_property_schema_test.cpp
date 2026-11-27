/// 霜天「属性描述契约」的单测（`include/st/ui/property_schema.hpp`）。
///
/// ## 这组用例钉的是什么
///
/// 契约的唯一价值是「**一份元数据同时驱动读写、面板生成、外部自省**」。
/// 一旦它自己不自洽（重名、范围反向、step 大于区间、整数不落格），
/// 面板会生成一个"拖到底也不动"的控件——**不崩、不报错**，
/// 只表现为"这个参数好像没什么用"，是最难定位的一类缺陷。
///
/// 所以这里重点钉三件事：
/// 1. `validate_property_schema` 能抓住每种不自洽（且**不会误报合法描述**）；
/// 2. `clamp_property_value` 是唯一的夹取入口，行为对整数/步进/NaN 都确定；
/// 3. 导出的 JSON 形状稳定（面板生成与外部工具都按它解析）。

#include "st/ui/property_schema.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/components/all.hpp"

namespace {

using st::ui::PropertyDesc;
using st::ui::PropertyType;

/// 一份完全合法的描述集（用于"不误报"的反向断言）。
constexpr std::string_view kChoices[] = {"title", "custom", "outro"};

constexpr PropertyDesc kGoodDescs[] = {
    {.name = "value", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 1.0f,
     .step = 0.01f, .summary = "当前值"},
    {.name = "segments", .type = PropertyType::Integer, .min_value = 3.0f, .max_value = 96.0f,
     .step = 1.0f, .summary = "细分"},
    {.name = "visible", .type = PropertyType::Boolean, .summary = "是否显示"},
    {.name = "kind", .type = PropertyType::Choice, .summary = "版式", .choices = kChoices},
    {.name = "status", .type = PropertyType::Text, .summary = "只读状态", .readonly = true},
};

}  // namespace

ST_TEST(property_schema_accepts_a_valid_description) {
  // 反向护栏：合法描述必须零问题——否则校验会被当成噪音而整体忽略。
  ST_CHECK(!st::ui::validate_property_schema(kGoodDescs).has_value());
}

ST_TEST(property_schema_rejects_duplicate_names) {
  // 重名 → 面板只生成第一个，第二个参数永远调不到（静默）。
  constexpr PropertyDesc descs[] = {
      {.name = "value", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 1.0f},
      {.name = "value", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 2.0f},
  };
  const auto problem = st::ui::validate_property_schema(descs);
  ST_REQUIRE(problem.has_value());
  ST_CHECK(problem->find("value") != std::string::npos);
}

ST_TEST(property_schema_rejects_inverted_range) {
  // 范围反向会让 clamp 把任何值都推到 min → "拖到哪都一样"。
  constexpr PropertyDesc descs[] = {
      {.name = "fov", .type = PropertyType::Number, .min_value = 110.0f, .max_value = 8.0f},
  };
  const auto problem = st::ui::validate_property_schema(descs);
  ST_REQUIRE(problem.has_value());
  ST_CHECK(problem->find("fov") != std::string::npos);
}

ST_TEST(property_schema_rejects_step_larger_than_range) {
  // step 大于整个区间 → 滑块只有一格（看起来像是"没做滑块"）。
  constexpr PropertyDesc descs[] = {
      {.name = "opacity", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 1.0f,
       .step = 5.0f},
  };
  ST_CHECK(st::ui::validate_property_schema(descs).has_value());
}

ST_TEST(property_schema_rejects_choice_without_options) {
  // 空候选的下拉是个永远点不出东西的框。
  constexpr PropertyDesc descs[] = {
      {.name = "kind", .type = PropertyType::Choice},
  };
  ST_CHECK(st::ui::validate_property_schema(descs).has_value());
}

ST_TEST(property_schema_rejects_choices_on_non_choice_type) {
  // 把 candidates 写在数字属性上几乎总是笔误（候选只对 choice 生效），
  // 静默忽略会让"我明明给了选项"变成一个查不出结果的问题。
  constexpr PropertyDesc descs[] = {
      {.name = "value", .type = PropertyType::Number, .min_value = 0.0f, .max_value = 1.0f,
       .summary = "", .choices = kChoices},
  };
  ST_CHECK(st::ui::validate_property_schema(descs).has_value());
}

ST_TEST(property_schema_rejects_empty_name) {
  constexpr PropertyDesc descs[] = {
      {.type = PropertyType::Number, .min_value = 0.0f, .max_value = 1.0f},
  };
  ST_CHECK(st::ui::validate_property_schema(descs).has_value());
}

ST_TEST(property_clamp_respects_range) {
  const auto desc = *st::ui::find_property_desc(kGoodDescs, "value");
  // 容差 1e-6 而非 1e-9：`step` 在描述里是 **float**（0.01f 在二进制下不精确），
  // 吸附后回头比对必然带 1e-8 量级的表示误差。拿 1e-9 去要求是拿错尺子——
  // 它量的是 float↔double 的表示误差，不是夹取逻辑。
  ST_CHECK_NEAR(st::ui::clamp_property_value(desc, 0.5), 0.5, 1e-6);
  ST_CHECK_NEAR(st::ui::clamp_property_value(desc, 2.0), 1.0, 1e-6);
  ST_CHECK_NEAR(st::ui::clamp_property_value(desc, -3.0), 0.0, 1e-6);
}

ST_TEST(property_clamp_snaps_integer_to_whole_steps) {
  // 整数属性必须落整格：显示 3 而实际 3.4 会在往返时暴露成"数值自己变了"。
  const auto desc = *st::ui::find_property_desc(kGoodDescs, "segments");
  ST_CHECK_NEAR(st::ui::clamp_property_value(desc, 3.4), 3.0, 1e-9);
  ST_CHECK_NEAR(st::ui::clamp_property_value(desc, 3.6), 4.0, 1e-9);
  ST_CHECK_NEAR(st::ui::clamp_property_value(desc, 200), 96.0, 1e-9);
}

ST_TEST(property_clamp_snaps_to_step_from_min) {
  // 从 min 起算吸附：保证 min 本身永远是一个合法停点
  // （若从 0 起算，min=0.3/step=0.2 时 min 就取不到）。
  constexpr PropertyDesc descs[] = {
      {.name = "t", .type = PropertyType::Number, .min_value = 0.3f, .max_value = 1.0f,
       .step = 0.2f},
  };
  ST_CHECK_NEAR(st::ui::clamp_property_value(descs[0], 0.3), 0.3, 1e-6);
  ST_CHECK_NEAR(st::ui::clamp_property_value(descs[0], 0.31), 0.3, 1e-6);
  ST_CHECK_NEAR(st::ui::clamp_property_value(descs[0], 0.45), 0.5, 1e-6);
}

ST_TEST(property_clamp_rejects_nan) {
  // NaN 若放过去会静默传播进布局，表现为"元素消失了"而不是报错。
  const auto desc = *st::ui::find_property_desc(kGoodDescs, "value");
  const double result = st::ui::clamp_property_value(desc, std::nan(""));
  ST_CHECK(std::isfinite(result));
  ST_CHECK_NEAR(result, 0.0, 1e-9);  // 落到 min
}

ST_TEST(property_schema_json_shape_is_stable) {
  const std::string text = st::ui::property_schema_to_json(kGoodDescs);
  const auto parsed = st::json_parse(text);
  ST_REQUIRE(parsed.has_value());
  const st::Json& list = parsed.value();
  ST_REQUIRE(list.is_array());
  ST_CHECK_EQ(list.size(), std::size_t{5});
  // 数值属性带范围；布尔/文本不带
  bool found_value = false;
  bool found_visible = false;
  bool found_kind = false;
  for (const st::Json& item : list) {
    const std::string name = item.value("name", std::string{});
    if (name == "value") {
      found_value = true;
      ST_CHECK_EQ(item.value("type", std::string{}), std::string("number"));
      ST_CHECK(item.contains("min") && item.contains("max"));
      ST_CHECK_NEAR(item.value("step", 0.0), 0.01, 1e-9);
    } else if (name == "visible") {
      found_visible = true;
      ST_CHECK(!item.contains("min"));  // 非数值不该有范围
    } else if (name == "kind") {
      found_kind = true;
      ST_CHECK(item.contains("choices"));
      ST_CHECK_EQ(item["choices"].size(), std::size_t{3});
    } else if (name == "status") {
      ST_CHECK(item.value("readonly", false));
    }
  }
  ST_CHECK(found_value);
  ST_CHECK(found_visible);
  ST_CHECK(found_kind);
}

ST_TEST(property_find_desc_returns_null_for_unknown) {
  ST_CHECK(st::ui::find_property_desc(kGoodDescs, "nonexistent") == nullptr);
  ST_CHECK(st::ui::find_property_desc(kGoodDescs, "value") != nullptr);
}

/// **内置组件的描述必须落在它的名字表里**（否则面板生成了控件但控制通道读不到）。
///
/// ## 守的是什么
///
/// `property_schema()` 与 `property_names()` / `readonly_property_names()` 是
/// **两份真相**：一份给自动面板，一份给 `get`/`set`。任何一边多写一个名字，
/// 症状都是“控件在、但拖它不生效”或“能设但界面上没有”——两种都只表现为
/// “这个参数好像没什么用”，不崩不报错。
///
/// 这条测试对**每个登记过的内置组件**做两件事：
/// ① 描述里的每个名字都在两张名字表之一（不是孤儿）；
/// ② 描述自洽（`validate_property_schema`：重名 / 范围反向 / step 越界）。
///
/// 覆盖是**按注册表遍历**的，所以将来新加描述立刻进这条判据，不用改测试。
ST_TEST(component_property_schema_matches_its_name_tables) {
  const std::vector<std::string> names = st::ui::component_registry::names();
  ST_CHECK(names.size() > 10U);   // 注册表没被裁空（自检基线）
  std::vector<std::string> orphans;
  std::vector<std::string> invalid;
  std::size_t with_schema = 0;
  std::size_t described = 0;
  for (const std::string& name : names) {
    std::unique_ptr<st::ui::Element> element = st::ui::component_registry::create(name);
    if (element == nullptr) continue;
    const std::span<const PropertyDesc> descs = element->property_schema();
    if (descs.empty()) continue;
    ++with_schema;
    described += descs.size();
    if (const auto problem = st::ui::validate_property_schema(descs); problem.has_value()) {
      invalid.push_back(std::format("{}: {}", name, *problem));
    }
    const std::vector<std::string_view> writable = element->property_names();
    const std::vector<std::string_view> readonly = element->readonly_property_names();
    for (const PropertyDesc& desc : descs) {
      const bool listed = std::ranges::find(writable, desc.name) != writable.end() ||
                          std::ranges::find(readonly, desc.name) != readonly.end();
      if (!listed) orphans.push_back(std::format("{}.{}", name, desc.name));
    }
  }
  st::log::out("[property-schema] {} 个组件声明了描述，共 {} 项；孤儿 {} 处，不自洽 {} 处\n",
               with_schema, described, orphans.size(), invalid.size());
  for (const std::string& item : orphans) st::log::out("  孤儿: {}\n", item);
  for (const std::string& item : invalid) st::log::out("  不自洽: {}\n", item);
  ST_CHECK(orphans.empty());
  ST_CHECK(invalid.empty());
  // 渐进采用的下限护栏（**进度尺**，不是正确性判据）：防止有人批量删描述。
  //
  // 2026-11-27 从 4 抬到 44：画廊重写那轮把「有运行时语义可调量」的组件
  // （Text/Button/Input/Table/Tree/Tabs/List/CodeEditor/MarkdownView/反馈族/导航族…）
  // 逐个体检并补上描述，覆盖 177 项。
  //
  // 仍未声明的是**刻意留白**：构造参数型（WindowFrame / TitleBar）、
  // 纯注入型（ListItem / ScrollBar / MenuPanel），以及语义还在收敛的少数组件。
  // 加描述时把这里抬上去；降下来必须有理由（比如组件被删）。
  ST_CHECK(with_schema >= 44U);
}
