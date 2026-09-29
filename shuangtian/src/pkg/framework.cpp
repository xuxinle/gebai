#include "st/pkg/framework.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"

namespace st::pkg {
namespace {

/// 工具链源（`src/pkg/*`）：引用方不需要——它们是 `st` 自身的实现。
[[nodiscard]] auto is_toolchain_source(std::string_view relative) noexcept -> bool {
  return relative.starts_with("src/pkg/") || relative.starts_with("src/pkg\\");
}

}  // namespace

auto resolve_spec_path(std::string_view base, std::string_view path) -> std::string {
  if (path.empty()) return {};
  if (fs::is_absolute(path)) return fs::normalize(path);
  return fs::normalize(fs::join(base, path));
}

auto load_framework(const FrameworkSpec& spec) -> Result<Framework> {
  Framework framework;
  framework.directory = spec.directory.empty() ? fs::normalize(spec.path) : spec.directory;
  if (framework.directory.empty()) {
    return unexpected(ErrorCode::Invalid, "framework.path 为空");
  }
  const std::string manifest_path = fs::join(framework.directory, "st.pkg");
  if (!fs::is_regular_file(manifest_path)) {
    return unexpected(ErrorCode::NotFound,
                      std::format("框架路径下没有 st.pkg: {}（framework.path 应指向霜天框架根目录）",
                                  framework.directory));
  }
  auto manifest = Manifest::load(manifest_path);
  if (!manifest) return forward_error(manifest.error());
  framework.name = manifest->name;
  framework.version = manifest->version;

  // 包含目录：清单声明的 + 清单目录自身（项目内相对包含习惯）
  for (const auto& dir : manifest->include_dirs) {
    framework.include_dirs.push_back(resolve_spec_path(framework.directory, dir));
  }

  const bool inherit = spec.inherit_flags;
  if (inherit) {
    framework.flags = manifest->flags;
    framework.c_flags = manifest->c_flags;
    framework.defines = manifest->defines;
  }
  framework.system_libs = manifest->system_libs;
  framework.embed = manifest->embed;
  framework.toolchains = manifest->toolchains;

  // 第三方 C 源（QuickJS 等）：展开 glob 成绝对路径，交由构建当"第三方"对待
  auto third_party = manifest->third_party_files();
  if (!third_party) return forward_error(third_party.error());
  for (const auto& file : *third_party) {
    framework.third_party_sources.push_back(fs::is_absolute(file) ? file : fs::join(framework.directory, file));
  }

  // 框架源：展开 glob 后排除工具链源与测试
  auto sources = manifest->source_files();
  if (!sources) return forward_error(sources.error());
  for (const auto& file : *sources) {
    const std::string absolute = fs::is_absolute(file) ? file : fs::join(framework.directory, file);
    if (is_toolchain_source(fs::relative_to(absolute, framework.directory))) continue;
    framework.sources.push_back(absolute);
  }
  std::ranges::sort(framework.sources);
  framework.sources.erase(std::unique(framework.sources.begin(), framework.sources.end()),
                          framework.sources.end());
  if (framework.sources.empty()) {
    return unexpected(ErrorCode::Invalid,
                      std::format("框架 {} 没有可并入的源（st.pkg 的 sources 是否为空？）",
                                  framework.directory));
  }

  framework.embed_support_directory = fs::join(framework.directory, "third_party/battery");
  log::info("已引用框架 {} v{}（{} 源文件 / {} 第三方源 / {} 包含目录）", framework.name,
            framework.version, framework.sources.size(), framework.third_party_sources.size(),
            framework.include_dirs.size());
  return framework;
}

}  // namespace st::pkg
