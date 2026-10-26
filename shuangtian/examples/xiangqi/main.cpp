/// 相期 · agent 控制的本地桌面常驻应用（**零 C++ UI 代码**）。
///
/// ## 这个应用检验什么
///
/// 前面几个示例检验"能不能把应用做出来"；相期检验的是另一种东西：
/// **能不能把「需要人介入的时刻」从终端搬到桌面上**。
///
/// agent 在本机跑着，迟早会遇到三类时刻：
///
/// | 时刻 | 相期的形态 | 能否忽略 |
/// |---|---|---|
/// | 有事要告诉你 | `notice` 提示卡 | 能（批量忽略） |
/// | 要你做一个决定 | `choice` 选择卡 | **不能**（必须有答案） |
/// | 要你去干一件事 | `todo` 待办卡 | **不能**（必须有结果） |
///
/// 后两类不允许忽略：那两类的价值就是"必须有结果"。允许忽略等于允许 agent
/// 的请求静默消失——**比不弹出来更糟**（它会以为你收到并默许了）。
///
/// ## 传输：WebSocket（客户端）
///
/// 相期是**客户端**，连 agent 控制器：
///
/// ```
///   相期 ──ws://host:port/xiangqi ──▶ agent 控制器
///         ◀── {"type":"cards","cards":[…]}      下发卡片（权威列表）
///         ◀── {"type":"card", …}                单张新增/更新
///         ◀── {"type":"dismiss","id":…}         agent 自己撤回
///         ── {"type":"answer","id":…,…} ──▶     回传答复
/// ```
///
/// 为什么用 WebSocket 而不是文件投递：
/// - **双向**：答复即时回到 agent，而不是等它轮询 outbox
/// - **有连接状态**：断线能被感知并显示（文件投递做不到「它还在不在」）
/// - **低延迟**：选择类交互对延迟敏感（人点完就想看到反应）
///
/// 文件投递**保留为降级路径**：WS 连不上时仍可从 `inbox/` 读卡片、答复仍写
/// `outbox/`。理由：agent 可能还没起、可能不支持 WS——那时让人完全看不到提醒，
/// 比降级严重得多。
///
/// ## 断线不自动重连（刻意的）
///
/// 断线把状态显式显示为「未连接」，不做静默自动重连。
/// 常驻应用里"看起来连着其实早就断了"是最坏的状态——用户以为 agent 在盯着，
/// 实际提醒根本没送到。显式断开 + 明确的连接指示让这个问题不可能被忽略。
///
/// ## 与 xmlapp 同一开发方式
///
/// 界面结构在 `ui/main.xml`，界面逻辑在 `ui/app.js`；本文件只做三件事：
/// 装配、经 `ScriptBridge::expose` 暴露能力、装载。
///
/// ## 托盘图标（未做，需先补框架能力）
///
/// 「任务栏小图标常驻」当前做不到：霜天没有托盘图标能力
/// （`docs/BACKLOG.md:5543` 自己列着「系统托盘」待做）。
/// 现状替代：窗口常驻 + 最小化到任务栏。真托盘需要框架补
/// win32 `Shell_NotifyIcon` + `st::ui` 平台无关抽象。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/app/app.hpp"
#include "st/codec/png.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/net.hpp"
#include "st/core/platform.hpp"
#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/core/json.hpp"
#include "st/net/ws.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/hot_reload.hpp"
#include "st/ui/script_bridge.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/xml_ui.hpp"

namespace {

using st::Json;

constexpr const char* KIND_NOTICE = "notice";
constexpr const char* KIND_CHOICE = "choice";
constexpr const char* KIND_TODO = "todo";

/// 一张卡片。
struct Card {
  std::string id;
  std::string kind{KIND_NOTICE};
  std::string title;
  std::string body;
  std::string from;
  std::string due;
  Json options{Json::array()};

  auto as_json() const -> Json {
    Json out{{"id", id}, {"kind", kind}, {"title", title}, {"body", body}};
    if (!from.empty()) out["from"] = from;
    if (!due.empty()) out["due"] = due;
    if (options.is_array() && !options.empty()) out["options"] = options;
    return out;
  }

  static auto from_json(const Json& obj) -> Card {
    Card card;
    if (obj.contains("id")) card.id = st::json_as_string(obj["id"]);
    if (obj.contains("kind")) card.kind = st::json_as_string(obj["kind"]);
    if (obj.contains("title")) card.title = st::json_as_string(obj["title"]);
    if (obj.contains("body")) card.body = st::json_as_string(obj["body"]);
    if (obj.contains("from")) card.from = st::json_as_string(obj["from"]);
    if (obj.contains("due")) card.due = st::json_as_string(obj["due"]);
    if (obj.contains("options") && obj["options"].is_array()) card.options = obj["options"];
    return card;
  }
};

/// `WsSession::Stream` 到 `TcpStream` 的适配器。
///
/// 为何需要这层：`WsSession` 的 `Stream` 接口用的是 `int`（-1 = 错），
/// 而 `TcpStream::read_some` 返回 `Result<size_t>`。两边语义相近但形状不同：
/// 直接改框架会把"错误可携带原因"压成 -1（那是**倒退**），所以在应用侧适配。
///
/// 这个类只做类型搬运，不做任何协议判断——协议全在 `WsSession` 里。
class TcpStreamAdapter : public st::net::ws::WsSession::Stream {
 public:
  explicit TcpStreamAdapter(st::net::TcpStream& tcp) : tcp_(tcp) {}

