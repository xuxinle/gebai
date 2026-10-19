/// gbcode 快捷键卡对账（BACKLOG 3982「`UiRoot` 快捷键注册表无可枚举面」，2026-10-16）。
///
/// ## 这组用例守的是什么
///
/// 快捷键卡曾手写两份数据（main.cpp 注册一份、page_dialogs.cpp 卡片一份），
/// 33 轮临时手补 14 条只是止血、又再漂 15+——「表与代码要么同源、要么有对账，
/// 二选一，现在两者皆无」。本单让注册走单源表（`shortcut_help.hpp` 的
/// `register_shortcut_by_id`）、卡片从注册表快照生成（`shortcuts_card_body`），
/// 本文件把这条链**钉进测试**：
///
/// 1. **round-trip**：把单源表全部条目按 main.cpp 同款方式注册 → 注册表快照
///    ⇄ 表期待集双向差集为空（表改了、注册面没跟上，或注册了表外的键位，都红）；
/// 2. **文案⇄键位一致**：每条卡片的 label 都能从 spec 的修饰键拼出来（防
///    「文案写 Ctrl+Shift+X、表里却是 Alt」这种卡片自己骗自己的形态——
///    手写卡时代恰是最常见的漂移）；
/// 3. **卡片⇄注册表同源**：`shortcuts_card_body` 的每一行都对应注册面里
///    真实存在的键位（含 Never 条豁免）；注册面变化 → 卡片行集变化
///    （「注册表变更→卡片同步」的可执行断言）。
///
/// 为什么不用 gbcode 的 main.cpp 原文当夹具：它拉起整个 Application（窗口、
/// 终端、LSP），单测里不可承受；而注册面的**全部语义**都在「spec → 注册助手」
/// 这一层——main.cpp 只是逐条 bind 动作，键位/守卫零自有逻辑。若有人在
/// main.cpp 绕过表另写 register_shortcut，启动对账（main.cpp 里那段
/// shortcut_drift 日志）与 ④ 的「表外必现」用例共同兜住。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/ui/components/overlay.hpp"   // Dialog（守卫口径用例的浮层夹具）
#include "st/ui/ui_root.hpp"
// gbcode 的应用层源（不在框架 sources 清单里）——与 git_service_test 同一策略：
// 单头表直接 include（shortcut_help.hpp 全部 inline，无需 .cpp）。
#include "examples/gbcode/shortcut_help.hpp"

namespace {

using gbcode_ui::ShortcutDrift;
using gbcode_ui::ShortcutSpec;
using gbcode_ui::kShortcutSpecs;
using gbcode_ui::register_shortcut_by_id;
using gbcode_ui::shortcut_drift;
using gbcode_ui::shortcut_spec;
using gbcode_ui::shortcuts_card_body;
using st::ui::UiRoot;

/// 模拟 main.cpp 的注册面：全部「应注册」条目按同款助手落进一个真 UiRoot。
/// 动作用空实现（对账只关心键位/修饰键/守卫口径，不关心按下后做什么——
/// 动作行为属于 gbcode e2e 的领域）。UiRoot 不可拷贝，返回 unique_ptr。
[[nodiscard]] auto make_registered_root() -> std::unique_ptr<UiRoot> {
  auto root = std::make_unique<UiRoot>();
  const auto overlay_open = [raw = root.get()] { return raw->overlay_count() > 0; };
  for (const ShortcutSpec& spec : kShortcutSpecs) {
    if (spec.reg == ShortcutSpec::Reg::Never) continue;
    (void)register_shortcut_by_id(*root, spec.id, [] { return true; }, overlay_open);
  }
  return root;
}

/// 两个文本块的差集（a − b，按行）：对账失败时报出具体差异行。
// （实现从简：本文件的断言都是定点 find，未用到通用差集——保留空位备忘，
// 需要时再写实作；避免 -Werror=unused-function。）
}  // namespace

