/// `st lint` 规则测试（`src/pkg/lint.cpp`）。
///
/// 重点覆盖 **L13**（组件遮蔽 `Element` 保护成员 / `semantics_flags` 覆写重建标志）：
/// 这条规则守的是一类**编译零警告、且能被错误的单测掩盖**的缺陷——焦点写基类、
/// 读遮蔽副本时，直接调 `set_focused` 的组件级测试读写落在同一侧，只有真实应用
/// （`UiRoot::set_focus`）才暴露（`CodeEditor` 光标永不绘制即此因）。
/// 规则本身若失灵，这类缺陷会再次静默进来，因此两个方向都要锁住：
/// **命中真实缺陷**（否则规则形同虚设）与**不误报反例**（否则 CI 天天假失败）。
///
/// 其余规则走 `lint_project` 的全量扫描（工程自身 0 违反即是它们的持续验证）。

#include "st/test/test.hpp"

#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/pkg/lint.hpp"
#include "st/pkg/manifest.hpp"

namespace {

/// 把源码写进临时文件并扫描（规则是按行文本判定，不需要真的编译）。
[[nodiscard]] auto lint_source(std::string_view name, std::string_view source)
    -> std::vector<st::pkg::LintViolation> {
  const std::string path = st::fs::join(st::fs::temp_dir(), std::string(name));
  {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << source;
  }
  auto result = st::pkg::lint_file(path);
  (void)st::fs::remove_file(path);
  if (!result) return {};
  return *result;
}

[[nodiscard]] auto has_rule(const std::vector<st::pkg::LintViolation>& violations,
                            std::string_view rule) -> bool {
  for (const auto& violation : violations) {
    if (violation.rule == rule) return true;
  }
  return false;
}

[[nodiscard]] auto count_rule(const std::vector<st::pkg::LintViolation>& violations,
                              std::string_view rule) -> std::size_t {
  std::size_t count = 0;
  for (const auto& violation : violations) {
    if (violation.rule == rule) ++count;
  }
  return count;
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// L13：组件遮蔽 Element 保护成员
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(lint_l13_flags_element_subclass_member_shadowing) {
  // 真实缺陷形态（CodeEditor 曾自带 bool focused_{false}）：
  // Element 子类里声明与基类同名的成员 → 必须命中
  const std::string source = R"(
#include "st/ui/element.hpp"
namespace st::ui {
class ProbeWidget : public Element {
 public:
  ProbeWidget() { set_focusable(true); }

 private:
  bool focused_{false};
  std::string key_{};
};
}  // namespace st::ui
)";
  const auto violations = lint_source("l13_shadow_probe.hpp", source);
  ST_CHECK(has_rule(violations, "L13"));
  ST_CHECK_EQ(count_rule(violations, "L13"), std::size_t{2});  // focused_ 与 key_ 各一处
}

ST_TEST(lint_l13_ignores_non_element_classes_and_plain_method_bodies) {
  // 反面一：不继承 Element 的类声明同名成员是合法的（UiRoot 就有 focused_/hovered_）
  // 反面二：`class Element` 自己不该被当成「Element 子类」（类名里含 Element 而已）
  // 反面三：成员函数体里的 `return children_;` 不是成员声明
  const std::string source = R"(
#include "st/ui/element.hpp"
namespace st::ui {
class NotAnElement {
 private:
  bool focused_{false};
  int bounds_{0};
};
class Element {
 public:
  auto children_count() const -> std::size_t { return children_; }

 private:
  bool focused_{false};
};
}  // namespace st::ui
)";
  const auto violations = lint_source("l13_shadow_negative.hpp", source);
  ST_CHECK(!has_rule(violations, "L13"));
}

ST_TEST(lint_l13_accepts_element_subclass_without_shadowing) {
  // 正确写法：子类只声明自己的成员（名字不与基类重合）
  const std::string source = R"(
#include "st/ui/element.hpp"
namespace st::ui {
class FineWidget : public Element {
 public:
  FineWidget() { set_focusable(true); }

 private:
  std::string label_{};
  int tab_width_{4};
};
}  // namespace st::ui
)";
  const auto violations = lint_source("l13_shadow_fine.hpp", source);
  ST_CHECK(!has_rule(violations, "L13"));
}

// ————————————————————————————————————————————————————————————————————————————
// L13：semantics_flags 覆写重建标志
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(lint_l13_flags_semantics_flags_rebuild_in_both_definition_forms) {
  // 缺陷形态：覆写体内以 `SemanticsFlags flags{}` 起手 → 丢掉基类的
  // visible/enabled/focused/hovered/pressed。类内 inline 定义与类外定义两种都要认出。
  const std::string inline_form = R"(
#include "st/ui/element.hpp"
namespace st::ui {
class ProbeWidget : public Element {
 public:
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override {
    SemanticsFlags flags{};
    flags.editable = true;
    return flags;
  }
};
}  // namespace st::ui
)";
  ST_CHECK(has_rule(lint_source("l13_flags_inline.hpp", inline_form), "L13"));

  const std::string out_of_line = R"(
