#pragma once

/// 霜天 · **属性描述（PropertyDesc）契约**：让「一个组件有哪些可调参数」变成可被程序读到的数据。
///
/// ## 为什么需要它（而这个框架已经有 `property_names()` 了）
///
/// `Element::property_names()` 只回答「**名字**是什么」——它能支撑
/// `get`/`set`/`invoke` 这条控制通道，但回答不了生成界面的那三个问题：
///
/// · 这个属性是数字还是布尔？→ 不知道，只能把字符串硬塞进 `set_property` 试；
/// · 数字的合法区间？→ 不知道，写 500 与写 5 都会被接受（后者可能没有任何差别）；
/// · 它属于哪个分组、该怎么显示？→ 不知道，所以每个面板只能手写一遍控件。
///
/// 结果是**同一份参数被写了两遍**：一次在组件的 `get/set_property` 里，
/// 一次在某个面板的 `build()` 里。加一个参数要改两处，漏一处就是
/// 「能设但界面上没有」或者「界面上有但设了不生效」——两者都只表现为
/// 「这个控件好像没什么用」，极难定位。
///
/// ## 借鉴的模型：数据驱动的属性自省
///
/// 成熟的三维/影视工具（Blender 的 RNA、Houdini 的 parameter template、Nuke 的 knob）
/// 都是**一份元数据同时驱动三件事**：程序化读写、自动生成属性面板、脚本/外部工具自省。
/// 本契约把这三件事收敛到同一个 `PropertyDesc` 列表上：
///
/// ```cpp
/// class Slider : public Element {
///  public:
///   auto property_schema() const -> std::span<const PropertyDesc> override {
///     static constexpr PropertyDesc kDescs[] = {
///       {.name = "value", .type = PropertyType::Number, .min_value = 0.0f,
///        .max_value = 1.0f, .step = 0.01f, .summary = "当前值"},
///     };
///     return kDescs;
///   }
/// };
/// ```
///
/// ## 三条纪律
///
/// 1. **描述必须与 `set_property` 的实现同源**：`min_value`/`max_value` 应直接用于
///    夹取，而不是描述里写一套、实现里夹另一套（那等于又造了一份真相）。
/// 2. **只描述"能被程序读写"的属性**：纯视觉内部量（如缓存的布局结果）不进表。
/// 3. **不改 `Element` 的虚函数面**：本契约是**可选**覆写（默认返回空表），
///    所以存量组件零改动、零开销——渐进采用，而不是一次性改造全部控件。

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace st::ui {

/// 属性值的类型。决定面板该生成什么控件，也决定 `set_property` 该接受什么文本。
enum class PropertyType : std::uint8_t {
  Text,    ///< 任意文本（单行输入框）
  Multiline,  ///< 多行文本（文本域）
  Number,  ///< 浮点数（滑块/数值输入）
  Integer, ///< 整数（步进器/数值输入）
  Boolean, ///< 布尔（开关）
  Choice,  ///< 枚举中的一项（下拉；取值见 `PropertyDesc::choices`）
  Color,   ///< `#rrggbb` / `#rrggbbaa`
};

[[nodiscard]] constexpr auto property_type_name(PropertyType type) noexcept -> std::string_view {
  switch (type) {
    case PropertyType::Text: return "text";
    case PropertyType::Multiline: return "multiline";
    case PropertyType::Number: return "number";
    case PropertyType::Integer: return "integer";
    case PropertyType::Boolean: return "bool";
    case PropertyType::Choice: return "choice";
    case PropertyType::Color: return "color";
  }
  return "text";
}

/// 由类型推断：该类型是否用数值范围。
[[nodiscard]] constexpr auto property_type_is_numeric(PropertyType type) noexcept -> bool {
  return type == PropertyType::Number || type == PropertyType::Integer;
}

/// 一个属性的完整描述。
///
/// 设计成**聚合可初始化**（`PropertyDesc{.name = ..., .type = ...}`）：
/// 组件里通常用 `static constexpr PropertyDesc kDescs[]` 声明，
/// 命名成员初始化能让"我漏填了哪个字段"在编译期就暴露（顺序错了也不会静默错位）。
struct PropertyDesc {
  /// 属性名（与 `get_property`/`set_property` 收到的字符串逐字一致）
  std::string_view name{};
  /// 值的类型
  PropertyType type{PropertyType::Text};
  /// 数值下界（仅 `property_type_is_numeric(type)` 时有意义）
  float min_value{0.0f};
  /// 数值上界
  float max_value{0.0f};
  /// 面板步进（滑块/步进器的粒度；0 = 不指定）
  float step{0.0f};
  /// 一句说明（给面板的悬浮提示，也给智能体读）
  std::string_view summary{};
  /// `Choice` 的候选项（其他类型忽略）。空表示自由文本。
  std::span<const std::string_view> choices{};
  /// 是否只读（`get` 能读、`set` 不生效）。
  ///
  /// 与 `Element::readonly_property_names()` 的关系：那里是**名字**列表（存量接口），
  /// 这里是**完整描述**。两者若都实现，名字必须一致——`schema_conflicts_with_names()`
  /// 提供可执行的核对，避免"描述说只读、名字表说可写"这类静默矛盾。
  bool readonly{false};
  /// 是否在面板上默认折叠（次要参数；智能体仍可读写）
  bool advanced{false};
};

/// 校验一组描述内部是否自洽（重名、范围反向、数值字段缺范围）。
///
/// 返回第一个问题的人话描述；全部通过返回 `nullopt`。
/// 为何要有它：这些错误**不会崩、不会报错**，只会让面板生成出一个"拖到底也不动"
/// 的控件——比缺功能更难查。
[[nodiscard]] auto validate_property_schema(std::span<const PropertyDesc> descs)
    -> std::optional<std::string>;

/// 把描述列表导成 JSON（面板生成、控制通道自省、测试断言共用）。
///
/// 形状：`[{"name":"value","type":"number","min":0,"max":1,"step":0.01,"summary":"…"}]`
[[nodiscard]] auto property_schema_to_json(std::span<const PropertyDesc> descs) -> std::string;

/// 按名查描述；未登记返回 `nullptr`。
[[nodiscard]] auto find_property_desc(std::span<const PropertyDesc> descs, std::string_view name)
    -> const PropertyDesc*;

/// 一个属性在给定输入文本下**应当被夹取到**的值（数值类型）。
///
/// 存在的理由：面板与人手输入都会给出越界值（拖滑块到端点、粘贴 "999"）。
/// 夹取逻辑只能有一份——放在这里，组件实现与面板生成器都调它，
/// 而不是各自写一次 `std::clamp`（那是最典型的"两处真相"）。
[[nodiscard]] auto clamp_property_value(const PropertyDesc& desc, double raw) -> double;

/// 把描述的**真实量程**与滑块的 `[0,1]` 单位值双向换算。
///
/// ## 为何需要它（而不是在面板里现算）
///
/// `Slider` 的值域是固定的 `[0,1]`（公共组件契约，不因一个面板而改），
/// 而属性描述的区间是真实量程（`-360..360`、`0..400`）。两者之间的换算是
/// **面板最容易错的地方**：把 `min..max` 当 `0..span` 用，中点就跑到 −180 或 180；
/// 方向写反，拖到底写进 0。而控件本身看起来完全正常。
///
/// 抽成两个纯函数而不是写在 `build_property_panel` 的回调里，是为了
/// **测试能直接验证真实实现**（否则只能在测试里再抄一遍公式——
/// 那是“实现错、抄件照着错”的自证）。
struct PropertyRangeMap {
  double min{0.0};
  double max{1.0};
};

[[nodiscard]] auto property_range_of(const PropertyDesc& desc) -> PropertyRangeMap;

/// 真实值 → 滑块单位值（`[0,1]`）。
[[nodiscard]] auto property_to_unit(const PropertyRangeMap& range, double value) -> float;

/// 滑块单位值 → 真实值（**含 step 吸附与夹取**）。
///
/// 吸附从 `min` 起算：保证 `min` 本身永远是一个合法停点
/// （若从 0 起算，`min=0.3` / `step=0.2` 时 `min` 就取不到）。
[[nodiscard]] auto property_from_unit(const PropertyRangeMap& range, const PropertyDesc& desc,
                                      double unit) -> double;

}  // namespace st::ui
