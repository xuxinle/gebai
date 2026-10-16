/// 控制通道协议一致性测试（st-control/1 · DESIGN.md §6.2 声明 vs 实现）。
///
/// 背景（审视报告 P0-1）：协议文档声明了 drag / wait for=frames / capture format 等能力，
/// 实现却拒绝或静默忽略——「§ 是稳定接口」的约定被实现漂移破坏，且没有任何测试拦截。
/// 本文件把 §6.2 的关键声明钉成端到端测试：起真实 Server + 真实 TCP 连接，
/// 按协议走完整请求/响应，而不是只测内部函数（那测不到「文档↔实现」的一致性）。
///
/// 覆盖：token 鉴权（缺失/错误拒绝、正确放行、hello 前方法被拒）、drag 序列、
/// wait for=frames、invoke 未知动作报错、capture 落盘白名单、key press 完整 down+up、
/// capture.hash（像素哈希）/ visual.diff（基线比对）。

#include "st/test/test.hpp"
#include <cstdio>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "st/codec/png.hpp"
#include "st/control/control.hpp"
#include "st/core/fs.hpp"
#include "st/core/net.hpp"
#include "st/core/time.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/file_dialog.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/ui_root.hpp"

#include "st/ext/json.hpp"

namespace {

using st::Json;
using st::ui::Button;
using st::ui::Input;
using st::ui::Panel;
using st::ui::UiRoot;

/// 测试用宿主：最小实现 control::Host（不依赖 app 层——控制层只面向该接口，正好验证依赖倒置）。
class TestHost final : public st::control::Host {
 public:
  UiRoot root_{};
  std::string app_name_{"protocol-test"};
  std::string app_version_{"0.0.0"};
  int quits{0};
  int repaints{0};

  /// 与真实画布同语义的夹取：`region` 为空 = 全屏（合成帧 64×48），
  /// 否则与帧缓冲求交（视口 64×48）。
  ///
  /// ⚠ 桩**必须真的夹取**，否则 "尺寸来自导出而非请求" 那条契约根本测不出来：
  /// 不夹取时请求值恒等于实际值，把一个直接从 `region` 算尺寸的错误实现
  /// 也会判绿（实测：本用例第一版就是这么假绿的）。
  [[nodiscard]] auto clamp_like_real_canvas(const st::math::IntRect& region) const
      -> std::pair<int, int> {
    constexpr int kFrameW = 64;
    constexpr int kFrameH = 48;
    if (region.is_empty()) return {kFrameW, kFrameH};
    const int x0 = std::max(0, region.x);
    const int y0 = std::max(0, region.y);
    const int x1 = std::min(kFrameW, region.x + region.width);
    const int y1 = std::min(kFrameH, region.y + region.height);
    if (x1 <= x0 || y1 <= y0) return {0, 0};
    return {x1 - x0, y1 - y0};
  }

  [[nodiscard]] auto root() -> UiRoot& override { return root_; }
  [[nodiscard]] auto app_name() const -> std::string override { return app_name_; }
  [[nodiscard]] auto app_version() const -> std::string override { return app_version_; }
  [[nodiscard]] auto backend_name() const -> std::string_view override { return "headless"; }
  [[nodiscard]] auto headless() const -> bool override { return true; }
  [[nodiscard]] auto viewport() const -> st::math::Size override { return {400, 300}; }
  [[nodiscard]] auto device_scale() const -> float override { return 1.0f; }
  auto set_device_scale(float) -> st::Status override { return st::ok(); }
  [[nodiscard]] auto metrics() const -> st::control::Metrics override { return {}; }  void request_quit() override { ++quits; }
  void request_repaint() override { ++repaints; }
  void set_theme_mode(st::ui::ThemeMode mode) override { root_.set_theme(mode == st::ui::ThemeMode::Dark ? st::ui::Theme::dark() : st::ui::Theme::light()); }
  [[nodiscard]] auto capture_to_file(std::string_view, st::math::IntRect region)
      -> st::Result<st::control::Host::SavedShot> override {
    const auto [width, height] = clamp_like_real_canvas(region);
    return st::control::Host::SavedShot{"/tmp/fake.png", width, height};
  }  [[nodiscard]] auto capture_png(st::math::IntRect region)
      -> st::Result<st::control::Host::PngView> override {
    const auto [width, height] = clamp_like_real_canvas(region);
    return st::control::Host::PngView{{0x89, 0x50, 0x4E, 0x47}, width, height};
  }
  /// 合成像素视图：微小渐变图案（视觉断言测试用——内容可预测、足够区分）。
  [[nodiscard]] auto capture_pixels(st::math::IntRect region) -> st::Result<st::control::PixelView> override {
    const auto [width, height] = clamp_like_real_canvas(region);
    st::control::PixelView view;
    view.width = width;
    view.height = height;
    view.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                   static_cast<std::size_t>(x)) *
                                  4U;
        view.rgba[index] = static_cast<std::uint8_t>((x * 4) & 0xFF);
        view.rgba[index + 1] = static_cast<std::uint8_t>((y * 4) & 0xFF);
        view.rgba[index + 2] = static_cast<std::uint8_t>(((x + y) * 2) & 0xFF);
        view.rgba[index + 3] = 0xFF;
      }
    }
    return view;
  }
  [[nodiscard]] auto log_lines(std::size_t) const -> std::vector<std::string> override { return {}; }
};

/// 帧协议客户端（与 Python 探针同构：4 字节大端长度 + JSON）。
class Probe {
 public:
  explicit Probe(std::uint16_t port, std::string token = "")
      : token_(std::move(token)) {
    auto connected = st::net::connect_tcp("127.0.0.1", port, 2000);
    ST_REQUIRE(connected.has_value());
    if (connected) stream_ = std::move(*connected);
  }

  [[nodiscard]] auto call(const std::string& method, Json params = Json::object(), int timeout_ms = 3000)
      -> Json {
    const std::uint64_t id = ++next_id_;
    Json request = Json::object();
    request["id"] = id;
    request["method"] = method;
    if (!token_.empty() && method == "hello") params["token"] = token_;
    request["params"] = params;
    const std::string body = st::json_dump(request);
    std::vector<std::uint8_t> frame(4 + body.size());
    frame[0] = static_cast<std::uint8_t>((body.size() >> 24U) & 0xFFU);
    frame[1] = static_cast<std::uint8_t>((body.size() >> 16U) & 0xFFU);
    frame[2] = static_cast<std::uint8_t>((body.size() >> 8U) & 0xFFU);
    frame[3] = static_cast<std::uint8_t>(body.size() & 0xFFU);
    std::copy(body.begin(), body.end(), frame.begin() + 4);
    auto status = stream_.write_all(frame);
    ST_CHECK(status.has_value());
    auto reply = read_frame(timeout_ms);
    ST_CHECK(reply.has_value());
    if (reply.has_value()) ST_CHECK_EQ((*reply)["id"].get<std::uint64_t>(), id);
    return *reply;
  }

  /// 读一帧（超时返回 nullopt）。
  [[nodiscard]] auto read_frame(int timeout_ms) -> std::optional<Json> {
    std::array<std::uint8_t, 4> header{};
    if (!read_exact(header, timeout_ms)) return std::nullopt;
    const std::uint32_t length = (static_cast<std::uint32_t>(header[0]) << 24U) |
                                 (static_cast<std::uint32_t>(header[1]) << 16U) |
                                 (static_cast<std::uint32_t>(header[2]) << 8U) |
                                 static_cast<std::uint32_t>(header[3]);
    std::vector<std::uint8_t> body(length);
    if (!read_exact(body, timeout_ms)) {
      return std::nullopt;
    }
    auto parsed = st::json_parse(std::string(body.begin(), body.end()));
    if (!parsed) return std::nullopt;
    return *parsed;
  }

 private:
  [[nodiscard]] auto read_exact(std::span<std::uint8_t> target, int timeout_ms) -> bool {
    std::size_t filled = 0;
    const std::int64_t deadline = st::time::now_ms() + timeout_ms;
    while (filled < target.size()) {
      const std::int64_t remaining = deadline - st::time::now_ms();
      if (remaining <= 0) return false;
      // 阻塞 socket 上 read_some 会无限等：先探可读，限定剩余窗口（不依赖 SO_RCVTIMEO）
      auto ready = stream_.wait_readable(static_cast<int>(remaining));
      if (!ready || !*ready) return false;
      auto chunk = stream_.read_some(target.subspan(filled));
      if (!chunk) return false;
      if (*chunk == 0) return false;
      filled += *chunk;
    }
    return true;
  }

  st::net::TcpStream stream_;
  std::string token_;
  std::uint64_t next_id_{0};
};

/// 一轮完整的服务器 fixture：真实监听 + 记录 token（从 start 传入的 options 读回）。
struct ServerFixture {
  TestHost host{};
  std::unique_ptr<st::control::Server> server{};
  std::uint16_t port{0};
  std::string token{};

  /// `capture_dirs_override`：传 `std::vector<std::string>{}`（显式空）表示
  /// **禁止一切落盘**；`std::nullopt` = 未设置（用 `default_capture_dirs()`）。
  explicit ServerFixture(std::optional<std::string> token_override = std::nullopt,
                         std::optional<std::vector<std::string>> capture_dirs_override =
                             std::nullopt) {
    host.root_.set_theme(st::ui::Theme::light());
    // 子节点必须在 **set_content 之前** 挂好：`set_content` 里的 `assign_ids` 会把根
    // 内容自身的 id 写成 "root"（与虚拟根同名），而 `UiRoot::find` 的遍历先查父后查子——
    // 一个与虚拟根同名的容器会**遮蔽整棵子树**，子树里所有 id 都变成 not_found
    // （实测踩到：面板恰好抢到 "root" 后，`#btn-ok`/`#code` 都突然找不到了）。
    auto panel = std::make_unique<Panel>();
    panel->set_id("root-panel");
    auto button = std::make_unique<Button>("确定");
    button->set_id("btn-ok");
    auto input = std::make_unique<Input>();
    input->set_id("name");
    input->set_placeholder("输入名字");
    // 带**组件自定义动作**的元素（`insert`/`undo` 不在通用动作表里）——
    // 用来钉住「协议不得用白名单误拒组件动作」这条契约。
    auto code = std::make_unique<st::ui::CodeEditor>();
    code->set_id("code");
    panel->add_child(std::move(button));
    panel->add_child(std::move(input));
    panel->add_child(std::move(code));
    host.root_.set_content(std::move(panel));
    host.root_.set_viewport({400, 300});
    host.root_.layout();

    server = std::make_unique<st::control::Server>(host);
    st::control::ServerOptions options;   // 默认：自动生成 token（哨兵 "\0"）
    options.port = 0;
    // 显式覆盖：给空串 = 关闭鉴权（与「未指定」区分）。
    if (token_override) options.token = *token_override;
    if (capture_dirs_override.has_value()) options.capture_dirs = capture_dirs_override;
    auto started = server->start(options);
    ST_CHECK(started.has_value());
    if (started) port = *started;
    token = server->token();
    server->poll();
    start_pump();
  }

