// 双宿主一致性 fixture：同一场景（docs/declarative.md 不变式 4）——
// C++（st::ui::dsl）与 JS（DeclarativeHost）各建一遍，产出树的结构/文本必须一致。

#include <memory>
#include <string>
#include <vector>

#include "st/test/test.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/declarative_host.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/ui_root.hpp"

namespace {

class FixtureTextPort final : public st::ui::TextPort {
 public:
  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return st::math::Size{8.0F * static_cast<float>(utf8.size()), size * 1.45F};
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float,
                     st::text::FontRole = st::text::FontRole::Proportional) const
      -> float override {
    return 8.0F * static_cast<float>(utf8.size());
  }
  [[nodiscard]] auto line_height(float size) const -> float override { return size * 1.45F; }
  void draw(st::raster::Surface&, std::string_view, st::math::Point, float, st::math::Color,
            st::text::FontRole = st::text::FontRole::Proportional) const override {}
  [[nodiscard]] auto ellipsize(std::string_view utf8, float, float) const -> std::string override {
    return std::string(utf8);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float, float) const
      -> std::vector<std::string_view> override {
    return {utf8};
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float, float, std::size_t) const
      -> std::vector<std::string> override {
    return {std::string(utf8)};
  }
};

/// 结构性签名：type + 语义文本（不含几何与 id 细节——双宿主的 id 生成路径不同，
/// 语义面才是"同一界面"的判据）。
[[nodiscard]] auto signature(const st::ui::Element& element) -> std::string {
  std::string out = std::string(element.type());
  const std::string text = element.semantics_text();
  if (!text.empty()) {
    out += "[";
    out += text;
    out += "]";
  }
  out += "{";
  for (std::size_t index = 0; index < element.child_count(); ++index) {
    out += signature(*element.child_at(index));
    if (index + 1 < element.child_count()) out += ",";
  }
  out += "}";
  return out;
}

/// C++ 宿主场景：计数 + 条件分支。
struct CppCounter : st::ui::dsl::Component {
  st::ui::dsl::State<int> count{0};
  st::ui::dsl::State<bool> extra{false};

  void build(st::ui::dsl::Composer& c) override {
    using namespace st::ui::dsl;
    column(c, {.gap = 12.0F, .padding = 24.0F}, [&] {
      text(c, [] { return std::string("霜天声明式"); });
      text(c, [&] { return std::format("点击了 {} 次", count.value()); });
      button(c, "+1", [this] { count.set(count.value() + 1); });
      if (extra.value()) {
        text(c, [] { return std::string("额外内容"); });
      }
    });
  }
};

/// JS 宿主场景：同一界面（compose 写法）。
constexpr std::string_view kJsCounter = R"JS(
let count = null;
let extra = null;
compose('Fixture', () => {
  if (count === null) count = useState(0);
  if (extra === null) extra = useState(false);
  const kids = [
    text('霜天声明式'),
    text(() => '点击了 ' + count.value + ' 次'),
    button('+1', () => { count.value = count.value + 1; }),
  ];
  if (extra.value) kids.push(text('额外内容'));
  return column({gap: 12, padding: 24}, kids);
});
)JS";

}  // namespace

ST_TEST(dual_host_mount_signature_matches) {
  FixtureTextPort port;
  // —— C++ 宿主 ——
  st::ui::UiRoot cpp_root;
  cpp_root.set_text_port(&port);
  cpp_root.set_viewport({600.0F, 400.0F});
  auto cpp_host = st::ui::dsl::mount(cpp_root, std::make_shared<CppCounter>());
  ST_REQUIRE(cpp_host != nullptr);
  cpp_root.layout(true);
  const std::string cpp_sig = signature(*cpp_root.content());

  // —— JS 宿主 ——
  st::ui::UiRoot js_root;
  js_root.set_text_port(&port);
  js_root.set_viewport({600.0F, 400.0F});
  js_root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
  js_root.layout(true);
  auto script = std::make_unique<st::ui::ScriptHost>(js_root);
  auto decl = st::ui::DeclarativeHost::attach(*script, js_root);
  ST_REQUIRE(decl != nullptr);
  auto status = decl->run(std::string(kJsCounter), "<fixture>");
  ST_REQUIRE(status.has_value());
  js_root.layout(true);
  const std::string js_sig = signature(*js_root.content());

  ST_CHECK_EQ(js_sig, cpp_sig);
}

ST_TEST(dual_host_state_step_signature_matches) {
  FixtureTextPort port;
  // C++：点 3 次 + 展开
  st::ui::UiRoot cpp_root;
  cpp_root.set_text_port(&port);
  cpp_root.set_viewport({600.0F, 400.0F});
  auto page = std::make_shared<CppCounter>();
  auto cpp_host = st::ui::dsl::mount(cpp_root, page);
  ST_REQUIRE(cpp_host != nullptr);
  cpp_root.layout(true);
  for (int index = 0; index < 3; ++index) {
    st::ui::Element* button = cpp_root.content()->child_at(2);
    (void)st::ui::invoke_element(cpp_root, *button, "click", "");
    (void)cpp_host->tick();
  }
  page->extra.set(true);
  (void)cpp_host->tick();
  cpp_root.layout(true);
  const std::string cpp_sig = signature(*cpp_root.content());

  // JS：点 3 次 + 展开
  st::ui::UiRoot js_root;
  js_root.set_text_port(&port);
  js_root.set_viewport({600.0F, 400.0F});
  js_root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
  js_root.layout(true);
  auto script = std::make_unique<st::ui::ScriptHost>(js_root);
  auto decl = st::ui::DeclarativeHost::attach(*script, js_root);
  ST_REQUIRE(decl != nullptr);
  // 展开由 JS 侧状态控制——加一个开关按钮让两侧都能触发（此处直接操 JS 状态）
  ST_REQUIRE(decl->run(std::string(kJsCounter), "<fixture>").has_value());
  js_root.layout(true);
  for (int index = 0; index < 3; ++index) {
    st::ui::Element* button = js_root.content()->child_at(2);
    (void)st::ui::invoke_element(js_root, *button, "click", "");
    (void)decl->tick();
  }
  // JS 侧展开：直接写状态（等价于 extra.value = true）
  ST_REQUIRE(script->eval("extra.value = true", "<fixture-extra>").has_value());
  (void)decl->tick();
  js_root.layout(true);
  const std::string js_sig = signature(*js_root.content());

  ST_CHECK_EQ(js_sig, cpp_sig);
}
