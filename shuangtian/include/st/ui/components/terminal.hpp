/// 终端（Terminal）：多会话命令行面板，框架级通用组件。
///
/// 「终端」在这里是**一屏滚回文本 + 一个常驻输入行 + 若干会话标签**的组合件，
/// 不绑定任何具体宿主应用：
///
/// * **会话（session）**：每个会话一条独立 shell——各自的工作目录、命令历史、
///   滚回缓冲。切换会话不改变任何作业的去向（见 `pump` 的归属说明）。
/// * **作业**：`run()` 起一条命令，跑在**工作线程**上、输出**逐行回流**。
///   因此长命令不会把界面卡住，`` 也能真的把子进程杀掉。
/// * **执行策略归宿主**：组件只负责"起进程 + 收输出 + 呈现"，**跑什么**由宿主定
///   （`before_run` 过滤钩子 / 或不用 `run` 而用 `append` 自己写输出）。
///   这样它既能做真 shell 面板，也能做"任务输出窗"。
///
/// ## 为什么是多会话而不是单会话
///
/// 一个终端面板只挂一条 shell 时，用户换个目录干活就得把当前会话 `cd` 走、
/// 原目录的上下文（滚回里的路径、历史里的命令）全被冲掉。
/// 会话标签是**并行的上下文**，不是"最近命令列表"的另一种画法。
///
/// ## 与宿主的接口（三条）
///
/// 1. **输出**：`append()` 写一行文本（宿主自己的提示/日志）；
/// 2. **事件**：`on_session_change` / `on_session_close` / `on_busy_change`；
/// 3. **每帧**：`pump()` ——**必须被每帧调用**，否则回流不会落地、作业不会收尾。
///    声明式宿主（`DeclarativeHost`）会在 `tick()` 里自动调它；手搭宿主自己调。
///
/// 只有输入行的键盘手势、标签的点击、滚动跟随这类**纯界面行为**留在组件内——
/// 它们跨宿主是同一套；而"哪些命令合法""按下回车要做什么"在宿主。

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "st/core/process.hpp"
#include "st/core/channel.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

class ScrollView;
class Tabs;
class Text;
class Input;

/// 终端会话（一个标签）。
///
/// 公开它而不是塞进 `Terminal` 的私有实现：宿主常要读会话状态来做自己的判断
/// （如"当前会话有活没干完吗"），而只给 getter 会让这类读取变成一串转发函数。
struct TerminalSession {
  /// 标签文字。
  std::string title{};
  /// 滚回：**分块**存（追加是 O(新增)，截断只丢头几块）。
  ///
  /// 为什么不存一个长字符串：① 每次追加都要整串重拷；② 截断要先算出丢了多长；
  /// ③ "又长了"这个量没法比较（滚动贴底判定靠它）。
  std::vector<std::string> chunks{};
  /// 工作目录（空 = 用 `Terminal::working_directory()` 的默认值）。
  ///
  /// **每会话独立**：`cd` 只影响当前这条 shell（与真实终端一致）。
  std::string cwd{};
  /// `cd -` 要回到的上一站。
  std::string prev_dir{};
  /// 命令历史（↑↓ 翻）与游标（`-1` = 不在翻历史，显示草稿）。
  std::vector<std::string> history{};
  std::ptrdiff_t history_cursor{-1};
  /// 用户主动往上翻过（暂停贴底跟随）；回到底部自动恢复。
  bool user_scrolled{false};
  /// 本会话是否有作业在跑（作业**归属**发起它的会话）。
  bool running{false};
  /// 运行中命令的显示名（`"st test"` 之类）。
  std::string running_name{};
};

class Terminal : public Element {
 public:
  /// 单会话滚回上限（**行数**，不是字节数）：终端是"最近发生了什么"的窗口，
  /// 不是日志归档。上限之外丢头——否则长命令跑一会儿就把界面卡在一次巨大重绘上。
  static constexpr std::size_t kMaxLines{4000};

  Terminal();
  ~Terminal() override;
  Terminal(const Terminal&) = delete;
  auto operator=(const Terminal&) -> Terminal& = delete;
  Terminal(Terminal&&) = delete;
  auto operator=(Terminal&&) -> Terminal& = delete;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Terminal"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Code; }

  // —— 会话（标签）——

