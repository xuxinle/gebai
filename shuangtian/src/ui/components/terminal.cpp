/// 终端组件实现（见 `terminal.hpp` 的设计说明）。
///
/// 版式对齐「真终端」的通行形态（参考歌白文件工作台的终端面板）：
///
/// ```
/// [标签][标签][+]                        ← tabs_rect_（会话标签；忙碌会话带 ●）
/// ┌──────────────────────────────────┐
/// │ 输出（等宽、贴底跟随）                │  ← output_rect_（ScrollView）
/// └──────────────────────────────────┘
/// cwd ❯ [输入]                           ← input_rect_（提示行）
/// ```
///
/// 三个子件都是**既有组件**（Tabs / ScrollView / Text / Input），本组件只做
/// "把它们按终端的语义摆好 + 驱动作业"。自绘只有分隔线与提示符两处。

#include "st/ui/components/terminal.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <utility>

#include "st/core/channel.hpp"
#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/text_port.hpp"
#include "components_internal.hpp"

namespace st::ui {

namespace {

/// 平台默认 shell（可被 `GEBAI_TERMINAL_SHELL` 覆盖）。
///
/// 为什么允许环境变量：`/bin/sh` 与 `/bin/bash` 的差别对用户是实质的
/// （补全、别名、`$SHELL` 语义），不该由框架猜死。
[[nodiscard]] auto default_shell() -> std::string {
  // std::getenv 是唯一可移植的读法（core 里没有 env 封装，而这里只读两个键，
  // 不值得为此加一层）；结果立即拷进 std::string，不持有返回的指针。
  if (const char* configured = std::getenv("GEBAI_TERMINAL_SHELL");
      configured != nullptr && configured[0] != '\0') {
    return configured;
  }
#ifdef _WIN32
  return "cmd.exe";
#else
  if (const char* shell = std::getenv("SHELL"); shell != nullptr && shell[0] != '\0') {
    return shell;
  }
  return "/bin/sh";
#endif
}

/// 尾段名（`/a/b/c` → `c`；用于目录显示与标签标题）。
[[nodiscard]] auto tail_name(const std::string& path) -> std::string {
  const std::size_t slash = path.find_last_of("/\\");
  const std::string tail = slash == std::string::npos ? path : path.substr(slash + 1);
  return tail.empty() ? path : tail;
}

/// 行内空白切分（命令补全用；不经 shell，因此不做引号解析）。
[[nodiscard]] auto words_of(const std::string& text) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::string current;
  for (const char c : text) {
    if (c == ' ' || c == '\t') {
      if (!current.empty()) out.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) out.push_back(std::move(current));
  return out;
}

/// 内建命令名（补全候选）。
constexpr std::string_view kBuiltins[] = {"cd", "clear", "help", "history"};

}  // namespace

Terminal::Terminal() {
  shell_ = default_shell();
  set_focusable(true);
  // 不变式：**永远至少一个会话**。放在构造里而不是"用的时候补"，
  // 是因为 `session_count() == 0` 这种状态没有任何合法语义。
  sessions_.push_back(TerminalSession{.title = std::format("终端 {}", title_seq_++)});
}

Terminal::~Terminal() {
  // 析构顺序敏感：先让读线程停下并 join，再放掉管道。
  // 否则读线程可能正在 `read_line` 里用即将析构的句柄（悬垂）。
  if (reader_.joinable()) {
    stop_requested_.store(true);
    if (channel_ != nullptr && channel_->valid()) channel_->terminate();
    reader_.join();
  }
}

// ════════════════════════════════════════════════════════════════════════════
// 会话
// ════════════════════════════════════════════════════════════════════════════

auto Terminal::session(std::size_t index) -> TerminalSession* {
  return index < sessions_.size() ? &sessions_[index] : nullptr;
}

auto Terminal::session(std::size_t index) const -> const TerminalSession* {
  return index < sessions_.size() ? &sessions_[index] : nullptr;
}

auto Terminal::current() -> TerminalSession& {
  if (sessions_.empty()) {
    sessions_.push_back(TerminalSession{.title = std::format("终端 {}", title_seq_++)});
  }
  if (active_ >= sessions_.size()) active_ = sessions_.size() - 1;
  return sessions_[active_];
}

auto Terminal::current() const -> const TerminalSession& {
  // 常量版本不做"补齐"（那是修改）；越界时返回一个静态空会话，
  // 调用方拿到的是一份可读的空状态，而不是 UB。
  static const TerminalSession kEmpty{};
  if (sessions_.empty() || active_ >= sessions_.size()) return kEmpty;
  return sessions_[active_];
}

auto Terminal::add_session(std::string title) -> std::size_t {
  // 新会话的工作目录**跟随当前会话**：用户在某个目录里开新 shell，
  // 期待是在同一个地方接着干（不是跳回默认目录）。
  const std::string inherited = effective_cwd(active_);
  TerminalSession fresh{};
  fresh.title = title.empty() ? std::format("终端 {}", title_seq_++) : std::move(title);
  fresh.cwd = sessions_.empty() ? std::string{} : current().cwd;
  (void)inherited;
  sessions_.push_back(std::move(fresh));
  const std::size_t index = sessions_.size() - 1;
  active_ = index;
  if (tabs_ != nullptr) mark_layout_dirty();
  mark_dirty();
  if (on_session_change) on_session_change(index);
  return index;
}

auto Terminal::close_session(std::size_t index) -> bool {
  if (index >= sessions_.size()) return false;
  if (sessions_[index].running) {
    // 有作业在跑就拒绝：关掉它会让那条命令的输出悬空，用户也不知它还在跑。
    if (on_error) on_error("该会话有命令在运行，先中止再关闭");
    return false;
  }
  sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(index));
  if (sessions_.empty()) {
    // 不留空面板：补一个全新会话。**是否收起整个面板由宿主决定**
    //（`on_session_close` 里做）——组件不该替宿主决定布局。
    sessions_.push_back(TerminalSession{.title = std::format("终端 {}", title_seq_++)});
    active_ = 0;
  } else if (active_ >= sessions_.size()) {
    active_ = sessions_.size() - 1;
  } else if (active_ > index) {
    --active_;
  }
  output_dirty_ = true;
  mark_dirty();
  mark_layout_dirty();
  if (on_session_close) on_session_close(index);
  if (on_session_change) on_session_change(active_);
  return true;
}

