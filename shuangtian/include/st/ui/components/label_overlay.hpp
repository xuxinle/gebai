#pragma once

/// @file label_overlay.hpp
/// @brief 把三维场景里的物体**投影到屏幕**并画出文字标签。
///
/// ## 为什么要这个组件（三维里为什么没有文字）
///
/// three.js 跑在 QuickJS 里，而脚本宿主**没有 Canvas 2D**——
/// 常规的「Canvas 画字 → 转纹理 → Sprite」这条路根本走不通。
/// 用几何体拼汉字更不现实。
///
/// 所以分工变成：
/// - **三维层**（`ThreeView`）：画结构（球 / 连线 / 光照），它有的能力没法画字；
/// - **本组件**：按 `ThreeView` 给出的**屏幕投影坐标**，用 `TextPort` 画文字。
///
/// 两层叠在同一块区域上——这正是"文字永远面向观察者、不会随视角倾斜"的做法，
/// 也是三维软件里标签的常见实现（billboard 的二维等价物）。
///
/// ## 为什么标签是主角
///
/// 知识图谱的用途是"看清知识之间的关系"。一堆彩色球只能看出"有几个簇"，
/// 看不出"这条是什么"。所以标签不该是装饰：**词条文字本身就是节点**，
/// 球只是它的位置标记。

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "st/ui/element.hpp"

namespace st::ui {

/// 一个待绘制的标签（世界坐标 → 由本组件投影）。
struct LabelAnchor {
  /// 世界坐标。
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};
  /// 文字（空 = 不画）。
  std::string text;
  /// 重要度（0~1）：决定字号与是否绘制。枢纽节点给高值。
  float weight{0.35f};
  /// 颜色（ARGB）。
  unsigned int argb{0xFFD8DEE9U};
};

/// 标签叠层：把一组世界坐标标签投影到屏幕并绘制。
class LabelOverlay : public Element {
 public:
  LabelOverlay();
  ~LabelOverlay() override;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "LabelOverlay"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  /// 换标签集（由 bridge 在图谱重建时调用）。
  void set_labels(std::vector<LabelAnchor> labels);

  /// 设定投影参数：与 `ThreeView` 的相机保持一致。
  ///
  /// 为什么不用 ThreeView 内部的投影：那需要它把每帧的相机状态暴露成可复用的
  /// 纯函数，而它暴露的是字符串（`camera_state`）——解析字符串再算一遍投影，
  /// 比在这里重算公式更不可靠。这里用同一套公式（球坐标 → 透视投影），
  /// 参数由调用方一次给全，纯函数、可单测。
  struct Projection {
    float yaw{0.6f};         ///< 与 ThreeView 一致的方位角（弧度）
    float pitch{0.42f};      ///< 俯仰角
    float distance{600.0f};  ///< 相机到目标距离
    float fov_degrees{46.0f};
    float target_x{0.0f};    ///< 观察目标（布局中心）
    float target_y{0.0f};
    float target_z{0.0f};
  };
  void set_projection(const Projection& projection);
  [[nodiscard]] auto projection() const noexcept -> const Projection& { return projection_; }

  /// 字体缩放（提高可读性；缺省按元素宽度自适应）。
  void set_font_scale(float scale);

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;

  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

  /// 诊断：本帧画了几个标签（证明"该画的画了"，而不是靠看截图）。
  [[nodiscard]] auto painted_labels() const noexcept -> std::size_t { return painted_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<LabelAnchor> labels_;
  Projection projection_{};
  float font_scale_{1.0f};
  mutable std::size_t painted_{0};
};

}  // namespace st::ui
