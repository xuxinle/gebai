#include "cefu_index.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "st/core/time.hpp"
#include "st/ext/database.hpp"

namespace cefu {

namespace {

/// 中文停用字：单字索引会带进大量噪声，靠停用字表压一部分。
/// 只在 1-gram 时过滤——2-gram 里出现停用字是正常的（「的任」「务的」）。
constexpr std::u32string_view kStopZH = U"的了是在和与或有吗呢吧啊我你他它这那要会就都很个上中";

auto is_stop(char32_t c) -> bool { return kStopZH.find(c) != std::u32string_view::npos; }

/// UTF-8 → code points（够用的最小实现；非法字节直接跳过）。
auto decode_utf8(std::string_view text) -> std::u32string {
  std::u32string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const auto byte = static_cast<unsigned char>(text[i]);
    char32_t cp = 0;
    std::size_t extra = 0;
    if (byte < 0x80) {
      cp = byte;
    } else if ((byte & 0xE0) == 0xC0) {
      cp = byte & 0x1F;
      extra = 1;
    } else if ((byte & 0xF0) == 0xE0) {
      cp = byte & 0x0F;
      extra = 2;
    } else if ((byte & 0xF8) == 0xF0) {
      cp = byte & 0x07;
      extra = 3;
    } else {
      ++i;  // 非法起始字节：跳过（不抛——分词不该因脏数据失败）
      continue;
    }
    if (i + extra >= text.size()) break;
    bool ok = true;
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto cont = static_cast<unsigned char>(text[i + k]);
      if ((cont & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cont & 0x3F);
    }
    if (!ok) {
      ++i;
      continue;
    }
    out.push_back(cp);
    i += extra + 1;
  }
  return out;
}

auto encode_utf8(char32_t cp) -> std::string {
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

auto is_han(char32_t cp) -> bool { return cp >= 0x4E00 && cp <= 0x9FFF; }
auto is_ascii_word(char32_t cp) -> bool {
  if (cp >= 128) return false;
  const auto c = static_cast<char>(cp);
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '.' || c == ':' || c == '#' || c == '@' || c == '-' || c == '/';
}
auto to_lower(std::string s) -> std::string {
  for (auto& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}
/// 紧凑串：小写并去掉空白（短语面匹配用）。
auto compact(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

auto now_ms() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr std::int64_t kDayMs = 86'400'000;

}  // namespace

/* ══════════════ 分词 ══════════════ */

auto tokenize(std::string_view text, std::span<const int> grams) -> std::vector<std::string> {
  const auto cps = decode_utf8(text);
  std::vector<std::string> out;
  out.reserve(cps.size() * 2);

  std::vector<char32_t> ascii;
  const auto flush_ascii = [&] {
    if (ascii.size() >= 2) {
      std::string word;
      word.reserve(ascii.size());
      for (const auto cp : ascii) word.push_back(static_cast<char>(cp));
      word = to_lower(std::move(word));
      // 带分隔符的再拆子片段：oc_7f3a → oc / 7f3a（两字查询也能命中）
      const bool has_sep = word.find_first_of("_.:#@-/") != std::string::npos;
      if (has_sep) {
        std::string part;
        for (const char c : word) {
          if (c == '_' || c == '.' || c == ':' || c == '#' || c == '@' || c == '-' || c == '/') {
            if (part.size() >= 2) out.push_back(part);
            part.clear();
          } else {
            part.push_back(c);
          }
        }
        if (part.size() >= 2) out.push_back(part);
      }
      out.push_back(std::move(word));
    }
    ascii.clear();
  };

  std::size_t i = 0;
  while (i < cps.size()) {
    const auto cp = cps[i];
    if (is_ascii_word(cp)) {
      ascii.push_back(cp);
      ++i;
      continue;
    }
    flush_ascii();
    if (!is_han(cp)) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < cps.size() && is_han(cps[j])) ++j;
    const auto seg_len = j - i;
    for (const int n : grams) {
      if (n <= 0) continue;
      if (n == 1) {
        for (std::size_t k = i; k < j; ++k) {
          if (is_stop(cps[k])) continue;
          out.push_back(encode_utf8(cps[k]));
        }
        continue;
      }
      const auto un = static_cast<std::size_t>(n);
      if (seg_len < un) continue;
      for (std::size_t k = 0; k + un <= seg_len; ++k) {
        std::string gram;
        gram.reserve(un * 3);
        for (std::size_t m = 0; m < un; ++m) gram += encode_utf8(cps[i + k + m]);
        out.push_back(std::move(gram));
      }
    }
    i = j;
  }
  flush_ascii();
  return out;
}

/* ══════════════ Entry ══════════════ */

auto Entry::field_values(std::string_view key, std::vector<std::string_view>& out) const -> void {
  out.clear();
  if (key == "title") {
    out.push_back(title);
  } else if (key == "aliases") {
    for (const auto& v : aliases) out.push_back(v);
  } else if (key == "category") {
    out.push_back(category);
  } else if (key == "owner") {
    out.push_back(owner);
  } else if (key == "facts") {
    for (const auto& v : facts) out.push_back(v);
  } else if (key == "commands") {
    for (const auto& v : commands) out.push_back(v);
  } else if (key == "causes") {
    for (const auto& v : causes) out.push_back(v);
  } else if (key == "symptom") {
    out.push_back(symptom);
  } else if (key == "steps") {
    for (const auto& v : steps) out.push_back(v);
  } else if (key == "tags") {
    for (const auto& v : tags) out.push_back(v);
  }
}

/* ══════════════ Index ══════════════ */

Index::Index() : fields_(kFieldCount) {}

auto Index::find(std::string_view id) const -> const Entry* {
  const auto it = id_to_doc_.find(id);
  if (it == id_to_doc_.end()) return nullptr;
  return &entries_[it->second];
}

auto Index::index_doc(std::uint32_t doc_id, const Entry& entry) -> void {
  auto& lens = field_len_[doc_id];
  std::vector<std::string_view> values;
  for (std::size_t fi = 0; fi < kFieldCount; ++fi) {
    const auto& spec = kFields[fi];
    entry.field_values(spec.key, values);
    if (values.empty()) {
      lens[fi] = 0;
      continue;
    }
    // 同字段内同 token 合并 tf（避免查询期重复累加）
    std::unordered_map<std::string, std::pair<float, std::int32_t>> per_token;
    per_token.reserve(values.size() * 4);
    for (std::size_t list_idx = 0; list_idx < values.size(); ++list_idx) {
      const auto& v = values[list_idx];
      if (v.empty()) continue;
      const auto toks = tokenize(v, spec.short_text ? short_grams_ : long_grams_);
      for (const auto& t : toks) {
        auto [it, inserted] = per_token.try_emplace(t, std::pair{0.0f, -1});
        it->second.first += 1.0f;
        if (spec.pos_decay) {
          const auto li = static_cast<std::int32_t>(list_idx);
          if (it->second.second < 0 || li < it->second.second) it->second.second = li;
        }
      }
    }
    lens[fi] = static_cast<std::int32_t>(per_token.size());
    for (const auto& [token, info] : per_token) {
      fields_[fi][token].push_back(Posting{doc_id, info.first, info.second});
    }
  }
}

auto Index::unindex_doc(std::uint32_t doc_id) -> void {
  for (auto& table : fields_) {
    for (auto it = table.begin(); it != table.end();) {
      auto& plist = it->second;
      plist.erase(std::remove_if(plist.begin(), plist.end(),
                                 [doc_id](const Posting& p) { return p.doc_id == doc_id; }),
                  plist.end());
      if (plist.empty()) {
        it = table.erase(it);
      } else {
        ++it;
      }
    }
  }
}

auto Index::upsert(const Entry& entry) -> void {
  const auto it = id_to_doc_.find(entry.id);
  if (it != id_to_doc_.end()) {
    const auto doc_id = it->second;
    unindex_doc(doc_id);
    entries_[doc_id] = entry;
    title_compact_[doc_id] = compact(entry.title);
    auto& al = alias_compact_[doc_id];
    al.clear();
    for (const auto& a : entry.aliases) al.push_back(compact(a));
    index_doc(doc_id, entry);
    return;
  }
  const auto doc_id = static_cast<std::uint32_t>(entries_.size());
  entries_.push_back(entry);
  id_to_doc_.emplace(entry.id, doc_id);
  field_len_.emplace_back();
  title_compact_.push_back(compact(entry.title));
  std::vector<std::string> al;
  al.reserve(entry.aliases.size());
  for (const auto& a : entry.aliases) al.push_back(compact(a));
  alias_compact_.push_back(std::move(al));
  index_doc(doc_id, entry);
}

auto Index::remove(std::string_view id) -> bool {
  const auto it = id_to_doc_.find(id);
  if (it == id_to_doc_.end()) return false;
  const auto doc_id = it->second;
  unindex_doc(doc_id);
  // 用末位填充（保持 docId 连续），并修正被移动者的引用
  const auto last = static_cast<std::uint32_t>(entries_.size() - 1);
  if (doc_id != last) {
    entries_[doc_id] = std::move(entries_[last]);
    field_len_[doc_id] = field_len_[last];
    title_compact_[doc_id] = std::move(title_compact_[last]);
    alias_compact_[doc_id] = std::move(alias_compact_[last]);
    id_to_doc_[entries_[doc_id].id] = doc_id;
    // 该文档的 posting 里 doc_id 需从 last 改成 doc_id
    for (auto& table : fields_) {
      for (auto& [token, plist] : table) {
        for (auto& p : plist) {
          if (p.doc_id == last) p.doc_id = doc_id;
        }
      }
    }
  }
  entries_.pop_back();
  field_len_.pop_back();
  title_compact_.pop_back();
  alias_compact_.pop_back();
  id_to_doc_.erase(it);
  return true;
}

auto Index::reset(std::vector<Entry> entries) -> void {
  entries_.clear();
  id_to_doc_.clear();
  field_len_.clear();
  title_compact_.clear();
  alias_compact_.clear();
  for (auto& table : fields_) table.clear();
  entries_.reserve(entries.size());
  for (const auto& e : entries) upsert(e);
  recompute_stats();
}

auto Index::recompute_stats() -> void {
  const auto n = static_cast<double>(entries_.size());
  if (n <= 0) {
    avg_len_.fill(1.0);
    return;
  }
  std::array<double, kFieldCount> sums{};
  for (const auto& lens : field_len_) {
    for (std::size_t fi = 0; fi < kFieldCount; ++fi) sums[fi] += lens[fi];
  }
  for (std::size_t fi = 0; fi < kFieldCount; ++fi) {
    avg_len_[fi] = sums[fi] > 0 ? sums[fi] / n : 1.0;
  }
}

auto Index::phrase_bonus(std::uint32_t doc_id, std::string_view query_compact) const -> double {
  double bonus = 0.0;
  const auto& title = title_compact_[doc_id];
  if (title.size() >= 2 &&
      (query_compact.find(title) != std::string_view::npos || title.find(query_compact) != std::string::npos)) {
    bonus += 8.0;
  }
  for (const auto& alias : alias_compact_[doc_id]) {
    if (alias.size() < 2) continue;
    if (query_compact.find(alias) != std::string_view::npos) {
      bonus += 6.0;
    } else if (query_compact.size() >= 2 &&
               (alias.starts_with(query_compact) || query_compact.starts_with(alias))) {
      bonus += 3.0;
    }
  }
  return bonus * 3.0;
}

auto Index::search(std::string_view query, const SearchOptions& options) const -> std::vector<Hit> {
  const auto n_docs = entries_.size();
  if (n_docs == 0) return {};

  // 查询 token：短字段用 1+2-gram，长字段只有 2-gram
  auto q_short = tokenize(query, short_grams_);
  auto q_long = tokenize(query, long_grams_);
  // 去重
  std::unordered_set<std::string> seen;
  std::vector<std::string> uniq;
  uniq.reserve(q_short.size());
  for (auto& t : q_short) {
    if (seen.insert(t).second) uniq.push_back(std::move(t));
  }
  q_short = std::move(uniq);
  std::unordered_set<std::string> long_set(q_long.begin(), q_long.end());
  if (q_short.empty()) return {};

  // 累加器：按 docId 索引的扁平数组（避免 map 的每文档分配）
  const auto n = static_cast<std::size_t>(n_docs);
  std::vector<double> scores(n, 0.0);
  // 覆盖度计数的去重标记：记「该文档最近一次为哪个 query token 计过数」。
  // 用 token 下标而不是集合——每个 token 只需判一次，省掉每文档一个 set。
  std::vector<std::uint32_t> token_mark(n, std::numeric_limits<std::uint32_t>::max());
  std::vector<std::int32_t> hit_count(n, 0);
  std::vector<std::uint32_t> touched;
  touched.reserve(256);

  const auto nd = static_cast<double>(n_docs);
  constexpr double k1 = 1.2;
  constexpr double k_bm25_len = 0.75;
  std::uint32_t token_idx = 0;

  for (const auto& token : q_short) {
    const auto this_token = token_idx++;
    double df_total = 0.0;
    bool present = false;
    for (std::size_t fi = 0; fi < kFieldCount; ++fi) {
      const auto it = fields_[fi].find(token);
      if (it != fields_[fi].end() && !it->second.empty()) {
        df_total += static_cast<double>(it->second.size());
        present = true;
      }
    }
    if (!present) continue;
    double idf = std::log(1.0 + (nd - df_total + 0.5) / (df_total + 0.5));
    if (idf <= 0.0) idf = 1e-6;

    for (std::size_t fi = 0; fi < kFieldCount; ++fi) {
      const auto& spec = kFields[fi];
      if (!spec.short_text && long_set.find(token) == long_set.end()) continue;
      const auto it = fields_[fi].find(token);
      if (it == fields_[fi].end()) continue;
      const double avg = avg_len_[fi] > 0 ? avg_len_[fi] : 1.0;
      for (const auto& p : it->second) {
        const auto& entry = entries_[p.doc_id];
        if (!options.category.empty() && entry.category != options.category) continue;
        if (options.visible && !options.visible(entry)) continue;
        double flen = field_len_[p.doc_id][fi];
        if (flen <= 0) flen = 1;
        const double tf = p.tf;
        double tf_score = tf * (k1 + 1.0) / (tf + k1 * (1.0 - k_bm25_len + k_bm25_len * flen / avg));
        if (spec.pos_decay && p.min_list > 0) {
          tf_score /= 1.0 + 0.35 * static_cast<double>(p.min_list);
        }
        const double s = spec.weight * idf * tf_score;
        if (scores[p.doc_id] == 0.0) touched.push_back(p.doc_id);
        scores[p.doc_id] += s;
        // 同一 token 可能命中多个字段：只对「首次为本 token 计过数」的文档递增
        if (token_mark[p.doc_id] != this_token) {
          token_mark[p.doc_id] = this_token;
          hit_count[p.doc_id] += 1;
        }
      }
    }
  }

  // 覆盖度加成（压制单字巧合）
  const auto q_len = static_cast<double>(q_short.size());
  for (const auto doc_id : touched) {
    const double cov = static_cast<double>(hit_count[doc_id]) / q_len;
    scores[doc_id] *= 1.0 + 0.8 * cov;
  }

  // 短语面匹配（倒排做不到）：只对候选算 + 少量全扫（文档数少时）
  const auto qc = compact(query);
  for (const auto doc_id : touched) {
    scores[doc_id] += phrase_bonus(doc_id, qc);
  }
  constexpr std::size_t kPhraseScanLimit = 20000;
  if (entries_.size() <= kPhraseScanLimit) {
    for (std::uint32_t doc_id = 0; doc_id < entries_.size(); ++doc_id) {
      if (scores[doc_id] != 0.0) continue;
      const auto& entry = entries_[doc_id];
      if (!options.category.empty() && entry.category != options.category) continue;
      if (options.visible && !options.visible(entry)) continue;
      const double bonus = phrase_bonus(doc_id, qc);
      if (bonus > 0.0) {
        scores[doc_id] = bonus;
        touched.push_back(doc_id);
      }
    }
  }

  std::vector<Hit> out;
  out.reserve(touched.size());
  for (const auto doc_id : touched) {
    if (scores[doc_id] < options.min_score) continue;
    Hit h;
    h.entry = &entries_[doc_id];
    h.score = scores[doc_id];
    out.push_back(std::move(h));
  }
  const auto limit = options.limit > 0 ? options.limit : 5;
  if (out.size() > limit) {
    std::partial_sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(limit), out.end(),
                      [](const Hit& a, const Hit& b) { return a.score > b.score; });
    out.resize(limit);
  } else {
    std::sort(out.begin(), out.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
  }
  return out;
}

auto Index::note_hit(std::string_view id) -> void {
  const auto it = id_to_doc_.find(id);
  if (it == id_to_doc_.end()) return;
  auto& e = entries_[it->second];
  e.hits += 1;
  e.last_hit_at = now_ms();
}

/* ══════════════ 相似度 ══════════════ */

auto Index::similarity(const Entry& a, const Entry& b) -> double {
  const auto token_set = [](const Entry& e) {
    std::unordered_set<std::string> s;
    const std::vector<int> grams{2};
    for (const auto& t : tokenize(e.title, grams)) s.insert(t);
    for (const auto& alias : e.aliases) {
      for (const auto& t : tokenize(alias, grams)) s.insert(t);
    }
    return s;
  };
  const auto sa = token_set(a);
  const auto sb = token_set(b);
  if (sa.empty() || sb.empty()) return 0.0;
  std::size_t inter = 0;
  for (const auto& t : sa) {
    if (sb.count(t) != 0) ++inter;
  }
  const double jac = static_cast<double>(inter) / static_cast<double>(sa.size() + sb.size() - inter);

  // 标题字符 bigram 的 Dice 系数
  const auto bigrams = [](std::string_view text) {
    const auto cps = decode_utf8(text);
    std::unordered_set<std::string> out;
    for (std::size_t i = 0; i + 2 <= cps.size(); ++i) {
      out.insert(encode_utf8(cps[i]) + encode_utf8(cps[i + 1]));
    }
    return out;
  };
  const auto ga = bigrams(compact(a.title));
  const auto gb = bigrams(compact(b.title));
  double dice = 0.0;
  if (!ga.empty() && !gb.empty()) {
    std::size_t i2 = 0;
    for (const auto& g : ga) {
      if (gb.count(g) != 0) ++i2;
    }
    dice = 2.0 * static_cast<double>(i2) / static_cast<double>(ga.size() + gb.size());
  }
  return std::max(jac, dice);
}

/* ══════════════ 整理巡检 ══════════════ */

auto Index::review(std::int64_t now, int stale_days, int unused_days, double merge_threshold) const
    -> std::vector<ReviewItem> {
  std::vector<ReviewItem> items;

  // ① 合并重复：同分类内两两比对（分类分桶把 O(n²) 压到可用）
  std::map<std::string, std::vector<const Entry*>, std::less<>> buckets;
  for (const auto& e : entries_) buckets[e.category].push_back(&e);
  for (const auto& [cat, list] : buckets) {
    for (std::size_t i = 0; i < list.size(); ++i) {
      for (std::size_t j = i + 1; j < list.size(); ++j) {
        const double s = similarity(*list[i], *list[j]);
        if (s >= merge_threshold) {
          items.push_back(ReviewItem{
              "merge",
              {list[i]->id, list[j]->id},
              std::format("标题/别名相似度 {:.2f}，疑似同一条知识写了两遍", s),
              "比对两条内容，保留信息全的那条，另一条 supersedes 掉或合并字段",
          });
        }
      }
    }
  }

  // ② 过期未降级
  for (const auto& e : entries_) {
    if (e.expires_at > 0 && now > e.expires_at && e.confidence != "stale") {
      items.push_back(ReviewItem{"stale",
                                 {e.id},
                                 std::format("已过期 {} 天，但仍标为 {}", (now - e.expires_at) / kDayMs,
                                             e.confidence),
                                 "确认是否仍然成立：成立则延期并标 verified，不成立则降 stale 或删除"});
    }
    if (e.confidence == "inferred" && now - e.updated_at > static_cast<std::int64_t>(stale_days) * kDayMs) {
      items.push_back(ReviewItem{"stale",
                                 {e.id},
                                 std::format("推断知识 {} 天未确认", (now - e.updated_at) / kDayMs),
                                 "核实后标 verified（延长有效期），或删除"});
    }
  }

  // ③ 从未命中
  for (const auto& e : entries_) {
    if (e.hits == 0 && now - e.created_at > static_cast<std::int64_t>(unused_days) * kDayMs) {
      items.push_back(ReviewItem{"unused",
                                 {e.id},
                                 std::format("建档 {} 天从未被检索命中", (now - e.created_at) / kDayMs),
                                 "检查是否描述太偏（补别名），确实无用则删除"});
    }
  }

  // ④ 分类名漂移（「存储」vs「存储/TSDB」）
  std::vector<std::string> cats;
  cats.reserve(buckets.size());
  for (const auto& [cat, _] : buckets) cats.push_back(cat);
  for (std::size_t i = 0; i < cats.size(); ++i) {
    for (std::size_t j = i + 1; j < cats.size(); ++j) {
      const auto norm = [](std::string s) {
        s = to_lower(std::move(s));
        s.erase(std::remove(s.begin(), s.end(), '/'), s.end());
        return s;
      };
      const auto a = norm(cats[i]);
      const auto b2 = norm(cats[j]);
      if (a == b2 || a.find(b2) != std::string::npos || b2.find(a) != std::string::npos) {
        items.push_back(ReviewItem{"categoryDrift",
                                   {},
                                   std::format("分类「{}」与「{}」疑似同一册", cats[i], cats[j]),
                                   "统一成一个分类名（否则目录会碎成多册）"});
      }
    }
  }

  // ⑤ 需要补别名
  for (const auto& e : entries_) {
    if (e.needs_review) {
      items.push_back(ReviewItem{"missingAlias",
                                 {e.id},
                                 "被检索到但排名不佳或曾检索失败",
                                 "把用户实际用过的问法补进 aliases（这是提升召回最便宜的办法）"});
    }
  }

  return items;
}

}  // namespace cefu
