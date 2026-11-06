#pragma once

/// 裁云（Nimbus）· **品牌与版式系统**。
///
/// ## 为什么需要这一层
///
/// 在这之前，每个场景各自写坐标（`kDesignWidth * 0.5f`、`y = 250.0f`…）——
/// 于是：
/// - 各场景的左边距/标题基线互不相同，"整片看下来不稳定"（这就是"太简单"里
///   最容易被忽略的一半：不是元素少，而是**没有系统**）；
/// - 没有任何品牌标记，所以画面看起来像"随便摆的几个字"而不是产品物料；
/// - 每加一个场景就要重新调一遍坐标，改版式等于重写。
///
/// 这一层把三件事固定下来：
/// 1. **12 栅格 + 安全边距**：所有内容落在同一套列上；
/// 2. **垂直节奏（baseline grid）**：标题/副标/正文的行距成比例，不手调；
/// 3. **品牌标记与装饰原语**：logo、角标、分隔线、进度指示——这些是"产品感"的
///    实际来源，而不是特效。
///
/// ## 边界
///
/// 只负责**位置与结构**，不负责绘制。绘制仍在 `motion.cpp` 的 `Painter` 里，
/// 这样版式可以独立测量（`layout_probe` 工具与 e2e 都能读），也不会把绘制逻辑
/// 分散到两个文件。

#include <string>
#include <string_view>

namespace caiyun {

/// 简易二维点（避免为两个 float 引入几何头）。
struct Point2 {
  float x{0.0f};
  float y{0.0f};
};

/// 网格系统：把一个画面切成 12 列，并给出常用的对齐锚点。
///
/// 列宽按"总宽 - 两侧安全边距"计算——安全边距不是装饰，是**电视/手机裁切安全区**
/// （有些播放器会做轻微裁切，贴边的元素会被切掉）。
struct Grid {
  float width{1920.0f};
  float height{1080.0f};
  float margin{96.0f};          ///< 两侧安全边距（设计像素）
  int columns{12};              ///< 列数
  float gutter{24.0f};          ///< 列间距

  [[nodiscard]] static auto make(float w, float h) -> Grid {
    Grid grid{};
    grid.width = w;
    grid.height = h;
    // 边距随画幅缩放：1080p 用 96，4K 同比到 192，否则"越大的画幅内容越贴边"。
    grid.margin = w * 0.05f;
    grid.gutter = w / 80.0f;
    return grid;
  }

  /// 可用内容宽（去掉两侧边距）。
  [[nodiscard]] auto content_width() const noexcept -> float { return width - margin * 2.0f; }

  /// 第 `index` 列（0 起）的左边界。
  [[nodiscard]] auto column_x(int index) const -> float;
  /// 跨 `span` 列的宽度。
  [[nodiscard]] auto span_width(int span) const -> float;
  /// 一列的中心（用于"内容整体居中"这种最常见需求）。
  [[nodiscard]] auto center_x() const noexcept -> float { return width * 0.5f; }

  /// 垂直节奏：把高度按 `step` 等分，返回第 `index` 条基线的 y。
  ///
  /// 用途：标题/副标题/正文的落点用同一套步长，行距自然一致。
  /// 手写 `y = 250 / 312 / 380` 这类数字会让每个场景的行距都不一样。
  [[nodiscard]] auto baseline(int index, float step) const -> float;
};

/// 品牌标记（标志 + 字标）。
///
/// 产品物料与"几个字"的差别，很大一部分就在这里：**每一帧都带一致的识标**。
struct BrandMark {
  std::string wordmark{"歌白"};       ///< 字标（中文名）
  std::string latin{"GEBAI AGENT"};   ///< 拉丁名
  /// 标志的几何语言：同心环 + 中心点（与产品 logo 同构，不用位图——
  /// 位图在任何尺寸下都可能糊，几何在任何 DPI 下都锐）。
  bool draw_glyph{true};
  float glyph_radius{26.0f};          ///< 标志半径（设计像素）
};

/// 版式槽位：常用内容块的落点。全部由 `Grid` 推导，不硬编码。
struct LayoutSlots {
  Grid grid{};
  BrandMark brand{};

  /// 品牌角标位置（左上角，安全边距内）。
  [[nodiscard]] auto brand_origin() const -> Point2 { return {grid.margin, grid.margin}; }

  /// 大标题区（垂直居中偏上）——不同场景共用，保证"标题永远在同一个高度"。
  [[nodiscard]] auto title_y() const -> float { return grid.height * 0.34f; }
  /// 副标题紧跟标题，间距与标题字号成比例（而非固定像素）。
  [[nodiscard]] auto subtitle_y(float title_size) const -> float {
    return title_y() + title_size * 1.28f;
  }
  /// 内容区（卡片、指标）的顶部。
  [[nodiscard]] auto content_y() const -> float { return grid.height * 0.42f; }
  /// 页脚（页码/说明/品牌小字）。
  [[nodiscard]] auto footer_y() const -> float { return grid.height - grid.margin * 0.9f; }
};

}  // namespace caiyun