  /// 预置字节：握手时多读到的那部分 WS 帧（见 `connect()` 里的说明）。
  ///
  /// 为什么放在适配器而不是环境变量/全局：它是**这一个连接**的状态，
  /// 放在流上才能保证多个连接互不干扰。
  void preload(std::string bytes) { pending_ = std::move(bytes); }

  auto write_all(std::span<const std::uint8_t> data) -> bool override {
    return static_cast<bool>(tcp_.write_all(data));
  }

  auto read_some(std::span<std::uint8_t> buffer) -> int override {
    // 预置字节优先：它们是握手应答后面紧跟的帧数据，本来已在 socket 缓冲区外
    if (!pending_.empty()) {
      const std::size_t n = std::min(buffer.size(), pending_.size());
      std::memcpy(buffer.data(), pending_.data(), n);
      pending_.erase(0, n);
      return static_cast<int>(n);
    }
    const auto n = tcp_.read_some(buffer);
    if (!n.has_value()) return -1;              // 错误与 EOF 在这里归为"不可继续读"
    return static_cast<int>(*n);                // 0 = 对端关闭（与接口约定一致）
  }

  /// 预置字节是否还没被解码器取完（`poll` 据此判断要不要无条件探一把）。
  [[nodiscard]] auto has_pending() const -> bool { return !pending_.empty(); }

 private:
  st::net::TcpStream& tcp_;
  std::string pending_{};
};

/// agent 控制器连接（WebSocket 客户端）。
///
/// 线程模型：**全部在主循环里跑**（`try_read_message` 非阻塞）。
/// 为什么不开线程：单开线程后所有卡片变更都要加锁，而卡片本来就是每秒最多
/// 几次的规模——为它引入并发是买麻烦。
class AgentLink {
 public:
  AgentLink() = default;
  ~AgentLink() { disconnect(); }

  AgentLink(const AgentLink&) = delete;
  auto operator=(const AgentLink&) -> AgentLink& = delete;

  [[nodiscard]] auto connected() const -> bool { return session_ != nullptr; }

  /// 连接状态的一句话描述（界面直接显示——这是"它还在不在"的唯一来源）。
  [[nodiscard]] auto status_text() const -> std::string {
    if (connected()) return conn_name_;
    return last_error_.empty() ? "未连接（agent 未运行）" : last_error_;
  }

  /// 发起连接。`url` 形如 `ws://127.0.0.1:7717/xiangqi`。
  ///
  /// 失败**不抛**（返回 false + 记 last_error）：常驻应用启动时 agent 很可能
  /// 还没起来，那是正常情况——直接崩掉比连不上更糟。
  auto connect(const std::string& url) -> bool {
    disconnect();
    const auto parsed = parse_ws_url(url);
    if (!parsed.has_value()) {
      last_error_ = "地址格式不对：" + url + "（应为 ws://host:port/path）";
      return false;
    }
    const auto& target = *parsed;
    auto tcp = st::net::connect_tcp(target.host, target.port, 2000);
    if (!tcp.has_value()) {
      last_error_ = std::format("连不上 {}:{} — {}", target.host, target.port, tcp.error().message);
      return false;
    }
    tcp_ = std::make_unique<st::net::TcpStream>(std::move(*tcp));
    tcp_->set_no_delay(true);

    // 握手：写请求 → 读到空行为止 → 校验 101 + Sec-WebSocket-Accept
    const std::string key = st::net::ws::make_handshake_key();
    const std::string request = st::net::ws::client_handshake_request(target.host, target.port, target.path, key);
    const auto write_status = tcp_->write_text(request);
    if (!write_status) {
      last_error_ = "握手写入失败";
      tcp_.reset();
      return false;
    }
    std::string response;
    std::uint8_t buffer[1024];
    while (response.find("\r\n\r\n") == std::string::npos) {
      const auto n = tcp_->read_some(buffer);
      if (!n.has_value() || *n == 0) break;
      response.append(reinterpret_cast<const char*>(buffer), *n);
      if (response.size() > 16384) break;   // 防对端刷屏
    }
    if (!st::net::ws::validate_server_handshake(response, key)) {
      last_error_ = "握手被拒（对端不是 WebSocket 或路径不对）";
      tcp_.reset();
      return false;
    }

    // ⚠ 关键：握手应答与第一帧数据**很可能在同一个 TCP 包**里到达。
    // 上面的 read_some 会把 `\r\n\r\n` 之后的字节一起读进来——那部分就是 WS 帧，
    // 如果直接丢掉，解码器永远看不到它们，表现为「连接成功但永远收不到消息」
    // （实测踩到：心跳日志一直是 处理=0，而链路上什么都没错）。
    // 因此把余下字节喂回解码器。
    const auto header_end = response.find("\r\n\r\n");
    const std::size_t leftover_offset = header_end + 4;
    std::string leftover = (leftover_offset < response.size()) ? response.substr(leftover_offset) : std::string{};

    adapter_ = std::make_unique<TcpStreamAdapter>(*tcp_);
    adapter_->preload(std::move(leftover));
    session_ = std::make_unique<st::net::ws::WsSession>(*adapter_, /*client_side=*/true);
    conn_name_ = std::format("已连接 {}", url);
    last_error_.clear();
    return true;
  }

