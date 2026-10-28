/// 册府（cefu）边车——把知识库能力交给模型。
///
/// ## 与 `examples/cefu`（带界面的应用）的关系
///
/// | | `examples/cefu`（应用） | 本边车 |
/// |---|---|---|
/// | 使用者 | 人（翻书、改、整理） | 模型（检索、写入、巡检） |
/// | 依赖 | 霜天全套（窗口/脚本）+ SQLite | **零依赖**（纯标准库） |
/// | 存储 | SQLite（可事务、可 sqlite3 直查） | JSON 落盘（千级词条，全量读写 <1ms） |
///
/// 引擎规则（分词/加权/阈值）在 `cefu_index.hpp`，与应用的语义一致；
/// **代码各自一份**是刻意的——共享会把界面依赖拖进边车。
///
/// ## 工具面
///
/// | 工具 | 用途 |
/// |---|---|
/// | `search` | 检索（摘要 + 分数 + 为何命中） |
/// | `get` | 取词条全文（计命中） |
/// | `put` | 结构化写入（强制 title/category/source，近似查重） |
/// | `list` | 按分类成册浏览 |
/// | `review` | 整理巡检（重复/过期/冷条目/分类漂移） |
/// | `confirm` | 标为已确认 |
/// | `stats` | 概览 |
///
/// 宿主注册时自动加 `cefu_` 前缀（`cefu_search` 等）。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "../framework.hpp"
#include "cefu_index.hpp"

namespace {

using gebai::Json;
using gebai::ToolResult;

/// 定点格式化（**不用 std::format**：边车由 build.bat 以 C++17 编译，
/// 而 std::format 是 C++20——用它会在这条构建路径上直接编译失败）。
template <typename... Args>
auto fmt(const char* pattern, Args... args) -> std::string {
  char buf[1024];
  const int n = std::snprintf(buf, sizeof buf, pattern, args...);
  if (n < 0) return {};
  const auto len = static_cast<std::size_t>(n);
  return std::string(buf, len < sizeof buf ? len : sizeof buf - 1);
}

inline constexpr std::int64_t kDayMs = 86'400'000;

auto now_ms() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/* ══════════════ JSON 小工具 ══════════════ */

auto str_list(const std::vector<std::string>& v) -> Json {
  Json a = Json::makeArr();
  for (const auto& s : v) a.arr.push_back(Json::makeStr(s));
  return a;
}

auto read_list(const Json& obj, const char* key) -> std::vector<std::string> {
  std::vector<std::string> out;
  const Json* j = obj.get(key);
  if (j == nullptr || !j->isArr()) return out;
  for (const auto& v : j->arr) {
    if (v.isStr()) out.push_back(v.str);
  }
  return out;
}

/* ══════════════ 存储 ══════════════ */

/// 数据文件路径：`$CEFU_DB` 优先，否则 `$GEBAI_HOME/data/knowledge/cefu.json`。
///
/// **为何认 GEBAI_HOME 而不是 USERPROFILE**：外部进程（本边车）只看得到宿主注入的环境变量，
/// 而歌白所有运行时数据都归在 `{GEBAI_HOME}/data/` 下（与 persona/相期同一约定）。
/// 旧版曾用 `<家目录>/knowledge/`，于是数据散落到用户家目录、与歌白数据分家；
/// 宿主现在还会显式注入 `CEFU_DB`（带旧位置回退），本函数只是无注入时的兜底。
///
/// 为什么是 JSON 而不是 SQLite：边车要**零依赖**（纯标准库即可编译），
/// 而 SQLite 要么链 amalgamation（~250KB 源码、每次构建多几秒），
/// 要么依赖系统库（换机器就缺）。千级词条的全量读写 <1ms，JSON 足够；
/// 附带好处是可 diff、可手工编辑、出问题肉眼能看。
auto data_path() -> std::string {
  if (const char* env = std::getenv("CEFU_DB"); env != nullptr && *env != '\0') return env;
  if (const char* gebai = std::getenv("GEBAI_HOME"); gebai != nullptr && *gebai != '\0') {
    return std::string(gebai) + "/data/knowledge/cefu.json";
  }
  std::string home;
  if (const char* h = std::getenv("USERPROFILE"); h != nullptr) home = h;
  else if (const char* h2 = std::getenv("HOME"); h2 != nullptr) home = h2;
  if (home.empty()) return "cefu.json";
  return home + "/knowledge/cefu.json";
}

struct Store {
  std::string path;
  cefu::Index index;

