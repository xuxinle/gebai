#include "st/lsp/client.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <utility>

#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"

namespace st::lsp {

namespace {

/// 从路径算 `file://` URI（`path_to_uri` 的内部实现，也供 root_uri 用）。
[[nodiscard]] auto encode_uri_path(std::string_view path) -> std::string {
  std::string result;
  result.reserve(path.size() + 8);
  // 百分号转义：只转 URI 里的保留字符（不转 `/` 与常见路径字符，保持可读）。
  const auto needs_escape = [](unsigned char c) {
    if (std::isalnum(c) != 0) return false;
    switch (c) {
      case '/': case '-': case '_': case '.': case '~': case ':': return false;
      default: return true;
    }
  };
  for (const char raw : path) {
    const auto c = static_cast<unsigned char>(raw);
    if (needs_escape(c)) {
      result += std::format("%{:02X}", static_cast<unsigned>(c));
    } else {
      result += raw;
    }
  }
  return result;
}

/// 百分号解码（`uri_to_path` 用）。
[[nodiscard]] auto decode_uri_path(std::string_view text) -> std::string {
  std::string result;
  result.reserve(text.size());
  for (std::size_t at = 0; at < text.size(); ++at) {
    if (text[at] == '%' && at + 2 < text.size()) {
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int high = hex(text[at + 1]);
      const int low = hex(text[at + 2]);
      if (high >= 0 && low >= 0) {
        result += static_cast<char>((high << 4) | low);
        at += 2;
        continue;
      }
    }
    result += text[at];
  }
  return result;
}

/// 请求 id 生成：从 1 单调递增（0 保留给"未发出"）。
[[nodiscard]] auto next_request_id(std::atomic<std::int64_t>& counter) -> std::int64_t {
  return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

}  // namespace

// ———————————————————————————— diff 计算 ————————————————————————————

auto compute_single_change(std::string_view old_text, std::string_view new_text)
    -> std::optional<TextChange> {
  if (old_text == new_text) return std::nullopt;   // 没变：不该发通知

  // 公共前缀（**按 UTF-8 码点边界**推进，不切在多字节字符中间）。
  std::size_t prefix = 0;
  while (prefix < old_text.size() && prefix < new_text.size() &&
         old_text[prefix] == new_text[prefix]) {
    ++prefix;
  }
  // 回退到码点边界：若 `prefix` 落在某个字符的续字节上，退到该字符起点。
  while (prefix > 0 && prefix < old_text.size() &&
         (static_cast<unsigned char>(old_text[prefix]) & 0xC0U) == 0x80U) {
    --prefix;
  }
  while (prefix > 0 && prefix < new_text.size() &&
         (static_cast<unsigned char>(new_text[prefix]) & 0xC0U) == 0x80U) {
    --prefix;
  }

  // 公共后缀（同样不回退到半个码点）。
  std::size_t suffix = 0;
  while (suffix < old_text.size() - prefix && suffix < new_text.size() - prefix &&
         old_text[old_text.size() - 1 - suffix] == new_text[new_text.size() - 1 - suffix]) {
    ++suffix;
  }
  while (suffix > 0 &&
         (static_cast<unsigned char>(old_text[old_text.size() - suffix]) & 0xC0U) == 0x80U) {
    --suffix;
  }

  TextChange change{};
  change.is_full = false;
  change.range.start = offset_to_position(old_text, prefix);
  change.range.end = offset_to_position(old_text, old_text.size() - suffix);
  change.text = std::string(new_text.substr(prefix, new_text.size() - suffix - prefix));
  return change;
}

// ———————————————————————————— Impl ————————————————————————————

/// 一条待处理消息（读线程 → 主线程）。
struct IncomingMessage {
  Message message{};
};

struct LspClient::Impl {
  // —— 跨线程：管道句柄 ——
  process::StreamHandle process{};

  // —— 线程间队列（互斥量 + 条件变量；消息频率低，不上无锁） ——
  std::mutex queue_mutex{};
  std::condition_variable queue_cv{};
  std::vector<IncomingMessage> incoming{};        ///< 读线程 → 主线程
  std::deque<std::string> outgoing{};             ///< 主线程 → 写线程（已编码字节）
  bool outgoing_closed{false};
  bool reader_done{false};
  std::string reader_error{};