  /// 断开（显式；界面会因此显示"未连接"）。
  void disconnect() {
    if (session_) {
      (void)session_->send_close(1000, "bye");
      session_.reset();
    }
    adapter_.reset();   // 必须在 tcp_ 之前释放（它持着 tcp_ 的引用）
    if (tcp_) {
      tcp_->close();
      tcp_.reset();
    }
  }

  /// 发一条 JSON。
  auto send(const Json& payload) -> bool {
    if (!session_) return false;
    const std::string text = st::json_dump(payload);
    if (!session_->send_text(text)) {
      last_error_ = "发送失败（连接已断）";
      disconnect();
      return false;
    }
    return true;
  }

  /// 非阻塞取一条入站消息（主循环每帧调一次）。
  ///
  /// 返回 `nullopt` 表示"这次没有"，而不是"断了"——断开靠 `connected()` 判断。
  /// 这个区分很重要：把"暂时没消息"当成"断开"会让界面疯狂闪烁。
  ///
  /// ⚠ **必须先 `wait_readable(0)` 探测**：`try_read_message` 只消费**已解析的帧**，
  /// 它本身不读 socket（底层 `pump_frames` 是阻塞 read）。直接调 `try_read_message`
  /// 的结果是永远拿不到任何消息——而现象是"WS 连上了但卡片不出现"，
  /// 且没有任何报错（实测踩到：表现为连接状态正常、卡片永远为空）。
  auto poll() -> std::optional<Json> {
    if (!session_ || !tcp_) return std::nullopt;
    // 有预置字节时必须无条件读一把（它们不在 socket 里，wait_readable 看不到）
    if (adapter_ == nullptr || !adapter_->has_pending()) {
      // 0 = 不等：无数据立即返回；有数据时下面的读不会阻塞
      const auto ready = tcp_->wait_readable(0);
      if (!ready.has_value()) {
        last_error_ = "连接异常（等待可读失败）";
        disconnect();
        return std::nullopt;
      }
      ++poll_probes_;
      if (!*ready) return std::nullopt;
      ++poll_readable_;
    }

    // ⚠ 必须用 `read_message()` 而不是 `try_read_message()`：
    // 后者只消费 `pending_frames_`（已解析的帧），**自身不读 socket**；
    // 而能进到这里说明 `wait_readable` 已确认有数据，`read_message` 不会阻塞。
    // 实测症状：可读计数一路涨（pong/控制帧在），但应用消息永远取不到。
    const auto message = session_->read_message();
    if (!message.has_value()) {
      // 读到 EOF / 半帧：EOF 必须立刻断开，否则主循环会在"可读但读不出"上忙转
      // （实测症状：CPU 打满、内存疯狂增长直到 std::bad_alloc）。
      last_error_ = session_->close_received() ? "对端已关闭连接" : "连接已结束（读取未返回消息）";
      disconnect();
      return std::nullopt;
    }
    if (message->opcode == st::net::ws::Opcode::Close) {
      last_error_ = "对端关闭连接" + (message->text.empty() ? std::string{} : "：" + message->text);
      disconnect();
      return std::nullopt;
    }
    if (message->opcode != st::net::ws::Opcode::Text) return std::nullopt;
    if (const auto parsed = st::json_parse(message->text); parsed.has_value()) return *parsed;
    return std::nullopt;
  }

  [[nodiscard]] auto last_error() const -> std::string { return last_error_; }
  [[nodiscard]] auto probe_count() const -> int { return poll_probes_; }
  [[nodiscard]] auto readable_count() const -> int { return poll_readable_; }

 private:
  struct WsUrl {
    std::string host;
    std::uint16_t port{0};
    std::string path{"/"};
  };