  auto load() -> void {
    path = data_path();
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty()) return;
    bool ok = false;
    const Json arr = Json::parse(text, &ok);
    if (!ok || !arr.isArr()) return;
    for (const auto& o : arr.arr) {
      if (!o.isObj()) continue;
      cefu::Entry e;
      e.id = o.getStr("id");
      e.category = o.getStr("category");
      e.title = o.getStr("title");
      e.aliases = read_list(o, "aliases");
      e.symptom = o.getStr("symptom");
      e.causes = read_list(o, "causes");
      e.steps = read_list(o, "steps");
      e.commands = read_list(o, "commands");
      e.facts = read_list(o, "facts");
      e.owner = o.getStr("owner");
      e.links = read_list(o, "links");
      e.tags = read_list(o, "tags");
      e.scope = o.getStr("scope", R"({"kind":"global"})");
      e.source_kind = o.getStr("source_kind");
      e.source_ref = o.getStr("source_ref");
      e.source_at = static_cast<std::int64_t>(o.getNum("source_at", 0));
      e.confidence = o.getStr("confidence", "inferred");
      e.created_at = static_cast<std::int64_t>(o.getNum("created_at", 0));
      e.updated_at = static_cast<std::int64_t>(o.getNum("updated_at", 0));
      e.expires_at = static_cast<std::int64_t>(o.getNum("expires_at", 0));
      e.supersedes = o.getStr("supersedes");
      e.hits = static_cast<std::int64_t>(o.getNum("hits", 0));
      e.last_hit_at = static_cast<std::int64_t>(o.getNum("last_hit_at", 0));
      const Json* nr = o.get("needs_review");
      e.needs_review = nr != nullptr && nr->isBool() && nr->b;
      if (!e.id.empty()) index.upsert(e);
    }
    index.recompute();
  }

  auto save() const -> bool {
    Json arr = Json::makeArr();
    for (const auto& e : index.entries()) {
      Json o = Json::makeObj();
      o.set("id", Json::makeStr(e.id));
      o.set("category", Json::makeStr(e.category));
      o.set("title", Json::makeStr(e.title));
      o.set("aliases", str_list(e.aliases));
      o.set("symptom", Json::makeStr(e.symptom));
      o.set("causes", str_list(e.causes));
      o.set("steps", str_list(e.steps));
      o.set("commands", str_list(e.commands));
      o.set("facts", str_list(e.facts));
      o.set("owner", Json::makeStr(e.owner));
      o.set("links", str_list(e.links));
      o.set("tags", str_list(e.tags));
      o.set("scope", Json::makeStr(e.scope));
      o.set("source_kind", Json::makeStr(e.source_kind));
      o.set("source_ref", Json::makeStr(e.source_ref));
      o.set("source_at", Json::makeNum(static_cast<double>(e.source_at)));
      o.set("confidence", Json::makeStr(e.confidence));
      o.set("created_at", Json::makeNum(static_cast<double>(e.created_at)));
      o.set("updated_at", Json::makeNum(static_cast<double>(e.updated_at)));
      o.set("expires_at", Json::makeNum(static_cast<double>(e.expires_at)));
      o.set("supersedes", Json::makeStr(e.supersedes));
      o.set("hits", Json::makeNum(static_cast<double>(e.hits)));
      o.set("last_hit_at", Json::makeNum(static_cast<double>(e.last_hit_at)));
      o.set("needs_review", Json::makeBool(e.needs_review));
      arr.arr.push_back(std::move(o));
    }
    // 先建父目录（家目录下 knowledge/ 可能不存在）
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) {
      const auto dir = path.substr(0, slash);
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << arr.dump();
    return out.good();
  }
};