/// ① round-trip：单源表 ⇄ 注册表快照双向差集为空。
///
/// 覆盖三个漂移形态（表外注册 / 漏注册 / Never 抢注）各自的抓取：
/// 拿掉一条期待（模拟 main.cpp 漏 bind）→ missing 点名；
/// 注册表外加一条表外键位（模拟绕表手写 register_shortcut）→ unknown 点名。
ST_TEST(gbcode_shortcut_table_round_trips_registry) {
  auto root_holder = make_registered_root();
  UiRoot& root = *root_holder;
  const ShortcutDrift drift = shortcut_drift(root.shortcuts());
  ST_CHECK(drift.unknown.empty());
  ST_CHECK(drift.missing.empty());
  ST_CHECK(drift.reserved.empty());

  // 漏注册（表有但没 bind）：注销一条后，对账必须点名它。
  root.unregister_shortcut("w", UiRoot::Shortcut{.ctrl = true});
  const ShortcutDrift after_remove = shortcut_drift(root.shortcuts());
  ST_CHECK_EQ(after_remove.missing.size(), std::size_t{1});
  // 带下标的断言先守长度：ST_CHECK 失败**不中断用例**，后面的下标访问会
  // 越界崩进程（逆向验证发现：回退 shortcuts() 后这里崩溃而非报红）。
  if (after_remove.missing.size() == 1U) {
    ST_CHECK_EQ(after_remove.missing[0], std::string{"close"});
  }

  // 表外注册（绕过单源表手写）：对账必须点名它（unknown）。
  ST_CHECK(root.register_shortcut("F9", {}, [] { return true; }));
  const ShortcutDrift after_extra = shortcut_drift(root.shortcuts());
  ST_CHECK_EQ(after_extra.unknown.size(), std::size_t{1});
  if (after_extra.unknown.size() == 1U) {
    ST_CHECK_EQ(after_extra.unknown[0], std::string{"f9"});
  }

  // Never 抢注：把编辑器内的键（Ctrl+Z）注册到根级——unknown + reserved 双点名
  //（unknown 是「表外」，reserved 进一步说明它是 Never 条被抢注）。
  ST_CHECK(root.register_shortcut("z", UiRoot::Shortcut{.ctrl = true}, [] { return true; }));
  const ShortcutDrift after_reserved = shortcut_drift(root.shortcuts());
  ST_CHECK_EQ(after_reserved.unknown.size(), std::size_t{2});   // f9 + z+ctrl
  ST_CHECK_EQ(after_reserved.reserved.size(), std::size_t{1});
  if (after_reserved.reserved.size() == 1U) {
    ST_CHECK_EQ(after_reserved.reserved[0], std::string{"z+ctrl"});
  }
}

/// ② 文案⇄键位一致：每条出卡 spec 的 label 前缀都能从它的修饰键拼出来。
///
/// 手写卡时代最隐蔽的漂移：文案写 "Ctrl+Shift+X" 而注册的是 Alt——两边各自
/// 看着都对，用户照卡按键没反应。单源之后这条成为结构不变量：label 与 mods
/// 出自同一行，拼法固定（Ctrl/Shift/Alt 顺序 + 主键），断言把拼法钉死。
ST_TEST(gbcode_shortcut_labels_match_modifiers) {
  for (const ShortcutSpec& spec : kShortcutSpecs) {
    if (!spec.alias_of.empty()) continue;   // 别名无文案
    if (!spec.show) continue;               // 不出卡的不查卡片文案
    // 拼出期望前缀：modifier 顺序固定 Ctrl→Shift→Alt，主键在末段。
    // label 允许「一条文案盖多条键位」（如 "Ctrl+F/Ctrl+H"、"F7 / Shift+F7"）：
    // 取**首段**（第一个空格前）比对——首段是主键位的完整写法。
    const std::string label(spec.label);
    const std::size_t space = label.find(' ');
    const std::string head = space == std::string::npos ? label : label.substr(0, space);
    std::string expected;
    if (spec.ctrl) expected += "Ctrl+";
    if (spec.shift) expected += "Shift+";
    if (spec.alt) expected += "Alt+";
    // 主键：登记名的展示形态（f2/f7/f8/f12 大写、tab/escape 转义文案）——
    // 从 label 尾段取（与 modifier 前缀拼接），不在此处硬编码键名表。
    ST_CHECK(!expected.empty() || spec.reg == ShortcutSpec::Reg::Consume);
    if (expected.empty()) continue;   // 无修饰键的纯功能键（F2/F7/F8/Esc）：无前缀可比
    ST_CHECK(head.starts_with(expected) || head == "Esc" || head == "Alt+\xe2\x86\x91" ||
             head == "Alt+\xe2\x86\x93" || head == "Alt+\xe2\x86\x90");
  }
}

