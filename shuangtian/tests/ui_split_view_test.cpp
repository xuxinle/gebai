/// 分栏组件单测：`SplitView`（比例/拖拽/键盘步进/属性面/动作面/几何等价/方向变更）。
///
/// 宿主约束：ui 层不依赖 text 层，本测试作为宿主把 `text::TextRenderer` 适配成 `ui::TextPort`
/// 注入 `UiRoot`（本组件不画文字，没有字体也能跑）。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/split_view.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Point;
using st::math::Rect;
using st::ui::Element;
using st::ui::Panel;
using st::ui::SplitView;

auto dispatch_kind(st::ui::UiRoot& root, st::ui::EventKind kind, Point point) -> bool {
  st::ui::Event event;
  event.kind = kind;
  event.position = point;
  return root.dispatch(event);
}

auto press_key(st::ui::UiRoot& root, std::string_view key) -> bool {
  st::ui::Event event;
  event.kind = st::ui::EventKind::KeyDown;
  event.key = std::string(key);
  return root.dispatch(event);
}

/// 夹具：SplitView 挂根内容，两侧是具名面板（可查几何）。
struct SplitFixture {
  st::ui::UiRoot root{};
  SplitView* split{nullptr};
  Element* first{nullptr};
  Element* second{nullptr};

  SplitFixture(SplitView::Orientation orientation, float ratio, st::math::Size viewport) {
    root.set_viewport(viewport);
    auto split_view = std::make_unique<SplitView>(orientation);
    split_view->set_id("split");
    split_view->set_ratio(ratio, false);
    auto left = std::make_unique<Panel>();
    left->set_id("pane-first");
    auto right = std::make_unique<Panel>();
    right->set_id("pane-second");
    first = left.get();
    second = right.get();
    split_view->set_first(std::move(left));
    split_view->set_second(std::move(right));
    split = split_view.get();
    root.set_content(std::move(split_view));
    root.layout(true);
  }

  ~SplitFixture() { root.set_content(nullptr); }

  SplitFixture(const SplitFixture&) = delete;
  auto operator=(const SplitFixture&) -> SplitFixture& = delete;

  [[nodiscard]] auto handle() const -> Rect { return split->handle_rect(); }
};

}  // namespace

ST_TEST(ui_split_view_ratio_layout_and_clamp) {
  SplitFixture fixture(SplitView::Orientation::Horizontal, 0.3f, st::math::Size{800.0f, 400.0f});
  auto* split = fixture.split;
  ST_REQUIRE(split != nullptr);
  ST_REQUIRE(!split->bounds().is_empty());
  ST_REQUIRE(fixture.first != nullptr);
  ST_REQUIRE(fixture.second != nullptr);

  // ① 初始几何：两侧不重叠、不漏缝（首宽 + 手柄 + 次宽 = 总宽）
  const Rect whole = split->bounds();
  const Rect a = fixture.first->bounds();
  const Rect b = fixture.second->bounds();
  const Rect handle = fixture.handle();
  ST_CHECK_NEAR(a.x, whole.x, 0.01f);
  ST_CHECK_NEAR(a.width + handle.width + b.width, whole.width, 0.01f);
  ST_CHECK_NEAR(b.x, a.right() + handle.width, 0.01f);
  ST_CHECK_NEAR(handle.x, a.right(), 0.01f);
  // 比例 0.3：首面板占（总宽 - 手柄）的 30%
  const float usable = whole.width - handle.width;
  ST_CHECK_NEAR(a.width, usable * 0.3f, 0.01f);
  ST_CHECK_NEAR(b.width, usable * 0.7f, 0.01f);

  // ② 手柄在分界线中心
  ST_CHECK_NEAR(handle.center().x, a.right() + handle.width * 0.5f, 0.01f);

  // ③ 夹取：超范围写比例被夹到 [min, 1-min]
  split->set_ratio(0.0f, false);
  ST_CHECK_NEAR(split->ratio(), split->min_ratio(), 0.0001f);
  split->set_ratio(1.0f, false);
  ST_CHECK_NEAR(split->ratio(), 1.0f - split->min_ratio(), 0.0001f);

  // min_ratio 改大后当前比例随之夹取
  split->set_min_ratio(0.4f);
  ST_CHECK(split->ratio() <= 0.6f + 0.0001f);
  ST_CHECK(split->ratio() >= 0.4f - 0.0001f);
}