auto store() -> Store& {
  static Store s;
  static bool loaded = false;
  if (!loaded) {
    s.load();
    loaded = true;
  }
  return s;
}

/* ══════════════ 输出整形 ══════════════ */

auto summary_json(const cefu::Entry& e) -> Json {
  Json o = Json::makeObj();
  o.set("id", Json::makeStr(e.id));
  o.set("title", Json::makeStr(e.title));
  o.set("category", Json::makeStr(e.category));
  o.set("confidence", Json::makeStr(e.confidence));
  o.set("hits", Json::makeNum(static_cast<double>(e.hits)));
  o.set("needs_review", Json::makeBool(e.needs_review));
  o.set("aliases", str_list(e.aliases));
  o.set("key_line", Json::makeStr(e.key_line()));
  return o;
}

auto full_json(const cefu::Entry& e) -> Json {
  Json o = summary_json(e);
  o.set("symptom", Json::makeStr(e.symptom));
  o.set("causes", str_list(e.causes));
  o.set("steps", str_list(e.steps));
  o.set("commands", str_list(e.commands));
  o.set("facts", str_list(e.facts));
  o.set("owner", Json::makeStr(e.owner));
  o.set("links", str_list(e.links));
  o.set("tags", str_list(e.tags));
  o.set("scope", Json::makeStr(e.scope));
  o.set("source_kind", Json::makeStr(e.source_kind));
  o.set("source_ref", Json::makeStr(e.source_ref));
  o.set("created_at", Json::makeNum(static_cast<double>(e.created_at)));
  o.set("updated_at", Json::makeNum(static_cast<double>(e.updated_at)));
  o.set("expires_at", Json::makeNum(static_cast<double>(e.expires_at)));
  o.set("supersedes", Json::makeStr(e.supersedes));
  o.set("last_hit_at", Json::makeNum(static_cast<double>(e.last_hit_at)));
  return o;
}

/// 词条全文的可读文本（模型读的是文字，不是 JSON）。
auto entry_text(const cefu::Entry& e) -> std::string {
  std::string s = fmt("%s（%s）\n", e.title.c_str(), e.category.c_str());
  if (!e.aliases.empty()) {
    s += "也叫：";
    for (std::size_t i = 0; i < e.aliases.size(); ++i) s += (i > 0 ? "、" : "") + e.aliases[i];
    s += "\n";
  }
  if (!e.symptom.empty()) s += "现象：" + e.symptom + "\n";
  if (!e.causes.empty()) {
    s += "可能原因：\n";
    for (const auto& c : e.causes) s += "  - " + c + "\n";
  }
  if (!e.steps.empty()) {
    s += "处理步骤（有序，第 1 条最该先做）：\n";
    for (std::size_t i = 0; i < e.steps.size(); ++i) {
      s += fmt("  %zu. %s\n", i + 1, e.steps[i].c_str());
    }
  }
  if (!e.commands.empty()) {
    s += "命令：\n";
    for (const auto& c : e.commands) s += "  $ " + c + "\n";
  }
  if (!e.facts.empty()) {
    s += "硬事实：\n";
    for (const auto& f : e.facts) s += "  - " + f + "\n";
  }
  if (!e.owner.empty()) s += "负责人：" + e.owner + "\n";
  s += fmt("出处：%s / %s（置信度 %s，命中 %lld 次）\n",
           e.source_kind.empty() ? "—" : e.source_kind.c_str(),
           e.source_ref.empty() ? "—" : e.source_ref.c_str(), e.confidence.c_str(),
           static_cast<long long>(e.hits));
  return s;
}

auto make_id(cefu::Index& index, std::string_view title) -> std::string {
  std::string slug;
  for (const char c : title) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) slug.push_back(c);
    else if ((c >= 'A' && c <= 'Z')) slug.push_back(static_cast<char>(c - 'A' + 'a'));
    else if ((c == ' ' || c == '-' || c == '_') && !slug.empty() && slug.back() != '-') slug.push_back('-');
  }
  while (!slug.empty() && slug.back() == '-') slug.pop_back();
  if (slug.empty()) slug = "entry";
  std::string id = "e_" + slug;
  int n = 2;
  while (index.find(id) != nullptr) id = fmt("e_%s_%d", slug.c_str(), n++);
  return id;
}

