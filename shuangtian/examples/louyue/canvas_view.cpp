/// 镂月自绘画布视口的实现（见 `canvas_view.hpp`）。

#include "canvas_view.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/raster/paint.hpp"

namespace louyue {

namespace {

using st::raster::Paint;
using st::ui::Event;
using st::ui::EventKind;

/// 缩放档位（设计文档 §3.3：25/50/100/200/400/800）。
/// **离散档位**而不是连续缩放：连续缩放会让像素对齐一直在变，
/// 100% 时"一个图素对一个像素"这个最重要的档位反而很难停准。
constexpr float kZoomSteps[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
constexpr int kZoomCount = static_cast<int>(std::size(kZoomSteps));

/// 棋盘格边长（视口像素）。
constexpr float kCheckerSize = 8.0f;

}  // namespace

auto tool_name(Tool tool) -> const char* {
  switch (tool) {
    case Tool::Move: return "移动";
    case Tool::Brush: return "画笔";
    case Tool::Eraser: return "橡皮";
    case Tool::Bucket: return "油漆桶";
    case Tool::Eyedropper: return "吸管";
    case Tool::Select: return "矩形选区";
    case Tool::Crop: return "裁切";
  }
  return "画笔";
}

auto tool_icon(Tool tool) -> const char* {
  switch (tool) {
    case Tool::Move: return "arrow-right";
    case Tool::Brush: return "brush";
    case Tool::Eraser: return "eraser";
    case Tool::Bucket: return "bucket";
    case Tool::Eyedropper: return "eyedropper";
    case Tool::Select: return "square";
    case Tool::Crop: return "crop";
  }
  return "brush";
}

CanvasView::CanvasView() { set_focusable(true); }

void CanvasView::set_document(Document* document) {
  document_ = document;
  mark_dirty();
}

void CanvasView::set_tool(Tool tool) {
  tool_ = tool;
  mark_dirty();
}

void CanvasView::set_zoom(float zoom) {
  zoom_ = std::clamp(zoom, kZoomSteps[0], kZoomSteps[kZoomCount - 1]);
  mark_dirty();
}

void CanvasView::zoom_step(int delta) {
  int index = 0;
  float best = 1.0e9f;
  for (int i = 0; i < kZoomCount; ++i) {
    const float distance = std::abs(kZoomSteps[i] - zoom_);
    if (distance < best) {
      best = distance;
      index = i;
    }
  }
  index = std::clamp(index + delta, 0, kZoomCount - 1);
  set_zoom(kZoomSteps[index]);
}

void CanvasView::set_foreground(st::math::Color color) {
  foreground_ = color;
  mark_dirty();
}

void CanvasView::set_background(st::math::Color color) { background_ = color; }

void CanvasView::set_brush_radius(float radius) {
  brush_radius_ = std::clamp(radius, 1.0f, 64.0f);
  mark_dirty();
}

auto CanvasView::cursor_color() const -> st::math::Color {
  if (document_ == nullptr || !cursor_inside_) return st::math::Color{0, 0, 0, 0};
  return document_->composite_pixel(static_cast<int>(std::floor(cursor_.x)),
                                    static_cast<int>(std::floor(cursor_.y)));
}

auto CanvasView::canvas_rect() const -> st::math::Rect {
  if (document_ == nullptr) return st::math::Rect{};
  const float w = static_cast<float>(document_->width()) * zoom_;
  const float h = static_cast<float>(document_->height()) * zoom_;
  const st::math::Rect area = bounds();
  return st::math::Rect{area.x + (area.width - w) * 0.5f, area.y + (area.height - h) * 0.5f, w, h};
}

auto CanvasView::to_image(st::math::Point view) const -> st::math::Point {
  const st::math::Rect area = canvas_rect();
  if (zoom_ <= 0.0f) return st::math::Point{};
  return st::math::Point{(view.x - area.x) / zoom_, (view.y - area.y) / zoom_};
}

auto CanvasView::to_view(st::math::Point image) const -> st::math::Point {
  const st::math::Rect area = canvas_rect();
  return st::math::Point{area.x + image.x * zoom_, area.y + image.y * zoom_};
}

void CanvasView::apply_theme(const st::ui::Theme& theme) {
  const auto& colors = theme.colors();
  style().background = colors.surface_sunken;
  checker_light_ = colors.surface;
  // 棋盘格的深色格：在 sunken 底上再压一档，保证两种格能分辨
  //（同色或过近会让"透明区"看起来像纯色块）。
  checker_dark_ = colors.surface_alt;
  border_ = colors.border;
}

void CanvasView::measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) {
  (void)context;
  // 视口占满可用空间（它在外层是 `grow` 的），最小给一点避免布局塌成 0。
  float width = 320.0f;
  float height = 240.0f;
  if (constraints.max_width < st::ui::kUnbounded) width = constraints.max_width;
  if (constraints.max_height < st::ui::kUnbounded) height = constraints.max_height;
  measured_ = st::math::Size{width, height};
}

void CanvasView::paint_checkerboard(st::raster::Surface& canvas, st::math::Rect area) const {
  canvas.fill_rect(area, Paint::solid(checker_light_));
  // 只画"深色格"：亮格由底色承担，画两遍纯属浪费（大画布下可见）。
  const int cols = static_cast<int>(std::ceil(area.width / kCheckerSize));
  const int rows = static_cast<int>(std::ceil(area.height / kCheckerSize));
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < cols; ++col) {
      if (((row + col) & 1) == 0) continue;
      const float x = area.x + static_cast<float>(col) * kCheckerSize;
      const float y = area.y + static_cast<float>(row) * kCheckerSize;
      const float w = std::min(kCheckerSize, area.right() - x);
      const float h = std::min(kCheckerSize, area.bottom() - y);
      if (w <= 0.0f || h <= 0.0f) continue;
      canvas.fill_rect(st::math::Rect{x, y, w, h}, Paint::solid(checker_dark_));
    }
  }
}