  std::jthread reader{};
  std::jthread writer{};

  // —— 仅主线程访问：状态机 ——
  ClientConfig config{};
  SessionState state{SessionState::Idle};
  std::string error{};
  ServerCapabilities capabilities{};
  std::string server_name{};
  std::string server_version{};
  std::int64_t init_deadline_ms{0};
  std::int64_t shutdown_deadline_ms{0};
  bool shutdown_sent{false};
  bool exit_sent{false};

  /// 已发出的请求：id → method（响应回来时要知道是哪个方法）。
  std::unordered_map<std::int64_t, std::string> pending_requests{};
  /// 已打开文档：uri → {version, text}。
  struct Document {
    std::int64_t version{0};
    std::string text{};
  };
  std::unordered_map<std::string, Document> documents{};

  std::atomic<std::int64_t> request_counter{0};

  /// 读线程：把管道字节喂给分帧器，消息进队列。
  ///
  /// 为什么不复用 `read_line`：LSP 不是按行的（消息体是 JSON，内部可能有换行），
  /// 且 `Content-Length` 明确给了字节数——按行读会把一条消息切碎。
  /// 这里用"块读 + 分帧器"，与协议层是同一套（`FrameReader`）。
  void reader_loop(std::stop_token stop) {
    FrameReader framer;
    std::array<char, 8192> buffer{};
    while (!stop.stop_requested()) {
      const auto count = process.read_some(buffer.data(), buffer.size());
      if (count == 0) break;   // EOF：server 退出
      framer.feed(std::string_view(buffer.data(), count));
      std::vector<IncomingMessage> batch;
      while (auto message = framer.next()) {
        batch.push_back(IncomingMessage{std::move(*message)});
      }
      if (!batch.empty()) {
        const std::scoped_lock guard(queue_mutex);
        for (auto& item : batch) incoming.push_back(std::move(item));
      }
      queue_cv.notify_one();
    }
    const std::scoped_lock guard(queue_mutex);
    reader_done = true;
    if (!framer.last_error().empty()) reader_error = framer.last_error();
    queue_cv.notify_one();
  }

  /// 写线程：把发送队列里的字节写进 stdin。
  void writer_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::string chunk;
      {
        std::unique_lock lock(queue_mutex);
        queue_cv.wait(lock, [this, &stop] {
          return !outgoing.empty() || outgoing_closed || stop.stop_requested();
        });
        if (outgoing.empty()) {
          if (outgoing_closed || stop.stop_requested()) break;
          continue;
        }
        chunk = std::move(outgoing.front());
        outgoing.pop_front();
      }
      if (!process.write(chunk)) {
        // 写失败 = 进程死了：如实记下（主线程 pump 会看到 reader_done + 状态转 Failed）。
        const std::scoped_lock guard(queue_mutex);
        reader_error = "写入语言服务器失败（进程可能已退出）";
        break;
      }
    }
  }

  /// 编码并入队（主线程调用；实际写由写线程做）。
  void enqueue(Message message) {
    const std::string bytes = encode_message(message);
    const std::scoped_lock guard(queue_mutex);
    outgoing.push_back(bytes);
    queue_cv.notify_one();
  }

  /// 状态转移的唯一入口（保证回调一定被通知）。
  ///
  /// ⚠ **不能读"pump 时拷的回调副本"**：`start()` 是同步路径（进程起不来时当场转
  /// `Failed`），而副本只在 `pump` 里刷新——于是"启动失败"这个最该通知用户的状态
  /// 反而不回调（实测：`notified` 断言失败）。
  /// 解法：Impl 持 owner 指针，直接读 `owner->on_state_change`。
  void set_state(SessionState next) {
    if (state == next) return;
    state = next;
    if (owner != nullptr && owner->on_state_change) owner->on_state_change(next);
  }

  /// 强制通知（幂等短路会吞掉"重试仍然失败"这种同状态转移——用户点了重连、
  /// 结果一点反馈都没有，看着像按钮坏了）。
  void force_state(SessionState next) {
    const bool same = state == next;
    state = next;
    if (!same && owner != nullptr && owner->on_state_change) {
      owner->on_state_change(next);
      return;
    }
    if (same && owner != nullptr && owner->on_state_change) owner->on_state_change(next);
  }

