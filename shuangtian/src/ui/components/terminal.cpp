/// 终端组件实现（见 `terminal.hpp` 的设计说明）。
///
/// 结构（真终端）：
///
/// ```
/// [标签][标签][+]                       ← tabs_rect_
/// ┌─────────────────────────────────┐
/// │ 屏幕（逐格渲染 AnsiScreen）        │  ← output_rect_（自绘：逐格画样式 + 画光标）
/// │                                  │
/// └─────────────────────────────────┘
/// ```
///
/// **没有输入框**：键盘事件直接转字节写回 PTY（`send_bytes`），
/// 行内编辑/补全/历史全由 shell 与内核行规程做——那才叫终端。
/// 光标是**屏幕状态**（`AnsiScreen::cursor_*`），渲染时画出来。

#include "st/ui/components/terminal.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <utility>

#include "st/core/channel.hpp"
#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/text/text.hpp"   // `text::FontRole`（终端必须用等宽角色）
#include "st/ui/components/basic.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/text_port.hpp"
#include "components_internal.hpp"

namespace st::ui {

namespace {

/// 平台默认 shell。
///
/// Windows 上默认用 **PowerShell**（`pwsh.exe` 优先，回落 `powershell.exe`），
/// 不用 `cmd.exe`：`cmd` 缺管道对象（`|` 只传文本）、没有 `&&`/`||` 之外的
/// 现代语法、UTF-8 支持靠代码页，开发场景里处处受限。
/// `pwsh`（PowerShell 7+）优先于 Windows PowerShell 5.1——前者的默认编码就是 UTF-8。
[[nodiscard]] auto default_shell() -> std::string {
  if (const char* configured = std::getenv("GEBAI_TERMINAL_SHELL");
      configured != nullptr && configured[0] != '\0') {
    return configured;
  }
#ifdef _WIN32
  // 先试 `pwsh.exe`（PowerShell 7+），再 `powershell.exe`（系统自带 5.1）。
  // `where` 不经 shell，直接用 `st::process::which` 查。
  if (auto found = st::process::which("pwsh"); found.has_value()) return *found;
  if (auto found = st::process::which("powershell"); found.has_value()) return *found;
  return "powershell.exe";   // 都查不到也交给系统解析（PATH 可能在运行期变化）
#else
  if (const char* shell = std::getenv("SHELL"); shell != nullptr && shell[0] != '\0') {
    return shell;
  }
  return "/bin/sh";
#endif
}

/// 尾段名（`/a/b/c` → `c`）。
[[nodiscard]] auto tail_name(const std::string& path) -> std::string {
  const std::size_t slash = path.find_last_of("/\\");
  const std::string tail = slash == std::string::npos ? path : path.substr(slash + 1);
  return tail.empty() ? path : tail;
}

/// 终端索引色 → RGB（xterm 的 256 色表）。
///
/// 前 16 色与 16..231 的 6×6×6 立方、232..255 的灰阶都按 xterm 口径算。
/// 为什么不用主题里的"强调色"映射：终端里 `31` 就是**红**，
/// 与界面的品牌色无关（用户跑 `git diff` 时期待的是传统配色）。
[[nodiscard]] auto index_to_rgb(std::uint8_t index, bool bright) -> math::Color {
  static constexpr std::uint8_t kBase[16][3] = {
      {0, 0, 0},       {205, 49, 49},   {13, 188, 121},  {229, 229, 16},
      {36, 114, 200},  {188, 63, 188},  {17, 168, 205},  {229, 229, 229},
      {102, 102, 102}, {241, 76, 76},   {35, 209, 139},  {245, 245, 67},
      {59, 142, 234},  {214, 112, 214}, {41, 184, 219},  {255, 255, 255}};
  if (index < 16) {
    const int slot = bright && index < 8 ? index + 8 : index;
    return math::Color{kBase[slot][0], kBase[slot][1], kBase[slot][2], 255};
  }
  if (index < 232) {
    const int value = index - 16;
    const int r = value / 36;
    const int g = (value / 6) % 6;
    const int b = value % 6;
    const auto level = [](int step) -> std::uint8_t {
      return static_cast<std::uint8_t>(step == 0 ? 0 : 55 + step * 40);
    };
    return math::Color{level(r), level(g), level(b), 255};
  }
  const auto grey = static_cast<std::uint8_t>(8 + (index - 232) * 10);
  return math::Color{grey, grey, grey, 255};
}

/// 把 UTF-8 码点编回字节。
auto utf8_append(char32_t ch, std::string& out) -> void {
  if (ch < 0x80U) {
    out.push_back(static_cast<char>(ch));
  } else if (ch < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (ch >> 6U)));
    out.push_back(static_cast<char>(0x80U | (ch & 0x3FU)));
  } else if (ch < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (ch >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((ch >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (ch & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (ch >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((ch >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((ch >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (ch & 0x3FU)));
  }
}

/// 解析颜色到 RGB（默认色走调用方给的兜底色）。
[[nodiscard]] auto resolve(const st::text::AnsiColor& color, const math::Color& fallback,
                           bool bright) -> math::Color {
  switch (color.kind) {
    case st::text::AnsiColor::Kind::Default: return fallback;
    case st::text::AnsiColor::Kind::Indexed: return index_to_rgb(color.index, bright);
    case st::text::AnsiColor::Kind::Rgb: return math::Color{color.r, color.g, color.b, 255};
  }
  return fallback;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// 生命周期
// ════════════════════════════════════════════════════════════════════════════

Terminal::Terminal() {
  shell_ = default_shell();
  set_focusable(true);
  // 不变式：**永远至少一个会话**。
  sessions_.push_back(std::make_unique<TerminalSession>(
      TerminalSession{.title = std::format("终端 {}", title_seq_++)}));
}

Terminal::~Terminal() {
  // 析构顺序敏感：先让 PTY 与读线程都停下，再放会话容器。
  // 读线程捕了 `shared_ptr<PtySession>`，会话对象被析构后线程还在跑也不悬垂
  //（这正是用 shared_ptr 的原因），但**仍要 join**——否则进程退出时线程还在读。
  //
  // ⚠ 这里**不调 `wait()`**：它要等子进程退出，而析构可能发生在 UI 线程
  //（宿主重组把本元素拆掉时）；等一下就冻界面。子进程由 `terminate` 杀。
  for (auto& session : sessions_) {
    if (session == nullptr) continue;
    if (session->reader.joinable()) session->reader.request_stop();
    if (session->pty != nullptr && session->pty->valid()) session->pty->terminate();
    if (session->reader.joinable()) session->reader.join();
  }
  if (reader_.joinable()) {
    stop_requested_.store(true);
    if (channel_ != nullptr && channel_->valid()) channel_->terminate();
    reader_.join();
  }
}

auto Terminal::default_factory() const -> const st::exec::ChannelFactory& {
  static const st::exec::ChannelFactory kDefault = st::exec::default_channel_factory();
  return kDefault;
}

// ════════════════════════════════════════════════════════════════════════════
// 会话
// ════════════════════════════════════════════════════════════════════════════

auto Terminal::current() -> TerminalSession& {
  if (sessions_.empty()) {
    sessions_.push_back(std::make_unique<TerminalSession>(
        TerminalSession{.title = std::format("终端 {}", title_seq_++)}));
  }
  if (active_ >= sessions_.size()) active_ = sessions_.size() - 1;
  return *sessions_[active_];
}

auto Terminal::current() const -> const TerminalSession& {
  // 越界时返回**函数内静态**的空白会话，而不是返回引用到可能被写的位置：
  // 这是只读路径，调用方拿不到可写句柄（`terminal_dirty_` 那条 L8 规则
  // 针对的是可变全局状态，这是 const 的例外，已用 `const` 限定意图）。
  if (sessions_.empty() || active_ >= sessions_.size()) {
    static const TerminalSession kEmpty{};
    return kEmpty;
  }
  return *sessions_[active_];
}

auto Terminal::session(std::size_t index) -> TerminalSession* {
  return index < sessions_.size() ? sessions_[index].get() : nullptr;
}

auto Terminal::session(std::size_t index) const -> const TerminalSession* {
  return index < sessions_.size() ? sessions_[index].get() : nullptr;
}

auto Terminal::screen(std::size_t index) const -> const st::text::AnsiScreen* {
  const TerminalSession* found = session(index);
  return found != nullptr ? found->screen.get() : nullptr;
}

auto Terminal::add_session(std::string title) -> std::size_t {
  // 新会话继承当前会话的工作目录与**模式**：用户点“+ 新标签”的意图就是
  // “再开一个终端”——新标签空空如也、还得再点一次才能用，那是没把意图接住。
  const bool spawn_pty = pty_active();
  // 新会话继承当前会话的工作目录（用户在某个目录里开新 shell，
  // 期待是在同一个地方接着干）。
  auto fresh = std::make_unique<TerminalSession>();
  fresh->title = title.empty() ? std::format("终端 {}", title_seq_++) : std::move(title);
  fresh->cwd = sessions_.empty() ? std::string{} : current().cwd;
  sessions_.push_back(std::move(fresh));
  const std::size_t index = sessions_.size() - 1;
  active_ = index;
  mark_layout_dirty();
  mark_dirty();
  if (on_session_change) on_session_change(index);
  if (spawn_pty) open_shell();
  return index;
}

auto Terminal::close_session(std::size_t index) -> bool {
  if (index >= sessions_.size()) return false;
  TerminalSession& target = *sessions_[index];
  // **只有行模式的作业才拦**：那种情况下关会话会把通道的读线程悬在外面。
  // PTY 模式下 `running` 的含义是“shell 活着”——那**不是**关不掉的理因
  //（关标签本来就该结束它的 shell）。旧写法把两者混为一谈，
  // 结果是真终端里每个标签都关不掉（实测踩到）。
  if (target.running && target.pty == nullptr) {
    if (on_error) on_error("该会话有命令在运行，先中止再关闭");
    return false;
  }
  // **延迟到下一帧真做**：本函数可能是从 `Tabs` 的 `on_close` 回调里进来的，
  // 而关闭会让宿主把本元素拆掉（最后一个标签 → 面板收起）——
  // 那就等于“在子组件的回调里强拆自己”，回到调用方时 `this` 已失效。
  // 记下待办，让 `pump`（顶层调用）去执行；调用方只需知道“受理了”。
  pending_close_ = index;
  return true;
}

/// 真正执行关闭（由 `pump` 在安全的时机调；见 `close_session` 的说明）。
void Terminal::apply_pending_close() {
  if (!pending_close_.has_value()) return;
  const std::size_t index = *pending_close_;
  pending_close_.reset();
  if (index >= sessions_.size()) return;
  // **先算清“用户是不是关掉了最后一个”**：erase 之后就判不出了
  //（自动补的那个会让 `session_count()` 恒为 1）。宿主据此决定收不收面板：
  // 两个标签关一个 —— 不该收；关到没标签了 —— 才收。
  const bool was_last = sessions_.size() <= 1;
  TerminalSession& target = *sessions_[index];
  // 读线程要先停干净再放对象：
  // ① `request_stop()` 让循环跳出（即使 `read` 因故没返回）——
  // ② `terminate()` 关伪控制台 ⇒ `read` 返回 0（两个保险都要）。
  // 缺①时读线程只能靠 EOF 退出，而 EOF 依赖平台细节；缺②时子进程留在后台。
  if (target.reader.joinable()) target.reader.request_stop();
  if (target.pty != nullptr && target.pty->valid()) target.pty->terminate();
  if (target.reader.joinable()) target.reader.join();
  // ⚠ **`wait()` 不在这里调**。它要等子进程退出，而这是 **UI 线程**；
  // 对方卡一下就冻界面（实测：关最后一个会话时整个应用无响应）。
  // 子进程已由 `terminate` 杀死 + 系统回收；退出码对“已关闭的会话”无意义。
  // 真正退出码的读取在 `pump` 里（读线程自己调 `wait`）。

  sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(index));
  if (sessions_.empty()) {
    // 不留空面板：补一个全新的。是否收起整个面板由宿主在 `on_session_close` 定
    //（它拿 `last` 参数判断）。**补的这个不自动起 shell**——面板马上要收起，
    // 白起一个进程；用户再打开时 `open_shell` 会补上。
    sessions_.push_back(std::make_unique<TerminalSession>(
        TerminalSession{.title = std::format("终端 {}", title_seq_++)}));
    active_ = 0;
  } else if (active_ >= sessions_.size()) {
    active_ = sessions_.size() - 1;
  } else if (active_ > index) {
    --active_;
  }
  mark_dirty();
  mark_layout_dirty();
  // ⚠⚠ **这一块之后绝对不能再碰 `this`**。
  //
  // 回调可能引发宿主重组，而重组会把**本元素整个拆掉**（关最后一个标签
  // ⇒ 面板收起 ⇒ 容器不再声明 `Terminal`）。实测的两种后果都踩到过：
  // 直接段错误 `0xC0000005`、以及 UI 线程死锁。
  //
  // 因此这里**最多发一个回调**，并且它必须是最后一步：
  // 若不变量（关掉的是最后一个）成立，就只发 `on_close_last_session`
  //（宿主据此收面板，不需要再知道"换成哪个标签"）。
  // 早先写成"先 `on_session_change` 再 `on_close_last_session`"——
  // 前者就会把 `this` 拆掉，后者成了纯 UAF。
  if (was_last && on_close_last_session) {
    on_close_last_session(true);
  } else if (on_session_close) {
    on_session_close(index);
  }
}

void Terminal::set_active_session(std::size_t index) {
  if (index >= sessions_.size() || index == active_) return;
  active_ = index;
  mark_dirty();
  if (on_session_change) on_session_change(index);
}

void Terminal::set_session_title(std::size_t index, std::string title) {
  TerminalSession* found = session(index);
  if (found == nullptr) return;
  found->title = std::move(title);
  mark_dirty();
}

auto Terminal::effective_cwd(std::size_t index) const -> std::string {
  const TerminalSession* found = session(index);
  if (found != nullptr && !found->cwd.empty()) return found->cwd;
  return cwd_default_;
}

// ════════════════════════════════════════════════════════════════════════════
// PTY 模式
// ════════════════════════════════════════════════════════════════════════════

void Terminal::start_pty(const std::shared_ptr<st::process::PtySession>& pty,
                         const std::shared_ptr<st::text::AnsiScreen>& screen,
                         const std::string& display) {
  TerminalSession& target = current();
  target.pty = pty;
  target.screen = screen;
  target.running = true;
  target.running_name = display;
  // 读线程：读字节 → 喂屏幕。**在 `pump` 里喂**（线程只往缓冲塞，
  // 界面状态只能主线程改——与行模式同一套分工）。
  target.reader = std::jthread([this, pty, screen](std::stop_token stop) {
    std::vector<char> buffer(8192);
    for (;;) {
      if (stop.stop_requested()) break;
      const std::size_t got = pty->read(buffer.data(), buffer.size());
      if (got == 0) break;   // 读到 EOF／被 `terminate` 关掉（见 `PtySession::terminate`）
      {
        const std::scoped_lock guard(pending_mutex_);
        // 缓冲按会话分：PTY 与行模式可能同时有活（宿主两种模式混用时）。
        pty_pending_.emplace_back(std::string(buffer.data(), got));
      }
    }
    // ⚠ `wait()` **只在读到 EOF 时**叫：如果是因为 `stop_requested` 跳出，
    // 子进程可能还在（那是“丢掉这个会话”的路径，会话对象已要销毁）——
    // 那时不应再阻塞等进程（子进程由 `terminate` 负责杀）。
    if (!stop.stop_requested()) {
      const int code = pty->wait();
      const std::scoped_lock guard(pending_mutex_);
      pty_exit_code_ = code;
      pty_exited_ = true;
    }
    (void)screen;
  });
}

void Terminal::open_shell(st::process::PtySize size) {
  TerminalSession& target = current();
  if (target.pty != nullptr && target.pty->valid()) return;   // 已有一条
  if (!st::process::PtySession::supported()) {
    if (on_error) on_error("本机不支持伪终端（Windows 需要 10 1809+）");
    return;
  }
  if (size.cols <= 0) size.cols = pty_size_.cols;
  if (size.rows <= 0) size.rows = pty_size_.rows;
  pty_size_ = size;

  auto pty = std::make_shared<st::process::PtySession>();
  const std::vector<std::string> argv = shell_args_;
  pty->open(shell_, argv, effective_cwd(active_), size);
  if (!pty->valid()) {
    const std::string reason = pty->error();
    if (on_error) on_error(std::format("启动终端失败：{}", reason));
    return;
  }
  // 屏幕模型与 PTY 同尺寸（TUI 程序的排版按这个来）。
  auto screen = std::make_shared<st::text::AnsiScreen>(size.cols, size.rows);
  start_pty(pty, screen, tail_name(shell_));
  if (on_busy_change) on_busy_change(true);
  mark_dirty();
  mark_layout_dirty();
}

void Terminal::attach_pty(std::shared_ptr<st::process::PtySession> pty) {
  if (pty == nullptr || !pty->valid()) return;
  auto screen = std::make_shared<st::text::AnsiScreen>(pty_size_.cols, pty_size_.rows);
  start_pty(pty, screen, tail_name(shell_));
  mark_dirty();
}

auto Terminal::pty_active() const -> bool {
  const TerminalSession& target = current();
  return target.pty != nullptr && target.pty->valid();
}

void Terminal::send_bytes(std::string_view bytes) {
  TerminalSession& target = current();
  if (target.pty == nullptr || !target.pty->valid() || bytes.empty()) return;
  // PTY 写入可能部分成功；补齐剩余部分（否则长粘贴会丢尾巴）。
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t written =
        target.pty->write(bytes.data() + offset, bytes.size() - offset);
    if (written == 0) break;
    offset += written;
  }
}

void Terminal::set_terminal_size(st::process::PtySize size) {
  if (size.cols <= 0 || size.rows <= 0) return;
  if (size.cols == pty_size_.cols && size.rows == pty_size_.rows) return;
  pty_size_ = size;
  for (auto& session : sessions_) {
    if (session == nullptr || session->pty == nullptr) continue;
    session->pty->resize(size);
    if (session->screen != nullptr) session->screen->resize(size.cols, size.rows);
  }
  mark_dirty();
}

// ════════════════════════════════════════════════════════════════════════════
// 行模式
// ════════════════════════════════════════════════════════════════════════════

void Terminal::set_channel_factory(st::exec::ChannelFactory factory) {
  factory_ = std::move(factory);
}

void Terminal::set_shell(std::string program, std::vector<std::string> args) {
  if (!program.empty()) shell_ = std::move(program);
  shell_args_ = std::move(args);
  spec_.program = shell_;
  spec_.args = shell_args_;
}

void Terminal::set_working_directory(std::string dir) {
  cwd_default_ = std::move(dir);
  mark_dirty();
}

auto Terminal::working_directory() const -> std::string { return cwd_default_; }

void Terminal::append_to(std::size_t index, std::string line) {
  TerminalSession* target = session(index);
  if (target == nullptr) return;
  if (!line.empty() && line.back() != '\n') line.push_back('\n');
  auto& chunks = target->chunks;
  chunks.push_back(std::move(line));
  std::size_t lines = 0;
  for (const auto& chunk : chunks) {
    lines += static_cast<std::size_t>(std::count(chunk.begin(), chunk.end(), '\n'));
  }
  while (lines > kMaxLines && !chunks.empty()) {
    lines -= static_cast<std::size_t>(
        std::count(chunks.front().begin(), chunks.front().end(), '\n'));
    chunks.erase(chunks.begin());
  }
  mark_dirty();
}

void Terminal::append(std::string line) { append_to(active_, std::move(line)); }

auto Terminal::session_text(std::size_t index) const -> std::string {
  const TerminalSession* target = session(index);
  if (target == nullptr) return {};
  // PTY 模式：屏幕内容（含回看？不含——回看是历史，屏幕是"现在"）。
  if (target->screen != nullptr) return target->screen->plain_text();
  std::string out;
  for (const auto& chunk : target->chunks) out += chunk;
  if (target->running && !target->running_name.empty()) {
    out += std::format("· {} 运行中…\n", target->running_name);
  }
  return out;
}

auto Terminal::line_count(std::size_t index) const -> std::size_t {
  const TerminalSession* target = session(index);
  if (target == nullptr) return 0;
  if (target->screen != nullptr) return static_cast<std::size_t>(target->screen->rows());
  std::size_t lines = 0;
  for (const auto& chunk : target->chunks) {
    lines += static_cast<std::size_t>(std::count(chunk.begin(), chunk.end(), '\n'));
  }
  return lines;
}

void Terminal::clear() {
  TerminalSession& target = current();
  if (target.screen != nullptr) {
    // 用**屏幕的**清屏语义（`\e[2J\e[H`），而不是自己清字符串——
    // 这样回看缓冲、样式、光标都按终端规范被正确处理。
    target.screen->feed("\x1b[2J\x1b[H");
  }
  target.chunks.clear();
  target.user_scrolled = false;
  mark_dirty();
}

auto Terminal::busy() const -> bool { return current().running; }

auto Terminal::any_busy() const -> bool {
  return std::any_of(sessions_.begin(), sessions_.end(),
                     [](const std::unique_ptr<TerminalSession>& session) {
                       return session != nullptr && session->running;
                     });
}

void Terminal::run_program(std::string program, std::vector<std::string> args,
                           std::string display) {
  if (current().running) {
    append_to(active_, std::format("[跳过] “{}”还在跑（先中止）", current().running_name));
    return;
  }
  append_to(active_, "❯ " + display);
  spec_.program = std::move(program);
  spec_.args = std::move(args);
  spec_.cwd = effective_cwd(active_);
  spec_.command.clear();
  const st::exec::ChannelFactory& factory = factory_ ? factory_ : default_factory();
  job_session_ = active_;
  auto channel = factory(spec_);
  if (channel == nullptr) {
    append_to(job_session_,
              std::format("[启动失败] 没有可用的通道（kind=\"{}\"）", spec_.kind));
    return;
  }
  channel->open(spec_);
  if (!channel->valid()) {
    append_to(job_session_, std::format("[启动失败] {}：{}", display, channel->error()));
    return;
  }
  channel_ = std::move(channel);
  sessions_[job_session_]->running = true;
  sessions_[job_session_]->running_name = display;
  stop_requested_.store(false);
  if (on_busy_change) on_busy_change(true);
  reader_ = std::jthread([this](std::stop_token stop) {
    std::string line;
    while (channel_ != nullptr && channel_->read_line(line)) {
      if (stop.stop_requested() || stop_requested_.load()) break;
      const std::scoped_lock guard(pending_mutex_);
      pending_.push_back(line);
    }
    const int code = channel_ != nullptr ? channel_->finish() : -1;
    const std::scoped_lock guard(pending_mutex_);
    job_finished_ = true;
    job_code_ = code;
  });
}

void Terminal::run(std::string command) {
  const std::string trimmed = std::string(st::trim(command));
  if (trimmed.empty()) return;
  // PTY 模式：字符串只是**输入**——原样送给 shell（回车要一起给）。
  if (pty_active()) {
    send_bytes(trimmed + "\r");
    return;
  }
  auto& history = current().history;
  if (history.empty() || history.back() != trimmed) history.push_back(trimmed);
  append_to(active_, "❯ " + trimmed);
  if (before_run) {
    if (std::string reason = before_run(trimmed); !reason.empty()) {
      append_to(active_, "[拒绝] " + reason);
      return;
    }
  }
  if (current().running) {
    append_to(active_, std::format("[跳过] “{}”还在跑（先中止）", current().running_name));
    return;
  }
  spec_.program = shell_;
  spec_.args = shell_args_;
  spec_.cwd = effective_cwd(active_);
  spec_.command = trimmed;
  const st::exec::ChannelFactory& factory = factory_ ? factory_ : default_factory();
  job_session_ = active_;
  auto channel = factory(spec_);
  if (channel == nullptr) {
    append_to(job_session_,
              std::format("[启动失败] 没有可用的通道（kind=\"{}\"）", spec_.kind));
    if (on_error) on_error("没有可用的通道");
    return;
  }
  channel->open(spec_);
  if (!channel->valid()) {
    append_to(job_session_, std::format("[启动失败] {}：{}", trimmed, channel->error()));
    if (on_error) on_error(std::format("启动失败：{}", channel->error()));
    return;
  }
  channel_ = std::move(channel);
  sessions_[job_session_]->running = true;
  sessions_[job_session_]->running_name = trimmed;
  stop_requested_.store(false);
  if (on_busy_change) on_busy_change(true);
  reader_ = std::jthread([this](std::stop_token stop) {
    std::string line;
    while (channel_ != nullptr && channel_->read_line(line)) {
      if (stop.stop_requested() || stop_requested_.load()) break;
      const std::scoped_lock guard(pending_mutex_);
      pending_.push_back(line);
    }
    const int code = channel_ != nullptr ? channel_->finish() : -1;
    const std::scoped_lock guard(pending_mutex_);
    job_finished_ = true;
    job_code_ = code;
  });
}

void Terminal::send_stop() {
  TerminalSession& target = current();
  if (target.pty != nullptr && target.pty->valid()) {
    // **PTY 模式：发 `Ctrl+C`（0x03），而不是杀 shell**。
    //
    // 这是真终端里“中止”的语义：把中断字符送进终端输入流，
    // 内核行规程向**前台进程组**发 SIGINT——当前命令被打断，
    // 而 shell 自己活着（接着给你提示符）。
    //
    // 杀 shell（`terminate`）会把整个会话弄没：用户想着“停一下这条”，
    // 结果终端直接死了、得重新开一个（实测踩到）。
    // 真要把整条会话关掉，那是 `close_session` 的事。
    send_bytes("\x03");
    return;
  }
  if (!target.running) {
    if (on_error) on_error("没有正在运行的命令");
    return;
  }
  stop_requested_.store(true);
  if (channel_ != nullptr) channel_->terminate();
}

void Terminal::finish_session(TerminalSession& session) {
  if (session.pty != nullptr && session.pty->valid()) session.pty->terminate();
  if (session.reader.joinable()) session.reader.join();
  if (session.pty != nullptr) (void)session.pty->wait();
  session.running = false;
  session.running_name.clear();
  if (!any_busy() && on_busy_change) on_busy_change(false);
}

// ════════════════════════════════════════════════════════════════════════════
// 每帧
// ════════════════════════════════════════════════════════════════════════════

void Terminal::pump() {
  // ⓪ 先处理“上一帧受理的关闭”。
  //
  // ⚠ **必须立即返回**：`apply_pending_close` 会发 `on_close_last_session`，
  // 宿主可能据此**把本元素整个拆掉**（关最后一个标签 ⇒ 面板收起）。
  // 那之后再碰 `this` 就是 use-after-free——实测表现为直接段错误
  //（`0xC0000005`，进程当场退出）。所以这里用了“处理完就走”。
  if (pending_close_.has_value()) {
    apply_pending_close();
    return;
  }

  // ① PTY 字节 → 屏幕（每个会话各自喂）。
  std::vector<std::string> pty_ready;
  {
    const std::scoped_lock guard(pending_mutex_);
    pty_ready.swap(pty_pending_);
  }
  if (!pty_ready.empty()) {
    for (auto& session : sessions_) {
      if (session == nullptr || session->screen == nullptr) continue;
      // 字节流是**全局**缓冲的（一条 PTY 一个读线程），所以要先判"这是哪个会话的"。
      // 简化：只有活动会话的 PTY 在跑时才有字节（多 PTY 同时跑的场景留待后续，
      // 那需要按会话分缓冲）。
      if (session.get() != &current() && sessions_.size() > 1) continue;
      for (const auto& chunk : pty_ready) session->screen->feed(chunk);
      // 标题变化（`OSC 0/2`）：shell 会设成"当前命令/目录"，标签跟着变更好用。
      const std::string title = session->screen->title();
      if (!title.empty() && title != session->running_name && on_title_change) {
        on_title_change(active_, title);
      }
    }
    mark_dirty();
  }
  // ② PTY 退出收尾。
  bool exited = false;
  int exit_code = 0;
  {
    const std::scoped_lock guard(pending_mutex_);
    exited = pty_exited_;
    exit_code = pty_exit_code_;
    if (exited) pty_exited_ = false;
  }
  if (exited) {
    TerminalSession& target = current();
    if (target.reader.joinable()) target.reader.join();
    target.exit_code = exit_code;
    target.running = false;
    append_to(active_, std::format("[退出码 {}] {}", exit_code,
                                   target.running_name.empty() ? "shell" : target.running_name));
    target.running_name.clear();
    if (!any_busy() && on_busy_change) on_busy_change(false);
    mark_dirty();
  }

  // ③ 行模式作业回流（与之前同一套：作业归属发起它的会话）。
  const std::size_t job = job_session_;
  if (job >= sessions_.size() || !sessions_[job]->running) {
    if (job < sessions_.size() && sessions_[job]->pty == nullptr) {
      const std::scoped_lock guard(pending_mutex_);
      pending_.clear();
    }
    return;
  }
  std::vector<std::string> ready;
  bool finished = false;
  int code = 0;
  {
    const std::scoped_lock guard(pending_mutex_);
    ready.swap(pending_);
    finished = job_finished_;
    code = job_code_;
  }
  if (finished && reader_.joinable()) reader_.join();
  for (auto& line : ready) append_to(job, std::move(line));
  if (!finished) return;
  append_to(job, std::format("[退出码 {}] {}（中止={}）", code, job_name_, stop_requested_.load()));
  sessions_[job]->running = false;
  sessions_[job]->running_name.clear();
  stop_requested_.store(false);
  job_finished_ = false;
  channel_.reset();
  if (!any_busy() && on_busy_change) on_busy_change(false);
  mark_dirty();
}

// ════════════════════════════════════════════════════════════════════════════
// 外观
// ════════════════════════════════════════════════════════════════════════════

void Terminal::set_monospace(bool value) {
  if (monospace_ == value) return;
  monospace_ = value;
  mark_dirty();
  mark_layout_dirty();
}

void Terminal::set_font_scale(float scale) {
  const float next = std::clamp(scale, 0.5f, 3.0f);
  if (next == font_scale_) return;
  font_scale_ = next;
  mark_dirty();
  mark_layout_dirty();
}

void Terminal::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.color = theme.colors().text;
  style_.font_size = metrics.font_base;
  base_font_size_ = metrics.font_base;
  // 单元格尺寸：等宽字体下"一列"的宽度。用它把**列数**换算成像素，
  // 屏幕才能按真实几何渲染（而不是让文本自己流式排）。
  cell_width_ = metrics.font_base * 0.6f;
  line_height_ = metrics.font_base * 1.35f;
  if (tabs_ != nullptr) tabs_->apply_theme(theme);
  if (view_ != nullptr) view_->apply_theme(theme);
  if (text_out_ != nullptr) text_out_->apply_theme(theme);
}

// ════════════════════════════════════════════════════════════════════════════
// 渲染
// ════════════════════════════════════════════════════════════════════════════

void Terminal::ensure_children() {
  if (tabs_ != nullptr) return;
  tabs_ = dynamic_cast<Tabs*>(add_child(std::make_unique<Tabs>()));
  tabs_->set_id("terminal-tabs");
  tabs_->on_change = [this](std::size_t index) { set_active_session(index); };
  tabs_->on_close = [this](std::size_t index) { (void)close_session(index); };
  // 输出区：
  //
  // * **PTY 模式不用子件**——屏幕必须由本元素**逐格自绘**（颜色、反显、光标
  //   都是逐格的事实）。套一个 `Text` 子件会出真 bug：子件在父的自绘**之后**
  //   重绘，把单色的纯文本盖在上面，于是"颜色全丢了"（实测：整片输出只剩
  //   主题文字色，ANSI 白解析了）。
  // * **行模式**才要子件：那是纯文本滚回，交给 `ScrollView` + `Text` 最省事。
  view_ = dynamic_cast<ScrollView*>(add_child(std::make_unique<ScrollView>()));
  view_->set_id("terminal-scroll");
  view_->set_follow_end(true);
  view_->set_on_scroll([this](float) {
    current().user_scrolled = view_ != nullptr && !view_->at_end();
  });
  text_out_ = dynamic_cast<Text*>(view_->add_child(std::make_unique<Text>()));
  text_out_->set_id("terminal-output");
  text_out_->set_multiline(true);
}

void Terminal::sync_screen(TerminalSession& session) {
  if (text_out_ == nullptr) return;
  const std::string text = session_text(active_);
  if (text == last_output_) return;
  text_out_->set_content(text);
  last_output_ = text;
  (void)session;
}

void Terminal::measure(const RenderContext& context, const Constraints& constraints) {
  ensure_children();
  const float width = constraints.max_width < kUnbounded ? constraints.max_width : 480.0f;
  const float height = constraints.max_height < kUnbounded ? constraints.max_height : 240.0f;
  measured_ = math::Size{width, height};
  (void)context;
}

void Terminal::arrange(const RenderContext& context, math::Rect rect) {
  ensure_children();
  bounds_ = rect;
  const Metrics& metrics = context.theme.metrics();
  const float tabs_h = tabs_visible_ ? metrics.control_height - 6.0f : 0.0f;
  tabs_rect_ = math::Rect{rect.x, rect.y, rect.width, tabs_h};
  output_rect_ = math::Rect{rect.x, rect.y + tabs_h, rect.width,
                            std::max(0.0f, rect.height - tabs_h)};
  if (tabs_ != nullptr && !tabs_rect_.is_empty()) {
    tabs_->measure(context, Constraints{});
    tabs_->arrange(context, tabs_rect_);
  }
  // **PTY 模式下把行模式的子件藏起来**：它们是单色的纯文本，会在本元素的
  // 逐格自绘之后重绘，把颜色/反显/光标全盖掉（见 `ensure_children` 的说明）。
  const bool pty = session(active_) != nullptr && session(active_)->screen != nullptr;
  if (view_ != nullptr) {
    view_->set_visible(!pty);
    if (!pty && !output_rect_.is_empty()) {
      view_->measure(context, Constraints{});
      view_->arrange(context, output_rect_.inset(math::Insets{4.0f, 4.0f, 4.0f, 4.0f}));
    }
  }
  // 把可用**列数/行数**换算出来上报给 PTY（TUI 程序据此排版）。
  const float usable_w = std::max(0.0f, output_rect_.width - 8.0f);
  const float usable_h = std::max(0.0f, output_rect_.height - 8.0f);
  const float cell_w = cell_width_ * font_scale_;
  const float cell_h = line_height_ * font_scale_;
  if (cell_w > 0.0f && cell_h > 0.0f) {
    const int cols = std::max(20, static_cast<int>(usable_w / cell_w));
    const int rows = std::max(4, static_cast<int>(usable_h / cell_h));
    if (cols != pty_size_.cols || rows != pty_size_.rows) {
      // 布局里不直接改 PTY（`arrange` 可能在测量阶段被调多次）；
      // 记下来，下一帧 `pump` 上报——尺寸抖动也顺带被吸收。
      pending_size_ = st::process::PtySize{cols, rows};
      pending_size_set_ = true;
    }
  }
  layout_dirty_ = false;
}

void Terminal::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const auto& colors = context.theme.colors();

  // 上一步记下的尺寸在这里上报（每帧一次，且此时布局已稳定）。
  auto* self = const_cast<Terminal*>(this);
  if (pending_size_set_) {
    const_cast<bool&>(pending_size_set_) = false;
    self->set_terminal_size(pending_size_);
  }
  // 屏幕底色（终端是"一屏文本"，底色把它与普通面板分开）。
  canvas.fill_rect(output_rect_, raster::Paint::solid(colors.surface_sunken));

  const TerminalSession& session = current();
  if (session.screen == nullptr) {
    // 行模式：`Text` 子件自己画（已挂进 `view_`）。
    self->sync_screen(const_cast<TerminalSession&>(session));
    return;
  }



  // ── PTY 模式：**逐格渲染屏幕** ──
  const st::text::AnsiScreen& screen = *session.screen;
  const TextPort& port = components_internal::text_port_of(context);
  const float font = base_font_size_ * font_scale_;
  // 单元格尺寸从**字体端口实际量出来**，不用猜的系数：
  // 猜出来的宽度会让"列 → 像素"换算与真实字形错位（表格列对不齐、光标飘）。
  const float cell_w = port.measure_width("M", font, text::FontRole::Monospace);
  const float line_h = port.line_height(font);
  // 基线：字形原点是**基线**，而我们是按行顶排的——所以要加上 ascent。
  const float ascent = port.ascent(font);
  const float origin_x = output_rect_.x + 4.0f;
  const float origin_y = output_rect_.y + 4.0f;

  const math::Color default_fg = colors.text;
  const math::Color default_bg = colors.surface_sunken;
  // 屏幕上第 0 行就画在输出区**顶部**——滚动已经由屏幕模型自己做了
  //（输出越过底部时它把整体上移）。这里再做一次“贴底”会把内容推到面板中间，
  // 看起来像“提示符跑到中间去了”（实测踩到）。
  for (int row = 0; row < screen.rows(); ++row) {
    const float y = origin_y + static_cast<float>(row) * line_h;
    if (y + line_h < output_rect_.y || y > output_rect_.bottom()) continue;   // 视口剔除
    // 逐格画：同一行的**相邻同风格**格子合成一段（减少绘制调用）。
    // 分段键是"样式 + 是否粗体"——粗体走单独的字形外扩参数，混在一段里会丢。
    int col = 0;
    while (col < screen.cols()) {
      const st::text::AnsiStyle style = screen.cell(row, col).style;
      int end = col;
      std::string run;
      while (end < screen.cols()) {
        const st::text::AnsiCell& next = screen.cell(row, end);
        if (next.continuation) {
          ++end;
          continue;
        }
        if (!(next.style == style)) break;
        utf8_append(next.ch, run);
        end += (next.ch >= 0x1100U && next.ch <= 0x3FFFDUL) ? 2 : 1;   // 宽字符占两列
      }
      if (run.empty()) {
        col = end > col ? end : col + 1;
        continue;
      }
      const float x = origin_x + static_cast<float>(col) * cell_w;
      const float width = static_cast<float>(end - col) * cell_w;
      math::Color fg = resolve(style.fg, default_fg, style.bold);
      math::Color bg = resolve(style.bg, default_bg, false);
      if (style.reverse) std::swap(fg, bg);
      if (style.dim) fg = math::Color{fg.r, fg.g, fg.b, 150};
      if (!(bg == default_bg)) {
        canvas.fill_rect(math::Rect{x, y, width, line_h}, raster::Paint::solid(bg));
      }
      // `FontRole::Monospace` 是**必须**的：终端的一切对齐（表格列、进度条、
      // 光标位置）都建立在"每列等宽"上；用比例字体渲染会整体散架。
      port.draw(canvas, run, math::Point{x, y + ascent}, font, fg, text::FontRole::Monospace,
                0.0f, style.bold);
      col = end;
    }
  }

  // **光标画出来**（这是"终端"与"输出区"在视觉上的分水岭）。
  if (screen.cursor_visible() && !screen.in_alt_screen()) {
    const float cx = origin_x + static_cast<float>(screen.cursor_col()) * cell_w;
    const float cy = origin_y + static_cast<float>(screen.cursor_row()) * line_h;
    const float blink = std::fmod(static_cast<float>(context.time_seconds), 1.0f);
    if (blink < 0.5f) {
      switch (screen.cursor_shape()) {
        case st::text::AnsiCursorShape::Block:
          canvas.fill_rect(math::Rect{cx, cy, cell_w, line_h},
                           raster::Paint::solid(math::Color{colors.text.r, colors.text.g,
                                                             colors.text.b, 110}));
          break;
        case st::text::AnsiCursorShape::Underline:
          canvas.fill_rect(math::Rect{cx, cy + line_h - 2.0f, cell_w, 2.0f},
                           raster::Paint::solid(colors.text));
          break;
        case st::text::AnsiCursorShape::Bar:
          canvas.fill_rect(math::Rect{cx, cy, 2.0f, line_h}, raster::Paint::solid(colors.text));
          break;
      }
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// 事件
// ════════════════════════════════════════════════════════════════════════════

auto Terminal::on_event(const RenderContext& context, Event& event) -> bool {
  // **键盘 → 字节**（PTY 模式）：这是真终端的输入方式。
  // 补全（Tab）、历史（↑↓）、行内编辑（←→/Home/Backspace）全交给 shell，
  // 组件只做"按键翻译成字节序列"这一件事。
  if (event.kind == EventKind::KeyDown || event.kind == EventKind::TextInput) {
    if (!pty_active()) return Element::on_event(context, event);
    std::string bytes;
    if (event.kind == EventKind::TextInput) {
      bytes = event.text;
    } else {
      const std::string& key = event.key;
      if (key == "Enter") bytes = "\r";
      else if (key == "Backspace") bytes = "\x7f";
      else if (key == "Tab") bytes = "\t";
      else if (key == "Escape" || key == "Esc") bytes = "\x1b";
      else if (key == "ArrowUp") bytes = "\x1b[A";
      else if (key == "ArrowDown") bytes = "\x1b[B";
      else if (key == "ArrowRight") bytes = "\x1b[C";
      else if (key == "ArrowLeft") bytes = "\x1b[D";
      else if (key == "Home") bytes = "\x1b[H";
      else if (key == "End") bytes = "\x1b[F";
      else if (key == "Delete") bytes = "\x1b[3~";
      else if (key == "PageUp") bytes = "\x1b[5~";
      else if (key == "PageDown") bytes = "\x1b[6~";
      else if (event.ctrl && key.size() == 1) {
        // Ctrl+字母 → 0x01..0x1A（`Ctrl+C` = 0x03、`Ctrl+D` = 0x04…）。
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
        if (lower >= 'a' && lower <= 'z') {
          bytes = std::string(1, static_cast<char>(lower - 'a' + 1));
        } else if (key == "[") {
          bytes = "\x1b";
        }
      }
      if (bytes.empty() && press_plain_bytes(key, event, bytes)) {
        // 无修饰的可见字符：直接送那个字符（终端的常规路径）。
      }
    }
    if (!bytes.empty()) {
      send_bytes(bytes);
      event.handled = true;
      return true;
    }
  }
  if (event.kind == EventKind::MouseDown) {
    // 点哪里都算"聚焦终端"，但**不**在应用层做"点击定位光标"——
    // 那是 shell 的事（光标位置属于屏幕状态，不是我们可以随便挪的）。
    if (output_rect_.contains(event.position)) {
      event.handled = true;
      return true;
    }
  }
  return Element::on_event(context, event);
}

/// 无修饰可见字符 → 原样送（终端要"敲什么就是什么"）。
auto Terminal::press_plain_bytes(const std::string& key, const Event& event, std::string& out)
    -> bool {
  if (event.ctrl || event.alt || event.meta) return false;
  if (key == "Shift" || key == "Control" || key == "Alt" || key == "Meta") return false;
  if (key.size() == 1) {
    out = key;
    return true;
  }
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// 属性面 / 动作面
// ════════════════════════════════════════════════════════════════════════════

auto Terminal::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.insert(names.end(),
               {"sessions", "active_session", "session_title", "working_directory", "busy",
                "output", "screen", "cursor", "pty", "alt_screen", "monospace",
                "tabs_visible", "font_scale"});
  return names;
}

auto Terminal::get_property(std::string_view name) const -> std::optional<std::string> {
  const TerminalSession* target = session(active_);
  if (name == "sessions") return std::to_string(sessions_.size());
  if (name == "active_session") return std::to_string(active_);
  if (name == "session_title") {
    return target != nullptr ? target->title : std::string{};
  }
  if (name == "working_directory") return effective_cwd(active_);
  if (name == "busy") return current().running ? "true" : "false";
  if (name == "output") return session_text(active_);
  if (name == "cursor") {
    if (target == nullptr || target->screen == nullptr) return std::string("0,0");
    return std::format("{},{}", target->screen->cursor_row(), target->screen->cursor_col());
  }
  if (name == "alt_screen") {
    if (target == nullptr || target->screen == nullptr) return std::string("false");
    return target->screen->in_alt_screen() ? "true" : "false";
  }
  if (name == "pty") {
    if (target == nullptr || target->pty == nullptr) return std::string("false");
    return target->pty->valid() ? "true" : "false";
  }
  if (name == "screen") {
    // 屏幕内容按**行**回报（`\n` 分隔、行尾去空白）——与 `output` 的区别是
    // 它保留**空格定位**（`output` 是 plain_text，行尾 trim）。
    if (target == nullptr || target->screen == nullptr) return std::string{};
    std::string out;
    for (int row = 0; row < target->screen->rows(); ++row) {
      if (row != 0) out.push_back('\n');
      out += target->screen->row_text(row);
    }
    return out;
  }
  if (name == "monospace") return monospace_ ? "true" : "false";
  if (name == "tabs_visible") return tabs_visible_ ? "true" : "false";
  if (name == "font_scale") return std::format("{:.2f}", font_scale_);
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
  if (name == "font_scale") {
    set_font_scale(static_cast<float>(std::strtod(std::string(value).c_str(), nullptr)));
    return true;
  }
  return Element::set_property(name, value);
}

auto Terminal::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "open_shell" || action == "shell") {
    open_shell();
    return true;
  }
  if (action == "run") {   // PTY 模式 = 送一行输入；行模式 = 跑一条命令
    run(std::string(argument));
    return true;
  }
  if (action == "send") {  // 原样送字节（自动化驱动按键序列用；`\r` 等用转义写）
    send_bytes(argument);
    return true;
  }
  if (action == "send_line") {   // 送一行并回车
    send_bytes(std::string(argument) + "\r");
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
    if (on_submit) on_submit(session_text(active_));
    return true;
  }
  return Element::invoke_action(action, argument);
}

auto Terminal::semantics_value() const -> std::string {
  const TerminalSession* target = session(active_);
  const std::string title = target != nullptr ? target->title : std::string{"-"};
  return std::format("{} | {} | {}", title, pty_active() ? "PTY" : "行模式",
                     current().running ? "运行中" : "空闲");
}

}  // namespace st::ui
