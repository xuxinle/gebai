// 文件系统路径层的健壮性测试。
//
// 背景（实测）：路径字符串的来源极杂——PATH 环境变量、外部清单、命令行、用户输入。
// Windows 上 `std::filesystem::path` 从 UTF-8 构造时，**非法字节序列会抛异常**；
// 而 `fs::is_regular_file()` 这类纯查询函数内部就会做这个转换，于是：
//   PATH 里只要有一条非 UTF-8 的目录（安装器写下的 GBK 名字很常见），
//   `st build --toolchain=<交叉工具链>` 就会在“逐条扫 PATH 找编译器”时直接 abort，
//   连一句错误信息都输出不来。
// 因此这一层的契约是：**转换不得抛，最差退化为“找不到该文件”**。

#include "st/test/test.hpp"

#include <string>

#include "st/core/fs.hpp"

namespace {

/// 非法 UTF-8 字节序列（`C3` 后面跟了非续字节 `28`）。
const std::string kInvalidUtf8 = std::string("\xC3\x28", 2);

}  // namespace

ST_TEST(fs_path_conversion_never_throws_on_invalid_utf8) {
  // 纯查询接口：非法字节只应导致“查不到”，不应抛异常/终止进程
  bool threw = false;
  try {
    (void)st::fs::is_regular_file(kInvalidUtf8);
    (void)st::fs::exists(kInvalidUtf8);
    (void)st::fs::is_directory(kInvalidUtf8);
    (void)st::fs::file_size(kInvalidUtf8);
    (void)st::fs::read_text(kInvalidUtf8);
    (void)st::fs::create_directories(kInvalidUtf8);
  } catch (...) {
    threw = true;
  }
  ST_CHECK(!threw);
}

ST_TEST(fs_path_conversion_handles_control_bytes) {
  // 控制字节与孤立续字节（各种“半截编码”）同样只能返回失败
  const std::string odd = std::string("dir/") + '\x80' + '\xFF' + "/leaf";
  bool threw = false;
  try {
    (void)st::fs::is_regular_file(odd);
    (void)st::fs::exists(odd);
  } catch (...) {
    threw = true;
  }
  ST_CHECK(!threw);
  ST_CHECK(!st::fs::is_regular_file(odd));
}

ST_TEST(fs_join_and_names_keep_utf8_bytes) {
  // 路径拼接是纯字符串运算：必须原样保留 UTF-8 字节（不得按平台编码改写）
  const std::string joined = st::fs::join("根目录", "子项/文件.txt");
  ST_CHECK(joined.find("根目录") != std::string::npos);
  ST_CHECK(joined.find("文件.txt") != std::string::npos);
  ST_CHECK_EQ(st::fs::file_name(joined), std::string("文件.txt"));
  ST_CHECK_EQ(st::fs::stem(joined), std::string("文件"));
  ST_CHECK_EQ(st::fs::extension(joined), std::string(".txt"));
}
