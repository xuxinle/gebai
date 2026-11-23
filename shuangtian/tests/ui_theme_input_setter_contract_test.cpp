/// **机制守门**：setter 改了“会被 `apply_theme` 读取的成员”，就必须声明样式输入变了。
///
/// ## 这条测试替代的是什么东西
///
/// 2026-11-23 的主题重构（取消每帧整树重算）切断了一条**隐式通道**：
///
/// ```
/// setter 改成员 + mark_dirty()   →   下一帧 apply_theme 自动把成员翻译成 style_
/// ```
///
/// 全仓有 31 处 setter 是这么写的，它们过去能工作**只因为 `apply_theme` 每帧都跑**。
/// 重构后这些 setter 必须显式调 `request_theme_refresh()`，否则症状是：
/// 设了参数 → `get` 回读对（读的是成员）→ **画面不变**（样式没重算）。
///
/// ## 为什么不能靠“改代码时记得”
///
/// 这个框架已经杀过一次同型反模式。`Element::refresh_theme` 的注释原话：
///
/// > 为什么不“让 72 个组件都记得转调基类”：那是靠**约定**保护的不变式，
/// > 新人加组件时必漏。把顺序改成结构性保证……就不存在“忘了”这件事。
///
/// 我这次补的钩子正是同类约定——所以必须有机械拦截，否则下次加一个
/// `set_corner_radius()` 就会静默失效，而且**不报错、不崩溃、单测不红**。
///
/// ## 本测试做什么（源码扫描，不需要运行组件）
///
///   1. 对每个类，收集其 `apply_theme` 里读到的 `foo_` 成员（= 样式输入）；
///   2. 对每个 `set_*` / `mark_*` / `toggle_*` / `add_*` 函数，看它写了哪些 `foo_`；
///   3. 两者有交集，且函数体里**既没有** `request_theme_refresh()`、**也没有**
///      直接写 `style_` / 调 `apply_appearance`（= 自给自足）⇒ 报违约。
///
/// 缓存/LRU 标记（`*_valid_` / `*_dirty_` / `needs_layout_` / `metrics_`）不算样式输入。
///
/// ## 自检（防止“扫 0 个函数 → 假绿”）
///
/// 本用例同时断言**已修复的 setter 确实在扫描结果里**（`Card::set_shadow_level`
/// 与 `Button::set_tone`）。若扫描器坏了（正则不匹配 / 根目录找错 / 去噪吃掉代码），
/// 这两条会立刻红——而不是安静地一条违规都报不出来。

#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/test/test.hpp"

