/// 主题自定义（`st/ui/theme_io.hpp`）的契约：稀疏覆盖、失败安全、令牌名表往返。
///
/// 为什么这些值得写成断言：主题是**外部输入**（用户手写 JSON、控制通道改色），
/// 而外部输入的错误方式几乎是无穷的——写错 token 名、颜色值打成半句、给数字塞字符串。
/// 逐条钉住的收益是：出错时给出的是「哪个键错了」而不是「界面看着不对」。
///
/// 另一半是**名字表与取址的一致性**：`palette_token_names()` 是给用户看的，
/// `palette_token()` 是给代码用的，两者漂移会让"文档里有、代码里没有"变成静默失败。

#include "st/test/test.hpp"

#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "st/ext/json.hpp"
#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/theme_io.hpp"

namespace {

using st::Json;
using st::math::Color;
using st::ui::Theme;
using st::ui::ThemeMode;

[[nodiscard]] auto same(const Color& lhs, const Color& rhs) -> bool {
  return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

/// 在 JSON 文本上跑一遍解析；失败时返回错误消息（便于断言"报的是哪个键"）。
[[nodiscard]] auto parse_error(std::string_view text) -> std::string {
  const auto parsed = st::ui::parse_theme_json(text, ThemeMode::Dark);
  if (parsed) return {};
  return parsed.error().message;
}

}  // namespace

// —— 令牌名表 ——

ST_TEST(theme_palette_token_names_are_unique_and_resolvable) {
  const auto& names = st::ui::palette_token_names();
  ST_CHECK(names.size() >= 30U);
  const Theme theme = Theme::dark();
  for (std::size_t i = 0; i < names.size(); ++i) {
    // 每个声明的名字都必须能取到值（否则用户在主题文件里写它会被当成未知 token）
    ST_CHECK(st::ui::palette_token(theme.colors(), names[i]).has_value());
    for (std::size_t j = i + 1; j < names.size(); ++j) {
      // 重名会让"按名取址"变成取第一个，后一个永久不可达
      ST_CHECK(names[i] != names[j]);
    }
  }
  ST_CHECK(!st::ui::palette_token(theme.colors(), "not_a_token").has_value());
  Theme mutable_theme = Theme::dark();
  ST_CHECK(!st::ui::set_palette_token(mutable_theme.colors(), "not_a_token", Color{1, 2, 3, 255}));
}

ST_TEST(theme_metric_token_names_are_unique_and_resolvable) {
  const auto& names = st::ui::metric_token_names();
  ST_CHECK(names.size() >= 20U);
  const Theme theme = Theme::dark();
  for (std::size_t i = 0; i < names.size(); ++i) {
    ST_CHECK(st::ui::metric_token(theme.metrics(), names[i]).has_value());
    for (std::size_t j = i + 1; j < names.size(); ++j) {
      ST_CHECK(names[i] != names[j]);
    }
  }
  ST_CHECK(!st::ui::metric_token(theme.metrics(), "not_a_token").has_value());
}

ST_TEST(theme_metric_token_write_clamps_to_sane_range) {
  // 尺度令牌是**外部输入**：负圆角/零字号会让界面直接消失，属于"配置写错不该让进程崩"。
  Theme theme = Theme::dark();
  ST_CHECK(st::ui::set_metric_token(theme.metrics(), "radius_md", -50.0f));
  ST_CHECK(theme.metrics().radius_md >= 0.0f);
  ST_CHECK(st::ui::set_metric_token(theme.metrics(), "font_base", 0.0f));
  // 字号不能变成 0（那会让整屏文字消失），必须被夹到下限之上
  ST_CHECK(theme.metrics().font_base > 0.0f);
  // NaN 直接拒绝（接受了会污染整个布局）
  ST_CHECK(!st::ui::set_metric_token(theme.metrics(), "radius_md",
                                     std::numeric_limits<float>::quiet_NaN()));
}

// —— 稀疏覆盖 ——

ST_TEST(theme_json_sparse_override_keeps_base_tokens) {
  const auto parsed = st::ui::parse_theme_json(R"({"base":"dark","colors":{"primary":"#7C5CFF"}})",
                                               ThemeMode::Light);
  ST_CHECK(parsed.has_value());
  if (!parsed) return;
  // `base` 决定模式（这正是它存在的理由：亮底上的深色自定义主题不能被当成暗色）
  ST_CHECK(parsed->mode() == ThemeMode::Dark);
  ST_CHECK(same(parsed->colors().primary, Color{0x7C, 0x5C, 0xFF, 0xFF}));
  // 未提及的 token 必须原样继承
  const Theme reference = Theme::dark();
  ST_CHECK(same(parsed->colors().bg, reference.colors().bg));
  ST_CHECK(same(parsed->colors().text, reference.colors().text));
}

ST_TEST(theme_json_base_defaults_to_fallback) {
  const auto parsed = st::ui::parse_theme_json(R"j({"colors":{"bg":"#101010"}})j", ThemeMode::Dark);
  ST_CHECK(parsed.has_value());
  if (!parsed) return;
  ST_CHECK(parsed->mode() == ThemeMode::Dark);
  ST_CHECK(same(parsed->colors().bg, Color{0x10, 0x10, 0x10, 0xFF}));
}

// —— 颜色语法 ——

ST_TEST(theme_json_accepts_documented_color_syntaxes) {
  struct Case {
    const char* text;
    Color expected;
  };
  // `rgba()` 存在是为了让从 CSS 搬过来的值能直接粘（歌白的主题文件就是 rgba 写法）。
  const Case cases[] = {
      {"#FF0000", Color{0xFF, 0x00, 0x00, 0xFF}},
      {"#abc", Color{0xAA, 0xBB, 0xCC, 0xFF}},
      {"#FF000080", Color{0xFF, 0x00, 0x00, 0x80}},
      {"rgb(255, 0, 0)", Color{0xFF, 0x00, 0x00, 0xFF}},
      {"rgba(255, 0, 0, 0.5)", Color{0xFF, 0x00, 0x00, 0x80}},
      {"rgba(255,0,0,128)", Color{0xFF, 0x00, 0x00, 0x80}},
      {"rgba(255,0,0,50%)", Color{0xFF, 0x00, 0x00, 0x80}},
  };
  for (const Case& item : cases) {
    const std::string text = std::string("{\"colors\":{\"primary\":\"") + item.text + "\"}}";
    const auto parsed = st::ui::parse_theme_json(text, ThemeMode::Dark);
    ST_CHECK(parsed.has_value());
    if (!parsed) {
      // 失败时把**是哪个用例**与原因打出来：十多个用例里只有一个红，
      // 而失败信息只有“parsed.has_value()”时根本定位不到（实测就卡在这里）。
      st::print("[case] color syntax '{}' 被拒绝：{}\n", item.text, parsed.error().message);
      continue;
    }
    if (!same(parsed->colors().primary, item.expected)) {
      // 同理：值是错的也要能看出是哪一个（rgba 的 alpha 换算最容易差一档）。
      const Color& got = parsed->colors().primary;
      st::print("[case] color syntax '{}' 得到 #{:02X}{:02X}{:02X}{:02X}，期望 #{:02X}{:02X}{:02X}{:02X}\n",
                item.text, got.r, got.g, got.b, got.a, item.expected.r, item.expected.g,
                item.expected.b, item.expected.a);
    }
    ST_CHECK(same(parsed->colors().primary, item.expected));
  }
}

ST_TEST(theme_json_rejects_malformed_colors) {
  // 半句、多余参数、不认识的语法都要报错，不能静默退化成某个默认色。
  // 注：用 `R"j(...)j"` 自定义分隔符——默认的 `)"` 会被 `rgba(255,0,0)"}})` 中的
  // `)"` 提前终止，让后半段变成乱码（实测编译报错就在此处）。
  ST_CHECK(!parse_error(R"j({"colors":{"primary":"#FF00"}})j").empty());
  ST_CHECK(!parse_error(R"j({"colors":{"primary":"rgba(255,0,0)"}})j").empty());
  ST_CHECK(!parse_error(R"j({"colors":{"primary":"rgb(255,0,0,0.5)"}})j").empty());
  ST_CHECK(!parse_error(R"j({"colors":{"primary":"hsl(0,100%,50%)"}})j").empty());
  ST_CHECK(!parse_error(R"j({"colors":{"primary":"red"}})j").empty());
  ST_CHECK(!parse_error(R"j({"colors":{"primary":123}})j").empty());
  // 通道越界的十进制值同样要挡住
  ST_CHECK(!parse_error(R"j({"colors":{"primary":"rgb(300,0,0)"}})j").empty());
}

// —— 失败安全 ——

ST_TEST(theme_json_unknown_token_is_reported_not_ignored) {
  // 这是本模块最重要的一条：拼错被静默忽略 = "我改了但没生效"，最难查的一类问题。
  const std::string message = parse_error(R"({"colors":{"primry":"#FF0000"}})");
  ST_CHECK(!message.empty());
  ST_CHECK(message.find("primry") != std::string::npos);

  const std::string metric_message = parse_error(R"({"metrics":{"radius_mid":14}})");
  ST_CHECK(!metric_message.empty());
  ST_CHECK(metric_message.find("radius_mid") != std::string::npos);

  // 顶层字段拼错同样要报
  const std::string top = parse_error(R"({"colour":{"primary":"#FF0000"}})");
  ST_CHECK(!top.empty());
  ST_CHECK(top.find("colour") != std::string::npos);
}

ST_TEST(theme_json_unknown_base_is_reported) {
  const std::string message = parse_error(R"({"base":"solarized"})");
  ST_CHECK(!message.empty());
  ST_CHECK(message.find("solarized") != std::string::npos);
}

ST_TEST(theme_json_rejects_non_object_and_broken_syntax) {
  ST_CHECK(!parse_error("[]").empty());
  ST_CHECK(!parse_error("{").empty());
  ST_CHECK(!parse_error(R"({"colors":[]})").empty());
  ST_CHECK(!parse_error(R"({"metrics":[]})").empty());
  ST_CHECK(!parse_error(R"({"metrics":{"radius_md":"big"}})").empty());
}

// —— 往返与运行时覆盖 ——

ST_TEST(theme_json_roundtrip_preserves_every_token) {
  for (const Theme& theme : {Theme::light(), Theme::dark()}) {
    const Json dumped = st::ui::theme_to_json(theme);
    const auto restored = st::ui::theme_from_json(dumped, ThemeMode::Light);
    ST_CHECK(restored.has_value());
    if (!restored) continue;
    ST_CHECK(restored->mode() == theme.mode());
    ST_CHECK(restored->name() == theme.name());
    ST_CHECK(restored->font_family() == theme.font_family());
    for (const std::string_view name : st::ui::palette_token_names()) {
      const auto before = st::ui::palette_token(theme.colors(), name);
      const auto after = st::ui::palette_token(restored->colors(), name);
      ST_CHECK(before.has_value() && after.has_value());
      if (before && after) ST_CHECK(same(*before, *after));
    }
    for (const std::string_view name : st::ui::metric_token_names()) {
      const auto before = st::ui::metric_token(theme.metrics(), name);
      const auto after = st::ui::metric_token(restored->metrics(), name);
      ST_CHECK(before.has_value() && after.has_value());
      if (before && after) ST_CHECK(*before == *after);
    }
  }
}

ST_TEST(theme_overrides_apply_onto_existing_without_resetting) {
  // 控制通道的 `set` 语义：分两次下发时前一笔不能被后一笔抹掉。
  Theme theme = Theme::dark();
  Json first = Json::object();
  first["colors"] = Json::object();
  first["colors"]["primary"] = "#7C5CFF";
  ST_CHECK(st::ui::apply_theme_overrides(theme, first).has_value());

  Json second = Json::object();
  second["metrics"] = Json::object();
  second["metrics"]["radius_md"] = 14.0;
  ST_CHECK(st::ui::apply_theme_overrides(theme, second).has_value());

  ST_CHECK(same(theme.colors().primary, Color{0x7C, 0x5C, 0xFF, 0xFF}));
  ST_CHECK(theme.metrics().radius_md == 14.0f);
}

ST_TEST(theme_overrides_accept_matching_base_and_reject_mismatch) {
  // 允许一致的 `base`：`theme` 返回的快照可以直接改两笔再喂回 `set`（导出→改→应用）。
  Theme dark_theme = Theme::dark();
  Json snapshot = st::ui::theme_to_json(dark_theme);
  snapshot["colors"]["bg"] = "#010101";
  const auto applied = st::ui::apply_theme_overrides(dark_theme, snapshot);
  ST_CHECK(applied.has_value());
  ST_CHECK(same(dark_theme.colors().bg, Color{0x01, 0x01, 0x01, 0xFF}));

  // 与当前模式不符的 `base` 要拒绝：调用方以为在改另一套主题，静默照做会改错对象。
  Theme light_theme = Theme::light();
  Json mismatch = Json::object();
  mismatch["base"] = "dark";
  mismatch["colors"] = Json::object();
  mismatch["colors"]["bg"] = "#010101";
  ST_CHECK(!st::ui::apply_theme_overrides(light_theme, mismatch).has_value());
  // 失败时**不得**修改任何状态
  ST_CHECK(same(light_theme.colors().bg, Theme::light().colors().bg));
}

ST_TEST(theme_overrides_reject_unknown_token) {
  Theme theme = Theme::dark();
  Json bad = Json::object();
  bad["colors"] = Json::object();
  bad["colors"]["nope"] = "#FF0000";
  const auto applied = st::ui::apply_theme_overrides(theme, bad);
  ST_CHECK(!applied.has_value());
  if (!applied) ST_CHECK(applied.error().message.find("nope") != std::string::npos);
}

ST_TEST(theme_load_file_reports_missing_file) {
  const auto missing = st::ui::load_theme_file("/nonexistent/theme-should-not-exist.json",
                                               ThemeMode::Dark);
  ST_CHECK(!missing.has_value());
  if (!missing) ST_CHECK(!missing.error().message.empty());
}

ST_TEST(theme_mode_names_roundtrip) {
  ST_CHECK(st::ui::theme_mode_from_name("light") == ThemeMode::Light);
  ST_CHECK(st::ui::theme_mode_from_name("dark") == ThemeMode::Dark);
  ST_CHECK(!st::ui::theme_mode_from_name("system").has_value());
  ST_CHECK(st::ui::theme_mode_name(ThemeMode::Light) == "light");
  ST_CHECK(st::ui::theme_mode_name(ThemeMode::Dark) == "dark");
}