ST_TEST(ui_split_view_drag_moves_ratio_and_persists_out_of_handle) {
  SplitFixture fixture(SplitView::Orientation::Horizontal, 0.5f, st::math::Size{800.0f, 400.0f});
  auto& root = fixture.root;
  auto* split = fixture.split;
  ST_REQUIRE(split != nullptr);

  int changes = 0;
  float last = -1.0f;
  split->on_change = [&](float value) {
    ++changes;
    last = value;
  };

  const Rect whole = split->bounds();
  const Rect handle = fixture.handle();
  const float mid_y = whole.center().y;

  // ① 按下手柄中心 → 拖到 600px 处：比例大幅后移
  ST_CHECK(dispatch_kind(root, st::ui::EventKind::MouseDown,
                         Point{handle.center().x, mid_y}));
  ST_CHECK(dispatch_kind(root, st::ui::EventKind::MouseMove, Point{600.0f, mid_y}));
  ST_CHECK(changes >= 1);
  ST_CHECK_NEAR(last, split->ratio(), 0.0001f);
  const float expected =
      (600.0f - whole.x - split->handle_size() * 0.5f) / (whole.width - split->handle_size());
  ST_CHECK_NEAR(split->ratio(), std::clamp(expected, 0.0f, 1.0f), 0.01f);

  // ② 拖出手柄（远超右边界）仍继续跟随（UiRoot 拖拽归属）→ 夹到上限
  ST_CHECK(dispatch_kind(root, st::ui::EventKind::MouseMove, Point{5000.0f, mid_y}));
  ST_CHECK_NEAR(split->ratio(), 1.0f - split->min_ratio(), 0.0001f);

  // ③ 释放后 move 不再改比例
  ST_CHECK(dispatch_kind(root, st::ui::EventKind::MouseUp, Point{5000.0f, mid_y}));
  const float frozen = split->ratio();
  (void)dispatch_kind(root, st::ui::EventKind::MouseMove, Point{100.0f, mid_y});
  ST_CHECK_NEAR(split->ratio(), frozen, 0.0001f);

  // ④ 拖拽后几何一致（手柄仍在分界线中心）
  const Rect a = fixture.first->bounds();
  const Rect now_handle = fixture.handle();
  ST_CHECK_NEAR(a.width + now_handle.width + fixture.second->bounds().width, whole.width, 0.01f);
  ST_CHECK_NEAR(now_handle.x, a.right(), 0.01f);
}

ST_TEST(ui_split_view_drag_preserves_grab_offset) {
  // 抓取点在手柄边上（不是正中）时，拖拽不跳变——抓取偏移在 MouseDown 时记录，全程保持。
  SplitFixture fixture(SplitView::Orientation::Horizontal, 0.5f, st::math::Size{800.0f, 400.0f});
  auto& root = fixture.root;
  auto* split = fixture.split;
  ST_REQUIRE(split != nullptr);

  const Rect handle = fixture.handle();
  const float mid_y = split->bounds().center().y;
  // 抓在手柄右缘（中心 + 3px）
  const float grab_x = handle.center().x + 3.0f;
  ST_CHECK(dispatch_kind(root, st::ui::EventKind::MouseDown, Point{grab_x, mid_y}));
  // 拖 100px：比例应恰好多移动 100px（不是跳到指针）
  const float before = split->ratio();
  ST_CHECK(dispatch_kind(root, st::ui::EventKind::MouseMove, Point{grab_x + 100.0f, mid_y}));
  const float whole_usable = split->bounds().width - split->handle_size();
  ST_CHECK_NEAR(split->ratio() - before, 100.0f / whole_usable, 0.005f);
  (void)dispatch_kind(root, st::ui::EventKind::MouseUp, Point{grab_x + 100.0f, mid_y});
}