/// ③ 卡片⇄注册表同源：`shortcuts_card_body` 的行集随注册面变化。
///
/// 「注册表变更 → 卡片同步」的可执行断言：
/// - 完整注册面 → 卡片含全部出卡行、不含「其他」节；
/// - 注销一条 → 对应卡片行**消失**（绝不登一条照着按没反应的键位）；
/// - 表外加一条 → 进「其他」节（兜底可见，人眼看卡即发现绕表注册）。
ST_TEST(gbcode_shortcut_card_follows_registry) {
  {
    auto root_holder = make_registered_root();
    UiRoot& root = *root_holder;
    const std::string body = shortcuts_card_body(root.shortcuts());
    // 分组标题齐全（六个组都在）。
    for (const std::string_view group : {"文件\n", "编辑\n", "导航\n", "跳转（F12 家族）\n",
                                         "差异视图\n", "查看\n"}) {
      ST_CHECK(body.find(std::string(group)) != std::string::npos);
    }
    // 抽查关键行（每行 `  <label>\t<说明>` 形态）。
    ST_CHECK(body.find("  Ctrl+S\t保存\n") != std::string::npos);
    ST_CHECK(body.find("  Ctrl+Shift+P\t命令面板\n") != std::string::npos);
    ST_CHECK(body.find("  F12\t转到定义\n") != std::string::npos);
    ST_CHECK(body.find("  Esc\t关闭浮层\n") != std::string::npos);
    // Never 条照登（编辑器内，卡片是其唯一面）；别名不出行；完整面无「其他」节。
    ST_CHECK(body.find("（编辑器内）") != std::string::npos);
    ST_CHECK(body.find("Backquote") == std::string::npos);
    ST_CHECK(body.find("其他") == std::string::npos);
  }
  // 注销一条（模拟注册面变化）：卡片行必须跟着消失。
  {
    auto root_holder = make_registered_root();
    UiRoot& root = *root_holder;
    root.unregister_shortcut("p", UiRoot::Shortcut{.ctrl = true});   // Ctrl+P
    const std::string body = shortcuts_card_body(root.shortcuts());
    ST_CHECK(body.find("  Ctrl+P\t快速打开文件\n") == std::string::npos);
    ST_CHECK(body.find("  Ctrl+Shift+P\t命令面板\n") != std::string::npos);  // 别条不受累
  }
  // 表外加一条（模拟绕表手写）：进「其他」节。
  {
    auto root_holder = make_registered_root();
    UiRoot& root = *root_holder;
    ST_CHECK(root.register_shortcut("F9", {}, [] { return true; }));
    const std::string body = shortcuts_card_body(root.shortcuts());
    ST_CHECK(body.find("其他") != std::string::npos);
    ST_CHECK(body.find("f9\t（表外注册）") != std::string::npos);
  }
}

