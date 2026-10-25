#include <format>

#include "cefu_index.hpp"
#include "st/ext/database.hpp"

namespace cefu {

/// Store 的 SQLite 实现。
///
/// ## 表结构为什么是一张宽表 + 若干 JSON 列
///
/// 词条的列表字段（aliases/steps/causes/facts/commands/links/tags）数量不定。两条路：
///
/// - **多表范式化**（entry + entry_aliases + entry_steps…）：查一条要 N 次 join，
///   而这个库的使用模式是「启动全量装载 + 单条写入」，join 成本纯属浪费。
/// - **宽表 + 列表存 JSON 文本**：一次 SELECT 拿全集；单条写入一次 INSERT。
///
/// 选后者。代价是列表字段在 SQL 里不能直接 `WHERE alias = ?`——但那些查询本来就该走
/// 内存索引（我们的检索引擎），SQL 在这一层只需要「原子、可事务、能用 sqlite3 命令行看」。
///
/// `text_zh` 列存一份「所有字段拼起来的中文文本」，只为人工用 sqlite3 做 LIKE 兜底查询。
struct Store::Impl {
  st::ext::Database db;

  static auto schema() -> std::string_view {
    return R"sql(
CREATE TABLE IF NOT EXISTS entries (
  id           TEXT PRIMARY KEY,
  category     TEXT NOT NULL,
  title        TEXT NOT NULL,
  aliases      TEXT NOT NULL DEFAULT '[]',
  symptom      TEXT NOT NULL DEFAULT '',
  causes       TEXT NOT NULL DEFAULT '[]',
  steps        TEXT NOT NULL DEFAULT '[]',
  commands     TEXT NOT NULL DEFAULT '[]',
  facts        TEXT NOT NULL DEFAULT '[]',
  owner        TEXT NOT NULL DEFAULT '',
  links        TEXT NOT NULL DEFAULT '[]',
  tags         TEXT NOT NULL DEFAULT '[]',
  scope        TEXT NOT NULL DEFAULT '{"kind":"global"}',
  source_kind  TEXT NOT NULL DEFAULT '',
  source_ref   TEXT NOT NULL DEFAULT '',
  source_at    INTEGER NOT NULL DEFAULT 0,
  confidence   TEXT NOT NULL DEFAULT 'inferred',
  created_at   INTEGER NOT NULL DEFAULT 0,
  updated_at   INTEGER NOT NULL DEFAULT 0,
  expires_at   INTEGER NOT NULL DEFAULT 0,
  supersedes   TEXT NOT NULL DEFAULT '',
  hits         INTEGER NOT NULL DEFAULT 0,
  last_hit_at  INTEGER NOT NULL DEFAULT 0,
  needs_review INTEGER NOT NULL DEFAULT 0,
  search_text  TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_entries_category ON entries(category);
CREATE INDEX IF NOT EXISTS idx_entries_confidence ON entries(confidence);
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
)sql";
  }
};

namespace {

auto to_json_array(const std::vector<std::string>& values) -> std::string {
  Json arr = Json::array();
  for (const auto& v : values) arr.push_back(Json(v));
  return arr.dump();
}

auto from_json_array(std::string_view text, std::vector<std::string>& out) -> void {
  out.clear();
  if (text.empty()) return;
  const auto parsed = Json::parse(std::string(text));
  if (parsed.is_discarded() || !parsed.is_array()) return;
  for (const auto& item : parsed) {
    if (item.is_string()) out.push_back(item.get<std::string>());
  }
}

/// 人工 sqlite3 LIKE 查询用的扁平文本。
auto build_search_text(const Entry& e) -> std::string {
  std::string out;
  const auto append = [&](std::string_view v) {
    if (v.empty()) return;
    if (!out.empty()) out.push_back(' ');
    out += v;
  };
  append(e.title);
  append(e.category);
  append(e.symptom);
  append(e.owner);
  for (const auto& v : e.aliases) append(v);
  for (const auto& v : e.causes) append(v);
  for (const auto& v : e.steps) append(v);
  for (const auto& v : e.commands) append(v);
  for (const auto& v : e.facts) append(v);
  for (const auto& v : e.tags) append(v);
  return out;
}

auto col_text(const st::ext::QueryResult& rows, std::size_t row, std::size_t col) -> std::string {
  const auto& value = rows.rows[row][col];
  if (const auto* s = std::get_if<std::string>(&value)) return *s;
  return {};
}

auto col_int(const st::ext::QueryResult& rows, std::size_t row, std::size_t col) -> std::int64_t {
  const auto& value = rows.rows[row][col];
  if (const auto* i = std::get_if<std::int64_t>(&value)) return *i;
  if (const auto* d = std::get_if<double>(&value)) return static_cast<std::int64_t>(*d);
  return 0;
}

}  // namespace

Store::Store() = default;
Store::~Store() = default;
Store::Store(Store&&) noexcept = default;
auto Store::operator=(Store&&) noexcept -> Store& = default;

auto Store::open(std::string_view path) -> st::Result<Store> {
  st::ext::OpenOptions options;
  options.create = true;
  options.busy_timeout = std::chrono::milliseconds{5000};
  auto db = st::ext::Database::open(path, options);
  if (!db.has_value()) return st::unexpected(db.error().code, db.error().message);

  Store store;
  store.impl_ = std::make_unique<Impl>();
  store.impl_->db = std::move(*db);
  if (auto status = store.impl_->db.exec(Impl::schema()); !status.has_value()) {
    return st::unexpected(status.error().code, status.error().message);
  }
  return store;
}

auto Store::load_all() -> st::Result<std::vector<Entry>> {
  const auto result = impl_->db.query(
      "SELECT id, category, title, aliases, symptom, causes, steps, commands, facts, owner, "
      "links, tags, scope, source_kind, source_ref, source_at, confidence, created_at, "
      "updated_at, expires_at, supersedes, hits, last_hit_at, needs_review "
      "FROM entries ORDER BY category, title");
  if (!result.has_value()) return st::unexpected(result.error().code, result.error().message);

  std::vector<Entry> out;
  out.reserve(result->rows.size());
  for (std::size_t r = 0; r < result->rows.size(); ++r) {
    Entry e;
    e.id = col_text(*result, r, 0);
    e.category = col_text(*result, r, 1);
    e.title = col_text(*result, r, 2);
    from_json_array(col_text(*result, r, 3), e.aliases);
    e.symptom = col_text(*result, r, 4);
    from_json_array(col_text(*result, r, 5), e.causes);
    from_json_array(col_text(*result, r, 6), e.steps);
    from_json_array(col_text(*result, r, 7), e.commands);
    from_json_array(col_text(*result, r, 8), e.facts);
    e.owner = col_text(*result, r, 9);
    from_json_array(col_text(*result, r, 10), e.links);
    from_json_array(col_text(*result, r, 11), e.tags);
    e.scope = col_text(*result, r, 12);
    e.source_kind = col_text(*result, r, 13);
    e.source_ref = col_text(*result, r, 14);
    e.source_at = col_int(*result, r, 15);
    e.confidence = col_text(*result, r, 16);
    e.created_at = col_int(*result, r, 17);
    e.updated_at = col_int(*result, r, 18);
    e.expires_at = col_int(*result, r, 19);
    e.supersedes = col_text(*result, r, 20);
    e.hits = col_int(*result, r, 21);
    e.last_hit_at = col_int(*result, r, 22);
    e.needs_review = col_int(*result, r, 23) != 0;
    if (e.confidence.empty()) e.confidence = "inferred";
    if (e.scope.empty()) e.scope = R"({"kind":"global"})";
    out.push_back(std::move(e));
  }
  return out;
}

auto Store::upsert(const Entry& entry) -> st::Result<bool> {
  const auto exists = impl_->db.query("SELECT 1 FROM entries WHERE id = ?", {st::ext::value_text(entry.id)});
  if (!exists.has_value()) return st::unexpected(exists.error().code, exists.error().message);
  const bool is_new = exists->rows.empty();

  const auto sql = R"sql(
INSERT INTO entries (id, category, title, aliases, symptom, causes, steps, commands, facts, owner,
                    links, tags, scope, source_kind, source_ref, source_at, confidence,
                    created_at, updated_at, expires_at, supersedes, hits, last_hit_at,
                    needs_review, search_text)
VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(id) DO UPDATE SET
  category=excluded.category, title=excluded.title, aliases=excluded.aliases,
  symptom=excluded.symptom, causes=excluded.causes, steps=excluded.steps,
  commands=excluded.commands, facts=excluded.facts, owner=excluded.owner,
  links=excluded.links, tags=excluded.tags, scope=excluded.scope,
  source_kind=excluded.source_kind, source_ref=excluded.source_ref, source_at=excluded.source_at,
  confidence=excluded.confidence, updated_at=excluded.updated_at, expires_at=excluded.expires_at,
  supersedes=excluded.supersedes, hits=excluded.hits, last_hit_at=excluded.last_hit_at,
  needs_review=excluded.needs_review, search_text=excluded.search_text
)sql";
  const std::vector<st::ext::Value> args{
      st::ext::value_text(entry.id),
      st::ext::value_text(entry.category),
      st::ext::value_text(entry.title),
      st::ext::value_text(to_json_array(entry.aliases)),
      st::ext::value_text(entry.symptom),
      st::ext::value_text(to_json_array(entry.causes)),
      st::ext::value_text(to_json_array(entry.steps)),
      st::ext::value_text(to_json_array(entry.commands)),
      st::ext::value_text(to_json_array(entry.facts)),
      st::ext::value_text(entry.owner),
      st::ext::value_text(to_json_array(entry.links)),
      st::ext::value_text(to_json_array(entry.tags)),
      st::ext::value_text(entry.scope),
      st::ext::value_text(entry.source_kind),
      st::ext::value_text(entry.source_ref),
      st::ext::value_integer(entry.source_at),
      st::ext::value_text(entry.confidence),
      st::ext::value_integer(entry.created_at),
      st::ext::value_integer(entry.updated_at),
      st::ext::value_integer(entry.expires_at),
      st::ext::value_text(entry.supersedes),
      st::ext::value_integer(entry.hits),
      st::ext::value_integer(entry.last_hit_at),
      st::ext::value_integer(entry.needs_review ? 1 : 0),
      st::ext::value_text(build_search_text(entry)),
  };
  auto written = impl_->db.execute(sql, args);
  if (!written.has_value()) return st::unexpected(written.error().code, written.error().message);
  return is_new;
}

