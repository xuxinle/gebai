/// `docs/AS_LIBRARY.md` 里那个非 GUI 示例的**可执行对照**。
///
/// ## 为什么需要它
///
/// `AS_LIBRARY.md` 是给"想用霜天但不要界面"的人看的第一份文档。
/// 文档里的示例代码如果编不过、或者行为与所述不符，比没有文档**更糟**——
/// 读者会先怀疑自己的能力，再怀疑整个框架。
///
/// 实测教训（2026-10-27 写这份文档时踩到）：初稿的示例用了
/// `st::Json::parse(...)` 与 `.as_int(...)`，**这两个 API 根本不存在**
/// （真实名字是 `st::json_parse` 与 `st::json_get_i64`）——
/// 那是照着"常见 JSON 库长什么样"编的，不是查过的。
/// 同一份初稿还写错了链接库（说"只链 ws2_32 通常就够"，
/// 实际缺 `dbghelp` 就链不上，因为 `core/platform_crash.cpp` 要做栈回溯）。
///
/// 所以这个文件的存在意义是：**让文档里的示例有编译与运行证据**。
/// 它验证四件事，每件都是文档里承诺过的行为：
///
/// | 场景 | 承诺 |
/// |---|---|
/// | 正常配置 | 读出 `port` 的值 |
/// | 缺键 | 退到默认值，**不崩、不抛** |
/// | 坏 JSON | 返回 `Result` 错误（不是崩溃），带"第 N 字节"诊断 |
/// | 文件不存在 | 返回 `Result` 错误 |
///
/// ## 它同时是「库是否真的自包含」的回归
///
/// `st.pkg` 里这个 target 声明了 `"libraries": ["st_core"]` 且 `exclude_sources: ["src/**"]`，
/// 即**完全不编译 `src/` 下任何单元**，只链库。
/// 若某个通用模块偷偷依赖了 `ui`/`raster`，这个 target 会**链接失败**——
/// 那是比"读代码猜依赖"可靠的判据。
///
/// ## 构建（等价于文档 §3.2 的链接行）
///
/// ```text
/// g++ -std=c++23 -Iinclude -Ithird_party
///     tools/library_usage_check.cpp build/dev/bin/st_core.a
///     -o build/dev/bin/library_usage_check
///     -lws2_32 -luser32 -lgdi32 -lshell32 -ldbghelp
/// ```
#include <string>

#include "st/core/entry.hpp"   // ST_MAIN：跨平台入口（Windows 的 argv 是 ANSI，中文参数会乱）
#include "st/core/error.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"

namespace {

/// 与文档里一字不差的示例。
auto load_config(const std::string& path) -> st::Result<int> {
  auto text = st::fs::read_text(path);
  if (!text) return st::forward_error(text.error());   // 错误链原样上抛

  auto doc = st::json_parse(*text);                    // 解析失败带"第 N 字节"诊断
  if (!doc) return st::forward_error(doc.error());

  // 读取用 json_get_*，**不要**写 (*doc)["port"]：
  // nlohmann 的 const operator[] 在键不存在时是未定义行为。
  return static_cast<int>(st::json_get_i64(*doc, "port", 8080));
}

/// 一个检查的公共形状：跑一遍，打印结论，累计失败数。
auto report(std::string_view label, bool ok, std::string_view detail, int& failures) -> void {
  if (ok) {
    st::log::out("{}  {}  ✅\n", label, detail);
  } else {
    st::log::out("{}  {}  ❌\n", label, detail);
    ++failures;
  }
}

}  // namespace

auto run(int /*argc*/, char** /*argv*/) -> int {
  const std::string path = "library_usage_check.json";
  int failures = 0;

  // ① 正常配置：读出 port
  (void)st::fs::write_text(path, R"({"port": 9090, "name": "demo"})");
  if (auto value = load_config(path); value) {
    report("正常配置  ", *value == 9090, std::format("port={}（期望 9090）", *value), failures);
  } else {
    report("正常配置  ", false, std::format("意外失败：{}", value.error().message), failures);
  }

  // ② 缺 port 键 → 默认值（不崩、不抛）
  (void)st::fs::write_text(path, R"({"name": "no-port"})");
  if (auto value = load_config(path); value) {
    report("缺键退默认", *value == 8080, std::format("port={}（期望 8080）", *value), failures);
  } else {
    report("缺键退默认", false, std::format("意外失败：{}", value.error().message), failures);
  }

  // ③ 坏 JSON → 返回错误（不是崩溃）
  (void)st::fs::write_text(path, "{not json");
  if (auto value = load_config(path); !value) {
    report("坏 JSON   ", true, "正确返回错误而非崩溃", failures);
  } else {
    report("坏 JSON   ", false, "却成功了", failures);
  }

  // ④ 文件不存在 → 返回错误
  if (auto value = load_config("definitely-not-here.json"); !value) {
    report("缺文件    ", true, "正确返回错误而非崩溃", failures);
  } else {
    report("缺文件    ", false, "却成功了", failures);
  }

  (void)st::fs::remove_file(path);

  if (failures == 0) {
    st::log::out("\n✅ AS_LIBRARY.md 的示例行为全部成立\n");
    return 0;
  }
  st::log::err("\n❌ {} 项与文档不符\n", failures);
  return 1;
}

ST_MAIN(run)
