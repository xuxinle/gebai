#include "st/pkg/compiler.hpp"

#include <algorithm>
#include <format>
#include <set>

#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/ext/json.hpp"

namespace st::pkg {

auto compiler_kind_of(std::string_view compiler) -> CompilerKind {
  const std::string name = st::ascii_lower(fs::file_name(compiler));
  if (name == "cl" || name == "cl.exe") return CompilerKind::Msvc;
  if (name.find("clang") != std::string::npos) {
    // `clang-cl` 是"MSVC 风味的 clang"：标志、环境与链接方式都按 MSVC 走
    return name.find("clang-cl") != std::string::npos ? CompilerKind::Msvc : CompilerKind::Clang;
  }
  // `g++` / `c++` / `x86_64-w64-mingw32-g++` 都是 GCC 系（含交叉编译器）
  return CompilerKind::Gcc;
}

auto compiler_kind_name(CompilerKind kind) -> std::string_view {
  switch (kind) {
    case CompilerKind::Gcc:
      return "gcc";
    case CompilerKind::Clang:
      return "clang";
    case CompilerKind::Msvc:
      return "msvc";
  }
  return "unknown";
}

namespace {

/// 版本号按数字段比较（`14.51` > `14.9`，字典序会给出相反答案）。
[[nodiscard]] auto compare_versions(std::string_view lhs, std::string_view rhs) -> int {
  const auto left = st::split(lhs, '.');
  const auto right = st::split(rhs, '.');
  const std::size_t count = std::max(left.size(), right.size());
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint64_t a = index < left.size() ? (st::parse_u64(left[index]).value_or(0)) : 0;
    const std::uint64_t b = index < right.size() ? (st::parse_u64(right[index]).value_or(0)) : 0;
    if (a != b) return a < b ? -1 : 1;
  }
  return 0;
}

/// 从 `cl.exe` 位置回溯 `vcvars64.bat`（向上找 `VC/Auxiliary/Build/`）。
[[nodiscard]] auto find_vcvars_near(std::string_view cl_path) -> std::string {
  std::string directory = fs::parent(cl_path);
  for (int level = 0; level < 8; ++level) {
    if (directory.empty()) break;
    const std::string candidate = fs::join(directory, "Auxiliary/Build/vcvars64.bat");
    if (fs::is_regular_file(candidate)) return candidate;
    const std::string up = fs::parent(directory);
    if (up == directory) break;
    directory = up;
  }
  return {};
}

/// 取工具集版本目录名（`.../MSVC/14.51.36231/bin/Hostx64/x64/cl.exe` → `14.51.36231`）。
[[nodiscard]] auto version_of(std::string_view cl_path) -> std::string {
  std::string directory = fs::parent(cl_path);
  for (int level = 0; level < 8; ++level) {
    if (directory.empty()) break;
    const std::string up = fs::parent(directory);
    if (up == directory || up.empty()) break;
    if (fs::file_name(up) == "MSVC") return fs::file_name(directory);
    directory = up;
  }
  return {};
}

/// 由 `vcvars64.bat` 反查同体系的 `cl.exe`（取版本最高的一套）。
[[nodiscard]] auto cl_from_vcvars(std::string_view vcvars) -> std::string {
  // vcvars 位于 `<VC>/Auxiliary/Build/`：向上三级到 `<VC>`
  std::string directory = fs::parent(vcvars);         // Build
  directory = fs::parent(directory);                  // Auxiliary
  const std::string vc_root = fs::parent(directory);  // VC
  auto matches = fs::expand_glob(vc_root, "Tools/MSVC/*/bin/Hostx64/x64/cl.exe");
  if (!matches || matches->empty()) return {};
  std::string best;
  std::string best_version;
  for (const auto& relative : *matches) {
    const std::string full = fs::is_absolute(relative) ? relative : fs::join(vc_root, relative);
    const std::string version = version_of(full);
    if (best.empty() || compare_versions(version, best_version) > 0) {
      best = full;
      best_version = version;
    }
  }
  return best;
}

/// `vswhere`（VS 官方查询器）报告的安装路径：能正确识别非默认盘/目录的安装。
[[nodiscard]] auto vswhere_install_path() -> std::string {
  std::vector<std::string> candidates;
  for (const auto name : {"ProgramFiles(x86)", "ProgramFiles"}) {
    const auto root = fs::read_env(name);
    if (!root.has_value() || root->empty()) continue;
    candidates.push_back(fs::join(*root, "Microsoft Visual Studio/Installer/vswhere.exe"));
  }
  for (const auto& candidate : candidates) {
    if (!fs::is_regular_file(candidate)) continue;
    auto result = process::run(candidate, {"-latest", "-products", "*", "-property", "installationPath"});
    if (!result || result->exit_code != 0) continue;
    const std::string path(st::trim(result->stdout_text));
    if (!path.empty() && fs::is_directory(path)) return path;
  }
  return {};
}

/// 扫描常见安装根目录（`vswhere` 不可用时的兜底）。
[[nodiscard]] auto scan_vcvars() -> std::vector<std::string> {
  std::vector<std::string> found;
  for (const auto name : {"ProgramFiles", "ProgramFiles(x86)"}) {
    const auto root = fs::read_env(name);
    if (!root.has_value() || root->empty()) continue;
    auto matches = fs::expand_glob(*root, "Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat");
    if (!matches) continue;
    for (const auto& relative : *matches) {
      found.push_back(fs::is_absolute(relative) ? relative : fs::join(*root, relative));
    }
  }
  return found;
}

/// 由 `vcvars` 路径组装完整工具集信息。
[[nodiscard]] auto toolchain_from_vcvars(std::string vcvars) -> Result<MsvcToolchain> {
  MsvcToolchain toolchain;
  toolchain.vcvars = std::move(vcvars);
  toolchain.cl = cl_from_vcvars(toolchain.vcvars);
  if (toolchain.cl.empty()) {
    return unexpected(ErrorCode::NotFound,
                      std::format("在 {} 附近未找到 cl.exe（VC/Tools/MSVC/*/bin/Hostx64/x64/）",
                                  toolchain.vcvars));
  }
  toolchain.version = version_of(toolchain.cl);
  return toolchain;
}

}  // namespace

