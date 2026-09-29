#include "st/control/control.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <format>
#include <memory>
#include <vector>

#include "st/core/base64.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/net.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/selector.hpp"

namespace st::control {
namespace {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kKeepAliveLogLines = 200;

[[nodiscard]] auto bounds_to_json(math::Rect rect) -> Json {
  Json value = Json::object();
  value["x"] = static_cast<double>(rect.x);
  value["y"] = static_cast<double>(rect.y);
  value["width"] = static_cast<double>(rect.width);
  value["height"] = static_cast<double>(rect.height);
  return value;
}

[[nodiscard]] auto semantics_to_json(const ui::SemanticsNode& node) -> Json {
  Json value = Json::object();
  value["id"] = node.id;
  value["type"] = node.type;
  value["role"] = std::string(ui::to_string(node.role));
  value["bounds"] = bounds_to_json(node.bounds);
  if (!node.text.empty()) value["text"] = node.text;
  if (!node.value.empty()) value["value"] = node.value;
  const ui::SemanticsFlags& flags = node.flags;
  Json state = Json::object();
  state["visible"] = flags.visible;
  state["enabled"] = flags.enabled;
  state["focused"] = flags.focused;
  state["hovered"] = flags.hovered;
  state["pressed"] = flags.pressed;
  state["selected"] = flags.selected;
  state["checked"] = flags.checked;
  state["scrollable"] = flags.scrollable;
  state["editable"] = flags.editable;
  value["state"] = std::move(state);
  if (!node.children.empty()) {
    Json children = Json::array();
    for (const auto& child : node.children) children.push_back(semantics_to_json(child));
    value["children"] = std::move(children);
  }
  return value;
}

[[nodiscard]] auto visual_to_json(const ui::VisualNode& node) -> Json {
  Json value = Json::object();
  value["id"] = node.id;
  value["type"] = node.type;
  value["bounds"] = bounds_to_json(node.bounds);
  value["visible"] = node.visible;
  if (!node.fill.empty()) value["fill"] = node.fill;
  if (node.radius > 0.0f) value["radius"] = static_cast<double>(node.radius);
  if (!node.text.empty()) value["text"] = node.text;
  value["hit_target"] = node.hit_target;
  if (!node.children.empty()) {
    Json children = Json::array();
    for (const auto& child : node.children) children.push_back(visual_to_json(child));
    value["children"] = std::move(children);
  }
  return value;
}

[[nodiscard]] auto parse_modifiers(const Json& params) -> std::array<bool, 4> {
  // ctrl / shift / alt / meta（也接受逗号分隔的 modifiers 字符串）
  std::array<bool, 4> flags{false, false, false, false};
  if (const Json* modifiers = json_find(params, "modifiers"); modifiers != nullptr) {
    if (modifiers->is_array()) {
      for (const auto& item : *modifiers) {
        const std::string name = ascii_lower(json_as_string(item));
        if (name == "ctrl" || name == "control") flags[0] = true;
        if (name == "shift") flags[1] = true;
        if (name == "alt" || name == "option") flags[2] = true;
        if (name == "meta" || name == "cmd" || name == "super") flags[3] = true;
      }
    } else if (modifiers->is_string()) {
      for (const auto part : split(json_as_string(*modifiers), ',')) {
        const std::string name = ascii_lower(trim(part));
        if (name == "ctrl" || name == "control") flags[0] = true;
        if (name == "shift") flags[1] = true;
        if (name == "alt" || name == "option") flags[2] = true;
        if (name == "meta" || name == "cmd" || name == "super") flags[3] = true;
      }
    }
  }
  for (const auto name : {"ctrl", "shift", "alt", "meta"}) {
    // 注意：这里**不能**用 nlohmann 的 `at()`——它在键缺失时抛 out_of_range，而按修饰键
    // 是否出现过是可选的（`{"kind":"click"}` 不带任何修饰键）。`json_at` 缺键返回 null 节点。
    if (const Json& flag = json_at(params, name); flag.is_boolean() && json_as_bool(flag)) {
      const std::string key = name;
      if (key == "ctrl") flags[0] = true;
      if (key == "shift") flags[1] = true;
      if (key == "alt") flags[2] = true;
      if (key == "meta") flags[3] = true;
    }
  }
  return flags;
}

struct Frame {
  std::uint32_t length{0};
  std::string body{};
};

}  // namespace

struct Server::Impl {
  struct Client {
    st::net::TcpStream stream{};
    std::vector<std::uint8_t> buffer{};  ///< 接收缓冲（二进制字节，非文本）
    bool subscribed{false};
    std::vector<std::string> event_kinds{};
    std::string peer{};
  };

