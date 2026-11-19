/// 声明式的**液态玻璃字段**：`BoxProps` 的四个玻璃项必须真的落到 `Style` 上。
///
/// ## 为何要有这条
///
/// 玻璃能力本来就在绘制层（`Style::backdrop_blur` / `backdrop_refraction` /
/// `backdrop_dispersion` / `backdrop_tint`），缺的只是**声明式入口**。
/// 这类“能力存在但写不出来”的缺口有个共同症状：**属性面完全正常**
/// （元素建出来了、`get` 回读的尺寸/颜色都对），只有画面是错的——
/// 因为声明式根本没把值传给 `Style`，绘制层拿到的还是默认值。
///
/// 小白的消息气泡是第一个用户：不给入口时只能退到 `custom<Panel>` 手写
/// `style()`，而那正是“一行能写完的事要开一个回调”的绕路。
///
/// ## 判据为什么是 `style()` 而不是截图
///
/// 玻璃的最终观感取决于后端（支持折射 / 只支持模糊 / 都不支持），
/// 截图只能证明“某台机器上是这个样”，证明不了“字段接通了”。
/// 而“字段是否落到 `Style`”是一句确定性断言——它不变，观感才可预期。
///
/// ## 另一条会被这条测试抓住的缺陷
///
/// `backdrop_*` 的默认值必须**不改变既有声明的行为**：
/// `backdrop_tint` 的缺省是 `1.0`（完全不透明），所以老代码不写玻璃时
/// 外观与从前一致。若有人把缺省改成 `0.8` 之类“更通透”的值，
/// 全仓库所有元素都会悄悄变半透明——这条测试用显式断言把默认值钉住。
#include <memory>
#include <string>

#include "st/test/test.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using namespace st::ui;
using namespace st::ui::dsl;

/// 一页两张卡片：一张显式给玻璃参数，一张什么都不给（默认值基线）。
struct GlassPage : Component {
  void build(Composer& c) override {
    column(c, {}, [&] {
      (void)card(c, {.radius = 14.0f,
                     .backdrop_blur = 18.0f,
                     .backdrop_tint = 0.32f,
                     .backdrop_refraction = 14.0f,
                     .backdrop_dispersion = 0.35f,
                     .key = "glass"},
                 [&] { (void)text(c, [] { return std::string("玻璃卡片"); }); });
      (void)card(c, {.key = "plain"},
                 [&] { (void)text(c, [] { return std::string("普通卡片"); }); });
    });
  }
};

/// 按 key 找卡片（`key` 是业务身份，重组/插入兄弟都不会串）。
[[nodiscard]] auto find_card_by_key(const Element& parent, std::string_view key)
    -> const Element* {
  for (std::size_t index = 0; index < parent.content_child_count(); ++index) {
    const Element* child = parent.child_at(index);
    if (child == nullptr) continue;
    if (child->key() == key) return child;
  }
  return nullptr;
}

}  // namespace

ST_TEST(dsl_glass_props_reach_style) {
  // **核心回归**：四个玻璃字段必须逐项落到 `Style`。
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<GlassPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  const Element* content = root.content();
  ST_REQUIRE(content != nullptr);
  const Element* glass = find_card_by_key(*content, "glass");
  ST_REQUIRE(glass != nullptr);

  const Style& style = glass->style();
  ST_CHECK(style.backdrop_blur > 17.9f);
  ST_CHECK(style.backdrop_blur < 18.1f);
  ST_CHECK(style.backdrop_refraction > 13.9f);
  ST_CHECK(style.backdrop_refraction < 14.1f);
  ST_CHECK(style.backdrop_dispersion > 0.34f);
  ST_CHECK(style.backdrop_dispersion < 0.36f);
  // `tint` 是“叠色不透明度”：0.32 表示偏通透，必须真的写进去。
  ST_CHECK(style.backdrop_tint > 0.31f);
  ST_CHECK(style.backdrop_tint < 0.33f);
  ST_CHECK(style.radius > 13.9f);
}

ST_TEST(dsl_glass_defaults_leave_plain_card_untouched) {
  // **默认值护栏**：不写玻璃字段的卡片必须保持“无玻璃、不透明”。
  //
  // 这条防的是“为了让气泡更通透，把 `backdrop_tint` 缺省改成 0.8”这类改动——
  // 那会让全仓库所有元素都悄悄变半透明，而**没有任何一处代码写过错**。
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<GlassPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  const Element* content = root.content();
  ST_REQUIRE(content != nullptr);
  const Element* plain = find_card_by_key(*content, "plain");
  ST_REQUIRE(plain != nullptr);

  const Style& style = plain->style();
  ST_CHECK(style.backdrop_blur == 0.0f);
  ST_CHECK(style.backdrop_refraction == 0.0f);
  ST_CHECK(style.backdrop_dispersion == 0.0f);
  ST_CHECK(style.backdrop_tint == 1.0f);
}

ST_TEST(dsl_glass_fields_are_independent) {
  // 四个量各管一件事：只给模糊时，折射/色散不能被“顺带”打开
  //（否则“我只想要毛玻璃”的调用方会得到意料之外的畸变）。
  struct OnlyBlur : Component {
    void build(Composer& c) override {
      (void)card(c, {.backdrop_blur = 12.0f, .key = "blur-only"},
                 [&] { (void)text(c, [] { return std::string("只模糊"); }); });
    }
  };

  UiRoot root;
  root.set_viewport({300.0f, 200.0f});
  auto page = std::make_shared<OnlyBlur>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  // `mount` 是**单根替换**：这一页只声明了一张 card，所以它自己就是根。
  const Element* card_element = root.content();
  ST_REQUIRE(card_element != nullptr);
  ST_CHECK(card_element->key() == "blur-only");

  const Style& style = card_element->style();
  ST_CHECK(style.backdrop_blur > 11.9f);
  ST_CHECK(style.backdrop_refraction == 0.0f);
  ST_CHECK(style.backdrop_dispersion == 0.0f);
}