auto find_msvc_toolchain() -> Result<MsvcToolchain> {
  // ① 显式指定（离线/非常规安装；也让"换一套工具集"无需改代码）
  if (const auto cl = fs::read_env("ST_CL"); cl.has_value() && fs::is_regular_file(*cl)) {
    MsvcToolchain toolchain;
    toolchain.cl = *cl;
    toolchain.version = version_of(toolchain.cl);
    toolchain.vcvars = find_vcvars_near(toolchain.cl);
    if (toolchain.vcvars.empty()) {
      return unexpected(ErrorCode::NotFound,
                        std::format("ST_CL={} 附近找不到 vcvars64.bat，无法准备 INCLUDE/LIB", *cl));
    }
    return toolchain;
  }
  if (const auto vcvars = fs::read_env("ST_VCVARS"); vcvars.has_value() && fs::is_regular_file(*vcvars)) {
    return toolchain_from_vcvars(*vcvars);
  }
  // ② VS 官方查询器
  const std::string install = vswhere_install_path();
  if (!install.empty()) {
    const std::string vcvars = fs::join(install, "VC/Auxiliary/Build/vcvars64.bat");
    if (fs::is_regular_file(vcvars)) {
      auto toolchain = toolchain_from_vcvars(vcvars);
      if (toolchain) return toolchain;
    }
  }
  // ③ 常见安装目录扫描
  auto scanned = scan_vcvars();
  std::ranges::sort(scanned);
  std::string reason;
  for (const auto& vcvars : scanned) {
    auto toolchain = toolchain_from_vcvars(vcvars);
    if (toolchain) return toolchain;
    reason = toolchain.error().message;
  }
  return unexpected(ErrorCode::NotFound,
                    std::format("未找到可用的 MSVC 工具集（安装 Visual Studio 的 C++ 工具集，"
                                "或用 ST_VCVARS/ST_CL 显式指定）{}",
                                reason.empty() ? std::string{} : std::format("：{}", reason)));
}

