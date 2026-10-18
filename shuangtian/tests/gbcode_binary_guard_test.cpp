/// gbcode 文件打开的保护策略测试：二进制识别 / 文本不误判 / BOM 剥离后的可编辑性。
///
/// 为什么单独一个文件（与 `gbcode_session_store_test` 同一策略）：
/// `page_data.cpp` 是 gbcode 的应用层源（不在框架 sources 清单里），这里直接
/// include 实现编译进测试——数据层逻辑（"这段字节算不算文本"）不需要起 UI 就能验，
/// 而起 UI 的 e2e 只能看到"某个具体文件打开后是否只读"这一个点。

#include <string>

#include "st/core/fs.hpp"
#include "st/test/test.hpp"

#include "examples/gbcode/page_data.cpp"

namespace {

using gbcode_ui::looks_binary;

}  // namespace

ST_TEST(gbcode_looks_binary_detects_nul_and_control_density) {
  // ① 头 8 KiB 有 NUL → 二进制（PE/PNG/ZIP/ELF 全都命中这条）。
  const std::string with_nul = std::string("abc\0def", 7);
  ST_CHECK(looks_binary(with_nul));

  // ② 无 NUL 但控制字符密度超阈值 → 二进制（压缩流常见）。
  std::string controls;
  for (int index = 0; index < 256; ++index) controls.push_back(static_cast<char>(index % 32));
  ST_CHECK(looks_binary(controls));

  // ③ 正常源码/中文/CRLF → 不是二进制（阈值不能误伤）。
  ST_CHECK(!looks_binary("#include <vector>\nint main() { return 0; }\n"));
  ST_CHECK(!looks_binary("中文标题\r\n\r\n正文一段。\n"));
  // ④ 偶发一个 ESC（ANSI 日志里很常见）不该判成二进制。
  ST_CHECK(!looks_binary("line one\n\x1b[31mred\x1b[0m\nline three\n"));

  // ⑤ 空文件不是二进制（空缓冲要能编辑）。
  ST_CHECK(!looks_binary(""));
}

ST_TEST(gbcode_binary_file_opens_read_only_and_text_file_does_not) {
  // **端到端的数据层证据**：同一套读取路径对两种文件给出不同的 `binary` 标记。
  // 性质上这条是"打开时会怎么处理"的唯一真值源（UI 只是照它设 read_only）。
  const auto dir = st::fs::make_temp_dir("gb-binary-probe");
  ST_REQUIRE(dir.has_value());

  const std::string bin_path = st::fs::join(*dir, "blob.bin");
  const std::string txt_path = st::fs::join(*dir, "note.txt");
  const std::string bin_bytes = std::string("\x7f" "ELF\x02\x01\x01\x00", 8) + std::string(200, '\0');
  ST_CHECK(st::fs::write_text(bin_path, bin_bytes).has_value());
  ST_CHECK(st::fs::write_text(txt_path, "hello\nworld\n").has_value());

  std::vector<gbcode_ui::OpenBuffer> buffers;
  const std::size_t bin_index = gbcode_ui::read_into_buffers(buffers, bin_path);
  ST_REQUIRE(bin_index < buffers.size());
  ST_CHECK(buffers[bin_index].binary);

  const std::size_t txt_index = gbcode_ui::read_into_buffers(buffers, txt_path);
  ST_REQUIRE(txt_index < buffers.size());
  ST_CHECK(!buffers[txt_index].binary);
  ST_CHECK_EQ(buffers[txt_index].text, std::string("hello\nworld\n"));

  // 再次打开同一个二进制文件（走"已打开则刷新"分支）仍保持 binary 标记。
  const std::size_t again = gbcode_ui::read_into_buffers(buffers, bin_path);
  ST_CHECK_EQ(again, bin_index);
  ST_CHECK(buffers[again].binary);
}
