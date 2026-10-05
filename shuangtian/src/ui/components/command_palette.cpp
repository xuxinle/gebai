#include "st/ui/components/command_palette.hpp"

#include <algorithm>
#include <format>

#include "st/core/string.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/ui_root.hpp"

namespace st::ui {

CommandPalette::CommandPalette(std::vector<Command> commands)
    : commands_(std::move(commands)) {
  set_id("command-palette");
  // 遮罩：半透明黑（亮暗主题同形——浮层惯例，与 Dialog 遮罩同族）
  mask_color_ = math::Color{0, 0, 0, 110};
  card_bg_ = math::Color::rgb(26, 34, 48);
  card_border_ = math::Color::rgba(255, 255, 255, 26);

  auto card = std::make_unique<Panel>(FlexDirection::Column);
  card->set_id("palette-card");
  card->style().width = kCardWidth;
  card->style().background = card_bg_;
  card->style().radius = 10.0f;
  card->style().border_color = card_border_;
  card->style().border_width = 1.0f;
  card->style().shadow = Shadow{};
  card->style().clip_children = true;

  auto input_row = std::make_unique<Panel>(FlexDirection::Row);
  input_row->set_id("palette-input-row");
  input_row->style().padding = math::Insets{12.0f, 10.0f, 12.0f, 10.0f};
  input_row->style().gap = 8.0f;
  input_row->style().align_items = Align::Center;
  input_row->add_child(std::make_unique<IconView>("svg:search", 16.0f));
  auto input = std::make_unique<Input>();
  input->set_id("palette-input");
  input->set_placeholder("输入命令名过滤，Enter 执行，Esc 关闭");
  input_ = static_cast<Input*>(input_row->add_child(std::move(input)));
  card->add_child(std::move(input_row));

  auto list = std::make_unique<List>();
  list->set_id("palette-list");
  list->style().max_height = kListMaxHeight;
  list_ = static_cast<List*>(card->add_child(std::move(list)));
  add_child(std::move(card));

  input_->on_change = [this](std::string_view value) {
    set_query(std::string(value));
  };
  input_->on_submit = [this](std::string_view) { activate_highlighted(); };
  rebuild();
}

void CommandPalette::set_commands(std::vector<Command> commands) {
  commands_ = std::move(commands);
  rebuild();
}

void CommandPalette::set_query(std::string query) {
  if (query_ == query) return;
  query_ = std::move(query);
  // 程序性设过滤词时也要把输入框的文本同步过去——否则“面板里显示的词”与
  // “输入框里的词”不一致，用户再敲一个字就会被旧内容替换（实测踩到）。
  if (input_ != nullptr && input_->value() != query_) input_->set_text(query_);
  rebuild();
}

void CommandPalette::grab_focus() {
  // 键盘直达过滤框。不调这句的话：根上的全局快捷键（Ctrl+Shift+P / Ctrl+P）把面板
  // 显示出来，焦点却仍在底层编辑器上——**敲进去的字会跑进代码里**，面板看着开着
  // 却打不了字（实测踩到的“面板打不了字”）。
  //
  // 走 `UiRoot::set_focus`（唯一入口：发 FocusOut/FocusIn、维护 Tab 环），而不是
  // 直接 `input_->activate()`：后者的语义是“触发本元素默认动作”（对 Input 就是
  // on_submit 提交），拿它当“拿焦点”会当场触发一次提交。
  if (input_ == nullptr) return;
  // 类型化宿主契约（原先 `owner_as<UiRoot>()` 是 `void*` + `static_cast`：
  // 转到错误类型是静默 UB；现在走 `Element::HostFocus`，写错即编译错误）。
  if (auto* host = this->host(); host != nullptr) {
    (void)host->set_keyboard_focus(input_);
    return;
  }
  // 未上树（单元测试/嵌入式）：至少让输入框自己可接收键盘
  input_->set_focused(true);
}

auto CommandPalette::active_id() const -> std::string {
  if (highlight_ == kNoSelection || highlight_ >= matched_.size()) return {};
  const std::size_t index = matched_[highlight_];
  return index < commands_.size() ? commands_[index].id : std::string{};
}

auto CommandPalette::move_highlight(int delta) -> std::size_t {
  if (matched_.empty()) return kNoSelection;
  std::size_t current = highlight_ == kNoSelection ? 0 : highlight_;
  if (delta > 0) {
    current = current + 1 >= matched_.size() ? 0 : current + 1;
  } else {
    current = current == 0 ? matched_.size() - 1 : current - 1;
  }
  select(current);
  return current;
}

auto CommandPalette::activate_highlighted() -> bool {
  if (highlight_ == kNoSelection || highlight_ >= matched_.size()) return false;
  return run_command(matched_[highlight_]);
}

auto CommandPalette::run_command(std::size_t command_index) -> bool {
  // **执行与通知只此一处**。曾经把 `handler()` 写在 Enter 路径、把 `on_command`
  // 写在列表项回调里——于是“鼠标点”与“回车”干的事不一样：点一下只发外部通知，
  // 示例侧把通知当成“已执行”又不动手 → **状态栏说执行了、文件其实没打开**。
  if (command_index >= commands_.size()) return false;
  const Command& command = commands_[command_index];
  const std::string id = command.id;
  if (command.handler) command.handler();
  if (on_command) on_command(id);
  return true;
}

void CommandPalette::rebuild() {
  // 过滤：title/detail 不区分大小写子串匹配（本地 to_lower——命令文案以 ASCII/中文为主）
  const auto contains = [](std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    const auto lower = [](std::string_view text) {
      std::string out(text);
      for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
      }
      return out;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
  };
  matched_.clear();
  for (std::size_t i = 0; i < commands_.size(); ++i) {
    if (contains(commands_[i].title, query_) || contains(commands_[i].detail, query_)) {
      matched_.push_back(i);
    }
  }
  std::vector<List::Entry> entries;
  entries.reserve(matched_.size());
  for (const std::size_t index : matched_) {
    List::Entry entry;
    entry.key = commands_[index].id;
    entry.label = commands_[index].detail.empty()
                      ? commands_[index].title
                      : commands_[index].title + "  ·  " + commands_[index].detail;
    // **点击条目必须能执行命令**。这里踩过一个真缺陷：执行链曾挂在元素层的
    // `ListItem::on_activate` 上（需要取出列表项的裸指针再去绑），既穿过组件私有
    // 子元素、又被 `List` 自己的“点击即选中本容器”语义盖住——结果是“点一下没反应”，
    // 只有 Enter 能用。现在把执行意图交给数据层的 `Entry::on_activate`：
    // `sync_items` 会在建项时把它接到项上，与 List 的选中链不再争同一个槽位。
    // 并且**与 Enter 走同一条 `run_command`**（handler + on_command 都在里面）——
    // 两条路径各自拼一份执行链，正是上一条教训的根源。
    entry.on_activate = [this, command_index = index]() { (void)run_command(command_index); };
    entries.push_back(std::move(entry));
  }
  list_->sync_items(entries);
  highlight_ = entries.empty() ? kNoSelection : 0;
  if (highlight_ != kNoSelection) list_->select(highlight_, false);
  mark_layout_dirty();
}

void CommandPalette::select(std::size_t index) {
  highlight_ = index;
  list_->select(index, false);
  mark_dirty();
}

void CommandPalette::apply_theme(const Theme& theme) {
  Element::apply_theme(theme);
  mask_color_ = math::Color{0, 0, 0, 110};
  // 深底浮层在亮暗主题下同形（与示例期实现一致——浮层惯例）；边框随主题取弱对比
  card_bg_ = theme.mode() == ThemeMode::Light ? math::Color::rgb(38, 46, 60)
                                              : math::Color::rgb(26, 34, 48);
  card_border_ = theme.mode() == ThemeMode::Light ? math::Color::rgba(255, 255, 255, 26)
                                                  : math::Color::rgba(255, 255, 255, 26);
}

void CommandPalette::measure(const RenderContext& context, const Constraints& constraints) {
  Element::measure(context, constraints);
  // FillViewport：占满可用空间（遮罩铺满视口）
  const float width = constraints.max_width != kUnbounded ? constraints.max_width : 1280.0f;
  const float height = constraints.max_height != kUnbounded ? constraints.max_height : 800.0f;
  measured_ = math::Size{width, height};
}

void CommandPalette::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  // 卡片顶部居中（VSCode 形态）
  if (child_count() == 0) return;
  Element* card = children().front().get();
  card->measure(context, Constraints{});
  const math::Size card_size = card->measured_size();
  const float x = rect.x + (rect.width - card_size.width) * 0.5f;
  const float y = rect.y + kTopOffset;
  card_rect_ = math::Rect{x, y, card_size.width, std::min(card_size.height, kListMaxHeight + 96.0f)};
  card->arrange(context, card_rect_);
  bounds_ = rect;
}

