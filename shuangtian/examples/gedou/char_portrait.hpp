#pragma once

/// 格斗示例 · **AI 生成的角色立绘**（SDXL 出图 → 抠图 → 内嵌）。
///
/// ## 素材从哪来
///
/// 用本机下载的 SDXL 动漫模型（`PetalRain 亚洲动漫 Illustrious V2`）生成：
/// `tools/sdxl_char.py --sheet` 出白底全身立绘，
/// `tools/cutout_char.py` 用**泛洪法**抠成透明（从图像边界出发，
/// 只挖与边框连通的白色——这样皮肤、白鞋、白条纹都保得住），
/// 再缩到 640 高存进 `assets/chars/`。
///
/// ## 为什么立绘要"分层"用不起来
///
/// 一张立绘是**整体图**，没法像 2D 矢量那样逐部位画。所以规则是：
///
/// | 状态 | 画什么 |
/// |---|---|
/// | 待机 / 走 / 防御 / 受击 | **立绘**（整体贴图，按骨架矩形摆位） |
/// | 出招（六个技能） | **2D 矢量**（`fighter_art`，肢体跟骨架动） |
///
/// 出招时切矢量的理由：立绘是固定姿势，出拳时必须让手真的伸出去，
/// 否则"看着没打中、判定却中了"——这类视觉/逻辑不同步比画风不统一更糟。
///
/// ## 摆位依据
///
/// 立绘按**骨架的包围盒**缩放摆位（脚底对齐 `pose` 的最低点、身高对齐髋到头）。
/// 这样即使立绘画得比矢量版高一点，也不会出现"贴图浮在地面上方"。
#include <string_view>
#include <vector>

#include "pose.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/surface.hpp"

namespace gedou {

/// 一张已解码的立绘（预乘 RGBA）。
class CharacterPortrait {
 public:
  /// 从内嵌字节解码（`b::embed<"...">()` 的结果）。
  ///
  /// 失败返回空对象（`valid() == false`）——调用方应退回矢量绘制，
  /// 而不是崩溃：缺素材不该让游戏不能玩。
  [[nodiscard]] static auto load(std::vector<unsigned char> bytes) -> CharacterPortrait;
  /// 从文件解码（开发期热更新用）。
  [[nodiscard]] static auto load_file(std::string_view path) -> CharacterPortrait;

  [[nodiscard]] auto valid() const noexcept -> bool { return surface_.get() != nullptr; }
  [[nodiscard]] auto width() const noexcept -> int;
  [[nodiscard]] auto height() const noexcept -> int;

  /// 按姿态画到画布上（脚底对齐、身高按骨架缩放）。
  ///
  /// `bottom_y` 是**这一帧角色脚底的世界 y**（跳跃时会小于地面线）。
  /// `height` 是目标绘制高度（世界像素）。
  void draw(st::raster::Surface& canvas, float center_x, float bottom_y, float height,
            bool facing_right) const;

 private:
  std::unique_ptr<st::raster::Surface> surface_{};
  /// 朝左用的镜像副本（`DrawOptions` 没有水平翻转，所以预先翻一份）。
  /// 代价是一张额外贴图的内存（约 640KB），换来绘制路径不特殊——
  /// 比"每帧逐像素镜像"或"改框架加字段"都便宜。
  std::unique_ptr<st::raster::Surface> mirrored_{};
};

}  // namespace gedou
