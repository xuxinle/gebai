/// 镂月自绘画布的实现（见 `canvas_view.hpp`）。
///
/// 视口职责已回流内置 Viewport（第二跳）：本文件不再有几何换算/档位缩放/
/// 棋盘格/视口裁剪——只剩「画文档原生尺寸的位图 + 工具输入 + 文档级动作」。

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

auto CanvasView::zoom() const noexcept -> float {
  return viewport_ != nullptr ? viewport_->zoom() : 1.0f;
}

void CanvasView::set_viewport(st::ui::Viewport* viewport) { viewport_ = viewport; }

void CanvasView::set_document(Document* document) {
  document_ = document;
  mark_dirty();
}

void CanvasView::set_tool(Tool tool) {
  tool_ = tool;
  mark_dirty();
}

void CanvasView::set_foreground(st::math::Color color) {
  foreground_ = color;
  mark_dirty();
}

void CanvasView::set_brush_radius(float radius) {
  brush_radius_ = std::clamp(radius, 1.0f, 64.0f);
  mark_dirty();
}

auto CanvasView::cursor_color() const -> st::math::Color {
  if (document_ == nullptr || !cursor_inside_) return st::math::Color{0, 0, 0, 0};
  return document_->composite_pixel(static_cast<int>(std::floor(cursor_.x)),
                                    static_cast<int>(std::floor(cursor_.y)));
}

auto CanvasView::image_at(st::math::Point window) const -> st::math::Point {
  // 事件携带**窗口绝对坐标**：先减视口 bounds 转局部（与基类滚轮路径同式，
  // viewport.cpp 的 `to_content` 收视口局部坐标），再走基类换算到文档坐标。
  if (viewport_ == nullptr) return st::math::Point{};
  const st::math::Rect area = viewport_->bounds();
  return viewport_->to_content(st::math::Point{window.x - area.x, window.y - area.y});
}

void CanvasView::apply_theme(const st::ui::Theme& theme) {
  const auto& colors = theme.colors();
  style().background = colors.surface_sunken;
  border_ = colors.border;
}

void CanvasView::measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) {
  // **文档原生尺寸**（不再吃满视口）：缩放/平移/裁剪全部由外层 Viewport 的
  // arrange 实现。约束恒忽略——内容有自己的固有大小，「被压扁」正是「缩放」
 // 与「重排」的区别（基类给内容传无界约束也是同一理由，viewport.cpp 注释）。
  (void)context;
  (void)constraints;
  float width = 320.0f;
  float height = 200.0f;
  if (document_ != nullptr) {
    width = static_cast<float>(document_->width());
    height = static_cast<float>(document_->height());
  }
  measured_ = st::math::Size{width, height};
}

void CanvasView::paint_content(const st::ui::RenderContext& context,
                               st::raster::Surface& canvas) const {
  (void)context;
  if (document_ == nullptr) return;
  const st::math::Rect area = bounds();
  if (area.is_empty()) return;

  // 合成到离屏画布再整体贴上来：逐层 `draw_canvas` 直接画会在缩放时每层
  // 各做一次重采样（层数一多就糊，且层间插值不一致）。刻意保留（第二跳
  // 的反向警示：这条不是隐疾，是防混合模糊的正确设计，别「顺手优化」掉）。
  st::raster::Canvas composed{document_->width(), document_->height(), 1.0f};
  document_->composite(composed);
  canvas.draw_canvas(composed, area);

  // 画布边框（100% 时帮用户判断“图到哪里为止”）。
  {
    st::raster::Path frame;
    frame.move_to(st::math::Point{area.x, area.y});
    frame.move_to(st::math::Point{area.right(), area.y});
    frame.move_to(st::math::Point{area.right(), area.bottom()});
    frame.move_to(st::math::Point{area.x, area.bottom()});
    frame.close();
    canvas.stroke_path(frame, Paint::solid(border_), 1.0f);
  }
  // 笔刷光标：在光标处画一个空心圆（半径 = 笔刷半径 × 视口缩放，
  // 让“落笔会有多大”在落笔前就看得见）。光标圆心在**本元素局部**算：
  // 文档坐标 → 视口坐标 → 减去本元素 bounds 左上（元素随视口 pan 移动，
  // 光标位置取自事件时刻的换算，与本帧绘制时刻的元素位置可能有一帧差）。
  if (cursor_inside_ && (tool_ == Tool::Brush || tool_ == Tool::Eraser)) {
    const st::ui::Viewport* view = viewport();
    if (view != nullptr) {
      const st::math::Rect viewport_area = view->bounds();
      const st::math::Point in_view =
          view->to_view(st::math::Point{cursor_.x, cursor_.y});
      const st::math::Point center{
          viewport_area.x + in_view.x - area.x, viewport_area.y + in_view.y - area.y};
      const float radius = brush_radius_ * view->zoom();
      if (radius > 1.0f) {
        // 双色描边（外白内黑）：单色圆在浅底/深底上总有一侧看不见。
        const st::raster::Path ring = st::raster::make_circle(center, radius);
        canvas.stroke_path(ring, Paint::solid(st::math::Color{255, 255, 255, 200}), 1.5f);
        canvas.stroke_path(ring, Paint::solid(st::math::Color{0, 0, 0, 120}), 0.5f);
      }
    }
  }
}

