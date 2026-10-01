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
    std::uint64_t id{0};               ///< 稳定连接 id（自增，不随 vector erase 漂移）
    st::net::TcpStream stream{};
    std::vector<std::uint8_t> buffer{};  ///< 接收缓冲（二进制字节，非文本）
    std::size_t consumed{0};           ///< 已消费游标（避免头部 erase 的 O(n) 搬移）
    bool subscribed{false};
    bool greeted{false};               ///< 鉴权门：hello（且 token 校验通过）前拒绝一切其他方法
    std::vector<std::string> event_kinds{};
    std::string peer{};
  };

  struct PendingWait {
    std::uint64_t client{0};           ///< 稳定连接 id（非 vector 下标：erase 后下标会漂移，
                                       ///< 响应会发给错误的客户端——审视报告 P1-3）
    std::uint64_t request_id{0};
    std::string kind{};
    std::string selector{};
    std::string text{};
    std::int64_t started_ms{0};        ///< 请求入队时刻（elapsed_ms 的唯一依据）
    std::int64_t deadline_ms{0};
    std::int64_t stable_since_ms{0};
    std::uint64_t last_version{0};
    std::uint64_t frames_target{0};
  };

  explicit Impl(Host& host_ref) : host(host_ref) {}

  Host& host;
  ServerOptions options{};

  st::net::TcpListener listener{};
  std::vector<std::unique_ptr<Client>> clients{};
  std::vector<PendingWait> waits{};
  std::vector<std::string> log_ring{};
  std::vector<std::string> capture_dirs{};   ///< 启动时展开后的落盘白名单
  std::uint64_t requests{0};
  std::uint64_t event_sequence{0};
  std::uint64_t next_client_id{1};
  std::uint64_t last_published_version{0};
  std::int64_t started_ms{0};
  std::uint64_t frames_seen{0};           ///< 全局帧计数（wait for=frames 用）
  bool active{false};

  /// 发送失败不再静默：写不进去的连接必然已坏（对端关闭/半开），
  /// 旧实现 `(void)send` 会把「响应丢失」留给客户端当超时猜——停摆事故里
  /// 客户端等 60s+ 也不知道发生了什么。现在：失败即返 false，由调用方回收连接。
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
    // 帧头与体合并成一次 send：分两次发在 Nagle/慢网络下多一次拆包机会，
    // 且只需一次 would-block 判定（Windows 错误码修复后这里是安全路径）。
    std::vector<std::uint8_t> frame;
    frame.reserve(body.size() + 4);
    frame.insert(frame.end(), header.begin(), header.end());
    frame.insert(frame.end(), body.begin(), body.end());
    if (auto status = client.stream.write_all(frame); !status) {
      log::warn("控制通道发送失败（{}）：{}", client.peer, status.error().message);
      return false;
    }
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

  /// 按稳定 id 找客户端（erase 后下标会漂移；nullptr = 已断开，挂起 wait 作废）
  [[nodiscard]] auto client_by_id(std::uint64_t id) -> Client* {
    for (auto& client : clients) {
      if (client->id == id) return client.get();
    }
    return nullptr;
  }

  /// 回收已断开的客户端：同时作废它名下所有挂起 wait（旧实现只删客户端不删 wait，
  /// 悬垂 wait 到期时按漂移后的下标把响应发错人）。
  void drop_client(Client& client) {
    std::erase_if(waits, [&](const PendingWait& wait) { return wait.client == client.id; });
    auto iterator = std::find_if(clients.begin(), clients.end(),
                                 [&](const std::unique_ptr<Client>& item) { return item.get() == &client; });
    if (iterator != clients.end()) clients.erase(iterator);
  }

  void publish_to_clients(std::string_view event, const Json& data) {
    Json message = Json::object();
    message["event"] = std::string(event);
    message["seq"] = ++event_sequence;
    message["data"] = data;
    for (auto iterator = clients.begin(); iterator != clients.end();) {
      Client* client = iterator->get();
      if (!client->subscribed) {
        ++iterator;
        continue;
      }
      if (!client->event_kinds.empty()) {
        const auto& kinds = client->event_kinds;
        if (std::ranges::find(kinds, std::string(event)) == kinds.end()) {
          ++iterator;
          continue;
        }
      }
      if (!send(*client, message)) {
        // 事件推送失败 = 连接已坏：立即回收（否则坏连接永远留在列表里，每次推送都再失败一次）
        drop_client(*client);
        continue;
      }
      ++iterator;
    }
  }

  /// 组件定位：接受 `id` 与选择器风格 `#id`（智能体常用后者，容错更省一轮往返）。
  /// 检查路径是否落在白名单目录内（分隔符归一化后按前缀比较）。
  [[nodiscard]] auto capture_path_allowed(std::string_view path) -> bool {
    if (capture_dirs.empty()) return false;
    std::string normalized(path);
    std::ranges::replace(normalized, '\\', '/');
    for (const auto& dir : capture_dirs) {
      std::string prefix = dir;
      std::ranges::replace(prefix, '\\', '/');
      if (!prefix.empty() && !prefix.ends_with('/')) prefix.push_back('/');
      if (normalized.rfind(prefix, 0) == 0) return true;
    }
    return false;
  }

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
      Client* owner = client_by_id(wait.client);
      if (owner == nullptr) {
        // 客户端已断开：挂起 wait 作废（旧实现按下标取 clients，断开漂移后会把响应发错人）
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
          result["elapsed_ms"] = static_cast<std::int64_t>(now - wait.started_ms);
          result["detail"] = "画面已稳定";
          respond(*owner, wait.request_id, std::move(result));
          iterator = waits.erase(iterator);
          continue;
        }
      } else if (wait.kind == "frames") {
        if (frames_seen >= wait.frames_target) {
          Json result = Json::object();
          result["satisfied"] = true;
          result["elapsed_ms"] = static_cast<std::int64_t>(now - wait.started_ms);
          result["detail"] = std::format("已渲染 {} 帧", frames_seen);
          respond(*owner, wait.request_id, std::move(result));
          iterator = waits.erase(iterator);
          continue;
        }
      } else if (satisfied.has_value() && *satisfied) {
        Json result = Json::object();
        result["satisfied"] = true;
        result["elapsed_ms"] = 0;
        result["detail"] = std::format("条件满足: {}", wait.kind);
        respond(*owner, wait.request_id, std::move(result));
        iterator = waits.erase(iterator);
        continue;
      }
      if (now >= wait.deadline_ms) {
        Json result = Json::object();
        result["satisfied"] = false;
        result["elapsed_ms"] = static_cast<std::int64_t>(now - wait.started_ms);
        result["detail"] = std::format("等待超时: {}", wait.kind);
        respond(*owner, wait.request_id, std::move(result));
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
    // 鉴权门：hello 之前只允许 ping（便于探活）与 hello 本身。token 不匹配直接断连——
    // 未握手的连接没有资格调用任何方法（旧实现任何本地进程连上就能读屏/注入键鼠）。
    if (!client.greeted && method != "hello" && method != "ping") {
      fail(client, id, ErrorCode::Invalid, "必须先 hello（携带 token）");
      drop_client(client);
      return;
    }
    if (method == "hello" && !client.greeted) {
      // 鉴权开关：只有「显式设置且非空」的 token 才要求校验（空串 = 显式关闭鉴权）。
      const bool auth_required = options.token.has_value() && !options.token->empty();
      const std::string provided = json_get_string(params, "token");
      if (auth_required && provided != *options.token) {
        // token 错误：回错误帧后立即断连——未鉴权连接不允许逗留（探活用 ping 即可）。
        fail(client, id, ErrorCode::Invalid,
             "token 校验失败：请从控制文件读取 token 并在 hello 参数携带");
        drop_client(client);
        return;
      }
      client.greeted = true;
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
      client->id = next_client_id++;
      client->peer = stream->peer_address();
      client->stream = std::move(*stream);
      client->stream.set_nonblocking(true);
      client->stream.set_no_delay(true);   // 响应是小帧：Nagle 会让后续小包等确认，白添延迟
      log::info("控制通道：客户端接入 {} (#{})", client->peer, client->id);
      clients.push_back(std::move(client));
    }
  }

  void read_clients() {
    for (std::size_t index = 0; index < clients.size();) {
      Client& client = *clients[index];
      std::array<std::uint8_t, 8192> buffer{};
      bool closed = false;
      // dispatch 可能在**本轮迭代中途**回收本连接（鉴权失败/发送失败 → `drop_client`
      // 把元素从 `clients` 里 erase 掉）：此时 `client` 引用已悬垂，后续任何访问都是
      // use-after-free，包括“检查是否还在”本身（`client_by_id(client.id)` 要先读已释放的 id）。
      // 因此 id 必须在 dispatch **之前**拷出来，用拷贝判定存活。
      bool recycled = false;
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
      // 游标消费：帧解析不再从头 erase（每帧 O(n) 搬移），只在游标推进后一次性压实。
      while (client.buffer.size() - client.consumed >= 4) {
        const auto base = client.buffer.begin() + static_cast<std::ptrdiff_t>(client.consumed);
        const auto first = static_cast<std::uint8_t>(base[0]);
        const auto second = static_cast<std::uint8_t>(base[1]);
        const auto third = static_cast<std::uint8_t>(base[2]);
        const auto fourth = static_cast<std::uint8_t>(base[3]);
        const std::uint32_t length = (static_cast<std::uint32_t>(first) << 24U) |
                                     (static_cast<std::uint32_t>(second) << 16U) |
                                     (static_cast<std::uint32_t>(third) << 8U) |
                                     static_cast<std::uint32_t>(fourth);
        if (length == 0 || length > options.max_frame) {
          log::warn("控制通道：帧长度非法（{} 字节，上限 {}）来自 {}", length, options.max_frame,
                    client.peer);
          closed = true;
          break;
        }
        if (client.buffer.size() - client.consumed < static_cast<std::size_t>(length) + 4) break;
        const std::string body(base + 4,
                               base + 4 + static_cast<std::ptrdiff_t>(length));
        client.consumed += static_cast<std::size_t>(length) + 4;
        const std::uint64_t live_id = client.id;   // dispatch 之前拷出（见上方注释）
        dispatch_frame(client, body);
        // 已被回收：立即跳出，且**不得再触碰 client**
        if (!client_by_id(live_id)) {
          recycled = true;
          break;
        }
      }
      // 连接已被回收：它后面的客户端已前移到当前下标，因此**不 ++index**（否则会跳过一个），
      // 同时跳过下面所有对 `client` 的访问（缓冲区压实/断开口志/回收都已完成或不再适用）。
      if (recycled) continue;
      if (client.consumed > 0 && !client.buffer.empty()) {
        client.buffer.erase(client.buffer.begin(),
                            client.buffer.begin() + static_cast<std::ptrdiff_t>(
                                std::min(client.consumed, client.buffer.size())));
      }
      client.consumed = 0;
      if (closed) {
        log::info("控制通道：客户端断开 {}", client.peer);
        drop_client(client);
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
  }  if (method == "tree") {
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
    // 动作白名单：拼错动作名曾返回 handled=false 的「成功但没效果」——对自动化是假阴性反馈
    // （AI 以为触发了实际没有）。白名单外直接报 unsupported。
    // 注意与组件能力对齐：`dismiss`（Dialog）、`action`（Dialog 按钮序号）、`clear`（TextArea）、
    // `add_row`/`clear_rows`/`scroll_by`（Table）等由组件 `invoke_action` 实现——
    // 这里的名单是「协议层通用动作 + 组件常见动作」，不在名单内的会被拒。
    static constexpr std::string_view kActions[] = {"click",     "dblclick", "focus",
                                                    "blur",      "toggle",   "select",
                                                    "scroll_to", "submit",   "open",
                                                    "close",     "dismiss",  "action",
                                                    "activate",  "add_row",  "clear_rows",
                                                    "scroll_by", "clear"};
    const std::string requested_action = json_get_string(params, "action");
    if (!requested_action.empty()) {
      const bool known = std::find(std::begin(kActions), std::end(kActions), requested_action) !=
                         std::end(kActions);
      if (!known) {
        return unexpected(ErrorCode::Unsupported,
                          std::format("未知动作: {}（可用: click/dblclick/focus/blur/toggle/select/"
                                      "scroll_to/submit/open/close/dismiss/action 等；"
                                      "组件自定义动作见各组件 invoke 面）",
                                      requested_action));
      }
    }
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
    } else if (kind == "drag") {
      // §6.2 声明的 drag：合成 move→down→move(to)→up 序列（拖拽语义 = 按住起点拖到终点）。
      // 之前文档声明了但实现缺失（审视报告 P0-1）——AI 按文档写 drag 必失败。
      const double to_x = json_get_double(params, "to_x", 0.0);
      const double to_y = json_get_double(params, "to_y", 0.0);
      const math::Point from = event.position;
      const math::Point to{static_cast<float>(to_x), static_cast<float>(to_y)};
      Json seq = Json::array();
      auto step = [&](ui::EventKind kind_value, math::Point at) {
        ui::Event piece = event;
        piece.kind = kind_value;
        piece.position = at;
        const bool piece_handled = root.dispatch(piece);
        Json entry = Json::object();
        entry["kind"] = std::string(kind_value == ui::EventKind::MouseDown ? "down"
                              : kind_value == ui::EventKind::MouseMove ? "move" : "up");
        entry["handled"] = piece_handled;
        seq.push_back(std::move(entry));
      };
      step(ui::EventKind::MouseMove, from);
      step(ui::EventKind::MouseDown, from);
      // 中间插一步移动（拖拽控件往往在 move 路径上响应，不是只在 up 时）
      step(ui::EventKind::MouseMove, math::Point{(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f});
      step(ui::EventKind::MouseMove, to);
      step(ui::EventKind::MouseUp, to);
      host.request_repaint();
      Json result = Json::object();
      result["handled"] = true;
      result["kind"] = kind;
      result["from"] = bounds_to_json(math::Rect{from.x, from.y, 0, 0});
      result["to"] = bounds_to_json(math::Rect{to.x, to.y, 0, 0});
      result["steps"] = std::move(seq);
      if (ui::Element* target = root.hit_test(to); target != nullptr) {
        result["hit"] = ui::element_to_json(*target);
      }
      return result;
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
        // 指定了目标就必须真把文本送进去：目标不存在或不可聚焦时**明确报错**——
        // 默默不回，事件会落到旧焦点元素上（写错元素比报告失败危险得多）。
        ui::Element* element = find_element(target);
        if (element == nullptr) {
          return unexpected(ErrorCode::NotFound, std::format("未找到元素: {}", target));
        }
        if (!root.set_focus(element)) {
          return unexpected(ErrorCode::Invalid,
                            std::format("元素不可聚焦（focusable() == false），文本无法送达: {}",
                                        target));
        }
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
        // press = down + up 完整序列（旧实现只发 KeyDown：按住类语义（shift+拖拽、长按）
        // 与真实事件流不符，且按住状态会泄漏到后续事件——审视报告 P1-4）。
        event.kind = ui::EventKind::KeyDown;
        const bool down_handled = root.dispatch(event);
        event.kind = ui::EventKind::KeyUp;
        const bool up_handled = root.dispatch(event);
        host.request_repaint();
        Json result = Json::object();
        result["handled"] = down_handled || up_handled;
        const ui::Element* focused_now = root.focused();
        result["focused"] = focused_now != nullptr ? focused_now->derived_id() : std::string{};
        result["kind"] = "press";
        return result;
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
      const std::string path = json_get_string(params, "path");
      // 落盘白名单：capture 是「让应用进程写文件」的原语，不限制路径等于本地越权写
      // （审视报告 P0：客户端可指定任意 path 覆盖属主可写文件）。空 path = 应用自动命名
      // （落在自己的 shots 目录，安全）；显式 path 必须落在白名单目录内。
      if (!path.empty() && !capture_path_allowed(path)) {
        return unexpected(ErrorCode::Invalid,
                          std::format("截图落盘路径不在白名单目录内: {}（允许：temp、可执行文件目录与控制文件目录）",
                                      path));
      }
      auto saved = host.capture_to_file(path, region);
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
    // 渲染器必须可查：`auto` 是按实测选的，用户有权知道这一帧谁画的、以及为什么。
    result["renderer"] = metrics.renderer;
    result["renderer_note"] = metrics.renderer_note;
    result["headless"] = metrics.headless;
    result["device_scale"] = static_cast<double>(metrics.device_scale);
    result["physical_width"] = static_cast<std::uint64_t>(metrics.physical_width);
    result["physical_height"] = static_cast<std::uint64_t>(metrics.physical_height);
    result["uptime_ms"] = metrics.uptime_ms;
    result["frames"] = static_cast<std::uint64_t>(metrics.frames);
    result["last_frame_ms"] = metrics.last_frame_ms;
    result["layout_ms"] = metrics.layout_ms;
    result["paint_ms"] = metrics.paint_ms;
    result["present_ms"] = metrics.present_ms;
    result["frame_p50_ms"] = metrics.frame_p50_ms;
    result["frame_p95_ms"] = metrics.frame_p95_ms;
    result["nodes"] = static_cast<std::uint64_t>(metrics.nodes);
  // 上一帧实际绘制的元素数：视口剔除的效果**只能**这样观测——
  // 应用跨运行的像素不确定（动画冻结值依赖帧时序），截图 A/B 不可靠。
  result["painted_elements"] = static_cast<std::uint64_t>(host.root().painted_elements());
  // 增量重绘的可观测性：上一帧是否走局部路径、重绘区域多大（AI/性能回归都用得上）。
  result["partial_frame"] = host.root().last_frame_partial();
  {
    const math::IntRect painted = host.root().dirty_rect();
    Json rect = Json::object();
    rect["x"] = painted.x;
    rect["y"] = painted.y;
    rect["width"] = painted.width;
    rect["height"] = painted.height;
    result["dirty_rect"] = std::move(rect);
  }
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
        kind != "stable" && kind != "frames") {
      return unexpected(ErrorCode::Unsupported, std::format("不支持的等待条件: {}", kind));
    }
    PendingWait wait;
    wait.client = client.id;   // 稳定连接 id（非下标）
    wait.request_id = id;
    wait.kind = kind;
    wait.selector = json_get_string(params, "selector");
    wait.text = json_get_string(params, "text");
    wait.frames_target = frames_seen + static_cast<std::uint64_t>(
                             std::max<std::int64_t>(1, json_get_i64(params, "frames", 1)));
    const std::int64_t timeout = json_get_i64(params, "timeout_ms", 5000);
    wait.started_ms = time::now_ms();
    wait.deadline_ms = wait.started_ms + timeout;
    wait.stable_since_ms = time::now_ms();
    wait.last_version = root.version();
    if (kind == "element" || kind == "gone") {
      if (wait.selector.empty()) return unexpected(ErrorCode::Invalid, "wait 需要 selector");
    }
    if (kind == "text" || kind == "text_gone") {
      if (wait.text.empty()) return unexpected(ErrorCode::Invalid, "wait 需要 text");
    }
    if (kind == "frames") {
      const std::int64_t want = json_get_i64(params, "frames", 0);
      if (want <= 0) return unexpected(ErrorCode::Invalid, "wait for=frames 需要正的 frames 参数");
    }
    // 立即满足则直接返回，不再挂起
    if (kind == "stable" || kind == "frames") {
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
      ui::Element* element = find_element(target);
      if (element == nullptr) return unexpected(ErrorCode::NotFound, std::format("未找到元素: {}", target));
      if (!root.set_focus(element)) {
        return unexpected(ErrorCode::Invalid,
                          std::format("元素不可聚焦（focusable() == false）: {}", target));
      }
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
  // 鉴权 token：未显式设置（`nullopt`）→ 自动生成。时间戳纳秒 + 指针熵 + 序号的十六进制，
  // 不追求密码学强度（本地回环 + 控制文件分发），只要求不可猜。
  if (!impl_->options.token.has_value()) {
    const std::int64_t ns = time::now_ns();
    const void* entropy = static_cast<const void*>(&impl_);
    const auto mix = static_cast<std::uint64_t>(ns) ^
                     (std::hash<const void*>{}(entropy) << 17U) ^ (impl_->next_client_id << 33U);
    impl_->options.token = std::format("{:016x}", mix);
  }
  // 落盘白名单：显式列表优先，否则默认（temp + 可执行文件目录）。
  impl_->capture_dirs = options.capture_dirs.empty() ? options.default_capture_dirs()
                                                      : options.capture_dirs;
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
    // token 随控制文件分发：客户端（智能体/工具）从文件读出后在 hello 携带。
    // 显式关闭鉴权时不写该字段（客户端据此知道无需 token）。
    if (impl_->options.token.has_value() && !impl_->options.token->empty()) {
      info["token"] = *impl_->options.token;
    }
    if (auto status = json_write_file(options.control_file, info, true); !status) {
      return forward_error(status.error());
    }
  }
  const bool auth_on = impl_->options.token.has_value() && !impl_->options.token->empty();
  log::info("控制通道已启动 tcp://{}:{}{}", options.bind, impl_->listener.port(),
            auth_on ? "（需 token）" : "（鉴权关闭）");
  return impl_->listener.port();
}

void Server::stop() {
  if (!impl_ || !impl_->active) return;
  impl_->clients.clear();
  impl_->waits.clear();
  impl_->listener.close();
  impl_->active = false;
  // 退出时清理控制文件：残留文件会让客户端把死实例误判为可控制（审视报告 P0-2）。
  // best-effort：删除失败（被占用等）不阻断退出。
  if (!impl_->options.control_file.empty()) {
    (void)fs::remove_file(impl_->options.control_file);
  }
}

void Server::poll() {
  if (!impl_ || !impl_->active) return;
  ++impl_->frames_seen;   // 每帧推进（wait for=frames 的条件源）
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

auto Server::token() const -> std::string {
  if (!impl_ || !impl_->options.token.has_value()) return {};
  // 空串 = 显式关闭鉴权；对外同样表现为空串（与「未 start」不可区分，调用方无需区分）。
  return *impl_->options.token;
}

auto Server::client_count() const noexcept -> std::size_t {
  return impl_ ? impl_->clients.size() : 0;
}

auto Server::running() const noexcept -> bool { return impl_ && impl_->active; }

auto ServerOptions::default_capture_dirs() const -> std::vector<std::string> {
  std::vector<std::string> dirs;
  dirs.push_back(fs::temp_dir());
  // 可执行文件同目录（默认 shots 目录的蓄意写应用通常在这里）
  if (auto exe = process::executable_path(); exe) {
    const std::size_t slash = exe->find_last_of("\\/");
    if (slash != std::string::npos) dirs.push_back(exe->substr(0, slash));
  }
  // 控制文件所在目录：智能体会话目录通常在这里（客户端凭控制文件发现本服务，
  // 截图直落会话目录免二次搬运——否则工作流断一截：只能先落 temp 再搬）。
  // 持有控制文件 = 已持有 token（文件权限由 OS 管），放行该目录不扩大攻击面。
  if (!control_file.empty()) {
    const std::size_t slash = control_file.find_last_of("\\/");
    if (slash != std::string::npos) {
      std::string dir = control_file.substr(0, slash == 0 ? 1 : slash);
      if (dir.size() == 1 && (dir == "/" || dir == "\\")) {
        // 控制文件在根目录：只放行根目录本身（前缀比较语义即如此，无需转殊）
      }
      dirs.push_back(std::move(dir));
    } else {
      dirs.push_back(".");  // 相对路径无分隔符：当前目录
    }
  }
  return dirs;
}

}  // namespace st::control