#include "st/ui/components/basic.hpp"
namespace st::ui {
auto ProbeWidget::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags{};
  flags.editable = false;
  return flags;
}
}  // namespace st::ui
)";
  ST_CHECK(has_rule(lint_source("l13_flags_outline.cpp", out_of_line), "L13"));
}

ST_TEST(lint_l13_accepts_base_result_first_and_unrelated_fresh_flags) {
  // 正确写法：以基类结果起手再覆写特有字段
  const std::string correct = R"(
#include "st/ui/element.hpp"
namespace st::ui {
auto ProbeWidget::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.editable = true;
  return flags;
}
}  // namespace st::ui
)";
  ST_CHECK(!has_rule(lint_source("l13_flags_correct.cpp", correct), "L13"));

  // 反面：覆写体**之外**构造一份全新标志是合法的（如根节点的默认标志）
  const std::string unrelated = R"(
#include "st/ui/element.hpp"
namespace st::ui {
auto make_root_flags() -> SemanticsFlags {
  SemanticsFlags flags{};
  flags.visible = true;
  return flags;
}
}  // namespace st::ui
)";
  ST_CHECK(!has_rule(lint_source("l13_flags_unrelated.cpp", unrelated), "L13"));
}

// ————————————————————————————————————————————————————————————————————————————
// 规则表与豁免
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(lint_rule_catalogue_exposes_every_rule_with_help) {
  const auto rules = st::pkg::known_rules();
  ST_CHECK(!rules.empty());
  bool has_l13 = false;
  for (const auto& [id, description] : rules) {
    ST_CHECK(!id.empty());
    ST_CHECK(!description.empty());
    if (id == "L13") has_l13 = true;
  }
  ST_CHECK(has_l13);
  // 说明文本对「专用检查实现的规则」（pattern 为空）也要给出可读信息，而不是空正则
  const std::string help = st::pkg::explain_rule("L13");
  ST_CHECK(help.find("L13") != std::string::npos);
  ST_CHECK(help.find("专用检查") != std::string::npos);
}

ST_TEST(lint_allow_comment_suppresses_violation) {
  // 行内 `// lint-allow: <规则> 原因` 可豁免（规则表约定）：豁免必须逐条写明原因
  const std::string source = R"(
#include "st/ui/element.hpp"
namespace st::ui {
class ProbeWidget : public Element {
 private:
  bool focused_{false};  // lint-allow: L13 探针：验证豁免通道有效
};
}  // namespace st::ui
)";
  ST_CHECK(!has_rule(lint_source("l13_allow.hpp", source), "L13"));
}

// ── 清单豁免（`lint.exempt`）：工程级边界 ────────────────────────────────────────
//
// 起因（真实应用 dev-tools）：`st lint` 把 `llm.cpp` 里 9 处 `throw std::runtime_error`
// 判成 L5 违规，而那些 throw 是 **worker 线程向主线程传播错误**的合理用法——
// 跨线程边界上，异常是唯一能完整携带"错误在子线程里发生在哪一步"的机制。
// 但 lint 是**文本级**检查，看不到"这行在哪个线程上下文里"。
//
// 修法是让工具承认自己看不出来的地方（把边界写进清单），而不是把业务层的异常
// 硬掰成 `Result`（为迁就工具而降低代码质量）。

