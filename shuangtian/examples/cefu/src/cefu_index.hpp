#pragma once

/// 知识库检索引擎（纯 C++，无外部依赖）。
///
/// ## 为什么自己写引擎，而不是 SQLite FTS5
///
/// 三条实测结论（15 个真实运维问法 / 12 条真实词条，人工标注最该命中的条目）：
///
/// | 方案 | top1 正确率 | 问题 |
/// |---|---|---|
/// | FTS5 `trigram` | 14/15 | 索引 3 字符片段，「CI」「红了」这类 **2 字词完全查不到** |
/// | FTS5 `unicode61` | 14/15 | 整段中文当一个 token，搜不到子串 |
/// | 自研（1+2-gram + 字段加权） | **15/15** | — |
///
/// 另外两件 FTS5 做不到、而运维知识恰恰需要的事：
///
/// 1. **字段级不同权重**：FTS5 多列只能给「整列一个权重」，做不到
///    「`steps` 的第一条比第三条重要」——而「第一步查什么」是最常被问的。
/// 2. **别名前缀匹配**：「CI 红」→「CI 挂了」。倒排索引天然做不了短语前缀。
///
/// ## 结构（全部扁平，无 map-of-map-of-struct）
///
/// ```
/// token 字典 → map<string, []Posting>   每字段一张表（字段即数组下标，无需在 posting 里存 field）
/// Posting    → {docId, tf, minListIdx}  三字段定长；minListIdx 只对有序字段有意义
/// 文档       → vector<Entry>            docId 即下标，索引与元数据一一对应
/// ```
///
/// 内存实测（真实长尾语料）：约 **18–20 字节/posting**，2 万条词条约 50MB。
///
/// ## 持久化
///
/// SQLite 只存**词条本身**（结构化字段 + 元数据），索引在启动时从词条重建。
/// 理由：索引是纯派生物，落盘只增加不一致风险（改了词条忘了改索引）；
/// 而重建 2 万条只要 0.9s、1 万条 0.2s 量级，开一次应用完全可接受。
/// 词条落 SQLite 的意义在**可查询、可事务、可人工审阅**（能用 sqlite3 命令行直接看）。

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/ext/json.hpp"

namespace cefu {

using st::Json;

/* ══════════════ 字段定义 ══════════════ */

/// 一个可检索字段。`weight` 是命中该字段的分值倍数；`pos_decay` 用于有序列表
/// （`steps`）：第一条最该先做，权重随序号衰减。
struct FieldSpec {
  std::string_view key;
  double weight;
  bool pos_decay;
  /// 短文本字段（标题/别名/标签/分类/负责人/事实）。
  ///
  /// **只有短字段索引 1-gram**：长文本里单个汉字的 df 极高、IDF 趋零，遍历它们几乎全是
  /// 无用功（实测 1 万条语料 135 万 posting 有大半是这个来源）；而短字段里的单字往往是
  /// 精确信号（「红」「慢」）。这一条把 posting 降了约 30%，质量仍是 15/15。
  bool short_text;
};

/// 字段表。**顺序即索引数组下标，不可随意调整**。
inline constexpr FieldSpec kFields[] = {
    {"title", 12.0, false, true},    // 标题：整条命中就是最强信号
    {"aliases", 10.0, false, true},  // 别名：别人会怎么问（同义/口语/缩写）
    {"category", 4.0, false, true},  // 分类：成册归属
    {"owner", 6.0, false, true},     // 负责人：人—职责关系
    {"facts", 5.0, false, true},     // 硬事实：IP/阈值/账号（短且精确）
    {"commands", 4.0, false, false}, // 命令：通常较长，只走 2-gram
    {"causes", 3.0, false, false},   // 可能原因（相互无先后语义，不衰减）
    {"symptom", 3.0, false, false},  // 现象
    {"steps", 3.0, true, false},     // 处理步骤：**有序**，第一条权重最高
    {"tags", 3.0, false, true},      // 自由标签
};
inline constexpr std::size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

/* ══════════════ 词条 ══════════════ */

/// 一条知识词条。
///
/// 字段设计的依据不是"好看"，而是**每一次真实的检索失败**：
///
/// | 字段 | 没有它时的失败 |
/// |---|---|
/// | `aliases` | 「CI 红了要重跑吗」搜不到「CI 挂了」（红了≠挂了） |
/// | `steps` 有序 | 「第一步查什么」——步骤与现象混在一句时模型不知顺序 |
/// | `facts` | 「写入放大阈值多少」需要精确取数（IP/阈值/账号） |
/// | `owner` | 「配额问题找谁」是「人—职责」结构化关系 |
/// | `causes` 排序 | 原因平铺时模型不知道先查哪个 |
struct Entry {
  std::string id;
  std::string category;
  std::string title;
  std::vector<std::string> aliases;
  std::string symptom;
  std::vector<std::string> causes;
  std::vector<std::string> steps;
  std::vector<std::string> commands;
  std::vector<std::string> facts;
  std::string owner;
  std::vector<std::string> links;
  std::vector<std::string> tags;

  /// 作用域（JSON 串，如 `{"kind":"global"}`）。
  ///
  /// 为何是字符串而不是枚举：作用域会随部署形态扩展（persona/driver/user），
  /// 枚举一改就要迁移数据；JSON 串让引擎只需一个"判断可见性"的回调。
  std::string scope;

  std::string source_kind; // file / session / person / tool
  std::string source_ref;
  std::int64_t source_at{0};

