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
#include "st/ui/style.hpp"
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

/// 探针元素：记下 `measure` 收到的**约束宽**（不画画、不参与布局）。
///
/// 为何用探针而不是看结果尺寸：本用例要钉的是「SplitView 给子面板传了什么约束」——
/// 内容型组件（如 MarkdownView）拿到 unbounded 时会落回自己的兑底常量
/// （排版宽卡在 608），而**它的 measured 尺寸看上去仍然“合理”**——
/// 只看尺寸会漏掉这个缺陷（实测确实漏了一轮）。
struct ConstraintProbe : Element {
  float seen_max_width{-1.0f};
  float seen_available_width{-1.0f};
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ConstraintProbe"; }
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override {
    (void)context;
    seen_max_width = constraints.max_width;
    seen_available_width = constraints.available_width;
    // 报一个小的自然尺寸：让 SplitView 的无界探测路径不会把自身撑到无边。
    measured_ = st::math::Size{40.0f, 20.0f};
  }
  void arrange(const st::ui::RenderContext& context, Rect rect) override {
    (void)context;
    bounds_ = rect;
  }
};

}  // namespace

ST_TEST(ui_split_view_children_get_bounded_width_inside_row) {
  // 回归：分栏处在**带固定宽兄弟的 Row 容器**里时，子面板必须拿到
  // **真实可用宽**（不是父级 Row 给的粗口径）。
  //
  // 背景（实测缺陷，用户报“Markdown 选中背景错位、没随窗口适应”）：
  // Row 给子节点的 `max_width` 是 `kUnbounded`（Flex 的 shrink-to-fit 语义），
  // 真可用宽在 `available_width`——但后者**不扣同排固定宽兄弟**（如活动栏 44px）。
  // 旧实现直接取 `max_width`：分栏以为自己在无界空间，退化成“两侧自然尺寸之和”，
  // 据此给子面板分的宽度偏离真实值；而 `arrange` 用的是真实 rect——
  //  **排版宽 ≠ 控件宽**，表现为文字按错误宽折行、选中高亮与文字错位。
  //
  // 判据：子面板的 `bounds().width`（arrange 给的真实几何）必须与它收到的
  // **排布约定宽**一致；这里用探针同时记下 measure 约束与最终 bounds，
  // 两者相等才算“排版与控件同口径”。
  constexpr float kViewportWidth = 800.0f;
  constexpr float kSidebarWidth = 44.0f;   // 同排固定兄弟（模拟 gbcode 的活动栏）
  constexpr float kHandle = 8.0f;
  constexpr float kRatio = 0.25f;

  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{kViewportWidth, 400.0f});

  auto row = std::make_unique<Panel>();
  row->set_id("row");
  row->style().direction = st::ui::FlexDirection::Row;
  row->style().grow = true;

  // 固定宽的兄弟（不 grow）：分栏必须把它的宽度扣掉。
  auto bar = std::make_unique<Panel>();
  bar->set_id("bar");
  bar->style().width = kSidebarWidth;
  row->add_child(std::move(bar));

  auto split_view = std::make_unique<SplitView>(SplitView::Orientation::Horizontal);
  split_view->set_id("split");
  split_view->set_ratio(kRatio, false);
  auto* first = split_view->add_child(std::make_unique<ConstraintProbe>());
  auto* second = split_view->add_child(std::make_unique<ConstraintProbe>());
  auto* probe_first = dynamic_cast<ConstraintProbe*>(first);
  auto* probe_second = dynamic_cast<ConstraintProbe*>(second);
  ST_REQUIRE(probe_first != nullptr);
  ST_REQUIRE(probe_second != nullptr);
  second->style().grow = true;
  split_view->style().grow = true;
  SplitView* split = split_view.get();
  row->add_child(std::move(split_view));
  root.set_content(std::move(row));
  root.layout(true);

  const Rect whole = split->bounds();
  // 分栏真实宽 = 视口宽 − 固定兄弟（活动栏）宽。
  const float expected_split = kViewportWidth - kSidebarWidth;
  ST_CHECK_NEAR(whole.width, expected_split, 0.01f);

  const float usable = expected_split - kHandle;
  const float expected_first = usable * kRatio;
  const float expected_second = usable - expected_first;

  // ① 给子面板的 measure 约束 = 真实分栏几何（不是按视口宽算的偏大值）
  ST_CHECK_NEAR(probe_first->seen_max_width, expected_first, 0.01f);
  ST_CHECK_NEAR(probe_second->seen_max_width, expected_second, 0.01f);
  // ② arrange 给的 bounds 与之一致（排版口径 == 控件口径）
  ST_CHECK_NEAR(first->bounds().width, expected_first, 0.01f);
  ST_CHECK_NEAR(second->bounds().width, expected_second, 0.01f);

  root.set_content(nullptr);
}

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

