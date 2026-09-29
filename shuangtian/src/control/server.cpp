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
#include "st/ui/selector.hpp"

namespace st::control {
namespace {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kKeepAliveLogLines = 200;

[[nodiscard]] auto bounds_to_json(math::Rect rect) -> Value {
  Value value = Value::object();
  value.set("x", static_cast<double>(rect.x));
  value.set("y", static_cast<double>(rect.y));
  value.set("width", static_cast<double>(rect.width));
  value.set("height", static_cast<double>(rect.height));
  return value;
}

[[nodiscard]] auto semantics_to_json(const ui::SemanticsNode& node) -> Value {
  Value value = Value::object();
  value.set("id", node.id);
  value.set("type", node.type);
  value.set("role", std::string(ui::to_string(node.role)));
  value.set("bounds", bounds_to_json(node.bounds));
  if (!node.text.empty()) value.set("text", node.text);
  if (!node.value.empty()) value.set("value", node.value);
  const ui::SemanticsFlags& flags = node.flags;
  Value state = Value::object();
  state.set("visible", flags.visible);
  state.set("enabled", flags.enabled);
  state.set("focused", flags.focused);
  state.set("hovered", flags.hovered);
  state.set("pressed", flags.pressed);
  state.set("selected", flags.selected);
  state.set("checked", flags.checked);
  state.set("scrollable", flags.scrollable);
  state.set("editable", flags.editable);
  value.set("state", std::move(state));
  if (!node.children.empty()) {
    Value children = Value::array();
    for (const auto& child : node.children) children.push(semantics_to_json(child));
    value.set("children", std::move(children));
  }
  return value;
}

[[nodiscard]] auto visual_to_json(const ui::VisualNode& node) -> Value {
  Value value = Value::object();
  value.set("id", node.id);
  value.set("type", node.type);
  value.set("bounds", bounds_to_json(node.bounds));
  value.set("visible", node.visible);
  if (!node.fill.empty()) value.set("fill", node.fill);
  if (node.radius > 0.0f) value.set("radius", static_cast<double>(node.radius));
  if (!node.text.empty()) value.set("text", node.text);
  value.set("hit_target", node.hit_target);
  if (!node.children.empty()) {
    Value children = Value::array();
    for (const auto& child : node.children) children.push(visual_to_json(child));
    value.set("children", std::move(children));
  }
  return value;
}

[[nodiscard]] auto element_to_json(ui::Element& element) -> Value {
  Value value = Value::object();
  value.set("id", element.derived_id());
  value.set("type", std::string(element.type()));
  value.set("role", std::string(ui::to_string(element.role())));
  value.set("bounds", bounds_to_json(element.bounds()));
  const std::string text = element.semantics_text();
  if (!text.empty()) value.set("text", text);
  const std::string item_value = element.semantics_value();
  if (!item_value.empty()) value.set("value", item_value);
  return value;
}

[[nodiscard]] auto parse_modifiers(const Value& params) -> std::array<bool, 4> {
  // ctrl / shift / alt / meta（也接受逗号分隔的 modifiers 字符串）
  std::array<bool, 4> flags{false, false, false, false};
  if (const Value* modifiers = params.find("modifiers"); modifiers != nullptr) {
    if (modifiers->is_array()) {
      for (const auto& item : modifiers->items()) {
        const std::string name = ascii_lower(item.as_string());
        if (name == "ctrl" || name == "control") flags[0] = true;
        if (name == "shift") flags[1] = true;
        if (name == "alt" || name == "option") flags[2] = true;
        if (name == "meta" || name == "cmd" || name == "super") flags[3] = true;
      }
    } else if (modifiers->is_string()) {
      for (const auto part : split(modifiers->as_string_view(), ',')) {
        const std::string name = ascii_lower(trim(part));
        if (name == "ctrl" || name == "control") flags[0] = true;
        if (name == "shift") flags[1] = true;
        if (name == "alt" || name == "option") flags[2] = true;
        if (name == "meta" || name == "cmd" || name == "super") flags[3] = true;
      }
    }
  }
  for (const auto name : {"ctrl", "shift", "alt", "meta"}) {
    if (const Value& flag = params.at(name); flag.is_bool() && flag.as_bool()) {
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

  [[nodiscard]] auto send(Client& client, const Value& message) -> bool {
    const std::string body = message.dump();
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

  void respond(Client& client, std::uint64_t id, Value result) {
    Value message = Value::object();
    message.set("id", static_cast<std::uint64_t>(id));
    message.set("ok", true);
    message.set("result", std::move(result));
    (void)send(client, message);
  }

  void fail(Client& client, std::uint64_t id, ErrorCode code, std::string text) {
    Value message = Value::object();
    message.set("id", static_cast<std::uint64_t>(id));
    message.set("ok", false);
    Value error = Value::object();
    error.set("code", std::string(st::to_string(code)));
    error.set("message", std::move(text));
    message.set("error", std::move(error));
    (void)send(client, message);
  }

  void publish_to_clients(std::string_view event, const Value& data) {
    Value message = Value::object();
    message.set("event", std::string(event));
    message.set("seq", ++event_sequence);
    message.set("data", data);
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
          Value result = Value::object();
          result.set("satisfied", true);
          result.set("elapsed_ms", static_cast<std::int64_t>(now - (wait.deadline_ms - 20000)));
          result.set("detail", "画面已稳定");
          respond(*clients[wait.client], wait.request_id, std::move(result));
          iterator = waits.erase(iterator);
          continue;
        }
      } else if (satisfied.has_value() && *satisfied) {
        Value result = Value::object();
        result.set("satisfied", true);
        result.set("elapsed_ms", 0);
        result.set("detail", std::format("条件满足: {}", wait.kind));
        respond(*clients[wait.client], wait.request_id, std::move(result));
        iterator = waits.erase(iterator);
        continue;
      }
      if (now >= wait.deadline_ms) {
        Value result = Value::object();
        result.set("satisfied", false);
        result.set("elapsed_ms", 0);
        result.set("detail", std::format("等待超时: {}", wait.kind));
        respond(*clients[wait.client], wait.request_id, std::move(result));
        iterator = waits.erase(iterator);
        continue;
      }
      ++iterator;
    }
  }

  [[nodiscard]] auto handle(Client& client, std::uint64_t id, std::string_view method,
                            const Value& params, bool& deferred) -> Result<Value>;

  void dispatch_frame(Client& client, std::string_view body) {
    auto message = json::parse(body);
    if (!message) {
      Value error = Value::object();
      error.set("id", 0);
      error.set("ok", false);
      Value detail = Value::object();
      detail.set("code", "bad_request");
      detail.set("message", message.error().message);
      error.set("error", std::move(detail));
      (void)send(client, error);
      return;
    }
    const std::uint64_t id = message->get_i64("id", 0) < 0
                                 ? 0
                                 : static_cast<std::uint64_t>(message->get_i64("id", 0));
    const std::string method = message->get_string("method");
    const Value& params = message->at("params");
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
    Value data = Value::object();
    data.set("version", version);
    publish_to_clients("ui.changed", data);
  }
};

auto Server::Impl::handle(Client& client, std::uint64_t id, std::string_view method,
                          const Value& params, bool& deferred) -> Result<Value> {
  auto& root = host.root();

  if (method == "hello") {
    Value result = Value::object();
    result.set("protocol", static_cast<std::uint64_t>(kProtocolVersion));
    Value app = Value::object();
    app.set("name", host.app_name());
    app.set("version", host.app_version());
    result.set("app", std::move(app));
    result.set("pid", static_cast<std::uint64_t>(::getpid() == 0 ? 0 : 0));
    result.set("backend", std::string(host.backend_name()));
    result.set("headless", host.headless());
    Value screen = Value::object();
    const math::Size viewport = host.viewport();
    // 坐标语义：**协议内一切坐标均为逻辑像素**；物理像素 = 逻辑 × scale
    screen.set("width", static_cast<double>(viewport.width));
    screen.set("height", static_cast<double>(viewport.height));
    screen.set("scale", static_cast<double>(host.device_scale()));
    screen.set("physical_width", static_cast<double>(viewport.width * host.device_scale()));
    screen.set("physical_height", static_cast<double>(viewport.height * host.device_scale()));
    screen.set("coordinate_space", "logical");
    result.set("screen", std::move(screen));
    Value theme = Value::object();
    theme.set("mode", root.theme().mode() == ui::ThemeMode::Dark ? "dark" : "light");
    result.set("theme", std::move(theme));
    Value capabilities = Value::array();
    for (const auto name : {"tree", "find", "get", "set", "invoke", "input.mouse", "input.key",
                            "input.text", "capture", "visual", "wait", "metrics", "events",
                            "theme", "app"}) {
      capabilities.push(Value{name});
    }
    result.set("capabilities", std::move(capabilities));
    if (params.get_bool("subscribe", false)) {
      client.subscribed = true;
      for (const auto& kind : params.get_string_array("kinds")) client.event_kinds.push_back(kind);
    }
    return result;
  }
  if (method == "ping") {
    Value result = Value::object();
    result.set("ts", static_cast<std::int64_t>(time::unix_ms()));
    return result;
  }
  if (method == "tree") {
    const auto depth = params.get_i64("depth", 0);
    Value result = Value::object();
    result.set("tree", semantics_to_json(root.semantics(static_cast<std::uint32_t>(depth))));
    result.set("version", root.version());
    return result;
  }
  if (method == "find") {
    auto selector = ui::Selector::parse(params.get_string("selector"));
    if (!selector) return forward_error(selector.error());
    const auto limit = static_cast<std::size_t>(params.get_i64("limit", 50));
    auto matches = root.query(*selector, limit);
    Value list = Value::array();
    for (auto* element : matches) list.push(element_to_json(*element));
    Value result = Value::object();
    result.set("selector", params.get_string("selector"));
    result.set("count", static_cast<std::uint64_t>(list.size()));
    result.set("matches", std::move(list));
    return result;
  }
  if (method == "get") {
    ui::Element* element = find_element(params.get_string("id"));
    if (element == nullptr) {
      return unexpected(ErrorCode::NotFound,
                        std::format("未找到元素: {}", params.get_string("id")));
    }
    Value result = element_to_json(*element);
    Value properties = Value::object();
    for (const auto name : element->property_names()) {
      if (auto value = element->get_property(name); value.has_value()) {
        properties.set(name, *value);
      }
    }
    properties.set("enabled", element->enabled());
    properties.set("visible", element->visible());
    result.set("props", std::move(properties));
    return result;
  }
  if (method == "set") {
    ui::Element* element = find_element(params.get_string("id"));
    if (element == nullptr) {
      return unexpected(ErrorCode::NotFound, std::format("未找到元素: {}", params.get_string("id")));
    }
    const Value& props = params.at("props");
    if (!props.is_object()) return unexpected(ErrorCode::Invalid, "props 必须是对象");
    Value changed = Value::array();
    for (const auto& [name, value] : props.fields()) {
      bool applied = false;
      if (name == "enabled") {
        element->set_enabled(value.as_bool(true));
        applied = true;
      } else if (name == "visible") {
        element->set_visible(value.as_bool(true));
        applied = true;
      } else if (name == "focused") {
        if (value.as_bool()) {
          root.set_focus(element);
        } else if (root.focused() == element) {
          root.set_focus(nullptr);
        }
        applied = true;
      } else if (name == "checked" || name == "selected" || name == "value" || name == "text" ||
                 name == "label" || name == "icon" || name == "options" || name == "active" ||
                 name == "scroll_offset") {
        const std::string text = value.is_string() ? value.as_string() : value.dump();
        applied = element->set_property(name, text);
      }
      if (applied) {
        changed.push(name);
        element->mark_dirty();
      }
    }
    host.request_repaint();
    Value result = Value::object();
    result.set("changed", std::move(changed));
    return result;
  }
  if (method == "invoke") {
    ui::Element* element = find_element(params.get_string("id"));
    if (element == nullptr) {
      return unexpected(ErrorCode::NotFound, std::format("未找到元素: {}", params.get_string("id")));
    }
    const std::string action = params.get_string("action", "click");
    const std::string argument = params.get_string("argument");
    bool handled = false;
    if (action == "focus" || action == "blur") {
      // 焦点必须经 UiRoot 设置：键盘事件按 root 的焦点元素派发，
      // 只改元素自身的 focused 标志会导致后续 input.text/input.key 无处可送。
      root.set_focus(action == "focus" ? element : nullptr);
      handled = true;
    } else {
      handled = element->invoke_action(action, argument);
    }
    host.request_repaint();
    Value result = Value::object();
    result.set("handled", handled);
    result.set("action", action);
    result.set("id", element->derived_id());
    return result;
  }
  if (method == "input.mouse") {
    const std::string kind = params.get_string("kind", "move");
    ui::Event event;
    event.position = math::Point{static_cast<float>(params.get_double("x", 0.0)),
                                 static_cast<float>(params.get_double("y", 0.0))};
    event.button = static_cast<int>(params.get_i64("button", 1));
    event.click_count = static_cast<int>(params.get_i64("click_count", 1));
    event.wheel_delta = static_cast<float>(params.get_double("delta", 0.0));
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
    Value hit = Value::object();
    if (ui::Element* target = root.hit_test(event.position); target != nullptr) {
      hit = element_to_json(*target);
    }
    const bool handled = root.dispatch(event);
    host.request_repaint();
    Value result = Value::object();
    result.set("handled", handled);
    result.set("hit", std::move(hit));
    result.set("kind", kind);
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
      event.text = params.get_string("text");
      if (const std::string target = params.get_string("id"); !target.empty()) {
        if (ui::Element* element = find_element(target); element != nullptr) root.set_focus(element);
      }
    } else {
      const std::string kind = params.get_string("kind", "press");
      event.key = params.get_string("key");
      event.code = params.get_string("code");
      event.text = params.get_string("text");
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
    Value result = Value::object();
    result.set("handled", handled);
    const ui::Element* focused = root.focused();
    result.set("focused", focused != nullptr ? focused->derived_id() : std::string{});
    return result;
  }
  if (method == "capture") {
    math::IntRect region{};
    if (const std::string target = params.get_string("id"); !target.empty()) {
      ui::Element* element = find_element(target);
      if (element == nullptr) {
        return unexpected(ErrorCode::NotFound, std::format("未找到元素: {}", target));
      }
      region = element->bounds().round_out();
    } else if (const Value* raw = params.find("region"); raw != nullptr && raw->is_object()) {
      region = math::IntRect{static_cast<int>(raw->get_i64("x", 0)),
                             static_cast<int>(raw->get_i64("y", 0)),
                             static_cast<int>(raw->get_i64("width", 0)),
                             static_cast<int>(raw->get_i64("height", 0))};
    }
    const std::string encode = params.get_string("encode", "base64");
    Value result = Value::object();
    if (encode == "file" || params.contains("path")) {
      auto saved = host.capture_to_file(params.get_string("path"), region);
      if (!saved) return forward_error(saved.error());
      result.set("path", *saved);
    } else {
      auto png = host.capture_png(region);
      if (!png) return forward_error(png.error());
      result.set("base64", base64_encode(std::span<const std::uint8_t>(*png)));
      result.set("bytes", static_cast<std::uint64_t>(png->size()));
    }
    result.set("format", "png");
    // 回包标注：region 为**逻辑坐标**（协议口径），pixel_size 为实际导出的**物理像素**尺寸——
    // 两者在 HiDPI 下不同（2x 时像素尺寸是逻辑尺寸的两倍），调用方据此换算而不必猜。
    result.set("region", bounds_to_json(math::Rect{static_cast<float>(region.x),
                                                   static_cast<float>(region.y),
                                                   static_cast<float>(region.width),
                                                   static_cast<float>(region.height)}));
    {
      const float scale = host.device_scale();
      const math::Size viewport = host.viewport();
      Value pixels = Value::object();
      if (region.is_empty()) {
        pixels.set("width", static_cast<std::int64_t>(
                                std::lround(static_cast<double>(viewport.width) * static_cast<double>(scale))));
        pixels.set("height", static_cast<std::int64_t>(
                                 std::lround(static_cast<double>(viewport.height) * static_cast<double>(scale))));
      } else {
        pixels.set("width", static_cast<std::int64_t>(
                                std::lround(static_cast<double>(region.width) * static_cast<double>(scale))));
        pixels.set("height", static_cast<std::int64_t>(
                                 std::lround(static_cast<double>(region.height) * static_cast<double>(scale))));
      }
      pixels.set("device_scale", static_cast<double>(scale));
      result.set("pixel_size", pixels);
    }
    return result;
  }
  if (method == "visual") {
    Value result = Value::object();
    result.set("tree", visual_to_json(root.visual_tree()));
    result.set("version", root.version());
    return result;
  }
  if (method == "metrics") {
    Metrics metrics = host.metrics();
    Value result = Value::object();
    result.set("backend", metrics.backend);
    result.set("headless", metrics.headless);
    result.set("device_scale", static_cast<double>(metrics.device_scale));
    result.set("physical_width", static_cast<std::uint64_t>(metrics.physical_width));
    result.set("physical_height", static_cast<std::uint64_t>(metrics.physical_height));
    result.set("uptime_ms", metrics.uptime_ms);
    result.set("frames", static_cast<std::uint64_t>(metrics.frames));
    result.set("last_frame_ms", metrics.last_frame_ms);
    result.set("frame_p50_ms", metrics.frame_p50_ms);
    result.set("frame_p95_ms", metrics.frame_p95_ms);
    result.set("nodes", static_cast<std::uint64_t>(metrics.nodes));
    result.set("requests", static_cast<std::uint64_t>(requests));
    result.set("clients", static_cast<std::uint64_t>(clients.size()));
    Value log_value = Value::array();
    for (const auto& line : log_ring) log_value.push(Value{line});
    result.set("log", std::move(log_value));
    return result;
  }
  if (method == "events") {
    client.subscribed = params.get_bool("enable", true);
    client.event_kinds = params.get_string_array("kinds");
    Value result = Value::object();
    result.set("enabled", client.subscribed);
    Value kinds = Value::array();
    for (const auto& kind : client.event_kinds) kinds.push(Value{kind});
    result.set("kinds", std::move(kinds));
    return result;
  }
  if (method == "theme") {
    if (const std::string mode = params.get_string("mode"); !mode.empty()) {
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
    Value result = Value::object();
    result.set("mode", root.theme().mode() == ui::ThemeMode::Dark ? "dark" : "light");
    return result;
  }
  if (method == "wait") {
    const std::string kind = params.get_string("for", "element");
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
    wait.selector = params.get_string("selector");
    wait.text = params.get_string("text");
    const std::int64_t timeout = params.get_i64("timeout_ms", 5000);
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
      return Value::object();
    }
    const std::optional<bool> satisfied = wait_satisfied(wait);
    if (satisfied.has_value() && *satisfied) {
      Value result = Value::object();
      result.set("satisfied", true);
      result.set("elapsed_ms", 0);
      result.set("detail", std::format("条件已满足: {}", kind));
      return result;
    }
    waits.push_back(wait);
    deferred = true;
    return Value::object();
  }
  if (method == "app") {
    const std::string action = params.get_string("action");
    if (action == "quit" || action == "close") {
      host.request_quit();
      Value result = Value::object();
      result.set("ok", true);
      result.set("action", action);
      return result;
    }
    if (action == "reload" || action == "repaint") {
      host.request_repaint();
      Value result = Value::object();
      result.set("ok", true);
      return result;
    }
    if (action == "set_scale" || action == "scale") {
      const double requested = params.get_double("scale", 0.0);
      if (requested <= 0.0) return unexpected(ErrorCode::Invalid, "set_scale 需要正的 scale 参数");
      if (auto status = host.set_device_scale(static_cast<float>(requested)); !status) {
        return forward_error(status.error());
      }
      host.request_repaint();
      Value result = Value::object();
      result.set("scale", static_cast<double>(host.device_scale()));
      result.set("physical_width", static_cast<double>(host.viewport().width * host.device_scale()));
      result.set("physical_height",
                 static_cast<double>(host.viewport().height * host.device_scale()));
      return result;
    }
    if (action == "log") {
      const auto limit = static_cast<std::size_t>(params.get_i64("limit", 50));
      Value lines = Value::array();
      for (const auto& line : host.log_lines(limit)) lines.push(Value{line});
      Value result = Value::object();
      result.set("lines", std::move(lines));
      return result;
    }
    if (action == "focus") {
      const std::string target = params.get_string("id");
      if (target.empty()) return unexpected(ErrorCode::Invalid, "app.focus 需要 id");
      root.set_focus(find_element(target));
      Value result = Value::object();
      result.set("ok", true);
      return result;
    }
    return unexpected(ErrorCode::Unsupported, std::format("未知 app 动作: {}", action));
  }
  if (method == "shutdown") {
    host.request_quit();
    Value result = Value::object();
    result.set("ok", true);
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
    Value info = Value::object();
    info.set("port", static_cast<std::uint64_t>(impl_->listener.port()));
    info.set("pid", static_cast<std::int64_t>(process::executable_path().has_value() ? 0 : 0));
    info.set("app", impl_->host.app_name());
    info.set("version", impl_->host.app_version());
    info.set("backend", std::string(impl_->host.backend_name()));
    info.set("headless", impl_->host.headless());
    info.set("protocol", static_cast<std::uint64_t>(kProtocolVersion));
    if (auto status = json::write_file(options.control_file, info, true); !status) {
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

void Server::publish(std::string_view event, const Value& data) {
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
