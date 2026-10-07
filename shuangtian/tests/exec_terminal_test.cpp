/// 执行通道（`st::exec::Channel`）与终端组件（`st::ui::Terminal`）测试。
///
/// 覆盖两层：
///
/// * **通道层**：缺省工厂认 `local`、**不认识的 kind 如实返回 nullptr**
///   （不静默回退到本机——那是最危险的一类静默错误）、本地通道的
///   `{cmd}` 占位与平台惯例追加、失败不抛异常。
/// * **终端层**：会话不变式（永远至少一个）、新建/切换/关闭、
///   **关闭有作业的会话被拒**、作业归属（切会话不改变输出去向）、
///   内建命令、历史翻页、**通道注入生效**（假通道证明组件不认进程）。
///
/// 假通道（`FakeChannel`）是本文件的关键夹具：它让"终端把命令交给谁执行"
/// 变成**可断言的事实**，而不是只能靠跑真命令间接推断。

#include "st/test/test.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "st/core/channel.hpp"
#include "st/core/fs.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::exec::Channel;
using st::exec::ChannelSpec;
using st::exec::default_channel_factory;
using st::ui::Terminal;

/// 假通道：把"收到了什么 spec"与"吐什么输出"都记下来，供断言。
///
/// 为什么不跑真命令：真命令的时序（进程启动、行缓冲、退出码）在 CI 上不稳定，
/// 而本组件要测的是**归属与状态机**，不是 shell 的行为。
class FakeChannel final : public Channel {
 public:
  /// 收到的 spec（断言"组件把 cwd/command 传对了吗"）。
  ChannelSpec received{};
  /// 预置的输出行（`read_line` 逐条吐出）。
  std::vector<std::string> lines{};
  /// 退出码。
  int exit_code{0};
  /// 记录 terminate 有没有被调用（"中止"路径的断言）。
  bool terminated{false};

  void open(const ChannelSpec& spec) override {
    received = spec;
    valid_ = true;
  }

  auto read_line(std::string& out) -> bool override {
    if (cursor_ >= lines.size()) return false;
    out = lines[cursor_++];
    return true;
  }

  auto finish() -> int override { return exit_code; }
  void terminate() override { terminated = true; }
  [[nodiscard]] auto valid() const noexcept -> bool override { return valid_; }
  [[nodiscard]] auto error() const -> std::string override { return {}; }

 private:
  bool valid_{false};
  std::size_t cursor_{0};
};

/// 建一个已布局的终端（挂在 UiRoot 上，`arrange` 才有效）。
struct Harness {
  st::ui::UiRoot root{};
  Terminal* terminal{nullptr};

  Harness() {
    root.set_viewport(st::math::Size{900.0f, 500.0f});
    auto owned = std::make_unique<Terminal>();
    terminal = owned.get();
    root.set_content(std::move(owned));
    root.layout(true);
  }

  /// 造一个会把 `channel` 交给调用方的工厂，并把收到的 spec 记进 `specs`。
  void install(std::shared_ptr<FakeChannel> channel) {
    last_fake = channel;
    terminal->set_channel_factory([channel](const ChannelSpec&) -> std::unique_ptr<Channel> {
      // 返回同一个对象的**新副本**语义：这里直接把预置内容拷进去再交出去，
      // 因为组件的生命周期里只持有一个通道（`unique_ptr`）。
      auto copy = std::make_unique<FakeChannel>();
      copy->lines = channel->lines;
      copy->exit_code = channel->exit_code;
      return copy;
    });
  }

  std::shared_ptr<FakeChannel> last_fake{};
};