auto compiler_environment(CompilerKind kind) -> Result<std::map<std::string, std::string>> {
  std::map<std::string, std::string> environment;
  if (kind != CompilerKind::Msvc) return environment;  // GCC 系：环境里什么也不用额外给
  auto toolchain = find_msvc_toolchain();
  if (!toolchain) return forward_error(toolchain.error());

  // —— 环境探测结果磁盘缓存 ——
  // `vcvars64.bat + set` 实测一次 2.8~4.4s（cmd 启动 + vsdevcmd 全套环境脚本），
  // 而其中真正被用到的只有 PATH/INCLUDE/LIB 等十一个键。每次 `st build` 都重跑一遍
  // 是「无改动构建 3.3s、其中真实编译只 0.4s」的直接原因（链接跳过早已工作，
  // 时间全交了 vcvars 税）。环境只随工具集安装变化：按 vcvars 路径+mtime+大小+版本
  // 做键缓存到 `~/.shuangtian/cache/msvc-env/`，命中即免跑。
  const auto cache_key = [&]() {
    std::string material(toolchain->vcvars);
    if (const auto modified = fs::modified_ns(toolchain->vcvars); modified.has_value()) {
      material.append("\n").append(std::to_string(*modified));
    }
    if (const auto size = fs::file_size(toolchain->vcvars); size.has_value()) {
      material.append("\n").append(std::to_string(*size));
    }
    material.append("\n").append(toolchain->cl).append("\n").append(toolchain->version);
    return std::string(st::hash::fnv1a64_hex(material));
  }();
  const auto cache_root = [&]() -> std::string {
    std::string home;
    if (const auto env = fs::read_env("ST_HOME"); env.has_value() && !env->empty()) {
      home = *env;
    } else {
      home = fs::join(fs::home_dir(), ".shuangtian");
    }
    return home.empty() ? std::string{} : fs::join(fs::join(home, "cache"), "msvc-env");
  }();
  if (!cache_root.empty()) {
    const std::string cache_file = fs::join(cache_root, cache_key + ".json");
    if (auto text = fs::read_text(cache_file); text.has_value()) {
      if (auto parsed = json_parse(*text); parsed.has_value()) {
        std::map<std::string, std::string> cached;
        bool valid = parsed->is_object();
        if (valid) {
          for (const auto& [key, value] : parsed->items()) {
            if (!value.is_string()) {
              valid = false;
              break;
            }
            cached[key] = value.get<std::string>();
          }
        }
        // 缓存命中也要求关键键齐全：半截缓存比没有更糟（会以「环境为空」的症状骗人）
        if (valid && cached.contains("INCLUDE") && cached.contains("LIB")) {
          log::info("MSVC 环境命中缓存（{}）", cache_key);
          return cached;
        }
      }
    }
  }

  // 写一个临时 `.bat` 而不是把命令塞进 `cmd /c "call ... && set"`：
  // 后者要穿过 cmd 的两轮解析，路径含空格/括号时极易拼错，而错误表现只是"环境为空"。
  const std::string script =
      fs::join(fs::temp_dir(), std::format("st-vcvars-{}.bat", process::current_id()));
  const std::string body = std::format(
      "@echo off\r\n"      // CRLF：LF-only 的 .bat 在部分 cmd 版本上解析异常
      "chcp 65001 >nul\r\n"  // 让 `set` 的输出是 UTF-8：否则中文路径按本地代码页输出、注入即乱码
      "call \"{}\" >nul\r\n"
      "set\r\n",
      toolchain->vcvars);
  if (auto status = fs::write_text(script, body); !status) return forward_error(status.error());
  auto result = process::run("cmd.exe", {"/d", "/c", script});
  (void)fs::remove_file(script);
  if (!result) return forward_error(result.error());
  if (result->exit_code != 0) {
    return unexpected(ErrorCode::Internal,
                      std::format("vcvars 执行失败（退出码 {}）: {}", result->exit_code,
                                  st::trim(result->stderr_text)));
  }

  // 只带"影响编译/链接"的键：环境里上百项，整包注入纯属浪费（每次编译都要构造一次环境块）
  const std::set<std::string> wanted = {"PATH",         "INCLUDE",        "LIB",
                                       "LIBPATH",      "PATHEXT",        "WindowsSdkDir",
                                       "UCRTVersion",  "VCToolsInstallDir", "VCINSTALLDIR",
                                       "VSINSTALLDIR", "WindowsSdkVersion"};
  const std::string& text = result->stdout_text;
  for (const auto line : st::split(text, '\n')) {
    const std::string_view trimmed = st::trim(line);
    // `=C:=C:\...` 这类驱动器当前目录变量以 `=` 开头，不是键值对
    if (trimmed.empty() || trimmed.front() == '=') continue;
    const std::size_t separator = trimmed.find('=');
    if (separator == std::string_view::npos || separator == 0) continue;
    const std::string key(trimmed.substr(0, separator));
    if (!wanted.contains(key)) continue;
    environment[key] = std::string(trimmed.substr(separator + 1));
  }
  if (!environment.contains("INCLUDE") || !environment.contains("LIB")) {
    return unexpected(ErrorCode::Internal,
                      std::format("vcvars 未提供 INCLUDE/LIB（{}）：MSVC 编译会找不到任何头文件与库",
                                  toolchain->vcvars));
  }
  // 探测成功 → 写缓存（写失败只是下次多跑一次 vcvars，不影响正确性）。
  // 原子写：半截缓存会在下次被关键键校验拦下，但完整写更省一次试探。
  if (!cache_root.empty()) {
    Json dump = Json::object();
    for (const auto& [key, value] : environment) dump[key] = value;
    const std::string cache_file = fs::join(cache_root, cache_key + ".json");
    const std::string temp_file = cache_file + ".tmp";
    if (fs::write_text(temp_file, dump.dump(2)).has_value()) {
      (void)fs::rename(temp_file, cache_file);
    }
  }
  log::info("MSVC 工具集 {}（{}）", toolchain->version, toolchain->cl);
  return environment;
}

