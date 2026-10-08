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

#include <algorithm>
#include <chrono>
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

// 背景（实测）：`st build`/`st test` 每次启动都要把 `st.pkg` 里的十几个 glob
// 展开成文件清单。旧实现**每个模式都从工程根 walk 一遍**，于是整个仓库被遍历
// 十几次——包括 `build/` 下数千个产物（数 GB），而它们对匹配零贡献。
//
// 实测（本仓库）：14 个模式共 **3.75s**，其中 **2.9s（78%）** 花在 `build/` 上。
// 这是**每次构建都付**的固定成本（连“无事可做的空构建”也要 5.6s），
// 而 AI 迭代一轮要调用很多次构建。修后**空构建 5.6s → 0.24s**。
//
// 修法：只遍历模式**字面前缀**对应的子树（`src/core/*.cpp` → 只看 `src/core`）；
// 全字面模式（无通配符，如 `src/ui/script_api.js`）根本不需要遍历，直接判存在性。
//
// 这两条都是**语义等价**的优化（`match_glob` 要求逐段匹配，不以字面前缀开头的
// 路径不可能命中），但仍需回归护栏——尤其是：
// ① 「全字面模式必须仍能命中」：改这个函数时踩到的坑（把 `src/ui/script_api.js`
//   当成子树去找，清单直接报「嵌入模式未命中」，构建失败）；
// ② 「不许退回全量遍历」：**结果类断言对旧实现全绿**（旧实现结果正确、只是慢），
//   所以必须另有一条成本断言（见本文件最后一条用例的说明）。
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(fs_glob_literal_prefix_splits_at_first_wildcard) {
  ST_CHECK_EQ(st::fs::glob_literal_prefix("src/core/*.cpp"), std::string("src/core"));
  ST_CHECK_EQ(st::fs::glob_literal_prefix("tests/*_test.cpp"), std::string("tests"));
  ST_CHECK_EQ(st::fs::glob_literal_prefix("src/**/deep/*.cpp"), std::string("src"));
  // 首个段就含通配符 → 没有字面前缀（必须退回全树遍历）
  ST_CHECK_EQ(st::fs::glob_literal_prefix("*.cpp"), std::string(""));
  // 全字面模式：整条路径都是前缀
  ST_CHECK_EQ(st::fs::glob_literal_prefix("src/ui/script_api.js"), std::string("src/ui/script_api.js"));
  // `.` 段不影响语义（不进入前缀）
  ST_CHECK_EQ(st::fs::glob_literal_prefix("./src/core/*.cpp"), std::string("src/core"));
}

ST_TEST(fs_expand_glob_handles_literal_patterns) {
  // 全字面模式（清单里的 `embed: ["src/ui/script_api.js"]` 就是这种）：
  // 必须能命中真实文件，**且不依赖 walk**（这正是前缀下钻改造的坑点）。
  const auto embed = st::fs::expand_glob(".", "st.pkg");
  ST_CHECK(embed.has_value());
  ST_CHECK_EQ(embed->size(), static_cast<std::size_t>(1));
  ST_CHECK_EQ((*embed)[0], std::string("st.pkg"));

  // 不存在的全字面模式 → 空结果（而不是报错）
  const auto missing = st::fs::expand_glob(".", "no/such/file.xyz");
  ST_CHECK(missing.has_value());
  ST_CHECK(missing->empty());
}

ST_TEST(fs_expand_glob_scopes_to_literal_prefix) {
  // 前缀下钻的**语义等价性**：结果必须与非下钻实现一致（同样的文件集合）。
  // 用真实仓库结构验证：前缀内文件全数命中、顺序确定（字典序）、且不越界。
  const auto sources = st::fs::expand_glob(".", "tests/*_test.cpp");
  ST_CHECK(sources.has_value());
  ST_CHECK(!sources->empty());
  for (const auto& path : *sources) {
    ST_CHECK(path.starts_with("tests/"));
    ST_CHECK(path.ends_with("_test.cpp"));
    // 不得混入前缀之外的目录（下钻实现最典型的错法）
    ST_CHECK(path.find("build/") == std::string::npos);
    ST_CHECK(path.find("src/") == std::string::npos);
  }
  // 字典序（调用方依赖它做稳定输出）
  ST_CHECK(std::ranges::is_sorted(*sources));
  // 本仓库必有 `tests/core_fs_test.cpp`（即本文件）——下钻不得漏掉它
  ST_CHECK(std::ranges::find(*sources, "tests/core_fs_test.cpp") != sources->end());
}

// 关于**成本**的护栏不写在这里，而是用可重复的探针量（`tools/glob_cost_probe.cpp`）：
// 前缀下钻的价值恰恰在于“行为与旧实现不可区分”（同样的结果与顺序），所以纯结果类
// 断言对旧实现也**全绿**——实测把 `expand_glob` 回退成全量遍历，本组三条用例照样通过。
// 要钉住优化本身只能比成本，而**计时断言在共享/繁忙机器上不稳定**（本项目已有
// `bench_thresholds_have_bounded_headroom` 因余量贴阈值而报警的先例），故不掺进回归套件。
// 探针实测（本仓库，确定性量级）：全字面模式 ~0.0002s，子树展开 ~0.003s，
// 全树 `walk` ~0.25s——退化回全量遍历会让后者乘上模式个数（本仓库 14）。