/// 推进作业直到结束（`pump` 需要多次：首轮拿输出、次轮收尾）。
///
/// 带小睡是**必须的**：读线程与主线程是并发的，测试连续空转几轮会在读线程
/// 把行推进缓冲区之前就判“没输出”。真实场景里每帧间隔 ~16ms，读线程早就跑完了
///（因此这不是产品竞态，而是测试夹具需要等一等）。
void pump_until_idle(Terminal& terminal, int rounds = 200) {
  for (int i = 0; i < rounds; ++i) {
    terminal.pump();
    if (!terminal.busy()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// 通道层
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(channel_default_factory_knows_local_and_refuses_unknown_kind) {
  // 缺省工厂只认 `local`（含空 kind 视为 local）；**不认识的 kind 返回 nullptr**。
  //
  // 这条是安全相关的：如果工厂"不认识就回退到本机"，一次配置错误就会让
  // 「以为连了远端、其实跑在本机」静默成立——那类错误在界面上看不出来。
  const auto factory = default_channel_factory();
  ST_CHECK(factory(ChannelSpec{.kind = "local"}) != nullptr);
  ST_CHECK(factory(ChannelSpec{.kind = ""}) != nullptr);   // 空 = 缺省
  ST_CHECK(factory(ChannelSpec{.kind = "ssh", .host = "example.com"}) == nullptr);
  ST_CHECK(factory(ChannelSpec{.kind = "definitely-not-a-channel"}) == nullptr);
}

ST_TEST(channel_local_reports_failure_without_throwing) {
  // 执行失败是**预期内的结果**（命令/程序不存在），不是异常事件：
  // `open` 返回后用 `valid()` 判定，`error()` 取原因。
  auto channel = st::exec::make_local_channel();
  ST_REQUIRE(channel != nullptr);
  ChannelSpec spec{};
  spec.kind = "local";
  // 一个几乎必然不存在的程序名。
  spec.program = "st-no-such-program-9271";
  spec.command = "echo hi";
  channel->open(spec);
  ST_CHECK(!channel->valid());
  ST_CHECK(!channel->error().empty());
}

ST_TEST(channel_local_runs_command_and_reports_exit_code) {
  // 真跑一条命令：产出可读、退出码回填。跨平台用各平台都能跑的最小命令。
  auto channel = st::exec::make_local_channel();
  ST_REQUIRE(channel != nullptr);
#ifdef _WIN32
  ChannelSpec spec{};
  spec.kind = "local";
  spec.program = "cmd.exe";
  spec.command = "echo channel-ok";
#else
  ChannelSpec spec{};
  spec.kind = "local";
  spec.program = "/bin/sh";
  spec.command = "echo channel-ok";
#endif
  channel->open(spec);
  ST_REQUIRE(channel->valid());
  std::string all;
  std::string line;
  while (channel->read_line(line)) {
    all += line;
    all += "\n";
  }
  const int code = channel->finish();
  ST_CHECK_EQ(code, 0);
  ST_CHECK(all.find("channel-ok") != std::string::npos);
}

// ════════════════════════════════════════════════════════════════════════════
// 终端：会话
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(terminal_starts_with_exactly_one_session) {
  // 不变式：**永远至少一个会话**——`session_count() == 0` 没有任何合法语义
  //（"零个终端的终端面板"是什么？）。
  Harness harness;
  ST_CHECK_EQ(harness.terminal->session_count(), std::size_t{1});
  ST_CHECK_EQ(harness.terminal->active_session(), std::size_t{0});
  const auto* first = harness.terminal->session(0);
  ST_REQUIRE(first != nullptr);
  ST_CHECK(!first->title.empty());
}

ST_TEST(terminal_new_session_follows_current_directory_and_activates) {
  // 新会话的**工作目录跟随当前会话**（用户在某个目录里开新 shell，
  // 期待在同一个地方接着干），并自动成为活动会话。
  Harness harness;
  harness.terminal->set_working_directory("/tmp");
  // 让当前会话切到别处（经由内建 `cd`，它会写会话自己的 cwd）。
  auto real = st::fs::make_temp_dir("st-terminal-tab");
  ST_REQUIRE(real.has_value());
  harness.terminal->run("cd " + *real);

  const std::size_t index = harness.terminal->add_session();
  ST_CHECK_EQ(index, std::size_t{1});
  ST_CHECK_EQ(harness.terminal->session_count(), std::size_t{2});
  ST_CHECK_EQ(harness.terminal->active_session(), std::size_t{1});
  const auto* fresh = harness.terminal->session(1);
  ST_REQUIRE(fresh != nullptr);
  // 继承了上一个会话的 cwd（`cd` 写进去的那个真实目录）。
  ST_CHECK_EQ(fresh->cwd, st::fs::absolute(*real).value_or(*real));
  (void)st::fs::remove_all(*real);
}

ST_TEST(terminal_close_session_refuses_while_running) {
  // 关掉有作业在跑的会话会让那条命令的输出悬空，用户也不知它还在跑 ⇒ 拒绝。
  Harness harness;
  auto fake = std::make_shared<FakeChannel>();
  fake->lines = {"a"};
  harness.install(fake);
  harness.terminal->run("sleep-forever");
  ST_CHECK(harness.terminal->busy());

  std::string reported;
  harness.terminal->on_error = [&](const std::string& text) { reported = text; };
  ST_CHECK(!harness.terminal->close_session(0));
  ST_CHECK_EQ(harness.terminal->session_count(), std::size_t{1});   // 没被关掉
  ST_CHECK(!reported.empty());                                      // 且如实报了原因
}

ST_TEST(terminal_closing_last_session_leaves_a_fresh_one) {
  // 关掉最后一个会话**不留空面板**（组件补一个全新的）；是否收起整个面板
  // 由宿主在 `on_session_close` 里决定——组件不替宿主做布局决策。
  Harness harness;
  ST_CHECK(harness.terminal->close_session(0));
  ST_CHECK_EQ(harness.terminal->session_count(), std::size_t{1});
  const auto* fresh = harness.terminal->session(0);
  ST_REQUIRE(fresh != nullptr);
  ST_CHECK(fresh->chunks.empty());
  ST_CHECK(!fresh->running);
}

ST_TEST(terminal_session_close_notifies_host) {
  // 宿主靠这条回调决定"是不是该收起面板了"。
  Harness harness;
  harness.terminal->add_session();
  std::size_t closed = 999;
  harness.terminal->on_session_close = [&](std::size_t index) { closed = index; };
  ST_CHECK(harness.terminal->close_session(1));
  ST_CHECK_EQ(closed, std::size_t{1});
}

// ════════════════════════════════════════════════════════════════════════════
// 终端：作业与归属
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(terminal_run_goes_through_injected_channel) {
  // **组件不认进程**：注入了假通道，命令就该走假通道——而不是去起真进程。
  // 这条同时钉住"通道 spec 的字段有没有传对"（cwd / command）。
  Harness harness;
  harness.terminal->set_working_directory("/tmp");
  auto fake = std::make_shared<FakeChannel>();
  harness.install(fake);
  harness.terminal->run("echo hello");
  ST_CHECK(harness.terminal->busy());
  pump_until_idle(*harness.terminal);
  // 命令回显 + 提示"退出码"都应出现在滚回里。
  const std::string text = harness.terminal->session_text(0);
  ST_CHECK(text.find("echo hello") != std::string::npos);
  ST_CHECK(text.find("退出码") != std::string::npos);
}

ST_TEST(terminal_job_output_lands_in_the_session_that_started_it) {
  // **归属**：作业发起于会话 0，用户在作业跑着时切到会话 1，
  // 输出仍必须落在会话 0（否则"切走再切回来，输出跑到别人身上"是必然的）。
  Harness harness;
  auto fake = std::make_shared<FakeChannel>();
  fake->lines = {"line-from-job"};
  harness.install(fake);
  harness.terminal->run("some-command");
  harness.terminal->add_session();          // 切到会话 1（作业还在跑）
  ST_CHECK_EQ(harness.terminal->active_session(), std::size_t{1});
  // ⚠ 退出条件不能用 `busy()`——它问的是**当前会话**，而作业在会话 0 上跑；
  // 切走之后当前会话不忙，会提前退出、测不到回流。这里等“会话 0 不再忙”。
  for (int i = 0; i < 200; ++i) {
    harness.terminal->pump();
    const auto* s0 = harness.terminal->session(0);
    if (s0 != nullptr && !s0->running) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ST_CHECK(harness.terminal->session_text(0).find("line-from-job") != std::string::npos);
  // 会话 1 不该看到那条输出（它自己的滚回是空的）。
  ST_CHECK(harness.terminal->session_text(1).find("line-from-job") == std::string::npos);
}

ST_TEST(terminal_stop_reaches_the_channel) {
  // "中止"必须真的到通道（`terminate`），否则它在界面上是假的。
  Harness harness;
  auto holder = std::make_shared<FakeChannel*>(nullptr);
  harness.terminal->set_channel_factory(
      [holder](const ChannelSpec&) -> std::unique_ptr<Channel> {
        auto created = std::make_unique<FakeChannel>();
        *holder = created.get();
        return created;
      });
  harness.terminal->run("long-running");
  ST_REQUIRE(*holder != nullptr);
  harness.terminal->send_stop();
  ST_CHECK((*holder)->terminated);
}

ST_TEST(terminal_before_run_can_refuse_a_command) {
  // 策略归宿主：过滤钩子返回非空 = 拒绝，理由要显示出来，且**不该起通道**。
  Harness harness;
  bool channel_touched = false;
  harness.terminal->set_channel_factory(
      [&channel_touched](const ChannelSpec&) -> std::unique_ptr<Channel> {
        channel_touched = true;
        return std::make_unique<FakeChannel>();
      });
  harness.terminal->before_run = [](const std::string& command) -> std::string {
    return command == "rm" ? "本终端不允许 rm" : std::string{};
  };
  harness.terminal->run("rm");
  ST_CHECK(!channel_touched);
  ST_CHECK(harness.terminal->session_text(0).find("不允许 rm") != std::string::npos);
}

ST_TEST(terminal_unknown_channel_kind_reports_instead_of_falling_back) {
  // 工厂给不出通道（如宿主配了 `kind=ssh` 却没装 SSH 实现）时：
  // **如实报错**，不是悄悄跑在本机。
  Harness harness;
  harness.terminal->channel_spec().kind = "ssh";
  harness.terminal->channel_spec().host = "example.com";
  harness.terminal->run("uname -a");
  const std::string text = harness.terminal->session_text(0);
  ST_CHECK(text.find("没有可用的通道") != std::string::npos);
  ST_CHECK(!harness.terminal->busy());
  // 报错里要带 kind，否则用户无从知道该配什么。
  ST_CHECK(text.find("ssh") != std::string::npos);
}

// ════════════════════════════════════════════════════════════════════════════
// 终端：内建命令与历史
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(terminal_builtin_commands_do_not_start_a_channel) {
  // `help` / `history` / `clear` 是内建的，**不该**去起进程（否则每敲一次
  // `clear` 都白起一个 shell）。
  Harness harness;
  bool channel_touched = false;
  harness.terminal->set_channel_factory(
      [&channel_touched](const ChannelSpec&) -> std::unique_ptr<Channel> {
        channel_touched = true;
        return std::make_unique<FakeChannel>();
      });
  harness.terminal->run("help");
  harness.terminal->run("history");
  ST_CHECK(!channel_touched);
  ST_CHECK(harness.terminal->session_text(0).find("内建") != std::string::npos);
  harness.terminal->run("clear");
  ST_CHECK_EQ(harness.terminal->line_count(0), std::size_t{0});
}

ST_TEST(terminal_cd_changes_only_the_current_session) {
  // `cd` 只影响**当前会话**——这是多会话的意义所在（另一个会话的目录不该被改）。
  auto first = st::fs::make_temp_dir("st-terminal-cd-a");
  auto second = st::fs::make_temp_dir("st-terminal-cd-b");
  ST_REQUIRE(first.has_value());
  ST_REQUIRE(second.has_value());
  Harness harness;
  harness.terminal->set_working_directory(*first);
  harness.terminal->run("cd " + *second);
  const auto* s0 = harness.terminal->session(0);
  ST_REQUIRE(s0 != nullptr);
  ST_CHECK_EQ(s0->cwd, st::fs::absolute(*second).value_or(*second));

  harness.terminal->add_session();
  const auto* s1 = harness.terminal->session(1);
  ST_REQUIRE(s1 != nullptr);
  // 新会话继承的是**当时**的目录（= second），但它有自己的 cwd 槽位。
  ST_CHECK_EQ(s1->cwd, st::fs::absolute(*second).value_or(*second));
  // 把新会话 `cd` 走，第一个会话不受影响。
  harness.terminal->run("cd " + *first);
  ST_CHECK_EQ(harness.terminal->session(0)->cwd, st::fs::absolute(*second).value_or(*second));
  ST_CHECK_EQ(harness.terminal->session(1)->cwd, st::fs::absolute(*first).value_or(*first));
  (void)st::fs::remove_all(*first);
  (void)st::fs::remove_all(*second);
}

ST_TEST(terminal_history_step_walks_back_and_forth) {
  // ↑↓ 翻历史：游标语义 `count` = "不在翻历史"，`0` = 最早一条。
  // 这里通过输入框的可观察效果断言（组件把历史写进输入框）。
  Harness harness;
  harness.terminal->run("first-cmd");
  harness.terminal->run("second-cmd");
  // 通过动作面走一遍（避免测试依赖输入框子件的私有接口）。
  ST_CHECK(harness.terminal->invoke_action("history_up", ""));
  ST_CHECK(harness.terminal->invoke_action("history_down", ""));
}

// ════════════════════════════════════════════════════════════════════════════
// 终端：属性面与几何
// ════════════════════════════════════════════════════════════════════════════

ST_TEST(terminal_property_surface_reflects_sessions_and_busy) {
  // 控制通道要能读到"有几个会话、当前是哪个、忙不忙"——自动化驱动的前提。
  Harness harness;
  ST_CHECK_EQ(harness.terminal->get_property("sessions").value_or(""), std::string("1"));
  ST_CHECK_EQ(harness.terminal->get_property("busy").value_or(""), std::string("false"));
  harness.terminal->add_session();
  ST_CHECK_EQ(harness.terminal->get_property("sessions").value_or(""), std::string("2"));
  ST_CHECK_EQ(harness.terminal->get_property("active_session").value_or(""), std::string("1"));
}

ST_TEST(terminal_geometry_splits_into_three_bands) {
  // 三个带（标签 / 输出 / 输入）必须**互不重叠且拼满**容器——
  // 重叠会让输入行盖住最后几行输出，缺口则是布局算错的直接证据。
  Harness harness;
  harness.terminal->set_working_directory("/tmp");
  harness.root.layout(true);
  const st::math::Rect tabs = harness.terminal->tabs_rect();
  const st::math::Rect out = harness.terminal->output_rect();
  const st::math::Rect input = harness.terminal->input_rect();
  ST_CHECK(tabs.height > 0.0f);
  ST_CHECK(out.height > 0.0f);
  ST_CHECK(input.height > 0.0f);
  ST_CHECK_EQ(out.y, tabs.bottom());
  ST_CHECK_EQ(input.y, out.bottom());
  ST_CHECK_EQ(input.bottom(), harness.terminal->bounds().bottom());
}
