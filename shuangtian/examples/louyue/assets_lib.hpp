#pragma once

/// 镂月（Lunaris）· **素材库**：把外部图片收编为海报可用的素材。
///
/// ## 为什么两个应用各有一份
///
/// 看起来该共用一份，但**不共用**是刻意的：裁云要图片 + 音频（配音）、要 cover 贴图
/// 与"素材缺失占位"；镂月只要图片、要 contain/圆角/蒙版这类排版语义。硬塞进一个
/// 带 `#ifdef` 的公共库，两边都会长出别人不需要的字段——而这两个示例的职责就是
/// 各自把本领域的能力说清楚。
///
/// 形状相同（`set_root` / `refresh` / `find` / `resolve` / `image` / `manifest`），
/// 所以调用方的心智负担没有增加。
///
/// ## 路径解析
///
/// 海报脚本里的 `image` 字段按顺序解析：绝对路径 → 相对脚本目录 → 素材库内名称
/// → 相对当前工作目录。都找不到时**不崩**：渲染侧画"缺图"占位框。
/// 静默跳过比占位框糟——它会让"图没进来"看起来像"设计就是没图"。

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

namespace louyue {

/// 一条素材记录。
struct AssetEntry {
  std::string name{};      ///< 逻辑名（不含扩展名；脚本按它引用）
  std::string path{};      ///< 绝对路径（真值）
  int width{0};
  int height{0};
  std::uint64_t bytes{0};
  /// 主色（中心区域均值）——图片块没指定叠色/描边色时用它，画面配色自动协调。
  st::math::Color average_color{0, 0, 0, 255};
};

/// 素材库：目录 + 索引 + 解码缓存。
class AssetLibrary {
 public:
  AssetLibrary() = default;

  void set_root(std::string root);
  [[nodiscard]] auto root() const -> const std::string& { return root_; }

  /// 重扫目录。返回素材条数。
  auto refresh() -> std::size_t;

  [[nodiscard]] auto entries() const -> const std::vector<AssetEntry>& { return entries_; }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return entries_.size(); }

  [[nodiscard]] auto resolve(std::string_view ref, std::string_view basis) const
      -> std::optional<std::string>;
  [[nodiscard]] auto find(std::string_view name) const -> const AssetEntry*;
  /// 取解码后的图片（带缓存，返回共享指针——`Canvas` 不可拷贝，缓存里存指针即可）。
  [[nodiscard]] auto image(std::string_view path) const
      -> std::shared_ptr<st::raster::Canvas>;

  /// 一行一条的清单（给 AI 看）。
  [[nodiscard]] auto manifest() const -> std::string;

 private:
  std::string root_{};
  std::vector<AssetEntry> entries_{};
  mutable std::vector<std::pair<std::string, std::shared_ptr<st::raster::Canvas>>> cache_{};
};

/// 把 source 按 **cover**（等比填满 + 居中裁切）贴进 area，可带圆角与叠色。
///
/// `tint` 会以 `tint_alpha` 强度覆盖在图上（做品牌色调统一 / 压暗背景让文字可读）。
void blit_cover(st::raster::Canvas& target, const st::raster::Canvas& source,
                st::math::Rect area, float alpha, float corner_radius,
                st::math::Color tint = {}, float tint_alpha = 0.0f);

/// 把 source 按 **contain**（完整可见 + 留边）贴进 area。
void blit_contain(st::raster::Canvas& target, const st::raster::Canvas& source,
                  st::math::Rect area, float alpha, float corner_radius = 0.0f);

}  // namespace louyue
