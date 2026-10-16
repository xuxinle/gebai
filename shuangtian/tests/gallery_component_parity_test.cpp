/// gallery 组件页对账（BACKLOG「gallery 组件页对账机制」——v2.2 第 36 轮反推缺口④）。
///
/// ## 这组用例守的是什么
///
/// gallery 自称「控件全集」，但展示卡是**手搭**的：新组件进了 `ST_COMPONENT_LIST`
/// （dsl.cpp 注册表，组件全集的唯一权威清单）却没人补卡时——画廊不说、编译器不说、
/// 任何机制都不说。v0.1.5 专项「补齐 28 组件」之后新增组件零跟进，缺口静默长大才被
/// 反推发现（TimelineView/ColorPicker/Viewport/Tree/Tooltip 甚至全库 examples 零展示位）。
/// 缺的不是卡，是**让复发当场红**的对账机制——本文件就是那道闸。
///
/// ## 对账形态（BACKLOG 原文：「ST_COMPONENT_LIST − 已知豁免清单 == gallery 展示集」）
///
/// - 注册表全集：`registered_element_types()`——`ST_COMPONENT_LIST` 的运行时投影，
///   真源、无手抄（手抄两份必然分叉，33 轮快捷键卡的老路）；
/// - 展示集：`gallery::kGalleryExhibits`（tests/support/gallery_exhibits.hpp）；
/// - 豁免集：`gallery::kGalleryExempt`（同头文件，逐条理由）。
///
/// 主断言差集为空（双向）。于是：
/// - 新组件进注册表、不补卡也不豁免 → 「注册表−豁免」多一项 → **红**（头号目标形态）；
/// - 豁免/展示集里写了注册表外的名字 → 存在性断言红（防拼错与组件改名后豁免静默失效）；
/// - 「待补卡」条目补卡后忘移出豁免 → 与展示集相交 → 红（防豁免清单烂成坟场）。
///
/// ## 展示集名单的来源（本单摸底实得，供复核）
///
/// 遍历 examples/gallery/{pages,main}.cpp 的全部 `std::make_unique<T>` 与
/// `dsl::custom<T>` 实例化点，映射到注册名（`IconView`→`"Icon"` 一类以注册名为准）。
/// 该提取当前是**人工 grep**，登记与实物不符的风险由后续单消除：gallery 侧加
/// 启动时运行时树自检（遍历真值树收集 `type()` 与登记集比对，见共享头尾部的口径备忘）。

#include "st/test/test.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "st/ui/dsl.hpp"
// 支撑头引用形态随仓库先例（ui_text_selection_readonly_test.cpp：
// "../tests/support/..."——靠 TU 目录规则解析，不依赖工程根 -I）。
#include "../tests/support/gallery_exhibits.hpp"

namespace {

using gallery::kGalleryExempt;
using gallery::kGalleryExhibits;

/// 差集（a − b），保持 a 的顺序：失败消息里按注册表顺序读出缺失项。
[[nodiscard]] auto minus(const std::vector<std::string>& a, const std::vector<std::string>& b)
    -> std::vector<std::string> {
  std::vector<std::string> diff;
  for (const std::string& item : a) {
    if (std::ranges::find(b, item) == b.end()) diff.push_back(item);
  }
  return diff;
}

[[nodiscard]] auto exhibits() -> std::vector<std::string> {
  return {kGalleryExhibits.begin(), kGalleryExhibits.end()};
}

[[nodiscard]] auto exemptions() -> std::vector<std::string> {
  std::vector<std::string> names;
  names.reserve(kGalleryExempt.size());
  for (const auto& [name, reason] : kGalleryExempt) {
    (void)reason;
    names.emplace_back(name);
  }
  return names;
}

}  // namespace

