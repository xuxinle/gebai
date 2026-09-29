#pragma once

/// 样式与设计 token 绑定：布局属性（Flex 子集）+ 外观属性（背景/边框/圆角/阴影）+ 排版属性。
/// 组件通过 `Style` 表达外观，具体色值来自 `Theme`（见 theme.hpp），实现"换主题不改组件代码"。

#include <cstdint>
#include <string>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"

namespace st::ui {

enum class FlexDirection : std::uint8_t { Row, Column };
enum class Align : std::uint8_t { Start, Center, End, Stretch };
enum class Justify : std::uint8_t { Start, Center, End, SpaceBetween, SpaceAround };
enum class FontWeight : std::uint8_t { Regular, Medium, SemiBold, Bold };
enum class TextAlign : std::uint8_t { Start, Center, End };

/// 自动尺寸哨兵。
inline constexpr float kAuto = -1.0f;
inline constexpr float kUnbounded = 1.0e9f;

struct Shadow {
  math::Color color{0, 0, 0, 0};
  float blur{0.0f};
  float offset_x{0.0f};
  float offset_y{0.0f};

  /// **第二层（环境光）**：与主层叠加成“关键光 + 环境光”。
  ///
  /// 单层投影在浅底上会被读成一条“灰边”，而不是“浮起来的物体”——
  /// 真实光照是近处紧、远处散的叠加，单个高斯无法同时表达两者。
  /// 两层各自与背景混合（不是先加后混），且阴影遮罩按几何参数缓存，
  /// 多一层只多一次查表合成（实测单张卡片投影 0.37 → 0.55 ms，可忽略）。
  math::Color color2{0, 0, 0, 0};
  float blur2{0.0f};
  float offset2_x{0.0f};
  float offset2_y{0.0f};

  [[nodiscard]] auto visible() const noexcept -> bool { return color.a != 0U && blur > 0.0f; }
  [[nodiscard]] auto second_visible() const noexcept -> bool {
    return color2.a != 0U && blur2 > 0.0f;
  }
};

struct Style {
  // —— 布局（Flex 子集）——
  FlexDirection direction{FlexDirection::Column};
  float gap{0.0f};
  math::Insets padding{};
  math::Insets margin{};
  bool grow{false};
  bool shrink{true};
  Align align_items{Align::Stretch};
  Align align_self{Align::Stretch};
  Justify justify{Justify::Start};
  bool wrap{false};
  float width{kAuto};
  float height{kAuto};
  float min_width{0.0f};
  float max_width{kUnbounded};
  float min_height{0.0f};
  float max_height{kUnbounded};

  // —— 外观 ——
  math::Color background{0, 0, 0, 0};
  float radius{0.0f};
  math::Color border_color{0, 0, 0, 0};
  float border_width{0.0f};
  Shadow shadow{};
  float opacity{1.0f};
  bool clip_children{false};

  // —— 排版 ——
  math::Color color{};
  float font_size{14.0f};
  FontWeight font_weight{FontWeight::Regular};
  TextAlign text_align{TextAlign::Start};
  bool monospace{false};

  [[nodiscard]] auto has_explicit_width() const noexcept -> bool { return width != kAuto; }
  [[nodiscard]] auto has_explicit_height() const noexcept -> bool { return height != kAuto; }
};

/// 常用属性构造（链式设置保持可读）。
[[nodiscard]] inline auto padding_all(float value) -> math::Insets { return math::Insets::all(value); }
[[nodiscard]] inline auto padding_xy(float horizontal, float vertical) -> math::Insets {
  return math::Insets::symmetric(horizontal, vertical);
}

}  // namespace st::ui