  /// 会话数。构造时会自动建好第一个（"终端 1"），因此**至少为 1**。
  [[nodiscard]] auto session_count() const noexcept -> std::size_t {
    return static_cast<std::size_t>(sessions_.size());
  }
  /// 新建一个会话（标题省略时按 `终端 N` 自增）；返回它的序号。
  ///
  /// 新会话的**工作目录跟随当前会话**——用户在某个目录里开新 shell，
  /// 期待是在同一个地方接着干。
  auto add_session(std::string title = {}) -> std::size_t;
  /// 关闭第 `index` 个会话。
  ///
  /// 拒绝关闭**有作业在跑**的会话（返回 false）——否则那条命令的输出会悬空，
  /// 用户也无从知道它还在跑。错误经 `on_error` 报出。
  /// 关掉最后一个会话时**不会**留下空面板：组件回到"自动补一个空会话"的状态，
  /// 是否收起整个面板由宿主决定（`on_session_close` 里做）。
  auto close_session(std::size_t index) -> bool;
  void set_active_session(std::size_t index);
  [[nodiscard]] auto active_session() const noexcept -> std::size_t { return active_; }
  [[nodiscard]] auto session(std::size_t index) -> TerminalSession*;
  [[nodiscard]] auto session(std::size_t index) const -> const TerminalSession*;
  void set_session_title(std::size_t index, std::string title);

  // —— 作业 ——

  /// **通道工厂**：决定"命令跑在哪"。缺省 = 本机子进程（开箱即用）。
  ///
  /// 注入它就能换通道（SSH / 容器 / 测试假通道）——组件本身对通道一无所知，
  /// 只按 `ChannelSpec` 描述需求、拿回一个 `Channel` 读写。
  /// 传空 = 回到缺省（本机）。
  void set_channel_factory(st::exec::ChannelFactory factory);
  /// 当前用的通道工厂（未注入时是缺省的那一个）。
  [[nodiscard]] auto channel_factory() const -> const st::exec::ChannelFactory& {
    return factory_;
  }
  /// 下一条命令要用的通道描述（宿主改它来换目标主机/类型；`program`/`cwd` 每次
  /// 执行时会被组件按当前会话补上）。
  [[nodiscard]] auto channel_spec() -> st::exec::ChannelSpec& { return spec_; }
  [[nodiscard]] auto channel_spec() const -> const st::exec::ChannelSpec& { return spec_; }

  /// 设 shell（默认取平台默认：`GEBAI_TERMINAL_SHELL` → `$SHELL`/`cmd.exe`）。
  /// 参数里 `{cmd}` 会被替换为命令行；无 `{cmd}` 则把命令行作为最后一个参数
  /// （`/c <cmd>` 或 `-c <cmd>` 自动补）。
  void set_shell(std::string program, std::vector<std::string> args = {});
  [[nodiscard]] auto shell_program() const noexcept -> const std::string& { return shell_; }

  /// 默认工作目录（会话没设 `cwd` 时用它）。
  void set_working_directory(std::string dir);
  [[nodiscard]] auto working_directory() const -> std::string;

  /// 在当前会话里起一条命令（**异步**：立即返回，输出经 `pump` 逐行回流）。
  void run(std::string command);
  /// 请求中止当前作业（SIGTERM / TerminateProcess）。
  void send_stop();
  /// 丢弃当前会话的滚回。
  void clear();
  /// 当前会话是否忙。
  [[nodiscard]] auto busy() const -> bool;

  /// **命令过滤钩子**：返回**非空字符串** = 拒绝执行，字符串作为拒绝理由显示出来。
  ///
  /// 为什么过滤在宿主：组件不知道"哪些命令该被允许"——那取决于应用的安全模型
  /// （白名单 / 只读模式 / 沙箱角色）。组件只提供这条缝，策略全在宿主。
  std::function<std::string(const std::string& command)> before_run{};

  // —— 输出（宿主自己写文本时用）——

  /// 往当前会话追加一行（自动补换行）。
  void append(std::string line);
  /// 往**指定**会话追加一行。
  void append_to(std::size_t index, std::string line);
  /// 会话全文（拼接所有块；含"运行中"提示行）。
  [[nodiscard]] auto session_text(std::size_t index) const -> std::string;
  [[nodiscard]] auto line_count(std::size_t index) const -> std::size_t;

  // —— 回调 ——

  /// 活动会话变了（点标签 / 新建 / 关闭后自动切换）。
  std::function<void(std::size_t)> on_session_change{};
  /// 请求关闭某会话（**宿主**决定收不收面板）。组件已把会话真的关掉了。
  std::function<void(std::size_t)> on_session_close{};
  /// 作业开始/结束（想联动状态栏的宿主用）。
  std::function<void(bool)> on_busy_change{};
  /// 一条提示（拒绝执行 / 没活可中止 …）。宿主可转状态栏。
  std::function<void(const std::string&)> on_error{};
  /// 用户敲了回车（`run` 之外的入口：宿主自己做命令分发时用这个接）。
  ///
  /// 装了它之后，回车**不再**走内置的 `run` —— 宿主全权接管。
  std::function<void(const std::string&)> on_submit{};

