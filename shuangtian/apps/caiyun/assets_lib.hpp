#pragma once

/// 裁云（Nimbus）· **素材库**：把外部图片/音频收编为可被 AI 与人共同引用的素材。
///
/// ## 为什么需要它
///
/// 在此之前裁云的画面上只有**几何与文字**——所有"素材"都是代码生成的形状。
/// 这让它做不出真正的宣传片：产品截图、品牌图、生成的背景图都进不来。
/// 素材库是"外部世界的内容"进入作品的唯一通道。
///
/// ## 为什么是"目录 + 索引"而不是数据库
///
/// 素材是**文件**：AI 用 `imagegen` 生成 PNG 落盘、人往目录里拖图片、脚本用相对路径引用。
/// 目录就是唯一真值，索引只是缓存（`refresh()` 重扫）。这样：
/// - AI 生成一张图 → 直接 `refresh()` 就能用，不需要"导入"这个动作；
/// - 人换一张图 → 重扫即见；
/// - 工程拷到别的机器 → 素材跟着目录走。
///
/// ## 路径语义
///
/// 脚本里的 `image` 字段支持三种写法，按顺序解析：
/// 1. **绝对路径**（`C:/x/a.png`）——直接用；
/// 2. **相对于脚本文件**（`assets/cover.png`）——可移植的写法，推荐；
/// 3. **素材库内名称**（`cover`）——按名字在素材库里找（AI 生成时常这么用）。
///
/// 都找不到时**不崩**：返回空 `optional`，渲染侧画一个"素材缺失"占位框
/// （比静默跳过好——静默会让"图没进来"看起来像"设计就是没图"）。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/surface.hpp"

namespace caiyun {

/// 一条素材记录。
struct AssetEntry {
  std::string name{};      ///< 逻辑名（不含扩展名；AI/脚本按它引用）
  std::string path{};      ///< 绝对路径（真值）
  std::string ext{};       ///< 小写扩展名（`png`/`jpg`/`wav`…）
  int width{0};            ///< 图片宽（音频/未知为 0）
  int height{0};           ///< 图片高
  std::uint64_t bytes{0};  ///< 文件大小
  /// 图片的主色（缩略图缩到 1×1 得到的均值）——`image` 块缺少强调色时用它，
  /// 让"用了素材"的画面配色自动协调。
  st::math::Color average_color{0, 0, 0, 255};
};

/// 素材库：一个目录 + 索引 + 图片缓存。
///
/// 图片**按需解码并缓存**（`image()`）；同一帧里多次引用同一张图不会重复解码。
/// 缓存以绝对路径为键——换文件（mtime 变化）时 `refresh()` 会让旧项失效。
class AssetLibrary {
 public:
  AssetLibrary() = default;

  /// 设素材目录（不存在的目录会被静默建立；空串 = 清空）。
  void set_root(std::string root);
  [[nodiscard]] auto root() const -> const std::string& { return root_; }

  /// 重扫目录，重建索引。返回素材条数。
  auto refresh() -> std::size_t;

  [[nodiscard]] auto entries() const -> const std::vector<AssetEntry>& { return entries_; }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return entries_.size(); }

  /// 按名字或路径解析成绝对路径。
  ///
  /// `basis` 是**脚本所在目录**（相对路径的基准）；空则相对当前工作目录。
  /// 找不到返回 `nullopt`。
  [[nodiscard]] auto resolve(std::string_view ref, std::string_view basis) const
      -> std::optional<std::string>;

  /// 按名字查一条（精确名、再退化为"带扩展名的全名"）。
  [[nodiscard]] auto find(std::string_view name) const -> const AssetEntry*;

  /// 取解码后的图片（带缓存）。失败返回 `nullopt`（**不抛**：素材缺失是常态，
  /// 渲染侧画占位框而不是让整帧失败）。
  /// 取解码后的图片（带缓存，返回共享指针——`Canvas` 不可拷贝，缓存里存指针即可）。
  [[nodiscard]] auto image(std::string_view path) const
      -> std::shared_ptr<st::raster::Canvas>;

  /// 一段可直接给 AI 看的清单（一行一条：`name  WxH  path`）。
  [[nodiscard]] auto manifest() const -> std::string;

 private:
  std::string root_{};
  std::vector<AssetEntry> entries_{};
  /// 解码缓存：绝对路径 → 图片。`mutable` 因为 `image()` 是 const 语义（读）。
  mutable std::vector<std::pair<std::string, std::shared_ptr<st::raster::Canvas>>> cache_{};
};

/// 把一个矩形按**等比缩放 + 居中裁切**（cover）贴进目标区域。
///
/// 为何是 cover 而不是 stretch：素材长宽比与目标框几乎不会一致，stretch 会把
/// 人脸/产品拉变形（一眼假）。cover 裁掉多余部分，视觉上永远"填满且不变形"。
void blit_cover(st::raster::Surface& target, const st::raster::Canvas& source,
                st::math::Rect area, float alpha, float corner_radius);

/// 把 source 按 contain（完整可见、留边）贴进 area。
void blit_contain(st::raster::Surface& target, const st::raster::Canvas& source,
                  st::math::Rect area, float alpha);

}  // namespace caiyun