  /// 解析 `ws://host:port/path`。
  ///
  /// 只支持 `ws://`：本机 agent 控制器走环回，TLS 没有收益却要接证书校验与
  /// 平台分支。要连远端时再加（那时才真的需要 TLS）。
  static auto parse_ws_url(const std::string& url) -> std::optional<WsUrl> {
    constexpr std::string_view prefix = "ws://";
    if (url.rfind(prefix, 0) != 0) return std::nullopt;
    const std::string rest = url.substr(prefix.size());
    const auto slash = rest.find('/');
    const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    WsUrl out;
    out.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos) {
      out.host = authority;
      out.port = 80;
    } else {
      out.host = authority.substr(0, colon);
      const auto port_text = authority.substr(colon + 1);
      try {
        const int port = std::stoi(port_text);
        if (port <= 0 || port > 65535) return std::nullopt;
        out.port = static_cast<std::uint16_t>(port);
      } catch (...) {
        return std::nullopt;
      }
    }
    if (out.host.empty()) return std::nullopt;
    return out;
  }

  std::unique_ptr<st::net::TcpStream> tcp_{};
  std::unique_ptr<TcpStreamAdapter> adapter_{};
  std::unique_ptr<st::net::ws::WsSession> session_{};
  std::string conn_name_{};
  std::string last_error_{};
  int poll_probes_{0};
  int poll_readable_{0};
};

/// 卡片存储 + 答复通道（含文件降级路径）。
struct CardStore {
  std::vector<Card> cards{};
  std::string home;
  AgentLink* link{nullptr};
  st::app::Application* app{nullptr};   ///< 窗口尺寸切换用（可为空：无头/测试）
  std::uint64_t seq{0};

  auto inbox_dir() const -> std::string { return st::fs::join(home, "inbox"); }
  auto outbox_dir() const -> std::string { return st::fs::join(home, "outbox"); }
  auto seen_path() const -> std::string { return st::fs::join(home, "seen.txt"); }
  std::vector<std::string> seen{};

  auto as_json() const -> Json {
    Json out = Json::array();
    for (const auto& card : cards) out.push_back(card.as_json());
    return out;
  }

  void load_seen() {
    const auto text = st::fs::read_text(seen_path());
    if (!text.has_value()) return;
    std::string line;
    for (const char ch : *text) {
      if (ch == '\n') {
        if (!line.empty()) seen.push_back(line);
        line.clear();
      } else if (ch != '\r') {
        line.push_back(ch);
      }
    }
    if (!line.empty()) seen.push_back(line);
  }

  void mark_seen(const std::string& name) {
    seen.push_back(name);
    (void)st::fs::append_text(seen_path(), name + "\n");
  }

  auto is_seen(const std::string& name) const -> bool {
    return std::find(seen.begin(), seen.end(), name) != seen.end();
  }

  /// 加入或更新一张卡片（同 id = 更新，agent 可能修正内容）。
  auto push(const Json& obj) -> Json {
    Card card = Card::from_json(obj);
    if (card.id.empty()) card.id = std::format("auto-{}-{}", st::time::now_ms(), ++seq);
    auto it = std::find_if(cards.begin(), cards.end(), [&](const Card& c) { return c.id == card.id; });
    if (it != cards.end()) {
      *it = card;
    } else {
      cards.push_back(card);
    }
    return as_json();
  }

  /// 整批替换（agent 下发的权威列表）。
  auto replace_all(const Json& list) -> Json {
    if (!list.is_array()) return as_json();
    std::vector<Card> next;
    next.reserve(list.size());
    for (const auto& item : list) next.push_back(Card::from_json(item));
    cards = std::move(next);
    return as_json();
  }

  /// 文件降级：扫 inbox 消费新文件。
  void poll_inbox() {
    (void)st::fs::create_directories(inbox_dir());
    const auto entries = st::fs::list_dir(inbox_dir());
    if (!entries.has_value()) return;
    std::vector<st::fs::DirEntry> files;
    for (const auto& entry : *entries) {
      if (!entry.is_dir) files.push_back(entry);
    }
    std::sort(files.begin(), files.end(),
              [](const st::fs::DirEntry& a, const st::fs::DirEntry& b) { return a.name < b.name; });
    for (const auto& file : files) {
      const std::string& name = file.name;
      if (name.size() < 5 || name.substr(name.size() - 5) != ".json") continue;
      if (is_seen(name)) continue;
      const auto text = st::fs::read_text(file.path);
      if (!text.has_value()) continue;
      const auto parsed = st::json_parse(*text);
      if (!parsed.has_value()) {
        // 坏文件不消费：留待人工看（静默删掉会让"我明明投了"变成无法排查）
        st::print("[相期] 跳过无法解析的投递：{}\n", name);
        continue;
      }
      if ((*parsed).is_array()) {
        for (const auto& item : *parsed) (void)push(item);
      } else {
        (void)push(*parsed);
      }
      mark_seen(name);
    }
  }