  /// 后台泵线程：真实应用里 Server::poll 由主循环每帧驱动；测试没有主循环，
  /// 用 5ms 节拍泵代替（与真实时序等价——每次 poll 都是非阻塞轮询）。
  void start_pump() {
    pumping = true;
    pump = std::thread([this] {
      while (pumping) {
        if (server) server->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }

  ~ServerFixture() {
    pumping = false;
    if (pump.joinable()) pump.join();
    if (server) server->stop();
  }

  std::thread pump{};
  std::atomic<bool> pumping{false};
};

/// 显式 token 版 fixture（鉴权路径可预期）。
struct TokenFixture {
  TestHost host{};
  std::unique_ptr<st::control::Server> server{};
  std::uint16_t port{0};
  std::string token{"test-token-1234"};

  TokenFixture() {
    host.root_.set_theme(st::ui::Theme::light());
    // 注意 `assign_ids` 会把根内容自身的 id 写成 "root"（与虚拟根同名），
    // 而 `UiRoot::find` 先查父后查子——同名容器会**遮蔽整棵子树**，
    // 子树里的 id 全部 not_found。面板必须给一个不是 "root" 的显式 id。
    auto panel = std::make_unique<Panel>();
    panel->set_id("root-panel");
    auto button = std::make_unique<Button>("确定");
    button->set_id("btn-ok");
    // 带**组件自定义动作**的元素（`insert`/`undo` 不在通用动作集里）——
    // 用来钉住「协议不得用白名单误拒组件动作」这条契约。
    auto code = std::make_unique<st::ui::CodeEditor>();
    code->set_id("code");
    panel->add_child(std::move(button));
    panel->add_child(std::move(code));
    host.root_.set_content(std::move(panel));
    host.root_.set_viewport({400, 300});
    host.root_.layout();

    server = std::make_unique<st::control::Server>(host);
    st::control::ServerOptions options;
    options.port = 0;
    options.token = token;
    auto started = server->start(options);
    ST_CHECK(started.has_value());
    if (started) port = *started;
    server->poll();
    pumping = true;
    pump = std::thread([this] {
      while (pumping) {
        if (server) server->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }

  ~TokenFixture() {
    pumping = false;
    if (pump.joinable()) pump.join();
    if (server) server->stop();
  }

  std::thread pump{};
  std::atomic<bool> pumping{false};
};

}  // namespace

ST_TEST(auth_wrong_token_is_rejected) {
  TokenFixture fx;
  ST_CHECK(fx.port != 0);
  Probe probe(fx.port, "wrong-token");
  const Json reply = probe.call("hello");
  ST_CHECK(reply.contains("error"));
  if (reply.contains("error")) {
    ST_CHECK(!reply["error"].value("code", std::string()).empty());
  }
}

ST_TEST(auth_correct_token_is_accepted) {
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  const Json reply = probe.call("hello");
  ST_CHECK(reply.value("ok", false));
  if (reply.value("ok", false)) {
    ST_CHECK_EQ(reply["result"]["app"]["name"].get<std::string>(), "protocol-test");
    ST_CHECK_EQ(reply["result"]["screen"]["coordinate_space"].get<std::string>(), "logical");
  }
}

ST_TEST(ui_create_and_remove_elements) {
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_REQUIRE(probe.call("hello").value("ok", false));

  // 建：面板 → 按钮（带属性/key）
  Json root_params = Json::object();
  root_params["type"] = "Panel";
  root_params["id"] = "created-root";
  Json root_reply = probe.call("ui.create", root_params);
  ST_CHECK(root_reply.value("ok", false));
  ST_CHECK_EQ(root_reply["result"]["id"].get<std::string>(), "created-root");

  Json button_params = Json::object();
  button_params["type"] = "Button";
  button_params["parent"] = "created-root";
  button_params["key"] = "go";
  Json button_props = Json::object();
  button_props["label"] = "开始";
  button_params["props"] = button_props;
  Json button_reply = probe.call("ui.create", button_params);
  ST_CHECK(button_reply.value("ok", false));
  const std::string button_id = button_reply["result"]["id"].get<std::string>();

  // 建出来的元素是**真值树的一员**：get/set 照常可用（同一份语义）
  Json got = probe.call("get", Json{{"id", button_id}});
  ST_CHECK(got.value("ok", false));
  ST_CHECK_EQ(got["result"]["props"]["label"].get<std::string>(), "开始");
  Json set_params = Json::object();
  set_params["id"] = button_id;
  Json new_props = Json::object();
  new_props["label"] = "已改";
  set_params["props"] = new_props;
  ST_CHECK(probe.call("set", set_params).value("ok", false));
  Json again = probe.call("get", Json{{"id", button_id}});
  ST_CHECK_EQ(again["result"]["props"]["label"].get<std::string>(), "已改");

  // 未知类型：明确报错（不静默成功）
  Json bad = Json::object();
  bad["type"] = "NoSuchWidget";
  const Json bad_reply = probe.call("ui.create", bad);
  ST_CHECK(bad_reply.contains("error"));

  // 删：元素消失（find 不再命中）
  Json remove_params = Json::object();
  remove_params["id"] = button_id;
  ST_CHECK(probe.call("ui.remove", remove_params).value("ok", false));
  Json find_params = Json::object();
  find_params["selector"] = "Button[label=已改]";
  const Json find_reply = probe.call("find", find_params);
  ST_CHECK_EQ(find_reply["result"]["matches"].size(), 0U);
}

ST_TEST(methods_before_hello_are_rejected) {
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  // 故意不 hello，直接 tree：必须被拒（旧实现任何进程连上即可读全树）
  const Json reply = probe.call("tree");
  ST_CHECK(reply.contains("error"));
}

ST_TEST(auth_disabled_when_token_empty) {
  // 显式空串 = 关闭鉴权（单用户开发机的显式姿态）。用 ServerFixture 是为了拿到它的
  // 泵线程——裸 Server 没有主循环，`poll()` 不跑则 hello 永远不被处理（本测试最初就漏了这个）。
  ServerFixture fx(std::string{});
  Probe probe(fx.port);   // 不带 token
  const Json reply = probe.call("hello");
  ST_CHECK(reply.value("ok", false));
}

ST_TEST(drag_is_dispatched_as_full_sequence) {
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));
  Json params = Json::object();
  params["kind"] = "drag";
  params["x"] = 20.0;
  params["y"] = 20.0;
  params["to_x"] = 80.0;
  params["to_y"] = 40.0;
  const Json reply = probe.call("input.mouse", params);
  ST_CHECK(reply.value("ok", false));
  if (reply.value("ok", false)) {
    // §6.2 声明 drag → 必须成功（旧实现返回「未知鼠标消息: drag」）
    ST_CHECK_EQ(reply["result"]["kind"].get<std::string>(), "drag");
    ST_CHECK(reply["result"].contains("steps"));
    ST_CHECK(reply["result"]["steps"].size() >= 4);   // move+down+move*2+up
  }
}

ST_TEST(wait_frames_condition_is_supported) {
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));
  Json params = Json::object();
  params["for"] = "frames";
  params["frames"] = 3;
  params["timeout_ms"] = 2000;
  // 帧计数由泵线程推进（每 5ms 一次 poll，每次 ++frames_seen）：wait 请求被挂起后，
  // 帧数达标即收到 satisfied 响应——**响应本身**就是条件语义的证据。
  // （不能像最初那样"手动再 poll 三次、再读第二帧"：泵线程已经替我们把帧推完了，
  //  第二次 read_frame 只会等到超时——这类"测试自己构造第二响应"是假绿/假红高发区。）
  const Json reply = probe.call("wait", params);
  ST_CHECK(reply.value("ok", false));
  if (reply.value("ok", false)) {
    ST_CHECK(reply["result"].value("satisfied", false));
  }
}

ST_TEST(invoke_unknown_action_reports_failure_without_rejecting_component_actions) {
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));
  Json params = Json::object();
  params["id"] = "btn-ok";
  params["action"] = "klik";   // 拼错的动作名（真正未知；`activate` 已是 TextArea 合法动作）
  const Json reply = probe.call("invoke", params);
  // 契约（两轮修正后的最终口径）：
  //   ① 不得**假装成功**——`handled=false` 是权威答复（自动化据此判定动作没发生）；
  //   ② 不得用协议层白名单**误拒组件自定义动作**——旧实现就是被这份名单挡住的，
  //      导致 `CodeEditor::undo/redo/set_text` 这类已实现的动作报“未知动作”。
  // 因此：协议层返回 ok=true（请求被正常处理），而 `handled` 如实为 false。
  ST_CHECK(reply.value("ok", false));
  if (reply.value("ok", false)) {
    ST_CHECK(!reply["result"].value("handled", true));
    ST_CHECK_EQ(reply["result"].value("action", std::string()), std::string("klik"));
  }
}

ST_TEST(invoke_reaches_component_custom_actions) {
  // 回归用例：协议层曾用一份 `kActions` 白名单拦动作，`CodeEditor` 的
  // `undo`/`redo`/`set_text`/`goto_line`/`select_all`/`scroll_to_line`… 全部被拦在门外。
  // 症状极其隐蔽：**能力存在却报“未知动作”**——AI 从此以为编辑器不支持这些操作，
  // 只能改用 set `text` 整篇重写（丢掉撤销历史、丢掉光标语义）。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  // nlohmann 的 `operator[] const` 在缺键时直接 assert 崩掉（不是抛异常），
  // 所以测试侧一律先 contains 再取值——否则一个失败响应会把整个测试进程带走。
  const auto handled_of = [](const Json& reply) -> bool {
    if (!reply.contains("result")) return false;
    return reply["result"].value("handled", false);
  };
  const auto invoke = [&probe, &handled_of](const char* id, const char* action,
                                            const char* argument = "") -> bool {
    Json params = Json::object();
    params["id"] = id;
    params["action"] = action;
    if (argument[0] != '\0') params["argument"] = argument;
    const Json reply = probe.call("invoke", params);
    return reply.value("ok", false) && handled_of(reply);
  };
  const auto text_of = [&probe](const char* id) -> std::string {
    Json params = Json::object();
    params["id"] = id;
    const Json reply = probe.call("get", params);
    if (!reply.contains("result") || !reply["result"].contains("props")) return {};
    return reply["result"]["props"].value("text", std::string());
  };

  // `insert` 是 CodeEditor 的动作（不在通用动作集里）：旧实现会报 unsupported
  ST_CHECK(invoke("code", "insert", "hello"));
  ST_CHECK_EQ(text_of("code"), std::string("hello"));

  // `undo` 同样：证明动作真的作用到了组件上
  ST_CHECK(invoke("code", "undo"));
  ST_CHECK_EQ(text_of("code"), std::string());

  // 另一个自定义动作面：select_all + 属性面回读选区
  ST_CHECK(invoke("code", "set_text", "abc"));
  ST_CHECK(invoke("code", "select_all"));
  Json get_params = Json::object();
  get_params["id"] = "code";
  const Json get_reply = probe.call("get", get_params);
  ST_CHECK(get_reply.value("ok", false));
  if (get_reply.contains("result") && get_reply["result"].contains("props")) {
    ST_CHECK_EQ(get_reply["result"]["props"].value("selection", std::string()), std::string("0:3"));
  }

  // 而真正未实现的动作仍然如实报 handled=false（不假装成功）
  ST_CHECK(!invoke("code", "teleport"));
}

