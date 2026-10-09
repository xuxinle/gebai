#include "st/ui/components/file_dialog.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <span>
#include <utility>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/text_port.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::draw_line;
using components_internal::draw_triangle;
using components_internal::fill_round_rect;
using components_internal::text_port_of;

namespace {

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

}  // namespace

FileDialog::FileDialog(Mode mode, std::string title)
    : mode_(mode), title_(std::move(title)) {
  style_.background = math::Color{0, 0, 0, 0};
  set_focusable(true);
  // 本组件是"**遮罩 + 居中卡片**"形态（与 `Dialog` 同类）：它需要 overlay 宿主的
  // **整个矩形**（遮罩要盖满屏、卡片才居中）。声明 `grow` 让 flex 把宿主剩余
  // 空间全给它——不声明时只拿到自己的测量高（实测 1280×420：遮罩只盖上半屏、
  // 卡片贴顶，用户报"布局不合理、遮蔽的父元素不合理"）。
  style_.grow = true;
  if (mode_ == Mode::Save) filename_ = "untitled.txt";
}

auto FileDialog::make(Mode mode, std::string title) -> std::unique_ptr<FileDialog> {
  return std::make_unique<FileDialog>(mode, std::move(title));
}

// —— 过滤 / 隐藏 / 位置 / 新建 ——

void FileDialog::set_name_filters(std::vector<std::string> extensions) {
  // 归一：小写、含点（`.cpp`）；空串剔除。目录永不过滤（导航必需）。
  name_filters_.clear();
  name_filters_.reserve(extensions.size());
  for (auto& ext : extensions) {
    if (!ext.empty() && ext.front() != '.') ext.insert(ext.begin(), '.');
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (!ext.empty() && ext != ".") name_filters_.push_back(std::move(ext));
  }
  reload();   // 可见集合变了
}

void FileDialog::set_show_hidden(bool show) {
  if (show == show_hidden_) return;
  show_hidden_ = show;
  reload();
}

void FileDialog::set_places(std::vector<Place> places) {
  custom_places_ = std::move(places);
  mark_layout_dirty();
  mark_dirty();
}

auto FileDialog::places() const -> std::vector<Place> {
  // 内置 + 自定义，存在性过滤（不存在的位置不显示——点了也只会报错）。
  std::vector<Place> result;
  const std::string home = st::fs::home_dir();
  if (!home.empty() && st::fs::is_directory(home)) {
    result.push_back(Place{"主目录", home});
  }
#ifdef _WIN32
  // 盘符（C: D: …）：枚举不到就不显示（不猜）。
  for (char letter = 'A'; letter <= 'Z'; ++letter) {
    const std::string root = std::string(1, letter) + ":\\";
    if (st::fs::is_directory(root)) result.push_back(Place{std::string(1, letter) + ":", root});
  }
#endif
  for (const Place& place : custom_places_) {
    if (st::fs::is_directory(place.path)) result.push_back(place);
  }
  return result;
}

auto FileDialog::create_folder(std::string name) -> std::string {
  if (directory_.empty() || name.empty()) return {};
  // 名字冲突：`名字`、`名字-1`、`名字-2`…（与桌面环境一致的不恼人策略）。
  std::string candidate = st::fs::join(directory_, name);
  for (int attempt = 1; st::fs::exists(candidate); ++attempt) {
    if (attempt > 999) return {};   // 上限保护（防呆）
    candidate = st::fs::join(directory_, name + "-" + std::to_string(attempt));
  }
  if (!st::fs::create_directories(candidate)) return {};
  reload();
  // 建完就进入（"建完就用"是通常意图；想留在原地可继续 ..）。
  set_directory(candidate);
  return candidate;
}

auto FileDialog::entry_visible(const st::fs::DirEntry& entry) const -> bool {
  if (entry.is_dir) return true;   // 目录永不过滤（导航必需）
  if (!show_hidden_ && !entry.name.empty() && entry.name.front() == '.') return false;
  if (name_filters_.empty()) return true;
  std::string ext = st::fs::extension(entry.name);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return std::find(name_filters_.begin(), name_filters_.end(), ext) != name_filters_.end();
}

