/// @file
/// @brief 原生子窗口**解除嵌入**时的几何还原决策（纯几何，无平台头）。
///
/// ## 为什么这件事需要一段独立的、可单测的逻辑
///
/// 嵌入期为让外部窗口的自绘标题栏退到宿主之外，会把它的位置与尺寸都改掉：
/// 窗口上移一个标题栏高度、高度再补上同样的量（见 `platform_win32.cpp`
/// 的 `sync_child_view`）。解除嵌入时只把父窗口摘回桌面、恢复样式位，
/// **窗口矩形仍停在嵌入期那个位置**——对顶层窗口而言那个位置就是屏幕坐标，
/// 于是标题栏正好落在屏幕上方之外：窗口看得见、拖不动。
///
/// 所以解除嵌入必须把几何也还原成**嵌入前**的样子。而"还原到哪儿"这件事
/// 有三条容易被写错的口径，它们与窗口系统无关，是纯几何：
///
/// 1. 嵌入前的位置就是用户记得的位置——原样送回去，不要"顺手居中"；
/// 2. 但若那个位置现在已经不可达（换了显示器、缩放变了、嵌入期间屏幕配置变了），
///    必须把它夹回**它所在那块**工作区，而不是主屏工作区（多显示器下会跨屏搬走）；
/// 3. 夹取只改位置、不改尺寸——窗口大小是用户/外部程序的选择。
///
/// 平台层只负责"执行这个决策"，决策本身在这里，于是它可被单测钉住
/// （平台后端在任何 CI 环境下都跑不起来，没有这一层就只能靠人工看窗口）。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace st::shell::detail {

/// 逻辑像素跨度 → 覆盖它的整数像素跨度（floor / ceil 边缘取整）。
///
/// ## 为什么必须边缘取整、不能对尺寸截断
///
/// 子窗口尺寸若用 `static_cast<int>(size * scale)` 截断，在分数 DPI（1.5×）下
/// 每次都会丢掉小数部分（1247×1.5=1870.5 → 1870），而宿主客户区是按边缘取整
/// 算出来的（1922）。于是子窗口底边盖不到客户区底边，**底部一条背景色缝**
/// （实测：1 物理像素的白线，浅色背景下看不见、深色页面上刺眼）。
/// 边缘取整（左/上 floor、右/下 ceil）保证子窗口**覆盖**全部逻辑区域，
/// 超出的部分会被宿主客户区裁掉，不会溢出到窗外。
[[nodiscard]] inline auto cover_span(float logical_pos, float logical_size, float scale) noexcept
    -> std::pair<int, int> {
  const float begin = logical_pos * scale;
  const float end = (logical_pos + logical_size) * scale;
  const int first = static_cast<int>(std::floor(begin + 0.001f));
  const int last = static_cast<int>(std::ceil(end - 0.001f));
  return {first, std::max(first + 1, last)};
}

/// 屏幕像素矩形（与 Win32 `RECT` 同一口径：右/下开区间，`right - left` 即宽）。
///
/// 不用 `math::Rect`：这里全是整数像素的窗口矩形，浮点只会引入取整噪音，
/// 而"差一像素"在本场景恰好是"拖得动 / 拖不动"的边界。
struct PixelRect {
  int left{0};
  int top{0};
  int right{0};
  int bottom{0};  ///< 右/下为开区间（与 `RECT` 一致）

  [[nodiscard]] auto width() const noexcept -> int { return right - left; }
  [[nodiscard]] auto height() const noexcept -> int { return bottom - top; }
  [[nodiscard]] auto is_valid() const noexcept -> bool {
    return width() > 0 && height() > 0;
  }

  /// 顶边中点——判定"这个窗口还拖得动吗"的取样点。
  ///
  /// 为何取**顶边中点**而不是左上角或中心：
  /// - 左上角正好是某个固定像素，任何一点边框裁切都会让判据翻转；
  /// - 中心在"窗口远高于屏幕"时仍然在屏内，会漏掉"标题栏在屏外"这个正是本次的缺陷；
  /// - 顶边中点就是标题栏所在的那条带子上最不容易落空的位置（左右两侧可能被
  ///   其他窗口占住，但"有没有落在工作区矩形里"只问几何，与遮挡无关）。
  [[nodiscard]] auto title_bar_point() const noexcept -> std::pair<int, int> {
    return {left + width() / 2, top};
  }

  friend auto operator==(const PixelRect&, const PixelRect&) -> bool = default;
};

