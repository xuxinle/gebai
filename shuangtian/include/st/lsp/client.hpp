#pragma once

/// LSP 客户端（阶段 2 地基）：**进程 + 生命周期 + 文档同步 + 异步泵**。
///
/// 分三层关注点，各归各位：
/// - `st/lsp/protocol`：字节 ↔ 消息（分帧、JSON-RPC、位置换算）——已就位；
/// - 本文件：**会话语义**——server 何时起来、握手到哪一步、文档同步到哪一版、
///   哪些请求在等响应；
/// - UI（后续阶段）：诊断怎么画、补全怎么弹。
///
/// ## 线程模型（与 `Terminal` 同一套契约，不另立范式）
///
/// ```
///  主线程                     读线程（jthread）         写线程（jthread）
///  ─────────                 ─────────────────         ────────────────
///  request()/notify() ──► 发送队列 ──►                --► write(管道)
///  pump()  ◄── 接收队列 ◄── FrameReader 解析
/// ```
///
/// - **只有主线程碰 `pump()`**：它把接收队列里的消息搬进本对象状态并触发回调。
///   UI 状态一律在主线改，读线程绝不回调用户代码——否则要么加锁到死，
///   要么在错误线程改 UI（两者都出事）。
/// - 写也是工作线程：`write()` 是阻塞调用，pipe 满时会把主线程卡住
///   （编辑器每敲一个键发一次 `didChange`，一次卡顿肉眼可见）。
/// - 队列用 `std::mutex` + `std::vector`（不是无锁）：消息频率是"每次编辑/每次响应"
///   量级，远够用；无锁队列在这里只会带来难查的内存序 bug。
///
/// ## 文档同步：**增量**而非全量
///
/// 协议允许 `TextDocumentSyncKind::Full`（每次变更发全文）。三条理由选增量：
/// 1. **带宽**：clangd 对 2000 行文件的全文同步会让每次敲键都传 60 KB；
/// 2. **server 端解析成本**：全文同步 = server 每次重解析整篇；
/// 3. **正确性更好做**：增量带 `range`，server 能保住自己那份增量语法树，
///    补全质量与延迟都与 VS Code 一致（它们都发增量）。
///
/// ⚠ 增量的**风险**是"两边版本漂移"（我们算的 diff 与 server 的理解不一致），
/// 一旦漂移所有位置全错。因此契约是：**每次变更必须带单调递增的 version**，
/// 且**只允许通过本类的 `did_change()` 改文档**（别处改了文本就 `did_open` 重来）。
/// 单一变更入口是这条契约能站住的前提。
///
/// ## 覆盖策略：不发起 `did_change` 的情形
///
/// 若一次变更**无法用单个 range 表达**（多处不连续编辑，如格式化、多光标删除），
/// 就发 `did_open` 重启文档（server 会当成新文档）——比发错 diff 好得多：
/// 错 diff 会让 server 的文件内容与我们永久不一致，而重开只是丢一次增量。

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "st/ext/json.hpp"
#include "st/lsp/protocol.hpp"

