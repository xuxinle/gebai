#include "battle_select.hpp"

#include <algorithm>

namespace gedou {

namespace {

/// 卡片与按钮的固定尺寸（逻辑像素）。
///
/// 用**固定值**而不是按视口比例：选人界面是"读文字 + 看立绘"的界面，
/// 元素缩得太小就没法读、放得太大又显得空。固定尺寸在常见窗口
/// （1180×680 到 1920×1080）里都合适。
constexpr float kCardWidth = 240.0f;
constexpr float kCardHeight = 320.0f;
constexpr float kCardGap = 48.0f;
constexpr float kButtonWidth = 220.0f;
constexpr float kButtonHeight = 52.0f;
constexpr float kModeWidth = 240.0f;
constexpr float kModeHeight = 44.0f;
constexpr float kSmallButton = 34.0f;

}  // namespace

auto battle_mode_label(BattleMode mode) -> std::string_view {
  switch (mode) {
    case BattleMode::PlayerVsAi: return "人机对战";
    case BattleMode::AiVsAi: return "机机对战";
  }
  return "人机对战";
}

auto select_layout(math::Rect viewport) -> SelectHitAreas {
  SelectHitAreas areas{};
  if (viewport.is_empty()) return areas;

  const float center_x = viewport.x + viewport.width * 0.5f;
  // 卡片整体水平居中：两张卡 + 间隔。
  const float total_width = kCardWidth * 2.0f + kCardGap;
  const float cards_left = center_x - total_width * 0.5f;
  // 垂直：略偏上，给下方的按钮区留位置。
  const float cards_top = viewport.y + std::max(80.0f, viewport.height * 0.16f);

  areas.left_card = math::Rect{cards_left, cards_top, kCardWidth, kCardHeight};
  areas.right_card =
      math::Rect{cards_left + kCardWidth + kCardGap, cards_top, kCardWidth, kCardHeight};

  // 下面一排：模式切换（居中）。
  const float mode_top = cards_top + kCardHeight + 28.0f;
  areas.mode_toggle =
      math::Rect{center_x - kModeWidth * 0.5f, mode_top, kModeWidth, kModeHeight};

  // 难度 +/-（在人机模式下才有意义，画在模式按钮右侧）。
  areas.difficulty_minus =
      math::Rect{areas.mode_toggle.right() + 18.0f, mode_top + (kModeHeight - kSmallButton) * 0.5f,
                 kSmallButton, kSmallButton};
  areas.difficulty_plus =
      math::Rect{areas.difficulty_minus.right() + kSmallButton + 6.0f,
                 areas.difficulty_minus.y, kSmallButton, kSmallButton};

  // 开始按钮：最下方。
  areas.start_button = math::Rect{center_x - kButtonWidth * 0.5f,
                                  mode_top + kModeHeight + 26.0f, kButtonWidth, kButtonHeight};
  return areas;
}

auto select_hit_test(const SelectHitAreas& areas, math::Point point) -> int {
  const auto inside = [&point](math::Rect rect) {
    if (rect.is_empty()) return false;
    return point.x >= rect.x && point.x <= rect.right() && point.y >= rect.y &&
           point.y <= rect.bottom();
  };
  // **按钮优先**：它们压在卡片上方（视觉上也是）。反过来判的话，
  // 点"开始"会被当成"选右侧角色"（实测过一次：点了开始却换了人）。
  if (inside(areas.start_button)) return 3;
  if (inside(areas.mode_toggle)) return 2;
  if (inside(areas.difficulty_minus)) return 4;
  if (inside(areas.difficulty_plus)) return 5;
  if (inside(areas.left_card)) return 0;
  if (inside(areas.right_card)) return 1;
  return -1;
}

}  // namespace gedou