void CommandPalette::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  (void)context;
  // 遮罩铺满（卡片区域稍亮由卡片自身背景承担）
  canvas.fill_rect(bounds_, raster::Paint::solid(mask_color_));
}

auto CommandPalette::on_event(const RenderContext& context, Event& event) -> bool {
  if (event.kind == EventKind::KeyDown) {
    if (event.key == "Escape") {
      if (on_close) on_close();
      return true;
    }
    if (event.key == "ArrowDown") {
      move_highlight(1);
      return true;
    }
    if (event.key == "ArrowUp") {
      move_highlight(-1);
      return true;
    }
    if (event.key == "Enter") {
      activate_highlighted();
      return true;
    }
  }
  // 遮罩点击（卡片外）→ 关闭；卡片内交给子元素
  if (event.kind == EventKind::MouseDown && !card_rect_.contains(event.position)) {
    if (on_close) on_close();
    return true;
  }
  return Element::on_event(context, event);
}

void CommandPalette::activate() {
  Element::activate();
  // 打开即聚焦过滤输入（键盘直达）
  if (input_ != nullptr) input_->activate();
}

auto CommandPalette::semantics_text() const -> std::string {
  return "命令面板";
}

auto CommandPalette::semantics_value() const -> std::string {
  return std::format("q={} match={}/{}", query_, matched_.size(), commands_.size());
}

