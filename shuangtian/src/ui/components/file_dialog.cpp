#include "st/ui/components/file_dialog.hpp"

#include <algorithm>
#include <span>
#include <utility>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {
namespace {

/// 文本端口取用（`RenderContext::text` 可为空 → 退化为 no-op 端口）。
[[nodiscard]] auto text_port_of(const RenderContext& context) -> const TextPort& {
  return context.text != nullptr ? *context.text : NullTextPort::instance();
}

/// 单行文本（省略 + 垂直居中；与 `Dialog` 的 draw_line 同一做法）。
void draw_line(const RenderContext& context, raster::Surface& canvas, std::string_view text,
               math::Rect box, float size, math::Color color) {
  if (text.empty() || box.width <= 0.0f || box.height <= 0.0f) return;
  const TextPort& port = text_port_of(context);
  const std::string clipped = port.ellipsize(text, size, box.width);
  if (clipped.empty()) return;
  const float line = port.line_height(size);
  const float y = box.y + (box.height - line) * 0.5f;
  port.draw(canvas, clipped, math::Point{box.x, y}, size, color);
}

/// 覆盖率取向归一（与 `overlay.cpp` 的 oriented 同因：raster 层只吃正绕向覆盖率，
/// `Path` 工厂产出反向绕向；按扁平化折线反转重建）。
[[nodiscard]] auto oriented(const raster::Path& path) -> raster::Path {
  raster::Path out;
  for (const raster::Polyline& polyline : path.flatten(0.25f)) {
    const std::span<const math::Point> points = polyline.points;
    if (points.size() < 2U) continue;
    out.move_to(points.back());
    for (std::size_t index = points.size() - 1U; index > 0U; --index) {
      out.line_to(points[index - 1U]);
    }
    if (polyline.closed) out.close();
  }
  return out;
}

void fill_round_rect(raster::Surface& canvas, math::Rect rect, float radius, math::Color color) {
  if (rect.is_empty()) return;
  raster::Path path;
  path.add_rounded_rect(rect, radius, radius, radius, radius);
  canvas.fill_path(oriented(path), raster::Paint::solid(color));
}

/// 展开三角（目录行前导；`expanded` 时旋转 90°）。
void draw_triangle(raster::Surface& canvas, math::Point center, float radius, math::Color color,
                   bool expanded) {
  raster::Path path;
  const float r = radius;
  if (expanded) {
    // 向下：顶点在下
    path.move_to(math::Point{center.x - r, center.y - r * 0.6f});
    path.line_to(math::Point{center.x + r, center.y - r * 0.6f});
    path.line_to(math::Point{center.x, center.y + r * 0.6f});
  } else {
    // 向右：顶点在右
    path.move_to(math::Point{center.x - r * 0.6f, center.y - r});
    path.line_to(math::Point{center.x - r * 0.6f, center.y + r});
    path.line_to(math::Point{center.x + r * 0.6f, center.y});
  }
  path.close();
  canvas.fill_path(oriented(path), raster::Paint::solid(color));
}

/// 文件大小可读化（`1234` → `1.2 KB`；目录返回空）。
[[nodiscard]] auto human_size(std::uint64_t bytes) -> std::string {
  if (bytes >= 1024ULL * 1024ULL) {
    return std::to_string(bytes / (1024ULL * 1024ULL)) + " MB";
  }
  if (bytes >= 1024ULL) {
    return std::to_string(bytes / 1024ULL) + " KB";
  }
  return std::to_string(bytes) + " B";
}

/// 双击判定窗口（ms）。
constexpr double kDoubleClickMs = 450.0;

}  // namespace

FileDialog::FileDialog(Mode mode, std::string title)
    : mode_(mode), title_(std::move(title)) {
  style_.background = math::Color{0, 0, 0, 0};
  set_focusable(true);
  if (mode_ == Mode::Save) filename_ = "untitled.txt";
}

auto FileDialog::make(Mode mode, std::string title) -> std::unique_ptr<FileDialog> {
  return std::make_unique<FileDialog>(mode, std::move(title));
}

auto FileDialog::entry(std::size_t index) const noexcept -> const st::fs::DirEntry* {
  return index < entries_.size() ? &entries_[index] : nullptr;
}

void FileDialog::set_directory(const std::string& path) {
  directory_ = path;
  reload();
}

void FileDialog::set_filename(std::string name) {
  filename_ = std::move(name);
  mark_dirty();
}

