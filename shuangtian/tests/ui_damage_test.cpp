/// 增量重绘（损坏区）测试。
///
/// 核心不变式：**局部重绘的结果必须与全量重绘逐像素一致**。
/// 这不是"锦上添花"的断言——增量重绘的每一个设计决策（清损坏区、按裁剪域剔除、
/// z 序靠整树遍历保证、布局变更保守整帧）都可以被它一票否决：
///
/// 1. 改一个纯绘制元素（颜色）→ 下一帧必须走**局部**，且像素与全量重绘一致；
/// 2. 元素重叠时，改**下层**元素 → 上层元素在重叠区不能被盖掉（z 序）；
/// 3. 需要重排的变更（文本变长）→ 保守**整帧**（重排会挪动兄弟，波及范围难界定）;
/// 4. `request_animation`（动画续帧）→ 上报损坏区，动画帧走局部。
#include <cstring>
#include <memory>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::Element;
using st::ui::Panel;
using st::ui::UiRoot;

inline constexpr int kWidth = 480;
inline constexpr int kHeight = 360;

/// 固定几何的测试盒子：`arrange` 忽略父容器给的矩形（便于构造精确重叠）。
class FixedBox : public Element {
 public:
  FixedBox(std::string id, Rect box, Color color) : box_(box) {
    set_id(std::move(id));
    style().background = color;
  }
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "FixedBox"; }
  void arrange(const st::ui::RenderContext&, Rect) override {
    bounds_ = box_;
    layout_dirty_ = false;
  }

 private:
  Rect box_{};
};

/// 拷一份画布像素（局部帧要画在"上一帧"之上，对照则从同一起点出发）。
void copy_pixels(const Canvas& from, Canvas& to) {
  const std::span<const std::uint32_t> source = from.pixels();
  const std::span<std::uint32_t> target = to.pixels();
  ST_CHECK_EQ(source.size(), target.size());
  if (source.size() == target.size()) {
    std::memcpy(target.data(), source.data(), source.size() * sizeof(std::uint32_t));
  }
}

void expect_same_pixels(const Canvas& left, const Canvas& right) {
  const std::span<const std::uint32_t> a = left.pixels();
  const std::span<const std::uint32_t> b = right.pixels();
  ST_CHECK_EQ(a.size(), b.size());
  if (a.size() != b.size()) return;
  std::size_t differing = 0;
  std::size_t first = 0;
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (a[index] != b[index]) {
      if (differing == 0) first = index;
      ++differing;
    }
  }
  if (differing != 0) {
    const auto x = static_cast<int>(first % static_cast<std::size_t>(kWidth));
    const auto y = static_cast<int>(first / static_cast<std::size_t>(kWidth));
    st::log::out("  首个差异像素 ({}, {}): {:08x} vs {:08x}（共 {} 处）\n", x, y, a[first], b[first],
              differing);
  }
  ST_CHECK_EQ(differing, 0);
}

}  // namespace

ST_TEST(damage_partial_repaint_is_pixel_identical_to_full) {
  UiRoot root;
  root.set_viewport({kWidth, kHeight});
  auto page = std::make_unique<Panel>();
  page->style().background = Color{0x10, 0x14, 0x18, 0xFF};
  // 两个重叠盒子：`under` 先画（下层），`over` 后画（上层）。
  const Color under_color{0xD0, 0x40, 0x40, 0xFF};
  const Color over_color{0x40, 0xC0, 0x70, 0xFF};
  auto* under = page->add_child(std::make_unique<FixedBox>("under", Rect{20, 30, 120, 80},
                                                           under_color));
  auto* over = page->add_child(
      std::make_unique<FixedBox>("over", Rect{80, 60, 120, 80}, over_color));
  root.set_content(std::move(page));

  Canvas live(kWidth, kHeight);
  root.paint_frame(live);  // 首帧：整帧
  ST_CHECK(!root.last_frame_partial());
  root.clear_dirty();

  // —— 改**下层**元素颜色（纯绘制变更）→ 期望局部重绘 ——
  const Color under_new{0x30, 0x70, 0xE0, 0xFF};
  under->style().background = under_new;
  under->mark_dirty();

  Canvas over_baseline(kWidth, kHeight);  // 对照用：同一"上一帧"起点
  copy_pixels(live, over_baseline);

  const bool partial = root.paint_frame(live);
  ST_CHECK(partial);  // 纯绘制变更必须走局部路径
  root.clear_dirty();

  // 对照：同一起点 + 强制整帧重绘（同一状态；mark_dirty_all 走保守全量）。
  root.mark_dirty_all();
  root.paint_frame(over_baseline);  // 整帧
  ST_CHECK(!root.last_frame_partial());
  root.clear_dirty();

  // 1) 局部帧与全量帧必须**逐像素一致**（含重叠区的 z 序）
  expect_same_pixels(live, over_baseline);

  // 2) 上层的重叠区不能被下层的新颜色盖掉（整帧对照已证；这里再做一个语义断言）
  const Color overlap = live.pixel_at_point(st::math::Point{140.0f, 100.0f});
  ST_CHECK_EQ(static_cast<int>(overlap.r), static_cast<int>(over_color.r));
  // 3) 下层自己（非重叠区）必须显示新颜色
  const Color under_only = live.pixel_at_point(st::math::Point{40.0f, 50.0f});
  ST_CHECK_EQ(static_cast<int>(under_only.b), static_cast<int>(under_new.b));
  (void)over;
}