/// **显式空白名单 = 禁止一切落盘**（而不是"未设置→用宽松默认"）。
///
/// 这条守的是"缺省值即歧义值"型缺陷的一个真实实例（2026-10-14，与 `token` 同型）：
///
/// ```cpp
/// // 旧写法（server.cpp）
/// impl_->capture_dirs = options.capture_dirs.empty() ? options.default_capture_dirs()
///                                                   : options.capture_dirs;
/// ```
///
/// 而 `control.hpp` 的注释承诺的是「**空向量 = 禁止一切落盘**（仍可 base64 回传）」——
/// 于是"想收紧到禁止落盘"会被静默扩成默认白名单（temp + exe 目录 + 控制文件目录）。
/// 这是**安全语义的反向**。
///
/// 同文件上方的 `token` 早有同名问题（`std::string token{"\0"}` 坍缩成空串，
/// "显式空串关闭鉴权"与"未设置"不可分）并已改成 `optional`——
/// **同一个错写法在同一文件里只改了一半**。这条用例把另一半钉住。
///
/// 反向验证：把 `server.cpp` 的判据改回 `empty() ? 默认 : 显式`，本用例立刻变红。
ST_TEST(capture_explicit_empty_whitelist_forbids_all_writes) {
  // 显式空白名单 = 禁止落盘。用既有 fixture（它自带 poll 泵）。
  ServerFixture fx{std::nullopt, std::vector<std::string>{}};
  Probe probe(fx.port, fx.token);
  ST_REQUIRE(probe.call("hello").value("ok", false));

  // 连"落在默认白名单里"的路径也必须被拒——那正是旧写法会放行的东西。
  const auto dir = st::fs::make_temp_dir("st-capture-deny");
  ST_REQUIRE(dir.has_value());
  Json params = Json::object();
  params["encode"] = "file";
  params["path"] = st::fs::join(*dir, "shot.png");
  const Json reply = probe.call("capture", params);
  ST_CHECK(!reply.value("ok", false));
  (void)st::fs::remove_all(*dir);
}

ST_TEST(capture_path_outside_whitelist_is_rejected) {
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));
  Json params = Json::object();
  params["encode"] = "file";
  params["path"] = "C:/Windows/system32/evil.png";   // 白名单外
  const Json reply = probe.call("capture", params);
  ST_CHECK(!reply.value("ok", false));
  if (reply.contains("error")) {
    ST_CHECK_EQ(reply["error"].value("code", std::string()), "invalid");
  }
}

