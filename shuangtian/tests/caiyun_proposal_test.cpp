/// 提案与采纳（P3 历史层）。
///
/// ## 这组用例守的是什么
///
/// 提案层要能回答「谁改了什么、为什么、采纳了没有、采纳后效果如何」。
/// 四个最容易坏的地方：
///
/// 1. **没有理由的提案**——留空的 rationale 在评审时一定被问"为什么"，
///    而那时已经没人记得了。建立时就该拒绝。
/// 2. **终态被二次裁决**——同一条提案"既采纳又否决"，审计流水就自相矛盾。
/// 3. **提案与历史脱钩**——改了提案文件却没进历史，等于这段决策没有留痕。
/// 4. **id 不安全**——id 会被当文件名；带 `../` 就写出包外了。
#include <string>
#include <vector>

#include "examples/caiyun/package.hpp"
#include "examples/caiyun/proposal.hpp"
// 测试目标把所有 `tests/*_test.cpp` 链成一个可执行文件，因此
// `package.cpp` / `package_history.cpp` 的实现只能由**一个**测试文件包含
//（它们已由 caiyun_package_test.cpp / caiyun_history 那边提供）。
// 这里只包含本层自己的实现。
#include "examples/caiyun/proposal.cpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"

namespace {

[[nodiscard]] auto fresh_pkg(std::string_view name) -> std::string {
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st_cyprop_" + std::string{name});
  (void)st::fs::remove_all(dir);
  (void)st::fs::create_directories(dir);
  // 建一个最小可用的包清单（proposal 层只依赖 history 与 manifest）
  caiyun::PackageManifest manifest{};
  manifest.project_id = "propo";
  manifest.scene_path = "scene.json";
  manifest.history_dir = "history";
  (void)st::fs::write_text(st::fs::join(dir, "scene.json"), R"({"scenes": []})");
  (void)caiyun::write_manifest(dir, manifest);
  return dir;
}

}  // namespace

ST_TEST(caiyun_proposal_requires_rationale) {
  const std::string dir = fresh_pkg("norationale");
  // 空理由必须被拒绝——这是"提案"与"命令"的分界线
  const auto empty = caiyun::propose(dir, "agent", "把相机拉近", "");
  ST_CHECK(!empty.has_value());
  if (!empty) {
    st::log::out("空理由的报错：{}\n", empty.error().message);
    ST_CHECK(empty.error().message.find("理由") != std::string::npos);
  }
}

ST_TEST(caiyun_proposal_create_and_read_back) {
  const std::string dir = fresh_pkg("create");
  st::Json changes = st::Json::object();
  changes["path"] = "scenes[1].cam_zoom_to";
  changes["before"] = 1.06;
  changes["after"] = 1.09;

  const auto made = caiyun::propose(dir, "agent", "把第二镜推近一点",
                                    "聚焦产品细节；实测 1.06 时细节仍偏小", changes);
  ST_CHECK(made.has_value());
  if (!made) {
    st::log::out("提议失败：{}\n", made.error().message);
    return;
  }
  ST_CHECK(!made->id.empty());
  ST_CHECK(made->state == caiyun::ProposalState::Open);
  ST_CHECK(!made->created_at.empty());

  const auto reread = caiyun::read_proposal(dir, made->id);
  ST_CHECK(reread.has_value());
  if (!reread) return;
  ST_CHECK(reread->summary == "把第二镜推近一点");
  ST_CHECK(reread->rationale.find("1.06") != std::string::npos);
  ST_CHECK(reread->changes.is_object());
  ST_CHECK(reread->changes["after"].get<double>() > 1.08);

  // 历史里必须有 propose 记录（提案与历史不脱钩）
  const auto history = caiyun::read_history(dir);
  ST_CHECK(history.has_value());
  if (!history) return;
  ST_CHECK(history->size() == 1);
  if (!history->empty()) {
    ST_CHECK((*history)[0].action == "propose");
    ST_CHECK((*history)[0].actor == "agent");
  }
}

