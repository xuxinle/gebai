/// 叠加层排布形态测试（`OverlayLayout`）。
///
/// 模态浮层（对话框/命令面板）需要「遮罩铺满视口 + 居中卡片」的形态——此前协议里
/// 没有这个落点，`Dialog` 被迫要求调用方注入 `set_viewport_rect`，每个应用重复造。
/// 现在 `add_overlay(el, FillViewport)` 直接分到全视口矩形，组件在 arrange 里自定位。

#include "st/test/test.hpp"

#include <memory>
#include <string>

#include "st/ui/components/overlay.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Dialog;
using st::ui::Toast;
using st::ui::UiRoot;

}  // namespace

ST_TEST(dialog_fill_viewport_covers_mask_and_centers_card) {
  UiRoot root;
  root.set_viewport(st::math::Size{1000.0f, 700.0f});
  root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));

  auto dialog = std::make_unique<Dialog>("确认", "删除这个文件？");
  dialog->set_actions({"取消", "删除"});
  Dialog* ptr = dialog.get();
  root.add_overlay(std::move(dialog), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);

  // 未注入 viewport_rect：遮罩已铺满整个视口（bounds = 视口矩形）
  const st::math::Rect bounds = ptr->bounds();
  ST_CHECK_EQ(bounds.width, 1000.0f);
  ST_CHECK_EQ(bounds.height, 700.0f);
  ST_CHECK_EQ(bounds.x, 0.0f);

  // 卡片：居中且在视口内
  const st::math::Rect card = ptr->card_rect();
  ST_CHECK(card.width > 0.0f);
  ST_CHECK(card.height > 0.0f);
  const float center_delta = (card.x + card.width * 0.5f) - (bounds.x + bounds.width * 0.5f);
  ST_CHECK(center_delta < 1.0f && center_delta > -1.0f);
  ST_CHECK(card.y > bounds.y);
  ST_CHECK(card.bottom() < bounds.bottom());

  // 视口变化后重排：遮罩跟随（不再需要调用方重新注入）
  root.set_viewport(st::math::Size{640.0f, 480.0f});
  root.layout(true);
  ST_CHECK_EQ(ptr->bounds().width, 640.0f);
  ST_CHECK_EQ(ptr->bounds().height, 480.0f);
  ST_CHECK(ptr->card_rect().width > 0.0f);
}

ST_TEST(toast_fill_viewport_bottom_centered) {
  UiRoot root;
  root.set_viewport(st::math::Size{1000.0f, 700.0f});
  root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));

  auto toast = Toast::make("已保存", st::ui::Tone::Success);
  Toast* ptr = toast.get();
  root.add_overlay(std::move(toast), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);

  // 底部居中：右下角留 kBottomMargin
  const st::math::Rect bounds = ptr->bounds();
  ST_CHECK(bounds.width < 1000.0f);  // 保持胶囊宽度，不铺满
  ST_CHECK_EQ(bounds.height, 40.0f);
  ST_CHECK(bounds.bottom() <= 700.0f);
  ST_CHECK(bounds.bottom() >= 700.0f - 40.0f - 1.0f);  // 贴底（含留白）
  const float center_delta = (bounds.x + bounds.width * 0.5f) - 500.0f;
  ST_CHECK(center_delta < 1.0f && center_delta > -1.0f);
}

ST_TEST(overlay_stack_layout_unchanged) {
  UiRoot root;
  root.set_viewport(st::math::Size{1000.0f, 700.0f});
  root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));

  // 默认 Stack：两个 Toast 自上而下堆叠（历史行为），不铺满
  auto first = Toast::make("第一条", st::ui::Tone::Default);
  Toast* first_ptr = first.get();
  root.add_overlay(std::move(first), UiRoot::OverlayLayout::Stack);
  auto second = Toast::make("第二条", st::ui::Tone::Default);
  Toast* second_ptr = second.get();
  root.add_overlay(std::move(second));
  root.layout(true);

  ST_CHECK_EQ(root.overlay_count(), std::size_t{2});
  ST_CHECK(root.overlay_layout(first_ptr) == UiRoot::OverlayLayout::Stack);
  ST_CHECK(root.overlay_layout(second_ptr) == UiRoot::OverlayLayout::Stack);
  ST_CHECK(first_ptr->bounds().height <= 41.0f);   // 自身高度，不是视口高
  ST_CHECK(second_ptr->bounds().y >= first_ptr->bounds().bottom() - 1.0f);  // 堆叠在下方

  // remove_overlay 后形态表同步：剩余浮层的形态可查
  root.remove_overlay(first_ptr);
  ST_CHECK_EQ(root.overlay_count(), std::size_t{1});
  ST_CHECK(root.overlay_layout(second_ptr) == UiRoot::OverlayLayout::Stack);
  root.clear_overlays();
  ST_CHECK_EQ(root.overlay_count(), std::size_t{0});
}
