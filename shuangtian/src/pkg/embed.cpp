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
///
/// **空字节序列返回一个空字面量 `""`**（而不是空串）：模板把它填进
/// `std::string_view(${GENERATED_BYTE_ARRAY}, ${FILESIZE})`，空串会生成
/// `std::string_view(, 0)` 这种语法错误。空字面量是合法且语义准确的——
/// 长度 0 的字符串视图，指向一个合法的静态字符。
///
/// 空序列是**真实会发生的**：可选嵌入（`EmbedContribution::optional`）在资源缺失时
/// 就用零字节占位，目的正是让 `b::embed<"…">()` 仍能编译（而运行期解码失败后降级）。
/// 早期没处理这一支，实测症状是 34 个生成单元全部报
/// `expected primary-expression before '(' token`——报错位置在生成代码里，
/// 看上去像模板坏了而不是“资源缺失”。
[[nodiscard]] auto byte_array_literal(const std::vector<std::uint8_t>& bytes) -> std::string {
  if (bytes.empty()) return "\"\"";
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
  if (bytes.size() % kPerLine != 0) out.push_back('"');
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
  /// 可选嵌入里未命中的占位条目（零字节）：不读文件（它本就不存在）。
  /// 存在的意义只是让 `b::embed<"…">()` **仍能编译**，运行时解码失败后自然降级。
  bool placeholder{false};
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
  // 可选嵌入里未命中的字面路径（与上面 pending 分开：它们没有真实文件可读）
  std::vector<PendingEntry> placeholders;
  for (const auto& contribution : request.contributions) {
    if (contribution.patterns.empty()) continue;
    const std::string origin =
        contribution.origin.empty() ? contribution.identifier_prefix : contribution.origin;
    for (const auto& pattern : contribution.patterns) {
      const auto matched = fs::expand_glob(contribution.base_directory, pattern);
      if (!matched) return forward_error(matched.error());
      if (matched->empty()) {
        // 声明了却没命中：多半是路径写错。静默通过会让运行期 `b::embed<...>()` 编译失败且难定位。
        //
        // **可选嵌入例外**（见 `EmbedContribution::optional`）：这类资源由本地产线生成、
        // 因体积不入库，在别的机器上**必然**不存在。对它报错等于把「没素材」当成「清单写错」，
        // 而且连编译都过不了。改为生成**零字节占位**，让 `b::embed<"…">()` 仍能编译，
        // 运行期解码失败后走调用方已有的降级路径。
        if (!contribution.optional) {
          return unexpected(ErrorCode::NotFound,
                            std::format("{} 的嵌入模式未命中任何文件: {}", origin, pattern));
        }
        // 占位条目的 lookup_key 必须是**声明的字面路径**：
        // `b::embed<"apps/x/a.png">()` 按字符串查表，非 glob 形态（带 `*`）不能当键。
        // 带通配符的模式无法预知具体文件名（不知道用户写了哪些），跳过并记一笔——
        // 那种写法在缺文件的机器上本来就无从补齐；注释里列出的逐文件声明不受影响。
        if (pattern.find_first_of("*?") != std::string::npos) {
          ST_LOG_INFO("嵌入资源 [{}]: 可选模式 {} 未命中，无字面路径可占位（跳过）", origin,
                      pattern);
          continue;
        }
        PendingEntry entry;
        entry.lookup_key = pattern;
        entry.identifier = embed_identifier(contribution.identifier_prefix, pattern);
        entry.absolute = fs::join(contribution.base_directory, pattern);  // 不存在（占位用）
        entry.origin = origin;
        entry.placeholder = true;
        placeholders.push_back(std::move(entry));
        ST_LOG_INFO("嵌入资源 [{}]: 缺少 {}（可选，将生成零字节占位，运行期降级）", origin,
                    pattern);
        continue;
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
  // 占位条目并入（它们不参与内容读取，只贡献声明与一个空字节数组）。
  // 放在真实条目之后合并排序：下面的唯一性检查同时覆盖两者——
  // 若同一路径既被真实文件命中又被声明为占位，那是真·配置矛盾，必须报错。
  pending.insert(pending.end(), placeholders.begin(), placeholders.end());
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
std::size_t placeholders_used = 0;
for (const auto& entry : pending) {
// 占位条目（可选嵌入里未命中的字面路径）：**不去读盘**——文件本就不存在。
// 直接给零字节：`EmbeddedFile` 的 `length()==0 / data()==""`，
// 调用方的解码会失败并走降级路径（如 `CharacterPortrait::load` 返回 invalid）。
std::vector<std::uint8_t> bytes;
if (!entry.placeholder) {
  auto read = fs::read_bytes(entry.absolute);
  if (!read) return forward_error(read.error());
  bytes = std::move(*read);
} else {
  ++placeholders_used;
}
entries.emplace_back(entry.identifier, entry.lookup_key);

const std::string target_path = fs::join(source_dir, entry.identifier + ".cpp");
output.sources.push_back(target_path);
std::string content = render_source(*source_template, entry.identifier, entry.lookup_key,
entry.absolute, bytes.size(),
byte_array_literal(bytes));
    // 按内容判变：重写成同一份内容只会白刷一次时间戳（进而拉动下游重编）。
    if (const auto existing = fs::read_text(target_path);
        existing.has_value() && *existing == content) {
      continue;
    }
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
  output.placeholder_count = placeholders_used;
  if (output.regenerated > 0) {
    ST_LOG_INFO("嵌入资源 [{}]: {} 个文件（本次重生成 {}）", request.scope, output.file_count,
                output.regenerated);
  }
  // 缺资源必须**说出声**：它不阻断构建（可选嵌入的设计就是“缺了也能编”），
  // 但“缺了几张”不能只躺在 debug 日志里——否则机器 A 跑得正常、机器 B
  // 静默退回矢量画风，而两边都以为自己是正常的（同一个工程、两种观感，却无人知道为何）。
  if (placeholders_used > 0) {
    ST_LOG_WARN("嵌入资源 [{}]: {} 项缺失 → 已生成零字节占位（构建照常，运行时由调用方降级）",
                request.scope, placeholders_used);
  }
  return output;
}

}  // namespace st::pkg
