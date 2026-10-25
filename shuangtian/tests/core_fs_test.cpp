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
#include <filesystem>
#include <fstream>
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

// ── 只读内存映射（`MappedFile` / `read_span`）────────────────────────────────────
//
// 背景（2026-10-14 内存评估 P0）：字体链 15 个 face 的原始文件字节**整读进 vector
// 且永不释放**，实测 ~86MB 常驻——而轮廓解析本来就是惰性的，那部分内存纯属白付。
// 改用内存映射后，那部分是**文件页的共享副本**，内存压力下系统可直接回收。
//
// 下面三条钉住的是这个改造的**机制**（而不是“内存数字”——后者靠
// `tools/mem_sample.py` 在真进程上量，不适合当回归断言：共享机器上不稳定）。

ST_TEST(fs_mapped_file_exposes_the_same_bytes_as_read_bytes) {
  // 映射与整读必须是**同一份内容**——否则“用映射替掉整读”就不是等价改造。
  // 用真实的 `st.pkg`（非空、有一定大小，且本用例本来就在仓库根跑）验证。
  const std::string path = "st.pkg";
  const auto plain = st::fs::read_bytes(path);
  ST_CHECK(plain.has_value());
  if (!plain) return;

  auto mapped = st::fs::MappedFile::open(path);
  ST_CHECK(mapped.has_value());
  if (!mapped) return;

  ST_CHECK(mapped->valid());
  ST_CHECK_EQ(mapped->size(), plain->size());
  const auto view = mapped->span();
  ST_CHECK_EQ(view.size(), plain->size());
  // 逐字节比对（`std::equal`——两边都是 `const uint8_t` 视图，不做拷贝）
  ST_CHECK(std::equal(view.begin(), view.end(), plain->begin()));
}

ST_TEST(fs_mapped_file_fails_safely_on_missing_file) {
  // 安全失败：不存在的文件、目录都只返回错误（调用方据此回退到整读）——
  // 而不是抛异常/崩溃。字体加载的回退路径就靠这条。
  const auto missing = st::fs::MappedFile::open("no/such/file/font.ttf");
  ST_CHECK(!missing.has_value());
  // 目录：`open` 在 POSIX 上会成功、`mmap` 才报 `ENODEV`（“设备不支持”）——
  // 看着像硬件问题。平台层显式挡掉，这里钉住“不崩且报错”。
  const auto directory = st::fs::MappedFile::open(".");
  ST_CHECK(!directory.has_value());
}

ST_TEST(fs_read_span_reads_exactly_the_requested_window) {
  // `read_span` 的契约（`font_face_count` 靠它只看文件头）：
  // ① 取出的窗口与整读的同区段**逐字节相同**；② 越出文件末尾**截断而不报错**。
  const std::string path = "st.pkg";
  const auto plain = st::fs::read_bytes(path);
  ST_CHECK(plain.has_value());
  if (!plain) return;
  ST_CHECK(plain->size() > 64);

  const auto head = st::fs::read_span(path, 0, 64);
  ST_CHECK(head.has_value());
  if (!head) return;
  ST_CHECK_EQ(head->size(), static_cast<std::size_t>(64));
  ST_CHECK(std::equal(head->begin(), head->end(), plain->begin()));

  // 带偏移的窗口
  const auto middle = st::fs::read_span(path, 32, 16);
  ST_CHECK(middle.has_value());
  if (middle) {
    ST_CHECK_EQ(middle->size(), static_cast<std::size_t>(16));
    ST_CHECK(std::equal(middle->begin(), middle->end(), plain->begin() + 32));
  }

  // 越界：请求比文件大得多 → 取到文件末尾为止（不报错、不越界读）
  const auto oversized = st::fs::read_span(path, plain->size() - 4, 1U << 20U);
  ST_CHECK(oversized.has_value());
  if (oversized) ST_CHECK_EQ(oversized->size(), static_cast<std::size_t>(4));

  // 完全在文件之外 → 空结果（不是错误）
  const auto beyond = st::fs::read_span(path, plain->size() + 1024, 16);
  ST_CHECK(beyond.has_value());
  if (beyond) ST_CHECK(beyond->empty());

  // 不存在的文件 → 错误（与上面的“越界”是两件事：一个能读、一个读不到）
  ST_CHECK(!st::fs::read_span("no/such/file/font.ttf", 0, 16).has_value());
}

ST_TEST(fs_read_text_strips_utf8_bom) {
  // **UTF-8 BOM 不属于文本内容**（2026-10-18 实测驱动）。
  //
  // 背景：Windows 上很多工具（记事本/旧版 VS）保存的文件带 `EF BB BF`。编辑器若把它
  // 当正文，首行就多一个不可见的 U+FEFF：光标在行首按“向右”要按两次、首字符怎么都
  // 删不掉，把文件存回去还可能变成双 BOM。
  //
  // 修在 `read_text`（文本读取的公共入口）：所有调用方（编辑器/配置/样例）口径一致。
  const std::string path = "st_bom_probe.txt";
  const std::string body = "hello\n世界\n";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ST_REQUIRE(out.good());
    out.write("\xEF\xBB\xBF", 3);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
  }
  const auto text = st::fs::read_text(path);
  ST_CHECK(text.has_value());
  if (text) {
    ST_CHECK_EQ(*text, body);                       // BOM 被剥掉，正文逐字节一致
    ST_CHECK(text->find("\xEF\xBB\xBF") == std::string::npos);   // 字节级无残留
  }
  // 无 BOM 的文件不受影响（不能“误删”正文首字节）。
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
  }
  const auto plain = st::fs::read_text(path);
  ST_CHECK(plain.has_value());
  if (plain) ST_CHECK_EQ(*plain, body);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

ST_TEST(fs_read_bytes_and_span_roundtrip_through_ftell_fseek) {
  // 覆盖 read_bytes / read_span 的实现路径：它们经 `std::ftell` / `std::fseek` 取尺寸与定位，
  // 而这两个 C 标准函数的参数/返回值类型是平台相关的 `long`（LP64 64 位 / LLP64 32 位）。
  // 本用例钉住三件事：
  //   ① 尺寸正确传递（ftell 的 long → int64_t → size_t 链路不丢）；
  //   ② 偏移定位正确（fseek 的 offset → long 转换不截断非零偏移）；
  //   ③ 内容逐字节一致。
  const std::string path = st::fs::join(st::fs::temp_dir(), "st_fs_roundtrip_probe.bin");
  std::string payload;
  payload.reserve(5000);
  for (int index = 0; index < 5000; ++index) {
    payload.push_back(static_cast<char>('a' + (index % 26)));
  }
  const auto written = st::fs::write_text(path, payload);
  ST_REQUIRE(written.has_value());

  const auto whole = st::fs::read_bytes(path);
  ST_REQUIRE(whole.has_value());
  ST_CHECK(whole->size() == payload.size());
  ST_CHECK(std::string(whole->begin(), whole->end()) == payload);

  // 非零偏移是关键：offset=0 时 long 截断不可见
  const auto slice = st::fs::read_span(path, 100, 50);
  ST_REQUIRE(slice.has_value());
  ST_CHECK(slice->size() == 50);
  ST_CHECK(std::string(slice->begin(), slice->end()) == payload.substr(100, 50));

  (void)st::fs::remove_file(path);
}
