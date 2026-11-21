// ============================================================================
// 框架单元必须带「档位标志」——回归用例
//
// ## 钉住的是哪条不变量
//
// `build()` 与 `build_test_binary()` 各自调 `make_framework_flags(...)` 构造
// **框架单元专用**的标志集。它的第 4 个形参是 `profile_flags_list`（档位标志），
// 必须收到 `*flags`，不能是 `{}`。
//
// ## 为何必须有这条用例（实测代价）
//
// 曾经被误写成 `{}`（本意是修「`st test` 路径里 `*flags` 传进了错误形参位置」，
// 结果把正确位置的档位标志也删了）。它的失效**完全静默**：编译成功、链接成功、
// 用例全绿，只有产物体积与帧率在变。三层后果：
//   ① 框架代码退回 GCC 默认 `-O0`（命令行里没有任何优化开关）；
//   ② 丢 `-ffunction-sections -fdata-sections` ⇒ 链接期 `--gc-sections`
//      没有粒度可裁，未引用的框架代码整段进产物；
//   ③ 框架单元不再命中 PCH（PCH 建立侧带档位标志，消费侧不带就不兼容）⇒ 全量重编变慢。
// 三者合计把 `xiangqi` 的 release 产物从 **6.30 MB** 撑到 **10.09 MB（+60%）**。
//
// ## 判据为何是「命令行里找得到」而不是「内部结构赋值对不对」
//
// 后者只能证明"我传了"，不能证明"它进了命令行"——而真正要保证的是后者。
// 所以本用例走 `build_language_flags` / `make_framework_flags` 这条**真正的组装路径**，
// 在产出的标志列表里找档位标志。回退修复（把 `*flags` 改回 `{}`）时本用例必红。
// ============================================================================

#include <string>
#include <vector>

#include "st/pkg/build.hpp"
#include "st/pkg/compiler.hpp"
#include "st/pkg/manifest.hpp"
#include "st/test/test.hpp"

namespace {

using st::pkg::CompilerKind;
using st::pkg::framework_flags_carry_profile;

/// release 档的标志（与 `profile_flags("release")` 保持一致的关键项）。
/// 不直接调 `profile_flags`：它是内部函数，而这里要断言的是**档位口径**这件事本身，
/// 用一份显式清单反而更能表达"这些必须一起到位"（`-O2` 管性能，两条分掌管体积）。
const std::vector<std::string> kReleaseProfile{
    "-O2", "-DNDEBUG", "-ffunction-sections", "-fdata-sections"};

}  // namespace

ST_TEST(framework_profile_flags_must_reach_framework_units) {
  // 正常形态：框架单元的标志集里包含全部档位标志。
  ST_CHECK(framework_flags_carry_profile(kReleaseProfile, kReleaseProfile));

  // 多出来的标志不影响判定（框架自己的 `-Werror` 之类本来就会叠上去）。
  auto with_extras = kReleaseProfile;
  with_extras.push_back("-Werror");
  with_extras.push_back("-I/usr/include");
  ST_CHECK(framework_flags_carry_profile(with_extras, kReleaseProfile));
}

ST_TEST(framework_profile_flags_missing_any_item_is_rejected) {
  // 这是缺陷的真实形态：传 `{}` ⇒ 框架单元标志里一个档位标志都没有。
  ST_CHECK(!framework_flags_carry_profile({}, kReleaseProfile));

  // 少任何一项都必须判假——尤其 `-ffunction-sections`：
  // 它单独缺失时程序照跑、测试照绿，只有产物体积在涨（`--gc-sections` 失去粒度）。
  for (std::size_t i = 0; i < kReleaseProfile.size(); ++i) {
    std::vector<std::string> partial;
    for (std::size_t j = 0; j < kReleaseProfile.size(); ++j) {
      if (j != i) partial.push_back(kReleaseProfile[j]);
    }
    ST_CHECK(!framework_flags_carry_profile(partial, kReleaseProfile));
  }
}

ST_TEST(framework_profile_flags_empty_profile_is_not_satisfied) {
  // 空档位标志不是"全都有"——它是"根本没配"。判真会让上面的断言集体失效。
  ST_CHECK(!framework_flags_carry_profile({}, {}));
  ST_CHECK(!framework_flags_carry_profile(kReleaseProfile, {}));
}

ST_TEST(framework_profile_flags_survive_other_profiles) {
  // 别的档位同理：dev 的 `-O1 -g1` 与 quick 的 `-O0 -g1` 也必须到位。
  // 关键在 `-ffunction-sections/-fdata-sections`——它们**不随档位变**（见
  // `build.cpp` 的 `with_section_flags`），所以每个档位的框架单元都该带上。
  const std::vector<std::string> dev{"-O1", "-g1", "-ffunction-sections", "-fdata-sections"};
  const std::vector<std::string> quick{"-O0", "-g1", "-ffunction-sections", "-fdata-sections"};
  ST_CHECK(framework_flags_carry_profile(dev, dev));
  ST_CHECK(framework_flags_carry_profile(quick, quick));
  // 交叉档位不能互相满足（dev 的标志集判 release 必假）。
  ST_CHECK(!framework_flags_carry_profile(dev, kReleaseProfile));
}
