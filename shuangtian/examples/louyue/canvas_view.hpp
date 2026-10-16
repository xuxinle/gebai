#pragma once

/// 镂月（Lunaris）的**自绘画布**。
///
/// ## 演进：视口职责已回流内置 `Viewport`（方案 A 第二跳）
///
/// 本类曾是「满铺视口 + 自绘画布」一体：几何换算（canvas_rect/to_image/to_view）、
/// 离散档位缩放、棋盘格、视口裁剪全在手写。第二跳后这些全部移交外层
/// `CanvasViewport`（内置 Viewport 的外壳）：**本类只画文档原生尺寸的位图**
/// （320×200 绘在自己的 (0,0,w,h)），缩放/平移/棋盘格/裁剪由 Viewport 的
/// arrange/paint 链实现。离屏合成**一次**再整体贴图的渲染策略刻意保留
/// （逐层直画会在缩放时每层各重采样一次——层数多就糊，且层间插值不一致）。
///
/// 事件先到这里再冒泡到外壳——坐标经 `viewport()->to_content` 换算成文档
/// 坐标（事件携带**窗口绝对坐标**，先减视口 bounds 转局部，再走基类换算）。
/// 仍走 `dsl::custom<T>` 逃生舱（高频自绘不进重组树），见设计文档 §9。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "document.hpp"
#include "st/ui/components/viewport.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace louyue {

/// 工具枚举（左工具栏的每一项）。
enum class Tool : std::uint8_t { Move, Brush, Eraser, Bucket, Eyedropper, Select, Crop };

[[nodiscard]] auto tool_name(Tool tool) -> const char*;
[[nodiscard]] auto tool_icon(Tool tool) -> const char*;

/// 缩放档位（设计文档 §3.3：25/50/100/200/400/800）。
///
/// **离散档**而不是连续缩放：连续缩放会让像素对齐一直在变，100% 时
/// "一个图素对一个像素"这个最重要的档位反而很难停准。档位表随第二跳从
/// canvas_view.cpp 的匿名 namespace 挪到这里——外壳的档位步进/吸附与单测
/// （`tests/ui_canvas_zoom_snap_test.cpp`）共用这一份真值。
inline constexpr float kZoomSteps[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};

/// 就近档的下标（距离并列取靠前：1.5 在 1.0/2.0 正中 → 1.0）。
[[nodiscard]] inline auto nearest_zoom_index(float zoom) -> int {
  int best = 0;
  float best_distance = std::abs(kZoomSteps[0] - zoom);
  for (int index = 1; index < static_cast<int>(std::size(kZoomSteps)); ++index) {
    const float distance = std::abs(kZoomSteps[index] - zoom);
    if (distance < best_distance) {
      best_distance = distance;
      best = index;
    }
  }
  return best;
}

/// 就近档：把任意连续 zoom 收回档位表（吸附的换算核心）。
///
/// 这也是**档位不变量的收口**：协议 `set zoom` 可给任意浮点值，旧实现里
/// 0.3 能混进系统（显示 30%，下一步 zoom_in 却跳到 50%——档位语义静默漂移）。
/// 吸附挂在视口的 `on_view_changed` 后，一切路径的终值恒在档位表上。
[[nodiscard]] inline auto nearest_zoom_step(float zoom) -> float {
  return kZoomSteps[nearest_zoom_index(zoom)];
}

/// 一个**纯色块**（颜色面板的前景色预览）。
///
/// ⚠ 为什么不能用 `Panel` + `style().background`：`apply_theme` 在**每次布局**
/// 都跑（`UiRoot::layout` 会遍历全树下线主题），它会用主题色盖掉调用方写的
/// `background`——于是在 `custom<Panel>` 的 configure 里设色只一帧内有效，
/// 下一帧就变回主题底色（实测：色块看着像“没画”）。
/// 自绘元素的路子才稳：`paint_content` 里直接画，不经主题。
class Swatch final : public st::ui::Element {
 public:
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Swatch"; }
  void set_color(st::math::Color color) {
    color_ = color;
    mark_dirty();
  }
  void apply_theme(const st::ui::Theme& theme) override { border_ = theme.colors().border_strong; }
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override;
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  st::math::Color color_{0, 0, 0, 255};
  st::math::Color border_{};
};

