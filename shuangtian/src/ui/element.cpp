#include "st/ui/element.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include "st/core/log.hpp"
#include "st/core/fs.hpp"
#include "st/perf/clock.hpp"
#include "st/perf/frame.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/translated_surface.hpp"
#include "st/math/path.hpp"
#include "st/ui/component_registry.hpp"   // Panel / Spacer 的登记（文件末尾）
#include "st/ui/components/panel.hpp"      // `component_meta<Panel/Spacer>` 的声明
#include "st/ui/line_layout.hpp"

namespace st::ui {
namespace {

[[nodiscard]] auto text_port_or_null(const RenderContext& context) -> const TextPort& {
  static const NullTextPort fallback;
  return context.text != nullptr ? *context.text : fallback;
}

/// 元素可能画到 `bounds_` 之外的**安全外扩量**（逻辑像素）。
///
/// 阴影的模糊与偏移、外发光、悬浮上浮都会溢出元素矩形；剔除时如果只按 `bounds_` 判断，
/// 这些行会被整块剔掉（表现为"卡片浮起来时边缘缺一块"这类难查的视觉回归）。
/// 因此这里按**当前主题**算实际外扩，而不是拍一个魔法数字。
[[nodiscard]] auto paint_margin(const RenderContext& context) -> float {
  const Shadow lg = shadow_lg(context.theme);
  const float shadow = std::max(lg.blur + std::abs(lg.offset_y),
                                lg.blur2 + std::abs(lg.offset2_y));
  const Metrics& metrics = context.theme.metrics();
  return shadow + metrics.hover_glow_width + metrics.hover_lift + 2.0f;
}

/// 子树里是否**可能**有元素与 `clip`（逻辑坐标）相交。
///
/// 为什么不能只看自己的矩形：框架允许子元素排到父容器之外（如外挂面板），
/// 按父矩形剔除会把可见的子元素一起剔掉。所以"自己有交集 → 直接画"，
/// "自己没交集 → 还要看后代"。这一步是纯矩形运算，相对绘制开销可忽略。
[[nodiscard]] auto subtree_may_paint(const Element& element, const math::Rect& clip,
                                     float margin) -> bool {
  const math::Rect inflated = element.bounds().inset(math::Insets::all(-margin));
  if (!inflated.is_empty() && !inflated.intersect(clip).is_empty()) return true;
  const auto children = element.children();
  for (const auto& child : children) {
    if (child == nullptr) continue;
    if (!child->visible()) continue;
    if (subtree_may_paint(*child, clip, margin)) return true;
  }
  return false;
}

[[nodiscard]] auto main_size(FlexDirection direction, math::Size size) noexcept -> float {
  return direction == FlexDirection::Row ? size.width : size.height;
}

[[nodiscard]] auto cross_size(FlexDirection direction, math::Size size) noexcept -> float {
  return direction == FlexDirection::Row ? size.height : size.width;
}

/// 把业务 key 转成 id 安全片段。
///
/// 为什么要转：id 会出现在**选择器**里（`#tasks/ListItem@task-42`），而选择器在
/// `#`/`.`/`:`/`[`/空白 处切词——业务 key 里带一个空格或点，选择器就再也定位不到这个元素。
/// 而 id 的用途本就是"被外部引用"，所以这里对不安全字符坚决替换，宁可变形不可坏用。
[[nodiscard]] auto sanitize_key(std::string_view key) -> std::string {
  std::string out;
  out.reserve(key.size());
  for (const char raw : key) {
    const auto value = static_cast<unsigned char>(raw);
    const bool safe = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                      (value >= '0' && value <= '9') || value == '_' || value == '-';
    out.push_back(safe ? raw : '-');
  }
  return out;
}

/// 兄弟间 key 撞车检测：撞了会生成**相同的 id**，于是按 id 查找可能命中另一个元素——
/// 这类错误静默且难查，所以在插入时就如实报出来。
void warn_on_duplicate_key(const Element& parent, const Element& child) {
  const std::string sanitized = sanitize_key(child.key());
  if (sanitized.empty()) return;
  for (const auto& sibling : parent.children()) {
    if (sibling.get() == &child || sibling->type() != child.type()) continue;
    if (sanitize_key(sibling->key()) != sanitized) continue;
    ST_LOG_WARN("同一父节点下 {} 的 key 重复（'{}'）：自动 id 会撞车，按 id 查找可能命中另一个元素",
              child.type(), child.key());
    return;
  }
}

}  // namespace

// —— NullTextPort ——

auto NullTextPort::instance() -> const NullTextPort& {
  static const NullTextPort port;
  return port;
}

auto NullTextPort::measure(std::string_view utf8, float size) const -> math::Size {
  (void)utf8;
  return math::Size{0.0f, size * 1.45f};
}

auto NullTextPort::measure_width(std::string_view utf8, float size, text::FontRole role) const -> float {
  (void)role;  // 无字体环境：角色无意义
  (void)utf8;
  (void)size;
  return 0.0f;
}

auto NullTextPort::line_height(float size) const -> float { return size * 1.45f; }

/// 无字体环境下的基线位：按常见的 ascent/descent 比例给（约 0.8 / 0.2）。
/// 布局单测用不上精确值，但**垂直居中**的公式需要它，否则除数会是 0。
auto NullTextPort::ascent(float size) const -> float { return size * 1.16f; }

auto NullTextPort::descent(float size) const -> float { return size * 0.29f; }

void NullTextPort::draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
                        math::Color color, text::FontRole role, float embolden,
                        bool bold) const {
  (void)role;  // 无字体环境：角色与字重都无意义
  (void)embolden;
  (void)bold;
  (void)canvas;
  (void)utf8;
  (void)origin;
  (void)size;
  (void)color;
}

auto NullTextPort::ellipsize(std::string_view utf8, float size, float max_width) const
    -> std::string {
  (void)size;
  (void)max_width;
  return std::string(utf8);
}

// 无字体环境没有字宽概念，中段省略与尾部省略行为一致（量宽恒 0，也就无从省略）。
auto NullTextPort::ellipsize_middle(std::string_view utf8, float size,
                                   float max_width) const -> std::string {
  return ellipsize(utf8, size, max_width);
}

auto NullTextPort::wrap(std::string_view utf8, float size, float max_width) const
    -> std::vector<std::string_view> {
  (void)size;
  (void)max_width;
  return {utf8};
}

auto NullTextPort::wrap_limited(std::string_view utf8, float size, float max_width,
                                std::size_t max_lines) const -> std::vector<std::string> {
  (void)size;
  (void)max_width;
  (void)max_lines;
  return {std::string(utf8)};
}

// —— Element ——

// 读取侧用的静态空值（无附属状态时返回它，避免为"读一下"就分配）。
const std::optional<math::Color> Element::kNoColor{};
const std::optional<Tone> Element::kNoTone{};
const std::optional<FontWeight> Element::kNoWeight{};

Element::Element() = default;
Element::~Element() = default;

auto Element::child_at(std::size_t index) const noexcept -> Element* {
  return index < children_.size() ? children_[index].get() : nullptr;
}

auto Element::derived_id() const -> ElementId {
  if (!id_.empty()) return id_;
  // **必须是拥有型容器**：下面 `std::format(...)` 产生的是临时 `std::string`，
  // 若存成 `std::string_view` 会立刻悬垂，拼出来的 id 里就会出现垃圾字节
  // （实测：列表项 id 变成 `task-list/task-list/\x00`，且相邻项 id 相同——
  //   选择器/协议/脚本全都拿不到正确的元素）。
  std::vector<std::string> parts;
  const Element* current = this;
  std::size_t depth = 0;
  while (current != nullptr && depth < 64) {
    if (!current->id_.empty()) {
      parts.push_back(current->id_);
      break;
    }
    const Element* parent = current->parent_;
    if (parent == nullptr) {
      parts.push_back("root");
      break;
    }
    const auto& siblings = parent->children_;
    std::size_t index = 0;
    for (std::size_t position = 0; position < siblings.size(); ++position) {
      if (siblings[position].get() == current) {
        index = position;
        break;
      }
    }
    // 有 key 就用 key：索引会随插入/删除/筛选整体位移，key 不会。
    // 这是"列表刷新后同一项仍能按 id 找到"的关键（选择器安全字符见 `sanitize_key`）。
    if (const std::string sanitized = sanitize_key(current->key_); !sanitized.empty()) {
      parts.push_back(std::format("{}@{}", current->type(), sanitized));
    } else {
      parts.push_back(std::format("{}[{}]", current->type(), index));
    }
    current = parent;
    ++depth;
  }
  std::string out;
  for (auto iterator = parts.rbegin(); iterator != parts.rend(); ++iterator) {
    if (!out.empty()) out.push_back('/');
    out.append(*iterator);
  }
  return out;
}

auto Element::add_child(std::unique_ptr<Element> child) -> Element* {
  if (child == nullptr) return nullptr;
  child->parent_ = this;
  Element* raw = child.get();
  warn_on_duplicate_key(*this, *raw);
  children_.push_back(std::move(child));
  // 新子元素继承本元素的宿主（与 UiRoot::wire_owner 同一契约）：不这样做的话，
  // 声明式每帧新建的元素 `host_` 恒为空——“要宿主给我焦点”的组件全部失效。
  raw->adopt_host();
  mark_layout_dirty();
  return raw;
}

auto Element::insert_child(std::size_t index, std::unique_ptr<Element> child) -> Element* {
  if (child == nullptr) return nullptr;
  child->parent_ = this;
  Element* raw = child.get();
  const std::size_t position = index > children_.size() ? children_.size() : index;
  warn_on_duplicate_key(*this, *raw);
  children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(position), std::move(child));
  raw->adopt_host();   // 同 add_child：新挂入的子元素继承宿主契约
  mark_layout_dirty();
  return raw;
}

