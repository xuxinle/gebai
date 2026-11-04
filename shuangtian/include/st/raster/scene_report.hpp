#pragma once

/// @file scene_report.hpp
/// @brief **渲染后的自检报告**：把"画面对不对"变成可读的数字与结论。
///
/// ## 为什么这一层是"闭环"的关键（这是本模块存在的全部理由）
///
/// 三维的失败几乎**从不报错**。字段拼错会报，但下面这些不会：
///
/// | 症状 | 解析器 | 渲染器 | 画面 |
/// |---|---|---|---|
/// | 相机钻进物体里 | 无意见 | 无意见 | 全黑/全糊 |
/// | 物体全在取景框外 | 无意见 | 无意见 | 一片背景色 |
/// | 物体只有 3 个像素 | 无意见 | 无意见 | 看着像"没画" |
/// | 两个物体完全重叠 | 无意见 | 无意见 | 只看到一个 |
/// | 光方向反了 | 无意见 | 无意见 | 全黑 |
/// | 颜色口径错（重复解码） | 无意见 | 无意见 | 偏暗，但"像配色问题" |
///
/// 这些正是**生成代码的模型**最容易踩、也最难自查的一类。模型的视觉理解确实弱
/// （这是用户的判断，也是本仓反复验证的事实：它无法从截图判断"这个球是不是
/// 被相机切掉了"），所以**不能要求它看图**。
///
/// 本模块的办法是：先把画面量成数字，再把数字变成**可执行的建议**。
/// 于是闭环变成纯文本往返：
///
/// ```text
/// 写 three.js  →  渲染  →  report（文本）  →  改脚本  →  再渲染
///                            ↑ 模型能读的就是这里的数字与建议
/// ```
///
/// ## 三类输出，各有各的读者
///
/// | 输出 | 读者 | 形态 |
/// |---|---|---|
/// | 事实（facts） | 程序 | 覆盖率、每物体像素数、包围盒、平均亮度 |
/// | 诊断（diagnostics） | 模型/人 | "相机可能在物体内部" + "把 eye 往外挪到 ≥ X" |
/// | 结论（verdict） | 程序/CI | `ok` / `nothing_drawn` / `camera_inside` …（可断言） |
///
/// 事实与诊断分开（与 `scene_describe` 同一立场）：前者是数据，后者是意见。
///
/// ## 它与 `describe_scene` 的分工
///
/// `describe_scene` 在**渲染前**看配置（包围盒、建议相机距离），不需要画一次；
/// 本模块在**渲染后**看像素（实际画出来多少）。两者合起来才闭环：
/// 前者防"还没画就知道错了"，后者防"画了但其实什么都没有"。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "st/core/json_fwd.hpp"
#include "st/math/color.hpp"

namespace st::raster {

struct SceneSpec;

/// 一个物体在画面上的**实测存在感**。
struct ObjectVisibility {
  /// 物体名（`SceneSpec` 里的 `name`；three 场景是 `mesh.name`）。
  std::string name{};
  /// 单独渲染这个物体时的非背景像素数（0 = 完全不在画面里）。
  int pixels{0};
  /// 像素占整幅的比例（0..1）。
  double coverage{0.0};
  /// 屏幕包围盒（像素坐标；无像素时全为 0）。
  int min_x{0};
  int min_y{0};
  int max_x{0};
  int max_y{0};
  /// 该物体是否被其他物体**完全**遮住（单独画有像素，合起来画没有）。
  bool fully_occluded{false};

  [[nodiscard]] auto visible() const noexcept -> bool { return pixels > 0; }
};

/// 诊断的严重级别（与 `scene_describe` 同一套语义，便于统一处理）。
enum class ReportLevel : std::uint8_t {
  Info = 0,     ///< 提示（能出图，但可能不是你想要的）
  Warning = 1,  ///< 很可能有问题
  Error = 2,    ///< 必然有问题（画面为空或全黑）
};

[[nodiscard]] auto report_level_name(ReportLevel level) noexcept -> std::string_view;

/// 一条诊断 + **可执行的建议**。
struct ReportDiagnostic {
  ReportLevel level{ReportLevel::Info};
  /// 问题短名（可程序判断）：`nothing_drawn` / `camera_inside` / `object_out_of_frame` /
  /// `object_tiny` / `all_overlapping` / `image_too_dark` / `image_blown_out` /
  /// `no_shading_variation`。
  std::string code{};
  /// 人/模型读的说明，**必须含具体数值**（"覆盖率 0.00%" 而不是"画面很小"）。
  std::string message{};
  /// 建议动作，**必须可执行**（"把 camera.position.z 从 4.0 加到 ≥ 6.0" 而不是"检查相机"）。
  std::string suggestion{};
};

/// 渲染自检报告。
struct SceneRenderReport {
  // ── 事实 ──
  int width{0};
  int height{0};
  /// 非背景像素数（背景色从四角采样，不是用传入的 clear 色——色调映射会改它）。
  int painted_pixels{0};
  /// 覆盖比例（0..1）。
  double coverage{0.0};
  /// 全画面平均亮度（0..255，sRGB）。
  double mean_luma{0.0};
  /// 画面亮度的标准差（**约 0 说明是纯色块**，通常意味着没有光照变化或场景空）。
  double luma_stddev{0.0};
  /// 每个物体的实测可见性（按 `SceneSpec.objects` 的先序）。
  std::vector<ObjectVisibility> objects{};
  /// 渲染耗时（毫秒）。
  double render_ms{0.0};

  // ── 诊断 ──
  std::vector<ReportDiagnostic> diagnostics{};

  // ── 结论 ──
  /// 主判据（程序可用它直接断言，不必去翻 diagnostics）。
  /// `ok` / `nothing_drawn` / `camera_inside` / `too_dark` …（与 `code` 同一套名字）
  std::string verdict{"ok"};

  [[nodiscard]] auto has_error() const noexcept -> bool;
  [[nodiscard]] auto has_warning() const noexcept -> bool;
  [[nodiscard]] auto ok() const noexcept -> bool { return verdict == "ok"; }
};

/// 渲染一份场景并产出报告（**会真的画两遍以上**：整幅 + 逐物体）。
///
/// ## 为什么要逐物体单独再画一遍
///
/// "这个物体在画面里有没有" 无法从整幅图反推（两个物体可能完全重叠，
/// 或被别的物体挡住）。逐物体单独渲染一遍是最直接的办法，而它在
/// **低分辨率**下很便宜（实测 200×200 单物体约 1~3 ms），换来的是
/// "哪个物体没出现"这种**只有在逐物体层面才存在**的事实。
///
/// @param probe_size 逐物体探测用的分辨率（默认 120；太大的话物体多时变慢）
[[nodiscard]] auto inspect_scene_render(const SceneSpec& spec, int width, int height,
                                        math::Color background, int probe_size = 120)
    -> SceneRenderReport;

/// 报告的 JSON 形态（给程序/模型读）。
[[nodiscard]] auto report_json(const SceneRenderReport& report) -> Json;

/// 报告的**人读文本**（多行；先结论、再事实、最后建议）。
///
/// 为何这个格式重要：它要能被直接贴回给模型当反馈。所以：
/// - 第一行就是**结论**（`FAIL nothing_drawn`），不必读到末尾才知道对错；
/// - 每个数字都带单位/参照（"覆盖 0.00%（期望 > 3%）"）；
/// - 建议是**能照抄的代码改动**。
[[nodiscard]] auto report_text(const SceneRenderReport& report) -> std::string;

}  // namespace st::raster
