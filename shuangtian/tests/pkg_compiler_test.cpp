// 编译器族差异层测试：标志翻译、链接参数、**依赖清单转换**。
//
// 为什么依赖清单转换必须有测试：它错了不会让构建失败，只会让"改了头文件不重编"——
// 而后果是结构体改了、依赖它的 `.o` 还是旧的，进程在运行期以 ABI 不匹配的方式崩溃
// （实测踩过：Canvas 加一个成员，界面起来就 access violation）。
// 这类静默降级只能靠断言钉住。

#include "st/pkg/compiler.hpp"
#include "st/test/test.hpp"

namespace {

using st::pkg::CompilerKind;
using st::pkg::SourceLanguage;

}  // namespace

ST_TEST(compiler_kind_detection) {
  ST_CHECK(st::pkg::compiler_kind_of("g++") == CompilerKind::Gcc);
  ST_CHECK(st::pkg::compiler_kind_of("x86_64-w64-mingw32-g++") == CompilerKind::Gcc);
  ST_CHECK(st::pkg::compiler_kind_of("clang++") == CompilerKind::Clang);
  ST_CHECK(st::pkg::compiler_kind_of("clang-cl") == CompilerKind::Msvc);
  ST_CHECK(st::pkg::compiler_kind_of("C:/Program Files/Microsoft Visual Studio/18/Community/"
                                     "VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe") ==
           CompilerKind::Msvc);
}

ST_TEST(compiler_flag_translation_gcc_passthrough) {
  // 非 MSVC 族：原样返回（清单本来就是 GCC 风格）
  const std::vector<std::string> flags{"-O2", "-Wall", "-Iinclude", "-DFOO=1", "-std=c++20"};
  const auto same = st::pkg::translate_flags(CompilerKind::Gcc, flags);
  ST_CHECK(same == flags);
}

ST_TEST(compiler_flag_translation_msvc) {
  std::vector<std::string> dropped;
  const auto translated = st::pkg::translate_flags(
      CompilerKind::Msvc,
      {"-std=c++20", "-O2", "-DNDEBUG", "-Iinclude", "-Wall", "-Wextra", "-Werror", "-Wshadow",
       "-fno-omit-frame-pointer", "-fno-strict-aliasing", "-w"},
      &dropped);
  const auto contains = [&translated](std::string_view flag) {
    return std::ranges::find(translated, flag) != translated.end();
  };
  ST_CHECK(contains("/std:c++20"));
  ST_CHECK(contains("/O2"));
  ST_CHECK(contains("/DNDEBUG"));
  ST_CHECK(contains("/Iinclude"));
  ST_CHECK(contains("/W4"));
  ST_CHECK(contains("/WX"));
  // 旧 Windows SDK 系统头 C5105 误报降级：/WX 仍在，但该条不升错
  ST_CHECK(contains("/w35105"));
  ST_CHECK(contains("/Oy-"));
  ST_CHECK(contains("/w"));
  // 无等价物者必须被记录（调用方会打印），不能悄悄消失
  ST_CHECK(std::ranges::find(dropped, "-Wshadow") != dropped.end());
  ST_CHECK(std::ranges::find(dropped, "-fno-strict-aliasing") != dropped.end());
  // 未启用 -Werror 时不应出现 /w35105（降级只随 /WX 生效）
  const auto no_werror =
      st::pkg::translate_flags(CompilerKind::Msvc, {"-std=c++20", "-Wall"}, &dropped);
  ST_CHECK(std::ranges::find(no_werror, "/w35105") == no_werror.end());
}

// PCH 消费端标志拼装：两族各自正确，且绝不能把 GCC 语法漏给 MSVC
ST_TEST(pch_consume_args_per_compiler_kind) {
  const auto msvc = st::pkg::pch_consume_args(CompilerKind::Msvc, "build/dev/pch",
                                              "prefix.hpp", "build/dev/pch/prefix.pch");
  const auto contains = [](const std::vector<std::string>& args, std::string_view flag) {
    return std::ranges::find(args, flag) != args.end();
  };
  ST_CHECK(contains(msvc, "/Ibuild/dev/pch/"));
  ST_CHECK(contains(msvc, "/Yuprefix.hpp"));
  ST_CHECK(contains(msvc, "/FIprefix.hpp"));
  ST_CHECK(contains(msvc, "/Fpbuild/dev/pch/prefix.pch"));
  // GCC 语法（D9002/D9024/D9027 三重错误）绝不能出现在 MSVC 命令里
  ST_CHECK(!contains(msvc, "-include"));

  const auto gcc = st::pkg::pch_consume_args(CompilerKind::Gcc, "build/dev/pch",
                                             "prefix.hpp", "");
  ST_CHECK(contains(gcc, "-Ibuild/dev/pch/"));
  ST_CHECK(contains(gcc, "-include"));
  ST_CHECK(contains(gcc, "prefix.hpp"));
  // GCC 系不吃 /Yu /FI /Fp
  ST_CHECK(!contains(gcc, "/Yuprefix.hpp"));
  ST_CHECK(!contains(gcc, "/FIprefix.hpp"));

  // 目录已带尾分隔符时不双写
  const auto trailing = st::pkg::pch_consume_args(CompilerKind::Msvc, "build/dev/pch/",
                                                  "prefix.hpp", "");
  ST_CHECK(contains(trailing, "/Ibuild/dev/pch/"));
  ST_CHECK(!contains(trailing, "/Ibuild/dev/pch//"));
}