ST_TEST(ui_split_view_keyboard_and_actions) {
  SplitFixture fixture(SplitView::Orientation::Horizontal, 0.5f, st::math::Size{800.0f, 400.0f});
  auto& root = fixture.root;
  auto* split = fixture.split;
  ST_REQUIRE(split != nullptr);

  split->set_focusable(true);
  root.set_focus(split);
  const float step = split->step();

  // ① 键盘：→ 增、← 减、Home/End 到两端
  ST_CHECK(press_key(root, "ArrowRight"));
  ST_CHECK_NEAR(split->ratio(), 0.5f + step, 0.0001f);
  ST_CHECK(press_key(root, "ArrowLeft"));
  ST_CHECK_NEAR(split->ratio(), 0.5f, 0.0001f);
  ST_CHECK(press_key(root, "Home"));
  ST_CHECK_NEAR(split->ratio(), split->min_ratio(), 0.0001f);
  ST_CHECK(press_key(root, "End"));
  ST_CHECK_NEAR(split->ratio(), 1.0f - split->min_ratio(), 0.0001f);

  // ② 动作面：step_forward / step_backward / reset / set
  split->set_ratio(0.5f, false);
  ST_CHECK(split->invoke_action("step_forward", {}));
  ST_CHECK_NEAR(split->ratio(), 0.5f + step, 0.0001f);
  ST_CHECK(split->invoke_action("step_backward", {}));
  ST_CHECK_NEAR(split->ratio(), 0.5f, 0.0001f);
  ST_CHECK(split->invoke_action("set", "0.25"));
  ST_CHECK_NEAR(split->ratio(), 0.25f, 0.0001f);
  ST_CHECK(split->invoke_action("reset", {}));
  ST_CHECK_NEAR(split->ratio(), 0.5f, 0.0001f);
  // ② b 双击（activate）= 归位
  split->set_ratio(0.8f, false);
  split->activate();
  ST_CHECK_NEAR(split->ratio(), 0.5f, 0.0001f);
}

ST_TEST(ui_split_view_property_and_semantics) {
  SplitFixture fixture(SplitView::Orientation::Horizontal, 0.35f, st::math::Size{800.0f, 400.0f});
  auto* split = fixture.split;
  ST_REQUIRE(split != nullptr);

  // ① 属性面读写
  ST_CHECK_EQ(split->get_property("ratio").value_or(""), "0.35");
  ST_CHECK(split->set_property("ratio", "0.6"));
  ST_CHECK_NEAR(split->ratio(), 0.6f, 0.0001f);
  ST_CHECK(split->set_property("min_ratio", "0.2"));
  ST_CHECK_NEAR(split->min_ratio(), 0.2f, 0.0001f);
  ST_CHECK(split->set_property("orientation", "vertical"));
  ST_CHECK(split->orientation() == SplitView::Orientation::Vertical);
  ST_CHECK_EQ(split->get_property("orientation").value_or(""), "vertical");
  ST_CHECK(!split->set_property("ratio", "abc"));  // 非法值不生效
  ST_CHECK(!split->set_property("no_such", "1"));

  // ② 语义：文本描述方向、值报百分比
  ST_CHECK_EQ(std::string(split->semantics_text()), "上下分栏");
  ST_CHECK_EQ(split->semantics_value(), "60%");
}