ST_TEST(ui_split_view_reset_and_activate_honor_default_ratio) {
  // 归位目标不是写死的 0.5：应用可以声明自己的默认比例（gbcode 侧栏 0.2、
  // 底部面板 2/3）——“点/双击分割线回到默认布局”是用户预期的手势语义。
  SplitFixture fixture(SplitView::Orientation::Vertical, 0.55f, st::math::Size{800.0f, 600.0f});
  auto* split = fixture.split;
  ST_REQUIRE(split != nullptr);

  // 未设置时仍是 0.5（既有行为不变）
  ST_CHECK_NEAR(split->default_ratio(), 0.5f, 0.0001f);
  split->activate();
  ST_CHECK_NEAR(split->ratio(), 0.5f, 0.0001f);

  // 声明默认 0.2：单击/双击与 reset 动作都回 0.2（不是 0.5）
  split->set_default_ratio(0.2f);
  ST_CHECK_NEAR(split->ratio(), 0.5f, 0.0001f);   // 只记目标，不动当前值
  split->set_ratio(0.7f, false);
  split->activate();
  ST_CHECK_NEAR(split->ratio(), 0.2f, 0.0001f);
  split->set_ratio(0.7f, false);
  ST_CHECK(split->invoke_action("reset", {}));
  ST_CHECK_NEAR(split->ratio(), 0.2f, 0.0001f);

  // 默认值同样受夹取约束（不能给出一个两侧倒挂的比例）
  split->set_min_ratio(0.1f);
  split->set_default_ratio(0.0f);
  ST_CHECK_NEAR(split->default_ratio(), 0.1f, 0.0001f);
  split->activate();
  ST_CHECK_NEAR(split->ratio(), 0.1f, 0.0001f);

  // 属性面：可读可写（与 `ratio`/`min_ratio` 同一契约）
  ST_CHECK(split->set_property("default_ratio", "1"));
  ST_CHECK_NEAR(split->default_ratio(), 0.9f, 0.0001f);   // 夹到 1 - min_ratio
  ST_CHECK(!split->set_property("default_ratio", "abc"));
  ST_CHECK(split->get_property("default_ratio").has_value());
}

