/// 册府（CeFu）· 知识库——**用霜天的 XML + JS 通路写界面，C++ 只做装配与暴露能力**。
///
/// ## 名字
///
/// 「册府」古指**藏书之府**（《晋书·张华传》"册府"即国家藏书之所）。取它有两层意思：
///
/// 1. **本义就是"书的库"**——不绕弯，看见名字就知道它是干什么的；
/// 2. 但它不是"仓库"而是"府"：府有编次、有掌守、有新旧更替。这恰好是本设计的三条主张：
///    **成册（category）/ 有出处（source）/ 勤整理（review）**。
///
/// 一个只懂"存"的库三个月就烂了；府要有人编、有人守、有人汰。
///
/// ## 这里要证明什么
///
/// ## 这个示例要证明什么
///
/// 知识库这东西天生该是**一本能翻的书**：按分类成册、按关键词检索、点开看词条全文。
/// 只做成"模型能调的工具"是不够的——人需要能翻、能改、能看出哪条过期了。
///
/// 而霜天的形态正好对上：
///
/// | 层 | 放什么 | 为什么 |
/// |---|---|---|
/// | `cefu_index.*` | 倒排索引 / 打分 / 查重 / 巡检 | 纯计算，与界面无关，可单测 |
/// | `cefu_store.*` | SQLite 存取 | 持久化，可事务、可用 sqlite3 命令行看 |
/// | 本文件 | 装配 + `bridge.expose` | **零界面代码**：不写任何布局/控件 |
/// | `ui/main.xml` | 界面长什么样 | 改保存即生效（HotReload） |
/// | `ui/app.js` | 界面怎么动 | 同上 |
///
/// ## 为什么业务数据在 C++ 而不在 JS
///
/// 真实应用的数据在 C++（SQLite、文件、网络）；JS 只该负责界面逻辑。
/// 把索引搬到 JS 里会变成「同一份知识两处状态」，而跨语言共享可变状态是缺陷温床——
/// 所以桥上的每个方法都**返回一份快照**，JS 拿到的是不可回写的副本。
///
/// ## 桥面（脚本里就是 `app.cefu.*`）
///
/// | 方法 | 用途 |
/// |---|---|
/// | `cefu.stats()` | 概览：词条数、分类数、待整理数、索引内存估算 |
/// | `cefu.categories()` | 分类成册（册名 + 条数） |
/// | `cefu.list(category?)` | 按册列词条（只给摘要，不给全文） |
/// | `cefu.get(id)` | 取一条完整词条 + 命中次数 |
/// | `cefu.search(query, limit?)` | 检索：返回摘要 + 分数 + **命中的路径** |
/// | `cefu.put(entry)` | 新增/更新（强制 title/category/source） |
/// | `cefu.remove(id)` | 删除 |
/// | `cefu.confirm(id)` | 标为 verified（人工裁决） |
/// | `cefu.review()` | 整理巡检（待合并/过期/从未命中/分类漂移） |
/// | `cefu.importText(text, category)` | 从 Markdown/文本按标题层级切词条草稿 |

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "cefu_index.hpp"
#include "st/app/app.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/ext/json.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/hot_reload.hpp"
#include "st/ui/script_bridge.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/xml_ui.hpp"

namespace {

using st::Json;

/// 引擎 + 存储的持有者（应用状态）。
///
/// 为什么把两者绑在一个类里而不是各自暴露：它们必须**始终一致**——
/// 每次写入都要「先落 SQLite 再进索引」，谁忘了其中一步都会让界面看到旧数据。
/// 绑在一起就没有"只写一半"的可能。
class CeFu {
 public:
  CeFu(std::string db_path, std::string seed_path)
      : db_path_(std::move(db_path)), seed_path_(std::move(seed_path)) {}

