#include "st/app/text_port.hpp"

namespace st::app {

auto RendererTextPort::measure(std::string_view utf8, float size) const -> math::Size {
  return renderer_.measure(utf8, size);
}

auto RendererTextPort::measure_width(std::string_view utf8, float size) const -> float {
  return renderer_.measure_width(utf8, size);
}

auto RendererTextPort::line_height(float size) const -> float {
  return renderer_.line_height(size);
}

void RendererTextPort::draw(raster::Canvas& canvas, std::string_view utf8, math::Point origin,
                            float size, math::Color color) const {
  (void)renderer_.draw(canvas, utf8, origin, size, color);
}

auto RendererTextPort::ellipsize(std::string_view utf8, float size, float max_width) const
    -> std::string {
  return renderer_.ellipsize(utf8, size, max_width);
}

auto RendererTextPort::wrap(std::string_view utf8, float size, float max_width) const
    -> std::vector<std::string_view> {
  return renderer_.wrap(utf8, size, max_width);
}

auto RendererTextPort::wrap_limited(std::string_view utf8, float size, float max_width,
                                    std::size_t max_lines) const -> std::vector<std::string> {
  std::vector<std::string> lines;
  for (const auto& line : renderer_.wrap(utf8, size, max_width)) lines.emplace_back(line);
  if (lines.size() <= max_lines) return lines;
  lines.resize(max_lines);
  lines.back() = renderer_.ellipsize(lines.back(), size, max_width);
  return lines;
}

}  // namespace st::app
