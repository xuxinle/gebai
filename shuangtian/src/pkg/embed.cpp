#include "st/pkg/embed.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <map>
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
/// 与上游 CMake 的 `configure_file` 语义一致：全量替换，未命中的占位符原样保留
/// （保留而非清空——生成结果里若残留 `${...}` 会直接编译失败，比静默生成空内容更早暴露问题）。
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
/// 后续所有十六进制字符——相邻字节恰好都是十六进制字符时（如 `0x12 0x34` 写成 `\x12\x34`
/// 没问题，但若拼接边界处理不慎写成 `\x1234`）就会被解析成一个越界值。
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

/// 读取模板文件（third_party/battery/*.in）。
[[nodiscard]] auto load_template(std::string_view path) -> Result<std::string> {
  auto text = fs::read_text(path);
  if (!text) {
    return unexpected(ErrorCode::NotFound,
                      std::format("嵌入模板缺失: {}（third_party/battery/ 应随仓库分发）", path));
  }
  return text;
}

/// 从模板 + 判定结果渲染单个资源的 `.cpp`。
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
/// 返回链的**顺序敏感**：`if constexpr` 链按文件路径排序生成，保证同一份清单两次构建
/// 产出逐字节相同的头（可复现构建 + 不会造成无谓的全局重编）。
[[nodiscard]] auto render_header(const std::string& template_text,
                                 const std::vector<std::pair<std::string, std::string>>& entries)
    -> std::string {
  std::string declarations;
  std::string returns;
  for (const auto& [identifier, filename] : entries) {
    declarations.append("static EmbeddedFile ").append(identifier).append(";\n        ");
    returns.append("if constexpr (identifier == \"").append(filename).append("\") { return EmbedInternal::")
        .append(identifier)
        .append("; }\n        else ");
  }
  std::string out = template_text;
  replace_all(out, "${EMBEDDED_FILES_DECLARATIONS}", declarations);
  replace_all(out, "${EMBEDDED_FILES_RETURNS}", returns);
  return out;
}

/// 目录不存在就建（含父级）。
[[nodiscard]] auto ensure_directory(std::string_view path) -> Status {
  return fs::create_directories(path);
}

}  // namespace

auto embed_identifier(std::string_view target, std::string_view relative_path) -> std::string {
  std::string out;
  out.reserve(target.size() + relative_path.size() + 1);
  out.append(target).push_back('_');
  out.append(relative_path);
  for (char& raw : out) {
    const auto value = static_cast<unsigned char>(raw);
    const bool keeps = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                       (value >= '0' && value <= '9') || value == '_';
    raw = keeps ? static_cast<char>(std::tolower(value)) : '_';
  }
  // 上游会把 `assets/banner.txt` → `assets_banner_txt`；这里同样把分隔点折成下划线
  return out;
}

