#include "st/text/diff.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// `parse_unified` / `to_unified` 的实现（与 diff.cpp 同属一个模块，拆文件只为可读性：
// 「两段文本 → 差异」与「patch 文本 ⇄ 差异」是两条独立的路径，各有各的坑）。

namespace st::text {

namespace {

/// 按行切 patch（保留空行；去掉行尾 `\r`）。
[[nodiscard]] auto patch_lines(std::string_view patch) -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  std::size_t begin = 0;
  while (begin < patch.size()) {
    const std::size_t eol = patch.find('\n', begin);
    const std::size_t end = eol == std::string_view::npos ? patch.size() : eol;
    std::string_view line = patch.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    out.push_back(line);
    if (eol == std::string_view::npos) break;
    begin = eol + 1;
  }
  return out;
}

/// `@@ -a[,b] +c[,d] @@ [节标题]` 解析。返回 false = 不是合法的 hunk 头。
[[nodiscard]] auto parse_header(std::string_view line, DiffHunk& out) -> bool {
  if (!line.starts_with("@@ ")) return false;
  std::size_t pos = 3;
  const auto number = [&line, &pos]() -> std::optional<std::size_t> {
    std::size_t value = 0;
    bool any = false;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
      value = value * 10 + static_cast<std::size_t>(line[pos] - '0');
      any = true;
      ++pos;
    }
    return any ? std::optional<std::size_t>(value) : std::nullopt;
  };
  const auto sign = [&line, &pos](char want) -> bool {
    if (pos < line.size() && line[pos] == want) {
      ++pos;
      return true;
    }
    return false;
  };
  if (!sign('-')) return false;
  const auto old_start = number();
  if (!old_start) return false;
  std::size_t old_count = 1;   // git 省略 `,1`
  if (pos < line.size() && line[pos] == ',') {
    ++pos;
    const auto parsed = number();
    if (!parsed) return false;
    old_count = *parsed;
  }
  if (!sign(' ') || !sign('+')) return false;
  const auto new_start = number();
  if (!new_start) return false;
  std::size_t new_count = 1;
  if (pos < line.size() && line[pos] == ',') {
    ++pos;
    const auto parsed = number();
    if (!parsed) return false;
    new_count = *parsed;
  }
  // `@@ … @@` 的收尾：先跳过计数与 `@@` 之间的空格，再吃 `@@`。
  // 这里曾写 `line.substr(pos, 2) != " @"` —— 它匹配的是**空格 + 第一个 @**，
  // 于是 `pos` 停在第二个 `@` 上，节标题多带一个 `@`（实测：`@ void f() {`）。
  while (pos < line.size() && line[pos] == ' ') ++pos;
  if (pos + 1 >= line.size() || line[pos] != '@' || line[pos + 1] != '@') return false;
  pos += 2;
  if (pos < line.size() && line[pos] == ' ') ++pos;
  out.old_start = *old_start;
  out.old_count = old_count;
  out.new_start = *new_start;
  out.new_count = new_count;
  out.section = std::string(line.substr(std::min(pos, line.size())));
  return true;
}

/// `--- a/x` / `+++ b/y` 的路径部分（丢掉 `\t` 后的时间戳；`/dev/null` → 空串）。
[[nodiscard]] auto header_path(std::string_view tail) -> std::string {
  const std::size_t tab = tail.find('\t');
  std::string_view path = tab == std::string_view::npos ? tail : tail.substr(0, tab);
  while (!path.empty() && path.back() == ' ') path.remove_suffix(1);
  if (path == "/dev/null") return {};
  if (path.size() > 2 && (path[0] == 'a' || path[0] == 'b') && path[1] == '/') {
    path = path.substr(2);
  }
  return std::string(path);
}

/// `diff --git a/x b/y` → (x, y)。路径含空格时可能切错——此时后面的
/// `---`/`+++`/`rename from|to` 会把它纠正过来（那几行是权威）。
[[nodiscard]] auto git_header_paths(std::string_view line) -> std::pair<std::string, std::string> {
  const std::string_view rest = line.substr(std::string_view("diff --git ").size());
  const std::size_t split = rest.find(" b/");
  if (split == std::string_view::npos) return {};
  return {header_path(rest.substr(0, split)), header_path(rest.substr(split + 1))};
}

}  // namespace

