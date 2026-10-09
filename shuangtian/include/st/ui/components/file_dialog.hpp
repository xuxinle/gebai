#pragma once

/// 通用文件对话框（打开/保存）：目录浏览 + 文件列表 + 文件名输入 + 确认/取消。
///
/// 形态：挂 `UiRoot` 为 `OverlayLayout::FillViewport` 浮层——遮罩铺满分到的矩形、
/// 居中卡片自绘（视觉规格与 `Dialog` 同源：`radius_xl`/`shadow_lg`/`surface`）。
/// 卡片内的目录行/文件列表/文件名输入全部**自绘**（不产生子 Element，与 Tree/List
/// 组件零耦合）；底部按钮行用 `Button` 子组件（与 `Dialog` 同一套做法）。
///
/// 交互：
/// - 目录导航：双击目录进入（`fs::list_dir` 重读）、`..` 返回上级；
/// - 列表键盘 ↑↓ 移动选中、Enter 确认（目录则进入、文件则确认全路径）；
/// - 文件名行可编辑（点击聚焦、TextInput 插入、Backspace 删除）；
/// - Esc/取消触发 `on_cancel`；确认时文件名为空则不触发 `on_confirm`。
/// `fs` 失败（不存在/无权限）呈现错误行，不崩溃。
///
/// 依赖纪律：文本经 `RenderContext::text`（空 → `NullTextPort`），颜色/间距/字号一律
/// 取 `theme` token；不 include text 层与 Tree/List 组件。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

class FileDialog : public Element {
 public:
  /// 三种模式：打开文件 / 保存文件 / **选择目录**。
  ///
  /// `Directory` 与另两种的差别不只在文案：
  /// * 确认返回的是**当前目录**（不是「目录/文件名」拼接）——文件名行在该模式下
  ///   不参与语义（界面上也隐藏）；
  /// * `confirm()` 的"文件名非空"前置条件换成"目录非空"。
  ///
  /// 为何要有它（2026-10-07）：应用需要"打开文件夹"（换工作区），而原先只有
  /// Open/Save，选目录只能靠外部 `--workspace` 参数定死，运行期换不了。
  enum class Mode : std::uint8_t { Open, Save, Directory };

  static constexpr float kMinWidth{620.0f};
  static constexpr float kMaxWidth{760.0f};
  static constexpr float kMinHeight{420.0f};
  static constexpr float kMaxHeight{480.0f};
  static constexpr float kPadding{20.0f};
  static constexpr float kRowHeight{32.0f};
  /// 位置侧栏宽（左列；窄了位置名被截、宽了列表被压——132 是"主目录/工作区"的舒适宽）。
  static constexpr float kPlacesWidth{132.0f};

  explicit FileDialog(Mode mode = Mode::Open, std::string title = {});