  // —— 每帧 ——

  /// 把工作线程攒下的输出搬到界面状态、收尾已结束的作业。
  ///
  /// **必须每帧调**。为什么不是组件自己在 `paint` 里做：`paint` 可能被视口剔除跳过
  /// （元素滚出可视区就不画），那样"看不见的时候作业永远收不了尾"。
  /// 每帧的时机归宿主（声明式宿主在 `tick()` 里自动调）。
  void pump();

  // —— 外观 ——

  /// 输出区字体是否用等宽（默认 true）。等宽是终端的**可读性前提**——
  /// 比例字体下表格化输出（`ls -l`、构建日志的列对齐）会散架。
  void set_monospace(bool value);
  [[nodiscard]] auto monospace() const noexcept -> bool { return monospace_; }
  /// 标签栏可见（单会话场景可关；默认 true）。
  void set_tabs_visible(bool value) noexcept { tabs_visible_ = value; }
  [[nodiscard]] auto tabs_visible() const noexcept -> bool { return tabs_visible_; }
  /// 输入行可见（纯输出窗场景可关；默认 true）。
  void set_input_visible(bool value) noexcept { input_visible_ = value; }
  [[nodiscard]] auto input_visible() const noexcept -> bool { return input_visible_; }

  // Element 接口
  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto get_property(std::string_view name) const
      -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  auto invoke_action(std::string_view action, std::string_view argument) -> bool override;
  [[nodiscard]] auto semantics_value() const -> std::string override;

  // —— 几何（测试与命中共用；arrange 后有效）——
  [[nodiscard]] auto tabs_rect() const noexcept -> math::Rect { return tabs_rect_; }
  [[nodiscard]] auto output_rect() const noexcept -> math::Rect { return output_rect_; }
  [[nodiscard]] auto input_rect() const noexcept -> math::Rect { return input_rect_; }

 private:
  /// 会话容器（`vector` 里存值：会话状态都是纯数据，没有需要稳定的地址）。
  std::vector<TerminalSession> sessions_{};
  std::size_t active_{0};
  /// 标题自增序号（`终端 N`）。
  std::size_t title_seq_{1};

  // —— 作业（同一时刻只有一个：`open` 一条管道、一个读线程）——
  // 通道：**不认具体类型**（本机进程只是缺省实现）。见 `set_channel_factory`。
  std::unique_ptr<st::exec::Channel> channel_{};
  st::exec::ChannelFactory factory_{};
  st::exec::ChannelSpec spec_{};
  std::jthread reader_{};
  std::vector<std::string> pending_{};
  std::mutex pending_mutex_{};
  std::atomic<bool> stop_requested_{false};
  bool job_finished_{false};
  int job_code_{0};
  std::string job_name_{};
  /// 作业归属的会话（切会话不改变输出去向）。
  std::size_t job_session_{0};

  // —— 外观 ——
  std::string shell_{};
  std::vector<std::string> shell_args_{};
  std::string cwd_default_{};
  bool monospace_{true};
  bool tabs_visible_{true};
  bool input_visible_{true};

  // —— 子件（组合而非自绘：标签/滚动/输入都是既有组件）——
  Tabs* tabs_{nullptr};
  ScrollView* view_{nullptr};
  Text* output_{nullptr};
  Input* input_{nullptr};

  math::Rect tabs_rect_{};
  math::Rect output_rect_{};
  math::Rect input_rect_{};
  /// 待画进 `Text` 的文本（在 `pump`/`append` 里置脏，`paint` 时同步）。
  bool output_dirty_{true};

  /// 取当前会话（容器为空时先补一个——不变式：**永远至少一个会话**）。
  auto current() -> TerminalSession&;
  [[nodiscard]] auto current() const -> const TerminalSession&;
  /// 会话的生效工作目录（自己的 `cwd` 优先，否则默认目录）。
  [[nodiscard]] auto effective_cwd(std::size_t index) const -> std::string;
  /// 内建命令（`cd` / `clear` / `help` / `history`）——返回 true 表示已处理。
  auto run_builtin(const std::string& command) -> bool;
  void start_job(const std::string& program, std::vector<std::string> args,
                 const std::string& display, const std::string& cwd,
                 const std::string& command);
  /// 生效的工厂（未注入时用缺省；**惰性初始化**，避免每个组件实例都建一份）。
  [[nodiscard]] auto default_factory() const -> const st::exec::ChannelFactory&;
  /// 把输入行的内容作为一条命令提交（走 `on_submit` 或 `run`）。
  void submit_input();
  /// ↑↓ 翻历史。
  void history_step(int delta);
  /// Tab 补全（内建命令 + 路径）。
  void complete();
};

}  // namespace st::ui