ST_TEST(damage_layout_change_falls_back_to_full_frame) {
  UiRoot root;
  root.set_viewport({kWidth, kHeight});
  auto page = std::make_unique<Panel>();
  auto text_element = std::make_unique<st::ui::Text>("短文本");
  auto* text = text_element.get();
  page->add_child(std::move(text_element));
  root.set_content(std::move(page));

  Canvas canvas(kWidth, kHeight);
  root.paint_frame(canvas);
  root.clear_dirty();
  ST_CHECK(!root.last_frame_partial());

  // 文本变长：需要重排（会挪动兄弟）→ 必须回落到整帧（保守但正确）。
  text->set_content("一段长得多的文本，触发重新测量与排版");
  const bool partial = root.paint_frame(canvas);
  ST_CHECK(!partial);
  root.clear_dirty();
}

ST_TEST(damage_animation_request_paints_partially) {
  UiRoot root;
  root.set_viewport({kWidth, kHeight});
  auto page = std::make_unique<Panel>();
  auto* box = page->add_child(
      std::make_unique<FixedBox>("box", Rect{40, 40, 100, 60}, Color{0x88, 0x44, 0xCC, 0xFF}));
  root.set_content(std::move(page));

  Canvas canvas(kWidth, kHeight);
  root.paint_frame(canvas);
  root.clear_dirty();

  // 外部触发动画续帧（组件在绘制中也会调用同一 API）→ 损坏区应覆盖该元素。
  box->request_animation();
  const bool partial = root.paint_frame(canvas);
  ST_CHECK(partial);
  ST_CHECK_EQ(root.dirty_rect().x, 0);
  ST_CHECK(root.dirty_rect().width > 0);
  root.clear_dirty();
}

ST_TEST(damage_idle_frame_is_quiet) {
  UiRoot root;
  root.set_viewport({kWidth, kHeight});
  auto page = std::make_unique<Panel>();
  page->add_child(std::make_unique<FixedBox>("box", Rect{10, 10, 40, 40}, Color{0xFF, 0, 0, 0xFF}));
  root.set_content(std::move(page));

  Canvas canvas(kWidth, kHeight);
  root.paint_frame(canvas);
  root.clear_dirty();
  // 静止（无动画、无损坏）→ 不再需要下一帧。
  ST_CHECK(!root.needs_frame());
  ST_CHECK(!root.dirty());
}

ST_TEST(damage_steady_state_settles_into_partial_frames) {
  // 回归用例（开发循环实测发现）：**静止的界面每帧都在整帧重绘**。
  //
  // 现象：gbcode 里 `invoke` 的 p50 是 **15.9 ms**，而 `get` 只有 4.1 ms——
  // 差值恰好等于整帧绘制（paint 11.5 ms）+ 控制通道节拍（4.1 ms）。
  // 根因：`UiRoot::layout()` 跑完会 `pending_full_ = true`，而 `layout()` 的进入条件是
  // `dirty_ || tree_layout_dirty()`，其中 `tree_layout_dirty()` 读的是**根元素上的
  // 累积 layout 标记**——一旦树里有人置过 `mark_layout_dirty()` 而标记没被清，
  // 之后**每一帧**都重排 + 整帧重绘。
  //
  // 本用例钉住的不变式：静止状态下连续两帧，第二帧必须是**局部**且损坏区为空
  // （没有任何东西变 → 不该重画 1280×800）。
  UiRoot root;
  root.set_viewport({kWidth, kHeight});
  auto page = std::make_unique<Panel>();
  page->style().background = Color{0x10, 0x14, 0x18, 0xFF};
  auto* box = page->add_child(
      std::make_unique<FixedBox>("box", Rect{40, 40, 100, 60}, Color{0x88, 0x44, 0xCC, 0xFF}));
  root.set_content(std::move(page));

  Canvas canvas(kWidth, kHeight);
  root.paint_frame(canvas);   // 首帧：整帧（合理——还没有上一帧）
  root.clear_dirty();

  // 静止：没有任何变更 → 不该要求重排，也不该要求整帧
  ST_CHECK(!root.tree_layout_dirty());
  ST_CHECK(!root.needs_frame());

  // 一次真实但**纯绘制**的变更（只改颜色，不动几何）→ 必须走局部
  box->style().background = Color{0x30, 0x70, 0xE0, 0xFF};
  box->mark_dirty();
  const bool partial = root.paint_frame(canvas);
  ST_CHECK(partial);            // 局部：不是整帧
  ST_CHECK(root.last_frame_partial());
  root.clear_dirty();

  // 关键：**这一帧跑完，树不该又变成 layout dirty**——否则下一帧又要整帧
  ST_CHECK(!root.tree_layout_dirty());
  ST_CHECK(!root.needs_frame());
}