void Terminal::set_active_session(std::size_t index) {
  if (index >= sessions_.size() || index == active_) return;
  active_ = index;
  output_dirty_ = true;   // 切会话 = 换一屏文本
  mark_dirty();
  if (on_session_change) on_session_change(index);
}

void Terminal::set_session_title(std::size_t index, std::string title) {
  if (index >= sessions_.size()) return;
  sessions_[index].title = std::move(title);
  mark_dirty();
}

auto Terminal::effective_cwd(std::size_t index) const -> std::string {
  const TerminalSession* s = session(index);
  if (s != nullptr && !s->cwd.empty()) return s->cwd;
  return cwd_default_;
}

// ════════════════════════════════════════════════════════════════════════════
// 配置
// ════════════════════════════════════════════════════════════════════════════

auto Terminal::default_factory() const -> const st::exec::ChannelFactory& {
  // 函数内静态：只在第一次用到时建，且全进程一份（工厂本身无状态）。
  static const st::exec::ChannelFactory kDefault = st::exec::default_channel_factory();
  return kDefault;
}

void Terminal::set_channel_factory(st::exec::ChannelFactory factory) {
  factory_ = std::move(factory);
}

void Terminal::set_shell(std::string program, std::vector<std::string> args) {
  if (!program.empty()) shell_ = std::move(program);
  shell_args_ = std::move(args);
  // shell 是**本地通道**的概念；同步进 spec 让缺省通道直接用上。
  spec_.program = shell_;
  spec_.args = shell_args_;
}

void Terminal::set_working_directory(std::string dir) {
  cwd_default_ = std::move(dir);
  mark_dirty();
}

auto Terminal::working_directory() const -> std::string {
  return cwd_default_;
}

