#pragma once

/// GPU 字形图集的**纹理内容管理**（平台无关；只管“像素怎么摆进大图”）。
///
/// ## 与 `ShelfPacker` 的分工
///
/// · `ShelfPacker`（`glyph_atlas.hpp`）：**位置**分配——某个 `w×h` 该放哪。
/// · 本类：**内容**管理——把覆盖率写进大图缓冲区、按 `cache_key` 命中、扩容时重排。
///
/// 拆开是因为两件事的可测性不同：位置分配纯几何（好测），
/// 内容管理涉及“命中/未命中/扩容后旧条目失效”，需要单独钉。
///
/// ## 为什么需要“扩容后重排”
///
/// 扩容 = 换一张更大的画布。此时旧条目在新画布上的位置可能全部作废
/// （简单实现是“清空重来”，但那样一扩容就丢掉所有已缓存的字形，
/// 下一帧又要重新上传几百个纹理 → 卡一下）。
///
/// 本类选择**保留像素并重新装箱**：扩容时把旧内容逐个搬进新画布。
/// 代价是一次性内存拷贝（几 MB 量级），换来扩容后**所有条目仍然命中**。
///
/// ## 格式
///
/// 统一用 **RGBA8**（每像素 4 字节）：
///   · 灰度模式：R=G=B=覆盖率，A=255（着色器只读 .r）
///   · 亚像素模式：R/G/B=逐通道覆盖率，A=255
/// 两种模式因此可以共用同一套搬运/上传代码，只是写入内容不同。
/// 代价是灰度模式下 4 倍内存——图集通常 ≤ 2048²，即 ≤ 16 MB，可接受。

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "st/raster/glyph_atlas.hpp"

namespace st::raster::atlas {

/// 一次图集内查询的结果：像素位置 + 尺寸。
struct Placement {
  Region region;
  /// 本次查询是否**新建**了条目（true = 调用方需要上传整张图集）。
  bool fresh{false};
};

/// 图集内容 + 位置分配器。
class GlyphAtlas {
 public:
  /// `initial_dimension` 是初始画布边长（正方形起步，扩容按 `next_capacity` 走）。
  /// `max_dimension` 上限：超过后不再扩容（新字形会**直接失败**，
  /// 调用方退回逐字形纹理路径——那总比无限吃内存好）。
  explicit GlyphAtlas(int initial_dimension = 512, int max_dimension = 2048);

  /// 查询/写入一个 `w×h` 的覆盖率块。
  ///
  /// `write` 回调接收“该块的 RGBA8 目标地址与其行跨距”，由调用方按格式填像素。
  /// 只有**新建**条目时才会调用它（命中时完全不碰像素）。
  ///
  /// 返回 `nullopt` 表示图集已满（装不下，且扩容到上限）。
  [[nodiscard]] auto place(std::uint64_t key, int w, int h,
                           const std::function<void(std::uint8_t* dst, int row_bytes)>& write)
      -> std::optional<Placement>;

  /// 图集的像素缓冲（RGBA8，行优先，宽度 = `width()`）。
  [[nodiscard]] auto pixels() const noexcept -> const std::vector<std::uint8_t>& { return pixels_; }

  [[nodiscard]] auto width() const noexcept -> int { return packer_.canvas_width(); }
  [[nodiscard]] auto height() const noexcept -> int { return packer_.canvas_height(); }
  /// 画布尺寸是否在上次查询后变大过（调用方据此重建纹理）。
  [[nodiscard]] auto take_resized() noexcept -> bool {
    const bool value = resized_;
    resized_ = false;
    return value;
  }
  [[nodiscard]] auto entry_count() const noexcept -> std::size_t { return entries_.size(); }
  [[nodiscard]] auto generation() const noexcept -> std::uint64_t { return generation_; }

  /// 清空（帧切换/上下文丢失时用）。
  void reset();

 private:
  /// 尝试扩容并重排。返回 false = 已到上限。
  [[nodiscard]] auto grow() -> bool;

  ShelfPacker packer_;
  std::vector<std::uint8_t> pixels_;
  std::unordered_map<std::uint64_t, Region> entries_;
  int max_dimension_{2048};
  bool resized_{false};
  /// 每次整图重排（扩容）自增，供调用方判断要不要重建 GPU 纹理。
  std::uint64_t generation_{0};
};

}  // namespace st::raster::atlas
