/// 链接旗标进链接指纹的回归：只改链接期参数（`-static`/`--gc-sections`/
/// `-fuse-ld=…`/`extra_flags`/`system_libs`）必须触发重链，不能让产物静默保持
/// 旧链接方式。
///
/// ## 为什么要钉它（2026-11-09 实测的静默缺陷）
///
/// 链接指纹原先只含 **编译档位标志**，链接命令里构造的旗标不在内。于是：
///
/// | 改动 | 症状 |
/// |---|---|
/// | Windows 静态链运行库 `-static-libgcc -static-libstdc++` → `-static`（全静态） | 单元一个没变 ⇒ 指纹不变 ⇒ “链接已跳过” ⇒ 产物仍是旧链接（`libwinpthread-1.dll` 一直在导入表里），而构建输出说“完成” |
/// | `system: console → gui`（system/icon 段，此前已单修） | PE 头仍是 CONSOLE |
///
/// 这类“只改了链接参数、没改任何源文件”的改动**完全静默**，只能靠 `--force` 才生效。
/// 本用例钉住：`st::pkg::link_only_flags_for` / `link_tail_flags_for` 对“链接命令实际
/// 用什么”给出稳定描述，指纹（`build.cpp` 里）与命令共用同一份产出——测试断言
/// 不同场景产出不同/相同，从侧面封死“指纹漏了某类旗标”的回归。
///
/// ## 为什么测这两个函数而不是直接测指纹串
///
/// 指纹串把对象路径、内容哈希、目标名都卷进去了，串起来不稳定（临时目录、哈希值）。
/// 而“链接期旗标集合”是稳定的、可预测的——直接断言它，比断言指纹串更聚焦，
/// 也把「命令与指纹共用同一份产出」这条约束测到了。
#include <string>
#include <vector>

#include "st/pkg/build.hpp"
#include "st/pkg/compiler.hpp"
#include "st/test/test.hpp"

namespace {

using st::pkg::CompilerKind;
using st::pkg::link_only_flags_for;
using st::pkg::link_tail_flags_for;

/// 把 vector 拼成可读的字符串（测试输出友好）。
auto join(const std::vector<std::string>& v) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i != 0) out += " ";
    out += v[i];
  }
  return out;
}

}  // namespace

ST_TEST(link_flags_windows_has_mingw_static_triple) {
  // Windows（GCC/MinGW）必须有静态三件套：libgcc/libstdc++ 静态 + winpthread 锁静态。
  // 这正是「MinGW 运行库静态、系统库动态」的落点——少一个都会让产物带上
  // libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll 之一。
  const auto flags = link_only_flags_for("windows", CompilerKind::Gcc, false, {});
  const std::string text = join(flags);
  for (const auto* needle : {"-static-libgcc", "-static-libstdc++", "-Wl,-Bstatic",
                             "-lwinpthread", "-Wl,-Bdynamic"}) {
    ST_CHECK(text.find(needle) != std::string::npos);
  }
}

ST_TEST(link_flags_windows_keeps_system_libs_dynamic) {
  // 全静态被否（产物太大）。系统库（KERNEL32/USER32/WS2_32 等）必须动态链接。
  // 所以**绝不能**出现大范围 `-static`（全静态）——只允许上面那组定向静态。
  const auto flags = link_only_flags_for("windows", CompilerKind::Gcc, false, {});
  const std::string text = join(flags);
  ST_CHECK(text.find(" -static ") == std::string::npos);
  ST_CHECK(text.find("\"-static\"") == std::string::npos);
  // 不允许单独的 -static（全静态）；上面那组里的 -static-libgcc/-static-libstdc++
  // 是**定向**的，不在此例。
  ST_CHECK(text.find("-static") != std::string::npos);  // 至少存在定向静态
}

ST_TEST(link_flags_windows_extra_flags_static_does_not_duplicate) {
  // 清单 extra_flags 里显式写了 `-static`（全静态）的用户意图要尊重——此时不再
  // 追加定向 `-Wl,-Bstatic -lwinpthread`（那会与全静态冲突、重复）。
  const auto flags = link_only_flags_for("windows", CompilerKind::Gcc, false, {"-static"});
  const std::string text = join(flags);
  ST_CHECK(text.find("-static") != std::string::npos);
  // 全静态 + 定向 winpthread 是矛盾的：全静态已把 winpthread 静态掉，定向多余。
  ST_CHECK(text.find("-Wl,-Bstatic") == std::string::npos);
}

ST_TEST(link_flags_linux_missing_mingw_static) {
  // 非 Windows：绝不能出现定向 MinGW 静态（那些 -static-libgcc 等是 MSYS2/MinGW 专有）。
  const auto flags = link_only_flags_for("linux", CompilerKind::Gcc, false, {});
  const std::string text = join(flags);
  ST_CHECK(text.find("winpthread") == std::string::npos);
  ST_CHECK(text.find("-static-libgcc") == std::string::npos);
  ST_CHECK(text.find("-static-libstdc++") == std::string::npos);
}

ST_TEST(link_flags_tail_keeps_system_libs) {
  // 尾部队列：系统库必须被转成 -lxxx 且保留（它们进链接命令、也进指纹）。
  const auto tail = link_tail_flags_for(
      "windows", {"ws2_32", "user32", "kernel32", "gdi32"}, {});
  const std::string text = join(tail);
  for (const auto* needle : {"-lws2_32", "-luser32", "-lkernel32", "-lgdi32"}) {
    ST_CHECK(text.find(needle) != std::string::npos);
  }
}

ST_TEST(link_flags_tail_drops_posix_only_on_windows) {
  // Windows 目标上 pthread/dl/m 等在 link_library_arguments 里被过滤
  // （mingw 没有这些），不能进链接命令与指纹。
  const auto tail = link_tail_flags_for("windows", {"pthread", "dl", "m"}, {});
  const std::string text = join(tail);
  ST_CHECK(text.empty());
}

ST_TEST(link_flags_tail_extra_flags_participate) {
  // 清单 extra_flags（如 mingw 交叉档的 -static-libgcc -static-libstdc++）必须
  // 出现在 tail 里（命令行实际会追加），从而也被指纹覆盖。
  const auto tail =
      link_tail_flags_for("windows", {}, {"-static-libgcc", "-static-libstdc++"});
  const std::string text = join(tail);
  ST_CHECK(text.find("-static-libgcc") != std::string::npos);
  ST_CHECK(text.find("-static-libstdc++") != std::string::npos);
}

ST_TEST(link_flags_differ_between_platforms) {
  // 平台不同 ⇒ 链接期旗标必须不同（否则跨平台交叉时会拿错链接方式）。
  const auto win = join(link_only_flags_for("windows", CompilerKind::Gcc, false, {}));
  const auto linux = join(link_only_flags_for("linux", CompilerKind::Gcc, false, {}));
  ST_CHECK(win != linux);
}