  std::string confidence{"inferred"}; // verified / inferred / stale
  std::int64_t created_at{0};
  std::int64_t updated_at{0};
  std::int64_t expires_at{0};
  std::string supersedes;
  std::int64_t hits{0};
  std::int64_t last_hit_at{0};
  bool needs_review{false};

  /// 取某字段的字符串列表（索引与查重共用；返回 view 避免拷贝）。
  auto field_values(std::string_view key, std::vector<std::string_view>& out) const -> void;
};

/* ══════════════ 分词 ══════════════ */

/// 中文 1-gram + 2-gram（+可选 3-gram），ASCII 词整体保留。
///
/// 为什么 1-gram 也要：「CI 红」这种残缺表达里「红」是单字，只有 2-gram 查不到；
/// 而噪声交给 IDF 与覆盖度加成压制。
[[nodiscard]] auto tokenize(std::string_view text, std::span<const int> grams) -> std::vector<std::string>;

/* ══════════════ 检索 ══════════════ */

struct Hit {
  const Entry* entry{nullptr};
  double score{0.0};
  /// 命中的路径（诊断用：知道**为什么**召回它，才谈得上"补别名"这类整理动作）
  std::vector<std::string> why;
};

struct SearchOptions {
  std::size_t limit{5};
  std::string category;                                   // 分类过滤（收窄到某一册）
  std::function<bool(const Entry&)> visible;               // 作用域过滤
  double min_score{0.0};
};

/// 索引（纯内存 + 从词条重建）。
class Index {
public:
  Index();

  /// 添加或更新（同 id 覆盖）。
  auto upsert(const Entry& entry) -> void;
  auto remove(std::string_view id) -> bool;

  /// 重建索引（整体替换词条集合）。用于启动装载与批量导入。
  auto reset(std::vector<Entry> entries) -> void;

  /// 重算字段平均长度（批量写入后调一次；逐条写入时不需要）。
  auto recompute_stats() -> void;

  [[nodiscard]] auto search(std::string_view query, const SearchOptions& options) const -> std::vector<Hit>;
  [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }
  [[nodiscard]] auto entries() const -> const std::vector<Entry>& { return entries_; }
  [[nodiscard]] auto find(std::string_view id) const -> const Entry*;

  /// 记录一次命中（整理依据：从未命中 = 疑似无用）。
  auto note_hit(std::string_view id) -> void;

  /// 近似查重（整理的核心动作）：标题+别名的 token 集合 Jaccard 与标题字符 bigram Dice 取大。
  ///
  /// 为什么不用编辑距离：运维词条标题常是「TSDB 写入延迟升高」vs「写入延迟高」，
  /// 编辑距离大但语义重合多；token 集合与字符 bigram 更能反映"说的是同一件事"。
  [[nodiscard]] static auto similarity(const Entry& a, const Entry& b) -> double;

  /// 整理巡检：把「知识库会腐烂」的几种形态逐个找出来。
  struct ReviewItem {
    std::string kind;   // merge / stale / unused / missingAlias / categoryDrift
    std::vector<std::string> ids;
    std::string reason;
    std::string action;
  };
  [[nodiscard]] auto review(std::int64_t now_ms, int stale_days, int unused_days,
                            double merge_threshold) const -> std::vector<ReviewItem>;

private:
  struct Posting {
    std::uint32_t doc_id;
    float tf;
    std::int32_t min_list; // 有序字段的最小序号；无序字段为 -1
  };
  using FieldTable = std::map<std::string, std::vector<Posting>, std::less<>>;

  std::vector<Entry> entries_;
  std::map<std::string, std::uint32_t, std::less<>> id_to_doc_;
  std::vector<FieldTable> fields_;
  /// docId → 每字段 token 数
  std::vector<std::array<std::int32_t, kFieldCount>> field_len_;
  std::array<double, kFieldCount> avg_len_{};
  /// 短语面匹配用（倒排做不到）：标题与别名的紧凑串（小写、去空白）
  std::vector<std::string> title_compact_;
  std::vector<std::vector<std::string>> alias_compact_;

  std::vector<int> short_grams_{1, 2};
  std::vector<int> long_grams_{2};

  auto index_doc(std::uint32_t doc_id, const Entry& entry) -> void;
  auto unindex_doc(std::uint32_t doc_id) -> void;
  auto phrase_bonus(std::uint32_t doc_id, std::string_view query_compact) const -> double;
};

/* ══════════════ 存储（SQLite） ══════════════ */

/// 词条仓库：SQLite 表 + JSON 快照（可 diff、可回滚、可用 sqlite3 直接查）。
class Store {
public:
  Store();
  ~Store();
  Store(Store&&) noexcept;
  auto operator=(Store&&) noexcept -> Store&;
  Store(const Store&) = delete;
  auto operator=(const Store&) -> Store& = delete;

  /// 打开（或创建）知识库文件；`:memory:` 用于测试。
  [[nodiscard]] static auto open(std::string_view path) -> st::Result<Store>;

  /// 读出全部词条（按分类 + 标题排序，便于成册浏览）。
  [[nodiscard]] auto load_all() -> st::Result<std::vector<Entry>>;

  /// 写入/更新一条（同 id 覆盖）。返回是否新增。
  [[nodiscard]] auto upsert(const Entry& entry) -> st::Result<bool>;

  [[nodiscard]] auto remove(std::string_view id) -> st::Result<bool>;

  /// 同步命中计数（整理依据）。
  [[nodiscard]] auto bump_hits(std::string_view id, std::int64_t at) -> st::Status;

  /// 分类统计（成册浏览与面板用）。
  [[nodiscard]] auto categories() -> st::Result<std::vector<std::pair<std::string, std::int64_t>>>;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cefu