ST_TEST(caiyun_proposal_accept_writes_history_with_effect) {
  const std::string dir = fresh_pkg("accept");
  const auto made = caiyun::propose(dir, "agent", "调亮主光", "暗场里主体读不清");
  ST_CHECK(made.has_value());
  if (!made) return;

  caiyun::ProposalEffect effect{};
  effect.snapshot_before = "snapshots/v1";
  effect.snapshot_after = "snapshots/v2";
  effect.mean_diff_pct = 3.25;
  effect.verdict = "局部改动（均差 3.250%）";

  const auto decided =
      caiyun::decide_proposal(dir, made->id, caiyun::ProposalState::Accepted, "human", "可以",
                              effect);
  ST_CHECK(decided.has_value());
  if (!decided) {
    st::log::out("采纳失败：{}\n", decided.error().message);
    return;
  }
  ST_CHECK(decided->state == caiyun::ProposalState::Accepted);
  ST_CHECK(decided->decided_by == "human");
  ST_CHECK(!decided->decided_at.empty());
  ST_CHECK(decided->decision_note == "可以");

  // 历史：propose + accept 两条，且 accept 里带效果评估
  const auto history = caiyun::read_history(dir);
  ST_CHECK(history.has_value());
  if (!history) return;
  ST_CHECK(history->size() == 2);
  if (history->size() < 2) return;
  const auto& accept = (*history)[1];
  ST_CHECK(accept.action == "accept");
  ST_CHECK(accept.detail.is_object());
  const auto effect_node = accept.detail.find("effect");
  ST_CHECK(effect_node != accept.detail.end());
  if (effect_node != accept.detail.end()) {
    ST_CHECK((*effect_node)["mean_diff_pct"].get<double>() > 3.2);
    ST_CHECK((*effect_node)["verdict"].get<std::string>().find("局部改动") != std::string::npos);
  }
}

ST_TEST(caiyun_proposal_terminal_state_cannot_be_redecided) {
  const std::string dir = fresh_pkg("terminal");
  const auto made = caiyun::propose(dir, "agent", "改标题", "原标题太长");
  ST_CHECK(made.has_value());
  if (!made) return;
  ST_CHECK(caiyun::decide_proposal(dir, made->id, caiyun::ProposalState::Accepted, "human")
               .has_value());

  // 再否决一次必须失败：同一条提案"既采纳又否决"会让审计自相矛盾
  const auto again =
      caiyun::decide_proposal(dir, made->id, caiyun::ProposalState::Rejected, "human");
  ST_CHECK(!again.has_value());
  if (!again) {
    st::log::out("二次裁决的报错：{}\n", again.error().message);
    ST_CHECK(again.error().message.find("终态") != std::string::npos);
  }

  // 裁决接口不接受 Open/Superseded（那是别的动作）
  const auto weird =
      caiyun::decide_proposal(dir, made->id, caiyun::ProposalState::Open, "human");
  ST_CHECK(!weird.has_value());
}

ST_TEST(caiyun_proposal_supersede_chain) {
  const std::string dir = fresh_pkg("supersede");
  const auto first = caiyun::propose(dir, "agent", "推近到 1.09", "细节偏小");
  ST_CHECK(first.has_value());
  if (!first) return;
  const auto second = caiyun::propose(dir, "agent", "推近到 1.12", "1.09 实测仍偏小");
  ST_CHECK(second.has_value());
  if (!second) return;

  const auto done =
      caiyun::supersede_proposal(dir, first->id, second->id, "agent", "幅度不够");
  ST_CHECK(done.has_value());
  if (!done) {
    st::log::out("取代失败：{}\n", done.error().message);
    return;
  }
  ST_CHECK(done->state == caiyun::ProposalState::Superseded);
  ST_CHECK(done->superseded_by == second->id);

  // 被取代后不能再裁决
  ST_CHECK(!caiyun::decide_proposal(dir, first->id, caiyun::ProposalState::Accepted, "human")
                .has_value());
}

ST_TEST(caiyun_proposal_rejects_unsafe_id) {
  const std::string dir = fresh_pkg("unsafe");
  // id 会被当文件名：带路径分隔符就能写出包外
  caiyun::Proposal evil{};
  evil.id = "../escaped";
  evil.summary = "x";
  ST_CHECK(!caiyun::write_proposal(dir, evil).has_value());

  evil.id = "ok-id_1.2";
  ST_CHECK(caiyun::write_proposal(dir, evil).has_value());

  // 读的时候同样要挡（防止被手工构造的文件名绕过）
  ST_CHECK(!caiyun::read_proposal(dir, "../package").has_value());
}

ST_TEST(caiyun_proposal_list_skips_broken_files) {
  const std::string dir = fresh_pkg("broken");
  ST_CHECK(caiyun::propose(dir, "a", "一", "理由一").has_value());
  ST_CHECK(caiyun::propose(dir, "b", "二", "理由二").has_value());
  // 手工塞一个坏文件：一条坏提案不该让整个清单读不出来
  (void)st::fs::write_text(st::fs::join(caiyun::proposals_dir(dir), "bad.json"), "{not json");

  const auto list = caiyun::list_proposals(dir);
  ST_CHECK(list.has_value());
  if (!list) return;
  ST_CHECK(list->size() == 2);
}