namespace st::lsp {

/// 会话状态。转换只由 `pump()` 推动（读线程不碰状态）。
enum class SessionState : std::uint8_t {
  Idle,          ///< 未启动
  Starting,      ///< 进程已起，`initialize` 已发，等响应
  Ready,         ///< 握手完成，可发业务请求
  Failed,        ///< 启动失败 / 握手失败 / 进程异常退出（`error()` 有原因）
  ShuttingDown,  ///< `shutdown` 已发，等响应
  Stopped,       ///< 正常收尾完成
};

/// 面向 UI 的会话快照（`pump` 后读；避免外部直接摸内部状态）。
struct SessionStatus {
  SessionState state{SessionState::Idle};
  std::string error{};            ///< `Failed` 时的原因（人可读）
  std::string server_name{};      ///< `initialize` 响应里的 `serverInfo.name`
  std::string server_version{};   ///< `serverInfo.version`
  bool supports_completion{false};
  bool supports_hover{false};
  bool supports_definition{false};
  bool supports_references{false};
  bool supports_document_symbol{false};
  bool supports_diagnostics{false};
  bool supports_workspace_symbol{false};
  std::int64_t pending_requests{0};   ///< 已发未答的请求数（诊断用）
  std::int64_t open_documents{0};
};

/// 服务端能力（`initialize` 响应 `capabilities` 的子集，够用就好）。
struct ServerCapabilities {
  bool completion{false};
  bool completion_resolve{false};
  bool hover{false};
  bool definition{false};
  bool references{false};
  bool document_symbol{false};
  bool workspace_symbol{false};
  /// 诊断推送方式（`publishDiagnostics` 一定有；`diagnosticProvider` 是拉取式，暂不支持）。
  bool diagnostics{true};
  /// 触发字符（如 `"."` `"::"` `">"`）——补全阶段的输入。
  std::vector<std::string> completion_trigger_characters;
};

/// 一条诊断（`textDocument/publishDiagnostics` 通知的解析结果）。
struct Diagnostic {
  Range range{};
  int severity{1};            ///< 1=Error 2=Warning 3=Information 4=Hint（协议值）
  std::string message{};
  std::string source{};       ///< 如 "clang"；用于显示
  std::string code{};         ///< 诊断码（可空）
  bool operator==(const Diagnostic& other) const noexcept {
    return severity == other.severity && message == other.message &&
           range.start.line == other.range.start.line &&
           range.start.character == other.range.start.character &&
           range.end.line == other.range.end.line &&
           range.end.character == other.range.end.character;
  }
};

/// 单次文本变更（增量的基本单位，语义同 LSP `TextDocumentContentChangeEvent`）。
struct TextChange {
  /// 变更前的区间（**基于我们发出去的上一版**）。空 `range` = 全文替换。
  bool is_full{false};
  Range range{};
  std::string text{};   ///< 新文本
};

/// 客户端配置。
struct ClientConfig {
  std::string program{};              ///< server 可执行文件（如 "clangd"）
  std::vector<std::string> args{};    ///< 附加参数
  std::string root_uri{};             ///< 工作区根（`file://...`；空 = 不上报根）
  std::string root_path{};            ///< 工作区根路径（用于算 root_uri）
  Json initialization_options{};      ///< 传给 server 的 `initializationOptions`
  /// 握手超时（server 卡死时如实报失败，不无限等）。
  std::int64_t init_timeout_ms{15000};
};

/// LSP 客户端：**一个实例 = 一个 server 进程**。
///
/// 生命周期：
/// ```cpp
/// LspClient client;
/// client.start(config);            // 起进程 + 发 initialize
/// // 主循环里每帧：
/// client.pump();                   // 搬消息、推进状态、触发回调
/// // 需要在 state()==Ready 后才能发业务请求
/// ```
class LspClient {
 public:
  LspClient();
  ~LspClient();
  LspClient(const LspClient&) = delete;
  auto operator=(const LspClient&) -> LspClient& = delete;

  // —— 生命周期 ——

  /// 启动 server（非阻塞：进程起来、`initialize` 发出即返回）。
  /// 失败**不抛**：`status()` 会进入 `Failed` 并带原因（见 `error()`）。
  void start(ClientConfig config);

  /// 优雅收尾：`shutdown` → `exit` → 关进程。**阻塞至多 `timeout_ms`**。
  /// 析构会自动调用（避免留下孤儿进程）。
  void stop(std::int64_t timeout_ms = 2000);

  /// 每帧泵：搬接收队列 → 推进状态机 → 触发回调。
  /// **只在主线程调**。返回本次处理的消息条数（0 = 无新消息）。
  auto pump() -> std::size_t;

  [[nodiscard]] auto status() const -> SessionStatus;
  [[nodiscard]] auto state() const -> SessionState;
  [[nodiscard]] auto capabilities() const -> ServerCapabilities;
  [[nodiscard]] auto error() const -> std::string;

  // —— 文档同步 ——

  /// 打开文档（发 `textDocument/didOpen`，version=1）。重复打开同一 uri：先 `did_close`。
  void did_open(std::string uri, std::string language_id, std::string text);