  /// 工厂：`FileDialog::make(Mode::Open, "打开文件")`。
  [[nodiscard]] static auto make(Mode mode, std::string title) -> std::unique_ptr<FileDialog>;
  /// 切换模式（声明式路径用：组件由 `create_element` 无参构造，模式得构造后设）。
  ///
  /// 切到 `Directory` 会清掉文件名——那个名字属于上一个模式，留着会让
  /// 确认按钮与语义对不上（该模式确认的是目录，不是「目录/文件名」）。
  void set_mode(Mode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    if (mode_ == Mode::Directory) filename_.clear();
    mark_layout_dirty();
  }
  [[nodiscard]] auto mode() const noexcept -> Mode { return mode_; }


  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "FileDialog"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Dialog; }
  /// 模态浮层：不可见时不拦截输入（与 `Dialog` 同一契约）。
  [[nodiscard]] auto intercepts_input() const noexcept -> bool override { return visible(); }

  /// 进入目录（同步 `fs::list_dir` 刷新列表；失败置错误行，列表清空不崩溃）。
  void set_directory(const std::string& path);
  [[nodiscard]] auto directory() const noexcept -> const std::string& { return directory_; }

  // —— 文件类型过滤（打开场景的"只看代码"）——
  /// 后缀白名单（含点小写，如 `.cpp`/`.md`；空 = 不过滤显示全部）。
  /// 目录永不过滤——过滤的本意是"找可打开的文件"，目录是导航必需。
  /// 匹配大小写不敏感（Windows 文件系统如此）。
  void set_name_filters(std::vector<std::string> extensions);
  [[nodiscard]] auto name_filters() const noexcept -> const std::vector<std::string>& {
    return name_filters_;
  }

  // —— 隐藏文件显隐 ——
  /// 是否显示 `.` 开头的条目（POSIX 隐藏文件；默认 false——跨平台安全侧）。
  /// Windows 的隐藏属性不在此列（`list_dir` 未暴露该位；且 DOS/系统文件误伤面大）。
  void set_show_hidden(bool show);
  [[nodiscard]] auto show_hidden() const noexcept -> bool { return show_hidden_; }

  // —— 位置侧栏（跨平台的"常用位置"）——
  /// 自定义位置项（追加在内置项之后；`path` 不存在时自动跳过显示）。
  /// 内置项：主目录（`fs::home_dir`，跨平台）+ Windows 盘符（`C:`、`D:`…，
  /// 按 `GetLogicalDrives`/`/proc/mounts` 口径枚举，枚举不到就不显示）。
  struct Place {
    std::string label{};
    std::string path{};
  };
  void set_places(std::vector<Place> places);
  [[nodiscard]] auto places() const -> std::vector<Place>;   // 内置 + 自定义（存在性已过滤）

  // —— 新建文件夹 ——
  /// 在当前目录下新建子目录（名字冲突时自动加 `-1`/`-2` 后缀）。
  /// 成功返回新目录名（并进入它——与用户"建完就用"的意图一致）。
  [[nodiscard]] auto create_folder(std::string name = "新建文件夹") -> std::string;

  /// 文件名（输入行内容；`Mode::Save` 下构造时预填）。
  void set_filename(std::string name);
  [[nodiscard]] auto filename() const noexcept -> const std::string& { return filename_; }

  /// 当前选中条目（未选中返回 nullptr）。
  [[nodiscard]] auto selected_entry() const noexcept -> const st::fs::DirEntry* {
    return selected_ < entries_.size() ? &entries_[selected_] : nullptr;
  }
  /// 可见条目数（过滤/隐藏后；`entry(index)` 同口径）。
  [[nodiscard]] auto entry_count() const noexcept -> std::size_t { return visible_.size(); }
  [[nodiscard]] auto entry(std::size_t index) const noexcept -> const st::fs::DirEntry*;

  /// 错误行文本（`fs` 失败时呈现；空 = 无错误）。
  [[nodiscard]] auto error_text() const noexcept -> const std::string& { return error_; }

  /// **程序化指向一个路径**（无头 / 自动化选路的入口）。
  ///
  /// 为何必须有（2026-10-05，来自实战）：`FileDialog` 是框架自绘组件，选路全靠鼠标
  /// 在列表里点、在文件名行里打字——而 **headless 下没有真实鼠标键盘**，
  /// 智能体根本"选不了文件"：实测「点选择图片无反应」，整条 OCR 流程端到端验不了。
  ///
  /// 语义（一条路径 = 一次完整的"用户选路"意图）：
  /// - 目录 → 进入该目录（`set_directory`），不改文件名；
  /// - 文件 → 进入它所在目录 + 回填文件名（等价于"点进那个目录再点那个文件"）；
  /// - **不存在 → 返回 false 且不改变任何状态**（不静默改成别的目录——自动化最怕的
  ///   就是"调了没报错但去了别处"）。
  ///
  /// 文件已存在且就在当前目录时，**同时选中列表里那一项**，让画面确实高亮它——
  /// 否则"程序化选路"与"用户在界面上看到的"会不一致。
  [[nodiscard]] auto set_pending_path(const std::string& path) -> bool;

  /// 程序化选中第 index 个条目（越界返回 false）。与鼠标点击走**同一个**
  /// `activate_entry`，因此回填文件名等副作用完全一致
  /// （避免"程序化路径是另一套语义"这类分叉）。
  [[nodiscard]] auto select_entry(std::size_t index) -> bool;

  /// 确认（按钮/Enter/双击文件）：文件名空则不触发。参数为「目录/文件名」拼好的全路径。
  std::function<void(const std::string& full_path)> on_confirm{};
  /// 取消（Esc/按钮/遮罩点击）。
  std::function<void()> on_cancel{};

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return title_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  // —— 布局几何（测试与命中共用；arrange 后有效） ——
  [[nodiscard]] auto card_rect() const noexcept -> math::Rect { return card_; }
  /// 对话框标题（空 = 不画标题行，省掉那段竖向空白）。
  void set_title(std::string title) {
    title_ = std::move(title);
    mark_layout_dirty();
    mark_dirty();
  }
  [[nodiscard]] auto title() const noexcept -> const std::string& { return title_; }
  /// 位置侧栏区（卡片内左列；无侧栏时为空矩形）。
  [[nodiscard]] auto places_rect() const noexcept -> math::Rect { return places_; }
  /// 面包屑行区（卡片内，目录行上方）。
  [[nodiscard]] auto breadcrumb_rect() const noexcept -> math::Rect { return breadcrumb_; }
  /// 第 index 个面包屑段命中区（越界为空矩形；末段是当前位置，不可点）。
  [[nodiscard]] auto crumb_rect(std::size_t index) const noexcept -> math::Rect;
  /// 位置侧栏第 index 项命中区（越界为空矩形）。
  [[nodiscard]] auto place_rect(std::size_t index) const noexcept -> math::Rect;
  /// 工具行（隐藏切换/新建文件夹）各钮命中区：`toolbar_toggle_hidden_rect()` /
  /// `toolbar_new_folder_rect()`。
  [[nodiscard]] auto toolbar_toggle_hidden_rect() const noexcept -> math::Rect;
  [[nodiscard]] auto toolbar_new_folder_rect() const noexcept -> math::Rect;
  /// 文件列表区（卡片内）。
  [[nodiscard]] auto list_rect() const noexcept -> math::Rect { return list_; }
  /// 文件名输入行区。
  [[nodiscard]] auto input_rect() const noexcept -> math::Rect { return input_; }
  /// `..`（上级）行矩形——列表首行。
  [[nodiscard]] auto parent_row_rect() const noexcept -> math::Rect;
  /// 第 index 个条目行矩形（含滚动偏移；越界返回空矩形）。
  [[nodiscard]] auto entry_rect(std::size_t index) const noexcept -> math::Rect;

 private:
  /// 重读当前目录（`list_dir` 失败置 `error_` 并清空条目；应用过滤与隐藏开关）。
  void reload();
  /// 条目是否该显示（后缀过滤 + 隐藏开关；目录永不过滤）。
  [[nodiscard]] auto entry_visible(const st::fs::DirEntry& entry) const -> bool;
  /// 面包屑段表（当前目录按分隔符拆；缓存于 reload 后，布局期重建）。
  [[nodiscard]] auto crumbs() const -> std::vector<std::pair<std::string, std::string>>;
  /// 列表内容总高（`..` + 条目）——滚动夹取用。
  [[nodiscard]] auto list_content_height() const -> float;
  /// 选中行滚入可视区（键盘导航后调）。
  void ensure_selected_visible();
  /// 进入条目（目录）或选中（文件）。
  void activate_entry(std::size_t index);
  void move_selection(int delta);
  /// 确认语义：拼全路径触发 `on_confirm`（文件名空则不触发）。
  void confirm();
  /// 输入行文本插入/删除（自绘单行编辑）。
  void input_insert(std::string_view text);
  void input_backspace();

  Mode mode_{Mode::Open};
  std::string title_{};
  std::string directory_{};
  std::vector<st::fs::DirEntry> entries_{};   ///< reload 后的全量条目
  std::vector<std::size_t> visible_{};        ///< 可见索引（过滤/隐藏后）→ entries_ 下标
  std::size_t selected_{static_cast<std::size_t>(-1)};   ///< 可见序号
  std::string filename_{};
  std::string error_{};
  std::vector<std::string> name_filters_{};   ///< 后缀白名单（空 = 全部）
  bool show_hidden_{false};                   ///< `.` 开头条目的显隐
  std::vector<Place> custom_places_{};        ///< 宿主自定义位置项
  float scroll_y_{0.0f};             ///< 列表纵向滚动（自绘行滚轮）
  bool input_focused_{false};        ///< 文件名行聚焦态（自绘光标依据）
  math::Rect card_{};
  math::Rect breadcrumb_{};          ///< 面包屑行
  math::Rect places_{};              ///< 位置侧栏列
  math::Rect list_{};
  math::Rect input_{};
  math::Rect toggle_hidden_button_{};   ///< 工具钮：隐藏文件切换
  math::Rect new_folder_button_{};      ///< 工具钮：新建文件夹
  Theme last_theme_{};                  ///< 上次下发的主题（新子元素补下发用）
  bool theme_valid_{false};             ///< `last_theme_` 是否有效（apply_theme 跑过）
  mutable std::vector<float> crumb_widths_{};   ///< 面包屑段宽缓存（布局期）
  Element* confirm_button_{nullptr};  ///< 非拥有（生命周期随子节点）
  Element* cancel_button_{nullptr};
};

}  // namespace st::ui