auto FileDialog::crumbs() const -> std::vector<std::pair<std::string, std::string>> {
  // 当前目录拆段：每段 {显示名, 跳转路径}。根段显示为 "/"（POSIX）或 "C:"（Win）。
  std::vector<std::pair<std::string, std::string>> result;
  if (directory_.empty()) return result;
  std::string accumulated;
  std::size_t at = 0;
  while (at < directory_.size()) {
    const std::size_t slash = directory_.find_first_of("/\\", at);
    const std::string segment = directory_.substr(at, slash == std::string::npos
                                                        ? std::string::npos : slash - at);
    if (segment.empty()) {
      // 首个空段 = 根（POSIX 开头的 `/`）。
      if (result.empty()) {
        accumulated = "/";
        result.emplace_back("/", "/");
      }
    } else if (segment.size() == 2 && segment[1] == ':' &&
               std::isalpha(static_cast<unsigned char>(segment[0])) != 0) {
      // Windows 盘符段。
      accumulated = segment + "\\";
      result.emplace_back(segment, accumulated);
    } else {
      accumulated = accumulated.empty() ? segment : st::fs::join(accumulated, segment);
      result.emplace_back(segment, accumulated);
    }
    at = (slash == std::string::npos) ? directory_.size() : slash + 1;
  }
  return result;
}

auto FileDialog::entry(std::size_t index) const noexcept -> const st::fs::DirEntry* {
  // index 是**可见序号**（过滤后）；经 visible_ 映射到全量条目。
  return index < visible_.size() ? &entries_[visible_[index]] : nullptr;
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
  // 在列表里把那一项选中（让画面确实高亮它，与实际状态一致）——按**可见序号**找
  //（过滤可能把同名文件藏起来，找不到就保持未选中，文件名仍在）。
  for (std::size_t index = 0; index < visible_.size(); ++index) {
    if (entries_[visible_[index]].name == filename_) {
      selected_ = index;
      break;
    }
  }
  mark_dirty();
  return true;
}

auto FileDialog::select_entry(std::size_t index) -> bool {
  if (index >= visible_.size()) return false;
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
  visible_.clear();
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
  // 可见集合（过滤/隐藏后）：`entry()`/命中/键盘导航都走 visible_ 下标。
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    if (entry_visible(entries_[index])) visible_.push_back(index);
  }
  mark_dirty();
}

auto FileDialog::list_content_height() const -> float {
  return kRowHeight * (static_cast<float>(visible_.size()) + 1.0f);   // +1 = `..` 行
}

void FileDialog::ensure_selected_visible() {
  if (selected_ == static_cast<std::size_t>(-1) || list_.is_empty()) return;
  const math::Rect row = entry_rect(selected_);
  const float over_top = list_.y - row.y;
  const float over_bottom = row.bottom() - list_.bottom();
  if (over_top > 0.0f) {
    scroll_y_ = std::max(0.0f, scroll_y_ - over_top);
  } else if (over_bottom > 0.0f) {
    const float max_scroll = std::max(0.0f, list_content_height() - list_.height);
    scroll_y_ = std::min(max_scroll, scroll_y_ + over_bottom);
  }
}

void FileDialog::activate_entry(std::size_t index) {
  const st::fs::DirEntry* found = entry(index);
  if (found == nullptr) return;
  const st::fs::DirEntry& entry = *found;
  if (entry.is_dir) {
    set_directory(entry.path);
    return;
  }
  // `Directory` 模式：**点文件不等于确认**——要选的目录是“当前所在”的那个。
  // 若沿用 Open 的“点文件即确认”，用户在浏览时误点一个文件就会把**该文件所在目录**
  // 当成选择结果确认掉（与“我想选文件夹”的意图不符，且无声）。
  if (mode_ == Mode::Directory) {
    selected_ = index;   // 仍然高亮（用户要知道自己点到了哪个），只是不确认
    mark_dirty();
    return;
  }
  filename_ = entry.name;
  selected_ = index;
  confirm();
}

