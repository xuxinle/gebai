#pragma once

/// 文本绘制端口：UI 层对"字体引擎"的唯一依赖面（依赖倒置）。
/// 目的：`ui` 层不依赖 `text` 层实现（编译解耦、可注入假实现做布局单测）；
/// 由 `app` 层用 `text::TextRenderer` 适配（见 `include/st/app/text_port.hpp`）。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"

namespace st::ui {

class TextPort {
 public:
  virtual ~TextPort() = default;

  /// 度量文本尺寸（宽 × 行高）。
  [[nodiscard]] virtual auto measure(std::string_view utf8, float size) const -> math::Size = 0;
  [[nodiscard]] virtual auto measure_width(std::string_view utf8, float size) const -> float = 0;
  [[nodiscard]] virtual auto line_height(float size) const -> float = 0;
  /// `origin` 为行左上角。
  virtual void draw(raster::Canvas& canvas, std::string_view utf8, math::Point origin, float size,
                    math::Color color) const = 0;
  [[nodiscard]] virtual auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string = 0;
  [[nodiscard]] virtual auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> = 0;
  /// 按最大行数折行（超出末行以省略号收尾）。
  [[nodiscard]] virtual auto wrap_limited(std::string_view utf8, float size, float max_width,
                                          std::size_t max_lines) const -> std::vector<std::string> = 0;
};

/// 空实现：无字体环境下布局仍可运行（文本宽度按 0 计，绘制为 no-op）。
class NullTextPort final : public TextPort {
 public:
  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> math::Size override;
  [[nodiscard]] auto measure_width(std::string_view utf8, float size) const -> float override;
  [[nodiscard]] auto line_height(float size) const -> float override;
  void draw(raster::Canvas& canvas, std::string_view utf8, math::Point origin, float size,
            math::Color color) const override;
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override;
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override;
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const -> std::vector<std::string> override;

  [[nodiscard]] static auto instance() -> const NullTextPort&;
};

}  // namespace st::ui
