#include "st/ui/builtin_themes.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/ui/theme_io.hpp"

#include "battery/embed.hpp"

namespace st::ui {
namespace {

/// 歌白主题表：名字 → 嵌入的 JSON 资源。
///
/// **必须与 `src/ui/themes/*.json` 的文件名严格一致**——
/// `b::embed<"…">()` 的键是编译期字符串，拼错就是编译失败
/// （这比运行时才发现好：主题缺一个不会变成"界面没变化"）。
struct Entry {
  std::string_view name;
  std::string_view json;
};

[[nodiscard]] auto load_embedded(const char* data, std::size_t length) -> std::string_view {
  // 嵌入资源是编译期生成的 `const char[]`，生命周期与进程同长，
  // 因此可以直接造 string_view（不需要拷贝）。
  return std::string_view(data, length);
}

[[nodiscard]] auto catalog() -> const std::vector<Entry>& {
  static const std::vector<Entry> entries = [] {
    std::vector<Entry> list;
    const auto add = [&list](std::string_view name, auto file) {
      list.push_back(Entry{name, load_embedded(file.data(), file.length())});
    };
    add("cny", b::embed<"src/ui/themes/cny.json">());
    add("cyberpunk", b::embed<"src/ui/themes/cyberpunk.json">());
    add("ink", b::embed<"src/ui/themes/ink.json">());
    add("matrix", b::embed<"src/ui/themes/matrix.json">());
    add("nebula", b::embed<"src/ui/themes/nebula.json">());
    add("qinhan", b::embed<"src/ui/themes/qinhan.json">());
    add("synthwave", b::embed<"src/ui/themes/synthwave.json">());
    add("tokyo-night", b::embed<"src/ui/themes/tokyo-night.json">());
    return list;
  }();
  return entries;
}

}  // namespace

auto builtin_theme(std::string_view name) -> std::optional<Theme> {
  // `light` / `dark` 直接走内置两档：它们有大量手调值（文本 gamma、卡片描边强度
  // 等派生量），经 JSON 往返会丢掉"不是 token 的那部分"。
  if (const auto mode = theme_mode_from_name(name); mode.has_value()) {
    return Theme::by_mode(*mode);
  }
  for (const auto& entry : catalog()) {
    if (entry.name != name) continue;
    // 主题 JSON 是**编译期嵌入**的常量，解析失败属于"打包错误"而非运行期输入错误：
    // 报错并返回 nullopt，让调用方明确知道这个名字取不到（不回退到别的主题——
    // 静默给一个"差不多的"会让"我选的主题没生效"变成一个谜）。
    auto parsed = parse_theme_json(entry.json, ThemeMode::Dark);
    if (!parsed) {
      ST_LOG_ERROR("内置主题 {} 解析失败：{}", entry.name, parsed.error().message);
      return std::nullopt;
    }
    return std::move(*parsed);
  }
  return std::nullopt;
}

auto builtin_theme_names() -> std::vector<std::string_view> {
  std::vector<std::string_view> names{"dark", "light"};
  for (const auto& entry : catalog()) names.push_back(entry.name);
  std::sort(names.begin(), names.end());
  return names;
}

auto builtin_theme_display_name(std::string_view name) -> std::string_view {
  // 中文名登记表（英文名是稳定 id，中文名是给选择器 UI 看的意象名）。
  // ⚠ 轻量映射不触发主题解析：给 10 套内置主题查名字不需要解析 JSON。
  static constexpr std::pair<std::string_view, std::string_view> kDisplayNames[]{
      {"light", "亮色"},
      {"dark", "暗色"},
      {"qinhan", "秦汉玄漆"},
      {"nebula", "星云青"},
      {"cny", "中国红"},
      {"cyberpunk", "赛博朋克"},
      {"ink", "水墨"},
      {"matrix", "矩阵绿"},
      {"synthwave", "合成波"},
      {"tokyo-night", "东京夜"},
  };
  for (const auto& [id, display] : kDisplayNames) {
    if (id == name) return display;
  }
  return name;   // 外部主题文件：原名即显示名（不报错——选择器不能因缺登记拒显示）
}

auto builtin_theme_preview_colors(std::string_view name)
    -> std::optional<ThemePreviewColors> {
  // 解析主题拿 bg/primary（两色最能一眼认主题：底色 + 主色）。
  // 解析失败/名字不存在 → nullopt（调用方用当前主题色占位）。
  const auto theme = builtin_theme(name);
  if (!theme.has_value()) return std::nullopt;
  return ThemePreviewColors{theme->colors().bg, theme->colors().primary};
}

}  // namespace st::ui
