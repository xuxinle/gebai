#pragma once

/// @file scene_describe.hpp
/// @brief **场景自检摘要**：回答"我写对了吗"，而不是只说"解析成功"。
///
/// ## 为什么解析成功还不够
///
/// 一份能解析的场景经常是**视觉上错的**：
/// - 相机在物体内部 → 画面全黑；
/// - 物体在取景框外 → 什么都没有；
/// - 场景全空 → 一片背景色；
/// - 地平面朝错方向 → 阴影一个像素不出。
///
/// 这些情况下解析器完全没意见（字段都对），但调用方（尤其模型）拿到的是
/// "成功 + 空白画面"，然后开始怀疑是别的地方错了。
///
/// 这份摘要把"能检查出来的配置问题"集中报出来，让调用方在**看一眼图之前**
/// 就知道哪里不对。
///
/// ## 两类输出
///
/// - **事实**（fact）：物体数、三角形数、包围盒、贴图数、动画时长——
///   这些无论对错都有用（可以直接拿来算相机距离）。
/// - **诊断**（diagnostic）：可能的配置问题，带严重级别与建议动作。
///
/// 事实与诊断分开而不是混在一起：前者是"数据"，后者是"意见"。
/// 混在一起会让调用方难以只取数据（而取数据是最常见的用法）。

#include <string>
#include <vector>

#include "st/core/json_fwd.hpp"
#include "st/math/matrix.hpp"

namespace st::raster {

struct SceneSpec;

/// 诊断的严重级别。
enum class DiagnosticLevel : std::uint8_t {
  Info = 0,     ///< 提示（不影响出图，但可能不是你想要的）
  Warning = 1,  ///< 很可能有问题（画面会明显不对）
  Error = 2,    ///< 必然有问题（画面为空或全黑）
};

[[nodiscard]] auto diagnostic_level_name(DiagnosticLevel level) noexcept -> std::string_view;

/// 一条诊断。
struct Diagnostic {
  DiagnosticLevel level{DiagnosticLevel::Info};
  /// 问题短名（`"camera_inside_geometry"` 这类，便于程序判断）
  std::string code{};
  /// 人/模型读的说明（含具体数值）
  std::string message{};
  /// 建议动作（**必须可执行**，不能只说"有问题"）
  std::string suggestion{};
};

/// 场景摘要：事实 + 诊断。
struct SceneSummary {
  // ── 事实 ──
  std::size_t object_count{0};        ///< 可画几何的物体数（不含纯分组）
  std::size_t group_count{0};         ///< 纯分组节点数
  std::size_t total_vertices{0};
  std::size_t total_triangles{0};
  std::size_t textured_objects{0};    ///< 带贴图的物体数
  std::size_t ground_objects{0};      ///< 标注为地面的物体数
  /// 全部几何的世界包围盒（含变换后的 8 角；空场景为空盒）。
  math::Vec3 bounds_min{};
  math::Vec3 bounds_max{};
  bool bounds_valid{false};
  /// 建议的相机距离（按包围球与 fov 反解；`bounds_valid` 为假时无意义）。
  float suggested_camera_distance{0.0f};
  /// 动画时长（秒；无动画为 0）。
  float animation_duration{0.0f};
  std::size_t animation_tracks{0};

  // ── 诊断 ──
  std::vector<Diagnostic> diagnostics{};

  [[nodiscard]] auto has_error() const noexcept -> bool;
  [[nodiscard]] auto has_warning() const noexcept -> bool;
};

/// 生成场景摘要。
///
/// ## 为什么需要真的生成网格
///
/// 三角形数与包围盒只能靠实际造几何得到（`radius` 与最终尺寸的关系随图元而异）。
/// 代价是这份摘要比"读一遍 JSON"慢——但它省掉的是"渲染出一张空图再回头查"。
/// 对几千三角形的场景，这个代价在毫秒级。
[[nodiscard]] auto describe_scene(const SceneSpec& spec) -> SceneSummary;

/// 摘要的 JSON 形式（给程序/模型读）。
[[nodiscard]] auto summary_json(const SceneSummary& summary) -> Json;

/// 摘要的人读文本（多行；每行一个事实或诊断）。
[[nodiscard]] auto summary_text(const SceneSummary& summary) -> std::string;

}  // namespace st::raster
