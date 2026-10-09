#pragma once

/// gbcode 的语言服务集成（LSP 阶段 3 起）。
///
/// 职责划分：**这是应用层的桥**——把 `st::lsp::LspClient`（框架的协议/会话能力）
/// 接到 gbcode 的编辑器与问题列表上。glue 代码独立成文件而不是塞进 `main.cpp`
/// （那里已经 3000+ 行，且 LSP 的生命周期/语言选择/诊断换算自成一摊）。
///
/// ## 按扩展名挑 server
///
/// 一张静态表（`{语言, 扩展名, 程序, 参数}`），命中就用；没有对应 server 的
/// 语言**如实不启用**（不假装连着——"编辑器说我在检查，其实没检查"比不检查更坏）。
///
/// ## 文档同步的版本纪律
///
/// 每次编辑器文本变化都要发 `didChange`，但**不能每帧发**（编辑器每帧都可能重组）。
/// 做法：记住"上次同步出去的文本"，变了才发——且用 `compute_single_change` 算增量。
/// 若一次变化无法用单区间表达（多处不连续编辑：格式化、多光标删除），
/// 退回 `did_open`（重开文档）——比发错 diff 好：错 diff 会让 server 的文件内容
/// 与我们永久不一致，而重开只是丢一次增量。

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/lsp/client.hpp"
#include "st/lsp/completion.hpp"
#include "st/ui/components/code_editor.hpp"

namespace gbcode {

/// 一个语言的 server 配方。
struct LanguageServerRecipe {
  std::string language{};                    ///< "cpp" / "python" / ...
  std::vector<std::string> extensions{};     ///< {".cpp", ".hpp", ...}
  std::string program{};                     ///< 可执行文件名（走 PATH）
  std::vector<std::string> args{};           ///< 附加参数
};

/// 内置配方表（程序不存在时那一条自动不可用）。
[[nodiscard]] auto builtin_recipes() -> const std::vector<LanguageServerRecipe>&;

/// 按文件路径挑配方（扩展名匹配；无匹配返回 nullptr）。
[[nodiscard]] auto recipe_for_path(std::string_view path) -> const LanguageServerRecipe*;

/// 按语言名挑配方（编辑器语言 id → 配方；无匹配返回 nullptr）。
[[nodiscard]] auto recipe_for_language(std::string_view language) -> const LanguageServerRecipe*;

/// 诊断对 UI 的呈现形态（与 `Problem` 同构：行号 + 严重级别 + 消息）。
struct LspProblem {
  std::size_t line{1};    ///< 1 起（与 gbcode 既有 `Problem` 同口径）
  bool warning{true};
  std::string message{};
  std::string source{};   ///< "clang" 等
  std::uint32_t column{1};   ///< 1 起（跳转/排序用）
};

/// 一个 server 会话 + 它负责的文档。
///
/// 生命周期：`ensure_started()`（懒启动，首次打开对应语言的文件时才起进程）
/// → `sync_document()`（每次编辑器文本变化）→ `pump()`（主循环每帧）
/// → `shutdown()`（退出时）。
class LanguageService {
 public:
  LanguageService();
  ~LanguageService();
  LanguageService(const LanguageService&) = delete;
  auto operator=(const LanguageService&) -> LanguageService& = delete;

  /// 设置工作区根（server 需要它做索引；空 = 不上报根）。
  void set_workspace(std::string path);

  /// 为某文件确保 server 已启动（已在跑则只切文档）。
  /// 返回 false = 该文件没有可用 server（**如实不启用**）。
  auto ensure_started(std::string_view file_path, std::string_view language) -> bool;

  /// 同步文档内容（文本没变则什么都不做——会被每帧调用）。
  void sync_document(std::string_view file_path, std::string_view text);

  /// 关闭某文件的文档（标签关掉时）。
  void close_document(std::string_view file_path);

  /// 主循环每帧调：泵消息、把诊断换算成 UI 形态。
  /// 返回本次是否有诊断更新（调用方据此刷新界面状态）。
  auto pump() -> bool;

  /// 当前活动文件的诊断（UI 直接读）。
  [[nodiscard]] auto problems() const -> const std::vector<LspProblem>&;
  [[nodiscard]] auto error_count() const -> std::size_t;
  [[nodiscard]] auto warning_count() const -> std::size_t;

  /// 会话状态的可读描述（状态栏显示："clangd 就绪 · 2 个问题" / "未启用"）。
  [[nodiscard]] auto status_text() const -> std::string;

  /// 是否已启用（有 server 在跑或已就绪）。
  [[nodiscard]] auto active() const -> bool;
  /// 当前 server 名（未启用时为空）。
  [[nodiscard]] auto server_name() const -> std::string;
  /// 最近一次失败原因（空 = 无）。
  [[nodiscard]] auto error() const -> std::string;

  /// 文档字节偏移 ↔ LSP 位置换算（宿主请求 hover/定义时用）。
  /// `path` 必须是已同步的文档；未同步返回 nullopt。
  [[nodiscard]] auto offset_to_position(std::string_view path, std::size_t offset) const
      -> std::optional<st::lsp::Position>;

  /// 发一条业务请求（就绪才发；返回 0 = 未发出）。
  auto request(std::string method, st::Json params) -> std::int64_t;
  /// 响应回调（`(id, method, result, is_error)`）。
  std::function<void(std::int64_t, const std::string&, const st::Json&, bool)> on_response{};

  // —— 补全（阶段 4）——

  /// 请求补全（`path` 的 `line`/`character` 处；行列为 **0 基**、列按 UTF-16 码元）。
  /// 返回请求 id（0 = 未发出）。响应经 `on_completion`。
  auto request_completion(std::string_view path, std::uint32_t line, std::uint32_t character)
      -> std::int64_t;

  /// 补全响应回调：`(请求 id, 候选, 是否不完整列表)`。
  std::function<void(std::int64_t, const std::vector<st::lsp::CompletionEntry>&, bool)>
      on_completion{};

  /// 触发字符集（来自 server 的 `initialize` 能力；空 = server 没声明）。
  [[nodiscard]] auto trigger_characters() const -> const std::vector<std::string>&;
  /// 取候选详情（`completionItem/resolve`；结果经 `on_completion_detail`）。
  auto resolve_completion(std::string_view path, std::size_t index) -> std::int64_t;
  /// 详情回调：`(请求 id, 文本)`。
  std::function<void(std::int64_t, const std::string&)> on_completion_detail{};

  /// 当前活动文件路径（诊断归属它；由 `ensure_started` 更新）。
  [[nodiscard]] auto active_path() const -> const std::string&;
  /// 切换活动文件（只影响"哪份诊断给 UI 看"，不重启 server）。
  void set_active_path(std::string path);

  /// 收尾（退出时调；阻塞至多 `timeout_ms`）。
  void shutdown(std::int64_t timeout_ms = 1500);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_{};
};

}  // namespace gbcode
