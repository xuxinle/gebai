/// 裁云图层属性描述表 + 由描述生成面板：契约测试。
///
/// ## 这组用例钉的是什么
///
/// 借鉴 Blender RNA 的那条线（属性自省 → 面板自动生成）只有在**两份真相不漂移**
/// 时才有价值。漂移的两种形态，都会表现为“这个控件好像没什么用”：
///
/// · 描述里写 `min=0, max=1`，但 `set_property` 里夹的是别的范围
///   → 面板拖到端点与脚本写 999 得到不同结果，而两边都“成功”；
/// · 描述里有的名字，`get/set` 里没有
///   → 面板生成了一个点了没反应的控件。
///
/// 所以这里钉的是**表与实现的一致性**，不是某个具体数值。

#include "examples/caiyun/layer_schema.cpp"

#include "st/core/print.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"

namespace {

using caiyun::Layer;

}  // namespace

ST_TEST(layer_schema_is_self_consistent) {
  // 重名、反向区间、step 大于区间、choice 无候选…… 全部不该出现。
  // 这些错误**不会崩**，只会生成一个拖到底也不动的控件。
  const auto problem = st::ui::validate_property_schema(caiyun::layer_property_schema());
  if (problem.has_value()) st::log::out("[layer-schema] {}\n", problem.value());
  ST_CHECK(!problem.has_value());
  ST_CHECK(caiyun::layer_property_schema().size() > 10);
}

ST_TEST(layer_schema_every_name_is_readable_and_writable) {
  // 表里有的名字，`get` 必须能读、`set` 必须能写。
  //
  // 反例（真实会犯）：往表里加一行 `cam_roll`，忘了在 `get_layer_property`
  // 加分支 → 面板生成了控件，读回永远是空、写入永远 false，
  // 而**界面看起来完全正常**。
  Layer layer{};
  for (const st::ui::PropertyDesc& desc : caiyun::layer_property_schema()) {
    const auto read = caiyun::get_layer_property(layer, desc.name);
    ST_CHECK(read.has_value());

    // 写一个“该类型必然合法”的值，验证 set 认得这个名字。
    std::string probe;
    switch (desc.type) {
      case st::ui::PropertyType::Number:
      case st::ui::PropertyType::Integer:
        probe = std::format("{:.4f}", static_cast<double>(desc.min_value));
        break;
      case st::ui::PropertyType::Boolean: probe = "true"; break;
      case st::ui::PropertyType::Choice:
        probe = std::string(desc.choices.front());
        break;
      case st::ui::PropertyType::Color: probe = "#3366ff"; break;
      case st::ui::PropertyType::Text:
      case st::ui::PropertyType::Multiline: probe = "probe"; break;
    }
    ST_CHECK(caiyun::set_layer_property(layer, desc.name, probe));
  }
}

ST_TEST(layer_schema_rejects_unknown_names) {
  Layer layer{};
  ST_CHECK(!caiyun::get_layer_property(layer, "definitely_not_a_property").has_value());
  ST_CHECK(!caiyun::set_layer_property(layer, "definitely_not_a_property", "1"));
  // `kind` 故意不在表里：改类型等于换一个图层，不是“改一个参数”。
  ST_CHECK(!caiyun::get_layer_property(layer, "kind").has_value());
}

ST_TEST(layer_schema_clamps_with_the_same_range_it_declares) {
  // **契约纪律 1 的可执行形式**：写入后的值必须落在描述声明的区间内。
  // 若实现里另写一套 clamp，这里会红——而那正是“两处真相”的起点。
  Layer layer{};
  for (const st::ui::PropertyDesc& desc : caiyun::layer_property_schema()) {
    if (!st::ui::property_type_is_numeric(desc.type)) continue;
    // 写一个远超上界的值
    ST_CHECK(caiyun::set_layer_property(layer, desc.name, "1000000"));
    const auto read = caiyun::get_layer_property(layer, desc.name);
    ST_REQUIRE(read.has_value());
    double value = 0.0;
    std::size_t consumed = 0;
    try {
      value = std::stod(read.value(), &consumed);
    } catch (const std::exception&) {
      ST_CHECK(false);
      continue;
    }
    ST_CHECK_NEAR(consumed, read.value().size(), 0.0);
    ST_CHECK(value <= static_cast<double>(desc.max_value) + 1e-3);
    ST_CHECK(value >= static_cast<double>(desc.min_value) - 1e-3);
  }
}

ST_TEST(layer_schema_step_is_respected) {
  // 带 step 的属性写入后必须落在 step 网格上（面板拖到哪就是哪）。
  // 不吸附的症状是“显示 0.5、实际 0.4732”，下次读回时“值自己变了”。
  Layer layer{};
  ST_CHECK(caiyun::set_layer_property(layer, "opacity", "0.4732"));
  const auto read = caiyun::get_layer_property(layer, "opacity");
  ST_REQUIRE(read.has_value());
  const double value = std::stod(read.value());
  ST_CHECK_NEAR(value, 0.47, 0.011);  // step 0.01
}

ST_TEST(layer_schema_color_round_trips) {
  Layer layer{};
  ST_CHECK(caiyun::set_layer_property(layer, "fill", "#3366ff"));
  const auto read = caiyun::get_layer_property(layer, "fill");
  ST_REQUIRE(read.has_value());
  ST_CHECK_EQ(read.value(), std::string("#3366ff"));
  // 三位简写与带 alpha 的八位都支持（脚本与人工输入都会给）
  ST_CHECK(caiyun::set_layer_property(layer, "fill", "#f00"));
  ST_CHECK_EQ(caiyun::get_layer_property(layer, "fill").value(), std::string("#ff0000"));
  ST_CHECK(caiyun::set_layer_property(layer, "fill", "#3366ff80"));
  ST_CHECK_EQ(caiyun::get_layer_property(layer, "fill").value(), std::string("#3366ff80"));
  // 非法颜色必须被拒（而不是静默变成黑）
  ST_CHECK(!caiyun::set_layer_property(layer, "fill", "not-a-color"));
  ST_CHECK(!caiyun::set_layer_property(layer, "fill", "#12345"));
}

ST_TEST(layer_schema_choice_round_trips) {
  Layer layer{};
  ST_CHECK(caiyun::set_layer_property(layer, "align", "right"));
  ST_CHECK_EQ(caiyun::get_layer_property(layer, "align").value(), std::string("right"));
  ST_CHECK(caiyun::set_layer_property(layer, "gradient_dir", "horizontal"));
  ST_CHECK_EQ(caiyun::get_layer_property(layer, "gradient_dir").value(),
              std::string("horizontal"));
  // 不在候选里的值必须被拒（否则面板会显示一个“没有选中项”的下拉）
  ST_CHECK(!caiyun::set_layer_property(layer, "align", "diagonal"));
}

ST_TEST(layer_schema_json_lists_every_property) {
  // 智能体经控制通道拿到的就是这份 JSON——它必须覆盖整张表。
  const std::string text = caiyun::layer_schema_json();
  const auto parsed = st::json_parse(text);
  ST_REQUIRE(parsed.has_value());
  const st::Json& list = parsed.value();
  ST_REQUIRE(list.is_array());
  ST_CHECK_EQ(list.size(), caiyun::layer_property_schema().size());
  for (const st::Json& item : list) {
    ST_CHECK(item.contains("name"));
    ST_CHECK(item.contains("type"));
    // 数值属性必须带范围：面板靠它决定滑杆量程
    if (item.value("type", std::string{}) == "number" ||
        item.value("type", std::string{}) == "integer") {
      ST_CHECK(item.contains("min") && item.contains("max"));
    }
  }
}
