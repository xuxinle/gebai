#pragma once

/// `AppIcon`：把一张 **真实应用图标（PNG）** 画进界面。
///
/// ## 为什么需要一个自绘元素，而不是用 `IconView`
///
/// 框架里能画图形的组件有两条路：`IconView`（内置图标表的路径数据 + SVG 注册表，
/// 都是单色矢量）与位图合成（`Surface::draw_canvas`）。应用图标是**彩色位图**
/// （每家自带一套配色与圆角底），走前者只能得到单色轮廓——桌面上十几个图标会
/// 彼此难以区分，而"用真实的应用图标"正是桌面壳的基本要求。
///
/// 所以这里做最小的一件事：持有解码后的 `Canvas`，在 `paint_content` 里按自己的
/// bounds 缩放贴上去。
///
/// ## 圆角是画出来的，不是改图的
///
/// 图标源图是**方形** PNG（各家在图里自带底色）。桌面图标按 macOS 语义有圆角与
/// 一点投影，所以用 `push_clip_rounded_rect` 裁圆角——**不要**去改源图：那份 PNG
/// 同时是窗口/任务栏图标（系统按方形使用它）。
///
/// ## 缺失时的姿态
///
/// 源图解码失败（坏文件/不认识的格式）→ 画一个中性圆角方块 + 首字母占位。
/// **绝不静默画空白**：空白图标会让用户以为"这个应用没装"，而实际只是图标坏了。

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/surface.hpp"
#include "st/ui/element.hpp"

namespace stdd {

/// 一张已解码的图标位图（不可变；多个 `AppIcon` 可共享同一份）。
struct IconBitmap {
  st::raster::Canvas canvas{1, 1, 1.0f};
  /// 空 = 没有源图（退回占位绘制）。
  bool valid{false};

  /// 从 PNG 字节解码；失败返回 `valid = false` 的空位图（不抛、不阻断启动）。
  [[nodiscard]] static auto from_png(std::span<const std::uint8_t> bytes)
      -> std::shared_ptr<IconBitmap>;
  /// 同上，但接的是 `b::embed<...>()` 那种 `char` 字节视图。
  ///
  /// 为何单开一个重载而不是让调用方 reinterpret_cast：嵌入字节本来就是**字节**，
  /// 只是签名上的元素类型叫 `char`——转成 `uint8_t` 是语义标注而不是位重解释
  /// （与 `st/app/app_icon.hpp` 同一姿态，那里也是经中间容器中转）。
  [[nodiscard]] static auto from_png(std::span<const char> bytes)
      -> std::shared_ptr<IconBitmap>;
  /// 从 PNG 文件解码（`desktop.json` 里用 `icon_path` 指外部图时走这条）。
  [[nodiscard]] static auto from_png_file(const std::string& path)
      -> std::shared_ptr<IconBitmap>;
};

/// 画一张应用图标：圆角裁切 + 图（缺图时首字母占位）。
class AppIcon : public st::ui::Element {
 public:
  AppIcon(std::shared_ptr<const IconBitmap> bitmap, std::string fallback_glyph,
          float size = 64.0f);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "AppIcon"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::Image; }

  void set_size(float size);
  [[nodiscard]] auto size() const noexcept -> float { return size_; }
  /// 不可用时整块变灰（与"可点/不可点"的语义对应）。
  void set_dimmed(bool dimmed);
  /// 圆角占边长的比例（macOS 约 0.225）。
  void set_rounding(float ratio);

  void measure(const st::ui::RenderContext& context,
               const st::ui::Constraints& constraints) override;
  void paint_content(const st::ui::RenderContext& context,
                     st::raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return glyph_; }

 private:
  std::shared_ptr<const IconBitmap> bitmap_{};
  std::string glyph_{};
  float size_{64.0f};
  float rounding_{0.225f};
  bool dimmed_{false};
};

}  // namespace stdd