/// ① 清单豁免真的生效——**且只在声明的范围内**。
///
/// 本条的重点是后半句：如果豁免"顺手放行了别的文件/别的规则"，它就是一把过宽的钥匙。
ST_TEST(lint_manifest_exemption_scopes_to_declared_paths) {
  using st::pkg::LintExemptions;
  const std::string root = st::fs::join(st::fs::temp_dir(), "st-lint-exempt-probe");
  (void)st::fs::remove_all(root);
  ST_REQUIRE(st::fs::create_directories(st::fs::join(root, "src/app")).has_value());
  ST_REQUIRE(st::fs::create_directories(st::fs::join(root, "src/ui")).has_value());
  // 跨线程错误传播写法的探针（L5：throw）
  const std::string source = R"(
namespace app {
void worker() { throw 42; }
}
)";
  ST_REQUIRE(st::fs::write_text(st::fs::join(root, "src/app/llm.cpp"), source).has_value());
  ST_REQUIRE(st::fs::write_text(st::fs::join(root, "src/ui/other.cpp"), source).has_value());

  // 不豁免：两个文件都报
  {
    auto report = st::pkg::lint_project(root, LintExemptions{});
    ST_REQUIRE(report.has_value());
    std::size_t l5 = 0;
    for (const auto& violation : report->violations) {
      if (violation.rule == "L5") ++l5;
    }
    ST_CHECK_EQ(l5, 2U);
    ST_CHECK_EQ(report->suppressed_by_manifest, 0U);
  }
  // 只豁免 `src/app/**`：业务层那些 throw 不再报，但 `src/ui/other.cpp` **照旧报**
  {
    auto report = st::pkg::lint_project(root, LintExemptions{{"L5", {"src/app/**"}}});
    ST_REQUIRE(report.has_value());
    std::size_t l5 = 0;
    for (const auto& violation : report->violations) {
      if (violation.rule == "L5") ++l5;
    }
    ST_CHECK_EQ(l5, 1U);
    ST_CHECK_EQ(report->suppressed_by_manifest, 1U);   // 豁免计数可见（不静默）
  }
  // 只豁免 L5：同一个文件里的别的规则**不受影响**（豁免是“规则×路径”的交叉，不是“整文件放行”）
  {
    auto report = st::pkg::lint_project(root, LintExemptions{{"L11", {"src/app/**"}}});
    ST_REQUIRE(report.has_value());
    std::size_t l5 = 0;
    for (const auto& violation : report->violations) {
      if (violation.rule == "L5") ++l5;
    }
    ST_CHECK_EQ(l5, 2U);   // 换了规则名 → 一条都不豁免
  }
  (void)st::fs::remove_all(root);
}

/// ② 清单里的 `lint.exempt` 能真的被解析出来（否则上面那条只是“库函数能用”，
/// 而应用写了清单却不生效——那正是用户真正会碰到的失效点）。
ST_TEST(lint_manifest_parses_exempt_section) {
  // 用**带分隔符**的裸字符串（`R"JSON(...)JSON"`）：JSON 里本来就有 `)"` 结尾形状
  // （`  })"` 后面紧跟 `\n` 时会提前终止裸串），普通 `R"(...)"` 会静默截断。
  // 裸串的结束标记必须**紧贴**内容（`)JSON"` 三个记号连续）——把 `)` 与 `JSON"`
  // 拆到两行（中间夹一个换行）会让编译器认为裸串未结束。
  const std::string text = R"JSON({
  "name": "probe",
  "version": "0.1.0",
  "lint": { "exempt": { "L5": ["src/app/*.cpp", "src/net/*.cpp"] } }
}
)JSON";
  auto manifest = st::pkg::Manifest::parse_json(st::Json::parse(text), "/tmp/probe");
  ST_REQUIRE(manifest.has_value());
  const auto found = manifest->lint_exempt.find("L5");
  ST_REQUIRE(found != manifest->lint_exempt.end());
  ST_CHECK_EQ(found->second.size(), 2U);
  ST_CHECK_EQ(found->second[0], std::string("src/app/*.cpp"));
}

/// ③ 清单里的 lint 段写坏（不是对象 / 规则值不是数组）**不能**让清单解析失败。
///
/// 理由是分工：lint 段是辅助配置，报错应当来自代码而不是配置的笔误；
/// 而且**宽松忽略**比报错更安全——报错会让一个只想跑构建的人被 lint 配置卡住。
ST_TEST(lint_manifest_tolerates_malformed_exempt_section) {
  const std::string text = R"JSON({
  "name": "probe",
  "version": "0.1.0",
  "lint": { "exempt": { "L5": "src/app/*.cpp", "L6": [1, 2] } }
}
)JSON";
  auto manifest = st::pkg::Manifest::parse_json(st::Json::parse(text), "/tmp/probe");
  ST_CHECK(manifest.has_value());
  if (manifest.has_value()) ST_CHECK(manifest->lint_exempt.empty());
}

// ————————————————————————————————————————————————————————————————————————————
// L8：可变全局（作用域感知）
//
// 本组用例锁的是一个**真实失效过的判据**：旧实现只判“花括号深度 0”，而本工程
// （以及任何标准做法）把辅助全局放在匿名命名空间里 —— 深度恒 ≥1，于是全部漏网。
// 于是 `src/ui/dsl.cpp` 的 `active_composers`（后被发现无锁遍历）长期“合规”。
// 用例必须同时锁住“匿名命名空间里能抓到”与“不误报”，否则收紧后的规则会天天假失败。
// ————————————————————————————————————————————————————————————————————————————

