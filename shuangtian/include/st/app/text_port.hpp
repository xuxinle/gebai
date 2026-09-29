#pragma once

/// 字体引擎 → UI 文本端口的适配层（依赖倒置：`ui` 层不认识 `text` 层）。
///
/// 独立成文件的原因：**测试与示例都需要真实的文本端口**（度量/绘制），若它藏在 `app.cpp` 的
/// 匿名命名空间里，任何脱离 `Application` 的组件测试就只能退化用 `NullTextPort`（宽度恒为 0），
/// 布局与命中相关断言全部失效。

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/text/text.hpp"
#include "st/ui/text_port.hpp"

namespace st::app {

/// 用 `text::TextRenderer` 驱动的文本端口。
///
/// DPI：入参一律**逻辑坐标**；`TextRenderer` 内部按画布 `device_scale` 以物理分辨率栅格化字形
/// （布局稳定、字形锐利），此处不做换算，避免重复缩放。
class RendererTextPort final : public ui::TextPort {
 public:
  explicit RendererTextPort(const text::TextRenderer& renderer) : renderer_(renderer) {}

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

 private:
  const text::TextRenderer& renderer_;
};

/// 便捷工厂。
[[nodiscard]] inline auto make_text_port(const text::TextRenderer& renderer)
    -> std::unique_ptr<ui::TextPort> {
  return std::make_unique<RendererTextPort>(renderer);
}

}  // namespace st::app