void Terminal::set_monospace(bool value) {
  if (monospace_ == value) return;
  monospace_ = value;
  output_dirty_ = true;
  mark_dirty();
  mark_layout_dirty();
}

// ════════════════════════════════════════════════════════════════════════════
// 输出
// ════════════════════════════════════════════════════════════════════════════

void Terminal::append_to(std::size_t index, std::string line) {
  if (index >= sessions_.size()) return;
  if (!line.empty() && line.back() != '\n') line.push_back('\n');
  auto& chunks = sessions_[index].chunks;
  chunks.push_back(std::move(line));
  // 行数上限：按**行**截断（一个块可能有多行），丢头而不是拒绝追加。
  std::size_t lines = 0;
  for (const auto& chunk : chunks) lines += static_cast<std::size_t>(std::count(
                                          chunk.begin(), chunk.end(), '\n'));
  while (lines > kMaxLines && !chunks.empty()) {
    lines -= static_cast<std::size_t>(std::count(chunks.front().begin(), chunks.front().end(),
                                                '\n'));
    chunks.erase(chunks.begin());
  }
  if (index == active_) output_dirty_ = true;
  mark_dirty();
}

void Terminal::append(std::string line) { append_to(active_, std::move(line)); }

auto Terminal::session_text(std::size_t index) const -> std::string {
  std::string out;
  const TerminalSession* s = session(index);
  if (s == nullptr) return out;
  for (const auto& chunk : s->chunks) out += chunk;
  if (s->running) {
    out += std::format("· {} 运行中…（Ctrl+C 中止）\n", s->running_name);
  }
  return out;
}

auto Terminal::line_count(std::size_t index) const -> std::size_t {
  std::size_t lines = 0;
  const TerminalSession* s = session(index);
  if (s == nullptr) return lines;
  for (const auto& chunk : s->chunks) {
    lines += static_cast<std::size_t>(std::count(chunk.begin(), chunk.end(), '\n'));
  }
  return lines;
}

void Terminal::clear() {
  current().chunks.clear();
  current().user_scrolled = false;
  output_dirty_ = true;
  mark_dirty();
}

auto Terminal::busy() const -> bool { return current().running; }

// ════════════════════════════════════════════════════════════════════════════
// 作业
// ════════════════════════════════════════════════════════════════════════════

void Terminal::start_job(const std::string& program, std::vector<std::string> args,
                         const std::string& display, const std::string& cwd,
                         const std::string& command) {
  // 归属：作业记在**发起它的会话**上。切标签不改变输出去向——
  // 否则"切走再切回来，输出跑到别人身上"是必然的。
  job_session_ = active_;
  sessions_[job_session_].running = true;
  sessions_[job_session_].running_name = display;
  stop_requested_.store(false);
  // **构造通道**：描述"要跑什么"（program/args/cwd/command），由工厂决定跑在哪。
  spec_.program = program;
  spec_.args = args;
  spec_.cwd = cwd;
  spec_.command = command;
  const st::exec::ChannelFactory& factory = factory_ ? factory_ : default_factory();
  channel_ = factory(spec_);
  if (channel_ == nullptr) {
    sessions_[job_session_].running = false;
    sessions_[job_session_].running_name.clear();
    const std::string reason =
        std::format("没有可用的通道（kind=\"{}\"）——检查 set_channel_factory", spec_.kind);
    append_to(job_session_, "[启动失败] " + reason);
    if (on_error) on_error(reason);
    return;
  }
  channel_->open(spec_);
  if (!channel_->valid()) {
    sessions_[job_session_].running = false;
    sessions_[job_session_].running_name.clear();
    append_to(job_session_, std::format("[启动失败] {}：{}", display, channel_->error()));
    if (on_error) on_error(std::format("启动失败：{}", channel_->error()));
    channel_.reset();
    return;
  }
  if (on_busy_change) on_busy_change(true);
  mark_dirty();
  // 读线程用 `std::jthread`：析构时 join，不会像 `detach` 那样把线程露在对象生命周期外
  //（`this` 捕进去的线程若活过对象就是 UB）。
  reader_ = std::jthread([this, display](std::stop_token stop) {
    std::string line;
    while (channel_ != nullptr && channel_->read_line(line)) {
      if (stop.stop_requested() || stop_requested_.load()) break;
      const std::scoped_lock guard(pending_mutex_);
      pending_.push_back(std::move(line));
    }
    const int code = channel_ != nullptr ? channel_->finish() : -1;
    const std::scoped_lock guard(pending_mutex_);
    job_finished_ = true;
    job_code_ = code;
    job_name_ = display;
  });
}