auto parse_unified(std::string_view patch) -> DiffDocument {
  DiffDocument document;
  const std::vector<std::string_view> lines = patch_lines(patch);

  FileDiff pending;
  bool has_pending = false;
  bool in_hunk = false;
  DiffHunk current;
  std::size_t consumed_old = 0;
  std::size_t consumed_new = 0;

  // 收尾当前文件：统计增删并入列。
  const auto flush_file = [&]() {
    if (!has_pending) return;
    const auto [additions, deletions] = count_changes(pending.hunks);
    pending.additions = additions;
    pending.deletions = deletions;
    document.additions += additions;
    document.deletions += deletions;
    document.binary = document.binary || pending.binary;
    document.files.push_back(std::move(pending));
    pending = FileDiff{};
    has_pending = false;
  };
  const auto start_file = [&]() {
    flush_file();
    pending = FileDiff{};
    has_pending = true;
  };
  const auto finish_hunk = [&]() {
    if (!in_hunk) return;
    in_hunk = false;
    if (!has_pending) return;
    // hunk 头声明与实际行数不符 = patch 结构异常（被截断、或不是我们认识的方言）。
    // **如实记进 warnings 并按实际行收录**——静默忽略会显示出一个"看着正常但少几行"的差异。
    if (consumed_old != current.old_count || consumed_new != current.new_count) {
      document.warnings.push_back(
          std::format("hunk @@ -{},{} +{},{} @@ 声明 {} / {} 行，实际收到 {} / {} 行",
                      current.old_start, current.old_count, current.new_start, current.new_count,
                      current.old_count, current.new_count, consumed_old, consumed_new));
    }
    pending.hunks.push_back(std::move(current));
    current = DiffHunk{};
  };
  // 往当前 hunk 追加一条正文行。行号由 `hunk 头起始行号 + 已收行数` 推出——
  // **不重数**：重数会在 `\ No newline` 与空行上出现 ±1（这是行号三种坐标里最容易错的一处）。
  const auto push_line = [&](DiffLineKind kind, std::string_view text) {
    DiffLine line;
    line.kind = kind;
    line.text = std::string(text);
    if (kind != DiffLineKind::Added) {
      line.old_line = current.old_start + consumed_old;
      ++consumed_old;
    }
    if (kind != DiffLineKind::Removed) {
      line.new_line = current.new_start + consumed_new;
      ++consumed_new;
    }
    current.lines.push_back(std::move(line));
  };

  for (const std::string_view line : lines) {
    const char prefix = line.empty() ? '\0' : line.front();
    const std::string_view body = line.empty() ? std::string_view{} : line.substr(1);

    if (in_hunk) {
      // hunk 正文**只看前缀字符**，绝不按内容判头部：hunk 里一行内容完全可能是
      // `-- 注释`（在 patch 文本里就是 `--- 注释`），按内容判会把正文吃掉。
      if (prefix == '+') {
        push_line(DiffLineKind::Added, body);
        continue;
      }
      if (prefix == '-') {
        push_line(DiffLineKind::Removed, body);
        continue;
      }
      if (prefix == ' ') {
        push_line(DiffLineKind::Context, body);
        continue;
      }
      if (prefix == '\\') {
        // `\ No newline at end of file`：标记**上一行**。
        if (!current.lines.empty()) current.lines.back().no_newline = true;
        continue;
      }
      finish_hunk();   // 其余前缀 = 本 hunk 结束；这一行接着按头部规则重新处理
    }

    if (line.empty()) continue;

    if (line.starts_with("diff --git ")) {
      start_file();
      const auto [old_path, new_path] = git_header_paths(line);
      pending.old_path = old_path;
      pending.new_path = new_path;
      continue;
    }
    if (line.starts_with("--- ")) {
      if (!has_pending) start_file();
      pending.old_path = header_path(line.substr(4));
      continue;
    }
    if (line.starts_with("+++ ")) {
      if (!has_pending) start_file();
      pending.new_path = header_path(line.substr(4));
      continue;
    }
    if (line.starts_with("@@ ")) {
      if (!has_pending) start_file();
      DiffHunk parsed;
      if (parse_header(line, parsed)) {
        current = std::move(parsed);
        consumed_old = 0;
        consumed_new = 0;
        in_hunk = true;
      }
      continue;
    }
    if (!has_pending) continue;   // 文件头之前的杂项（如邮件头）忽略

    if (line.starts_with("new file mode ")) {
      pending.added = true;
      continue;
    }
    if (line.starts_with("deleted file mode ")) {
      pending.deleted = true;
      continue;
    }
    if (line.starts_with("rename from ")) {
      pending.renamed = true;
      pending.old_path = header_path(line.substr(12));
      continue;
    }
    if (line.starts_with("rename to ")) {
      pending.renamed = true;
      pending.new_path = header_path(line.substr(10));
      continue;
    }
    if (line.starts_with("copy from ") || line.starts_with("copy to ")) continue;
    // 二进制：git 有两种提示（`Binary files a/x and b/y differ` 与 `GIT binary patch`）。
    if (line.starts_with("Binary files ") || line.starts_with("GIT binary patch")) {
      pending.binary = true;
      continue;
    }
    // 其余元信息（`index …`、`old mode …`、`similarity index …`、`diff --cc` 等）不参与结构。
  }
  finish_hunk();
  flush_file();
  return document;
}