  /// 打开数据库 + 装载索引。
  ///
  /// 顺序刻意是「先建表、再读词条、最后建索引」：索引是纯派生物，
  /// 它与词条不一致的唯一可能来源就是"读失败还继续建"，所以读失败直接放弃装载。
  auto open() -> bool {
    auto store = cefu::Store::open(db_path_);
    if (!store.has_value()) {
      error_ = std::format("打开知识库失败：{}（{}）", db_path_, store.error().message);
      return false;
    }
    store_ = std::move(*store);

    auto entries = store_->load_all();
    if (!entries.has_value()) {
      error_ = std::format("读取词条失败：{}", entries.error().message);
      return false;
    }
    // 空库首次启动：从种子文件导入（让界面一打开就有内容，而不是空白页）
    if (entries->empty() && !seed_path_.empty()) {
      seed_from(seed_path_);
      auto again = store_->load_all();
      if (again.has_value()) entries = std::move(*again);
    }
    index_.reset(std::move(*entries));
    loaded_ = true;
    return true;
  }

  [[nodiscard]] auto error() const -> const std::string& { return error_; }
  [[nodiscard]] auto entry_count() const -> std::size_t { return index_.size(); }

  auto install(st::ui::ScriptBridge& bridge) -> void {
    (void)bridge.expose("cefu.stats", [this](const std::vector<Json>&) -> st::Result<Json> {
      const auto& all = index_.entries();
      std::size_t verified = 0;
      std::size_t inferred = 0;
      std::size_t stale = 0;
      std::size_t needs_review = 0;
      for (const auto& e : all) {
        if (e.confidence == "verified") ++verified;
        else if (e.confidence == "stale") ++stale;
        else ++inferred;
        if (e.needs_review) ++needs_review;
      }
      const auto items = index_.review(st::time::now_ms(), 30, 90, 0.6);
      Json out = Json::object();
      out["entries"] = static_cast<std::int64_t>(all.size());
      out["categories"] = static_cast<std::int64_t>(category_count());
      out["verified"] = static_cast<std::int64_t>(verified);
      out["inferred"] = static_cast<std::int64_t>(inferred);
      out["stale"] = static_cast<std::int64_t>(stale);
      out["needs_review"] = static_cast<std::int64_t>(needs_review);
      out["review_items"] = static_cast<std::int64_t>(items.size());
      out["db_path"] = db_path_;
      out["loaded"] = loaded_;
      out["error"] = error_;
      return out;
    });

    (void)bridge.expose("cefu.categories", [this](const std::vector<Json>&) -> st::Result<Json> {
      Json out = Json::array();
      for (const auto& [name, count] : categories()) {
        Json item = Json::object();
        item["name"] = name;
        item["count"] = static_cast<std::int64_t>(count);
        out.push_back(std::move(item));
      }
      return out;
    });

    (void)bridge.expose("cefu.list", [this](const std::vector<Json>& args) -> st::Result<Json> {
      const std::string category = args.empty() ? std::string{} : st::json_as_string(args[0]);
      Json out = Json::array();
      for (const auto& e : index_.entries()) {
        if (!category.empty() && e.category != category) continue;
        out.push_back(summary_json(e));
      }
      return out;
    });

    (void)bridge.expose("cefu.get", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "cefu.get 需要词条 id");
      const auto id = st::json_as_string(args[0]);
      const auto* entry = index_.find(id);
      if (entry == nullptr) {
        return st::unexpected(st::ErrorCode::NotFound, "没有这个词条：" + id);
      }
      // 取一次算一次命中（面板据此看"哪些从没被用过"）
      index_.note_hit(id);
      if (auto status = store_->bump_hits(id, st::time::now_ms()); !status.has_value()) {
        // 命中计数写失败不该让读词条失败（读数优先于统计）
        st::print("知识库：命中计数写失败 {}\n", status.error().message);
      }
      return full_json(*entry);
    });