  /// 反向指针（构造时设；生命周期由 `LspClient` 保证长于 `Impl`）。
  LspClient* owner{nullptr};

  /// 处理一条消息（主线程）。返回是否已识别处理。
  void handle(Message& message) {
    if (message.is_response()) {
      const auto id = json_as_i64(message.id, 0);
      std::string method;
      if (const auto found = pending_requests.find(id); found != pending_requests.end()) {
        method = found->second;
        pending_requests.erase(found);
      }
      // 握手响应：转 Ready 或 Failed。
      if (method == "initialize") {
        if (message.is_error) {
          error = std::format("初始化失败：{}（code {}）", message.error_message,
                              message.error_code);
          set_state(SessionState::Failed);
          return;
        }
        parse_capabilities(message.body);
        set_state(SessionState::Ready);
        // 协议要求：`initialize` 响应后必须尽快发 `initialized` 通知，
        // 否则 server 会一直等（clangd 在收到前不发任何诊断）。
        enqueue(make_notification("initialized", Json::object()));
        // 已经 open 过的文档要重发（重连场景：server 是新的，它不知道任何文档）。
        resend_open_documents();
      } else if (method == "shutdown") {
        shutdown_sent = false;
        enqueue(make_notification("exit"));
        exit_sent = true;
        set_state(SessionState::Stopped);
      }
      if (response_handler) response_handler(id, method, message.body, message.is_error);
      return;
    }

    if (message.is_request()) {
      // ⚠ **服务端请求必须回复**，哪怕是空回复。不回的后果不是"少一个功能"——
      // clangd 会**阻塞等待**（它在 `workspace/configuration` / `client/registerCapability`
      // 上等服务端应答），表现为"诊断能收到（推送式）但 hover/补全永远没响应"，
      // 实测 hover 等满 30 秒超时。给用户回调优先，没人接就自动回空。
      if (server_request_handler) {
        server_request_handler(message);
        return;
      }
      if (message.method == "workspace/configuration") {
        // 配置查询：回等长的 null 数组（每个 item 一个 null）——比空数组更正确，
        // 某些 server 会按下标取值。
        std::size_t count = 0;
        if (const Json* items = json_find(message.body, "items");
            items != nullptr && items->is_array()) {
          count = items->size();
        }
        Json result = Json::array();
        for (std::size_t index = 0; index < count; ++index) result.push_back(Json());
        if (owner != nullptr) owner->reply(message, std::move(result));
        return;
      }
      if (message.method == "client/registerCapability" || message.method == "client/unregisterCapability" ||
          message.method == "window/workDoneProgress/create") {
        if (owner != nullptr) owner->reply(message, Json());
        return;
      }
      // 其它未知服务端请求：回"方法未实现"错误（**必须回**，不能让 server 白等）。
      if (owner != nullptr) {
        owner->reply_error(message, error_code::kMethodNotFound,
                           std::format("未实现的服务端请求：{}", message.method));
      }
      return;
    }

    // 通知。
    if (message.method == "textDocument/publishDiagnostics") {
      if (diagnostics_handler) {
        const std::string uri = json_get_string(message.body, "uri");
        std::vector<Diagnostic> list;
        if (const Json* items = json_find(message.body, "diagnostics");
            items != nullptr && items->is_array()) {
          list.reserve(items->size());
          for (const auto& item : *items) {
            Diagnostic diagnostic{};
            diagnostic.range = range_from_json(json_at(item, "range"));
            diagnostic.severity =
                static_cast<int>(json_get_i64(item, "severity", 1));
            diagnostic.message = json_get_string(item, "message");
            diagnostic.source = json_get_string(item, "source");
            if (const Json* code = json_find(item, "code"); code != nullptr) {
              diagnostic.code = code->is_string() ? code->get<std::string>() : json_dump(*code);
            }
            list.push_back(std::move(diagnostic));
          }
        }
        diagnostics_handler(uri, list);
      }
      return;
    }
    if (message.method == "window/logMessage" || message.method == "window/showMessage") {
      if (log_handler) {
        log_handler(static_cast<int>(json_get_i64(message.body, "type", 3)),
                    json_get_string(message.body, "message"));
      }
      return;
    }
    if (unhandled_handler) unhandled_handler(message.method, message.body);
  }