auto Store::remove(std::string_view id) -> st::Result<bool> {
  auto changed = impl_->db.execute("DELETE FROM entries WHERE id = ?", {st::ext::value_text(id)});
  if (!changed.has_value()) return st::unexpected(changed.error().code, changed.error().message);
  return *changed > 0;
}

auto Store::bump_hits(std::string_view id, std::int64_t at) -> st::Status {
  auto changed = impl_->db.execute(
      "UPDATE entries SET hits = hits + 1, last_hit_at = ? WHERE id = ?",
      {st::ext::value_integer(at), st::ext::value_text(id)});
  if (!changed.has_value()) return st::unexpected(changed.error().code, changed.error().message);
  return {};
}

auto Store::categories() -> st::Result<std::vector<std::pair<std::string, std::int64_t>>> {
  const auto result =
      impl_->db.query("SELECT category, COUNT(*) FROM entries GROUP BY category ORDER BY category");
  if (!result.has_value()) return st::unexpected(result.error().code, result.error().message);
  std::vector<std::pair<std::string, std::int64_t>> out;
  out.reserve(result->rows.size());
  for (std::size_t r = 0; r < result->rows.size(); ++r) {
    out.emplace_back(col_text(*result, r, 0), col_int(*result, r, 1));
  }
  return out;
}

}  // namespace cefu
