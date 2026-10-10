#include "lsp_bridge.hpp"

#include "st/lsp/completion.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"

namespace gbcode {

namespace {

/// 扩展名归一（小写，含点）。
[[nodiscard]] auto normalize_extension(std::string_view path) -> std::string {
  std::string ext = st::fs::extension(path);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

/// URI ↔ 路径（LSP 一律用 URI；gbcode 内部一律用路径）。
[[nodiscard]] auto to_uri(std::string_view path) -> std::string {
  return st::lsp::LspClient::path_to_uri(path);
}

/// LSP 的 `languageId`（server 按它选解析器；clangd 只认 `cpp`/`c`，不认 `c++`）。
[[nodiscard]] auto language_id_for(std::string_view path) -> std::string {
  if (const LanguageServerRecipe* recipe = recipe_for_path(path); recipe != nullptr) {
    return recipe->language;
  }
  return "plaintext";
}

}  // namespace

auto builtin_recipes() -> const std::vector<LanguageServerRecipe>& {
  // 顺序即优先级（扩展名有歧义时取先命中的）。`.h` 归 C++（实践中更常见）。
  static const std::vector<LanguageServerRecipe> recipes = {
      LanguageServerRecipe{
          .language = "cpp",
          .extensions = {".cpp", ".cc", ".cxx", ".c", ".hpp", ".hh", ".hxx", ".h", ".inl"},
          .program = "clangd",
          // `--background-index=0`：gbcode 是编辑器不是 IDE，后台索引会吃掉
          // 一整个核并拖慢首次诊断（用户感受是"打开文件就卡"）。
          // `--clang-tidy=0` 同理：静态检查留给显式的构建/CI。
          .args = {"--background-index=0", "--clang-tidy=0", "--log=error"},
      },
      LanguageServerRecipe{
          .language = "python",
          .extensions = {".py", ".pyi"},
          .program = "pyright-langserver",
          .args = {"--stdio"},
      },
      LanguageServerRecipe{
          .language = "go",
          .extensions = {".go"},
          .program = "gopls",
          .args = {},
      },
      LanguageServerRecipe{
          .language = "rust",
          .extensions = {".rs"},
          .program = "rust-analyzer",
          // ⚠ 参数名必须是 `--no-log-buffering`：写成 `--no-log-buffer` 时
          // rust-analyzer **直接退出**（未知参数），而症状是"进程已退出"这种
          // 与参数无关的报错——实测踩到（真机四语言冒烟的价值就在这）。
          .args = {"--no-log-buffering"},
      },
  };
  return recipes;
}

auto recipe_for_path(std::string_view path) -> const LanguageServerRecipe* {
  const std::string ext = normalize_extension(path);
  if (ext.empty()) return nullptr;
  for (const auto& recipe : builtin_recipes()) {
    if (std::find(recipe.extensions.begin(), recipe.extensions.end(), ext) !=
        recipe.extensions.end()) {
      return &recipe;
    }
  }
  return nullptr;
}

auto recipe_for_language(std::string_view language) -> const LanguageServerRecipe* {
  for (const auto& recipe : builtin_recipes()) {
    if (recipe.language == language) return &recipe;
  }
  return nullptr;
}

// ———————————————————————————— LanguageService ————————————————————————————

struct LanguageService::Impl {
  st::lsp::LspClient client{};
  std::string workspace{};
  std::string active_path{};
  std::string server_name{};
  std::string error{};
  bool started{false};
  /// 已同步给 server 的文本（按路径）——变了才发 `didChange`。
  std::map<std::string, std::string, std::less<>> synced{};
  /// 行起始字节偏移缓存（按路径）：`offset_to_position` 的加速结构。
  ///
  /// 为什么需要：光标每次移动（hover 防抖、补全、跳转）都要调一次位置换算，
  /// 旧实现线性扫描全文 O(n)——几千行的文件每帧扫一遍，CPU 剖面里它排在
  /// LSP 相关热点第一。改为「行偏移表 + 二分定位」后 O(log lines + line bytes)。
  /// 文本变化时增量重建（只在对应路径上重建，别的文件不受影响）。
  mutable std::map<std::string, std::vector<std::size_t>, std::less<>> line_index{};
  /// 全部诊断（按 URI 存；只有活动文件的给 UI）。
  std::map<std::string, std::vector<LspProblem>, std::less<>> diagnostics{};
  /// 诊断指纹（按 URI）：位置+级别+消息的哈希——server 每 didChange 后都推一整批，
  /// 内容没变时（常见：在无关行打字）旧实现全量拷贝+排序再触发 UI 刷新。
  /// 指纹相同则跳过存储与通知（pump 不会因此返回 true，UI 不重组）。
  std::map<std::string, std::size_t, std::less<>> diagnostic_fingerprint{};
  /// 最近一次诊断更新的 uri（pump 返回 true 时用）。
  std::string last_updated{};
  /// 最近一次补全候选（`completionItem/resolve` 要用原始 JSON 回传）。
  std::vector<st::lsp::CompletionEntry> last_completion{};
  /// 触发字符缓存（`capabilities()` 返回副本，不能返回其成员引用）。
  mutable std::vector<std::string> trigger_cache{};
  /// 最近一次导航请求族名（definition/declaration/typeDefinition/implementation/
  /// references）——应用层拿它与 `on_locations` 的结果配对呈现（「找到 3 处引用」
  /// vs「跳到定义」）。存这里而不是让应用层自己记 id：请求未发出时（返回 0）
  /// 应用层无从知道族名，这个口径由桥统一维护。
  std::string last_navigation_method{};
};

LanguageService::LanguageService() : impl_(std::make_unique<Impl>()) {
  impl_->client.on_diagnostics = [this](const std::string& uri,
                                       const std::vector<st::lsp::Diagnostic>& list) {
    const std::string path = st::lsp::LspClient::uri_to_path(uri);
    std::vector<LspProblem> problems;
    problems.reserve(list.size());
    for (const auto& diagnostic : list) {
      LspProblem problem{};
      // LSP 的 severity：1=Error 2=Warning 3=Info 4=Hint；gbcode 的 `warning` 布尔
      // 只有两档，Info/Hint 归入"警告"（状态栏的"问题"计数语义）。
      problem.warning = diagnostic.severity != 1;
      problem.line = diagnostic.range.start.line + 1;
      problem.column = static_cast<std::uint32_t>(diagnostic.range.start.character + 1);
      problem.message = diagnostic.message;
      problem.source = diagnostic.source;
      problems.push_back(std::move(problem));
    }
    // server 的推送顺序不保证；按位置排序让状态栏"下一个问题"跳转是单调的。
    std::sort(problems.begin(), problems.end(), [](const LspProblem& a, const LspProblem& b) {
      if (a.line != b.line) return a.line < b.line;
      return a.column < b.column;
    });
    // 指纹去重：内容没变的推送直接跳过（省一次全量拷贝 + 一次 UI 刷新）。
    // 指纹用 FNV-1a 折叠位置/级别/消息——碰撞概率低到可忽略（诊断是短文本）。
    std::size_t hash = 1469598103934665603ULL;
    for (const auto& p : problems) {
      const auto mix = [&hash](std::size_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
      };
      mix(p.line);
      mix(p.column);
      mix(p.warning ? 1 : 0);
      for (const char c : p.message) mix(static_cast<unsigned char>(c));
      mix(0x7F);   // 消息间分隔，防拼接歧义
    }
    const auto known = impl_->diagnostic_fingerprint.find(path);
    if (known != impl_->diagnostic_fingerprint.end() && known->second == hash) return;
    impl_->diagnostic_fingerprint.insert_or_assign(path, hash);
    impl_->diagnostics[path] = std::move(problems);
    impl_->last_updated = path;
  };
  impl_->client.on_state_change = [this](st::lsp::SessionState state) {
    if (state == st::lsp::SessionState::Failed) {
      impl_->error = impl_->client.error();
    }
  };
  impl_->client.on_response = [this](std::int64_t id, const std::string& method,
                                     const st::Json& result, bool is_error) {
    // 补全相关响应在这里**消化掉**（应用层不必自己解析协议字段）。
    if (method == "textDocument/completion") {
      if (is_error) return;
      impl_->last_completion = st::lsp::parse_completion(result);
      if (on_completion) on_completion(id, impl_->last_completion,
                                       st::lsp::completion_is_incomplete(result));
      return;
    }
    if (method == "completionItem/resolve") {
      if (!is_error && on_completion_detail) {
        on_completion_detail(id, st::lsp::parse_completion_detail(result));
      }
      return;
    }
    // —— 导航类（阶段 5）：在桥内消化，应用层拿的是解析结果 ——
    if (method == "textDocument/definition" || method == "textDocument/references" ||
        method == "textDocument/implementation" || method == "textDocument/declaration") {
      if (on_locations) {
        on_locations(id, is_error ? std::vector<st::lsp::Location>{}
                                  : st::lsp::parse_locations(result));
      }
      return;
    }
    if (method == "textDocument/hover") {
      if (on_hover) {
        on_hover(id, is_error ? std::nullopt : st::lsp::parse_hover(result));
      }
      return;
    }
    if (method == "textDocument/documentSymbol") {
      if (on_symbols) {
        on_symbols(id, is_error ? std::vector<st::lsp::DocumentSymbol>{}
                                : st::lsp::parse_document_symbols(result));
      }
      return;
    }
    if (method == "workspace/symbol") {
      if (on_workspace_symbols) {
        on_workspace_symbols(id, is_error ? std::vector<st::lsp::WorkspaceSymbol>{}
                                          : st::lsp::parse_workspace_symbols(result));
      }
      return;
    }
    // —— 编辑类（阶段 7）——
    if (method == "textDocument/formatting" || method == "textDocument/rangeFormatting") {
      if (on_format_edits) {
        on_format_edits(id, is_error ? std::vector<st::lsp::TextEdit>{}
                                     : st::lsp::parse_formatting_edits(result));
      }
      return;
    }
    if (method == "textDocument/prepareRename") {
      if (on_prepare_rename) {
        on_prepare_rename(id, is_error ? std::nullopt : st::lsp::parse_prepare_rename(result));
      }
      return;
    }
    if (method == "textDocument/rename") {
      if (on_rename) {
        on_rename(id, is_error ? st::lsp::WorkspaceEdit{} : st::lsp::parse_workspace_edit(result));
      }
      return;
    }
    if (on_response) on_response(id, method, result, is_error);
  };
}

LanguageService::~LanguageService() { shutdown(500); }

void LanguageService::set_workspace(std::string path) {
  if (impl_->workspace == path) return;
  impl_->workspace = std::move(path);
  // 换工作区：旧 server 的索引属于旧根，继续用会给错结果（跨项目的符号解析）。
  if (impl_->started) {
    impl_->client.stop(800);
    impl_->started = false;
    impl_->synced.clear();
    impl_->line_index.clear();
    impl_->diagnostics.clear();
    impl_->diagnostic_fingerprint.clear();
    impl_->server_name.clear();
  }
}

auto LanguageService::ensure_started(std::string_view file_path, std::string_view language)
    -> bool {
  if (impl_->started) return true;
  const LanguageServerRecipe* recipe = recipe_for_language(language);
  if (recipe == nullptr) recipe = recipe_for_path(file_path);
  if (recipe == nullptr) {
    impl_->error = "没有可用的语言服务器";
    return false;
  }
  // server 程序不存在：**如实不启用**（不假装"正在检查"）。
  if (!st::process::which(recipe->program).has_value()) {
    impl_->error = std::format("未安装 {}", recipe->program);
    return false;
  }

  st::lsp::ClientConfig config{};
  config.program = recipe->program;
  config.args = recipe->args;
  config.root_path = impl_->workspace;
  impl_->client.start(config);
  impl_->started = true;
  impl_->server_name = recipe->program;
  impl_->error.clear();
  return true;
}

void LanguageService::sync_document(std::string_view file_path, std::string_view text) {
  if (!impl_->started) return;
  const std::string path(file_path);
  const std::string uri = to_uri(path);
  const auto found = impl_->synced.find(path);
  if (found == impl_->synced.end()) {
    // 首次：didOpen。
    impl_->synced.emplace(path, std::string(text));
    impl_->client.did_open(uri, language_id_for(path), std::string(text));
    return;
  }
  if (found->second == text) return;   // 没变：不发（本函数每帧被调）

  // 变了：算增量。单区间表达不了（多处不连续编辑）就重开文档。
  if (const auto change = st::lsp::compute_single_change(found->second, text);
      change.has_value()) {
    impl_->client.did_change(uri, {*change}, std::string(text));
  } else {
    impl_->client.did_close(uri);
    impl_->client.did_open(uri, language_id_for(path), std::string(text));
  }
  found->second = std::string(text);
  // 文本变了：行偏移缓存作废（下次 offset_to_position 重建）。
  impl_->line_index.erase(path);
}

void LanguageService::document_saved(std::string_view file_path) {
  if (!impl_->started) return;
  const auto found = impl_->synced.find(std::string(file_path));
  if (found == impl_->synced.end()) return;
  // 带全文：宿主刚写盘，这份就是磁盘真相；server 可据此校验一致性。
  impl_->client.did_save(to_uri(file_path), found->second);
}

void LanguageService::close_document(std::string_view file_path) {
  if (!impl_->started) return;
  const std::string path(file_path);
  impl_->synced.erase(path);
  impl_->line_index.erase(path);
  impl_->diagnostics.erase(path);
  impl_->diagnostic_fingerprint.erase(path);
  impl_->client.did_close(to_uri(path));
  if (impl_->active_path == path) impl_->active_path.clear();
}

auto LanguageService::pump() -> bool {
  if (!impl_->started) return false;
  (void)impl_->client.pump();
  if (impl_->last_updated.empty()) return false;
  // 只有**活动文件**的诊断更新才算"UI 需要刷新"（别的文件的诊断存在那里备用）。
  const bool relevant = impl_->last_updated == impl_->active_path;
  impl_->last_updated.clear();
  return relevant;
}

auto LanguageService::problems() const -> const std::vector<LspProblem>& {
  static const std::vector<LspProblem> empty{};
  const auto found = impl_->diagnostics.find(impl_->active_path);
  return found == impl_->diagnostics.end() ? empty : found->second;
}

auto LanguageService::error_count() const -> std::size_t {
  const auto& list = problems();
  return static_cast<std::size_t>(
      std::count_if(list.begin(), list.end(), [](const LspProblem& p) { return !p.warning; }));
}

auto LanguageService::warning_count() const -> std::size_t {
  const auto& list = problems();
  return static_cast<std::size_t>(
      std::count_if(list.begin(), list.end(), [](const LspProblem& p) { return p.warning; }));
}

auto LanguageService::status_text() const -> std::string {
  if (!impl_->started) {
    return impl_->error.empty() ? std::string("LSP 未启用") : ("LSP：" + impl_->error);
  }
  const auto status = impl_->client.status();
  switch (status.state) {
    case st::lsp::SessionState::Starting: return impl_->server_name + " 启动中…";
    case st::lsp::SessionState::Ready: {
      const std::size_t errors = error_count();
      const std::size_t warnings = warning_count();
      if (errors == 0 && warnings == 0) return impl_->server_name + " 就绪";
      return std::format("{} · {} 错误 {} 警告", impl_->server_name, errors, warnings);
    }
    case st::lsp::SessionState::Failed: return "LSP 失败：" + impl_->client.error();
    case st::lsp::SessionState::ShuttingDown: return impl_->server_name + " 收尾中…";
    case st::lsp::SessionState::Stopped: return "LSP 已停止";
    case st::lsp::SessionState::Idle: break;
  }
  return "LSP 未启用";
}

auto LanguageService::active() const -> bool {
  return impl_->started && impl_->client.state() == st::lsp::SessionState::Ready;
}

auto LanguageService::offset_to_position(std::string_view path, std::size_t offset) const
    -> std::optional<st::lsp::Position> {
  const auto found = impl_->synced.find(std::string(path));
  if (found == impl_->synced.end()) return std::nullopt;
  // 行偏移表懒建（首次访问或文本变更后）：记录每行起始字节，之后二分定位。
  // 与直接调 `st::lsp::offset_to_position`（全文线性扫描）相比，热路径
  //（光标移动 → hover/补全/跳转的位置换算）从 O(file) 降到 O(log lines + 行内字节)。
  const auto index_found = impl_->line_index.find(std::string(path));
  if (index_found == impl_->line_index.end()) {
    std::vector<std::size_t> offsets;
    offsets.reserve(64);
    offsets.push_back(0);
    for (std::size_t at = 0; at < found->second.size(); ++at) {
      if (found->second[at] == '\n') offsets.push_back(at + 1);
    }
    impl_->line_index.emplace(std::string(path), std::move(offsets));
  }
  const std::vector<std::size_t>& offsets = impl_->line_index.find(std::string(path))->second;
  const std::size_t clamped = std::min(offset, found->second.size());
  // 二分找「起始偏移 ≤ clamped 的最后一行」，行内前缀交给协议层换算
  //（单行很短，线性无妨；UTF-16 语义必须复用它，不能自己重写）。
  const auto upper = std::upper_bound(offsets.begin(), offsets.end(), clamped);
  const std::size_t line = static_cast<std::size_t>(upper - offsets.begin()) - 1;
  const std::size_t line_begin = offsets[line];
  const std::size_t line_end = line + 1 < offsets.size() ? offsets[line + 1] - 1
                                                        : found->second.size();
  const std::string_view line_text =
      std::string_view(found->second).substr(line_begin, line_end - line_begin);
  const st::lsp::Position in_line = st::lsp::offset_to_position(line_text, clamped - line_begin);
  st::lsp::Position position{};
  position.line = line;
  position.character = in_line.character;
  return position;
}

auto LanguageService::request(std::string method, st::Json params) -> std::int64_t {
  if (!impl_->started) return 0;
  return impl_->client.request(std::move(method), std::move(params));
}

auto LanguageService::server_name() const -> std::string { return impl_->server_name; }

auto LanguageService::last_navigation_method() const -> const std::string& {
  return impl_->last_navigation_method;
}

auto LanguageService::error() const -> std::string { return impl_->error; }

auto LanguageService::active_path() const -> const std::string& { return impl_->active_path; }

void LanguageService::set_active_path(std::string path) {
  impl_->active_path = std::move(path);
}

auto LanguageService::request_completion(std::string_view path, std::uint32_t line,
                                        std::uint32_t character) -> std::int64_t {
  if (!impl_->started) return 0;
  st::Json params = st::Json::object();
  st::Json item = st::Json::object();
  item["uri"] = to_uri(path);
  params["textDocument"] = std::move(item);
  st::Json position = st::Json::object();
  position["line"] = line;
  position["character"] = character;
  params["position"] = std::move(position);
  // `context` 可选；给了能让 server 知道"是手动触发还是字符触发"（影响候选范围）。
  return impl_->client.request("textDocument/completion", std::move(params));
}

auto LanguageService::trigger_characters() const -> const std::vector<std::string>& {
  // `capabilities()` 返回**副本**——直接返回它的成员是返回临时对象的引用（实测编译报
  // "returning reference to temporary"）。缓存在 Impl 里。
  const auto& capabilities = impl_->client.capabilities();
  impl_->trigger_cache = capabilities.completion_trigger_characters;
  return impl_->trigger_cache;
}

auto LanguageService::resolve_completion(std::string_view path, std::size_t index)
    -> std::int64_t {
  (void)path;
  if (!impl_->started || index >= impl_->last_completion.size()) return 0;
  // `completionItem/resolve` 要求把**候选原始 JSON 原样回传**（server 靠它认项）。
  return impl_->client.request("completionItem/resolve", impl_->last_completion[index].raw);
}

namespace {

/// 组一个"文本位置"参数（`{textDocument:{uri}, position:{line, character}}`）。
[[nodiscard]] auto position_params(std::string_view path, std::uint32_t line,
                                   std::uint32_t character) -> st::Json {
  st::Json params = st::Json::object();
  st::Json item = st::Json::object();
  item["uri"] = st::lsp::LspClient::path_to_uri(path);
  params["textDocument"] = std::move(item);
  st::Json position = st::Json::object();
  position["line"] = line;
  position["character"] = character;
  params["position"] = std::move(position);
  return params;
}

}  // namespace

auto LanguageService::request_definition(std::string_view path, std::uint32_t line,
                                        std::uint32_t character) -> std::int64_t {
  if (!impl_->started) return 0;
  impl_->last_navigation_method = "definition";
  return impl_->client.request("textDocument/definition",
                              position_params(path, line, character));
}

auto LanguageService::request_declaration(std::string_view path, std::uint32_t line,
                                          std::uint32_t character) -> std::int64_t {
  if (!impl_->started) return 0;
  impl_->last_navigation_method = "declaration";
  return impl_->client.request("textDocument/declaration",
                              position_params(path, line, character));
}

auto LanguageService::request_type_definition(std::string_view path, std::uint32_t line,
                                              std::uint32_t character) -> std::int64_t {
  if (!impl_->started) return 0;
  impl_->last_navigation_method = "typeDefinition";
  return impl_->client.request("textDocument/typeDefinition",
                              position_params(path, line, character));
}

auto LanguageService::request_implementation(std::string_view path, std::uint32_t line,
                                             std::uint32_t character) -> std::int64_t {
  if (!impl_->started) return 0;
  impl_->last_navigation_method = "implementation";
  return impl_->client.request("textDocument/implementation",
                              position_params(path, line, character));
}

auto LanguageService::request_references(std::string_view path, std::uint32_t line,
                                        std::uint32_t character, bool include_declaration)
    -> std::int64_t {
  if (!impl_->started) return 0;
  impl_->last_navigation_method = "references";
  st::Json params = position_params(path, line, character);
  // `context.includeDeclaration`：是否把"声明本身"也算一处引用。
  // 找"谁用了它"时应当 false（否则第一项永远是定义处，用户每次都要跳过它）。
  st::Json context = st::Json::object();
  context["includeDeclaration"] = include_declaration;
  params["context"] = std::move(context);
  return impl_->client.request("textDocument/references", std::move(params));
}

auto LanguageService::request_hover(std::string_view path, std::uint32_t line,
                                    std::uint32_t character) -> std::int64_t {
  if (!impl_->started) return 0;
  return impl_->client.request("textDocument/hover", position_params(path, line, character));
}

auto LanguageService::request_document_symbols(std::string_view path) -> std::int64_t {
  if (!impl_->started) return 0;
  st::Json params = st::Json::object();
  st::Json item = st::Json::object();
  item["uri"] = st::lsp::LspClient::path_to_uri(path);
  params["textDocument"] = std::move(item);
  return impl_->client.request("textDocument/documentSymbol", std::move(params));
}

auto LanguageService::request_workspace_symbols(std::string_view query) -> std::int64_t {
  if (!impl_->started) return 0;
  st::Json params = st::Json::object();
  params["query"] = std::string(query);
  return impl_->client.request("workspace/symbol", std::move(params));
}

auto LanguageService::request_format(std::string_view path,
                                     const st::lsp::FormattingOptions& options) -> std::int64_t {
  if (!impl_->started) return 0;
  st::Json params = st::Json::object();
  st::Json item = st::Json::object();
  item["uri"] = to_uri(path);
  params["textDocument"] = std::move(item);
  params["options"] = st::lsp::formatting_options_json(options);
  return impl_->client.request("textDocument/formatting", std::move(params));
}

auto LanguageService::request_range_format(std::string_view path, std::uint32_t start_line,
                                           std::uint32_t start_character, std::uint32_t end_line,
                                           std::uint32_t end_character,
                                           const st::lsp::FormattingOptions& options)
    -> std::int64_t {
  if (!impl_->started) return 0;
  st::Json params = st::lsp::text_position_params(to_uri(path), start_line, start_character);
  st::Json range = st::Json::object();
  st::Json start = st::Json::object();
  start["line"] = start_line;
  start["character"] = start_character;
  st::Json end = st::Json::object();
  end["line"] = end_line;
  end["character"] = end_character;
  range["start"] = std::move(start);
  range["end"] = std::move(end);
  params["range"] = std::move(range);
  params["options"] = st::lsp::formatting_options_json(options);
  return impl_->client.request("textDocument/rangeFormatting", std::move(params));
}

auto LanguageService::request_prepare_rename(std::string_view path, std::uint32_t line,
                                             std::uint32_t character) -> std::int64_t {
  if (!impl_->started) return 0;
  return impl_->client.request("textDocument/prepareRename",
                              st::lsp::text_position_params(to_uri(path), line, character));
}

auto LanguageService::request_rename(std::string_view path, std::uint32_t line,
                                     std::uint32_t character, std::string_view new_name)
    -> std::int64_t {
  if (!impl_->started) return 0;
  st::Json params = st::lsp::text_position_params(to_uri(path), line, character);
  params["newName"] = std::string(new_name);
  return impl_->client.request("textDocument/rename", std::move(params));
}

void LanguageService::shutdown(std::int64_t timeout_ms) {
  if (impl_ == nullptr || !impl_->started) return;
  impl_->client.stop(timeout_ms);
  impl_->started = false;
  impl_->synced.clear();
  impl_->line_index.clear();
  impl_->diagnostic_fingerprint.clear();
}

// —— 仅供测试的注入口 ——

void LanguageService::sync_document_if_ready_for_test(std::string path, std::string text) {
  impl_->synced.insert_or_assign(path, std::move(text));
  impl_->line_index.erase(path);
}

void LanguageService::push_diagnostics_for_test(std::string uri, std::string message) {
  // 复用真实回调链（诊断→指纹→存储）——注入点在 `client.on_diagnostics` 上游，
  // 模拟的就是 server 推送这一层。先泵一次 client 让回调副本就位（handle 读的是
  // Impl 里的副本，它在 client.pump 里刷新——生产路径每帧都泵，测试路径要手动）。
  (void)impl_->client.pump();
  std::vector<st::lsp::Diagnostic> list;
  st::lsp::Diagnostic diagnostic{};
  diagnostic.message = std::move(message);
  diagnostic.severity = 1;
  diagnostic.range.start.line = 0;
  diagnostic.range.start.character = 0;
  list.push_back(std::move(diagnostic));
  impl_->client.inject_diagnostics_for_test(std::move(uri), std::move(list));
}

}  // namespace gbcode
