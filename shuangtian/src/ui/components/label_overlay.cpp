/// @file label_overlay.cpp
/// @brief 标签叠层实现（投影 + 避让 + 画线 + 文字）。

#include "st/ui/components/label_overlay.hpp"

#include <algorithm>
#include <cmath>
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

/// 投影一个世界坐标点。
///
/// 公式与 `ThreeView` 的轨道相机一致（球坐标 → 视图 → 透视除法）。
/// 全用 float：本仓开了 `-Werror=double-promotion`，float 与 double 混算即编译失败。
auto project_point(const LabelOverlay::Projection& p, float wx, float wy, float wz, float width,
                   float height) -> ScreenPoint {
  const float cy = std::cos(p.yaw);
  const float sy = std::sin(p.yaw);
  const float cp = std::cos(p.pitch);
  const float sp = std::sin(p.pitch);
  const float eye_x = p.target_x + p.distance * cp * sy;
  const float eye_y = p.target_y + p.distance * sp;
  const float eye_z = p.target_z + p.distance * cp * cy;

  float fx = p.target_x - eye_x;
  float fy = p.target_y - eye_y;
  float fz = p.target_z - eye_z;
  const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
  if (flen < 1e-6F) return {};
  fx /= flen;
  fy /= flen;
  fz /= flen;

  // 右向量 = forward × up（up = +Y）→ (fz, 0, -fx)
  float rx = fz;
  float ry = 0.0F;
  float rz = -fx;
  const float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
  if (rlen < 1e-6F) {
    // 正俯视：任取一个水平方向（否则标签会全丢，比略偏一点更糟）
    rx = 1.0F;
    ry = 0.0F;
    rz = 0.0F;
  } else {
    rx /= rlen;
    ry /= rlen;
    rz /= rlen;
  }
  const float ux = ry * fz - rz * fy;
  const float uy = rz * fx - rx * fz;
  const float uz = rx * fy - ry * fx;

  const float dx = wx - eye_x;
  const float dy = wy - eye_y;
  const float dz = wz - eye_z;
  const float depth = dx * fx + dy * fy + dz * fz;
  if (depth < 1.0F) return {};  // 近平面

  const float sx = dx * rx + dy * ry + dz * rz;
  const float sy_world = dx * ux + dy * uy + dz * uz;

  const float fov = p.fov_degrees * kPi / 180.0F;
  // 焦距按高度算（与 ThreeView 的语义一致：宽高比只改变水平可见范围）
  const float focal = (height * 0.5F) / std::tan(fov * 0.5F);
  ScreenPoint out;
  out.x = width * 0.5F + sx * focal / depth;
  out.y = height * 0.5F - sy_world * focal / depth;
  out.depth = depth;
  out.visible = true;
  return out;
}

/// 画一条抗锯齿的细线。
///
/// 为什么自己画：本仓 `Surface` 只有矩形与覆盖率混合，没有线段原语。
/// 而线条质量是这个视图的观感核心——第一版用三维圆柱画线，
/// 不到 1 像素宽时是断续脏点，加粗后又与文字对不上位置。
///
/// 抗锯齿做法：逐行算**每个像素到线段的距离**→ 覆盖率 → `blend_coverage_row`。
/// 用现成的覆盖率混合而不是直接写像素：它走框架的混合管线（色彩空间、
/// 不透明度都一致），而且不依赖后端是否开了 alpha。
void draw_thin_line(raster::Surface& canvas, math::Point a, math::Point b, math::Color color,
                    float width, bool dashed = false) {
  const float half = std::max(0.5F, width * 0.5F);
  const float pad = std::ceil(half) + 1.0F;

  const int x0 = std::max(0, static_cast<int>(std::floor(std::min(a.x, b.x) - pad)));
  const int x1 =
      std::min(canvas.width() - 1, static_cast<int>(std::ceil(std::max(a.x, b.x) + pad)));
  const int y0 = std::max(0, static_cast<int>(std::floor(std::min(a.y, b.y) - pad)));
  const int y1 =
      std::min(canvas.height() - 1, static_cast<int>(std::ceil(std::max(a.y, b.y) + pad)));
  if (x1 < x0 || y1 < y0) return;

  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float len_sq = dx * dx + dy * dy;
  if (len_sq < 1e-6F) return;
  const float inv_len_sq = 1.0F / len_sq;

  const int row_len = x1 - x0 + 1;
  std::vector<float> coverage(static_cast<std::size_t>(row_len), 0.0F);
  const raster::Paint paint = raster::Paint::solid(color);

  for (int y = y0; y <= y1; ++y) {
    bool any = false;
    const float py = static_cast<float>(y) + 0.5F;
    for (int i = 0; i < row_len; ++i) {
      const float px = static_cast<float>(x0 + i) + 0.5F;
      const float t = std::clamp(((px - a.x) * dx + (py - a.y) * dy) * inv_len_sq, 0.0F, 1.0F);
      const float ddx = px - (a.x + dx * t);
      const float ddy = py - (a.y + dy * t);
      const float dist = std::sqrt(ddx * ddx + ddy * ddy);
      // dist <= half-0.5 全实，>= half+0.5 全空，中间线性过渡
      float c = std::clamp(half + 0.5F - dist, 0.0F, 1.0F);
      // 虚线：沿线段方向的周期（6 实 5 空）。
      //
      // 为何长线要虚线：投影下跨半屏的直线会与附近短线混在一起，
      // 读者分不清哪条连哪对。虚线是约定俗成的“间接/弱关系”视觉语，
      // 不需要额外图例。
      if (dashed) {
        const float along = t * std::sqrt(len_sq);
        if (std::fmod(along, 11.0F) > 6.0F) c = 0.0F;
      }
      coverage[static_cast<std::size_t>(i)] = c;
      if (c > 0.0F) any = true;
    }
    if (!any) continue;
    canvas.blend_coverage_row(y, x0, coverage, paint, 1.0F, raster::BlendMode::SrcOver);
  }
}

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