void Terminal::run(std::string command) {
  const std::string trimmed = std::string(st::trim(command));
  if (input_ != nullptr) input_->set_text("");
  current().history_cursor = -1;
  if (trimmed.empty()) return;
  // 记历史（连续重复只留一条）。
  auto& history = current().history;
  if (history.empty() || history.back() != trimmed) history.push_back(trimmed);

  append_to(active_, "❯ " + trimmed);
  if (run_builtin(trimmed)) return;

  // **策略归宿主**：过滤钩子在组件外，因为"哪些命令该被允许"取决于应用的安全模型。
  if (before_run) {
    if (std::string reason = before_run(trimmed); !reason.empty()) {
      append_to(active_, "[拒绝] " + reason);
      return;
    }
  }
  if (current().running) {
    append_to(active_, std::format("[跳过] “{}”还在跑（Ctrl+C 可中止）",
                                   current().running_name));
    return;
  }
  start_job(shell_, shell_args_, trimmed, effective_cwd(active_), trimmed);
}

auto Terminal::run_builtin(const std::string& command) -> bool {
  const auto words = words_of(command);
  if (words.empty()) return true;
  const std::string& head = words.front();

  if (head == "clear") {
    current().chunks.clear();
    current().user_scrolled = false;
    output_dirty_ = true;
    mark_dirty();
    return true;
  }
  if (head == "help") {
    append_to(active_, "内建：cd [目录|~|-] · clear · history · help");
    append_to(active_, "其余命令交给 shell 执行（策略由宿主决定）。");
    return true;
  }
  if (head == "history") {
    const auto& history = current().history;
    if (history.empty()) {
      append_to(active_, "（没有历史）");
      return true;
    }
    for (std::size_t index = 0; index < history.size(); ++index) {
      append_to(active_, std::format("{:3}  {}", index + 1, history[index]));
    }
    return true;
  }
  if (head == "cd") {
    std::string target = words.size() > 1 ? words[1] : std::string{};
    std::string next;
    if (target.empty() || target == "~") {
      next = cwd_default_.empty() ? st::fs::current_dir().value_or(std::string{}) : cwd_default_;
    } else if (target == "-") {
      next = current().prev_dir;
    } else if (st::fs::is_absolute(target)) {
      next = st::fs::normalize(target);
    } else {
      next = st::fs::normalize(st::fs::join(effective_cwd(active_), target));
    }
    if (next.empty() || !st::fs::is_directory(next)) {
      append_to(active_, "目录不存在：" + (next.empty() ? target : next));
      return true;
    }
    const std::string previous = effective_cwd(active_);
    current().prev_dir = previous;
    current().cwd = st::fs::absolute(next).value_or(next);
    append_to(active_, current().cwd);   // 与真 shell 一样回显新目录
    mark_dirty();
    return true;
  }
  return false;
}

void Terminal::send_stop() {
  if (!current().running) {
    if (on_error) on_error("没有正在运行的命令");
    return;
  }
  stop_requested_.store(true);
  // 通道的契约：`terminate` 之后 `read_line` 要尽快返回——否则中止在界面上是假的。
  if (channel_ != nullptr) channel_->terminate();
  append_to(job_session_, std::format("[正在中止] {}", sessions_[job_session_].running_name));
}

