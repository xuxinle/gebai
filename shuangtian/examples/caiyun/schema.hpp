#pragma once

/// 裁云 · **脚本字段 schema**（单一真值表）。
///
/// ## 为什么这张表必须存在
///
/// 裁云脚本有上百个可写键（仅三维就有三十多个）。在此之前，字段的存在性只体现在
/// `director.cpp` 里一行行 `st::json_get_*(node, "名字", ...)` 调用中——**没有任何东西
/// 能让程序（或写脚本的智能体）回答"这个字段存在吗、范围是多少、默认是什么"**。
///
/// 实测代价：往三维脚本里写 `model_rot` / `camera_azimuth` / `light_rimm` 三个拼错的
/// 键，程序照常出片、日志零提示。而"球没有自转"与"我写对了但转速是 0"在画面上
/// 完全一样——这类错误只能靠人盯着画面猜。
///
/// ## 这张表的三项用途，都必须走同一个入口
///
/// 1. **校验**：解析时用它判定"这个键属于这一层吗""值在范围内吗"；
/// 2. **诊断**：未知键给出编辑距离最近的候选（`model_rot` → `model_spin`）；
/// 3. **自省**：经控制通道导出成 JSON schema，智能体无需读源码即可知道全部字段。
///
/// 三者共用一份数据，是因为**分散写必然漂移**——而漂移的后果是"校验说没问题、
/// 导出里却没有这个字段"，这类矛盾比缺字段更难查。
///
/// ## 与其维护两份真相，不如让表覆盖自己（`schema_coverage`）
///
/// 这张表可能与解析代码脱节：新加一个字段写了解析却忘了登记。所以配套
/// `layer_keys_read_by_parser()` 之类的探针，由测试断言"解析读到的每个键都在表里"。
/// 这是把"两处真相必须一致"变成**可执行的断言**，而不是一条写着遵守的约定。

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace caiyun {

/// 字段值的类型。用于校验（"类型不符"可诊断）与自省（智能体该写数字还是字符串）。
enum class SchemaType : std::uint8_t {
  Number,   ///< 任意数值（设计像素、角度、时长…）
  Integer,  ///< 整数（段数、粒子数、枚举序号）
  Bool,
  String,   ///< 自由字符串（文案、素材引用、模型形状）
  Color,    ///< `#rrggbb` 或 `#rrggbbaa`
  Dimension, ///< 数字（设计像素）或 `"50%"` 字符串——两种写法都合法
  Array,    ///< 数组（points / lines / values / tracks）
  Object,   ///< 对象（tracks 的按属性名分组写法）
};

[[nodiscard]] auto schema_type_name(SchemaType type) -> std::string_view;

/// 字段所属的作用域。
///
/// 为何要区分：同一个键名在不同层含义不同（`x` 在图层里是位置，在脚本顶层不存在），
/// 而"作用域写错"是智能体最常犯的一类错（把 `cam_fov` 写到场景上）。
enum class SchemaScope : std::uint8_t {
  Script,   ///< 顶层
  Audio,    ///< `audio` 对象内
  Scene,    ///< `scenes[]` 的每一项
  Layer,    ///< `layers[]` 的每一项
  KeyFrame, ///< `tracks` 里每个关键帧 `{t, v, ease}`
  ObjEntry, ///< `tracks` 的数组写法 `{property, keys}`
};

[[nodiscard]] auto schema_scope_name(SchemaScope scope) -> std::string_view;

/// 一个字段的完整描述。
///
/// 数值范围用 `min_value` / `max_value` 且**只在 `has_range` 为真时生效**：
/// 有些字段刻意不限（`model_spin` 允许负值表示反向转，`cam_azimuth` 允许
/// 超过 360 连续累加），用哨兵值表达"无限制"会让合法取值被误判。
struct FieldSpec {
  /// 字段名（JSON 里的键，逐字一致）
  std::string_view name{};
  /// 所属层
  SchemaScope scope{SchemaScope::Layer};
  SchemaType type{SchemaType::Number};
  /// 是否真的有范围（false = 任意值）
  bool has_range{false};
  /// 允许的下界（`has_range` 为真时有效）
  double min_value{0.0};
  /// 允许的上界（`has_range` 为真时有效）
  double max_value{0.0};
  /// 是否在数值越界时**夹取**（解析器当前的实际行为）。
  ///
  /// 为何需要它：夹取是"失败安全"，但静默夹取会让智能体以为自己写对了。
  /// 表里登记后，诊断可以把"你写了 200，已按 96 处理"如实说出去——
  /// 既保住宽容，又不隐瞒。
  bool clamped{true};
  /// 一句说明（给智能体看的话，简明到能直接进提示词）
  std::string_view summary{};
  /// 编辑距离候选是否可提示（少数键如 `tracks` 的子键不做建议）
  bool suggestable{true};
  /// 常见**同义写法**（最多 3 个；空串表示无）。
  ///
  /// ## 为何需要别名（纯编辑距离不够）
  ///
  /// 编辑距离只能抓**拼写错**，抓不到**语义错**。实测：把 `model_spin`（自转速度）
  /// 写成 `model_rot`（旋转），它与 `model_bob`（上下浮动）的编辑距离更近——
  /// 纯距离算法推荐了 `model_bob`，**改完画面仍然不对**，比不给建议更浪费时间。
  ///
  /// 而 `camera_azimuth` 那类前缀错又会把单字母字段（`h`）拉进来：
  /// 子串启发在短名上必错。所以：**别名命中优先且确定**，
  /// 子串启发只在双方名长足够时参与，编辑距离兜底。
  std::array<std::string_view, 3> aliases{};
};

/// 全部字段（脚本 / 音频 / 场景 / 图层 / 关键帧）。
[[nodiscard]] auto all_field_specs() -> const std::vector<FieldSpec>&;

/// 按作用域取字段（自省与校验都从这里进，避免各自过滤出错）。
[[nodiscard]] auto fields_in_scope(SchemaScope scope) -> std::vector<const FieldSpec*>;

/// 按名与作用域查字段；未登记返回 `nullptr`。
[[nodiscard]] auto find_field_schema(SchemaScope scope, std::string_view name)
    -> const FieldSpec*;

/// 在作用域内找**最相近**的已知字段名（编辑距离 + 前缀/子串启发）。
///
/// 返回空表示"差得太远，不建议"——胡乱建议比不给出错（智能体会照着改错）。
/// 判据：距离 ≤ `max_distance`，且明显优于第二候选；否则返回空。
[[nodiscard]] auto suggest_field(SchemaScope scope, std::string_view unknown)
    -> std::string_view;

/// 两个串的 Levenshtein 距离（暴露出来供测试钉住建议质量）。
[[nodiscard]] auto edit_distance(std::string_view a, std::string_view b) -> std::size_t;

/// 校验一条诊断记录。
struct Diagnostic {
  /// `error` = 必须修（脚本语义已错）；`warning` = 已按兜底处理但需知情。
  bool is_error{false};
  /// 出问题的位置（`scenes[0].layers[2]`），便于智能体直接定位。
  std::string path{};
  /// 出问题的键（未知键/越界键）
  std::string key{};
  /// 可读说明（含建议名与实际生效值）
  std::string message{};
  /// 建议改用的字段名（空 = 无建议）
  std::string suggestion{};

  /// 单行渲染（日志/控制通道共用同一段文本，避免两处措辞分叉）
  [[nodiscard]] auto to_string() const -> std::string;
};

/// 将一个未知键变成诊断（带候选建议）。
[[nodiscard]] auto unknown_key_diagnostic(SchemaScope scope, std::string_view key,
                                          std::string_view path) -> Diagnostic;

/// 将一个越界值变成诊断（说明原值、生效值、合法范围）。
[[nodiscard]] auto out_of_range_diagnostic(const FieldSpec& spec, double written, double applied,
                                           std::string_view path) -> Diagnostic;

/// 类型不符诊断（例如给数字字段写了字符串）。
[[nodiscard]] auto wrong_type_diagnostic(const FieldSpec& spec, std::string_view actual_type,
                                         std::string_view path) -> Diagnostic;

/// 把 schema 导成 JSON（智能体的自省入口）。
///
/// `data.compare_diagnostics` 也会用它做"表本身是否自洽"的检查。形状：
/// `{"version":1,"scopes":{"layer":[{"name":"model_spin","type":"number",...}]}}`
[[nodiscard]] auto schema_to_json() -> std::string;

/// 建议的最大编辑距离（随未知串长度缩放：长键允许更多打字错误）。
[[nodiscard]] auto suggestion_distance_limit(std::size_t key_length) -> std::size_t;

}  // namespace caiyun