    (void)bridge.expose("cefu.search", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "cefu.search 需要查询词");
      const auto query = st::json_as_string(args[0]);
      const auto limit = args.size() > 1 ? static_cast<std::size_t>(args[1].get<double>()) : std::size_t{8};
      cefu::SearchOptions options;
      options.limit = limit;
      const auto hits = index_.search(query, options);
      Json out = Json::array();
      for (const auto& h : hits) {
        auto item = summary_json(*h.entry);
        item["score"] = h.score;
        // 把"为什么召回它"带出来——这是人判断"该不该补别名"的唯一依据
        Json why = Json::array();
        for (const auto& w : h.why) why.push_back(Json(w));
        item["why"] = why;
        out.push_back(std::move(item));
      }
      return out;
    });

    (void)bridge.expose("cefu.put", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty() || !args[0].is_object()) {
        return st::unexpected(st::ErrorCode::Invalid, "cefu.put 需要一个词条对象");
      }
      const auto& obj = args[0];
      cefu::Entry entry;
      entry.id = obj.value("id", std::string{});
      entry.title = obj.value("title", std::string{});
      entry.category = obj.value("category", std::string{});
      if (entry.title.empty() || entry.category.empty()) {
        return st::unexpected(st::ErrorCode::Invalid,
                              "词条必须有 title 与 category——知识库是工具书，"
                              "没有标题与分类的条目无法被成册浏览，也就无法被找到");
      }
      if (entry.id.empty()) entry.id = make_id(entry.category, entry.title);
      entry.symptom = obj.value("symptom", std::string{});
      entry.owner = obj.value("owner", std::string{});
      entry.scope = obj.value("scope", std::string{R"({"kind":"global"})"});
      const auto list_of = [&obj](const char* key, std::vector<std::string>& out) {
        const auto it = obj.find(key);
        if (it == obj.end() || !it->is_array()) return;
        for (const auto& v : *it) {
          if (v.is_string()) out.push_back(v.get<std::string>());
        }
      };
      list_of("aliases", entry.aliases);
      list_of("causes", entry.causes);
      list_of("steps", entry.steps);
      list_of("commands", entry.commands);
      list_of("facts", entry.facts);
      list_of("links", entry.links);
      list_of("tags", entry.tags);

      entry.source_kind = obj.value("source_kind", std::string{"person"});
      entry.source_ref = obj.value("source_ref", std::string{});
      if (entry.source_ref.empty()) {
        // 没有出处的知识无法事后判断该不该信——这是"知识库变谣言库"的起点
        return st::unexpected(st::ErrorCode::Invalid,
                              "必须给 source_ref（从哪知道的：文件:行号 / 会话 id / 人名）——"
                              "没有出处的知识事后无法判断该不该信");
      }
      entry.confidence = obj.value("confidence", std::string{"inferred"});
      const auto now = st::time::now_ms();
      entry.created_at = now;
      entry.updated_at = now;

      // 查重：同分类内高度相似时**不静默新增**，把既有词条交回去让人选
      for (const auto& other : index_.entries()) {
        if (other.id == entry.id) continue;
        if (other.category != entry.category) continue;
        const double sim = cefu::Index::similarity(other, entry);
        if (sim >= 0.6) {
          return st::unexpected(
              st::ErrorCode::Exists,
              std::format("与既有词条「{}」（{}）相似度 {:.2f}，疑似同一条知识。"
                          "要补充它就沿用该 id 重新 put，要另建请在 id 里体现区别",
                          other.title, other.id, sim));
        }
      }

      auto is_new = store_->upsert(entry);
      if (!is_new.has_value()) {
        return st::unexpected(is_new.error().code, "写库失败：" + is_new.error().message);
      }
      index_.upsert(entry);
      index_.recompute_stats();
      Json out = Json::object();
      out["id"] = entry.id;
      out["created"] = *is_new;
      return out;
    });

    (void)bridge.expose("cefu.remove", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "cefu.remove 需要词条 id");
      const auto id = st::json_as_string(args[0]);
      auto ok = store_->remove(id);
      if (!ok.has_value()) {
        return st::unexpected(ok.error().code, "删库失败：" + ok.error().message);
      }
      const bool removed = index_.remove(id);
      index_.recompute_stats();
      Json out = Json::object();
      out["removed"] = removed;
      return out;
    });

    (void)bridge.expose("cefu.confirm", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "cefu.confirm 需要词条 id");
      const auto id = st::json_as_string(args[0]);
      const auto* found = index_.find(id);
      if (found == nullptr) return st::unexpected(st::ErrorCode::NotFound, "没有这个词条：" + id);
      cefu::Entry entry = *found;
      entry.confidence = "verified";
      entry.needs_review = false;
      entry.updated_at = st::time::now_ms();
      // 确认后延长有效期：verified 的事实不该因为 14 天 TTL 就被当成过期
      entry.expires_at = entry.updated_at + 180LL * 86'400'000;
      if (auto written = store_->upsert(entry); !written.has_value()) {
        return st::unexpected(written.error().code, "写库失败：" + written.error().message);
      }
      index_.upsert(entry);
      Json out = Json::object();
      out["id"] = id;
      out["confidence"] = entry.confidence;
      return out;
    });

    (void)bridge.expose("cefu.review", [this](const std::vector<Json>&) -> st::Result<Json> {
      const auto items = index_.review(st::time::now_ms(), 30, 90, 0.6);
      Json out = Json::array();
      for (const auto& it : items) {
        Json ids = Json::array();
        for (const auto& id : it.ids) ids.push_back(Json(id));
        Json item = Json::object();
        item["kind"] = it.kind;
        item["ids"] = ids;
        item["reason"] = it.reason;
        item["action"] = it.action;
        out.push_back(std::move(item));
      }
      return out;
    });

    (void)bridge.expose("cefu.importText", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "cefu.importText 需要文本");
      const auto text = st::json_as_string(args[0]);
      const auto category = args.size() > 1 ? st::json_as_string(args[1]) : std::string{"导入/未分类"};
      const auto drafts = split_markdown(text, category);
      // 导入只落**草稿**（inferred + needs_review）：存量材料质量未知，必须有人过一遍
      std::size_t created = 0;
      for (const auto& entry : drafts) {
        if (store_->upsert(entry).has_value()) {
          index_.upsert(entry);
          ++created;
        }
      }
      index_.recompute_stats();
      Json out = Json::object();
      out["created"] = static_cast<std::int64_t>(created);
      out["category"] = category;
      return out;
    });
  }

 private:
  [[nodiscard]] auto category_count() const -> std::size_t {
    std::vector<std::string> names;
    for (const auto& e : index_.entries()) {
      if (std::find(names.begin(), names.end(), e.category) == names.end()) names.push_back(e.category);
    }
    return names.size();
  }

  [[nodiscard]] auto categories() const -> std::vector<std::pair<std::string, std::size_t>> {
    std::vector<std::pair<std::string, std::size_t>> out;
    for (const auto& e : index_.entries()) {
      const auto it = std::find_if(out.begin(), out.end(),
                                   [&e](const auto& p) { return p.first == e.category; });
      if (it == out.end()) {
        out.emplace_back(e.category, 1);
      } else {
        ++it->second;
      }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
  }

  /// 摘要（列表/检索结果用）：只给"能认出来"的信息，不给全文。
  ///
  /// 为什么列表不给全文：词条全文（步骤/命令/事实）可能上百字，一屏列出几十条时
  /// 既看不出结构也让滚动失去意义。摘要 + 点开看全文才是工具书的翻法。
  static auto summary_json(const cefu::Entry& e) -> Json {
    Json aliases = Json::array();
    for (const auto& a : e.aliases) aliases.push_back(Json(a));
    Json out = Json::object();
    out["id"] = e.id;
    out["title"] = e.title;
    out["category"] = e.category;
    out["aliases"] = aliases;
    out["owner"] = e.owner;
    out["confidence"] = e.confidence;
    out["hits"] = e.hits;
    out["updated_at"] = e.updated_at;
    out["needs_review"] = e.needs_review;
    out["key_line"] = key_line(e);
    return out;
  }

  /// 一句话结论：检索时最能帮人判断的那句（优先 steps[0]，其次 symptom）。
  static auto key_line(const cefu::Entry& e) -> std::string {
    if (!e.steps.empty() && !e.steps[0].empty()) return e.steps[0];
    if (!e.symptom.empty()) return e.symptom;
    if (!e.causes.empty() && !e.causes[0].empty()) return e.causes[0];
    return {};
  }

  static auto full_json(const cefu::Entry& e) -> Json {
    const auto list = [](const std::vector<std::string>& v) {
      Json out = Json::array();
      for (const auto& s : v) out.push_back(Json(s));
      return out;
    };
    Json out = Json::object();
    out["id"] = e.id;
    out["title"] = e.title;
    out["category"] = e.category;
    out["aliases"] = list(e.aliases);
    out["symptom"] = e.symptom;
    out["causes"] = list(e.causes);
    out["steps"] = list(e.steps);
    out["commands"] = list(e.commands);
    out["facts"] = list(e.facts);
    out["owner"] = e.owner;
    out["links"] = list(e.links);
    out["tags"] = list(e.tags);
    out["scope"] = e.scope;
    out["confidence"] = e.confidence;
    out["source_kind"] = e.source_kind;
    out["source_ref"] = e.source_ref;
    out["created_at"] = e.created_at;
    out["updated_at"] = e.updated_at;
    out["expires_at"] = e.expires_at;
    out["supersedes"] = e.supersedes;
    out["hits"] = e.hits;
    out["last_hit_at"] = e.last_hit_at;
    out["needs_review"] = e.needs_review;
    return out;
  }

  /// 由标题生成稳定 id（中文原样保留 slug 化后的可读串，冲突时加序号）。
  auto make_id(std::string_view category, std::string_view title) const -> std::string {
    std::string slug;
    for (const char c : title) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        slug.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
      } else if (c == ' ' || c == '-' || c == '_') {
        if (!slug.empty() && slug.back() != '-') slug.push_back('-');
      }
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "entry";
    (void)category;
    std::string id = "e_" + slug;
    int suffix = 2;
    while (index_.find(id) != nullptr) {
      id = std::format("e_{}_{}", slug, suffix++);
    }
    return id;
  }

  /// 按 Markdown 标题层级切草稿：`##`/`###` 起一条，正文进 symptom/steps。
  ///
  /// 只做**确定性切分**，不做语义理解：导入是给人一个起点，不是让人相信自动结果。
  static auto split_markdown(std::string_view text, std::string_view category) -> std::vector<cefu::Entry> {
    std::vector<cefu::Entry> out;
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
      if (c == '\n') {
        lines.push_back(current);
        current.clear();
      } else {
        current.push_back(c);
      }
    }
    if (!current.empty()) lines.push_back(current);

    cefu::Entry* active = nullptr;
    std::vector<std::string> body;
    const auto flush = [&] {
      if (active == nullptr) return;
      // 正文里以 "- " 开头的行进 steps（运维知识的主体就是步骤与事实）
      for (const auto& line : body) {
        if (line.size() > 2 && line[0] == '-' && line[1] == ' ') {
          active->steps.push_back(line.substr(2));
        } else if (active->symptom.empty() && !line.empty()) {
          active->symptom = line;
        } else if (!line.empty()) {
          active->facts.push_back(line);
        }
      }
      body.clear();
    };

    const auto now = st::time::now_ms();
    for (const auto& line : lines) {
      const bool is_h2 = line.starts_with("## ");
      const bool is_h3 = line.starts_with("### ");
      if (is_h2 || is_h3) {
        flush();
        out.push_back(cefu::Entry{});
        active = &out.back();
        active->title = line.substr(is_h2 ? 3 : 4);
        active->category = std::string(category);
        active->confidence = "inferred";
        active->needs_review = true;
        active->source_kind = "file";
        active->source_ref = "import";
        active->source_at = now;
        active->created_at = now;
        active->updated_at = now;
        active->scope = R"({"kind":"global"})";
        continue;
      }
      if (active != nullptr) body.push_back(line);
    }
    flush();

    // 去掉空标题，并补 id
    std::vector<cefu::Entry> cleaned;
    for (auto& e : out) {
      if (e.title.empty()) continue;
      std::string slug;
      for (const char c : e.title) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) slug.push_back(c);
        else if (c == ' ' && !slug.empty() && slug.back() != '-') slug.push_back('-');
      }
      if (slug.empty()) slug = "imported";
      e.id = std::format("e_imp_{}_{}", slug, cleaned.size() + 1);
      e.source_ref = std::format("import#{}", cleaned.size() + 1);
      cleaned.push_back(std::move(e));
    }
    return cleaned;
  }

  /// 首次启动的种子导入（让界面打开就有内容）。
  auto seed_from(const std::string& path) -> void {
    const auto text = st::fs::read_text(path);
    if (!text.has_value()) {
      st::print("知识库：种子文件读不到 {}\n", path);
      return;
    }
    const auto drafts = split_markdown(*text, "示例/值班知识");
    for (const auto& e : drafts) {
      (void)store_->upsert(e);
    }
  }

  std::string db_path_;
  std::string seed_path_;
  std::string error_;
  bool loaded_{false};
  cefu::Index index_;
  std::optional<cefu::Store> store_;
};