auto FileDialog::set_pending_path(const std::string& path) -> bool {
  if (path.empty() || !st::fs::exists(path)) return false;   // 不存在的路径：不改任何状态
  if (st::fs::is_directory(path)) {
    set_directory(path);
    mark_dirty();
    return true;
  }
  // 文件：先进入它所在目录（`reload` 会把 selected_ 复位），再回填文件名。
  // 顺序不能反——`set_directory` 内部会清选中态。
  set_directory(st::fs::parent(path));
  filename_ = st::fs::file_name(path);
  // 在列表里把那一项选中（让画面确实高亮它，与实际状态一致）
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    if (entries_[index].name == filename_) {
      selected_ = index;
      break;
    }
  }
  mark_dirty();
  return true;
}

auto FileDialog::select_entry(std::size_t index) -> bool {
  if (index >= entries_.size()) return false;
  // 与鼠标点击**同一个** `activate_entry`：回填文件名等副作用完全一致。
  // 目录项会「进入目录」而不触发 `on_confirm`（与双击目录同语义）——
  // 这正是"模拟选了一个目录"该有的行为。
  activate_entry(index);
  mark_dirty();
  return true;
}

void FileDialog::reload() {
  error_.clear();
  entries_.clear();
  selected_ = static_cast<std::size_t>(-1);
  scroll_y_ = 0.0f;
  auto listed = st::fs::list_dir(directory_);
  if (!listed) {
    error_ = listed.error().to_string();
    mark_dirty();
    return;
  }
  entries_ = std::move(*listed);
  std::sort(entries_.begin(), entries_.end(), [](const st::fs::DirEntry& a,
                                                 const st::fs::DirEntry& b) {
    if (a.is_dir != b.is_dir) return a.is_dir;  // 目录在前
    return a.name < b.name;
  });
  mark_dirty();
}

void FileDialog::activate_entry(std::size_t index) {
  if (index >= entries_.size()) return;
  const st::fs::DirEntry& entry = entries_[index];
  if (entry.is_dir) {
    set_directory(entry.path);
    return;
  }
  filename_ = entry.name;
  selected_ = index;
  confirm();
}

void FileDialog::move_selection(int delta) {
  if (entries_.empty()) return;
  if (selected_ == static_cast<std::size_t>(-1)) {
    selected_ = delta > 0 || entries_.size() == 1U ? 0 : entries_.size() - 1U;
  } else {
    const auto current = static_cast<std::ptrdiff_t>(selected_);
    const auto total = static_cast<std::ptrdiff_t>(entries_.size());
    std::ptrdiff_t next = current + delta;
    if (next < 0) next = 0;
    if (next >= total) next = total - 1;
    selected_ = static_cast<std::size_t>(next);
  }
  // 文件条目选中即回填文件名（目录不改文件名——保存场景保持输入内容）
  if (entries_[selected_].is_dir == false) filename_ = entries_[selected_].name;
  mark_dirty();
}

void FileDialog::confirm() {
  if (filename_.empty() || on_confirm == nullptr) {
    mark_dirty();
    return;
  }
  on_confirm(st::fs::join(directory_, filename_));
}

void FileDialog::input_insert(std::string_view text) {
  if (text.empty()) return;
  filename_.append(text);
  mark_dirty();
}

void FileDialog::input_backspace() {
  if (filename_.empty()) return;
  // 按 UTF-8 码点退格（与编辑器语义一致：不留下半个字符）
  std::size_t end = filename_.size();
  std::size_t begin = end > 0U ? end - 1U : end;
  while (begin > 0U && (static_cast<unsigned char>(filename_[begin]) & 0xC0U) == 0x80U) {
    --begin;
  }
  filename_.erase(begin, end - begin);
  mark_dirty();
}

void FileDialog::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.color = theme.colors().text;
  style_.font_size = metrics.font_base;
  style_.padding = math::Insets{};
}

void FileDialog::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  // 遮罩铺满分到的约束（FillViewport 挂载时即视口）；卡片尺寸在 arrange 里按 bounds 定。
  float width = constraints.max_width < kUnbounded ? constraints.max_width : 640.0f;
  float height = constraints.max_height < kUnbounded ? constraints.max_height : 420.0f;
  measured_ = math::Size{width, height};
}

