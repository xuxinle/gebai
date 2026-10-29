#pragma once

/// @file animation.hpp
/// @brief **关键帧动画**：轨道、插值、缓动、采样——把"骨架"变成"会动的骨架"。
///
/// ## 为什么单独一层（而不是塞进 scene_graph）
///
/// `scene_graph` 回答"节点摆在哪"，`animation` 回答"某一时刻摆在哪"。
/// 前者是**静态**求解（一个矩阵），后者是**时间上的函数**（矩阵 × 时间）。
/// 两者混在一起会让"只想摆场景、不要动画"的调用方被迫接一套时间概念。
///
/// ## 与 Blender / 游戏引擎的对应
///
/// | 本模块 | Blender | glTF 2.0 |
/// |---|---|---|
/// | `Track`（一个通道） | F-Curve | `animation.channels[]` |
/// | `Keyframe` | 关键帧点 | `accessor` 里的一个采样 |
/// | `Easing` | Interpolation（Bezier/Linear/Constant） | `sampler.interpolation` |
/// | `sample()` | 求值 F-Curve | 播放器逐帧采样 |
///
/// ## 三种插值口径（各自在什么场合用）
///
/// | 名称 | 观感 | 用在哪 |
/// |---|---|---|
/// | `Constant` | 阶跃（前一帧的值保持到下一帧） | 开关类（可见性、材质切换） |
/// | `Linear` | 匀速 | 机械运动、匀速旋转、位置平移 |
/// | `Cubic`（缓入缓出） | 两头慢中间快 | **绝大多数"自然"动作**：抬手、转头、呼吸 |
///
/// 默认 `Linear` 而不是 `Cubic`：缓动是**艺术选择**，不该由库替调用方决定；
/// 而"默认匀速"至少是可预测的。要自然感就显式写 `"easing": "ease-in-out"`。
///
/// ## 时间轴约定
///
/// - 单位：**秒**（与裁云的时间轴一致；帧率由调用方乘除，库不掺和 fps）
/// - 关键帧**不要求有序**（解析时排序；手写 JSON 常按叙事顺序写）
/// - 时间可以重复（同一时刻两个关键帧）→ 后写的赢（覆盖语义，便于"补一个修正帧"）
/// - 采样在关键帧范围外**保持端点值**（不循环、不外推）：循环是播放器的策略，
///   库只负责"给我 t 时刻的值"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/matrix.hpp"
#include "st/math/quaternion.hpp"
#include "st/raster/scene_graph.hpp"

namespace st::raster {

/// 插值方式。
enum class Easing : std::uint8_t {
  Linear = 0,    ///< 匀速（默认）
  Constant = 1,  ///< 阶跃：保持前一关键帧的值（开关类）
  EaseIn = 2,    ///< 缓入（慢起）
  EaseOut = 3,   ///< 缓出（慢停）
  EaseInOut = 4, ///< 缓入缓出（最常用）
};

[[nodiscard]] auto easing_name(Easing easing) noexcept -> std::string_view;
[[nodiscard]] auto parse_easing(std::string_view name) -> std::optional<Easing>;
[[nodiscard]] auto easing_names() -> std::string;
/// 把 `[0,1]` 的线性 t 按缓动曲线重映射（返回值仍在 `[0,1]`）。
[[nodiscard]] auto apply_easing(Easing easing, float t) noexcept -> float;

/// 一个关键帧：时间 + 值（三种类型统一存放，按轨道类型取用）。
///
/// ## 为什么不给每类值各造一种轨道
///
/// 平移/旋转/缩放三条轨道的**时间逻辑完全相同**（排序、分段、缓动、端点保持），
/// 唯一差别是"值怎么插"。分成三种轨道类型会让分段与缓动写三遍——
/// 而那种重复实现几乎必然在其中一处写错（比如缓动只加了位置、没加旋转）。
struct Keyframe {
  /// 时间（秒）。
  float time{0.0f};
  /// 平移 / 缩放 / 欧拉角的原始值（按轨道种类取用）。
  ///
  /// 注意：**旋转轨道**用这里的三个分量作欧拉角输入，进采样时立刻转四元数
  /// （欧拉角不能插值，见 `math::Quat` 的说明）。这样脚本层仍可以写人算得出的角度。
  math::Vec3 value{0.0f, 0.0f, 0.0f};
  /// 缓动（作用于**本关键帧到下一关键帧**这一段）。
  Easing easing{Easing::Linear};
};

/// 一条通道：对某个节点的某个属性做时间动画。
enum class TrackKind : std::uint8_t {
  Translation = 0,  ///< 平移（线性插值）
  Rotation = 1,     ///< 旋转（欧拉角输入 → 四元数 slerp；见 `Keyframe::value`）
  Scale = 2,        ///< 缩放（线性插值）
  Visible = 3,      ///< 可见性（`value.x > 0.5` 为真；阶跃插值）
};

[[nodiscard]] auto track_kind_name(TrackKind kind) noexcept -> std::string_view;
[[nodiscard]] auto parse_track_kind(std::string_view name) -> std::optional<TrackKind>;
[[nodiscard]] auto track_kind_names() -> std::string;

struct Track {
  /// 目标节点名（与 `SceneNode::name` 对应；解析时校验存在性）。
  std::string node{};
  TrackKind kind{TrackKind::Translation};
  std::vector<Keyframe> keys{};