  void parse_capabilities(const Json& result) {
    const Json& server_info = json_at(result, "serverInfo");
    server_name = json_get_string(server_info, "name");
    server_version = json_get_string(server_info, "version");
    const Json& caps = json_at(result, "capabilities");
    const auto has = [&caps](std::string_view key) {
      const Json* value = json_find(caps, key);
      return value != nullptr && !value->is_null() && *value != false;
    };
    capabilities.completion = json_get_bool(caps, "completionProvider", false) || has("completionProvider");
    capabilities.hover = has("hoverProvider");
    capabilities.definition = has("definitionProvider");
    capabilities.references = has("referencesProvider");
    capabilities.document_symbol = has("documentSymbolProvider");
    capabilities.workspace_symbol = has("workspaceSymbolProvider");
    if (const Json& completion = json_at(caps, "completionProvider");
        completion.is_object()) {
      capabilities.completion_resolve = completion.contains("resolveProvider") &&
                                        json_get_bool(completion, "resolveProvider");
      capabilities.completion_trigger_characters =
          json_get_string_array(completion, "triggerCharacters");
    }
  }

  void resend_open_documents() {
    for (const auto& [uri, document] : documents) {
      Json params = Json::object();
      Json item = Json::object();
      item["uri"] = uri;
      item["languageId"] = "";   // 重发用空（server 已知道？不，它不知道——但 languageId 影响解析）
      item["version"] = document.version;
      item["text"] = document.text;
      params["textDocument"] = std::move(item);
      enqueue(make_notification("textDocument/didOpen", std::move(params)));
    }
  }

  // 回调副本（主线程从 client 的 std::function 拷进来，避免 pump 时被外部改）。
  std::function<void(std::int64_t, const std::string&, const Json&, bool)> response_handler{};
  std::function<void(const Message&)> server_request_handler{};
  std::function<void(const std::string&, const std::vector<Diagnostic>&)> diagnostics_handler{};
  std::function<void(int, const std::string&)> log_handler{};
  std::function<void(const std::string&, const Json&)> unhandled_handler{};
};

auto LspClient::client_capabilities() -> Json {
  // **只声明我们真会用的能力**——声明了却不实现比不声明更糟：server 会等服务端的
  // 配合消息（如声明 `didSave` 却从不发，clangd 的诊断链会卡在半途）。
  Json caps = Json::object();
  Json text_document = Json::object();
  Json synchronization = Json::object();
  // 增量同步：阶段 2 已实现（`did_change` 带 range）——声明从简（打开/变更/关闭）。
  synchronization["dynamicRegistration"] = false;
  synchronization["willSave"] = false;
  synchronization["willSaveWaitUntil"] = false;
  synchronization["didSave"] = true;   // 保存后 server 可重读磁盘（clangd 要这个）
  text_document["synchronization"] = std::move(synchronization);
  // 语义高亮暂不做（阶段 3 之后再说）；声明了 server 会推一堆我们用不上的数据。
  text_document["publishDiagnostics"] = [] {
    Json value = Json::object();
    value["relatedInformation"] = true;
    value["versionSupport"] = true;   // 诊断带 version：能丢弃过期诊断
    return value;
  }();
  caps["textDocument"] = std::move(text_document);
  Json workspace = Json::object();
  workspace["workspaceFolders"] = true;
  workspace["configuration"] = false;   // 我们不接 `workspace/configuration` 请求（会回空）
  workspace["applyEdit"] = false;       // 不让 server 直接改文件（安全侧）
  caps["workspace"] = std::move(workspace);
  return caps;
}

LspClient::LspClient() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
LspClient::~LspClient() {
  stop();
}