/// 解析参数（--headless / --ui / --db / --seed / --control-port / --shots）。
struct Options {
  bool headless{false};
  std::string ui_dir{"ui"};
  std::string db_path{"cefu.db"};
  std::string seed_path{"seed.md"};
  int control_port{0};
  /// 控制通道把「端口 + 握手 token」写到这个文件（自动化靠它连上来）。
  ///
  /// **漏了它的后果是「自动化永远连不上」**：token 是每进程随机生成的，只印在日志里，
  /// 调用方拿不到就没法握手（实测踩过：--control-file 没接，e2e 一直报 token 校验失败）。
  std::string control_file{};
  std::string shots{};
  /// 渲染器：auto（按实测帧耗时选）/ gpu / software。空 = 框架默认。
  ///
  /// 保留这个开关是因为**呈现故障要能二分**：应用内截图走软件缓冲、真实窗口走
  /// 交换链，所以"截图有内容但窗口黑"必须能切到 software 复现与定位
  /// （实测踩过：GPU 呈现异常时截图全对、屏幕全黑，没有这个开关只能盲猜）。
  std::string renderer{};
};

auto parse_options(int argc, char** argv) -> Options {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag(argv[i]);
    if (flag == "--headless") {
      o.headless = true;
    } else if (flag == "--ui" && i + 1 < argc) {
      o.ui_dir = argv[++i];
    } else if (flag == "--db" && i + 1 < argc) {
      o.db_path = argv[++i];
    } else if (flag == "--seed" && i + 1 < argc) {
      o.seed_path = argv[++i];
    } else if (flag == "--control-port" && i + 1 < argc) {
      o.control_port = std::atoi(argv[++i]);
    } else if (flag == "--control-file" && i + 1 < argc) {
      o.control_file = argv[++i];
    } else if (flag == "--renderer" && i + 1 < argc) {
      o.renderer = argv[++i];
    } else if (flag == "--shots" && i + 1 < argc) {
      o.shots = argv[++i];
    }
  }
  return o;
}