  /// 末关键帧时间（空轨道返回 0）。
  [[nodiscard]] auto duration() const noexcept -> float;
};

/// 一组轨道 = 一段动画（可以有多段，按名字区分）。
struct Clip {
  std::string name{"clip"};
  std::vector<Track> tracks{};

  [[nodiscard]] auto duration() const noexcept -> float;
};

/// 采样结果：一个节点在某一时刻的**局部**变换（已含枢轴的动画前的 TRS）。
struct SampledTransform {
  std::string node{};
  TransformTRS local{};
  bool visible{true};
  /// 本节点是否被任何轨道命中（未命中的保持场景里写死的值）。
  bool animated{false};
};

/// 采样一个片段在 `time` 时刻的全部轨道。
///
/// ## 为什么返回"按名字的列表"而不是直接改树
///
/// 采样是**纯函数**（同样输入 → 同样输出，这是"逐帧可复现"的前提）。
/// 直接改树会让同一个树被时间推进反复改写，一旦中途异常就留下半改状态；
/// 而返回列表让调用方决定怎么应用（渲染一次 / 导出所有帧 / 只取某个节点）。
[[nodiscard]] auto sample_clip(const Clip& clip, float time) -> std::vector<SampledTransform>;

/// 把采样结果应用到节点树（返回**新树**）。
///
/// 为什么要返回新树而不是就地改：节点树的解析结果会被缓存/复用
/// （同一份场景渲染多帧），就地改会让"上一帧的姿态"渗透到下一帧。
/// 代价是每帧复制一次树——节点数上千时仍远低于渲染成本，换来的是确定性。
[[nodiscard]] auto apply_sample(const std::vector<SceneNode>& roots,
                                const std::vector<SampledTransform>& sampled)
    -> std::vector<SceneNode>;

/// 把一段动画**稠密化**（按固定步长重新采样），用于导出到只支持线性插值的格式。
///
/// ## 为什么需要它（一条必须说清的限制）
///
/// glTF 的插值只有 `LINEAR` / `STEP` / `CUBICSPLINE` 三种，而本框架的
/// `ease-in` / `ease-out` / `ease-in-out` 是**任意曲线**，无法用 LINEAR 表达。
/// 直接写 LINEAR 会让缓动消失（动画变成匀速）——在霜天里看着对、在 Blender 里不对。
///
/// 解法：导出前按固定步长（如 1/60 秒）把所有轨道重采样成稠密线性关键帧。
/// 曲线形状因此保真，代价是关键帧变多（一段 2 秒动画在 60Hz 下 121 个关键帧）。
///
/// `step` 为 0 或负时返回原片段（不稠密化）。
[[nodiscard]] auto densify(const Clip& clip, float step) -> Clip;

/// 校验一段动画（返回带路径的错误）。
///
/// 检查项：① 轨道指向的节点必须存在；② 每种属性同一节点只能有一条轨道
/// （两条平移轨道会互相覆盖，而"谁赢"取决于顺序——那是不可复现的来源）；
/// ③ 关键帧数量 ≥ 1；④ 时间必须有限（NaN 会污染整条插值链）。
[[nodiscard]] auto validate_clip(const Clip& clip, const std::vector<SceneNode>& roots)
    -> Status;

}  // namespace st::raster