void LabelOverlay::set_projection(const Projection& projection) {
  projection_ = projection;
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
    const auto at = project_point(projection_, label.x, label.y, label.z, width, height);
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
  // id → 最终矩形（标签位置，供避让计算）
  std::unordered_map<std::string, math::Rect> boxes;
  boxes.reserve(placed.size());
  // id → **投影点**（连线端点用这个，不是矩形）
  std::unordered_map<std::string, ScreenPoint> anchors;
  anchors.reserve(placed.size());

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
    if (!item.anchor->id.empty()) {
      boxes.emplace(item.anchor->id, best);
      anchors.emplace(item.anchor->id, item.at);
    }
  }

  // ── 连线：连**投影点**而不是文字框（理由见头文件的说明）──
  //
  // ## 长线不隐藏、而是**变淡**（而不是一刀切不画）
  //
  // 先说试过的两种极端：
  // - 全画：投影下跨半屏的线穿过画面，看不出端点是哪个字，还与附近短线混淆
  //   （截图里顶部那几条）。
  // - 全不画：会丢掉**真实存在**的关系——而“谁连着谁”正是图谱要表达的。
  //
  // 取中间：短线满强度，长线按长度线性减淡。
  //
  // ## 阀值为何是 55/150（分簇后的实测调法）
  //
  // 分簇后簇内距离典型 40~100px、跨簇 200~800px。
  // 目标：只让**真正的近邻**发亮，簇内稍远的与跨簇的都淡下去。
  // 55px 开始变淡、150px 到底——于是画面上的线不再是“跨屏长条”，
  // 而是几个贴着文字的小三角与短线，不抢文字的戏。
  constexpr float kMinEdgeLength = 12.0F;
  constexpr float kFadeStart = 55.0F;
  constexpr float kFadeEnd = 150.0F;
  constexpr float kMinAlpha = 0.10F;
  for (const auto& edge : edges_) {
    const auto it_from = anchors.find(edge.from);
    const auto it_to = anchors.find(edge.to);
    if (it_from == anchors.end() || it_to == anchors.end()) continue;

    const ScreenPoint& a = it_from->second;
    const ScreenPoint& b = it_to->second;
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < kMinEdgeLength) continue;  // 太短的线看起来就是脏点

    // 端点从圆点边缘起算（不然线会穿进圆点中心，细看很脏）
    const float inset = 4.0F;
    const float ux = dx / len;
    const float uy = dy / len;
    const math::Point pa{a.x + ux * inset, a.y + uy * inset};
    const math::Point pb{b.x - ux * inset, b.y - uy * inset};

    const float fade =
        std::clamp(1.0F - (len - kFadeStart) / (kFadeEnd - kFadeStart), kMinAlpha, 1.0F);
    math::Color color = math::Color::from_rgba_hex(edge.argb);
    color.a = static_cast<std::uint8_t>(static_cast<float>(color.a) * fade);

    // 长线用虚线：让“跳得远的关系”在形态上就与近邻关系区分开，
    // 而不是只靠颜色深浅（深色背景上深浅差不敏感）。
    const bool dashed = len > kFadeStart;
    draw_thin_line(canvas, pa, pb, color, edge.width * (0.75F + 0.25F * fade), dashed);
    ++painted_edges_;
  }

  // ── 圆点：投影点的可见锚（画在线之上，让“这里是节点”一目了然）──
  for (const auto& item : placed) {
    if (item.dropped) continue;
    const float r = item.anchor->weight > 0.6F ? 4.5F : 3.2F;
    const math::Color dot_color = math::Color::from_rgba_hex(item.anchor->argb);
    canvas.fill_rect(math::Rect{item.at.x - r, item.at.y - r, r * 2.0F, r * 2.0F},
                     raster::Paint::solid(dot_color), r);
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
  return std::nullopt;
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
  return false;
}

auto LabelOverlay::property_names() const -> std::vector<std::string_view> {
  return {"label_count", "edge_count", "painted", "painted_edges", "font_scale"};
}

}  // namespace st::ui