/// 画布内容：**文档原生尺寸**的自绘位图（视口职责已移交外层，见文件头）。
class CanvasView final : public st::ui::Element {
 public:
  CanvasView();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "CanvasView"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::Panel; }

  /// 缩放档位——**真值在外层视口**（缩放已随几何层一起回流）。未接线时
  /// 报 1.0（防御：挂载顺序保证实际看不到）。
  [[nodiscard]] auto zoom() const noexcept -> float;
  /// 外层视口（宿主挂载时接上；坐标换算与缩放的真值源）。
  [[nodiscard]] auto viewport() const noexcept -> st::ui::Viewport* { return viewport_; }
  void set_viewport(st::ui::Viewport* viewport);

  void set_document(Document* document);
  void set_tool(Tool tool);
  [[nodiscard]] auto tool() const noexcept -> Tool { return tool_; }

  void set_foreground(st::math::Color color);
  [[nodiscard]] auto foreground() const noexcept -> st::math::Color { return foreground_; }

  void set_brush_radius(float radius);
  [[nodiscard]] auto brush_radius() const noexcept -> float { return brush_radius_; }

  /// 光标所在的**文档坐标**（画布外返回 false 时的值无意义）——状态栏与吸管用。
  [[nodiscard]] auto cursor_image_pos() const noexcept -> st::math::Point { return cursor_; }
  [[nodiscard]] auto cursor_inside() const noexcept -> bool { return cursor_inside_; }
  /// 光标处的**合成结果**颜色（吸管读的就是它）。
  [[nodiscard]] auto cursor_color() const -> st::math::Color;

  /// 命令式作画（e2e 的 `invoke stroke` 走这里）：
  /// 在文档坐标画一段，入历史栈。返回是否真的改了像素。
  auto stroke(st::math::Point from, st::math::Point to) -> bool;
  /// 命令式取色（e2e 的 `invoke pick <x> <y>`）。
  auto pick_at(st::math::Point point) -> bool;

  /// 内容变化后的回调（宿主用它刷新图层面板/历史面板/状态栏）。
  std::function<void()> on_document_changed{};

  // —— Element 接口 ——
  void apply_theme(const st::ui::Theme& theme) override;
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override;
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override;
  auto on_event(const st::ui::RenderContext& context, st::ui::Event& event) -> bool override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 内容变化后的统一收尾（标记重绘 + 通知宿主）。
  void notify();

  /// 窗口绝对坐标 → 文档坐标（经外层视口；未接线时返回原点）。
  [[nodiscard]] auto image_at(st::math::Point window) const -> st::math::Point;

  st::ui::Viewport* viewport_{nullptr};
  Document* document_{nullptr};
  Tool tool_{Tool::Brush};
  float brush_radius_{6.0f};
  st::math::Color foreground_{32, 34, 44, 255};

  st::math::Point cursor_{};
  bool cursor_inside_{false};
  bool dragging_{false};
  /// 上一笔的落点（文档坐标）——高频 MouseMove 下用它做插值起点。
  st::math::Point last_{};

  st::math::Color border_{};
};

/// 内置 `Viewport` 的外壳：louyue 的视口语义（回流方案 A 第二跳收尾）。
///
/// 与基类的差异全部是 louyue 的产品决策：
/// - **档位缩放**：滚轮/e2e 的 `zoom_in`/`zoom_out` 按 `kZoomSteps` 一档档爬
///   （就近档 ±1，锚 = 光标）。不能沿用基类的乘性 `kZoomStep`（1.25）：它一步
///   到 1.25，吸附（`on_view_changed` 里 `snap_zoom`）又把 1.25 收回 1.0——
///   缩放会原地不动。
/// - **档位吸附**：`snap_zoom` 把任意来源（协议 `set zoom` 连续值、`fit` 的
///   适配值）收回最近档——「档位不变量」的收口（旧隐疾#1 根治）。
/// - **左键平移（Move 工具）**：内容对 Move 工具的左键放行冒泡到这里
///   （`CanvasView::on_event`），外壳把它接成拖拽平移——该工具曾是空操作
///   （旧隐疾#3 闭环）。中键平移是基类自带的。
/// - **首帧 fit**：内容回到文档原生尺寸后，首次落位即 `fit_to_view`
///   （整幅图适配居中——旧「每次布局重居中」改为「一次 fit 后位置保持」，
///   平移/缩放不再被 resize 重置）。
///
/// 棋盘格用基类的（锚内容 + 格子随缩放——CanvasView 的自绘棋盘格已随
/// 几何层一起拆除，满铺时代它被关是因为整个被内容盖住）。
///
/// 方法全部 **inline 在本头**：只依赖基类（st 库）与档位表，单测
/// （`tests/ui_canvas_zoom_snap_test.cpp`）直接链接本头即可——tests 目标
/// 不链 examples 源，落 cpp 会让测试二进制缺符号（实测踩到前的静态推演）。
class CanvasViewport final : public st::ui::Viewport {
 public:
  /// 档位吸附：把当前 zoom 收回 `kZoomSteps` 最近档（已是档位则无操作）。
  /// 宿主把它挂进 `on_view_changed`（先吸附再刷新回显——消费者读到的恒是档位值）。
  void snap_zoom() {
    const float snapped = nearest_zoom_step(zoom());
    if (snapped != zoom()) set_zoom(snapped);
  }

  /// e2e 契约动作（`invoke canvas zoom_in/zoom_out` 经 `CanvasView` 委托到这里；
  /// 协议面不可改名，见 canvas_view.cpp 的委托实现）。锚 = 视口中心（无鼠标位）。
  void zoom_in() { step(center(), +1); }
  void zoom_out() { step(center(), -1); }