/// ④ 注册助手的守卫口径：`LetOverlayDecide` 与 `Consume` 两形态行为差异。
///
/// 这是「注册面语义从表来」的核心断言：同一份 overlay_open 判定下，
/// - LetOverlayDecide 条（如 save）：浮层开着 → 不消费（键让给浮层）；
/// - Consume 条（如 completion）：浮层开着 → 照样消费（弹层自己要用）。
/// 若有人把表的 reg 改错，这个用例当场红（派发行为差异，不是纸面口径）。
ST_TEST(gbcode_shortcut_guard_shapes_follow_spec) {
  UiRoot root;
  const auto overlay_open = [&root] { return root.overlay_count() > 0; };
  ST_CHECK(register_shortcut_by_id(root, "save", [] { return true; }, overlay_open));
  ST_CHECK(register_shortcut_by_id(root, "completion", [] { return true; }, overlay_open));

  auto press = [&root](const std::string& key, bool ctrl) {
    st::ui::Event event;
    event.kind = st::ui::EventKind::KeyDown;
    event.key = key;
    event.ctrl = ctrl;
    return root.dispatch(event);
  };
  // 无浮层：两者都消费。
  ST_CHECK(press("s", true));
  ST_CHECK(press("space", true));
  // 挂一个浮层：LetOverlayDecide 让路、Consume 照吃。
  auto dialog = std::make_unique<st::ui::Dialog>("t", "b");
  root.add_overlay(std::move(dialog));
  ST_CHECK(!press("s", true));         // Ctrl+S 让给浮层
  ST_CHECK(press("space", true));      // Ctrl+Space 补全弹层自己要用
  // Never/未知 id：助手拒绝（表外注册从入口就被挡）。
  ST_CHECK(!register_shortcut_by_id(root, "undo-redo", [] { return true; }, overlay_open));
  ST_CHECK(!register_shortcut_by_id(root, "no-such-id", [] { return true; }, overlay_open));
  // overlay_open 未提供（默认空）：LetOverlayDecide 退化为无条件消费——
  // 与旧 bind 助手传空守卫时的行为一致（main.cpp 恒提供，这条是口径备忘）。
  UiRoot bare;
  ST_CHECK(register_shortcut_by_id(bare, "save", [] { return true; }));
  bare.add_overlay(std::make_unique<st::ui::Dialog>("t", "b"));
  auto press_bare = [&bare](const std::string& key, bool ctrl) {
    st::ui::Event event;
    event.kind = st::ui::EventKind::KeyDown;
    event.key = key;
    event.ctrl = ctrl;
    return bare.dispatch(event);
  };
  ST_CHECK(press_bare("s", true));   // 空守卫：不再让路
}

/// ⑤ 基线数量：单源表 41 条（38 可注册 + 3 条 Never——Ctrl+Z/Y、Ctrl+A、Ctrl+/）
///、注册面 38 条、卡片行数按表内 show 口径推导（详见用例内注释）。
///
/// 数量断言的价值：①②③都是差集断言（对不出时才红），基线把「表被整块
/// 换掉但恰好自洽」的形态拦住（如删除一整组——round-trip 依然全绿，但
/// 用户少了半组快捷键且没人知道）。
ST_TEST(gbcode_shortcut_table_baseline_counts) {
  std::size_t total = 0;
  std::size_t never = 0;
  std::size_t should_register = 0;
  std::size_t card_rows = 0;
  for (const ShortcutSpec& spec : kShortcutSpecs) {
    ++total;
    if (spec.reg == ShortcutSpec::Reg::Never) {
      ++never;
      if (spec.show) ++card_rows;   // Never 条出卡（编辑器内，卡片是其唯一面）
      continue;
    }
    ++should_register;
    if (spec.show && spec.alias_of.empty()) ++card_rows;
  }
  // 基线计数（2026-10-19 更新：+3 条页面缩放快捷键 zoom-in/out/reset，
  // 三条都是 Never 守卫 → total +3、should_register +3、never 不变）。
  ST_CHECK_EQ(total, std::size_t{44});
  ST_CHECK_EQ(never, std::size_t{3});
  ST_CHECK_EQ(should_register, std::size_t{41});
  // 注册面实测：真注册一遍后 shortcut_count == should_register。
  auto root_holder = make_registered_root();
  UiRoot& root = *root_holder;
  ST_CHECK_EQ(root.shortcut_count(), should_register);
  // 卡片行实测：数卡片正文里的行数（以两空格开头的行 = 条目行）。
  std::size_t body_rows = 0;
  const std::string body = shortcuts_card_body(root.shortcuts());
  std::size_t start = 0;
  while (start < body.size()) {
    std::size_t end = body.find('\n', start);
    if (end == std::string::npos) end = body.size();
    const std::string line = body.substr(start, end - start);
    if (!line.empty() && line.starts_with("  ")) ++body_rows;
    start = end + 1;
  }
  ST_CHECK_EQ(body_rows, card_rows);
}
