#include "st/ui/components/mind_map_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/edit/undo.hpp"
#include "st/math/path.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/components/input_box.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/wheel.hpp"

#include "components_internal.hpp"

namespace st::ui {
namespace {

using md::mindmap::LayoutDirection;
using md::mindmap::Node;

constexpr std::size_t kNoNode = Node::kNoNode;

/// 鼠标左键的键位（与 `MermaidView` 同一常量口径：右键/中键在宿主里通常另有语义）。
constexpr int kLeftButton{1};

/// 线宽（逻辑像素）。与 `MermaidView` 同档：0.5 的倍数在两条渲染腿上都不碎。
constexpr float kLinkWidth = 1.6f;
constexpr float kFrameWidth = 1.0f;
constexpr float kNodeRadius = 8.0f;
constexpr float kRootRadius = 12.0f;
/// 折叠徽标半径（相对字号）。
constexpr float kBadgeRadiusRatio = 0.42f;

/// 三次贝塞尔求值（连线与命中都用它，避免两套公式）。
auto bezier_at(math::Point p0, math::Point p1, math::Point p2, math::Point p3, float t)
    -> math::Point {
  const float u = 1.0f - t;
  const float a = u * u * u;
  const float b = 3.0f * u * u * t;
  const float c = 3.0f * u * t * t;
  const float d = t * t * t;
  return math::Point{a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                     a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

/// 把一条贝塞尔切成折线（光栅层没有曲线描边的 dash，但更重要的是：
/// 曲线描边在两条渲染腿上的抗锯齿不同，折线+密集采样在两边更一致）。
void stroke_bezier(raster::Surface& canvas, math::Point p0, math::Point p1, math::Point p2,
                   math::Point p3, math::Color color, float width) {
  // 采样密度按**长度**定：短连线少画几段（省），长连线多画（不显折角）。
  const float rough = std::abs(p3.x - p0.x) + std::abs(p3.y - p0.y);
  const int segments = std::clamp(static_cast<int>(rough / 6.0f) + 8, 10, 64);
  math::Path path;
  path.move_to(p0);
  for (int index = 1; index <= segments; ++index) {
    const float t = static_cast<float>(index) / static_cast<float>(segments);
    path.line_to(bezier_at(p0, p1, p2, p3, t));
  }
  canvas.stroke_path(path, raster::Paint::solid(color), width);
}

/// 按**一级分支**取色：同一个一级祖先下的所有后代同色。
///
/// 为何不能用 `level_color(…, depth)`（实测踩到的缺陷）：按深度着色会让
/// **同一层的所有节点同色**——截图里“框架内核”与“应用生态”都是绿色，
/// 读者完全看不出“哪些叶子属于哪一支”。而思维导图的颜色存在的唯一理由
/// 就是标分支归属，按深度上色等于把这个信息丢掉。
[[nodiscard]] auto branch_color(const md::mindmap::Document& document,
                                const std::vector<math::Color>& palette, std::size_t index,
                                math::Color fallback, math::Color root_color) -> math::Color {
  if (!document.valid() || palette.empty()) return fallback;
  if (index == document.root()) return root_color;
  // 往上爬到根的直系子节点（即所属的一级分支）。
  std::size_t cursor = index;
  std::size_t guard = document.node_count() + 1U;
  while (guard > 0U) {
    --guard;
    const md::mindmap::Node* node = document.node(cursor);
    if (node == nullptr) return fallback;
    if (node->parent == document.root()) break;
    if (node->parent == md::mindmap::Node::kNoNode) return fallback;
    cursor = node->parent;
  }
  const md::mindmap::Node* root = document.node(document.root());
  if (root == nullptr) return fallback;
  for (std::size_t ordinal = 0; ordinal < root->children.size(); ++ordinal) {
    if (root->children[ordinal] == cursor) return palette[ordinal % palette.size()];
  }
  return fallback;
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 快照命令
// ————————————————————————————————————————————————————————————————————————————

MindMapView::SnapshotCommand::SnapshotCommand(md::mindmap::Document before,
                                               md::mindmap::Document after, std::string name)
    : before_(std::move(before)), after_(std::move(after)), name_(std::move(name)) {}

void MindMapView::SnapshotCommand::undo(edit::CommandContext& context) {
  auto& view = edit::context_as<MindMapView>(context);
  view.document_ = before_;
  // 撤销后下标可能已失效（删子树会让数组变短）——夹到合法范围，而不是留一个
  // 指向"别处"的选中下标（那会让下一次编辑改到错误节点，且没有任何报错）。
  if (view.selected_ >= view.document_.node_count()) {
    view.selected_ = view.document_.valid() ? view.document_.root() : kNoNode;
  }
  view.relayout();
  view.dirty_ = true;
  view.mark_dirty();
  if (view.on_change) view.on_change();
}

void MindMapView::SnapshotCommand::redo(edit::CommandContext& context) {
  auto& view = edit::context_as<MindMapView>(context);
  view.document_ = after_;
  if (view.selected_ >= view.document_.node_count()) {
    view.selected_ = view.document_.valid() ? view.document_.root() : kNoNode;
  }
  view.relayout();
  view.dirty_ = true;
  view.mark_dirty();
  if (view.on_change) view.on_change();
}

auto MindMapView::SnapshotCommand::name() const -> std::string { return name_; }

auto MindMapView::SnapshotCommand::is_empty() const -> bool {
  // 逐字节比对**序列化结果**而不是比结构：结构层面"改了又改回去"也是两个不同文档，
  // 但用户看到的一模一样——那种"撤销了但画面没变"的历史条目正是这条要防的。
  return before_.to_freemind(false) == after_.to_freemind(false);
}

// ————————————————————————————————————————————————————————————————————————————
// 构造与内容
// ————————————————————————————————————————————————————————————————————————————

MindMapView::MindMapView()
    : document_(std::string("中心主题")), undo_(*this) {
  // 自适应尺寸：它没有"内容自然尺寸"可推（布局依赖字体），所以默认吃满可用区域。
  // 显式给 `style().height` 时按给定值（常见用法：卡片里固定高度 320）。
  style_.grow = true;
  set_focusable(true);
  selected_ = document_.valid() ? document_.root() : kNoNode;
  relayout();
}

MindMapView::~MindMapView() = default;

void MindMapView::set_document(md::mindmap::Document document) {
  // 换文档 = 打开另一份文件：**清空撤销历史**。保留它会让"撤销"把用户的编辑
  // 施加到上一份文档上（跨文档回滚），那是数据损坏而不是手感问题。
  finish_rename(false);
  document_ = std::move(document);
  undo_.clear();
  selected_ = document_.valid() ? document_.root() : kNoNode;
  last_error_.clear();
  dirty_ = false;
  fit_ = true;      // 换了文档，用户想看的是"新文档全貌"
  zoom_ = 1.0f;
  pan_x_ = 0.0f;
  pan_y_ = 0.0f;
  relayout();
  mark_dirty();
}

void MindMapView::set_source(std::string_view text) {
  md::mindmap::Document built;
  // 第一行当根，其余行按**缩进的空格数**判层级（每 2 空格算一层）。
  //
  // 为什么不按"第几行"或固定格式：XML/JS 侧传过来的就是人写的文本，
  // 而缩进是最通用的树形表达（YAML/markdown 列表同一套直觉）。
  std::vector<std::size_t> stack;   // 每层的父节点下标
  bool has_root = false;
  std::size_t line_count = 0;
  for (const std::string_view raw : st::split(std::string(text), '\n')) {
    const std::string_view line = st::trim(raw);
    if (line.empty()) continue;
    ++line_count;
    // 缩进宽度（制表符算 2 空格，与多数编辑器默认一致）
    std::size_t indent = 0;
    for (const char c : raw) {
      if (c == ' ') {
        ++indent;
      } else if (c == '\t') {
        indent += 2;
      } else {
        break;
      }
    }
    const std::size_t level = indent / 2U;
    if (!has_root) {
      built = md::mindmap::Document(std::string(line));
      stack.assign(1, built.root());
      has_root = true;
      continue;
    }
    // 夹取目标层：缩进跳级（0 → 3 层）时挂到最深的已知父级下，而不是拒绝整行。
    const std::size_t depth = std::min(level, stack.size());
    if (depth == 0U) {
      // 又一个"根层"的行：作为根的子节点（多数导图确实有多条一级分支）。
      const std::size_t created = built.add_child(built.root(), std::string(line));
      stack.resize(1);
      stack.push_back(created);
      continue;
    }
    const std::size_t parent = stack[depth - 1U];
    const std::size_t created = built.add_child(parent, std::string(line));
    stack.resize(depth);
    stack.push_back(created);
  }
  if (!has_root) built = md::mindmap::Document(std::string("中心主题"));
  (void)line_count;
  set_document(std::move(built));
}

auto MindMapView::source() const -> std::string {
  std::string out;
  const auto emit = [&](auto&& self, std::size_t index) -> void {
    const Node* node = document_.node(index);
    if (node == nullptr) return;
    out.append(document_.depth_of(index) * 2U, ' ');
    out += node->text;
    out.push_back('\n');
    for (const std::size_t child : node->children) self(self, child);
  };
  if (document_.valid()) emit(emit, document_.root());
  return out;
}

// ————————————————————————————————————————————————————————————————————————————
// 选中与编辑
// ————————————————————————————————————————————————————————————————————————————

void MindMapView::set_selected(std::size_t index) {
  if (index >= document_.node_count()) index = kNoNode;
  if (selected_ == index) return;
  selected_ = index;
  mark_dirty();
}

auto MindMapView::selected_node() const noexcept -> const Node* { return document_.node(selected_); }

auto MindMapView::apply_edit(std::string_view name,
                             const std::function<bool(md::mindmap::Document&)>& mutate) -> bool {
  if (!document_.valid()) return false;
  md::mindmap::Document before = document_;
  if (!mutate(document_)) return false;   // 没改动：不产生历史条目（"空命令不入栈"）
  relayout();
  dirty_ = true;
  // 用 push（而非 apply）：改动已经落定，命令只负责把两份快照换回去。
  (void)undo_.push(std::make_unique<SnapshotCommand>(std::move(before), document_,
                                                     std::string(name)));
  mark_dirty();
  if (on_change) on_change();
  return true;
}

auto MindMapView::begin_rename(std::size_t index) -> bool {
  if (!editable_) return false;
  const Node* node = document_.node(index);
  if (node == nullptr) return false;
  if (renaming_ == index) return true;   // 已在改名：幂等
  finish_rename(false);
  renaming_ = index;
  // 改名期间把节点展开到可见（否则会在一个"看不见的框"上打字）。
  attach_rename_input();
  mark_dirty();
  return true;
}

// ————————————————————————————————————————————————————————————————————————————
// 内嵌改名：生命周期
// ————————————————————————————————————————————————————————————————————————————
//
// ## 为何“摘下来先存着，等下一帧再释放”（实测崩溃）
//
// 提交路径是：`Input::activate()` → `on_submit` → 本组件的 `commit_rename` →
// `detach_rename_input`。此刻**正在执行的就是那个 `Input` 的成员函数**——
// 当场销毁它等于让 `activate()` 在返回途中使用已释放的 `this`，而控制通道
// 紧接着还要读它的属性面（构建 `invoke` 回包）。
//
// 实测现象：`invoke submit` 让整个应用退出（无任何 C++ 异常信息），
// 而单元测试直接调 `commit_rename` 却是绿的——因为那条路不在派发栈里。
// 这正是“只有真实调用路径能发现”的那类缺陷。
//
// 框架的 `UiRoot::retire_subtree` 只处理**声明式重组**的退役，这里是自己摘子元素，
// 所以由本组件自己押一下：摘下来的 `unique_ptr` 存到 `retiring_input_`，
// 在下一次事件或绘制时释放（那时调用栈已完全退出，无人在用它）。

void MindMapView::flush_retired_input() { retiring_input_.reset(); }

auto MindMapView::commit_rename(std::string text) -> bool {
  const std::size_t index = renaming_;
  detach_rename_input();
  if (index == kNoNode) return false;
  const bool changed = apply_edit("改文字", [&](md::mindmap::Document& doc) {
    return doc.set_text(index, std::move(text));
  });
  mark_dirty();
  return changed;
}

void MindMapView::cancel_rename() {
  detach_rename_input();
  mark_dirty();
}

void MindMapView::finish_rename(bool commit) {
  if (renaming_ == kNoNode) return;
  if (commit) {
    if (rename_input_ != nullptr) {
      (void)commit_rename(rename_input_->value());
      return;
    }
    // 输入框已不在（被外部摘掉）：按"取消"处理，而不是提交一个空串。
    cancel_rename();
    return;
  }
  cancel_rename();
}

void MindMapView::attach_rename_input() {
  auto field = std::make_unique<Input>();
  // id 要给唯一前缀：同一个页面里可能有好几个导图，而内嵌框是**运行期才建**的
  // （不在声明式树里，拿不到自动 id 的去重）。
  const std::string prefix = id().empty() ? std::string("mindmap") : id();
  field->set_id(prefix + "/rename");
  field->set_text(document_.text_of(renaming_));
  field->style().absolute = true;
  field->style().font_size = options_.font_size;
  field->style().radius = kNodeRadius * 0.5f;
  Input* raw = field.get();
  add_child(std::move(field));
  rename_input_ = raw;

  // 选中全文：进改名时用户几乎总是要整段替换（而不是在末尾接着打）。
  raw->set_selection(0, document_.text_of(renaming_).size());
  // `Enter` → `on_submit` → 提交；`Escape` 由本组件的 `on_event` 兜底（`Input` 不处理它）。
  raw->on_submit = [this](std::string_view value) {
    // ⚠ 这个 lambda 是在那个 `Input` 自己的 `activate()` 里被调的。
    // 所有需要的值都在 `value` 里，**不要碰 `rename_input_`**（`commit_rename`
    // 会把它搞下来并押到退役区）。
    (void)commit_rename(std::string(value));
  };
  if (auto* host = this->host(); host != nullptr) {
    (void)host->set_keyboard_focus(raw);
  } else {
    raw->set_focused(true);
  }
  place_rename_input();
}

void MindMapView::detach_rename_input() {
  renaming_ = kNoNode;
  Input* field = rename_input_;
  rename_input_ = nullptr;
  rename_anchor_ = math::Rect{};
  if (field == nullptr) return;
  // 先清回调：摘下来之后它不再参与任何派发，但回调里的 `this` 捕获取消掉更稳。
  field->on_submit = nullptr;
  if (field->parent() != this) return;
  // 押到退役区，**不在这里销毁**（见上面的说明：可能正处在它自己的派发栈里）。
  // 若上一轮还押着一个没来得及释放，先释放旧的（那时早已过了它的派发）。
  retiring_input_.reset();
  retiring_input_ = remove_child(field);
}


void MindMapView::place_rename_input() {
  if (rename_input_ == nullptr) return;
  if (renaming_ >= layout_.boxes.size()) return;
  const math::Rect box = layout_.boxes[renaming_].rect;
  if (box.is_empty()) {
    rename_input_->set_visible(false);
    return;
  }
  const math::Point origin = content_origin();
  const math::Rect screen{origin.x + box.x * scale_, origin.y + box.y * scale_,
                          std::max(box.width * scale_, 60.0f),
                          std::max(box.height * scale_, line_height_)};
  rename_input_->set_visible(true);
  // 换位置只在真变了时写：`set_visible`/样式写入都会标脏，每帧写会让增量重绘作废。
  Style& style = rename_input_->style();
  if (style.width != screen.width) style.width = screen.width;
  if (style.height != screen.height) style.height = screen.height;
  const float local_x = screen.x - bounds_.x;
  const float local_y = screen.y - bounds_.y;
  const bool moved = style.left != local_x || style.top != local_y;
  style.left = local_x;
  style.top = local_y;
  if (moved) rename_input_->mark_dirty();
  rename_anchor_ = box;
}

// ————————————————————————————————————————————————————————————————————————————
// 文件读写
// ————————————————————————————————————————————————————————————————————————————

void MindMapView::set_file_path(std::string path) {
  file_path_ = std::move(path);
}

auto MindMapView::load_file(std::string_view path) -> Status {
  const auto text = st::fs::read_text(path);
  if (!text.has_value()) {
    last_error_ = std::format("读不到文件 {}：{}", std::string(path), text.error().message);
    mark_dirty();
    return st::forward_error(text.error());
  }
  auto parsed = md::mindmap::Document::from_freemind(*text);
  if (!parsed.has_value()) {
    last_error_ = std::format("解析失败 {}：{}", std::string(path), parsed.error().message);
    mark_dirty();
    return st::forward_error(parsed.error());
  }
  set_document(std::move(*parsed));
  file_path_ = std::string(path);
  last_error_.clear();
  dirty_ = false;
  mark_dirty();
  return st::ok();
}

auto MindMapView::save_file(std::string_view path) -> Status {
  if (!document_.valid()) {
    last_error_ = "没有可保存的内容（文档为空）";
    mark_dirty();
    return st::unexpected(ErrorCode::Invalid, last_error_);
  }
  const std::string text = document_.to_freemind(true);
  // 先确保父目录存在：用户选了新目录下的文件名时，否则会得到一个语焉不详的 Io 错误。
  if (const std::string parent = st::fs::parent(path); !parent.empty()) {
    if (!st::fs::exists(parent)) {
      const Status created = st::fs::create_directories(parent);
      if (!created.has_value()) {
        last_error_ = std::format("建不了目录 {}：{}", parent, created.error().message);
        mark_dirty();
        return st::forward_error(created.error());
      }
    }
  }
  const Status written = st::fs::write_text(path, text);
  if (!written.has_value()) {
    last_error_ = std::format("写不了文件 {}：{}", std::string(path), written.error().message);
    mark_dirty();
    return st::forward_error(written.error());
  }
  file_path_ = std::string(path);
  last_error_.clear();
  dirty_ = false;
  mark_dirty();
  return st::ok();
}

auto MindMapView::load() -> Status {
  if (file_path_.empty()) {
    last_error_ = "还没有设定文件路径（先 set file=… 或调用 load 时带参数）";
    mark_dirty();
    return st::unexpected(ErrorCode::Invalid, last_error_);
  }
  return load_file(file_path_);
}

auto MindMapView::save() -> Status {
  if (file_path_.empty()) {
    // 没路径：不是错误——把内容交给应用（它可能弹保存框）。
    // 但要如实说明"没写成"，否则调用方会以为已经落盘。
    last_error_ = "没有文件路径：已请求应用选择保存位置";
    if (on_save_requested) on_save_requested(std::string("未命名.mm"), document_.to_freemind(true));
    mark_dirty();
    return st::unexpected(ErrorCode::Invalid, last_error_);
  }
  const Status result = save_file(file_path_);
  if (!result.has_value() && on_save_requested) {
    on_save_requested(st::fs::file_name(file_path_), document_.to_freemind(true));
  }
  return result;
}

// ————————————————————————————————————————————————————————————————————————————
// 布局与视图
// ————————————————————————————————————————————————————————————————————————————

void MindMapView::rebuild_layout(const md::mindmap::MeasureText& measure) {
  layout_ = md::mindmap::layout(document_, options_, measure);
  content_size_ = layout_.size;
  line_height_ = options_.font_size * options_.line_gap;
  update_view();
  // 改名框要跟着新几何走（改名期间用户可能同时按了折叠/加节点）。
  if (rename_input_ != nullptr) place_rename_input();
}

void MindMapView::relayout() {
  // 用**估算度量**重排：真度量要 `RenderContext`，而编辑动作（键盘/动作面）里拿不到。
  //
  // 这与 `MermaidView` 是同一条取舍：估算只用于"几何大致对"，下一帧 `measure`
  // 会用真实端口再排一次。两处走**同一个** `rebuild_layout` 入口，所以公式只有一份。
  const float estimate = options_.font_size * 0.62f;
  rebuild_layout([estimate](std::string_view text, float) {
    return estimate * static_cast<float>(st::utf8_length(text));
  });
}

auto MindMapView::content_origin() const -> math::Point {
  const float scaled_width = content_size_.width * scale_;
  const float scaled_height = content_size_.height * scale_;
  return math::Point{bounds_.x + (bounds_.width - scaled_width) * 0.5f + pan_x_,
                     bounds_.y + (bounds_.height - scaled_height) * 0.5f + pan_y_};
}

auto MindMapView::to_content(math::Point screen) const -> math::Point {
  const math::Point origin = content_origin();
  if (scale_ <= 0.0001f) return math::Point{0.0f, 0.0f};
  return math::Point{(screen.x - origin.x) / scale_, (screen.y - origin.y) / scale_};
}

void MindMapView::update_view() {
  float fit_scale = 1.0f;
  if (fit_ && !bounds_.is_empty() && content_size_.width > 0.0f && content_size_.height > 0.0f) {
    // **只缩不放**：小图放大到撑满会把 1px 线画成 3px 粗，看着像低清预览
    //（与 `MermaidView` 同一条结论）。
    fit_scale = std::min(bounds_.width / content_size_.width,
                         bounds_.height / content_size_.height);
    fit_scale = std::min(fit_scale, 1.0f);
    fit_scale = std::max(fit_scale, 0.05f);
  }
  scale_ = fit_scale * zoom_;
}

void MindMapView::set_zoom(float zoom) {
  const float clamped = std::clamp(zoom, kMinZoom, kMaxZoom);
  if (std::abs(clamped - zoom_) < 1e-6f) return;
  zoom_ = clamped;
  update_view();
  if (rename_input_ != nullptr) place_rename_input();
  mark_dirty();
}

void MindMapView::set_fit(bool fit) {
  if (fit_ == fit) return;
  fit_ = fit;
  update_view();
  if (rename_input_ != nullptr) place_rename_input();
  mark_dirty();
}

void MindMapView::pan_by(float dx, float dy) {
  pan_x_ += dx;
  pan_y_ += dy;
  if (rename_input_ != nullptr) place_rename_input();
  mark_dirty();
}

void MindMapView::set_pan(float x, float y) {
  pan_x_ = x;
  pan_y_ = y;
  if (rename_input_ != nullptr) place_rename_input();
  mark_dirty();
}

void MindMapView::reset_view() {
  zoom_ = 1.0f;
  pan_x_ = 0.0f;
  pan_y_ = 0.0f;
  update_view();
  if (rename_input_ != nullptr) place_rename_input();
  mark_dirty();
}

void MindMapView::zoom_at(math::Point anchor, float factor) {
  const float clamped = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);
  if (std::abs(clamped - zoom_) < 1e-6f) return;
  // ① 先用**缩放前**的倍率把锚点换成内容坐标
  const math::Point origin = content_origin();
  const float content_x = (anchor.x - origin.x) / scale_;
  const float content_y = (anchor.y - origin.y) / scale_;
  // ② 改倍率，③ 反解 `pan` 让同一个内容点重新落在锚点下。
  //
  // 为何不能只改 `zoom`：`content_origin()` 里的居中偏移含 `bounds - content×scale`
  // 这一项，倍率一变它就变——只改 `zoom` 等于以**视口中心**为轴缩放，指针下的东西会跑。
  zoom_ = clamped;
  update_view();
  const math::Point moved = content_origin();
  pan_x_ += (anchor.x - moved.x) - content_x * scale_;
  pan_y_ += (anchor.y - moved.y) - content_y * scale_;
  if (rename_input_ != nullptr) place_rename_input();
  mark_dirty();
}

void MindMapView::set_direction(md::mindmap::LayoutDirection direction) {
  if (options_.direction == direction) return;
  options_.direction = direction;
  relayout();
  mark_dirty();
}

void MindMapView::set_layout_font_size(float size) {
  const float clamped = std::clamp(size, 8.0f, 48.0f);
  if (std::abs(options_.font_size - clamped) < 1e-6f) return;
  options_.font_size = clamped;
  relayout();
  mark_dirty();
}

void MindMapView::ensure_selected_visible() {
  if (selected_ >= layout_.boxes.size() || bounds_.is_empty()) return;
  const math::Rect box = layout_.boxes[selected_].rect;
  if (box.is_empty()) return;
  const math::Point origin = content_origin();
  const math::Rect screen{origin.x + box.x * scale_, origin.y + box.y * scale_,
                          box.width * scale_, box.height * scale_};
  // 留 24px 边距：贴着边线的节点看起来像"几乎看不见"
  const math::Rect view = bounds_.inset(math::Insets::all(std::min(24.0f, bounds_.height * 0.1f)));
  float dx = 0.0f;
  float dy = 0.0f;
  if (screen.x < view.x) dx = view.x - screen.x;
  if (screen.right() > view.right()) dx = view.right() - screen.right();
  if (screen.y < view.y) dy = view.y - screen.y;
  if (screen.bottom() > view.bottom()) dy = view.bottom() - screen.bottom();
  if (dx == 0.0f && dy == 0.0f) return;
  pan_by(dx, dy);
}

// ————————————————————————————————————————————————————————————————————————————
// 撤销 / 批量展开
// ————————————————————————————————————————————————————————————————————————————

auto MindMapView::undo() -> bool {
  finish_rename(false);   // 撤销前先收掉改名框：否则它会在旧几何上继续显示
  if (!undo_.undo()) return false;
  mark_dirty();
  return true;
}

auto MindMapView::redo() -> bool {
  finish_rename(false);
  if (!undo_.redo()) return false;
  mark_dirty();
  return true;
}

void MindMapView::expand_all() {
  (void)apply_edit("全部展开", [](md::mindmap::Document& doc) {
    bool changed = false;
    for (std::size_t index = 0; index < doc.node_count(); ++index) {
      if (doc.node(index)->collapsed) changed = doc.set_collapsed(index, false) || changed;
    }
    return changed;
  });
}

void MindMapView::collapse_all() {
  (void)apply_edit("全部折叠", [](md::mindmap::Document& doc) {
    bool changed = false;
    for (std::size_t index = 0; index < doc.node_count(); ++index) {
      // 叶子节点折叠无意义（会产生一堆"点了没反应"的历史条目）。
      if (doc.node(index)->children.empty()) continue;
      changed = doc.set_collapsed(index, true) || changed;
    }
    return changed;
  });
  // 全部折叠后唯一可见的是根：选中可能指向一个已经看不见的节点，得收回来。
  if (selected_ >= layout_.boxes.size() || layout_.boxes[selected_].rect.is_empty()) {
    set_selected(document_.valid() ? document_.root() : kNoNode);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void MindMapView::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  surface_ = colors.surface;
  border_ = colors.border;
  text_color_ = colors.text;
  text_muted_ = colors.text_muted;
  link_color_ = colors.border_strong;
  accent_ = colors.primary;
  selection_fill_ = colors.primary_soft;
  root_fill_ = colors.primary_soft;
  depth_fill_ = colors.surface_raised;
  // 分支配色：一级分支各占一个色相（眼睛靠颜色分"这几大支"，而不是靠文字慢慢读）。
  branch_colors_ = {colors.primary,      colors.success, colors.warning, colors.danger,
                    colors.accent,       colors.primary_hover, colors.text_muted,
                    colors.border_strong};
  // 自己不画整体底：导图应当"长在"宿主容器上（与 `MermaidView` 相反——那个是
  // 一张独立图，有底板才不与周围文字混）。
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.color = text_color_;
  apply_text_overrides();
  // 内嵌改名框是独立子元素（不继承本组件的主题缓存），要显式转发。
  if (rename_input_ != nullptr) rename_input_->refresh_theme(theme);
}

void MindMapView::measure(const RenderContext& context, const Constraints& constraints) {
  // 关键一步：用**真实端口重排**。布局结果依赖字宽，而字宽只有这里拿得到。
  // 没这一步会得到"用估算宽度排好版、用真实宽度画字"的错位（文字溢出节点框）。
  const TextPort& port = components_internal::text_port_of(context);
  rebuild_layout([&port](std::string_view text, float font_size) {
    return port.measure_width(text, font_size);
  });
  const float natural_width = content_size_.width > 0.0f ? content_size_.width : 320.0f;
  const float natural_height = content_size_.height > 0.0f ? content_size_.height : 200.0f;
  float width = fill_width(constraints, style_.width, natural_width);
  float height = fill_height(constraints, style_.height, natural_height);
  if (width <= 0.0f) width = natural_width;
  if (height <= 0.0f) height = natural_height;
  measured_ = math::Size{std::max(width, 0.0f), std::max(height, 0.0f)};
}

void MindMapView::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  update_view();
  if (rename_input_ != nullptr) place_rename_input();
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void MindMapView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const TextPort& port = components_internal::text_port_of(context);
  const math::Point origin = content_origin();
  const auto to_screen = [&](math::Point point) {
    return math::Point{origin.x + point.x * scale_, origin.y + point.y * scale_};
  };

  canvas.push_clip_rect(bounds_.inset(math::Insets::all(2.0f)));

  if (!document_.valid()) {
    const std::string hint("（空导图）");
    port.draw(canvas, hint,
              math::Point{bounds_.x +
                              (bounds_.width - port.measure_width(hint, options_.font_size)) * 0.5f,
                          bounds_.y + bounds_.height * 0.5f},
              options_.font_size, text_muted_);
    canvas.pop_clip();
    return;
  }

  // ① 连线（先画，压在节点下面：曲线落进节点边缘时不该盖住节点的字）
  const float link_width = std::max(kLinkWidth * scale_, 1.0f);
  for (const auto& link : layout_.links) {
    if (link.child >= layout_.boxes.size()) continue;
    const Node* child = document_.node(link.child);
    if (child == nullptr) continue;
    // 连线颜色跟**所属一级分支**走（不是跟深度）：同一支的整条链路同色，
    // 才能在密集处一眼读出“这几个节点是同一类”。
    const math::Color color =
        child->argb != 0U
            ? math::Color::from_hex(child->argb)
            : branch_color(document_, branch_colors_, link.child, link_color_, accent_);
    stroke_bezier(canvas, to_screen(link.from), to_screen(link.control1),
                  to_screen(link.control2), to_screen(link.to), color.with_alpha_f(0.55f),
                  link_width);
  }

  // ② 节点
  const float font = options_.font_size * scale_;
  for (std::size_t index = 0; index < layout_.boxes.size(); ++index) {
    const auto& box = layout_.boxes[index];
    if (box.rect.is_empty()) continue;  // 被折叠隐藏
    const Node* node = document_.node(index);
    if (node == nullptr) continue;
    const math::Rect rect =
        math::Rect::from_ltrb(origin.x + box.rect.x * scale_, origin.y + box.rect.y * scale_,
                              origin.x + box.rect.right() * scale_,
                              origin.y + box.rect.bottom() * scale_);
    const bool is_root = index == document_.root();
    const bool is_selected = index == selected_;
    const math::Color accent =
        node->argb != 0U ? math::Color::from_hex(node->argb)
                         : branch_color(document_, branch_colors_, index, accent_, accent_);

    // 底色：根用主色淡色、深层用抬升面（越深越"轻"），选中用选中色覆盖。
    math::Color fill = is_root ? root_fill_ : depth_fill_;
    if (box.depth >= 3U) fill = surface_.lighten(0.04f);
    if (is_selected) fill = selection_fill_;
    const float radius = is_root ? kRootRadius : kNodeRadius;
    components_internal::fill_round_rect(canvas, rect, radius, raster::Paint::solid(fill));

    // 描边：根与一级分支用强调色（层级感），其余用常规描边。
    const float border_width =
        (is_root || box.depth <= 1U) ? std::max(1.6f * scale_, 1.0f)
                                     : std::max(kFrameWidth * scale_, 1.0f);
    const math::Color border_color = (is_root || box.depth <= 1U) ? accent.with_alpha_f(0.72f)
                                                                : border_;
    components_internal::paint_outline(canvas, rect, radius, border_color, border_width);

    // 选中环：键盘导航后必须看得见"现在选中谁"（否则按方向键一片安静）。
    if (is_selected) {
      components_internal::paint_outline(canvas, rect.inflate(std::max(2.0f * scale_, 1.5f)),
                                         radius + 2.0f, accent_,
                                         std::max(1.5f * scale_, 1.0f));
    }

    // 文字：按布局层给的行与 `text_rect` 画（两者同源，不会"算两行画三行"）。
    const math::Rect text_area =
        math::Rect::from_ltrb(origin.x + box.text_rect.x * scale_,
                              origin.y + box.text_rect.y * scale_,
                              origin.x + box.text_rect.right() * scale_,
                              origin.y + box.text_rect.bottom() * scale_);
    const float row_height = port.line_height(font);
    const float text_height = row_height * static_cast<float>(box.lines.size());
    float text_y = text_area.y + (text_area.height - text_height) * 0.5f;
    for (const std::string& line : box.lines) {
      port.draw(canvas, line, math::Point{text_area.x, text_y}, font, text_color_);
      text_y += row_height;
    }

    // 折叠徽标：右下角一个小圆 + 数字（"这里还有 N 个节点"）。
    if (show_collapse_badges_ && box.has_hidden_children) {
      const math::Rect badge = badge_rect(index);
      if (!badge.is_empty()) {
        const math::Rect screen_badge =
            math::Rect::from_ltrb(origin.x + badge.x * scale_, origin.y + badge.y * scale_,
                                  origin.x + badge.right() * scale_,
                                  origin.y + badge.bottom() * scale_);
        math::Path badge_path;
        badge_path.add_circle(screen_badge.center(), screen_badge.width * 0.5f);
        canvas.fill_path(badge_path, raster::Paint::solid(accent));
        const std::string count = std::to_string(box.hidden_children);
        const float badge_font = std::max(screen_badge.width * 0.62f, 8.0f);
        const float text_width = port.measure_width(count, badge_font);
        port.draw(canvas, count,
                  math::Point{screen_badge.center().x - text_width * 0.5f,
                              screen_badge.center().y - port.line_height(badge_font) * 0.5f},
                  badge_font, surface_);
      }
    }
  }

  // ③ 拖拽反馈：源节点高亮 + 目标节点"将被落到谁下面"的预览。
  //
  // 为何必须提前画：拖拽提交后才知道落点，用户只能在松手后看到结果——
  // 而"落错了"是不可撤销的意外（虽然可以 Ctrl+Z）。提前预览把它变成可预见的操作。
  if (drag_mode_ == DragMode::Reparent && drag_active_) {
    const auto outline_node = [&](std::size_t index, math::Color color, float width) {
      if (index >= layout_.boxes.size()) return;
      const auto& box = layout_.boxes[index];
      if (box.rect.is_empty()) return;
      const math::Rect rect =
          math::Rect::from_ltrb(origin.x + box.rect.x * scale_, origin.y + box.rect.y * scale_,
                                origin.x + box.rect.right() * scale_,
                                origin.y + box.rect.bottom() * scale_);
      components_internal::paint_outline(canvas, rect, kNodeRadius, color, width);
    };
    outline_node(drag_node_, accent_.with_alpha_f(0.9f), std::max(2.0f * scale_, 1.5f));
    if (drag_target_ != kNoNode) {
      if (drag_target_ < layout_.boxes.size() && !layout_.boxes[drag_target_].rect.is_empty()) {
        const auto& target = layout_.boxes[drag_target_];
        const math::Rect rect =
            math::Rect::from_ltrb(origin.x + target.rect.x * scale_,
                                  origin.y + target.rect.y * scale_,
                                  origin.x + target.rect.right() * scale_,
                                  origin.y + target.rect.bottom() * scale_);
        components_internal::fill_round_rect(canvas,
                                             rect.inflate(std::max(3.0f * scale_, 2.0f)),
                                             kNodeRadius + 2.0f,
                                             raster::Paint::solid(accent_.with_alpha_f(0.18f)));
      }
      outline_node(drag_target_, accent_, std::max(2.0f * scale_, 1.5f));
    }
  }

  canvas.pop_clip();
}

// ————————————————————————————————————————————————————————————————————————————
// 命中与几何查询
// ————————————————————————————————————————————————————————————————————————————

auto MindMapView::node_rect(std::size_t index) const -> math::Rect {
  if (index >= layout_.boxes.size()) return math::Rect{};
  const auto& box = layout_.boxes[index];
  if (box.rect.is_empty()) return math::Rect{};
  const math::Point origin = content_origin();
  return math::Rect::from_ltrb(origin.x + box.rect.x * scale_, origin.y + box.rect.y * scale_,
                               origin.x + box.rect.right() * scale_,
                               origin.y + box.rect.bottom() * scale_);
}

auto MindMapView::badge_rect(std::size_t index) const -> math::Rect {
  if (!show_collapse_badges_ || index >= layout_.boxes.size()) return math::Rect{};
  const auto& box = layout_.boxes[index];
  if (box.rect.is_empty() || !box.has_hidden_children) return math::Rect{};
  // 内容坐标：贴在节点框右下角外侧一半（与绘制同一口径，否则点击命中不到）。
  const float radius = options_.font_size * kBadgeRadiusRatio;
  return math::Rect{box.rect.right() - radius * 0.6f, box.rect.bottom() - radius * 0.9f,
                    radius * 2.0f, radius * 2.0f};
}

auto MindMapView::node_at(math::Point screen) const -> std::size_t {
  // 先查徽标（它比节点框更小、且部分在框外——不先查会被"框内但不在徽标"的情况抢走）
  for (std::size_t index = 0; index < layout_.boxes.size(); ++index) {
    const math::Rect badge = node_rect(index);
    if (badge.is_empty()) continue;
    const math::Rect content_badge = badge_rect(index);
    if (content_badge.is_empty()) continue;
    const math::Point origin = content_origin();
    const math::Rect screen_badge =
        math::Rect::from_ltrb(origin.x + content_badge.x * scale_,
                              origin.y + content_badge.y * scale_,
                              origin.x + content_badge.right() * scale_,
                              origin.y + content_badge.bottom() * scale_);
    if (screen_badge.contains(screen)) return index;
  }
  const math::Point content = to_content(screen);
  return md::mindmap::hit_test(layout_, content);
}

auto MindMapView::cursor_at(math::Point point) const noexcept -> MouseCursor {
  if (!bounds_.contains(point)) return MouseCursor::Arrow;
  const std::size_t hit = node_at(point);
  if (hit != kNoNode) {
    // 节点上：可拖（改父级）也给 `Move`——这是"这里能拖"的唯一非视觉提示
    // （光标是系统级效果，截图里不存在）。
    return drag_reparent_ ? MouseCursor::Move : MouseCursor::Hand;
  }
  return pan_enabled_ ? MouseCursor::Move : MouseCursor::Arrow;
}

// ————————————————————————————————————————————————————————————————————————————
// 键盘
// ————————————————————————————————————————————————————————————————————————————

auto MindMapView::visible_order() const -> std::vector<std::size_t> {
  std::vector<std::size_t> order;
  order.reserve(document_.node_count());
  // 先序（与绘制/阅读顺序一致），只收可见的。
  const auto walk = [&](auto&& self, std::size_t index) -> void {
    const Node* node = document_.node(index);
    if (node == nullptr) return;
    order.push_back(index);
    if (node->collapsed) return;
    for (const std::size_t child : node->children) self(self, child);
  };
  if (document_.valid()) walk(walk, document_.root());
  return order;
}

auto MindMapView::neighbor_in_direction(std::size_t from, int dx, int dy) const -> std::size_t {
  if (from >= layout_.boxes.size()) return kNoNode;
  const math::Rect current = layout_.boxes[from].rect;
  if (current.is_empty()) return kNoNode;
  const math::Point center = current.center();
  std::size_t best = kNoNode;
  float best_score = 0.0f;
  for (std::size_t index = 0; index < layout_.boxes.size(); ++index) {
    if (index == from) continue;
    const math::Rect rect = layout_.boxes[index].rect;
    if (rect.is_empty()) continue;
    const math::Point candidate = rect.center();
    const float delta_x = candidate.x - center.x;
    const float delta_y = candidate.y - center.y;
    // 必须在要去的那个方向上（容忍 1px：同层节点的中心 y 有时相差不到半个像素）。
    if (dx > 0 && delta_x <= 1.0f) continue;
    if (dx < 0 && delta_x >= -1.0f) continue;
    if (dy > 0 && delta_y <= 1.0f) continue;
    if (dy < 0 && delta_y >= -1.0f) continue;
    // 打分 = 主轴距离 + 副轴距离 × 3（越小越好）。
    //
    // 为何副轴权重要大：按 ↑ 时用户想的是"同层上一行"，而不是"左上方向那个最近的点"。
    // 副轴距离不加权时，相邻层的一个节点会比同层的上一个更“近"——方向键就会斜着跳。
    float score = 0.0f;
    if (dx != 0) {
      score = std::abs(delta_x) + std::abs(delta_y) * 3.0f;
    } else {
      score = std::abs(delta_y) + std::abs(delta_x) * 3.0f;
    }
    if (best == kNoNode || score < best_score) {
      best = index;
      best_score = score;
    }
  }
  return best;
}

auto MindMapView::handle_key(const RenderContext& context, Event& event) -> bool {
  (void)context;
  const std::string& key = event.key;
  // 撤销/重做**不受 editable 限制**：只读视图也可能因为应用回写而需要回退。
  if (event.ctrl) {
    const std::string lower = [&] {
      std::string copy = key;
      std::ranges::transform(copy, copy.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return copy;
    }();
    if (lower == "z") {
      if (event.shift) {
        (void)redo();
      } else {
        (void)undo();
      }
      return true;
    }
    if (lower == "y") {
      (void)redo();
      return true;
    }
    if (lower == "s") {
      // Ctrl+S：有路径就存，没路径/写失败就交给应用（不静默吞掉）。
      (void)save();
      return true;
    }
    return false;
  }

  const auto order = [&] { return visible_order(); };
  if (key == "Tab") {
    const auto list = order();
    if (list.empty()) return false;
    const auto at = std::find(list.begin(), list.end(), selected_);
    std::size_t next = 0;
    if (at == list.end()) {
      next = event.shift ? list.size() - 1U : 0U;
    } else {
      const std::size_t position = static_cast<std::size_t>(at - list.begin());
      next = event.shift ? (position == 0U ? list.size() - 1U : position - 1U)
                         : (position + 1U) % list.size();
    }
    set_selected(list[next]);
    ensure_selected_visible();
    return true;
  }

  // 方向键：按**几何**找邻居（按数组顺序在思维导图里完全是错的）。
  if (key == "ArrowUp" || key == "ArrowDown" || key == "ArrowLeft" ||
      key == "ArrowRight") {
    if (selected_ == kNoNode) {
      set_selected(document_.valid() ? document_.root() : kNoNode);
      return true;
    }
    const int dx = key == "ArrowRight" ? 1 : (key == "ArrowLeft" ? -1 : 0);
    const int dy = key == "ArrowDown" ? 1 : (key == "ArrowUp" ? -1 : 0);
    const std::size_t next = neighbor_in_direction(selected_, dx, dy);
    if (next != kNoNode) {
      set_selected(next);
      ensure_selected_visible();
    }
    // 没邻居也消费：否则 ↑↓ 会落到外层滚动容器上（那不是用户按键时的意思）。
    return true;
  }

  if (!editable_) return false;
  if (key == "Enter" || key == "F2") {
    if (selected_ == kNoNode) return false;
    return begin_rename(selected_);
  }
  if (key == " ") {
    if (selected_ == kNoNode) return false;
    return apply_edit("折叠/展开", [&](md::mindmap::Document& doc) {
      return doc.toggle_collapsed(selected_);
    });
  }
  if (key == "Insert") {
    if (selected_ == kNoNode) return false;
    const std::size_t parent = selected_;
    std::size_t created = kNoNode;
    const bool ok = apply_edit("加子节点", [&](md::mindmap::Document& doc) {
      created = doc.add_child(parent, "新主题");
      return created != kNoNode;
    });
    if (ok && created != kNoNode) {
      set_selected(created);
      ensure_selected_visible();
    }
    return ok;
  }
  if (key == "Delete") {
    if (selected_ == kNoNode) return false;
    const std::size_t victim = selected_;
    std::size_t next = kNoNode;
    const bool ok = apply_edit("删子树", [&](md::mindmap::Document& doc) {
      next = doc.remove_subtree(victim);
      return next != kNoNode || victim != doc.root();
    });
    if (ok) set_selected(next == kNoNode ? (document_.valid() ? document_.root() : kNoNode) : next);
    return ok;
  }
  return false;
}

// ————————————————————————————————————————————————————————————————————————————
// 鼠标
// ————————————————————————————————————————————————————————————————————————————

auto MindMapView::on_event(const RenderContext& context, Event& event) -> bool {
  // 上一轮押下的改名框到这里一定已过了它的派发（这是新的一次事件），可以放心释放。
  // 放在最前：早释放，而不是一直押到下一次改名（那期间它白占着一个元素与一份文本）。
  flush_retired_input();
  const bool inside = bounds_.contains(event.position);
  switch (event.kind) {
    case EventKind::Wheel: {
      if (!inside || event.wheel_delta == 0.0f) return false;
      if (wheel_zoom_) {
        zoom_at(event.position,
                event.wheel_delta > 0.0f ? kWheelZoomStep : 1.0f / kWheelZoomStep);
        return true;
      }
      const float scaled_height = content_size_.height * scale_;
      if (scaled_height <= bounds_.height) return false;
      pan_by(0.0f, event.wheel_delta * wheel_step(context.theme));
      return true;
    }
    case EventKind::MouseDown: {
      if (!inside || event.button != kLeftButton) return false;
      // 先处理折叠徽标：它贴在节点框的角上，应与节点命中区分开。
      const std::size_t hit = node_at(event.position);
      if (hit != kNoNode && !badge_rect(hit).is_empty()) {
        const math::Point origin = content_origin();
        const math::Rect content_badge = badge_rect(hit);
        const math::Rect screen_badge =
            math::Rect::from_ltrb(origin.x + content_badge.x * scale_,
                                  origin.y + content_badge.y * scale_,
                                  origin.x + content_badge.right() * scale_,
                                  origin.y + content_badge.bottom() * scale_);
        if (screen_badge.contains(event.position) && editable_) {
          (void)apply_edit("折叠/展开", [&](md::mindmap::Document& doc) {
            return doc.toggle_collapsed(hit);
          });
          set_selected(hit);
          return true;
        }
      }

      set_selected(hit);
      drag_origin_ = event.position;
      drag_current_ = event.position;
      drag_pan_x_ = pan_x_;
      drag_pan_y_ = pan_y_;
      drag_active_ = false;
      drag_target_ = kNoNode;
      if (hit != kNoNode && drag_reparent_ && editable_) {
        drag_node_ = hit;
        drag_mode_ = DragMode::Reparent;
        event.handled = true;
        return true;
      }
      if (!pan_enabled_) {
        drag_mode_ = DragMode::None;
        drag_node_ = kNoNode;
        return hit != kNoNode;   // 点到节点 = 选了它（已消费）；点空白不消费
      }
      drag_node_ = kNoNode;
      drag_mode_ = DragMode::Pan;
      return true;
    }
    case EventKind::MouseMove: {
      if (drag_mode_ == DragMode::None) {
        // 吸附提示：悬停时改光标（每帧不会再回写样式，只改底座缓存）。
        return false;
      }
      drag_current_ = event.position;
      const float dx = drag_current_.x - drag_origin_.x;
      const float dy = drag_current_.y - drag_origin_.y;
      if (!drag_active_ && std::sqrt(dx * dx + dy * dy) < kDragThreshold) return true;
      drag_active_ = true;
      if (drag_mode_ == DragMode::Pan) {
        pan_x_ = drag_pan_x_ + dx;
        pan_y_ = drag_pan_y_ + dy;
        if (rename_input_ != nullptr) place_rename_input();
        mark_dirty();
        return true;
      }
      // Reparent：算出当前悬停的目标。
      const std::size_t target = node_at(event.position);
      const std::size_t accepted = [&] {
        if (target == kNoNode || target == drag_node_) return kNoNode;
        // 驼成环：拖到自己的后代上会把子树从根上剪下来（形成一个环）——布局会无限递归。
        if (document_.is_descendant_of(target, drag_node_)) return kNoNode;
        return target;
      }();
      if (accepted != drag_target_) {
        drag_target_ = accepted;
        mark_dirty();
      }
      return true;
    }
    case EventKind::MouseUp: {
      if (drag_mode_ == DragMode::None) return false;
      const DragMode mode = drag_mode_;
      const std::size_t source = drag_node_;
      const std::size_t target = drag_target_;
      const bool active = drag_active_;
      drag_mode_ = DragMode::None;
      drag_node_ = kNoNode;
      drag_target_ = kNoNode;
      drag_active_ = false;
      if (mode != DragMode::Reparent || !active) {
        mark_dirty();
        return mode != DragMode::None;
      }
      mark_dirty();
      if (target == kNoNode || source == kNoNode) return true;  // 落到空白 = 取消
      (void)apply_edit("改父级", [&](md::mindmap::Document& doc) {
        return doc.reparent(source, target);
      });
      return true;
    }
    case EventKind::DoubleClick: {
      if (!inside) return false;
      const std::size_t hit = node_at(event.position);
      if (hit == kNoNode) {
        // 双击空白 = 回初始视角（与 `MermaidView`/`ThreeView` 同约定）。
        reset_view();
        return true;
      }
      // 双击节点 = 改名（**不**顺手展开："只想选一下"的人不应被切换状态）。
      if (!editable_) return false;
      return begin_rename(hit);
    }
    case EventKind::KeyDown:
      return handle_key(context, event);
    default:
      return false;
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 语义与属性面
// ————————————————————————————————————————————————————————————————————————————

auto MindMapView::semantics_text() const -> std::string {
  if (!document_.valid()) return "思维导图（空）";
  const Node* selected = document_.node(selected_);
  if (selected == nullptr) {
    return std::format("思维导图：{} 个节点 / {} 层", document_.node_count(), document_.max_depth());
  }
  return std::format("思维导图：{} 个节点，选中「{}」", document_.node_count(), selected->text);
}

auto MindMapView::semantics_value() const -> std::string {
  return document_.valid() ? document_.text_of(document_.root()) : std::string{};
}

auto MindMapView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "file" || name == "path_file") return file_path_;
  if (name == "direction") {
    return std::string(md::mindmap::to_string(options_.direction));
  }
  if (name == "font_size") return std::format("{:.1f}", options_.font_size);
  if (name == "zoom") return std::format("{:.3f}", zoom_);
  if (name == "fit") return std::string(fit_ ? "true" : "false");
  if (name == "pan_x") return std::format("{:.1f}", pan_x_);
  if (name == "pan_y") return std::format("{:.1f}", pan_y_);
  if (name == "selected") {
    return selected_ == kNoNode ? std::string{} : std::to_string(selected_);
  }
  if (name == "editable") return std::string(editable_ ? "true" : "false");
  if (name == "pan_enabled") return std::string(pan_enabled_ ? "true" : "false");
  if (name == "wheel_zoom") return std::string(wheel_zoom_ ? "true" : "false");
  if (name == "show_collapse_badges") {
    return std::string(show_collapse_badges_ ? "true" : "false");
  }
  if (name == "node_count") return std::to_string(document_.node_count());
  if (name == "visible_count") return std::to_string(document_.visible_count());
  if (name == "depth") return std::to_string(document_.max_depth());
  if (name == "selected_text") {
    const Node* node = document_.node(selected_);
    return node == nullptr ? std::string{} : node->text;
  }
  if (name == "selected_path" || name == "path") {
    if (selected_ == kNoNode) return std::string{};
    std::string out;
    for (const std::size_t index : document_.path_of(selected_)) {
      if (!out.empty()) out += " / ";
      out += document_.text_of(index);
    }
    return out;
  }
  if (name == "error") return last_error_;
  if (name == "content_width") return std::format("{:.1f}", content_size_.width);
  if (name == "content_height") return std::format("{:.1f}", content_size_.height);
  if (name == "scale") return std::format("{:.3f}", scale_);
  if (name == "dragging") return std::string(dragging() ? "true" : "false");
  if (name == "editing") return std::string(is_renaming() ? "true" : "false");
  if (name == "can_undo") return std::string(can_undo() ? "true" : "false");
  if (name == "can_redo") return std::string(can_redo() ? "true" : "false");
  if (name == "dirty") return std::string(dirty_ ? "true" : "false");
  return Element::get_property(name);
}

auto MindMapView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "file" || name == "path_file") {
    set_file_path(std::string(value));
    return true;
  }
  if (name == "direction") {
    const auto parsed = md::mindmap::parse_direction(std::string(st::trim(value)));
    if (!parsed.has_value()) return false;
    set_direction(*parsed);
    return true;
  }
  if (name == "font_size") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_layout_font_size(static_cast<float>(*parsed));
    return true;
  }
  if (name == "zoom") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_zoom(static_cast<float>(*parsed));
    return true;
  }
  if (name == "fit") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_fit(*parsed);
    return true;
  }
  if (name == "pan_x" || name == "pan_y") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    if (name == "pan_x") {
      set_pan(static_cast<float>(*parsed), pan_y_);
    } else {
      set_pan(pan_x_, static_cast<float>(*parsed));
    }
    return true;
  }
  if (name == "selected") {
    const std::string_view trimmed = st::trim(value);
    if (trimmed.empty() || trimmed == "none") {
      set_selected(kNoNode);
      return true;
    }
    const auto parsed = st::parse_i64(trimmed);
    if (!parsed.has_value() || *parsed < 0) return false;
    const auto index = static_cast<std::size_t>(*parsed);
    if (index >= document_.node_count()) return false;
    set_selected(index);
    ensure_selected_visible();
    return true;
  }
  if (name == "editable" || name == "pan_enabled" || name == "wheel_zoom" ||
      name == "show_collapse_badges") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    if (name == "editable") editable_ = *parsed;
    else if (name == "pan_enabled") pan_enabled_ = *parsed;
    else if (name == "wheel_zoom") wheel_zoom_ = *parsed;
    else show_collapse_badges_ = *parsed;
    mark_dirty();
    return true;
  }
  if (name == "source" || name == "text") {
    set_source(value);
    return true;
  }
  return Element::set_property(name, value);
}