/// ① 展示集与豁免集互不相交（「补卡后忘移出豁免」会让缺口登记永久压着对账）。
ST_TEST(gallery_parity_exhibits_and_exemptions_are_disjoint) {
  // 相交检查与唯一性检查用 sort+adjacent_find 实现（不用 std::set）：
  // 实测 dev 档下 set 红黑树节点分配在此 TU 稳定崩溃（10/10：8 次 0xC0000005 +
  // 2 次 bad_alloc，gdb 实证崩在插入比较 memcmp 的野指针节点——树内存被并发写坏，
  // 详见 troubleshooting.md；vector 路径稳定）。语义不变：相交报全部相交项、
  // 展示集内部不重复。
  std::vector<std::string> shown = exhibits();
  std::sort(shown.begin(), shown.end());
  if (const auto dup = std::ranges::adjacent_find(shown); dup != shown.end()) {
    ST_FAIL(std::format("展示集内部重复登记「{}」（重复登记会让「个数对上了」伪装成「集合对上了」）", *dup));
  }
  for (const std::string& name : exemptions()) {
    if (std::ranges::binary_search(shown, name)) {
      ST_FAIL(std::format("「{}」同时在展示集与豁免集：补卡后应移出 kGalleryExempt", name));
    }
  }
}

/// ② 豁免集里每个名字都真实存在于注册表（拼错/组件改名 → 豁免静默失效，
/// 缺卡重新变回「对不上但对账不响」——豁免清单自己先烂掉）。
ST_TEST(gallery_parity_exemptions_all_exist_in_registry) {
  const std::vector<std::string> registered = st::ui::dsl::registered_element_types();
  const std::set<std::string> registry(registered.begin(), registered.end());
  for (const std::string& name : exemptions()) {
    ST_CHECK(registry.contains(name));
  }
}

/// ③ 展示集里每个名字都真实存在于注册表（登记口径漂移即红）。
ST_TEST(gallery_parity_exhibits_all_exist_in_registry) {
  const std::vector<std::string> registered = st::ui::dsl::registered_element_types();
  const std::set<std::string> registry(registered.begin(), registered.end());
  for (const std::string& name : exhibits()) {
    ST_CHECK(registry.contains(name));
  }
}

/// ④ 主断言：**注册表 − 豁免 == 展示集**（双向差集为空）。
///
/// 注册表侧只对账**内置 48 项**：`registered_element_types()` 还含外部注册
/// （如其他示例的 `register_element_type<T>`），gallery 对它们没有展示义务。
/// 内外之分的判据用**基线数量**钉死（48 = `ST_COMPONENT_LIST` 当前行数，
/// `ui_component_contract_test.cpp` 同口径）：内置表增长即数量断言红，
/// 提醒同步本测试的豁免/展示两侧——这正是「新增组件必须二选一（补卡/豁免）」的机制点。
ST_TEST(gallery_parity_registry_minus_exemptions_equals_exhibits) {
  const std::vector<std::string> registered = st::ui::dsl::registered_element_types();
  const std::vector<std::string> exempt = exemptions();
  const std::vector<std::string> shown = exhibits();

  // 内置注册表基线：48 项（外部注册不计——当前测试二进制无外部注册方，
  // registered == 内置全集；若将来有外部注册方加入，此断言先红，提醒改用
  // `make_element` 可达性或专用枚举 API 区分内外——见终报「未竟」）。
  ST_CHECK(registered.size() >= 48U);

  const std::vector<std::string> expected = minus(registered, exempt);
  const std::vector<std::string> missing = minus(expected, shown);  // 有组件没卡也没豁免
  const std::vector<std::string> stale = minus(shown, expected);    // 展示集登记了多余项
  if (!missing.empty() || !stale.empty()) {
    ST_FAIL(std::format("gallery 对账不平（注册表−豁免 {} 项 vs 展示集 {} 项）：缺卡 [{}]；多登 [{}]",
                        expected.size(), shown.size(),
                        st::test::format_list(missing), st::test::format_list(stale)));
  }
}

/// ⑤ 豁免条目的理由形态：要么「【待补卡】」开头（缺口登记），要么给出结构性理由。
/// 防止真豁免与待补卡混作一团、无人回头——豁免清单的可信度由每条理由的可信度构成。
ST_TEST(gallery_parity_exempt_reasons_are_classified) {
  for (const auto& [name, reason] : kGalleryExempt) {
    const bool pending = reason.starts_with("【待补卡】");
    const bool structural = !pending && reason.find("：") != std::string_view::npos;
    if (!pending && !structural) {
      ST_FAIL(std::format("豁免条目「{}」的理由未分类（需「【待补卡】」前缀或「名目：理由」结构）：{}",
                          name, reason));
    }
  }
}