void FileDialog::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  const Metrics& metrics = context.theme.metrics();

  // 卡片：目标尺寸夹入视口（过窄/过矮时退让）。
  float card_width = std::clamp(rect.width * 0.8f, kMinWidth, kMaxWidth);
  if (card_width > rect.width - metrics.space_lg * 2.0f) {
    card_width = std::max(0.0f, rect.width - metrics.space_lg * 2.0f);
  }
  float card_height = std::clamp(kMaxHeight, kMinHeight, std::max(kMinHeight, rect.height));
  if (card_height > rect.height - metrics.space_lg * 2.0f) {
    card_height = std::max(0.0f, rect.height - metrics.space_lg * 2.0f);
  }
  card_ = math::Rect{rect.x + (rect.width - card_width) * 0.5f,
                     rect.y + (rect.height - card_height) * 0.5f, card_width, card_height};

  // —— 卡片内纵向分块（自上而下）：标题 / 目录行 / 列表 / 输入行 / 按钮行 ——
  const TextPort& port = text_port_of(context);
  float y = card_.y + kPadding;
  const float content_width = std::max(0.0f, card_.width - kPadding * 2.0f);
  const float title_line = port.line_height(metrics.font_xl);
  y += title_line + metrics.space_md;

  const float dir_line = port.line_height(metrics.font_base) + metrics.space_xs * 2.0f;
  y += dir_line + metrics.space_sm;

  // 输入行 + 按钮行先预留（列表占余下空间）。
  const float input_height = metrics.control_height;
  const float buttons_height = metrics.control_height;
  const float bottom_reserved = input_height + metrics.space_sm + buttons_height + kPadding;

  const float list_top = y;
  const float list_bottom = card_.bottom() - bottom_reserved;
  list_ = math::Rect{card_.x + kPadding, list_top, content_width,
                     std::max(0.0f, list_bottom - list_top)};

  input_ = math::Rect{card_.x + kPadding, list_.bottom() + metrics.space_sm, content_width,
                      input_height};

  // —— 按钮行（子 Button；右对齐：取消在左、确认在右） ——
  if (confirm_button_ == nullptr) {
    auto cancel = std::make_unique<Button>("取消", Button::Variant::Secondary, Button::Size::Medium);
    cancel->on_click = [this]() {
      if (on_cancel) on_cancel();
    };
    auto ok = std::make_unique<Button>(
        mode_ == Mode::Save ? "保存" : "打开", Button::Variant::Primary, Button::Size::Medium);
    ok->on_click = [this]() { FileDialog::confirm(); };
    cancel_button_ = add_child(std::move(cancel));
    confirm_button_ = add_child(std::move(ok));
  }
  const Constraints unbounded;
  confirm_button_->measure(context, unbounded);
  cancel_button_->measure(context, unbounded);
  const float button_y = card_.bottom() - kPadding - buttons_height;
  float cursor_x = card_.right() - kPadding;
  const math::Size confirm_size = confirm_button_->measured_size();
  confirm_button_->arrange(context,
                           math::Rect{cursor_x - confirm_size.width, button_y, confirm_size.width,
                                      confirm_size.height});
  cursor_x -= confirm_size.width + metrics.space_sm;
  const math::Size cancel_size = cancel_button_->measured_size();
  cancel_button_->arrange(context,
                          math::Rect{cursor_x - cancel_size.width, button_y, cancel_size.width,
                                     cancel_size.height});

  layout_dirty_ = false;
}

auto FileDialog::parent_row_rect() const noexcept -> math::Rect {
  if (list_.is_empty()) return math::Rect{};
  return math::Rect{list_.x, list_.y, list_.width, kRowHeight};
}

auto FileDialog::entry_rect(std::size_t index) const noexcept -> math::Rect {
  if (list_.is_empty() || index >= entries_.size()) return math::Rect{};
  // 列表区首行是 `..`（上级），条目行跟在其后。
  const float y = list_.y + kRowHeight * (static_cast<float>(index) + 1.0f) - scroll_y_;
  return math::Rect{list_.x, y, list_.width, kRowHeight};
}

