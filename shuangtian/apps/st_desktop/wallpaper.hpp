#pragma once

/// `Wallpaper`：把一张位图按 **cover** 语义铺满自己的 bounds（等比放大 + 居中裁切）。
///
/// ## 为什么需要它（2026-11-15）
///
/// 桌面原来用"径向渐变 + 底部一条离散色阶"当背景。色阶的意图是**给玻璃提供可透
/// 的结构**（背板模糊必须有东西可糊，纯平渐变下玻璃"实现正确但肉眼不可见"），
/// 但做法错了：几个平涂矩形叠出来的硬边横条**读起来就是渲染 bug**——用户第一反应
/// 是"这画错了"，而不是"这是背景"。
///
/// 换成一张真实照片/纹理后两件事同时成立：① 背景本身是"图"，看起来就是桌面壁纸；
/// ② 它的细颗粒在模糊后留下**不规则**的层次，玻璃依然可辨。
///
/// ## cover 而不是 stretch
///
/// 壁纸素材通常是 16:9，而窗口是任意比例（用户可能把桌面切成方形窗口调试）。
/// 直接拉伸会把纹理糊成方向性条纹（横向被拉长），一眼看出是拉伸。
/// cover = 等比放大到"至少铺满"，多出来的部分居中裁掉。
///
/// 实现上不裁源图：**把目标矩形算成比窗口大、再用裁剪栈裁掉溢出**——
/// `draw_canvas` 内部按 `clip_rect()` 求交，于是裁切是免费的。
///
/// ## 缺图时
///
/// 没有可用的位图就退回纯色底（由 `Style::background` 给）。桌面不该因为
/// 壁纸素材缺失就白屏——那是最容易被当成"应用起不来"的降级。
///
/// ## 为什么不走 `Style` 的背景图字段
///
/// 框架的 `Style` 只有纯色/渐变（`set_gradient_background`），没有位图背景。
/// 这一轮只需要"桌面背后的那一张图"，为它在框架里加一个位图背景能力、
/// 连带污染属性面与 XML 映射，不划算。等第二个使用者出现时再提升到框架。
/// 自绘元素这条路框架本来就是公开的（`Element::paint_content`）。

#include <memory>
#include <string>

#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/surface.hpp"
#include "st/ui/element.hpp"

namespace stdd {

/// 已解码的位图（不可变，可共享）。
struct ImageBitmap {
  st::raster::Canvas canvas{1, 1, 1.0f};
  bool valid{false};

  /// 从 PNG 字节解码；失败返回 `valid = false`（不抛、不阻断启动）。
  [[nodiscard]] static auto from_png(std::span<const std::uint8_t> bytes)
      -> std::shared_ptr<ImageBitmap>;
  /// 同上，接 `b::embed<...>()` 的 `char` 字节视图。
  [[nodiscard]] static auto from_png(std::span<const char> bytes)
      -> std::shared_ptr<ImageBitmap>;
  /// 从 PNG 文件解码。
  [[nodiscard]] static auto from_png_file(const std::string& path)
      -> std::shared_ptr<ImageBitmap>;
};

/// 壁纸元素：cover 铺满自己的 bounds。
class Wallpaper : public st::ui::Element {
 public:
  explicit Wallpaper(std::shared_ptr<const ImageBitmap> bitmap);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Wallpaper"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::Image; }

  void set_bitmap(std::shared_ptr<const ImageBitmap> bitmap);
  /// 位图缺失时的可见提示（要不要画文字说明）。默认关：桌面上不该出现调试文案。
  void set_missing_hint_visible(bool visible);

  void paint_content(const st::ui::RenderContext& context,
                     st::raster::Surface& canvas) const override;
  /// 桌面是**装饰层**：点击应落到它上面的图标/Dock，而不是被它吃掉。
  [[nodiscard]] auto intercepts_input() const noexcept -> bool override { return false; }

 private:
  std::shared_ptr<const ImageBitmap> bitmap_{};
  bool missing_hint_{false};
};

}  // namespace stdd