auto dialect_flags(CompilerKind kind, SourceLanguage language) -> std::vector<std::string> {
  if (kind != CompilerKind::Msvc) return {};
  std::vector<std::string> flags;
  // `/utf-8`：源码与执行字符集都按 UTF-8。**这一条不能省**——没有它 MSVC 按本地代码页
  // （中文机器是 GBK）读源码：本仓库的中文注释与界面文案会被静默改写，能编过、界面全是乱码。
  flags.push_back("/utf-8");
  // `/bigobj`：目标文件节数上限。模板/内联重的翻译单元会超默认上限（报 C1128）。
  flags.push_back("/bigobj");
  // MSVC 把 `fopen`/`getenv`/`strcpy` 判为"不安全"只是它的偏好；本框架照样用它们
  // （跨平台一致性优先：同一份源码在三个平台上走同一套 API）。
  flags.push_back("/D_CRT_SECURE_NO_WARNINGS");
  flags.push_back("/D_CRT_NONSTDC_NO_DEPRECATE");
  if (language == SourceLanguage::Cxx) {
    flags.push_back("/EHsc");            // 标准 C++ 异常模型（STL 的 bad_alloc 需要它）
    flags.push_back("/permissive-");     // 一致的两阶段名字查找等
    flags.push_back("/Zc:__cplusplus");  // `__cplusplus` 如实上报 202002L（默认报 199711L）
    flags.push_back("/Zc:preprocessor"); // 一致性预处理器：变参宏展开与 GCC/Clang 对齐
  }
  return flags;
}

