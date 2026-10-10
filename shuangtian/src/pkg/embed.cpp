#include "st/pkg/embed.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/string.hpp"

namespace st::pkg {
namespace {

/// 模板里的占位符（`${NAME}`）替换。
///
/// 与上游 CMake 的 `configure_file` 语义一致：全量替换；未命中的占位符原样保留
/// （残留 `${...}` 会让生成结果直接编译失败，比静默生成空内容更早暴露问题）。
void replace_all(std::string& text, std::string_view placeholder, std::string_view value) {
  std::size_t position = 0;
  while ((position = text.find(placeholder, position)) != std::string::npos) {
    text.replace(position, placeholder.size(), value);
    position += value.size();
  }
}

/// 生成 C++ 字节数组字面量。
///
/// 用**八进制转义**（`\NNN`，固定三位）而不是十六进制（`\xNN`）：C++ 的 `\x` 会**贪婪吃掉**
/// 后续所有十六进制字符——相邻字节恰好都是十六进制字符时会被解析成一个越界值。
/// 八进制恰好三位、不多不少，从语法上不可能歧义。
[[nodiscard]] auto byte_array_literal(const std::vector<std::uint8_t>& bytes) -> std::string {
  std::string out;
  out.reserve(bytes.size() * 5 + 16);
  constexpr std::size_t kPerLine = 16;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index % kPerLine == 0) out.append("\n            \"");
    out.push_back('\\');
    const auto value = static_cast<unsigned>(bytes[index]);
    out.push_back(static_cast<char>('0' + ((value >> 6U) & 7U)));
    out.push_back(static_cast<char>('0' + ((value >> 3U) & 7U)));
    out.push_back(static_cast<char>('0' + (value & 7U)));
    if (index % kPerLine == kPerLine - 1) out.push_back('"');
  }
  if (!bytes.empty() && bytes.size() % kPerLine != 0) out.push_back('"');
  return out;
}

[[nodiscard]] auto load_template(std::string_view path) -> Result<std::string> {
  auto text = fs::read_text(path);
  if (!text) {
    return unexpected(ErrorCode::NotFound,
                      std::format("嵌入模板缺失: {}（third_party/battery/ 应随框架分发）", path));
  }
  return text;
}

[[nodiscard]] auto render_source(const std::string& template_text, std::string_view identifier,
                                 std::string_view filename, std::string_view full_path,
                                 std::size_t byte_count, std::string array_literal) -> std::string {
  std::string out = template_text;
  replace_all(out, "${IDENTIFIER}", identifier);
  replace_all(out, "${FILENAME}", filename);
  replace_all(out, "${FULL_PATH}", full_path);
  replace_all(out, "${FILESIZE}", std::to_string(byte_count));
  replace_all(out, "${GENERATED_BYTE_ARRAY}", array_literal);
  return out;
}

/// 渲染声明头：填 `${EMBEDDED_FILES_DECLARATIONS}` 与 `${EMBEDDED_FILES_RETURNS}`。
///
/// 返回链按**查找键排序**生成：同一份清单两次构建产出逐字节相同的头（可复现构建，
/// 也避免无谓的全局重编）。
[[nodiscard]] auto render_header(const std::string& template_text,
                                 const std::vector<std::pair<std::string, std::string>>& entries)
    -> std::string {
  std::string declarations;
  std::string returns;
  for (const auto& [identifier, filename] : entries) {
    declarations.append("static EmbeddedFile ").append(identifier).append(";\n        ");
    returns.append("if constexpr (identifier == \"")
        .append(filename)
        .append("\") { return EmbedInternal::")
        .append(identifier)
        .append("; }\n        else ");
  }
  std::string out = template_text;
  replace_all(out, "${EMBEDDED_FILES_DECLARATIONS}", declarations);
  replace_all(out, "${EMBEDDED_FILES_RETURNS}", returns);
  return out;
}

[[nodiscard]] auto ensure_directory(std::string_view path) -> Status {
  return fs::create_directories(path);
}

/// 一个待生成的资源（跨贡献方汇总后按查找键排序）。
struct PendingEntry {
  std::string identifier{};
  std::string lookup_key{};  ///< `b::embed<"...">()` 里书写的相对路径原文
  std::string absolute{};
  std::string origin{};
};

}  // namespace

auto embed_identifier(std::string_view prefix, std::string_view relative_path) -> std::string {
  std::string out;
  out.reserve(prefix.size() + relative_path.size() + 1);
  out.append(prefix).push_back('_');
  out.append(relative_path);
  for (char& raw : out) {
    const auto value = static_cast<unsigned char>(raw);
    const bool keeps = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                       (value >= '0' && value <= '9') || value == '_';
    raw = keeps ? static_cast<char>(std::tolower(value)) : '_';
  }
  return out;
}