ST_TEST(ui_split_view_vertical_orientation_geometry) {
  SplitFixture fixture(SplitView::Orientation::Vertical, 0.4f, st::math::Size{600.0f, 500.0f});
  auto* split = fixture.split;
  ST_REQUIRE(split != nullptr);

  const Rect whole = split->bounds();
  const Rect a = fixture.first->bounds();
  const Rect b = fixture.second->bounds();
  const Rect handle = fixture.handle();
  // 上下方向：高度按比例、宽度拉满；手柄横向拉满
  const float usable = whole.height - handle.height;
  ST_CHECK_NEAR(a.height, usable * 0.4f, 0.01f);
  ST_CHECK_NEAR(a.height + handle.height + b.height, whole.height, 0.01f);
  ST_CHECK_NEAR(a.width, whole.width, 0.01f);
  ST_CHECK_NEAR(handle.width, whole.width, 0.01f);
  ST_CHECK_NEAR(handle.y, a.bottom(), 0.01f);

  // 上下分栏的键盘语义：↓ 增、↑ 减
  split->set_ratio(0.5f, false);
  split->set_focusable(true);
  fixture.root.set_focus(split);
  ST_CHECK(press_key(fixture.root, "ArrowDown"));
  ST_CHECK_NEAR(split->ratio(), 0.5f + split->step(), 0.0001f);
  ST_CHECK(press_key(fixture.root, "ArrowUp"));
  ST_CHECK_NEAR(split->ratio(), 0.5f, 0.0001f);
}

ST_TEST(ui_split_view_single_pane_and_dsl_factory) {
  // ① 只有一侧：布局优雅退化（单面板占满可用空间），绝不崩
  st::ui::UiRoot root{};
  root.set_viewport(st::math::Size{400.0f, 300.0f});
  auto only = std::make_unique<SplitView>();
  auto pane = std::make_unique<Panel>();
  pane->set_id("only-pane");
  Element* pane_ptr = pane.get();
  only->set_first(std::move(pane));
  SplitView* split = only.get();
  root.set_content(std::move(only));
  root.layout(true);
  ST_CHECK(!pane_ptr->bounds().is_empty());
  ST_CHECK_NEAR(pane_ptr->bounds().width, split->bounds().width, 0.01f);
  root.set_content(nullptr);

  // ② dsl 工厂可造（`ui.create` / 声明式共用注册表）
  auto made = st::ui::dsl::make_element("SplitView");
  ST_REQUIRE(made != nullptr);
  ST_CHECK_EQ(std::string(made->type()), "SplitView");
}

