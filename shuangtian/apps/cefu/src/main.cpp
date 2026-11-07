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
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "battery/embed.hpp"
#include "cefu_graph.hpp"
#include "cefu_index.hpp"
#include "cefu_scene.hpp"
#include "../../app_icon.hpp"   // 品牌标记：SVG（标题栏）
#include "st/app/app.hpp"
#include "st/codec/png.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/ui/components/label_overlay.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/three_view.hpp"
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
  /// 全部词条（供图谱构建用；只读快照语义，不暴露索引本身）。
  [[nodiscard]] auto entries() const -> const std::vector<cefu::Entry>& { return index_.entries(); }

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
        st::log::out("知识库：命中计数写失败 {}\n", status.error().message);
      }
      return full_json(*entry);
    });

    (void)bridge.expose("cefu.search", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "cefu.search 需要查询词");
      const auto query = st::json_as_string(args[0]);
      const auto limit = args.size() > 1 ? static_cast<std::size_t>(args[1].get<double>()) : std::size_t{8};
      cefu::SearchOptions options;
      options.limit = limit;
      // 作用域过滤（第三个参数）：把 `{"kind":"persona","id":"xiaoshuang"}`
      // 这类上下文传进来，就**只看得到自己范围内的条目**。
      //
      // 为何必须接上（否则 scope 字段形同虚设）：不传 visible 时，
      // A 群/他人格/别的项目学到的知识会全部参与排序——现象是
      // “它答了一个完全无关部署的旧故障”，而且很自信（因为确实命中了一条）。
      //
      // 语义：词条 scope 为 global → 所有人可见；否则要求与查询上下文
      // 至少有一个相同维度（kind+id 全等）。
      //
      // 不传上下文时**只看 global**（缺省安全侧）：
      // 不过滤会让所有非公共条目都参与排序，而调用方往往不知道要传——
      // 那正是“知识库混入无关部署”的成因。要查自己范围的，显式传 scope。
      // 注：直接调 `cefu.search` 的旧脚本（只传 query/limit）也会变成只看 global，
      // 这是**有意的行为变更**：隔离优先于向后兼容。
      {
        std::string ctx_kind;
        std::string ctx_id;
        if (args.size() > 2 && args[2].is_object()) {
          ctx_kind = st::json_as_string(args[2].value("kind", std::string{}));
          ctx_id = st::json_as_string(args[2].value("id", std::string{}));
        }
        options.visible = [ctx_kind, ctx_id](const cefu::Entry& e) -> bool {
          // global 永远可见（公共知识：环境/制度/找谁）
          if (e.scope.find(R"("kind":"global")") != std::string::npos) return true;
          // 没给上下文：非 global 一律隐藏（缺省安全侧）
          if (ctx_kind.empty()) return false;
          // 同 kind 且同 id → 可见；只要有一个不匹配就隐藏
          const bool kind_match = e.scope.find("\"kind\":\"" + ctx_kind + "\"") != std::string::npos;
          if (!kind_match) return false;
          if (ctx_id.empty()) return true;
          return e.scope.find("\"id\":\"" + ctx_id + "\"") != std::string::npos;
        };
      }
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
          if (v.is_string()) {
            out.push_back(v.get<std::string>());
            continue;
          }
          // 对象形态 `{k,v}` → 统一存成 `"k=v"` 字符串。
          //
          // 为何要容这两种形态（实测踩到）：
          // - 工具面（cefu_put）把 facts 声明为 `[{k,v}]` 对象数组，模型会照此传；
          // - 而索引层 `Entry::facts` 一直是 `vector<string>`（导入与检索都按字符串）。
          // 原来这里只收字符串，**对象形态被静默丢掉**——现象是“写了硬事实，
          // 取回来却没有”，而调用方看不到任何报错。
          // 存成 `k=v` 而不丢 k：检索时键名与值都是信号，图谱要用值判定
          // “足够具体的事实”（端口/IP/路径）以建隐含关联边。
          if (v.is_object()) {
            const auto k = v.value("k", v.value("key", std::string{}));
            const auto val = v.value("v", v.value("value", std::string{}));
            if (!k.empty() || !val.empty()) {
              out.push_back(k + "=" + val);
            }
          }
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

      // `needs_review`：推断类知识**必须进待确认队列**（否则永远没人看它）。
      //
      // 为何在这里定而不是让调用方传：调用方（人格自动学习/模型手写）
      // 很容易忘、也不应该为它负责——它是「这条知识的可信度该不该被人看一眼」
      // 的直接推论，属于存储层的内在不变量：
      //   confidence=verified → 人说的/实测过的，不用再确认
      //   confidence=inferred → 推断，**默认待人确认**（否则它永远停在推断）
      //
      // 实测踩到的后果：自动学习写进去的 inferred 条目 needs_review=0，
      // 于是 cefu_review 永远不报它们，也没有任何地方会提示“这些还没验证”——
      // 知识库看起来在学，实际是一湖未审核的推断。
      entry.needs_review = entry.confidence != "verified";
      // 过期时刻：verified 的事实给 180 天；推断的先给 30 天，
      // 让人在这段时间内确认（未确认则自然过期，不会被无限当真）。
      entry.expires_at = now + (entry.confidence == "verified" ? 180LL : 30LL) * 86'400'000;

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
      const auto items = index_.review(st::time::now_ms(), 30, 90, 0.6);      Json out = Json::array();
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
      st::log::out("知识库：种子文件读不到 {}\n", path);
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