/* ══════════════ 工具 ══════════════ */

auto tool_search(const Json& args) -> ToolResult {
  const auto query = args.getStr("query");
  if (query.empty()) return ToolResult{"cefu_search 需要 query", Json(), false};
  const auto limit = args.getNum("limit", 5) > 0 ? static_cast<std::size_t>(args.getNum("limit", 5)) : std::size_t{5};
  auto hits = store().index.search(query, limit);
  if (hits.empty()) {
    return ToolResult{fmt("册府里没有与「%s」相关的词条。\n"
       "如果这确实是一条该记住的知识，用 cefu_put 写进来（必须给 title / category / source_ref）。",
       query.c_str()), Json::makeArr(), true};
  }
  Json arr = Json::makeArr();
  std::string text = fmt("册府命中 %zu 条：", hits.size());
  for (const auto& h : hits) {
    Json item = summary_json(*h.entry);
    item.set("score", Json::makeNum(h.score));
    item.set("why", str_list(h.why));
    arr.arr.push_back(std::move(item));
    text += fmt("\n· [%s] %s（%s）— %s", h.entry->id.c_str(), h.entry->title.c_str(),
       h.entry->category.c_str(), h.entry->key_line().c_str());
  }
  text += "\n（要全文用 cefu_get）";
  return ToolResult{std::move(text), std::move(arr), true};
}

auto tool_get(const Json& args) -> ToolResult {
  const auto id = args.getStr("id");
  if (id.empty()) return ToolResult{"cefu_get 需要 id", Json(), false};
  auto& idx = store().index;
  const auto* found = idx.find(id);
  if (found == nullptr) {
    return ToolResult{fmt("册府里没有词条：%s（用 cefu_search 或 cefu_list 先找 id）", id.c_str()), Json(), false};
  }
  // 计一次命中：必须改实体的副本再写回（index 的 entries() 是只读视图）
  cefu::Entry copy = *found;
  copy.hits += 1;
  copy.last_hit_at = now_ms();
  idx.upsert(copy);
  (void)store().save();
  return ToolResult{entry_text(copy), full_json(copy), true};
}

auto tool_put(const Json& args) -> ToolResult {
  cefu::Entry e;
  e.title = args.getStr("title");
  e.category = args.getStr("category");
  e.source_ref = args.getStr("source_ref");
  // 三条硬约束：没有标题与分类的条目无法成册浏览；没有出处的知识事后无法判断该不该信
  if (e.title.empty() || e.category.empty()) {
    return ToolResult{"册府要求 title 与 category：没有标题与分类的条目无法成册浏览，也就无法被找到。", Json(), false};
  }
  if (e.source_ref.empty()) {
    return ToolResult{"册府要求 source_ref（从哪知道的：文件:行号 / 会话 id / 人名）——"
                      "没有出处的知识事后无法判断该不该信。", Json(), false};
  }
  e.id = args.getStr("id");
  e.symptom = args.getStr("symptom");
  e.owner = args.getStr("owner");
  e.aliases = read_list(args, "aliases");
  e.causes = read_list(args, "causes");
  e.steps = read_list(args, "steps");
  e.commands = read_list(args, "commands");
  e.facts = read_list(args, "facts");
  e.tags = read_list(args, "tags");
  e.links = read_list(args, "links");
  e.source_kind = args.getStr("source_kind", "tool");
  e.confidence = args.getStr("confidence", "inferred");
  e.scope = R"({"kind":"global"})";
  const auto t = now_ms();
  e.created_at = t;
  e.updated_at = t;
  const bool is_new = e.id.empty() || store().index.find(e.id) == nullptr;
  if (e.id.empty()) e.id = make_id(store().index, e.title);

  // 查重：同册高度相似时**不静默新增**，把既有词条交回去让人（模型）决定
  for (const auto& other : store().index.entries()) {
    if (other.id == e.id || other.category != e.category) continue;
    if (const auto sim = cefu::Index::similarity(other, e); sim >= 0.6) {
      return ToolResult{fmt("与既有词条「%s」（%s）相似度 %.2f，疑似同一条知识。"
       "要补充它就沿用该 id 重新写，要另建请让标题体现区别。",
       other.title.c_str(), other.id.c_str(), sim), full_json(other), false};
    }
  }
  store().index.upsert(e);
  store().index.recompute();
  (void)store().save();
  return ToolResult{fmt("已写入册府：%s（%s）\n%s", e.id.c_str(), is_new ? "新建" : "更新", entry_text(e).c_str()), full_json(e), true};
}

