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
          .args = {"--log-file", "/dev/null"},
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
  /// 全部诊断（按 URI 存；只有活动文件的给 UI）。
  std::map<std::string, std::vector<LspProblem>, std::less<>> diagnostics{};
  /// 最近一次诊断更新的 uri（pump 返回 true 时用）。
  std::string last_updated{};
  /// 最近一次补全候选（`completionItem/resolve` 要用原始 JSON 回传）。
  std::vector<st::lsp::CompletionEntry> last_completion{};
  /// 触发字符缓存（`capabilities()` 返回副本，不能返回其成员引用）。
  mutable std::vector<std::string> trigger_cache{};
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
    impl_->diagnostics.clear();
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
}

void LanguageService::close_document(std::string_view file_path) {
  if (!impl_->started) return;
  const std::string path(file_path);
  impl_->synced.erase(path);
  impl_->diagnostics.erase(path);
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
  return st::lsp::offset_to_position(found->second, offset);
}

auto LanguageService::request(std::string method, st::Json params) -> std::int64_t {
  if (!impl_->started) return 0;
  return impl_->client.request(std::move(method), std::move(params));
}

auto LanguageService::server_name() const -> std::string { return impl_->server_name; }

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
  return impl_->client.request("textDocument/definition",
                              position_params(path, line, character));
}

auto LanguageService::request_references(std::string_view path, std::uint32_t line,
                                        std::uint32_t character, bool include_declaration)
    -> std::int64_t {
  if (!impl_->started) return 0;
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

void LanguageService::shutdown(std::int64_t timeout_ms) {
  if (impl_ == nullptr || !impl_->started) return;
  impl_->client.stop(timeout_ms);
  impl_->started = false;
  impl_->synced.clear();
}

}  // namespace gbcode
