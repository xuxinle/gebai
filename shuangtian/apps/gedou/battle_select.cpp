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

  // ── 整体等比缩放（视口小的时候） ──
  //
  // ⚠ 这里曾经是**固定像素堆叠**：卡片 320 + 模式 44 + 开始 52 + 三处间距，
  // 竖向一共吃掉 480 多像素。窗口一窄（如 720×480），开始按钮就被推到
  // 画布外——肉眼看不见，但命中区域还在那里，属于"看得见的按钮不见了、
  // 看不见的区域还能点"的双重问题。
  //
  // 修法：算一遍竖向总高，超过可用高度就整体等比缩小（下限 0.6，
  // 再小文字就读不清了，那种情况让它在纵向滚动/由用户放大窗口）。
  const float title_band = std::max(96.0f, viewport.height * 0.14f);   // 标题 + 副标题
  const float stacked = kCardHeight + 28.0f + kModeHeight + 26.0f + kButtonHeight;
  const float available = std::max(120.0f, viewport.height - title_band - 24.0f);
  const float fit = std::clamp(available / stacked, 0.6f, 1.0f);

  // 卡片整体水平居中：两张卡 + 间隔。窗口太窄时按**宽度**再收一次。
  const float total_width = kCardWidth * fit * 2.0f + kCardGap * fit;
  const float width_fit = std::min(1.0f, (viewport.width - 48.0f) / total_width);
  const float scale = std::min(fit, width_fit);
  const float final_card_w = kCardWidth * scale;
  const float final_card_h = kCardHeight * scale;
  const float final_gap = kCardGap * scale;

  const float cards_left = center_x - (final_card_w * 2.0f + final_gap) * 0.5f;
  const float cards_top = viewport.y + title_band;

  areas.left_card = math::Rect{cards_left, cards_top, final_card_w, final_card_h};
  areas.right_card = math::Rect{cards_left + final_card_w + final_gap, cards_top,
                                final_card_w, final_card_h};

  // 下面一排：模式切换（居中）。
  const float mode_top = cards_top + final_card_h + 28.0f * scale;
  const float final_mode_w = kModeWidth * scale;
  const float final_mode_h = kModeHeight * scale;
  areas.mode_toggle =
      math::Rect{center_x - final_mode_w * 0.5f, mode_top, final_mode_w, final_mode_h};

  // 难度 +/-（在人机模式下才有意义，画在模式按钮右侧）。
  const float final_small = kSmallButton * scale;
  areas.difficulty_minus =
      math::Rect{areas.mode_toggle.right() + 18.0f * scale,
                 mode_top + (final_mode_h - final_small) * 0.5f, final_small, final_small};
  areas.difficulty_plus =
      math::Rect{areas.difficulty_minus.right() + final_small + 6.0f * scale,
                 areas.difficulty_minus.y, final_small, final_small};

  // 开始按钮：最下方。
  const float final_button_w = kButtonWidth * scale;
  const float final_button_h = kButtonHeight * scale;
  areas.start_button = math::Rect{center_x - final_button_w * 0.5f,
                                  mode_top + final_mode_h + 26.0f * scale,
                                  final_button_w, final_button_h};
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
