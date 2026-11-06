/// 意图层（`intent.md`）：这个片子为什么存在、按什么标准验收。
///
/// ## 为什么需要结构化，而不只是一段 markdown
///
/// 纯 markdown 是**写给人看的**——AI 每次打开工程都要重新读、重新理解，
/// 而"这段意图里哪句是硬约束"没有任何标记。实测现象：同一个工程被两个会话
/// 接手，第一个把"6 秒"当建议（渲了 9 秒），第二个当硬约束——
/// 因为意图里写的就是"约 6 秒"。
///
/// 所以意图层分两半：
///
/// - **人读区**：自由 markdown（说明、参考、取舍理由）；
/// - **机器读区**：`<!-- caiyun:intent -->` 标记之间的一段 JSON，
///   写明 `duration_s` / `size` / `must_show` 这类**可校验**的约束。
///
/// ```markdown
/// # 意图
/// 给内部技术分享用的 6 秒开场。
///
/// <!-- caiyun:intent
/// {"duration_s": 6.0, "tolerance_s": 0.5,
///  "size": [1920, 1080],
///  "must_show": ["产品名", "三个要点"],
///  "tone": "冷静，不要炫技"}
/// -->
/// ```
///
/// ## 为什么用 HTML 注释包 JSON
///
/// 三种做法都试过权衡：
/// - 独立 `intent.json`：人写意图时要同时改两个文件，必然漂移；
/// - YAML front matter：要引入 YAML 解析（够用但多一个依赖与一套语法）；
/// - **HTML 注释包 JSON**：markdown 渲染器不显示它、人可以直接写、
///   解析器只要找一对标记。markdown 里塞 JSON 的通行做法。
///
/// 标记缺失时**不是错误**：意图层可以先只有人读区，机器读区按需补上。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/json.hpp"

namespace caiyun {

// 前向声明：本层只是“拿脚本核对意图”，不需要 Script 的完整定义。
// 反向包含 director.hpp 会形成循环（director.hpp 将来要包含本层以输出偏差）。
struct Script;

using st::Result;

/// 机器可读的意图约束。
///
/// 全部字段可选：只写关心的那几条。未写的项不参与校验
/// （而不是"取默认值硬套"——那会让一个没写时长的工程被 10 秒限制判失败）。
struct IntentSpec {
  /// 目标时长（秒）。`nullopt` = 未声明。
  std::optional<double> duration_s{};
  /// 时长容差（秒），默认 0.5。只在 `duration_s` 有值时生效。
  double tolerance_s{0.5};
  /// 目标画幅（宽, 高）。`nullopt` = 未声明。
  std::optional<std::pair<int, int>> size{};
  /// **必须出现的元素**（按文案或图层名做子串匹配）。
  std::vector<std::string> must_show{};
  /// 语气/风格的一句话（人读；不参与自动校验）。
  std::string tone{};
  /// 原始 JSON（便于原样写回与展示）。
  st::Json raw{};
};

/// 一条意图与实现不符的提示。
///
/// 为何不复用 `Diagnostic`：诊断说的是"脚本错了"（必须修），
/// 而意图偏差说的是"和当初说好的不一样"（可能是**有意**变更）。
/// 两者混在一起会让 `--strict` 把合理的创作调整判成错误。
struct IntentDeviation {
  /// 哪个约束（`duration_s` / `size` / `must_show`）。
  std::string constraint{};
  /// 实际值（人能读的形态）。
  std::string actual{};
  /// 期望值。
  std::string expected{};
  /// 一句说明（含"这条可能是有意变更"的提示）。
  std::string message{};

  [[nodiscard]] auto to_string() const -> std::string;
};

/// 解析 `intent.md` 全文。
///
/// 标记缺失 → 返回空的 `IntentSpec`（不是错误）。
/// 标记在但 JSON 坏 → **错误**：静默忽略会让"我写了约束但没生效"极难查。
[[nodiscard]] auto parse_intent(std::string_view markdown) -> Result<IntentSpec>;

/// 把 `IntentSpec` 写回成带标记的 markdown 片段（供创建包时生成骨架）。
[[nodiscard]] auto render_intent_block(const IntentSpec& spec) -> std::string;

/// 拿意图约束去核对一份脚本，返回**偏差清单**（空 = 全部符合）。
///
/// 只核对能自动判定的项：时长、画幅、必须出现。语气不判。
[[nodiscard]] auto check_intent(const IntentSpec& spec, const Script& script)
    -> std::vector<IntentDeviation>;

}  // namespace caiyun
