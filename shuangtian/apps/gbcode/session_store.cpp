#include "session_store.hpp"

/// 会话持久化的实现。JSON 结构（键序稳定，人可读、可 diff）：
///
/// ```json
/// {
///   "workspace": "C:/work/proj",
///   "files":     [{"path": "C:/work/proj/a.cpp", "line": 3, "column": 7}],
///   "active":    0,
///   "recent_files":   ["C:/work/proj/a.cpp"],
///   "recent_folders": ["C:/work/proj"]
/// }
/// ```
///
/// 为什么用 `st::Json`（ordered_json）而不是手拼文本：转义（路径里的 `\`、中文、
/// 空格）交给 JSON 库，读取侧类型安全（`json_get_*` 永不抛、缺键给默认值）。

#include "st/core/error.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"

namespace gbcode {

namespace {

/// MRU 上限（文件与文件夹各自的上限；与 `push_recent` 的默认值同源）。
constexpr std::size_t kRecentLimit = 10;

}  // namespace

auto default_session_path() -> std::string {
  // ST_HOME 与框架工具链的缓存目录（`~/.shuangtian/cache`）同一条约定：
  // 用户级数据都从这里起，测试/沙箱可以用 ST_HOME 重定向。
  std::string home = st::fs::read_env("ST_HOME").value_or("");
  if (home.empty()) {
    home = st::fs::home_dir();
    if (home.empty()) home = st::fs::temp_dir();   // 最后兜底：有总比没有好
  }
  return st::fs::join(st::fs::join(home, ".shuangtian"), "gbcode/session.json");
}


auto load_session(const std::string& path) -> SessionState {
  SessionState state;
  // 文件不存在：空态（首次启动的常态，不是错误）。
  if (!st::fs::exists(path)) return state;
  const auto parsed = st::json_parse_file(path);
  if (!parsed) return state;   // 损坏：空态（见文件头的失败安全姿态）
  const st::Json& doc = *parsed;
  if (!doc.is_object()) return state;

  state.workspace = st::json_get_string(doc, "workspace");
  state.active = static_cast<std::size_t>(
      std::max<std::int64_t>(0, st::json_get_i64(doc, "active", 0)));

  if (const st::Json* files = st::json_find(doc, "files"); files != nullptr && files->is_array()) {
    for (const auto& entry : *files) {
      if (!entry.is_object()) continue;
      SessionFile file;
      file.path = st::json_get_string(entry, "path");
      if (file.path.empty()) continue;
      file.line = static_cast<std::size_t>(
          std::max<std::int64_t>(1, st::json_get_i64(entry, "line", 1)));
      file.column = static_cast<std::size_t>(
          std::max<std::int64_t>(1, st::json_get_i64(entry, "column", 1)));
      state.files.push_back(std::move(file));
    }
  }
  state.recent_files = st::json_get_string_array(doc, "recent_files");
  state.recent_folders = st::json_get_string_array(doc, "recent_folders");
  return state;
}


auto save_session(const std::string& path, const SessionState& state) -> bool {
  st::Json doc = st::Json::object();
  doc["workspace"] = state.workspace;
  st::Json files = st::Json::array();
  for (const auto& file : state.files) {
    st::Json entry = st::Json::object();
    entry["path"] = file.path;
    entry["line"] = static_cast<std::int64_t>(file.line);
    entry["column"] = static_cast<std::int64_t>(file.column);
    files.push_back(std::move(entry));
  }
  doc["files"] = std::move(files);
  doc["active"] = static_cast<std::int64_t>(state.active);
  st::Json recent_files = st::Json::array();
  for (const auto& item : state.recent_files) recent_files.push_back(item);
  doc["recent_files"] = std::move(recent_files);
  st::Json recent_folders = st::Json::array();
  for (const auto& item : state.recent_folders) recent_folders.push_back(item);
  doc["recent_folders"] = std::move(recent_folders);

  if (!st::fs::ensure_parent(path)) return false;
  return st::json_write_file(path, doc).has_value();
}


auto push_recent(std::vector<std::string>& recents, std::string path, std::size_t limit) -> void {
  if (path.empty()) return;
  // 已在清单里：先删掉再头插（提到队首）。
  std::erase(recents, path);
  recents.insert(recents.begin(), std::move(path));
  if (recents.size() > limit) recents.resize(limit);
}

}  // namespace gbcode
