#pragma once

/// 格斗示例 · **选人与对战模式**（开局界面）。
///
/// ## 为什么做成"视图内部的一个覆盖层"而不是另一个页面
///
/// 选人界面的全部内容都是**自绘**（两张角色卡、模式切换、开始按钮），
/// 与 `GameView` 用的是同一套画布能力；做成独立页面就要在两套绘制上下文之间
/// 来回搬状态（谁持有 `Match`、谁处理键盘、退出时怎么回到对战）。
///
/// 覆盖层的语义是：**对战一直在跑，只是暂时被挡住**。于是
/// "开始对战" = 关掉覆盖层，不需要重建 `Match`，也不会有
/// "重开后 AI 状态丢了"这类问题。
///
/// ## 两种模式的区别（这是本界面唯一需要解释的东西）
///
/// | 模式 | 玩家控制 | 对手 |
/// |---|---|---|
/// | 人机 | 键盘（`A/D` `S` `J/K/L` `U`） | AI（难度可调） |
/// | 机机 | 无（两侧都是 AI） | AI |
///
/// 机机不是"两个人抢一个键盘"的退化，而是**观战/平衡验证**的形态：
/// 调数值时盯着它跑几十局，比人肉互打快得多。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"

namespace gedou {

/// `st::math` 的本地别名（与 `pose.hpp` 一致；单独包含本头时也要能解析）。
namespace math = st::math;

/// 对战模式。
enum class BattleMode : std::uint8_t {
  PlayerVsAi,   ///< 人机（默认）
  AiVsAi,       ///< 机机（观战 / 平衡验证）
};

/// 选人界面的状态。
struct SelectState {
  bool active{true};                       ///< 是否还在选人（false = 已进入对战）
  std::size_t left{0};                     ///< 左侧角色下标
  std::size_t right{1};                    ///< 右侧角色下标
  BattleMode mode{BattleMode::PlayerVsAi};
  /// 当前被选中的控件（0 = 左角色卡、1 = 右角色卡、2 = 模式、3 = 开始）。
  int focus{0};
};

/// 选人界面的**命中区域**（逻辑像素；`draw` 时按当前视口算好，点击时复用）。
///
/// 为什么让绘制产出命中区：布局与命中必须同源。分开算过一次，
/// 结果是"点了没反应"——因为按钮画在 A 处、判定框算在 B 处（差 12px）。
struct SelectHitAreas {
  math::Rect left_card{};
  math::Rect right_card{};
  math::Rect mode_toggle{};
  math::Rect start_button{};
  /// 难度：`-` / `+` 两个小按钮（只在人机模式可用）。
  math::Rect difficulty_minus{};
  math::Rect difficulty_plus{};
};

/// 由视口尺寸算出各区域的布局（纯函数，可单测）。
[[nodiscard]] auto select_layout(math::Rect viewport) -> SelectHitAreas;

/// 命中测试：返回点中哪个控件（-1 = 没点中任何东西）。
///
/// 顺序有意义：**按钮优先于卡片**——按钮压在卡片上方（视觉上也是），
/// 若先判卡片，点按钮会被当成"选角色"。
[[nodiscard]] auto select_hit_test(const SelectHitAreas& areas, math::Point point) -> int;

/// 模式的中文名（界面与日志共用一份）。
[[nodiscard]] auto battle_mode_label(BattleMode mode) -> std::string_view;

}  // namespace gedou