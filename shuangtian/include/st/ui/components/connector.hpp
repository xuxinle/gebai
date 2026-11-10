#pragma once

/// @file connector.hpp
/// @brief `Connector`：连接**任意两个点/元素**的线（直线 / 正交折线 / 贝塞尔曲线，
/// 可选箭头、虚线、中点标注、端点吸附与命中）。
///
/// ## 为什么它是一个独立组件（而不是让每个应用自己画）
///
/// 「把两个东西连起来」在界面里是**反复出现**的需求：节点编辑器、流程图、思维导图、
/// 状态机、依赖图、拓扑图、时序标注、组织架构。画一条线本身不难，难的是每次都
/// 要重算一遍的六件事——端点吸附到哪个边/哪个角度、线要不要绕开盒子、箭头要对准
/// 切向、虚线怎么切、线宽怎么随 DPI 变、刷新后端点跑到哪去了。散在各处实现，
/// 这六件事就会各错一遍，而且**错了不会报错**（线画在错的位置上看起来只是"图有点怪"）。
///
/// ## 三种用法（由易到难）
///
/// **① 两点连线**（最常见：直接给坐标）
/// ```cpp
/// auto line = std::make_unique<st::ui::Connector>();
/// line->set_points({100, 100}, {420, 260});
/// line->set_route(st::ui::Connector::Route::Bezier);
/// line->style().absolute = true;   // 叠在画布上，不参与 flex 流
/// canvas_layer->add_child(std::move(line));
/// ```
///
/// **② 连接两个元素**（端点跟着元素走，元素一动线就跟着动）
/// ```cpp
/// auto line = std::make_unique<st::ui::Connector>();
/// line->set_source(from_element, st::ui::Connector::Side::Right);
/// line->set_target(to_element,   st::ui::Connector::Side::Left);
/// overlay->add_child(std::move(line));   // 挂在**共同的祖先**下（见下）
/// ```
///
/// **③ 端点表达式**（不持有元素，每次绘制现取——推荐给"元素列表会重建"的界面）
/// ```cpp
/// line->set_source_fn([this] { return bounds_of_card(3); });
/// line->set_target_fn([this] { return cursor_point; });
/// ```
///
/// ## 坐标系（唯一的对外约定，务必看清）
///
/// 线**画在本组件的坐标盒里**：固定坐标与端点表达式都用**本地坐标**（原点 = `bounds_`
/// 左上角），绘制时统一换到画布绝对坐标。于是——
///
/// - 把 `Connector` 作为一张卡的 `absolute` 子元素（铺满卡片）时，`set_points` 的
///   坐标就是**卡片内容盒内的局部坐标**——页面重排不会飘；
/// - 想用画布绝对坐标：把线的 `bounds` 铺满内容区（`left/top = 0`、`width/height`
///   取父级宽高），此时局部坐标 ≡ 画布坐标。
/// - 落在 `bounds` 之外的坐标也能画（连线常需要跨出自己的盒子），但会被祖先的
///   `clip_children` 裁掉——与所有自绘元素一致。
///
/// 另外两条必须遵守的框架约定（都不是本组件的规定，是全框架的）：
/// - 画在**别的元素下面**时，本组件必须是"后声明"的兄弟（绘制顺序按声明顺序）；
///   更稳的做法是单独一层 overlay 容器。
/// - 命中判定按**描边附近 `hit_tolerance` 像素**算，不是整块 bounds——所以它压在
///   按钮上不会吃掉按钮的点击（除非鼠标正好落在线附近）。
///
/// ## 端点的四种给法（优先级从高到低）
///
/// 1. `set_source_fn` / `set_target_fn`：每次绘制现算的表达式（返回矩形或点）；
/// 2. `set_source(element, side)` / `set_target(element, side)`：跟随某个元素（元素
///    不在树上/不可见时该条线**整条不画**，不会画到 (0,0)）；
/// 3. `set_points(from, to)`：固定坐标（**本组件本地坐标**）；
/// 4. 什么都没设 → 用自身 `bounds` 的左上角到右下角（便于在 gallery 里看到东西）。
///
/// ## 属性面（控制通道 / DSL 可直接读写）
///
/// `route`（straight/orthogonal/curve）、`from`/`to`（"x,y"）、`arrow`（none/start/end/both）、
/// `arrow_size`、`dash`（"6 4"，空 = 实线）、`width`、`tone`、`stroke_color`（`#rrggbb`，
/// 覆盖 tone）、`hit_tolerance`、`label`（中点标注）、`source_side`/`target_side`、
/// `smooth`（正交转角是否圆化）、`curvature`、`min_segment`。
/// 只读：`source_ok`/`target_ok`（端点是否解析成功）、`path_length`（像素长度，便于断言）。
///
/// ## 一个已知取舍：不画在别的元素"里面"自动跟随
///
/// 端点是**每次绘制现取**的（不是缓存），所以"元素被拖走 → 线下一帧就到位"，代价是
/// 一条线每帧重算一次端点矩形（纯矩形读取，微秒级）。若你把线放进某个会滚动的容器，
/// 线的坐标会随容器一起被平移——那是期望行为（跟随内容滚动）。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/math/path.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// 连一条线（端点、路由、箭头、虚线、命中）。
class Connector : public Element {
 public:
  /// 路由形态。
  enum class Route : std::uint8_t {
    /// 两端直线相连。
    Straight,
    /// 正交折线（水平出、竖直走、水平入；`smooth` 把转角改成小圆弧）。
    Orthogonal,
    /// 三次贝塞尔：控制点沿两端的"出线方向"外推（水平出线则线条在端点处水平）。
    Curve,
  };