auto Element::remove_child(Element* child) -> std::unique_ptr<Element> {
  for (auto iterator = children_.begin(); iterator != children_.end(); ++iterator) {
    if (iterator->get() == child) {
      std::unique_ptr<Element> detached = std::move(*iterator);
      detached->parent_ = nullptr;
      children_.erase(iterator);
      mark_layout_dirty();
      return detached;
    }
  }
  return nullptr;
}

void Element::clear_children() {
  for (auto& child : children_) child->parent_ = nullptr;
  children_.clear();
  mark_layout_dirty();
}

auto Element::hit_test(math::Point point) const noexcept -> bool {
  if (!visible_) return false;
  return bounds_.contains(point);
}

auto Element::hit_test_children(math::Point point) const noexcept -> bool {
  for (const auto& child : children_) {
    if (child == nullptr || !child->visible()) continue;
    if (child->hit_test(point) || child->hit_test_children(point)) return true;
  }
  return false;
}

void Element::adopt_host() {
  if (parent_ != nullptr) host_ = parent_->host_;
  for (const auto& child : children_) {
    if (child != nullptr) child->adopt_host();
  }
}

auto Element::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags;
  flags.visible = visible_;
  flags.enabled = enabled_;
  flags.focused = focused_;
  flags.hovered = hovered_;
  flags.pressed = pressed_;
  return flags;
}

void Element::collect_semantics(SemanticsNode& node) const {
  node.id = derived_id();
  node.type = std::string(type());
  node.role = role();
  node.bounds = bounds_;
  node.text = semantics_text();
  node.value = semantics_value();
  node.flags = semantics_flags();
  for (const auto& child : children_) {
    if (!child->visible_) continue;
    SemanticsNode child_node;
    child->collect_semantics(child_node);
    node.children.push_back(std::move(child_node));
  }
}

void Element::collect_visual(VisualNode& node) const {
  node.id = derived_id();
  node.type = std::string(type());
  node.bounds = bounds_;
  node.visible = visible_;
  node.opacity = style_.opacity;
  node.fill = style_.background.a != 0U ? style_.background.to_css() : std::string{};
  node.radius = style_.radius;
  node.text = semantics_text();
  // 文本**颜色/字号/字重**上报（只在本层真的画了文字时；语义文本为空就不报）：
  // 这三项既不在语义树里、像素里也不好断言，是"标红生效了吗"唯一便宜的读数口。
  // 判据用 `style_.background` 之外的信息也可，但**以语义文本为准最诚实**——
  // 没有文本的层就算 style 里有色值，画面上也没有那笔颜色可断言。
  if (!node.text.empty()) {
    node.text_color = style_.color.to_css();
    node.font_size = style_.font_size;
    node.font_weight = std::string(ui::to_string(style_.font_weight));
  }
  node.hit_target = true;
  for (const auto& child : children_) {
    if (!child->visible_) continue;
    VisualNode child_node;
    child_node.depth = node.depth + 1;
    child->collect_visual(child_node);
    node.children.push_back(std::move(child_node));
  }
}

auto Element::get_property(std::string_view name) const -> std::optional<std::string> {
  // `hovered` 暴露给控制通道：悬浮是**只能看像素、看不出状态**的交互态，
  // 不给读取口就只能靠截图猜（自动化验证会很脆）。
  if (name == "hovered") return hovered_ ? "true" : "false";
  if (name == "hover_progress") return std::format("{:.3f}", static_cast<double>(hover_progress()));
  if (name == "hover_effect") return hover_effect_.enabled ? "true" : "false";
  // 显式排版覆盖的读回口（与写入口 `set_text_*` 对称）：
  // `color` 读的是**最终生效值**（`style_.color`，已是主题或覆盖的结果），
  // 而 `color_override` 读的是"有没有显式设过"——两者用途不同：
  // 前者给"看起来对不对"，后者给"我设的有没有被主题盖掉"。
  if (name == "color") return style_.color.to_css();
  if (name == "color_override") {
    const auto& override_color = text_color_override();
    return override_color.has_value() ? std::optional<std::string>(override_color->to_css())
                                      : std::nullopt;
  }
  if (name == "tone_override") {
    const auto& override_tone = text_tone_override();
    return override_tone.has_value()
               ? std::optional<std::string>(std::string(tone_name(*override_tone)))
               : std::nullopt;
  }
  // ## 字号统一叫 `font_size`（不再叫 `size`）
  //
  // 旧名 `size` 与三维场景的 `size`（全长）**同名不同义**——那是模型最容易踩的
  // 一类坑（名字一样、含义完全无关，且两边都不报错）。
  // 这里改成与内部字段 `Style::font_size` 一致。
  //
  // 兼容期：仍接受 `size`，但走告警路径（见 set_property）。
  if (name == "font_size") return std::format("{:.2f}", static_cast<double>(style_.font_size));
  if (name == "weight") return std::string(ui::to_string(style_.font_weight));
  // —— 动画诊断口 ——
  //
  // `animation` 返回**空串** = 未设动画（而不是 `"none"`：后者在 CSS 里是隔写，
  // 两者的语义一样但读者容易把它们当成“设了但没效果”）。
  if (name == "animation") {
    const RenderExtras* state = extras_if_any();
    return state != nullptr && state->anim_active ? std::optional<std::string>(state->animation.name)
                                                  : std::nullopt;
  }
  if (name == "animation_running") return animation_running() ? "true" : "false";
  // 本帧的动画透明度（`get` 能读到“现在到底多淡”，比截图量像素可靠）。
  if (name == "animation_opacity") {
    const anim::Props& props = animation_props();
    return props.opacity.has_value()
               ? std::optional<std::string>(std::format("{:.3f}", static_cast<double>(*props.opacity)))
               : std::nullopt;
  }
  if (name == "animation_translate") {
    return std::format("{:.2f},{:.2f}", static_cast<double>(animation_translate_.x),
                       static_cast<double>(animation_translate_.y));
  }
  if (name == "transition") {
    const RenderExtras* state = extras_if_any();
    return state != nullptr && state->transition.active()
               ? std::optional<std::string>(std::format("{:.3f}s", state->transition.duration))
               : std::nullopt;
  }
  return std::nullopt;
}

auto Element::set_property(std::string_view name, std::string_view value) -> bool {
  (void)name;
  (void)value;
  return false;
}

auto Element::property_names() const -> std::vector<std::string_view> {
  return {"hovered", "hover_progress", "hover_effect", "color", "color_override",
          "tone_override", "font_size", "weight"};
}

auto Element::readonly_property_names() const -> std::vector<std::string_view> {
  // 默认空：哪些属性只读由组件自己声明。
  //
  // 动画诊断五项**只能读**：它们反映的是“这一帧播到哪了”（由时间轴推出），
  // 写入它们没有意义；而入 `property_names()` 会让通用护欄
  // （“声明为可写的名字必须能 `set` 生效”）必然失败。
  // 要触发动画走 `invoke_action("animate", 预设名)`——那是**动作**不是属性。
  return {"animation", "animation_running", "animation_opacity", "animation_translate",
          "transition"};
}

auto Element::property_schema() const -> std::span<const PropertyDesc> {
  // 默认空表：**未声明的组件零成本**。
  //
  // 为何不在这里“根据 `property_names()` 自动合成一份描述”：
  // 那只能猜出 `Text` 类型与无范围，而错误的范围比没有范围更危险
  // （面板会生成一个拖到底也不动的控件）。宁缺毋滥。
  return {};
}

auto Element::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "click" || action == "activate") {
    activate();
    return true;
  }
  if (action == "focus") {
    set_focused(true);
    return true;
  }
  if (action == "blur") {
    set_focused(false);
    return true;
  }
  // 动画的**动作**入口（属性面只暴露只读诊断，原因见 `readonly_property_names`）：
  //   invoke animate <预设名>   —— 用内置预设起播（名字见 `anim::preset_names()`）
  //   invoke stop-animation     —— 清除动画
  // 为何走动作而不走 `set animation=…`：起播是**带副作用的动作**（重置起点、标脏、
  // 请求续帧），而 `set` 的契约是“改一个值”——把它做成属性会让
  // “重复 `set` 同一个名字”反复重播（声明式每帧下发属性时会变成死循环动画）。
  if (action == "animate") {
    return set_preset_animation(argument);
  }
  if (action == "stop-animation") {
    clear_animation();
    return true;
  }
  return false;
}

void Element::mark_dirty() {
  dirty_ = true;
  for (Element* current = parent_; current != nullptr; current = current->parent_) {
    current->dirty_ = true;
  }
  // 增量重绘：把"我变了"上报给所在树的根（UiRoot 帧首收集）
  record_damage();
}

void Element::record_damage() const noexcept {
  const Element* top = this;
  for (const Element* current = parent_; current != nullptr; current = current->parent_) {
    top = current;
  }
  if (extras_if_any() == nullptr || extras().paint_margin_hint < 0.0f) {
    // **零几何元素不占像素：它的变更不可能造成可见损坏，不该要求整帧。**
    //
    // 为何这条单独成立（不依赖 margin 是否已知）：`bounds` 为空时，
    // `bounds_.inflate(margin)` 也只会得到 margin 大小的包围盒——而它实际画不出任何东西
    // （`Element::paint` 的 `paint_box` 第一行就是 `if (bounds_.is_empty()) return;`）。
    //
    // 实测代价：gbcode 稳态下一次改文字事件里有 **226 次**这样的 `needs_full`
    // （git/终端面板隐藏时的 0×0 控件），于是每次交互都退化成整帧。
    if (bounds_.is_empty()) return;
    // 非零几何且从未绘制过 → 外扩量未知，保守要求整帧。
    top->extras().damage_needs_full = true;
    top->extras().damage_valid = true;
    return;
  }
  const RenderExtras& self = extras();
  const math::Rect extent = bounds_.inflate(self.paint_margin_hint);
  RenderExtras& top_state = top->extras();
  top_state.damage = top_state.damage_valid ? top_state.damage.union_with(extent) : extent;
  top->extras().damage_valid = true;
}

