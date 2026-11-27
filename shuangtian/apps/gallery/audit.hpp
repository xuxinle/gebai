#pragma once

/// 画廊的**自检引擎**：把框架的运行时不变式变成可读的 PASS / FAIL 表。
///
/// ## 为什么画廊要有自检台
///
/// 框架里有一批"只在出问题时才说话"的约束：注册表与展示集对账、树上不该出现
/// 未知类型、属性面两张表不得重叠、主题令牌的对比度、布局必须落整像素、
/// 图标名必须在内置表里、物理尺寸必须等于逻辑 × DPI。
///
/// 它们今天的形态各不相同：有的在 `tests/`（要靠 `st test` 跑）、有的只打在
/// 启动日志里（`main.cpp` 的 `audit_gallery_tree`）、有的**只是注释里的约定**。
/// 于是"框架现在健康吗"这个问题没有单一答案——而这正是**质检场地**存在的意义：
/// 把散落的判据收进一页界面，改完代码看一眼就知道有没有踩到。
///
/// ## 依赖方向（这一条决定了本模块的形状）
///
/// 页面不该知道 `st::app::Application` 的存在（`PageHooks` 的既有约定）。
/// 所以本头**只吃纯数据**：`AuditEnv` 由 `main.cpp` 从 `Application`/`Metrics`
/// 摘出来填，自检函数只认这堆数。好处是自检可以脱离应用跑（测试直接用）。
///
/// ## 三档状态的分工
///
/// | 状态 | 含义 |
/// |---|---|
/// | `Pass` | 判据成立 |
/// | `Warn` | 判据成立但有值得注意的量（如"整像素 3 处例外"）——不阻断，但要看 |
/// | `Fail` | 判据不成立：这是一处**真缺陷**，明细里点名是谁 |
/// | `Info` | 纯信息量（不计入通过率；如"帧耗时 p50 = 4.1ms"） |

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace st::ui {
class UiRoot;
class Panel;
}

namespace gallery {

/// 自检项状态。
enum class AuditStatus : std::uint8_t { Pass, Warn, Fail, Info };

[[nodiscard]] auto audit_status_label(AuditStatus status) noexcept -> const char*;
/// 状态对应的语气（页面据此上色；不在此头里依赖 `Tone` 以保持轻量——用字符串）。
[[nodiscard]] auto audit_status_tone(AuditStatus status) noexcept -> const char*;

/// 一项自检的结论。
struct AuditCheck {
  std::string id{};
  std::string title{};
  /// 一句话结论（页面上直接显示的那行）。
  std::string summary{};
  AuditStatus status{AuditStatus::Info};
  /// 明细（最多展示十来行；超出在最后一行写"还有 N 条"）。
  std::vector<std::string> details{};
  /// 本项耗时（毫秒）——自检本身不该成为负担，这个数字要能看见。
  double elapsed_ms{0.0};
};

/// 一次自检的完整报告。
struct AuditReport {
  std::vector<AuditCheck> checks{};
  std::size_t pass_count{0};
  std::size_t warn_count{0};
  std::size_t fail_count{0};
  double total_ms{0.0};
  /// 生成时应用已跑的帧数（同一实例两次自检的帧号必然不同——它就是"新鲜度"凭证）。
  std::uint64_t frames{0};

  [[nodiscard]] auto healthy() const noexcept -> bool { return fail_count == 0; }
};

/// 自检需要的外部事实（由应用侧摘出来，页面与自检引擎都不认 `Application`）。
struct AuditEnv {
  std::string backend{};
  std::string renderer{};
  std::string text_renderer{};
  float device_scale{1.0f};
  int logical_width{0};
  int logical_height{0};
  int physical_width{0};
  int physical_height{0};
  std::size_t nodes{0};
  std::uint64_t frames{0};
  double frame_p50_ms{0.0};
  double frame_p95_ms{0.0};
};

/// 跑全部检查（同步；调用点由宿主控制，不做节流）。
///
/// `root` 用于需要遍历真值树的检查（树自检 / 属性面 / 整像素 / 图标）。
/// 一遍遍历拿多份数据：树有几百个元素，每项各遍历一次会让自检本身变慢。
[[nodiscard]] auto run_audit(st::ui::UiRoot& root, const AuditEnv& env) -> AuditReport;

}  // namespace gallery