void CanvasView::paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const {
  (void)context;
  const st::math::Rect area = bounds();
  if (area.is_empty()) return;

  // 视口底（画布之外的留白）。
  canvas.fill_rect(area, Paint::solid(style().background));
  if (document_ == nullptr) return;

  const st::math::Rect target = canvas_rect();
  // 画布区裁剪到视口内（缩放 >100% 时画布会超出视口）。
  const st::math::Rect visible{
      std::max(target.x, area.x), std::max(target.y, area.y),
      std::min(target.right(), area.right()) - std::max(target.x, area.x),
      std::min(target.bottom(), area.bottom()) - std::max(target.y, area.y)};
  if (visible.width <= 0.0f || visible.height <= 0.0f) return;

  paint_checkerboard(canvas, visible);

  // 合成到离屏画布再整体贴上来：逐层 `draw_canvas` 直接画到视口会在缩放时
  // 每层各做一次重采样（层数一多就糊，且层间插值不一致）。
  st::raster::Canvas composed{document_->width(), document_->height(), 1.0f};
  document_->composite(composed);
  canvas.draw_canvas(composed, target);

  // 画布边框（100% 时帮用户判断“图到哪里为止”）。
  {
    st::raster::Path frame;
    frame.move_to(st::math::Point{target.x, target.y});
    frame.line_to(st::math::Point{target.right(), target.y});
    frame.line_to(st::math::Point{target.right(), target.bottom()});
    frame.line_to(st::math::Point{target.x, target.bottom()});
    frame.close();
    canvas.stroke_path(frame, Paint::solid(border_), 1.0f);
  }
  // 笔刷光标：在光标处画一个空心圆（半径 = 笔刷半径 × 缩放），
  // 让“落笔会有多大”在落笔前就看得见。
  if (cursor_inside_ && (tool_ == Tool::Brush || tool_ == Tool::Eraser)) {
    const float r = brush_radius_ * zoom_;
    const st::math::Point center = to_view(st::math::Point{cursor_.x + 0.5f, cursor_.y + 0.5f});
    if (r > 1.0f) {
      // 双色描边（外白内黑）：单色圆在浅底/深底上总有一侧看不见。
      const st::raster::Path ring = st::raster::make_circle(center, r);
      canvas.stroke_path(ring, Paint::solid(st::math::Color{255, 255, 255, 200}), 1.5f);
      canvas.stroke_path(ring, Paint::solid(st::math::Color{0, 0, 0, 120}), 0.5f);
    }
  }
}

auto CanvasView::stroke(st::math::Point from, st::math::Point to) -> bool {
  if (document_ == nullptr || !document_->has_layer()) return false;

  // 橡皮与画笔的差别只在"画什么"：橡皮不是画白色，而是把该处抹成透明
  //（否则黑色背景上的橡皮笔迹是白的，观感完全不对）。
  const bool erase = tool_ == Tool::Eraser;
  const st::math::Color color = erase ? st::math::Color{0, 0, 0, 0} : foreground_;

  const std::size_t layer = document_->active_layer();
  // 撤销补丁：先算"受影响矩形"，再存 before → 落笔 → 存 after。
  // ⚠ 顺序不能反：先存 before 才能拿到改动前的像素。
  const st::math::Rect area = stroke_bounds(from, to, brush_radius_);
  PixelPatchCommand patch{layer, area, erase ? "橡皮" : "画笔"};
  patch.capture_before(*document_);
  (void)stroke_segment(document_->active_pixels(), from, to, brush_radius_, color, erase);
  patch.capture_after(*document_);
  if (!patch.changed()) return false;   // 空笔（同一处重复落笔）不入历史

  document_->push(std::make_unique<PixelPatchCommand>(std::move(patch)));
  mark_dirty();
  if (on_document_changed) on_document_changed();
  return true;
}