auto translate_flags(CompilerKind kind, const std::vector<std::string>& flags,
                     std::vector<std::string>* dropped) -> std::vector<std::string> {
  if (kind != CompilerKind::Msvc) return flags;
  std::vector<std::string> out;
  std::set<std::string> emitted;  // `/W4`、`/WX` 这类开关去重（清单里会有多条来源）
  const auto push_once = [&out, &emitted](const std::string& flag) {
    if (emitted.insert(flag).second) out.push_back(flag);
  };
  const auto record = [dropped](std::string_view flag) {
    if (dropped != nullptr) dropped->emplace_back(flag);
  };

  for (std::size_t index = 0; index < flags.size(); ++index) {
    const std::string& flag = flags[index];
    // `-x c` / `-x c++-header`：MSVC 由扩展名决定语言，语言标志在 `dialect_flags` 里给
    if (flag == "-x") {
      if (index + 1 < flags.size()) ++index;
      continue;
    }
    if (flag.starts_with("/")) {  // 已是 MSVC 写法：直接透传（`/arch:AVX2` 之类）
      out.push_back(flag);
      continue;
    }
    if (flag.starts_with("-D") || flag.starts_with("-I")) {
      out.push_back(std::format("/{}", flag.substr(1)));
      continue;
    }
    if (flag.starts_with("-isystem")) {
      // 外部头（`/external:I` 需配 `/external:W0` 才真的免告警；这里按普通包含目录处理）
      out.push_back(std::format("/I{}", flag.substr(std::string_view("-isystem").size())));
      continue;
    }
    if (flag.starts_with("-std=")) {
      const std::string_view standard(flag);
      if (standard.find("++") != std::string_view::npos) {
        const std::string_view version = standard.substr(standard.find("++") + 2);
        if (version == "20" || version == "latest") {
          out.push_back("/std:c++20");
        } else if (version == "17") {
          out.push_back("/std:c++17");
        } else if (version == "14") {
          out.push_back("/std:c++14");
        } else {
          out.push_back("/std:c++20");  // 更新的标准：MSVC 尚无对应开关，取能用的最新
        }
      } else if (standard.find("c17") != std::string_view::npos) {
        out.push_back("/std:c17");
      } else {
        out.push_back("/std:c11");  // `gnu11`/`c11`：MSVC 最高 c17，c11 是安全选择
      }
      continue;
    }
    if (flag == "-w") {
      out.push_back("/w");  // 同一拼法：关掉全部告警（第三方源码用）
      continue;
    }
    if (flag == "-Wall" || flag == "-Wextra") {
      push_once("/W4");  // MSVC 的"严格"档
      continue;
    }
    if (flag == "-Werror") {
      push_once("/WX");
      continue;
    }
    if (flag.starts_with("-W")) {  // `-Wconversion`/`-Wshadow`/`-Wno-*`：MSVC 无对应项
      record(flag);
      continue;
    }
    if (flag == "-g" || flag.starts_with("-g")) {
      // `/Z7`：调试信息直接写进 `.obj`，不共用 PDB。
      // 为什么不用更常见的 `/Zi`：它把调试信息写进**一个共享的** PDB 文件，
      // 而本框架默认多进程并行编译——多个 cl 同时写同一个 PDB 会报 C1041
      // （“无法打开程序数据库”），要么加 `/FS` 串行化（把并行编译的收益吐回去），
      // 要么就用 `/Z7`。链接时 `/DEBUG` 仍会生成 PDB。
      out.push_back("/Z7");
      continue;
    }
    if (flag == "-fno-omit-frame-pointer") {
      out.push_back("/Oy-");  // 保留帧指针（性能剖析/调用栈用）
      continue;
    }
    if (flag == "-O0" || flag == "-Og") {
      out.push_back("/Od");
      continue;
    }
    if (flag == "-Os") {
      out.push_back("/O1");  // MSVC 里 `/O1` 才是"优先小体积"
      continue;
    }
    if (flag == "-O1" || flag == "-O2" || flag == "-O3" || flag == "-Ofast") {
      // MSVC 只有 `/Od` / `/O1`(小) / `/O2`(快) 三档：`-O1` 取"快"这一档，
      // 让 dev 档在无头验证里也跑得动（`/O1` 会按体积优化，反而慢）。
      out.push_back("/O2");
      continue;
    }
    if (flag.starts_with("-fsanitize=") || flag.starts_with("-fno-sanitize=")) {
      // MSVC 只有 ASan（`/fsanitize=address`），没有 UBSan；混用会让"san 档"语义在两平台分叉，
      // 因此这里整体丢弃并记录——san 档的权威平台仍是 GCC/Clang。
      record(flag);
      continue;
    }
    if (flag.starts_with("-")) {  // 其余 GCC 专属（`-pipe`/`-pthread`/`-fno-strict-aliasing`…）
      record(flag);
      continue;
    }
    out.push_back(flag);  // 不以 `-`/`/` 开头的（如裸路径/对象）原样透传
  }
  return out;
}