namespace {

/// `@@ -a,b +c,d @@[ 节标题]`。
[[nodiscard]] auto header_text(const DiffHunk& hunk) -> std::string {
  return std::format("@@ -{},{} +{},{} @@{}", hunk.old_start, hunk.old_count, hunk.new_start,
                     hunk.new_count,
                     hunk.section.empty() ? std::string{} : " " + hunk.section);
}

/// 该侧最后一条有行号的行是不是「无末行换行」。
[[nodiscard]] auto side_ends_without_newline(const DiffHunk& hunk, bool old_side) -> bool {
  for (auto it = hunk.lines.rbegin(); it != hunk.lines.rend(); ++it) {
    if ((old_side ? it->old_line : it->new_line) != 0) return it->no_newline;
  }
  return false;
}

void append_hunk_lines(std::string& out, const DiffHunk& hunk) {
  const bool old_marked = side_ends_without_newline(hunk, true);
  const bool new_marked = side_ends_without_newline(hunk, false);
  for (const DiffLine& line : hunk.lines) {
    switch (line.kind) {
      case DiffLineKind::Context: out += ' '; break;
      case DiffLineKind::Removed: out += '-'; break;
      case DiffLineKind::Added: out += '+'; break;
    }
    out += line.text;
    out += '\n';
    // 标记的位置与 git 一致：紧跟在**那一侧的末行**之后；同一行两侧都无换行时出两条。
    if (old_marked && line.no_newline && line.old_line != 0 && line.kind != DiffLineKind::Added) {
      out += "\\ No newline at end of file\n";
    }
    if (new_marked && line.no_newline && line.new_line != 0 && line.kind == DiffLineKind::Added) {
      out += "\\ No newline at end of file\n";
    }
    if (new_marked && line.no_newline && line.new_line != 0 &&
        line.kind == DiffLineKind::Context) {
      // 两侧都无末行换行的**相同**末行（内容一致、都缺换行）：只需在最后标一次。
      if (&line == &hunk.lines.back()) out += "\\ No newline at end of file\n";
    }
  }
}

}  // namespace

auto to_unified(const DiffHunk& hunk) -> std::string {
  std::string out = header_text(hunk);
  out += '\n';
  append_hunk_lines(out, hunk);
  return out;
}

auto to_unified(const FileDiff& file) -> std::string {
  const std::string new_path = file.new_path.empty() ? file.old_path : file.new_path;
  if (file.binary) {
    return std::format("Binary files a/{} and b/{} differ\n", file.old_path, new_path);
  }
  std::string out = std::format("diff --git a/{} b/{}\n", file.old_path, new_path);
  if (file.added) out += "new file mode 100644\n";
  if (file.renamed) out += std::format("rename from {}\nrename to {}\n", file.old_path, new_path);
  if (file.deleted) out += "deleted file mode 100644\n";
  out += std::format("--- {}\n", file.added ? "/dev/null" : "a/" + file.old_path);
  out += std::format("+++ {}\n", file.deleted ? "/dev/null" : "b/" + new_path);
  for (const DiffHunk& hunk : file.hunks) {
    out += header_text(hunk);
    out += '\n';
    append_hunk_lines(out, hunk);
  }
  return out;
}