// ── 分隔线必须画在**两面板之间**（不是画在面板内部）───────────────
//
// 回归：`paint_content` 里 `handle_rect()` 返回的已经是**绝对坐标**，
// 旧代码又加了一次 `bounds_.x` —— 分隔线整整偏出一个 `bounds_.x`，
// 跑到第二个面板**内部**去（实测：分栏落在 x=44 时线被画到 361.66，
// 而两面板交界只在 314..322；在 gbcode 里就表现为“标签下方一条
// 穿过代码行号栏的竖线”，用户报的正是这个）。
//
// 这里用离屏画布扫像素——几何断言（`handle_rect`）单独看是**发现不了**
// 这个错的（错在“把绝对量又当相对量用了一次”，几何本身没矛盾）。
ST_TEST(ui_split_view_divider_paints_between_panes) {
  // ⚠ 分栏必须**不落在原点**：`bounds_.x == 0` 时「多加一次 bounds_.x」与不加等价，
  // 缺陷根本不会显形（第一版测试就是这么写的，回退修复后依然是绿的——假绿）。
  // 真实场景里分栏几乎总在某层容器内部（gbcode 里 x=44），这里用一个
  // 带左内边距的宿主把它推到非零位置。
  st::ui::UiRoot root{};
  root.set_viewport(st::math::Size{800.0f, 400.0f});
  auto host = std::make_unique<Panel>();
  host->set_id("host");
  host->style().padding = st::math::Insets{44.0f, 30.0f, 0.0f, 30.0f};  // 左 44
  host->style().direction = st::ui::FlexDirection::Row;
  auto split_view = std::make_unique<SplitView>(SplitView::Orientation::Horizontal);
  split_view->set_id("split");
  split_view->set_ratio(0.3f, false);
  auto left = std::make_unique<Panel>();
  left->set_id("pane-first");
  auto right = std::make_unique<Panel>();
  right->set_id("pane-second");
  SplitView* split = split_view.get();
  Element* left_ptr = left.get();
  Element* right_ptr = right.get();
  split_view->set_first(std::move(left));
  split_view->set_second(std::move(right));
  host->add_child(std::move(split_view));
  root.set_content(std::move(host));
  root.layout(true);
  ST_REQUIRE(split->bounds().x > 40.0f);   // 确保确实非原点（防测试自身退化）
  const Rect a = left_ptr->bounds();
  const Rect b = right_ptr->bounds();

  st::raster::Canvas canvas(800, 400);
  root.paint_frame(canvas);

  // 沿一条水平扫描线找**分隔线**（颜色 = 主题 `border`）。
  //
  // 判据必须是**精确颜色**：用「与背景不同」会把面板自己的边界像素也算进来，
  // `inked` 横跨整行，中心值毫无意义（实测差 278.9，纯属把两边边界的中点当成了线）。
  const Rect whole = split->bounds();
  const float y = whole.center().y;
  // 判据 = 「不是根背景色」。
  //
  // 不能要求精确等于 `theme.border`：`paint_frame` 是在根背景（`surface`）上做
  // **alpha 混合**的，线的落盘像素是 `D6DFEB` 而非 `D3DCE9`（实测 dump 确认），
  // 精确比色会永远扫不到（得到一个恒空的假绿）。
  const st::math::Color background =
      canvas.pixel_at_point(st::math::Point{a.x + a.width * 0.5f, y});
  const auto is_line = [&](float x) {
    const st::math::Color c = canvas.pixel_at_point(st::math::Point{x, y});
    return c.r != background.r || c.g != background.g || c.b != background.b;
  };

  // 在**整个分栏宽度**内找线——这一点至关重要：缺陷的表现是「线偏到编辑器
  // 面板内部」，若只扫交界附近，**线不在范围内就扫出空集**，被前置断言一挡，
  // 测试反而“通过”（实测踩到：把修复回退后测试仍绿，是个假绿）。
  // 步长必须**细于线宽**（常态 1px 发丝线）——用 2.0 会整条跳过。
  //
  // 两点面板本身是纯色（无背景、无边界），所以除线之外不会有其他非背景像素：
  // `inked` 只可能来自分隔线。
  std::vector<float> inked;
  for (float x = whole.x; x <= whole.right(); x += 0.25f) {
    if (is_line(x)) inked.push_back(x);
  }
  ST_REQUIRE(!inked.empty());   // 分隔线必须被画出来

  // 把非背景像素**聚类**（沿 x 连续 = 同一段）。一条分隔线是一段；把
  // `front/back` 直接当端点是不行的——画面里可能还有别的非背景像素
  // （实测有一个在 x≈520 处，会让端点跨到那里、中心值彻底失真）。
  struct Span {
    float first;
    float last;
  };
  std::vector<Span> spans;
  for (float x : inked) {
    if (!spans.empty() && x - spans.back().last <= 0.76f) {
      spans.back().last = x;
    } else {
      spans.push_back(Span{x, x});
    }
  }
  // 分隔线 = 落在两面板交界带内的那一段
  const Span* divider = nullptr;
  for (const Span& span : spans) {
    if (span.first >= a.right() - 1.5f && span.last <= b.x + 1.5f) divider = &span;
  }
  ST_REQUIRE(divider != nullptr);   // 交界带里必须有一段 = 分隔线

  // ① 线的中心**必须**等于 `handle_rect().center().x`（同一几何，不容偏移）。
  // 这一条是**主断言**：偏移一个 `bounds_.x` 会立刻被它抓到。
  const float painted_center = (divider->first + divider->last) * 0.5f;
  ST_CHECK_NEAR(painted_center, split->handle_rect().center().x, 1.0f);
  // ② 线必须落在两面板交界区（首面板右缘 .. 次面板左缘）
  ST_CHECK(painted_center >= a.right() - 0.5f);
  ST_CHECK(painted_center <= b.x + 0.5f);
  // ③ 首面板内部与次面板内部都不得出现分隔线
  ST_CHECK(!is_line(a.x + a.width * 0.5f));
  ST_CHECK(!is_line(b.x + b.width * 0.5f));
}
