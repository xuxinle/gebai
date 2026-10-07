#include "st/pkg/compiler.hpp"

#include <algorithm>
#include <format>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"

namespace st::pkg {

auto compiler_kind_of(std::string_view compiler) -> Result<CompilerKind> {
  const std::string name = st::ascii_lower(fs::file_name(compiler));
  // `clang-cl` 是 MSVC 风味的 clang（`/std:c++20`、`/I`、`/WX`…）：本层不支持 MSVC 口径，
  // 误判成 Clang 会拿 `-std=c++20` 去喂它，报一堆与真正原因相隔很远的错。
  if (name.find("clang-cl") != std::string::npos) {
    return unexpected(ErrorCode::Unsupported,
                      std::format("不支持 clang-cl（MSVC 口径的工具链）: {}；"
                                  "请用 GCC 风格的 clang++（必要时配 --target）",
                                  compiler));
  }
  if (name.find("clang") != std::string::npos) return CompilerKind::Clang;
  // `g++` / `c++` / `x86_64-w64-mingw32-g++` 都是 GCC 系（含交叉编译器）
  return CompilerKind::Gcc;
}

auto compiler_kind_name(CompilerKind kind) -> std::string_view {
  switch (kind) {
    case CompilerKind::Gcc:
      return "gcc";
    case CompilerKind::Clang:
      return "clang";
  }
  return "unknown";
}

auto dialect_flags(CompilerKind kind, SourceLanguage language) -> std::vector<std::string> {
  (void)kind;
  (void)language;
  // GCC 与 Clang 的方言固定项为空：`-std=c++20` 由清单 `flags` 给出（两族拼法相同），
  // 编码无需干预（两族都按 UTF-8 读源码）。
  // 保留这个函数是因为"恒定标志"这个概念必须有一个落点——它是**跨平台清单**
  // 与**某族特例**之间的边界——将来若新增族（标志拼法不同的前端）只需在这里补分支。
  return {};
}

auto pch_consume_args(std::string_view directory, std::string_view header)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  const std::string_view separator =
      directory.ends_with('/') || directory.ends_with('\\') ? "" : "/";
  out.push_back(std::format("-I{}{}", directory, separator));
  out.push_back("-include");
  out.push_back(std::string(header));
  return out;
}

auto link_library_arguments(std::string_view platform,
                            const std::vector<std::string>& libraries) -> std::vector<std::string> {
  // POSIX 专属库名在 Windows 目标上不存在（mingw 没有 libdl/libm）
  static const std::vector<std::string_view> kPosixOnly = {"pthread", "dl", "m", "rt", "c",
                                                           "stdc++", "gcc", "gcc_s", "winpthread"};
  // Windows 专属导入库：非 Windows 目标上不存在。
  // **不能只靠 `starts_with("win")`**：Win32 的主力库名不以 `win` 开头
  // （`ws2_32`/`user32`/`gdi32`/`shell32`），按前缀筛会漏掉全部它们——
  // 而漏掉的后果是给 Linux 目标发 `-lws2_32`，链接期报一堆未定义符号。
  // 列成显式名单：名单短且稳定，而"猜前缀"在这件事上已经错过一次。
  static const std::vector<std::string_view> kWindowsOnly = {
      "ws2_32", "user32", "gdi32", "shell32", "kernel32", "advapi32", "ole32",
      "comctl32", "comdlg32", "winmm", "d3d11", "dxgi", "dwmapi", "uxtheme"};
  std::vector<std::string> out;
  for (const auto& library : libraries) {
    const auto name = std::string_view(library);
    const bool posix_only = std::ranges::find(kPosixOnly, name) != kPosixOnly.end();
    const bool windows_only = std::ranges::find(kWindowsOnly, name) != kWindowsOnly.end();
    if (platform == "windows" && posix_only) continue;
    if (platform != "windows" && (windows_only || name.starts_with("win"))) continue;
    out.push_back(std::format("-l{}", library));
  }
  return out;
}

auto strip_sanitizers(const std::vector<std::string>& flags) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(flags.size());
  for (const auto& flag : flags) {
    // `-fsanitize=…` / `-fno-sanitize-…` 直接匹配；`-fsanitize-recover=<list>` 是
    // **带值的另一条**（前缀不同、`=` 位置不同），必须单独判——否则它会漏下去，
    // 让第三方源仍然带着 sanitizer 运行时开关（那正是本函数要避免的）。
    if (flag.starts_with("-fsanitize=") || flag.starts_with("-fno-sanitize=")) continue;
    if (flag == "-fsanitize-recover" || flag == "-fno-sanitize-recover" ||
        flag.starts_with("-fsanitize-recover=") || flag.starts_with("-fno-sanitize-recover=")) {
      continue;
    }
    out.push_back(flag);
  }
  return out;
}

}  // namespace st::pkg