void Element::record_damage_rect(math::Rect rect) const noexcept {
  // 空矩形不登记（与 `record_damage` 对零几何元素的处理同一逻辑：画不出的东西
  // 不占像素）。矩形有效性由调用方保证（它是唯一知道“改了哪块”的人）。
  if (rect.is_empty()) return;
  const Element* top = this;
  for (const Element* current = parent_; current != nullptr; current = current->parent_) {
    top = current;
  }
  RenderExtras& top_state = top->extras();
  top_state.damage = top_state.damage_valid ? top_state.damage.union_with(rect) : rect;
  top_state.damage_valid = true;
}

auto Element::take_damage() const noexcept -> DamageReport {
  DamageReport report;
  const RenderExtras& state = extras();
  report.rect = state.damage;
  report.valid = state.damage_valid;
  report.needs_full = state.damage_needs_full;
  RenderExtras& mutable_state = extras();
  mutable_state.damage = math::Rect{};
  mutable_state.damage_valid = false;
  mutable_state.damage_needs_full = false;
  return report;
}

void Element::mark_layout_dirty() {
  layout_dirty_ = true;
  dirty_ = true;
  for (Element* current = parent_; current != nullptr; current = current->parent_) {
    current->layout_dirty_ = true;
    current->dirty_ = true;
  }
  // **刻意不在这里 `record_damage()`**。
  //
  // 重排会挑动兄弟（挨着排的元素跟着位移），波及范围难界定——因此“需要重排”
  // 在 `paint_frame` 里走的是**整帧**（`damage_empty` 或 `layout()` 的几何签名）。
  // 若在这里上报一个“本元素 bounds”的窄脏区，会让布局变更也能通过 `use_full`
  // 检查走局部重绘，**而那个窄区盖不住被挑动的兄弟**。
  // 回归：`tests/ui_damage_test.cpp` 的 `damage_layout_change_falls_back_to_full_frame`。
}

void Element::clear_dirty() noexcept {
  dirty_ = false;
  layout_dirty_ = false;
}

// —— 动画（CSS 式；引擎在 `st/ui/animation.cpp`）——

void Element::set_animation(anim::Animation animation, anim::Keyframes frames) {
  if (animation.empty() || frames.empty()) {
    clear_animation();
    return;
  }
  RenderExtras& state = extras();
  state.animation = std::move(animation);
  // **归一化在设值时做一次**（排序/去重/补端点）：采样是每帧都跑的，
  // 把排序放在采样里等于每帧排一次序（动画越复杂越贵，且排序结果毫无变化）。
  state.keyframes = std::move(frames).normalized();
  state.anim_active = true;
  state.anim_running = true;
  // 起点由**首帧绘制**定（`-1`）：这样“设好动画、下帧才上屏”的元素不会因为
  // 构造与首帧之间的时间差而生从半程开始播。
  state.anim_start = -1.0;
  state.anim_props = anim::Props{};
  mark_dirty();
}

auto Element::set_preset_animation(std::string_view preset) -> bool {
  std::optional<anim::Keyframes> frames = anim::preset_keyframes(preset);
  std::optional<anim::Animation> animation = anim::preset_animation(preset);
  if (!frames.has_value() || !animation.has_value()) {
    // 名字不存在：**不静默装个空的**——那会让“写错动画名”看起来像“动画生效了但看不出”。
    ST_LOG_WARN("未知的动画预设：{}（可用：fadeIn/fadeInUp/fadeOut/modalIn/slideInLeft/"
                "slideInRight/pulse/shake）", preset);
    return false;
  }
  set_animation(std::move(*animation), std::move(*frames));
  return true;
}

void Element::clear_animation() {
  const RenderExtras* state = extras_if_any();
  if (state == nullptr || !state->anim_active) return;
  RenderExtras& mutable_state = extras();
  mutable_state.anim_active = false;
  mutable_state.anim_running = false;
  mutable_state.anim_start = -1.0;
  mutable_state.anim_props = anim::Props{};
  animation_translate_ = math::Point{};
  animation_scale_ = 1.0f;
  mark_dirty();
}

void Element::set_animation_delay(double seconds) {
  const RenderExtras* state = extras_if_any();
  if (state == nullptr || !state->anim_active) return;
  RenderExtras& mutable_state = extras();
  mutable_state.animation.delay = seconds;
  // 改延迟等于“重新计时”：起点抬到下帧（否则已播的时长会跟新延迟叠在一起）。
  mutable_state.anim_start = -1.0;
  mark_dirty();
}

auto Element::transition_configured() const noexcept -> bool {
  const RenderExtras* state = extras_if_any();
  return state != nullptr && state->transition.active();
}

auto Element::animation_running() const noexcept -> bool {  const RenderExtras* state = extras_if_any();
  return state != nullptr && state->anim_running;
}

auto Element::animation_props() const noexcept -> const anim::Props& {
  static const anim::Props kEmpty{};
  const RenderExtras* state = extras_if_any();
  return state != nullptr ? state->anim_props : kEmpty;
}

auto Element::animation_transforms() const noexcept -> bool {
  return animation_translate_.x != 0.0f || animation_translate_.y != 0.0f ||
         animation_scale_ != 1.0f;
}

void Element::set_transition(anim::Transition transition) {
  RenderExtras& state = extras();
  state.transition = std::move(transition);
  state.transition_start = -1.0;
  state.transition_running = false;
  state.transition_from = anim::Props{};
  state.transition_to = anim::Props{};
  state.style_snapshot = anim::Props{};
  mark_dirty();
}

void Element::clear_transition() {
  const RenderExtras* state = extras_if_any();
  if (state == nullptr || !state->transition.active()) return;
  RenderExtras& mutable_state = extras();
  mutable_state.transition = anim::Transition{};
  mutable_state.transition_start = -1.0;
  mutable_state.transition_running = false;
  mutable_state.transition_from = anim::Props{};
  mutable_state.transition_to = anim::Props{};
  mutable_state.style_snapshot = anim::Props{};
  mark_dirty();
}

auto Element::transition_active() const noexcept -> bool {
  const RenderExtras* state = extras_if_any();
  return state != nullptr && state->transition_running;
}

namespace {

/// 把一份 `Style` 里**可动画的项**取成 `Props`（过渡的起止点与“值变没变”判据）。
[[nodiscard]] auto snapshot_style(const Style& style) -> anim::Props {
  anim::Props out{};
  out.opacity = style.opacity;
  out.background = style.background;
  out.border_color = style.border_color;
  out.color = style.color;
  out.radius = style.radius;
  out.border_width = style.border_width;
  out.font_size = style.font_size;
  return out;
}

/// 把动画/过渡算出的属性**写进一份 `Style` 副本**（返回 `paint_style()` 要用的快照）。
void apply_props(Style& style, const anim::Props& props) {
  // 逐项只在声明了才写——**未声明的项必须保持样式原值**（关键帧 `from{opacity:0}`
  // 只该动 opacity，若把其余字段写成默认值，背景色也会被一起冲掉）。
  if (props.opacity.has_value()) style.opacity = *props.opacity;
  if (props.background.has_value()) style.background = *props.background;
  if (props.border_color.has_value()) style.border_color = *props.border_color;
  if (props.color.has_value()) style.color = *props.color;
  if (props.radius.has_value()) style.radius = *props.radius;
  if (props.border_width.has_value()) style.border_width = *props.border_width;
  if (props.font_size.has_value()) style.font_size = *props.font_size;
}

}  // namespace