/// 裸文件名控制文件（无目录）的落盘白名单（BACKLOG P2 2026-10-15）：
///
/// `default_capture_dirs` 对无分隔符 control_file 曾放行字面 "." ——而客户端请求的
/// path 是绝对路径，前缀比较永远不匹配，该目录下全部拒绝；智能体会话里 ctl.json
/// 常直接写在 CWD，视觉断言基线写入会莫名被拒，报错文案也不指向这个原因。
///
/// 契约（三分支）：
///   ① 裸文件名 control_file → CWD 绝对路径下的请求放行；
///   ② 白名单外的目录仍然拒绝（安全语义不因容错而松动）；
///   ③ 带目录的 control_file（绝对路径）行为不变。
///
/// 逆向验证：把裸文件名分支回退成 `dirs.push_back(".")` 后，用例①变红（拒绝），
/// 用例②③不受影响；恢复后回绿。实测 2026-10-15。
ST_TEST(capture_bare_control_file_allows_cwd_and_rejects_outside) {
  // 本用例不能被 TokenFixture 复用：它不设 control_file，测不到裸文件名分支。
  // 自建 fixture：control_file 为裸文件名，其他选项与 ServerFixture 一致。
  TestHost host{};
  host.root_.set_theme(st::ui::Theme::light());
  auto panel = std::make_unique<Panel>();
  panel->set_id("root-panel");
  auto button = std::make_unique<Button>("确定");
  button->set_id("btn-ok");
  panel->add_child(std::move(button));
  host.root_.set_content(std::move(panel));
  host.root_.set_viewport({400, 300});
  host.root_.layout();

  const auto cwd_result = st::fs::current_dir();
  ST_REQUIRE(cwd_result.has_value());
  const std::string control_file = "ctl-bare-cwd-test.json";   // 裸文件名：无目录分隔符

  st::control::Server server(host);
  st::control::ServerOptions options;
  options.port = 0;
  options.control_file = control_file;
  auto started = server.start(options);
  ST_REQUIRE(started.has_value());
  const std::uint16_t port = *started;
  const std::string token = server.token();
  std::atomic<bool> pumping{false};
  std::thread pump([&server, &pumping] {
    while (pumping) {
      server.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  pumping = true;

  Probe probe(port, token);
  ST_REQUIRE(probe.call("hello").value("ok", false));

  const auto cleanup = [&] {
    pumping = false;
    if (pump.joinable()) pump.join();
    server.stop();
  };

  // ① CWD 绝对路径下的基线写入：放行（修复前：字面 "." 前缀比较不匹配，被拒）。
  {
    const std::string baseline = st::fs::join(*cwd_result, "st-bare-ctl-baseline.png");
    Json write = Json::object();
    write["path"] = baseline;
    write["write_baseline"] = true;
    const Json written = probe.call("visual.diff", write);
    ST_CHECK(written.value("ok", false));
    ST_CHECK(st::fs::exists(baseline));
    (void)st::fs::remove_all(baseline);
  }

  // capture 显式 path 同口径：CWD 下放行（另一条白名单拦的通道）。
  {
    Json params = Json::object();
    params["encode"] = "file";
    params["path"] = st::fs::join(*cwd_result, "st-bare-ctl-shot.png");
    const Json reply = probe.call("capture", params);
    ST_CHECK(reply.value("ok", false));
  }

  // ② 白名单外的目录：仍然拒绝（安全语义不能松动）。
  {
    Json params = Json::object();
    params["encode"] = "file";
    params["path"] = "C:/Windows/system32/evil.png";
    const Json reply = probe.call("capture", params);
    ST_CHECK(!reply.value("ok", false));
    if (reply.contains("error")) {
      ST_CHECK_EQ(reply["error"].value("code", std::string()), "invalid");
    }
  }

  // ③ 带目录的 control_file：行为不变（目录条目照旧、不在白名单的目录照拒）。
  {
    const auto dir = st::fs::make_temp_dir("st-bare-ctl-dir");
    ST_REQUIRE(dir.has_value());
    TestHost host2{};
    host2.root_.set_theme(st::ui::Theme::light());
    auto panel2 = std::make_unique<Panel>();
    panel2->set_id("root-panel");
    panel2->add_child(std::make_unique<Button>("确定"));
    host2.root_.set_content(std::move(panel2));
    host2.root_.set_viewport({400, 300});
    host2.root_.layout();

    st::control::Server server2(host2);
    st::control::ServerOptions options2;
    options2.port = 0;
    options2.control_file = st::fs::join(*dir, "ctl.json");
    auto started2 = server2.start(options2);
    ST_REQUIRE(started2.has_value());
    const std::uint16_t port2 = *started2;
    const std::string token2 = server2.token();
    std::atomic<bool> pumping2{false};
    std::thread pump2([&server2, &pumping2] {
      while (pumping2) {
        server2.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    pumping2 = true;

    Probe probe2(port2, token2);
    ST_REQUIRE(probe2.call("hello").value("ok", false));

    // ③a 该目录下的基线写入：照旧放行（既有行为）。
    {
      const std::string baseline = st::fs::join(*dir, "baseline.png");
      Json write = Json::object();
      write["path"] = baseline;
      write["write_baseline"] = true;
      const Json written = probe2.call("visual.diff", write);
      ST_CHECK(written.value("ok", false));
    }
    // ③b 裸文件名场景下才放的 CWD：在带目录场景中**不在白名单**，必须拒
    // （不同控制文件形态的白名单互不串味——这是「行为不变」的负向钉子）。
    {
      Json write = Json::object();
      write["path"] = st::fs::join(*cwd_result, "st-bare-ctl-stray.png");
      write["write_baseline"] = true;
      const Json reply = probe2.call("visual.diff", write);
      ST_CHECK(!reply.value("ok", false));
    }

    pumping2 = false;
    if (pump2.joinable()) pump2.join();
    server2.stop();
    (void)st::fs::remove_all(*dir);
  }

  cleanup();
}

/// 区域参数：**半形参数一律报错，不静默当全屏**。
///
/// 回归（2026-10-06）：这里曾经只认 `region` 对象，于是调用方发扁平的
/// `x`/`y`/`width`/`height` 时**静默截了全屏**——调用方拿到 "ok" 却是一张
/// 完全不对的图。同样，`region` 宽高为 0/负也被静默当全屏。
///
/// 为何静默比报错危险：两个后续动作（量尺寸、像素断言）都基于"这图是那个区域"——
/// 错了不会崩，只会让结论静默失真。
ST_TEST(capture_region_rejects_half_formed_params) {
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  // 扁平参数（没用 region 包起来）→ 报 invalid，且错误信息要指名怎么改。
  //
  // ⚠ 断言顺序：**先 `ST_CHECK(!ok)` 再读 `error`**。反过来写会踩一个尴尬的坑：
  // 回退实现后（不报错）响应里根本没有 `error` 字段，而 nlohmann 的
  // `operator[] const` 缺键是 **UB（断言崩溃）**——测试会“没优雅地抓住”，
  // 逆向验证脚本判它失败，而真正的问题是断言写法不稳。
  {
    Json flat = Json::object();
    flat["x"] = 10; flat["y"] = 20; flat["width"] = 30; flat["height"] = 40;
    const Json reply = probe.call("capture", flat);
    ST_CHECK(!reply.value("ok", false));
    if (reply.contains("error")) {
      ST_CHECK_EQ(reply["error"].value("code", std::string()), "invalid");
      ST_CHECK(reply["error"].value("message", std::string()).find("region") != std::string::npos);
    }
  }
  // 只给一个扁平字段也是同样的错（半个参数比全不给危险）。
  {
    Json half = Json::object();
    half["width"] = 30;
    ST_CHECK(!probe.call("capture", half).value("ok", false));
  }
  // `region` 宽/高 ≤ 0 → 报 invalid（不能当成全屏）。
  for (const auto width : {0, -3}) {
    Json region = Json::object();
    region["x"] = 0; region["y"] = 0; region["width"] = width; region["height"] = 8;
    Json params = Json::object();
    params["region"] = region;
    const Json reply = probe.call("capture", params);
    ST_CHECK(!reply.value("ok", false));
    if (reply.contains("error")) {
      ST_CHECK_EQ(reply["error"].value("code", std::string()), "invalid");
    }
  }
  // `region` 不是对象 → 报 invalid。
  {
    Json params = Json::object();
    params["region"] = "0,0,8,8";
    ST_CHECK(!probe.call("capture", params).value("ok", false));
  }
  // 不给区域 = 全屏，仍然合法（全屏是唯一的空区域语义）。
  ST_CHECK(probe.call("capture.hash").value("ok", false));
}

/// `capture` 回包的 `pixel_size` 必须是**实际导出**尺寸，而不是拿 `region` 算的。
///
/// 回归（2026-10-06）：`pixel_size` 曾由 `region × scale` 算出，而 `region` 会被画布
/// 夹取——请求 200×200 落在右下角时，**文件实际 80×50，响应却写 200×200**。
/// 那让"按响应校验尺寸"彻底失效，而调用方正是靠它确认"拿到的是哪一块"。
///
/// 桩里 `capture_png`/`capture_to_file` 的尺寸随 `region` 线性变化（测试桩刻意不含夹取），
/// 所以这里锁的是**接线**：尺寸必须来自 Host 的返回，不能自己算。
ST_TEST(capture_pixel_size_comes_from_the_export_not_the_request) {
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  // ⚠ `reply["result"]` 在 **const Json** 上取缺键是 nlohmann 的 UB（它会直接
  // `Assertion failed` 杀掉进程，而不是报个测试失败）。成功路径上该键必在，
  // 但一旦前面某步返回了错误帧，这里就会把“一条用例红了”变成“整个测试进程没了”——
  // 全量跑时后面所有用例都不再执行，且崩溃点看起来完全与真正原因无关。
  // 因此取值一律走 `find_result`：取不到就**报断言失败**并返回空。
  const auto find_result = [](const Json& reply) -> const Json* {
    const auto it = reply.find("result");
    return it != reply.end() && it->is_object() ? &(*it) : nullptr;
  };
  auto size_of = [&](const Json& reply) -> std::pair<std::int64_t, std::int64_t> {
    const Json* result = find_result(reply);
    ST_CHECK(result != nullptr);
    if (result == nullptr) return {0, 0};
    const auto it = result->find("pixel_size");
    ST_CHECK(it != result->end());
    if (it == result->end()) return {0, 0};
    return std::pair<std::int64_t, std::int64_t>{it->value("width", std::int64_t{0}),
                                                 it->value("height", std::int64_t{0})};
  };
  // ⚠ file 分支的落盘路径**必须落在白名单内**（默认白名单 = 临时目录 + 可执行文件目录
  // + 控制文件目录）。硬编码 `/tmp/...` 在 Linux 上能过、在 Windows 上不是白名单目录，
  // `capture` 会如实例地返回错误帧——于是旧写法的 `reply["result"]` 当场把进程断言掉。
  // 这正是全量 `st test` “跑到 capture 用例就崩”的真因（而单跑因为不看后续分支一直没暴露）。
  const auto temp_dir_result = st::fs::make_temp_dir("st-capture-size");
  ST_REQUIRE(temp_dir_result.has_value());
  const std::string temp_dir = *temp_dir_result;
  // base64 分支：请求 37×40 完全在帧内（桩帧 64×48）→ 实际就是 37×40。
  {
    Json region = Json::object();
    region["x"] = 0; region["y"] = 0; region["width"] = 37; region["height"] = 40;
    Json params = Json::object();
    params["region"] = region;
    const Json reply = probe.call("capture", params);
    ST_CHECK(reply.value("ok", false));
    // 桩回的是 region 尺寸；若实现改用 viewport×scale（旧写法），这里会是 64×48。
    ST_CHECK_EQ(size_of(reply).first, 37);
    ST_CHECK_EQ(size_of(reply).second, 40);
  }
  // **关键用例：请求超出帧缓冲**（帧 64×48，从 (50,40) 请求 30×30）。
  // 实际只有 14×8，而 `region` 写的是 30×30——这两者必须报后者（实际值），
  // 报前者就是谎报，也是旧实现的真实行为（`region × scale`）。
  {
    Json region = Json::object();
    region["x"] = 50; region["y"] = 40; region["width"] = 30; region["height"] = 30;
    Json params = Json::object();
    params["region"] = region;
    const Json reply = probe.call("capture", params);
    ST_CHECK(reply.value("ok", false));
    ST_CHECK_EQ(size_of(reply).first, 14);
    ST_CHECK_EQ(size_of(reply).second, 8);
    // 响应的 `region` 仍是**请求值**（调用方靠它区分"我要的"与"得到的"）。
    ST_CHECK_EQ(reply["result"]["region"].value("width", 0.0), 30.0);
  }
  // 起点为负：从 (-10,-10) 请求 20×20 → 实际只有帧内那 10×10。
  {
    Json region = Json::object();
    region["x"] = -10; region["y"] = -10; region["width"] = 20; region["height"] = 20;
    Json params = Json::object();
    params["region"] = region;
    const Json reply = probe.call("capture", params);
    ST_CHECK(reply.value("ok", false));
    ST_CHECK_EQ(size_of(reply).first, 10);
    ST_CHECK_EQ(size_of(reply).second, 10);
  }
  // file 分支也必须带上尺寸（旧实现在这条路径上根本没有任何尺寸信息）。
  {
    Json region = Json::object();
    region["x"] = 0; region["y"] = 0; region["width"] = 21; region["height"] = 11;
    Json params = Json::object();
    params["region"] = region;
    params["encode"] = "file";
    params["path"] = st::fs::join(temp_dir, "capture-size-probe.png");
    const Json reply = probe.call("capture", params);
    ST_CHECK(reply.value("ok", false));
    ST_CHECK_EQ(size_of(reply).first, 21);
    ST_CHECK_EQ(size_of(reply).second, 11);
  }
  // file 分支的夹取同样不能谎报。
  {
    Json region = Json::object();
    region["x"] = 60; region["y"] = 44; region["width"] = 20; region["height"] = 20;
    Json params = Json::object();
    params["region"] = region;
    params["encode"] = "file";
    params["path"] = st::fs::join(temp_dir, "capture-size-probe2.png");
    const Json reply = probe.call("capture", params);
    ST_CHECK(reply.value("ok", false));
    ST_CHECK_EQ(size_of(reply).first, 4);
    ST_CHECK_EQ(size_of(reply).second, 4);
  }
  // 全屏：桩的全屏约定是 64×48（也验证了"尺寸来自导出"而非写死）。
  {
    const Json reply = probe.call("capture");
    ST_CHECK(reply.value("ok", false));
    ST_CHECK_EQ(size_of(reply).first, 64);
    ST_CHECK_EQ(size_of(reply).second, 48);
  }
}

ST_TEST(capture_hash_is_stable_and_region_sensitive) {
  // 像素哈希：同区域两次调用一致；同方法换区域/换内容即变——这是"画面变了没有"的快速判定。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  // 默认区域（全屏合成帧）两次调一致
  const Json first = probe.call("capture.hash");
  ST_CHECK(first.value("ok", false));
  const Json second = probe.call("capture.hash");
  ST_CHECK(second.value("ok", false));
  ST_CHECK_EQ(first["result"]["hash"].get<std::string>(),
              second["result"]["hash"].get<std::string>());
  ST_CHECK_EQ(first["result"]["algorithm"].get<std::string>(), "fnv1a64");
  ST_CHECK(first["result"]["bytes"].get<std::int64_t>() > 0);

  // 换区域 → 哈希不同（合成像素是 x/y 渐变，任意不同区域内容都不同）
  Json region = Json::object();
  region["x"] = 0; region["y"] = 0; region["width"] = 8; region["height"] = 8;
  Json params = Json::object();
  params["region"] = region;
  const Json small = probe.call("capture.hash", params);
  ST_CHECK(small.value("ok", false));
  ST_CHECK(small["result"]["hash"].get<std::string>() !=
            first["result"]["hash"].get<std::string>());
  ST_CHECK_EQ(small["result"]["width"].get<std::int64_t>(), 8);
  ST_CHECK_EQ(small["result"]["height"].get<std::int64_t>(), 8);

  // 缺元素 → 明确 not_found（与 capture 同口径）
  Json missing = Json::object();
  missing["id"] = "#no-such-element";
  const Json missing_reply = probe.call("capture.hash", missing);
  ST_CHECK(!missing_reply.value("ok", false));
  if (missing_reply.contains("error")) {
    ST_CHECK_EQ(missing_reply["error"].value("code", std::string()), "not_found");
  }
}

ST_TEST(visual_diff_baseline_write_and_compare) {
  // 基线写盘 → 同帧比对（changed=false、diff_pixels=0）→ 人为改基线图→ 检测到差异。
  //
  // 为什么用测试自己造基线：真基线图属版本数据，不应让协议测试依赖工作区文件；
  // 用合成图可把"比对逻辑本身"钉住（写盘/读回/逐像素差/阈值/尺寸不符）。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  const auto dir_result = st::fs::make_temp_dir("st-visual-diff");
  ST_REQUIRE(dir_result.has_value());
  const std::string dir = *dir_result;
  const std::string baseline = st::fs::join(dir, "baseline.png");

  // ① 写基线
  Json write = Json::object();
  write["path"] = baseline;
  write["write_baseline"] = true;
  const Json written = probe.call("visual.diff", write);
  ST_CHECK(written.value("ok", false));
  ST_CHECK(written["result"].value("written", false));
  ST_CHECK(st::fs::exists(baseline));

  // ② 同帧自比：无差异
  Json compare = Json::object();
  compare["path"] = baseline;
  const Json same = probe.call("visual.diff", compare);
  ST_CHECK(same.value("ok", false));
  ST_CHECK(!same["result"].value("changed", true));
  ST_CHECK_EQ(same["result"]["diff_pixels"].get<std::int64_t>(), 0);
  ST_CHECK_EQ(same["result"]["max_diff"].get<std::int64_t>(), 0);

  // ③ 修改磁盘基线（改一个像素的蓝色通道）→ 比对检测到差异
  {
    auto image = st::codec::png_read_file(baseline);
    ST_REQUIRE(image.has_value());
    image->rgba[0] = static_cast<std::uint8_t>(image->rgba[0] ^ 0x80U);
    auto status = st::codec::png_write_file(baseline, *image);
    ST_CHECK(status.has_value());
  }
  const Json different = probe.call("visual.diff", compare);
  ST_CHECK(different.value("ok", false));
  ST_CHECK(different["result"].value("changed", false));
  ST_CHECK_EQ(different["result"]["diff_pixels"].get<std::int64_t>(), 1);
  ST_CHECK_EQ(different["result"]["max_diff"].get<std::int64_t>(), 128);
  ST_CHECK(different["result"]["diff_ratio"].get<double>() > 0.0);
  ST_CHECK(different["result"].contains("diff_bounds"));

  // ④ 基线不存在且不写 → not_found（不静默把当前帧当基线）
  Json fresh = Json::object();
  fresh["path"] = st::fs::join(dir, "absent.png");
  const Json absent = probe.call("visual.diff", fresh);
  ST_CHECK(!absent.value("ok", false));
  if (absent.contains("error")) {
    ST_CHECK_EQ(absent["error"].value("code", std::string()), "not_found");
  }

  (void)st::fs::remove_all(dir);
}

ST_TEST(visual_diff_rejects_size_mismatch) {
  // 基线尺寸与当前截图不一致 → 明确报错（不做缩放对齐：缩放本身会引入伪差异）。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  const auto dir_result = st::fs::make_temp_dir("st-visual-mismatch");
  ST_REQUIRE(dir_result.has_value());
  const std::string dir = *dir_result;
  const std::string baseline = st::fs::join(dir, "wrong-size.png");
  {
    st::codec::PngImage image;
    image.width = 5;
    image.height = 7;
    image.rgba.assign(5U * 7U * 4U, 0x20U);
    auto status = st::codec::png_write_file(baseline, image);
    ST_CHECK(status.has_value());
  }
  Json compare = Json::object();
  compare["path"] = baseline;
  const Json reply = probe.call("visual.diff", compare);
  ST_CHECK(!reply.value("ok", false));
  if (reply.contains("error")) {
    ST_CHECK_EQ(reply["error"].value("code", std::string()), "invalid");
  }
  (void)st::fs::remove_all(dir);
}

ST_TEST(key_press_sends_down_and_up) {
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));
  Json params = Json::object();
  params["kind"] = "press";
  params["key"] = "a";
  const Json reply = probe.call("input.key", params);
  ST_CHECK(reply.value("ok", false));
  if (reply.value("ok", false)) {
    ST_CHECK_EQ(reply["result"]["kind"].get<std::string>(), "press");
  }
}

ST_TEST(send_failure_drops_client_without_killing_others) {
  // 坏连接（读一半就关）不拖垮服务器：另一条好连接仍可正常调用。
  TokenFixture fx;
  {
    // 连上就走（不 hello）——制造一个只耗资源的短命连接
    auto zombie = st::net::connect_tcp("127.0.0.1", fx.port, 1000);
    ST_CHECK(zombie.has_value());
  }
  fx.server->poll();
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));
  const Json tree = probe.call("tree");
  ST_CHECK(tree.value("ok", false));
}

ST_TEST(protocol_table_smoke_every_documented_method) {
  // §6.2 方法表逐项 smoke：每个文档方法至少能拿到结构化响应（ok 或明确 error，
  // 不允许连接中断/挂死）。这是「文档↔实现」一致性的底线护栏。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  struct Case {
    std::string method;
    Json params;
  };
  std::vector<Case> cases;
  auto add = [&](const std::string& method, Json params) { cases.push_back({method, params}); };
  add("ping", Json::object());
  {
    Json p = Json::object(); p["depth"] = 2; add("tree", p);
  }
  {
    Json p = Json::object(); p["selector"] = "Button"; add("find", p);
  }
  {
    Json p = Json::object(); p["id"] = "btn-ok"; add("get", p);
  }
  {
    Json p = Json::object(); p["id"] = "btn-ok";
    Json props = Json::object(); props["label"] = "好"; p["props"] = props;
    add("set", p);
  }
  {
    Json p = Json::object(); p["id"] = "btn-ok"; p["action"] = "focus"; add("invoke", p);
  }
  {
    Json p = Json::object(); p["kind"] = "move"; p["x"] = 10.0; p["y"] = 10.0;
    add("input.mouse", p);
  }
  {
    Json p = Json::object(); p["kind"] = "press"; p["key"] = "tab"; add("input.key", p);
  }
  {
    Json p = Json::object(); p["text"] = "hi"; add("input.text", p);
  }
  add("capture", Json::object());
  add("capture.hash", Json::object());
  add("visual", Json::object());
  {
    Json p = Json::object(); p["path"] = "<nonexistent-baseline>"; add("visual.diff", p);
  }
  {
    Json p = Json::object(); p["for"] = "element"; p["selector"] = "Button";
    p["timeout_ms"] = 500; add("wait", p);
  }
  add("metrics", Json::object());
  {
    Json p = Json::object(); p["enable"] = false; add("events", p);
  }
  {
    Json p = Json::object(); p["mode"] = "dark"; add("theme", p);
  }
  {
    Json p = Json::object(); p["action"] = "repaint"; add("app", p);
  }
  for (const auto& item : cases) {
    const Json reply = probe.call(item.method, item.params);
    // 要么 ok=true 要么带 error 结构——不能两者皆无（协议破损）
    const bool well_formed = reply.value("ok", false) || reply.contains("error");
    ST_CHECK(well_formed);
  }
}

ST_TEST(ui_changed_event_carries_changed_ids) {
  // 事件流完整消费（BACKLOG）：订阅 `ui.changed` 后，`set` 一个元素应收到
  // 带**变更 id 清单**的事件（而不只是版本号）——客户端据此知道"哪些元素变了"，
  // 不必整树重拉。
  //
  // 关键口径：清单只报"状态/内容变了"的元素；纯悬浮过渡与光标闪烁不进清单
  // （否则逐帧刷屏，「变更清单」就失去了信噪比）。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);

  Json hello_params = Json::object();
  hello_params["subscribe"] = true;
  Json kinds = Json::array();
  kinds.push_back("ui.changed");
  hello_params["kinds"] = std::move(kinds);
  const Json hello = probe.call("hello", hello_params);
  ST_CHECK(hello.value("ok", false));

  // 改一个元素 → 等带 changed 清单的事件
  Json set_params = Json::object();
  set_params["id"] = "btn-ok";
  Json props = Json::object();
  props["label"] = "已改";
  set_params["props"] = std::move(props);
  const Json set_reply = probe.call("set", set_params);
  ST_CHECK(set_reply.value("ok", false));

  // 最多等 2 秒；期间可能先收到无 changed 的事件（其它版本变化）——只认带清单的
  bool saw_changed = false;
  for (int attempt = 0; attempt < 40 && !saw_changed; ++attempt) {
    const auto frame = probe.read_frame(500);
    if (!frame.has_value()) continue;
    if (!frame->contains("event")) continue;
    if ((*frame)["event"].get<std::string>() != "ui.changed") continue;
    const Json& data = st::json_at(*frame, "data");
    if (!data.contains("changed")) continue;
    const Json& changed = data["changed"];
    ST_CHECK(changed.is_array());
    bool has_button = false;
    for (const auto& item : changed) {
      if (item.is_string() && item.get<std::string>() == "btn-ok") has_button = true;
    }
    ST_CHECK(has_button);
    ST_CHECK(data.contains("version"));
    saw_changed = true;
  }
  ST_CHECK(saw_changed);
}

// ————————————————————————————————————————————————————————————————————————————
// 开发效率原语：按 id 输入 / invoke 回带状态 / 属性等待 / 列表视口属性
// ———————————————————————————————————————————————————————————————————————————

ST_TEST(input_mouse_by_id_uses_element_center) {
  // 回归用例：AI 的意图是“点这个元素”，而不是“点在 (x,y)”。
  // 旧路径必须先 `find` 取 `bounds` 再自己算中心——而 `find` 连**不可见**元素也会返回，
  // 本会话实测因此点空两次（“点了没反应”）。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  Json find_params = Json::object();
  find_params["selector"] = "#btn-ok";
  const Json found = probe.call("find", find_params);
  ST_REQUIRE(found.value("ok", false));
  ST_REQUIRE(found["result"]["matches"].is_array() && !found["result"]["matches"].empty());
  const Json& box = found["result"]["matches"][0]["bounds"];
  const double cx = box["x"].get<double>() + box["width"].get<double>() * 0.5;
  const double cy = box["y"].get<double>() + box["height"].get<double>() * 0.5;

  Json click = Json::object();
  click["kind"] = "click";
  click["id"] = "btn-ok";
  const Json reply = probe.call("input.mouse", click);
  ST_CHECK(reply.value("ok", false));
  // 服务端自己算的中心点应与我们算的一致
  if (reply.contains("result") && reply["result"].contains("position")) {
    const Json& at = reply["result"]["position"];
    ST_CHECK_NEAR(at["x"].get<double>(), cx, 1.0);
    ST_CHECK_NEAR(at["y"].get<double>(), cy, 1.0);
  }

  // 不可见元素必须**明确拒绝**，而不是点到别处
  Json hidden = Json::object();
  hidden["kind"] = "click";
  hidden["id"] = "no-such-element";
  const Json missing = probe.call("input.mouse", hidden);
  ST_CHECK(!missing.value("ok", false));
}

ST_TEST(invoke_returns_post_action_state) {
  // 回归用例：`invoke` 之后几乎总要看一眼“变成什么了”。不带状态就得再发一次 `get`
  // ——每个动作多一次往返。这里断言 `state` 与随后 `get` 的结果一致（同一个属性面）。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  Json invoke_params = Json::object();
  invoke_params["id"] = "code";
  invoke_params["action"] = "set_text";
  invoke_params["argument"] = "hello world";
  const Json reply = probe.call("invoke", invoke_params);
  ST_REQUIRE(reply.value("ok", false));
  const Json& result = reply["result"];
  ST_CHECK(result.value("handled", false));
  ST_REQUIRE(result.contains("state"));
  ST_CHECK_EQ(result["state"].value("text", std::string()), std::string("hello world"));
  ST_CHECK_EQ(result["state"].value("lines", std::string()), std::string("1"));

  // 与 `get` 的属性面对齐（同一份 `property_names`，不另立白名单）
  Json get_params = Json::object();
  get_params["id"] = "code";
  const Json got = probe.call("get", get_params);
  ST_REQUIRE(got.value("ok", false));
  ST_CHECK_EQ(got["result"]["props"].value("text", std::string()), std::string("hello world"));
}

ST_TEST(wait_property_condition_becomes_true) {
  // 新增等待条件：等**某元素的某属性等于某值**——真实开发里绝大多数等待是这个形状
  // （“文字变成 X”“列表有 N 项”）。旧的 `text` 条件只能匹配文本子串，且要拼整棵语义树。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  Json wait_params = Json::object();
  wait_params["for"] = "property";
  wait_params["selector"] = "#code";
  wait_params["property"] = "text";
  wait_params["value"] = "ready";
  wait_params["timeout_ms"] = 1500;

  // 后台改属性：**必须另开一条连接**。
  // 挂起的 `wait` 会把该连接上后续的请求**一起押着**（它们不构成“新条件”），
  // 在同一个连接里“挂起等待 + 随后写入”必然死等到超时——实测踩到，花了 6 秒才发现。
  // （协议层面这是有意的背压：等待期间不排队新请求，见 `Server::dispatch_frame`
  //   的 busy-connection 检查；本用例正是它的行为注解。）
  Probe writer(fx.port, fx.token);
  ST_CHECK(writer.call("hello").value("ok", false));
  std::thread writer_thread([&writer]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Json params = Json::object();
    params["id"] = "code";
    params["action"] = "set_text";
    params["argument"] = "ready";
    (void)writer.call("invoke", params);
  });
  const Json reply = probe.call("wait", wait_params);
  writer_thread.join();
  ST_CHECK(reply.value("ok", false));
  ST_CHECK(reply["result"].value("satisfied", false));

  // 条件永远不成立时必须**超时返回 false**，而不是假装满足
  Json never = wait_params;
  never["value"] = "never-happens";
  never["timeout_ms"] = 200;
  const Json missed = probe.call("wait", never);
  ST_CHECK(missed.value("ok", false));
  ST_CHECK(!missed["result"].value("satisfied", true));

  // 元素没声明这个属性 → 如实不满足（不静默当成满足）
  Json unknown = wait_params;
  unknown["property"] = "no_such_property";
  unknown["timeout_ms"] = 150;
  const Json bogus = probe.call("wait", unknown);
  ST_CHECK(bogus.value("ok", false));
  ST_CHECK(!bogus["result"].value("satisfied", true));
}