  /// 端点吸附的位置。
  enum class Side : std::uint8_t {
    Center,  ///< 矩形中心
    Top,
    Right,
    Bottom,
    Left,
    ///< **自动**：朝对方那一侧（最常用的"看着自然"形态）
    Auto,
  };

  /// 箭头位置。
  enum class Arrow : std::uint8_t {
    None,
    Start,
    End,
    Both,
  };

  static constexpr float kDefaultWidth{1.5f};      ///< 默认线宽（逻辑像素）
  static constexpr float kDefaultArrowSize{9.0f};  ///< 默认箭头长（逻辑像素）
  static constexpr float kDefaultTolerance{5.0f};  ///< 默认命中带宽（两侧各 5px）
  static constexpr float kMinCornerRadius{2.0f};   ///< 正交路由转角圆弧下限
  static constexpr float kMaxCornerRadius{18.0f};  ///< 正交路由转角圆弧上限

  Connector();
  explicit Connector(math::Point from, math::Point to);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Connector"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Separator; }

  // —— 端点 ——
  /// 固定坐标（两端都给；单端用 `set_source_point`/`set_target_point`）。
  /// 坐标是**本组件本地坐标**（原点 = 自身 bounds 左上角）。
  void set_points(math::Point from, math::Point to);
  void set_source_point(math::Point from);
  void set_target_point(math::Point to);
  [[nodiscard]] auto source_point() const noexcept -> math::Point { return fixed_from_; }
  [[nodiscard]] auto target_point() const noexcept -> math::Point { return fixed_to_; }
  [[nodiscard]] auto has_fixed_points() const noexcept -> bool { return fixed_points_; }

  /// 跟随某个元素（该元素须与本组件在**同一棵树上**才有值；不在树上/不可见 → 线不画）。
  ///
  /// `side` 决定吸附到它的哪条边；`Auto` 会按对方节点的位置自动选（见 `anchored()`）。
  void set_source(Element* element, Side side = Side::Auto);
  void set_target(Element* element, Side side = Side::Auto);
  void set_source_side(Side side);
  void set_target_side(Side side);
  [[nodiscard]] auto source_element() const noexcept -> Element* { return source_.node; }
  [[nodiscard]] auto target_element() const noexcept -> Element* { return target_.node; }