/// 册府图标（窗口/托盘共用）：深色圆角底 + 白色书页轮廓。
///
/// 小尺寸（16px 托盘）走极简式（两页白 + 书脊），大尺寸（窗口）加书页折线。
/// 用程序化绘制而不是嵌入式资源：与相期图标同一套做法，无外部素材依赖。
static auto make_cefu_icon_png(std::uint32_t size) -> std::vector<std::uint8_t> {
  st::codec::PngImage image;
  image.width = size;
  image.height = size;
  image.rgba.assign(static_cast<std::size_t>(size) * size * 4U, 0U);

  const auto to_byte = [](double v) -> std::uint8_t {
    const double clamped = v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v);
    return static_cast<std::uint8_t>(clamped + 0.5);
  };

  // 配色：暖棕底 + 白书页 + 深书脊
  constexpr double kBgR = 0x8F / 255.0, kBgG = 0x7A / 255.0, kBgB = 0x66 / 255.0;
  constexpr double kPageR = 0xFF / 255.0, kPageG = 0xFB / 255.0, kPageB = 0xF2 / 255.0;
  constexpr double kSpineR = 0x5A / 255.0, kSpineG = 0x4B / 255.0, kSpineB = 0x3D / 255.0;

  // 圆角方形 SDF（半径 = 0.22 边长）
  constexpr double kRadius = 0.22;
  const auto sd_round_rect = [](double u, double v, double r) {
    const double qx = std::abs(u - 0.5) - (0.5 - r);
    const double qy = std::abs(v - 0.5) - (0.5 - r);
    const double ax = qx > 0.0 ? qx : 0.0, ay = qy > 0.0 ? qy : 0.0;
    const double outside = std::sqrt(ax * ax + ay * ay);
    const double inside = qx > qy ? qx : qy;
    return outside + (inside < 0.0 ? inside : 0.0) - r;
  };

  // 书页：左侧页 + 右侧页（中央书脊），上下留页边
  constexpr double kTop = 0.26, kBot = 0.74;
  constexpr double kLeft = 0.16, kRight = 0.84;      // 左右页外缘
  constexpr double kSpine = 0.50;                    // 书脊中线
  constexpr double kSpineW = 0.045;                  // 书脊半宽
  constexpr int kSub = 4;

  for (std::uint32_t y = 0; y < size; ++y) {
    for (std::uint32_t x = 0; x < size; ++x) {
      int cover = 0;
      for (int sy = 0; sy < kSub; ++sy) {
        for (int sx = 0; sx < kSub; ++sx) {
          const double uu = (static_cast<double>(x) + (sx + 0.5) / kSub) / size;
          const double vv = (static_cast<double>(y) + (sy + 0.5) / kSub) / size;
          if (sd_round_rect(uu, vv, kRadius) <= 0.0) ++cover;
        }
      }
      if (cover == 0) continue;
      const double ratio = static_cast<double>(cover) / (kSub * kSub);
      const double u = (static_cast<double>(x) + 0.5) / size;
      const double v = (static_cast<double>(y) + 0.5) / size;
      // 底色：中央微亮（书页反光）
      double r = kBgR, g = kBgG, b = kBgB;
      if (u >= kLeft && u <= kRight && v >= kTop && v <= kBot) {
        if (u >= kSpine - kSpineW && u <= kSpine + kSpineW) {
          r = kSpineR; g = kSpineG; b = kSpineB;
        } else {
          const double page_edge = std::min(std::min(u - kLeft, kRight - u),
                                            std::min(v - kTop, kBot - v));
          const double shade = page_edge < 0.12 ? 0.88 + 0.12 * (page_edge / 0.12) : 1.0;
          r = kPageR * shade; g = kPageG * shade; b = kPageB * shade;
        }
      }
      const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4U;
      image.rgba[i + 0U] = to_byte(r * 255.0);
      image.rgba[i + 1U] = to_byte(g * 255.0);
      image.rgba[i + 2U] = to_byte(b * 255.0);
      image.rgba[i + 3U] = to_byte(255.0 * ratio);
    }
  }
  auto encoded = st::codec::png_encode(image);
  if (!encoded) return {};
  return *encoded;
}

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
  for (const char* candidate : {"apps/cefu/ui", "../apps/cefu/ui",
                                "../../apps/cefu/ui", "../../../apps/cefu/ui"}) {
    if (st::fs::is_regular_file(st::fs::join(candidate, "main.xml"))) return candidate;
  }
  return requested;  // 找不到就原样返回，让后面的报错说清路径
}