auto LspClient::path_to_uri(std::string_view path) -> std::string {
  if (path.empty()) return {};
  std::string result = "file://";
  if (path.front() != '/' && path.front() != '\\') {
    // 相对路径：转成绝对（URI 必须绝对）。
    if (const auto absolute = st::fs::absolute(path); absolute.has_value()) {
      static_cast<void>(0);
      // 复用下方逻辑处理绝对路径（避免递归：这里直接拼）。
      const std::string& abs = *absolute;
      if (!abs.empty() && abs.front() != '/') result += "/";
      result += encode_uri_path(abs);
      return result;
    }
  }
#if defined(_WIN32)
  // Windows 盘符：`C:\a\b` → `file:///C:/a/b`（多一个斜杠）。
  if (path.size() >= 2 && path[1] == ':') result += "/";
#endif
  std::string normalized(path);
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  result += encode_uri_path(normalized);
  return result;
}

auto LspClient::uri_to_path(std::string_view uri) -> std::string {
  constexpr std::string_view kPrefix = "file://";
  if (uri.rfind(kPrefix, 0) != 0) return {};
  std::string rest = decode_uri_path(uri.substr(kPrefix.size()));
  // `file:///C:/a` 的第三个斜杠是被我们加上的盘符前导斜杠，去掉。
  if (rest.size() >= 3 && rest[0] == '/' && rest[2] == ':') {
    rest.erase(0, 1);
  }
  std::replace(rest.begin(), rest.end(), '/', static_cast<char>(
#if defined(_WIN32)
      '\\'
#else
      '/'
#endif
  ));
  return rest;
}

void LspClient::start(ClientConfig config) {
  if (impl_->state != SessionState::Idle && impl_->state != SessionState::Failed &&
      impl_->state != SessionState::Stopped) {
    return;   // 已在运行/启动中：不重复起
  }
  impl_->config = std::move(config);
  impl_->error.clear();
  impl_->capabilities = ServerCapabilities{};
  impl_->pending_requests.clear();
  impl_->shutdown_sent = false;
  impl_->exit_sent = false;
  {
    const std::scoped_lock guard(impl_->queue_mutex);
    impl_->incoming.clear();
    impl_->outgoing.clear();
    impl_->outgoing_closed = false;
    impl_->reader_done = false;
    impl_->reader_error.clear();
  }

  if (impl_->config.program.empty()) {
    impl_->error = "未指定语言服务器程序";
    impl_->force_state(SessionState::Failed);
    return;
  }
  impl_->process.open(impl_->config.program, impl_->config.args, impl_->config.root_path, true);
  if (!impl_->process.valid()) {
    impl_->error = std::format("无法启动 '{}'：{}", impl_->config.program,
                               impl_->process.error());
    impl_->force_state(SessionState::Failed);
    return;
  }
  // 线程启动顺序：先读后写（读线程可能立刻收到 server 的早期消息）。
  impl_->reader = std::jthread([this](std::stop_token stop) { impl_->reader_loop(stop); });
  impl_->writer = std::jthread([this](std::stop_token stop) { impl_->writer_loop(stop); });
  impl_->set_state(SessionState::Starting);

  // —— `initialize` 请求 ——
  Json params = Json::object();
  params["processId"] = static_cast<std::int64_t>(st::process::current_id());
  Json client_info = Json::object();
  client_info["name"] = "shuangtian";
  client_info["version"] = "0.1.0";
  params["clientInfo"] = std::move(client_info);
  params["rootUri"] = impl_->config.root_uri.empty()
                          ? (impl_->config.root_path.empty()
                                 ? Json()
                                 : Json(path_to_uri(impl_->config.root_path)))
                          : Json(impl_->config.root_uri);
  // `rootPath`（已废弃但老 server 仍读）只在有根时给。
  if (!impl_->config.root_path.empty()) {
    params["rootPath"] = impl_->config.root_path;
  }
  params["capabilities"] = client_capabilities();
  if (!impl_->config.initialization_options.is_null()) {
    params["initializationOptions"] = impl_->config.initialization_options;
  }
  // `workspaceFolders`：有根才报（空数组比缺字段更明确，但空根时就别报）。
  if (!impl_->config.root_path.empty()) {
    Json folders = Json::array();
    Json folder = Json::object();
    folder["uri"] = path_to_uri(impl_->config.root_path);
    folder["name"] = st::fs::file_name(impl_->config.root_path);
    folders.push_back(std::move(folder));
    params["workspaceFolders"] = std::move(folders);
  }

  const std::int64_t id = next_request_id(impl_->request_counter);
  impl_->pending_requests.emplace(id, "initialize");
  impl_->init_deadline_ms = st::time::now_ms() + impl_->config.init_timeout_ms;
  impl_->enqueue(make_request(id, "initialize", std::move(params)));
}

