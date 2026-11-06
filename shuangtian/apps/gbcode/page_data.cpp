// 数据与工具的实现（声明见 page_data.hpp）。
//
// 2026-10-18：内置样例表（`samples()`）已删除——gbcode 转为正式使用后
// 工作区数据一律来自磁盘，不再有“无参数时给假文件”的演示形态。

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "page_data.hpp"
#include "st/text/highlight.hpp"

namespace gbcode_ui {

/// 自定义语言：业务日志（`stlog`）。
///
/// 这是**真能力**：运行时注册的语言与内置语言走同一台扫描器（“规则即数据”），
/// 产物是可用的高亮语言而不是界面上的弹窗。
void register_custom_language() {
  st::text::LanguageSpec spec = st::text::make_language("stlog");
  st::text::with_aliases(spec, {"slog", "applog"});
  st::text::with_keywords(spec, {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"});
  st::text::with_types(spec, {"service", "component", "trace_id", "span_id", "duration"});
  st::text::with_builtins(spec, {"true", "false", "null", "req", "resp"});
  st::text::with_line_comment(spec, "#");
  spec.key_value_keys = true;
  (void)st::text::global_languages().register_language(spec, /*replace=*/true);
}

/// 便宜的逐行检查（不引入分析器）：行尾空白 / Tab 缩进 / TODO·FIXME / 超长行。
///
/// 为什么值得做：状态栏的「错误 · 警告」计数与问题面板若没有真实来源，就只是摆设
/// ——改完代码什么都不会变，读者也无法验证计数联动是对的。
[[nodiscard]] auto lint_text(std::string_view text) -> std::vector<Problem> {
  std::vector<Problem> problems;
  std::size_t line_no = 0;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t eol = text.find('\n', begin);
    const std::size_t end = eol == std::string_view::npos ? text.size() : eol;
    std::string_view line = text.substr(begin, end - begin);
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
      problems.push_back(Problem{line_no, true, "行尾有多余空白"});
    }
    if (!line.empty() && line.front() == '\t') {
      problems.push_back(Problem{line_no, true, "用 Tab 缩进（本工程约定空格）"});
    }
    if (line.find("TODO") != std::string_view::npos ||
        line.find("FIXME") != std::string_view::npos) {
      problems.push_back(Problem{line_no, true, "待办标记（TODO/FIXME）"});
    }
    if (line.size() > 120) problems.push_back(Problem{line_no, true, "行超长（>120 列）"});
    if (eol == std::string_view::npos) break;
    begin = eol + 1;
  }
  return problems;
}

/// 读文件进列表（已打开则刷新内容，保持同一标签）；返回该标签下标（失败 = size()）。
[[nodiscard]] auto read_into_buffers(std::vector<OpenBuffer>& buffers, const std::string& path)
    -> std::size_t {
  auto content = st::fs::read_text(path);
  if (!content) return buffers.size();
  // 二进制保护（实测踩到）：二进制文件按文本展示只是难看，**能被编辑并保存**
  // 才是真危险——Ctrl+S 会把屏幕上的乱码写回原文件（原字节不可恢复）。
  // 所以在这里一次性判定，后续（打开/激活/保存）全部看 `buffer.binary`。
  const bool binary = looks_binary(*content);
  for (std::size_t index = 0; index < buffers.size(); ++index) {
    if (buffers[index].path == path) {
      buffers[index].text = *content;
      buffers[index].dirty = false;
      buffers[index].binary = binary;
      return index;
    }
  }
  OpenBuffer buffer;
  buffer.key = path;
  buffer.path = path;
  buffer.binary = binary;
  const std::size_t slash = path.find_last_of("/\\");
  buffer.label = slash == std::string::npos ? path : path.substr(slash + 1);
  buffer.language = st::text::language_from_path(path).value_or("");
  buffer.text = std::move(*content);
  buffers.push_back(std::move(buffer));
  return buffers.size() - 1;
}

[[nodiscard]] auto looks_binary(std::string_view bytes) -> bool {
  // 只看头 8 KiB：足以认出 PE/PNG/ZIP/ELF/数据库文件，且不会为了判定
  // 一个 200MB 的日志把整文件扫一遍（读进内存的代价已经付过一次了）。
  const std::size_t window = std::min<std::size_t>(bytes.size(), 8192);
  if (window == 0) return false;
  std::size_t suspicious = 0;
  for (std::size_t index = 0; index < window; ++index) {
    const auto byte = static_cast<unsigned char>(bytes[index]);
    if (byte == 0x00) return true;   // NUL：文本文件里不出现（最硬的信号）
    const bool printable_control = byte == '\t' || byte == '\r' || byte == '\n' || byte == '\f' ||
                                   byte == '\v' || byte == '\b';
    if (byte < 0x20 && !printable_control) ++suspicious;
  }
  // 两条同时满足才算二进制：
  // ① 控制字符密度 > 5%（真正的二进制文件远超这个数）；
  // ② **控制字符个数 ≥ 8**。
  //
  // 为何必须有第二条（实测踩到）：只比密度的话，一个**短**文件里恰好有两三个
  // ANSI 转义符（带颜色的日志片段、make 输出）就会被判成二进制——
  // 而它明明是可读的文本（回归用例 `gbcode_looks_binary_detects_…` 当场抓住）。
  return suspicious >= 8 && suspicious * 20 > window;
}

/// 扫描一层目录并归一为「目录在前、各自字典序、忽略点开头」的稳定序。
/// 与旧实现同口径，但面向工作区树的缓存扫描（每帧重扫的系统调用不再能承受）。
[[nodiscard]] auto list_dir_sorted(const std::string& root) -> std::vector<DirEntryLite> {
  std::vector<DirEntryLite> out;
  auto entries = st::fs::list_dir(root);
  if (!entries) return out;
  std::vector<const st::fs::DirEntry*> dirs;
  std::vector<const st::fs::DirEntry*> files;
  for (const auto& entry : *entries) {
    if (!entry.name.empty() && entry.name.front() == '.') continue;   // 隐藏项不入树
    (entry.is_dir ? dirs : files).push_back(&entry);
  }
  const auto by_name = [](const auto* a, const auto* b) { return a->name < b->name; };
  std::sort(dirs.begin(), dirs.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);
  out.reserve(dirs.size() + files.size());
  for (const auto* dir : dirs) out.push_back(DirEntryLite{.name = dir->name, .is_dir = true});
  for (const auto* file : files) out.push_back(DirEntryLite{.name = file->name});
  return out;
}

}  // namespace gbcode_ui