/// 解析种子文件路径。
///
/// ## 关键：**显式指定就不猜候选**（否则关不掉种子导入）
///
/// 原来只要 `requested` 不存在就去试相对候选列表。于是「明确传一个不存在的路径」
/// （意图是“不要导入种子”）会又被猜回 `apps/cefu/seed.md`——配置明明写了、
/// 行为却没变。
///
/// 调用方（无头控制器）正是靠 `--seed <不存在>` 来关掉示例数据导入：
/// seed.md 里那 7 条示例都是 `needs_review=1` 的假数据，进生产库就是检索污染源。
///
/// 现在：给了非空路径就**只用它**（存在就读、不存在就跳过导入）；
/// 只有调用方没给（空串）时才去猜候选——那才是“找默认位置”的本意。
auto resolve_seed(const std::string& requested) -> std::string {
  if (!requested.empty()) return requested;
  for (const char* candidate : {"apps/cefu/seed.md", "../apps/cefu/seed.md",
                                "../../apps/cefu/seed.md", "../../../apps/cefu/seed.md"}) {
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
  // 托盘常驻：窗口可关但进程不退出，从托盘图标恢复/退出
  if (!options.headless) {
    app_options.tray_enabled = true;
    app_options.window_icon_png = make_cefu_icon_png(128);
    app_options.tray_icon_png = make_cefu_icon_png(16);
    app_options.tray_tooltip = "册府 · 知识库";
    // 关窗→托盘常驻：点 × 只隐藏窗口，退出走托盘右键菜单（「退出」）
    app_options.tray_minimize_on_close = true;
  }

  // 品牌标记：标题栏用矢量（任意 DPI 下重新光栅化才清晰）。

  // 窗口/任务栏图标仍走下面代码现画的那一份（PNG，给系统 API）。

  st::app::icon::register_svg("cefu-logo",

                              b::embed<"apps/cefu/assets/cefu-logo.svg">());


  st::app::Application app("cefu", "0.1.0", app_options);

  // ① 外壳：标题栏 + 内容槽。
  // ⚠ **先挂壳、后挂 XML 树**：热重载要换根，而换根的落点是 XML 根当前的父元素。
  auto frame = std::make_unique<st::ui::WindowFrame>("册府 · 知识库");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) {
    frame->title_bar()->set_id("titlebar");
    st::app::icon::apply_brand(frame->title_bar(), "cefu-logo");
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
  // 内嵌回退：单文件分发时 `ui/` 不随 exe 同行，靠编进 exe 的 `main.xml` / `app.js`。
  // 开发期仍读磁盘（`watch = true` 改文件即重载）；发布产物完全不读磁盘。
  hot_options.embedded = [](const std::string& name) -> std::optional<std::string> {
    if (name == "main.xml") {
      const auto file = b::embed<"apps/cefu/ui/main.xml">();
      return std::string(file.data(), file.length());
    }
    if (name == "app.js") {
      const auto file = b::embed<"apps/cefu/ui/app.js">();
      return std::string(file.data(), file.length());
    }
    return std::nullopt;
  };
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

  // ⑤′ 三维图谱视口：**直接挂到窗口内容区**，不经过 XML。
  //
  // ## 为何不走 XML 占位（踩过的坑，记下来免得重蹈）
  //
  // 最初在 main.xml 里放了一个 `<Column id="graph-pane" absolute="true">` 占位。
  // 它**始终不出现在元素树里**（树里固定 92 个元素，没有它）。已逐个排除：
  // XML 语法（逐标签扫描 depth=0）、注释里的 `--`、`visible="false"`、
  // 热重载未生效（已改为重启验证）、`absolute` 属性未生效（已补进白名单）。
  //
  // 对照实验定位到：**根 Column 是排流的**，主区 `grow="true"` 把 684
  // 高度吃满（54+580+50=684），跟排元素被布局到 **0 尺寸而遭剔除**——
  // 而 `absolute` 本应豁免它却没生效（时序：先算尺寸后读属性）。
  //
  // 与其继续改框架的布局时序，不如走另一边：**视口本来就不该跟 XML 布局
  // 打架**。它是全屏覆盖层，自己控制显示时机。
  st::ui::ThreeView* graph_view = nullptr;
  st::ui::LabelOverlay* graph_labels = nullptr;
  std::string graph_mount_error;
  const auto mount_graph_view = [&]() -> bool {
    if (graph_view != nullptr) return true;
    // ⚠ 不能捕 `frame`：它已经被 `set_content(std::move(frame))` 移走，
    //    指针悬空（实测：直接用它就段错误 0xC0000005）。
    //    从 app 的根按 id 现查才是对的，而且热重载后依然成立。
    // `find` 收的是**裸 id**，不是 CSS 选择器（实测：传 `#content` 找不到）。
    auto* host = app.root().find("content");
    if (host == nullptr) {
      graph_mount_error = "找不到 #content 容器";
      return false;
    }
    // 无头模式不建三维视口。
    //
    // 为何：`ThreeView` 需要真的三维后端（离屏纹理 + 合成）；无头后端
    // 下这条路径未验证且历史上有段错误（0xC0000005）记录。而核验通路
    // 恰恰包含无头截图，所以要在核验时能明确知道“图谱在无头里不渲染”，
    // 而不是让它静默消失或把进程带走。
    if (app.headless()) {
      graph_mount_error = "无头模式不提供三维视口（用有头窗口看图谱）";
      return false;
    }
    auto view = std::make_unique<st::ui::ThreeView>();
    view->set_id("graph-viewport");
    view->set_background(st::math::Color{0x0D, 0x11, 0x17, 0xFF});
    // **吃满可用高度**——这就是“视口为什么总是 320 高”的答案。
    //
    // `ThreeView::measure` 的缺省逻辑是：高度 = `preferred_height_`（320），
    // 只有 `fill_height_` 为真时才用 `available_height`。而我上一轮不知道
    // 有这个开关，在外面硬设 style.width/height 去凑——那既绕又不对
    // （absolute 元素的尺寸不走 flex 分配，设多少是多少，与父区无关）。
    view->set_fill_height(true);
    // 宽度同理：吃满可用宽度（`fill_width` 在 measure 里已经是这个语义）。
    view->style().width = st::ui::kAuto;
    view->style().height = st::ui::kAuto;
    // **必须用软件腿**。
    //
    // 实测：GPU 腿在交互式视口里把离屏纹理合成回画布那一步失效——
    // 清屏色看得见（整块深蓝），而球体与连线完全不见（截图就是一片黑）。
    // 这是 `three_view.hpp` 里 `backend_pref_` 注释已经记下的已知问题。
    // 软件腿逐像素直写画布，行为确定。
    view->set_backend("software");
    // 关掉自转：知识图谱的结构本身就是信息，自己动会干扰“看清哪里连哪里”
    view->set_animation_enabled(false);
    // 绝对定位：**不参与父的 flex 分配**，铺满内容区、叠在三栏之上。
    // 这是唯一能“跟排流同时存在”的形态（排流里没有位置给它）。
    view->style().absolute = true;
    view->style().left = 0.0f;
    view->style().top = 0.0f;
    // 尺寸交给 `set_fill_height` + `fill_width`（见上）。
    //
    // ## 为什么不再自己算尺寸（四次试错的结论）
    //
    // ① left+top+right+bottom → 高 320（right/bottom 需“自身宽”参与，而它还是 auto）
    // ② 取父 bounds → 拿到 0（布局时机还未算完）
    // ③ kAuto → 塌成 0（ThreeView 无自然尺寸）
    // ④ 取 app.viewport() 全尺寸 → 盖满但自身没画出内容（全白）
    //
    // 真正的开关是 `set_fill_height(true)`（组件自己的接口），不在这四个里。
    // 默认隐藏：图谱是“要看才看”的视图，一启动就盖住查阅界面是错的。
    view->set_visible(false);
    st::ui::ThreeView* raw = view.get();
    graph_view = (host->add_child(std::move(view)) != nullptr) ? raw : nullptr;
    if (graph_view == nullptr) return false;

    // 标签层：**叠在视口之上**，用 `TextPort` 画词条文字。
    //
    // 为何必须另开一层（三维里为什么没字）：three.js 跑在 QuickJS 里，
    // 而脚本宿主**没有 Canvas 2D**——「Canvas 画字 → 纹理 → Sprite」走不通。
    // 所以结构交给三维、文字交给这一层（按同一套相机参数投影到屏幕）。
    auto overlay = std::make_unique<st::ui::LabelOverlay>();
    overlay->set_id("graph-labels");
    overlay->style().absolute = true;
    overlay->style().left = 0.0f;
    overlay->style().top = 0.0f;
    overlay->style().width = st::ui::kAuto;
    overlay->style().height = st::ui::kAuto;
    overlay->set_visible(false);
    graph_labels = overlay.get();
    if (host->add_child(std::move(overlay)) == nullptr) graph_labels = nullptr;
    return true;
  };
  (void)mount_graph_view();
  // 装载图谱：生成 three.js 脚本 → 交给视口。
  const auto load_graph = [&]() -> Json {
    if (!mount_graph_view() || graph_view == nullptr) return Json(false);
    const auto graph = cefu::graph::build(knowledge.entries());
    auto laid = graph;
    cefu::graph::layout(laid);
    const auto script_text = cefu::scene::build_script(laid, cefu::scene::Options{});
    const bool ok = graph_view->load_script(script_text, "<cefu-graph>");
    // 装载后把相机拉近。
    //
    // ## 为何不用 `frame_scene(1.02)`（试过，无效）
    //
    // 实测：包围盒 305×366×297（半径约 183），而 frame_scene 算出的
    // 相机距离是 **600**（eye 到 target）——远了 3 倍，画面只占中间一小块。
    //
    // 根因在它的算法里：`render_size` 报的是 3533×1870（**物理像素**），
    // 而取景公式里的视口高度按物理尺寸算，于是 fov 对应的高度缩放也翻了 1.5 倍
    //（DPI scale）——距离被放大。不往里改（那是框架里另一处需要单独验证的修复），
    // 这里直接按实际包围盒重算距离：让包围球恰好填满视口高度的一半 / tan(fov/2)。
    if (ok && graph_view != nullptr) {
      // 包围球半径：从**图本身**算，不靠硬编码（不同规模的库尺寸不同）。
      double max_r = 1.0;
      for (const auto& node : laid.nodes) {
        const auto x = static_cast<double>(node.position.x);
        const auto y = static_cast<double>(node.position.y);
        const auto z = static_cast<double>(node.position.z);
        max_r = std::max(max_r, std::sqrt(x * x + y * y + z * z));
      }
      // 节点自身的半径也要算进去，否则边缘球会被切。
      max_r *= 1.0 + cefu::scene::Options{}.node_radius_ratio * 2.0;
      constexpr double fov_deg = 46.0;
      const double half = fov_deg * 3.14159265358979 / 360.0;
      // 1.12 余量：留一点呼吸空间，但不至于大片空白
      const double distance = max_r / std::tan(half) * 1.12;
      auto camera = graph_view->camera_state();
      camera.distance = static_cast<float>(distance);
      graph_view->set_camera_state(camera);

      // 把同一套相机参数交给标签层，并填上**词条文字**。
      //
      // 文字才是知识图谱的主体：一堆彩色球只能看出“有几个簇”，
      // 看不出“这条是什么”。球只是位置标记。
      if (graph_labels != nullptr) {
        // 标签颜色跟随节点语义（与三维球同色系，便于对应）
        const auto argb_of = [](cefu::graph::NodeKind kind,
                                const std::string& confidence) -> unsigned int {
          switch (kind) {
            case cefu::graph::NodeKind::Category:
              return 0xFFE8B54BU;  // 册：暖金
            case cefu::graph::NodeKind::Person:
              return 0xFF4ED4A0U;  // 人：薄荷绿
            case cefu::graph::NodeKind::Scope:
              return 0xFFB48CF0U;  // 作用域：紫
            case cefu::graph::NodeKind::Entry:
              break;
          }
          // 词条文字用**高亮近白**：它是主角，必须先读得到。
          // 语义（已确认 / 待确认 / 过期）靠色相微差区分，
          // 但三者都保证在深底上可读——实测把过期那条染成暗灰后字就没了。
          if (confidence == "verified") return 0xFFE8F2FFU;  // 略带蓝的白
          if (confidence == "stale") return 0xFFC4CBD4U;     // 过期：**中性灰白**（比上一版亮很多）
          return 0xFFFFE9C4U;                                 // 待确认：暖白
        };
        std::vector<st::ui::LabelAnchor> labels;
        labels.reserve(laid.nodes.size());
        for (const auto& node : laid.nodes) {
          st::ui::LabelAnchor anchor;
          anchor.x = node.position.x;
          anchor.y = node.position.y;
          anchor.z = node.position.z;
          anchor.text = node.label;
          // 权重：枢纽 > 词条；已验证 > 待确认（重要的先被读到）
          float weight = node.kind == cefu::graph::NodeKind::Entry ? 0.35f : 0.9f;
          if (node.confidence == "verified") weight += 0.15f;
          if (node.confidence == "stale") weight -= 0.1f;
          anchor.weight = std::clamp(weight, 0.0f, 1.0f);
          anchor.argb = argb_of(node.kind, node.confidence);
          labels.push_back(std::move(anchor));
        }
        graph_labels->set_labels(std::move(labels));
        // 投影参数与上面的相机保持一致（yaw/pitch/fov 来自场景相机、距离是刚算的）
        st::ui::LabelOverlay::Projection projection;
        projection.yaw = camera.yaw;
        projection.pitch = camera.pitch;
        projection.distance = camera.distance;
        projection.fov_degrees = camera.fov_y_degrees;
        // target：布局的中心（相机/标签必须绕同一个点转，否则文字会偏离节点）
        double cx = 0.0;
        double cy = 0.0;
        double cz = 0.0;
        if (!laid.nodes.empty()) {
          for (const auto& node : laid.nodes) {
            cx += static_cast<double>(node.position.x);
            cy += static_cast<double>(node.position.y);
            cz += static_cast<double>(node.position.z);
          }
          const auto count = static_cast<double>(laid.nodes.size());
          cx /= count;
          cy /= count;
          cz /= count;
        }
        projection.target_x = static_cast<float>(cx);
        projection.target_y = static_cast<float>(cy);
        projection.target_z = static_cast<float>(cz);
        graph_labels->set_projection(projection);
      }
    }
    Json out = Json::object();
    out["ok"] = ok;
    out["nodes"] = static_cast<std::int64_t>(laid.nodes.size());
    out["edges"] = static_cast<std::int64_t>(laid.edges.size());
    out["entries"] = static_cast<std::int64_t>(laid.entry_count);
    out["categories"] = static_cast<std::int64_t>(laid.category_count);
    out["persons"] = static_cast<std::int64_t>(laid.person_count);
    out["isolated"] = static_cast<std::int64_t>(laid.isolated_entries);
    out["largest_cluster"] = static_cast<std::int64_t>(laid.largest_cluster);
    out["error"] = graph_view->last_error();
    return out;
  };

  // 图谱模式开关：脚本调这个切换，而不是自己在 JS 里控制可见性。
  //
  // 为何由 C++ 持有开关：视口是 C++ 造的元素，它的可见性属于“视图状态”；
  // 让 JS 去 `set({visible})` 需要它在 XML 树里可寻址——而它恰恰不在。
  (void)bridge.expose("cefu.graph_toggle", [&](const std::vector<Json>& args) -> st::Result<Json> {
    if (!mount_graph_view() || graph_view == nullptr) {
      return st::unexpected(st::ErrorCode::Unsupported,
                            graph_mount_error.empty() ? "三维视口未就绪" : graph_mount_error);
    }
    const bool want = args.empty() ? !graph_view->visible() : st::json_as_bool(args[0], true);
    if (want) {
      const auto info = load_graph();
      graph_view->set_visible(true);
      if (graph_labels != nullptr) graph_labels->set_visible(true);
      app.request_repaint();
      return info;
    }
    graph_view->set_visible(false);
    if (graph_labels != nullptr) graph_labels->set_visible(false);
    app.request_repaint();
    Json out = Json::object();
    out["ok"] = true;
    out["visible"] = false;
    return out;
  });
  (void)bridge.expose("cefu.graph_reset", [&](const std::vector<Json>&) -> st::Result<Json> {
    if (graph_view == nullptr) return Json(false);
    graph_view->reset_view();
    app.request_repaint();
    return Json(true);
  });
  // 诊断：视口的相机与取景信息。
  //
  // 为何走桥而不是 `get`：视口是 C++ 挂的**绝对定位元素**，不在声明式树里，
  // 选择器找不到它（实测 `get(#graph-viewport)` 一律 not_found）。
  (void)bridge.expose("cefu.graph_debug", [&](const std::vector<Json>&) -> st::Result<Json> {
    Json out = Json::object();
    if (graph_view == nullptr) {
      out["mounted"] = false;
      return out;
    }
    out["mounted"] = true;
    for (const char* key : {"camera_state", "frame_bounds", "render_size", "object_count", "backend"}) {
      const auto value = graph_view->get_property(key);
      out[key] = value.has_value() ? *value : std::string{};
    }
    return out;
  });
  // 诊断用：把生成的脚本落盘，供外部检查坐标/相机是否合理。
  //
  // 为何需要它：三维画面“一片背景色”时，原因可能是相机太远/太近、
  // 几何在视野外、或根本没生成——而截图只能告诉你“没有”。把脚本拿出来
  // 数一数坐标范围，一秒就能分辨是哪一种。
  (void)bridge.expose("cefu.graph_dump", [&](const std::vector<Json>& args) -> st::Result<Json> {
    const std::string path = args.empty() ? std::string{"graph.js"} : st::json_as_string(args[0]);
    const auto graph = cefu::graph::build(knowledge.entries());
    auto laid = graph;
    cefu::graph::layout(laid);
    const auto text = cefu::scene::build_script(laid, cefu::scene::Options{});
    auto written = st::fs::write_text(path, text);
    if (!written) return st::unexpected(written.error().code, written.error().message);
    Json out = Json::object();
    out["path"] = path;
    out["bytes"] = static_cast<std::int64_t>(text.size());
    out["nodes"] = static_cast<std::int64_t>(laid.nodes.size());
    return out;
  });
  st::log::out("册府就绪：界面 {} · 桥 {} 个函数 · 词条 {} 条 · 库 {}\n", ui_dir,
            bridge.exposed().size(), knowledge.entry_count(), options.db_path);
  st::log::out("  {}\n", hot.last_summary());

  // ⑥ 主循环：每帧推进声明式宿主（改文件后热重载即时生效）
  std::uint64_t frames = 0;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    // 托盘左键点击：把窗口叫回前台（关窗只隐藏、进程常驻，恢复靠这里）
    if (app.take_tray_clicks() > 0) (void)app.window_restore();
    if (hot.poll() > 0) app.request_repaint();
    if (app.root().tick_declarative_hosts() > 0U) app.request_repaint();
    app.tick();
    ++frames;
    app.pace_loop(frame_start_ms);
  }
  st::log::out("册府退出：{} 帧\n", frames);
  return opened ? 0 : 1;
}

}  // namespace

ST_MAIN(run_app)