auto generate_embeds(const Manifest& manifest, std::string_view target,
                     std::string_view profile, std::string_view build_subdir, bool with_runtime)
    -> Result<EmbedOutput> {
  EmbedOutput output;
  if (manifest.directory.empty()) {
    return unexpected(ErrorCode::Invalid, "清单缺少 directory，无法生成嵌入资源");
  }
  // `target` 为空 = 工程级（库）嵌入：用 `manifest.embed`，标识符前缀取工程名。
  // 非空则查目标：目标不存在或未声明嵌入都视为"无需嵌入"（正常状态，不是错误）。
  const std::string scope(target);
  const std::vector<std::string>* patterns = nullptr;
  std::string identifier_prefix;
  if (scope.empty()) {
    patterns = &manifest.embed;
    identifier_prefix = manifest.name;
  } else {
    const TargetSpec* spec = manifest.find_target(scope);
    if (spec == nullptr) return output;
    patterns = &spec->embed;
    identifier_prefix = scope;
  }
  if (patterns->empty()) return output;

  // 1) 展开 glob → 相对路径（保持使用者书写形态，它同时是 `b::embed<"...">()` 的查找键）
  std::vector<std::string> relatives;
  for (const auto& pattern : *patterns) {
    const auto matched = fs::expand_glob(manifest.directory, pattern);
    if (!matched) return forward_error(matched.error());
    if (matched->empty()) {
      // 声明了却没命中：多半是路径写错。静默通过会让运行期 `b::embed<...>()` 编译失败且难以定位
      return unexpected(ErrorCode::NotFound,
                        std::format("目标 {} 的嵌入模式未命中任何文件: {}", target, pattern));
    }
    for (const auto& absolute : *matched) {
      relatives.push_back(fs::relative_to(absolute, manifest.directory));
    }
  }
  std::ranges::sort(relatives);
  relatives.erase(std::unique(relatives.begin(), relatives.end()), relatives.end());

  // 2) 标识符唯一性（上游同样会因重复而失败——重复即覆盖，静默覆盖会让资源悄悄串味）
  //
  // 两个方向的映射都要：`by_identifier` 用于查重（冲突时能报出"谁和谁撞了"），
  // `by_relative` 供后续按路径取标识符。只留一个方向就会写出"用错键查表"的错（曾因此崩在 map::at）。
  std::map<std::string, std::string> by_identifier;  // identifier → filename
  std::map<std::string, std::string> by_relative;    // filename → identifier
  for (const auto& relative : relatives) {
    const std::string identifier = embed_identifier(identifier_prefix, relative);
    if (const auto existing = by_identifier.find(identifier); existing != by_identifier.end()) {
      return unexpected(ErrorCode::Invalid,
                        std::format("嵌入标识符冲突: {} 与 {} 都映射到 {}", existing->second, relative,
                                    identifier));
    }
    by_identifier.emplace(identifier, relative);
    by_relative.emplace(relative, identifier);
  }

  // 3) 定位模板与输出目录
  const std::string scope_dir = scope.empty() ? std::string("lib") : scope;
  const std::string embed_root =
      fs::join(manifest.directory,
               std::format("build/{}/embed/{}", build_subdir.empty() ? profile : build_subdir,
                           scope_dir));
  const std::string include_dir = fs::join(embed_root, "include");
  const std::string source_dir = fs::join(embed_root, "src");
  const std::string header_dir = fs::join(include_dir, "battery");
  if (auto status = ensure_directory(header_dir); !status) return forward_error(status.error());
  if (auto status = ensure_directory(source_dir); !status) return forward_error(status.error());

  const std::string template_root = fs::join(manifest.directory, "third_party/battery");
  auto header_template = load_template(fs::join(template_root, "embed.hpp.in"));
  if (!header_template) return forward_error(header_template.error());
  auto source_template = load_template(fs::join(template_root, "embed_source.cpp.in"));
  if (!source_template) return forward_error(source_template.error());

  // 4) 逐文件生成（增量：源文件比产物新才重写）
  const bool production = profile == "release";
  std::vector<std::pair<std::string, std::string>> entries;
  for (const auto& relative : relatives) {
    const std::string absolute = fs::join(manifest.directory, relative);
    auto bytes = fs::read_bytes(absolute);
    if (!bytes) return forward_error(bytes.error());
    const auto mapped = by_relative.find(relative);
    if (mapped == by_relative.end()) continue;  // 理论上不可达；不变量被破坏时跳过而非崩溃
    const std::string identifier = mapped->second;
    entries.emplace_back(identifier, relative);

    const std::string target_path = fs::join(source_dir, identifier + ".cpp");
    output.sources.push_back(target_path);
    const bool stale = [&]() {
      const auto generated_time = fs::modified_ns(target_path);
      const auto source_time = fs::modified_ns(absolute);
      if (!generated_time.has_value() || !source_time.has_value()) return true;
      return *source_time > *generated_time;
    }();
    if (!stale) continue;
    // 渲染时需要"字节内容"（生成数组字面量）与"字节数"（写进注释）：
  // 直接传 `std::vector<std::uint8_t>`，不必把它 reinterpret_cast 成字符视图。
  std::string content = render_source(*source_template, identifier, relative, absolute, bytes->size(),
                                      byte_array_literal(*bytes));
    if (auto status = fs::write_text(target_path, content); !status) return forward_error(status.error());
    ++output.regenerated;
  }

  // 5) 声明头：内容指纹变了才重写（它被每个翻译单元包含，无谓重写会引发全量重编）
  const std::string header_path = fs::join(header_dir, "embed.hpp");
  std::string rendered_header = render_header(*header_template, entries);
  if (production) {
    // 生产模式：不携带构建机绝对路径（体积更小、不泄露环境）
    rendered_header = std::string("#ifndef B_PRODUCTION_MODE\n#define B_PRODUCTION_MODE 1\n#endif\n") +
                      rendered_header;
  }
  const bool header_changed = [&]() {
    const auto existing = fs::read_text(header_path);
    return !existing.has_value() || *existing != rendered_header;
  }();
  if (header_changed) {
    if (auto status = fs::write_text(header_path, rendered_header); !status) {
      return forward_error(status.error());
    }
    ++output.regenerated;
  }

  // 6) 运行时实现（上游的热重载部分）随目标编译：它 `#include "battery/embed.hpp"`，
  //    而那个头是**按目标生成**的（声明集合不同），所以不能放进共享库对象目录。
  const std::string runtime = fs::join(manifest.directory, "third_party/battery/embed_impl.cpp");
  if (!fs::is_regular_file(runtime)) {
    return unexpected(ErrorCode::NotFound,
                      std::format("battery::embed 运行时缺失: {}（third_party/ 应随仓库分发）", runtime));
  }
  if (with_runtime) output.sources.push_back(runtime);

  output.include_dir = include_dir;
  output.file_count = entries.size();
  if (output.regenerated > 0) {
    log::info("嵌入资源 [{}]: {} 个文件（本次重生成 {}）", scope_dir, output.file_count,
              output.regenerated);
  }
  return output;
}

}  // namespace st::pkg