auto LspClient::pump() -> std::size_t {
  // 回调先拷进 Impl：`pump` 里用户回调可能重设 `client.on_*`（如切换页面），
  // 直接读 `this->on_xxx` 会在回调链中途换掉正在用的那个。
  impl_->response_handler = on_response;
  impl_->server_request_handler = on_server_request;
  impl_->diagnostics_handler = on_diagnostics;
  impl_->log_handler = on_log;
  impl_->unhandled_handler = on_unhandled;

  std::vector<IncomingMessage> batch;
  bool reader_finished = false;
  std::string reader_error;
  {
    const std::scoped_lock guard(impl_->queue_mutex);
    batch.swap(impl_->incoming);
    reader_finished = impl_->reader_done;
    reader_error = impl_->reader_error;
  }
  for (auto& item : batch) {
    impl_->handle(item.message);
  }

  // 超时检查：握手里 server 不回 = 卡死（如实报，不无限等）。
  const std::int64_t now = st::time::now_ms();
  if (impl_->state == SessionState::Starting && impl_->init_deadline_ms > 0 &&
      now > impl_->init_deadline_ms) {
    impl_->error = std::format("语言服务器初始化超时（{} ms，无响应）",
                               impl_->config.init_timeout_ms);
    impl_->set_state(SessionState::Failed);
  }

  // 退出检测：读线程结束（EOF）= 进程退出。未主动 shutdown 就是异常退出。
  if (reader_finished && impl_->state != SessionState::Stopped &&
      impl_->state != SessionState::Failed) {
    if (impl_->state == SessionState::ShuttingDown) {
      impl_->set_state(SessionState::Stopped);
    } else {
      impl_->error = reader_error.empty()
                         ? "语言服务器进程已退出"
                         : std::format("语言服务器进程已退出：{}", reader_error);
      impl_->set_state(SessionState::Failed);
    }
  }
  return batch.size();
}

auto LspClient::stop(std::int64_t timeout_ms) -> void {
  if (impl_ == nullptr) return;
  const bool running = impl_->process.valid();
  if (!running) {
    impl_->state = SessionState::Stopped;
    return;
  }
  // 优雅收尾：`shutdown`（请求）→ 等响应 → `exit`（通知）。
  if (impl_->state == SessionState::Ready || impl_->state == SessionState::Starting) {
    const std::int64_t id = next_request_id(impl_->request_counter);
    impl_->pending_requests.emplace(id, "shutdown");
    impl_->shutdown_deadline_ms = st::time::now_ms() + timeout_ms;
    impl_->set_state(SessionState::ShuttingDown);
    impl_->enqueue(make_request(id, "shutdown"));
  }
  // 有界等待：pump 里会把 `exit` 发出去；到点就硬收。
  const std::int64_t deadline = st::time::now_ms() + timeout_ms;
  while (st::time::now_ms() < deadline) {
    (void)pump();
    if (impl_->state == SessionState::Stopped) break;
    if (impl_->exit_sent) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!impl_->exit_sent && impl_->state != SessionState::Failed) {
    impl_->enqueue(make_notification("exit"));
    impl_->exit_sent = true;
  }
  // 停线程 → 收进程。
  impl_->reader.request_stop();
  impl_->writer.request_stop();
  {
    const std::scoped_lock guard(impl_->queue_mutex);
    impl_->outgoing_closed = true;
    impl_->queue_cv.notify_all();
  }
  impl_->process.terminate();
  if (impl_->reader.joinable()) impl_->reader.join();
  if (impl_->writer.joinable()) impl_->writer.join();
  (void)impl_->process.finish();
  impl_->state = SessionState::Stopped;
}