auto tool_list(const Json& args) -> ToolResult {
  const auto cat = args.getStr("category");
  std::map<std::string, std::vector<const cefu::Entry*>, std::less<>> buckets;
  for (const auto& e : store().index.entries()) {
    if (!cat.empty() && e.category != cat) continue;
    buckets[e.category].push_back(&e);
  }
  if (buckets.empty()) {
    return ToolResult{cat.empty() ? "册府还是空的（用 cefu_put 写第一条）"
                                  : fmt("册府里没有「%s」这一册", cat.c_str()), Json::makeArr(), true};
  }
  Json arr = Json::makeArr();
  std::string text;
  for (const auto& [c, list] : buckets) {
    text += fmt("\n【%s】（%zu 条）", c.c_str(), list.size());
    for (const auto* ep : list) {
      text += fmt("\n  · %s — %s", ep->id.c_str(), ep->title.c_str());
      arr.arr.push_back(summary_json(*ep));
    }
  }
  return ToolResult{"册府目录：" + text, std::move(arr), true};
}

auto tool_review(const Json& args) -> ToolResult {
  const auto merge_th = args.getNum("merge_threshold", 0.6);
  const auto stale_days = static_cast<std::int64_t>(args.getNum("stale_days", 30));
  const auto unused_days = static_cast<std::int64_t>(args.getNum("unused_days", 90));
  const auto& entries = store().index.entries();
  const auto now = now_ms();

  Json items = Json::makeArr();
  std::string text;
  const auto add = [&](const char* kind, const std::string& ids, const std::string& reason,
                       const std::string& action) {
    Json o = Json::makeObj();
    o.set("kind", Json::makeStr(kind));
    o.set("ids", Json::makeStr(ids));
    o.set("reason", Json::makeStr(reason));
    o.set("action", Json::makeStr(action));
    items.arr.push_back(std::move(o));
    text += fmt("\n· [%s] %s\n    → %s", kind, reason.c_str(), action.c_str());
  };

  // ① 重复（同册两两比）
  std::map<std::string, std::vector<const cefu::Entry*>, std::less<>> buckets;
  for (const auto& e : entries) buckets[e.category].push_back(&e);
  for (const auto& [cat, list] : buckets) {
    (void)cat;
    for (std::size_t i = 0; i < list.size(); ++i) {
      for (std::size_t j = i + 1; j < list.size(); ++j) {
        if (const auto s = cefu::Index::similarity(*list[i], *list[j]); s >= merge_th) {
          add("merge", list[i]->id + " / " + list[j]->id,
              fmt("「%s」与「%s」相似度 %.2f", list[i]->title.c_str(), list[j]->title.c_str(), s),
              "比对内容，保留信息全的那条，另一条合并（或写 supersedes 指向新条目）");
        }
      }
    }
  }
  // ② 过期 / ③ 冷条目
  for (const auto& e : entries) {
    if (e.expires_at > 0 && now > e.expires_at) {
      add("stale", e.id,
          fmt("「%s」已过期 %lld 天", e.title.c_str(), static_cast<long long>((now - e.expires_at) / kDayMs)),
          "核实是否仍成立：成立则 cefu_confirm，不成立则删除");
    } else if (e.confidence == "inferred" && now - e.updated_at > stale_days * kDayMs) {
      add("stale", e.id,
          fmt("「%s」推断知识 %lld 天未确认", e.title.c_str(), static_cast<long long>((now - e.updated_at) / kDayMs)),
          "核实后 cefu_confirm，或删除");
    }
    if (e.hits == 0 && now - e.created_at > unused_days * kDayMs) {
      add("unused", e.id,
          fmt("「%s」建档 %lld 天从未被检索命中", e.title.c_str(), static_cast<long long>((now - e.created_at) / kDayMs)),
          "补别名（提升召回最便宜的办法），确实无用则删除");
    }
  }
  // ④ 分类漂移（同一册被写成多个名字，目录会碎）
  std::vector<std::string> cats;
  cats.reserve(buckets.size());
  for (const auto& [c, _] : buckets) cats.push_back(c);
  const auto norm = [](std::string s) {
    s = cefu::to_lower(std::move(s));
    s.erase(std::remove(s.begin(), s.end(), '/'), s.end());
    return s;
  };
  for (std::size_t i = 0; i < cats.size(); ++i) {
    for (std::size_t j = i + 1; j < cats.size(); ++j) {
      const auto a = norm(cats[i]);
      const auto b = norm(cats[j]);
      if (a == b || a.find(b) != std::string::npos || b.find(a) != std::string::npos) {
        add("categoryDrift", "",
            fmt("分类「%s」与「%s」疑似同一册", cats[i].c_str(), cats[j].c_str()),
            "统一成一个分类名");
      }
    }
  }

  if (items.arr.empty()) return ToolResult{"册府很干净：没有需要整理的事项。", items, true};
  return ToolResult{fmt("册府待整理 %zu 项：%s", items.arr.size(), text.c_str()), std::move(items), true};
}