  /// 答复：优先走 WebSocket，同时落 outbox。
  auto answer(const std::string& id, const Json& value) -> Json {
    auto it = std::find_if(cards.begin(), cards.end(), [&](const Card& c) { return c.id == id; });
    if (it == cards.end()) return as_json();   // 已处理过（重复点击）：不报错

    Json record{{"type", "answer"},
                {"id", id},
                {"kind", it->kind},
                {"answer", value},
                {"answered_at_ms", st::time::now_ms()}};

    // 双写：WS 送达 agent + 落盘留底。
    // 为何两份都要：WS 可能恰好断在这一刻（send 失败就靠盘）；而盘上有记录
    // 才能在"我点了但没反应"时查到底是哪一环丢的。
    bool sent = false;
    if (link != nullptr && link->connected()) sent = link->send(record);
    (void)st::fs::create_directories(outbox_dir());
    (void)st::fs::write_text(st::fs::join(outbox_dir(), id + ".json"), st::json_dump(record, true));
    if (!sent) st::print("[相期] 答复 {} 未能实时送达（已落盘 {}）\n", id, outbox_dir());

    cards.erase(it);
    return as_json();
  }

  /// 批量忽略：**只清 notice**（choice/todo 必须给出结果）。
  auto dismiss_all() -> Json {
    std::vector<Card> kept;
    kept.reserve(cards.size());
    for (auto& card : cards) {
      if (card.kind == KIND_NOTICE) continue;
      kept.push_back(card);
    }
    cards = std::move(kept);
    return as_json();
  }

  /// 安装到桥：`app.cards.*`。
  ///
  /// 每个函数返回**新的全量列表**（与 xmlapp 的 TaskStore 同一取向）：
  /// JS 拿快照，不与 C++ 共享可变状态。
  void install(st::ui::ScriptBridge& bridge) {
    (void)bridge.expose("cards.list", [this](const std::vector<Json>&) -> st::Result<Json> {
      return as_json();
    });

    (void)bridge.expose("cards.poll", [this](const std::vector<Json>&) -> st::Result<Json> {
      // 文件降级路径：WS 没连上时才读（连上了以 WS 为准，避免两路打架）
      if (link == nullptr || !link->connected()) poll_inbox();

      Json peer{{"connected", link != nullptr && link->connected()},
                {"name", link != nullptr && link->connected() ? "agent 控制器" : ""},
                {"detail", link != nullptr ? link->status_text() : "未初始化"}};
      Json out{{"cards", as_json()}, {"peer", peer}};
      return out;
    });

    (void)bridge.expose("cards.push", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "cards.push 需要一张卡片对象");
      return push(args[0]);
    });

    (void)bridge.expose("cards.answer", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.size() < 2) return st::unexpected(st::ErrorCode::Invalid, "cards.answer 需要 id 与答复");
      const std::string id = st::json_as_string(args[0]);
      Json value = args[1];
      if (value.is_string()) {
        const std::string raw = st::json_as_string(value);
        if (const auto parsed = st::json_parse(raw); parsed.has_value()) value = *parsed;
      }
      return answer(id, value);
    });

    (void)bridge.expose("cards.dismiss_all", [this](const std::vector<Json>&) -> st::Result<Json> {
      return dismiss_all();
    });

    // 用系统默认程序打开 URL（授权/登录类卡片要"把人送到浏览器"）。
    //
    // 为何由 C++ 提供而不是 JS 自己做：JS 没有启动外部程序的能力，而
    // `window.open` 在原生窗口里不存在。框架已有 `st::platform::open_external`
    // （各平台走系统 API，不经 shell，URL 不会被解释成命令）。
    //
    // 为何返回 bool 而不是抛错：打不开浏览器不是"请求非法"，而是环境限制
    //（无桌面环境/无默认浏览器）——JS 据此提示用户手动复制链接。
    (void)bridge.expose("platform.open_url", [](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "platform.open_url 需要 URL");
      const std::string url = st::json_as_string(args[0]);
      if (url.empty()) return st::unexpected(st::ErrorCode::Invalid, "URL 不能为空");
      // 只接受 http(s)：其它协议（file:// / javascript: / 自定义 scheme）
      // 不应由"点卡片按钮"这条路径启动——那是任意程序启动面。
      if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return st::unexpected(st::ErrorCode::Invalid, "只允许打开 http(s) 链接");
      }
      const bool ok = st::platform::open_external(url);
      st::print("[相期] 打开外部链接（{}）：{}\n", ok ? "成功" : "失败", url);
      return Json{{{"ok", ok}, {"url", url}}};
    });

    (void)bridge.expose("link.status", [this](const std::vector<Json>&) -> st::Result<Json> {
      return Json{{"connected", link != nullptr && link->connected()},
                  {"detail", link != nullptr ? link->status_text() : "未初始化"}};
    });

    // 窗口尺寸：轻提示形态需要把窗口缩到小尺寸。
    //
    // 为何给 JS 而不是在 C++ 里决定："什么时候该缩"是纯粹的呈现决策，
    // 而呈现逻辑本来就归 JS（见 xmlapp 的能力分工）。C++ 只提供动作。
    (void)bridge.expose("window.set_size", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.size() < 2) return st::unexpected(st::ErrorCode::Invalid, "window.set_size 需要 (宽, 高)");
      if (app == nullptr) return st::unexpected(st::ErrorCode::Unsupported, "应用未就绪");
      const int width = static_cast<int>(args[0].get<double>());
      const int height = static_cast<int>(args[1].get<double>());
      if (auto status = app->set_window_size(width, height); !status) {
        return st::unexpected(status.error().code, status.error().message);
      }
      return st::Json(true);
    });
  }
};

