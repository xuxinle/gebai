/// 提案与采纳：让历史层能回答「谁改了什么、为什么、后来采纳没有」。
///
/// ## 为什么提案要与历史分开建模
///
/// 历史层是**追加的、不可改的**事实流：这件事发生过。
/// 而提案是**可悬置的意图**："AI 建议把第二镜的相机拉近"——它可能被采纳，
/// 也可能被否决，还可能被另一个提案取代。这些状态变更必须能表达，
/// 而"往历史里再追加一条"无法表达（历史只记发生过什么，不记"当前是什么状态"）。
///
/// 所以：
/// - **提案内容**存 `proposals/<id>.json`（可被状态更新改写）；
/// - **状态变更**追加进历史（`propose` / `accept` / `reject` / `supersede`）；
/// - 两处用同一个 `id` 关联。历史是审计流水，提案文件是当前视图。
///
/// ## 为什么不用数据库
///
/// 提案的数量级是"一个工程几十条"，而工程要能被 git 管理、能被人直接打开看。
/// 一个 JSON 文件一条提案，`git log` 就能看出"这个建议什么时候被采纳的"。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "package.hpp"
#include "st/core/error.hpp"
#include "st/core/json.hpp"

namespace caiyun {

using st::Result;

/// 提案的当前状态。
enum class ProposalState : std::uint8_t {
  Open,       ///< 待定（等人或另一个智能体表态）
  Accepted,   ///< 已采纳
  Rejected,   ///< 已否决
  Superseded, ///< 被另一个提案取代（保留链条，不删记录）
};

[[nodiscard]] auto to_string(ProposalState state) -> std::string_view;
[[nodiscard]] auto parse_proposal_state(std::string_view text) -> ProposalState;

/// 一条提案。
struct Proposal {
  /// 稳定标识（`p-<时间戳>-<序号>` 或人取的名字）。同一工程内唯一。
  std::string id{};
  /// 谁提的：`agent` / `human` / 具体名字。
  std::string author{};
  /// 建立时间（ISO-8601 UTC）。
  std::string created_at{};
  /// 一句话说清要做什么（面向人）。
  std::string summary{};
  /// 为什么——**这条是提案的核心**，没有理由的提案不该被采纳。
  std::string rationale{};
  /// 状态。
  ProposalState state{ProposalState::Open};
  /// 被谁采纳/否决 + 时间 + 理由（只在终态时有值）。
  std::string decided_by{};
  std::string decided_at{};
  std::string decision_note{};
  /// 提案涉及的具体改动（自由形状：`{"path": "...", "before": ..., "after": ...}`）。
  st::Json changes{};
  /// 若被取代，指向新提案的 id。
  std::string superseded_by{};
};

/// 一条提案的效果评估（采纳后由调用方回填）。
///
/// 为何要有它：一个"看起来变好了"的改动，需要有**可比对的证据**才能说服人。
/// 视觉 diff 的均差、时长变化、过曝比例——都放在这里。
struct ProposalEffect {
  /// 采纳前的快照名与采纳后的快照名（对应 `snapshots/<名>/`）。
  std::string snapshot_before{};
  std::string snapshot_after{};
  /// 视觉均差（%）。0 = 与采纳前逐像素一致。
  double mean_diff_pct{0.0};
  /// 落定的判词（`完全一致` / `局部改动` …）。
  std::string verdict{};
  /// 其他度量（时长/过曝/静止占比…自由形状）。
  st::Json metrics{};
};

/// 提案目录（`<包根>/proposals/`）。
[[nodiscard]] auto proposals_dir(std::string_view package_root) -> std::string;

/// 写一条提案（覆盖同 id 的旧文件）。
[[nodiscard]] auto write_proposal(std::string_view package_root, const Proposal& proposal)
    -> st::Status;

/// 读一条提案。不存在返回 `NotFound`。
[[nodiscard]] auto read_proposal(std::string_view package_root, std::string_view id)
    -> Result<Proposal>;

/// 列出全部提案（按建立时间升序；坏文件跳过并记警告）。
[[nodiscard]] auto list_proposals(std::string_view package_root)
    -> Result<std::vector<Proposal>>;

/// 新建一条提案（自动生成 id 与时间），并往历史层追加 `propose` 记录。
[[nodiscard]] auto propose(std::string_view package_root, std::string_view author,
                           std::string_view summary, std::string_view rationale,
                           st::Json changes = {}) -> Result<Proposal>;

/// 采纳/否决一条提案：改状态 + 追加历史记录。
///
/// **会检查当前状态**：已终态的提案不能再改（那会让审计流水对不上）。
/// 要推翻一个已采纳的改动，正确做法是**提一条新提案**指向它。
[[nodiscard]] auto decide_proposal(std::string_view package_root, std::string_view id,
                                   ProposalState decision, std::string_view decided_by,
                                   std::string_view note = {},
                                   ProposalEffect effect = {}) -> Result<Proposal>;

/// 把一条提案标记为被另一条取代。
[[nodiscard]] auto supersede_proposal(std::string_view package_root, std::string_view id,
                                      std::string_view by_id, std::string_view by_whom,
                                      std::string_view note = {}) -> Result<Proposal>;

}  // namespace caiyun