  struct PendingWait {
    std::size_t client{0};
    std::uint64_t request_id{0};
    std::string kind{};
    std::string selector{};
    std::string text{};
    std::int64_t deadline_ms{0};
    std::int64_t stable_since_ms{0};
    std::uint64_t last_version{0};
  };

  explicit Impl(Host& host_ref) : host(host_ref) {}

  Host& host;
  ServerOptions options{};

  st::net::TcpListener listener{};
  std::vector<std::unique_ptr<Client>> clients{};
  std::vector<PendingWait> waits{};
  std::vector<std::string> log_ring{};
  std::uint64_t requests{0};
  std::uint64_t event_sequence{0};
  std::uint64_t last_published_version{0};
  std::int64_t started_ms{0};
  bool active{false};

  [[nodiscard]] auto send(Client& client, const Json& message) -> bool {
    const std::string body = json_dump(message);
    if (body.size() > options.max_frame) {
      log::warn("控制通道响应超过帧上限（{} 字节）", body.size());
      return false;
    }
    const auto length = static_cast<std::uint32_t>(body.size());
    std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>((length >> 24U) & 0xFFU),
                                       static_cast<std::uint8_t>((length >> 16U) & 0xFFU),
                                       static_cast<std::uint8_t>((length >> 8U) & 0xFFU),
                                       static_cast<std::uint8_t>(length & 0xFFU)};
    if (auto status = client.stream.write_all(header); !status) return false;
    if (auto status = client.stream.write_text(body); !status) return false;
    return true;
  }

  void respond(Client& client, std::uint64_t id, Json result) {
    Json message = Json::object();
    message["id"] = static_cast<std::uint64_t>(id);
    message["ok"] = true;
    message["result"] = std::move(result);
    (void)send(client, message);
  }

  void fail(Client& client, std::uint64_t id, ErrorCode code, std::string text) {
    Json message = Json::object();
    message["id"] = static_cast<std::uint64_t>(id);
    message["ok"] = false;
    Json error = Json::object();
    error["code"] = std::string(st::to_string(code));
    error["message"] = std::move(text);
    message["error"] = std::move(error);
    (void)send(client, message);
  }

  void publish_to_clients(std::string_view event, const Json& data) {
    Json message = Json::object();
    message["event"] = std::string(event);
    message["seq"] = ++event_sequence;
    message["data"] = data;
    for (auto& client : clients) {
      if (!client->subscribed) continue;
      if (!client->event_kinds.empty()) {
        const auto& kinds = client->event_kinds;
        if (std::ranges::find(kinds, std::string(event)) == kinds.end()) continue;
      }
      (void)send(*client, message);
    }
  }

  /// 组件定位：接受 `id` 与选择器风格 `#id`（智能体常用后者，容错更省一轮往返）。
  [[nodiscard]] auto find_element(std::string_view id) -> ui::Element* {
    if (!id.empty() && id.front() == '#') id.remove_prefix(1);
    return host.root().find(id);
  }


  [[nodiscard]] auto wait_satisfied(const PendingWait& wait) -> std::optional<bool> {
    auto& root = host.root();
    if (wait.kind == "element" || wait.kind == "gone") {
      auto selector = ui::Selector::parse(wait.selector);
      if (!selector) return false;
      const auto matches = root.query(*selector, 1);
      const bool present = !matches.empty();
      return wait.kind == "element" ? present : !present;
    }
    if (wait.kind == "text" || wait.kind == "text_gone") {
      const auto node = root.semantics(64);
      bool found = false;
      const auto search = [&](auto&& self, const ui::SemanticsNode& item) -> void {
        if (found) return;
        if (item.text.find(wait.text) != std::string::npos ||
            item.value.find(wait.text) != std::string::npos) {
          found = true;
          return;
        }
        for (const auto& child : item.children) self(self, child);
      };
      search(search, node);
      return wait.kind == "text" ? found : !found;
    }
    if (wait.kind == "stable") {
      return false;  // 由时间判定（见 poll_waits）
    }
    return std::nullopt;
  }