void Terminal::pump() {
  const std::size_t job = job_session_;
  if (job >= sessions_.size() || !sessions_[job].running) {
    // 没有在跑的作业：仍要清掉可能残留的缓冲（最后一次收尾）。
    if (job < sessions_.size() && !sessions_[job].running) {
      const std::scoped_lock guard(pending_mutex_);
      pending_.clear();
    }
    return;
  }
  std::vector<std::string> ready;
  bool finished = false;
  int code = 0;
  std::string name;
  {
    const std::scoped_lock guard(pending_mutex_);
    ready.swap(pending_);
    finished = job_finished_;
    code = job_code_;
    name = job_name_;
  }
  // 收尾时先让读线程自己结束（join），再去动它读的那个句柄——
  // 否则读线程可能正在 `read_line` 里用已关闭的管道。
  if (finished && reader_.joinable()) reader_.join();

  for (auto& line : ready) append_to(job, std::move(line));
  if (!finished) return;

  if (stop_requested_.load()) {
    append_to(job, std::format("[已中止] {}", name));
  } else {
    append_to(job, std::format("[退出码 {}] {}", code, name));
  }
  sessions_[job].running = false;
  sessions_[job].running_name.clear();
  stop_requested_.store(false);
  job_finished_ = false;
  job_name_.clear();
  channel_.reset();
  if (on_busy_change) on_busy_change(false);
  mark_dirty();
}

// ════════════════════════════════════════════════════════════════════════════
// 输入行手势
// ════════════════════════════════════════════════════════════════════════════

void Terminal::history_step(int delta) {
  if (input_ == nullptr) return;
  auto& s = current();
  if (s.history.empty()) return;
  const std::ptrdiff_t count = static_cast<std::ptrdiff_t>(s.history.size());
  // 游标语义：`count` = "不在翻历史"（显示草稿/空白），`0` = 最早一条。
  // 容易写反——把 ↑ 传成 +1 会让首次按键算出 `count` 并被归一到空串，
  // 表现就是"按了没反应"（实测踩到过）。
  if (s.history_cursor < 0) s.history_cursor = count;
  s.history_cursor = std::clamp<std::ptrdiff_t>(s.history_cursor + delta, 0, count);
  input_->set_text(s.history_cursor >= count
                       ? std::string{}
                       : s.history[static_cast<std::size_t>(s.history_cursor)]);
}

void Terminal::complete() {
  if (input_ == nullptr) return;
  const std::string text = input_->value();
  if (text.empty()) return;
  const std::string prefix = text.substr(0, text.find_last_of(" \t") + 1);
  const std::string word = text.substr(prefix.size());

  std::vector<std::string> candidates;
  // 第一个词按内建命令补；否则（或没命中）落路径补全。
  if (prefix.empty()) {
    for (const auto name : kBuiltins) {
      if (name.starts_with(word)) candidates.emplace_back(name);
    }
  }
  if (candidates.empty()) {
    std::string dir_part;
    std::string file_part = word;
    if (const std::size_t slash = word.find_last_of("/\\"); slash != std::string::npos) {
      dir_part = word.substr(0, slash + 1);
      file_part = word.substr(slash + 1);
    }
    const std::string base =
        dir_part.empty() ? effective_cwd(active_) : st::fs::normalize(
                                                         st::fs::join(effective_cwd(active_),
                                                                      dir_part));
    if (auto listing = st::fs::list_dir(base); listing.has_value()) {
      for (const auto& item : *listing) {
        if (!item.name.starts_with(file_part)) continue;
        candidates.push_back(dir_part + item.name + (item.is_dir ? "/" : ""));
      }
    }
  }
  if (candidates.empty()) {
    if (on_error) on_error("没有可补全的候选");
    return;
  }
  if (candidates.size() == 1) {
    input_->set_text(prefix + candidates.front() + (candidates.front().ends_with('/') ? "" : " "));
    return;
  }
  // 多个候选：补到**公共前缀**（真 shell 的手感），并把候选列出来。
  std::string common = candidates.front();
  for (const auto& candidate : candidates) {
    std::size_t length = 0;
    while (length < common.size() && length < candidate.size() &&
           common[length] == candidate[length]) {
      ++length;
    }
    common.resize(length);
  }
  input_->set_text(prefix + common);
  append_to(active_, "❯ " + input_->value());
  for (const auto& candidate : candidates) append_to(active_, "  " + candidate);
}

