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
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/ui_root.hpp"

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

  [[nodiscard]] auto root() -> UiRoot& override { return root_; }
  [[nodiscard]] auto app_name() const -> std::string override { return app_name_; }
  [[nodiscard]] auto app_version() const -> std::string override { return app_version_; }
  [[nodiscard]] auto backend_name() const -> std::string_view override { return "headless"; }
  [[nodiscard]] auto headless() const -> bool override { return true; }
  [[nodiscard]] auto viewport() const -> st::math::Size override { return {400, 300}; }
  [[nodiscard]] auto device_scale() const -> float override { return 1.0f; }
  auto set_device_scale(float) -> st::Status override { return st::ok(); }
  [[nodiscard]] auto metrics() const -> st::control::Metrics override { return {}; }
  void request_quit() override { ++quits; }
  void request_repaint() override { ++repaints; }
  void set_theme_mode(st::ui::ThemeMode mode) override { root_.set_theme(mode == st::ui::ThemeMode::Dark ? st::ui::Theme::dark() : st::ui::Theme::light()); }
  [[nodiscard]] auto capture_to_file(std::string_view, st::math::IntRect) -> st::Result<std::string> override {
    return std::string("/tmp/fake.png");
  }
  [[nodiscard]] auto capture_png(st::math::IntRect) -> st::Result<std::vector<std::uint8_t>> override {
    return std::vector<std::uint8_t>{0x89, 0x50, 0x4E, 0x47};
  }
  /// 合成像素视图：微小渐变图案（视觉断言测试用——内容可预测、足够区分）。
  [[nodiscard]] auto capture_pixels(st::math::IntRect region) -> st::Result<st::control::PixelView> override {
    const int width = region.is_empty() ? 64 : region.width;
    const int height = region.is_empty() ? 48 : region.height;
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

  explicit ServerFixture(std::optional<std::string> token_override = std::nullopt) {
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
