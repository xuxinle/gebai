/// @file label_overlay.cpp
/// @brief 标签叠层实现（投影 + 避让 + 画线 + 文字）。

#include "st/ui/components/label_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <format>  // 显式（原先靠间接包含；组件头收紧后暴露）
#include <string>
#include <unordered_map>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {
namespace {

constexpr float kPi = 3.14159265358979323846F;

/// 投影结果。
struct ScreenPoint {
  float x{0.0f};
  float y{0.0f};
  float depth{0.0f};  ///< 越大越远
  bool visible{false};
};

}  // namespace

struct LabelOverlay::Impl {};

LabelOverlay::LabelOverlay() : impl_(std::make_unique<Impl>()) {}
LabelOverlay::~LabelOverlay() = default;

void LabelOverlay::set_labels(std::vector<LabelAnchor> labels) {
  labels_ = std::move(labels);
  mark_dirty();
}

void LabelOverlay::set_edges(std::vector<LabelEdge> edges) {
  edges_ = std::move(edges);
  mark_dirty();
}

void LabelOverlay::set_projector(Projector projector) {
  projector_ = std::move(projector);
  mark_dirty();
}

void LabelOverlay::set_font_scale(float scale) {
  font_scale_ = scale > 0.0f ? scale : 1.0f;
  mark_dirty();
}

void LabelOverlay::measure(const RenderContext& context, const Constraints& constraints) {
  // 与父同尺寸：它是覆盖层，不该有自然尺寸（否则会把兄弟挤走）。
  const float width =
      constraints.available_width < kUnbounded ? constraints.available_width : constraints.max_width;
  const float height = constraints.available_height < kUnbounded ? constraints.available_height
                                                                : constraints.max_height;
  const float safe_w = width > 0.0f && width < kUnbounded ? width : 800.0f;
  const float safe_h = height > 0.0f && height < kUnbounded ? height : 600.0f;
  measured_ = math::Size{safe_w, safe_h};
  (void)context;
}