void FileDialog::move_selection(int delta) {
  if (visible_.empty()) return;
  if (selected_ == static_cast<std::size_t>(-1)) {
    selected_ = delta > 0 || visible_.size() == 1U ? 0 : visible_.size() - 1U;
  } else {
    const auto current = static_cast<std::ptrdiff_t>(selected_);
    const auto total = static_cast<std::ptrdiff_t>(visible_.size());
    std::ptrdiff_t next = current + delta;
    if (next < 0) next = 0;
    if (next >= total) next = total - 1;
    selected_ = static_cast<std::size_t>(next);
  }
  // 文件条目选中即回填文件名（目录不改文件名——保存场景保持输入内容）
  const st::fs::DirEntry* picked = entry(selected_);
  if (picked != nullptr && !picked->is_dir) filename_ = picked->name;
  ensure_selected_visible();
  mark_dirty();
}

void FileDialog::confirm() {
  if (on_confirm == nullptr) {
    mark_dirty();
    return;
  }
  if (mode_ == Mode::Directory) {
    // 选目录：返回**当前目录本身**（不是「目录/文件名」拼接），且前置条件是
    // “目录非空”——文件名行在这个模式下不参与语义（界面上隐藏）。
    if (directory_.empty()) {
      mark_dirty();
      return;
    }
    on_confirm(directory_);
    return;
  }
  if (filename_.empty()) {
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
  // 存下主题（新子元素创建时用它补一次下发——见 `arrange` 里按钮创建处的说明）。
  last_theme_ = theme;
  theme_valid_ = true;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.color = theme.colors().text;
  style_.font_size = metrics.font_base;
  style_.padding = math::Insets{};
  // 已建好的子按钮也要跟主题（主题切换时本函数会重跑）。
  if (cancel_button_ != nullptr) cancel_button_->apply_theme(theme);
  if (confirm_button_ != nullptr) confirm_button_->apply_theme(theme);
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

  // —— 卡片内纵向分块（自上而下，间距按 8px 基数收敛）——
  // 标题（有才占位）/ 面包屑行(含工具钮) / 列表+侧栏 / 输入行 / 按钮行
  const TextPort& port = text_port_of(context);
  float y = card_.y + kPadding;
  const float content_width = std::max(0.0f, card_.width - kPadding * 2.0f);
  // ⚠ 无标题时不留空：旧写法无条件 `y += title_line + space_md`，标题为空时
  // 顶部白掉一整行+间距（实测 60px 空洞，看着像"内容没加载出来"）。
  if (!title_.empty()) {
    const float title_line = port.line_height(metrics.font_xl);
    y += title_line + metrics.space_md;
  }

  // 面包屑行：路径分段可点跳转（跨层导航，替代逐级 `..`）；行右端挂两个工具钮
  // （隐藏文件切换 / 新建文件夹）——它们是"目录视图"的属性，放面包屑行最顺。
  const float crumb_line = port.line_height(metrics.font_base) + metrics.space_xs * 2.0f;
  breadcrumb_ = math::Rect{card_.x + kPadding, y, content_width, crumb_line};
  const float tool_w = 30.0f;
  toggle_hidden_button_ = math::Rect{breadcrumb_.right() - tool_w * 2.0f - 4.0f,
                                     breadcrumb_.y + 2.0f, tool_w,
                                     std::max(0.0f, breadcrumb_.height - 4.0f)};
  new_folder_button_ = math::Rect{breadcrumb_.right() - tool_w, breadcrumb_.y + 2.0f, tool_w,
                                  std::max(0.0f, breadcrumb_.height - 4.0f)};
  // 面包屑段宽缓存（绘制与命中共用）。
  crumb_widths_.clear();
  for (const auto& [label, target] : crumbs()) {
    (void)target;
    crumb_widths_.push_back(port.measure_width(label, metrics.font_xs) + 16.0f);
  }
  y += crumb_line + metrics.space_sm;

  // 输入行 + 按钮行先预留（列表占余下空间）。
  const float input_height = metrics.control_height;
  const float buttons_height = metrics.control_height;
  // `Directory` 模式**没有文件名行**（文件名在该模式下不参与语义）：
  // 预留高度里不含它，列表因此多占一条输入行的空间。
  const bool has_input_row = mode_ != Mode::Directory;
  // 输入行与列表之间留 `space_md`（分组关系：输入行是独立一行操作，不贴着列表），
  // 与按钮行同基（旧值 space_sm 让输入框看起来属于列表）。
  const float bottom_reserved = (has_input_row ? input_height + metrics.space_md : 0.0f) +
                                buttons_height + kPadding;

  // 位置侧栏（左列）：主目录/盘符/宿主自定义项。窄了位置名被截、宽了列表被压，
  // 132 是"主目录/工作区"这类名字的舒适宽（不再硬编码在函数体里）。
  const std::vector<Place> all_places = places();
  const bool has_places = !all_places.empty();
  const float list_gap = has_places ? metrics.space_md : 0.0f;
  places_ = has_places ? math::Rect{card_.x + kPadding, y, kPlacesWidth,
                                    std::max(0.0f, card_.bottom() - bottom_reserved - y)}
                       : math::Rect{};

  const float list_top = y;
  const float list_bottom = card_.bottom() - bottom_reserved;
  // 列表高**对齐到整行**（不多不少）：余下的零头留在底部当内边距——
  // 不齐的话最后一行被硬切一半（多模态评审指出的"生硬裁切"）。
  const float list_left = has_places ? places_.right() + list_gap : card_.x + kPadding;
  const float list_right = card_.right() - kPadding;
  const float raw_height = std::max(0.0f, list_bottom - list_top);
  const float rows_visible = std::max(1.0f, std::floor(raw_height / kRowHeight));
  list_ = math::Rect{list_left, list_top, std::max(0.0f, list_right - list_left),
                     rows_visible * kRowHeight};
  // 侧栏与列表同高（两列底部对齐才不会"一个到底一个悬空"）。
  if (has_places) places_.height = list_.height;

  input_ = has_input_row
               ? math::Rect{card_.x + kPadding, list_.bottom() + metrics.space_md, content_width,
                            input_height}
               : math::Rect{};

  // —— 按钮行（子 Button；右对齐：取消在左、确认在右） ——
  if (confirm_button_ == nullptr) {
    auto cancel = std::make_unique<Button>("取消", Button::Variant::Secondary, Button::Size::Medium);
    cancel->on_click = [this]() {
      if (on_cancel) on_cancel();
    };
    auto ok = std::make_unique<Button>(
          mode_ == Mode::Save ? "保存" : (mode_ == Mode::Directory ? "选择此文件夹" : "打开"),
          Button::Variant::Primary, Button::Size::Medium);
    ok->on_click = [this]() { FileDialog::confirm(); };
    cancel_button_ = add_child(std::move(cancel));
    confirm_button_ = add_child(std::move(ok));
    // ⚠ **新子元素要立即补一次 `apply_theme`**：`UiRoot::layout` 的主题下发
    //（`apply_theme_tree`）在本帧的 `arrange` **之前**已跑完，而按钮是这里才建的——
    // 不补的话它们的 `style_` 停在默认值（实测：主按钮无主色底、次按钮无描边，
    // 两个都成了"裸文字"，用户看不出该点哪个）。
    if (theme_valid_) {
      cancel_button_->apply_theme(last_theme_);
      confirm_button_->apply_theme(last_theme_);
    }
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

auto FileDialog::crumb_rect(std::size_t index) const noexcept -> math::Rect {
  if (breadcrumb_.is_empty() || index >= crumb_widths_.size()) return math::Rect{};
  // 从左往右排，段间以 `›` 分隔（绘制与命中同一几何；工具钮占了行右端，
  // 可用宽到工具钮左缘为止——超出部分命中不到也画不全，与视觉一致）。
  const float available = toggle_hidden_button_.is_empty()
      ? breadcrumb_.width : (toggle_hidden_button_.x - breadcrumb_.x - 8.0f);
  float x = breadcrumb_.x;
  for (std::size_t at = 0; at < index && at < crumb_widths_.size(); ++at) {
    x += crumb_widths_[at] + 10.0f;   // 段宽 + 分隔符占位
  }
  if (x > breadcrumb_.x + available) return math::Rect{};   // 段已在可用区外
  const float width = std::min(crumb_widths_[index],
                               breadcrumb_.x + available - x);
  return math::Rect{x, breadcrumb_.y, width, breadcrumb_.height};
}

auto FileDialog::place_rect(std::size_t index) const noexcept -> math::Rect {
  if (places_.is_empty()) return math::Rect{};
  // 首行是"位置"小标题（见绘制），位置项从它下面开始。
  constexpr float kLabelBand = 22.0f;
  const float y = places_.y + kLabelBand + kRowHeight * static_cast<float>(index);
  if (y + kRowHeight > places_.bottom()) return math::Rect{};   // 超出侧栏高
  return math::Rect{places_.x, y, places_.width, kRowHeight};
}

auto FileDialog::toolbar_toggle_hidden_rect() const noexcept -> math::Rect {
  return toggle_hidden_button_;
}

auto FileDialog::toolbar_new_folder_rect() const noexcept -> math::Rect {
  return new_folder_button_;
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

  // 面包屑行：路径分段可点（末段是当前位置，主色、不可点；其余弱色 hover 主色）。
  const float crumb_line = port.line_height(metrics.font_base) + metrics.space_xs * 2.0f;
  const auto crumb_list = crumbs();
  for (std::size_t index = 0; index < crumb_list.size(); ++index) {
    const math::Rect seg = crumb_rect(index);
    if (seg.is_empty()) continue;
    const bool last = index + 1 == crumb_list.size();
    draw_line(context, canvas, crumb_list[index].first,
              math::Rect{seg.x, seg.y, seg.width, seg.height}, metrics.font_xs,
              last ? colors.text : colors.text_muted);
    // 分隔符 `›`（除末段）。
    if (!last) {
      draw_line(context, canvas, "›",
                math::Rect{seg.right(), seg.y, 10.0f, seg.height}, metrics.font_xs,
                colors.text_faint);
    }
  }
  // 工具钮：隐藏切换（`eye`/`eye-off`）+ 新建文件夹（`folder` + `+`）。
  // 用**内置矢量图标集**（与列表箭头/活动栏同一笔法）——自绘线条图标的笔画粗细/比例
  // 与组件库不一致，在对话框里一眼看出"不是同一套"。
  // 颜色用 `text`（不是 `text_muted`）：评审指出"图标过小过淡、看着像禁用"。
  {
    const math::Rect eye = toggle_hidden_button_;
    if (!eye.is_empty()) {
      const float size = std::min(eye.width, eye.height) * 0.7f;
      const math::Rect box{eye.center().x - size * 0.5f, eye.center().y - size * 0.5f, size,
                           size};
      Icon::draw(canvas, show_hidden_ ? "eye-off" : "eye", box,
                 show_hidden_ ? colors.primary : colors.text, 1.7f);
    }
    const math::Rect folder = new_folder_button_;
    if (!folder.is_empty()) {
      const float size = std::min(folder.width, folder.height) * 0.7f;
      const math::Rect box{folder.center().x - size * 0.5f, folder.center().y - size * 0.5f,
                           size, size};
      Icon::draw(canvas, "folder", box, colors.text, 1.7f);
      // 右上角 `+`（只给文件夹图标会让"新建"的含义靠用户猜）。
      const math::Point tip{box.right() - 1.0f, box.y + 1.0f};
      raster::Path plus;
      plus.move_to(math::Point{tip.x - 4.0f, tip.y});
      plus.line_to(math::Point{tip.x + 4.0f, tip.y});
      plus.move_to(math::Point{tip.x, tip.y - 4.0f});
      plus.line_to(math::Point{tip.x, tip.y + 4.0f});
      canvas.stroke_path(plus, raster::Paint::solid(colors.primary), 1.8f);
    }
  }
  y += crumb_line + metrics.space_sm;

  // 错误行优先呈现（fs 失败时列表区只画错误）。
  if (!error_.empty()) {
    draw_line(context, canvas, error_,
              math::Rect{list_.x, list_.y, list_.width, port.line_height(metrics.font_base)},
              metrics.font_base, colors.danger);
    return;
  }

  // —— 位置侧栏（左列；有位置项时）——
  // 不画独立圆角卡片（"卡片里再套两张卡"是多模态评审指出的割裂感来源）：
  // 整列平底 + 右侧 1px 分隔线，与列表同一高度、同一语言。
  if (!places_.is_empty()) {
    fill_round_rect(canvas, places_, metrics.radius_md, colors.surface_alt);
    // 右侧分隔线：用 `border_strong`（`border` 太淡，评审指出"分界不够硬朗"）。
    canvas.fill_rect(math::Rect{places_.right() - 1.0f, places_.y + 4.0f, 1.0f,
                                std::max(0.0f, places_.height - 8.0f)},
                     raster::Paint::solid(colors.border_strong));
    // 小标题（"位置"）——左列没有标题会像一排无名的路径行。
    const float label_h = port.line_height(metrics.font_xs) + metrics.space_xs;
    draw_line(context, canvas, "位置",
              math::Rect{places_.x + 12.0f, places_.y + 6.0f, places_.width - 24.0f, label_h},
              metrics.font_xs, colors.text_faint);
    const std::vector<Place> shown = places();
    for (std::size_t index = 0; index < shown.size(); ++index) {
      const math::Rect row = place_rect(index);
      if (row.is_empty()) continue;
      // 激活判定：**当前目录等于或在其之下**——只看相等的话，用户进了
      // "主目录/子目录"后侧栏就不再提示"你在这个位置里"（导航上下文丢失）。
      const std::string here = st::fs::normalize(directory_);
      const std::string place = st::fs::normalize(shown[index].path);
      const bool active = here == place || here.rfind(place + "/", 0) == 0;
      if (active) {
        fill_round_rect(canvas, row.inset(math::Insets{3.0f, 6.0f, 3.0f, 6.0f}), metrics.radius_sm,
                        colors.primary_soft);
      }
      Icon::draw(canvas, "folder",
                 math::Rect{row.x + 12.0f, row.y + (row.height - 14.0f) * 0.5f, 14.0f, 14.0f},
                 active ? colors.primary : colors.text, 1.5f);
      draw_line(context, canvas, shown[index].label,
                math::Rect{row.x + 32.0f, row.y, row.width - 38.0f, row.height},
                metrics.font_xs, active ? colors.primary : colors.text_muted);
    }
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

  const std::size_t total = visible_.size();   // 可见集合（过滤/隐藏后）
  for (std::size_t index = 0; index < total; ++index) {
    const math::Rect row = entry_rect(index);
    if (row.bottom() < list_.y || row.y > list_.bottom()) continue;  // 视口剔除
    const st::fs::DirEntry& entry = entries_[visible_[index]];
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

  // 列表滚动条（内容超出时）：比 `ScrollView` 的 8px 窄、但用 `text_faint` 提亮——
  // 评审指出旧版"太细太淡、可发现性弱"（8px 宽但 `border_strong` 在浅灰列表底上几乎看不见）。
  {
    const float content_h = list_content_height();
    if (content_h > list_.height + 0.5f) {
      const float ratio = std::clamp(list_.height / content_h, 0.08f, 1.0f);
      const float thumb_h = std::clamp(list_.height * ratio, 24.0f, list_.height);
      const float max_scroll = content_h - list_.height;
      const float travel = list_.height - thumb_h;
      const float thumb_y = list_.y + (scroll_y_ / max_scroll) * travel;
      fill_round_rect(canvas, math::Rect{list_.right() - 9.0f, thumb_y, 5.0f, thumb_h},
                      metrics.radius_pill, colors.text_faint);
    }
  }

  // —— 文件名输入行（自绘：边框 + 底 + "文件名" 占位 + 文本 + 光标） ——
  // `Directory` 模式没有这一行（`input_` 为空，`is_empty()` 即判据）。
  // 视觉与 `Input` 组件同源：surface 底 + 1px 描边 + 左侧标签（没描边与标签时
  // 用户会把它误读为装饰条/禁用态——多模态评审实测指出）。
  if (!input_.is_empty()) {
    const bool focused_row = input_focused_;
    fill_round_rect(canvas, input_, metrics.radius_md, colors.surface);
    raster::Path border;
    border.add_rounded_rect(input_, metrics.radius_md);
    canvas.stroke_path(border,
                       raster::Paint::solid(focused_row ? colors.primary : colors.border),
                       focused_row ? std::max(metrics.border_width * 2.0f, 2.0f)
                                   : metrics.border_width);
    // 输入区文本：空时用**单个**引导文案（"文件名…"）——旧版把"文件名"标签与
    // 占位并排，两个词挤在一个框里显冗余（评审指出）。有值时标签不占位。
    const math::Rect input_text{input_.x + metrics.space_sm, input_.y,
                               std::max(0.0f, input_.width - metrics.space_sm * 2.0f),
                               input_.height};
    if (filename_.empty()) {
      draw_line(context, canvas, "文件名", input_text, metrics.font_base, colors.text_faint);
    } else {
      draw_line(context, canvas, filename_, input_text, metrics.font_base, colors.text);
    }
    if (input_focused_) {
      const float text_width = port.measure_width(filename_, metrics.font_base);
      const float caret_x = input_text.x + std::min(text_width, input_text.width - 1.0f);
      canvas.fill_rect(math::Rect{caret_x, input_.y + 5.0f, 1.0f, input_.height - 10.0f},
                       raster::Paint::solid(colors.primary));
    }
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
      if (event.key == "Home") {
        // 列表首项（跨平台惯例；文件名行聚焦时不动——那是文本编辑的 Home）。
        if (!input_focused_ && !visible_.empty()) {
          selected_ = 0;
          ensure_selected_visible();
          mark_dirty();
        }
        return true;
      }
      if (event.key == "End") {
        if (!input_focused_ && !visible_.empty()) {
          selected_ = visible_.size() - 1;
          ensure_selected_visible();
          mark_dirty();
        }
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
      if (event.ctrl && event.shift && (event.key == "n" || event.key == "N")) {
        // Ctrl+Shift+N：新建文件夹（Windows/macOS 桌面环境同款键位）。
        (void)create_folder();
        return true;
      }
      if (event.ctrl && (event.key == "h" || event.key == "H")) {
        // Ctrl+H：隐藏文件显隐切换（多数 Linux 文件管理器的键位）。
        set_show_hidden(!show_hidden_);
        return true;
      }
      return false;
    }
    case EventKind::TextInput:
      // 输入行聚焦时字符进入文件名（未聚焦也接受——单输入位的对话框惯例）
      // `Directory` 模式**没有文件名行**：不接受字符，免得用户对着一个不存在的
      // 输入位打字而界面上毫无反馈。
      if (mode_ == Mode::Directory) return true;
      input_insert(event.text);
      return true;
    case EventKind::Wheel: {
      // 列表纵向滚动（自绘行）。
      const float max_scroll = std::max(0.0f, list_content_height() - list_.height);
      scroll_y_ = std::clamp(scroll_y_ + event.wheel_delta * 48.0f, 0.0f, max_scroll);
      mark_dirty();
      return true;
    }
    case EventKind::MouseDown:
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      if (!card_.is_empty() && card_.contains(event.position)) {
        // 工具钮（面包屑行右端）：Click 才触发（按下/抬起只吞）。
        if (event.kind == EventKind::Click) {
          if (!toggle_hidden_button_.is_empty() &&
              toggle_hidden_button_.contains(event.position)) {
            set_show_hidden(!show_hidden_);
            return true;
          }
          if (!new_folder_button_.is_empty() &&
              new_folder_button_.contains(event.position)) {
            (void)create_folder();
            return true;
          }
        }
        // 面包屑段：点非末段跳转（末段是当前位置）。
        if (event.kind == EventKind::Click) {
          const auto crumb_list = crumbs();
          for (std::size_t index = 0; index + 1 < crumb_list.size(); ++index) {
            const math::Rect seg = crumb_rect(index);
            if (!seg.is_empty() && seg.contains(event.position)) {
              set_directory(crumb_list[index].second);
              return true;
            }
          }
        }
        // 位置侧栏项：点击跳转。
        if (event.kind == EventKind::Click && !places_.is_empty() &&
            places_.contains(event.position)) {
          const std::vector<Place> shown = places();
          for (std::size_t index = 0; index < shown.size(); ++index) {
            const math::Rect row = place_rect(index);
            if (!row.is_empty() && row.contains(event.position)) {
              set_directory(shown[index].path);
              return true;
            }
          }
          return true;   // 侧栏空白：吞掉
        }
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
        for (std::size_t index = 0; index < visible_.size(); ++index) {
          const math::Rect row = entry_rect(index);
          if (row.is_empty() || !row.contains(event.position)) continue;
          selected_ = index;
          const st::fs::DirEntry* entry = FileDialog::entry(index);
          if (event.kind == EventKind::DoubleClick ||
              event.kind == EventKind::TripleClick) {
            activate_entry(index);  // 双击：目录进入/文件确认
          } else if (entry != nullptr && !entry->is_dir) {
            filename_ = entry->name;
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
  if (name == "mode") {
    return std::string(mode_ == Mode::Save   ? "save"
                       : mode_ == Mode::Directory ? "directory"
                                                  : "open");
  }
  if (name == "error") return error_;
  if (name == "selected") {
    const st::fs::DirEntry* entry = selected_entry();
    return entry != nullptr ? entry->name : std::string();
  }
  if (name == "show_hidden") return show_hidden_ ? std::string("true") : std::string("false");
  if (name == "filters") {
    std::string joined;
    for (const auto& ext : name_filters_) {
      if (!joined.empty()) joined.push_back('|');
      joined += ext;
    }
    return joined;
  }
  if (name == "places") {
    std::string joined;
    for (const Place& place : places()) {
      if (!joined.empty()) joined.push_back('|');
      joined += place.label + "=" + place.path;
    }
    return joined;
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
  if (name == "show_hidden") {
    set_show_hidden(value == "true" || value == "1");
    return true;
  }
  if (name == "filters") {
    // `|` 分隔的后缀清单（`.cpp|.md`；空串 = 清空过滤）。
    std::vector<std::string> parts;
    for (const auto& part : st::split(value, '|')) {
      parts.emplace_back(part);
    }
    set_name_filters(std::move(parts));
    return true;
  }
  return Element::set_property(name, value);
}

auto FileDialog::property_names() const -> std::vector<std::string_view> {
  std::vector<std::string_view> names = Element::property_names();
  names.insert(names.end(),
               {"directory", "filename", "mode", "error", "selected", "pending_path",
                "show_hidden", "filters", "places"});
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
  if (action == "new_folder" || action == "mkdir") {
    // 新建文件夹（参数 = 名字，缺省默认名；返回失败时空串——动作面仍 true，
    // 失败细节看 `directory` 是否变化/`error`）。
    (void)create_folder(argument.empty() ? std::string("新建文件夹") : std::string(argument));
    return true;
  }
  if (action == "toggle_hidden") {
    set_show_hidden(!show_hidden_);
    return true;
  }
  if (action == "goto_crumb" || action == "crumb") {
    // 面包屑跳转（参数 = 段序号）。
    const auto parsed = st::parse_u64(argument);
    if (!parsed.has_value()) return false;
    const auto crumb_list = crumbs();
    if (*parsed >= crumb_list.size()) return false;
    set_directory(crumb_list[*parsed].second);
    return true;
  }
  if (action == "goto_place" || action == "place") {
    // 位置侧栏跳转（参数 = 项序号）。
    const auto parsed = st::parse_u64(argument);
    if (!parsed.has_value()) return false;
    const std::vector<Place> shown = places();
    if (*parsed >= shown.size()) return false;
    set_directory(shown[*parsed].path);
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