auto Element::advance_animation(const RenderContext& context) const -> bool {
  const RenderExtras* existing = extras_if_any();
  const bool has_animation = existing != nullptr && existing->anim_active;
  const bool has_transition = existing != nullptr && existing->transition.active();
  if (!has_animation && !has_transition) {
    animation_translate_ = math::Point{};
    animation_scale_ = 1.0f;
    return false;
  }
  RenderExtras& state = extras();
  anim::Props combined{};
  bool running = false;

  if (state.anim_active) {
    if (state.anim_start < 0.0) state.anim_start = context.time_seconds;
    if (context.reduced_motion) {
      // 无障碍：不播中间帧，直接落终态。用“一个足够大的 elapsed”让采样器
      // 走**它自己的结束分支**（而不是让这里再造一套“终态是什么”的逻辑）——
      // 两份实现必然漂移（`fill`/`direction`/`iterations` 全都参与终态判定）。
      const double span = state.animation.delay +
                          state.animation.duration *
                              static_cast<double>(state.animation.iterations > 0
                                                      ? state.animation.iterations
                                                      : 1) +
                          1.0;
      const anim::Sample sample =
          anim::sample_animation(state.keyframes, state.animation, span);
      state.anim_props = sample.props;
      state.anim_running = false;
    } else {
      const anim::Sample sample = anim::sample_animation(
          state.keyframes, state.animation, context.time_seconds - state.anim_start);
      state.anim_props = sample.props;
      state.anim_running = sample.running;
      running = running || sample.running;
    }
    combined = anim::overlay_props(combined, state.anim_props);
  }

  if (state.transition.active()) {
    const anim::Props target = snapshot_style(style_);
    if (!state.style_snapshot_valid) {
      // 第一次推进：只记基准，**不开过渡**（否则首帧会无端播一段）。
      state.style_snapshot = target;
      state.style_snapshot_valid = true;
    } else if (!anim::props_equal(target, state.style_snapshot)) {
      // 值变了：从“当前视觉值”起步（不是从上一份声明的目标值——那会让
      // 连续多次变化（如快速划过一排按钮）出现跳变）。
      anim::Props from = state.style_snapshot;
      anim::Props current = anim::lerp_props(
          state.transition_from, state.transition_to,
          anim::transition_progress(state.transition,
                                    state.transition_start < 0.0
                                        ? 0.0
                                        : context.time_seconds - state.transition_start));
      // 只把**参与过渡的属性**换成当前视觉值，其余用新目标（新目标里没动的项不追）。
      from = anim::overlay_props(from, current);
      state.transition_from = from;
      state.transition_to = target;
      state.transition_start = context.time_seconds;
      state.transition_running = true;
      state.style_snapshot = target;
    }
    if (state.transition_running) {
      // 无障碍：直接落位（不播中间帧）。
      const float progress =
          context.reduced_motion
              ? 1.0f
              : anim::transition_progress(state.transition,
                                          context.time_seconds - state.transition_start);
      if (progress >= 1.0f) {
        state.transition_running = false;
      } else {
        running = true;
      }
      const anim::Props interpolated =
          anim::lerp_props(state.transition_from, state.transition_to, progress);
      combined = anim::overlay_props(combined, interpolated);
    }
  }

  // 几何变换取**动画**的属性（过渡不参与位移：过渡的触发是“值变了”，
  // 而位移/缩放没有“样式值”可比较——它们是动画专有的表达）。
  animation_translate_ = math::Point{state.anim_props.translate_x.value_or(0.0f),
                                     state.anim_props.translate_y.value_or(0.0f)};
  animation_scale_ = state.anim_props.scale.value_or(1.0f);

  if (combined.empty()) {
    return running;
  }
  state.animated_style = style_;
  apply_props(state.animated_style, combined);
  // 位置变了 → 本帧必须重画（损坏区由续帧协议带着上报）。
  if (running) {
    record_damage();
    // **几何变化时的损坏区要盖住“旧位置 + 新位置”**：只报原地那一块，
    // 元素滑出之后的残影就没人清（局部重绘路径会把画布上的旧像素留着）。
    if (animation_transforms()) {
      const float margin = state.paint_margin_hint > 0.0f ? state.paint_margin_hint : 0.0f;
      const math::Rect here = bounds_.inflate(margin);
      const math::Rect there = bounds_.offset(animation_translate_.x, animation_translate_.y)
                                   .inflate(margin);
      record_damage_rect(here.union_with(there));
    }
  }
  return running;
}

auto Element::paint_style() const noexcept -> const Style& {
  if (style_override_ != nullptr) return *style_override_;
  const RenderExtras* state = extras_if_any();
  if (state == nullptr) return style_;
  // 判据是“本帧动画/过渡真的算出了覆盖”而不是“设过动画”：后者在播完之后
  // 仍为真（`fill: none` 时终态就是原样式），那时应当回到零成本的 `style_`。
  if (state->anim_props.empty() && !state->transition_running) return style_;
  return state->animated_style;
}

auto Element::content_box() const noexcept -> math::Rect { return bounds_.inset(style_.padding); }

void Element::measure(const RenderContext& context, const Constraints& constraints) {
  const float horizontal_padding = style_.padding.horizontal();
  const float vertical_padding = style_.padding.vertical();
  const float available_width =
      std::max(0.0f, (style_.has_explicit_width() ? style_.width : constraints.max_width) -
                         horizontal_padding);
  const float available_height =
      style_.has_explicit_height() ? style_.height - vertical_padding : constraints.max_height;

  const bool row = style_.direction == FlexDirection::Row;
  // **wrap 只在有可用宽约束时才可能生效**：没有上界就永远装得下，自然只有一行。
  const bool wrapping = row && style_.wrap && available_width < kUnbounded;
  float main_total = 0.0f;
  float cross_max = 0.0f;
  std::size_t visible_children = 0;
  std::vector<math::Size> child_sizes;
  if (wrapping) child_sizes.reserve(children_.size());

  for (auto& child : children_) {
    if (!child->visible_) continue;
    Constraints child_constraints;
    child_constraints.max_width = row ? kUnbounded : available_width;
    child_constraints.max_height = row ? available_height : kUnbounded;
    child_constraints.available_width = available_width;
    child_constraints.available_height = available_height;
    child->measure(context, child_constraints);
    const math::Size child_size = child->measured_size();
    main_total += main_size(style_.direction, child_size);
    cross_max = std::max(cross_max, cross_size(style_.direction, child_size));
    if (wrapping) child_sizes.push_back(child_size);
    ++visible_children;
  }
  if (visible_children > 1) main_total += style_.gap * static_cast<float>(visible_children - 1);

  float width = 0.0f;
  float height = 0.0f;
  if (row) {
    if (wrapping && !child_sizes.empty()) {
      // 换行容器的尺寸**不能**把小节点宽高"全累加"（那是单行语义）：
      // 宽度取最宽的一行、高度取各行高之和。缺了这段，容器高度只等于最高的一行，
      // 于是第二行起被父容器裁掉——图标全表这类界面会直接少一半（实测踩到）。
      float line_width = 0.0f;
      float line_height = 0.0f;
      float widest_line = 0.0f;
      float total_height = 0.0f;
      bool line_empty = true;
      for (const math::Size& size : child_sizes) {
        if (!line_empty && line_width + style_.gap + size.width > available_width) {
          widest_line = std::max(widest_line, line_width);
          total_height += line_height + style_.gap;
          line_width = size.width;
          line_height = size.height;
        } else {
          line_width += (line_empty ? 0.0f : style_.gap) + size.width;
          line_height = std::max(line_height, size.height);
        }
        line_empty = false;
      }
      widest_line = std::max(widest_line, line_width);
      total_height += line_height;
      // 行宽不会超过可用宽（超过就换行了）；夹一刀防止单个超宽子节点把容器撑破
      width = std::min(widest_line, available_width);
      height = total_height;
    } else {
      width = main_total;
      height = cross_max;
    }
  } else {
    width = cross_max;
    height = main_total;
  }

  if (style_.has_explicit_width()) {
    width = style_.width;
  } else {
    width += horizontal_padding;
  }
  if (style_.has_explicit_height()) {
    height = style_.height;
  } else {
    height += vertical_padding;
  }

  width = std::clamp(width, style_.min_width, style_.max_width);
  height = std::clamp(height, style_.min_height, style_.max_height);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void Element::place_absolute(const RenderContext& context, Element& child, math::Rect content) {
  const Style& child_style = child.style();
  math::Size size = child.measured_size();
  if (child_style.width != kAuto) size.width = child_style.width;
  if (child_style.height != kAuto) size.height = child_style.height;
  float x = content.x + child_style.left;
  float y = content.y + child_style.top;
  // 右/下缘锚定：优先于 left/top（两者同给时以**具体边**为准，与 CSS 的
  // “left+right 同时给则由宽度反解”不同——这里选择更可预测的那个：
  // 贴右缘是常见需求，而“又贴左又贴右”等于让别人替你算宽，不如用 width）。
  //
  // ⚠ 右缘基准用的是**本元素的 bounds_ 右缘**，不是 `content` 的：
  // `content` 已扣过 padding，贴右缘若按它算会往左缩一个 padding。
  // 而“贴右缘”的直觉是“贴容器的右边缘”（与 CSS 一致）。
  if (child_style.right >= 0.0f) x = bounds_.right() - child_style.right - size.width;
  if (child_style.bottom >= 0.0f) y = bounds_.bottom() - child_style.bottom - size.height;
  child.arrange(context, math::Rect{x, y, size.width, size.height});
}

void Element::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  layout_children(context, content_box());
  layout_dirty_ = false;
}

  void Element::layout_children(const RenderContext& context, math::Rect content) {
  const bool row = style_.direction == FlexDirection::Row;
  std::vector<Element*> visible;
  visible.reserve(children_.size());
  for (auto& child : children_) {
    if (!child->visible_) continue;
    // 绝对定位的子元素**不进流**：它不占位、不影向兄弟与父的尺寸。
    // 单独在函数末尾摆放（见 `place_absolute`）——两件事必须分开，
    // 否则“叠在上层的东西”会把下面的兄弟挤走（那正是没有叠放能力的旧症状）。
    if (child->style().absolute) {
      place_absolute(context, *child, content);
      continue;
    }
    visible.push_back(child.get());
  }
  if (visible.empty()) return;

  // —— 换行排布（`wrap`）——
  //
  // 实现方式：先分行（每行能装下多少），再逐行排。两趟是必要的——
  // 行高由该行**最高**的子元素决定，而"谁和谁同一行"又取决于行宽，
  // 一遍扫无法同时知道两件事（试过一遍扫，结果是与 `measure` 的估算不一致）。
  if (row && style_.wrap && content.width < kUnbounded) {
    struct Line {
      std::size_t begin{0};
      std::size_t end{0};
      float height{0.0f};
      float width{0.0f};
    };
    std::vector<Line> lines;
    for (std::size_t index = 0; index < visible.size(); ++index) {
      Element* child = visible[index];
      const auto& child_style = child->style();
      float main_extent = main_size(style_.direction, child->measured_size());
      if (child_style.width != kAuto) main_extent = child_style.width;
      const float cross_extent = cross_size(style_.direction, child->measured_size());
      if (lines.empty()) lines.push_back(Line{});
      Line& line = lines.back();
      const float needed = line.end == line.begin
                               ? main_extent
                               : line.width + style_.gap + main_extent;
      if (line.end > line.begin && needed > content.width) {
        lines.push_back(Line{.begin = index, .end = index + 1, .height = cross_extent,
                             .width = main_extent});
        continue;
      }
      line.end = index + 1;
      line.width = needed;
      line.height = std::max(line.height, cross_extent);
    }

    float cursor_y = content.y;
    for (const Line& line : lines) {
      float cursor_x = content.x;
      for (std::size_t index = line.begin; index < line.end; ++index) {
        Element* child = visible[index];
        const auto& child_style = child->style();
        float main_extent = main_size(style_.direction, child->measured_size());
        if (child_style.width != kAuto) main_extent = child_style.width;
        float cross_extent = cross_size(style_.direction, child->measured_size());
        if (child_style.height != kAuto) cross_extent = child_style.height;

        const Align align = child_style.align_self != Align::Stretch ? child_style.align_self
                                                                     : style_.align_items;
        float cross_offset = 0.0f;
        if (align == Align::Center) cross_offset = (line.height - cross_extent) * 0.5f;
        else if (align == Align::End) cross_offset = line.height - cross_extent;

        child->arrange(context, math::Rect{cursor_x, cursor_y + cross_offset, main_extent,
                                           cross_extent}
                                      .inset(child_style.margin));
        cursor_x += main_extent + style_.gap;
      }
      cursor_y += line.height + style_.gap;
    }
    return;
  }

  const auto count = static_cast<float>(visible.size());
  const float total_gap = style_.gap * (count - 1.0f);
  const float main_available = row ? content.width : content.height;
  const float cross_available = row ? content.height : content.width;

  float fixed_main = 0.0f;
  float grow_weight = 0.0f;
  for (const auto* child : visible) {
    const auto& child_style = child->style();
    const float size = main_size(style_.direction, child->measured_size());
    const float explicit_size = row ? child_style.width : child_style.height;
    if (child_style.grow) {
      grow_weight += 1.0f;
      continue;
    }
    fixed_main += explicit_size == kAuto ? size : explicit_size;
  }

  float leftover = main_available - total_gap - fixed_main;
  if (leftover < 0.0f) leftover = 0.0f;
  const float grow_unit = grow_weight > 0.0f ? leftover / grow_weight : 0.0f;

  // **把均分产生的整像素余数按顺序补给前几个 `grow` 子项**。
  //
  // ## 为何不能直接均分（实测代价）
  //
  // `leftover / grow_weight` 常常落在半像素上：gallery 的四张统计卡是
  // 858 可用宽 - 3×8 gap = 834，÷4 = **208.5**，于是每张卡宽 214.5、
  // 第二张卡的 x 落在 **490.5**。后果是卡片的**左右边界跨在半个像素上**，
  // 1px 描边被劈成两半（各 50% 覆盖）——观感是“左右边缘发虚”。
  //
  // 实测同一张卡的两个方向的边界剖面（暗色）：
  //   上边缘（y 为整数）  14 → **88**          一处锐利过渡
  //   右边缘（x 为 .5）   54 → **50** → 9      分散在两三个像素上
  // 这种“只在一个方向发虚”最容易被读成渲染 bug。
  //
  // ## 为何是“补给前几个”而不是四舍五入每个
  //
  // 四舍五入每个子项会让总宽不再等于可用宽（4×208.5 各自取整后可能多出 2px），
  // 表现为末尾空隙或溢出。按顺序发余数则总和不变，且偏差均摊（每个子项最多差 1px）。
  // 与 Chromium 的 `LayoutUnit` 处理同构。
  const int grow_count = static_cast<int>(grow_weight);
  const int base_extent = static_cast<int>(grow_unit);
  int grow_remainder = 0;
  if (grow_count > 0) {
    grow_remainder = static_cast<int>(std::lround(leftover)) - base_extent * grow_count;
  }
  int grow_index = 0;

  float cursor = row ? content.x : content.y;
  float used = total_gap + fixed_main;
  for (auto* child : visible) {
    const auto& child_style = child->style();
    float main_extent = main_size(style_.direction, child->measured_size());
    const float explicit_main = row ? child_style.width : child_style.height;
    if (explicit_main != kAuto) main_extent = explicit_main;
    if (child_style.grow) {
      // 前 `grow_remainder` 个子项各多拿 1px（余数均摊），其余崄基准值。
      main_extent = static_cast<float>(base_extent +
                                       (grow_index < grow_remainder ? 1 : 0));
      ++grow_index;
    }

    float cross_extent = cross_size(style_.direction, child->measured_size());
    const float explicit_cross = row ? child_style.height : child_style.width;
    if (explicit_cross != kAuto) cross_extent = explicit_cross;

    const Align align = child_style.align_self != Align::Stretch ? child_style.align_self
                                                                 : style_.align_items;
    float cross_offset = 0.0f;
    if (align == Align::Stretch) {
      cross_extent = cross_available;
    } else if (align == Align::Center) {
      cross_offset = (cross_available - cross_extent) * 0.5f;
    } else if (align == Align::End) {
      cross_offset = cross_available - cross_extent;
    }

    math::Rect target;
    if (row) {
      target = math::Rect{cursor, content.y + cross_offset, main_extent, cross_extent};
    } else {
      target = math::Rect{content.x + cross_offset, cursor, cross_extent, main_extent};
    }
    target = target.inset(child_style.margin);
    child->arrange(context, target);

    cursor += main_extent + style_.gap;
    used += main_extent;
  }

  // 主轴剩余空间分配（无 grow 子节点时按 justify 处理）
  if (grow_weight == 0.0f && style_.justify != Justify::Start) {
    const float slack = main_available - used;
    if (slack > 0.0f) {
      float offset = 0.0f;
      float extra_gap = 0.0f;
      const std::size_t gaps = visible.size() > 1 ? visible.size() - 1 : 0;
      switch (style_.justify) {
        case Justify::Center: offset = slack * 0.5f; break;
        case Justify::End: offset = slack; break;
        case Justify::SpaceBetween:
          extra_gap = gaps > 0 ? slack / static_cast<float>(gaps) : 0.0f;
          break;
        case Justify::SpaceAround:
          extra_gap = slack / count;
          offset = extra_gap * 0.5f;
          break;
        case Justify::Start: break;
      }
      float shift = row ? content.x : content.y;
      shift += offset;
      for (auto* child : visible) {
        math::Rect moved = child->bounds();
        if (row) {
          moved.x += shift - (row ? content.x : content.y);
        } else {
          moved.y += shift - (row ? content.x : content.y);
        }
        child->arrange(context, moved);
        shift += (row ? moved.width : moved.height) + style_.gap + extra_gap;
      }
    }
  }
}