  /// 变更文档（发 `textDocument/didChange`，version 自增）。
  ///
  /// `changes` 里的 range 语义见 `TextChange`；`full_text` 是**变更后**的全文
  /// （用于维护我们自己的副本与后续 diff——增量必须知道当前内容）。
  void did_change(std::string_view uri, const std::vector<TextChange>& changes,
                  std::string full_text);

  /// 关闭文档（发 `textDocument/didClose`）。之后 server 不再为它诊断。
  void did_close(std::string_view uri);

  [[nodiscard]] auto document_version(std::string_view uri) const -> std::int64_t;
  [[nodiscard]] auto is_open(std::string_view uri) const -> bool;
  [[nodiscard]] auto document_text(std::string_view uri) const -> std::string;

  // —— 请求（异步：响应经 `on_response` 回调）——

  /// 发一条自定义请求，返回请求 id（0 = 未发出，如未就绪）。响应经 `on_response`。
  [[nodiscard]] auto request(std::string method, Json params) -> std::int64_t;

  /// 发一条通知（不等响应）。
  void notify(std::string method, Json params);

  /// 取消已发出的请求（`$/cancelRequest`；补全这种"输入变了就作废"的场景要用）。
  void cancel(std::int64_t request_id);

  // —— 回调（都在 `pump()` 的主线调用栈里触发）——

  /// 收到响应：`(request_id, method, result, is_error)`。
  /// 错误时 `result` 是 `{"code":..,"message":..}`。
  std::function<void(std::int64_t, const std::string&, const Json&, bool)> on_response{};
  /// 收到服务端请求（如 `workspace/configuration`）：需要**回复**，用 `reply()`。
  std::function<void(const Message&)> on_server_request{};
  /// 状态变化（`Failed` 时 `status().error` 有原因）。
  std::function<void(SessionState)> on_state_change{};
  /// 诊断推送（`textDocument/publishDiagnostics`）。
  std::function<void(const std::string& uri, const std::vector<Diagnostic>&)> on_diagnostics{};
  /// 服务端日志（`window/logMessage`）——接状态栏/输出面板。
  std::function<void(int type, const std::string& message)> on_log{};
  /// 未识别的通知（便于排查"server 说了什么我们没接"）。
  std::function<void(const std::string& method, const Json& params)> on_unhandled{};

  /// 回复服务端请求（在 `on_server_request` 里用）。
  void reply(const Message& request, Json result);
  /// 回复服务端请求（错误）。
  void reply_error(const Message& request, int code, std::string message);

  /// 把路径转成 `file://` URI（跨平台：Windows 盘符要 `file:///C:/...`）。
  [[nodiscard]] static auto path_to_uri(std::string_view path) -> std::string;
  /// 把 `file://` URI 转回路径（失败返回空）。
  [[nodiscard]] static auto uri_to_path(std::string_view uri) -> std::string;

  /// 我们上报给 server 的客户端能力（`initialize` 的 `capabilities` 字段）。
  /// 公开只为可测：单测断言"声明的能力与实现一致"，防止声明了却没实现
  /// （声明了 `synchronization.didSave` 却不接 `didSave`，server 会等消息）。
  [[nodiscard]] static auto client_capabilities() -> Json;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_{};
};

/// 计算把 `old_text` 变成 `new_text` 的**最小单区间 diff**。
///
/// 返回 `nullopt` = 无法用单个 range 表达（多处不连续变更）——调用方应改用
/// 全文重开（见类注释的"覆盖策略"）。
///
/// 算法：去掉公共前缀与公共后缀，中间那段就是 [start, end) 被替换成 `new_text`
/// 的对应片段。这是**单区间最小表达**，对"敲一个字符""删除一行""粘贴一段"
/// 这些常见编辑都是最优的（`A` + `B` 前后缀能覆盖绝大多输入）。
///
/// ⚠ **按 UTF-8 码点边界切**：切在半个多字节字符中间会让 LSP 的 range 落到
/// server 无法理解的位置（它按 UTF-16 索引，半个码点直接崩或错位）。
[[nodiscard]] auto compute_single_change(std::string_view old_text, std::string_view new_text)
    -> std::optional<TextChange>;

}  // namespace st::lsp
