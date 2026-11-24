/// **页面底渐变随主题**（`Style::use_theme_backdrop`）+ **`glass` 字符串配置**。
///
/// 两件事都必须“结构上跟随主题”，而不能靠应用在 `build()` 里写死颜色：
/// 玻璃/半透明层的通透感建立在“背后有层次”上，而层次的颜色必须与亮/暗主题
/// **同时**成立；`build()` 在换肤时不会重跑，写了也拿不到新主题。
///
/// 同理，玻璃参数走**一条字符串**（`glass="blur:8,tint:0.22,…"`）而不是四个字段：
/// 漏写一个就静默半开（只给 blur 忘了 tint → “模糊但实心”），而字符串是
/// 一条声明，XML / 控制通道 / C++ 声明式三处共用同一解析器。
#include <memory>
#include <string>

#include "st/test/test.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace math = st::math;
using namespace st::ui;
using namespace st::ui::dsl;

namespace {

struct BackdropPage : Component {
  void build(Composer& c) override {
    (void)column(c, {.grow = true, .id = "backdrop", .key = "backdrop"}, [&] {
      (void)text(c, [] { return std::string("内容"); });
    });
  }
};

}  // namespace

ST_TEST(backdrop_gradient_follows_theme) {
  // **核心 ①**：同一个元素，换主题后底渐变必须变成新主题那一套。
  //
  // 判据用**色标**而不是“有没有渐变”：两套主题都定义了渐变，
  // 只判 `has_value()` 会在“换肤了但没重算”时恒绿——那正是要防的缺陷。
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  root.set_theme(Theme::light());
  auto page = std::make_shared<BackdropPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  Element* backdrop = root.find("backdrop");
  ST_REQUIRE(backdrop != nullptr);
  backdrop->style().use_theme_backdrop = true;
  // `set_theme` 只**标脏**，真正下发在 `layout()`（`apply_theme_tree`）。
  // 所以两步都要有：只 set 不 layout = 读到的还是旧样式（用例会假红/假绿）。
  root.set_theme(Theme::light());
  root.layout(true);

  const auto& light_gradient = backdrop->style().background_gradient;
  ST_REQUIRE(light_gradient != nullptr);
  ST_REQUIRE(light_gradient->stops().size() >= 2);
  const math::Color light_first = light_gradient->stops().front().color;

  // 换成暗色主题：必须重算成暗色那一套。
  root.set_theme(Theme::dark());
  root.layout(true);
  const auto& dark_gradient = backdrop->style().background_gradient;
  ST_REQUIRE(dark_gradient != nullptr);
  const math::Color dark_first = dark_gradient->stops().front().color;

  // 亮色首标（#FCF1E2）与暗色首标（#1A1712）**亮度相差极大**——
  // 用“暗色明显更暗”做判据，而不是逐通道比相等（换档位时不该红）。
  const int light_luma = light_first.r + light_first.g + light_first.b;
  const int dark_luma = dark_first.r + dark_first.g + dark_first.b;
  ST_CHECK(light_luma > dark_luma + 200);
}

ST_TEST(theme_glass_switches_fill_and_tint_with_mode) {
  // **核心 ③**：玻璃的**底色与 tint** 必须随主题换档。
  //
  // 为何必须有这一条（实测代价）：暗色沿用亮色档（纯白 `fill` + `tint 0.22`）时，
  // 气泡内亮度 **73.7** vs 背景 **23.0**（亮 50）——看上去是一块“发辉的灰纸”，
  // 而不是玻璃。而亮色档本身是对的，所以只测亮色永远发现不了。
  //
  // 判据用**底色 alpha**：暗色档是“白但低 alpha”（α≈18%），亮色档是全白。
  // 拿 alpha 而不是 RGB 颜色：色相都是白，只有透明度在变（那正是“膜厚”）。
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<BackdropPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  Element* backdrop = root.find("backdrop");
  ST_REQUIRE(backdrop != nullptr);
  backdrop->style().theme_glass = true;

  root.set_theme(Theme::light());
  root.layout(true);
  const std::uint8_t light_alpha = backdrop->style().background.a;
  const float light_tint = backdrop->style().backdrop_tint;

  root.set_theme(Theme::dark());
  root.layout(true);
  const std::uint8_t dark_alpha = backdrop->style().background.a;
  const float dark_tint = backdrop->style().backdrop_tint;

  // 亮色档：白膜不透明（≈255）；暗色档：显著更透（α≈46 = 0x2E）。
  ST_CHECK(light_alpha > 200U);
  ST_CHECK(dark_alpha < 120U);
  // tint 也必须换档（暗色更高，总白膜厚度 = alpha × tint）。
  ST_CHECK(dark_tint > light_tint + 0.1f);
}