/// 悬浮特效的**唯一实现点**（基类绘制）：背景提亮 / 描边变色 / 上浮 / 外发光。
///
/// 为什么放在基类而不是每个组件里：悬浮反馈是**跨组件一致性**问题——
/// 按钮、列表项、表格行、标签页各写一套，观感一定会散（间距、时长、色阶都不同）。
/// 组件只需声明"要哪些效果"（`HoverEffect`），参数由主题令牌统一给。
void Element::paint_box(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  // 动画值在这里生效：`paint_style()` 无动画时就是 `style_` 本身（零开销），
  // 有动画时是「样式 + 动画覆盖」的快照。**不能把动画值写回 `style_`**——那是
  // 真实样式，主题切换/控制通道读回会看到被污染的假值（详见 `paint_style` 的说明）。
  const Style& style = paint_style();
  const auto& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  // 悬浮进度（0..1）：未开特效的组件恒为 0，走的是原有路径，零额外开销
  const float hover_t = hover_effect_.enabled ? advance_hover(context) : 0.0f;
  const bool hovering = hover_t > 0.001f;
  // 上浮：把整块框体（背景/描边/阴影）上移，配合阴影形成"抬起"感。
  // 只移框体、不移内容？——不移内容会显得文字"陷"在框里。
  // 因此这里把绘制用的框体整体上移，内容由组件自己的 `paint_content` 决定
  // （组件若要用上浮，从 `hover_lift_offset()` 取同一个位移）。
  const math::Rect box = hovering && hover_effect_.lift
                             ? bounds_.offset(0.0f, -metrics.hover_lift * hover_t)
                             : bounds_;
  math::Color background = style.background;
  math::Color border = style.border_color;
  if (hovering && hover_effect_.background) {
    // **不透明底要"同向提亮/压暗"，不能换成中性灰**。
    //
    // 旧实现是 `background.mix(surface_hover, hover_t)`，而 `hover_t` 会收敛到 1——
    // 于是 hover 稳定后它是一个**完全替换**：实测暗色下主按钮 `#8b9aff`
    // 悬停后变成 `#2b2b30` 中性灰，按钮看起来像"被禁用"。
    //
    // 幅度取 `surface_hover` 相对 `surface` 的**实际明度偏移**（两个都是实色：
    // 亚克力调色板里它们已经不透明了），命中 `surface` 时正好等于主题定的悬浮档。
    // ⚠ 不要用 `surface_hover.a / 255` 当幅度：那是 alpha（现在恒为 1），
    // 会得到 0.45 的荒谬幅度（实测把 `#15151a` 直接抬到 `#7e7e81`）。
    const float up = static_cast<float>(colors.surface_hover.r) -
                     static_cast<float>(colors.surface.r);
    const float down = static_cast<float>(colors.surface.r) -
                       static_cast<float>(colors.surface_pressed.r);
    if (background.a == 0U) {
      // 无底（Ghost）平铺一层半透明中性色：它的"语气"本就不在底色上。
      //
      // ⚠ 这里**不能用 `surface_hover`**：Ghost 按钮的宿主底色通常就是
      // `surface_alt`（活动栏/标题栏/标签栏），而 `surface_hover` 与它几乎同色——
      // 实测暗色下 hover 后底色仅从 `(42,46,69)` 变 `(52,56,78)`（Δ≈10/255，
      // 约 4%），用户直接读成“没反应”（反馈：菜单栏/活动栏悬浮响应又慢又不好）。
      //
      // 改用**白/黑半透光**：在任意底色上都能拉开亮度，强度与 `border` 同一口径
      // （`theme.hpp` 亚克力口径：描边用半透光而不是合成实色，才能在四种底色上强度一致）。
      // 暗色提亮、亮色压暗——跟随 `surface_hover` 相对 `surface` 的方向，
      // 不是写死“提亮”（亮色主题里提亮会直接消失）。
      const bool host_is_dark = colors.surface_alt.r < 128U;
      const float strength = 0.10f * hover_t;   // 稳定后 10% 白/黑光
      const std::uint8_t channel = host_is_dark ? 255U : 0U;
      background = math::Color{channel, channel, channel,
                               static_cast<std::uint8_t>(strength * 255.0f)};
    } else {
      const bool dark_base = static_cast<float>(background.r) < 128.0f;
      const float delta = dark_base ? up : -down;
      // **按下比悬停再深一档**：否则两者像素完全相同，用户看不到"按下去了"
      // （实测：`pressed_` 在旧实现里算出的色因为 `apply_theme` 不重跑而永远不生效，
      // 于是按下与悬停一模一样）。系数 1.6 取自 `surface_pressed` 相对 `surface_hover`
      // 的额外偏移量级（暗色 36 vs 23、亮色 23 vs 13）。
      const float pressed_boost = pressed_ ? 1.6f : 1.0f;
      const float amount = delta / 255.0f * hover_t * pressed_boost;
      background = amount >= 0.0f ? background.lighten(amount) : background.darken(-amount);
      // 按下时把透明度抬满：`Soft` 这类带 alpha 的底在叠加后仍要看得出变化。
      if (pressed_ && background.a != 0U) background.a = 255U;
    }
  } else if (pressed_ && hover_effect_.enabled && hover_effect_.background &&
             style.background.a != 0U) {
    // 极窄的一条缝：按下了但 hover_t 尚未起来（如程序化 `set_pressed`）。
    // 给一个固定压暗，保证"按下"本身在任何情况下都可见。
    //
    // ⚠ 这一支**必须一并看 `enabled`**：它不经过 `hover_t`（`advance_hover` 只在
    // `enabled` 时才被调用），所以只查 `background` 就会在关掉特效的组件上
    // 仍然压暗——实测：标题栏显式 `set_hover_effect({.enabled=false})` 后，
    // 按住标题栏拖动时整条栏依旧从 #EDEDF2 变 #DADADF。
    background = background.darken(0.08f);
  }
  if (hovering && hover_effect_.border && colors.border_hover.a != 0U) {
    border = border.mix(colors.border_hover, hover_t);
  }
  // 外发光：把强调色以低不透明度铺一层**放大**的圆角矩形（v0.2 的阴影/模糊机制复用）
  if (hovering && hover_effect_.glow && colors.glow.a != 0U) {
    const float spread = metrics.hover_glow_width * hover_t;
    const math::Color glow{colors.glow.r, colors.glow.g, colors.glow.b,
                           static_cast<std::uint8_t>(colors.glow.a * hover_t)};
    canvas.fill_rect(box.inflate(spread), raster::Paint::solid(glow),
                     style.radius + spread, raster::DrawOptions{.opacity = style.opacity});
  }
  if (style.shadow.visible() && !style.shadow.inset) {
    // 上浮时阴影跟着走（否则“抬起”会被阴影钉在原地，看着像两层错位）
    const math::Rect shadow_box = box.inset(style.margin);
    // **告诉后端“哪块随后会被不透明底盖掉”**（`covered_region`）。
    //
    // 语义：下文（第 820 行附近）会用 `background` 以同半径铺满 `box`。若它
    // **不透明**（`a == 255`）、`opacity >= 0.999` 且 `SrcOver`，则阴影落在
    // `box` 内的部分**必然被完全覆盖**——那部分先画一遍是纯浪费。
    //
    // 正确性是硬保证而非近似（`src-over` 结合律）：设贴图贡献 U、底色 B 不透明，
    // 则 `over(B, over(D, U)) == B` 对任意 D、U 成立（不透明源的 `over` 就是替换）。
    // 后端据此按行解析出精确跨度跳过（圆角处不能整矩形跳，否则四角丢阴影）。
    //
    // 为何要显式判 `opacity >= 0.999`：半透明合成后底色仍半透明，不能跳
    //（契约第 1 条要求“完全覆盖”，而 `opacity < 1` 的底不满足）。
    const bool can_skip_covered = background.a == 255U && style.opacity >= 0.999f;
    const auto options = raster::DrawOptions{
        .opacity = style.opacity,
        .covered_region = can_skip_covered ? box : math::Rect{},
        .covered_radius = can_skip_covered ? style.radius : 0.0f,
    };
    // **走 `draw_shadow_layered`，不是调两次 `draw_shadow`**：
    // 两层的遮罩都只取决于几何，后端可以把它们预合成成**一张**缓存贴图、只做一次
    // 逐像素合成（软件光栅器就是这样做的）。实测 96 张卡片：14.0 ms/帧 → 6.9 ms/帧
    // （`docs/PAINT_DIAGNOSIS.md` §3.1）。语义（先环境层、后关键层）写进了接口，
    // 因此换后端不会改变观感。
    //
    // 单层阴影（`second_visible()` 为假）时 `ambient_blur` 传 0——后端据此退回
    // 单层路径，行为与以前逐像素相同。
    const bool has_ambient = style.shadow.second_visible();
    canvas.draw_shadow_layered(
        shadow_box, style.radius, style.shadow.color, style.shadow.blur,
        math::Point{style.shadow.offset_x, style.shadow.offset_y},
        has_ambient ? style.shadow.color2 : math::Color{0, 0, 0, 0},
        has_ambient ? style.shadow.blur2 : 0.0f,
        math::Point{style.shadow.offset2_x, style.shadow.offset2_y}, options);
  }
  // **渐变底优先**：设了 `background_gradient` 就用它（`background` 退为回退色）。
  //
  // `Style` 里存的是 **0~1 归一化**几何（`apply_theme` 跑在布局之前，拿不到尺寸），
  // 而光栅器要的是**绝对逻辑坐标**（`canvas.cpp` 按 `(p - from) / (to - from)` 算 t）。
  // 这里做一次映射：
  //
  //   `from_box = box.xy + from_norm * box.wh`
  //   `to_box   = box.xy + to_norm   * box.wh`
  //
  // 对圆形/径向渐变同理（圆心与半径一起缩放）。非线性的径向/扫掠这里**不处理**：
  // 目前只有线性渐变在用（主按钮的双色），非线性的语义需要“非等比缩放”决策，
  // 与其拍一个再重做，不如等真有需求时按那时候的场景定。
  bool painted_gradient = false;
  if (style.background_gradient.has_value() &&
      !style.background_gradient->stops().empty() && style.gradient_opacity > 0.0f) {
    const raster::Gradient& source = *style.background_gradient;

    // ① 色标：悬停/按下的明度偏移必须也作用到渐变，否则主按钮悬浮时“没反应”——
    //    渐变分支不看 `background`，而上面那整段提亮/压暗改的正是 `background`。
    //    用与纯色路径**同一个量**，只是把它应用到每个色标上，而不是重算一套。
    std::vector<raster::GradientStop> stops;
    stops.reserve(source.stops().size());
    const float grad_up =
        static_cast<float>(colors.surface_hover.r) - static_cast<float>(colors.surface.r);
    const float grad_down =
        static_cast<float>(colors.surface.r) - static_cast<float>(colors.surface_pressed.r);
    const bool dark_base = colors.surface.r < 128U;
    const float delta = dark_base ? grad_up : -grad_down;
    const float pressed_boost = pressed_ ? 1.6f : 1.0f;
    const float amount = delta / 255.0f * hover_t * pressed_boost;
    for (const auto& stop : source.stops()) {
      math::Color color = stop.color;
      if (hovering && hover_effect_.background && amount != 0.0f) {
        color = amount >= 0.0f ? color.lighten(amount) : color.darken(-amount);
      }
      stops.push_back(raster::GradientStop{stop.offset, color});
    }

    // ② 几何：`Style` 存的是 **0~1 归一化**，光栅器要**绝对坐标**，
    //    所以三种渐变都要映射（不只线性）。
    //
    //    ⚠ 只映射线性时，径向/扫掠的圆心会停在画布原点附近而半径按 0~1 算，
    //    于是整个元素被推出渐变范围、clamp 到**端点色**——实测整块纯色（中心与
    //    角落同色 `(0,0,255)`），看上去像“没画渐变”。
    const auto map_point = [&](math::Point p) {
      return math::Point{box.x + p.x * box.width, box.y + p.y * box.height};
    };
    // 半径按**较短边**缩放：0~1 的半径语义是“相对元素尺寸”，
    // 用较长边会让圆形渐变在宽元素上被拉成椭圆（CSS 的 `farthest-side` 同口径）。
    const float radius_scale = std::min(box.width, box.height);
    // ⚠ `Gradient` 的构造函数是私有的（只能经 static 工厂创建），所以不能用
    // “先默认构造再 switch 赋值”；这里用 `std::optional` 承接，每个分支各造一份。
    std::optional<raster::Gradient> mapped;
    switch (source.kind()) {
      case raster::Gradient::Kind::Linear:
        mapped = raster::Gradient::linear(map_point(source.start()), map_point(source.end()),
                                          std::move(stops));
        break;
      case raster::Gradient::Kind::Radial:
        mapped = raster::Gradient::radial(map_point(source.start()),
                                          source.radius() * radius_scale, std::move(stops));
        break;
      case raster::Gradient::Kind::Sweep:
        // 扫掠是纯角度量，圆心要映射，半径与角度无关联（无需缩放）。
        mapped = raster::Gradient::sweep(map_point(source.start()), std::move(stops));
        break;
    }
    canvas.fill_rect(box, raster::Paint::with_gradient(std::move(*mapped)), style.radius,
                     raster::DrawOptions{.opacity = style.opacity * style.gradient_opacity});
    painted_gradient = true;
  }
  if (!painted_gradient && background.a != 0U) {
    canvas.fill_rect(box, raster::Paint::solid(background), style.radius,
                     raster::DrawOptions{.opacity = style.opacity});
  }
  // **内阴影（`inset`）在底色之后画**——它要压在底色之上才看得见。
  //
  // ## 为何不用 `draw_shadow`（实测踩到，两轮才想清楚）
  //
  // 最初想“内阴影 = 外投影的偏移取反 + 裁剪到框内”，以为能直接复用。
  // 但 `draw_shadow` 的遮罩语义是「**矩形内满强度、矩形外只有 blur 宽度的衰减带**」
  // （见 `tools/draw_shadow_profile_probe.cpp` 的实测剖面：矩形内 3.1 恒定、
  // 边缘 4~6px 内降到 0）。于是把矩形放到框内 → 整块都吃满阴影，
  // 变成了“换了个更深的底色”而不是“边缘压暗”。把矩形外扩也不行：
  // 衰减带只有 blur 宽，而框宽几百像素，中心永远落在矩形内。
  //
  // ## 正确的形状：四条渐变带（与 CSS `inset box-shadow` 同构）
  //
  // 每条带从**框边**的 `shadow.color` 渐变到**框内 `spread` 处**的透明；
  // 四条叠在一起就是“四周内缘压暗”。用圆角裁剪把四角收齐。
  // 偏移（`offset_x/offset_y`）通过**偏置四条带的过渡长度**表达方向：
  // 偏移为正的那一侧带更窄（光从那侧来），负的那侧更宽。
  if (style.shadow.visible() && style.shadow.inset) {
    const float spread = std::max(style.shadow.blur * 2.0f, 2.0f);
    const math::Color edge = style.shadow.color;
    const math::Color clear = edge.with_alpha(0);
    // 偏移归一化到 [-1, 1]，用来把两侧过渡长度调成不对称。
    const float ox = std::clamp(style.shadow.offset_x / spread, -1.0f, 1.0f);
    const float oy = std::clamp(style.shadow.offset_y / spread, -1.0f, 1.0f);
    // 偏移为正 ⇒ 该侧变窄（受光），为负 ⇒ 变宽（背光）。
    const float top_len = spread * (1.0f - oy * 0.5f);
    const float bottom_len = spread * (1.0f + oy * 0.5f);
    const float left_len = spread * (1.0f - ox * 0.5f);
    const float right_len = spread * (1.0f + ox * 0.5f);

    canvas.push_clip_rounded_rect(box, style.radius);
    const auto band = [&](math::Point from, math::Point to, math::Rect rect) {
      if (rect.width <= 0.0f || rect.height <= 0.0f) return;
      canvas.fill_rect(
          rect,
          raster::Paint::with_gradient(raster::Gradient::linear(
              from, to,
              std::vector<raster::GradientStop>{raster::GradientStop{0.0f, edge},
                                                raster::GradientStop{1.0f, clear}})),
          0.0f, raster::DrawOptions{.opacity = style.opacity});
    };
    const float ix = box.x + std::min(style.margin.left, 0.0f);
    const float iy = box.y + std::min(style.margin.top, 0.0f);
    const float iw = box.width;
    const float ih = box.height;
    // 上带：从顶边往下渐变
    band(math::Point{ix, iy}, math::Point{ix, iy + top_len},
         math::Rect{ix, iy, iw, top_len});
    // 下带：从底边往上渐变
    band(math::Point{ix, iy + ih}, math::Point{ix, iy + ih - bottom_len},
         math::Rect{ix, iy + ih - bottom_len, iw, bottom_len});
    // 左带
    band(math::Point{ix, iy}, math::Point{ix + left_len, iy},
         math::Rect{ix, iy, left_len, ih});
    // 右带
    band(math::Point{ix + iw, iy}, math::Point{ix + iw - right_len, iy},
         math::Rect{ix + iw - right_len, iy, right_len, ih});
    canvas.pop_clip();
  }
  // 顶部内高光（亚克力的“玻璃边缘”）：最后画，压在描边之上。
  //
  // 为什么不是一条实线：等宽实线在两端会被读成“多了一道边框”，
  // 而玻璃边缘的受光是中间强、向两侧衰减的。渐变两端 alpha 归零，于是自然消失在
  // 圆角处（不需要额外裁剪，也不会在圆角处露出直角）。
  //
  // **2026-10-27 改为径向（左上受光）**：横向单条在宽卡片上会被读成“贴了一条亮胶带”——
  // 因为真实的受光不在“顶边中点”，而在**光源方向**（约定左上）。
  // 改成以左上角为圆心的径向后，亮度沿对角线向向衰减，与 `Shadow` 的“光从上来”
  // （内阴影压在上内缘）方向一致——两者方向一致才像一个物体，否则会自相矛盾。
  if (style.top_highlight.a != 0U && style.highlight_width > 0.0f && box.width > 0.0f) {
    const math::Color edge = style.top_highlight;
    const math::Color fading = edge.with_alpha(0);
    // 只占顶边**内沿**那一条（高度 = highlight_width）：径向渐变的垂直影响
    // 天然衰减得很快，但把它限制在一条带里才能保证“不下泼到内容区”。
    const math::Rect band{box.x, box.y, box.width, style.highlight_width};
    // 径向半径：取宽度的 55%。太小 → 只有左上一小段亮（像块污渍）；
    // 太大 → 整条顶边均匀发亮，又退化回“亮胶带”。宽高都参与，使窄卡也不会局部过亮。
    const float reach = std::max(box.width, box.height * 3.0f) * 0.55f;
    const raster::Gradient gradient = raster::Gradient::radial(
        math::Point{band.x, band.y}, reach,
        std::vector<raster::GradientStop>{raster::GradientStop{0.0f, edge},
                                          raster::GradientStop{0.45f, edge.with_alpha(
                                                                            static_cast<std::uint8_t>(
                                                                                edge.a / 3))},
                                          raster::GradientStop{1.0f, fading}});
    // 裁剪到圆角内：半径大于 0 时顶边那段会被圆角削掉，不裁就会在角上多出一小块。
    if (style.radius > 0.0f) canvas.push_clip_rounded_rect(box, style.radius);
    canvas.fill_rect(band, raster::Paint::with_gradient(gradient), 0.0f,
                     raster::DrawOptions{.opacity = style.opacity});
    if (style.radius > 0.0f) canvas.pop_clip();
  }
  if (style.border_width > 0.0f && border.a != 0U) {
    // **环形填充**而不是描边：卡片边框是界面里出现次数最多的描边，而 `stroke_path`
    // 把圆角矩形展开成 ~152 条边（每段一个四边形 + 顶点补圆），环形只有 76 条边。
    // 实测 96 张卡片：6.9 ms/帧 → 3.9 ms/帧（`docs/PAINT_DIAGNOSIS.md` §2.2）。
    //
    // **矩形先对齐到整像素再画边**：环形填充是**向内**的，所以只要外缘落在整像素上，
    // 1px 的边就恰好盖满一整行（列）像素——这是边框锐利的充分条件。
    // 不齐时外缘被抗锯齿摊在两行上、各约 50%，看上去就是"边缘发虚"：
    // 实测小数坐标的卡片 `y=143.727` 上缘是 `#131317`+`#2a2f2f` 两行各半，
    // 而整像素的 `y=244` 是一行满值 `#313136`。
    //
    // 只对齐**边框这一道**（不对齐背景/阴影/文本）：它们的观感取决于面积与
    // 连续位置，硬吸到格上会让卡片高度跳变、圆角起阶梯（弧线无法同时对齐两边）。
    const math::Rect snapped{
        std::round(box.x), std::round(box.y),
        std::max(0.0f, std::round(box.right()) - std::round(box.x)),
        std::max(0.0f, std::round(box.bottom()) - std::round(box.y))};
    canvas.fill_path(
        math::make_rounded_border_ring(snapped, style.radius, style.border_width),
        raster::Paint::solid(border),
        raster::DrawOptions{.opacity = style.opacity});
  }
}