auto generate_embeds(const EmbedRequest& request) -> Result<EmbedOutput> {
  EmbedOutput output;
  if (request.work_directory.empty()) {
    return unexpected(ErrorCode::Invalid, "嵌入生成缺少工程目录");
  }

  // 1) 汇总所有贡献方：展开 glob → 待生成列表
  std::vector<PendingEntry> pending;
  for (const auto& contribution : request.contributions) {
    if (contribution.patterns.empty()) continue;
    const std::string origin =
        contribution.origin.empty() ? contribution.identifier_prefix : contribution.origin;
    for (const auto& pattern : contribution.patterns) {
      const auto matched = fs::expand_glob(contribution.base_directory, pattern);
      if (!matched) return forward_error(matched.error());
      if (matched->empty()) {
        // 声明了却没命中：多半是路径写错。静默通过会让运行期 `b::embed<...>()` 编译失败且难定位
        return unexpected(ErrorCode::NotFound,
                          std::format("{} 的嵌入模式未命中任何文件: {}", origin, pattern));
      }
      for (const auto& relative : *matched) {
        PendingEntry entry;
        entry.lookup_key = relative;
        entry.identifier = embed_identifier(contribution.identifier_prefix, relative);
        entry.absolute = fs::join(contribution.base_directory, relative);
        entry.origin = origin;
        pending.push_back(std::move(entry));
      }
    }
  }
  if (pending.empty()) return output;  // 没有要嵌的资源：正常状态

  // 2) 标识符唯一性 + 查找键唯一性
  //    标识符重复 = 生成的符号会互撞；查找键重复 = 同一把钥匙指向两份内容。
  //    两者都必须报错，否则表现为"嵌入的资源串味了"这种极难查的问题。
  std::ranges::sort(pending, [](const PendingEntry& left, const PendingEntry& right) {
    return left.lookup_key < right.lookup_key;
  });
  for (std::size_t index = 1; index < pending.size(); ++index) {
    if (pending[index].lookup_key == pending[index - 1].lookup_key) {
      return unexpected(ErrorCode::Invalid,
                        std::format("嵌入查找键重复: {}（来自 {} 与 {}）", pending[index].lookup_key,
                                    pending[index - 1].origin, pending[index].origin));
    }
  }
  {
    std::map<std::string, std::string> seen;  // identifier → lookup_key
    for (const auto& entry : pending) {
      if (const auto found = seen.find(entry.identifier); found != seen.end()) {
        return unexpected(ErrorCode::Invalid,
                          std::format("嵌入标识符冲突: {} 与 {} 都映射到 {}",
                                      found->second, entry.lookup_key, entry.identifier));
      }
      seen.emplace(entry.identifier, entry.lookup_key);
    }
  }

  // 3) 模板与输出目录
  const std::string embed_root = fs::join(
      request.work_directory, std::format("build/{}/embed/{}", request.build_subdir, request.scope));
  const std::string include_dir = fs::join(embed_root, "include");
  const std::string header_dir = fs::join(include_dir, "battery");
  const std::string source_dir = fs::join(embed_root, "src");
  if (auto status = ensure_directory(header_dir); !status) return forward_error(status.error());
  if (auto status = ensure_directory(source_dir); !status) return forward_error(status.error());

  auto header_template = load_template(fs::join(request.template_directory, "embed.hpp.in"));
  if (!header_template) return forward_error(header_template.error());
  auto source_template = load_template(fs::join(request.template_directory, "embed_source.cpp.in"));
  if (!source_template) return forward_error(source_template.error());

  // 4) 逐资源生成（增量：源文件比产物新才重写）
  std::vector<std::pair<std::string, std::string>> entries;  // identifier → lookup_key
  for (const auto& entry : pending) {
    auto bytes = fs::read_bytes(entry.absolute);
    if (!bytes) return forward_error(bytes.error());
    entries.emplace_back(entry.identifier, entry.lookup_key);

    const std::string target_path = fs::join(source_dir, entry.identifier + ".cpp");
    output.sources.push_back(target_path);
    const bool stale = [&]() {
      const auto generated_time = fs::modified_ns(target_path);
      const auto source_time = fs::modified_ns(entry.absolute);
      if (!generated_time.has_value() || !source_time.has_value()) return true;
      return *source_time > *generated_time;
    }();
    if (!stale) continue;
    std::string content = render_source(*source_template, entry.identifier, entry.lookup_key,
                                       entry.absolute, bytes->size(),
                                       byte_array_literal(*bytes));
    if (auto status = fs::write_text(target_path, content); !status) {
      return forward_error(status.error());
    }
    ++output.regenerated;
  }

  // 5) 声明头：渲染结果变了才重写（它被每个翻译单元包含，无谓重写会引发全量重编）
  const std::string header_path = fs::join(header_dir, "embed.hpp");
  std::string rendered = render_header(*header_template, entries);
  if (request.production) {
    // 生产模式：不携带构建机绝对路径（体积更小、不泄露环境），热重载随之关闭
    rendered = std::string("#ifndef B_PRODUCTION_MODE\n#define B_PRODUCTION_MODE 1\n#endif\n") +
               rendered;
  }
  const auto existing = fs::read_text(header_path);
  if (!existing.has_value() || *existing != rendered) {
    if (auto status = fs::write_text(header_path, rendered); !status) {
      return forward_error(status.error());
    }
    ++output.regenerated;
  }

  // 6) 运行时实现（热重载）：**恰好一处**提供，避免同一可执行文件里出现两份进程级全局表
  if (request.with_runtime) {
    if (!fs::is_regular_file(request.runtime_source)) {
      return unexpected(
          ErrorCode::NotFound,
          std::format("battery::embed 运行时缺失: {}（third_party/battery/ 应随框架分发）",
                      request.runtime_source));
    }
    output.sources.push_back(request.runtime_source);
  }

  output.include_dir = include_dir;
  output.file_count = entries.size();
  if (output.regenerated > 0) {
    ST_LOG_INFO("嵌入资源 [{}]: {} 个文件（本次重生成 {}）", request.scope, output.file_count,
              output.regenerated);
  }
  return output;
}

}  // namespace st::pkg