namespace {

/// 去掉注释与字符串/字符字面量（**含跨行字符串**）。
///
/// 为什么必须处理跨行：本仓组件里有跨行的日志格式串，逐行正则会把其中的
/// `{`/`}` 当成代码花括号 ⇒ 函数体范围算错 ⇒ 后面整批函数被吞掉。
/// 开发本测试时踩过：`Text::set_tone` / `set_font_size` / `set_scale` 三个函数
/// 整体消失，导致它们被漏判（靠“已修复的也在报告里”自检才发现）。
auto strip_noise(const std::string& source) -> std::string {
  std::string out;
  out.reserve(source.size());
  const std::size_t size = source.size();
  std::size_t index = 0;
  while (index < size) {
    const char current = source[index];
    if (current == '/' && index + 1 < size && source[index + 1] == '/') {
      while (index < size && source[index] != '\n') {
        out.push_back(' ');
        ++index;
      }
    } else if (current == '/' && index + 1 < size && source[index + 1] == '*') {
      const std::size_t end = source.find("*/", index + 2);
      const std::size_t stop = end == std::string::npos ? size : end + 2;
      for (; index < stop; ++index) out.push_back(source[index] == '\n' ? '\n' : ' ');
    } else if (current == '"' || current == '\'') {
      const char quote = current;
      out.push_back(' ');
      ++index;
      while (index < size) {
        if (source[index] == '\\' && index + 1 < size) {
          out.push_back(source[index] == '\n' ? '\n' : ' ');
          out.push_back(' ');
          index += 2;
          continue;
        }
        const char ch = source[index];
        out.push_back(ch == '\n' ? '\n' : ' ');
        ++index;
        if (ch == quote) break;
      }
    } else {
      out.push_back(current);
      ++index;
    }
  }
  return out;
}

struct Function {
  std::string cls;
  std::string name;
  std::string body;   ///< 已去噪的函数体（含首尾花括号）
};

auto is_ident(const std::string& text, std::size_t index) -> bool {
  const unsigned char ch = static_cast<unsigned char>(text[index]);
  return std::isalpha(ch) != 0 || ch == '_';
}

auto is_ident_body(const std::string& text, std::size_t index) -> bool {
  const unsigned char ch = static_cast<unsigned char>(text[index]);
  return std::isalnum(ch) != 0 || ch == '_';
}

/// 扫 `Cls::fn(...) { ... }` 定义。
///
/// 不用语法树：本仓组件定义格式统一（`返回类型 Cls::fn(`），而这项检查只关心
/// “写了哪些 `foo_`”，正则级扫描足够，也不必依赖编译器前端。
auto scan_functions(const std::string& source) -> std::vector<Function> {
  const std::string clean = strip_noise(source);
  std::vector<Function> out;
  const std::size_t size = clean.size();

  std::size_t line_start = 0;
  while (line_start < size) {
    std::size_t line_end = clean.find('\n', line_start);
    if (line_end == std::string::npos) line_end = size;
    const std::string line = clean.substr(line_start, line_end - line_start);

    // 必须以“仅缩进 + 标识符”开头（函数定义的形态）。这样能排除行内调用
    // （`foo.Bar::baz()`）与缩进很深的表达式。
    std::size_t cursor = 0;
    while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) ++cursor;
    if (cursor < line.size() && is_ident(line, cursor)) {
      // 取开头的标识符串（返回类型/修饰符可能有多个，用空格分隔）
      std::string last_identifier;
      std::size_t scan = cursor;
      bool ok = true;
      while (scan < line.size()) {
        if (line[scan] == ' ' || line[scan] == '\t') {
          ++scan;
          continue;
        }
        if (scan + 1 < line.size() && line[scan] == ':' && line[scan + 1] == ':') break;
        if (!is_ident(line, scan)) {
          // 允许 `*` `&` 这类修饰（指针/引用返回），其它一律放弃本行
          if (line[scan] == '*' || line[scan] == '&') {
            ++scan;
            continue;
          }
          ok = false;
          break;
        }
        const std::size_t begin = scan;
        while (scan < line.size() && is_ident_body(line, scan)) ++scan;
        last_identifier = line.substr(begin, scan - begin);
      }
      if (ok && !last_identifier.empty() && scan + 1 < line.size() && line[scan] == ':' &&
          line[scan + 1] == ':') {
        const std::string cls = last_identifier;
        const std::size_t name_begin = scan + 2;
        std::size_t name_end = name_begin;
        while (name_end < line.size() && is_ident_body(line, name_end)) ++name_end;
        const std::string name = line.substr(name_begin, name_end - name_begin);
        const bool constructor = name == cls || name == "~" + cls;
        const bool callable = name_end < line.size() && line[name_end] == '(';
        if (!name.empty() && callable &&
            std::isalpha(static_cast<unsigned char>(name[0])) != 0) {
          // 参数表可能跨行：从 `(` 起找配对的 `)`，其后的第一个 `{` 就是函数体。
          std::size_t paren = clean.find('(', name_end + line_start);
          int paren_depth = 0;
          std::size_t body_open = std::string::npos;
          if (paren != std::string::npos) {
            for (std::size_t probe = paren; probe < size; ++probe) {
              if (clean[probe] == '(') {
                ++paren_depth;
              } else if (clean[probe] == ')') {
                --paren_depth;
                if (paren_depth == 0) {
                  body_open = clean.find('{', probe);
                  break;
                }
              }
            }
          }
          if (body_open != std::string::npos) {
            int brace_depth = 0;
            std::size_t end = body_open;
            while (end < size) {
              if (clean[end] == '{') {
                ++brace_depth;
              } else if (clean[end] == '}') {
                --brace_depth;
                if (brace_depth == 0) {
                  ++end;
                  break;
                }
              }
              ++end;
            }
            out.push_back(Function{cls, name, clean.substr(body_open, end - body_open)});
            line_start = end;
            continue;
          }
          (void)constructor;
        }
      }
    }
    line_start = line_end + 1;
  }
  return out;
}

/// 收集 `apply_theme` 里读到的成员（`foo_` 形式）。
auto theme_reads_of(const std::vector<Function>& functions)
    -> std::map<std::string, std::set<std::string>> {
  std::map<std::string, std::set<std::string>> reads;
  for (const Function& fn : functions) {
    if (fn.name != "apply_theme") continue;
    const std::string& body = fn.body;
    std::size_t index = 0;
    while (index < body.size()) {
      if (is_ident(body, index)) {
        const std::size_t begin = index;
        while (index < body.size() && is_ident_body(body, index)) ++index;
        const std::string token = body.substr(begin, index - begin);
        if (token.size() > 1 && token.back() == '_') reads[fn.cls].insert(token);
      } else {
        ++index;
      }
    }
  }
  return reads;
}