auto Element::paint_text(const RenderContext& context, raster::Surface& canvas, std::string_view text,
                         math::Rect box) const -> void {
  if (text.empty()) return;
  // 字号/字色/对齐都取**本帧生效样式**（动画可以动 `font-size` 与 `color`）。
  const Style& style = paint_style();
  const TextPort& port = text_port_or_null(context);
  const float size = style.font_size;
  const std::string clipped = port.ellipsize(text, size, box.width);
  if (clipped.empty()) return;
  const float width = port.measure_width(clipped, size);
  float x = box.x;
  if (style.text_align == TextAlign::Center) {
    x = box.x + (box.width - width) * 0.5f;
  } else if (style.text_align == TextAlign::End) {
    x = box.right() - width;
  }
  // **按墨迹区居中**（全仓统一口径，见 `st::ui::centered_line_top`）。
  //
  // 行高含字体预留的头尾空间（实测 DejaVu Sans `hhea.ascender = 0.928em`，
  // 而大写字母墨迹只有 `0.729em`），按行盒居中会把文字**推下约 2.5px**。
  // `centered_line_top` 在端口报不出墨迹时自动退回行盒居中。
  const float y = centered_line_top(port, clipped, size, box.y, box.height);
  // **字重落像素**：框架没有独立字重的字体面，非 Regular 档用合成加粗（沿水平外扩）。
  // 不读 `style.font_weight` 时，界面里所有 SemiBold/Bold 都与 Regular 逐像素相同——
  // 「字重」就只是个落不到画面上的属性。
  //
  // 端口只持有物理口径的语义量（半径），采样格步数由渲染器按自己的模式换算。
  const float physical = size * canvas.device_scale();
  // **Bold 档优先走真粗体字体面**（实测比合成加粗锐 36~37%、英文字间离散 14.1%→0%，
  // 见 `prefers_real_bold`）：此时**不再叠加合成加粗**——叠了会把真粗体的字再撑开、
  // 字腔粘住。探不到真粗体面时自动回退常规面，此时仍用合成加粗补足（Medium/SemiBold 同理）。
  const bool real_bold = prefers_real_bold(style.font_weight) && port.has_real_bold();
  const float embolden = real_bold ? 0.0f : embolden_radius(physical, style.font_weight);
  port.draw(canvas, clipped, math::Point{x, y}, size, style.color, text::FontRole::Proportional,
            embolden, real_bold);
}

