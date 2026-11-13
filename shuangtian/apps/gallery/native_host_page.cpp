/// @file
/// @brief gallery 的“原生嵌入”页实现（见 `native_host_page.hpp` 的契约说明）。

#include "native_host_page.hpp"

#include <format>
#include <string>
#include <utility>

#include "st/ui/components/button.hpp"
#include "st/ui/components/card.hpp"
#include "st/ui/components/heading.hpp"
#include "st/ui/components/key_value_row.hpp"
#include "st/ui/components/native_host.hpp"
#include "st/ui/components/panel.hpp"
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
using st::ui::NativeHost;
using st::ui::Panel;
using st::ui::Text;
using st::ui::Tone;

[[nodiscard]] auto make_page(std::string_view id, std::string_view title,
                             std::string_view subtitle) -> std::unique_ptr<Panel> {
  auto page = std::make_unique<Panel>(FlexDirection::Column);
  page->set_id(std::format("page-{}", id));
  page->style().padding = Insets::all(24.0f);
  page->style().gap = 16.0f;
  page->add_child(std::make_unique<Heading>(std::string(title), 2));
  auto sub = std::make_unique<Text>(std::string(subtitle));
  sub->set_tone(Tone::Muted);
  page->add_child(std::move(sub));
  return page;
}

[[nodiscard]] auto make_card(std::string_view id, std::string_view title)
    -> std::unique_ptr<Card> {
  auto card = std::make_unique<Card>(18.0f);
  card->set_id(std::string(id));
  card->style().direction = FlexDirection::Column;
  card->style().gap = 12.0f;
  if (!title.empty()) {
    auto heading = std::make_unique<Text>(std::string(title));
    heading->set_weight(st::ui::FontWeight::SemiBold);
    card->add_child(std::move(heading));
  }
  return card;
}

[[nodiscard]] auto make_row(bool grow = false) -> std::unique_ptr<Panel> {
  auto row = std::make_unique<Panel>(FlexDirection::Row);
  row->style().gap = 8.0f;
  row->style().align_items = st::ui::Align::Center;
  row->style().grow = grow;
  return row;
}

}  // namespace

