#pragma once

/// 图标位图的重采样（**与平台无关**）与图标源尺寸常量。
///
/// ## 为什么单独抽出来
///
/// 各平台后端把应用给的 PNG 位图转成系统图标时，**几乎总要改变尺寸**：
/// Windows 的 `ICON_BIG` 要 32×32、`ICON_SMALL` 与通知区要 16×16，而应用给的是
/// 一张（或多张）源图。这一步的算法直接决定观感，却埋在 `platform_win32.cpp` 里——
/// 于是它既没有测试，也没人注意到它其实决定了「图标糊不糊」。
///
/// ## 算法：面积平均（不是最近邻）
///
/// 最近邻只保 1/N² 的样本、丢掉其余，结果是**边缘硬且跳、细笔画随机消失**。
/// 实测（16px 相期图标，真值 = 直接在 16px 栅格绘制）：
///
/// | 路径 | 与真值的平均通道偏差（0–255） | 过渡带像素数（越大越连续） |
/// |---|---|---|
/// | 最近邻 | **8.77** | **24** |
/// | 面积平均 | 6.65 | 52 |
///
/// 两个正交量同向 ⇒ 面积平均更好。注意即使面积平均也与真值有差距：
/// **源图按目标尺寸分别绘制**才是根治（应用侧的事，见 `examples/xiangqi/main.cpp`）。
///
/// ## 口径细节
///
/// - **按预乘 alpha 累加**：半透明边缘的 RGB 权重与其覆盖率成正比，否则边缘发灰；
/// - 缩幅为 1（源 = 目标）时**退化为恒等**，不引入任何变化（有测试钉住）；
/// - 放大也跟着走同一套采样（等价于双线性），比最近邻锯齿少。

#include <cstdint>
#include <span>
#include <vector>

namespace st::shell {

/// 应用可用的图标源尺寸（**按目标尺寸分别绘制**，避免「画大再缩」）。
///
/// 选这三档的理由：16 是任务栏与通知区的逻辑尺寸；32 是 Windows `ICON_BIG` 与
/// 高 DPI 下 16 逻辑尺寸的物理尺寸；64/128 供 200%/400% 缩放的窗口图标。
inline constexpr std::uint32_t kIconSourceSizes[] = {16, 24, 32, 48, 64, 128};

/// 重采样一张 RGBA8 位图到 `dst_width × dst_height`。
///
/// 输入 `rgba` 须为**非预乘**的直通 alpha（与 `codec::PngImage` 一致），
/// 长度必须等于 `src_width * src_height * 4`；不满足时返回空 vector（调用方如实报错，
/// 不猜尺寸）。输出同样是直通 alpha。
///
/// `samples` 是每目标像素每个方向的子样本数（≥1）：4 足够消除 2~3 倍缩幅的台阶，
/// 而图标尺寸下这点开销可忽略。
[[nodiscard]] auto resample_rgba(std::span<const std::uint8_t> rgba, int src_width, int src_height,
                                 int dst_width, int dst_height, int samples = 4)
    -> std::vector<std::uint8_t>;

}  // namespace st::shell