void Element::paint(const RenderContext& context, raster::Surface& canvas) const {
  // **外扩量回填必须在 `visible_` 检查之前**。
  //
  // 为何：`paint_margin_hint` 是“元素标脏时能安全上报多大损坏区”的依据，
  // 而它只能在绘制时拿到 `RenderContext`。旧位置在 visible 之后 ——
  // 于是**隐藏元素永远回填不了**，margin 恒为初值 −1，它一标脏就退化成整帧。
  //
  // 回填本身只算一次浮点（`paint_margin` 只依赖主题，与元素无关），
  // 放在这里对可见元素零额外开销。
  extras().paint_margin_hint = paint_margin(context);
  if (!visible_) return;
  // **动画推进必须早于一切几何与样式读取**：`paint_style()` / 位移量都在这一行之后
  // 才是本帧的值。与 `advance_hover` 同一个位置理由（绘制是元素的唯一时间源）。
  const bool animating = advance_animation(context);
  if (animating) request_animation();   // 走既有汇总链：播完自然不再续帧
  // **视口剔除**：与当前裁剪区无交集就整块不画。
  //
  // 为什么必须有：绘制是自顶向下的，一个排到屏幕外很远的元素（例如滚动到视野外的
  // 三维视图、长列表的尾部项）照样会被完整绘制——`SceneView` 因此每帧白付一次
  // 渲染+回读（实测 13.9 ms，而整帧其余部分才 4.3 ms）。事件、布局都不受影响，只有绘制跳过。
  //
  // 安全前提（两条，缺一就会"东西不见了"）：
  // ① 按**主题实际值**外扩（阴影/发光/上浮都会画到 bounds 之外，见 `paint_margin`）；
  // ② 自己无交集时还要看**后代**（框架允许子元素排到父容器之外）。
  {
    const math::IntRect physical = canvas.clip_rect();
    const float scale = canvas.device_scale();
    const float inv = scale > 0.0f ? 1.0f / scale : 1.0f;
    const math::Rect logical_clip{static_cast<float>(physical.x) * inv,
                                  static_cast<float>(physical.y) * inv,
                                  static_cast<float>(physical.width) * inv,
                                  static_cast<float>(physical.height) * inv};
    const float margin = paint_margin(context);
    // 外扩量已在函数入口回填（见那里的说明：隐藏元素也要回填）。
    if (!subtree_may_paint(*this, logical_clip, margin)) return;
  }
  if (context.painted_elements != nullptr) ++*context.painted_elements;
  // **元素级剖析**（关闭时只多两次判空，不读时钟）：计时包住整个 `paint_impl`，
  // 于是子类的绘制与它重写的流程都在这里面。自身耗时 = 子树 − 直接子元素
  // （子元素自己也调 `paint`，所以 `paint_cost_ns_` 已是含子孙的量）。
  perf::FrameProfiler* profiler =
      ::st::perf::enabled() ? ::st::perf::active_frame_profiler() : nullptr;
  if (profiler == nullptr) {
    paint_animated(context, canvas);
    return;
  }
  const std::int64_t element_start = ::st::perf::now_ticks();
  paint_animated(context, canvas);
  const std::int64_t subtree_ns = ::st::perf::ticks_to_ns(::st::perf::now_ticks() - element_start);
  // 只减**本帧真的画过**的子孙：被视口剔除的子元素不会重新计时，
  // 它手里的是旧帧的值——减它会把别人的账算进来（或得出负值）。
  const std::uint64_t serial = profiler->frame_serial();
  std::int64_t children_ns = 0;
  for (const auto& child : children_) {
    if (child->paint_cost_serial() == serial) children_ns += child->paint_cost_ns();
  }
  const std::int64_t self_ns = subtree_ns > children_ns ? subtree_ns - children_ns : 0;
  paint_cost_ns_ = subtree_ns;  // 供父元素相减（**必须记含子孙的量**）
  paint_cost_serial_ = serial;
  profiler->record_element(type(), id_, self_ns, subtree_ns);
}