/// **移除子树后，腾出的那块必须被重绘**（局部帧与全量逐像素一致）。
///
/// 这条钉的是一个“属性面、语义树全对，只有像素能看出来”的缺陷：
/// `remove_child`/`clear_children` 只调 `mark_layout_dirty()`，而它**刻意不报 damage**
/// （理由见其实现处：重排会挑动兄弟，窄脏区盖不住）。那条理由对“位置变了”成立，
/// 但对“这里曾经有东西、现在没了”不成立——**没有任何存活元素**会为腾空区域负责，
/// 于是旧像素原封不动留在后备缓冲里。
///
/// 实测现象（dev-tools 切页签）：`tree` 里已经是新的一页，屏上还是上一页。
/// 当时的“修法”是给内容区铺一层不透明底色、再显式 `mark_dirty_all()`——
/// 那只是把缺陷推给“必须有东西盖着”，本用例钉住的是真正的修复。
///
/// 判据用**像素**而不是 `dirty_rect`：缺陷的现场就是“局部帧与全量帧不一致”。
ST_TEST(damage_removed_subtree_is_repainted) {
  UiRoot root;
  root.set_viewport({kWidth, kHeight});
  auto page = std::make_unique<Panel>();
  // 底色由页面自己铺（与真实场景一致：外壳有一层不透明底），
  // 这样“旧盒子的像素是否被清回去”能逐像素核对。
  page->style().background = Color{0x10, 0x14, 0x18, 0xFF};
  auto page_raw = page.get();
  auto old_box = std::make_unique<FixedBox>("old", Rect{40, 40, 160, 100},
                                           Color{0xD0, 0x40, 0x40, 0xFF});
  page->add_child(std::move(old_box));
  root.set_content(std::move(page));

  Canvas live(kWidth, kHeight);
  root.paint_frame(live);   // 首帧整帧
  root.clear_dirty();
  // 先确认旧盒子真的画上去了（否则下面“被清掉”无从谈起）。
  ST_CHECK_EQ(static_cast<int>(live.pixel_at_point(st::math::Point{100.0f, 80.0f}).r),
              static_cast<int>(0xD0));

  // 移除那个盒子（声明式场景里这是“旧页整棵子树被换掉”的最小形式）。
  (void)page_raw->remove_child(page_raw->child_at(0));

  Canvas baseline(kWidth, kHeight);
  copy_pixels(live, baseline);
  const bool partial = root.paint_frame(live);
  root.clear_dirty();
  ST_CHECK(partial);   // 纯移除：应走局部（若能整帧也对，但局部正确才是本用例的诉求）

  // 对照：同一起点 + 强制整帧
  root.mark_dirty_all();
  root.paint_frame(baseline);
  root.clear_dirty();

  // ① 局部帧必须把旧盒子的像素清回页面底色
  const Color after = live.pixel_at_point(st::math::Point{100.0f, 80.0f});
  ST_CHECK_EQ(static_cast<int>(after.r), static_cast<int>(0x10));
  // ② 且与全量重绘逐像素一致（这条是最强判据：任何残留都会在这里现形）
  expect_same_pixels(live, baseline);
}

/// `clear_children` 同理（整批移除：列表刷新/切页时最常见的那条路径）。
ST_TEST(damage_cleared_children_are_repainted) {
  UiRoot root;
  root.set_viewport({kWidth, kHeight});
  auto page = std::make_unique<Panel>();
  page->style().background = Color{0x10, 0x14, 0x18, 0xFF};
  auto page_raw = page.get();
  page->add_child(std::make_unique<FixedBox>("a", Rect{20, 20, 120, 60},
                                             Color{0xD0, 0x40, 0x40, 0xFF}));
  page->add_child(std::make_unique<FixedBox>("b", Rect{20, 120, 120, 60},
                                             Color{0x40, 0xC0, 0x70, 0xFF}));
  root.set_content(std::move(page));

  Canvas live(kWidth, kHeight);
  root.paint_frame(live);
  root.clear_dirty();
  ST_CHECK_EQ(static_cast<int>(live.pixel_at_point(st::math::Point{60.0f, 40.0f}).r),
              static_cast<int>(0xD0));
  ST_CHECK_EQ(static_cast<int>(live.pixel_at_point(st::math::Point{60.0f, 140.0f}).g),
              static_cast<int>(0xC0));

  page_raw->clear_children();

  Canvas baseline(kWidth, kHeight);
  copy_pixels(live, baseline);
  (void)root.paint_frame(live);
  root.clear_dirty();
  root.mark_dirty_all();
  root.paint_frame(baseline);
  root.clear_dirty();

  // 两块的像素都要回到页面底色
  ST_CHECK_EQ(static_cast<int>(live.pixel_at_point(st::math::Point{60.0f, 40.0f}).r),
              static_cast<int>(0x10));
  ST_CHECK_EQ(static_cast<int>(live.pixel_at_point(st::math::Point{60.0f, 140.0f}).r),
              static_cast<int>(0x10));
  expect_same_pixels(live, baseline);
}
