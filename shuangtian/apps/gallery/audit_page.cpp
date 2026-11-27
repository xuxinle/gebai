/// 「自检台」页：把框架的运行时不变式变成界面上的 PASS / FAIL 表。
///
/// 为什么单独一页而不是塞进「组件」页：这一页的读者是**改框架的人**（智能体/开发者），
/// 不是来找控件的用户。两者混在一页会让"我要看按钮长什么样"和"框架健康吗"互相干扰。
///
/// 分工（依赖方向）：页面不调检查、不知道真值树——它只渲染应用交回来的 `AuditReport`。
/// 渲染逻辑在这里（`fill_audit_view`），因为它同时服务两个时机：建页时一次、每次跑完再调。

#include <format>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "audit.hpp"
#include "pages.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/button.hpp"
#include "st/ui/components/card.hpp"
#include "st/ui/components/heading.hpp"
#include "st/ui/components/key_value_row.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/text.hpp"

namespace gallery {
namespace {

using st::math::Insets;
using st::ui::Button;
using st::ui::Card;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::Heading;
using st::ui::KeyValueRow;
using st::ui::Panel;
using st::ui::Table;
using st::ui::TableColumn;
using st::ui::Text;
using st::ui::TextAlign;
using st::ui::Tone;

[[nodiscard]] auto row_tone(AuditStatus status) -> Table::CellTone {
  switch (status) {
    case AuditStatus::Pass: return Table::CellTone::Accent;
    case AuditStatus::Warn: return Table::CellTone::Danger;
    case AuditStatus::Fail: return Table::CellTone::Danger;
    case AuditStatus::Info: return Table::CellTone::Muted;
  }
  return Table::CellTone::Muted;
}

[[nodiscard]] auto text_tone(AuditStatus status) -> Tone {
  switch (status) {
    case AuditStatus::Pass: return Tone::Success;
    case AuditStatus::Warn: return Tone::Warning;
    case AuditStatus::Fail: return Tone::Danger;
    case AuditStatus::Info: return Tone::Muted;
  }
  return Tone::Muted;
}

/// 一行摘要（页面顶部与状态栏共用同一份格式化——两处各写一遍必然不一致）。
[[nodiscard]] auto summary_line(const AuditReport& report) -> std::string {
  return std::format("PASS {} · WARN {} · FAIL {} · {:.1f}ms · 第 {} 帧", report.pass_count,
                     report.warn_count, report.fail_count, report.total_ms, report.frames);
}

}  // namespace

void fill_audit_view(Element& table_element, Element& details_element, const AuditReport& report) {
  auto* table = dynamic_cast<Table*>(&table_element);
  if (table != nullptr) {
    table->clear_rows();
    for (const AuditCheck& check : report.checks) {
      table->add_row({check.title, audit_status_label(check.status), check.summary,
                      std::format("{:.2f} ms", check.elapsed_ms)});
      const std::size_t row = table->row_count() - 1;
      table->set_cell_tone(row, 1U, row_tone(check.status));
    }
    // 默认选中第一项：进页就能看到明细，不必先点一下
    if (!report.checks.empty()) table->set_selected_row(0U);
  }
  // 明细：先铺全部检查的"状态 + 标题 + 一行结论"，再把每项的明细缩进列出。
  // 为什么全铺而不是只铺选中项：质检时要**一眼看全**有哪几项不达标，
  // 点行切换只是"看某一条的细节"的二段动作。
  details_element.clear_children();
  if (report.checks.empty()) {
    auto empty = std::make_unique<Text>("（还没跑过自检：点上面的「跑一次自检」）");
    empty->set_tone(Tone::Faint);
    empty->set_scale(Text::Scale::Xs);
    details_element.add_child(std::move(empty));
    return;
  }
  for (const AuditCheck& check : report.checks) {
    auto head = std::make_unique<Text>(
        std::format("[{}] {} — {}", audit_status_label(check.status), check.title, check.summary));
    head->set_tone(text_tone(check.status));
    head->set_scale(Text::Scale::Xs);
    head->set_weight(st::ui::FontWeight::SemiBold);
    details_element.add_child(std::move(head));
    for (const std::string& line : check.details) {
      auto item = std::make_unique<Text>("  · " + line);
      item->set_tone(Tone::Muted);
      item->set_scale(Text::Scale::Xs);
      item->style().padding = Insets{0.0f, 0.0f, 0.0f, 16.0f};
      details_element.add_child(std::move(item));
    }
  }
  details_element.mark_layout_dirty();
}

/// 构建自检台页。
///
/// 交互面（全部有显式 id，控制通道可驱动）：
/// - `btn-audit-run`：跑一次自检；
/// - `audit-summary`：一行摘要；
/// - `audit-table`：结论表（检查项 / 状态 / 结论 / 耗时）；
/// - `audit-details`：明细（全部检查逐条列出，不达标项点名）。
[[nodiscard]] auto build_audit_page(const PageHooks& hooks) -> std::unique_ptr<Panel> {
  auto page = std::make_unique<Panel>(FlexDirection::Column);
  page->set_id("page-audit");
  page->style().padding = Insets::all(24.0f);
  page->style().gap = 16.0f;
  page->add_child(std::make_unique<Heading>("自检台", 2));
  auto sub = std::make_unique<Text>(
      "框架的运行时不变式：注册表对账 · 真值树 · 属性面 · 主题对比度 · 布局整像素 · "
      "图标覆盖 · DPI 与尺寸 · 元素身份。任一项 FAIL 都是真缺陷，明细里点名是谁。");
  sub->set_tone(Tone::Muted);
  page->add_child(std::move(sub));

  // —— 运行条 ——
  auto run_card = std::make_unique<Card>(16.0f);
  run_card->set_id("card-audit-run");
  run_card->style().direction = FlexDirection::Column;
  run_card->style().gap = 10.0f;
  auto run_row = std::make_unique<Panel>(FlexDirection::Row);
  run_row->style().gap = 10.0f;
  run_row->style().align_items = st::ui::Align::Center;
  auto run_button =
      std::make_unique<Button>("跑一次自检", Button::Variant::Primary, Button::Size::Small);
  run_button->set_id("btn-audit-run");
  run_button->set_icon("refresh");
  run_row->add_child(std::move(run_button));
  auto summary_text = std::make_unique<Text>("尚未运行");
  summary_text->set_id("audit-summary");
  summary_text->set_tone(Tone::Muted);
  summary_text->set_scale(Text::Scale::Xs);
  run_row->add_child(std::move(summary_text));
  run_card->add_child(std::move(run_row));
  auto note = std::make_unique<Text>(
      "自检既能在这里点按钮跑，也能经控制通道 invoke 同一个按钮；应用启动时也会跑一次"
      "（结论同时打在启动日志里）。");
  note->set_tone(Tone::Faint);
  note->set_scale(Text::Scale::Xs);
  run_card->add_child(std::move(note));
  page->add_child(std::move(run_card));

  // —— 结论表 ——
  std::vector<TableColumn> columns{
      TableColumn{.name = "检查项", .width = 150.0f},
      TableColumn{.name = "状态", .width = 80.0f},
      TableColumn{.name = "结论"},
      TableColumn{.name = "耗时", .width = 84.0f, .align = TextAlign::End},
  };
  auto table = std::make_unique<Table>(std::move(columns));
  table->set_id("audit-table");
  table->style().height = 288.0f;
  table->set_row_height(30.0f);
  table->set_zebra(true);
  page->add_child(std::move(table));

  // —— 明细 ——
  auto details_card = std::make_unique<Card>(16.0f);
  details_card->set_id("card-audit-details");
  details_card->style().direction = FlexDirection::Column;
  details_card->style().gap = 6.0f;
  auto details_title = std::make_unique<Text>("明细");
  details_title->set_weight(st::ui::FontWeight::SemiBold);
  details_card->add_child(std::move(details_title));
  auto details_host = std::make_unique<Panel>(FlexDirection::Column);
  details_host->set_id("audit-details");
  details_host->style().gap = 3.0f;
  details_card->add_child(std::move(details_host));
  page->add_child(std::move(details_card));

  // —— 判据出处（质检场地的可追溯性）——
  auto about_card = std::make_unique<Card>(16.0f);
  about_card->set_id("card-audit-about");
  about_card->style().direction = FlexDirection::Column;
  about_card->style().gap = 4.0f;
  auto about_title = std::make_unique<Text>("这些判据从哪来");
  about_title->set_weight(st::ui::FontWeight::SemiBold);
  about_card->add_child(std::move(about_title));
  about_card->add_child(std::make_unique<KeyValueRow>(
      "注册表对账", "tests/gallery_component_parity_test.cpp 的运行时版（同一条差集）"));
  about_card->add_child(std::make_unique<KeyValueRow>(
      "真值树", "main.cpp 的 audit_gallery_tree——启动日志那份的结构化版"));
  about_card->add_child(std::make_unique<KeyValueRow>(
      "属性面", "property_names / readonly_property_names / property_schema 三表一致"));
  about_card->add_child(std::make_unique<KeyValueRow>(
      "整像素", "tests/ui_grow_pixel_test.cpp（grow 均分必须落整像素）"));
  about_card->add_child(std::make_unique<KeyValueRow>(
      "图标覆盖", "icon.cpp 的内置表：写错名字不报错也不占位，只能靠这条查"));
  about_card->add_child(std::make_unique<KeyValueRow>(
      "DPI", "physical == logical × device_scale；比值恰等于 scale 就是尺寸不同步"));
  page->add_child(std::move(about_card));

  // —— 接线 ——
  //
  // 页面元素在建页时由 `main.cpp` 记下（`page-audit` 之下按 id 查），所以这里
  // 只把"跑一次"接到 hooks；跑完由应用把它自己的报告交给 `fill_audit_view`。
  // 这样一来：报告的真源只有一份（应用侧），表格/明细/摘要三处渲染全部由它派生。
  // hooks 在 wire_audit_page 里用（建页时页面还没记账，接不了按钮）。
  (void)hooks;
  return page;
}

/// 把「跑一次自检」按钮的行为接上（建页之后由 `main.cpp` 调用）。
///
/// 为何不写在 `build_audit_page` 里：按钮回调需要页面自己的表格/明细元素，
/// 而建页时页面尚未挂树（`find` 在子树内查找，游离页面也能找到，但此时
/// 页面还未被应用记账）。接在“挂完再 wire”的位置，语义最清楚。
namespace {

/// 在子树里按 id 找元素（Element 没有 find；UiRoot::find 是全树查找，在应用侧）。
[[nodiscard]] auto find_in(Element& node, std::string_view id) -> Element* {
  if (node.id() == id) return &node;
  for (const auto& child : node.children()) {
    if (child == nullptr) continue;
    if (Element* found = find_in(*child, id); found != nullptr) return found;
  }
  return nullptr;
}

}  // namespace

void wire_audit_page(Panel& page, const PageHooks& hooks) {
  auto* run = dynamic_cast<Button*>(find_in(page, "btn-audit-run"));
  auto* summary = dynamic_cast<Text*>(find_in(page, "audit-summary"));
  auto* table = find_in(page, "audit-table");
  auto* details = find_in(page, "audit-details");
  if (run == nullptr || summary == nullptr || table == nullptr || details == nullptr) return;
  auto* summary_ptr = summary;
  auto* table_ptr = table;
  auto* details_ptr = details;
  run->on_click = [hooks, summary_ptr, table_ptr, details_ptr]() {
    if (!hooks.run_self_audit) {
      summary_ptr->set_content("未接自检通道（应用没提供 run_self_audit）");
      return;
    }
    const AuditReport report = hooks.run_self_audit();
    summary_ptr->set_content(summary_line(report));
    fill_audit_view(*table_ptr, *details_ptr, report);
    if (hooks.set_status) hooks.set_status("自检完成：" + summary_line(report));
  };
  // 建页后立刻跑一次：进页就有结论，不必先点一下。
  run->on_click();
}

}  // namespace gallery
