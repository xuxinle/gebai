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
  /// JS 编排层：**两种写法都合法**——文件路径字符串，或 `{"source": ...}` 内联对象。
  ///
  /// 为何要单独一个类型（而不是复用 String）：内联写法一直存在
  /// （`parse_layout_script` 两种都收），但 schema 层只声明了 String，
  /// 于是**拍平后的自包含脚本会被 --strict 判为类型错误**。
  /// 实测代价：七套模板迁移后从包拍平再渲染，三条 layout 全被判 error。
  Layout,
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
  /// 三维场景里的 `camera` 对象（`objects[]` 的兄弟节点）。
  Camera,
  /// 三维场景里的 `lighting` 对象。
  ///
  /// ## 为什么这两个要单独成 scope
  ///
  /// 它们是**嵌套对象**，字段集与图层/layer 完全不同（`eye`/`target`/`fov` 与
  /// `key_direction`/`ambient`/`extra_lights`）。混进 `Layer` 会让
  /// 未知字段检查用错表——实测就是这样：`lighting.extra_lights` 是合法字段，
  /// 却因为没有 Lighting scope 被报成"未知字段"。
  Lighting,
  /// 三维场景里的 `shadow` 对象。
  Shadow,
  /// 三维场景里的 `objects[]` 每一项。
  Object3d,
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

/// `--strict` 下该被拦的诊断条数（纯函数，供渲染入口与回归测试共用）。
///
/// ## 为何不直接用 `Diagnostic::is_error`
///
/// 未知字段的 `is_error` **有意为 false**（`unknown_key_diagnostic` 的注释：
/// “宽容：未知键不阻断加载，老脚本仍要能出片”）。那个默认是对的——
/// 手工调参时写个未登记字段不该被拦。
///
/// 但 `--strict` 的语义正好相反：它要在 CI/智能体场景下把“静默忽略”
/// **变成失败**。而**字段名写错恰好是典型场景**（实测：`model_rot` /
/// `camera_azimuth` 两个拼错的键，旧行为照常出片、日志零提示，
/// 而“球没自转”与“写对了但转速是 0”在画面上完全一样）。
///
/// 所以这里按**事实**（未知键 / 类型错）判，而不是按那个宽容的默认标位。
[[nodiscard]] auto strict_violation_count(const std::vector<Diagnostic>& list) -> std::size_t;

/// 把字段表导成**标准 JSON Schema**（draft-2020-12）。
///
/// ## 为何要另外导出一份（已有 `schema_to_json` 还不够）
///
/// `schema_to_json` 是**自省面**：形状按我们的作用域分（script/audio/scene/...），
/// 是给人/智能体**读**的。而这一份是**机器契约**：喂给约束解码（structured outputs /
/// GBNF / Outlines / XGrammar）后，模型在**生成时**就写不出错误字段名——
/// 这是唯一能真正消灭拼写/类型错误的机制（校验只能事后抓）。
///
/// 两份都从同一张 `FieldSpec` 表生成，不会漂移。
///
/// ## 映射规则
///
/// - 每个作用域一个 `$defs` 条目，`additionalProperties: false` —— 写错键即失败；
/// - `has_range` → `minimum`/`maximum`；`Integer` 额外加 `type: integer`；
/// - `Dimension` → `oneOf [number, string]`（裁云接受 `640` 与 `"50%"`）；
/// - `Color` → `pattern: ^#[0-9a-fA-F]{6,8}$`；
/// - `aliases` → `deprecated` 提示写进 `description`（不列入 properties：
///   别名字段本来就不是合法键，列进去反而会让 schema 认下拼错的写法）。
[[nodiscard]] auto schema_to_json_schema() -> std::string;

/// 把 schema 导成 JSON（智能体的自省入口）。
///
/// `data.compare_diagnostics` 也会用它做"表本身是否自洽"的检查。形状：
/// `{"version":1,"scopes":{"layer":[{"name":"model_spin","type":"number",...}]}}`
/// **未知字段检查**：对象里出现了该 scope 不认识的键就报错。
///
/// ## 为什么裁云也必须做这件事
///
/// 三维场景脚本已经在做（`scene_schema.hpp`），但裁云的剧本脚本没有。
/// 结果是同一个病：`{"kind":"text","sise":18}` 里的 `sise`（拼错 `size`）
/// 会被静默忽略，元素用默认字号画出来——作者看到"渲染成功"，
/// 只有看图才发现字号不对。
///
/// `Aliases` 机制已经在（`size`/`scale` 等多种写法都认），
/// 所以这里只需要对比"对象里的键"与"schema 认识的键（含别名）"。
///
/// @param keys  要检查的对象里出现了哪些键
/// @param scope  它属于哪一层（决定用哪张字段表）
/// @param path   报错时用的定位（如 `scenes[0].layers[3]`）
/// @return 全部合法时 ok；否则返回第一个未知字段的错误，消息里带最接近的候选。
/// @param keys 该对象里出现的全部键（由调用方从自己的 JSON 类型取，避免耦合）
[[nodiscard]] auto check_unknown_fields(const std::vector<std::string>& keys, SchemaScope scope,
                                        std::string_view path) -> std::string;

[[nodiscard]] auto schema_to_json() -> std::string;

/// 建议的最大编辑距离（随未知串长度缩放：长键允许更多打字错误）。
[[nodiscard]] auto suggestion_distance_limit(std::size_t key_length) -> std::size_t;

}  // namespace caiyun