auto CommandPalette::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "query") return query_;
  if (name == "command_count") return std::to_string(commands_.size());
  if (name == "match_count") return std::to_string(matched_.size());
  if (name == "active") {
    return highlight_ == kNoSelection ? std::string("-1") : std::to_string(highlight_);
  }
  return Element::get_property(name);
}

auto CommandPalette::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "query") {
    set_query(std::string(value));
    if (input_ != nullptr) input_->set_text(std::string(value));
    return true;
  }
  if (name == "commands") return false;  // 数据经 API 注入，不走字符串通道
  return Element::set_property(name, value);
}

auto CommandPalette::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.push_back("query");
  names.push_back("command_count");
  names.push_back("match_count");
  names.push_back("active");
  return names;
}

auto CommandPalette::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "activate" || action == "submit") return activate_highlighted();
  if (action == "select") {
    // argument：序号或命令 id
    if (argument.empty()) return false;
    if (std::all_of(argument.begin(), argument.end(),
                    [](char ch) { return ch >= '0' && ch <= '9'; })) {
      const auto index = static_cast<std::size_t>(std::stoull(std::string(argument)));
      if (index >= matched_.size()) return false;
      select(index);
      return true;
    }
    for (std::size_t i = 0; i < matched_.size(); ++i) {
      if (commands_[matched_[i]].id == argument) {
        select(i);
        return true;
      }
    }
    return false;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