  void arrange(const st::ui::RenderContext& context, st::math::Rect rect) override {
    // 首次落位（从零尺寸矩形进来）= 一次 fit：整幅图适配居中。此后**不再**
    // 自动重居中——平移/缩放的位置要能保持（旧满铺实现每次布局都居中，
    // resize 会把用户的平移悄悄重置）。`fit_to_view` 需要 content 已量好：
    // 基类 arrange 会量/排内容，所以先走基类再 fit，fit 只改 zoom/pan，
    // 再走一次基类把新 zoom/pan 落到内容的矩形上。
    const bool first = bounds().is_empty() && !rect.is_empty();
    Viewport::arrange(context, rect);
    if (first) {
      fit_to_view();
      Viewport::arrange(context, rect);
    }
  }

  auto on_event(const st::ui::RenderContext& context, st::ui::Event& event) -> bool override {
    switch (event.kind) {
      case st::ui::EventKind::Wheel: {
        // 档位缩放：锚 = 光标（就近档 ±1）。Ctrl 与否都缩放（旧实现只认
        // Ctrl+滚轮；无 Ctrl 滚轮历史上无人处理——现在也接住，画布不再需要
        // 「留给外层滚动」的语义）。
        if (event.wheel_delta == 0.0f) return false;
        const st::math::Point local{event.position.x - bounds().x,
                                    event.position.y - bounds().y};
        step(local, event.wheel_delta > 0.0f ? +1 : -1);
        event.handled = true;
        return true;
      }
      case st::ui::EventKind::MouseDown: {
        // 中键 = 基类的拖拽平移；左键平移由宿主经 `wants_pan` 判定
        //（Move 工具，CanvasView 放行冒泡——见 `wants_pan` 处的分层说明）。
        if (event.button == kMiddleButton) break;
        if (event.button == 1 && wants_pan != nullptr && wants_pan()) {
          panning_ = true;
          pan_last_ = event.position;
          event.handled = true;
          return true;
        }
        return false;
      }
      case st::ui::EventKind::MouseMove: {
        if (!panning_) break;
        const st::math::Point pan = this->pan();
        set_pan(st::math::Point{
            pan.x - (event.position.x - pan_last_.x) / zoom(),
            pan.y - (event.position.y - pan_last_.y) / zoom()});
        pan_last_ = event.position;
        event.handled = true;
        return true;
      }
      case st::ui::EventKind::MouseUp: {
        if (!panning_) break;
        panning_ = false;
        event.handled = true;
        if (on_view_changed) on_view_changed();
        return true;
      }
      default:
        break;
    }
    return Viewport::on_event(context, event);
  }

  [[nodiscard]] auto type() const noexcept -> std::string_view override {
    return "CanvasViewport";
  }

  /// 左键是否应进入平移模式（宿主注入，如「当前工具 == 移动」）。
  ///
  /// 为什么是一个回调而不是直接 `dynamic_cast<CanvasView*>`：外壳只应认识
  /// **视口语义**，「哪种工具想拖画布」是宿主/内容层的知识。回调注入解耦
  /// 两层；它同时解决一个链接细节——**本类所有虚函数均 inline 在头**
  ///（无 key function，vtable/typeinfo 按需弱发射到每个使用它的 TU），
  /// 若事件路径里 dynamic_cast 到内容类型，会引用内容的 typeinfo（锚在
  /// 内容的 key function TU，即 examples 目标）——tests 目标链接即炸
  /// `undefined typeinfo`（实测踩到 vtable 同型问题的姊妹形态）。
  std::function<bool()> wants_pan{};

  /// 中键的 `Event::button` 取值（与 `platform_win32` 的映射、内置 Viewport 同口）。
  static constexpr int kMiddleButton{3};

 private:
  /// 就近档 ±1 步进（`delta` 为 0 时无操作）；`anchor` 是视口局部坐标。
  /// 取档基准是「当前值的就近档」而不是当前值本身：协议 set 的连续值
  ///（0.3）不会让档位语义漂移（0.3 → 就近档 0.25 → +1 = 0.5）。
  void step(st::math::Point anchor, int delta) {
    const int index = nearest_zoom_index(zoom());
    const int next =
        std::clamp(index + delta, 0, static_cast<int>(std::size(kZoomSteps)) - 1);
    zoom_at(anchor, kZoomSteps[next] / zoom());
  }

  /// 视口中心（局部坐标）——无鼠标位时的缩放锚。
  [[nodiscard]] auto center() const -> st::math::Point {
    return st::math::Point{bounds().width * 0.5f, bounds().height * 0.5f};
  }

  /// Move 工具的左键拖拽平移进行态。
  bool panning_{false};
  st::math::Point pan_last_{};
};

}  // namespace louyue