auto tool_confirm(const Json& args) -> ToolResult {
  const auto id = args.getStr("id");
  if (id.empty()) return ToolResult{"cefu_confirm 需要 id", Json(), false};
  const auto* found = store().index.find(id);
  if (found == nullptr) {
    return ToolResult{fmt("册府里没有词条：%s", id.c_str()), Json(), false};
  }
  cefu::Entry copy = *found;
  copy.confidence = "verified";
  copy.needs_review = false;
  copy.updated_at = now_ms();
  copy.expires_at = copy.updated_at + 180 * kDayMs;
  store().index.upsert(copy);
  (void)store().save();
  return ToolResult{fmt("已确认：%s（%s）——有效期延到 180 天后", copy.id.c_str(), copy.title.c_str()), full_json(copy), true};
}

auto tool_stats(const Json&) -> ToolResult {
  const auto& entries = store().index.entries();
  std::size_t verified = 0, stale = 0, inferred = 0;
  std::set<std::string> cats;
  for (const auto& e : entries) {
    cats.insert(e.category);
    if (e.confidence == "verified") ++verified;
    else if (e.confidence == "stale") ++stale;
    else ++inferred;
  }
  Json o = Json::makeObj();
  o.set("entries", Json::makeNum(static_cast<double>(entries.size())));
  o.set("categories", Json::makeNum(static_cast<double>(cats.size())));
  o.set("verified", Json::makeNum(static_cast<double>(verified)));
  o.set("inferred", Json::makeNum(static_cast<double>(inferred)));
  o.set("stale", Json::makeNum(static_cast<double>(stale)));
  o.set("path", Json::makeStr(store().path));
  return ToolResult{fmt("册府：%zu 条 / %zu 册 / 已确认 %zu · 待确认 %zu · 过期 %zu\n库文件：%s",
       entries.size(), cats.size(), verified, inferred, stale, store().path.c_str()), o, true};
}

/* ══════════════ 工具 schema ══════════════ */

constexpr const char* kSearchSchema = R"json({"type":"object","properties":{
"query":{"type":"string","description":"检索词。用你自然想问的那句话（中文短句即可，不必拆词）"},
"limit":{"type":"number","description":"返回条数，默认 5"}},"required":["query"]})json";

constexpr const char* kGetSchema = R"json({"type":"object","properties":{
"id":{"type":"string","description":"词条 id（从 cefu_search / cefu_list 得到）"}},"required":["id"]})json";