/// 缓存/失效标志：不是样式输入（由布局/绘制路径自己处理）。
auto is_cache_flag(const std::string& member) -> bool {
  static const std::vector<std::string> kSuffixes{"_valid_", "_dirty_"};
  for (const std::string& suffix : kSuffixes) {
    if (member.size() >= suffix.size() &&
        member.compare(member.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return true;
    }
  }
  return member == "metrics_" || member == "needs_layout_";
}

/// 写了哪些成员（`foo_ =` / `foo_ +=`）。
auto written_members(const std::string& body) -> std::set<std::string> {
  std::set<std::string> written;
  std::size_t index = 0;
  while (index < body.size()) {
    if (is_ident(body, index)) {
      const std::size_t begin = index;
      while (index < body.size() && is_ident_body(body, index)) ++index;
      const std::string token = body.substr(begin, index - begin);
      if (token.size() > 1 && token.back() == '_') {
        std::size_t cursor = index;
        while (cursor < body.size() && body[cursor] == ' ') ++cursor;
        const bool assignment =
            cursor < body.size() && body[cursor] == '=' &&
            (cursor + 1 >= body.size() || body[cursor + 1] != '=');
        const bool compound =
            cursor + 1 < body.size() && body[cursor] == '+' && body[cursor + 1] == '=';
        if (assignment || compound) written.insert(token);
      }
    } else {
      ++index;
    }
  }
  return written;
}

/// 自给自足：自己就把样式写下去了（不需要主题重算）。
auto writes_style_directly(const std::string& body) -> bool {
  static const std::vector<std::string> kMarkers{
      "style_.",  "style().",  "apply_appearance",     "request_theme_refresh",
      "set_solid_background", "set_gradient_background",
  };
  for (const std::string& marker : kMarkers) {
    if (body.find(marker) != std::string::npos) return true;
  }
  return false;
}

/// 定位框架根（含 `st.pkg` 的目录）。找不到就明确失败——不能静默扫 0 个文件，
/// 那会让本用例假绿，而它存在的意义正是拦截静默失效。
auto locate_root() -> std::string {
  const auto cwd = st::fs::current_dir();
  if (!cwd.has_value()) return {};
  std::string cursor = *cwd;
  for (int depth = 0; depth < 10; ++depth) {
    if (st::fs::exists(st::fs::join(cursor, "st.pkg"))) return cursor;
    const std::string normalized = st::fs::normalize(cursor);
    const std::size_t slash = normalized.find_last_of('/');
    if (slash == std::string::npos || slash == 0) break;
    const std::string parent = normalized.substr(0, slash);
    if (parent == cursor) break;
    cursor = parent;
  }
  return {};
}

auto gather_functions() -> std::vector<Function> {
  std::vector<Function> all;
  const std::string root = locate_root();
  if (root.empty()) return all;

  const auto list = st::fs::expand_glob(root, "src/ui/components/*.cpp");
  if (!list.has_value()) return all;
  for (const std::string& relative : *list) {
    const auto text = st::fs::read_text(st::fs::join(root, relative));
    if (!text.has_value()) continue;
    for (Function& fn : scan_functions(*text)) all.push_back(std::move(fn));
  }
  const auto base = st::fs::read_text(st::fs::join(root, "src/ui/element.cpp"));
  if (base.has_value()) {
    for (Function& fn : scan_functions(*base)) all.push_back(std::move(fn));
  }
  return all;
}

}  // namespace

ST_TEST(theme_style_input_setters_declare_refresh) {
  const std::vector<Function> all = gather_functions();
  ST_REQUIRE(!all.empty());   // 根定位失败 / glob 无命中 → 直接红，不许假绿

  const auto reads = theme_reads_of(all);
  ST_REQUIRE(!reads.empty());   // 一个 apply_theme 都没扫到 → 扫描器坏了

  // 自检：扫描器必须能看到已知存在的方法（否则后面的“0 违规”毫无意义）。
  const auto has = [&all](const std::string& cls, const std::string& name) {
    for (const Function& fn : all) {
      if (fn.cls == cls && fn.name == name) return true;
    }
    return false;
  };
  ST_CHECK(has("Card", "set_shadow_level"));
  ST_CHECK(has("Button", "set_tone"));
  ST_CHECK(has("Text", "set_tone"));   // 这条专防“跨行字符串吃掉后续函数”
  ST_CHECK(has("Text", "set_scale"));
  ST_CHECK(has("Card", "apply_theme"));

  std::vector<std::string> violations;
  for (const Function& fn : all) {
    const bool is_setter = fn.name.rfind("set_", 0) == 0 || fn.name.rfind("mark_", 0) == 0 ||
                           fn.name.rfind("toggle_", 0) == 0 || fn.name.rfind("add_", 0) == 0;
    if (!is_setter) continue;
    const auto entry = reads.find(fn.cls);
    if (entry == reads.end()) continue;   // 该类没有 apply_theme：不参与主题推导

    const auto written = written_members(fn.body);
    std::vector<std::string> shared;
    for (const std::string& member : written) {
      if (is_cache_flag(member)) continue;
      if (entry->second.count(member) != 0) shared.push_back(member);
    }
    if (shared.empty()) continue;
    if (writes_style_directly(fn.body)) continue;

    std::string joined;
    for (const std::string& member : shared) {
      if (!joined.empty()) joined += ", ";
      joined += member;
    }
    violations.push_back(std::format(
        "{}::{} 写了 `{}`（`apply_theme` 会读它），但既没调 request_theme_refresh()、"
        "也没直接写 style_ —— 该 setter 的改动会**静默不生效**（设了参数、回读对、画面不变）。"
        "二选一：加 `request_theme_refresh();`，或在函数体里写 `// theme-input: none <理由>`",
        fn.cls, fn.name, joined));
  }

  st::test::Registry::instance().count_check();
  for (const std::string& violation : violations) {
    st::test::Registry::instance().record_failure(__FILE__, __LINE__, violation);
  }
}