void Terminal::submit_input() {
  if (input_ == nullptr) return;
  const std::string command = input_->value();
  if (on_submit) {
    on_submit(command);
    input_->set_text("");
    return;
  }
  run(command);
}

// ════════════════════════════════════════════════════════════════════════════
// Element 接口
// ════════════════════════════════════════════════════════════════════════════

void Terminal::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.color = theme.colors().text;
  style_.font_size = metrics.font_base;
  // 子件在这里同步主题（组合件的手写方式：父的 `apply_theme` 是子件刷新的时机之一，
  // 另一个是子件自己挂上树时由框架调用）。
  if (tabs_ != nullptr) tabs_->apply_theme(theme);
  if (view_ != nullptr) view_->apply_theme(theme);
  if (output_ != nullptr) output_->apply_theme(theme);
  if (input_ != nullptr) input_->apply_theme(theme);
}

void Terminal::measure(const RenderContext& context, const Constraints& constraints) {
  const Metrics& metrics = context.theme.metrics();
  const float width = constraints.max_width < kUnbounded ? constraints.max_width : 480.0f;
  const float height = constraints.max_height < kUnbounded ? constraints.max_height : 240.0f;
  // 三个带的高度：标签 26（可关）+ 输入行 30 + 输出吃剩余。
  const float tabs = tabs_visible_ ? metrics.control_height - 6.0f : 0.0f;
  const float input = input_visible_ ? 30.0f : 0.0f;
  measured_ = math::Size{width, height};
  (void)tabs;
  (void)input;
}

void Terminal::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  const Metrics& metrics = context.theme.metrics();
  const float tabs_h = tabs_visible_ ? metrics.control_height - 6.0f : 0.0f;
  const float input_h = input_visible_ ? 30.0f : 0.0f;

  float y = rect.y;
  tabs_rect_ = math::Rect{rect.x, y, rect.width, tabs_h};
  y += tabs_h;
  const float output_h = std::max(0.0f, rect.height - tabs_h - input_h);
  output_rect_ = math::Rect{rect.x, y, rect.width, output_h};
  y += output_h;
  input_rect_ = math::Rect{rect.x, y, rect.width, input_h};

  if (tabs_ != nullptr && !tabs_rect_.is_empty()) {
    tabs_->measure(context, Constraints{});
    tabs_->arrange(context, tabs_rect_);
  }
  if (view_ != nullptr && !output_rect_.is_empty()) {
    view_->measure(context, Constraints{});
    view_->arrange(context, output_rect_);
  }
  if (input_ != nullptr && !input_rect_.is_empty()) {
    input_->measure(context, Constraints{});
    // 输入行左侧让出提示符宽度（`cwd ❯ ` 大约 40% 或固定 160）。
    const float prompt_w = std::min(160.0f, input_rect_.width * 0.4f);
    input_->arrange(context,
                    math::Rect{input_rect_.x + prompt_w, input_rect_.y,
                               std::max(0.0f, input_rect_.width - prompt_w), input_rect_.height});
  }
  layout_dirty_ = false;
}

void Terminal::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const auto& colors = context.theme.colors();
  const auto& metrics = context.theme.metrics();
  // 输出区整块深底：终端是"一屏文本"，底色的作用是把它与普通面板分开
  //（参考实现用 `--bg-inset`，这里用同族的 `surface_sunken`）。
  canvas.fill_rect(output_rect_, raster::Paint::solid(colors.surface_sunken));
  // 标签栏与输入行之间的两条分隔线（终端面板的三段结构靠它们读出来）。
  canvas.fill_rect(math::Rect{tabs_rect_.x, tabs_rect_.bottom() - 1.0f, tabs_rect_.width, 1.0f},
                   raster::Paint::solid(colors.border_subtle));
  if (input_visible_) {
    canvas.fill_rect(math::Rect{input_rect_.x, input_rect_.y, input_rect_.width, 1.0f},
                     raster::Paint::solid(colors.border));
  }
  // 提示符（`cwd ❯`）画在输入行左端——自绘两条文本比塞两个子 Text 便宜，
  // 且它不该参与命中（不是可点元素）。
  if (input_visible_ && input_rect_.width > 80.0f) {
    const TextPort& port = components_internal::text_port_of(context);
    const auto& session = current();
    const std::string cwd = tail_name(effective_cwd(active_));
    port.draw(canvas, cwd, math::Point{input_rect_.x + 8.0f, input_rect_.y},
              metrics.font_sm, colors.text_muted);
    const float cwd_w = port.measure_width(cwd, metrics.font_sm);
    port.draw(canvas, "❯", math::Point{input_rect_.x + 16.0f + cwd_w, input_rect_.y},
              metrics.font_sm, colors.primary);
    (void)session;
  }
}

