/// @file label_overlay.cpp
/// @brief 标签叠层实现（投影 + 文字绘制 + 简单避让）。

#include "st/ui/components/label_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// 投影一个世界坐标点。
///
/// 公式与 `ThreeView` 的轨道相机一致（球坐标 → 视图 → 透视除法）。
/// 为什么不复用它的投影：它内部把相机状态封装在 `OrbitCameraState` 里、
/// 且只在绘制路径上算；而标签要在**另一层**用同一套参数算。
/// 与其暴露半个内部结构，不如把公式写成这里的纯函数（可单测）。
struct ScreenPoint {
  float x{0.0f};
  float y{0.0f};
  float depth{0.0f};  ///< 越大越远
  bool visible{false};
};

auto project_point(const LabelOverlay::Projection& p, float wx, float wy, float wz, float width,
                   float height) -> ScreenPoint {
  // 全用 float：本仓开了 -Werror=double-promotion，float 与 double 混算即编译失败。
  // 精度足够：坐标量级 ~10²，float 有效位 ~7 位，差一个像素都看不出。
  const float cy = std::cos(p.yaw);
  const float sy = std::sin(p.yaw);
  const float cp = std::cos(p.pitch);
  const float sp = std::sin(p.pitch);
  const float eye_x = p.target_x + p.distance * cp * sy;
  const float eye_y = p.target_y + p.distance * sp;
  const float eye_z = p.target_z + p.distance * cp * cy;

  // 视线方向（eye → target）
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
  // 上向量 = right × forward
  const float ux = ry * fz - rz * fy;
  const float uy = rz * fx - rx * fz;
  const float uz = rx * fy - ry * fx;

  // 相机空间
  const float dx = wx - eye_x;
  const float dy = wy - eye_y;
  const float dz = wz - eye_z;
  const float depth = dx * fx + dy * fy + dz * fz;
  if (depth < 1.0F) return {};  // 近平面

  const float sx = dx * rx + dy * ry + dz * rz;
  const float sy_world = dx * ux + dy * uy + dz * uz;

  const float fov = p.fov_degrees * static_cast<float>(kPi) / 180.0F;
  // 焦距按高度算（与 ThreeView 的语义一致：宽高比只改变水平可见范围）
  const float focal = (height * 0.5F) / std::tan(fov * 0.5F);
  ScreenPoint out;
  out.x = width * 0.5F + sx * focal / depth;
  out.y = height * 0.5F - sy_world * focal / depth;
  out.depth = depth;
  out.visible = true;
  return out;
}

}  // namespace

struct LabelOverlay::Impl {};

LabelOverlay::LabelOverlay() : impl_(std::make_unique<Impl>()) {}
LabelOverlay::~LabelOverlay() = default;

void LabelOverlay::set_labels(std::vector<LabelAnchor> labels) {
  labels_ = std::move(labels);
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
  const float width = constraints.available_width < kUnbounded ? constraints.available_width
                                                               : constraints.max_width;
  const float height = constraints.available_height < kUnbounded ? constraints.available_height
                                                                 : constraints.max_height;
  const float safe_w = width > 0.0f && width < kUnbounded ? width : 800.0f;
  const float safe_h = height > 0.0f && height < kUnbounded ? height : 600.0f;
  measured_ = math::Size{safe_w, safe_h};
  (void)context;
}