auto CanvasView::stroke(st::math::Point from, st::math::Point to) -> bool {
  if (document_ == nullptr || !document_->has_layer()) return false;
  // AI 接管时不接受人工笔触（鼠标/触控也走这里）。
  if (!document_->can_edit()) return false;

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
      const st::math::Point image = image_at(event.position);
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
      if (tool_ == Tool::Move) {
        // Move 工具 = 平移模式：不消费，冒泡给外壳接左键拖拽平移
        //（旧隐疾#3：该工具曾是空操作）。
        return false;
      }
      const st::math::Point image = image_at(event.position);
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
      // **抬起 = 一笔结束**：封笔后，后面推入的补丁不再并进这一笔的历史条目。
      //
      // 不封会怎样：用户在抬起后又落同处一笔，`try_merge` 的三条判据
      //（同图层 / 同工具 / 位置相邻）**全部满足**，两笔会并成一条——
      // 撤销一次退回两笔之前，用户判定"撤销退多了"。
      if (document_ != nullptr) document_->seal_stroke();
      return false;
    case EventKind::Wheel: {
      // 缩放档位已随几何层回流外壳：滚轮（无论是否 Ctrl）冒泡给
      // `CanvasViewport`（旧实现是 Ctrl+滚轮在这里消费、无 Ctrl 放行）。
      return false;
    }
    default:
      return false;
  }
}

auto CanvasView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "zoom") return std::format("{:.2f}", static_cast<double>(zoom()));
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
    // 连续值直接进视口——吸附由 `on_view_changed` 里的 snap_zoom 收口
    //（档位不变量的唯一闸门；读回来恒是档位值）。
    if (viewport_ == nullptr) return false;
    if (const auto parsed = parse_float(value); parsed.has_value()) {
      viewport_->set_zoom(*parsed);
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
  // —— AI 控制态：编辑类动作一律拒绝 ——
  //
  // 只看类动作（`pick`/`zoom_in`/`zoom_out`/`export` 与自省类）不问门禁；其余写动作
  // 全部拦。为何在**动作面入口**统一判、而不在每个分支里判：写动作有十几个，
  // 逐个判必然漏一处——本仓已因“入口多、逐个禁用”踩过一次（裁云的 set_opacity）。
  //
  // 白名单写在前面（而不是黑名单）：将来新加的写动作**默认被拦**，
  // 忘了登记只是“该能用却不能用”（看得见、好查），而不是“该拦住却放行了”。
  if (document_ != nullptr && !document_->can_edit()) {
    const bool read_only = action == "pick" || action == "zoom_in" ||
                           action == "zoom_out" || action == "export" || action == "refresh";
    if (!read_only) return false;
  }
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
    // 坐标是**文档坐标**——与视口无关（命令式动作不经坐标换算，e2e 契约原样）。
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
  // e2e 契约动作 `zoom_in`/`zoom_out`：缩放真值在外壳，经**委托转发**保住
  // `invoke canvas zoom_in` 的协议面（外壳是本元素的父，控制通道 find id=canvas
  // 只到本元素）。
  if (action == "zoom_in" || action == "zoom_out") {
    auto* shell = dynamic_cast<CanvasViewport*>(viewport_);
    if (shell == nullptr) return false;
    if (action == "zoom_in") shell->zoom_in();
    else shell->zoom_out();
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

// —— CanvasViewport：全部定义在 canvas_view.hpp（头内 inline）——
// 无 key function（所有虚函数含构造路径均在头），vtable/typeinfo 按需弱发射，
// tests 目标（不链 examples 源）也能链接（vtable 错锚在 cpp 的教训见
// `wants_pan` 处的链接说明）。本文件不再有任何 CanvasViewport:: 定义。

}  // namespace louyue
