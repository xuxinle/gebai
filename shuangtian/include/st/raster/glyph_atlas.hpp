#pragma once

/// 字形纹理图集的**分配器**（平台无关，不做任何图形调用）。
///
/// ## 为什么需要图集
///
/// GPU 路径上每个字形一次 `submit`，每帧 305 个字形 → **610 次 draw**
/// （LCD 亚像素要两遍混合）。实测 GPU 侧 `text` 占整帧的 **84%**
/// （0.67 / 1.40 ms，`gallery --renderer gpu`）。
///
/// 而 `submit` 的成本几乎全在**状态设置**（渲染目标/视口/scissor/混合状态/
/// 常量缓冲 Map+memcpy/资源与采样器绑定），不在真正的光栅化——所以
/// **把 N 个字形合成一次 draw** 是这里唯一有意义的优化。
///
/// 合成一次 draw 需要：同一次绘制内的所有字形来自**同一张纹理**（否则纹理绑定
/// 无法共用），且每个字形有**自己的矩形 + UV**。前者靠图集，后者靠实例数据。
///
/// ## 这个类的职责
///
/// 只管“把一块 `w×h` 的小图放进大纹理的哪一格”，返回像素坐标。
/// 纯计算、无状态副作用、可单测——图集逻辑最容易错的地方（越界、重叠、
/// 行高计算、扩容后旧条目失效）都在这里被钉住。
///
/// ## 不做的事
///
/// 不做淘汰、不做纹理上传、不管 D3D。淘汰策略由调用方决定
/// （字形集在工作集稳定后很少变化，实测 1 张→96 张摊销只涨 7.7%，
/// 见 `tools/shadow_amortized_probe.cpp`——但那测的是阴影，字形另说）。
///
/// ## 布局算法
///
/// 简单的**货架式（shelf）**装箱：每行从左到右填，填不下就开新行，
/// 行高 = 该行最高的条目。对字形这种“尺寸相近但种类多”的负载足够好，
/// 且实现可一眼看懂——`skyline` 之类更省空间但难以验证。
///
/// 代价：每个条目独占整行高度（不跨行共享垂直空间）。表头 1 像素的
/// 间隔用于避免线性采样的串色（点采样不需要，但留出间隔更稳）。

#include <cstdint>
#include <optional>
#include <vector>

namespace st::raster::atlas {

/// 一块已分配的子矩形（像素坐标，左上角原点）。
struct Region {
  int x{0};
  int y{0};
  int width{0};
  int height{0};

  [[nodiscard]] auto is_empty() const noexcept -> bool { return width <= 0 || height <= 0; }
};

/// 货架式图集分配器。
class ShelfPacker {
 public:
  /// `padding` 是条目之间的间隔（像素）。默认 1：够避免线性采样串色，
  /// 又不会浪费太多空间（字形通常 10~40 px，1 px 间隔约占 3~10%）。
  explicit ShelfPacker(int padding = 1) noexcept : padding_(padding) {}

  /// 试图在 `width × height` 的画布上分配 `w × h`。
  ///
  /// 成功返回区域并**记住它已被占用**；失败返回 `nullopt`（但不改变状态，
  /// 调用方可据此扩容后重试）。要求 `w/h > 0` 且不超过画布尺寸
  /// （超过时永远放不下，返回 `nullopt`）。
  [[nodiscard]] auto allocate(int w, int h) -> std::optional<Region>;

  /// 清空所有占用（扩容换新画布时用）。
  void reset() noexcept;

  /// 当前画布尺寸（由 `set_canvas` 设定；分配不会超它）。
  void set_canvas(int width, int height) noexcept;

  [[nodiscard]] auto canvas_width() const noexcept -> int { return canvas_width_; }
  [[nodiscard]] auto canvas_height() const noexcept -> int { return canvas_height_; }

  /// 已用到的最大高度（判断“要不要扩容”的依据）。
  [[nodiscard]] auto used_height() const noexcept -> int { return shelf_y_ + shelf_height_; }
  /// 已分配的条目数（诊断/测试用）。
  [[nodiscard]] auto entry_count() const noexcept -> std::size_t { return entries_.size(); }

 private:
  struct Entry {
    Region region;
  };

  int padding_{1};
  int canvas_width_{0};
  int canvas_height_{0};
  /// 当前货架的左上角 y，以及该货架已用高度与当前 x 游标。
  int shelf_y_{0};
  int shelf_height_{0};
  int cursor_x_{0};
  std::vector<Entry> entries_;
};

/// 图集的**扩容决策**（拆出来是为了可测）。
///
/// 返回下一档尺寸。策略是**面积翻倍**（宽度优先，因为字形排布通常宽大于高）：
///   · 若当前宽度 < 上限 → 宽度翻倍（高度不变）
///   · 否则高度翻倍
///
/// 上限防止无限增长（超出说明“图集装不下这么多字形”，调用方该考虑淘汰）。
[[nodiscard]] auto next_capacity(int width, int height, int max_dimension) noexcept
    -> std::optional<std::pair<int, int>>;

}  // namespace st::raster::atlas