ST_TEST(compiler_link_libraries_msvc_drops_posix) {
  const auto args = st::pkg::link_library_arguments(
      CompilerKind::Msvc, "windows", {"pthread", "dl", "m", "ws2_32", "user32"});
  const auto contains = [&args](std::string_view name) {
    return std::ranges::find(args, name) != args.end();
  };
  ST_CHECK(!contains("pthread.lib"));
  ST_CHECK(!contains("dl.lib"));
  ST_CHECK(!contains("m.lib"));
  ST_CHECK(contains("ws2_32.lib"));
  ST_CHECK(contains("user32.lib"));
  // GCC 族照旧 `-l` 形式
  const auto gcc = st::pkg::link_library_arguments(CompilerKind::Gcc, "linux", {"pthread"});
  ST_CHECK(std::ranges::find(gcc, "-lpthread") != gcc.end());
}

// —— 依赖清单：v1.2（`Includes` 为字符串数组）——
ST_TEST(depfile_conversion_source_dependencies_v12) {
  const std::string json = R"({
    "Version": "1.2",
    "Data": {
      "Source": "c:\\proj\\src\\a.cpp",
      "Includes": [
        "c:\\proj\\include\\a.hpp",
        "c:\\proj\\include\\b.hpp"
      ]
    }
  })";
  const auto depfile = st::pkg::depfile_from_source_dependencies(json, "obj/a.cpp.o");
  ST_CHECK(depfile.has_value());
  // 依赖必须**全部**进入清单（漏掉任何一个都会造成"改了不重编"）
  ST_CHECK(depfile->find("c:\\proj\\include\\a.hpp") != std::string::npos);
  ST_CHECK(depfile->find("c:\\proj\\include\\b.hpp") != std::string::npos);
  ST_CHECK(depfile->find("c:\\proj\\src\\a.cpp") != std::string::npos);
  // 顶层的 Version 不是依赖
  ST_CHECK(depfile->find("1.2") == std::string::npos);
  ST_CHECK(depfile->starts_with("obj/a.cpp.o: "));
}

// —— 依赖清单：早期嵌套形态（对象数组 + 内层 Includes）——
ST_TEST(depfile_conversion_source_dependencies_nested) {
  const std::string json = R"({
    "Version": "1.1",
    "Data": {
      "Source": "c:\\proj\\src\\a.cpp",
      "Includes": [
        { "Source": "c:\\proj\\include\\a.hpp",
          "Includes": [ { "Source": "c:\\proj\\include\\deep.hpp" } ] }
      ]
    }
  })";
  const auto depfile = st::pkg::depfile_from_source_dependencies(json, "obj/a.cpp.o");
  ST_CHECK(depfile.has_value());
  ST_CHECK(depfile->find("c:\\proj\\include\\deep.hpp") != std::string::npos);
}

// —— 带空格的路径必须转义（否则会被切成两个不存在的依赖 → 每次都判"需要重建"）——
ST_TEST(depfile_conversion_escapes_spaces) {
  const std::string json = R"({
    "Version": "1.2",
    "Data": { "Source": "c:\\my proj\\src\\a.cpp", "Includes": ["c:\\my proj\\inc\\x.hpp"] }
  })";
  const auto depfile = st::pkg::depfile_from_source_dependencies(json, "obj/a.cpp.o");
  ST_CHECK(depfile.has_value());
  ST_CHECK(depfile->find("c:\\my\\ proj\\inc\\x.hpp") != std::string::npos);
}

ST_TEST(depfile_conversion_rejects_broken_json) {
  ST_CHECK(!st::pkg::depfile_from_source_dependencies("{ not json", "obj/a.cpp.o").has_value());
}

ST_TEST(dialect_flags_msvc_pins_utf8_and_language) {
  const auto cxx = st::pkg::dialect_flags(CompilerKind::Msvc, SourceLanguage::Cxx);
  const auto has = [&cxx](std::string_view flag) {
    return std::ranges::find(cxx, flag) != cxx.end();
  };
  // `/utf-8` 不能省：没有它 MSVC 按本地代码页读源码，中文文案静默乱码
  ST_CHECK(has("/utf-8"));
  ST_CHECK(has("/EHsc"));
  // C 源不能吃 C++ 专属开关
  const auto c = st::pkg::dialect_flags(CompilerKind::Msvc, SourceLanguage::C);
  ST_CHECK(std::ranges::find(c, "/EHsc") == c.end());
  // GCC 族没有"恒定追加"的标志
  ST_CHECK(st::pkg::dialect_flags(CompilerKind::Gcc, SourceLanguage::Cxx).empty());
}
