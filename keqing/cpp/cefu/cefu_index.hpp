/// 册府（cefu）检索内核——边车用。
///
/// ## 为什么这份引擎与 `examples/cefu` 不共享代码
///
/// 两处用途不同，依赖面也不同：
///
/// | | `examples/cefu`（应用） | `keqing/cpp/cefu`（边车） |
/// |---|---|---|
/// | 使用者 | 人（翻书、改、整理） | 模型（检索、写入、巡检） |
/// | 依赖 | 霜天全套（窗口/脚本/热重载）+ SQLite | **零依赖**（纯标准库，C++17） |
/// | 存储 | SQLite（可事务、可 sqlite3 直查） | JSON 落盘（千级词条，全量读写 <1ms） |
///
/// 强行共享会把界面与 SQLite 依赖拖进边车（边车要能在任意机器上编译）。
/// 所以**规则与阈值一致、代码各自一份**——判据是"同一组问法得到同一组答案"。
///
/// ## 引擎要点（与设计文档一致）
///
/// 1. **中文 1-gram + 2-gram**：1-gram 保召回（2 字词不失效），2-gram 保精度。
/// 2. **字段加权 BM25**：标题 12 ≫ 别名 10 ≫ 负责人 6 ≫ 事实 5 ≫ 正文 3。
/// 3. **长文本字段不索引 1-gram**：posting 降 30%、质量不变（实测）。
/// 4. **覆盖度加成**：命中 3 个查询词的条目优于只命中 1 个 3 次的。
/// 5. **短语面**：标题/别名整体出现在查询里时大幅加分——这是"叫得出名字"的情形。

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cefu {

/* ══════════════ UTF-8 与分词 ══════════════ */

inline constexpr std::u32string_view kStopHan = U"的了是在和与或有吗呢吧啊我你他它这那要会就都很个上中";

inline auto is_stop(char32_t c) -> bool { return kStopHan.find(c) != std::u32string_view::npos; }
inline auto is_han(char32_t cp) -> bool { return cp >= 0x4E00 && cp <= 0x9FFF; }

inline auto decode_utf8(std::string_view text) -> std::u32string {
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
      ++i;
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

inline auto encode_utf8(char32_t cp) -> std::string {
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

inline auto is_ascii_word(char32_t cp) -> bool {
  if (cp >= 128) return false;
  const auto c = static_cast<char>(cp);
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '.' || c == ':' || c == '#' || c == '@' || c == '-' || c == '/';
}

inline auto to_lower(std::string s) -> std::string {
  for (auto& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

/// 去空白 + 小写（短语面比较用）。
inline auto compact(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

inline constexpr int kShortGrams[] = {1, 2};
inline constexpr int kLongGrams[] = {2};

/// 中文按 gram 切，ASCII 词整体保留（`ou_abc` 这类标识符会拆成 `ou`/`abc` 两段索引）。
inline auto tokenize(std::string_view text, const int* grams, std::size_t gram_count)
    -> std::vector<std::string> {
  const auto cps = decode_utf8(text);
  std::vector<std::string> out;
  out.reserve(cps.size() * 2);

  std::vector<char32_t> ascii;
  const auto flush_ascii = [&] {
    if (ascii.size() < 2) {
      ascii.clear();
      return;
    }
    std::string word;
    word.reserve(ascii.size());
    for (const auto cp : ascii) word.push_back(static_cast<char>(cp));
    word = to_lower(std::move(word));
    if (word.find_first_of("_.:#@-/") != std::string::npos) {
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
    for (std::size_t gi = 0; gi < gram_count; ++gi) {
      const int n = grams[gi];
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

/* ══════════════ 词条 ══════════════ */

struct FieldSpec {
  std::string_view key;
  double weight;
  bool pos_decay;   ///< 列表型字段：命中越靠前越重要（steps[0] > steps[3]）
  bool short_text;  ///< 短文本（标题/别名）：索引 1-gram 提升召回
};

inline constexpr FieldSpec kFields[] = {
    {"title", 12.0, false, true},    {"aliases", 10.0, false, true},
    {"category", 4.0, false, true},  {"owner", 6.0, false, true},
    {"facts", 5.0, false, true},     {"commands", 4.0, false, false},
    {"causes", 3.0, false, false},   {"symptom", 3.0, false, false},
    {"steps", 3.0, true, false},     {"tags", 3.0, false, true},
};
inline constexpr std::size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

struct Entry {
  std::string id, category, title;
  std::vector<std::string> aliases, causes, steps, commands, facts, links, tags;
  std::string symptom, owner, scope;
  std::string source_kind, source_ref;
  std::int64_t source_at{0}, created_at{0}, updated_at{0}, expires_at{0};
  std::string confidence{"inferred"};
  std::string supersedes;
  std::int64_t hits{0}, last_hit_at{0};
  bool needs_review{false};

  auto values(std::string_view key, std::vector<std::string_view>& out) const -> void {
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

  /// 一句话结论：优先"第一步该做什么"（工具书里最有用的一行）。
  [[nodiscard]] auto key_line() const -> std::string {
    if (!steps.empty()) return steps.front();
    if (!symptom.empty()) return symptom;
    if (!causes.empty()) return causes.front();
    return {};
  }
};

/* ══════════════ 倒排索引 ══════════════ */

inline constexpr std::size_t kNoDoc = static_cast<std::size_t>(-1);

struct Hit {
  const Entry* entry;
  double score;
  std::vector<std::string> why;
};

class Index {
 public:
  Index() : fields_(kFieldCount), avg_(kFieldCount, 1.0) {}

  auto upsert(const Entry& e) -> void {
    const auto it = id_to_doc_.find(e.id);
    if (it != id_to_doc_.end()) {
      unindex(it->second);
      entries_[it->second] = e;
      index(it->second);
      return;
    }
    const auto doc = entries_.size();
    entries_.push_back(e);
    id_to_doc_.emplace(e.id, doc);
    field_len_.push_back(std::vector<std::int32_t>(kFieldCount, 0));
    index(doc);
  }

  [[nodiscard]] auto find(std::string_view id) const -> const Entry* {
    const auto it = id_to_doc_.find(std::string(id));
    return it == id_to_doc_.end() ? nullptr : &entries_[it->second];
  }

  [[nodiscard]] auto entries() const -> const std::vector<Entry>& { return entries_; }
  [[nodiscard]] auto mutable_entries() -> std::vector<Entry>& { return entries_; }
  [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }
  auto rebuild() -> void {
    id_to_doc_.clear();
    for (std::size_t i = 0; i < entries_.size(); ++i) id_to_doc_.emplace(entries_[i].id, i);
  }

  /// 重算每个字段的平均长度（BM25 的长度归一化基准）。
  auto recompute() -> void {
    const auto n = static_cast<double>(entries_.size());
    avg_.assign(kFieldCount, 1.0);
    if (n <= 0) return;
    std::vector<double> sums(kFieldCount, 0.0);
    for (const auto& lens : field_len_) {
      for (std::size_t fi = 0; fi < kFieldCount; ++fi) sums[fi] += static_cast<double>(lens[fi]);
    }
    for (std::size_t fi = 0; fi < kFieldCount; ++fi) avg_[fi] = sums[fi] > 0 ? sums[fi] / n : 1.0;
  }

  [[nodiscard]] auto search(std::string_view query, std::size_t limit) const -> std::vector<Hit> {
    if (entries_.empty()) return {};

    // 两条 token 流：短字段（1+2gram）与长字段（仅 2gram）
    auto short_tokens = tokenize(query, kShortGrams, 2);
    auto long_tokens = tokenize(query, kLongGrams, 1);
    std::unordered_set<std::string> seen;
    std::vector<std::string> q;
    q.reserve(short_tokens.size());
    for (auto& t : short_tokens) {
      if (seen.insert(t).second) q.push_back(std::move(t));
    }
    if (q.empty()) return {};
    const std::unordered_set<std::string> long_set(long_tokens.begin(), long_tokens.end());

    const auto n = entries_.size();
    std::vector<double> scores(n, 0.0);
    std::vector<std::int32_t> hit_count(n, 0);
    // 去重标记：记"该文档最近一次为哪个 query token 计过数"（每 token 只记一次覆盖度）
    std::vector<std::uint32_t> token_mark(n, UINT32_MAX);
    std::vector<std::size_t> touched;
    touched.reserve(256);

    const auto nd = static_cast<double>(n);
    constexpr double k1 = 1.2;
    constexpr double k_len = 0.75;
    std::uint32_t token_idx = 0;

    for (const auto& token : q) {
      const auto this_token = token_idx++;
      double df_total = 0;
      for (std::size_t fi = 0; fi < kFieldCount; ++fi) {
        const auto it = fields_[fi].find(token);
        if (it != fields_[fi].end()) df_total += static_cast<double>(it->second.size());
      }
      if (df_total <= 0) continue;
      double idf = std::log(1.0 + (nd - df_total + 0.5) / (df_total + 0.5));
      if (idf <= 0) idf = 1e-6;

      for (std::size_t fi = 0; fi < kFieldCount; ++fi) {
        const auto& spec = kFields[fi];
        // 长文本字段只认 2-gram：1-gram posting 太多（实测降 30% 索引量、质量不变）
        if (!spec.short_text && long_set.find(token) == long_set.end()) continue;
        const auto it = fields_[fi].find(token);
        if (it == fields_[fi].end()) continue;
        const double avg = avg_[fi] > 0 ? avg_[fi] : 1.0;
        for (const auto& p : it->second) {
          double flen = static_cast<double>(field_len_[p.doc][fi]);
          if (flen <= 0) flen = 1;
          const double tf = p.tf;
          double tf_score = tf * (k1 + 1.0) / (tf + k1 * (1.0 - k_len + k_len * flen / avg));
          if (spec.pos_decay && p.min_list > 0) tf_score /= 1.0 + 0.35 * static_cast<double>(p.min_list);
          const double s = spec.weight * idf * tf_score;
          if (scores[p.doc] == 0.0) touched.push_back(p.doc);
          scores[p.doc] += s;
          if (token_mark[p.doc] != this_token) {
            token_mark[p.doc] = this_token;
            hit_count[p.doc] += 1;
          }
        }
      }
    }

    // 覆盖度加成：命中 3 个查询词优于 1 个词命中 3 次
    const auto qlen = static_cast<double>(q.size());
    for (const auto doc : touched) {
      scores[doc] *= 1.0 + 0.8 * (static_cast<double>(hit_count[doc]) / qlen);
    }

    // 短语面：标题/别名整体出现在查询里（"叫得出名字"的情形）
    const auto qc = compact(query);
    std::map<std::size_t, std::vector<std::string>> whys;
    for (std::size_t doc = 0; doc < entries_.size(); ++doc) {
      double bonus = 0;
      std::vector<std::string> why;
      const auto title = compact(entries_[doc].title);
      if (title.size() >= 2 &&
          (qc.find(title) != std::string::npos || title.find(qc) != std::string::npos)) {
        bonus += 8;
        why.emplace_back("标题整体匹配");
      }
      for (const auto& a : entries_[doc].aliases) {
        const auto ac = compact(a);
        if (ac.size() < 2) continue;
        if (qc.find(ac) != std::string::npos) {
          bonus += 6;
          why.push_back("别名「" + a + "」");
        } else if (qc.size() >= 2 && (ac.rfind(qc, 0) == 0 || qc.rfind(ac, 0) == 0)) {
          bonus += 3;
        }
      }
      if (bonus > 0) {
        if (scores[doc] == 0.0) touched.push_back(doc);
        scores[doc] += bonus * 3;
        if (!why.empty()) whys.emplace(doc, std::move(why));
      }
    }

    std::vector<Hit> out;
    out.reserve(touched.size());
    for (const auto doc : touched) {
      if (scores[doc] <= 0) continue;
      Hit h{&entries_[doc], scores[doc], {}};
      const auto wi = whys.find(doc);
      if (wi != whys.end()) h.why = wi->second;
      out.push_back(std::move(h));
    }
    // 没有短语面命中时，也说清"靠什么召回的"：报出得分最高的字段。
    // 否则 `why` 为空会让调用方以为"莫名其妙就出来了"——而判断"该不该补别名"正需要这个依据。
    for (auto& h : out) {
      if (!h.why.empty()) continue;
      double best = 0;
      std::string best_field;
      std::vector<std::string_view> vals;
      for (std::size_t fi = 0; fi < kFieldCount; ++fi) {
        const auto& spec = kFields[fi];
        h.entry->values(spec.key, vals);
        auto best_in_field = 0.0;
        for (const auto& v : vals) {
          if (v.empty()) continue;
          const auto toks = spec.short_text ? tokenize(v, kShortGrams, 2) : tokenize(v, kLongGrams, 1);
          for (const auto& t : toks) {
            if (seen.count(t) != 0) best_in_field = std::max(best_in_field, spec.weight);
          }
        }
        if (best_in_field > best) {
          best = best_in_field;
          best_field = std::string(spec.key);
        }
      }
      if (!best_field.empty()) h.why.push_back("命中字段 " + best_field);
    }

    std::sort(out.begin(), out.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (out.size() > limit) out.resize(limit);
    return out;
  }

  /// 近似查重：标题 + 别名的 2-gram Jaccard。
  [[nodiscard]] static auto similarity(const Entry& a, const Entry& b) -> double {
    const auto make_set = [](const Entry& e) {
      std::unordered_set<std::string> s;
      for (const auto& t : tokenize(e.title, kLongGrams, 1)) s.insert(t);
      for (const auto& al : e.aliases) {
        for (const auto& t : tokenize(al, kLongGrams, 1)) s.insert(t);
      }
      return s;
    };
    const auto sa = make_set(a);
    const auto sb = make_set(b);
    if (sa.empty() || sb.empty()) return 0.0;
    std::size_t inter = 0;
    for (const auto& t : sa) {
      if (sb.count(t) != 0) ++inter;
    }
    return static_cast<double>(inter) / static_cast<double>(sa.size() + sb.size() - inter);
  }

 private:
  struct Posting {
    std::size_t doc;
    double tf;
    std::int32_t min_list;
  };
  using Table = std::map<std::string, std::vector<Posting>, std::less<>>;

  std::vector<Entry> entries_;
  std::map<std::string, std::size_t, std::less<>> id_to_doc_;
  std::vector<Table> fields_;
  std::vector<std::vector<std::int32_t>> field_len_;
  std::vector<double> avg_;

  auto index(std::size_t doc) -> void {
    auto& lens = field_len_[doc];
    std::vector<std::string_view> vals;
    for (std::size_t fi = 0; fi < kFieldCount; ++fi) {
      const auto& spec = kFields[fi];
      entries_[doc].values(spec.key, vals);
      std::unordered_map<std::string, std::pair<double, std::int32_t>> per;
      for (std::size_t li = 0; li < vals.size(); ++li) {
        if (vals[li].empty()) continue;
        const auto toks = spec.short_text ? tokenize(vals[li], kShortGrams, 2)
                                          : tokenize(vals[li], kLongGrams, 1);
        for (const auto& t : toks) {
          auto& slot = per[t];
          slot.first += 1.0;
          if (spec.pos_decay) {
            const auto l = static_cast<std::int32_t>(li);
            if (slot.second < 0 || l < slot.second) slot.second = l;
          }
        }
      }
      lens[fi] = static_cast<std::int32_t>(per.size());
      for (const auto& [t, info] : per) {
        fields_[fi][t].push_back(Posting{doc, info.first, info.second});
      }
    }
  }

  auto unindex(std::size_t doc) -> void {
    for (auto& table : fields_) {
      for (auto it = table.begin(); it != table.end();) {
        auto& plist = it->second;
        plist.erase(std::remove_if(plist.begin(), plist.end(),
                                   [doc](const Posting& p) { return p.doc == doc; }),
                    plist.end());
        if (plist.empty()) {
          it = table.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
};

}  // namespace cefu