  /// 端点表达式（最高优先级）：返回 `std::nullopt` 表示"这一刻没有端点"（线不画）。
  /// 返回矩形时按 `side` 吸附，返回点时直接用它。
  void set_source_fn(std::function<std::optional<math::Rect>()> fn);
  void set_target_fn(std::function<std::optional<math::Rect>()> fn);
  /// 端点表达式返回的是**点**（而不是矩形）。
  void set_source_point_fn(std::function<std::optional<math::Point>()> fn);
  void set_target_point_fn(std::function<std::optional<math::Point>()> fn);

  // —— 形态 ——
  void set_route(Route route);
  [[nodiscard]] auto route() const noexcept -> Route { return route_; }
  /// 正交路由是否把直角改成圆弧（默认 true；纯 90° 方角看起来像表格线）。
  void set_smooth_corners(bool smooth);
  [[nodiscard]] auto smooth_corners() const noexcept -> bool { return smooth_corners_; }
  /// 正交路由里"竖直/水平中段"的最小长度：太短时退化成直线，避免出现 S 形折角。
  void set_min_segment(float length);
  [[nodiscard]] auto min_segment() const noexcept -> float { return min_segment_; }
  /// 贝塞尔曲线的弯曲强度（0 = 退化为直线；1 = 默认；>1 更弯）。
  void set_curvature(float curvature);
  [[nodiscard]] auto curvature() const noexcept -> float { return curvature_; }

  void set_arrow(Arrow arrow);
  [[nodiscard]] auto arrow() const noexcept -> Arrow { return arrow_; }
  void set_arrow_size(float size);
  [[nodiscard]] auto arrow_size() const noexcept -> float { return arrow_size_; }

  void set_width(float width);
  [[nodiscard]] auto width() const noexcept -> float { return width_; }
  /// 虚线样式（单位逻辑像素；空 = 实线）。非法输入（负值/全零）被忽略。
  void set_dash(std::vector<float> pattern);
  void set_dash_csv(std::string_view csv);
  [[nodiscard]] auto dash() const noexcept -> const std::vector<float>& { return dash_; }
  [[nodiscard]] auto dash_csv() const -> std::string;

  void set_tone(Tone tone);
  [[nodiscard]] auto tone() const noexcept -> Tone { return tone_; }
  /// 显式颜色（`#rrggbb` / `#rrggbbaa` / 色名 / 空 = 跟随 tone）。
  /// **非法色值返回 false**（属性面据此如实拒绝，不假装改了）。
  auto set_color_hex(std::string_view hex) -> bool;
  [[nodiscard]] auto color_hex() const -> std::string;

  /// 命中带宽（两侧各多少像素；0 = 只能正好压在线中心才命中）。
  void set_hit_tolerance(float tolerance);
  [[nodiscard]] auto hit_tolerance() const noexcept -> float { return hit_tolerance_; }

  /// 线上滚动的标注文本（空 = 不画）。画在中点、带一层底衬，避免压在线上读不清。
  void set_label(std::string label);
  [[nodiscard]] auto label() const noexcept -> const std::string& { return label_; }

  /// 端点悬停/拖动注入：线段附近发生的事件先问这些回调（返回 true = 已消费）。
  ///
  /// 为什么不走 `on_event` 覆写：命中区是**沿线的窄带**，而 `on_event` 只对命中到
  /// 本元素的点派发——两者必须同源（同一个 `distance_to_path`），否则"点到了但
  /// 没反应"。本组件把两者都收在这里：`hit_test` 用同一条判据。
  std::function<bool(const Event&)> on_line_event{};