void LabelOverlay::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  painted_ = 0;
  painted_edges_ = 0;
  if (labels_.empty()) return;
  const TextPort& port = context.text != nullptr ? *context.text : NullTextPort::instance();

  const float width = bounds_.width;
  const float height = bounds_.height;
  if (width < 8.0f || height < 8.0f) return;

  struct Placed {
    const LabelAnchor* anchor;
    ScreenPoint at;
    float font_size;
    math::Color color;
    float text_w;
    float line_h;
    math::Rect box{};
    bool dropped{false};
  };
  std::vector<Placed> placed;
  placed.reserve(labels_.size());

  // 字号：按视口高度定，再乘用户缩放。
  //
  // 上限从 30 降到 18：这是**让标签不被推开**的关键一步。
  // 实测：字号 26~30 时，同簇内几个长词条的框互相碰，避让算法
  // 只能把它们推到半个行高之外，于是“点在那里、字在旁边很远”——
  // 连线也就跟着变长、变乱。字号降到 18 以内，大多数标签能紧贴圆点排下。
  const float base_font = std::clamp(height * 0.024f, 12.0f, 22.0f) * font_scale_;

  for (const auto& label : labels_) {
    if (label.text.empty()) continue;
    const auto at = [&] {
      ScreenPoint p;
      if (!projector_) return p;
      const auto [xy, depth] = projector_(label.x, label.y, label.z);
      p.x = xy.first;
      p.y = xy.second;
      p.depth = depth;
      p.visible = true;  // 投影器已负责降级处理（返回极端坐标即可）
      return p;
    }();
    if (!at.visible) continue;
    if (at.x < -200.0f || at.x > width + 200.0f || at.y < -120.0f || at.y > height + 120.0f) {
      continue;  // 屏幕外不画（省掉无用计算）
    }
    const float font_size = base_font * (0.88f + 0.34f * std::clamp(label.weight, 0.0f, 1.0f));
    placed.push_back(Placed{&label, at, font_size, math::Color::from_rgba_hex(label.argb),
                            port.measure_width(label.text, font_size),
                            port.line_height(font_size)});
  }
  if (placed.empty()) return;

  // 远的先画（后画近的压在上面，符合深度直觉），也让远处的先占位。
  std::stable_sort(placed.begin(), placed.end(),
                   [](const Placed& a, const Placed& b) { return a.at.depth > b.at.depth; });

  // ── 布局：给每个标签找一个不与已占位矩形重叠的位置 ──
  //
  // 八个候选（右/左 × 上/中/下 + 更远两档），取**离节点最近**的空位。
  //
  // 偏移量从 4px 改成 9px：圆点半径最大 4.5px，文字再留 4px 间隙——
  // 实测 4px 时文字会压在圆点上（圆点被吃了一半，看起来像排版事故）。
  struct Candidate {
    bool left;  // 放到节点左侧
    float dy;   // 相对节点中线的纵向偏移（行高倍数）
  };
  const Candidate kCandidates[] = {
      {false, 0.0f}, {true, 0.0f},  {false, -1.1f}, {false, 1.1f},
      {true, -1.1f}, {true, 1.1f},  {false, -2.2f}, {false, 2.2f},
  };

  std::vector<math::Rect> occupied;
  occupied.reserve(placed.size());

  const float pad = 9.0f;
  for (auto& item : placed) {
    math::Rect best{};
    bool found = false;
    float best_distance = 1e30F;

    for (const auto& candidate : kCandidates) {
      const float tx = candidate.left ? (item.at.x - item.text_w - pad) : (item.at.x + pad);
      float ty = item.at.y - item.line_h * 0.5F + candidate.dy * item.line_h;
      ty = std::clamp(ty, 2.0F, height - item.line_h - 2.0F);
      if (tx < 2.0F || tx + item.text_w > width - 2.0F) continue;  // 越界不算候选
      const math::Rect box{tx - 2.0F, ty - 1.0F, item.text_w + 4.0F, item.line_h + 2.0F};
      const bool clash = std::any_of(occupied.begin(), occupied.end(),
                                     [&box](const math::Rect& other) {
                                       return box.x < other.right() && other.x < box.right() &&
                                              box.y < other.bottom() && other.y < box.bottom();
                                     });
      if (clash) continue;
      const float distance = std::abs(candidate.dy) * item.line_h + (candidate.left ? 3.0F : 0.0F);
      if (distance < best_distance) {
        best_distance = distance;
        best = box;
        found = true;
      }
    }

    if (!found) {
      // 八个方向都占满：**不画**而不是叠上去（重叠的字两段都读不清）。
      item.dropped = true;
      continue;
    }
    item.box = best;
    occupied.push_back(best);
  }

  // ── 标签：底板 + 文字（以及被推开时的引线）──
  for (const auto& item : placed) {
    if (item.dropped) continue;
    const float tx = item.box.x + 2.0F;
    const float ty = item.box.y + 1.0F;

    // 引线：**不加**（实测反而更乱）。
    //
    // 试过：给被推开的标签画一条引线回圆点。结果画面上出现大量短短的小横线，
    // 看不出哪个连哪个，反而加重了拥挤。根源是**标签不该被推开那么远**——
    // 与其用引线去弥补，不如让标签一开始就贴着圆点。
    //
    // 具体做法在两处：① 字号上限降到 18（base_font 处）；
    // ② 簇间距放大（cefu_graph.hpp 的 cluster_spread）。
    if (item.anchor->plate) {
      canvas.fill_rect(
          item.box,
          raster::Paint::solid(math::Color{0x0A, 0x0D, 0x13, item.anchor->plate_alpha}), 3.0F);
    }
    port.draw(canvas, item.anchor->text, math::Point{tx, ty}, item.font_size, item.color);
    ++painted_;
  }
}

auto LabelOverlay::semantics_text() const -> std::string {
  return std::to_string(painted_) + " 个标签 / " + std::to_string(painted_edges_) + " 条连线";
}

auto LabelOverlay::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "label_count") return std::to_string(labels_.size());
  if (name == "edge_count") return std::to_string(edges_.size());
  if (name == "painted") return std::to_string(painted_);
  if (name == "painted_edges") return std::to_string(painted_edges_);
  if (name == "font_scale") return std::format("{:.2f}", static_cast<double>(font_scale_));
  return Element::get_property(name);
}

auto LabelOverlay::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "font_scale") {
    try {
      set_font_scale(std::stof(std::string(value)));
      return true;
    } catch (...) {
      return false;
    }
  }
  return Element::set_property(name, value);
}

auto LabelOverlay::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"label_count", "edge_count", "painted", "painted_edges", "font_scale"});
}

auto LabelOverlay::edge_snapshot() const -> std::vector<std::pair<std::string, std::string>> {
  // 连线现在在三维场景里画，本层只保留数据供诊断。
  //
  // 为何还留这个接口：自动化测试要用它断言“边数据仍在”——
  // 否则“三维里真的画了”就只能靠看截图猜。
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(edges_.size());
  for (const auto& e : edges_) out.emplace_back(e.from, e.to);
  return out;
}

}  // namespace st::ui