void Element::paint_animated(const RenderContext& context, raster::Surface& canvas) const {
  const RenderExtras* state = extras_if_any();
  const anim::Props& props = state != nullptr ? state->anim_props : anim::Props{};
  const float opacity = props.opacity.has_value() ? std::clamp(*props.opacity, 0.0f, 1.0f) : 1.0f;
  const bool scaled = animation_scale_ != 1.0f;
  // 三条路：
  // ① 纯平移（最常见：滑入/抖动/上浮）——包一层 `TranslatedSurface`，零像素代价；
  // ② 缩放或子树透明度——只能离屏合成（缩放要重采样，透明度要乘到整个子树）；
  // ③ 无几何变化——直接画（零开销，与没有动画时完全同一条路径）。
  const bool need_offscreen = scaled || opacity < 0.999f;
  if (!need_offscreen) {
    if (animation_translate_.x != 0.0f || animation_translate_.y != 0.0f) {
      raster::TranslatedSurface shifted(canvas, animation_translate_);
      paint_impl(context, shifted);
      return;
    }
    paint_impl(context, canvas);
    return;
  }

  // —— 离屏合成 ——
  //
  // 区域取 `bounds + paint_margin`：阴影/发光/内高光都画到盒子外，
  // 按 bounds 截会把它们削掉（“缩放动画期间卡片边缘发硬”）。
  const float margin = paint_margin(context);
  const math::Rect region = bounds_.offset(animation_translate_.x, animation_translate_.y)
                                .inflate(margin);
  if (region.is_empty()) return;
  const int width = std::max(1, static_cast<int>(region.width));
  const int height = std::max(1, static_cast<int>(region.height));
  raster::Canvas scratch = raster::Canvas::for_logical_size(width, height, canvas.device_scale());
  // 画进 scratch 时把坐标平移到 scratch 原点，并**临时改走未含透明度的样式**：
  // 透明度由最后那次合成统一乘上（否则会叠两次：里面乘一次、外面再乘一次）。
  Style& original_style = extras().animated_style;
  const Style saved = original_style;
  if (opacity < 0.999f) {
    original_style = paint_style();
    original_style.opacity = style_.opacity;   // 去掉动画透明度（合成时统一乘）
  }
  const Style* saved_override = style_override_;
  style_override_ = &original_style;
  {
    raster::TranslatedSurface shifted(scratch, math::Point{-region.x, -region.y});
    paint_impl(context, shifted);
  }
  style_override_ = saved_override;
  original_style = saved;
  // 目标矩形：以 `bounds_` 中心为基准缩放（CSS `transform-origin: center` 默认）。
  const math::Point center = bounds_.offset(animation_translate_.x, animation_translate_.y).center();
  const float factor = animation_scale_;
  const math::Rect destination{
      center.x + (region.x - center.x) * factor,
      center.y + (region.y - center.y) * factor,
      region.width * factor,
      region.height * factor,
  };
  canvas.draw_canvas(scratch, destination, raster::DrawOptions{.opacity = opacity});
}

void Element::paint_impl(const RenderContext& context, raster::Surface& canvas) const {
  paint_box(context, canvas);
  paint_content(context, canvas);
  if (style_.clip_children) {
    canvas.push_clip_rounded_rect(bounds_, style_.radius);
    for (const auto& child : children_) child->paint(context, canvas);
    canvas.pop_clip();
    return;
  }
  for (const auto& child : children_) child->paint(context, canvas);
}

auto Element::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  // 基类不消费；但行为注入的 handler 仍要给机会（组件覆写若已消费则不会走到这里）。
  if (event_handler_) return event_handler_(event);
  return false;
}

// —— Panel / Spacer ——

Panel::Panel(FlexDirection direction) {
  style_.direction = direction;
  style_.background = math::Color{0, 0, 0, 0};
}

Spacer::Spacer(float size) {
  style_.width = size;
  style_.height = size;
  style_.grow = false;
}

void Spacer::arrange(const RenderContext& context, math::Rect rect) {
  // 副轴尺寸夹到非负：占位块没有内容，比它的容器还高时只会得到一个**负高度矩形**
  // （实测：30px 高的行里 `grow` 占位块的 `bounds.height == −18`）。矩形本身不画东西，
  // 但负尺寸会被协议/测试当成异常值读走——“这块多大”的答案不该是负数。
  Element::arrange(context, math::Rect{rect.x, rect.y, std::max(0.0f, rect.width),
                                       std::max(0.0f, rect.height)});
}

}  // namespace st::ui

// ── 基础框架容器的登记（`st/ui/component_registry.hpp`）────────────────────
//
// `Panel` / `Spacer` 定义在本文件（基类布局代码要用它们，搬到组件头会形成环），
// 但它们的**登记**同样要走这条路：声明在 `st/ui/components/panel.hpp`，
// 定义在这里。于是 `dsl::make_element("Panel")` 能造 `<Row>` / `<Column>`。
ST_UI_COMPONENT_IMPL_NAMED(::st::ui::Panel, "Panel")
ST_UI_COMPONENT_IMPL_NAMED(::st::ui::Spacer, "Spacer")