ST_TEST(backdrop_gradient_absent_falls_back_to_solid) {
  // 主题**没定**渐变时必须退回纯色底，而不是留着上一套渐变。
  //
  // 为何要这一条：`set_gradient_background` 一旦设过，`background_gradient`
  // 就非空；换到一个没有渐变的主题若不显式清，画面会停留在旧主题的渐变上——
  // 那是“换肤换了一半”的经典形态。
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<BackdropPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  Element* backdrop = root.find("backdrop");
  ST_REQUIRE(backdrop != nullptr);
  backdrop->style().use_theme_backdrop = true;

  root.set_theme(Theme::light());
  root.layout(true);
  ST_CHECK(backdrop->style().background_gradient != nullptr);

  Theme no_gradient = Theme::light();
  no_gradient.set_backdrop_gradient(std::nullopt);
  root.set_theme(no_gradient);
  root.layout(true);
  ST_CHECK(backdrop->style().background_gradient == nullptr);
  // 退回纯色时用的是主题底色（不透明），不是透明裸底。
  ST_CHECK(backdrop->style().background.a == 255U);
}

ST_TEST(glass_shorthand_sets_all_params) {
  // **核心 ②**：一条字符串把四五个参数一次说完。
  UiRoot root;
  root.set_viewport({300.0f, 200.0f});
  auto page = std::make_shared<BackdropPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  Element* backdrop = root.find("backdrop");
  ST_REQUIRE(backdrop != nullptr);
  const bool ok = apply_glass_shorthand(*backdrop, "blur:8,tint:0.22,refraction:20,dispersion:0.35");
  ST_CHECK(ok);
  const Style& style = backdrop->style();
  ST_CHECK(style.backdrop_blur > 7.9f && style.backdrop_blur < 8.1f);
  ST_CHECK(style.backdrop_tint > 0.21f && style.backdrop_tint < 0.23f);
  ST_CHECK(style.backdrop_refraction > 19.9f && style.backdrop_refraction < 20.1f);
  ST_CHECK(style.backdrop_dispersion > 0.34f && style.backdrop_dispersion < 0.36f);
}

ST_TEST(glass_shorthand_off_clears_everything) {
  // `glass="off"` 必须把**四个量全清**（只清一部分会留一块半透明残影）。
  UiRoot root;
  root.set_viewport({300.0f, 200.0f});
  auto page = std::make_shared<BackdropPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  Element* backdrop = root.find("backdrop");
  ST_REQUIRE(backdrop != nullptr);
  (void)apply_glass_shorthand(*backdrop, "blur:8,tint:0.22,refraction:20,dispersion:0.35");
  ST_CHECK(apply_glass_shorthand(*backdrop, "off"));
  const Style& style = backdrop->style();
  ST_CHECK(style.backdrop_blur == 0.0f);
  ST_CHECK(style.backdrop_tint == 0.0f);
  ST_CHECK(style.backdrop_refraction == 0.0f);
  ST_CHECK(style.backdrop_dispersion == 0.0f);
}

ST_TEST(glass_shorthand_unknown_key_reports_failure) {
  // 键名全拼错时必须返回 false——报成功会让调用方对着“没生效”查半天。
  UiRoot root;
  root.set_viewport({300.0f, 200.0f});
  auto page = std::make_shared<BackdropPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  Element* backdrop = root.find("backdrop");
  ST_REQUIRE(backdrop != nullptr);
  ST_CHECK(!apply_glass_shorthand(*backdrop, "blurr:8,tnt:0.22"));
  // 认出一部分就算成功（拼错一个键不该让整条配置作废）。
  ST_CHECK(apply_glass_shorthand(*backdrop, "blur:8,wrongkey:1"));
  ST_CHECK(backdrop->style().backdrop_blur > 7.9f);
}

ST_TEST(glass_shorthand_bare_number_is_blur_with_readable_tint) {
  // `glass="8"` 这类最简写法：只给模糊时自动补一个可读的 tint，
  // 否则玻璃几乎全透、文字背后内容全穿（那不该是“默认好用”的行为）。
  UiRoot root;
  root.set_viewport({300.0f, 200.0f});
  auto page = std::make_shared<BackdropPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  Element* backdrop = root.find("backdrop");
  ST_REQUIRE(backdrop != nullptr);
  ST_CHECK(apply_glass_shorthand(*backdrop, "8"));
  ST_CHECK(backdrop->style().backdrop_blur > 7.9f);
  ST_CHECK(backdrop->style().backdrop_tint > 0.2f);
}