void LabelOverlay::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  painted_ = 0;
  if (labels_.empty()) return;
  const TextPort& port = context.text != nullptr ? *context.text : NullTextPort::instance();

  const float width = bounds_.width;
  const float height = bounds_.height;
  if (width < 8.0f || height < 8.0f) return;

  struct Placed {
    std::string text;
    ScreenPoint at;
    float font_size;
    math::Color color;
    float text_w;
    float weight;
    bool plate;
    unsigned char plate_alpha;
  };
  std::vector<Placed> placed;
  placed.reserve(labels_.size());

  // 字号：按视口高度定（缩放时文字跟着变），再乘用户缩放。
  //
  // 最小值 12 不是随手取的：知识图谱的字是主角（球只是位置标记），
  // 再小中文的横笔会在软件光栅器里糊成一团。系数也从 0.020 提到 0.026：
  // 实测 0.020 在 1920 高的窗口里只有 13px，密集区读不清。
  const float base_font = std::clamp(height * 0.026f, 12.0f, 30.0f) * font_scale_;

  for (const auto& label : labels_) {
    if (label.text.empty()) continue;
    const auto at = project_point(projection_, label.x, label.y, label.z, width, height);
    if (!at.visible) continue;
    // 屏幕外的不画（裁剪，省掉一半无用计算）
    if (at.x < -200.0f || at.x > width + 200.0f || at.y < -100.0f || at.y > height + 100.0f) {
      continue;
    }
    // 重要的字大一点：枢纽与高置信度的词条更容易被读到
    const float font_size = base_font * (0.88f + 0.34f * std::clamp(label.weight, 0.0f, 1.0f));
    const float text_w = port.measure_width(label.text, font_size);
    placed.push_back(Placed{label.text, at, font_size, math::Color::from_rgba_hex(label.argb), text_w,
                            label.weight, label.plate, label.plate_alpha});
  }
  if (placed.empty()) return;

  // 绘制顺序：远的先画（后画近的会压在远的上面，符合深度直觉）
  std::stable_sort(placed.begin(), placed.end(),
                   [](const Placed& a, const Placed& b) { return a.at.depth > b.at.depth; });

  // 避让：多方向候选 + 选位移最小的空位。
  //
  // 上一版只“向下让”，密集区里连续下移几次就跑到别人头上了
  //（实测截图：“GPU 利用率低”压在“训练任务长期 pending”上）。
  // 改成八个方向轮流试（右/左/上/下 + 四个斜向偏移），取**离节点最近**的那个空位：
  // 标签离自己的节点越近，归属越清楚；实在没空位才接受重叠。
  std::vector<math::Rect> occupied;
  occupied.reserve(placed.size());

  // 候选偏移（以“右侧对齐节点中线”为基准的 dy，以及左右翻转）
  struct Candidate {
    float dx;   // 相对节点的横向偏移（已含文字宽度的处理在外面）
    float dy;   // 相对节点中线的纵向偏移（行高倍数）
    bool left;  // 是否放到节点左侧
  };
  const Candidate kCandidates[] = {
      {0.0f, 0.0f, false},   // 右、居中
      {0.0f, 0.0f, true},    // 左、居中
      {0.0f, -1.1f, false},  // 右、上
      {0.0f, 1.1f, false},   // 右、下
      {0.0f, -1.1f, true},   // 左、上
      {0.0f, 1.1f, true},    // 左、下
      {0.0f, -2.2f, false},  // 右、更上
      {0.0f, 2.2f, false},   // 右、更下
  };

  for (const auto& item : placed) {
    const float line_h = port.line_height(item.font_size);
    const float pad = 4.0f;

    math::Rect best{};
    bool found = false;
    float best_distance = 1e30F;

    for (const auto& candidate : kCandidates) {
      float tx = candidate.left ? (item.at.x - item.text_w - pad) : (item.at.x + pad);
      float ty = item.at.y - line_h * 0.5F + candidate.dy * line_h;
      ty = std::clamp(ty, 2.0F, height - line_h - 2.0F);
      // 越界就不算候选（否则会把标签挤出屏幕）
      if (tx < 2.0F || tx + item.text_w > width - 2.0F) continue;
      const math::Rect box{tx - 2.0F, ty - 1.0F, item.text_w + 4.0F, line_h + 2.0F};
      const bool clash =
          std::any_of(occupied.begin(), occupied.end(), [&box](const math::Rect& other) {
            return box.x < other.right() && other.x < box.right() && box.y < other.bottom() &&
                   other.y < box.bottom();
          });
      if (clash) continue;
      // 位移越小越好（=|dy| × 行高 + 左右翻转换的惩罚）
      const float distance = std::abs(candidate.dy) * line_h + (candidate.left ? 3.0F : 0.0F);
      if (distance < best_distance) {
        best_distance = distance;
        best = box;
        found = true;
      }
    }

    if (!found) {
      // 八个方向都占满了：**不画**而不是叠上去。
      //
      // 重叠的字两段都读不清，不如让密集区只显示先到的那些
      //（它们是按深度排序的，也就先到的是远的）——这比一坨糊字有用。
      continue;
    }

    occupied.push_back(best);
    const float tx = best.x + 2.0F;
    const float ty = best.y + 1.0F;

    // 底板：深色半透明，保证文字在任何背景上都能读。
    //
    // 不做描边而做底板：这个渲染器没有文字描边能力，而底板一次
    // `fill_rect` 就够，且对密集区域的对比度更稳。
    //
    // 但**分类/人/作用域不画底板**（`plate=false`）：它们是“册名”，
    // 往往落在空白处，加底板反而变成一块黑斑（实测截图里“运维”“调度”
    // 就像被消字了）。
    if (item.plate) {
      canvas.fill_rect(best,
                       raster::Paint::solid(math::Color{0x0A, 0x0D, 0x13, item.plate_alpha}), 3.0F);
    }
    port.draw(canvas, item.text, math::Point{tx, ty}, item.font_size, item.color);
    ++painted_;
  }
}

auto LabelOverlay::semantics_text() const -> std::string {
  return std::to_string(painted_) + " 个标签";
}

auto LabelOverlay::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "label_count") return std::to_string(labels_.size());
  if (name == "painted") return std::to_string(painted_);
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
  return {"label_count", "painted", "font_scale"};
}

}  // namespace st::ui