  void poll_waits() {
    const std::int64_t now = time::now_ms();
    for (auto iterator = waits.begin(); iterator != waits.end();) {
      PendingWait& wait = *iterator;
      if (wait.client >= clients.size()) {
        iterator = waits.erase(iterator);
        continue;
      }
      std::optional<bool> satisfied = wait_satisfied(wait);
      if (wait.kind == "stable") {
        const std::uint64_t version = host.root().version();
        if (version != wait.last_version) {
          wait.last_version = version;
          wait.stable_since_ms = now;
        }
        const std::int64_t stable_ms = now - wait.stable_since_ms;
        if (stable_ms >= 120) {
          Json result = Json::object();
          result["satisfied"] = true;
          result["elapsed_ms"] = static_cast<std::int64_t>(now - (wait.deadline_ms - 20000));
          result["detail"] = "画面已稳定";
          respond(*clients[wait.client], wait.request_id, std::move(result));
          iterator = waits.erase(iterator);
          continue;
        }
      } else if (satisfied.has_value() && *satisfied) {
        Json result = Json::object();
        result["satisfied"] = true;
        result["elapsed_ms"] = 0;
        result["detail"] = std::format("条件满足: {}", wait.kind);
        respond(*clients[wait.client], wait.request_id, std::move(result));
        iterator = waits.erase(iterator);
        continue;
      }
      if (now >= wait.deadline_ms) {
        Json result = Json::object();
        result["satisfied"] = false;
        result["elapsed_ms"] = 0;
        result["detail"] = std::format("等待超时: {}", wait.kind);
        respond(*clients[wait.client], wait.request_id, std::move(result));
        iterator = waits.erase(iterator);
        continue;
      }
      ++iterator;
    }
  }

  [[nodiscard]] auto handle(Client& client, std::uint64_t id, std::string_view method,
                            const Json& params, bool& deferred) -> Result<Json>;

  void dispatch_frame(Client& client, std::string_view body) {
    auto message = json_parse(body);
    if (!message) {
      Json error = Json::object();
      error["id"] = 0;
      error["ok"] = false;
      Json detail = Json::object();
      detail["code"] = "bad_request";
      detail["message"] = message.error().message;
      error["error"] = std::move(detail);
      (void)send(client, error);
      return;
    }
    const std::uint64_t id = json_get_i64(*message, "id", 0) < 0
                                 ? 0
                                 : static_cast<std::uint64_t>(json_get_i64(*message, "id", 0));
    const std::string method = json_get_string(*message, "method");
    const Json& params = json_at(*message, "params");
    if (method.empty()) {
      fail(client, id, ErrorCode::Invalid, "缺少 method");
      return;
    }
    ++requests;
    bool deferred = false;
    const std::int64_t start_ns = time::now_ns();
    auto result = handle(client, id, method, params, deferred);
    if (deferred) {
      // 已挂起（wait）：不立即响应
    } else if (result) {
      respond(client, id, std::move(*result));
    } else {
      fail(client, id, result.error().code, result.error().message);
    }
    if (options.log_calls) {
      const std::string line = std::format("{} [{}] {} {}us", time::iso8601_now(), client.peer,
                                           method, (time::now_ns() - start_ns) / 1000);
      log::info("control {}", line);
      log_ring.push_back(line);
      if (log_ring.size() > kKeepAliveLogLines) log_ring.erase(log_ring.begin());
    }
  }

  void accept_clients() {
    while (true) {
      auto ready = listener.wait_readable(0);
      if (!ready || !*ready) return;
      auto stream = listener.accept();
      if (!stream) return;
      auto client = std::make_unique<Client>();
      client->peer = stream->peer_address();
      client->stream = std::move(*stream);
      client->stream.set_nonblocking(true);
      log::info("控制通道：客户端接入 {}", client->peer);
      clients.push_back(std::move(client));
    }
  }

  void read_clients() {
    for (std::size_t index = 0; index < clients.size();) {
      Client& client = *clients[index];
      std::array<std::uint8_t, 8192> buffer{};
      bool closed = false;
      // **先探可读再读**：客户端是非阻塞的，直接 read_some 在无数据时会走「等待可读」分支，
      // 空转 5 秒后被判超时——既拖慢主循环，又会把"只是没说话"的连接误判为断开（曾因此丢掉 wait 挂起连接）。
      while (true) {
        auto readable = client.stream.wait_readable(0);
        if (!readable) {
          closed = true;
          break;
        }
        if (!*readable) break;  // 本次没有数据，保持连接
        auto chunk = client.stream.read_some(buffer);
        if (!chunk) {
          // 非阻塞下的 EAGAIN/超时按"暂时无数据"处理；真正的 EOF 由 `*chunk == 0` 表达
          if (chunk.error().code == ErrorCode::Timeout) break;
          closed = true;
          break;
        }
        if (*chunk == 0) {
          closed = true;
          break;
        }
        client.buffer.insert(client.buffer.end(), buffer.begin(),
                             buffer.begin() + static_cast<std::ptrdiff_t>(*chunk));
        if (*chunk < buffer.size()) break;
      }
      while (client.buffer.size() >= 4) {
        const auto first = static_cast<std::uint8_t>(client.buffer[0]);
        const auto second = static_cast<std::uint8_t>(client.buffer[1]);
        const auto third = static_cast<std::uint8_t>(client.buffer[2]);
        const auto fourth = static_cast<std::uint8_t>(client.buffer[3]);
        const std::uint32_t length = (static_cast<std::uint32_t>(first) << 24U) |
                                     (static_cast<std::uint32_t>(second) << 16U) |
                                     (static_cast<std::uint32_t>(third) << 8U) |
                                     static_cast<std::uint32_t>(fourth);
        if (length == 0 || length > options.max_frame) {
          closed = true;
          break;
        }
        if (client.buffer.size() < static_cast<std::size_t>(length) + 4) break;
        const std::string body(client.buffer.begin() + 4,
                               client.buffer.begin() +
                                   static_cast<std::ptrdiff_t>(static_cast<std::size_t>(length) + 4));
        client.buffer.erase(client.buffer.begin(),
                            client.buffer.begin() +
                                static_cast<std::ptrdiff_t>(static_cast<std::size_t>(length) + 4));
        dispatch_frame(client, body);
      }
      if (closed) {
        log::info("控制通道：客户端断开 {}", client.peer);
        clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index));
        continue;
      }
      ++index;
    }
  }

  void publish_changes() {
    const std::uint64_t version = host.root().version();
    if (version == last_published_version) return;
    last_published_version = version;
    Json data = Json::object();
    data["version"] = version;
    publish_to_clients("ui.changed", data);
  }
};