constexpr const char* kPutSchema = R"json({"type":"object","properties":{
"title":{"type":"string","description":"词条标题：一行能认出来是什么"},
"category":{"type":"string","description":"分类（成册）：如 存储/TSDB、人/职责、流程/发布"},
"source_ref":{"type":"string","description":"出处：文件:行号 / 会话 id / 人名。必填"},
"id":{"type":"string","description":"可选；省略则按标题生成。要补充既有条目时传它的 id"},
"aliases":{"type":"array","items":{"type":"string"},"description":"别名：别人会怎么问（同义词/口语/缩写/报错原文）"},
"symptom":{"type":"string","description":"现象：怎么认出来是这个问题"},
"causes":{"type":"array","items":{"type":"string"},"description":"可能原因，按常见度排序"},
"steps":{"type":"array","items":{"type":"string"},"description":"处理步骤，有序（第一条最该先做）"},
"commands":{"type":"array","items":{"type":"string"},"description":"可直接复制的命令"},
"facts":{"type":"array","items":{"type":"string"},"description":"硬事实：IP、阈值、chat_id、账号"},
"owner":{"type":"string","description":"谁负责/找谁拍板"},
"tags":{"type":"array","items":{"type":"string"},"description":"自由标签"}},"required":["title","category","source_ref"]})json";

constexpr const char* kListSchema = R"json({"type":"object","properties":{
"category":{"type":"string","description":"只看这一册；省略 = 全部"}}})json";

constexpr const char* kReviewSchema = R"json({"type":"object","properties":{
"merge_threshold":{"type":"number","description":"判为重复的相似度阈值，默认 0.6"},
"stale_days":{"type":"number","description":"inferred 超过多少天未确认算过期，默认 30"},
"unused_days":{"type":"number","description":"建档多少天零命中算冷条目，默认 90"}}})json";

constexpr const char* kConfirmSchema = R"json({"type":"object","properties":{
"id":{"type":"string","description":"词条 id"}},"required":["id"]})json";

constexpr const char* kEmptySchema = R"json({"type":"object","properties":{}})json";

}  // namespace

int main() {
  auto& reg = gebai::registry();
  reg.add({"search",
           "检索册府（知识库）。**回答任何「怎么办/找谁/在哪查」之前先检索**，不要凭印象答。"
           "返回词条摘要 + 分数 + 为何命中（why 里会写清是标题整体匹配还是别名命中）。"
           "检索是字段加权的（标题≫别名≫正文），直接问自然语言即可，不必自己拆词。",
           Json::parse(kSearchSchema), &tool_search});
  reg.add({"get",
           "取册府某条词条的**全文**：现象 / 原因 / 有序步骤 / 命令 / 硬事实 / 出处，并计一次命中。"
           "处理步骤是有序的（第 1 条最该先做）——这是工具书与普通笔记的区别。",
           Json::parse(kGetSchema), &tool_get});
  reg.add({"put",
           "往册府写一条**结构化词条**（知识库是工具书：必须成册、必须有出处）。"
           "已有同 id 则更新。同一册下与既有词条高度相似会被拒绝并返回那条——先 cefu_search 确认。"
           "写入时机：用户纠正你、你实测出一个稳定结论、同一个问题被问第二遍。",
           Json::parse(kPutSchema), &tool_put});
  reg.add({"list", "按分类成册浏览册府（像翻目录）。用来看「这个库里都记了些什么」。",
           Json::parse(kListSchema), &tool_list});
  reg.add({"review",
           "整理巡检：找重复词条 / 过期未确认 / 从未被命中的冷条目 / 分类名漂移，并给出每条该做的动作。"
           "知识库不做整理三个月就成垃圾场——用户问「知识库还好吗」或你写入多条后跑它。",
           Json::parse(kReviewSchema), &tool_review});
  reg.add({"confirm",
           "把词条标为**已确认**（verified）并延长有效期 180 天。用于人/你核实过的事实——"
           "推断来的知识不清洗会自己过期。",
           Json::parse(kConfirmSchema), &tool_confirm});
  reg.add({"stats", "册府概览：词条数 / 分类数 / 置信度分布 / 库文件路径。", Json::parse(kEmptySchema),
           &tool_stats});
  gebai::protocol::run();
  return 0;
}
