#pragma once

/// 命令面板组件：VSCode Ctrl+Shift+P 形态——顶部居中卡片 + 输入过滤 + 命令列表，
/// 键盘 ↑↓ 导航、Enter 执行、Esc/点击遮罩关闭。
///
/// 结构：`FillViewport` 浮层（遮罩铺满视口）+ 自绘卡片定位（顶部居中，`y = kTopOffset`）。
/// 卡片内部用 `Input`（过滤输入）与 `List`（结果）子组件——与 Dialog「按钮用 Button」
/// 同一做法：可交互部分复用内置组件，容器与遮罩自绘。
///
/// 数据驱动：`set_commands` 传入命令表（id/title/detail/handler），过滤在组件内做
/// （title/detail 不区分大小写子串匹配）；`set_query` 以编程方式设置过滤词
/// （控制通道 `set query` 同路）。
///
/// 属性面：`query`（读写）、`command_count`（全部命令数）、`match_count`（当前过滤命中数）、
/// `active`（高亮序号）。动作面：`activate`（执行当前高亮）、`select`（argument=序号或命令 id）。
///
/// 由 codeeditor 示例的示例级实现升为框架组件（DESIGN §8.1.1 反推的框架缺口）。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class CommandPalette : public Element {
 public:
  /// 一条命令：`id` 业务身份、`title` 主文案、`detail` 次文案（也参与过滤）。
  struct Command {
    std::string id{};
    std::string title{};
    std::string detail{};
    std::function<void()> handler{};
  };

  static constexpr float kCardWidth{520.0f};
  static constexpr float kListMaxHeight{300.0f};
  static constexpr float kTopOffset{48.0f};

  explicit CommandPalette(std::vector<Command> commands = {});

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "CommandPalette"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Dialog; }
  /// 模态浮层：不可见时不拦截输入（与 `Dialog` 同契约）。
  [[nodiscard]] auto intercepts_input() const noexcept -> bool override { return visible(); }

  // —— 数据 ——

  /// 整表替换命令（保持过滤词重新过滤）。
  void set_commands(std::vector<Command> commands);
  [[nodiscard]] auto command_count() const noexcept -> std::size_t { return commands_.size(); }
  /// 过滤词（空 = 全部）。变化即重过滤并把高亮归零。
  void set_query(std::string query);
  [[nodiscard]] auto query() const noexcept -> const std::string& { return query_; }
  /// 当前过滤命中数。
  [[nodiscard]] auto match_count() const noexcept -> std::size_t { return matched_.size(); }
  /// 当前高亮命令 id（无命中为空）。
  [[nodiscard]] auto active_id() const -> std::string;

  // —— 键盘导航 ——

  /// 高亮移动（环绕）；返回新高亮序号（无命中 kNoSelection）。
  auto move_highlight(int delta) -> std::size_t;
  /// 执行当前高亮命令（回调在组件外派发；触发 `on_command`）。返回是否执行了。
  auto activate_highlighted() -> bool;
  /// 把键盘焦点交给过滤输入框（**显示面板后必须调一次**）。
  ///
  /// 不调的话：根上的全局快捷键（Ctrl+Shift+P / Ctrl+P）负责把面板显示出来，
  /// 而焦点仍在底层编辑器上——**敲进去的字会跑到编辑器里**，面板看着开着却打不了字。
  /// 焦点属于宿主决定的事（框架不替宿主抢焦点），所以这里只提供入口。
  void grab_focus();

  // —— 几何（arrange 后有效；测试/命中用）——

  [[nodiscard]] auto card_rect() const noexcept -> math::Rect { return card_rect_; }

  /// 关闭回调（Esc / 遮罩点击触发；是否移除浮层由调用方决定）。
  std::function<void()> on_close{};
  /// 命令执行回调（参数为命令 id——比直接调 handler 多一个外部观测点）。
  std::function<void(std::string_view id)> on_command{};

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  auto invoke_action(std::string_view action, std::string_view argument) -> bool override;

 private:
  void rebuild();
  void select(std::size_t index);
  /// 执行第 `commands_` 下标处的命令：**handler 与 `on_command` 的唯一出口**。
  /// Enter（`activate_highlighted`）与鼠标点击（列表项回调）共用它——
  /// 两条路径各拼一份执行链，必然有一边漏掉一环。
  auto run_command(std::size_t command_index) -> bool;

  std::vector<Command> commands_{};
  std::vector<std::size_t> matched_{};  // 命中命令在 commands_ 的下标（按表序）
  std::string query_{};
  std::size_t highlight_{kNoSelection};
  Input* input_{nullptr};   // 非拥有（子元素）
  List* list_{nullptr};     // 非拥有（子元素）
  math::Rect card_rect_{};
  math::Color mask_color_{};
  math::Color card_bg_{};
  math::Color card_border_{};
};

}  // namespace st::ui