ST_TEST(ui_split_view_drag_release_click_does_not_reset_ratio) {
  // 回归：拖动后的松手**不是点击**（Win32 后端在 LBUTTONUP 后无条件补发 Click，
  // 见 platform_win32.cpp 的 WM_LBUTTONUP）。UiRoot 的 Click 兑底 activate() 若不
  // 区分拖动，拖 SplitView 手柄松手就会触发「双击归位」语义 → 比例弹回 0.5
  //（实测：用户拖侧栏/底部面板松手就回弹到一半宽度）。
  SplitFixture fixture(SplitView::Orientation::Horizontal, 0.3f, st::math::Size{800.0f, 400.0f});
  auto& root = fixture.root;
  auto* split = fixture.split;
  ST_REQUIRE(split != nullptr);

  const Rect whole = split->bounds();
  const Rect handle = fixture.handle();
  const float mid_y = whole.center().y;
  // 拖到 0.55：down → move（位移远超阈值）→ up → **补发 Click**（复刻 Win32 序列）
  ST_CHECK(dispatch_kind(root, st::ui::EventKind::MouseDown,
                         Point{handle.center().x, mid_y}));
  const float target_x = whole.x + (whole.width - split->handle_size()) * 0.55f +
                         split->handle_size() * 0.5f;
  ST_CHECK(dispatch_kind(root, st::ui::EventKind::MouseMove, Point{target_x, mid_y}));
  ST_CHECK_NEAR(split->ratio(), 0.55f, 0.01f);
  (void)dispatch_kind(root, st::ui::EventKind::MouseUp, Point{target_x, mid_y});
  (void)dispatch_kind(root, st::ui::EventKind::Click, Point{target_x, mid_y});   // ← 修复前这里触发 activate
  ST_CHECK_NEAR(split->ratio(), 0.55f, 0.01f);   // 不能弹回 0.5

  // 对照：原地微动（位移 < 阈值 4px）的 Click 仍是点击 → 兑底 activate 照常工作
  //（SplitView::activate = 归位到 `default_ratio`（默认 0.5）；这证明修复没有误伤按钮类点击路径）。
  ST_CHECK(dispatch_kind(root, st::ui::EventKind::MouseDown, Point{target_x, mid_y}));
  (void)dispatch_kind(root, st::ui::EventKind::MouseMove, Point{target_x + 2.0f, mid_y});
  (void)dispatch_kind(root, st::ui::EventKind::MouseUp, Point{target_x + 2.0f, mid_y});
  (void)dispatch_kind(root, st::ui::EventKind::Click, Point{target_x + 2.0f, mid_y});
  ST_CHECK_NEAR(split->ratio(), 0.5f, 0.01f);   // 微动点击 → activate 归位仍生效
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

ST_TEST(ui_split_view_measures_children_with_bounded_width) {
  // 子面板**必须**收到有限的宽约束（而不仅是有个看上去合理的尺寸）。
  //
  // 为何单独钉这一条：旧实现只有一条 `loose`（全 unbounded）分支——有界时
  // 也拿它去 measure 子面板。内容型组件拿到 unbounded 时会落回**自己的兑底
  // 常量**（MarkdownView = `kDefaultViewWidth` 640），于是：
  //   · 面板拉伸/拖分割条时文字不重排（实测：侧栏 0.22→0.40，控件宽
  //     957.8→736.8 而 MarkdownView 的排版宽恒为 608）；
  //   · 而 `arrange` 阶段又给了正确 rect——arrange **不重 measure**，
  //     所以错就永远错。
  //
  // 判据用探针记下的**约束值**而不是子元素尺寸：只看尺寸会漏
  //（探针报的自然尺寸是固定的，尺寸看上去永远“合理”）。
  constexpr st::math::Size kViewport{800.0f, 400.0f};
  st::ui::UiRoot root;
  root.set_viewport(kViewport);
  auto split_view = std::make_unique<SplitView>(SplitView::Orientation::Horizontal);
  split_view->set_id("split");
  split_view->set_ratio(0.3f, false);
  auto first = std::make_unique<ConstraintProbe>();
  auto second = std::make_unique<ConstraintProbe>();
  ConstraintProbe* probe_first = first.get();
  ConstraintProbe* probe_second = second.get();
  split_view->set_first(std::move(first));
  split_view->set_second(std::move(second));
  SplitView* split = split_view.get();
  root.set_content(std::move(split_view));
  root.layout(true);

  ST_REQUIRE(split != nullptr);
  const float full = split->bounds().width;
  ST_REQUIRE(full > 0.0f);
  const float handle = split->handle_rect().width;
  const float usable = full - handle;

  // ① 两侧都拿到**有限**约束（不再是 `kUnbounded`）。
  ST_CHECK(probe_first->seen_max_width < st::ui::kUnbounded);
  ST_CHECK(probe_second->seen_max_width < st::ui::kUnbounded);
  // ② 且与 `handle_rect` 的几何一致：主轴按 ratio 分
  //（与 `layout_children` 同一份几何——两处不一致会表现为“拖了不重排”）。
  ST_CHECK_NEAR(probe_first->seen_max_width, usable * 0.3f, 1.0f);
  ST_CHECK_NEAR(probe_second->seen_max_width, usable * 0.7f, 1.0f);
  // ③ 交叉轴拿满（高度不受 ratio 影响）
  ST_CHECK_NEAR(probe_first->seen_available_width, probe_first->seen_max_width, 0.5f);
  root.set_content(nullptr);
}

ST_TEST(ui_split_view_child_constraints_follow_ratio) {
  // 拖比例后，子面板收到的约束宽**跟着变**（这是“面板跟随”的根）。
  //
  // 与上一条互补：上一条钉“约束是有限的”，这一条钉“约束随几何变”
  //（只钉前者时，写死一个常量也能过——那是另一种形态的假绿）。
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{800.0f, 400.0f});
  auto split_view = std::make_unique<SplitView>(SplitView::Orientation::Horizontal);
  split_view->set_ratio(0.3f, false);
  auto first = std::make_unique<ConstraintProbe>();
  auto second = std::make_unique<ConstraintProbe>();
  ConstraintProbe* probe_second = second.get();
  split_view->set_first(std::move(first));
  split_view->set_second(std::move(second));
  SplitView* split = split_view.get();
  root.set_content(std::move(split_view));
  root.layout(true);

  const float before = probe_second->seen_max_width;
  ST_REQUIRE(before > 0.0f);
  split->set_ratio(0.6f, false);
  root.layout(true);
  const float after = probe_second->seen_max_width;
  // 首面板占比变大 → 次面板收到的约束宽**变小**。
  ST_CHECK(before > after);
  ST_CHECK_NEAR(after, (split->bounds().width - split->handle_rect().width) * 0.4f, 1.0f);
  root.set_content(nullptr);
}
