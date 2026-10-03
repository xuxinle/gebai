#pragma once

/// SVG（图标子集）解析与渲染：**矢量数据源 + 按目标尺寸重新光栅化**——任意缩放清晰的根源。
///
/// 设计取向（与自研光栅器/字体同一路线：**不引第三方**，只支持图标场景需要的子集，
/// 但子集内做到与主流图标库（VSCode Codicons / Lucide / Feather / Tabler）真实数据兼容）：
/// - XML：元素/属性/自闭合/注释/CDATA——无 DTD、无命名空间前缀处理（属性按本地名匹配）；
/// - `path d`：**全指令** M/L/H/V/C/S/Q/T/A/Z + 大小写（相对坐标）+ 隐式重复 + 科学计数法；
/// - 基本形状：rect（含圆角 rx/ry、百分比）、circle、ellipse、line、polyline、polygon；
/// - `viewBox` → 目标矩形变换（`preserveAspectRatio` 的 meet/slice × 对齐枚举，默认
///   `xMidYMid meet`——图标渲染不裁切、居中缩放）；
/// - 样式：fill / stroke / stroke-width / stroke-linecap / fill-rule / opacity /
///   fill-opacity / stroke-opacity / none / currentColor + `<g>` 级联继承 +
///   `transform` 属性（translate/scale/rotate/matrix）；
/// - **sprite**：`<symbol id>` + `<use href="#id">`（图标库标准组织形态；
///   `<use>` 链式引用与自引用防环）。
///
/// 渲染：解析产物是**路径树**（`Document::nodes`），不是位图——绘制时按目标 box 变换后
/// 交给 `raster::Canvas`，每个目标尺寸都从矢量重新光栅化（位图放大 = 模糊，矢量重栅 = 清晰）。
/// 颜色缺省语义对齐主流图标库：**无 fill 属性 = 黑色实心**（Codicons/Font Awesome 形态），
/// 显式 `fill="none"` 才是描边形态（Lucide/Feather 形态）。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/raster/surface.hpp"
#include "st/raster/canvas.hpp"

namespace st::ui::svg {

/// 路径填充规则（even-odd 用于含孔图形：环形、回字、a/b/o/d…）。
enum class FillRule : std::uint8_t { NonZero, EvenOdd };

/// 渲染一个节点需要的全部样式（继承链上合并后的结果）。
struct Style {
  std::optional<math::Color> fill{};         ///< 显式 fill 色（nullopt = 未指定/缺省黑）
  std::optional<math::Color> stroke{};
  /// `currentColor` 语义：取环境色（图标场景 = 调用方传入的 `override_color`）。
  /// 单色图标（Lucide/Feather/Codicons）几乎全用它——主题色/语义色着色的错由此实现。
  bool fill_current{false};
  bool stroke_current{false};
  float stroke_width{1.0f};
  float opacity{1.0f};
  float fill_opacity{1.0f};
  float stroke_opacity{1.0f};
  FillRule fill_rule{FillRule::NonZero};
  bool fill_none{false};
  bool stroke_none{false};
};

/// `transform` 属性的仿射矩阵（行主序 2×3：`[a c e; b d f]`，SVG 口径）。
struct Transform {
  float a{1.0f}, b{0.0f}, c{0.0f}, d{1.0f}, e{0.0f}, f{0.0f};

  [[nodiscard]] static auto identity() noexcept -> Transform { return {}; }
  [[nodiscard]] static auto translate(float tx, float ty) noexcept -> Transform;
  [[nodiscard]] static auto scale(float sx, float sy) noexcept -> Transform;
  /// 角度制，绕原点。
  [[nodiscard]] static auto rotate(float degrees) noexcept -> Transform;
  [[nodiscard]] static auto matrix(float a, float b, float c, float d, float e,
                                   float f) noexcept -> Transform;
  [[nodiscard]] auto operator*(const Transform& rhs) const noexcept -> Transform;
  [[nodiscard]] auto apply(math::Point point) const noexcept -> math::Point;
  /// 描边宽度等标量随本变换的近似缩放（x/y 向量长度的几何均值）。
  [[nodiscard]] auto scalar_factor() const noexcept -> float;
};

/// 解析后的一个绘制节点（一个 `path`/`rect`/… 元素）。
struct Node {
  raster::Path path{};      ///< 已是绝对坐标、目标文档空间的路径
  Style style{};            ///< 合并继承后的最终样式
  float stroke_width_viewbox{1.0f};  ///< viewBox 空间下的描边宽（渲染时随缩放换算）
};

/// 解析产物：viewBox（缺省 0 0 24 24，图标事实标准）+ 节点表。
struct Document {
  float view_x{0.0f};
  float view_y{0.0f};
  float view_w{24.0f};
  float view_h{24.0f};
  std::vector<Node> nodes{};

