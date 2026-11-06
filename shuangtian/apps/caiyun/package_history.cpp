#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <format>

#include "package.hpp"
#include "st/core/fs.hpp"

namespace caiyun {
namespace {

/// 历史文件名：按日期分卷。
///
/// 为何分卷而不是单个 `history.jsonl`：一个长期工程的日志会长到几十万行，
/// 单文件会让"看最近发生了什么"必须先载入全部。分卷后按日期就能定位。
[[nodiscard]] auto today_file_name() -> std::string {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  return std::format("{:04d}-{:02d}-{:02d}.jsonl", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday);
}

[[nodiscard]] auto now_iso8601_local() -> std::string {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z", tm.tm_year + 1900,
                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/// 把一行 JSONL 解析成 `HistoryEntry`。解析失败返回 nullopt（**跳过该行**）。
///
/// 为何跳过而不是整体失败：日志是**追加**的，崩溃可能留下半行。
/// 一条坏行不该让整段历史读不出来——那会把"查看历史"变成"看不了历史"。
[[nodiscard]] auto parse_entry(std::string_view line) -> std::optional<HistoryEntry> {
  if (line.empty()) return std::nullopt;
  const auto json = st::json_parse(line);
  if (!json || !json->is_object()) return std::nullopt;
  HistoryEntry entry{};
  entry.seq = static_cast<std::int64_t>(st::json_get_double(*json, "seq", 0.0));
  entry.at = st::json_get_string(*json, "at", "");
  entry.action = st::json_get_string(*json, "action", "");
  entry.actor = st::json_get_string(*json, "actor", "");
  entry.summary = st::json_get_string(*json, "summary", "");
  if (const st::Json* detail = st::json_find(*json, "detail"); detail != nullptr) {
    entry.detail = *detail;
  }
  if (entry.seq <= 0) return std::nullopt;
  return entry;
}

}  // namespace

auto append_history(std::string_view root, HistoryEntry entry) -> Result<std::int64_t> {
  const auto manifest = read_manifest(root);
  if (!manifest) return st::forward_error(manifest.error());
  const std::string history_dir =
      manifest->history_dir.empty() ? st::fs::join(std::string(root), "history")
                                    : st::fs::join(std::string(root), manifest->history_dir);
  if (const auto made = st::fs::create_directories(history_dir); !made) {
    return st::forward_error(made.error());
  }

  // 序号取"现有最大值 + 1"。读全部是为了正确性（分卷后最大值可能在任意卷）。
  // 代价可接受：历史是低频写入（人/Agent 的每次动作一条），不是每帧。
  const auto existing = read_history(root);
  std::int64_t next_seq = 1;
  if (existing) {
    for (const auto& old : *existing) {
      if (old.seq >= next_seq) next_seq = old.seq + 1;
    }
  }

  entry.seq = next_seq;
  if (entry.at.empty()) entry.at = now_iso8601_local();

  st::Json json = st::Json::object();
  // 一行一条，键序固定：seq 在最前，便于 `head -1` 之类的工具直接看到顺序。
  json["seq"] = entry.seq;
  json["at"] = entry.at;
  json["action"] = entry.action;
  json["actor"] = entry.actor;
  json["summary"] = entry.summary;
  if (!entry.detail.is_null()) json["detail"] = entry.detail;

  // `dump()`（紧凑）而不是 `dump(2)`：JSONL 里一行必须是一条完整记录，
  // 缩进换行会把一条记录拆成多行，解析时就只剩第一行（而且那行还不是合法 JSON）。
  const std::string line = json.dump() + "\n";
  const std::string path = st::fs::join(history_dir, today_file_name());
  if (const auto appended = st::fs::append_text(path, line); !appended) {
    return st::forward_error(appended.error());
  }
  return next_seq;
}

auto read_history(std::string_view root) -> Result<std::vector<HistoryEntry>> {
  const auto manifest = read_manifest(root);
  if (!manifest) return st::forward_error(manifest.error());
  const std::string history_dir =
      manifest->history_dir.empty() ? st::fs::join(std::string(root), "history")
                                    : st::fs::join(std::string(root), manifest->history_dir);
  std::vector<HistoryEntry> out;
  if (!st::fs::is_directory(history_dir)) return out;  // 还没有历史：空列表，不是错误

  const auto entries = st::fs::list_dir(history_dir);
  if (!entries) return st::forward_error(entries.error());

  std::vector<std::string> files;
  for (const auto& entry : *entries) {
    if (entry.is_dir) continue;
    if (entry.name.size() > 6 && entry.name.substr(entry.name.size() - 6) == ".jsonl") {
      files.push_back(entry.path);
    }
  }
  // 文件名按日期命名，字典序即时间序。
  std::sort(files.begin(), files.end());

  for (const std::string& file : files) {
    const auto text = st::fs::read_text(file);
    if (!text) continue;  // 单卷读失败不阻断其余卷
    std::size_t pos = 0;
    while (pos < text->size()) {
      const std::size_t next = text->find('\n', pos);
      const std::size_t end = next == std::string::npos ? text->size() : next;
      if (const auto parsed = parse_entry(std::string_view(*text).substr(pos, end - pos))) {
        out.push_back(*parsed);
      }
      if (next == std::string::npos) break;
      pos = next + 1;
    }
  }

  std::sort(out.begin(), out.end(),
            [](const HistoryEntry& a, const HistoryEntry& b) { return a.seq < b.seq; });
  return out;
}

}  // namespace caiyun