void FileDialog::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();

  // 遮罩（覆盖整个分到的矩形）。
  canvas.fill_rect(bounds_, raster::Paint::solid(colors.overlay));
  if (card_.is_empty()) return;

  // 卡片：阴影 + surface 底 + radius_xl。
  const Shadow shadow = shadow_lg(context.theme);
  canvas.draw_shadow(card_, metrics.radius_xl, shadow.blur, shadow.color,
                     math::Point{shadow.offset_x, shadow.offset_y});
  fill_round_rect(canvas, card_, metrics.radius_xl, colors.surface);

  const TextPort& port = text_port_of(context);
  const float content_width = std::max(0.0f, card_.width - kPadding * 2.0f);

  // 标题行。
  float y = card_.y + kPadding;
  const float title_line = port.line_height(metrics.font_xl);
  draw_line(context, canvas, title_, math::Rect{card_.x + kPadding, y, content_width, title_line},
            metrics.font_xl, colors.text);
  y += title_line + metrics.space_md;

  // 目录行（当前路径，小字弱色）。
  const float dir_line = port.line_height(metrics.font_base) + metrics.space_xs * 2.0f;
  draw_line(context, canvas, directory_,
            math::Rect{card_.x + kPadding, y, content_width, dir_line}, metrics.font_xs,
            colors.text_muted);
  y += dir_line + metrics.space_sm;

  // 错误行优先呈现（fs 失败时列表区只画错误）。
  if (!error_.empty()) {
    draw_line(context, canvas, error_,
              math::Rect{list_.x, list_.y, list_.width, port.line_height(metrics.font_base)},
              metrics.font_base, colors.danger);
    return;
  }

  // —— 文件列表（自绘行；首行 `..`） ——
  // 底色（浅凹槽）。
  fill_round_rect(canvas, list_, metrics.radius_md, colors.surface_alt);

  // `..` 行（有上级时）。
  if (!directory_.empty() && directory_ != "/" && directory_ != "\\") {
    const math::Rect up = parent_row_rect();
    draw_triangle(canvas, math::Point{up.x + 10.0f, up.y + up.height * 0.5f}, 4.0f,
                  colors.text_muted, true);
    draw_line(context, canvas, "..", math::Rect{up.x + 20.0f, up.y, up.width - 20.0f, up.height},
              metrics.font_base, colors.text_muted);
  }

  const std::size_t total = entries_.size();
  for (std::size_t index = 0; index < total; ++index) {
    const math::Rect row = entry_rect(index);
    if (row.bottom() < list_.y || row.y > list_.bottom()) continue;  // 视口剔除
    const st::fs::DirEntry& entry = entries_[index];
    const bool is_selected = index == selected_;
    if (is_selected) {
      // 选中底 + 左侧 2px 指示条（与 List 视觉同源）。
      fill_round_rect(canvas, row, metrics.radius_sm, colors.primary_soft);
      canvas.fill_rect(math::Rect{row.x, row.y + 2.0f, 2.0f, row.height - 4.0f},
                       raster::Paint::solid(colors.primary));
    }
    float text_x = row.x + 8.0f;
    if (entry.is_dir) {
      draw_triangle(canvas, math::Point{row.x + 12.0f, row.y + row.height * 0.5f}, 4.0f,
                    colors.text_muted, false);
      text_x = row.x + 22.0f;
    }
    const float size_width = port.measure_width(human_size(entry.size), metrics.font_xs);
    const float name_width = std::max(0.0f, row.width - (text_x - row.x) - size_width - 16.0f);
    draw_line(context, canvas, entry.name,
              math::Rect{text_x, row.y, name_width, row.height}, metrics.font_base,
              is_selected ? colors.primary : colors.text);
    if (!entry.is_dir) {
      draw_line(context, canvas, human_size(entry.size),
                math::Rect{row.right() - size_width - 8.0f, row.y, size_width + 8.0f, row.height},
                metrics.font_xs, colors.text_muted);
    }
  }

  // —— 文件名输入行（自绘：底 + 文本 + 光标） ——
  fill_round_rect(canvas, input_, metrics.radius_md, colors.surface_alt);
  const math::Insets inset{metrics.space_sm, 4.0f, metrics.space_sm, 4.0f};
  const math::Rect input_text{input_.x + inset.left, input_.y,
                              std::max(0.0f, input_.width - inset.horizontal()), input_.height};
  draw_line(context, canvas, filename_, input_text, metrics.font_base, colors.text);
  if (input_focused_) {
    const float text_width = port.measure_width(filename_, metrics.font_base);
    const float caret_x = input_text.x + std::min(text_width, input_text.width - 1.0f);
    canvas.fill_rect(math::Rect{caret_x, input_.y + 5.0f, 1.0f, input_.height - 10.0f},
                     raster::Paint::solid(colors.primary));
  }
}