ST_TEST(list_components_expose_viewport_properties) {
  // 列表类组件的**通用视口契约**：`get_property` 之前在这些组件上是完全缺失的
  // （`Tree`/`List`/`ScrollView` 计数为 0），而“用户看得到哪一块”恰是 AI 最常问的。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  const auto props_of = [&probe](const char* id) {
    Json params = Json::object();
    params["id"] = id;
    const Json reply = probe.call("get", params);
    // 不用 ST_REQUIRE：它在 lambda 里会展开成 `return;`（返回类型不符）
    if (!reply.value("ok", false)) return Json::object();
    return reply["result"]["props"];
  };
  // `CodeEditor` 是列表类里最先补齐的：视口四件套 + 滚动
  const Json editor = props_of("code");
  ST_CHECK(editor.contains("first_visible_line"));
  ST_CHECK(editor.contains("visible_lines"));
  ST_CHECK(editor.contains("scroll"));
  ST_CHECK(editor.contains("lines"));
  ST_CHECK(editor.contains("indent_guides"));
  ST_CHECK(editor.contains("auto_pairs"));
}

ST_TEST(wait_connection_is_reusable_after_satisfied) {
  // 回归用例（自伤）：给“挂起 wait 的连接”加背压门时，**忘了在等待结束时清标志**——
  // 于是这条连接以后每次调用都被拒（实测：等完之后连 `get` 都失败）。
  // 门必须只覆盖“正在等待”那一段。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_CHECK(probe.call("hello").value("ok", false));

  Json params = Json::object();
  params["id"] = "code";
  params["action"] = "set_text";
  params["argument"] = "seed";
  ST_CHECK(probe.call("invoke", params).value("ok", false));

  // 立即满足的 wait（属性已等于目标）→ 走同步返回分支
  Json wait_params = Json::object();
  wait_params["for"] = "property";
  wait_params["selector"] = "#code";
  wait_params["property"] = "text";
  wait_params["value"] = "seed";
  wait_params["timeout_ms"] = 500;
  ST_CHECK(probe.call("wait", wait_params).value("ok", false));

  // 等待结束后，同一条连接必须能继续干活
  Json get_params = Json::object();
  get_params["id"] = "code";
  const Json after = probe.call("get", get_params);
  ST_CHECK(after.value("ok", false));
  ST_CHECK_EQ(after["result"]["props"].value("text", std::string()), std::string("seed"));

  // 超时结束的 wait 同样不能把连接锁死
  Json miss = wait_params;
  miss["value"] = "never";
  miss["timeout_ms"] = 120;
  ST_CHECK(probe.call("wait", miss).value("ok", false));
  ST_CHECK(probe.call("get", get_params).value("ok", false));

  // 等待期间（挂起中）复用同一连接 → **当场拒绝**，而不是静默挂起
  Probe writer(fx.port, fx.token);
  ST_CHECK(writer.call("hello").value("ok", false));
  Json change = Json::object();
  change["id"] = "code";
  change["action"] = "set_text";
  change["argument"] = "changed";
  std::thread writer_thread([&writer, &change]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    (void)writer.call("invoke", change);
  });
  Json slow = wait_params;
  slow["value"] = "changed";
  slow["timeout_ms"] = 3000;
  const Json waited = probe.call("wait", slow, 5000);
  writer_thread.join();
  ST_CHECK(waited.value("ok", false));
  ST_CHECK(waited["result"].value("satisfied", false));
  ST_CHECK(probe.call("get", get_params).value("ok", false));
}