  [[nodiscard]] auto is_empty() const noexcept -> bool { return nodes.empty(); }
};

/// 解析 SVG 文本（`<svg>` 根或 `<symbol>` 片段均可）。失败返回 nullopt——**解析器不抛异常、
/// 不崩溃**：不认识的元素/属性/指令一律跳过（图标场景宁缺毋滥，不猜语义）。
[[nodiscard]] auto parse(std::string_view source) -> std::optional<Document>;

/// viewBox（`view_x..`）→ 目标矩形 `box` 的变换（preserveAspectRatio 语义；
/// `slice=false` 即 meet：完整可见、居中/按对齐留白）。
[[nodiscard]] auto fit_transform(const Document& doc, math::Rect box,
                                 bool slice = false) -> Transform;

/// 绘制到画布：按 `box` 缩放 + 可选整体单色覆盖（图标场景：主题色/语义色着色；
/// `override_color` 为 nullopt 时用文档自带颜色）。矢量按目标尺寸光栅化。
void draw(raster::Surface& canvas, const Document& doc, math::Rect box,
          std::optional<math::Color> override_color = std::nullopt);

// ————————————————————————————————————————————
// 图标集（sprite 形态）：一份 SVG 文本携带 N 个图标，按 symbol id 取用
// ————————————————————————————————————————————

/// 从 sprite 文本加载的图标集（`<symbol id="...">…</symbol>`；单文件单 `<svg>` 根，
/// 根上可有 viewBox（作为 symbol 缺省 viewBox））。
class IconSet {
 public:
  /// 解析并装载；`source` 为空/无 symbol 返回 false（已有内容保持不变）。
  auto load(std::string_view source) -> bool;
  /// 全部图标 id（文档序）。
  [[nodiscard]] auto ids() const -> std::vector<std::string>;
  [[nodiscard]] auto has(std::string_view id) const noexcept -> bool;
  /// 取某图标（每次现场重建；调用方负责缓存位图——见 `IconSetPainter`/组件层）。
  [[nodiscard]] auto get(std::string_view id) const -> std::optional<Document>;
  /// 单图标直载（一个文件一个 `<svg>`，非 sprite）。
  auto load_single(std::string_view id, std::string_view source) -> bool;

 private:
  struct Symbol {
    std::string id;
    std::string source;  // 原始 inner XML（惰性解析）
  };
  std::vector<Symbol> symbols_{};
  /// sprite 根 <svg> 的 presentation 属性原文（`fill="none" stroke="currentColor" …`）。
  /// `get()` 给 symbol 包外壳时必须带上——丢了它们整个图标集会塌成黑色实心剪影
  /// （Lucide 的描边风格全在根上；实测踩过）。
  std::string root_attributes_{};
};

/// **按 (id, 物理尺寸, 颜色) 缓存位图**的绘制器：同一图标同一尺寸第二次绘制零光栅化。
/// 矢量重栅保证清晰，缓存保证开销——两者不矛盾：尺寸变了就是另一个缓存键。
///
/// 上限策略：条目数上限（默认 256，LRU 淘汰）+ 总像素预算（默认 16 MiB）。
class IconSetPainter {
 public:
  struct CacheStats {
    std::size_t entries{0};
    std::size_t hits{0};
    std::size_t misses{0};
  };

  explicit IconSetPainter(std::shared_ptr<const IconSet> set) : set_(std::move(set)) {}

  /// 绘制（`box` 尺寸按 `1`px 对齐量化后作缓存键——亚像素差异不产生新条目）。
  void draw(raster::Surface& canvas, std::string_view id, math::Rect box,
            math::Color color) const;
  /// 丢弃全部缓存（主题切换/内存压力时；图标集本体不受影响）。
  void clear_cache() const;
  [[nodiscard]] auto cache_stats() const -> CacheStats;

 private:
  struct Entry {
    std::string id;
    math::Color color{};
    int size{0};  // 物理像素（量化后）
    mutable raster::Canvas bitmap{0, 0};
    mutable std::uint64_t used{0};
  };
  static constexpr std::size_t kMaxEntries = 256;

  std::shared_ptr<const IconSet> set_{};
  mutable std::vector<Entry> cache_{};
  mutable std::uint64_t clock_{0};
  mutable std::size_t hits_{0};
  mutable std::size_t misses_{0};
  mutable std::size_t cached_pixels_{0};
  static constexpr std::size_t kPixelBudget = 16U << 20U;
};

/// 解析单个 `d` 属性为路径（独立入口：测试与 `Icon` 私有表迁移用）。
[[nodiscard]] auto parse_path(std::string_view d) -> std::optional<raster::Path>;

/// 解析颜色（`#rgb #rgba #rrggbb #rrggbbaa` 与 17 个 CSS 基本色名；`currentColor`
/// 返回 nullopt——语义是"取环境色"，由调用方决定）。
[[nodiscard]] auto parse_color(std::string_view text) -> std::optional<math::Color>;

/// viewBox 空间 → 逻辑像素后的描边宽度换算。
[[nodiscard]] auto stroke_width_px(const Document& doc, math::Rect box) -> float;

}  // namespace st::ui::svg