/// 点是否落在矩形内（右/下开区间）。
[[nodiscard]] inline auto contains(const PixelRect& rect, int x, int y) noexcept -> bool {
  return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

/// 两个矩形的相交面积（不相交为 0）。
[[nodiscard]] inline auto intersection_area(const PixelRect& a, const PixelRect& b) noexcept
    -> std::int64_t {
  const int left = std::max(a.left, b.left);
  const int top = std::max(a.top, b.top);
  const int right = std::min(a.right, b.right);
  const int bottom = std::min(a.bottom, b.bottom);
  if (right <= left || bottom <= top) return 0;
  return static_cast<std::int64_t>(right - left) * static_cast<std::int64_t>(bottom - top);
}

/// 标题栏是否可达：顶边中点落在**任一**工作区内。
///
/// 用"工作区"而不是"显示器矩形"：工作区已扣掉任务栏，而标题栏缩到任务栏底下
/// 同样拖不动（这正是任务栏自动隐藏时也不该被判为可达的原因）。
[[nodiscard]] inline auto title_bar_reachable(PixelRect window,
                                              std::span<const PixelRect> work_areas) noexcept -> bool {
  if (!window.is_valid()) return false;
  const auto [x, y] = window.title_bar_point();
  return std::ranges::any_of(work_areas,
                             [x, y](const PixelRect& work) { return contains(work, x, y); });
}

/// 解除嵌入后应当下发的矩形（屏幕像素）。
///
/// - `saved`：**嵌入前**捕获的屏幕矩形（用户记得的那个位置），`nullopt` = 没记到；
/// - `current`：窗口**当前**矩形（嵌入期那个位置，已被上移一个标题栏高度）；
/// - `work_areas`：各显示器工作区（顺序无关）。
///
/// ## 为何总是返回一个矩形，而不是 `optional`
///
/// 因为尺寸也必须还原：嵌入期窗口被加高了一个标题栏高度，光恢复位置会让它
/// 比原尺寸高。若用 `optional` 表达“不用动”，调用方很容易在“位置不用动”时
/// 连尺寸也不还原——那正是本次缺陷的形状（只恢复样式、没恢复几何）。
/// 所以这里承诺：**总是给出完整的目标矩形**，调用方无条件下发。
///
/// ## 三条口径
///
/// 1. **位置原样送回**（若可达）——不要“顺手居中”，那是用户的窗口；
/// 2. 可达性以**标题栏**为准（顶边中点落在某块工作区内）：标题栏在屏外就是拖不动；
/// 3. 不可达时夹回相交面积最大的那块工作区，**只平移不改尺寸**；
///    比工作区还大时对齐左上边（标题栏与左边缘必定可见）。
///
/// `saved` 缺失时用 `current` 当基准：那个位置虽带着上移量，但“夹回可达处”
/// 总比“原样留个拖不动的窗口”好——这是兜底路径，不是正常路径。
[[nodiscard]] inline auto plan_detach_placement(std::optional<PixelRect> saved, PixelRect current,
                                               std::span<const PixelRect> work_areas)
    -> PixelRect {
  const PixelRect base = saved.has_value() && saved->is_valid() ? *saved : current;
  if (!base.is_valid()) return base;
  if (work_areas.empty()) return base;   // 问不到工作区：只能原样交回去
  if (title_bar_reachable(base, work_areas)) return base;

  // 选落点工作区：相交面积最大者；全不相交用第一块（等价于"最近的那块屏幕"，
  // 平台层传进来时已按窗口所在显示器排好序）。
  const PixelRect* target = &work_areas.front();
  std::int64_t best_area = intersection_area(base, *target);
  for (const PixelRect& work : work_areas) {
    const std::int64_t area = intersection_area(base, work);
    if (area > best_area) {
      best_area = area;
      target = &work;
    }
  }

  // **只平移，不改尺寸**。窗口比工作区还大时无法完全夹进去，那就对齐左上边——
  // 标题栏与左边缘一定可见（拖得动、也够得着关闭按钮）。
  PixelRect placed = base;
  const int width = placed.width();
  const int height = placed.height();
  if (width <= target->width()) {
    placed.left = std::clamp(placed.left, target->left, target->right - width);
  } else {
    placed.left = target->left;
  }
  if (height <= target->height()) {
    placed.top = std::clamp(placed.top, target->top, target->bottom - height);
  } else {
    placed.top = target->top;
  }
  placed.right = placed.left + width;
  placed.bottom = placed.top + height;
  return placed;
}

}  // namespace st::shell::detail
