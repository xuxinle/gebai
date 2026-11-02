#pragma once

/// @file report.hpp
/// **诊断报告的契约**：给"不会推理统计口径的调用方"（大模型/脚本/CI）用的产物。
///
/// ## 为什么不是"一堆数字 + 一篇文档"
///
/// 实测教训（本仓反复出现）：数字摆在那里，读者仍会读错——把 debug 档当 release、
/// 把墙钟间隔当帧内耗时、把 3 个样本的 p95 当结论、把"被限帧"当"渲染很慢"。
/// 人会读错，模型会**更快更自信地**读错（它没有"这次环境不太对"的直觉）。
///
/// 所以报告不是数据转储，而是**三件东西**：
/// | 字段 | 回答什么 |
/// |---|---|
/// | `caveats[]` | **这份数据能不能用来下结论**（档位/后端/DPI/样本量/是否被限帧/时钟精度）。任何一条不满足就明说"本次不可比较" |
/// | `findings[]` | **该动哪里**：每条 = 判据 + 实测证据（含统计量）+ 定位线索（`文件:行` / 元素 id）+ 建议动作 |
/// | `sections{}` | 原始分解（阶段/元素/原语/帧序列），供需要细节时下钻 |
///
/// 模型只需读 `findings` 就能干活；`caveats` 让它不会把噪声当回归；`sections`
/// 是它追问"为什么"时的证据。
///
/// ## schema 稳定
///
/// `schema` 字段是**协议**：字段增删必须升版本（`st.perf.report/1` → `/2`），
/// 因为读它的是自动化流程而不是人。新增字段是兼容的（读者忽略未知字段），
/// 但**改名/换单位属于破坏性变更**。

#include <cstdint>
#include <string>
#include <vector>

namespace st::perf {

/// 报告 schema 版本（读者据此判断字段语义）。
inline constexpr std::string_view kReportSchema{"st.perf.report/1"};

/// 一条**可下钻的发现**：判据 + 证据 + 定位 + 建议。
///
/// 刻意做成"一句话就能照做"：`title` 说现象、`evidence` 给数字（含统计量与单位）、
/// `where` 给位置、`action` 给下一步。模型据此可以直接改代码或再跑一条命令。
struct Finding {
  /// 严重度：`critical`（已达/超出预算或明显异常）/ `warn`（接近预算、或读数可疑）/
  /// `info`（观察项，不一定要动）。
  std::string severity{"info"};
  /// 机器可读的类别（`frame_budget` / `layout_dominant` / `paint_hotspot` /
  /// `element_hotspot` / `present_dominant` / `idle_churn` / `clock_precision` …）。
  /// 自动化按这个分流，**不靠 title 的文案**。
  std::string kind{};
  /// 一句话现象（人读；也是模型最先看的）。
  std::string title{};
  /// 证据（必须含统计量与单位；**不许裸数字**）。
  std::string evidence{};
  /// 定位线索（`src/ui/components/card.cpp:812` / 元素 id / 阶段名）；可为空。
  std::string where{};
  /// 建议动作（一句话，具体到"改哪里/量什么"）；可为空。
  std::string action{};
};

/// 一条**口径告诫**：这份数据的前提是否成立。
///
/// `ok=false` 的条目意味着"按这个口径得出的结论不可信/不可比较"——例如
/// `profile=debug`（-O0）、`samples<30`、`paced=true`（墙钟被限帧拉长）。
struct Caveat {
  std::string kind{};    ///< `profile` / `samples` / `pacing` / `clock` / `backend` / `dpi` …
  bool ok{true};         ///< 前提是否成立
  std::string detail{};  ///< 实测事实（如 `profile=debug（-O0，耗时约为 release 的 2~4 倍）`）
  /// 处置建议（`ok=false` 时必须给：怎么重跑才可信）。
  std::string fix{};
};

/// 测量环境（报告自述的一部分——**没有它，数字不可比**）。
struct MeasurementContext {
  std::string backend{};
  std::string renderer{};
  bool headless{true};
  int width{0};              ///< 逻辑宽
  int height{0};             ///< 逻辑高
  float device_scale{1.0F};
  std::string profile{};     ///< 构建档（dev/debug/release）
  std::string app{};
  std::string app_version{};
  std::string os{};
  std::string cpu{};
  std::uint32_t frames{0};   ///< 参与统计的帧数
  std::int64_t wall_ms{0};   ///< 这段采样跨越的墙钟时间
};

/// 完整诊断报告。
struct Report {
  std::string schema{std::string(kReportSchema)};
  std::string kind{};  ///< `frame`（帧剖析）/ `startup`（启动分解）/ `compare`（对比）
  MeasurementContext context{};
  std::vector<Caveat> caveats{};
  std::vector<Finding> findings{};

  /// 结构化分解（阶段 / 元素 / 原语 / 帧序列）。**内容随 kind 变**，
  /// 由各生产者填写：这是"追问为什么"时的证据层，不要求模型先读。
  std::string sections_json{"{}"};
};

/// 序列化为 JSON 文本（缩进 2 空格；字段顺序稳定）。
[[nodiscard]] auto to_json(const Report& report) -> std::string;

/// 序列化为**面向人的**文本摘要（终端的默认输出）。
///
/// 布局刻意固定：`结论` → `口径` → `分解` → `复现`。模型若只看文本，
/// 前两节就足以判断"要不要信 + 该动哪里"。
[[nodiscard]] auto to_text(const Report& report) -> std::string;

/// 读一份报告（`st perf --baseline` / `--compare` 用）。失败返回 `nullopt`。
///
/// 只解析报告需要的字段（`kind`/`context`/`sections` 里的数值项）——
/// 刻意不引入通用 JSON 依赖到报告契约里：报告是**协议**，它的读者不该被
/// 某个 JSON 库的版本牵着走。
struct ReportSummary {
  std::string schema{};
  std::string kind{};
  MeasurementContext context{};
  /// 关键标量（如 `frame_p50_ms` / `paint_ms` / `samples`）——对比时按 key 成对比较。
  std::vector<std::pair<std::string, double>> metrics{};
};

[[nodiscard]] auto summarize_json(std::string_view text) -> ReportSummary;

}  // namespace st::perf