auto CanvasView::pick_at(st::math::Point point) -> bool {
  if (document_ == nullptr) return false;
  const int x = static_cast<int>(std::floor(point.x));
  const int y = static_cast<int>(std::floor(point.y));
  if (x < 0 || y < 0 || x >= document_->width() || y >= document_->height()) return false;
  foreground_ = document_->composite_pixel(x, y);
  mark_dirty();
  if (on_document_changed) on_document_changed();
  return true;
}

auto CanvasView::on_event(const st::ui::RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::MouseMove: {
      const st::math::Point image = to_image(event.position);
      cursor_ = st::math::Point{std::floor(image.x), std::floor(image.y)};
      cursor_inside_ = document_ != nullptr && cursor_.x >= 0.0f && cursor_.y >= 0.0f &&
                       cursor_.x < static_cast<float>(document_->width()) &&
                       cursor_.y < static_cast<float>(document_->height());
      if (dragging_) {
        if (tool_ == Tool::Brush || tool_ == Tool::Eraser) {
          (void)stroke(last_, image);
        }
        last_ = image;
      }
      mark_dirty();
      return false;
    }
    case EventKind::HoverOut:
      cursor_inside_ = false;
      mark_dirty();
      return false;
    case EventKind::MouseDown: {
      if (event.button != 1) return false;
      set_focused(true);
      const st::math::Point image = to_image(event.position);
      if (tool_ == Tool::Eyedropper) {
        (void)pick_at(image);
        return true;
      }
      if (tool_ == Tool::Brush || tool_ == Tool::Eraser) {
        dragging_ = true;
        last_ = image;
        // 单点落笔也要画出一个点（拖动才开始画的话，点一下没反应）。
        (void)stroke(image, image);
      }
      return true;
    }
    case EventKind::MouseUp:
      dragging_ = false;
      return false;
    case EventKind::Wheel: {
      // Ctrl+滚轮 = 缩放（设计文档 §3.3）；不按 Ctrl 的滚轮留给外层滚动。
      if (event.ctrl) {
        zoom_step(event.wheel_delta > 0.0f ? 1 : -1);
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

auto CanvasView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "zoom") return std::format("{:.2f}", static_cast<double>(zoom_));
  if (name == "tool") return std::string(tool_name(tool_));
  if (name == "brush_radius") return std::format("{:.1f}", static_cast<double>(brush_radius_));
  if (name == "cursor") {
    return std::format("{},{}", static_cast<int>(cursor_.x), static_cast<int>(cursor_.y));
  }
  if (name == "cursor_color") {
    const st::math::Color color = cursor_color();
    return std::format("#{:02X}{:02X}{:02X}", color.r, color.g, color.b);
  }
  if (name == "foreground") {
    return std::format("#{:02X}{:02X}{:02X}", foreground_.r, foreground_.g, foreground_.b);
  }
  if (name == "canvas_size") {
    if (document_ == nullptr) return std::string("0x0");
    return std::format("{}x{}", document_->width(), document_->height());
  }
  if (name == "layer_count") {
    return std::to_string(document_ == nullptr ? 0U : document_->layers().size());
  }
  return std::nullopt;
}

auto CanvasView::set_property(std::string_view name, std::string_view value) -> bool {
  const auto parse_float = [](std::string_view text) -> std::optional<float> {
    try {
      return std::stof(std::string(text));
    } catch (...) {
      return std::nullopt;
    }
  };
  if (name == "zoom") {
    if (const auto parsed = parse_float(value); parsed.has_value()) {
      set_zoom(*parsed);
      return true;
    }
    return false;
  }
  if (name == "brush_radius") {
    if (const auto parsed = parse_float(value); parsed.has_value()) {
      set_brush_radius(*parsed);
      return true;
    }
    return false;
  }
  if (name == "tool") {
    for (const Tool candidate : {Tool::Move, Tool::Brush, Tool::Eraser, Tool::Bucket,
                                 Tool::Eyedropper, Tool::Select, Tool::Crop}) {
      if (value == tool_name(candidate)) {
        set_tool(candidate);
        return true;
      }
    }
    return false;
  }
  return false;
}

auto CanvasView::property_names() const -> std::vector<std::string_view> {
  return {"zoom", "tool", "brush_radius", "cursor", "cursor_color", "foreground", "canvas_size",
          "layer_count"};
}

auto CanvasView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  const auto parse_pair = [](std::string_view text) -> std::optional<std::pair<float, float>> {
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    try {
      return std::pair<float, float>{std::stof(std::string(text.substr(0, comma))),
                                     std::stof(std::string(text.substr(comma + 1)))};
    } catch (...) {
      return std::nullopt;
    }
  };

  if (action == "stroke") {
    // 形如 `x1,y1,x2,y2` —— 设计文档 §3.4 约定的动作面（e2e 依赖，不可改名）。
    const std::size_t comma = argument.find(',');
    if (comma == std::string_view::npos) return false;
    const std::string_view rest = argument.substr(comma + 1);
    const std::size_t comma2 = rest.find(',');
    if (comma2 == std::string_view::npos) return false;
    const std::string_view rest2 = rest.substr(comma2 + 1);
    const std::size_t comma3 = rest2.find(',');
    if (comma3 == std::string_view::npos) return false;
    try {
      const float x1 = std::stof(std::string(argument.substr(0, comma)));
      const float y1 = std::stof(std::string(rest.substr(0, comma2)));
      const float x2 = std::stof(std::string(rest2.substr(0, comma3)));
      const float y2 = std::stof(std::string(rest2.substr(comma3 + 1)));
      return stroke(st::math::Point{x1, y1}, st::math::Point{x2, y2});
    } catch (...) {
      return false;
    }
  }
  if (action == "pick") {
    if (const auto pair = parse_pair(argument); pair.has_value()) {
      return pick_at(st::math::Point{pair->first, pair->second});
    }
    return false;
  }
  if (action == "zoom_in") {
    zoom_step(1);
    return true;
  }
  if (action == "zoom_out") {
    zoom_step(-1);
    return true;
  }
  // —— 文档级动作（设计文档 §3.4：不可改名，`tools/louyue_e2e.py` 依赖）——
  //
  // 为何挂在**画布元素**上而不是另设一个控制器元素：控制通道的 `invoke` 要
  // 先 `find` 到一个 id；画布已经是 e2e 必然要用的靶点，再引入一个只为收动作
  // 而存在的元素会让协议面多一个“它不是界面的一部分”的特例。
  if (document_ == nullptr) return false;

  if (action == "export") {
    if (argument.empty()) return false;
    return document_->export_png(argument);
  }
  if (action == "undo") {
    const bool changed = document_->undo();
    if (changed) notify();
    return changed;
  }
  if (action == "redo") {
    const bool changed = document_->redo();
    if (changed) notify();
    return changed;
  }
  if (action == "add_layer") {
    document_->add_layer(std::format("图层 {}", document_->layers().size() + 1U));
    notify();
    return true;
  }
  if (action == "remove_layer") {
    // 参数是**模型索引**（自底向上）——与 `set_blend` 同口径。
    std::size_t index = document_->active_layer();
    if (!argument.empty()) {
      try {
        index = static_cast<std::size_t>(std::stoul(std::string(argument)));
      } catch (...) {
        return false;
      }
    }
    const bool removed = document_->remove_layer(index);
    if (removed) notify();
    return removed;
  }
  // 带「图层索引 + 值」两个参数的动作共用一个解析（三处各写一遍易漂移）。
  const auto parse_indexed = [&](float& value_out) -> std::optional<std::size_t> {
    const std::size_t space = argument.find(' ');
    if (space == std::string_view::npos) return std::nullopt;
    try {
      const auto layer =
          static_cast<std::size_t>(std::stoul(std::string(argument.substr(0, space))));
      if (layer >= document_->layers().size()) return std::nullopt;
      value_out = std::stof(std::string(argument.substr(space + 1)));
      return layer;
    } catch (...) {
      return std::nullopt;
    }
  };
  if (action == "set_opacity") {
    float value = 1.0f;
    const auto layer = parse_indexed(value);
    if (!layer.has_value()) return false;
    document_->layers()[*layer].opacity = std::clamp(value, 0.0f, 1.0f);
    notify();
    return true;
  }
  if (action == "set_visible") {
    float value = 1.0f;
    const auto layer = parse_indexed(value);
    if (!layer.has_value()) return false;
    document_->layers()[*layer].visible = value != 0.0f;
    notify();
    return true;
  }
  if (action == "set_blend") {
    // 形如 `<图层索引> <模式索引>`（模式索引对应 `kBlendModes`）。
    float value = 0.0f;
    const auto layer = parse_indexed(value);
    const int mode = static_cast<int>(value);
    if (!layer.has_value() || mode < 0 || mode >= static_cast<int>(kBlendModeCount)) return false;
    document_->layers()[*layer].blend = kBlendModes[mode];
    notify();
    return true;
  }
  if (action == "clear_layer") {
    // 把活动图层清成透明（e2e 需要“重置到已知状态”而不重建文档——
    // 重建文档会清掉历史，让“撤销回到落笔前”这类断言失去参照）。
    if (!document_->has_layer()) return false;
    document_->active_pixels().clear(st::math::Color{0, 0, 0, 0});
    document_->set_dirty(true);
    notify();
    return true;
  }
  if (action == "reset_layers") {
    // 只留一层并清成指定底色（形如 `#rrggbb`；空则透明）——给 e2e 一个
    // “回到干净起点”的口子，避免重建文档（那会清掉历史）。
    while (document_->layers().size() > 1U) {
      (void)document_->remove_layer(document_->layers().size() - 1U);
    }
    document_->set_active_layer(0U);
    document_->layers().front().blend = BlendMode::SrcOver;
    document_->layers().front().opacity = 1.0f;
    document_->layers().front().visible = true;
    if (argument.empty()) {
      document_->active_pixels().clear(st::math::Color{0, 0, 0, 0});
    } else if (argument.size() >= 7U && argument.front() == '#') {
      try {
        const auto channel = [&](std::size_t at) {
          return static_cast<std::uint8_t>(
              std::stoul(std::string(argument.substr(at, 2)), nullptr, 16));
        };
        document_->active_pixels().clear(
            st::math::Color{channel(1), channel(3), channel(5), static_cast<std::uint8_t>(255)});
      } catch (...) {
        return false;
      }
    } else {
      return false;
    }
    document_->set_dirty(true);
    notify();
    return true;
  }
  if (action == "clear_history") {
    document_->clear_history();
    notify();
    return true;
  }
  if (action == "set_pixel") {
    // 形如 `x y #rrggbb[aa]`——给 e2e 一个“不靠笔画”也能造出确定性像素的口子
    //（笔画受笔刷半径/插值影响，而单像素写入是精确的）。
    const std::size_t first = argument.find(' ');
    if (first == std::string_view::npos) return false;
    const std::size_t second = argument.find(' ', first + 1);
    if (second == std::string_view::npos) return false;
    try {
      const int x = std::stoi(std::string(argument.substr(0, first)));
      const int y = std::stoi(std::string(argument.substr(first + 1, second - first - 1)));
      if (x < 0 || y < 0 || x >= document_->width() || y >= document_->height()) return false;
      const std::string hex(argument.substr(second + 1));
      if (hex.size() < 7U || hex.front() != '#') return false;
      const auto channel = [&](std::size_t at) {
        return static_cast<std::uint8_t>(std::stoul(hex.substr(at, 2), nullptr, 16));
      };
      const st::math::Color color{channel(1), channel(3), channel(5),
                                  hex.size() >= 9 ? channel(7) : static_cast<std::uint8_t>(255)};
      document_->active_pixels().set_pixel(x, y, color);
      document_->set_dirty(true);
      notify();
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
}

void CanvasView::notify() {
  mark_dirty();
  if (on_document_changed) on_document_changed();
}

// —— Swatch（纯色块）——

void Swatch::measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) {
  (void)context;
  float width = 48.0f;
  float height = 28.0f;
  if (constraints.max_width < st::ui::kUnbounded) width = constraints.max_width;
  if (constraints.max_height < st::ui::kUnbounded) height = constraints.max_height;
  measured_ = st::math::Size{width, height};
}

void Swatch::paint_content(const st::ui::RenderContext& context,
                           st::raster::Surface& canvas) const {
  (void)context;
  const st::math::Rect area = bounds();
  if (area.is_empty()) return;
  // 底色先铺白再叠色：前景色常带透明度（吸管可能取得半透明白），
  // 不铺底时“白色前景”在一片深色面板上看着就是“没画”。
  canvas.fill_rect(area, Paint::solid(st::math::Color{255, 255, 255, 255}), 4.0f);
  canvas.fill_rect(area, Paint::solid(color_), 4.0f);
  canvas.fill_rect(area, Paint::solid(border_), 4.0f);
}

auto Swatch::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "color") {
    return std::format("#{:02X}{:02X}{:02X}", color_.r, color_.g, color_.b);
  }
  return std::nullopt;
}

auto Swatch::property_names() const -> std::vector<std::string_view> { return {"color"}; }

}  // namespace louyue