auto MindMapView::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"file", "direction", "font_size", "zoom", "fit", "pan_x",
                                  "pan_y", "selected", "editable", "pan_enabled",
                                  "wheel_zoom", "show_collapse_badges", "source"});
}

auto MindMapView::readonly_property_names() const -> std::vector<std::string_view> {
  return Element::with_universal_readonly(
      {"node_count", "visible_count", "depth", "selected_text", "selected_path", "error",
       "content_width", "content_height", "scale", "dragging", "editing", "can_undo",
       "can_redo", "dirty"});
}

auto MindMapView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  // —— 视图 ——
  if (action == "reset_view") {
    reset_view();
    return true;
  }
  if (action == "fit") {
    set_fit(true);
    reset_view();
    return true;
  }
  if (action == "zoom_in") {
    set_zoom(zoom_ * kWheelZoomStep);
    return true;
  }
  if (action == "zoom_out") {
    set_zoom(zoom_ / kWheelZoomStep);
    return true;
  }
  if (action == "select_root") {
    set_selected(document_.valid() ? document_.root() : kNoNode);
    ensure_selected_visible();
    return true;
  }
  // —— 文件（参数里给了路径就用它，否则用已设路径）——
  if (action == "load") {
    const std::string path(st::trim(argument));
    return (path.empty() ? load() : load_file(path)).has_value();
  }
  if (action == "save") {
    const std::string path(st::trim(argument));
    return (path.empty() ? save() : save_file(path)).has_value();
  }
  // —— 撤销 ——
  if (action == "undo") return undo();
  if (action == "redo") return redo();
  if (action == "expand_all") {
    expand_all();
    return true;
  }
  if (action == "collapse_all") {
    collapse_all();
    return true;
  }
  // —— 编辑（需 editable）——
  if (!editable_) return false;
  if (action == "new") {
    set_document(md::mindmap::Document(std::string(argument.empty() ? "中心主题" : argument)));
    return true;
  }
  if (action == "add_child" || action == "add_sibling") {
    if (selected_ == kNoNode) return false;
    const std::size_t anchor = selected_;
    std::size_t created = kNoNode;
    const bool child = action == "add_child";
    const bool ok = apply_edit(child ? "加子节点" : "加同级", [&](md::mindmap::Document& doc) {
      created = child ? doc.add_child(anchor, "新主题") : doc.add_sibling(anchor, "新主题");
      return created != kNoNode;
    });
    if (ok && created != kNoNode) {
      set_selected(created);
      ensure_selected_visible();
    }
    return ok;
  }
  if (action == "remove" || action == "remove_subtree") {
    if (selected_ == kNoNode) return false;
    const std::size_t victim = selected_;
    std::size_t next = kNoNode;
    const bool ok = apply_edit("删子树", [&](md::mindmap::Document& doc) {
      next = doc.remove_subtree(victim);
      return next != kNoNode || victim != doc.root();
    });
    if (ok) set_selected(next == kNoNode ? (document_.valid() ? document_.root() : kNoNode) : next);
    return ok;
  }
  if (action == "rename") {
    if (selected_ == kNoNode) return false;
    if (argument.empty()) return begin_rename(selected_);
    return apply_edit("改文字", [&](md::mindmap::Document& doc) {
      return doc.set_text(selected_, std::string(argument));
    });
  }
  if (action == "toggle_collapse") {
    if (selected_ == kNoNode) return false;
    return apply_edit("折叠/展开", [&](md::mindmap::Document& doc) {
      return doc.toggle_collapsed(selected_);
    });
  }
  if (action == "move_up") {
    if (selected_ == kNoNode) return false;
    return apply_edit("上移", [&](md::mindmap::Document& doc) {
      return doc.move_up(selected_);
    });
  }
  if (action == "move_down") {
    if (selected_ == kNoNode) return false;
    return apply_edit("下移", [&](md::mindmap::Document& doc) {
      return doc.move_down(selected_);
    });
  }
  return false;
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）─────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::MindMapView);