// ————————————————————————————————————————————————————————————————————————————
// 能力清单不得与实现漂移（结构评审：重复真源）
// ————————————————————————————————————————————————————————————————————————————

/// `hello` 返回的 `capabilities` 是**手写的第二份「有哪些方法」清单**，
/// 与 `handle` 里的 `if (method == "...")` 分派链是两套真源。
///
/// 风险是**静默漂移**：新增一个方法却忘了加进 `capabilities`（客户端据此判断能否调用，
/// 于是新方法永远没人用），或删掉一个方法却留着它（客户端调了才发现 Unsupported）。
/// 两种都不会被编译器发现，也不会让任何既有用例变红。
///
/// 这条用例遍历 `capabilities` 里的每个名字**真的发一次请求**，并断言
/// 「不是 `未知方法` 错误」——即声明的能力必须真的被实现。
ST_TEST(find_reports_total_beyond_limit) {
  // 契约（DESIGN §6.2 `find` 行）：`count` = 本次返回条数（受 `limit` 限制），
  // `total` = 命中总数（不受 `limit` 影响）。
  //
  // 为何要单独守这条：旧结果里只有 `count`，而它等于 `matches.size()`——
  // “命中几个”与“回了几个”是同一个数。调用方（智能体）拿 `limit=20` 时会把
  // `count:20` 读成“命中 20 个”，明明多于 20 也看不出来（实测：小模型据此
  // 误解返回结构并写出错误的解析分支）。
  ServerFixture fx;
  Probe probe(fx.port, fx.token);
  ST_REQUIRE(probe.call("hello").value("ok", false));

  // 再补几个 Button：夹具自带 1 个（#btn-ok），使总数达到 5 > limit=2
  for (int index = 0; index < 4; ++index) {
    Json create = Json::object();
    create["type"] = "Button";
    Json props = Json::object();
    props["label"] = "额外";
    create["props"] = props;
    ST_REQUIRE(probe.call("ui.create", create).value("ok", false));
  }

  Json limited = Json::object();
  limited["selector"] = "Button";
  limited["limit"] = 2;
  const Json result = probe.call("find", limited)["result"];
  ST_CHECK_EQ(result["matches"].size(), 2U);
  ST_CHECK_EQ(result["count"].get<std::uint64_t>(), 2U);  // 回了几条
  // 命中几条（关键：不被 limit 截断）。用 value() 而不是 operator[]：缺字段时
  // 拿 0 报断言失败，而不是抛异常（异常会掩盖真因，见 CONVENTIONS §7 异常边界条）。
  ST_CHECK_EQ(result.value("total", std::uint64_t{0}), 5U);

  // 反向：limit 超过命中数时两个数相等（否则 total 是个恒等于全树的假数）
  Json all = Json::object();
  all["selector"] = "Button";
  all["limit"] = 50;
  const Json all_result = probe.call("find", all)["result"];
  ST_CHECK_EQ(all_result["count"].get<std::uint64_t>(), 5U);
  ST_CHECK_EQ(all_result.value("total", std::uint64_t{0}), 5U);
}