  // —— 诊断（测试与自动化断言用） ——
  /// 端点是否解析成功（元素不在树上/表达式返回 nullopt → false）。
  [[nodiscard]] auto source_ok() const -> bool;
  [[nodiscard]] auto target_ok() const -> bool;
  /// 当前解析出的两端（画布绝对坐标）。
  [[nodiscard]] auto resolved_from() const -> math::Point;
  [[nodiscard]] auto resolved_to() const -> math::Point;
  /// 当前这条线的像素长度（两端解析失败时为 0）。
  [[nodiscard]] auto path_length() const -> float;
  /// 当前这条线的路径（画布绝对坐标；端点解析失败时为空路径）。
  [[nodiscard]] auto build_path() const -> math::Path;
  /// 路径控制点个数（含贝塞尔控制点）。正交/曲线路由比直线多。
  [[nodiscard]] auto path_point_count() const -> std::size_t;
  /// 点到这条线的最近距离（画布绝对坐标）。`hit_test` 与测试共用这一条判据。
  [[nodiscard]] auto distance_to_line(math::Point point) const -> float;
  /// **端点被人为移动后**（拖拽节点、列表重排）调一次：标脏 + 上报整块 bounds。
  ///
  /// 为何不只是 `mark_dirty()`：损坏区只覆盖本组件自己的 bounds，而旧线可能画到了
  /// bounds 之外（端点在上一次绘制时更远）——那块区域的旧像素没人清，表现为
  /// "拖动后留下一条残影"。
  void refresh();

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;

  /// 端点解析结果（供 `paint_content` 与诊断共用一份，避免两处各算一遍导致不一致）。
  struct Resolved {
    bool ok{false};
    math::Point from{};   ///< 已吸附后的画布绝对坐标
    math::Point to{};
    math::Point from_dir{};  ///< 出线方向（单位向量；贝塞尔控制点用）
    math::Point to_dir{};    ///< 入线方向（从外侧指向端点，即"线进入端点的方向"）
  };
  [[nodiscard]] auto resolve() const -> Resolved;

  /// 把一个矩形按 `side` 吸附成一点；`toward` 用于 `Side::Auto`。
  [[nodiscard]] static auto anchor_of(math::Rect rect, Side side, math::Point toward) -> math::Point;
  /// 吸附点在该矩形上的**外向法线方向**（单位向量）。
  [[nodiscard]] static auto outward_of(math::Rect rect, math::Point anchor) -> math::Point;

 private:
  /// 端点来源（三者互斥，优先级见文件头）。
  struct Endpoint {
    std::function<std::optional<math::Rect>()> rect_fn{};
    std::function<std::optional<math::Point>()> point_fn{};
    Element* node{nullptr};
    Side side{Side::Auto};
    bool has_fixed{false};
    math::Point fixed{};
  };

  [[nodiscard]] auto resolve_endpoint(const Endpoint& endpoint, math::Point fallback,
                                      math::Point toward) const
      -> std::optional<std::pair<math::Point, math::Point>>;
  [[nodiscard]] auto route_points(math::Point from, math::Point to, math::Point from_dir,
                                  math::Point to_dir) const -> math::Path;

  Endpoint source_{};
  Endpoint target_{};
  bool fixed_points_{false};
  math::Point fixed_from_{};
  math::Point fixed_to_{};

  Route route_{Route::Straight};
  bool smooth_corners_{true};
  float min_segment_{18.0f};
  float curvature_{1.0f};

  Arrow arrow_{Arrow::None};
  float arrow_size_{kDefaultArrowSize};

  float width_{kDefaultWidth};
  std::vector<float> dash_{};
  Tone tone_{Tone::Muted};
  std::optional<math::Color> explicit_color_{};
  float hit_tolerance_{kDefaultTolerance};
  std::string label_{};

  math::Color stroke_color_{};
  math::Color label_color_{};
  math::Color label_background_{};

  /// 上一帧画出来的路径包围盒（画布绝对坐标）——`refresh()` 用它把**旧线的残影**
  /// 一起报进损坏区（端点被拖走时旧线可能落在本组件 bounds 之外）。
  mutable math::Rect last_path_bounds_{};
  mutable bool last_path_valid_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Connector, "Connector")