/// ① 匿名命名空间里的可变全局必须命中（旧实现漏网的那一类）。
ST_TEST(lint_l8_flags_mutable_global_inside_anonymous_namespace) {
  const auto violations = lint_source("l8_anon_ns.cpp", R"CPP(
namespace st::probe {
namespace {
std::unordered_set<int*> registry;
std::mutex registry_mutex;
}  // namespace
}  // namespace st::probe
)CPP");
  ST_CHECK_EQ(count_rule(violations, "L8"), 2U);
}

/// ② 具名命名空间（含嵌套）里的同名全局同样命中。
ST_TEST(lint_l8_flags_mutable_global_in_named_namespace) {
  const auto violations = lint_source("l8_named_ns.cpp", R"CPP(
namespace st::probe {
namespace detail {
int counter = 0;
}
}
)CPP");
  ST_CHECK_EQ(count_rule(violations, "L8"), 1U);
}

/// ③ 函数内局部 `static` 命中（它同样是进程生存期共享状态）。
/// 真实原型：进程级剪贴板 `static std::string clipboard;`、全局语言注册表。
ST_TEST(lint_l8_flags_function_local_static) {
  const auto violations = lint_source("l8_fn_static.cpp", R"CPP(
std::string& clipboard() {
  static std::string store;
  return store;
}
)CPP");
  ST_CHECK_EQ(count_rule(violations, "L8"), 1U);
}

/// ④ 反例必须干净：不能误报函数签名续行、类成员、不可变全局，
/// 以及字符串字面量里的大括号（按原文计数会让作用域栈错位，
/// 实测会把类成员判成全局——markdown 解析器里满是大括号字面量）。
ST_TEST(lint_l8_ignores_signatures_members_and_immutables) {
  const auto violations = lint_source("l8_clean.cpp", R"CPP(
namespace st::probe {

constexpr int kLimit = 8;
const std::string kName = "probe";
static const char* const kGlyphs = "{}[]";

class Widget {
 public:
  std::string text_{};
  int count_{0};
 private:
  std::string buffer_{};
};

void fill(int size, float radius = 0.0f) {
  if (radius <= 0.0f) return;
  (void)size;
}

}  // namespace st::probe
)CPP");
  ST_CHECK_EQ(count_rule(violations, "L8"), 0U);
}

/// ⑤ 行内豁免：写在命名空间开括号**之前**时开启作用域窗口（既有惯例，
/// `log.cpp`/`test_runner.cpp` 靠它整块登记）；写在声明行上则是单点豁免。
ST_TEST(lint_l8_prefix_allow_comment_covers_scope) {
  const auto block_form = lint_source("l8_allow_block.cpp", R"CPP(
// lint-allow: L8 日志为进程级基础设施（见 CONVENTIONS §8 登记）
namespace st::probe {
namespace {
std::string sink;
std::mutex sink_mutex;
}  // namespace
}  // namespace st::probe
)CPP");
  ST_CHECK_EQ(count_rule(block_form, "L8"), 0U);

  const auto single_form = lint_source("l8_allow_single.cpp", R"CPP(
std::string g_sink;  // lint-allow: L8 进程级日志 sink
std::mutex g_other;
)CPP");
  ST_CHECK_EQ(count_rule(single_form, "L8"), 1U);   // 只豁免本行，下一行的真全局仍命中
}

// ————————————————————————————————————————————————————————————————————————————
// L14：系统头 / 平台 API 单点封装（§10 第 1 条）
//
// 本规则补的是一个“文档写了、实现没写”的缺口（§8 一直声称检查禁用 include）。
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(lint_l14_flags_system_headers_and_dlopen) {
  const auto violations = lint_source("l14_headers.cpp", R"CPP(
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <dlfcn.h>

#include "st/ext/json.hpp"
)CPP");
  ST_CHECK_EQ(count_rule(violations, "L14"), 3U);
}

ST_TEST(lint_l14_flags_direct_dlopen_calls) {
  const auto violations = lint_source("l14_dlopen.cpp", R"CPP(
void* handle = dlopen("libx.so", 2);
void* symbol = dlsym(handle, "init");
)CPP");
  ST_CHECK_EQ(count_rule(violations, "L14"), 2U);
}

/// `platform_*` 是系统头的合法落脚点（单点封装）；`backend.cpp` 因运行时后端探测获准。
ST_TEST(lint_l14_exempts_platform_and_backend_files) {
  const auto platform = lint_source("platform_probe.cpp", "#include <windows.h>\n");
  ST_CHECK_EQ(count_rule(platform, "L14"), 0U);

  const auto backend = lint_source("backend.cpp", "#include <dlfcn.h>\n");
  ST_CHECK_EQ(count_rule(backend, "L14"), 0U);

  // 普通文件不获准（豁免不能泛化）。
  const auto plain = lint_source("ordinary.cpp", "#include <windows.h>\n");
  ST_CHECK_EQ(count_rule(plain, "L14"), 1U);
}