auto link_library_arguments(CompilerKind kind, std::string_view platform,
                            const std::vector<std::string>& libraries) -> std::vector<std::string> {
  std::vector<std::string> out;
  if (kind != CompilerKind::Msvc) {
    for (const auto& library : libraries) out.push_back(std::format("-l{}", library));
    return out;
  }
  // MSVC：`lib` → `lib.lib`；并丢掉只在 POSIX 上存在的库名——
  // 清单是跨平台的（`pthread`/`dl`/`m`），照转就是"找不到 pthread.lib"这种必然失败。
  static const std::set<std::string> kPosixOnly = {"pthread", "dl", "m",   "rt",      "c",
                                                  "stdc++",  "gcc", "gcc_s", "winpthread"};
  for (const auto& library : libraries) {
    if (kPosixOnly.contains(library)) continue;
    if (platform != "windows" && library.starts_with("win")) continue;  // 目标非 Windows
    out.push_back(std::format("{}.lib", library));
  }
  return out;
}

namespace {

/// 递归收集 `/sourceDependencies` JSON 里的头文件路径。
/// 兼容两种形态：v1.2（`Includes` 是**字符串数组**）与早期（嵌套对象带 `Source`/`Includes`）。
///
/// 只向下走 `Data`/`Includes`/`Source` 三个键：顶层的 `Version` 等标量不该被当成依赖。
/// （踩过的坑：只处理对象与数组、漏了“数组元素是字符串”这一层，结果只收集到源文件本身——
/// 表现为“改了头文件不重编”，而依赖清单看上去“存在且合法”，静默降级到极点。）
void collect_includes(const st::Json& node, std::vector<std::string>& out) {
  if (node.is_string()) {
    const std::string path = st::json_as_string(node);
    if (!path.empty()) out.push_back(path);
    return;
  }
  if (node.is_array()) {
    for (const auto& item : node) collect_includes(item, out);
    return;
  }
  if (!node.is_object()) return;
  for (const auto& [key, value] : node.items()) {
    if (key == "Includes" || key == "Data" || key == "Source") collect_includes(value, out);
  }
}

}  // namespace

auto depfile_from_source_dependencies(std::string_view json_text, std::string_view object_path)
    -> Result<std::string> {
  auto parsed = st::json_parse(json_text);
  if (!parsed) return forward_error(parsed.error());
  std::vector<std::string> dependencies;
  collect_includes(*parsed, dependencies);
  std::ranges::sort(dependencies);
  dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
  std::string text(object_path);
  text.push_back(':');
  for (const auto& dependency : dependencies) {
    text.push_back(' ');
    for (const char ch : dependency) {
      // 与 `parse_depfile` 的转义约定对齐：空格必须写成 `\ `，
      // 否则 `C:\Program Files\...` 会被切成两个不存在的依赖 → 每次都判"需要重建"
      if (ch == ' ') text.push_back('\\');
      text.push_back(ch);
    }
  }
  text.push_back('\n');
  return text;
}

}  // namespace st::pkg