struct Options {
  bool headless{false};
  bool watch{false};   ///< 改 XML/JS 即重载（常驻应用默认关：不每帧扫文件）
  std::string theme{"light"};
  std::string ui_dir{"examples/xiangqi/ui"};
  std::uint16_t control_port{0};
  std::string control_file{};
  std::string shots{};
  std::string home{};
  /// agent 控制器地址（`--agent` 或环境变量 `ST_XIANGQI_AGENT` 覆盖）。
  std::string agent_url{"ws://127.0.0.1:7717/xiangqi"};
};

auto parse_options(int argc, char** argv) -> Options {
  Options options;
  if (const char* env = std::getenv("ST_XIANGQI_AGENT"); env != nullptr && *env != '\0') {
    options.agent_url = env;
  }
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string{argv[++i]} : std::string{}; };
    if (arg == "--headless") {
      options.headless = true;
    } else if (arg == "--watch") {
      options.watch = true;
    } else if (arg == "--no-watch") {
      options.watch = false;
    } else if (arg == "--theme") {
      options.theme = next();
    } else if (arg == "--ui") {
      options.ui_dir = next();
    } else if (arg == "--control-port") {
      const std::string text = next();
      try {
        options.control_port = static_cast<std::uint16_t>(std::stoi(text));
      } catch (...) {
        std::fprintf(stderr, "[相期] --control-port 不是合法端口：%s\n", text.c_str());
      }
    } else if (arg == "--control-file") {
      options.control_file = next();
    } else if (arg == "--shots") {
      options.shots = next();
    } else if (arg == "--home") {
      options.home = next();
    } else if (arg == "--agent") {
      options.agent_url = next();
    }
  }
  return options;
}

/// 生成托盘图标（圆底 + 铃铛剪影）。
///
/// 为何程序内生成而不嵌资源文件：图标是 32×32 的简单图形，代码生成只需十几行，
/// 而嵌资源会引入一份二进制资产（改颜色就要换文件）。真要换品牌图标时，
/// 把 `AppOptions::tray_icon_png` 换成 `b::embed<"assets/tray.png">()` 即可。
///
/// 颜色取琥珀色（#d97706）——与界面里「提醒」那一档同色，形成视觉对应。
static auto make_tray_icon_png() -> std::vector<std::uint8_t> {
  constexpr std::uint32_t size = 32;   // 32px：高 DPI 下缩到 16 也不糊
  st::codec::PngImage image;
  image.width = size;
  image.height = size;
  image.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);

  const auto put = [&image](std::uint32_t x, std::uint32_t y, std::uint8_t r, std::uint8_t g,
                            std::uint8_t b, std::uint8_t a) {
    const std::size_t i = (static_cast<std::size_t>(y) * image.width + x) * 4;
    image.rgba[i + 0] = r;
    image.rgba[i + 1] = g;
    image.rgba[i + 2] = b;
    image.rgba[i + 3] = a;
  };

  const double center = (size - 1) / 2.0;
  const double outer = size / 2.0 - 0.5;
  // 颜色先定成常量：直接写字面量会被 `-Werror=conversion` 拦（int → uint8_t）
  constexpr auto kWhite = static_cast<std::uint8_t>(255);
  constexpr auto kAmberR = static_cast<std::uint8_t>(0xD9);
  constexpr auto kAmberG = static_cast<std::uint8_t>(0x77);
  constexpr auto kAmberB = static_cast<std::uint8_t>(0x06);
  for (std::uint32_t y = 0; y < size; ++y) {
    for (std::uint32_t x = 0; x < size; ++x) {
      const double dx = x - center;
      const double dy = y - center;
      const double dist = std::sqrt(dx * dx + dy * dy);
      if (dist > outer + 1.0) continue;   // 圆外留空（透明）
      // 抗锯齿：边缘 1px 内做 alpha 渐变，否则小图标边缘会硬
      const auto alpha = dist <= outer - 0.5
                             ? static_cast<std::uint8_t>(255)
                             : static_cast<std::uint8_t>(255.0 * (outer + 0.5 - dist));
      // 铃铛：上半弧体 + 下横杠 + 底部小锤（简化到“看得懂是铃铛”的程度）
      const bool bell_body = dist < outer * 0.55 && y < size * 0.62;
      const bool bell_rim = y >= size * 0.62 && y < size * 0.70 && std::abs(dx) < outer * 0.55;
      const bool bell_clapper = dist < size * 0.10 && y >= size * 0.72;
      if (bell_body || bell_rim || bell_clapper) {
        put(x, y, kWhite, kWhite, kWhite, alpha);          // 铃铛白
      } else {
        put(x, y, kAmberR, kAmberG, kAmberB, alpha);       // 底琥珀
      }
    }
  }

  auto encoded = st::codec::png_encode(image);
  if (!encoded) return {};
  return *encoded;
}