auto build_partial_patch(const DiffHunk& hunk, const std::vector<std::size_t>& selected,
                         std::string_view header) -> std::string {
  if (selected.empty()) return {};
  std::vector<bool> chosen(hunk.lines.size(), false);
  for (const std::size_t index : selected) {
    if (index < chosen.size()) chosen[index] = true;
  }

  // 选中的行 → 保留原类型；未选中的 → **降级为上下文**（只对删除行；新增行直接丢）。
  //
  // 为何删除行要降级而不是丢掉：它在**旧侧**仍然存在（这次不删而已），
  // 丢掉它会让新旧两侧的行数对不上，git apply 会以「补丁不一致」为由拒绝。
  std::vector<DiffLine> kept;
  kept.reserve(hunk.lines.size());
  std::size_t kept_added = 0;
  std::size_t kept_removed = 0;
  for (std::size_t index = 0; index < hunk.lines.size(); ++index) {
    const DiffLine& line = hunk.lines[index];
    if (line.kind == DiffLineKind::Context) {
      kept.push_back(line);
    } else if (line.kind == DiffLineKind::Removed) {
      if (chosen[index]) {
        kept.push_back(line);
        ++kept_removed;
      } else {
        DiffLine context = line;
        context.kind = DiffLineKind::Context;
        kept.push_back(std::move(context));
      }
    } else {   // Added
      if (chosen[index]) {
        kept.push_back(line);
        ++kept_added;
      }
      // 未选中的新增行：丢掉（它本来就不存在于旧侧，无所谓“降级”）。
    }
  }

  // 一行改动都没选中：不硬造空 hunk（`git apply` 会报一个与用户意图无关的错）。
  if (kept_added == 0 && kept_removed == 0) return {};

  // 选中改动所在的行区间：头里的行号与计数必须**只覆盖这一段**，
  // 否则 git 会把它往错的位置贴（那是最难查的一类缺陷：补丁应用成功但内容错位）。
  std::size_t first_old = 0;
  std::size_t first_new = 0;
  bool have_first = false;
  for (const DiffLine& line : kept) {
    if (line.kind == DiffLineKind::Added) {
      if (!have_first) {
        // 纯新增起始：旧侧没有行号，锚在**当前旧行数之后**（即 `+N,0` 的语义）。
        have_first = true;
      }
      continue;
    }
    if (!have_first) {
      first_old = line.old_line;
      first_new = line.new_line;
      have_first = true;
      break;
    }
  }
  if (!have_first) {
    first_old = hunk.old_start;
    first_new = hunk.new_start;
  } else if (first_old == 0) {
    first_old = hunk.old_start;
  }
  if (first_new == 0) first_new = hunk.new_start;

  DiffHunk partial;
  partial.old_start = first_old;
  partial.new_start = first_new;
  partial.old_count = 0;
  partial.new_count = 0;
  for (const DiffLine& line : kept) {
    if (line.kind != DiffLineKind::Added) ++partial.old_count;
    if (line.kind != DiffLineKind::Removed) ++partial.new_count;
  }
  partial.section = hunk.section;
  partial.lines = std::move(kept);

  std::string out;
  if (!header.empty()) {
    out += header;
    if (!out.ends_with('\n')) out += '\n';
  }
  out += header_text(partial);
  out += '\n';
  append_hunk_lines(out, partial);
  return out;
}

auto build_partial_patch(
    const FileDiff& file,
    const std::vector<std::pair<std::size_t, std::vector<std::size_t>>>& selected)
    -> std::string {
  const std::string new_path = file.new_path.empty() ? file.old_path : file.new_path;
  if (file.binary) return {};   // 二进制没有“逐行选择”这回事
  const std::string header = std::format(
      "diff --git a/{} b/{}\n--- {}\n+++ {}\n", file.old_path, new_path,
      file.added ? "/dev/null" : "a/" + file.old_path,
      file.deleted ? "/dev/null" : "b/" + new_path);
  std::string out;
  // 按 hunk 下标升序输出（补丁里 hunk 的顺序必须与文件一致，否则 git 会拒）。
  std::vector<std::size_t> order;
  order.reserve(selected.size());
  for (const auto& [index, _] : selected) {
    if (index < file.hunks.size()) order.push_back(index);
  }
  std::ranges::sort(order);
  order.erase(std::unique(order.begin(), order.end()), order.end());
  for (const std::size_t index : order) {
    const auto found = std::ranges::find_if(
        selected, [index](const auto& item) { return item.first == index; });
    if (found == selected.end()) continue;
    // 只有第一个 hunk 带文件头（多个头会让 git 以为这是多个文件）。
    out += build_partial_patch(file.hunks[index], found->second, out.empty() ? header : "");
  }
  return out;
}

}  // namespace st::text