auto LspClient::status() const -> SessionStatus {
  SessionStatus status{};
  status.state = impl_->state;
  status.error = impl_->error;
  status.server_name = impl_->server_name;
  status.server_version = impl_->server_version;
  status.supports_completion = impl_->capabilities.completion;
  status.supports_hover = impl_->capabilities.hover;
  status.supports_definition = impl_->capabilities.definition;
  status.supports_references = impl_->capabilities.references;
  status.supports_document_symbol = impl_->capabilities.document_symbol;
  status.supports_diagnostics = impl_->capabilities.diagnostics;
  status.supports_workspace_symbol = impl_->capabilities.workspace_symbol;
  status.pending_requests = static_cast<std::int64_t>(impl_->pending_requests.size());
  status.open_documents = static_cast<std::int64_t>(impl_->documents.size());
  return status;
}

auto LspClient::state() const -> SessionState { return impl_->state; }

auto LspClient::capabilities() const -> ServerCapabilities { return impl_->capabilities; }

auto LspClient::error() const -> std::string { return impl_->error; }

void LspClient::did_open(std::string uri, std::string language_id, std::string text) {
  if (const auto found = impl_->documents.find(uri); found != impl_->documents.end()) {
    did_close(uri);   // 重复打开：先关（server 侧同一 uri 只应有一份）
  }
  Impl::Document document{};
  document.version = 1;
  document.text = text;
  impl_->documents.emplace(uri, document);

  Json params = Json::object();
  Json item = Json::object();
  item["uri"] = uri;
  item["languageId"] = std::move(language_id);
  item["version"] = document.version;
  item["text"] = std::move(text);
  params["textDocument"] = std::move(item);
  impl_->enqueue(make_notification("textDocument/didOpen", std::move(params)));
}

void LspClient::did_change(std::string_view uri, const std::vector<TextChange>& changes,
                           std::string full_text) {
  const auto found = impl_->documents.find(std::string(uri));
  if (found == impl_->documents.end()) return;   // 未打开：忽略（不是错误，是用途错）
  found->second.version += 1;
  found->second.text = std::move(full_text);

  Json params = Json::object();
  Json item = Json::object();
  item["uri"] = std::string(uri);
  item["version"] = found->second.version;
  Json list = Json::array();
  for (const auto& change : changes) {
    Json entry = Json::object();
    if (!change.is_full) entry["range"] = range_to_json(change.range);
    entry["text"] = change.text;
    list.push_back(std::move(entry));
  }
  item["contentChanges"] = std::move(list);
  params["textDocument"] = std::move(item);
  impl_->enqueue(make_notification("textDocument/didChange", std::move(params)));
}

void LspClient::did_close(std::string_view uri) {
  const auto found = impl_->documents.find(std::string(uri));
  if (found == impl_->documents.end()) return;
  impl_->documents.erase(found);
  Json params = Json::object();
  Json item = Json::object();
  item["uri"] = std::string(uri);
  params["textDocument"] = std::move(item);
  impl_->enqueue(make_notification("textDocument/didClose", std::move(params)));
}

auto LspClient::document_version(std::string_view uri) const -> std::int64_t {
  const auto found = impl_->documents.find(std::string(uri));
  return found == impl_->documents.end() ? 0 : found->second.version;
}

auto LspClient::is_open(std::string_view uri) const -> bool {
  return impl_->documents.contains(std::string(uri));
}

auto LspClient::document_text(std::string_view uri) const -> std::string {
  const auto found = impl_->documents.find(std::string(uri));
  return found == impl_->documents.end() ? std::string{} : found->second.text;
}

auto LspClient::request(std::string method, Json params) -> std::int64_t {
  if (impl_->state != SessionState::Ready) return 0;   // 未就绪：如实返回 0，不假装发出去
  const std::int64_t id = next_request_id(impl_->request_counter);
  impl_->pending_requests.emplace(id, method);
  impl_->enqueue(make_request(id, std::move(method), std::move(params)));
  return id;
}

void LspClient::notify(std::string method, Json params) {
  impl_->enqueue(make_notification(std::move(method), std::move(params)));
}

void LspClient::cancel(std::int64_t request_id) {
  if (request_id <= 0) return;
  Json params = Json::object();
  params["id"] = request_id;
  notify("$/cancelRequest", std::move(params));
}

void LspClient::reply(const Message& request_message, Json result) {
  impl_->enqueue(make_response(request_message.id, std::move(result)));
}

void LspClient::reply_error(const Message& request_message, int code, std::string message) {
  impl_->enqueue(make_error_response(request_message.id, code, std::move(message)));
}

}  // namespace st::lsp