/// 找资源目录：先看参数给的相对路径，再看可执行文件旁边的 ui 目录
/// （从仓库根跑 `st run cefu` 与从 build/dev/bin 跑都要能用）。
auto resolve_ui_dir(const std::string& requested) -> std::string {
  if (st::fs::is_regular_file(st::fs::join(requested, "main.xml"))) return requested;
  for (const char* candidate : {"examples/cefu/ui", "../examples/cefu/ui",
                                "../../examples/cefu/ui", "../../../examples/cefu/ui"}) {
    if (st::fs::is_regular_file(st::fs::join(candidate, "main.xml"))) return candidate;
  }
  return requested;  // 找不到就原样返回，让后面的报错说清路径
}

auto resolve_seed(const std::string& requested) -> std::string {
  if (st::fs::is_regular_file(requested)) return requested;
  for (const char* candidate : {"examples/cefu/seed.md", "../examples/cefu/seed.md",
                                "../../examples/cefu/seed.md", "../../../examples/cefu/seed.md"}) {
    if (st::fs::is_regular_file(candidate)) return candidate;
  }
  return requested;
}

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);
  const std::string ui_dir = resolve_ui_dir(options.ui_dir);
  const std::string seed_path = resolve_seed(options.seed_path);

  st::app::AppOptions app_options;
  app_options.width = 1180;
  app_options.height = 780;
  app_options.title = "册府 · 知识库";
  app_options.headless = options.headless;
  app_options.backend = options.headless ? "headless" : std::string{};
  app_options.control_port = static_cast<std::uint16_t>(options.control_port);
  app_options.control_file = options.control_file;
  if (!options.renderer.empty()) app_options.renderer = options.renderer;
  app_options.screenshot_dir = options.shots;
  // 脚本宿主只在需要时创建：本示例的界面逻辑全在 JS 里，必须打开
  app_options.enable_script = true;

  st::app::Application app("cefu", "0.1.0", app_options);

  // ① 外壳：标题栏 + 内容槽。
  // ⚠ **先挂壳、后挂 XML 树**：热重载要换根，而换根的落点是 XML 根当前的父元素。
  auto frame = std::make_unique<st::ui::WindowFrame>("册府 · 知识库");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) {
    frame->title_bar()->set_id("titlebar");
    frame->title_bar()->set_icon("list");
  }
  app.set_content(std::move(frame));

  // ② 起服务（脚本宿主在 start() 里创建，桥必须在它之后建）
  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败：%s\n", status.error().message.c_str());
    return 1;
  }
  st::ui::ScriptHost* script = app.script();
  if (script == nullptr || !script->valid()) {
    std::fprintf(stderr, "脚本宿主未就绪（AppOptions::enable_script 打开了吗）\n");
    return 1;
  }

  // ③ 数据层：打开知识库（空库首启动会导入种子）
  CeFu knowledge(options.db_path, seed_path);
  const bool opened = knowledge.open();
  if (!opened) {
    std::fprintf(stderr, "册府打开失败：%s\n", knowledge.error().c_str());
    // 不直接退出：界面仍应能打开并显示错误（比"闪退"有用得多）
  }

  // ④ 暴露能力（脚本里是 app.cefu.*）
  st::ui::ScriptBridge bridge(*script, &app.root());
  knowledge.install(bridge);

  // ⑤ 改文件即生效：XML 建树、JS 重装都由 HotReload 管
  st::ui::HotReload::Options hot_options;
  hot_options.ui_dir = ui_dir;
  hot_options.watch = true;
  st::ui::HotReload hot(app.root(), *script, bridge, hot_options);
  (void)bridge.expose("hot.reload", [&hot](const std::vector<Json>&) -> st::Result<Json> {
    auto status = hot.refresh_all();
    if (!status) return st::unexpected(status.error().code, status.error().message);
    return Json(true);
  });

  if (auto status = hot.refresh_all(); !status) {
    std::fprintf(stderr, "界面装载失败：%s\n", status.error().message.c_str());
    return 1;
  }
  st::print("册府就绪：界面 {} · 桥 {} 个函数 · 词条 {} 条 · 库 {}\n", ui_dir,
            bridge.exposed().size(), knowledge.entry_count(), options.db_path);
  st::print("  {}\n", hot.last_summary());

  // ⑥ 主循环：每帧推进声明式宿主（改文件后热重载即时生效）
  std::uint64_t frames = 0;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    if (hot.poll() > 0) app.request_repaint();
    if (app.root().tick_declarative_hosts() > 0U) app.request_repaint();
    app.tick();
    ++frames;
    app.pace_loop(frame_start_ms);
  }
  st::print("册府退出：{} 帧\n", frames);
  return opened ? 0 : 1;
}

}  // namespace

ST_MAIN(run_app)