auto Server::Impl::handle(Client& client, std::uint64_t id, std::string_view method,
                          const Json& params, bool& deferred) -> Result<Json> {
  auto& root = host.root();

  if (method == "hello") {
    Json result = Json::object();
    result["protocol"] = static_cast<std::uint64_t>(kProtocolVersion);
    Json app = Json::object();
    app["name"] = host.app_name();
    app["version"] = host.app_version();
    result["app"] = std::move(app);
    // 之前这里是 `::getpid() == 0 ? 0 : 0`——**永远返回 0**（pid 从未真正上报过），
    // 且 `getpid` 是 POSIX 接口，Windows 上不存在（交叉编译直接报错）。
    result["pid"] = process::current_id();
    result["backend"] = std::string(host.backend_name());
    result["headless"] = host.headless();
    Json screen = Json::object();
    const math::Size viewport = host.viewport();
    // 坐标语义：**协议内一切坐标均为逻辑像素**；物理像素 = 逻辑 × scale
    screen["width"] = static_cast<double>(viewport.width);
    screen["height"] = static_cast<double>(viewport.height);
    screen["scale"] = static_cast<double>(host.device_scale());
    screen["physical_width"] = static_cast<double>(viewport.width * host.device_scale());
    screen["physical_height"] = static_cast<double>(viewport.height * host.device_scale());
    screen["coordinate_space"] = "logical";
    result["screen"] = std::move(screen);
    Json theme = Json::object();
    theme["mode"] = root.theme().mode() == ui::ThemeMode::Dark ? "dark" : "light";
    result["theme"] = std::move(theme);
    Json capabilities = Json::array();
    for (const auto name : {"tree", "find", "get", "set", "invoke", "input.mouse", "input.key",
                            "input.text", "capture", "visual", "wait", "metrics", "events",
                            "theme", "app"}) {
      capabilities.push_back(Json(name));
    }
    // `script` 只在真正启用时上报：能力清单是"这个进程能做什么"的事实说明，
    // 不能列一个一调就报 Unsupported 的方法。
    if (host.script() != nullptr) capabilities.push_back(Json("script"));
    result["capabilities"] = std::move(capabilities);
    if (json_get_bool(params, "subscribe", false)) {
      client.subscribed = true;
      for (const auto& kind : json_get_string_array(params, "kinds")) client.event_kinds.push_back(kind);
    }
    return result;
  }
  if (method == "ping") {
    Json result = Json::object();
    result["ts"] = static_cast<std::int64_t>(time::unix_ms());
    return result;
  }
  if (method == "tree") {
    const auto depth = json_get_i64(params, "depth", 0);
    Json result = Json::object();
    result["tree"] = semantics_to_json(root.semantics(static_cast<std::uint32_t>(depth)));
    result["version"] = root.version();
    return result;
  }
  if (method == "find") {
    auto selector = ui::Selector::parse(json_get_string(params, "selector"));
    if (!selector) return forward_error(selector.error());
    const auto limit = static_cast<std::size_t>(json_get_i64(params, "limit", 50));
    auto matches = root.query(*selector, limit);
    Json list = Json::array();
    for (auto* element : matches) list.push_back(ui::element_to_json(*element));
    Json result = Json::object();
    result["selector"] = json_get_string(params, "selector");
    result["count"] = static_cast<std::uint64_t>(list.size());
    result["matches"] = std::move(list);
    return result;
  }
  if (method == "get") {
    ui::Element* element = find_element(json_get_string(params, "id"));
    if (element == nullptr) {
      return unexpected(ErrorCode::NotFound,
                        std::format("未找到元素: {}", json_get_string(params, "id")));
    }
    return ui::element_snapshot(*element);
  }
  if (method == "set") {
    ui::Element* element = find_element(json_get_string(params, "id"));
    if (element == nullptr) {
      return unexpected(ErrorCode::NotFound, std::format("未找到元素: {}", json_get_string(params, "id")));
    }
    const Json& props = json_at(params, "props");
    if (!props.is_object()) return unexpected(ErrorCode::Invalid, "props 必须是对象");
    Json changed = ui::apply_properties(root, *element, props);
    host.request_repaint();
    Json result = Json::object();
    result["changed"] = std::move(changed);
    return result;
  }
  if (method == "invoke") {
    ui::Element* element = find_element(json_get_string(params, "id"));
    if (element == nullptr) {
      return unexpected(ErrorCode::NotFound, std::format("未找到元素: {}", json_get_string(params, "id")));
    }
    const std::string action = json_get_string(params, "action", "click");
    const std::string argument = json_get_string(params, "argument");
    const bool handled = ui::invoke_element(root, *element, action, argument);
    host.request_repaint();
    Json result = Json::object();
    result["handled"] = handled;
    result["action"] = action;
    result["id"] = element->derived_id();
    return result;
  }
  if (method == "input.mouse") {
    const std::string kind = json_get_string(params, "kind", "move");
    ui::Event event;
    event.position = math::Point{static_cast<float>(json_get_double(params, "x", 0.0)),
                                 static_cast<float>(json_get_double(params, "y", 0.0))};
    event.button = static_cast<int>(json_get_i64(params, "button", 1));
    event.click_count = static_cast<int>(json_get_i64(params, "click_count", 1));
    event.wheel_delta = static_cast<float>(json_get_double(params, "delta", 0.0));
    const std::array<bool, 4> modifiers = parse_modifiers(params);
    event.ctrl = modifiers[0];
    event.shift = modifiers[1];
    event.alt = modifiers[2];
    event.meta = modifiers[3];

    if (kind == "move") {
      event.kind = ui::EventKind::MouseMove;
    } else if (kind == "down") {
      event.kind = ui::EventKind::MouseDown;
    } else if (kind == "up") {
      event.kind = ui::EventKind::MouseUp;
    } else if (kind == "click") {
      event.kind = ui::EventKind::MouseDown;
      (void)root.dispatch(event);
      event.kind = ui::EventKind::MouseUp;
      (void)root.dispatch(event);
      event.kind = ui::EventKind::Click;
    } else if (kind == "dblclick") {
      event.kind = ui::EventKind::DoubleClick;
    } else if (kind == "triple") {
      event.kind = ui::EventKind::TripleClick;
    } else if (kind == "scroll" || kind == "wheel") {
      event.kind = ui::EventKind::Wheel;
    } else {
      return unexpected(ErrorCode::Invalid, std::format("未知鼠标消息: {}", kind));
    }
    Json hit = Json::object();
    if (ui::Element* target = root.hit_test(event.position); target != nullptr) {
      hit = ui::element_to_json(*target);
    }
    const bool handled = root.dispatch(event);
    host.request_repaint();
    Json result = Json::object();
    result["handled"] = handled;
    result["hit"] = std::move(hit);
    result["kind"] = kind;
    return result;
  }
  if (method == "input.key" || method == "input.text") {
    ui::Event event;
    const std::array<bool, 4> modifiers = parse_modifiers(params);
    event.ctrl = modifiers[0];
    event.shift = modifiers[1];
    event.alt = modifiers[2];
    event.meta = modifiers[3];
    if (method == "input.text") {
      event.kind = ui::EventKind::TextInput;
      event.text = json_get_string(params, "text");
      if (const std::string target = json_get_string(params, "id"); !target.empty()) {
        if (ui::Element* element = find_element(target); element != nullptr) root.set_focus(element);
      }
    } else {
      const std::string kind = json_get_string(params, "kind", "press");
      event.key = json_get_string(params, "key");
      event.code = json_get_string(params, "code");
      event.text = json_get_string(params, "text");
      if (kind == "down") {
        event.kind = ui::EventKind::KeyDown;
      } else if (kind == "up") {
        event.kind = ui::EventKind::KeyUp;
      } else {
        event.kind = ui::EventKind::KeyDown;
      }
    }
    if (event.kind == ui::EventKind::TextInput && event.text.empty()) {
      return unexpected(ErrorCode::Invalid, "input.text 需要 text 参数");
    }
    const bool handled = root.dispatch(event);
    host.request_repaint();
    Json result = Json::object();
    result["handled"] = handled;
    const ui::Element* focused = root.focused();
    result["focused"] = focused != nullptr ? focused->derived_id() : std::string{};
    return result;
  }
  if (method == "capture") {
    math::IntRect region{};
    if (const std::string target = json_get_string(params, "id"); !target.empty()) {
      ui::Element* element = find_element(target);
      if (element == nullptr) {
        return unexpected(ErrorCode::NotFound, std::format("未找到元素: {}", target));
      }
      region = element->bounds().round_out();
    } else if (const Json* raw = json_find(params, "region"); raw != nullptr && raw->is_object()) {
      region = math::IntRect{static_cast<int>(json_get_i64(*raw, "x", 0)),
                             static_cast<int>(json_get_i64(*raw, "y", 0)),
                             static_cast<int>(json_get_i64(*raw, "width", 0)),
                             static_cast<int>(json_get_i64(*raw, "height", 0))};
    }
    const std::string encode = json_get_string(params, "encode", "base64");
    Json result = Json::object();
    if (encode == "file" || params.contains("path")) {
      auto saved = host.capture_to_file(json_get_string(params, "path"), region);
      if (!saved) return forward_error(saved.error());
      result["path"] = *saved;
    } else {
      auto png = host.capture_png(region);
      if (!png) return forward_error(png.error());
      result["base64"] = base64_encode(std::span<const std::uint8_t>(*png));
      result["bytes"] = static_cast<std::uint64_t>(png->size());
    }
    result["format"] = "png";
    // 回包标注：region 为**逻辑坐标**（协议口径），pixel_size 为实际导出的**物理像素**尺寸——
    // 两者在 HiDPI 下不同（2x 时像素尺寸是逻辑尺寸的两倍），调用方据此换算而不必猜。
    result["region"] = bounds_to_json(math::Rect{static_cast<float>(region.x),
                                                   static_cast<float>(region.y),
                                                   static_cast<float>(region.width),
                                                   static_cast<float>(region.height)});
    {
      const float scale = host.device_scale();
      const math::Size viewport = host.viewport();
      Json pixels = Json::object();
      if (region.is_empty()) {
        pixels["width"] = static_cast<std::int64_t>(
                                std::lround(static_cast<double>(viewport.width) * static_cast<double>(scale)));
        pixels["height"] = static_cast<std::int64_t>(
                                 std::lround(static_cast<double>(viewport.height) * static_cast<double>(scale)));
      } else {
        pixels["width"] = static_cast<std::int64_t>(
                                std::lround(static_cast<double>(region.width) * static_cast<double>(scale)));
        pixels["height"] = static_cast<std::int64_t>(
                                 std::lround(static_cast<double>(region.height) * static_cast<double>(scale)));
      }
      pixels["device_scale"] = static_cast<double>(scale);
      result["pixel_size"] = pixels;
    }
    return result;
  }
  if (method == "visual") {
    Json result = Json::object();
    result["tree"] = visual_to_json(root.visual_tree());
    result["version"] = root.version();
    return result;
  }
  if (method == "script") {
    // 默认关闭：未开启时明确告知"该能力未启用"（而不是含糊的"未知方法"）
    ui::ScriptHost* script = host.script();
    if (script == nullptr) {
      return unexpected(ErrorCode::Unsupported,
                        "脚本能力未启用（需以 --enable-script 启动；脚本=进程内执行代码，故默认关闭）");
    }
    // 一个方法覆盖四种用途：执行 / 调用 / 绑定 / 读状态（AI 用同一入口完成"写逻辑 → 触发 → 验证"）
    const std::string handler = json_get_string(params, "on");
    if (!handler.empty()) {
      auto bound = script->bind(json_get_string(params, "selector"),
                                json_get_string(params, "event", "click"), handler);
      if (!bound) return forward_error(bound.error());
      host.request_repaint();
      Json result = Json::object();
      result["binding"] = *bound;
      return result;
    }
    const std::string unbind_id = json_get_string(params, "off");
    if (!unbind_id.empty()) {
      Json result = Json::object();
      result["removed"] = script->unbind(unbind_id);
      return result;
    }
    if (json_get_bool(params, "bindings", false)) {
      Json list = Json::array();
      for (const auto& binding : script->bindings()) {
        Json item = Json::object();
        item["id"] = binding.id;
        item["selector"] = binding.selector;
        item["event"] = binding.event;
        item["handler"] = binding.handler;
        list.push_back(std::move(item));
      }
      Json result = Json::object();
      result["bindings"] = std::move(list);
      return result;
    }
    if (json_get_bool(params, "state", false)) {
      auto state = script->state();
      if (!state) return forward_error(state.error());
      Json result = Json::object();
      result["state"] = std::move(*state);
      return result;
    }
    const std::string code = json_get_string(params, "code");
    if (!code.empty()) {
      auto evaluated = script->eval(code, json_get_string(params, "filename", "<control>"));
      host.request_repaint();
      if (!evaluated) return forward_error(evaluated.error());
      Json result = Json::object();
      result["result"] = std::move(*evaluated);
      result["ops"] = static_cast<std::uint64_t>(script->last_stats().ops);
      result["memory"] = static_cast<std::uint64_t>(script->last_stats().memory_used);
      return result;
    }
    const std::string function = json_get_string(params, "function");
    if (function.empty()) {
      return unexpected(ErrorCode::Invalid, "script 需要 code / function / on / off / bindings / state 之一");
    }
    std::vector<Json> arguments;
    const Json& raw_arguments = json_at(params, "args");
    if (raw_arguments.is_array()) {
      for (const Json& item : raw_arguments) arguments.push_back(item);
    }
    auto called = script->call(function, arguments);
    host.request_repaint();
    if (!called) return forward_error(called.error());
    Json result = Json::object();
    result["result"] = std::move(*called);
    return result;
  }
  if (method == "metrics") {
    Metrics metrics = host.metrics();
    Json result = Json::object();
    result["backend"] = metrics.backend;
    result["headless"] = metrics.headless;
    result["device_scale"] = static_cast<double>(metrics.device_scale);
    result["physical_width"] = static_cast<std::uint64_t>(metrics.physical_width);
    result["physical_height"] = static_cast<std::uint64_t>(metrics.physical_height);
    result["uptime_ms"] = metrics.uptime_ms;
    result["frames"] = static_cast<std::uint64_t>(metrics.frames);
    result["last_frame_ms"] = metrics.last_frame_ms;
    result["frame_p50_ms"] = metrics.frame_p50_ms;
    result["frame_p95_ms"] = metrics.frame_p95_ms;
    result["nodes"] = static_cast<std::uint64_t>(metrics.nodes);
    result["requests"] = static_cast<std::uint64_t>(requests);
    result["clients"] = static_cast<std::uint64_t>(clients.size());
    Json log_value = Json::array();
    for (const auto& line : log_ring) log_value.push_back(Json(line));
    result["log"] = std::move(log_value);
    return result;
  }
  if (method == "events") {
    client.subscribed = json_get_bool(params, "enable", true);
    client.event_kinds = json_get_string_array(params, "kinds");
    Json result = Json::object();
    result["enabled"] = client.subscribed;
    Json kinds = Json::array();
    for (const auto& kind : client.event_kinds) kinds.push_back(Json(kind));
    result["kinds"] = std::move(kinds);
    return result;
  }
  if (method == "theme") {
    if (const std::string mode = json_get_string(params, "mode"); !mode.empty()) {
      if (mode == "light") {
        host.set_theme_mode(ui::ThemeMode::Light);
      } else if (mode == "dark") {
        host.set_theme_mode(ui::ThemeMode::Dark);
      } else if (mode == "toggle") {
        host.set_theme_mode(root.theme().mode() == ui::ThemeMode::Dark ? ui::ThemeMode::Light
                                                                     : ui::ThemeMode::Dark);
      } else {
        return unexpected(ErrorCode::Invalid, std::format("未知主题模式: {}", mode));
      }
      host.request_repaint();
    }
    Json result = Json::object();
    result["mode"] = root.theme().mode() == ui::ThemeMode::Dark ? "dark" : "light";
    return result;
  }
  if (method == "wait") {
    const std::string kind = json_get_string(params, "for", "element");
    if (kind != "element" && kind != "gone" && kind != "text" && kind != "text_gone" &&
        kind != "stable") {
      return unexpected(ErrorCode::Unsupported, std::format("不支持的等待条件: {}", kind));
    }
    PendingWait wait;
    wait.client = 0;
    for (std::size_t index = 0; index < clients.size(); ++index) {
      if (clients[index].get() == &client) {
        wait.client = index;
        break;
      }
    }
    wait.request_id = id;
    wait.kind = kind;
    wait.selector = json_get_string(params, "selector");
    wait.text = json_get_string(params, "text");
    const std::int64_t timeout = json_get_i64(params, "timeout_ms", 5000);
    wait.deadline_ms = time::now_ms() + timeout;
    wait.stable_since_ms = time::now_ms();
    wait.last_version = root.version();
    if (kind == "element" || kind == "gone") {
      if (wait.selector.empty()) return unexpected(ErrorCode::Invalid, "wait 需要 selector");
    }
    if (kind == "text" || kind == "text_gone") {
      if (wait.text.empty()) return unexpected(ErrorCode::Invalid, "wait 需要 text");
    }
    // 立即满足则直接返回，不再挂起
    if (kind == "stable") {
      waits.push_back(wait);
      deferred = true;
      return Json::object();
    }
    const std::optional<bool> satisfied = wait_satisfied(wait);
    if (satisfied.has_value() && *satisfied) {
      Json result = Json::object();
      result["satisfied"] = true;
      result["elapsed_ms"] = 0;
      result["detail"] = std::format("条件已满足: {}", kind);
      return result;
    }
    waits.push_back(wait);
    deferred = true;
    return Json::object();
  }
  if (method == "app") {
    const std::string action = json_get_string(params, "action");
    if (action == "quit" || action == "close") {
      host.request_quit();
      Json result = Json::object();
      result["ok"] = true;
      result["action"] = action;
      return result;
    }
    if (action == "reload" || action == "repaint") {
      host.request_repaint();
      Json result = Json::object();
      result["ok"] = true;
      return result;
    }
    if (action == "set_scale" || action == "scale") {
      const double requested = json_get_double(params, "scale", 0.0);
      if (requested <= 0.0) return unexpected(ErrorCode::Invalid, "set_scale 需要正的 scale 参数");
      if (auto status = host.set_device_scale(static_cast<float>(requested)); !status) {
        return forward_error(status.error());
      }
      host.request_repaint();
      Json result = Json::object();
      result["scale"] = static_cast<double>(host.device_scale());
      result["physical_width"] = static_cast<double>(host.viewport().width * host.device_scale());
      result["physical_height"] = static_cast<double>(host.viewport().height * host.device_scale());
      return result;
    }
    if (action == "log") {
      const auto limit = static_cast<std::size_t>(json_get_i64(params, "limit", 50));
      Json lines = Json::array();
      for (const auto& line : host.log_lines(limit)) lines.push_back(Json(line));
      Json result = Json::object();
      result["lines"] = std::move(lines);
      return result;
    }
    if (action == "focus") {
      const std::string target = json_get_string(params, "id");
      if (target.empty()) return unexpected(ErrorCode::Invalid, "app.focus 需要 id");
      root.set_focus(find_element(target));
      Json result = Json::object();
      result["ok"] = true;
      return result;
    }
    return unexpected(ErrorCode::Unsupported, std::format("未知 app 动作: {}", action));
  }
  if (method == "shutdown") {
    host.request_quit();
    Json result = Json::object();
    result["ok"] = true;
    return result;
  }
  return unexpected(ErrorCode::Unsupported, std::format("未知方法: {}", method));
}