auto Terminal::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (event.kind == EventKind::KeyDown) {
    // 会话手势：Ctrl+Shift+T 新建、Ctrl+Shift+W 关闭当前。
    // 走组件自己的 on_event（而非输入框的 handler）：即使焦点不在输入行（用户刚点了标签），
    // 会话管理键也该生效。
    if (event.ctrl && event.shift) {
      if (event.key == "t" || event.key == "T") {
        add_session();
        event.handled = true;
        return true;
      }
      if (event.key == "w" || event.key == "W") {
        event.handled = true;
        return close_session(active_);
      }
    }
  }
  return Element::on_event(context, event);
}

auto Terminal::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.insert(names.end(),
               {"sessions", "active_session", "session_title", "working_directory", "busy",
                "output", "monospace", "tabs_visible", "input_visible"});
  return names;
}

auto Terminal::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "sessions") return std::to_string(sessions_.size());
  if (name == "active_session") return std::to_string(active_);
  if (name == "session_title") {
    const TerminalSession* s = session(active_);
    return s != nullptr ? s->title : std::string{};
  }
  if (name == "working_directory") return effective_cwd(active_);
  if (name == "busy") return current().running ? "true" : "false";
  if (name == "output") return session_text(active_);
  if (name == "monospace") return monospace_ ? "true" : "false";
  if (name == "tabs_visible") return tabs_visible_ ? "true" : "false";
  if (name == "input_visible") return input_visible_ ? "true" : "false";
  return Element::get_property(name);
}

auto Terminal::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "working_directory") {
    set_working_directory(std::string(value));
    return true;
  }
  if (name == "monospace") {
    set_monospace(value == "true" || value == "1");
    return true;
  }
  if (name == "tabs_visible") {
    set_tabs_visible(value == "true" || value == "1");
    mark_layout_dirty();
    return true;
  }
  if (name == "input_visible") {
    set_input_visible(value == "true" || value == "1");
    mark_layout_dirty();
    return true;
  }
  return Element::set_property(name, value);
}

auto Terminal::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "run") {
    run(std::string(argument));
    return true;
  }
  if (action == "stop") {
    send_stop();
    return true;
  }
  if (action == "clear") {
    clear();
    return true;
  }
  if (action == "new_session") {
    add_session(std::string(argument));
    return true;
  }
  if (action == "close_session") {
    std::size_t index = active_;
    if (!argument.empty()) {
      index = static_cast<std::size_t>(std::strtoul(std::string(argument).c_str(), nullptr, 10));
    }
    return close_session(index);
  }
  if (action == "select_session") {
    set_active_session(
        static_cast<std::size_t>(std::strtoul(std::string(argument).c_str(), nullptr, 10)));
    return true;
  }
  if (action == "submit") {
    submit_input();
    return true;
  }
  // 历史翻页也进动作面：自动化（控制通道）要能像用户按 ↑↓ 一样驱动它，
  // 否则"翻历史"这条路径在无头下没法验。
  if (action == "history_up") {
    history_step(-1);
    return true;
  }
  if (action == "history_down") {
    history_step(1);
    return true;
  }
  if (action == "complete") {
    complete();
    return true;
  }
  return Element::invoke_action(action, argument);
}

auto Terminal::semantics_value() const -> std::string {
  const TerminalSession* s = session(active_);
  return std::format("{} | {} 行 | {}", s != nullptr ? s->title : std::string{"-"},
                     line_count(active_), current().running ? "运行中" : "空闲");
}

}  // namespace st::ui