auto build_native_host_page(NativeHostSource* source) -> std::unique_ptr<Element> {
  auto page = make_page("native-host", "原生嵌入",
                        "把外部程序的窗口嵌进界面：不进 framebuffer，capture 拍不到它");

  // ⚠ 这里**不能**先问 `source->supported()` 再据以禁用按钮：页面是在
  // `app.start()` **之前**装配的，那时后端还没建（`child_view_supported()` 必然
  // 是 false）。能把外部窗口嵌进来与否只能在**用的时候**判定——所以按钮恒可点，
  // 不支持的真实结果由点击后状态行如实报出。

  // —— 它是什么 ——
  auto about = make_card("card-nh-about", "它是什么");
  auto about_text = std::make_unique<Text>(
      "框架不实现浏览器引擎。NativeHost 是逃生门：给外部窗口一块正确的屏幕矩形、"
      "把它裁在自己范围内、销毁时还原成独立窗口。界面其余部分不受影响。");
  about_text->set_tone(Tone::Muted);
  about->add_child(std::move(about_text));
  about->add_child(
      std::make_unique<KeyValueRow>("不可用时", "退化为占位块（不报错、不崩、布局照常）"));
  about->add_child(
      std::make_unique<KeyValueRow>("像素可见性", "嵌入内容由平台合成器上屏，不进 framebuffer"));
  page->add_child(std::move(about));

  // —— 嵌入区 ——
  auto host_card = make_card("card-nh-host", "嵌入区");
  auto host = std::make_unique<NativeHost>();
  host->set_id("native-host-demo");
  host->set_preferred_size({0.0f, 420.0f});
  host->set_placeholder_text(
      source == nullptr ? "没有可用的外部窗口来源" : "点「嵌入演示窗口」把外部窗口嵌到这里");
  NativeHost* host_ptr = host.get();
  host_card->add_child(std::move(host));
  page->add_child(std::move(host_card));

  // —— 自检读数 ——
  // 为什么页面必须画这些：嵌入内容的像素拍不到，`attached`/`content_*` 是**唯一**
  // 能判断“它到底有没有在跑、有没有对位”的通道（见 native_host.hpp 头注释）。
  auto probe_card = make_card("card-nh-probe", "自检读数");
  auto status_row = make_row();
  auto status_label = std::make_unique<Text>("状态");
  status_label->set_tone(Tone::Muted);
  status_label->set_scale(Text::Scale::Xs);
  auto status_text = std::make_unique<Text>("—");
  status_text->set_scale(Text::Scale::Xs);
  Text* status_ptr = status_text.get();
  status_row->add_child(std::move(status_label));
  status_row->add_child(std::move(status_text));
  probe_card->add_child(std::move(status_row));

  auto attached_row = make_row();
  auto attached_label = std::make_unique<Text>("已嵌入");
  attached_label->set_tone(Tone::Muted);
  attached_label->set_scale(Text::Scale::Xs);
  auto attached_text = std::make_unique<Text>("否");
  attached_text->set_scale(Text::Scale::Xs);
  Text* attached_ptr = attached_text.get();
  attached_row->add_child(std::move(attached_label));
  attached_row->add_child(std::move(attached_text));
  probe_card->add_child(std::move(attached_row));

  auto geometry_row = make_row();
  auto geometry_label = std::make_unique<Text>("几何");
  geometry_label->set_tone(Tone::Muted);
  geometry_label->set_scale(Text::Scale::Xs);
  auto geometry_text = std::make_unique<Text>("—");
  geometry_text->set_scale(Text::Scale::Xs);
  Text* geometry_ptr = geometry_text.get();
  geometry_row->add_child(std::move(geometry_label));
  geometry_row->add_child(std::move(geometry_text));
  probe_card->add_child(std::move(geometry_row));
  page->add_child(std::move(probe_card));

  // —— 操作 ——
  auto actions = make_row();
  auto attach_button = std::make_unique<Button>("嵌入演示窗口", Button::Variant::Primary);
  attach_button->set_id("btn-nh-attach");
  auto detach_button = std::make_unique<Button>("解除嵌入", Button::Variant::Ghost);
  detach_button->set_id("btn-nh-detach");
  Button* attach_ptr = attach_button.get();
  Button* detach_ptr = detach_button.get();
  actions->add_child(std::move(attach_button));
  actions->add_child(std::move(detach_button));
  page->add_child(std::move(actions));

  if (source == nullptr) {
    attach_ptr->set_enabled(false);
    detach_ptr->set_enabled(false);
  }

  // 一次“取窗口 + 嵌入 + 刷新读数”的完整动作。
  const auto do_attach = [host_ptr, source, status_ptr, attached_ptr, geometry_ptr]() {
    if (source == nullptr) return;
    if (!source->supported()) {
      // 不支持时**当场如实报**，不假装成功、也不留半嵌入状态。
      status_ptr->set_content("此环境不支持嵌入：" + source->status_text());
      attached_ptr->set_content("否");
      geometry_ptr->set_content("—");
      host_ptr->set_placeholder_text("此环境不支持嵌入原生窗口");
      return;
    }
    status_ptr->set_content("正在获取窗口…");
    void* child = source->acquire();
    if (child == nullptr) {
      host_ptr->set_placeholder_text("还没拿到窗口（" + source->status_text() + "）");
      status_ptr->set_content(source->status_text());
      return;
    }
    const bool ok = host_ptr->attach(child);
    status_ptr->set_content(ok ? "已嵌入" : ("嵌入失败：" + source->status_text()));
    attached_ptr->set_content(ok ? "是" : "否");
    if (!ok) {
      host_ptr->set_placeholder_text("嵌入失败：" + source->status_text());
      geometry_ptr->set_content("—");
      return;
    }
    // 几何读数取**宿主侧真实读数**（不是本地标志位）：判断“内容是否落在 (0,0)
    // 并填满宿主”要的就是这两组数相减。读经属性面，与控制通道 `get` 同一口径。
    const auto read = [host_ptr](std::string_view name) {
      return host_ptr->get_property(name).value_or("0");
    };
    geometry_ptr->set_content(std::format("内容 {},{}({}×{}) / 宿主 {}×{}", read("content_x"),
                                          read("content_y"), read("content_width"),
                                          read("content_height"), read("host_width"),
                                          read("host_height")));
  };
  attach_ptr->on_click = do_attach;

  // 「解除嵌入」同时把读数复位——否则界面会一直显示上一次的成功状态。
  detach_ptr->on_click = [host_ptr, attached_ptr, status_ptr, geometry_ptr]() {
    host_ptr->detach();
    attached_ptr->set_content("否");
    status_ptr->set_content("已解除");
    geometry_ptr->set_content("—");
    host_ptr->set_placeholder_text("已解除嵌入（外部窗口已还原为独立窗口）");
  };

  // —— 已知边界 ——
  auto limits = make_card("card-nh-limits", "已知边界");
  limits->add_child(std::make_unique<KeyValueRow>(
      "平台", "Windows 已实现（SetParent + 裁标题栏）；X11 / Wayland 未实现"));
  limits->add_child(
      std::make_unique<KeyValueRow>("截图", "capture / visual_diff 拍不到嵌入内容"));
  limits->add_child(std::make_unique<KeyValueRow>(
      "共存", "同窗口多个嵌入物、磨砂背景、圆角裁剪尚未支持"));
  page->add_child(std::move(limits));

  return page;
}

}  // namespace gallery
