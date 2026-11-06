#include "proposal.hpp"

#include <algorithm>
#include <ctime>
#include <format>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"

namespace caiyun {
namespace {

using st::ErrorCode;

[[nodiscard]] auto now_iso8601() -> std::string {
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

/// 提案 id 只能含安全字符：它会被当成文件名（以及将来可能进 URL/命令行）。
[[nodiscard]] auto is_safe_id(std::string_view id) -> bool {
  if (id.empty()) return false;
  for (const char ch : id) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
    if (!ok) return false;
  }
  return true;
}

[[nodiscard]] auto proposal_path(std::string_view package_root, std::string_view id)
    -> std::string {
  return st::fs::join(proposals_dir(package_root), std::string{id} + ".json");
}

[[nodiscard]] auto serialize(const Proposal& proposal) -> st::Json {
  st::Json node = st::Json::object();
  node["id"] = proposal.id;
  node["author"] = proposal.author;
  node["created_at"] = proposal.created_at;
  node["summary"] = proposal.summary;
  node["rationale"] = proposal.rationale;
  node["state"] = std::string{to_string(proposal.state)};
  if (!proposal.decided_by.empty()) node["decided_by"] = proposal.decided_by;
  if (!proposal.decided_at.empty()) node["decided_at"] = proposal.decided_at;
  if (!proposal.decision_note.empty()) node["decision_note"] = proposal.decision_note;
  if (!proposal.superseded_by.empty()) node["superseded_by"] = proposal.superseded_by;
  if (!proposal.changes.is_null()) node["changes"] = proposal.changes;
  return node;
}

}  // namespace

auto to_string(ProposalState state) -> std::string_view {
  switch (state) {
    case ProposalState::Open: return "open";
    case ProposalState::Accepted: return "accepted";
    case ProposalState::Rejected: return "rejected";
    case ProposalState::Superseded: return "superseded";
  }
  return "open";
}

auto parse_proposal_state(std::string_view text) -> ProposalState {
  if (text == "accepted") return ProposalState::Accepted;
  if (text == "rejected") return ProposalState::Rejected;
  if (text == "superseded") return ProposalState::Superseded;
  return ProposalState::Open;
}

auto proposals_dir(std::string_view package_root) -> std::string {
  return st::fs::join(std::string{package_root}, "proposals");
}

auto write_proposal(std::string_view package_root, const Proposal& proposal) -> st::Status {
  if (!is_safe_id(proposal.id)) {
    return st::unexpected(ErrorCode::Invalid,
                          std::format("提案 id 只能含字母/数字/-/_/.：「{}」", proposal.id));
  }
  if (const auto made = st::fs::create_directories(proposals_dir(package_root)); !made) {
    return st::forward_error(made.error());
  }
  return st::fs::write_text(proposal_path(package_root, proposal.id),
                            st::json_dump(serialize(proposal), true, 2));
}

auto read_proposal(std::string_view package_root, std::string_view id) -> Result<Proposal> {
  if (!is_safe_id(id)) {
    return st::unexpected(ErrorCode::Invalid, std::format("非法提案 id：「{}」", id));
  }
  const std::string path = proposal_path(package_root, id);
  const auto text = st::fs::read_text(path);
  if (!text) {
    return st::unexpected(ErrorCode::NotFound, std::format("提案不存在：{}", path));
  }
  const auto parsed = st::json_parse(*text);
  if (!parsed || !parsed->is_object()) {
    return st::unexpected(ErrorCode::Parse, std::format("提案文件不是合法 JSON：{}", path));
  }
  Proposal proposal{};
  proposal.id = st::json_get_string(*parsed, "id", std::string{id});
  proposal.author = st::json_get_string(*parsed, "author");
  proposal.created_at = st::json_get_string(*parsed, "created_at");
  proposal.summary = st::json_get_string(*parsed, "summary");
  proposal.rationale = st::json_get_string(*parsed, "rationale");
  proposal.state = parse_proposal_state(st::json_get_string(*parsed, "state", "open"));
  proposal.decided_by = st::json_get_string(*parsed, "decided_by");
  proposal.decided_at = st::json_get_string(*parsed, "decided_at");
  proposal.decision_note = st::json_get_string(*parsed, "decision_note");
  proposal.superseded_by = st::json_get_string(*parsed, "superseded_by");
  if (const auto it = parsed->find("changes"); it != parsed->end()) proposal.changes = *it;
  return proposal;
}

auto list_proposals(std::string_view package_root) -> Result<std::vector<Proposal>> {
  std::vector<Proposal> out;
  const std::string dir = proposals_dir(package_root);
  if (!st::fs::is_directory(dir)) return out;

  const auto entries = st::fs::list_dir(dir);
  if (!entries) return st::forward_error(entries.error());
  for (const auto& entry : *entries) {
    if (entry.is_dir) continue;
    if (entry.name.size() < 5 || entry.name.substr(entry.name.size() - 5) != ".json") continue;
    const std::string id = entry.name.substr(0, entry.name.size() - 5);
    const auto proposal = read_proposal(package_root, id);
    if (!proposal) {
      // 坏文件跳过而不是整体失败：一条坏提案不该让整个清单读不出来。
      ST_LOG_WARN("跳过坏提案 {}：{}", id, proposal.error().message);
      continue;
    }
    out.push_back(*proposal);
  }
  std::sort(out.begin(), out.end(),
            [](const Proposal& a, const Proposal& b) { return a.created_at < b.created_at; });
  return out;
}

auto propose(std::string_view package_root, std::string_view author, std::string_view summary,
             std::string_view rationale, st::Json changes) -> Result<Proposal> {
  if (rationale.empty()) {
    // **没有理由的提案不该被采纳**，所以在建立时就拒绝。
    // 实测：留空的理由在评审时一定会被问"为什么"，那时已经没人记得了。
    return st::unexpected(ErrorCode::Invalid, "提案必须写明理由（rationale 不能为空）");
  }
  Proposal proposal{};
  proposal.author = std::string{author};
  proposal.created_at = now_iso8601();
  proposal.summary = std::string{summary};
  proposal.rationale = std::string{rationale};
  proposal.state = ProposalState::Open;
  proposal.changes = std::move(changes);

  // id = 时间戳 + 一个短后缀，避免同秒内撞号。
  // 用时间戳而不是递增序号：多个智能体并发提议时，递增序号需要锁
  //（读最大值→写，中间有竞态），而时间戳天然不冲突。
  const std::string stamp = now_iso8601();
  std::string compact;
  for (const char ch : stamp) {
    if (ch >= '0' && ch <= '9') compact.push_back(ch);
  }
  // 同秒撞号：找到第一个没被占用的后缀。
  for (int salt = 0; salt < 1000; ++salt) {
    proposal.id = salt == 0 ? std::format("p-{}", compact)
                            : std::format("p-{}-{}", compact, salt);
    if (!st::fs::exists(proposal_path(package_root, proposal.id))) break;
  }

  if (const auto wrote = write_proposal(package_root, proposal); !wrote) {
    return st::forward_error(wrote.error());
  }

  HistoryEntry entry{};
  entry.action = "propose";
  entry.actor = proposal.author;
  entry.summary = proposal.summary;
  entry.detail = st::Json::object();
  entry.detail["proposal_id"] = proposal.id;
  entry.detail["rationale"] = proposal.rationale;
  if (const auto appended = append_history(package_root, std::move(entry)); !appended) {
    return st::forward_error(appended.error());
  }
  return proposal;
}

auto decide_proposal(std::string_view package_root, std::string_view id,
                     ProposalState decision, std::string_view decided_by,
                     std::string_view note, ProposalEffect effect) -> Result<Proposal> {
  if (decision != ProposalState::Accepted && decision != ProposalState::Rejected) {
    return st::unexpected(ErrorCode::Invalid,
                          "decide_proposal 只接受 accepted / rejected（取代请用 supersede）");
  }
  auto proposal = read_proposal(package_root, id);
  if (!proposal) return st::forward_error(proposal.error());

  // 已终态的不能再改：那会让审计流水自相矛盾（同一条提案"既采纳又否决"）。
  if (proposal->state != ProposalState::Open) {
    return st::unexpected(
        ErrorCode::Invalid,
        std::format("提案 {} 已是终态（{}），不能再次裁决。要推翻它请提一条新提案。", id,
                    to_string(proposal->state)));
  }

  proposal->state = decision;
  proposal->decided_by = std::string{decided_by};
  proposal->decided_at = now_iso8601();
  proposal->decision_note = std::string{note};
  if (const auto wrote = write_proposal(package_root, *proposal); !wrote) {
    return st::forward_error(wrote.error());
  }

  HistoryEntry entry{};
  entry.action = decision == ProposalState::Accepted ? "accept" : "reject";
  entry.actor = std::string{decided_by};
  entry.summary = std::format("{}：{}", decision == ProposalState::Accepted ? "采纳" : "否决",
                              proposal->summary);
  entry.detail = st::Json::object();
  entry.detail["proposal_id"] = proposal->id;
  if (!note.empty()) entry.detail["note"] = std::string{note};
  // 效果评估进历史而不是只留在提案文件里：审计要看的是
  // "采纳那一刻发生了什么"，而提案文件反映的是**当前**状态。
  if (!effect.verdict.empty() || effect.mean_diff_pct > 0.0 ||
      !effect.snapshot_before.empty()) {
    st::Json eff = st::Json::object();
    if (!effect.snapshot_before.empty()) eff["snapshot_before"] = effect.snapshot_before;
    if (!effect.snapshot_after.empty()) eff["snapshot_after"] = effect.snapshot_after;
    if (effect.mean_diff_pct > 0.0) eff["mean_diff_pct"] = effect.mean_diff_pct;
    if (!effect.verdict.empty()) eff["verdict"] = effect.verdict;
    if (!effect.metrics.is_null()) eff["metrics"] = effect.metrics;
    entry.detail["effect"] = std::move(eff);
  }
  if (const auto appended = append_history(package_root, std::move(entry)); !appended) {
    return st::forward_error(appended.error());
  }
  return proposal;
}

auto supersede_proposal(std::string_view package_root, std::string_view id,
                        std::string_view by_id, std::string_view by_whom,
                        std::string_view note) -> Result<Proposal> {
  auto proposal = read_proposal(package_root, id);
  if (!proposal) return st::forward_error(proposal.error());
  if (proposal->state != ProposalState::Open) {
    return st::unexpected(ErrorCode::Invalid,
                          std::format("提案 {} 已是终态（{}），不能再被取代", id,
                                      to_string(proposal->state)));
  }
  proposal->state = ProposalState::Superseded;
  proposal->superseded_by = std::string{by_id};
  proposal->decided_by = std::string{by_whom};
  proposal->decided_at = now_iso8601();
  proposal->decision_note = std::string{note};
  if (const auto wrote = write_proposal(package_root, *proposal); !wrote) {
    return st::forward_error(wrote.error());
  }

  HistoryEntry entry{};
  entry.action = "supersede";
  entry.actor = std::string{by_whom};
  entry.summary = std::format("取代提案 {} → {}", id, by_id);
  entry.detail = st::Json::object();
  entry.detail["proposal_id"] = std::string{id};
  entry.detail["superseded_by"] = std::string{by_id};
  if (const auto appended = append_history(package_root, std::move(entry)); !appended) {
    return st::forward_error(appended.error());
  }
  return proposal;
}

}  // namespace caiyun