ST_TEST(capabilities_list_matches_implemented_methods) {
  ServerFixture fx;
  Probe probe(fx.port, fx.token);
  const Json hello = probe.call("hello");
  ST_REQUIRE(hello.value("ok", false));
  const Json& result = hello["result"];
  ST_REQUIRE(result.contains("capabilities"));
  ST_REQUIRE(result["capabilities"].is_array());
  ST_CHECK(result["capabilities"].size() >= 19U);

  // 只探「参数无关、必定可调用」的那一类方法：其余（wait/invoke/input.*/set/capture…）
  // 要么需要有效目标，要么会阻塞或产生副作用——它们是否**存在**由 `hello` 之外的方式保证
  // （调用它们得到的是「参数错误」而不是「未知方法」，同样能区分存在与否）。
  // 这里对每个声明能力都发一次**空参数**请求，断言错误（若有）不是「未知方法」。
  for (const auto& capability : result["capabilities"]) {
    const std::string method = capability.get<std::string>();
    const Json reply = probe.call(method, Json::object(), 2000);
    const std::string message = reply.contains("error") && reply["error"].is_object()
                                    ? reply["error"].value("message", std::string{})
                                    : std::string{};
    const bool unknown_method = message.find("未知方法") != std::string::npos;
    ST_CHECK(!unknown_method);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// `#` 选择器对含点 id 的容错（BACKLOG P2 2026-10-15）：整串 id 匹配优先
// ————————————————————————————————————————————————————————————————————————————

/// `Selector::parse` 按 CSS 语义把 `.` 当类型分隔符，而业务 id 常含点
/// （git 面板的 `git-file-<path>` 等）——`find("#git-file-alpha.txt")` 被拆成
/// `Id: git-file-alpha + Type: txt`，count 恒 0，调用方只看到「找不到元素」，
/// 离真因很远（智能体会反复撞）。
///
/// 容错契约：`#xxx` 形态的纯 id 选择器先按整串试精确匹配，命中即用；
/// 不命中退回 CSS 解析。三条断言分别钉住：
///   ① 含点 id 经 `#` 整串找到；
///   ② 不含点 id 行为不变（两条路径本就等价）；
///   ③ 双失败时报错文案含属性写法指引（不再只有一句「找不到」）。
/// 逆向验证：把 server.cpp 里整串优先分支临时回退（只走 CSS 解析）后，
/// 用例①变红（count 恒 0）、用例③变红（报错变回静默 count:0），
/// 用例②始终绿（它守的就是不变量）。实测 2026-10-15。
ST_TEST(find_hash_selector_matches_id_containing_dots) {
  ServerFixture fx;
  Probe probe(fx.port, fx.token);
  ST_REQUIRE(probe.call("hello").value("ok", false));

  // 含点 id 的元素（git 面板业务 id 的形态）
  {
    Json create = Json::object();
    create["type"] = "Button";
    Json props = Json::object();
    props["label"] = "alpha";
    create["id"] = "git-file-alpha.txt";
    create["props"] = props;
    ST_REQUIRE(probe.call("ui.create", create).value("ok", false));
  }

  // ① 含点 id：`#` 整串容错命中（修复前拆成 Id: git-file-alpha + Type: txt，count 恒 0）
  {
    Json params = Json::object();
    params["selector"] = "#git-file-alpha.txt";
    const Json reply = probe.call("find", params);
    ST_REQUIRE(reply.value("ok", false));
    const Json& result = reply["result"];
    // matches 非空作为前置（REQUIRES）：回退验证时它是空数组，若只 CHECK size 后面
    // 的 [0] 索引会越界 abort 杀掉整个分片（PITFALLS 场景 2：分片静默失联）。
    ST_REQUIRE(result["matches"].is_array() && !result["matches"].empty());
    ST_CHECK_EQ(result["matches"].size(), 1U);
    ST_CHECK_EQ(result["count"].get<std::uint64_t>(), 1U);
    ST_CHECK_EQ(result.value("total", std::uint64_t{0}), 1U);
    ST_CHECK_EQ(result["matches"][0].value("id", std::string{}), std::string("git-file-alpha.txt"));
  }

  // 与属性写法互为对照：同一元素，`[id=...]` 两条路径都能命中（语义一致性）
  {
    Json params = Json::object();
    params["selector"] = "[id=git-file-alpha.txt]";
    const Json reply = probe.call("find", params);
    ST_REQUIRE(reply.value("ok", false));
    ST_CHECK_EQ(reply["result"]["matches"].size(), 1U);
  }

  // ② 不含点 id：行为与修复前完全一致（夹具自带 #btn-ok）
  {
    Json params = Json::object();
    params["selector"] = "#btn-ok";
    const Json reply = probe.call("find", params);
    ST_REQUIRE(reply.value("ok", false));
    ST_REQUIRE(reply["result"]["matches"].is_array());
    ST_CHECK_EQ(reply["result"]["matches"].size(), 1U);
  }
  // 不存在的无点 id：仍是「ok + count 0」的静默形态（既有调用方依赖它做存在性探询）
  {
    Json params = Json::object();
    params["selector"] = "#btn-missing";
    const Json reply = probe.call("find", params);
    ST_REQUIRE(reply.value("ok", false));
    ST_CHECK_EQ(reply["result"]["matches"].size(), 0U);
    ST_CHECK_EQ(reply["result"]["count"].get<std::uint64_t>(), 0U);
  }

  // ③ 含点且整串不中、CSS 拆分也不中：报错文案必须含属性写法指引
  {
    Json params = Json::object();
    params["selector"] = "#git-file-missing.txt";
    const Json reply = probe.call("find", params);
    // 先查 ok 再取 error（PITFALLS：nlohmann 缺键 operator[] const 是 UB）
    ST_REQUIRE(!reply.value("ok", true));
    ST_REQUIRE(reply.contains("error") && reply["error"].is_object());
    const std::string message = reply["error"].value("message", std::string{});
    ST_CHECK(message.find("[id=git-file-missing.txt]") != std::string::npos);
    ST_CHECK(message.find("CSS") != std::string::npos);
  }

  // 混合选择器不受容错影响：`#` 后带空格/属性段的仍走 CSS 解析（本例按 Button 类型命中）
  {
    Json params = Json::object();
    params["selector"] = "#btn-ok[enabled=true]";
    const Json reply = probe.call("find", params);
    ST_REQUIRE(reply.value("ok", false));
    ST_CHECK_EQ(reply["result"]["matches"].size(), 1U);
  }
}

ST_TEST(invoke_activate_item_reaches_menu_panel_by_id) {
  // MenuPanel 动作面端到端（BACKLOG 3800）：`invoke("activate_item", item_id)`
  // ——e2e 原本只能 `bounds.y + 16 + i*32` 数行序点击，菜单动态化（最近打开）后
  // 行序会变，坐标法脆。这条用例钉住协议层三件事：
  //   ① id 激活可达且真的派发了动作（on_activate 链）；
  //   ② 无效 id 如实 `handled=false`（不假装成功、不报错——自动化据此分支）；
  //   ③ 未命中后面板仍在（不关不派发，可重试）。
  // 单元层的同链断言见 ui_menu_test.cpp 的
  // `menu_panel_activate_item_by_id_drives_same_chain`。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_REQUIRE(probe.call("hello").value("ok", false));

  // 面板必须先“打开”：MenuPanel 本身是 overlay（dsl 的 menu_panel_overlay），
  // 挂到 root 才能被 find 定位（UiRoot::find 会搜 overlay 树，但不会凭空造出它）。
  auto menu = std::make_unique<st::ui::MenuPanel>(std::vector<st::ui::MenuItem>{
      st::ui::MenuItem{.id = "new", .label = "新建"},
      st::ui::MenuItem{.id = "sep", .label = "", .separator = true},
      st::ui::MenuItem{.id = "recent-1", .label = "最近打开 1"}});
  menu->set_id("demo-menu");  // 已有 id 会保留（assign_ids 只填空 id）
  st::ui::MenuPanel* menu_ptr = menu.get();
  fx.host.root_.add_overlay(std::move(menu), UiRoot::OverlayLayout::FillViewport);
  fx.host.root_.layout();

  std::vector<std::string> activated;
  int closed = 0;
  menu_ptr->on_activate = [&activated, menu_ptr](std::size_t index) {
    activated.push_back(std::string(menu_ptr->item_id(index)));
  };
  menu_ptr->on_close = [&closed]() { ++closed; };

  const auto invoke_action = [&probe](const char* id, const char* action,
                                       const char* argument) -> bool {
    Json params = Json::object();
    params["id"] = id;
    params["action"] = action;
    params["argument"] = argument;
    const Json reply = probe.call("invoke", params);
    if (!reply.value("ok", false)) return false;
    if (!reply.contains("result")) return false;
    return reply["result"].value("handled", false);
  };

  // ① id 激活：动作真实发生（回调链走到，下标指向带 id 的那一项）。
  ST_REQUIRE(invoke_action("demo-menu", "activate_item", "recent-1"));
  ST_CHECK_EQ(activated.size(), 1U);
  ST_CHECK_EQ(activated.front(), std::string("recent-1"));
  // 与鼠标 Click 同链的关闭信号也到了（声明式 overlay 据此摘面板）。
  ST_CHECK_EQ(closed, 1);

  // ② 无效 id：如实 handled=false，不派发、不关闭。
  ST_CHECK(!invoke_action("demo-menu", "activate_item", "no-such-item"));
  ST_CHECK_EQ(activated.size(), 1U);
  ST_CHECK_EQ(closed, 1);

  // ③ 未命中后面板仍在：find 仍能定位（自动化可重试或改走下标路径）。
  Json find_params = Json::object();
  find_params["selector"] = "#demo-menu";
  const Json found = probe.call("find", find_params);
  ST_REQUIRE(found.value("ok", false));
  ST_CHECK_EQ(found["result"]["matches"].size(), 1U);
}

ST_TEST(set_pending_paths_reaches_file_dialog_via_protocol) {
  // FileDialog 多选选路的协议层端到端（BACKLOG 3895，MenuPanel 用例同模式）：
  // `ui.create` 建对话框（工厂默认 Open）→ `set` multi_select/pending_paths →
  // `get`/`invoke` 回带的 state 读回 `selected_paths` → `invoke confirm` 驱动
  // `on_confirm_multiple` 整份交付。这条钉住的是「外部（智能体）与界面之间唯一的
  // 通用通道」上能力可达——只留 C++ 接口等于“能力存在但外部用不到”。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_REQUIRE(probe.call("hello").value("ok", false));

  // 造临时文件树（同目录两个文件——多选的前提）。
  auto made = st::fs::make_temp_dir("st-fd-proto");
  ST_REQUIRE(made.has_value());
  const std::string dir = *made;
  (void)st::fs::write_text(st::fs::join(dir, "a.txt"), "abc");
  (void)st::fs::write_text(st::fs::join(dir, "b.md"), "hello");
  struct Clean { std::string d; ~Clean() { (void)st::fs::remove_all(d); } } clean{dir};

  // 建对话框：FileDialog 工厂注册在 ui.create（dsl.cpp X 宏），挂 root 后可 find。
  Json create_params = Json::object();
  create_params["type"] = "FileDialog";
  create_params["id"] = "the-dialog";
  const Json created = probe.call("ui.create", create_params);
  ST_REQUIRE(created.value("ok", false));
  ST_CHECK_EQ(created["result"]["type"].get<std::string>(), std::string("FileDialog"));

  // 回调挂接：元素在服务进程里，只能从测试宿主侧拿（find 同一棅树）。
  st::ui::FileDialog* dialog =
      dynamic_cast<st::ui::FileDialog*>(fx.host.root_.find("the-dialog"));
  ST_REQUIRE(dialog != nullptr);
  std::vector<std::string> delivered;
  dialog->on_confirm_multiple = [&](const std::vector<std::string>& paths) {
    delivered = paths;
  };

  const auto set_prop = [&probe](const char* id, const char* name,
                                 const std::string& value) -> bool {
    Json params = Json::object();
    params["id"] = id;
    Json props = Json::object();
    props[name] = value;
    params["props"] = props;
    const Json reply = probe.call("set", params);
    if (!reply.value("ok", false)) return false;
    if (!reply.contains("result") || !reply["result"].contains("changed")) return false;
    for (const auto& item : reply["result"]["changed"]) {
      if (item.get<std::string>() == std::string(name)) return true;
    }
    return false;
  };

  // ① 开多选：changed 回带（apply_properties 只把元素声明的属性放行，
  // multi_select 已在 property_names 里——这条同时钉住「声明即可达」）。
  ST_CHECK(set_prop("the-dialog", "multi_select", "true"));
  // ② 程序化多选选路：`|` 分隔两个路径。
  const std::string a = st::fs::join(dir, "a.txt");
  const std::string b = st::fs::join(dir, "b.md");
  ST_CHECK(set_prop("the-dialog", "pending_paths", a + "|" + b));
  // ③ get 快照读回：selected_paths 与注入一致（顺序保留）。
  {
    Json params = Json::object();
    params["id"] = "the-dialog";
    const Json got = probe.call("get", params);
    ST_REQUIRE(got.value("ok", false));
    ST_CHECK_EQ(got["result"]["props"]["selected_paths"].get<std::string>(),
                a + "|" + b);
  }
  // ④ invoke confirm：handled=true，回带 state 里 selected_paths 仍在，
  // 且 on_confirm_multiple 已收到整份清单（外部驱动 → 元素回调链全通）。
  {
    Json params = Json::object();
    params["id"] = "the-dialog";
    params["action"] = "confirm";
    params["argument"] = "";
    const Json reply = probe.call("invoke", params);
    ST_REQUIRE(reply.value("ok", false));
    ST_CHECK(reply["result"].value("handled", false));
    ST_CHECK_EQ(reply["result"]["state"]["selected_paths"].get<std::string>(),
                a + "|" + b);
  }
  ST_CHECK_EQ(delivered.size(), std::size_t{2});
  ST_CHECK(delivered.size() == 2U && delivered[0] == a && delivered[1] == b);

  // ⑤ 无效路径不静默：set 响应仍 ok（元素存在即 ok），但 changed 清单里
  // **没有** pending_paths（apply_properties 只把真正应用的名字登记进去）——
  // 调用方看 changed 就能判别“没改到”，不会把失败误读成成功。
  // ⚠ 这里同时是一条已知缺口（见终报）：set 对“声明过但 set_property 拒了”的
  // 属性**没有顶层 error**（与 invoke 的 handled=false 红线不同口径）。
  {
    Json params = Json::object();
    params["id"] = "the-dialog";
    Json props = Json::object();
    props["pending_paths"] = st::fs::join(dir, "nope.txt");
    params["props"] = props;
    const Json reply = probe.call("set", params);
    ST_REQUIRE(reply.value("ok", true));
    bool in_changed = false;
    if (reply.contains("result") && reply["result"].contains("changed")) {
      for (const auto& item : reply["result"]["changed"]) {
        if (item.get<std::string>() == std::string("pending_paths")) in_changed = true;
      }
    }
    ST_CHECK(!in_changed);   // 不在 changed 里 = 没改到，调用方可判别
  }
}

ST_TEST(invoke_confirm_reaches_directory_mode_file_dialog_via_protocol) {
  // 选目录模式的协议层端到端（BACKLOG 3895 后半，mode 写侧补齐后改纯协议路径）：
  // `ui.create` 建 Open（工厂无参只能建 Open）→ `set mode=directory` **纯协议切到
  // Directory 模式** → `set pending_path` 指向目录 → `invoke confirm` 交出**目录本身**。
  // 这条钉住“换工作区”类自动化（gbcode open_folder_dialog）的协议面可达性。
  //（旧版用 C++ 侧 `FileDialog::make(Directory)` 挂载——那是因为 mode 无写侧；
  // 写侧接通后，C++ 侧挂载反而是被测缺口本身未覆盖的盲区。）
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_REQUIRE(probe.call("hello").value("ok", false));

  auto made = st::fs::make_temp_dir("st-fd-dir-proto");
  ST_REQUIRE(made.has_value());
  const std::string dir = *made;
  (void)st::fs::create_directories(st::fs::join(dir, "sub"));
  struct Clean { std::string d; ~Clean() { (void)st::fs::remove_all(d); } } clean{dir};

  // FileDialog 是模态浮层形态：ui.create 协议建 Open 模式实例（工厂无参构造），
  // 挂 root 为 overlay（与旧版 C++ 侧挂载同一条 overlay 路）。
  {
    Json params = Json::object();
    params["type"] = "FileDialog";
    params["id"] = "dir-dialog";
    const Json reply = probe.call("ui.create", params);
    ST_REQUIRE(reply.value("ok", false));
  }
  // ⓪ set mode=directory：纯协议切模式（本用例的主断言对象）。
  // changed 里必须出现 mode（apply_properties 只登记真正应用的名字）。
  {
    Json params = Json::object();
    params["id"] = "dir-dialog";
    Json props = Json::object();
    props["mode"] = "directory";
    params["props"] = props;
    const Json reply = probe.call("set", params);
    ST_REQUIRE(reply.value("ok", false));
    bool mode_changed = false;
    for (const auto& item : reply["result"]["changed"]) {
      if (item.get<std::string>() == std::string("mode")) mode_changed = true;
    }
    ST_CHECK(mode_changed);
  }
  // ⓪' 无效模式串不静默：不在 changed 里（拼写错不猜，与 pending_paths 的
  // 无效路径同口径）。
  {
    Json params = Json::object();
    params["id"] = "dir-dialog";
    Json props = Json::object();
    props["mode"] = "directry";   // 故意拼错
    params["props"] = props;
    const Json reply = probe.call("set", params);
    ST_REQUIRE(reply.value("ok", true));
    bool in_changed = false;
    if (reply.contains("result") && reply["result"].contains("changed")) {
      for (const auto& item : reply["result"]["changed"]) {
        if (item.get<std::string>() == std::string("mode")) in_changed = true;
      }
    }
    ST_CHECK(!in_changed);
  }
  // get 读回 mode 确认切换落位（读侧与写侧同口径：小写 `directory`）。
  {
    Json params = Json::object();
    params["id"] = "dir-dialog";
    const Json got = probe.call("get", params);
    ST_REQUIRE(got.value("ok", false));
    ST_CHECK_EQ(got["result"]["props"]["mode"].get<std::string>(),
                std::string("directory"));
  }

  // 找回对话框接上确认回调（与 set_pending_paths 用例同手法：ui.create 后元素
  // 归 UiRoot 管，find 同一棅树拿到裸指针，不断所有权）。
  auto* raw = dynamic_cast<st::ui::FileDialog*>(fx.host.root_.find("dir-dialog"));
  ST_REQUIRE(raw != nullptr);
  std::string confirmed;
  raw->on_confirm = [&](const std::string& path) { confirmed = path; };

  // ① set pending_path（目录路径）：changed 回带（进入该目录）。
  {
    Json params = Json::object();
    params["id"] = "dir-dialog";
    Json props = Json::object();
    props["pending_path"] = st::fs::join(dir, "sub");
    params["props"] = props;
    const Json reply = probe.call("set", params);
    ST_REQUIRE(reply.value("ok", false));
    bool in_changed = false;
    for (const auto& item : reply["result"]["changed"]) {
      if (item.get<std::string>() == std::string("pending_path")) in_changed = true;
    }
    ST_CHECK(in_changed);
  }
  // ② get 读回 directory 就是目标目录。
  {
    Json params = Json::object();
    params["id"] = "dir-dialog";
    const Json got = probe.call("get", params);
    ST_REQUIRE(got.value("ok", false));
    ST_CHECK_EQ(got["result"]["props"]["directory"].get<std::string>(),
                st::fs::join(dir, "sub"));
  }
  // ③ invoke confirm：交出目录本身（不是拼接路径）。
  {
    Json params = Json::object();
    params["id"] = "dir-dialog";
    params["action"] = "confirm";
    params["argument"] = "";
    const Json reply = probe.call("invoke", params);
    ST_REQUIRE(reply.value("ok", false));
    ST_CHECK(reply["result"].value("handled", false));
  }
  ST_CHECK_EQ(confirmed, st::fs::join(dir, "sub"));
}

ST_TEST(set_reports_rejected_properties_alongside_changed) {
  // set 应答的 rejected 清单（BACKLOG 4022 方向①，与 changed 对偶）：混合批次
  // 一次下发「合法名 + 拼错名 + 无效值 + 能力未开」，断言 changed/rejected **各自**
  // 的内容与拒因分类——e2e 拼错属性名不再需要 diff changed 才能发现静默跳过。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_REQUIRE(probe.call("hello").value("ok", false));

  // 在同一棵夹具树上加一个**不可聚焦**的 Text（基类 focusable_=false 且不开启），
  // 专测「名字合法但能力未开」的拒因——Button/Input 是可聚焦的，测不到这个分支。
  auto text = std::make_unique<st::ui::Text>("一段只读文本");
  text->set_id("plain-text");
  fx.host.root_.find("root-panel")->add_child(std::move(text));

  const auto reason_of = [](const Json& reply, const std::string& name) -> std::string {
    if (!reply.contains("result")) return "<no-result>";
    const Json& result = reply["result"];
    if (!result.contains("rejected")) return "<no-rejected>";
    for (const auto& item : result["rejected"]) {
      if (item.value("name", std::string{}) == name) {
        return item.value("reason", std::string("<no-reason>"));
      }
    }
    return "<not-rejected>";
  };
  const auto in_changed = [](const Json& reply, const std::string& name) -> bool {
    if (!reply.contains("result")) return false;
    const Json& result = reply["result"];
    if (!result.contains("changed")) return false;
    for (const auto& item : result["changed"]) {
      if (item.get<std::string>() == name) return true;
    }
    return false;
  };

  // ① 混合批次（主体断言）：四类属性一次下发，changed/rejected 各归各位。
  {
    Json params = Json::object();
    params["id"] = "btn-ok";
    Json props = Json::object();
    props["label"] = "保存";           // 合法：元素声明的属性面（changed）
    props["labell"] = "拼错的属性名";   // 拼错名：unknown_name（原缺陷场景本体）
    props["variant"] = "primry";       // 无效值：名字认得但枚举拼错（invalid_value）
    props["align_items"] = "middle";   // 无效值：布局面枚举拼错（invalid_value）
    props["border_color"] = "not-a-color";  // 无效值：颜色解析拒（invalid_value）
    params["props"] = props;
    const Json reply = probe.call("set", params);
    ST_REQUIRE(reply.value("ok", true));   // 元素存在即 ok：批量语义不变

    // changed：恰好只含 label（全部合法项）
    ST_REQUIRE(reply["result"].contains("changed"));
    ST_REQUIRE(reply["result"]["changed"].is_array());
    ST_CHECK_EQ(reply["result"]["changed"].size(), 1U);
    ST_CHECK(in_changed(reply, "label"));
    // 改动真的落位（回读走 get，不同信念源）
    {
      Json get_params = Json::object();
      get_params["id"] = "btn-ok";
      const Json got = probe.call("get", get_params);
      ST_REQUIRE(got.value("ok", false));
      ST_CHECK_EQ(got["result"]["props"]["label"].get<std::string>(), std::string("保存"));
    }

    // rejected：四项各自带正确拒因
    ST_REQUIRE(reply["result"].contains("rejected"));
    ST_REQUIRE(reply["result"]["rejected"].is_array());
    ST_CHECK_EQ(reply["result"]["rejected"].size(), 4U);
    ST_CHECK_EQ(reason_of(reply, "labell"), std::string("unknown_name"));
    ST_CHECK_EQ(reason_of(reply, "variant"), std::string("invalid_value"));
    ST_CHECK_EQ(reason_of(reply, "align_items"), std::string("invalid_value"));
    ST_CHECK_EQ(reason_of(reply, "border_color"), std::string("invalid_value"));
    // 对偶不变式：同一属性不重复出现（拼错项不得混进 changed）
    ST_CHECK(!in_changed(reply, "labell"));
    ST_CHECK(!in_changed(reply, "variant"));
  }

  // ② 能力未开：focused=true 到不可聚焦元素（名字合法但能力没开，unfocusable）。
  {
    Json params = Json::object();
    params["id"] = "plain-text";
    Json props = Json::object();
    props["focused"] = true;
    params["props"] = props;
    const Json reply = probe.call("set", params);
    ST_REQUIRE(reply.value("ok", true));
    ST_CHECK(!in_changed(reply, "focused"));   // 旧口径不变：不进 changed
    ST_CHECK_EQ(reason_of(reply, "focused"), std::string("unfocusable"));
    // 拒因分类是关键差异：名字认得且值是合法布尔，只是元素不可聚焦——
    // 若误报 invalid_value，调用方会去改值而不是去查元素类型。
  }

  // ③ 未知布局名 vs 已知布局名的区分度：拼错的布局名仍归 unknown_name
  //    （is_layout_property_name 只认完整真名，不前缀匹配）。
  {
    Json params = Json::object();
    params["id"] = "btn-ok";
    Json props = Json::object();
    props["pading"] = 8;   // "padding" 拼错：名字不认得 → unknown_name
    params["props"] = props;
    const Json reply = probe.call("set", params);
    ST_REQUIRE(reply.value("ok", true));
    ST_CHECK_EQ(reason_of(reply, "pading"), std::string("unknown_name"));
    ST_CHECK(!in_changed(reply, "pading"));
  }
}

ST_TEST(set_all_valid_props_stays_legacy_compatible) {
  // 全合法 set 的**兼容性**用例（BACKLOG 4022 方向①的零破坏面）：
  // 没有被拒项时，应答**不携带 rejected 字段**（与 ui.changed 事件「非空才带」
  // 同一写法）——老客户端（不认识 rejected）永远看不到它，新客户端可据此
  // 省一次判空。
  TokenFixture fx;
  Probe probe(fx.port, fx.token);
  ST_REQUIRE(probe.call("hello").value("ok", false));

  Json params = Json::object();
  params["id"] = "btn-ok";
  Json props = Json::object();
  props["label"] = "全合法批次";   // 元素属性面
  props["enabled"] = true;        // 框架级
  props["margin"] = 6;            // 布局面
  params["props"] = props;
  const Json reply = probe.call("set", params);
  ST_REQUIRE(reply.value("ok", false));

  ST_REQUIRE(reply["result"].contains("changed"));
  ST_CHECK_EQ(reply["result"]["changed"].size(), 3U);
  // 兼容性断言本体：rejected 字段**缺席**（不是空数组——空数组也是新形态，
  // 老客户端拿严格 schema 校验会失败）。
  ST_CHECK(!reply["result"].contains("rejected"));
}
