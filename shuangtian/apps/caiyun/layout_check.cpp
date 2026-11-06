/// 布局自检的实现（见 `layout_check.hpp` 的说明）。

#include "layout_check.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "layer.hpp"

#include <st/ui/text_port.hpp>

namespace caiyun {

namespace {

/// 一个图层在画布上的可见矩形（像素）。
struct Box {
  float x{0.0f};
  float y{0.0f};
  float w{0.0f};
  float h{0.0f};

  [[nodiscard]] auto right() const -> float { return x + w; }
  [[nodiscard]] auto bottom() const -> float { return y + h; }
  [[nodiscard]] auto empty() const -> bool { return w <= 0.0f || h <= 0.0f; }
};

/// 两个矩形是否相交（含边接触算**不算**相交——紧贴是合法排版）。
[[nodiscard]] auto overlaps(const Box& a, const Box& b) -> bool {
  if (a.empty() || b.empty()) return false;
  return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

/// 相交面积占较小者的比例：用来区分"轻微压边"与"基本盖住"。
[[nodiscard]] auto overlap_ratio(const Box& a, const Box& b) -> float {
  const float ix = std::min(a.right(), b.right()) - std::max(a.x, b.x);
  const float iy = std::min(a.bottom(), b.bottom()) - std::max(a.y, b.y);
  if (ix <= 0.0f || iy <= 0.0f) return 0.0f;
  const float inter = ix * iy;
  const float smaller = std::min(a.w * a.h, b.w * b.h);
  if (smaller <= 0.0f) return 0.0f;
  return inter / smaller;
}

/// 图层名（匿名时给个可定位的占位）。
[[nodiscard]] auto label_of(const Layer& layer) -> std::string {
  return layer.name.empty() ? std::string("(未命名)") : layer.name;
}

}  // namespace

auto LayoutReport::error_count() const -> int {
  return static_cast<int>(std::count_if(issues.begin(), issues.end(),
                                        [](const LayoutIssue& i) {
                                          return i.level == LayoutIssueLevel::Error;
                                        }));
}

auto LayoutReport::warning_count() const -> int {
  return static_cast<int>(std::count_if(issues.begin(), issues.end(),
                                        [](const LayoutIssue& i) {
                                          return i.level == LayoutIssueLevel::Warning;
                                        }));
}

auto check_layout(const Script& script, const st::ui::TextPort* text_port) -> LayoutReport {
  LayoutReport report{};

  const float canvas_w = static_cast<float>(script.width);
  const float canvas_h = static_cast<float>(script.height);
  if (canvas_w <= 0.0f || canvas_h <= 0.0f) return report;

  for (std::size_t scene_index = 0; scene_index < script.scenes.size(); ++scene_index) {
    const Scene& scene = script.scenes[scene_index];

    // 本场景内所有**文字层**的框，用于两两查重叠。
    struct Placed {
      Box box{};
      std::string label{};
      std::size_t layer_index{0};
    };
    std::vector<Placed> text_boxes{};

    for (std::size_t layer_index = 0; layer_index < scene.layers.size(); ++layer_index) {
      const Layer& layer = scene.layers[layer_index];
      const std::string path = std::format("scenes[{}].layers[{}]", scene_index, layer_index);
      const std::string label = label_of(layer);

      // 坐标解析与渲染期同一套规则（`%` / `px` / 设计像素），
      // 否则"检查通过的布局，渲染出来却是另一个样子"——那比不检查更糟。
      const auto parse = [](const std::string& raw, float canvas, float design) -> float {
        if (raw.empty()) return 0.0f;
        return parse_dim_public(raw, canvas, design).value_or(0.0f);
      };
      Box box{};
      box.x = parse(layer.x, canvas_w, canvas_w);
      box.y = parse(layer.y, canvas_h, canvas_h);
      box.w = parse(layer.w, canvas_w, canvas_w);
      box.h = parse(layer.h, canvas_h, canvas_h);

      // ## 1. 出界
      //
      // 判定阈值取"超出 1 像素以上"：浮点换算常有零点几像素的误差，
      // 拿 `>` 直接比会把完全贴边的正确布局报成错。
      if (!box.empty()) {
        constexpr float kSlack = 1.0f;
        if (box.x < -kSlack || box.y < -kSlack || box.right() > canvas_w + kSlack ||
            box.bottom() > canvas_h + kSlack) {
          report.issues.push_back(LayoutIssue{
              .level = LayoutIssueLevel::Error,
              .path = path,
              .layer = label,
              .message = std::format(
                  "图层超出画布：可见区域 ({:.0f},{:.0f}) 到 ({:.0f},{:.0f})，"
                  "画布只有 {}×{}——超出部分不会被看到",
                  box.x, box.y, box.right(), box.bottom(), script.width, script.height)});
        }
      }

      // ## 2. 文字溢出图层框
      //
      // 只对**文字**有意义：矩形/图片的 w/h 就是它自己的形状，不存在"溢出"。
      // 需要 `text_port` 才能量真实宽度；拿不到就**跳过**（而不是当成 0——
      // 那样溢出检查会永远通过，比不检查更危险）。
      if (layer.kind == LayerKind::Text && !layer.text.empty() && text_port != nullptr &&
          box.w > 0.0f) {
        const auto measured = text_port->measure(layer.text, layer.size);
        const float needed = static_cast<float>(measured.width);
        // 容忍 2%：字距/抗锯齿的舍入不该被报成错。
        if (needed > box.w * 1.02f) {
          report.issues.push_back(LayoutIssue{
              .level = LayoutIssueLevel::Warning,
              .path = path,
              .layer = label,
              .message = std::format(
                  "文字比图层框宽：实测 {:.0f}px，框宽 {:.0f}px（\"{}\"）"
                  "——超出部分会被裁掉或压到旁边的内容上",
                  needed, box.w,
                  layer.text.size() > 18 ? layer.text.substr(0, 18) + "…" : layer.text)});
        }
      }

      // 收集文字层用于重叠检查（模型/矩形叠字是正常设计，不查）
      if (layer.kind == LayerKind::Text && !layer.text.empty()) {
        text_boxes.push_back(Placed{.box = box, .label = label, .layer_index = layer_index});
      }
    }

    // ## 3. 文字层互相重叠
    //
    // 只查**文字与文字**：文字压在模型上、压在色块上是常见排版手法；
    // 但两段文字相交，几乎总是坐标算错（实测踩过：副标题压在页码条上）。
    //
    // 用"相交面积 / 较小者"而不是布尔相交：整段压住（>50%）报 Error，
    // 只是压边（>8%）报 Warning。这样紧贴相邻的排布不会误报。
    for (std::size_t i = 0; i < text_boxes.size(); ++i) {
      for (std::size_t k = i + 1; k < text_boxes.size(); ++k) {
        const Placed& a = text_boxes[i];
        const Placed& b = text_boxes[k];
        if (!overlaps(a.box, b.box)) continue;
        const float ratio = overlap_ratio(a.box, b.box);
        if (ratio < 0.08f) continue;

        report.issues.push_back(LayoutIssue{
            .level = ratio > 0.5f ? LayoutIssueLevel::Error : LayoutIssueLevel::Warning,
            .path = std::format("scenes[{}].layers[{}]", scene_index, b.layer_index),
            .layer = b.label,
            .message = std::format(
                "两段文字互相遮挡（重叠 {:.0f}%）：\"{}\" 与 \"{}\"——"
                "多半是 y 坐标靠得太近，挪开其中一个",
                ratio * 100.0f, a.label, b.label)});
      }
    }

    report.checked_layers += static_cast<int>(scene.layers.size());
  }

  return report;
}

}  // namespace caiyun