auto resolve_resource_dir(const std::string& requested) -> std::string {
  if (st::fs::is_regular_file(st::fs::join(requested, "main.xml"))) return requested;
  for (const char* candidate : {"examples/xiangqi/ui", "../examples/xiangqi/ui",
                                "../../examples/xiangqi/ui", "../../../examples/xiangqi/ui"}) {
    if (st::fs::is_regular_file(st::fs::join(candidate, "main.xml"))) return candidate;
  }
  return requested;
}

auto resolve_home(const std::string& requested) -> std::string {
  if (!requested.empty()) return requested;
  const auto cwd = st::fs::current_dir();
  const std::string base = cwd.has_value() ? *cwd : std::string{"."};
  return st::fs::join(base, "相期");
}

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);
  const std::string ui_dir = resolve_resource_dir(options.ui_dir);
  const std::string home = resolve_home(options.home);

  st::app::AppOptions app_options;
  app_options.width = 460;
  app_options.height = 620;
  app_options.title = "相期";
  app_options.headless = options.headless;
  app_options.backend = options.headless ? "headless" : std::string{};
  app_options.theme = options.theme == "dark" ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light;
  app_options.control_port = options.control_port;
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;
  app_options.enable_script = true;
  // 托盘（常驻形态）：无头模式不启（没有通知区，且 e2e 不需要它）。
  //
  // 图标与窗口图标共用一份：常驻面板在任务栏与通知区应当是同一个记号，
  // 否则用户会以为它们是两个东西。
  if (!options.headless) {
    app_options.tray_enabled = true;
    app_options.window_icon_png = make_tray_icon_png();
    app_options.tray_icon_png = app_options.window_icon_png;
    app_options.tray_tooltip = "相期 · 有事等你";
  }

  st::app::Application app("xiangqi", "0.1.0", app_options);

  // ① 外壳（**先壳后树**）：热重载换根的落点是 XML 根当前的父元素。
  auto frame = std::make_unique<st::ui::WindowFrame>("相期");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) {
    frame->title_bar()->set_id("titlebar");
    frame->title_bar()->set_icon("bell");

    // 徽标 / 连接图标 / 「全部忽略」全部上提到标题栏。
    //
    // ## 三个槽各管一段（框架提供，不是应用自己摆坐标）
    //
    //     [图标 品牌名][leading…][标题…][after_title…]……[trailing…][控制按钮]
    //
    // - 连接图标 → `add_after_title`：**紧跟标题文字右缘**。
    //   这正是该槽存在的理由：另两个槽都做不到——leading 在标题**之前**，
    //   trailing 是从右往左往按钮堆的。
    // - 徽标 / 「全部忽略」→ `add_trailing`：靠右排，与窗口按钮一组。
    //
    // ## 为何往标题栏里塞
    //
    // 以前是三条横向带：窗口标题栏、XML 的 header（又一遍图标+相期+徽标）、
    // 底部 footer（连接状态+全部忽略）。信息重复，而且白吃 2×40px。
    // 常驻面板的面积是稀缺资源（贴角、吃屏幕），内容区整块留给卡片。
    auto link_icon = std::make_unique<st::ui::IconView>("warning", 12.0f);
    link_icon->set_id("link-icon");
    frame->title_bar()->add_after_title(std::move(link_icon));

    auto badge = std::make_unique<st::ui::Text>("暂无待办");
    badge->set_id("badge");
    frame->title_bar()->add_trailing(std::move(badge));

    auto link = std::make_unique<st::ui::Text>("");
    link->set_id("link-text");
    // 连接状态的文字不占位：具体原因挂在图标的 hint 上。
    // 保留这个元素是给控制通道/测试留一个稳定的文本锚点。
    link->set_visible(false);
    frame->title_bar()->add_trailing(std::move(link));

    // 「全部忽略」：轻操作，但高频——放标题栏比占一整条底栏划算。
    auto dismiss = std::make_unique<st::ui::Button>("全部忽略");
    dismiss->set_id("dismiss-all");
    dismiss->set_variant(st::ui::Button::Variant::Ghost);
    dismiss->set_size(st::ui::Button::Size::Small);
    dismiss->set_hint("把所有提示清掉（选择类与待办类不会被忽略——那两类必须给出结果）");
    frame->title_bar()->add_trailing(std::move(dismiss));
  }
  app.set_content(std::move(frame));

  // ③ 起服务（脚本宿主在 start() 里创建——桥必须在它之后建）
  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败：%s\n", status.error().message.c_str());
    return 1;
  }
  st::ui::ScriptHost* script = app.script();
  if (script == nullptr || !script->valid()) {
    std::fprintf(stderr, "脚本宿主未就绪（AppOptions::enable_script 打开了吗）\n");
    return 1;
  }

  // ④ 连 agent（连不上不算错误：agent 可能还没起，界面会显示未连接）
  AgentLink link;
  if (link.connect(options.agent_url)) {
    st::print("[相期] 已连接 agent 控制器：{}\n", options.agent_url);
  } else {
    st::print("[相期] 未能连接 agent 控制器（{}）——降级为文件投递，界面显示未连接\n", link.status_text());
  }

  // ⑤ 暴露能力 + 装载 JS
  CardStore store;
  store.home = home;
  store.link = &link;
  store.app = &app;
  (void)st::fs::create_directories(store.inbox_dir());
  (void)st::fs::create_directories(store.outbox_dir());
  store.load_seen();
  store.poll_inbox();   // 启动时先吃掉已投递的卡片：重启不该丢件

  st::ui::ScriptBridge bridge(*script, &app.root());
  store.install(bridge);

  // ⑤.5 **改文件即生效**（改 XML/JS 保存即可，不重编不重启）
  //
  // 对相期这类常驻面板意义更大：它常年挂在屏幕上，为调一个间距就重启一次会把
  // “agent 投过来的卡片”与连接状态一起重来（重启会重新处理 inbox）。
  // 默认**不**扫文件（常驻形态不为一一个调试便利每帧 stat）：`--watch` 打开。
  st::ui::HotReload::Options hot_options;
  hot_options.ui_dir = ui_dir;
  hot_options.watch = options.watch;
  st::ui::HotReload hot(app.root(), *script, bridge, hot_options);
  // 标题栏里那些元素是壳层自己的（不在 XML 里），热重载换根不会碰它们。
  (void)bridge.expose("hot.reload", [&hot](const std::vector<Json>&) -> st::Result<Json> {
    auto status = hot.refresh_all();
    if (!status) return st::unexpected(status.error().code, status.error().message);
    return Json(true);
  });
  (void)bridge.expose("hot.status", [&hot](const std::vector<Json>&) -> st::Result<Json> {
    return hot.status();
  });
  if (auto status = hot.refresh_all(); !status) {
    std::fprintf(stderr, "装载失败：%s\n", status.error().message.c_str());
    return 1;
  }
  st::print("相期就绪：界面 {} · 逻辑 {} · 桥 {} 个函数 · 数据 {} · 热重载 {}\n", ui_dir,
            st::fs::join(ui_dir, "app.js"), bridge.exposed().size(), home,
            options.watch ? "开" : "关（--watch 打开）");

  // ⑥ 主循环：每帧顺手把 agent 下发的消息吃掉。
  //
  // 为什么在循环里读而不是开线程：卡片是每秒最多几次的规模，
  // 非阻塞 try_read 的开销远小于引入线程+锁的复杂度。
  std::uint64_t frames = 0;
  std::uint64_t handled = 0;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();

    // 托盘点击：把窗口叫回前台。
    //
    // 这就是托盘图标对用户心理模型的全部：点一下 = “我来看一眼”。
    // 不做右键菜单（那是另一套交互，且各平台差异大）。
    if (app.take_tray_clicks() > 0) {
      (void)app.window_minimize();   // placeholder…
    }

    // 处理入站消息（每帧最多 8 条：留出渲染时间，且不会真的堆积）
    for (int budget = 0; budget < 8; ++budget) {
      const auto message = link.poll();
      if (!message.has_value()) break;
      ++handled;
      const auto& msg = *message;
      const std::string type = msg.contains("type") ? st::json_as_string(msg["type"]) : std::string{};
      if (type == "cards" || type == "sync") {
        // 权威列表：整批替换（agent 是真相源）
        if (msg.contains("cards")) (void)store.replace_all(msg["cards"]);
      } else if (type == "card") {
        // 单张新增/更新：更省流量的增量形态
        (void)store.push(msg);
      } else if (type == "dismiss") {
        // agent 主动撤回某张卡（如它自己解决了）
        if (msg.contains("id")) {
          const std::string id = st::json_as_string(msg["id"]);
          (void)store.answer(id, Json{{"dismissed_by", "agent"}});
        }
      }
    }

    if (app.root().tick_declarative_hosts() > 0U) app.request_repaint();
    app.tick();
    ++frames;
    // 低频心跳（约 20 秒一次）：常驻应用需要"它还活着"的可观测证据。
    // 探测/可读两个计数用于区分"没有消息"与"有数据但读不出"——
    // 后者是 WS 客户端最容易出的问题（实测踩过一次，无任何报错）。
    if (frames % 1200 == 0) {
      st::print("[相期] 心跳 帧={} 连接={} 卡片={} 处理={} 探测={} 可读={}\n", frames, link.connected(),
                store.cards.size(), handled, link.probe_count(), link.readable_count());
    }
    app.pace_loop(frame_start_ms);
  }
  link.disconnect();
  st::print("相期退出：{} 帧，处理消息 {} 条（未处理的卡片留在 {}，下次启动会重新出现）\n", frames,
            handled, store.inbox_dir());
  return 0;
}

}  // namespace

ST_MAIN(run_app)
