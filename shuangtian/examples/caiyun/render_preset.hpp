/// 三维相机与光照的**预设 + 局部覆盖**：每镜独立，但能复用。
///
/// ## 为什么需要它
///
/// 当前每份 `model_script` 都要把相机与光照**整份写一遍**：
///
/// ```jsonc
/// {"camera":{"eye":[0,0.4,5.4],"target":[0,0,0],"fov":44},
///  "lighting":{"key_direction":[-0.4,0.66,0.64],"ambient":0.1,"diffuse":0.52,
///              "rim":0.26,"exposure":0.92,"tone_map":"agx",
///              "extra_lights":[{...},{...}]},
///  "objects":[...]}
/// ```
///
/// 十幕片子就是这段写十遍。改一个打光参数要改十处，漏一处就出现
/// "有一镜明显不一样"——而它看起来像"设计如此"。
///
/// ## 设计：每镜独立是默认，复用的是**预设**
///
/// 语义只有一条：**本镜最终值 = 预设值叠加本镜覆盖值**。
///
/// - 不写 `preset`：完全用本镜写的值（当前行为，零改动）。
/// - 写 `preset: "studio"`：先取预设，再用本镜写的字段覆盖。
/// - 写 `preset` 又写 `null`：显式**删掉**预设里的某一项
///   （例如预设带两盏补光，本镜只想要主光 —— `"extra_lights": null`）。
///
/// `null = 删除` 这条是必要的：没有它，"本镜不要补光"就表达不出来
/// （留空等于"用预设的"，而写空数组又会被当成"一个空的补光列表"）。
///
/// ## 预设存在哪
///
/// 预设是**工程包的一部分**（`lighting/*.json`、`cameras/*.json`），
/// 不是硬编码常量。理由：打光是创作资产，一个团队会积累自己的灯位；
/// 硬编码进程序等于把资产锁在二进制里，改一次要重新编译。
///
/// 同时提供一组**内置预设**（`studio` / `moody` / `flat`），
/// 让新工程不必从零开始——它们在 `builtin_lighting_presets()` 里。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/json.hpp"

namespace caiyun {

using st::Result;

/// 一条预设：名字 + 一份 JSON 片段（相机或光照）。
struct RenderPreset {
  std::string name{};
  /// 预设内容（与 `model_script.camera` / `.lighting` 同形状）。
  st::Json body{};
  /// 来源（文件名或 `<builtin>`），用于诊断"这个预设从哪来"。
  std::string origin{};
};

/// 解析「预设 + 覆盖」的合并结果。
///
/// ## 合并规则（逐键深合并，数组整体替换）
///
/// - 对象 + 对象 → 逐键递归合并；
/// - 任一为 `null` → 结果为 `null`，且调用方应把它**删掉**（见 `null 删除` 说明）；
/// - 其余（含数组、标量、形状不同）→ 覆盖方胜出。
///
/// 为何数组整体替换而不是逐项合并：数组在语义上是"一个列表"。
/// 逐项合并会让"我想用预设的两盏灯，但把第一盏换掉"变成必须知道
/// 预设里有几盏、分别是什么——那比直接写两盏还麻烦。要微调就整体重写。
[[nodiscard]] auto merge_preset(const st::Json& base, const st::Json& override_values)
    -> st::Json;

/// 内置预设（新工程不必从零开始）。
///
/// `studio`：主光 + 两盏冷暖补光，总光量 0.88（实测的过曝分界线）
/// `moody`：低环境光 + 强边缘光，暗调
/// `flat`：平坦无方向感，适合"界面截图式"的三维块
[[nodiscard]] auto builtin_lighting_presets() -> std::vector<RenderPreset>;

/// 内置相机预设。
///
/// `front`：正前方略高视角（产品展示）
/// `hero`：低角仰拍（有气势）
/// `top`：俯视（结构展示）
[[nodiscard]] auto builtin_camera_presets() -> std::vector<RenderPreset>;

/// 在预设表里按名查找。找不到返回 `nullptr`。
[[nodiscard]] auto find_preset(const std::vector<RenderPreset>& presets,
                               std::string_view name) -> const RenderPreset*;

/// 从包目录读预设：`<root>/<subdir>/*.json`，每个文件名（去扩展名）就是预设名。
///
/// 缺失目录返回空列表（不是错误）：一个包可以只有内置预设。
[[nodiscard]] auto load_presets_from_dir(std::string_view dir) -> std::vector<RenderPreset>;

/// 把一份 `model_script` 里的 camera/lighting 按预设展开（就地替换）。
///
/// 失败：预设名找不到 / 预设文件坏。**错误信息要指路**——
/// 报"第几幕第几个图层的哪个键"，而不是只说"预设不存在"。
[[nodiscard]] auto apply_render_presets(st::Json& model_script,
                                        const std::vector<RenderPreset>& camera_presets,
                                        const std::vector<RenderPreset>& lighting_presets,
                                        std::string_view where) -> Result<void>;

}  // namespace caiyun