Server::Server(Host& host) : impl_(std::make_unique<Impl>(host)) {}

Server::~Server() { stop(); }

auto Server::start(const ServerOptions& options) -> Result<std::uint16_t> {
  impl_->options = options;
  auto listener = st::net::TcpListener::bind(options.bind, options.port);
  if (!listener) return forward_error(listener.error());
  listener->set_nonblocking(false);
  impl_->listener = std::move(*listener);
  impl_->active = true;
  impl_->started_ms = time::now_ms();

  if (!options.control_file.empty()) {
    Json info = Json::object();
    info["port"] = static_cast<std::uint64_t>(impl_->listener.port());
    // 又一个"恒为 0"的占位写法（同 `hello` 里那处 `getpid()`）：外部工具靠这个 pid
    // 判断进程是否还活着，写 0 等于让它永远判不出来。
    info["pid"] = static_cast<std::int64_t>(process::current_id());
    info["app"] = impl_->host.app_name();
    info["version"] = impl_->host.app_version();
    info["backend"] = std::string(impl_->host.backend_name());
    info["headless"] = impl_->host.headless();
    info["protocol"] = static_cast<std::uint64_t>(kProtocolVersion);
    if (auto status = json_write_file(options.control_file, info, true); !status) {
      return forward_error(status.error());
    }
  }
  log::info("控制通道已启动 tcp://{}:{}", options.bind, impl_->listener.port());
  return impl_->listener.port();
}

void Server::stop() {
  if (!impl_ || !impl_->active) return;
  impl_->clients.clear();
  impl_->waits.clear();
  impl_->listener.close();
  impl_->active = false;
}

void Server::poll() {
  if (!impl_ || !impl_->active) return;
  impl_->accept_clients();
  impl_->read_clients();
  impl_->poll_waits();
  impl_->publish_changes();
}

void Server::publish(std::string_view event, const Json& data) {
  if (!impl_) return;
  impl_->publish_to_clients(event, data);
}

auto Server::port() const noexcept -> std::uint16_t {
  return impl_ ? impl_->listener.port() : 0;
}

auto Server::client_count() const noexcept -> std::size_t {
  return impl_ ? impl_->clients.size() : 0;
}

auto Server::running() const noexcept -> bool { return impl_ && impl_->active; }

}  // namespace st::control