auto FileDialog::on_event(const RenderContext& context, Event& event) -> bool {
  switch (event.kind) {
    case EventKind::KeyDown: {
      if (event.key == "Escape" || event.key == "Esc") {
        event.handled = true;
        if (on_cancel) on_cancel();
        return true;
      }
      if (event.key == "ArrowDown") {
        move_selection(1);
        return true;
      }
      if (event.key == "ArrowUp") {
        move_selection(-1);
        return true;
      }
      if (event.key == "Enter") {
        if (selected_ != static_cast<std::size_t>(-1)) {
          activate_entry(selected_);
        } else {
          confirm();
        }
        return true;
      }
      if (event.key == "Backspace") {
        if (input_focused_) input_backspace();
        return true;
      }
      return false;
    }
    case EventKind::TextInput:
      // 输入行聚焦时字符进入文件名（未聚焦也接受——单输入位的对话框惯例）
      input_insert(event.text);
      return true;
    case EventKind::Wheel: {
      // 列表纵向滚动（自绘行）。
      const float content_height =
          kRowHeight * (static_cast<float>(entries_.size()) + 1.0f);
      const float max_scroll = std::max(0.0f, content_height - list_.height);
      scroll_y_ = std::clamp(scroll_y_ + event.wheel_delta * 48.0f, 0.0f, max_scroll);
      mark_dirty();
      return true;
    }
    case EventKind::MouseDown:
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      if (!card_.is_empty() && card_.contains(event.position)) {
        // 卡片内：输入行聚焦切换 + 列表行命中（其余区域只吞掉）
        input_focused_ = input_.contains(event.position);
        if (input_focused_) {
          mark_dirty();
          return true;
        }
        const math::Rect up = parent_row_rect();
        if (!up.is_empty() && up.contains(event.position)) {
          set_directory(st::fs::parent(directory_));
          return true;
        }
        for (std::size_t index = 0; index < entries_.size(); ++index) {
          const math::Rect row = entry_rect(index);
          if (row.is_empty() || !row.contains(event.position)) continue;
          selected_ = index;
          if (event.kind == EventKind::DoubleClick ||
              event.kind == EventKind::TripleClick) {
            activate_entry(index);  // 双击：目录进入/文件确认
          } else if (!entries_[index].is_dir) {
            filename_ = entries_[index].name;
          }
          mark_dirty();
          return true;
        }
        return true;
      }
      // 遮罩点击 = 取消。
      event.handled = true;
      if (on_cancel) on_cancel();
      return true;
    }
    case EventKind::MouseMove:
      return true;  // 模态：吞掉指针，避免穿透
    default:
      (void)context;
      return false;
  }
}

[[nodiscard]] auto FileDialog::semantics_value() const -> std::string {
  return directory_ + " | " + std::to_string(entries_.size()) + " 项 | " + filename_;
}

auto FileDialog::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "directory") return directory_;
  if (name == "filename") return filename_;
  if (name == "mode") return mode_ == Mode::Save ? std::string("save") : std::string("open");
  if (name == "error") return error_;
  if (name == "selected") {
    const st::fs::DirEntry* entry = selected_entry();
    return entry != nullptr ? entry->name : std::string();
  }
  return Element::get_property(name);
}

auto FileDialog::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "directory") {
    set_directory(std::string(value));
    return true;
  }
  if (name == "filename") {
    set_filename(std::string(value));
    return true;
  }
  // `pending_path`：**程序化选路**（无头/自动化唯一的选路入口，见 `set_pending_path`）。
  // 走属性面而不是只留 C++ 接口：控制通道的 `set` 是外部（智能体）与界面之间
  // **唯一**的通用写入口——只开 C++ 接口等于"能力存在但外部用不到"。
  // 失败（路径不存在）返回 false，`set` 会如实报错，不静默。
  if (name == "pending_path") {
    return set_pending_path(std::string(value));
  }
  return Element::set_property(name, value);
}

auto FileDialog::property_names() const -> std::vector<std::string_view> {
  std::vector<std::string_view> names = Element::property_names();
  names.insert(names.end(),
               {"directory", "filename", "mode", "error", "selected", "pending_path"});
  return names;
}

auto FileDialog::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "confirm") {
    confirm();
    return true;
  }
  if (action == "cancel") {
    if (on_cancel) on_cancel();
    return true;
  }
  if (action == "up") {
    set_directory(st::fs::parent(directory_));
    return true;
  }
  // `select`：程序化选中第 index 个条目（与鼠标点它同一条 `activate_entry`）。
  // 与 `pending_path` 分工：前者给"第几项"（列表位置稳定时方便），
  // 后者给"哪个路径"（真正想表达的东西）。两者都走同一套副作用。
  if (action == "select") {
    const auto parsed = st::parse_u64(argument);
    if (!parsed.has_value()) return false;
    return select_entry(static_cast<std::size_t>(*parsed));
  }
  if (action == "reload") {
    reload();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
